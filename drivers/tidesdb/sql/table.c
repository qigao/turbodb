#include "table.h"
#include "work.h"
#include "../row.h"
#include <stdio.h>
#include <string.h>

typedef struct table_name { char bytes[ORM_SQL_SELECT_NAME_BYTES + 1]; size_t size; } table_name;

static orm_status_t table_error(orm_error_t *error, orm_status_t status, const char *message) {
  orm_error_set(error, status, message); return status;
}
static orm_status_t table_native(orm_error_t *error, int code, const char *operation) {
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "SQL table source %s: native error %d", operation, code);
  const orm_status_t status = code == ORM_TDB_ERR_MEMORY ? ORM_STATUS_OUT_OF_MEMORY :
      code == ORM_TDB_ERR_TOO_LARGE || code == ORM_TDB_ERR_MEMORY_LIMIT ? ORM_STATUS_LIMIT_EXCEEDED :
      code == ORM_TDB_ERR_CONFLICT || code == ORM_TDB_ERR_BUSY ? ORM_STATUS_BUSY : ORM_STATUS_DATASTORE_ERROR;
  return table_error(error, status, message);
}
static orm_status_t table_vector(orm_sql_table_source *table, vec_t *vector, size_t count,
    size_t size, size_t align, size_t *bytes, orm_error_t *error) {
  orm_status_t status = orm_sql_work_allocate(vector, count, size, align, 0, table->source.budget, bytes, error);
  if (status != ORM_STATUS_OK) return status;
  const stl_status resized = vec_resize(vector, count);
  if (resized != STL_OK)
    return table_error(error, resized == STL_OUT_OF_MEMORY ? ORM_STATUS_OUT_OF_MEMORY :
        resized == STL_CAPACITY_EXCEEDED ? ORM_STATUS_LIMIT_EXCEEDED : ORM_STATUS_INTERNAL_ERROR,
        "SQL table workspace allocation failed");
  memset(vec_data(vector), 0, count * size);
  return ORM_STATUS_OK;
}
static orm_status_t table_steps(orm_sql_table_source *table, size_t count, size_t cost, orm_error_t *error) {
  if (cost && count > UINT64_MAX / cost)
    return table_error(error, ORM_STATUS_LIMIT_EXCEEDED, "SQL table validation steps overflow");
  orm_sql_budget_amount charge = {0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = (uint64_t)count * cost;
  return orm_tidesdb_sql_budget_reserve(table->source.budget, &charge, error);
}
static void table_clear(orm_sql_table_source *table) {
  if (vec_size(&table->fields)) memset(vec_data(&table->fields), 0, vec_size(&table->fields) * sizeof(orm_tidesdb_field_view));
  if (vec_size(&table->values)) memset(vec_data(&table->values), 0, vec_size(&table->values) * sizeof(orm_value_t));
}

orm_status_t orm_tidesdb_sql_table_close(orm_sql_table_source *table, orm_error_t *error) {
  if (!table || !table->source.budget) return ORM_STATUS_OK;
  if (table->source.active) return table_error(error, ORM_STATUS_BUSY, "SQL table source has an active scan");
  table_clear(table);
  if (table->iterator) orm_tidesdb_iter_free(table->iterator);
  orm_status_t status = ORM_STATUS_OK;
  vec_t *vectors[] = {&table->prefix, &table->names, &table->types, &table->fields, &table->values};
  const size_t bytes[] = {table->prefix_bytes, table->name_bytes, table->type_bytes, table->field_bytes, table->value_bytes};
  for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
    const orm_status_t released = orm_sql_work_release(vectors[i], bytes[i], table->source.budget,
        status == ORM_STATUS_OK ? error : NULL);
    if (status == ORM_STATUS_OK) status = released;
  }
  if (table->metadata_bytes) {
    const orm_status_t released = orm_tidesdb_sql_budget_release(table->source.budget,
        ORM_SQL_BUDGET_WORK_BYTES, table->metadata_bytes, status == ORM_STATUS_OK ? error : NULL);
    if (status == ORM_STATUS_OK) status = released;
  }
  *table = (orm_sql_table_source){0};
  return status;
}

