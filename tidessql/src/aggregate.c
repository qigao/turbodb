#include "aggregate.h"
#include "work.h"
#include "error.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static const char aggregate_output_busy[] = "SQL aggregate output still active";

typedef struct aggregate_distinct_row {
  const turbodb_value_t *values;
  size_t first, count;
} aggregate_distinct_row;
static int aggregate_distinct_compare(const void *left, const void *right) {
  const aggregate_distinct_row *a=left,*b=right;
  for(size_t i=0;i<a->count;++i) {
    const int order=orm_sql_value_order(&a->values[a->first+i],&b->values[b->first+i]);
    if(order) return order;
  }
  return 0;
}
static bool aggregate_distinct_copy(void *to,const void *from) { *(aggregate_distinct_row *)to=*(const aggregate_distinct_row *)from; return true; }
static void aggregate_distinct_move(void *to,void *from) { *(aggregate_distinct_row *)to=*(aggregate_distinct_row *)from; }
static void aggregate_distinct_destroy(void *value) { (void)value; }
/* Records and CSTL scratch borrow only this group's owned immutable snapshots. */
static const cmeta_type_traits aggregate_distinct_traits={
    CMETA_TRAIT_COMPARE|CMETA_TRAIT_COPY|CMETA_TRAIT_MOVE|CMETA_TRAIT_DESTROY|
    CMETA_TRAIT_TRIVIAL_COPY|CMETA_TRAIT_TRIVIAL_DESTROY,
    NULL,NULL,aggregate_distinct_compare,aggregate_distinct_copy,aggregate_distinct_move,aggregate_distinct_destroy};
static const cmeta_type_desc aggregate_distinct_type={"sql_aggregate_distinct_row",sizeof(aggregate_distinct_row),
    _Alignof(aggregate_distinct_row),CMETA_T_OBJECT,NULL,&aggregate_distinct_traits};

