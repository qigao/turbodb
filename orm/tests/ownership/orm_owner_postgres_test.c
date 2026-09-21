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

/* This fixture uses the actual libpq backend and only session-local tables.
 * Observers forward native calls; they do not supply references or leases. */
static const char *conninfo;
static orm_connection_t *connection;
static orm_transaction_t *transaction;
static orm_query_t *query;
static cflow_publisher first, second;
static orm_error_t error;
static orm_transaction_backend_ops observed_ops;
static void (*native_destroy)(void *);
static orm_status_t (*native_commit)(void *, orm_error_t *);
static orm_status_t (*native_rollback)(void *, orm_error_t *);
static unsigned destroy_calls, commit_calls, rollback_calls;

Struct(pg_owner_row, (int, id));
static const cmeta_type_identity row_identity = CMETA_TYPE_ID_ATOM_INIT("orm.owner.PgRow");
static const cmeta_type_traits row_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_desc row_type = {
    .name = "pg_owner_row", .size = sizeof(pg_owner_row),
    .align = _Alignof(pg_owner_row), .kind = CMETA_T_OBJECT,
    .traits = &row_traits, .identity = &row_identity};
static const cmeta_data_field_desc row_fields[] = {
    {"orm.owner.PgRow.id", "id", offsetof(pg_owner_row, id), &cmeta_data_int}};
static const cmeta_data_struct_shape row_shape = {
    .layout = StructMeta(pg_owner_row), .fields = row_fields, .field_count = 1u};
static const cmeta_data_desc row_data = {
    .struct_size = offsetof(cmeta_data_desc, shape) + sizeof(((cmeta_data_desc *)0)->shape),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.owner.PgRow.data", .display_name = "PgRow",
    .kind = CMETA_DATA_STRUCT, .storage_type = &row_type, .shape = &row_shape};

static void observed_destroy(void *context) {
  ++destroy_calls;
  native_destroy(context);
}
static orm_status_t observed_commit(void *context, orm_error_t *out) {
  ++commit_calls;
  return native_commit(context, out);
}
static orm_status_t observed_rollback(void *context, orm_error_t *out) {
  ++rollback_calls;
  return native_rollback(context, out);
}
static void drop_publisher(cflow_publisher *publisher) {
  if (!cflow_publisher_valid(publisher)) return;
  cflow_publisher moved = *publisher;
  memset(publisher, 0, sizeof(*publisher));
  cflow_publisher_destroy(&moved);
}
static void drop_query(void) {
  orm_query_t *moved = query;
  query = NULL;
  orm_query_destroy(moved);
}
static void drop_transaction(void) {
  orm_transaction_t *moved = transaction;
  transaction = NULL;
  orm_transaction_release(moved);
}
static orm_status_t connect_postgres(orm_connection_t **out) {
  orm_config_t config;
  const orm_option_t option = {orm_view("conninfo"), orm_view(conninfo)};
  orm_config(&config);
  config.driver = orm_view("postgresql");
  config.options = &option;
  config.option_count = 1u;
  return orm_postgresql_connect(&config, out, &error);
}
static orm_status_t execute_sql(const char *sql) {
  orm_query_t *command = NULL;
  orm_result_t *result = NULL;
  orm_status_t status = orm_raw(connection, orm_view(sql), &command, &error);
  if (status == ORM_STATUS_OK)
    status = orm_query_execute(command, &result, &error);
  orm_result_destroy(result);
  orm_query_destroy(command);
  return status;
}
static void check_row_count(int64_t expected) {
  orm_query_t *select = NULL;
  orm_result_t *result = NULL;
  int64_t count = -1;
  orm_status_t status = orm_raw(connection,
      orm_view("select count(*) from pg_temp.orm_owned_rows"), &select, &error);
  if (status == ORM_STATUS_OK) status = orm_query_execute(select, &result, &error);
  if (status == ORM_STATUS_OK)
    status = orm_result_get_int64(result, 0u, 0u, &count, &error);
  orm_result_destroy(result);
  orm_query_destroy(select);
  check_equal(status, ORM_STATUS_OK);
  check_equal(count, expected);
}
static void open_command(cflow_publisher *out) {
  check_equal(orm_query_open_command_flow_in_transaction(
      query, transaction, out, &error), ORM_STATUS_OK);
  check_true(cflow_publisher_valid(out));
}
static void open_rows(const char *sql) {
  orm_flow_config_t config;
  drop_query();
  check_equal(orm_raw(connection, orm_view(sql), &query, &error), ORM_STATUS_OK);
  orm_flow_config(&config, &row_data);
  check_equal(orm_query_open_flow_in_transaction(
      query, transaction, &config, &first, &error), ORM_STATUS_OK);
}
static void execute_first(void) {
  orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
  const cflow_step step = cflow_publisher_resume(&first, NULL, &result);
  check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
  check_equal(result.affected_rows, UINT64_C(1));
}

