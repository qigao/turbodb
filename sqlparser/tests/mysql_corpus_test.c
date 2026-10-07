#include "corpus_support.h"

struct mysql_fixture {
  const char *file, *source, *oracle, *mysql_error, *mode, *baseline, *gap;
  size_t first_line, last_line, offset, length;
};
#include "mysql_corpus_files.h"
static cmeta_fs_buf_t upstream;

static void check_provenance(const struct mysql_fixture *fixture) {
  read_fixture(SQLPARSER_MYSQL_CORPUS_ROOT "upstream/", fixture->source, &upstream);
  check_equal(input.len, fixture->length);
  check_true(fixture->offset <= upstream.len);
  check_true(input.len <= upstream.len - fixture->offset);
  check_equal(memcmp(input.base, upstream.base + fixture->offset, input.len), 0);
  size_t first = 1, last = 1;
  for (size_t i=0; i<fixture->offset; ++i)
    if (upstream.base[i]=='\n') ++first;
  last = first;
  for (size_t i=0; i+1<input.len; ++i)
    if (input.base[i]=='\n') ++last;
  check_equal(first, fixture->first_line);
  check_equal(last, fixture->last_line);
  cmeta_fs_buf_free(&upstream);
}

spec("MySQL 8.4 official SQL corpus and documented coverage gaps") {
  after_each() { clear_corpus(); cmeta_fs_buf_free(&upstream); }

  it("retains exact upstream SQL and distinguishes syntax errors from server errors") {
    size_t accepted=0, rejected=0, missing=0, overaccepted=0, regressions=0;
    size_t upstream_syntax_errors=0, upstream_server_errors=0;
    const size_t count = sizeof(fixtures)/sizeof(fixtures[0]);
    report = tstr_new();
    check_not_null(report);
    append_report("file\tmysql_expectation\tmysql_error\tsql_mode\tparser_baseline\tactual\tclassification\tsource\tfirst_line\tlast_line\toffset\tline\tbyte_column\tmessage\tgap\n");
    for (size_t i=0; i<count; ++i) {
      const struct mysql_fixture *fixture = &fixtures[i];
      info("fixture: %s; upstream: %s:%zu", fixture->file, fixture->source, fixture->first_line);
      read_fixture(SQLPARSER_MYSQL_CORPUS_ROOT, fixture->file, &input);
      check_provenance(fixture);
      const bool syntax_error = strcmp(fixture->oracle,"syntax_error")==0;
      const bool server_error = strcmp(fixture->oracle,"server_error")==0;
      check_true(syntax_error || server_error || strcmp(fixture->oracle,"success")==0);
      check_equal(fixture->mysql_error[0]!='\0', syntax_error || server_error);
      upstream_syntax_errors += syntax_error;
      upstream_server_errors += server_error;
      const bool expected_accept = strcmp(fixture->baseline,"accept")==0;
      check_true(expected_accept || strcmp(fixture->baseline,"reject")==0);
      /* A known mismatch remains a coverage gap even when its regression
       * expectation passes. Server errors do not imply invalid syntax. */
      check_equal(fixture->gap[0]!='\0', expected_accept==syntax_error);
      sqlparser_error error = {0};
      sqlparser_status status = sqlparser_parse_dialect(input.base,input.len,
          SQLPARSER_MYSQL,NULL,&document,&error);
      check_true(status==SQLPARSER_OK || status==SQLPARSER_SYNTAX_ERROR);
      const bool actual_accept = status==SQLPARSER_OK;
      size_t line=0,column=0;
      char context[CONTEXT_CAPACITY] = {0};
      if (actual_accept) {
        check_not_null(document);
        validate_document();
        check_equal(sqlparser_statements(document).count,1u);
        ++accepted;
      } else {
        check_null(document);
        check_equal(error.status,status);
        check_true(error.offset<=input.len);
        check_true(error.message[0]!='\0');
        error_position(error.offset,&line,&column,context);
        ++rejected;
      }
      const char *classification = "matches_syntax";
      if (actual_accept && syntax_error) { ++overaccepted; classification="over_accept"; }
      if (!actual_accept && !syntax_error) { ++missing; classification="missing_syntax"; }
      if (actual_accept!=expected_accept) {
        ++regressions;
        printf("MYSQL BASELINE MISMATCH %s expected=%s actual=%s upstream=%s byte=%zu\n",
            fixture->file,fixture->baseline,actual_accept?"accept":"reject",fixture->oracle,error.offset);
      }
      char row[REPORT_ROW_CAPACITY];
      int length = snprintf(row,sizeof(row),"%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%zu\t%zu\t%zu\t%zu\t%zu\t%s\t%s\n",
          fixture->file,fixture->oracle,fixture->mysql_error,fixture->mode,fixture->baseline,
          actual_accept?"accept":"reject",classification,fixture->source,fixture->first_line,
          fixture->last_line,error.offset,line,column,error.message,fixture->gap);
      check_true(length>=0 && (size_t)length<sizeof(row));
      append_report(row);
      sqlparser_document_destroy(document); document=NULL;
      cmeta_fs_buf_free(&input);
    }
    cmeta_fs_buf_t output = {report,tstr_len(report)};
    check_equal(cmeta_fs_write_file(SQLPARSER_MYSQL_CORPUS_REPORT,&output),0);
    printf("MYSQL CORPUS files=%zu accepted=%zu rejected=%zu upstream_syntax_errors=%zu upstream_server_errors=%zu missing_syntax=%zu over_accept=%zu baseline_mismatches=%zu\n",
        count,accepted,rejected,upstream_syntax_errors,upstream_server_errors,missing,overaccepted,regressions);
    printf("MYSQL CORPUS report=%s\n",SQLPARSER_MYSQL_CORPUS_REPORT);
    check_equal(accepted+rejected,count);
    check_equal(regressions,0u);
  }
}
