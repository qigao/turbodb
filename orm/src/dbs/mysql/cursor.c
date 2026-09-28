#include "cursor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct mysql_cursor_state_t {
  mysql_cursor_source_t source;
  mysql_cursor_row_store_t rows;
  mysql_cursor_config_t config;
  uint64_t result_rows;
  uint64_t result_bytes;
  int cancel_requested;
  int terminal;
  char error_message[ORM_C_ERROR_MESSAGE_CAPACITY];
} mysql_cursor_state_t;

static void mysql_cursor_set_error(
    orm_error_t *error, orm_status_t status, const char *message) {
  if (error == NULL || error->struct_size < sizeof(*error))
    return;
  error->status = status;
  (void)snprintf(error->message, sizeof(error->message), "%s",
                 status == ORM_STATUS_OK
                     ? ""
                     : (message != NULL ? message
                                        : "MySQL cursor error"));
}

static int mysql_cursor_source_valid(const mysql_cursor_source_t *source) {
  return source != NULL && source->context != NULL &&
         source->ops != NULL &&
         source->ops->struct_size >= sizeof(*source->ops) &&
         source->ops->abi_version == MYSQL_CURSOR_SOURCE_OPS_ABI_VERSION &&
         source->ops->next != NULL &&
         source->ops->cancel != NULL &&
         source->ops->destroy != NULL;
}

static orm_status_t mysql_cursor_wire_status(
    mysql_wire_status_t status) {
  switch (status) {
    case MYSQL_WIRE_STATUS_LIMIT:
      return ORM_STATUS_LIMIT_EXCEEDED;
    case MYSQL_WIRE_STATUS_OK:
      return ORM_STATUS_OK;
    case MYSQL_WIRE_STATUS_NEED_MORE:
    case MYSQL_WIRE_STATUS_INVALID:
    case MYSQL_WIRE_STATUS_SEQUENCE:
    default:
      return ORM_STATUS_DATASTORE_ERROR;
  }
}

static orm_row_cursor_step mysql_cursor_next(
    void *context, cserde_reader *out_row) {
  mysql_cursor_state_t *state = (mysql_cursor_state_t *)context;
  orm_row_cursor_step result = ORM_ROW_CURSOR_STEP_INIT;
  mysql_cursor_source_step_t source_step;
  mysql_wire_status_t wire_status;
  cserde_status reader_status;

  if (state == NULL || out_row == NULL) {
    result.kind = ORM_ROW_CURSOR_ERROR;
    result.status = ORM_STATUS_INVALID_ARGUMENT;
    result.message = "invalid MySQL cursor resume";
    return result;
  }

  if (state->terminal)
    return result;

  source_step = state->source.ops->next(state->source.context);
  switch (source_step.kind) {
    case MYSQL_CURSOR_SOURCE_DONE:
      state->terminal = 1;
      result.kind = ORM_ROW_CURSOR_DONE;
      return result;

    case MYSQL_CURSOR_SOURCE_ERROR:
      state->terminal = 1;
      result.kind = ORM_ROW_CURSOR_ERROR;
      result.status = source_step.status != ORM_STATUS_OK
                          ? source_step.status
                          : ORM_STATUS_DATASTORE_ERROR;
      result.message = source_step.message != NULL
                           ? source_step.message
                           : "iterate MySQL result source";
      return result;

    case MYSQL_CURSOR_SOURCE_ROW:
      break;

    default:
      state->terminal = 1;
      result.kind = ORM_ROW_CURSOR_ERROR;
      result.status = ORM_STATUS_INTERNAL_ERROR;
      result.message = "MySQL source returned an invalid step";
      return result;
  }

  if (source_step.row == NULL || source_step.row_size == 0u) {
    state->terminal = 1;
    result.kind = ORM_ROW_CURSOR_ERROR;
    result.status = ORM_STATUS_DATASTORE_ERROR;
    result.message = "MySQL source returned an empty row";
    return result;
  }

  if (state->result_rows >= state->config.max_result_rows ||
      (uint64_t)source_step.row_size > state->config.max_result_bytes ||
      state->result_bytes > state->config.max_result_bytes ||
      (uint64_t)source_step.row_size >
          state->config.max_result_bytes - state->result_bytes) {
    state->terminal = 1;
    result.kind = ORM_ROW_CURSOR_ERROR;
    result.status = ORM_STATUS_LIMIT_EXCEEDED;
    result.message = "MySQL result exceeds configured bounds";
    return result;
  }

  wire_status = mysql_cursor_row_store_load(
      &state->rows, source_step.row, source_step.row_size);
  if (wire_status != MYSQL_WIRE_STATUS_OK) {
    state->terminal = 1;
    result.kind = ORM_ROW_CURSOR_ERROR;
    result.status = mysql_cursor_wire_status(wire_status);
    result.message = wire_status == MYSQL_WIRE_STATUS_LIMIT
                         ? "MySQL row exceeds configured bound"
                         : "invalid MySQL binary row";
    return result;
  }

  reader_status = mysql_cursor_row_store_reader(&state->rows, out_row);
  if (reader_status != CSERDE_OK) {
    state->terminal = 1;
    result.kind = ORM_ROW_CURSOR_ERROR;
    result.status = ORM_STATUS_INTERNAL_ERROR;
    result.message = "initialize MySQL row reader";
    return result;
  }

  ++state->result_rows;
  state->result_bytes += (uint64_t)source_step.row_size;
  result.kind = ORM_ROW_CURSOR_ROW;
  return result;
}

