#include "row.h"

#include <float.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  ORM_TIDESDB_MAGIC_SIZE = 8u,
  ORM_TIDESDB_ROW_HEADER_SIZE = 12u,
  ORM_TIDESDB_FIELD_HEADER_SIZE = 8u,
  ORM_TIDESDB_NULL_FLAG = 1u,
  ORM_TIDESDB_FIELD_NAME_MAX = UINT16_MAX,
  ORM_TIDESDB_NUMBER_BUFFER_SIZE = 128u
};

static const unsigned char orm_tidesdb_magic[ORM_TIDESDB_MAGIC_SIZE] = {
    'O', 'R', 'M', 'T', 'D', 'B', 1u, 0u};

static orm_status_t orm_tidesdb_fail(orm_error_t *error,
                                     orm_status_t status,
                                     const char *message) {
  orm_error_set(error, status, message);
  return status;
}

static uint16_t orm_tidesdb_read_u16(const unsigned char *data) {
  return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8u));
}

static uint32_t orm_tidesdb_read_u32(const unsigned char *data) {
  return (uint32_t)data[0] | ((uint32_t)data[1] << 8u) |
         ((uint32_t)data[2] << 16u) | ((uint32_t)data[3] << 24u);
}

static orm_status_t orm_tidesdb_append(tstr *output, const void *data,
                                       size_t size, size_t max_bytes,
                                       orm_error_t *error) {
  tstr next;
  if (size > max_bytes || tstr_len(*output) > max_bytes - size)
    return orm_tidesdb_fail(error, ORM_STATUS_LIMIT_EXCEEDED,
                            "encoded TidesDB row exceeds its byte limit");
  next = tstr_cat_len(*output, (const char *)data, size);
  if (next == NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                            "append encoded TidesDB row");
  *output = next;
  return ORM_STATUS_OK;
}

static orm_status_t orm_tidesdb_append_u16(tstr *output, uint16_t value,
                                           size_t max_bytes,
                                           orm_error_t *error) {
  const unsigned char encoded[2] = {
      (unsigned char)value, (unsigned char)(value >> 8u)};
  return orm_tidesdb_append(output, encoded, sizeof(encoded), max_bytes,
                            error);
}

static orm_status_t orm_tidesdb_append_u32(tstr *output, uint32_t value,
                                           size_t max_bytes,
                                           orm_error_t *error) {
  const unsigned char encoded[4] = {
      (unsigned char)value, (unsigned char)(value >> 8u),
      (unsigned char)(value >> 16u), (unsigned char)(value >> 24u)};
  return orm_tidesdb_append(output, encoded, sizeof(encoded), max_bytes,
                            error);
}

static int orm_tidesdb_parse_uint64(const unsigned char *data, size_t size,
                                    uint64_t limit, uint64_t *out) {
  uint64_t value = 0u;
  size_t index;
  if (size == 0u)
    return 0;
  for (index = 0u; index < size; ++index) {
    const unsigned char digit = data[index];
    if (digit < (unsigned char)'0' || digit > (unsigned char)'9')
      return 0;
    if (value > (limit - (uint64_t)(digit - (unsigned char)'0')) / 10u)
      return 0;
    value = value * 10u + (uint64_t)(digit - (unsigned char)'0');
  }
  *out = value;
  return 1;
}

static int orm_tidesdb_parse_sint64(const unsigned char *data, size_t size,
                                    int64_t *out) {
  uint64_t magnitude;
  size_t offset = 0u;
  uint64_t limit = (uint64_t)INT64_MAX;
  if (size != 0u && data[0] == (unsigned char)'-') {
    offset = 1u;
    ++limit;
  }
  if (!orm_tidesdb_parse_uint64(data + offset, size - offset, limit,
                                &magnitude))
    return 0;
  *out = offset != 0u
             ? (magnitude == (uint64_t)INT64_MAX + 1u
                    ? INT64_MIN
                    : -(int64_t)magnitude)
             : (int64_t)magnitude;
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
  if (index < size && (data[index] == (unsigned char)'-' ||
                       data[index] == (unsigned char)'+')) {
    if (data[index++] == (unsigned char)'-')
      sign = -1;
  }
  while (index < size && data[index] >= (unsigned char)'0' &&
         data[index] <= (unsigned char)'9') {
    value = value * 10.0L + (long double)(data[index++] - (unsigned char)'0');
    ++digits;
  }
  if (index < size && data[index] == (unsigned char)'.') {
    ++index;
    while (index < size && data[index] >= (unsigned char)'0' &&
           data[index] <= (unsigned char)'9') {
      value = value * 10.0L +
              (long double)(data[index++] - (unsigned char)'0');
      ++digits;
      ++fractional_digits;
    }
  }
  if (digits == 0)
    return 0;
  if (index < size &&
      (data[index] == (unsigned char)'e' ||
       data[index] == (unsigned char)'E')) {
    int exponent_digits = 0;
    ++index;
    if (index < size && (data[index] == (unsigned char)'-' ||
                         data[index] == (unsigned char)'+')) {
      if (data[index++] == (unsigned char)'-')
        exponent_sign = -1;
    }
    while (index < size && data[index] >= (unsigned char)'0' &&
           data[index] <= (unsigned char)'9') {
      if (exponent < 10000)
        exponent = exponent * 10 + (int)(data[index] - (unsigned char)'0');
      ++index;
      ++exponent_digits;
    }
    if (exponent_digits == 0)
      return 0;
  }
  if (index != size)
    return 0;
  exponent = exponent_sign * exponent - fractional_digits;
  if (exponent > DBL_MAX_10_EXP ||
      exponent < DBL_MIN_10_EXP - DBL_MANT_DIG)
    return 0;
  value *= powl(10.0L, (long double)exponent);
  value *= (long double)sign;
  *out = (double)value;
  return isfinite(*out) != 0;
}

