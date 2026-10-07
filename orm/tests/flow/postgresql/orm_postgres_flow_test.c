#include "orm_postgres_cursor.h"
#include "orm_text_token.h"
#include "orm_async_wait.h"

#define TINYMOCK_GENERATE_FUNCTION_OVERRIDES 1
#include "tinymock.h"

#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
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
  const char *sqlstate;
} orm_postgres_test_result;

FunctionDeclResult(value, int, CMETA_RESULT_VALUE, orm_postgres_test_send_mock,
    (void *, context, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER),
    (void *, request, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER));
FunctionDeclResult(value, int, CMETA_RESULT_VALUE, orm_postgres_test_single_row,
    (void *, context, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER));
FunctionDecl(value, void, orm_postgres_test_release_result,
    (void *, result, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,
     &cmeta_type_void_ptr, CMETA_ABI_OBJECT_POINTER));
TINYMOCk_FUNCTION_DECLARE(orm_postgres_test_send_mock);
TINYMOCk_FUNCTION_DECLARE(orm_postgres_test_single_row);
TINYMOCk_FUNCTION_DECLARE(orm_postgres_test_release_result);

enum { ORM_POSTGRES_TEST_MAX_SCRIPTED_RESULTS = 8 };
typedef struct orm_postgres_test_scripted_result {
  void *expected_connection;
  void *result;
  bool check_connection;
} orm_postgres_test_scripted_result;

static orm_postgres_test_scripted_result orm_postgres_test_results[
    ORM_POSTGRES_TEST_MAX_SCRIPTED_RESULTS];
static void *orm_postgres_test_actual_connections[
    ORM_POSTGRES_TEST_MAX_SCRIPTED_RESULTS];
static void *orm_postgres_test_expected_releases[
    ORM_POSTGRES_TEST_MAX_SCRIPTED_RESULTS];
static size_t orm_postgres_test_result_count;
static size_t orm_postgres_test_result_calls;
static size_t orm_postgres_test_release_count;
static void *orm_postgres_test_expected_send_context;
static void *orm_postgres_test_expected_send_request;
static void *orm_postgres_test_expected_single_context;
static size_t orm_postgres_test_expected_send_count;
static size_t orm_postgres_test_expected_single_count;

static void *orm_postgres_test_next_result(void *connection) {
  const size_t call = orm_postgres_test_result_calls++;
  if (call >= ORM_POSTGRES_TEST_MAX_SCRIPTED_RESULTS ||
      call >= orm_postgres_test_result_count)
    return NULL;
  orm_postgres_test_actual_connections[call] = connection;
  return orm_postgres_test_results[call].result;
}

typedef struct postgres_async_fixture {
  orm_async_wait wait;
  int ready;
  int aborted;
  int released;
} postgres_async_fixture;
static orm_row_cursor_step postgres_async_poll(void *context) {
  postgres_async_fixture *fixture = context;
  if (!fixture->ready) return orm_async_wait_step(&fixture->wait);
  orm_row_cursor_step step = ORM_ROW_CURSOR_STEP_INIT;
  return step;
}
static void postgres_async_abort(void *context) {
  postgres_async_fixture *fixture = context;
  ++fixture->aborted;
  orm_async_wait_destroy(&fixture->wait);
}
static void postgres_async_release(void *context) {
  postgres_async_fixture *fixture = context;
  ++fixture->released;
  orm_async_wait_destroy(&fixture->wait);
}

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

static const char *orm_postgres_test_result_sqlstate(const void *result) {
  const orm_postgres_test_result *typed =
      (const orm_postgres_test_result *)result;
  return typed->sqlstate != NULL ? typed->sqlstate
                                 : typed->error != NULL ? "23505" : NULL;
}

static int orm_postgres_test_connection_ok(void *context) {
  return *(const int *)context >= 0;
}

static const orm_postgres_command_ops orm_postgres_test_command_ops = {
    sizeof(orm_postgres_command_ops), ORM_POSTGRES_COMMAND_OPS_ABI_VERSION,
    orm_postgres_test_send, orm_postgres_test_single_row,
    orm_postgres_test_next_result, orm_postgres_test_release_result,
    orm_postgres_test_connection_error, orm_postgres_test_connection_ok};

static const orm_postgres_result_ops orm_postgres_test_result_ops = {
    sizeof(orm_postgres_result_ops), ORM_POSTGRES_RESULT_OPS_ABI_VERSION,
    orm_postgres_test_result_status, orm_postgres_test_result_rows,
    orm_postgres_test_result_columns, orm_postgres_test_column_name,
    orm_postgres_test_column_type, orm_postgres_test_is_null,
    orm_postgres_test_value, orm_postgres_test_length,
    orm_postgres_test_command_tuples, orm_postgres_test_result_error,
    orm_postgres_test_result_sqlstate};

static void orm_postgres_test_reset_mocks(void) {
  TINYMOCk_FUNCTION_RESET(orm_postgres_test_send_mock);
  TINYMOCk_FUNCTION_RESET(orm_postgres_test_single_row);
  TINYMOCk_FUNCTION_RESET(orm_postgres_test_release_result);
  orm_postgres_test_result_count = 0u;
  orm_postgres_test_result_calls = 0u;
  orm_postgres_test_release_count = 0u;
  orm_postgres_test_expected_send_context = NULL;
  orm_postgres_test_expected_send_request = NULL;
  orm_postgres_test_expected_single_context = NULL;
  orm_postgres_test_expected_send_count = 0u;
  orm_postgres_test_expected_single_count = 0u;
}

