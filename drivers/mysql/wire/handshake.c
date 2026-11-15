#include "handshake.h"

#include <string.h>

static mysql_wire_status_t mysql_wire_read_cstring(
    const uint8_t *data, size_t size, size_t *offset,
    char *out, size_t out_capacity) {
  size_t cursor;
  size_t start;
  size_t length;

  if (data == NULL || offset == NULL || out == NULL || out_capacity == 0u)
    return MYSQL_WIRE_STATUS_INVALID;

  cursor = *offset;
  if (cursor >= size)
    return MYSQL_WIRE_STATUS_NEED_MORE;
  start = cursor;
  while (cursor < size && data[cursor] != 0u)
    ++cursor;
  if (cursor == size)
    return MYSQL_WIRE_STATUS_NEED_MORE;
  length = cursor - start;
  if (length + 1u > out_capacity)
    return MYSQL_WIRE_STATUS_LIMIT;

  if (length != 0u)
    memcpy(out, data + start, length);
  out[length] = '\0';
  *offset = cursor + 1u;
  return MYSQL_WIRE_STATUS_OK;
}

static mysql_wire_status_t mysql_wire_write_bytes(
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

static mysql_wire_status_t mysql_wire_write_cstring(
    uint8_t *out, size_t capacity, size_t *offset, const char *value) {
  size_t length;
  mysql_wire_status_t status;
  if (value == NULL)
    return MYSQL_WIRE_STATUS_INVALID;
  length = strlen(value);
  status = mysql_wire_write_bytes(out, capacity, offset, value, length);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  if (*offset >= capacity)
    return MYSQL_WIRE_STATUS_LIMIT;
  out[(*offset)++] = 0u;
  return MYSQL_WIRE_STATUS_OK;
}

mysql_wire_status_t mysql_wire_parse_greeting(
    const uint8_t *payload, size_t payload_size, mysql_wire_greeting_t *out) {
  size_t offset = 0u;
  size_t part2_wire_length;
  size_t part2_available;
  size_t part2_copy;
  uint16_t capability_low;
  uint16_t capability_high;
  uint8_t auth_plugin_data_length;
  mysql_wire_status_t status;

  if (payload == NULL || out == NULL)
    return MYSQL_WIRE_STATUS_INVALID;
  memset(out, 0, sizeof(*out));
  if (payload_size < 1u)
    return MYSQL_WIRE_STATUS_NEED_MORE;

  out->protocol_version = payload[offset++];
  if (out->protocol_version != UINT8_C(10))
    return MYSQL_WIRE_STATUS_INVALID;

  status = mysql_wire_read_cstring(payload, payload_size, &offset,
                                   out->server_version,
                                   sizeof(out->server_version));
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  status = mysql_wire_read_u32_le(payload, payload_size, &offset,
                                  &out->connection_id);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  if (payload_size - offset < 8u + 1u)
    return MYSQL_WIRE_STATUS_NEED_MORE;
  memcpy(out->auth_plugin_data, payload + offset, 8u);
  out->auth_plugin_data_length = 8u;
  offset += 8u;
  if (payload[offset++] != 0u)
    return MYSQL_WIRE_STATUS_INVALID;

  status = mysql_wire_read_u16_le(payload, payload_size, &offset,
                                  &capability_low);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  if (offset == payload_size) {
    out->capabilities = capability_low;
    return MYSQL_WIRE_STATUS_OK;
  }

  if (payload_size - offset < 1u + 2u + 2u + 1u + 10u)
    return MYSQL_WIRE_STATUS_NEED_MORE;

  out->character_set = payload[offset++];
  status = mysql_wire_read_u16_le(payload, payload_size, &offset,
                                  &out->status_flags);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_wire_read_u16_le(payload, payload_size, &offset,
                                  &capability_high);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  out->capabilities =
      (uint32_t)capability_low | ((uint32_t)capability_high << 16u);

  auth_plugin_data_length = payload[offset++];
  offset += 10u;

  if ((out->capabilities & MYSQL_WIRE_CLIENT_SECURE_CONNECTION) != 0u &&
      offset < payload_size) {
    part2_wire_length =
        auth_plugin_data_length > 8u ? (size_t)auth_plugin_data_length - 8u
                                    : 13u;
    if (part2_wire_length < 13u)
      part2_wire_length = 13u;
    part2_available = payload_size - offset;
    if (part2_wire_length > part2_available)
      part2_wire_length = part2_available;

    part2_copy = part2_wire_length;
    if (part2_copy != 0u &&
        payload[offset + part2_copy - 1u] == 0u)
      --part2_copy;
    if (part2_copy >
        sizeof(out->auth_plugin_data) - out->auth_plugin_data_length)
      return MYSQL_WIRE_STATUS_LIMIT;
    if (part2_copy != 0u)
      memcpy(out->auth_plugin_data + out->auth_plugin_data_length,
             payload + offset, part2_copy);
    out->auth_plugin_data_length += part2_copy;
    offset += part2_wire_length;
  }

  if ((out->capabilities & MYSQL_WIRE_CLIENT_PLUGIN_AUTH) != 0u &&
      offset < payload_size) {
    status = mysql_wire_read_cstring(payload, payload_size, &offset,
                                     out->auth_plugin,
                                     sizeof(out->auth_plugin));
    if (status != MYSQL_WIRE_STATUS_OK)
      return status;
  }

  if (out->auth_plugin_data_length > 20u)
    out->auth_plugin_data_length = 20u;
  return MYSQL_WIRE_STATUS_OK;
}

mysql_wire_status_t mysql_wire_select_client_capabilities(
    const mysql_wire_greeting_t *greeting, bool use_tls, bool use_database,
    uint32_t *out_capabilities) {
  const uint32_t required =
      MYSQL_WIRE_CLIENT_PROTOCOL_41 |
      MYSQL_WIRE_CLIENT_SECURE_CONNECTION |
      MYSQL_WIRE_CLIENT_PLUGIN_AUTH;
  uint32_t selected;

  if (greeting == NULL || out_capabilities == NULL)
    return MYSQL_WIRE_STATUS_INVALID;
  *out_capabilities = 0u;

  if ((greeting->capabilities & required) != required)
    return MYSQL_WIRE_STATUS_INVALID;
  if (use_tls && (greeting->capabilities & MYSQL_WIRE_CLIENT_SSL) == 0u)
    return MYSQL_WIRE_STATUS_INVALID;
  if (use_database &&
      (greeting->capabilities & MYSQL_WIRE_CLIENT_CONNECT_WITH_DB) == 0u)
    return MYSQL_WIRE_STATUS_INVALID;

  selected = required |
             (greeting->capabilities &
              (MYSQL_WIRE_CLIENT_LONG_PASSWORD |
               MYSQL_WIRE_CLIENT_LONG_FLAG));

  if (use_tls)
    selected |= MYSQL_WIRE_CLIENT_SSL;
  if (use_database)
    selected |= MYSQL_WIRE_CLIENT_CONNECT_WITH_DB;

  selected |= greeting->capabilities &
              (MYSQL_WIRE_CLIENT_TRANSACTIONS |
               MYSQL_WIRE_CLIENT_MULTI_RESULTS |
               MYSQL_WIRE_CLIENT_DEPRECATE_EOF);

  *out_capabilities = selected;
  return MYSQL_WIRE_STATUS_OK;
}

mysql_wire_status_t mysql_wire_build_ssl_request(
    uint32_t capabilities, uint32_t max_packet_size, uint8_t character_set,
    uint8_t *out, size_t out_capacity, size_t *out_size) {
  size_t offset = 0u;
  uint8_t reserved[23] = {0};
  mysql_wire_status_t status;

  if (out == NULL || out_size == NULL ||
      (capabilities & MYSQL_WIRE_CLIENT_SSL) == 0u)
    return MYSQL_WIRE_STATUS_INVALID;
  *out_size = 0u;

  status = mysql_wire_write_u32_le(out, out_capacity, &offset, capabilities);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_wire_write_u32_le(out, out_capacity, &offset, max_packet_size);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  if (offset >= out_capacity)
    return MYSQL_WIRE_STATUS_LIMIT;
  out[offset++] = character_set;
  status = mysql_wire_write_bytes(out, out_capacity, &offset,
                                  reserved, sizeof(reserved));
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  *out_size = offset;
  return offset == 32u ? MYSQL_WIRE_STATUS_OK : MYSQL_WIRE_STATUS_INVALID;
}

mysql_wire_status_t mysql_wire_build_handshake_response(
    uint32_t capabilities, uint32_t max_packet_size, uint8_t character_set,
    const char *username, const uint8_t *auth_response,
    size_t auth_response_size, const char *database, const char *auth_plugin,
    uint8_t *out, size_t out_capacity, size_t *out_size) {
  size_t offset = 0u;
  uint8_t reserved[23] = {0};
  mysql_wire_status_t status;

  if (username == NULL || auth_plugin == NULL || out == NULL ||
      out_size == NULL || (auth_response == NULL && auth_response_size != 0u) ||
      auth_response_size > UINT8_MAX ||
      (capabilities & MYSQL_WIRE_CLIENT_PROTOCOL_41) == 0u ||
      (capabilities & MYSQL_WIRE_CLIENT_SECURE_CONNECTION) == 0u ||
      (capabilities & MYSQL_WIRE_CLIENT_PLUGIN_AUTH) == 0u)
    return MYSQL_WIRE_STATUS_INVALID;
  if ((capabilities & MYSQL_WIRE_CLIENT_CONNECT_WITH_DB) != 0u &&
      database == NULL)
    return MYSQL_WIRE_STATUS_INVALID;
  *out_size = 0u;

  status = mysql_wire_write_u32_le(out, out_capacity, &offset, capabilities);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_wire_write_u32_le(out, out_capacity, &offset, max_packet_size);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  if (offset >= out_capacity)
    return MYSQL_WIRE_STATUS_LIMIT;
  out[offset++] = character_set;
  status = mysql_wire_write_bytes(out, out_capacity, &offset,
                                  reserved, sizeof(reserved));
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  status = mysql_wire_write_cstring(out, out_capacity, &offset, username);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  if (offset >= out_capacity)
    return MYSQL_WIRE_STATUS_LIMIT;
  out[offset++] = (uint8_t)auth_response_size;
  status = mysql_wire_write_bytes(out, out_capacity, &offset,
                                  auth_response, auth_response_size);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  if ((capabilities & MYSQL_WIRE_CLIENT_CONNECT_WITH_DB) != 0u) {
    status = mysql_wire_write_cstring(out, out_capacity, &offset, database);
    if (status != MYSQL_WIRE_STATUS_OK)
      return status;
  }

  status = mysql_wire_write_cstring(out, out_capacity, &offset, auth_plugin);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  *out_size = offset;
  return MYSQL_WIRE_STATUS_OK;
}

mysql_wire_status_t mysql_wire_parse_auth_switch(
    const uint8_t *payload, size_t payload_size, mysql_wire_auth_switch_t *out) {
  size_t offset = 1u;
  size_t remaining;
  mysql_wire_status_t status;

  if (payload == NULL || out == NULL || payload_size < 2u ||
      payload[0] != UINT8_C(0xfe))
    return MYSQL_WIRE_STATUS_INVALID;
  memset(out, 0, sizeof(*out));

  status = mysql_wire_read_cstring(payload, payload_size, &offset,
                                   out->plugin, sizeof(out->plugin));
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

  remaining = payload_size - offset;
  if (remaining != 0u && payload[payload_size - 1u] == 0u)
    --remaining;
  if (remaining > sizeof(out->data))
    return MYSQL_WIRE_STATUS_LIMIT;
  if (remaining != 0u)
    memcpy(out->data, payload + offset, remaining);
  out->data_length = remaining;
  return MYSQL_WIRE_STATUS_OK;
}
