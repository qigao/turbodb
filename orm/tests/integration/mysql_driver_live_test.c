#include "orm.h"
#include <orm_runtime.h>

#include <cmeta/struct.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MYSQL_DRIVER_LIVE_DATA_PREFIX_SIZE                                  \
  (offsetof(cmeta_data_desc, shape) +                                      \
   sizeof(((cmeta_data_desc *)0)->shape))

Struct(mysql_driver_live_row, (long, s));

static const cmeta_type_identity mysql_driver_live_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("orm.test.MySQLDriverLiveRow");
static const cmeta_type_traits mysql_driver_live_row_traits = {
    .flags = CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY};
static const cmeta_type_desc mysql_driver_live_row_type = {
    .name = "mysql_driver_live_row",
    .size = sizeof(mysql_driver_live_row),
    .align = _Alignof(mysql_driver_live_row),
    .kind = CMETA_T_OBJECT,
    .traits = &mysql_driver_live_row_traits,
    .identity = &mysql_driver_live_row_identity};
static const cmeta_data_field_desc mysql_driver_live_row_fields[] = {
    {"orm.test.MySQLDriverLiveRow.s", "s",
     offsetof(mysql_driver_live_row, s), &cmeta_data_long}};
static const cmeta_data_struct_shape mysql_driver_live_row_shape = {
    .layout = StructMeta(mysql_driver_live_row),
    .fields = mysql_driver_live_row_fields,
    .field_count = 1u};
static const cmeta_data_desc mysql_driver_live_row_data = {
    .struct_size = MYSQL_DRIVER_LIVE_DATA_PREFIX_SIZE,
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "orm.test.MySQLDriverLiveRow.data",
    .display_name = "MySQLDriverLiveRow",
    .kind = CMETA_DATA_STRUCT,
    .storage_type = &mysql_driver_live_row_type,
    .shape = &mysql_driver_live_row_shape};

static int fail_status(
    const char *operation, orm_status_t got,
    orm_status_t expected, const orm_error_t *error) {
  if (got == expected)
    return 0;
  fprintf(stderr, "%s: expected=%d got=%d message=%s\n",
          operation, (int)expected, (int)got,
          error != NULL ? error->message : "");
  return 1;
}

static int run_command(
    orm_query_t *query, orm_error_t *error,
    uint64_t *affected) {
  cflow_publisher publisher = {0};
  orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
  cflow_step step;
  orm_status_t status =
      orm_query_open_command_flow(query, &publisher, error);
  if (status != ORM_STATUS_OK)
    return fail_status(
        "open MySQL command Publisher",
        status, ORM_STATUS_OK, error);

  step = cflow_publisher_resume(&publisher, NULL, &result);
  cflow_publisher_destroy(&publisher);
  if (step.kind != CFLOW_STEP_VALUE_AND_DONE) {
    fprintf(stderr, "MySQL command Publisher step=%d message=%s\n",
            (int)step.kind,
            step.error != NULL ? step.error : "");
    return 1;
  }
  if (affected != NULL)
    *affected = result.affected_rows;
  return 0;
}

static int read_probe(
    orm_connection_t *connection, orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_flow_config_t flow;
  cflow_publisher publisher = {0};
  mysql_driver_live_row row = {0};
  cflow_step step;
  int failed = 0;

  if (fail_status(
          "create MySQL RAW SELECT",
          orm_raw(
              connection,
              orm_view("SELECT s FROM m3_probe WHERE s=?1"),
              &query, error),
          ORM_STATUS_OK, error))
    return 1;
  if (fail_status(
          "bind MySQL RAW SELECT parameter",
          orm_query_bind(query, orm_i64(-42), error),
          ORM_STATUS_OK, error)) {
    orm_query_destroy(query);
    return 1;
  }

  orm_flow_config(&flow, &mysql_driver_live_row_data);
  if (fail_status(
          "open MySQL RAW row Publisher",
          orm_query_open_flow(query, &flow, &publisher, error),
          ORM_STATUS_OK, error)) {
    orm_query_destroy(query);
    return 1;
  }

  step = cflow_publisher_resume(&publisher, NULL, &row);
  if (step.kind != CFLOW_STEP_VALUE || row.s != -42L) {
    fprintf(stderr, "MySQL RAW SELECT mismatch step=%d s=%ld\n",
            (int)step.kind, row.s);
    failed = 1;
  }
  if (!failed) {
    step = cflow_publisher_resume(&publisher, NULL, &row);
    if (step.kind != CFLOW_STEP_DONE) {
      fprintf(stderr,
              "MySQL RAW SELECT expected DONE got=%d\n",
              (int)step.kind);
      failed = 1;
    }
  }

  cflow_publisher_destroy(&publisher);
  orm_query_destroy(query);
  return failed;
}