static int orm_tidesdb_read_value(orm_value_kind_t kind, bool is_null,
    const unsigned char *bytes, size_t size, orm_value_t *out) {
  if (kind < ORM_VALUE_NULL || kind > ORM_VALUE_BLOB) return 0;
  if (is_null) { *out = orm_null(); return kind == ORM_VALUE_NULL && size == 0; }
  if (kind == ORM_VALUE_NULL || !bytes ||
      (kind != ORM_VALUE_BLOB && memchr(bytes, 0, size))) return 0;
  orm_value_t value = {.kind = kind};
  switch (kind) {
    case ORM_VALUE_INT64:
      if (!orm_tidesdb_parse_sint64(bytes, size, &value.data.int64_value)) return 0;
      break;
    case ORM_VALUE_UINT64:
      if (!orm_tidesdb_parse_uint64(bytes, size, UINT64_MAX, &value.data.uint64_value)) return 0;
      break;
    case ORM_VALUE_DOUBLE:
      if (!orm_tidesdb_parse_double(bytes, size, &value.data.double_value)) return 0;
      break;
    case ORM_VALUE_BOOLEAN:
      if (size != 1 || (bytes[0] != '0' && bytes[0] != '1')) return 0;
      value.data.boolean_value = bytes[0] == '1'; break;
    case ORM_VALUE_TEXT: value = orm_text_v((vstr){(const char *)bytes, size}); break;
    case ORM_VALUE_BLOB: value = orm_blob(bytes, size); break;
    default: return 0;
  }
  *out = value;
  return 1;
}

static int orm_tidesdb_cell_valid(const orm_tidesdb_cell *cell) {
  orm_value_t ignored;
  return cell && orm_tidesdb_read_value(cell->kind, cell->is_null,
      (const unsigned char *)cell->bytes, tstr_len(cell->bytes), &ignored);
}

static void orm_tidesdb_cell_destroy(orm_tidesdb_cell *cell) {
  if (cell == NULL)
    return;
  tstr_freep(&cell->bytes);
  memset(cell, 0, sizeof(*cell));
}

static orm_status_t orm_tidesdb_cell_copy(orm_tidesdb_cell *out,
                                          const orm_tidesdb_cell *input,
                                          orm_error_t *error) {
  memset(out, 0, sizeof(*out));
  if (!orm_tidesdb_cell_valid(input))
    return orm_tidesdb_fail(error, ORM_STATUS_DATASTORE_ERROR,
                            "invalid TidesDB row cell");
  out->kind = input->kind;
  out->is_null = input->is_null;
  out->bytes = tstr_clone(input->bytes);
  if (out->bytes == NULL) {
    if (input->is_null)
      out->bytes = tstr_new();
    if (out->bytes == NULL)
      return orm_tidesdb_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                              "copy TidesDB row cell");
  }
  return ORM_STATUS_OK;
}

orm_status_t orm_tidesdb_row_init(orm_tidesdb_row *row, size_t max_fields,
                                  orm_error_t *error) {
  if (row == NULL || max_fields == 0u)
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                            "invalid TidesDB row configuration");
  memset(row, 0, sizeof(*row));
  if (vec_init_bytes(&row->fields, sizeof(orm_tidesdb_field),
                     _Alignof(orm_tidesdb_field), max_fields) != STL_OK)
    return orm_tidesdb_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                            "initialize TidesDB row fields");
  return ORM_STATUS_OK;
}

void orm_tidesdb_row_destroy(orm_tidesdb_row *row) {
  size_t index;
  if (row == NULL)
    return;
  for (index = 0u; index < vec_size(&row->fields); ++index) {
    orm_tidesdb_field *field =
        (orm_tidesdb_field *)vec_at(&row->fields, index);
    if (field != NULL) {
      tstr_freep(&field->name);
      orm_tidesdb_cell_destroy(&field->value);
    }
  }
  vec_destroy(&row->fields);
  memset(row, 0, sizeof(*row));
}