static turbodb_status_t aggregate_error(turbodb_error_t *error, turbodb_status_t status, const char *message) {
  tdsql_error_set(error,status,message); return status;
}
static bool aggregate_numeric(orm_sql_type type) {
  return type.kind == TURBODB_VALUE_NULL || type.kind == TURBODB_VALUE_INT64 ||
      type.kind == TURBODB_VALUE_UINT64 || type.kind == TURBODB_VALUE_DOUBLE || type.kind == TURBODB_VALUE_BOOLEAN;
}
turbodb_status_t orm_tidesdb_sql_aggregate_type(orm_sql_aggregate_kind kind,
    const orm_sql_type *input, orm_sql_type *out, turbodb_error_t *error) {
  if (!out || (kind != ORM_SQL_COUNT_ALL && !input))
    return aggregate_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"aggregate type requires input and output");
  if (kind < ORM_SQL_COUNT_ALL || kind > ORM_SQL_BIT_XOR)
    return aggregate_error(error,TURBODB_STATUS_UNSUPPORTED,"unsupported SQL aggregate function");
  if (kind != ORM_SQL_COUNT_ALL) {
    orm_sql_predicate validator;
    const turbodb_status_t status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,*input,NULL,&validator,error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  if (kind == ORM_SQL_COUNT_ALL || kind == ORM_SQL_COUNT_VALUE) *out = (orm_sql_type){TURBODB_VALUE_INT64,false};
  else if (orm_sql_aggregate_bit_kind(kind)) {
    if (!aggregate_numeric(*input))
      return aggregate_error(error,TURBODB_STATUS_UNSUPPORTED,"SQL numeric bit aggregates require numeric, BOOL or NULL input");
    *out=(orm_sql_type){TURBODB_VALUE_UINT64,false};
  } else if (orm_sql_aggregate_moment_kind(kind)) {
    if (!aggregate_numeric(*input))
      return aggregate_error(error,TURBODB_STATUS_UNSUPPORTED,"SQL variance/stddev requires numeric, BOOL or NULL input");
    *out=(orm_sql_type){TURBODB_VALUE_DOUBLE,true};
  } else {
    if (kind == ORM_SQL_SUM || kind == ORM_SQL_AVG) {
      if (input->kind == TURBODB_VALUE_INT64 || input->kind == TURBODB_VALUE_UINT64 || input->kind == TURBODB_VALUE_BOOLEAN)
        return aggregate_error(error,TURBODB_STATUS_UNSUPPORTED,"SQL SUM/AVG exact-value input requires DECIMAL support");
      if (input->kind != TURBODB_VALUE_DOUBLE && input->kind != TURBODB_VALUE_NULL)
        return aggregate_error(error,TURBODB_STATUS_UNSUPPORTED,"SQL SUM/AVG requires DOUBLE or NULL input");
    } else if (!aggregate_numeric(*input))
      return aggregate_error(error,TURBODB_STATUS_UNSUPPORTED,"SQL MIN/MAX requires numeric, BOOL or NULL input");
    *out = (orm_sql_type){input->kind,true};
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t aggregate_validate(orm_sql_row_source *source,
    const orm_sql_aggregate_spec *spec, turbodb_error_t *error) {
  if (!source || !source->budget || !source->types || !source->columns || !source->context || !source->next ||
      !spec || (!spec->key_count && !spec->item_count) || (spec->key_count && !spec->keys) ||
      (spec->item_count && !spec->items))
    return aggregate_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL aggregate specification");
  if (source->active) return aggregate_error(error,TURBODB_STATUS_BUSY,"SQL aggregate input already active");
  const uint64_t limit = source->budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES];
  if (source->columns > limit || spec->item_count > SIZE_MAX-spec->key_count ||
      spec->key_count+spec->item_count > limit)
    return aggregate_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"SQL aggregate shape exceeds plan capacity");
  orm_sql_budget_amount steps = {0};
  steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = spec->key_count+spec->item_count;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(source->budget,&steps,error);
  if (status != TURBODB_STATUS_OK) return status;
  steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = source->columns;
  status = orm_tidesdb_sql_budget_reserve(source->budget,&steps,error);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; i < spec->key_count; ++i) {
    if (spec->keys[i] >= source->columns)
      return aggregate_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"SQL group key outside input");
    if (!aggregate_numeric(source->types[spec->keys[i]]))
      return aggregate_error(error,TURBODB_STATUS_UNSUPPORTED,"SQL grouping requires numeric, BOOL or NULL keys");
  }
  for (size_t i = 0; i < spec->item_count; ++i) {
    const orm_sql_aggregate_item *item = &spec->items[i];
    const size_t arguments=orm_sql_aggregate_arguments(item);
    if (arguments>1 && (item->kind!=ORM_SQL_COUNT_VALUE || !item->distinct))
      return aggregate_error(error,TURBODB_STATUS_UNSUPPORTED,"multiple aggregate arguments require DISTINCT COUNT");
    if (item->distinct && item->kind!=ORM_SQL_COUNT_VALUE && item->kind!=ORM_SQL_MIN &&
        item->kind!=ORM_SQL_MAX && item->kind!=ORM_SQL_SUM && item->kind!=ORM_SQL_AVG)
      return aggregate_error(error,TURBODB_STATUS_UNSUPPORTED,"aggregate DISTINCT is not supported for this function");
    if (item->kind != ORM_SQL_COUNT_ALL && (item->slot >= source->columns || arguments>source->columns-item->slot))
      return aggregate_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"SQL aggregate argument outside input");
    steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=arguments-1;
    status=orm_tidesdb_sql_budget_reserve(source->budget,&steps,error);
    if(status!=TURBODB_STATUS_OK) return status;
    for(size_t argument=0;item->distinct && argument<arguments;++argument) {
      if(!aggregate_numeric(source->types[item->slot+argument]))
        return aggregate_error(error,TURBODB_STATUS_UNSUPPORTED,"aggregate DISTINCT requires numeric, BOOL or NULL arguments");
      orm_sql_type checked;
      status=orm_tidesdb_sql_aggregate_type(ORM_SQL_COUNT_VALUE,&source->types[item->slot+argument],&checked,error);
      if(status!=TURBODB_STATUS_OK) return status;
    }
    orm_sql_type type;
    status = orm_tidesdb_sql_aggregate_type(item->kind,
        item->kind == ORM_SQL_COUNT_ALL ? NULL : &source->types[item->slot],&type,error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  return TURBODB_STATUS_OK;
}

static turbodb_status_t aggregate_distinct_clear(orm_sql_aggregate *run,turbodb_error_t *error) {
  turbodb_status_t status=orm_sql_work_release(&run->distinct_order,run->distinct_order_bytes,run->budget,error);
  run->distinct_order_bytes=0;
  const turbodb_status_t released=orm_sql_rows_close(&run->distinct_rows,status==TURBODB_STATUS_OK?error:NULL);
  if(status==TURBODB_STATUS_OK) status=released;
  run->distinct_rows.budget=run->budget;
  return status;
}

