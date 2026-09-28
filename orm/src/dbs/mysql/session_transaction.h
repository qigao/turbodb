#ifndef TURBODB_ORM_MYSQL_SESSION_TRANSACTION_H
#define TURBODB_ORM_MYSQL_SESSION_TRANSACTION_H

#include "session_cursor.h"

#include <orm.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mysql_transaction_session_t mysql_transaction_session_t;

mysql_session_status_t mysql_transaction_session_begin(
    const mysql_session_config_t *config,
    orm_isolation_t isolation,
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

#if defined(ORM_MYSQL_ENABLE_FAULT_INJECTION)
/*
 * Live qualification only: drop the COMMIT acknowledgement path after the
 * COMMIT command has reached CNet send completion. This must never be enabled
 * in the production Driver target.
 */
void mysql_transaction_session_test_drop_commit_ack(
    mysql_transaction_session_t *transaction, int enabled);
unsigned mysql_transaction_session_test_commit_send_count(
    const mysql_transaction_session_t *transaction);
#endif

#ifdef __cplusplus
}
#endif

#endif
