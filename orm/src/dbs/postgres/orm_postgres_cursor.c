#include "orm_postgres_cursor.h"
#include "orm_text_token.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum orm_postgres_reader_phase {
  ORM_POSTGRES_READER_MAP_BEGIN = 0,
  ORM_POSTGRES_READER_KEY,
  ORM_POSTGRES_READER_VALUE,
  ORM_POSTGRES_READER_MAP_END,
  ORM_POSTGRES_READER_DONE
} orm_postgres_reader_phase;

typedef struct orm_postgres_cursor_state orm_postgres_cursor_state;

typedef struct orm_postgres_reader_state {
  orm_postgres_cursor_state *cursor;
  size_t column;
  orm_postgres_reader_phase phase;
} orm_postgres_reader_state;

struct orm_postgres_cursor_state {
  orm_postgres_driver driver;
  orm_postgres_cursor_config config;
  size_t owned_column_count;
  uint64_t owned_affected_rows;
  orm_error_t owned_runtime_error;
  void *current_result;
  orm_postgres_reader_state reader;
  unsigned char *bytea_scratch;
  size_t bytea_capacity;
  size_t bytea_used;
  size_t result_bytes;
  uint64_t result_rows;
  size_t *bytea_offsets;
  size_t *bytea_sizes;
  int query_active;
  int terminal;
  char error_message[ORM_C_ERROR_MESSAGE_CAPACITY];
};

enum {
  ORM_POSTGRES_DECIMAL_BASE = 10u,
  ORM_POSTGRES_BOOL_OID = 16u,
  ORM_POSTGRES_BYTEA_OID = 17u,
  ORM_POSTGRES_INT8_OID = 20u,
  ORM_POSTGRES_INT2_OID = 21u,
  ORM_POSTGRES_INT4_OID = 23u,
  ORM_POSTGRES_OID_OID = 26u,
  ORM_POSTGRES_FLOAT4_OID = 700u,
  ORM_POSTGRES_FLOAT8_OID = 701u
};

static cserde_status orm_postgres_emit_text_value(
    uint32_t oid, const unsigned char *data, size_t size, cserde_token *out) {
  switch (oid) {
    case ORM_POSTGRES_BOOL_OID:
      if (size != 1u ||
          (data[0] != (unsigned char)'t' && data[0] != (unsigned char)'f'))
        return CSERDE_SOURCE_ERROR;
      out->kind = CSERDE_BOOL;
      out->value.boolean = data[0] == (unsigned char)'t';
      return CSERDE_OK;
    case ORM_POSTGRES_INT8_OID:
    case ORM_POSTGRES_INT2_OID:
    case ORM_POSTGRES_INT4_OID:
      return orm_text_token_sint(data, size, out);
    case ORM_POSTGRES_OID_OID:
      return orm_text_token_uint(data, size, out);
    case ORM_POSTGRES_FLOAT4_OID:
    case ORM_POSTGRES_FLOAT8_OID:
      return orm_text_token_float(data, size, 0, out);
    default:
      out->kind = CSERDE_STRING;
      out->value.slice.data = data;
      out->value.slice.size = size;
      out->value.slice.lifetime = CSERDE_VIEW_TRANSIENT;
      return CSERDE_OK;
  }
}

static void orm_postgres_set_error(orm_error_t *error, orm_status_t status,
                                   const char *message) {
  if (error == NULL || error->struct_size < sizeof(*error))
    return;
  error->status = status;
  if (status == ORM_STATUS_OK) {
    error->message[0] = '\0';
    return;
  }
  (void)snprintf(error->message, sizeof(error->message), "%s",
                 message != NULL ? message : orm_status_message(status));
}

static int orm_postgres_command_ops_valid(
    const orm_postgres_command_ops *ops) {
  return ops != NULL && ops->struct_size >= sizeof(*ops) &&
         ops->abi_version == ORM_POSTGRES_COMMAND_OPS_ABI_VERSION &&
         ops->send_query != NULL && ops->enable_single_row != NULL &&
         ops->next_result != NULL && ops->release_result != NULL &&
         ops->connection_error != NULL;
}

static int orm_postgres_result_ops_valid(const orm_postgres_result_ops *ops) {
  return ops != NULL && ops->struct_size >= sizeof(*ops) &&
         ops->abi_version == ORM_POSTGRES_RESULT_OPS_ABI_VERSION &&
         ops->status != NULL && ops->rows != NULL && ops->columns != NULL &&
         ops->column_name != NULL && ops->column_type != NULL &&
         ops->is_null != NULL && ops->value != NULL && ops->length != NULL &&
         ops->command_tuples != NULL && ops->error != NULL &&
         ops->sqlstate != NULL;
}

