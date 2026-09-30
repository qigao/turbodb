#ifndef TURBODB_MYSQL_WIRE_HANDSHAKE_H
#define TURBODB_MYSQL_WIRE_HANDSHAKE_H

#include "codec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYSQL_WIRE_CLIENT_LONG_PASSWORD UINT32_C(0x00000001)
#define MYSQL_WIRE_CLIENT_LONG_FLAG UINT32_C(0x00000004)
#define MYSQL_WIRE_CLIENT_CONNECT_WITH_DB UINT32_C(0x00000008)
#define MYSQL_WIRE_CLIENT_PROTOCOL_41 UINT32_C(0x00000200)
#define MYSQL_WIRE_CLIENT_SSL UINT32_C(0x00000800)
#define MYSQL_WIRE_CLIENT_TRANSACTIONS UINT32_C(0x00002000)
#define MYSQL_WIRE_CLIENT_SECURE_CONNECTION UINT32_C(0x00008000)
#define MYSQL_WIRE_CLIENT_MULTI_STATEMENTS UINT32_C(0x00010000)
#define MYSQL_WIRE_CLIENT_MULTI_RESULTS UINT32_C(0x00020000)
#define MYSQL_WIRE_CLIENT_PLUGIN_AUTH UINT32_C(0x00080000)
#define MYSQL_WIRE_CLIENT_DEPRECATE_EOF UINT32_C(0x01000000)

#define MYSQL_WIRE_AUTH_PLUGIN_NAME_CAPACITY 64u
#define MYSQL_WIRE_SERVER_VERSION_CAPACITY 128u
#define MYSQL_WIRE_AUTH_DATA_CAPACITY 32u

typedef struct mysql_wire_greeting_t {
  uint8_t protocol_version;
  char server_version[MYSQL_WIRE_SERVER_VERSION_CAPACITY];
  uint32_t connection_id;
  uint32_t capabilities;
  uint8_t character_set;
  uint16_t status_flags;
  uint8_t auth_plugin_data[MYSQL_WIRE_AUTH_DATA_CAPACITY];
  size_t auth_plugin_data_length;
  char auth_plugin[MYSQL_WIRE_AUTH_PLUGIN_NAME_CAPACITY];
} mysql_wire_greeting_t;

typedef struct mysql_wire_auth_switch_t {
  char plugin[MYSQL_WIRE_AUTH_PLUGIN_NAME_CAPACITY];
  uint8_t data[MYSQL_WIRE_AUTH_DATA_CAPACITY];
  size_t data_length;
} mysql_wire_auth_switch_t;

mysql_wire_status_t mysql_wire_parse_greeting(
    const uint8_t *payload, size_t payload_size, mysql_wire_greeting_t *out);

mysql_wire_status_t mysql_wire_select_client_capabilities(
    const mysql_wire_greeting_t *greeting, bool use_tls, bool use_database,
    uint32_t *out_capabilities);

mysql_wire_status_t mysql_wire_build_ssl_request(
    uint32_t capabilities, uint32_t max_packet_size, uint8_t character_set,
    uint8_t *out, size_t out_capacity, size_t *out_size);

mysql_wire_status_t mysql_wire_build_handshake_response(
    uint32_t capabilities, uint32_t max_packet_size, uint8_t character_set,
    const char *username, const uint8_t *auth_response,
    size_t auth_response_size, const char *database, const char *auth_plugin,
    uint8_t *out, size_t out_capacity, size_t *out_size);

mysql_wire_status_t mysql_wire_parse_auth_switch(
    const uint8_t *payload, size_t payload_size, mysql_wire_auth_switch_t *out);

#ifdef __cplusplus
}
#endif

#endif
