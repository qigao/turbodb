#include "row.h"

#include <float.h>
#include <limits.h>
#include <string.h>

_Static_assert(sizeof(float) == 4u, "MySQL FLOAT requires 32-bit float");
_Static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
               "MySQL FLOAT requires IEEE-754 binary32");
_Static_assert(sizeof(double) == 8u, "MySQL DOUBLE requires 64-bit double");
_Static_assert(DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "MySQL DOUBLE requires IEEE-754 binary64");

static mysql_wire_status_t mysql_row_read_required_bytes(
    const uint8_t *payload, size_t payload_size, size_t *offset,
    mysql_wire_bytes_t *out) {
  mysql_wire_status_t status =
      mysql_wire_read_lenenc_bytes(payload, payload_size, offset, out);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  return out->is_null ? MYSQL_WIRE_STATUS_INVALID : MYSQL_WIRE_STATUS_OK;
}

mysql_wire_status_t mysql_wire_decode_column_definition41(
    const uint8_t *payload, size_t payload_size,
    mysql_column_definition_t *out) {
  size_t offset = 0u;
  uint64_t fixed_length = 0u;
  bool is_null = false;
  mysql_wire_status_t status;

  if (payload == NULL || out == NULL)
    return MYSQL_WIRE_STATUS_INVALID;
  memset(out, 0, sizeof(*out));

  status = mysql_row_read_required_bytes(
      payload, payload_size, &offset, &out->catalog);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_row_read_required_bytes(
      payload, payload_size, &offset, &out->schema);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_row_read_required_bytes(
      payload, payload_size, &offset, &out->table);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_row_read_required_bytes(
      payload, payload_size, &offset, &out->org_table);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_row_read_required_bytes(
      payload, payload_size, &offset, &out->name);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_row_read_required_bytes(
      payload, payload_size, &offset, &out->org_name);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  status = mysql_wire_read_lenenc_uint(
      payload, payload_size, &offset, &fixed_length, &is_null);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  if (is_null || fixed_length != UINT64_C(0x0c))
    return MYSQL_WIRE_STATUS_INVALID;

  status = mysql_wire_read_u16_le(
      payload, payload_size, &offset, &out->character_set);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_wire_read_u32_le(
      payload, payload_size, &offset, &out->column_length);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  if (offset >= payload_size)
    return MYSQL_WIRE_STATUS_NEED_MORE;
  out->type = payload[offset++];

  status = mysql_wire_read_u16_le(
      payload, payload_size, &offset, &out->flags);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  if (offset >= payload_size)
    return MYSQL_WIRE_STATUS_NEED_MORE;
  out->decimals = payload[offset++];

  if (offset > payload_size || payload_size - offset < 2u)
    return MYSQL_WIRE_STATUS_NEED_MORE;
  if (payload[offset] != 0u || payload[offset + 1u] != 0u)
    return MYSQL_WIRE_STATUS_INVALID;
  offset += 2u;

  return offset == payload_size
             ? MYSQL_WIRE_STATUS_OK
             : MYSQL_WIRE_STATUS_INVALID;
}

static int64_t mysql_row_sign_extend(uint64_t raw, unsigned bits) {
  uint64_t sign;
  uint64_t mask;

  if (bits == 64u) {
    if (raw <= (uint64_t)INT64_MAX)
      return (int64_t)raw;
    if (raw == (UINT64_C(1) << 63u))
      return INT64_MIN;
    return -(int64_t)((~raw) + UINT64_C(1));
  }

  sign = UINT64_C(1) << (bits - 1u);
  mask = (UINT64_C(1) << bits) - UINT64_C(1);
  raw &= mask;
  if ((raw & sign) == 0u)
    return (int64_t)raw;
  return -(int64_t)(((~raw) & mask) + UINT64_C(1));
}

