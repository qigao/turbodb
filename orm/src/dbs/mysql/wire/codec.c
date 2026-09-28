#include "codec.h"

#include <limits.h>
#include <string.h>

static mysql_wire_status_t mysql_wire_read_le(
    const uint8_t *data, size_t size, size_t *offset, unsigned width,
    uint64_t *out) {
  size_t cursor;
  uint64_t value = 0u;
  unsigned i;

  if (data == NULL || offset == NULL || out == NULL || width == 0u ||
      width > 8u)
    return MYSQL_WIRE_STATUS_INVALID;

  cursor = *offset;
  if (cursor > size || size - cursor < (size_t)width)
    return MYSQL_WIRE_STATUS_NEED_MORE;

  for (i = 0u; i < width; ++i)
    value |= (uint64_t)data[cursor + i] << (8u * i);

  *offset = cursor + (size_t)width;
  *out = value;
  return MYSQL_WIRE_STATUS_OK;
}

static mysql_wire_status_t mysql_wire_write_le(
    uint8_t *data, size_t size, size_t *offset, unsigned width,
    uint64_t value) {
  size_t cursor;
  unsigned i;

  if (data == NULL || offset == NULL || width == 0u || width > 8u)
    return MYSQL_WIRE_STATUS_INVALID;

  cursor = *offset;
  if (cursor > size || size - cursor < (size_t)width)
    return MYSQL_WIRE_STATUS_LIMIT;

  for (i = 0u; i < width; ++i)
    data[cursor + i] = (uint8_t)(value >> (8u * i));

  *offset = cursor + (size_t)width;
  return MYSQL_WIRE_STATUS_OK;
}

mysql_wire_status_t mysql_wire_read_u16_le(
    const uint8_t *data, size_t size, size_t *offset, uint16_t *out) {
  uint64_t value;
  mysql_wire_status_t status;
  if (out == NULL)
    return MYSQL_WIRE_STATUS_INVALID;
  status = mysql_wire_read_le(data, size, offset, 2u, &value);
  if (status == MYSQL_WIRE_STATUS_OK)
    *out = (uint16_t)value;
  return status;
}

mysql_wire_status_t mysql_wire_read_u24_le(
    const uint8_t *data, size_t size, size_t *offset, uint32_t *out) {
  uint64_t value;
  mysql_wire_status_t status;
  if (out == NULL)
    return MYSQL_WIRE_STATUS_INVALID;
  status = mysql_wire_read_le(data, size, offset, 3u, &value);
  if (status == MYSQL_WIRE_STATUS_OK)
    *out = (uint32_t)value;
  return status;
}

mysql_wire_status_t mysql_wire_read_u32_le(
    const uint8_t *data, size_t size, size_t *offset, uint32_t *out) {
  uint64_t value;
  mysql_wire_status_t status;
  if (out == NULL)
    return MYSQL_WIRE_STATUS_INVALID;
  status = mysql_wire_read_le(data, size, offset, 4u, &value);
  if (status == MYSQL_WIRE_STATUS_OK)
    *out = (uint32_t)value;
  return status;
}

mysql_wire_status_t mysql_wire_read_u64_le(
    const uint8_t *data, size_t size, size_t *offset, uint64_t *out) {
  return mysql_wire_read_le(data, size, offset, 8u, out);
}

mysql_wire_status_t mysql_wire_write_u16_le(
    uint8_t *data, size_t size, size_t *offset, uint16_t value) {
  return mysql_wire_write_le(data, size, offset, 2u, value);
}

mysql_wire_status_t mysql_wire_write_u24_le(
    uint8_t *data, size_t size, size_t *offset, uint32_t value) {
  if (value > UINT32_C(0x00ffffff))
    return MYSQL_WIRE_STATUS_LIMIT;
  return mysql_wire_write_le(data, size, offset, 3u, value);
}

mysql_wire_status_t mysql_wire_write_u32_le(
    uint8_t *data, size_t size, size_t *offset, uint32_t value) {
  return mysql_wire_write_le(data, size, offset, 4u, value);
}

mysql_wire_status_t mysql_wire_write_u64_le(
    uint8_t *data, size_t size, size_t *offset, uint64_t value) {
  return mysql_wire_write_le(data, size, offset, 8u, value);
}

