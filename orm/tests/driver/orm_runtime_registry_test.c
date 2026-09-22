#include <orm_runtime.h>

#include <tinytest.h>

#include <stdlib.h>
#include <string.h>

static const char *fixture_path(void) {
  const char *path = getenv("ORM_MODULE_FIXTURE");
  return path == NULL ? "" : path;
}

static orm_driver_load_config_t load_config(const char *id) {
  orm_driver_load_config_t config;
  memset(&config, 0, sizeof(config));
  config.struct_size = (uint32_t)sizeof(config);
  config.abi_version = ORM_RUNTIME_ABI_VERSION;
  config.module_path = orm_view(fixture_path());
  config.expected_driver_id = orm_view(id);
  return config;
}

spec("runtime driver registry") {
  it("loads validates initializes and publishes one explicit module atomically") {
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_error_t error;
    orm_driver_info_t info;
    orm_driver_load_config_t load = load_config("fixture");

    orm_runtime_config_init(&config);
    orm_error_init(&error);
    check_equal(config.execution.execution_model,
                ORM_DRIVER_EXEC_CALLER_BLOCKING);
    check_equal(orm_runtime_create(&config, &runtime, &error),
                ORM_STATUS_OK);
    check_not_null(runtime);

    check_equal(orm_runtime_load_driver(runtime, &load, &error),
                ORM_STATUS_OK);
    memset(&info, 0xa5, sizeof(info));
    check_equal(orm_runtime_driver_info(runtime, orm_view("fixture"),
                                        &info, &error),
                ORM_STATUS_OK);
    check_equal(info.canonical_id_size, 7u);
    check_equal(memcmp(info.canonical_id, "fixture", 7u), 0);
    check_equal(info.execution_models, ORM_DRIVER_EXEC_CALLER_BLOCKING);

    memset(&info, 0, sizeof(info));
    check_equal(orm_runtime_driver_info(runtime, orm_view("fixture-alias"),
                                        &info, &error),
                ORM_STATUS_OK);
    check_equal(info.canonical_id_size, 7u);
    check_equal(memcmp(info.canonical_id, "fixture", 7u), 0);

    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("rejects an ID mismatch without publishing a partial registration") {
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_error_t error;
    orm_driver_info_t info;
    orm_driver_load_config_t wrong = load_config("other");
    orm_driver_load_config_t right = load_config("fixture");

    orm_runtime_config_init(&config);
    check_equal(orm_runtime_create(&config, &runtime, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &wrong, &error),
                ORM_STATUS_DRIVER_ID_MISMATCH);
    check_equal(orm_runtime_driver_info(runtime, orm_view("fixture"),
                                        &info, &error),
                ORM_STATUS_DRIVER_NOT_REGISTERED);
    check_equal(orm_runtime_load_driver(runtime, &right, &error),
                ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("rejects duplicate canonical registration without replacing the first") {
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_error_t error;
    orm_driver_info_t info;
    orm_driver_load_config_t load = load_config("fixture");

    orm_runtime_config_init(&config);
    check_equal(orm_runtime_create(&config, &runtime, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &load, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &load, &error),
                ORM_STATUS_DRIVER_ALREADY_REGISTERED);
    check_equal(orm_runtime_driver_info(runtime, orm_view("fixture"),
                                        &info, &error),
                ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("rejects a repeated explicit module path before reopening it") {
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_error_t error;
    orm_driver_load_config_t first = load_config("fixture");
    orm_driver_load_config_t second = load_config("other");

    orm_runtime_config_init(&config);
    check_equal(orm_runtime_create(&config, &runtime, &error), ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &first, &error), ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &second, &error),
                ORM_STATUS_DRIVER_ALREADY_REGISTERED);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("enforces the configured bounded driver registry") {
    orm_runtime_config_t config;
    orm_runtime_t *runtime = NULL;
    orm_error_t error;
    orm_driver_load_config_t load = load_config("fixture");

    orm_runtime_config_init(&config);
    config.max_drivers = 1u;
    check_equal(orm_runtime_create(&config, &runtime, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &load, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &load, &error),
                ORM_STATUS_LIMIT_EXCEEDED);
    orm_runtime_release(runtime);
  }

  it("holds the runtime open while a driver connection exists") {
    orm_runtime_config_t runtime_config;
    orm_runtime_t *runtime = NULL;
    orm_connection_t *connection = NULL;
    orm_config_t connection_config;
    orm_error_t error;
    orm_driver_load_config_t load = load_config("fixture");

    orm_runtime_config_init(&runtime_config);
    orm_config(&connection_config);
    connection_config.driver = orm_view("fixture");
    check_equal(orm_runtime_create(&runtime_config, &runtime, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &load, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_connect(runtime, &connection_config,
                                    &connection, &error),
                ORM_STATUS_OK);
    check_not_null(connection);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);
    orm_disconnect(connection);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("does not publish a module lease when connect fails") {
    orm_runtime_config_t runtime_config;
    orm_runtime_t *runtime = NULL;
    orm_connection_t *connection = (orm_connection_t *)(uintptr_t)1u;
    orm_config_t connection_config;
    orm_error_t error;
    orm_driver_load_config_t load = load_config("fixture");

    orm_runtime_config_init(&runtime_config);
    orm_config(&connection_config);
    connection_config.driver = orm_view("missing");
    check_equal(orm_runtime_create(&runtime_config, &runtime, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &load, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_connect(runtime, &connection_config,
                                    &connection, &error),
                ORM_STATUS_DRIVER_NOT_REGISTERED);
    check_null(connection);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("keeps runtime and query leases through a driver row Publisher") {
    orm_runtime_config_t runtime_config;
    orm_runtime_t *runtime = NULL;
    orm_connection_t *connection = NULL;
    orm_query_t *query = NULL;
    orm_config_t connection_config;
    orm_flow_config_t flow_config;
    cflow_publisher rows = {0};
    orm_error_t error;
    int value = 0;
    orm_driver_load_config_t load = load_config("fixture");

    orm_runtime_config_init(&runtime_config);
    orm_config(&connection_config);
    connection_config.driver = orm_view("fixture-alias");
    orm_flow_config(&flow_config, &cmeta_data_int);

    check_equal(orm_runtime_create(&runtime_config, &runtime, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &load, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_connect(runtime, &connection_config,
                                    &connection, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_create(connection, orm_view("rows"),
                                 &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_open_flow(query, &flow_config, &rows, &error),
                ORM_STATUS_OK);

    orm_query_destroy(query);
    query = NULL;
    orm_disconnect(connection);
    connection = NULL;
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);

    const cflow_step step = cflow_publisher_resume(&rows, NULL, &value);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(value, 7);
    cflow_publisher_destroy(&rows);

    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("keeps the module leased through a lazy driver command Publisher") {
    orm_runtime_config_t runtime_config;
    orm_runtime_t *runtime = NULL;
    orm_connection_t *connection = NULL;
    orm_query_t *query = NULL;
    orm_config_t connection_config;
    cflow_publisher command = {0};
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    orm_error_t error;
    orm_driver_load_config_t load = load_config("fixture");

    orm_runtime_config_init(&runtime_config);
    orm_config(&connection_config);
    connection_config.driver = orm_view("fixture");
    check_equal(orm_runtime_create(&runtime_config, &runtime, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &load, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_connect(runtime, &connection_config,
                                    &connection, &error),
                ORM_STATUS_OK);
    check_equal(orm_insert(connection, orm_view("rows"), &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow(query, &command, &error),
                ORM_STATUS_OK);

    orm_query_destroy(query);
    query = NULL;
    orm_disconnect(connection);
    connection = NULL;
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);

    const cflow_step step =
        cflow_publisher_resume(&command, NULL, &result);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(result.affected_rows, UINT64_C(3));
    cflow_publisher_destroy(&command);

    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("keeps the module leased through a Driver transaction Publisher") {
    orm_runtime_config_t runtime_config;
    orm_runtime_t *runtime = NULL;
    orm_connection_t *connection = NULL;
    orm_transaction_t *transaction = NULL;
    orm_query_t *query = NULL;
    orm_config_t connection_config;
    cflow_publisher command = {0};
    orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
    orm_error_t error;
    orm_driver_load_config_t load = load_config("fixture");

    orm_runtime_config_init(&runtime_config);
    orm_config(&connection_config);
    connection_config.driver = orm_view("fixture");
    check_equal(orm_runtime_create(&runtime_config, &runtime, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(runtime, &load, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_connect(runtime, &connection_config,
                                    &connection, &error),
                ORM_STATUS_OK);
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                      &transaction, &error),
                ORM_STATUS_OK);
    check_equal(orm_insert(connection, orm_view("rows"), &query, &error),
                ORM_STATUS_OK);
    check_equal(orm_query_open_command_flow_in_transaction(
                    query, transaction, &command, &error),
                ORM_STATUS_OK);

    orm_query_destroy(query);
    query = NULL;
    orm_disconnect(connection);
    connection = NULL;
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);

    const cflow_step step =
        cflow_publisher_resume(&command, NULL, &result);
    check_equal(step.kind, CFLOW_STEP_VALUE_AND_DONE);
    check_equal(result.affected_rows, UINT64_C(3));
    cflow_publisher_destroy(&command);

    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    orm_transaction_destroy(transaction);
    transaction = NULL;
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
    orm_runtime_release(runtime);
  }

  it("keeps two runtimes using the same module independent") {
    orm_runtime_config_t config;
    orm_runtime_t *first = NULL;
    orm_runtime_t *second = NULL;
    orm_connection_t *connection = NULL;
    orm_config_t connection_config;
    orm_error_t error;
    orm_driver_load_config_t load = load_config("fixture");

    orm_runtime_config_init(&config);
    orm_config(&connection_config);
    connection_config.driver = orm_view("fixture-alias");
    check_equal(orm_runtime_create(&config, &first, &error), ORM_STATUS_OK);
    check_equal(orm_runtime_create(&config, &second, &error), ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(first, &load, &error), ORM_STATUS_OK);
    check_equal(orm_runtime_load_driver(second, &load, &error), ORM_STATUS_OK);
    check_equal(orm_runtime_connect(first, &connection_config,
                                    &connection, &error),
                ORM_STATUS_OK);
    check_equal(orm_runtime_close(first, &error), ORM_STATUS_BUSY);
    check_equal(orm_runtime_close(second, &error), ORM_STATUS_OK);
    orm_runtime_release(second);
    orm_disconnect(connection);
    check_equal(orm_runtime_close(first, &error), ORM_STATUS_OK);
    orm_runtime_release(first);
  }
}
