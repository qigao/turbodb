#include "orm_mongo_cursor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum orm_mongo_reader_phase {
  ORM_MONGO_READER_MAP_BEGIN = 0,
  ORM_MONGO_READER_KEY,
  ORM_MONGO_READER_VALUE,
  ORM_MONGO_READER_MAP_END,
  ORM_MONGO_READER_DONE
} orm_mongo_reader_phase;

typedef struct orm_mongo_cursor_state orm_mongo_cursor_state;

typedef struct orm_mongo_reader_state {
  orm_mongo_cursor_state *cursor;
  size_t column;
  orm_mongo_reader_phase phase;
} orm_mongo_reader_state;

struct orm_mongo_cursor_state {
  orm_mongo_driver driver;
  orm_mongo_field *fields;
  unsigned char *names;
  orm_mongo_value *values;
  size_t field_count;
  uint64_t max_rows;
  uint64_t max_result_bytes;
  uint64_t rows;
  uint64_t result_bytes;
  int terminal;
  orm_mongo_reader_state reader;
  char error_message[ORM_C_ERROR_MESSAGE_CAPACITY];
};

static void orm_mongo_set_error(orm_error_t *error, orm_status_t status,
                                const char *message) {
  if (error == NULL || error->struct_size < sizeof(*error))
    return;
  error->status = status;
  (void)snprintf(error->message, sizeof(error->message), "%s",
                 status == ORM_STATUS_OK
                     ? ""
                     : (message != NULL ? message
                                        : orm_status_message(status)));
}

static int orm_mongo_driver_valid(const orm_mongo_driver *driver) {
  return driver != NULL && driver->context != NULL && driver->ops != NULL &&
         driver->ops->struct_size >= sizeof(*driver->ops) &&
         driver->ops->abi_version == ORM_MONGO_DRIVER_OPS_ABI_VERSION &&
         driver->ops->next != NULL && driver->ops->find != NULL &&
         driver->ops->destroy != NULL;
}

static cserde_status orm_mongo_reader_next(void *context,
                                            cserde_token *out) {
  orm_mongo_reader_state *reader = (orm_mongo_reader_state *)context;
  orm_mongo_cursor_state *state;
  orm_mongo_value value = ORM_MONGO_VALUE_INIT;
  if (reader == NULL || out == NULL || reader->cursor == NULL)
    return CSERDE_INVALID_ARGUMENT;
  state = reader->cursor;
  memset(out, 0, sizeof(*out));
  switch (reader->phase) {
    case ORM_MONGO_READER_MAP_BEGIN:
      out->kind = CSERDE_MAP_BEGIN;
      reader->phase = state->field_count == 0u
                          ? ORM_MONGO_READER_MAP_END
                          : ORM_MONGO_READER_KEY;
      return CSERDE_OK;
    case ORM_MONGO_READER_KEY:
      out->kind = CSERDE_STRING;
      out->value.slice.data = state->fields[reader->column].output_name.data;
      out->value.slice.size = state->fields[reader->column].output_name.size;
      out->value.slice.lifetime = CSERDE_VIEW_STABLE;
      reader->phase = ORM_MONGO_READER_VALUE;
      return CSERDE_OK;
    case ORM_MONGO_READER_VALUE:
      value = state->values[reader->column];
      switch (value.kind) {
        case ORM_MONGO_VALUE_NULL:
          out->kind = CSERDE_NULL;
          break;
        case ORM_MONGO_VALUE_BOOL:
          out->kind = CSERDE_BOOL;
          out->value.boolean = value.data.boolean;
          break;
        case ORM_MONGO_VALUE_SINT:
          out->kind = CSERDE_SINT;
          out->value.sint = value.data.sint;
          break;
        case ORM_MONGO_VALUE_UINT:
          out->kind = CSERDE_UINT;
          out->value.uint = value.data.uint;
          break;
        case ORM_MONGO_VALUE_FLOAT:
          out->kind = CSERDE_FLOAT;
          out->value.floating = value.data.floating;
          break;
        case ORM_MONGO_VALUE_STRING:
        case ORM_MONGO_VALUE_BYTES:
          out->kind = value.kind == ORM_MONGO_VALUE_STRING
                          ? CSERDE_STRING
                          : CSERDE_BYTES;
          out->value.slice.data = value.data.slice.data;
          out->value.slice.size = value.data.slice.size;
          out->value.slice.lifetime = CSERDE_VIEW_TRANSIENT;
          if (out->value.slice.size != 0u &&
              out->value.slice.data == NULL)
            return CSERDE_SOURCE_ERROR;
          break;
        default:
          return CSERDE_SOURCE_ERROR;
      }
      ++reader->column;
      reader->phase = reader->column == state->field_count
                          ? ORM_MONGO_READER_MAP_END
                          : ORM_MONGO_READER_KEY;
      return CSERDE_OK;
    case ORM_MONGO_READER_MAP_END:
      out->kind = CSERDE_MAP_END;
      reader->phase = ORM_MONGO_READER_DONE;
      return CSERDE_OK;
    case ORM_MONGO_READER_DONE:
      return CSERDE_DONE;
    default:
      return CSERDE_SOURCE_ERROR;
  }
}