static void orm_postgres_test_verify_mocks(void) {
  TINYMOCk_FUNCTION_VERIFY_TIMES(orm_postgres_test_send_mock,
                                 orm_postgres_test_expected_send_count);
  if (orm_postgres_test_expected_send_count != 0u) {
    check_true(TINYMOCk_FUNCTION_ARG_POINTER_EQUAL(
        orm_postgres_test_send_mock, 0u, "context",
        orm_postgres_test_expected_send_context));
    check_true(TINYMOCk_FUNCTION_ARG_POINTER_EQUAL(
        orm_postgres_test_send_mock, 0u, "request",
        orm_postgres_test_expected_send_request));
  }
  TINYMOCk_FUNCTION_VERIFY_TIMES(orm_postgres_test_single_row,
                                 orm_postgres_test_expected_single_count);
  if (orm_postgres_test_expected_single_count != 0u)
    check_true(TINYMOCk_FUNCTION_ARG_POINTER_EQUAL(
        orm_postgres_test_single_row, 0u, "context",
        orm_postgres_test_expected_single_context));
  check_equal(orm_postgres_test_result_calls,
              orm_postgres_test_result_count);
  for (size_t i = 0u; i < orm_postgres_test_result_count; ++i)
    if (orm_postgres_test_results[i].check_connection)
      check_true(orm_postgres_test_actual_connections[i] ==
                 orm_postgres_test_results[i].expected_connection);
  TINYMOCk_FUNCTION_VERIFY_TIMES(orm_postgres_test_release_result,
                                 orm_postgres_test_release_count);
  for (size_t i = 0u; i < orm_postgres_test_release_count; ++i)
    check_true(TINYMOCk_FUNCTION_ARG_POINTER_EQUAL(
        orm_postgres_test_release_result, i, "result",
        orm_postgres_test_expected_releases[i]));
  TINYMOCk_FUNCTION_DESTROY(orm_postgres_test_send_mock);
  TINYMOCk_FUNCTION_DESTROY(orm_postgres_test_single_row);
  TINYMOCk_FUNCTION_DESTROY(orm_postgres_test_release_result);
}

static void orm_postgres_test_expect_send(
    void *connection, orm_postgres_query_request *request, int returned) {
  check_equal(orm_postgres_test_expected_send_count, (size_t)0u);
  check_true(TINYMOCk_FUNCTION_SET_RETURN(
      orm_postgres_test_send_mock, returned));
  orm_postgres_test_expected_send_context = connection;
  orm_postgres_test_expected_send_request = request;
  orm_postgres_test_expected_send_count = 1u;
}

static void orm_postgres_test_expect_next_impl(
    void *connection, bool check_connection, void *result) {
  check_less(orm_postgres_test_result_count,
             (size_t)ORM_POSTGRES_TEST_MAX_SCRIPTED_RESULTS);
  orm_postgres_test_results[orm_postgres_test_result_count++] =
      (orm_postgres_test_scripted_result){connection, result,
                                          check_connection};
}

static void orm_postgres_test_expect_next(void *connection, void *result) {
  orm_postgres_test_expect_next_impl(connection, true, result);
}

static void orm_postgres_test_expect_next_any(void *result) {
  orm_postgres_test_expect_next_impl(NULL, false, result);
}

static void orm_postgres_test_expect_release(void *result) {
  check_less(orm_postgres_test_release_count,
             (size_t)ORM_POSTGRES_TEST_MAX_SCRIPTED_RESULTS);
  orm_postgres_test_expected_releases[orm_postgres_test_release_count++] =
      result;
}