static orm_status_t orm_postgres_sqlstate_status(const char *sqlstate) {
  if (sqlstate == NULL || strlen(sqlstate) != 5u)
    return ORM_STATUS_SQL_ERROR;
  if (sqlstate[0] == '2' && sqlstate[1] == '3')
    return ORM_STATUS_CONSTRAINT;
  if (strcmp(sqlstate, "40001") == 0 || strcmp(sqlstate, "40P01") == 0 ||
      strcmp(sqlstate, "55P03") == 0)
    return ORM_STATUS_BUSY;
  return ORM_STATUS_SQL_ERROR;
}

static int orm_postgres_config_valid(
    const orm_postgres_cursor_config *config) {
  return config != NULL && config->struct_size >= sizeof(*config) &&
         config->abi_version == ORM_POSTGRES_CURSOR_CONFIG_ABI_VERSION &&
         config->max_columns != 0u && config->max_result_rows != 0u &&
         config->max_result_bytes != 0u;
}

static void orm_postgres_release_current(orm_postgres_cursor_state *state) {
  if (state->current_result == NULL)
    return;
  state->driver.command->release_result(state->current_result);
  state->current_result = NULL;
}

static void orm_postgres_drain(orm_postgres_cursor_state *state) {
  void *result;
  orm_postgres_release_current(state);
  while (state->query_active) {
    result = state->driver.command->next_result(state->driver.context);
    if (result == NULL) {
      state->query_active = 0;
      break;
    }
    state->driver.command->release_result(result);
  }
}

static cserde_status orm_postgres_reader_next(void *context,
                                               cserde_token *out) {
  orm_postgres_reader_state *reader =
      (orm_postgres_reader_state *)context;
  orm_postgres_cursor_state *state;
  const orm_postgres_result_ops *ops;
  size_t columns;

  if (reader == NULL || out == NULL || reader->cursor == NULL)
    return CSERDE_INVALID_ARGUMENT;
  state = reader->cursor;
  ops = state->driver.result;
  columns = *state->config.column_count;
  memset(out, 0, sizeof(*out));

  switch (reader->phase) {
    case ORM_POSTGRES_READER_MAP_BEGIN:
      out->kind = CSERDE_MAP_BEGIN;
      reader->phase = columns == 0u ? ORM_POSTGRES_READER_MAP_END
                                    : ORM_POSTGRES_READER_KEY;
      return CSERDE_OK;
    case ORM_POSTGRES_READER_KEY: {
      const char *name = ops->column_name(state->current_result,
                                          reader->column);
      if (name == NULL)
        return CSERDE_SOURCE_ERROR;
      out->kind = CSERDE_STRING;
      out->value.slice.data = (const unsigned char *)name;
      out->value.slice.size = strlen(name);
      out->value.slice.lifetime = CSERDE_VIEW_TRANSIENT;
      reader->phase = ORM_POSTGRES_READER_VALUE;
      return CSERDE_OK;
    }
    case ORM_POSTGRES_READER_VALUE:
      if (ops->is_null(state->current_result, 0u, reader->column)) {
        out->kind = CSERDE_NULL;
      } else {
        if (ops->column_type(state->current_result, reader->column) ==
            ORM_POSTGRES_BYTEA_OID) {
          out->kind = CSERDE_BYTES;
          out->value.slice.size = state->bytea_sizes[reader->column];
          out->value.slice.data =
              out->value.slice.size == 0u
                  ? NULL
                  : state->bytea_scratch +
                        state->bytea_offsets[reader->column];
        } else {
          const char *value =
              ops->value(state->current_result, 0u, reader->column);
          const size_t length = (size_t)
              ops->length(state->current_result, 0u, reader->column);
          if (value == NULL)
            return CSERDE_SOURCE_ERROR;
          if (orm_postgres_emit_text_value(
                  ops->column_type(state->current_result, reader->column),
                  (const unsigned char *)value, length, out) != CSERDE_OK)
            return CSERDE_SOURCE_ERROR;
        }
        if (out->kind == CSERDE_BYTES)
          out->value.slice.lifetime = CSERDE_VIEW_TRANSIENT;
      }
      ++reader->column;
      reader->phase = reader->column == columns
                          ? ORM_POSTGRES_READER_MAP_END
                          : ORM_POSTGRES_READER_KEY;
      return CSERDE_OK;
    case ORM_POSTGRES_READER_MAP_END:
      out->kind = CSERDE_MAP_END;
      reader->phase = ORM_POSTGRES_READER_DONE;
      return CSERDE_OK;
    case ORM_POSTGRES_READER_DONE:
      return CSERDE_DONE;
    default:
      return CSERDE_INVALID_STATE;
  }
}

