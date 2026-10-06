#ifndef ORM_TIDESDB_SQL_EXPLAIN_H
#define ORM_TIDESDB_SQL_EXPLAIN_H
#include "select.h"
#include "index_lookup.h"

enum { ORM_SQL_EXPLAIN_COLUMNS = 12, ORM_SQL_EXPLAIN_EXTRA_BYTES = 256 };
/* Private single-owner derived source. Zero initialize, keep at a stable address
 * until close. Owns all metadata/text; borrows only the active budget after open.
 * No Catalog lease here: runtime retains its relation or unit source until query close. */
typedef struct orm_sql_explain_source {
  orm_sql_row_source source;
  orm_sql_schema_column columns[ORM_SQL_EXPLAIN_COLUMNS];
  orm_sql_type types[ORM_SQL_EXPLAIN_COLUMNS];
  turbodb_value_t values[ORM_SQL_EXPLAIN_COLUMNS];
  char table[ORM_SQL_SELECT_NAME_BYTES+1], extra[ORM_SQL_EXPLAIN_EXTRA_BYTES];
  char index[ORM_SQL_SELECT_NAME_BYTES+1], key_length[sizeof(size_t)*3+1];
  vec_t details;
  size_t detail_bytes, position, metadata_bytes;
  bool done, recursive;
} orm_sql_explain_source;

/* Immutable fixed TRADITIONAL output schema. Copies one static name/type pair;
 * false leaves out unchanged. No owner, allocation or budget charge. */
bool orm_sql_explain_column_at(size_t ordinal,orm_sql_schema_column *out);

/* Snapshots a validated SELECT plan into one TRADITIONAL row. table is the bound
 * table/alias, or empty for a unit source (NULL table/type, No tables used).
 * Table metadata is nullable so compound/dependency explanations share a schema.
 * Validates parameters/page values, never evaluates expressions or
 * reads data. Nullable estimates are unknown, never invented. No AST/plan/parameter
 * borrowing after return. Failure leaves out empty and refunds work. O(parameters
 * plus payload validation bytes), bounded constant output storage. */
turbodb_status_t orm_tidesdb_sql_explain_open(const orm_sql_select *plan, vstr table,
    const turbodb_value_t *parameters, size_t count, orm_sql_explain_source *out, turbodb_error_t *error);
/* Construction-only selected index access. Copies name/used-prefix-byte
 * length; nullable estimates remain unknown. possible_keys reports this chosen
 * candidate (selection stops at the first match), not a costed optimizer list.
 * Zero-limit plans keep their existing empty-access description. */
turbodb_status_t orm_sql_explain_index(orm_sql_explain_source *, const orm_sql_index_definition *, size_t,
    orm_sql_index_access, bool, turbodb_error_t *);
/* One row per table occurrence, in physical loop order, owned names/Extra.
 * Includes project-specific join descriptions, not optimizer estimates. */
turbodb_status_t orm_tidesdb_sql_explain_open_from(const orm_sql_select *plan, const orm_sql_from *from,
    const turbodb_value_t *parameters, size_t count, orm_sql_explain_source *out, turbodb_error_t *error);
typedef enum orm_sql_explain_block {
  ORM_SQL_EXPLAIN_PRIMARY, ORM_SQL_EXPLAIN_UNION_BRANCH, ORM_SQL_EXPLAIN_SUBQUERY,
  ORM_SQL_EXPLAIN_DERIVED, ORM_SQL_EXPLAIN_RECURSIVE_BRANCH,
  ORM_SQL_EXPLAIN_UNION_ALL, ORM_SQL_EXPLAIN_UNION_DISTINCT, ORM_SQL_EXPLAIN_QUERY_GROUP,
  ORM_SQL_EXPLAIN_INTERSECT_ALL, ORM_SQL_EXPLAIN_INTERSECT_DISTINCT,
  ORM_SQL_EXPLAIN_EXCEPT_ALL, ORM_SQL_EXPLAIN_EXCEPT_DISTINCT,
  ORM_SQL_EXPLAIN_INTERSECT_BRANCH, ORM_SQL_EXPLAIN_EXCEPT_BRANCH
} orm_sql_explain_block;
/* Construction-only annotation before the first next. Leaves keep their schema;
 * result nodes make id nullable and must be annotated before scan attachment.
 * Recursive branches label only the first physical table row's Extra; repeated
 * construction annotations do not duplicate that marker.
 * Labels/Extra are owned or static, never borrowed from the caller. */
turbodb_status_t orm_tidesdb_sql_explain_block(orm_sql_explain_source *source,
    orm_sql_explain_block kind, int64_t id, turbodb_error_t *error);
/* Close the consuming scan first (otherwise BUSY, unchanged). NULL/empty no-op. */
turbodb_status_t orm_tidesdb_sql_explain_close(orm_sql_explain_source *source, turbodb_error_t *error);
#endif
