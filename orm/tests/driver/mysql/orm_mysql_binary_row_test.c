#include "row.h"

#include <tinytest.h>

#include <stdint.h>
#include <string.h>

static mysql_column_definition_t column(uint8_t type, uint16_t flags) {
  mysql_column_definition_t value;
  memset(&value, 0, sizeof(value));
  value.type = type;
  value.flags = flags;
  return value;
}

spec("mysql ColumnDefinition41") {
  (void)ttest_config__;

  it("decodes borrowed names and fixed metadata") {
    static const uint8_t payload[] = {
      0x03,'d','e','f',
      0x02,'d','b',
      0x01,'t',
      0x01,'t',
      0x02,'i','d',
      0x02,'i','d',
      0x0c,
      0x21,0x00,
      0x14,0x00,0x00,0x00,
      MYSQL_FIELD_TYPE_LONGLONG,
      0x20,0x00,
      0x00,
      0x00,0x00
    };
    mysql_column_definition_t out;

    check_equal(mysql_wire_decode_column_definition41(
                    payload, sizeof(payload), &out),
                MYSQL_WIRE_STATUS_OK);
    check_equal(out.catalog.length, (size_t)3u);
    check_equal(memcmp(out.catalog.data, "def", 3u), 0);
    check_equal(out.schema.length, (size_t)2u);
    check_equal(memcmp(out.schema.data, "db", 2u), 0);
    check_equal(out.name.length, (size_t)2u);
    check_equal(memcmp(out.name.data, "id", 2u), 0);
    check_equal(out.character_set, UINT16_C(0x21));
    check_equal(out.column_length, UINT32_C(20));
    check_equal(out.type, MYSQL_FIELD_TYPE_LONGLONG);
    check_true((out.flags & MYSQL_COLUMN_FLAG_UNSIGNED) != 0u);
    check_equal(out.decimals, UINT8_C(0));
  }

  it("rejects wrong fixed-length marker reserved bytes and trailing data") {
    static const uint8_t valid[] = {
      0x03,'d','e','f',0,0,0,0,0,
      0x0c,0x21,0,1,0,0,0,MYSQL_FIELD_TYPE_TINY,
      0,0,0,0,0
    };
    uint8_t payload[sizeof(valid) + 1u];
    mysql_column_definition_t out;

    memcpy(payload, valid, sizeof(valid));
    payload[9] = 0x0b;
    check_equal(mysql_wire_decode_column_definition41(
                    payload, sizeof(valid), &out),
                MYSQL_WIRE_STATUS_INVALID);

    memcpy(payload, valid, sizeof(valid));
    payload[sizeof(valid) - 1u] = 1u;
    check_equal(mysql_wire_decode_column_definition41(
                    payload, sizeof(valid), &out),
                MYSQL_WIRE_STATUS_INVALID);

    memcpy(payload, valid, sizeof(valid));
    payload[sizeof(valid)] = 0u;
    check_equal(mysql_wire_decode_column_definition41(
                    payload, sizeof(payload), &out),
                MYSQL_WIRE_STATUS_INVALID);
  }
}

