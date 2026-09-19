#define TINYTEST_NO_MAIN
#include "orm_internal.h"
#include "orm_postgresql.h"
#include <tinytest.h>
#include <cmeta/struct.h>
#include <libpq-fe.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Each case kills only the backend PID of the connection it just created.
 * The controller holds an advisory lock, so a dispatched SELECT cannot finish
 * before termination. No sleeps, persistent tables, or synthetic owner holds. */
static const char *conninfo;
static PGconn *controller;
static orm_connection_t *connection;
static orm_query_t *query, *sibling;
static cflow_publisher rows, pending, second;
static orm_error_t error;
static int target_pid;
static unsigned destroy_calls, execute_calls, next_calls;
static orm_status_t last_native_status;
static orm_backend_ops observed_backend;
static orm_row_cursor_ops observed_cursor;
static void (*native_destroy)(void *);
static orm_status_t (*native_execute)(void *, const orm_query_plan *,
    const orm_limits *, uint64_t *, orm_error_t *);
static orm_status_t (*native_open)(void *, const orm_query_plan *,
    const orm_limits *, orm_row_cursor *, orm_error_t *);
static orm_row_cursor_next_fn native_next;

Struct(pg_loss_row, (int, id));
static const cmeta_type_identity row_identity = CMETA_TYPE_ID_ATOM_INIT("orm.PgLossRow");
static const cmeta_type_traits row_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_desc row_type = {
    .name = "pg_loss_row", .size = sizeof(pg_loss_row),
    .align = _Alignof(pg_loss_row), .kind = CMETA_T_OBJECT,
    .traits = &row_traits, .identity = &row_identity};
static const cmeta_data_field_desc row_fields[] = {
    {"orm.PgLossRow.id", "id", offsetof(pg_loss_row, id), &cmeta_data_int}};
static const cmeta_data_struct_shape row_shape = {
    .layout = StructMeta(pg_loss_row), .fields = row_fields, .field_count = 1u};
static const cmeta_data_desc row_data = {
    .struct_size = offsetof(cmeta_data_desc, shape) + sizeof(((cmeta_data_desc *)0)->shape),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.PgLossRow.data", .display_name = "PgLossRow",
    .kind = CMETA_DATA_STRUCT, .storage_type = &row_type, .shape = &row_shape};

static void drop_publisher(cflow_publisher *publisher) {
  if (!cflow_publisher_valid(publisher)) return;
  cflow_publisher moved = *publisher;
  memset(publisher, 0, sizeof(*publisher));
  cflow_publisher_destroy(&moved);
}
static void drop_query(orm_query_t **value) {
  orm_query_t *moved = *value;
  *value = NULL;
  orm_query_destroy(moved);
}
static void observed_destroy(void *context) {
  ++destroy_calls;
  native_destroy(context);
}
static orm_status_t observed_execute(void *context, const orm_query_plan *plan,
    const orm_limits *limits, uint64_t *affected, orm_error_t *out) {
  ++execute_calls;
  return native_execute(context, plan, limits, affected, out);
}
static orm_row_cursor_step observed_next(void *context, cserde_reader *out) {
  ++next_calls;
  const orm_row_cursor_step step = native_next(context, out);
  last_native_status = step.status;
  return step;
}
static orm_status_t observed_open(void *context, const orm_query_plan *plan,
    const orm_limits *limits, orm_row_cursor *out, orm_error_t *out_error) {
  const orm_status_t status = native_open(context, plan, limits, out, out_error);
  if (status == ORM_STATUS_OK) {
    observed_cursor = *out->ops;
    native_next = observed_cursor.next;
    observed_cursor.next = observed_next;
    out->ops = &observed_cursor;
  }
  return status;
}
static void control(const char *sql) {
  PGresult *result = PQexec(controller, sql);
  const int ok = result != NULL && PQresultStatus(result) == PGRES_TUPLES_OK;
  if (!ok) (void)fprintf(stderr, "controller: %s\n", PQerrorMessage(controller));
  PQclear(result);
  check_true(ok);
}
static void terminate_target(void) {
  char sql[128];
  (void)snprintf(sql, sizeof(sql), "select pg_terminate_backend(%d, 5000)", target_pid);
  PGresult *result = PQexec(controller, sql);
  const int terminated = result != NULL && PQresultStatus(result) == PGRES_TUPLES_OK &&
      PQntuples(result) == 1 && PQnfields(result) == 1 &&
      strcmp(PQgetvalue(result, 0, 0), "t") == 0;
  PQclear(result);
  check_true(terminated);
}
static void open_rows(const char *sql) {
  orm_flow_config_t config;
  check_equal(orm_raw(connection, orm_view(sql), &query, &error), ORM_STATUS_OK);
  orm_flow_config(&config, &row_data);
  check_equal(orm_query_open_flow(query, &config, &rows, &error), ORM_STATUS_OK);
}
static void open_blocked_rows(void) {
  char sql[160];
  (void)snprintf(sql, sizeof(sql), "select pg_advisory_lock(%d::bigint)", target_pid);
  control(sql);
  (void)snprintf(sql, sizeof(sql),
      "select 7::integer as id from pg_advisory_lock(%d::bigint)", target_pid);
  open_rows(sql);
}
static void make_pending(void) {
  check_equal(orm_raw(connection, orm_view("create temporary table pg_loss_unused(id integer)"),
                      &sibling, &error), ORM_STATUS_OK);
  check_equal(orm_query_open_command_flow(sibling, &pending, &error), ORM_STATUS_OK);
}

