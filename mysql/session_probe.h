#ifndef TURBODB_MYSQL_SESSION_PROBE_H
#define TURBODB_MYSQL_SESSION_PROBE_H

#include "session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Private protocol qualification support; not installed as client API. */
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

#ifdef __cplusplus
}
#endif

#endif