static void orm_postgres_test_expect_start(
    void *connection, orm_postgres_query_request *request) {
  int success = 1;
  orm_postgres_test_expect_send(connection, request, success);
  check_equal(orm_postgres_test_expected_single_count, (size_t)0u);
  check_true(TINYMOCk_FUNCTION_SET_RETURN(
      orm_postgres_test_single_row, success));
  orm_postgres_test_expected_single_context = connection;
  orm_postgres_test_expected_single_count = 1u;
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

static void orm_postgres_test_check_sint(cserde_reader *reader,
                                          int64_t expected) {
  cserde_token token = {0};
  check_equal(cserde_reader_next(reader, &token), CSERDE_OK);
  check_equal(token.kind, CSERDE_SINT);
  check_equal(token.value.sint, expected);
}

static void orm_postgres_test_check_uint(cserde_reader *reader,
                                          uint64_t expected) {
  cserde_token token = {0};
  check_equal(cserde_reader_next(reader, &token), CSERDE_OK);
  check_equal(token.kind, CSERDE_UINT);
  check_equal(token.value.uint, expected);
}

static void orm_postgres_test_check_float(cserde_reader *reader,
                                           double expected) {
  cserde_token token = {0};
  check_equal(cserde_reader_next(reader, &token), CSERDE_OK);
  check_equal(token.kind, CSERDE_FLOAT);
  check_equal(token.value.floating, expected);
}

static void orm_postgres_test_check_bool(cserde_reader *reader,
                                          bool expected) {
  cserde_token token = {0};
  check_equal(cserde_reader_next(reader, &token), CSERDE_OK);
  check_equal(token.kind, CSERDE_BOOL);
  check_equal(token.value.boolean, expected);
}

static int orm_postgres_test_use_comma_numeric_locale(char *saved_locale,
                                                       size_t capacity) {
  static const char *const candidates[] = {
      "German_Germany.1252", "de-DE", "de_DE.UTF-8", "fr_FR.UTF-8"};
  const char *current = setlocale(LC_NUMERIC, NULL);
  size_t index;
  if (current == NULL || strlen(current) >= capacity)
    return 0;
  (void)memcpy(saved_locale, current, strlen(current) + 1u);
  for (index = 0u; index < sizeof(candidates) / sizeof(candidates[0]); ++index) {
    if (setlocale(LC_NUMERIC, candidates[index]) != NULL &&
        localeconv()->decimal_point != NULL &&
        strcmp(localeconv()->decimal_point, ",") == 0)
      return 1;
  }
  (void)setlocale(LC_NUMERIC, saved_locale);
  return 0;
}

spec("ORM PostgreSQL single-row cursor") {
  it("parses backend decimal tokens independently of process locale") {
    static const unsigned char decimal[] = "1.25";
    char saved_locale[128];
    cserde_token token = {0};
    cserde_status status;
    const char *restored_locale;
    const int comma_locale = orm_postgres_test_use_comma_numeric_locale(
        saved_locale, sizeof(saved_locale));

#if defined(_WIN32)
    check_true(comma_locale);
#endif
    if (comma_locale) {
      status = orm_text_token_float(decimal, sizeof(decimal) - 1u, 1, &token);
      restored_locale = setlocale(LC_NUMERIC, saved_locale);
      check_equal(status, CSERDE_OK);
      check_equal(token.kind, CSERDE_FLOAT);
      check_equal(token.value.floating, 1.25);
      check_not_null(restored_locale);
    } else {
      check_equal(orm_text_token_float(decimal, sizeof(decimal) - 1u, 1,
                                       &token),
                  CSERDE_OK);
    }
  }

  it("rejects integer overflow and controls non-finite decimal tokens") {
    static const unsigned char sint_max[] = "9223372036854775807";
    static const unsigned char sint_min[] = "-9223372036854775808";
    static const unsigned char sint_overflow[] = "9223372036854775808";
    static const unsigned char uint_max[] = "18446744073709551615";
    static const unsigned char uint_overflow[] = "18446744073709551616";
    static const unsigned char negative_uint[] = "-1";
    static const unsigned char nan_text[] = "NaN";
    static const unsigned char float_overflow[] = "1e9999";
    cserde_token token = {0};

    check_equal(orm_text_token_sint(sint_max, sizeof(sint_max) - 1u, &token),
                CSERDE_OK);
    check_equal(token.value.sint, INT64_MAX);
    check_equal(orm_text_token_sint(sint_min, sizeof(sint_min) - 1u, &token),
                CSERDE_OK);
    check_equal(token.value.sint, INT64_MIN);
    check_equal(orm_text_token_sint(sint_overflow,
                                     sizeof(sint_overflow) - 1u, &token),
                CSERDE_SOURCE_ERROR);
    check_equal(orm_text_token_uint(uint_max, sizeof(uint_max) - 1u, &token),
                CSERDE_OK);
    check_equal(token.value.uint, UINT64_MAX);
    check_equal(orm_text_token_uint(uint_overflow,
                                     sizeof(uint_overflow) - 1u, &token),
                CSERDE_SOURCE_ERROR);
    check_equal(orm_text_token_uint(negative_uint,
                                     sizeof(negative_uint) - 1u, &token),
                CSERDE_SOURCE_ERROR);
    check_equal(orm_text_token_float(nan_text, sizeof(nan_text) - 1u, 0,
                                     &token),
                CSERDE_OK);
    check_true(isnan(token.value.floating));
    check_equal(orm_text_token_float(nan_text, sizeof(nan_text) - 1u, 1,
                                     &token),
                CSERDE_SOURCE_ERROR);
    check_equal(orm_text_token_float(float_overflow,
                                     sizeof(float_overflow) - 1u, 0, &token),
                CSERDE_SOURCE_ERROR);
  }

  it("rejects a row beyond max_result_rows") {
    static const char *const names[] = {"value"};
    static const uint32_t types[] = {25u};
    static const char *const values[] = {"x"};
    static const size_t lengths[] = {1u};
    static const uint8_t nulls[] = {0u};
    orm_postgres_test_result first = {
        ORM_POSTGRES_RESULT_SINGLE_ROW, 1u, names, types, values, lengths,
        nulls, NULL, NULL, NULL};
    orm_postgres_test_result second = first;
    orm_postgres_test_result terminal = {
        ORM_POSTGRES_RESULT_TUPLES_DONE, 1u, names, types, NULL, NULL, NULL,
        NULL, NULL, NULL};
    int connection_token = 0;
    orm_postgres_driver driver = {
        &orm_postgres_test_command_ops, &orm_postgres_test_result_ops,
        &connection_token};
    orm_postgres_query_request request = {
        "select value from rows", 0, NULL, NULL, NULL, NULL, 0};
    orm_postgres_cursor_config config = ORM_POSTGRES_CURSOR_CONFIG_INIT(
        1u, 1u, 128u, NULL, NULL, NULL);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};

    orm_error_init(&error);
    orm_postgres_test_reset_mocks();
    orm_postgres_test_expect_start(&connection_token, &request);
    orm_postgres_test_expect_next(&connection_token, &first);
    orm_postgres_test_expect_next(&connection_token, &second);
    orm_postgres_test_expect_next(&connection_token, &terminal);
    orm_postgres_test_expect_next(&connection_token, NULL);
    orm_postgres_test_expect_release(&first);
    orm_postgres_test_expect_release(&second);
    orm_postgres_test_expect_release(&terminal);

    check_equal(orm_postgres_cursor_start(&cursor, &driver, &request, &config,
                                          &error),
                ORM_STATUS_OK);
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

  it("owns optional execution metadata and drains on destroy") {
    int connection_token = 0;
    orm_postgres_driver driver = {
        &orm_postgres_test_command_ops, &orm_postgres_test_result_ops,
        &connection_token};
    orm_postgres_query_request request = {
        "select 1", 0, NULL, NULL, NULL, NULL, 0};
    orm_postgres_cursor_config config =
        ORM_POSTGRES_CURSOR_CONFIG_INIT(1u, UINT64_MAX, 64u, NULL, NULL, NULL);
    orm_row_cursor cursor = {0};
    orm_error_t error;

    orm_error_init(&error);
    orm_postgres_test_reset_mocks();
    orm_postgres_test_expect_start(&connection_token, &request);
    orm_postgres_test_expect_next(&connection_token, NULL);

    check_equal(orm_postgres_cursor_start(&cursor, &driver, &request,
                                          &config, &error),
                ORM_STATUS_OK);
    cursor.ops->destroy(cursor.context);
    orm_postgres_test_verify_mocks();
  }

  it("emits backend-typed scalar tokens and preserves field order") {
    static const char *const names[] = {
        "id", "small_value", "big_value", "active", "ratio", "real_value",
        "object_id", "name"};
    static const uint32_t types[] = {23u, 21u, 20u, 16u,
                                     701u, 700u, 26u, 25u};
    static const char *const values[] = {
        "7", "-12", "9223372036854775807", "t", "1.25", "2.5",
        "4294967295", "Alice"};
    static const size_t lengths[] = {1u, 3u, 19u, 1u, 4u, 3u, 10u, 5u};
    static const uint8_t nulls[] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    orm_postgres_test_result row = {
        ORM_POSTGRES_RESULT_SINGLE_ROW, 8u, names, types, values, lengths,
        nulls, NULL, NULL, NULL};
    orm_postgres_test_result terminal = {
        ORM_POSTGRES_RESULT_TUPLES_DONE, 8u, names, types, NULL, NULL, NULL,
        NULL, NULL, NULL};
    int connection_token = 0;
    orm_postgres_driver driver = {
        &orm_postgres_test_command_ops, &orm_postgres_test_result_ops,
        &connection_token};
    orm_postgres_query_request request = {
        "select id, small_value, big_value, active, ratio, real_value, "
        "object_id, name from person",
        0, NULL, NULL, NULL, NULL, 0};
    uint64_t affected_rows = 0u;
    size_t columns = 0u;
    orm_error_t runtime_error;
    orm_postgres_cursor_config config = ORM_POSTGRES_CURSOR_CONFIG_INIT(
        8u, UINT64_MAX, 1024u, &columns, &affected_rows, &runtime_error);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};
    cserde_token token = {0};

    orm_error_init(&error);
    orm_error_init(&runtime_error);
    orm_postgres_test_reset_mocks();
    orm_postgres_test_expect_start(&connection_token, &request);
    orm_postgres_test_expect_next(&connection_token, &row);
    orm_postgres_test_expect_next(&connection_token, &terminal);
    orm_postgres_test_expect_next(&connection_token, NULL);
    orm_postgres_test_expect_release(&row);
    orm_postgres_test_expect_release(&terminal);

    check_equal(orm_postgres_cursor_start(&cursor, &driver, &request,
                                          &config, &error), ORM_STATUS_OK);
    check_equal(cursor.ops->next(cursor.context, &reader).kind,
                ORM_ROW_CURSOR_ROW);
    orm_postgres_test_check_token(&reader, CSERDE_MAP_BEGIN, NULL, 0u);
    orm_postgres_test_check_token(&reader, CSERDE_STRING, "id", 2u);
    orm_postgres_test_check_sint(&reader, INT64_C(7));
    orm_postgres_test_check_token(&reader, CSERDE_STRING, "small_value", 11u);
    orm_postgres_test_check_sint(&reader, INT64_C(-12));
    orm_postgres_test_check_token(&reader, CSERDE_STRING, "big_value", 9u);
    orm_postgres_test_check_sint(&reader, INT64_MAX);
    orm_postgres_test_check_token(&reader, CSERDE_STRING, "active", 6u);
    orm_postgres_test_check_bool(&reader, true);
    orm_postgres_test_check_token(&reader, CSERDE_STRING, "ratio", 5u);
    orm_postgres_test_check_float(&reader, 1.25);
    orm_postgres_test_check_token(&reader, CSERDE_STRING, "real_value", 10u);
    orm_postgres_test_check_float(&reader, 2.5);
    orm_postgres_test_check_token(&reader, CSERDE_STRING, "object_id", 9u);
    orm_postgres_test_check_uint(&reader, UINT64_C(4294967295));
    orm_postgres_test_check_token(&reader, CSERDE_STRING, "name", 4u);
    orm_postgres_test_check_token(&reader, CSERDE_STRING, "Alice", 5u);
    orm_postgres_test_check_token(&reader, CSERDE_MAP_END, NULL, 0u);
    check_equal(cserde_reader_next(&reader, &token), CSERDE_DONE);
    check_equal(cursor.ops->next(cursor.context, &reader).kind,
                ORM_ROW_CURSOR_DONE);
    check_equal(columns, (size_t)8u);
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
        nulls, NULL, NULL, NULL};
    orm_postgres_test_result terminal = {
        ORM_POSTGRES_RESULT_TUPLES_DONE, 2u, names, types, NULL, NULL, NULL,
        NULL, NULL, NULL};
    int connection_token = 0;
    orm_postgres_driver driver = {
        &orm_postgres_test_command_ops, &orm_postgres_test_result_ops,
        &connection_token};
    orm_postgres_query_request request = {
        "select payload, missing from files", 0, NULL, NULL, NULL, NULL, 0};
    orm_error_t runtime_error;
    orm_postgres_cursor_config config =
        ORM_POSTGRES_CURSOR_CONFIG_INIT(2u, UINT64_MAX, 1024u, NULL, NULL,
                                        &runtime_error);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};
    cserde_token token = {0};

    orm_error_init(&error);
    orm_error_init(&runtime_error);
    orm_postgres_test_reset_mocks();
    orm_postgres_test_expect_start(&connection_token, &request);
    orm_postgres_test_expect_next(&connection_token, &row);
    orm_postgres_test_expect_next(&connection_token, &terminal);
    orm_postgres_test_expect_next(&connection_token, NULL);
    orm_postgres_test_expect_release(&row);
    orm_postgres_test_expect_release(&terminal);

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
        lengths, nulls, NULL, NULL, NULL};
    orm_postgres_test_result second = {
        ORM_POSTGRES_RESULT_SINGLE_ROW, 1u, names, types, second_values,
        lengths, nulls, NULL, NULL, NULL};
    orm_postgres_test_result terminal = {
        ORM_POSTGRES_RESULT_TUPLES_DONE, 1u, names, types, NULL, NULL, NULL,
        NULL, NULL, NULL};
    int connection_token = 0;
    orm_postgres_driver driver = {
        &orm_postgres_test_command_ops, &orm_postgres_test_result_ops,
        &connection_token};
    orm_postgres_query_request request = {
        "select value from payloads", 0, NULL, NULL, NULL, NULL, 0};
    orm_error_t runtime_error;
    orm_postgres_cursor_config config =
        ORM_POSTGRES_CURSOR_CONFIG_INIT(1u, UINT64_MAX, 4u, NULL, NULL,
                                        &runtime_error);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};

    orm_error_init(&error);
    orm_error_init(&runtime_error);
    orm_postgres_test_reset_mocks();
    orm_postgres_test_expect_start(&connection_token, &request);
    orm_postgres_test_expect_next(&connection_token, &first);
    orm_postgres_test_expect_next(&connection_token, &second);
    orm_postgres_test_expect_next(&connection_token, &terminal);
    orm_postgres_test_expect_next(&connection_token, NULL);
    orm_postgres_test_expect_release(&first);
    orm_postgres_test_expect_release(&second);
    orm_postgres_test_expect_release(&terminal);

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
        nulls, NULL, NULL, NULL};
    orm_postgres_test_result fatal = {
        ORM_POSTGRES_RESULT_ERROR, 0u, NULL, NULL, NULL, NULL, NULL, NULL,
        "forced error after one row", NULL};
    int connection_token = 0;
    orm_postgres_driver driver = {
        &orm_postgres_test_command_ops, &orm_postgres_test_result_ops,
        &connection_token};
    orm_postgres_query_request request = {
        "select value from unstable_query", 0, NULL, NULL, NULL, NULL, 0};
    orm_error_t runtime_error;
    orm_postgres_cursor_config config =
        ORM_POSTGRES_CURSOR_CONFIG_INIT(1u, UINT64_MAX, 128u, NULL, NULL,
                                        &runtime_error);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};

    orm_error_init(&error);
    orm_error_init(&runtime_error);
    orm_postgres_test_reset_mocks();
    orm_postgres_test_expect_start(&connection_token, &request);
    orm_postgres_test_expect_next(&connection_token, &row);
    orm_postgres_test_expect_next(&connection_token, &fatal);
    orm_postgres_test_expect_next(&connection_token, NULL);
    orm_postgres_test_expect_release(&row);
    orm_postgres_test_expect_release(&fatal);

    check_equal(orm_postgres_cursor_start(&cursor, &driver, &request,
                                          &config, &error), ORM_STATUS_OK);
    check_equal(cursor.ops->next(cursor.context, &reader).kind,
                ORM_ROW_CURSOR_ROW);
    {
      const orm_row_cursor_step step = cursor.ops->next(cursor.context, &reader);
      check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
      check_equal(step.status, ORM_STATUS_CONSTRAINT);
      check_not_null(strstr(step.message, "SQLSTATE=23505"));
      check_not_null(strstr(step.message, "forced error after one row"));
    }
    cursor.ops->destroy(cursor.context);
    orm_postgres_test_verify_mocks();
  }

  it("classifies only PostgreSQL errors with a safe retry contract as busy") {
    static const struct {
      const char *sqlstate;
      orm_status_t expected;
    } cases[] = {{"40001", ORM_STATUS_BUSY},
                 {"40P01", ORM_STATUS_BUSY},
                 {"55P03", ORM_STATUS_BUSY},
                 {"40003", ORM_STATUS_SQL_ERROR}};
    int connection_token = 0;
    orm_postgres_driver driver = {
        &orm_postgres_test_command_ops, &orm_postgres_test_result_ops,
        &connection_token};
    orm_postgres_query_request request = {
        "update retry_contract set value = 1", 0, NULL, NULL, NULL, NULL, 0};
    orm_error_t runtime_error;
    orm_postgres_cursor_config config = ORM_POSTGRES_CURSOR_CONFIG_INIT(
        1u, UINT64_MAX, 1u, NULL, NULL, &runtime_error);

    for (size_t index = 0u; index < sizeof(cases) / sizeof(cases[0]); ++index) {
      orm_postgres_test_result fatal = {
          .status = ORM_POSTGRES_RESULT_ERROR,
          .error = "retry classification",
          .sqlstate = cases[index].sqlstate};
      orm_row_cursor cursor = {0};
      orm_error_t error;
      cserde_reader reader = {0};

      orm_error_init(&error);
      orm_error_init(&runtime_error);
      orm_postgres_test_reset_mocks();
      orm_postgres_test_expect_start(&connection_token, &request);
      orm_postgres_test_expect_next(&connection_token, &fatal);
      orm_postgres_test_expect_next(&connection_token, NULL);
      orm_postgres_test_expect_release(&fatal);

      check_equal(orm_postgres_cursor_start(&cursor, &driver, &request,
                                            &config, &error), ORM_STATUS_OK);
      {
        const orm_row_cursor_step step =
            cursor.ops->next(cursor.context, &reader);
        check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
        check_equal(step.status, cases[index].expected);
      }
      cursor.ops->destroy(cursor.context);
      orm_postgres_test_verify_mocks();
    }
  }

  it("reports affected rows only after command completion") {
    orm_postgres_test_result command = {
        ORM_POSTGRES_RESULT_COMMAND_DONE, 0u, NULL, NULL, NULL, NULL, NULL,
        "42", NULL, NULL};
    int connection_token = 0;
    orm_postgres_driver driver = {
        &orm_postgres_test_command_ops, &orm_postgres_test_result_ops,
        &connection_token};
    orm_postgres_query_request request = {
        "update person set active = false", 0, NULL, NULL, NULL, NULL, 0};
    uint64_t affected_rows = 0u;
    orm_error_t runtime_error;
    orm_postgres_cursor_config config = ORM_POSTGRES_CURSOR_CONFIG_INIT(
        1u, UINT64_MAX, 1u, NULL, &affected_rows, &runtime_error);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};

    orm_error_init(&error);
    orm_error_init(&runtime_error);
    orm_postgres_test_reset_mocks();
    orm_postgres_test_expect_start(&connection_token, &request);
    orm_postgres_test_expect_next(&connection_token, &command);
    orm_postgres_test_expect_next(&connection_token, NULL);
    orm_postgres_test_expect_release(&command);

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
        "-1", NULL, NULL};
    int connection_token = 0;
    orm_postgres_driver driver = {
        &orm_postgres_test_command_ops, &orm_postgres_test_result_ops,
        &connection_token};
    orm_postgres_query_request request = {
        "update person set active = false", 0, NULL, NULL, NULL, NULL, 0};
    orm_error_t runtime_error;
    orm_postgres_cursor_config config =
        ORM_POSTGRES_CURSOR_CONFIG_INIT(1u, UINT64_MAX, 1u, NULL, NULL,
                                        &runtime_error);
    orm_row_cursor cursor = {0};
    orm_error_t error;
    cserde_reader reader = {0};

    orm_error_init(&error);
    orm_error_init(&runtime_error);
    orm_postgres_test_reset_mocks();
    orm_postgres_test_expect_start(&connection_token, &request);
    orm_postgres_test_expect_next(&connection_token, &command);
    orm_postgres_test_expect_next(&connection_token, NULL);
    orm_postgres_test_expect_release(&command);

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


