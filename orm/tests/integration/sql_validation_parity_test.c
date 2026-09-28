#include <orm_runtime.h>

#include <data_bind.h>
#include <data_bind_message_plan.h>
#include <data_bind_native_binding.h>
#include <tinytest.h>

#include <cmeta/data.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct orm_validation_parity_row {
  uint32_t id;
} orm_validation_parity_row;

static const cmeta_type_identity ORM_VALIDATION_PARITY_ROW_ID =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.ValidationParityRow");
static const cmeta_type_desc ORM_VALIDATION_PARITY_ROW_TYPE = {
    "orm_validation_parity_row",
    sizeof(orm_validation_parity_row),
    _Alignof(orm_validation_parity_row),
    CMETA_T_OBJECT,
    NULL,
    NULL,
    &ORM_VALIDATION_PARITY_ROW_ID
};
static const cmeta_field_desc ORM_VALIDATION_PARITY_LAYOUT_FIELDS[] = {
    {"id", "uint32_t", offsetof(orm_validation_parity_row, id),
     sizeof(uint32_t), _Alignof(uint32_t), &cmeta_type_uint32, NULL}
};
static const cmeta_struct_desc ORM_VALIDATION_PARITY_LAYOUT = {
    "ValidationParityRow",
    sizeof(orm_validation_parity_row),
    _Alignof(orm_validation_parity_row),
    ORM_VALIDATION_PARITY_LAYOUT_FIELDS,
    1u
};
static const cmeta_data_field_desc ORM_VALIDATION_PARITY_FIELDS[] = {
    {"orm.test.ValidationParityRow.id", "id",
     offsetof(orm_validation_parity_row, id), &cmeta_data_uint32}
};
static const cmeta_data_struct_shape ORM_VALIDATION_PARITY_SHAPE = {
    &ORM_VALIDATION_PARITY_LAYOUT,
    ORM_VALIDATION_PARITY_FIELDS,
    1u
};
static const cmeta_data_desc ORM_VALIDATION_PARITY_DATA = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.ValidationParityRow.data",
    .display_name = "ValidationParityRow",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &ORM_VALIDATION_PARITY_ROW_TYPE,
    .shape = &ORM_VALIDATION_PARITY_SHAPE
};
static const DataBindNativeTypeBinding ORM_VALIDATION_PARITY_NATIVE =
    DATA_BIND_NATIVE_TYPE_BINDING_INIT(
        "ValidationParityRow", &ORM_VALIDATION_PARITY_DATA);

static DataBindMessagePlan *compile_validation_plan(void) {
  static const char schema[] =
      "message ValidationParityRow { @Min(10) uint32 id; }";
  DataBind *codec = NULL;
  DataBindMessagePlan *plan = NULL;
  DataBindError error = DATA_BIND_ERROR_INIT;
  DataBindMessagePlanDiagnostic diagnostic =
      DATA_BIND_MESSAGE_PLAN_DIAGNOSTIC_INIT;

  if (data_bind_create_from_text(
          schema, sizeof(schema) - 1u, &codec, &error) != DATA_BIND_OK)
    return NULL;
  if (data_bind_message_plan_compile(
          codec, "ValidationParityRow", &ORM_VALIDATION_PARITY_NATIVE,
          &plan, &diagnostic) != DATA_BIND_OK) {
    data_bind_free(codec);
    return NULL;
  }
  data_bind_free(codec);
  return plan;
}

static int load_driver(orm_runtime_t *runtime, const char *id,
                       const char *path, orm_error_t *error) {
  orm_driver_load_config_t load;
  memset(&load, 0, sizeof(load));
  load.struct_size = (uint32_t)sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.module_path = orm_view(path);
  load.expected_driver_id = orm_view(id);
  return orm_runtime_load_driver(runtime, &load, error) == ORM_STATUS_OK;
}

static orm_connection_t *connect_sqlite(
    orm_runtime_t *runtime, orm_error_t *error) {
  orm_config_t config;
  orm_option_t filename;
  orm_connection_t *connection = NULL;

  orm_config(&config);
  filename.keyword = orm_view("filename");
  filename.value = orm_view(":memory:");
  config.driver = orm_view("sqlite");
  config.options = &filename;
  config.option_count = 1u;
  if (orm_runtime_connect(runtime, &config, &connection, error) !=
      ORM_STATUS_OK)
    return NULL;
  return connection;
}

