#include <tidessql/tidessql.h>
#include <tinytest.h>
#include <stdlib.h>

static tdsql_database *database;
static tdsql_connection *first, *second;
static tdsql_result *result;
static tdsql_transaction *transaction;
static turbodb_error_t error;
static char *directory;
enum { SESSION_CAPACITY = 2, ORIGINAL_SCORE = 10, CHANGED_SCORE = 25 };

static turbodb_status_t command(tdsql_connection *connection, const char *sql) {
  const tdsql_request input = tdsql_request_default(turbodb_view(sql));
  uint64_t affected = 0;
  return tdsql_connection_execute(connection, &input, &affected, &error);
}
static void query(tdsql_connection *connection, const char *sql) {
  const tdsql_request input = tdsql_request_default(turbodb_view(sql));
  check_null(result);
  const turbodb_status_t status = tdsql_connection_query(connection, &input, &result, &error);
  if (status != TURBODB_STATUS_OK) info("SQL: %s; error: %s", sql, error.message);
  check_equal(status, TURBODB_STATUS_OK);
  check_not_null(result);
}
static void close_result(void) {
  check_equal(tdsql_result_destroy_checked(result, &error), TURBODB_STATUS_OK);
  result = NULL;
}
static void scalar_is(tdsql_connection *connection, const char *sql, int64_t expected) {
  query(connection, sql);
  tdsql_row row = {0};
  check_equal(tdsql_result_next(result, &row, &error), TURBODB_STATUS_OK);
  check_equal(row.state, TDSQL_ROW); check_equal(row.count, 1u);
  check_equal(row.values[0].kind, TURBODB_VALUE_INT64);
  check_equal(row.values[0].data.int64_value, expected);
  check_equal(tdsql_result_next(result, &row, &error), TURBODB_STATUS_OK);
  check_equal(row.state, TDSQL_DONE);
  close_result();
}
static turbodb_status_t open_database(bool initialize, const char *capacity,
    tdsql_database **out) {
  const turbodb_option_t options[] = {
    {turbodb_view("path"), turbodb_view(directory)},
    {turbodb_view("column_family"), turbodb_view("shared")},
    {turbodb_view("sql_initialize"), turbodb_view(initialize ? "true" : "false")},
    {turbodb_view("sql_max_connections"), turbodb_view(capacity)}
  };
  tdsql_config config = tdsql_config_default();
  config.options = options; config.option_count = sizeof(options)/sizeof(options[0]);
  return tdsql_database_open(&config, out, &error);
}
static void connect_session(tdsql_connection **out) {
  check_equal(tdsql_database_connect(database, out, &error), TURBODB_STATUS_OK);
  check_not_null(*out);
}
static void close_session(tdsql_connection **connection) {
  check_equal(tdsql_connection_close(*connection, &error), TURBODB_STATUS_OK);
  *connection = NULL;
}

