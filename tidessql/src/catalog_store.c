#include "store_internal.h"
#include "index_directory.h"
#include "index_internal.h"
#include "name.h"
#include "wire.h"
#include "work.h"
#include "error.h"
#include <stdio.h>
#include <string.h>
#include <limits.h>

enum { STORE_MANIFEST_BYTES = ORM_SQL_STORE_MANIFEST_BYTES, STORE_EPOCH = 8, STORE_NEXT = 16,
       STORE_FORMAT = ORM_SQL_STORE_FORMAT_OFFSET, STORE_INDEXED_FORMAT = ORM_SQL_STORE_FORMAT_INDEXED,
       STORE_ENTRY_HEADER = 24, STORE_GENERATION = 8, STORE_CREATED = 16,
       STORE_VERSION_KEY_BYTES = 1 + ORM_SQL_WIRE_U64,
       STORE_NAME_KEY_BYTES = ORM_SQL_STORE_NAME_KEY_BYTES, STORE_CREATE_WRITES = 3,
       STORE_CAUSE_BYTES = TURBODB_ERROR_MESSAGE_CAPACITY / 2 };
static const uint8_t store_manifest_key[] = {0, 'T', 'D', 'B', 'M'};
static const uint8_t store_magic[] = {'T', 'D', 'B', 'R', 1, 1, 0, 0};
static const char store_savepoint[] = "catalog-private-create";
static const char store_command_savepoint[] = "catalog-private-command";
static const char store_user_prefix[] = "catalog-user:";
typedef struct store_user_savepoint { char name[ORM_SQL_SELECT_NAME_BYTES + 1]; } store_user_savepoint;
typedef enum store_savepoint_operation { STORE_SAVEPOINT_CREATE, STORE_SAVEPOINT_ROLLBACK, STORE_SAVEPOINT_RELEASE } store_savepoint_operation;
typedef struct store_manifest { uint64_t epoch, next; uint8_t format; } store_manifest;

