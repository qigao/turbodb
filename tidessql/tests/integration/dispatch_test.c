#include <tidessql/tidessql.h>
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

static tdsql_connection *connection;
static tdsql_transaction *transaction, *finished;
static tdsql_response response;
static turbodb_error_t error;
static char *directory;
static size_t parameter_reads;
enum { RESPONSE_SENTINEL = 47, ORIGINAL_SCORE = 10, CHANGED_SCORE = 25 };

static turbodb_value_t TDSQL_CALL read_parameter(const void *context, size_t index) {
  ++parameter_reads;
  return ((const turbodb_value_t *)context)[index];
}
static turbodb_status_t run(const char *sql) {
  const tdsql_request input = tdsql_request_default(turbodb_view(sql));
  return tdsql_connection_run(connection, &input, &response, &error);
}
static void close_result(void) {
  check_equal(tdsql_result_destroy_checked(response.result, &error), TURBODB_STATUS_OK);
  response.result = NULL;
}
static tdsql_session_state state(void) {
  tdsql_session_state out = tdsql_session_state_default();
  check_equal(tdsql_connection_state(connection, &out, &error), TURBODB_STATUS_OK);
  return out;
}
static tdsql_row next(void) {
  tdsql_row row = {0};
  check_equal(tdsql_result_next(response.result, &row, &error), TURBODB_STATUS_OK);
  return row;
}
static void scalar_is(const char *sql, int64_t expected) {
  check_equal(run(sql), TURBODB_STATUS_OK); check_equal(response.kind, TDSQL_ROWS);
  const tdsql_row row = next();
  check_equal(row.state, TDSQL_ROW); check_equal(row.count, 1u);
  check_equal(row.values[0].kind, TURBODB_VALUE_INT64);
  check_equal(row.values[0].data.int64_value, expected);
  check_equal(next().state, TDSQL_DONE); close_result();
}

