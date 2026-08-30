#include "postgresql/dbtool_postgresql_records.h"

#include "data/dbtool_model_internal.h"
#include "postgresql/dbtool_postgresql.h"

#include <sds.h>
#include <turbo_uuid.h>

#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  DBTOOL_PG_OID_BOOL = 16,
  DBTOOL_PG_OID_BYTEA = 17,
  DBTOOL_PG_OID_INT8 = 20,
  DBTOOL_PG_OID_INT2 = 21,
  DBTOOL_PG_OID_INT4 = 23,
  DBTOOL_PG_OID_TEXT = 25,
  DBTOOL_PG_OID_FLOAT4 = 700,
  DBTOOL_PG_OID_FLOAT8 = 701,
  DBTOOL_PG_OID_NUMERIC = 1700,
  DBTOOL_PG_OID_UUID = 2950,
  DBTOOL_PG_TEXT_CAPACITY = 64,
  DBTOOL_PG_PARSE_CAPACITY = 128
};

typedef struct dbtool_postgresql_sink_s {
  PGconn *connection;
  const dbtool_table_v1 *table;
  unsigned char *included_columns;
  Oid *parameter_types;
  const char **parameter_values;
  int *parameter_lengths;
  int *parameter_formats;
  char (*text_storage)[DBTOOL_PG_TEXT_CAPACITY];
  size_t parameter_count;
  int prepared;
  int active;
  int failed;
} dbtool_postgresql_sink_t;

typedef struct dbtool_postgresql_source_s {
  PGconn *connection;
  const dbtool_table_v1 *table;
  PGresult *row_result;
  dbtool_cell *cells;
  unsigned char *uuid_storage;
  unsigned char **owned_bytes;
  int finished;
  int failed;
} dbtool_postgresql_source_t;

static dbtool_status dbtool_postgresql_record_fail(dbtool_error *error,
                                                   dbtool_status status,
                                                   const char *stage,
                                                   const char *message) {
  dbtool_error_set(error, status, stage, 0, message);
  return status;
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
  if (identifier == NULL || identifier[0] == '\0' ||
      !dbtool_sds_append(text, "\"", 1u))
    return 0;
  while ((quote = strchr(cursor, '"')) != NULL) {
    size_t prefix = (size_t)(quote - cursor);
    if (!dbtool_sds_append(text, cursor, prefix) ||
        !dbtool_sds_append(text, "\"\"", 2u))
      return 0;
    cursor = quote + 1;
  }
  return dbtool_sds_append_literal(text, cursor) &&
         dbtool_sds_append(text, "\"", 1u);
}

static Oid dbtool_postgresql_storage_oid(dbtool_storage_kind kind) {
  switch (kind) {
    case DBTOOL_STORAGE_INTEGER16: return DBTOOL_PG_OID_INT2;
    case DBTOOL_STORAGE_INTEGER32: return DBTOOL_PG_OID_INT4;
    case DBTOOL_STORAGE_INTEGER64: return DBTOOL_PG_OID_INT8;
    case DBTOOL_STORAGE_UINT64_DECIMAL: return DBTOOL_PG_OID_NUMERIC;
    case DBTOOL_STORAGE_FLOAT32: return DBTOOL_PG_OID_FLOAT4;
    case DBTOOL_STORAGE_FLOAT64: return DBTOOL_PG_OID_FLOAT8;
    case DBTOOL_STORAGE_BOOLEAN: return DBTOOL_PG_OID_BOOL;
    case DBTOOL_STORAGE_TEXT: return DBTOOL_PG_OID_TEXT;
    case DBTOOL_STORAGE_BYTES: return DBTOOL_PG_OID_BYTEA;
    case DBTOOL_STORAGE_UUID: return DBTOOL_PG_OID_UUID;
    default: return 0u;
  }
}

static int dbtool_postgresql_column_allows_absent(
    const dbtool_column_v1 *column) {
  return (column->flags & (DBTOOL_COLUMN_OPTIONAL |
                           DBTOOL_COLUMN_HAS_DEFAULT |
                           DBTOOL_COLUMN_GENERATED)) != 0u;
}

static int dbtool_postgresql_column_allows_null(
    const dbtool_column_v1 *column) {
  return (column->flags & DBTOOL_COLUMN_OPTIONAL) != 0u;
}

static dbtool_status dbtool_postgresql_command(
    PGconn *connection, const char *sql, const char *stage,
    const char *operation, dbtool_error *error) {
  PGresult *result = PQexec(connection, sql);
  ExecStatusType status;
  dbtool_status mapped;
  if (result == NULL)
    return dbtool_postgresql_error(DBTOOL_STATUS_CONNECTION_ERROR, stage, 0,
                                   operation, PQerrorMessage(connection), error);
  status = PQresultStatus(result);
  if (status == PGRES_COMMAND_OK) {
    PQclear(result);
    return DBTOOL_STATUS_OK;
  }
  mapped = dbtool_postgresql_error(DBTOOL_STATUS_SQL_ERROR, stage, (int)status,
                                   operation, PQresultErrorMessage(result),
                                   error);
  PQclear(result);
  return mapped;
}

