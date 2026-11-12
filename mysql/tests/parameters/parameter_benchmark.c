#include <parameters.h>
#include <core/tstr.h>
#include <tinytest.h>
#include <string.h>

enum { SAMPLES = 200, BATCH = 1024 };

spec("MySQL parameter lowering benchmarks") {
  static tstr sql;
  static tstr output;

  after_each() {
    tstr_freep(&sql);
    tstr_freep(&output);
  }

  const char *queries[] = {
      "SELECT id, name FROM users WHERE id=?1 AND name=?2 ORDER BY id LIMIT 10",
      "INSERT INTO t VALUES (?1,?2,?1,?2,?1,?2,?1,?2,?1,?2,?1,?2)",
      "SELECT 'it''s \\' text ?9', `name?8` FROM t /* ?7 */ WHERE id=?1 -- ?6\n"};
  for (size_t kind = 0u; kind < sizeof(queries) / sizeof(queries[0]); ++kind) {
    bench("typical SQL workload %zu", kind) {
      sql = tstr_dup(queries[kind]);
      check_not_null(sql);
      output = tstr_new_len(NULL, tstr_len(sql));
      check_not_null(output);
      uint32_t order[16];
      size_t used;
      mysql_parameter_lower_result_t result;
      mysql_parameter_lower_options_t options = {false, false, 16u};
      check_equal(mysql_parameter_lower((const uint8_t *)sql, tstr_len(sql),
                      &options, (uint8_t *)output, tstr_len(output), &used,
                      order, 16u, &result), MYSQL_WIRE_STATUS_OK);
      check_equal(result.bind_count, kind == 0u ? 2u : kind == 1u ? 12u : 1u);
      size_t failures = 0u;
      benchmark_io("typical parameter lower", SAMPLES, BATCH, BATCH * tstr_len(sql)) {
        for (size_t i = 0u; i < BATCH; ++i) {
          if (mysql_parameter_lower((const uint8_t *)sql, tstr_len(sql),
                  &options, (uint8_t *)output, tstr_len(output), &used,
                  order, 16u, &result) != MYSQL_WIRE_STATUS_OK) ++failures;
        }
      }
      check_equal(failures, 0u);
    }
  }

  for (size_t length = 16u; length <= 4096u; length *= 16u) {
    const char *prefixes[] = {"SELECT ", "SELECT '", "SELECT /*"};
    const char *suffixes[] = {", ?2, ?1", "', ?2, ?1", "*/ ?2, ?1"};
    for (size_t kind = 0u; kind < 3u; ++kind) {
      bench("lower %zu bytes of %s", length, prefixes[kind]) {
        const size_t prefix_len = strlen(prefixes[kind]);
        const size_t suffix_len = strlen(suffixes[kind]);
        sql = tstr_new_len(NULL, prefix_len + length + suffix_len);
        check_not_null(sql);
        memcpy(sql, prefixes[kind], prefix_len);
        memset(sql + prefix_len, 'x', length);
        memcpy(sql + prefix_len + length, suffixes[kind], suffix_len);
        output = tstr_new_len(NULL, tstr_len(sql));
        check_not_null(output);
        uint32_t order[2];
        size_t used = 0u;
        mysql_parameter_lower_result_t result;
        mysql_parameter_lower_options_t options = {false, false, 2u};
        check_equal(mysql_parameter_lower((const uint8_t *)sql, tstr_len(sql),
                        &options, (uint8_t *)output, tstr_len(output), &used,
                        order, 2u, &result), MYSQL_WIRE_STATUS_OK);
        check_equal(used, tstr_len(sql) - 2u);
        check_equal(result.bind_count, 2u);
        check_equal(order[0], (uint32_t)2u);
        check_equal(order[1], (uint32_t)1u);
        check_equal(output, sql, prefix_len + length);
        size_t failures = 0u;
        benchmark_io("parameter lower", SAMPLES, BATCH, BATCH * tstr_len(sql)) {
          for (size_t i = 0u; i < BATCH; ++i) {
            if (mysql_parameter_lower((const uint8_t *)sql, tstr_len(sql),
                    &options, (uint8_t *)output, tstr_len(output), &used,
                    order, 2u, &result) != MYSQL_WIRE_STATUS_OK) ++failures;
          }
        }
        check_equal(failures, 0u);
      }
    }
  }
}