spec("real PostgreSQL native ownership") {
  (void)ttest_config__;
  before_each() {
    connection = NULL; transaction = NULL; query = NULL;
    memset(&first, 0, sizeof(first)); memset(&second, 0, sizeof(second));
    destroy_calls = commit_calls = rollback_calls = 0u;
    orm_error_init(&error);
    check_equal(connect_postgres(&connection), ORM_STATUS_OK);
    check_equal(execute_sql("create temporary table orm_owned_rows(id integer) "
                            "on commit preserve rows"), ORM_STATUS_OK);
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                      &transaction, &error), ORM_STATUS_OK);
    observed_ops = *transaction->backend.ops;
    native_destroy = observed_ops.destroy;
    native_commit = observed_ops.commit;
    native_rollback = observed_ops.rollback;
    observed_ops.destroy = observed_destroy;
    observed_ops.commit = observed_commit;
    observed_ops.rollback = observed_rollback;
    transaction->backend.ops = &observed_ops;
    check_equal(orm_raw(connection, orm_view("insert into pg_temp.orm_owned_rows values (7)"),
                        &query, &error), ORM_STATUS_OK);
  }
  after_each() {
    drop_publisher(&second); drop_publisher(&first);
    drop_query(); drop_transaction();
    orm_connection_release(connection); connection = NULL;
  }
  it("requires explicit finish before close and keeps the parent until final release") {
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_INVALID_STATE);
    check_equal(destroy_calls, 0u); check_equal(rollback_calls, 0u);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_OK);
    check_equal(destroy_calls, 1u);
    drop_query();
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_BUSY);
    drop_transaction();
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
  }
  it("executes an admitted command after releasing every external parent handle") {
    open_command(&first); drop_transaction(); drop_query();
    orm_connection_release(connection); connection = NULL;
    check_equal(destroy_calls, 0u); check_equal(rollback_calls, 0u);
    execute_first();
    check_equal(destroy_calls, 0u);
    drop_publisher(&first);
    check_equal(destroy_calls, 1u); check_equal(rollback_calls, 1u);
    check_equal(commit_calls, 0u);
  }
  it("rolls back a real write on final release without committing it") {
    open_command(&first); execute_first(); drop_transaction();
    check_equal(rollback_calls, 0u);
    drop_publisher(&first);
    check_equal(rollback_calls, 1u); check_equal(destroy_calls, 1u);
    check_equal(commit_calls, 0u); check_row_count(0);
  }
  it("commits only after the command Publisher is destroyed") {
    open_command(&first); execute_first();
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_BUSY);
    check_equal(commit_calls, 0u);
    drop_publisher(&first);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    check_equal(commit_calls, 1u); check_row_count(1);
  }
  it("keeps the transaction until two unconsumed Publishers are destroyed") {
    open_command(&first); open_command(&second); drop_transaction();
    check_equal(destroy_calls, 0u);
    drop_publisher(&first); check_equal(destroy_calls, 0u);
    drop_publisher(&second);
    check_equal(destroy_calls, 1u); check_equal(rollback_calls, 1u);
  }
  it("decodes an actual libpq row after external parent handles are released") {
    pg_owner_row row = {0};
    open_rows("select 7::integer as id");
    drop_transaction(); drop_query();
    orm_connection_release(connection); connection = NULL;
    check_equal(destroy_calls, 0u);
    const cflow_step step = cflow_publisher_resume(&first, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_VALUE); check_equal(row.id, 7);
    check_equal(destroy_calls, 0u);
    drop_publisher(&first);
    check_equal(destroy_calls, 1u); check_equal(rollback_calls, 1u);
  }
  it("blocks all control operations before an unconsumed row cursor is drained") {
    open_rows("select 7::integer as id");
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_BUSY);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_BUSY);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_BUSY);
    check_equal(orm_transaction_savepoint(transaction, orm_view("held"), &error), ORM_STATUS_BUSY);
    check_equal(orm_transaction_rollback_to_savepoint(transaction, orm_view("held"), &error), ORM_STATUS_BUSY);
    check_equal(orm_transaction_release_savepoint(transaction, orm_view("held"), &error), ORM_STATUS_BUSY);
    check_equal(commit_calls, 0u); check_equal(rollback_calls, 0u);
    drop_publisher(&first);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_OK);
  }
  it("cancellation drains results but does not release the owner lease") {
    open_rows("select generate_series(1, 3)::integer as id");
    /* The current libpq cursor cancels by synchronous drain, not PQcancel. */
    cflow_publisher_cancel(&first);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_BUSY);
    check_equal(rollback_calls, 0u); check_equal(destroy_calls, 0u);
    drop_publisher(&first);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_OK);
    check_row_count(0);
  }
  it("keeps terminal SQL-error Publishers owned until destruction and permits rollback") {
    pg_owner_row row = {0};
    open_rows("select missing_owner_column::integer as id");
    const cflow_step step = cflow_publisher_resume(&first, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_BUSY);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_BUSY);
    drop_publisher(&first);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_OK);
    check_row_count(0);
  }
  it("keeps row decoding failures owned until native results have been drained") {
    pg_owner_row row = {0};
    open_rows("select 'not-an-integer'::text as id");
    const cflow_step step = cflow_publisher_resume(&first, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_ERROR);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_BUSY);
    drop_publisher(&first);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_OK);
    check_row_count(0);
  }
  it("freezes the original query plan for the lifetime of the row Publisher") {
    open_rows("select 7::integer as id");
    check_equal(orm_query_bind(query, orm_i64(9), &error), ORM_STATUS_BUSY);
    check_equal(orm_query_close(query, &error), ORM_STATUS_BUSY);
    drop_publisher(&first);
    check_equal(orm_query_close(query, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_OK);
  }
  it("releases admission after a rejected command Publisher open") {
    check_equal(orm_query_open_command_flow_in_transaction(
        query, transaction, NULL, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    check_equal(commit_calls, 1u); check_row_count(0);
  }
  it("rejects a query from another real connection without retaining either owner") {
    orm_connection_t *other = NULL;
    orm_query_t *other_query = NULL;
    orm_status_t status = connect_postgres(&other);
    if (status == ORM_STATUS_OK)
      status = orm_raw(other, orm_view("insert into absent_owner_table values (1)"),
                       &other_query, &error);
    if (status == ORM_STATUS_OK)
      status = orm_query_open_command_flow_in_transaction(
          other_query, transaction, &first, &error);
    orm_query_destroy(other_query); orm_connection_release(other);
    check_equal(status, ORM_STATUS_INVALID_STATE);
    check_false(cflow_publisher_valid(&first));
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_OK);
  }
}

int main(int argc, char **argv) {
  conninfo = getenv("TURBODB_ORM_PGSQL_TEST_CONNINFO");
  if (conninfo == NULL || conninfo[0] == '\0') {
    (void)fprintf(stderr, "PostgreSQL ownership tests require TURBODB_ORM_PGSQL_TEST_CONNINFO\n");
    return EXIT_FAILURE;
  }
  PGconn *probe = PQconnectdb(conninfo);
  if (probe == NULL || PQstatus(probe) != CONNECTION_OK) {
    (void)fprintf(stderr, "PostgreSQL ownership tests require a reachable real server\n");
    if (probe != NULL) PQfinish(probe);
    return EXIT_FAILURE;
  }
  (void)printf("PostgreSQL server_version_num=%d libpq_version=%d\n",
                PQserverVersion(probe), PQlibVersion());
  PQfinish(probe);
  return ttest_main__(argc, argv, TTEST_INVOKE_SPEC_ADAPTER__, TT_USE_COLOR != 0,
                      TT_USE_TAP != 0, TTEST_IS_ATTY__() != 0, TTEST_PRINT_TRACE_DEFAULT__);
}