static orm_status_t table_decode(orm_sql_table_source *table, const uint8_t *data, size_t size, orm_error_t *error) {
  const size_t columns = table->source.columns;
  if (size > table->max_row_bytes)
    return table_error(error, ORM_STATUS_LIMIT_EXCEEDED, "SQL table row exceeds max_row_bytes");
  orm_status_t status = table_steps(table, size, columns + 1, error);
  if (status == ORM_STATUS_OK) status = table_steps(table, columns,
      columns * (ORM_SQL_SELECT_NAME_BYTES + 1), error);
  size_t fields = 0;
  if (status == ORM_STATUS_OK) status = orm_tidesdb_row_decode_view(data, size, table->max_row_bytes,
      vec_data(&table->fields), columns, &fields, error);
  if (status != ORM_STATUS_OK) return status;
  if (fields != columns) return table_error(error, ORM_STATUS_TYPE_ERROR, "SQL table row does not match declared field count");
  for (size_t i = 0; i < columns; ++i) {
    const table_name *name = vec_at_const(&table->names, i);
    const orm_sql_type *type = vec_at_const(&table->types, i);
    const orm_value_t *value = NULL;
    for (size_t j = 0; j < fields; ++j) {
      const orm_tidesdb_field_view *field = vec_at_const(&table->fields, j);
      if (name->size == field->name.len && !memcmp(name->bytes, field->name.data, name->size)) { value = &field->value; break; }
    }
    if (!value) return table_error(error, ORM_STATUS_TYPE_ERROR, "SQL table row is missing a declared column");
    orm_sql_predicate validator; orm_value_t ignored;
    status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL, *type, NULL, &validator, error);
    if (status == ORM_STATUS_OK)
      status = orm_tidesdb_sql_predicate_eval(&validator, value, NULL, table->source.budget, &ignored, error);
    if (status != ORM_STATUS_OK) return status;
    *(orm_value_t *)vec_at(&table->values, i) = *value;
  }
  return ORM_STATUS_OK;
}

static orm_status_t table_read(orm_sql_table_source *table, const orm_value_t **out, orm_error_t *error) {
  int code;
  if (!table->iterator) {
    code = orm_tidesdb_iter_new(table->transaction, table->family, &table->iterator);
    if (code != ORM_TDB_SUCCESS) return table_native(error, code, "open iterator");
    code = orm_tidesdb_iter_seek(table->iterator, vec_data_const(&table->prefix), vec_size(&table->prefix));
  } else if (table->advance) code = orm_tidesdb_iter_next(table->iterator);
  else code = ORM_TDB_SUCCESS;
  if (code == ORM_TDB_ERR_NOT_FOUND) { table->done = true; *out = NULL; return ORM_STATUS_OK; }
  if (code != ORM_TDB_SUCCESS) return table_native(error, code, "advance iterator");
  if (!orm_tidesdb_iter_valid(table->iterator)) { table->done = true; *out = NULL; return ORM_STATUS_OK; }
  uint8_t *key = NULL, *data = NULL; size_t key_size = 0, size = 0;
  code = orm_tidesdb_iter_key(table->iterator, &key, &key_size);
  if (code != ORM_TDB_SUCCESS) return table_native(error, code, "read key");
  if (!key || key_size < vec_size(&table->prefix) || memcmp(key, vec_data_const(&table->prefix), vec_size(&table->prefix))) {
    table->done = true; *out = NULL; return ORM_STATUS_OK;
  }
  code = orm_tidesdb_iter_value(table->iterator, &data, &size);
  if (code != ORM_TDB_SUCCESS) return table_native(error, code, "read value");
  if (size > UINT64_MAX - key_size) return table_error(error, ORM_STATUS_LIMIT_EXCEEDED, "SQL table read byte overflow");
  orm_sql_budget_amount charge = {0};
  charge.value[ORM_SQL_BUDGET_READ_ROWS] = 1;
  charge.value[ORM_SQL_BUDGET_READ_BYTES] = (uint64_t)key_size + size;
  orm_status_t status = orm_tidesdb_sql_budget_reserve(table->source.budget, &charge, error);
  if (status == ORM_STATUS_OK) status = table_decode(table, data, size, error);
  if (status == ORM_STATUS_OK) { table->advance = true; *out = vec_data_const(&table->values); }
  return status;
}
static orm_status_t table_next(void *context, const orm_value_t **out, orm_error_t *error) {
  orm_sql_table_source *table = context;
  table_clear(table);
  if (table->failure.status != ORM_STATUS_OK) {
    orm_error_set(error, table->failure.status, table->failure.message); return table->failure.status;
  }
  if (table->done) { *out = NULL; return ORM_STATUS_OK; }
  orm_error_t cause; orm_error_init(&cause);
  const orm_status_t status = table_read(table, out, &cause);
  if (status != ORM_STATUS_OK) {
    table_clear(table); table->failure = cause; orm_error_set(error, status, cause.message);
  }
  return status;
}