static orm_status_t orm_tidesdb_number_cell(orm_tidesdb_cell *out_cell,
                                            const char *text, int written,
                                            orm_error_t *error) {
  if (written <= 0 || (size_t)written >= ORM_TIDESDB_NUMBER_BUFFER_SIZE)
    return orm_tidesdb_fail(error, ORM_STATUS_INTERNAL_ERROR,
                            "format TidesDB numeric value");
  out_cell->bytes = tstr_dup_len(text, (size_t)written);
  if (out_cell->bytes == NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                            "allocate TidesDB numeric value");
  return ORM_STATUS_OK;
}

static int orm_tidesdb_normalize_decimal(char *buffer, int written) {
  const struct lconv *format = localeconv();
  const char *point = format != NULL ? format->decimal_point : NULL;
  size_t point_size;
  char *position;
  if (point == NULL || strcmp(point, ".") == 0)
    return written;
  point_size = strlen(point);
  if (point_size == 0u)
    return -1;
  position = strstr(buffer, point);
  if (position == NULL)
    return written;
  *position = '.';
  if (point_size > 1u) {
    memmove(position + 1u, position + point_size,
            (size_t)written - (size_t)(position - buffer) - point_size + 1u);
    written -= (int)(point_size - 1u);
  }
  return written;
}

orm_status_t orm_tidesdb_cell_from_value(orm_tidesdb_cell *out_cell,
                                         const orm_owned_value *value,
                                         orm_error_t *error) {
  char buffer[ORM_TIDESDB_NUMBER_BUFFER_SIZE];
  int written;
  if (out_cell == NULL || value == NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                            "invalid TidesDB value conversion");
  memset(out_cell, 0, sizeof(*out_cell));
  out_cell->kind = value->kind;
  out_cell->is_null = value->kind == ORM_VALUE_NULL;
  switch (value->kind) {
    case ORM_VALUE_NULL:
      out_cell->bytes = tstr_new();
      break;
    case ORM_VALUE_INT64:
      written = snprintf(buffer, sizeof(buffer), "%lld",
                         (long long)value->data.int64_value);
      return orm_tidesdb_number_cell(out_cell, buffer, written, error);
    case ORM_VALUE_UINT64:
      written = snprintf(buffer, sizeof(buffer), "%llu",
                         (unsigned long long)value->data.uint64_value);
      return orm_tidesdb_number_cell(out_cell, buffer, written, error);
    case ORM_VALUE_DOUBLE:
      if (!isfinite(value->data.double_value))
        return orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                                "TidesDB double value must be finite");
      written = snprintf(buffer, sizeof(buffer), "%.17g",
                         value->data.double_value);
      written = orm_tidesdb_normalize_decimal(buffer, written);
      return orm_tidesdb_number_cell(out_cell, buffer, written, error);
    case ORM_VALUE_BOOLEAN:
      out_cell->bytes = tstr_dup(value->data.boolean_value != 0u ? "1" : "0");
      break;
    case ORM_VALUE_TEXT:
    case ORM_VALUE_BLOB:
      out_cell->bytes = tstr_clone(value->bytes);
      if (out_cell->bytes == NULL && value->bytes == NULL)
        out_cell->bytes = tstr_new();
      break;
    default:
      return orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                              "unknown TidesDB value kind");
  }
  if (out_cell->bytes == NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                            "allocate TidesDB row value");
  return ORM_STATUS_OK;
}

const orm_tidesdb_cell *orm_tidesdb_row_find(const orm_tidesdb_row *row,
                                             vstr name) {
  size_t index;
  if (row == NULL || !orm_view_valid(name, false))
    return NULL;
  for (index = 0u; index < vec_size(&row->fields); ++index) {
    const orm_tidesdb_field *field =
        (const orm_tidesdb_field *)vec_at_const(&row->fields, index);
    if (field != NULL && tstr_eq_v(field->name, name))
      return &field->value;
  }
  return NULL;
}

