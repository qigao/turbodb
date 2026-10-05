#include "index_store.h"
#include "index_record.h"
#include "relation.h"
#include "runtime.h"
#include <tinytest.h>
#include <cstl/sort.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <float.h>

static size_t put_calls, fail_put, reserves, fail_reserve, resizes, fail_resize, gets, fail_get, news, fail_new;
static size_t delete_calls, fail_delete;
static bool fail_savepoint, fail_release, fail_rollback, fail_sort;
static int fail_iterator;
enum { FAIL_SEEK = 1, FAIL_NEXT, FAIL_KEY, FAIL_VALUE };
enum { COMMIT_OK, COMMIT_BEFORE, COMMIT_AFTER };
static int commit_fault;
static stl_status probe_reserve(vec_t *v, size_t n) { return ++reserves == fail_reserve ? STL_OUT_OF_MEMORY : vec_reserve(v, n); }
static stl_status probe_resize(vec_t *v, size_t n) { return ++resizes == fail_resize ? STL_OUT_OF_MEMORY : vec_resize(v, n); }
static tstr probe_show_text(const void *data, size_t size) { return ++reserves == fail_reserve ? NULL : tstr_new_len(data, size); }
static stl_status probe_sort(void *base, size_t count, const cmeta_type_desc *type, size_t bytes) {
  return fail_sort ? STL_OUT_OF_MEMORY : stable_sort(base, count, type, bytes);
}
static int probe_put(orm_tidesdb_transaction_t *tx, orm_tidesdb_column_family_t *cf,
    const uint8_t *key, size_t key_size, const uint8_t *data, size_t size, time_t ttl) {
  return ++put_calls == fail_put ? ORM_TDB_ERR_IO : orm_tidesdb_txn_put(tx, cf, key, key_size, data, size, ttl);
}
static int probe_get(orm_tidesdb_transaction_t *tx, orm_tidesdb_column_family_t *cf,
    const uint8_t *key, size_t key_size, uint8_t **data, size_t *size) {
  return ++gets == fail_get ? ORM_TDB_ERR_IO : orm_tidesdb_txn_get(tx, cf, key, key_size, data, size);
}
static int probe_delete(orm_tidesdb_transaction_t *tx, orm_tidesdb_column_family_t *cf, const uint8_t *key, size_t size) {
  return ++delete_calls == fail_delete ? ORM_TDB_ERR_IO : orm_tidesdb_txn_delete(tx, cf, key, size);
}
static int probe_commit(orm_tidesdb_transaction_t *tx) {
  if (commit_fault == COMMIT_BEFORE) return ORM_TDB_ERR_IO;
  const int code = orm_tidesdb_txn_commit(tx);
  return code == ORM_TDB_SUCCESS && commit_fault == COMMIT_AFTER ? ORM_TDB_ERR_IO : code;
}
static int probe_savepoint(orm_tidesdb_transaction_t *tx, const char *name) {
  return fail_savepoint ? ORM_TDB_ERR_IO : orm_tidesdb_txn_savepoint(tx, name);
}
static int probe_release(orm_tidesdb_transaction_t *tx, const char *name) {
  return fail_release ? ORM_TDB_ERR_IO : orm_tidesdb_txn_release_savepoint(tx, name);
}
static int probe_rollback(orm_tidesdb_transaction_t *tx, const char *name) {
  return fail_rollback ? ORM_TDB_ERR_IO : orm_tidesdb_txn_rollback_to_savepoint(tx, name);
}
static int probe_new(orm_tidesdb_transaction_t *tx, orm_tidesdb_column_family_t *cf, orm_tidesdb_iterator_t **it) {
  return ++news == fail_new ? ORM_TDB_ERR_IO : orm_tidesdb_iter_new(tx, cf, it);
}
static int probe_seek(orm_tidesdb_iterator_t *it, const uint8_t *key, size_t size) {
  return fail_iterator == FAIL_SEEK ? ORM_TDB_ERR_IO : orm_tidesdb_iter_seek(it, key, size);
}
static int probe_next(orm_tidesdb_iterator_t *it) { return fail_iterator == FAIL_NEXT ? ORM_TDB_ERR_IO : orm_tidesdb_iter_next(it); }
static int probe_key(orm_tidesdb_iterator_t *it, uint8_t **key, size_t *size) {
  return fail_iterator == FAIL_KEY ? ORM_TDB_ERR_IO : orm_tidesdb_iter_key(it, key, size);
}
static int probe_value(orm_tidesdb_iterator_t *it, uint8_t **data, size_t *size) {
  return fail_iterator == FAIL_VALUE ? ORM_TDB_ERR_IO : orm_tidesdb_iter_value(it, data, size);
}
#define vec_reserve probe_reserve
#define vec_resize probe_resize
#define stable_sort probe_sort
#define orm_tidesdb_txn_put probe_put
#define orm_tidesdb_txn_get probe_get
#define orm_tidesdb_txn_delete probe_delete
#define orm_tidesdb_txn_commit probe_commit
#define orm_tidesdb_txn_savepoint probe_savepoint
#define orm_tidesdb_txn_release_savepoint probe_release
#define orm_tidesdb_txn_rollback_to_savepoint probe_rollback
#define orm_tidesdb_iter_new probe_new
#define orm_tidesdb_iter_seek probe_seek
#define orm_tidesdb_iter_next probe_next
#define orm_tidesdb_iter_key probe_key
#define orm_tidesdb_iter_value probe_value
#include "../../src/work.c"
#include "../../src/catalog_store.c"
#include "../../src/index_directory.c"
#include "../../src/index_change.c"
#include "../../src/relation.c"
#include "../../src/index_store.c"
#include "../../src/index_drop.c"
#include "../../src/table_clear.c"
#include "../../src/table_alter.c"
#include "../../src/index_lookup.c"
#define tstr_new_len probe_show_text
#include "../../src/show.c"
#undef tstr_new_len
#undef vec_reserve
#undef vec_resize
#undef stable_sort
#undef orm_tidesdb_txn_put
#undef orm_tidesdb_txn_get
#undef orm_tidesdb_txn_delete
#undef orm_tidesdb_txn_commit
#undef orm_tidesdb_txn_savepoint
#undef orm_tidesdb_txn_release_savepoint
#undef orm_tidesdb_txn_rollback_to_savepoint
#undef orm_tidesdb_iter_new
#undef orm_tidesdb_iter_seek
#undef orm_tidesdb_iter_next
#undef orm_tidesdb_iter_key
#undef orm_tidesdb_iter_value

enum { MAX_RECORD = 4096, WORK = 4 * 1024 * 1024, LIMIT = 1000000, COLUMNS = 3,
       ROWS = 5, OUTPUT_SENTINEL = 99, SAVEPOINTS = 4, TUPLE_BYTES = 18,
       DATA_KEY_BYTES = 43, FAULT_SWEEP_READ_LIMIT = 4 * LIMIT };
static const char family_name[] = "sql-index-build", table_name[] = "items", save_name[] = "before_index";
static const char table_sql[] = "CREATE TABLE items (id BIGINT PRIMARY KEY, a BIGINT, b BIGINT UNSIGNED)";
static const char unique_sql[] = "CREATE UNIQUE INDEX ix ON items (a ASC, b DESC)";
static const char ordinary_sql[] = "CREATE INDEX ix ON items (a ASC, b DESC)";
static const char inline_sql[] = "CREATE TABLE fresh(id BIGINT PRIMARY KEY,a BIGINT UNIQUE,b BIGINT UNSIGNED,KEY (b DESC,a ASC))";
static const char double_ddl[] = "CREATE TABLE items(id BIGINT PRIMARY KEY,a DOUBLE,b BIGINT UNSIGNED)";
static const char double_unique[] = "CREATE UNIQUE INDEX ix ON items(a DESC,b)";
static char *directory;
static orm_tidesdb_database_t *database;
static orm_tidesdb_column_family_t *family;
static orm_tidesdb_sql_budget budget;
static orm_sql_catalog_store owner, other;
static orm_sql_query lookup_query;
enum { LOOKUP_CONTEXT_BYTES = 256 };
static char lookup_sql_context[LOOKUP_CONTEXT_BYTES];
static double lookup_probe_context;
static turbodb_error_t error;
static turbodb_value_t rows[ROWS * COLUMNS];

static void faults_clear(void) {
  put_calls = fail_put = reserves = fail_reserve = resizes = fail_resize = gets = fail_get = news = fail_new = 0;
  delete_calls = fail_delete = 0;
  commit_fault = COMMIT_OK;
  fail_savepoint = fail_release = fail_rollback = fail_sort = false; fail_iterator = 0;
}
static void database_open(void) {
  orm_tidesdb_config_t config = orm_tidesdb_default_config(); config.db_path = directory;
  check_equal(orm_tidesdb_open(&config, &database), ORM_TDB_SUCCESS);
}
static void begin_owner(orm_sql_catalog_store *store) {
  check_equal(orm_tidesdb_sql_catalog_begin(database, family, MAX_RECORD, &budget, store, &error), TURBODB_STATUS_OK);
}
static void create_table(const char *sql) {
  sqlparser_document *doc = NULL; sqlparser_error diagnostic; orm_sql_table_definition definition = {0};
  check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &diagnostic), SQLPARSER_OK);
  check_equal(orm_tidesdb_sql_catalog_bind_create(doc, &budget, &definition, &error), TURBODB_STATUS_OK);
  uint64_t id = 0; bool created = false;
  check_equal(orm_tidesdb_sql_catalog_create(&owner, &definition, &id, &created, &error), TURBODB_STATUS_OK);
  check_true(created); sqlparser_document_destroy(doc);
  check_equal(orm_tidesdb_sql_catalog_destroy(&definition, &error), TURBODB_STATUS_OK);
}
static void seed(void) {
  check_equal(orm_tidesdb_sql_relation_insert_rows(&owner, vstr_from_cstr(table_name), rows, ROWS, COLUMNS, &error), TURBODB_STATUS_OK);
}
static turbodb_status_t build_on(orm_sql_catalog_store *store, const char *sql, uint64_t *id) {
  sqlparser_document *doc = NULL; sqlparser_error diagnostic;
  check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &diagnostic), SQLPARSER_OK);
  const turbodb_status_t status = orm_tidesdb_sql_index_build(doc, store, id, &error);
  sqlparser_document_destroy(doc); return status;
}
static turbodb_status_t drop_on(orm_sql_catalog_store *store, const char *sql, uint64_t *id) {
  sqlparser_document *doc = NULL; sqlparser_error diagnostic;
  check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &diagnostic), SQLPARSER_OK);
  const turbodb_status_t status = orm_tidesdb_sql_index_drop(doc, store, id, &error);
  sqlparser_document_destroy(doc); return status;
}
static turbodb_status_t clear_on(orm_sql_catalog_store *store, const char *sql) {
  sqlparser_document *doc = NULL; sqlparser_error diagnostic;
  check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &diagnostic), SQLPARSER_OK);
  const turbodb_status_t status = orm_sql_table_clear(doc, store, &error);
  sqlparser_document_destroy(doc); return status;
}
static turbodb_status_t alter_on(orm_sql_catalog_store *store, const char *sql) {
  sqlparser_document *doc = NULL; sqlparser_error diagnostic;
  check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &diagnostic), SQLPARSER_OK);
  const turbodb_status_t status = orm_sql_table_alter(doc, store, &error);
  sqlparser_document_destroy(doc); return status;
}
static const char *const column_rewrite_sql[] = {
  "ALTER TABLE items ADD extra DOUBLE", "ALTER TABLE items DROP spare",
  "ALTER TABLE items ADD extra DOUBLE NOT NULL DEFAULT (1.25*2.0) FIRST", "ALTER TABLE items ADD extra DOUBLE AFTER a"};
static const char *const column_rewrite_undo[] = {
  "ALTER TABLE items DROP extra", "ALTER TABLE items ADD spare BIGINT",
  "ALTER TABLE items DROP extra", "ALTER TABLE items DROP extra"};
enum { COLUMN_REWRITE_CASES = sizeof(column_rewrite_sql) / sizeof(column_rewrite_sql[0]) };
static void next_statement(void) {
  check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
}
static void empty_column_indexes(void) {
  uint64_t id=0;
  check_equal(build_on(&owner,"CREATE UNIQUE INDEX keep_b ON items(b)",&id),TURBODB_STATUS_OK);
  check_equal(build_on(&owner,"CREATE INDEX keep_id ON items(id)",&id),TURBODB_STATUS_OK);
}
static void table_is(bool expected, uint64_t expected_id) {
  orm_sql_table_definition table = {0}; uint64_t id = 0, version = 0; bool found = false;
  check_equal(orm_tidesdb_sql_catalog_lookup(&owner, vstr_from_cstr(table_name), &table, &id, &version, &found, &error), TURBODB_STATUS_OK);
  check_equal(found, expected); if (expected) check_equal(id, expected_id);
  check_equal(orm_tidesdb_sql_catalog_destroy(&table, &error), TURBODB_STATUS_OK);
}
static void column_default_is(size_t ordinal, bool specified, int64_t expected) {
  orm_sql_table_definition table = {0}; orm_sql_table_schema schema = {0};
  uint64_t id = 0, version = 0; bool found = false;
  check_equal(orm_tidesdb_sql_catalog_lookup(&owner, vstr_from_cstr(table_name), &table, &id, &version, &found, &error), TURBODB_STATUS_OK);
  check_true(found); check_equal(id, 1u);
  check_equal(orm_tidesdb_sql_catalog_schema(&table, &schema, &error), TURBODB_STATUS_OK);
  check_less(ordinal, schema.count); check_equal(schema.defaults[ordinal].specified, specified);
  if (specified) {
    check_equal(schema.defaults[ordinal].value.kind, TURBODB_VALUE_INT64);
    check_equal(schema.defaults[ordinal].value.data.int64_value, expected);
  }
  check_equal(orm_tidesdb_sql_catalog_destroy(&table, &error), TURBODB_STATUS_OK);
}
static void save(void) {
  check_equal(orm_tidesdb_sql_catalog_savepoint(&owner, vstr_from_cstr(save_name), SAVEPOINTS, &error), TURBODB_STATUS_OK);
}
static void restore(void) {
  check_equal(orm_tidesdb_sql_catalog_rollback_to(&owner, vstr_from_cstr(save_name), &error), TURBODB_STATUS_OK);
}
static void manifest_is(orm_tidesdb_transaction_t *tx, uint8_t format, uint64_t epoch, uint64_t next) {
  const uint8_t key[] = {0,'T','D','B','M'}; uint8_t *data = NULL; size_t size = 0;
  check_equal(orm_tidesdb_txn_get(tx, family, key, sizeof(key), &data, &size), ORM_TDB_SUCCESS);
  check_equal(size, ORM_SQL_STORE_MANIFEST_BYTES);
  const uint8_t header[] = {'T','D','B','R',format,1,0,0};
  check_equal(memcmp(data, header, sizeof(header)), 0);
  check_equal(orm_sql_wire_read(data + STORE_EPOCH, ORM_SQL_WIRE_U64), epoch);
  check_equal(orm_sql_wire_read(data + STORE_NEXT, ORM_SQL_WIRE_U64), next); orm_tidesdb_free(data);
}
static void version_is(orm_tidesdb_transaction_t *tx, uint64_t expected) {
  uint8_t key[ORM_SQL_STORE_VERSION_KEY_BYTES], *data = NULL; size_t size = 0;
  orm_sql_store_version_key(1, key);
  check_equal(orm_tidesdb_txn_get(tx, family, key, sizeof(key), &data, &size), ORM_TDB_SUCCESS);
  check_equal(size, ORM_SQL_WIRE_U64); check_equal(orm_sql_wire_read(data, size), expected); orm_tidesdb_free(data);
}
static void recover_failed_owner(void) {
  if (!owner.failed) return;
  check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
  begin_owner(&owner); seed(); save();
}
static size_t namespace_count(orm_tidesdb_transaction_t *tx, uint8_t space) {
  orm_tidesdb_iterator_t *it = NULL; size_t count = 0;
  check_equal(orm_tidesdb_iter_new(tx, family, &it), ORM_TDB_SUCCESS);
  int code = orm_tidesdb_iter_seek(it, &space, sizeof(space));
  while (code == ORM_TDB_SUCCESS && orm_tidesdb_iter_valid(it)) {
    uint8_t *key = NULL; size_t size = 0;
    check_equal(orm_tidesdb_iter_key(it, &key, &size), ORM_TDB_SUCCESS); check_true(size > 0);
    if (key[0] != space) break;
    ++count; code = orm_tidesdb_iter_next(it);
  }
  check_true(code == ORM_TDB_SUCCESS || code == ORM_TDB_ERR_NOT_FOUND);
  orm_tidesdb_iter_free(it); return count;
}
static void no_index(uint64_t version) {
  manifest_is(owner.transaction, 1, 1, 2); version_is(owner.transaction, version);
  check_equal(namespace_count(owner.transaction, INDEX_UNIQUE_NS), 0u);
  check_equal(namespace_count(owner.transaction, INDEX_DATA_NS), 0u);
  check_equal(namespace_count(owner.transaction, INDEX_DIRECTORY_NS), 0u);
  check_equal(owner.active_sources, 0u);
  check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
}
static void verify_entries(orm_tidesdb_transaction_t *tx, bool unique) {
  const uint8_t directory_key[] = {6,1,0,0,0,0,0,0,0,2,'i','x'};
  uint8_t *data = NULL; size_t size = 0;
  check_equal(orm_tidesdb_txn_get(tx, family, directory_key, sizeof(directory_key), &data, &size), ORM_TDB_SUCCESS);
  orm_sql_schema_column columns[] = {
    {vstr_from_cstr("id"), {TURBODB_VALUE_INT64, false}}, {vstr_from_cstr("a"), {TURBODB_VALUE_INT64, true}},
    {vstr_from_cstr("b"), {TURBODB_VALUE_UINT64, true}}};
  orm_sql_table_schema schema = {vstr_from_cstr(table_name), columns, COLUMNS}; orm_sql_index_record record = {0};
  check_equal(orm_tidesdb_sql_index_record_decode(data, size, &schema, MAX_RECORD, &budget, &record, &error), TURBODB_STATUS_OK);
  orm_tidesdb_free(data); check_equal(record.identity.table_id, 1u); check_equal(record.identity.index_id, 2u);
  check_equal(record.identity.table_generation, 1u); check_equal(record.identity.generation, 1u);
  check_equal(record.definition.unique, unique);
  check_equal(namespace_count(tx, INDEX_UNIQUE_NS), unique ? ROWS - 2u : 0u);
  check_equal(namespace_count(tx, INDEX_DATA_NS), ROWS); check_equal(namespace_count(tx, INDEX_DIRECTORY_NS), 1u);
  for (size_t r = 0; r < ROWS; ++r) {
    uint8_t key[DATA_KEY_BYTES] = {5,2,0,0,0,0,0,0,0,1}, tuple[TUPLE_BYTES], primary[ORM_SQL_WIRE_U64];
    bool null = false;
    check_equal(orm_tidesdb_sql_index_key_encode(&record.definition, rows + r * COLUMNS, COLUMNS,
        tuple, sizeof(tuple), &null, &error), TURBODB_STATUS_OK);
    memcpy(key + INDEX_PREFIX_BYTES, tuple, sizeof(tuple));
    orm_sql_wire_order_write(primary, orm_sql_wire_signed_order(rows[r * COLUMNS].data.int64_value));
    memcpy(key + sizeof(key) - sizeof(primary), primary, sizeof(primary));
    data = NULL; size = 0;
    check_equal(orm_tidesdb_txn_get(tx, family, key, sizeof(key), &data, &size), ORM_TDB_SUCCESS);
    check_equal(size, sizeof(primary)); check_equal(memcmp(data, primary, size), 0); orm_tidesdb_free(data);
    if (unique && !null) {
      key[0] = INDEX_UNIQUE_NS; data = NULL; size = 0;
      check_equal(orm_tidesdb_txn_get(tx, family, key, sizeof(key) - sizeof(primary), &data, &size), ORM_TDB_SUCCESS);
      check_equal(size, sizeof(primary)); check_equal(memcmp(data, primary, size), 0); orm_tidesdb_free(data);
    }
  }
  if (rows[(ROWS - 1) * COLUMNS + 2].data.uint64_value == 8) {
    /* NULLs first, then signed minimum, then descending b within a=0. */
    const size_t order[] = {2, 3, 0, 4, 1}; const uint8_t prefix = INDEX_DATA_NS;
    orm_tidesdb_iterator_t *it = NULL;
    check_equal(orm_tidesdb_iter_new(tx, family, &it), ORM_TDB_SUCCESS);
    check_equal(orm_tidesdb_iter_seek(it, &prefix, sizeof(prefix)), ORM_TDB_SUCCESS);
    for (size_t i = 0; i < ROWS; ++i) {
      uint8_t *key = NULL; size_t key_size = 0;
      check_true(orm_tidesdb_iter_valid(it));
      check_equal(orm_tidesdb_iter_key(it, &key, &key_size), ORM_TDB_SUCCESS);
      check_equal(key_size, DATA_KEY_BYTES);
      const uint64_t pk = orm_sql_wire_order_read(key + key_size - ORM_SQL_WIRE_U64);
      check_equal(orm_sql_wire_order_signed(pk), rows[order[i] * COLUMNS].data.int64_value);
      const int code = orm_tidesdb_iter_next(it); check_true(code == ORM_TDB_SUCCESS || code == ORM_TDB_ERR_NOT_FOUND);
    }
    orm_tidesdb_iter_free(it);
  }
  check_equal(orm_tidesdb_sql_index_record_destroy(&record, &error), TURBODB_STATUS_OK);
}
static void reopen_raw(orm_tidesdb_transaction_t **out) {
  check_equal(orm_tidesdb_close(database), ORM_TDB_SUCCESS); database = NULL; database_open();
  family = orm_tidesdb_get_column_family(database, family_name); check_not_null(family);
  check_equal(orm_tidesdb_txn_begin_with_isolation(database, ORM_TDB_ISOLATION_SERIALIZABLE, out), ORM_TDB_SUCCESS);
}
static void build_many(void) {
  uint64_t id = 0; check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_OK); check_equal(id, 2u);
  check_equal(build_on(&owner, "CREATE INDEX by_b ON items (b DESC, a)", &id), TURBODB_STATUS_OK); check_equal(id, 3u);
  check_equal(build_on(&owner, "CREATE UNIQUE INDEX by_id ON items (id DESC)", &id), TURBODB_STATUS_OK); check_equal(id, 4u);
}
/* Re-derive every expected entry from Data, and check exact namespace counts to
 * detect both missing entries and stale entries left by a mutation. */
