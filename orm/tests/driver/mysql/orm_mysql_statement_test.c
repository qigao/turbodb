#include "statement.h"

#include <tinytest.h>

#include <stdint.h>
#include <string.h>

spec("mysql prepared statement wire") {
  (void)ttest_config__;

  it("builds COM_STMT_PREPARE as command plus raw SQL") {
    static const uint8_t sql[] = "SELECT ? + ?";
    uint8_t out[64] = {0};
    size_t size = 0u;

    check_equal(mysql_wire_build_stmt_prepare(
                    sql, sizeof(sql) - 1u,
                    out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_OK);
    check_equal(size, sizeof(sql));
    check_equal(out[0], MYSQL_COM_STMT_PREPARE);
    check_equal(memcmp(out + 1u, sql, sizeof(sql) - 1u), 0);
  }

  it("decodes COM_STMT_PREPARE_OK without borrowing payload") {
    static const uint8_t payload[] = {
      0x00,
      0x78,0x56,0x34,0x12,
      0x02,0x00,
      0x03,0x00,
      0x00,
      0x05,0x00
    };
    mysql_stmt_prepare_ok_t ok;

    check_equal(mysql_wire_decode_stmt_prepare_ok(
                    payload, sizeof(payload), &ok),
                MYSQL_WIRE_STATUS_OK);
    check_equal(ok.statement_id, UINT32_C(0x12345678));
    check_equal(ok.column_count, UINT16_C(2));
    check_equal(ok.parameter_count, UINT16_C(3));
    check_equal(ok.warning_count, UINT16_C(5));
  }

  it("rejects truncated malformed or extended prepare-ok packets") {
    static const uint8_t valid[] = {
      0x00,1,0,0,0,0,0,0,0,0,0,0
    };
    uint8_t bad[sizeof(valid) + 1u];
    mysql_stmt_prepare_ok_t ok;

    check_equal(mysql_wire_decode_stmt_prepare_ok(
                    valid, sizeof(valid) - 1u, &ok),
                MYSQL_WIRE_STATUS_NEED_MORE);

    memcpy(bad, valid, sizeof(valid));
    bad[9] = 1u;
    check_equal(mysql_wire_decode_stmt_prepare_ok(
                    bad, sizeof(valid), &ok),
                MYSQL_WIRE_STATUS_INVALID);

    memcpy(bad, valid, sizeof(valid));
    bad[sizeof(valid)] = 0u;
    check_equal(mysql_wire_decode_stmt_prepare_ok(
                    bad, sizeof(bad), &ok),
                MYSQL_WIRE_STATUS_INVALID);
  }

  it("builds CLOSE and RESET with opaque little-endian statement ids") {
    uint8_t out[8] = {0};
    size_t size = 0u;
    static const uint8_t close_expected[] = {
      MYSQL_COM_STMT_CLOSE,0x78,0x56,0x34,0x12
    };
    static const uint8_t reset_expected[] = {
      MYSQL_COM_STMT_RESET,0x78,0x56,0x34,0x12
    };

    check_equal(mysql_wire_build_stmt_close(
                    UINT32_C(0x12345678), out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_OK);
    check_equal(size, sizeof(close_expected));
    check_equal(memcmp(out, close_expected, sizeof(close_expected)), 0);

    memset(out, 0, sizeof(out));
    check_equal(mysql_wire_build_stmt_reset(
                    UINT32_C(0x12345678), out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_OK);
    check_equal(size, sizeof(reset_expected));
    check_equal(memcmp(out, reset_expected, sizeof(reset_expected)), 0);
  }

  it("builds zero-parameter EXECUTE with iteration-count one") {
    uint8_t out[16] = {0};
    size_t size = 0u;
    static const uint8_t expected[] = {
      MYSQL_COM_STMT_EXECUTE,
      0x04,0x03,0x02,0x01,
      0x00,
      0x01,0x00,0x00,0x00
    };

    check_equal(mysql_wire_build_stmt_execute(
                    UINT32_C(0x01020304), NULL, 0u,
                    out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_OK);
    check_equal(size, sizeof(expected));
    check_equal(memcmp(out, expected, sizeof(expected)), 0);
  }

  it("encodes null bitmap types flags and binary parameter values") {
    static const uint8_t text[] = {'f','o','o'};
    static const uint8_t blob[] = {0x00,0x01};
    mysql_stmt_value_t values[7] = {
      {.kind = MYSQL_STMT_VALUE_NULL},
      {.kind = MYSQL_STMT_VALUE_SINT64, .data.sint64_value = -2},
      {.kind = MYSQL_STMT_VALUE_UINT64, .data.uint64_value = UINT64_C(0x1122)},
      {.kind = MYSQL_STMT_VALUE_DOUBLE, .data.double_value = 10.2},
      {.kind = MYSQL_STMT_VALUE_BOOL, .data.bool_value = 1u},
      {.kind = MYSQL_STMT_VALUE_TEXT, .data.bytes = {text, sizeof(text)}},
      {.kind = MYSQL_STMT_VALUE_BLOB, .data.bytes = {blob, sizeof(blob)}}
    };
    uint8_t out[128] = {0};
    size_t size = 0u;
    size_t offset = 0u;
    uint32_t statement_id = 0u;

    check_equal(mysql_wire_build_stmt_execute(
                    UINT32_C(7), values, 7u,
                    out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_OK);

    check_equal(out[offset++], MYSQL_COM_STMT_EXECUTE);
    check_equal(mysql_wire_read_u32_le(
                    out, size, &offset, &statement_id),
                MYSQL_WIRE_STATUS_OK);
    check_equal(statement_id, UINT32_C(7));
    check_equal(out[offset++], UINT8_C(0));
    check_equal(out[offset++], UINT8_C(1));
    check_equal(out[offset++], UINT8_C(0));
    check_equal(out[offset++], UINT8_C(0));
    check_equal(out[offset++], UINT8_C(0));

    check_equal(out[offset++], UINT8_C(0x01));
    check_equal(out[offset++], UINT8_C(1));

    check_equal(out[offset++], MYSQL_TYPE_NULL);
    check_equal(out[offset++], UINT8_C(0));
    check_equal(out[offset++], MYSQL_TYPE_LONGLONG);
    check_equal(out[offset++], UINT8_C(0));
    check_equal(out[offset++], MYSQL_TYPE_LONGLONG);
    check_equal(out[offset++], MYSQL_TYPE_UNSIGNED_FLAG);
    check_equal(out[offset++], MYSQL_TYPE_DOUBLE);
    check_equal(out[offset++], UINT8_C(0));
    check_equal(out[offset++], MYSQL_TYPE_TINY);
    check_equal(out[offset++], UINT8_C(0));
    check_equal(out[offset++], MYSQL_TYPE_VAR_STRING);
    check_equal(out[offset++], UINT8_C(0));
    check_equal(out[offset++], MYSQL_TYPE_BLOB);
    check_equal(out[offset++], UINT8_C(0));

    {
      static const uint8_t expected_values[] = {
        0xfe,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0x22,0x11,0x00,0x00,0x00,0x00,0x00,0x00,
        0x66,0x66,0x66,0x66,0x66,0x66,0x24,0x40,
        0x01,
        0x03,'f','o','o',
        0x02,0x00,0x01
      };
      check_equal(size - offset, sizeof(expected_values));
      check_equal(memcmp(out + offset,
                         expected_values,
                         sizeof(expected_values)), 0);
    }
  }

  it("uses execute null-bitmap offset zero across byte boundaries") {
    mysql_stmt_value_t values[9] = {0};
    uint8_t out[128] = {0};
    size_t size = 0u;
    size_t i;

    for (i = 0u; i < 9u; ++i) {
      values[i].kind = MYSQL_STMT_VALUE_BOOL;
      values[i].data.bool_value = 0u;
    }
    values[8].kind = MYSQL_STMT_VALUE_NULL;

    check_equal(mysql_wire_build_stmt_execute(
                    UINT32_C(1), values, 9u,
                    out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_OK);
    check_true(size > 12u);
    check_equal(out[10], UINT8_C(0x00));
    check_equal(out[11], UINT8_C(0x01));
  }

  it("fails closed for invalid values parameter bounds and output capacity") {
    mysql_stmt_value_t value = {
      .kind = MYSQL_STMT_VALUE_BOOL,
      .data.bool_value = 2u
    };
    uint8_t out[32] = {0};
    size_t size = 0u;

    check_equal(mysql_wire_build_stmt_execute(
                    UINT32_C(1), &value, 1u,
                    out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_INVALID);

    value.kind = MYSQL_STMT_VALUE_TEXT;
    value.data.bytes.data = NULL;
    value.data.bytes.size = 1u;
    check_equal(mysql_wire_build_stmt_execute(
                    UINT32_C(1), &value, 1u,
                    out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_INVALID);

    check_equal(mysql_wire_build_stmt_execute(
                    UINT32_C(1), &value, (size_t)UINT16_MAX + 1u,
                    out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_LIMIT);

    value.kind = MYSQL_STMT_VALUE_UINT64;
    value.data.uint64_value = UINT64_MAX;
    check_equal(mysql_wire_build_stmt_execute(
                    UINT32_C(1), &value, 1u,
                    out, 4u, &size),
                MYSQL_WIRE_STATUS_LIMIT);
  }
}
