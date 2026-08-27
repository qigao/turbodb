#include "orm_redis_cursor.h"
#include "orm_text_token.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum orm_redis_reader_phase {
  ORM_REDIS_READER_MAP_BEGIN = 0,
  ORM_REDIS_READER_KEY,
  ORM_REDIS_READER_VALUE,
  ORM_REDIS_READER_MAP_END,
  ORM_REDIS_READER_DONE
} orm_redis_reader_phase;

typedef struct orm_redis_cursor_state orm_redis_cursor_state;

typedef struct orm_redis_reader_state {
  orm_redis_cursor_state *cursor;
  size_t column;
  orm_redis_reader_phase phase;
} orm_redis_reader_state;

struct orm_redis_cursor_state {
  orm_redis_row_driver driver;
  orm_redis_field_view *fields;
  const cmeta_data_desc **field_shapes;
  unsigned char *field_names;
  size_t field_count;
  size_t max_rows;
  size_t rows;
  void *row;
  int terminal;
  orm_redis_reader_state reader;
  char error_message[ORM_C_ERROR_MESSAGE_CAPACITY];
};

static void orm_redis_set_error(orm_error_t *error, orm_status_t status,
                                const char *message) {
  if (error == NULL || error->struct_size < sizeof(*error)) return;
  error->status = status;
  (void)snprintf(error->message, sizeof(error->message), "%s",
                 status == ORM_STATUS_OK ? "" :
                 (message != NULL ? message : orm_status_message(status)));
}

static int orm_redis_driver_valid(const orm_redis_row_driver *driver) {
  const orm_redis_row_driver_ops *ops;
  const orm_redis_reply_ops *reply_ops;
  if (driver == NULL || driver->context == NULL) return 0;
  ops = driver->ops;
  reply_ops = driver->reply_ops;
  return ops != NULL && reply_ops != NULL &&
         ops->struct_size >= sizeof(*ops) &&
         ops->abi_version == ORM_REDIS_ROW_DRIVER_OPS_ABI_VERSION &&
         ops->next != NULL && ops->cancel != NULL &&
         ops->release_row != NULL && ops->destroy != NULL &&
         reply_ops->struct_size >= sizeof(*reply_ops) &&
         reply_ops->abi_version == ORM_REDIS_REPLY_OPS_ABI_VERSION &&
         reply_ops->kind != NULL && reply_ops->integer != NULL &&
         reply_ops->bytes != NULL && reply_ops->child_count != NULL &&
         reply_ops->child != NULL;
}

static void orm_redis_release_row(orm_redis_cursor_state *state) {
  if (state->row == NULL) return;
  state->driver.ops->release_row(state->driver.context, state->row);
  state->row = NULL;
}

static cserde_status orm_redis_reply_bytes(
    orm_redis_cursor_state *state, const void *reply,
    const unsigned char **data, size_t *size) {
  if (reply == NULL || state->driver.reply_ops->kind(
                           state->driver.context, reply) != ORM_REDIS_REPLY_STRING)
    return CSERDE_SOURCE_ERROR;
  *data = state->driver.reply_ops->bytes(state->driver.context, reply, size);
  return *size == 0u || *data != NULL ? CSERDE_OK : CSERDE_SOURCE_ERROR;
}

static const void *orm_redis_find_field(orm_redis_cursor_state *state,
                                        const orm_redis_field_view *field,
                                        cserde_status *status) {
  const orm_redis_reply_ops *ops = state->driver.reply_ops;
  size_t index;
  size_t count;
  if (ops->kind(state->driver.context, state->row) != ORM_REDIS_REPLY_ARRAY) {
    *status = CSERDE_SOURCE_ERROR;
    return NULL;
  }
  count = ops->child_count(state->driver.context, state->row);
  if ((count & 1u) != 0u) {
    *status = CSERDE_SOURCE_ERROR;
    return NULL;
  }
  for (index = 0u; index < count; index += 2u) {
    const void *name = ops->child(state->driver.context, state->row, index);
    const unsigned char *data = NULL;
    size_t size = 0u;
    if (orm_redis_reply_bytes(state, name, &data, &size) != CSERDE_OK) {
      *status = CSERDE_SOURCE_ERROR;
      return NULL;
    }
    if (size == field->size &&
        (size == 0u || memcmp(data, field->data, size) == 0)) {
      *status = CSERDE_OK;
      return ops->child(state->driver.context, state->row, index + 1u);
    }
  }
  *status = CSERDE_OK;
  return NULL;
}