spec("real PostgreSQL established-session loss") {
  (void)ttest_config__;
  before_each() {
    connection = NULL; controller = NULL; query = sibling = NULL;
    memset(&rows, 0, sizeof(rows)); memset(&pending, 0, sizeof(pending));
    memset(&second, 0, sizeof(second));
    destroy_calls = execute_calls = next_calls = 0u;
    last_native_status = ORM_STATUS_OK;
    orm_error_init(&error);
    controller = PQconnectdb(conninfo);
    check_not_null(controller);
    check_equal((int)PQstatus(controller), (int)CONNECTION_OK);
    orm_config_t config;
    const orm_option_t option = {orm_view("conninfo"), orm_view(conninfo)};
    orm_config(&config); config.driver = orm_view("postgresql");
    config.options = &option; config.option_count = 1u;
    check_equal(orm_postgresql_connect(&config, &connection, &error), ORM_STATUS_OK);
    orm_query_t *pid_query = NULL;
    orm_result_t *pid_result = NULL;
    int64_t pid = 0;
    orm_status_t status = orm_raw(connection, orm_view("select pg_backend_pid()"), &pid_query, &error);
    if (status == ORM_STATUS_OK) status = orm_query_execute(pid_query, &pid_result, &error);
    if (status == ORM_STATUS_OK) status = orm_result_get_int64(pid_result, 0u, 0u, &pid, &error);
    orm_result_destroy(pid_result); orm_query_destroy(pid_query);
    check_equal(status, ORM_STATUS_OK);
    check_true(pid > 0 && pid <= INT32_MAX);
    target_pid = (int)pid;
    check_true(target_pid != PQbackendPID(controller));
    observed_backend = *connection->backend.ops;
    native_destroy = observed_backend.destroy;
    native_execute = observed_backend.execute_command;
    native_open = observed_backend.open_cursor;
    observed_backend.destroy = observed_destroy;
    observed_backend.execute_command = observed_execute;
    observed_backend.open_cursor = observed_open;
    connection->backend.ops = &observed_backend;
  }
  after_each() {
    /* Unlock before draining any cursor, even on an early assertion failure. */
    if (controller != NULL) { PQfinish(controller); controller = NULL; }
    drop_publisher(&second); drop_publisher(&pending); drop_publisher(&rows);
    drop_query(&sibling); drop_query(&query);
    orm_connection_release(connection); connection = NULL;
  }
  it("keeps successful completion and does not poison a healthy connection") {
    pg_loss_row row = {0};
    open_rows("select 7::integer as id");
    check_equal(cflow_publisher_resume(&rows, NULL, &row).kind, CFLOW_STEP_VALUE);
    check_equal(row.id, 7);
    check_equal(cflow_publisher_resume(&rows, NULL, &row).kind, CFLOW_STEP_DONE);
    check_equal(connection->failure, ORM_STATUS_OK);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_BUSY);
    drop_publisher(&rows); drop_query(&query);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
    check_equal(destroy_calls, 1u);
  }
  it("reports terminated dispatched rows as error while retaining their parents") {
    pg_loss_row row = {0};
    open_blocked_rows(); terminate_target();
    cflow_step step = cflow_publisher_resume(&rows, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_equal(last_native_status, ORM_STATUS_CONNECTION_ERROR);
    check_not_null(step.error);
    char diagnostic[ORM_C_ERROR_MESSAGE_CAPACITY];
    (void)snprintf(diagnostic, sizeof(diagnostic), "%s", step.error);
    const unsigned calls = next_calls;
    step = cflow_publisher_resume(&rows, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_equal(strcmp(step.error, diagnostic), 0);
    check_equal(next_calls, calls);
    check_equal(orm_query_close(query, &error), ORM_STATUS_BUSY);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_BUSY);
    check_equal(destroy_calls, 0u);
    drop_publisher(&rows); drop_query(&query);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
    check_equal(destroy_calls, 1u);
  }
  it("blocks sibling admission and an already-created command after row connection loss") {
    pg_loss_row row = {0};
    make_pending(); open_blocked_rows(); terminate_target();
    check_equal(cflow_publisher_resume(&rows, NULL, &row).kind, CFLOW_STEP_ERROR);
    check_equal(connection->failure, ORM_STATUS_CONNECTION_ERROR);
    orm_query_t *rejected = NULL;
    orm_status_t status = orm_raw(connection, orm_view("select 1"), &rejected, &error);
    orm_query_destroy(rejected);
    check_equal(status, ORM_STATUS_INVALID_STATE);
    check_null(rejected);
    check_equal(orm_query_open_command_flow(sibling, &second, &error), ORM_STATUS_INVALID_STATE);
    check_false(cflow_publisher_valid(&second));
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    check_equal(cflow_publisher_resume(&pending, NULL, &result).kind, CFLOW_STEP_ERROR);
    check_equal(execute_calls, 0u);
    check_equal(destroy_calls, 0u);
  }
  it("blocks a second lazy command after the first discovers a dead session") {
    make_pending();
    check_equal(orm_query_open_command_flow(sibling, &second, &error), ORM_STATUS_OK);
    terminate_target();
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    check_equal(cflow_publisher_resume(&pending, NULL, &result).kind, CFLOW_STEP_ERROR);
    check_equal(execute_calls, 1u);
    check_equal(connection->failure, ORM_STATUS_CONNECTION_ERROR);
    check_equal(cflow_publisher_resume(&second, NULL, &result).kind, CFLOW_STEP_ERROR);
    check_equal(execute_calls, 1u);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_BUSY);
  }
  it("does not turn an ordinary SQL rejection into permanent connection failure") {
    pg_loss_row row = {0};
    open_rows("select (1 / 0)::integer as id");
    check_equal(cflow_publisher_resume(&rows, NULL, &row).kind, CFLOW_STEP_ERROR);
    check_equal(last_native_status, ORM_STATUS_SQL_ERROR);
    check_equal(connection->failure, ORM_STATUS_OK);
    drop_publisher(&rows); drop_query(&query);
    open_rows("select 7::integer as id");
    check_equal(cflow_publisher_resume(&rows, NULL, &row).kind, CFLOW_STEP_VALUE);
    check_equal(row.id, 7);
  }
  it("records a dead session discovered by cancel before the first resume") {
    make_pending(); open_blocked_rows(); terminate_target();
    cflow_publisher_cancel(&rows);
    check_equal(next_calls, 0u);
    check_equal(connection->failure, ORM_STATUS_CONNECTION_ERROR);
    check_equal(orm_query_close(query, &error), ORM_STATUS_BUSY);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_BUSY);
    check_equal(destroy_calls, 0u);
    check_equal(orm_query_open_command_flow(sibling, &second, &error), ORM_STATUS_INVALID_STATE);
    check_false(cflow_publisher_valid(&second));
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    check_equal(cflow_publisher_resume(&pending, NULL, &result).kind, CFLOW_STEP_ERROR);
    check_equal(execute_calls, 0u);
    cflow_publisher_cancel(&rows);
    check_equal(connection->failure, ORM_STATUS_CONNECTION_ERROR);
    drop_publisher(&rows); drop_publisher(&pending);
    drop_query(&query); drop_query(&sibling);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
    check_equal(destroy_calls, 1u);
  }
  it("records a dead session drained by destroy without cancel or resume") {
    make_pending(); open_blocked_rows(); terminate_target();
    drop_publisher(&rows);
    check_equal(next_calls, 0u);
    check_equal(connection->failure, ORM_STATUS_CONNECTION_ERROR);
    check_equal(destroy_calls, 0u);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_BUSY);
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    check_equal(cflow_publisher_resume(&pending, NULL, &result).kind, CFLOW_STEP_ERROR);
    check_equal(execute_calls, 0u);
    drop_publisher(&pending); drop_query(&query); drop_query(&sibling);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
    check_equal(destroy_calls, 1u);
  }
  it("does not poison a healthy session when unconsumed rows are cancelled") {
    make_pending(); open_rows("select 7::integer as id");
    cflow_publisher_cancel(&rows);
    check_equal(next_calls, 0u);
    check_equal(connection->failure, ORM_STATUS_OK);
    check_equal(orm_query_close(query, &error), ORM_STATUS_BUSY);
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    check_equal(cflow_publisher_resume(&pending, NULL, &result).kind, CFLOW_STEP_VALUE);
    check_equal(execute_calls, 1u);
  }
  it("does not poison a healthy session when cancellation drains a SQL rejection") {
    open_rows("select (1 / 0)::integer as id");
    cflow_publisher_cancel(&rows);
    check_equal(next_calls, 0u);
    check_equal(connection->failure, ORM_STATUS_OK);
    drop_publisher(&rows); drop_query(&query);
    open_rows("select 7::integer as id");
    pg_loss_row row = {0};
    check_equal(cflow_publisher_resume(&rows, NULL, &row).kind, CFLOW_STEP_VALUE);
    check_equal(row.id, 7);
  }
}

int main(int argc, char **argv) {
  conninfo = getenv("TURBODB_ORM_PGSQL_TEST_CONNINFO");
  if (conninfo == NULL || conninfo[0] == '\0') {
    (void)fprintf(stderr, "Live PostgreSQL session-loss tests require TURBODB_ORM_PGSQL_TEST_CONNINFO\n");
    return EXIT_FAILURE;
  }
  return ttest_main__(argc, argv, TTEST_INVOKE_SPEC_ADAPTER__, TT_USE_COLOR != 0,
                      TT_USE_TAP != 0, TTEST_IS_ATTY__() != 0, TTEST_PRINT_TRACE_DEFAULT__);
}
