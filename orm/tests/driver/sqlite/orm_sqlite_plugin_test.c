#include <orm_runtime.h>
#include <orm_sqlite.h>
#include "../../support/async_query.h"

#include <cmeta/struct.h>
#include <tinytest.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SQLITE_PLUGIN_ROW_DATA_PREFIX_SIZE \
  (offsetof(cmeta_data_desc, shape) + sizeof(((cmeta_data_desc *)0)->shape))

Struct(sqlite_plugin_row,
    (int, id),
    (long, score)
);

static const cmeta_type_identity sqlite_plugin_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.SqlitePluginRow");
static const cmeta_type_traits sqlite_plugin_row_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_desc sqlite_plugin_row_type = {
    .name = "sqlite_plugin_row",
    .size = sizeof(sqlite_plugin_row),
    .align = _Alignof(sqlite_plugin_row),
    .kind = CMETA_T_OBJECT,
    .traits = &sqlite_plugin_row_traits,
    .identity = &sqlite_plugin_row_identity};
static const cmeta_data_field_desc sqlite_plugin_row_fields[] = {
    {"orm.test.SqlitePluginRow.id", "id", offsetof(sqlite_plugin_row, id),
     &cmeta_data_int},
    {"orm.test.SqlitePluginRow.score", "score", offsetof(sqlite_plugin_row, score),
     &cmeta_data_long}};
static const cmeta_data_struct_shape sqlite_plugin_row_shape = {
    .layout = StructMeta(sqlite_plugin_row),
    .fields = sqlite_plugin_row_fields,
    .field_count = sizeof(sqlite_plugin_row_fields) /
                   sizeof(sqlite_plugin_row_fields[0])};
static const cmeta_data_desc sqlite_plugin_row_data = {
    .struct_size = SQLITE_PLUGIN_ROW_DATA_PREFIX_SIZE,
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.SqlitePluginRow.data",
    .display_name = "SqlitePluginRow",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &sqlite_plugin_row_type,
    .shape = &sqlite_plugin_row_shape};

static const char *sqlite_plugin_path(void) {
  const char *path = getenv("ORM_SQLITE_PLUGIN");
  return path != NULL ? path : "";
}

static orm_runtime_t *sqlite_plugin_runtime(orm_error_t *error) {
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
  load.module_path = orm_view(sqlite_plugin_path());
  load.expected_driver_id = orm_view("sqlite");
  check_equal(orm_runtime_load_driver(runtime, &load, error), ORM_STATUS_OK);

  memset(&info, 0, sizeof(info));
  check_equal(orm_runtime_driver_info(runtime, orm_view("sqlite"), &info, error),
              ORM_STATUS_OK);
  check_equal(info.canonical_id_size, 6u);
  check_equal(memcmp(info.canonical_id, "sqlite", 6u), 0);
  check_true((info.capabilities & ORM_DRIVER_CAP_SELECT) != 0u);
  check_true((info.capabilities & ORM_DRIVER_CAP_INSERT) != 0u);
  check_true((info.capabilities & ORM_DRIVER_CAP_UPDATE) != 0u);
  check_true((info.capabilities & ORM_DRIVER_CAP_DELETE) != 0u);
  check_true((info.capabilities & ORM_DRIVER_CAP_RAW_SQL) != 0u);
  check_true((info.capabilities & ORM_DRIVER_CAP_TRANSACTION) != 0u);
  check_true((info.capabilities & ORM_DRIVER_CAP_SAVEPOINT) != 0u);
  check_true((info.capabilities & ORM_DRIVER_CAP_INCREMENTAL_ROWS) != 0u);
  check_true((info.execution_models & ORM_DRIVER_EXEC_OWNER_EXECUTOR) != 0u);
  check_equal(info.execution_models & ORM_DRIVER_EXEC_NATIVE_WAIT, UINT64_C(0));

  memset(&storage, 0, sizeof(storage));
  check_equal(orm_runtime_driver_storage_info(
                  runtime, orm_view("sqlite"), &storage, error),
              ORM_STATUS_OK);
  check_true(orm_driver_storage_capabilities_valid(&storage));
  check_equal(
      storage.capabilities,
      (uint64_t)(ORM_DRIVER_STORAGE_CAP_ATOMIC_STATE_METADATA |
                 ORM_DRIVER_STORAGE_CAP_FILE_BACKED_CHECKPOINT |
                 ORM_DRIVER_STORAGE_CAP_STAGED_RESTORE));
  check_equal(storage.max_progress_metadata_bytes,
              (uint64_t)ORM_DRIVER_STORAGE_LIMIT_CONFIGURED);
  check_equal(storage.max_checkpoint_chunk_bytes, (uint64_t)0u);
  check_equal(storage.max_restore_chunk_bytes,
              (uint64_t)ORM_SQLITE_MAX_RESTORE_CHUNK_BYTES);
  return runtime;
}