static cserde_status orm_redis_reader_next(void *context, cserde_token *out) {
  orm_redis_reader_state *reader = (orm_redis_reader_state *)context;
  orm_redis_cursor_state *state;
  const orm_redis_reply_ops *ops;
  const cmeta_data_desc *shape;
  const void *value;
  int64_t integer;
  cserde_status status;
  orm_redis_reply_kind kind;
  if (reader == NULL || out == NULL || reader->cursor == NULL)
    return CSERDE_INVALID_ARGUMENT;
  state = reader->cursor;
  ops = state->driver.reply_ops;
  memset(out, 0, sizeof(*out));
  switch (reader->phase) {
    case ORM_REDIS_READER_MAP_BEGIN:
      out->kind = CSERDE_MAP_BEGIN;
      reader->phase = state->field_count == 0u ? ORM_REDIS_READER_MAP_END
                                               : ORM_REDIS_READER_KEY;
      return CSERDE_OK;
    case ORM_REDIS_READER_KEY:
      out->kind = CSERDE_STRING;
      out->value.slice.data = state->fields[reader->column].data;
      out->value.slice.size = state->fields[reader->column].size;
      out->value.slice.lifetime = CSERDE_VIEW_STABLE;
      reader->phase = ORM_REDIS_READER_VALUE;
      return CSERDE_OK;
    case ORM_REDIS_READER_VALUE:
      value = orm_redis_find_field(state, &state->fields[reader->column], &status);
      if (status != CSERDE_OK) return status;
      if (value == NULL || ops->kind(state->driver.context, value) == ORM_REDIS_REPLY_NULL) {
        out->kind = CSERDE_NULL;
      } else {
        kind = ops->kind(state->driver.context, value);
        shape = state->field_shapes[reader->column];
        if (kind == ORM_REDIS_REPLY_INTEGER) {
          integer = ops->integer(state->driver.context, value);
          if (shape != NULL && shape->kind == CMETA_DATA_BOOL) {
            if (integer != 0 && integer != 1) return CSERDE_SOURCE_ERROR;
            out->kind = CSERDE_BOOL;
            out->value.boolean = integer != 0;
          } else if (shape != NULL && shape->kind == CMETA_DATA_UINT) {
            if (integer < 0) return CSERDE_SOURCE_ERROR;
            out->kind = CSERDE_UINT;
            out->value.uint = (uint64_t)integer;
          } else {
            out->kind = CSERDE_SINT;
            out->value.sint = integer;
          }
        } else if (kind == ORM_REDIS_REPLY_STRING) {
          const unsigned char *data =
              ops->bytes(state->driver.context, value, &out->value.slice.size);
          if (out->value.slice.size != 0u && data == NULL)
            return CSERDE_SOURCE_ERROR;
          if (shape != NULL && shape->kind == CMETA_DATA_SINT) {
            if (orm_text_token_sint(data, out->value.slice.size, out) !=
                CSERDE_OK)
              return CSERDE_SOURCE_ERROR;
          } else if (shape != NULL && shape->kind == CMETA_DATA_UINT) {
            if (orm_text_token_uint(data, out->value.slice.size, out) !=
                CSERDE_OK)
              return CSERDE_SOURCE_ERROR;
          } else if (shape != NULL && shape->kind == CMETA_DATA_FLOAT) {
            if (orm_text_token_float(data, out->value.slice.size, 1, out) !=
                CSERDE_OK)
              return CSERDE_SOURCE_ERROR;
          } else if (shape != NULL && shape->kind == CMETA_DATA_BOOL) {
            if (out->value.slice.size != 1u ||
                (data[0] != (unsigned char)'0' &&
                 data[0] != (unsigned char)'1'))
              return CSERDE_SOURCE_ERROR;
            out->kind = CSERDE_BOOL;
            out->value.boolean = data[0] == (unsigned char)'1';
          } else {
            out->kind = shape != NULL && shape->kind == CMETA_DATA_BYTES
                            ? CSERDE_BYTES
                            : CSERDE_STRING;
            out->value.slice.data = data;
            out->value.slice.lifetime = CSERDE_VIEW_TRANSIENT;
          }
        } else {
          return CSERDE_SOURCE_ERROR;
        }
      }
      ++reader->column;
      reader->phase = reader->column == state->field_count
                          ? ORM_REDIS_READER_MAP_END : ORM_REDIS_READER_KEY;
      return CSERDE_OK;
    case ORM_REDIS_READER_MAP_END:
      out->kind = CSERDE_MAP_END;
      reader->phase = ORM_REDIS_READER_DONE;
      return CSERDE_OK;
    case ORM_REDIS_READER_DONE: return CSERDE_DONE;
    default: return CSERDE_SOURCE_ERROR;
  }
}

static const cserde_reader_ops orm_redis_reader_ops = {
    sizeof(cserde_reader_ops), CSERDE_READER_OPS_ABI_VERSION,
    orm_redis_reader_next};

