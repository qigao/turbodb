#ifndef ORM_TIDESDB_SQL_SHOW_H
#define ORM_TIDESDB_SQL_SHOW_H
#include "catalog_store.h"
#include "index_directory.h"
#include "scan.h"
#include "diagnostics.h"
#include "expr.h"
#include <tstr.h>

enum { ORM_SQL_SHOW_COLUMNS = 6, ORM_SQL_SHOW_INDEX_COLUMNS = 15,
       ORM_SQL_SHOW_MAX_COLUMNS = ORM_SQL_SHOW_INDEX_COLUMNS,
       ORM_SQL_SHOW_LABEL_BYTES = ORM_SQL_SELECT_NAME_BYTES + sizeof("Tables_in_") };
/* Private synchronous result source. Zero initialize; keep its address, owner
 * and budget stable through close. schema and row payloads borrow this source.
 * Each next invalidates the previous row. AST/database_name borrow open only.
 * Only one scan may borrow source; close that scan before show_close. */
typedef struct orm_sql_show_source {
  orm_sql_row_source source;
  orm_sql_catalog_store *owner;
  orm_sql_catalog_cursor cursor;
  orm_sql_table_definition definition;
  orm_sql_index_set indexes;
  vec_t keys;
  tstr create_sql;
  orm_sql_table_schema schema;
  orm_sql_schema_column columns[ORM_SQL_SHOW_MAX_COLUMNS];
  orm_sql_type types[ORM_SQL_SHOW_MAX_COLUMNS];
  turbodb_value_t values[ORM_SQL_SHOW_MAX_COLUMNS];
  char label[ORM_SQL_SHOW_LABEL_BYTES];
  char default_text[64];
  size_t metadata_bytes, key_bytes, create_bytes, position, part;
  turbodb_error_t failure;
  const orm_sql_diagnostics *diagnostics;
  orm_sql_session_snapshot session;
  orm_sql_expr pattern;
  turbodb_value_t pattern_value;
  orm_sql_like like;
  uint64_t offset, limit;
  sqlparser_show_kind kind;
  bool full, count, done;
} orm_sql_show_source;

/* One MySQL SHOW [FULL] TABLES, COLUMNS/FIELDS, INDEX/INDEXES/KEYS, CREATE TABLE,
 * WARNINGS [LIMIT ...], COUNT(*) WARNINGS, or SESSION/LOCAL VARIABLES.
 * database_name is an explicit ASCII display label, never a CF selector.
 * TABLES/COLUMNS/VARIABLES accept LIKE. WHERE on those forms and INDEX is bound
 * by runtime against this source's schema; this source supplies its input rows.
 * Database qualifiers remain unsupported;
 * FULL/EXTENDED COLUMNS and EXTENDED INDEX remain unsupported.
 * INDEX returns 15 fields; local Index_type is LSM, unknown Cardinality NULL.
 * Table and index definitions are owned snapshots, charged to the same budget.
 * CREATE TABLE emits normalized complete MySQL DDL, including named inline
 * indexes, replayable by this profile when the target name is absent.
 * Unsupported syntax -> UNSUPPORTED, absent table -> SQL_ERROR; budgets and
 * storage errors propagate. Failure leaves output empty and refunds work.
 * Example: show_open(doc, owner, name, &s), scan_open_source(&s.source,...),
 * scan_next(...), scan_close(...), show_close(&s), catalog_finish(owner,...).
 * Enumeration does not promise alphabetical order. No writes/implicit commit. */
turbodb_status_t orm_tidesdb_sql_show_open(const sqlparser_document *document,
    orm_sql_catalog_store *store, vstr database_name, orm_sql_show_source *out, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_show_open_diagnostics(
    const sqlparser_document *document, orm_sql_catalog_store *store,
    vstr database_name, const orm_sql_diagnostics *diagnostics,
    orm_sql_show_source *out, turbodb_error_t *error);
/* Same source contract, with an immutable session snapshot. VARIABLES requires
 * a valid snapshot, displays exactly the supported session variables, and LIKE
 * accepts an ASCII literal with case-insensitive name matching. GLOBAL remains
 * unsupported; table names use the profile's case-sensitive LIKE matching. */
turbodb_status_t orm_sql_show_open_evaluation(const sqlparser_document *document,
    orm_sql_catalog_store *store, vstr database_name, orm_sql_evaluation evaluation,
    orm_sql_show_source *out, turbodb_error_t *error);
/* NULL/empty is a no-op. BUSY if a scan is alive; otherwise releases the owner
 * lease, even on EOF/error/cancellation. Does not end the native transaction. */
turbodb_status_t orm_tidesdb_sql_show_close(orm_sql_show_source *source, turbodb_error_t *error);
#endif
