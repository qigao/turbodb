#include "index_change.h"
#include "work.h"
#include "error.h"
#include <string.h>

static turbodb_status_t delta_error(turbodb_error_t *error, turbodb_status_t status, const char *message) {
  tdsql_error_set(error, status, message); return status;
}
static turbodb_status_t delta_charge(orm_tidesdb_sql_budget *budget, size_t count, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = count;
  return orm_tidesdb_sql_budget_reserve(budget, &amount, error);
}
static int delta_key_compare(const orm_sql_store_write *x, const orm_sql_store_write *y) {
  const size_t size = x->key_size < y->key_size ? x->key_size : y->key_size;
  const int compared = memcmp(x->key, y->key, size);
  return compared ? compared : (x->key_size > y->key_size) - (x->key_size < y->key_size);
}
static int delta_compare(const void *a, const void *b) {
  const orm_sql_index_mutation *x = a, *y = b;
  const int key = delta_key_compare(&x->write, &y->write);
  return key ? key : (x->sequence > y->sequence) - (x->sequence < y->sequence);
}
static bool delta_copy(void *to, const void *from) { *(orm_sql_index_mutation *)to = *(const orm_sql_index_mutation *)from; return true; }
static void delta_move(void *to, void *from) { *(orm_sql_index_mutation *)to = *(orm_sql_index_mutation *)from; }
/* Mutations only borrow the immutable key workspace. */
static void delta_destroy(void *value) { (void)value; }
static const cmeta_type_traits delta_traits = {
  CMETA_TRAIT_COMPARE | CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY |
    CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY,
  NULL, NULL, delta_compare, delta_copy, delta_move, delta_destroy};
static const cmeta_type_desc delta_type = {"sql_index_mutation", sizeof(orm_sql_index_mutation),
  _Alignof(orm_sql_index_mutation), CMETA_T_OBJECT, NULL, &delta_traits};