static mysql_wire_status_t mysql_row_decode_integer(
    const uint8_t *payload, size_t payload_size, size_t *offset,
    const mysql_column_definition_t *column, unsigned width,
    mysql_binary_value_t *out) {
  uint64_t raw = 0u;
  mysql_wire_status_t status;

  switch (width) {
    case 1u:
      if (*offset >= payload_size)
        return MYSQL_WIRE_STATUS_NEED_MORE;
      raw = payload[(*offset)++];
      break;
    case 2u: {
      uint16_t value;
      status = mysql_wire_read_u16_le(
          payload, payload_size, offset, &value);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
      raw = value;
      break;
    }
    case 4u: {
      uint32_t value;
      status = mysql_wire_read_u32_le(
          payload, payload_size, offset, &value);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
      raw = value;
      break;
    }
    case 8u:
      status = mysql_wire_read_u64_le(
          payload, payload_size, offset, &raw);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
      break;
    default:
      return MYSQL_WIRE_STATUS_INVALID;
  }

  if ((column->flags & MYSQL_COLUMN_FLAG_UNSIGNED) != 0u) {
    out->kind = MYSQL_BINARY_VALUE_UINT64;
    out->data.uint64_value = raw;
  } else {
    out->kind = MYSQL_BINARY_VALUE_SINT64;
    out->data.sint64_value =
        mysql_row_sign_extend(raw, width * 8u);
  }
  return MYSQL_WIRE_STATUS_OK;
}

static mysql_wire_status_t mysql_row_decode_float(
    const uint8_t *payload, size_t payload_size, size_t *offset,
    mysql_binary_value_t *out) {
  uint32_t raw;
  float value;
  mysql_wire_status_t status =
      mysql_wire_read_u32_le(payload, payload_size, offset, &raw);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  memcpy(&value, &raw, sizeof(value));
  out->kind = MYSQL_BINARY_VALUE_DOUBLE;
  out->data.double_value = (double)value;
  return MYSQL_WIRE_STATUS_OK;
}

static mysql_wire_status_t mysql_row_decode_double(
    const uint8_t *payload, size_t payload_size, size_t *offset,
    mysql_binary_value_t *out) {
  uint64_t raw;
  double value;
  mysql_wire_status_t status =
      mysql_wire_read_u64_le(payload, payload_size, offset, &raw);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  memcpy(&value, &raw, sizeof(value));
  out->kind = MYSQL_BINARY_VALUE_DOUBLE;
  out->data.double_value = value;
  return MYSQL_WIRE_STATUS_OK;
}

static mysql_wire_status_t mysql_row_decode_bytes(
    const uint8_t *payload, size_t payload_size, size_t *offset,
    mysql_binary_value_t *out) {
  mysql_wire_bytes_t bytes;
  mysql_wire_status_t status =
      mysql_wire_read_lenenc_bytes(
          payload, payload_size, offset, &bytes);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  if (bytes.is_null)
    return MYSQL_WIRE_STATUS_INVALID;
  out->kind = MYSQL_BINARY_VALUE_BYTES;
  out->data.bytes = bytes;
  return MYSQL_WIRE_STATUS_OK;
}

static bool mysql_row_valid_datetime(
    const mysql_binary_datetime_t *value, bool date_only) {
  if (value->month > 12u || value->day > 31u ||
      value->microsecond > UINT32_C(999999))
    return false;
  if (date_only)
    return value->hour == 0u && value->minute == 0u &&
           value->second == 0u && value->microsecond == 0u;
  return value->hour <= 23u &&
         value->minute <= 59u &&
         value->second <= 59u;
}

