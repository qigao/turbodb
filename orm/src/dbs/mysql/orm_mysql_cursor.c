#include "orm_mysql_cursor.h"
#include "orm_text_token.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum orm_mysql_reader_phase {
  ORM_MYSQL_READER_MAP_BEGIN = 0,
  ORM_MYSQL_READER_KEY,
  ORM_MYSQL_READER_VALUE,
  ORM_MYSQL_READER_MAP_END,
  ORM_MYSQL_READER_DONE
} orm_mysql_reader_phase;

typedef struct orm_mysql_cursor_state orm_mysql_cursor_state;

typedef struct orm_mysql_reader_state {
  orm_mysql_cursor_state *cursor;
  size_t column;
  orm_mysql_reader_phase phase;
} orm_mysql_reader_state;

struct orm_mysql_cursor_state {
  MYSQL_STMT *statement;
  MYSQL_RES *metadata;
  MYSQL_FIELD *fields;
  MYSQL_BIND *binds;
  unsigned long *lengths;
  bool *is_null;
  bool *errors;
  unsigned char **buffers;
  unsigned char **expanded;
  size_t *capacities;
  size_t column_count;
  uint64_t max_result_rows;
  size_t max_result_bytes;
  uint64_t result_rows;
  size_t result_bytes;
  int terminal;
  orm_mysql_reader_state reader;
  char error_message[ORM_C_ERROR_MESSAGE_CAPACITY];
};

static void orm_mysql_set_error(orm_error_t *error, orm_status_t status,
                                const char *message) {
  if (error == NULL || error->struct_size < sizeof(*error))
    return;
  error->status = status;
  if (status == ORM_STATUS_OK) {
    error->message[0] = '\0';
    return;
  }
  (void)snprintf(error->message, sizeof(error->message), "%s",
                 message != NULL && message[0] != '\0'
                     ? message : orm_status_message(status));
}

static int orm_mysql_binary_type(enum enum_field_types type) {
  return type == MYSQL_TYPE_TINY_BLOB ||
         type == MYSQL_TYPE_MEDIUM_BLOB ||
         type == MYSQL_TYPE_LONG_BLOB ||
         type == MYSQL_TYPE_BLOB ||
         type == MYSQL_TYPE_GEOMETRY ||
         type == MYSQL_TYPE_BIT;
}

static cserde_status orm_mysql_emit_value(
    const MYSQL_FIELD *field, const unsigned char *data, size_t size,
    cserde_token *out) {
  if (field == NULL || out == NULL || (size != 0u && data == NULL))
    return CSERDE_INVALID_ARGUMENT;

  switch (field->type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_YEAR:
      return (field->flags & UNSIGNED_FLAG) != 0u
                 ? orm_text_token_uint(data, size, out)
                 : orm_text_token_sint(data, size, out);
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
      return orm_text_token_float(data, size, 0, out);
    default:
      if (orm_mysql_binary_type(field->type)) {
        out->kind = CSERDE_BYTES;
      } else {
        out->kind = CSERDE_STRING;
      }
      out->value.slice.data = data;
      out->value.slice.size = size;
      out->value.slice.lifetime = CSERDE_VIEW_TRANSIENT;
      return CSERDE_OK;
  }
}

static cserde_status orm_mysql_reader_next(void *context, cserde_token *out) {
  orm_mysql_reader_state *reader = (orm_mysql_reader_state *)context;
  orm_mysql_cursor_state *state;
  if (reader == NULL || out == NULL || reader->cursor == NULL)
    return CSERDE_INVALID_ARGUMENT;
  state = reader->cursor;
  memset(out, 0, sizeof(*out));

  switch (reader->phase) {
    case ORM_MYSQL_READER_MAP_BEGIN:
      out->kind = CSERDE_MAP_BEGIN;
      reader->phase = state->column_count == 0u
                          ? ORM_MYSQL_READER_MAP_END
                          : ORM_MYSQL_READER_KEY;
      return CSERDE_OK;
    case ORM_MYSQL_READER_KEY: {
      const MYSQL_FIELD *field = &state->fields[reader->column];
      if (field->name == NULL)
        return CSERDE_SOURCE_ERROR;
      out->kind = CSERDE_STRING;
      out->value.slice.data = (const unsigned char *)field->name;
      out->value.slice.size = field->name_length;
      out->value.slice.lifetime = CSERDE_VIEW_TRANSIENT;
      reader->phase = ORM_MYSQL_READER_VALUE;
      return CSERDE_OK;
    }
    case ORM_MYSQL_READER_VALUE:
      if (state->is_null[reader->column]) {
        out->kind = CSERDE_NULL;
      } else if (orm_mysql_emit_value(
                     &state->fields[reader->column],
                     state->expanded[reader->column] != NULL
                         ? state->expanded[reader->column]
                         : state->buffers[reader->column],
                     (size_t)state->lengths[reader->column],
                     out) != CSERDE_OK) {
        return CSERDE_SOURCE_ERROR;
      }
      ++reader->column;
      reader->phase = reader->column == state->column_count
                          ? ORM_MYSQL_READER_MAP_END
                          : ORM_MYSQL_READER_KEY;
      return CSERDE_OK;
    case ORM_MYSQL_READER_MAP_END:
      out->kind = CSERDE_MAP_END;
      reader->phase = ORM_MYSQL_READER_DONE;
      return CSERDE_OK;
    case ORM_MYSQL_READER_DONE:
      return CSERDE_DONE;
    default:
      return CSERDE_INVALID_STATE;
  }
}