/* These scripted native responses exercise the real cursor implementation.
 * They are not a live-server/network-loss or native ownership proof. */
static void postgres_check_failure(const char *sqlstate, int token,
                                   orm_status_t expected) {
  orm_postgres_test_result fatal = {
      .status = ORM_POSTGRES_RESULT_ERROR,
      .error = "native diagnostic preserved",
      .sqlstate = sqlstate};
  orm_postgres_driver driver = {
      &orm_postgres_test_command_ops, &orm_postgres_test_result_ops, &token};
  orm_postgres_query_request request = {
      "select 1", 0, NULL, NULL, NULL, NULL, 0};
  orm_error_t error, runtime_error;
  orm_postgres_cursor_config config = ORM_POSTGRES_CURSOR_CONFIG_INIT(
      1u, 1u, 64u, NULL, NULL, &runtime_error);
  orm_row_cursor cursor = {0};
  cserde_reader reader = {0};
  orm_error_init(&error); orm_error_init(&runtime_error);
  orm_postgres_test_reset_mocks();
  orm_postgres_test_expect_start(&token, &request);
  orm_postgres_test_expect_next(&token, &fatal);
  orm_postgres_test_expect_next(&token, NULL);
  orm_postgres_test_expect_release(&fatal);
  check_equal(orm_postgres_cursor_start(&cursor, &driver, &request, &config,
                                       &error), ORM_STATUS_OK);
  const orm_row_cursor_step step = cursor.ops->next(cursor.context, &reader);
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "%s", step.message != NULL ? step.message : "");
  cursor.ops->destroy(cursor.context);
  check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
  check_equal(step.status, expected);
  check_not_null(strstr(message, "native diagnostic preserved"));
  check_equal(runtime_error.status, expected);
  /* The caller-owned diagnostic survives the native result and cursor. */
  check_not_null(strstr(runtime_error.message, "native diagnostic preserved"));
  orm_postgres_test_verify_mocks();
}