static turbodb_status_t delta_dimensions(const orm_sql_index_set *set, size_t max_bytes,
    size_t *width, size_t *writes, size_t *maximum, turbodb_error_t *error) {
  *width = *writes = *maximum = 0;
  for (size_t i = 0; i < vec_size(&set->records); ++i) {
    const orm_sql_index_record *record = vec_at_const(&set->records, i); size_t tuple = 0;
    turbodb_status_t status = orm_tidesdb_sql_index_key_size(&record->definition, &tuple, error);
    if (status != TURBODB_STATUS_OK) return status;
    if (tuple > SIZE_MAX - INDEX_PREFIX_BYTES - ORM_SQL_WIRE_U64)
      return delta_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "index change key size overflow");
    const size_t key_size = INDEX_PREFIX_BYTES + tuple + ORM_SQL_WIRE_U64;
    const size_t unique_size = record->definition.unique ? key_size - ORM_SQL_WIRE_U64 : 0;
    const size_t entries = record->definition.unique ? 2 : 1;
    if (key_size > max_bytes || key_size > SIZE_MAX - *width ||
        unique_size > SIZE_MAX - *width - key_size || entries > SIZE_MAX - *writes)
      return delta_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "index change physical capacity exceeded");
    *width += key_size + unique_size; *writes += entries;
    if (key_size > *maximum) *maximum = key_size;
  }
  return TURBODB_STATUS_OK;
}
static void delta_append(orm_sql_index_delta *delta, const uint8_t *key, size_t size,
    const uint8_t *primary, orm_sql_store_operation operation) {
  const size_t sequence = delta->count++;
  *(orm_sql_index_mutation *)vec_at(&delta->mutations, sequence) = (orm_sql_index_mutation){
    {key, operation == ORM_SQL_STORE_PUT ? primary : NULL, size,
      operation == ORM_SQL_STORE_PUT ? ORM_SQL_WIRE_U64 : 0, operation}, primary, sequence};
}
static turbodb_status_t delta_row(const orm_sql_index_record *record, const orm_sql_index_change *change,
    const turbodb_value_t *row, orm_sql_store_operation operation, orm_sql_index_delta *delta,
    size_t *offset, turbodb_error_t *error) {
  size_t tuple = 0;
  turbodb_status_t status = orm_tidesdb_sql_index_key_size(&record->definition, &tuple, error);
  if (status != TURBODB_STATUS_OK) return status;
  const size_t size = INDEX_PREFIX_BYTES + tuple + ORM_SQL_WIRE_U64;
  const size_t stride = size + (record->definition.unique ? size - ORM_SQL_WIRE_U64 : 0);
  status = delta_charge(delta->budget, stride, error);
  if (status != TURBODB_STATUS_OK) return status;
  uint8_t *key = (uint8_t *)vec_data(&delta->keys) + *offset; *offset += stride;
  uint8_t *primary = key + size - ORM_SQL_WIRE_U64;
  const turbodb_value_t *pk = row + change->primary;
  if (pk->reserved || (pk->kind != TURBODB_VALUE_INT64 && pk->kind != TURBODB_VALUE_UINT64))
    return delta_error(error, TURBODB_STATUS_TYPE_ERROR, "index change requires a valid integer primary key");
  orm_sql_wire_order_write(primary, pk->kind == TURBODB_VALUE_INT64 ? orm_sql_wire_signed_order(pk->data.int64_value) : pk->data.uint64_value);
  orm_sql_index_prefix(key, INDEX_DATA_NS, &record->identity);
  bool contains_null = false;
  status = orm_tidesdb_sql_index_key_encode(&record->definition, row, change->columns,
      key + INDEX_PREFIX_BYTES, tuple, &contains_null, error);
  if (status != TURBODB_STATUS_OK) return status;
  delta_append(delta, key, size, primary, operation);
  if (record->definition.unique && !contains_null) {
    uint8_t *unique = key + size; orm_sql_index_prefix(unique, INDEX_UNIQUE_NS, &record->identity);
    memcpy(unique + INDEX_PREFIX_BYTES, key + INDEX_PREFIX_BYTES, tuple);
    delta_append(delta, unique, size - ORM_SQL_WIRE_U64, primary, operation);
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t delta_fill(const orm_sql_index_set *set, const orm_sql_index_change *change,
    orm_sql_index_delta *delta, turbodb_error_t *error) {
  size_t offset = 0; turbodb_status_t status = TURBODB_STATUS_OK;
  for (size_t r = 0; status == TURBODB_STATUS_OK && r < change->rows; ++r) {
    for (size_t i = 0; status == TURBODB_STATUS_OK && i < vec_size(&set->records); ++i) {
      const orm_sql_index_record *record = vec_at_const(&set->records, i);
      if (change->before) status = delta_row(record, change, change->before + r * change->columns,
          ORM_SQL_STORE_DELETE, delta, &offset, error);
      if (status == TURBODB_STATUS_OK && change->after) status = delta_row(record, change, change->after + r * change->columns,
          ORM_SQL_STORE_PUT, delta, &offset, error);
    }
  }
  return status;
}
static turbodb_status_t delta_validate_group(orm_sql_catalog_store *store, const orm_sql_index_mutation *mutations,
    size_t count, turbodb_error_t *error) {
  const orm_sql_store_write *key = &mutations[0].write; store_buffer current = {0};
  turbodb_status_t status = orm_sql_store_get(store, key->key, key->key_size, store->max_record_bytes, &current, error);
  if (status == TURBODB_STATUS_OK && current.found && current.size != ORM_SQL_WIRE_U64)
    status = delta_error(error, TURBODB_STATUS_DATASTORE_ERROR, "malformed index primary-key value");
  const uint8_t *primary = current.found ? current.data : NULL;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < count; ++i) {
    const orm_sql_index_mutation *m = &mutations[i];
    status = delta_charge(store->budget, ORM_SQL_WIRE_U64, error);
    if (status != TURBODB_STATUS_OK) break;
    if (m->write.operation == ORM_SQL_STORE_DELETE) {
      if (!primary || memcmp(primary, m->primary, ORM_SQL_WIRE_U64))
        status = delta_error(error, TURBODB_STATUS_DATASTORE_ERROR, "old row index is missing or points to another primary key");
      primary = NULL;
    } else {
      if (primary) status = delta_error(error, m->write.key[0] == INDEX_UNIQUE_NS ? TURBODB_STATUS_CONSTRAINT : TURBODB_STATUS_DATASTORE_ERROR,
          m->write.key[0] == INDEX_UNIQUE_NS ? "SQL unique index key is occupied" : "orphan index entry at destination primary key");
      primary = m->primary;
    }
  }
  const turbodb_status_t closed = orm_sql_store_buffer_close(store, &current, status == TURBODB_STATUS_OK ? error : NULL);
  if (closed != TURBODB_STATUS_OK) store->failed = true;
  if (status == TURBODB_STATUS_OK) status = closed;
  if (status == TURBODB_STATUS_DATASTORE_ERROR) store->failed = true;
  return status;
}
static turbodb_status_t delta_validate(orm_sql_catalog_store *store, orm_sql_index_delta *delta,
    size_t maximum, turbodb_error_t *error) {
  vec_t sorted = {0}; size_t bytes = 0;
  turbodb_status_t status = orm_sql_work_zero(&sorted, delta->count, sizeof(orm_sql_index_mutation),
      _Alignof(orm_sql_index_mutation), store->budget, &bytes, error);
  if (status == TURBODB_STATUS_OK) status = delta_charge(store->budget, delta->count, error);
  if (status == TURBODB_STATUS_OK) {
    memcpy(vec_data(&sorted), vec_data_const(&delta->mutations), delta->count * sizeof(orm_sql_index_mutation));
    status = orm_sql_work_sort(vec_data(&sorted), delta->count, &delta_type, maximum, store->budget, error);
  }
  size_t start = 0;
  while (status == TURBODB_STATUS_OK && start < delta->count) {
    const orm_sql_index_mutation *first = vec_at_const(&sorted, start); size_t end = start + 1;
    while (end < delta->count) {
      status = delta_charge(store->budget, maximum, error);
      if (status != TURBODB_STATUS_OK || delta_key_compare(&first->write, &((const orm_sql_index_mutation *)vec_at_const(&sorted, end))->write)) break;
      ++end;
    }
    if (status == TURBODB_STATUS_OK) status = delta_validate_group(store, first, end - start, error);
    start = end;
  }
  const turbodb_status_t closed = orm_sql_work_release(&sorted, bytes, store->budget, status == TURBODB_STATUS_OK ? error : NULL);
  if (closed != TURBODB_STATUS_OK) store->failed = true;
  return status == TURBODB_STATUS_OK ? closed : status;
}
turbodb_status_t orm_sql_index_delta_close(orm_sql_index_delta *delta, turbodb_error_t *error) {
  if (!delta || !delta->budget) return TURBODB_STATUS_OK;
  turbodb_status_t status = orm_sql_work_release(&delta->mutations, delta->mutation_bytes, delta->budget, error);
  const turbodb_status_t keys = orm_sql_work_release(&delta->keys, delta->key_bytes, delta->budget, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = keys;
  const turbodb_status_t metadata = delta->metadata ? orm_tidesdb_sql_budget_release(delta->budget,
      ORM_SQL_BUDGET_WORK_BYTES, delta->metadata, status == TURBODB_STATUS_OK ? error : NULL) : TURBODB_STATUS_OK;
  if (status == TURBODB_STATUS_OK) status = metadata;
  *delta = (orm_sql_index_delta){0}; return status;
}
turbodb_status_t orm_sql_index_delta_prepare(orm_sql_catalog_store *store, const orm_sql_index_set *set,
    const orm_sql_index_change *change, orm_sql_index_delta *out, turbodb_error_t *error) {
  if (!store || !set || set->budget != store->budget || !change || (!change->before && !change->after) ||
      !change->rows || !change->columns || change->primary >= change->columns || !out || out->budget || out->keys.initialized || out->mutations.initialized)
    return delta_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid index change inputs or occupied output");
  turbodb_status_t status = orm_sql_store_writable(store, error);
  if (status != TURBODB_STATUS_OK || !vec_size(&set->records)) return status;
  if (change->rows > SIZE_MAX / change->columns / sizeof(turbodb_value_t))
    return delta_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "index change row dimensions overflow");
  size_t width = 0, writes = 0, maximum = 0;
  status = delta_dimensions(set, store->max_record_bytes, &width, &writes, &maximum, error);
  if (status != TURBODB_STATUS_OK) return status;
  const size_t sides = (change->before != NULL) + (change->after != NULL);
  if (width > SIZE_MAX / sides || writes > SIZE_MAX / sides || change->rows > SIZE_MAX / (writes * sides))
    return delta_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "index change capacity overflow");
  orm_sql_index_delta delta = {.budget = store->budget};
  status = orm_tidesdb_sql_budget_reserve_capacity(store->budget, 1, sizeof(delta), 0, &delta.metadata, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&delta.keys, change->rows, width * sides, 1,
      store->budget, &delta.key_bytes, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&delta.mutations, change->rows * writes * sides,
      sizeof(orm_sql_index_mutation), _Alignof(orm_sql_index_mutation), store->budget, &delta.mutation_bytes, error);
  if (status == TURBODB_STATUS_OK) status = delta_fill(set, change, &delta, error);
  if (status == TURBODB_STATUS_OK) status = delta_validate(store, &delta, maximum, error);
  if (status == TURBODB_STATUS_OK) { *out = delta; return status; }
  const turbodb_status_t closed = orm_sql_index_delta_close(&delta, NULL);
  if (closed != TURBODB_STATUS_OK) store->failed = true;
  return status;
}

