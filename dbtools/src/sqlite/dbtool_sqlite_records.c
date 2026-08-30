#include "sqlite/dbtool_sqlite_records.h"

#include "sqlite/dbtool_sqlite.h"

#include <sds.h>
#include <turbo_uuid.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct dbtool_sqlite_sink_s {
  sqlite3 *database;
  const dbtool_table_v1 *table;
  sqlite3_stmt *statement;
  unsigned char *included_columns;
  int active;
  int failed;
} dbtool_sqlite_sink_t;

typedef struct dbtool_sqlite_source_s {
  sqlite3 *database;
  const dbtool_table_v1 *table;
  sqlite3_stmt *statement;
  dbtool_cell *cells;
  unsigned char *uuid_storage;
  int finished;
  int failed;
} dbtool_sqlite_source_t;

static dbtool_status dbtool_sqlite_record_fail(dbtool_error *error, dbtool_status status,
                                               const char *stage, const char *message) {
  dbtool_error_set(error, status, stage, 0, message);
  return status;
}

static int dbtool_sqlite_column_storage_matches(const dbtool_column_v1 *column) {
  switch (column->scalar_kind) {
  case DBTOOL_SCALAR_INT64:
    return column->storage_kind >= DBTOOL_STORAGE_INTEGER16 &&
           column->storage_kind <= DBTOOL_STORAGE_INTEGER64;
  case DBTOOL_SCALAR_UINT64:
    return (column->storage_kind >= DBTOOL_STORAGE_INTEGER16 &&
            column->storage_kind <= DBTOOL_STORAGE_INTEGER64) ||
           column->storage_kind == DBTOOL_STORAGE_UINT64_DECIMAL;
  case DBTOOL_SCALAR_DOUBLE:
    return column->storage_kind == DBTOOL_STORAGE_FLOAT64;
  case DBTOOL_SCALAR_BOOLEAN:
    return column->storage_kind == DBTOOL_STORAGE_BOOLEAN;
  case DBTOOL_SCALAR_TEXT:
    return column->storage_kind == DBTOOL_STORAGE_TEXT;
  case DBTOOL_SCALAR_BYTES:
    return column->storage_kind == DBTOOL_STORAGE_BYTES;
  case DBTOOL_SCALAR_UUID:
    return column->storage_kind == DBTOOL_STORAGE_UUID;
  default:
    return 0;
  }
}

static const dbtool_table_v1 *dbtool_sqlite_record_table(const dbtool_model_v1 *model,
                                                         size_t table_index, dbtool_error *error,
                                                         const char *stage) {
  static const uint32_t known_flags =
      DBTOOL_COLUMN_OPTIONAL | DBTOOL_COLUMN_HAS_DEFAULT | DBTOOL_COLUMN_GENERATED;
  const dbtool_table_v1 *table;
  size_t index;
  if (model == NULL || model->struct_size < sizeof(*model) ||
      model->abi_version != DBTOOL_MODEL_ABI_VERSION || model->tables == NULL ||
      table_index >= model->table_count) {
    dbtool_sqlite_record_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, stage,
                              "invalid generated database model");
    return NULL;
  }
  table = &model->tables[table_index];
  if (table->struct_size < sizeof(*table) || table->abi_version != DBTOOL_MODEL_ABI_VERSION ||
      table->index != table_index || table->name == NULL || table->name[0] == '\0' ||
      table->database_name == NULL || table->database_name[0] == '\0' || table->columns == NULL ||
      table->column_count == 0u || table->column_count > SIZE_MAX / sizeof(dbtool_cell) ||
      table->column_count > SIZE_MAX / TURBO_UUID_SIZE) {
    dbtool_sqlite_record_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, stage,
                              "invalid generated table metadata");
    return NULL;
  }
  for (index = 0u; index < table->column_count; ++index) {
    const dbtool_column_v1 *column = &table->columns[index];
    if (column->struct_size < sizeof(*column) || column->abi_version != DBTOOL_MODEL_ABI_VERSION ||
        column->index != index || column->name == NULL || column->name[0] == '\0' ||
        column->database_name == NULL || column->database_name[0] == '\0' ||
        (column->flags & ~known_flags) != 0u ||
        ((column->flags & DBTOOL_COLUMN_GENERATED) != 0u &&
         (column->flags & (DBTOOL_COLUMN_OPTIONAL | DBTOOL_COLUMN_HAS_DEFAULT)) != 0u) ||
        !dbtool_sqlite_column_storage_matches(column)) {
      dbtool_sqlite_record_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, stage,
                                "invalid generated column metadata");
      return NULL;
    }
  }
  return table;
}

