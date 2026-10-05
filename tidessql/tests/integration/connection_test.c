#include <tidessql/tidessql.h>
#include <cmeta/cmeta.h>
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

static tdsql_connection *connection;
static tdsql_transaction *transaction;
static tdsql_result *result;
static turbodb_error_t error;
static char *directory;
static uint64_t affected;
enum { AFFECTED_SENTINEL = 47 };

static tdsql_request request(const char *sql) {
  return tdsql_request_default(turbodb_view(sql));
}
static turbodb_value_t TDSQL_CALL read_parameter(const void *context, size_t index) {
  return ((const turbodb_value_t *)context)[index];
}
static void connect_database(bool initialize) {
  const turbodb_option_t options[] = {
    {turbodb_view("path"), turbodb_view(directory)},
    {turbodb_view("column_family"), turbodb_view("direct")},
    {turbodb_view("sql_initialize"), turbodb_view(initialize ? "true" : "false")}
  };
  const tdsql_config config = {.struct_size=sizeof(tdsql_config), .abi_version=TDSQL_ABI_VERSION, .options=options,
      .option_count=sizeof(options)/sizeof(options[0]), .limits=tdsql_limits_default()};
  check_equal(tdsql_connection_open(&config, &connection, &error), TURBODB_STATUS_OK);
  check_not_null(connection);
}
static turbodb_status_t execute(const char *sql) {
  const tdsql_request input = request(sql);
  return transaction ? tdsql_transaction_execute(transaction, &input, &affected, &error)
      : tdsql_connection_execute(connection, &input, &affected, &error);
}
static void query(const char *sql) {
  const tdsql_request input = request(sql);
  check_null(result);
  const turbodb_status_t status = transaction
      ? tdsql_transaction_query(transaction, &input, &result, &error)
      : tdsql_connection_query(connection, &input, &result, &error);
  check_equal(status, TURBODB_STATUS_OK);
  check_not_null(result);
}
static tdsql_row next(void) {
  tdsql_row row = {0};
  check_equal(tdsql_result_next(result, &row, &error), TURBODB_STATUS_OK);
  return row;
}
static void close_result(void) {
  check_equal(tdsql_result_destroy_checked(result, &error), TURBODB_STATUS_OK); result = NULL;
}
static void finish(bool commit) {
  check_equal(tdsql_transaction_finish(transaction, commit, &error), TURBODB_STATUS_OK);
  check_equal(tdsql_transaction_release_checked(transaction, &error), TURBODB_STATUS_OK); transaction = NULL;
}
static void score_is(int64_t expected) {
  query("SELECT score FROM items WHERE id=1");
  const tdsql_row row = next();
  check_equal(row.state, TDSQL_ROW); check_equal(row.count, 1u);
  check_equal(row.values[0].kind, TURBODB_VALUE_INT64);
  check_equal(row.values[0].data.int64_value, expected);
  check_equal(next().state, TDSQL_DONE); close_result();
}