turbodb_status_t orm_tidesdb_sql_aggregate_close(orm_sql_aggregate *run, turbodb_error_t *error) {
  if (!run || !run->budget) return TURBODB_STATUS_OK;
  if (run->source.active) return aggregate_error(error,TURBODB_STATUS_BUSY,aggregate_output_busy);
  turbodb_status_t status = orm_tidesdb_sql_scan_close(&run->scan,error);
  const turbodb_status_t distinct=aggregate_distinct_clear(run,status==TURBODB_STATUS_OK?error:NULL);
  if(status==TURBODB_STATUS_OK) status=distinct;
  vec_t *vectors[] = {&run->keys,&run->items,&run->types,&run->output,&run->counts,&run->moments,&run->distinct_slots,&run->distinct_input};
  const size_t bytes[] = {run->key_bytes,run->item_bytes,run->type_bytes,run->output_bytes,run->count_bytes,run->moment_bytes,
      run->distinct_slot_bytes,run->distinct_input_bytes};
  for (size_t i = 0; i < sizeof(vectors)/sizeof(vectors[0]); ++i) {
    const turbodb_status_t released = orm_sql_work_release(vectors[i],bytes[i],run->budget,
        status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  if (run->groups) {
    const turbodb_status_t released = orm_tidesdb_sql_budget_release(run->budget,ORM_SQL_BUDGET_GROUPS,
        run->groups,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  if (run->metadata_bytes) {
    const turbodb_status_t released = orm_tidesdb_sql_budget_release(run->budget,ORM_SQL_BUDGET_WORK_BYTES,
        run->metadata_bytes,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  *run = (orm_sql_aggregate){0}; return status;
}

static turbodb_status_t aggregate_pull(void *context, const turbodb_value_t **out, turbodb_error_t *error);

turbodb_status_t orm_tidesdb_sql_aggregate_open(orm_sql_row_source *source,
    const orm_sql_aggregate_spec *spec, orm_sql_aggregate *out, turbodb_error_t *error) {
  if (!out || out->budget)
    return aggregate_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"SQL aggregate requires empty output");
  turbodb_status_t status = aggregate_validate(source,spec,error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_aggregate run = {.budget=source->budget,.state=ORM_SQL_SCAN_OPEN};
  tdsql_error_init(&run.failure);
  vec_t projection = {0}, orders = {0}; size_t projection_bytes = 0, order_bytes = 0;
  const size_t width = spec->key_count+spec->item_count;
  bool average = false, moments = false; size_t distinct_width=0;
  for (size_t i = 0; i < spec->item_count; ++i) {
    if (spec->items[i].kind == ORM_SQL_AVG) average = true;
    if (orm_sql_aggregate_moment_kind(spec->items[i].kind)) moments = true;
    if(orm_sql_aggregate_distinct(&spec->items[i])) {
      const size_t arguments=orm_sql_aggregate_arguments(&spec->items[i]);
      const uint64_t limit=source->budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES];
      if(arguments>SIZE_MAX-distinct_width || distinct_width+arguments>limit)
        return aggregate_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"aggregate DISTINCT argument capacity exceeded");
      distinct_width+=arguments;
    }
  }
  status = orm_tidesdb_sql_budget_reserve_capacity(run.budget,1,sizeof(run),0,&run.metadata_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run.keys,spec->key_count,sizeof(size_t),
      _Alignof(size_t),run.budget,&run.key_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run.items,spec->item_count,sizeof(orm_sql_aggregate_item),
      _Alignof(orm_sql_aggregate_item),run.budget,&run.item_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run.types,width,sizeof(orm_sql_type),
      _Alignof(orm_sql_type),run.budget,&run.type_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run.output,width,sizeof(turbodb_value_t),
      _Alignof(turbodb_value_t),run.budget,&run.output_bytes,error);
  if (status == TURBODB_STATUS_OK && average) status = orm_sql_work_zero(&run.counts,spec->item_count,sizeof(uint64_t),
      _Alignof(uint64_t),run.budget,&run.count_bytes,error);
  if (status == TURBODB_STATUS_OK && moments) status = orm_sql_work_zero(&run.moments,spec->item_count,sizeof(orm_sql_reduction),
      _Alignof(orm_sql_reduction),run.budget,&run.moment_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&run.distinct_slots,distinct_width,sizeof(size_t),
      _Alignof(size_t),run.budget,&run.distinct_slot_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&run.distinct_input,distinct_width,sizeof(turbodb_value_t),
      _Alignof(turbodb_value_t),run.budget,&run.distinct_input_bytes,error);
  run.distinct_rows.budget=run.budget;
  if(status==TURBODB_STATUS_OK) {
    orm_sql_budget_amount steps={0}; steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=distinct_width;
    status=orm_tidesdb_sql_budget_reserve(run.budget,&steps,error);
  }
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&projection,source->columns,sizeof(size_t),
      _Alignof(size_t),run.budget,&projection_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&orders,spec->key_count,sizeof(orm_sql_scan_order),
      _Alignof(orm_sql_scan_order),run.budget,&order_bytes,error);
  if (status == TURBODB_STATUS_OK) {
    if (spec->key_count) memcpy(vec_data(&run.keys),spec->keys,spec->key_count*sizeof(size_t));
    if (spec->item_count) memcpy(vec_data(&run.items),spec->items,spec->item_count*sizeof(orm_sql_aggregate_item));
    size_t distinct_slot=0;
    for(size_t i=0;i<spec->item_count;++i)
      if(orm_sql_aggregate_distinct(&spec->items[i]))
        for(size_t argument=0;argument<orm_sql_aggregate_arguments(&spec->items[i]);++argument)
          *(size_t *)vec_at(&run.distinct_slots,distinct_slot++)=spec->items[i].slot+argument;
    for (size_t i = 0; i < source->columns; ++i) *(size_t *)vec_at(&projection,i) = i;
    for (size_t i = 0; i < spec->key_count; ++i) {
      *(orm_sql_type *)vec_at(&run.types,i) = source->types[spec->keys[i]];
      *(orm_sql_scan_order *)vec_at(&orders,i) = (orm_sql_scan_order){.slot=spec->keys[i]};
    }
    for (size_t i = 0; status == TURBODB_STATUS_OK && i < spec->item_count; ++i) {
      const orm_sql_aggregate_item *item = &spec->items[i];
      status = orm_tidesdb_sql_aggregate_type(item->kind,
          item->kind == ORM_SQL_COUNT_ALL ? NULL : &source->types[item->slot],vec_at(&run.types,spec->key_count+i),error);
    }
    const orm_sql_scan_spec scan = {.projection=vec_data_const(&projection),.projection_count=source->columns,
        .limit=UINT64_MAX,.orders=vec_data_const(&orders),.order_count=spec->key_count};
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_scan_open_source(source,&scan,run.budget,&run.scan,error);
  }
  vec_t *temporary[] = {&projection,&orders}; const size_t bytes[] = {projection_bytes,order_bytes};
  for (size_t i = 0; i < sizeof(temporary)/sizeof(temporary[0]); ++i) {
    const turbodb_status_t released = orm_sql_work_release(temporary[i],bytes[i],run.budget,
        status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t released = orm_tidesdb_sql_aggregate_close(&run,NULL);
    return released == TURBODB_STATUS_OK ? status : released;
  }
  *out = run;
  out->source = (orm_sql_row_source){out->budget,vec_data_const(&out->types),width,out,aggregate_pull,false};
  return TURBODB_STATUS_OK;
}

static turbodb_status_t aggregate_read(orm_sql_aggregate *run, turbodb_error_t *error) {
  if (run->pending || run->eof) return TURBODB_STATUS_OK;
  orm_sql_scan_row row;
  const turbodb_status_t status = orm_tidesdb_sql_scan_next(&run->scan,&row,error);
  if (status != TURBODB_STATUS_OK) return status;
  if (row.state == ORM_SQL_SCAN_ROW) run->pending = row.values;
  else if (row.state == ORM_SQL_SCAN_DONE) run->eof = true;
  else return aggregate_error(error,TURBODB_STATUS_INVALID_STATE,"SQL aggregate input is cancelled");
  return TURBODB_STATUS_OK;
}
static turbodb_status_t aggregate_begin(orm_sql_aggregate *run, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_GROUPS] = 1;
  amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = vec_size(&run->output)+vec_size(&run->counts)+vec_size(&run->moments);
  const turbodb_status_t status = orm_tidesdb_sql_budget_reserve(run->budget,&amount,error);
  if (status != TURBODB_STATUS_OK) return status;
  ++run->groups;
  if (vec_size(&run->counts)) memset(vec_data(&run->counts),0,vec_size(&run->counts)*sizeof(uint64_t));
  if (vec_size(&run->moments)) memset(vec_data(&run->moments),0,vec_size(&run->moments)*sizeof(orm_sql_reduction));
  const size_t keys = vec_size(&run->keys);
  for (size_t i = 0; i < keys; ++i)
    *(turbodb_value_t *)vec_at(&run->output,i) = run->pending[*(const size_t *)vec_at_const(&run->keys,i)];
  for (size_t i = 0; i < vec_size(&run->items); ++i) {
    const orm_sql_aggregate_item *item = vec_at_const(&run->items,i);
    *(turbodb_value_t *)vec_at(&run->output,keys+i) = orm_sql_reduction_begin(item->kind).value;
  }
  return TURBODB_STATUS_OK;
}
orm_sql_reduction orm_sql_reduction_begin(orm_sql_aggregate_kind kind) {
  if (orm_sql_aggregate_bit_kind(kind))
    return (orm_sql_reduction){.value=turbodb_u64(kind == ORM_SQL_BIT_AND ? UINT64_MAX : 0)};
  return (orm_sql_reduction){.value=kind == ORM_SQL_COUNT_ALL || kind == ORM_SQL_COUNT_VALUE ? turbodb_i64(0) : turbodb_null()};
}
static turbodb_status_t aggregate_bit_number(const turbodb_value_t *input, uint64_t *out, turbodb_error_t *error) {
  switch (input->kind) {
    case TURBODB_VALUE_INT64: *out=(uint64_t)input->data.int64_value; break;
    case TURBODB_VALUE_UINT64: *out=input->data.uint64_value; break;
    case TURBODB_VALUE_BOOLEAN: *out=input->data.boolean_value != 0; break;
    default: {
      /* Bit evaluation rounds like real-to-integer expression evaluation, not column assignment. */
      const double rounded=rint(input->data.double_value);
      if (rounded < (double)INT64_MIN || rounded >= -(double)INT64_MIN)
        return aggregate_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"SQL bit aggregate DOUBLE exceeds signed integer capacity");
      *out=(uint64_t)(int64_t)rounded;
      break;
    }
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t aggregate_moment_add(orm_sql_reduction *state, const turbodb_value_t *input, turbodb_error_t *error) {
  if (state->count == UINT64_MAX)
    return aggregate_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"SQL variance/stddev count exceeds capacity");
  double sample;
  switch (input->kind) {
    case TURBODB_VALUE_INT64: sample=(double)input->data.int64_value; break;
    case TURBODB_VALUE_UINT64: sample=(double)input->data.uint64_value; break;
    case TURBODB_VALUE_BOOLEAN: sample=(double)input->data.boolean_value; break;
    default: sample=input->data.double_value; break;
  }
  const uint64_t count=state->count+1;
  double mean=sample, deviation=0.0;
  if (state->count) {
    const double delta=sample-state->mean;
    mean=state->mean+delta/(double)count;
    deviation=state->squared_deviation+delta*(sample-mean);
  }
  if (!isfinite(mean) || !isfinite(deviation) || deviation<0.0)
    return aggregate_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"SQL variance/stddev exceeds finite DOUBLE capacity");
  state->count=count; state->mean=mean; state->squared_deviation=deviation;
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_reduction_add(orm_sql_aggregate_kind kind, orm_sql_reduction *state,
    const turbodb_value_t *input, turbodb_error_t *error) {
  if (input && input->kind == TURBODB_VALUE_NULL) return TURBODB_STATUS_OK;
  turbodb_value_t *value=&state->value;
  if (orm_sql_aggregate_moment_kind(kind)) return aggregate_moment_add(state,input,error);
  if (orm_sql_aggregate_bit_kind(kind)) {
    uint64_t bits;
    const turbodb_status_t status=aggregate_bit_number(input,&bits,error);
    if (status != TURBODB_STATUS_OK) return status;
    const uint64_t current=value->data.uint64_value;
    *value=turbodb_u64(kind == ORM_SQL_BIT_AND ? current&bits : kind == ORM_SQL_BIT_OR ? current|bits : current^bits);
    return TURBODB_STATUS_OK;
  }
  if (kind == ORM_SQL_COUNT_ALL || kind == ORM_SQL_COUNT_VALUE) {
    if (value->data.int64_value == INT64_MAX)
      return aggregate_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"SQL COUNT exceeds BIGINT capacity");
    ++value->data.int64_value;
  } else if (kind == ORM_SQL_SUM || kind == ORM_SQL_AVG) {
    if (kind == ORM_SQL_AVG && state->count == UINT64_MAX)
      return aggregate_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"SQL AVG count exceeds capacity");
    const double total=(value->kind == TURBODB_VALUE_NULL ? 0.0 : value->data.double_value)+input->data.double_value;
    if (!isfinite(total)) return aggregate_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"SQL SUM/AVG exceeds finite DOUBLE capacity");
    *value=turbodb_f64(total);
    if (kind == ORM_SQL_AVG) ++state->count;
  } else if (value->kind == TURBODB_VALUE_NULL ||
      (kind == ORM_SQL_MIN ? orm_sql_value_order(input,value)<0 : orm_sql_value_order(input,value)>0)) *value=*input;
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_reduction_result(orm_sql_aggregate_kind kind, const orm_sql_reduction *state,
    turbodb_value_t *out, turbodb_error_t *error) {
  turbodb_value_t value=state->value;
  if (orm_sql_aggregate_moment_kind(kind)) {
    const bool sample=kind == ORM_SQL_VAR_SAMP || kind == ORM_SQL_STDDEV_SAMP;
    value=state->count <= (uint64_t)sample ? turbodb_null() :
        turbodb_f64(state->squared_deviation/(double)(state->count-(uint64_t)sample));
    if (value.kind != TURBODB_VALUE_NULL && (kind == ORM_SQL_STDDEV_POP || kind == ORM_SQL_STDDEV_SAMP))
      value.data.double_value=sqrt(value.data.double_value);
  } else if (kind == ORM_SQL_AVG && value.kind != TURBODB_VALUE_NULL) {
    if (!state->count) return aggregate_error(error,TURBODB_STATUS_INTERNAL_ERROR,"SQL AVG non-NULL sum has no inputs");
    value.data.double_value/=(double)state->count;
  }
  *out=value;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t aggregate_add(orm_sql_aggregate *run,size_t index,const turbodb_value_t *input,turbodb_error_t *error) {
  const orm_sql_aggregate_item *item=vec_at_const(&run->items,index);
  turbodb_value_t *value=vec_at(&run->output,vec_size(&run->keys)+index);
  if(orm_sql_aggregate_moment_kind(item->kind))
    return orm_sql_reduction_add(item->kind,vec_at(&run->moments,index),input,error);
  uint64_t *count=item->kind==ORM_SQL_AVG?vec_at(&run->counts,index):NULL;
  orm_sql_reduction state={.value=*value,.count=count?*count:0};
  const turbodb_status_t status=orm_sql_reduction_add(item->kind,&state,input,error);
  if(status!=TURBODB_STATUS_OK) return status;
  *value=state.value; if(count) *count=state.count;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t aggregate_distinct_finish(orm_sql_aggregate *run,turbodb_error_t *error) {
  if(!vec_size(&run->distinct_slots)) return TURBODB_STATUS_OK;
  const size_t rows=vec_size(&run->distinct_rows.snapshots);
  turbodb_status_t status=orm_sql_work_zero(&run->distinct_order,rows,sizeof(aggregate_distinct_row),
      _Alignof(aggregate_distinct_row),run->budget,&run->distinct_order_bytes,error);
  size_t first=0;
  for(size_t item_index=0;status==TURBODB_STATUS_OK && item_index<vec_size(&run->items);++item_index) {
    const orm_sql_aggregate_item *item=vec_at_const(&run->items,item_index);
    if(!orm_sql_aggregate_distinct(item)) continue;
    const size_t arguments=orm_sql_aggregate_arguments(item);
    if(arguments>(UINT64_MAX-1)/2 || rows>UINT64_MAX/(2*(uint64_t)arguments+1))
      return aggregate_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"aggregate DISTINCT comparison steps overflow");
    orm_sql_budget_amount steps={0}; steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=rows;
    status=orm_tidesdb_sql_budget_reserve(run->budget,&steps,error);
    if(status!=TURBODB_STATUS_OK) break;
    for(size_t row=0;row<rows;++row)
      *(aggregate_distinct_row *)vec_at(&run->distinct_order,row)=
          (aggregate_distinct_row){orm_sql_rows_at(&run->distinct_rows,row),first,arguments};
    status=orm_sql_work_sort(vec_data(&run->distinct_order),rows,&aggregate_distinct_type,arguments,run->budget,error);
    steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=(uint64_t)rows*(2*(uint64_t)arguments+1);
    if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_budget_reserve(run->budget,&steps,error);
    const aggregate_distinct_row *previous=NULL;
    for(size_t row=0;status==TURBODB_STATUS_OK && row<rows;++row) {
      const aggregate_distinct_row *candidate=vec_at_const(&run->distinct_order,row);
      bool null=false;
      for(size_t argument=0;argument<arguments;++argument)
        if(candidate->values[first+argument].kind==TURBODB_VALUE_NULL) { null=true; break; }
      if(null || (previous && !aggregate_distinct_compare(previous,candidate))) continue;
      steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
      status=orm_tidesdb_sql_budget_reserve(run->budget,&steps,error);
      if(status==TURBODB_STATUS_OK) status=aggregate_add(run,item_index,&candidate->values[first],error);
      previous=candidate;
    }
    first+=arguments;
  }
  return status==TURBODB_STATUS_OK?aggregate_distinct_clear(run,error):status;
}
static turbodb_status_t aggregate_finish(orm_sql_aggregate *run, turbodb_error_t *error) {
  const turbodb_status_t distinct=aggregate_distinct_finish(run,error);
  if(distinct!=TURBODB_STATUS_OK) return distinct;
  if (!vec_size(&run->counts) && !vec_size(&run->moments)) return TURBODB_STATUS_OK;
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = vec_size(&run->items);
  const turbodb_status_t status = orm_tidesdb_sql_budget_reserve(run->budget,&amount,error);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; i < vec_size(&run->items); ++i) {
    const orm_sql_aggregate_item *item = vec_at_const(&run->items,i);
    turbodb_value_t *value = vec_at(&run->output,vec_size(&run->keys)+i);
    if (orm_sql_aggregate_moment_kind(item->kind)) {
      const turbodb_status_t finished=orm_sql_reduction_result(item->kind,vec_at_const(&run->moments,i),value,error);
      if (finished != TURBODB_STATUS_OK) return finished;
      continue;
    }
    if (item->kind != ORM_SQL_AVG || value->kind == TURBODB_VALUE_NULL) continue;
    const orm_sql_reduction state={.value=*value,.count=*(const uint64_t *)vec_at_const(&run->counts,i)};
    const turbodb_status_t finished=orm_sql_reduction_result(item->kind,&state,value,error);
    if (finished != TURBODB_STATUS_OK) return finished;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t aggregate_fold(orm_sql_aggregate *run, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = vec_size(&run->items);
  const turbodb_status_t status = orm_tidesdb_sql_budget_reserve(run->budget,&amount,error);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; i < vec_size(&run->items); ++i) {
    const orm_sql_aggregate_item *item = vec_at_const(&run->items,i);
    const turbodb_value_t *input = item->kind == ORM_SQL_COUNT_ALL ? NULL : &run->pending[item->slot];
    if(orm_sql_aggregate_distinct(item)) continue;
    const turbodb_status_t added=aggregate_add(run,i,input,error);
    if (added != TURBODB_STATUS_OK) return added;
  }
  const size_t width=vec_size(&run->distinct_slots);
  if(width) {
    amount=(orm_sql_budget_amount){0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=width;
    turbodb_status_t captured=orm_tidesdb_sql_budget_reserve(run->budget,&amount,error);
    if(captured!=TURBODB_STATUS_OK) return captured;
    for(size_t i=0;i<width;++i)
      *(turbodb_value_t *)vec_at(&run->distinct_input,i)=run->pending[*(const size_t *)vec_at_const(&run->distinct_slots,i)];
    turbodb_value_t *snapshot=NULL;
    captured=orm_sql_rows_append(&run->distinct_rows,vec_data_const(&run->distinct_input),width,0,&snapshot,error);
    if(captured!=TURBODB_STATUS_OK) return captured;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t aggregate_same_group(orm_sql_aggregate *run, bool *same, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = vec_size(&run->keys);
  const turbodb_status_t status = orm_tidesdb_sql_budget_reserve(run->budget,&amount,error);
  if (status != TURBODB_STATUS_OK) return status;
  *same = true;
  for (size_t i = 0; i < vec_size(&run->keys); ++i) {
    const size_t slot = *(const size_t *)vec_at_const(&run->keys,i);
    if (orm_sql_value_order(vec_at_const(&run->output,i),&run->pending[slot])) { *same = false; break; }
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t aggregate_next(orm_sql_aggregate *run, orm_sql_scan_row *out, turbodb_error_t *error) {
  memset(vec_data(&run->output),0,vec_size(&run->output)*sizeof(turbodb_value_t));
  if (run->state == ORM_SQL_SCAN_ERROR) {
    tdsql_error_set(error,run->failure.status,run->failure.message); return run->failure.status;
  }
  if (run->state == ORM_SQL_SCAN_CANCELLED || run->state == ORM_SQL_SCAN_DONE) {
    *out = (orm_sql_scan_row){run->state,NULL,0}; return TURBODB_STATUS_OK;
  }
  turbodb_error_t cause; tdsql_error_init(&cause);
  turbodb_status_t status = aggregate_read(run,&cause);
  if (status == TURBODB_STATUS_OK && !run->pending && (vec_size(&run->keys) || run->groups)) {
    run->state = ORM_SQL_SCAN_DONE;
    *out = (orm_sql_scan_row){ORM_SQL_SCAN_DONE,NULL,0}; return TURBODB_STATUS_OK;
  }
  if (status == TURBODB_STATUS_OK) status = aggregate_begin(run,&cause);
  while (status == TURBODB_STATUS_OK && run->pending) {
    status = aggregate_fold(run,&cause);
    if (status != TURBODB_STATUS_OK) break;
    run->pending = NULL;
    status = aggregate_read(run,&cause);
    if (status != TURBODB_STATUS_OK || !run->pending) break;
    bool same;
    status = aggregate_same_group(run,&same,&cause);
    if (status != TURBODB_STATUS_OK || !same) break;
  }
  if (status == TURBODB_STATUS_OK) status = aggregate_finish(run,&cause);
  if (status != TURBODB_STATUS_OK) {
    char message[TURBODB_ERROR_MESSAGE_CAPACITY];
    (void)snprintf(message,sizeof(message),"TidesDB SQL aggregate at group %llu: %s",
        (unsigned long long)run->groups,cause.message);
    tdsql_error_set(&run->failure,status,message); tdsql_error_set(error,status,message);
    run->state = ORM_SQL_SCAN_ERROR; run->pending = NULL;
    memset(vec_data(&run->output),0,vec_size(&run->output)*sizeof(turbodb_value_t));
    return status;
  }
  run->state = ORM_SQL_SCAN_ROW;
  *out = (orm_sql_scan_row){ORM_SQL_SCAN_ROW,vec_data_const(&run->output),vec_size(&run->output)};
  return TURBODB_STATUS_OK;
}
static turbodb_status_t aggregate_pull(void *context, const turbodb_value_t **out, turbodb_error_t *error) {
  orm_sql_scan_row row;
  const turbodb_status_t status = aggregate_next(context,&row,error);
  if (status == TURBODB_STATUS_OK) *out = row.state == ORM_SQL_SCAN_ROW ? row.values : NULL;
  return status;
}
orm_sql_row_source *orm_tidesdb_sql_aggregate_source(orm_sql_aggregate *run) {
  return run && run->budget ? &run->source : NULL;
}
turbodb_status_t orm_tidesdb_sql_aggregate_next(orm_sql_aggregate *run,
    orm_sql_scan_row *out, turbodb_error_t *error) {
  if (!run || !run->budget || !out)
    return aggregate_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL aggregate next");
  if (run->source.active) return aggregate_error(error,TURBODB_STATUS_BUSY,aggregate_output_busy);
  return aggregate_next(run,out,error);
}
turbodb_status_t orm_tidesdb_sql_aggregate_cancel(orm_sql_aggregate *run, turbodb_error_t *error) {
  if (!run || !run->budget)
    return aggregate_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL aggregate cancel");
  if (run->source.active) return aggregate_error(error,TURBODB_STATUS_BUSY,aggregate_output_busy);
  const turbodb_status_t status = orm_tidesdb_sql_scan_cancel(&run->scan,error);
  if (status != TURBODB_STATUS_OK) return status;
  if (run->state == ORM_SQL_SCAN_OPEN || run->state == ORM_SQL_SCAN_ROW) run->state = ORM_SQL_SCAN_CANCELLED;
  run->pending = NULL;
  memset(vec_data(&run->output),0,vec_size(&run->output)*sizeof(turbodb_value_t));
  return TURBODB_STATUS_OK;
}