static orm_connection_t *sqlite_plugin_connect(
    orm_runtime_t *runtime, const char *filename, orm_error_t *error) {
  orm_config_t config;
  orm_option_t option;
  orm_connection_t *connection = NULL;

  orm_config(&config);
  option.keyword = orm_view("filename");
  option.value = orm_view(filename);
  config.driver = orm_view("sqlite");
  config.options = &option;
  config.option_count = 1u;
  check_equal(orm_runtime_connect(runtime, &config, &connection, error),
              ORM_STATUS_OK);
  check_not_null(connection);
  return connection;
}

static orm_result_t *sqlite_plugin_execute(
    orm_connection_t *connection, const char *sql, orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  check_equal(orm_raw(connection, orm_view(sql), &query, error), ORM_STATUS_OK);
  check_not_null(query);
  check_equal(orm_query_execute(query, &result, error), ORM_STATUS_OK);
  check_not_null(result);
  orm_query_destroy(query);
  return result;
}

static uint64_t sqlite_plugin_affected(
    orm_query_t *query, orm_error_t *error) {
  orm_result_t *result = NULL;
  uint64_t affected = 0u;
  check_equal(orm_query_execute(query, &result, error), ORM_STATUS_OK);
  check_not_null(result);
  check_equal(orm_result_affected_rows(result, &affected, error), ORM_STATUS_OK);
  orm_result_destroy(result);
  orm_query_destroy(query);
  return affected;
}

static uint64_t sqlite_plugin_affected_in_transaction(
    orm_query_t *query, orm_transaction_t *transaction, orm_error_t *error) {
  orm_result_t *result = NULL;
  uint64_t affected = 0u;
  check_equal(orm_query_execute_in_transaction(
                  query, transaction, &result, error),
              ORM_STATUS_OK);
  check_not_null(result);
  check_equal(orm_result_affected_rows(result, &affected, error), ORM_STATUS_OK);
  orm_result_destroy(result);
  orm_query_destroy(query);
  return affected;
}

