#include "statement.h"

#include <float.h>
#include <string.h>

_Static_assert(sizeof(double) == 8u, "MySQL DOUBLE requires 64-bit double");
_Static_assert(DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
               "MySQL DOUBLE requires IEEE-754 binary64");

static mysql_wire_status_t mysql_stmt_emit(
    uint8_t *out, size_t capacity, size_t *offset,
    const void *data, size_t size) {
  if (out == NULL || offset == NULL || (data == NULL && size != 0u))
    return MYSQL_WIRE_STATUS_INVALID;
  if (*offset > capacity || capacity - *offset < size)
    return MYSQL_WIRE_STATUS_LIMIT;
  if (size != 0u)
    memcpy(out + *offset, data, size);
  *offset += size;
  return MYSQL_WIRE_STATUS_OK;
}

static mysql_wire_status_t mysql_stmt_emit_byte(
    uint8_t *out, size_t capacity, size_t *offset, uint8_t value) {
  return mysql_stmt_emit(out, capacity, offset, &value, 1u);
}

static mysql_wire_status_t mysql_stmt_emit_lenenc_bytes(
    uint8_t *out, size_t capacity, size_t *offset,
    const uint8_t *data, size_t size) {
  mysql_wire_status_t status;
  status = mysql_wire_write_lenenc_uint(
      out, capacity, offset, (uint64_t)size);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  return mysql_stmt_emit(out, capacity, offset, data, size);
}

static mysql_wire_status_t mysql_stmt_value_type(
    const mysql_stmt_value_t *value, uint8_t *type, uint8_t *flags) {
  if (value == NULL || type == NULL || flags == NULL)
    return MYSQL_WIRE_STATUS_INVALID;

  *flags = 0u;
  switch (value->kind) {
    case MYSQL_STMT_VALUE_NULL:
      *type = MYSQL_TYPE_NULL;
      return MYSQL_WIRE_STATUS_OK;
    case MYSQL_STMT_VALUE_SINT64:
      *type = MYSQL_TYPE_LONGLONG;
      return MYSQL_WIRE_STATUS_OK;
    case MYSQL_STMT_VALUE_UINT64:
      *type = MYSQL_TYPE_LONGLONG;
      *flags = MYSQL_TYPE_UNSIGNED_FLAG;
      return MYSQL_WIRE_STATUS_OK;
    case MYSQL_STMT_VALUE_DOUBLE:
      *type = MYSQL_TYPE_DOUBLE;
      return MYSQL_WIRE_STATUS_OK;
    case MYSQL_STMT_VALUE_BOOL:
      if (value->data.bool_value > 1u)
        return MYSQL_WIRE_STATUS_INVALID;
      *type = MYSQL_TYPE_TINY;
      return MYSQL_WIRE_STATUS_OK;
    case MYSQL_STMT_VALUE_TEXT:
      if (value->data.bytes.data == NULL && value->data.bytes.size != 0u)
        return MYSQL_WIRE_STATUS_INVALID;
      *type = MYSQL_TYPE_VAR_STRING;
      return MYSQL_WIRE_STATUS_OK;
    case MYSQL_STMT_VALUE_BLOB:
      if (value->data.bytes.data == NULL && value->data.bytes.size != 0u)
        return MYSQL_WIRE_STATUS_INVALID;
      *type = MYSQL_TYPE_BLOB;
      return MYSQL_WIRE_STATUS_OK;
    default:
      return MYSQL_WIRE_STATUS_INVALID;
  }
}

static mysql_wire_status_t mysql_stmt_emit_value(
    uint8_t *out, size_t capacity, size_t *offset,
    const mysql_stmt_value_t *value) {
  uint64_t bits;

  if (value == NULL)
    return MYSQL_WIRE_STATUS_INVALID;

  switch (value->kind) {
    case MYSQL_STMT_VALUE_NULL:
      return MYSQL_WIRE_STATUS_OK;
    case MYSQL_STMT_VALUE_SINT64:
      return mysql_wire_write_u64_le(
          out, capacity, offset, (uint64_t)value->data.sint64_value);
    case MYSQL_STMT_VALUE_UINT64:
      return mysql_wire_write_u64_le(
          out, capacity, offset, value->data.uint64_value);
    case MYSQL_STMT_VALUE_DOUBLE:
      memcpy(&bits, &value->data.double_value, sizeof(bits));
      return mysql_wire_write_u64_le(out, capacity, offset, bits);
    case MYSQL_STMT_VALUE_BOOL:
      return mysql_stmt_emit_byte(
          out, capacity, offset, value->data.bool_value);
    case MYSQL_STMT_VALUE_TEXT:
    case MYSQL_STMT_VALUE_BLOB:
      return mysql_stmt_emit_lenenc_bytes(
          out, capacity, offset,
          value->data.bytes.data, value->data.bytes.size);
    default:
      return MYSQL_WIRE_STATUS_INVALID;
  }
}

mysql_wire_status_t mysql_wire_build_stmt_prepare(
    const uint8_t *sql, size_t sql_size,
    uint8_t *out, size_t out_capacity, size_t *out_size) {
  size_t offset = 0u;
  mysql_wire_status_t status;

  if (sql == NULL || sql_size == 0u || out == NULL || out_size == NULL)
    return MYSQL_WIRE_STATUS_INVALID;
  *out_size = 0u;

  status = mysql_stmt_emit_byte(
      out, out_capacity, &offset, MYSQL_COM_STMT_PREPARE);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_stmt_emit(out, out_capacity, &offset, sql, sql_size);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  *out_size = offset;
  return MYSQL_WIRE_STATUS_OK;
}

