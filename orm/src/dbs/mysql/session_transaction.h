#ifndef TURBODB_ORM_MYSQL_SESSION_TRANSACTION_H
#define TURBODB_ORM_MYSQL_SESSION_TRANSACTION_H

#include "session.h"

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

mysql_session_status_t mysql_transaction_session_commit(
    mysql_transaction_session_t *transaction,
    mysql_session_error_t *error);

mysql_session_status_t mysql_transaction_session_rollback(
    mysql_transaction_session_t *transaction,
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