static orm_row_cursor_step orm_redis_cursor_next(void *context,
                                                  cserde_reader *out_row) {
  orm_redis_cursor_state *state = (orm_redis_cursor_state *)context;
  orm_row_cursor_step step = ORM_ROW_CURSOR_STEP_INIT;
  orm_redis_driver_step native_step;
  if (state == NULL || out_row == NULL) {
    step.kind = ORM_ROW_CURSOR_ERROR;
    step.status = ORM_STATUS_INVALID_ARGUMENT;
    step.message = "invalid Redis cursor resume";
    return step;
  }
  if (state->terminal) return step;
  orm_redis_release_row(state);
  native_step = state->driver.ops->next(state->driver.context, &state->row);
  if (native_step.kind == ORM_REDIS_DRIVER_WAIT) {
    if (state->row != NULL || !cflow_waitable_valid(&native_step.waitable)) {
      state->terminal = 1;
      step.kind = ORM_ROW_CURSOR_ERROR;
      step.status = ORM_STATUS_INTERNAL_ERROR;
      step.message = "Redis driver returned an invalid WAIT step";
      return step;
    }
    step.kind = ORM_ROW_CURSOR_WAIT;
    step.waitable = native_step.waitable;
    return step;
  }
  if (native_step.kind == ORM_REDIS_DRIVER_DONE) {
    state->terminal = 1;
    return step;
  }
  if (native_step.kind == ORM_REDIS_DRIVER_ERROR || state->row == NULL) {
    state->terminal = 1;
    step.kind = ORM_ROW_CURSOR_ERROR;
    step.status = native_step.status == ORM_STATUS_OK
                      ? ORM_STATUS_DATASTORE_ERROR : native_step.status;
    (void)snprintf(state->error_message, sizeof(state->error_message), "%s",
                   native_step.message != NULL ? native_step.message
                                               : "read Redis row failed");
    step.message = state->error_message;
    return step;
  }
  if (state->rows == state->max_rows) {
    state->terminal = 1;
    step.kind = ORM_ROW_CURSOR_ERROR;
    step.status = ORM_STATUS_LIMIT_EXCEEDED;
    step.message = "Redis result exceeds max_result_rows";
    return step;
  }
  if (state->driver.reply_ops->kind(state->driver.context, state->row) !=
      ORM_REDIS_REPLY_ARRAY) {
    state->terminal = 1;
    step.kind = ORM_ROW_CURSOR_ERROR;
    step.status = ORM_STATUS_DATASTORE_ERROR;
    step.message = "Redis Query Engine returned an invalid row";
    return step;
  }
  ++state->rows;
  state->reader.cursor = state;
  state->reader.column = 0u;
  state->reader.phase = ORM_REDIS_READER_MAP_BEGIN;
  if (cserde_reader_init(out_row, &orm_redis_reader_ops, &state->reader) != CSERDE_OK) {
    state->terminal = 1;
    step.kind = ORM_ROW_CURSOR_ERROR;
    step.status = ORM_STATUS_INTERNAL_ERROR;
    step.message = "initialize Redis row reader";
    return step;
  }
  step.kind = ORM_ROW_CURSOR_ROW;
  return step;
}

static void orm_redis_cursor_cancel(void *context) {
  orm_redis_cursor_state *state = (orm_redis_cursor_state *)context;
  if (state == NULL || state->terminal) return;
  state->terminal = 1;
  orm_redis_release_row(state);
  state->driver.ops->cancel(state->driver.context);
}

static void orm_redis_cursor_destroy(void *context) {
  orm_redis_cursor_state *state = (orm_redis_cursor_state *)context;
  if (state == NULL) return;
  orm_redis_release_row(state);
  state->driver.ops->destroy(state->driver.context);
  free(state->field_names);
  free(state->field_shapes);
  free(state->fields);
  free(state);
}

static const cmeta_data_field_desc *orm_redis_find_shape_field(
    const cmeta_data_struct_shape *shape, const orm_redis_field_view *field) {
  size_t index;
  for (index = 0u; index < shape->field_count; ++index) {
    const cmeta_data_field_desc *candidate =
        cmeta_data_struct_field(shape, index);
    const size_t name_size = candidate != NULL && candidate->name != NULL
                                 ? strlen(candidate->name)
                                 : 0u;
    if (candidate != NULL && candidate->name != NULL &&
        name_size == field->size &&
        (name_size == 0u ||
         memcmp(candidate->name, field->data, name_size) == 0))
      return candidate;
  }
  return NULL;
}

