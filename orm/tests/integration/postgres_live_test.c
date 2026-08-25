#include <orm_postgresql.h>
#include <tinytest.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
  #include <process.h>
  #define orm_test_pid _getpid
#else
  #include <unistd.h>
  #define orm_test_pid getpid
#endif

static orm_connection_t *live_connection;
static orm_connection_t *limited_connection;
static orm_transaction_t *live_transaction;
static char live_schema[96];

static orm_connection_t *connect_live(uint64_t max_result_rows,
                                      orm_error_t *error) {
  const char *conninfo = getenv("TURBODB_ORM_PGSQL_TEST_CONNINFO");
  const char *password = getenv("PGPASSWORD");
  orm_option_t options[2];
  orm_config_t config;
  orm_connection_t *connection = NULL;

  check_not_null(conninfo);
  check(conninfo[0] != '\0', "TURBODB_ORM_PGSQL_TEST_CONNINFO is empty");
  options[0].keyword = orm_view("conninfo");
  options[0].value = orm_view(conninfo);
  orm_config(&config);
  config.driver = orm_view("postgresql");
  config.options = options;
  config.option_count = 1u;
  if (password != NULL && password[0] != '\0') {
    options[1].keyword = orm_view("password");
    options[1].value = orm_view(password);
    config.option_count = 2u;
  }
  if (max_result_rows != 0u) config.max_result_rows = max_result_rows;
  check(orm_connect(&config, &connection, error) == ORM_STATUS_OK,
        error->message);
  check_not_null(connection);
  return connection;
}

static orm_status_t execute_raw(orm_connection_t *connection,
                                const char *sql,
                                orm_result_t **out,
                                orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_status_t status = orm_raw(connection, orm_view(sql), &query, error);
  if (status == ORM_STATUS_OK)
    status = orm_query_execute(query, &result, error);
  orm_query_destroy(query);
  if (out != NULL)
    *out = result;
  else
    orm_result_destroy(result);
  return status;
}

static orm_status_t insert_row_impl(orm_connection_t *connection,
                                    orm_transaction_t *transaction,
                                    const char *schema,
                                    int64_t id,
                                    const char *name,
                                    const uint8_t *payload,
                                    size_t payload_size,
                                    orm_error_t *error) {
  char sql[512];
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_status_t status;

  (void)snprintf(sql, sizeof(sql),
                 "insert into \"%s\".items(id,name,payload) values ($1,$2,$3)",
                 schema);
  status = orm_raw(connection, orm_view(sql), &query, error);
  if (status == ORM_STATUS_OK)
    status = orm_query_bind(query, orm_i64(id), error);
  if (status == ORM_STATUS_OK)
    status = orm_query_bind(query, orm_text(name), error);
  if (status == ORM_STATUS_OK)
    status = orm_query_bind(query, orm_blob(payload, payload_size), error);
  if (status == ORM_STATUS_OK)
    status = transaction != NULL
                 ? orm_query_execute_in_transaction(query, transaction, &result,
                                                    error)
                 : orm_query_execute(query, &result, error);
  orm_result_destroy(result);
  orm_query_destroy(query);
  return status;
}

static orm_status_t insert_row(orm_connection_t *connection,
                               const char *schema,
                               int64_t id,
                               const char *name,
                               const uint8_t *payload,
                               size_t payload_size,
                               orm_error_t *error) {
  return insert_row_impl(connection, NULL, schema, id, name, payload,
                         payload_size, error);
}

static int row_exists(orm_connection_t *connection, const char *schema,
                      int64_t id, orm_error_t *error) {
  char sql[256];
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  uint64_t rows = 0u;

  (void)snprintf(sql, sizeof(sql),
                 "select id from \"%s\".items where id=$1", schema);
  check(orm_raw(connection, orm_view(sql), &query, error) == ORM_STATUS_OK,
        error->message);
  check(orm_query_bind(query, orm_i64(id), error) == ORM_STATUS_OK,
        error->message);
  check(orm_query_execute(query, &result, error) == ORM_STATUS_OK,
        error->message);
  check(orm_result_row_count(result, &rows, error) == ORM_STATUS_OK,
        error->message);
  orm_result_destroy(result);
  orm_query_destroy(query);
  return rows == 1u;
}

static void cleanup_live_test(void) {
  orm_error_t error;
  char sql[160];
  orm_error_init(&error);
  if (live_transaction != NULL) {
    orm_transaction_destroy(live_transaction);
    live_transaction = NULL;
  }
  if (limited_connection != NULL) {
    orm_disconnect(limited_connection);
    limited_connection = NULL;
  }
  if (live_connection != NULL) {
    if (live_schema[0] != '\0') {
      (void)snprintf(sql, sizeof(sql), "drop schema if exists \"%s\" cascade",
                     live_schema);
      (void)execute_raw(live_connection, sql, NULL, &error);
    }
    orm_disconnect(live_connection);
    live_connection = NULL;
  }
  live_schema[0] = '\0';
}

