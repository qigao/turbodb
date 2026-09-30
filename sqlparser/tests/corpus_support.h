#ifndef SQLPARSER_CORPUS_SUPPORT_H
#define SQLPARSER_CORPUS_SUPPORT_H

#include <sqlparser/sqlparser.h>
#include <salts_fs.h>
#include <tstr.h>
#include <tinytest.h>
#include <stdio.h>
#include <string.h>

enum { REPORT_MAX_BYTES = 16 * 1024 * 1024, CONTEXT_CAPACITY = 80,
       REPORT_ROW_CAPACITY = 2048 };
static salts_fs_buf_t input;
static sqlparser_document *document;
static tstr path, report;

static void append_report(const char *row) {
  const size_t length = strlen(row);
  check_true(tstr_len(report) <= REPORT_MAX_BYTES - length);
  tstr updated = tstr_cat_len(report, row, length);
  check_not_null(updated);
  report = updated;
}

static void validate_document(void) {
  const size_t count = sqlparser_node_count(document);
  for (size_t i = 1; i <= count; ++i) {
    const sqlparser_node *node = sqlparser_get_node(document, (sqlparser_id)i);
    check_not_null(node);
    check_true(node->span.offset <= input.len);
    check_true(node->span.length <= input.len - node->span.offset);
    check_true(node->next <= count);
    check_not_null(sqlparser_text(document, node->span));
  }
  const sqlparser_list statements = sqlparser_statements(document);
  sqlparser_id id = statements.first, last = SQLPARSER_NONE;
  for (size_t i = 0; i < statements.count; ++i) {
    const sqlparser_node *node = sqlparser_get_node(document, id);
    check_not_null(node);
    last = id;
    id = node->next;
  }
  check_equal(id, SQLPARSER_NONE);
  check_equal(last, statements.last);
}

/* Positions are one-based byte columns; context is TSV-safe, not normalized SQL. */
static void error_position(size_t offset, size_t *line, size_t *column,
                           char context[CONTEXT_CAPACITY]) {
  *line = *column = 1;
  for (size_t i = 0; i < offset; ++i) {
    if (input.base[i] == '\n') { ++*line; *column = 1; }
    else ++*column;
  }
  size_t length = input.len - offset;
  if (length >= CONTEXT_CAPACITY) length = CONTEXT_CAPACITY - 1;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char ch = (unsigned char)input.base[offset + i];
    context[i] = ch < ' ' || ch == 127 ? ' ' : (char)ch;
  }
  context[length] = '\0';
}

static void clear_corpus(void) {
  sqlparser_document_destroy(document); document = NULL;
  salts_fs_buf_free(&input);
  tstr_free(path); path = NULL;
  tstr_free(report); report = NULL;
}

static void read_fixture(const char *root, const char *file, salts_fs_buf_t *buffer) {
  path = tstr_new_len(root, strlen(root));
  check_not_null(path);
  tstr updated = tstr_cat_len(path, file, strlen(file));
  check_not_null(updated);
  path = updated;
  salts_fs_stat_t stat;
  check_equal(salts_fs_stat(path, &stat), 0);
  check_true(stat.size <= sqlparser_default_limits().max_input_bytes);
  check_equal(salts_fs_read_file(path, buffer), 0);
  tstr_free(path); path = NULL;
}
#endif