static mysql_wire_status_t mysql_row_decode_datetime(
    const uint8_t *payload, size_t payload_size, size_t *offset,
    bool date_only, mysql_binary_value_t *out) {
  mysql_binary_datetime_t value;
  uint8_t length;
  mysql_wire_status_t status;

  memset(&value, 0, sizeof(value));
  if (*offset >= payload_size)
    return MYSQL_WIRE_STATUS_NEED_MORE;

  length = payload[(*offset)++];
  value.wire_length = length;
  if (length == 0u) {
    out->kind = MYSQL_BINARY_VALUE_DATETIME;
    out->data.datetime_value = value;
    return MYSQL_WIRE_STATUS_OK;
  }
  if (date_only) {
    if (length != 4u)
      return MYSQL_WIRE_STATUS_INVALID;
  } else if (length != 4u && length != 7u && length != 11u) {
    return MYSQL_WIRE_STATUS_INVALID;
  }

  status = mysql_wire_read_u16_le(
      payload, payload_size, offset, &value.year);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  if (*offset > payload_size || payload_size - *offset < 2u)
    return MYSQL_WIRE_STATUS_NEED_MORE;
  value.month = payload[(*offset)++];
  value.day = payload[(*offset)++];

  if (length >= 7u) {
    if (*offset > payload_size || payload_size - *offset < 3u)
      return MYSQL_WIRE_STATUS_NEED_MORE;
    value.hour = payload[(*offset)++];
    value.minute = payload[(*offset)++];
    value.second = payload[(*offset)++];
  }
  if (length == 11u) {
    status = mysql_wire_read_u32_le(
        payload, payload_size, offset, &value.microsecond);
    if (status != MYSQL_WIRE_STATUS_OK)
      return status;
  }

  if (!mysql_row_valid_datetime(&value, date_only))
    return MYSQL_WIRE_STATUS_INVALID;

  out->kind = MYSQL_BINARY_VALUE_DATETIME;
  out->data.datetime_value = value;
  return MYSQL_WIRE_STATUS_OK;
}

static mysql_wire_status_t mysql_row_decode_time(
    const uint8_t *payload, size_t payload_size, size_t *offset,
    mysql_binary_value_t *out) {
  mysql_binary_time_t value;
  uint8_t length;
  mysql_wire_status_t status;

  memset(&value, 0, sizeof(value));
  if (*offset >= payload_size)
    return MYSQL_WIRE_STATUS_NEED_MORE;

  length = payload[(*offset)++];
  value.wire_length = length;
  if (length == 0u) {
    out->kind = MYSQL_BINARY_VALUE_TIME;
    out->data.time_value = value;
    return MYSQL_WIRE_STATUS_OK;
  }
  if (length != 8u && length != 12u)
    return MYSQL_WIRE_STATUS_INVALID;
  if (*offset >= payload_size)
    return MYSQL_WIRE_STATUS_NEED_MORE;

  if (payload[*offset] > 1u)
    return MYSQL_WIRE_STATUS_INVALID;
  value.negative = payload[(*offset)++] != 0u;

  status = mysql_wire_read_u32_le(
      payload, payload_size, offset, &value.days);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  if (*offset > payload_size || payload_size - *offset < 3u)
    return MYSQL_WIRE_STATUS_NEED_MORE;
  value.hour = payload[(*offset)++];
  value.minute = payload[(*offset)++];
  value.second = payload[(*offset)++];

  if (length == 12u) {
    status = mysql_wire_read_u32_le(
        payload, payload_size, offset, &value.microsecond);
    if (status != MYSQL_WIRE_STATUS_OK)
      return status;
  }

  if (value.hour > 23u || value.minute > 59u ||
      value.second > 59u ||
      value.microsecond > UINT32_C(999999))
    return MYSQL_WIRE_STATUS_INVALID;

  out->kind = MYSQL_BINARY_VALUE_TIME;
  out->data.time_value = value;
  return MYSQL_WIRE_STATUS_OK;
}

