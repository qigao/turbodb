#include "row_reader.h"

#include <tinytest.h>

#include <stdint.h>
#include <string.h>

static mysql_column_definition_t test_column(
    const char *name, uint8_t type, uint16_t charset) {
  mysql_column_definition_t column;
  memset(&column, 0, sizeof(column));
  column.name.data = (const uint8_t *)name;
  column.name.length = strlen(name);
  column.type = type;
  column.character_set = charset;
  return column;
}

static void next_kind(
    cserde_reader *reader, cserde_token_kind expected,
    cserde_token *token) {
  check_equal(cserde_reader_next(reader, token), CSERDE_OK);
  check_equal(token->kind, expected);
}

spec("mysql CSerde row reader") {
  (void)ttest_config__;

  it("maps scalar text decimal and binary values into one row map") {
    static const uint8_t text[] = {'h','e','l','l','o'};
    static const uint8_t decimal[] = {'1','2','3','.','4','5'};
    static const uint8_t blob[] = {0x00,0xff,0x01};
    mysql_column_definition_t columns[7] = {
      test_column("n", MYSQL_FIELD_TYPE_LONGLONG, 63u),
      test_column("u", MYSQL_FIELD_TYPE_LONGLONG, 63u),
      test_column("f", MYSQL_FIELD_TYPE_DOUBLE, 63u),
      test_column("nil", MYSQL_FIELD_TYPE_NULL, 63u),
      test_column("txt", MYSQL_FIELD_TYPE_VAR_STRING, 255u),
      test_column("decv", MYSQL_FIELD_TYPE_NEWDECIMAL, 255u),
      test_column("bin", MYSQL_FIELD_TYPE_BLOB, 63u)
    };
    mysql_binary_value_t values[7] = {
      {.kind = MYSQL_BINARY_VALUE_SINT64,
       .mysql_type = MYSQL_FIELD_TYPE_LONGLONG,
       .data.sint64_value = -42},
      {.kind = MYSQL_BINARY_VALUE_UINT64,
       .mysql_type = MYSQL_FIELD_TYPE_LONGLONG,
       .data.uint64_value = UINT64_MAX},
      {.kind = MYSQL_BINARY_VALUE_DOUBLE,
       .mysql_type = MYSQL_FIELD_TYPE_DOUBLE,
       .data.double_value = 10.5},
      {.kind = MYSQL_BINARY_VALUE_NULL,
       .mysql_type = MYSQL_FIELD_TYPE_NULL},
      {.kind = MYSQL_BINARY_VALUE_BYTES,
       .mysql_type = MYSQL_FIELD_TYPE_VAR_STRING,
       .data.bytes = {text, sizeof(text), false}},
      {.kind = MYSQL_BINARY_VALUE_BYTES,
       .mysql_type = MYSQL_FIELD_TYPE_NEWDECIMAL,
       .data.bytes = {decimal, sizeof(decimal), false}},
      {.kind = MYSQL_BINARY_VALUE_BYTES,
       .mysql_type = MYSQL_FIELD_TYPE_BLOB,
       .data.bytes = {blob, sizeof(blob), false}}
    };
    mysql_row_reader_state state;
    cserde_reader reader = {0};
    cserde_token token;
    size_t i;

    check_equal(mysql_row_reader_init(
                    &state, columns, values, 7u, &reader),
                CSERDE_OK);

    next_kind(&reader, CSERDE_MAP_BEGIN, &token);
    for (i = 0u; i < 7u; ++i) {
      next_kind(&reader, CSERDE_STRING, &token);
      check_equal(token.value.slice.size, columns[i].name.length);
      check_equal(memcmp(token.value.slice.data, columns[i].name.data,
                         columns[i].name.length), 0);

      switch (i) {
        case 0u:
          next_kind(&reader, CSERDE_SINT, &token);
          check_equal(token.value.sint, INT64_C(-42));
          break;
        case 1u:
          next_kind(&reader, CSERDE_UINT, &token);
          check_equal(token.value.uint, UINT64_MAX);
          break;
        case 2u:
          next_kind(&reader, CSERDE_FLOAT, &token);
          check_true(token.value.floating > 10.49);
          check_true(token.value.floating < 10.51);
          break;
        case 3u:
          next_kind(&reader, CSERDE_NULL, &token);
          break;
        case 4u:
          next_kind(&reader, CSERDE_STRING, &token);
          check_equal(token.value.slice.size, sizeof(text));
          check_equal(memcmp(token.value.slice.data, text, sizeof(text)), 0);
          break;
        case 5u:
          next_kind(&reader, CSERDE_STRING, &token);
          check_equal(token.value.slice.size, sizeof(decimal));
          check_equal(memcmp(token.value.slice.data, decimal,
                             sizeof(decimal)), 0);
          break;
        case 6u:
          next_kind(&reader, CSERDE_BYTES, &token);
          check_equal(token.value.slice.size, sizeof(blob));
          check_equal(memcmp(token.value.slice.data, blob, sizeof(blob)), 0);
          break;
        default:
          check_true(0);
      }
    }
    next_kind(&reader, CSERDE_MAP_END, &token);
    check_equal(cserde_reader_next(&reader, &token), CSERDE_DONE);
  }

  it("treats nonbinary BLOB-family values as strings") {
    static const uint8_t text[] = {'t','e','x','t'};
    mysql_column_definition_t column =
        test_column("body", MYSQL_FIELD_TYPE_BLOB, 255u);
    mysql_binary_value_t value = {
      .kind = MYSQL_BINARY_VALUE_BYTES,
      .mysql_type = MYSQL_FIELD_TYPE_BLOB,
      .data.bytes = {text, sizeof(text), false}
    };
    mysql_row_reader_state state;
    cserde_reader reader = {0};
    cserde_token token;

    check_equal(mysql_row_reader_init(
                    &state, &column, &value, 1u, &reader),
                CSERDE_OK);
    next_kind(&reader, CSERDE_MAP_BEGIN, &token);
    next_kind(&reader, CSERDE_STRING, &token);
    next_kind(&reader, CSERDE_STRING, &token);
    check_equal(token.value.slice.size, sizeof(text));
    check_equal(memcmp(token.value.slice.data, text, sizeof(text)), 0);
  }

  it("formats DATE DATETIME TIMESTAMP and TIME without timezone conversion") {
    mysql_column_definition_t columns[4] = {
      test_column("d", MYSQL_FIELD_TYPE_DATE, 63u),
      test_column("dt", MYSQL_FIELD_TYPE_DATETIME, 63u),
      test_column("ts", MYSQL_FIELD_TYPE_TIMESTAMP, 63u),
      test_column("tm", MYSQL_FIELD_TYPE_TIME, 63u)
    };
    mysql_binary_value_t values[4] = {
      {.kind = MYSQL_BINARY_VALUE_DATETIME,
       .mysql_type = MYSQL_FIELD_TYPE_DATE,
       .data.datetime_value = {
         .year = 2026u, .month = 9u, .day = 28u, .wire_length = 4u}},
      {.kind = MYSQL_BINARY_VALUE_DATETIME,
       .mysql_type = MYSQL_FIELD_TYPE_DATETIME,
       .data.datetime_value = {
         .year = 2026u, .month = 9u, .day = 28u,
         .hour = 18u, .minute = 29u, .second = 7u,
         .microsecond = 123456u, .wire_length = 11u}},
      {.kind = MYSQL_BINARY_VALUE_DATETIME,
       .mysql_type = MYSQL_FIELD_TYPE_TIMESTAMP,
       .data.datetime_value = {
         .year = 2026u, .month = 9u, .day = 28u,
         .hour = 18u, .minute = 29u, .second = 7u,
         .wire_length = 7u}},
      {.kind = MYSQL_BINARY_VALUE_TIME,
       .mysql_type = MYSQL_FIELD_TYPE_TIME,
       .data.time_value = {
         .negative = true, .days = 2u, .hour = 3u,
         .minute = 4u, .second = 5u,
         .microsecond = 6u, .wire_length = 12u}}
    };
    static const char *expected[] = {
      "2026-09-28",
      "2026-09-28 18:29:07.123456",
      "2026-09-28 18:29:07",
      "-51:04:05.000006"
    };
    mysql_row_reader_state state;
    cserde_reader reader = {0};
    cserde_token token;
    size_t i;

    check_equal(mysql_row_reader_init(
                    &state, columns, values, 4u, &reader),
                CSERDE_OK);
    next_kind(&reader, CSERDE_MAP_BEGIN, &token);

    for (i = 0u; i < 4u; ++i) {
      next_kind(&reader, CSERDE_STRING, &token);
      next_kind(&reader, CSERDE_STRING, &token);
      check_equal(token.value.slice.size, strlen(expected[i]));
      check_equal(memcmp(token.value.slice.data, expected[i],
                         strlen(expected[i])), 0);
      check_equal(token.value.slice.lifetime, CSERDE_VIEW_TRANSIENT);
    }
  }

  it("formats zero temporal values canonically") {
    mysql_column_definition_t columns[3] = {
      test_column("d", MYSQL_FIELD_TYPE_DATE, 63u),
      test_column("dt", MYSQL_FIELD_TYPE_DATETIME, 63u),
      test_column("tm", MYSQL_FIELD_TYPE_TIME, 63u)
    };
    mysql_binary_value_t values[3] = {
      {.kind = MYSQL_BINARY_VALUE_DATETIME,
       .mysql_type = MYSQL_FIELD_TYPE_DATE,
       .data.datetime_value = {.wire_length = 0u}},
      {.kind = MYSQL_BINARY_VALUE_DATETIME,
       .mysql_type = MYSQL_FIELD_TYPE_DATETIME,
       .data.datetime_value = {.wire_length = 0u}},
      {.kind = MYSQL_BINARY_VALUE_TIME,
       .mysql_type = MYSQL_FIELD_TYPE_TIME,
       .data.time_value = {.wire_length = 0u}}
    };
    static const char *expected[] = {
      "0000-00-00",
      "0000-00-00 00:00:00",
      "00:00:00"
    };
    mysql_row_reader_state state;
    cserde_reader reader = {0};
    cserde_token token;
    size_t i;

    check_equal(mysql_row_reader_init(
                    &state, columns, values, 3u, &reader),
                CSERDE_OK);
    next_kind(&reader, CSERDE_MAP_BEGIN, &token);
    for (i = 0u; i < 3u; ++i) {
      next_kind(&reader, CSERDE_STRING, &token);
      next_kind(&reader, CSERDE_STRING, &token);
      check_equal(token.value.slice.size, strlen(expected[i]));
      check_equal(memcmp(token.value.slice.data, expected[i],
                         strlen(expected[i])), 0);
    }
  }

  it("rejects unsupported bytes and invalid initialization") {
    static const uint8_t value_bytes[] = {1u};
    mysql_column_definition_t column =
        test_column("x", UINT8_C(0x11), 63u);
    mysql_binary_value_t value = {
      .kind = MYSQL_BINARY_VALUE_BYTES,
      .mysql_type = UINT8_C(0x11),
      .data.bytes = {value_bytes, sizeof(value_bytes), false}
    };
    mysql_row_reader_state state;
    cserde_reader reader = {0};
    cserde_token token;

    check_equal(mysql_row_reader_init(
                    NULL, &column, &value, 1u, &reader),
                CSERDE_INVALID_ARGUMENT);
    check_equal(mysql_row_reader_init(
                    &state, NULL, &value, 1u, &reader),
                CSERDE_INVALID_ARGUMENT);

    check_equal(mysql_row_reader_init(
                    &state, &column, &value, 1u, &reader),
                CSERDE_OK);
    next_kind(&reader, CSERDE_MAP_BEGIN, &token);
    next_kind(&reader, CSERDE_STRING, &token);
    check_equal(cserde_reader_next(&reader, &token), CSERDE_UNSUPPORTED);
  }
}
