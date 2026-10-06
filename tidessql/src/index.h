#ifndef ORM_TIDESDB_SQL_INDEX_H
#define ORM_TIDESDB_SQL_INDEX_H
#include "schema.h"
#include <cstl/vec.h>
#include <sqlparser/sqlparser.h>

typedef struct orm_sql_index_part {
  size_t column;
  orm_sql_type type;
  bool descending;
} orm_sql_index_part;

/* Private, immutable after binding. Zero initialize; single synchronous owner.
 * Names and parts outlive the borrowed AST/schema. Move only, never shallow-copy
 * owners. Parts refer to ordinals in the supplied Catalog snapshot; a future
 * storage caller must validate that table version before publishing the index.
 * Budget must stay active until destroy. No storage or transaction side effects. */
typedef struct orm_sql_index_definition {
  orm_tidesdb_sql_budget *budget;
  vec_t parts;
  size_t part_bytes, metadata_bytes, source_offset;
  char name[ORM_SQL_SELECT_NAME_BYTES + 1], table[ORM_SQL_SELECT_NAME_BYTES + 1];
  size_t name_size, table_size;
  bool unique, generated_name;
} orm_sql_index_definition;

/* Bind one MySQL CREATE [UNIQUE] INDEX name ON table (column [ASC|DESC], ...).
 * schema must be a valid immutable Catalog schema; names are case-sensitive.
 * I64/U64/DOUBLE key columns; nullability is preserved, including UNIQUE keys.
 * SQL_ERROR: unknown/duplicate columns, mismatched table, reserved PRIMARY name.
 * UNSUPPORTED: other statements/dialects, expressions, prefixes, qualifiers,
 * partial indexes, IF NOT EXISTS or other key types. LIMIT_EXCEEDED: resource
 * or identifier bounds. OUT_OF_MEMORY: allocation failure. INVALID_ARGUMENT:
 * missing arguments or occupied output. Failure preserves output and refunds
 * temporary WORK; AST/PLAN/STEP charges remain cumulative. error is optional.
 * Typical lifecycle: bind_create(doc, &schema, budget, &definition, error),
 * consume read-only definition.parts, then destroy(&definition, error).
 * This complete private binder does not enable SQL execution of CREATE INDEX. */
turbodb_status_t orm_tidesdb_sql_index_bind_create(const sqlparser_document *document,
    const orm_sql_table_schema *schema, orm_tidesdb_sql_budget *budget,
    orm_sql_index_definition *out, turbodb_error_t *error);

/* Private CREATE TABLE binder steps. Caller validated the single MySQL CREATE
 * root and schema. Table keys may omit the index name; column keys are UNIQUE.
 * generated_name marks the first-column name for catalog-level suffixing.
 * Owns independent names/parts on success. Same type/error/lifetime rules as
 * bind_create; AST was already charged by the complete table binder. */
turbodb_status_t orm_sql_index_bind_table_key(const sqlparser_document *, const sqlparser_node *,
    const orm_sql_table_schema *, orm_tidesdb_sql_budget *, orm_sql_index_definition *, turbodb_error_t *);
turbodb_status_t orm_sql_index_bind_column_key(const sqlparser_document *, const sqlparser_node *, sqlparser_id,
    const orm_sql_table_schema *, orm_tidesdb_sql_budget *, orm_sql_index_definition *, turbodb_error_t *);

/* Releases owned storage and WORK, zeros the definition; NULL/empty is a no-op.
 * All borrowed parts/name views expire here. Reports budget invariant failures. */
turbodb_status_t orm_tidesdb_sql_index_destroy(orm_sql_index_definition *definition, turbodb_error_t *error);

/* Private tuple codec, NOT a complete persistent Index/Unique key. The caller
 * adds index identity/generation and primary-key suffix in the storage layer.
 * For one immutable definition, memcmp order agrees with its SQL key ordering.
 * Each part is a NULL marker plus an ordered I64/U64/finite binary64 payload,
 * complemented for DESC. DOUBLE zero signs encode identically; decode yields
 * positive zero. Persistent DOUBLE indexes require Catalog format v3.
 * Caller budgets/owns the fixed output storage; no allocations or retained
 * pointers. Input and output regions must not overlap. Budget stays active.
 * Size computes required capacity without consuming STEP; unchanged on error. */
turbodb_status_t orm_tidesdb_sql_index_key_size(const orm_sql_index_definition *definition,
    size_t *out, turbodb_error_t *error);

/* Encode indexed columns from a full row (non-key columns are not validated).
 * On success writes exactly key_size bytes and sets contains_null. A unique
 * occupancy record is needed iff definition.unique && !contains_null; this
 * function does not perform uniqueness checks. All failures preserve bytes and
 * contains_null; consumed STEP remains charged. TYPE_ERROR for indexed values
 * outside the bound type/nullability or with nonzero reserved; INVALID_ARGUMENT
 * for absent arguments/column slots; LIMIT_EXCEEDED for capacity/STEP bounds.
 * Use key_size, allocate caller-owned workspace, encode, consume bytes. */
turbodb_status_t orm_tidesdb_sql_index_key_encode(const orm_sql_index_definition *definition,
    const turbodb_value_t *row, size_t count, uint8_t *out, size_t capacity,
    bool *contains_null, turbodb_error_t *error);

/* Decode exact key_size bytes into K values in key-part order, where K is
 * vec_size(definition.parts); output capacity must be >= K. Validates the entire
 * tuple before publishing any values or contains_null. DATASTORE_ERROR for
 * wrong length, markers, noncanonical NULL payloads, NULL in a NOT NULL key,
 * nonfinite DOUBLE or noncanonical negative-zero DOUBLE payloads.
 * Other argument/resource errors follow encode. Trailing output slots are
 * untouched. No inference of a schema or a key format from untrusted bytes. */
turbodb_status_t orm_tidesdb_sql_index_key_decode(const orm_sql_index_definition *definition,
    const uint8_t *data, size_t size, turbodb_value_t *out, size_t capacity,
    bool *contains_null, turbodb_error_t *error);
#endif
