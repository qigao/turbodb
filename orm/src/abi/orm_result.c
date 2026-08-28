#include "orm_internal.h"

#include <cserde/reader.h>

#include <stdlib.h>
#include <string.h>

typedef struct orm_result_cell {
  orm_value_kind_t kind;
  union {
    int64_t int64_value;
    uint64_t uint64_value;
    double double_value;
    uint8_t boolean_value;
  } data;
  tstr bytes;
} orm_result_cell;

struct orm_result {
  vec_t cells;
  uint64_t row_count;
  uint64_t column_count;
  uint64_t affected_rows;
  uint64_t payload_bytes;
};

static orm_status_t orm_result_fail(orm_error_t *error, orm_status_t status,
                                    const char *message) {
  orm_error_set(error, status, message);
  return status;
}

static void orm_result_cell_destroy(orm_result_cell *cell) {
  if (cell == NULL)
    return;
  tstr_freep(&cell->bytes);
  memset(cell, 0, sizeof(*cell));
}

void ORM_C_CALL orm_result_destroy(orm_result_t *result) {
  size_t index;
  if (result == NULL)
    return;
  for (index = 0u; index < vec_size(&result->cells); ++index)
    orm_result_cell_destroy((orm_result_cell *)vec_at(&result->cells, index));
  vec_destroy(&result->cells);
  free(result);
}

static orm_status_t orm_result_create(const orm_limits *limits,
                                      orm_result_t **out_result,
                                      orm_error_t *error) {
  orm_result_t *result;
  size_t row_limit;
  size_t cell_limit;
  if (limits == NULL || out_result == NULL || limits->max_columns == 0u ||
      limits->max_result_rows == 0u ||
      limits->max_result_rows > (uint64_t)SIZE_MAX) {
    return orm_result_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                           "invalid ORM result bounds");
  }
  row_limit = (size_t)limits->max_result_rows;
  if (row_limit > SIZE_MAX / limits->max_columns)
    cell_limit = SIZE_MAX;
  else
    cell_limit = row_limit * limits->max_columns;
  result = (orm_result_t *)calloc(1u, sizeof(*result));
  if (result == NULL)
    return orm_result_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                           "allocate ORM result");
  if (vec_init_bytes(&result->cells, sizeof(orm_result_cell),
                     _Alignof(orm_result_cell), cell_limit) != STL_OK) {
    free(result);
    return orm_result_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                           "initialize ORM result storage");
  }
  *out_result = result;
  return ORM_STATUS_OK;
}

static orm_status_t orm_result_reader_error(cserde_status status,
                                            orm_error_t *error) {
  orm_status_t mapped = ORM_STATUS_INTERNAL_ERROR;
  if (status == CSERDE_LIMIT_EXCEEDED)
    mapped = ORM_STATUS_LIMIT_EXCEEDED;
  else if (status == CSERDE_UNSUPPORTED)
    mapped = ORM_STATUS_UNSUPPORTED;
  else if (status == CSERDE_VALUE_OUT_OF_RANGE)
    mapped = ORM_STATUS_OUT_OF_RANGE;
  else if (status == CSERDE_INVALID_ARGUMENT ||
           status == CSERDE_INVALID_STATE ||
           status == CSERDE_INVALID_TOKEN ||
           status == CSERDE_UNEXPECTED_END)
    mapped = ORM_STATUS_TYPE_ERROR;
  return orm_result_fail(error, mapped, "decode ORM result row");
}

