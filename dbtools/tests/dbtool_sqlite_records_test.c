#include "sqlite/dbtool_sqlite.h"
#include "sqlite/dbtool_sqlite_records.h"

#include "tinytest.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

enum { TEST_COLUMN_COUNT = 11 };

static const dbtool_column_v1 TEST_COLUMNS[] = {
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 0u, DBTOOL_COLUMN_GENERATED,
     DBTOOL_SCALAR_INT64, DBTOOL_STORAGE_INTEGER64, "id", "id"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 1u, 0u, DBTOOL_SCALAR_INT64,
     DBTOOL_STORAGE_INTEGER64, "signed_value", "signed_value"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 2u, 0u, DBTOOL_SCALAR_UINT64,
     DBTOOL_STORAGE_INTEGER64, "small_unsigned", "small_unsigned"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 3u, 0u, DBTOOL_SCALAR_UINT64,
     DBTOOL_STORAGE_UINT64_DECIMAL, "exact_unsigned", "exact_unsigned"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 4u, 0u, DBTOOL_SCALAR_DOUBLE,
     DBTOOL_STORAGE_FLOAT64, "ratio", "ratio"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 5u, 0u, DBTOOL_SCALAR_BOOLEAN,
     DBTOOL_STORAGE_BOOLEAN, "active", "active"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 6u, 0u, DBTOOL_SCALAR_TEXT,
     DBTOOL_STORAGE_TEXT, "name", "name"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 7u, 0u, DBTOOL_SCALAR_BYTES,
     DBTOOL_STORAGE_BYTES, "payload", "payload"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 8u, 0u, DBTOOL_SCALAR_UUID,
     DBTOOL_STORAGE_UUID, "request_id", "request_id"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 9u, DBTOOL_COLUMN_OPTIONAL,
     DBTOOL_SCALAR_TEXT, DBTOOL_STORAGE_TEXT, "note", "note"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 10u, DBTOOL_COLUMN_HAS_DEFAULT,
     DBTOOL_SCALAR_TEXT, DBTOOL_STORAGE_TEXT, "default_name", "default_name"}};

static const dbtool_table_v1 TEST_TABLE = {sizeof(dbtool_table_v1),
                                           DBTOOL_MODEL_ABI_VERSION,
                                           0u,
                                           "Account",
                                           "account records",
                                           TEST_COLUMNS,
                                           TEST_COLUMN_COUNT};

static const dbtool_model_v1 TEST_MODEL = {
    sizeof(dbtool_model_v1), DBTOOL_MODEL_ABI_VERSION, NULL, &TEST_TABLE, 1u, NULL, NULL};

static const dbtool_column_v1 CHILD_COLUMNS[] = {
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 0u, 0u, DBTOOL_SCALAR_INT64,
     DBTOOL_STORAGE_INTEGER64, "parent_id", "parent_id"}};

static const dbtool_table_v1 CHILD_TABLE = {sizeof(dbtool_table_v1),
                                            DBTOOL_MODEL_ABI_VERSION,
                                            0u,
                                            "Child",
                                            "children",
                                            CHILD_COLUMNS,
                                            1u};

static const dbtool_model_v1 CHILD_MODEL = {
    sizeof(dbtool_model_v1), DBTOOL_MODEL_ABI_VERSION, NULL, &CHILD_TABLE, 1u, NULL, NULL};

static const char TEST_DDL[] =
    "CREATE TABLE \"account records\" ("
    "\"id\" INTEGER PRIMARY KEY AUTOINCREMENT,"
    "\"signed_value\" INTEGER NOT NULL,"
    "\"small_unsigned\" INTEGER NOT NULL CHECK(\"small_unsigned\" BETWEEN 0 AND 4294967295),"
    "\"exact_unsigned\" TEXT NOT NULL,"
    "\"ratio\" REAL NOT NULL,"
    "\"active\" INTEGER NOT NULL CHECK(\"active\" IN (0,1)),"
    "\"name\" TEXT NOT NULL UNIQUE,"
    "\"payload\" BLOB NOT NULL,"
    "\"request_id\" TEXT NOT NULL,"
    "\"note\" TEXT,"
    "\"default_name\" TEXT NOT NULL DEFAULT 'server'"
    ");";