static void postgres_check_eof(int token, orm_status_t expected) {
  orm_postgres_driver driver = {
      &orm_postgres_test_command_ops, &orm_postgres_test_result_ops, &token};
  orm_postgres_query_request request = {
      "select 1", 0, NULL, NULL, NULL, NULL, 0};
  orm_error_t error, runtime_error;
  orm_postgres_cursor_config config = ORM_POSTGRES_CURSOR_CONFIG_INIT(
      1u, 1u, 64u, NULL, NULL, &runtime_error);
  orm_row_cursor cursor = {0};
  cserde_reader reader = {0};
  orm_error_init(&error); orm_error_init(&runtime_error);
  orm_postgres_test_reset_mocks();
  orm_postgres_test_expect_start(&token, &request);
  orm_postgres_test_expect_next(&token, NULL);
  check_equal(orm_postgres_cursor_start(&cursor, &driver, &request, &config,
                                       &error), ORM_STATUS_OK);
  const orm_row_cursor_step step = cursor.ops->next(cursor.context, &reader);
  const orm_row_cursor_step_kind repeated = cursor.ops->next(cursor.context, &reader).kind;
  cursor.ops->destroy(cursor.context);
  check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
  check_equal(step.status, expected);
  check_equal(runtime_error.status, expected);
  check_equal(repeated, ORM_ROW_CURSOR_DONE);
  orm_postgres_test_verify_mocks();
}