static orm_status_t orm_result_copy_cell(orm_result_t *result,
                                         const orm_limits *limits,
                                         const cserde_token *token,
                                         orm_result_cell *out_cell,
                                         orm_error_t *error) {
  uint64_t bytes = 0u;
  const void *data = NULL;
  memset(out_cell, 0, sizeof(*out_cell));
  switch (token->kind) {
    case CSERDE_NULL:
      out_cell->kind = ORM_VALUE_NULL;
      break;
    case CSERDE_BOOL:
      out_cell->kind = ORM_VALUE_BOOLEAN;
      out_cell->data.boolean_value = token->value.boolean ? 1u : 0u;
      bytes = sizeof(uint8_t);
      break;
    case CSERDE_SINT:
      out_cell->kind = ORM_VALUE_INT64;
      out_cell->data.int64_value = token->value.sint;
      bytes = sizeof(int64_t);
      break;
    case CSERDE_UINT:
      out_cell->kind = ORM_VALUE_UINT64;
      out_cell->data.uint64_value = token->value.uint;
      bytes = sizeof(uint64_t);
      break;
    case CSERDE_FLOAT:
      out_cell->kind = ORM_VALUE_DOUBLE;
      out_cell->data.double_value = token->value.floating;
      bytes = sizeof(double);
      break;
    case CSERDE_STRING:
    case CSERDE_BYTES:
      out_cell->kind = token->kind == CSERDE_STRING ? ORM_VALUE_TEXT
                                                     : ORM_VALUE_BLOB;
      data = token->value.slice.data;
      bytes = (uint64_t)token->value.slice.size;
      if (token->value.slice.size != 0u && data == NULL)
        return orm_result_fail(error, ORM_STATUS_TYPE_ERROR,
                               "ORM result contains an invalid byte view");
      break;
    default:
      return orm_result_fail(error, ORM_STATUS_TYPE_ERROR,
                             "ORM result cells must be scalar values");
  }
  if (result->payload_bytes > limits->max_result_bytes ||
      bytes > limits->max_result_bytes - result->payload_bytes)
    return orm_result_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                           "ORM result exceeds configured byte limit");
  if (out_cell->kind == ORM_VALUE_TEXT || out_cell->kind == ORM_VALUE_BLOB) {
    if (bytes > (uint64_t)SIZE_MAX)
      return orm_result_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                             "ORM result cell exceeds platform size");
    out_cell->bytes = tstr_new_len(data, (size_t)bytes);
    if (out_cell->bytes == NULL)
      return orm_result_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                             "copy ORM result cell");
  }
  result->payload_bytes += bytes;
  return ORM_STATUS_OK;
}

static void orm_result_rollback_row(orm_result_t *result, size_t row_start,
                                    uint64_t payload_start) {
  while (vec_size(&result->cells) > row_start) {
    orm_result_cell cell;
    memset(&cell, 0, sizeof(cell));
    if (vec_pop(&result->cells, &cell) != STL_OK)
      break;
    orm_result_cell_destroy(&cell);
  }
  result->payload_bytes = payload_start;
}

static orm_status_t orm_result_copy_row(orm_result_t *result,
                                        const orm_limits *limits,
                                        cserde_reader *reader,
                                        orm_error_t *error) {
  const size_t row_start = vec_size(&result->cells);
  const uint64_t payload_start = result->payload_bytes;
  uint64_t columns = 0u;
  cserde_token token;
  cserde_status reader_status;
  orm_status_t status = ORM_STATUS_OK;

  reader_status = cserde_reader_next(reader, &token);
  if (reader_status != CSERDE_OK || token.kind != CSERDE_MAP_BEGIN)
    return reader_status == CSERDE_OK
               ? orm_result_fail(error, ORM_STATUS_TYPE_ERROR,
                                 "ORM result row must be a map")
               : orm_result_reader_error(reader_status, error);
  for (;;) {
    reader_status = cserde_reader_next(reader, &token);
    if (reader_status != CSERDE_OK) {
      status = orm_result_reader_error(reader_status, error);
      break;
    }
    if (token.kind == CSERDE_MAP_END)
      break;
    if (token.kind != CSERDE_STRING) {
      status = orm_result_fail(error, ORM_STATUS_TYPE_ERROR,
                               "ORM result column name must be text");
      break;
    }
    if (columns >= (uint64_t)limits->max_columns) {
      status = orm_result_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                               "ORM result exceeds configured column limit");
      break;
    }
    reader_status = cserde_reader_next(reader, &token);
    if (reader_status != CSERDE_OK) {
      status = orm_result_reader_error(reader_status, error);
      break;
    }
    {
      orm_result_cell cell;
      status = orm_result_copy_cell(result, limits, &token, &cell, error);
      if (status != ORM_STATUS_OK)
        break;
      if (vec_push(&result->cells, &cell) != STL_OK) {
        orm_result_cell_destroy(&cell);
        status = orm_result_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                                 "grow ORM result storage");
        break;
      }
    }
    ++columns;
  }
  if (status == ORM_STATUS_OK) {
    reader_status = cserde_reader_next(reader, &token);
    if (reader_status != CSERDE_DONE)
      status = reader_status == CSERDE_OK
                   ? orm_result_fail(error, ORM_STATUS_TYPE_ERROR,
                                     "ORM result row has trailing data")
                   : orm_result_reader_error(reader_status, error);
  }
  if (status == ORM_STATUS_OK && result->row_count != 0u &&
      columns != result->column_count)
    status = orm_result_fail(error, ORM_STATUS_TYPE_ERROR,
                             "ORM result rows have inconsistent columns");
  if (status != ORM_STATUS_OK) {
    orm_result_rollback_row(result, row_start, payload_start);
    return status;
  }
  if (result->row_count == 0u)
    result->column_count = columns;
  ++result->row_count;
  return ORM_STATUS_OK;
}

