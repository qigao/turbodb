#include "relation.h"
#include "index_change.h"
#include "store_internal.h"
#include "work.h"
#include "error.h"
#include <float.h>
#include <string.h>

enum { REL_HEADER = 8, REL_COUNT = 4, REL_CELL = 9, REL_KIND = 3,
       REL_TABLE_ID = 1, REL_GENERATION = 9 };
static const uint8_t relation_magic[] = {'R', 'R', 1, 0};
_Static_assert(sizeof(double) == sizeof(uint64_t) && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024,
    "relational wire requires IEEE binary64");

static turbodb_status_t relation_error(turbodb_error_t *error, turbodb_status_t status, const char *reason) {
  tdsql_error_set(error, status, reason); return status;
}
static turbodb_status_t relation_vector(vec_t *v, size_t count, size_t width, size_t alignment,
    orm_tidesdb_sql_budget *budget, size_t *bytes, turbodb_error_t *error) {
  return orm_sql_work_zero(v, count, width, alignment, budget, bytes, error);
}
static void relation_prefix(uint64_t id, uint8_t *key) {
  key[0] = REL_KIND;
  orm_sql_wire_write(key + REL_TABLE_ID, ORM_SQL_WIRE_U64, id);
  orm_sql_wire_write(key + REL_GENERATION, ORM_SQL_WIRE_U64, 1);
}
static void relation_primary(const turbodb_value_t *value, uint8_t key[ORM_SQL_WIRE_U64]) {
  uint64_t word = value->kind == TURBODB_VALUE_INT64 ?
      orm_sql_wire_signed_order(value->data.int64_value) : value->data.uint64_value;
  orm_sql_wire_order_write(key, word);
}
turbodb_status_t orm_sql_relation_row_size(size_t count, size_t max_bytes, size_t *out, turbodb_error_t *error) {
  if (!out) return relation_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "row size output required");
  if (!count || count > UINT32_MAX || count > (SIZE_MAX - REL_HEADER) / REL_CELL ||
      REL_HEADER + count * REL_CELL > max_bytes)
    return relation_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL relation row exceeds record capacity");
  *out = REL_HEADER + count * REL_CELL; return TURBODB_STATUS_OK;
}
static turbodb_status_t relation_validate(orm_sql_type type, const turbodb_value_t *value,
    orm_tidesdb_sql_budget *budget, turbodb_error_t *error) {
  orm_sql_predicate predicate; turbodb_value_t ignored;
  turbodb_status_t status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL, type, NULL, &predicate, error);
  return status == TURBODB_STATUS_OK ? orm_tidesdb_sql_predicate_eval(&predicate, value, NULL, budget, &ignored, error) : status;
}
static void relation_encode(const turbodb_value_t *values, size_t count, uint8_t *data) {
  memcpy(data, relation_magic, sizeof(relation_magic)); orm_sql_wire_write(data + REL_COUNT, ORM_SQL_WIRE_U32, count);
  for (size_t i = 0; i < count; ++i) {
    uint8_t *cell = data + REL_HEADER + i * REL_CELL; uint64_t bits = 0;
    cell[0] = values[i].kind == TURBODB_VALUE_NULL;
    switch (values[i].kind) {
      case TURBODB_VALUE_INT64: bits = (uint64_t)values[i].data.int64_value; break;
      case TURBODB_VALUE_UINT64: bits = values[i].data.uint64_value; break;
      case TURBODB_VALUE_DOUBLE: memcpy(&bits, &values[i].data.double_value, sizeof(bits)); break;
      default: break; /* Fully type-checked before encoding; only NULL remains. */
    }
    orm_sql_wire_write(cell + 1, ORM_SQL_WIRE_U64, bits);
  }
}
turbodb_status_t orm_sql_relation_encode_record(const orm_sql_table_schema *schema, size_t primary,
    const turbodb_value_t *values, const uint8_t prefix[ORM_SQL_RELATION_PREFIX_BYTES],
    uint8_t *data, size_t capacity, orm_tidesdb_sql_budget *budget, turbodb_error_t *error) {
  if (!schema || !schema->columns || primary >= schema->count || !values || !prefix || !data || !budget ||
      capacity <= ORM_SQL_RELATION_KEY_BYTES ||
      (values[primary].kind != TURBODB_VALUE_INT64 && values[primary].kind != TURBODB_VALUE_UINT64))
    return relation_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid row record encoding inputs");
  size_t size = 0;
  turbodb_status_t status = orm_sql_relation_row_size(schema->count, capacity - ORM_SQL_RELATION_KEY_BYTES, &size, error);
  if (status == TURBODB_STATUS_OK && size != capacity - ORM_SQL_RELATION_KEY_BYTES)
    status = relation_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "row record capacity must be exact");
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < schema->count; ++i)
    status = relation_validate(schema->columns[i].type, &values[i], budget, error);
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = schema->count;
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_reserve(budget, &amount, error);
  if (status == TURBODB_STATUS_OK) {
    memcpy(data, prefix, ORM_SQL_RELATION_PREFIX_BYTES);
    relation_primary(&values[primary], data + ORM_SQL_RELATION_PREFIX_BYTES);
    relation_encode(values, schema->count, data + ORM_SQL_RELATION_KEY_BYTES);
  }
  return status;
}
static turbodb_status_t relation_decode(const orm_sql_table_schema *schema, size_t primary,
    const uint8_t *data, size_t size, const uint8_t *key, size_t key_size,
    orm_tidesdb_sql_budget *budget, turbodb_value_t *out, turbodb_error_t *error) {
  if (!data || key_size != ORM_SQL_RELATION_KEY_BYTES || size != REL_HEADER + schema->count * REL_CELL ||
      memcmp(data, relation_magic, sizeof(relation_magic)) || orm_sql_wire_read(data + REL_COUNT, ORM_SQL_WIRE_U32) != schema->count)
    return relation_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL relation invalid row header or length");
  for (size_t i = 0; i < schema->count; ++i) {
    const uint8_t *cell = data + REL_HEADER + i * REL_CELL;
    const uint64_t bits = orm_sql_wire_read(cell + 1, ORM_SQL_WIRE_U64);
    if (cell[0] > 1 || (cell[0] && bits)) return relation_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL relation invalid NULL encoding");
    turbodb_value_t value = turbodb_null();
    if (!cell[0]) switch (schema->columns[i].type.kind) {
      case TURBODB_VALUE_INT64: value = turbodb_i64(bits <= INT64_MAX ? (int64_t)bits : -1 - (int64_t)(UINT64_MAX - bits)); break;
      case TURBODB_VALUE_UINT64: value = turbodb_u64(bits); break;
      case TURBODB_VALUE_DOUBLE: { double number; memcpy(&number, &bits, sizeof(number)); value = turbodb_f64(number); break; }
      default: return relation_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL relation unsupported persisted type");
    }
    turbodb_status_t status = relation_validate(schema->columns[i].type, &value, budget, error);
    if (status != TURBODB_STATUS_OK) return status == TURBODB_STATUS_TYPE_ERROR ?
        relation_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL relation persisted value violates schema") : status;
    if (i == primary) {
      uint8_t normalized[ORM_SQL_WIRE_U64]; relation_primary(&value, normalized);
      if (memcmp(normalized, key + ORM_SQL_RELATION_PREFIX_BYTES, sizeof(normalized)))
        return relation_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL relation primary key differs from stored row");
    }
    if (out) out[i] = value;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t relation_lookup(orm_sql_catalog_store *store, vstr name, orm_sql_table_definition *definition,
    orm_sql_table_schema *schema, uint64_t *id, uint64_t *version, orm_sql_catalog_snapshot *snapshot, turbodb_error_t *error) {
  bool found = false;
  turbodb_status_t status = orm_sql_store_lookup(store, name, definition, id, version, &found, snapshot, error);
  if (status == TURBODB_STATUS_OK && !found) return relation_error(error, TURBODB_STATUS_SQL_ERROR, "SQL relation table does not exist");
  return status == TURBODB_STATUS_OK ? orm_tidesdb_sql_catalog_schema(definition, schema, error) : status;
}
static turbodb_status_t relation_existing(orm_sql_catalog_store *store, const orm_sql_table_schema *schema,
    size_t primary, const uint8_t *key, bool required, turbodb_value_t *row, turbodb_error_t *error) {
  store_buffer existing = {0};
  turbodb_status_t status = orm_sql_store_get(store, key, ORM_SQL_RELATION_KEY_BYTES,
      store->max_record_bytes, &existing, error);
  if (status == TURBODB_STATUS_OK && existing.found) {
    status = relation_decode(schema, primary, existing.data, existing.size, key,
        ORM_SQL_RELATION_KEY_BYTES, store->budget, row, error);
    if (status == TURBODB_STATUS_OK && !required) status = relation_error(error, TURBODB_STATUS_CONSTRAINT, "SQL relation primary key already exists");
  }
  if (status == TURBODB_STATUS_OK && required && !existing.found)
    status = relation_error(error, TURBODB_STATUS_CONSTRAINT, "SQL relation target key does not exist");
  if (status == TURBODB_STATUS_DATASTORE_ERROR) store->failed = true;
  const turbodb_status_t freed = orm_sql_store_buffer_close(store, &existing, status == TURBODB_STATUS_OK ? error : NULL);
  return status == TURBODB_STATUS_OK ? freed : status;
}
static turbodb_status_t relation_find_key(orm_sql_catalog_store *store,
    const orm_sql_table_schema *schema, size_t primary, const uint8_t *key,
    turbodb_value_t *row, bool *found, turbodb_error_t *error) {
  store_buffer existing = {0}; bool present = false;
  turbodb_status_t status = orm_sql_store_get(store, key, ORM_SQL_RELATION_KEY_BYTES,
      store->max_record_bytes, &existing, error);
  if (status == TURBODB_STATUS_OK && existing.found) {
    status = relation_decode(schema, primary, existing.data, existing.size, key,
        ORM_SQL_RELATION_KEY_BYTES, store->budget, row, error);
    present = status == TURBODB_STATUS_OK;
  }
  if (status == TURBODB_STATUS_DATASTORE_ERROR) store->failed = true;
  const turbodb_status_t closed = orm_sql_store_buffer_close(store, &existing,
      status == TURBODB_STATUS_OK ? error : NULL);
  if (closed != TURBODB_STATUS_OK) store->failed = true;
  if (status == TURBODB_STATUS_OK) status = closed;
  if (status == TURBODB_STATUS_OK) *found = present;
  return status;
}

turbodb_status_t orm_sql_relation_insert_conflict(orm_sql_catalog_store *store,
    vstr table, const turbodb_value_t *candidate, size_t columns, turbodb_value_t *out,
    bool *found, turbodb_error_t *error) {
  if (!candidate || !columns || !out || !found)
    return relation_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid INSERT conflict lookup");
  turbodb_status_t status = orm_sql_store_writable(store, error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_table_definition definition = {0}; orm_sql_table_schema schema = {0};
  orm_sql_catalog_snapshot snapshot = {0}; orm_sql_index_set indexes = {0};
  vec_t key = {0}, expected = {0}; size_t key_bytes = 0, expected_bytes = 0;
  uint64_t table_id = 0, version = 0; size_t maximum_tuple = 0;
  bool present = false;
  status = relation_lookup(store, table, &definition, &schema, &table_id,
      &version, &snapshot, error);
  (void)version;
  if (status == TURBODB_STATUS_OK && columns != schema.count)
    status = relation_error(error, TURBODB_STATUS_TYPE_ERROR, "SQL relation column count mismatch");
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < columns; ++i)
    status = relation_validate(schema.columns[i].type, &candidate[i], store->budget, error);
  if (status == TURBODB_STATUS_OK && candidate[definition.primary_key].kind != TURBODB_VALUE_INT64 &&
      candidate[definition.primary_key].kind != TURBODB_VALUE_UINT64)
    status = relation_error(error, TURBODB_STATUS_TYPE_ERROR, "SQL relation primary key must be an integer");
  uint8_t data_key[ORM_SQL_RELATION_KEY_BYTES];
  if (status == TURBODB_STATUS_OK) {
    relation_prefix(table_id, data_key);
    relation_primary(&candidate[definition.primary_key], data_key + ORM_SQL_RELATION_PREFIX_BYTES);
    status = relation_find_key(store, &schema, definition.primary_key, data_key,
        out, &present, error);
  }
  if (status == TURBODB_STATUS_OK && !present && snapshot.format >= ORM_SQL_STORE_FORMAT_INDEXED)
    status = orm_sql_index_set_load(store, &schema, table_id, snapshot, &indexes, error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && !present && i < vec_size(&indexes.records); ++i) {
    const orm_sql_index_record *record = vec_at_const(&indexes.records, i);
    if (!record->definition.unique) continue;
    size_t tuple = 0;
    status = orm_tidesdb_sql_index_key_size(&record->definition, &tuple, error);
    if (status == TURBODB_STATUS_OK && (tuple > SIZE_MAX - INDEX_PREFIX_BYTES ||
        INDEX_PREFIX_BYTES + tuple > store->max_record_bytes))
      status = relation_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL unique key exceeds record capacity");
    if (status == TURBODB_STATUS_OK && tuple > maximum_tuple) maximum_tuple = tuple;
  }
  if (status == TURBODB_STATUS_OK && !present && maximum_tuple) {
    status = relation_vector(&key, 1, INDEX_PREFIX_BYTES + maximum_tuple, 1,
        store->budget, &key_bytes, error);
    if (status == TURBODB_STATUS_OK) status = relation_vector(&expected, 1,
        maximum_tuple, 1, store->budget, &expected_bytes, error);
  }
  for (size_t i = 0; status == TURBODB_STATUS_OK && !present && i < vec_size(&indexes.records); ++i) {
    const orm_sql_index_record *record = vec_at_const(&indexes.records, i);
    if (!record->definition.unique) continue;
    size_t tuple = 0; bool contains_null = false;
    status = orm_tidesdb_sql_index_key_size(&record->definition, &tuple, error);
    uint8_t *bytes = vec_data(&key);
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_index_key_encode(
        &record->definition, candidate, columns, bytes + INDEX_PREFIX_BYTES,
        tuple, &contains_null, error);
    if (status != TURBODB_STATUS_OK || contains_null) continue;
    orm_sql_index_prefix(bytes, INDEX_UNIQUE_NS, &record->identity);
    store_buffer owner = {0}; uint8_t primary[ORM_SQL_WIRE_U64]; bool occupied = false;
    status = orm_sql_store_get(store, bytes, INDEX_PREFIX_BYTES + tuple,
        store->max_record_bytes, &owner, error);
    if (status == TURBODB_STATUS_OK && owner.found) {
      if (owner.size != sizeof(primary))
        status = relation_error(error, TURBODB_STATUS_DATASTORE_ERROR,
            "malformed unique-index primary-key value");
      else { memcpy(primary, owner.data, sizeof(primary)); occupied = true; }
    }
    const turbodb_status_t owner_closed = orm_sql_store_buffer_close(store, &owner,
        status == TURBODB_STATUS_OK ? error : NULL);
    if (owner_closed != TURBODB_STATUS_OK) store->failed = true;
    if (status == TURBODB_STATUS_OK) status = owner_closed;
    if (status != TURBODB_STATUS_OK || !occupied) continue;
    relation_prefix(table_id, data_key);
    memcpy(data_key + ORM_SQL_RELATION_PREFIX_BYTES, primary, sizeof(primary));
    bool data_found = false;
    status = relation_find_key(store, &schema, definition.primary_key, data_key,
        out, &data_found, error);
    if (status == TURBODB_STATUS_OK && !data_found)
      status = relation_error(error, TURBODB_STATUS_DATASTORE_ERROR,
          "unique index points to missing relation row");
    bool existing_null = false;
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_index_key_encode(
        &record->definition, out, columns, vec_data(&expected), tuple,
        &existing_null, error);
    if (status == TURBODB_STATUS_OK && (existing_null ||
        memcmp(vec_data_const(&expected), bytes + INDEX_PREFIX_BYTES, tuple)))
      status = relation_error(error, TURBODB_STATUS_DATASTORE_ERROR,
          "unique index tuple differs from relation row");
    if (status == TURBODB_STATUS_OK) present = true;
  }
  if (status == TURBODB_STATUS_DATASTORE_ERROR) store->failed = true;
  const turbodb_status_t key_closed = orm_sql_work_release(&key, key_bytes,
      store->budget, status == TURBODB_STATUS_OK ? error : NULL);
  if (key_closed != TURBODB_STATUS_OK) store->failed = true;
  if (status == TURBODB_STATUS_OK) status = key_closed;
  const turbodb_status_t expected_closed = orm_sql_work_release(&expected,
      expected_bytes, store->budget, status == TURBODB_STATUS_OK ? error : NULL);
  if (expected_closed != TURBODB_STATUS_OK) store->failed = true;
  if (status == TURBODB_STATUS_OK) status = expected_closed;
  const turbodb_status_t indexes_closed = orm_sql_index_set_close(&indexes,
      status == TURBODB_STATUS_OK ? error : NULL);
  if (indexes_closed != TURBODB_STATUS_OK) store->failed = true;
  if (status == TURBODB_STATUS_OK) status = indexes_closed;
  const turbodb_status_t destroyed = orm_tidesdb_sql_catalog_destroy(&definition,
      status == TURBODB_STATUS_OK ? error : NULL);
  if (destroyed != TURBODB_STATUS_OK) store->failed = true;
  if (status == TURBODB_STATUS_OK) status = destroyed;
  if (status == TURBODB_STATUS_OK) *found = present;
  return status;
}
typedef struct relation_old_key { uint64_t key; size_t position; } relation_old_key;
static int relation_old_compare(const void *a, const void *b) {
  const uint64_t x = ((const relation_old_key *)a)->key, y = ((const relation_old_key *)b)->key;
  return (x > y) - (x < y);
}
static bool relation_old_copy(void *to, const void *from) { *(relation_old_key *)to = *(const relation_old_key *)from; return true; }
static void relation_old_move(void *to, void *from) { *(relation_old_key *)to = *(relation_old_key *)from; }
/* Index entries are scalars; they own no resources. */
static void relation_old_destroy(void *value) { (void)value; }
static const cmeta_type_traits relation_old_traits = {
    CMETA_TRAIT_COMPARE | CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY |
        CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY,
    NULL, NULL, relation_old_compare, relation_old_copy, relation_old_move, relation_old_destroy};
static const cmeta_type_desc relation_old_type = {"sql_relation_old_key", sizeof(relation_old_key),
    _Alignof(relation_old_key), CMETA_T_OBJECT, NULL, &relation_old_traits};
/* Distinct final keys are checked before this lookup. A destination owned by a
 * later row is still occupied; an earlier row has already vacated it. */
static turbodb_status_t relation_destination(orm_sql_catalog_store *store, const orm_sql_table_schema *schema,
    size_t primary, const vec_t *old_index, size_t position, const uint8_t *key, turbodb_error_t *error) {
  const uint64_t word = orm_sql_wire_read(key + ORM_SQL_RELATION_PREFIX_BYTES, ORM_SQL_WIRE_U64);
  size_t first = 0, last = vec_size(old_index);
  while (first < last) {
    orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
    const turbodb_status_t status = orm_tidesdb_sql_budget_reserve(store->budget, &amount, error);
    if (status != TURBODB_STATUS_OK) return status;
    const size_t mid = first + (last - first) / 2;
    const relation_old_key *entry = vec_at_const(old_index, mid);
    if (entry->key < word) first = mid + 1; else last = mid;
  }
  if (first < vec_size(old_index)) {
    const relation_old_key *entry = vec_at_const(old_index, first);
    if (entry->key == word) return entry->position <= position ? TURBODB_STATUS_OK :
        relation_error(error, TURBODB_STATUS_CONSTRAINT, "SQL relation destination key is not yet vacant");
  }
  return relation_existing(store, schema, primary, key, false, NULL, error);
}
typedef enum relation_change { REL_INSERT, REL_UPDATE, REL_DELETE } relation_change;
static turbodb_status_t relation_write_rows(orm_sql_catalog_store *store, vstr table,
    const turbodb_value_t *values, size_t rows, size_t count, relation_change change, const turbodb_value_t *old_keys, turbodb_error_t *error) {
  if (!values || !rows || !count) return relation_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "SQL relation requires complete rows");
  enum { MOVE_WRITES = 2 };
  if (rows == SIZE_MAX || rows > SIZE_MAX / sizeof(*values) / count ||
      (old_keys && rows > (SIZE_MAX - 1) / MOVE_WRITES))
    return relation_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL relation batch size overflow");
  turbodb_status_t status = orm_sql_store_writable(store, error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_table_definition definition = {0}; orm_sql_table_schema schema = {0};
  uint64_t id = 0, version = 0; size_t size = 0, row_slots = 0, write_count = 0;
  vec_t records = {0}, descriptors = {0}, keys = {0}, old_index = {0}, before = {0}, combined = {0};
  size_t before_bytes = 0, combined_bytes = 0;
  orm_sql_catalog_snapshot snapshot = {0}; orm_sql_index_set indexes = {0}; orm_sql_index_delta delta = {0};
  size_t record_bytes = 0, descriptor_bytes = 0, key_bytes = 0, old_bytes = 0;
  const size_t key_width = ORM_SQL_RELATION_KEY_BYTES * (old_keys ? MOVE_WRITES : 1);
  const size_t write_capacity = rows * (old_keys ? MOVE_WRITES : 1) + 1;
  status = relation_lookup(store, table, &definition, &schema, &id, &version, &snapshot, error);
  if (status == TURBODB_STATUS_OK && change != REL_DELETE && count != schema.count) status = relation_error(error, TURBODB_STATUS_TYPE_ERROR, "SQL relation column count mismatch");
  if (status == TURBODB_STATUS_OK && change != REL_DELETE) status = orm_sql_relation_row_size(count, store->max_record_bytes, &size, error);
  if (status == TURBODB_STATUS_OK && (version == UINT64_MAX || size > SIZE_MAX - key_width))
    status = relation_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL relation version or record capacity exhausted");
  if (status == TURBODB_STATUS_OK) {
    orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS] = rows;
    status = orm_tidesdb_sql_budget_reserve(store->budget, &amount, error);
    if (status == TURBODB_STATUS_OK) row_slots = rows;
  }
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < rows * count; ++i)
    status = relation_validate(schema.columns[change == REL_DELETE ? definition.primary_key : i % count].type,
        &values[i], store->budget, error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && old_keys && i < rows; ++i)
    status = relation_validate(schema.columns[definition.primary_key].type, &old_keys[i], store->budget, error);
  if (status == TURBODB_STATUS_OK && snapshot.format >= ORM_SQL_STORE_FORMAT_INDEXED)
    status = orm_sql_index_set_load(store, &schema, id, snapshot, &indexes, error);
  if (status == TURBODB_STATUS_OK && vec_size(&indexes.records) && change != REL_INSERT) {
    if (schema.count > SIZE_MAX / sizeof(turbodb_value_t))
      status = relation_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL index old row width overflow");
    else status = orm_sql_work_zero(&before, rows, schema.count * sizeof(turbodb_value_t), _Alignof(turbodb_value_t),
        store->budget, &before_bytes, error);
  }
  const size_t width = status == TURBODB_STATUS_OK ? size + key_width : 0;
  if (status == TURBODB_STATUS_OK) status = relation_vector(&records, rows, width, 1, store->budget, &record_bytes, error);
  if (status == TURBODB_STATUS_OK) status = relation_vector(&descriptors, write_capacity, sizeof(orm_sql_store_write),
      _Alignof(orm_sql_store_write), store->budget, &descriptor_bytes, error);
  if (status == TURBODB_STATUS_OK) status = relation_vector(&keys, rows, sizeof(uint64_t), _Alignof(uint64_t), store->budget, &key_bytes, error);
  if (status == TURBODB_STATUS_OK && old_keys) status = relation_vector(&old_index, rows, sizeof(relation_old_key),
      _Alignof(relation_old_key), store->budget, &old_bytes, error);
  for (size_t r = 0; status == TURBODB_STATUS_OK && r < rows; ++r) {
    uint8_t *record = vec_at(&records, r); const turbodb_value_t *row = values + r * count;
    relation_prefix(id, record);
    relation_primary(&row[change == REL_DELETE ? 0 : definition.primary_key], record + ORM_SQL_RELATION_PREFIX_BYTES);
    if (change != REL_DELETE) relation_encode(row, count, record + ORM_SQL_RELATION_KEY_BYTES);
    if (old_keys) {
      uint8_t *old = record + ORM_SQL_RELATION_KEY_BYTES + size;
      relation_prefix(id, old); relation_primary(&old_keys[r], old + ORM_SQL_RELATION_PREFIX_BYTES);
      *(relation_old_key *)vec_at(&old_index, r) = (relation_old_key){
          orm_sql_wire_read(old + ORM_SQL_RELATION_PREFIX_BYTES, ORM_SQL_WIRE_U64), r};
      if (memcmp(old, record, ORM_SQL_RELATION_KEY_BYTES))
        *(orm_sql_store_write *)vec_at(&descriptors, write_count++) = (orm_sql_store_write){
            old, NULL, ORM_SQL_RELATION_KEY_BYTES, 0, ORM_SQL_STORE_DELETE};
    }
    *(orm_sql_store_write *)vec_at(&descriptors, write_count++) = (orm_sql_store_write){
        record, change == REL_DELETE ? NULL : record + ORM_SQL_RELATION_KEY_BYTES, ORM_SQL_RELATION_KEY_BYTES, size,
        change == REL_DELETE ? ORM_SQL_STORE_DELETE : ORM_SQL_STORE_PUT};
    *(uint64_t *)vec_at(&keys, r) = orm_sql_wire_read(record + ORM_SQL_RELATION_PREFIX_BYTES, ORM_SQL_WIRE_U64);
  }
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_sort_u64(vec_data(&keys), rows, store->budget, error);
  for (size_t r = 1; status == TURBODB_STATUS_OK && r < rows; ++r)
    if (*(const uint64_t *)vec_at_const(&keys, r - 1) == *(const uint64_t *)vec_at_const(&keys, r))
      status = relation_error(error, TURBODB_STATUS_CONSTRAINT, "SQL relation duplicate primary key within batch");
  if (status == TURBODB_STATUS_OK && old_keys)
    status = orm_sql_work_sort(vec_data(&old_index), rows, &relation_old_type, 1, store->budget, error);
  for (size_t r = 1; status == TURBODB_STATUS_OK && old_keys && r < rows; ++r)
    if (((const relation_old_key *)vec_at_const(&old_index, r - 1))->key ==
        ((const relation_old_key *)vec_at_const(&old_index, r))->key)
      status = relation_error(error, TURBODB_STATUS_CONSTRAINT, "SQL relation duplicate source key within batch");
  for (size_t r = 0; status == TURBODB_STATUS_OK && r < rows; ++r) {
    const uint8_t *record = vec_at_const(&records, r);
    status = relation_existing(store, &schema, definition.primary_key,
        old_keys ? record + ORM_SQL_RELATION_KEY_BYTES + size : record, change != REL_INSERT,
        before.initialized ? vec_at(&before, r) : NULL, error);
  }
  for (size_t r = 0; status == TURBODB_STATUS_OK && old_keys && r < rows; ++r)
    status = relation_destination(store, &schema, definition.primary_key, &old_index, r, vec_at_const(&records, r), error);
  if (status == TURBODB_STATUS_OK && vec_size(&indexes.records)) {
    const orm_sql_index_change index_change = {vec_data_const(&before), change == REL_DELETE ? NULL : values,
        rows, schema.count, definition.primary_key};
    status = orm_sql_index_delta_prepare(store, &indexes, &index_change, &delta, error);
  }
  if (status == TURBODB_STATUS_OK && delta.count) {
    if (delta.count > SIZE_MAX - write_count - 1)
      status = relation_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL indexed batch write count overflow");
    else status = orm_sql_work_zero(&combined, write_count + 1 + delta.count, sizeof(orm_sql_store_write),
        _Alignof(orm_sql_store_write), store->budget, &combined_bytes, error);
  }
  if (status == TURBODB_STATUS_OK) {
    uint8_t stamp_key[ORM_SQL_STORE_VERSION_KEY_BYTES], stamp[ORM_SQL_WIRE_U64];
    orm_sql_store_version_key(id, stamp_key); orm_sql_wire_write(stamp, ORM_SQL_WIRE_U64, version + 1);
    *(orm_sql_store_write *)vec_at(&descriptors, write_count++) = (orm_sql_store_write){stamp_key, stamp, sizeof(stamp_key), sizeof(stamp)};
    if (delta.count) {
      memcpy(vec_data(&combined), vec_data_const(&descriptors), write_count * sizeof(orm_sql_store_write));
      for (size_t i = 0; i < delta.count; ++i)
        *(orm_sql_store_write *)vec_at(&combined, write_count++) =
            ((const orm_sql_index_mutation *)vec_at_const(&delta.mutations, i))->write;
    }
    status = orm_sql_store_batch(store, vec_data_const(delta.count ? &combined : &descriptors), write_count, error);
  }
  vec_t *vectors[] = {&records, &descriptors, &keys, &old_index, &before, &combined};
  const size_t bytes[] = {record_bytes, descriptor_bytes, key_bytes, old_bytes, before_bytes, combined_bytes};
  for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
    const turbodb_status_t released = orm_sql_work_release(vectors[i], bytes[i], store->budget, status == TURBODB_STATUS_OK ? error : NULL);
    if (released != TURBODB_STATUS_OK) store->failed = true;
    if (status == TURBODB_STATUS_OK) status = released;
  }
  const turbodb_status_t delta_closed = orm_sql_index_delta_close(&delta, status == TURBODB_STATUS_OK ? error : NULL);
  if (delta_closed != TURBODB_STATUS_OK) store->failed = true;
  if (status == TURBODB_STATUS_OK) status = delta_closed;
  const turbodb_status_t indexes_closed = orm_sql_index_set_close(&indexes, status == TURBODB_STATUS_OK ? error : NULL);
  if (indexes_closed != TURBODB_STATUS_OK) store->failed = true;
  if (status == TURBODB_STATUS_OK) status = indexes_closed;
  if (row_slots) {
    const turbodb_status_t released = orm_tidesdb_sql_budget_release(store->budget, ORM_SQL_BUDGET_MATERIALIZED_ROWS,
        row_slots, status == TURBODB_STATUS_OK ? error : NULL);
    if (released != TURBODB_STATUS_OK) store->failed = true;
    if (status == TURBODB_STATUS_OK) status = released;
  }
  const turbodb_status_t destroyed = orm_tidesdb_sql_catalog_destroy(&definition, status == TURBODB_STATUS_OK ? error : NULL);
  if (destroyed != TURBODB_STATUS_OK) store->failed = true;
  return status == TURBODB_STATUS_OK ? destroyed : status;
}
turbodb_status_t orm_tidesdb_sql_relation_insert_rows(orm_sql_catalog_store *store, vstr table,
    const turbodb_value_t *values, size_t rows, size_t columns, turbodb_error_t *error) {
  return relation_write_rows(store, table, values, rows, columns, REL_INSERT, NULL, error);
}
turbodb_status_t orm_tidesdb_sql_relation_update_rows(orm_sql_catalog_store *store, vstr table,
    const turbodb_value_t *values, size_t rows, size_t columns, turbodb_error_t *error) {
  return relation_write_rows(store, table, values, rows, columns, REL_UPDATE, NULL, error);
}
turbodb_status_t orm_tidesdb_sql_relation_move_rows(orm_sql_catalog_store *store, vstr table,
    const turbodb_value_t *old_keys, const turbodb_value_t *values, size_t rows, size_t columns, turbodb_error_t *error) {
  if (!old_keys) return relation_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "SQL relation source keys required");
  return relation_write_rows(store, table, values, rows, columns, REL_UPDATE, old_keys, error);
}
turbodb_status_t orm_tidesdb_sql_relation_delete_rows(orm_sql_catalog_store *store, vstr table,
    const turbodb_value_t *keys, size_t rows, turbodb_error_t *error) {
  return relation_write_rows(store, table, keys, rows, 1, REL_DELETE, NULL, error);
}
turbodb_status_t orm_tidesdb_sql_relation_insert(orm_sql_catalog_store *store, vstr table,
    const turbodb_value_t *values, size_t count, turbodb_error_t *error) {
  return orm_tidesdb_sql_relation_insert_rows(store, table, values, 1, count, error);
}
static turbodb_status_t relation_read(orm_sql_relation_source *source, const turbodb_value_t **out, turbodb_error_t *error) {
  turbodb_status_t status = orm_sql_store_ready(source->owner, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (source->lookup.budget) return orm_sql_index_lookup_read(source, out, error);
  int code = ORM_TDB_SUCCESS;
  if (!source->iterator) {
    code = orm_tidesdb_iter_new(source->owner->transaction, source->owner->family, &source->iterator);
    if (code == ORM_TDB_SUCCESS) code = orm_tidesdb_iter_seek(source->iterator, source->prefix, sizeof(source->prefix));
  } else if (source->advance) code = orm_tidesdb_iter_next(source->iterator);
  if (code != ORM_TDB_SUCCESS && code != ORM_TDB_ERR_NOT_FOUND) return orm_sql_store_native(error, code, "read relation iterator");
  if (code == ORM_TDB_ERR_NOT_FOUND || !orm_tidesdb_iter_valid(source->iterator)) { source->done = true; *out = NULL; return TURBODB_STATUS_OK; }
  uint8_t *key = NULL, *data = NULL; size_t key_size = 0, size = 0;
  code = orm_tidesdb_iter_key(source->iterator, &key, &key_size);
  if (code != ORM_TDB_SUCCESS) return orm_sql_store_native(error, code, "read relation key");
  if (!key) return relation_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL relation iterator returned no key");
  if (key_size < sizeof(source->prefix) || memcmp(key, source->prefix, sizeof(source->prefix))) { source->done = true; *out = NULL; return TURBODB_STATUS_OK; }
  code = orm_tidesdb_iter_value(source->iterator, &data, &size);
  if (code != ORM_TDB_SUCCESS) return orm_sql_store_native(error, code, "read relation value");
  if (key_size > UINT64_MAX - size) return relation_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL relation read byte overflow");
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_READ_ROWS] = 1;
  amount.value[ORM_SQL_BUDGET_READ_BYTES] = key_size + size;
  status = orm_tidesdb_sql_budget_reserve(source->source.budget, &amount, error);
  if (status == TURBODB_STATUS_OK && size > source->owner->max_record_bytes) status = relation_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL relation row exceeds record capacity");
  if (status == TURBODB_STATUS_OK) status = relation_decode(&source->schema, source->definition.primary_key,
      data, size, key, key_size, source->source.budget, vec_data(&source->values), error);
  if (status == TURBODB_STATUS_OK) { source->advance = true; *out = vec_data_const(&source->values); }
  return status;
}
turbodb_status_t orm_sql_relation_index_row(orm_sql_relation_source *source, const uint8_t *primary,
    const turbodb_value_t **out, turbodb_error_t *error) {
  uint8_t key[ORM_SQL_RELATION_KEY_BYTES];
  memcpy(key, source->prefix, sizeof(source->prefix));
  memcpy(key + sizeof(source->prefix), primary, ORM_SQL_WIRE_U64);
  store_buffer row = {0};
  turbodb_status_t status = orm_sql_store_get(source->owner, key, sizeof(key), source->owner->max_record_bytes, &row, error);
  if (status == TURBODB_STATUS_OK && !row.found)
    status = relation_error(error, TURBODB_STATUS_DATASTORE_ERROR, "index points to a missing Data row");
  if (status == TURBODB_STATUS_OK) status = relation_decode(&source->schema, source->definition.primary_key,
      row.data, row.size, key, sizeof(key), source->source.budget, vec_data(&source->values), error);
  const turbodb_status_t closed = orm_sql_store_buffer_close(source->owner, &row, status == TURBODB_STATUS_OK ? error : NULL);
  if (closed != TURBODB_STATUS_OK) source->owner->failed = true;
  if (status == TURBODB_STATUS_OK) status = closed;
  if (status == TURBODB_STATUS_OK) *out = vec_data_const(&source->values);
  return status;
}
static turbodb_status_t relation_next(void *context, const turbodb_value_t **out, turbodb_error_t *error) {
  orm_sql_relation_source *source = context;
  memset(vec_data(&source->values), 0, vec_size(&source->values) * sizeof(turbodb_value_t));
  if (source->failure.status != TURBODB_STATUS_OK) { tdsql_error_set(error, source->failure.status, source->failure.message); return source->failure.status; }
  if (source->done) { *out = NULL; return TURBODB_STATUS_OK; }
  turbodb_error_t cause; tdsql_error_init(&cause); const turbodb_status_t status = relation_read(source, out, &cause);
  if (status != TURBODB_STATUS_OK) {
    memset(vec_data(&source->values), 0, vec_size(&source->values) * sizeof(turbodb_value_t));
    source->failure = cause; tdsql_error_set(error, status, cause.message);
    if (status == TURBODB_STATUS_DATASTORE_ERROR) source->owner->failed = true;
  }
  return status;
}
turbodb_status_t orm_tidesdb_sql_relation_open(orm_sql_catalog_store *store, vstr table,
    orm_sql_relation_source *out, turbodb_error_t *error) {
  if (!out || out->source.budget) return relation_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "empty SQL relation source required");
  turbodb_status_t status = orm_sql_store_ready(store, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (store->active_sources == SIZE_MAX) return relation_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL relation source count overflow");
  orm_sql_relation_source source = {.source = {.budget = store->budget, .next = relation_next}};
  uint64_t id = 0, version = 0; size_t row_size = 0; orm_sql_catalog_snapshot snapshot = {0};
  status = orm_tidesdb_sql_budget_reserve_capacity(store->budget, 1, sizeof(source), 0, &source.metadata_bytes, error);
  if (status == TURBODB_STATUS_OK) status = relation_lookup(store, table, &source.definition, &source.schema, &id, &version, &snapshot, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_relation_row_size(source.schema.count, store->max_record_bytes, &row_size, error);
  if (status == TURBODB_STATUS_OK) status = relation_vector(&source.types, source.schema.count, sizeof(orm_sql_type), _Alignof(orm_sql_type), store->budget, &source.type_bytes, error);
  if (status == TURBODB_STATUS_OK) status = relation_vector(&source.values, source.schema.count, sizeof(turbodb_value_t), _Alignof(turbodb_value_t), store->budget, &source.value_bytes, error);
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t released = orm_tidesdb_sql_relation_close(&source, NULL); return released == TURBODB_STATUS_OK ? status : released;
  }
  for (size_t i = 0; i < source.schema.count; ++i) *(orm_sql_type *)vec_at(&source.types, i) = source.schema.columns[i].type;
  relation_prefix(id, source.prefix); source.owner = store; source.source.columns = source.schema.count;
  source.format = snapshot.format; source.next_index_id = snapshot.next_id;
  *out = source; out->source.context = out; out->source.types = vec_data_const(&out->types); ++store->active_sources;
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_relation_rewind(orm_sql_relation_source *source, turbodb_error_t *error) {
  if (!source || !source->owner || !source->source.budget)
    return relation_error(error, TURBODB_STATUS_INVALID_STATE, "SQL relation source is closed");
  if (source->source.active) return relation_error(error, TURBODB_STATUS_BUSY, "SQL relation source has an active scan");
  if (source->failure.status != TURBODB_STATUS_OK)
    return relation_error(error, source->failure.status, source->failure.message);
  turbodb_status_t status = orm_sql_store_ready(source->owner, error);
  orm_sql_budget_amount charge = {0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_reserve(source->source.budget, &charge, error);
  if (status != TURBODB_STATUS_OK) return status;
  memset(vec_data(&source->values), 0, vec_size(&source->values) * sizeof(turbodb_value_t));
  if (source->iterator) orm_tidesdb_iter_free(source->iterator);
  source->iterator = NULL; source->advance = source->done = false;
  source->lookup.range_position = 0;
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_tidesdb_sql_relation_close(orm_sql_relation_source *source, turbodb_error_t *error) {
  if (!source || !source->source.budget) return TURBODB_STATUS_OK;
  if (source->source.active) return relation_error(error, TURBODB_STATUS_BUSY, "SQL relation source has an active scan");
  if (vec_size(&source->values)) memset(vec_data(&source->values), 0, vec_size(&source->values) * sizeof(turbodb_value_t));
  if (source->iterator) orm_tidesdb_iter_free(source->iterator);
  turbodb_status_t status = orm_sql_index_lookup_close(&source->lookup, error);
  const turbodb_status_t definition = orm_tidesdb_sql_catalog_destroy(&source->definition, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = definition;
  vec_t *vectors[] = {&source->types, &source->values}; const size_t bytes[] = {source->type_bytes, source->value_bytes};
  for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
    const turbodb_status_t released = orm_sql_work_release(vectors[i], bytes[i], source->source.budget, status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  if (source->metadata_bytes) {
    const turbodb_status_t released = orm_tidesdb_sql_budget_release(source->source.budget, ORM_SQL_BUDGET_WORK_BYTES,
        source->metadata_bytes, status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  if (source->owner) { --source->owner->active_sources; if (status != TURBODB_STATUS_OK) source->owner->failed = true; }
  *source = (orm_sql_relation_source){0}; return status;
}

/* Numeric Catalog rows carry no borrowed payloads. Count before allocating so
 * the fixed row matrix is admitted in full and every charged row has one owner. */
turbodb_status_t orm_sql_relation_snapshot(orm_sql_relation_source *source, vec_t *rows,
    size_t *bytes, size_t *count, turbodb_error_t *error) {
  if (!source || !source->owner || !rows || rows->initialized || !bytes || *bytes || !count || *count ||
      source->source.active || source->iterator || source->done || source->lookup.budget)
    return relation_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "whole-table snapshot requires unopened relation and empty output");
  orm_tidesdb_sql_budget *budget = source->source.budget;
  turbodb_status_t status = TURBODB_STATUS_OK;
  while (status == TURBODB_STATUS_OK) {
    const turbodb_value_t *row = NULL; status = source->source.next(source->source.context, &row, error);
    if (status != TURBODB_STATUS_OK || !row) break;
    if (*count == SIZE_MAX) return relation_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "table snapshot row count overflow");
    orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS] = 1;
    status = orm_tidesdb_sql_budget_reserve(budget, &amount, error);
    if (status == TURBODB_STATUS_OK) ++*count;
  }
  if (status != TURBODB_STATUS_OK) return status;
  if (source->schema.count > SIZE_MAX / sizeof(turbodb_value_t))
    return relation_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "table snapshot width overflow");
  const size_t width = source->schema.count * sizeof(turbodb_value_t);
  status = orm_sql_work_zero(rows, *count, width, _Alignof(turbodb_value_t), budget, bytes, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_relation_rewind(source, error);
  size_t position = 0;
  while (status == TURBODB_STATUS_OK) {
    const turbodb_value_t *row = NULL; status = source->source.next(source->source.context, &row, error);
    if (status != TURBODB_STATUS_OK || !row) break;
    if (position == *count) return relation_error(error, TURBODB_STATUS_DATASTORE_ERROR, "table snapshot grew");
    orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = source->schema.count;
    status = orm_tidesdb_sql_budget_reserve(budget, &amount, error);
    if (status == TURBODB_STATUS_OK) memcpy(vec_at(rows, position++), row, width);
  }
  if (status == TURBODB_STATUS_OK && position != *count)
    status = relation_error(error, TURBODB_STATUS_DATASTORE_ERROR, "table snapshot shrank");
  return status;
}