suite("TurboDB ORM PostgreSQL live") {
  after_each() { cleanup_live_test(); }

  it("round trips parameters, bytea, diagnostics and result limits") {
    static const uint8_t payload[] = {0x00u, 0x01u, 0x7fu, 0x80u, 0xffu};
    orm_error_t error;
    orm_connection_t *connection = NULL;
    orm_connection_t *limited = NULL;
    orm_result_t *result = NULL;
    orm_transaction_t *transaction = NULL;
    orm_blob_t read_payload;
    orm_string_view_t read_name;
    char *schema = live_schema;
    char sql[512];

    orm_error_init_s(&error, (uint32_t)sizeof(error));
    check(orm_postgresql_register(&error) == ORM_STATUS_OK, error.message);
    connection = connect_live(0u, &error);
    live_connection = connection;
    (void)snprintf(schema, sizeof(live_schema), "orm_pg_live_%ld_%lld",
                   (long)orm_test_pid(), (long long)time(NULL));
    (void)snprintf(sql, sizeof(sql), "create schema \"%s\"", schema);
    check(execute_raw(connection, sql, NULL, &error) == ORM_STATUS_OK,
          error.message);
    (void)snprintf(sql, sizeof(sql),
                   "create table \"%s\".items("
                   "id bigint primary key,name text not null,payload bytea not null)",
                   schema);
    check(execute_raw(connection, sql, NULL, &error) == ORM_STATUS_OK,
          error.message);

    check(insert_row(connection, schema, 1, "alpha", payload,
                     sizeof(payload), &error) == ORM_STATUS_OK,
          error.message);
    (void)snprintf(sql, sizeof(sql),
                   "select name,payload from \"%s\".items where id=$1", schema);
    {
      orm_query_t *query = NULL;
      check(orm_raw(connection, orm_view(sql), &query, &error) == ORM_STATUS_OK,
            error.message);
      check(orm_query_bind(query, orm_i64(1), &error) == ORM_STATUS_OK,
            error.message);
      check(orm_query_execute(query, &result, &error) == ORM_STATUS_OK,
            error.message);
      orm_query_destroy(query);
    }
    check(orm_result_get_text(result, 0u, 0u, &read_name, &error) ==
              ORM_STATUS_OK,
          error.message);
    check(read_name.len == 5u && memcmp(read_name.data, "alpha", 5u) == 0,
          "PostgreSQL text round trip mismatch");
    check(orm_result_get_blob(result, 0u, 1u, &read_payload, &error) ==
              ORM_STATUS_OK,
          error.message);
    check(read_payload.size == sizeof(payload) &&
              memcmp(read_payload.data, payload, sizeof(payload)) == 0,
          "PostgreSQL bytea round trip mismatch");
    orm_result_destroy(result);
    result = NULL;

    check(orm_transaction_begin(connection, ORM_ISOLATION_SNAPSHOT,
                                &transaction, &error) == ORM_STATUS_OK,
          error.message);
    live_transaction = transaction;
    check(insert_row_impl(connection, transaction, schema, 2, "committed",
                          payload, sizeof(payload), &error) == ORM_STATUS_OK,
          error.message);
    check(orm_transaction_commit(transaction, &error) == ORM_STATUS_OK,
          error.message);
    orm_transaction_destroy(transaction);
    transaction = NULL;
    live_transaction = NULL;
    check_true(row_exists(connection, schema, 2, &error));

    check(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                &transaction, &error) == ORM_STATUS_OK,
          error.message);
    live_transaction = transaction;
    check(insert_row_impl(connection, transaction, schema, 3, "rolled-back",
                          payload, sizeof(payload), &error) == ORM_STATUS_OK,
          error.message);
    check(orm_transaction_rollback(transaction, &error) == ORM_STATUS_OK,
          error.message);
    orm_transaction_destroy(transaction);
    transaction = NULL;
    live_transaction = NULL;
    check_false(row_exists(connection, schema, 3, &error));

    check(orm_transaction_begin(connection, ORM_ISOLATION_READ_COMMITTED,
                                &transaction, &error) == ORM_STATUS_OK,
          error.message);
    live_transaction = transaction;
    check(insert_row_impl(connection, transaction, schema, 4,
                          "destructor-rollback", payload, sizeof(payload),
                          &error) == ORM_STATUS_OK,
          error.message);
    orm_transaction_destroy(transaction);
    transaction = NULL;
    live_transaction = NULL;
    check_false(row_exists(connection, schema, 4, &error));

    check(insert_row(connection, schema, 1, "duplicate", payload,
                     sizeof(payload), &error) == ORM_STATUS_BUSY,
          "PostgreSQL unique violation was not mapped to busy");
    check_equal(orm_error_backend_code(&error), "23505");

    check(execute_raw(connection, "set statement_timeout=50", NULL, &error) ==
              ORM_STATUS_OK,
          error.message);
    check(execute_raw(connection, "select pg_sleep(0.2)", NULL, &error) ==
              ORM_STATUS_SQL_ERROR,
          "PostgreSQL statement timeout did not fail as SQL error");
    check_equal(orm_error_backend_code(&error), "57014");
    check(execute_raw(connection, "set statement_timeout=0", NULL, &error) ==
              ORM_STATUS_OK,
          error.message);

    limited = connect_live(1u, &error);
    limited_connection = limited;
    check(execute_raw(limited, "select generate_series(1,2)", NULL, &error) ==
              ORM_STATUS_LIMIT_EXCEEDED,
          "PostgreSQL result row limit was not enforced");
    orm_disconnect(limited);
    limited_connection = NULL;

    (void)snprintf(sql, sizeof(sql), "drop schema \"%s\" cascade", schema);
    check(execute_raw(connection, sql, NULL, &error) == ORM_STATUS_OK,
          error.message);
    orm_disconnect(connection);
    live_connection = NULL;
    live_schema[0] = '\0';
  }
}
