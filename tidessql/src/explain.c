#include "explain.h"
#include "name.h"
#include "work.h"
#include "error.h"
#include <stdio.h>
#include <string.h>

enum { EXPLAIN_ID, EXPLAIN_SELECT_TYPE, EXPLAIN_TABLE, EXPLAIN_PARTITIONS, EXPLAIN_TYPE,
  EXPLAIN_POSSIBLE_KEYS, EXPLAIN_KEY, EXPLAIN_KEY_LEN, EXPLAIN_REF, EXPLAIN_ROWS, EXPLAIN_FILTERED, EXPLAIN_EXTRA };
typedef struct explain_detail {
  char table[ORM_SQL_SELECT_NAME_BYTES+1], extra[ORM_SQL_EXPLAIN_EXTRA_BYTES];
} explain_detail;
typedef struct explain_visit { size_t node; unsigned flags; } explain_visit;
enum { EXPLAIN_INNER = 1, EXPLAIN_LEFT = 2, EXPLAIN_CROSS = 4, EXPLAIN_RIGHT = 8, EXPLAIN_MATERIALIZED = 16 };
enum { EXPLAIN_DELIMITER_BYTES = sizeof("; ")-1 };
static turbodb_status_t explain_error(turbodb_error_t *error, turbodb_status_t status, const char *reason) {
  tdsql_error_set(error,status,reason); return status;
}
static turbodb_status_t explain_next(void *context, const turbodb_value_t **out, turbodb_error_t *error) {
  orm_sql_explain_source *source = context;
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
  const turbodb_status_t status = orm_tidesdb_sql_budget_reserve(source->source.budget,&amount,error);
  if (status != TURBODB_STATUS_OK) return status;
  if (vec_size(&source->details)) {
    if (source->position == vec_size(&source->details)) { *out = NULL; return TURBODB_STATUS_OK; }
    const explain_detail *detail = vec_at_const(&source->details,source->position++);
    source->values[EXPLAIN_TABLE] = turbodb_text(detail->table);
    source->values[EXPLAIN_EXTRA] = detail->extra[0] ? turbodb_text(detail->extra) : turbodb_null();
    *out = source->values; return TURBODB_STATUS_OK;
  }
  *out = source->done ? NULL : source->values; source->done = true; return TURBODB_STATUS_OK;
}
turbodb_status_t orm_tidesdb_sql_explain_open(const orm_sql_select *plan, vstr table,
    const turbodb_value_t *parameters, size_t count, orm_sql_explain_source *out, turbodb_error_t *error) {
  if (!plan || !plan->budget || !out || out->source.budget)
    return explain_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"empty EXPLAIN source and bound SELECT required");
  const char *reason = NULL;
  turbodb_status_t status = table.len ? orm_sql_name_validate(table,&reason) : TURBODB_STATUS_OK;
  if (status != TURBODB_STATUS_OK) return explain_error(error,status,reason);
  uint64_t offset = 0, limit = 0;
  status = orm_tidesdb_sql_select_validate_parameters(plan,parameters,count,&offset,&limit,error);
  if (status != TURBODB_STATUS_OK) return status;
  size_t bytes = 0;
  status = orm_tidesdb_sql_budget_reserve_capacity(plan->budget,1,sizeof(*out),0,&bytes,error);
  if (status != TURBODB_STATUS_OK) return status;
  const bool keyed = vec_size(&plan->group_keys) != 0;
  const bool windows = vec_size(&plan->windows) != 0;
  const bool sort = keyed || plan->distinct || vec_size(&plan->orders) || vec_size(&plan->window_orders);
  const bool where = plan->grouped ? plan->pre_filter.budget != NULL : plan->filter.budget != NULL;
  const bool having = plan->grouped && plan->filter.budget;
  const int length = snprintf(out->extra,sizeof(out->extra),"%s%s%s%s%s%s%s%s%s%s",
      !table.len ? "No tables used; " : "",
      !limit ? "Zero limit; " : "",
      where ? "Using where; " : "", sort ? "Using temporary; Using filesort; " : "",
      plan->grouped ? (keyed ? "Group aggregate; " : "Global aggregate; ") : "",
      windows ? "Window; " : "",
      having ? "Using having; " : "", plan->distinct ? "Distinct; " : "",
      limit != UINT64_MAX && limit ? "Limit; " : "", offset ? "Offset; " : "");
  if (length < 0 || (size_t)length >= sizeof(out->extra)) status = TURBODB_STATUS_LIMIT_EXCEEDED;
  orm_sql_budget_amount amount = {0};
  if (status == TURBODB_STATUS_OK) {
    amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = ORM_SQL_EXPLAIN_COLUMNS + table.len + (size_t)length;
    status = orm_tidesdb_sql_budget_reserve(plan->budget,&amount,error);
  }
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t released = orm_tidesdb_sql_budget_release(plan->budget,ORM_SQL_BUDGET_WORK_BYTES,bytes,NULL);
    *out = (orm_sql_explain_source){0};
    return explain_error(error,released == TURBODB_STATUS_OK ? status : released,"EXPLAIN formatting capacity or step budget exceeded");
  }
  if (length) out->extra[length-EXPLAIN_DELIMITER_BYTES] = '\0';
  if (table.len) memcpy(out->table,table.data,table.len);
  out->table[table.len] = '\0';
  static const char *const names[ORM_SQL_EXPLAIN_COLUMNS] = {
    "id","select_type","table","partitions","type","possible_keys","key","key_len","ref","rows","filtered","Extra"};
  for (size_t i = 0; i < ORM_SQL_EXPLAIN_COLUMNS; ++i) {
    out->types[i] = (orm_sql_type){i == EXPLAIN_ID || i == EXPLAIN_ROWS ? TURBODB_VALUE_INT64 :
        i == EXPLAIN_FILTERED ? TURBODB_VALUE_DOUBLE : TURBODB_VALUE_TEXT,i >= EXPLAIN_TABLE};
    out->columns[i] = (orm_sql_schema_column){vstr_from_cstr(names[i]),out->types[i]}; out->values[i] = turbodb_null();
  }
  out->values[EXPLAIN_ID] = turbodb_i64(1); out->values[EXPLAIN_SELECT_TYPE] = turbodb_text("SIMPLE");
  out->values[EXPLAIN_TABLE] = table.len ? turbodb_text(out->table) : turbodb_null();
  out->values[EXPLAIN_TYPE] = limit && table.len ? turbodb_text("ALL") : turbodb_null();
  if (!limit) out->values[EXPLAIN_ROWS] = turbodb_i64(0);
  if (length) out->values[EXPLAIN_EXTRA] = turbodb_text(out->extra);
  out->metadata_bytes = bytes;
  out->source = (orm_sql_row_source){plan->budget,out->types,ORM_SQL_EXPLAIN_COLUMNS,out,explain_next,false};
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_explain_index(orm_sql_explain_source *source, const orm_sql_index_definition *index,
    size_t tuple_bytes, orm_sql_index_access access, bool contains_null, turbodb_error_t *error) {
  if (!source || !source->source.budget || source->source.active || source->done || vec_size(&source->details) ||
      !index || index->budget != source->source.budget)
    return explain_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"index EXPLAIN annotation requires an unattached source");
  if (source->values[EXPLAIN_TYPE].kind == TURBODB_VALUE_NULL) return TURBODB_STATUS_OK;
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = index->name_size;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(source->source.budget,&amount,error);
  if (status != TURBODB_STATUS_OK) return status;
  const int length = snprintf(source->key_length,sizeof(source->key_length),"%zu",tuple_bytes);
  if (length < 0 || (size_t)length >= sizeof(source->key_length))
    return explain_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"index EXPLAIN length overflow");
  memcpy(source->index,index->name,index->name_size+1);
  source->values[EXPLAIN_POSSIBLE_KEYS] = source->values[EXPLAIN_KEY] = turbodb_text(source->index);
  source->values[EXPLAIN_KEY_LEN] = turbodb_text(source->key_length);
  source->values[EXPLAIN_REF] = access == ORM_SQL_INDEX_RANGE ? turbodb_null() : turbodb_text("const");
  source->values[EXPLAIN_TYPE] = turbodb_text(access == ORM_SQL_INDEX_RANGE ? "range" :
      access == ORM_SQL_INDEX_EQUAL && index->unique && !contains_null ? "const" : "ref");
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_tidesdb_sql_explain_open_from(const orm_sql_select *plan, const orm_sql_from *from,
    const turbodb_value_t *parameters, size_t count, orm_sql_explain_source *out, turbodb_error_t *error) {
  if (!plan || !from || !from->budget || from->budget != plan->budget || !from->tables)
    return explain_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"EXPLAIN requires matching SELECT and FROM plans");
  const orm_sql_from_node *first = NULL;
  for (size_t i = 0; i < from->count && !first; ++i) {
    const orm_sql_from_node *node = orm_tidesdb_sql_from_at(from,i);
    if (node->leaf) first = node;
  }
  if (!first) return explain_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"EXPLAIN FROM has no table");
  turbodb_status_t status = orm_tidesdb_sql_explain_open(plan,vstr_from_cstr(first->qualifier),parameters,count,out,error);
  if (status != TURBODB_STATUS_OK) return status;
  vec_t stack = {0}; size_t bytes = 0, pending = 0, rows = 0;
  status = orm_sql_work_zero(&out->details,from->tables,sizeof(explain_detail),_Alignof(explain_detail),
      from->budget,&out->detail_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&stack,from->count,sizeof(explain_visit),
      _Alignof(explain_visit),from->budget,&bytes,error);
  if (status == TURBODB_STATUS_OK) *(explain_visit *)vec_at(&stack,pending++) = (explain_visit){0,0};
  while (status == TURBODB_STATUS_OK && pending) {
    const explain_visit visit = *(const explain_visit *)vec_at_const(&stack,--pending);
    const orm_sql_from_node *node = orm_tidesdb_sql_from_at(from,visit.node);
    orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
    status = orm_tidesdb_sql_budget_reserve(from->budget,&amount,error);
    if (status != TURBODB_STATUS_OK) break;
    if (!node->leaf) {
      unsigned flags = visit.flags | (node->reversed ? EXPLAIN_RIGHT :
          node->kind == ORM_SQL_JOIN_LEFT ? EXPLAIN_LEFT : node->kind == ORM_SQL_JOIN_INNER ? EXPLAIN_INNER : EXPLAIN_CROSS);
      *(explain_visit *)vec_at(&stack,pending++) = (explain_visit){node->reversed ? node->left : node->right,flags|EXPLAIN_MATERIALIZED};
      *(explain_visit *)vec_at(&stack,pending++) = (explain_visit){node->reversed ? node->right : node->left,flags};
      continue;
    }
    explain_detail *detail = vec_at(&out->details,rows);
    memcpy(detail->table,node->qualifier,sizeof(detail->table));
    const unsigned flags = visit.flags;
    const int length = snprintf(detail->extra,sizeof(detail->extra),"%s%s%s%s%s%s%s",
        !rows ? out->extra : "", !rows && out->extra[0] && flags ? "; " : "",
        flags & EXPLAIN_INNER ? "Nested loop INNER JOIN; " : "",
        flags & EXPLAIN_LEFT ? "Nested loop LEFT JOIN; " : "",
        flags & EXPLAIN_CROSS ? "Nested loop CROSS JOIN; " : "",
        flags & EXPLAIN_RIGHT ? "RIGHT JOIN via LEFT; " : "",
        flags & EXPLAIN_MATERIALIZED ? "Materialized input; " : "");
    if (length < 0 || (size_t)length >= sizeof(detail->extra)) {
      status = explain_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"EXPLAIN join description exceeds capacity"); break;
    }
    if (flags) detail->extra[length-EXPLAIN_DELIMITER_BYTES] = 0;
    amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = strlen(detail->table)+(size_t)length;
    status = orm_tidesdb_sql_budget_reserve(from->budget,&amount,error); ++rows;
  }
  const turbodb_status_t released = orm_sql_work_release(&stack,bytes,from->budget,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t cleanup = orm_tidesdb_sql_explain_close(out,NULL);
    if (cleanup != TURBODB_STATUS_OK) status = cleanup;
  }
  return status;
}
turbodb_status_t orm_tidesdb_sql_explain_block(orm_sql_explain_source *source,
    orm_sql_explain_block kind, int64_t id, turbodb_error_t *error) {
  if (!source || !source->source.budget || source->done || source->position ||
      kind < ORM_SQL_EXPLAIN_PRIMARY || kind > ORM_SQL_EXPLAIN_EXCEPT_BRANCH || id <= 0)
    return explain_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid EXPLAIN block annotation");
  const bool result = kind >= ORM_SQL_EXPLAIN_UNION_ALL && kind <= ORM_SQL_EXPLAIN_EXCEPT_DISTINCT;
  if (result && (source->source.active || vec_size(&source->details)))
    return explain_error(error,TURBODB_STATUS_INVALID_STATE,"EXPLAIN result annotation requires unattached single row");
  const bool recursive = kind == ORM_SQL_EXPLAIN_RECURSIVE_BRANCH && !source->recursive;
  if (result || recursive) {
    char *extra = recursive && vec_size(&source->details) ?
        ((explain_detail *)vec_at(&source->details,0))->extra : source->extra;
    const char *operation = recursive ? "Recursive" : kind == ORM_SQL_EXPLAIN_UNION_ALL ? "UNION ALL" :
        kind == ORM_SQL_EXPLAIN_UNION_DISTINCT ? "UNION DISTINCT; Using temporary; Using filesort" :
        kind == ORM_SQL_EXPLAIN_INTERSECT_ALL ? "INTERSECT ALL; Using temporary; Using filesort" :
        kind == ORM_SQL_EXPLAIN_INTERSECT_DISTINCT ? "INTERSECT DISTINCT; Using temporary; Using filesort" :
        kind == ORM_SQL_EXPLAIN_EXCEPT_ALL ? "EXCEPT ALL; Using temporary; Using filesort" :
        kind == ORM_SQL_EXPLAIN_EXCEPT_DISTINCT ? "EXCEPT DISTINCT; Using temporary; Using filesort" : "Query group";
    const size_t used = strlen(extra), added = strlen(operation);
    const size_t separator = used ? EXPLAIN_DELIMITER_BYTES : 0;
    if (added + separator >= sizeof(source->extra) - used)
      return explain_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"EXPLAIN compound description exceeds capacity");
    orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = added + separator;
    const turbodb_status_t status = orm_tidesdb_sql_budget_reserve(source->source.budget,&amount,error);
    if (status != TURBODB_STATUS_OK) return status;
    if (separator) memcpy(extra+used,"; ",separator);
    memcpy(extra+used+separator,operation,added+1);
    source->values[EXPLAIN_EXTRA] = turbodb_text(extra);
    if (recursive) source->recursive = true;
    if (result) source->types[EXPLAIN_ID].nullable = source->columns[EXPLAIN_ID].type.nullable = true;
  }
  source->values[EXPLAIN_ID] = result ? turbodb_null() : turbodb_i64(id);
  source->values[EXPLAIN_SELECT_TYPE] = turbodb_text(result ?
      (kind == ORM_SQL_EXPLAIN_QUERY_GROUP ? "QUERY GROUP" :
       kind == ORM_SQL_EXPLAIN_INTERSECT_ALL || kind == ORM_SQL_EXPLAIN_INTERSECT_DISTINCT ? "INTERSECT RESULT" :
       kind == ORM_SQL_EXPLAIN_EXCEPT_ALL || kind == ORM_SQL_EXPLAIN_EXCEPT_DISTINCT ? "EXCEPT RESULT" : "UNION RESULT") :
      kind == ORM_SQL_EXPLAIN_PRIMARY ? "PRIMARY" : kind == ORM_SQL_EXPLAIN_SUBQUERY ? "SUBQUERY" :
      kind == ORM_SQL_EXPLAIN_DERIVED ? "DERIVED" :
      kind == ORM_SQL_EXPLAIN_INTERSECT_BRANCH ? "INTERSECT" : kind == ORM_SQL_EXPLAIN_EXCEPT_BRANCH ? "EXCEPT" : "UNION");
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_tidesdb_sql_explain_close(orm_sql_explain_source *source, turbodb_error_t *error) {
  if (!source || !source->source.budget) return TURBODB_STATUS_OK;
  if (source->source.active) return explain_error(error,TURBODB_STATUS_BUSY,"EXPLAIN source has an active scan");
  turbodb_status_t status = orm_sql_work_release(&source->details,source->detail_bytes,source->source.budget,error);
  const turbodb_status_t released = orm_tidesdb_sql_budget_release(source->source.budget,ORM_SQL_BUDGET_WORK_BYTES,
      source->metadata_bytes,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  *source = (orm_sql_explain_source){0}; return status;
}