static orm_status_t orm_redis_cursor_configure_shape(
    void *context, const cmeta_data_desc *row_shape, orm_error_t *error) {
  orm_redis_cursor_state *state = (orm_redis_cursor_state *)context;
  const cmeta_data_struct_shape *shape;
  size_t index;
  if (state == NULL || row_shape == NULL ||
      !cmeta_data_desc_valid(row_shape) ||
      row_shape->kind != CMETA_DATA_STRUCT || row_shape->shape == NULL) {
    orm_redis_set_error(error, ORM_STATUS_INVALID_ARGUMENT,
                        "Redis row shape must be a valid struct descriptor");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  shape = (const cmeta_data_struct_shape *)row_shape->shape;
  for (index = 0u; index < state->field_count; ++index) {
    const cmeta_data_field_desc *field =
        orm_redis_find_shape_field(shape, &state->fields[index]);
    if (field == NULL || field->value == NULL ||
        !cmeta_data_desc_valid(field->value)) {
      orm_redis_set_error(error, ORM_STATUS_TYPE_ERROR,
                          "Redis projection is missing from the row shape");
      return ORM_STATUS_TYPE_ERROR;
    }
    switch (field->value->kind) {
      case CMETA_DATA_BOOL:
      case CMETA_DATA_SINT:
      case CMETA_DATA_UINT:
      case CMETA_DATA_FLOAT:
      case CMETA_DATA_STRING:
      case CMETA_DATA_BYTES:
      case CMETA_DATA_ENUM:
        break;
      default:
        orm_redis_set_error(
            error, ORM_STATUS_UNSUPPORTED,
            "Redis bulk-string projection requires a scalar row field");
        return ORM_STATUS_UNSUPPORTED;
    }
    state->field_shapes[index] = field->value;
  }
  orm_redis_set_error(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

static const orm_row_cursor_ops orm_redis_cursor_ops = {
    sizeof(orm_row_cursor_ops), ORM_ROW_CURSOR_OPS_ABI_VERSION,
    "redis-resp-stream", orm_redis_cursor_next, orm_redis_cursor_cancel,
    orm_redis_cursor_destroy, orm_redis_cursor_configure_shape};

orm_status_t orm_redis_cursor_start(
    orm_row_cursor *out_cursor, orm_redis_row_driver *driver,
    const orm_redis_field_view *fields, size_t field_count,
    const orm_redis_cursor_config *config, orm_error_t *error) {
  orm_redis_cursor_state *state;
  size_t field_bytes = 0u;
  size_t index;
  if (out_cursor == NULL || out_cursor->ops != NULL ||
      out_cursor->context != NULL || !orm_redis_driver_valid(driver) ||
      config == NULL || config->struct_size < sizeof(*config) ||
      config->abi_version != ORM_REDIS_CURSOR_CONFIG_ABI_VERSION ||
      config->max_rows == 0u || config->max_field_name_bytes == 0u ||
      config->wait_timeout_ns == 0u ||
      (field_count != 0u && fields == NULL)) {
    orm_redis_set_error(error, ORM_STATUS_INVALID_ARGUMENT,
                        "invalid Redis cursor configuration");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  for (index = 0u; index < field_count; ++index) {
    if ((fields[index].size != 0u && fields[index].data == NULL) ||
        field_bytes > config->max_field_name_bytes ||
        fields[index].size > config->max_field_name_bytes - field_bytes) {
      orm_redis_set_error(error, ORM_STATUS_LIMIT_EXCEEDED,
                          "Redis field names exceed configured bounds");
      return ORM_STATUS_LIMIT_EXCEEDED;
    }
    field_bytes += fields[index].size;
  }
  if (field_count > SIZE_MAX / sizeof(*state->fields)) {
    orm_redis_set_error(error, ORM_STATUS_LIMIT_EXCEEDED,
                        "Redis field metadata size overflows");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  state = (orm_redis_cursor_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    orm_redis_set_error(error, ORM_STATUS_OUT_OF_MEMORY, NULL);
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  state->fields = (orm_redis_field_view *)calloc(
      field_count == 0u ? 1u : field_count, sizeof(*state->fields));
  state->field_shapes = (const cmeta_data_desc **)calloc(
      field_count == 0u ? 1u : field_count, sizeof(*state->field_shapes));
  state->field_names = (unsigned char *)malloc(field_bytes == 0u ? 1u : field_bytes);
  if (state->fields == NULL || state->field_shapes == NULL ||
      state->field_names == NULL) {
    free(state->field_names);
    free(state->field_shapes);
    free(state->fields);
    free(state);
    orm_redis_set_error(error, ORM_STATUS_OUT_OF_MEMORY, NULL);
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  field_bytes = 0u;
  for (index = 0u; index < field_count; ++index) {
    state->fields[index].data = state->field_names + field_bytes;
    state->fields[index].size = fields[index].size;
    if (fields[index].size != 0u)
      memcpy(state->field_names + field_bytes, fields[index].data,
             fields[index].size);
    field_bytes += fields[index].size;
  }
  state->driver = *driver;
  memset(driver, 0, sizeof(*driver));
  state->field_count = field_count;
  state->max_rows = config->max_rows;
  out_cursor->ops = &orm_redis_cursor_ops;
  out_cursor->context = state;
  out_cursor->wait_timeout_ns = config->wait_timeout_ns;
  orm_redis_set_error(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}
