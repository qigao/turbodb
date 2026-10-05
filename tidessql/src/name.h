#ifndef ORM_TIDESDB_SQL_NAME_H
#define ORM_TIDESDB_SQL_NAME_H
#include "schema.h"
#include <sqlparser/sqlparser.h>

/* Private ASCII identifier profile shared by DDL and SELECT. No allocation.
 * On failure reason points to a static diagnostic; text may be consumed. */
turbodb_status_t orm_sql_name_validate(vstr name, const char **reason);
turbodb_status_t orm_sql_name_part(vstr *text, vstr *out, const char **reason);
/* Borrow one unqualified AST identifier, including supported backtick quoting.
 * Output borrows document until destruction; unchanged on failure. */
turbodb_status_t orm_sql_name_node(const sqlparser_document *document, sqlparser_id id,
    vstr *out, const char **reason);
void orm_sql_name_space(vstr *text);
#endif