orm_status_t orm_tidesdb_row_set(orm_tidesdb_row *row, vstr name,
                                 const orm_tidesdb_cell *cell,
                                 orm_error_t *error) {
  orm_tidesdb_field field = {0};
  size_t index;
  orm_status_t status;
  if (row == NULL || !row->fields.initialized ||
      !orm_view_valid(name, false) || name.len > ORM_TIDESDB_FIELD_NAME_MAX ||
      memchr(name.data, 0, name.len) != NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                            "invalid TidesDB field name");
  for (index = 0u; index < vec_size(&row->fields); ++index) {
    orm_tidesdb_field *current =
        (orm_tidesdb_field *)vec_at(&row->fields, index);
    if (current != NULL && tstr_eq_v(current->name, name)) {
      orm_tidesdb_cell replacement = {0};
      status = orm_tidesdb_cell_copy(&replacement, cell, error);
      if (status != ORM_STATUS_OK)
        return status;
      orm_tidesdb_cell_destroy(&current->value);
      current->value = replacement;
      return ORM_STATUS_OK;
    }
  }
  field.name = tstr_from_v(name);
  if (field.name == NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                            "copy TidesDB field name");
  status = orm_tidesdb_cell_copy(&field.value, cell, error);
  if (status != ORM_STATUS_OK) {
    tstr_freep(&field.name);
    return status;
  }
  {
    const stl_status pushed = vec_push(&row->fields, &field);
    if (pushed != STL_OK) {
      tstr_freep(&field.name);
      orm_tidesdb_cell_destroy(&field.value);
      return orm_tidesdb_fail(
          error, pushed == STL_CAPACITY_EXCEEDED ? ORM_STATUS_LIMIT_EXCEEDED
                                                 : ORM_STATUS_OUT_OF_MEMORY,
          "TidesDB row field count exceeds its limit");
    }
  }
  return ORM_STATUS_OK;
}

orm_status_t orm_tidesdb_row_encode(const orm_tidesdb_row *row,
                                    size_t max_bytes, tstr *out_bytes,
                                    orm_error_t *error) {
  tstr encoded;
  size_t index;
  orm_status_t status;
  if (row == NULL || !row->fields.initialized || out_bytes == NULL ||
      *out_bytes != NULL || max_bytes < ORM_TIDESDB_ROW_HEADER_SIZE ||
      vec_size(&row->fields) > UINT32_MAX)
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                            "invalid TidesDB row encoding request");
  encoded = tstr_new();
  if (encoded == NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                            "allocate encoded TidesDB row");
  status = orm_tidesdb_append(&encoded, orm_tidesdb_magic,
                              sizeof(orm_tidesdb_magic), max_bytes, error);
  if (status == ORM_STATUS_OK)
    status = orm_tidesdb_append_u32(&encoded,
                                    (uint32_t)vec_size(&row->fields),
                                    max_bytes, error);
  for (index = 0u; status == ORM_STATUS_OK &&
                   index < vec_size(&row->fields); ++index) {
    const orm_tidesdb_field *field =
        (const orm_tidesdb_field *)vec_at_const(&row->fields, index);
    const size_t name_size = field != NULL ? tstr_len(field->name) : 0u;
    const size_t value_size = field != NULL ? tstr_len(field->value.bytes) : 0u;
    const unsigned char kind =
        field != NULL ? (unsigned char)field->value.kind : 0u;
    const unsigned char flags =
        field != NULL && field->value.is_null ? ORM_TIDESDB_NULL_FLAG : 0u;
    if (field == NULL || name_size == 0u || name_size > UINT16_MAX ||
        value_size > UINT32_MAX || memchr(field->name, 0, name_size) != NULL ||
        !orm_tidesdb_cell_valid(&field->value)) {
      status = orm_tidesdb_fail(error, ORM_STATUS_DATASTORE_ERROR,
                                "invalid TidesDB row field");
      break;
    }
    status = orm_tidesdb_append_u16(&encoded, (uint16_t)name_size,
                                    max_bytes, error);
    if (status == ORM_STATUS_OK)
      status = orm_tidesdb_append(&encoded, &kind, sizeof(kind), max_bytes,
                                  error);
    if (status == ORM_STATUS_OK)
      status = orm_tidesdb_append(&encoded, &flags, sizeof(flags), max_bytes,
                                  error);
    if (status == ORM_STATUS_OK)
      status = orm_tidesdb_append_u32(&encoded, (uint32_t)value_size,
                                      max_bytes, error);
    if (status == ORM_STATUS_OK)
      status = orm_tidesdb_append(&encoded, field->name, name_size,
                                  max_bytes, error);
    if (status == ORM_STATUS_OK)
      status = orm_tidesdb_append(&encoded, field->value.bytes, value_size,
                                  max_bytes, error);
  }
  if (status != ORM_STATUS_OK) {
    tstr_free(encoded);
    return status;
  }
  *out_bytes = encoded;
  return ORM_STATUS_OK;
}

typedef struct orm_tidesdb_field_reader {
  const unsigned char *data;
  size_t size, offset, count;
} orm_tidesdb_field_reader;