spec("SQLite runtime Plugin") {
  (void)ttest_config__;
  it("runs asynchronous rows through the real Plugin with demand and connection reuse") {
    orm_error_t error;
    orm_error_init(&error);
    orm_runtime_t *runtime = sqlite_plugin_runtime(&error);
    orm_connection_t *connection = sqlite_plugin_connect(runtime, ":memory:", &error);
    check_equal(orm_test_async_query(connection, &error), ORM_STATUS_OK);
    check_equal(orm_test_async_query(connection, &error), ORM_STATUS_OK);
    orm_disconnect(connection);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("retains the query connection and Plugin until a pending async Publisher is destroyed") {
    orm_error_t error;
    orm_error_init(&error);
    orm_runtime_t *runtime = sqlite_plugin_runtime(&error);
    orm_connection_t *connection = sqlite_plugin_connect(runtime, ":memory:", &error);
    orm_query_t *query = NULL;
    cflow_scheduler scheduler = {0};
    cflow_publisher rows = {0};
    orm_flow_config_t flow;
    check_true(cflow_scheduler_test_init(&scheduler));
    const orm_async_config_t async = {sizeof(async), &scheduler, 1u, 10000u};
    orm_flow_config(&flow, &sqlite_plugin_row_data);
    check_equal(orm_raw(connection, orm_view("select 7 as id, 19 as score"),
                        &query, &error), ORM_STATUS_OK);
    check_equal(orm_query_open_async_flow(query, &flow, &async, &rows, &error), ORM_STATUS_OK);
    sqlite_plugin_row row = {0};
    cflow_publish_context context = {&scheduler, 1u};
    check_equal(cflow_publisher_resume(&rows, &context, &row).kind, CFLOW_STEP_WAIT);
    orm_query_destroy(query);
    orm_disconnect(connection);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);
    cflow_publisher_destroy(&rows);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
    cflow_scheduler_destroy(&scheduler);
  }

  it("loads the real Plugin and keeps it leased through a typed row Publisher") {
    orm_error_t error;
    orm_runtime_t *runtime;
    orm_connection_t *connection;
    orm_query_t *query = NULL;
    orm_flow_config_t flow;
    cflow_publisher rows = {0};
    sqlite_plugin_row first = {0}, second = {0}, terminal = {0};
    cflow_step step;

    orm_error_init(&error);
    check_true(sqlite_plugin_path()[0] != '\0');
    runtime = sqlite_plugin_runtime(&error);
    connection = sqlite_plugin_connect(runtime, ":memory:", &error);

    check_equal(orm_raw(
                    connection,
                    orm_view("select 7 as id, 19 as score "
                             "union all select 11, 29 order by id"),
                    &query, &error),
                ORM_STATUS_OK);
    orm_flow_config(&flow, &sqlite_plugin_row_data);
    check_equal(orm_query_open_flow(query, &flow, &rows, &error), ORM_STATUS_OK);

    orm_query_destroy(query);
    query = NULL;
    orm_disconnect(connection);
    connection = NULL;
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);

    step = cflow_publisher_resume(&rows, NULL, &first);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(first.id, 7);
    check_equal(first.score, 19L);
    step = cflow_publisher_resume(&rows, NULL, &second);
    check_equal(step.kind, CFLOW_STEP_VALUE);
    check_equal(second.id, 11);
    check_equal(second.score, 29L);
    step = cflow_publisher_resume(&rows, NULL, &terminal);
    check_equal(step.kind, CFLOW_STEP_DONE);
    cflow_publisher_destroy(&rows);

    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("preserves structured commands raw parameters and SQLite value boundaries") {
    static const unsigned char payload[] = {0x00u, 0x7fu, 0xffu};
    orm_error_t error;
    orm_runtime_t *runtime;
    orm_connection_t *connection;
    orm_query_t *query = NULL;
    orm_result_t *result = NULL;
    uint64_t rows = 0u;
    int64_t id = 0;
    orm_string_view_t text = {0};
    orm_blob_t blob = {0};
    uint8_t is_null = 0u;

    orm_error_init(&error);
    runtime = sqlite_plugin_runtime(&error);
    connection = sqlite_plugin_connect(runtime, ":memory:", &error);

    result = sqlite_plugin_execute(
        connection,
        "create table items("
        "id integer primary key,name text,payload blob,value integer)",
        &error);
    orm_result_destroy(result);

    check_equal(orm_insert(connection, orm_view("items"), &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(query, orm_view("id"), orm_i64(1), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(query, orm_view("name"), orm_text("alpha"), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(
                    query, orm_view("payload"),
                    orm_blob(payload, sizeof(payload)), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(query, orm_view("value"), orm_null(), &error),
                ORM_STATUS_OK);
    check_equal(sqlite_plugin_affected(query, &error), UINT64_C(1));
    query = NULL;

    check_equal(orm_update(connection, orm_view("items"), &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(query, orm_view("name"), orm_text("beta"), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_where(
                    query, orm_view("id"), ORM_COMPARE_EQUAL, orm_i64(1), &error),
                ORM_STATUS_OK);
    check_equal(sqlite_plugin_affected(query, &error), UINT64_C(1));
    query = NULL;

    check_equal(orm_raw(
                    connection,
                    orm_view("select id,name,payload,value from items where id=?1"),
                    &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_bind(query, orm_i64(1), &error), ORM_STATUS_OK);
    check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
    check_equal(orm_result_row_count(result, &rows, &error), ORM_STATUS_OK);
    check_equal(rows, UINT64_C(1));
    check_equal(orm_result_get_int64(result, 0u, 0u, &id, &error), ORM_STATUS_OK);
    check_equal(id, INT64_C(1));
    check_equal(orm_result_get_text(result, 0u, 1u, &text, &error), ORM_STATUS_OK);
    check_equal(text.len, (size_t)4u);
    check_equal(memcmp(text.data, "beta", 4u), 0);
    check_equal(orm_result_get_blob(result, 0u, 2u, &blob, &error), ORM_STATUS_OK);
    check_equal(blob.size, sizeof(payload));
    check_equal(memcmp(blob.data, payload, sizeof(payload)), 0);
    check_equal(orm_result_is_null(result, 0u, 3u, &is_null, &error),
                ORM_STATUS_OK);
    check_equal(is_null, (uint8_t)1u);
    orm_result_destroy(result);
    result = NULL;
    orm_query_destroy(query);
    query = NULL;

    check_equal(orm_insert(connection, orm_view("items"), &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(query, orm_view("id"), orm_i64(2), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(query, orm_view("name"), orm_text("delete"), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(query, orm_view("payload"), orm_blob(NULL, 0u), &error),
                ORM_STATUS_OK);
    check_equal(orm_query_set(query, orm_view("value"), orm_i64(5), &error),
                ORM_STATUS_OK);
    check_equal(sqlite_plugin_affected(query, &error), UINT64_C(1));
    query = NULL;

    check_equal(orm_delete(connection, orm_view("items"), &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_where(
                    query, orm_view("id"), ORM_COMPARE_EQUAL, orm_i64(2), &error),
                ORM_STATUS_OK);
    check_equal(sqlite_plugin_affected(query, &error), UINT64_C(1));

    orm_disconnect(connection);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("preserves transaction rollback commit and savepoint behavior") {
    orm_error_t error;
    orm_runtime_t *runtime;
    orm_connection_t *connection;
    orm_transaction_t *transaction = NULL;
    orm_query_t *query = NULL;
    orm_result_t *result = NULL;
    int64_t value = 0;

    orm_error_init(&error);
    runtime = sqlite_plugin_runtime(&error);
    connection = sqlite_plugin_connect(runtime, ":memory:", &error);
    result = sqlite_plugin_execute(
        connection, "create table state(id integer primary key,value integer)", &error);
    orm_result_destroy(result);

    check_equal(orm_transaction_begin(
                    connection, ORM_ISOLATION_SERIALIZABLE, &transaction, &error),
                ORM_STATUS_OK);
    check_equal(orm_transaction_savepoint(
                    transaction, orm_view("before_update"), &error),
                ORM_STATUS_OK);
    check_equal(orm_raw(
                    connection,
                    orm_view("insert into state values(1,42)"),
                    &query, &error),
                ORM_STATUS_OK);
    check_equal(sqlite_plugin_affected_in_transaction(query, transaction, &error),
                UINT64_C(1));
    query = NULL;
    check_equal(orm_transaction_rollback_to_savepoint(
                    transaction, orm_view("before_update"), &error),
                ORM_STATUS_OK);
    check_equal(orm_transaction_release_savepoint(
                    transaction, orm_view("before_update"), &error),
                ORM_STATUS_OK);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    orm_transaction_destroy(transaction);
    transaction = NULL;

    result = sqlite_plugin_execute(
        connection, "select count(*) from state", &error);
    check_equal(orm_result_get_int64(result, 0u, 0u, &value, &error),
                ORM_STATUS_OK);
    check_equal(value, INT64_C(0));
    orm_result_destroy(result);

    check_equal(orm_transaction_begin(
                    connection, ORM_ISOLATION_SERIALIZABLE, &transaction, &error),
                ORM_STATUS_OK);
    check_equal(orm_raw(
                    connection, orm_view("insert into state values(1,99)"),
                    &query, &error),
                ORM_STATUS_OK);
    check_equal(sqlite_plugin_affected_in_transaction(query, transaction, &error),
                UINT64_C(1));
    query = NULL;
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    orm_transaction_destroy(transaction);
    transaction = NULL;

    result = sqlite_plugin_execute(
        connection, "select value from state where id=1", &error);
    check_equal(orm_result_get_int64(result, 0u, 0u, &value, &error),
                ORM_STATUS_OK);
    check_equal(value, INT64_C(99));
    orm_result_destroy(result);

    orm_disconnect(connection);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("keeps separate SQLite files isolated under one runtime") {
    char *first_path = tt_make_temp_file("orm-sqlite-plugin-a", ".db");
    char *second_path = tt_make_temp_file("orm-sqlite-plugin-b", ".db");
    orm_error_t error;
    orm_runtime_t *runtime;
    orm_connection_t *first;
    orm_connection_t *second;
    orm_result_t *result;
    int64_t value = 0;

    check_not_null(first_path);
    check_not_null(second_path);
    orm_error_init(&error);
    runtime = sqlite_plugin_runtime(&error);
    first = sqlite_plugin_connect(runtime, first_path, &error);
    second = sqlite_plugin_connect(runtime, second_path, &error);

    result = sqlite_plugin_execute(
        first, "create table state(value integer)", &error);
    orm_result_destroy(result);
    result = sqlite_plugin_execute(
        first, "insert into state values(11)", &error);
    orm_result_destroy(result);
    result = sqlite_plugin_execute(
        second, "create table state(value integer)", &error);
    orm_result_destroy(result);
    result = sqlite_plugin_execute(
        second, "insert into state values(22)", &error);
    orm_result_destroy(result);

    result = sqlite_plugin_execute(first, "select value from state", &error);
    check_equal(orm_result_get_int64(result, 0u, 0u, &value, &error),
                ORM_STATUS_OK);
    check_equal(value, INT64_C(11));
    orm_result_destroy(result);
    result = sqlite_plugin_execute(second, "select value from state", &error);
    check_equal(orm_result_get_int64(result, 0u, 0u, &value, &error),
                ORM_STATUS_OK);
    check_equal(value, INT64_C(22));
    orm_result_destroy(result);

    orm_disconnect(first);
    orm_disconnect(second);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);

    check_equal(tt_remove_file(first_path), 0);
    check_equal(tt_remove_file(second_path), 0);
    free(first_path);
    free(second_path);
  }
}