mysql_wire_status_t mysql_wire_decode_stmt_prepare_ok(
    const uint8_t *payload, size_t payload_size,
    mysql_stmt_prepare_ok_t *out) {
  size_t offset = 1u;
  uint8_t reserved;
  mysql_wire_status_t status;

  if (payload == NULL || out == NULL)
    return MYSQL_WIRE_STATUS_INVALID;
  memset(out, 0, sizeof(*out));

  if (payload_size < 12u)
    return MYSQL_WIRE_STATUS_NEED_MORE;
  if (payload[0] != UINT8_C(0x00))
    return MYSQL_WIRE_STATUS_INVALID;

  status = mysql_wire_read_u32_le(
      payload, payload_size, &offset, &out->statement_id);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_wire_read_u16_le(
      payload, payload_size, &offset, &out->column_count);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_wire_read_u16_le(
      payload, payload_size, &offset, &out->parameter_count);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  reserved = payload[offset++];
  if (reserved != 0u)
    return MYSQL_WIRE_STATUS_INVALID;

  status = mysql_wire_read_u16_le(
      payload, payload_size, &offset, &out->warning_count);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  return offset == payload_size
             ? MYSQL_WIRE_STATUS_OK
             : MYSQL_WIRE_STATUS_INVALID;
}

mysql_wire_status_t mysql_wire_build_stmt_close(
    uint32_t statement_id,
    uint8_t *out, size_t out_capacity, size_t *out_size) {
  size_t offset = 0u;
  mysql_wire_status_t status;

  if (statement_id == 0u || out == NULL || out_size == NULL)
    return MYSQL_WIRE_STATUS_INVALID;
  *out_size = 0u;

  status = mysql_stmt_emit_byte(
      out, out_capacity, &offset, MYSQL_COM_STMT_CLOSE);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_wire_write_u32_le(
      out, out_capacity, &offset, statement_id);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  *out_size = offset;
  return MYSQL_WIRE_STATUS_OK;
}

mysql_wire_status_t mysql_wire_build_stmt_reset(
    uint32_t statement_id,
    uint8_t *out, size_t out_capacity, size_t *out_size) {
  size_t offset = 0u;
  mysql_wire_status_t status;

  if (statement_id == 0u || out == NULL || out_size == NULL)
    return MYSQL_WIRE_STATUS_INVALID;
  *out_size = 0u;

  status = mysql_stmt_emit_byte(
      out, out_capacity, &offset, MYSQL_COM_STMT_RESET);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_wire_write_u32_le(
      out, out_capacity, &offset, statement_id);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  *out_size = offset;
  return MYSQL_WIRE_STATUS_OK;
}

mysql_wire_status_t mysql_wire_build_stmt_execute(
    uint32_t statement_id,
    const mysql_stmt_value_t *values, size_t value_count,
    uint8_t *out, size_t out_capacity, size_t *out_size) {
  size_t offset = 0u;
  size_t null_bitmap_offset;
  size_t null_bitmap_size;
  size_t i;
  mysql_wire_status_t status;

  if (statement_id == 0u || out == NULL || out_size == NULL ||
      (values == NULL && value_count != 0u))
    return MYSQL_WIRE_STATUS_INVALID;
  *out_size = 0u;

  null_bitmap_size = (value_count + 7u) / 8u;

  status = mysql_stmt_emit_byte(
      out, out_capacity, &offset, MYSQL_COM_STMT_EXECUTE);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_wire_write_u32_le(
      out, out_capacity, &offset, statement_id);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  status = mysql_stmt_emit_byte(out, out_capacity, &offset, 0u);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_wire_write_u32_le(
      out, out_capacity, &offset, UINT32_C(1));
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  null_bitmap_offset = offset;
  if (null_bitmap_size != 0u) {
    if (offset > out_capacity || out_capacity - offset < null_bitmap_size)
      return MYSQL_WIRE_STATUS_LIMIT;
    memset(out + offset, 0, null_bitmap_size);
    offset += null_bitmap_size;
  }

  if (value_count != 0u) {
    status = mysql_stmt_emit_byte(out, out_capacity, &offset, UINT8_C(1));
    if (status != MYSQL_WIRE_STATUS_OK)
      return status;

    for (i = 0u; i < value_count; ++i) {
      uint8_t type;
      uint8_t flags;
      status = mysql_stmt_value_type(&values[i], &type, &flags);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
      status = mysql_stmt_emit_byte(out, out_capacity, &offset, type);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
      status = mysql_stmt_emit_byte(out, out_capacity, &offset, flags);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
      if (values[i].kind == MYSQL_STMT_VALUE_NULL)
        out[null_bitmap_offset + i / 8u] |=
            (uint8_t)(UINT8_C(1) << (i % 8u));
    }

    for (i = 0u; i < value_count; ++i) {
      status = mysql_stmt_emit_value(
          out, out_capacity, &offset, &values[i]);
      if (status != MYSQL_WIRE_STATUS_OK)
        return status;
    }
  }

  *out_size = offset;
  return MYSQL_WIRE_STATUS_OK;
}
