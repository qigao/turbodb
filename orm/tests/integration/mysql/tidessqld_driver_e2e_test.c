#include "process_fixture.h"

#include "orm.h"
#include <orm_runtime.h>

#include <cmeta/struct.h>
#include <tinytest.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TEST_DAEMON_TIMEOUT_MS = 15000 };

#define TEST_ROW_DATA_PREFIX_SIZE                                          \
  (offsetof(cmeta_data_desc, shape) +                                      \
   sizeof(((cmeta_data_desc *)0)->shape))

Struct(tidessqld_orm_row, (long, value));

static const cmeta_type_identity test_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.TidesSqlProcessRow");
static const cmeta_type_traits test_row_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_desc test_row_type = {
    .name = "tidessqld_orm_row",
    .size = sizeof(tidessqld_orm_row),
    .align = _Alignof(tidessqld_orm_row),
    .kind = CMETA_T_OBJECT,
    .traits = &test_row_traits,
    .identity = &test_row_identity};
static const cmeta_data_field_desc test_row_fields[] = {
    {"orm.test.TidesSqlProcessRow.value", "value",
     offsetof(tidessqld_orm_row, value), &cmeta_data_long}};
static const cmeta_data_struct_shape test_row_shape = {
    .layout = StructMeta(tidessqld_orm_row),
    .fields = test_row_fields,
    .field_count = 1u};
static const cmeta_data_desc test_row_data = {
    .struct_size = TEST_ROW_DATA_PREFIX_SIZE,
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.TidesSqlProcessRow.data",
    .display_name = "TidesSqlProcessRow",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &test_row_type,
    .shape = &test_row_shape};

static int report_status(
    const char *operation, orm_status_t status, const orm_error_t *error) {
  if (status == ORM_STATUS_OK) return 0;
  (void)fprintf(stderr, "%s failed: status=%d message=%s\n",
                operation, (int)status,
                error != NULL ? error->message : "");
  return 1;
}

static int run_command(
    orm_query_t *query, orm_transaction_t *transaction,
    uint64_t *affected, orm_error_t *error) {
  cflow_publisher publisher = {0};
  orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
  orm_status_t status;
  cflow_step step;

  if (affected != NULL) *affected = 0u;
  status = transaction == NULL
               ? orm_query_open_command_flow(query, &publisher, error)
               : orm_query_open_command_flow_in_transaction(
                     query, transaction, &publisher, error);
  if (report_status("open command flow", status, error) != 0) return 1;

  step = cflow_publisher_resume(&publisher, NULL, &result);
  cflow_publisher_destroy(&publisher);
  if (step.kind != CFLOW_STEP_VALUE_AND_DONE) {
    (void)fprintf(stderr, "command flow failed: step=%d message=%s\n",
                  (int)step.kind,
                  step.error != NULL ? step.error : "");
    return 1;
  }
  if (affected != NULL) *affected = result.affected_rows;
  return 0;
}

static int raw_command(
    orm_connection_t *connection, const char *sql,
    const orm_value_t *values, size_t value_count,
    uint64_t *affected, orm_error_t *error) {
  orm_query_t *query = NULL;
  int failed = report_status(
      "create raw command",
      orm_raw(connection, orm_view(sql), &query, error), error);

  for (size_t i = 0u; !failed && i < value_count; ++i)
    failed = report_status(
        "bind raw command", orm_query_bind(query, values[i], error), error);
  if (!failed) failed = run_command(query, NULL, affected, error);
  orm_query_destroy(query);
  return failed;
}

static int structured_update(
    orm_connection_t *connection, orm_transaction_t *transaction,
    int64_t value, uint64_t *affected, orm_error_t *error) {
  orm_query_t *query = NULL;
  int failed = report_status(
      "create structured update",
      orm_update(connection, orm_view("orm_process_items"), &query, error),
      error);

  if (!failed)
    failed = report_status(
        "set structured value",
        orm_query_set(query, orm_view("value"), orm_i64(value), error),
        error);
  if (!failed)
    failed = report_status(
        "set structured predicate",
        orm_query_where(query, orm_view("id"), ORM_COMPARE_EQUAL,
                        orm_i64(1), error),
        error);
  if (!failed) failed = run_command(query, transaction, affected, error);
  orm_query_destroy(query);
  return failed;
}

