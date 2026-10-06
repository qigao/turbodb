#ifndef ORM_TIDESDB_SQL_CATALOG_STORE_H
#define ORM_TIDESDB_SQL_CATALOG_STORE_H
#include "catalog.h"
#include "bridge.h"

/* Private directory transaction owner; zero initialize. No public ORM entry.
 * Single synchronous owner; DB/CF/budget must outlive finish; CF configuration
 * must remain unchanged. Owns its native
 * SERIALIZABLE transaction; do not use/mutate native handles outside this API.
 * No cache, automatic retries or schema migration. The private
 * relation module shares this owner; active sources block mutation and finish.
 * Its fixed metadata retains shared work capacity across budget end/begin.
 * All operations except finish require an active statement. Close every source
 * and destroy statement definitions/plans before budget_end. Native transaction
 * and cumulative limits survive statement boundaries; no implicit reset. */
typedef struct orm_sql_catalog_store {
  orm_tidesdb_transaction_t *transaction;
  orm_tidesdb_column_family_t *family;
  orm_tidesdb_sql_budget *budget;
  size_t max_record_bytes, metadata_bytes, active_sources;
  vec_t savepoints;
  size_t savepoint_bytes, savepoint_limit;
  bool failed;
} orm_sql_catalog_store;

/* User savepoints, owned by the same native transaction. Require an active
 * statement budget, healthy owner and no sources. Names: unquoted ASCII
 * identifiers <=63 bytes, case-insensitive. Create replaces/moves an existing
 * name to newest; rollback keeps the named point and removes later points;
 * release removes only that point. Missing rollback/release -> SQL_ERROR.
 * max_savepoints is positive and <=INT_MAX/2-1, fixed after first allocation.
 * Storage is lazily preallocated, retained until finish, charged to WORK.
 * Full -> LIMIT_EXCEEDED, allocation -> OUT_OF_MEMORY, no native side effects.
 * Native failure poisons owner: only full rollback/finish(false) remains legal.
 * No read/write budget refunds or implicit commit. O(max_savepoints) time/space.
 * Example: budget_begin; savepoint(owner,"s",64); budget_end; run statements;
 * budget_begin; rollback_to(owner,"s"); budget_end; catalog_finish(owner,true).
 * See relational_test.c for executable API use. */
turbodb_status_t orm_tidesdb_sql_catalog_savepoint(orm_sql_catalog_store *store,
    vstr name, size_t max_savepoints, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_catalog_rollback_to(orm_sql_catalog_store *store,
    vstr name, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_catalog_release_savepoint(orm_sql_catalog_store *store,
    vstr name, turbodb_error_t *error);

/* Explicit bootstrap of an EXISTING empty SYNC_FULL CF. Caller guarantees
 * exclusive/quiescent access; CF creation is outside this transaction. Commits
 * only the Manifest; refuses every nonempty CF, including initialized ones.
 * Failure leaves CF intact. No automatic bootstrap in begin. */
turbodb_status_t orm_tidesdb_sql_catalog_initialize(orm_tidesdb_database_t *database,
    orm_tidesdb_column_family_t *family, size_t max_record_bytes,
    orm_tidesdb_sql_budget *budget, turbodb_error_t *error);
/* Accepts base v1 and indexed v2. Missing Manifest is INVALID_STATE, unknown
 * version UNSUPPORTED, corrupt
 * metadata DATASTORE_ERROR. max_record_bytes bounds encoded values and work
 * reservation before native get (native internal memory is separately owned).
 * Failure leaves output empty. */
turbodb_status_t orm_tidesdb_sql_catalog_begin(orm_tidesdb_database_t *database,
    orm_tidesdb_column_family_t *family, size_t max_record_bytes,
    orm_tidesdb_sql_budget *budget, orm_sql_catalog_store *out, turbodb_error_t *error);
/* Same ownership/error contract as begin, with an optional explicit abort-cleanup
 * observation. Cleared on entry; set only if begin's native rollback or retained
 * ownership cleanup failed. Distinguishes cleanup failure from a primary read
 * error with the same status, so session adapters can quarantine without parsing
 * error text. No change to the historical begin error codes. */
turbodb_status_t orm_sql_catalog_begin_checked(orm_tidesdb_database_t *database,
    orm_tidesdb_column_family_t *family, size_t max_record_bytes,
    orm_tidesdb_sql_budget *budget, orm_sql_catalog_store *out,
    bool *cleanup_failed, turbodb_error_t *error);
/* Lookup by normalized ASCII name, in this transaction's snapshot. On success
 * found=false leaves entry/id/version unchanged; found=true gives an owned
 * definition, destroyed via catalog_destroy before budget end. All outputs are
 * unchanged on error. No native buffers survive the call. */
turbodb_status_t orm_tidesdb_sql_catalog_lookup(orm_sql_catalog_store *store, vstr name,
    orm_sql_table_definition *out, uint64_t *table_id, uint64_t *version,
    bool *found, turbodb_error_t *error);
/* Snapshot enumeration, physical key order. Keep owner/cursor addresses stable.
 * One owned definition per successful next; destroy it before reuse. EOF sets
 * found=false only. Errors leave outputs unchanged and lock the first failure.
 * The cursor blocks writes/finish until close, even at EOF or after failure.
 * Workspace is bounded independently of table count; native memory is separate. */
typedef struct orm_sql_catalog_cursor {
  orm_sql_catalog_store *owner;
  orm_tidesdb_iterator_t *iterator;
  size_t metadata_bytes;
  uint64_t epoch, next_id;
  turbodb_error_t failure;
  bool advance, done;
} orm_sql_catalog_cursor;
turbodb_status_t orm_tidesdb_sql_catalog_cursor_open(orm_sql_catalog_store *store,
    orm_sql_catalog_cursor *out, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_catalog_cursor_next(orm_sql_catalog_cursor *cursor,
    orm_sql_table_definition *out, uint64_t *table_id, uint64_t *version,
    bool *found, turbodb_error_t *error);
/* NULL/empty is a no-op. Owned definitions may outlive cursor close. */
turbodb_status_t orm_tidesdb_sql_catalog_cursor_close(orm_sql_catalog_cursor *cursor, turbodb_error_t *error);
/* Definition must use the same budget. Atomic statement under a private
 * savepoint. Duplicate name is CONSTRAINT unless IF NOT EXISTS, which returns
 * existing id/created=false without comparing schemas. Success is uncommitted;
 * finish(true) must succeed before reporting durable creation. Failure leaves
 * output scalars unchanged; rollback/release failure poisons the owner. */
turbodb_status_t orm_tidesdb_sql_catalog_create(orm_sql_catalog_store *store,
    const orm_sql_table_definition *definition, uint64_t *table_id, bool *created, turbodb_error_t *error);
/* Commit or rollback then destroy owner and release retained work. May run
 * between statements with an inactive budget. Failed owner rejects
 * commit, requiring finish(false). Commit conflict -> BUSY; all other commit
 * failures -> COMMIT_UNKNOWN even if rollback succeeds; no automatic retry.
 * BUSY while a relation source is alive, including EOF/cancelled sources.
 * NULL/empty is a no-op. Definitions may outlive finish while budget is active. */
turbodb_status_t orm_tidesdb_sql_catalog_finish(orm_sql_catalog_store *store, bool commit, turbodb_error_t *error);
#endif