static void verify_live_indexes_extra(size_t expected_rows, size_t expected_indexes,
    size_t extra_data, size_t extra_unique, size_t extra_directories) {
  orm_sql_table_definition table = {0}; orm_sql_catalog_snapshot snapshot = {0}; orm_sql_table_schema schema = {0};
  uint64_t id = 0, version = 0; bool found = false; orm_sql_index_set set = {0};
  check_equal(orm_sql_store_lookup(&owner, vstr_from_cstr(table_name), &table, &id, &version, &found, &snapshot, &error), TURBODB_STATUS_OK);
  check_true(found); check_true(snapshot.format == ORM_SQL_STORE_FORMAT_INDEXED || snapshot.format == ORM_SQL_STORE_FORMAT_REAL_INDEXED);
  check_equal(orm_tidesdb_sql_catalog_schema(&table, &schema, &error), TURBODB_STATUS_OK);
  check_equal(orm_sql_index_set_load(&owner, &schema, id, snapshot, &set, &error), TURBODB_STATUS_OK);
  check_equal(vec_size(&set.records), expected_indexes);
  orm_sql_relation_source source = {0};
  check_equal(orm_tidesdb_sql_relation_open(&owner, vstr_from_cstr(table_name), &source, &error), TURBODB_STATUS_OK);
  size_t count = 0, unique_count = 0;
  for (;;) {
    const turbodb_value_t *row = NULL;
    check_equal(source.source.next(source.source.context, &row, &error), TURBODB_STATUS_OK);
    if (!row) break;
    ++count;
    for (size_t i = 0; i < vec_size(&set.records); ++i) {
      const orm_sql_index_record *record = vec_at_const(&set.records, i); size_t tuple = 0;
      check_equal(orm_tidesdb_sql_index_key_size(&record->definition, &tuple, &error), TURBODB_STATUS_OK);
      uint8_t key[MAX_RECORD], primary[ORM_SQL_WIRE_U64], *value = NULL; size_t size = 0;
      const size_t key_size = INDEX_PREFIX_BYTES + tuple + ORM_SQL_WIRE_U64; check_true(key_size <= MAX_RECORD);
      orm_sql_index_prefix(key, INDEX_DATA_NS, &record->identity); bool null = false;
      check_equal(orm_tidesdb_sql_index_key_encode(&record->definition, row, schema.count,
          key + INDEX_PREFIX_BYTES, tuple, &null, &error), TURBODB_STATUS_OK);
      orm_sql_wire_order_write(primary, row[table.primary_key].kind == TURBODB_VALUE_INT64 ?
          orm_sql_wire_signed_order(row[table.primary_key].data.int64_value) : row[table.primary_key].data.uint64_value);
      memcpy(key + key_size - ORM_SQL_WIRE_U64, primary, sizeof(primary));
      check_equal(orm_tidesdb_txn_get(owner.transaction, family, key, key_size, &value, &size), ORM_TDB_SUCCESS);
      check_equal(size, sizeof(primary)); check_equal(memcmp(value, primary, sizeof(primary)), 0); orm_tidesdb_free(value);
      if (record->definition.unique && !null) {
        ++unique_count; key[0] = INDEX_UNIQUE_NS; value = NULL; size = 0;
        check_equal(orm_tidesdb_txn_get(owner.transaction, family, key, key_size - ORM_SQL_WIRE_U64, &value, &size), ORM_TDB_SUCCESS);
        check_equal(size, sizeof(primary)); check_equal(memcmp(value, primary, sizeof(primary)), 0); orm_tidesdb_free(value);
      }
    }
  }
  check_equal(count, expected_rows);
  check_equal(orm_tidesdb_sql_relation_close(&source, &error), TURBODB_STATUS_OK);
  check_equal(orm_sql_index_set_close(&set, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_catalog_destroy(&table, &error), TURBODB_STATUS_OK);
  check_equal(namespace_count(owner.transaction, INDEX_DATA_NS), expected_rows * expected_indexes + extra_data);
  check_equal(namespace_count(owner.transaction, INDEX_UNIQUE_NS), unique_count + extra_unique);
  check_equal(namespace_count(owner.transaction, INDEX_DIRECTORY_NS), expected_indexes + extra_directories);
}
static void verify_live_indexes(size_t expected_rows, size_t expected_indexes) {
  verify_live_indexes_extra(expected_rows, expected_indexes, 0, 0, 0);
}
static void verify_seed_rows(void) {
  orm_sql_relation_source source = {0};
  check_equal(orm_tidesdb_sql_relation_open(&owner, vstr_from_cstr(table_name), &source, &error), TURBODB_STATUS_OK);
  for (size_t r = 0; r < ROWS; ++r) {
    const turbodb_value_t *actual = NULL;
    check_equal(source.source.next(source.source.context, &actual, &error), TURBODB_STATUS_OK); check_not_null(actual);
    for (size_t c = 0; c < COLUMNS; ++c) {
      const turbodb_value_t expected = rows[r*COLUMNS+c]; check_equal(actual[c].kind, expected.kind);
      if (expected.kind == TURBODB_VALUE_INT64) check_equal(actual[c].data.int64_value, expected.data.int64_value);
      else if (expected.kind == TURBODB_VALUE_UINT64) check_equal(actual[c].data.uint64_value, expected.data.uint64_value);
    }
  }
  const turbodb_value_t *end = NULL; check_equal(source.source.next(source.source.context, &end, &error), TURBODB_STATUS_OK); check_null(end);
  check_equal(orm_tidesdb_sql_relation_close(&source, &error), TURBODB_STATUS_OK);
}
static turbodb_status_t move_one(void) {
  const turbodb_value_t old = turbodb_i64(-1), row[] = {turbodb_i64(10), turbodb_i64(2), turbodb_u64(20)};
  return orm_tidesdb_sql_relation_move_rows(&owner, vstr_from_cstr(table_name), &old, row, 1, COLUMNS, &error);
}
static turbodb_status_t execute_params(const char *sql, const turbodb_value_t *parameters, size_t count, size_t *affected) {
  sqlparser_document *doc = NULL; sqlparser_error diagnostic;
  check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &diagnostic), SQLPARSER_OK);
  enum { DEPTH = 32 };
  const turbodb_status_t status = orm_tidesdb_sql_runtime_execute(doc, &owner,
      parameters, count, DEPTH, 0, false, NULL, affected, &error);
  if (status != TURBODB_STATUS_OK) info("SQL status %d: %s", status, error.message);
  sqlparser_document_destroy(doc); return status;
}
static turbodb_status_t execute_sql(const char *sql, size_t *affected) { return execute_params(sql, NULL, 0, affected); }
static void double_seed(void) {
  size_t affected = OUTPUT_SENTINEL;
  check_equal(execute_sql("DROP TABLE items", &affected), TURBODB_STATUS_OK); create_table(double_ddl);
  const double samples[] = {-DBL_MAX, -0.0, 0.0, DBL_TRUE_MIN, DBL_MAX};
  for (size_t i = 0; i < ROWS; ++i) {
    rows[i * COLUMNS] = turbodb_i64((int64_t)i + 1);
    rows[i * COLUMNS + 1] = turbodb_f64(samples[i]); rows[i * COLUMNS + 2] = turbodb_u64(i);
  }
  seed(); manifest_is(owner.transaction, 1, 2, 3);
}
static void double_unindexed(void) {
  manifest_is(owner.transaction, 1, 2, 3);
  check_equal(namespace_count(owner.transaction, INDEX_DIRECTORY_NS), 0u);
  check_equal(namespace_count(owner.transaction, INDEX_DATA_NS), 0u);
  check_equal(namespace_count(owner.transaction, INDEX_UNIQUE_NS), 0u);
  check_equal(owner.active_sources, 0u);
}
static void fresh_absent(void) {
  orm_sql_table_definition table={0}; uint64_t id=0,version=0; bool found=true;
  check_equal(orm_tidesdb_sql_catalog_lookup(&owner,vstr_from_cstr("fresh"),&table,&id,&version,&found,&error),TURBODB_STATUS_OK);
  check_false(found); check_equal(orm_tidesdb_sql_catalog_destroy(&table,&error),TURBODB_STATUS_OK);
  manifest_is(owner.transaction,1,1,2); check_equal(namespace_count(owner.transaction,INDEX_DIRECTORY_NS),0u);
}
static turbodb_status_t lookup_open(const char *sql, const turbodb_value_t *parameters, size_t count) {
  (void)snprintf(lookup_sql_context, sizeof(lookup_sql_context), "%s", sql);
  lookup_probe_context = count && parameters[0].kind == TURBODB_VALUE_DOUBLE ? parameters[0].data.double_value : 0.0;
  sqlparser_document *doc = NULL; sqlparser_error diagnostic;
  check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &diagnostic), SQLPARSER_OK);
  enum { DEPTH = 32 };
  const turbodb_status_t status = orm_tidesdb_sql_runtime_open(doc, &owner, vstr_from_cstr("app"), parameters, count, DEPTH, &lookup_query, &error);
  if (status != TURBODB_STATUS_OK) info("lookup open status %d: %s", status, error.message);
  sqlparser_document_destroy(doc); return status;
}
static void lookup_row(int64_t expected) {
  orm_sql_scan_row row = {0}; check_equal(orm_tidesdb_sql_runtime_next(&lookup_query, &row, &error), TURBODB_STATUS_OK);
  if (row.state != ORM_SQL_SCAN_ROW)
    info("lookup SQL=%s first DOUBLE probe=%.17g missing id=%lld", lookup_sql_context, lookup_probe_context, (long long)expected);
  check_equal(row.state, ORM_SQL_SCAN_ROW); check_equal(row.values[0].kind, TURBODB_VALUE_INT64);
  if (row.values[0].data.int64_value != expected)
    info("lookup SQL=%s first DOUBLE probe=%.17g", lookup_sql_context, lookup_probe_context);
  check_equal(row.values[0].data.int64_value, expected);
}
static void lookup_end(void) {
  orm_sql_scan_row row = {0}; check_equal(orm_tidesdb_sql_runtime_next(&lookup_query, &row, &error), TURBODB_STATUS_OK);
  check_equal(row.state, ORM_SQL_SCAN_DONE); check_equal(orm_tidesdb_sql_runtime_close(&lookup_query, &error), TURBODB_STATUS_OK);
}
static turbodb_status_t lookup_measure(const sqlparser_document *doc, int64_t expected) {
  enum { DEPTH = 32 };
  orm_sql_query query = {0}; orm_sql_scan_row row = {0};
  turbodb_status_t status = orm_tidesdb_sql_runtime_open(doc, &owner, vstr_from_cstr("app"), NULL, 0, DEPTH, &query, &error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_runtime_next(&query, &row, &error);
  if (status == TURBODB_STATUS_OK && (row.state != ORM_SQL_SCAN_ROW || row.count != 1 ||
      row.values[0].kind != TURBODB_VALUE_INT64 || row.values[0].data.int64_value != expected)) status = TURBODB_STATUS_INTERNAL_ERROR;
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_runtime_next(&query, &row, &error);
  if (status == TURBODB_STATUS_OK && row.state != ORM_SQL_SCAN_DONE) status = TURBODB_STATUS_INTERNAL_ERROR;
  const turbodb_status_t closed = orm_tidesdb_sql_runtime_close(&query, NULL);
  return status == TURBODB_STATUS_OK ? closed : status;
}
static turbodb_status_t lookup_benchmark_sample(const sqlparser_document *doc, int64_t expected) {
  turbodb_status_t status = orm_tidesdb_sql_budget_end(&budget, &error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_begin(&budget, &error);
  return status == TURBODB_STATUS_OK ? lookup_measure(doc, expected) : status;
}
spec("TidesDB private first-index transaction") {
  before_each() {
    faults_clear(); tdsql_error_init(&error); owner = other = (orm_sql_catalog_store){0};
    lookup_query = (orm_sql_query){0};
    orm_sql_budget_limits limits = {0};
    for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) limits.statement.value[i] = LIMIT;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = WORK;
    limits.transaction = (orm_sql_transaction_budget_amount){LIMIT, LIMIT, LIMIT};
    check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    const turbodb_value_t input[] = {turbodb_i64(INT64_MIN), turbodb_i64(INT64_MIN), turbodb_u64(UINT64_MAX),
      turbodb_i64(-1), turbodb_i64(0), turbodb_u64(7), turbodb_i64(0), turbodb_null(), turbodb_u64(7),
      turbodb_i64(1), turbodb_null(), turbodb_u64(7), turbodb_i64(INT64_MAX), turbodb_i64(0), turbodb_u64(8)};
    memcpy(rows, input, sizeof(rows));
    directory = tt_make_temp_dir("orm-sql-index"); check_not_null(directory); database_open();
    orm_tidesdb_column_family_config_t config = orm_tidesdb_default_column_family_config(); config.sync_mode = ORM_TDB_SYNC_FULL;
    check_equal(orm_tidesdb_create_column_family(database, family_name, &config), ORM_TDB_SUCCESS);
    family = orm_tidesdb_get_column_family(database, family_name); check_not_null(family);
    check_equal(orm_tidesdb_sql_catalog_initialize(database, family, MAX_RECORD, &budget, &error), TURBODB_STATUS_OK);
    begin_owner(&owner); create_table(table_sql);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); begin_owner(&owner);
  }
  after_each() {
    faults_clear();
    check_equal(orm_tidesdb_sql_runtime_close(&lookup_query, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, false, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u); check_equal(budget.retained_work_bytes, 0u);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_close(database), ORM_TDB_SUCCESS); database = NULL; family = NULL;
    check_equal(tt_remove_tree(directory), 0); free(directory); directory = NULL;
  }
  it("publishes an inline table and all index definitions in one metadata batch") {
    size_t affected=OUTPUT_SENTINEL; faults_clear();
    check_equal(execute_sql(inline_sql,&affected),TURBODB_STATUS_OK); check_equal(affected,0u); check_equal(put_calls,5u);
    manifest_is(owner.transaction,2,4,5); check_equal(namespace_count(owner.transaction,INDEX_DIRECTORY_NS),2u);
    check_equal(namespace_count(owner.transaction,INDEX_DATA_NS),0u); check_equal(namespace_count(owner.transaction,INDEX_UNIQUE_NS),0u);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK); begin_owner(&owner);
    const turbodb_value_t values[]={turbodb_u64(7),turbodb_u64(7),turbodb_u64(7)};
    check_equal(execute_params("INSERT INTO fresh(id,a,b) VALUES(1,10,?),(2,NULL,?),(3,NULL,?)",values,3,&affected),TURBODB_STATUS_OK);
    check_equal(execute_params("INSERT INTO fresh(id,a,b) VALUES(4,10,?)",values,1,&affected),TURBODB_STATUS_CONSTRAINT);
    check_equal(lookup_open("SELECT id FROM fresh WHERE a=10",NULL,0),TURBODB_STATUS_OK); lookup_row(1); lookup_end();
    check_equal(execute_sql("DROP TABLE fresh",&affected),TURBODB_STATUS_OK);
    check_equal(namespace_count(owner.transaction,INDEX_DIRECTORY_NS),0u);
  }
  group("DOUBLE index persistence and access") {
    it("backfills DOUBLE composite unique keys and preserves v3 after new integer indexes and reopen") {
      double_seed(); uint64_t id = OUTPUT_SENTINEL;
      check_equal(build_on(&owner, double_unique, &id), TURBODB_STATUS_OK); check_equal(id, 3u);
      manifest_is(owner.transaction, 3, 3, 4); verify_live_indexes(ROWS, 1);
      check_equal(build_on(&owner, "CREATE INDEX ints ON items(b)", &id), TURBODB_STATUS_OK);
      manifest_is(owner.transaction, 3, 4, 5); verify_live_indexes(ROWS, 2);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_close(database), ORM_TDB_SUCCESS); database = NULL; family = NULL;
      database_open(); family = orm_tidesdb_get_column_family(database, family_name); check_not_null(family); begin_owner(&owner);
      manifest_is(owner.transaction, 3, 4, 5); verify_live_indexes(ROWS, 2);
      check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_OK);
      check_equal(drop_on(&owner, "DROP INDEX ints ON items", &id), TURBODB_STATUS_OK);
      manifest_is(owner.transaction, 3, 4, 5); check_equal(namespace_count(owner.transaction, INDEX_DIRECTORY_NS), 0u);
    }
    it("treats both zero signs as one unique value and admits repeated NULLs") {
      double_seed(); uint64_t id = OUTPUT_SENTINEL;
      check_equal(build_on(&owner, "CREATE UNIQUE INDEX ix ON items(a)", &id), TURBODB_STATUS_CONSTRAINT);
      check_equal(id, OUTPUT_SENTINEL); double_unindexed();
      size_t affected = OUTPUT_SENTINEL;
      check_equal(execute_sql("DELETE FROM items WHERE id=3", &affected), TURBODB_STATUS_OK);
      check_equal(build_on(&owner, "CREATE UNIQUE INDEX ix ON items(a DESC)", &id), TURBODB_STATUS_OK);
      check_equal(execute_sql("INSERT INTO items VALUES(3,0.0,2)", &affected), TURBODB_STATUS_CONSTRAINT);
      check_equal(execute_sql("INSERT INTO items VALUES(6,NULL,6),(7,NULL,7)", &affected), TURBODB_STATUS_OK);
      verify_live_indexes(ROWS + 1, 1);
      check_equal(execute_sql("REPLACE INTO items VALUES(8,0.0,8)", &affected), TURBODB_STATUS_OK); check_equal(affected, 2u);
      check_equal(lookup_open("SELECT id FROM items WHERE a=0 ORDER BY id", NULL, 0), TURBODB_STATUS_OK); lookup_row(8); lookup_end();
      check_equal(lookup_open("SELECT id FROM items WHERE a IS NULL ORDER BY id", NULL, 0), TURBODB_STATUS_OK); lookup_row(6); lookup_row(7); lookup_end();
      verify_live_indexes(ROWS + 1, 1);
    }
    it("matches indexed and scan results at finite endpoints signed zero and adjacent subnormals") {
      double_seed(); uint64_t id = 0; check_equal(build_on(&owner, "CREATE INDEX ix ON items(a DESC)", &id), TURBODB_STATUS_OK);
      const double probes[] = {-DBL_MAX, -DBL_TRUE_MIN, -0.0, 0.0, DBL_TRUE_MIN, DBL_MAX};
      const char *operators[] = {"=", "<", "<=", ">", ">="};
      enum { QUERY_BYTES = 128, SAMPLE_COUNT = 6 };
      for (size_t op = 0; op < sizeof(operators)/sizeof(operators[0]); ++op) for (size_t p = 0; p < SAMPLE_COUNT; ++p) {
        char sql[QUERY_BYTES]; (void)snprintf(sql, sizeof(sql), "SELECT id FROM items WHERE a%s? ORDER BY id", operators[op]);
        const turbodb_value_t parameter = turbodb_f64(probes[p]); check_equal(lookup_open(sql, &parameter, 1), TURBODB_STATUS_OK);
        for (size_t r = 0; r < ROWS; ++r) {
          const double value = rows[r*COLUMNS+1].data.double_value;
          const bool matches = op == 0 ? value == probes[p] : op == 1 ? value < probes[p] :
              op == 2 ? value <= probes[p] : op == 3 ? value > probes[p] : value >= probes[p];
          if (matches) lookup_row((int64_t)r + 1);
        }
        lookup_end();
        (void)snprintf(sql, sizeof(sql), "SELECT id FROM items WHERE (a+0.0)%s? ORDER BY id", operators[op]);
        check_equal(lookup_open(sql, &parameter, 1), TURBODB_STATUS_OK);
        for (size_t r = 0; r < ROWS; ++r) {
          const double value = rows[r*COLUMNS+1].data.double_value;
          const bool matches = op == 0 ? value == probes[p] : op == 1 ? value < probes[p] :
              op == 2 ? value <= probes[p] : op == 3 ? value > probes[p] : value >= probes[p];
          if (matches) lookup_row((int64_t)r + 1);
        }
        lookup_end();
      }
      check_equal(lookup_open("SELECT id FROM items WHERE a IN(0.0,0,4.9406564584124654e-324) OR a BETWEEN -1.0 AND 0.0 ORDER BY id", NULL, 0), TURBODB_STATUS_OK);
      lookup_row(2); lookup_row(3); lookup_row(4); lookup_end();
    }
    it("rolls back a DOUBLE format upgrade and index data through the same savepoint") {
      double_seed(); save(); uint64_t id = 0;
      check_equal(build_on(&owner, double_unique, &id), TURBODB_STATUS_OK); manifest_is(owner.transaction, 3, 3, 4);
      restore(); double_unindexed();
      check_equal(build_on(&owner, "CREATE INDEX ints ON items(b)", &id), TURBODB_STATUS_OK);
      manifest_is(owner.transaction, 2, 3, 4); verify_live_indexes(ROWS, 1);
      check_equal(build_on(&owner, double_unique, &id), TURBODB_STATUS_OK);
      manifest_is(owner.transaction, 3, 4, 5); verify_live_indexes(ROWS, 2);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); begin_owner(&owner);
      no_index(1);
    }
    it("never publishes a partial DOUBLE upgrade under allocation or native write failure") {
      double_seed(); save(); faults_clear(); uint64_t id = 0;
      check_equal(build_on(&owner, double_unique, &id), TURBODB_STATUS_OK);
      const size_t counts[] = {reserves, resizes, put_calls}; faults_clear(); restore();
      for (unsigned phase = 0; phase < 3; ++phase) for (size_t failure = 1; failure <= counts[phase]; ++failure) {
        next_statement(); faults_clear(); id = OUTPUT_SENTINEL;
        if (phase == 0) fail_reserve = failure; else if (phase == 1) fail_resize = failure; else fail_put = failure;
        check_equal(build_on(&owner, double_unique, &id), phase == 2 ? TURBODB_STATUS_DATASTORE_ERROR : TURBODB_STATUS_OUT_OF_MEMORY);
        check_equal(id, OUTPUT_SENTINEL); faults_clear(); double_unindexed();
        check_false(owner.failed); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], budget.retained_work_bytes);
        restore();
      }
      check_equal(build_on(&owner, double_unique, &id), TURBODB_STATUS_OK); verify_live_indexes(ROWS, 1);
    }
    it("rejects DOUBLE index records below the advertised Manifest capability") {
      double_seed(); uint64_t id = 0; check_equal(build_on(&owner, double_unique, &id), TURBODB_STATUS_OK);
      uint8_t key[ORM_SQL_STORE_MANIFEST_KEY_BYTES], data[ORM_SQL_STORE_MANIFEST_BYTES];
      check_equal(orm_sql_store_catalog_barrier(&owner, key, data, &error), TURBODB_STATUS_OK);
      data[ORM_SQL_STORE_FORMAT_OFFSET] = ORM_SQL_STORE_FORMAT_INDEXED;
      check_equal(orm_tidesdb_txn_put(owner.transaction, family, key, sizeof(key), data, sizeof(data), 0), ORM_TDB_SUCCESS);
      check_equal(lookup_open("SELECT id FROM items WHERE a=0", NULL, 0), TURBODB_STATUS_DATASTORE_ERROR);
      check_true(owner.failed); check_equal(owner.active_sources, 0u);
    }
    it("publishes inline DOUBLE keys atomically and never downgrades v3 for later integer tables") {
      size_t affected = OUTPUT_SENTINEL;
      check_equal(execute_sql("CREATE TABLE fresh(id BIGINT PRIMARY KEY,a DOUBLE UNIQUE,b BIGINT,KEY combo(b,a DESC))", &affected), TURBODB_STATUS_OK);
      manifest_is(owner.transaction, 3, 4, 5);
      check_equal(execute_sql("INSERT INTO fresh VALUES(1,-0.0,7),(2,NULL,8)", &affected), TURBODB_STATUS_OK);
      check_equal(execute_sql("INSERT INTO fresh VALUES(3,0.0,9)", &affected), TURBODB_STATUS_CONSTRAINT);
      check_equal(execute_sql("CREATE TABLE ints(id BIGINT PRIMARY KEY,b BIGINT,KEY(b))", &affected), TURBODB_STATUS_OK);
      manifest_is(owner.transaction, 3, 6, 7);
      check_equal(execute_sql("TRUNCATE TABLE fresh", &affected), TURBODB_STATUS_OK);
      check_equal(namespace_count(owner.transaction, INDEX_DATA_NS), 0u);
      check_equal(namespace_count(owner.transaction, INDEX_UNIQUE_NS), 0u);
      check_equal(execute_sql("DROP TABLE fresh,ints", &affected), TURBODB_STATUS_OK);
      manifest_is(owner.transaction, 3, 6, 7);
    }
  }
  it("refunds each inline CREATE allocation failure before any native write") {
    save(); faults_clear(); size_t affected=OUTPUT_SENTINEL;
    check_equal(execute_sql(inline_sql,&affected),TURBODB_STATUS_OK); const size_t allocations[]={reserves,resizes}; restore();
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(size_t phase=0;phase<2;++phase) for(size_t point=1;point<=allocations[phase];++point) {
      next_statement(); faults_clear(); if(phase) fail_resize=point; else fail_reserve=point; affected=OUTPUT_SENTINEL;
      check_equal(execute_sql(inline_sql,&affected),TURBODB_STATUS_OUT_OF_MEMORY); check_equal(affected,OUTPUT_SENTINEL);
      check_equal(put_calls,0u); check_equal(delete_calls,0u); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); faults_clear(); fresh_absent();
    }
  }
  it("rolls back every inline metadata write failure without retaining a table or IDs") {
    for(size_t point=1;point<=5;++point) {
      next_statement(); faults_clear(); fail_put=point; size_t affected=OUTPUT_SENTINEL;
      check_equal(execute_sql(inline_sql,&affected),TURBODB_STATUS_DATASTORE_ERROR); check_equal(affected,OUTPUT_SENTINEL);
      check_false(owner.failed); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],budget.retained_work_bytes);
      faults_clear(); fresh_absent();
    }
    size_t affected=OUTPUT_SENTINEL; check_equal(execute_sql(inline_sql,&affected),TURBODB_STATUS_OK);
    manifest_is(owner.transaction,2,4,5);
  }
  it("rejects inline metadata read failures without publishing definitions") {
    save(); faults_clear(); size_t affected=OUTPUT_SENTINEL;
    check_equal(execute_sql(inline_sql,&affected),TURBODB_STATUS_OK); const size_t counts[]={gets,news,1}; restore();
    for(size_t phase=0;phase<3;++phase) for(size_t point=1;point<=counts[phase];++point) {
      next_statement(); faults_clear();
      if(phase==2) fail_iterator=FAIL_SEEK; else if(phase) fail_new=point; else fail_get=point;
      affected=OUTPUT_SENTINEL; check_equal(execute_sql(inline_sql,&affected),TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(affected,OUTPUT_SENTINEL); check_equal(put_calls,0u);
      faults_clear(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK); begin_owner(&owner);
      fresh_absent();
    }
  }
  it("admits all inline CREATE resources before publishing metadata") {
    const orm_sql_budget_resource resources[]={ORM_SQL_BUDGET_AST_NODES,ORM_SQL_BUDGET_PLAN_NODES,
      ORM_SQL_BUDGET_WORK_BYTES,ORM_SQL_BUDGET_READ_ROWS,ORM_SQL_BUDGET_READ_BYTES,
      ORM_SQL_BUDGET_EXECUTION_STEPS,ORM_SQL_BUDGET_WRITE_ROWS,ORM_SQL_BUDGET_WRITE_BYTES};
    for(size_t i=0;i<sizeof(resources)/sizeof(resources[0]);++i) {
      next_statement(); faults_clear(); size_t affected=OUTPUT_SENTINEL;
      const uint64_t limit=budget.limits.statement.value[resources[i]];
      budget.limits.statement.value[resources[i]]=budget.used.value[resources[i]];
      check_equal(execute_sql(inline_sql,&affected),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(affected,OUTPUT_SENTINEL);
      budget.limits.statement.value[resources[i]]=limit;
      check_equal(put_calls,0u); check_false(owner.failed); fresh_absent();
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],budget.retained_work_bytes);
    }
  }
  it("handles inline CREATE savepoint and rollback failures at the batch boundary") {
    for(size_t phase=0;phase<3;++phase) {
      faults_clear(); size_t affected=OUTPUT_SENTINEL;
      if(phase==0) fail_savepoint=true;
      else if(phase==1) fail_release=true;
      else { fail_put=2; fail_rollback=true; }
      check_equal(execute_sql(inline_sql,&affected),TURBODB_STATUS_DATASTORE_ERROR); check_equal(affected,OUTPUT_SENTINEL);
      if(phase==2) check_true(owner.failed);
      faults_clear(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK); begin_owner(&owner); fresh_absent();
    }
  }
  it("rejects inline ID exhaustion and orphan namespaces before native writes") {
    uint8_t header[STORE_MANIFEST_BYTES];
    store_manifest_encode((store_manifest){UINT64_MAX-2,UINT64_MAX-1,ORM_SQL_STORE_FORMAT_BASE},header);
    check_equal(orm_tidesdb_txn_put(owner.transaction,family,store_manifest_key,sizeof(store_manifest_key),header,sizeof(header),0),ORM_TDB_SUCCESS);
    faults_clear(); size_t affected=OUTPUT_SENTINEL;
    check_equal(execute_sql(inline_sql,&affected),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(put_calls,0u);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK); begin_owner(&owner);
    const uint8_t spaces[]={STORE_CREATE_DATA_NS,INDEX_DIRECTORY_NS,INDEX_DATA_NS,2};
    for(size_t i=0;i<sizeof(spaces);++i) {
      uint8_t key[1+ORM_SQL_WIRE_U64]={0},value[ORM_SQL_WIRE_U64]={1}; key[0]=spaces[i];
      orm_sql_wire_write(key+1,ORM_SQL_WIRE_U64,i<2?2:3);
      check_equal(orm_tidesdb_txn_put(owner.transaction,family,key,sizeof(key),value,sizeof(value),0),ORM_TDB_SUCCESS);
      faults_clear(); check_equal(execute_sql(inline_sql,&affected),TURBODB_STATUS_DATASTORE_ERROR); check_true(owner.failed); check_equal(put_calls,0u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK); begin_owner(&owner); fresh_absent();
    }
  }
  it("serializes complete inline CREATE against concurrent creation in either commit order") {
    check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK);
    const char sql[]="CREATE TABLE fresh(id BIGINT PRIMARY KEY,a BIGINT,b BIGINT UNSIGNED)";
    sqlparser_document *doc=NULL; sqlparser_error diagnostic; enum { DEPTH=32 };
    check_equal(sqlparser_parse(sql,strlen(sql),NULL,&doc,&diagnostic),SQLPARSER_OK);
    for(size_t first=0;first<2;++first) {
      begin_owner(&owner); begin_owner(&other); size_t affected=OUTPUT_SENTINEL;
      check_equal(execute_sql(inline_sql,&affected),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_runtime_execute(doc,&other,NULL,0,DEPTH,0,false,NULL,&affected,&error),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(first?&owner:&other,true,&error),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(first?&other:&owner,true,&error),TURBODB_STATUS_BUSY);
      begin_owner(&owner);
      check_equal(namespace_count(owner.transaction,INDEX_DIRECTORY_NS),first?2u:0u);
      check_equal(lookup_open("SHOW INDEX FROM fresh",NULL,0),TURBODB_STATUS_OK);
      for(size_t i=0;i<(first?4u:1u);++i) {
        orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&lookup_query,&row,&error),TURBODB_STATUS_OK);
        check_equal(row.state,ORM_SQL_SCAN_ROW);
      }
      lookup_end(); check_equal(clear_on(&owner,"DROP TABLE fresh"),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    }
    sqlparser_document_destroy(doc); begin_owner(&owner);
  }
  it("recovers either absent or complete inline definitions after uncertain commits") {
    for(int fault=COMMIT_BEFORE;fault<=COMMIT_AFTER;++fault) {
      size_t affected=OUTPUT_SENTINEL; check_equal(execute_sql(inline_sql,&affected),TURBODB_STATUS_OK); commit_fault=fault;
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_COMMIT_UNKNOWN); faults_clear();
      orm_tidesdb_transaction_t *tx=NULL; reopen_raw(&tx);
      manifest_is(tx,fault==COMMIT_AFTER?2:1,fault==COMMIT_AFTER?4:1,fault==COMMIT_AFTER?5:2);
      check_equal(namespace_count(tx,INDEX_DIRECTORY_NS),fault==COMMIT_AFTER?2u:0u);
      check_equal(orm_tidesdb_txn_rollback(tx),ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx); begin_owner(&owner);
      if(fault==COMMIT_BEFORE) fresh_absent();
      else {
        const turbodb_value_t value=turbodb_u64(9);
        check_equal(execute_params("INSERT INTO fresh(id,a,b) VALUES(1,10,?)",&value,1,&affected),TURBODB_STATUS_OK);
        check_equal(execute_params("INSERT INTO fresh(id,a,b) VALUES(2,10,?)",&value,1,&affected),TURBODB_STATUS_CONSTRAINT);
      }
    }
  }
  it("persists complete ordinary and unique backfills including duplicate NULL tuples") {
    seed(); save(); uint64_t id = OUTPUT_SENTINEL;
    check_equal(build_on(&owner, ordinary_sql, &id), TURBODB_STATUS_OK); check_equal(id, 2u);
    verify_entries(owner.transaction, false); restore(); no_index(2);
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_OK); verify_entries(owner.transaction, true);
    manifest_is(owner.transaction, 2, 2, 3); version_is(owner.transaction, 3);
    check_equal(namespace_count(owner.transaction, 3), ROWS);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    orm_tidesdb_transaction_t *tx = NULL; reopen_raw(&tx);
    verify_entries(tx, true); manifest_is(tx, 2, 2, 3); version_is(tx, 3);
    check_equal(orm_tidesdb_txn_rollback(tx), ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx);
    check_equal(orm_tidesdb_sql_catalog_begin(database, family, MAX_RECORD, &budget, &owner, &error), TURBODB_STATUS_OK);
    check_not_null(owner.transaction);
    check_equal(orm_tidesdb_sql_catalog_require_legacy(database, family, &error), TURBODB_STATUS_INVALID_STATE);
  }
  it("publishes an empty index with only directory, TableVersion and Manifest writes") {
    faults_clear(); uint64_t id = OUTPUT_SENTINEL;
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_OK); check_equal(id, 2u);
    check_equal(put_calls, INDEX_METADATA_WRITES); manifest_is(owner.transaction, 2, 2, 3); version_is(owner.transaction, 2);
    check_equal(namespace_count(owner.transaction, INDEX_DATA_NS), 0u);
    check_equal(namespace_count(owner.transaction, INDEX_UNIQUE_NS), 0u);
    check_equal(namespace_count(owner.transaction, INDEX_DIRECTORY_NS), 1u);
  }
  it("rejects duplicate non-NULL composite tuples before any put while allowing ordinary duplicates") {
    rows[(ROWS - 1) * COLUMNS + 2] = turbodb_u64(7); seed();
    faults_clear(); uint64_t id = OUTPUT_SENTINEL;
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_CONSTRAINT); check_equal(id, OUTPUT_SENTINEL);
    check_equal(put_calls, 0u); check_false(owner.failed); no_index(2);
    check_equal(build_on(&owner, ordinary_sql, &id), TURBODB_STATUS_OK); verify_entries(owner.transaction, false);
  }
  it("maintains indexes on subsequent DML and restores v1 through user savepoint rollback") {
    seed(); save(); uint64_t id = 0;
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_OK);
    const turbodb_value_t row[] = {turbodb_i64(10), turbodb_i64(11), turbodb_u64(12)};
    check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr(table_name), row, COLUMNS, &error), TURBODB_STATUS_OK);
    orm_sql_relation_source source = {0};
    check_equal(orm_tidesdb_sql_relation_open(&owner, vstr_from_cstr(table_name), &source, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_relation_close(&source, &error), TURBODB_STATUS_OK);
    check_equal(namespace_count(owner.transaction, INDEX_DATA_NS), ROWS + 1u);
    check_equal(namespace_count(owner.transaction, INDEX_UNIQUE_NS), ROWS - 1u);
    id = OUTPUT_SENTINEL; check_equal(build_on(&owner, ordinary_sql, &id), TURBODB_STATUS_CONSTRAINT);
    check_equal(id, OUTPUT_SENTINEL); restore(); no_index(2);
    check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr(table_name), row, COLUMNS, &error), TURBODB_STATUS_OK);
  }
  it("rolls back every partial publication without losing earlier uncommitted rows") {
    seed(); const size_t writes = ROWS + ROWS - 2 + INDEX_METADATA_WRITES;
    for (size_t point = 1; point <= writes; ++point) {
      faults_clear(); fail_put = point; uint64_t id = OUTPUT_SENTINEL;
      const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(id, OUTPUT_SENTINEL); check_false(owner.failed); check_equal(put_calls, point);
      faults_clear(); no_index(2); check_equal(namespace_count(owner.transaction, 3), ROWS);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
    }
    uint64_t id = 0; check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_OK); verify_entries(owner.transaction, true);
  }
  it("unwinds every workspace reserve and resize allocation failure") {
    seed(); save(); faults_clear(); uint64_t id = 0;
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_OK);
    const size_t reserve_count = reserves, resize_count = resizes; restore();
    const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for (size_t phase = 0; phase < 2; ++phase) {
      const size_t count = phase ? resize_count : reserve_count;
      for (size_t point = 1; point <= count; ++point) {
        faults_clear(); if (phase) fail_resize = point; else fail_reserve = point; id = OUTPUT_SENTINEL;
        check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_OUT_OF_MEMORY);
        check_equal(id, OUTPUT_SENTINEL); check_equal(put_calls, 0u); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work); faults_clear(); no_index(2);
      }
    }
    fail_sort = true; id = OUTPUT_SENTINEL;
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_OUT_OF_MEMORY); check_equal(id, OUTPUT_SENTINEL);
    faults_clear(); no_index(2); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
  }
  it("unwinds metadata reads and both scan passes without publishing partial state") {
    seed(); save(); faults_clear(); uint64_t id = 0;
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_OK);
    const size_t get_count = gets, new_count = news; restore();
    const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for (size_t point = 1; point <= get_count; ++point) {
      faults_clear(); fail_get = point; id = OUTPUT_SENTINEL;
      check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_DATASTORE_ERROR); check_equal(id, OUTPUT_SENTINEL);
      check_equal(put_calls, 0u); faults_clear(); no_index(2);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work); recover_failed_owner();
    }
    for (size_t point = 1; point <= new_count; ++point) {
      faults_clear(); fail_new = point; id = OUTPUT_SENTINEL;
      check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_DATASTORE_ERROR); check_equal(id, OUTPUT_SENTINEL);
      check_equal(put_calls, 0u); faults_clear(); no_index(2); recover_failed_owner();
    }
    for (int point = FAIL_SEEK; point <= FAIL_VALUE; ++point) {
      faults_clear(); fail_iterator = point; id = OUTPUT_SENTINEL;
      check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_DATASTORE_ERROR); check_equal(id, OUTPUT_SENTINEL);
      check_equal(put_calls, 0u); faults_clear(); no_index(2);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work); recover_failed_owner();
    }
  }
  it("retains the owner on savepoint creation failure and poisons failed rollback") {
    seed(); uint64_t id = OUTPUT_SENTINEL; fail_savepoint = true;
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_DATASTORE_ERROR); check_equal(id, OUTPUT_SENTINEL);
    check_false(owner.failed); faults_clear(); no_index(2);
    fail_put = 2; fail_rollback = true;
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_DATASTORE_ERROR); check_true(owner.failed);
    check_equal(id, OUTPUT_SENTINEL);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_INVALID_STATE);
    faults_clear(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    begin_owner(&owner); no_index(1);
  }
  it("poisons failed savepoint release so no partial statement can be committed") {
    seed(); uint64_t id = OUTPUT_SENTINEL; fail_release = true;
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_DATASTORE_ERROR); check_true(owner.failed);
    check_equal(id, OUTPUT_SENTINEL); faults_clear();
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); begin_owner(&owner); no_index(1);
  }
  it("detects a row writer committing before the index builder") {
    seed(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    begin_owner(&owner); begin_owner(&other); uint64_t id = 0;
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_OK);
    const turbodb_value_t row[] = {turbodb_i64(10), turbodb_i64(11), turbodb_u64(12)};
    check_equal(orm_tidesdb_sql_relation_insert(&other, vstr_from_cstr(table_name), row, COLUMNS, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_BUSY);
    begin_owner(&owner); no_index(3); check_equal(namespace_count(owner.transaction, 3), ROWS + 1u);
  }
  it("rejects stale row writes when the builder commits first") {
    seed(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    begin_owner(&owner); begin_owner(&other); uint64_t id = 0;
    const turbodb_value_t row[] = {turbodb_i64(10), turbodb_i64(11), turbodb_u64(12)};
    check_equal(orm_tidesdb_sql_relation_insert(&other, vstr_from_cstr(table_name), row, COLUMNS, &error), TURBODB_STATUS_OK);
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_BUSY);
    orm_tidesdb_transaction_t *tx = NULL; reopen_raw(&tx); verify_entries(tx, true);
    check_equal(namespace_count(tx, 3), ROWS); check_equal(orm_tidesdb_txn_rollback(tx), ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx);
  }
  it("serializes two index builders through the shared Manifest and table version") {
    seed(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    begin_owner(&owner); begin_owner(&other); uint64_t first = 0, second = 0;
    check_equal(build_on(&owner, unique_sql, &first), TURBODB_STATUS_OK);
    check_equal(build_on(&other, "CREATE INDEX second ON items (b)", &second), TURBODB_STATUS_OK);
    check_equal(first, second); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_BUSY);
    orm_tidesdb_transaction_t *tx = NULL; reopen_raw(&tx); verify_entries(tx, true);
    check_equal(orm_tidesdb_txn_rollback(tx), ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx);
  }
  it("rejects unsupported definitions, missing tables and live source leases without writes") {
    const char *inputs[] = {"CREATE INDEX ix ON missing (a)", "CREATE INDEX ix ON items (missing)",
      "CREATE INDEX ix ON items (a(2))", "CREATE INDEX ix ON items ((a+1))"};
    const turbodb_status_t expected[] = {TURBODB_STATUS_SQL_ERROR, TURBODB_STATUS_SQL_ERROR, TURBODB_STATUS_UNSUPPORTED, TURBODB_STATUS_UNSUPPORTED};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
      uint64_t id = OUTPUT_SENTINEL; faults_clear();
      check_equal(build_on(&owner, inputs[i], &id), expected[i]); check_equal(id, OUTPUT_SENTINEL); check_equal(put_calls, 0u);
      no_index(1);
    }
    orm_sql_relation_source source = {0}; uint64_t id = OUTPUT_SENTINEL;
    check_equal(orm_tidesdb_sql_relation_open(&owner, vstr_from_cstr(table_name), &source, &error), TURBODB_STATUS_OK);
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_BUSY); check_equal(id, OUTPUT_SENTINEL);
    check_equal(orm_tidesdb_sql_relation_close(&source, &error), TURBODB_STATUS_OK); no_index(1);
    sqlparser_document *doc = NULL; sqlparser_error diagnostic;
    check_equal(sqlparser_parse_dialect(unique_sql, strlen(unique_sql), SQLPARSER_SQLITE, NULL, &doc, &diagnostic), SQLPARSER_OK);
    check_equal(orm_tidesdb_sql_index_build(doc, &owner, &id, &error), TURBODB_STATUS_UNSUPPORTED);
    sqlparser_document_destroy(doc); no_index(1);
  }
  it("enforces every used statement resource at the exact build boundary") {
    seed(); save(); faults_clear();
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    uint64_t id = 0; check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_OK);
    const orm_sql_budget_amount peak = budget.peak; restore();
    const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_WORK_BYTES, ORM_SQL_BUDGET_MATERIALIZED_ROWS,
      ORM_SQL_BUDGET_AST_NODES, ORM_SQL_BUDGET_PLAN_NODES, ORM_SQL_BUDGET_EXECUTION_STEPS,
      ORM_SQL_BUDGET_WRITE_ROWS, ORM_SQL_BUDGET_WRITE_BYTES, ORM_SQL_BUDGET_READ_ROWS, ORM_SQL_BUDGET_READ_BYTES};
    for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); ++i) {
      const orm_sql_budget_resource resource = resources[i]; const uint64_t original = budget.limits.statement.value[resource];
      check_true(peak.value[resource] > 1);
      const uint64_t capacities[] = {peak.value[resource] / 2, peak.value[resource] - 1, peak.value[resource]};
      for (size_t j = 0; j < sizeof(capacities) / sizeof(capacities[0]); ++j) {
        check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
        check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
        budget.limits.statement.value[resource] = capacities[j]; id = OUTPUT_SENTINEL; faults_clear();
        const turbodb_status_t status = build_on(&owner, unique_sql, &id);
        budget.limits.statement.value[resource] = original;
        if (j == 2) { check_equal(status, TURBODB_STATUS_OK); check_equal(id, 2u); restore(); }
        else { check_equal(status, TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(id, OUTPUT_SENTINEL); check_equal(put_calls, 0u); }
        check_false(owner.failed); no_index(2);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], budget.retained_work_bytes);
      }
    }
  }
  it("allows repeated unique tuples when NULL is in a later composite part") {
    rows[1] = turbodb_i64(0); rows[2] = turbodb_null(); rows[(ROWS - 1) * COLUMNS + 2] = turbodb_null(); seed();
    uint64_t id = 0; check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_OK);
    check_equal(namespace_count(owner.transaction, INDEX_DATA_NS), ROWS);
    check_equal(namespace_count(owner.transaction, INDEX_UNIQUE_NS), 1u);
  }
  it("backfills unsigned primary-key endpoints without signed narrowing") {
    create_table("CREATE TABLE unsigned_items (id BIGINT UNSIGNED PRIMARY KEY, a BIGINT)");
    const turbodb_value_t input[] = {turbodb_u64(0), turbodb_i64(-1), turbodb_u64(UINT64_MAX), turbodb_i64(-1)};
    check_equal(orm_tidesdb_sql_relation_insert_rows(&owner, vstr_from_cstr("unsigned_items"), input, 2, 2, &error), TURBODB_STATUS_OK);
    uint64_t id = 0; check_equal(build_on(&owner, "CREATE INDEX ix ON unsigned_items (a DESC)", &id), TURBODB_STATUS_OK);
    check_equal(id, 3u); manifest_is(owner.transaction, 2, 3, 4);
    const uint8_t prefix = INDEX_DATA_NS; orm_tidesdb_iterator_t *it = NULL;
    check_equal(orm_tidesdb_iter_new(owner.transaction, family, &it), ORM_TDB_SUCCESS);
    check_equal(orm_tidesdb_iter_seek(it, &prefix, sizeof(prefix)), ORM_TDB_SUCCESS);
    const uint64_t expected[] = {0, UINT64_MAX};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
      uint8_t *key = NULL, *value = NULL; size_t key_size = 0, value_size = 0;
      check_equal(orm_tidesdb_iter_key(it, &key, &key_size), ORM_TDB_SUCCESS);
      check_equal(key_size, INDEX_PREFIX_BYTES + 1 + 2 * ORM_SQL_WIRE_U64);
      check_equal(orm_sql_wire_order_read(key + key_size - ORM_SQL_WIRE_U64), expected[i]);
      check_equal(orm_tidesdb_iter_value(it, &value, &value_size), ORM_TDB_SUCCESS);
      check_equal(value_size, ORM_SQL_WIRE_U64); check_equal(orm_sql_wire_order_read(value), expected[i]);
      const int code = orm_tidesdb_iter_next(it); check_true(code == ORM_TDB_SUCCESS || code == ORM_TDB_ERR_NOT_FOUND);
    }
    orm_tidesdb_iter_free(it);
  }
  it("refuses orphan index namespaces and poisoned Catalog identities without overwriting") {
    for (uint8_t space = INDEX_UNIQUE_NS; space <= INDEX_DIRECTORY_NS; ++space) {
      const uint8_t key[] = {space, 1}, value[] = {1};
      check_equal(orm_tidesdb_txn_put(owner.transaction, family, key, sizeof(key), value, sizeof(value), 0), ORM_TDB_SUCCESS);
      uint64_t id = OUTPUT_SENTINEL; faults_clear();
      check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_DATASTORE_ERROR); check_equal(id, OUTPUT_SENTINEL);
      check_true(owner.failed); check_equal(put_calls, 0u); check_equal(namespace_count(owner.transaction, space), 1u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); begin_owner(&owner); no_index(1);
    }
    uint8_t key[ORM_SQL_STORE_VERSION_KEY_BYTES], value[ORM_SQL_WIRE_U64]; orm_sql_store_version_key(2, key);
    orm_sql_wire_write(value, sizeof(value), 1);
    check_equal(orm_tidesdb_txn_put(owner.transaction, family, key, sizeof(key), value, sizeof(value), 0), ORM_TDB_SUCCESS);
    uint64_t id = OUTPUT_SENTINEL; faults_clear();
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_DATASTORE_ERROR); check_equal(id, OUTPUT_SENTINEL);
    check_true(owner.failed); check_equal(put_calls, 0u);
  }
  it("fails before scanning when object IDs or table versions are exhausted") {
    save(); uint8_t key[ORM_SQL_STORE_VERSION_KEY_BYTES], value[ORM_SQL_WIRE_U64];
    orm_sql_store_version_key(1, key); orm_sql_wire_write(value, sizeof(value), UINT64_MAX);
    check_equal(orm_tidesdb_txn_put(owner.transaction, family, key, sizeof(key), value, sizeof(value), 0), ORM_TDB_SUCCESS);
    uint64_t id = OUTPUT_SENTINEL; faults_clear();
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(id, OUTPUT_SENTINEL);
    check_equal(put_calls, 0u); check_equal(news, 0u); check_false(owner.failed); restore(); no_index(1);
    uint8_t manifest[STORE_MANIFEST_BYTES]; store_manifest_encode((store_manifest){UINT64_MAX - 1, UINT64_MAX, 1}, manifest);
    check_equal(orm_tidesdb_txn_put(owner.transaction, family, store_manifest_key, sizeof(store_manifest_key),
        manifest, sizeof(manifest), 0), ORM_TDB_SUCCESS); faults_clear();
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(id, OUTPUT_SENTINEL);
    check_equal(put_calls, 0u); check_equal(news, 0u); check_false(owner.failed); restore(); no_index(1);
  }
  it("rejects malformed Manifest and corrupted base rows without publishing an index") {
    uint8_t manifest[STORE_MANIFEST_BYTES]; store_manifest_encode((store_manifest){1, 4, 1}, manifest);
    check_equal(orm_tidesdb_txn_put(owner.transaction, family, store_manifest_key, sizeof(store_manifest_key),
        manifest, sizeof(manifest), 0), ORM_TDB_SUCCESS);
    uint64_t id = OUTPUT_SENTINEL; faults_clear();
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_DATASTORE_ERROR); check_true(owner.failed);
    check_equal(id, OUTPUT_SENTINEL); check_equal(put_calls, 0u);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); begin_owner(&owner); seed();
    uint8_t key[ORM_SQL_RELATION_KEY_BYTES] = {3,1,0,0,0,0,0,0,0,1}; const uint8_t invalid[] = {0};
    orm_sql_wire_order_write(key + ORM_SQL_RELATION_PREFIX_BYTES, 0);
    check_equal(orm_tidesdb_txn_put(owner.transaction, family, key, sizeof(key), invalid, sizeof(invalid), 0), ORM_TDB_SUCCESS);
    faults_clear(); check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_DATASTORE_ERROR);
    check_true(owner.failed); check_equal(id, OUTPUT_SENTINEL); check_equal(put_calls, 0u); no_index(2);
  }
  it("bounds the full physical key independently of tuple and directory record sizes") {
    create_table("CREATE TABLE w (a BIGINT PRIMARY KEY, b BIGINT, c BIGINT, d BIGINT, e BIGINT, "
        "f BIGINT, g BIGINT, h BIGINT, i BIGINT, j BIGINT)");
    const char sql[] = "CREATE INDEX ix ON w (a,b,c,d,e,f,g,h,i,j)";
    enum { FULL_KEY_BYTES = 115 };
    owner.max_record_bytes = FULL_KEY_BYTES - 1; faults_clear(); uint64_t id = OUTPUT_SENTINEL;
    check_equal(build_on(&owner, sql, &id), TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(id, OUTPUT_SENTINEL);
    check_equal(put_calls, 0u); check_equal(news, 0u); check_false(owner.failed);
    owner.max_record_bytes = FULL_KEY_BYTES;
    check_equal(build_on(&owner, sql, &id), TURBODB_STATUS_OK); check_equal(id, 3u);
    manifest_is(owner.transaction, 2, 3, 4); owner.max_record_bytes = MAX_RECORD;
  }
  it("detects a concurrent phantom insert even when the backfill snapshot is empty") {
    begin_owner(&other); uint64_t id = 0;
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_relation_insert(&other, vstr_from_cstr(table_name), rows, COLUMNS, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_BUSY);
    begin_owner(&owner); no_index(2); check_equal(namespace_count(owner.transaction, 3), 1u);
  }
  it("maintains three indexes through insert, update, primary-key move, delete and reopen") {
    seed(); build_many(); verify_live_indexes(ROWS, 3); manifest_is(owner.transaction, 2, 4, 5); version_is(owner.transaction, 5);
    const turbodb_value_t inserted[] = {turbodb_i64(10), turbodb_i64(2), turbodb_u64(9), turbodb_i64(11), turbodb_null(), turbodb_u64(7)};
    check_equal(orm_tidesdb_sql_relation_insert_rows(&owner, vstr_from_cstr(table_name), inserted, 2, COLUMNS, &error), TURBODB_STATUS_OK);
    verify_live_indexes(ROWS + 2, 3);
    const turbodb_value_t updated[] = {turbodb_i64(10), turbodb_i64(3), turbodb_u64(8)};
    check_equal(orm_tidesdb_sql_relation_update_rows(&owner, vstr_from_cstr(table_name), updated, 1, COLUMNS, &error), TURBODB_STATUS_OK);
    verify_live_indexes(ROWS + 2, 3);
    const turbodb_value_t old = turbodb_i64(10), moved[] = {turbodb_i64(12), turbodb_i64(3), turbodb_u64(8)};
    check_equal(orm_tidesdb_sql_relation_move_rows(&owner, vstr_from_cstr(table_name), &old, moved, 1, COLUMNS, &error), TURBODB_STATUS_OK);
    const turbodb_value_t removed = turbodb_i64(11);
    check_equal(orm_tidesdb_sql_relation_delete_rows(&owner, vstr_from_cstr(table_name), &removed, 1, &error), TURBODB_STATUS_OK);
    verify_live_indexes(ROWS + 1, 3); version_is(owner.transaction, 9);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    orm_tidesdb_transaction_t *tx = NULL; reopen_raw(&tx);
    check_equal(orm_tidesdb_txn_rollback(tx), ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx); begin_owner(&owner);
    verify_live_indexes(ROWS + 1, 3); version_is(owner.transaction, 9);
    const turbodb_value_t all[] = {turbodb_i64(INT64_MIN), turbodb_i64(-1), turbodb_i64(0), turbodb_i64(1), turbodb_i64(INT64_MAX), turbodb_i64(12)};
    check_equal(orm_tidesdb_sql_relation_delete_rows(&owner, vstr_from_cstr(table_name), all, ROWS + 1, &error), TURBODB_STATUS_OK);
    verify_live_indexes(0, 3);
    check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr(table_name), moved, COLUMNS, &error), TURBODB_STATUS_OK);
    verify_live_indexes(1, 3);
  }
  it("rejects insert and update unique conflicts before writing Data or indexes") {
    seed(); build_many(); faults_clear();
    const turbodb_value_t conflict[] = {turbodb_i64(10), turbodb_i64(0), turbodb_u64(7)};
    check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr(table_name), conflict, COLUMNS, &error), TURBODB_STATUS_CONSTRAINT);
    check_equal(put_calls, 0u); check_equal(delete_calls, 0u); check_false(owner.failed); version_is(owner.transaction, 5);
    const turbodb_value_t changed[] = {turbodb_i64(INT64_MIN), turbodb_i64(0), turbodb_u64(8)};
    check_equal(orm_tidesdb_sql_relation_update_rows(&owner, vstr_from_cstr(table_name), changed, 1, COLUMNS, &error), TURBODB_STATUS_CONSTRAINT);
    check_equal(put_calls, 0u); check_equal(delete_calls, 0u); check_false(owner.failed); verify_live_indexes(ROWS, 3);
    const turbodb_value_t duplicated[] = {turbodb_i64(10), turbodb_i64(4), turbodb_u64(9), turbodb_i64(11), turbodb_i64(4), turbodb_u64(9)};
    check_equal(orm_tidesdb_sql_relation_insert_rows(&owner, vstr_from_cstr(table_name), duplicated, 2, COLUMNS, &error), TURBODB_STATUS_CONSTRAINT);
    check_equal(put_calls, 0u); check_equal(delete_calls, 0u); verify_live_indexes(ROWS, 3);
  }
  it("uses ordered unique occupancy, allowing earlier releases and rejecting later releases") {
    seed(); build_many();
    const turbodb_value_t blocked[] = {turbodb_i64(-1), turbodb_i64(0), turbodb_u64(8), turbodb_i64(INT64_MAX), turbodb_i64(0), turbodb_u64(9)};
    faults_clear();
    check_equal(orm_tidesdb_sql_relation_update_rows(&owner, vstr_from_cstr(table_name), blocked, 2, COLUMNS, &error), TURBODB_STATUS_CONSTRAINT);
    check_equal(put_calls, 0u); check_equal(delete_calls, 0u); verify_live_indexes(ROWS, 3);
    const turbodb_value_t allowed[] = {turbodb_i64(INT64_MAX), turbodb_i64(0), turbodb_u64(9), turbodb_i64(-1), turbodb_i64(0), turbodb_u64(8)};
    check_equal(orm_tidesdb_sql_relation_update_rows(&owner, vstr_from_cstr(table_name), allowed, 2, COLUMNS, &error), TURBODB_STATUS_OK);
    verify_live_indexes(ROWS, 3);
    const turbodb_value_t same[] = {turbodb_i64(INT64_MAX), turbodb_i64(0), turbodb_u64(8), turbodb_i64(-1), turbodb_i64(0), turbodb_u64(9)};
    check_equal(orm_tidesdb_sql_relation_update_rows(&owner, vstr_from_cstr(table_name), same, 2, COLUMNS, &error), TURBODB_STATUS_CONSTRAINT);
    verify_live_indexes(ROWS, 3);
  }
  it("updates unique occupancy owners when primary keys move through a vacated destination") {
    seed(); build_many();
    const turbodb_value_t old[] = {turbodb_i64(INT64_MAX), turbodb_i64(-1)};
    const turbodb_value_t moved[] = {turbodb_i64(2), turbodb_i64(0), turbodb_u64(8), turbodb_i64(INT64_MAX), turbodb_i64(0), turbodb_u64(7)};
    check_equal(orm_tidesdb_sql_relation_move_rows(&owner, vstr_from_cstr(table_name), old, moved, 2, COLUMNS, &error), TURBODB_STATUS_OK);
    verify_live_indexes(ROWS, 3);
  }
  it("maintains indexes through SQL expression updates and predicate deletes") {
    seed(); build_many(); size_t affected = OUTPUT_SENTINEL;
    check_equal(execute_sql("UPDATE items SET a = a + 10 WHERE id = -1", &affected), TURBODB_STATUS_OK); check_equal(affected, 1u);
    verify_live_indexes(ROWS, 3);
    check_equal(execute_sql("DELETE FROM items WHERE a IS NULL", &affected), TURBODB_STATUS_OK); check_equal(affected, 2u);
    verify_live_indexes(ROWS - 2, 3);
    const turbodb_value_t unsigned_seven = turbodb_u64(7);
    check_equal(execute_params("INSERT INTO items(id,a,b) VALUES (10, 10, ?)", &unsigned_seven, 1, &affected), TURBODB_STATUS_CONSTRAINT);
    verify_live_indexes(ROWS - 2, 3);
    check_equal(execute_params("INSERT INTO items(id,a,b) VALUES (10, NULL, ?)", &unsigned_seven, 1, &affected), TURBODB_STATUS_OK); verify_live_indexes(ROWS - 1, 3);
  }
  it("rolls back every indexed put and delete failure as one complete statement") {
    seed(); build_many(); save(); faults_clear(); check_equal(move_one(), TURBODB_STATUS_OK);
    const size_t put_count = put_calls, delete_count = delete_calls; restore();
    const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for (size_t phase = 0; phase < 2; ++phase) {
      const size_t count = phase ? delete_count : put_count;
      for (size_t point = 1; point <= count; ++point) {
        faults_clear(); if (phase) fail_delete = point; else fail_put = point;
        check_equal(move_one(), TURBODB_STATUS_DATASTORE_ERROR); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work); faults_clear();
        version_is(owner.transaction, 5); verify_live_indexes(ROWS, 3);
      }
    }
  }
  it("unwinds every indexed DML reserve, resize and sort failure before native writes") {
    seed(); build_many(); save(); faults_clear(); check_equal(move_one(), TURBODB_STATUS_OK);
    const size_t reserve_count = reserves, resize_count = resizes; restore();
    const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for (size_t phase = 0; phase < 2; ++phase) {
      const size_t count = phase ? resize_count : reserve_count;
      for (size_t point = 1; point <= count; ++point) {
        faults_clear(); if (phase) fail_resize = point; else fail_reserve = point;
        check_equal(move_one(), TURBODB_STATUS_OUT_OF_MEMORY); check_false(owner.failed);
        check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work); faults_clear();
        version_is(owner.transaction, 5); verify_live_indexes(ROWS, 3);
      }
    }
    fail_sort = true; check_equal(move_one(), TURBODB_STATUS_OUT_OF_MEMORY); faults_clear(); verify_live_indexes(ROWS, 3);
  }
  it("keeps format v2 when creating another table and scopes index names to their table") {
    seed(); build_many(); create_table("CREATE TABLE another (id BIGINT PRIMARY KEY, a BIGINT)");
    manifest_is(owner.transaction, 2, 5, 6); uint64_t id = 0;
    check_equal(build_on(&owner, "CREATE INDEX ix ON another (a)", &id), TURBODB_STATUS_OK); check_equal(id, 6u);
    const turbodb_value_t row[] = {turbodb_i64(1), turbodb_i64(9)};
    check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr("another"), row, 2, &error), TURBODB_STATUS_OK);
    manifest_is(owner.transaction, 2, 6, 7); check_equal(namespace_count(owner.transaction, INDEX_DIRECTORY_NS), 4u);
    check_equal(namespace_count(owner.transaction, INDEX_DATA_NS), ROWS * 3u + 1);
  }
  it("serializes concurrent unique claims and concurrent index creation against DML") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    begin_owner(&owner); begin_owner(&other);
    const turbodb_value_t first[] = {turbodb_i64(10), turbodb_i64(9), turbodb_u64(9)}, second[] = {turbodb_i64(11), turbodb_i64(9), turbodb_u64(9)};
    check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr(table_name), first, COLUMNS, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_relation_insert(&other, vstr_from_cstr(table_name), second, COLUMNS, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_BUSY);
    begin_owner(&owner); verify_live_indexes(ROWS + 1, 3); begin_owner(&other); uint64_t id = 0;
    check_equal(build_on(&other, "CREATE INDEX more ON items (b)", &id), TURBODB_STATUS_OK);
    size_t affected = 0; check_equal(execute_sql("UPDATE items SET a = a + 1 WHERE id = 10", &affected), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_BUSY);
    begin_owner(&owner); verify_live_indexes(ROWS + 1, 3);
  }
  it("enforces indexed DML resource limits before the atomic batch") {
    seed(); build_many(); save();
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK); check_equal(move_one(), TURBODB_STATUS_OK);
    const orm_sql_budget_amount peak = budget.peak; restore();
    const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_WORK_BYTES, ORM_SQL_BUDGET_PLAN_NODES,
      ORM_SQL_BUDGET_EXECUTION_STEPS, ORM_SQL_BUDGET_READ_ROWS, ORM_SQL_BUDGET_READ_BYTES,
      ORM_SQL_BUDGET_WRITE_ROWS, ORM_SQL_BUDGET_WRITE_BYTES};
    for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); ++i) {
      const orm_sql_budget_resource resource = resources[i]; const uint64_t original = budget.limits.statement.value[resource];
      for (size_t exact = 0; exact < 2; ++exact) {
        check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
        check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
        budget.limits.statement.value[resource] = peak.value[resource] - (exact ? 0 : 1); faults_clear();
        const turbodb_status_t status = move_one(); budget.limits.statement.value[resource] = original;
        if (exact) { check_equal(status, TURBODB_STATUS_OK); restore(); }
        else { check_equal(status, TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(put_calls, 0u); check_equal(delete_calls, 0u); }
        check_false(owner.failed); verify_live_indexes(ROWS, 3);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], budget.retained_work_bytes);
      }
    }
  }
  it("rejects corrupt directory identity, name, generation and duplicate index IDs before DML") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); begin_owner(&owner);
    const uint8_t key[] = {6,1,0,0,0,0,0,0,0,2,'i','x'};
    enum { TABLE_ID_OFFSET = 4, TABLE_GENERATION_OFFSET = 12, INDEX_ID_OFFSET = 20,
           INDEX_GENERATION_OFFSET = 28, NAME_OFFSET = 41, HEADER_FLAGS = 3 };
    const size_t offsets[] = {TABLE_ID_OFFSET, TABLE_GENERATION_OFFSET, INDEX_ID_OFFSET, INDEX_GENERATION_OFFSET, NAME_OFFSET, HEADER_FLAGS};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
      uint8_t *data = NULL; size_t size = 0;
      check_equal(orm_tidesdb_txn_get(owner.transaction, family, key, sizeof(key), &data, &size), ORM_TDB_SUCCESS);
      data[offsets[i]] = offsets[i] == INDEX_ID_OFFSET ? 3 : (uint8_t)(data[offsets[i]] + 1);
      if (offsets[i] == HEADER_FLAGS) data[offsets[i]] = UINT8_MAX;
      check_equal(orm_tidesdb_txn_put(owner.transaction, family, key, sizeof(key), data, size, 0), ORM_TDB_SUCCESS); orm_tidesdb_free(data);
      faults_clear(); check_equal(move_one(), TURBODB_STATUS_DATASTORE_ERROR); check_true(owner.failed);
      check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); begin_owner(&owner); verify_live_indexes(ROWS, 3);
    }
  }
  it("refuses missing or mismatched old index entries instead of silently repairing them") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); begin_owner(&owner);
    uint8_t key[DATA_KEY_BYTES] = {5,2,0,0,0,0,0,0,0,1};
    /* ix tuple for the old row (-1, 0, unsigned 7), ASC a / DESC b. */
    key[INDEX_PREFIX_BYTES] = 1;
    orm_sql_wire_order_write(key + INDEX_PREFIX_BYTES + 1, orm_sql_wire_signed_order(0));
    key[INDEX_PREFIX_BYTES + 1 + ORM_SQL_WIRE_U64] = UINT8_MAX - 1;
    orm_sql_wire_order_write(key + INDEX_PREFIX_BYTES + 2 + ORM_SQL_WIRE_U64, ~UINT64_C(7));
    orm_sql_wire_order_write(key + DATA_KEY_BYTES - ORM_SQL_WIRE_U64, orm_sql_wire_signed_order(-1));
    for (size_t variant = 0; variant < 3; ++variant) {
      if (!variant) check_equal(orm_tidesdb_txn_delete(owner.transaction, family, key, sizeof(key)), ORM_TDB_SUCCESS);
      else {
        uint8_t wrong[ORM_SQL_WIRE_U64 + 1] = {0};
        check_equal(orm_tidesdb_txn_put(owner.transaction, family, key, sizeof(key), wrong,
            variant == 1 ? ORM_SQL_WIRE_U64 : sizeof(wrong), 0), ORM_TDB_SUCCESS);
      }
      faults_clear(); check_equal(move_one(), TURBODB_STATUS_DATASTORE_ERROR); check_true(owner.failed);
      check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); begin_owner(&owner); verify_live_indexes(ROWS, 3);
    }
  }
  it("propagates every indexed metadata and occupancy read failure without writes") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); begin_owner(&owner);
    save(); faults_clear(); check_equal(move_one(), TURBODB_STATUS_OK); const size_t count = gets; restore();
    for (size_t point = 1; point <= count; ++point) {
      faults_clear(); fail_get = point;
      check_equal(move_one(), TURBODB_STATUS_DATASTORE_ERROR); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
      faults_clear(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
      begin_owner(&owner); verify_live_indexes(ROWS, 3);
    }
  }
  it("unwinds index directory iterator failures without partial Data writes") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); begin_owner(&owner);
    for (int phase = 0; phase <= FAIL_VALUE; ++phase) {
      faults_clear(); if (!phase) fail_new = 1; else fail_iterator = phase;
      check_equal(move_one(), TURBODB_STATUS_DATASTORE_ERROR); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
      faults_clear(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
      begin_owner(&owner); verify_live_indexes(ROWS, 3);
    }
  }
  it("reports unknown commit outcomes without retrying index publication") {
    seed(); build_many(); commit_fault = COMMIT_BEFORE;
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_COMMIT_UNKNOWN);
    faults_clear(); begin_owner(&owner); no_index(1); seed(); build_many();
    commit_fault = COMMIT_AFTER;
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_COMMIT_UNKNOWN); faults_clear();
    orm_tidesdb_transaction_t *tx = NULL; reopen_raw(&tx);
    check_equal(orm_tidesdb_txn_rollback(tx), ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx); begin_owner(&owner);
    verify_live_indexes(ROWS, 3); manifest_is(owner.transaction, 2, 4, 5);
  }
  it("drops only the named index and never recycles IDs or downgrades the Manifest") {
    seed(); build_many(); uint64_t id = OUTPUT_SENTINEL;
    check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_OK); check_equal(id, 2u);
    manifest_is(owner.transaction, 2, 4, 5); version_is(owner.transaction, 6); verify_live_indexes(ROWS, 2);
    const turbodb_value_t duplicate[] = {turbodb_i64(10), turbodb_i64(0), turbodb_u64(7)};
    check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr(table_name), duplicate, COLUMNS, &error), TURBODB_STATUS_OK);
    verify_live_indexes(ROWS + 1, 2);
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_CONSTRAINT);
    check_equal(build_on(&owner, ordinary_sql, &id), TURBODB_STATUS_OK); check_equal(id, 5u);
    check_equal(drop_on(&owner, "DROP INDEX by_b ON items", &id), TURBODB_STATUS_OK); check_equal(id, 3u);
    verify_live_indexes(ROWS + 1, 2);
    check_equal(drop_on(&owner, "DROP INDEX by_id ON items", &id), TURBODB_STATUS_OK); check_equal(id, 4u);
    check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_OK); check_equal(id, 5u);
    verify_live_indexes(ROWS + 1, 0); manifest_is(owner.transaction, 2, 5, 6);
    check_equal(move_one(), TURBODB_STATUS_CONSTRAINT);
    size_t affected = 0; check_equal(execute_sql("DELETE FROM items WHERE id=10", &affected), TURBODB_STATUS_OK);
    check_equal(affected, 1u); check_equal(move_one(), TURBODB_STATUS_OK); verify_live_indexes(ROWS, 0);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    orm_tidesdb_transaction_t *tx = NULL; reopen_raw(&tx);
    manifest_is(tx, 2, 5, 6); check_equal(namespace_count(tx, INDEX_DIRECTORY_NS), 0u);
    check_equal(orm_tidesdb_txn_rollback(tx), ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx); begin_owner(&owner);
    verify_live_indexes(ROWS, 0); check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_OK); check_equal(id, 6u);
    verify_live_indexes(ROWS, 1);
  }
  it("drops an empty index and restores directory and constraints through a user savepoint") {
    uint64_t id = 0; check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_OK); save(); faults_clear();
    check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_OK);
    check_equal(delete_calls, 1u); check_equal(put_calls, 1u); verify_live_indexes(0, 0);
    manifest_is(owner.transaction, 2, 2, 3); version_is(owner.transaction, 3);
    restore(); verify_live_indexes(0, 1); version_is(owner.transaction, 2);
    seed(); save(); check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_OK);
    verify_live_indexes(ROWS, 0); restore(); verify_entries(owner.transaction, true);
    const turbodb_value_t duplicate[] = {turbodb_i64(10), turbodb_i64(0), turbodb_u64(7)};
    check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr(table_name), duplicate, COLUMNS, &error), TURBODB_STATUS_CONSTRAINT);
  }
  it("rolls back every DROP INDEX delete and final version put failure") {
    seed(); build_many(); save(); faults_clear(); uint64_t id = 0;
    check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_OK);
    const size_t deletes = delete_calls, puts = put_calls; restore();
    check_equal(deletes, ROWS + ROWS - 2u + 1u); check_equal(puts, 1u);
    const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for (size_t point = 0; point <= deletes; ++point) {
      faults_clear(); if (!point) fail_put = 1; else fail_delete = point; id = OUTPUT_SENTINEL;
      check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(id, OUTPUT_SENTINEL); check_false(owner.failed); faults_clear();
      version_is(owner.transaction, 5); verify_live_indexes(ROWS, 3); manifest_is(owner.transaction, 2, 4, 5);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
    }
  }
  it("unwinds every DROP INDEX reserve, resize and sort failure before deletion") {
    seed(); build_many(); save(); faults_clear(); uint64_t id = 0;
    check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_OK);
    const size_t reserve_count = reserves, resize_count = resizes; restore();
    const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for (size_t phase = 0; phase < 3; ++phase) {
      const size_t count = phase == 2 ? 1 : phase ? resize_count : reserve_count;
      for (size_t point = 1; point <= count; ++point) {
        faults_clear(); if (phase == 2) fail_sort = true; else if (phase) fail_resize = point; else fail_reserve = point;
        id = OUTPUT_SENTINEL; check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_OUT_OF_MEMORY);
        check_equal(id, OUTPUT_SENTINEL); check_false(owner.failed); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
        faults_clear(); verify_live_indexes(ROWS, 3);
      }
    }
  }
  it("propagates DROP INDEX point read and iterator failures with no storage mutation") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); begin_owner(&owner);
    save(); faults_clear(); uint64_t id = 0; check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_OK);
    const size_t read_count = gets, iterator_count = news; restore();
    for (size_t phase = 0; phase < 3; ++phase) {
      const size_t count = phase == 2 ? FAIL_VALUE : phase ? iterator_count : read_count;
      for (size_t point = 1; point <= count; ++point) {
        faults_clear(); if (phase == 2) fail_iterator = (int)point; else if (phase) fail_new = point; else fail_get = point;
        id = OUTPUT_SENTINEL; check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_DATASTORE_ERROR);
        check_equal(id, OUTPUT_SENTINEL); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
        faults_clear(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
        begin_owner(&owner); verify_live_indexes(ROWS, 3);
      }
    }
  }
  it("enforces exact DROP INDEX resource limits and releases all materialized rows") {
    seed(); build_many(); save(); uint64_t id = 0;
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_OK);
    const orm_sql_budget_amount peak = budget.peak; restore();
    const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_WORK_BYTES, ORM_SQL_BUDGET_PLAN_NODES,
      ORM_SQL_BUDGET_MATERIALIZED_ROWS, ORM_SQL_BUDGET_AST_NODES, ORM_SQL_BUDGET_EXECUTION_STEPS,
      ORM_SQL_BUDGET_READ_ROWS, ORM_SQL_BUDGET_READ_BYTES, ORM_SQL_BUDGET_WRITE_ROWS, ORM_SQL_BUDGET_WRITE_BYTES};
    for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); ++i) {
      const orm_sql_budget_resource resource = resources[i]; const uint64_t original = budget.limits.statement.value[resource];
      check_true(peak.value[resource] > 0);
      for (size_t exact = 0; exact < 2; ++exact) {
        check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
        check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
        budget.limits.statement.value[resource] = peak.value[resource] - (exact ? 0 : 1); faults_clear();
        id = OUTPUT_SENTINEL; const turbodb_status_t status = drop_on(&owner, "DROP INDEX ix ON items", &id);
        budget.limits.statement.value[resource] = original;
        if (exact) { check_equal(status, TURBODB_STATUS_OK); restore(); }
        else {
          check_equal(status, TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(id, OUTPUT_SENTINEL);
          check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
        }
        check_false(owner.failed); verify_live_indexes(ROWS, 3);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], budget.retained_work_bytes);
      }
    }
  }
  it("serializes DROP INDEX against DML in either commit order") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    for (size_t drop_first = 0; drop_first < 2; ++drop_first) {
      begin_owner(&owner); begin_owner(&other); uint64_t id = 0;
      check_equal(drop_on(&other, "DROP INDEX ix ON items", &id), TURBODB_STATUS_OK);
      size_t affected = 0; check_equal(execute_sql("UPDATE items SET a=a+1 WHERE id=-1", &affected), TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(drop_first ? &other : &owner, true, &error), TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(drop_first ? &owner : &other, true, &error), TURBODB_STATUS_BUSY);
      begin_owner(&owner); verify_live_indexes(ROWS, drop_first ? 2 : 3);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    }
  }
  it("serializes concurrent DROP INDEX and index creation without losing directories") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    begin_owner(&owner); begin_owner(&other); uint64_t id = 0;
    check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_OK);
    check_equal(drop_on(&other, "DROP INDEX ix ON items", &id), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_BUSY);
    begin_owner(&owner); begin_owner(&other);
    check_equal(drop_on(&owner, "DROP INDEX by_b ON items", &id), TURBODB_STATUS_OK);
    check_equal(build_on(&other, "CREATE INDEX more ON items (a)", &id), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_BUSY);
    begin_owner(&owner); verify_live_indexes(ROWS, 3);
  }
  it("rejects orphan generations and malformed physical entries instead of leaving unreachable keys") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    for (size_t variant = 0; variant < 3; ++variant) {
      begin_owner(&owner);
      uint8_t key[DATA_KEY_BYTES] = {5,2,0,0,0,0,0,0,0,1}, value[ORM_SQL_WIRE_U64] = {0};
      if (!variant) key[1 + ORM_SQL_WIRE_U64] = 2;
      const size_t size = variant == 1 ? INDEX_PREFIX_BYTES : sizeof(key);
      check_equal(orm_tidesdb_txn_put(owner.transaction, family, key, size, value, sizeof(value), 0), ORM_TDB_SUCCESS);
      faults_clear(); uint64_t id = OUTPUT_SENTINEL;
      check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(id, OUTPUT_SENTINEL); check_true(owner.failed); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    }
    begin_owner(&owner); verify_live_indexes(ROWS, 3);
  }
  it("rejects missing or mismatched DROP INDEX entries before deleting the directory") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    for (size_t variant = 0; variant < 3; ++variant) {
      begin_owner(&owner); orm_tidesdb_iterator_t *it = NULL;
      const uint8_t prefix[] = {5,2,0,0,0,0,0,0,0}; uint8_t key[DATA_KEY_BYTES], *view = NULL; size_t size = 0;
      check_equal(orm_tidesdb_iter_new(owner.transaction, family, &it), ORM_TDB_SUCCESS);
      check_equal(orm_tidesdb_iter_seek(it, prefix, sizeof(prefix)), ORM_TDB_SUCCESS);
      check_equal(orm_tidesdb_iter_key(it, &view, &size), ORM_TDB_SUCCESS);
      check_equal(size, sizeof(key)); memcpy(key, view, sizeof(key)); orm_tidesdb_iter_free(it);
      if (!variant) check_equal(orm_tidesdb_txn_delete(owner.transaction, family, key, sizeof(key)), ORM_TDB_SUCCESS);
      else {
        uint8_t wrong[ORM_SQL_WIRE_U64 + 1]; memset(wrong, UINT8_MAX, sizeof(wrong));
        check_equal(orm_tidesdb_txn_put(owner.transaction, family, key, sizeof(key), wrong,
            variant == 1 ? ORM_SQL_WIRE_U64 : sizeof(wrong), 0), ORM_TDB_SUCCESS);
      }
      faults_clear(); uint64_t id = OUTPUT_SENTINEL;
      check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(id, OUTPUT_SENTINEL); check_true(owner.failed); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    }
    begin_owner(&owner); verify_live_indexes(ROWS, 3);
  }
  it("preserves DROP INDEX atomicity across savepoint creation and cleanup failures") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    for (size_t variant = 0; variant < 3; ++variant) {
      begin_owner(&owner); faults_clear();
      if (!variant) fail_savepoint = true;
      else if (variant == 1) { fail_delete = 2; fail_rollback = true; }
      else fail_release = true;
      uint64_t id = OUTPUT_SENTINEL;
      check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(id, OUTPUT_SENTINEL); check_equal(owner.failed, variant != 0);
      if (variant) check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_INVALID_STATE);
      faults_clear(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
      begin_owner(&owner); verify_live_indexes(ROWS, 3);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    }
  }
  it("reports unknown DROP INDEX commit outcomes and reopens either complete state") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    for (int fault = COMMIT_BEFORE; fault <= COMMIT_AFTER; ++fault) {
      begin_owner(&owner); uint64_t id = 0;
      check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_OK); commit_fault = fault;
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_COMMIT_UNKNOWN); faults_clear();
      orm_tidesdb_transaction_t *tx = NULL; reopen_raw(&tx);
      manifest_is(tx, 2, 4, 5);
      check_equal(orm_tidesdb_txn_rollback(tx), ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx);
      begin_owner(&owner); verify_live_indexes(ROWS, fault == COMMIT_BEFORE ? 3 : 2);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    }
  }
  it("rejects unsupported or absent DROP INDEX targets, parameters and active sources") {
    seed(); uint64_t id = OUTPUT_SENTINEL;
    check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_SQL_ERROR);
    build_many(); faults_clear();
    const char *invalid[] = {"DROP INDEX ix ON missing", "DROP INDEX missing ON items", "DROP INDEX IX ON items"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
      check_equal(drop_on(&owner, invalid[i], &id), TURBODB_STATUS_SQL_ERROR);
    check_equal(drop_on(&owner, "DROP INDEX `PRIMARY` ON items", &id), TURBODB_STATUS_UNSUPPORTED);
    check_equal(drop_on(&owner, "DROP INDEX ix ON db.items", &id), TURBODB_STATUS_UNSUPPORTED);
    sqlparser_document *doc = NULL; sqlparser_error diagnostic;
    const char sqlite_sql[] = "DROP INDEX IF EXISTS ix";
    check_equal(sqlparser_parse_dialect(sqlite_sql, strlen(sqlite_sql), SQLPARSER_SQLITE, NULL, &doc, &diagnostic), SQLPARSER_OK);
    check_equal(orm_tidesdb_sql_index_drop(doc, &owner, &id, &error), TURBODB_STATUS_UNSUPPORTED); sqlparser_document_destroy(doc);
    size_t affected = OUTPUT_SENTINEL; const turbodb_value_t parameter = turbodb_i64(1);
    check_equal(execute_params("DROP INDEX ix ON items", &parameter, 1, &affected), TURBODB_STATUS_SQL_ERROR);
    check_equal(affected, OUTPUT_SENTINEL);
    orm_sql_relation_source source = {0};
    check_equal(orm_tidesdb_sql_relation_open(&owner, vstr_from_cstr(table_name), &source, &error), TURBODB_STATUS_OK);
    check_equal(drop_on(&owner, "DROP INDEX ix ON items", &id), TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_relation_close(&source, &error), TURBODB_STATUS_OK);
    check_equal(id, OUTPUT_SENTINEL); check_equal(put_calls, 0u); check_equal(delete_calls, 0u); verify_live_indexes(ROWS, 3);
    check_equal(execute_sql("DROP INDEX ix ON items", &affected), TURBODB_STATUS_OK); check_equal(affected, 0u);
    verify_live_indexes(ROWS, 2);
  }
  it("reads only full-key index matches and rechecks residual equalities") {
    seed(); build_many();
    check_equal(lookup_open("SELECT id FROM items WHERE a=0 AND b=7", NULL, 0), TURBODB_STATUS_OK);
    check_not_null(lookup_query.as.select.source.lookup.budget);
    check_equal(lookup_query.as.select.source.lookup.record.identity.index_id, 2u);
    const uint64_t before = budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    lookup_row(-1); lookup_end(); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS] - before, 3u);
    check_equal(lookup_open("SELECT id FROM items t WHERE 7=t.b AND t.a=0 AND t.id=1", NULL, 0), TURBODB_STATUS_OK);
    check_not_null(lookup_query.as.select.source.lookup.budget); lookup_end();
    check_equal(lookup_open("SELECT id FROM items WHERE a=0 AND b=7 AND a=1", NULL, 0), TURBODB_STATUS_OK); lookup_end();
    verify_live_indexes(ROWS, 3);
  }
  it("clears base-format empty and populated tables with rollback and monotonic recreation") {
    size_t affected = OUTPUT_SENTINEL;
    check_equal(execute_sql("TRUNCATE items", &affected), TURBODB_STATUS_OK); check_equal(affected, 0u); version_is(owner.transaction, 2);
    seed(); save();
    check_equal(execute_sql("TRUNCATE TABLE items", &affected), TURBODB_STATUS_OK); version_is(owner.transaction, 4);
    check_equal(namespace_count(owner.transaction, REL_KIND), 0u); table_is(true, 1); manifest_is(owner.transaction, 1, 1, 2);
    restore(); check_equal(namespace_count(owner.transaction, REL_KIND), ROWS); version_is(owner.transaction, 3);
    check_equal(execute_sql("DROP TABLE items", &affected), TURBODB_STATUS_OK); check_equal(affected, 0u); table_is(false, 0);
    check_equal(namespace_count(owner.transaction, REL_KIND), 0u); check_equal(namespace_count(owner.transaction, 2), 0u);
    manifest_is(owner.transaction, 1, 1, 2);
    create_table(table_sql); table_is(true, 2); seed(); restore(); table_is(true, 1);
    check_equal(namespace_count(owner.transaction, REL_KIND), ROWS); manifest_is(owner.transaction, 1, 1, 2);
  }
  it("truncates all index entries while preserving definitions IDs and unique enforcement") {
    seed(); build_many(); save(); size_t affected = OUTPUT_SENTINEL;
    check_equal(execute_sql("TRUNCATE TABLE items", &affected), TURBODB_STATUS_OK); check_equal(affected, 0u);
    verify_live_indexes(0, 3); version_is(owner.transaction, 6); manifest_is(owner.transaction, 2, 4, 5); table_is(true, 1);
    seed(); verify_live_indexes(ROWS, 3);
    const turbodb_value_t duplicate[] = {turbodb_i64(8),turbodb_i64(0),turbodb_u64(7)};
    check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr(table_name), duplicate, COLUMNS, &error), TURBODB_STATUS_CONSTRAINT);
    restore(); verify_live_indexes(ROWS, 3); version_is(owner.transaction, 5);
    check_equal(execute_sql("TRUNCATE TABLE items", &affected), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    orm_tidesdb_transaction_t *tx = NULL; reopen_raw(&tx); manifest_is(tx, 2, 4, 5);
    check_equal(orm_tidesdb_txn_rollback(tx), ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx); begin_owner(&owner);
    verify_live_indexes(0, 3); seed(); verify_live_indexes(ROWS, 3);
  }
  it("drops every table index and recreates with new identities without downgrading v2") {
    seed(); build_many(); save(); size_t affected = OUTPUT_SENTINEL;
    check_equal(execute_sql("DROP TABLE IF EXISTS items", &affected), TURBODB_STATUS_OK); table_is(false, 0); check_equal(affected, 0u);
    for (uint8_t space = 1; space <= INDEX_DIRECTORY_NS; ++space) check_equal(namespace_count(owner.transaction, space), 0u);
    manifest_is(owner.transaction, 2, 4, 5); restore(); verify_live_indexes(ROWS, 3);
    check_equal(execute_sql("DROP TABLE items", &affected), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    orm_tidesdb_transaction_t *tx = NULL; reopen_raw(&tx); manifest_is(tx, 2, 4, 5);
    check_equal(orm_tidesdb_txn_rollback(tx), ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx); begin_owner(&owner); table_is(false, 0);
    create_table(table_sql); table_is(true, 5); seed(); uint64_t id = 0;
    check_equal(build_on(&owner, unique_sql, &id), TURBODB_STATUS_OK); check_equal(id, 6u); verify_live_indexes(ROWS, 1);
  }
  it("preserves other tables and their index namespaces during table clear") {
    seed(); build_many(); create_table("CREATE TABLE kept(id BIGINT PRIMARY KEY,a BIGINT,b BIGINT UNSIGNED)");
    const turbodb_value_t row[] = {turbodb_i64(9),turbodb_i64(10),turbodb_u64(11)};
    check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr("kept"), row, COLUMNS, &error), TURBODB_STATUS_OK);
    uint64_t id = 0; check_equal(build_on(&owner, "CREATE UNIQUE INDEX ux ON kept(a)", &id), TURBODB_STATUS_OK); save();
    const char *sql[] = {"TRUNCATE TABLE items", "DROP TABLE items"};
    for (size_t i = 0; i < 2; ++i) {
      check_equal(clear_on(&owner, sql[i]), TURBODB_STATUS_OK);
      check_equal(lookup_open("SELECT id FROM kept WHERE a=10", NULL, 0), TURBODB_STATUS_OK); lookup_row(9); lookup_end();
      check_equal(namespace_count(owner.transaction, REL_KIND), 1u); check_equal(namespace_count(owner.transaction, INDEX_DATA_NS), 1u);
      restore();
    }
  }
  it("rolls back every partial multi-table DROP batch without losing either table") {
    seed(); build_many(); create_table("CREATE TABLE kept(id BIGINT PRIMARY KEY,a BIGINT,b BIGINT UNSIGNED)");
    const turbodb_value_t row[] = {turbodb_i64(9),turbodb_i64(10),turbodb_u64(11)};
    check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr("kept"), row, COLUMNS, &error), TURBODB_STATUS_OK);
    uint64_t id = 0; check_equal(build_on(&owner, "CREATE INDEX keep_ix ON kept(a)", &id), TURBODB_STATUS_OK); save();
    faults_clear(); check_equal(clear_on(&owner, "DROP TABLE items,kept"), TURBODB_STATUS_OK);
    const size_t deletes = delete_calls, puts = put_calls; check_true(deletes > 0); check_equal(puts, 2u); restore();
    for (size_t phase = 0; phase < 2; ++phase) {
      const size_t count = phase ? puts : deletes;
      for (size_t point = 1; point <= count; ++point) {
        faults_clear(); if (phase) fail_put = point; else fail_delete = point;
        check_equal(clear_on(&owner, "DROP TABLE items,kept"), TURBODB_STATUS_DATASTORE_ERROR);
        check_false(owner.failed); faults_clear(); verify_live_indexes_extra(ROWS, 3, 1, 0, 1);
        check_equal(lookup_open("SELECT id FROM kept WHERE a=10", NULL, 0), TURBODB_STATUS_OK); lookup_row(9); lookup_end();
      }
    }
  }
  it("validates table DDL shapes and missing targets before storage mutation") {
    seed(); build_many(); const char *unsupported[] = {"DROP TEMPORARY TABLE items", "DROP TABLE db.items", "TRUNCATE db.items"};
    for (size_t i = 0; i < sizeof(unsupported)/sizeof(unsupported[0]); ++i) {
      faults_clear(); check_equal(clear_on(&owner, unsupported[i]), TURBODB_STATUS_UNSUPPORTED);
      check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
    }
    const char *missing[] = {"DROP TABLE missing", "TRUNCATE TABLE missing"};
    for (size_t i = 0; i < 2; ++i) check_equal(clear_on(&owner, missing[i]), TURBODB_STATUS_SQL_ERROR);
    faults_clear(); check_equal(clear_on(&owner, "DROP TABLE items,missing"), TURBODB_STATUS_SQL_ERROR);
    check_equal(put_calls, 0u); check_equal(delete_calls, 0u); verify_live_indexes(ROWS, 3);
    faults_clear(); check_equal(clear_on(&owner, "DROP TABLE IF EXISTS missing"), TURBODB_STATUS_OK);
    check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
    const char *sql[] = {"DROP TABLE items", "TRUNCATE TABLE items"};
    const turbodb_value_t parameter = turbodb_i64(1); size_t affected = OUTPUT_SENTINEL;
    for (size_t i = 0; i < 2; ++i) {
      check_equal(execute_params(sql[i], &parameter, 1, &affected), TURBODB_STATUS_SQL_ERROR); check_equal(affected, OUTPUT_SENTINEL);
    }
    orm_sql_relation_source source = {0};
    check_equal(orm_tidesdb_sql_relation_open(&owner, vstr_from_cstr(table_name), &source, &error), TURBODB_STATUS_OK);
    for (size_t i = 0; i < 2; ++i) check_equal(clear_on(&owner, sql[i]), TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_relation_close(&source, &error), TURBODB_STATUS_OK); verify_live_indexes(ROWS, 3);
  }
  it("rolls back each table clear delete and final put failure without losing earlier writes") {
    seed(); build_many(); save(); const char *sql[] = {"TRUNCATE TABLE items", "DROP TABLE items"};
    for (size_t op = 0; op < 2; ++op) {
      faults_clear(); check_equal(clear_on(&owner, sql[op]), TURBODB_STATUS_OK); const size_t deletes = delete_calls, puts = put_calls; restore();
      for (size_t phase = 0; phase < 2; ++phase) for (size_t point = 1; point <= (phase ? puts : deletes); ++point) {
        faults_clear(); if (phase) fail_put = point; else fail_delete = point;
        size_t affected = OUTPUT_SENTINEL; check_equal(execute_sql(sql[op], &affected), TURBODB_STATUS_DATASTORE_ERROR);
        check_equal(affected, OUTPUT_SENTINEL); check_false(owner.failed); faults_clear(); verify_live_indexes(ROWS, 3);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
      }
    }
  }
  it("unwinds every table clear vector allocation and sorting failure before deletion") {
    seed(); build_many(); save(); const char *sql[] = {"TRUNCATE TABLE items", "DROP TABLE items"};
    for (size_t op = 0; op < 2; ++op) {
      faults_clear(); check_equal(clear_on(&owner, sql[op]), TURBODB_STATUS_OK); const size_t rs = reserves, rz = resizes; restore();
      const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for (size_t phase = 0; phase < 3; ++phase) for (size_t point = 1; point <= (phase == 2 ? 1 : phase ? rz : rs); ++point) {
        faults_clear(); if (phase == 2) fail_sort = true; else if (phase) fail_resize = point; else fail_reserve = point;
        check_equal(clear_on(&owner, sql[op]), TURBODB_STATUS_OUT_OF_MEMORY);
        check_equal(put_calls, 0u); check_equal(delete_calls, 0u); check_false(owner.failed);
        check_equal(owner.active_sources, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
      }
      faults_clear(); verify_live_indexes(ROWS, 3);
    }
  }
  it("enforces exact table clear quotas before publishing writes") {
    seed(); build_many(); save(); const char *sql[] = {"TRUNCATE TABLE items", "DROP TABLE items"};
    const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_WORK_BYTES, ORM_SQL_BUDGET_PLAN_NODES,
      ORM_SQL_BUDGET_MATERIALIZED_ROWS, ORM_SQL_BUDGET_AST_NODES, ORM_SQL_BUDGET_EXECUTION_STEPS,
      ORM_SQL_BUDGET_READ_ROWS, ORM_SQL_BUDGET_READ_BYTES, ORM_SQL_BUDGET_WRITE_ROWS, ORM_SQL_BUDGET_WRITE_BYTES};
    for (size_t op = 0; op < 2; ++op) {
      check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK); check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
      check_equal(clear_on(&owner, sql[op]), TURBODB_STATUS_OK); const orm_sql_budget_amount peak = budget.peak; restore();
      for (size_t i = 0; i < sizeof(resources)/sizeof(resources[0]); ++i) {
        const orm_sql_budget_resource resource = resources[i]; const uint64_t original = budget.limits.statement.value[resource];
        check_true(peak.value[resource] > 0);
        for (size_t exact = 0; exact < 2; ++exact) {
          check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK); check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
          budget.limits.statement.value[resource] = peak.value[resource] - (exact ? 0 : 1); faults_clear();
          const turbodb_status_t status = clear_on(&owner, sql[op]); budget.limits.statement.value[resource] = original;
          check_equal(status, exact ? TURBODB_STATUS_OK : TURBODB_STATUS_LIMIT_EXCEEDED);
          if (exact) restore(); else { check_equal(put_calls, 0u); check_equal(delete_calls, 0u); }
          check_false(owner.failed); verify_live_indexes(ROWS, 3); check_equal(owner.active_sources, 0u);
          check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
          check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], budget.retained_work_bytes);
        }
      }
    }
  }
  it("detects missing index entries and orphan Data generations before table clear") {
    seed(); build_many(); save(); const char *sql[] = {"TRUNCATE TABLE items", "DROP TABLE items"};
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    for (size_t op = 0; op < 2; ++op) for (size_t bad = 0; bad < 3; ++bad) {
      begin_owner(&owner);
      if (!bad) {
        uint8_t key[DATA_KEY_BYTES], *view = NULL; size_t size = 0; orm_tidesdb_iterator_t *iterator = NULL;
        const uint8_t prefix[] = {INDEX_DATA_NS,2,0,0,0,0,0,0,0};
        check_equal(orm_tidesdb_iter_new(owner.transaction, family, &iterator), ORM_TDB_SUCCESS);
        check_equal(orm_tidesdb_iter_seek(iterator, prefix, sizeof(prefix)), ORM_TDB_SUCCESS);
        check_equal(orm_tidesdb_iter_key(iterator, &view, &size), ORM_TDB_SUCCESS);
        check_equal(size, sizeof(key)); memcpy(key, view, size); orm_tidesdb_iter_free(iterator);
        check_equal(orm_tidesdb_txn_delete(owner.transaction, family, key, sizeof(key)), ORM_TDB_SUCCESS);
      } else {
        uint8_t key[ORM_SQL_RELATION_KEY_BYTES]; relation_prefix(1, key);
        orm_sql_wire_write(key + REL_GENERATION, ORM_SQL_WIRE_U64, 2);
        orm_sql_wire_order_write(key + ORM_SQL_RELATION_PREFIX_BYTES, 0);
        const uint8_t value[] = {1};
        check_equal(orm_tidesdb_txn_put(owner.transaction, family, key, bad == 1 ? sizeof(key) : ORM_SQL_RELATION_PREFIX_BYTES,
            value, sizeof(value), 0), ORM_TDB_SUCCESS);
      }
      faults_clear(); check_equal(clear_on(&owner, sql[op]), TURBODB_STATUS_DATASTORE_ERROR); check_true(owner.failed);
      check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    }
    begin_owner(&owner); verify_live_indexes(ROWS, 3);
  }
  it("rejects exhausted TRUNCATE versions but permits removing the exhausted table") {
    seed(); build_many(); uint8_t key[ORM_SQL_STORE_VERSION_KEY_BYTES], stamp[ORM_SQL_WIRE_U64];
    orm_sql_store_version_key(1, key); orm_sql_wire_write(stamp, sizeof(stamp), UINT64_MAX);
    check_equal(orm_tidesdb_txn_put(owner.transaction, family, key, sizeof(key), stamp, sizeof(stamp), 0), ORM_TDB_SUCCESS);
    faults_clear(); size_t affected = OUTPUT_SENTINEL;
    check_equal(execute_sql("TRUNCATE TABLE items", &affected), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(affected, OUTPUT_SENTINEL); check_equal(put_calls, 0u); check_equal(delete_calls, 0u); check_false(owner.failed);
    verify_live_indexes(ROWS, 3);
    check_equal(execute_sql("DROP TABLE items", &affected), TURBODB_STATUS_OK); check_equal(affected, 0u); table_is(false, 0);
  }
  it("serializes table clear with DML in both commit orders") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    const char *sql[] = {"TRUNCATE TABLE items", "DROP TABLE items"};
    for (size_t op = 0; op < 2; ++op) {
      for (size_t ddl_first = 0; ddl_first < 2; ++ddl_first) {
        begin_owner(&owner); begin_owner(&other);
        check_equal(clear_on(&other, sql[op]), TURBODB_STATUS_OK); size_t affected = 0;
        check_equal(execute_sql("UPDATE items SET a=a+1 WHERE id=-1", &affected), TURBODB_STATUS_OK);
        check_equal(orm_tidesdb_sql_catalog_finish(ddl_first ? &other : &owner, true, &error), TURBODB_STATUS_OK);
        check_equal(orm_tidesdb_sql_catalog_finish(ddl_first ? &owner : &other, true, &error), TURBODB_STATUS_BUSY);
      }
      begin_owner(&owner);
      if (!op) { verify_live_indexes(0, 3); seed(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); }
      else table_is(false, 0);
    }
  }
  it("conflicts empty-table TRUNCATE with a concurrent insert") {
    build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    for (size_t ddl_first = 0; ddl_first < 2; ++ddl_first) {
      begin_owner(&owner); begin_owner(&other); check_equal(clear_on(&other, "TRUNCATE TABLE items"), TURBODB_STATUS_OK); seed();
      check_equal(orm_tidesdb_sql_catalog_finish(ddl_first ? &other : &owner, true, &error), TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(ddl_first ? &owner : &other, true, &error), TURBODB_STATUS_BUSY);
      begin_owner(&owner); check_equal(clear_on(&owner, "TRUNCATE TABLE items"), TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    }
    begin_owner(&owner); verify_live_indexes(0, 3);
  }
  it("propagates every table clear point read and iterator fault without deleting data") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); begin_owner(&owner);
    const char *sql[] = {"TRUNCATE TABLE items", "DROP TABLE items"};
    for (size_t op = 0; op < 2; ++op) {
      save(); faults_clear(); check_equal(clear_on(&owner, sql[op]), TURBODB_STATUS_OK); const size_t reads = gets, iterators = news; restore();
      for (size_t phase = 0; phase < 3; ++phase) for (size_t point = 1; point <= (phase == 2 ? FAIL_VALUE : phase ? iterators : reads); ++point) {
        faults_clear(); if (phase == 2) fail_iterator = (int)point; else if (phase) fail_new = point; else fail_get = point;
        check_equal(clear_on(&owner, sql[op]), TURBODB_STATUS_DATASTORE_ERROR);
        check_equal(put_calls, 0u); check_equal(delete_calls, 0u); check_equal(owner.active_sources, 0u);
        faults_clear(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); begin_owner(&owner);
        verify_live_indexes(ROWS, 3);
      }
    }
  }
  it("requires full rollback after table clear savepoint cleanup failure") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); begin_owner(&owner);
    const char *sql[] = {"TRUNCATE TABLE items", "DROP TABLE items"};
    for (size_t op = 0; op < 2; ++op) for (size_t phase = 0; phase < 3; ++phase) {
      faults_clear(); if (!phase) fail_savepoint = true; else if (phase == 1) fail_release = true;
      else { fail_delete = 1; fail_rollback = true; }
      size_t affected = OUTPUT_SENTINEL; check_equal(execute_sql(sql[op], &affected), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(affected, OUTPUT_SENTINEL); check_equal(owner.failed, phase != 0); check_equal(owner.active_sources, 0u);
      if (!phase) { check_equal(put_calls, 0u); check_equal(delete_calls, 0u); }
      faults_clear(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); begin_owner(&owner);
      verify_live_indexes(ROWS, 3);
    }
  }
  it("serializes DROP directory removal with unrelated table creation through the Manifest barrier") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    const char sql[] = "CREATE TABLE kept(id BIGINT PRIMARY KEY)"; sqlparser_document *doc = NULL; sqlparser_error diagnostic;
    check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &diagnostic), SQLPARSER_OK); enum { DEPTH = 32 };
    for (size_t drop_first = 0; drop_first < 2; ++drop_first) {
      begin_owner(&owner); begin_owner(&other); check_equal(clear_on(&owner, "DROP TABLE items"), TURBODB_STATUS_OK); size_t affected = OUTPUT_SENTINEL;
      check_equal(orm_tidesdb_sql_runtime_execute(doc, &other, NULL, 0, DEPTH, 0, false, NULL, &affected, &error), TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(drop_first ? &owner : &other, true, &error), TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(drop_first ? &other : &owner, true, &error), TURBODB_STATUS_BUSY);
      begin_owner(&owner);
      if (!drop_first) {
        verify_live_indexes(ROWS, 3); check_equal(clear_on(&owner, "DROP TABLE kept"), TURBODB_STATUS_OK);
        check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
      } else { table_is(false, 0); check_equal(namespace_count(owner.transaction, 1), 0u); }
    }
    sqlparser_document_destroy(doc);
  }
  it("reopens either complete table clear outcome after uncertain native commits") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    const char *sql[] = {"TRUNCATE TABLE items", "DROP TABLE items"};
    for (size_t op = 0; op < 2; ++op) {
      for (int fault = COMMIT_BEFORE; fault <= COMMIT_AFTER; ++fault) {
        begin_owner(&owner); check_equal(clear_on(&owner, sql[op]), TURBODB_STATUS_OK); commit_fault = fault;
        check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_COMMIT_UNKNOWN); faults_clear();
        orm_tidesdb_transaction_t *tx = NULL; reopen_raw(&tx); manifest_is(tx, 2, 4, 5);
        check_equal(orm_tidesdb_txn_rollback(tx), ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx); begin_owner(&owner);
        if (op && fault == COMMIT_AFTER) {
          table_is(false, 0); for (uint8_t space = 1; space <= INDEX_DIRECTORY_NS; ++space) check_equal(namespace_count(owner.transaction, space), 0u);
        } else verify_live_indexes(fault == COMMIT_BEFORE ? ROWS : 0, 3);
        if (!op && fault == COMMIT_AFTER) seed();
        check_equal(orm_tidesdb_sql_catalog_finish(&owner, !op && fault == COMMIT_AFTER, &error), TURBODB_STATUS_OK);
      }
    }
    begin_owner(&owner);
  }
  it("renames indexed tables without changing identity rows or uniqueness and rolls back atomically") {
    seed(); build_many(); save(); faults_clear(); size_t affected = OUTPUT_SENTINEL;
    check_equal(execute_sql("ALTER TABLE items RENAME TO renamed", &affected), TURBODB_STATUS_OK);
    check_equal(affected, 0u); check_equal(put_calls, 6u); check_equal(delete_calls, 1u);
    table_is(false, 0); manifest_is(owner.transaction, 2, 4, 5); version_is(owner.transaction, 6);
    check_equal(lookup_open("SELECT id FROM renamed WHERE a=0 AND b=7", NULL, 0), TURBODB_STATUS_OK);
    check_not_null(lookup_query.as.select.source.lookup.budget); lookup_row(-1); lookup_end();
    const turbodb_value_t unsigned_value = turbodb_u64(7);
    check_equal(execute_params("INSERT INTO renamed(id,a,b) VALUES(10,0,?)", &unsigned_value, 1, &affected), TURBODB_STATUS_CONSTRAINT);
    check_equal(execute_sql("UPDATE renamed SET a=9 WHERE id=-1", &affected), TURBODB_STATUS_OK);
    check_equal(lookup_open("SELECT id FROM renamed WHERE a=9 AND b=7", NULL, 0), TURBODB_STATUS_OK); lookup_row(-1); lookup_end();
    restore(); table_is(true, 1); verify_live_indexes(ROWS, 3);
    check_equal(lookup_open("SELECT id FROM renamed", NULL, 0), TURBODB_STATUS_SQL_ERROR);
    check_equal(orm_tidesdb_sql_runtime_close(&lookup_query, &error), TURBODB_STATUS_OK);
  }
  it("renames primary and indexed columns while preserving ordinals and index access") {
    seed(); build_many(); save(); size_t affected = OUTPUT_SENTINEL;
    check_equal(execute_sql("ALTER TABLE items RENAME COLUMN id TO identifier", &affected), TURBODB_STATUS_OK);
    check_equal(execute_sql("ALTER TABLE items RENAME COLUMN a TO amount", &affected), TURBODB_STATUS_OK);
    check_equal(execute_sql("ALTER TABLE items RENAME COLUMN b TO unsigned_value", &affected), TURBODB_STATUS_OK);
    check_equal(lookup_open("SELECT identifier FROM items WHERE amount=0 AND unsigned_value=7", NULL, 0), TURBODB_STATUS_OK);
    check_not_null(lookup_query.as.select.source.lookup.budget); lookup_row(-1); lookup_end();
    const turbodb_value_t unsigned_value = turbodb_u64(7);
    check_equal(execute_params("INSERT INTO items(identifier,amount,unsigned_value) VALUES(10,0,?)", &unsigned_value, 1, &affected), TURBODB_STATUS_CONSTRAINT);
    check_equal(execute_sql("UPDATE items SET identifier=10,amount=9 WHERE identifier=-1", &affected), TURBODB_STATUS_OK);
    check_equal(lookup_open("SELECT identifier FROM items WHERE amount=9 AND unsigned_value=7", NULL, 0), TURBODB_STATUS_OK); lookup_row(10); lookup_end();
    check_equal(execute_sql("DELETE FROM items WHERE identifier=10", &affected), TURBODB_STATUS_OK); check_equal(affected, 1u);
    restore(); verify_live_indexes(ROWS, 3); version_is(owner.transaction, 5);
  }
  it("persists table and column rename across native reopen and supports subsequent index DDL") {
    seed(); build_many(); size_t affected = OUTPUT_SENTINEL;
    check_equal(execute_sql("ALTER TABLE items RENAME COLUMN a TO longer_column", &affected), TURBODB_STATUS_OK);
    check_equal(execute_sql("ALTER TABLE items RENAME TO x", &affected), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    orm_tidesdb_transaction_t *tx = NULL; reopen_raw(&tx); manifest_is(tx, 2, 4, 5); version_is(tx, 7);
    check_equal(orm_tidesdb_txn_rollback(tx), ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx); begin_owner(&owner);
    check_equal(lookup_open("SELECT id FROM x WHERE longer_column=0 AND b=7", NULL, 0), TURBODB_STATUS_OK); lookup_row(-1); lookup_end();
    check_equal(execute_sql("DROP INDEX ix ON x", &affected), TURBODB_STATUS_OK);
    check_equal(execute_sql("CREATE INDEX replacement ON x(longer_column)", &affected), TURBODB_STATUS_OK);
    check_equal(execute_sql("ALTER TABLE x RENAME TO items", &affected), TURBODB_STATUS_OK);
    check_equal(execute_sql("TRUNCATE items", &affected), TURBODB_STATUS_OK); verify_live_indexes(0, 3);
  }
  it("renames empty base tables and permits same-name rename without losing the directory") {
    save(); size_t affected = OUTPUT_SENTINEL;
    check_equal(execute_sql("ALTER TABLE items RENAME TO items", &affected), TURBODB_STATUS_OK);
    table_is(true, 1); version_is(owner.transaction, 2); manifest_is(owner.transaction, 1, 1, 2);
    check_equal(execute_sql("ALTER TABLE items RENAME COLUMN a TO a", &affected), TURBODB_STATUS_OK);
    version_is(owner.transaction, 3);
    check_equal(execute_sql("ALTER TABLE items RENAME TO EMPTY", &affected), TURBODB_STATUS_OK); table_is(false, 0);
    check_equal(execute_sql("ALTER TABLE EMPTY RENAME COLUMN a TO amount", &affected), TURBODB_STATUS_OK);
    const turbodb_value_t unsigned_value = turbodb_u64(3);
    check_equal(execute_params("INSERT INTO EMPTY(id,amount,b) VALUES(1,2,?)", &unsigned_value, 1, &affected), TURBODB_STATUS_OK);
    restore(); table_is(true, 1); version_is(owner.transaction, 1); no_index(1);
  }
  it("rejects unsupported ALTER missing names duplicates parameters and active readers before writes") {
    seed(); build_many(); create_table("CREATE TABLE occupied(id BIGINT PRIMARY KEY)");
    const char *unsupported[] = {"ALTER TABLE items ENGINE=InnoDB", "ALTER TABLE items ALTER COLUMN a SET DEFAULT (UNSUPPORTED_FN(1))",
      "ALTER TABLE db.items RENAME TO renamed", "ALTER TABLE items RENAME TO db.renamed"};
    size_t affected = OUTPUT_SENTINEL;
    for (size_t i=0;i<sizeof(unsupported)/sizeof(unsupported[0]);++i) {
      faults_clear(); check_equal(execute_sql(unsupported[i], &affected), TURBODB_STATUS_UNSUPPORTED);
      check_equal(affected, OUTPUT_SENTINEL); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
    }
    check_equal(execute_sql("ALTER TABLE missing RENAME TO renamed", &affected), TURBODB_STATUS_SQL_ERROR);
    check_equal(execute_sql("ALTER TABLE items RENAME COLUMN missing TO a", &affected), TURBODB_STATUS_SQL_ERROR);
    check_equal(execute_sql("ALTER TABLE items RENAME COLUMN a TO b", &affected), TURBODB_STATUS_SQL_ERROR);
    check_equal(execute_sql("ALTER TABLE items RENAME TO occupied", &affected), TURBODB_STATUS_CONSTRAINT);
    const turbodb_value_t parameter = turbodb_i64(1);
    check_equal(execute_params("ALTER TABLE items RENAME TO renamed", &parameter, 1, &affected), TURBODB_STATUS_SQL_ERROR);
    orm_sql_relation_source source = {0};
    check_equal(orm_tidesdb_sql_relation_open(&owner, vstr_from_cstr(table_name), &source, &error), TURBODB_STATUS_OK);
    check_equal(execute_sql("ALTER TABLE items RENAME TO renamed", &affected), TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_relation_close(&source, &error), TURBODB_STATUS_OK);
    check_false(owner.failed); verify_live_indexes(ROWS, 3);
  }
  it("rolls back every partial rename write while preserving earlier statements") {
    seed(); build_many(); save(); const char *sql[] = {"ALTER TABLE items RENAME TO renamed", "ALTER TABLE items RENAME COLUMN a TO amount"};
    for (size_t op=0;op<2;++op) {
      faults_clear(); check_equal(alter_on(&owner, sql[op]), TURBODB_STATUS_OK);
      const size_t puts = put_calls, deletes = delete_calls; restore();
      const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for (size_t phase=0;phase<2;++phase) for (size_t point=1;point<=(phase?deletes:puts);++point) {
        faults_clear(); if (phase) fail_delete=point; else fail_put=point;
        check_equal(alter_on(&owner, sql[op]), TURBODB_STATUS_DATASTORE_ERROR); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work); faults_clear(); verify_live_indexes(ROWS, 3);
        version_is(owner.transaction, 5); table_is(true, 1);
      }
    }
  }
  it("unwinds every rename workspace reserve and resize failure without native writes") {
    seed(); build_many(); save(); const char *sql[] = {"ALTER TABLE items RENAME TO renamed", "ALTER TABLE items RENAME COLUMN a TO amount"};
    for (size_t op=0;op<2;++op) {
      faults_clear(); check_equal(alter_on(&owner, sql[op]), TURBODB_STATUS_OK);
      const size_t allocated[] = {reserves,resizes}; restore(); const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for (size_t phase=0;phase<2;++phase) for (size_t point=1;point<=allocated[phase];++point) {
        faults_clear(); if (phase) fail_resize=point; else fail_reserve=point;
        check_equal(alter_on(&owner, sql[op]), TURBODB_STATUS_OUT_OF_MEMORY); check_false(owner.failed);
        check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work); check_equal(owner.active_sources, 0u);
      }
      faults_clear(); verify_live_indexes(ROWS, 3);
    }
  }
  it("enforces exact rename quotas before writing any directory record") {
    seed(); build_many(); save(); const char *sql[] = {"ALTER TABLE items RENAME TO renamed", "ALTER TABLE items RENAME COLUMN a TO amount"};
    const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_WORK_BYTES, ORM_SQL_BUDGET_PLAN_NODES,
      ORM_SQL_BUDGET_AST_NODES, ORM_SQL_BUDGET_EXECUTION_STEPS, ORM_SQL_BUDGET_READ_ROWS,
      ORM_SQL_BUDGET_READ_BYTES, ORM_SQL_BUDGET_WRITE_ROWS, ORM_SQL_BUDGET_WRITE_BYTES};
    for (size_t op=0;op<2;++op) {
      check_equal(orm_tidesdb_sql_budget_end(&budget,&error), TURBODB_STATUS_OK); check_equal(orm_tidesdb_sql_budget_begin(&budget,&error), TURBODB_STATUS_OK);
      check_equal(alter_on(&owner,sql[op]), TURBODB_STATUS_OK); const orm_sql_budget_amount peak=budget.peak; restore();
      for (size_t i=0;i<sizeof(resources)/sizeof(resources[0]);++i) {
        const orm_sql_budget_resource resource=resources[i]; const uint64_t original=budget.limits.statement.value[resource];
        check_true(peak.value[resource]>0);
        for (size_t exact=0;exact<2;++exact) {
          check_equal(orm_tidesdb_sql_budget_end(&budget,&error), TURBODB_STATUS_OK); check_equal(orm_tidesdb_sql_budget_begin(&budget,&error), TURBODB_STATUS_OK);
          budget.limits.statement.value[resource]=peak.value[resource]-(exact?0:1); faults_clear();
          const turbodb_status_t status=alter_on(&owner,sql[op]); budget.limits.statement.value[resource]=original;
          check_equal(status,exact?TURBODB_STATUS_OK:TURBODB_STATUS_LIMIT_EXCEEDED);
          if (exact) restore(); else { check_equal(put_calls,0u); check_equal(delete_calls,0u); }
          check_false(owner.failed); verify_live_indexes(ROWS,3);
          check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],budget.retained_work_bytes);
        }
      }
    }
  }
  it("conflicts rename with concurrent DML in both commit orders") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    const char *sql[]={"ALTER TABLE items RENAME TO renamed","ALTER TABLE items RENAME COLUMN a TO amount"};
    const char *undo[]={"ALTER TABLE renamed RENAME TO items","ALTER TABLE items RENAME COLUMN amount TO a"};
    const turbodb_value_t input[]={turbodb_i64(10),turbodb_i64(11),turbodb_u64(12)};
    for(size_t op=0;op<2;++op) for(size_t first=0;first<2;++first) {
      begin_owner(&owner); begin_owner(&other);
      check_equal(alter_on(&owner,sql[op]),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_relation_insert(&other,vstr_from_cstr(table_name),input,COLUMNS,&error),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(first?&owner:&other,true,&error),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(first?&other:&owner,true,&error),TURBODB_STATUS_BUSY);
      begin_owner(&owner); size_t affected=0;
      if(first) check_equal(alter_on(&owner,undo[op]),TURBODB_STATUS_OK);
      else { check_equal(execute_sql("DELETE FROM items WHERE id=10",&affected),TURBODB_STATUS_OK); check_equal(affected,1u); }
      verify_live_indexes(ROWS,3); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    }
    begin_owner(&owner);
  }
  it("rejects rename at exhausted table version before any write") {
    seed(); build_many(); uint8_t key[ORM_SQL_STORE_VERSION_KEY_BYTES], value[ORM_SQL_WIRE_U64];
    orm_sql_store_version_key(1,key); orm_sql_wire_write(value,sizeof(value),UINT64_MAX);
    check_equal(orm_tidesdb_txn_put(owner.transaction,family,key,sizeof(key),value,sizeof(value),0),ORM_TDB_SUCCESS);
    const char *sql[]={"ALTER TABLE items RENAME TO renamed","ALTER TABLE items RENAME COLUMN a TO amount"};
    for(size_t i=0;i<2;++i) {
      faults_clear(); size_t affected=OUTPUT_SENTINEL;
      check_equal(execute_sql(sql[i],&affected),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(affected,OUTPUT_SENTINEL);
      check_equal(put_calls,0u); check_equal(delete_calls,0u); check_false(owner.failed);
    }
  }
  it("fails closed on rename metadata reads and iterator errors without leaking workspace") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK); begin_owner(&owner); save();
    const char *sql[]={"ALTER TABLE items RENAME TO renamed","ALTER TABLE items RENAME COLUMN a TO amount"};
    for(size_t op=0;op<2;++op) {
      faults_clear(); check_equal(alter_on(&owner,sql[op]),TURBODB_STATUS_OK); const size_t reads=gets, iterators=news; restore();
      for(size_t phase=0;phase<3;++phase) {
        const size_t count=phase==0?reads:phase==1?iterators:FAIL_VALUE;
        for(size_t point=1;point<=count;++point) {
          const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; faults_clear();
          if(!phase) fail_get=point; else if(phase==1) fail_new=point; else fail_iterator=(int)point;
          check_equal(alter_on(&owner,sql[op]),TURBODB_STATUS_DATASTORE_ERROR); check_equal(put_calls,0u); check_equal(delete_calls,0u);
          check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
          faults_clear(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK); begin_owner(&owner); save();
          verify_live_indexes(ROWS,3);
        }
      }
    }
  }
  it("requires full rollback after rename savepoint cleanup fails") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK); begin_owner(&owner);
    const char *sql[]={"ALTER TABLE items RENAME TO renamed","ALTER TABLE items RENAME COLUMN a TO amount"};
    for(size_t op=0;op<2;++op) for(size_t phase=0;phase<3;++phase) {
      faults_clear(); if(!phase) fail_savepoint=true; else if(phase==1) fail_release=true; else { fail_put=2; fail_rollback=true; }
      size_t affected=OUTPUT_SENTINEL;
      check_equal(execute_sql(sql[op],&affected),TURBODB_STATUS_DATASTORE_ERROR); check_equal(affected,OUTPUT_SENTINEL);
      check_equal(owner.failed,phase!=0); if(!phase) { check_equal(put_calls,0u); check_equal(delete_calls,0u); }
      faults_clear(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK); begin_owner(&owner);
      verify_live_indexes(ROWS,3); table_is(true,1);
    }
  }
  it("serializes rename with concurrent destination creation in both commit orders") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    const char sql[]="CREATE TABLE renamed(id BIGINT PRIMARY KEY)"; sqlparser_document *doc=NULL; sqlparser_error diagnostic;
    check_equal(sqlparser_parse(sql,strlen(sql),NULL,&doc,&diagnostic),SQLPARSER_OK); enum { DEPTH=32 };
    for(size_t first=0;first<2;++first) {
      begin_owner(&owner); begin_owner(&other); check_equal(alter_on(&owner,"ALTER TABLE items RENAME TO renamed"),TURBODB_STATUS_OK);
      size_t affected=OUTPUT_SENTINEL;
      check_equal(orm_tidesdb_sql_runtime_execute(doc,&other,NULL,0,DEPTH,0,false,NULL,&affected,&error),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(first?&owner:&other,true,&error),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(first?&other:&owner,true,&error),TURBODB_STATUS_BUSY);
      begin_owner(&owner);
      if(first) check_equal(alter_on(&owner,"ALTER TABLE renamed RENAME TO items"),TURBODB_STATUS_OK);
      else check_equal(clear_on(&owner,"DROP TABLE renamed"),TURBODB_STATUS_OK);
      table_is(true,1); verify_live_indexes(ROWS,3);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    }
    sqlparser_document_destroy(doc); begin_owner(&owner);
  }
  it("reopens a complete old or renamed directory after uncertain commit") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    const char *sql[]={"ALTER TABLE items RENAME TO renamed","ALTER TABLE items RENAME COLUMN a TO amount"};
    const char *undo[]={"ALTER TABLE renamed RENAME TO items","ALTER TABLE items RENAME COLUMN amount TO a"};
    const char *select[]={"SELECT id FROM renamed WHERE a=0 AND b=7","SELECT id FROM items WHERE amount=0 AND b=7"};
    for(size_t op=0;op<2;++op) for(int fault=COMMIT_BEFORE;fault<=COMMIT_AFTER;++fault) {
      begin_owner(&owner); check_equal(alter_on(&owner,sql[op]),TURBODB_STATUS_OK); commit_fault=fault;
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_COMMIT_UNKNOWN); faults_clear();
      orm_tidesdb_transaction_t *tx=NULL; reopen_raw(&tx); manifest_is(tx,2,4,5);
      check_equal(orm_tidesdb_txn_rollback(tx),ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx); begin_owner(&owner);
      if(fault==COMMIT_AFTER) {
        check_equal(lookup_open(select[op],NULL,0),TURBODB_STATUS_OK); check_not_null(lookup_query.as.select.source.lookup.budget);
        lookup_row(-1); lookup_end(); check_equal(alter_on(&owner,undo[op]),TURBODB_STATUS_OK);
      }
      verify_live_indexes(ROWS,3); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    }
    begin_owner(&owner);
  }
  it("rejects malformed index directories before rename and requires transaction rollback") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    const char *sql[]={"ALTER TABLE items RENAME TO renamed","ALTER TABLE items RENAME COLUMN a TO amount"};
    const uint8_t key[]={INDEX_DIRECTORY_NS,1,0,0,0,0,0,0,0,2,'i','x'}, invalid[]={0};
    for(size_t op=0;op<2;++op) {
      begin_owner(&owner);
      check_equal(orm_tidesdb_txn_put(owner.transaction,family,key,sizeof(key),invalid,sizeof(invalid),0),ORM_TDB_SUCCESS);
      faults_clear(); check_equal(alter_on(&owner,sql[op]),TURBODB_STATUS_DATASTORE_ERROR);
      check_true(owner.failed); check_equal(put_calls,0u); check_equal(delete_calls,0u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK);
    }
    begin_owner(&owner); verify_live_indexes(ROWS,3);
  }
  it("bounds renamed schema records and accepts maximum length quoted identifiers") {
    seed(); save(); enum { SQL_BYTES=256 }; char name[ORM_SQL_SELECT_NAME_BYTES+1], sql[SQL_BYTES];
    memset(name,'z',ORM_SQL_SELECT_NAME_BYTES); name[ORM_SQL_SELECT_NAME_BYTES]=0;
    uint8_t key[ORM_SQL_STORE_NAME_KEY_BYTES]; store_buffer record={0};
    check_equal(orm_sql_store_name_key(vstr_from_cstr(table_name),key,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_store_get(&owner,key,2+strlen(table_name),MAX_RECORD,&record,&error),TURBODB_STATUS_OK);
    const size_t old_size=record.size; check_equal(orm_sql_store_buffer_close(&owner,&record,&error),TURBODB_STATUS_OK);
    for(size_t column=0;column<2;++column) {
      (void)snprintf(sql,sizeof(sql),column?"ALTER TABLE items RENAME COLUMN a TO `%s`":"ALTER TABLE items RENAME TO `%s`",name);
      owner.max_record_bytes=old_size; faults_clear();
      check_equal(alter_on(&owner,sql),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(put_calls,0u); check_equal(delete_calls,0u);
      owner.max_record_bytes=MAX_RECORD; check_false(owner.failed); check_equal(alter_on(&owner,sql),TURBODB_STATUS_OK); restore();
    }
    table_is(true,1); no_index(2);
  }
  group("ALTER defaults in the native indexed store") {
    it("converts default text without rewriting rows and refuses invalid conversions before native writes") {
      seed();build_many();
      next_statement();faults_clear();size_t affected=OUTPUT_SENTINEL;
      check_equal(execute_sql("ALTER TABLE items ALTER a SET DEFAULT '0.5e1'",&affected),TURBODB_STATUS_OK);
      check_equal(affected,0u);check_equal(put_calls,3u);check_equal(delete_calls,0u);
      column_default_is(1,true,5);verify_seed_rows();verify_live_indexes(ROWS,3);
      const char *sql[]={"ALTER TABLE items ALTER a SET DEFAULT '2.5junk'",
        "ALTER TABLE items ALTER a SET DEFAULT '9223372036854775807.5'",
        "ALTER TABLE items ADD extra BIGINT UNSIGNED NOT NULL DEFAULT '-0.5'"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        next_statement();faults_clear();affected=OUTPUT_SENTINEL;
        const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
        check_equal(execute_sql(sql[i],&affected),i?TURBODB_STATUS_OUT_OF_RANGE:TURBODB_STATUS_TYPE_ERROR);
        check_equal(affected,OUTPUT_SENTINEL);check_equal(put_calls,0u);check_equal(delete_calls,0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);check_false(owner.failed);
        column_default_is(1,true,5);verify_seed_rows();verify_live_indexes(ROWS,3);
      }
    }
    it("changes only metadata and leaves every existing row and index key intact") {
      seed(); build_many(); save();
      const char *sql[] = {"ALTER TABLE items ALTER a SET DEFAULT (2+3)",
        "ALTER TABLE items ALTER COLUMN a DROP DEFAULT"};
      for (size_t op = 0; op < sizeof(sql)/sizeof(sql[0]); ++op) {
        next_statement(); faults_clear(); size_t affected = OUTPUT_SENTINEL;
        check_equal(execute_sql(sql[op], &affected), TURBODB_STATUS_OK); check_equal(affected, 0u);
        check_equal(put_calls, 3u); check_equal(delete_calls, 0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
        column_default_is(1, op == 0, 5); verify_seed_rows();
        verify_live_indexes(ROWS, 3); check_false(owner.failed);
      }
      restore(); column_default_is(1, false, 0); verify_live_indexes(ROWS, 3);
    }
    it("unwinds each default allocation and partial metadata write while preserving prior DML") {
      seed(); build_many(); check_equal(alter_on(&owner, "ALTER TABLE items ALTER a SET DEFAULT 7"), TURBODB_STATUS_OK); save();
      const char *sql[] = {"ALTER TABLE items ALTER a SET DEFAULT (2+3)", "ALTER TABLE items ALTER a DROP DEFAULT",
        "ALTER TABLE items ALTER a SET DEFAULT '0.5e1'"};
      for (size_t op = 0; op < sizeof(sql)/sizeof(sql[0]); ++op) {
        next_statement(); faults_clear(); check_equal(alter_on(&owner, sql[op]), TURBODB_STATUS_OK);
        const size_t counts[] = {reserves, resizes, put_calls}; restore();
        const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; check_equal(counts[2], 3u);
        for (size_t phase = 0; phase < sizeof(counts)/sizeof(counts[0]); ++phase)
          for (size_t point = 1; point <= counts[phase]; ++point) {
            next_statement(); faults_clear();
            if (!phase) fail_reserve = point; else if (phase == 1) fail_resize = point; else fail_put = point;
            check_equal(alter_on(&owner, sql[op]), phase == 2 ? TURBODB_STATUS_DATASTORE_ERROR : TURBODB_STATUS_OUT_OF_MEMORY);
            check_false(owner.failed); check_equal(owner.active_sources, 0u);
            check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
            check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
            if (phase != 2) check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
            faults_clear(); column_default_is(1, true, 7); verify_live_indexes(ROWS, 3);
          }
      }
    }
    it("requires rollback when default batch cleanup fails") {
      seed(); build_many(); check_equal(alter_on(&owner, "ALTER TABLE items ALTER a SET DEFAULT 7"), TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
      const char *sql[] = {"ALTER TABLE items ALTER a SET DEFAULT 9", "ALTER TABLE items ALTER a DROP DEFAULT"};
      for (size_t op = 0; op < sizeof(sql)/sizeof(sql[0]); ++op) for (size_t phase = 0; phase < 3; ++phase) {
        begin_owner(&owner); faults_clear();
        if (!phase) fail_savepoint = true; else if (phase == 1) fail_release = true; else { fail_put = 2; fail_rollback = true; }
        size_t affected = OUTPUT_SENTINEL;
        check_equal(execute_sql(sql[op], &affected), TURBODB_STATUS_DATASTORE_ERROR); check_equal(affected, OUTPUT_SENTINEL);
        check_equal(owner.failed, phase != 0); faults_clear();
        check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
        begin_owner(&owner); column_default_is(1, true, 7); verify_live_indexes(ROWS, 3);
        check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
      }
      begin_owner(&owner);
    }
    it("recovers exactly the committed default after either uncertain commit outcome") {
      seed(); build_many(); check_equal(alter_on(&owner, "ALTER TABLE items ALTER a SET DEFAULT 7"), TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
      const char *sql[] = {"ALTER TABLE items ALTER a SET DEFAULT 9", "ALTER TABLE items ALTER a DROP DEFAULT"};
      for (size_t op = 0; op < sizeof(sql)/sizeof(sql[0]); ++op) for (int fault = COMMIT_BEFORE; fault <= COMMIT_AFTER; ++fault) {
        begin_owner(&owner); check_equal(alter_on(&owner, sql[op]), TURBODB_STATUS_OK); commit_fault = fault;
        check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_COMMIT_UNKNOWN); faults_clear();
        orm_tidesdb_transaction_t *tx = NULL; reopen_raw(&tx); manifest_is(tx, 2, 4, 5);
        check_equal(orm_tidesdb_txn_rollback(tx), ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx); begin_owner(&owner);
        column_default_is(1, fault == COMMIT_BEFORE || !op, fault == COMMIT_BEFORE ? 7 : 9);
        verify_seed_rows(); verify_live_indexes(ROWS, 3);
        check_equal(alter_on(&owner, "ALTER TABLE items ALTER a SET DEFAULT 7"), TURBODB_STATUS_OK);
        check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
      }
      begin_owner(&owner);
    }
    it("rejects every native metadata read failure before publishing a changed default") {
      seed(); build_many(); check_equal(alter_on(&owner,"ALTER TABLE items ALTER a SET DEFAULT 7"),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK); begin_owner(&owner); save();
      const char *sql[]={"ALTER TABLE items ALTER a SET DEFAULT (2+3)","ALTER TABLE items ALTER a DROP DEFAULT",
        "ALTER TABLE items ALTER a SET DEFAULT '0.5e1'"};
      for(size_t op=0;op<sizeof(sql)/sizeof(sql[0]);++op) {
        next_statement(); faults_clear(); check_equal(alter_on(&owner,sql[op]),TURBODB_STATUS_OK);
        const size_t counts[]={gets,news,FAIL_VALUE}; restore();
        for(size_t phase=0;phase<sizeof(counts)/sizeof(counts[0]);++phase) for(size_t point=1;point<=counts[phase];++point) {
          next_statement(); faults_clear(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
          if(!phase) fail_get=point; else if(phase==1) fail_new=point; else fail_iterator=(int)point;
          check_equal(alter_on(&owner,sql[op]),TURBODB_STATUS_DATASTORE_ERROR); check_equal(put_calls,0u); check_equal(delete_calls,0u);
          check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
          faults_clear(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK); begin_owner(&owner); save();
          column_default_is(1,true,7); verify_live_indexes(ROWS,3);
        }
      }
    }
    it("admits exact default metadata budgets and rejects one unit below each quota before writes") {
      seed(); build_many(); check_equal(alter_on(&owner,"ALTER TABLE items ALTER a SET DEFAULT 7"),TURBODB_STATUS_OK); save();
      const char *sql[]={"ALTER TABLE items ALTER a SET DEFAULT (2+3)","ALTER TABLE items ALTER a DROP DEFAULT",
        "ALTER TABLE items ALTER a SET DEFAULT '0.5e1'"};
      const orm_sql_budget_resource resources[]={ORM_SQL_BUDGET_WORK_BYTES,ORM_SQL_BUDGET_PLAN_NODES,ORM_SQL_BUDGET_AST_NODES,
        ORM_SQL_BUDGET_EXECUTION_STEPS,ORM_SQL_BUDGET_READ_ROWS,ORM_SQL_BUDGET_READ_BYTES,ORM_SQL_BUDGET_WRITE_ROWS,ORM_SQL_BUDGET_WRITE_BYTES};
      for(size_t op=0;op<sizeof(sql)/sizeof(sql[0]);++op) {
        next_statement(); check_equal(alter_on(&owner,sql[op]),TURBODB_STATUS_OK); const orm_sql_budget_amount peak=budget.peak; restore();
        for(size_t i=0;i<sizeof(resources)/sizeof(resources[0]);++i) {
          const orm_sql_budget_resource resource=resources[i]; const uint64_t original=budget.limits.statement.value[resource];
          check_greater(peak.value[resource],0u);
          for(size_t exact=0;exact<2;++exact) {
            next_statement(); budget.limits.statement.value[resource]=peak.value[resource]-(exact?0:1); faults_clear();
            const turbodb_status_t status=alter_on(&owner,sql[op]); budget.limits.statement.value[resource]=original;
            check_equal(status,exact?TURBODB_STATUS_OK:TURBODB_STATUS_LIMIT_EXCEEDED);
            if(exact) restore(); else { check_equal(put_calls,0u); check_equal(delete_calls,0u); }
            check_false(owner.failed); column_default_is(1,true,7); verify_live_indexes(ROWS,3);
            check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],budget.retained_work_bytes);
            check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
          }
        }
      }
    }
    it("serializes default metadata with old-schema inserts in both commit orders") {
      seed(); build_many(); check_equal(alter_on(&owner,"ALTER TABLE items ALTER a SET DEFAULT 7"),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
      const char *sql[]={"ALTER TABLE items ALTER a SET DEFAULT 9","ALTER TABLE items ALTER a DROP DEFAULT"};
      const turbodb_value_t input[]={turbodb_i64(10),turbodb_i64(11),turbodb_u64(12)};
      for(size_t op=0;op<sizeof(sql)/sizeof(sql[0]);++op) for(size_t first=0;first<2;++first) {
        begin_owner(&owner); begin_owner(&other); check_equal(alter_on(&owner,sql[op]),TURBODB_STATUS_OK);
        check_equal(orm_tidesdb_sql_relation_insert(&other,vstr_from_cstr(table_name),input,COLUMNS,&error),TURBODB_STATUS_OK);
        check_equal(orm_tidesdb_sql_catalog_finish(first?&owner:&other,true,&error),TURBODB_STATUS_OK);
        check_equal(orm_tidesdb_sql_catalog_finish(first?&other:&owner,true,&error),TURBODB_STATUS_BUSY);
        begin_owner(&owner); column_default_is(1,!first||!op,first?9:7); verify_live_indexes(first?ROWS:ROWS+1,3);
        if(first) check_equal(alter_on(&owner,"ALTER TABLE items ALTER a SET DEFAULT 7"),TURBODB_STATUS_OK);
        else { size_t affected=0; check_equal(execute_sql("DELETE FROM items WHERE id=10",&affected),TURBODB_STATUS_OK); check_equal(affected,1u); }
        check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
      }
      begin_owner(&owner); column_default_is(1,true,7); verify_live_indexes(ROWS,3);
    }
    it("refuses default changes while a query source owns the transaction snapshot") {
      seed(); build_many(); check_equal(lookup_open("SELECT id FROM items WHERE a=0 AND b=7", NULL, 0), TURBODB_STATUS_OK);
      faults_clear(); check_equal(alter_on(&owner, "ALTER TABLE items ALTER a SET DEFAULT 5"), TURBODB_STATUS_BUSY);
      check_equal(alter_on(&owner, "ALTER TABLE items ALTER a DROP DEFAULT"), TURBODB_STATUS_BUSY); check_equal(put_calls, 0u);
      lookup_row(-1); lookup_end(); column_default_is(1, false, 0);
      check_equal(alter_on(&owner, "ALTER TABLE items ALTER a SET DEFAULT 5"), TURBODB_STATUS_OK); column_default_is(1, true, 5);
    }
  }
  it("adds and removes columns on base-format tables without upgrading the Manifest") {
    save(); size_t affected=OUTPUT_SENTINEL;
    check_equal(execute_sql("ALTER TABLE items DROP a",&affected),TURBODB_STATUS_OK);
    check_equal(execute_sql("ALTER TABLE items ADD c DOUBLE",&affected),TURBODB_STATUS_OK);
    manifest_is(owner.transaction,1,1,2); version_is(owner.transaction,3);
    const turbodb_value_t input[]={turbodb_i64(7),turbodb_u64(UINT64_MAX),turbodb_f64(2.5)};
    check_equal(execute_params("INSERT INTO items(id,b,c) VALUES(?,?,?)",input,3,&affected),TURBODB_STATUS_OK);
    check_equal(lookup_open("SELECT id FROM items WHERE b=18446744073709551615",NULL,0),TURBODB_STATUS_OK); lookup_row(7); lookup_end();
    check_equal(execute_sql("TRUNCATE items",&affected),TURBODB_STATUS_OK);
    check_equal(execute_sql("ALTER TABLE items DROP b",&affected),TURBODB_STATUS_OK);
    manifest_is(owner.transaction,1,1,2); restore(); no_index(1); table_is(true,1);
  }
  it("appends numeric columns to empty tables and preserves nullability through insertion and rollback") {
    empty_column_indexes(); save(); size_t affected=OUTPUT_SENTINEL;
    check_equal(execute_sql("ALTER TABLE items ADD COLUMN c DOUBLE",&affected),TURBODB_STATUS_OK); check_equal(affected,0u);
    check_equal(execute_sql("ALTER TABLE items ADD d BIGINT UNSIGNED NOT NULL",&affected),TURBODB_STATUS_OK);
    const turbodb_value_t args[]={turbodb_i64(1),turbodb_i64(2),turbodb_u64(3),turbodb_f64(4.5),turbodb_u64(5)};
    check_equal(execute_params("INSERT INTO items(id,a,b,c,d) VALUES(?,?,?,?,?)",args,5,&affected),TURBODB_STATUS_OK);
    const turbodb_value_t floating=turbodb_f64(4.5);
    check_equal(lookup_open("SELECT id FROM items WHERE b=3 AND c=? AND d=5",&floating,1),TURBODB_STATUS_OK); lookup_row(1); lookup_end();
    check_equal(execute_sql("INSERT INTO items(id,a,b,c,d) VALUES(2,NULL,NULL,NULL,NULL)",&affected),TURBODB_STATUS_CONSTRAINT);
    restore(); version_is(owner.transaction,3); verify_live_indexes(0,2); table_is(true,1);
    check_equal(execute_sql("ALTER TABLE items ADD COLUMN c BIGINT NULL",&affected),TURBODB_STATUS_OK);
    check_equal(execute_sql("INSERT INTO items(id,a,b,c) VALUES(2,NULL,NULL,NULL)",&affected),TURBODB_STATUS_OK);
    check_equal(lookup_open("SELECT id FROM items WHERE c IS NULL",NULL,0),TURBODB_STATUS_OK); lookup_row(2); lookup_end();
  }
  it("removes a middle column and remaps surviving index ordinals atomically") {
    empty_column_indexes(); save(); size_t affected=OUTPUT_SENTINEL;
    check_equal(execute_sql("ALTER TABLE items DROP COLUMN a",&affected),TURBODB_STATUS_OK); check_equal(affected,0u);
    const turbodb_value_t args[]={turbodb_i64(1),turbodb_u64(7)};
    check_equal(execute_params("INSERT INTO items(id,b) VALUES(?,?)",args,2,&affected),TURBODB_STATUS_OK);
    check_equal(lookup_open("SELECT id FROM items WHERE b=7",NULL,0),TURBODB_STATUS_OK);
    check_not_null(lookup_query.as.select.source.lookup.budget); lookup_row(1); lookup_end();
    const turbodb_value_t duplicate[]={turbodb_i64(2),turbodb_u64(7)};
    check_equal(execute_params("INSERT INTO items(id,b) VALUES(?,?)",duplicate,2,&affected),TURBODB_STATUS_CONSTRAINT);
    verify_live_indexes(1,2); restore(); verify_live_indexes(0,2); version_is(owner.transaction,3);
  }
  it("remaps a non-leading primary key and persists altered schemas across reopen") {
    create_table("CREATE TABLE later(extra DOUBLE,id BIGINT UNSIGNED PRIMARY KEY,score BIGINT)"); size_t affected=0;
    check_equal(execute_sql("CREATE UNIQUE INDEX score_key ON later(score)",&affected),TURBODB_STATUS_OK);
    check_equal(execute_sql("ALTER TABLE later DROP extra",&affected),TURBODB_STATUS_OK);
    check_equal(execute_sql("ALTER TABLE later ADD extra DOUBLE NOT NULL",&affected),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    orm_tidesdb_transaction_t *tx=NULL; reopen_raw(&tx); check_equal(orm_tidesdb_txn_rollback(tx),ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx); begin_owner(&owner);
    const turbodb_value_t args[]={turbodb_u64(UINT64_MAX),turbodb_i64(11),turbodb_f64(2.5)};
    check_equal(execute_params("INSERT INTO later(id,score,extra) VALUES(?,?,?)",args,3,&affected),TURBODB_STATUS_OK);
    check_equal(lookup_open("SELECT score FROM later WHERE id=18446744073709551615",NULL,0),TURBODB_STATUS_OK); lookup_row(11); lookup_end();
    check_equal(execute_sql("DELETE FROM later WHERE score=11",&affected),TURBODB_STATUS_OK); check_equal(affected,1u);
    check_equal(execute_sql("ALTER TABLE later RENAME COLUMN score TO renamed",&affected),TURBODB_STATUS_OK);
  }
  it("rejects nonempty NOT NULL additions and unsupported column dependencies without modifying storage") {
    empty_column_indexes(); const char *invalid[]={"ALTER TABLE items DROP COLUMN id", "ALTER TABLE items DROP COLUMN b",
      "ALTER TABLE items ADD c TEXT", "ALTER TABLE items ADD c BIGINT DEFAULT (UNSUPPORTED_FN(1))"}; size_t affected=OUTPUT_SENTINEL;
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
      faults_clear(); check_equal(execute_sql(invalid[i],&affected),TURBODB_STATUS_UNSUPPORTED);
      check_equal(affected,OUTPUT_SENTINEL); check_equal(put_calls,0u); check_equal(delete_calls,0u);
    }
    check_equal(execute_sql("ALTER TABLE items ADD a BIGINT",&affected),TURBODB_STATUS_SQL_ERROR);
    check_equal(execute_sql("ALTER TABLE items DROP missing",&affected),TURBODB_STATUS_SQL_ERROR);
    check_equal(execute_sql("ALTER TABLE items ADD c BIGINT PRIMARY KEY",&affected),TURBODB_STATUS_SQL_ERROR);
    check_equal(execute_sql("INSERT INTO items(id,a,b) VALUES(1,NULL,NULL)",&affected),TURBODB_STATUS_OK);
    const char *sql[]={"ALTER TABLE items ADD c BIGINT NOT NULL","ALTER TABLE items ADD c DOUBLE NOT NULL"};
    for(size_t i=0;i<2;++i) {
      faults_clear(); check_equal(execute_sql(sql[i],&affected),TURBODB_STATUS_UNSUPPORTED);
      check_equal(put_calls,0u); check_equal(delete_calls,0u); check_false(owner.failed);
    }
    verify_live_indexes(1,2);
  }
  it("rewrites populated base rows with NULL additions and preserves numeric extrema on removal") {
    seed(); save(); size_t affected=OUTPUT_SENTINEL;
    check_equal(execute_sql("ALTER TABLE items ADD extra DOUBLE",&affected),TURBODB_STATUS_OK); check_equal(affected,0u);
    check_equal(lookup_open("SELECT id FROM items WHERE extra IS NULL ORDER BY id",NULL,0),TURBODB_STATUS_OK);
    lookup_row(INT64_MIN); lookup_row(-1); lookup_row(0); lookup_row(1); lookup_row(INT64_MAX); lookup_end();
    const turbodb_value_t floating=turbodb_f64(2.5);
    check_equal(execute_params("UPDATE items SET extra=? WHERE id=-1",&floating,1,&affected),TURBODB_STATUS_OK);
    check_equal(alter_on(&owner,"ALTER TABLE items ADD nullable_unsigned BIGINT UNSIGNED"),TURBODB_STATUS_OK);
    check_equal(alter_on(&owner,"ALTER TABLE items DROP a"),TURBODB_STATUS_OK);
    check_equal(lookup_open("SELECT id FROM items WHERE extra=? AND nullable_unsigned IS NULL",&floating,1),TURBODB_STATUS_OK);
    lookup_row(-1); lookup_end();
    check_equal(alter_on(&owner,"ALTER TABLE items DROP extra"),TURBODB_STATUS_OK);
    check_equal(lookup_open("SELECT id FROM items WHERE b=18446744073709551615",NULL,0),TURBODB_STATUS_OK);
    lookup_row(INT64_MIN); lookup_end(); manifest_is(owner.transaction,1,1,2);
    restore(); check_equal(lookup_open("SELECT id FROM items ORDER BY id",NULL,0),TURBODB_STATUS_OK);
    lookup_row(INT64_MIN); lookup_row(-1); lookup_row(0); lookup_row(1); lookup_row(INT64_MAX); lookup_end();
  }
  it("inserts columns at every ordinal and preserves decoded names keys and index tuples") {
    seed(); build_many(); save();
    const char *sql[]={"ALTER TABLE items ADD extra DOUBLE FIRST", "ALTER TABLE items ADD extra DOUBLE AFTER id",
      "ALTER TABLE items ADD extra DOUBLE AFTER a", "ALTER TABLE items ADD extra DOUBLE AFTER b"};
    const char *names[]={"id","a","b"};
    for(size_t position=0;position<=COLUMNS;++position) {
      check_equal(alter_on(&owner,sql[position]),TURBODB_STATUS_OK);
      orm_sql_table_definition table={0}; orm_sql_table_schema schema={0}; uint64_t id=0,version=0; bool found=false;
      check_equal(orm_tidesdb_sql_catalog_lookup(&owner,vstr_from_cstr(table_name),&table,&id,&version,&found,&error),TURBODB_STATUS_OK);
      check_true(found); check_equal(id,1u); check_equal(table.primary_key,position==0?1u:0u);
      check_equal(orm_tidesdb_sql_catalog_schema(&table,&schema,&error),TURBODB_STATUS_OK); check_equal(schema.count,COLUMNS+1);
      for(size_t c=0;c<schema.count;++c) {
        const char *name=c==position?"extra":names[c-(c>position)];
        check_equal(schema.columns[c].name.len,strlen(name)); check_equal(memcmp(schema.columns[c].name.data,name,strlen(name)),0);
      }
      check_equal(orm_tidesdb_sql_catalog_destroy(&table,&error),TURBODB_STATUS_OK); verify_live_indexes(ROWS,3);
      check_equal(lookup_open("SELECT id FROM items WHERE a=0 AND b=7",NULL,0),TURBODB_STATUS_OK);
      check_not_null(lookup_query.as.select.source.lookup.budget); lookup_row(-1); lookup_end();
      check_equal(lookup_open("SELECT id FROM items WHERE id=-1 AND extra IS NULL",NULL,0),TURBODB_STATUS_OK); lookup_row(-1); lookup_end();
      size_t affected=0; const turbodb_value_t floating=turbodb_f64(2.5);
      check_equal(execute_params("UPDATE items SET extra=? WHERE id=-1",&floating,1,&affected),TURBODB_STATUS_OK);
      check_equal(alter_on(&owner,"ALTER TABLE items RENAME COLUMN extra TO amount"),TURBODB_STATUS_OK);
      check_equal(lookup_open("SELECT id FROM items WHERE amount=?",&floating,1),TURBODB_STATUS_OK); lookup_row(-1); lookup_end();
      check_equal(alter_on(&owner,"ALTER TABLE items DROP amount"),TURBODB_STATUS_OK); verify_live_indexes(ROWS,3); restore();
    }
  }
  it("rejects invalid placement targets duplicate names and positioned NOT NULL additions before writing") {
    seed(); build_many();
    const char *sql[]={"ALTER TABLE items ADD extra DOUBLE AFTER absent", "ALTER TABLE items ADD extra DOUBLE AFTER extra",
      "ALTER TABLE items ADD a BIGINT FIRST", "ALTER TABLE items ADD b BIGINT AFTER id",
      "ALTER TABLE items ADD extra BIGINT NOT NULL FIRST", "ALTER TABLE items ADD extra DOUBLE NOT NULL AFTER id"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      faults_clear(); check_equal(alter_on(&owner,sql[i]),i<4?TURBODB_STATUS_SQL_ERROR:TURBODB_STATUS_UNSUPPORTED);
      check_equal(put_calls,0u); check_equal(delete_calls,0u); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],budget.retained_work_bytes); verify_live_indexes(ROWS,3);
    }
  }
  it("rewrites a populated middle column before an unsigned primary key without changing index identities") {
    create_table("CREATE TABLE later(extra DOUBLE,id BIGINT UNSIGNED PRIMARY KEY,score BIGINT)"); size_t affected=0;
    const turbodb_value_t args[]={turbodb_f64(2.5),turbodb_u64(UINT64_MAX),turbodb_i64(11)};
    check_equal(execute_params("INSERT INTO later(extra,id,score) VALUES(?,?,?)",args,3,&affected),TURBODB_STATUS_OK);
    check_equal(execute_sql("CREATE UNIQUE INDEX score_key ON later(score)",&affected),TURBODB_STATUS_OK);
    check_equal(alter_on(&owner,"ALTER TABLE later DROP extra"),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    orm_tidesdb_transaction_t *tx=NULL; reopen_raw(&tx);
    check_equal(orm_tidesdb_txn_rollback(tx),ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx); begin_owner(&owner);
    check_equal(lookup_open("SELECT score FROM later WHERE id=18446744073709551615",NULL,0),TURBODB_STATUS_OK); lookup_row(11); lookup_end();
    check_equal(lookup_open("SELECT score FROM later WHERE score=11",NULL,0),TURBODB_STATUS_OK);
    check_not_null(lookup_query.as.select.source.lookup.budget); lookup_row(11); lookup_end();
    const turbodb_value_t duplicate[]={turbodb_u64(1),turbodb_i64(11)};
    check_equal(execute_params("INSERT INTO later(id,score) VALUES(?,?)",duplicate,2,&affected),TURBODB_STATUS_CONSTRAINT);
    check_equal(execute_sql("UPDATE later SET score=12 WHERE score=11",&affected),TURBODB_STATUS_OK); check_equal(affected,1u);
    check_equal(execute_sql("DELETE FROM later WHERE score=12",&affected),TURBODB_STATUS_OK); check_equal(affected,1u);
  }
  it("unwinds every populated ALTER allocation and partial row write while retaining earlier DML") {
    /* This sweep deliberately repeats full index verification after every
     * injected fault; its transaction quota must not terminate the sweep. */
    budget.limits.transaction.read_bytes = FAULT_SWEEP_READ_LIMIT;
    seed(); build_many(); check_equal(alter_on(&owner,"ALTER TABLE items ADD spare BIGINT"),TURBODB_STATUS_OK);
    size_t affected=0; check_equal(execute_sql("UPDATE items SET spare=11",&affected),TURBODB_STATUS_OK); save();
    for(size_t op=0;op<COLUMN_REWRITE_CASES;++op) {
      next_statement();
      faults_clear(); check_equal(alter_on(&owner,column_rewrite_sql[op]),TURBODB_STATUS_OK); const size_t counts[]={reserves,resizes,put_calls}; restore();
      check_true(counts[2]>ROWS);
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(size_t phase=0;phase<3;++phase) for(size_t point=1;point<=counts[phase];++point) {
        faults_clear(); next_statement(); if(!phase) fail_reserve=point; else if(phase==1) fail_resize=point; else fail_put=point;
        check_equal(alter_on(&owner,column_rewrite_sql[op]),phase==2?TURBODB_STATUS_DATASTORE_ERROR:TURBODB_STATUS_OUT_OF_MEMORY);
        check_false(owner.failed); check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
        if(phase!=2) check_equal(put_calls,0u); check_equal(delete_calls,0u);
        faults_clear(); verify_live_indexes(ROWS,3);
        check_equal(lookup_open("SELECT spare FROM items WHERE a=0 AND b=7",NULL,0),TURBODB_STATUS_OK); lookup_row(11); lookup_end();
      }
      faults_clear(); fail_sort=true; check_equal(alter_on(&owner,column_rewrite_sql[op]),TURBODB_STATUS_OUT_OF_MEMORY);
      check_equal(put_calls,0u); check_false(owner.failed); faults_clear(); verify_live_indexes(ROWS,3);
    }
  }
  it("enforces exact populated ALTER resource quotas before any row rewrite") {
    seed(); build_many(); check_equal(alter_on(&owner,"ALTER TABLE items ADD spare BIGINT"),TURBODB_STATUS_OK); save();
    const orm_sql_budget_resource resources[]={ORM_SQL_BUDGET_WORK_BYTES,ORM_SQL_BUDGET_PLAN_NODES,ORM_SQL_BUDGET_AST_NODES,
      ORM_SQL_BUDGET_MATERIALIZED_ROWS,ORM_SQL_BUDGET_EXECUTION_STEPS,ORM_SQL_BUDGET_READ_ROWS,ORM_SQL_BUDGET_READ_BYTES,
      ORM_SQL_BUDGET_WRITE_ROWS,ORM_SQL_BUDGET_WRITE_BYTES};
    for(size_t op=0;op<COLUMN_REWRITE_CASES;++op) {
      check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
      check_equal(alter_on(&owner,column_rewrite_sql[op]),TURBODB_STATUS_OK); const orm_sql_budget_amount peak=budget.peak; restore();
      for(size_t i=0;i<sizeof(resources)/sizeof(resources[0]);++i) {
        const orm_sql_budget_resource resource=resources[i]; const uint64_t original=budget.limits.statement.value[resource];
        check_true(peak.value[resource]>0);
        for(size_t exact=0;exact<2;++exact) {
          check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
          budget.limits.statement.value[resource]=peak.value[resource]-(exact?0:1); faults_clear();
          const turbodb_status_t status=alter_on(&owner,column_rewrite_sql[op]); budget.limits.statement.value[resource]=original;
          check_equal(status,exact?TURBODB_STATUS_OK:TURBODB_STATUS_LIMIT_EXCEEDED);
          if(exact) restore(); else { check_equal(put_calls,0u); check_equal(delete_calls,0u); }
          check_false(owner.failed); verify_live_indexes(ROWS,3);
          check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],budget.retained_work_bytes);
          check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
        }
      }
    }
  }
  it("fails closed on every populated ALTER read and iterator failure") {
    seed(); build_many(); check_equal(alter_on(&owner,"ALTER TABLE items ADD spare BIGINT"),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK); begin_owner(&owner); save();
    for(size_t op=0;op<COLUMN_REWRITE_CASES;++op) {
      next_statement();
      faults_clear(); check_equal(alter_on(&owner,column_rewrite_sql[op]),TURBODB_STATUS_OK); const size_t counts[]={gets,news,FAIL_VALUE}; restore();
      for(size_t phase=0;phase<3;++phase) for(size_t point=1;point<=counts[phase];++point) {
        const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; faults_clear(); next_statement();
        if(!phase) fail_get=point; else if(phase==1) fail_new=point; else fail_iterator=(int)point;
        check_equal(alter_on(&owner,column_rewrite_sql[op]),TURBODB_STATUS_DATASTORE_ERROR); check_equal(put_calls,0u); check_equal(delete_calls,0u);
        check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
        faults_clear(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK); begin_owner(&owner); save();
        verify_live_indexes(ROWS,3);
      }
    }
  }
  it("rejects missing occupancy corrupt rows and orphan generations before populated rewrites") {
    seed(); build_many(); check_equal(alter_on(&owner,"ALTER TABLE items ADD spare BIGINT"),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    const uint8_t spaces[]={INDEX_DATA_NS,INDEX_UNIQUE_NS,REL_KIND,INDEX_DATA_NS,REL_KIND};
    for(size_t op=0;op<COLUMN_REWRITE_CASES;++op) for(size_t bad=0;bad<sizeof(spaces)/sizeof(spaces[0]);++bad) {
      begin_owner(&owner); uint8_t prefix[1+ORM_SQL_WIRE_U64]={0}, key[DATA_KEY_BYTES];
      prefix[0]=spaces[bad]; orm_sql_wire_write(prefix+1,ORM_SQL_WIRE_U64,spaces[bad]==REL_KIND?1:2);
      orm_tidesdb_iterator_t *iterator=NULL; uint8_t *view=NULL; size_t size=0;
      check_equal(orm_tidesdb_iter_new(owner.transaction,family,&iterator),ORM_TDB_SUCCESS);
      check_equal(orm_tidesdb_iter_seek(iterator,prefix,sizeof(prefix)),ORM_TDB_SUCCESS);
      check_equal(orm_tidesdb_iter_key(iterator,&view,&size),ORM_TDB_SUCCESS);
      check_true(size<=sizeof(key)); memcpy(key,view,size); orm_tidesdb_iter_free(iterator);
      if(bad<2) check_equal(orm_tidesdb_txn_delete(owner.transaction,family,key,size),ORM_TDB_SUCCESS);
      else {
        if(bad>2) orm_sql_wire_write(key+1+ORM_SQL_WIRE_U64,ORM_SQL_WIRE_U64,2);
        const uint8_t invalid[]={0};
        check_equal(orm_tidesdb_txn_put(owner.transaction,family,key,size,invalid,sizeof(invalid),0),ORM_TDB_SUCCESS);
      }
      faults_clear(); check_equal(alter_on(&owner,column_rewrite_sql[op]),TURBODB_STATUS_DATASTORE_ERROR);
      check_true(owner.failed); check_equal(put_calls,0u); check_equal(delete_calls,0u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK);
      begin_owner(&owner); verify_live_indexes(ROWS,3);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK);
    }
    begin_owner(&owner);
  }
  it("keeps populated ALTER outcomes atomic across uncertain commits and native reopen") {
    seed(); build_many(); check_equal(alter_on(&owner,"ALTER TABLE items ADD spare BIGINT"),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    for(size_t op=0;op<COLUMN_REWRITE_CASES;++op) for(int fault=COMMIT_BEFORE;fault<=COMMIT_AFTER;++fault) {
      begin_owner(&owner); check_equal(alter_on(&owner,column_rewrite_sql[op]),TURBODB_STATUS_OK); commit_fault=fault;
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_COMMIT_UNKNOWN); faults_clear();
      orm_tidesdb_transaction_t *tx=NULL; reopen_raw(&tx); manifest_is(tx,2,4,5);
      check_equal(orm_tidesdb_txn_rollback(tx),ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx); begin_owner(&owner);
      orm_sql_table_definition table={0}; orm_sql_table_schema schema={0}; uint64_t id=0,version=0; bool found=false;
      check_equal(orm_tidesdb_sql_catalog_lookup(&owner,vstr_from_cstr(table_name),&table,&id,&version,&found,&error),TURBODB_STATUS_OK);
      check_true(found); check_equal(orm_tidesdb_sql_catalog_schema(&table,&schema,&error),TURBODB_STATUS_OK);
      check_equal(schema.count,fault==COMMIT_BEFORE?COLUMNS+1:(op==1?COLUMNS:COLUMNS+2)); check_equal(id,1u);
      check_equal(orm_tidesdb_sql_catalog_destroy(&table,&error),TURBODB_STATUS_OK); verify_live_indexes(ROWS,3);
      check_equal(lookup_open("SELECT id FROM items WHERE a=-9223372036854775808 AND b=18446744073709551615",NULL,0),TURBODB_STATUS_OK);
      lookup_row(INT64_MIN); lookup_end();
      if(fault==COMMIT_AFTER) check_equal(alter_on(&owner,column_rewrite_undo[op]),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    }
    begin_owner(&owner);
  }
  it("conflicts populated row rewrites with old-schema inserts and index builds in both commit orders") {
    seed(); build_many(); check_equal(alter_on(&owner,"ALTER TABLE items ADD spare BIGINT"),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    const turbodb_value_t input[]={turbodb_i64(10),turbodb_i64(11),turbodb_u64(12),turbodb_null()};
    for(size_t op=0;op<COLUMN_REWRITE_CASES;++op) for(size_t index=0;index<2;++index) for(size_t first=0;first<2;++first) {
      begin_owner(&owner); begin_owner(&other); uint64_t id=0;
      check_equal(alter_on(&owner,column_rewrite_sql[op]),TURBODB_STATUS_OK);
      if(index) check_equal(build_on(&other,"CREATE INDEX concurrent ON items(spare)",&id),TURBODB_STATUS_OK);
      else check_equal(orm_tidesdb_sql_relation_insert(&other,vstr_from_cstr(table_name),input,COLUMNS+1,&error),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(first?&owner:&other,true,&error),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(first?&other:&owner,true,&error),TURBODB_STATUS_BUSY);
      begin_owner(&owner); size_t affected=0;
      if(first) check_equal(alter_on(&owner,column_rewrite_undo[op]),TURBODB_STATUS_OK);
      else if(index) check_equal(drop_on(&owner,"DROP INDEX concurrent ON items",&id),TURBODB_STATUS_OK);
      else check_equal(execute_sql("DELETE FROM items WHERE id=10",&affected),TURBODB_STATUS_OK);
      verify_live_indexes(ROWS,3); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    }
    begin_owner(&owner);
  }
  it("unwinds all empty-table ALTER allocations and partial metadata writes") {
    empty_column_indexes(); save(); const char *sql[]={"ALTER TABLE items ADD c BIGINT","ALTER TABLE items DROP a"};
    for(size_t op=0;op<2;++op) {
      faults_clear(); check_equal(alter_on(&owner,sql[op]),TURBODB_STATUS_OK); const size_t counts[]={reserves,resizes,put_calls}; restore();
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(size_t phase=0;phase<3;++phase) for(size_t point=1;point<=counts[phase];++point) {
        faults_clear(); if(!phase) fail_reserve=point; else if(phase==1) fail_resize=point; else fail_put=point;
        check_equal(alter_on(&owner,sql[op]),phase==2?TURBODB_STATUS_DATASTORE_ERROR:TURBODB_STATUS_OUT_OF_MEMORY);
        check_false(owner.failed); check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
        if(phase!=2) check_equal(put_calls,0u); check_equal(delete_calls,0u);
        faults_clear(); verify_live_indexes(0,2); version_is(owner.transaction,3);
      }
    }
  }
  it("enforces exact empty-table ALTER resource quotas before metadata writes") {
    empty_column_indexes(); save(); const char *sql[]={"ALTER TABLE items ADD c BIGINT","ALTER TABLE items DROP a"};
    const orm_sql_budget_resource resources[]={ORM_SQL_BUDGET_WORK_BYTES,ORM_SQL_BUDGET_PLAN_NODES,ORM_SQL_BUDGET_AST_NODES,
      ORM_SQL_BUDGET_EXECUTION_STEPS,ORM_SQL_BUDGET_READ_ROWS,ORM_SQL_BUDGET_READ_BYTES,ORM_SQL_BUDGET_WRITE_ROWS,ORM_SQL_BUDGET_WRITE_BYTES};
    for(size_t op=0;op<2;++op) {
      check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
      check_equal(alter_on(&owner,sql[op]),TURBODB_STATUS_OK); const orm_sql_budget_amount peak=budget.peak; restore();
      for(size_t i=0;i<sizeof(resources)/sizeof(resources[0]);++i) {
        const orm_sql_budget_resource resource=resources[i]; const uint64_t original=budget.limits.statement.value[resource];
        check_true(peak.value[resource]>0);
        for(size_t exact=0;exact<2;++exact) {
          check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
          budget.limits.statement.value[resource]=peak.value[resource]-(exact?0:1); faults_clear();
          const turbodb_status_t status=alter_on(&owner,sql[op]); budget.limits.statement.value[resource]=original;
          check_equal(status,exact?TURBODB_STATUS_OK:TURBODB_STATUS_LIMIT_EXCEEDED);
          if(exact) restore(); else { check_equal(put_calls,0u); check_equal(delete_calls,0u); }
          check_false(owner.failed); verify_live_indexes(0,2); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],budget.retained_work_bytes);
        }
      }
    }
  }
  it("conflicts empty-table ADD and DROP with concurrent inserts in both commit orders") {
    empty_column_indexes(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    const char *sql[]={"ALTER TABLE items ADD c BIGINT","ALTER TABLE items DROP a"};
    const char *undo[]={"ALTER TABLE items DROP c","ALTER TABLE items ADD a BIGINT"};
    const turbodb_value_t input[]={turbodb_i64(10),turbodb_i64(11),turbodb_u64(12)};
    for(size_t op=0;op<2;++op) for(size_t first=0;first<2;++first) {
      begin_owner(&owner); begin_owner(&other); check_equal(alter_on(&owner,sql[op]),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_relation_insert(&other,vstr_from_cstr(table_name),input,COLUMNS,&error),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(first?&owner:&other,true,&error),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(first?&other:&owner,true,&error),TURBODB_STATUS_BUSY);
      begin_owner(&owner); size_t affected=0;
      if(first) check_equal(alter_on(&owner,undo[op]),TURBODB_STATUS_OK);
      else check_equal(execute_sql("DELETE FROM items WHERE id=10",&affected),TURBODB_STATUS_OK);
      verify_live_indexes(0,2); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    }
    begin_owner(&owner);
  }
  it("fails closed on all empty-table ALTER reads and iterator failures") {
    empty_column_indexes(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK); begin_owner(&owner); save();
    const char *sql[]={"ALTER TABLE items ADD c BIGINT","ALTER TABLE items DROP a"};
    for(size_t op=0;op<2;++op) {
      faults_clear(); check_equal(alter_on(&owner,sql[op]),TURBODB_STATUS_OK); const size_t counts[]={gets,news,FAIL_VALUE}; restore();
      for(size_t phase=0;phase<3;++phase) for(size_t point=1;point<=counts[phase];++point) {
        const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; faults_clear();
        if(!phase) fail_get=point; else if(phase==1) fail_new=point; else fail_iterator=(int)point;
        check_equal(alter_on(&owner,sql[op]),TURBODB_STATUS_DATASTORE_ERROR); check_equal(put_calls,0u); check_equal(delete_calls,0u);
        check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
        faults_clear(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK); begin_owner(&owner); save();
        verify_live_indexes(0,2);
      }
    }
  }
  it("rejects orphan Data generations and index entries rather than hiding them through schema changes") {
    empty_column_indexes(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    const char *sql[]={"ALTER TABLE items ADD c BIGINT","ALTER TABLE items DROP a"};
    const uint8_t spaces[]={3,INDEX_DATA_NS,INDEX_UNIQUE_NS};
    for(size_t op=0;op<2;++op) for(size_t i=0;i<sizeof(spaces)/sizeof(spaces[0]);++i) {
      begin_owner(&owner); uint8_t key[ORM_SQL_RELATION_PREFIX_BYTES]={0}, value[ORM_SQL_WIRE_U64]={0};
      key[0]=spaces[i]; orm_sql_wire_write(key+1,ORM_SQL_WIRE_U64,i?2:1);
      orm_sql_wire_write(key+1+ORM_SQL_WIRE_U64,ORM_SQL_WIRE_U64,2);
      check_equal(orm_tidesdb_txn_put(owner.transaction,family,key,sizeof(key),value,sizeof(value),0),ORM_TDB_SUCCESS);
      faults_clear(); check_equal(alter_on(&owner,sql[op]),TURBODB_STATUS_DATASTORE_ERROR);
      check_true(owner.failed); check_equal(put_calls,0u); check_equal(delete_calls,0u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK);
    }
    begin_owner(&owner); verify_live_indexes(0,2);
  }
  it("requires full rollback after empty-table ALTER savepoint cleanup failure") {
    empty_column_indexes(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK); begin_owner(&owner);
    const char *sql[]={"ALTER TABLE items ADD c BIGINT","ALTER TABLE items DROP a"};
    for(size_t op=0;op<2;++op) for(size_t phase=0;phase<3;++phase) {
      faults_clear(); if(!phase) fail_savepoint=true; else if(phase==1) fail_release=true; else { fail_put=2; fail_rollback=true; }
      size_t affected=OUTPUT_SENTINEL;
      check_equal(execute_sql(sql[op],&affected),TURBODB_STATUS_DATASTORE_ERROR); check_equal(affected,OUTPUT_SENTINEL);
      check_equal(owner.failed,phase!=0);
      faults_clear(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK); begin_owner(&owner);
      verify_live_indexes(0,2); version_is(owner.transaction,3);
    }
  }
  it("keeps empty-table ALTER outcomes atomic across uncertain commits and native reopen") {
    empty_column_indexes(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    const char *sql[]={"ALTER TABLE items ADD c BIGINT","ALTER TABLE items DROP a"};
    const size_t expected[]={4,2};
    for(size_t op=0;op<2;++op) for(int fault=COMMIT_BEFORE;fault<=COMMIT_AFTER;++fault) {
      begin_owner(&owner); check_equal(alter_on(&owner,sql[op]),TURBODB_STATUS_OK); commit_fault=fault;
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_COMMIT_UNKNOWN); faults_clear();
      orm_tidesdb_transaction_t *tx=NULL; reopen_raw(&tx); manifest_is(tx,2,3,4);
      check_equal(orm_tidesdb_txn_rollback(tx),ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx); begin_owner(&owner);
      orm_sql_table_definition table={0}; orm_sql_table_schema schema={0}; uint64_t id=0,version=0; bool found=false;
      check_equal(orm_tidesdb_sql_catalog_lookup(&owner,vstr_from_cstr(table_name),&table,&id,&version,&found,&error),TURBODB_STATUS_OK);
      check_true(found); check_equal(orm_tidesdb_sql_catalog_schema(&table,&schema,&error),TURBODB_STATUS_OK);
      check_equal(schema.count,fault==COMMIT_BEFORE?COLUMNS:expected[op]); check_equal(id,1u);
      check_equal(orm_tidesdb_sql_catalog_destroy(&table,&error),TURBODB_STATUS_OK);
      if(!op && fault==COMMIT_AFTER) check_equal(alter_on(&owner,"ALTER TABLE items DROP c"),TURBODB_STATUS_OK);
      verify_live_indexes(0,2); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    }
    begin_owner(&owner);
  }
  it("serializes column changes with concurrent index creation on the old schema") {
    empty_column_indexes(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    const char *sql[]={"ALTER TABLE items ADD c BIGINT","ALTER TABLE items DROP a"};
    const char *undo[]={"ALTER TABLE items DROP c","ALTER TABLE items ADD a BIGINT"};
    for(size_t op=0;op<2;++op) for(size_t first=0;first<2;++first) {
      begin_owner(&owner); begin_owner(&other); uint64_t id=0;
      check_equal(alter_on(&owner,sql[op]),TURBODB_STATUS_OK);
      check_equal(build_on(&other,"CREATE INDEX concurrent ON items(a)",&id),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(first?&owner:&other,true,&error),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(first?&other:&owner,true,&error),TURBODB_STATUS_BUSY);
      begin_owner(&owner);
      if(first) check_equal(alter_on(&owner,undo[op]),TURBODB_STATUS_OK);
      else check_equal(drop_on(&owner,"DROP INDEX concurrent ON items",&id),TURBODB_STATUS_OK);
      verify_live_indexes(0,2); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    }
    begin_owner(&owner);
  }
  it("uses nullable composite lookup for IS NULL and null-safe equality") {
    seed(); build_many();
    check_equal(lookup_open("SELECT id FROM items WHERE a IS NULL AND b=7 ORDER BY id", NULL, 0), TURBODB_STATUS_OK);
    check_true(lookup_query.as.select.source.lookup.contains_null);
    const uint64_t before = budget.used.value[ORM_SQL_BUDGET_READ_ROWS]; lookup_row(0); lookup_row(1); lookup_end();
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS] - before, 4u);
    const turbodb_value_t params[] = {turbodb_null(), turbodb_u64(7)};
    check_equal(lookup_open("SELECT id FROM items WHERE a<=>? AND b=?", params, 2), TURBODB_STATUS_OK);
    lookup_row(0); lookup_row(1); lookup_end();
    check_equal(lookup_open("SELECT id FROM items WHERE a=NULL AND b=7", NULL, 0), TURBODB_STATUS_OK);
    check_true(lookup_query.as.select.source.lookup.empty); faults_clear(); lookup_end(); check_equal(news, 0u); check_equal(gets, 0u);
  }
  it("normalizes integer equality probes exactly at signed and unsigned boundaries") {
    seed(); build_many();
    check_equal(lookup_open("SELECT id FROM items WHERE a=-9223372036854775808 AND b=18446744073709551615", NULL, 0), TURBODB_STATUS_OK);
    lookup_row(INT64_MIN); lookup_end();
    const char *empty[] = {"SELECT id FROM items WHERE a=18446744073709551615 AND b=7",
      "SELECT id FROM items WHERE a=0 AND b=-1", "SELECT id FROM items WHERE id IS NULL"};
    for (size_t i = 0; i < sizeof(empty) / sizeof(empty[0]); ++i) {
      check_equal(lookup_open(empty[i], NULL, 0), TURBODB_STATUS_OK); check_true(lookup_query.as.select.source.lookup.empty);
      faults_clear(); lookup_end(); check_equal(news, 0u); check_equal(gets, 0u);
    }
    const turbodb_value_t params[] = {turbodb_u64(0), turbodb_i64(7)};
    check_equal(lookup_open("SELECT id FROM items WHERE a=? AND b=?", params, 2), TURBODB_STATUS_OK); lookup_row(-1); lookup_end();
  }
  it("retains ordinary scanning for cross-index OR, NOT BETWEEN and expression predicates") {
    seed(); build_many();
    const char *scan[] = {"SELECT id FROM items WHERE a NOT BETWEEN -1 AND -1 AND a=0 ORDER BY id",
      "SELECT id FROM items WHERE (a=0 AND b=7) OR id=9223372036854775807 ORDER BY id",
      "SELECT id FROM items WHERE a+0=0 AND b=7 OR id=9223372036854775807 ORDER BY id"};
    for (size_t i = 0; i < sizeof(scan) / sizeof(scan[0]); ++i) {
      check_equal(lookup_open(scan[i], NULL, 0), TURBODB_STATUS_OK); check_null(lookup_query.as.select.source.lookup.budget);
      const uint64_t before = budget.used.value[ORM_SQL_BUDGET_READ_ROWS]; lookup_row(-1); lookup_row(INT64_MAX); lookup_end();
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS] - before, ROWS);
    }
  }
  it("matches an independent OR and IN oracle for overlapping ASC and DESC intervals") {
    enum { INPUT_ROWS = 8, BOUNDS = 3 };
    const turbodb_value_t input[] = {turbodb_i64(0),turbodb_i64(INT64_MIN),turbodb_u64(UINT64_MAX),
      turbodb_i64(1),turbodb_i64(-1),turbodb_u64(0), turbodb_i64(2),turbodb_i64(0),turbodb_u64(7),
      turbodb_i64(3),turbodb_i64(1),turbodb_u64(8), turbodb_i64(4),turbodb_i64(INT64_MAX),turbodb_u64(UINT64_MAX),
      turbodb_i64(5),turbodb_null(),turbodb_u64(7), turbodb_i64(6),turbodb_i64(0),turbodb_null(), turbodb_i64(7),turbodb_i64(0),turbodb_u64(8)};
    check_equal(orm_tidesdb_sql_relation_insert_rows(&owner, vstr_from_cstr(table_name), input, INPUT_ROWS, COLUMNS, &error), TURBODB_STATUS_OK);
    save();
    const char *indexes[] = {"CREATE INDEX ix ON items (a ASC,b DESC)", "CREATE INDEX ix ON items (a DESC,b ASC)"};
    for (size_t direction = 0; direction < 2; ++direction) {
      uint64_t id = 0; check_equal(build_on(&owner, indexes[direction], &id), TURBODB_STATUS_OK);
      for (size_t lower = 0; lower < BOUNDS; ++lower) for (size_t upper = 0; upper < BOUNDS; ++upper)
        for (size_t point = 0; point < BOUNDS; ++point) {
          const int64_t lo = (int64_t)lower - 1, hi = (int64_t)upper - 1, value = (int64_t)point - 1;
          const turbodb_value_t params[] = {turbodb_i64(lo),turbodb_i64(hi),turbodb_i64(value),turbodb_i64(lo)};
          check_equal(lookup_open("SELECT id FROM items WHERE (a>=? AND a<=?) OR a IN (?,?) OR a IS NULL ORDER BY id", params, 4), TURBODB_STATUS_OK);
          check_not_null(lookup_query.as.select.source.lookup.budget);
          check_equal(lookup_query.as.select.source.lookup.access, ORM_SQL_INDEX_RANGE);
          const uint64_t reads = budget.used.value[ORM_SQL_BUDGET_READ_ROWS]; size_t matched = 0;
          for (size_t r = 0; r < INPUT_ROWS; ++r) {
            const turbodb_value_t a = input[r*COLUMNS+1];
            if (a.kind == TURBODB_VALUE_NULL || (a.data.int64_value >= lo && a.data.int64_value <= hi) ||
                a.data.int64_value == value || a.data.int64_value == lo) { lookup_row((int64_t)r); ++matched; }
          }
          lookup_end(); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS] - reads, matched * 2);
        }
      restore();
    }
  }
  it("deduplicates IN probes and excludes NULL and out-of-domain list items") {
    seed(); build_many();
    check_equal(lookup_open("SELECT id FROM items WHERE a IN (0,NULL,-9223372036854775808,0,18446744073709551615) ORDER BY id", NULL, 0), TURBODB_STATUS_OK);
    check_equal(lookup_query.as.select.source.lookup.range_count, 2u);
    const uint64_t before = budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    lookup_row(INT64_MIN); lookup_row(-1); lookup_row(INT64_MAX); lookup_end();
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS] - before, 9u);
    const char *queries[] = {"SELECT id FROM items WHERE a=0 AND b IN (8,8,NULL,-1)",
      "SELECT id FROM items WHERE a IN (0,0) AND b>7", "SELECT id FROM items WHERE a IN (0,1) AND a=0 AND b=8",
      "SELECT id FROM items WHERE a>=0 AND b IN (8,NULL)", "SELECT id FROM items WHERE a=0 AND b IN (8) AND id>0"};
    for (size_t i = 0; i < sizeof(queries)/sizeof(queries[0]); ++i) {
      check_equal(lookup_open(queries[i], NULL, 0), TURBODB_STATUS_OK);
      check_not_null(lookup_query.as.select.source.lookup.budget); lookup_row(INT64_MAX); lookup_end();
    }
    const char *empty[] = {"SELECT id FROM items WHERE a IN (NULL,18446744073709551615)",
      "SELECT id FROM items WHERE b IN (-1,NULL)", "SELECT id FROM items WHERE a IN (NULL) OR a>9223372036854775807"};
    for (size_t i = 0; i < sizeof(empty)/sizeof(empty[0]); ++i) {
      check_equal(lookup_open(empty[i], NULL, 0), TURBODB_STATUS_OK);
      check_true(lookup_query.as.select.source.lookup.empty); faults_clear(); lookup_end(); check_equal(news, 0u); check_equal(gets, 0u);
    }
  }
  it("merges contained, adjacent and duplicate composite intervals without repeating rows") {
    seed(); build_many();
    const char *queries[] = {"SELECT id FROM items WHERE a=0 OR (a=0 AND b=7) OR a=0 ORDER BY id",
      "SELECT id FROM items WHERE (a=0 AND b>=7 AND b<8) OR (a=0 AND b>=8 AND b<=8) ORDER BY id",
      "SELECT id FROM items WHERE (a=0 AND b=7) OR a=0 ORDER BY id"};
    for (size_t i = 0; i < sizeof(queries)/sizeof(queries[0]); ++i) {
      check_equal(lookup_open(queries[i], NULL, 0), TURBODB_STATUS_OK);
      check_equal(lookup_query.as.select.source.lookup.range_count, 1u);
      check_equal(lookup_query.as.select.source.lookup.bound_bytes, TUPLE_BYTES);
      const uint64_t before = budget.used.value[ORM_SQL_BUDGET_READ_ROWS]; lookup_row(-1); lookup_row(INT64_MAX); lookup_end();
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS] - before, 6u);
    }
    check_equal(lookup_open("SELECT COUNT(*) AS n FROM items WHERE a=0 OR a=0", NULL, 0), TURBODB_STATUS_OK); lookup_row(2); lookup_end();
  }
  it("retains scanning when a union lacks one common index or needs distributive expansion") {
    seed(); build_many();
    const char *queries[] = {"SELECT id FROM items WHERE a IN (0) AND b IN (7,8) ORDER BY id",
      "SELECT id FROM items WHERE a=0 AND (b=7 OR b=8) ORDER BY id",
      "SELECT id FROM items WHERE a NOT IN (1) AND a=0 ORDER BY id",
      "SELECT id FROM items WHERE a IN (0+0) ORDER BY id"};
    for (size_t i = 0; i < sizeof(queries)/sizeof(queries[0]); ++i) {
      check_equal(lookup_open(queries[i], NULL, 0), TURBODB_STATUS_OK);
      check_null(lookup_query.as.select.source.lookup.budget); lookup_row(-1); lookup_row(INT64_MAX); lookup_end();
    }
  }
  it("rebinds IN lists and OR NULL branches after destroying the AST") {
    seed(); build_many(); const char sql[] = "SELECT id FROM items WHERE a IN (?,?) OR a<=>? ORDER BY id";
    sqlparser_document *doc = NULL; sqlparser_error diagnostic;
    check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &diagnostic), SQLPARSER_OK);
    const orm_sql_type types[] = {{TURBODB_VALUE_INT64,true},{TURBODB_VALUE_INT64,true},{TURBODB_VALUE_INT64,true}};
    const turbodb_value_t inputs[][3] = {{turbodb_i64(0),turbodb_i64(0),turbodb_i64(0)}, {turbodb_null(),turbodb_null(),turbodb_i64(9)},
      {turbodb_i64(INT64_MIN),turbodb_null(),turbodb_null()}, {turbodb_i64(0),turbodb_i64(INT64_MIN),turbodb_i64(0)}};
    enum { DEPTH = 32 }; const orm_sql_query_scope scope = {doc, sqlparser_statements(doc).first, types, 3, DEPTH, &budget};
    check_equal(orm_tidesdb_sql_runtime_block_open(&scope, &owner, inputs[0], false, &lookup_query, &error), TURBODB_STATUS_OK);
    sqlparser_document_destroy(doc);
    for (size_t run = 0; run < sizeof(inputs)/sizeof(inputs[0]); ++run) {
      if (run) check_equal(orm_sql_runtime_execution_open(&lookup_query, inputs[run], 3, NULL, &error), TURBODB_STATUS_OK);
      if (run >= 2) lookup_row(INT64_MIN);
      if (run == 0 || run == 3) { lookup_row(-1); lookup_row(INT64_MAX); }
      if (run == 2) { lookup_row(0); lookup_row(1); }
      orm_sql_scan_row row = {0}; check_equal(orm_tidesdb_sql_runtime_next(&lookup_query, &row, &error), TURBODB_STATUS_OK);
      check_equal(row.state, ORM_SQL_SCAN_DONE);
      check_equal(orm_sql_runtime_execution_close(&lookup_query, &error), TURBODB_STATUS_OK);
    }
    check_equal(orm_tidesdb_sql_runtime_close(&lookup_query, &error), TURBODB_STATUS_OK);
  }
  it("keeps EXPLAIN, zero LIMIT and cancellation lazy for multi-range plans") {
    seed(); build_many();
    check_equal(lookup_open("EXPLAIN SELECT id+9223372036854775807 AS n FROM items WHERE a=0 OR (a=0 AND b=7)", NULL, 0), TURBODB_STATUS_OK);
    check_equal(strcmp(lookup_query.as.select.explain.source.key_length, "18"), 0);
    faults_clear(); orm_sql_scan_row row = {0};
    check_equal(orm_tidesdb_sql_runtime_next(&lookup_query, &row, &error), TURBODB_STATUS_OK);
    check_equal(row.state, ORM_SQL_SCAN_ROW); check_equal(strcmp(row.values[4].data.text_value.data,"range"), 0);
    check_equal(row.values[8].kind, TURBODB_VALUE_NULL); lookup_end(); check_equal(gets, 0u); check_equal(news, 0u);
    check_equal(lookup_open("SELECT id FROM items WHERE a IN (0,-9223372036854775808) LIMIT 0", NULL, 0), TURBODB_STATUS_OK);
    faults_clear(); lookup_end(); check_equal(gets, 0u); check_equal(news, 0u);
    check_equal(lookup_open("SELECT id FROM items WHERE a IN (0,-9223372036854775808)", NULL, 0), TURBODB_STATUS_OK);
    lookup_row(INT64_MIN); faults_clear(); check_equal(orm_tidesdb_sql_runtime_cancel(&lookup_query, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_runtime_close(&lookup_query, &error), TURBODB_STATUS_OK); check_equal(gets, 0u); check_equal(news, 0u);
  }
  it("locks a seek failure between two disjoint ranges without publishing another row") {
    seed(); build_many();
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); begin_owner(&owner);
    check_equal(lookup_open("SELECT id FROM items WHERE a IN (0,-9223372036854775808)", NULL, 0), TURBODB_STATUS_OK);
    lookup_row(INT64_MIN); faults_clear(); fail_iterator = FAIL_SEEK;
    orm_sql_scan_row row = {.count=OUTPUT_SENTINEL};
    check_equal(orm_tidesdb_sql_runtime_next(&lookup_query, &row, &error), TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(row.count, OUTPUT_SENTINEL); check_true(owner.failed); check_equal(gets, 0u);
    faults_clear();
    check_equal(orm_tidesdb_sql_runtime_next(&lookup_query, &row, &error), TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(gets, 0u); check_equal(news, 0u);
    check_equal(orm_tidesdb_sql_runtime_close(&lookup_query, &error), TURBODB_STATUS_OK);
  }
  it("propagates multi-range ordering allocation failure and releases all plan storage") {
    seed(); build_many(); const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    faults_clear(); fail_sort = true;
    check_equal(lookup_open("SELECT id FROM items WHERE a IN (0,-9223372036854775808)", NULL, 0), TURBODB_STATUS_OUT_OF_MEMORY);
    check_null(lookup_query.owner); check_equal(owner.active_sources, 0u); check_false(owner.failed);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
  }
  it("reads an equality left prefix without entering its adjacent tuples") {
    seed(); build_many();
    check_equal(lookup_open("SELECT id FROM items WHERE a=0 ORDER BY id", NULL, 0), TURBODB_STATUS_OK);
    check_equal(lookup_query.as.select.source.lookup.access, ORM_SQL_INDEX_PREFIX);
    check_equal(lookup_query.as.select.source.lookup.bound_bytes, TUPLE_BYTES / 2);
    const uint64_t before = budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    lookup_row(-1); lookup_row(INT64_MAX); lookup_end();
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS] - before, 6u);
    check_equal(lookup_open("SELECT id FROM items WHERE a IS NULL ORDER BY id", NULL, 0), TURBODB_STATUS_OK);
    const uint64_t nullable = budget.used.value[ORM_SQL_BUDGET_READ_ROWS]; lookup_row(0); lookup_row(1); lookup_end();
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS] - nullable, 4u);
    check_equal(lookup_open("SELECT id FROM items WHERE a=0 AND b>6 ORDER BY id", NULL, 0), TURBODB_STATUS_OK);
    check_equal(lookup_query.as.select.source.lookup.access, ORM_SQL_INDEX_RANGE);
    lookup_row(-1); lookup_row(INT64_MAX); lookup_end();
  }
  it("matches the integer oracle for every ASC and DESC open or closed range combination") {
    enum { INPUT_ROWS = 8, BOUNDS = 3 };
    const turbodb_value_t input[] = {turbodb_i64(0),turbodb_i64(INT64_MIN),turbodb_u64(UINT64_MAX),
      turbodb_i64(1),turbodb_i64(-1),turbodb_u64(0), turbodb_i64(2),turbodb_i64(0),turbodb_u64(7),
      turbodb_i64(3),turbodb_i64(1),turbodb_u64(8), turbodb_i64(4),turbodb_i64(INT64_MAX),turbodb_u64(UINT64_MAX),
      turbodb_i64(5),turbodb_null(),turbodb_u64(7), turbodb_i64(6),turbodb_i64(0),turbodb_null(), turbodb_i64(7),turbodb_i64(0),turbodb_u64(8)};
    check_equal(orm_tidesdb_sql_relation_insert_rows(&owner, vstr_from_cstr(table_name), input, INPUT_ROWS, COLUMNS, &error), TURBODB_STATUS_OK);
    save();
    const char *indexes[] = {"CREATE INDEX ix ON items (a ASC,b DESC)", "CREATE INDEX ix ON items (a DESC,b ASC)"};
    for (size_t direction = 0; direction < 2; ++direction) {
      uint64_t id = 0; check_equal(build_on(&owner, indexes[direction], &id), TURBODB_STATUS_OK);
      for (size_t lower = 0; lower < BOUNDS; ++lower) for (size_t upper = 0; upper < BOUNDS; ++upper)
        for (size_t inclusive = 0; inclusive < 4; ++inclusive) {
          const int64_t lo = (int64_t)lower - 1, hi = (int64_t)upper - 1;
          char sql[128]; const int size = snprintf(sql, sizeof(sql), "SELECT id FROM items WHERE a%s? AND a%s? ORDER BY id",
              inclusive & 1 ? ">=" : ">", inclusive & 2 ? "<=" : "<");
          check_true(size > 0 && (size_t)size < sizeof(sql)); const turbodb_value_t params[] = {turbodb_i64(lo),turbodb_i64(hi)};
          check_equal(lookup_open(sql, params, 2), TURBODB_STATUS_OK); check_equal(lookup_query.as.select.source.lookup.access, ORM_SQL_INDEX_RANGE);
          const uint64_t reads = budget.used.value[ORM_SQL_BUDGET_READ_ROWS]; size_t matched = 0;
          for (size_t r = 0; r < INPUT_ROWS; ++r) {
            const turbodb_value_t value = input[r*COLUMNS+1]; if (value.kind == TURBODB_VALUE_NULL) continue;
            if ((inclusive & 1 ? value.data.int64_value >= lo : value.data.int64_value > lo) &&
                (inclusive & 2 ? value.data.int64_value <= hi : value.data.int64_value < hi)) { lookup_row((int64_t)r); ++matched; }
          }
          lookup_end(); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS] - reads, matched * 2);
        }
      restore();
    }
  }
  it("bounds the next composite key column and ignores later parts only for interval construction") {
    seed(); uint64_t id = 0;
    const char *indexes[] = {"CREATE INDEX ix ON items (a ASC,b DESC,id)", "CREATE INDEX ix ON items (a DESC,b ASC,id DESC)"};
    save();
    for (size_t direction = 0; direction < 2; ++direction) {
      check_equal(build_on(&owner, indexes[direction], &id), TURBODB_STATUS_OK);
      const char *queries[] = {"SELECT id FROM items WHERE a=0 AND b>7 AND b<=8", "SELECT id FROM items WHERE a=0 AND 7<b AND 8>=b",
        "SELECT id FROM items t WHERE t.a=0 AND t.b BETWEEN 8 AND 9", "SELECT id FROM items WHERE a=0 AND b>=7 AND b<=8 AND id>0",
        "SELECT id FROM items WHERE a=0 AND b>=0 AND b>=8 AND b<=8"};
      for (size_t i = 0; i < sizeof(queries)/sizeof(queries[0]); ++i) {
        check_equal(lookup_open(queries[i], NULL, 0), TURBODB_STATUS_OK);
        check_equal(lookup_query.as.select.source.lookup.access, ORM_SQL_INDEX_RANGE);
        check_equal(lookup_query.as.select.source.lookup.equal_parts, 1u);
        check_equal(lookup_query.as.select.source.lookup.bound_bytes, TUPLE_BYTES);
        lookup_row(INT64_MAX); lookup_end();
      }
      check_equal(lookup_open("SELECT id FROM items WHERE b=7 ORDER BY id", NULL, 0), TURBODB_STATUS_OK);
      check_null(lookup_query.as.select.source.lookup.budget); lookup_row(-1); lookup_row(0); lookup_row(1); lookup_end();
      restore();
    }
  }
  it("excludes NULL range values and handles signed or unsigned out-of-domain endpoints") {
    seed(); build_many();
    const char *empty[] = {"SELECT id FROM items WHERE a>18446744073709551615", "SELECT id FROM items WHERE a<-9223372036854775808",
      "SELECT id FROM items WHERE a>9223372036854775807", "SELECT id FROM items WHERE a=0 AND b<0",
      "SELECT id FROM items WHERE a=0 AND b<=-1", "SELECT id FROM items WHERE b>18446744073709551615",
      "SELECT id FROM items WHERE a BETWEEN 1 AND -1", "SELECT id FROM items WHERE a>=NULL",
      "SELECT id FROM items WHERE a IS NULL AND b BETWEEN NULL AND 8"};
    for (size_t i = 0; i < sizeof(empty)/sizeof(empty[0]); ++i) {
      check_equal(lookup_open(empty[i], NULL, 0), TURBODB_STATUS_OK); check_true(lookup_query.as.select.source.lookup.empty);
      faults_clear(); lookup_end(); check_equal(news, 0u); check_equal(gets, 0u);
    }
    const char *wide[] = {"SELECT id FROM items WHERE a<18446744073709551615 ORDER BY id",
      "SELECT id FROM items WHERE a<=18446744073709551615 ORDER BY id", "SELECT id FROM items WHERE a>=-9223372036854775808 ORDER BY id"};
    for (size_t i = 0; i < sizeof(wide)/sizeof(wide[0]); ++i) {
      check_equal(lookup_open(wide[i], NULL, 0), TURBODB_STATUS_OK); lookup_row(INT64_MIN); lookup_row(-1); lookup_row(INT64_MAX); lookup_end();
    }
    check_equal(lookup_open("SELECT id FROM items WHERE a=0 AND b>-1 ORDER BY id", NULL, 0), TURBODB_STATUS_OK);
    lookup_row(-1); lookup_row(INT64_MAX); lookup_end();
    check_equal(lookup_open("SELECT id FROM items WHERE b>=18446744073709551615", NULL, 0), TURBODB_STATUS_OK); lookup_row(INT64_MIN); lookup_end();
  }
  it("carries a DESC NULL prefix boundary and checks actual nullable unique tuples") {
    seed(); uint64_t id = 0; check_equal(build_on(&owner, "CREATE UNIQUE INDEX ix ON items (a DESC,b DESC)", &id), TURBODB_STATUS_OK);
    const turbodb_value_t nullable[] = {turbodb_i64(10),turbodb_i64(0),turbodb_null()};
    check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr(table_name), nullable, COLUMNS, &error), TURBODB_STATUS_OK);
    check_equal(lookup_open("SELECT id FROM items WHERE a IS NULL ORDER BY id", NULL, 0), TURBODB_STATUS_OK);
    check_true(lookup_query.as.select.source.lookup.upper_size < lookup_query.as.select.source.lookup.lower_size);
    lookup_row(0); lookup_row(1); lookup_end();
    check_equal(lookup_open("SELECT id FROM items WHERE a=0 ORDER BY id", NULL, 0), TURBODB_STATUS_OK);
    const uint64_t before = budget.used.value[ORM_SQL_BUDGET_READ_ROWS]; lookup_row(-1); lookup_row(10); lookup_row(INT64_MAX); lookup_end();
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS] - before, 8u);
    check_equal(lookup_open("SELECT id FROM items WHERE a=0 AND b>=0 ORDER BY id", NULL, 0), TURBODB_STATUS_OK);
    lookup_row(-1); lookup_row(INT64_MAX); lookup_end();
  }
  it("rebinds range endpoints and nullable prefixes across nonempty and empty executions") {
    seed(); build_many(); const char sql[] = "SELECT id FROM items WHERE a<=>? AND b>=? AND b<? ORDER BY id";
    sqlparser_document *doc = NULL; sqlparser_error diagnostic;
    check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &diagnostic), SQLPARSER_OK);
    const orm_sql_type types[] = {{TURBODB_VALUE_INT64,true}, {TURBODB_VALUE_UINT64,false}, {TURBODB_VALUE_UINT64,false}};
    const turbodb_value_t inputs[][3] = {{turbodb_i64(0),turbodb_u64(7),turbodb_u64(8)}, {turbodb_i64(0),turbodb_u64(8),turbodb_u64(9)},
      {turbodb_i64(0),turbodb_u64(8),turbodb_u64(8)}, {turbodb_null(),turbodb_u64(7),turbodb_u64(8)}};
    enum { DEPTH = 32 }; const orm_sql_query_scope scope = {doc, sqlparser_statements(doc).first, types, 3, DEPTH, &budget};
    check_equal(orm_tidesdb_sql_runtime_block_open(&scope, &owner, inputs[0], false, &lookup_query, &error), TURBODB_STATUS_OK);
    sqlparser_document_destroy(doc);
    for (size_t run = 0; run < sizeof(inputs)/sizeof(inputs[0]); ++run) {
      if (run) check_equal(orm_sql_runtime_execution_open(&lookup_query, inputs[run], 3, NULL, &error), TURBODB_STATUS_OK);
      check_equal(lookup_query.as.select.source.lookup.access, ORM_SQL_INDEX_RANGE);
      if (!run) lookup_row(-1);
      else if (run == 1) lookup_row(INT64_MAX);
      else if (run == 3) { lookup_row(0); lookup_row(1); }
      orm_sql_scan_row row = {0}; check_equal(orm_tidesdb_sql_runtime_next(&lookup_query, &row, &error), TURBODB_STATUS_OK);
      check_equal(row.state, ORM_SQL_SCAN_DONE);
      check_equal(orm_sql_runtime_execution_close(&lookup_query, &error), TURBODB_STATUS_OK);
    }
    check_equal(orm_tidesdb_sql_runtime_close(&lookup_query, &error), TURBODB_STATUS_OK);
  }
  it("checks the complete stored tuple when a prefix lookup touches a modified suffix") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); begin_owner(&owner);
    check_equal(lookup_open("SELECT id FROM items WHERE a=0", NULL, 0), TURBODB_STATUS_OK);
    uint8_t key[ORM_SQL_RELATION_KEY_BYTES], *data = NULL; size_t size = 0;
    memcpy(key, lookup_query.as.select.source.prefix, ORM_SQL_RELATION_PREFIX_BYTES);
    orm_sql_wire_order_write(key + ORM_SQL_RELATION_PREFIX_BYTES, orm_sql_wire_signed_order(INT64_MAX));
    check_equal(orm_tidesdb_txn_get(owner.transaction, family, key, sizeof(key), &data, &size), ORM_TDB_SUCCESS);
    /* The equality prefix a=0 is unchanged; only the unbound b suffix is corrupt. */
    data[REL_HEADER + 2*REL_CELL + 1] ^= 1;
    check_equal(orm_tidesdb_txn_put(owner.transaction, family, key, sizeof(key), data, size, 0), ORM_TDB_SUCCESS); orm_tidesdb_free(data);
    orm_sql_scan_row row = {.count=OUTPUT_SENTINEL};
    check_equal(orm_tidesdb_sql_runtime_next(&lookup_query, &row, &error), TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(row.count, OUTPUT_SENTINEL); check_true(owner.failed);
    check_equal(orm_tidesdb_sql_runtime_close(&lookup_query, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); begin_owner(&owner); verify_live_indexes(ROWS, 3);
  }
  it("admits range lookup work and read budgets at exact capacity and rejects one unit less") {
    seed(); build_many(); const char sql[] = "SELECT id FROM items WHERE a=0 AND b>7 AND b<=8";
    sqlparser_document *doc = NULL; sqlparser_error diagnostic;
    check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &diagnostic), SQLPARSER_OK);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    check_equal(lookup_measure(doc, INT64_MAX), TURBODB_STATUS_OK); const orm_sql_budget_amount peak = budget.peak;
    const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_WORK_BYTES, ORM_SQL_BUDGET_EXECUTION_STEPS,
      ORM_SQL_BUDGET_READ_ROWS, ORM_SQL_BUDGET_READ_BYTES};
    for (size_t i = 0; i < sizeof(resources)/sizeof(resources[0]); ++i) {
      const orm_sql_budget_resource resource = resources[i]; const uint64_t original = budget.limits.statement.value[resource];
      for (size_t exact = 0; exact < 2; ++exact) {
        check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
        check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
        budget.limits.statement.value[resource] = peak.value[resource] - (exact ? 0 : 1); faults_clear();
        const turbodb_status_t status = lookup_measure(doc, INT64_MAX); budget.limits.statement.value[resource] = original;
        check_equal(status, exact ? TURBODB_STATUS_OK : TURBODB_STATUS_LIMIT_EXCEEDED);
        check_false(owner.failed); check_equal(owner.active_sources, 0u); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], budget.retained_work_bytes);
      }
    }
    sqlparser_document_destroy(doc);
  }
  it("admits multi-range lookup work and read budgets at exact capacity and rejects one unit less") {
    seed(); build_many(); const char sql[] = "SELECT id FROM items WHERE (a IN (0,0,NULL) AND b=8) OR a=99";
    sqlparser_document *doc = NULL; sqlparser_error diagnostic;
    check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &diagnostic), SQLPARSER_OK);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    check_equal(lookup_measure(doc, INT64_MAX), TURBODB_STATUS_OK); const orm_sql_budget_amount peak = budget.peak;
    const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_WORK_BYTES, ORM_SQL_BUDGET_EXECUTION_STEPS,
      ORM_SQL_BUDGET_READ_ROWS, ORM_SQL_BUDGET_READ_BYTES};
    for (size_t i = 0; i < sizeof(resources)/sizeof(resources[0]); ++i) {
      const orm_sql_budget_resource resource = resources[i]; const uint64_t original = budget.limits.statement.value[resource];
      for (size_t exact = 0; exact < 2; ++exact) {
        check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
        check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
        budget.limits.statement.value[resource] = peak.value[resource] - (exact ? 0 : 1); faults_clear();
        const turbodb_status_t status = lookup_measure(doc, INT64_MAX); budget.limits.statement.value[resource] = original;
        check_equal(status, exact ? TURBODB_STATUS_OK : TURBODB_STATUS_LIMIT_EXCEEDED);
        check_false(owner.failed); check_equal(owner.active_sources, 0u); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], budget.retained_work_bytes);
      }
    }
    sqlparser_document_destroy(doc);
  }
  it("copies parameter probes and preserves lazy LIMIT zero, cancel and statement resume") {
    seed(); build_many(); turbodb_value_t params[] = {turbodb_i64(10), turbodb_i64(0), turbodb_u64(7)};
    check_equal(lookup_open("SELECT id+? AS n FROM items WHERE a=? AND b=?", params, 3), TURBODB_STATUS_OK);
    params[2] = turbodb_u64(8); lookup_row(9);
    check_equal(orm_sql_runtime_execution_close(&lookup_query, &error), TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_execution_resume(&lookup_query, &error), TURBODB_STATUS_OK); lookup_row(9); lookup_end();
    check_equal(lookup_open("SELECT id FROM items WHERE a=0 AND b=7 LIMIT 0", NULL, 0), TURBODB_STATUS_OK);
    faults_clear(); lookup_end(); check_equal(news, 0u); check_equal(gets, 0u);
    check_equal(lookup_open("SELECT id FROM items WHERE a=0 AND b=7", NULL, 0), TURBODB_STATUS_OK); faults_clear();
    check_equal(orm_tidesdb_sql_runtime_cancel(&lookup_query, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_runtime_close(&lookup_query, &error), TURBODB_STATUS_OK); check_equal(news, 0u); check_equal(gets, 0u);
  }
  it("re-encodes a reusable query block from new parameters after the AST is destroyed") {
    seed(); build_many(); const char sql[] = "SELECT id FROM items WHERE a=? AND b=?";
    sqlparser_document *doc = NULL; sqlparser_error diagnostic;
    check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &diagnostic), SQLPARSER_OK);
    const orm_sql_type types[] = {{TURBODB_VALUE_INT64,false}, {TURBODB_VALUE_UINT64,false}};
    const turbodb_value_t first[] = {turbodb_i64(0),turbodb_u64(7)}, second[] = {turbodb_i64(0),turbodb_u64(8)};
    enum { DEPTH = 32 };
    const orm_sql_query_scope scope = {doc, sqlparser_statements(doc).first, types, 2, DEPTH, &budget};
    check_equal(orm_tidesdb_sql_runtime_block_open(&scope, &owner, first, false, &lookup_query, &error), TURBODB_STATUS_OK);
    sqlparser_document_destroy(doc); lookup_row(-1);
    check_equal(orm_sql_runtime_execution_close(&lookup_query, &error), TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_execution_open(&lookup_query, second, 2, NULL, &error), TURBODB_STATUS_OK);
    lookup_row(INT64_MAX); lookup_end();
  }
  it("uses ordinary nonunique indexes for duplicate tuples and keeps ordering and aggregation") {
    seed(); uint64_t id = 0; check_equal(build_on(&owner, "CREATE INDEX by_a ON items (a DESC)", &id), TURBODB_STATUS_OK);
    check_equal(lookup_open("SELECT id FROM items WHERE a=0 ORDER BY id DESC LIMIT 1 OFFSET 1", NULL, 0), TURBODB_STATUS_OK);
    check_not_null(lookup_query.as.select.source.lookup.budget); lookup_row(-1); lookup_end();
    check_equal(lookup_open("SELECT COUNT(*) AS n FROM items WHERE a=0", NULL, 0), TURBODB_STATUS_OK);
    lookup_row(2); lookup_end();
    check_equal(lookup_open("SELECT DISTINCT a FROM items WHERE a=0", NULL, 0), TURBODB_STATUS_OK); lookup_row(0); lookup_end();
  }
  it("enumerates SHOW index snapshots without further native reads or allocations") {
    seed(); build_many();
    const char *sql[]={"SHOW COLUMNS FROM items","SHOW INDEX FROM items","SHOW CREATE TABLE items"};
    const size_t expected[]={COLUMNS,6,1};
    for(size_t op=0;op<sizeof(sql)/sizeof(sql[0]);++op) {
      check_equal(lookup_open(sql[op],NULL,0),TURBODB_STATUS_OK);
      uint64_t id=OUTPUT_SENTINEL;
      check_equal(drop_on(&owner,"DROP INDEX ix ON items",&id),TURBODB_STATUS_BUSY);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_BUSY);
      faults_clear(); fail_get=fail_new=fail_reserve=fail_resize=1; fail_iterator=FAIL_VALUE;
      const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_BYTES];
      for(size_t i=0;i<expected[op];++i) {
        orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&lookup_query,&row,&error),TURBODB_STATUS_OK);
        check_equal(row.state,ORM_SQL_SCAN_ROW);
        check_equal(row.count,op==2?2:op?ORM_SQL_SHOW_INDEX_COLUMNS:ORM_SQL_SHOW_COLUMNS);
      }
      lookup_end(); check_equal(gets,0u); check_equal(news,0u); check_equal(reserves,0u); check_equal(resizes,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_BYTES],reads);
      check_equal(put_calls,0u); check_equal(delete_calls,0u); check_equal(owner.active_sources,0u); faults_clear();
    }
  }
  it("parses complete SHOW CREATE output and enforces formatting work and step boundaries") {
    seed(); build_many(); next_statement();
    check_equal(lookup_open("SHOW CREATE TABLE items",NULL,0),TURBODB_STATUS_OK);
    const uint64_t peak=budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES];
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&lookup_query,&row,&error),TURBODB_STATUS_OK);
    const vstr ddl=row.values[1].data.text_value; check_true(ddl.len>0);
    sqlparser_document *doc=NULL; sqlparser_error diagnostic;
    check_equal(sqlparser_parse(ddl.data,ddl.len,NULL,&doc,&diagnostic),SQLPARSER_OK);
    const sqlparser_node *root=sqlparser_get_node(doc,sqlparser_statements(doc).first);
    check_equal(root->kind,SQLPARSER_CREATE_TABLE); check_equal(root->as.create_table.elements.count,7u);
    sqlparser_document_destroy(doc);
    orm_sql_schema_column column={0};
    check_equal(orm_tidesdb_sql_runtime_column(&lookup_query,1,&column,&error),TURBODB_STATUS_OK);
    check_equal(column.type.kind,TURBODB_VALUE_TEXT); check_false(column.type.nullable);
    check_equal(column.name.len,strlen("Create Table")); check_equal(memcmp(column.name.data,"Create Table",column.name.len),0);
    const uint64_t text_bytes=ddl.len; lookup_end();
    const orm_sql_budget_resource resources[]={ORM_SQL_BUDGET_WORK_BYTES,ORM_SQL_BUDGET_EXECUTION_STEPS};
    const uint64_t required[]={peak,steps};
    for(size_t i=0;i<sizeof(resources)/sizeof(resources[0]);++i) {
      const uint64_t original=budget.limits.statement.value[resources[i]];
      next_statement(); budget.limits.statement.value[resources[i]]=required[i]-1;
      check_equal(lookup_open("SHOW CREATE TABLE items",NULL,0),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_null(lookup_query.owner); check_false(owner.failed); check_equal(owner.active_sources,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],budget.retained_work_bytes);
      next_statement(); budget.limits.statement.value[resources[i]]=required[i];
      check_equal(lookup_open("SHOW CREATE TABLE items",NULL,0),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_runtime_close(&lookup_query,&error),TURBODB_STATUS_OK);
      budget.limits.statement.value[resources[i]]=original;
    }
    next_statement(); const uint64_t limit=budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=steps-text_bytes;
    check_equal(lookup_open("SHOW CREATE TABLE items",NULL,0),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(lookup_query.owner); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],budget.retained_work_bytes);
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=limit;
  }
  it("unwinds every indexed SHOW snapshot allocation failure") {
    seed(); build_many(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *sql[]={"SHOW COLUMNS FROM items","SHOW INDEX FROM items","SHOW CREATE TABLE items"};
    for(size_t op=0;op<sizeof(sql)/sizeof(sql[0]);++op) {
      next_statement(); faults_clear(); check_equal(lookup_open(sql[op],NULL,0),TURBODB_STATUS_OK);
      const size_t allocations[]={reserves,resizes};
      check_equal(orm_tidesdb_sql_runtime_close(&lookup_query,&error),TURBODB_STATUS_OK);
      for(size_t phase=0;phase<2;++phase) for(size_t point=1;point<=allocations[phase];++point) {
        next_statement(); faults_clear(); if(phase) fail_resize=point; else fail_reserve=point;
        check_equal(lookup_open(sql[op],NULL,0),TURBODB_STATUS_OUT_OF_MEMORY);
        check_null(lookup_query.owner); check_equal(owner.active_sources,0u); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_equal(put_calls,0u); check_equal(delete_calls,0u);
      }
      faults_clear();
    }
  }
  it("fails closed and refunds indexed SHOW snapshots on every metadata read fault") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK); begin_owner(&owner);
    const char *sql[]={"SHOW COLUMNS FROM items","SHOW INDEX FROM items","SHOW CREATE TABLE items"};
    for(size_t op=0;op<sizeof(sql)/sizeof(sql[0]);++op) {
      next_statement(); faults_clear(); check_equal(lookup_open(sql[op],NULL,0),TURBODB_STATUS_OK);
      const size_t counts[]={gets,news,FAIL_VALUE};
      check_equal(orm_tidesdb_sql_runtime_close(&lookup_query,&error),TURBODB_STATUS_OK);
      for(size_t phase=0;phase<sizeof(counts)/sizeof(counts[0]);++phase) for(size_t point=1;point<=counts[phase];++point) {
        next_statement(); faults_clear();
        if(phase==2) fail_iterator=(int)point; else if(phase) fail_new=point; else fail_get=point;
        check_equal(lookup_open(sql[op],NULL,0),TURBODB_STATUS_DATASTORE_ERROR); check_true(owner.failed);
        check_null(lookup_query.owner); check_equal(owner.active_sources,0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],budget.retained_work_bytes);
        check_equal(put_calls,0u); check_equal(delete_calls,0u); faults_clear();
        check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK); begin_owner(&owner);
      }
    }
    verify_live_indexes(ROWS,3);
  }
  it("bounds indexed SHOW construction and locks pull budget failures until close") {
    seed(); build_many(); const char *sql[]={"SHOW COLUMNS FROM items","SHOW INDEX FROM items","SHOW CREATE TABLE items"};
    const orm_sql_budget_resource resources[]={ORM_SQL_BUDGET_WORK_BYTES,ORM_SQL_BUDGET_PLAN_NODES,
      ORM_SQL_BUDGET_READ_ROWS,ORM_SQL_BUDGET_READ_BYTES,ORM_SQL_BUDGET_EXECUTION_STEPS};
    for(size_t op=0;op<sizeof(sql)/sizeof(sql[0]);++op) {
      for(size_t i=0;i<sizeof(resources)/sizeof(resources[0]);++i) {
        next_statement(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
        const uint64_t limit=budget.limits.statement.value[resources[i]];
        budget.limits.statement.value[resources[i]]=budget.used.value[resources[i]];
        check_equal(lookup_open(sql[op],NULL,0),TURBODB_STATUS_LIMIT_EXCEEDED);
        budget.limits.statement.value[resources[i]]=limit;
        check_null(lookup_query.owner); check_equal(owner.active_sources,0u); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      next_statement(); check_equal(lookup_open(sql[op],NULL,0),TURBODB_STATUS_OK);
      const uint64_t limit=budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      orm_sql_scan_row row={.count=OUTPUT_SENTINEL};
      check_equal(orm_tidesdb_sql_runtime_next(&lookup_query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=limit;
      check_equal(orm_tidesdb_sql_runtime_next(&lookup_query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(row.count,OUTPUT_SENTINEL); check_false(owner.failed);
      check_equal(orm_tidesdb_sql_runtime_close(&lookup_query,&error),TURBODB_STATUS_OK); check_equal(owner.active_sources,0u);
    }
  }
  it("rejects corrupt unknown and base-format index directories in all table SHOW forms") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
    const char *sql[]={"SHOW COLUMNS FROM items","SHOW INDEX FROM items","SHOW CREATE TABLE items"};
    const uint8_t key[]={INDEX_DIRECTORY_NS,1,0,0,0,0,0,0,0,2,'i','x'};
    enum { WIRE_VERSION_OFFSET=2, WIRE_TABLE_ID_OFFSET=4 };
    for(size_t op=0;op<sizeof(sql)/sizeof(sql[0]);++op) for(size_t variant=0;variant<3;++variant) {
      begin_owner(&owner); uint8_t *data=NULL; size_t size=0;
      const uint8_t *target=variant==2?store_manifest_key:key;
      const size_t key_size=variant==2?sizeof(store_manifest_key):sizeof(key);
      check_equal(orm_tidesdb_txn_get(owner.transaction,family,target,key_size,&data,&size),ORM_TDB_SUCCESS);
      if(variant==2) data[STORE_FORMAT]=ORM_SQL_STORE_FORMAT_BASE;
      else data[variant?WIRE_VERSION_OFFSET:WIRE_TABLE_ID_OFFSET]=UINT8_MAX;
      check_equal(orm_tidesdb_txn_put(owner.transaction,family,target,key_size,data,size,0),ORM_TDB_SUCCESS); orm_tidesdb_free(data);
      faults_clear(); check_equal(lookup_open(sql[op],NULL,0),variant==1?TURBODB_STATUS_UNSUPPORTED:TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(owner.failed,variant!=1); check_null(lookup_query.owner); check_equal(owner.active_sources,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],budget.retained_work_bytes);
      check_equal(put_calls,0u); check_equal(delete_calls,0u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK);
    }
    begin_owner(&owner); verify_live_indexes(ROWS,3);
  }
  it("unwinds every lookup planning allocation failure without publishing a partial query") {
    seed(); build_many(); faults_clear();
    check_equal(lookup_open("SELECT id FROM items WHERE a IN (0,-9223372036854775808) OR a=9", NULL, 0), TURBODB_STATUS_OK);
    const size_t reserve_count = reserves, resize_count = resizes;
    check_equal(orm_tidesdb_sql_runtime_close(&lookup_query, &error), TURBODB_STATUS_OK);
    const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for (size_t phase = 0; phase < 2; ++phase) {
      const size_t count = phase ? resize_count : reserve_count;
      for (size_t point = 1; point <= count; ++point) {
        faults_clear(); if (phase) fail_resize = point; else fail_reserve = point;
        check_equal(lookup_open("SELECT id FROM items WHERE a IN (0,-9223372036854775808) OR a=9", NULL, 0), TURBODB_STATUS_OUT_OF_MEMORY);
        check_null(lookup_query.owner); check_equal(owner.active_sources, 0u); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
      }
    }
  }
  it("locks lookup native read failures and preserves output without retry") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); begin_owner(&owner);
    for (size_t phase = 0; phase < 3; ++phase) {
      const size_t count = phase == 2 ? FAIL_VALUE : phase ? 2 : 1;
      for (size_t point = 1; point <= count; ++point) {
        check_equal(lookup_open("SELECT id FROM items WHERE a=0 AND b=7", NULL, 0), TURBODB_STATUS_OK); faults_clear();
        if (phase == 2) fail_iterator = (int)point; else if (phase) fail_get = point; else fail_new = point;
        orm_sql_scan_row row = {.count=OUTPUT_SENTINEL};
        /* NEXT faults happen on the second pull, after the only matching row. */
        if (phase == 2 && point == FAIL_NEXT) lookup_row(-1);
        check_equal(orm_tidesdb_sql_runtime_next(&lookup_query, &row, &error), TURBODB_STATUS_DATASTORE_ERROR);
        check_equal(row.count, OUTPUT_SENTINEL); const size_t reads = gets, iterators = news;
        check_equal(orm_tidesdb_sql_runtime_next(&lookup_query, &row, &error), TURBODB_STATUS_DATASTORE_ERROR);
        check_equal(gets, reads); check_equal(news, iterators); check_true(owner.failed);
        faults_clear(); check_equal(orm_tidesdb_sql_runtime_close(&lookup_query, &error), TURBODB_STATUS_OK);
        check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); begin_owner(&owner);
      }
    }
    verify_live_indexes(ROWS, 3);
  }
  it("explains the chosen index without reading entries or evaluating projections") {
    seed(); build_many();
    check_equal(lookup_open("EXPLAIN SELECT id+9223372036854775807 AS n FROM items WHERE a=0 AND b=7", NULL, 0), TURBODB_STATUS_OK);
    check_equal(strcmp(lookup_query.as.select.explain.source.index, "ix"), 0);
    check_equal(strcmp(lookup_query.as.select.explain.source.key_length, "18"), 0);
    check_null(lookup_query.as.select.source.iterator); const uint64_t reads = budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    faults_clear(); fail_new = fail_get = 1; orm_sql_scan_row row = {0};
    check_equal(orm_tidesdb_sql_runtime_next(&lookup_query, &row, &error), TURBODB_STATUS_OK); check_equal(row.state, ORM_SQL_SCAN_ROW);
    check_equal(row.values[9].kind, TURBODB_VALUE_NULL); check_equal(row.values[10].kind, TURBODB_VALUE_NULL); lookup_end();
    check_equal(gets, 0u); check_equal(news, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], reads); faults_clear();
  }
  it("fails closed on touched orphan rows, wrong tuples and invalid unique owners") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    for (size_t variant = 0; variant < 4; ++variant) {
      begin_owner(&owner);
      check_equal(lookup_open("SELECT id FROM items WHERE a=0 AND b=7", NULL, 0), TURBODB_STATUS_OK);
      uint8_t key[DATA_KEY_BYTES], data_key[ORM_SQL_RELATION_KEY_BYTES], primary[ORM_SQL_WIRE_U64];
      orm_sql_wire_order_write(primary, orm_sql_wire_signed_order(-1));
      memcpy(key, vec_data_const(&lookup_query.as.select.source.lookup.key), DATA_KEY_BYTES - ORM_SQL_WIRE_U64);
      memcpy(key + DATA_KEY_BYTES - ORM_SQL_WIRE_U64, primary, sizeof(primary));
      memcpy(data_key, lookup_query.as.select.source.prefix, ORM_SQL_RELATION_PREFIX_BYTES);
      memcpy(data_key + ORM_SQL_RELATION_PREFIX_BYTES, primary, sizeof(primary));
      if (!variant) {
        uint8_t wrong[ORM_SQL_WIRE_U64] = {0};
        check_equal(orm_tidesdb_txn_put(owner.transaction, family, key, sizeof(key), wrong, sizeof(wrong), 0), ORM_TDB_SUCCESS);
      } else if (variant == 1) check_equal(orm_tidesdb_txn_delete(owner.transaction, family, data_key, sizeof(data_key)), ORM_TDB_SUCCESS);
      else if (variant == 2) {
        uint8_t *data = NULL; size_t size = 0;
        check_equal(orm_tidesdb_txn_get(owner.transaction, family, data_key, sizeof(data_key), &data, &size), ORM_TDB_SUCCESS);
        data[REL_HEADER + REL_CELL + 1] ^= 1;
        check_equal(orm_tidesdb_txn_put(owner.transaction, family, data_key, sizeof(data_key), data, size, 0), ORM_TDB_SUCCESS); orm_tidesdb_free(data);
      } else {
        key[0] = INDEX_UNIQUE_NS;
        check_equal(orm_tidesdb_txn_delete(owner.transaction, family, key, sizeof(key)-ORM_SQL_WIRE_U64), ORM_TDB_SUCCESS);
      }
      faults_clear(); orm_sql_scan_row row = {.count=OUTPUT_SENTINEL};
      check_equal(orm_tidesdb_sql_runtime_next(&lookup_query, &row, &error), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(row.count, OUTPUT_SENTINEL); check_true(owner.failed); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
      check_equal(orm_tidesdb_sql_runtime_close(&lookup_query, &error), TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    }
    begin_owner(&owner); verify_live_indexes(ROWS, 3);
  }
  it("bounds lookup reads exactly and detects concurrent row changes through TableVersion") {
    seed(); build_many(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); begin_owner(&owner);
    check_equal(lookup_open("SELECT id FROM items WHERE a=0 AND b=7", NULL, 0), TURBODB_STATUS_OK);
    const uint64_t limit = budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS];
    budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS] = budget.used.value[ORM_SQL_BUDGET_READ_ROWS] + 2;
    orm_sql_scan_row row = {.count=OUTPUT_SENTINEL};
    check_equal(orm_tidesdb_sql_runtime_next(&lookup_query, &row, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(row.count, OUTPUT_SENTINEL); check_false(owner.failed); budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS] = limit;
    check_equal(orm_tidesdb_sql_runtime_close(&lookup_query, &error), TURBODB_STATUS_OK);
    check_equal(lookup_open("SELECT id FROM items WHERE a=0 AND b=7", NULL, 0), TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS] = budget.used.value[ORM_SQL_BUDGET_READ_ROWS] + 3;
    lookup_row(-1); lookup_end(); budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS] = limit;
    begin_owner(&other);
    check_equal(lookup_open("SELECT id FROM items WHERE a=0 AND b=7", NULL, 0), TURBODB_STATUS_OK);
    const turbodb_value_t changed[] = {turbodb_i64(-1),turbodb_i64(2),turbodb_u64(7)};
    check_equal(orm_tidesdb_sql_relation_update_rows(&other, vstr_from_cstr(table_name), changed, 1, COLUMNS, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_OK);
    lookup_row(-1); lookup_end();
    /* A write makes the reader's transaction non-read-only, so its stale table
     * snapshot must fail SERIALIZABLE commit even though its index range is small. */
    const turbodb_value_t added[] = {turbodb_i64(10),turbodb_i64(3),turbodb_u64(9)};
    check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr(table_name), added, COLUMNS, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_BUSY);
    begin_owner(&owner); verify_live_indexes(ROWS, 3);
  }
  it("uses equality access inside UNION branches, derived CTEs and IN dependencies") {
    seed(); build_many();
    check_equal(lookup_open("SELECT id FROM items WHERE a=0 AND b=7 UNION ALL SELECT id FROM items WHERE a=0 AND b=8", NULL, 0), TURBODB_STATUS_OK);
    lookup_row(-1); lookup_row(INT64_MAX); lookup_end();
    check_equal(lookup_open("WITH c AS (SELECT id FROM items WHERE a=0 AND b=7) SELECT id FROM c", NULL, 0), TURBODB_STATUS_OK);
    lookup_row(-1); lookup_end();
    check_equal(lookup_open("SELECT id FROM items WHERE id IN (SELECT id FROM items WHERE a=0 AND b=7)", NULL, 0), TURBODB_STATUS_OK);
    lookup_row(-1); lookup_end();
  }
  bench("compares the same selective SELECT before and after indexing 512 committed rows") {
    enum { BENCH_ROWS = 512, BENCH_SAMPLES = 16, BENCH_MATCH = BENCH_ROWS / 2 };
    /* Samples share a native snapshot, so transaction read charges intentionally
     * accumulate even though each sample starts a fresh statement budget. */
    budget.limits.transaction.read_bytes = (uint64_t)LIMIT * BENCH_SAMPLES;
    vec_t input = {0}; size_t bytes = 0;
    check_equal(orm_sql_work_zero(&input, BENCH_ROWS * COLUMNS, sizeof(turbodb_value_t), _Alignof(turbodb_value_t), &budget, &bytes, &error), TURBODB_STATUS_OK);
    for (size_t i = 0; i < BENCH_ROWS; ++i) {
      *(turbodb_value_t *)vec_at(&input, i*COLUMNS) = turbodb_i64((int64_t)i);
      *(turbodb_value_t *)vec_at(&input, i*COLUMNS+1) = turbodb_i64((int64_t)i);
      *(turbodb_value_t *)vec_at(&input, i*COLUMNS+2) = turbodb_u64((uint64_t)i);
    }
    check_equal(orm_tidesdb_sql_relation_insert_rows(&owner, vstr_from_cstr(table_name), vec_data_const(&input), BENCH_ROWS, COLUMNS, &error), TURBODB_STATUS_OK);
    check_equal(orm_sql_work_release(&input, bytes, &budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); begin_owner(&owner);
    const char sql[] = "SELECT id FROM items WHERE a=256 AND b=256";
    const char range_sql[] = "SELECT id FROM items WHERE a>=256 AND a<257";
    const char multi_sql[] = "SELECT id FROM items WHERE a IN (256,256,NULL) OR (a=256 AND b=256)";
    sqlparser_document *doc = NULL, *range_doc = NULL, *multi_doc = NULL; sqlparser_error diagnostic;
    check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &diagnostic), SQLPARSER_OK);
    check_equal(sqlparser_parse(range_sql, strlen(range_sql), NULL, &range_doc, &diagnostic), SQLPARSER_OK);
    check_equal(sqlparser_parse(multi_sql, strlen(multi_sql), NULL, &multi_doc, &diagnostic), SQLPARSER_OK);
    check_equal(lookup_measure(doc, BENCH_MATCH), TURBODB_STATUS_OK); size_t failures = 0;
    benchmark_ops("512-row Data scan: bind/read/close", BENCH_SAMPLES, 1) {
      if (lookup_benchmark_sample(doc, BENCH_MATCH) != TURBODB_STATUS_OK) ++failures;
    }
    check_equal(lookup_measure(range_doc, BENCH_MATCH), TURBODB_STATUS_OK);
    benchmark_ops("512-row range Data scan: bind/read/close", BENCH_SAMPLES, 1) {
      if (lookup_benchmark_sample(range_doc, BENCH_MATCH) != TURBODB_STATUS_OK) ++failures;
    }
    check_equal(lookup_measure(multi_doc, BENCH_MATCH), TURBODB_STATUS_OK);
    benchmark_ops("512-row IN/OR Data scan: bind/read/close", BENCH_SAMPLES, 1) {
      if (lookup_benchmark_sample(multi_doc, BENCH_MATCH) != TURBODB_STATUS_OK) ++failures;
    }
    check_equal(failures, 0u); uint64_t id = 0; check_equal(build_on(&owner, ordinary_sql, &id), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); begin_owner(&owner);
    check_equal(lookup_measure(doc, BENCH_MATCH), TURBODB_STATUS_OK);
    benchmark_ops("512-row equality index: bind/read/close", BENCH_SAMPLES, 1) {
      if (lookup_benchmark_sample(doc, BENCH_MATCH) != TURBODB_STATUS_OK) ++failures;
    }
    check_equal(lookup_measure(range_doc, BENCH_MATCH), TURBODB_STATUS_OK);
    benchmark_ops("512-row range index: bind/read/close", BENCH_SAMPLES, 1) {
      if (lookup_benchmark_sample(range_doc, BENCH_MATCH) != TURBODB_STATUS_OK) ++failures;
    }
    check_equal(lookup_measure(multi_doc, BENCH_MATCH), TURBODB_STATUS_OK);
    benchmark_ops("512-row IN/OR index: bind/read/close", BENCH_SAMPLES, 1) {
      if (lookup_benchmark_sample(multi_doc, BENCH_MATCH) != TURBODB_STATUS_OK) ++failures;
    }
    check_equal(failures, 0u); sqlparser_document_destroy(doc); sqlparser_document_destroy(range_doc); sqlparser_document_destroy(multi_doc);
  }
  it("rejects CREATE INDEX parameters without changing affected rows or storage") {
    seed(); const turbodb_value_t parameter = turbodb_i64(1); size_t affected = OUTPUT_SENTINEL; faults_clear();
    check_equal(execute_params(unique_sql, &parameter, 1, &affected), TURBODB_STATUS_SQL_ERROR);
    check_equal(affected, OUTPUT_SENTINEL); check_equal(put_calls, 0u); no_index(2);
    check_equal(execute_sql(unique_sql, &affected), TURBODB_STATUS_OK); check_equal(affected, 0u); verify_live_indexes(ROWS, 1);
  }
}
