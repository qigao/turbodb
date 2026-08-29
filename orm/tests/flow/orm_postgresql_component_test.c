#include <orm_postgresql.h>

#include "tinytest.h"

spec("ORM PostgreSQL component") {
  it("reports an ABI mismatch before validating the driver token") {
    orm_config_t config;
    orm_connection_t *connection = (orm_connection_t *)1;
    orm_error_t error;

    orm_config(&config);
    orm_error_init(&error);
    config.struct_size = 0u;
    config.driver = orm_view("sqlite");

    check_equal(orm_postgresql_connect(&config, &connection, &error),
                ORM_STATUS_ABI_MISMATCH);
    check_null(connection);
    check_equal(error.status, ORM_STATUS_ABI_MISMATCH);
  }

  it("rejects a non-PostgreSQL driver after ABI validation") {
    orm_config_t config;
    orm_connection_t *connection = (orm_connection_t *)1;
    orm_error_t error;

    orm_config(&config);
    orm_error_init(&error);
    config.driver = orm_view("sqlite");

    check_equal(orm_postgresql_connect(&config, &connection, &error),
                ORM_STATUS_INVALID_ARGUMENT);
    check_null(connection);
    check_equal(error.status, ORM_STATUS_INVALID_ARGUMENT);
  }
}
