#include <orm_runtime.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinytest.h"

static orm_status_t mysql_live_fail(orm_error_t *error, const char *message) {
  if (error != NULL && error->struct_size >= sizeof(*error)) {
    error->status = ORM_STATUS_INTERNAL_ERROR;
    (void)snprintf(error->message, sizeof(error->message), "%s", message);
  }
  return ORM_STATUS_INTERNAL_ERROR;
}

static orm_status_t mysql_live_execute(
    orm_query_t *query, orm_transaction_t *transaction,
    orm_result_t **out_result, orm_error_t *error) {
  return transaction == NULL
             ? orm_query_execute(query, out_result, error)
             : orm_query_execute_in_transaction(query, transaction,
                                                out_result, error);
}

static orm_status_t mysql_live_raw(
    orm_connection_t *connection, const char *sql,
    orm_result_t **out_result, orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_status_t status =
      orm_raw(connection, orm_view(sql), &query, error);
  if (status == ORM_STATUS_OK)
    status = orm_query_execute(query, &result, error);
  orm_query_destroy(query);
  if (status != ORM_STATUS_OK) {
    orm_result_destroy(result);
    return status;
  }
  if (out_result != NULL)
    *out_result = result;
  else
    orm_result_destroy(result);
  return ORM_STATUS_OK;
}

static orm_status_t mysql_live_insert(
    orm_connection_t *connection, orm_transaction_t *transaction,
    uint64_t id, const char *name,
    const unsigned char *payload, size_t payload_size,
    int64_t score, uint64_t *affected_rows,
    orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_status_t status =
      orm_insert(connection, orm_view("turbodb_mysql_live"), &query, error);
  if (status == ORM_STATUS_OK)
    status = orm_query_set(query, orm_view("id"), orm_u64(id), error);
  if (status == ORM_STATUS_OK)
    status = orm_query_set(query, orm_view("name"), orm_text(name), error);
  if (status == ORM_STATUS_OK)
    status = orm_query_set(query, orm_view("payload"),
                           orm_blob(payload, payload_size), error);
  if (status == ORM_STATUS_OK)
    status = orm_query_set(query, orm_view("score"), orm_i64(score), error);
  if (status == ORM_STATUS_OK)
    status = mysql_live_execute(query, transaction, &result, error);
  if (status == ORM_STATUS_OK && affected_rows != NULL)
    status = orm_result_affected_rows(result, affected_rows, error);
  orm_result_destroy(result);
  orm_query_destroy(query);
  return status;
}

static orm_status_t mysql_live_row_count(
    orm_connection_t *connection, uint64_t id,
    uint64_t expected, orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  uint64_t rows = 0u;
  orm_status_t status =
      orm_query_create(connection, orm_view("turbodb_mysql_live"),
                       &query, error);
  if (status == ORM_STATUS_OK)
    status = orm_query_add_column(query, orm_view("id"), error);
  if (status == ORM_STATUS_OK)
    status = orm_query_where(query, orm_view("id"),
                             ORM_COMPARE_EQUAL, orm_u64(id), error);
  if (status == ORM_STATUS_OK)
    status = orm_query_execute(query, &result, error);
  if (status == ORM_STATUS_OK)
    status = orm_result_row_count(result, &rows, error);
  if (status == ORM_STATUS_OK && rows != expected)
    status = mysql_live_fail(error, "MySQL row-count mismatch");
  orm_result_destroy(result);
  orm_query_destroy(query);
  return status;
}