static int dbtool_sds_append(sds *text, const void *data, size_t size) {
  sds next;
  if (text == NULL || *text == NULL || (size != 0u && data == NULL)) return 0;
  next = sdscatlen(*text, data, size);
  if (next == NULL) return 0;
  *text = next;
  return 1;
}

static int dbtool_sds_append_literal(sds *text, const char *literal) {
  return literal != NULL && dbtool_sds_append(text, literal, strlen(literal));
}

static int dbtool_sds_append_identifier(sds *text, const char *identifier) {
  const char *cursor = identifier;
  const char *quote;
  if (identifier == NULL || identifier[0] == '\0' || !dbtool_sds_append(text, "\"", 1u)) return 0;
  while ((quote = strchr(cursor, '"')) != NULL) {
    size_t prefix = (size_t)(quote - cursor);
    if (!dbtool_sds_append(text, cursor, prefix) || !dbtool_sds_append(text, "\"\"", 2u)) return 0;
    cursor = quote + 1;
  }
  return dbtool_sds_append_literal(text, cursor) && dbtool_sds_append(text, "\"", 1u);
}

static sds dbtool_sqlite_insert_sql(const dbtool_table_v1 *table, const dbtool_record_view *record,
                                    unsigned char *included_columns) {
  sds sql = sdsempty();
  size_t included_count = 0u;
  size_t index;
  if (sql == NULL || !dbtool_sds_append_literal(&sql, "INSERT INTO ") ||
      !dbtool_sds_append_identifier(&sql, table->database_name))
    goto fail;
  for (index = 0u; index < table->column_count; ++index) {
    included_columns[index] = record->cells[index].kind != DBTOOL_VALUE_ABSENT;
    if (included_columns[index] != 0u) ++included_count;
  }
  if (included_count == 0u) {
    if (!dbtool_sds_append_literal(&sql, " DEFAULT VALUES")) goto fail;
    return sql;
  }
  if (!dbtool_sds_append_literal(&sql, " (")) goto fail;
  included_count = 0u;
  for (index = 0u; index < table->column_count; ++index) {
    if (included_columns[index] == 0u) continue;
    if (included_count != 0u && !dbtool_sds_append_literal(&sql, ",")) goto fail;
    if (!dbtool_sds_append_identifier(&sql, table->columns[index].database_name)) goto fail;
    ++included_count;
  }
  if (!dbtool_sds_append_literal(&sql, ") VALUES (")) goto fail;
  for (index = 0u; index < included_count; ++index) {
    if (index != 0u && !dbtool_sds_append_literal(&sql, ",")) goto fail;
    if (!dbtool_sds_append_literal(&sql, "?")) goto fail;
  }
  if (!dbtool_sds_append_literal(&sql, ")")) goto fail;
  return sql;

fail:
  sdsfree(sql);
  return NULL;
}

static sds dbtool_sqlite_select_sql(const dbtool_table_v1 *table) {
  sds sql = sdsempty();
  size_t index;
  if (sql == NULL || !dbtool_sds_append_literal(&sql, "SELECT ")) goto fail;
  for (index = 0u; index < table->column_count; ++index) {
    if (index != 0u && !dbtool_sds_append_literal(&sql, ",")) goto fail;
    if (!dbtool_sds_append_identifier(&sql, table->columns[index].database_name)) goto fail;
  }
  if (!dbtool_sds_append_literal(&sql, " FROM ") ||
      !dbtool_sds_append_identifier(&sql, table->database_name))
    goto fail;
  return sql;

fail:
  sdsfree(sql);
  return NULL;
}

static int dbtool_sqlite_column_allows_absent(const dbtool_column_v1 *column) {
  return (column->flags &
          (DBTOOL_COLUMN_OPTIONAL | DBTOOL_COLUMN_HAS_DEFAULT | DBTOOL_COLUMN_GENERATED)) != 0u;
}

