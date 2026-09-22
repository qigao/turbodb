#include "orm_driver_owner_bridge.h"
#include "orm_internal.h"

#include <tinytest.h>

#include <string.h>

static vstr view(const char *text) {
  return vstr_from_cstr(text);
}

static orm_connection_t *open_sqlite(orm_error_t *error) {
  orm_config_t config;
  orm_option_t filename;
  orm_connection_t *connection = NULL;
  orm_config(&config);
  filename.keyword = view("filename");
  filename.value = view(":memory:");
  config.driver = view("sqlite");
  config.options = &filename;
  config.option_count = 1u;
  check_equal(orm_connect(&config, &connection, error), ORM_STATUS_OK);
  check_not_null(connection);
  return connection;
}

spec("Driver host lifetime bridge uses the native query owner") {
  it("acquires the exact query execution lease from the host plan token") {
    orm_error_t error;
    orm_connection_t *connection;
    orm_query_t *query = NULL;
    void *lease = NULL;
    const orm_driver_lifetime_ops_v1 *lifetime =
        orm_driver_owner_services_v1();

    orm_error_init(&error);
    connection = open_sqlite(&error);
    check_equal(orm_raw(connection, view("select 1"), &query, &error),
                ORM_STATUS_OK);
    check_not_null(query);
    check_not_null(lifetime);
    check_equal(lifetime->header.abi_version, ORM_DRIVER_ABI_VERSION);
    check_equal(lifetime->acquire(&query->plan, &lease, &error),
                ORM_STATUS_OK);
    check_true(lease == query);

    check_equal(orm_query_close(query, &error), ORM_STATUS_BUSY);
    check_equal(orm_query_set_limit(query, 1u, &error), ORM_STATUS_BUSY);
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_BUSY);

    lifetime->release(lease);
    lease = NULL;
    check_equal(orm_query_set_limit(query, 1u, &error), ORM_STATUS_OK);
    check_equal(orm_query_close(query, &error), ORM_STATUS_OK);
    orm_query_release(query);
    query = NULL;
    check_equal(orm_connection_close(connection, &error), ORM_STATUS_OK);
    orm_connection_release(connection);
  }

  it("fails without publishing a lease for invalid host tokens") {
    orm_error_t error;
    void *lease = (void *)(uintptr_t)1u;
    const orm_driver_lifetime_ops_v1 *lifetime =
        orm_driver_owner_services_v1();

    orm_error_init(&error);
    check_equal(lifetime->acquire(NULL, &lease, &error),
                ORM_STATUS_INVALID_ARGUMENT);
    check_null(lease);
    check_equal(error.status, ORM_STATUS_INVALID_ARGUMENT);
    check_equal(lifetime->acquire(NULL, NULL, &error),
                ORM_STATUS_INVALID_ARGUMENT);
  }
}
