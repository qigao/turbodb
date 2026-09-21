#include <orm.h>

#include "tinytest.h"

#include <stdint.h>
#include <stdlib.h>

static orm_connection_t *sqlite_provider_open(const char *path,
                                               orm_error_t *error) {
  orm_config_t config;
  orm_option_t filename;
  orm_connection_t *connection = NULL;

  orm_config(&config);
  filename.keyword = orm_view("filename");
  filename.value = orm_view(path);
  config.driver = orm_view("sqlite");
  config.options = &filename;
  config.option_count = 1u;

  check_equal(orm_connect(&config, &connection, error), ORM_STATUS_OK);
  check_not_null(connection);
  return connection;
}

static void sqlite_provider_execute(orm_connection_t *connection,
                                    const char *sql,
                                    orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;

  check_equal(orm_raw(connection, orm_view(sql), &query, error), ORM_STATUS_OK);
  check_not_null(query);
  check_equal(orm_query_execute(query, &result, error), ORM_STATUS_OK);
  check_not_null(result);

  orm_result_destroy(result);
  orm_query_destroy(query);
}

static void sqlite_provider_execute_in_transaction(
    orm_connection_t *connection, orm_transaction_t *transaction,
    const char *sql, orm_error_t *error) {
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;

  check_equal(orm_raw(connection, orm_view(sql), &query, error), ORM_STATUS_OK);
  check_not_null(query);
  check_equal(
      orm_query_execute_in_transaction(query, transaction, &result, error),
      ORM_STATUS_OK);
  check_not_null(result);

  orm_result_destroy(result);
  orm_query_destroy(query);
}

static void sqlite_provider_seed(const char *path) {
  orm_error_t error;
  orm_connection_t *connection;

  orm_error_init(&error);
  connection = sqlite_provider_open(path, &error);

  sqlite_provider_execute(
      connection,
      "create table application_state("
      "id integer primary key,value integer not null)",
      &error);
  sqlite_provider_execute(
      connection,
      "create table provider_progress("
      "slot integer primary key,applied integer not null)",
      &error);
  sqlite_provider_execute(connection,
                          "insert into application_state values(1,10)", &error);
  sqlite_provider_execute(connection,
                          "insert into provider_progress values(1,100)", &error);

  orm_disconnect(connection);
}

static void sqlite_provider_read(const char *path, int64_t *application_value,
                                 int64_t *applied) {
  orm_error_t error;
  orm_connection_t *connection;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  uint64_t rows = 0u;
  uint64_t columns = 0u;

  orm_error_init(&error);
  connection = sqlite_provider_open(path, &error);
  check_equal(
      orm_raw(connection,
              orm_view(
                  "select "
                  "(select value from application_state where id=1),"
                  "(select applied from provider_progress where slot=1)"),
              &query, &error),
      ORM_STATUS_OK);
  check_equal(orm_query_execute(query, &result, &error), ORM_STATUS_OK);
  check_not_null(result);
  check_equal(orm_result_row_count(result, &rows, &error), ORM_STATUS_OK);
  check_equal(orm_result_column_count(result, &columns, &error), ORM_STATUS_OK);
  check_equal(rows, UINT64_C(1));
  check_equal(columns, UINT64_C(2));
  check_equal(orm_result_get_int64(result, 0u, 0u, application_value, &error),
              ORM_STATUS_OK);
  check_equal(orm_result_get_int64(result, 0u, 1u, applied, &error),
              ORM_STATUS_OK);

  orm_result_destroy(result);
  orm_query_destroy(query);
  orm_disconnect(connection);
}

static void sqlite_provider_mutate(orm_connection_t *connection,
                                   orm_transaction_t *transaction,
                                   orm_error_t *error) {
  sqlite_provider_execute_in_transaction(
      connection, transaction,
      "update application_state set value=20 where id=1", error);
  sqlite_provider_execute_in_transaction(
      connection, transaction,
      "update provider_progress set applied=101 where slot=1", error);
}

spec("SQLite external provider transaction atomicity") {
  it("commits application state and caller progress together") {
    char *path = tt_make_temp_file("orm-sqlite-provider-commit", ".db");
    orm_error_t error;
    orm_connection_t *connection;
    orm_transaction_t *transaction = NULL;
    int64_t application_value = 0;
    int64_t applied = 0;

    check_not_null(path);
    sqlite_provider_seed(path);

    orm_error_init(&error);
    connection = sqlite_provider_open(path, &error);
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                      &transaction, &error),
                ORM_STATUS_OK);
    check_not_null(transaction);
    sqlite_provider_mutate(connection, transaction, &error);
    check_equal(orm_transaction_commit(transaction, &error), ORM_STATUS_OK);
    orm_transaction_destroy(transaction);
    orm_disconnect(connection);

    sqlite_provider_read(path, &application_value, &applied);
    check_equal(application_value, INT64_C(20));
    check_equal(applied, INT64_C(101));

    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("rolls back application state and caller progress together") {
    char *path = tt_make_temp_file("orm-sqlite-provider-rollback", ".db");
    orm_error_t error;
    orm_connection_t *connection;
    orm_transaction_t *transaction = NULL;
    int64_t application_value = 0;
    int64_t applied = 0;

    check_not_null(path);
    sqlite_provider_seed(path);

    orm_error_init(&error);
    connection = sqlite_provider_open(path, &error);
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                      &transaction, &error),
                ORM_STATUS_OK);
    check_not_null(transaction);
    sqlite_provider_mutate(connection, transaction, &error);
    check_equal(orm_transaction_rollback(transaction, &error), ORM_STATUS_OK);
    orm_transaction_destroy(transaction);
    orm_disconnect(connection);

    sqlite_provider_read(path, &application_value, &applied);
    check_equal(application_value, INT64_C(10));
    check_equal(applied, INT64_C(100));

    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("destroying an active transaction publishes neither mutation") {
    char *path = tt_make_temp_file("orm-sqlite-provider-destroy", ".db");
    orm_error_t error;
    orm_connection_t *connection;
    orm_transaction_t *transaction = NULL;
    int64_t application_value = 0;
    int64_t applied = 0;

    check_not_null(path);
    sqlite_provider_seed(path);

    orm_error_init(&error);
    connection = sqlite_provider_open(path, &error);
    check_equal(orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                      &transaction, &error),
                ORM_STATUS_OK);
    check_not_null(transaction);
    sqlite_provider_mutate(connection, transaction, &error);

    orm_transaction_destroy(transaction);
    orm_disconnect(connection);

    sqlite_provider_read(path, &application_value, &applied);
    check_equal(application_value, INT64_C(10));
    check_equal(applied, INT64_C(100));

    check_equal(tt_remove_file(path), 0);
    free(path);
  }
}
