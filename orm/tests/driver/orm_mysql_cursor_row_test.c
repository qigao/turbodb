#include "cursor_row.h"

#include <tinytest.h>

#include <stdint.h>
#include <string.h>

static mysql_column_definition_t test_column(
    uint8_t *name, size_t name_size,
    uint8_t type, uint16_t flags, uint16_t charset) {
  mysql_column_definition_t column;
  memset(&column, 0, sizeof(column));
  column.name.data = name;
  column.name.length = name_size;
  column.type = type;
  column.flags = flags;
  column.character_set = charset;
  return column;
}

static void expect_key(
    cserde_reader *reader, const char *expected) {
  cserde_token token;
  check_equal(cserde_reader_next(reader, &token), CSERDE_OK);
  check_equal(token.kind, CSERDE_STRING);
  check_equal(token.value.slice.size, strlen(expected));
  check_equal(memcmp(token.value.slice.data, expected, strlen(expected)), 0);
}

spec("mysql cursor-owned row storage") {
  (void)ttest_config__;

  it("copies column names and current row bytes away from borrowed packet storage") {
    uint8_t id_name[] = {'i','d'};
    uint8_t text_name[] = {'t','x','t'};
    mysql_column_definition_t columns[2] = {
      test_column(id_name, sizeof(id_name),
                  MYSQL_FIELD_TYPE_LONGLONG, 0u, 63u),
      test_column(text_name, sizeof(text_name),
                  MYSQL_FIELD_TYPE_VAR_STRING, 0u, 255u)
    };
    uint8_t row[] = {
      0x00,0x00,
      0xd6,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
      0x03,'f','o','o'
    };
    mysql_cursor_row_store_t store;
    cserde_reader reader = {0};
    cserde_token token;

    check_equal(mysql_cursor_row_store_init(
                    &store, columns, 2u,
                    2u, 16u, 64u),
                MYSQL_WIRE_STATUS_OK);

    memset(id_name, 'X', sizeof(id_name));
    memset(text_name, 'Y', sizeof(text_name));

    check_equal(mysql_cursor_row_store_load(
                    &store, row, sizeof(row)),
                MYSQL_WIRE_STATUS_OK);

    memset(row, 0xee, sizeof(row));

    check_equal(mysql_cursor_row_store_reader(
                    &store, &reader),
                CSERDE_OK);
    check_equal(cserde_reader_next(&reader, &token), CSERDE_OK);
    check_equal(token.kind, CSERDE_MAP_BEGIN);

    expect_key(&reader, "id");
    check_equal(cserde_reader_next(&reader, &token), CSERDE_OK);
    check_equal(token.kind, CSERDE_SINT);
    check_equal(token.value.sint, INT64_C(-42));

    expect_key(&reader, "txt");
    check_equal(cserde_reader_next(&reader, &token), CSERDE_OK);
    check_equal(token.kind, CSERDE_STRING);
    check_equal(token.value.slice.size, (size_t)3u);
    check_equal(memcmp(token.value.slice.data, "foo", 3u), 0);

    check_equal(cserde_reader_next(&reader, &token), CSERDE_OK);
    check_equal(token.kind, CSERDE_MAP_END);
    check_equal(cserde_reader_next(&reader, &token), CSERDE_DONE);

    mysql_cursor_row_store_destroy(&store);
  }

  it("replaces only the current row on the next bounded load") {
    uint8_t name[] = {'v'};
    mysql_column_definition_t column =
        test_column(name, sizeof(name),
                    MYSQL_FIELD_TYPE_VAR_STRING, 0u, 255u);
    static const uint8_t first[] = {
      0x00,0x00,0x03,'o','n','e'
    };
    static const uint8_t second[] = {
      0x00,0x00,0x03,'t','w','o'
    };
    mysql_cursor_row_store_t store;
    cserde_reader reader = {0};
    cserde_token token;

    check_equal(mysql_cursor_row_store_init(
                    &store, &column, 1u,
                    1u, 8u, 32u),
                MYSQL_WIRE_STATUS_OK);
    check_equal(mysql_cursor_row_store_load(
                    &store, first, sizeof(first)),
                MYSQL_WIRE_STATUS_OK);
    check_equal(mysql_cursor_row_store_load(
                    &store, second, sizeof(second)),
                MYSQL_WIRE_STATUS_OK);

    check_equal(mysql_cursor_row_store_reader(
                    &store, &reader),
                CSERDE_OK);
    check_equal(cserde_reader_next(&reader, &token), CSERDE_OK);
    check_equal(token.kind, CSERDE_MAP_BEGIN);
    expect_key(&reader, "v");
    check_equal(cserde_reader_next(&reader, &token), CSERDE_OK);
    check_equal(token.kind, CSERDE_STRING);
    check_equal(token.value.slice.size, (size_t)3u);
    check_equal(memcmp(token.value.slice.data, "two", 3u), 0);

    mysql_cursor_row_store_destroy(&store);
  }

  it("enforces metadata column and row hard bounds") {
    uint8_t name[] = {'a','b','c'};
    mysql_column_definition_t column =
        test_column(name, sizeof(name),
                    MYSQL_FIELD_TYPE_TINY, 0u, 63u);
    mysql_cursor_row_store_t store;
    static const uint8_t row[] = {0x00,0x00,0x01};

    check_equal(mysql_cursor_row_store_init(
                    &store, &column, 1u,
                    0u, 8u, 8u),
                MYSQL_WIRE_STATUS_INVALID);

    check_equal(mysql_cursor_row_store_init(
                    &store, &column, 1u,
                    1u, 2u, 8u),
                MYSQL_WIRE_STATUS_LIMIT);

    check_equal(mysql_cursor_row_store_init(
                    &store, &column, 1u,
                    1u, 8u, 2u),
                MYSQL_WIRE_STATUS_OK);
    check_equal(mysql_cursor_row_store_load(
                    &store, row, sizeof(row)),
                MYSQL_WIRE_STATUS_LIMIT);
    mysql_cursor_row_store_destroy(&store);
  }

  it("clears failed row decode and refuses a reader without a current row") {
    uint8_t name[] = {'x'};
    mysql_column_definition_t column =
        test_column(name, sizeof(name),
                    MYSQL_FIELD_TYPE_LONGLONG, 0u, 63u);
    mysql_cursor_row_store_t store;
    static const uint8_t truncated[] = {0x00,0x00,0x01};
    cserde_reader reader = {0};

    check_equal(mysql_cursor_row_store_init(
                    &store, &column, 1u,
                    1u, 8u, 32u),
                MYSQL_WIRE_STATUS_OK);
    check_equal(mysql_cursor_row_store_reader(
                    &store, &reader),
                CSERDE_INVALID_STATE);

    check_equal(mysql_cursor_row_store_load(
                    &store, truncated, sizeof(truncated)),
                MYSQL_WIRE_STATUS_NEED_MORE);
    check_equal(mysql_cursor_row_store_reader(
                    &store, &reader),
                CSERDE_INVALID_STATE);

    mysql_cursor_row_store_destroy(&store);
  }

  it("destroys idempotently and zeros the ownership record") {
    mysql_cursor_row_store_t store;
    memset(&store, 0xa5, sizeof(store));
    mysql_cursor_row_store_destroy(&store);
    check_null(store.columns);
    check_null(store.values);
    check_null(store.column_names);
    check_null(store.row_storage);
    check_equal(store.column_count, (size_t)0u);
    check_equal(store.row_capacity, (size_t)0u);
  }
}
