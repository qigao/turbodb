#include <orm_sqlite.h>

#include <tinytest.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *sqlite_plugin_path(void) {
  const char *path = getenv("ORM_SQLITE_PLUGIN");
  return path != NULL ? path : "";
}

static int file_exists(const char *path) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) return 0;
  (void)fclose(file);
  return 1;
}

static char *unused_path(const char *prefix) {
  char *path = tt_make_temp_file(prefix, ".db");
  if (path != NULL) check_equal(tt_remove_file(path), 0);
  return path;
}

static orm_runtime_t *open_runtime(orm_error_t *error) {
  orm_runtime_config_t config;
  orm_runtime_t *runtime = NULL;
  orm_driver_load_config_t load;

  orm_runtime_config_init(&config);
  check_equal(orm_runtime_create(&config, &runtime, error), ORM_STATUS_OK);
  check_not_null(runtime);

  memset(&load, 0, sizeof(load));
  load.struct_size = (uint32_t)sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.module_path = orm_view(sqlite_plugin_path());
  load.expected_driver_id = orm_view("sqlite");
  check_equal(orm_runtime_load_driver(runtime, &load, error), ORM_STATUS_OK);
  return runtime;
}

static void close_runtime(orm_runtime_t *runtime) {
  orm_error_t error;
  orm_error_init(&error);
  check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
  orm_runtime_release(runtime);
}

static orm_connection_t *open_database(
    orm_runtime_t *runtime, const char *path, orm_error_t *error) {
  orm_config_t config;
  orm_option_t filename;
  orm_connection_t *connection = NULL;

  orm_config(&config);
  filename.keyword = orm_view("filename");
  filename.value = orm_view(path);
  config.driver = orm_view("sqlite");
  config.options = &filename;
  config.option_count = 1u;
  check_equal(orm_runtime_connect(runtime, &config, &connection, error),
              ORM_STATUS_OK);
  check_not_null(connection);
  return connection;
}

static void execute_sql(
    orm_connection_t *connection, const char *sql, orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  check_equal(orm_raw(connection, orm_view(sql), &query, error), ORM_STATUS_OK);
  check_not_null(query);
  check_equal(orm_query_execute(query, &result, error), ORM_STATUS_OK);
  check_not_null(result);
  orm_result_destroy(result);
  orm_query_destroy(query);
}

static void seed_database(
    orm_runtime_t *runtime, const char *path, int value) {
  char sql[128];
  orm_error_t error;
  orm_error_init(&error);
  orm_connection_t *connection = open_database(runtime, path, &error);
  execute_sql(connection,
              "create table state(id integer primary key,value integer not null)",
              &error);
  (void)snprintf(sql, sizeof(sql), "insert into state values(1,%d)", value);
  execute_sql(connection, sql, &error);
  orm_disconnect(connection);
}

static void update_database(
    orm_runtime_t *runtime, const char *path, int value) {
  char sql[128];
  orm_error_t error;
  orm_error_init(&error);
  orm_connection_t *connection = open_database(runtime, path, &error);
  (void)snprintf(sql, sizeof(sql), "update state set value=%d where id=1", value);
  execute_sql(connection, sql, &error);
  orm_disconnect(connection);
}

static int64_t read_database(orm_runtime_t *runtime, const char *path) {
  orm_error_t error;
  orm_connection_t *connection;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  int64_t value = 0;

  orm_error_init(&error);
  connection = open_database(runtime, path, &error);
  check_equal(orm_raw(connection, orm_view("select value from state where id=1"),
                      &query, &error), ORM_STATUS_OK);
  check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
  check_not_null(result);
  check_equal(orm_result_get_int64(result, 0u, 0u, &value, &error),
              ORM_STATUS_OK);
  orm_result_destroy(result);
  orm_query_destroy(query);
  orm_disconnect(connection);
  return value;
}

static orm_sqlite_file_copy_config copy_config(
    const char *source, const char *staging, const char *destination) {
  orm_sqlite_file_copy_config config;
  orm_sqlite_file_copy_config_init(&config);
  config.source_path = orm_view(source);
  config.staging_path = orm_view(staging);
  config.destination_path = orm_view(destination);
  config.pages_per_step = 8u;
  config.max_busy_retries = 16u;
  return config;
}

static void cleanup3(char *a, char *b, char *c) {
  char *paths[] = {a, b, c};
  for (size_t i = 0u; i < 3u; ++i) {
    if (paths[i] != NULL) {
      (void)tt_remove_file(paths[i]);
      free(paths[i]);
    }
  }
}