static const cserde_reader_ops orm_postgres_reader_ops = {
    sizeof(cserde_reader_ops), CSERDE_READER_OPS_ABI_VERSION,
    orm_postgres_reader_next};

static orm_row_cursor_step orm_postgres_cursor_error(
    orm_postgres_cursor_state *state, orm_status_t status,
    const char *message) {
  orm_row_cursor_step step = ORM_ROW_CURSOR_STEP_INIT;
  (void)snprintf(state->error_message, sizeof(state->error_message), "%s",
                 message != NULL ? message : orm_status_message(status));
  step.kind = ORM_ROW_CURSOR_ERROR;
  step.status = status;
  step.message = state->error_message;
  return step;
}

static orm_status_t orm_postgres_observe_dimensions(
    orm_postgres_cursor_state *state, const void *result) {
  const int64_t signed_rows = state->driver.result->rows(result);
  const int64_t signed_columns = state->driver.result->columns(result);
  size_t rows;
  size_t columns;
  if (signed_rows < 0 || signed_columns < 0)
    return ORM_STATUS_INTERNAL_ERROR;
  rows = (size_t)signed_rows;
  columns = (size_t)signed_columns;
  if (columns > state->config.max_columns)
    return ORM_STATUS_LIMIT_EXCEEDED;
  if (rows > 1u)
    return ORM_STATUS_DATASTORE_ERROR;
  if (*state->config.column_count != 0u &&
      *state->config.column_count != columns)
    return ORM_STATUS_DATASTORE_ERROR;
  *state->config.column_count = columns;
  return ORM_STATUS_OK;
}

static orm_status_t orm_postgres_validate_row_bytes(
    orm_postgres_cursor_state *state) {
  size_t total = 0u;
  size_t bytea_capacity = 0u;
  size_t column;
  unsigned char *scratch;
  for (column = 0u; column < *state->config.column_count; ++column) {
    int64_t signed_length;
    size_t length;
    if (state->driver.result->is_null(state->current_result, 0u, column))
      continue;
    signed_length =
        state->driver.result->length(state->current_result, 0u, column);
    if (signed_length < 0)
      return ORM_STATUS_INTERNAL_ERROR;
    length = (size_t)signed_length;
    if (total > state->config.max_result_bytes ||
        length > state->config.max_result_bytes - total)
      return ORM_STATUS_LIMIT_EXCEEDED;
    total += length;
    if (state->driver.result->column_type(state->current_result, column) ==
        ORM_POSTGRES_BYTEA_OID) {
      if (bytea_capacity > state->config.max_result_bytes ||
          length > state->config.max_result_bytes - bytea_capacity)
        return ORM_STATUS_LIMIT_EXCEEDED;
      bytea_capacity += length;
    }
  }
  if (state->result_bytes > state->config.max_result_bytes ||
      total > state->config.max_result_bytes - state->result_bytes)
    return ORM_STATUS_LIMIT_EXCEEDED;
  state->result_bytes += total;
  if (bytea_capacity > state->bytea_capacity) {
    scratch = (unsigned char *)realloc(state->bytea_scratch, bytea_capacity);
    if (scratch == NULL)
      return ORM_STATUS_OUT_OF_MEMORY;
    state->bytea_scratch = scratch;
    state->bytea_capacity = bytea_capacity;
  }
  return ORM_STATUS_OK;
}

static int orm_postgres_hex_digit(unsigned char value) {
  if (value >= (unsigned char)'0' && value <= (unsigned char)'9')
    return (int)(value - (unsigned char)'0');
  if (value >= (unsigned char)'a' && value <= (unsigned char)'f')
    return (int)(value - (unsigned char)'a') + 10;
  if (value >= (unsigned char)'A' && value <= (unsigned char)'F')
    return (int)(value - (unsigned char)'A') + 10;
  return -1;
}