static const cserde_reader_ops orm_mysql_reader_ops = {
    sizeof(cserde_reader_ops), CSERDE_READER_OPS_ABI_VERSION,
    orm_mysql_reader_next};

static orm_row_cursor_step orm_mysql_cursor_error(
    orm_mysql_cursor_state *state, orm_status_t status,
    const char *message) {
  orm_row_cursor_step step = ORM_ROW_CURSOR_STEP_INIT;
  state->terminal = 1;
  (void)snprintf(state->error_message, sizeof(state->error_message), "%s",
                 message != NULL && message[0] != '\0'
                     ? message : orm_status_message(status));
  step.kind = ORM_ROW_CURSOR_ERROR;
  step.status = status;
  step.message = state->error_message;
  return step;
}

static orm_status_t orm_mysql_expand_column(
    orm_mysql_cursor_state *state, size_t column) {
  MYSQL_BIND fetch;
  unsigned char *next;
  const size_t length = (size_t)state->lengths[column];
  if (length <= state->capacities[column])
    return ORM_STATUS_OK;

  next = (unsigned char *)malloc(length != 0u ? length : 1u);
  if (next == NULL)
    return ORM_STATUS_OUT_OF_MEMORY;
  free(state->expanded[column]);
  state->expanded[column] = next;

  fetch = state->binds[column];
  fetch.buffer = next;
  fetch.buffer_length = (unsigned long)length;
  fetch.length = &state->lengths[column];
  fetch.is_null = &state->is_null[column];
  fetch.error = &state->errors[column];
  if (mysql_stmt_fetch_column(
          state->statement, &fetch, (unsigned int)column, 0u) != 0)
    return ORM_STATUS_DATASTORE_ERROR;
  return ORM_STATUS_OK;
}