static orm_status_t orm_result_materialize_rows(
    orm_query_t *query, orm_backend *database,
    orm_transaction_backend *transaction, orm_result_t *result,
    orm_error_t *error) {
  orm_row_cursor cursor = {0};
  orm_status_t status;
  bool done = false;
  status = database != NULL
               ? database->ops->open_cursor(database->context, &query->plan,
                                            &query->connection->limits,
                                            &cursor, error)
               : transaction->ops->open_cursor(transaction->context,
                                               &query->plan,
                                               &query->connection->limits,
                                               &cursor, error);
  if (status != ORM_STATUS_OK)
    return status;
  if (!orm_row_cursor_valid(&cursor)) {
    orm_row_cursor_dispose(&cursor);
    return orm_result_fail(error, ORM_STATUS_INTERNAL_ERROR,
                           "ORM backend returned an invalid cursor");
  }
  while (!done) {
    cserde_reader reader = {0};
    const orm_row_cursor_step step = cursor.ops->next(cursor.context, &reader);
    switch (step.kind) {
      case ORM_ROW_CURSOR_ROW:
      case ORM_ROW_CURSOR_ROW_AND_DONE:
        if (result->row_count >= query->connection->limits.max_result_rows)
          status = orm_result_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                                   "ORM result exceeds configured row limit");
        else
          status = orm_result_copy_row(result, &query->connection->limits,
                                       &reader, error);
        done = status != ORM_STATUS_OK ||
               step.kind == ORM_ROW_CURSOR_ROW_AND_DONE;
        break;
      case ORM_ROW_CURSOR_WAIT:
        status = orm_result_fail(
            error, ORM_STATUS_UNSUPPORTED,
            "materialized execution does not support asynchronous WAIT");
        done = true;
        break;
      case ORM_ROW_CURSOR_DONE:
        done = true;
        break;
      case ORM_ROW_CURSOR_ERROR:
        status = orm_result_fail(
            error,
            step.status == ORM_STATUS_OK ? ORM_STATUS_INTERNAL_ERROR
                                         : step.status,
            step.message != NULL ? step.message : "read ORM result cursor");
        done = true;
        break;
      default:
        status = orm_result_fail(error, ORM_STATUS_INTERNAL_ERROR,
                                 "ORM cursor returned an invalid step");
        done = true;
        break;
    }
  }
  if (status == ORM_STATUS_OK && cursor.ops->column_count != NULL) {
    uint64_t columns = 0u;
    status = cursor.ops->column_count(cursor.context, &columns);
    if (status == ORM_STATUS_OK &&
        columns > query->connection->limits.max_columns)
      status = orm_result_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                               "ORM result exceeds configured column limit");
    else if (status == ORM_STATUS_OK && result->row_count != 0u &&
             columns != result->column_count)
      status = orm_result_fail(error, ORM_STATUS_TYPE_ERROR,
                               "ORM result metadata is inconsistent");
    else if (status == ORM_STATUS_OK)
      result->column_count = columns;
  }
  orm_row_cursor_dispose(&cursor);
  return status;
}