static int dbtool_sqlite_column_allows_null(const dbtool_column_v1 *column) {
  return (column->flags & DBTOOL_COLUMN_OPTIONAL) != 0u;
}

static dbtool_status dbtool_sqlite_bind_cell(dbtool_sqlite_sink_t *sink, int parameter,
                                             const dbtool_column_v1 *column,
                                             const dbtool_cell *cell, dbtool_error *error) {
  static const unsigned char empty_blob = 0u;
  char decimal[21];
  char uuid_text[TURBO_UUID_STRING_SIZE];
  turbo_uuid_t uuid;
  int code = SQLITE_MISUSE;
  if (cell->kind == DBTOOL_VALUE_NULL) {
    if (!dbtool_sqlite_column_allows_null(column))
      return dbtool_sqlite_record_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "bind-record",
                                       "non-optional database field is null");
    code = sqlite3_bind_null(sink->statement, parameter);
  } else {
    switch (column->scalar_kind) {
    case DBTOOL_SCALAR_INT64:
      if (cell->kind != DBTOOL_VALUE_INT64) break;
      code = sqlite3_bind_int64(sink->statement, parameter, (sqlite3_int64)cell->data.int64_value);
      break;
    case DBTOOL_SCALAR_UINT64:
      if (cell->kind != DBTOOL_VALUE_UINT64) break;
      if (column->storage_kind == DBTOOL_STORAGE_UINT64_DECIMAL) {
        int length = snprintf(decimal, sizeof(decimal), "%" PRIu64, cell->data.uint64_value);
        if (length <= 0 || (size_t)length >= sizeof(decimal)) break;
        code = sqlite3_bind_text(sink->statement, parameter, decimal, length, SQLITE_TRANSIENT);
      } else if (cell->data.uint64_value <= (uint64_t)INT64_MAX) {
        code =
            sqlite3_bind_int64(sink->statement, parameter, (sqlite3_int64)cell->data.uint64_value);
      }
      break;
    case DBTOOL_SCALAR_DOUBLE:
      if (cell->kind == DBTOOL_VALUE_DOUBLE && isfinite(cell->data.double_value))
        code = sqlite3_bind_double(sink->statement, parameter, cell->data.double_value);
      break;
    case DBTOOL_SCALAR_BOOLEAN:
      if (cell->kind == DBTOOL_VALUE_BOOLEAN &&
          (cell->data.boolean_value == 0 || cell->data.boolean_value == 1))
        code = sqlite3_bind_int(sink->statement, parameter, cell->data.boolean_value);
      break;
    case DBTOOL_SCALAR_TEXT:
      if (cell->kind == DBTOOL_VALUE_TEXT && cell->data.bytes.size <= (size_t)INT_MAX &&
          (cell->data.bytes.size == 0u || cell->data.bytes.data != NULL))
        code = sqlite3_bind_text(sink->statement, parameter,
                                 cell->data.bytes.size == 0u ? ""
                                                             : (const char *)cell->data.bytes.data,
                                 (int)cell->data.bytes.size, SQLITE_TRANSIENT);
      break;
    case DBTOOL_SCALAR_BYTES:
      if (cell->kind == DBTOOL_VALUE_BYTES && cell->data.bytes.size <= (size_t)INT_MAX &&
          (cell->data.bytes.size == 0u || cell->data.bytes.data != NULL))
        code = sqlite3_bind_blob(sink->statement, parameter,
                                 cell->data.bytes.size == 0u ? &empty_blob : cell->data.bytes.data,
                                 (int)cell->data.bytes.size, SQLITE_TRANSIENT);
      break;
    case DBTOOL_SCALAR_UUID:
      if (cell->kind == DBTOOL_VALUE_UUID && cell->data.bytes.size == TURBO_UUID_SIZE &&
          cell->data.bytes.data != NULL) {
        memcpy(uuid.bytes, cell->data.bytes.data, TURBO_UUID_SIZE);
        if (turbo_uuid_format(&uuid, uuid_text, sizeof(uuid_text)) == TURBO_OK)
          code = sqlite3_bind_text(sink->statement, parameter, uuid_text,
                                   TURBO_UUID_STRING_SIZE - 1, SQLITE_TRANSIENT);
      }
      break;
    default:
      break;
    }
  }
  if (code == SQLITE_MISUSE)
    return dbtool_sqlite_record_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "bind-record",
                                     "record cell does not match generated column");
  if (code != SQLITE_OK)
    return dbtool_sqlite_error(sink->database, DBTOOL_STATUS_SQL_ERROR, "bind-record",
                               "bind SQLite record value", NULL, error);
  return DBTOOL_STATUS_OK;
}

