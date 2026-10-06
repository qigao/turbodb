#include "subquery.h"
#include "work.h"
#include "error.h"

static turbodb_status_t subquery_eval(void *context, const turbodb_value_t *probe,
    const orm_sql_predicate *comparison, const turbodb_value_t *outer_row,
    size_t outer_count, turbodb_value_t *out, turbodb_error_t *error);

enum { SUBQUERY_SCALAR_ROWS = 2, SUBQUERY_EXISTS_ROWS = 1 };
static turbodb_status_t subquery_error(turbodb_error_t *error, turbodb_status_t status, const char *reason) {
  tdsql_error_set(error,status,reason); return status;
}
static bool subquery_membership(orm_sql_subquery_kind kind) {
  return kind == ORM_SQL_SUBQUERY_IN || kind == ORM_SQL_SUBQUERY_NOT_IN;
}
static turbodb_status_t subquery_fail(orm_sql_subquery *run, turbodb_status_t status,
    const turbodb_error_t *cause, turbodb_error_t *error) {
  run->state = ORM_SQL_SUBQUERY_FAILED;
  tdsql_error_set(&run->failure,status,cause->message);
  tdsql_error_set(error,status,run->failure.message); return status;
}
turbodb_status_t orm_tidesdb_sql_subquery_close(orm_sql_subquery *run, turbodb_error_t *error) {
  if (!run || !run->budget) return TURBODB_STATUS_OK;
  if (run->source.active_runs || run->evaluating) return subquery_error(error,TURBODB_STATUS_BUSY,"SQL subquery is in use");
  turbodb_status_t status = orm_tidesdb_sql_scan_close(&run->input,error);
  turbodb_status_t released = orm_sql_rows_close(&run->cache,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  if (run->metadata_bytes) {
    released = orm_tidesdb_sql_budget_release(run->budget,ORM_SQL_BUDGET_WORK_BYTES,
        run->metadata_bytes,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  *run = (orm_sql_subquery){0}; return status;
}
static turbodb_status_t subquery_open(orm_sql_row_source *source,
    orm_sql_subquery_kind kind, const orm_sql_type *probe_type,
    orm_sql_subquery *out, bool compiled, turbodb_error_t *error) {
  const bool membership = subquery_membership(kind);
  if (!source || !source->budget || !source->types || !source->columns || !source->next ||
      !out || out->budget || kind < ORM_SQL_SUBQUERY_SCALAR || kind > ORM_SQL_SUBQUERY_NOT_IN ||
      (compiled ? (!membership || probe_type != NULL) : membership != (probe_type != NULL)))
    return subquery_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL subquery inputs");
  if (source->active) return subquery_error(error,TURBODB_STATUS_BUSY,"SQL subquery input already active");
  if ((kind == ORM_SQL_SUBQUERY_SCALAR || membership) && source->columns != 1)
    return subquery_error(error,TURBODB_STATUS_SQL_ERROR,"SQL scalar and IN subqueries require one column");
  orm_sql_predicate equality = {0};
  turbodb_status_t status = TURBODB_STATUS_OK;
  if (membership && probe_type) {
    status = orm_tidesdb_sql_predicate_bind(ORM_SQL_EQUAL,*probe_type,source->types,&equality,error);
  }
  if (status != TURBODB_STATUS_OK) return status;
  *out = (orm_sql_subquery){.budget=source->budget,.kind=kind,.state=ORM_SQL_SUBQUERY_PENDING,
      .equality=equality,.fixed_probe=probe_type!=NULL,.cache={.budget=source->budget},.scalar=turbodb_null()};
  out->result = membership ? (probe_type ? equality.result : (orm_sql_type){TURBODB_VALUE_BOOLEAN,true}) : kind == ORM_SQL_SUBQUERY_SCALAR ?
      (orm_sql_type){source->types[0].kind,true} : (orm_sql_type){TURBODB_VALUE_BOOLEAN,false};
  out->source = (orm_sql_expr_query_source){out->budget,{kind,membership ? (orm_sql_type){TURBODB_VALUE_BOOLEAN,true} : out->result,
      membership ? source->types[0] : (orm_sql_type){TURBODB_VALUE_NULL,true}},out,subquery_eval,0};
  tdsql_error_init(&out->failure);
  status = orm_tidesdb_sql_budget_reserve_capacity(out->budget,1,sizeof(*out),0,&out->metadata_bytes,error);
  const size_t projection = 0;
  const orm_sql_scan_spec spec = {.projection=&projection,.projection_count=1,
      .limit=membership ? UINT64_MAX : kind == ORM_SQL_SUBQUERY_SCALAR ? SUBQUERY_SCALAR_ROWS : SUBQUERY_EXISTS_ROWS};
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_scan_open_source(source,&spec,out->budget,&out->input,error);
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t released = orm_tidesdb_sql_subquery_close(out,NULL);
    return released == TURBODB_STATUS_OK ? status : released;
  }
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_tidesdb_sql_subquery_open(orm_sql_row_source *source,
    orm_sql_subquery_kind kind, const orm_sql_type *probe_type,
    orm_sql_subquery *out, turbodb_error_t *error) {
  return subquery_open(source,kind,probe_type,out,false,error);
}
turbodb_status_t orm_sql_subquery_open_set(orm_sql_row_source *source,
    orm_sql_subquery_kind kind, orm_sql_subquery *out, turbodb_error_t *error) {
  return subquery_open(source,kind,NULL,out,true,error);
}
static turbodb_status_t subquery_prepare(orm_sql_subquery *run, turbodb_error_t *error) {
  const bool membership = subquery_membership(run->kind);
  for (;;) {
    orm_sql_scan_row row;
    turbodb_status_t status = orm_tidesdb_sql_scan_next(&run->input,&row,error);
    if (status != TURBODB_STATUS_OK) return status;
    if (row.state == ORM_SQL_SCAN_CANCELLED)
      return subquery_error(error,TURBODB_STATUS_INVALID_STATE,"SQL subquery input cancelled");
    if (!membership && run->kind != ORM_SQL_SUBQUERY_SCALAR) {
      const bool exists = row.state == ORM_SQL_SCAN_ROW;
      run->scalar = turbodb_bool(run->kind == ORM_SQL_SUBQUERY_EXISTS ? exists : !exists); break;
    }
    if (row.state == ORM_SQL_SCAN_DONE) break;
    if (!membership && vec_size(&run->cache.snapshots))
      return subquery_error(error,TURBODB_STATUS_SQL_ERROR,"SQL scalar subquery returned more than one row");
    turbodb_value_t *copy = NULL;
    status = orm_sql_rows_append(&run->cache,row.values,1,0,&copy,error);
    if (status != TURBODB_STATUS_OK) return status;
    if (!membership) run->scalar = copy[0];
  }
  run->state = ORM_SQL_SUBQUERY_READY; return TURBODB_STATUS_OK;
}
static turbodb_status_t subquery_contains(orm_sql_subquery *run, const turbodb_value_t *probe,
    const orm_sql_predicate *comparison, turbodb_value_t *out, turbodb_error_t *error) {
  turbodb_value_t result = turbodb_bool(false);
  for (size_t i = 0; i < vec_size(&run->cache.snapshots); ++i) {
    turbodb_value_t equal;
    const turbodb_status_t status = orm_tidesdb_sql_predicate_eval(comparison,probe,
        orm_sql_rows_at(&run->cache,i),run->budget,&equal,error);
    if (status != TURBODB_STATUS_OK) return status;
    if (equal.kind == TURBODB_VALUE_BOOLEAN && equal.data.boolean_value) { result = equal; break; }
    if (equal.kind == TURBODB_VALUE_NULL) result = equal;
  }
  if (run->kind == ORM_SQL_SUBQUERY_NOT_IN && result.kind == TURBODB_VALUE_BOOLEAN)
    result.data.boolean_value = !result.data.boolean_value;
  *out = result; return TURBODB_STATUS_OK;
}
orm_sql_expr_query_source *orm_tidesdb_sql_subquery_source(orm_sql_subquery *run) {
  return run && run->budget ? &run->source : NULL;
}
turbodb_status_t orm_tidesdb_sql_subquery_eval(orm_sql_subquery *run,
    const turbodb_value_t *probe, turbodb_value_t *out, turbodb_error_t *error) {
  if (run && run->budget && run->source.active_runs)
    return subquery_error(error,TURBODB_STATUS_BUSY,"SQL subquery has expression consumers");
  if (run && run->budget && subquery_membership(run->kind) && !run->fixed_probe)
    return subquery_error(error,TURBODB_STATUS_INVALID_STATE,"IN source requires a compiled comparison");
  return subquery_eval(run,probe,run && subquery_membership(run->kind) ? &run->equality : NULL,
      NULL,0,out,error);
}
static turbodb_status_t subquery_eval(void *context,
    const turbodb_value_t *probe, const orm_sql_predicate *comparison, const turbodb_value_t *outer_row,
    size_t outer_count, turbodb_value_t *out, turbodb_error_t *error) {
  (void)outer_row; (void)outer_count;
  orm_sql_subquery *run = context;
  if (!run || !run->budget) return subquery_error(error,TURBODB_STATUS_INVALID_STATE,"SQL subquery is closed");
  if (!out || subquery_membership(run->kind) != (probe != NULL) ||
      subquery_membership(run->kind) != (comparison != NULL))
    return subquery_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL subquery evaluation arguments");
  if (comparison && (comparison->op != ORM_SQL_EQUAL ||
      comparison->right.kind != run->source.type.element.kind ||
      comparison->right.nullable != run->source.type.element.nullable))
    return subquery_error(error,TURBODB_STATUS_TYPE_ERROR,"IN comparison differs from source column");
  if (run->state == ORM_SQL_SUBQUERY_FAILED) {
    tdsql_error_set(error,run->failure.status,run->failure.message); return run->failure.status;
  }
  if (run->state == ORM_SQL_SUBQUERY_CANCELLED)
    return subquery_error(error,TURBODB_STATUS_INVALID_STATE,"SQL subquery is cancelled");
  if (run->evaluating) return subquery_error(error,TURBODB_STATUS_BUSY,"SQL subquery evaluation is not reentrant");
  run->evaluating = true;
  turbodb_error_t cause; tdsql_error_init(&cause);
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(run->budget,&amount,&cause);
  turbodb_value_t value = turbodb_null();
  if (status == TURBODB_STATUS_OK && probe) {
    orm_sql_predicate validator;
    status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,comparison->left,NULL,&validator,&cause);
    if (status == TURBODB_STATUS_OK)
      status = orm_tidesdb_sql_predicate_eval(&validator,probe,NULL,run->budget,&value,&cause);
  }
  if (status == TURBODB_STATUS_OK && run->state == ORM_SQL_SUBQUERY_PENDING) status = subquery_prepare(run,&cause);
  if (status == TURBODB_STATUS_OK) {
    if (probe) status = subquery_contains(run,probe,comparison,&value,&cause);
    else value = run->scalar;
  }
  run->evaluating = false;
  if (status != TURBODB_STATUS_OK) return subquery_fail(run,status,&cause,error);
  *out = value; return TURBODB_STATUS_OK;
}
turbodb_status_t orm_tidesdb_sql_subquery_cancel(orm_sql_subquery *run, turbodb_error_t *error) {
  if (!run || !run->budget) return subquery_error(error,TURBODB_STATUS_INVALID_STATE,"SQL subquery is closed");
  if (run->source.active_runs || run->evaluating) return subquery_error(error,TURBODB_STATUS_BUSY,"SQL subquery is in use");
  if (run->state == ORM_SQL_SUBQUERY_FAILED || run->state == ORM_SQL_SUBQUERY_CANCELLED) return TURBODB_STATUS_OK;
  const turbodb_status_t status = orm_tidesdb_sql_scan_cancel(&run->input,error);
  if (status == TURBODB_STATUS_OK) run->state = ORM_SQL_SUBQUERY_CANCELLED;
  return status;
}
