#include "rows.h"
#include "work.h"
#include "error.h"
#include <string.h>

enum { ROWS_INITIAL_CAPACITY = 8 };
static turbodb_status_t rows_error(turbodb_error_t *error, turbodb_status_t status, const char *message) {
  tdsql_error_set(error,status,message); return status;
}
static turbodb_status_t rows_slot(orm_sql_rows *run, orm_sql_snapshot **out, turbodb_error_t *error) {
  const size_t count = vec_size(&run->snapshots), capacity = vec_capacity(&run->snapshots);
  if (count == capacity) {
    const uint64_t configured = run->budget->limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
    const size_t limit = configured > SIZE_MAX ? SIZE_MAX : (size_t)configured;
    if (count >= limit) return rows_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL rows row capacity exceeded");
    const size_t next = capacity ? (capacity > limit / 2 ? limit : capacity * 2) :
        (limit < ROWS_INITIAL_CAPACITY ? limit : ROWS_INITIAL_CAPACITY);
    orm_sql_budget_amount steps = {0}; steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = next;
    turbodb_status_t status = orm_tidesdb_sql_budget_reserve(run->budget, &steps, error);
    vec_t grown = {0}; size_t bytes = 0;
    if (status == TURBODB_STATUS_OK) status = orm_sql_work_allocate(&grown, next, sizeof(orm_sql_snapshot),
        _Alignof(orm_sql_snapshot), 0, run->budget, &bytes, error);
    if (status == TURBODB_STATUS_OK && vec_resize(&grown, count) != STL_OK)
      status = rows_error(error, TURBODB_STATUS_OUT_OF_MEMORY, "SQL rows registry resize failed");
    if (status != TURBODB_STATUS_OK) {
      const turbodb_status_t released = orm_sql_work_release(&grown, bytes, run->budget, NULL);
      return released == TURBODB_STATUS_OK ? status : released;
    }
    if (count) memcpy(vec_data(&grown), vec_data_const(&run->snapshots), count * sizeof(orm_sql_snapshot));
    status = orm_sql_work_release(&run->snapshots, run->snapshot_bytes, run->budget, error);
    run->snapshots = grown; run->snapshot_bytes = bytes;
    if (status != TURBODB_STATUS_OK) return status;
  }
  if (vec_resize(&run->snapshots, count + 1) != STL_OK)
    return rows_error(error, TURBODB_STATUS_OUT_OF_MEMORY, "SQL rows registry append failed");
  *out = vec_at(&run->snapshots, count);
  **out = (orm_sql_snapshot){0};
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_snapshot_copy(orm_sql_snapshot *snapshot, const turbodb_value_t *row,
    size_t columns, size_t extra, orm_tidesdb_sql_budget *budget, turbodb_error_t *error) {
  if (!snapshot || snapshot->values.initialized || snapshot->bytes || !row || !columns || !budget)
    return rows_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL snapshot copy");
  if (columns > SIZE_MAX/sizeof(turbodb_value_t) || extra > SIZE_MAX/sizeof(turbodb_value_t)-columns)
    return rows_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"SQL snapshot width overflow");
  size_t payload = 0;
  for (size_t i = 0; i < columns; ++i) {
    const size_t size = row[i].kind == TURBODB_VALUE_TEXT ? row[i].data.text_value.len :
        row[i].kind == TURBODB_VALUE_BLOB ? row[i].data.blob_value.size : 0;
    const void *data = row[i].kind == TURBODB_VALUE_TEXT ? row[i].data.text_value.data :
        row[i].kind == TURBODB_VALUE_BLOB ? row[i].data.blob_value.data : NULL;
    if (size && !data) return rows_error(error, TURBODB_STATUS_TYPE_ERROR, "SQL rows source payload is missing");
    if (size > SIZE_MAX - payload) return rows_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL rows payload overflow");
    payload += size;
  }
  const size_t cells = payload / sizeof(turbodb_value_t) + (payload % sizeof(turbodb_value_t) != 0);
  if (extra > SIZE_MAX - columns ||
      cells > SIZE_MAX - columns - extra || payload > UINT64_MAX - columns - extra)
    return rows_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL rows snapshot overflow");
  orm_sql_budget_amount charge = {0};
  charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = (uint64_t)payload + columns + extra;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(budget, &charge, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&snapshot->values,columns+extra+cells,
      sizeof(turbodb_value_t),_Alignof(turbodb_value_t),budget,&snapshot->bytes,error);
  if (status != TURBODB_STATUS_OK) return status;
  turbodb_value_t *copy = vec_data(&snapshot->values);
  memcpy(copy, row, columns * sizeof(*copy));
  unsigned char *bytes = (unsigned char *)(copy + columns + extra);
  for (size_t i = 0; i < columns; ++i) {
    const bool text = copy[i].kind == TURBODB_VALUE_TEXT;
    if (!text && copy[i].kind != TURBODB_VALUE_BLOB) continue;
    const size_t size = text ? copy[i].data.text_value.len : copy[i].data.blob_value.size;
    const void *data = text ? (const void *)copy[i].data.text_value.data : copy[i].data.blob_value.data;
    if (size) memcpy(bytes, data, size);
    if (text) copy[i].data.text_value.data = (const char *)bytes;
    else copy[i].data.blob_value.data = bytes;
    bytes += size;
  }
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_rows_append(orm_sql_rows *run, const turbodb_value_t *row,
    size_t columns, size_t extra, turbodb_value_t **out, turbodb_error_t *error) {
  if (!run || !run->budget || !row || !columns || !out)
    return rows_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL rows append");
  if (run->failed) return rows_error(error,TURBODB_STATUS_INVALID_STATE,"SQL rows requires cleanup after failure");
  orm_sql_budget_amount charge = {0}; charge.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS] = 1;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(run->budget,&charge,error);
  orm_sql_snapshot *snapshot = NULL;
  if (status == TURBODB_STATUS_OK) {
    ++run->materialized;
    status = rows_slot(run,&snapshot,error);
  }
  if (status == TURBODB_STATUS_OK) status = orm_sql_snapshot_copy(snapshot,row,columns,extra,run->budget,error);
  if (status == TURBODB_STATUS_OK) *out = vec_data(&snapshot->values);
  if (status != TURBODB_STATUS_OK) run->failed = true;
  return status;
}
const turbodb_value_t *orm_sql_rows_at(const orm_sql_rows *run, size_t index) {
  if (!run || run->failed || index >= vec_size(&run->snapshots)) return NULL;
  const orm_sql_snapshot *snapshot = vec_at_const(&run->snapshots,index);
  return vec_data_const(&snapshot->values);
}
turbodb_status_t orm_sql_rows_close(orm_sql_rows *run, turbodb_error_t *error) {
  if (!run || !run->budget) return TURBODB_STATUS_OK;
  turbodb_status_t status = TURBODB_STATUS_OK;
  for (size_t i = 0; i < vec_size(&run->snapshots); ++i) {
    orm_sql_snapshot *snapshot = vec_at(&run->snapshots,i);
    const turbodb_status_t released = orm_sql_work_release(&snapshot->values,snapshot->bytes,run->budget,
        status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  const turbodb_status_t released = orm_sql_work_release(&run->snapshots,run->snapshot_bytes,run->budget,
      status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  if (run->materialized) {
    const turbodb_status_t refunded = orm_tidesdb_sql_budget_release(run->budget,ORM_SQL_BUDGET_MATERIALIZED_ROWS,
        run->materialized,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = refunded;
  }
  *run = (orm_sql_rows){0}; return status;
}
