#include <turbodb/dbtool_format.h>

#include "tinytest.h"

#include <stdint.h>
#include <string.h>

enum {
  TEST_COLUMN_COUNT = 10,
  TEST_OUTPUT_CAPACITY = 4096,
  TEST_CELL_STORAGE_CAPACITY = 64
};

static const dbtool_column_v1 TEST_COLUMNS[] = {
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 0u, 0u,
     DBTOOL_SCALAR_INT64, "signed_value", "signed_value"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 1u, 0u,
     DBTOOL_SCALAR_UINT64, "unsigned_value", "unsigned_value"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 2u, 0u,
     DBTOOL_SCALAR_DOUBLE, "ratio", "ratio"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 3u, 0u,
     DBTOOL_SCALAR_BOOLEAN, "active", "active"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 4u, 0u,
     DBTOOL_SCALAR_TEXT, "name", "name"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 5u, 0u,
     DBTOOL_SCALAR_BYTES, "payload", "payload"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 6u, 0u,
     DBTOOL_SCALAR_UUID, "request_id", "request_id"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 7u,
     DBTOOL_COLUMN_OPTIONAL, DBTOOL_SCALAR_TEXT, "note", "note"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 8u,
     DBTOOL_COLUMN_GENERATED, DBTOOL_SCALAR_INT64, "generated_id",
     "generated_id"},
    {sizeof(dbtool_column_v1), DBTOOL_MODEL_ABI_VERSION, 9u,
     DBTOOL_COLUMN_HAS_DEFAULT, DBTOOL_SCALAR_TEXT, "default_name",
     "default_name"}};

static const dbtool_table_v1 TEST_TABLE = {
    sizeof(dbtool_table_v1), DBTOOL_MODEL_ABI_VERSION, 0u, "Account",
    "accounts", TEST_COLUMNS, TEST_COLUMN_COUNT};

static const char TEST_SCHEMA[] =
    "message Account { int64 signed_value; uint64 unsigned_value; "
    "double ratio; bool active; string name; bytes payload; "
    "uuid request_id; optional string note; optional int64 generated_id; "
    "optional string default_name; }\n";

static const dbtool_format_model_v1 TEST_MODEL = {
    sizeof(dbtool_format_model_v1), DBTOOL_FORMAT_MODEL_ABI_VERSION,
    DBTOOL_DIALECT_SQLITE, TEST_SCHEMA, sizeof(TEST_SCHEMA) - 1u,
    &TEST_TABLE, 1u};

typedef struct captured_record_s {
  dbtool_cell cells[TEST_COLUMN_COUNT];
  unsigned char storage[TEST_COLUMN_COUNT][TEST_CELL_STORAGE_CAPACITY];
  size_t count;
} captured_record_t;

typedef struct output_buffer_s {
  unsigned char bytes[TEST_OUTPUT_CAPACITY];
  size_t size;
} output_buffer_t;

typedef struct failing_output_s {
  size_t calls;
} failing_output_t;

static dbtool_transfer_limits test_limits(void) {
  dbtool_transfer_limits limits = DBTOOL_TRANSFER_LIMITS_INIT;
  limits.chunk_bytes = 7u;
  limits.max_input_bytes = TEST_OUTPUT_CAPACITY;
  limits.max_rows = 4u;
  limits.max_columns = TEST_COLUMN_COUNT;
  limits.max_cell_bytes = TEST_CELL_STORAGE_CAPACITY;
  limits.max_record_bytes = 1024u;
  limits.max_output_bytes = TEST_OUTPUT_CAPACITY;
  return limits;
}

static dbtool_status capture_record(void *context,
                                    const dbtool_record_view *record,
                                    dbtool_error *error) {
  captured_record_t *captured = (captured_record_t *)context;
  size_t index;
  (void)error;
  if (captured == NULL || record == NULL || record->cell_count != TEST_COLUMN_COUNT)
    return DBTOOL_STATUS_INTERNAL_ERROR;
  ++captured->count;
  for (index = 0u; index < record->cell_count; ++index) {
    captured->cells[index] = record->cells[index];
    if (record->cells[index].kind == DBTOOL_VALUE_TEXT ||
        record->cells[index].kind == DBTOOL_VALUE_BYTES ||
        record->cells[index].kind == DBTOOL_VALUE_UUID) {
      size_t size = record->cells[index].data.bytes.size;
      if (size > sizeof(captured->storage[index]))
        return DBTOOL_STATUS_LIMIT_EXCEEDED;
      memcpy(captured->storage[index], record->cells[index].data.bytes.data,
             size);
      captured->cells[index].data.bytes.data = captured->storage[index];
    }
  }
  return DBTOOL_STATUS_OK;
}

static dbtool_status collect_output(void *context, const unsigned char *data,
                                    size_t size, dbtool_error *error) {
  output_buffer_t *output = (output_buffer_t *)context;
  (void)error;
  if (output == NULL || data == NULL ||
      size > sizeof(output->bytes) - output->size)
    return DBTOOL_STATUS_LIMIT_EXCEEDED;
  memcpy(output->bytes + output->size, data, size);
  output->size += size;
  return DBTOOL_STATUS_OK;
}