spec("TidesSQL direct connection contract") {
  before_each() {
    connection = NULL; transaction = NULL; result = NULL; affected = 0;
    turbodb_error_init(&error);
    directory = tt_make_temp_dir("tidessql-connection"); check_not_null(directory);
    connect_database(true);
    check_equal(execute("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)"), TURBODB_STATUS_OK);
    check_equal(execute("INSERT INTO items(id,score) VALUES(1,10)"), TURBODB_STATUS_OK);
  }
  it("rejects incompatible and truncated SDK inputs before consuming parameters or outputs") {
    check_equal(tdsql_abi_version(), TDSQL_ABI_VERSION);
    tdsql_config config = tdsql_config_default();
    tdsql_connection *invalid = NULL;
    config.abi_version = TDSQL_ABI_VERSION + 1;
    check_equal(tdsql_connection_open(&config, &invalid, &error), TURBODB_STATUS_ABI_MISMATCH);
    check_null(invalid);
    config.abi_version = TDSQL_ABI_VERSION; config.struct_size = sizeof(config.struct_size);
    check_equal(tdsql_connection_open(&config, &invalid, &error), TURBODB_STATUS_ABI_MISMATCH);
    check_null(invalid);
    tdsql_request input = request("SELECT ? AS value");
    input.abi_version = TDSQL_ABI_VERSION + 1;
    input.parameter_count = 1; input.read_parameter = read_parameter;
    input.parameter_context = NULL;
    check_equal(tdsql_connection_query(connection, &input, &result, &error), TURBODB_STATUS_ABI_MISMATCH);
    check_null(result);
    input.abi_version = TDSQL_ABI_VERSION; input.struct_size = sizeof(input.struct_size);
    affected = AFFECTED_SENTINEL;
    check_equal(tdsql_connection_execute(connection, &input, &affected, &error), TURBODB_STATUS_ABI_MISMATCH);
    check_equal(affected, AFFECTED_SENTINEL);
    check_equal(tdsql_connection_begin(connection, &transaction, &error), TURBODB_STATUS_OK);
    check_equal(tdsql_transaction_query(transaction, &input, &result, &error), TURBODB_STATUS_ABI_MISMATCH);
    check_null(result);
    check_equal(tdsql_transaction_execute(transaction, &input, &affected, &error), TURBODB_STATUS_ABI_MISMATCH);
    check_equal(affected, AFFECTED_SENTINEL);
    finish(false); score_is(10);
  }
  after_each() {
    close_result();
    if (transaction) finish(false);
    check_equal(tdsql_connection_close(connection, &error), TURBODB_STATUS_OK); connection = NULL;
    check_equal(tt_remove_tree(directory), 0); free(directory); directory = NULL;
  }
  it("owns query parameters and SQL after the synchronous call returns") {
    char sql[] = "SELECT ? AS label";
    char label[] = "original";
    turbodb_value_t parameters[] = {turbodb_text(label)};
    tdsql_request input = request(sql);
    input.parameter_count = 1; input.read_parameter = read_parameter;
    input.parameter_context = parameters;
    check_equal(tdsql_connection_query(connection, &input, &result, &error), TURBODB_STATUS_OK);
    memset(sql, 'x', sizeof(sql)); memset(label, 'y', sizeof(label)); parameters[0] = turbodb_null();
    tdsql_column column = {0};
    check_equal(tdsql_result_columns(result), 1u);
    check_equal(tdsql_result_column(result, 0, &column, &error), TURBODB_STATUS_OK);
    check_equal(column.name.len, strlen("label")); check_equal(memcmp(column.name.data,"label",column.name.len),0);
    const tdsql_row row = next();
    check_equal(row.state, TDSQL_ROW); check_equal(row.values[0].kind, TURBODB_VALUE_TEXT);
    check_equal(row.values[0].data.text_value.len, strlen("original"));
    check_equal(memcmp(row.values[0].data.text_value.data,"original",row.values[0].data.text_value.len),0);
    check_equal(next().state, TDSQL_DONE);
  }
  it("holds the statement through EOF and cancellation until result destroy") {
    query("SELECT score FROM items");
    check_equal(next().state, TDSQL_ROW); check_equal(next().state, TDSQL_DONE);
    affected = AFFECTED_SENTINEL;
    check_equal(execute("UPDATE items SET score=20 WHERE id=1"), TURBODB_STATUS_BUSY);
    check_equal(affected, AFFECTED_SENTINEL);
    check_equal(tdsql_connection_close(connection, &error), TURBODB_STATUS_BUSY);
    close_result();
    check_equal(execute("UPDATE items SET score=20 WHERE id=1"), TURBODB_STATUS_OK);
    query("SELECT score FROM items");
    check_equal(tdsql_result_cancel(result, &error), TURBODB_STATUS_OK);
    check_equal(next().state, TDSQL_DONE);
    check_equal(tdsql_connection_close(connection, &error), TURBODB_STATUS_BUSY);
    close_result(); score_is(20);
  }
  it("owns zeroed aligned consumer context and rejects context overflow without poisoning") {
    tdsql_request input = request("SELECT 1 AS number");
    input.result_context_bytes = sizeof(cmeta_capture_storage);
    check_equal(tdsql_connection_query(connection, &input, &result, &error), TURBODB_STATUS_OK);
    unsigned char *context = tdsql_result_context(result); check_not_null(context);
    check_equal((uintptr_t)context % _Alignof(cmeta_capture_storage), 0u);
    for (size_t i=0; i<input.result_context_bytes; ++i) check_equal(context[i], 0u);
    memset(context, 'z', input.result_context_bytes);
    check_equal(next().state, TDSQL_ROW);
    check_equal(context[0], (unsigned char)'z'); close_result();
    input.result_context_bytes = SIZE_MAX;
    check_equal(tdsql_connection_query(connection, &input, &result, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(result); score_is(10);
  }
  it("keeps SQL session access modes and automatic transaction state inside the engine") {
    check_equal(execute("SET SESSION transaction_read_only=ON"), TURBODB_STATUS_OK);
    check_equal(execute("BEGIN"), TURBODB_STATUS_OK);
    affected = AFFECTED_SENTINEL;
    check_equal(execute("INSERT INTO items(id,score) VALUES(2,20)"), TURBODB_STATUS_SQL_ERROR);
    check_equal(affected, AFFECTED_SENTINEL); score_is(10);
    check_equal(execute("ROLLBACK"), TURBODB_STATUS_OK);
    check_equal(execute("SET SESSION transaction_read_only=OFF"), TURBODB_STATUS_OK);
    check_equal(execute("SET autocommit=OFF"), TURBODB_STATUS_OK);
    check_equal(execute("UPDATE items SET score=30 WHERE id=1"), TURBODB_STATUS_OK); score_is(30);
    check_equal(execute("ROLLBACK"), TURBODB_STATUS_OK); score_is(10);
    check_equal(execute("SET autocommit=ON"), TURBODB_STATUS_OK);
  }
  it("preserves explicit savepoint rollback and finished handle lifetimes") {
    check_equal(tdsql_connection_begin(connection, &transaction, &error), TURBODB_STATUS_OK);
    check_equal(tdsql_transaction_savepoint(transaction, turbodb_view("point"), TDSQL_SAVEPOINT_CREATE, &error), TURBODB_STATUS_OK);
    check_equal(execute("UPDATE items SET score=40 WHERE id=1"), TURBODB_STATUS_OK);
    check_equal(tdsql_transaction_savepoint(transaction, turbodb_view("point"), TDSQL_SAVEPOINT_ROLLBACK, &error), TURBODB_STATUS_OK);
    score_is(10);
    check_equal(tdsql_transaction_finish(transaction, true, &error), TURBODB_STATUS_OK);
    check_equal(tdsql_connection_close(connection, &error), TURBODB_STATUS_BUSY);
    tdsql_transaction *finished = transaction; transaction = NULL;
    check_equal(tdsql_connection_begin(connection, &transaction, &error), TURBODB_STATUS_OK);
    tdsql_transaction_release(finished);
    check_equal(execute("UPDATE items SET score=50 WHERE id=1"), TURBODB_STATUS_OK);
    query("SELECT score FROM items");
    check_equal(tdsql_transaction_finish(transaction, true, &error), TURBODB_STATUS_BUSY);
    check_equal(tdsql_connection_close(connection, &error), TURBODB_STATUS_BUSY);
    close_result(); finish(false); score_is(10);
  }
  it("commits parameterized writes and reopens the same native database") {
    check_equal(tdsql_connection_begin(connection, &transaction, &error), TURBODB_STATUS_OK);
    const turbodb_value_t parameters[] = {turbodb_i64(60), turbodb_i64(1)};
    tdsql_request input = request("UPDATE items SET score=? WHERE id=?");
    input.parameter_count = sizeof(parameters)/sizeof(parameters[0]);
    input.read_parameter = read_parameter; input.parameter_context = parameters;
    check_equal(tdsql_transaction_execute(transaction, &input, &affected, &error), TURBODB_STATUS_OK);
    check_equal(affected, 1u); finish(true);
    check_equal(tdsql_connection_close(connection, &error), TURBODB_STATUS_OK); connection = NULL;
    connect_database(false); score_is(60);
  }
  it("preserves binding errors and indexed column access without weakening query limits") {
    tdsql_request input = request("SELECT 1 AS same_name,2 AS same_name");
    input.unique_column_names = true;
    check_equal(tdsql_connection_query(connection, &input, &result, &error), TURBODB_STATUS_SQL_ERROR);
    check_null(result);
    input.unique_column_names = false;
    check_equal(tdsql_connection_query(connection, &input, &result, &error), TURBODB_STATUS_SQL_ERROR);
    check_null(result);
    input = request("SELECT 1 AS first_value,2 AS second_value");
    check_equal(tdsql_connection_query(connection, &input, &result, &error), TURBODB_STATUS_OK);
    check_equal(tdsql_result_columns(result), 2u);
    const tdsql_row row = next();
    check_equal(row.values[0].data.int64_value, 1); check_equal(row.values[1].data.int64_value, 2);
    close_result();
    input = request("SELECT ? AS value"); input.parameter_count = 1;
    check_equal(tdsql_connection_query(connection, &input, &result, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_null(result);
    input = request("SELECT 1 AS value UNION ALL SELECT 2 AS value"); input.limits.max_result_rows = 1;
    check_equal(tdsql_connection_query(connection, &input, &result, &error), TURBODB_STATUS_OK);
    check_equal(next().state, TDSQL_ROW);
    tdsql_row unchanged = {.state=TDSQL_CANCELLED};
    check_equal(tdsql_result_next(result, &unchanged, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(unchanged.state, TDSQL_CANCELLED);
    close_result(); score_is(10);
  }
}
