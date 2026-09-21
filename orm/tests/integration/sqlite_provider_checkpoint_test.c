#include <orm.h>
#include <orm_sqlite.h>

#include "tinytest.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *sqlite_maintenance_unused_path(const char *prefix) {
  char *path = tt_make_temp_file(prefix, ".db");
  if (path != NULL)
    check_equal(tt_remove_file(path), 0);
  return path;
}

static orm_connection_t *sqlite_maintenance_open(const char *path,
                                                  orm_error_t *error) {
  orm_config_t config;
  orm_option_t filename;
  orm_connection_t *connection = NULL;

  orm_config(&config);
  filename.keyword = orm_view("filename");
  filename.value = orm_view(path);
  config.driver = orm_view("sqlite");
  config.options = &filename;
  config.option_count = 1u;

  check_equal(orm_connect(&config, &connection, error), ORM_STATUS_OK);
  check_not_null(connection);
  return connection;
}

static void sqlite_maintenance_execute(orm_connection_t *connection,
                                       const char *sql,
                                       orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;

  check_equal(orm_raw(connection, orm_view(sql), &query, error), ORM_STATUS_OK);
  check_not_null(query);
  check_equal(orm_query_execute(query, &result, error), ORM_STATUS_OK);
  check_not_null(result);

  orm_result_destroy(result);
  orm_query_destroy(query);
}

static void sqlite_maintenance_seed(const char *path, int value) {
  char sql[128];
  orm_error_t error;
  orm_connection_t *connection;

  orm_error_init(&error);
  connection = sqlite_maintenance_open(path, &error);
  sqlite_maintenance_execute(
      connection,
      "create table state(id integer primary key,value integer not null)",
      &error);
  (void)snprintf(sql, sizeof(sql), "insert into state values(1,%d)", value);
  sqlite_maintenance_execute(connection, sql, &error);
  orm_disconnect(connection);
}

static void sqlite_maintenance_update(const char *path, int value) {
  char sql[128];
  orm_error_t error;
  orm_connection_t *connection;

  orm_error_init(&error);
  connection = sqlite_maintenance_open(path, &error);
  (void)snprintf(sql, sizeof(sql), "update state set value=%d where id=1", value);
  sqlite_maintenance_execute(connection, sql, &error);
  orm_disconnect(connection);
}

static int64_t sqlite_maintenance_read(const char *path) {
  orm_error_t error;
  orm_connection_t *connection;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  int64_t value = 0;

  orm_error_init(&error);
  connection = sqlite_maintenance_open(path, &error);
  check_equal(orm_raw(connection, orm_view("select value from state where id=1"),
                      &query, &error),
              ORM_STATUS_OK);
  check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
  check_not_null(result);
  check_equal(orm_result_get_int64(result, 0u, 0u, &value, &error),
              ORM_STATUS_OK);

  orm_result_destroy(result);
  orm_query_destroy(query);
  orm_disconnect(connection);
  return value;
}

