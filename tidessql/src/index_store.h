#ifndef ORM_TIDESDB_SQL_INDEX_STORE_H
#define ORM_TIDESDB_SQL_INDEX_STORE_H
#include "catalog_store.h"
#include <sqlparser/sqlparser.h>

/* Private atomic index construction, used by the SQL runtime.
 * Borrows one MySQL CREATE [UNIQUE] INDEX document and a synchronous SERIALIZABLE
 * owner. No live sources allowed. Binds against the actual Catalog schema,
 * scans its complete snapshot twice, rejects duplicate non-NULL unique tuples,
 * and batches index entries, directory, TableVersion and Manifest atomically.
 * The first pass counts rows for fixed capacity; no cursor views survive next.
 * Base rows remain the fact source; index entries are derived. No engine commit.
 * Success sets index_id; failure preserves it and rolls back only this command.
 * The caller commits/rolls back with catalog_finish, or rolls back its savepoint.
 * Cleanup failure poisons owner, which must roll back. No automatic retry.
 *
 * Supports I64/U64/DOUBLE keys, first and subsequent indexes. Integer indexes
 * require Manifest v2; DOUBLE indexes require v3 and record wire v2. Publication
 * upgrades only when necessary; later index/table creation preserves the highest
 * capability. Older drivers reject unsupported formats. Indexed DML uses the
 * shared index-maintenance
 * path. No automatic downgrade, implicit commit or asynchronous build. Names
 * are unique within their table; existing names return CONSTRAINT. Directory
 * and both generations are checked against the same transaction snapshot.
 *
 * Missing table -> SQL_ERROR; duplicate key -> CONSTRAINT; unsupported dialect,
 * statement/key type/format -> UNSUPPORTED; corrupt storage -> DATASTORE_ERROR;
 * capacity/WORK/MATERIALIZED/READ/WRITE/STEP -> LIMIT_EXCEEDED; allocation -> OOM.
 * Catalog max_record_bytes also bounds physical index keys and directory value.
 * O(N*C + N*K log N) time, O(N*K + N + C) work, bounded by existing budgets.
 * All temporary ownership ends before return; output is borrowed scalar only.
 */
turbodb_status_t orm_tidesdb_sql_index_build(const sqlparser_document *document,
    orm_sql_catalog_store *store, uint64_t *index_id, turbodb_error_t *error);
/* Drop one MySQL secondary index, same owner/transaction/cleanup contract.
 * Re-derives its keys from Data and validates all physical entries before one
 * atomic batch removes Index/Unique/directory and increments TableVersion.
 * Data and other indexes are unchanged. The Manifest keeps its format, epoch/next-id
 * stay unchanged, and dropped IDs are never recycled. No background deletion.
 * Missing table/index -> SQL_ERROR; PRIMARY or other dialect/form -> UNSUPPORTED;
 * corrupt directory/rows/entries -> DATASTORE_ERROR and owner requires rollback.
 * Uses bounded two-pass row materialization and the existing index-change
 * validator: O(N*C + N*K log N) time and O(N*C + N*K) work, plus directory load.
 * READ/WORK/MATERIALIZED/PLAN/STEP/WRITE limits and max_record_bytes apply. On
 * success *index_id identifies the removed index; failure preserves it. Caller
 * must commit before reporting durability. Optional error; no automatic retry. */
turbodb_status_t orm_tidesdb_sql_index_drop(const sqlparser_document *document,
    orm_sql_catalog_store *store, uint64_t *index_id, turbodb_error_t *error);
/* Read-only PREPARE validation; only Catalog/secondary-index directory metadata
 * is read. No physical index entries, rows, evaluation, IDs or publication.
 * CREATE name collisions/uniqueness/physical capacity remain EXECUTE checks.
 * Borrows inputs through return; closes all temporary ownership. */
turbodb_status_t orm_sql_index_create_validate(const sqlparser_document *, orm_sql_catalog_store *, turbodb_error_t *);
turbodb_status_t orm_sql_index_drop_validate(const sqlparser_document *, orm_sql_catalog_store *, turbodb_error_t *);
#endif
