#ifndef TURBODB_ORM_MYSQL_WIRE_RESULT_H
#define TURBODB_ORM_MYSQL_WIRE_RESULT_H

#include "codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYSQL_WIRE_CLIENT_PROTOCOL_41 UINT32_C(0x00000200)
#define MYSQL_WIRE_CLIENT_SESSION_TRACK UINT32_C(0x00800000)
#define MYSQL_WIRE_CLIENT_DEPRECATE_EOF UINT32_C(0x01000000)
#define MYSQL_WIRE_SERVER_SESSION_STATE_CHANGED UINT16_C(0x4000)

typedef struct mysql_wire_ok_packet_t {
  uint8_t header;
  uint64_t affected_rows;
  uint64_t last_insert_id;
  uint16_t status_flags;
  uint16_t warnings;
  mysql_wire_bytes_t info;
  mysql_wire_bytes_t session_state;
} mysql_wire_ok_packet_t;

typedef struct mysql_wire_err_packet_t {
  uint16_t error_code;
  bool has_sql_state;
  char sql_state[6];
  mysql_wire_bytes_t message;
} mysql_wire_err_packet_t;

typedef struct mysql_wire_eof_packet_t {
  uint16_t warnings;
  uint16_t status_flags;
} mysql_wire_eof_packet_t;

mysql_wire_status_t mysql_wire_decode_ok_packet(
    const uint8_t *payload, size_t payload_size, uint32_t capabilities,
    mysql_wire_ok_packet_t *out);
mysql_wire_status_t mysql_wire_decode_err_packet(
    const uint8_t *payload, size_t payload_size, uint32_t capabilities,
    mysql_wire_err_packet_t *out);
mysql_wire_status_t mysql_wire_decode_eof_packet(
    const uint8_t *payload, size_t payload_size, uint32_t capabilities,
    mysql_wire_eof_packet_t *out);

#ifdef __cplusplus
}
#endif

#endif
