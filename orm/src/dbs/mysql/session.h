#ifndef TURBODB_ORM_MYSQL_SESSION_H
#define TURBODB_ORM_MYSQL_SESSION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mysql_session_status_t {
  MYSQL_SESSION_OK = 0,
  MYSQL_SESSION_INVALID = 1,
  MYSQL_SESSION_IO = 2,
  MYSQL_SESSION_PROTOCOL = 3,
  MYSQL_SESSION_AUTH = 4,
  MYSQL_SESSION_TIMEOUT = 5
} mysql_session_status_t;

typedef struct mysql_session_config_t {
  const char *host;
  uint16_t port;
  const char *username;
  const char *password;
  const char *database;
  const char *ca_file;
  const char *server_name;
  uint32_t timeout_ms;
} mysql_session_config_t;

typedef struct mysql_session_error_t {
  mysql_session_status_t status;
  int cnet_status;
  int cnet_native_status;
  uint16_t server_error;
  char sql_state[6];
  char stage[48];
  char message[256];
} mysql_session_error_t;

/*
 * M2 caller-driven qualification API. It owns one bounded CNet client on the
 * calling thread, performs HandshakeV10 + verified STARTTLS + authentication,
 * sends COM_PING, and tears the session down. It is not a public Driver ABI.
 */
mysql_session_status_t mysql_session_connect_and_ping(
    const mysql_session_config_t *config, mysql_session_error_t *error);

#ifdef __cplusplus
}
#endif

#endif