orm_status_t orm_tidesdb_sql_table_open(orm_tidesdb_transaction_t *transaction,
    orm_tidesdb_column_family_t *family, vstr prefix, const orm_sql_table_schema *schema,
    size_t max_row_bytes, orm_tidesdb_sql_budget *budget, orm_sql_table_source *out, orm_error_t *error) {
  if (!transaction || !family || !prefix.data || !prefix.len || !schema || !schema->columns ||
      !schema->count || !max_row_bytes || !budget || !out || out->source.budget)
    return table_error(error, ORM_STATUS_INVALID_ARGUMENT, "invalid SQL table source arguments");
  if (schema->count > budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES] ||
      schema->count > SIZE_MAX / (ORM_SQL_SELECT_NAME_BYTES + 1))
    return table_error(error, ORM_STATUS_LIMIT_EXCEEDED, "SQL table schema exceeds capacity");
  orm_sql_table_source table = {.source = {.budget = budget, .columns = schema->count, .next = table_next},
      .transaction = transaction, .family = family, .max_row_bytes = max_row_bytes};
  orm_error_init(&table.failure);
  orm_status_t status = orm_tidesdb_sql_budget_reserve_capacity(budget, 1, sizeof(table), 0, &table.metadata_bytes, error);
  if (status == ORM_STATUS_OK) status = table_vector(&table, &table.prefix, prefix.len, sizeof(char), _Alignof(char), &table.prefix_bytes, error);
  if (status == ORM_STATUS_OK) status = table_vector(&table, &table.names, schema->count, sizeof(table_name), _Alignof(table_name), &table.name_bytes, error);
  if (status == ORM_STATUS_OK) status = table_vector(&table, &table.types, schema->count, sizeof(orm_sql_type), _Alignof(orm_sql_type), &table.type_bytes, error);
  if (status == ORM_STATUS_OK) status = table_vector(&table, &table.fields, schema->count, sizeof(orm_tidesdb_field_view), _Alignof(orm_tidesdb_field_view), &table.field_bytes, error);
  if (status == ORM_STATUS_OK) status = table_vector(&table, &table.values, schema->count, sizeof(orm_value_t), _Alignof(orm_value_t), &table.value_bytes, error);
  if (status == ORM_STATUS_OK) memcpy(vec_data(&table.prefix), prefix.data, prefix.len);
  for (size_t i = 0; status == ORM_STATUS_OK && i < schema->count; ++i) {
    const orm_sql_schema_column *column = &schema->columns[i];
    if (!column->name.data || !column->name.len || column->name.len > ORM_SQL_SELECT_NAME_BYTES || memchr(column->name.data, 0, column->name.len)) {
      status = table_error(error, ORM_STATUS_INVALID_ARGUMENT, "invalid SQL table column name"); break;
    }
    orm_sql_predicate validator;
    status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL, column->type, NULL, &validator, error);
    if (status == ORM_STATUS_OK) status = table_steps(&table, i + 1, ORM_SQL_SELECT_NAME_BYTES + 1, error);
    for (size_t j = 0; status == ORM_STATUS_OK && j < i; ++j) {
      const table_name *name = vec_at_const(&table.names, j);
      if (name->size == column->name.len && !memcmp(name->bytes, column->name.data, name->size))
        status = table_error(error, ORM_STATUS_SQL_ERROR, "duplicate SQL table schema column");
    }
    if (status == ORM_STATUS_OK) {
      table_name *name = vec_at(&table.names, i); name->size = column->name.len;
      memcpy(name->bytes, column->name.data, name->size);
      *(orm_sql_type *)vec_at(&table.types, i) = column->type;
    }
  }
  if (status != ORM_STATUS_OK) {
    const orm_status_t released = orm_tidesdb_sql_table_close(&table, NULL);
    return released == ORM_STATUS_OK ? status : released;
  }
  *out = table; out->source.context = out; out->source.types = vec_data_const(&out->types);
  return ORM_STATUS_OK;
}
