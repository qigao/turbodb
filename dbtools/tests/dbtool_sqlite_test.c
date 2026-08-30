#include "sqlite/dbtool_sqlite.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>
#include "tinytest.h"

static dbtool_status apply_sql(const char *database, const char *sql,
                               size_t sql_size, dbtool_apply_result *result,
                               dbtool_error *error) {
  const dbtool_schema_driver_ops *ops = dbtool_sqlite_schema_driver();
  const dbtool_connection_config config = {database, NULL, 50u};
  void *context = NULL;
  dbtool_status status = ops->open(&context, &config, error);
  if (status == DBTOOL_STATUS_OK)
    status = ops->apply(context, sql, sql_size, result, error);
  if (context != NULL)
    ops->close(context);
  return status;
}

static int table_exists(const char *database, const char *name) {
  sqlite3 *connection = NULL;
  sqlite3_stmt *statement = NULL;
  int found = 0;
  if (sqlite3_open_v2(database, &connection, SQLITE_OPEN_READONLY, NULL) !=
      SQLITE_OK)
    goto cleanup;
  if (sqlite3_prepare_v2(
          connection,
          "select count(*) from sqlite_master where type='table' and name=?1",
          -1, &statement, NULL) != SQLITE_OK)
    goto cleanup;
  if (sqlite3_bind_text(statement, 1, name, -1, SQLITE_STATIC) != SQLITE_OK)
    goto cleanup;
  if (sqlite3_step(statement) == SQLITE_ROW)
    found = sqlite3_column_int(statement, 0) == 1;

cleanup:
  if (statement != NULL)
    (void)sqlite3_finalize(statement);
  if (connection != NULL)
    (void)sqlite3_close_v2(connection);
  return found;
}

spec("SQLite standalone schema driver") {
  it("applies every statement in one bootstrap script") {
    static const char sql[] =
        "create table alpha(id integer primary key);"
        "create table beta(id integer primary key);";
    char *database = tt_make_temp_file("dbtool-sqlite-success", ".db");
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    check_not_null(database);
    check_equal(apply_sql(database, sql, sizeof(sql) - 1u, &result, &error),
                DBTOOL_STATUS_OK);
    check_true(table_exists(database, "alpha"));
    check_true(table_exists(database, "beta"));
    check_equal(result.statements, (uint64_t)2u);

    check_equal(tt_remove_file(database), 0);
    free(database);
  }

  it("rolls back the first statement when a later statement fails") {
    static const char sql[] =
        "create table first_table(id integer);"
        "create table broken_table(";
    char *database = tt_make_temp_file("dbtool-sqlite-rollback", ".db");
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    check_not_null(database);
    check_equal(apply_sql(database, sql, sizeof(sql) - 1u, &result, &error),
                DBTOOL_STATUS_SQL_ERROR);
    check_false(table_exists(database, "first_table"));
    check_equal(error.stage, "apply-schema");

    check_equal(tt_remove_file(database), 0);
    free(database);
  }

  it("rejects transaction and savepoint control inside the input") {
    static const char *const scripts[] = {
        "begin; create table tx_table(id integer); commit;",
        "savepoint user_sp; create table sp_table(id integer);"};
    static const char *const tables[] = {"tx_table", "sp_table"};
    size_t index;
    for (index = 0u; index < 2u; ++index) {
      char *database = tt_make_temp_file("dbtool-sqlite-control", ".db");
      dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
      dbtool_error error = DBTOOL_ERROR_INIT;

      check_not_null(database);
      check_equal(apply_sql(database, scripts[index], strlen(scripts[index]),
                            &result, &error),
                  DBTOOL_STATUS_UNSUPPORTED);
      check_false(table_exists(database, tables[index]));
      check_contains(error.message, "transaction control");

      check_equal(tt_remove_file(database), 0);
      free(database);
    }
  }

  it("rejects embedded NUL instead of executing a prefix") {
    static const char sql[] =
        "create table prefix_table(id integer);\0"
        "create table suffix_table(id integer);";
    char *database = tt_make_temp_file("dbtool-sqlite-nul", ".db");
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    check_not_null(database);
    check_equal(apply_sql(database, sql, sizeof(sql) - 1u, &result, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    check_false(table_exists(database, "prefix_table"));
    check_contains(error.message, "NUL");

    check_equal(tt_remove_file(database), 0);
    free(database);
  }

  it("rejects a busy timeout outside SQLite int range before opening") {
    const dbtool_schema_driver_ops *ops = dbtool_sqlite_schema_driver();
    const dbtool_connection_config config = {":memory:", NULL, UINT32_MAX};
    dbtool_error error = DBTOOL_ERROR_INIT;
    void *context = NULL;

    check_equal(ops->open(&context, &config, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    check_null(context);
    check_equal(error.stage, "open-driver");
  }

  it("reports a duplicate table and preserves the prior committed schema") {
    static const char sql[] = "create table existing_table(id integer);";
    char *database = tt_make_temp_file("dbtool-sqlite-duplicate", ".db");
    dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;

    check_not_null(database);
    check_equal(apply_sql(database, sql, sizeof(sql) - 1u, &result, &error),
                DBTOOL_STATUS_OK);
    check_equal(apply_sql(database, sql, sizeof(sql) - 1u, &result, &error),
                DBTOOL_STATUS_SQL_ERROR);
    check_true(table_exists(database, "existing_table"));

    check_equal(tt_remove_file(database), 0);
    free(database);
  }
}