static dbtool_status dbtool_sqlite_sink_prepare(dbtool_sqlite_sink_t *sink,
                                                const dbtool_record_view *record,
                                                dbtool_error *error) {
  sds sql = dbtool_sqlite_insert_sql(sink->table, record, sink->included_columns);
  int code;
  if (sql == NULL)
    return dbtool_sqlite_record_fail(error, DBTOOL_STATUS_OUT_OF_MEMORY, "prepare-record",
                                     "build SQLite insert statement");
  if (sdslen(sql) > (size_t)INT_MAX) {
    sdsfree(sql);
    return dbtool_sqlite_record_fail(error, DBTOOL_STATUS_LIMIT_EXCEEDED, "prepare-record",
                                     "SQLite insert statement is too large");
  }
  code = sqlite3_prepare_v2(sink->database, sql, (int)sdslen(sql), &sink->statement, NULL);
  sdsfree(sql);
  if (code != SQLITE_OK)
    return dbtool_sqlite_error(sink->database, DBTOOL_STATUS_SQL_ERROR, "prepare-record",
                               "prepare SQLite insert", NULL, error);
  return DBTOOL_STATUS_OK;
}

static dbtool_status dbtool_sqlite_sink_begin(void *factory_context, void **out_context,
                                              const dbtool_model_v1 *model, size_t table_index,
                                              dbtool_error *error) {
  sqlite3 *database = dbtool_sqlite_native_database(factory_context);
  const dbtool_table_v1 *table;
  dbtool_sqlite_sink_t *sink;
  char *detail = NULL;
  int code;
  if (out_context != NULL) *out_context = NULL;
  dbtool_error_init(error);
  table = dbtool_sqlite_record_table(model, table_index, error, "begin-records");
  if (out_context == NULL || database == NULL || table == NULL ||
      sqlite3_get_autocommit(database) == 0)
    return table == NULL
               ? (error != NULL ? error->status : DBTOOL_STATUS_INVALID_ARGUMENT)
               : dbtool_sqlite_record_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "begin-records",
                                           "invalid SQLite record sink state");
  sink = (dbtool_sqlite_sink_t *)calloc(1u, sizeof(*sink));
  if (sink == NULL)
    return dbtool_sqlite_record_fail(error, DBTOOL_STATUS_OUT_OF_MEMORY, "begin-records",
                                     "allocate SQLite record sink");
  sink->included_columns = (unsigned char *)calloc(table->column_count, sizeof(unsigned char));
  if (sink->included_columns == NULL) {
    free(sink);
    return dbtool_sqlite_record_fail(error, DBTOOL_STATUS_OUT_OF_MEMORY, "begin-records",
                                     "allocate SQLite column presence");
  }
  sink->database = database;
  sink->table = table;
  code = sqlite3_exec(database, "BEGIN IMMEDIATE", NULL, NULL, &detail);
  if (code != SQLITE_OK) {
    dbtool_status status = dbtool_sqlite_error(database, DBTOOL_STATUS_SQL_ERROR, "begin-records",
                                               "begin SQLite record transaction", detail, error);
    sqlite3_free(detail);
    free(sink->included_columns);
    free(sink);
    return status;
  }
  sink->active = 1;
  *out_context = sink;
  return DBTOOL_STATUS_OK;
}

