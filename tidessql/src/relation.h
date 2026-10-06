#ifndef ORM_TIDESDB_SQL_RELATION_H
#define ORM_TIDESDB_SQL_RELATION_H
#include "catalog_store.h"
#include "scan.h"
#include "index_lookup.h"
enum { ORM_SQL_RELATION_PREFIX_BYTES = 17, ORM_SQL_RELATION_KEY_BYTES = 25 };
/* Private numeric wire helpers for atomic schema rewrite. Size bounds the
 * value bytes (excluding the Data key). Encode validates a full schema row
 * before touching output; prefix comes from a validated live relation snapshot.
 * Output is exactly KEY_BYTES+row_size, borrows only the call, and remains
 * unchanged on error. No allocation/native mutation; active budget required. */
turbodb_status_t orm_sql_relation_row_size(size_t columns, size_t max_bytes, size_t *, turbodb_error_t *);
turbodb_status_t orm_sql_relation_encode_record(const orm_sql_table_schema *, size_t primary,
    const turbodb_value_t *, const uint8_t prefix[ORM_SQL_RELATION_PREFIX_BYTES],
    uint8_t *, size_t capacity, orm_tidesdb_sql_budget *, turbodb_error_t *);
/* Private numeric relational rows in the Catalog CF.
 * Insert borrows a complete schema-ordered row for the call. Strict kind/NULL
 * checks, no conversion/default/TTL. Duplicate PK -> CONSTRAINT, missing table
 * -> SQL_ERROR. Data and TableVersion+1 are one atomic statement; success is
 * uncommitted until catalog_finish(true). Indexed v2 also reads the complete
 * table index directory and batches all Index/Unique changes with Data/version;
 * duplicate non-NULL unique tuples reject the whole statement. Corrupt old
 * index entries poison the owner rather than being repaired. Same owner must
 * have no live sources.
 * max_record_bytes bounds the encoded row. Failure does not report affected rows.
 * The caller owns a count-element immutable array for this synchronous call. */
turbodb_status_t orm_tidesdb_sql_relation_insert(orm_sql_catalog_store *store, vstr table,
    const turbodb_value_t *values, size_t count, turbodb_error_t *error);
/* Row-major rows*columns immutable values, borrowed through return. Nonempty,
 * checked dimensions; O(rows*columns + rows log rows) work, O(rows*columns)
 * storage, bounded by work/materialized-row and read/write/step budgets.
 * Validates the entire batch before any put. One savepoint and one version
 * increment per successful batch; any duplicate rejects all rows. */
turbodb_status_t orm_tidesdb_sql_relation_insert_rows(orm_sql_catalog_store *store, vstr table,
    const turbodb_value_t *values, size_t rows, size_t columns, turbodb_error_t *error);
/* Same bounded atomic batch protocol, but every distinct key must already
 * exist (else CONSTRAINT). Update replaces complete rows at their current PK;
 * it neither changes keys nor detects unchanged values. Delete borrows rows
 * primary-key scalars. Both validate persisted records before modifying them.
 * Empty batches are INVALID_ARGUMENT; SQL handles zero affected rows itself. */
turbodb_status_t orm_tidesdb_sql_relation_update_rows(orm_sql_catalog_store *store, vstr table,
    const turbodb_value_t *values, size_t rows, size_t columns, turbodb_error_t *error);
/* Ordered replacement of rows identified by rows borrowed old_keys scalars.
 * New PKs are taken from complete values. Both sets must be distinct; old rows
 * must exist. A destination may be vacant, the same row, or an earlier row's
 * vacated key; later occupied keys reject with CONSTRAINT. Validates all before
 * one savepoint containing per-row delete/put and one version update. O(R log R
 * + R*C) work, bounded O(R*C) storage, <= 2R+1 writes. No no-op detection. */