static turbodb_status_t store_error(turbodb_error_t *error, turbodb_status_t status, const char *message) {
  tdsql_error_set(error, status, message); return status;
}
turbodb_status_t orm_sql_store_native(turbodb_error_t *error, int code, const char *operation) {
  char message[TURBODB_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "SQL Catalog %s: native error %d", operation, code);
  return store_error(error, code == ORM_TDB_ERR_MEMORY ? TURBODB_STATUS_OUT_OF_MEMORY :
      code == ORM_TDB_ERR_TOO_LARGE || code == ORM_TDB_ERR_MEMORY_LIMIT ? TURBODB_STATUS_LIMIT_EXCEEDED :
      code == ORM_TDB_ERR_CONFLICT || code == ORM_TDB_ERR_BUSY ? TURBODB_STATUS_BUSY :
      code == ORM_TDB_ERR_PRECONDITION ? TURBODB_STATUS_INVALID_STATE : TURBODB_STATUS_DATASTORE_ERROR, message);
}
turbodb_status_t orm_sql_store_ready(orm_sql_catalog_store *store, turbodb_error_t *error) {
  if (!store || !store->transaction || !store->budget)
    return store_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "SQL Catalog owner is required");
  if (store->failed || !store->budget->statement_active)
    return store_error(error, TURBODB_STATUS_INVALID_STATE, "SQL Catalog owner requires rollback or an active budget");
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_store_writable(orm_sql_catalog_store *store, turbodb_error_t *error) {
  const turbodb_status_t status = orm_sql_store_ready(store, error);
  if (status != TURBODB_STATUS_OK) return status;
  return store->active_sources ? store_error(error, TURBODB_STATUS_BUSY, "SQL Catalog has live relation sources") : TURBODB_STATUS_OK;
}
static turbodb_status_t store_abort_checked(orm_sql_catalog_store *store, turbodb_status_t primary,
    bool *cleanup_failed, turbodb_error_t *error) {
  turbodb_error_t cause; tdsql_error_init(&cause);
  if (error) cause = *error;
  turbodb_error_t cleanup; tdsql_error_init(&cleanup);
  const turbodb_status_t status = orm_tidesdb_sql_catalog_finish(store, false, &cleanup);
  if (status == TURBODB_STATUS_OK) return primary;
  if (cleanup_failed) *cleanup_failed = true;
  char message[TURBODB_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "%.*s; cleanup: %.*s", STORE_CAUSE_BYTES - 1, cause.message,
      STORE_CAUSE_BYTES - 16, cleanup.message);
  return store_error(error, status, message);
}
static turbodb_status_t store_abort(orm_sql_catalog_store *store, turbodb_status_t primary, turbodb_error_t *error) {
  return store_abort_checked(store, primary, NULL, error);
}
turbodb_status_t orm_sql_store_buffer_close(orm_sql_catalog_store *store, store_buffer *buffer, turbodb_error_t *error) {
  orm_tidesdb_free(buffer->data);
  const turbodb_status_t status = buffer->reserved ? orm_tidesdb_sql_budget_release(store->budget,
      ORM_SQL_BUDGET_WORK_BYTES, buffer->reserved, error) : TURBODB_STATUS_OK;
  *buffer = (store_buffer){0}; return status;
}
turbodb_status_t orm_sql_store_get(orm_sql_catalog_store *store, const uint8_t *key, size_t key_size,
    size_t capacity, store_buffer *out, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0};
  amount.value[ORM_SQL_BUDGET_READ_ROWS] = 1; amount.value[ORM_SQL_BUDGET_READ_BYTES] = key_size;
  amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(store->budget, &amount, error);
  if (status == TURBODB_STATUS_OK)
    status = orm_tidesdb_sql_budget_reserve_capacity(store->budget, capacity, 1, sizeof(*out), &out->reserved, error);
  if (status != TURBODB_STATUS_OK) return status;
  const int code = orm_tidesdb_txn_get(store->transaction, store->family, key, key_size, &out->data, &out->size);
  if (code == ORM_TDB_ERR_NOT_FOUND) return TURBODB_STATUS_OK;
  if (code != ORM_TDB_SUCCESS) return orm_sql_store_native(error, code, "read metadata");
  amount = (orm_sql_budget_amount){0}; amount.value[ORM_SQL_BUDGET_READ_BYTES] = out->size;
  status = orm_tidesdb_sql_budget_reserve(store->budget, &amount, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (out->size > capacity) return store_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL Catalog record exceeds byte limit");
  if (!out->data || !out->size) return store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL Catalog empty record");
  out->found = true; return TURBODB_STATUS_OK;
}
static void store_manifest_encode(store_manifest manifest, uint8_t data[STORE_MANIFEST_BYTES]) {
  memcpy(data, store_magic, sizeof(store_magic)); data[STORE_FORMAT] = manifest.format;
  orm_sql_wire_write(data + STORE_EPOCH, ORM_SQL_WIRE_U64, manifest.epoch);
  orm_sql_wire_write(data + STORE_NEXT, ORM_SQL_WIRE_U64, manifest.next);
}
static turbodb_status_t store_manifest_read(orm_sql_catalog_store *store, store_manifest *out, turbodb_error_t *error) {
  store_buffer buffer = {0};
  turbodb_status_t status = orm_sql_store_get(store, store_manifest_key, sizeof(store_manifest_key), STORE_MANIFEST_BYTES, &buffer, error);
  if (status == TURBODB_STATUS_OK && !buffer.found)
    status = store_error(error, TURBODB_STATUS_INVALID_STATE, "SQL Catalog is not explicitly initialized");
  if (status == TURBODB_STATUS_OK && (buffer.size != STORE_MANIFEST_BYTES || memcmp(buffer.data, store_magic, ORM_SQL_WIRE_U32)))
    status = store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL Catalog invalid Manifest");
  if (status == TURBODB_STATUS_OK && ((buffer.data[STORE_FORMAT] != ORM_SQL_STORE_FORMAT_BASE && buffer.data[STORE_FORMAT] != STORE_INDEXED_FORMAT &&
      buffer.data[STORE_FORMAT] != ORM_SQL_STORE_FORMAT_REAL_INDEXED) ||
      memcmp(buffer.data + STORE_FORMAT + 1, store_magic + STORE_FORMAT + 1, sizeof(store_magic) - STORE_FORMAT - 1)))
    status = store_error(error, TURBODB_STATUS_UNSUPPORTED, "SQL Catalog unsupported format or profile");
  if (status == TURBODB_STATUS_OK) {
    const store_manifest manifest = {orm_sql_wire_read(buffer.data + STORE_EPOCH, ORM_SQL_WIRE_U64),
        orm_sql_wire_read(buffer.data + STORE_NEXT, ORM_SQL_WIRE_U64), buffer.data[STORE_FORMAT]};
    if (manifest.epoch == UINT64_MAX || manifest.next != manifest.epoch + 1)
      status = store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL Catalog inconsistent Manifest counters");
    else *out = manifest;
  }
  const turbodb_status_t released = orm_sql_store_buffer_close(store, &buffer, status == TURBODB_STATUS_OK ? error : NULL);
  return status == TURBODB_STATUS_OK ? released : status;
}
turbodb_status_t orm_sql_store_catalog_barrier(orm_sql_catalog_store *store,
    uint8_t key[ORM_SQL_STORE_MANIFEST_KEY_BYTES], uint8_t data[ORM_SQL_STORE_MANIFEST_BYTES], turbodb_error_t *error) {
  store_manifest manifest = {0};
  turbodb_status_t status = orm_sql_store_writable(store, error);
  if (status == TURBODB_STATUS_OK) status = store_manifest_read(store, &manifest, error);
  if (status == TURBODB_STATUS_DATASTORE_ERROR) store->failed = true;
  if (status == TURBODB_STATUS_OK) { memcpy(key, store_manifest_key, sizeof(store_manifest_key)); store_manifest_encode(manifest, data); }
  return status;
}
turbodb_status_t orm_sql_store_prepare_index(orm_sql_catalog_store *store,
    orm_sql_index_publication *out, turbodb_error_t *error) {
  if (!out) return store_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "index publication output required");
  turbodb_status_t status = orm_sql_store_writable(store, error);
  if (status != TURBODB_STATUS_OK) return status;
  store_manifest manifest = {0};
  status = store_manifest_read(store, &manifest, error);
  if (status != TURBODB_STATUS_OK) {
    if (status == TURBODB_STATUS_DATASTORE_ERROR) store->failed = true;
    return status;
  }
  if (manifest.next == UINT64_MAX)
    return store_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL Catalog object ID exhausted");
  uint8_t key[STORE_VERSION_KEY_BYTES]; orm_sql_store_version_key(manifest.next, key);
  store_buffer unused = {0};
  status = orm_sql_store_get(store, key, sizeof(key), ORM_SQL_WIRE_U64, &unused, error);
  if (status == TURBODB_STATUS_OK && unused.found) {
    store->failed = true;
    status = store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "index ID already has an orphan table version");
  }
  const turbodb_status_t released = orm_sql_store_buffer_close(store, &unused, status == TURBODB_STATUS_OK ? error : NULL);
  if (released != TURBODB_STATUS_OK) store->failed = true;
  if (status == TURBODB_STATUS_OK) status = released;
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_index_publication publication = {.id = manifest.next, .previous_format = manifest.format};
  memcpy(publication.key, store_manifest_key, sizeof(store_manifest_key));
  store_manifest_encode((store_manifest){manifest.epoch + 1, manifest.next + 1,
      manifest.format < STORE_INDEXED_FORMAT ? STORE_INDEXED_FORMAT : manifest.format}, publication.data);
  *out = publication; return TURBODB_STATUS_OK;
}
static turbodb_status_t store_start(orm_tidesdb_database_t *database, orm_tidesdb_column_family_t *family,
    size_t max_record_bytes, orm_tidesdb_sql_budget *budget, orm_sql_catalog_store *out,
    bool *cleanup_failed, turbodb_error_t *error) {
  if (!database || !family || !budget || !out || out->budget || max_record_bytes < STORE_MANIFEST_BYTES)
    return store_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid SQL Catalog begin arguments");
  orm_sql_catalog_store store = {.family = family, .budget = budget, .max_record_bytes = max_record_bytes};
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve_retained_capacity(budget, 1, sizeof(store), 0, &store.metadata_bytes, error);
  int code = ORM_TDB_SUCCESS;
  if (status == TURBODB_STATUS_OK) code = orm_tidesdb_verify_sync_full(family);
  if (status == TURBODB_STATUS_OK && code == ORM_TDB_SUCCESS)
    code = orm_tidesdb_txn_begin_with_isolation(database, ORM_TDB_ISOLATION_SERIALIZABLE, &store.transaction);
  if (status == TURBODB_STATUS_OK && code != ORM_TDB_SUCCESS) status = orm_sql_store_native(error, code, "begin directory transaction");
  if (status == TURBODB_STATUS_OK) { *out = store; return TURBODB_STATUS_OK; }
  return store_abort_checked(&store, status, cleanup_failed, error);
}
turbodb_status_t orm_tidesdb_sql_catalog_begin(orm_tidesdb_database_t *database,
    orm_tidesdb_column_family_t *family, size_t max_record_bytes,
    orm_tidesdb_sql_budget *budget, orm_sql_catalog_store *out, turbodb_error_t *error) {
  return orm_sql_catalog_begin_checked(database, family, max_record_bytes, budget, out, NULL, error);
}
turbodb_status_t orm_sql_catalog_begin_checked(orm_tidesdb_database_t *database,
    orm_tidesdb_column_family_t *family, size_t max_record_bytes,
    orm_tidesdb_sql_budget *budget, orm_sql_catalog_store *out, bool *cleanup_failed, turbodb_error_t *error) {
  if (cleanup_failed) *cleanup_failed = false;
  if (!out || out->budget) return store_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "empty SQL Catalog output required");
  orm_sql_catalog_store store = {0}; store_manifest manifest;
  turbodb_status_t status = store_start(database, family, max_record_bytes, budget, &store, cleanup_failed, error);
  if (status == TURBODB_STATUS_OK) status = store_manifest_read(&store, &manifest, error);
  if (status == TURBODB_STATUS_OK) { *out = store; return TURBODB_STATUS_OK; }
  return store_abort_checked(&store, status, cleanup_failed, error);
}
turbodb_status_t orm_tidesdb_sql_catalog_finish(orm_sql_catalog_store *store, bool commit, turbodb_error_t *error) {
  if (!store || !store->budget) return TURBODB_STATUS_OK;
  if (store->active_sources) return store_error(error, TURBODB_STATUS_BUSY, "SQL Catalog has live relation sources");
  if (commit && store->failed) return store_error(error, TURBODB_STATUS_INVALID_STATE, "SQL Catalog failed owner must roll back");
  turbodb_status_t status = TURBODB_STATUS_OK;
  if (store->transaction) {
    int code = commit ? orm_tidesdb_txn_commit(store->transaction) : orm_tidesdb_txn_rollback(store->transaction);
    if (code != ORM_TDB_SUCCESS) {
      if (commit && code != ORM_TDB_ERR_CONFLICT) {
        char message[TURBODB_ERROR_MESSAGE_CAPACITY];
        (void)snprintf(message, sizeof(message), "SQL Catalog commit outcome unknown: native error %d; do not retry automatically", code);
        status = store_error(error, TURBODB_STATUS_COMMIT_UNKNOWN, message);
      } else status = orm_sql_store_native(error, code, commit ? "commit" : "rollback");
      if (commit) {
        const int cleanup = orm_tidesdb_txn_rollback(store->transaction);
        if (cleanup != ORM_TDB_SUCCESS) {
          char message[TURBODB_ERROR_MESSAGE_CAPACITY];
          (void)snprintf(message, sizeof(message), "SQL Catalog commit error %d; rollback error %d", code, cleanup);
          status = store_error(error, status, message);
        }
      }
    }
    orm_tidesdb_txn_free(store->transaction);
  }
  vec_destroy(&store->savepoints);
  if (store->savepoint_bytes) {
    const turbodb_status_t released = orm_tidesdb_sql_budget_release_retained(store->budget,
        store->savepoint_bytes, status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  const turbodb_status_t released = store->metadata_bytes ? orm_tidesdb_sql_budget_release_retained(store->budget,
      store->metadata_bytes, status == TURBODB_STATUS_OK ? error : NULL) : TURBODB_STATUS_OK;
  *store = (orm_sql_catalog_store){0}; return status == TURBODB_STATUS_OK ? released : status;
}

static turbodb_status_t store_savepoints_allocate(orm_sql_catalog_store *store, size_t limit, turbodb_error_t *error) {
  if (store->savepoints.initialized) return TURBODB_STATUS_OK;
  size_t reserved = 0; vec_t names = {0};
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve_retained_capacity(store->budget, limit,
      sizeof(store_user_savepoint), sizeof(void *) + _Alignof(store_user_savepoint) - 1, &reserved, error);
  if (status != TURBODB_STATUS_OK) return status;
  stl_status allocated = vec_init_bytes(&names, sizeof(store_user_savepoint), _Alignof(store_user_savepoint), limit);
  if (allocated == STL_OK) allocated = vec_reserve(&names, limit);
  if (allocated != STL_OK) {
    vec_destroy(&names);
    status = orm_tidesdb_sql_budget_release_retained(store->budget, reserved, error);
    if (status != TURBODB_STATUS_OK) { store->failed = true; return status; }
    return store_error(error, allocated == STL_OUT_OF_MEMORY ? TURBODB_STATUS_OUT_OF_MEMORY :
        allocated == STL_CAPACITY_EXCEEDED ? TURBODB_STATUS_LIMIT_EXCEEDED : TURBODB_STATUS_INTERNAL_ERROR,
        "SQL savepoint registry allocation failed");
  }
  store->savepoints = names; store->savepoint_bytes = reserved; store->savepoint_limit = limit;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t store_savepoint_call(orm_sql_catalog_store *store, vstr name,
    size_t limit, store_savepoint_operation operation, turbodb_error_t *error) {
  turbodb_status_t status = orm_sql_store_writable(store, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (operation == STORE_SAVEPOINT_CREATE && (!limit || limit > INT_MAX / 2 - 1 ||
      (store->savepoints.initialized && store->savepoint_limit != limit)))
    return store_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid or changed SQL savepoint capacity");
  const char *reason = NULL;
  status = orm_sql_name_validate(name, &reason);
  if (status != TURBODB_STATUS_OK) return store_error(error, status == TURBODB_STATUS_LIMIT_EXCEEDED ? status : TURBODB_STATUS_INVALID_ARGUMENT, reason);
  store_user_savepoint record = {0};
  for (size_t i = 0; i < name.len; ++i) {
    const unsigned char c = (unsigned char)name.data[i];
    record.name[i] = c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : (char)c;
  }
  const size_t count = vec_size(&store->savepoints);
  /* Cover name comparisons, Vec moves and up to two native calls before any
   * side effect. The checked capacity bound keeps this arithmetic in uint64. */
  orm_sql_budget_amount amount = {0};
  amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = (uint64_t)(count + 1) * sizeof(record.name) + count + 2;
  status = orm_tidesdb_sql_budget_reserve(store->budget, &amount, error);
  if (status != TURBODB_STATUS_OK) return status;
  size_t index = 0;
  for (; index < count; ++index) {
    const store_user_savepoint *other = vec_at_const(&store->savepoints, index);
    if (!strcmp(record.name, other->name)) break;
  }
  if (operation != STORE_SAVEPOINT_CREATE && index == count)
    return store_error(error, TURBODB_STATUS_SQL_ERROR, "SQL savepoint does not exist");
  if (operation == STORE_SAVEPOINT_CREATE) {
    if (index == count && count == limit) return store_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL savepoint capacity exceeded");
    status = store_savepoints_allocate(store, limit, error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  char native_name[sizeof(store_user_prefix) + ORM_SQL_SELECT_NAME_BYTES];
  memcpy(native_name, store_user_prefix, sizeof(store_user_prefix) - 1);
  memcpy(native_name + sizeof(store_user_prefix) - 1, record.name, name.len + 1);
  int code = ORM_TDB_SUCCESS;
  if (operation == STORE_SAVEPOINT_RELEASE || (operation == STORE_SAVEPOINT_CREATE && index < count))
    code = orm_tidesdb_txn_release_savepoint(store->transaction, native_name);
  else if (operation == STORE_SAVEPOINT_ROLLBACK)
    code = orm_tidesdb_txn_rollback_to_savepoint(store->transaction, native_name);
  if (code == ORM_TDB_SUCCESS && operation != STORE_SAVEPOINT_RELEASE)
    code = orm_tidesdb_txn_savepoint(store->transaction, native_name);
  if (code != ORM_TDB_SUCCESS) {
    store->failed = true;
    return orm_sql_store_native(error, code, "user savepoint operation failed; full rollback required");
  }
  stl_status changed = STL_OK;
  if (operation == STORE_SAVEPOINT_ROLLBACK) changed = vec_resize(&store->savepoints, index + 1);
  else {
    if (index < count) changed = vec_erase(&store->savepoints, index, NULL);
    if (changed == STL_OK && operation == STORE_SAVEPOINT_CREATE) changed = vec_push(&store->savepoints, &record);
  }
  if (changed != STL_OK) {
    store->failed = true;
    return store_error(error, TURBODB_STATUS_INTERNAL_ERROR, "SQL savepoint registry invariant failed; full rollback required");
  }
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_tidesdb_sql_catalog_savepoint(orm_sql_catalog_store *store, vstr name,
    size_t max_savepoints, turbodb_error_t *error) {
  return store_savepoint_call(store, name, max_savepoints, STORE_SAVEPOINT_CREATE, error);
}
turbodb_status_t orm_tidesdb_sql_catalog_rollback_to(orm_sql_catalog_store *store, vstr name, turbodb_error_t *error) {
  return store_savepoint_call(store, name, 0, STORE_SAVEPOINT_ROLLBACK, error);
}
turbodb_status_t orm_tidesdb_sql_catalog_release_savepoint(orm_sql_catalog_store *store, vstr name, turbodb_error_t *error) {
  return store_savepoint_call(store, name, 0, STORE_SAVEPOINT_RELEASE, error);
}
static turbodb_status_t store_write_budget(orm_sql_catalog_store *store, size_t rows, size_t bytes, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_WRITE_ROWS] = rows;
  amount.value[ORM_SQL_BUDGET_WRITE_BYTES] = bytes; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = rows;
  return orm_tidesdb_sql_budget_reserve(store->budget, &amount, error);
}
turbodb_status_t orm_sql_store_batch(orm_sql_catalog_store *store, const orm_sql_store_write *writes,
    size_t count, turbodb_error_t *error) {
  turbodb_status_t status = orm_sql_store_writable(store, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (!writes || !count) return store_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "empty SQL write batch");
  size_t bytes = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!writes[i].key || !writes[i].key_size ||
        (writes[i].operation != ORM_SQL_STORE_PUT && writes[i].operation != ORM_SQL_STORE_DELETE) ||
        (writes[i].operation == ORM_SQL_STORE_PUT && (!writes[i].value || !writes[i].value_size)) ||
        (writes[i].operation == ORM_SQL_STORE_DELETE && (writes[i].value || writes[i].value_size)))
      return store_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid SQL batch record");
    if (writes[i].key_size > SIZE_MAX - bytes || writes[i].value_size > SIZE_MAX - bytes - writes[i].key_size)
      return store_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL batch byte overflow");
    bytes += writes[i].key_size + writes[i].value_size;
  }
  status = store_write_budget(store, count, bytes, error);
  if (status != TURBODB_STATUS_OK) return status;
  int code = orm_tidesdb_txn_savepoint(store->transaction, store_savepoint);
  if (code != ORM_TDB_SUCCESS) return orm_sql_store_native(error, code, "create savepoint");
  for (size_t i = 0; i < count; ++i) {
    code = writes[i].operation == ORM_SQL_STORE_DELETE ?
        orm_tidesdb_txn_delete(store->transaction, store->family, writes[i].key, writes[i].key_size) :
        orm_tidesdb_txn_put(store->transaction, store->family, writes[i].key, writes[i].key_size,
            writes[i].value, writes[i].value_size, 0);
    if (code != ORM_TDB_SUCCESS) { status = orm_sql_store_native(error, code, "write batch"); break; }
  }
  code = status == TURBODB_STATUS_OK ? orm_tidesdb_txn_release_savepoint(store->transaction, store_savepoint) :
      orm_tidesdb_txn_rollback_to_savepoint(store->transaction, store_savepoint);
  if (code != ORM_TDB_SUCCESS) {
    turbodb_error_t cause; tdsql_error_init(&cause); if (error) cause = *error;
    char message[TURBODB_ERROR_MESSAGE_CAPACITY];
    (void)snprintf(message, sizeof(message), "SQL statement status %d (%.*s); savepoint cleanup error %d; owner requires rollback",
        (int)status, STORE_CAUSE_BYTES - 1, status == TURBODB_STATUS_OK ? "writes staged" : cause.message, code);
    store->failed = true; return store_error(error, TURBODB_STATUS_DATASTORE_ERROR, message);
  }
  return status;
}
turbodb_status_t orm_sql_store_command_begin(orm_sql_catalog_store *store, turbodb_error_t *error) {
  const turbodb_status_t status = orm_sql_store_writable(store, error);
  if (status != TURBODB_STATUS_OK) return status;
  const int code = orm_tidesdb_txn_savepoint(store->transaction, store_command_savepoint);
  return code == ORM_TDB_SUCCESS ? TURBODB_STATUS_OK :
      orm_sql_store_native(error, code, "create SQL command savepoint");
}
turbodb_status_t orm_sql_store_command_finish(orm_sql_catalog_store *store,
    turbodb_status_t status, turbodb_error_t *error) {
  if (!store || !store->transaction)
    return store_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "SQL command owner is required");
  const int code = status == TURBODB_STATUS_OK ?
      orm_tidesdb_txn_release_savepoint(store->transaction, store_command_savepoint) :
      orm_tidesdb_txn_rollback_to_savepoint(store->transaction, store_command_savepoint);
  if (code == ORM_TDB_SUCCESS) return status;
  turbodb_error_t cause; tdsql_error_init(&cause); if (error) cause = *error;
  char message[TURBODB_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message),
      "SQL command status %d (%.*s); savepoint cleanup error %d; owner requires rollback",
      (int)status, STORE_CAUSE_BYTES - 1,
      status == TURBODB_STATUS_OK ? "writes staged" : cause.message, code);
  store->failed = true;
  return store_error(error, TURBODB_STATUS_DATASTORE_ERROR, message);
}
turbodb_status_t orm_tidesdb_sql_catalog_initialize(orm_tidesdb_database_t *database,
    orm_tidesdb_column_family_t *family, size_t max_record_bytes,
    orm_tidesdb_sql_budget *budget, turbodb_error_t *error) {
  orm_sql_catalog_store store = {0}; orm_tidesdb_iterator_t *iterator = NULL;
  turbodb_status_t status = store_start(database, family, max_record_bytes, budget, &store, NULL, error);
  if (status == TURBODB_STATUS_OK) {
    orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_READ_ROWS] = 1;
    amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
    status = orm_tidesdb_sql_budget_reserve(budget, &amount, error);
  }
  if (status == TURBODB_STATUS_OK) {
    int code = orm_tidesdb_iter_new(store.transaction, family, &iterator);
    const uint8_t first_key = 0;
    if (code == ORM_TDB_SUCCESS) code = orm_tidesdb_iter_seek(iterator, &first_key, sizeof(first_key));
    if (code != ORM_TDB_SUCCESS && code != ORM_TDB_ERR_NOT_FOUND) status = orm_sql_store_native(error, code, "check empty CF");
    else if (code == ORM_TDB_SUCCESS && orm_tidesdb_iter_valid(iterator))
      status = store_error(error, TURBODB_STATUS_INVALID_STATE, "SQL Catalog bootstrap requires an empty CF");
  }
  if (iterator) orm_tidesdb_iter_free(iterator);
  if (status == TURBODB_STATUS_OK)
    status = store_write_budget(&store, 1, sizeof(store_manifest_key) + STORE_MANIFEST_BYTES, error);
  if (status == TURBODB_STATUS_OK) {
    uint8_t data[STORE_MANIFEST_BYTES]; store_manifest_encode((store_manifest){0, 1, ORM_SQL_STORE_FORMAT_BASE}, data);
    const int code = orm_tidesdb_txn_put(store.transaction, family, store_manifest_key, sizeof(store_manifest_key), data, sizeof(data), 0);
    if (code != ORM_TDB_SUCCESS) status = orm_sql_store_native(error, code, "initialize Manifest");
  }
  return status == TURBODB_STATUS_OK ? orm_tidesdb_sql_catalog_finish(&store, true, error) : store_abort(&store, status, error);
}
turbodb_status_t orm_sql_store_name_key(vstr name, uint8_t key[STORE_NAME_KEY_BYTES], turbodb_error_t *error) {
  const char *reason = NULL; const turbodb_status_t status = orm_sql_name_validate(name, &reason);
  if (status != TURBODB_STATUS_OK) return store_error(error, status, reason);
  key[0] = 1; key[1] = (uint8_t)name.len; memcpy(key + 2, name.data, name.len); return TURBODB_STATUS_OK;
}
void orm_sql_store_version_key(uint64_t id, uint8_t key[STORE_VERSION_KEY_BYTES]) {
  key[0] = 2; orm_sql_wire_write(key + 1, ORM_SQL_WIRE_U64, id);
}
static turbodb_status_t store_lookup(orm_sql_catalog_store *store, vstr name, store_manifest manifest,
    orm_sql_table_definition *out, uint64_t *table_id, uint64_t *version, bool *found, turbodb_error_t *error) {
  uint8_t key[STORE_NAME_KEY_BYTES]; store_buffer buffer = {0}, stamp = {0};
  orm_sql_table_definition definition = {0}; uint64_t id = 0, value = 0;
  turbodb_status_t status = orm_sql_store_name_key(name, key, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_store_get(store, key, 2 + name.len, store->max_record_bytes, &buffer, error);
  if (status == TURBODB_STATUS_OK && buffer.found) {
    if (buffer.size <= STORE_ENTRY_HEADER) status = store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL Catalog truncated entry");
    else {
      id = orm_sql_wire_read(buffer.data, ORM_SQL_WIRE_U64);
      const uint64_t generation = orm_sql_wire_read(buffer.data + STORE_GENERATION, ORM_SQL_WIRE_U64);
      const uint64_t epoch = orm_sql_wire_read(buffer.data + STORE_CREATED, ORM_SQL_WIRE_U64);
      if (!id || id >= manifest.next || generation != 1 || epoch != id || epoch > manifest.epoch)
        status = store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL Catalog inconsistent entry identity");
      else status = orm_tidesdb_sql_catalog_decode(buffer.data + STORE_ENTRY_HEADER, buffer.size - STORE_ENTRY_HEADER,
          store->max_record_bytes, store->budget, &definition, error);
    }
    orm_sql_table_schema schema = {0};
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_catalog_schema(&definition, &schema, error);
    if (status == TURBODB_STATUS_OK && (name.len != schema.name.len || memcmp(name.data, schema.name.data, name.len)))
      status = store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL Catalog key and table name differ");
    if (status == TURBODB_STATUS_OK) {
      uint8_t version_key[STORE_VERSION_KEY_BYTES]; orm_sql_store_version_key(id, version_key);
      status = orm_sql_store_get(store, version_key, sizeof(version_key), ORM_SQL_WIRE_U64, &stamp, error);
      if (status == TURBODB_STATUS_OK && (!stamp.found || stamp.size != ORM_SQL_WIRE_U64 ||
          !(value = orm_sql_wire_read(stamp.data, ORM_SQL_WIRE_U64))))
        status = store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL Catalog missing or invalid table version");
    }
  }
  const bool present = buffer.found;
  store_buffer *buffers[] = {&stamp, &buffer};
  for (size_t i = 0; i < sizeof(buffers) / sizeof(buffers[0]); ++i) {
    const turbodb_status_t released = orm_sql_store_buffer_close(store, buffers[i], status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  if (status == TURBODB_STATUS_OK) {
    *found = present;
    if (present) { *out = definition; *table_id = id; *version = value; }
  } else {
    const turbodb_status_t released = orm_tidesdb_sql_catalog_destroy(&definition, NULL);
    if (released != TURBODB_STATUS_OK) status = released;
  }
  return status;
}
turbodb_status_t orm_sql_store_lookup(orm_sql_catalog_store *store, vstr name,
    orm_sql_table_definition *out, uint64_t *table_id, uint64_t *version, bool *found, orm_sql_catalog_snapshot *snapshot, turbodb_error_t *error) {
  if (!out || out->budget || !table_id || !version || !found)
    return store_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid SQL Catalog lookup outputs");
  turbodb_status_t status = orm_sql_store_ready(store, error); store_manifest manifest = {0};
  if (status == TURBODB_STATUS_OK) status = store_manifest_read(store, &manifest, error);
  if (status == TURBODB_STATUS_OK) status = store_lookup(store, name, manifest, out, table_id, version, found, error);
  if (store && status == TURBODB_STATUS_DATASTORE_ERROR) store->failed = true;
  if (status == TURBODB_STATUS_OK && snapshot) *snapshot = (orm_sql_catalog_snapshot){manifest.next, manifest.format};
  return status;
}
turbodb_status_t orm_tidesdb_sql_catalog_lookup(orm_sql_catalog_store *store, vstr name,
    orm_sql_table_definition *out, uint64_t *table_id, uint64_t *version, bool *found, turbodb_error_t *error) {
  return orm_sql_store_lookup(store, name, out, table_id, version, found, NULL, error);
}
turbodb_status_t orm_tidesdb_sql_catalog_cursor_open(orm_sql_catalog_store *store,
    orm_sql_catalog_cursor *out, turbodb_error_t *error) {
  if (!out || out->owner) return store_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "empty SQL Catalog cursor required");
  turbodb_status_t status = orm_sql_store_ready(store, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (store->active_sources == SIZE_MAX) return store_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL Catalog source count overflow");
  orm_sql_catalog_cursor cursor = {0}; store_manifest manifest = {0};
  status = orm_tidesdb_sql_budget_reserve_capacity(store->budget, 1, sizeof(cursor), 0, &cursor.metadata_bytes, error);
  if (status == TURBODB_STATUS_OK) status = store_manifest_read(store, &manifest, error);
  if (status != TURBODB_STATUS_OK) {
    if (status == TURBODB_STATUS_DATASTORE_ERROR) store->failed = true;
    if (cursor.metadata_bytes) {
      const turbodb_status_t released = orm_tidesdb_sql_budget_release(store->budget, ORM_SQL_BUDGET_WORK_BYTES, cursor.metadata_bytes, NULL);
      if (released != TURBODB_STATUS_OK) { store->failed = true; return released; }
    }
    return status;
  }
  cursor.owner = store; cursor.epoch = manifest.epoch; cursor.next_id = manifest.next;
  *out = cursor; ++store->active_sources; return TURBODB_STATUS_OK;
}
static turbodb_status_t store_cursor_read(orm_sql_catalog_cursor *cursor, orm_sql_table_definition *out,
    uint64_t *table_id, uint64_t *version, bool *found, turbodb_error_t *error) {
  orm_sql_catalog_store *store = cursor->owner;
  turbodb_status_t status = orm_sql_store_ready(store, error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
  status = orm_tidesdb_sql_budget_reserve(store->budget, &amount, error);
  if (status != TURBODB_STATUS_OK) return status;
  int code = ORM_TDB_SUCCESS;
  if (!cursor->iterator) {
    const uint8_t prefix = 1;
    code = orm_tidesdb_iter_new(store->transaction, store->family, &cursor->iterator);
    if (code == ORM_TDB_SUCCESS) code = orm_tidesdb_iter_seek(cursor->iterator, &prefix, sizeof(prefix));
  } else if (cursor->advance) code = orm_tidesdb_iter_next(cursor->iterator);
  if (code != ORM_TDB_SUCCESS && code != ORM_TDB_ERR_NOT_FOUND) return orm_sql_store_native(error, code, "enumerate tables");
  if (code == ORM_TDB_ERR_NOT_FOUND || !orm_tidesdb_iter_valid(cursor->iterator)) {
    cursor->done = true; *found = false; return TURBODB_STATUS_OK;
  }
  uint8_t *key = NULL; size_t size = 0;
  code = orm_tidesdb_iter_key(cursor->iterator, &key, &size);
  if (code != ORM_TDB_SUCCESS) return orm_sql_store_native(error, code, "read Catalog key");
  if (!key || !size) return store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL Catalog iterator returned an empty key");
  if (key[0] != 1) { cursor->done = true; *found = false; return TURBODB_STATUS_OK; }
  amount = (orm_sql_budget_amount){0}; amount.value[ORM_SQL_BUDGET_READ_ROWS] = 1;
  amount.value[ORM_SQL_BUDGET_READ_BYTES] = size;
  status = orm_tidesdb_sql_budget_reserve(store->budget, &amount, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (size <= 2 || size > STORE_NAME_KEY_BYTES || key[1] != size - 2)
    return store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL Catalog invalid name key");
  const vstr name = {(const char *)key + 2, size - 2}; const char *reason = NULL;
  if (orm_sql_name_validate(name, &reason) != TURBODB_STATUS_OK)
    return store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL Catalog invalid persisted name");
  bool present = false;
  status = store_lookup(store, name, (store_manifest){cursor->epoch, cursor->next_id}, out, table_id, version, &present, error);
  if (status == TURBODB_STATUS_OK && !present) return store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL Catalog enumerated entry disappeared");
  if (status == TURBODB_STATUS_OK) { cursor->advance = true; *found = true; }
  return status;
}
turbodb_status_t orm_tidesdb_sql_catalog_cursor_next(orm_sql_catalog_cursor *cursor,
    orm_sql_table_definition *out, uint64_t *table_id, uint64_t *version, bool *found, turbodb_error_t *error) {
  if (!cursor || !cursor->owner || !out || out->budget || !table_id || !version || !found)
    return store_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid SQL Catalog cursor outputs");
  if (cursor->failure.status != TURBODB_STATUS_OK) return store_error(error, cursor->failure.status, cursor->failure.message);
  if (cursor->done) { *found = false; return TURBODB_STATUS_OK; }
  turbodb_error_t cause; tdsql_error_init(&cause);
  const turbodb_status_t status = store_cursor_read(cursor, out, table_id, version, found, &cause);
  if (status != TURBODB_STATUS_OK) {
    cursor->failure = cause;
    if (status == TURBODB_STATUS_DATASTORE_ERROR) cursor->owner->failed = true;
    tdsql_error_set(error, status, cause.message);
  }
  return status;
}
turbodb_status_t orm_tidesdb_sql_catalog_cursor_close(orm_sql_catalog_cursor *cursor, turbodb_error_t *error) {
  if (!cursor || !cursor->owner) return TURBODB_STATUS_OK;
  if (cursor->iterator) orm_tidesdb_iter_free(cursor->iterator);
  orm_sql_catalog_store *store = cursor->owner;
  const turbodb_status_t status = orm_tidesdb_sql_budget_release(store->budget, ORM_SQL_BUDGET_WORK_BYTES, cursor->metadata_bytes, error);
  --store->active_sources;
  if (status != TURBODB_STATUS_OK) store->failed = true;
  *cursor = (orm_sql_catalog_cursor){0}; return status;
}
turbodb_status_t orm_sql_store_encode_table(const orm_sql_table_definition *definition, uint64_t id,
    size_t max_bytes, vec_t *out, size_t *reserved, turbodb_error_t *error) {
  if (!definition || !definition->budget || !id || !out || out->initialized || !reserved || *reserved)
    return store_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid table record encoding arguments");
  if (max_bytes <= STORE_ENTRY_HEADER)
    return store_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "table record capacity exhausted");
  vec_t encoded = {0}, record = {0}; size_t encoded_work = 0, record_work = 0;
  turbodb_status_t status = orm_tidesdb_sql_catalog_encode(definition, max_bytes - STORE_ENTRY_HEADER,
      &encoded, &encoded_work, error);
  const size_t size = STORE_ENTRY_HEADER + vec_size(&encoded);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&record, size, 1, 1, definition->budget, &record_work, error);
  if (status == TURBODB_STATUS_OK) {
    uint8_t *data = vec_data(&record);
    orm_sql_wire_write(data, ORM_SQL_WIRE_U64, id);
    orm_sql_wire_write(data + STORE_GENERATION, ORM_SQL_WIRE_U64, 1);
    orm_sql_wire_write(data + STORE_CREATED, ORM_SQL_WIRE_U64, id);
    memcpy(data + STORE_ENTRY_HEADER, vec_data_const(&encoded), vec_size(&encoded));
  }
  const turbodb_status_t closed = orm_sql_work_release(&encoded, encoded_work, definition->budget, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = closed;
  if (status == TURBODB_STATUS_OK) { *out = record; *reserved = record_work; }
  else {
    const turbodb_status_t released = orm_sql_work_release(&record, record_work, definition->budget, NULL);
    if (released != TURBODB_STATUS_OK) status = released;
  }
  return status;
}
/* V1 has only Manifest, table directory, TableVersion and Data namespaces.
 * Refuse orphan/new-format data instead of overwriting a partial publication. */
turbodb_status_t orm_sql_store_unused_prefix(orm_sql_catalog_store *store, const uint8_t *prefix, size_t size, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0};
  amount.value[ORM_SQL_BUDGET_READ_ROWS] = 1;
  amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(store->budget, &amount, error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_tidesdb_iterator_t *iterator = NULL;
  int code = orm_tidesdb_iter_new(store->transaction, store->family, &iterator);
  if (code == ORM_TDB_SUCCESS) code = orm_tidesdb_iter_seek(iterator, prefix, size);
  if (code != ORM_TDB_SUCCESS && code != ORM_TDB_ERR_NOT_FOUND)
    status = orm_sql_store_native(error, code, "probe first-index namespaces");
  else if (code == ORM_TDB_SUCCESS && orm_tidesdb_iter_valid(iterator)) {
    uint8_t *key = NULL; size_t key_size = 0;
    code = orm_tidesdb_iter_key(iterator, &key, &key_size);
    if (code != ORM_TDB_SUCCESS) status = orm_sql_store_native(error, code, "probe index namespace key");
    else {
      amount = (orm_sql_budget_amount){0}; amount.value[ORM_SQL_BUDGET_READ_BYTES] = key_size;
      status = orm_tidesdb_sql_budget_reserve(store->budget, &amount, error);
      if (status == TURBODB_STATUS_OK && (size == 1 || !key || !key_size || (key_size >= size && !memcmp(key, prefix, size)))) {
        store->failed = true;
        status = store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "Catalog has orphan index namespace at allocated ID");
      }
    }
  }
  if (iterator) orm_tidesdb_iter_free(iterator);
  return status;
}
typedef struct store_create_index {
  vec_t record;
  size_t bytes, key_size;
  uint8_t key[INDEX_DIRECTORY_HEADER + ORM_SQL_SELECT_NAME_BYTES];
} store_create_index;
enum { STORE_CREATE_DATA_NS = 3 };
static turbodb_status_t store_create_indexes(orm_sql_catalog_store *store, store_manifest manifest,
    const orm_sql_table_schema *schema, const vec_t *indexes, vec_t *records, size_t *bytes, turbodb_error_t *error) {
  const size_t count=vec_size(indexes);
  turbodb_status_t status=orm_sql_work_zero(records,count,sizeof(store_create_index),_Alignof(store_create_index),store->budget,bytes,error);
  if (status==TURBODB_STATUS_OK && manifest.format==ORM_SQL_STORE_FORMAT_BASE) {
    const uint8_t prefix=INDEX_UNIQUE_NS;
    status=orm_sql_store_unused_prefix(store,&prefix,sizeof(prefix),error);
  }
  uint8_t prefix[1+ORM_SQL_WIRE_U64];
  orm_sql_wire_write(prefix+1,ORM_SQL_WIRE_U64,manifest.next);
  const uint8_t table_spaces[]={STORE_CREATE_DATA_NS,INDEX_DIRECTORY_NS};
  for (size_t i=0; status==TURBODB_STATUS_OK && i<sizeof(table_spaces); ++i) {
    prefix[0]=table_spaces[i]; status=orm_sql_store_unused_prefix(store,prefix,sizeof(prefix),error);
  }
  for (size_t i=0; status==TURBODB_STATUS_OK && i<count; ++i) {
    const orm_sql_index_definition *definition=vec_at_const(indexes,i);
    if (definition->budget!=store->budget) return store_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"CREATE index budget mismatch");
    orm_sql_index_record record={.definition=*definition,.identity={manifest.next,1,manifest.next+1+i,1}};
    store_create_index *encoded=vec_at(records,i);
    uint8_t version_key[STORE_VERSION_KEY_BYTES]; orm_sql_store_version_key(record.identity.index_id,version_key);
    store_buffer version={0};
    status=orm_sql_store_get(store,version_key,sizeof(version_key),ORM_SQL_WIRE_U64,&version,error);
    if (status==TURBODB_STATUS_OK && version.found) status=store_error(error,TURBODB_STATUS_DATASTORE_ERROR,"CREATE index ID has orphan version");
    const turbodb_status_t released=orm_sql_store_buffer_close(store,&version,status==TURBODB_STATUS_OK?error:NULL);
    if (status==TURBODB_STATUS_OK) status=released;
    orm_sql_wire_write(prefix+1,ORM_SQL_WIRE_U64,record.identity.index_id);
    for (uint8_t space=INDEX_UNIQUE_NS; status==TURBODB_STATUS_OK && space<=INDEX_DATA_NS; ++space) {
      prefix[0]=space; status=orm_sql_store_unused_prefix(store,prefix,sizeof(prefix),error);
    }
    if (status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_index_record_encode(&record,schema,store->max_record_bytes,
        &encoded->record,&encoded->bytes,error);
    size_t tuple_bytes=0;
    if (status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_index_key_size(definition,&tuple_bytes,error);
    if (status==TURBODB_STATUS_OK && (tuple_bytes>SIZE_MAX-INDEX_PREFIX_BYTES-ORM_SQL_WIRE_U64 ||
        tuple_bytes+INDEX_PREFIX_BYTES+ORM_SQL_WIRE_U64>store->max_record_bytes))
      status=store_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"CREATE index physical key exceeds record capacity");
    if (status==TURBODB_STATUS_OK) encoded->key_size=orm_sql_index_directory_key(&record,encoded->key);
  }
  if (status==TURBODB_STATUS_DATASTORE_ERROR) store->failed=true;
  return status;
}
static turbodb_status_t store_create(orm_sql_catalog_store *store,
    const orm_sql_table_definition *definition, const vec_t *indexes, uint64_t *table_id, bool *created, turbodb_error_t *error) {
  if (!store || !definition || definition->budget != store->budget || !table_id || !created)
    return store_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid SQL Catalog create arguments");
  turbodb_status_t status = orm_sql_store_writable(store, error); store_manifest manifest = {0};
  orm_sql_table_schema schema = {0}; orm_sql_table_definition existing = {0};
  uint64_t existing_id = 0, version = 0; bool found = false;
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_catalog_schema(definition, &schema, error);
  if (status == TURBODB_STATUS_OK) status = store_manifest_read(store, &manifest, error);
  if (status == TURBODB_STATUS_OK) status = store_lookup(store, schema.name, manifest, &existing, &existing_id, &version, &found, error);
  const turbodb_status_t destroyed = orm_tidesdb_sql_catalog_destroy(&existing, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = destroyed;
  if (status != TURBODB_STATUS_OK) { if (status == TURBODB_STATUS_DATASTORE_ERROR) store->failed = true; return status; }
  if (found) {
    if (!definition->if_not_exists) return store_error(error, TURBODB_STATUS_CONSTRAINT, "SQL Catalog table already exists");
    *table_id = existing_id; *created = false; return TURBODB_STATUS_OK;
  }
  const size_t count=indexes?vec_size(indexes):0;
  if (manifest.next == UINT64_MAX || count>UINT64_MAX-manifest.next-1 || count>SIZE_MAX-STORE_CREATE_WRITES ||
      store->max_record_bytes <= STORE_ENTRY_HEADER)
    return store_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL Catalog object ID or record capacity exhausted");
  uint8_t unused_key[STORE_VERSION_KEY_BYTES]; orm_sql_store_version_key(manifest.next, unused_key);
  store_buffer unused = {0};
  status = orm_sql_store_get(store, unused_key, sizeof(unused_key), ORM_SQL_WIRE_U64, &unused, error);
  if (status == TURBODB_STATUS_OK && unused.found) {
    store->failed = true;
    status = store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "SQL Catalog object ID already has an orphan table version");
  }
  const turbodb_status_t freed = orm_sql_store_buffer_close(store, &unused, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = freed;
  if (status != TURBODB_STATUS_OK) { if (status == TURBODB_STATUS_DATASTORE_ERROR) store->failed = true; return status; }
  vec_t record = {0}, index_records={0}, batch={0}; size_t record_work = 0, index_work=0, batch_work=0;
  status = orm_sql_store_encode_table(definition, manifest.next, store->max_record_bytes, &record, &record_work, error);
  if (status==TURBODB_STATUS_OK && count) status=store_create_indexes(store,manifest,&schema,indexes,&index_records,&index_work,error);
  if (status==TURBODB_STATUS_OK && count) status=orm_sql_work_zero(&batch,count+STORE_CREATE_WRITES,sizeof(orm_sql_store_write),
      _Alignof(orm_sql_store_write),store->budget,&batch_work,error);
  uint8_t key[STORE_NAME_KEY_BYTES], version_key[STORE_VERSION_KEY_BYTES], stamp[ORM_SQL_WIRE_U64], header[STORE_MANIFEST_BYTES];
  if (status == TURBODB_STATUS_OK) status = orm_sql_store_name_key(schema.name, key, error);
  if (status == TURBODB_STATUS_OK) {
    orm_sql_store_version_key(manifest.next, version_key); orm_sql_wire_write(stamp, ORM_SQL_WIRE_U64, 1);
    uint8_t format = count && manifest.format < STORE_INDEXED_FORMAT ? STORE_INDEXED_FORMAT : manifest.format;
    for (size_t i = 0; i < count; ++i)
      if (orm_sql_index_has_double(vec_at_const(indexes, i))) format = ORM_SQL_STORE_FORMAT_REAL_INDEXED;
    store_manifest_encode((store_manifest){manifest.epoch + count + 1, manifest.next + count + 1,
        format}, header);
    const orm_sql_store_write writes[] = {
      {key, vec_data_const(&record), 2 + schema.name.len, vec_size(&record)},
      {version_key, stamp, sizeof(version_key), sizeof(stamp)},
      {store_manifest_key, header, sizeof(store_manifest_key), sizeof(header)}};
    if (!count) status = orm_sql_store_batch(store, writes, STORE_CREATE_WRITES, error);
    else {
      memcpy(vec_data(&batch),writes,sizeof(writes));
      for (size_t i=0; i<count; ++i) {
        const store_create_index *index=vec_at_const(&index_records,i);
        *(orm_sql_store_write *)vec_at(&batch,STORE_CREATE_WRITES+i)=(orm_sql_store_write){
            index->key,vec_data_const(&index->record),index->key_size,vec_size(&index->record)};
      }
      status=orm_sql_store_batch(store,vec_data_const(&batch),vec_size(&batch),error);
    }
  }
  for (size_t i=0; i<vec_size(&index_records); ++i) {
    store_create_index *index=vec_at(&index_records,i);
    const turbodb_status_t released=orm_sql_work_release(&index->record,index->bytes,store->budget,status==TURBODB_STATUS_OK?error:NULL);
    if (released!=TURBODB_STATUS_OK) store->failed=true;
    if (status==TURBODB_STATUS_OK) status=released;
  }
  vec_t *vectors[] = {&record,&index_records,&batch}; const size_t work[] = {record_work,index_work,batch_work};
  for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
    const turbodb_status_t released = orm_sql_work_release(vectors[i], work[i], store->budget, status == TURBODB_STATUS_OK ? error : NULL);
    if (released != TURBODB_STATUS_OK) store->failed = true;
    if (status == TURBODB_STATUS_OK) status = released;
  }
  if (status == TURBODB_STATUS_OK) { *table_id = manifest.next; *created = true; }
  return status;
}
turbodb_status_t orm_tidesdb_sql_catalog_create(orm_sql_catalog_store *store,
    const orm_sql_table_definition *definition, uint64_t *id, bool *created, turbodb_error_t *error) {
  return store_create(store,definition,NULL,id,created,error);
}
turbodb_status_t orm_sql_store_create_all(orm_sql_catalog_store *store,
    const orm_sql_create_definition *definition, uint64_t *id, bool *created, turbodb_error_t *error) {
  if (!definition) return store_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"complete CREATE definition required");
  return store_create(store,&definition->table,&definition->indexes,id,created,error);
}

turbodb_status_t orm_sql_store_audit_prefix(orm_sql_catalog_store *store, const orm_sql_store_audit *audit, turbodb_error_t *error) {
  if (!audit || !audit->id || !audit->generation || audit->key_size < 1 + 2 * ORM_SQL_WIRE_U64 ||
      !audit->min_value_size || audit->min_value_size > audit->max_value_size)
    return store_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid physical namespace audit");
  turbodb_status_t status = orm_sql_store_ready(store, error); if (status != TURBODB_STATUS_OK) return status;
  uint8_t prefix[1 + ORM_SQL_WIRE_U64]; prefix[0] = audit->space;
  orm_sql_wire_write(prefix + 1, ORM_SQL_WIRE_U64, audit->id);
  orm_tidesdb_iterator_t *iterator = NULL;
  orm_sql_budget_amount initial = {0}; initial.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
  status = orm_tidesdb_sql_budget_reserve(store->budget, &initial, error);
  if (status != TURBODB_STATUS_OK) return status;
  int code = orm_tidesdb_iter_new(store->transaction, store->family, &iterator);
  if (code == ORM_TDB_SUCCESS) code = orm_tidesdb_iter_seek(iterator, prefix, sizeof(prefix));
  size_t count = 0;
  while (status == TURBODB_STATUS_OK && code == ORM_TDB_SUCCESS && orm_tidesdb_iter_valid(iterator)) {
    uint8_t *key = NULL, *value = NULL; size_t key_size = 0, size = 0;
    code = orm_tidesdb_iter_key(iterator, &key, &key_size);
    if (code != ORM_TDB_SUCCESS) break;
    if (!key) { status = store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "namespace audit received no key"); break; }
    if (key_size < sizeof(prefix) || memcmp(key, prefix, sizeof(prefix))) break;
    code = orm_tidesdb_iter_value(iterator, &value, &size);
    if (code != ORM_TDB_SUCCESS) break;
    if (key_size > SIZE_MAX - size) { status = store_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "namespace audit read capacity overflow"); break; }
    orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_READ_ROWS] = 1;
    amount.value[ORM_SQL_BUDGET_READ_BYTES] = key_size + size; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
    status = orm_tidesdb_sql_budget_reserve(store->budget, &amount, error);
    if (status != TURBODB_STATUS_OK) break;
    if (key_size != audit->key_size || !value || size < audit->min_value_size || size > audit->max_value_size ||
        orm_sql_wire_read(key + 1 + ORM_SQL_WIRE_U64, ORM_SQL_WIRE_U64) != audit->generation || count == audit->entries) {
      status = store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "namespace audit found orphan or malformed entries"); break;
    }
    ++count; code = orm_tidesdb_iter_next(iterator);
  }
  if (status == TURBODB_STATUS_OK && code != ORM_TDB_SUCCESS && code != ORM_TDB_ERR_NOT_FOUND)
    status = orm_sql_store_native(error, code, "audit physical namespace");
  if (status == TURBODB_STATUS_OK && count != audit->entries)
    status = store_error(error, TURBODB_STATUS_DATASTORE_ERROR, "namespace audit count mismatch");
  if (iterator) orm_tidesdb_iter_free(iterator);
  return status;
}