static orm_status_t mysql_live_run(orm_error_t *error) {
  static const unsigned char payload[] = {0x00u, 0x7fu, 0xffu, 0x11u};
  const char *host = getenv("ORM_MYSQL_HOST");
  const char *user = getenv("ORM_MYSQL_USER");
  const char *password = getenv("ORM_MYSQL_PASSWORD");
  const char *database = getenv("ORM_MYSQL_DATABASE");
  const char *port = getenv("ORM_MYSQL_PORT");
  orm_option_t options[5];
  orm_runtime_config_t runtime_config;
  orm_runtime_t *runtime = NULL;
  orm_driver_load_config_t load;
  orm_config_t config;
  orm_connection_t *connection = NULL;
  orm_transaction_t *transaction = NULL;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_string_view_t text = {0};
  orm_blob_t blob = {0};
  uint64_t affected = 0u;
  uint64_t unsigned_value = 0u;
  int64_t signed_value = 0;
  orm_status_t status;

  options[0] = (orm_option_t){orm_view("host"), orm_view(host)};
  options[1] = (orm_option_t){orm_view("user"), orm_view(user)};
  options[2] = (orm_option_t){orm_view("password"), orm_view(password)};
  options[3] = (orm_option_t){orm_view("database"), orm_view(database)};
  options[4] = (orm_option_t){orm_view("port"), orm_view(port)};

  orm_runtime_config_init(&runtime_config);
  status = orm_runtime_create(&runtime_config, &runtime, error);
  if (status != ORM_STATUS_OK)
    goto cleanup;

  memset(&load, 0, sizeof(load));
  load.struct_size = (uint32_t)sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.module_path = orm_view(getenv("ORM_MYSQL_PLUGIN"));
  load.expected_driver_id = orm_view("mysql");
  status = orm_runtime_load_driver(runtime, &load, error);
  if (status != ORM_STATUS_OK)
    goto cleanup;

  orm_config(&config);
  config.driver = orm_view("mysql");
  config.options = options;
  config.option_count = 5u;
  config.max_result_rows = 32u;
  status = orm_runtime_connect(runtime, &config, &connection, error);
  if (status != ORM_STATUS_OK)
    goto cleanup;

  status = mysql_live_raw(
      connection, "drop table if exists turbodb_mysql_live", NULL, error);
  if (status == ORM_STATUS_OK)
    status = mysql_live_raw(
        connection,
        "create table turbodb_mysql_live("
        "id bigint unsigned primary key,"
        "name varchar(128) not null,"
        "payload varbinary(64) not null,"
        "score bigint not null,"
        "exact_value decimal(20,4) not null default 1234567890123456.1250,"
        "occurred datetime(6) not null default '2026-09-28 12:34:56.123456')",
        NULL, error);
  if (status != ORM_STATUS_OK)
    goto cleanup;

  status = mysql_live_insert(connection, NULL, UINT64_C(1), "alpha",
                             payload, sizeof(payload), INT64_C(7),
                             &affected, error);
  if (status == ORM_STATUS_OK && affected != UINT64_C(1))
    status = mysql_live_fail(error, "MySQL insert affected-row mismatch");
  if (status != ORM_STATUS_OK)
    goto cleanup;

  status = orm_raw(
      connection,
      orm_view("select ?2 as a, ?1 as b, ?2 as c"),
      &query, error);
  if (status == ORM_STATUS_OK)
    status = orm_query_bind(query, orm_i64(11), error);
  if (status == ORM_STATUS_OK)
    status = orm_query_bind(query, orm_i64(22), error);
  if (status == ORM_STATUS_OK)
    status = orm_query_execute(query, &result, error);
  if (status == ORM_STATUS_OK)
    status = orm_result_get_int64(result, 0u, 0u, &signed_value, error);
  if (status == ORM_STATUS_OK && signed_value != INT64_C(22))
    status = mysql_live_fail(error, "MySQL portable bind reorder mismatch");
  if (status == ORM_STATUS_OK)
    status = orm_result_get_int64(result, 0u, 1u, &signed_value, error);
  if (status == ORM_STATUS_OK && signed_value != INT64_C(11))
    status = mysql_live_fail(error, "MySQL portable bind order mismatch");
  if (status == ORM_STATUS_OK)
    status = orm_result_get_int64(result, 0u, 2u, &signed_value, error);
  if (status == ORM_STATUS_OK && signed_value != INT64_C(22))
    status = mysql_live_fail(error, "MySQL repeated portable bind mismatch");
  orm_result_destroy(result);
  orm_query_destroy(query);
  result = NULL;
  query = NULL;
  if (status != ORM_STATUS_OK)
    goto cleanup;

  status = orm_raw(
      connection,
      orm_view("select id,name,payload,score,exact_value,occurred "
               "from turbodb_mysql_live where id=?1"),
      &query, error);
  if (status == ORM_STATUS_OK)
    status = orm_query_bind(query, orm_u64(UINT64_C(1)), error);
  if (status == ORM_STATUS_OK)
    status = orm_query_execute(query, &result, error);
  if (status == ORM_STATUS_OK)
    status = orm_result_get_uint64(result, 0u, 0u, &unsigned_value, error);
  if (status == ORM_STATUS_OK && unsigned_value != UINT64_C(1))
    status = mysql_live_fail(error, "MySQL uint64 round trip mismatch");
  if (status == ORM_STATUS_OK)
    status = orm_result_get_text(result, 0u, 1u, &text, error);
  if (status == ORM_STATUS_OK &&
      (text.len != 5u || memcmp(text.data, "alpha", 5u) != 0))
    status = mysql_live_fail(error, "MySQL text round trip mismatch");
  if (status == ORM_STATUS_OK)
    status = orm_result_get_blob(result, 0u, 2u, &blob, error);
  if (status == ORM_STATUS_OK &&
      (blob.size != sizeof(payload) ||
       memcmp(blob.data, payload, sizeof(payload)) != 0))
    status = mysql_live_fail(error, "MySQL blob round trip mismatch");
  if (status == ORM_STATUS_OK)
    status = orm_result_get_int64(result, 0u, 3u, &signed_value, error);
  if (status == ORM_STATUS_OK && signed_value != INT64_C(7))
    status = mysql_live_fail(error, "MySQL signed integer mismatch");
  if (status == ORM_STATUS_OK)
    status = orm_result_get_text(result, 0u, 4u, &text, error);
  if (status == ORM_STATUS_OK &&
      (text.len != 21u ||
       memcmp(text.data, "1234567890123456.1250", 21u) != 0))
    status = mysql_live_fail(error, "MySQL decimal lost exact text");
  if (status == ORM_STATUS_OK)
    status = orm_result_get_text(result, 0u, 5u, &text, error);
  if (status == ORM_STATUS_OK &&
      (text.len != 26u ||
       memcmp(text.data, "2026-09-28 12:34:56.123456", 26u) != 0))
    status = mysql_live_fail(error, "MySQL datetime text mismatch");
  orm_result_destroy(result);
  orm_query_destroy(query);
  result = NULL;
  query = NULL;
  if (status != ORM_STATUS_OK)
    goto cleanup;

  status = mysql_live_insert(connection, NULL, UINT64_C(1), "duplicate",
                             payload, sizeof(payload), INT64_C(9),
                             NULL, error);
  if (status != ORM_STATUS_CONSTRAINT) {
    if (status == ORM_STATUS_OK)
      status = mysql_live_fail(error,
                               "MySQL duplicate key was not a constraint");
    goto cleanup;
  }
  status = ORM_STATUS_OK;

  status = orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                 &transaction, error);
  if (status == ORM_STATUS_OK)
    status = orm_transaction_savepoint(
        transaction, orm_view("before_insert"), error);
  if (status == ORM_STATUS_OK)
    status = mysql_live_insert(connection, transaction, UINT64_C(2),
                               "rolled-back", payload, sizeof(payload),
                               INT64_C(12), NULL, error);
  if (status == ORM_STATUS_OK)
    status = orm_transaction_rollback_to_savepoint(
        transaction, orm_view("before_insert"), error);
  if (status == ORM_STATUS_OK)
    status = orm_transaction_release_savepoint(
        transaction, orm_view("before_insert"), error);
  if (status == ORM_STATUS_OK)
    status = orm_transaction_commit(transaction, error);
  orm_transaction_destroy(transaction);
  transaction = NULL;
  if (status == ORM_STATUS_OK)
    status = mysql_live_row_count(connection, UINT64_C(2), 0u, error);
  if (status != ORM_STATUS_OK)
    goto cleanup;

  status = orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                 &transaction, error);
  if (status == ORM_STATUS_OK)
    status = mysql_live_insert(connection, transaction, UINT64_C(3),
                               "committed", payload, sizeof(payload),
                               INT64_C(14), NULL, error);
  if (status == ORM_STATUS_OK)
    status = orm_transaction_commit(transaction, error);
  orm_transaction_destroy(transaction);
  transaction = NULL;
  if (status == ORM_STATUS_OK)
    status = mysql_live_row_count(connection, UINT64_C(3), 1u, error);
  if (status != ORM_STATUS_OK)
    goto cleanup;

  if (orm_runtime_close(runtime, error) != ORM_STATUS_BUSY) {
    status = mysql_live_fail(error,
                             "MySQL connection did not retain Plugin lease");
    goto cleanup;
  }

cleanup:
  orm_result_destroy(result);
  orm_query_destroy(query);
  orm_transaction_destroy(transaction);
  orm_disconnect(connection);
  if (runtime != NULL) {
    const orm_status_t close_status = orm_runtime_close(runtime, error);
    if (status == ORM_STATUS_OK && close_status != ORM_STATUS_OK)
      status = close_status;
    orm_runtime_release(runtime);
  }
  return status;
}

spec("ORM MySQL live Driver") {
  it("qualifies prepared CRUD, values, transactions, and Plugin lease") {
    orm_error_t error;
    orm_status_t status;

    check_not_null(getenv("ORM_MYSQL_PLUGIN"));
    check_not_null(getenv("ORM_MYSQL_HOST"));
    check_not_null(getenv("ORM_MYSQL_USER"));
    check_not_null(getenv("ORM_MYSQL_PASSWORD"));
    check_not_null(getenv("ORM_MYSQL_DATABASE"));
    check_not_null(getenv("ORM_MYSQL_PORT"));

    orm_error_init(&error);
    status = mysql_live_run(&error);
    check_equal(status, ORM_STATUS_OK);
  }
}