static dbtool_status dbtool_sqlite_sink_write(void *context, const dbtool_record_view *record,
                                              dbtool_error *error) {
  dbtool_sqlite_sink_t *sink = (dbtool_sqlite_sink_t *)context;
  size_t index;
  int parameter = 1;
  int code;
  dbtool_status status;
  dbtool_error_init(error);
  if (sink == NULL || !sink->active || sink->failed || record == NULL || record->cells == NULL ||
      record->cell_count != sink->table->column_count)
    return dbtool_sqlite_record_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "write-record",
                                     "invalid SQLite record sink state");
  for (index = 0u; index < record->cell_count; ++index) {
    if (record->cells[index].kind == DBTOOL_VALUE_ABSENT &&
        !dbtool_sqlite_column_allows_absent(&sink->table->columns[index])) {
      sink->failed = 1;
      return dbtool_sqlite_record_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "write-record",
                                       "required database field is absent");
    }
  }
  if (sink->statement == NULL) {
    status = dbtool_sqlite_sink_prepare(sink, record, error);
    if (status != DBTOOL_STATUS_OK) {
      sink->failed = 1;
      return status;
    }
  } else {
    for (index = 0u; index < record->cell_count; ++index) {
      int included = record->cells[index].kind != DBTOOL_VALUE_ABSENT;
      if (included != (sink->included_columns[index] != 0u)) {
        sink->failed = 1;
        return dbtool_sqlite_record_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "write-record",
                                         "record column presence differs from prepared insert");
      }
    }
  }
  for (index = 0u; index < record->cell_count; ++index) {
    if (sink->included_columns[index] == 0u) continue;
    status = dbtool_sqlite_bind_cell(sink, parameter, &sink->table->columns[index],
                                     &record->cells[index], error);
    if (status != DBTOOL_STATUS_OK) {
      sink->failed = 1;
      (void)sqlite3_clear_bindings(sink->statement);
      (void)sqlite3_reset(sink->statement);
      return status;
    }
    ++parameter;
  }
  code = sqlite3_step(sink->statement);
  if (code != SQLITE_DONE) {
    status = dbtool_sqlite_error(sink->database, DBTOOL_STATUS_SQL_ERROR, "write-record",
                                 "execute SQLite insert", NULL, error);
    sink->failed = 1;
    (void)sqlite3_clear_bindings(sink->statement);
    (void)sqlite3_reset(sink->statement);
    return status;
  }
  code = sqlite3_reset(sink->statement);
  (void)sqlite3_clear_bindings(sink->statement);
  if (code != SQLITE_OK) {
    sink->failed = 1;
    return dbtool_sqlite_error(sink->database, DBTOOL_STATUS_SQL_ERROR, "write-record",
                               "reset SQLite insert", NULL, error);
  }
  return DBTOOL_STATUS_OK;
}

static dbtool_status dbtool_sqlite_sink_commit(void *context, dbtool_error *error) {
  dbtool_sqlite_sink_t *sink = (dbtool_sqlite_sink_t *)context;
  char *detail = NULL;
  int code;
  dbtool_error_init(error);
  if (sink == NULL || !sink->active || sink->failed)
    return dbtool_sqlite_record_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "commit-records",
                                     "invalid SQLite record sink state");
  code = sqlite3_exec(sink->database, "COMMIT", NULL, NULL, &detail);
  if (code != SQLITE_OK) {
    dbtool_status status =
        dbtool_sqlite_error(sink->database, DBTOOL_STATUS_SQL_ERROR, "commit-records",
                            "commit SQLite record transaction", detail, error);
    sink->failed = 1;
    sqlite3_free(detail);
    return status;
  }
  sink->active = 0;
  return DBTOOL_STATUS_OK;
}

static dbtool_status dbtool_sqlite_sink_rollback(void *context, dbtool_error *error) {
  dbtool_sqlite_sink_t *sink = (dbtool_sqlite_sink_t *)context;
  char *detail = NULL;
  int code;
  dbtool_error_init(error);
  if (sink == NULL || !sink->active)
    return dbtool_sqlite_record_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "rollback-records",
                                     "invalid SQLite record sink state");
  code = sqlite3_exec(sink->database, "ROLLBACK", NULL, NULL, &detail);
  if (code != SQLITE_OK) {
    dbtool_status status =
        dbtool_sqlite_error(sink->database, DBTOOL_STATUS_SQL_ERROR, "rollback-records",
                            "rollback SQLite record transaction", detail, error);
    sqlite3_free(detail);
    return status;
  }
  sink->active = 0;
  return DBTOOL_STATUS_OK;
}