static sds dbtool_postgresql_insert_sql(
    const dbtool_table_v1 *table, const dbtool_record_view *record,
    unsigned char *included_columns, size_t *out_parameter_count) {
  sds sql = sdsempty();
  size_t included_count = 0u;
  size_t index;
  char placeholder[32];
  if (sql == NULL || !dbtool_sds_append_literal(&sql, "INSERT INTO ") ||
      !dbtool_sds_append_identifier(&sql, table->database_name))
    goto fail;
  for (index = 0u; index < table->column_count; ++index) {
    included_columns[index] =
        record->cells[index].kind != DBTOOL_VALUE_ABSENT;
    if (included_columns[index] != 0u) ++included_count;
  }
  *out_parameter_count = included_count;
  if (included_count == 0u) {
    if (!dbtool_sds_append_literal(&sql, " DEFAULT VALUES")) goto fail;
    return sql;
  }
  if (!dbtool_sds_append_literal(&sql, " (")) goto fail;
  included_count = 0u;
  for (index = 0u; index < table->column_count; ++index) {
    if (included_columns[index] == 0u) continue;
    if (included_count != 0u && !dbtool_sds_append_literal(&sql, ","))
      goto fail;
    if (!dbtool_sds_append_identifier(&sql,
                                      table->columns[index].database_name))
      goto fail;
    ++included_count;
  }
  if (!dbtool_sds_append_literal(&sql, ") VALUES (")) goto fail;
  for (index = 0u; index < included_count; ++index) {
    int length = snprintf(placeholder, sizeof(placeholder), "$%zu", index + 1u);
    if (length <= 0 || (size_t)length >= sizeof(placeholder) ||
        (index != 0u && !dbtool_sds_append_literal(&sql, ",")) ||
        !dbtool_sds_append(&sql, placeholder, (size_t)length))
      goto fail;
  }
  if (!dbtool_sds_append_literal(&sql, ")")) goto fail;
  return sql;

fail:
  sdsfree(sql);
  return NULL;
}

static sds dbtool_postgresql_select_sql(const dbtool_table_v1 *table) {
  sds sql = sdsempty();
  size_t index;
  if (sql == NULL || !dbtool_sds_append_literal(&sql, "SELECT ")) goto fail;
  for (index = 0u; index < table->column_count; ++index) {
    if (index != 0u && !dbtool_sds_append_literal(&sql, ",")) goto fail;
    if (!dbtool_sds_append_identifier(&sql,
                                      table->columns[index].database_name))
      goto fail;
  }
  if (!dbtool_sds_append_literal(&sql, " FROM ") ||
      !dbtool_sds_append_identifier(&sql, table->database_name))
    goto fail;
  return sql;

fail:
  sdsfree(sql);
  return NULL;
}

static int dbtool_postgresql_sink_allocate(dbtool_postgresql_sink_t *sink) {
  size_t count = sink->table->column_count;
  if (count > SIZE_MAX / sizeof(Oid) ||
      count > SIZE_MAX / sizeof(const char *) ||
      count > SIZE_MAX / sizeof(int) ||
      count > SIZE_MAX / DBTOOL_PG_TEXT_CAPACITY)
    return 0;
  sink->included_columns = (unsigned char *)calloc(count, 1u);
  sink->parameter_types = (Oid *)calloc(count, sizeof(Oid));
  sink->parameter_values = (const char **)calloc(count, sizeof(const char *));
  sink->parameter_lengths = (int *)calloc(count, sizeof(int));
  sink->parameter_formats = (int *)calloc(count, sizeof(int));
  sink->text_storage =
      (char(*)[DBTOOL_PG_TEXT_CAPACITY])calloc(count,
                                               DBTOOL_PG_TEXT_CAPACITY);
  return sink->included_columns != NULL && sink->parameter_types != NULL &&
         sink->parameter_values != NULL && sink->parameter_lengths != NULL &&
         sink->parameter_formats != NULL && sink->text_storage != NULL;
}

static void dbtool_postgresql_sink_free(dbtool_postgresql_sink_t *sink) {
  free(sink->text_storage);
  free(sink->parameter_formats);
  free(sink->parameter_lengths);
  free(sink->parameter_values);
  free(sink->parameter_types);
  free(sink->included_columns);
  free(sink);
}