static const cserde_reader_ops orm_mongo_reader_ops = {
    sizeof(cserde_reader_ops), CSERDE_READER_OPS_ABI_VERSION,
    orm_mongo_reader_next};

static orm_status_t orm_mongo_prepare_values(orm_mongo_cursor_state *state,
                                             const void *document) {
  uint64_t row_bytes = 0u;
  size_t index;
  for (index = 0u; index < state->field_count; ++index) {
    uint64_t value_bytes;
    orm_status_t status = state->driver.ops->find(
        state->driver.context, document,
        state->fields[index].source_path.data,
        state->fields[index].source_path.size, &state->values[index]);
    if (status != ORM_STATUS_OK)
      return status;
    switch (state->values[index].kind) {
      case ORM_MONGO_VALUE_NULL:
        value_bytes = 0u;
        break;
      case ORM_MONGO_VALUE_BOOL:
        value_bytes = sizeof(uint8_t);
        break;
      case ORM_MONGO_VALUE_SINT:
      case ORM_MONGO_VALUE_UINT:
      case ORM_MONGO_VALUE_FLOAT:
        value_bytes = sizeof(uint64_t);
        break;
      case ORM_MONGO_VALUE_STRING:
      case ORM_MONGO_VALUE_BYTES:
        if (state->values[index].data.slice.size != 0u &&
            state->values[index].data.slice.data == NULL)
          return ORM_STATUS_DATASTORE_ERROR;
        value_bytes = (uint64_t)state->values[index].data.slice.size;
        break;
      default:
        return ORM_STATUS_DATASTORE_ERROR;
    }
    if (value_bytes > UINT64_MAX - row_bytes)
      return ORM_STATUS_LIMIT_EXCEEDED;
    row_bytes += value_bytes;
  }
  if (state->result_bytes > state->max_result_bytes ||
      row_bytes > state->max_result_bytes - state->result_bytes)
    return ORM_STATUS_LIMIT_EXCEEDED;
  state->result_bytes += row_bytes;
  return ORM_STATUS_OK;
}

static orm_row_cursor_step orm_mongo_cursor_next(void *context,
                                                  cserde_reader *out_row) {
  orm_mongo_cursor_state *state = (orm_mongo_cursor_state *)context;
  orm_row_cursor_step step = ORM_ROW_CURSOR_STEP_INIT;
  const void *document = NULL;
  orm_mongo_driver_step driver_step;
  if (state == NULL || out_row == NULL) {
    step.kind = ORM_ROW_CURSOR_ERROR;
    step.status = ORM_STATUS_INVALID_ARGUMENT;
    step.message = "invalid MongoDB cursor resume";
    return step;
  }
  if (state->terminal)
    return step;
  driver_step = state->driver.ops->next(state->driver.context, &document);
  if (driver_step.kind == ORM_MONGO_DRIVER_DONE) {
    state->terminal = 1;
    return step;
  }
  if (driver_step.kind == ORM_MONGO_DRIVER_ERROR || document == NULL) {
    state->terminal = 1;
    step.kind = ORM_ROW_CURSOR_ERROR;
    step.status = driver_step.status == ORM_STATUS_OK
                      ? ORM_STATUS_DATASTORE_ERROR
                      : driver_step.status;
    step.message = driver_step.message != NULL
                       ? driver_step.message
                       : "iterate MongoDB cursor";
    return step;
  }
  if (state->rows == state->max_rows) {
    state->terminal = 1;
    step.kind = ORM_ROW_CURSOR_ERROR;
    step.status = ORM_STATUS_LIMIT_EXCEEDED;
    step.message = "MongoDB result exceeds max_rows";
    return step;
  }
  {
    const orm_status_t status = orm_mongo_prepare_values(state, document);
    if (status != ORM_STATUS_OK) {
      state->terminal = 1;
      step.kind = ORM_ROW_CURSOR_ERROR;
      step.status = status;
      step.message = status == ORM_STATUS_LIMIT_EXCEEDED
                         ? "MongoDB result exceeds max_result_bytes"
                         : "read MongoDB result fields";
      return step;
    }
  }
  state->reader.cursor = state;
  state->reader.column = 0u;
  state->reader.phase = ORM_MONGO_READER_MAP_BEGIN;
  if (cserde_reader_init(out_row, &orm_mongo_reader_ops,
                        &state->reader) != CSERDE_OK) {
    state->terminal = 1;
    step.kind = ORM_ROW_CURSOR_ERROR;
    step.status = ORM_STATUS_INTERNAL_ERROR;
    step.message = "initialize MongoDB row reader";
    return step;
  }
  ++state->rows;
  step.kind = ORM_ROW_CURSOR_ROW;
  return step;
}

static void orm_mongo_cursor_cancel(void *context) {
  orm_mongo_cursor_state *state = (orm_mongo_cursor_state *)context;
  if (state != NULL)
    state->terminal = 1;
}

