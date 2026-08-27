#include "orm_tidesdb_cursor.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  ORM_TIDESDB_MAGIC_SIZE = 8u,
  ORM_TIDESDB_ROW_HEADER_SIZE = 12u,
  ORM_TIDESDB_FIELD_HEADER_SIZE = 8u,
  ORM_TIDESDB_NULL_FLAG = 1u
};

typedef enum orm_tidesdb_reader_phase {
  ORM_TIDESDB_READER_MAP_BEGIN = 0,
  ORM_TIDESDB_READER_KEY,
  ORM_TIDESDB_READER_VALUE,
  ORM_TIDESDB_READER_MAP_END,
  ORM_TIDESDB_READER_DONE
} orm_tidesdb_reader_phase;

typedef struct orm_tidesdb_cursor_state orm_tidesdb_cursor_state;

typedef struct orm_tidesdb_reader_state {
  orm_tidesdb_cursor_state *cursor;
  size_t offset;
  size_t fields_left;
  orm_tidesdb_reader_phase phase;
  orm_value_kind_t value_kind;
  const unsigned char *value;
  size_t value_size;
} orm_tidesdb_reader_state;

struct orm_tidesdb_cursor_state {
  orm_tidesdb_driver driver;
  size_t max_rows;
  size_t max_row_bytes;
  size_t max_fields;
  size_t rows;
  const unsigned char *row;
  size_t row_size;
  int terminal;
  orm_tidesdb_reader_state reader;
  char error_message[ORM_C_ERROR_MESSAGE_CAPACITY];
};

static const unsigned char orm_tidesdb_magic[ORM_TIDESDB_MAGIC_SIZE] = {
    'O', 'R', 'M', 'T', 'D', 'B', 1u, 0u};

static void orm_tidesdb_set_error(orm_error_t *error, orm_status_t status,
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

static int orm_tidesdb_driver_valid(const orm_tidesdb_driver *driver) {
  return driver != NULL && driver->ops != NULL && driver->context != NULL &&
         driver->ops->struct_size >= sizeof(*driver->ops) &&
         driver->ops->abi_version == ORM_TIDESDB_DRIVER_OPS_ABI_VERSION &&
         driver->ops->next != NULL && driver->ops->release_row != NULL &&
         driver->ops->destroy != NULL;
}

static uint16_t orm_tidesdb_read_u16(const unsigned char *data) {
  return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8u));
}

