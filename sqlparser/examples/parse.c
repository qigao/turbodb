#include <sqlparser/sqlparser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  sqlparser_dialect dialect = SQLPARSER_MYSQL;
  int argument = 1;
  if (argc > argument && strcmp(argv[argument], "--sqlite") == 0) {
    dialect = SQLPARSER_SQLITE;
    ++argument;
  } else if (argc > argument && strcmp(argv[argument], "--mysql") == 0) {
    ++argument;
  }
  if (argc > argument + 1) {
    fprintf(stderr, "usage: sqlparser_example [--mysql|--sqlite] [SQL]\n");
    return EXIT_FAILURE;
  }
  const char *sql = argc > argument ? argv[argument] :
      dialect == SQLPARSER_SQLITE ? "SELECT 'hello'||' sqlite'; PRAGMA cache_size;" :
      "SELECT id, name FROM users WHERE id = ?; SET NAMES utf8mb4;";
  sqlparser_document *document = NULL;
  sqlparser_error error;
  sqlparser_status status = sqlparser_parse_dialect(sql, strlen(sql), dialect, NULL, &document, &error);
  if (status != SQLPARSER_OK) {
    fprintf(stderr, "SQL error at byte %zu: %s\n", error.offset, error.message);
    return EXIT_FAILURE;
  }
  printf("dialect=%s statements=%zu\n", dialect == SQLPARSER_SQLITE ? "sqlite" : "mysql",
         sqlparser_statements(document).count);
  for (sqlparser_id id = sqlparser_statements(document).first; id != SQLPARSER_NONE;) {
    const sqlparser_node *node = sqlparser_get_node(document, id);
    printf("kind=%d offset=%zu length=%zu\n", (int)node->kind,
           node->span.offset, node->span.length);
    id = node->next;
  }
  sqlparser_document_destroy(document);
  return EXIT_SUCCESS;
}
