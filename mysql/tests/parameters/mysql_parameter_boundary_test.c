#include <parameters.h>
#include <tinytest.h>
#include <string.h>

spec("MySQL parameter lowering boundaries") {
  (void)ttest_config__;

  it("fills an exact-size output without writing a trailing terminator") {
    static const uint8_t sql[] = "?2,?1";
    static const uint8_t expected[] = "?,?";
    mysql_parameter_lower_options_t options = {false, false, 2u};
    uint8_t output[sizeof(expected)];
    uint32_t order[2] = {0};
    size_t size = 0u;
    mysql_parameter_lower_result_t result;
    memset(output, 0xa5, sizeof(output));
    check_equal(mysql_parameter_lower(sql, sizeof(sql) - 1u, &options,
                    output, sizeof(expected) - 1u, &size, order, 2u, &result),
                MYSQL_WIRE_STATUS_OK);
    check_equal(size, sizeof(expected) - 1u);
    check_equal(memcmp(output, expected, size), 0);
    check_equal(output[size], (uint8_t)0xa5);
    check_equal(order[0], (uint32_t)2u);
    check_equal(order[1], (uint32_t)1u);
  }

  it("does not publish an output length or overwrite the bind-order guard on overflow") {
    static const uint8_t sql[] = "?,?";
    mysql_parameter_lower_options_t options = {false, false, 2u};
    uint8_t output[sizeof(sql)] = {0};
    uint32_t order[] = {0u, UINT32_MAX};
    size_t size = sizeof(output);
    mysql_parameter_lower_result_t result;
    check_equal(mysql_parameter_lower(sql, sizeof(sql) - 1u, &options,
                    output, sizeof(output), &size, order, 1u, &result),
                MYSQL_WIRE_STATUS_LIMIT);
    check_equal(size, (size_t)0u);
    check_equal(order[1], UINT32_MAX);
  }

  it("rejects every unterminated lexical region without publishing partial SQL") {
    const char *inputs[] = {"SELECT 'open", "SELECT \"open", "SELECT `open",
                            "SELECT /* open"};
    mysql_parameter_lower_options_t options = {false, false, 2u};
    for (size_t i = 0u; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
      uint8_t output[32] = {0};
      uint32_t order[2] = {0};
      size_t size = sizeof(output);
      mysql_parameter_lower_result_t result;
      check_equal(mysql_parameter_lower((const uint8_t *)inputs[i], strlen(inputs[i]),
                      &options, output, sizeof(output), &size, order, 2u, &result),
                  MYSQL_WIRE_STATUS_INVALID);
      check_equal(size, (size_t)0u);
    }
  }

  it("resumes parameter recognition after CRLF comments") {
    static const uint8_t sql[] = "-- ?9\r\n?1 # ?8\r\n?1";
    static const uint8_t expected[] = "-- ?9\r\n? # ?8\r\n?";
    mysql_parameter_lower_options_t options = {false, false, 2u};
    uint8_t output[sizeof(sql)] = {0};
    uint32_t order[2] = {0};
    size_t size = 0u;
    mysql_parameter_lower_result_t result;
    check_equal(mysql_parameter_lower(sql, sizeof(sql) - 1u, &options,
                    output, sizeof(output), &size, order, 2u, &result),
                MYSQL_WIRE_STATUS_OK);
    check_equal(size, sizeof(expected) - 1u);
    check_equal(memcmp(output, expected, size), 0);
    check_equal(result.bind_count, (size_t)2u);
    check_equal(order[0], (uint32_t)1u);
    check_equal(order[1], (uint32_t)1u);
  }
}