static orm_status_t orm_tidesdb_read_header(const unsigned char *data, size_t size,
    size_t max_bytes, size_t capacity, orm_tidesdb_field_reader *out, orm_error_t *error) {
  if ((!data && size) || size > max_bytes)
    return orm_tidesdb_fail(error, size > max_bytes ? ORM_STATUS_LIMIT_EXCEEDED : ORM_STATUS_INVALID_ARGUMENT,
        "invalid TidesDB row decoding request");
  if (size < ORM_TIDESDB_ROW_HEADER_SIZE || memcmp(data, orm_tidesdb_magic, ORM_TIDESDB_MAGIC_SIZE))
    return orm_tidesdb_fail(error, ORM_STATUS_DATASTORE_ERROR, "TidesDB row has an unknown format or version");
  const size_t count = orm_tidesdb_read_u32(data + ORM_TIDESDB_MAGIC_SIZE);
  if (count > capacity)
    return orm_tidesdb_fail(error, ORM_STATUS_LIMIT_EXCEEDED, "stored TidesDB row field count exceeds its limit");
  *out = (orm_tidesdb_field_reader){data, size, ORM_TIDESDB_ROW_HEADER_SIZE, count};
  return ORM_STATUS_OK;
}

static orm_status_t orm_tidesdb_read_field(orm_tidesdb_field_reader *reader,
    orm_tidesdb_field_view *out, vstr *raw, orm_error_t *error) {
  const unsigned char *data = reader->data;
  size_t offset = reader->offset;
  if (ORM_TIDESDB_FIELD_HEADER_SIZE > reader->size - offset)
    return orm_tidesdb_fail(error, ORM_STATUS_DATASTORE_ERROR, "truncated TidesDB row field header");
  const size_t name_size = orm_tidesdb_read_u16(data + offset);
  const orm_value_kind_t kind = (orm_value_kind_t)data[offset + 2u];
  const unsigned flags = data[offset + 3u];
  const size_t value_size = orm_tidesdb_read_u32(data + offset + 4u);
  offset += ORM_TIDESDB_FIELD_HEADER_SIZE;
  if (!name_size || name_size > reader->size - offset || memchr(data + offset, 0, name_size))
    return orm_tidesdb_fail(error, ORM_STATUS_DATASTORE_ERROR, "invalid TidesDB row field name");
  const vstr name = {(const char *)data + offset, name_size};
  offset += name_size;
  if (value_size > reader->size - offset || (flags & ~ORM_TIDESDB_NULL_FLAG))
    return orm_tidesdb_fail(error, ORM_STATUS_DATASTORE_ERROR, "truncated or invalid TidesDB row value");
  orm_value_t value;
  if (!orm_tidesdb_read_value(kind, (flags & ORM_TIDESDB_NULL_FLAG) != 0, data + offset, value_size, &value))
    return orm_tidesdb_fail(error, ORM_STATUS_DATASTORE_ERROR, "invalid TidesDB row value");
  *out = (orm_tidesdb_field_view){name, value};
  *raw = (vstr){(const char *)data + offset, value_size};
  reader->offset = offset + value_size;
  return ORM_STATUS_OK;
}

orm_status_t orm_tidesdb_row_decode_view(const unsigned char *data, size_t size,
    size_t max_bytes, orm_tidesdb_field_view *fields, size_t capacity,
    size_t *count, orm_error_t *error) {
  if (!fields || !capacity || capacity > SIZE_MAX / sizeof(*fields) || !count)
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT, "invalid TidesDB field view output");
  orm_tidesdb_field_reader reader = {0};
  orm_status_t status = orm_tidesdb_read_header(data, size, max_bytes, capacity, &reader, error);
  for (size_t i = 0; status == ORM_STATUS_OK && i < reader.count; ++i) {
    vstr raw;
    status = orm_tidesdb_read_field(&reader, &fields[i], &raw, error);
    for (size_t j = 0; status == ORM_STATUS_OK && j < i; ++j)
      if (fields[j].name.len == fields[i].name.len &&
          !memcmp(fields[j].name.data, fields[i].name.data, fields[i].name.len))
        status = orm_tidesdb_fail(error, ORM_STATUS_DATASTORE_ERROR, "TidesDB row contains a duplicate field");
  }
  if (status == ORM_STATUS_OK && reader.offset != size)
    status = orm_tidesdb_fail(error, ORM_STATUS_DATASTORE_ERROR, "TidesDB row has trailing bytes");
  if (status != ORM_STATUS_OK) memset(fields, 0, capacity * sizeof(*fields));
  else *count = reader.count;
  return status;
}