static dbtool_status dbtool_postgresql_sink_begin(
    void *factory_context, void **out_context, const dbtool_model_v1 *model,
    size_t table_index, dbtool_error *error) {
  PGconn *connection = dbtool_postgresql_native_connection(factory_context);
  const dbtool_table_v1 *table = dbtool_model_table_valid(model, table_index);
  dbtool_postgresql_sink_t *sink;
  dbtool_status status;
  if (out_context != NULL) *out_context = NULL;
  dbtool_error_init(error);
  if (out_context == NULL || connection == NULL || table == NULL)
    return dbtool_postgresql_record_fail(
        error, DBTOOL_STATUS_INVALID_ARGUMENT, "begin-records",
        "invalid PostgreSQL record sink state");
  sink = (dbtool_postgresql_sink_t *)calloc(1u, sizeof(*sink));
  if (sink == NULL)
    return dbtool_postgresql_record_fail(
        error, DBTOOL_STATUS_OUT_OF_MEMORY, "begin-records",
        "allocate PostgreSQL record sink");
  sink->connection = connection;
  sink->table = table;
  if (!dbtool_postgresql_sink_allocate(sink)) {
    dbtool_postgresql_sink_free(sink);
    return dbtool_postgresql_record_fail(
        error, DBTOOL_STATUS_OUT_OF_MEMORY, "begin-records",
        "allocate PostgreSQL parameter state");
  }
  status = dbtool_postgresql_command(connection, "BEGIN", "begin-records",
                                     "begin PostgreSQL record transaction",
                                     error);
  if (status != DBTOOL_STATUS_OK) {
    dbtool_postgresql_sink_free(sink);
    return status;
  }
  sink->active = 1;
  *out_context = sink;
  return DBTOOL_STATUS_OK;
}

static dbtool_status dbtool_postgresql_sink_prepare(
    dbtool_postgresql_sink_t *sink, const dbtool_record_view *record,
    dbtool_error *error) {
  sds sql = dbtool_postgresql_insert_sql(
      sink->table, record, sink->included_columns, &sink->parameter_count);
  PGresult *result;
  size_t column_index;
  size_t parameter_index = 0u;
  if (sql == NULL)
    return dbtool_postgresql_record_fail(
        error, DBTOOL_STATUS_OUT_OF_MEMORY, "prepare-record",
        "build PostgreSQL insert statement");
  if (sink->parameter_count > (size_t)INT_MAX) {
    sdsfree(sql);
    return dbtool_postgresql_record_fail(
        error, DBTOOL_STATUS_LIMIT_EXCEEDED, "prepare-record",
        "PostgreSQL parameter count exceeds int range");
  }
  for (column_index = 0u; column_index < sink->table->column_count;
       ++column_index) {
    if (sink->included_columns[column_index] == 0u) continue;
    sink->parameter_types[parameter_index++] = dbtool_postgresql_storage_oid(
        sink->table->columns[column_index].storage_kind);
  }
  result = PQprepare(sink->connection, "", sql, (int)sink->parameter_count,
                     sink->parameter_types);
  sdsfree(sql);
  if (result == NULL)
    return dbtool_postgresql_error(
        DBTOOL_STATUS_CONNECTION_ERROR, "prepare-record", 0,
        "prepare PostgreSQL insert", PQerrorMessage(sink->connection), error);
  if (PQresultStatus(result) != PGRES_COMMAND_OK) {
    dbtool_status status = dbtool_postgresql_error(
        DBTOOL_STATUS_SQL_ERROR, "prepare-record", (int)PQresultStatus(result),
        "prepare PostgreSQL insert", PQresultErrorMessage(result), error);
    PQclear(result);
    return status;
  }
  PQclear(result);
  sink->prepared = 1;
  return DBTOOL_STATUS_OK;
}

static int dbtool_postgresql_write_text(char *output, size_t capacity,
                                        const char *format, ...) {
  va_list arguments;
  int length;
  va_start(arguments, format);
  length = vsnprintf(output, capacity, format, arguments);
  va_end(arguments);
  return length > 0 && (size_t)length < capacity;
}