static const char CHILD_DDL[] =
    "CREATE TABLE parents(id INTEGER PRIMARY KEY);"
    "CREATE TABLE children(parent_id INTEGER NOT NULL "
    "REFERENCES parents(id) DEFERRABLE INITIALLY DEFERRED);";

static void *open_database(const char *path, dbtool_error *error) {
  const dbtool_schema_driver_ops *driver = dbtool_sqlite_schema_driver();
  const dbtool_connection_config config = {path, NULL, 100u};
  dbtool_apply_result result = DBTOOL_APPLY_RESULT_INIT;
  void *context = NULL;
  if (driver->open(&context, &config, error) != DBTOOL_STATUS_OK) return NULL;
  if (driver->apply(context, TEST_DDL, sizeof(TEST_DDL) - 1u, &result, error) != DBTOOL_STATUS_OK) {
    driver->close(context);
    return NULL;
  }
  return context;
}

static dbtool_record_view make_record(dbtool_cell cells[TEST_COLUMN_COUNT], const char *name) {
  static const unsigned char payload[] = {'a', 0u, 'b'};
  static const unsigned char uuid[] = {0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u,
                                       0x88u, 0x99u, 0xaau, 0xbbu, 0xccu, 0xddu, 0xeeu, 0xffu};
  memset(cells, 0, sizeof(*cells) * TEST_COLUMN_COUNT);
  cells[0].kind = DBTOOL_VALUE_ABSENT;
  cells[1].kind = DBTOOL_VALUE_INT64;
  cells[1].data.int64_value = INT64_MIN;
  cells[2].kind = DBTOOL_VALUE_UINT64;
  cells[2].data.uint64_value = UINT32_MAX;
  cells[3].kind = DBTOOL_VALUE_UINT64;
  cells[3].data.uint64_value = UINT64_MAX;
  cells[4].kind = DBTOOL_VALUE_DOUBLE;
  cells[4].data.double_value = 1.5;
  cells[5].kind = DBTOOL_VALUE_BOOLEAN;
  cells[5].data.boolean_value = 1;
  cells[6].kind = DBTOOL_VALUE_TEXT;
  cells[6].data.bytes.data = (const unsigned char *)name;
  cells[6].data.bytes.size = strlen(name);
  cells[7].kind = DBTOOL_VALUE_BYTES;
  cells[7].data.bytes.data = payload;
  cells[7].data.bytes.size = sizeof(payload);
  cells[8].kind = DBTOOL_VALUE_UUID;
  cells[8].data.bytes.data = uuid;
  cells[8].data.bytes.size = sizeof(uuid);
  cells[9].kind = DBTOOL_VALUE_NULL;
  cells[10].kind = DBTOOL_VALUE_ABSENT;
  return (dbtool_record_view){cells, TEST_COLUMN_COUNT};
}

static int query_int(sqlite3 *database, const char *sql) {
  sqlite3_stmt *statement = NULL;
  int value = -1;
  if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) == SQLITE_OK &&
      sqlite3_step(statement) == SQLITE_ROW)
    value = sqlite3_column_int(statement, 0);
  sqlite3_finalize(statement);
  return value;
}