orm_status_t orm_tidesdb_row_decode(const unsigned char *data, size_t size,
    size_t max_bytes, size_t max_fields, orm_tidesdb_row *out_row, orm_error_t *error) {
  if (!out_row || out_row->fields.initialized)
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT, "invalid TidesDB row decoding request");
  orm_tidesdb_field_reader reader = {0};
  orm_status_t status = orm_tidesdb_read_header(data, size, max_bytes, max_fields, &reader, error);
  orm_tidesdb_row decoded = {0};
  if (status == ORM_STATUS_OK) status = orm_tidesdb_row_init(&decoded, max_fields, error);
  for (size_t i = 0; status == ORM_STATUS_OK && i < reader.count; ++i) {
    orm_tidesdb_field_view field; vstr raw;
    status = orm_tidesdb_read_field(&reader, &field, &raw, error);
    if (status != ORM_STATUS_OK) break;
    if (orm_tidesdb_row_find(&decoded, field.name)) {
      status = orm_tidesdb_fail(error, ORM_STATUS_DATASTORE_ERROR, "TidesDB row contains a duplicate field");
      break;
    }
    orm_tidesdb_cell cell = {.kind = field.value.kind, .is_null = field.value.kind == ORM_VALUE_NULL,
        .bytes = tstr_new_len(raw.data, raw.len)};
    if (!cell.bytes)
      status = orm_tidesdb_fail(error, ORM_STATUS_OUT_OF_MEMORY, "copy TidesDB row value");
    else status = orm_tidesdb_row_set(&decoded, field.name, &cell, error);
    orm_tidesdb_cell_destroy(&cell);
  }
  if (status == ORM_STATUS_OK && reader.offset != size)
    status = orm_tidesdb_fail(error, ORM_STATUS_DATASTORE_ERROR, "TidesDB row has trailing bytes");
  if (status != ORM_STATUS_OK) orm_tidesdb_row_destroy(&decoded);
  else *out_row = decoded;
  return status;
}

static orm_status_t orm_tidesdb_unqualified(const orm_query_plan *plan,
                                            tstr column, vstr *out,
                                            orm_error_t *error) {
  const char *separator = strchr(column, '.');
  const size_t column_size = tstr_len(column);
  if (separator == NULL) {
    out->data = column;
    out->len = column_size;
    return ORM_STATUS_OK;
  }
  if (strchr(separator + 1, '.') != NULL ||
      (size_t)(separator - column) != tstr_len(plan->table) ||
      memcmp(column, plan->table, tstr_len(plan->table)) != 0 ||
      separator[1] == '\0')
    return orm_tidesdb_fail(error, ORM_STATUS_UNSUPPORTED,
                            "TidesDB column must belong to the selected table");
  out->data = separator + 1;
  out->len = column_size - (size_t)(separator + 1 - column);
  return ORM_STATUS_OK;
}

static int orm_tidesdb_bytes_compare(tstr left, tstr right) {
  const size_t left_size = tstr_len(left);
  const size_t right_size = tstr_len(right);
  const size_t common = left_size < right_size ? left_size : right_size;
  const int compared = common != 0u ? memcmp(left, right, common) : 0;
  if (compared != 0)
    return compared < 0 ? -1 : 1;
  return left_size < right_size ? -1 : (left_size > right_size ? 1 : 0);
}

