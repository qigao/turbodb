#include "query.h"

#include <tinytest.h>

#include <string.h>

spec("mysql COM_QUERY control packet") {
  (void)ttest_config__;

  it("encodes command byte plus raw SQL") {
    static const uint8_t sql[] = "START TRANSACTION";
    uint8_t out[64] = {0};
    size_t size = 0u;

    check_equal(mysql_wire_build_query(
                    sql, sizeof(sql) - 1u,
                    out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_OK);
    check_equal(size, sizeof(sql));
    check_equal(out[0], MYSQL_COM_QUERY);
    check_equal(memcmp(out + 1u, sql, sizeof(sql) - 1u), 0);
  }

  it("rejects empty input and bounded output overflow") {
    static const uint8_t sql[] = "ROLLBACK";
    uint8_t out[8] = {0};
    size_t size = 99u;

    check_equal(mysql_wire_build_query(
                    NULL, 0u, out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_INVALID);
    check_equal(size, (size_t)0u);

    check_equal(mysql_wire_build_query(
                    sql, sizeof(sql) - 1u,
                    out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_LIMIT);
    check_equal(size, (size_t)0u);
  }
}