spec("SQLite plugin maintenance checkpoint and restore") {
  it("publishes a stable file-backed checkpoint through the plugin extension") {
    orm_error_t error;
    orm_error_init(&error);
    orm_runtime_t *runtime = open_runtime(&error);
    char *source = unused_path("orm-sqlite-checkpoint-source");
    char *staging = unused_path("orm-sqlite-checkpoint-stage");
    char *checkpoint = unused_path("orm-sqlite-checkpoint-final");
    check_not_null(source);
    check_not_null(staging);
    check_not_null(checkpoint);

    seed_database(runtime, source, 41);
    orm_sqlite_file_copy_config config =
        copy_config(source, staging, checkpoint);
    orm_sqlite_file_copy_result result;
    orm_sqlite_file_copy_result_init(&result);

    check_equal(orm_runtime_sqlite_checkpoint_create(
                    runtime, orm_view("sqlite"), &config, &result, &error),
                ORM_STATUS_OK);
    check_equal(result.publication_state,
                ORM_SQLITE_PUBLICATION_PUBLISHED_DURABLE);
    check_greater(result.pages_copied, UINT64_C(0));
    check_equal(file_exists(staging), 0);
    check_equal(read_database(runtime, checkpoint), INT64_C(41));

    update_database(runtime, source, 99);
    check_equal(read_database(runtime, source), INT64_C(99));
    check_equal(read_database(runtime, checkpoint), INT64_C(41));

    cleanup3(source, staging, checkpoint);
    close_runtime(runtime);
  }

  it("restores through staging and durably replaces an offline database") {
    orm_error_t error;
    orm_error_init(&error);
    orm_runtime_t *runtime = open_runtime(&error);
    char *source = unused_path("orm-sqlite-restore-source");
    char *checkpoint_stage = unused_path("orm-sqlite-checkpoint-stage");
    char *checkpoint = unused_path("orm-sqlite-restore-checkpoint");
    char *restore_stage = unused_path("orm-sqlite-restore-stage");
    char *destination = unused_path("orm-sqlite-restore-destination");

    seed_database(runtime, source, 77);
    seed_database(runtime, destination, 12);

    orm_sqlite_file_copy_config checkpoint_config =
        copy_config(source, checkpoint_stage, checkpoint);
    orm_sqlite_file_copy_result result;
    orm_sqlite_file_copy_result_init(&result);
    check_equal(orm_runtime_sqlite_checkpoint_create(
                    runtime, orm_view("sqlite"), &checkpoint_config,
                    &result, &error),
                ORM_STATUS_OK);

    orm_sqlite_file_copy_config restore_config =
        copy_config(checkpoint, restore_stage, destination);
    orm_sqlite_file_copy_result_init(&result);
    check_equal(orm_runtime_sqlite_restore_publish(
                    runtime, orm_view("sqlite"), &restore_config,
                    &result, &error),
                ORM_STATUS_OK);
    check_equal(result.publication_state,
                ORM_SQLITE_PUBLICATION_PUBLISHED_DURABLE);
    check_equal(file_exists(restore_stage), 0);
    check_equal(read_database(runtime, destination), INT64_C(77));

    cleanup3(source, checkpoint_stage, checkpoint);
    cleanup3(restore_stage, destination, NULL);
    close_runtime(runtime);
  }

  it("rejects corrupt restore input before publication") {
    orm_error_t error;
    orm_error_init(&error);
    orm_runtime_t *runtime = open_runtime(&error);
    char *corrupt = unused_path("orm-sqlite-corrupt");
    char *staging = unused_path("orm-sqlite-corrupt-stage");
    char *destination = unused_path("orm-sqlite-corrupt-destination");
    seed_database(runtime, destination, 19);

    FILE *file = fopen(corrupt, "wb");
    check_not_null(file);
    if (file != NULL) {
      static const char invalid[] = "not-a-sqlite-database";
      check_equal(fwrite(invalid, 1u, sizeof(invalid) - 1u, file),
                  sizeof(invalid) - 1u);
      check_equal(fclose(file), 0);
    }

    orm_sqlite_file_copy_config config =
        copy_config(corrupt, staging, destination);
    orm_sqlite_file_copy_result result;
    orm_sqlite_file_copy_result_init(&result);
    check_not_equal(orm_runtime_sqlite_restore_publish(
                        runtime, orm_view("sqlite"), &config, &result, &error),
                    ORM_STATUS_OK);
    check_equal(result.publication_state,
                ORM_SQLITE_PUBLICATION_NOT_PUBLISHED);
    check_equal(read_database(runtime, destination), INT64_C(19));

    cleanup3(corrupt, staging, destination);
    close_runtime(runtime);
  }

  it("rejects stale staging without replacing either file") {
    orm_error_t error;
    orm_error_init(&error);
    orm_runtime_t *runtime = open_runtime(&error);
    char *source = unused_path("orm-sqlite-stale-source");
    char *staging = tt_make_temp_file("orm-sqlite-stale-stage", ".db");
    char *destination = unused_path("orm-sqlite-stale-destination");
    seed_database(runtime, source, 55);
    seed_database(runtime, destination, 21);

    FILE *file = fopen(staging, "wb");
    check_not_null(file);
    if (file != NULL) {
      static const char marker[] = "stale";
      check_equal(fwrite(marker, 1u, sizeof(marker) - 1u, file),
                  sizeof(marker) - 1u);
      check_equal(fclose(file), 0);
    }

    orm_sqlite_file_copy_config config =
        copy_config(source, staging, destination);
    orm_sqlite_file_copy_result result;
    orm_sqlite_file_copy_result_init(&result);
    check_equal(orm_runtime_sqlite_checkpoint_create(
                    runtime, orm_view("sqlite"), &config, &result, &error),
                ORM_STATUS_INVALID_STATE);
    check_equal(result.publication_state,
                ORM_SQLITE_PUBLICATION_NOT_PUBLISHED);
    check_equal(read_database(runtime, destination), INT64_C(21));
    check_equal(file_exists(staging), 1);

    cleanup3(source, staging, destination);
    close_runtime(runtime);
  }
}