static dbtool_status dbtool_postgresql_bind_cell(
    dbtool_postgresql_sink_t *sink, size_t parameter_index,
    const dbtool_column_v1 *column, const dbtool_cell *cell,
    dbtool_error *error) {
  char *text = sink->text_storage[parameter_index];
  turbo_uuid_t uuid;
  if (cell->kind == DBTOOL_VALUE_NULL) {
    if (!dbtool_postgresql_column_allows_null(column))
      return dbtool_postgresql_record_fail(
          error, DBTOOL_STATUS_INVALID_ARGUMENT, "bind-record",
          "non-optional database field is null");
    sink->parameter_values[parameter_index] = NULL;
    return DBTOOL_STATUS_OK;
  }
  switch (column->scalar_kind) {
    case DBTOOL_SCALAR_INT64:
      if (cell->kind != DBTOOL_VALUE_INT64 ||
          (column->storage_kind == DBTOOL_STORAGE_INTEGER16 &&
           (cell->data.int64_value < INT16_MIN ||
            cell->data.int64_value > INT16_MAX)) ||
          (column->storage_kind == DBTOOL_STORAGE_INTEGER32 &&
           (cell->data.int64_value < INT32_MIN ||
            cell->data.int64_value > INT32_MAX)) ||
          !dbtool_postgresql_write_text(text, DBTOOL_PG_TEXT_CAPACITY,
                                        "%" PRId64,
                                        cell->data.int64_value))
        break;
      sink->parameter_values[parameter_index] = text;
      return DBTOOL_STATUS_OK;
    case DBTOOL_SCALAR_UINT64:
      if (cell->kind != DBTOOL_VALUE_UINT64 ||
          (column->storage_kind == DBTOOL_STORAGE_INTEGER16 &&
           cell->data.uint64_value > INT16_MAX) ||
          (column->storage_kind == DBTOOL_STORAGE_INTEGER32 &&
           cell->data.uint64_value > INT32_MAX) ||
          (column->storage_kind == DBTOOL_STORAGE_INTEGER64 &&
           cell->data.uint64_value > INT64_MAX) ||
          !dbtool_postgresql_write_text(text, DBTOOL_PG_TEXT_CAPACITY,
                                        "%" PRIu64,
                                        cell->data.uint64_value))
        break;
      sink->parameter_values[parameter_index] = text;
      return DBTOOL_STATUS_OK;
    case DBTOOL_SCALAR_DOUBLE:
      if (cell->kind != DBTOOL_VALUE_DOUBLE ||
          !isfinite(cell->data.double_value) ||
          (column->storage_kind == DBTOOL_STORAGE_FLOAT32 &&
           fabs(cell->data.double_value) > FLT_MAX) ||
          !dbtool_postgresql_write_text(
              text, DBTOOL_PG_TEXT_CAPACITY,
              column->storage_kind == DBTOOL_STORAGE_FLOAT32 ? "%.9g"
                                                             : "%.17g",
              cell->data.double_value))
        break;
      sink->parameter_values[parameter_index] = text;
      return DBTOOL_STATUS_OK;
    case DBTOOL_SCALAR_BOOLEAN:
      if (cell->kind != DBTOOL_VALUE_BOOLEAN ||
          (cell->data.boolean_value != 0 && cell->data.boolean_value != 1))
        break;
      sink->parameter_values[parameter_index] =
          cell->data.boolean_value != 0 ? "t" : "f";
      return DBTOOL_STATUS_OK;
    case DBTOOL_SCALAR_TEXT:
      if (cell->kind != DBTOOL_VALUE_TEXT ||
          cell->data.bytes.size > (size_t)INT_MAX ||
          (cell->data.bytes.size != 0u && cell->data.bytes.data == NULL) ||
          (cell->data.bytes.size != 0u &&
           memchr(cell->data.bytes.data, '\0', cell->data.bytes.size) != NULL))
        break;
      sink->parameter_values[parameter_index] =
          cell->data.bytes.size == 0u
              ? ""
              : (const char *)cell->data.bytes.data;
      sink->parameter_lengths[parameter_index] = (int)cell->data.bytes.size;
      return DBTOOL_STATUS_OK;
    case DBTOOL_SCALAR_BYTES:
      if (cell->kind != DBTOOL_VALUE_BYTES ||
          cell->data.bytes.size > (size_t)INT_MAX ||
          (cell->data.bytes.size != 0u && cell->data.bytes.data == NULL))
        break;
      sink->parameter_values[parameter_index] =
          cell->data.bytes.size == 0u
              ? ""
              : (const char *)cell->data.bytes.data;
      sink->parameter_lengths[parameter_index] = (int)cell->data.bytes.size;
      sink->parameter_formats[parameter_index] = 1;
      return DBTOOL_STATUS_OK;
    case DBTOOL_SCALAR_UUID:
      if (cell->kind != DBTOOL_VALUE_UUID ||
          cell->data.bytes.size != TURBO_UUID_SIZE ||
          cell->data.bytes.data == NULL)
        break;
      memcpy(uuid.bytes, cell->data.bytes.data, TURBO_UUID_SIZE);
      if (turbo_uuid_format(&uuid, text, DBTOOL_PG_TEXT_CAPACITY) != TURBO_OK)
        break;
      sink->parameter_values[parameter_index] = text;
      return DBTOOL_STATUS_OK;
    default:
      break;
  }
  return dbtool_postgresql_record_fail(
      error, DBTOOL_STATUS_INVALID_ARGUMENT, "bind-record",
      "record cell does not match generated PostgreSQL column");
}

