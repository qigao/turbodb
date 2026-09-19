#include "orm_internal.h"
#include <tinytest.h>
#include <cmeta/struct.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Fault injection only changes a native return at the real SQLite boundary.
 * The core, query plan, cursor allocation and owner graph are not simulated. */
static orm_connection_t *connection;
static orm_query_t *rows, *command, *extra;
static orm_transaction_t *transaction;
static cflow_publisher first, second;
static orm_error_t error;
static orm_backend_ops backend_ops;
static orm_row_cursor_ops cursor_ops;
static orm_status_t (*native_open)(void *, const orm_query_plan *,
    const orm_limits *, orm_row_cursor *, orm_error_t *);
static orm_status_t (*native_execute)(void *, const orm_query_plan *,
    const orm_limits *, uint64_t *, orm_error_t *);
static void (*native_disconnect)(void *);
static orm_row_cursor_next_fn native_next;
static orm_row_cursor_cancel_fn native_cancel;
static orm_row_cursor_destroy_fn native_cursor_destroy;
static orm_status_t execute_error, next_error, open_error;
static unsigned execute_calls, next_calls, disconnect_calls;
static int overwrite_diagnostic;
static int use_checked_cancel;
static unsigned checked_cancel_calls;
static orm_status_t cancel_error;
static orm_transaction_backend_ops original_transaction_ops, failing_transaction_ops;
static orm_status_t failed_control(void *context, orm_error_t *out) {
  (void)context;
  orm_error_set(out, ORM_STATUS_CONNECTION_ERROR, "native control connection lost");
  return ORM_STATUS_CONNECTION_ERROR;
}
static orm_status_t failed_begin(void *context, orm_isolation_t isolation,
    orm_transaction_backend *out, orm_error_t *out_error) {
  (void)context; (void)isolation; (void)out;
  return failed_control(NULL, out_error);
}
static char diagnostic[ORM_C_ERROR_MESSAGE_CAPACITY];