static void dbtool_sqlite_sink_close(void *context) {
  dbtool_sqlite_sink_t *sink = (dbtool_sqlite_sink_t *)context;
  if (sink == NULL) return;
  if (sink->active) (void)sqlite3_exec(sink->database, "ROLLBACK", NULL, NULL, NULL);
  if (sink->statement != NULL) (void)sqlite3_finalize(sink->statement);
  free(sink->included_columns);
  free(sink);
}

static int dbtool_sqlite_parse_uint64(const unsigned char *text, size_t size, uint64_t *out) {
  char buffer[21];
  char *end = NULL;
  unsigned long long parsed;
  size_t index;
  if (text == NULL || out == NULL || size == 0u || size >= sizeof(buffer) ||
      (size > 1u && text[0] == '0'))
    return 0;
  for (index = 0u; index < size; ++index)
    if (text[index] < '0' || text[index] > '9') return 0;
  memcpy(buffer, text, size);
  buffer[size] = '\0';
  errno = 0;
  parsed = strtoull(buffer, &end, 10);
  if (errno == ERANGE || end != buffer + size) return 0;
  *out = (uint64_t)parsed;
  return 1;
}

static dbtool_status dbtool_sqlite_source_cell(dbtool_sqlite_source_t *source, size_t index,
                                               dbtool_error *error) {
  const dbtool_column_v1 *column = &source->table->columns[index];
  dbtool_cell *cell = &source->cells[index];
  int type = sqlite3_column_type(source->statement, (int)index);
  int size;
  const unsigned char *text;
  turbo_uuid_t uuid;
  if (type == SQLITE_NULL) {
    if (!dbtool_sqlite_column_allows_null(column))
      return dbtool_sqlite_record_fail(error, DBTOOL_STATUS_SQL_ERROR, "read-record",
                                       "non-optional database column is null");
    cell->kind = DBTOOL_VALUE_NULL;
    return DBTOOL_STATUS_OK;
  }
  switch (column->storage_kind) {
  case DBTOOL_STORAGE_INTEGER16:
  case DBTOOL_STORAGE_INTEGER32:
  case DBTOOL_STORAGE_INTEGER64: {
    sqlite3_int64 value;
    if (type != SQLITE_INTEGER) break;
    value = sqlite3_column_int64(source->statement, (int)index);
    if (column->scalar_kind == DBTOOL_SCALAR_INT64) {
      cell->kind = DBTOOL_VALUE_INT64;
      cell->data.int64_value = (int64_t)value;
      return DBTOOL_STATUS_OK;
    }
    if (column->scalar_kind == DBTOOL_SCALAR_UINT64 && value >= 0) {
      cell->kind = DBTOOL_VALUE_UINT64;
      cell->data.uint64_value = (uint64_t)value;
      return DBTOOL_STATUS_OK;
    }
    break;
  }
  case DBTOOL_STORAGE_UINT64_DECIMAL:
    if (type != SQLITE_TEXT || column->scalar_kind != DBTOOL_SCALAR_UINT64) break;
    size = sqlite3_column_bytes(source->statement, (int)index);
    text = sqlite3_column_text(source->statement, (int)index);
    if (size > 0 && dbtool_sqlite_parse_uint64(text, (size_t)size, &cell->data.uint64_value)) {
      cell->kind = DBTOOL_VALUE_UINT64;
      return DBTOOL_STATUS_OK;
    }
    return dbtool_sqlite_record_fail(error, DBTOOL_STATUS_SQL_ERROR, "read-record",
                                     "stored value is not canonical uint64 text");
  case DBTOOL_STORAGE_FLOAT64:
    if ((type == SQLITE_FLOAT || type == SQLITE_INTEGER) &&
        column->scalar_kind == DBTOOL_SCALAR_DOUBLE) {
      double value = sqlite3_column_double(source->statement, (int)index);
      if (isfinite(value)) {
        cell->kind = DBTOOL_VALUE_DOUBLE;
        cell->data.double_value = value;
        return DBTOOL_STATUS_OK;
      }
    }
    break;
  case DBTOOL_STORAGE_BOOLEAN:
    if (type == SQLITE_INTEGER && column->scalar_kind == DBTOOL_SCALAR_BOOLEAN) {
      int value = sqlite3_column_int(source->statement, (int)index);
      if (value == 0 || value == 1) {
        cell->kind = DBTOOL_VALUE_BOOLEAN;
        cell->data.boolean_value = value;
        return DBTOOL_STATUS_OK;
      }
    }
    break;
  case DBTOOL_STORAGE_TEXT:
    if (type == SQLITE_TEXT && column->scalar_kind == DBTOOL_SCALAR_TEXT) {
      size = sqlite3_column_bytes(source->statement, (int)index);
      text = sqlite3_column_text(source->statement, (int)index);
      if (size >= 0 && (size == 0 || text != NULL)) {
        cell->kind = DBTOOL_VALUE_TEXT;
        cell->data.bytes.data = text;
        cell->data.bytes.size = (size_t)size;
        return DBTOOL_STATUS_OK;
      }
    }
    break;
  case DBTOOL_STORAGE_BYTES:
    if (type == SQLITE_BLOB && column->scalar_kind == DBTOOL_SCALAR_BYTES) {
      size = sqlite3_column_bytes(source->statement, (int)index);
      text = (const unsigned char *)sqlite3_column_blob(source->statement, (int)index);
      if (size >= 0 && (size == 0 || text != NULL)) {
        cell->kind = DBTOOL_VALUE_BYTES;
        cell->data.bytes.data = text;
        cell->data.bytes.size = (size_t)size;
        return DBTOOL_STATUS_OK;
      }
    }
    break;
  case DBTOOL_STORAGE_UUID:
    if (type == SQLITE_TEXT && column->scalar_kind == DBTOOL_SCALAR_UUID) {
      char uuid_text[TURBO_UUID_STRING_SIZE];
      size = sqlite3_column_bytes(source->statement, (int)index);
      text = sqlite3_column_text(source->statement, (int)index);
      if (size == TURBO_UUID_STRING_SIZE - 1 && text != NULL &&
          memchr(text, '\0', (size_t)size) == NULL) {
        memcpy(uuid_text, text, (size_t)size);
        uuid_text[size] = '\0';
        if (turbo_uuid_parse(uuid_text, &uuid) == TURBO_OK) {
          memcpy(source->uuid_storage + index * TURBO_UUID_SIZE, uuid.bytes, TURBO_UUID_SIZE);
          cell->kind = DBTOOL_VALUE_UUID;
          cell->data.bytes.data = source->uuid_storage + index * TURBO_UUID_SIZE;
          cell->data.bytes.size = TURBO_UUID_SIZE;
          return DBTOOL_STATUS_OK;
        }
      }
    }
    break;
  default:
    break;
  }
  return dbtool_sqlite_record_fail(error, DBTOOL_STATUS_SQL_ERROR, "read-record",
                                   "stored SQLite value does not match generated column");
}