spec("mysql binary result rows") {
  (void)ttest_config__;

  it("decodes integer double bytes decimal date and offset-two NULL bitmap") {
    mysql_column_definition_t columns[9] = {
      column(MYSQL_FIELD_TYPE_TINY, 0u),
      column(MYSQL_FIELD_TYPE_SHORT, MYSQL_COLUMN_FLAG_UNSIGNED),
      column(MYSQL_FIELD_TYPE_INT24, 0u),
      column(MYSQL_FIELD_TYPE_LONGLONG, MYSQL_COLUMN_FLAG_UNSIGNED),
      column(MYSQL_FIELD_TYPE_DOUBLE, 0u),
      column(MYSQL_FIELD_TYPE_VAR_STRING, 0u),
      column(MYSQL_FIELD_TYPE_NEWDECIMAL, 0u),
      column(MYSQL_FIELD_TYPE_DATE, 0u),
      column(MYSQL_FIELD_TYPE_BLOB, 0u)
    };
    static const uint8_t payload[] = {
      0x00,
      0x00,0x04,
      0xfe,
      0x34,0x12,
      0xfd,0xff,0xff,0xff,
      0x22,0x11,0,0,0,0,0,0,
      0x66,0x66,0x66,0x66,0x66,0x66,0x24,0x40,
      0x03,'f','o','o',
      0x06,'1','2','3','.','4','5',
      0x04,0xda,0x07,0x0a,0x11
    };
    mysql_binary_value_t values[9];

    check_equal(mysql_wire_decode_binary_row(
                    payload, sizeof(payload),
                    columns, 9u, values, 9u),
                MYSQL_WIRE_STATUS_OK);

    check_equal(values[0].kind, MYSQL_BINARY_VALUE_SINT64);
    check_equal(values[0].data.sint64_value, INT64_C(-2));

    check_equal(values[1].kind, MYSQL_BINARY_VALUE_UINT64);
    check_equal(values[1].data.uint64_value, UINT64_C(0x1234));

    check_equal(values[2].kind, MYSQL_BINARY_VALUE_SINT64);
    check_equal(values[2].data.sint64_value, INT64_C(-3));

    check_equal(values[3].kind, MYSQL_BINARY_VALUE_UINT64);
    check_equal(values[3].data.uint64_value, UINT64_C(0x1122));

    check_equal(values[4].kind, MYSQL_BINARY_VALUE_DOUBLE);
    check_true(values[4].data.double_value > 10.1999999);
    check_true(values[4].data.double_value < 10.2000001);

    check_equal(values[5].kind, MYSQL_BINARY_VALUE_BYTES);
    check_equal(values[5].data.bytes.length, (size_t)3u);
    check_equal(memcmp(values[5].data.bytes.data, "foo", 3u), 0);

    check_equal(values[6].kind, MYSQL_BINARY_VALUE_BYTES);
    check_equal(values[6].data.bytes.length, (size_t)6u);
    check_equal(memcmp(values[6].data.bytes.data, "123.45", 6u), 0);

    check_equal(values[7].kind, MYSQL_BINARY_VALUE_DATETIME);
    check_equal(values[7].data.datetime_value.year, UINT16_C(2010));
    check_equal(values[7].data.datetime_value.month, UINT8_C(10));
    check_equal(values[7].data.datetime_value.day, UINT8_C(17));
    check_equal(values[7].data.datetime_value.wire_length, UINT8_C(4));

    check_equal(values[8].kind, MYSQL_BINARY_VALUE_NULL);
  }

  it("decodes float datetime timestamp and signed TIME without timezone assumptions") {
    mysql_column_definition_t columns[4] = {
      column(MYSQL_FIELD_TYPE_FLOAT, 0u),
      column(MYSQL_FIELD_TYPE_DATETIME, 0u),
      column(MYSQL_FIELD_TYPE_TIMESTAMP, 0u),
      column(MYSQL_FIELD_TYPE_TIME, 0u)
    };
    static const uint8_t payload[] = {
      0x00,0x00,
      0x33,0x33,0x23,0x41,
      0x0b,0xda,0x07,0x0a,0x11,0x13,0x1b,0x1e,0x01,0x00,0x00,0x00,
      0x07,0xda,0x07,0x0a,0x11,0x13,0x1b,0x1e,
      0x0c,0x01,0x78,0x00,0x00,0x00,0x13,0x1b,0x1e,0x01,0x00,0x00,0x00
    };
    mysql_binary_value_t values[4];

    check_equal(mysql_wire_decode_binary_row(
                    payload, sizeof(payload),
                    columns, 4u, values, 4u),
                MYSQL_WIRE_STATUS_OK);

    check_equal(values[0].kind, MYSQL_BINARY_VALUE_DOUBLE);
    check_true(values[0].data.double_value > 10.1999);
    check_true(values[0].data.double_value < 10.2001);

    check_equal(values[1].kind, MYSQL_BINARY_VALUE_DATETIME);
    check_equal(values[1].data.datetime_value.wire_length, UINT8_C(11));
    check_equal(values[1].data.datetime_value.microsecond, UINT32_C(1));

    check_equal(values[2].kind, MYSQL_BINARY_VALUE_DATETIME);
    check_equal(values[2].data.datetime_value.wire_length, UINT8_C(7));
    check_equal(values[2].data.datetime_value.hour, UINT8_C(19));

    check_equal(values[3].kind, MYSQL_BINARY_VALUE_TIME);
    check_true(values[3].data.time_value.negative);
    check_equal(values[3].data.time_value.days, UINT32_C(120));
    check_equal(values[3].data.time_value.hour, UINT8_C(19));
    check_equal(values[3].data.time_value.microsecond, UINT32_C(1));
  }

  it("accepts zero temporal encodings") {
    mysql_column_definition_t columns[3] = {
      column(MYSQL_FIELD_TYPE_DATE, 0u),
      column(MYSQL_FIELD_TYPE_DATETIME, 0u),
      column(MYSQL_FIELD_TYPE_TIME, 0u)
    };
    static const uint8_t payload[] = {
      0x00,0x00,
      0x00,
      0x00,
      0x00
    };
    mysql_binary_value_t values[3];

    check_equal(mysql_wire_decode_binary_row(
                    payload, sizeof(payload),
                    columns, 3u, values, 3u),
                MYSQL_WIRE_STATUS_OK);
    check_equal(values[0].data.datetime_value.wire_length, UINT8_C(0));
    check_equal(values[1].data.datetime_value.wire_length, UINT8_C(0));
    check_equal(values[2].data.time_value.wire_length, UINT8_C(0));
  }

  it("honors the signed LONGLONG minimum without implementation-defined cast") {
    mysql_column_definition_t col =
        column(MYSQL_FIELD_TYPE_LONGLONG, 0u);
    static const uint8_t payload[] = {
      0x00,0x00,
      0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80
    };
    mysql_binary_value_t value;

    check_equal(mysql_wire_decode_binary_row(
                    payload, sizeof(payload), &col, 1u, &value, 1u),
                MYSQL_WIRE_STATUS_OK);
    check_equal(value.kind, MYSQL_BINARY_VALUE_SINT64);
    check_equal(value.data.sint64_value, INT64_MIN);
  }

  it("fails closed on bad headers truncation invalid temporal values and trailing bytes") {
    mysql_column_definition_t col =
        column(MYSQL_FIELD_TYPE_DATETIME, 0u);
    uint8_t payload[16] = {
      0x01,0x00,
      0x07,0xda,0x07,0x0a,0x11,0x13,0x1b,0x1e
    };
    mysql_binary_value_t value;

    check_equal(mysql_wire_decode_binary_row(
                    payload, 10u, &col, 1u, &value, 1u),
                MYSQL_WIRE_STATUS_INVALID);

    payload[0] = 0x00;
    check_equal(mysql_wire_decode_binary_row(
                    payload, 9u, &col, 1u, &value, 1u),
                MYSQL_WIRE_STATUS_NEED_MORE);

    payload[0] = 0x00;
    payload[1] = 0x00;
    payload[2] = 0x07;
    payload[3] = 0xda;
    payload[4] = 0x07;
    payload[5] = 13u;
    payload[6] = 17u;
    payload[7] = 19u;
    payload[8] = 27u;
    payload[9] = 30u;
    check_equal(mysql_wire_decode_binary_row(
                    payload, 10u, &col, 1u, &value, 1u),
                MYSQL_WIRE_STATUS_INVALID);

    payload[5] = 10u;
    payload[10] = 0u;
    check_equal(mysql_wire_decode_binary_row(
                    payload, 11u, &col, 1u, &value, 1u),
                MYSQL_WIRE_STATUS_INVALID);
  }

  it("rejects unsupported types and insufficient value capacity") {
    mysql_column_definition_t col = column(UINT8_C(0x11), 0u);
    static const uint8_t payload[] = {0x00,0x00};
    mysql_binary_value_t value;

    check_equal(mysql_wire_decode_binary_row(
                    payload, sizeof(payload), &col, 1u, &value, 1u),
                MYSQL_WIRE_STATUS_INVALID);

    col.type = MYSQL_FIELD_TYPE_TINY;
    check_equal(mysql_wire_decode_binary_row(
                    payload, sizeof(payload), &col, 1u, &value, 0u),
                MYSQL_WIRE_STATUS_LIMIT);
  }
}
