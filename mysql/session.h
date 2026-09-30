#ifndef TURBODB_MYSQL_SESSION_H
#define TURBODB_MYSQL_SESSION_H

#include "wire/statement.h"

#include <stddef.h>
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
  MYSQL_SESSION_TIMEOUT = 5,
  MYSQL_SESSION_COMMIT_UNKNOWN = 6,
  MYSQL_SESSION_INVALID_STATE = 7,
  MYSQL_SESSION_UNSUPPORTED = 8,
  MYSQL_SESSION_SQL_ERROR = 9
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
 * Opens a bounded caller-driven CNet session, verifies TLS and authentication,
 * sends COM_PING, then closes the session. Config strings are borrowed until
 * return. Returns MYSQL_SESSION_OK or a native status; error may be NULL.
 */
mysql_session_status_t mysql_session_connect_and_ping(
    const mysql_session_config_t *config, mysql_session_error_t *error);

typedef struct mysql_session_command_result_t {
  uint64_t affected_rows;
  uint64_t last_insert_id;
  uint16_t status_flags;
  uint16_t warnings;
} mysql_session_command_result_t;

/*
 * Executes one command in a dedicated authenticated session, then closes it.
 * SQL/config/parameter views are borrowed only for this call. Binary binding
 * uses COM_STMT_PREPARE/EXECUTE. max_command_bytes bounds command storage.
 * out receives affected rows and insert ID on success; errors return a native
 * status and optional diagnostics. No client-side SQL escaping is performed.
 */
mysql_session_status_t mysql_session_execute_prepared(
    const mysql_session_config_t *config,
    const uint8_t *sql, size_t sql_size,
    const mysql_stmt_value_t *parameters, size_t parameter_count,
    size_t max_command_bytes,
    mysql_session_command_result_t *out,
    mysql_session_error_t *error);

#ifdef __cplusplus
}
#endif

#endif