static dbtool_status dbtool_postgresql_sink_write(
    void *context, const dbtool_record_view *record, dbtool_error *error) {
  dbtool_postgresql_sink_t *sink = (dbtool_postgresql_sink_t *)context;
  size_t column_index;
  size_t parameter_index = 0u;
  PGresult *result;
  dbtool_status status;
  dbtool_error_init(error);
  if (sink == NULL || !sink->active || sink->failed || record == NULL ||
      record->cells == NULL || record->cell_count != sink->table->column_count)
    return dbtool_postgresql_record_fail(
        error, DBTOOL_STATUS_INVALID_ARGUMENT, "write-record",
        "invalid PostgreSQL record sink state");
  for (column_index = 0u; column_index < record->cell_count; ++column_index) {
    if (record->cells[column_index].kind == DBTOOL_VALUE_ABSENT &&
        !dbtool_postgresql_column_allows_absent(
            &sink->table->columns[column_index])) {
      sink->failed = 1;
      return dbtool_postgresql_record_fail(
          error, DBTOOL_STATUS_INVALID_ARGUMENT, "write-record",
          "required database field is absent");
    }
  }
  if (!sink->prepared) {
    status = dbtool_postgresql_sink_prepare(sink, record, error);
    if (status != DBTOOL_STATUS_OK) {
      sink->failed = 1;
      return status;
    }
  } else {
    for (column_index = 0u; column_index < record->cell_count;
         ++column_index) {
      int included = record->cells[column_index].kind != DBTOOL_VALUE_ABSENT;
      if (included != (sink->included_columns[column_index] != 0u)) {
        sink->failed = 1;
        return dbtool_postgresql_record_fail(
            error, DBTOOL_STATUS_INVALID_ARGUMENT, "write-record",
            "record column presence differs from prepared insert");
      }
    }
  }
  memset(sink->parameter_values, 0,
         sink->table->column_count * sizeof(*sink->parameter_values));
  memset(sink->parameter_lengths, 0,
         sink->table->column_count * sizeof(*sink->parameter_lengths));
  memset(sink->parameter_formats, 0,
         sink->table->column_count * sizeof(*sink->parameter_formats));
  for (column_index = 0u; column_index < record->cell_count; ++column_index) {
    if (sink->included_columns[column_index] == 0u) continue;
    status = dbtool_postgresql_bind_cell(
        sink, parameter_index, &sink->table->columns[column_index],
        &record->cells[column_index], error);
    if (status != DBTOOL_STATUS_OK) {
      sink->failed = 1;
      return status;
    }
    ++parameter_index;
  }
  result = PQexecPrepared(
      sink->connection, "", (int)sink->parameter_count,
      sink->parameter_values, sink->parameter_lengths,
      sink->parameter_formats, 0);
  if (result == NULL) {
    sink->failed = 1;
    return dbtool_postgresql_error(
        DBTOOL_STATUS_CONNECTION_ERROR, "write-record", 0,
        "execute PostgreSQL insert", PQerrorMessage(sink->connection), error);
  }
  if (PQresultStatus(result) != PGRES_COMMAND_OK) {
    status = dbtool_postgresql_error(
        DBTOOL_STATUS_SQL_ERROR, "write-record", (int)PQresultStatus(result),
        "execute PostgreSQL insert", PQresultErrorMessage(result), error);
    PQclear(result);
    sink->failed = 1;
    return status;
  }
  PQclear(result);
  return DBTOOL_STATUS_OK;
}

static dbtool_status dbtool_postgresql_sink_commit(void *context,
                                                   dbtool_error *error) {
  dbtool_postgresql_sink_t *sink = (dbtool_postgresql_sink_t *)context;
  dbtool_status status;
  dbtool_error_init(error);
  if (sink == NULL || !sink->active || sink->failed)
    return dbtool_postgresql_record_fail(
        error, DBTOOL_STATUS_INVALID_ARGUMENT, "commit-records",
        "invalid PostgreSQL record sink state");
  status = dbtool_postgresql_command(
      sink->connection, "COMMIT", "commit-records",
      "commit PostgreSQL record transaction", error);
  if (status != DBTOOL_STATUS_OK) {
    sink->failed = 1;
    return status;
  }
  sink->active = 0;
  return DBTOOL_STATUS_OK;
}

static dbtool_status dbtool_postgresql_sink_rollback(void *context,
                                                     dbtool_error *error) {
  dbtool_postgresql_sink_t *sink = (dbtool_postgresql_sink_t *)context;
  dbtool_status status;
  dbtool_error_init(error);
  if (sink == NULL || !sink->active)
    return dbtool_postgresql_record_fail(
        error, DBTOOL_STATUS_INVALID_ARGUMENT, "rollback-records",
        "invalid PostgreSQL record sink state");
  status = dbtool_postgresql_command(
      sink->connection, "ROLLBACK", "rollback-records",
      "rollback PostgreSQL record transaction", error);
  if (status == DBTOOL_STATUS_OK) sink->active = 0;
  return status;
}

static void dbtool_postgresql_sink_close(void *context) {
  dbtool_postgresql_sink_t *sink = (dbtool_postgresql_sink_t *)context;
  dbtool_error ignored = DBTOOL_ERROR_INIT;
  if (sink == NULL) return;
  if (sink->active)
    (void)dbtool_postgresql_command(
        sink->connection, "ROLLBACK", "rollback-records",
        "rollback PostgreSQL record transaction", &ignored);
  dbtool_postgresql_sink_free(sink);
}

static int dbtool_postgresql_copy_parse_text(const char *value, int length,
                                             char *buffer,
                                             size_t capacity) {
  if (value == NULL || length <= 0 || (size_t)length >= capacity ||
      memchr(value, '\0', (size_t)length) != NULL)
    return 0;
  memcpy(buffer, value, (size_t)length);
  buffer[length] = '\0';
  return 1;
}

