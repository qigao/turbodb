#ifndef ORM_TIDESDB_SQL_INDEX_DIRECTORY_H
#define ORM_TIDESDB_SQL_INDEX_DIRECTORY_H
#include "index_record.h"
#include "store_internal.h"
#include <string.h>

enum { INDEX_UNIQUE_NS = 4, INDEX_DATA_NS = 5, INDEX_DIRECTORY_NS = 6,
       INDEX_PREFIX_BYTES = 1 + 2 * ORM_SQL_WIRE_U64,
       INDEX_DIRECTORY_HEADER = 2 + ORM_SQL_WIRE_U64 };
static inline void orm_sql_index_prefix(uint8_t *key, uint8_t space, const orm_sql_index_identity *identity) {
  key[0] = space;
  orm_sql_wire_write(key + 1, ORM_SQL_WIRE_U64, identity->index_id);
  orm_sql_wire_write(key + 1 + ORM_SQL_WIRE_U64, ORM_SQL_WIRE_U64, identity->generation);
}
/* Record was bound/decoded and identity-checked before encoding this key. */
static inline size_t orm_sql_index_directory_key(const orm_sql_index_record *record,
    uint8_t key[INDEX_DIRECTORY_HEADER + ORM_SQL_SELECT_NAME_BYTES]) {
  key[0] = INDEX_DIRECTORY_NS;
  orm_sql_wire_write(key + 1, ORM_SQL_WIRE_U64, record->identity.table_id);
  key[INDEX_DIRECTORY_HEADER - 1] = (uint8_t)record->definition.name_size;
  memcpy(key + INDEX_DIRECTORY_HEADER, record->definition.name, record->definition.name_size);
  return INDEX_DIRECTORY_HEADER + record->definition.name_size;
}
/* Private, owned statement snapshot. Zero initialize, move only. Records own
 * independent definitions; no native/schema/iterator views escape load. Borrow
 * record pointers until close. Same synchronous owner/budget; no cache. */
typedef struct orm_sql_index_set {
  orm_tidesdb_sql_budget *budget;
  vec_t records;
  size_t bytes, metadata;
} orm_sql_index_set;
/* schema/table_id/snapshot must come from this owner's current Catalog lookup.
 * Load this table's complete directory with fixed capacity and checked names,
 * schema, generations, identity bounds and duplicate IDs. No writes; source
 * handles close before return. O(I^2 + sum(K^2)), bounded PLAN/WORK/READ/STEP.
 * Output unchanged on error; corruption poisons owner. Unknown record versions
 * -> UNSUPPORTED, budgets -> LIMIT_EXCEEDED, allocations -> OUT_OF_MEMORY. */
turbodb_status_t orm_sql_index_set_load(orm_sql_catalog_store *, const orm_sql_table_schema *,
    uint64_t table_id, orm_sql_catalog_snapshot, orm_sql_index_set *, turbodb_error_t *);
turbodb_status_t orm_sql_index_set_close(orm_sql_index_set *, turbodb_error_t *);
#endif
