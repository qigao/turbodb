#ifndef ORM_TIDESDB_SQL_CATALOG_H
#define ORM_TIDESDB_SQL_CATALOG_H
#include "schema.h"
#include "index.h"
#include <cstl/vec.h>
#include <sqlparser/sqlparser.h>

/* Private CREATE definition, not a persistent Catalog entry. Zero initialize;
 * single synchronous owner; fields read-only after bind. Owns names and ordered
 * columns and parallel explicit-default metadata independently of the parser
 * document. Budget remains active through destroy. No table IDs, storage keys,
 * transaction or disk side effects. */
typedef struct orm_sql_table_definition {
  orm_tidesdb_sql_budget *budget;
  vec_t names, columns, defaults;
  size_t name_bytes, column_bytes, default_bytes, metadata_bytes;
  size_t primary_key;
  bool if_not_exists;
} orm_sql_table_definition;

/* Complete, move-only CREATE owner; table-only binding remains deliberately
 * strict. Index vector owns each definition. Zero initialize and destroy_all
 * on every successful bind; AST borrowed only through bind. No native I/O.
 * Accepts table INDEX/UNIQUE integer keys, including directions, and column
 * UNIQUE. Missing names use the first column plus _2/_3 suffixes; separate
 * CONSTRAINT symbols remain unsupported. Unknown/duplicate columns -> SQL_ERROR,
 * duplicate explicit index names -> CONSTRAINT, unsupported forms
 * -> UNSUPPORTED; allocation/budget errors propagate and refund owned WORK. */
typedef struct orm_sql_create_definition {
  orm_sql_table_definition table;
  vec_t indexes;
  size_t index_bytes;
} orm_sql_create_definition;
turbodb_status_t orm_sql_catalog_bind_create_all(const sqlparser_document *, orm_tidesdb_sql_budget *,
    orm_sql_create_definition *, turbodb_error_t *);
turbodb_status_t orm_sql_catalog_destroy_all(orm_sql_create_definition *, turbodb_error_t *);

/* PREPARE-only structural validation. Borrows AST/active budget through return;
 * compiles defaults but never evaluates or exports incomplete definitions.
 * No native I/O or retained ownership. Value conversion/nullability/range errors
 * are checked by ordinary DDL execution. Temporary WORK is refunded. */
turbodb_status_t orm_sql_catalog_validate_create(const sqlparser_document *, orm_tidesdb_sql_budget *, turbodb_error_t *);
turbodb_status_t orm_sql_catalog_validate_columns(const orm_sql_table_definition *, const sqlparser_document *,
    sqlparser_id added, size_t ordinal, turbodb_error_t *);
turbodb_status_t orm_sql_catalog_validate_default(const orm_sql_table_definition *, const sqlparser_document *,
    size_t ordinal, sqlparser_id expression, turbodb_error_t *);

/* Bind exactly one MySQL CREATE TABLE: BIGINT [UNSIGNED], DOUBLE, NULL/NOT NULL,
 * constant-expression column defaults converted to finite numeric/NULL values,
 * and one integer PRIMARY KEY (inline or table-level). PK implies NOT NULL and may have a
 * compatible numeric default.
 * ASCII case-sensitive names follow the same private profile as SELECT.
 * Defaults use the scalar expression whitelist without inputs or queries;
 * numeric/BOOL/numeric-text results use strict column assignment conversion.
 * Invalid text -> TYPE_ERROR, range failures -> OUT_OF_RANGE, nonnullable NULL
 * -> SQL_ERROR. Conversion finishes before borrowed expression bytes expire.
 * Dynamic expressions, AUTO_INCREMENT, indexes, named/composite keys,
 * temporary/LIKE/AS SELECT and other unsupported attributes/options fail.
 * IF NOT EXISTS is retained as command intent; existence is NOT checked here.
 * SQL_ERROR: duplicate columns/keys or unknown key column. UNSUPPORTED: outside
 * subset. LIMIT_EXCEEDED: resource/name bounds. Failed bind leaves output empty
 * and refunds work; AST/plan/step consumption is retained. Invalid arguments or
 * occupied output leave output unchanged. O(nodes + columns^2) time, O(columns)
 * work, bounded by the active budget; comparisons/traversal are charged.
 * This does not create a table or validate rows/uniqueness. */
