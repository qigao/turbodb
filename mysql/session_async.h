#ifndef TURBODB_MYSQL_SESSION_ASYNC_H
#define TURBODB_MYSQL_SESSION_ASYNC_H
#include "session_cursor.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct mysql_async_source { void *context; } mysql_async_source;
typedef enum mysql_async_step_kind {
  MYSQL_ASYNC_WAIT, MYSQL_ASYNC_METADATA, MYSQL_ASYNC_ROW,
  MYSQL_ASYNC_DONE, MYSQL_ASYNC_ERROR
} mysql_async_step_kind;
typedef struct mysql_async_step {
  mysql_async_step_kind kind;
  mysql_session_status_t status;
  const char *message;
  const mysql_column_definition_t *columns;
  size_t column_count;
  const uint8_t *row;
  size_t row_size;
} mysql_async_step;

/* Single calling thread. Start admits a CNet connection without waiting for the
 * server. Config, SQL and parameter storage are borrowed until METADATA or
 * destroy. Each next performs at most one poll(0); WAIT is not completion.
 * Column metadata lasts through destroy; row/error views expire at next/destroy.
 * The caller enforces its total query deadline. Close/destroy are control-plane
 * operations outside callbacks. Close uses config.timeout_ms; on failure the
 * handle and borrowed inputs remain owned and close may be retried. Destroy
 * requires close to succeed, terminating on an undrained transport rather than
 * releasing callback storage while CNet still owns it. */
mysql_session_status_t mysql_session_start_async_source(
    const mysql_session_config_t *config, const uint8_t *sql, size_t sql_size,
    const mysql_stmt_value_t *parameters, size_t parameter_count,
    const mysql_session_cursor_limits_t *limits, mysql_async_source *out,
    mysql_session_error_t *error);
mysql_async_step mysql_session_async_next(mysql_async_source *source);
void mysql_session_async_cancel(mysql_async_source *source);
mysql_session_status_t mysql_session_async_close(mysql_async_source *source,
                                                mysql_session_error_t *error);
void mysql_session_async_destroy(mysql_async_source *source);

#ifdef __cplusplus
}
#endif
#endif