static int dbtool_postgresql_parse_int64(const char *value, int length,
                                         int64_t *out) {
  char buffer[DBTOOL_PG_PARSE_CAPACITY];
  char *end;
  intmax_t parsed;
  if (!dbtool_postgresql_copy_parse_text(value, length, buffer,
                                          sizeof(buffer)) ||
      buffer[0] == '+' || (length > 1 && buffer[0] == '0') ||
      (length > 1 && buffer[0] == '-' && buffer[1] == '0'))
    return 0;
  errno = 0;
  parsed = strtoimax(buffer, &end, 10);
  if (errno == ERANGE || end != buffer + length || parsed < INT64_MIN ||
      parsed > INT64_MAX)
    return 0;
  *out = (int64_t)parsed;
  return 1;
}

static int dbtool_postgresql_parse_uint64(const char *value, int length,
                                          uint64_t *out) {
  char buffer[DBTOOL_PG_PARSE_CAPACITY];
  char *end;
  uintmax_t parsed;
  if (!dbtool_postgresql_copy_parse_text(value, length, buffer,
                                          sizeof(buffer)) ||
      buffer[0] == '+' || buffer[0] == '-' ||
      (length > 1 && buffer[0] == '0'))
    return 0;
  errno = 0;
  parsed = strtoumax(buffer, &end, 10);
  if (errno == ERANGE || end != buffer + length || parsed > UINT64_MAX)
    return 0;
  *out = (uint64_t)parsed;
  return 1;
}

static int dbtool_postgresql_parse_double(const char *value, int length,
                                          dbtool_storage_kind storage,
                                          double *out) {
  char buffer[DBTOOL_PG_PARSE_CAPACITY];
  char *end;
  double parsed;
  if (!dbtool_postgresql_copy_parse_text(value, length, buffer,
                                          sizeof(buffer)))
    return 0;
  errno = 0;
  parsed = strtod(buffer, &end);
  if (errno == ERANGE || end != buffer + length || !isfinite(parsed) ||
      (storage == DBTOOL_STORAGE_FLOAT32 && fabs(parsed) > FLT_MAX))
    return 0;
  *out = parsed;
  return 1;
}

static void dbtool_postgresql_source_release_row(
    dbtool_postgresql_source_t *source) {
  size_t index;
  for (index = 0u; index < source->table->column_count; ++index) {
    if (source->owned_bytes[index] != NULL)
      PQfreemem(source->owned_bytes[index]);
    source->owned_bytes[index] = NULL;
  }
  if (source->row_result != NULL) PQclear(source->row_result);
  source->row_result = NULL;
}

static void dbtool_postgresql_source_drain(dbtool_postgresql_source_t *source) {
  PGresult *result;
  dbtool_postgresql_source_release_row(source);
  while ((result = PQgetResult(source->connection)) != NULL) PQclear(result);
}