static orm_status_t orm_postgres_decode_bytea(
    orm_postgres_cursor_state *state, size_t column) {
  const unsigned char *input = (const unsigned char *)
      state->driver.result->value(state->current_result, 0u, column);
  const size_t input_size = (size_t)
      state->driver.result->length(state->current_result, 0u, column);
  const size_t output_offset = state->bytea_used;
  size_t input_index = 0u;

  if (input == NULL)
    return ORM_STATUS_DATASTORE_ERROR;
  state->bytea_offsets[column] = output_offset;
  if (input_size >= 2u && input[0] == (unsigned char)'\\' &&
      input[1] == (unsigned char)'x') {
    if ((input_size - 2u) % 2u != 0u)
      return ORM_STATUS_DATASTORE_ERROR;
    for (input_index = 2u; input_index < input_size; input_index += 2u) {
      const int high = orm_postgres_hex_digit(input[input_index]);
      const int low = orm_postgres_hex_digit(input[input_index + 1u]);
      if (high < 0 || low < 0)
        return ORM_STATUS_DATASTORE_ERROR;
      state->bytea_scratch[state->bytea_used++] =
          (unsigned char)((high << 4) | low);
    }
  } else {
    while (input_index < input_size) {
      if (input[input_index] != (unsigned char)'\\') {
        state->bytea_scratch[state->bytea_used++] = input[input_index++];
        continue;
      }
      if (input_index + 1u >= input_size)
        return ORM_STATUS_DATASTORE_ERROR;
      if (input[input_index + 1u] == (unsigned char)'\\') {
        state->bytea_scratch[state->bytea_used++] = (unsigned char)'\\';
        input_index += 2u;
        continue;
      }
      if (input_index + 3u >= input_size ||
          input[input_index + 1u] < (unsigned char)'0' ||
          input[input_index + 1u] > (unsigned char)'3' ||
          input[input_index + 2u] < (unsigned char)'0' ||
          input[input_index + 2u] > (unsigned char)'7' ||
          input[input_index + 3u] < (unsigned char)'0' ||
          input[input_index + 3u] > (unsigned char)'7')
        return ORM_STATUS_DATASTORE_ERROR;
      state->bytea_scratch[state->bytea_used++] = (unsigned char)(
          ((input[input_index + 1u] - (unsigned char)'0') << 6) |
          ((input[input_index + 2u] - (unsigned char)'0') << 3) |
          (input[input_index + 3u] - (unsigned char)'0'));
      input_index += 4u;
    }
  }
  state->bytea_sizes[column] = state->bytea_used - output_offset;
  return ORM_STATUS_OK;
}

static orm_status_t orm_postgres_prepare_bytea(
    orm_postgres_cursor_state *state) {
  size_t column;
  state->bytea_used = 0u;
  memset(state->bytea_offsets, 0,
         state->config.max_columns * sizeof(*state->bytea_offsets));
  memset(state->bytea_sizes, 0,
         state->config.max_columns * sizeof(*state->bytea_sizes));
  for (column = 0u; column < *state->config.column_count; ++column) {
    orm_status_t status;
    if (state->driver.result->is_null(state->current_result, 0u, column) ||
        state->driver.result->column_type(state->current_result, column) !=
            ORM_POSTGRES_BYTEA_OID)
      continue;
    status = orm_postgres_decode_bytea(state, column);
    if (status != ORM_STATUS_OK)
      return status;
  }
  return ORM_STATUS_OK;
}

static orm_status_t orm_postgres_parse_affected_rows(
    orm_postgres_cursor_state *state, const void *result) {
  const char *text = state->driver.result->command_tuples(result);
  uint64_t value = 0u;
  const unsigned char *cursor;
  if (text == NULL || *text == '\0') {
    *state->config.affected_rows = 0u;
    return ORM_STATUS_OK;
  }
  cursor = (const unsigned char *)text;
  while (*cursor != '\0') {
    uint64_t digit;
    if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9')
      return ORM_STATUS_INTERNAL_ERROR;
    digit = (uint64_t)(*cursor - (unsigned char)'0');
    if (value > (UINT64_MAX - digit) / ORM_POSTGRES_DECIMAL_BASE)
      return ORM_STATUS_INTERNAL_ERROR;
    value = value * ORM_POSTGRES_DECIMAL_BASE + digit;
    ++cursor;
  }
  *state->config.affected_rows = value;
  return ORM_STATUS_OK;
}