static dbtool_status reject_output(void *context, const unsigned char *data,
                                   size_t size, dbtool_error *error) {
  failing_output_t *output = (failing_output_t *)context;
  (void)data;
  (void)size;
  (void)error;
  ++output->calls;
  return DBTOOL_STATUS_FILE_ERROR;
}

static dbtool_status decode_json(const unsigned char *json, size_t json_size,
                                 captured_record_t *captured,
                                 dbtool_error *error) {
  dbtool_transfer_limits limits = test_limits();
  dbtool_decoder decoder = {0};
  dbtool_status status = dbtool_format_open_decoder_v1(
      &TEST_MODEL, 0u, DBTOOL_FORMAT_JSON, &limits, capture_record, captured,
      &decoder, error);
  size_t offset = 0u;
  if (status != DBTOOL_STATUS_OK) return status;
  while (offset < json_size) {
    size_t size = json_size - offset;
    if (size > limits.chunk_bytes) size = limits.chunk_bytes;
    status = decoder.ops->feed(decoder.context, json + offset, size, 0, error);
    if (status != DBTOOL_STATUS_OK) break;
    offset += size;
  }
  if (status == DBTOOL_STATUS_OK)
    status = decoder.ops->feed(decoder.context, NULL, 0u, 1, error);
  decoder.ops->close(decoder.context);
  return status;
}