static void mysql_cursor_cancel(void *context) {
  mysql_cursor_state_t *state = (mysql_cursor_state_t *)context;
  if (state == NULL || state->terminal || state->cancel_requested)
    return;
  state->cancel_requested = 1;
  state->source.ops->cancel(state->source.context);
}

static void mysql_cursor_destroy(void *context) {
  mysql_cursor_state_t *state = (mysql_cursor_state_t *)context;
  if (state == NULL)
    return;
  if (state->source.context != NULL &&
      state->source.ops != NULL &&
      state->source.ops->destroy != NULL)
    state->source.ops->destroy(state->source.context);
  state->source.ops = NULL;
  state->source.context = NULL;
  mysql_cursor_row_store_destroy(&state->rows);
  free(state);
}

static orm_status_t mysql_cursor_column_count(
    void *context, uint64_t *out_count) {
  const mysql_cursor_state_t *state =
      (const mysql_cursor_state_t *)context;
  if (state == NULL || out_count == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;
  *out_count = (uint64_t)state->rows.column_count;
  return ORM_STATUS_OK;
}

static const orm_row_cursor_ops mysql_cursor_ops = {
    sizeof(orm_row_cursor_ops),
    ORM_ROW_CURSOR_OPS_ABI_VERSION,
    "mysql",
    mysql_cursor_next,
    mysql_cursor_cancel,
    mysql_cursor_destroy,
    NULL,
    mysql_cursor_column_count};

orm_status_t mysql_cursor_start(
    orm_row_cursor *out_cursor,
    mysql_cursor_source_t *source,
    const mysql_column_definition_t *columns,
    size_t column_count,
    const mysql_cursor_config_t *config,
    orm_error_t *error) {
  mysql_cursor_state_t *state;
  mysql_wire_status_t wire_status;

  if (out_cursor == NULL ||
      out_cursor->ops != NULL ||
      out_cursor->context != NULL ||
      !mysql_cursor_source_valid(source) ||
      config == NULL ||
      config->struct_size < sizeof(*config) ||
      config->abi_version != MYSQL_CURSOR_CONFIG_ABI_VERSION ||
      config->max_result_rows == 0u ||
      config->max_result_bytes == 0u ||
      config->max_columns == 0u ||
      config->max_metadata_bytes == 0u ||
      config->max_row_bytes == 0u ||
      config->max_row_bytes > (size_t)config->max_result_bytes ||
      (columns == NULL && column_count != 0u)) {
    mysql_cursor_set_error(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "invalid MySQL cursor configuration");
    return ORM_STATUS_INVALID_ARGUMENT;
  }

  state = (mysql_cursor_state_t *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    mysql_cursor_set_error(
        error, ORM_STATUS_OUT_OF_MEMORY,
        "allocate MySQL cursor");
    return ORM_STATUS_OUT_OF_MEMORY;
  }

  wire_status = mysql_cursor_row_store_init(
      &state->rows, columns, column_count,
      config->max_columns,
      config->max_metadata_bytes,
      config->max_row_bytes);
  if (wire_status != MYSQL_WIRE_STATUS_OK) {
    const orm_status_t status = mysql_cursor_wire_status(wire_status);
    mysql_cursor_row_store_destroy(&state->rows);
    free(state);
    mysql_cursor_set_error(
        error, status,
        wire_status == MYSQL_WIRE_STATUS_LIMIT
            ? "MySQL cursor metadata exceeds configured bounds"
            : "invalid MySQL cursor metadata");
    return status;
  }

  state->source = *source;
  state->config = *config;
  source->ops = NULL;
  source->context = NULL;

  out_cursor->ops = &mysql_cursor_ops;
  out_cursor->context = state;
  mysql_cursor_set_error(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}