static uint32_t orm_tidesdb_read_u32(const unsigned char *data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
         ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

static int orm_tidesdb_contains_nul(const unsigned char *data, size_t size) {
  return size != 0u && memchr(data, 0, size) != NULL;
}

static int orm_tidesdb_parse_uint64(const unsigned char *data, size_t size,
                                    uint64_t limit, uint64_t *out) {
  uint64_t value = 0u;
  size_t index;
  if (size == 0u)
    return 0;
  for (index = 0u; index < size; ++index) {
    const unsigned char digit = data[index];
    if (digit < '0' || digit > '9')
      return 0;
    if (value > (limit - (uint64_t)(digit - '0')) / 10u)
      return 0;
    value = value * 10u + (uint64_t)(digit - '0');
  }
  *out = value;
  return 1;
}

static int orm_tidesdb_parse_sint64(const unsigned char *data, size_t size,
                                    int64_t *out) {
  uint64_t magnitude;
  size_t offset = 0u;
  uint64_t limit = (uint64_t)INT64_MAX;
  if (size != 0u && data[0] == '-') {
    offset = 1u;
    limit += 1u;
  }
  if (!orm_tidesdb_parse_uint64(data + offset, size - offset, limit,
                                &magnitude))
    return 0;
  if (offset != 0u) {
    *out = magnitude == (uint64_t)INT64_MAX + 1u
               ? INT64_MIN
               : -(int64_t)magnitude;
  } else {
    *out = (int64_t)magnitude;
  }
  return 1;
}

static int orm_tidesdb_parse_double(const unsigned char *data, size_t size,
                                    double *out) {
  size_t index = 0u;
  int sign = 1;
  int exponent_sign = 1;
  int exponent = 0;
  int digits = 0;
  int fractional_digits = 0;
  long double value = 0.0L;
  if (size != 0u && (data[index] == '-' || data[index] == '+')) {
    if (data[index++] == '-')
      sign = -1;
  }
  while (index < size && data[index] >= '0' && data[index] <= '9') {
    value = value * 10.0L + (long double)(data[index++] - '0');
    ++digits;
  }
  if (index < size && data[index] == '.') {
    ++index;
    while (index < size && data[index] >= '0' && data[index] <= '9') {
      value = value * 10.0L + (long double)(data[index++] - '0');
      ++digits;
      ++fractional_digits;
    }
  }
  if (digits == 0)
    return 0;
  if (index < size && (data[index] == 'e' || data[index] == 'E')) {
    int exponent_digits = 0;
    ++index;
    if (index < size && (data[index] == '-' || data[index] == '+')) {
      if (data[index++] == '-')
        exponent_sign = -1;
    }
    while (index < size && data[index] >= '0' && data[index] <= '9') {
      if (exponent < 10000)
        exponent = exponent * 10 + (int)(data[index] - '0');
      ++index;
      ++exponent_digits;
    }
    if (exponent_digits == 0)
      return 0;
  }
  if (index != size)
    return 0;
  exponent = exponent_sign * exponent - fractional_digits;
  if (exponent > DBL_MAX_10_EXP || exponent < DBL_MIN_10_EXP - DBL_MANT_DIG)
    return 0;
  value *= powl(10.0L, (long double)exponent);
  value *= (long double)sign;
  *out = (double)value;
  return isfinite(*out) != 0;
}

static int orm_tidesdb_kind_valid(orm_value_kind_t kind) {
  return kind >= ORM_VALUE_NULL && kind <= ORM_VALUE_BLOB;
}

static int orm_tidesdb_value_valid(orm_value_kind_t kind, unsigned flags,
                                   const unsigned char *value,
                                   size_t value_size) {
  int64_t sint_value;
  uint64_t uint_value;
  double double_value;
  const int is_null = (flags & ORM_TIDESDB_NULL_FLAG) != 0u;
  if ((flags & ~ORM_TIDESDB_NULL_FLAG) != 0u || !orm_tidesdb_kind_valid(kind))
    return 0;
  if (is_null)
    return kind == ORM_VALUE_NULL && value_size == 0u;
  if (kind == ORM_VALUE_NULL ||
      (kind != ORM_VALUE_BLOB && orm_tidesdb_contains_nul(value, value_size)))
    return 0;
  switch (kind) {
    case ORM_VALUE_INT64:
      return orm_tidesdb_parse_sint64(value, value_size, &sint_value);
    case ORM_VALUE_UINT64:
      return orm_tidesdb_parse_uint64(value, value_size, UINT64_MAX,
                                      &uint_value);
    case ORM_VALUE_DOUBLE:
      return orm_tidesdb_parse_double(value, value_size, &double_value);
    case ORM_VALUE_BOOLEAN:
      return value_size == 1u && (value[0] == '0' || value[0] == '1');
    case ORM_VALUE_TEXT:
    case ORM_VALUE_BLOB:
      return 1;
    default:
      return 0;
  }
}

static orm_status_t orm_tidesdb_validate_row(
    const orm_tidesdb_cursor_state *state, const unsigned char *row,
    size_t row_size) {
  size_t offset = ORM_TIDESDB_ROW_HEADER_SIZE;
  uint32_t field_count;
  uint32_t index;
  if (row == NULL || row_size < ORM_TIDESDB_ROW_HEADER_SIZE ||
      row_size > state->max_row_bytes)
    return row_size > state->max_row_bytes ? ORM_STATUS_LIMIT_EXCEEDED
                                           : ORM_STATUS_DATASTORE_ERROR;
  if (memcmp(row, orm_tidesdb_magic, ORM_TIDESDB_MAGIC_SIZE) != 0)
    return ORM_STATUS_DATASTORE_ERROR;
  field_count = orm_tidesdb_read_u32(row + ORM_TIDESDB_MAGIC_SIZE);
  if ((size_t)field_count > state->max_fields)
    return ORM_STATUS_LIMIT_EXCEEDED;
  for (index = 0u; index < field_count; ++index) {
    uint16_t name_size;
    orm_value_kind_t kind;
    unsigned flags;
    uint32_t value_size;
    if (offset > row_size ||
        ORM_TIDESDB_FIELD_HEADER_SIZE > row_size - offset)
      return ORM_STATUS_DATASTORE_ERROR;
    name_size = orm_tidesdb_read_u16(row + offset);
    kind = (orm_value_kind_t)row[offset + 2u];
    flags = row[offset + 3u];
    value_size = orm_tidesdb_read_u32(row + offset + 4u);
    offset += ORM_TIDESDB_FIELD_HEADER_SIZE;
    if (name_size == 0u || (size_t)name_size > row_size - offset ||
        orm_tidesdb_contains_nul(row + offset, name_size))
      return ORM_STATUS_DATASTORE_ERROR;
    offset += name_size;
    if ((size_t)value_size > row_size - offset ||
        !orm_tidesdb_value_valid(kind, flags, row + offset, value_size))
      return ORM_STATUS_DATASTORE_ERROR;
    offset += value_size;
  }
  return offset == row_size ? ORM_STATUS_OK : ORM_STATUS_DATASTORE_ERROR;
}

static cserde_status orm_tidesdb_reader_next(void *context,
                                             cserde_token *out) {
  orm_tidesdb_reader_state *reader = (orm_tidesdb_reader_state *)context;
  orm_tidesdb_cursor_state *state;
  const unsigned char *row;
  if (reader == NULL || out == NULL || reader->cursor == NULL)
    return CSERDE_INVALID_ARGUMENT;
  state = reader->cursor;
  row = state->row;
  memset(out, 0, sizeof(*out));
  switch (reader->phase) {
    case ORM_TIDESDB_READER_MAP_BEGIN:
      out->kind = CSERDE_MAP_BEGIN;
      reader->phase = reader->fields_left == 0u
                          ? ORM_TIDESDB_READER_MAP_END
                          : ORM_TIDESDB_READER_KEY;
      return CSERDE_OK;
    case ORM_TIDESDB_READER_KEY: {
      const uint16_t name_size = orm_tidesdb_read_u16(row + reader->offset);
      reader->value_kind = (orm_value_kind_t)row[reader->offset + 2u];
      reader->value = row + reader->offset + ORM_TIDESDB_FIELD_HEADER_SIZE +
                      name_size;
      reader->value_size = orm_tidesdb_read_u32(row + reader->offset + 4u);
      out->kind = CSERDE_STRING;
      out->value.slice.data = row + reader->offset + ORM_TIDESDB_FIELD_HEADER_SIZE;
      out->value.slice.size = name_size;
      out->value.slice.lifetime = CSERDE_VIEW_TRANSIENT;
      reader->offset += ORM_TIDESDB_FIELD_HEADER_SIZE + name_size;
      reader->phase = ORM_TIDESDB_READER_VALUE;
      return CSERDE_OK;
    }
    case ORM_TIDESDB_READER_VALUE:
      switch (reader->value_kind) {
        case ORM_VALUE_NULL:
          out->kind = CSERDE_NULL;
          break;
        case ORM_VALUE_INT64:
          out->kind = CSERDE_SINT;
          (void)orm_tidesdb_parse_sint64(reader->value, reader->value_size,
                                         &out->value.sint);
          break;
        case ORM_VALUE_UINT64:
          out->kind = CSERDE_UINT;
          (void)orm_tidesdb_parse_uint64(reader->value, reader->value_size,
                                         UINT64_MAX, &out->value.uint);
          break;
        case ORM_VALUE_DOUBLE:
          out->kind = CSERDE_FLOAT;
          (void)orm_tidesdb_parse_double(reader->value, reader->value_size,
                                         &out->value.floating);
          break;
        case ORM_VALUE_BOOLEAN:
          out->kind = CSERDE_BOOL;
          out->value.boolean = reader->value[0] == '1';
          break;
        case ORM_VALUE_TEXT:
        case ORM_VALUE_BLOB:
          out->kind = reader->value_kind == ORM_VALUE_TEXT
                          ? CSERDE_STRING
                          : CSERDE_BYTES;
          out->value.slice.data = reader->value;
          out->value.slice.size = reader->value_size;
          out->value.slice.lifetime = CSERDE_VIEW_TRANSIENT;
          break;
        default:
          return CSERDE_SOURCE_ERROR;
      }
      reader->offset += reader->value_size;
      --reader->fields_left;
      reader->phase = reader->fields_left == 0u
                          ? ORM_TIDESDB_READER_MAP_END
                          : ORM_TIDESDB_READER_KEY;
      return CSERDE_OK;
    case ORM_TIDESDB_READER_MAP_END:
      out->kind = CSERDE_MAP_END;
      reader->phase = ORM_TIDESDB_READER_DONE;
      return CSERDE_OK;
    case ORM_TIDESDB_READER_DONE:
      return CSERDE_DONE;
    default:
      return CSERDE_SOURCE_ERROR;
  }
}

static const cserde_reader_ops orm_tidesdb_reader_ops = {
    sizeof(cserde_reader_ops), CSERDE_READER_OPS_ABI_VERSION,
    orm_tidesdb_reader_next};

static void orm_tidesdb_release_current(orm_tidesdb_cursor_state *state) {
  if (state->row == NULL)
    return;
  state->driver.ops->release_row(state->driver.context, state->row);
  state->row = NULL;
  state->row_size = 0u;
}

static orm_row_cursor_step orm_tidesdb_cursor_next(void *context,
                                                   cserde_reader *out_row) {
  orm_tidesdb_cursor_state *state = (orm_tidesdb_cursor_state *)context;
  orm_row_cursor_step step = ORM_ROW_CURSOR_STEP_INIT;
  orm_tidesdb_driver_step driver_step;
  orm_status_t status;
  const unsigned char *row = NULL;
  size_t row_size = 0u;
  if (state == NULL || out_row == NULL) {
    step.kind = ORM_ROW_CURSOR_ERROR;
    step.status = ORM_STATUS_INVALID_ARGUMENT;
    step.message = "invalid TidesDB cursor resume";
    return step;
  }
  orm_tidesdb_release_current(state);
  if (state->terminal)
    return step;
  driver_step = state->driver.ops->next(state->driver.context, &row, &row_size);
  if (driver_step.kind == ORM_TIDESDB_DRIVER_DONE) {
    state->terminal = 1;
    return step;
  }
  if (driver_step.kind == ORM_TIDESDB_DRIVER_ERROR || row == NULL) {
    state->terminal = 1;
    step.kind = ORM_ROW_CURSOR_ERROR;
    step.status = driver_step.status == ORM_STATUS_OK
                      ? ORM_STATUS_DATASTORE_ERROR
                      : driver_step.status;
    step.message = driver_step.message != NULL
                       ? driver_step.message
                       : "iterate TidesDB cursor";
    return step;
  }
  state->row = row;
  state->row_size = row_size;
  if (state->rows == state->max_rows) {
    status = ORM_STATUS_LIMIT_EXCEEDED;
  } else {
    status = orm_tidesdb_validate_row(state, row, row_size);
  }
  if (status != ORM_STATUS_OK) {
    state->terminal = 1;
    (void)snprintf(state->error_message, sizeof(state->error_message), "%s",
                   status == ORM_STATUS_LIMIT_EXCEEDED
                       ? "TidesDB row exceeds configured bounds"
                       : "invalid TidesDB stored row encoding");
    step.kind = ORM_ROW_CURSOR_ERROR;
    step.status = status;
    step.message = state->error_message;
    return step;
  }
  state->reader.cursor = state;
  state->reader.offset = ORM_TIDESDB_ROW_HEADER_SIZE;
  state->reader.fields_left =
      orm_tidesdb_read_u32(row + ORM_TIDESDB_MAGIC_SIZE);
  state->reader.phase = ORM_TIDESDB_READER_MAP_BEGIN;
  if (cserde_reader_init(out_row, &orm_tidesdb_reader_ops,
                         &state->reader) != CSERDE_OK) {
    state->terminal = 1;
    step.kind = ORM_ROW_CURSOR_ERROR;
    step.status = ORM_STATUS_INTERNAL_ERROR;
    step.message = "initialize TidesDB row reader";
    return step;
  }
  ++state->rows;
  step.kind = ORM_ROW_CURSOR_ROW;
  return step;
}

static void orm_tidesdb_cursor_cancel(void *context) {
  orm_tidesdb_cursor_state *state = (orm_tidesdb_cursor_state *)context;
  if (state != NULL)
    state->terminal = 1;
}

static void orm_tidesdb_cursor_destroy(void *context) {
  orm_tidesdb_cursor_state *state = (orm_tidesdb_cursor_state *)context;
  if (state == NULL)
    return;
  orm_tidesdb_release_current(state);
  state->driver.ops->destroy(state->driver.context);
  free(state);
}

static const orm_row_cursor_ops orm_tidesdb_cursor_ops = {
    sizeof(orm_row_cursor_ops), ORM_ROW_CURSOR_OPS_ABI_VERSION,
    "tidesdb-row", orm_tidesdb_cursor_next, orm_tidesdb_cursor_cancel,
    orm_tidesdb_cursor_destroy, NULL};

orm_status_t orm_tidesdb_cursor_start(
    orm_row_cursor *out_cursor, orm_tidesdb_driver *driver,
    const orm_tidesdb_cursor_config *config, orm_error_t *error) {
  orm_tidesdb_cursor_state *state;
  if (out_cursor == NULL || out_cursor->ops != NULL ||
      out_cursor->context != NULL || !orm_tidesdb_driver_valid(driver) ||
      config == NULL || config->struct_size < sizeof(*config) ||
      config->abi_version != ORM_TIDESDB_CURSOR_CONFIG_ABI_VERSION ||
      config->max_rows == 0u ||
      config->max_row_bytes < ORM_TIDESDB_ROW_HEADER_SIZE ||
      config->max_fields == 0u) {
    orm_tidesdb_set_error(error, ORM_STATUS_INVALID_ARGUMENT,
                          "invalid TidesDB cursor configuration");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  state = (orm_tidesdb_cursor_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    orm_tidesdb_set_error(error, ORM_STATUS_OUT_OF_MEMORY, NULL);
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  state->driver = *driver;
  state->max_rows = config->max_rows;
  state->max_row_bytes = config->max_row_bytes;
  state->max_fields = config->max_fields;
  driver->ops = NULL;
  driver->context = NULL;
  out_cursor->ops = &orm_tidesdb_cursor_ops;
  out_cursor->context = state;
  orm_tidesdb_set_error(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}