turbodb_status_t orm_tidesdb_sql_catalog_bind_create(const sqlparser_document *document,
    orm_tidesdb_sql_budget *budget, orm_sql_table_definition *out, turbodb_error_t *error);

/* Borrowed immutable view until definition destruction; columns and defaults
 * have the same count and ordinal, and no allocation occurs here. For
 * example: bind_create(ddl), definition_schema(def, &schema), select_bind(query,
 * &schema, ...), destroy(def). SELECT copies the schema during bind. */
turbodb_status_t orm_tidesdb_sql_catalog_schema(const orm_sql_table_definition *definition,
    orm_sql_table_schema *out, turbodb_error_t *error);
/* Private rename preparation on an exclusively owned decoded definition.
 * slot=0 renames the table; slot=column ordinal+1 renames a column. Input name
 * must not alias definition storage. No allocation or native mutation; all
 * borrowed schema views expire on success. Duplicate columns -> SQL_ERROR.
 * Preserves types, column order and primary key. Failure leaves names intact. */
turbodb_status_t orm_sql_catalog_rename(orm_sql_table_definition *, size_t slot, vstr name, turbodb_error_t *);
/* Private preparation on an exclusively owned decoded definition. expression
 * NONE removes the explicit default, otherwise reuses CREATE's constant binding
 * and strict numeric assignment/nullability rules. Caller resolves the
 * column name; ordinal is zero based. Borrows MySQL AST/budget through return,
 * owns resulting scalar independently of AST; no native I/O. Failure leaves the
 * old default intact and refunds temporary work. Existing schema/default views
 * must not be retained across mutation/destruction. table_alter publishes the
 * prepared definition atomically; this function does not persist or commit. */
turbodb_status_t orm_sql_catalog_default(orm_sql_table_definition *, const sqlparser_document *,
    size_t ordinal, sqlparser_id expression, turbodb_error_t *);
/* Prepare an independently owned definition with one inserted numeric column
 * or one removed non-PK column. column=AST COLUMN for ADD, ordinal in [0,count]
 * is its new position; for DROP column is NONE and ordinal is the old column.
 * Document borrows through return. Remaining PK ordinals are remapped.
 * Caller owns empty out; source remains immutable. No native reads/writes or
 * row/index validation here; table_alter owns those checks. Numeric/NULL defaults
 * use CREATE rules; no extra key or other constraints. Failure refunds WORK and
 * preserves output. */
turbodb_status_t orm_sql_catalog_columns(const orm_sql_table_definition *, const sqlparser_document *,
    sqlparser_id column, size_t ordinal, orm_sql_table_definition *out, turbodb_error_t *);
/* NULL/empty is a no-op. Caller must release all borrowed schema views first. */
turbodb_status_t orm_tidesdb_sql_catalog_destroy(orm_sql_table_definition *definition, turbodb_error_t *error);

/* Private schema wire v1/v2, specified in design.md. Definitions without an
 * explicit default retain exact v1 bytes; any explicit default selects v2.
 * Encode owns one byte vec;
 * caller releases with orm_sql_work_release(out, reserved, definition->budget).
 * Output vec/reserved must be zero. Decode copies all metadata; input borrows
 * only the call. Both require max_bytes > 0 and an active budget. Invalid wire
 * is DATASTORE_ERROR, unknown version UNSUPPORTED; byte/work limits are
 * LIMIT_EXCEEDED. Failures leave outputs empty, refund work, retain counters.
 * No table IDs, IF NOT EXISTS intent, disk access or native transaction here. */
turbodb_status_t orm_tidesdb_sql_catalog_encode(const orm_sql_table_definition *definition,
    size_t max_bytes, vec_t *out, size_t *reserved, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_catalog_decode(const uint8_t *data, size_t size, size_t max_bytes,
    orm_tidesdb_sql_budget *budget, orm_sql_table_definition *out, turbodb_error_t *error);
#endif
