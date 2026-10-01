#include "orm.h"
#include <orm_runtime.h>

#include <cmeta/struct.h>
#include <tinytest.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "row_fixture.h"

static const char *orm_tides_plugin_path(void) {
  const char *path = getenv("ORM_TIDESDB_PLUGIN");
  return path != NULL ? path : "";
}

static orm_runtime_t *orm_tides_plugin_runtime(orm_error_t *error) {
  orm_runtime_config_t config;
  orm_runtime_t *runtime = NULL;
  orm_driver_load_config_t load;
  orm_driver_info_t info;
  orm_driver_storage_capabilities_v1 storage;

  orm_runtime_config_init(&config);
  check_equal(orm_runtime_create(&config, &runtime, error), ORM_STATUS_OK);
  check_not_null(runtime);

  memset(&load, 0, sizeof(load));
  load.struct_size = (uint32_t)sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.module_path = orm_view(orm_tides_plugin_path());
  load.expected_driver_id = orm_view("tidesdb");
  {
    const orm_status_t load_status =
        orm_runtime_load_driver(runtime, &load, error);
    if (load_status != ORM_STATUS_OK) {
      (void)fprintf(stderr, "TidesDB Plugin load failed: status=%d message=%s\n",
                    (int)load_status, error->message);
      (void)orm_runtime_close(runtime, error);
      orm_runtime_release(runtime);
      runtime = NULL;
    }
    check_equal(load_status, ORM_STATUS_OK);
    if (load_status != ORM_STATUS_OK)
      return NULL;
  }

  memset(&info, 0, sizeof(info));
  check_equal(
      orm_runtime_driver_info(runtime, orm_view("tidesdb"), &info, error),
      ORM_STATUS_OK);
  check_equal(info.canonical_id_size, 7u);
  check_equal(memcmp(info.canonical_id, "tidesdb", 7u), 0);
  check_true((info.capabilities & ORM_DRIVER_CAP_SELECT) != 0u);
  check_true((info.capabilities & ORM_DRIVER_CAP_INSERT) != 0u);
  check_true((info.capabilities & ORM_DRIVER_CAP_UPDATE) != 0u);
  check_true((info.capabilities & ORM_DRIVER_CAP_DELETE) != 0u);
  check_true((info.capabilities & ORM_DRIVER_CAP_TRANSACTION) != 0u);
  check_true((info.capabilities & ORM_DRIVER_CAP_SAVEPOINT) != 0u);
  check_true((info.capabilities & ORM_DRIVER_CAP_INCREMENTAL_ROWS) != 0u);
  check_true((info.capabilities & ORM_DRIVER_CAP_SERIALIZABLE) != 0u);
  check_true((info.capabilities & ORM_DRIVER_CAP_RAW_SQL) != 0u);

  memset(&storage, 0, sizeof(storage));
  check_equal(orm_runtime_driver_storage_info(
                  runtime, orm_view("tidesdb"), &storage, error),
              ORM_STATUS_OK);
  check_true(orm_driver_storage_capabilities_valid(&storage));
  check_equal(
      storage.capabilities,
      (uint64_t)(ORM_DRIVER_STORAGE_CAP_ATOMIC_STATE_METADATA |
                 ORM_DRIVER_STORAGE_CAP_ORDERED_REPLAY_CLASSIFICATION |
                 ORM_DRIVER_STORAGE_CAP_FILE_BACKED_CHECKPOINT |
                 ORM_DRIVER_STORAGE_CAP_STAGED_RESTORE));
  check_equal(storage.max_progress_metadata_bytes,
              (uint64_t)ORM_DRIVER_STORAGE_LIMIT_CONFIGURED);
  check_equal(storage.max_batch_operations, (uint64_t)0u);
  check_equal(storage.max_batch_bytes, (uint64_t)0u);
  check_equal(storage.max_checkpoint_chunk_bytes, (uint64_t)0u);
  check_equal(storage.max_restore_chunk_bytes, (uint64_t)0u);
  return runtime;
}

