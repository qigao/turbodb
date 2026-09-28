#include <orm_tidesdb.h>

#include <salts_fs.h>
#include <cmeta/struct.h>
#include <tinytest.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TIDES_MAINT_DATA_PREFIX_SIZE                                       \
  (offsetof(cmeta_data_desc, shape) +                                      \
   sizeof(((cmeta_data_desc *)0)->shape))

Struct(tides_maint_row, (long, id), (long, score));

static const cmeta_type_identity tides_maint_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.TidesMaintenanceRow");
static const cmeta_type_traits tides_maint_row_traits = {
    CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_desc tides_maint_row_type = {
    "tides_maint_row", sizeof(tides_maint_row), _Alignof(tides_maint_row),
    CMETA_T_OBJECT, NULL, &tides_maint_row_traits, &tides_maint_row_identity};
static const cmeta_data_field_desc tides_maint_row_fields[] = {
    {"orm.test.TidesMaintenanceRow.id", "id", offsetof(tides_maint_row, id),
     &cmeta_data_long},
    {"orm.test.TidesMaintenanceRow.score", "score",
     offsetof(tides_maint_row, score), &cmeta_data_long}};
static const cmeta_data_struct_shape tides_maint_row_shape = {
    StructMeta(tides_maint_row), tides_maint_row_fields, 2u};
static const cmeta_data_desc tides_maint_row_data = {
    TIDES_MAINT_DATA_PREFIX_SIZE, CMETA_DATA_DESC_ABI_VERSION,
    "orm.test.TidesMaintenanceRow.data", "TidesMaintenanceRow",
    CMETA_DATA_STRUCT, &tides_maint_row_type, &tides_maint_row_shape};

static const char *tides_plugin_path(void) {
  const char *path = getenv("ORM_TIDESDB_PLUGIN");
  return path != NULL ? path : "";
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
  load.module_path = orm_view(tides_plugin_path());
  load.expected_driver_id = orm_view("tidesdb");
  check_equal(orm_runtime_load_driver(runtime, &load, error), ORM_STATUS_OK);
  return runtime;
}

static orm_connection_t *open_generation(
    orm_runtime_t *runtime, const char *path, orm_error_t *error) {
  orm_option_t options[2];
  orm_config_t config;
  orm_connection_t *connection = NULL;

  orm_config(&config);
  options[0] = (orm_option_t){orm_view("path"), orm_view(path)};
  options[1] =
      (orm_option_t){orm_view("column_family"), orm_view("provider")};
  config.driver = orm_view("tidesdb");
  config.options = options;
  config.option_count = 2u;
  check_equal(orm_runtime_connect(runtime, &config, &connection, error),
              ORM_STATUS_OK);
  check_not_null(connection);
  return connection;
}

static void seed_generation(
    orm_runtime_t *runtime, const char *path, long score) {
  orm_error_t error;
  orm_connection_t *connection;
  orm_query_t *query = NULL;
  cflow_publisher source = {0};
  orm_command_result_t command = ORM_COMMAND_RESULT_INIT;
  cflow_step step;

  orm_error_init(&error);
  connection = open_generation(runtime, path, &error);
  check_equal(orm_insert(connection, orm_view("people"), &query, &error),
              ORM_STATUS_OK);
  check_equal(orm_query_set(query, orm_view("id"), orm_i64(1), &error),
              ORM_STATUS_OK);
  check_equal(orm_query_set(query, orm_view("score"), orm_i64(score), &error),
              ORM_STATUS_OK);
  check_equal(orm_query_open_command_flow(query, &source, &error),
              ORM_STATUS_OK);
  step = cflow_publisher_resume(&source, NULL, &command);
  check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
  check_equal(command.affected_rows, (uint64_t)1u);
  cflow_publisher_destroy(&source);
  orm_query_destroy(query);
  orm_disconnect(connection);
}

static long read_generation_connection(orm_connection_t *connection) {
  orm_error_t error;
  orm_query_t *query = NULL;
  cflow_publisher source = {0};
  orm_flow_config_t flow_config;
  tides_maint_row row = {0};
  cflow_step step;

  orm_error_init(&error);
  check_equal(orm_query_create(connection, orm_view("people"), &query, &error),
              ORM_STATUS_OK);
  check_equal(orm_query_add_column(query, orm_view("id"), &error),
              ORM_STATUS_OK);
  check_equal(orm_query_add_column(query, orm_view("score"), &error),
              ORM_STATUS_OK);
  check_equal(orm_query_where(query, orm_view("id"), ORM_COMPARE_EQUAL,
                              orm_i64(1), &error), ORM_STATUS_OK);
  orm_flow_config(&flow_config, &tides_maint_row_data);
  check_equal(orm_query_open_flow(query, &flow_config, &source, &error),
              ORM_STATUS_OK);
  step = cflow_publisher_resume(&source, NULL, &row);
  check_equal(step.kind, CFLOW_STEP_VALUE);
  cflow_publisher_destroy(&source);
  orm_query_destroy(query);
  return row.score;
}

static void join_path(char *out, size_t out_size,
                      const char *base, const char *child) {
  check_equal(salts_fs_path_join(out, out_size, base, child), 0);
}

static void prepare_layout(
    const char *root, char *generations, size_t generations_size) {
  join_path(generations, generations_size, root, "generations");
  check_equal(salts_fs_mkdir(generations, 0755), 0);
}

static void create_generation_dir(
    const char *generations, const char *id,
    char *path, size_t path_size) {
  join_path(path, path_size, generations, id);
  check_equal(salts_fs_mkdir(path, 0755), 0);
}

static orm_tidesdb_generation_publish_result publish_generation(
    orm_runtime_t *runtime, const char *root, const char *id,
    orm_error_t *error) {
  orm_tidesdb_generation_publish_config config;
  orm_tidesdb_generation_publish_result result;
  orm_tidesdb_generation_publish_config_init(&config);
  orm_tidesdb_generation_publish_result_init(&result);
  config.provider_root = orm_view(root);
  config.generation_id = orm_view(id);
  check_equal(orm_runtime_tidesdb_publish_generation(
                  runtime, orm_view("tidesdb"), &config, &result, error),
              ORM_STATUS_OK);
  return result;
}

static orm_tidesdb_active_result resolve_active(
    orm_runtime_t *runtime, const char *root, orm_error_t *error) {
  orm_tidesdb_active_resolve_config config;
  orm_tidesdb_active_result result;
  orm_tidesdb_active_resolve_config_init(&config);
  orm_tidesdb_active_result_init(&result);
  config.provider_root = orm_view(root);
  check_equal(orm_runtime_tidesdb_resolve_active(
                  runtime, orm_view("tidesdb"), &config, &result, error),
              ORM_STATUS_OK);
  return result;
}

spec("TidesDB staged generation publication") {
  it("switches future ACTIVE resolution while old generation handles remain live") {
    char *root = tt_make_temp_dir("orm-tides-provider");
    char generations[1024];
    char g1_path[1024];
    char g2_path[1024];
    char resolved_path[1024];
    orm_error_t error;
    orm_runtime_t *runtime;
    orm_driver_storage_capabilities_v1 storage;
    orm_tidesdb_generation_publish_result published;
    orm_tidesdb_active_result active;
    orm_connection_t *old_connection;
    orm_connection_t *new_connection;

    check_not_null(root);
    if (root == NULL) return;
    orm_error_init(&error);
    runtime = open_runtime(&error);
    prepare_layout(root, generations, sizeof(generations));
    create_generation_dir(generations, "g1", g1_path, sizeof(g1_path));
    create_generation_dir(generations, "g2", g2_path, sizeof(g2_path));
    seed_generation(runtime, g1_path, 11L);
    seed_generation(runtime, g2_path, 22L);

    memset(&storage, 0, sizeof(storage));
    check_equal(orm_runtime_driver_storage_info(
                    runtime, orm_view("tidesdb"), &storage, &error),
                ORM_STATUS_OK);
    check_true((storage.capabilities &
                ORM_DRIVER_STORAGE_CAP_STAGED_RESTORE) != 0u);
    check_equal(storage.max_restore_chunk_bytes, (uint64_t)0u);

    published = publish_generation(runtime, root, "g1", &error);
    check_equal(published.publication_state,
                ORM_TIDESDB_PUBLICATION_PUBLISHED_DURABLE);
    active = resolve_active(runtime, root, &error);
    check_equal(active.generation_size, 2u);
    check_equal(memcmp(active.generation, "g1", 2u), 0);

    old_connection = open_generation(runtime, g1_path, &error);
    check_equal(read_generation_connection(old_connection), 11L);

    published = publish_generation(runtime, root, "g2", &error);
    check_equal(published.publication_state,
                ORM_TIDESDB_PUBLICATION_PUBLISHED_DURABLE);
    active = resolve_active(runtime, root, &error);
    check_equal(active.generation_size, 2u);
    check_equal(memcmp(active.generation, "g2", 2u), 0);

    check_equal(read_generation_connection(old_connection), 11L);
    join_path(resolved_path, sizeof(resolved_path),
              generations, active.generation);
    new_connection = open_generation(runtime, resolved_path, &error);
    check_equal(read_generation_connection(new_connection), 22L);

    orm_disconnect(new_connection);
    orm_disconnect(old_connection);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
    check_equal(tt_remove_tree(root), 0);
    free(root);
  }

  it("rejects an unprepared generation without changing ACTIVE") {
    char *root = tt_make_temp_dir("orm-tides-provider-reject");
    char generations[1024];
    char g1_path[1024];
    orm_error_t error;
    orm_runtime_t *runtime;
    orm_tidesdb_generation_publish_config config;
    orm_tidesdb_generation_publish_result result;
    orm_tidesdb_active_result active;

    check_not_null(root);
    if (root == NULL) return;
    orm_error_init(&error);
    runtime = open_runtime(&error);
    prepare_layout(root, generations, sizeof(generations));
    create_generation_dir(generations, "g1", g1_path, sizeof(g1_path));
    seed_generation(runtime, g1_path, 31L);
    (void)publish_generation(runtime, root, "g1", &error);

    orm_tidesdb_generation_publish_config_init(&config);
    orm_tidesdb_generation_publish_result_init(&result);
    config.provider_root = orm_view(root);
    config.generation_id = orm_view("missing");
    check_not_equal(orm_runtime_tidesdb_publish_generation(
                        runtime, orm_view("tidesdb"), &config, &result, &error),
                    ORM_STATUS_OK);
    check_equal(result.publication_state,
                ORM_TIDESDB_PUBLICATION_NOT_PUBLISHED);

    active = resolve_active(runtime, root, &error);
    check_equal(active.generation_size, 2u);
    check_equal(memcmp(active.generation, "g1", 2u), 0);

    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
    check_equal(tt_remove_tree(root), 0);
    free(root);
  }

  it("holds the Plugin/runtime lease while maintenance binding is acquired") {
    orm_error_t error;
    orm_runtime_t *runtime;
    orm_runtime_driver_extension_t *extension = NULL;
    void *binding = NULL;

    orm_error_init(&error);
    runtime = open_runtime(&error);
    check_equal(orm_runtime_driver_acquire_extension(
                    runtime, orm_view("tidesdb"),
                    orm_view(ORM_TIDESDB_MAINTENANCE_EXPORT_ID),
                    orm_view(ORM_TIDESDB_MAINTENANCE_CONTRACT_ID),
                    ORM_TIDESDB_MAINTENANCE_CONTRACT_VERSION,
                    TurboDb_TidesMaintenance_interface(),
                    &extension, &binding, &error),
                ORM_STATUS_OK);
    check_not_null(extension);
    check_not_null(binding);
    check_true(TurboDb_TidesMaintenance_valid(
        (TurboDb_TidesMaintenance *)binding));
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);
    check_equal(orm_runtime_driver_release_extension(extension, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }
}
