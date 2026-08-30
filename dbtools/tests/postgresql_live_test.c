#include "postgresql/dbtool_postgresql.h"

#include <stdlib.h>
#include <string.h>

#include <libpq-fe.h>
#include "tinytest.h"

static dbtool_status live_apply(const char *conninfo, const char *sql,
                                dbtool_apply_result *result,
                                dbtool_error *error) {
  const dbtool_schema_driver_ops *ops = dbtool_postgresql_schema_driver();
  const dbtool_connection_config config = {NULL, conninfo, 0u};
  void *context = NULL;
  dbtool_status status = ops->open(&context, &config, error);
  if (status == DBTOOL_STATUS_OK)
    status = ops->apply(context, sql, strlen(sql), result, error);
  if (context != NULL)
    ops->close(context);
  return status;
}

static void clear_result(PGresult **result) {
  if (result != NULL && *result != NULL) {
    PQclear(*result);
    *result = NULL;
  }
}

spec("PostgreSQL standalone schema driver live") {
  it("applies two tables, exposes them in pg_catalog, and enforces a foreign key") {
    static const char schema_sql[] =
        "create table dbtool_pg_live_parent("
        "id integer primary key);"
        "create table dbtool_pg_live_child("
        "id integer primary key, parent_id integer not null references "
        "dbtool_pg_live_parent(id));";
    static const char inspect_sql[] =
        "select count(*) from pg_catalog.pg_class "
        "where relkind='r' and relname in "
        "('dbtool_pg_live_parent','dbtool_pg_live_child')";
    static const char constraint_sql[] =
        "insert into dbtool_pg_live_child(id,parent_id) values(1,999)";
    static const char cleanup_sql[] =
        "drop table if exists dbtool_pg_live_child, dbtool_pg_live_parent";
    const char *conninfo = getenv("TURBODB_DBTOOLS_PG_TEST_CONNINFO");
    dbtool_apply_result apply_result = DBTOOL_APPLY_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;
    PGconn *connection = NULL;
    PGresult *native_result = NULL;

    check_not_null(conninfo);
    if (conninfo == NULL)
      return;

    connection = PQconnectdb(conninfo);
    check_not_null(connection);
    if (connection == NULL)
      return;
    check_equal(PQstatus(connection), CONNECTION_OK);
    if (PQstatus(connection) != CONNECTION_OK)
      goto cleanup;

    native_result = PQexec(connection, cleanup_sql);
    check_not_null(native_result);
    if (native_result == NULL)
      goto cleanup;
    check_equal(PQresultStatus(native_result), PGRES_COMMAND_OK);
    clear_result(&native_result);

    check_equal(live_apply(conninfo, schema_sql, &apply_result, &error),
                DBTOOL_STATUS_OK);
    check_equal(apply_result.statements, (uint64_t)2u);

    native_result = PQexec(connection, inspect_sql);
    check_not_null(native_result);
    if (native_result == NULL)
      goto cleanup;
    check_equal(PQresultStatus(native_result), PGRES_TUPLES_OK);
    check_equal(PQntuples(native_result), 1);
    check_equal(PQnfields(native_result), 1);
    if (PQresultStatus(native_result) == PGRES_TUPLES_OK &&
        PQntuples(native_result) == 1 && PQnfields(native_result) == 1)
      check_equal(PQgetvalue(native_result, 0, 0), "2");
    clear_result(&native_result);

    native_result = PQexec(connection, constraint_sql);
    check_not_null(native_result);
    if (native_result == NULL)
      goto cleanup;
    check_equal(PQresultStatus(native_result), PGRES_FATAL_ERROR);
    check_equal(PQresultErrorField(native_result, PG_DIAG_SQLSTATE), "23503");

  cleanup:
    clear_result(&native_result);
    if (connection != NULL && PQstatus(connection) == CONNECTION_OK) {
      native_result = PQexec(connection, cleanup_sql);
      if (native_result != NULL)
        check_equal(PQresultStatus(native_result), PGRES_COMMAND_OK);
      clear_result(&native_result);
    }
    if (connection != NULL)
      PQfinish(connection);
  }
}