spec("dbtool format") {
  it("round trips exact JSON scalars null bytes and UUID in fixed chunks") {
    static const unsigned char input[] =
        "[{\"signed_value\":-9223372036854775808,"
        "\"unsigned_value\":18446744073709551615,\"ratio\":1.5,"
        "\"active\":true,\"name\":\"Alice\",\"payload\":\"a\\u0000b\","
        "\"request_id\":\"00112233-4455-6677-8899-aabbccddeeff\","
        "\"note\":null}]";
    static const unsigned char expected_uuid[] = {
        0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u,
        0x88u, 0x99u, 0xaau, 0xbbu, 0xccu, 0xddu, 0xeeu, 0xffu};
    captured_record_t first = {0};
    captured_record_t second = {0};
    output_buffer_t output = {{0}, 0u};
    dbtool_transfer_limits limits = test_limits();
    dbtool_encoder encoder = {0};
    dbtool_record_view record;
    dbtool_error error = DBTOOL_ERROR_INIT;

    check_equal(decode_json(input, sizeof(input) - 1u, &first, &error),
                DBTOOL_STATUS_OK);
    check_equal(first.count, 1u);
    check_equal(first.cells[0].kind, DBTOOL_VALUE_INT64);
    check(first.cells[0].data.int64_value == INT64_MIN);
    check_equal(first.cells[1].kind, DBTOOL_VALUE_UINT64);
    check(first.cells[1].data.uint64_value == UINT64_MAX);
    check_equal(first.cells[2].data.double_value, 1.5);
    check_equal(first.cells[3].data.boolean_value, 1);
    check_equal(first.cells[4].data.bytes.size, 5u);
    check_equal(first.cells[5].data.bytes.size, 3u);
    check_equal(first.cells[5].data.bytes.data[1], 0u);
    check_equal(first.cells[6].data.bytes.size, sizeof(expected_uuid));
    check_equal(memcmp(first.cells[6].data.bytes.data, expected_uuid,
                       sizeof(expected_uuid)),
                0);
    check_equal(first.cells[7].kind, DBTOOL_VALUE_NULL);
    check_equal(first.cells[8].kind, DBTOOL_VALUE_ABSENT);
    check_equal(first.cells[9].kind, DBTOOL_VALUE_ABSENT);

    check_equal(dbtool_format_open_encoder_v1(
                    &TEST_MODEL, 0u, DBTOOL_FORMAT_JSON, &limits,
                    collect_output, &output, &encoder, &error),
                DBTOOL_STATUS_OK);
    record.cells = first.cells;
    record.cell_count = TEST_COLUMN_COUNT;
    check_equal(encoder.ops->write(encoder.context, &record, &error),
                DBTOOL_STATUS_OK);
    check_equal(encoder.ops->finish(encoder.context, &error),
                DBTOOL_STATUS_OK);
    encoder.ops->close(encoder.context);
    check(output.size > 2u);
    check_equal(output.bytes[0], '[');
    check_equal(output.bytes[output.size - 1u], ']');
    check_equal(decode_json(output.bytes, output.size, &second, &error),
                DBTOOL_STATUS_OK);
    check_equal(second.count, 1u);
    check(second.cells[0].data.int64_value == INT64_MIN);
    check(second.cells[1].data.uint64_value == UINT64_MAX);
    check_equal(second.cells[5].data.bytes.size, 3u);
    check_equal(second.cells[5].data.bytes.data[1], 0u);
    check_equal(second.cells[7].kind, DBTOOL_VALUE_NULL);
    check_equal(second.cells[8].kind, DBTOOL_VALUE_ABSENT);
    check_equal(second.cells[9].kind, DBTOOL_VALUE_ABSENT);
  }

  it("rejects missing or null required JSON fields before emitting a row") {
    static const unsigned char missing[] = "[{\"signed_value\":1}]";
    static const unsigned char null_required[] =
        "[{\"signed_value\":null,\"unsigned_value\":1,\"ratio\":1,"
        "\"active\":true,\"name\":\"x\",\"payload\":\"x\","
        "\"request_id\":\"00112233-4455-6677-8899-aabbccddeeff\"}]";
    captured_record_t captured = {0};
    dbtool_error error = DBTOOL_ERROR_INIT;

    check_not_equal(decode_json(missing, sizeof(missing) - 1u, &captured, &error),
                    DBTOOL_STATUS_OK);
    check_equal(captured.count, 0u);
    captured = (captured_record_t){0};
    check_not_equal(decode_json(null_required, sizeof(null_required) - 1u,
                                &captured, &error),
                    DBTOOL_STATUS_OK);
    check_equal(captured.count, 0u);
  }

  it("rejects inconsistent generated column metadata at open") {
    dbtool_column_v1 columns[TEST_COLUMN_COUNT];
    dbtool_table_v1 table = TEST_TABLE;
    dbtool_format_model_v1 model = TEST_MODEL;
    dbtool_transfer_limits limits = test_limits();
    captured_record_t captured = {0};
    dbtool_decoder decoder = {0};
    dbtool_error error = DBTOOL_ERROR_INIT;

    memcpy(columns, TEST_COLUMNS, sizeof(columns));
    columns[0].index = 1u;
    table.columns = columns;
    model.tables = &table;
    check_equal(dbtool_format_open_decoder_v1(
                    &model, 0u, DBTOOL_FORMAT_JSON, &limits, capture_record,
                    &captured, &decoder, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    check_null(decoder.ops);
    check_null(decoder.context);
  }

  it("makes JSON decoder and encoder failures terminal") {
    static const unsigned char invalid_json[] = "x";
    dbtool_transfer_limits limits = test_limits();
    captured_record_t captured = {0};
    failing_output_t output = {0};
    dbtool_decoder decoder = {0};
    dbtool_encoder encoder = {0};
    dbtool_record_view record = {captured.cells, TEST_COLUMN_COUNT};
    dbtool_error error = DBTOOL_ERROR_INIT;

    check_equal(dbtool_format_open_decoder_v1(
                    &TEST_MODEL, 0u, DBTOOL_FORMAT_JSON, &limits,
                    capture_record, &captured, &decoder, &error),
                DBTOOL_STATUS_OK);
    check_not_equal(decoder.ops->feed(decoder.context, invalid_json,
                                      sizeof(invalid_json) - 1u, 0, &error),
                    DBTOOL_STATUS_OK);
    check_equal(decoder.ops->feed(decoder.context, NULL, 0u, 1, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    decoder.ops->close(decoder.context);

    captured.cells[0].kind = DBTOOL_VALUE_INT64;
    captured.cells[1].kind = DBTOOL_VALUE_UINT64;
    captured.cells[2].kind = DBTOOL_VALUE_DOUBLE;
    captured.cells[3].kind = DBTOOL_VALUE_BOOLEAN;
    captured.cells[4].kind = DBTOOL_VALUE_TEXT;
    captured.cells[5].kind = DBTOOL_VALUE_BYTES;
    captured.cells[6].kind = DBTOOL_VALUE_UUID;
    captured.cells[7].kind = DBTOOL_VALUE_NULL;
    captured.cells[8].kind = DBTOOL_VALUE_ABSENT;
    captured.cells[9].kind = DBTOOL_VALUE_ABSENT;
    captured.cells[4].data.bytes.data = (const unsigned char *)"x";
    captured.cells[4].data.bytes.size = 1u;
    captured.cells[5].data.bytes.data = (const unsigned char *)"x";
    captured.cells[5].data.bytes.size = 1u;
    captured.cells[6].data.bytes.data =
        (const unsigned char *)"0123456789abcdef";
    captured.cells[6].data.bytes.size = 16u;

    check_equal(dbtool_format_open_encoder_v1(
                    &TEST_MODEL, 0u, DBTOOL_FORMAT_JSON, &limits,
                    reject_output, &output, &encoder, &error),
                DBTOOL_STATUS_OK);
    check_equal(encoder.ops->write(encoder.context, &record, &error),
                DBTOOL_STATUS_FILE_ERROR);
    check_equal(output.calls, 1u);
    check_equal(encoder.ops->write(encoder.context, &record, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    check_equal(output.calls, 1u);
    check_equal(encoder.ops->finish(encoder.context, &error),
                DBTOOL_STATUS_INVALID_ARGUMENT);
    encoder.ops->close(encoder.context);
  }
}
