#include <parameters.h>
#include <tinytest.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

spec("MySQL parameter lowering boundaries") {
  (void)ttest_config__;
  static void *pages;
  static size_t page_size;
  after_each() {
    if (pages != NULL) {
#ifdef _WIN32
      VirtualFree(pages, 0u, MEM_RELEASE);
#else
      munmap(pages, page_size * 2u);
#endif
      pages = NULL;
    }
  }

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

  it("recognizes lexical transitions at every vector lane and tail") {
    const char *fragments[] = {
        "?2", "'?9' ?2", "\"?9\" ?2", "`?9` ?2", "-- ?9\n?2",
        "# ?9\r?2", "/* ?9 */ ?2", "/*! ?2 */", "'it''s \\' ?9' ?2"};
    const char *expected[] = {
        "?", "'?9' ?", "\"?9\" ?", "`?9` ?", "-- ?9\n?",
        "# ?9\r?", "/* ?9 */ ?", "/*! ? */", "'it''s \\' ?9' ?"};
    mysql_parameter_lower_options_t options = {false, false, 2u};
    for (size_t lane = 0u; lane < 32u; ++lane) {
      for (size_t kind = 0u; kind < sizeof(fragments) / sizeof(fragments[0]); ++kind) {
        uint8_t sql[128], output[129], wanted[128];
        uint32_t order[2] = {0};
        mysql_parameter_lower_result_t result;
        size_t used = 0u;
        const size_t prefix = 16u + lane;
        const size_t input_size = prefix + strlen(fragments[kind]) + 17u;
        const size_t expected_size = prefix + strlen(expected[kind]) + 17u;
        memset(sql, 'x', sizeof(sql));
        memset(wanted, 'x', sizeof(wanted));
        memset(output, 0xa5, sizeof(output));
        memcpy(sql + prefix, fragments[kind], strlen(fragments[kind]));
        memcpy(wanted + prefix, expected[kind], strlen(expected[kind]));
        check_equal(mysql_parameter_lower(sql, input_size, &options,
                        output, expected_size, &used, order, 2u, &result),
                    MYSQL_WIRE_STATUS_OK);
        check_equal(used, expected_size);
        check_equal(output, wanted, used);
        check_equal(output[used], (uint8_t)0xa5);
        check_equal(result.bind_count, 1u);
        check_equal(order[0], (uint32_t)2u);
      }
    }
  }

  it("preserves ordinary binary bytes including NUL and SIMD lookup candidates") {
    mysql_parameter_lower_options_t options = {false, false, 1u};
    for (unsigned byte = 0u; byte <= 255u; ++byte) {
      if (byte == '\'' || byte == '"' || byte == '`' || byte == '#' ||
          byte == '-' || byte == '/' || byte == '*' || byte == '?') continue;
      uint8_t sql[66], output[65];
      uint32_t order[1];
      size_t used;
      mysql_parameter_lower_result_t result;
      memset(sql, (int)byte, sizeof(sql));
      sql[64] = '?';
      sql[65] = '1';
      check_equal(mysql_parameter_lower(sql, sizeof(sql), &options,
                      output, sizeof(output), &used, order, 1u, &result),
                  MYSQL_WIRE_STATUS_OK);
      check_equal(used, sizeof(output));
      check_equal(output, sql, sizeof(output));
      check_equal(result.bind_count, 1u);
      check_equal(order[0], (uint32_t)1u);
    }
  }

  it("retains in-place lowering when a removed index precedes long spans") {
    static const uint8_t expected[] =
        "? abcdefghijklmnopqrstuvwxyz 'long literal ?9 with ordinary text' "
        "/* long comment ?8 with ordinary text */ ?";
    uint8_t sql[] =
        "?2 abcdefghijklmnopqrstuvwxyz 'long literal ?9 with ordinary text' "
        "/* long comment ?8 with ordinary text */ ?1";
    mysql_parameter_lower_options_t options = {false, false, 2u};
    uint32_t order[2];
    size_t used;
    mysql_parameter_lower_result_t result;
    check_equal(mysql_parameter_lower(sql, sizeof(sql) - 1u, &options,
                    sql, sizeof(sql), &used, order, 2u, &result),
                MYSQL_WIRE_STATUS_OK);
    check_equal(used, sizeof(expected) - 1u);
    check_equal(sql, expected, used);
    check_equal(order[0], (uint32_t)2u);
    check_equal(order[1], (uint32_t)1u);
  }

  it("keeps every output capacity boundary intact during span copies") {
    static const uint8_t sql[] =
        "SELECT abcdefghijklmnopqrstuvwxyz, 'long literal', ?1";
    mysql_parameter_lower_options_t options = {false, false, 1u};
    for (size_t capacity = 0u; capacity < sizeof(sql) - 2u; ++capacity) {
      uint8_t output[sizeof(sql)];
      uint32_t order[1];
      size_t used = sizeof(output);
      mysql_parameter_lower_result_t result;
      memset(output, 0xa5, sizeof(output));
      check_equal(mysql_parameter_lower(sql, sizeof(sql) - 1u, &options,
                      output, capacity, &used, order, 1u, &result),
                  MYSQL_WIRE_STATUS_LIMIT);
      check_equal(used, 0u);
      for (size_t guard = capacity; guard < sizeof(output); ++guard)
        check_equal(output[guard], (uint8_t)0xa5);
    }
  }

  it("never reads past an unaligned counted input ending at a guard page") {
#ifdef _WIN32
    SYSTEM_INFO system_info;
    DWORD old_protection;
    GetSystemInfo(&system_info);
    page_size = system_info.dwPageSize;
    pages = VirtualAlloc(NULL, page_size * 2u, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check_not_null(pages);
    check_true(VirtualProtect((uint8_t *)pages + page_size, page_size,
                              PAGE_NOACCESS, &old_protection) != 0);
#else
    const long system_page_size = sysconf(_SC_PAGESIZE);
    check_greater(system_page_size, 0L);
    page_size = (size_t)system_page_size;
    void *mapping = mmap(NULL, page_size * 2u, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check_true(mapping != MAP_FAILED);
    pages = mapping;
    check_equal(mprotect((uint8_t *)pages + page_size, page_size, PROT_NONE), 0);
#endif
    mysql_parameter_lower_options_t options = {false, false, 1u};
    for (size_t length = 1u; length <= 96u; ++length) {
      uint8_t *sql = (uint8_t *)pages + page_size - length;
      uint8_t output[97];
      uint32_t order[1];
      size_t used;
      mysql_parameter_lower_result_t result;
      memset(sql, 'x', length);
      if (length >= 2u) memcpy(sql + length - 2u, "?1", 2u);
      check_equal(mysql_parameter_lower(sql, length, &options,
                      output, sizeof(output), &used, order, 1u, &result),
                  MYSQL_WIRE_STATUS_OK);
      check_equal(used, length >= 2u ? length - 1u : length);
      check_equal(output, sql, used);
      check_equal(result.bind_count, length >= 2u ? 1u : 0u);
    }
  }
}
