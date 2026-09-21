#include <orm_runtime.h>

#include <tinytest.h>

#include <stdlib.h>
#include <string.h>

static const char *fixture_path(void) {
  const char *path = getenv("ORM_MODULE_FIXTURE");
  return path == NULL ? "" : path;
}

static orm_driver_load_config_t load_config(const char *id) {
  orm_driver_load_config_t config;
  memset(&config, 0, sizeof(config));
  config.struct_size = (uint32_t)sizeof(config);
  config.abi_version = ORM_RUNTIME_ABI_VERSION;
  config.module_path = orm_view(fixture_path());
  config.expected_driver_id = orm_view(id);
  return config;
}

spec("runtime driver registry") {
  it("loads validates initializes and publishes one explicit module atomically") {
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_error_t error;
    orm_driver_info_t info;
    orm_driver_load_config_t load = load_config("fixture");

    orm_runtime_config_init(&config);
    orm_error_init(&error);
    check_equal(config.execution.execution_model,
                ORM_DRIVER_EXEC_CALLER_BLOCKING);
    check_equal(orm_runtime_create(&config, &runtime, &error),
                ORM_STATUS_OK);
    check_not_null(runtime);

    check_equal(orm_runtime_load_driver(runtime, &load, &error),
                ORM_STATUS_OK);
    memset(&info, 0xa5, sizeof(info));
    check_equal(orm_runtime_driver_info(runtime, orm_view("fixture"),
                                        &info, &error),
                ORM_STATUS_OK);
    check_equal(info.canonical_id_size, 7u);
    check_equal(memcmp(info.canonical_id, "fixture", 7u), 0);
    check_equal(info.execution_models, ORM_DRIVER_EXEC_CALLER_BLOCKING);

    memset(&info, 0, sizeof(info));
    check_equal(orm_runtime_driver_info(runtime, orm_view("fixture-alias"),
                                        &info, &error),
                ORM_STATUS_OK);
    check_equal(info.canonical_id_size, 7u);
    check_equal(memcmp(info.canonical_id, "fixture", 7u), 0);

    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("rejects an ID mismatch without publishing a partial registration") {
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_error_t error;
    orm_driver_info_t info;
    orm_driver_load_config_t wrong = load_config("other");
    orm_driver_load_config_t right = load_config("fixture");

    orm_runtime_config_init(&config);
    check_equal(orm_runtime_create(&config, &runtime, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &wrong, &error),
                ORM_STATUS_DRIVER_ID_MISMATCH);
    check_equal(orm_runtime_driver_info(runtime, orm_view("fixture"),
                                        &info, &error),
                ORM_STATUS_DRIVER_NOT_REGISTERED);
    check_equal(orm_runtime_load_driver(runtime, &right, &error),
                ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("rejects duplicate canonical registration without replacing the first") {
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_error_t error;
    orm_driver_info_t info;
    orm_driver_load_config_t load = load_config("fixture");

    orm_runtime_config_init(&config);
    check_equal(orm_runtime_create(&config, &runtime, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &load, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &load, &error),
                ORM_STATUS_DRIVER_ALREADY_REGISTERED);
    check_equal(orm_runtime_driver_info(runtime, orm_view("fixture"),
                                        &info, &error),
                ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("enforces the configured bounded driver registry") {
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_error_t error;
    orm_driver_load_config_t load = load_config("fixture");

    orm_runtime_config_init(&config);
    config.max_drivers = 1u;
    check_equal(orm_runtime_create(&config, &runtime, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &load, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &load, &error),
                ORM_STATUS_LIMIT_EXCEEDED);
    orm_runtime_release(runtime);
  }
}
