#include <orm.h>
#include <tinytest.h>

static orm_connection_t *open_sqlite(orm_error_t *error) {
  orm_config_t config;
  orm_option_t filename;
  orm_connection_t *connection = NULL;
  orm_config(&config);
  filename.keyword = orm_view("filename");
  filename.value = orm_view(":memory:");
  config.driver = orm_view("sqlite");
  config.options = &filename;
  config.option_count = 1u;
  check_equal(orm_connect(&config, &connection, error), ORM_STATUS_OK);
  check_not_null(connection);
  return connection;
}

static void execute_command(orm_connection_t *connection, const char *sql,
                            orm_error_t *error) {
  orm_query_t *query = NULL;
  cflow_publisher publisher = {0};
  orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
  check_equal(orm_raw(connection, orm_view(sql), &query, error), ORM_STATUS_OK);
  check_equal(orm_query_open_command_flow(query, &publisher, error), ORM_STATUS_OK);
  check_equal(cflow_publisher_resume(&publisher, NULL, &result).kind,
              CFLOW_STEP_VALUE_AND_DONE);
  cflow_publisher_destroy(&publisher);
  orm_query_destroy(query);
}

spec("ORM public retained ownership") {
  it("returns BUSY from checked close until an admitted Publisher leaves") {
    orm_error_t error;
    orm_connection_t *connection;
    orm_query_t *query = NULL;
    cflow_publisher publisher = {0};
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;

    orm_error_init(&error);
    connection = open_sqlite(&error);
    check_equal(orm_raw(connection, orm_view("create table owner_gate(id integer)"),
                        &query, &error), ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow(query, &publisher, &error),
                ORM_STATUS_OK);

    check_equal(orm_query_close(query, &error), ORM_STATUS_BUSY);
    check_equal(error.status, ORM_STATUS_BUSY);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_BUSY);
    check_equal(error.status, ORM_STATUS_BUSY);

    check_equal(cflow_publisher_resume(&publisher, NULL, &result).kind,
                CFLOW_STEP_VALUE_AND_DONE);
    cflow_publisher_destroy(&publisher);

    check_equal(orm_query_close(query, &error), ORM_STATUS_OK);
    check_equal(orm_query_close(query, &error), ORM_STATUS_OK);
    orm_query_release(query);

    check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
    orm_connection_release(connection);
  }

  it("keeps a Publisher alive after legacy query destroy releases its caller hold") {
    orm_error_t error;
    orm_connection_t *connection;
    orm_query_t *query = NULL;
    cflow_publisher publisher = {0};
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;

    orm_error_init(&error);
    connection = open_sqlite(&error);
    check_equal(orm_raw(connection, orm_view("create table early_destroy(id integer)"),
                        &query, &error), ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow(query, &publisher, &error),
                ORM_STATUS_OK);

    orm_query_destroy(query);
    query = NULL;

    check_equal(cflow_publisher_resume(&publisher, NULL, &result).kind,
                CFLOW_STEP_VALUE_AND_DONE);
    cflow_publisher_destroy(&publisher);

    check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
    orm_connection_release(connection);
  }

  it("retains a connection across legacy disconnect and releases it explicitly") {
    orm_error_t error;
    orm_connection_t *connection;
    orm_query_t *query = NULL;

    orm_error_init(&error);
    connection = open_sqlite(&error);
    check_equal(orm_connection_retain(connection), ORM_STATUS_OK);
    orm_disconnect(connection);

    check_equal(orm_raw(connection, orm_view("select 1"), &query, &error),
                ORM_STATUS_OK);
    orm_query_destroy(query);

    check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
    orm_connection_release(connection);
  }

  it("keeps transaction checked close BUSY through an active Publisher") {
    orm_error_t error;
    orm_connection_t *connection;
    orm_transaction_t *transaction = NULL;
    orm_query_t *query = NULL;
    cflow_publisher publisher = {0};
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;

    orm_error_init(&error);
    connection = open_sqlite(&error);
    execute_command(connection, "create table tx_owner(id integer)", &error);
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                      &transaction, &error), ORM_STATUS_OK);
    check_equal(orm_raw(connection, orm_view("insert into tx_owner values(7)"),
                        &query, &error), ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow_in_transaction(
                    query, transaction, &publisher, &error),
                ORM_STATUS_OK);

    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_BUSY);
    check_equal(cflow_publisher_resume(&publisher, NULL, &result).kind,
                CFLOW_STEP_VALUE_AND_DONE);
    cflow_publisher_destroy(&publisher);
    orm_query_destroy(query);

    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_OK);
    check_equal(orm_transaction_close(transaction, &error), ORM_STATUS_OK);
    orm_transaction_release(transaction);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
    orm_connection_release(connection);
  }

  it("reports invalid public retain and close arguments without aborting") {
    orm_error_t error;
    orm_error_init(&error);
    check_equal(orm_connection_retain(NULL), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(orm_query_retain(NULL), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(orm_transaction_retain(NULL), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(orm_connection_close(NULL, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(error.status, ORM_STATUS_INVALID_ARGUMENT);
    check_equal(orm_query_close(NULL, &error), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(orm_transaction_close(NULL, &error), ORM_STATUS_INVALID_ARGUMENT);
  }
}