Struct(failure_row, (int, id));
static const cmeta_type_identity row_identity = CMETA_TYPE_ID_ATOM_INIT("orm.failure.Row");
static const cmeta_type_traits row_traits = {
  .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_desc row_type = {
  .name = "failure_row", .size = sizeof(failure_row), .align = _Alignof(failure_row),
  .kind = CMETA_T_OBJECT, .traits = &row_traits, .identity = &row_identity};
static const cmeta_data_field_desc row_fields[] = {
  {"orm.failure.Row.id", "id", offsetof(failure_row, id), &cmeta_data_int}};
static const cmeta_data_struct_shape row_shape = {
  .layout = StructMeta(failure_row), .fields = row_fields, .field_count = 1u};
static const cmeta_data_desc row_data = {
  .struct_size = offsetof(cmeta_data_desc, shape) + sizeof(((cmeta_data_desc *)0)->shape),
  .abi_version = CMETA_DATA_DESC_ABI_VERSION, .stable_id = "orm.failure.Row.data",
  .display_name = "Row", .kind = CMETA_DATA_STRUCT,
  .storage_type = &row_type, .shape = &row_shape};

static void drop(cflow_publisher *publisher) {
  if (cflow_publisher_valid(publisher)) cflow_publisher_destroy(publisher);
  memset(publisher, 0, sizeof(*publisher));
}
static void observed_disconnect(void *context) {
  ++disconnect_calls;
  native_disconnect(context);
}
static orm_status_t observed_execute(void *context, const orm_query_plan *plan,
    const orm_limits *limits, uint64_t *affected, orm_error_t *out) {
  ++execute_calls;
  if (execute_error != ORM_STATUS_OK) {
    orm_error_set(out, execute_error, "native command connection lost");
    return execute_error;
  }
  return native_execute(context, plan, limits, affected, out);
}
static orm_row_cursor_step observed_next(void *context, cserde_reader *out) {
  ++next_calls;
  if (next_error != ORM_STATUS_OK) {
    orm_row_cursor_step step = ORM_ROW_CURSOR_STEP_INIT;
    step.kind = ORM_ROW_CURSOR_ERROR;
    step.status = next_error;
    step.message = diagnostic;
    return step;
  }
  return native_next(context, out);
}
static void observed_cancel(void *context) {
  native_cancel(context);
  if (overwrite_diagnostic)
    (void)snprintf(diagnostic, sizeof(diagnostic), "%s", "cancel replaced borrowed text");
}
static orm_status_t observed_checked_cancel(void *context, orm_error_t *out) {
  ++checked_cancel_calls;
  native_cancel(context);
  orm_error_set(out, cancel_error, "native drain diagnostic");
  return cancel_error;
}
static void observed_cursor_destroy(void *context) {
  native_cursor_destroy(context);
}
static orm_status_t observed_open(void *context, const orm_query_plan *plan,
    const orm_limits *limits, orm_row_cursor *out, orm_error_t *out_error) {
  if (open_error != ORM_STATUS_OK) {
    orm_error_set(out_error, open_error, "native open connection lost");
    return open_error;
  }
  orm_status_t status = native_open(context, plan, limits, out, out_error);
  if (status == ORM_STATUS_OK) {
    cursor_ops = *out->ops;
    native_next = cursor_ops.next;
    native_cancel = cursor_ops.cancel;
    native_cursor_destroy = cursor_ops.destroy;
    cursor_ops.next = observed_next;
    cursor_ops.cancel = observed_cancel;
    cursor_ops.destroy = observed_cursor_destroy;
    out->ops = &cursor_ops;
    if (use_checked_cancel) out->cancel_checked = observed_checked_cancel;
  }
  return status;
}
static orm_status_t open_rows(cflow_publisher *out) {
  orm_flow_config_t config;
  orm_flow_config(&config, &row_data);
  return orm_query_open_flow(rows, &config, out, &error);
}
static cflow_step run_command(cflow_publisher *publisher) {
  orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
  return cflow_publisher_resume(publisher, NULL, &result);
}
static void fail_command(void) {
  check_equal(orm_query_open_command_flow(command, &first, &error), ORM_STATUS_OK);
  execute_error = ORM_STATUS_CONNECTION_ERROR;
  check_equal(run_command(&first).kind, CFLOW_STEP_ERROR);
}

spec("native connection failure propagation") {
  (void)ttest_config__;
  before_each() {
    orm_config_t config;
    const orm_option_t filename = {orm_view("filename"), orm_view(":memory:")};
    connection = NULL; rows = command = extra = NULL; transaction = NULL;
    memset(&first, 0, sizeof(first)); memset(&second, 0, sizeof(second));
    execute_error = next_error = open_error = ORM_STATUS_OK;
    use_checked_cancel = 0; checked_cancel_calls = 0u; cancel_error = ORM_STATUS_OK;
    execute_calls = next_calls = disconnect_calls = 0u; overwrite_diagnostic = 0;
    (void)snprintf(diagnostic, sizeof(diagnostic), "%s", "original native connection loss");
    orm_error_init(&error); orm_config(&config); config.driver = orm_view("sqlite");
    config.options = &filename; config.option_count = 1u;
    check_equal(orm_connect(&config, &connection, &error), ORM_STATUS_OK);
    backend_ops = *connection->backend.ops;
    native_open = backend_ops.open_cursor; native_execute = backend_ops.execute_command;
    native_disconnect = backend_ops.destroy;
    backend_ops.open_cursor = observed_open; backend_ops.execute_command = observed_execute;
    backend_ops.destroy = observed_disconnect; connection->backend.ops = &backend_ops;
    check_equal(orm_raw(connection, orm_view("select 7 as id"), &rows, &error), ORM_STATUS_OK);
    check_equal(orm_raw(connection, orm_view("create table failure_probe(id integer)"),
                        &command, &error), ORM_STATUS_OK);
  }
  after_each() {
    drop(&second); drop(&first);
    orm_query_destroy(extra); orm_query_destroy(rows); orm_query_destroy(command);
    if (transaction != NULL && transaction->backend.ops == &failing_transaction_ops)
      transaction->backend.ops = &original_transaction_ops;
    orm_transaction_release(transaction); orm_connection_release(connection);
  }
  it("blocks new queries after a native command connection failure") {
    fail_command();
    check_equal(orm_raw(connection, orm_view("select 1"), &extra, &error), ORM_STATUS_INVALID_STATE);
    check_null(extra);
  }
  it("blocks new transactions after a native command connection failure") {
    fail_command();
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                     &transaction, &error), ORM_STATUS_INVALID_STATE);
    check_null(transaction);
  }
  it("blocks existing query Publisher admission after connection failure") {
    fail_command();
    check_equal(open_rows(&second), ORM_STATUS_INVALID_STATE);
    check_false(cflow_publisher_valid(&second));
  }
  it("does not execute a previously admitted lazy sibling after failure") {
    check_equal(orm_query_open_command_flow(command, &second, &error), ORM_STATUS_OK);
    fail_command(); execute_error = ORM_STATUS_OK;
    check_equal(run_command(&second).kind, CFLOW_STEP_ERROR);
    check_equal(execute_calls, 1u);
  }
  it("propagates a row cursor connection error to the real connection owner") {
    failure_row row = {0};
    check_equal(open_rows(&first), ORM_STATUS_OK);
    next_error = ORM_STATUS_CONNECTION_ERROR;
    check_equal(cflow_publisher_resume(&first, NULL, &row).kind, CFLOW_STEP_ERROR);
    check_equal(orm_raw(connection, orm_view("select 1"), &extra, &error), ORM_STATUS_INVALID_STATE);
  }
  it("copies borrowed native diagnostics before cancellation invalidates them") {
    failure_row row = {0};
    check_equal(open_rows(&first), ORM_STATUS_OK);
    next_error = ORM_STATUS_CONNECTION_ERROR; overwrite_diagnostic = 1;
    const cflow_step step = cflow_publisher_resume(&first, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_equal(strcmp(step.error, "original native connection loss"), 0);
  }
  it("rejects an already open sibling without another native next call") {
    failure_row row = {0};
    check_equal(open_rows(&first), ORM_STATUS_OK);
    check_equal(open_rows(&second), ORM_STATUS_OK);
    next_error = ORM_STATUS_CONNECTION_ERROR;
    check_equal(cflow_publisher_resume(&first, NULL, &row).kind, CFLOW_STEP_ERROR);
    next_error = ORM_STATUS_OK;
    check_equal(cflow_publisher_resume(&second, NULL, &row).kind, CFLOW_STEP_ERROR);
    check_equal(next_calls, 1u);
  }
  it("records failed native row-open before releasing its temporary hold") {
    open_error = ORM_STATUS_CONNECTION_ERROR;
    check_equal(open_rows(&first), ORM_STATUS_CONNECTION_ERROR);
    check_false(cflow_publisher_valid(&first));
    check_equal(orm_raw(connection, orm_view("select 1"), &extra, &error), ORM_STATUS_INVALID_STATE);
  }
  it("keeps failed and pending Publishers alive after external releases") {
    check_equal(orm_query_open_command_flow(command, &first, &error), ORM_STATUS_OK);
    check_equal(open_rows(&second), ORM_STATUS_OK);
    orm_query_destroy(command); command = NULL; orm_query_destroy(rows); rows = NULL;
    orm_connection_release(connection); connection = NULL;
    execute_error = ORM_STATUS_CONNECTION_ERROR;
    check_equal(run_command(&first).kind, CFLOW_STEP_ERROR);
    check_equal(disconnect_calls, 0u);
    drop(&first); check_equal(disconnect_calls, 0u);
    drop(&second); check_equal(disconnect_calls, 1u);
  }
  it("does not poison a connection for an ordinary SQL rejection") {
    check_equal(orm_query_open_command_flow(command, &first, &error), ORM_STATUS_OK);
    execute_error = ORM_STATUS_SQL_ERROR;
    check_equal(run_command(&first).kind, CFLOW_STEP_ERROR);
    check_equal(orm_raw(connection, orm_view("select 1"), &extra, &error), ORM_STATUS_OK);
    check_equal(open_rows(&second), ORM_STATUS_OK);
  }
  it("does not poison a connection for a row type error") {
    failure_row row = {0};
    check_equal(open_rows(&first), ORM_STATUS_OK); next_error = ORM_STATUS_TYPE_ERROR;
    check_equal(cflow_publisher_resume(&first, NULL, &row).kind, CFLOW_STEP_ERROR);
    check_equal(orm_raw(connection, orm_view("select 1"), &extra, &error), ORM_STATUS_OK);
  }
  it("keeps checked-close BUSY until error Publishers release their holds") {
    fail_command(); check_equal(orm_connection_close(connection, &error), ORM_STATUS_BUSY);
    orm_query_destroy(rows); rows = NULL; orm_query_destroy(command); command = NULL;
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_BUSY);
    drop(&first);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
    check_equal(disconnect_calls, 1u);
  }
  it("invalidates the connection when native transaction begin fails") {
    backend_ops.begin_transaction = failed_begin;
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                     &transaction, &error), ORM_STATUS_CONNECTION_ERROR);
    check_null(transaction);
    check_equal(orm_raw(connection, orm_view("select 1"), &extra, &error), ORM_STATUS_INVALID_STATE);
  }
  it("invalidates the connection on a native rollback connection error") {
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                     &transaction, &error), ORM_STATUS_OK);
    original_transaction_ops = *transaction->backend.ops;
    failing_transaction_ops = original_transaction_ops;
    failing_transaction_ops.rollback = failed_control;
    transaction->backend.ops = &failing_transaction_ops;
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_CONNECTION_ERROR);
    check_equal(orm_raw(connection, orm_view("select 1"), &extra, &error), ORM_STATUS_INVALID_STATE);
  }
  it("does not commit an existing transaction after its connection failed") {
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                     &transaction, &error), ORM_STATUS_OK);
    fail_command();
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_INVALID_STATE);
  }
  it("reports checked cancellation before parent release and does not cancel twice") {
    use_checked_cancel = 1; cancel_error = ORM_STATUS_CONNECTION_ERROR;
    check_equal(open_rows(&first), ORM_STATUS_OK);
    cflow_publisher_cancel(&first);
    check_equal(connection->failure, ORM_STATUS_CONNECTION_ERROR);
    check_equal(next_calls, 0u);
    check_equal(orm_query_close(rows, &error), ORM_STATUS_BUSY);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_BUSY);
    check_equal(disconnect_calls, 0u);
    check_equal(orm_query_open_command_flow(command, &second, &error), ORM_STATUS_INVALID_STATE);
    cflow_publisher_cancel(&first); drop(&first);
    check_equal(checked_cancel_calls, 1u);
  }
  it("reports cancellation failure when an unconsumed Publisher is destroyed") {
    use_checked_cancel = 1; cancel_error = ORM_STATUS_CONNECTION_ERROR;
    check_equal(open_rows(&first), ORM_STATUS_OK);
    drop(&first);
    check_equal(connection->failure, ORM_STATUS_CONNECTION_ERROR);
    check_equal(next_calls, 0u);
    check_equal(disconnect_calls, 0u);
    check_equal(checked_cancel_calls, 1u);
  }
  it("preserves the primary query diagnostic while reporting a separate drain failure") {
    use_checked_cancel = 1; cancel_error = ORM_STATUS_CONNECTION_ERROR;
    check_equal(open_rows(&first), ORM_STATUS_OK);
    next_error = ORM_STATUS_SQL_ERROR;
    failure_row row = {0};
    const cflow_step step = cflow_publisher_resume(&first, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_equal(strcmp(step.error, "original native connection loss"), 0);
    check_equal(connection->failure, ORM_STATUS_CONNECTION_ERROR);
    drop(&first);
    check_equal(checked_cancel_calls, 1u);
  }
  it("does not invalidate the connection for an ordinary checked cancellation error") {
    use_checked_cancel = 1; cancel_error = ORM_STATUS_SQL_ERROR;
    check_equal(open_rows(&first), ORM_STATUS_OK);
    cflow_publisher_cancel(&first);
    check_equal(connection->failure, ORM_STATUS_OK);
    check_equal(orm_raw(connection, orm_view("select 1"), &extra, &error), ORM_STATUS_OK);
    drop(&first);
    check_equal(checked_cancel_calls, 1u);
  }

}
