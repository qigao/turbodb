#include "postgresql/dbtool_postgresql.h"
#include "postgresql/dbtool_postgresql_records.h"

#include "fake_libpq.h"
#include "tinytest.h"

#include <stdint.h>
#include <string.h>

enum { TEST_COLUMN_COUNT = 11 };

static const dbtool_column_v1 TEST_COLUMNS[] = {
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 0u, 0u,
     DBTOOL_SCALAR_INT64, DBTOOL_STORAGE_INTEGER16, "small_signed", "small_signed"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 1u, 0u,
     DBTOOL_SCALAR_UINT64, DBTOOL_STORAGE_INTEGER32, "medium_unsigned", "medium_unsigned"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 2u, 0u,
     DBTOOL_SCALAR_INT64, DBTOOL_STORAGE_INTEGER64, "large_signed", "large_signed"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 3u, 0u,
     DBTOOL_SCALAR_UINT64, DBTOOL_STORAGE_UINT64_DECIMAL, "exact_unsigned", "exact_unsigned"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 4u, 0u,
     DBTOOL_SCALAR_DOUBLE, DBTOOL_STORAGE_FLOAT32, "single_ratio", "single_ratio"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 5u, 0u,
     DBTOOL_SCALAR_DOUBLE, DBTOOL_STORAGE_FLOAT64, "double_ratio", "double_ratio"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 6u, 0u,
     DBTOOL_SCALAR_BOOLEAN, DBTOOL_STORAGE_BOOLEAN, "active", "active"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 7u, 0u,
     DBTOOL_SCALAR_TEXT, DBTOOL_STORAGE_TEXT, "name", "name"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 8u, 0u,
     DBTOOL_SCALAR_BYTES, DBTOOL_STORAGE_BYTES, "payload", "payload"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 9u, 0u,
     DBTOOL_SCALAR_UUID, DBTOOL_STORAGE_UUID, "request_id", "request_id"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 10u,
     DBTOOL_COLUMN_OPTIONAL, DBTOOL_SCALAR_TEXT, DBTOOL_STORAGE_TEXT, "note", "note"}};

static const dbtool_table_v1 TEST_TABLE = {
    sizeof(dbtool_table_v1), DBTOOL_MODEL_ABI_VERSION, 0u, "Order",
    "order items", TEST_COLUMNS, TEST_COLUMN_COUNT};
static const dbtool_model_v1 TEST_MODEL = {
    sizeof(dbtool_model_v1), DBTOOL_MODEL_ABI_VERSION, NULL, &TEST_TABLE, 1u,
    NULL, NULL};

static void *open_connection(dbtool_error *error) {
  const dbtool_schema_driver_ops *driver = dbtool_postgresql_schema_driver();
  const dbtool_connection_config config = {NULL, "host=fake", 0u};
  void *context = NULL;
  if (driver->open(&context, &config, error) != DBTOOL_STATUS_OK) return NULL;
  return context;
}

static dbtool_record_view make_record(dbtool_cell cells[TEST_COLUMN_COUNT],
                                      const char *name) {
  static const unsigned char payload[] = {'a', 0u, 'b'};
  static const unsigned char uuid[] = {
      0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u,
      0x88u, 0x99u, 0xaau, 0xbbu, 0xccu, 0xddu, 0xeeu, 0xffu};
  memset(cells, 0, sizeof(*cells) * TEST_COLUMN_COUNT);
  cells[0] = (dbtool_cell){DBTOOL_VALUE_INT64, {.int64_value = INT16_MIN}};
  cells[1] = (dbtool_cell){DBTOOL_VALUE_UINT64, {.uint64_value = UINT16_MAX}};
  cells[2] = (dbtool_cell){DBTOOL_VALUE_INT64, {.int64_value = INT64_MIN}};
  cells[3] = (dbtool_cell){DBTOOL_VALUE_UINT64, {.uint64_value = UINT64_MAX}};
  cells[4] = (dbtool_cell){DBTOOL_VALUE_DOUBLE, {.double_value = 1.25}};
  cells[5] = (dbtool_cell){DBTOOL_VALUE_DOUBLE, {.double_value = -2.5}};
  cells[6] = (dbtool_cell){DBTOOL_VALUE_BOOLEAN, {.boolean_value = 1}};
  cells[7].kind = DBTOOL_VALUE_TEXT;
  cells[7].data.bytes = (dbtool_bytes_view){(const unsigned char *)name, strlen(name)};
  cells[8].kind = DBTOOL_VALUE_BYTES;
  cells[8].data.bytes = (dbtool_bytes_view){payload, sizeof(payload)};
  cells[9].kind = DBTOOL_VALUE_UUID;
  cells[9].data.bytes = (dbtool_bytes_view){uuid, sizeof(uuid)};
  cells[10].kind = DBTOOL_VALUE_NULL;
  return (dbtool_record_view){cells, TEST_COLUMN_COUNT};
}