static int read_structured_value(
    orm_connection_t *connection, long expected, orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_flow_config_t flow;
  cflow_publisher publisher = {0};
  tidessqld_orm_row row = {0};
  cflow_step step;
  int failed = report_status(
      "create structured query",
      orm_query_create(
          connection, orm_view("orm_process_items"), &query, error),
      error);

  if (!failed)
    failed = report_status(
        "select structured column",
        orm_query_add_column(query, orm_view("value"), error), error);
  if (!failed)
    failed = report_status(
        "set structured query predicate",
        orm_query_where(query, orm_view("id"), ORM_COMPARE_EQUAL,
                        orm_i64(1), error),
        error);
  orm_flow_config(&flow, &test_row_data);
  if (!failed)
    failed = report_status(
        "open structured row flow",
        orm_query_open_flow(query, &flow, &publisher, error), error);

  if (!failed) {
    step = cflow_publisher_resume(&publisher, NULL, &row);
    if ((step.kind != CFLOW_STEP_VALUE &&
         step.kind != CFLOW_STEP_VALUE_AND_DONE) ||
        row.value != expected) {
      (void)fprintf(stderr,
                    "structured row mismatch: step=%d value=%ld expected=%ld\n",
                    (int)step.kind, row.value, expected);
      failed = 1;
    } else if (step.kind == CFLOW_STEP_VALUE) {
      step = cflow_publisher_resume(&publisher, NULL, &row);
      if (step.kind != CFLOW_STEP_DONE) {
        (void)fprintf(stderr,
                      "structured row flow did not terminate: step=%d\n",
                      (int)step.kind);
        failed = 1;
      }
    }
  }

  if (cflow_publisher_valid(&publisher))
    cflow_publisher_destroy(&publisher);
  orm_query_destroy(query);
  return failed;
}

static orm_runtime_t *open_runtime(orm_error_t *error) {
  orm_runtime_config_t config;
  orm_driver_load_config_t load;
  orm_runtime_t *runtime = NULL;

  orm_runtime_config_init(&config);
  if (report_status(
          "create ORM runtime",
          orm_runtime_create(&config, &runtime, error), error) != 0)
    return NULL;
  memset(&load, 0, sizeof(load));
  load.struct_size = (uint32_t)sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.module_path = orm_view(TEST_ORM_MYSQL_PLUGIN);
  load.expected_driver_id = orm_view("mysql");
  if (report_status(
          "load MySQL ORM plugin",
          orm_runtime_load_driver(runtime, &load, error), error) != 0) {
    orm_runtime_release(runtime);
    return NULL;
  }
  return runtime;
}

static orm_connection_t *open_connection(
    orm_runtime_t *runtime, uint16_t port, orm_error_t *error) {
  char port_text[8];
  orm_option_t options[8];
  orm_config_t config;
  orm_connection_t *connection = NULL;

  (void)snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
  options[0] = (orm_option_t){orm_view("host"), orm_view("127.0.0.1")};
  options[1] = (orm_option_t){orm_view("port"), orm_view(port_text)};
  options[2] = (orm_option_t){orm_view("username"), orm_view("alice")};
  options[3] =
      (orm_option_t){orm_view("password"), orm_view("test-password")};
  options[4] = (orm_option_t){orm_view("database"), orm_view("tenant")};
  options[5] = (orm_option_t){orm_view("ca_file"), orm_view(TEST_TLS_CA)};
  options[6] =
      (orm_option_t){orm_view("server_name"), orm_view("localhost")};
  options[7] = (orm_option_t){orm_view("timeout_ms"), orm_view("5000")};
  orm_config(&config);
  config.driver = orm_view("mysql");
  config.options = options;
  config.option_count = 8u;
  if (report_status(
          "connect ORM MySQL driver",
          orm_runtime_connect(runtime, &config, &connection, error),
          error) != 0)
    return NULL;
  return connection;
}

