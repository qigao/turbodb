#include "orm.h"
#include <orm_runtime.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *plugin_path(void) {
  const char *path = getenv("ORM_MONGODB_PLUGIN");
  return path != NULL ? path : "";
}

static int run_cycle(void) {
  orm_runtime_config_t runtime_config;
  orm_runtime_t *runtime = NULL;
  orm_driver_load_config_t load;
  orm_driver_info_t info;
  orm_config_t config;
  orm_option_t option;
  orm_connection_t *connection = NULL;
  orm_error_t error;
  orm_status_t status;
  const uint64_t required =
      ORM_DRIVER_CAP_SELECT | ORM_DRIVER_CAP_INSERT |
      ORM_DRIVER_CAP_UPDATE | ORM_DRIVER_CAP_DELETE |
      ORM_DRIVER_CAP_TRANSACTION | ORM_DRIVER_CAP_INCREMENTAL_ROWS |
      ORM_DRIVER_CAP_READ_UNCOMMITTED | ORM_DRIVER_CAP_READ_COMMITTED |
      ORM_DRIVER_CAP_REPEATABLE_READ | ORM_DRIVER_CAP_SNAPSHOT |
      ORM_DRIVER_CAP_SERIALIZABLE;
  const uint64_t forbidden =
      ORM_DRIVER_CAP_SAVEPOINT | ORM_DRIVER_CAP_RAW_SQL;

  orm_error_init(&error);
  orm_runtime_config_init(&runtime_config);
  if (orm_runtime_create(&runtime_config, &runtime, &error) != ORM_STATUS_OK ||
      runtime == NULL) {
    fprintf(stderr, "create MongoDB runtime failed: %s\n", error.message);
    return 1;
  }

  memset(&load, 0, sizeof(load));
  load.struct_size = (uint32_t)sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.module_path = orm_view(plugin_path());
  load.expected_driver_id = orm_view("mongodb");
  if (orm_runtime_load_driver(runtime, &load, &error) != ORM_STATUS_OK) {
    fprintf(stderr, "load MongoDB Driver failed: %s\n", error.message);
    orm_runtime_release(runtime);
    return 1;
  }

  memset(&info, 0, sizeof(info));
  if (orm_runtime_driver_info(runtime, orm_view("mongodb"), &info, &error) !=
          ORM_STATUS_OK ||
      info.canonical_id_size != 7u ||
      memcmp(info.canonical_id, "mongodb", 7u) != 0 ||
      (info.capabilities & required) != required ||
      (info.capabilities & forbidden) != 0u) {
    fprintf(stderr, "MongoDB Driver capability contract mismatch: %s\n",
            error.message);
    orm_runtime_release(runtime);
    return 1;
  }

  /* Reject before network I/O; this still enters the real Driver factory and
   * verifies runtime -> Plugin -> backend error mapping. */
  orm_config(&config);
  option = (orm_option_t){orm_view("unknown_option"), orm_view("x")};
  config.driver = orm_view("mongodb");
  config.options = &option;
  config.option_count = 1u;
  orm_error_init(&error);
  status = orm_runtime_connect(runtime, &config, &connection, &error);
  if (status != ORM_STATUS_INVALID_ARGUMENT || connection != NULL) {
    fprintf(stderr, "MongoDB Driver accepted invalid configuration: %d %s\n",
            (int)status, error.message);
    orm_disconnect(connection);
    orm_runtime_release(runtime);
    return 1;
  }

  orm_error_init(&error);
  if (orm_runtime_close(runtime, &error) != ORM_STATUS_OK) {
    fprintf(stderr, "close MongoDB Driver runtime failed: %s\n", error.message);
    orm_runtime_release(runtime);
    return 1;
  }
  orm_runtime_release(runtime);
  return 0;
}

int main(void) {
  if (plugin_path()[0] == '\0') {
    fprintf(stderr, "ORM_MONGODB_PLUGIN is not configured\n");
    return 1;
  }
  if (run_cycle() != 0)
    return 1;
  return run_cycle();
}
