#include "orm_runtime_internal.h"

#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

enum { MYSQL, POSTGRESQL, DRIVER_COUNT };
static const char *const driver_ids[DRIVER_COUNT] = {"mysql", "postgresql"};
static const char *const fixture_variables[DRIVER_COUNT] = {
    "ORM_MYSQL_MOCK_PLUGIN", "ORM_POSTGRESQL_MOCK_PLUGIN"};
static const int expected_rows[DRIVER_COUNT] = {42, 84};

typedef struct driver_fixture {
  orm_runtime_driver *entry;
  TurboDb_Driver *original;
  TurboDb_Driver binding;
  TurboDb_Driver_vtable vtable;
  orm_connection_t *connection;
} driver_fixture;

static orm_runtime_t *runtime;
static orm_error_t error;
static driver_fixture drivers[DRIVER_COUNT];

typedef struct create_gate_script {
  void *owners[3];
  orm_status_t results[3];
  size_t count;
  size_t index;
} create_gate_script;

static create_gate_script gate_script;

static orm_status_t create_gate(void *owner) {
  const size_t index = gate_script.index++;
  check_true(index < gate_script.count);
  check_true(gate_script.owners[index] == owner);
  return gate_script.results[index];
}

static void expect_create_gate(void *owner, orm_status_t result) {
  check_true(gate_script.count < DRIVER_COUNT + 1u);
  gate_script.owners[gate_script.count] = owner;
  gate_script.results[gate_script.count] = result;
  ++gate_script.count;
}

/* Keep real plugin admission, leases, cursors and cleanup; inject only the
 * driver factory outcome through the host's private binding for this test. */
static orm_status_t ORM_DRIVER_CALL create_connection(
    void *self, const orm_config_t *config, const orm_driver_limits_v1 *limits,
    orm_driver_connection_v1 *out, orm_error_t *out_error) {
  const orm_status_t status = create_gate(self);
  if (status != ORM_STATUS_OK) {
    orm_error_init(out_error);
    out_error->status = status;
    return status;
  }
  for (size_t i = 0; i < DRIVER_COUNT; ++i) {
    if (drivers[i].original->self == self)
      return TurboDb_Driver_create(drivers[i].original, config, limits,
                                   out, out_error);
  }
  return ORM_STATUS_INVALID_ARGUMENT;
}

static void connect_driver(size_t index, orm_status_t expected) {
  orm_config_t config;
  orm_config(&config);
  config.driver = orm_view(driver_ids[index]);
  expect_create_gate(drivers[index].original->self, expected);
  check_equal(orm_runtime_connect(runtime, &config,
                                  &drivers[index].connection, &error), expected);
  if (expected == ORM_STATUS_OK)
    check_not_null(drivers[index].connection);
  else
    check_null(drivers[index].connection);
}

static void disconnect_driver(size_t index) {
  orm_disconnect(drivers[index].connection);
  drivers[index].connection = NULL;
}

static void check_row(size_t index) {
  orm_query_t *query = NULL;
  cflow_publisher rows = {0};
  orm_flow_config_t flow;
  int value = 0;
  orm_flow_config(&flow, &cmeta_data_int);
  check_equal(orm_query_create(drivers[index].connection, orm_view("rows"),
                               &query, &error), ORM_STATUS_OK);
  check_equal(orm_query_open_flow(query, &flow, &rows, &error), ORM_STATUS_OK);
  check_equal(cflow_publisher_resume(&rows, NULL, &value).kind,
              CFLOW_STEP_VALUE_AND_DONE);
  check_equal(value, expected_rows[index]);
  cflow_publisher_destroy(&rows);
  orm_query_destroy(query);
}

spec("runtime driver coexistence") {
  before_each() {
    orm_runtime_config_t config;
    orm_runtime_config_init(&config);
    orm_error_init(&error);
    memset(drivers, 0, sizeof(drivers));
    runtime = NULL;
    gate_script = (create_gate_script){0};
    check_equal(orm_runtime_create(&config, &runtime, &error), ORM_STATUS_OK);
    for (size_t i = 0; i < DRIVER_COUNT; ++i) {
      const char *path = getenv(fixture_variables[i]);
      check_not_null(path);
      orm_driver_load_config_t load = {0};
      load.struct_size = sizeof(load);
      load.abi_version = ORM_RUNTIME_ABI_VERSION;
      load.module_path = orm_view(path);
      load.expected_driver_id = orm_view(driver_ids[i]);
      check_equal(orm_runtime_load_driver(runtime, &load, &error), ORM_STATUS_OK);
      drivers[i].entry = runtime_find_driver(runtime, orm_view(driver_ids[i]));
      check_not_null(drivers[i].entry);
      drivers[i].original = drivers[i].entry->binding;
      drivers[i].binding = *drivers[i].original;
      drivers[i].vtable = *drivers[i].original->vtable;
      drivers[i].vtable.create = create_connection;
      drivers[i].binding.vtable = &drivers[i].vtable;
      drivers[i].entry->binding = &drivers[i].binding;
    }
  }

  after_each() {
    for (size_t i = 0; i < DRIVER_COUNT; ++i) {
      disconnect_driver(i);
      if (drivers[i].entry != NULL)
        drivers[i].entry->binding = drivers[i].original;
    }
    if (runtime != NULL) {
      check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
      orm_runtime_release(runtime);
      runtime = NULL;
    }
    check_equal(gate_script.index, gate_script.count);
  }

  it("routes connections and rows to each registered driver") {
    connect_driver(MYSQL, ORM_STATUS_OK);
    connect_driver(POSTGRESQL, ORM_STATUS_OK);
    check_row(MYSQL);
    check_row(POSTGRESQL);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);
    check_equal(gate_script.index, (size_t)2u);
  }

  it("keeps PostgreSQL usable across MySQL disconnect and reconnect") {
    connect_driver(MYSQL, ORM_STATUS_OK);
    connect_driver(POSTGRESQL, ORM_STATUS_OK);
    disconnect_driver(MYSQL);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);
    check_row(POSTGRESQL);
    connect_driver(MYSQL, ORM_STATUS_OK);
    check_row(MYSQL);
    disconnect_driver(POSTGRESQL);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);
    check_row(MYSQL);
    check_equal(gate_script.index, (size_t)3u);
  }

  it("isolates a failed MySQL connection and permits retry beside PostgreSQL") {
    connect_driver(POSTGRESQL, ORM_STATUS_OK);
    connect_driver(MYSQL, ORM_STATUS_CONNECTION_ERROR);
    check_row(POSTGRESQL);
    check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_BUSY);
    connect_driver(MYSQL, ORM_STATUS_OK);
    check_row(MYSQL);
    check_row(POSTGRESQL);
    check_equal(gate_script.index, (size_t)3u);
  }
}