turbodb_status_t orm_tidesdb_sql_relation_move_rows(orm_sql_catalog_store *store, vstr table,
    const turbodb_value_t *old_keys, const turbodb_value_t *values, size_t rows, size_t columns, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_relation_delete_rows(orm_sql_catalog_store *store, vstr table,
    const turbodb_value_t *keys, size_t rows, turbodb_error_t *error);

/* Resolve the row that conflicts with one complete schema-ordered INSERT
 * candidate. Primary key is checked first, followed by non-NULL UNIQUE indexes
 * in the validated directory order. Success with found=false leaves out
 * unspecified; found=true writes exactly columns scalar values. Derived unique
 * ownership is verified against Data before publication. No mutation. */
turbodb_status_t orm_sql_relation_insert_conflict(orm_sql_catalog_store *store, vstr table,
    const turbodb_value_t *candidate, size_t columns, turbodb_value_t *out,
    bool *found, turbodb_error_t *error);

/* Zero initialize, stable address through close; single synchronous owner.
 * Owns definition/iterator/work, retains store (also at a stable address).
 * Private fields are read-only to callers. schema is borrowed until close;
 * SELECT bind copies it. Open reads Catalog and TableVersion, not Data. */
typedef struct orm_sql_relation_source {
  orm_sql_row_source source;
  orm_sql_catalog_store *owner;
  orm_sql_table_definition definition;
  orm_sql_table_schema schema;
  orm_tidesdb_iterator_t *iterator;
  vec_t types, values;
  size_t type_bytes, value_bytes, metadata_bytes;
  uint8_t prefix[ORM_SQL_RELATION_PREFIX_BYTES];
  bool advance, done;
  turbodb_error_t failure;
  uint64_t next_index_id;
  uint8_t format;
  orm_sql_index_lookup lookup;
} orm_sql_relation_source;
/* Internal index reader only: fetch/decode one required Data row by its copied
 * ordered primary key, using this source's snapshot/workspace. No iterator
 * mutation. Missing Data is corruption. Caller applies sticky error handling. */
turbodb_status_t orm_sql_relation_index_row(orm_sql_relation_source *source, const uint8_t *primary,
    const turbodb_value_t **out, turbodb_error_t *error);
/* Lazy iterator, fixed O(columns) scan work, no driver workspace allocation in next.
 * Native engine allocation is separate. Each candidate charges physical key+
 * value bytes before decoding. Validates unprojected cells and key/row PK match.
 * Use select_open_source(&source.source), select_close, relation_close, finish.
 * A failed open publishes nothing/refunds work. Error next preserves *row and
 * locks the first error; outputs expire at next/close. No SQL ordering promise.
 * Runtime may attach an equality/prefix/range index lookup after binding and before scan
 * attachment; its owned metadata/key scratch adds O(columns + key width) work.
 * Both modes use this same source lease and sticky-error/rewind/close protocol. */
turbodb_status_t orm_tidesdb_sql_relation_open(orm_sql_catalog_store *store, vstr table,
    orm_sql_relation_source *out, turbodb_error_t *error);
/* Internal compiled-plan reuse: close consumers, rewind, then reopen runs.
 * Keeps schema, source address and owner lease in the same transaction snapshot.
 * Releases the iterator; next recreates it lazily without re-reading Catalog.
 * Successful rewind invalidates borrowed rows, retains work/read quotas and
 * charges one execution step. No driver allocation; no row reads. Active
 * consumers (also at EOF/cancel) -> BUSY. Closed -> INVALID_STATE. Prior source
 * failure is preserved; all failures leave iteration state unchanged. */
turbodb_status_t orm_sql_relation_rewind(orm_sql_relation_source *source, turbodb_error_t *error);
/* Internal whole-table DDL snapshot, before attaching consumers or lookup.
 * Two passes, O(rows*columns), fixed numeric copies; source remains owned by
 * caller. Start with zero outputs; even on failure release vector/bytes and
 * count MATERIALIZED_ROWS charges before ending the statement budget. */
turbodb_status_t orm_sql_relation_snapshot(orm_sql_relation_source *, vec_t *, size_t *bytes,
    size_t *count, turbodb_error_t *);
/* BUSY while a scan borrows source. EOF/cancel/error still need close before
 * owner writes/finish. NULL/empty is a no-op; never ends the owner's transaction. */
turbodb_status_t orm_tidesdb_sql_relation_close(orm_sql_relation_source *source, turbodb_error_t *error);
#endif