static void orm_mongo_cursor_destroy(void *context) {
  orm_mongo_cursor_state *state = (orm_mongo_cursor_state *)context;
  if (state == NULL)
    return;
  state->driver.ops->destroy(state->driver.context);
  free(state->values);
  free(state->names);
  free(state->fields);
  free(state);
}

static const orm_row_cursor_ops orm_mongo_cursor_ops = {
    sizeof(orm_row_cursor_ops), ORM_ROW_CURSOR_OPS_ABI_VERSION,
    "mongodb-document", orm_mongo_cursor_next, orm_mongo_cursor_cancel,
    orm_mongo_cursor_destroy};

orm_status_t orm_mongo_cursor_start(
    orm_row_cursor *out_cursor, orm_mongo_driver *driver,
    const orm_mongo_field *fields, size_t field_count,
    const orm_mongo_cursor_config *config, orm_error_t *error) {
  orm_mongo_cursor_state *state;
  size_t name_bytes = 0u;
  size_t index;
  if (out_cursor == NULL || out_cursor->ops != NULL ||
      out_cursor->context != NULL || !orm_mongo_driver_valid(driver) ||
      config == NULL || config->struct_size < sizeof(*config) ||
      config->abi_version != ORM_MONGO_CURSOR_CONFIG_ABI_VERSION ||
      config->max_rows == 0u || config->max_result_bytes == 0u ||
      config->max_field_name_bytes == 0u ||
      (field_count != 0u && fields == NULL)) {
    orm_mongo_set_error(error, ORM_STATUS_INVALID_ARGUMENT,
                        "invalid MongoDB cursor configuration");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  for (index = 0u; index < field_count; ++index) {
    const size_t output_size = fields[index].output_name.size;
    const size_t path_size = fields[index].source_path.size;
    if ((output_size != 0u && fields[index].output_name.data == NULL) ||
        (path_size != 0u && fields[index].source_path.data == NULL) ||
        output_size > config->max_field_name_bytes - name_bytes) {
      orm_mongo_set_error(error, ORM_STATUS_LIMIT_EXCEEDED,
                          "MongoDB field names exceed configured bounds");
      return ORM_STATUS_LIMIT_EXCEEDED;
    }
    name_bytes += output_size;
    if (path_size > config->max_field_name_bytes - name_bytes) {
      orm_mongo_set_error(error, ORM_STATUS_LIMIT_EXCEEDED,
                          "MongoDB field paths exceed configured bounds");
      return ORM_STATUS_LIMIT_EXCEEDED;
    }
    name_bytes += path_size;
  }
  if (field_count > SIZE_MAX / sizeof(*state->fields)) {
    orm_mongo_set_error(error, ORM_STATUS_LIMIT_EXCEEDED,
                        "MongoDB field metadata size overflows");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  if (field_count > (SIZE_MAX - name_bytes) / 2u) {
    orm_mongo_set_error(error, ORM_STATUS_LIMIT_EXCEEDED,
                        "MongoDB field storage size overflows");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  name_bytes += field_count * 2u;
  state = (orm_mongo_cursor_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    orm_mongo_set_error(error, ORM_STATUS_OUT_OF_MEMORY, NULL);
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  state->fields = (orm_mongo_field *)calloc(
      field_count == 0u ? 1u : field_count, sizeof(*state->fields));
  state->values = (orm_mongo_value *)calloc(
      field_count == 0u ? 1u : field_count, sizeof(*state->values));
  state->names = (unsigned char *)malloc(name_bytes == 0u ? 1u : name_bytes);
  if (state->fields == NULL || state->values == NULL || state->names == NULL) {
    free(state->names);
    free(state->values);
    free(state->fields);
    free(state);
    orm_mongo_set_error(error, ORM_STATUS_OUT_OF_MEMORY, NULL);
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  name_bytes = 0u;
  for (index = 0u; index < field_count; ++index) {
    state->fields[index].output_name.data = state->names + name_bytes;
    state->fields[index].output_name.size = fields[index].output_name.size;
    if (fields[index].output_name.size != 0u)
      memcpy(state->names + name_bytes, fields[index].output_name.data,
             fields[index].output_name.size);
    name_bytes += fields[index].output_name.size;
    state->names[name_bytes++] = '\0';
    state->fields[index].source_path.data = state->names + name_bytes;
    state->fields[index].source_path.size = fields[index].source_path.size;
    if (fields[index].source_path.size != 0u)
      memcpy(state->names + name_bytes, fields[index].source_path.data,
             fields[index].source_path.size);
    name_bytes += fields[index].source_path.size;
    state->names[name_bytes++] = '\0';
  }
  state->driver = *driver;
  state->field_count = field_count;
  state->max_rows = config->max_rows;
  state->max_result_bytes = config->max_result_bytes;
  driver->ops = NULL;
  driver->context = NULL;
  out_cursor->ops = &orm_mongo_cursor_ops;
  out_cursor->context = state;
  orm_mongo_set_error(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}
