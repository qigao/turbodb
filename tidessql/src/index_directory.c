#include "index_directory.h"
#include "index_internal.h"
#include "work.h"
#include "error.h"
#include <string.h>

typedef struct index_directory {
  orm_sql_catalog_store *store;
  orm_tidesdb_iterator_t *iterator;
  uint8_t prefix[1 + ORM_SQL_WIRE_U64];
  bool advance;
} index_directory;
static turbodb_status_t directory_error(turbodb_error_t *error, turbodb_status_t status, const char *message) {
  tdsql_error_set(error, status, message); return status;
}
static turbodb_status_t directory_charge(orm_tidesdb_sql_budget *budget, orm_sql_budget_resource resource,
    uint64_t count, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0}; amount.value[resource] = count;
  return orm_tidesdb_sql_budget_reserve(budget, &amount, error);
}
/* Output bytes borrow iterator only until its next call. */
static turbodb_status_t directory_next(index_directory *d, vstr *name, const uint8_t **data,
    size_t *size, bool *found, turbodb_error_t *error) {
  turbodb_status_t status = directory_charge(d->store->budget, ORM_SQL_BUDGET_EXECUTION_STEPS, 1, error);
  if (status != TURBODB_STATUS_OK) return status;
  const int code = d->advance ? orm_tidesdb_iter_next(d->iterator) :
      orm_tidesdb_iter_seek(d->iterator, d->prefix, sizeof(d->prefix));
  d->advance = true;
  if (code != ORM_TDB_SUCCESS && code != ORM_TDB_ERR_NOT_FOUND)
    return orm_sql_store_native(error, code, "scan index directory");
  *found = false;
  if (code == ORM_TDB_ERR_NOT_FOUND || !orm_tidesdb_iter_valid(d->iterator)) return TURBODB_STATUS_OK;
  uint8_t *key = NULL, *value = NULL; size_t key_size = 0, value_size = 0;
  int read = orm_tidesdb_iter_key(d->iterator, &key, &key_size);
  if (read != ORM_TDB_SUCCESS) return orm_sql_store_native(error, read, "read index directory key");
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_READ_ROWS] = 1;
  amount.value[ORM_SQL_BUDGET_READ_BYTES] = key_size;
  status = orm_tidesdb_sql_budget_reserve(d->store->budget, &amount, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (!key || !key_size) return directory_error(error, TURBODB_STATUS_DATASTORE_ERROR, "empty index directory key");
  if (key[0] != INDEX_DIRECTORY_NS) return TURBODB_STATUS_OK;
  if (key_size < sizeof(d->prefix)) return directory_error(error, TURBODB_STATUS_DATASTORE_ERROR, "truncated index directory scope");
  if (memcmp(key, d->prefix, sizeof(d->prefix))) return TURBODB_STATUS_OK;
  if (key_size <= INDEX_DIRECTORY_HEADER || key_size > INDEX_DIRECTORY_HEADER + ORM_SQL_SELECT_NAME_BYTES ||
      key[INDEX_DIRECTORY_HEADER - 1] != key_size - INDEX_DIRECTORY_HEADER)
    return directory_error(error, TURBODB_STATUS_DATASTORE_ERROR, "invalid index directory name key");
  if (key_size > d->store->max_record_bytes)
    return directory_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "index directory key exceeds byte limit");
  read = orm_tidesdb_iter_value(d->iterator, &value, &value_size);
  if (read != ORM_TDB_SUCCESS) return orm_sql_store_native(error, read, "read index directory value");
  status = directory_charge(d->store->budget, ORM_SQL_BUDGET_READ_BYTES, value_size, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (!value || !value_size) return directory_error(error, TURBODB_STATUS_DATASTORE_ERROR, "empty index directory value");
  if (value_size > d->store->max_record_bytes)
    return directory_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "index directory record exceeds byte limit");
  *name = (vstr){(const char *)key + INDEX_DIRECTORY_HEADER, key_size - INDEX_DIRECTORY_HEADER};
  *data = value; *size = value_size; *found = true; return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_index_set_close(orm_sql_index_set *set, turbodb_error_t *error) {
  if (!set || !set->budget) return TURBODB_STATUS_OK;
  turbodb_status_t status = TURBODB_STATUS_OK;
  for (size_t i = 0; i < vec_size(&set->records); ++i) {
    const turbodb_status_t closed = orm_tidesdb_sql_index_record_destroy(vec_at(&set->records, i), status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = closed;
  }
  const turbodb_status_t records = orm_sql_work_release(&set->records, set->bytes, set->budget, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = records;
  const turbodb_status_t metadata = set->metadata ? orm_tidesdb_sql_budget_release(set->budget,
      ORM_SQL_BUDGET_WORK_BYTES, set->metadata, status == TURBODB_STATUS_OK ? error : NULL) : TURBODB_STATUS_OK;
  if (status == TURBODB_STATUS_OK) status = metadata;
  *set = (orm_sql_index_set){0}; return status;
}
turbodb_status_t orm_sql_index_set_load(orm_sql_catalog_store *store, const orm_sql_table_schema *schema,
    uint64_t table_id, orm_sql_catalog_snapshot snapshot, orm_sql_index_set *out, turbodb_error_t *error) {
  const uint64_t next_id = snapshot.next_id;
  if (!schema || !schema->columns || !schema->count || !table_id || table_id >= next_id ||
      !out || out->budget || out->records.initialized)
    return directory_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid index directory load arguments");
  turbodb_status_t status = orm_sql_store_ready(store, error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_index_set set = {.budget = store->budget}; index_directory d = {.store = store};
  d.prefix[0] = INDEX_DIRECTORY_NS; orm_sql_wire_write(d.prefix + 1, ORM_SQL_WIRE_U64, table_id);
  status = orm_tidesdb_sql_budget_reserve_capacity(store->budget, 1, sizeof(d) + sizeof(set), 0, &set.metadata, error);
  if (status == TURBODB_STATUS_OK) {
    const int code = orm_tidesdb_iter_new(store->transaction, store->family, &d.iterator);
    if (code != ORM_TDB_SUCCESS) status = orm_sql_store_native(error, code, "open index directory");
  }
  size_t count = 0;
  while (status == TURBODB_STATUS_OK) {
    vstr name = {0}; const uint8_t *data = NULL; size_t size = 0; bool found = false;
    status = directory_next(&d, &name, &data, &size, &found, error);
    if (status != TURBODB_STATUS_OK || !found) break;
    if (count == SIZE_MAX) { status = directory_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "index directory count overflow"); break; }
    status = directory_charge(store->budget, ORM_SQL_BUDGET_PLAN_NODES, 1, error);
    if (status == TURBODB_STATUS_OK) ++count;
  }
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&set.records, count, sizeof(orm_sql_index_record),
      _Alignof(orm_sql_index_record), store->budget, &set.bytes, error);
  d.advance = false; size_t position = 0;
  while (status == TURBODB_STATUS_OK) {
    vstr name = {0}; const uint8_t *data = NULL; size_t size = 0; bool found = false;
    status = directory_next(&d, &name, &data, &size, &found, error);
    if (status != TURBODB_STATUS_OK || !found) break;
    if (position == count) { status = directory_error(error, TURBODB_STATUS_DATASTORE_ERROR, "index directory snapshot grew"); break; }
    orm_sql_index_record *record = vec_at(&set.records, position);
    status = orm_tidesdb_sql_index_record_decode(data, size, schema, store->max_record_bytes, store->budget, record, error);
    if (status != TURBODB_STATUS_OK) break;
    if (snapshot.format < ORM_SQL_STORE_FORMAT_REAL_INDEXED && orm_sql_index_has_double(&record->definition))
      status = directory_error(error, TURBODB_STATUS_DATASTORE_ERROR, "DOUBLE index requires Catalog format v3");
    if (record->identity.table_id != table_id || record->identity.table_generation != 1 || record->identity.generation != 1 ||
        record->identity.index_id <= table_id || record->identity.index_id >= next_id ||
        record->definition.name_size != name.len || memcmp(record->definition.name, name.data, name.len))
      status = directory_error(error, TURBODB_STATUS_DATASTORE_ERROR, "index directory key, identity or generation mismatch");
    for (size_t i = 0; status == TURBODB_STATUS_OK && i < position; ++i) {
      status = directory_charge(store->budget, ORM_SQL_BUDGET_EXECUTION_STEPS, 1, error);
      if (status == TURBODB_STATUS_OK && ((const orm_sql_index_record *)vec_at_const(&set.records, i))->identity.index_id == record->identity.index_id)
        status = directory_error(error, TURBODB_STATUS_DATASTORE_ERROR, "duplicate index identity in directory");
    }
    ++position;
  }
  if (d.iterator) orm_tidesdb_iter_free(d.iterator);
  if (status == TURBODB_STATUS_OK && position != count)
    status = directory_error(error, TURBODB_STATUS_DATASTORE_ERROR, "index directory snapshot shrank");
  if (status == TURBODB_STATUS_DATASTORE_ERROR) store->failed = true;
  if (status == TURBODB_STATUS_OK) { *out = set; return status; }
  const turbodb_status_t closed = orm_sql_index_set_close(&set, NULL);
  if (closed != TURBODB_STATUS_OK) store->failed = true;
  return status;
}
