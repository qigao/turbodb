#ifndef ORM_TIDESDB_SQL_STORE_INTERNAL_H
#define ORM_TIDESDB_SQL_STORE_INTERNAL_H
#include "catalog_store.h"
#include "wire.h"
enum { ORM_SQL_STORE_FORMAT_BASE = 1, ORM_SQL_STORE_FORMAT_INDEXED = 2, ORM_SQL_STORE_FORMAT_REAL_INDEXED = 3 };
enum { ORM_SQL_STORE_FORMAT_OFFSET = 4 };
enum { ORM_SQL_STORE_VERSION_KEY_BYTES = 1 + ORM_SQL_WIRE_U64 };
enum { ORM_SQL_STORE_MANIFEST_KEY_BYTES = 5, ORM_SQL_STORE_MANIFEST_BYTES = 24 };
enum { ORM_SQL_STORE_NAME_KEY_BYTES = 2 + ORM_SQL_SELECT_NAME_BYTES };
/* Validated normalized name. Output key length is 2 + name.len. */
turbodb_status_t orm_sql_store_name_key(vstr, uint8_t key[ORM_SQL_STORE_NAME_KEY_BYTES], turbodb_error_t *);
/* Re-encode the current Manifest unchanged. A same-value PUT in the caller's
 * atomic DDL batch is a directory conflict barrier, not an ID allocation. */
turbodb_status_t orm_sql_store_catalog_barrier(orm_sql_catalog_store *,
    uint8_t key[ORM_SQL_STORE_MANIFEST_KEY_BYTES], uint8_t data[ORM_SQL_STORE_MANIFEST_BYTES], turbodb_error_t *);
/* Audit an ID's complete physical namespace after expected entries have been
 * individually validated. Counts plus generation/shape checks reject extras;
 * this does not replace row/tuple validation. Bounded O(entries) native scan. */
typedef struct orm_sql_store_audit {
  uint8_t space;
  uint64_t id, generation;
  size_t key_size, min_value_size, max_value_size, entries;
} orm_sql_store_audit;
turbodb_status_t orm_sql_store_audit_prefix(orm_sql_catalog_store *, const orm_sql_store_audit *, turbodb_error_t *);
/* Private index publication, prepared from the current snapshot without writes.
 * The caller must atomically batch this manifest with backfill, directory and
 * TableVersion. This is the internal publication boundary used by CREATE INDEX. */
typedef struct orm_sql_index_publication {
  uint64_t id;
  uint8_t previous_format;
  uint8_t key[ORM_SQL_STORE_MANIFEST_KEY_BYTES], data[ORM_SQL_STORE_MANIFEST_BYTES];
} orm_sql_index_publication;
turbodb_status_t orm_sql_store_prepare_index(orm_sql_catalog_store *, orm_sql_index_publication *, turbodb_error_t *);
/* Reject any entry at an unused object prefix; a one-byte prefix rejects the
 * entire remaining namespace range when leaving base format. Charges READ/STEP.
 * Caller owns a ready transaction; corruption poisons it, no native writes. */
turbodb_status_t orm_sql_store_unused_prefix(orm_sql_catalog_store *, const uint8_t *, size_t, turbodb_error_t *);
/* Publish a fully bound CREATE and all empty secondary indexes in one batch.
 * Same ownership/IF NOT EXISTS contract as catalog_create; no implicit commit. */
turbodb_status_t orm_sql_store_create_all(orm_sql_catalog_store *, const orm_sql_create_definition *,
    uint64_t *, bool *, turbodb_error_t *);
typedef struct orm_sql_catalog_snapshot { uint64_t next_id; uint8_t format; } orm_sql_catalog_snapshot;
/* Same lookup contract, also returns the transaction's Manifest bounds. No
 * cached format: a user savepoint rollback changes the next lookup naturally. */
turbodb_status_t orm_sql_store_lookup(orm_sql_catalog_store *, vstr, orm_sql_table_definition *,
    uint64_t *, uint64_t *, bool *, orm_sql_catalog_snapshot *, turbodb_error_t *);
/* Encode a validated owned schema with an existing or newly allocated ID.
 * Preserves wire generation=1 and creation epoch=id. Empty outputs required;
 * release the returned WORK vec via orm_sql_work_release. No native mutation. */
turbodb_status_t orm_sql_store_encode_table(const orm_sql_table_definition *, uint64_t id,
    size_t max_bytes, vec_t *, size_t *reserved, turbodb_error_t *);
typedef struct store_buffer { uint8_t *data; size_t size, reserved; bool found; } store_buffer;
typedef enum orm_sql_store_operation { ORM_SQL_STORE_PUT, ORM_SQL_STORE_DELETE } orm_sql_store_operation;
typedef struct orm_sql_store_write {
  const uint8_t *key, *value;
  size_t key_size, value_size;
  orm_sql_store_operation operation;
} orm_sql_store_write;
/* Owner implementation boundary. All buffers borrow only the synchronous call;
 * get buffers must be closed on every path. Batch reserves all writes before a
 * savepoint, rolls back partial puts, and poisons the owner on cleanup failure.
 * Callers must bind/validate their complete command before batch admission. */
turbodb_status_t orm_sql_store_ready(orm_sql_catalog_store *, turbodb_error_t *);
turbodb_status_t orm_sql_store_writable(orm_sql_catalog_store *, turbodb_error_t *);
turbodb_status_t orm_sql_store_native(turbodb_error_t *, int, const char *);
turbodb_status_t orm_sql_store_get(orm_sql_catalog_store *, const uint8_t *, size_t, size_t, store_buffer *, turbodb_error_t *);
turbodb_status_t orm_sql_store_buffer_close(orm_sql_catalog_store *, store_buffer *, turbodb_error_t *);
void orm_sql_store_version_key(uint64_t, uint8_t key[ORM_SQL_STORE_VERSION_KEY_BYTES]);
turbodb_status_t orm_sql_store_batch(orm_sql_catalog_store *, const orm_sql_store_write *, size_t, turbodb_error_t *);
/* Private outer statement boundary for commands that must publish multiple
 * validated batches sequentially. Nesting with store_batch and user savepoints
 * is native-transaction local. Finish releases on success or rolls back the
 * complete command on failure; cleanup failure poisons the owner. */
turbodb_status_t orm_sql_store_command_begin(orm_sql_catalog_store *, turbodb_error_t *);
turbodb_status_t orm_sql_store_command_finish(orm_sql_catalog_store *, turbodb_status_t, turbodb_error_t *);
#endif