static orm_status_t orm_tidesdb_compare_cells(const orm_tidesdb_cell *left,
                                              const orm_tidesdb_cell *right,
                                              int *out,
                                              orm_error_t *error) {
  const int left_numeric = left->kind == ORM_VALUE_INT64 ||
                           left->kind == ORM_VALUE_UINT64 ||
                           left->kind == ORM_VALUE_DOUBLE ||
                           left->kind == ORM_VALUE_BOOLEAN;
  const int right_numeric = right->kind == ORM_VALUE_INT64 ||
                            right->kind == ORM_VALUE_UINT64 ||
                            right->kind == ORM_VALUE_DOUBLE ||
                            right->kind == ORM_VALUE_BOOLEAN;
  if ((left->kind == ORM_VALUE_TEXT && right->kind == ORM_VALUE_TEXT) ||
      (left->kind == ORM_VALUE_BLOB && right->kind == ORM_VALUE_BLOB)) {
    *out = orm_tidesdb_bytes_compare(left->bytes, right->bytes);
    return ORM_STATUS_OK;
  }
  if (!left_numeric || !right_numeric)
    return orm_tidesdb_fail(error, ORM_STATUS_TYPE_ERROR,
                            "TidesDB comparison requires compatible value types");
  if (left->kind == ORM_VALUE_DOUBLE || right->kind == ORM_VALUE_DOUBLE) {
    double left_value;
    double right_value;
    if (left->kind == ORM_VALUE_DOUBLE) {
      (void)orm_tidesdb_parse_double((const unsigned char *)left->bytes,
                                     tstr_len(left->bytes), &left_value);
    } else if (left->kind == ORM_VALUE_INT64) {
      int64_t value;
      (void)orm_tidesdb_parse_sint64((const unsigned char *)left->bytes,
                                     tstr_len(left->bytes), &value);
      left_value = (double)value;
    } else {
      uint64_t value;
      (void)orm_tidesdb_parse_uint64((const unsigned char *)left->bytes,
                                     tstr_len(left->bytes), UINT64_MAX, &value);
      left_value = (double)value;
    }
    if (right->kind == ORM_VALUE_DOUBLE) {
      (void)orm_tidesdb_parse_double((const unsigned char *)right->bytes,
                                     tstr_len(right->bytes), &right_value);
    } else if (right->kind == ORM_VALUE_INT64) {
      int64_t value;
      (void)orm_tidesdb_parse_sint64((const unsigned char *)right->bytes,
                                     tstr_len(right->bytes), &value);
      right_value = (double)value;
    } else {
      uint64_t value;
      (void)orm_tidesdb_parse_uint64((const unsigned char *)right->bytes,
                                     tstr_len(right->bytes), UINT64_MAX, &value);
      right_value = (double)value;
    }
    *out = left_value < right_value ? -1 : (left_value > right_value ? 1 : 0);
    return ORM_STATUS_OK;
  }
  if (left->kind == ORM_VALUE_INT64 && right->kind == ORM_VALUE_INT64) {
    int64_t left_value;
    int64_t right_value;
    (void)orm_tidesdb_parse_sint64((const unsigned char *)left->bytes,
                                   tstr_len(left->bytes), &left_value);
    (void)orm_tidesdb_parse_sint64((const unsigned char *)right->bytes,
                                   tstr_len(right->bytes), &right_value);
    *out = left_value < right_value ? -1 : (left_value > right_value ? 1 : 0);
    return ORM_STATUS_OK;
  }
  if (left->kind != ORM_VALUE_INT64 && right->kind != ORM_VALUE_INT64) {
    uint64_t left_value;
    uint64_t right_value;
    (void)orm_tidesdb_parse_uint64((const unsigned char *)left->bytes,
                                   tstr_len(left->bytes), UINT64_MAX,
                                   &left_value);
    (void)orm_tidesdb_parse_uint64((const unsigned char *)right->bytes,
                                   tstr_len(right->bytes), UINT64_MAX,
                                   &right_value);
    *out = left_value < right_value ? -1 : (left_value > right_value ? 1 : 0);
    return ORM_STATUS_OK;
  }
  {
    const orm_tidesdb_cell *signed_cell =
        left->kind == ORM_VALUE_INT64 ? left : right;
    const orm_tidesdb_cell *unsigned_cell =
        left->kind == ORM_VALUE_INT64 ? right : left;
    int64_t signed_value;
    uint64_t unsigned_value;
    int comparison;
    (void)orm_tidesdb_parse_sint64((const unsigned char *)signed_cell->bytes,
                                   tstr_len(signed_cell->bytes), &signed_value);
    (void)orm_tidesdb_parse_uint64(
        (const unsigned char *)unsigned_cell->bytes,
        tstr_len(unsigned_cell->bytes), UINT64_MAX, &unsigned_value);
    if (signed_value < 0)
      comparison = -1;
    else {
      const uint64_t converted = (uint64_t)signed_value;
      comparison = converted < unsigned_value
                       ? -1
                       : (converted > unsigned_value ? 1 : 0);
    }
    *out = left->kind == ORM_VALUE_INT64 ? comparison : -comparison;
    return ORM_STATUS_OK;
  }
}

static int orm_tidesdb_like(vstr value, vstr pattern) {
  size_t value_index = 0u;
  size_t pattern_index = 0u;
  size_t wildcard = SIZE_MAX;
  size_t wildcard_value = 0u;
  while (value_index < value.len) {
    if (pattern_index < pattern.len &&
        (pattern.data[pattern_index] == '_' ||
         pattern.data[pattern_index] == value.data[value_index])) {
      ++value_index;
      ++pattern_index;
    } else if (pattern_index < pattern.len &&
               pattern.data[pattern_index] == '%') {
      wildcard = pattern_index++;
      wildcard_value = value_index;
    } else if (wildcard != SIZE_MAX) {
      pattern_index = wildcard + 1u;
      value_index = ++wildcard_value;
    } else {
      return 0;
    }
  }
  while (pattern_index < pattern.len && pattern.data[pattern_index] == '%')
    ++pattern_index;
  return pattern_index == pattern.len;
}

