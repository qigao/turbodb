#ifndef ORM_TIDESDB_SQL_INDEX_RECORD_H
#define ORM_TIDESDB_SQL_INDEX_RECORD_H
#include "index.h"

typedef struct orm_sql_index_identity {
  uint64_t table_id, table_generation, index_id, generation;
} orm_sql_index_identity;

/* Private owned Catalog record, zero initialize. Definition owns its parts;
 * record/definition are move-only. Identity is immutable and nonzero once
 * populated by the caller's Catalog transaction. No native side effects here.
 * Codec does not allocate identities, publish READY state, or verify that IDs
 * match a storage lookup key; the future storage owner must do that check.
 * Budget remains active through record_destroy. */
typedef struct orm_sql_index_record {
  orm_sql_index_definition definition;
  orm_sql_index_identity identity;
} orm_sql_index_record;

/* Encode an owned definition/identity after checking it against the supplied
 * valid immutable table schema. Does not retain schema. Output vec and *reserved
 * must be zero; success transfers one fixed owned byte vector and WORK receipt.
 * Release with orm_sql_work_release(out, *reserved, definition.budget, error).
 * max_bytes bounds the wire record. Invalid owner/identity/definition/schema
 * mismatch -> INVALID_ARGUMENT, byte/resource bounds -> LIMIT_EXCEEDED,
 * allocation failure -> OUT_OF_MEMORY. Failures preserve outputs/refund WORK.
 * No AST dependency; no C struct layout or enum values stored in the wire. */
turbodb_status_t orm_tidesdb_sql_index_record_encode(const orm_sql_index_record *record,
    const orm_sql_table_schema *schema, size_t max_bytes, vec_t *out,
    size_t *reserved, turbodb_error_t *error);

/* Decode wire v1 (integer-only) or v2 (contains DOUBLE) into an empty owned record.
 * Both input bytes and
 * schema are borrowed only through return; outputs must not alias either.
 * DATASTORE_ERROR for corrupt magic/flags/names/identity/columns/length or a
 * schema mismatch; UNSUPPORTED for an unknown wire version. The caller checks
 * returned identity against its Catalog lookup scope before using the record.
 * Other errors follow encode; successful output charges PLAN/WORK/STEP, failed
 * output stays untouched and refunds WORK. No native I/O or automatic migration.
 * Example: decode(bytes, size, &schema, max_bytes, budget, &record, error),
 * consume record.definition/identity, then record_destroy(&record, error). */
turbodb_status_t orm_tidesdb_sql_index_record_decode(const uint8_t *data, size_t size,
    const orm_sql_table_schema *schema, size_t max_bytes, orm_tidesdb_sql_budget *budget,
    orm_sql_index_record *out, turbodb_error_t *error);

/* NULL/empty is allowed; releases all owned WORK and clears the whole record. */
turbodb_status_t orm_tidesdb_sql_index_record_destroy(orm_sql_index_record *record, turbodb_error_t *error);
#endif
