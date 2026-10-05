#ifndef ORM_TIDESDB_SQL_INDEX_CHANGE_H
#define ORM_TIDESDB_SQL_INDEX_CHANGE_H
#include "index_directory.h"

/* Full schema-ordered rows, borrowed for prepare only. NULL before means insert,
 * NULL after means delete. Both present means ordered update/primary-key move.
 * Rows and primary scalars must already be validated against the Catalog; the
 * before rows must be decoded from the same transaction's existing Data. */
typedef struct orm_sql_index_change {
  const turbodb_value_t *before, *after;
  size_t rows, columns, primary;
} orm_sql_index_change;
typedef struct orm_sql_index_mutation {
  orm_sql_store_write write;
  const uint8_t *primary;
  size_t sequence;
} orm_sql_index_mutation;
/* Private, move-only bounded command, zero initialize. Owns key bytes and ordered
 * mutations until close; mutation views expire then. Never commits or writes.
 * All predicates/occupancy checks succeed before publishing output. The caller
 * MUST append published writes to the same store_batch as Data and TableVersion
 * (or index directory and TableVersion for a validated DROP INDEX command).
 * Validation-only callers may close a prepared delta without publishing it;
 * no caller may independently advance the derived index state. */
typedef struct orm_sql_index_delta {
  orm_tidesdb_sql_budget *budget;
  vec_t keys, mutations;
  size_t key_bytes, mutation_bytes, metadata, count;
} orm_sql_index_delta;
/* O(E log E * maximum_key_bytes) validation, O(total key bytes + E) storage.
 * E is <= 4 * rows * index count. Fixed workspace, checked arithmetic and shared
 * WORK/READ/STEP limits. Simulates writes per physical key in original row order:
 * a later occupied unique key cannot be taken before its row vacates it.
 * CONSTRAINT for unique collisions; DATASTORE_ERROR for missing/mismatched old
 * entries or malformed occupancy (poisons owner); limits/OOM retain their code.
 * Failure preserves output and releases work; cumulative charges remain. */
turbodb_status_t orm_sql_index_delta_prepare(orm_sql_catalog_store *, const orm_sql_index_set *,
    const orm_sql_index_change *, orm_sql_index_delta *, turbodb_error_t *);
turbodb_status_t orm_sql_index_delta_close(orm_sql_index_delta *, turbodb_error_t *);
/* Audit complete index/unique namespaces against a decoded whole-table before
 * snapshot. Requires every expected entry already validated by delta_prepare;
 * counts/generation checks then reject extras. Zero rows allowed. Uses scoped
 * native iterators, no mutation or retained views; READ/STEP limits apply.
 * Caller poisons owner on DATASTORE_ERROR. */
turbodb_status_t orm_sql_index_snapshot_audit(orm_sql_catalog_store *, const orm_sql_index_set *,
    const orm_sql_index_change *snapshot, turbodb_error_t *);
#endif
