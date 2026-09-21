#include "orm_postgres_libpq.h"

#include <tinytest.h>
#include <stdio.h>
#include <string.h>

/* Real libpq, no fake server and no network requirement. A rejected conninfo
 * gives a genuine CONNECTION_BAD handle that still must be PQfinish'd. */
static const char invalid_conninfo[] = "turbodb_invalid_connection_option=1";

spec("ORM PostgreSQL real libpq failed connection") {
  (void)ttest_config__;
  it("reads native connection health instead of inferring it from diagnostics") {
    PGconn *connection = PQconnectdb(invalid_conninfo);
    check_not_null(connection);
    const orm_postgres_driver driver = orm_postgres_libpq_driver(connection);
    const int native_status = (int)PQstatus(connection);
    const int adapter_ok = driver.command->connection_ok(driver.context);
    PQfinish(connection);
    check_equal(native_status, (int)CONNECTION_BAD);
    check_equal(adapter_ok, 0);
  }
  it("rejects dispatch without a cursor and retains the copied native error") {
    PGconn *connection = PQconnectdb(invalid_conninfo);
    check_not_null(connection);
    const orm_postgres_driver driver = orm_postgres_libpq_driver(connection);
    orm_postgres_query_request request = {
        "select 1", 0, NULL, NULL, NULL, NULL, 0};
    orm_postgres_cursor_config config = ORM_POSTGRES_CURSOR_CONFIG_INIT(
        1u, 1u, 64u, NULL, NULL, NULL);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    char native_message[ORM_C_ERROR_MESSAGE_CAPACITY];
    orm_error_init(&error);
    const orm_status_t status = orm_postgres_cursor_start(
        &cursor, &driver, &request, &config, &error);
    (void)snprintf(native_message, sizeof(native_message), "%s",
                   PQerrorMessage(connection));
    /* Keep failure diagnostics valid even if the caller immediately closes
     * the failed native connection. Never dereference it after PQfinish. */
    if (cursor.ops != NULL) cursor.ops->destroy(cursor.context);
    PQfinish(connection);
    check_equal(status, ORM_STATUS_CONNECTION_ERROR);
    check_equal(error.status, ORM_STATUS_CONNECTION_ERROR);
    check_true(error.message[0] != '\0');
    check_equal(strcmp(error.message, native_message), 0);
    check_null(cursor.ops);
    check_null(cursor.context);
  }
}