static orm_row_cursor_step orm_mysql_cursor_next(
    void *context, cserde_reader *out_row) {
  orm_mysql_cursor_state *state = (orm_mysql_cursor_state *)context;
  orm_row_cursor_step step = ORM_ROW_CURSOR_STEP_INIT;
  int fetch_status;
  size_t row_bytes = 0u;
  size_t column;

  if (state == NULL || out_row == NULL) {
    step.kind = ORM_ROW_CURSOR_ERROR;
    step.status = ORM_STATUS_INVALID_ARGUMENT;
    step.message = "invalid MySQL cursor next";
    return step;
  }
  if (state->terminal) {
    step.kind = ORM_ROW_CURSOR_DONE;
    return step;
  }
  if (state->column_count == 0u) {
    state->terminal = 1;
    step.kind = ORM_ROW_CURSOR_DONE;
    return step;
  }

  for (column = 0u; column < state->column_count; ++column) {
    free(state->expanded[column]);
    state->expanded[column] = NULL;
  }
  memset(state->is_null, 0, state->column_count * sizeof(*state->is_null));
  memset(state->errors, 0, state->column_count * sizeof(*state->errors));
  fetch_status = mysql_stmt_fetch(state->statement);
  if (fetch_status == MYSQL_NO_DATA) {
    state->terminal = 1;
    step.kind = ORM_ROW_CURSOR_DONE;
    return step;
  }
  if (fetch_status != 0 && fetch_status != MYSQL_DATA_TRUNCATED)
    return orm_mysql_cursor_error(
        state, ORM_STATUS_DATASTORE_ERROR, mysql_stmt_error(state->statement));
  if (state->result_rows >= state->max_result_rows)
    return orm_mysql_cursor_error(
        state, ORM_STATUS_LIMIT_EXCEEDED,
        "MySQL result row count exceeds configured bounds");

  for (column = 0u; column < state->column_count; ++column) {
    orm_status_t status;
    size_t length;
    if (state->is_null[column])
      continue;
    length = (size_t)state->lengths[column];
    if (row_bytes > state->max_result_bytes ||
        length > state->max_result_bytes - row_bytes)
      return orm_mysql_cursor_error(
          state, ORM_STATUS_LIMIT_EXCEEDED,
          "MySQL result row bytes exceed configured bounds");
    row_bytes += length;
    if (state->errors[column] || length > state->capacities[column]) {
      status = orm_mysql_expand_column(state, column);
      if (status != ORM_STATUS_OK)
        return orm_mysql_cursor_error(
            state, status,
            status == ORM_STATUS_OUT_OF_MEMORY
                ? "allocate MySQL result column"
                : mysql_stmt_error(state->statement));
    }
  }
  if (state->result_bytes > state->max_result_bytes ||
      row_bytes > state->max_result_bytes - state->result_bytes)
    return orm_mysql_cursor_error(
        state, ORM_STATUS_LIMIT_EXCEEDED,
        "MySQL cumulative result bytes exceed configured bounds");

  state->result_bytes += row_bytes;
  ++state->result_rows;
  state->reader.cursor = state;
  state->reader.column = 0u;
  state->reader.phase = ORM_MYSQL_READER_MAP_BEGIN;
  if (cserde_reader_init(out_row, &orm_mysql_reader_ops, &state->reader) !=
      CSERDE_OK)
    return orm_mysql_cursor_error(
        state, ORM_STATUS_INTERNAL_ERROR,
        "initialize MySQL row reader");
  step.kind = ORM_ROW_CURSOR_ROW;
  return step;
}

static void orm_mysql_cursor_cancel(void *context) {
  orm_mysql_cursor_state *state = (orm_mysql_cursor_state *)context;
  if (state == NULL || state->terminal)
    return;
  state->terminal = 1;
  if (state->statement != NULL)
    (void)mysql_stmt_free_result(state->statement);
}

static void orm_mysql_cursor_destroy(void *context) {
  orm_mysql_cursor_state *state = (orm_mysql_cursor_state *)context;
  size_t column;
  if (state == NULL)
    return;
  if (state->statement != NULL)
    (void)mysql_stmt_close(state->statement);
  if (state->metadata != NULL)
    mysql_free_result(state->metadata);
  for (column = 0u; column < state->column_count; ++column) {
    free(state->buffers[column]);
    free(state->expanded[column]);
  }
  free(state->buffers);
  free(state->expanded);
  free(state->capacities);
  free(state->binds);
  free(state->lengths);
  free(state->is_null);
  free(state->errors);
  free(state);
}

