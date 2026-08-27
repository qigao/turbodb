#include "orm_postgres_cursor.h"

#include "tinymock.h"

#include <stdint.h>
#include <string.h>

typedef struct orm_postgres_test_result {
  orm_postgres_result_status status;
  size_t columns;
  const char *const *names;
  const uint32_t *types;
  const char *const *values;
  const size_t *lengths;
  const uint8_t *nulls;
  const char *affected_rows;
  const char *error;
} orm_postgres_test_result;

TINYMOCk_MOCK(int, orm_postgres_test_send_mock, void *, void *)
TINYMOCk_MOCK(int, orm_postgres_test_single_row, void *)
TINYMOCk_MOCK(void *, orm_postgres_test_next_result, void *)
TINYMOCk_MOCK_VOID(orm_postgres_test_release_result, void *)

static int orm_postgres_test_send(
    void *context, const orm_postgres_query_request *request) {
  return orm_postgres_test_send_mock(context, (void *)request);
}

static const char *orm_postgres_test_connection_error(void *context) {
  (void)context;
  return "forced PostgreSQL connection error";
}

static orm_postgres_result_status orm_postgres_test_result_status(
    const void *result) {
  return ((const orm_postgres_test_result *)result)->status;
}

static int64_t orm_postgres_test_result_rows(const void *result) {
  return ((const orm_postgres_test_result *)result)->status ==
                 ORM_POSTGRES_RESULT_SINGLE_ROW
             ? 1
             : 0;
}

static int64_t orm_postgres_test_result_columns(const void *result) {
  return (int64_t)((const orm_postgres_test_result *)result)->columns;
}

static const char *orm_postgres_test_column_name(const void *result,
                                                  size_t column) {
  return ((const orm_postgres_test_result *)result)->names[column];
}

static uint32_t orm_postgres_test_column_type(const void *result,
                                               size_t column) {
  return ((const orm_postgres_test_result *)result)->types[column];
}

static int orm_postgres_test_is_null(const void *result, size_t row,
                                     size_t column) {
  (void)row;
  return ((const orm_postgres_test_result *)result)->nulls[column] != 0u;
}

static const char *orm_postgres_test_value(const void *result, size_t row,
                                            size_t column) {
  (void)row;
  return ((const orm_postgres_test_result *)result)->values[column];
}

static int64_t orm_postgres_test_length(const void *result, size_t row,
                                        size_t column) {
  (void)row;
  return (int64_t)((const orm_postgres_test_result *)result)->lengths[column];
}

static const char *orm_postgres_test_command_tuples(const void *result) {
  return ((const orm_postgres_test_result *)result)->affected_rows;
}

static const char *orm_postgres_test_result_error(const void *result) {
  return ((const orm_postgres_test_result *)result)->error;
}

static const orm_postgres_command_ops orm_postgres_test_command_ops = {
    sizeof(orm_postgres_command_ops), ORM_POSTGRES_COMMAND_OPS_ABI_VERSION,
    orm_postgres_test_send, orm_postgres_test_single_row,
    orm_postgres_test_next_result, orm_postgres_test_release_result,
    orm_postgres_test_connection_error};

static const orm_postgres_result_ops orm_postgres_test_result_ops = {
    sizeof(orm_postgres_result_ops), ORM_POSTGRES_RESULT_OPS_ABI_VERSION,
    orm_postgres_test_result_status, orm_postgres_test_result_rows,
    orm_postgres_test_result_columns, orm_postgres_test_column_name,
    orm_postgres_test_column_type, orm_postgres_test_is_null,
    orm_postgres_test_value, orm_postgres_test_length,
    orm_postgres_test_command_tuples, orm_postgres_test_result_error};

static void orm_postgres_test_reset_mocks(void) {
  mock_orm_postgres_test_send_mock_reset();
  mock_orm_postgres_test_single_row_reset();
  mock_orm_postgres_test_next_result_reset();
  mock_orm_postgres_test_release_result_reset();
}

static void orm_postgres_test_verify_mocks(void) {
  mock_orm_postgres_test_send_mock_verify();
  mock_orm_postgres_test_single_row_verify();
  mock_orm_postgres_test_next_result_verify();
  mock_orm_postgres_test_release_result_verify();
}

static void orm_postgres_test_expect_start(
    void *connection, orm_postgres_query_request *request) {
  mock_orm_postgres_test_send_mock_expect(
      TINYMOCk_ARG(connection), TINYMOCk_ARG((void *)request),
      TINYMOCk_RETURN(1));
  mock_orm_postgres_test_single_row_expect(
      TINYMOCk_ARG(connection), TINYMOCk_RETURN(1));
}