static dbtool_status dbtool_postgresql_source_cell(
    dbtool_postgresql_source_t *source, size_t index, dbtool_error *error) {
  const dbtool_column_v1 *column = &source->table->columns[index];
  dbtool_cell *cell = &source->cells[index];
  const char *value;
  int length;
  turbo_uuid_t uuid;
  if (PQgetisnull(source->row_result, 0, (int)index)) {
    if (!dbtool_postgresql_column_allows_null(column))
      return dbtool_postgresql_record_fail(
          error, DBTOOL_STATUS_SQL_ERROR, "read-record",
          "non-optional PostgreSQL column is null");
    cell->kind = DBTOOL_VALUE_NULL;
    return DBTOOL_STATUS_OK;
  }
  value = PQgetvalue(source->row_result, 0, (int)index);
  length = PQgetlength(source->row_result, 0, (int)index);
  switch (column->scalar_kind) {
    case DBTOOL_SCALAR_INT64:
      if (dbtool_postgresql_parse_int64(value, length,
                                        &cell->data.int64_value)) {
        if ((column->storage_kind == DBTOOL_STORAGE_INTEGER16 &&
             (cell->data.int64_value < INT16_MIN ||
              cell->data.int64_value > INT16_MAX)) ||
            (column->storage_kind == DBTOOL_STORAGE_INTEGER32 &&
             (cell->data.int64_value < INT32_MIN ||
              cell->data.int64_value > INT32_MAX)))
          break;
        cell->kind = DBTOOL_VALUE_INT64;
        return DBTOOL_STATUS_OK;
      }
      break;
    case DBTOOL_SCALAR_UINT64:
      if (dbtool_postgresql_parse_uint64(value, length,
                                         &cell->data.uint64_value)) {
        if ((column->storage_kind == DBTOOL_STORAGE_INTEGER16 &&
             cell->data.uint64_value > INT16_MAX) ||
            (column->storage_kind == DBTOOL_STORAGE_INTEGER32 &&
             cell->data.uint64_value > INT32_MAX) ||
            (column->storage_kind == DBTOOL_STORAGE_INTEGER64 &&
             cell->data.uint64_value > INT64_MAX))
          break;
        cell->kind = DBTOOL_VALUE_UINT64;
        return DBTOOL_STATUS_OK;
      }
      return dbtool_postgresql_record_fail(
          error, DBTOOL_STATUS_SQL_ERROR, "read-record",
          "stored value is not canonical uint64 text");
    case DBTOOL_SCALAR_DOUBLE:
      if (dbtool_postgresql_parse_double(value, length, column->storage_kind,
                                         &cell->data.double_value)) {
        cell->kind = DBTOOL_VALUE_DOUBLE;
        return DBTOOL_STATUS_OK;
      }
      break;
    case DBTOOL_SCALAR_BOOLEAN:
      if (length == 1 && value != NULL && (value[0] == 't' || value[0] == 'f')) {
        cell->kind = DBTOOL_VALUE_BOOLEAN;
        cell->data.boolean_value = value[0] == 't';
        return DBTOOL_STATUS_OK;
      }
      break;
    case DBTOOL_SCALAR_TEXT:
      if (value != NULL && length >= 0 &&
          memchr(value, '\0', (size_t)length) == NULL) {
        cell->kind = DBTOOL_VALUE_TEXT;
        cell->data.bytes.data = (const unsigned char *)value;
        cell->data.bytes.size = (size_t)length;
        return DBTOOL_STATUS_OK;
      }
      break;
    case DBTOOL_SCALAR_BYTES:
      if (value != NULL && length >= 0) {
        size_t decoded_size = 0u;
        unsigned char *decoded =
            PQunescapeBytea((const unsigned char *)value, &decoded_size);
        if (decoded != NULL) {
          source->owned_bytes[index] = decoded;
          cell->kind = DBTOOL_VALUE_BYTES;
          cell->data.bytes.data = decoded;
          cell->data.bytes.size = decoded_size;
          return DBTOOL_STATUS_OK;
        }
      }
      break;
    case DBTOOL_SCALAR_UUID:
      if (length == TURBO_UUID_STRING_SIZE - 1 && value != NULL) {
        char text[TURBO_UUID_STRING_SIZE];
        memcpy(text, value, (size_t)length);
        text[length] = '\0';
        if (turbo_uuid_parse(text, &uuid) == TURBO_OK) {
          memcpy(source->uuid_storage + index * TURBO_UUID_SIZE, uuid.bytes,
                 TURBO_UUID_SIZE);
          cell->kind = DBTOOL_VALUE_UUID;
          cell->data.bytes.data =
              source->uuid_storage + index * TURBO_UUID_SIZE;
          cell->data.bytes.size = TURBO_UUID_SIZE;
          return DBTOOL_STATUS_OK;
        }
      }
      break;
    default:
      break;
  }
  return dbtool_postgresql_record_fail(
      error, DBTOOL_STATUS_SQL_ERROR, "read-record",
      "stored PostgreSQL value does not match generated column");
}

static dbtool_status dbtool_postgresql_source_open(
    void *factory_context, void **out_context, const dbtool_model_v1 *model,
    size_t table_index, dbtool_error *error) {
  PGconn *connection = dbtool_postgresql_native_connection(factory_context);
  const dbtool_table_v1 *table = dbtool_model_table_valid(model, table_index);
  dbtool_postgresql_source_t *source;
  sds sql;
  if (out_context != NULL) *out_context = NULL;
  dbtool_error_init(error);
  if (out_context == NULL || connection == NULL || table == NULL ||
      table->column_count > (size_t)INT_MAX ||
      table->column_count > SIZE_MAX / TURBO_UUID_SIZE ||
      table->column_count > SIZE_MAX / sizeof(unsigned char *))
    return dbtool_postgresql_record_fail(
        error, DBTOOL_STATUS_INVALID_ARGUMENT, "open-records",
        "invalid PostgreSQL record source state");
  source = (dbtool_postgresql_source_t *)calloc(1u, sizeof(*source));
  if (source == NULL)
    return dbtool_postgresql_record_fail(
        error, DBTOOL_STATUS_OUT_OF_MEMORY, "open-records",
        "allocate PostgreSQL record source");
  source->cells =
      (dbtool_cell *)calloc(table->column_count, sizeof(*source->cells));
  source->uuid_storage =
      (unsigned char *)calloc(table->column_count, TURBO_UUID_SIZE);
  source->owned_bytes = (unsigned char **)calloc(
      table->column_count, sizeof(*source->owned_bytes));
  if (source->cells == NULL || source->uuid_storage == NULL ||
      source->owned_bytes == NULL) {
    free(source->owned_bytes);
    free(source->uuid_storage);
    free(source->cells);
    free(source);
    return dbtool_postgresql_record_fail(
        error, DBTOOL_STATUS_OUT_OF_MEMORY, "open-records",
        "allocate PostgreSQL record views");
  }
  source->connection = connection;
  source->table = table;
  sql = dbtool_postgresql_select_sql(table);
  if (sql == NULL) {
    free(source->owned_bytes);
    free(source->uuid_storage);
    free(source->cells);
    free(source);
    return dbtool_postgresql_record_fail(
        error, DBTOOL_STATUS_OUT_OF_MEMORY, "open-records",
        "build PostgreSQL select statement");
  }
  if (PQsendQueryParams(connection, sql, 0, NULL, NULL, NULL, NULL, 0) != 1) {
    dbtool_status status = dbtool_postgresql_error(
        DBTOOL_STATUS_CONNECTION_ERROR, "open-records", 0,
        "send PostgreSQL select", PQerrorMessage(connection), error);
    sdsfree(sql);
    free(source->owned_bytes);
    free(source->uuid_storage);
    free(source->cells);
    free(source);
    return status;
  }
  sdsfree(sql);
  if (PQsetSingleRowMode(connection) != 1) {
    dbtool_postgresql_source_drain(source);
    free(source->owned_bytes);
    free(source->uuid_storage);
    free(source->cells);
    free(source);
    return dbtool_postgresql_record_fail(
        error, DBTOOL_STATUS_CONNECTION_ERROR, "open-records",
        "enable PostgreSQL single-row mode");
  }
  *out_context = source;
  return DBTOOL_STATUS_OK;
}

