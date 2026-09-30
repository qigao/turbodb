#include "parameters.h"

#include <tinytest.h>

#include <stdint.h>
#include <string.h>

static mysql_parameter_lower_options_t test_options(void) {
  mysql_parameter_lower_options_t options = {
      .ansi_quotes = false,
      .no_backslash_escapes = false,
      .max_parameters = 64u};
  return options;
}

static void lower_ok(
    const char *input, const mysql_parameter_lower_options_t *options,
    const char *expected, const uint32_t *expected_order,
    size_t expected_count, mysql_parameter_style_t expected_style) {
  uint8_t output[512] = {0};
  uint32_t order[64] = {0};
  size_t output_size = 0u;
  mysql_parameter_lower_result_t result;

  check_equal(mysql_parameter_lower(
                  (const uint8_t *)input, strlen(input), options,
                  output, sizeof(output), &output_size,
                  order, sizeof(order) / sizeof(order[0]), &result),
              MYSQL_WIRE_STATUS_OK);
  check_equal(output_size, strlen(expected));
  check_equal(memcmp(output, expected, output_size), 0);
  check_equal(result.style, expected_style);
  check_equal(result.bind_count, expected_count);
  if (expected_count != 0u)
    check_equal(memcmp(order, expected_order,
                       expected_count * sizeof(order[0])), 0);
}