spec("TidesDB public reactive C facade") {
  (void)ttest_config__;
  it("executes a demanded command and streams the stored typed row") {
    char *path = tt_make_temp_dir("orm-tides-public-flow");
    orm_option_t options[4];
    orm_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_connection_t *connection = NULL;
    orm_query_t *query = NULL;
    orm_error_t error;
    cflow_publisher source = {0};
    orm_command_result_t command = ORM_COMMAND_RESULT_INIT;
    orm_flow_config_t flow_config;
    orm_tides_public_row row = {0};
    cflow_step step;

    check_not_null(path);
    if (path == NULL) return;

    orm_config(&config);
    options[0] = (orm_option_t){orm_view("path"), orm_view(path)};
    options[1] =
        (orm_option_t){orm_view("column_family"), orm_view("orm_cflow")};
    options[2] =
        (orm_option_t){orm_view("max_scan_rows"), orm_view("128")};
    options[3] =
        (orm_option_t){orm_view("max_scan_bytes"), orm_view("1048576")};
    config.driver = orm_view("tidesdb");
    config.options = options;
    config.option_count = 4u;
    orm_error_init(&error);
    check_true(orm_tides_plugin_path()[0] != '\0');
    runtime = orm_tides_plugin_runtime(&error);

    check_equal(orm_runtime_connect(runtime, &config, &connection, &error),
                ORM_STATUS_OK);
    check_equal(orm_insert(connection, orm_view("people"), &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(query, orm_view("id"), orm_i64(7), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(query, orm_view("score"), orm_i64(19), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow(query, &source, &error),
                ORM_STATUS_OK);
    step = cflow_publisher_resume(&source, NULL, &command);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(command.affected_rows, (uint64_t)1u);
    cflow_publisher_destroy(&source);
    source = (cflow_publisher){0};
    orm_query_destroy(query);
    query = NULL;

    check_equal(orm_query_create(connection, orm_view("people"), &query,
                                 &error),
                ORM_STATUS_OK);
    check_equal(orm_query_add_column(query, orm_view("id"), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_add_column(query, orm_view("score"), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_where(query, orm_view("id"), ORM_COMPARE_EQUAL,
                                orm_i64(7), &error),
                ORM_STATUS_OK);
    orm_flow_config(&flow_config, &orm_tides_public_row_data);
    check_equal(orm_query_open_flow(query, &flow_config, &source, &error),
                ORM_STATUS_OK);
    step = cflow_publisher_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(row.id, 7L);
    check_equal(row.score, 19L);
    step = cflow_publisher_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_DONE);

    cflow_publisher_destroy(&source);
    orm_query_destroy(query);
    orm_disconnect(connection);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
    check_equal(tt_remove_tree(path), 0);
    free(path);
  }

  it("updates, deletes, and commits through the pure C transaction backend") {
    char *path = tt_make_temp_dir("orm-tides-public-crud");
    orm_option_t options[2];
    orm_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_connection_t *connection = NULL;
    orm_transaction_t *transaction = NULL;
    orm_query_t *query = NULL;
    orm_error_t error;
    cflow_publisher source = {0};
    orm_command_result_t command = ORM_COMMAND_RESULT_INIT;
    orm_flow_config_t flow_config;
    orm_tides_public_row row = {0};
    cflow_step step;

    check_not_null(path);
    if (path == NULL) return;
    orm_config(&config);
    options[0] = (orm_option_t){orm_view("path"), orm_view(path)};
    options[1] = (orm_option_t){orm_view("column_family"),
                                orm_view("orm_crud")};
    config.driver = orm_view("tidesdb");
    config.options = options;
    config.option_count = 2u;
    orm_error_init(&error);
    check_true(orm_tides_plugin_path()[0] != '\0');
    runtime = orm_tides_plugin_runtime(&error);
    check_equal(orm_runtime_connect(runtime, &config, &connection, &error),
                ORM_STATUS_OK);

    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                      &transaction, &error), ORM_STATUS_OK);
    check_equal(orm_insert(connection, orm_view("people"), &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(query, orm_view("id"), orm_i64(9), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(query, orm_view("score"), orm_i64(10), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow_in_transaction(
                    query, transaction, &source, &error), ORM_STATUS_OK);
    step = cflow_publisher_resume(&source, NULL, &command);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(command.affected_rows, (uint64_t)1u);
    cflow_publisher_destroy(&source);
    source = (cflow_publisher){0};
    orm_query_destroy(query);
    query = NULL;

    check_equal(orm_query_create(connection, orm_view("people"), &query,
                                 &error), ORM_STATUS_OK);
    check_equal(orm_query_add_column(query, orm_view("id"), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_add_column(query, orm_view("score"), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_where(query, orm_view("id"), ORM_COMPARE_EQUAL,
                                orm_i64(9), &error), ORM_STATUS_OK);
    orm_flow_config(&flow_config, &orm_tides_public_row_data);
    check_equal(orm_query_open_flow_in_transaction(
                    query, transaction, &flow_config, &source, &error),
                ORM_STATUS_OK);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_BUSY);
    step = cflow_publisher_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(row.score, 10L);
    cflow_publisher_destroy(&source);
    source = (cflow_publisher){0};
    orm_query_destroy(query);
    query = NULL;
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    orm_transaction_destroy(transaction);
    transaction = NULL;

    check_equal(orm_update(connection, orm_view("people"), &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(query, orm_view("score"), orm_i64(20), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_where(query, orm_view("id"), ORM_COMPARE_EQUAL,
                                orm_i64(9), &error), ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow(query, &source, &error),
                ORM_STATUS_OK);
    command = (orm_command_result_t)ORM_COMMAND_RESULT_INIT;
    step = cflow_publisher_resume(&source, NULL, &command);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(command.affected_rows, (uint64_t)1u);
    cflow_publisher_destroy(&source);
    source = (cflow_publisher){0};
    orm_query_destroy(query);
    query = NULL;

    check_equal(orm_query_create(connection, orm_view("people"), &query,
                                 &error), ORM_STATUS_OK);
    check_equal(orm_query_add_column(query, orm_view("id"), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_add_column(query, orm_view("score"), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_where(query, orm_view("id"), ORM_COMPARE_EQUAL,
                                orm_i64(9), &error), ORM_STATUS_OK);
    orm_flow_config(&flow_config, &orm_tides_public_row_data);
    check_equal(orm_query_open_flow(query, &flow_config, &source, &error),
                ORM_STATUS_OK);
    step = cflow_publisher_resume(&source, NULL, &row);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(row.score, 20L);
    cflow_publisher_destroy(&source);
    source = (cflow_publisher){0};
    orm_query_destroy(query);
    query = NULL;

    check_equal(orm_delete(connection, orm_view("people"), &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_where(query, orm_view("id"), ORM_COMPARE_EQUAL,
                                orm_i64(9), &error), ORM_STATUS_OK);
    check_equal(orm_query_where(query, orm_view("score"), ORM_COMPARE_EQUAL,
                                orm_i64(20), &error), ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow(query, &source, &error),
                ORM_STATUS_OK);
    command = (orm_command_result_t)ORM_COMMAND_RESULT_INIT;
    step = cflow_publisher_resume(&source, NULL, &command);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(command.affected_rows, (uint64_t)1u);

    cflow_publisher_destroy(&source);
    orm_query_destroy(query);
    orm_disconnect(connection);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
    check_equal(tt_remove_tree(path), 0);
    free(path);
  }
}
