#include <orm_runtime.h>

#include <stdio.h>
#include <string.h>

static void matrix_stage(const char *stage) {
  fprintf(stderr, "sql-matrix: %s\n", stage);
  fflush(stderr);
}

static int load_driver(orm_runtime_t *runtime, const char *id,
                       const char *path, orm_error_t *error) {
  orm_driver_load_config_t load;
  orm_driver_info_t info;
  memset(&load, 0, sizeof(load));
  load.struct_size = (uint32_t)sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.module_path = orm_view(path);
  load.expected_driver_id = orm_view(id);
  matrix_stage("load-driver-before");
  if (orm_runtime_load_driver(runtime, &load, error) != ORM_STATUS_OK)
    return 0;
  matrix_stage("load-driver-after");
  memset(&info, 0, sizeof(info));
  if (orm_runtime_driver_info(runtime, orm_view(id), &info, error) !=
      ORM_STATUS_OK)
    return 0;
  return info.canonical_id_size == strlen(id) &&
         memcmp(info.canonical_id, id, info.canonical_id_size) == 0u;
}

static int exercise_sqlite(orm_runtime_t *runtime, orm_error_t *error) {
  orm_config_t config;
  orm_option_t filename;
  orm_connection_t *connection = NULL;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  int64_t value = 0;

  orm_config(&config);
  filename.keyword = orm_view("filename");
  filename.value = orm_view(":memory:");
  config.driver = orm_view("sqlite");
  config.options = &filename;
  config.option_count = 1u;

  matrix_stage("sqlite-connect-before");
  if (orm_runtime_connect(runtime, &config, &connection, error) !=
      ORM_STATUS_OK)
    return 0;
  matrix_stage("sqlite-connect-after");
  matrix_stage("sqlite-raw-before");
  if (orm_raw(connection, orm_view("select 41 + 1"), &query, error) !=
      ORM_STATUS_OK)
    goto fail;
  matrix_stage("sqlite-raw-after");
  matrix_stage("sqlite-execute-before");
  if (orm_query_execute(query, &result, error) != ORM_STATUS_OK)
    goto fail;
  matrix_stage("sqlite-execute-after");
  matrix_stage("sqlite-read-before");
  if (orm_result_get_int64(result, 0u, 0u, &value, error) != ORM_STATUS_OK ||
      value != INT64_C(42))
    goto fail;
  matrix_stage("sqlite-read-after");

  matrix_stage("sqlite-destroy-result");
  orm_result_destroy(result);
  matrix_stage("sqlite-destroy-query");
  orm_query_destroy(query);
  matrix_stage("sqlite-disconnect");
  orm_disconnect(connection);
  matrix_stage("sqlite-done");
  return 1;

fail:
  orm_result_destroy(result);
  orm_query_destroy(query);
  orm_disconnect(connection);
  return 0;
}

int main(int argc, char **argv) {
  orm_runtime_config_t config;
  orm_runtime_t *runtime = NULL;
  orm_error_t error;
  int use_sqlite;
  int use_postgresql;

  if (argc != 4) {
    fprintf(stderr,
            "usage: %s <sqlite|postgresql|both> <sqlite-plugin> <postgresql-plugin>\n",
            argc > 0 ? argv[0] : "orm_sql_plugin_matrix");
    return 2;
  }

  use_sqlite = strcmp(argv[1], "sqlite") == 0 ||
               strcmp(argv[1], "both") == 0;
  use_postgresql = strcmp(argv[1], "postgresql") == 0 ||
                   strcmp(argv[1], "both") == 0;
  if (!use_sqlite && !use_postgresql)
    return 3;
  if ((use_sqlite && argv[2][0] == '\0') ||
      (use_postgresql && argv[3][0] == '\0'))
    return 4;

  orm_runtime_config_init(&config);
  orm_error_init(&error);
  if (orm_runtime_create(&config, &runtime, &error) != ORM_STATUS_OK)
    return 5;

  if (use_sqlite && !load_driver(runtime, "sqlite", argv[2], &error)) {
    orm_runtime_release(runtime);
    return 6;
  }
  if (use_postgresql &&
      !load_driver(runtime, "postgresql", argv[3], &error)) {
    orm_runtime_release(runtime);
    return 7;
  }
  if (use_sqlite && !exercise_sqlite(runtime, &error)) {
    orm_runtime_release(runtime);
    return 8;
  }

  matrix_stage("runtime-close-before");
  if (orm_runtime_close(runtime, &error) != ORM_STATUS_OK) {
    orm_runtime_release(runtime);
    return 9;
  }
  matrix_stage("runtime-close-after");
  orm_runtime_release(runtime);
  matrix_stage("runtime-release-after");
  return 0;
}