static mysql_wire_status_t mysql_row_decode_value(
    const uint8_t *payload, size_t payload_size, size_t *offset,
    const mysql_column_definition_t *column,
    mysql_binary_value_t *out) {
  if (payload == NULL || offset == NULL ||
      column == NULL || out == NULL)
    return MYSQL_WIRE_STATUS_INVALID;

  memset(out, 0, sizeof(*out));
  out->mysql_type = column->type;

  switch (column->type) {
    case MYSQL_FIELD_TYPE_NULL:
      out->kind = MYSQL_BINARY_VALUE_NULL;
      return MYSQL_WIRE_STATUS_OK;
    case MYSQL_FIELD_TYPE_TINY:
      return mysql_row_decode_integer(
          payload, payload_size, offset, column, 1u, out);
    case MYSQL_FIELD_TYPE_SHORT:
    case MYSQL_FIELD_TYPE_YEAR:
      return mysql_row_decode_integer(
          payload, payload_size, offset, column, 2u, out);
    case MYSQL_FIELD_TYPE_LONG:
    case MYSQL_FIELD_TYPE_INT24:
      return mysql_row_decode_integer(
          payload, payload_size, offset, column, 4u, out);
    case MYSQL_FIELD_TYPE_LONGLONG:
      return mysql_row_decode_integer(
          payload, payload_size, offset, column, 8u, out);
    case MYSQL_FIELD_TYPE_FLOAT:
      return mysql_row_decode_float(
          payload, payload_size, offset, out);
    case MYSQL_FIELD_TYPE_DOUBLE:
      return mysql_row_decode_double(
          payload, payload_size, offset, out);
    case MYSQL_FIELD_TYPE_DATE:
      return mysql_row_decode_datetime(
          payload, payload_size, offset, true, out);
    case MYSQL_FIELD_TYPE_DATETIME:
    case MYSQL_FIELD_TYPE_TIMESTAMP:
      return mysql_row_decode_datetime(
          payload, payload_size, offset, false, out);
    case MYSQL_FIELD_TYPE_TIME:
      return mysql_row_decode_time(
          payload, payload_size, offset, out);
    case MYSQL_FIELD_TYPE_DECIMAL:
    case MYSQL_FIELD_TYPE_VARCHAR:
    case MYSQL_FIELD_TYPE_BIT:
    case MYSQL_FIELD_TYPE_JSON:
    case MYSQL_FIELD_TYPE_NEWDECIMAL:
    case MYSQL_FIELD_TYPE_ENUM:
    case MYSQL_FIELD_TYPE_SET:
    case MYSQL_FIELD_TYPE_TINY_BLOB:
    case MYSQL_FIELD_TYPE_MEDIUM_BLOB:
    case MYSQL_FIELD_TYPE_LONG_BLOB:
    case MYSQL_FIELD_TYPE_BLOB:
    case MYSQL_FIELD_TYPE_VAR_STRING:
    case MYSQL_FIELD_TYPE_STRING:
    case MYSQL_FIELD_TYPE_GEOMETRY:
      return mysql_row_decode_bytes(
          payload, payload_size, offset, out);
    default:
      return MYSQL_WIRE_STATUS_INVALID;
  }
}

mysql_wire_status_t mysql_wire_decode_binary_row(
    const uint8_t *payload, size_t payload_size,
    const mysql_column_definition_t *columns, size_t column_count,
    mysql_binary_value_t *values, size_t value_capacity) {
  const uint8_t *null_bitmap;
  size_t null_bitmap_size;
  size_t offset;
  size_t i;

  if (payload == NULL ||
      (columns == NULL && column_count != 0u) ||
      (values == NULL && column_count != 0u))
    return MYSQL_WIRE_STATUS_INVALID;
  if (column_count > value_capacity)
    return MYSQL_WIRE_STATUS_LIMIT;
  if (column_count > SIZE_MAX - 9u)
    return MYSQL_WIRE_STATUS_LIMIT;
  if (payload_size < 1u)
    return MYSQL_WIRE_STATUS_NEED_MORE;
  if (payload[0] != UINT8_C(0x00))
    return MYSQL_WIRE_STATUS_INVALID;

  null_bitmap_size = (column_count + 9u) / 8u;
  if (payload_size - 1u < null_bitmap_size)
    return MYSQL_WIRE_STATUS_NEED_MORE;

  null_bitmap = payload + 1u;
  offset = 1u + null_bitmap_size;

  for (i = 0u; i < column_count; ++i) {
    const size_t bit = i + 2u;
    const bool is_null =
        (null_bitmap[bit / 8u] &
         (uint8_t)(UINT8_C(1) << (bit % 8u))) != 0u;
    mysql_wire_status_t status;

    memset(&values[i], 0, sizeof(values[i]));
    values[i].mysql_type = columns[i].type;
    if (is_null) {
      values[i].kind = MYSQL_BINARY_VALUE_NULL;
      continue;
    }

    status = mysql_row_decode_value(
        payload, payload_size, &offset, &columns[i], &values[i]);
    if (status != MYSQL_WIRE_STATUS_OK)
      return status;
  }

  return offset == payload_size
             ? MYSQL_WIRE_STATUS_OK
             : MYSQL_WIRE_STATUS_INVALID;
}
