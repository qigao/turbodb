#include <orm_tidesdb.h>

#include <cmeta/struct.h>
#include <salts_fs.h>
#include <tinytest.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TIDES_RESTORE_DATA_PREFIX_SIZE                                      \
  (offsetof(cmeta_data_desc, shape) +                                      \
   sizeof(((cmeta_data_desc *)0)->shape))

Struct(tides_restore_row, (long, id), (long, score));

static const cmeta_type_identity tides_restore_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.TidesRestoreRow");
static const cmeta_type_traits tides_restore_row_traits = {
    CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_desc tides_restore_row_type = {
    "tides_restore_row", sizeof(tides_restore_row),
    _Alignof(tides_restore_row), CMETA_T_OBJECT, NULL,
    &tides_restore_row_traits, &tides_restore_row_identity};
static const cmeta_data_field_desc tides_restore_row_fields[] = {
    {"orm.test.TidesRestoreRow.id", "id",
     offsetof(tides_restore_row, id), &cmeta_data_long},
    {"orm.test.TidesRestoreRow.score", "score",
     offsetof(tides_restore_row, score), &cmeta_data_long}};
static const cmeta_data_struct_shape tides_restore_row_shape = {
    StructMeta(tides_restore_row), tides_restore_row_fields, 2u};
static const cmeta_data_desc tides_restore_row_data = {
    TIDES_RESTORE_DATA_PREFIX_SIZE, CMETA_DATA_DESC_ABI_VERSION,
    "orm.test.TidesRestoreRow.data", "TidesRestoreRow",
    CMETA_DATA_STRUCT, &tides_restore_row_type,
    &tides_restore_row_shape};

static const char *tides_plugin_path(void) {
  const char *path = getenv("ORM_TIDESDB_PLUGIN");
  return path != NULL ? path : "";
}

static orm_runtime_t *open_runtime(orm_error_t *error) {
  orm_runtime_config_t config;
  orm_runtime_t *runtime = NULL;
  orm_driver_load_config_t load;

  orm_runtime_config_init(&config);
  check_equal(orm_runtime_create(&config, &runtime, error),
              ORM_STATUS_OK);
  check_not_null(runtime);
  memset(&load, 0, sizeof(load));
  load.struct_size = (uint32_t)sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.module_path = orm_view(tides_plugin_path());
  load.expected_driver_id = orm_view("tidesdb");
  check_equal(orm_runtime_load_driver(runtime, &load, error),
              ORM_STATUS_OK);
  return runtime;
}

static orm_connection_t *open_connection(
    orm_runtime_t *runtime, const char *path, orm_error_t *error) {
  orm_config_t config;
  orm_option_t options[2];
  orm_connection_t *connection = NULL;

  orm_config(&config);
  options[0] = (orm_option_t){orm_view("path"), orm_view(path)};
  options[1] =
      (orm_option_t){orm_view("column_family"), orm_view("restore_cf")};
  config.driver = orm_view("tidesdb");
  config.options = options;
  config.option_count = 2u;
  check_equal(orm_runtime_connect(
                  runtime, &config, &connection, error),
              ORM_STATUS_OK);
  check_not_null(connection);
  return connection;
}

static void insert_score(
    orm_connection_t *connection, long score, orm_error_t *error) {
  orm_query_t *query = NULL;
  cflow_publisher publisher = {0};
  orm_command_result_t command = ORM_COMMAND_RESULT_INIT;
  cflow_step step;

  check_equal(orm_insert(connection, orm_view("state"),
                         &query, error),
              ORM_STATUS_OK);
  check_equal(orm_query_set(
                  query, orm_view("id"), orm_i64(1), error),
              ORM_STATUS_OK);
  check_equal(orm_query_set(
                  query, orm_view("score"), orm_i64(score), error),
              ORM_STATUS_OK);
  check_equal(orm_query_open_command_flow(
                  query, &publisher, error),
              ORM_STATUS_OK);
  step = cflow_publisher_resume(&publisher, NULL, &command);
  check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
  check_equal(command.affected_rows, UINT64_C(1));
  cflow_publisher_destroy(&publisher);
  orm_query_destroy(query);
}

static long read_score(
    orm_connection_t *connection, orm_error_t *error) {
  orm_query_t *query = NULL;
  cflow_publisher publisher = {0};
  orm_flow_config_t flow_config;
  tides_restore_row row = {0};
  cflow_step step;

  check_equal(orm_query_create(
                  connection, orm_view("state"), &query, error),
              ORM_STATUS_OK);
  check_equal(orm_query_add_column(
                  query, orm_view("id"), error),
              ORM_STATUS_OK);
  check_equal(orm_query_add_column(
                  query, orm_view("score"), error),
              ORM_STATUS_OK);
  check_equal(orm_query_where(
                  query, orm_view("id"), ORM_COMPARE_EQUAL,
                  orm_i64(1), error),
              ORM_STATUS_OK);
  orm_flow_config(&flow_config, &tides_restore_row_data);
  check_equal(orm_query_open_flow(
                  query, &flow_config, &publisher, error),
              ORM_STATUS_OK);
  step = cflow_publisher_resume(&publisher, NULL, &row);
  check_equal(step.kind, CFLOW_STEP_VALUE);
  check_equal(row.id, 1L);
  step = cflow_publisher_resume(&publisher, NULL, &row);
  check_equal(step.kind, CFLOW_STEP_DONE);
  cflow_publisher_destroy(&publisher);
  orm_query_destroy(query);
  return row.score;
}

static void seed_source(
    orm_runtime_t *runtime, const char *path, long score) {
  orm_error_t error;
  orm_connection_t *connection;
  orm_error_init(&error);
  connection = open_connection(runtime, path, &error);
  insert_score(connection, score, &error);
  orm_disconnect(connection);
}

static orm_tidesdb_generation_config generation_config(
    const char *provider_root, const char *checkpoint_path,
    const char *generation_id) {
  orm_tidesdb_generation_config config;
  orm_tidesdb_generation_config_init(&config);
  config.provider_root = orm_view(provider_root);
  if (checkpoint_path != NULL)
    config.checkpoint_path = orm_view(checkpoint_path);
  if (generation_id != NULL)
    config.generation_id = orm_view(generation_id);
  return config;
}

static orm_tidesdb_active_result resolve_active(
    orm_runtime_t *runtime, const char *provider_root,
    orm_error_t *error) {
  orm_tidesdb_generation_config config =
      generation_config(provider_root, NULL, NULL);
  orm_tidesdb_active_result result;
  orm_tidesdb_active_result_init(&result);
  check_equal(orm_runtime_tidesdb_resolve_active(
                  runtime, orm_view("tidesdb"),
                  &config, &result, error),
              ORM_STATUS_OK);
  return result;
}

static void cleanup_tree(char *path) {
  if (path == NULL) return;
  (void)tt_remove_tree(path);
  free(path);
}

spec("TidesDB staged restore generation publication") {
  it("switches future opens while preserving an old open generation") {
    char *provider_root = tt_make_temp_dir("orm-tides-provider-root");
    char *source_one = tt_make_temp_dir("orm-tides-source-one");
    char *source_two = tt_make_temp_dir("orm-tides-source-two");
    orm_error_t error;
    orm_runtime_t *runtime;
    orm_tidesdb_active_result active;
    orm_tidesdb_restore_result restore;
    orm_tidesdb_generation_config config;
    orm_connection_t *old_connection = NULL;
    orm_connection_t *new_connection = NULL;
    orm_driver_storage_capabilities_v1 storage;

    check_not_null(provider_root);
    check_not_null(source_one);
    check_not_null(source_two);
    if (provider_root == NULL || source_one == NULL ||
        source_two == NULL) {
      cleanup_tree(provider_root);
      cleanup_tree(source_one);
      cleanup_tree(source_two);
      return;
    }

    orm_error_init(&error);
    runtime = open_runtime(&error);
    check_not_null(runtime);

    active = resolve_active(runtime, provider_root, &error);
    check_equal(active.found, 0u);

    seed_source(runtime, source_one, 100L);
    config = generation_config(
        provider_root, source_one, "g-000001");
    orm_tidesdb_restore_result_init(&restore);
    check_equal(orm_runtime_tidesdb_restore_publish(
                    runtime, orm_view("tidesdb"),
                    &config, &restore, &error),
                ORM_STATUS_OK);
    check_equal(restore.publication_state,
                ORM_TIDESDB_PUBLICATION_PUBLISHED_DURABLE);

    active = resolve_active(runtime, provider_root, &error);
    check_equal(active.found, 1u);
    check_equal(strcmp(active.generation_id, "g-000001"), 0);
    old_connection =
        open_connection(runtime, active.generation_path, &error);
    check_equal(read_score(old_connection, &error), 100L);

    seed_source(runtime, source_two, 200L);
    config = generation_config(
        provider_root, source_two, "g-000002");
    orm_tidesdb_restore_result_init(&restore);
    check_equal(orm_runtime_tidesdb_restore_publish(
                    runtime, orm_view("tidesdb"),
                    &config, &restore, &error),
                ORM_STATUS_OK);
    check_equal(restore.publication_state,
                ORM_TIDESDB_PUBLICATION_PUBLISHED_DURABLE);

    active = resolve_active(runtime, provider_root, &error);
    check_equal(active.found, 1u);
    check_equal(strcmp(active.generation_id, "g-000002"), 0);
    new_connection =
        open_connection(runtime, active.generation_path, &error);
    check_equal(read_score(new_connection, &error), 200L);

    /* Existing handles retain the immutable old generation. */
    check_equal(read_score(old_connection, &error), 100L);

    /* An existing generation can never be overwritten/reused. */
    orm_tidesdb_restore_result_init(&restore);
    check_equal(orm_runtime_tidesdb_restore_publish(
                    runtime, orm_view("tidesdb"),
                    &config, &restore, &error),
                ORM_STATUS_INVALID_STATE);
    check_equal(restore.publication_state,
                ORM_TIDESDB_PUBLICATION_NOT_PUBLISHED);
    active = resolve_active(runtime, provider_root, &error);
    check_equal(strcmp(active.generation_id, "g-000002"), 0);

    memset(&storage, 0, sizeof(storage));
    check_equal(orm_runtime_driver_storage_info(
                    runtime, orm_view("tidesdb"),
                    &storage, &error),
                ORM_STATUS_OK);
    check_true((storage.capabilities &
                ORM_DRIVER_STORAGE_CAP_STAGED_RESTORE) != 0u);
    check_equal(storage.max_restore_chunk_bytes,
                (uint64_t)ORM_DRIVER_STORAGE_LIMIT_CONFIGURED);

    orm_disconnect(new_connection);
    orm_disconnect(old_connection);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);

    cleanup_tree(provider_root);
    cleanup_tree(source_one);
    cleanup_tree(source_two);
  }

  it("rejects unsafe ACTIVE content without path traversal") {
    char *provider_root = tt_make_temp_dir("orm-tides-provider-invalid");
    char active_path[ORM_TIDESDB_GENERATION_PATH_MAX_BYTES + 1u];
    orm_error_t error;
    orm_runtime_t *runtime;
    orm_tidesdb_generation_config config;
    orm_tidesdb_active_result active;
    salts_fs_buf_t contents;
    static char invalid[] = "../escape";

    check_not_null(provider_root);
    if (provider_root == NULL) return;
    orm_error_init(&error);
    runtime = open_runtime(&error);
    check_equal(salts_fs_path_join(
                    active_path, sizeof(active_path),
                    provider_root, ORM_TIDESDB_ACTIVE_FILE_NAME),
                0);
    contents = salts_fs_buf_init(
        invalid, sizeof(invalid) - 1u);
    check_equal(salts_fs_write_file(active_path, &contents), 0);

    config = generation_config(provider_root, NULL, NULL);
    orm_tidesdb_active_result_init(&active);
    check_equal(orm_runtime_tidesdb_resolve_active(
                    runtime, orm_view("tidesdb"),
                    &config, &active, &error),
                ORM_STATUS_INVALID_STATE);
    check_equal(active.found, 0u);

    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
    cleanup_tree(provider_root);
  }
}
