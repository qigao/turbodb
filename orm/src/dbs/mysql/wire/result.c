#include "result.h"

#include <string.h>

static mysql_wire_status_t mysql_wire_complete_packet_status(
    mysql_wire_status_t status) {
  return status == MYSQL_WIRE_STATUS_NEED_MORE
             ? MYSQL_WIRE_STATUS_INVALID
             : status;
}

mysql_wire_status_t mysql_wire_decode_ok_packet(
    const uint8_t *payload, size_t payload_size, uint32_t capabilities,
    mysql_wire_ok_packet_t *out) {
  size_t offset = 1u;
  bool is_null;
  mysql_wire_status_t status;

  if (payload == NULL || out == NULL || payload_size == 0u)
    return MYSQL_WIRE_STATUS_INVALID;

  memset(out, 0, sizeof(*out));
  out->header = payload[0];
  if (out->header != UINT8_C(0x00)) {
    if (out->header != UINT8_C(0xfe) ||
        (capabilities & MYSQL_WIRE_CLIENT_DEPRECATE_EOF) == 0u ||
        payload_size < 7u)
      return MYSQL_WIRE_STATUS_INVALID;
  }

  status = mysql_wire_read_lenenc_uint(
      payload, payload_size, &offset, &out->affected_rows, &is_null);
  status = mysql_wire_complete_packet_status(status);
  if (status != MYSQL_WIRE_STATUS_OK || is_null)
    return status == MYSQL_WIRE_STATUS_OK ? MYSQL_WIRE_STATUS_INVALID : status;

  status = mysql_wire_read_lenenc_uint(
      payload, payload_size, &offset, &out->last_insert_id, &is_null);
  status = mysql_wire_complete_packet_status(status);
  if (status != MYSQL_WIRE_STATUS_OK || is_null)
    return status == MYSQL_WIRE_STATUS_OK ? MYSQL_WIRE_STATUS_INVALID : status;

  if ((capabilities & MYSQL_WIRE_CLIENT_PROTOCOL_41) != 0u) {
    status = mysql_wire_complete_packet_status(mysql_wire_read_u16_le(
        payload, payload_size, &offset, &out->status_flags));
    if (status != MYSQL_WIRE_STATUS_OK)
      return status;
    status = mysql_wire_complete_packet_status(mysql_wire_read_u16_le(
        payload, payload_size, &offset, &out->warnings));
    if (status != MYSQL_WIRE_STATUS_OK)
      return status;
  }

  if ((capabilities & MYSQL_WIRE_CLIENT_SESSION_TRACK) != 0u) {
    status = mysql_wire_complete_packet_status(mysql_wire_read_lenenc_bytes(
        payload, payload_size, &offset, &out->info));
    if (status != MYSQL_WIRE_STATUS_OK || out->info.is_null)
      return status == MYSQL_WIRE_STATUS_OK ? MYSQL_WIRE_STATUS_INVALID : status;

    if ((out->status_flags & MYSQL_WIRE_SERVER_SESSION_STATE_CHANGED) != 0u) {
      status = mysql_wire_complete_packet_status(mysql_wire_read_lenenc_bytes(
          payload, payload_size, &offset, &out->session_state));
      if (status != MYSQL_WIRE_STATUS_OK || out->session_state.is_null)
        return status == MYSQL_WIRE_STATUS_OK
                   ? MYSQL_WIRE_STATUS_INVALID
                   : status;
    }
    if (offset != payload_size)
      return MYSQL_WIRE_STATUS_INVALID;
  } else {
    out->info.data = payload + offset;
    out->info.length = payload_size - offset;
    out->info.is_null = false;
  }

  return MYSQL_WIRE_STATUS_OK;
}

mysql_wire_status_t mysql_wire_decode_err_packet(
    const uint8_t *payload, size_t payload_size, uint32_t capabilities,
    mysql_wire_err_packet_t *out) {
  size_t offset = 1u;
  mysql_wire_status_t status;

  if (payload == NULL || out == NULL || payload_size < 3u ||
      payload[0] != UINT8_C(0xff))
    return MYSQL_WIRE_STATUS_INVALID;

  memset(out, 0, sizeof(*out));
  status = mysql_wire_complete_packet_status(mysql_wire_read_u16_le(
      payload, payload_size, &offset, &out->error_code));
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  if ((capabilities & MYSQL_WIRE_CLIENT_PROTOCOL_41) != 0u) {
    if (payload_size - offset < 6u || payload[offset] != (uint8_t)'#')
      return MYSQL_WIRE_STATUS_INVALID;
    ++offset;
    memcpy(out->sql_state, payload + offset, 5u);
    out->sql_state[5] = '\0';
    out->has_sql_state = true;
    offset += 5u;
  }

  out->message.data = payload + offset;
  out->message.length = payload_size - offset;
  out->message.is_null = false;
  return MYSQL_WIRE_STATUS_OK;
}

mysql_wire_status_t mysql_wire_decode_eof_packet(
    const uint8_t *payload, size_t payload_size, uint32_t capabilities,
    mysql_wire_eof_packet_t *out) {
  size_t offset = 1u;
  mysql_wire_status_t status;

  if (payload == NULL || out == NULL || payload_size == 0u ||
      payload[0] != UINT8_C(0xfe) || payload_size >= 9u)
    return MYSQL_WIRE_STATUS_INVALID;

  memset(out, 0, sizeof(*out));
  if ((capabilities & MYSQL_WIRE_CLIENT_PROTOCOL_41) == 0u)
    return payload_size == 1u ? MYSQL_WIRE_STATUS_OK
                              : MYSQL_WIRE_STATUS_INVALID;

  if (payload_size != 5u)
    return MYSQL_WIRE_STATUS_INVALID;

  status = mysql_wire_complete_packet_status(mysql_wire_read_u16_le(
      payload, payload_size, &offset, &out->warnings));
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_wire_complete_packet_status(mysql_wire_read_u16_le(
      payload, payload_size, &offset, &out->status_flags));
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  return offset == payload_size ? MYSQL_WIRE_STATUS_OK
                                : MYSQL_WIRE_STATUS_INVALID;
}