spec("SQLite generated record driver") {
  (void)ttest_config__;

  it("round trips exact scalars defaults null bytes and UUID") {
    const dbtool_record_sink_ops *sink = dbtool_sqlite_record_sink();
    const dbtool_record_source_ops *source = dbtool_sqlite_record_source();
    const dbtool_schema_driver_ops *schema_driver = dbtool_sqlite_schema_driver();
    char *path = tt_make_temp_file("dbtool-sqlite-records", ".db");
    dbtool_error error = DBTOOL_ERROR_INIT;
    dbtool_cell input_cells[TEST_COLUMN_COUNT];
    dbtool_record_view input = make_record(input_cells, "Alice");
    dbtool_record_view output = {0};
    void *database_context = open_database(path, &error);
    void *sink_context = NULL;
    void *source_context = NULL;

    check_not_null(path);
    check_not_null(database_context);
    check_equal(sink->begin(database_context, &sink_context, &TEST_MODEL, 0u, &error),
                DBTOOL_STATUS_OK);
    check_equal(sink->write(sink_context, &input, &error), DBTOOL_STATUS_OK);
    check_equal(sink->commit(sink_context, &error), DBTOOL_STATUS_OK);
    sink->close(sink_context);
    check_equal(query_int(dbtool_sqlite_native_database(database_context),
                          "SELECT typeof(\"small_unsigned\")='integer' AND "
                          "typeof(\"exact_unsigned\")='text' AND "
                          "\"exact_unsigned\"='18446744073709551615' "
                          "FROM \"account records\""),
                1);

    check_equal(source->open(database_context, &source_context, &TEST_MODEL, 0u, &error),
                DBTOOL_STATUS_OK);
    check_equal(source->next(source_context, &output, &error), DBTOOL_RECORD_ROW);
    check_equal(output.cell_count, TEST_COLUMN_COUNT);
    check_equal(output.cells[0].kind, DBTOOL_VALUE_INT64);
    check(output.cells[1].data.int64_value == INT64_MIN);
    check(output.cells[2].data.uint64_value == UINT32_MAX);
    check(output.cells[3].data.uint64_value == UINT64_MAX);
    check_equal(output.cells[4].data.double_value, 1.5);
    check_equal(output.cells[5].data.boolean_value, 1);
    check_equal(output.cells[7].data.bytes.size, 3u);
    check_equal(output.cells[7].data.bytes.data[1], 0u);
    check_equal(output.cells[8].data.bytes.size, 16u);
    check_equal(output.cells[9].kind, DBTOOL_VALUE_NULL);
    check_equal(output.cells[10].data.bytes.size, 6u);
    check_equal(memcmp(output.cells[10].data.bytes.data, "server", 6u), 0);
    check_equal(source->next(source_context, &output, &error), DBTOOL_RECORD_DONE);
    source->close(source_context);

    schema_driver->close(database_context);
    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("rolls back earlier rows after a later constraint failure") {
    const dbtool_record_sink_ops *sink = dbtool_sqlite_record_sink();
    const dbtool_schema_driver_ops *schema_driver = dbtool_sqlite_schema_driver();
    char *path = tt_make_temp_file("dbtool-sqlite-record-rollback", ".db");
    dbtool_error error = DBTOOL_ERROR_INIT;
    dbtool_cell first_cells[TEST_COLUMN_COUNT];
    dbtool_cell second_cells[TEST_COLUMN_COUNT];
    dbtool_record_view first = make_record(first_cells, "duplicate");
    dbtool_record_view second = make_record(second_cells, "duplicate");
    void *database_context = open_database(path, &error);
    void *sink_context = NULL;
    sqlite3 *native = NULL;

    check_equal(sink->begin(database_context, &sink_context, &TEST_MODEL, 0u, &error),
                DBTOOL_STATUS_OK);
    check_equal(sink->write(sink_context, &first, &error), DBTOOL_STATUS_OK);
    check_equal(sink->write(sink_context, &second, &error), DBTOOL_STATUS_SQL_ERROR);
    check_equal(sink->rollback(sink_context, &error), DBTOOL_STATUS_OK);
    sink->close(sink_context);
    native = dbtool_sqlite_native_database(database_context);
    check_equal(query_int(native, "SELECT count(*) FROM \"account records\""), 0);

    schema_driver->close(database_context);
    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("rejects a changed presence shape and rolls back the batch") {
    const dbtool_record_sink_ops *sink = dbtool_sqlite_record_sink();
    const dbtool_schema_driver_ops *schema_driver = dbtool_sqlite_schema_driver();
    char *path = tt_make_temp_file("dbtool-sqlite-record-shape", ".db");
    dbtool_error error = DBTOOL_ERROR_INIT;
    dbtool_cell first_cells[TEST_COLUMN_COUNT];
    dbtool_cell second_cells[TEST_COLUMN_COUNT];
    dbtool_record_view first = make_record(first_cells, "first-shape");
    dbtool_record_view second = make_record(second_cells, "second-shape");
    void *database_context = open_database(path, &error);
    void *sink_context = NULL;
    sqlite3 *native = NULL;

    second_cells[10].kind = DBTOOL_VALUE_TEXT;
    second_cells[10].data.bytes.data = (const unsigned char *)"client";
    second_cells[10].data.bytes.size = 6u;

    check_not_null(path);
    check_not_null(database_context);
    check_equal(sink->begin(database_context, &sink_context, &TEST_MODEL, 0u, &error),
                DBTOOL_STATUS_OK);
    check_equal(sink->write(sink_context, &first, &error), DBTOOL_STATUS_OK);
    check_equal(sink->write(sink_context, &second, &error), DBTOOL_STATUS_INVALID_ARGUMENT);
    check_contains(error.message, "presence differs");
    check_equal(sink->rollback(sink_context, &error), DBTOOL_STATUS_OK);
    sink->close(sink_context);
    native = dbtool_sqlite_native_database(database_context);
    check_equal(query_int(native, "SELECT count(*) FROM \"account records\""), 0);

    schema_driver->close(database_context);
    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("rejects invalid generated metadata before opening a transaction") {
    const dbtool_record_sink_ops *sink = dbtool_sqlite_record_sink();
    const dbtool_schema_driver_ops *schema_driver = dbtool_sqlite_schema_driver();
    char *path = tt_make_temp_file("dbtool-sqlite-record-metadata", ".db");
    dbtool_error error = DBTOOL_ERROR_INIT;
    dbtool_column_v1 columns[TEST_COLUMN_COUNT];
    dbtool_table_v1 table = TEST_TABLE;
    dbtool_model_v1 model = TEST_MODEL;
    void *database_context = open_database(path, &error);
    void *sink_context = NULL;
    sqlite3 *native = dbtool_sqlite_native_database(database_context);

    memcpy(columns, TEST_COLUMNS, sizeof(columns));
    columns[1].flags = UINT32_C(1) << 31u;
    table.columns = columns;
    model.tables = &table;

    check_not_null(path);
    check_not_null(database_context);
    check_equal(sink->begin(database_context, &sink_context, &model, 0u, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    check_null(sink_context);
    check_contains(error.message, "column metadata");
    check_equal(sqlite3_get_autocommit(native), 1);

    schema_driver->close(database_context);
    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("makes a deferred commit failure terminal until rollback") {
    const dbtool_record_sink_ops *sink = dbtool_sqlite_record_sink();
    const dbtool_schema_driver_ops *schema_driver = dbtool_sqlite_schema_driver();
    char *path = tt_make_temp_file("dbtool-sqlite-record-commit", ".db");
    dbtool_error error = DBTOOL_ERROR_INIT;
    dbtool_apply_result apply_result = DBTOOL_APPLY_RESULT_INIT;
    dbtool_cell cell = {DBTOOL_VALUE_INT64, {.int64_value = 7}};
    dbtool_record_view record = {&cell, 1u};
    void *database_context = open_database(path, &error);
    void *sink_context = NULL;
    sqlite3 *native = dbtool_sqlite_native_database(database_context);

    check_not_null(path);
    check_not_null(database_context);
    check_equal(schema_driver->apply(database_context, CHILD_DDL,
                                     sizeof(CHILD_DDL) - 1u, &apply_result,
                                     &error),
                DBTOOL_STATUS_OK);
    check_equal(sqlite3_exec(native, "PRAGMA foreign_keys=ON", NULL, NULL, NULL),
                SQLITE_OK);
    check_equal(sink->begin(database_context, &sink_context, &CHILD_MODEL, 0u,
                            &error),
                DBTOOL_STATUS_OK);
    check_equal(sink->write(sink_context, &record, &error), DBTOOL_STATUS_OK);
    check_equal(sink->commit(sink_context, &error), DBTOOL_STATUS_SQL_ERROR);
    check_equal(error.stage, "commit-records");
    check_equal(sink->write(sink_context, &record, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    check_equal(sink->rollback(sink_context, &error), DBTOOL_STATUS_OK);
    sink->close(sink_context);
    check_equal(query_int(native, "SELECT count(*) FROM children"), 0);

    schema_driver->close(database_context);
    check_equal(tt_remove_file(path), 0);
    free(path);
  }

  it("rejects malformed stored uint64 text and reuses the borrowed row view") {
    const dbtool_record_sink_ops *sink = dbtool_sqlite_record_sink();
    const dbtool_record_source_ops *source = dbtool_sqlite_record_source();
    const dbtool_schema_driver_ops *schema_driver = dbtool_sqlite_schema_driver();
    char *path = tt_make_temp_file("dbtool-sqlite-record-source", ".db");
    dbtool_error error = DBTOOL_ERROR_INIT;
    dbtool_cell first_cells[TEST_COLUMN_COUNT];
    dbtool_cell second_cells[TEST_COLUMN_COUNT];
    dbtool_record_view first = make_record(first_cells, "first");
    dbtool_record_view second = make_record(second_cells, "second");
    dbtool_record_view row = {0};
    dbtool_record_step step;
    const dbtool_cell *borrowed_cells = NULL;
    int64_t first_id = 0;
    void *database_context = open_database(path, &error);
    void *sink_context = NULL;
    void *source_context = NULL;
    sqlite3 *native = dbtool_sqlite_native_database(database_context);

    check_equal(sink->begin(database_context, &sink_context, &TEST_MODEL, 0u, &error),
                DBTOOL_STATUS_OK);
    check_equal(sink->write(sink_context, &first, &error), DBTOOL_STATUS_OK);
    check_equal(sink->write(sink_context, &second, &error), DBTOOL_STATUS_OK);
    check_equal(sink->commit(sink_context, &error), DBTOOL_STATUS_OK);
    sink->close(sink_context);

    check_equal(source->open(database_context, &source_context, &TEST_MODEL, 0u, &error),
                DBTOOL_STATUS_OK);
    check_equal(source->next(source_context, &row, &error), DBTOOL_RECORD_ROW);
    borrowed_cells = row.cells;
    first_id = row.cells[0].data.int64_value;
    check_equal(source->next(source_context, &row, &error), DBTOOL_RECORD_ROW);
    check(row.cells == borrowed_cells);
    check_not_equal(row.cells[0].data.int64_value, first_id);
    check_equal(source->next(source_context, &row, &error), DBTOOL_RECORD_DONE);
    source->close(source_context);

    check_equal(sqlite3_exec(native,
                             "UPDATE \"account records\" SET \"exact_unsigned\"='01' "
                             "WHERE \"name\"='first'",
                             NULL, NULL, NULL),
                SQLITE_OK);
    check_equal(source->open(database_context, &source_context, &TEST_MODEL, 0u, &error),
                DBTOOL_STATUS_OK);
    do {
      step = source->next(source_context, &row, &error);
    } while (step == DBTOOL_RECORD_ROW);
    check_equal(step, DBTOOL_RECORD_ERROR);
    check_contains(error.message, "canonical uint64");
    source->close(source_context);

    schema_driver->close(database_context);
    check_equal(tt_remove_file(path), 0);
    free(path);
  }
}