orm_status_t orm_tidesdb_row_matches(const orm_tidesdb_row *row,
                                     const orm_query_plan *plan,
                                     bool *out_matches,
                                     orm_error_t *error) {
  size_t index;
  if (row == NULL || plan == NULL || out_matches == NULL)
    return orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                            "invalid TidesDB predicate evaluation");
  *out_matches = false;
  for (index = 0u; index < vec_size(&plan->predicates); ++index) {
    const orm_predicate *predicate =
        (const orm_predicate *)vec_at_const(&plan->predicates, index);
    const orm_tidesdb_cell *stored;
    orm_tidesdb_cell expected = {0};
    vstr column;
    orm_status_t status = orm_tidesdb_unqualified(plan, predicate->column,
                                                  &column, error);
    int comparison = 0;
    int matched;
    if (status != ORM_STATUS_OK)
      return status;
    stored = orm_tidesdb_row_find(row, column);
    if (predicate->value.kind == ORM_VALUE_NULL) {
      const int is_null = stored == NULL || stored->is_null;
      matched = predicate->comparison == ORM_COMPARE_EQUAL ? is_null
                : predicate->comparison == ORM_COMPARE_NOT_EQUAL ? !is_null
                                                                 : 0;
      if (!matched)
        return ORM_STATUS_OK;
      continue;
    }
    if (stored == NULL || stored->is_null)
      return ORM_STATUS_OK;
    status = orm_tidesdb_cell_from_value(&expected, &predicate->value, error);
    if (status != ORM_STATUS_OK)
      return status;
    if (predicate->comparison == ORM_COMPARE_LIKE ||
        predicate->comparison == ORM_COMPARE_NOT_LIKE) {
      if (stored->kind != ORM_VALUE_TEXT || expected.kind != ORM_VALUE_TEXT) {
        orm_tidesdb_cell_destroy(&expected);
        return orm_tidesdb_fail(error, ORM_STATUS_TYPE_ERROR,
                                "TidesDB LIKE requires text values");
      }
      matched = orm_tidesdb_like(tstr_to_v(stored->bytes),
                                 tstr_to_v(expected.bytes));
      if (predicate->comparison == ORM_COMPARE_NOT_LIKE)
        matched = !matched;
    } else {
      status = orm_tidesdb_compare_cells(stored, &expected, &comparison,
                                         error);
      if (status != ORM_STATUS_OK) {
        orm_tidesdb_cell_destroy(&expected);
        return status;
      }
      switch (predicate->comparison) {
        case ORM_COMPARE_EQUAL: matched = comparison == 0; break;
        case ORM_COMPARE_NOT_EQUAL: matched = comparison != 0; break;
        case ORM_COMPARE_LESS: matched = comparison < 0; break;
        case ORM_COMPARE_LESS_EQUAL: matched = comparison <= 0; break;
        case ORM_COMPARE_GREATER: matched = comparison > 0; break;
        case ORM_COMPARE_GREATER_EQUAL: matched = comparison >= 0; break;
        default:
          orm_tidesdb_cell_destroy(&expected);
          return orm_tidesdb_fail(error, ORM_STATUS_INVALID_ARGUMENT,
                                  "unknown TidesDB comparison operator");
      }
    }
    orm_tidesdb_cell_destroy(&expected);
    if (!matched)
      return ORM_STATUS_OK;
  }
  *out_matches = true;
  return ORM_STATUS_OK;
}

orm_status_t orm_tidesdb_row_project(const orm_tidesdb_row *row,
                                     const orm_query_plan *plan,
                                     const orm_limits *limits,
                                     orm_tidesdb_row *out_row,
                                     orm_error_t *error) {
  orm_tidesdb_row projected = {0};
  size_t index;
  orm_status_t status;
  if (row == NULL || plan == NULL || limits == NULL || out_row == NULL ||
      out_row->fields.initialized || plan->select_all ||
      vec_size(&plan->columns) == 0u)
    return orm_tidesdb_fail(error,
                            plan != NULL && plan->select_all
                                ? ORM_STATUS_UNSUPPORTED
                                : ORM_STATUS_INVALID_ARGUMENT,
                            "TidesDB SELECT requires an explicit projection");
  status = orm_tidesdb_row_init(&projected, limits->max_columns, error);
  if (status != ORM_STATUS_OK)
    return status;
  for (index = 0u; index < vec_size(&plan->columns); ++index) {
    const tstr selected =
        *(const tstr *)vec_at_const(&plan->columns, index);
    const orm_tidesdb_cell *cell;
    orm_tidesdb_cell null_cell = {ORM_VALUE_NULL, true, NULL};
    vstr output;
    status = orm_tidesdb_unqualified(plan, selected, &output, error);
    if (status != ORM_STATUS_OK)
      goto fail;
    cell = orm_tidesdb_row_find(row, output);
    if (cell == NULL) {
      null_cell.bytes = tstr_new();
      if (null_cell.bytes == NULL) {
        status = orm_tidesdb_fail(error, ORM_STATUS_OUT_OF_MEMORY,
                                  "allocate TidesDB null projection");
        goto fail;
      }
      cell = &null_cell;
    }
    status = orm_tidesdb_row_set(&projected, output, cell, error);
    orm_tidesdb_cell_destroy(&null_cell);
    if (status != ORM_STATUS_OK)
      goto fail;
  }
  *out_row = projected;
  return ORM_STATUS_OK;

fail:
  orm_tidesdb_row_destroy(&projected);
  return status;
}