static orm_status_t orm_mysql_cursor_column_count(
    void *context, uint64_t *out_count) {
  const orm_mysql_cursor_state *state =
      (const orm_mysql_cursor_state *)context;
  if (state == NULL || out_count == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;
  *out_count = (uint64_t)state->column_count;
  return ORM_STATUS_OK;
}

static const orm_row_cursor_ops orm_mysql_cursor_ops = {
    sizeof(orm_row_cursor_ops), ORM_ROW_CURSOR_OPS_ABI_VERSION,
    "mysql", orm_mysql_cursor_next, orm_mysql_cursor_cancel,
    orm_mysql_cursor_destroy, NULL, orm_mysql_cursor_column_count};

orm_status_t orm_mysql_cursor_from_statement(
    orm_row_cursor *out_cursor,
    MYSQL_STMT **statement,
    const orm_mysql_cursor_config *config,
    orm_error_t *error) {
  enum { ORM_MYSQL_INITIAL_COLUMN_BYTES = 256u };
  orm_mysql_cursor_state *state;
  size_t column;

  if (out_cursor == NULL || out_cursor->ops != NULL ||
      out_cursor->context != NULL || statement == NULL ||
      *statement == NULL || config == NULL ||
      config->max_columns == 0u || config->max_result_rows == 0u ||
      config->max_result_bytes == 0u) {
    orm_mysql_set_error(error, ORM_STATUS_INVALID_ARGUMENT,
                        "invalid MySQL cursor");
    return ORM_STATUS_INVALID_ARGUMENT;
  }

  state = (orm_mysql_cursor_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    orm_mysql_set_error(error, ORM_STATUS_OUT_OF_MEMORY,
                        "allocate MySQL cursor");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  state->statement = *statement;
  state->max_result_rows = config->max_result_rows;
  state->max_result_bytes = config->max_result_bytes;
  state->column_count = (size_t)mysql_stmt_field_count(state->statement);
  if (config->affected_rows != NULL)
    *config->affected_rows = mysql_stmt_affected_rows(state->statement);

  if (state->column_count > config->max_columns) {
    orm_mysql_cursor_destroy(state);
    orm_mysql_set_error(error, ORM_STATUS_LIMIT_EXCEEDED,
                        "MySQL result column count exceeds configured bounds");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  if (state->column_count != 0u) {
    state->metadata = mysql_stmt_result_metadata(state->statement);
    if (state->metadata == NULL) {
      char diagnostic[ORM_C_ERROR_MESSAGE_CAPACITY];
      (void)snprintf(diagnostic, sizeof(diagnostic), "%s",
                     mysql_stmt_error(state->statement));
      orm_mysql_cursor_destroy(state);
      orm_mysql_set_error(error, ORM_STATUS_DATASTORE_ERROR, diagnostic);
      return ORM_STATUS_DATASTORE_ERROR;
    }
    state->fields = mysql_fetch_fields(state->metadata);
    if (state->fields == NULL) {
      orm_mysql_cursor_destroy(state);
      orm_mysql_set_error(error, ORM_STATUS_DATASTORE_ERROR,
                          "MySQL result metadata has no fields");
      return ORM_STATUS_DATASTORE_ERROR;
    }

    state->binds = (MYSQL_BIND *)calloc(
        state->column_count, sizeof(*state->binds));
    state->lengths = (unsigned long *)calloc(
        state->column_count, sizeof(*state->lengths));
    state->is_null = (bool *)calloc(
        state->column_count, sizeof(*state->is_null));
    state->errors = (bool *)calloc(
        state->column_count, sizeof(*state->errors));
    state->buffers = (unsigned char **)calloc(
        state->column_count, sizeof(*state->buffers));
    state->expanded = (unsigned char **)calloc(
        state->column_count, sizeof(*state->expanded));
    state->capacities = (size_t *)calloc(
        state->column_count, sizeof(*state->capacities));
    if (state->binds == NULL || state->lengths == NULL ||
        state->is_null == NULL || state->errors == NULL ||
        state->buffers == NULL || state->expanded == NULL ||
        state->capacities == NULL) {
      orm_mysql_cursor_destroy(state);
      orm_mysql_set_error(error, ORM_STATUS_OUT_OF_MEMORY,
                          "allocate MySQL result bindings");
      return ORM_STATUS_OUT_OF_MEMORY;
    }

    for (column = 0u; column < state->column_count; ++column) {
      const size_t capacity =
          state->max_result_bytes < ORM_MYSQL_INITIAL_COLUMN_BYTES
              ? state->max_result_bytes
              : ORM_MYSQL_INITIAL_COLUMN_BYTES;
      state->buffers[column] =
          (unsigned char *)malloc(capacity != 0u ? capacity : 1u);
      if (state->buffers[column] == NULL) {
        orm_mysql_cursor_destroy(state);
        orm_mysql_set_error(error, ORM_STATUS_OUT_OF_MEMORY,
                            "allocate MySQL result buffer");
        return ORM_STATUS_OUT_OF_MEMORY;
      }
      state->capacities[column] = capacity;
      state->binds[column].buffer_type = MYSQL_TYPE_STRING;
      state->binds[column].buffer = state->buffers[column];
      state->binds[column].buffer_length = (unsigned long)capacity;
      state->binds[column].length = &state->lengths[column];
      state->binds[column].is_null = &state->is_null[column];
      state->binds[column].error = &state->errors[column];
    }
    if (mysql_stmt_bind_result(state->statement, state->binds)) {
      char diagnostic[ORM_C_ERROR_MESSAGE_CAPACITY];
      (void)snprintf(diagnostic, sizeof(diagnostic), "%s",
                     mysql_stmt_error(state->statement));
      orm_mysql_cursor_destroy(state);
      orm_mysql_set_error(error, ORM_STATUS_DATASTORE_ERROR, diagnostic);
      return ORM_STATUS_DATASTORE_ERROR;
    }
  }

  out_cursor->ops = &orm_mysql_cursor_ops;
  out_cursor->context = state;
  *statement = NULL;
  orm_mysql_set_error(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}