mysql_wire_status_t mysql_wire_read_lenenc_uint(
    const uint8_t *data, size_t size, size_t *offset, uint64_t *value,
    bool *is_null) {
  size_t cursor;
  uint8_t first;
  uint64_t decoded = 0u;
  mysql_wire_status_t status;

  if (data == NULL || offset == NULL || value == NULL || is_null == NULL)
    return MYSQL_WIRE_STATUS_INVALID;

  cursor = *offset;
  if (cursor >= size)
    return MYSQL_WIRE_STATUS_NEED_MORE;

  first = data[cursor++];
  if (first <= UINT8_C(0xfa)) {
    decoded = first;
  } else if (first == UINT8_C(0xfb)) {
    *offset = cursor;
    *value = 0u;
    *is_null = true;
    return MYSQL_WIRE_STATUS_OK;
  } else if (first == UINT8_C(0xfc)) {
    uint16_t v16;
    status = mysql_wire_read_u16_le(data, size, &cursor, &v16);
    if (status != MYSQL_WIRE_STATUS_OK)
      return status;
    decoded = v16;
  } else if (first == UINT8_C(0xfd)) {
    uint32_t v24;
    status = mysql_wire_read_u24_le(data, size, &cursor, &v24);
    if (status != MYSQL_WIRE_STATUS_OK)
      return status;
    decoded = v24;
  } else if (first == UINT8_C(0xfe)) {
    status = mysql_wire_read_u64_le(data, size, &cursor, &decoded);
    if (status != MYSQL_WIRE_STATUS_OK)
      return status;
  } else {
    return MYSQL_WIRE_STATUS_INVALID;
  }

  *offset = cursor;
  *value = decoded;
  *is_null = false;
  return MYSQL_WIRE_STATUS_OK;
}

mysql_wire_status_t mysql_wire_read_lenenc_bytes(
    const uint8_t *data, size_t size, size_t *offset,
    mysql_wire_bytes_t *out) {
  size_t cursor;
  uint64_t length;
  bool is_null;
  mysql_wire_status_t status;

  if (data == NULL || offset == NULL || out == NULL)
    return MYSQL_WIRE_STATUS_INVALID;

  out->data = NULL;
  out->length = 0u;
  out->is_null = false;
  cursor = *offset;
  status = mysql_wire_read_lenenc_uint(
      data, size, &cursor, &length, &is_null);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  if (is_null) {
    out->is_null = true;
    *offset = cursor;
    return MYSQL_WIRE_STATUS_OK;
  }

#if SIZE_MAX < UINT64_MAX
  if (length > (uint64_t)SIZE_MAX)
    return MYSQL_WIRE_STATUS_LIMIT;
#endif
  if (cursor > size || (uint64_t)(size - cursor) < length)
    return MYSQL_WIRE_STATUS_NEED_MORE;

  out->data = data + cursor;
  out->length = (size_t)length;
  cursor += (size_t)length;
  *offset = cursor;
  return MYSQL_WIRE_STATUS_OK;
}

mysql_wire_status_t mysql_wire_write_lenenc_uint(
    uint8_t *data, size_t size, size_t *offset, uint64_t value) {
  size_t cursor;

  if (data == NULL || offset == NULL)
    return MYSQL_WIRE_STATUS_INVALID;

  cursor = *offset;
  if (value <= UINT64_C(250)) {
    if (cursor >= size)
      return MYSQL_WIRE_STATUS_LIMIT;
    data[cursor++] = (uint8_t)value;
  } else if (value < UINT64_C(1) << 16u) {
    if (cursor >= size)
      return MYSQL_WIRE_STATUS_LIMIT;
    data[cursor++] = UINT8_C(0xfc);
    if (mysql_wire_write_u16_le(data, size, &cursor, (uint16_t)value) !=
        MYSQL_WIRE_STATUS_OK)
      return MYSQL_WIRE_STATUS_LIMIT;
  } else if (value < UINT64_C(1) << 24u) {
    if (cursor >= size)
      return MYSQL_WIRE_STATUS_LIMIT;
    data[cursor++] = UINT8_C(0xfd);
    if (mysql_wire_write_u24_le(data, size, &cursor, (uint32_t)value) !=
        MYSQL_WIRE_STATUS_OK)
      return MYSQL_WIRE_STATUS_LIMIT;
  } else {
    if (cursor >= size)
      return MYSQL_WIRE_STATUS_LIMIT;
    data[cursor++] = UINT8_C(0xfe);
    if (mysql_wire_write_u64_le(data, size, &cursor, value) !=
        MYSQL_WIRE_STATUS_OK)
      return MYSQL_WIRE_STATUS_LIMIT;
  }

  *offset = cursor;
  return MYSQL_WIRE_STATUS_OK;
}

mysql_wire_status_t mysql_wire_write_lenenc_null(
    uint8_t *data, size_t size, size_t *offset) {
  if (data == NULL || offset == NULL)
    return MYSQL_WIRE_STATUS_INVALID;
  if (*offset >= size)
    return MYSQL_WIRE_STATUS_LIMIT;
  data[(*offset)++] = UINT8_C(0xfb);
  return MYSQL_WIRE_STATUS_OK;
}