turbodb_status_t orm_sql_index_snapshot_audit(orm_sql_catalog_store *store, const orm_sql_index_set *set,
    const orm_sql_index_change *snapshot, turbodb_error_t *error) {
  if (!store || !set || set->budget != store->budget || !snapshot || !snapshot->columns ||
      (snapshot->rows && !snapshot->before))
    return delta_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid index snapshot audit");
  if (snapshot->rows > SIZE_MAX / snapshot->columns / sizeof(turbodb_value_t))
    return delta_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "index snapshot dimensions overflow");
  turbodb_status_t status = orm_sql_store_ready(store, error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < vec_size(&set->records); ++i) {
    const orm_sql_index_record *record = vec_at_const(&set->records, i); size_t tuple = 0, unique = 0;
    status = orm_tidesdb_sql_index_key_size(&record->definition, &tuple, error);
    if (status == TURBODB_STATUS_OK && (tuple > SIZE_MAX - INDEX_PREFIX_BYTES - ORM_SQL_WIRE_U64 ||
        INDEX_PREFIX_BYTES + tuple + ORM_SQL_WIRE_U64 > store->max_record_bytes))
      status = delta_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "snapshot index key exceeds capacity");
    const size_t parts = vec_size(&record->definition.parts);
    for (size_t r = 0; status == TURBODB_STATUS_OK && record->definition.unique && r < snapshot->rows; ++r) {
      status = delta_charge(store->budget, parts, error); bool nullable = false;
      const turbodb_value_t *row = snapshot->before + r * snapshot->columns;
      for (size_t p = 0; p < parts; ++p) {
        const orm_sql_index_part *part = vec_at_const(&record->definition.parts, p);
        if (row[part->column].kind == TURBODB_VALUE_NULL) { nullable = true; break; }
      }
      if (!nullable) ++unique;
    }
    const orm_sql_store_audit entries = {INDEX_DATA_NS, record->identity.index_id, record->identity.generation,
        INDEX_PREFIX_BYTES + tuple + ORM_SQL_WIRE_U64, ORM_SQL_WIRE_U64, ORM_SQL_WIRE_U64, snapshot->rows};
    const orm_sql_store_audit occupancy = {INDEX_UNIQUE_NS, record->identity.index_id, record->identity.generation,
        INDEX_PREFIX_BYTES + tuple, ORM_SQL_WIRE_U64, ORM_SQL_WIRE_U64, unique};
    if (status == TURBODB_STATUS_OK) status = orm_sql_store_audit_prefix(store, &entries, error);
    if (status == TURBODB_STATUS_OK) status = orm_sql_store_audit_prefix(store, &occupancy, error);
  }
  return status;
}
