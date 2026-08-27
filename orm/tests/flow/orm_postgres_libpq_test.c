#include "c_api_test_support.h"
#include "orm_postgres_libpq.h"

#include <libpq-fe.h>
#include "tinytest.h"

spec("ORM PostgreSQL libpq C adapter") {
  it("maps single-row libpq results into the cursor driver contract") {
    static const char *const values[] = {"7", "Alice"};
    static const uint8_t nulls[] = {0u, 0u};
    PGconn *connection;
    orm_postgres_driver driver;
    orm_postgres_query_request request = {
        "select id, name from person", 0, NULL, NULL, NULL, NULL, 0};
    void *row;
    void *terminal;

    fake_pg_reset();
    fake_pg_set_result(1u, 2u, values, nulls);
    connection = PQconnectdbParams(NULL, NULL, 0);
    check_not_null(connection);
    driver = orm_postgres_libpq_driver(connection);

    check_equal(driver.command->send_query(driver.context, &request), 1);
    check_equal(driver.command->enable_single_row(driver.context), 1);
    row = driver.command->next_result(driver.context);
    check_not_null(row);
    check_equal(driver.result->status(row),
                ORM_POSTGRES_RESULT_SINGLE_ROW);
    check_equal(driver.result->rows(row), (size_t)1u);
    check_equal(driver.result->columns(row), (size_t)2u);
    check_not_null(driver.result->column_name(row, 0u));
    check_equal(driver.result->value(row, 0u, 1u), "Alice");
    check_equal(driver.result->length(row, 0u, 1u), (size_t)5u);
    driver.command->release_result(row);

    terminal = driver.command->next_result(driver.context);
    check_not_null(terminal);
    check_equal(driver.result->status(terminal),
                ORM_POSTGRES_RESULT_TUPLES_DONE);
    driver.command->release_result(terminal);
    check_null(driver.command->next_result(driver.context));
    check_equal(fake_pg_cleared_results(), 2);
    PQfinish(connection);
  }
}
