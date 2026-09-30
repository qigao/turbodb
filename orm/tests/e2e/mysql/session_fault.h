#ifndef TURBODB_MYSQL_SESSION_FAULT_TEST_H
#define TURBODB_MYSQL_SESSION_FAULT_TEST_H

#include "session_transaction.h"

#if defined(MYSQL_ENABLE_FAULT_INJECTION)
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

#endif
