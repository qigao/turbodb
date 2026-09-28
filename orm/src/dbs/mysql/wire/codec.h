#ifndef TURBODB_ORM_MYSQL_WIRE_CODEC_H
#define TURBODB_ORM_MYSQL_WIRE_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mysql_wire_status_t {
  MYSQL_WIRE_STATUS_OK = 0,
  MYSQL_WIRE_STATUS_NEED_MORE = 1,
  MYSQL_WIRE_STATUS_INVALID = 2,
  MYSQL_WIRE_STATUS_LIMIT = 3,
  MYSQL_WIRE_STATUS_SEQUENCE = 4
} mysql_wire_status_t;

typedef struct mysql_wire_bytes_t {
  const uint8_t *data;
  size_t length;
  bool is_null;
} mysql_wire_bytes_t;

mysql_wire_status_t mysql_wire_read_u16_le(
    const uint8_t *data, size_t size, size_t *offset, uint16_t *out);
mysql_wire_status_t mysql_wire_read_u24_le(
    const uint8_t *data, size_t size, size_t *offset, uint32_t *out);
mysql_wire_status_t mysql_wire_read_u32_le(
    const uint8_t *data, size_t size, size_t *offset, uint32_t *out);
mysql_wire_status_t mysql_wire_read_u64_le(
    const uint8_t *data, size_t size, size_t *offset, uint64_t *out);

mysql_wire_status_t mysql_wire_write_u16_le(
    uint8_t *data, size_t size, size_t *offset, uint16_t value);
mysql_wire_status_t mysql_wire_write_u24_le(
    uint8_t *data, size_t size, size_t *offset, uint32_t value);
mysql_wire_status_t mysql_wire_write_u32_le(
    uint8_t *data, size_t size, size_t *offset, uint32_t value);
mysql_wire_status_t mysql_wire_write_u64_le(
    uint8_t *data, size_t size, size_t *offset, uint64_t value);

mysql_wire_status_t mysql_wire_read_lenenc_uint(
    const uint8_t *data, size_t size, size_t *offset, uint64_t *value,
    bool *is_null);
mysql_wire_status_t mysql_wire_read_lenenc_bytes(
    const uint8_t *data, size_t size, size_t *offset,
    mysql_wire_bytes_t *out);
mysql_wire_status_t mysql_wire_write_lenenc_uint(
    uint8_t *data, size_t size, size_t *offset, uint64_t value);
mysql_wire_status_t mysql_wire_write_lenenc_null(
    uint8_t *data, size_t size, size_t *offset);

#ifdef __cplusplus
}
#endif

#endif