spec("TidesSQL unified dispatch and read-only session state") {
  before_each() {
    connection = NULL; transaction = finished = NULL; response = tdsql_response_default();
    parameter_reads = 0; turbodb_error_init(&error);
    directory = tt_make_temp_dir("tidessql-dispatch"); check_not_null(directory);
    const turbodb_option_t options[] = {
      {turbodb_view("path"), turbodb_view(directory)},
      {turbodb_view("column_family"), turbodb_view("dispatch")},
      {turbodb_view("sql_initialize"), turbodb_view("true")}
    };
    tdsql_config config = tdsql_config_default();
    config.options = options; config.option_count = sizeof(options)/sizeof(options[0]);
    check_equal(tdsql_connection_open(&config, &connection, &error), TURBODB_STATUS_OK);
    check_equal(run("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)"), TURBODB_STATUS_OK);
    check_equal(run("INSERT INTO items VALUES(1,10)"), TURBODB_STATUS_OK);
  }
  after_each() {
    close_result();
    check_equal(tdsql_transaction_release_checked(finished, &error), TURBODB_STATUS_OK); finished = NULL;
    check_equal(tdsql_transaction_release_checked(transaction, &error), TURBODB_STATUS_OK); transaction = NULL;
    check_equal(tdsql_connection_close(connection, &error), TURBODB_STATUS_OK); connection = NULL;
    check_equal(tt_remove_tree(directory), 0); free(directory); directory = NULL;
  }
  it("returns command counts and owned rows without trial dispatch") {
    check_equal(response.kind, TDSQL_COMMAND); check_null(response.result);
    check_equal(response.affected_rows, 1u); check_equal(response.last_insert_id, 0u);
    check_equal(run("UPDATE items SET score=25 WHERE id=1"), TURBODB_STATUS_OK);
    check_equal(response.kind, TDSQL_COMMAND); check_equal(response.affected_rows, 1u);
    scalar_is("SELECT score FROM items WHERE id=1", CHANGED_SCORE);
    check_equal(response.affected_rows, 0u); check_equal(response.last_insert_id, 0u);
  }
  it("classifies CTE queries compound queries SHOW and EXPLAIN as rows") {
    scalar_is("WITH q AS(SELECT score FROM items) SELECT score FROM q", ORIGINAL_SCORE);
    check_equal(run("SELECT id FROM items UNION ALL SELECT 2 AS id"), TURBODB_STATUS_OK);
    check_equal(response.kind, TDSQL_ROWS); check_equal(next().state, TDSQL_ROW);
    check_equal(next().state, TDSQL_ROW); check_equal(next().state, TDSQL_DONE); close_result();
    check_equal(run("SHOW COLUMNS FROM items"), TURBODB_STATUS_OK);
    check_equal(response.kind, TDSQL_ROWS); check_greater(tdsql_result_columns(response.result), 0u); close_result();
    check_equal(run("EXPLAIN SELECT id FROM items"), TURBODB_STATUS_OK);
    check_equal(response.kind, TDSQL_ROWS); check_equal(next().state, TDSQL_ROW); close_result();
  }
  it("binds command parameters exactly once and copies query parameter bytes") {
    turbodb_value_t parameters[] = {turbodb_i64(CHANGED_SCORE), turbodb_i64(1)};
    tdsql_request input = tdsql_request_default(turbodb_view("UPDATE items SET score=? WHERE id=?"));
    input.parameter_count = sizeof(parameters)/sizeof(parameters[0]);
    input.read_parameter = read_parameter; input.parameter_context = parameters;
    check_equal(tdsql_connection_run(connection, &input, &response, &error), TURBODB_STATUS_OK);
    check_equal(response.kind, TDSQL_COMMAND); check_equal(response.affected_rows, 1u);
    check_equal(parameter_reads, input.parameter_count);
    char text[] = "original";
    parameters[0] = turbodb_text(text); input = tdsql_request_default(turbodb_view("SELECT ? AS value"));
    input.parameter_count = 1; input.read_parameter = read_parameter; input.parameter_context = parameters;
    parameter_reads = 0;
    check_equal(tdsql_connection_run(connection, &input, &response, &error), TURBODB_STATUS_OK);
    check_equal(parameter_reads, 1u); text[0] = 'x'; parameters[0] = turbodb_i64(0);
    const tdsql_row row = next(); check_equal(row.state, TDSQL_ROW);
    check_equal(row.values[0].kind, TURBODB_VALUE_TEXT);
    check_equal(row.values[0].data.text_value.len, strlen("original"));
    check_equal(row.values[0].data.text_value.data, "original", strlen("original")); close_result();
  }
  it("classifies CTE UPDATE and DELETE as commands using their statement body") {
    check_equal(run("WITH q AS(SELECT 1 AS id) UPDATE items SET score=25 WHERE id IN(SELECT id FROM q)"), TURBODB_STATUS_OK);
    check_equal(response.kind, TDSQL_COMMAND); check_equal(response.affected_rows, 1u); check_null(response.result);
    scalar_is("SELECT score FROM items WHERE id=1", CHANGED_SCORE);
    check_equal(run("WITH q AS(SELECT 1 AS id) DELETE FROM items WHERE id IN(SELECT id FROM q)"), TURBODB_STATUS_OK);
    check_equal(response.kind, TDSQL_COMMAND); check_equal(response.affected_rows, 1u); check_null(response.result);
    scalar_is("SELECT COUNT(*) AS count FROM items", 0);
  }
  it("preserves all supported parameter kinds through the unified result path") {
    enum { VALUE_COUNT = 7 };
    const unsigned char blob[] = {0, 1, 255};
    const turbodb_value_t parameters[VALUE_COUNT] = {
      turbodb_null(), turbodb_i64(INT64_MIN), turbodb_u64(UINT64_MAX), turbodb_f64(1.5),
      turbodb_bool(true), turbodb_text("text"), turbodb_blob(blob, sizeof(blob))
    };
    tdsql_request input = tdsql_request_default(turbodb_view(
        "SELECT ? AS n,? AS i,? AS u,? AS d,? AS b,? AS t,? AS bytes"));
    input.parameter_count = VALUE_COUNT; input.read_parameter = read_parameter; input.parameter_context = parameters;
    check_equal(tdsql_connection_run(connection, &input, &response, &error), TURBODB_STATUS_OK);
    check_equal(parameter_reads, (size_t)VALUE_COUNT);
    const tdsql_row row = next(); check_equal(row.state, TDSQL_ROW); check_equal(row.count, (size_t)VALUE_COUNT);
    for (size_t i = 0; i < VALUE_COUNT; ++i) check_equal(row.values[i].kind, parameters[i].kind);
    check_equal(row.values[1].data.int64_value, INT64_MIN); check_equal(row.values[2].data.uint64_value, UINT64_MAX);
    check_equal(row.values[3].data.double_value, 1.5); check_equal(row.values[4].data.boolean_value, 1u);
    check_equal(row.values[5].data.text_value.data, "text", strlen("text"));
    check_equal(row.values[6].data.blob_value.size, sizeof(blob));
    check_equal(row.values[6].data.blob_value.data, blob, sizeof(blob)); close_result();
  }
  it("keeps response unchanged when SQL parsing binding or output ABI fails") {
    response.affected_rows = RESPONSE_SENTINEL;
    check_equal(run("SELECT FROM"), TURBODB_STATUS_SQL_ERROR);
    check_equal(response.affected_rows, RESPONSE_SENTINEL); check_null(response.result);
    check_equal(run("SELECT missing FROM items"), TURBODB_STATUS_SQL_ERROR);
    check_equal(response.affected_rows, RESPONSE_SENTINEL); check_null(response.result);
    tdsql_request input = tdsql_request_default(turbodb_view("SELECT ? AS n"));
    input.parameter_count = 1; input.read_parameter = read_parameter;
    response.abi_version += 1;
    check_equal(tdsql_connection_run(connection, &input, &response, &error), TURBODB_STATUS_ABI_MISMATCH);
    check_equal(parameter_reads, 0u); check_equal(response.affected_rows, RESPONSE_SENTINEL);
    response.abi_version = TDSQL_ABI_VERSION; response.struct_size = sizeof(response.struct_size);
    check_equal(tdsql_connection_run(connection, &input, &response, &error), TURBODB_STATUS_ABI_MISMATCH);
    check_equal(parameter_reads, 0u); response = tdsql_response_default();
    scalar_is("SELECT score FROM items WHERE id=1", ORIGINAL_SCORE);
  }
  it("observes a busy autocommit result without creating a multi-statement transaction") {
    tdsql_session_state snapshot = state();
    check_true(snapshot.autocommit); check_false(snapshot.in_transaction); check_false(snapshot.busy);
    check_equal(run("SELECT score FROM items"), TURBODB_STATUS_OK);
    snapshot = state(); check_true(snapshot.autocommit); check_false(snapshot.in_transaction); check_true(snapshot.busy);
    check_equal(next().state, TDSQL_ROW); check_equal(next().state, TDSQL_DONE);
    check_true(state().busy);
    const tdsql_response before = response;
    check_equal(run("UPDATE items SET score=25 WHERE id=1"), TURBODB_STATUS_BUSY);
    check_true(response.result == before.result); check_equal(response.kind, before.kind);
    close_result(); check_false(state().busy); check_false(state().in_transaction);
  }
  it("tracks SQL transaction control and pending read-only mode from the actual owner") {
    check_equal(run("SET TRANSACTION READ ONLY"), TURBODB_STATUS_OK);
    check_false(state().session_read_only); check_false(state().in_transaction);
    check_equal(run("BEGIN"), TURBODB_STATUS_OK);
    tdsql_session_state snapshot = state();
    check_true(snapshot.in_transaction); check_true(snapshot.transaction_read_only); check_false(snapshot.busy);
    check_equal(run("UPDATE items SET score=25 WHERE id=1"), TURBODB_STATUS_SQL_ERROR);
    check_true(state().in_transaction);
    check_equal(run("ROLLBACK"), TURBODB_STATUS_OK);
    snapshot = state(); check_false(snapshot.in_transaction); check_false(snapshot.transaction_read_only);
    check_equal(run("BEGIN"), TURBODB_STATUS_OK); check_false(state().transaction_read_only);
    check_equal(run("COMMIT AND CHAIN"), TURBODB_STATUS_OK); check_true(state().in_transaction);
    check_equal(run("ROLLBACK"), TURBODB_STATUS_OK); check_false(state().in_transaction);
  }
  it("starts an implicit multi-statement owner only when autocommit is disabled and SQL runs") {
    check_equal(run("SET autocommit=OFF"), TURBODB_STATUS_OK);
    check_false(state().autocommit); check_false(state().in_transaction);
    scalar_is("SELECT score FROM items WHERE id=1", ORIGINAL_SCORE);
    check_true(state().in_transaction); check_false(state().busy);
    check_equal(run("UPDATE items SET score=25 WHERE id=1"), TURBODB_STATUS_OK);
    check_equal(run("SET autocommit=ON"), TURBODB_STATUS_OK);
    check_true(state().autocommit); check_false(state().in_transaction);
    scalar_is("SELECT score FROM items WHERE id=1", CHANGED_SCORE);
  }
  it("keeps external transaction observations correct when a finished handle outlives its replacement") {
    check_equal(tdsql_connection_begin(connection, &transaction, &error), TURBODB_STATUS_OK);
    check_true(state().in_transaction); check_true(state().busy);
    check_equal(tdsql_transaction_finish(transaction, false, &error), TURBODB_STATUS_OK);
    check_false(state().in_transaction); check_false(state().busy);
    finished = transaction; transaction = NULL;
    check_equal(tdsql_connection_begin(connection, &transaction, &error), TURBODB_STATUS_OK);
    check_equal(tdsql_transaction_release_checked(finished, &error), TURBODB_STATUS_OK); finished = NULL;
    check_true(state().in_transaction); check_true(state().busy);
    check_equal(tdsql_transaction_release_checked(transaction, &error), TURBODB_STATUS_OK); transaction = NULL;
    check_false(state().in_transaction); check_false(state().busy);
  }
  it("reads warnings without clearing them while results are active or after destruction") {
    check_equal(run("SELECT CAST('12x' AS SIGNED) AS n"), TURBODB_STATUS_OK);
    check_equal(next().state, TDSQL_ROW); check_equal(state().warning_count, 1u);
    check_equal(state().warning_count, 1u); close_result(); check_equal(state().warning_count, 1u);
    check_equal(run("SHOW WARNINGS"), TURBODB_STATUS_OK);
    check_equal(response.kind, TDSQL_ROWS); check_equal(next().state, TDSQL_ROW); close_result();
    check_equal(state().warning_count, 1u);
    check_equal(run("SET autocommit=ON"), TURBODB_STATUS_OK); check_equal(state().warning_count, 0u);
  }
  it("rejects invalid snapshot output without altering it or changing the session") {
    tdsql_session_state snapshot = tdsql_session_state_default();
    snapshot.abi_version += 1; snapshot.warning_count = RESPONSE_SENTINEL; snapshot.busy = true;
    check_equal(tdsql_connection_state(connection, &snapshot, &error), TURBODB_STATUS_ABI_MISMATCH);
    check_equal(snapshot.warning_count, RESPONSE_SENTINEL); check_true(snapshot.busy);
    snapshot.abi_version = TDSQL_ABI_VERSION; snapshot.struct_size = sizeof(snapshot.struct_size);
    check_equal(tdsql_connection_state(connection, &snapshot, &error), TURBODB_STATUS_ABI_MISMATCH);
    check_equal(snapshot.warning_count, RESPONSE_SENTINEL);
    check_equal(tdsql_connection_state(NULL, &snapshot, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(tdsql_connection_state(connection, NULL, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_false(state().in_transaction); check_true(state().autocommit);
  }
  it("preserves result limits through the unified query path") {
    tdsql_request input = tdsql_request_default(turbodb_view("SELECT 1 AS n UNION ALL SELECT 2 AS n"));
    input.limits.max_result_rows = 1;
    check_equal(tdsql_connection_run(connection, &input, &response, &error), TURBODB_STATUS_OK);
    check_equal(next().state, TDSQL_ROW);
    tdsql_row unchanged = {.state=TDSQL_CANCELLED};
    check_equal(tdsql_result_next(response.result, &unchanged, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(unchanged.state, TDSQL_CANCELLED); check_true(state().busy);
    close_result(); scalar_is("SELECT score FROM items WHERE id=1", ORIGINAL_SCORE);
  }
  it("rejects unsupported SQL and multi-statements without executing another dispatch path") {
    response.affected_rows = RESPONSE_SENTINEL;
    check_equal(run("CREATE VIEW v AS SELECT score FROM items"), TURBODB_STATUS_UNSUPPORTED);
    check_equal(response.affected_rows, RESPONSE_SENTINEL); check_null(response.result);
    check_equal(run("UPDATE items SET score=25 WHERE id=1; DELETE FROM items"), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(response.affected_rows, RESPONSE_SENTINEL);
    scalar_is("SELECT score FROM items WHERE id=1", ORIGINAL_SCORE);
    check_equal(run("CREATE TABLE generated(id BIGINT PRIMARY KEY AUTO_INCREMENT)"), TURBODB_STATUS_UNSUPPORTED);
    check_equal(run("SELECT LAST_INSERT_ID() AS n"), TURBODB_STATUS_UNSUPPORTED);
  }
}
