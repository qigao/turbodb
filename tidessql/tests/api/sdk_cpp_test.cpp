#include <tidessql/tidessql.h>
#include <tinytest.hpp>
#include <cstdlib>
#include <type_traits>

static_assert(std::is_standard_layout<tdsql_config>::value, "SDK config keeps C layout");
static_assert(std::is_standard_layout<tdsql_request>::value, "SDK request keeps C layout");
static_assert(std::is_standard_layout<turbodb_value_t>::value, "Shared values keep C layout");
static_assert(std::is_standard_layout<tdsql_response>::value, "SQL responses keep C layout");
static_assert(std::is_standard_layout<tdsql_session_state>::value, "Session snapshots keep C layout");
static_assert(std::is_standard_layout<tdsql_bindings>::value, "Prepared bindings keep C layout");

suite("TidesSQL C++ public SDK") {
  it("executes SQL using only public C headers and checked lifetimes") {
    char *directory = tt_make_temp_dir("tidessql-sdk-cpp"); check_not_null(directory);
    const turbodb_option_t options[] = {
      {turbodb_view("path"), turbodb_view(directory)},
      {turbodb_view("column_family"), turbodb_view("cpp")},
      {turbodb_view("sql_initialize"), turbodb_view("true")}
    };
    tdsql_config config = tdsql_config_default();
    config.options = options; config.option_count = sizeof(options)/sizeof(options[0]);
    turbodb_error_t error; turbodb_error_init(&error);
    tdsql_connection *connection = nullptr;
    check_equal(tdsql_connection_open(&config, &connection, &error), TURBODB_STATUS_OK);
    tdsql_session_state snapshot = tdsql_session_state_default();
    check_equal(tdsql_connection_state(connection, &snapshot, &error), TURBODB_STATUS_OK);
    check_true(snapshot.autocommit); check_false(snapshot.in_transaction); check_false(snapshot.busy);
    tdsql_request input = tdsql_request_default(turbodb_view("SELECT 7 AS number"));
    tdsql_response response = tdsql_response_default();
    check_equal(tdsql_connection_run(connection, &input, &response, &error), TURBODB_STATUS_OK);
    check_equal(response.kind, TDSQL_ROWS); check_equal(response.last_insert_id, 0u);
    tdsql_result *result = response.result;
    tdsql_column column = {};
    check_equal(tdsql_result_column(result, 0, &column, &error), TURBODB_STATUS_OK);
    check_equal(column.kind, TURBODB_VALUE_INT64);
    tdsql_row row = {};
    check_equal(tdsql_result_next(result, &row, &error), TURBODB_STATUS_OK);
    check_equal(row.state, TDSQL_ROW); check_equal(row.count, 1u);
    check_equal(row.values[0].data.int64_value, 7);
    check_equal(tdsql_result_next(result, &row, &error), TURBODB_STATUS_OK);
    check_equal(row.state, TDSQL_DONE);
    check_equal(tdsql_result_destroy_checked(result, &error), TURBODB_STATUS_OK);
    check_equal(tdsql_connection_state(connection, &snapshot, &error), TURBODB_STATUS_OK);
    check_false(snapshot.busy); check_false(snapshot.in_transaction);
    input = tdsql_request_default(turbodb_view("SELECT ? AS number"));
    tdsql_statement *statement = nullptr;
    check_equal(tdsql_connection_statement_prepare(connection, &input, &statement, &error), TURBODB_STATUS_OK);
    check_equal(tdsql_statement_parameters(statement), 1u);
    const turbodb_value_t value = turbodb_i64(7);
    const tdsql_bindings bindings = tdsql_bindings_default(&value, 1);
    check_equal(tdsql_statement_execute(statement, &bindings, &response, &error), TURBODB_STATUS_OK);
    check_equal(tdsql_result_next(response.result, &row, &error), TURBODB_STATUS_OK);
    check_equal(row.values[0].data.int64_value, 7);
    check_equal(tdsql_result_destroy_checked(response.result, &error), TURBODB_STATUS_OK);
    check_equal(tdsql_statement_reset(statement, &error), TURBODB_STATUS_OK);
    check_equal(tdsql_statement_close(statement, &error), TURBODB_STATUS_OK);
    check_equal(tdsql_connection_close(connection, &error), TURBODB_STATUS_OK);
    check_equal(tt_remove_tree(directory), 0); std::free(directory);
  }
}