static void postgres_check_send_failure(int token, orm_status_t expected) {
  orm_postgres_driver driver = {
      &orm_postgres_test_command_ops, &orm_postgres_test_result_ops, &token};
  orm_postgres_query_request request = {
      "select 1", 0, NULL, NULL, NULL, NULL, 0};
  orm_error_t error;
  orm_postgres_cursor_config config = ORM_POSTGRES_CURSOR_CONFIG_INIT(
      1u, 1u, 64u, NULL, NULL, NULL);
  orm_row_cursor cursor = {0};
  orm_error_init(&error); orm_postgres_test_reset_mocks();
  orm_postgres_test_expect_send(&token, &request, 0);
  check_equal(orm_postgres_cursor_start(&cursor, &driver, &request, &config,
                                       &error), expected);
  check_equal(error.status, expected);
  check_null(cursor.context); check_null(cursor.ops);
  orm_postgres_test_verify_mocks();
}

static int postgres_unexpected_send(void *context,
                                     const orm_postgres_query_request *request) {
  (void)request;
  ++*(int *)context;
  return 0;
}

static void postgres_check_completion(orm_postgres_result_status result_status) {
  static const char *const names[] = {"id"};
  static const uint32_t types[] = {23u};
  static const char *const values[] = {"7"};
  static const size_t lengths[] = {1u};
  static const uint8_t nulls[] = {0u};
  const int is_row = result_status == ORM_POSTGRES_RESULT_SINGLE_ROW;
  orm_postgres_test_result result = {
      .status = result_status, .columns = is_row ? 1u : 0u,
      .names = names, .types = types, .values = values,
      .lengths = lengths, .nulls = nulls, .affected_rows = "1"};
  int token = 0;
  orm_postgres_driver driver = {
      &orm_postgres_test_command_ops, &orm_postgres_test_result_ops, &token};
  orm_postgres_query_request request = {"select 7 as id", 0, NULL, NULL, NULL, NULL, 0};
  orm_error_t error, runtime_error;
  orm_postgres_cursor_config config = ORM_POSTGRES_CURSOR_CONFIG_INIT(
      1u, 1u, 64u, NULL, NULL, &runtime_error);
  orm_row_cursor cursor = {0}; cserde_reader reader = {0};
  cserde_token value = {0}; cserde_status decoded = CSERDE_OK;
  orm_error_init(&error); orm_error_init(&runtime_error);
  orm_postgres_test_reset_mocks(); orm_postgres_test_expect_start(&token, &request);
  orm_postgres_test_expect_next(&token, &result);
  orm_postgres_test_expect_next(&token, NULL);
  orm_postgres_test_expect_release(&result);
  check_equal(orm_postgres_cursor_start(&cursor, &driver, &request, &config,
                                       &error), ORM_STATUS_OK);
  /* A completed command is a known result, even if EOF arrives after loss. */
  if (!is_row) token = -1;
  const orm_row_cursor_step first_step = cursor.ops->next(cursor.context, &reader);
  orm_row_cursor_step last_step = first_step;
  if (is_row && first_step.kind == ORM_ROW_CURSOR_ROW) {
    for (int i = 0; i < 3 && decoded == CSERDE_OK; ++i)
      decoded = cserde_reader_next(&reader, &value);
    token = -1;
    last_step = cursor.ops->next(cursor.context, &reader);
  }
  cursor.ops->destroy(cursor.context);
  if (is_row) {
    check_equal(first_step.kind, ORM_ROW_CURSOR_ROW);
    check_equal(decoded, CSERDE_OK);
    check_equal(value.kind, CSERDE_SINT); check_equal(value.value.sint, INT64_C(7));
    check_equal(last_step.kind, ORM_ROW_CURSOR_ERROR);
    check_equal(last_step.status, ORM_STATUS_CONNECTION_ERROR);
    check_equal(runtime_error.status, ORM_STATUS_CONNECTION_ERROR);
  } else {
    check_equal(last_step.kind, ORM_ROW_CURSOR_DONE);
    check_equal(runtime_error.status, ORM_STATUS_OK);
  }
  orm_postgres_test_verify_mocks();
}