int main(void) {
  const char *module = getenv("ORM_MYSQL_PLUGIN");
  const char *host = getenv("ORM_MYSQL_HOST");
  const char *port = getenv("ORM_MYSQL_PORT");
  const char *user = getenv("ORM_MYSQL_USER");
  const char *password = getenv("ORM_MYSQL_PASSWORD");
  const char *database = getenv("ORM_MYSQL_DATABASE");
  const char *ca_file = getenv("ORM_MYSQL_CA_FILE");
  const char *server_name = getenv("ORM_MYSQL_SERVER_NAME");
  orm_runtime_config_t runtime_config;
  orm_runtime_t *runtime = NULL;
  orm_driver_load_config_t load;
  orm_config_t config;
  orm_option_t options[8];
  orm_connection_t *connection = NULL;
  orm_query_t *query = NULL;
  orm_error_t error;
  uint64_t affected = 0u;
  int failed = 0;

  if (module == NULL || module[0] == '\0' ||
      host == NULL || host[0] == '\0' ||
      port == NULL || port[0] == '\0' ||
      user == NULL || password == NULL ||
      database == NULL || database[0] == '\0' ||
      ca_file == NULL || ca_file[0] == '\0' ||
      server_name == NULL || server_name[0] == '\0') {
    fprintf(stderr, "MySQL Driver live environment is incomplete\n");
    return 1;
  }

  orm_error_init(&error);
  orm_runtime_config_init(&runtime_config);
  if (fail_status(
          "create MySQL runtime",
          orm_runtime_create(
              &runtime_config, &runtime, &error),
          ORM_STATUS_OK, &error))
    return 1;

  memset(&load, 0, sizeof(load));
  load.struct_size = (uint32_t)sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.module_path = orm_view(module);
  load.expected_driver_id = orm_view("mysql");
  if (fail_status(
          "load MySQL Driver",
          orm_runtime_load_driver(runtime, &load, &error),
          ORM_STATUS_OK, &error)) {
    orm_runtime_release(runtime);
    return 1;
  }

  orm_config(&config);
  options[0] = (orm_option_t){orm_view("host"), orm_view(host)};
  options[1] = (orm_option_t){orm_view("port"), orm_view(port)};
  options[2] = (orm_option_t){orm_view("username"), orm_view(user)};
  options[3] = (orm_option_t){orm_view("password"), orm_view(password)};
  options[4] = (orm_option_t){orm_view("database"), orm_view(database)};
  options[5] = (orm_option_t){orm_view("ca_file"), orm_view(ca_file)};
  options[6] = (orm_option_t){orm_view("server_name"), orm_view(server_name)};
  options[7] = (orm_option_t){orm_view("timeout_ms"), orm_view("5000")};
  config.driver = orm_view("mysql");
  config.options = options;
  config.option_count = 8u;

  if (fail_status(
          "connect MySQL Driver",
          orm_runtime_connect(
              runtime, &config, &connection, &error),
          ORM_STATUS_OK, &error)) {
    (void)orm_runtime_close(runtime, &error);
    orm_runtime_release(runtime);
    return 1;
  }

  if (read_probe(connection, &error) != 0)
    failed = 1;

  if (!failed) {
    if (orm_raw(
            connection,
            orm_view(
                "UPDATE m4_driver SET n=n+?1 WHERE s=?2"),
            &query, &error) != ORM_STATUS_OK ||
        orm_query_bind(query, orm_i64(1), &error) != ORM_STATUS_OK ||
        orm_query_bind(query, orm_i64(-13), &error) != ORM_STATUS_OK ||
        run_command(query, &error, &affected) != 0 ||
        affected != UINT64_C(1)) {
      fprintf(stderr,
              "MySQL runtime Driver UPDATE failed affected=%llu message=%s\n",
              (unsigned long long)affected, error.message);
      failed = 1;
    }
    orm_query_destroy(query);
    query = NULL;
  }

  orm_error_init(&error);
  if (orm_runtime_close(runtime, &error) != ORM_STATUS_BUSY) {
    fprintf(stderr,
            "live MySQL connection did not retain Plugin lease: %s\n",
            error.message);
    failed = 1;
  }

  orm_disconnect(connection);
  connection = NULL;
  orm_error_init(&error);
  if (orm_runtime_close(runtime, &error) != ORM_STATUS_OK) {
    fprintf(stderr,
            "close MySQL runtime failed: %s\n",
            error.message);
    failed = 1;
  }
  orm_runtime_release(runtime);
  return failed;
}
