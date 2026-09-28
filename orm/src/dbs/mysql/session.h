#ifndef TURBODB_ORM_MYSQL_SESSION_H
#define TURBODB_ORM_MYSQL_SESSION_H

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
  MYSQL_SESSION_COMMIT_UNKNOWN = 6
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

typedef struct mysql_session_prepared_probe_t {
  int64_t signed_value;
  uint64_t unsigned_value;
  char text[32];
  size_t text_size;
  char decimal[32];
  size_t decimal_size;
  uint32_t row_count;
} mysql_session_prepared_probe_t;

/*
 * M3 qualification API. Reuses the same bounded CNet/TLS/auth session, then
 * performs one real COM_STMT_PREPARE/EXECUTE/CLOSE round-trip against the
 * workflow fixture and decodes one binary result row. Not public Driver ABI.
 */
mysql_session_status_t mysql_session_prepared_probe(
    const mysql_session_config_t *config,
    mysql_session_prepared_probe_t *out,
    mysql_session_error_t *error);

#define MYSQL_SESSION_TEXT_PROBE_MAX_COLUMNS 4u
#define MYSQL_SESSION_TEXT_PROBE_FIELD_CAPACITY 128u

typedef struct mysql_session_text_probe_field_t {
  uint8_t is_null;
  size_t size;
  uint8_t data[MYSQL_SESSION_TEXT_PROBE_FIELD_CAPACITY];
} mysql_session_text_probe_field_t;

typedef struct mysql_session_text_probe_t {
  uint32_t column_count;
  uint32_t row_count;
  mysql_session_text_probe_field_t
      columns[MYSQL_SESSION_TEXT_PROBE_MAX_COLUMNS];
} mysql_session_text_probe_t;

/*
 * Protocol qualification API for COM_QUERY text results. It performs one real
 * simple query on a bounded caller-driven CNet/TLS/auth session, copies exactly
 * one text row into fixed output storage, and tears the session down.
 * Normal Driver CRUD/RAW execution continues to use COM_STMT binary binding.
 */
mysql_session_status_t mysql_session_text_query_probe(
    const mysql_session_config_t *config,
    const uint8_t *sql, size_t sql_size,
    mysql_session_text_probe_t *out,
    mysql_session_error_t *error);

typedef struct mysql_session_command_result_t {
  uint64_t affected_rows;
  uint64_t last_insert_id;
  uint16_t status_flags;
  uint16_t warnings;
} mysql_session_command_result_t;

/*
 * M4 internal command API. SQL and parameter views are borrowed only for the
 * call. Data parameters always use COM_STMT_PREPARE/EXECUTE binary binding;
 * no client-side SQL escaping fallback is permitted.
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