static dbtool_record_step dbtool_postgresql_source_next(
    void *context, dbtool_record_view *out, dbtool_error *error) {
  dbtool_postgresql_source_t *source = (dbtool_postgresql_source_t *)context;
  ExecStatusType status;
  size_t index;
  dbtool_error_init(error);
  if (out != NULL) *out = (dbtool_record_view){0};
  if (source == NULL || out == NULL || source->finished || source->failed) {
    dbtool_postgresql_record_fail(
        error, DBTOOL_STATUS_INVALID_ARGUMENT, "read-record",
        "invalid PostgreSQL record source state");
    return DBTOOL_RECORD_ERROR;
  }
  dbtool_postgresql_source_release_row(source);
  source->row_result = PQgetResult(source->connection);
  if (source->row_result == NULL) {
    source->failed = 1;
    (void)dbtool_postgresql_error(
        DBTOOL_STATUS_CONNECTION_ERROR, "read-record", 0,
        "receive PostgreSQL row", PQerrorMessage(source->connection), error);
    return DBTOOL_RECORD_ERROR;
  }
  status = PQresultStatus(source->row_result);
  if (status == PGRES_TUPLES_OK) {
    dbtool_postgresql_source_drain(source);
    source->finished = 1;
    return DBTOOL_RECORD_DONE;
  }
  if (status != PGRES_SINGLE_TUPLE || PQntuples(source->row_result) != 1 ||
      PQnfields(source->row_result) != (int)source->table->column_count) {
    (void)dbtool_postgresql_error(
        DBTOOL_STATUS_SQL_ERROR, "read-record", (int)status,
        "receive PostgreSQL row", PQresultErrorMessage(source->row_result),
        error);
    source->failed = 1;
    dbtool_postgresql_source_drain(source);
    return DBTOOL_RECORD_ERROR;
  }
  memset(source->cells, 0,
         source->table->column_count * sizeof(*source->cells));
  for (index = 0u; index < source->table->column_count; ++index) {
    if (dbtool_postgresql_source_cell(source, index, error) !=
        DBTOOL_STATUS_OK) {
      source->failed = 1;
      dbtool_postgresql_source_drain(source);
      return DBTOOL_RECORD_ERROR;
    }
  }
  out->cells = source->cells;
  out->cell_count = source->table->column_count;
  return DBTOOL_RECORD_ROW;
}

static void dbtool_postgresql_source_close(void *context) {
  dbtool_postgresql_source_t *source = (dbtool_postgresql_source_t *)context;
  if (source == NULL) return;
  if (!source->finished) dbtool_postgresql_source_drain(source);
  else dbtool_postgresql_source_release_row(source);
  free(source->owned_bytes);
  free(source->uuid_storage);
  free(source->cells);
  free(source);
}

static const dbtool_record_sink_ops DBTOOL_POSTGRESQL_SINK_OPS = {
    sizeof(dbtool_record_sink_ops), DBTOOL_RECORD_ABI_VERSION,
    dbtool_postgresql_sink_begin, dbtool_postgresql_sink_write,
    dbtool_postgresql_sink_commit, dbtool_postgresql_sink_rollback,
    dbtool_postgresql_sink_close};

static const dbtool_record_source_ops DBTOOL_POSTGRESQL_SOURCE_OPS = {
    sizeof(dbtool_record_source_ops), DBTOOL_RECORD_ABI_VERSION,
    dbtool_postgresql_source_open, dbtool_postgresql_source_next,
    dbtool_postgresql_source_close};

const dbtool_record_sink_ops *dbtool_postgresql_record_sink(void) {
  return &DBTOOL_POSTGRESQL_SINK_OPS;
}

const dbtool_record_source_ops *dbtool_postgresql_record_source(void) {
  return &DBTOOL_POSTGRESQL_SOURCE_OPS;
}
