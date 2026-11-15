#ifndef TURBODB_MYSQL_SESSION_CURSOR_H
#define TURBODB_MYSQL_SESSION_CURSOR_H

#include "source.h"
#include "wire/row.h"
#include "session.h"
#include "wire/statement.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mysql_session_cursor_limits_t {
  /* Transaction cancellation drain budget. A raw source does not impose a
   * total row/byte quota; its consumer must stop requesting rows at its limit. */
  uint64_t max_result_rows;
  size_t max_columns;
  size_t max_metadata_bytes;
  size_t max_row_bytes;
  size_t max_command_bytes;
} mysql_session_cursor_limits_t;

/*
 * Opens one prepared result on the existing caller-driven CNet session model.
 * SQL/parameter views are borrowed only until this function returns.
 *
 * On success:
 * - out_source owns the live CNet/MySQL session;
 * - out_columns points into source-owned metadata valid until source destroy;
 * - next() rows are borrowed until the next next/cancel/destroy operation;
 * - caller destroys out_source exactly once, on the calling thread.
 */
mysql_session_status_t mysql_session_open_prepared_source(
    const mysql_session_config_t *config,
    const uint8_t *sql, size_t sql_size,
    const mysql_stmt_value_t *parameters, size_t parameter_count,
    const mysql_session_cursor_limits_t *limits,
    mysql_cursor_source_t *out_source,
    const mysql_column_definition_t **out_columns,
    size_t *out_column_count,
    mysql_session_error_t *error);

#ifdef __cplusplus
}
#endif

#endif