static orm_status_t orm_result_execute(
    orm_query_t *query, orm_backend *database,
    orm_transaction_backend *transaction, orm_result_t **out_result,
    orm_error_t *error) {
  orm_result_t *result = NULL;
  orm_status_t status;
  if (out_result != NULL)
    *out_result = NULL;
  if (query == NULL || query->connection == NULL || out_result == NULL ||
      (database == NULL && transaction == NULL))
    return orm_result_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                           "invalid ORM materialized execution");
  status = orm_result_create(&query->connection->limits, &result, error);
  if (status != ORM_STATUS_OK)
    return status;
  if (query->plan.kind == ORM_QUERY_SELECT ||
      (query->plan.kind == ORM_QUERY_RAW &&
       orm_query_returns_rows(&query->plan))) {
    status = orm_result_materialize_rows(query, database, transaction, result,
                                         error);
  } else {
    status = database != NULL
                 ? database->ops->execute_command(
                       database->context, &query->plan,
                       &query->connection->limits, &result->affected_rows,
                       error)
                 : transaction->ops->execute_command(
                       transaction->context, &query->plan,
                       &query->connection->limits, &result->affected_rows,
                       error);
  }
  if (status != ORM_STATUS_OK) {
    orm_result_destroy(result);
    return status;
  }
  *out_result = result;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t ORM_C_CALL orm_query_execute(orm_query_t *query,
                                         orm_result_t **out_result,
                                         orm_error_t *error) {
  return orm_result_execute(
      query, query != NULL && query->connection != NULL
                 ? &query->connection->backend
                 : NULL,
      NULL, out_result, error);
}

orm_status_t ORM_C_CALL orm_query_execute_in_transaction(
    orm_query_t *query, orm_transaction_t *transaction,
    orm_result_t **out_result, orm_error_t *error) {
  if (out_result != NULL)
    *out_result = NULL;
  if (query == NULL || transaction == NULL ||
      transaction->state != ORM_TRANSACTION_ACTIVE ||
      query->connection != transaction->connection)
    return orm_result_fail(
        error, ORM_STATUS_INVALID_STATE,
        "query and transaction do not share an active connection");
  return orm_result_execute(query, NULL, &transaction->backend, out_result,
                            error);
}

static orm_status_t orm_result_get_count(const orm_result_t *result,
                                         uint64_t *out_count,
                                         uint64_t value,
                                         orm_error_t *error) {
  if (out_count != NULL)
    *out_count = 0u;
  if (result == NULL || out_count == NULL)
    return orm_result_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                           "invalid ORM result count access");
  *out_count = value;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t ORM_C_CALL orm_result_row_count(const orm_result_t *result,
                                             uint64_t *out_count,
                                             orm_error_t *error) {
  return orm_result_get_count(result, out_count,
                              result != NULL ? result->row_count : 0u, error);
}

orm_status_t ORM_C_CALL orm_result_column_count(const orm_result_t *result,
                                                uint64_t *out_count,
                                                orm_error_t *error) {
  return orm_result_get_count(result, out_count,
                              result != NULL ? result->column_count : 0u,
                              error);
}

orm_status_t ORM_C_CALL orm_result_affected_rows(const orm_result_t *result,
                                                 uint64_t *out_count,
                                                 orm_error_t *error) {
  return orm_result_get_count(result, out_count,
                              result != NULL ? result->affected_rows : 0u,
                              error);
}

static orm_status_t orm_result_cell_at(const orm_result_t *result,
                                       uint64_t row, uint64_t column,
                                       const orm_result_cell **out_cell,
                                       orm_error_t *error) {
  uint64_t index;
  if (out_cell != NULL)
    *out_cell = NULL;
  if (result == NULL || out_cell == NULL)
    return orm_result_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                           "invalid ORM result cell access");
  if (row >= result->row_count || column >= result->column_count)
    return orm_result_fail(error, ORM_STATUS_OUT_OF_RANGE,
                           "ORM result cell is out of range");
  if (row > UINT64_MAX / result->column_count)
    return orm_result_fail(error, ORM_STATUS_OUT_OF_RANGE,
                           "ORM result cell index overflow");
  index = row * result->column_count + column;
  if (index > (uint64_t)SIZE_MAX)
    return orm_result_fail(error, ORM_STATUS_OUT_OF_RANGE,
                           "ORM result cell exceeds platform size");
  *out_cell = (const orm_result_cell *)vec_at_const(&result->cells,
                                                    (size_t)index);
  if (*out_cell == NULL)
    return orm_result_fail(error, ORM_STATUS_INTERNAL_ERROR,
                           "ORM result storage is inconsistent");
  return ORM_STATUS_OK;
}

orm_status_t ORM_C_CALL orm_result_is_null(const orm_result_t *result,
                                           uint64_t row, uint64_t column,
                                           uint8_t *out_is_null,
                                           orm_error_t *error) {
  const orm_result_cell *cell;
  orm_status_t status;
  if (out_is_null != NULL)
    *out_is_null = 0u;
  if (out_is_null == NULL)
    return orm_result_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                           "null ORM result null output");
  status = orm_result_cell_at(result, row, column, &cell, error);
  if (status != ORM_STATUS_OK)
    return status;
  *out_is_null = cell->kind == ORM_VALUE_NULL ? 1u : 0u;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