spec("PostgreSQL generated record driver") {
  before_each() { fake_libpq_reset(); }

  it("prepares once and binds exact PostgreSQL parameter widths") {
    static const Oid expected_types[] = {21u, 23u, 20u, 1700u, 700u, 701u,
                                         16u, 25u, 17u, 2950u, 25u};
    const dbtool_record_sink_ops *sink = dbtool_postgresql_record_sink();
    const dbtool_schema_driver_ops *driver = dbtool_postgresql_schema_driver();
    dbtool_error error = DBTOOL_ERROR_INIT;
    dbtool_cell first_cells[TEST_COLUMN_COUNT];
    dbtool_cell second_cells[TEST_COLUMN_COUNT];
    dbtool_record_view first = make_record(first_cells, "Alice");
    dbtool_record_view second = make_record(second_cells, "Bob");
    void *database_context = open_connection(&error);
    void *sink_context = NULL;
    const fake_libpq_metrics *metrics;
    size_t index;

    check_not_null(database_context);
    check_equal(sink->begin(database_context, &sink_context, &TEST_MODEL, 0u,
                            &error), DBTOOL_STATUS_OK);
    check_equal(sink->write(sink_context, &first, &error), DBTOOL_STATUS_OK);
    metrics = fake_libpq_get_metrics();
    check_equal(metrics->parameter_nulls[8], 0);
    check_equal(metrics->parameter_formats[8], 1);
    check_equal(metrics->parameter_lengths[8], 3);
    check_equal(metrics->parameter_values[8][1], 0u);
    second_cells[8].data.bytes = (dbtool_bytes_view){NULL, 0u};
    check_equal(sink->write(sink_context, &second, &error), DBTOOL_STATUS_OK);
    check_contains(fake_libpq_get_metrics()->sql,
                   "INSERT INTO \"order items\"");
    check_equal(sink->commit(sink_context, &error), DBTOOL_STATUS_OK);
    sink->close(sink_context);
    metrics = fake_libpq_get_metrics();
    check_equal(metrics->prepare_calls, 1);
    check_equal(metrics->exec_prepared_calls, 2);
    for (index = 0u; index < TEST_COLUMN_COUNT; ++index)
      check_equal(metrics->parameter_types[index], expected_types[index]);
    check_equal((const char *)metrics->parameter_values[0], "-32768");
    check_equal((const char *)metrics->parameter_values[1], "65535");
    check_equal((const char *)metrics->parameter_values[3], "18446744073709551615");
    check_equal(metrics->parameter_nulls[8], 0);
    check_equal(metrics->parameter_formats[8], 1);
    check_equal(metrics->parameter_lengths[8], 0);
    driver->close(database_context);
  }

  it("rolls back after a later prepared execution failure") {
    const dbtool_record_sink_ops *sink = dbtool_postgresql_record_sink();
    const dbtool_schema_driver_ops *driver = dbtool_postgresql_schema_driver();
    dbtool_error error = DBTOOL_ERROR_INIT;
    dbtool_cell first_cells[TEST_COLUMN_COUNT];
    dbtool_cell second_cells[TEST_COLUMN_COUNT];
    dbtool_record_view first = make_record(first_cells, "first");
    dbtool_record_view second = make_record(second_cells, "second");
    void *database_context = open_connection(&error);
    void *sink_context = NULL;

    check_equal(sink->begin(database_context, &sink_context, &TEST_MODEL, 0u,
                            &error), DBTOOL_STATUS_OK);
    check_equal(sink->write(sink_context, &first, &error), DBTOOL_STATUS_OK);
    fake_libpq_set_sync_result(PGRES_FATAL_ERROR, "forced constraint");
    check_equal(sink->write(sink_context, &second, &error), DBTOOL_STATUS_SQL_ERROR);
    check_contains(error.message, "forced constraint");
    fake_libpq_set_sync_result(PGRES_COMMAND_OK, "");
    check_equal(sink->rollback(sink_context, &error), DBTOOL_STATUS_OK);
    sink->close(sink_context);
    driver->close(database_context);
  }

  it("streams a text row and releases its PGresult on the next call") {
    static const ExecStatusType statuses[] = {PGRES_SINGLE_TUPLE, PGRES_TUPLES_OK};
    static const char *const values[] = {
        "-32768", "65535", "-9223372036854775808", "18446744073709551615",
        "1.25", "-2.5", "t", "Alice", "\\x610062",
        "00112233-4455-6677-8899-aabbccddeeff", ""};
    static const int lengths[] = {6, 5, 20, 20, 4, 4, 1, 5, 8, 36, 0};
    static const int nulls[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const dbtool_record_source_ops *source = dbtool_postgresql_record_source();
    const dbtool_schema_driver_ops *driver = dbtool_postgresql_schema_driver();
    dbtool_error error = DBTOOL_ERROR_INIT;
    dbtool_record_view row = {0};
    void *database_context = open_connection(&error);
    void *source_context = NULL;

    fake_libpq_set_results(statuses, NULL, 2u);
    fake_libpq_set_result_row(0u, values, lengths, nulls, TEST_COLUMN_COUNT);
    check_equal(source->open(database_context, &source_context, &TEST_MODEL, 0u,
                             &error), DBTOOL_STATUS_OK);
    check_equal(source->next(source_context, &row, &error), DBTOOL_RECORD_ROW);
    check(row.cells[0].data.int64_value == INT16_MIN);
    check(row.cells[3].data.uint64_value == UINT64_MAX);
    check_equal(row.cells[8].data.bytes.size, 3u);
    check_equal(row.cells[8].data.bytes.data[1], 0u);
    check_equal(row.cells[10].kind, DBTOOL_VALUE_NULL);
    check_equal(fake_libpq_get_metrics()->clear_calls, 0);
    check_equal(source->next(source_context, &row, &error), DBTOOL_RECORD_DONE);
    check_equal(fake_libpq_get_metrics()->clear_calls, 2);
    check_true(fake_libpq_all_results_cleared());
    source->close(source_context);
    driver->close(database_context);
  }

  it("drains every result after a malformed numeric row") {
    static const ExecStatusType statuses[] = {PGRES_SINGLE_TUPLE, PGRES_FATAL_ERROR};
    static const char *const messages[] = {"", "later server error"};
    static const char *const values[] = {
        "-32768", "65535", "-9223372036854775808", "01", "1.25", "-2.5",
        "t", "Alice", "\\x610062",
        "00112233-4455-6677-8899-aabbccddeeff", ""};
    static const int lengths[] = {6, 5, 20, 2, 4, 4, 1, 5, 8, 36, 0};
    static const int nulls[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    const dbtool_record_source_ops *source = dbtool_postgresql_record_source();
    const dbtool_schema_driver_ops *driver = dbtool_postgresql_schema_driver();
    dbtool_error error = DBTOOL_ERROR_INIT;
    dbtool_record_view row = {0};
    void *database_context = open_connection(&error);
    void *source_context = NULL;

    fake_libpq_set_results(statuses, messages, 2u);
    fake_libpq_set_result_row(0u, values, lengths, nulls, TEST_COLUMN_COUNT);
    check_equal(source->open(database_context, &source_context, &TEST_MODEL, 0u,
                             &error), DBTOOL_STATUS_OK);
    check_equal(source->next(source_context, &row, &error), DBTOOL_RECORD_ERROR);
    check_contains(error.message, "uint64");
    check_equal(fake_libpq_get_metrics()->get_result_calls, 3);
    check_true(fake_libpq_all_results_cleared());
    source->close(source_context);
    driver->close(database_context);
  }

  it("drains the active query when single-row mode cannot be enabled") {
    static const ExecStatusType statuses[] = {PGRES_TUPLES_OK};
    const dbtool_record_source_ops *source = dbtool_postgresql_record_source();
    const dbtool_schema_driver_ops *driver = dbtool_postgresql_schema_driver();
    dbtool_error error = DBTOOL_ERROR_INIT;
    void *database_context = open_connection(&error);
    void *source_context = NULL;

    fake_libpq_set_results(statuses, NULL, 1u);
    fake_libpq_set_single_row_result(0);
    check_equal(source->open(database_context, &source_context, &TEST_MODEL,
                             0u, &error),
                DBTOOL_STATUS_CONNECTION_ERROR);
    check_null(source_context);
    check_equal(fake_libpq_get_metrics()->get_result_calls, 2);
    check_true(fake_libpq_all_results_cleared());
    driver->close(database_context);
  }
}