spec("ORM PostgreSQL connection failure classification") {
  (void)ttest_config__;
  it("classifies connection-exception SQLSTATEs without treating them as BUSY") {
    static const char *const states[] = {"08000", "08003", "08006", "08007"};
    for (size_t i = 0u; i < sizeof(states) / sizeof(states[0]); ++i)
      postgres_check_failure(states[i], 0, ORM_STATUS_CONNECTION_ERROR);
  }
  it("classifies server shutdown while the cached connection status is still OK") {
    postgres_check_failure("57P01", 0, ORM_STATUS_CONNECTION_ERROR);
    postgres_check_failure("57P02", 0, ORM_STATUS_CONNECTION_ERROR);
  }
  it("uses native connection health when a fatal result has no SQLSTATE") {
    postgres_check_failure("", -1, ORM_STATUS_CONNECTION_ERROR);
  }
  it("keeps an unclassified healthy-connection error as SQL_ERROR") {
    postgres_check_failure("", 0, ORM_STATUS_SQL_ERROR);
  }
  it("preserves an explicit server rejection even when the connection later fails") {
    postgres_check_failure("23505", -1, ORM_STATUS_CONSTRAINT);
  }
  it("does not silently finish an unacknowledged query on a healthy connection") {
    postgres_check_eof(0, ORM_STATUS_DATASTORE_ERROR);
  }
  it("does not silently finish an unacknowledged query on a broken connection") {
    postgres_check_eof(-1, ORM_STATUS_CONNECTION_ERROR);
  }
  it("classifies failed dispatch on a broken connection and leaves no cursor") {
    postgres_check_send_failure(-1, ORM_STATUS_CONNECTION_ERROR);
  }
  it("does not infer a connection failure from a healthy-connection send rejection") {
    postgres_check_send_failure(0, ORM_STATUS_SQL_ERROR);
  }
  it("reports connection loss after an already decoded partial row") {
    postgres_check_completion(ORM_POSTGRES_RESULT_SINGLE_ROW);
  }
  it("accepts a confirmed empty query completion instead of mistaking it for truncation") {
    postgres_check_completion(ORM_POSTGRES_RESULT_TUPLES_DONE);
  }
  it("does not reclassify a confirmed command as unknown after connection loss") {
    postgres_check_completion(ORM_POSTGRES_RESULT_COMMAND_DONE);
  }
  it("requires the native health callback before dispatch") {
    int token = 0;
    orm_postgres_command_ops incomplete = orm_postgres_test_command_ops;
    incomplete.connection_ok = NULL;
    incomplete.send_query = postgres_unexpected_send;
    orm_postgres_driver driver = {&incomplete, &orm_postgres_test_result_ops, &token};
    orm_postgres_query_request request = {"select 1", 0, NULL, NULL, NULL, NULL, 0};
    orm_postgres_cursor_config config = ORM_POSTGRES_CURSOR_CONFIG_INIT(
        1u, 1u, 64u, NULL, NULL, NULL);
    orm_row_cursor cursor = {0}; orm_error_t error;
    orm_error_init(&error); orm_postgres_test_reset_mocks();
    check_equal(orm_postgres_cursor_start(&cursor, &driver, &request, &config,
                                         &error), ORM_STATUS_INVALID_ARGUMENT);
    check_null(cursor.context); check_null(cursor.ops);
    check_equal(token, 0);
    orm_postgres_test_verify_mocks();
  }
}