spec("TidesSQL shared database and independent sessions") {
  before_each() {
    database = NULL; first = NULL; second = NULL; result = NULL; transaction = NULL;
    turbodb_error_init(&error);
    directory = tt_make_temp_dir("tidessql-shared"); check_not_null(directory);
    check_equal(open_database(true, "2", &database), TURBODB_STATUS_OK);
    connect_session(&first); connect_session(&second);
    check_equal(command(first, "CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)"), TURBODB_STATUS_OK);
    check_equal(command(first, "INSERT INTO items VALUES(1,10)"), TURBODB_STATUS_OK);
  }
  after_each() {
    close_result();
    check_equal(tdsql_transaction_release_checked(transaction, &error), TURBODB_STATUS_OK);
    transaction = NULL;
    close_session(&first); close_session(&second);
    check_equal(tdsql_database_close(database, &error), TURBODB_STATUS_OK); database = NULL;
    check_equal(tt_remove_tree(directory), 0); free(directory); directory = NULL;
  }
  it("shares committed storage while retaining independent transaction snapshots") {
    check_equal(command(first, "BEGIN"), TURBODB_STATUS_OK);
    check_equal(command(first, "UPDATE items SET score=25 WHERE id=1"), TURBODB_STATUS_OK);
    scalar_is(first, "SELECT score FROM items WHERE id=1", CHANGED_SCORE);
    scalar_is(second, "SELECT score FROM items WHERE id=1", ORIGINAL_SCORE);
    check_equal(command(first, "COMMIT"), TURBODB_STATUS_OK);
    scalar_is(second, "SELECT score FROM items WHERE id=1", CHANGED_SCORE);
  }
  it("keeps autocommit and session access defaults independent") {
    check_equal(command(first, "SET autocommit=OFF"), TURBODB_STATUS_OK);
    check_equal(command(first, "SET SESSION transaction_read_only=ON"), TURBODB_STATUS_OK);
    scalar_is(first, "SELECT @@autocommit", 0);
    scalar_is(first, "SELECT @@transaction_read_only", 1);
    scalar_is(second, "SELECT @@autocommit", 1);
    scalar_is(second, "SELECT @@transaction_read_only", 0);
    check_equal(command(first, "UPDATE items SET score=25 WHERE id=1"), TURBODB_STATUS_SQL_ERROR);
    check_equal(command(first, "ROLLBACK"), TURBODB_STATUS_OK);
    check_equal(command(second, "UPDATE items SET score=25 WHERE id=1"), TURBODB_STATUS_OK);
  }
  it("rolls back a closed SQL session without closing storage or its sibling") {
    check_equal(command(first, "BEGIN"), TURBODB_STATUS_OK);
    check_equal(command(first, "UPDATE items SET score=25 WHERE id=1"), TURBODB_STATUS_OK);
    close_session(&first);
    scalar_is(second, "SELECT score FROM items WHERE id=1", ORIGINAL_SCORE);
    connect_session(&first);
    scalar_is(first, "SELECT @@autocommit", 1);
    check_equal(command(first, "UPDATE items SET score=25 WHERE id=1"), TURBODB_STATUS_OK);
    scalar_is(second, "SELECT score FROM items WHERE id=1", CHANGED_SCORE);
  }
  it("rejects full admission and recovers the slot only after session release") {
    tdsql_connection *extra = first;
    check_equal(tdsql_database_connect(database, &extra, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(extra);
    check_equal(tdsql_database_close(database, &error), TURBODB_STATUS_BUSY);
    close_session(&first); connect_session(&first);
    extra = first;
    check_equal(tdsql_database_connect(database, &extra, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(extra);
    scalar_is(second, "SELECT score FROM items WHERE id=1", ORIGINAL_SCORE);
  }
  it("keeps result and external transaction owners alive until explicit release") {
    query(first, "SELECT score FROM items");
    check_equal(tdsql_connection_close(first, &error), TURBODB_STATUS_BUSY);
    check_equal(tdsql_database_close(database, &error), TURBODB_STATUS_BUSY);
    close_result();
    check_equal(tdsql_connection_begin(first, &transaction, &error), TURBODB_STATUS_OK);
    check_equal(tdsql_transaction_finish(transaction, false, &error), TURBODB_STATUS_OK);
    check_equal(tdsql_connection_close(first, &error), TURBODB_STATUS_BUSY);
    check_equal(tdsql_transaction_release_checked(transaction, &error), TURBODB_STATUS_OK);
    transaction = NULL;
    close_session(&first);
    scalar_is(second, "SELECT score FROM items WHERE id=1", ORIGINAL_SCORE);
  }
  it("releases the database lock only after the last session and persists commits") {
    check_equal(command(first, "UPDATE items SET score=25 WHERE id=1"), TURBODB_STATUS_OK);
    close_session(&first); close_session(&second);
    check_equal(tdsql_database_close(database, &error), TURBODB_STATUS_OK); database = NULL;
    check_equal(open_database(false, "2", &database), TURBODB_STATUS_OK);
    connect_session(&first); connect_session(&second);
    scalar_is(second, "SELECT score FROM items WHERE id=1", CHANGED_SCORE);
  }
  it("rejects invalid configuration and ABI before opening storage") {
    tdsql_database *invalid = database;
    tdsql_config config = tdsql_config_default(); config.abi_version += 1;
    check_equal(tdsql_database_open(&config, &invalid, &error), TURBODB_STATUS_ABI_MISMATCH);
    check_null(invalid);
    config = tdsql_config_default(); config.struct_size = sizeof(config.struct_size);
    check_equal(tdsql_database_open(&config, &invalid, &error), TURBODB_STATUS_ABI_MISMATCH);
    check_equal(open_database(false, "0", &invalid), TURBODB_STATUS_INVALID_ARGUMENT);
    check_null(invalid);
    check_equal(open_database(false, "18446744073709551616", &invalid), TURBODB_STATUS_INVALID_ARGUMENT);
    check_null(invalid);
    tdsql_connection *invalid_session = first;
    check_equal(tdsql_database_connect(NULL, &invalid_session, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_null(invalid_session);
    check_equal(tdsql_database_close(NULL, &error), TURBODB_STATUS_OK);
    check_equal(tdsql_database_connect(database, NULL, &error), TURBODB_STATUS_INVALID_ARGUMENT);
  }
  it("keeps statement diagnostics isolated across sessions") {
    tdsql_row row = {0};
    query(first, "SELECT CAST('12x' AS SIGNED) AS n");
    check_equal(tdsql_result_next(result, &row, &error), TURBODB_STATUS_OK);
    check_equal(row.state, TDSQL_ROW); close_result();
    query(second, "SHOW WARNINGS");
    check_equal(tdsql_result_next(result, &row, &error), TURBODB_STATUS_OK);
    check_equal(row.state, TDSQL_DONE); close_result();
    query(first, "SHOW WARNINGS");
    check_equal(tdsql_result_next(result, &row, &error), TURBODB_STATUS_OK);
    check_equal(row.state, TDSQL_ROW); check_equal(row.count, 3u);
    close_result();
  }
}