static dbtool_status dbtool_sqlite_source_open(void *factory_context, void **out_context,
                                               const dbtool_model_v1 *model, size_t table_index,
                                               dbtool_error *error) {
  sqlite3 *database = dbtool_sqlite_native_database(factory_context);
  const dbtool_table_v1 *table;
  dbtool_sqlite_source_t *source;
  sds sql;
  int code;
  if (out_context != NULL) *out_context = NULL;
  dbtool_error_init(error);
  table = dbtool_sqlite_record_table(model, table_index, error, "open-records");
  if (out_context == NULL || database == NULL || table == NULL)
    return table == NULL
               ? (error != NULL ? error->status : DBTOOL_STATUS_INVALID_ARGUMENT)
               : dbtool_sqlite_record_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "open-records",
                                           "invalid SQLite record source");
  source = (dbtool_sqlite_source_t *)calloc(1u, sizeof(*source));
  if (source == NULL)
    return dbtool_sqlite_record_fail(error, DBTOOL_STATUS_OUT_OF_MEMORY, "open-records",
                                     "allocate SQLite record source");
  source->cells = (dbtool_cell *)calloc(table->column_count, sizeof(*source->cells));
  source->uuid_storage = (unsigned char *)calloc(table->column_count, TURBO_UUID_SIZE);
  if (source->cells == NULL || source->uuid_storage == NULL) {
    free(source->uuid_storage);
    free(source->cells);
    free(source);
    return dbtool_sqlite_record_fail(error, DBTOOL_STATUS_OUT_OF_MEMORY, "open-records",
                                     "allocate SQLite record views");
  }
  source->database = database;
  source->table = table;
  sql = dbtool_sqlite_select_sql(table);
  if (sql == NULL) {
    free(source->uuid_storage);
    free(source->cells);
    free(source);
    return dbtool_sqlite_record_fail(error, DBTOOL_STATUS_OUT_OF_MEMORY, "open-records",
                                     "build SQLite select statement");
  }
  if (sdslen(sql) > (size_t)INT_MAX) {
    sdsfree(sql);
    free(source->uuid_storage);
    free(source->cells);
    free(source);
    return dbtool_sqlite_record_fail(error, DBTOOL_STATUS_LIMIT_EXCEEDED, "open-records",
                                     "SQLite select statement is too large");
  }
  code = sqlite3_prepare_v2(database, sql, (int)sdslen(sql), &source->statement, NULL);
  sdsfree(sql);
  if (code != SQLITE_OK) {
    dbtool_status status = dbtool_sqlite_error(database, DBTOOL_STATUS_SQL_ERROR, "open-records",
                                               "prepare SQLite select", NULL, error);
    free(source->uuid_storage);
    free(source->cells);
    free(source);
    return status;
  }
  *out_context = source;
  return DBTOOL_STATUS_OK;
}