static orm_row_cursor_step orm_postgres_cursor_next(void *context,
                                                     cserde_reader *out_row) {
  orm_postgres_cursor_state *state =
      (orm_postgres_cursor_state *)context;
  orm_row_cursor_step step = ORM_ROW_CURSOR_STEP_INIT;

  if (state->terminal) {
    step.kind = ORM_ROW_CURSOR_DONE;
    return step;
  }
  orm_postgres_release_current(state);
  for (;;) {
    orm_postgres_result_status result_status;
    orm_status_t status;
    const char *message;
    state->current_result =
        state->driver.command->next_result(state->driver.context);
    if (state->current_result == NULL) {
      state->query_active = 0;
      state->terminal = 1;
      step.kind = ORM_ROW_CURSOR_DONE;
      return step;
    }
    result_status = state->driver.result->status(state->current_result);
    if (result_status == ORM_POSTGRES_RESULT_ERROR) {
      char diagnostic[ORM_C_ERROR_MESSAGE_CAPACITY];
      const char *sqlstate = state->driver.result->sqlstate(state->current_result);
      const orm_status_t error_status = orm_postgres_sqlstate_status(sqlstate);
      message = state->driver.result->error(state->current_result);
      if (sqlstate != NULL && strlen(sqlstate) == 5u) {
        (void)snprintf(diagnostic, sizeof(diagnostic), "SQLSTATE=%s %s",
                       sqlstate,
                       message != NULL && *message != '\0'
                           ? message
                           : "PostgreSQL query failed");
        message = diagnostic;
      }
      return orm_postgres_cursor_error(
          state, error_status,
          message != NULL && *message != '\0' ? message
                                              : "PostgreSQL query failed");
    }
    status = orm_postgres_observe_dimensions(state, state->current_result);
    if (status != ORM_STATUS_OK)
      return orm_postgres_cursor_error(
          state, status,
          status == ORM_STATUS_LIMIT_EXCEEDED
              ? "PostgreSQL result exceeds max_columns"
              : status == ORM_STATUS_INTERNAL_ERROR
                    ? "libpq returned a negative result dimension"
                    : "PostgreSQL returned inconsistent dimensions");

    if (result_status == ORM_POSTGRES_RESULT_SINGLE_ROW) {
      if (state->result_rows >= state->config.max_result_rows)
        return orm_postgres_cursor_error(
            state, ORM_STATUS_LIMIT_EXCEEDED,
            "PostgreSQL result exceeds max_result_rows");
      status = orm_postgres_validate_row_bytes(state);
      if (status != ORM_STATUS_OK)
        return orm_postgres_cursor_error(
            state, status,
            status == ORM_STATUS_OUT_OF_MEMORY
                ? "allocate PostgreSQL bytea scratch"
                : status == ORM_STATUS_INTERNAL_ERROR
                      ? "libpq returned a negative field length"
                : "PostgreSQL result exceeds max_result_bytes");
      status = orm_postgres_prepare_bytea(state);
      if (status != ORM_STATUS_OK)
        return orm_postgres_cursor_error(
            state, status, "PostgreSQL returned malformed bytea text");
      state->reader.cursor = state;
      state->reader.column = 0u;
      state->reader.phase = ORM_POSTGRES_READER_MAP_BEGIN;
      if (cserde_reader_init(out_row, &orm_postgres_reader_ops,
                             &state->reader) != CSERDE_OK)
        return orm_postgres_cursor_error(
            state, ORM_STATUS_INTERNAL_ERROR,
            "initialize PostgreSQL row reader");
      ++state->result_rows;
      step.kind = ORM_ROW_CURSOR_ROW;
      return step;
    }
    if (result_status == ORM_POSTGRES_RESULT_COMMAND_DONE) {
      status = orm_postgres_parse_affected_rows(state, state->current_result);
      if (status != ORM_STATUS_OK)
        return orm_postgres_cursor_error(
            state, status,
            "PostgreSQL returned an invalid affected-row count");
      orm_postgres_release_current(state);
      continue;
    }
    if (result_status == ORM_POSTGRES_RESULT_TUPLES_DONE) {
      orm_postgres_release_current(state);
      continue;
    }
    return orm_postgres_cursor_error(
        state, ORM_STATUS_DATASTORE_ERROR,
        "PostgreSQL returned an unknown result status");
  }
}

static void orm_postgres_cursor_cancel(void *context) {
  orm_postgres_cursor_state *state =
      (orm_postgres_cursor_state *)context;
  if (state == NULL || state->terminal)
    return;
  orm_postgres_drain(state);
  state->terminal = 1;
}

