#include <orm_runtime.h>

#include <tinytest.h>

#include <stdlib.h>
#include <string.h>

#define POSTGRESQL_PLUGIN_CAPABILITIES                                      \
  (ORM_DRIVER_CAP_SELECT | ORM_DRIVER_CAP_INSERT |                         \
   ORM_DRIVER_CAP_UPDATE | ORM_DRIVER_CAP_DELETE |                         \
   ORM_DRIVER_CAP_RAW_SQL | ORM_DRIVER_CAP_TRANSACTION |                   \
   ORM_DRIVER_CAP_SAVEPOINT | ORM_DRIVER_CAP_INCREMENTAL_ROWS |            \
   ORM_DRIVER_CAP_READ_COMMITTED | ORM_DRIVER_CAP_REPEATABLE_READ |        \
   ORM_DRIVER_CAP_SERIALIZABLE)

static const char *postgresql_plugin_path(void) {
  const char *path = getenv("ORM_POSTGRESQL_PLUGIN");
  return path != NULL ? path : "";
}

static orm_driver_load_config_t postgresql_load(const char *id) {
  orm_driver_load_config_t load;
  memset(&load, 0, sizeof(load));
  load.struct_size = (uint32_t)sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.module_path = orm_view(postgresql_plugin_path());
  load.expected_driver_id = orm_view(id);
  return load;
}

spec("PostgreSQL runtime Plugin contract") {
  it("loads the canonical Plugin and publishes exact capabilities") {
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_driver_info_t info;
    orm_error_t error;
    orm_driver_load_config_t load = postgresql_load("postgresql");

    check_true(postgresql_plugin_path()[0] != '\0');
    orm_runtime_config_init(&config);
    orm_error_init(&error);
    check_equal(orm_runtime_create(&config, &runtime, &error), ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &load, &error), ORM_STATUS_OK);

    memset(&info, 0, sizeof(info));
    check_equal(orm_runtime_driver_info(runtime, orm_view("postgresql"),
                                        &info, &error),
                ORM_STATUS_OK);
    check_equal(info.canonical_id_size, (uint32_t)10u);
    check_equal(memcmp(info.canonical_id, "postgresql", 10u), 0);
    check_equal(info.capabilities, (uint64_t)POSTGRESQL_PLUGIN_CAPABILITIES);
    check_equal(info.execution_models, ORM_DRIVER_EXEC_CALLER_BLOCKING);

    memset(&info, 0, sizeof(info));
    check_equal(orm_runtime_driver_info(runtime, orm_view("postgres"),
                                        &info, &error),
                ORM_STATUS_DRIVER_NOT_REGISTERED);

    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("rejects a noncanonical expected ID without leaving a Plugin loaded") {
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_error_t error;
    orm_driver_load_config_t alias = postgresql_load("postgres");
    orm_driver_load_config_t canonical = postgresql_load("postgresql");

    orm_runtime_config_init(&config);
    check_equal(orm_runtime_create(&config, &runtime, &error), ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &alias, &error),
                ORM_STATUS_DRIVER_ID_MISMATCH);
    check_equal(orm_runtime_load_driver(runtime, &canonical, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("fails connection validation without leaking the Plugin lease") {
    orm_runtime_config_t runtime_config;
    orm_runtime_t *runtime = NULL;
    orm_config_t config;
    orm_connection_t *connection = (orm_connection_t *)(uintptr_t)1u;
    orm_error_t error;
    orm_driver_load_config_t load = postgresql_load("postgresql");

    orm_runtime_config_init(&runtime_config);
    check_equal(orm_runtime_create(&runtime_config, &runtime, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &load, &error), ORM_STATUS_OK);

    orm_config(&config);
    config.driver = orm_view("postgresql");
    check_equal(orm_runtime_connect(runtime, &config, &connection, &error),
                ORM_STATUS_INVALID_ARGUMENT);
    check_null(connection);

    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }
}
