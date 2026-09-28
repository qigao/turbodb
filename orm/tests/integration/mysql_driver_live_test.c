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

static int run_command_in_transaction(
    orm_query_t *query, orm_transaction_t *transaction,
    orm_error_t *error, uint64_t *affected) {
  orm_result_t *result = NULL;
  uint64_t count = 0u;
  orm_status_t status;

  if (affected != NULL)
    *affected = 0u;
  status = orm_query_execute_in_transaction(
      query, transaction, &result, error);
  if (status != ORM_STATUS_OK) {
    orm_result_destroy(result);
    return fail_status(
        "execute MySQL transaction command",
        status, ORM_STATUS_OK, error);
  }
  status = orm_result_affected_rows(
      result, &count, error);
  orm_result_destroy(result);
  if (status != ORM_STATUS_OK)
    return fail_status(
        "read MySQL transaction affected_rows",
        status, ORM_STATUS_OK, error);
  if (affected != NULL)
    *affected = count;
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


static orm_connection_t *connect_database(
    orm_runtime_t *runtime,
    const char *host, const char *port,
    const char *user, const char *password,
    const char *database, const char *ca_file,
    const char *server_name, orm_error_t *error) {
  orm_config_t config;
  orm_option_t options[8];
  orm_connection_t *connection = NULL;

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

  if (orm_runtime_connect(runtime, &config, &connection, error) !=
      ORM_STATUS_OK)
    return NULL;
  return connection;
}

static int update_driver_value(
    orm_connection_t *connection, int64_t delta,
    orm_error_t *error) {
  orm_query_t *query = NULL;
  uint64_t affected = 0u;
  int failed = 0;

  if (orm_raw(
          connection,
          orm_view("UPDATE m4_driver SET n=n+?1 WHERE s=?2"),
          &query, error) != ORM_STATUS_OK ||
      orm_query_bind(query, orm_i64(delta), error) != ORM_STATUS_OK ||
      orm_query_bind(query, orm_i64(-13), error) != ORM_STATUS_OK ||
      run_command(query, error, &affected) != 0 ||
      affected != UINT64_C(1)) {
    fprintf(stderr,
            "MySQL datasource UPDATE failed affected=%llu message=%s\n",
            (unsigned long long)affected,
            error != NULL ? error->message : "");
    failed = 1;
  }
  orm_query_destroy(query);
  return failed;
}

static int read_driver_value(
    orm_connection_t *connection, long expected,
    orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_flow_config_t flow;
  cflow_publisher publisher = {0};
  mysql_driver_live_row row = {0};
  cflow_step step;
  int failed = 0;

  if (orm_raw(
          connection,
          orm_view("SELECT n AS s FROM m4_driver WHERE s=?1"),
          &query, error) != ORM_STATUS_OK ||
      orm_query_bind(query, orm_i64(-13), error) != ORM_STATUS_OK) {
    orm_query_destroy(query);
    return 1;
  }

  orm_flow_config(&flow, &mysql_driver_live_row_data);
  if (orm_query_open_flow(query, &flow, &publisher, error) !=
      ORM_STATUS_OK) {
    orm_query_destroy(query);
    return 1;
  }

  step = cflow_publisher_resume(&publisher, NULL, &row);
  if (step.kind != CFLOW_STEP_VALUE || row.s != expected) {
    fprintf(stderr,
            "MySQL datasource SELECT mismatch step=%d got=%ld expected=%ld\n",
            (int)step.kind, row.s, expected);
    failed = 1;
  }
  if (!failed) {
    step = cflow_publisher_resume(&publisher, NULL, &row);
    if (step.kind != CFLOW_STEP_DONE) {
      fprintf(stderr,
              "MySQL datasource SELECT expected DONE got=%d\n",
              (int)step.kind);
      failed = 1;
    }
  }

  cflow_publisher_destroy(&publisher);
  orm_query_destroy(query);
  return failed;
}


static int structured_insert(
    orm_connection_t *connection, int64_t id, int64_t value,
    orm_error_t *error) {
  orm_query_t *query = NULL;
  uint64_t affected = 0u;
  int failed = 0;

  if (orm_insert(
          connection, orm_view("structured_items"),
          &query, error) != ORM_STATUS_OK ||
      orm_query_set(
          query, orm_view("id"), orm_i64(id), error) !=
          ORM_STATUS_OK ||
      orm_query_set(
          query, orm_view("s"), orm_i64(value), error) !=
          ORM_STATUS_OK ||
      orm_query_set(
          query, orm_view("note"), orm_null(), error) !=
          ORM_STATUS_OK ||
      run_command(query, error, &affected) != 0 ||
      affected != UINT64_C(1)) {
    fprintf(stderr,
            "MySQL structured INSERT failed affected=%llu message=%s\n",
            (unsigned long long)affected,
            error != NULL ? error->message : "");
    failed = 1;
  }
  orm_query_destroy(query);
  return failed;
}

static int structured_update(
    orm_connection_t *connection, int64_t id, int64_t value,
    orm_error_t *error) {
  orm_query_t *query = NULL;
  uint64_t affected = 0u;
  int failed = 0;

  if (orm_update(
          connection, orm_view("structured_items"),
          &query, error) != ORM_STATUS_OK ||
      orm_query_set(
          query, orm_view("s"), orm_i64(value), error) !=
          ORM_STATUS_OK ||
      orm_query_where(
          query, orm_view("id"), ORM_COMPARE_EQUAL,
          orm_i64(id), error) != ORM_STATUS_OK ||
      run_command(query, error, &affected) != 0 ||
      affected != UINT64_C(1)) {
    fprintf(stderr,
            "MySQL structured UPDATE failed affected=%llu message=%s\n",
            (unsigned long long)affected,
            error != NULL ? error->message : "");
    failed = 1;
  }
  orm_query_destroy(query);
  return failed;
}

static int structured_delete(
    orm_connection_t *connection, int64_t id,
    orm_error_t *error) {
  orm_query_t *query = NULL;
  uint64_t affected = 0u;
  int failed = 0;

  if (orm_delete(
          connection, orm_view("structured_items"),
          &query, error) != ORM_STATUS_OK ||
      orm_query_where(
          query, orm_view("id"), ORM_COMPARE_EQUAL,
          orm_i64(id), error) != ORM_STATUS_OK ||
      run_command(query, error, &affected) != 0 ||
      affected != UINT64_C(1)) {
    fprintf(stderr,
            "MySQL structured DELETE failed affected=%llu message=%s\n",
            (unsigned long long)affected,
            error != NULL ? error->message : "");
    failed = 1;
  }
  orm_query_destroy(query);
  return failed;
}

static int structured_read(
    orm_connection_t *connection, int64_t id,
    long expected, int expect_present,
    orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_flow_config_t flow;
  cflow_publisher publisher = {0};
  mysql_driver_live_row row = {0};
  cflow_step step;
  int failed = 0;

  if (orm_query_create(
          connection, orm_view("structured_items"),
          &query, error) != ORM_STATUS_OK ||
      orm_query_add_column(
          query, orm_view("s"), error) != ORM_STATUS_OK ||
      orm_query_where(
          query, orm_view("id"), ORM_COMPARE_EQUAL,
          orm_i64(id), error) != ORM_STATUS_OK) {
    orm_query_destroy(query);
    return 1;
  }

  orm_flow_config(&flow, &mysql_driver_live_row_data);
  if (orm_query_open_flow(
          query, &flow, &publisher, error) != ORM_STATUS_OK) {
    orm_query_destroy(query);
    return 1;
  }

  step = cflow_publisher_resume(&publisher, NULL, &row);
  if (expect_present) {
    if ((step.kind != CFLOW_STEP_VALUE &&
         step.kind != CFLOW_STEP_VALUE_AND_DONE) ||
        row.s != expected) {
      fprintf(stderr,
              "MySQL structured SELECT mismatch step=%d got=%ld expected=%ld\n",
              (int)step.kind, row.s, expected);
      failed = 1;
    } else if (step.kind == CFLOW_STEP_VALUE) {
      step = cflow_publisher_resume(&publisher, NULL, &row);
      if (step.kind != CFLOW_STEP_DONE) {
        fprintf(stderr,
                "MySQL structured SELECT expected DONE got=%d\n",
                (int)step.kind);
        failed = 1;
      }
    }
  } else if (step.kind != CFLOW_STEP_DONE) {
    fprintf(stderr,
            "MySQL structured SELECT expected empty result got=%d\n",
            (int)step.kind);
    failed = 1;
  }

  cflow_publisher_destroy(&publisher);
  orm_query_destroy(query);
  return failed;
}


static int view_equals(
    orm_string_view_t value, const char *expected) {
  const size_t size = strlen(expected);
  return value.len == size &&
         (size == 0u || memcmp(value.data, expected, size) == 0);
}

static int qualify_live_values(
    orm_connection_t *connection, orm_error_t *error) {
  static const unsigned char expected_blob[] = {
      0x00u, 0x7fu, 0xffu};
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_string_view_t text = {0};
  orm_blob_t blob = {0};
  uint64_t u = 0u;
  uint64_t rows = 0u;
  uint64_t columns = 0u;
  uint8_t is_null = 0u;
  int failed = 0;

  if (orm_raw(
          connection,
          orm_view(
              "SELECT n_null,u,b,decv,d,t,dt "
              "FROM m5_values WHERE id=?1"),
          &query, error) != ORM_STATUS_OK ||
      orm_query_bind(
          query, orm_i64(1), error) != ORM_STATUS_OK ||
      orm_query_execute(
          query, &result, error) != ORM_STATUS_OK) {
    fprintf(stderr,
            "MySQL live value SELECT failed: %s\n",
            error != NULL ? error->message : "");
    failed = 1;
    goto cleanup;
  }

  if (orm_result_row_count(
          result, &rows, error) != ORM_STATUS_OK ||
      orm_result_column_count(
          result, &columns, error) != ORM_STATUS_OK ||
      rows != UINT64_C(1) || columns != UINT64_C(7)) {
    fprintf(stderr,
            "MySQL live value shape mismatch rows=%llu columns=%llu message=%s\n",
            (unsigned long long)rows,
            (unsigned long long)columns,
            error != NULL ? error->message : "");
    failed = 1;
    goto cleanup;
  }

  if (orm_result_is_null(
          result, 0u, 0u, &is_null, error) != ORM_STATUS_OK ||
      is_null != 1u) {
    fprintf(stderr, "MySQL live NULL mapping failed: %s\n",
            error != NULL ? error->message : "");
    failed = 1;
  }

  if (!failed &&
      (orm_result_get_uint64(
           result, 0u, 1u, &u, error) != ORM_STATUS_OK ||
       u != UINT64_MAX)) {
    fprintf(stderr,
            "MySQL live uint64 mapping failed got=%llu message=%s\n",
            (unsigned long long)u,
            error != NULL ? error->message : "");
    failed = 1;
  }

  if (!failed &&
      (orm_result_get_blob(
           result, 0u, 2u, &blob, error) != ORM_STATUS_OK ||
       blob.size != sizeof(expected_blob) ||
       memcmp(blob.data, expected_blob, sizeof(expected_blob)) != 0)) {
    fprintf(stderr,
            "MySQL live BLOB mapping failed size=%zu message=%s\n",
            blob.size, error != NULL ? error->message : "");
    failed = 1;
  }

  if (!failed &&
      (orm_result_get_text(
           result, 0u, 3u, &text, error) != ORM_STATUS_OK ||
       !view_equals(text, "1234567890123.456789"))) {
    fprintf(stderr,
            "MySQL live DECIMAL mapping failed value=%.*s message=%s\n",
            (int)text.len, text.data != NULL ? text.data : "",
            error != NULL ? error->message : "");
    failed = 1;
  }

  if (!failed &&
      (orm_result_get_text(
           result, 0u, 4u, &text, error) != ORM_STATUS_OK ||
       !view_equals(text, "2026-09-28"))) {
    fprintf(stderr,
            "MySQL live DATE mapping failed value=%.*s message=%s\n",
            (int)text.len, text.data != NULL ? text.data : "",
            error != NULL ? error->message : "");
    failed = 1;
  }

  if (!failed &&
      (orm_result_get_text(
           result, 0u, 5u, &text, error) != ORM_STATUS_OK ||
       !view_equals(text, "12:34:56.123456"))) {
    fprintf(stderr,
            "MySQL live TIME mapping failed value=%.*s message=%s\n",
            (int)text.len, text.data != NULL ? text.data : "",
            error != NULL ? error->message : "");
    failed = 1;
  }

  if (!failed &&
      (orm_result_get_text(
           result, 0u, 6u, &text, error) != ORM_STATUS_OK ||
       !view_equals(text, "2026-09-28 12:34:56.654321"))) {
    fprintf(stderr,
            "MySQL live DATETIME mapping failed value=%.*s message=%s\n",
            (int)text.len, text.data != NULL ? text.data : "",
            error != NULL ? error->message : "");
    failed = 1;
  }

cleanup:
  orm_result_destroy(result);
  orm_query_destroy(query);
  return failed;
}

static int qualify_constraint(
    orm_connection_t *connection, orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_status_t status;
  int failed = 0;

  if (orm_insert(
          connection, orm_view("m5_constraint"),
          &query, error) != ORM_STATUS_OK ||
      orm_query_set(
          query, orm_view("id"), orm_i64(1), error) !=
          ORM_STATUS_OK ||
      orm_query_set(
          query, orm_view("note"), orm_text("duplicate"), error) !=
          ORM_STATUS_OK) {
    fprintf(stderr,
            "prepare MySQL duplicate constraint query failed: %s\n",
            error != NULL ? error->message : "");
    failed = 1;
    goto cleanup;
  }

  status = orm_query_execute(query, &result, error);
  if (status != ORM_STATUS_CONSTRAINT || result != NULL) {
    fprintf(stderr,
            "MySQL duplicate constraint expected=%d got=%d result=%p message=%s\n",
            (int)ORM_STATUS_CONSTRAINT, (int)status,
            (void *)result,
            error != NULL ? error->message : "");
    failed = 1;
  }

cleanup:
  orm_result_destroy(result);
  orm_query_destroy(query);
  return failed;
}

static int qualify_row_limit(
    orm_runtime_t *runtime,
    const char *host, const char *port,
    const char *user, const char *password,
    const char *database, const char *ca_file,
    const char *server_name, orm_error_t *error) {
  orm_config_t config;
  orm_option_t options[8];
  orm_connection_t *connection = NULL;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_status_t status;
  int failed = 0;

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
  config.max_result_rows = 1u;

  if (orm_runtime_connect(
          runtime, &config, &connection, error) != ORM_STATUS_OK) {
    fprintf(stderr,
            "connect bounded MySQL datasource failed: %s\n",
            error != NULL ? error->message : "");
    return 1;
  }

  if (orm_raw(
          connection,
          orm_view("SELECT 1 AS s UNION ALL SELECT 2 AS s"),
          &query, error) != ORM_STATUS_OK) {
    failed = 1;
    goto cleanup;
  }

  status = orm_query_execute(query, &result, error);
  if (status != ORM_STATUS_LIMIT_EXCEEDED || result != NULL) {
    fprintf(stderr,
            "MySQL row limit expected=%d got=%d result=%p message=%s\n",
            (int)ORM_STATUS_LIMIT_EXCEEDED, (int)status,
            (void *)result,
            error != NULL ? error->message : "");
    failed = 1;
  }

cleanup:
  orm_result_destroy(result);
  orm_query_destroy(query);
  orm_disconnect(connection);
  return failed;
}


static int qualify_cancel_lease(
    orm_runtime_t *runtime,
    const char *host, const char *port,
    const char *user, const char *password,
    const char *database, const char *ca_file,
    const char *server_name, orm_error_t *error) {
  orm_connection_t *connection = NULL;
  orm_query_t *query = NULL;
  orm_query_t *probe = NULL;
  orm_result_t *probe_result = NULL;
  orm_flow_config_t flow;
  cflow_publisher publisher = {0};
  mysql_driver_live_row row = {0};
  cflow_step step;
  int64_t value = 0;
  int failed = 0;

  connection = connect_database(
      runtime, host, port, user, password,
      database, ca_file, server_name, error);
  if (connection == NULL) {
    fprintf(stderr,
            "connect MySQL cancel datasource failed: %s\n",
            error != NULL ? error->message : "");
    return 1;
  }

  if (orm_raw(
          connection,
          orm_view(
              "WITH RECURSIVE seq(n) AS ("
              "SELECT 1 UNION ALL SELECT n+1 FROM seq WHERE n<32"
              ") SELECT n AS s FROM seq"),
          &query, error) != ORM_STATUS_OK) {
    failed = 1;
    goto cleanup;
  }

  orm_flow_config(&flow, &mysql_driver_live_row_data);
  if (orm_query_open_flow(
          query, &flow, &publisher, error) != ORM_STATUS_OK) {
    failed = 1;
    goto cleanup;
  }

  step = cflow_publisher_resume(&publisher, NULL, &row);
  if (step.kind != CFLOW_STEP_VALUE || row.s != 1L) {
    fprintf(stderr,
            "MySQL cancel publisher first row mismatch step=%d value=%ld\n",
            (int)step.kind, row.s);
    failed = 1;
    goto cleanup;
  }

  if (orm_query_close(query, error) != ORM_STATUS_BUSY ||
      orm_connection_close(connection, error) != ORM_STATUS_BUSY) {
    fprintf(stderr,
            "MySQL live Publisher did not retain query/connection lease: %s\n",
            error != NULL ? error->message : "");
    failed = 1;
    goto cleanup;
  }

  cflow_publisher_cancel(&publisher);

  if (orm_query_close(query, error) != ORM_STATUS_BUSY ||
      orm_connection_close(connection, error) != ORM_STATUS_BUSY) {
    fprintf(stderr,
            "MySQL cancel released ownership before Publisher terminal: %s\n",
            error != NULL ? error->message : "");
    failed = 1;
    goto cleanup;
  }

  cflow_publisher_destroy(&publisher);
  memset(&publisher, 0, sizeof(publisher));

  if (orm_query_close(query, error) != ORM_STATUS_OK) {
    fprintf(stderr,
            "MySQL query stayed BUSY after Publisher destroy: %s\n",
            error != NULL ? error->message : "");
    failed = 1;
    goto cleanup;
  }
  orm_query_release(query);
  query = NULL;

  /*
   * Early cancellation closes only the in-flight physical CNet session.
   * The logical datasource remains healthy and can create a fresh session.
   */
  if (orm_raw(
          connection, orm_view("SELECT 42"),
          &probe, error) != ORM_STATUS_OK ||
      orm_query_execute(
          probe, &probe_result, error) != ORM_STATUS_OK ||
      orm_result_get_int64(
          probe_result, 0u, 0u, &value, error) != ORM_STATUS_OK ||
      value != INT64_C(42)) {
    fprintf(stderr,
            "MySQL datasource unusable after early cancel value=%lld message=%s\n",
            (long long)value,
            error != NULL ? error->message : "");
    failed = 1;
    goto cleanup;
  }

  orm_result_destroy(probe_result);
  probe_result = NULL;
  orm_query_destroy(probe);
  probe = NULL;

  if (orm_connection_close(connection, error) != ORM_STATUS_OK) {
    fprintf(stderr,
            "MySQL connection stayed BUSY after cancel terminal: %s\n",
            error != NULL ? error->message : "");
    failed = 1;
    goto cleanup;
  }
  orm_connection_release(connection);
  connection = NULL;

cleanup:
  if (cflow_publisher_valid(&publisher))
    cflow_publisher_destroy(&publisher);
  orm_result_destroy(probe_result);
  orm_query_destroy(probe);
  if (query != NULL) {
    (void)orm_query_close(query, error);
    orm_query_release(query);
  }
  if (connection != NULL) {
    (void)orm_connection_close(connection, error);
    orm_connection_release(connection);
  }
  return failed;
}

int main(void) {
  const char *module = getenv("ORM_MYSQL_PLUGIN");
  const char *host = getenv("ORM_MYSQL_HOST");
  const char *port = getenv("ORM_MYSQL_PORT");
  const char *user = getenv("ORM_MYSQL_USER");
  const char *password = getenv("ORM_MYSQL_PASSWORD");
  const char *database_a = getenv("ORM_MYSQL_DATABASE");
  const char *database_b = getenv("ORM_MYSQL_DATABASE_B");
  const char *ca_file = getenv("ORM_MYSQL_CA_FILE");
  const char *server_name = getenv("ORM_MYSQL_SERVER_NAME");
  orm_runtime_config_t runtime_config;
  orm_runtime_t *runtime = NULL;
  orm_driver_load_config_t load;
  orm_connection_t *connection_a = NULL;
  orm_connection_t *connection_b = NULL;
  orm_transaction_t *transaction_a = NULL;
  orm_error_t error;
  int failed = 0;

  if (module == NULL || module[0] == '\0' ||
      host == NULL || host[0] == '\0' ||
      port == NULL || port[0] == '\0' ||
      user == NULL || password == NULL ||
      database_a == NULL || database_a[0] == '\0' ||
      database_b == NULL || database_b[0] == '\0' ||
      ca_file == NULL || ca_file[0] == '\0' ||
      server_name == NULL || server_name[0] == '\0') {
    fprintf(stderr, "MySQL Driver live environment is incomplete\n");
    return 1;
  }

  orm_error_init(&error);
  orm_runtime_config_init(&runtime_config);
  if (fail_status(
          "create MySQL runtime",
          orm_runtime_create(&runtime_config, &runtime, &error),
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

  if (qualify_cancel_lease(
          runtime, host, port, user, password,
          database_a, ca_file, server_name, &error) != 0) {
    failed = 1;
    goto cleanup;
  }

  connection_a = connect_database(
      runtime, host, port, user, password,
      database_a, ca_file, server_name, &error);
  if (connection_a == NULL) {
    fprintf(stderr, "connect MySQL datasource A failed: %s\n", error.message);
    failed = 1;
    goto cleanup;
  }

  connection_b = connect_database(
      runtime, host, port, user, password,
      database_b, ca_file, server_name, &error);
  if (connection_b == NULL) {
    fprintf(stderr, "connect MySQL datasource B failed: %s\n", error.message);
    failed = 1;
    goto cleanup;
  }

  if (read_probe(connection_a, &error) != 0)
    failed = 1;

  if (!failed && qualify_live_values(connection_a, &error) != 0)
    failed = 1;
  if (!failed && qualify_constraint(connection_a, &error) != 0)
    failed = 1;
  if (!failed &&
      qualify_row_limit(
          runtime, host, port, user, password,
          database_a, ca_file, server_name, &error) != 0)
    failed = 1;

  if (!failed && update_driver_value(connection_a, INT64_C(1), &error) != 0)
    failed = 1;
  if (!failed &&
      (read_driver_value(connection_a, 1L, &error) != 0 ||
       read_driver_value(connection_b, 100L, &error) != 0))
    failed = 1;

  /*
   * A transaction on datasource A must be private to A. While A owns a
   * persistent transaction session, datasource B must still admit and execute
   * its own command through the same loaded Driver module.
   */
  if (!failed &&
      orm_transaction_begin(
          connection_a, ORM_ISOLATION_READ_COMMITTED,
          &transaction_a, &error) != ORM_STATUS_OK) {
    fprintf(stderr, "begin datasource A transaction failed: %s\n",
            error.message);
    failed = 1;
  }

  if (!failed && update_driver_value(connection_b, INT64_C(10), &error) != 0)
    failed = 1;
  if (!failed && read_driver_value(connection_b, 110L, &error) != 0)
    failed = 1;

  if (transaction_a != NULL) {
    if (orm_transaction_rollback(transaction_a, &error) != ORM_STATUS_OK) {
      fprintf(stderr, "rollback datasource A transaction failed: %s\n",
              error.message);
      failed = 1;
    }
    orm_transaction_destroy(transaction_a);
    transaction_a = NULL;
  }

  if (!failed &&
      (read_driver_value(connection_a, 1L, &error) != 0 ||
       read_driver_value(connection_b, 110L, &error) != 0))
    failed = 1;

  /*
   * Structured CRUD is lowered only inside the MySQL Driver. It must use the
   * same prepared binary command/cursor path as RAW SQL and preserve datasource
   * isolation under one loaded Driver module.
   */
  if (!failed &&
      (structured_insert(connection_a, INT64_C(1), INT64_C(41), &error) != 0 ||
       structured_insert(connection_b, INT64_C(1), INT64_C(141), &error) != 0))
    failed = 1;
  if (!failed &&
      (structured_read(connection_a, INT64_C(1), 41L, 1, &error) != 0 ||
       structured_read(connection_b, INT64_C(1), 141L, 1, &error) != 0))
    failed = 1;

  if (!failed &&
      orm_transaction_begin(
          connection_a, ORM_ISOLATION_READ_COMMITTED,
          &transaction_a, &error) != ORM_STATUS_OK) {
    fprintf(stderr,
            "begin structured MySQL transaction failed: %s\n",
            error.message);
    failed = 1;
  }
  if (!failed) {
    orm_query_t *tx_query = NULL;
    uint64_t tx_affected = 0u;
    if (orm_update(
            connection_a, orm_view("structured_items"),
            &tx_query, &error) != ORM_STATUS_OK ||
        orm_query_set(
            tx_query, orm_view("s"), orm_i64(99), &error) !=
            ORM_STATUS_OK ||
        orm_query_where(
            tx_query, orm_view("id"), ORM_COMPARE_EQUAL,
            orm_i64(1), &error) != ORM_STATUS_OK ||
        run_command_in_transaction(
            tx_query, transaction_a, &error,
            &tx_affected) != 0 ||
        tx_affected != UINT64_C(1)) {
      fprintf(stderr,
              "structured transaction UPDATE failed affected=%llu message=%s\n",
              (unsigned long long)tx_affected, error.message);
      failed = 1;
    }
    orm_query_destroy(tx_query);
  }
  if (transaction_a != NULL) {
    if (orm_transaction_rollback(
            transaction_a, &error) != ORM_STATUS_OK) {
      fprintf(stderr,
              "rollback structured MySQL transaction failed: %s\n",
              error.message);
      failed = 1;
    }
    orm_transaction_destroy(transaction_a);
    transaction_a = NULL;
  }
  if (!failed &&
      structured_read(
          connection_a, INT64_C(1), 41L, 1, &error) != 0)
    failed = 1;

  if (!failed &&
      structured_update(
          connection_a, INT64_C(1), INT64_C(42), &error) != 0)
    failed = 1;
  if (!failed &&
      (structured_read(connection_a, INT64_C(1), 42L, 1, &error) != 0 ||
       structured_read(connection_b, INT64_C(1), 141L, 1, &error) != 0))
    failed = 1;
  if (!failed &&
      structured_delete(connection_a, INT64_C(1), &error) != 0)
    failed = 1;
  if (!failed &&
      (structured_read(connection_a, INT64_C(1), 0L, 0, &error) != 0 ||
       structured_read(connection_b, INT64_C(1), 141L, 1, &error) != 0))
    failed = 1;

  orm_error_init(&error);
  if (orm_runtime_close(runtime, &error) != ORM_STATUS_BUSY) {
    fprintf(stderr,
            "two live MySQL connections did not retain Plugin leases: %s\n",
            error.message);
    failed = 1;
  }

  orm_disconnect(connection_a);
  connection_a = NULL;

  orm_error_init(&error);
  if (orm_runtime_close(runtime, &error) != ORM_STATUS_BUSY) {
    fprintf(stderr,
            "datasource B did not retain its independent Plugin lease: %s\n",
            error.message);
    failed = 1;
  }
  if (read_driver_value(connection_b, 110L, &error) != 0)
    failed = 1;

cleanup:
  if (transaction_a != NULL) {
    (void)orm_transaction_rollback(transaction_a, &error);
    orm_transaction_destroy(transaction_a);
  }
  orm_disconnect(connection_b);
  orm_disconnect(connection_a);
  connection_b = NULL;
  connection_a = NULL;

  orm_error_init(&error);
  if (runtime != NULL &&
      orm_runtime_close(runtime, &error) != ORM_STATUS_OK) {
    fprintf(stderr, "close MySQL runtime failed: %s\n", error.message);
    failed = 1;
  }
  orm_runtime_release(runtime);
  return failed;
}