spec("mysql raw SQL parameter lowering") {
  (void)ttest_config__;

  it("lowers portable repeated and out-of-order indices") {
    const uint32_t expected[] = {2u, 1u, 2u};
    mysql_parameter_lower_options_t options = test_options();
    lower_ok("a=?2 OR b=?1 OR c=?2", &options,
             "a=? OR b=? OR c=?", expected, 3u,
             MYSQL_PARAMETER_STYLE_PORTABLE);
  }

  it("preserves native positional markers and records sequential binding") {
    const uint32_t expected[] = {1u, 2u, 3u};
    mysql_parameter_lower_options_t options = test_options();
    lower_ok("INSERT INTO t VALUES (?, ?, ?)", &options,
             "INSERT INTO t VALUES (?, ?, ?)", expected, 3u,
             MYSQL_PARAMETER_STYLE_NATIVE);
  }

  it("ignores markers in strings identifiers and ordinary comments") {
    const uint32_t expected[] = {2u, 1u};
    mysql_parameter_lower_options_t options = test_options();
    lower_ok(
        "SELECT '?2', \"?3\", \x60?4\x60 FROM t /* ?5 */ "
        "WHERE /*!80000 a=?2 */ b=?1 -- ?6\n# ?7\n",
        &options,
        "SELECT '?2', \"?3\", \x60?4\x60 FROM t /* ?5 */ "
        "WHERE /*!80000 a=? */ b=? -- ?6\n# ?7\n",
        expected, 2u, MYSQL_PARAMETER_STYLE_PORTABLE);
  }

  it("treats MySQL executable comments as SQL rather than dead comments") {
    const uint32_t expected[] = {3u};
    mysql_parameter_lower_options_t options = test_options();
    lower_ok("SELECT 1 /*!80000 + ?3 */", &options,
             "SELECT 1 /*!80000 + ? */", expected, 1u,
             MYSQL_PARAMETER_STYLE_PORTABLE);
  }

  it("supports doubled quote escapes without seeing embedded markers") {
    const uint32_t expected[] = {1u};
    mysql_parameter_lower_options_t options = test_options();
    lower_ok("SELECT 'it''s ?9', \"a\"\"?8\", \x60x\x60\x60?7\x60, ?1",
             &options,
             "SELECT 'it''s ?9', \"a\"\"?8\", \x60x\x60\x60?7\x60, ?",
             expected, 1u, MYSQL_PARAMETER_STYLE_PORTABLE);
  }

  it("honors backslash escaping unless NO_BACKSLASH_ESCAPES is active") {
    const uint32_t expected[] = {1u};
    mysql_parameter_lower_options_t options = test_options();
    lower_ok("SELECT 'a\\'b', ?1", &options,
             "SELECT 'a\\'b', ?", expected, 1u,
             MYSQL_PARAMETER_STYLE_PORTABLE);

    options.no_backslash_escapes = true;
    {
      uint8_t output[128] = {0};
      uint32_t order[8] = {0};
      size_t output_size = 0u;
      mysql_parameter_lower_result_t result;
      check_equal(mysql_parameter_lower(
                      (const uint8_t *)"SELECT 'a\\'b', ?1",
                      strlen("SELECT 'a\\'b', ?1"), &options,
                      output, sizeof(output), &output_size,
                      order, 8u, &result),
                  MYSQL_WIRE_STATUS_INVALID);
    }
  }

  it("accepts double-quoted identifiers under ANSI_QUOTES without markers") {
    const uint32_t expected[] = {1u};
    mysql_parameter_lower_options_t options = test_options();
    options.ansi_quotes = true;
    lower_ok("SELECT \"?9\" FROM t WHERE id=?1", &options,
             "SELECT \"?9\" FROM t WHERE id=?", expected, 1u,
             MYSQL_PARAMETER_STYLE_PORTABLE);
  }

  it("fails closed when native and portable marker styles are mixed") {
    mysql_parameter_lower_options_t options = test_options();
    uint8_t output[64] = {0};
    uint32_t order[8] = {0};
    size_t output_size = 0u;
    mysql_parameter_lower_result_t result;

    check_equal(mysql_parameter_lower(
                    (const uint8_t *)"a=?2 AND b=?",
                    strlen("a=?2 AND b=?"), &options,
                    output, sizeof(output), &output_size,
                    order, 8u, &result),
                MYSQL_WIRE_STATUS_INVALID);
  }

  it("rejects zero over-budget and overflowing portable indices") {
    mysql_parameter_lower_options_t options = test_options();
    const char *bad[] = {
        "SELECT ?0",
        "SELECT ?65",
        "SELECT ?184467440737095516160"};
    size_t i;

    for (i = 0u; i < sizeof(bad) / sizeof(bad[0]); ++i) {
      uint8_t output[128] = {0};
      uint32_t order[8] = {0};
      size_t output_size = 0u;
      mysql_parameter_lower_result_t result;
      check_true(mysql_parameter_lower(
                     (const uint8_t *)bad[i], strlen(bad[i]), &options,
                     output, sizeof(output), &output_size,
                     order, 8u, &result) != MYSQL_WIRE_STATUS_OK);
    }
  }

  it("enforces output and bind-order hard bounds") {
    mysql_parameter_lower_options_t options = test_options();
    uint8_t small_output[4] = {0};
    uint32_t order[1] = {0};
    size_t output_size = 0u;
    mysql_parameter_lower_result_t result;

    check_equal(mysql_parameter_lower(
                    (const uint8_t *)"SELECT ?1",
                    strlen("SELECT ?1"), &options,
                    small_output, sizeof(small_output), &output_size,
                    order, 1u, &result),
                MYSQL_WIRE_STATUS_LIMIT);

    {
      uint8_t output[64] = {0};
      check_equal(mysql_parameter_lower(
                      (const uint8_t *)"?1,?2", strlen("?1,?2"),
                      &options, output, sizeof(output), &output_size,
                      order, 1u, &result),
                  MYSQL_WIRE_STATUS_LIMIT);
    }
  }

  it("accepts SQL with no parameters and reports NONE style") {
    mysql_parameter_lower_options_t options = test_options();
    lower_ok("SELECT 42", &options, "SELECT 42", NULL, 0u,
             MYSQL_PARAMETER_STYLE_NONE);
  }

  it("rejects implicit-commit and transaction-control raw SQL in managed transactions") {
    static const char *const rejected[] = {
        "CREATE TABLE t(id INT)",
        "  /* comment */ ALTER TABLE t ADD c INT",
        "-- leading comment\nDROP TABLE t",
        "# comment\nTRUNCATE TABLE t",
        "RENAME TABLE a TO b",
        "GRANT SELECT ON *.* TO u",
        "LOCK TABLES t READ",
        "START TRANSACTION",
        "COMMIT",
        "ROLLBACK",
        "SAVEPOINT s",
        "SET autocommit=1",
        "CACHE INDEX t IN cache",
        "CHECK TABLE t",
        "LOAD DATA INFILE 'x' INTO TABLE t",
        "CHANGE REPLICATION SOURCE TO SOURCE_HOST='x'",
        "STOP REPLICA",
        "/*!80000 CREATE TABLE t(id INT) */"};
    static const char *const allowed[] = {
        "SELECT * FROM t",
        "INSERT INTO t VALUES (1)",
        "UPDATE t SET a=1",
        "DELETE FROM t",
        "WITH cte AS (SELECT 1) SELECT * FROM cte"};
    size_t i;

    for (i = 0u; i < sizeof(rejected) / sizeof(rejected[0]); ++i)
      check_true(mysql_sql_reject_managed_transaction(
          (const uint8_t *)rejected[i], strlen(rejected[i])));
    for (i = 0u; i < sizeof(allowed) / sizeof(allowed[0]); ++i)
      check_false(mysql_sql_reject_managed_transaction(
          (const uint8_t *)allowed[i], strlen(allowed[i])));
  }
}