static void orm_postgres_cursor_destroy(void *context) {
  orm_postgres_cursor_state *state =
      (orm_postgres_cursor_state *)context;
  if (state == NULL)
    return;
  orm_postgres_drain(state);
  free(state->bytea_sizes);
  free(state->bytea_offsets);
  free(state->bytea_scratch);
  free(state);
}

static orm_status_t orm_postgres_cursor_column_count(void *context,
                                                     uint64_t *out_count) {
  const orm_postgres_cursor_state *state =
      (const orm_postgres_cursor_state *)context;
  if (state == NULL || state->config.column_count == NULL || out_count == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;
  *out_count = (uint64_t)*state->config.column_count;
  return ORM_STATUS_OK;
}

static const orm_row_cursor_ops orm_postgres_cursor_ops = {
    sizeof(orm_row_cursor_ops), ORM_ROW_CURSOR_OPS_ABI_VERSION,
    "postgresql-single-row", orm_postgres_cursor_next,
    orm_postgres_cursor_cancel, orm_postgres_cursor_destroy, NULL,
    orm_postgres_cursor_column_count};

orm_status_t orm_postgres_cursor_start(
    orm_row_cursor *out_cursor, const orm_postgres_driver *driver,
    const orm_postgres_query_request *request,
    const orm_postgres_cursor_config *config, orm_error_t *error) {
  orm_postgres_cursor_state *state;
  const char *message;

  if (out_cursor == NULL || out_cursor->ops != NULL ||
      out_cursor->context != NULL || driver == NULL || request == NULL ||
      request->sql == NULL || request->parameter_count < 0 ||
      !orm_postgres_command_ops_valid(driver->command) ||
      !orm_postgres_result_ops_valid(driver->result) ||
      !orm_postgres_config_valid(config)) {
    orm_postgres_set_error(error, ORM_STATUS_INVALID_ARGUMENT,
                           "invalid PostgreSQL cursor configuration");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  state = (orm_postgres_cursor_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    orm_postgres_set_error(error, ORM_STATUS_OUT_OF_MEMORY, NULL);
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  state->driver = *driver;
  state->config = *config;
  orm_error_init(&state->owned_runtime_error);
  if (state->config.column_count == NULL)
    state->config.column_count = &state->owned_column_count;
  if (state->config.affected_rows == NULL)
    state->config.affected_rows = &state->owned_affected_rows;
  if (state->config.runtime_error == NULL)
    state->config.runtime_error = &state->owned_runtime_error;
  if (state->config.max_columns >
      SIZE_MAX / sizeof(*state->bytea_offsets)) {
    orm_postgres_set_error(error, ORM_STATUS_INVALID_ARGUMENT,
                           "PostgreSQL max_columns exceeds platform range");
    free(state);
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  state->bytea_offsets = (size_t *)calloc(
      state->config.max_columns, sizeof(*state->bytea_offsets));
  state->bytea_sizes = (size_t *)calloc(
      state->config.max_columns, sizeof(*state->bytea_sizes));
  if (state->bytea_offsets == NULL || state->bytea_sizes == NULL) {
    free(state->bytea_sizes);
    free(state->bytea_offsets);
    free(state);
    orm_postgres_set_error(error, ORM_STATUS_OUT_OF_MEMORY, NULL);
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  *state->config.column_count = 0u;
  *state->config.affected_rows = 0u;
  orm_postgres_set_error(state->config.runtime_error, ORM_STATUS_OK, NULL);

  if (!state->driver.command->send_query(state->driver.context, request)) {
    message = state->driver.command->connection_error(state->driver.context);
    orm_postgres_set_error(error, ORM_STATUS_SQL_ERROR, message);
    free(state->bytea_sizes);
    free(state->bytea_offsets);
    free(state);
    return ORM_STATUS_SQL_ERROR;
  }
  state->query_active = 1;
  if (!state->driver.command->enable_single_row(state->driver.context)) {
    orm_postgres_drain(state);
    orm_postgres_set_error(error, ORM_STATUS_DATASTORE_ERROR,
                           "enable PostgreSQL single-row mode");
    free(state->bytea_sizes);
    free(state->bytea_offsets);
    free(state);
    return ORM_STATUS_DATASTORE_ERROR;
  }
  out_cursor->ops = &orm_postgres_cursor_ops;
  out_cursor->context = state;
  orm_postgres_set_error(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}
