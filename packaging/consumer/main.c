#include <orm_runtime.h>
#include <turbodb_mysql.h>
#include <stdio.h>

enum { MODULE_PATH_CAPACITY = 4096 };

int main(void) {
  mysql_session_error_t mysql_error = {0};
  if (mysql_session_connect_and_ping(NULL, &mysql_error) != MYSQL_SESSION_INVALID) {
    fprintf(stderr, "installed MySQL client: invalid config was accepted\n");
    return 1;
  }
  const char *drivers[] = {"sqlite", "postgresql", "mysql", "redis", "tidesdb"};
  orm_runtime_config_t runtime_config;
  orm_runtime_t *runtime = NULL;
  orm_connection_t *connection = NULL;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_error_t error;
  int exit_status = 1;
  orm_error_init(&error);
  orm_runtime_config_init(&runtime_config);
  if (orm_runtime_create(&runtime_config, &runtime, &error) != ORM_STATUS_OK)
    goto cleanup;
  for (size_t i = 0; i < sizeof(drivers) / sizeof(drivers[0]); ++i) {
    char path[MODULE_PATH_CAPACITY];
    const int length = snprintf(path, sizeof(path), "%s/turbodb_driver_%s%s",
                                SDK_DRIVER_DIR, drivers[i], SDK_MODULE_SUFFIX);
    if (length < 0 || (size_t)length >= sizeof(path)) {
      fprintf(stderr, "installed module path exceeds capacity\n");
      goto cleanup;
    }
    orm_driver_load_config_t load = {0};
    load.struct_size = sizeof(load);
    load.abi_version = ORM_RUNTIME_ABI_VERSION;
    load.module_path = orm_view(path);
    load.expected_driver_id = orm_view(drivers[i]);
    if (orm_runtime_load_driver(runtime, &load, &error) != ORM_STATUS_OK) {
      fprintf(stderr, "load installed driver: %s\n", path);
      goto cleanup;
    }
  }
  orm_config_t config;
  orm_config(&config);
  const orm_option_t option = {orm_view("filename"), orm_view(":memory:")};
  config.driver = orm_view("sqlite");
  config.options = &option;
  config.option_count = 1u;
  if (orm_runtime_connect(runtime, &config, &connection, &error) != ORM_STATUS_OK)
    goto cleanup;
  if (orm_raw(connection, orm_view("CREATE TABLE package_probe(id INTEGER)"),
              &query, &error) != ORM_STATUS_OK)
    goto cleanup;
  if (orm_query_execute(query, &result, &error) != ORM_STATUS_OK)
    goto cleanup;
  exit_status = 0;
cleanup:
  if (exit_status != 0) fprintf(stderr, "installed SDK: %s\n", error.message);
  orm_result_destroy(result);
  orm_query_destroy(query);
  orm_connection_release(connection);
  if (runtime != NULL) {
    if (orm_runtime_close(runtime, &error) != ORM_STATUS_OK) {
      fprintf(stderr, "installed SDK close: %s\n", error.message);
      exit_status = 1;
    }
    orm_runtime_release(runtime);
  }
  return exit_status;
}
