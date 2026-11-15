#include <turbodb_mysql.h>
#include <tinytest.h>

#include <string.h>

spec("standalone MySQL client without ORM") {
  (void)ttest_config__;

  it("rejects an invalid connection before opening transport") {
    mysql_session_error_t error = {0};
    check_equal(mysql_session_connect_and_ping(NULL, &error),
                MYSQL_SESSION_INVALID);
    check_equal(error.status, MYSQL_SESSION_INVALID);
    check_equal(strcmp(error.stage, "config"), 0);
  }

  it("rejects unsupported isolation without transferring a transaction") {
    mysql_transaction_session_t *transaction = NULL;
    mysql_session_error_t error = {0};
    const size_t command_capacity = 4096u;
    check_equal(mysql_transaction_session_begin(
                    NULL, (mysql_isolation_t)-1, command_capacity,
                    &transaction, &error),
                MYSQL_SESSION_INVALID);
    check_null(transaction);
    check_equal(strcmp(error.stage, "transaction-config"), 0);
  }

  it("clears source outputs when a prepared query is invalid") {
    mysql_cursor_source_t source = {0};
    const mysql_column_definition_t *columns = NULL;
    size_t column_count = 1u;
    mysql_session_error_t error = {0};
    check_equal(mysql_session_open_prepared_source(
                    NULL, NULL, 0u, NULL, 0u, NULL, &source,
                    &columns, &column_count, &error),
                MYSQL_SESSION_INVALID);
    check_null(source.ops);
    check_null(source.context);
    check_null(columns);
    check_equal(column_count, (size_t)0u);
  }

  it("decodes a bounded binary row through the native row store") {
    static const uint8_t name[] = {'v'};
    static const uint8_t row[] = {0x00, 0x00, 0x2a};
    const size_t column_limit = 1u;
    const size_t metadata_limit = 64u;
    const size_t row_limit = 16u;
    mysql_column_definition_t column = {0};
    mysql_cursor_row_store_t store = {0};
    mysql_wire_status_t status;
    cserde_reader reader = {0};
    cserde_token token;
    column.name.data = name;
    column.name.length = sizeof(name);
    column.type = MYSQL_FIELD_TYPE_TINY;
    status = mysql_cursor_row_store_init(
        &store, &column, 1u, column_limit, metadata_limit, row_limit);
    check_equal(status, MYSQL_WIRE_STATUS_OK);
    if (status == MYSQL_WIRE_STATUS_OK) {
      check_equal(mysql_cursor_row_store_load(&store, row, sizeof(row)),
                  MYSQL_WIRE_STATUS_OK);
      check_equal(mysql_cursor_row_store_reader(&store, &reader), CSERDE_OK);
      check_equal(cserde_reader_next(&reader, &token), CSERDE_OK);
      check_equal(token.kind, CSERDE_MAP_BEGIN);
      check_equal(cserde_reader_next(&reader, &token), CSERDE_OK);
      check_equal(token.kind, CSERDE_STRING);
      check_equal(cserde_reader_next(&reader, &token), CSERDE_OK);
      check_equal(token.kind, CSERDE_SINT);
      check_equal(token.value.sint, (int64_t)42);
      mysql_cursor_row_store_destroy(&store);
    }
  }
}
