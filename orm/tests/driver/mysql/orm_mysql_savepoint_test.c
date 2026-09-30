#include "transaction_control.h"

#include <tinytest.h>

#include <string.h>

spec("mysql savepoint control builder") {
  (void)ttest_config__;

  it("builds quoted SAVEPOINT rollback-to and release statements") {
    static const uint8_t name[] = "sp_1";
    uint8_t out[96] = {0};
    size_t size = 0u;

    check_equal(mysql_transaction_build_savepoint_control(
                    MYSQL_SAVEPOINT_CREATE,
                    name, sizeof(name) - 1u,
                    out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_OK);
    check_equal(size, strlen("SAVEPOINT `sp_1`"));
    check_equal(memcmp(out, "SAVEPOINT `sp_1`", size), 0);

    check_equal(mysql_transaction_build_savepoint_control(
                    MYSQL_SAVEPOINT_ROLLBACK_TO,
                    name, sizeof(name) - 1u,
                    out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_OK);
    check_equal(size, strlen("ROLLBACK TO SAVEPOINT `sp_1`"));
    check_equal(memcmp(out, "ROLLBACK TO SAVEPOINT `sp_1`", size), 0);

    check_equal(mysql_transaction_build_savepoint_control(
                    MYSQL_SAVEPOINT_RELEASE,
                    name, sizeof(name) - 1u,
                    out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_OK);
    check_equal(size, strlen("RELEASE SAVEPOINT `sp_1`"));
    check_equal(memcmp(out, "RELEASE SAVEPOINT `sp_1`", size), 0);
  }

  it("rejects injection characters spaces and leading digits") {
    static const uint8_t leading_digit[] = "1bad";
    static const uint8_t semicolon[] = "sp;DROP";
    static const uint8_t space[] = "two words";
    static const uint8_t backtick[] = { 's', 'p', 0x60, 'x' };
    uint8_t out[96] = {0};
    size_t size = 0u;

    check_equal(mysql_transaction_build_savepoint_control(
                    MYSQL_SAVEPOINT_CREATE,
                    leading_digit, sizeof(leading_digit) - 1u,
                    out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_INVALID);
    check_equal(mysql_transaction_build_savepoint_control(
                    MYSQL_SAVEPOINT_CREATE,
                    semicolon, sizeof(semicolon) - 1u,
                    out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_INVALID);
    check_equal(mysql_transaction_build_savepoint_control(
                    MYSQL_SAVEPOINT_CREATE,
                    space, sizeof(space) - 1u,
                    out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_INVALID);
    check_equal(mysql_transaction_build_savepoint_control(
                    MYSQL_SAVEPOINT_CREATE,
                    backtick, sizeof(backtick),
                    out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_INVALID);
  }

  it("enforces the 64-byte local identifier and output bounds") {
    uint8_t valid[64];
    uint8_t too_long[65];
    uint8_t out[96] = {0};
    size_t size = 0u;

    memset(valid, 'a', sizeof(valid));
    memset(too_long, 'a', sizeof(too_long));

    check_equal(mysql_transaction_build_savepoint_control(
                    MYSQL_SAVEPOINT_CREATE,
                    valid, sizeof(valid),
                    out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_OK);

    check_equal(mysql_transaction_build_savepoint_control(
                    MYSQL_SAVEPOINT_CREATE,
                    too_long, sizeof(too_long),
                    out, sizeof(out), &size),
                MYSQL_WIRE_STATUS_INVALID);

    check_equal(mysql_transaction_build_savepoint_control(
                    MYSQL_SAVEPOINT_ROLLBACK_TO,
                    valid, sizeof(valid),
                    out, 8u, &size),
                MYSQL_WIRE_STATUS_LIMIT);
  }
}
