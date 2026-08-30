#include "postgresql/dbtool_postgresql.h"
#include "postgresql/dbtool_postgresql_records.h"

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

enum { LIVE_COLUMN_COUNT = 4 };

static const dbtool_column_v1 LIVE_COLUMNS[] = {
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 0u, 0u,
     DBTOOL_SCALAR_UINT64, DBTOOL_STORAGE_UINT64_DECIMAL, "id", "id"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 1u, 0u,
     DBTOOL_SCALAR_BYTES, DBTOOL_STORAGE_BYTES, "payload", "payload"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 2u, 0u,
     DBTOOL_SCALAR_UUID, DBTOOL_STORAGE_UUID, "request_id", "request_id"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 3u,
     DBTOOL_COLUMN_OPTIONAL, DBTOOL_SCALAR_TEXT, DBTOOL_STORAGE_TEXT, "note",
     "note"}};

static const dbtool_table_v1 LIVE_TABLE = {
    sizeof(dbtool_table_v1), DBTOOL_MODEL_ABI_VERSION, 0u, "LiveRecord",
    "dbtool_pg_live_records", LIVE_COLUMNS, LIVE_COLUMN_COUNT};

static const dbtool_model_v1 LIVE_MODEL = {
    sizeof(dbtool_model_v1), DBTOOL_MODEL_ABI_VERSION, NULL, &LIVE_TABLE, 1u,
    NULL, NULL};

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

  it("round trips exact generated record values through libpq") {
    static const char create_sql[] =
        "drop table if exists dbtool_pg_live_records;"
        "create table dbtool_pg_live_records("
        "id numeric(20,0) primary key,payload bytea not null,"
        "request_id uuid not null,note text);";
    static const char cleanup_sql[] =
        "drop table if exists dbtool_pg_live_records";
    static const char truncate_sql[] = "truncate table dbtool_pg_live_records";
    static const unsigned char payload[] = {'a', 0u, 'b'};
    static const unsigned char uuid[] = {
        0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u,
        0x88u, 0x99u, 0xaau, 0xbbu, 0xccu, 0xddu, 0xeeu, 0xffu};
    const char *conninfo = getenv("TURBODB_DBTOOLS_PG_TEST_CONNINFO");
    const dbtool_schema_driver_ops *driver = dbtool_postgresql_schema_driver();
    const dbtool_record_sink_ops *sink = dbtool_postgresql_record_sink();
    const dbtool_record_source_ops *source = dbtool_postgresql_record_source();
    const dbtool_connection_config config = {NULL, conninfo, 0u};
    dbtool_apply_result apply_result = DBTOOL_APPLY_RESULT_INIT;
    dbtool_error error = DBTOOL_ERROR_INIT;
    dbtool_cell cells[LIVE_COLUMN_COUNT] = {
        {DBTOOL_VALUE_UINT64, {.uint64_value = UINT64_MAX}},
        {DBTOOL_VALUE_BYTES, {.bytes = {payload, sizeof(payload)}}},
        {DBTOOL_VALUE_UUID, {.bytes = {uuid, sizeof(uuid)}}},
        {.kind = DBTOOL_VALUE_NULL}};
    dbtool_record_view input = {cells, LIVE_COLUMN_COUNT};
    dbtool_record_view output = {0};
    void *database_context = NULL;
    void *sink_context = NULL;
    void *source_context = NULL;

    check_not_null(conninfo);
    if (conninfo == NULL) return;
    check_equal(driver->open(&database_context, &config, &error),
                DBTOOL_STATUS_OK);
    if (database_context == NULL) return;
    check_equal(driver->apply(database_context, create_sql,
                              sizeof(create_sql) - 1u, &apply_result, &error),
                DBTOOL_STATUS_OK);
    check_equal(sink->begin(database_context, &sink_context, &LIVE_MODEL, 0u,
                            &error), DBTOOL_STATUS_OK);
    check_equal(sink->write(sink_context, &input, &error), DBTOOL_STATUS_OK);
    check_equal(sink->commit(sink_context, &error), DBTOOL_STATUS_OK);
    sink->close(sink_context);
    sink_context = NULL;

    check_equal(source->open(database_context, &source_context, &LIVE_MODEL,
                             0u, &error), DBTOOL_STATUS_OK);
    check_equal(source->next(source_context, &output, &error),
                DBTOOL_RECORD_ROW);
    check(output.cells[0].data.uint64_value == UINT64_MAX);
    check_equal(output.cells[1].data.bytes.size, sizeof(payload));
    check_equal(memcmp(output.cells[1].data.bytes.data, payload,
                       sizeof(payload)), 0);
    check_equal(output.cells[2].data.bytes.size, sizeof(uuid));
    check_equal(memcmp(output.cells[2].data.bytes.data, uuid, sizeof(uuid)), 0);
    check_equal(output.cells[3].kind, DBTOOL_VALUE_NULL);
    check_equal(source->next(source_context, &output, &error),
                DBTOOL_RECORD_DONE);
    source->close(source_context);
    source_context = NULL;

    check_equal(driver->apply(database_context, truncate_sql,
                              sizeof(truncate_sql) - 1u, &apply_result, &error),
                DBTOOL_STATUS_OK);
    check_equal(sink->begin(database_context, &sink_context, &LIVE_MODEL, 0u,
                            &error), DBTOOL_STATUS_OK);
    check_equal(sink->write(sink_context, &input, &error), DBTOOL_STATUS_OK);
    check_equal(sink->write(sink_context, &input, &error),
                DBTOOL_STATUS_SQL_ERROR);
    check_equal(sink->rollback(sink_context, &error), DBTOOL_STATUS_OK);
    sink->close(sink_context);
    sink_context = NULL;

    check_equal(source->open(database_context, &source_context, &LIVE_MODEL,
                             0u, &error), DBTOOL_STATUS_OK);
    check_equal(source->next(source_context, &output, &error),
                DBTOOL_RECORD_DONE);
    source->close(source_context);
    source_context = NULL;

    cells[0].data.uint64_value = 0u;
    cells[1].data.bytes = (dbtool_bytes_view){NULL, 0u};
    check_equal(sink->begin(database_context, &sink_context, &LIVE_MODEL, 0u,
                            &error), DBTOOL_STATUS_OK);
    check_equal(sink->write(sink_context, &input, &error), DBTOOL_STATUS_OK);
    check_equal(sink->commit(sink_context, &error), DBTOOL_STATUS_OK);
    sink->close(sink_context);
    sink_context = NULL;

    check_equal(source->open(database_context, &source_context, &LIVE_MODEL,
                             0u, &error), DBTOOL_STATUS_OK);
    check_equal(source->next(source_context, &output, &error),
                DBTOOL_RECORD_ROW);
    check_equal(output.cells[0].data.uint64_value, 0u);
    check_equal(output.cells[1].kind, DBTOOL_VALUE_BYTES);
    check_equal(output.cells[1].data.bytes.size, 0u);
    check_equal(source->next(source_context, &output, &error),
                DBTOOL_RECORD_DONE);
    source->close(source_context);
    source_context = NULL;

    check_equal(driver->apply(database_context, cleanup_sql,
                              sizeof(cleanup_sql) - 1u, &apply_result, &error),
                DBTOOL_STATUS_OK);
    driver->close(database_context);
  }
}