orm_status_t ORM_C_CALL orm_result_value_kind(const orm_result_t *result,
                                              uint64_t row, uint64_t column,
                                              orm_value_kind_t *out_kind,
                                              orm_error_t *error) {
  const orm_result_cell *cell;
  orm_status_t status;
  if (out_kind != NULL)
    *out_kind = ORM_VALUE_NULL;
  if (out_kind == NULL)
    return orm_result_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                           "null ORM result kind output");
  status = orm_result_cell_at(result, row, column, &cell, error);
  if (status != ORM_STATUS_OK)
    return status;
  *out_kind = cell->kind;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

static orm_status_t orm_result_require_kind(
    const orm_result_t *result, uint64_t row, uint64_t column,
    orm_value_kind_t kind, const orm_result_cell **out_cell,
    orm_error_t *error) {
  orm_status_t status = orm_result_cell_at(result, row, column, out_cell,
                                           error);
  if (status != ORM_STATUS_OK)
    return status;
  if ((*out_cell)->kind == ORM_VALUE_NULL)
    return orm_result_fail(error, ORM_STATUS_NULL_VALUE,
                           "ORM result cell is null");
  if ((*out_cell)->kind != kind)
    return orm_result_fail(error, ORM_STATUS_TYPE_ERROR,
                           "ORM result cell has a different type");
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

#define ORM_RESULT_SCALAR_GETTER(name_, type_, kind_, member_)               \
  orm_status_t ORM_C_CALL name_(const orm_result_t *result, uint64_t row,    \
                                uint64_t column, type_ *out_value,            \
                                orm_error_t *error) {                         \
    const orm_result_cell *cell;                                              \
    orm_status_t status;                                                      \
    if (out_value != NULL)                                                    \
      *out_value = (type_)0;                                                  \
    if (out_value == NULL)                                                    \
      return orm_result_fail(error, ORM_STATUS_INVALID_ARGUMENT,              \
                             "null ORM result value output");                \
    status = orm_result_require_kind(result, row, column, kind_, &cell,       \
                                     error);                                  \
    if (status != ORM_STATUS_OK)                                              \
      return status;                                                          \
    *out_value = cell->data.member_;                                          \
    return ORM_STATUS_OK;                                                     \
  }

ORM_RESULT_SCALAR_GETTER(orm_result_get_int64, int64_t, ORM_VALUE_INT64,
                         int64_value)
ORM_RESULT_SCALAR_GETTER(orm_result_get_uint64, uint64_t, ORM_VALUE_UINT64,
                         uint64_value)
ORM_RESULT_SCALAR_GETTER(orm_result_get_double, double, ORM_VALUE_DOUBLE,
                         double_value)
ORM_RESULT_SCALAR_GETTER(orm_result_get_boolean, uint8_t, ORM_VALUE_BOOLEAN,
                         boolean_value)

orm_status_t ORM_C_CALL orm_result_get_text(const orm_result_t *result,
                                            uint64_t row, uint64_t column,
                                            orm_string_view_t *out_value,
                                            orm_error_t *error) {
  const orm_result_cell *cell;
  orm_status_t status;
  if (out_value != NULL)
    *out_value = (orm_string_view_t){0};
  if (out_value == NULL)
    return orm_result_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                           "null ORM result text output");
  status = orm_result_require_kind(result, row, column, ORM_VALUE_TEXT, &cell,
                                   error);
  if (status != ORM_STATUS_OK)
    return status;
  *out_value = tstr_to_v(cell->bytes);
  return ORM_STATUS_OK;
}

orm_status_t ORM_C_CALL orm_result_get_blob(const orm_result_t *result,
                                            uint64_t row, uint64_t column,
                                            orm_blob_t *out_value,
                                            orm_error_t *error) {
  const orm_result_cell *cell;
  orm_status_t status;
  if (out_value != NULL)
    *out_value = (orm_blob_t){0};
  if (out_value == NULL)
    return orm_result_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                           "null ORM result blob output");
  status = orm_result_require_kind(result, row, column, ORM_VALUE_BLOB, &cell,
                                   error);
  if (status != ORM_STATUS_OK)
    return status;
  out_value->data = cell->bytes;
  out_value->size = tstr_len(cell->bytes);
  return ORM_STATUS_OK;
}
