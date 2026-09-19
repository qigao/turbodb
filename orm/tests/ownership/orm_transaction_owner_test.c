#include "orm_internal.h"
#include <tinytest.h>
#include <string.h>

/* Observers delegate to real SQLite operations without supplying any owner
 * reference. A failed lifetime assertion stops before accessing freed state. */
static orm_connection_t *connection;
static orm_transaction_t *transaction;
static orm_query_t *query;
static cflow_publisher first;
static cflow_publisher second;
static orm_error_t error;
static orm_transaction_backend_ops observed_ops;
static void (*native_destroy)(void *);
static orm_status_t (*native_rollback)(void *, orm_error_t *);
static orm_status_t (*native_commit)(void *, orm_error_t *);
static unsigned destroy_calls;
static unsigned rollback_calls;
static unsigned commit_calls;

static void observed_destroy(void *context) {
  ++destroy_calls;
  native_destroy(context);
}
static orm_status_t observed_rollback(void *context, orm_error_t *out_error) {
  ++rollback_calls;
  return native_rollback(context, out_error);
}
static orm_status_t observed_commit(void *context, orm_error_t *out_error) {
  ++commit_calls;
  return native_commit(context, out_error);
}
static void drop_publisher(cflow_publisher *publisher) {
  if (!cflow_publisher_valid(publisher)) return;
  cflow_publisher released = *publisher;
  memset(publisher, 0, sizeof(*publisher));
  cflow_publisher_destroy(&released);
}
static void drop_transaction(void) {
  orm_transaction_t *released = transaction;
  transaction = NULL;
  orm_transaction_destroy(released);
}
static void open_command(cflow_publisher *publisher) {
  check_equal(orm_query_open_command_flow_in_transaction(
                  query, transaction, publisher, &error), ORM_STATUS_OK);
  check_true(cflow_publisher_valid(publisher));
}

spec("real SQLite transaction Publisher ownership") {
  (void)ttest_config__;
  before_each() {
    orm_config_t config;
    orm_option_t filename;
    orm_result_t *result = NULL;
    connection = NULL; transaction = NULL; query = NULL;
    memset(&first, 0, sizeof(first)); memset(&second, 0, sizeof(second));
    destroy_calls = 0u; rollback_calls = 0u; commit_calls = 0u;
    orm_error_init(&error); orm_config(&config);
    filename.keyword = orm_view("filename"); filename.value = orm_view(":memory:");
    config.driver = orm_view("sqlite"); config.options = &filename;
    config.option_count = 1u;
    check_equal(orm_connect(&config, &connection, &error), ORM_STATUS_OK);
    check_equal(orm_raw(connection, orm_view("create table tx_owned(id integer)"),
                        &query, &error), ORM_STATUS_OK);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    orm_result_destroy(result); orm_query_destroy(query); query = NULL;
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                      &transaction, &error), ORM_STATUS_OK);
    observed_ops = *transaction->backend.ops;
    native_destroy = observed_ops.destroy; native_rollback = observed_ops.rollback;
    native_commit = observed_ops.commit;
    observed_ops.destroy = observed_destroy; observed_ops.rollback = observed_rollback;
    observed_ops.commit = observed_commit;
    transaction->backend.ops = &observed_ops;
    check_equal(orm_raw(connection, orm_view("insert into tx_owned values (7)"),
                        &query, &error), ORM_STATUS_OK);
  }
  after_each() {
    /* Old-core lazy command destroy frees its state without calling the native
     * transaction; never resume after a failed retention check. */
    drop_publisher(&second); drop_publisher(&first);
    orm_query_destroy(query); query = NULL;
    drop_transaction(); orm_disconnect(connection); connection = NULL;
  }
  it("normally releases the transaction once after its Publisher") {
    open_command(&first); drop_publisher(&first);
    check_equal(destroy_calls, 0u);
    drop_transaction();
    check_equal(rollback_calls, 1u); check_equal(destroy_calls, 1u);
    check_equal(commit_calls, 0u);
  }
  it("retains a released transaction until its unconsumed Publisher is destroyed") {
    open_command(&first); drop_transaction();
    check_equal(destroy_calls, 0u); check_equal(rollback_calls, 0u);
    drop_publisher(&first);
    check_equal(rollback_calls, 1u); check_equal(destroy_calls, 1u);
    check_equal(commit_calls, 0u);
  }
  it("can execute the admitted command after the external transaction is released") {
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    open_command(&first); drop_transaction();
    check_equal(destroy_calls, 0u);
    const cflow_step step = cflow_publisher_resume(&first, NULL, &result);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(result.affected_rows, (uint64_t)1u);
    check_equal(rollback_calls, 0u); check_equal(destroy_calls, 0u);
    drop_publisher(&first);
    check_equal(rollback_calls, 1u); check_equal(destroy_calls, 1u);
    check_equal(commit_calls, 0u);
  }
  it("does not drop the transaction lease on cancellation") {
    open_command(&first); cflow_publisher_cancel(&first); drop_transaction();
    check_equal(destroy_calls, 0u); check_equal(rollback_calls, 0u);
    drop_publisher(&first);
    check_equal(rollback_calls, 1u); check_equal(destroy_calls, 1u);
  }
  it("retains a transaction until both Publishers have been destroyed") {
    open_command(&first); open_command(&second); drop_transaction();
    check_equal(destroy_calls, 0u);
    drop_publisher(&first); check_equal(destroy_calls, 0u);
    drop_publisher(&second);
    check_equal(rollback_calls, 1u); check_equal(destroy_calls, 1u);
  }
  it("rejects commit before native execution while an unconsumed Publisher exists") {
    open_command(&first);
    const orm_status_t status = orm_transaction_commit(transaction, &error);
    check_equal(status, ORM_STATUS_BUSY); check_equal(commit_calls, 0u);
    check_equal(transaction->state, ORM_TRANSACTION_ACTIVE);
    drop_publisher(&first);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    check_equal(commit_calls, 1u);
    drop_transaction(); check_equal(rollback_calls, 0u); check_equal(destroy_calls, 1u);
  }
  it("rejects rollback before native execution while an unconsumed Publisher exists") {
    open_command(&first);
    const orm_status_t status = orm_transaction_rollback(transaction, &error);
    check_equal(status, ORM_STATUS_BUSY); check_equal(rollback_calls, 0u);
    check_equal(transaction->state, ORM_TRANSACTION_ACTIVE);
    drop_publisher(&first);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_OK);
    check_equal(rollback_calls, 1u);
    drop_transaction(); check_equal(rollback_calls, 1u); check_equal(destroy_calls, 1u);
  }
  it("keeps commit blocked after cancel until the Publisher handle is consumed") {
    open_command(&first); cflow_publisher_cancel(&first);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_BUSY);
    check_equal(commit_calls, 0u);
    drop_publisher(&first);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
  }
  it("does not retain a transaction after rejected Publisher creation") {
    check_equal(orm_query_open_command_flow_in_transaction(
                    query, transaction, NULL, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    drop_transaction();
    check_equal(destroy_calls, 1u); check_equal(rollback_calls, 0u);
  }
}
