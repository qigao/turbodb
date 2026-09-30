#ifndef TURBODB_MYSQL_SESSION_TRANSACTION_H
#define TURBODB_MYSQL_SESSION_TRANSACTION_H

#include "session_cursor.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mysql_isolation_t {
  MYSQL_ISOLATION_READ_UNCOMMITTED = 0,
  MYSQL_ISOLATION_READ_COMMITTED,
  MYSQL_ISOLATION_REPEATABLE_READ,
  MYSQL_ISOLATION_SERIALIZABLE
} mysql_isolation_t;

typedef struct mysql_transaction_session_t mysql_transaction_session_t;

/* Returns an owned transaction on success, NULL on failure. Config strings are
 * borrowed only during the call. All subsequent operations use its owner thread.
 * Destroy any active source before commit/rollback/destroy or another command. */
mysql_session_status_t mysql_transaction_session_begin(
    const mysql_session_config_t *config,
    mysql_isolation_t isolation,
    size_t max_command_bytes,
    mysql_transaction_session_t **out_transaction,
    mysql_session_error_t *error);

mysql_session_status_t mysql_transaction_session_execute_prepared(
    mysql_transaction_session_t *transaction,
    const uint8_t *sql, size_t sql_size,
    const mysql_stmt_value_t *parameters, size_t parameter_count,
    mysql_session_command_result_t *out,
    mysql_session_error_t *error);

mysql_session_status_t mysql_transaction_session_open_prepared_source(
    mysql_transaction_session_t *transaction,
    const uint8_t *sql, size_t sql_size,
    const mysql_stmt_value_t *parameters, size_t parameter_count,
    const mysql_session_cursor_limits_t *limits,
    mysql_cursor_source_t *out_source,
    const mysql_column_definition_t **out_columns,
    size_t *out_column_count,
    mysql_session_error_t *error);

mysql_session_status_t mysql_transaction_session_commit(
    mysql_transaction_session_t *transaction,
    mysql_session_error_t *error);

mysql_session_status_t mysql_transaction_session_rollback(
    mysql_transaction_session_t *transaction,
    mysql_session_error_t *error);

mysql_session_status_t mysql_transaction_session_savepoint(
    mysql_transaction_session_t *transaction,
    const uint8_t *name, size_t name_size,
    mysql_session_error_t *error);

mysql_session_status_t mysql_transaction_session_rollback_to_savepoint(
    mysql_transaction_session_t *transaction,
    const uint8_t *name, size_t name_size,
    mysql_session_error_t *error);

mysql_session_status_t mysql_transaction_session_release_savepoint(
    mysql_transaction_session_t *transaction,
    const uint8_t *name, size_t name_size,
    mysql_session_error_t *error);

/*
 * Destroy retains ownership until CNet reaches terminal stop/destroy.
 * If the transaction is still active it attempts a bounded ROLLBACK first.
 */
void mysql_transaction_session_destroy(
    mysql_transaction_session_t *transaction);



#ifdef __cplusplus
}
#endif

#endif
