#include "catalog_store.h"
#include "select.h"
#include "show.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>

static size_t put_calls, get_calls, reserves, resizes, fail_put, fail_get, fail_reserve, fail_resize;
static bool fail_savepoint, fail_release, fail_rollback_to, fail_commit_after, fail_rollback_after;
typedef enum cursor_fault { CURSOR_OK, CURSOR_NEW, CURSOR_SEEK, CURSOR_NEXT, CURSOR_KEY } cursor_fault;
static cursor_fault fail_cursor;
static size_t cursor_calls;
static int probe_iter_new(orm_tidesdb_transaction_t *tx, orm_tidesdb_column_family_t *cf, orm_tidesdb_iterator_t **out) {
  ++cursor_calls; return fail_cursor == CURSOR_NEW ? ORM_TDB_ERR_IO : orm_tidesdb_iter_new(tx, cf, out);
}
static int probe_iter_seek(orm_tidesdb_iterator_t *iterator, const uint8_t *key, size_t size) {
  ++cursor_calls; return fail_cursor == CURSOR_SEEK ? ORM_TDB_ERR_IO : orm_tidesdb_iter_seek(iterator, key, size);
}
static int probe_iter_next(orm_tidesdb_iterator_t *iterator) {
  ++cursor_calls; return fail_cursor == CURSOR_NEXT ? ORM_TDB_ERR_IO : orm_tidesdb_iter_next(iterator);
}
static int probe_iter_key(orm_tidesdb_iterator_t *iterator, uint8_t **key, size_t *size) {
  ++cursor_calls; return fail_cursor == CURSOR_KEY ? ORM_TDB_ERR_IO : orm_tidesdb_iter_key(iterator, key, size);
}
static int probe_put(orm_tidesdb_transaction_t *tx, orm_tidesdb_column_family_t *cf,
    const uint8_t *key, size_t n, const uint8_t *data, size_t size, time_t ttl) {
  return ++put_calls == fail_put ? ORM_TDB_ERR_IO : orm_tidesdb_txn_put(tx, cf, key, n, data, size, ttl);
}
static int probe_get(orm_tidesdb_transaction_t *tx, orm_tidesdb_column_family_t *cf,
    const uint8_t *key, size_t n, uint8_t **data, size_t *size) {
  return ++get_calls == fail_get ? ORM_TDB_ERR_IO : orm_tidesdb_txn_get(tx, cf, key, n, data, size);
}
static int probe_savepoint(orm_tidesdb_transaction_t *tx, const char *name) {
  return fail_savepoint ? ORM_TDB_ERR_MEMORY : orm_tidesdb_txn_savepoint(tx, name);
}
static int probe_release(orm_tidesdb_transaction_t *tx, const char *name) {
  return fail_release ? ORM_TDB_ERR_IO : orm_tidesdb_txn_release_savepoint(tx, name);
}
static int probe_rollback_to(orm_tidesdb_transaction_t *tx, const char *name) {
  return fail_rollback_to ? ORM_TDB_ERR_IO : orm_tidesdb_txn_rollback_to_savepoint(tx, name);
}
static int probe_commit(orm_tidesdb_transaction_t *tx) {
  const int code = orm_tidesdb_txn_commit(tx);
  return code == ORM_TDB_SUCCESS && fail_commit_after ? ORM_TDB_ERR_IO : code;
}
static int probe_owner_rollback(orm_tidesdb_transaction_t *tx) {
  const int code = orm_tidesdb_txn_rollback(tx);
  return code == ORM_TDB_SUCCESS && fail_rollback_after ? ORM_TDB_ERR_IO : code;
}
static stl_status probe_reserve(vec_t *v, size_t n) { return ++reserves == fail_reserve ? STL_OUT_OF_MEMORY : vec_reserve(v, n); }
static stl_status probe_resize(vec_t *v, size_t n) { return ++resizes == fail_resize ? STL_OUT_OF_MEMORY : vec_resize(v, n); }
#define orm_tidesdb_txn_put probe_put
#define orm_tidesdb_txn_get probe_get
#define orm_tidesdb_txn_savepoint probe_savepoint
#define orm_tidesdb_txn_release_savepoint probe_release
#define orm_tidesdb_txn_rollback_to_savepoint probe_rollback_to
#define orm_tidesdb_txn_commit probe_commit
#define orm_tidesdb_txn_rollback probe_owner_rollback
#define vec_reserve probe_reserve
#define vec_resize probe_resize
#define orm_tidesdb_iter_new probe_iter_new
#define orm_tidesdb_iter_seek probe_iter_seek
#define orm_tidesdb_iter_next probe_iter_next
#define orm_tidesdb_iter_key probe_iter_key
#include "../../src/work.c"
#include "../../src/catalog.c"
#include "../../src/catalog_store.c"
#include "../../src/show.c"
#undef orm_tidesdb_iter_new
#undef orm_tidesdb_iter_seek
#undef orm_tidesdb_iter_next
#undef orm_tidesdb_iter_key
#undef orm_tidesdb_txn_put
#undef orm_tidesdb_txn_get
#undef orm_tidesdb_txn_savepoint
#undef orm_tidesdb_txn_release_savepoint
#undef orm_tidesdb_txn_rollback_to_savepoint
#undef orm_tidesdb_txn_commit
#undef orm_tidesdb_txn_rollback
#undef vec_reserve
#undef vec_resize

enum { MAX_RECORD = 4096, WORK = 4 * 1024 * 1024, LIMIT = 1000000, DEPTH = 32 };
static const char family_name[] = "sql-catalog-store";
static const char ddl[] = "CREATE TABLE items (id BIGINT PRIMARY KEY, score BIGINT, weight DOUBLE NOT NULL)";
static char *directory;
static orm_tidesdb_database_t *database;
static orm_tidesdb_column_family_t *family;
static orm_tidesdb_sql_budget budget;
static orm_sql_catalog_store owner, other;
static orm_sql_table_definition definition, loaded;
static turbodb_error_t error;
static orm_sql_show_source show_source;
static orm_sql_scan show_scan;

