#include <sqlparser/sqlparser.h>
#include <tinytest.h>
#include <string.h>

enum { SQLP_BENCH_SAMPLES = 200, SQLP_BENCH_BATCH = 64 };
static const char query[] =
    "SELECT t.id, count(*) n FROM t JOIN u ON t.id=u.id "
    "WHERE t.id BETWEEN 1 AND 100 GROUP BY t.id ORDER BY n DESC LIMIT 10;";

spec("SQL parser benchmarks") {
  bench("typical query including input ownership and AST destruction") {
    sqlparser_document *document = NULL;
    check_equal(sqlparser_parse(query, sizeof(query)-1u, NULL, &document, NULL), SQLPARSER_OK);
    check_equal(sqlparser_statements(document).count, 1u);
    sqlparser_document_destroy(document);
    size_t failures = 0;
    benchmark_bytes("parse and destroy SELECT", SQLP_BENCH_SAMPLES, sizeof(query)-1u) {
      document = NULL;
      if (sqlparser_parse(query, sizeof(query)-1u, NULL, &document, NULL) != SQLPARSER_OK) ++failures;
      sqlparser_document_destroy(document);
    }
    check_equal(failures, 0u);
  }

  bench("batch close to its configured statement boundary") {
    char sql[(sizeof(query)-1u)*SQLP_BENCH_BATCH];
    for (size_t i = 0; i < SQLP_BENCH_BATCH; ++i)
      memcpy(sql+i*(sizeof(query)-1u), query, sizeof(query)-1u);
    sqlparser_limits limits = sqlparser_default_limits();
    limits.max_statements = SQLP_BENCH_BATCH;
    sqlparser_document *document = NULL;
    check_equal(sqlparser_parse(sql, sizeof(sql), &limits, &document, NULL), SQLPARSER_OK);
    check_equal(sqlparser_statements(document).count, SQLP_BENCH_BATCH);
    sqlparser_document_destroy(document);
    size_t failures = 0;
    benchmark_io("parse and destroy 64 statements", SQLP_BENCH_SAMPLES, SQLP_BENCH_BATCH, sizeof(sql)) {
      document = NULL;
      if (sqlparser_parse(sql, sizeof(sql), &limits, &document, NULL) != SQLPARSER_OK) ++failures;
      sqlparser_document_destroy(document);
    }
    check_equal(failures, 0u);
  }
}
