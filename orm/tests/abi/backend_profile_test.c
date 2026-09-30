#include <orm.h>

#include <tinytest.h>

spec("ORM backend profile") {
  it("provides the SQLite backend in the internal test core") {
    const orm_option_t filename = {orm_view("filename"), orm_view(":memory:")};
    orm_config_t config;
    orm_connection_t *connection = (orm_connection_t *)1;
    orm_error_t error;
    orm_status_t status;

    orm_config(&config);
    orm_error_init(&error);
    config.driver = orm_view("sqlite");
    config.options = &filename;
    config.option_count = 1u;

    status = orm_connect(&config, &connection, &error);
    check_equal(status, ORM_STATUS_OK);
    check_not_null(connection);
    check_equal(error.status, ORM_STATUS_OK);
    orm_disconnect(connection);
  }
}
