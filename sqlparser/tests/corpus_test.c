#include "corpus_support.h"
#include "corpus_files.h"

static void run_corpus(sqlparser_dialect dialect, const char *report_path) {
  size_t accepted = 0, rejected = 0, limited = 0, bytes = 0, statements = 0;
  const size_t files = sizeof(corpus_files) / sizeof(corpus_files[0]);
  report = tstr_new();
  check_not_null(report);
  append_report("file\tstatus\tbytes\tstatements\toffset\tline\tbyte_column\tmessage\tcontext\n");
  for (size_t i = 0; i < files; ++i) {
    info("corpus: %s", corpus_files[i]);
    read_fixture(SQLPARSER_CORPUS_ROOT "/", corpus_files[i], &input);
    sqlparser_error error = {0};
    const sqlparser_status status = sqlparser_parse_dialect(input.base, input.len, dialect, NULL, &document, &error);
    check_true(status == SQLPARSER_OK || status == SQLPARSER_SYNTAX_ERROR ||
               status == SQLPARSER_LIMIT_EXCEEDED);
    const char *label;
    size_t line = 0, column = 0, batch_statements = 0;
    char context[CONTEXT_CAPACITY] = {0};
    if (status == SQLPARSER_OK) {
      check_not_null(document);
      validate_document();
      batch_statements = sqlparser_statements(document).count;
      statements += batch_statements;
      ++accepted;
      label = "accepted";
    } else {
      check_null(document);
      check_equal(error.status, status);
      check_true(error.offset <= input.len);
      check_true(error.message[0] != '\0');
      error_position(error.offset, &line, &column, context);
      if (status == SQLPARSER_SYNTAX_ERROR) { ++rejected; label = "syntax_error"; }
      else { ++limited; label = "limit_exceeded"; }
    }
    /* Only fixtures with a known expectation are assertions of compatibility. */
    if (strncmp(corpus_files[i], "parse-errors/", sizeof("parse-errors/") - 1) == 0)
      check_equal(status, SQLPARSER_SYNTAX_ERROR);
    if (strcmp(corpus_files[i], "select/basic-select.sql") == 0)
      check_equal(status, SQLPARSER_OK);
    if (dialect == SQLPARSER_SQLITE) {
      if (strcmp(corpus_files[i], "official-suite/check-1.sql") == 0 ||
          strcmp(corpus_files[i], "official-suite/schema5-1.sql") == 0)
        check_equal(status, SQLPARSER_OK);
      if (strcmp(corpus_files[i], "delete/delete-limit.sql") == 0 ||
          strcmp(corpus_files[i], "update/update-limit.sql") == 0 ||
          strcmp(corpus_files[i], "official-suite/wherelimit-1.sql") == 0)
        check_equal(status, sqlparser_sqlite_update_delete_limit_enabled()
            ? SQLPARSER_OK : SQLPARSER_SYNTAX_ERROR);
    }
    char row[REPORT_ROW_CAPACITY];
    const int length = snprintf(row, sizeof(row), "%s\t%s\t%zu\t%zu\t%zu\t%zu\t%zu\t%s\t%s\n",
      corpus_files[i], label, input.len, batch_statements, error.offset,
      line, column, error.message, context);
    check_true(length >= 0 && (size_t)length < sizeof(row));
    append_report(row);
    bytes += input.len;
    sqlparser_document_destroy(document); document = NULL;
    salts_fs_buf_free(&input);
  }
  salts_fs_buf_t output = {report, tstr_len(report)};
  check_equal(salts_fs_write_file(report_path, &output), 0);
  printf("CORPUS dialect=%s files=%zu bytes=%zu accepted=%zu syntax_errors=%zu limits=%zu accepted_statements=%zu\n",
    dialect == SQLPARSER_SQLITE ? "sqlite" : "mysql", files, bytes, accepted, rejected, limited, statements);
  printf("CORPUS report=%s\n", report_path);
  if (dialect == SQLPARSER_SQLITE)
    printf("CORPUS sqlite_update_delete_limit=%d\n", (int)sqlparser_sqlite_update_delete_limit_enabled());
  check_equal(accepted + rejected + limited, files);
}

spec("SQL file corpus compatibility and result integrity") {
  after_each() { clear_corpus(); }

  it("reports every file using the MySQL dialect") {
    run_corpus(SQLPARSER_MYSQL, SQLPARSER_CORPUS_REPORT);
  }
  it("reports every file using the SQLite dialect") {
    run_corpus(SQLPARSER_SQLITE, SQLPARSER_SQLITE_CORPUS_REPORT);
  }
}