static int exercise_orm_process_chain(
    orm_connection_t *connection, orm_error_t *error) {
  static const char create_sql[] =
      "CREATE TABLE orm_process_items("
      "id BIGINT PRIMARY KEY,value BIGINT NOT NULL)";
  static const char insert_sql[] =
      "INSERT INTO orm_process_items(id,value) VALUES(?1,?2)";
  const orm_value_t insert_values[] = {orm_i64(1), orm_i64(40)};
  orm_transaction_t *transaction = NULL;
  uint64_t affected = 0u;
  int failed = 0;

  if (raw_command(connection, create_sql, NULL, 0u,
                  &affected, error) != 0)
    failed = 1;
  if (!failed &&
      (raw_command(connection, insert_sql, insert_values, 2u,
                   &affected, error) != 0 ||
       affected != UINT64_C(1)))
    failed = 1;
  if (!failed &&
      (structured_update(connection, NULL, INT64_C(42),
                         &affected, error) != 0 ||
       affected != UINT64_C(1)))
    failed = 1;
  if (!failed && read_structured_value(connection, 42L, error) != 0)
    failed = 1;
  if (!failed &&
      report_status(
          "begin ORM transaction",
          orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                &transaction, error),
          error) != 0)
    failed = 1;
  if (!failed &&
      (structured_update(connection, transaction, INT64_C(99),
                         &affected, error) != 0 ||
       affected != UINT64_C(1)))
    failed = 1;
  if (transaction != NULL) {
    if (report_status(
            "rollback ORM transaction",
            orm_transaction_rollback(transaction, error), error) != 0)
      failed = 1;
    orm_transaction_destroy(transaction);
  }
  if (!failed && read_structured_value(connection, 42L, error) != 0)
    failed = 1;
  return failed;
}

static int run_process_e2e(void) {
  char *directory = tt_make_temp_dir("orm-mysql-tidessqld");
  char *config = NULL;
  char *database = NULL;
  tidessqld_test_daemon daemon = {0};
  orm_runtime_t *runtime = NULL;
  orm_connection_t *connection = NULL;
  orm_error_t error;
  uint16_t port = 0u;
  int started = 0;
  int failed = 0;

  orm_error_init(&error);
  if (directory == NULL) return 1;
  config = tidessqld_test_join_path(directory, "tidessqld.toml");
  database = tidessqld_test_join_path(directory, "database");
  if (config == NULL || database == NULL ||
      tidessqld_test_write_config(
          config, database, TEST_TLS_CERT, TEST_TLS_KEY) != 0) {
    failed = 1;
    goto cleanup;
  }
  if (tidessqld_test_daemon_start(
          &daemon, tidessqld_test_executable(TEST_TIDESSQLD), config) != 0) {
    failed = 1;
    goto cleanup;
  }
  started = 1;
  if (tidessqld_test_daemon_wait_for_port(
          &daemon, TEST_DAEMON_TIMEOUT_MS, &port) != 0) {
    failed = 1;
    goto cleanup;
  }
  runtime = open_runtime(&error);
  if (runtime == NULL) {
    failed = 1;
    goto cleanup;
  }
  connection = open_connection(runtime, port, &error);
  if (connection == NULL) {
    failed = 1;
    goto cleanup;
  }
  failed = exercise_orm_process_chain(connection, &error);

cleanup:
  orm_disconnect(connection);
  if (runtime != NULL) {
    orm_error_init(&error);
    if (orm_runtime_close(runtime, &error) != ORM_STATUS_OK) {
      (void)fprintf(stderr, "close ORM runtime failed: %s\n", error.message);
      failed = 1;
    }
    orm_runtime_release(runtime);
  }
  if (started &&
      tidessqld_test_daemon_stop(
          &daemon, TEST_DAEMON_TIMEOUT_MS) != 0)
    failed = 1;
  tidessqld_test_daemon_force_cleanup(
      &daemon, TEST_DAEMON_TIMEOUT_MS);
  if (tt_remove_tree(directory) != 0) failed = 1;
  free(database);
  free(config);
  free(directory);
  return failed;
}

spec("ORM MySQL driver against standalone tidessqld") {
  it("loads the plugin and preserves structured rollback over TLS") {
    check_equal(run_process_e2e(), 0);
  }
}