static void faults_clear(void) {
  put_calls = get_calls = reserves = resizes = fail_put = fail_get = fail_reserve = fail_resize = 0;
  fail_savepoint = fail_release = fail_rollback_to = fail_commit_after = fail_rollback_after = false;
  fail_cursor = CURSOR_OK; cursor_calls = 0;
}
static void open_database(void) {
  orm_tidesdb_config_t config = orm_tidesdb_default_config(); config.db_path = directory;
  check_equal(orm_tidesdb_open(&config, &database), ORM_TDB_SUCCESS);
}
static void reopen(void) {
  check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_catalog_finish(&other, false, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_close(database), ORM_TDB_SUCCESS); database = NULL;
  open_database(); family = orm_tidesdb_get_column_family(database, family_name); check_not_null(family);
}
static void initialize(void) {
  check_equal(orm_tidesdb_sql_catalog_initialize(database, family, MAX_RECORD, &budget, &error), TURBODB_STATUS_OK);
}
static void begin_owner(orm_sql_catalog_store *store) {
  check_equal(orm_tidesdb_sql_catalog_begin(database, family, MAX_RECORD, &budget, store, &error), TURBODB_STATUS_OK);
}
static void bind_ddl(const char *sql) {
  check_equal(orm_tidesdb_sql_catalog_destroy(&definition, &error), TURBODB_STATUS_OK);
  sqlparser_document *doc = NULL; sqlparser_error parse_error;
  check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &parse_error), SQLPARSER_OK);
  check_equal(orm_tidesdb_sql_catalog_bind_create(doc, &budget, &definition, &error), TURBODB_STATUS_OK);
  sqlparser_document_destroy(doc);
}
static uint64_t create(orm_sql_catalog_store *store) {
  uint64_t id = 0; bool created = false;
  check_equal(orm_tidesdb_sql_catalog_create(store, &definition, &id, &created, &error), TURBODB_STATUS_OK);
  check_true(created); return id;
}
static bool lookup(orm_sql_catalog_store *store, const char *name, uint64_t expected_id) {
  check_equal(orm_tidesdb_sql_catalog_destroy(&loaded, &error), TURBODB_STATUS_OK);
  uint64_t id = 0, version = 0; bool found = false;
  check_equal(orm_tidesdb_sql_catalog_lookup(store, vstr_from_cstr(name), &loaded, &id, &version, &found, &error), TURBODB_STATUS_OK);
  check_equal(found, expected_id != 0);
  if (found) { check_equal(id, expected_id); check_equal(version, 1u); }
  return found;
}
static void raw_put(const uint8_t *key, size_t key_size, const uint8_t *data, size_t size) {
  orm_tidesdb_transaction_t *tx = NULL;
  check_equal(orm_tidesdb_txn_begin_with_isolation(database, ORM_TDB_ISOLATION_SERIALIZABLE, &tx), ORM_TDB_SUCCESS);
  check_equal(orm_tidesdb_txn_put(tx, family, key, key_size, data, size, 0), ORM_TDB_SUCCESS);
  check_equal(orm_tidesdb_txn_commit(tx), ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx);
}
static void raw_value(const uint8_t *key, size_t key_size, const uint8_t *expected, size_t expected_size) {
  orm_tidesdb_transaction_t *tx = NULL; uint8_t *data = NULL; size_t size = 0;
  check_equal(orm_tidesdb_txn_begin_with_isolation(database, ORM_TDB_ISOLATION_SERIALIZABLE, &tx), ORM_TDB_SUCCESS);
  check_equal(orm_tidesdb_txn_get(tx, family, key, key_size, &data, &size), ORM_TDB_SUCCESS);
  check_equal(size, expected_size); check_equal(memcmp(data, expected, size), 0);
  orm_tidesdb_free(data); check_equal(orm_tidesdb_txn_rollback(tx), ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx);
}
static turbodb_status_t open_show_on(orm_sql_catalog_store *store, const char *sql) {
  info("SHOW input: %s", sql);
  sqlparser_document *doc = NULL; sqlparser_error parse_error;
  check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &parse_error), SQLPARSER_OK);
  const turbodb_status_t status = orm_tidesdb_sql_show_open(doc, store, vstr_from_cstr("local"), &show_source, &error);
  sqlparser_document_destroy(doc); return status;
}
static void close_show(void) {
  check_equal(orm_tidesdb_sql_scan_close(&show_scan, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_show_close(&show_source, &error), TURBODB_STATUS_OK);
}
static const turbodb_value_t *show_row(void) {
  const turbodb_value_t *row = NULL;
  check_equal(show_source.source.next(show_source.source.context, &row, &error), TURBODB_STATUS_OK);
  return row;
}
static void text_is(const turbodb_value_t *value, const char *expected) {
  check_equal(value->kind, TURBODB_VALUE_TEXT);
  check_equal(value->data.text_value.len, strlen(expected));
  check_equal(memcmp(value->data.text_value.data, expected, strlen(expected)), 0);
}
spec("TidesDB persistent private Catalog") {
  before_each() {
    faults_clear(); tdsql_error_init(&error);
    owner = other = (orm_sql_catalog_store){0}; definition = loaded = (orm_sql_table_definition){0};
    show_source = (orm_sql_show_source){0}; show_scan = (orm_sql_scan){0};
    orm_sql_budget_limits limits = {0};
    for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) limits.statement.value[i] = LIMIT;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = WORK;
    limits.transaction = (orm_sql_transaction_budget_amount){LIMIT, LIMIT, LIMIT};
    check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    directory = tt_make_temp_dir("orm-sql-catalog"); check_not_null(directory); open_database();
    orm_tidesdb_column_family_config_t config = orm_tidesdb_default_column_family_config(); config.sync_mode = ORM_TDB_SYNC_FULL;
    check_equal(orm_tidesdb_create_column_family(database, family_name, &config), ORM_TDB_SUCCESS);
    family = orm_tidesdb_get_column_family(database, family_name); check_not_null(family);
  }
  after_each() {
    faults_clear();
    close_show();
    check_equal(orm_tidesdb_sql_catalog_destroy(&loaded, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_destroy(&definition, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, false, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    check_equal(budget.retained_work_bytes, 0u);
    if (budget.statement_active) check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_close(database), ORM_TDB_SUCCESS); database = NULL; family = NULL;
    check_equal(tt_remove_tree(directory), 0); free(directory); directory = NULL;
  }
  it("requires explicit initialization and persists the exact Manifest golden bytes") {
    check_equal(orm_tidesdb_sql_catalog_begin(database, family, MAX_RECORD, &budget, &owner, &error), TURBODB_STATUS_INVALID_STATE);
    check_null(owner.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    initialize(); const uint8_t key[] = {0,'T','D','B','M'};
    const uint8_t manifest[] = {'T','D','B','R',1,1,0,0, 0,0,0,0,0,0,0,0, 1,0,0,0,0,0,0,0};
    raw_value(key, sizeof(key), manifest, sizeof(manifest)); reopen(); begin_owner(&owner);
    check_false(lookup(&owner, "absent", 0));
    check_equal(orm_tidesdb_sql_catalog_initialize(database, family, MAX_RECORD, &budget, &error), TURBODB_STATUS_INVALID_STATE);
  }
  it("refuses a nonempty legacy CF without modifying it") {
    const uint8_t key[] = "orm:items:1", data[] = "legacy-content";
    raw_put(key, sizeof(key) - 1, data, sizeof(data) - 1);
    check_equal(orm_tidesdb_sql_catalog_initialize(database, family, MAX_RECORD, &budget, &error), TURBODB_STATUS_INVALID_STATE);
    raw_value(key, sizeof(key) - 1, data, sizeof(data) - 1);
    check_equal(orm_tidesdb_sql_catalog_begin(database, family, MAX_RECORD, &budget, &owner, &error), TURBODB_STATUS_INVALID_STATE);
  }
  it("loads committed definitions after reopen and binds SELECT over an empty table") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); check_equal(create(&owner), 1u);
    check_true(lookup(&owner, "items", 1));
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    reopen(); begin_owner(&owner); check_true(lookup(&owner, "items", 1));
    orm_sql_table_schema schema = {0}; check_equal(orm_tidesdb_sql_catalog_schema(&loaded, &schema, &error), TURBODB_STATUS_OK);
    check_equal(schema.count, 3u); check_false(schema.columns[0].type.nullable);
    const char sql[] = "SELECT * FROM items"; sqlparser_document *doc = NULL; sqlparser_error parse_error;
    check_equal(sqlparser_parse(sql, sizeof(sql) - 1, NULL, &doc, &parse_error), SQLPARSER_OK);
    orm_sql_select plan = {0}; orm_sql_select_run run = {0};
    check_equal(orm_tidesdb_sql_select_bind(doc, &schema, DEPTH, &budget, &plan, &error), TURBODB_STATUS_OK);
    sqlparser_document_destroy(doc); check_equal(orm_tidesdb_sql_catalog_destroy(&loaded, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_open(&plan, NULL, 0, &run, &error), TURBODB_STATUS_OK);
    orm_sql_scan_row row = {0}; check_equal(orm_tidesdb_sql_scan_next(&run.scan, &row, &error), TURBODB_STATUS_OK);
    check_equal(row.state, ORM_SQL_SCAN_DONE);
    check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_destroy(&plan, &error), TURBODB_STATUS_OK);
  }
  it("rolls back all catalog records and ID allocation together") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); check_equal(create(&owner), 1u);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    reopen(); begin_owner(&owner); check_false(lookup(&owner, "items", 0)); check_equal(create(&owner), 1u);
    bind_ddl("CREATE TABLE second (id BIGINT UNSIGNED PRIMARY KEY)"); check_equal(create(&owner), 2u);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    reopen(); begin_owner(&owner); check_true(lookup(&owner, "items", 1)); check_true(lookup(&owner, "second", 2));
  }
  it("honors IF NOT EXISTS without changing the existing definition or consuming an ID") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); check_equal(create(&owner), 1u);
    uint64_t id = 99; bool created = true;
    check_equal(orm_tidesdb_sql_catalog_create(&owner, &definition, &id, &created, &error), TURBODB_STATUS_CONSTRAINT);
    check_equal(id, 99u); check_true(created);
    bind_ddl("CREATE TABLE IF NOT EXISTS items (different BIGINT PRIMARY KEY)");
    const uint64_t writes = budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS];
    check_equal(orm_tidesdb_sql_catalog_create(&owner, &definition, &id, &created, &error), TURBODB_STATUS_OK);
    check_equal(id, 1u); check_false(created); check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS], writes);
    check_true(lookup(&owner, "items", 1)); orm_sql_table_schema schema = {0};
    check_equal(orm_tidesdb_sql_catalog_schema(&loaded, &schema, &error), TURBODB_STATUS_OK); check_equal(schema.count, 3u);
    bind_ddl("CREATE TABLE second (id BIGINT PRIMARY KEY)"); check_equal(create(&owner), 2u);
  }
  it("keeps old snapshots and rejects competing catalog ID allocation") {
    initialize(); begin_owner(&owner); begin_owner(&other); bind_ddl(ddl);
    check_false(lookup(&other, "items", 0)); check_equal(create(&owner), 1u);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    check_false(lookup(&other, "items", 0));
    bind_ddl("CREATE TABLE second (id BIGINT PRIMARY KEY)"); check_equal(create(&other), 1u);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_BUSY);
    reopen(); begin_owner(&owner); check_true(lookup(&owner, "items", 1)); check_false(lookup(&owner, "second", 0));
  }
  it("allows only one of two concurrent same-name creators to commit") {
    initialize(); begin_owner(&owner); begin_owner(&other); bind_ddl(ddl);
    check_equal(create(&owner), 1u); check_equal(create(&other), 1u);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_BUSY);
    reopen(); begin_owner(&owner); check_true(lookup(&owner, "items", 1));
  }
  it("rolls back every partial CREATE write while retaining earlier successful statements") {
    initialize(); begin_owner(&owner); bind_ddl("CREATE TABLE first (id BIGINT PRIMARY KEY)"); check_equal(create(&owner), 1u);
    bind_ddl(ddl);
    for (size_t point = 1; point <= STORE_CREATE_WRITES; ++point) {
      put_calls = 0; fail_put = point; uint64_t id = 99; bool created = false;
      const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(orm_tidesdb_sql_catalog_create(&owner, &definition, &id, &created, &error), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(id, 99u); check_false(created); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work); fail_put = 0;
      check_false(lookup(&owner, "items", 0)); check_true(lookup(&owner, "first", 1));
      check_equal(orm_tidesdb_sql_catalog_destroy(&loaded, &error), TURBODB_STATUS_OK);
    }
    check_equal(create(&owner), 2u); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    reopen(); begin_owner(&owner); check_true(lookup(&owner, "items", 2)); check_true(lookup(&owner, "first", 1));
  }
  it("poisons the owner when savepoint rollback or release fails") {
    initialize(); bind_ddl(ddl);
    for (unsigned pass = 0; pass < 2; ++pass) {
      begin_owner(&owner); put_calls = 0;
      if (pass) fail_release = true; else { fail_put = 2; fail_rollback_to = true; }
      uint64_t id = 99; bool created = false;
      check_equal(orm_tidesdb_sql_catalog_create(&owner, &definition, &id, &created, &error), TURBODB_STATUS_DATASTORE_ERROR);
      check_true(owner.failed); check_equal(id, 99u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_INVALID_STATE);
      faults_clear(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
      begin_owner(&owner); check_false(lookup(&owner, "items", 0));
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    }
  }
  it("bounds sequential SQL commands with an outer savepoint and poisons cleanup failures") {
    initialize(); begin_owner(&owner);
    fail_savepoint = true;
    check_equal(orm_sql_store_command_begin(&owner, &error), TURBODB_STATUS_OUT_OF_MEMORY);
    check_false(owner.failed); fail_savepoint = false;

    check_equal(orm_sql_store_command_begin(&owner, &error), TURBODB_STATUS_OK);
    check_equal(orm_sql_store_command_finish(&owner, TURBODB_STATUS_CONSTRAINT, &error),
        TURBODB_STATUS_CONSTRAINT);
    check_false(owner.failed);
    check_equal(orm_sql_store_command_begin(&owner, &error), TURBODB_STATUS_OK);
    fail_release = true;
    check_equal(orm_sql_store_command_finish(&owner, TURBODB_STATUS_OK, &error),
        TURBODB_STATUS_DATASTORE_ERROR);
    check_contains(error.message, "SQL command status"); check_true(owner.failed);
    faults_clear();
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);

    begin_owner(&owner);
    check_equal(orm_sql_store_command_begin(&owner, &error), TURBODB_STATUS_OK);
    tdsql_error_set(&error, TURBODB_STATUS_SQL_ERROR, "duplicate assignment failed");
    fail_rollback_to = true;
    check_equal(orm_sql_store_command_finish(&owner, TURBODB_STATUS_SQL_ERROR, &error),
        TURBODB_STATUS_DATASTORE_ERROR);
    check_contains(error.message, "duplicate assignment failed"); check_true(owner.failed);
    faults_clear();
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
  }
  it("reports uncertain commit after native commit and never retries the command") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); check_equal(create(&owner), 1u);
    const size_t written = put_calls; fail_commit_after = true;
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_COMMIT_UNKNOWN);
    check_null(owner.transaction); check_equal(put_calls, written); fail_commit_after = false;
    reopen(); begin_owner(&owner); check_true(lookup(&owner, "items", 1));
  }
  it("rejects corrupt versions and missing table stamps instead of treating them as absent tables") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); check_equal(create(&owner), 1u);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    uint8_t key[STORE_VERSION_KEY_BYTES]; orm_sql_store_version_key(1, key); const uint8_t bad = 0;
    raw_put(key, sizeof(key), &bad, sizeof(bad)); begin_owner(&owner);
    uint64_t id = 99, version = 99; bool found = false;
    check_equal(orm_tidesdb_sql_catalog_lookup(&owner, vstr_from_cstr("items"), &loaded, &id, &version, &found, &error), TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(id, 99u); check_false(found); check_null(loaded.budget); check_true(owner.failed);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    orm_tidesdb_transaction_t *tx = NULL;
    check_equal(orm_tidesdb_txn_begin_with_isolation(database, ORM_TDB_ISOLATION_SERIALIZABLE, &tx), ORM_TDB_SUCCESS);
    check_equal(orm_tidesdb_txn_delete(tx, family, key, sizeof(key)), ORM_TDB_SUCCESS);
    check_equal(orm_tidesdb_txn_commit(tx), ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx);
    begin_owner(&owner);
    check_equal(orm_tidesdb_sql_catalog_lookup(&owner, vstr_from_cstr("items"), &loaded, &id, &version, &found, &error), TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(id, 99u); check_false(found); check_null(loaded.budget);
  }
  it("rejects unknown Manifest formats and counter overflow without writes") {
    initialize(); uint8_t data[STORE_MANIFEST_BYTES]; store_manifest_encode((store_manifest){0,1, 1}, data);
    data[4] = ORM_SQL_STORE_FORMAT_REAL_INDEXED + 1; raw_put(store_manifest_key, sizeof(store_manifest_key), data, sizeof(data));
    check_equal(orm_tidesdb_sql_catalog_begin(database, family, MAX_RECORD, &budget, &owner, &error), TURBODB_STATUS_UNSUPPORTED);
    store_manifest_encode((store_manifest){UINT64_MAX - 1, UINT64_MAX, 1}, data);
    raw_put(store_manifest_key, sizeof(store_manifest_key), data, sizeof(data)); begin_owner(&owner); bind_ddl(ddl);
    uint64_t id = 0; bool created = false; put_calls = 0;
    check_equal(orm_tidesdb_sql_catalog_create(&owner, &definition, &id, &created, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(put_calls, 0u);
  }
  it("admits all writes before any native put and refunds failed allocations") {
    initialize(); begin_owner(&owner); bind_ddl(ddl);
    uint64_t id = 99; bool created = false; const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const uint64_t write_limit = budget.limits.statement.value[ORM_SQL_BUDGET_WRITE_ROWS];
    budget.limits.statement.value[ORM_SQL_BUDGET_WRITE_ROWS] = budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS] + 2;
    put_calls = 0; check_equal(orm_tidesdb_sql_catalog_create(&owner, &definition, &id, &created, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(put_calls, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
    budget.limits.statement.value[ORM_SQL_BUDGET_WRITE_ROWS] = write_limit;
    for (unsigned pass = 0; pass < 2; ++pass) for (size_t point = 1; point <= 2; ++point) {
      reserves = resizes = put_calls = 0; if (pass) fail_resize = point; else fail_reserve = point;
      check_equal(orm_tidesdb_sql_catalog_create(&owner, &definition, &id, &created, &error), TURBODB_STATUS_OUT_OF_MEMORY);
      check_equal(id, 99u); check_equal(put_calls, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
      fail_reserve = fail_resize = 0;
    }
    fail_savepoint = true; put_calls = 0;
    check_equal(orm_tidesdb_sql_catalog_create(&owner, &definition, &id, &created, &error), TURBODB_STATUS_OUT_OF_MEMORY);
    check_equal(put_calls, 0u); fail_savepoint = false; check_equal(create(&owner), 1u);
  }
  it("propagates native get failure and releases reserved read buffers") {
    initialize(); get_calls = 0; fail_get = 1;
    check_equal(orm_tidesdb_sql_catalog_begin(database, family, MAX_RECORD, &budget, &owner, &error), TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u); fail_get = 0;
    begin_owner(&owner); const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    get_calls = 0; fail_get = 2; uint64_t id = 99, version = 99; bool found = true;
    check_equal(orm_tidesdb_sql_catalog_lookup(&owner, vstr_from_cstr("items"), &loaded, &id, &version, &found, &error), TURBODB_STATUS_DATASTORE_ERROR);
    check_true(found); check_equal(id, 99u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
  }
  it("persists exact name entry and TableVersion golden bytes") {
    initialize(); begin_owner(&owner); bind_ddl("CREATE TABLE t (id BIGINT PRIMARY KEY)"); check_equal(create(&owner), 1u);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    const uint8_t key[] = {1,1,'t'}, version_key[] = {2,1,0,0,0,0,0,0,0}, version[] = {1,0,0,0,0,0,0,0};
    const uint8_t record[] = {1,0,0,0,0,0,0,0, 1,0,0,0,0,0,0,0, 1,0,0,0,0,0,0,0,
      'S','C',1,0, 1,0,0,0, 0,0,0,0, 1,'t', 2,1,0,'i','d'};
    raw_value(key, sizeof(key), record, sizeof(record)); raw_value(version_key, sizeof(version_key), version, sizeof(version));
    uint8_t manifest[STORE_MANIFEST_BYTES]; store_manifest_encode((store_manifest){1,2, 1}, manifest);
    raw_value(store_manifest_key, sizeof(store_manifest_key), manifest, sizeof(manifest));
  }
  it("refuses unknown entry identity wrong table names and truncated definitions") {
    initialize(); begin_owner(&owner); bind_ddl("CREATE TABLE t (id BIGINT PRIMARY KEY)"); check_equal(create(&owner), 1u);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    const uint8_t key[] = {1,1,'t'};
    const uint8_t record[] = {1,0,0,0,0,0,0,0, 1,0,0,0,0,0,0,0, 1,0,0,0,0,0,0,0,
      'S','C',1,0, 1,0,0,0, 0,0,0,0, 1,'t', 2,1,0,'i','d'};
    const size_t offsets[] = {0, STORE_GENERATION, STORE_CREATED, STORE_ENTRY_HEADER + 13};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
      uint8_t data[sizeof(record)]; memcpy(data, record, sizeof(data)); data[offsets[i]] = 2;
      if (i == 3) data[offsets[i]] = 'u';
      raw_put(key, sizeof(key), data, sizeof(data)); begin_owner(&owner);
      uint64_t id = 99, version = 99; bool found = false;
      check_equal(orm_tidesdb_sql_catalog_lookup(&owner, vstr_from_cstr("t"), &loaded, &id, &version, &found, &error), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(id, 99u); check_null(loaded.budget);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    }
    raw_put(key, sizeof(key), record, STORE_ENTRY_HEADER); begin_owner(&owner);
    uint64_t id = 99, version = 99; bool found = false;
    check_equal(orm_tidesdb_sql_catalog_lookup(&owner, vstr_from_cstr("t"), &loaded, &id, &version, &found, &error), TURBODB_STATUS_DATASTORE_ERROR);
  }
  it("refuses to repair orphan version records during CREATE") {
    initialize(); const uint8_t key[] = {2,1,0,0,0,0,0,0,0}, version[] = {1,0,0,0,0,0,0,0};
    raw_put(key, sizeof(key), version, sizeof(version)); begin_owner(&owner); bind_ddl(ddl);
    uint64_t id = 99; bool created = false; put_calls = 0;
    check_equal(orm_tidesdb_sql_catalog_create(&owner, &definition, &id, &created, &error), TURBODB_STATUS_DATASTORE_ERROR);
    check_contains(error.message, "orphan"); check_true(owner.failed); check_equal(put_calls, 0u);
    check_equal(id, 99u);
  }
  it("requires effective SYNC_FULL and leaves failed bootstrap uninitialized") {
    const char async_name[] = "catalog-not-durable";
    orm_tidesdb_column_family_config_t config = orm_tidesdb_default_column_family_config();
    check_equal(orm_tidesdb_create_column_family(database, async_name, &config), ORM_TDB_SUCCESS);
    orm_tidesdb_column_family_t *async_family = orm_tidesdb_get_column_family(database, async_name);
    check_equal(orm_tidesdb_sql_catalog_initialize(database, async_family, MAX_RECORD, &budget, &error), TURBODB_STATUS_INVALID_STATE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    put_calls = 0; fail_put = 1;
    check_equal(orm_tidesdb_sql_catalog_initialize(database, family, MAX_RECORD, &budget, &error), TURBODB_STATUS_DATASTORE_ERROR);
    faults_clear(); reopen();
    check_equal(orm_tidesdb_sql_catalog_begin(database, family, MAX_RECORD, &budget, &owner, &error), TURBODB_STATUS_INVALID_STATE);
    initialize(); begin_owner(&owner);
  }
  it("enforces record read and workspace budgets without publishing outputs") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); check_equal(create(&owner), 1u);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_begin(database, family, STORE_MANIFEST_BYTES, &budget, &owner, &error), TURBODB_STATUS_OK);
    uint64_t id = 99, version = 99; bool found = false;
    const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_tidesdb_sql_catalog_lookup(&owner, vstr_from_cstr("items"), &loaded, &id, &version, &found, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(loaded.budget); check_equal(id, 99u); check_false(found);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
    owner.max_record_bytes = MAX_RECORD;
    const uint64_t work_limit = budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES];
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = base; get_calls = 0;
    check_equal(orm_tidesdb_sql_catalog_lookup(&owner, vstr_from_cstr("items"), &loaded, &id, &version, &found, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(get_calls, 0u); budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = work_limit;
    budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS] = budget.used.value[ORM_SQL_BUDGET_READ_ROWS]; get_calls = 0;
    check_equal(orm_tidesdb_sql_catalog_lookup(&owner, vstr_from_cstr("items"), &loaded, &id, &version, &found, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(get_calls, 0u);
  }
  it("releases native payloads and partial definitions at every lookup failure point") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); check_equal(create(&owner), 1u);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    for (size_t point = 1; point <= 3; ++point) {
      begin_owner(&owner); const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      get_calls = 0; fail_get = point; uint64_t id = 99, version = 99; bool found = true;
      check_equal(orm_tidesdb_sql_catalog_lookup(&owner, vstr_from_cstr("items"), &loaded, &id, &version, &found, &error), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(id, 99u); check_true(found); check_null(loaded.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work); fail_get = 0;
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    }
    begin_owner(&owner); const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for (unsigned pass = 0; pass < 2; ++pass) for (size_t point = 1; point <= 2; ++point) {
      reserves = resizes = 0; if (pass) fail_resize = point; else fail_reserve = point;
      uint64_t id = 99, version = 99; bool found = true;
      check_equal(orm_tidesdb_sql_catalog_lookup(&owner, vstr_from_cstr("items"), &loaded, &id, &version, &found, &error), TURBODB_STATUS_OUT_OF_MEMORY);
      check_equal(id, 99u); check_true(found); check_null(loaded.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work); fail_reserve = fail_resize = 0;
    }
    check_true(lookup(&owner, "items", 1));
  }
  it("returns an empty SHOW TABLES result with owned metadata and no writes") {
    initialize(); begin_owner(&owner);
    const uint64_t writes = budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS];
    check_equal(open_show_on(&owner, "SHOW TABLES"), TURBODB_STATUS_OK);
    check_equal(show_source.schema.count, 1u);
    check_equal(show_source.schema.columns[0].name.len, strlen("Tables_in_local"));
    check_equal(memcmp(show_source.schema.columns[0].name.data, "Tables_in_local", strlen("Tables_in_local")), 0);
    check_null(show_row()); const size_t calls = cursor_calls; check_null(show_row()); check_equal(cursor_calls, calls);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_BUSY);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS], writes);
    close_show(); check_equal(owner.active_sources, 0u);
  }
  it("enumerates uncommitted tables only and emits FULL table types after reopen") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); create(&owner);
    bind_ddl("CREATE TABLE x (id BIGINT PRIMARY KEY)"); create(&owner);
    const char *queries[] = {"SHOW TABLES", "SHOW FULL TABLES"};
    for (size_t pass = 0; pass < 2; ++pass) {
      check_equal(open_show_on(&owner, queries[pass]), TURBODB_STATUS_OK);
      check_equal(show_source.schema.count, pass + 1);
      const turbodb_value_t *row = show_row(); check_not_null(row); text_is(&row[0], "x");
      if (pass) text_is(&row[1], "BASE TABLE");
      row = show_row(); check_not_null(row); text_is(&row[0], "items");
      if (pass) text_is(&row[1], "BASE TABLE");
      check_null(show_row()); close_show();
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
      reopen(); begin_owner(&owner);
    }
  }
  it("keeps SHOW TABLES on the owner snapshot across another commit") {
    initialize(); begin_owner(&owner); begin_owner(&other); bind_ddl(ddl); create(&owner);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    check_equal(open_show_on(&other, "SHOW TABLES"), TURBODB_STATUS_OK);
    check_null(show_row()); close_show();
    check_equal(orm_tidesdb_sql_catalog_finish(&other, false, &error), TURBODB_STATUS_OK);
    begin_owner(&owner); check_equal(open_show_on(&owner, "SHOW TABLES"), TURBODB_STATUS_OK);
    const turbodb_value_t *row = show_row(); check_not_null(row); text_is(&row[0], "items");
    check_null(show_row());
  }
  it("emits six MySQL column fields in definition order including nullable and unsigned types") {
    initialize(); begin_owner(&owner);
    bind_ddl("CREATE TABLE items (score BIGINT, id BIGINT UNSIGNED PRIMARY KEY, weight DOUBLE NOT NULL)"); create(&owner);
    const char *queries[] = {"SHOW COLUMNS FROM items", "SHOW FIELDS IN `items`"};
    const char *names[] = {"Field", "Type", "Null", "Key", "Default", "Extra"};
    const char *fields[] = {"score", "id", "weight"};
    const char *types[] = {"bigint", "bigint unsigned", "double"};
    for (size_t pass = 0; pass < 2; ++pass) {
      check_equal(open_show_on(&owner, queries[pass]), TURBODB_STATUS_OK); check_equal(show_source.schema.count, 6u);
      for (size_t i = 0; i < 6; ++i) {
        check_equal(show_source.schema.columns[i].name.len, strlen(names[i]));
        check_equal(memcmp(show_source.schema.columns[i].name.data, names[i], strlen(names[i])), 0);
      }
      for (size_t i = 0; i < 3; ++i) {
        const turbodb_value_t *row = show_row(); check_not_null(row);
        text_is(&row[0], fields[i]); text_is(&row[1], types[i]); text_is(&row[2], i ? "NO" : "YES");
        text_is(&row[3], i == 1 ? "PRI" : ""); check_equal(row[4].kind, TURBODB_VALUE_NULL); text_is(&row[5], "");
      }
      check_null(show_row()); close_show();
    }
  }
  it("adapts SHOW to the common scan and retains leases until scan and source close") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); create(&owner);
    check_equal(open_show_on(&owner, "SHOW COLUMNS FROM items"), TURBODB_STATUS_OK);
    const size_t projection[] = {0, 2, 4};
    const orm_sql_scan_spec spec = {.projection = projection, .projection_count = 3, .offset = 1, .limit = 1};
    check_equal(orm_tidesdb_sql_scan_open_source(&show_source.source, &spec, &budget, &show_scan, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_show_close(&show_source, &error), TURBODB_STATUS_BUSY);
    uint64_t id = 99; bool created = true;
    check_equal(orm_tidesdb_sql_catalog_create(&owner, &definition, &id, &created, &error), TURBODB_STATUS_BUSY);
    check_equal(id, 99u); check_true(created);
    orm_sql_scan_row row = {0};
    check_equal(orm_tidesdb_sql_scan_next(&show_scan, &row, &error), TURBODB_STATUS_OK);
    check_equal(row.state, ORM_SQL_SCAN_ROW); check_equal(row.count, 3u);
    text_is(&row.values[0], "score"); text_is(&row.values[1], "YES"); check_equal(row.values[2].kind, TURBODB_VALUE_NULL);
    check_equal(orm_tidesdb_sql_scan_next(&show_scan, &row, &error), TURBODB_STATUS_OK); check_equal(row.state, ORM_SQL_SCAN_DONE);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_BUSY);
    close_show(); check_equal(owner.active_sources, 0u);
  }
  it("cancels a TABLES scan before opening its native iterator") {
    initialize(); begin_owner(&owner); check_equal(open_show_on(&owner, "SHOW TABLES"), TURBODB_STATUS_OK);
    const size_t projection = 0; const orm_sql_scan_spec spec = {.projection = &projection, .projection_count = 1, .limit = UINT64_MAX};
    check_equal(orm_tidesdb_sql_scan_open_source(&show_source.source, &spec, &budget, &show_scan, &error), TURBODB_STATUS_OK);
    cursor_calls = 0;
    check_equal(orm_tidesdb_sql_scan_cancel(&show_scan, &error), TURBODB_STATUS_OK);
    orm_sql_scan_row row = {0}; check_equal(orm_tidesdb_sql_scan_next(&show_scan, &row, &error), TURBODB_STATUS_OK);
    check_equal(row.state, ORM_SQL_SCAN_CANCELLED); check_equal(cursor_calls, 0u);
    check_equal(orm_tidesdb_sql_show_close(&show_source, &error), TURBODB_STATUS_BUSY); close_show();
  }
  it("filters TABLES and COLUMNS LIKE results without retaining leases or changing data") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); create(&owner);
    const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],
        writes = budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS];
    const char *inputs[] = {"SHOW TABLES LIKE 'i%'", "SHOW COLUMNS FROM items LIKE 'ID'"};
    const char *expected[] = {"items", "id"};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
      check_equal(open_show_on(&owner, inputs[i]), TURBODB_STATUS_OK);
      const turbodb_value_t *row = show_row(); check_not_null(row);
      text_is(&row[0], expected[i]); check_null(show_row()); close_show();
      check_equal(owner.active_sources, 0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
    }
    check_equal(open_show_on(&owner, "SHOW TABLES LIKE 'I%'"), TURBODB_STATUS_OK);
    check_null(show_row()); close_show();
    check_equal(owner.active_sources, 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS], writes);
  }
  it("supplies unfiltered WHERE input rows for the runtime predicate owner") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); create(&owner);
    const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],
        writes = budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS];
    check_equal(open_show_on(&owner, "SHOW TABLES WHERE FALSE"), TURBODB_STATUS_OK);
    const turbodb_value_t *row = show_row(); check_not_null(row);
    text_is(&row[0], "items"); check_null(show_row()); close_show();
    check_equal(open_show_on(&owner, "SHOW COLUMNS FROM items WHERE FALSE"), TURBODB_STATUS_OK);
    const char *names[] = {"id", "score", "weight"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
      row = show_row(); check_not_null(row); text_is(&row[0], names[i]);
    }
    check_null(show_row()); close_show();
    check_equal(owner.active_sources, 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS], writes);
  }
  it("rejects SHOW variants and absent tables without retaining output or changing data") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); create(&owner);
    const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], writes = budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS];
    const char *unsupported[] = {"SHOW TABLES FROM app",
      "SHOW FULL COLUMNS FROM items", "SHOW EXTENDED COLUMNS FROM items", "SHOW COLUMNS FROM app.items",
      "SHOW COLUMNS FROM items FROM app",
      "SHOW DATABASES", "SHOW EXTENDED INDEX FROM items", "SHOW TABLES; SHOW TABLES", "SELECT 1"};
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
      check_equal(open_show_on(&owner, unsupported[i]), TURBODB_STATUS_UNSUPPORTED);
      check_null(show_source.source.budget); check_equal(owner.active_sources, 0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
    }
    check_equal(open_show_on(&owner, "SHOW COLUMNS FROM missing"), TURBODB_STATUS_SQL_ERROR);
    check_null(show_source.source.budget); check_equal(owner.active_sources, 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work); check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS], writes);
  }
  it("locks each iterator failure without publishing a row and requires rollback") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); create(&owner);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    for (cursor_fault fault = CURSOR_NEW; fault <= CURSOR_KEY; ++fault) {
      begin_owner(&owner); check_equal(open_show_on(&owner, "SHOW TABLES"), TURBODB_STATUS_OK);
      if (fault == CURSOR_NEXT) check_not_null(show_row());
      fail_cursor = fault;
      turbodb_value_t sentinel = turbodb_i64(99); const turbodb_value_t *row = &sentinel;
      check_equal(show_source.source.next(&show_source, &row, &error), TURBODB_STATUS_DATASTORE_ERROR);
      check_true(row == &sentinel); check_true(owner.failed); const size_t calls = cursor_calls;
      fail_cursor = CURSOR_OK;
      check_equal(show_source.source.next(&show_source, &row, &error), TURBODB_STATUS_DATASTORE_ERROR);
      check_true(row == &sentinel); check_equal(cursor_calls, calls);
      close_show(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_INVALID_STATE);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    }
  }
  it("rejects a malformed persisted Catalog name key without treating it as EOF") {
    initialize(); const uint8_t key[] = {1, 3, 'x'}, value = 0; raw_put(key, sizeof(key), &value, sizeof(value));
    begin_owner(&owner); check_equal(open_show_on(&owner, "SHOW TABLES"), TURBODB_STATUS_OK);
    const turbodb_value_t *row = NULL;
    check_equal(show_source.source.next(&show_source, &row, &error), TURBODB_STATUS_DATASTORE_ERROR);
    check_null(row); check_true(owner.failed);
  }
  it("checks table-version corruption while enumerating names") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); create(&owner);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    const uint8_t key[] = {2, 1, 0, 0, 0, 0, 0, 0, 0}, invalid[] = {0};
    raw_put(key, sizeof(key), invalid, sizeof(invalid)); begin_owner(&owner);
    check_equal(open_show_on(&owner, "SHOW TABLES"), TURBODB_STATUS_OK);
    const turbodb_value_t *row = NULL;
    check_equal(show_source.source.next(&show_source, &row, &error), TURBODB_STATUS_DATASTORE_ERROR);
    check_null(row); check_true(owner.failed);
  }
  it("recovers all SHOW schema allocations after failure at open or next") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); create(&owner);
    const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for (size_t mode = 0; mode < 2; ++mode) for (size_t pass = 0; pass < 2; ++pass) for (size_t point = 1; point <= 2; ++point) {
      if (mode) check_equal(open_show_on(&owner, "SHOW TABLES"), TURBODB_STATUS_OK);
      reserves = resizes = 0; if (pass) fail_resize = point; else fail_reserve = point;
      if (mode) {
        const turbodb_value_t *row = NULL;
        check_equal(show_source.source.next(&show_source, &row, &error), TURBODB_STATUS_OUT_OF_MEMORY); check_null(row);
        fail_reserve = fail_resize = 0;
        check_equal(show_source.source.next(&show_source, &row, &error), TURBODB_STATUS_OUT_OF_MEMORY);
      } else {
        check_equal(open_show_on(&owner, "SHOW COLUMNS FROM items"), TURBODB_STATUS_OUT_OF_MEMORY);
        check_null(show_source.source.budget);
      }
      fail_reserve = fail_resize = 0; close_show(); check_false(owner.failed);
      check_equal(owner.active_sources, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
    }
    check_equal(open_show_on(&owner, "SHOW TABLES"), TURBODB_STATUS_OK); check_not_null(show_row());
  }
  it("bounds SHOW open metadata and cursor reads and locks budget failures") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); create(&owner);
    const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = work;
    check_equal(open_show_on(&owner, "SHOW TABLES"), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(show_source.source.budget); check_equal(owner.active_sources, 0u);
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = WORK;
    const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_READ_ROWS, ORM_SQL_BUDGET_READ_BYTES, ORM_SQL_BUDGET_EXECUTION_STEPS};
    for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); ++i) {
      check_equal(open_show_on(&owner, "SHOW TABLES"), TURBODB_STATUS_OK);
      budget.limits.statement.value[resources[i]] = budget.used.value[resources[i]];
      const turbodb_value_t *row = NULL;
      check_equal(show_source.source.next(&show_source, &row, &error), TURBODB_STATUS_LIMIT_EXCEEDED); check_null(row);
      budget.limits.statement.value[resources[i]] = LIMIT;
      const size_t calls = cursor_calls;
      check_equal(show_source.source.next(&show_source, &row, &error), TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(cursor_calls, calls);
      close_show(); check_false(owner.failed); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
    }
  }
  it("propagates each Catalog point-read failure from SHOW and releases the owner lease") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); create(&owner);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    for (size_t point = 1; point <= 2; ++point) {
      begin_owner(&owner); const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(open_show_on(&owner, "SHOW TABLES"), TURBODB_STATUS_OK);
      get_calls = 0; fail_get = point; const turbodb_value_t *row = NULL;
      check_equal(show_source.source.next(&show_source, &row, &error), TURBODB_STATUS_DATASTORE_ERROR); check_null(row);
      fail_get = 0; close_show(); check_true(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work); check_equal(owner.active_sources, 0u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    }
  }

  it("owns a maximum-length database label and rejects invalid labels and SQLite documents") {
    initialize(); begin_owner(&owner);
    sqlparser_document *doc = NULL; sqlparser_error parse_error;
    const char sql[] = "SHOW TABLES";
    check_equal(sqlparser_parse(sql, sizeof(sql) - 1, NULL, &doc, &parse_error), SQLPARSER_OK);
    char label[ORM_SQL_SELECT_NAME_BYTES + 1]; memset(label, 'a', sizeof(label));
    const vstr oversized = {label, sizeof(label)}, valid = {label, ORM_SQL_SELECT_NAME_BYTES};
    const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_tidesdb_sql_show_open(doc, &owner, oversized, &show_source, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_tidesdb_sql_show_open(doc, &owner, vstr_from_cstr(""), &show_source, &error), TURBODB_STATUS_UNSUPPORTED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work); check_equal(owner.active_sources, 0u);
    check_equal(orm_tidesdb_sql_show_open(doc, &owner, valid, &show_source, &error), TURBODB_STATUS_OK);
    sqlparser_document_destroy(doc); doc = NULL; memset(label, 'z', sizeof(label));
    const vstr name = show_source.schema.columns[0].name;
    check_equal(name.len, strlen("Tables_in_") + ORM_SQL_SELECT_NAME_BYTES);
    for (size_t i = strlen("Tables_in_"); i < name.len; ++i) check_equal(name.data[i], 'a');
    check_equal(name.data[name.len], '\0'); close_show();
    const sqlparser_options options = {.dialect = SQLPARSER_SQLITE}; const char sqlite[] = "SELECT 1";
    check_equal(sqlparser_parse_with_options(sqlite, sizeof(sqlite) - 1, &options, NULL, &doc, &parse_error), SQLPARSER_OK);
    check_equal(orm_tidesdb_sql_show_open(doc, &owner, vstr_from_cstr("local"), &show_source, &error), TURBODB_STATUS_UNSUPPORTED);
    sqlparser_document_destroy(doc); check_null(show_source.source.budget);
  }
  it("rejects truncated Catalog entries during enumeration before publishing scalars") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); create(&owner);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    const uint8_t key[] = {1, 5, 'i', 't', 'e', 'm', 's'}, value = 0;
    raw_put(key, sizeof(key), &value, sizeof(value)); begin_owner(&owner);
    orm_sql_catalog_cursor cursor = {0};
    check_equal(orm_tidesdb_sql_catalog_cursor_open(&owner, &cursor, &error), TURBODB_STATUS_OK);
    uint64_t id = 99, version = 99; bool found = true;
    check_equal(orm_tidesdb_sql_catalog_cursor_next(&cursor, &loaded, &id, &version, &found, &error), TURBODB_STATUS_DATASTORE_ERROR);
    check_null(loaded.budget); check_equal(id, 99u); check_equal(version, 99u); check_true(found);
    check_true(owner.failed);
    check_equal(orm_tidesdb_sql_catalog_cursor_close(&cursor, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_cursor_close(&cursor, &error), TURBODB_STATUS_OK);
    check_equal(owner.active_sources, 0u);
  }
  it("refunds metadata and pins after each SHOW open point-read failure") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); create(&owner);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    const char *queries[] = {"SHOW TABLES", "SHOW COLUMNS FROM items"};
    for (size_t mode = 0; mode < 2; ++mode) for (size_t point = 1; point <= (mode ? 3u : 1u); ++point) {
      begin_owner(&owner); const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      get_calls = 0; fail_get = point;
      check_equal(open_show_on(&owner, queries[mode]), TURBODB_STATUS_DATASTORE_ERROR);
      fail_get = 0; check_null(show_source.source.budget); check_true(owner.failed);
      check_equal(owner.active_sources, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    }
  }

  it("releases each retained owner on commit success and conflict between statements") {
    initialize(); begin_owner(&owner); begin_owner(&other); bind_ddl(ddl);
    create(&owner); create(&other);
    check_equal(orm_tidesdb_sql_catalog_destroy(&definition, &error), TURBODB_STATUS_OK);
    check_equal(budget.retained_work_bytes, owner.metadata_bytes + other.metadata_bytes);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    const uint64_t writes = budget.transaction_used.write_bytes;
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    check_equal(budget.retained_work_bytes, other.metadata_bytes);
    check_equal(orm_tidesdb_sql_budget_reset_transaction(&budget, &error), TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_BUSY);
    check_null(owner.transaction); check_null(other.transaction);
    check_equal(budget.retained_work_bytes, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    check_equal(budget.transaction_used.write_bytes, writes);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    reopen(); begin_owner(&owner); check_true(lookup(&owner, "items", 1));
  }
  it("preserves COMMIT_UNKNOWN and releases retained work when no statement is active") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); create(&owner);
    check_equal(orm_tidesdb_sql_catalog_destroy(&definition, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    const size_t writes = put_calls; fail_commit_after = true;
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_COMMIT_UNKNOWN);
    check_equal(put_calls, writes); check_null(owner.transaction); check_null(owner.budget);
    check_false(budget.statement_active); check_equal(budget.retained_work_bytes, 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u); fail_commit_after = false;
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    reopen(); begin_owner(&owner); check_true(lookup(&owner, "items", 1));
  }
  it("preserves rollback errors while releasing inactive owner metadata exactly once") {
    initialize(); begin_owner(&owner); bind_ddl(ddl); create(&owner);
    check_equal(orm_tidesdb_sql_catalog_destroy(&definition, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK); fail_rollback_after = true;
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_DATASTORE_ERROR);
    check_contains(error.message, "rollback"); check_null(owner.transaction); check_null(owner.budget);
    check_equal(budget.retained_work_bytes, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); fail_rollback_after = false;
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    reopen(); begin_owner(&owner); check_false(lookup(&owner, "items", 0));
  }

}