static void orm_postgres_test_check_token(cserde_reader *reader,
                                           cserde_token_kind kind,
                                           const void *data, size_t size) {
  cserde_token token = {0};
  check_equal(cserde_reader_next(reader, &token), CSERDE_OK);
  check_equal(token.kind, kind);
  if (kind == CSERDE_STRING || kind == CSERDE_BYTES) {
    check_equal(token.value.slice.size, size);
    if (size != 0u)
      check_equal(memcmp(token.value.slice.data, data, size), 0);
  }
}

spec("ORM PostgreSQL single-row cursor") {
  it("owns optional execution metadata and drains on destroy") {
    int connection_token = 0;
    orm_postgres_driver driver = {
        &orm_postgres_test_command_ops, &orm_postgres_test_result_ops,
        &connection_token};
    orm_postgres_query_request request = {
        "select 1", 0, NULL, NULL, NULL, NULL, 0};
    orm_postgres_cursor_config config =
        ORM_POSTGRES_CURSOR_CONFIG_INIT(1u, 64u, NULL, NULL, NULL);
    orm_row_cursor cursor = {0};
    orm_error_t error;

    orm_error_init(&error);
    orm_postgres_test_reset_mocks();
    orm_postgres_test_expect_start(&connection_token, &request);
    mock_orm_postgres_test_next_result_expect(
        TINYMOCk_ARG((void *)&connection_token),
        TINYMOCk_RETURN((void *)NULL));

    check_equal(orm_postgres_cursor_start(&cursor, &driver, &request,
                                          &config, &error),
                ORM_STATUS_OK);
    cursor.ops->destroy(cursor.context);
    orm_postgres_test_verify_mocks();
  }

  it("emits one reader for one row demand and preserves field order") {
    static const char *const names[] = {"id", "name"};
    static const uint32_t types[] = {23u, 25u};
    static const char *const values[] = {"7", "Alice"};
    static const size_t lengths[] = {1u, 5u};
    static const uint8_t nulls[] = {0u, 0u};
    orm_postgres_test_result row = {
        ORM_POSTGRES_RESULT_SINGLE_ROW, 2u, names, types, values, lengths,
        nulls, NULL, NULL};
    orm_postgres_test_result terminal = {
        ORM_POSTGRES_RESULT_TUPLES_DONE, 2u, names, types, NULL, NULL, NULL,
        NULL, NULL};
    int connection_token = 0;
    orm_postgres_driver driver = {
        &orm_postgres_test_command_ops, &orm_postgres_test_result_ops,
        &connection_token};
    orm_postgres_query_request request = {
        "select id, name from person", 0, NULL, NULL, NULL, NULL, 0};
    uint64_t affected_rows = 0u;
    size_t columns = 0u;
    orm_error_t runtime_error;
    orm_postgres_cursor_config config = ORM_POSTGRES_CURSOR_CONFIG_INIT(
        2u, 1024u, &columns, &affected_rows, &runtime_error);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};
    cserde_token token = {0};

    orm_error_init(&error);
    orm_error_init(&runtime_error);
    orm_postgres_test_reset_mocks();
    orm_postgres_test_expect_start(&connection_token, &request);
    mock_orm_postgres_test_next_result_expect(
        TINYMOCk_ARG((void *)&connection_token), TINYMOCk_RETURN((void *)&row));
    mock_orm_postgres_test_next_result_expect(
        TINYMOCk_ARG((void *)&connection_token),
        TINYMOCk_RETURN((void *)&terminal));
    mock_orm_postgres_test_next_result_expect(
        TINYMOCk_ARG((void *)&connection_token), TINYMOCk_RETURN((void *)NULL));
    mock_orm_postgres_test_release_result_expect(TINYMOCk_ARG((void *)&row));
    mock_orm_postgres_test_release_result_expect(TINYMOCk_ARG((void *)&terminal));

    check_equal(orm_postgres_cursor_start(&cursor, &driver, &request,
                                          &config, &error), ORM_STATUS_OK);
    check_equal(cursor.ops->next(cursor.context, &reader).kind,
                ORM_ROW_CURSOR_ROW);
    orm_postgres_test_check_token(&reader, CSERDE_MAP_BEGIN, NULL, 0u);
    orm_postgres_test_check_token(&reader, CSERDE_STRING, "id", 2u);
    orm_postgres_test_check_token(&reader, CSERDE_STRING, "7", 1u);
    orm_postgres_test_check_token(&reader, CSERDE_STRING, "name", 4u);
    orm_postgres_test_check_token(&reader, CSERDE_STRING, "Alice", 5u);
    orm_postgres_test_check_token(&reader, CSERDE_MAP_END, NULL, 0u);
    check_equal(cserde_reader_next(&reader, &token), CSERDE_DONE);
    check_equal(cursor.ops->next(cursor.context, &reader).kind,
                ORM_ROW_CURSOR_DONE);
    check_equal(columns, (size_t)2u);
    check_equal(affected_rows, (uint64_t)0u);
    cursor.ops->destroy(cursor.context);
    orm_postgres_test_verify_mocks();
  }

  it("decodes bytea as bytes and preserves null") {
    static const char *const names[] = {"payload", "missing"};
    static const uint32_t types[] = {17u, 25u};
    static const char *const values[] = {"\\x0001ff", NULL};
    static const size_t lengths[] = {8u, 0u};
    static const uint8_t nulls[] = {0u, 1u};
    static const unsigned char expected[] = {0x00u, 0x01u, 0xffu};
    orm_postgres_test_result row = {
        ORM_POSTGRES_RESULT_SINGLE_ROW, 2u, names, types, values, lengths,
        nulls, NULL, NULL};
    orm_postgres_test_result terminal = {
        ORM_POSTGRES_RESULT_TUPLES_DONE, 2u, names, types, NULL, NULL, NULL,
        NULL, NULL};
    int connection_token = 0;
    orm_postgres_driver driver = {
        &orm_postgres_test_command_ops, &orm_postgres_test_result_ops,
        &connection_token};
    orm_postgres_query_request request = {
        "select payload, missing from files", 0, NULL, NULL, NULL, NULL, 0};
    orm_error_t runtime_error;
    orm_postgres_cursor_config config =
        ORM_POSTGRES_CURSOR_CONFIG_INIT(2u, 1024u, NULL, NULL, &runtime_error);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};
    cserde_token token = {0};

    orm_error_init(&error);
    orm_error_init(&runtime_error);
    orm_postgres_test_reset_mocks();
    orm_postgres_test_expect_start(&connection_token, &request);
    mock_orm_postgres_test_next_result_expect(
        TINYMOCk_ARG((void *)&connection_token), TINYMOCk_RETURN((void *)&row));
    mock_orm_postgres_test_next_result_expect(
        TINYMOCk_ARG((void *)&connection_token),
        TINYMOCk_RETURN((void *)&terminal));
    mock_orm_postgres_test_next_result_expect(
        TINYMOCk_ARG((void *)&connection_token), TINYMOCk_RETURN((void *)NULL));
    mock_orm_postgres_test_release_result_expect(TINYMOCk_ARG((void *)&row));
    mock_orm_postgres_test_release_result_expect(TINYMOCk_ARG((void *)&terminal));

    check_equal(orm_postgres_cursor_start(&cursor, &driver, &request,
                                          &config, &error), ORM_STATUS_OK);
    check_equal(cursor.ops->next(cursor.context, &reader).kind,
                ORM_ROW_CURSOR_ROW);
    orm_postgres_test_check_token(&reader, CSERDE_MAP_BEGIN, NULL, 0u);
    orm_postgres_test_check_token(&reader, CSERDE_STRING, "payload", 7u);
    orm_postgres_test_check_token(&reader, CSERDE_BYTES, expected,
                                  sizeof(expected));
    orm_postgres_test_check_token(&reader, CSERDE_STRING, "missing", 7u);
    check_equal(cserde_reader_next(&reader, &token), CSERDE_OK);
    check_equal(token.kind, CSERDE_NULL);
    orm_postgres_test_check_token(&reader, CSERDE_MAP_END, NULL, 0u);
    check_equal(cserde_reader_next(&reader, &token), CSERDE_DONE);
    check_equal(cursor.ops->next(cursor.context, &reader).kind,
                ORM_ROW_CURSOR_DONE);
    cursor.ops->destroy(cursor.context);
    orm_postgres_test_verify_mocks();
  }

  it("rejects cumulative payload beyond max_result_bytes") {
    static const char *const names[] = {"value"};
    static const uint32_t types[] = {25u};
    static const char *const first_values[] = {"abc"};
    static const char *const second_values[] = {"def"};
    static const size_t lengths[] = {3u};
    static const uint8_t nulls[] = {0u};
    orm_postgres_test_result first = {
        ORM_POSTGRES_RESULT_SINGLE_ROW, 1u, names, types, first_values,
        lengths, nulls, NULL, NULL};
    orm_postgres_test_result second = {
        ORM_POSTGRES_RESULT_SINGLE_ROW, 1u, names, types, second_values,
        lengths, nulls, NULL, NULL};
    orm_postgres_test_result terminal = {
        ORM_POSTGRES_RESULT_TUPLES_DONE, 1u, names, types, NULL, NULL, NULL,
        NULL, NULL};
    int connection_token = 0;
    orm_postgres_driver driver = {
        &orm_postgres_test_command_ops, &orm_postgres_test_result_ops,
        &connection_token};
    orm_postgres_query_request request = {
        "select value from payloads", 0, NULL, NULL, NULL, NULL, 0};
    orm_error_t runtime_error;
    orm_postgres_cursor_config config =
        ORM_POSTGRES_CURSOR_CONFIG_INIT(1u, 4u, NULL, NULL, &runtime_error);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};

    orm_error_init(&error);
    orm_error_init(&runtime_error);
    orm_postgres_test_reset_mocks();
    orm_postgres_test_expect_start(&connection_token, &request);
    mock_orm_postgres_test_next_result_expect(
        TINYMOCk_ARG((void *)&connection_token), TINYMOCk_RETURN((void *)&first));
    mock_orm_postgres_test_next_result_expect(
        TINYMOCk_ARG((void *)&connection_token), TINYMOCk_RETURN((void *)&second));
    mock_orm_postgres_test_next_result_expect(
        TINYMOCk_ARG((void *)&connection_token),
        TINYMOCk_RETURN((void *)&terminal));
    mock_orm_postgres_test_next_result_expect(
        TINYMOCk_ARG((void *)&connection_token), TINYMOCk_RETURN((void *)NULL));
    mock_orm_postgres_test_release_result_expect(TINYMOCk_ARG((void *)&first));
    mock_orm_postgres_test_release_result_expect(TINYMOCk_ARG((void *)&second));
    mock_orm_postgres_test_release_result_expect(TINYMOCk_ARG((void *)&terminal));

    check_equal(orm_postgres_cursor_start(&cursor, &driver, &request,
                                          &config, &error), ORM_STATUS_OK);
    check_equal(cursor.ops->next(cursor.context, &reader).kind,
                ORM_ROW_CURSOR_ROW);
    {
      const orm_row_cursor_step step = cursor.ops->next(cursor.context, &reader);
      check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
      check_equal(step.status, ORM_STATUS_LIMIT_EXCEEDED);
    }
    cursor.ops->destroy(cursor.context);
    orm_postgres_test_verify_mocks();
  }

  it("preserves a fatal error after an emitted row") {
    static const char *const names[] = {"value"};
    static const uint32_t types[] = {25u};
    static const char *const values[] = {"visible-before-failure"};
    static const size_t lengths[] = {22u};
    static const uint8_t nulls[] = {0u};
    orm_postgres_test_result row = {
        ORM_POSTGRES_RESULT_SINGLE_ROW, 1u, names, types, values, lengths,
        nulls, NULL, NULL};
    orm_postgres_test_result fatal = {
        ORM_POSTGRES_RESULT_ERROR, 0u, NULL, NULL, NULL, NULL, NULL, NULL,
        "forced error after one row"};
    int connection_token = 0;
    orm_postgres_driver driver = {
        &orm_postgres_test_command_ops, &orm_postgres_test_result_ops,
        &connection_token};
    orm_postgres_query_request request = {
        "select value from unstable_query", 0, NULL, NULL, NULL, NULL, 0};
    orm_error_t runtime_error;
    orm_postgres_cursor_config config =
        ORM_POSTGRES_CURSOR_CONFIG_INIT(1u, 128u, NULL, NULL, &runtime_error);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};

    orm_error_init(&error);
    orm_error_init(&runtime_error);
    orm_postgres_test_reset_mocks();
    orm_postgres_test_expect_start(&connection_token, &request);
    mock_orm_postgres_test_next_result_expect(
        TINYMOCk_ARG((void *)&connection_token), TINYMOCk_RETURN((void *)&row));
    mock_orm_postgres_test_next_result_expect(
        TINYMOCk_ARG((void *)&connection_token), TINYMOCk_RETURN((void *)&fatal));
    mock_orm_postgres_test_next_result_expect(
        TINYMOCk_ARG((void *)&connection_token), TINYMOCk_RETURN((void *)NULL));
    mock_orm_postgres_test_release_result_expect(TINYMOCk_ARG((void *)&row));
    mock_orm_postgres_test_release_result_expect(TINYMOCk_ARG((void *)&fatal));

    check_equal(orm_postgres_cursor_start(&cursor, &driver, &request,
                                          &config, &error), ORM_STATUS_OK);
    check_equal(cursor.ops->next(cursor.context, &reader).kind,
                ORM_ROW_CURSOR_ROW);
    {
      const orm_row_cursor_step step = cursor.ops->next(cursor.context, &reader);
      check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
      check_equal(step.status, ORM_STATUS_SQL_ERROR);
      check_not_null(strstr(step.message, "forced error after one row"));
    }
    cursor.ops->destroy(cursor.context);
    orm_postgres_test_verify_mocks();
  }

  it("reports affected rows only after command completion") {
    orm_postgres_test_result command = {
        ORM_POSTGRES_RESULT_COMMAND_DONE, 0u, NULL, NULL, NULL, NULL, NULL,
        "42", NULL};
    int connection_token = 0;
    orm_postgres_driver driver = {
        &orm_postgres_test_command_ops, &orm_postgres_test_result_ops,
        &connection_token};
    orm_postgres_query_request request = {
        "update person set active = false", 0, NULL, NULL, NULL, NULL, 0};
    uint64_t affected_rows = 0u;
    orm_error_t runtime_error;
    orm_postgres_cursor_config config = ORM_POSTGRES_CURSOR_CONFIG_INIT(
        1u, 1u, NULL, &affected_rows, &runtime_error);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};

    orm_error_init(&error);
    orm_error_init(&runtime_error);
    orm_postgres_test_reset_mocks();
    orm_postgres_test_expect_start(&connection_token, &request);
    mock_orm_postgres_test_next_result_expect(
        TINYMOCk_ARG((void *)&connection_token),
        TINYMOCk_RETURN((void *)&command));
    mock_orm_postgres_test_next_result_expect(
        TINYMOCk_ARG((void *)&connection_token), TINYMOCk_RETURN((void *)NULL));
    mock_orm_postgres_test_release_result_expect(TINYMOCk_ARG((void *)&command));

    check_equal(orm_postgres_cursor_start(&cursor, &driver, &request,
                                          &config, &error), ORM_STATUS_OK);
    check_equal(affected_rows, (uint64_t)0u);
    check_equal(cursor.ops->next(cursor.context, &reader).kind,
                ORM_ROW_CURSOR_DONE);
    check_equal(affected_rows, (uint64_t)42u);
    cursor.ops->destroy(cursor.context);
    orm_postgres_test_verify_mocks();
  }

  it("rejects signed affected-row text") {
    orm_postgres_test_result command = {
        ORM_POSTGRES_RESULT_COMMAND_DONE, 0u, NULL, NULL, NULL, NULL, NULL,
        "-1", NULL};
    int connection_token = 0;
    orm_postgres_driver driver = {
        &orm_postgres_test_command_ops, &orm_postgres_test_result_ops,
        &connection_token};
    orm_postgres_query_request request = {
        "update person set active = false", 0, NULL, NULL, NULL, NULL, 0};
    orm_error_t runtime_error;
    orm_postgres_cursor_config config =
        ORM_POSTGRES_CURSOR_CONFIG_INIT(1u, 1u, NULL, NULL, &runtime_error);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};

    orm_error_init(&error);
    orm_error_init(&runtime_error);
    orm_postgres_test_reset_mocks();
    orm_postgres_test_expect_start(&connection_token, &request);
    mock_orm_postgres_test_next_result_expect(
        TINYMOCk_ARG((void *)&connection_token),
        TINYMOCk_RETURN((void *)&command));
    mock_orm_postgres_test_next_result_expect(
        TINYMOCk_ARG((void *)&connection_token), TINYMOCk_RETURN((void *)NULL));
    mock_orm_postgres_test_release_result_expect(TINYMOCk_ARG((void *)&command));

    check_equal(orm_postgres_cursor_start(&cursor, &driver, &request,
                                          &config, &error), ORM_STATUS_OK);
    {
      const orm_row_cursor_step step = cursor.ops->next(cursor.context, &reader);
      check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
      check_equal(step.status, ORM_STATUS_INTERNAL_ERROR);
    }
    cursor.ops->destroy(cursor.context);
    orm_postgres_test_verify_mocks();
  }
}