static dbtool_record_step dbtool_sqlite_source_next(void *context, dbtool_record_view *out,
                                                    dbtool_error *error) {
  dbtool_sqlite_source_t *source = (dbtool_sqlite_source_t *)context;
  size_t index;
  int code;
  dbtool_error_init(error);
  if (out != NULL) *out = (dbtool_record_view){0};
  if (source == NULL || out == NULL || source->finished || source->failed) {
    dbtool_sqlite_record_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "read-record",
                              "invalid SQLite record source state");
    return DBTOOL_RECORD_ERROR;
  }
  code = sqlite3_step(source->statement);
  if (code == SQLITE_DONE) {
    source->finished = 1;
    return DBTOOL_RECORD_DONE;
  }
  if (code != SQLITE_ROW) {
    source->failed = 1;
    (void)dbtool_sqlite_error(source->database, DBTOOL_STATUS_SQL_ERROR, "read-record",
                              "step SQLite select", NULL, error);
    return DBTOOL_RECORD_ERROR;
  }
  memset(source->cells, 0, source->table->column_count * sizeof(*source->cells));
  for (index = 0u; index < source->table->column_count; ++index) {
    if (dbtool_sqlite_source_cell(source, index, error) != DBTOOL_STATUS_OK) {
      source->failed = 1;
      return DBTOOL_RECORD_ERROR;
    }
  }
  out->cells = source->cells;
  out->cell_count = source->table->column_count;
  return DBTOOL_RECORD_ROW;
}

static void dbtool_sqlite_source_close(void *context) {
  dbtool_sqlite_source_t *source = (dbtool_sqlite_source_t *)context;
  if (source == NULL) return;
  if (source->statement != NULL) (void)sqlite3_finalize(source->statement);
  free(source->uuid_storage);
  free(source->cells);
  free(source);
}

static const dbtool_record_sink_ops DBTOOL_SQLITE_SINK_OPS = {
    sizeof(dbtool_record_sink_ops), DBTOOL_RECORD_ABI_VERSION, dbtool_sqlite_sink_begin,
    dbtool_sqlite_sink_write,       dbtool_sqlite_sink_commit, dbtool_sqlite_sink_rollback,
    dbtool_sqlite_sink_close};

static const dbtool_record_source_ops DBTOOL_SQLITE_SOURCE_OPS = {
    sizeof(dbtool_record_source_ops), DBTOOL_RECORD_ABI_VERSION, dbtool_sqlite_source_open,
    dbtool_sqlite_source_next, dbtool_sqlite_source_close};

const dbtool_record_sink_ops *dbtool_sqlite_record_sink(void) { return &DBTOOL_SQLITE_SINK_OPS; }

const dbtool_record_source_ops *dbtool_sqlite_record_source(void) {
  return &DBTOOL_SQLITE_SOURCE_OPS;
}
