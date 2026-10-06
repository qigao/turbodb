#include "union.h"
#include "work.h"
#include "error.h"

static const char union_busy[] = "SQL UNION output has an active consumer";
enum { UNION_TYPE_ARRAYS=2 };
static turbodb_status_t union_error(turbodb_error_t *error, turbodb_status_t status, const char *reason) {
  tdsql_error_set(error,status,reason); return status;
}
turbodb_status_t orm_tidesdb_sql_union_type(orm_sql_type left, orm_sql_type right,
    orm_sql_type *out, turbodb_error_t *error) {
  if (!out) return union_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"UNION type output required");
  orm_sql_predicate validator;
  turbodb_status_t status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,left,NULL,&validator,error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,right,NULL,&validator,error);
  if (status != TURBODB_STATUS_OK) return status;
  const bool real=(left.kind==TURBODB_VALUE_DOUBLE && (right.kind==TURBODB_VALUE_INT64 || right.kind==TURBODB_VALUE_UINT64)) ||
      (right.kind==TURBODB_VALUE_DOUBLE && (left.kind==TURBODB_VALUE_INT64 || left.kind==TURBODB_VALUE_UINT64));
  if (left.kind != right.kind && left.kind != TURBODB_VALUE_NULL && right.kind != TURBODB_VALUE_NULL && !real)
    return union_error(error,TURBODB_STATUS_UNSUPPORTED,"UNION mixed non-NULL types need explicit type conversion support");
  *out = (orm_sql_type){real ? TURBODB_VALUE_DOUBLE : left.kind == TURBODB_VALUE_NULL ? right.kind : left.kind,left.nullable || right.nullable};
  return TURBODB_STATUS_OK;
}
bool orm_sql_union_nullable(orm_sql_union_kind kind,orm_sql_type left,orm_sql_type right) {
  if(kind==ORM_SQL_INTERSECT_ALL || kind==ORM_SQL_INTERSECT_DISTINCT) return left.nullable && right.nullable;
  if(kind==ORM_SQL_EXCEPT_ALL || kind==ORM_SQL_EXCEPT_DISTINCT) return left.nullable;
  return left.nullable || right.nullable;
}
static turbodb_status_t union_result_type(orm_sql_type left,orm_sql_type right,orm_sql_union_kind kind,
    orm_sql_type *out,turbodb_error_t *error) {
  orm_sql_type result_type={0};
  const turbodb_status_t status=orm_tidesdb_sql_union_type(left,right,&result_type,error);
  if(status==TURBODB_STATUS_OK) {
    result_type.nullable=orm_sql_union_nullable(kind,left,right); *out=result_type;
  }
  return status;
}
turbodb_status_t orm_tidesdb_sql_union_close(orm_sql_union *run, turbodb_error_t *error) {
  if (!run || !run->budget) return TURBODB_STATUS_OK;
  if (run->source.active) return union_error(error,TURBODB_STATUS_BUSY,union_busy);
  turbodb_status_t status = orm_tidesdb_sql_scan_close(&run->output,error);
  for (size_t i = 0; i < ORM_SQL_UNION_INPUTS; ++i) {
    const turbodb_status_t released = orm_tidesdb_sql_scan_close(&run->inputs[i],status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  turbodb_status_t released = orm_sql_work_release(&run->types,run->type_bytes,run->budget,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  if (run->metadata_bytes) {
    released = orm_tidesdb_sql_budget_release(run->budget,ORM_SQL_BUDGET_WORK_BYTES,run->metadata_bytes,
        status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  *run = (orm_sql_union){0}; return status;
}
static turbodb_status_t union_concat(void *context, const turbodb_value_t **out, turbodb_error_t *error) {
  orm_sql_union *run = context;
  while (run->position < ORM_SQL_UNION_INPUTS) {
    orm_sql_scan_row row;
    const turbodb_status_t status = orm_tidesdb_sql_scan_next(&run->inputs[run->position],&row,error);
    if (status != TURBODB_STATUS_OK) return status;
    if (row.state == ORM_SQL_SCAN_ROW) { *out = row.values; return TURBODB_STATUS_OK; }
    if (row.state != ORM_SQL_SCAN_DONE) return union_error(error,TURBODB_STATUS_INVALID_STATE,"UNION input cancelled");
    ++run->position;
  }
  *out = NULL; return TURBODB_STATUS_OK;
}
static turbodb_status_t union_set_advance(orm_sql_union *run,size_t side,turbodb_error_t *error) {
  orm_sql_budget_amount steps={0}; steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
  turbodb_status_t status=orm_tidesdb_sql_budget_reserve(run->budget,&steps,error);
  orm_sql_scan_row row={0};
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_scan_next(&run->inputs[side],&row,error);
  if(status!=TURBODB_STATUS_OK) return status;
  if(row.state!=ORM_SQL_SCAN_ROW && row.state!=ORM_SQL_SCAN_DONE)
    return union_error(error,TURBODB_STATUS_INVALID_STATE,"set operation input cancelled");
  run->pending[side]=row.state==ORM_SQL_SCAN_ROW?row.values:NULL;
  run->advance[side]=false; return TURBODB_STATUS_OK;
}
static turbodb_status_t union_set_pull(void *context,const turbodb_value_t **out,turbodb_error_t *error) {
  orm_sql_union *run=context;
  for(size_t side=0;side<ORM_SQL_UNION_INPUTS;++side) {
    if(!run->prepared || run->advance[side]) {
      const turbodb_status_t status=union_set_advance(run,side,error);
      if(status!=TURBODB_STATUS_OK) return status;
    }
  }
  run->prepared=true;
  const bool intersect=run->kind==ORM_SQL_INTERSECT_ALL || run->kind==ORM_SQL_INTERSECT_DISTINCT;
  while(run->pending[0]) {
    orm_sql_budget_amount steps={0}; steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=run->concat.columns;
    turbodb_status_t status=orm_tidesdb_sql_budget_reserve(run->budget,&steps,error);
    if(status!=TURBODB_STATUS_OK) return status;
    int order=-1;
    if(run->pending[1]) for(size_t column=0;column<run->concat.columns;++column) {
      order=orm_sql_value_order(&run->pending[0][column],&run->pending[1][column]);
      if(order) break;
    }
    if((intersect && !order) || (!intersect && order<0)) {
      *out=run->pending[0]; run->advance[0]=true; run->advance[1]=!order;
      return TURBODB_STATUS_OK;
    }
    if(order<=0) status=union_set_advance(run,0,error);
    if(status==TURBODB_STATUS_OK && order>=0) status=union_set_advance(run,1,error);
    if(status!=TURBODB_STATUS_OK) return status;
  }
  *out=NULL; return TURBODB_STATUS_OK;
}
static turbodb_status_t union_pull(void *context, const turbodb_value_t **out, turbodb_error_t *error) {
  orm_sql_scan_row row;
  const turbodb_status_t status = orm_tidesdb_sql_scan_next(&((orm_sql_union *)context)->output,&row,error);
  if (status == TURBODB_STATUS_OK) *out = row.state == ORM_SQL_SCAN_ROW ? row.values : NULL;
  return status;
}
turbodb_status_t orm_sql_union_validate_as(const orm_sql_row_source *left, const orm_sql_row_source *right,
    orm_sql_union_kind kind, const orm_sql_type *types, turbodb_error_t *error) {
  if (!left || !right || left == right || !left->budget || left->budget != right->budget ||
      !left->next || !right->next || !left->types || !right->types || !left->columns || !right->columns)
    return union_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"UNION requires independent sources with one budget");
  if (left->active || right->active) return union_error(error,TURBODB_STATUS_BUSY,"UNION input already active");
  if (kind < ORM_SQL_UNION_ALL || kind > ORM_SQL_EXCEPT_DISTINCT)
    return union_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid UNION mode");
  if (left->columns != right->columns) return union_error(error,TURBODB_STATUS_SQL_ERROR,"UNION branch column counts differ");
  if (left->columns > left->budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
    return union_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"UNION width exceeds plan capacity");
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = left->columns;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(left->budget,&amount,error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < left->columns; ++i) {
    orm_sql_type merged;
    if(!types) status = union_result_type(left->types[i],right->types[i],kind,&merged,error);
    else {
      merged=types[i];
      const orm_sql_type inputs[]={left->types[i],right->types[i]};
      for(size_t side=0;status==TURBODB_STATUS_OK && side<ORM_SQL_UNION_INPUTS;++side) {
        orm_sql_type widened;
        status=orm_tidesdb_sql_union_type(inputs[side],merged,&widened,error);
        if(status==TURBODB_STATUS_OK && widened.kind!=merged.kind)
          status=union_error(error,TURBODB_STATUS_UNSUPPORTED,"set result type narrows an input kind");
      }
      if(status==TURBODB_STATUS_OK && orm_sql_union_nullable(kind,left->types[i],right->types[i]) && !merged.nullable)
        status=union_error(error,TURBODB_STATUS_TYPE_ERROR,"set result type narrows result nullability");
    }
    if (status == TURBODB_STATUS_OK && kind != ORM_SQL_UNION_ALL && (merged.kind == TURBODB_VALUE_TEXT || merged.kind == TURBODB_VALUE_BLOB))
      status = union_error(error,TURBODB_STATUS_UNSUPPORTED,kind == ORM_SQL_UNION_DISTINCT ?
          "UNION DISTINCT requires numeric BOOL or NULL columns" : "INTERSECT and EXCEPT require numeric BOOL or NULL columns");
  }
  return status;
}
turbodb_status_t orm_tidesdb_sql_union_validate(const orm_sql_row_source *left, const orm_sql_row_source *right,
    orm_sql_union_kind kind, turbodb_error_t *error) {
  return orm_sql_union_validate_as(left,right,kind,NULL,error);
}
turbodb_status_t orm_tidesdb_sql_union_open(orm_sql_row_source *left, orm_sql_row_source *right,
    orm_sql_union_kind kind, orm_sql_union *out, turbodb_error_t *error) {
  return orm_sql_union_open_as(left,right,kind,NULL,out,error);
}
turbodb_status_t orm_sql_union_open_as(orm_sql_row_source *left, orm_sql_row_source *right,
    orm_sql_union_kind kind, const orm_sql_type *types, orm_sql_union *out, turbodb_error_t *error) {
  if (!out || out->budget) return union_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"empty UNION output required");
  turbodb_status_t status = orm_sql_union_validate_as(left,right,kind,types,error);
  if (status != TURBODB_STATUS_OK) return status;
  const size_t width = left->columns;
  const bool set=kind>=ORM_SQL_INTERSECT_ALL;
  bool promote=false,nullable_input=false;
  for(size_t i=0;status==TURBODB_STATUS_OK && i<width;++i) {
    const orm_sql_type a=left->types[i],b=right->types[i];
    orm_sql_type target={0};
    if(types) target=types[i]; else status=union_result_type(a,b,kind,&target,error);
    promote=promote || (target.kind==TURBODB_VALUE_DOUBLE &&
        (a.kind==TURBODB_VALUE_INT64 || a.kind==TURBODB_VALUE_UINT64 || b.kind==TURBODB_VALUE_INT64 || b.kind==TURBODB_VALUE_UINT64));
    nullable_input=nullable_input || (!target.nullable && (a.nullable || b.nullable));
  }
  if(status!=TURBODB_STATUS_OK) return status;
  const bool coercions=promote && nullable_input;
  if(coercions && width>SIZE_MAX/UNION_TYPE_ARRAYS)
    return union_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"set conversion type count overflow");
  *out = (orm_sql_union){.budget=left->budget,.kind=kind};
  vec_t projection = {0},orders={0}; size_t bytes = 0,order_bytes=0;
  status = orm_tidesdb_sql_budget_reserve_capacity(out->budget,1,sizeof(*out),0,&out->metadata_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&out->types,coercions?width*UNION_TYPE_ARRAYS:width,sizeof(orm_sql_type),
      _Alignof(orm_sql_type),out->budget,&out->type_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&projection,width,sizeof(size_t),_Alignof(size_t),out->budget,&bytes,error);
  if(status==TURBODB_STATUS_OK && set) status=orm_sql_work_zero(&orders,width,sizeof(orm_sql_scan_order),
      _Alignof(orm_sql_scan_order),out->budget,&order_bytes,error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < width; ++i) {
    if(types) *(orm_sql_type *)vec_at(&out->types,i)=types[i];
    else status = union_result_type(left->types[i],right->types[i],kind,vec_at(&out->types,i),error);
    if(status==TURBODB_STATUS_OK && coercions) {
      orm_sql_type input_type=*(const orm_sql_type *)vec_at_const(&out->types,i);
      input_type.nullable=input_type.nullable || left->types[i].nullable || right->types[i].nullable;
      *(orm_sql_type *)vec_at(&out->types,width+i)=input_type;
    }
    *(size_t *)vec_at(&projection,i) = i;
    if(set) *(orm_sql_scan_order *)vec_at(&orders,i)=(orm_sql_scan_order){.slot=i};
  }
  orm_sql_scan_spec spec = {.projection=vec_data_const(&projection),.projection_count=width,.limit=UINT64_MAX,
      .orders=vec_data_const(&orders),.order_count=set?width:0,
      .distinct=kind==ORM_SQL_INTERSECT_DISTINCT || kind==ORM_SQL_EXCEPT_DISTINCT};
  orm_sql_row_source *inputs[]={left,right};
  for(size_t side=0;status==TURBODB_STATUS_OK && side<ORM_SQL_UNION_INPUTS;++side) {
    bool input_promote=false;
    for(size_t i=0;i<width;++i) {
      const orm_sql_type target=*(const orm_sql_type *)vec_at_const(&out->types,i);
      const turbodb_value_kind_t original=inputs[side]->types[i].kind;
      input_promote=input_promote || (target.kind==TURBODB_VALUE_DOUBLE && (original==TURBODB_VALUE_INT64 || original==TURBODB_VALUE_UINT64));
    }
    spec.coerce_types=input_promote ? (const orm_sql_type *)vec_data_const(&out->types)+(coercions?width:0) : NULL;
    status=orm_tidesdb_sql_scan_open_source(inputs[side],&spec,out->budget,&out->inputs[side],error);
  }
  if (status == TURBODB_STATUS_OK) {
    out->concat = (orm_sql_row_source){out->budget,vec_data_const(&out->types),width,out,set?union_set_pull:union_concat,false};
    spec.distinct = kind == ORM_SQL_UNION_DISTINCT;
    spec.orders=NULL; spec.order_count=0;
    spec.coerce_types=NULL;
    status = orm_tidesdb_sql_scan_open_source(&out->concat,&spec,out->budget,&out->output,error);
  }
  const turbodb_status_t released = orm_sql_work_release(&projection,bytes,out->budget,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  const turbodb_status_t order_released=orm_sql_work_release(&orders,order_bytes,out->budget,status==TURBODB_STATUS_OK?error:NULL);
  if(status==TURBODB_STATUS_OK) status=order_released;
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t cleanup = orm_tidesdb_sql_union_close(out,NULL);
    return cleanup == TURBODB_STATUS_OK ? status : cleanup;
  }
  out->source = (orm_sql_row_source){out->budget,vec_data_const(&out->types),width,out,union_pull,false};
  return TURBODB_STATUS_OK;
}
orm_sql_row_source *orm_tidesdb_sql_union_source(orm_sql_union *run) { return run && run->budget ? &run->source : NULL; }
turbodb_status_t orm_tidesdb_sql_union_next(orm_sql_union *run, orm_sql_scan_row *out, turbodb_error_t *error) {
  if (!run || !run->budget) return union_error(error,TURBODB_STATUS_INVALID_STATE,"UNION run is closed");
  if (run->source.active) return union_error(error,TURBODB_STATUS_BUSY,union_busy);
  return orm_tidesdb_sql_scan_next(&run->output,out,error);
}
turbodb_status_t orm_tidesdb_sql_union_cancel(orm_sql_union *run, turbodb_error_t *error) {
  if (!run || !run->budget) return union_error(error,TURBODB_STATUS_INVALID_STATE,"UNION run is closed");
  if (run->source.active) return union_error(error,TURBODB_STATUS_BUSY,union_busy);
  return orm_tidesdb_sql_scan_cancel(&run->output,error);
}