static orm_connection_t *connect_postgresql(
    orm_runtime_t *runtime, const char *conninfo, orm_error_t *error) {
  orm_config_t config;
  orm_option_t option;
  orm_connection_t *connection = NULL;

  orm_config(&config);
  option.keyword = orm_view("conninfo");
  option.value = orm_view(conninfo);
  config.driver = orm_view("postgresql");
  config.options = &option;
  config.option_count = 1u;
  if (orm_runtime_connect(runtime, &config, &connection, error) !=
      ORM_STATUS_OK)
    return NULL;
  return connection;
}

static int validated_value(
    orm_connection_t *connection, const DataBindMessagePlan *plan,
    uint32_t value, int expect_valid, orm_error_t *error) {
  char sql[64];
  orm_query_t *query = NULL;
  orm_flow_config_t flow;
  cflow_publisher publisher = {0};
  orm_validation_parity_row row = {UINT32_MAX};
  cflow_step step;
  int length;

  length = snprintf(sql, sizeof(sql), "select %u as id", (unsigned)value);
  if (length <= 0 || (size_t)length >= sizeof(sql))
    return 0;
  if (orm_raw(connection, orm_view(sql), &query, error) != ORM_STATUS_OK)
    return 0;
  orm_flow_config(&flow, &ORM_VALIDATION_PARITY_DATA);
  if (orm_query_open_validated_flow(
          query, &flow, plan, &publisher, error) != ORM_STATUS_OK) {
    orm_query_destroy(query);
    return 0;
  }
  orm_query_destroy(query);
  query = NULL;

  step = cflow_publisher_resume(&publisher, NULL, &row);
  if (expect_valid) {
    if (step.kind != CFLOW_STEP_VALUE &&
        step.kind != CFLOW_STEP_VALUE_AND_DONE) {
      cflow_publisher_destroy(&publisher);
      return 0;
    }
    if (row.id != value) {
      cflow_publisher_destroy(&publisher);
      return 0;
    }
    if (step.kind == CFLOW_STEP_VALUE) {
      step = cflow_publisher_resume(&publisher, NULL, &row);
      if (step.kind != CFLOW_STEP_DONE) {
        cflow_publisher_destroy(&publisher);
        return 0;
      }
    }
  } else {
    if (step.kind != CFLOW_STEP_ERROR || step.error == NULL ||
        strstr(step.error, "row validation failed") == NULL ||
        strstr(step.error, "id") == NULL || row.id != 0u) {
      cflow_publisher_destroy(&publisher);
      return 0;
    }
  }

  cflow_publisher_destroy(&publisher);
  return 1;
}

spec("SQL Driver DataBind validation parity") {
  it("returns identical validation semantics for SQLite and PostgreSQL") {
    const char *sqlite_plugin = getenv("ORM_SQLITE_PLUGIN");
    const char *postgresql_plugin = getenv("ORM_POSTGRESQL_PLUGIN");
    const char *conninfo = getenv("TURBODB_ORM_PGSQL_TEST_CONNINFO");
    orm_runtime_config_t runtime_config;
    orm_runtime_t *runtime = NULL;
    orm_connection_t *sqlite = NULL;
    orm_connection_t *postgresql = NULL;
    DataBindMessagePlan *plan = NULL;
    orm_error_t error;

    check_not_null(sqlite_plugin);
    check_not_null(postgresql_plugin);
    check_not_null(conninfo);
    if (sqlite_plugin == NULL || postgresql_plugin == NULL || conninfo == NULL)
      return;

    orm_runtime_config_init(&runtime_config);
    orm_error_init(&error);
    check_equal(
        orm_runtime_create(&runtime_config, &runtime, &error),
        ORM_STATUS_OK);
    check_not_null(runtime);
    if (runtime == NULL)
      return;

    check_true(load_driver(runtime, "sqlite", sqlite_plugin, &error));
    check_true(load_driver(runtime, "postgresql", postgresql_plugin, &error));

    sqlite = connect_sqlite(runtime, &error);
    postgresql = connect_postgresql(runtime, conninfo, &error);
    check_not_null(sqlite);
    check_not_null(postgresql);
    if (sqlite == NULL || postgresql == NULL)
      goto cleanup;

    plan = compile_validation_plan();
    check_not_null(plan);
    if (plan == NULL)
      goto cleanup;

    check_true(validated_value(sqlite, plan, 11u, 1, &error));
    check_true(validated_value(postgresql, plan, 11u, 1, &error));
    check_true(validated_value(sqlite, plan, 5u, 0, &error));
    check_true(validated_value(postgresql, plan, 5u, 0, &error));

cleanup:
    data_bind_message_plan_free(plan);
    orm_disconnect(postgresql);
    orm_disconnect(sqlite);
    if (runtime != NULL) {
      check_equal(orm_runtime_close(runtime, &error), ORM_STATUS_OK);
      orm_runtime_release(runtime);
    }
  }
}