/* Draining is an error-observation boundary even without a preceding next(). */
static void postgres_check_cancel_error(const char *sqlstate, int connection_token,
                                       int resume_first, orm_status_t expected) {
  orm_postgres_test_result result = {
      .status = ORM_POSTGRES_RESULT_ERROR, .error = "drained native error",
      .sqlstate = sqlstate};
  orm_postgres_driver driver = {
      &orm_postgres_test_command_ops, &orm_postgres_test_result_ops, &connection_token};
  orm_postgres_query_request request = {"select 1", 0, NULL, NULL, NULL, NULL, 0};
  orm_error_t error, runtime_error;
  orm_postgres_cursor_config config = ORM_POSTGRES_CURSOR_CONFIG_INIT(
      1u, 1u, 64u, NULL, NULL, &runtime_error);
  orm_row_cursor cursor = {0}; cserde_reader reader = {0};
  orm_error_init(&error); orm_error_init(&runtime_error);
  orm_postgres_test_reset_mocks();
  orm_postgres_test_expect_start(&connection_token, &request);
  if (sqlstate != NULL) {
    orm_postgres_test_expect_next(&connection_token, &result);
    orm_postgres_test_expect_release(&result);
  }
  orm_postgres_test_expect_next(&connection_token, NULL);
  const orm_status_t started = orm_postgres_cursor_start(&cursor, &driver, &request,
                                                        &config, &error);
  if (started == ORM_STATUS_OK) {
    if (resume_first) (void)cursor.ops->next(cursor.context, &reader);
    cursor.ops->cancel(cursor.context);
    cursor.ops->cancel(cursor.context);
    cursor.ops->destroy(cursor.context);
  }
  check_equal(started, ORM_STATUS_OK);
  check_equal(runtime_error.status, expected);
  if (expected != ORM_STATUS_OK) check_true(runtime_error.message[0] != '\0');
  orm_postgres_test_verify_mocks();
}

spec("ORM PostgreSQL cancellation drain diagnostics") {
  (void)ttest_config__;
  it("observes server termination while cancelling unconsumed results") {
    postgres_check_cancel_error("57P01", 0, 0, ORM_STATUS_CONNECTION_ERROR);
  }
  it("observes connection exceptions while cancelling unconsumed results") {
    postgres_check_cancel_error("08006", 0, 0, ORM_STATUS_CONNECTION_ERROR);
  }
  it("observes disconnected EOF while cancelling without a result") {
    postgres_check_cancel_error(NULL, -1, 0, ORM_STATUS_CONNECTION_ERROR);
  }
  it("keeps ordinary drained SQL rejection distinct from connection loss") {
    postgres_check_cancel_error("23505", 0, 0, ORM_STATUS_CONSTRAINT);
  }
  it("keeps a healthy empty drain successful") {
    postgres_check_cancel_error(NULL, 0, 0, ORM_STATUS_OK);
  }
  it("preserves the operation error when cancellation later discovers disconnection") {
    postgres_check_cancel_error("23505", -1, 1, ORM_STATUS_CONSTRAINT);
  }
}

spec("PostgreSQL asynchronous cursor ownership") {
  (void)ttest_config__;
  static cflow_scheduler scheduler;
  static postgres_async_fixture fixture;
  static orm_row_cursor cursor;
  static orm_error_t error;
  static int connection;
  static orm_postgres_query_request request;
  before_each() {
    scheduler = (cflow_scheduler){0};
    check_true(cflow_scheduler_test_init(&scheduler));
    fixture = (postgres_async_fixture){0};
    const orm_async_config_t async_config = {sizeof(orm_async_config_t), &scheduler, 2u, 10u};
    check_true(orm_async_wait_init(&fixture.wait, &async_config));
    cursor = (orm_row_cursor){0};
    connection = 0;
    orm_error_init(&error);
    request = (orm_postgres_query_request){"select 1", 0, NULL, NULL, NULL, NULL, 0};
    const orm_postgres_driver driver = {&orm_postgres_test_command_ops,
        &orm_postgres_test_result_ops, &connection};
    const orm_postgres_cursor_config config = ORM_POSTGRES_CURSOR_CONFIG_INIT(4u, 8u, 4096u, NULL, NULL, NULL);
    const orm_postgres_async async = {&fixture, postgres_async_poll, postgres_async_abort, postgres_async_release};
    orm_postgres_test_reset_mocks();
    orm_postgres_test_expect_start(&connection, &request);
    check_equal(orm_postgres_cursor_start_async(&cursor, &driver, &request, &config,
                                               &async, &error), ORM_STATUS_OK);
  }
  after_each() {
    cursor.ops->destroy(cursor.context);
    check_equal(fixture.released, 1);
    cflow_scheduler_destroy(&scheduler);
    orm_postgres_test_verify_mocks();
  }
  it("returns WAIT without reading a result and aborts cancellation without draining") {
    cserde_reader reader = {0};
    orm_row_cursor_step step = cursor.ops->next(cursor.context, &reader);
    check_equal(step.kind, ORM_ROW_CURSOR_WAIT);
    check_true(cflow_waitable_valid(&step.waitable));
    cursor.ops->cancel(cursor.context);
    cursor.ops->cancel(cursor.context);
    check_equal(fixture.aborted, 1);
    check_equal(orm_postgres_test_result_calls, (size_t)0u);
  }
  it("reads ready results and releases a fully consumed query without aborting") {
    cserde_reader reader = {0};
    check_equal(cursor.ops->next(cursor.context, &reader).kind, ORM_ROW_CURSOR_WAIT);
    fixture.ready = 1;
    orm_postgres_test_result terminal = {0};
    terminal.status = ORM_POSTGRES_RESULT_TUPLES_DONE;
    orm_postgres_test_expect_next_any(&terminal);
    orm_postgres_test_expect_release(&terminal);
    orm_postgres_test_expect_next_any(NULL);
    check_equal(cursor.ops->next(cursor.context, &reader).kind, ORM_ROW_CURSOR_DONE);
    check_equal(cursor.ops->next(cursor.context, &reader).kind, ORM_ROW_CURSOR_DONE);
    check_equal(fixture.aborted, 0);
    check_equal(orm_postgres_test_result_calls, (size_t)2u);
  }
  it("reports the deadline without performing a blocking result read") {
    cserde_reader reader = {0};
    check_equal(cursor.ops->next(cursor.context, &reader).kind, ORM_ROW_CURSOR_WAIT);
    (void)cflow_scheduler_advance(&scheduler, 10u);
    orm_row_cursor_step step = cursor.ops->next(cursor.context, &reader);
    check_equal(step.kind, ORM_ROW_CURSOR_ERROR);
    check_equal(step.status, ORM_STATUS_CONNECTION_ERROR);
    cursor.ops->cancel(cursor.context);
    check_equal(fixture.aborted, 1);
    check_equal(orm_postgres_test_result_calls, (size_t)0u);
  }
}