static orm_sqlite_file_copy_config sqlite_maintenance_config(
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

static void sqlite_maintenance_cleanup(char *a, char *b, char *c) {
  if (a != NULL) {
    (void)tt_remove_file(a);
    free(a);
  }
  if (b != NULL) {
    (void)tt_remove_file(b);
    free(b);
  }
  if (c != NULL) {
    (void)tt_remove_file(c);
    free(c);
  }
}

spec("SQLite external provider checkpoint and restore") {
  it("publishes a file-backed checkpoint from a stable SQLite snapshot") {
    char *source = sqlite_maintenance_unused_path("orm-sqlite-checkpoint-source");
    char *staging = sqlite_maintenance_unused_path("orm-sqlite-checkpoint-stage");
    char *checkpoint =
        sqlite_maintenance_unused_path("orm-sqlite-checkpoint-final");
    orm_sqlite_file_copy_config config;
    orm_sqlite_file_copy_result result;
    orm_error_t error;

    check_not_null(source);
    check_not_null(staging);
    check_not_null(checkpoint);
    sqlite_maintenance_seed(source, 41);

    config = sqlite_maintenance_config(source, staging, checkpoint);
    orm_sqlite_file_copy_result_init(&result);
    orm_error_init(&error);

    check_equal(orm_sqlite_checkpoint_create(&config, &result, &error),
                ORM_STATUS_OK);
    check_equal(result.publication_state,
                ORM_SQLITE_PUBLICATION_PUBLISHED_DURABLE);
    check_greater(result.pages_copied, UINT64_C(0));
    check_equal(tt_file_exists(staging), 0);
    check_equal(sqlite_maintenance_read(checkpoint), INT64_C(41));

    sqlite_maintenance_update(source, 99);
    check_equal(sqlite_maintenance_read(source), INT64_C(99));
    check_equal(sqlite_maintenance_read(checkpoint), INT64_C(41));

    sqlite_maintenance_cleanup(source, staging, checkpoint);
  }

  it("restores through staging and atomically replaces an offline database") {
    char *checkpoint =
        sqlite_maintenance_unused_path("orm-sqlite-restore-checkpoint");
    char *checkpoint_stage =
        sqlite_maintenance_unused_path("orm-sqlite-restore-checkpoint-stage");
    char *source = sqlite_maintenance_unused_path("orm-sqlite-restore-source");
    char *destination =
        sqlite_maintenance_unused_path("orm-sqlite-restore-destination");
    char *restore_stage =
        sqlite_maintenance_unused_path("orm-sqlite-restore-stage");
    orm_sqlite_file_copy_config checkpoint_config;
    orm_sqlite_file_copy_config restore_config;
    orm_sqlite_file_copy_result result;
    orm_error_t error;

    check_not_null(checkpoint);
    check_not_null(checkpoint_stage);
    check_not_null(source);
    check_not_null(destination);
    check_not_null(restore_stage);

    sqlite_maintenance_seed(source, 77);
    sqlite_maintenance_seed(destination, 12);

    checkpoint_config =
        sqlite_maintenance_config(source, checkpoint_stage, checkpoint);
    orm_sqlite_file_copy_result_init(&result);
    orm_error_init(&error);
    check_equal(
        orm_sqlite_checkpoint_create(&checkpoint_config, &result, &error),
        ORM_STATUS_OK);

    restore_config =
        sqlite_maintenance_config(checkpoint, restore_stage, destination);
    orm_sqlite_file_copy_result_init(&result);
    orm_error_init(&error);
    check_equal(orm_sqlite_restore_publish(&restore_config, &result, &error),
                ORM_STATUS_OK);
    check_equal(result.publication_state,
                ORM_SQLITE_PUBLICATION_PUBLISHED_DURABLE);
    check_equal(tt_file_exists(restore_stage), 0);
    check_equal(sqlite_maintenance_read(destination), INT64_C(77));

    sqlite_maintenance_cleanup(checkpoint, checkpoint_stage, source);
    sqlite_maintenance_cleanup(destination, restore_stage, NULL);
  }

  it("rejects a corrupt checkpoint before publishing over the destination") {
    char *corrupt =
        sqlite_maintenance_unused_path("orm-sqlite-restore-corrupt");
    char *staging =
        sqlite_maintenance_unused_path("orm-sqlite-restore-corrupt-stage");
    char *destination =
        sqlite_maintenance_unused_path("orm-sqlite-restore-corrupt-dest");
    orm_sqlite_file_copy_config config;
    orm_sqlite_file_copy_result result;
    orm_error_t error;
    FILE *file;

    check_not_null(corrupt);
    check_not_null(staging);
    check_not_null(destination);
    sqlite_maintenance_seed(destination, 19);

    file = fopen(corrupt, "wb");
    check_not_null(file);
    if (file != NULL) {
      static const char invalid[] = "not-a-sqlite-database";
      check_equal(fwrite(invalid, 1u, sizeof(invalid) - 1u, file),
                  sizeof(invalid) - 1u);
      check_equal(fclose(file), 0);
    }

    config = sqlite_maintenance_config(corrupt, staging, destination);
    orm_sqlite_file_copy_result_init(&result);
    orm_error_init(&error);
    check_not_equal(orm_sqlite_restore_publish(&config, &result, &error),
                    ORM_STATUS_OK);
    check_equal(result.publication_state,
                ORM_SQLITE_PUBLICATION_NOT_PUBLISHED);
    check_equal(sqlite_maintenance_read(destination), INT64_C(19));

    sqlite_maintenance_cleanup(corrupt, staging, destination);
  }

  it("rejects stale staging without overwriting either staging or final state") {
    char *source = sqlite_maintenance_unused_path("orm-sqlite-stale-source");
    char *staging = tt_make_temp_file("orm-sqlite-stale-stage", ".db");
    char *destination =
        sqlite_maintenance_unused_path("orm-sqlite-stale-destination");
    orm_sqlite_file_copy_config config;
    orm_sqlite_file_copy_result result;
    orm_error_t error;
    FILE *file;

    check_not_null(source);
    check_not_null(staging);
    check_not_null(destination);
    sqlite_maintenance_seed(source, 55);
    sqlite_maintenance_seed(destination, 21);

    file = fopen(staging, "wb");
    check_not_null(file);
    if (file != NULL) {
      static const char marker[] = "stale";
      check_equal(fwrite(marker, 1u, sizeof(marker) - 1u, file),
                  sizeof(marker) - 1u);
      check_equal(fclose(file), 0);
    }

    config = sqlite_maintenance_config(source, staging, destination);
    orm_sqlite_file_copy_result_init(&result);
    orm_error_init(&error);
    check_equal(orm_sqlite_checkpoint_create(&config, &result, &error),
                ORM_STATUS_INVALID_STATE);
    check_equal(result.publication_state,
                ORM_SQLITE_PUBLICATION_NOT_PUBLISHED);
    check_equal(sqlite_maintenance_read(destination), INT64_C(21));
    check_equal(tt_file_exists(staging), 1);

    sqlite_maintenance_cleanup(source, staging, destination);
  }
}
