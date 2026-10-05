#include "window.h"
#include "work.h"
#include "error.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static const char window_busy[] = "SQL window consumer or callback still active";
static turbodb_status_t window_error(turbodb_error_t *error, turbodb_status_t status, const char *message) {
  tdsql_error_set(error,status,message); return status;
}
static turbodb_status_t window_steps(orm_sql_window *run, size_t steps, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = steps;
  return orm_tidesdb_sql_budget_reserve(run->budget,&amount,error);
}
static bool window_key_type(orm_sql_type type) {
  return type.kind == TURBODB_VALUE_NULL || type.kind == TURBODB_VALUE_INT64 ||
      type.kind == TURBODB_VALUE_UINT64 || type.kind == TURBODB_VALUE_DOUBLE || type.kind == TURBODB_VALUE_BOOLEAN;
}
turbodb_status_t orm_sql_window_offset_type(orm_sql_type value, orm_sql_type fallback,
    orm_sql_type *out, turbodb_error_t *error) {
  if (!out || (!window_key_type(value) && value.kind != TURBODB_VALUE_TEXT && value.kind != TURBODB_VALUE_BLOB) ||
      (!window_key_type(fallback) && fallback.kind != TURBODB_VALUE_TEXT && fallback.kind != TURBODB_VALUE_BLOB))
    return window_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL offset window types");
  if (value.kind != TURBODB_VALUE_NULL && fallback.kind != TURBODB_VALUE_NULL && value.kind != fallback.kind)
    return window_error(error,TURBODB_STATUS_UNSUPPORTED,"LAG/LEAD value and default require the same type or NULL");
  *out = (orm_sql_type){value.kind == TURBODB_VALUE_NULL ? fallback.kind : value.kind,
      value.nullable || fallback.nullable || value.kind == TURBODB_VALUE_NULL || fallback.kind == TURBODB_VALUE_NULL};
  return TURBODB_STATUS_OK;
}
static turbodb_status_t window_validate_frame(orm_sql_row_source *source, const orm_sql_window_spec *spec, turbodb_error_t *error) {
  const orm_sql_window_frame *frame=&spec->frame;
  if (frame->unit == ORM_SQL_FRAME_DEFAULT) return TURBODB_STATUS_OK;
  if (frame->unit != ORM_SQL_FRAME_ROWS && frame->unit != ORM_SQL_FRAME_RANGE)
    return window_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL frame unit");
  const orm_sql_frame_boundary_kind start=frame->boundaries[0].kind,end=frame->boundaries[1].kind;
  if (start < ORM_SQL_BOUND_UNBOUNDED_PRECEDING || start > ORM_SQL_BOUND_UNBOUNDED_FOLLOWING ||
      end < ORM_SQL_BOUND_UNBOUNDED_PRECEDING || end > ORM_SQL_BOUND_UNBOUNDED_FOLLOWING ||
      start == ORM_SQL_BOUND_UNBOUNDED_FOLLOWING || end == ORM_SQL_BOUND_UNBOUNDED_PRECEDING || start > end)
    return window_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL frame boundary order");
  for(size_t i=0;i<ORM_SQL_FRAME_BOUNDARIES;++i) {
    const orm_sql_frame_boundary *bound=&frame->boundaries[i];
    if (bound->kind != ORM_SQL_BOUND_PRECEDING && bound->kind != ORM_SQL_BOUND_FOLLOWING) continue;
    const turbodb_value_t *value=&bound->distance;
    if (value->reserved || (value->kind != TURBODB_VALUE_INT64 && value->kind != TURBODB_VALUE_UINT64 &&
        (frame->unit != ORM_SQL_FRAME_RANGE || value->kind != TURBODB_VALUE_DOUBLE)) ||
        (value->kind == TURBODB_VALUE_INT64 && value->data.int64_value < 0) ||
        (value->kind == TURBODB_VALUE_DOUBLE && (!isfinite(value->data.double_value) || value->data.double_value < 0)))
      return window_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL frame distance");
    if (frame->unit == ORM_SQL_FRAME_RANGE && (spec->order_count != 1 ||
        source->types[spec->orders[0].slot].kind == TURBODB_VALUE_NULL))
      return window_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"RANGE distance requires one numeric order key");
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t window_validate(orm_sql_row_source *source,
    const orm_sql_window_spec *spec, turbodb_error_t *error) {
  if (!source || !source->budget || !source->types || !source->columns || !source->context || !source->next ||
      !spec || (spec->partition_count && !spec->partitions) || (spec->order_count && !spec->orders))
    return window_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL window specification");
  if (source->active) return window_error(error,TURBODB_STATUS_BUSY,"SQL window input already active");
  if (spec->kind < ORM_SQL_ROW_NUMBER || spec->kind > ORM_SQL_WINDOW_BIT_XOR)
    return window_error(error,TURBODB_STATUS_UNSUPPORTED,"unsupported SQL window function");
  if (spec->kind == ORM_SQL_NTILE ? (!spec->buckets || spec->buckets > ORM_SQL_WINDOW_MAX_BUCKETS) : spec->buckets != 0)
    return window_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL window bucket count");
  if (orm_sql_window_value_kind(spec->kind)) {
    if ((orm_sql_window_offset_kind(spec->kind) ? spec->offset > ORM_SQL_WINDOW_MAX_OFFSET :
        spec->kind == ORM_SQL_NTH_VALUE ? (!spec->offset || spec->offset > ORM_SQL_WINDOW_MAX_NTH) : spec->offset != 0) ||
        (orm_sql_window_frame_kind(spec->kind) && (spec->has_default || spec->default_slot)) || spec->value_slot >= source->columns ||
        (spec->has_default && spec->default_slot >= source->columns))
      return window_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL window offset or value slot");
    orm_sql_type result;
    const turbodb_status_t status = orm_sql_window_aggregate_kind(spec->kind) ?
        orm_tidesdb_sql_aggregate_type(orm_sql_window_reduction_kind(spec->kind),&source->types[spec->value_slot],&result,error) :
        orm_sql_window_offset_type(source->types[spec->value_slot],
        spec->has_default ? source->types[spec->default_slot] : (orm_sql_type){TURBODB_VALUE_NULL,true},&result,error);
    if (status != TURBODB_STATUS_OK) return status;
  } else if (spec->offset || spec->value_slot || spec->default_slot || spec->has_default)
    return window_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"offset slots require LAG or LEAD");
  const uint64_t limit = source->budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES];
  if (source->columns == SIZE_MAX || source->columns+1 > limit ||
      spec->partition_count > SIZE_MAX-spec->order_count || spec->partition_count+spec->order_count > limit)
    return window_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"SQL window shape exceeds plan capacity");
  orm_sql_budget_amount amount = {0};
  amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = spec->partition_count+spec->order_count;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(source->budget,&amount,error);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; i < spec->partition_count+spec->order_count; ++i) {
    const bool partition = i < spec->partition_count;
    const orm_sql_scan_order *order = partition ? NULL : &spec->orders[i-spec->partition_count];
    const size_t slot = partition ? spec->partitions[i] : order->slot;
    if (slot >= source->columns)
      return window_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"SQL window key outside input");
    if (!window_key_type(source->types[slot]))
      return window_error(error,TURBODB_STATUS_UNSUPPORTED,"SQL window requires numeric, BOOL or NULL keys");
    if (order && (order->expression.program || order->expression.slots || order->expression.count ||
        order->expression.query_slots || order->expression.query_count))
      return window_error(error,TURBODB_STATUS_UNSUPPORTED,"SQL window key expressions require prior evaluation");
  }
  return window_validate_frame(source,spec,error);
}
turbodb_status_t orm_tidesdb_sql_window_close(orm_sql_window *run, turbodb_error_t *error) {
  if (!run || !run->budget) return TURBODB_STATUS_OK;
  if (run->evaluating || run->source.active) return window_error(error,TURBODB_STATUS_BUSY,window_busy);
  turbodb_status_t status = orm_tidesdb_sql_scan_close(&run->scan,error);
  if (status != TURBODB_STATUS_OK) return status;
  status = orm_sql_rows_close(&run->rows,error);
  vec_t *vectors[] = {&run->orders,&run->types};
  const size_t bytes[] = {run->order_bytes,run->type_bytes};
  for (size_t i = 0; i < sizeof(vectors)/sizeof(vectors[0]); ++i) {
    const turbodb_status_t released = orm_sql_work_release(vectors[i],bytes[i],run->budget,
        status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  if (run->metadata_bytes) {
    const turbodb_status_t released = orm_tidesdb_sql_budget_release(run->budget,ORM_SQL_BUDGET_WORK_BYTES,
        run->metadata_bytes,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  *run = (orm_sql_window){0}; return status;
}
static turbodb_status_t window_pull(void *context, const turbodb_value_t **out, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_window_open(orm_sql_row_source *source,
    const orm_sql_window_spec *spec, orm_sql_window *out, turbodb_error_t *error) {
  if (!out || out->budget)
    return window_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"SQL window requires empty output");
  turbodb_status_t status = window_validate(source,spec,error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_window run = {.budget=source->budget,.state=ORM_SQL_SCAN_OPEN,.kind=spec->kind,
      .buckets=spec->buckets,.partition_count=spec->partition_count,.input_columns=source->columns,
      .offset=spec->offset,.value_slot=spec->value_slot,.default_slot=spec->default_slot,.has_default=spec->has_default,.frame=spec->frame};
  if (run.frame.unit == ORM_SQL_FRAME_DEFAULT) {
    run.frame.unit=ORM_SQL_FRAME_RANGE;
    run.frame.boundaries[0].kind=ORM_SQL_BOUND_UNBOUNDED_PRECEDING;
    run.frame.boundaries[1].kind=spec->order_count ? ORM_SQL_BOUND_CURRENT_ROW : ORM_SQL_BOUND_UNBOUNDED_FOLLOWING;
  }
  run.rows.budget = run.budget; tdsql_error_init(&run.failure);
  vec_t projection = {0}; size_t projection_bytes = 0;
  const size_t width = source->columns+1, keys = spec->partition_count+spec->order_count;
  status = orm_tidesdb_sql_budget_reserve_capacity(run.budget,1,sizeof(run),0,&run.metadata_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run.orders,keys,sizeof(orm_sql_scan_order),
      _Alignof(orm_sql_scan_order),run.budget,&run.order_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run.types,width,sizeof(orm_sql_type),
      _Alignof(orm_sql_type),run.budget,&run.type_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&projection,source->columns,sizeof(size_t),
      _Alignof(size_t),run.budget,&projection_bytes,error);
  if (status == TURBODB_STATUS_OK) status = window_steps(&run,width,error);
  if (status == TURBODB_STATUS_OK) {
    memcpy(vec_data(&run.types),source->types,source->columns*sizeof(orm_sql_type));
    orm_sql_type result_type = {
        spec->kind == ORM_SQL_PERCENT_RANK || spec->kind == ORM_SQL_CUME_DIST ? TURBODB_VALUE_DOUBLE : TURBODB_VALUE_INT64,false};
    if (orm_sql_window_aggregate_kind(spec->kind))
      status=orm_tidesdb_sql_aggregate_type(orm_sql_window_reduction_kind(spec->kind),
          spec->kind == ORM_SQL_WINDOW_COUNT_ALL ? NULL : &source->types[spec->value_slot],&result_type,error);
    else if (orm_sql_window_value_kind(spec->kind))
      status = orm_sql_window_offset_type(source->types[spec->value_slot],
          spec->has_default ? source->types[spec->default_slot] : (orm_sql_type){TURBODB_VALUE_NULL,true},&result_type,error);
    *(orm_sql_type *)vec_at(&run.types,source->columns) = result_type;
    for (size_t i = 0; i < source->columns; ++i) *(size_t *)vec_at(&projection,i) = i;
    for (size_t i = 0; i < spec->partition_count; ++i)
      *(orm_sql_scan_order *)vec_at(&run.orders,i) = (orm_sql_scan_order){.slot=spec->partitions[i]};
    for (size_t i = 0; i < spec->order_count; ++i)
      *(orm_sql_scan_order *)vec_at(&run.orders,spec->partition_count+i) = spec->orders[i];
    const orm_sql_scan_spec scan = {.projection=vec_data_const(&projection),.projection_count=source->columns,
        .limit=UINT64_MAX,.orders=vec_data_const(&run.orders),.order_count=keys};
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_scan_open_source(source,&scan,run.budget,&run.scan,error);
  }
  const turbodb_status_t released = orm_sql_work_release(&projection,projection_bytes,run.budget,
      status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t closed = orm_tidesdb_sql_window_close(&run,NULL);
    return closed == TURBODB_STATUS_OK ? status : closed;
  }
  *out = run;
  out->source = (orm_sql_row_source){out->budget,vec_data_const(&out->types),width,out,window_pull,false};
  return TURBODB_STATUS_OK;
}
static turbodb_status_t window_same(orm_sql_window *run, size_t left, size_t right,
    size_t begin, size_t end, bool *same, turbodb_error_t *error) {
  turbodb_status_t status = window_steps(run,end-begin,error);
  if (status != TURBODB_STATUS_OK) return status;
  const turbodb_value_t *a = orm_sql_rows_at(&run->rows,left), *b = orm_sql_rows_at(&run->rows,right);
  *same = true;
  for (size_t i = begin; i < end; ++i) {
    const size_t slot = ((const orm_sql_scan_order *)vec_at_const(&run->orders,i))->slot;
    if (orm_sql_value_order(&a[slot],&b[slot])) { *same = false; break; }
  }
  return TURBODB_STATUS_OK;
}
static int64_t window_bucket(size_t ordinal, size_t count, uint64_t buckets) {
  if (buckets >= count) return (int64_t)ordinal+1;
  const size_t n = (size_t)buckets, base = count/n, remainder = count%n;
  /* count = base*n+remainder, n>=1: cutoff never exceeds count. */
  const size_t cutoff = remainder ? (base+1)*remainder : 0;
  return (int64_t)(ordinal < cutoff ? ordinal/(base+1)+1 : remainder+(ordinal-cutoff)/base+1);
}
static uint64_t window_integer(const turbodb_value_t *value) {
  return value->kind == TURBODB_VALUE_INT64 ? (uint64_t)value->data.int64_value :
      value->kind == TURBODB_VALUE_UINT64 ? value->data.uint64_value : (uint64_t)value->data.boolean_value;
}
static double window_real(const turbodb_value_t *value) {
  return value->kind == TURBODB_VALUE_DOUBLE ? value->data.double_value :
      value->kind == TURBODB_VALUE_INT64 ? (double)value->data.int64_value : (double)window_integer(value);
}
static int window_range_compare(const turbodb_value_t *candidate, const turbodb_value_t *reference,
    const orm_sql_frame_boundary *bound, bool descending) {
  if (candidate->kind == TURBODB_VALUE_NULL) return descending ? 1 : -1;
  const bool plus = (bound->kind == ORM_SQL_BOUND_FOLLOWING) != descending;
  int comparison;
  if (reference->kind == TURBODB_VALUE_DOUBLE || bound->distance.kind == TURBODB_VALUE_DOUBLE) {
    const double target = window_real(reference)+(plus ? window_real(&bound->distance) : -window_real(&bound->distance));
    const double value = window_real(candidate);
    comparison = value < target ? -1 : value > target ? 1 : 0;
  } else {
    comparison = orm_sql_value_order(candidate,reference);
    if ((plus && comparison >= 0) || (!plus && comparison <= 0)) {
      /* Modulo subtraction preserves the exact ordered distance even across INT64 zero. */
      const uint64_t distance = plus ? window_integer(candidate)-window_integer(reference) :
          window_integer(reference)-window_integer(candidate);
      const uint64_t limit = window_integer(&bound->distance);
      comparison = (distance > limit)-(distance < limit);
      if (!plus) comparison = -comparison;
    }
  }
  return descending ? -comparison : comparison;
}
static size_t window_rows_bound(const orm_sql_frame_boundary *bound, size_t begin, size_t end,
    size_t position, bool upper) {
  if (bound->kind == ORM_SQL_BOUND_UNBOUNDED_PRECEDING) return begin;
  if (bound->kind == ORM_SQL_BOUND_UNBOUNDED_FOLLOWING) return end;
  if (bound->kind == ORM_SQL_BOUND_CURRENT_ROW) return position+(upper ? 1u : 0u);
  const uint64_t distance = window_integer(&bound->distance);
  if (bound->kind == ORM_SQL_BOUND_PRECEDING)
    return distance > position-begin ? begin : position-(size_t)distance+(upper ? 1u : 0u);
  const size_t available = end-position-(upper ? 1u : 0u);
  return distance >= available ? end : position+(size_t)distance+(upper ? 1u : 0u);
}
static turbodb_status_t window_range_bound(orm_sql_window *run, const orm_sql_frame_boundary *bound,
    size_t begin, size_t end, size_t position, size_t peer, size_t after, bool upper,
    size_t *out, turbodb_error_t *error) {
  if (bound->kind == ORM_SQL_BOUND_UNBOUNDED_PRECEDING) { *out=begin; return TURBODB_STATUS_OK; }
  if (bound->kind == ORM_SQL_BOUND_UNBOUNDED_FOLLOWING) { *out=end; return TURBODB_STATUS_OK; }
  if (bound->kind == ORM_SQL_BOUND_CURRENT_ROW) { *out=upper ? after : peer; return TURBODB_STATUS_OK; }
  const orm_sql_scan_order *order = vec_at_const(&run->orders,run->partition_count);
  const turbodb_value_t *reference = &orm_sql_rows_at(&run->rows,position)[order->slot];
  if (reference->kind == TURBODB_VALUE_NULL) { *out=upper ? after : peer; return TURBODB_STATUS_OK; }
  /* Two binary searches per row; no frame-sized allocation or repeated frame scan. */
  while (begin < end) {
    const turbodb_status_t status = window_steps(run,1,error);
    if (status != TURBODB_STATUS_OK) return status;
    const size_t middle = begin+(end-begin)/2;
    const int comparison = window_range_compare(&orm_sql_rows_at(&run->rows,middle)[order->slot],reference,bound,order->descending);
    if (comparison < 0 || (upper && comparison == 0)) begin=middle+1;
    else end=middle;
  }
  *out=begin; return TURBODB_STATUS_OK;
}
static turbodb_status_t window_frame_extent(orm_sql_window *run, size_t begin, size_t end,
    size_t position, size_t peer, size_t after, size_t bounds[ORM_SQL_FRAME_BOUNDARIES], turbodb_error_t *error) {
  for (size_t i=0;i<ORM_SQL_FRAME_BOUNDARIES;++i) {
    const orm_sql_frame_boundary *bound=&run->frame.boundaries[i];
    if (run->frame.unit == ORM_SQL_FRAME_ROWS) bounds[i]=window_rows_bound(bound,begin,end,position,i != 0);
    else {
      const turbodb_status_t status=window_range_bound(run,bound,begin,end,position,peer,after,i != 0,&bounds[i],error);
      if (status != TURBODB_STATUS_OK) return status;
    }
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t window_frame_value(orm_sql_window *run, size_t begin, size_t end,
    size_t position, size_t peer, size_t after, turbodb_value_t *out, turbodb_error_t *error) {
  size_t bounds[ORM_SQL_FRAME_BOUNDARIES];
  const turbodb_status_t status=window_frame_extent(run,begin,end,position,peer,after,bounds,error);
  if (status != TURBODB_STATUS_OK) return status;
  const uint64_t nth=run->kind == ORM_SQL_NTH_VALUE ? run->offset : 1;
  if (bounds[0] >= bounds[1] || nth > bounds[1]-bounds[0]) *out=turbodb_null();
  else {
    const size_t target=run->kind == ORM_SQL_LAST_VALUE ? bounds[1]-1 : bounds[0]+(size_t)(nth-1);
    *out=orm_sql_rows_at(&run->rows,target)[run->value_slot];
  }
  return TURBODB_STATUS_OK;
}
typedef struct window_reduction_cache {
  orm_sql_reduction state;
  size_t begin,end;
  bool valid;
} window_reduction_cache;
static turbodb_status_t window_frame_reduce(orm_sql_window *run, size_t bounds[ORM_SQL_FRAME_BOUNDARIES],
    window_reduction_cache *cache, turbodb_value_t *out, turbodb_error_t *error) {
  turbodb_status_t status=window_steps(run,1,error);
  if (status != TURBODB_STATUS_OK) return status;
  const orm_sql_aggregate_kind kind=orm_sql_window_reduction_kind(run->kind);
  if (kind == ORM_SQL_COUNT_ALL) { *out=turbodb_i64(bounds[0]<bounds[1] ? (int64_t)(bounds[1]-bounds[0]) : 0); return TURBODB_STATUS_OK; }
  if (bounds[0]>=bounds[1]) { *out=orm_sql_reduction_begin(kind).value; return TURBODB_STATUS_OK; }
  if (!cache->valid || cache->begin != bounds[0] || cache->end>bounds[1]) {
    *cache=(window_reduction_cache){orm_sql_reduction_begin(kind),bounds[0],bounds[0],true};
  }
  /* Reuse only the same ordered prefix; moving the start requires a new fold.
   * Inverse DOUBLE sums would change rounding and hide intermediate overflow. */
  for (;cache->end<bounds[1];++cache->end) {
    status=window_steps(run,1,error);
    if (status == TURBODB_STATUS_OK) status=orm_sql_reduction_add(kind,&cache->state,
        &orm_sql_rows_at(&run->rows,cache->end)[run->value_slot],error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  return orm_sql_reduction_result(kind,&cache->state,out,error);
}
static turbodb_status_t window_partition(orm_sql_window *run, size_t begin, size_t end, turbodb_error_t *error) {
  if (orm_sql_window_offset_kind(run->kind)) {
    turbodb_status_t status = window_steps(run,end-begin,error);
    if (status != TURBODB_STATUS_OK) return status;
    for (size_t i = begin; i < end; ++i) {
      orm_sql_snapshot *snapshot = vec_at(&run->rows.snapshots,i);
      turbodb_value_t *result = vec_at(&snapshot->values,run->input_columns);
      const size_t distance = run->kind == ORM_SQL_LAG ? i-begin : end-i-1;
      if (run->offset <= distance) {
        const size_t target = run->kind == ORM_SQL_LAG ? i-(size_t)run->offset : i+(size_t)run->offset;
        /* All snapshots are complete and immutable until this owner closes. */
        *result = orm_sql_rows_at(&run->rows,target)[run->value_slot];
      } else *result = run->has_default ? ((const turbodb_value_t *)vec_data_const(&snapshot->values))[run->default_slot] : turbodb_null();
    }
    return TURBODB_STATUS_OK;
  }
  size_t dense = 0;
  window_reduction_cache cache={0};
  for (size_t peer = begin; peer < end;) {
    size_t after = peer+1; ++dense;
    while (after < end) {
      bool same;
      turbodb_status_t status = window_same(run,peer,after,run->partition_count,vec_size(&run->orders),&same,error);
      if (status != TURBODB_STATUS_OK) return status;
      if (!same) break;
      ++after;
    }
    turbodb_status_t status = window_steps(run,after-peer,error);
    if (status != TURBODB_STATUS_OK) return status;
    for (size_t i = peer; i < after; ++i) {
      orm_sql_snapshot *snapshot = vec_at(&run->rows.snapshots,i);
      turbodb_value_t *result = vec_at(&snapshot->values,run->input_columns);
      switch (run->kind) {
        case ORM_SQL_ROW_NUMBER: *result = turbodb_i64((int64_t)(i-begin)+1); break;
        case ORM_SQL_RANK: *result = turbodb_i64((int64_t)(peer-begin)+1); break;
        case ORM_SQL_DENSE_RANK: *result = turbodb_i64((int64_t)dense); break;
        case ORM_SQL_PERCENT_RANK:
          *result = turbodb_f64(end-begin == 1 ? 0.0 : (double)(peer-begin)/(double)(end-begin-1)); break;
        case ORM_SQL_CUME_DIST: *result = turbodb_f64((double)(after-begin)/(double)(end-begin)); break;
        case ORM_SQL_NTILE: *result = turbodb_i64(window_bucket(i-begin,end-begin,run->buckets)); break;
        case ORM_SQL_FIRST_VALUE: case ORM_SQL_LAST_VALUE: case ORM_SQL_NTH_VALUE:
          status=window_frame_value(run,begin,end,i,peer,after,result,error);
          if (status != TURBODB_STATUS_OK) return status;
          break;
        case ORM_SQL_WINDOW_COUNT_ALL: case ORM_SQL_WINDOW_COUNT_VALUE: case ORM_SQL_WINDOW_MIN:
        case ORM_SQL_WINDOW_MAX: case ORM_SQL_WINDOW_SUM: case ORM_SQL_WINDOW_AVG:
        case ORM_SQL_WINDOW_VAR_POP: case ORM_SQL_WINDOW_VAR_SAMP:
        case ORM_SQL_WINDOW_STDDEV_POP: case ORM_SQL_WINDOW_STDDEV_SAMP:
        case ORM_SQL_WINDOW_BIT_AND: case ORM_SQL_WINDOW_BIT_OR: case ORM_SQL_WINDOW_BIT_XOR: {
          size_t bounds[ORM_SQL_FRAME_BOUNDARIES];
          status=window_frame_extent(run,begin,end,i,peer,after,bounds,error);
          if (status == TURBODB_STATUS_OK) status=window_frame_reduce(run,bounds,&cache,result,error);
          if (status != TURBODB_STATUS_OK) return status;
          break;
        }
        default: return window_error(error,TURBODB_STATUS_INVALID_STATE,"offset window reached the rank calculation path");
      }
    }
    peer = after;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t window_prepare(orm_sql_window *run, turbodb_error_t *error) {
  for (;;) {
    orm_sql_scan_row row;
    turbodb_status_t status = orm_tidesdb_sql_scan_next(&run->scan,&row,error);
    if (status != TURBODB_STATUS_OK) return status;
    if (row.state == ORM_SQL_SCAN_DONE) break;
    if (row.state != ORM_SQL_SCAN_ROW)
      return window_error(error,TURBODB_STATUS_INVALID_STATE,"SQL window input is cancelled");
    if (vec_size(&run->rows.snapshots) >= INT64_MAX)
      return window_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"SQL window row count exceeds INT64");
    turbodb_value_t *copy;
    status = orm_sql_rows_append(&run->rows,row.values,run->input_columns,1,&copy,error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  const size_t count = vec_size(&run->rows.snapshots);
  for (size_t begin = 0; begin < count;) {
    size_t end = begin+1;
    while (end < count) {
      bool same;
      turbodb_status_t status = window_same(run,begin,end,0,run->partition_count,&same,error);
      if (status != TURBODB_STATUS_OK) return status;
      if (!same) break;
      ++end;
    }
    turbodb_status_t status = window_partition(run,begin,end,error);
    if (status != TURBODB_STATUS_OK) return status;
    begin = end;
  }
  run->prepared = true; return TURBODB_STATUS_OK;
}
static turbodb_status_t window_next(orm_sql_window *run, orm_sql_scan_row *out, turbodb_error_t *error) {
  if (run->evaluating) return window_error(error,TURBODB_STATUS_BUSY,window_busy);
  if (run->state == ORM_SQL_SCAN_ERROR) {
    tdsql_error_set(error,run->failure.status,run->failure.message); return run->failure.status;
  }
  if (run->state == ORM_SQL_SCAN_DONE || run->state == ORM_SQL_SCAN_CANCELLED) {
    *out = (orm_sql_scan_row){run->state,NULL,0}; return TURBODB_STATUS_OK;
  }
  run->evaluating = true;
  turbodb_error_t cause; tdsql_error_init(&cause);
  turbodb_status_t status = run->prepared ? TURBODB_STATUS_OK : window_prepare(run,&cause);
  if (status == TURBODB_STATUS_OK && run->position < vec_size(&run->rows.snapshots)) status = window_steps(run,1,&cause);
  run->evaluating = false;
  if (status != TURBODB_STATUS_OK) {
    char message[TURBODB_ERROR_MESSAGE_CAPACITY];
    (void)snprintf(message,sizeof(message),"TidesDB SQL window at row %zu: %s",run->position,cause.message);
    tdsql_error_set(&run->failure,status,message); tdsql_error_set(error,status,message);
    run->state = ORM_SQL_SCAN_ERROR; return status;
  }
  if (run->position == vec_size(&run->rows.snapshots)) {
    run->state = ORM_SQL_SCAN_DONE; *out = (orm_sql_scan_row){ORM_SQL_SCAN_DONE,NULL,0};
  } else {
    run->state = ORM_SQL_SCAN_ROW;
    *out = (orm_sql_scan_row){ORM_SQL_SCAN_ROW,orm_sql_rows_at(&run->rows,run->position++),run->input_columns+1};
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t window_pull(void *context, const turbodb_value_t **out, turbodb_error_t *error) {
  orm_sql_scan_row row;
  const turbodb_status_t status = window_next(context,&row,error);
  if (status == TURBODB_STATUS_OK) *out = row.state == ORM_SQL_SCAN_ROW ? row.values : NULL;
  return status;
}
orm_sql_row_source *orm_tidesdb_sql_window_source(orm_sql_window *run) {
  return run && run->budget ? &run->source : NULL;
}
turbodb_status_t orm_tidesdb_sql_window_next(orm_sql_window *run, orm_sql_scan_row *out, turbodb_error_t *error) {
  if (!run || !run->budget || !out) return window_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL window next");
  if (run->source.active) return window_error(error,TURBODB_STATUS_BUSY,window_busy);
  return window_next(run,out,error);
}
turbodb_status_t orm_tidesdb_sql_window_cancel(orm_sql_window *run, turbodb_error_t *error) {
  if (!run || !run->budget) return window_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL window cancel");
  if (run->evaluating || run->source.active) return window_error(error,TURBODB_STATUS_BUSY,window_busy);
  const turbodb_status_t status = orm_tidesdb_sql_scan_cancel(&run->scan,error);
  if (status != TURBODB_STATUS_OK) return status;
  if (run->state == ORM_SQL_SCAN_OPEN || run->state == ORM_SQL_SCAN_ROW) run->state = ORM_SQL_SCAN_CANCELLED;
  return TURBODB_STATUS_OK;
}
