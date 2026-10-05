#include "relation.h"
#include "select.h"
#include "insert.h"
#include "change.h"
#include "cte_store.h"
#include <tinytest.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <cstl/sort.h>

static size_t reserves, resizes, fail_reserve, fail_resize, put_calls, fail_put;
static int fail_iterator;
static bool fail_savepoint, fail_release, fail_rollback;
static bool fail_sort;
static size_t sort_calls, fail_sort_at;
enum { OBSERVED_WRITES = 3 };
static bool observe_writes;
static uint8_t observed_ids[OBSERVED_WRITES];
static size_t observed_count;
static void observe_key(const uint8_t *key, size_t size) {
  if (!observe_writes || size != ORM_SQL_RELATION_KEY_BYTES) return;
  /* Fixture keys are positive small I64 values; the last BE byte identifies them. */
  if (observed_count < OBSERVED_WRITES) observed_ids[observed_count] = key[size - 1];
  ++observed_count;
}
static stl_status probe_sort(void *base, size_t count, const cmeta_type_desc *type, size_t bytes) {
  return ++sort_calls == fail_sort_at || fail_sort ? STL_OUT_OF_MEMORY : stable_sort(base, count, type, bytes);
}
static size_t delete_calls, fail_delete, new_calls, fail_new_at;
enum { FAIL_NEW = 1, FAIL_SEEK, FAIL_NEXT, FAIL_KEY, FAIL_VALUE };
static stl_status probe_reserve(vec_t *v, size_t n) { return ++reserves == fail_reserve ? STL_OUT_OF_MEMORY : vec_reserve(v, n); }
static stl_status probe_resize(vec_t *v, size_t n) { return ++resizes == fail_resize ? STL_OUT_OF_MEMORY : vec_resize(v, n); }
static int probe_put(orm_tidesdb_transaction_t *tx, orm_tidesdb_column_family_t *cf,
    const uint8_t *key, size_t key_size, const uint8_t *data, size_t size, time_t ttl) {
  observe_key(key, key_size);
  return ++put_calls == fail_put ? ORM_TDB_ERR_IO : orm_tidesdb_txn_put(tx, cf, key, key_size, data, size, ttl);
}
static int probe_savepoint(orm_tidesdb_transaction_t *tx, const char *name) {
  return fail_savepoint ? ORM_TDB_ERR_IO : orm_tidesdb_txn_savepoint(tx, name);
}
static int probe_delete(orm_tidesdb_transaction_t *tx, orm_tidesdb_column_family_t *cf,
    const uint8_t *key, size_t key_size) {
  observe_key(key, key_size);
  return ++delete_calls == fail_delete ? ORM_TDB_ERR_IO : orm_tidesdb_txn_delete(tx, cf, key, key_size);
}
static int probe_release(orm_tidesdb_transaction_t *tx, const char *name) {
  return fail_release ? ORM_TDB_ERR_IO : orm_tidesdb_txn_release_savepoint(tx, name);
}
static int probe_rollback(orm_tidesdb_transaction_t *tx, const char *name) {
  return fail_rollback ? ORM_TDB_ERR_IO : orm_tidesdb_txn_rollback_to_savepoint(tx, name);
}
static int probe_new(orm_tidesdb_transaction_t *tx, orm_tidesdb_column_family_t *cf, orm_tidesdb_iterator_t **it) {
  return ++new_calls == fail_new_at || fail_iterator == FAIL_NEW ? ORM_TDB_ERR_IO : orm_tidesdb_iter_new(tx, cf, it);
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
#define stable_sort probe_sort
#define vec_resize probe_resize
#define orm_tidesdb_txn_put probe_put
#define orm_tidesdb_txn_delete probe_delete
#define orm_tidesdb_txn_savepoint probe_savepoint
#define orm_tidesdb_txn_release_savepoint probe_release
#define orm_tidesdb_txn_rollback_to_savepoint probe_rollback
#define orm_tidesdb_iter_new probe_new
#define orm_tidesdb_iter_seek probe_seek
#define orm_tidesdb_iter_next probe_next
#define orm_tidesdb_iter_key probe_key
#define orm_tidesdb_iter_value probe_value
#include "../../src/work.c"
#include "../../src/catalog.c"
#include "../../src/catalog_store.c"
#include "../../src/relation.c"
#include "../../src/expr.c"
#include "../../src/binding.c"
#include "../../src/insert.c"
#include "../../src/change.c"
#undef vec_reserve
#undef stable_sort
#undef vec_resize
#undef orm_tidesdb_txn_put
#undef orm_tidesdb_txn_delete
#undef orm_tidesdb_txn_savepoint
#undef orm_tidesdb_txn_release_savepoint
#undef orm_tidesdb_txn_rollback_to_savepoint
#undef orm_tidesdb_iter_new
#undef orm_tidesdb_iter_seek
#undef orm_tidesdb_iter_next
#undef orm_tidesdb_iter_key
#undef orm_tidesdb_iter_value

enum { MAX_RECORD = 4096, WORK = 4 * 1024 * 1024, LIMIT = 1000000, DEPTH = 32, COLUMNS = 3 };
static const char family_name[] = "sql-relation";
static const char ddl[] = "CREATE TABLE items (id BIGINT PRIMARY KEY, score BIGINT, weight DOUBLE NOT NULL)";
static char *directory;
static orm_tidesdb_database_t *database;
static orm_tidesdb_column_family_t *family;
static orm_tidesdb_sql_budget budget;
static orm_sql_catalog_store owner, other;
static orm_sql_relation_source source;
static orm_sql_select plan;
static orm_sql_select_run run;
static turbodb_error_t error;
static struct {
  orm_sql_relation_source relation;
  orm_sql_from from;
  orm_sql_from_run from_run;
  orm_sql_select plan;
  orm_sql_select_run run;
  orm_sql_cte_store constants, result;
  orm_sql_cte_reader seed, constant, output;
  orm_sql_row_source seed_source, member_source;
  orm_sql_type seed_type, member_type;
  size_t opens, closes;
} rounds;

static turbodb_status_t round_pull(void *context, const turbodb_value_t **out, turbodb_error_t *e) {
  orm_sql_select_run *execution = context; orm_sql_scan_row row;
  const turbodb_status_t status = orm_tidesdb_sql_scan_next(&execution->scan, &row, e);
  if (status == TURBODB_STATUS_OK) *out = row.state == ORM_SQL_SCAN_ROW ? row.values : NULL;
  return status;
}
static turbodb_status_t round_open(void *context, orm_sql_row_source *frontier, orm_sql_row_source **out, turbodb_error_t *e) {
  check_true(context == &rounds); ++rounds.opens;
  turbodb_status_t status = orm_sql_relation_rewind(&rounds.relation, e);
  if (status == TURBODB_STATUS_OK) status = orm_sql_cte_reader_rewind(&rounds.constant, e);
  orm_sql_row_source *sources[] = {frontier, &rounds.relation.source, orm_sql_cte_reader_source(&rounds.constant)};
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_from_open(&rounds.from, sources,
      sizeof(sources) / sizeof(sources[0]), NULL, 0, &rounds.from_run, e);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_select_open_source(&rounds.plan,
      rounds.from_run.source, NULL, 0, &rounds.run, e);
  if (status == TURBODB_STATUS_OK) *out = &rounds.member_source;
  return status;
}
static turbodb_status_t round_close(void *context, turbodb_error_t *e) {
  check_true(context == &rounds); ++rounds.closes;
  turbodb_status_t status = orm_tidesdb_sql_select_close(&rounds.run, e);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_from_close(&rounds.from_run, e);
  return status;
}
static void rounds_close(void) {
  check_equal(orm_sql_cte_reader_close(&rounds.output, &error), TURBODB_STATUS_OK);
  check_equal(orm_sql_cte_store_close(&rounds.result, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_select_close(&rounds.run, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_from_close(&rounds.from_run, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_select_destroy(&rounds.plan, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_from_destroy(&rounds.from, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_relation_close(&rounds.relation, &error), TURBODB_STATUS_OK);
  check_equal(orm_sql_cte_reader_close(&rounds.seed, &error), TURBODB_STATUS_OK);
  check_equal(orm_sql_cte_reader_close(&rounds.constant, &error), TURBODB_STATUS_OK);
  check_equal(orm_sql_cte_store_close(&rounds.constants, &error), TURBODB_STATUS_OK);
}

static void faults_clear(void) {
  reserves = resizes = fail_reserve = fail_resize = put_calls = fail_put = 0; fail_iterator = 0;
  fail_savepoint = fail_release = fail_rollback = false;
  fail_sort = false; sort_calls = fail_sort_at = 0;
  observe_writes = false; observed_count = 0;
  delete_calls = fail_delete = new_calls = fail_new_at = 0;
}
static void database_open(void) {
  orm_tidesdb_config_t config = orm_tidesdb_default_config(); config.db_path = directory;
  check_equal(orm_tidesdb_open(&config, &database), ORM_TDB_SUCCESS);
}
static void owner_begin(orm_sql_catalog_store *out) {
  check_equal(orm_tidesdb_sql_catalog_begin(database, family, MAX_RECORD, &budget, out, &error), TURBODB_STATUS_OK);
}
static void create_table(const char *sql) {
  sqlparser_document *doc = NULL; sqlparser_error parse_error; orm_sql_table_definition definition = {0};
  check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &parse_error), SQLPARSER_OK);
  check_equal(orm_tidesdb_sql_catalog_bind_create(doc, &budget, &definition, &error), TURBODB_STATUS_OK);
  sqlparser_document_destroy(doc); uint64_t id = 0; bool created = false;
  check_equal(orm_tidesdb_sql_catalog_create(&owner, &definition, &id, &created, &error), TURBODB_STATUS_OK); check_true(created);
  check_equal(orm_tidesdb_sql_catalog_destroy(&definition, &error), TURBODB_STATUS_OK);
}
static turbodb_status_t insert_id(orm_sql_catalog_store *store, const char *table, int64_t id) {
  const turbodb_value_t values[] = {turbodb_i64(id), turbodb_i64(20), turbodb_f64(1.5)};
  return orm_tidesdb_sql_relation_insert(store, vstr_from_cstr(table), values, COLUMNS, &error);
}
static turbodb_status_t insert_sql_mode(const char *sql, const turbodb_value_t *parameters,
    size_t count, bool client_found_rows, size_t *affected) {
  sqlparser_document *doc = NULL; sqlparser_error parse_error;
  check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &parse_error), SQLPARSER_OK);
  const turbodb_status_t status = orm_tidesdb_sql_insert_execute(doc, &owner,
      parameters, count, DEPTH, 0, client_found_rows, NULL, affected, &error);
  sqlparser_document_destroy(doc); return status;
}
static turbodb_status_t insert_sql(const char *sql, const turbodb_value_t *parameters,
    size_t count, size_t *affected) {
  return insert_sql_mode(sql,parameters,count,false,affected);
}
static turbodb_status_t change_sql_mode(const char *sql,
    const turbodb_value_t *parameters, size_t count, bool client_found_rows,
    size_t *affected) {
  sqlparser_document *doc = NULL; sqlparser_error parse_error;
  check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &parse_error), SQLPARSER_OK);
  const turbodb_status_t status = orm_tidesdb_sql_change_execute(doc, &owner,
      parameters, count, DEPTH, 0, client_found_rows, NULL, affected, &error);
  sqlparser_document_destroy(doc); return status;
}
static turbodb_status_t change_sql(const char *sql, const turbodb_value_t *parameters,
    size_t count, size_t *affected) {
  return change_sql_mode(sql,parameters,count,false,affected);
}
static void seed_items(void) {
  for (int64_t id = 1; id <= COLUMNS; ++id) check_equal(insert_id(&owner, "items", id), TURBODB_STATUS_OK);
}
static void query_close(void) {
  check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_select_destroy(&plan, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_relation_close(&source, &error), TURBODB_STATUS_OK);
}
static void query_run_open(void) {
  check_equal(orm_tidesdb_sql_select_open_source(&plan, &source.source, NULL, 0, &run, &error), TURBODB_STATUS_OK);
}
static void query_open(orm_sql_catalog_store *store, const char *table, const char *sql) {
  check_equal(orm_tidesdb_sql_relation_open(store, vstr_from_cstr(table), &source, &error), TURBODB_STATUS_OK);
  sqlparser_document *doc = NULL; sqlparser_error parse_error;
  check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &parse_error), SQLPARSER_OK);
  check_equal(orm_tidesdb_sql_select_bind(doc, &source.schema, DEPTH, &budget, &plan, &error), TURBODB_STATUS_OK);
  sqlparser_document_destroy(doc);
  query_run_open();
}
static orm_sql_scan_row next(void) {
  orm_sql_scan_row row = {0}; check_equal(orm_tidesdb_sql_scan_next(&run.scan, &row, &error), TURBODB_STATUS_OK); return row;
}
static uint64_t version(orm_sql_catalog_store *store, const char *table) {
  orm_sql_table_definition definition = {0}; uint64_t id = 0, stamp = 0; bool found = false;
  check_equal(orm_tidesdb_sql_catalog_lookup(store, vstr_from_cstr(table), &definition, &id, &stamp, &found, &error), TURBODB_STATUS_OK);
  check_true(found); check_equal(orm_tidesdb_sql_catalog_destroy(&definition, &error), TURBODB_STATUS_OK); return stamp;
}
static void reopen(void) {
  query_close(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_catalog_finish(&other, false, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_close(database), ORM_TDB_SUCCESS); database = NULL;
  database_open(); family = orm_tidesdb_get_column_family(database, family_name); owner_begin(&owner);
}
static void raw_put(const uint8_t *key, size_t key_size, const uint8_t *data, size_t size) {
  orm_tidesdb_transaction_t *tx = NULL;
  check_equal(orm_tidesdb_txn_begin_with_isolation(database, ORM_TDB_ISOLATION_SERIALIZABLE, &tx), ORM_TDB_SUCCESS);
  check_equal(orm_tidesdb_txn_put(tx, family, key, key_size, data, size, 0), ORM_TDB_SUCCESS);
  check_equal(orm_tidesdb_txn_commit(tx), ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx);
}
spec("TidesDB relational numeric rows") {
  before_each() {
    memset(&rounds, 0, sizeof(rounds));
    faults_clear(); tdsql_error_init(&error); owner = other = (orm_sql_catalog_store){0};
    source = (orm_sql_relation_source){0}; plan = (orm_sql_select){0}; run = (orm_sql_select_run){0};
    orm_sql_budget_limits limits = {0};
    for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) limits.statement.value[i] = LIMIT;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = WORK;
    limits.transaction = (orm_sql_transaction_budget_amount){LIMIT, LIMIT, LIMIT};
    check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    directory = tt_make_temp_dir("orm-sql-relation"); check_not_null(directory); database_open();
    orm_tidesdb_column_family_config_t config = orm_tidesdb_default_column_family_config(); config.sync_mode = ORM_TDB_SYNC_FULL;
    check_equal(orm_tidesdb_create_column_family(database, family_name, &config), ORM_TDB_SUCCESS);
    family = orm_tidesdb_get_column_family(database, family_name); check_not_null(family);
    check_equal(orm_tidesdb_sql_catalog_initialize(database, family, MAX_RECORD, &budget, &error), TURBODB_STATUS_OK);
    owner_begin(&owner); create_table(ddl);
    create_table("CREATE TABLE other (id BIGINT PRIMARY KEY, score BIGINT, weight DOUBLE NOT NULL)");
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); owner_begin(&owner);
  }
  after_each() {
    faults_clear(); rounds_close(); query_close();
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, false, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_close(database), ORM_TDB_SUCCESS); database = NULL; family = NULL;
    check_equal(tt_remove_tree(directory), 0); free(directory); directory = NULL;
  }
  it("persists rows and reads them through SQL with Catalog-derived schema after reopen") {
    check_equal(insert_id(&owner, "items", 1), TURBODB_STATUS_OK);
    check_equal(insert_id(&owner, "items", 2), TURBODB_STATUS_OK); check_equal(version(&owner, "items"), 3u);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); reopen();
    query_open(&owner, "items", "SELECT t.id, t.score + 2 AS adjusted, t.weight FROM items t WHERE t.id > 1 LIMIT 1");
    orm_sql_scan_row row = next(); check_equal(row.state, ORM_SQL_SCAN_ROW); check_equal(row.count, 3u);
    check_equal(row.values[0].data.int64_value, 2); check_equal(row.values[1].data.int64_value, 22);
    check_true(row.values[2].data.double_value == 1.5); check_equal(next().state, ORM_SQL_SCAN_DONE);
  }
  it("round trips signed endpoints and preserves numeric key ordering and table isolation") {
    const int64_t ids[] = {INT64_MIN, -1, 0, 1, INT64_MAX};
    for (size_t i = sizeof(ids) / sizeof(ids[0]); i; --i) check_equal(insert_id(&owner, "items", ids[i - 1]), TURBODB_STATUS_OK);
    check_equal(insert_id(&owner, "other", 8), TURBODB_STATUS_OK);
    query_open(&owner, "items", "SELECT * FROM items");
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) check_equal(next().values[0].data.int64_value, ids[i]);
    check_equal(next().state, ORM_SQL_SCAN_DONE);
  }
  it("round trips unsigned endpoints NULL and signed floating zero") {
    create_table("CREATE TABLE unsigned_items (id BIGINT UNSIGNED PRIMARY KEY, score BIGINT, weight DOUBLE NOT NULL)");
    const uint64_t ids[] = {0, UINT64_MAX};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
      const turbodb_value_t values[] = {turbodb_u64(ids[i]), turbodb_null(), turbodb_f64(-0.0)};
      check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr("unsigned_items"), values, COLUMNS, &error), TURBODB_STATUS_OK);
    }
    query_open(&owner, "unsigned_items", "SELECT * FROM unsigned_items");
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
      const orm_sql_scan_row row = next(); check_equal(row.values[0].data.uint64_value, ids[i]);
      check_equal(row.values[1].kind, TURBODB_VALUE_NULL); check_true(signbit(row.values[2].data.double_value));
    }
    check_equal(next().state, ORM_SQL_SCAN_DONE);
  }
  it("rejects duplicate keys invalid values and wrong shape without changing rows or version") {
    check_equal(insert_id(&owner, "items", 1), TURBODB_STATUS_OK);
    check_equal(insert_id(&owner, "items", 1), TURBODB_STATUS_CONSTRAINT); check_equal(version(&owner, "items"), 2u);
    turbodb_value_t values[] = {turbodb_i64(2), turbodb_i64(20), turbodb_f64(1.5)};
    const turbodb_value_t invalid[] = {turbodb_null(), turbodb_u64(2), turbodb_text("2")};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      values[0] = invalid[i];
      check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr("items"), values, COLUMNS, &error), TURBODB_STATUS_TYPE_ERROR);
    }
    values[0] = turbodb_i64(2); values[2] = turbodb_f64(INFINITY);
    check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr("items"), values, COLUMNS, &error), TURBODB_STATUS_TYPE_ERROR);
    check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr("items"), values, 1, &error), TURBODB_STATUS_TYPE_ERROR);
    check_equal(insert_id(&owner, "absent", 1), TURBODB_STATUS_SQL_ERROR); check_equal(version(&owner, "items"), 2u);
  }
  it("rolls back data and table versions together and keeps earlier successful statements") {
    check_equal(insert_id(&owner, "items", 1), TURBODB_STATUS_OK);
    for (size_t point = 1; point <= 2; ++point) {
      put_calls = 0; fail_put = point;
      check_equal(insert_id(&owner, "items", 2), TURBODB_STATUS_DATASTORE_ERROR); fail_put = 0;
      check_false(owner.failed); check_equal(version(&owner, "items"), 2u);
      query_open(&owner, "items", "SELECT id FROM items"); check_equal(next().values[0].data.int64_value, 1);
      check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
    }
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); reopen();
    check_equal(version(&owner, "items"), 1u); query_open(&owner, "items", "SELECT id FROM items");
    check_equal(next().state, ORM_SQL_SCAN_DONE);
  }
  it("keeps the owner alive and forbids writes until every source closes") {
    check_equal(insert_id(&owner, "items", 1), TURBODB_STATUS_OK);
    query_open(&owner, "items", "SELECT id FROM items LIMIT 0"); check_null(source.iterator);
    check_equal(next().state, ORM_SQL_SCAN_DONE); check_null(source.iterator);
    check_equal(insert_id(&owner, "items", 2), TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_relation_close(&source, &error), TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
    check_equal(insert_id(&owner, "items", 2), TURBODB_STATUS_BUSY); query_close();
    check_equal(owner.active_sources, 0u); check_equal(insert_id(&owner, "items", 2), TURBODB_STATUS_OK);
  }
  it("reuses a compiled SELECT over partial and exhausted relation sources without rebinding") {
    seed_items(); query_open(&owner, "items", "SELECT id, score + 2 AS adjusted FROM items");
    check_equal(next().values[0].data.int64_value, 1);
    const orm_sql_schema_column *columns = source.schema.columns;
    const orm_sql_type *types = source.source.types;
    enum { ROUNDS = 3 };
    for (size_t round = 0; round < ROUNDS; ++round) {
      check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
      const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      const uint64_t reads = budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
      const uint64_t steps = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      const size_t allocations = reserves, iterators = new_calls;
      check_equal(orm_sql_relation_rewind(&source, &error), TURBODB_STATUS_OK);
      check_null(source.iterator); check_false(source.done); check_false(source.advance);
      check_true(source.schema.columns == columns); check_true(source.source.types == types);
      check_equal(owner.active_sources, 1u); check_equal(reserves, allocations); check_equal(new_calls, iterators);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], reads);
      check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], steps + 1);
      check_equal(insert_id(&owner, "items", 4), TURBODB_STATUS_BUSY);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_BUSY);
      query_run_open();
      for (int64_t id = 1; id <= COLUMNS; ++id) {
        const orm_sql_scan_row row = next(); check_equal(row.state, ORM_SQL_SCAN_ROW);
        check_equal(row.values[0].data.int64_value, id); check_equal(row.values[1].data.int64_value, 22);
      }
      check_equal(next().state, ORM_SQL_SCAN_DONE);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], reads + COLUMNS);
    }
  }
  it("requires consumer close before rewind including EOF and cancellation") {
    check_equal(orm_sql_relation_rewind(NULL, &error), TURBODB_STATUS_INVALID_STATE);
    check_equal(orm_sql_relation_rewind(&source, &error), TURBODB_STATUS_INVALID_STATE);
    seed_items(); query_open(&owner, "items", "SELECT id FROM items LIMIT 1");
    check_equal(orm_sql_relation_rewind(&source, &error), TURBODB_STATUS_BUSY); check_null(source.iterator);
    check_equal(next().values[0].data.int64_value, 1); check_equal(next().state, ORM_SQL_SCAN_DONE);
    orm_tidesdb_iterator_t *iterator = source.iterator;
    check_equal(orm_sql_relation_rewind(&source, &error), TURBODB_STATUS_BUSY); check_true(source.iterator == iterator);
    check_equal(orm_tidesdb_sql_scan_cancel(&run.scan, &error), TURBODB_STATUS_OK);
    check_equal(orm_sql_relation_rewind(&source, &error), TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
    check_equal(orm_sql_relation_rewind(&source, &error), TURBODB_STATUS_OK);
    query_run_open(); check_equal(next().values[0].data.int64_value, 1);
  }
  it("retains the transaction snapshot across rewinds and detects stale write decisions") {
    check_equal(insert_id(&owner, "items", 1), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    owner_begin(&owner); owner_begin(&other);
    query_open(&owner, "items", "SELECT id FROM items");
    check_equal(next().values[0].data.int64_value, 1); check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(insert_id(&other, "items", 2), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
    check_equal(orm_sql_relation_rewind(&source, &error), TURBODB_STATUS_OK); query_run_open();
    check_equal(next().values[0].data.int64_value, 1); check_equal(next().state, ORM_SQL_SCAN_DONE);
    query_close(); check_equal(insert_id(&owner, "other", 1), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_BUSY);
  }
  it("rewinds empty and unopened sources lazily without catalog or row reads") {
    query_open(&owner, "items", "SELECT id FROM items");
    check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
    const uint64_t reads = budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    faults_clear(); fail_iterator = FAIL_NEW;
    check_equal(orm_sql_relation_rewind(&source, &error), TURBODB_STATUS_OK);
    check_equal(new_calls, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], reads);
    faults_clear(); query_run_open(); check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
    check_equal(orm_sql_relation_rewind(&source, &error), TURBODB_STATUS_OK); check_null(source.iterator);
    query_run_open(); check_equal(next().state, ORM_SQL_SCAN_DONE);
  }
  it("preserves a partial cursor when rewind cannot reserve its step") {
    seed_items(); query_open(&owner, "items", "SELECT id FROM items");
    check_equal(next().values[0].data.int64_value, 1);
    check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
    orm_tidesdb_iterator_t *iterator = source.iterator;
    const uint64_t limit = budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_sql_relation_rewind(&source, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_true(source.iterator == iterator); check_true(source.advance); check_false(source.done);
    check_equal(source.failure.status, TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = limit;
    query_run_open(); check_equal(next().values[0].data.int64_value, 2);
  }
  it("does not use rewind to clear a latched read quota failure") {
    seed_items(); query_open(&owner, "items", "SELECT id FROM items");
    const uint64_t limit = budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS];
    budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS] = budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    orm_sql_scan_row row = {0};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &row, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
    const turbodb_error_t cause = source.failure;
    budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS] = limit;
    orm_tidesdb_iterator_t *iterator = source.iterator;
    check_equal(orm_sql_relation_rewind(&source, &error), cause.status);
    check_equal(strcmp(error.message, cause.message), 0); check_true(source.iterator == iterator);
    check_false(owner.failed);
  }
  it("reopens compiled recursive JOIN members over native rows and one shared CTE cache") {
    seed_items(); query_open(&owner, "items", "SELECT id AS n FROM items WHERE id = 1");
    rounds.seed_type = orm_tidesdb_sql_select_column_at(&plan, 0)->type;
    rounds.seed_source = (orm_sql_row_source){&budget, &rounds.seed_type, 1, &run, round_pull, false};
    check_equal(orm_sql_cte_store_open(&rounds.seed_source, &rounds.constants, &error), TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_reader_open(&rounds.constants, &rounds.seed, &error), TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_reader_open(&rounds.constants, &rounds.constant, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_relation_open(&owner, vstr_from_cstr("items"), &rounds.relation, &error), TURBODB_STATUS_OK);
    const orm_sql_schema_column recursive_column = {vstr_from_cstr("n"), {TURBODB_VALUE_INT64, true}};
    const orm_sql_schema_column constant_column = {vstr_from_cstr("n"), rounds.seed_type};
    const orm_sql_table_schema recursive_schema = {vstr_from_cstr("r"), &recursive_column, 1};
    const orm_sql_table_schema constant_schema = {vstr_from_cstr("c"), &constant_column, 1};
    const orm_sql_table_schema *schemas[] = {&recursive_schema, &rounds.relation.schema, &constant_schema};
    const char sql[] = "SELECT r.n + t.id + c.n AS n FROM r JOIN items t ON t.id = 1 CROSS JOIN c WHERE r.n < 5";
    sqlparser_document *doc = NULL; sqlparser_error parse_error;
    check_equal(sqlparser_parse(sql, sizeof(sql) - 1, NULL, &doc, &parse_error), SQLPARSER_OK);
    check_equal(orm_tidesdb_sql_from_bind(doc, schemas, sizeof(schemas) / sizeof(schemas[0]),
        NULL, 0, DEPTH, &budget, &rounds.from, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_bind_from(doc, &rounds.from, DEPTH, &rounds.plan, &error), TURBODB_STATUS_OK);
    sqlparser_document_destroy(doc);
    rounds.member_type = orm_tidesdb_sql_select_column_at(&rounds.plan, 0)->type;
    rounds.member_source = (orm_sql_row_source){&budget, &rounds.member_type, 1, &rounds.run, round_pull, false};
    const orm_sql_cte_recursion recursion = {&rounds, round_open, round_close, DEPTH, false};
    check_equal(orm_sql_cte_store_open_recursive(orm_sql_cte_reader_source(&rounds.seed),
        &recursion, &rounds.result, &error), TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_reader_open(&rounds.result, &rounds.output, &error), TURBODB_STATUS_OK);
    check_null(source.iterator); check_null(rounds.relation.iterator); check_equal(rounds.opens, 0u);
    orm_sql_row_source *output = orm_sql_cte_reader_source(&rounds.output);
    const int64_t expected[] = {1, 3, 5};
    const uint64_t reads = budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
      const turbodb_value_t *row = NULL;
      check_equal(output->next(output->context, &row, &error), TURBODB_STATUS_OK); check_not_null(row);
      check_equal(row[0].data.int64_value, expected[i]);
    }
    const turbodb_value_t *row = NULL; check_equal(output->next(output->context, &row, &error), TURBODB_STATUS_OK); check_null(row);
    check_equal(rounds.opens, 3u); check_equal(rounds.closes, rounds.opens);
    check_equal(vec_size(&rounds.constants.rows.snapshots), 1u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], reads + COLUMNS * (1 + rounds.opens));
    check_equal(owner.active_sources, 2u); check_false(rounds.result.frontier.active);
    check_equal(orm_sql_cte_reader_rewind(&rounds.output, &error), TURBODB_STATUS_OK);
    check_equal(output->next(output->context, &row, &error), TURBODB_STATUS_OK); check_equal(row[0].data.int64_value, 1);
    check_equal(rounds.opens, 3u);
  }
  it("detects competing writes to the same table but permits independent tables") {
    owner_begin(&other); check_equal(insert_id(&owner, "items", 1), TURBODB_STATUS_OK);
    check_equal(insert_id(&other, "items", 2), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_BUSY);
    owner_begin(&owner); owner_begin(&other);
    check_equal(insert_id(&owner, "items", 3), TURBODB_STATUS_OK); check_equal(insert_id(&other, "other", 3), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_OK);
  }
  it("registers an empty range read so a later write to another table cannot commit stale decisions") {
    owner_begin(&other); query_open(&owner, "items", "SELECT id FROM items");
    check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
    check_equal(insert_id(&other, "items", 1), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_OK);
    query_open(&owner, "items", "SELECT id FROM items"); check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
    check_equal(insert_id(&owner, "other", 1), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_BUSY);
  }
  it("matches exact persisted Data key and numeric wire bytes") {
    check_equal(insert_id(&owner, "items", 1), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    const uint8_t key[] = {3,1,0,0,0,0,0,0,0, 1,0,0,0,0,0,0,0, 128,0,0,0,0,0,0,1};
    const uint8_t expected[] = {'R','R',1,0,3,0,0,0, 0,1,0,0,0,0,0,0,0,
      0,20,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,248,63};
    orm_tidesdb_transaction_t *tx = NULL; uint8_t *data = NULL; size_t size = 0;
    check_equal(orm_tidesdb_txn_begin_with_isolation(database, ORM_TDB_ISOLATION_SERIALIZABLE, &tx), ORM_TDB_SUCCESS);
    check_equal(orm_tidesdb_txn_get(tx, family, key, sizeof(key), &data, &size), ORM_TDB_SUCCESS);
    check_equal(size, sizeof(expected)); check_equal(memcmp(data, expected, sizeof(expected)), 0); orm_tidesdb_free(data);
    check_equal(orm_tidesdb_txn_rollback(tx), ORM_TDB_SUCCESS); orm_tidesdb_txn_free(tx);
  }
  it("rejects malformed rows including unprojected values and key mismatch with a sticky error") {
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    const uint8_t key[] = {3,1,0,0,0,0,0,0,0, 1,0,0,0,0,0,0,0, 128,0,0,0,0,0,0,1};
    const uint8_t valid[] = {'R','R',1,0,3,0,0,0, 0,1,0,0,0,0,0,0,0,
      0,20,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,248,63};
    const size_t offsets[] = {0,2,3,4,8,9,17,26,34};
    const uint8_t mutations[] = {0,2,1,4,1,2,2,1,127};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
      uint8_t data[sizeof(valid)]; memcpy(data, valid, sizeof(data)); data[offsets[i]] = mutations[i];
      raw_put(key, sizeof(key), data, sizeof(data)); owner_begin(&owner);
      query_open(&owner, "items", "SELECT id FROM items"); orm_sql_scan_row row = {.state = ORM_SQL_SCAN_CANCELLED};
      check_equal(orm_tidesdb_sql_scan_next(&run.scan, &row, &error), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(row.state, ORM_SQL_SCAN_CANCELLED); check_true(owner.failed);
      const uint64_t reads = budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
      check_equal(orm_tidesdb_sql_scan_next(&run.scan, &row, &error), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], reads);
      query_close(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    }
    raw_put(key, sizeof(key), valid, sizeof(valid) - 1); owner_begin(&owner);
    check_equal(insert_id(&owner, "items", 1), TURBODB_STATUS_DATASTORE_ERROR); check_true(owner.failed);
  }
  it("propagates every native iterator failure without publishing a partial row") {
    check_equal(insert_id(&owner, "items", 1), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    for (int failure = FAIL_NEW; failure <= FAIL_VALUE; ++failure) {
      owner_begin(&owner); query_open(&owner, "items", "SELECT * FROM items");
      if (failure == FAIL_NEXT) check_equal(next().state, ORM_SQL_SCAN_ROW);
      fail_iterator = failure; orm_sql_scan_row row = {.state = ORM_SQL_SCAN_CANCELLED};
      check_equal(orm_tidesdb_sql_scan_next(&run.scan, &row, &error), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(row.state, ORM_SQL_SCAN_CANCELLED); fail_iterator = 0;
      check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
      const turbodb_error_t cause = source.failure;
      check_equal(orm_sql_relation_rewind(&source, &error), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(strcmp(error.message, cause.message), 0);
      query_close(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    }
  }
  it("refunds every selected insert and source allocation failure") {
    const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for (unsigned pass = 0; pass < 2; ++pass) for (size_t point = 1; point <= 3; ++point) {
      reserves = resizes = 0; if (pass) fail_resize = point; else fail_reserve = point;
      check_equal(insert_id(&owner, "items", 1), TURBODB_STATUS_OUT_OF_MEMORY);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base); faults_clear();
    }
    for (unsigned pass = 0; pass < 2; ++pass) for (size_t point = 1; point <= 4; ++point) {
      reserves = resizes = 0; if (pass) fail_resize = point; else fail_reserve = point;
      check_equal(orm_tidesdb_sql_relation_open(&owner, vstr_from_cstr("items"), &source, &error), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(source.owner); check_equal(owner.active_sources, 0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base); faults_clear();
    }
    check_equal(insert_id(&owner, "items", 1), TURBODB_STATUS_OK); query_open(&owner, "items", "SELECT * FROM items");
    reserves = resizes = 0; fail_reserve = fail_resize = 1;
    check_equal(next().state, ORM_SQL_SCAN_ROW); check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(reserves, 0u); check_equal(resizes, 0u);
  }
  it("rejects exhausted table versions and write limits before writing") {
    const uint64_t write_limit = budget.limits.statement.value[ORM_SQL_BUDGET_WRITE_ROWS];
    budget.limits.statement.value[ORM_SQL_BUDGET_WRITE_ROWS] = budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS] + 1;
    put_calls = 0; check_equal(insert_id(&owner, "items", 1), TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(put_calls, 0u);
    budget.limits.statement.value[ORM_SQL_BUDGET_WRITE_ROWS] = write_limit;
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    const uint8_t key[] = {2,1,0,0,0,0,0,0,0}, stamp[] = {255,255,255,255,255,255,255,255};
    raw_put(key, sizeof(key), stamp, sizeof(stamp)); owner_begin(&owner); put_calls = 0;
    check_equal(insert_id(&owner, "items", 1), TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(put_calls, 0u);
  }
  it("retains multiple sources and cancellation performs no Data reads") {
    query_open(&owner, "items", "SELECT id FROM items"); orm_sql_relation_source second = {0};
    check_equal(orm_tidesdb_sql_relation_open(&owner, vstr_from_cstr("other"), &second, &error), TURBODB_STATUS_OK);
    check_equal(owner.active_sources, 2u); uint64_t id = 99; bool created = false;
    check_equal(orm_tidesdb_sql_catalog_create(&owner, &source.definition, &id, &created, &error), TURBODB_STATUS_BUSY);
    check_equal(id, 99u); check_equal(orm_tidesdb_sql_relation_close(&second, &error), TURBODB_STATUS_OK);
    const uint64_t reads = budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(orm_tidesdb_sql_scan_cancel(&run.scan, &error), TURBODB_STATUS_OK);
    check_equal(next().state, ORM_SQL_SCAN_CANCELLED); check_null(source.iterator);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], reads); check_equal(owner.active_sources, 1u);
    query_close(); check_equal(owner.active_sources, 0u); check_equal(insert_id(&owner, "items", 1), TURBODB_STATUS_OK);
  }
  it("rejects every truncated row trailing bytes malformed key and NULL payload") {
    check_equal(orm_tidesdb_sql_relation_open(&owner, vstr_from_cstr("items"), &source, &error), TURBODB_STATUS_OK);
    const uint8_t key[] = {3,1,0,0,0,0,0,0,0, 1,0,0,0,0,0,0,0, 128,0,0,0,0,0,0,1};
    const uint8_t valid[] = {'R','R',1,0,3,0,0,0, 0,1,0,0,0,0,0,0,0,
      0,20,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,248,63};
    for (size_t n = 0; n < sizeof(valid); ++n)
      check_equal(relation_decode(&source.schema, 0, valid, n, key, sizeof(key), &budget, NULL, &error), TURBODB_STATUS_DATASTORE_ERROR);
    uint8_t data[sizeof(valid) + 1]; memcpy(data, valid, sizeof(valid)); data[sizeof(valid)] = 0;
    check_equal(relation_decode(&source.schema, 0, data, sizeof(data), key, sizeof(key), &budget, NULL, &error), TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(relation_decode(&source.schema, 0, valid, sizeof(valid), key, sizeof(key) - 1, &budget, NULL, &error), TURBODB_STATUS_DATASTORE_ERROR);
    data[REL_HEADER + REL_CELL] = 1;
    check_equal(relation_decode(&source.schema, 0, data, sizeof(valid), key, sizeof(key), &budget, NULL, &error), TURBODB_STATUS_DATASTORE_ERROR);
    memcpy(data, valid, sizeof(valid)); memset(data + REL_HEADER + 2 * REL_CELL, 0, REL_CELL); data[REL_HEADER + 2 * REL_CELL] = 1;
    check_equal(relation_decode(&source.schema, 0, data, sizeof(valid), key, sizeof(key), &budget, NULL, &error), TURBODB_STATUS_DATASTORE_ERROR);
  }
  it("rejects concurrent insertion of an identical primary key") {
    owner_begin(&other); check_equal(insert_id(&owner, "items", 1), TURBODB_STATUS_OK);
    check_equal(insert_id(&other, "items", 1), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_BUSY);
    reopen(); check_equal(version(&owner, "items"), 2u);
    query_open(&owner, "items", "SELECT id FROM items"); check_equal(next().values[0].data.int64_value, 1);
    check_equal(next().state, ORM_SQL_SCAN_DONE);
  }
  it("enforces workspace row-byte and execution limits and leaves failed outputs empty") {
    const uint64_t work_limit = budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES];
    const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = base;
    check_equal(orm_tidesdb_sql_relation_open(&owner, vstr_from_cstr("items"), &source, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(source.owner); check_equal(owner.active_sources, 0u);
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = work_limit;
    check_equal(insert_id(&owner, "items", 1), TURBODB_STATUS_OK);
    query_open(&owner, "items", "SELECT id FROM items");
    const size_t max_record = owner.max_record_bytes; owner.max_record_bytes = REL_HEADER;
    orm_sql_scan_row row = {.state = ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &row, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(row.state, ORM_SQL_SCAN_CANCELLED); owner.max_record_bytes = max_record; query_close();
    query_open(&owner, "items", "SELECT id FROM items");
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &row, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(row.state, ORM_SQL_SCAN_CANCELLED);
  }
  group("SQL INSERT VALUES") {
    it("reorders complete columns evaluates expressions and persists the entire batch") {
      const turbodb_value_t parameters[] = {turbodb_f64(1.5), turbodb_i64(10), turbodb_f64(2.5), turbodb_i64(30)};
      size_t affected = SIZE_MAX; const uint64_t stamp = version(&owner, "items");
      check_equal(insert_sql("INSERT INTO `items` (`weight`,id,score) VALUES (?,2,?+1),(?,1,?*2)",
          parameters, sizeof(parameters) / sizeof(parameters[0]), &affected), TURBODB_STATUS_OK);
      check_equal(affected, 2u); check_equal(version(&owner, "items"), stamp + 1);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); reopen();
      query_open(&owner, "items", "SELECT * FROM items");
      orm_sql_scan_row row = next(); check_equal(row.values[0].data.int64_value, 1);
      check_equal(row.values[1].data.int64_value, 60); check_true(row.values[2].data.double_value == 2.5);
      row = next(); check_equal(row.values[0].data.int64_value, 2);
      check_equal(row.values[1].data.int64_value, 11); check_true(row.values[2].data.double_value == 1.5);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("binds parameters in source order including unexecuted CASE branches") {
      const turbodb_value_t parameters[] = {turbodb_i64(1), turbodb_i64(99), turbodb_i64(7), turbodb_f64(2.5),
          turbodb_i64(2), turbodb_null(), turbodb_i64(8), turbodb_f64(3.5)};
      size_t affected = SIZE_MAX;
      check_equal(insert_sql("INSERT INTO items (id,score,weight) VALUES (?,CASE WHEN FALSE THEN ? ELSE ? END,?),"
          "(?,COALESCE(?,?),?)", parameters, sizeof(parameters) / sizeof(parameters[0]), &affected), TURBODB_STATUS_OK);
      check_equal(affected, 2u); query_open(&owner, "items", "SELECT score FROM items");
      check_equal(next().values[0].data.int64_value, 7); check_equal(next().values[0].data.int64_value, 8);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("supports signed and unsigned boundaries with MySQL assignment conversion") {
      create_table("CREATE TABLE unsigned_items (id BIGINT UNSIGNED PRIMARY KEY, score BIGINT, weight DOUBLE NOT NULL)");
      const turbodb_value_t parameters[] = {turbodb_u64(0), turbodb_f64(-0.0), turbodb_f64(1.5)}; size_t affected = SIZE_MAX;
      check_equal(insert_sql("INSERT INTO unsigned_items (id,score,weight) VALUES (?,NULL,?),"
          "(18446744073709551615,-9223372036854775808,?)", parameters, COLUMNS, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 2u); query_open(&owner, "unsigned_items", "SELECT * FROM unsigned_items");
      orm_sql_scan_row row = next(); check_equal(row.values[0].data.uint64_value, 0u);
      check_equal(row.values[1].kind, TURBODB_VALUE_NULL); check_true(signbit(row.values[2].data.double_value));
      row = next(); check_equal(row.values[0].data.uint64_value, UINT64_MAX);
      check_equal(row.values[1].data.int64_value, INT64_MIN); query_close();
      const turbodb_value_t weight = turbodb_f64(1.5);
      check_equal(insert_sql("INSERT INTO unsigned_items (id,score,weight) VALUES (1,NULL,?)", &weight, 1, &affected), TURBODB_STATUS_OK);
      check_equal(affected,1u);
    }
    it("normalizes INSERT SET and reuses duplicate-key and IGNORE execution") {
      const turbodb_value_t first[] = {turbodb_f64(1.5), turbodb_i64(1), turbodb_i64(10)};
      size_t affected = SIZE_MAX;
      check_equal(insert_sql("INSERT INTO items SET weight=?,id=?,score=?",
          first, sizeof(first) / sizeof(first[0]), &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u);
      const turbodb_value_t duplicate[] = {turbodb_i64(20), turbodb_f64(2.5), turbodb_i64(1)};
      check_equal(insert_sql("INSERT INTO items SET score=?,weight=?,id=? "
          "ON DUPLICATE KEY UPDATE score=VALUES(score)+1,weight=VALUES(weight)",
          duplicate, sizeof(duplicate) / sizeof(duplicate[0]), &affected), TURBODB_STATUS_OK);
      check_equal(affected, 2u);
      const turbodb_value_t ignored[] = {turbodb_f64(3.5), turbodb_i64(1), turbodb_i64(99)};
      check_equal(insert_sql("INSERT IGNORE INTO items SET weight=?,id=?,score=?",
          ignored, sizeof(ignored) / sizeof(ignored[0]), &affected), TURBODB_STATUS_OK);
      check_equal(affected, 0u);
      query_open(&owner, "items", "SELECT id,score,weight FROM items");
      orm_sql_scan_row row = next();
      check_equal(row.values[0].data.int64_value, 1);
      check_equal(row.values[1].data.int64_value, 21);
      check_equal(row.values[2].data.double_value, 2.5);
      check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
    }
    it("binds MySQL INSERT row and column aliases to the candidate row") {
      size_t affected=SIZE_MAX;
      check_equal(insert_sql("INSERT INTO items VALUES(1,10,1.5)",NULL,0,
          &affected),TURBODB_STATUS_OK);
      check_equal(insert_sql("INSERT INTO items(id,score,weight) VALUES(1,50,2.5) "
          "AS incoming(i,s,w) ON DUPLICATE KEY UPDATE "
          "score=incoming.s+1,weight=w",NULL,0,&affected),TURBODB_STATUS_OK);
      check_equal(affected,2u);
      check_equal(insert_sql("INSERT INTO items SET id=1,score=70,weight=3.5 "
          "AS incoming ON DUPLICATE KEY UPDATE "
          "score=incoming.score+1,weight=incoming.weight",NULL,0,&affected),
          TURBODB_STATUS_OK);
      check_equal(affected,2u);
      check_equal(insert_sql("INSERT INTO items VALUES(1,99,4.5) "
          "AS incoming(id,score,weight) ON DUPLICATE KEY UPDATE score=score+1",
          NULL,0,&affected),TURBODB_STATUS_OK);
      check_equal(affected,2u);
      query_open(&owner,"items","SELECT score,weight FROM items");
      orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,72);
      check_equal(row.values[1].data.double_value,3.5); query_close();
      const char *invalid[]={
        "INSERT INTO items VALUES(2,20,2.5) AS items ON DUPLICATE KEY UPDATE score=1",
        "INSERT INTO items VALUES(2,20,2.5) AS incoming(x,x,w) ON DUPLICATE KEY UPDATE score=1",
        "INSERT INTO items VALUES(2,20,2.5) AS incoming(x,y) ON DUPLICATE KEY UPDATE score=1",
        "INSERT INTO items VALUES(1,20,2.5) AS incoming(x,y,w) ON DUPLICATE KEY UPDATE score=incoming.missing"
      };
      for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        affected=SIZE_MAX; put_calls=0;
        check_equal(insert_sql(invalid[i],NULL,0,&affected),TURBODB_STATUS_SQL_ERROR);
        check_equal(affected,SIZE_MAX); check_equal(put_calls,0u);
      }
    }
    it("materializes INSERT SELECT and keeps sequential writes atomic") {
      const turbodb_value_t weights[] = {turbodb_f64(1.5),turbodb_f64(2.5)};
      size_t affected = SIZE_MAX;
      check_equal(insert_sql("INSERT INTO items(id,score,weight) VALUES"
          "(1,2,?),(2,3,?)",weights,2,&affected),TURBODB_STATUS_OK);
      check_equal(affected,2u);
      const turbodb_value_t increment = turbodb_i64(5);
      const turbodb_status_t selected = insert_sql("INSERT INTO items(id,score,weight) "
          "SELECT id+10,score+?,weight FROM items WHERE id=1",
          &increment,1,&affected);
      if (selected != TURBODB_STATUS_OK) info("INSERT SELECT: %s",error.message);
      check_equal(selected,TURBODB_STATUS_OK);
      check_equal(affected,1u);
      const turbodb_value_t update[] = {turbodb_i64(10),turbodb_i64(1)};
      check_equal(insert_sql("INSERT INTO items(id,score,weight) "
          "SELECT id,score+?,weight FROM items WHERE id=1 "
          "ON DUPLICATE KEY UPDATE score=VALUES(score)+?",
          update,2,&affected),TURBODB_STATUS_OK);
      check_equal(affected,2u);
      check_equal(insert_sql("INSERT IGNORE INTO items(id,score,weight) "
          "SELECT id,score,weight FROM items",NULL,0,&affected),TURBODB_STATUS_OK);
      check_equal(affected,0u);
      check_equal(insert_sql("INSERT INTO items(id,score,weight) "
          "SELECT id,score,weight FROM items WHERE FALSE",NULL,0,&affected),
          TURBODB_STATUS_OK);
      check_equal(affected,0u);
      affected = SIZE_MAX;
      check_equal(insert_sql("INSERT INTO items(id,score,weight) "
          "SELECT 20,200,weight FROM items WHERE id=1 UNION ALL "
          "SELECT 1,201,weight FROM items WHERE id=1",NULL,0,&affected),
          TURBODB_STATUS_CONSTRAINT);
      check_equal(affected,SIZE_MAX);
      query_open(&owner,"items","SELECT id,score,weight FROM items");
      orm_sql_scan_row row = next();
      check_equal(row.values[0].data.int64_value,1);
      check_equal(row.values[1].data.int64_value,13);
      row = next(); check_equal(row.values[0].data.int64_value,2);
      row = next(); check_equal(row.values[0].data.int64_value,11);
      check_equal(next().state,ORM_SQL_SCAN_DONE); query_close();
      check_equal(insert_sql("INSERT INTO items(id,score,weight) "
          "SELECT id,score FROM items",NULL,0,&affected),TURBODB_STATUS_SQL_ERROR);
      check_equal(insert_sql("INSERT INTO items(id,score,weight) "
          "SELECT id,weight,weight FROM items",NULL,0,&affected),TURBODB_STATUS_CONSTRAINT);
    }
    it("replaces primary conflicts across VALUES SET and SELECT after reopen") {
      create_table("CREATE TABLE replace_items (id BIGINT PRIMARY KEY,a BIGINT NOT NULL,"
          "b BIGINT NOT NULL)");
      size_t affected = SIZE_MAX;
      check_equal(insert_sql("INSERT INTO replace_items VALUES(1,10,100),(2,20,200)",
          NULL,0,&affected),TURBODB_STATUS_OK);
      check_equal(affected,2u);
      check_equal(insert_sql("REPLACE INTO replace_items VALUES(1,11,101)",
          NULL,0,&affected),TURBODB_STATUS_OK);
      check_equal(affected,2u);
      check_equal(insert_sql("REPLACE INTO replace_items SET id=3,a=30,b=300",
          NULL,0,&affected),TURBODB_STATUS_OK);
      check_equal(affected,1u);
      check_equal(insert_sql("REPLACE INTO replace_items SELECT 2,21,201",
          NULL,0,&affected),TURBODB_STATUS_OK);
      check_equal(affected,2u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK);
      reopen(); query_open(&owner,"replace_items","SELECT id,a,b FROM replace_items ORDER BY id");
      orm_sql_scan_row row = next();
      check_equal(row.values[0].data.int64_value,1);
      check_equal(row.values[1].data.int64_value,11);
      check_equal(row.values[2].data.int64_value,101);
      row = next(); check_equal(row.values[0].data.int64_value,2);
      check_equal(row.values[1].data.int64_value,21);
      check_equal(row.values[2].data.int64_value,201);
      row = next(); check_equal(row.values[0].data.int64_value,3);
      check_equal(row.values[1].data.int64_value,30);
      check_equal(row.values[2].data.int64_value,300);
      check_equal(next().state,ORM_SQL_SCAN_DONE); query_close();
    }
    it("rolls back REPLACE conflict deletion and insertion faults") {
      create_table("CREATE TABLE replace_faults (id BIGINT PRIMARY KEY,score BIGINT NOT NULL)");
      size_t affected = SIZE_MAX;
      check_equal(insert_sql("INSERT INTO replace_faults VALUES(1,10)",
          NULL,0,&affected),TURBODB_STATUS_OK);
      const uint64_t stamp = version(&owner,"replace_faults");
      for (size_t point = 0; point <= 3; ++point) {
        faults_clear(); affected = SIZE_MAX;
        if (!point) fail_delete = 1; else fail_put = point;
        check_equal(insert_sql("REPLACE INTO replace_faults VALUES(1,99)",
            NULL,0,&affected),TURBODB_STATUS_DATASTORE_ERROR);
        check_equal(affected,SIZE_MAX); check_false(owner.failed); faults_clear();
        check_equal(version(&owner,"replace_faults"),stamp);
        query_open(&owner,"replace_faults","SELECT id,score FROM replace_faults");
        orm_sql_scan_row row = next();
        check_equal(row.values[0].data.int64_value,1);
        check_equal(row.values[1].data.int64_value,10);
        check_equal(next().state,ORM_SQL_SCAN_DONE); query_close();
      }
    }
    it("applies implicit NULL defaults across every INSERT candidate form") {
      const turbodb_value_t weights[] = {turbodb_f64(1.5),turbodb_f64(2.5),
          turbodb_f64(3.5),turbodb_f64(4.5),turbodb_f64(5.5),turbodb_f64(9.5)};
      size_t affected = SIZE_MAX;
      check_equal(insert_sql("INSERT INTO items VALUES(1,10,?)",
          weights,1,&affected),TURBODB_STATUS_OK);
      check_equal(affected,1u);
      check_equal(insert_sql("INSERT INTO items(weight,id) VALUES(?,2)",
          weights+1,1,&affected),TURBODB_STATUS_OK);
      check_equal(insert_sql("INSERT INTO items SET id=3,weight=?",
          weights+2,1,&affected),TURBODB_STATUS_OK);
      check_equal(insert_sql("INSERT INTO items(id,weight) SELECT 4,?",
          weights+3,1,&affected),TURBODB_STATUS_OK);
      check_equal(insert_sql("INSERT INTO items(id,weight) VALUES(5,5.5)",
          NULL,0,&affected),TURBODB_STATUS_OK);
      check_equal(insert_sql("INSERT INTO items(id,score,weight) VALUES(1,99,?) "
          "ON DUPLICATE KEY UPDATE score=DEFAULT",weights+5,1,&affected),
          TURBODB_STATUS_OK);
      check_equal(affected,2u);
      query_open(&owner,"items","SELECT id,score,weight FROM items ORDER BY id");
      for (int64_t id = 1; id <= 5; ++id) {
        const orm_sql_scan_row row = next();
        check_equal(row.values[0].data.int64_value,id);
        check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
        check_equal(row.values[2].data.double_value,weights[id-1].data.double_value);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE); query_close();
    }
    it("rejects missing required defaults and target shape errors before writes") {
      const char *sql[] = {"INSERT INTO items(id) VALUES(1)",
        "INSERT INTO items SET id=1", "INSERT INTO items() VALUES()",
        "INSERT INTO items(id,weight) VALUES(1,DEFAULT)",
        "INSERT INTO items() VALUES(1)", "INSERT INTO items VALUES(1,2)",
        "INSERT INTO items() SELECT 1", "INSERT INTO items(id) SELECT 1"};
      const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for (size_t i = 0; i < sizeof(sql)/sizeof(sql[0]); ++i) {
        size_t affected = SIZE_MAX; put_calls = 0;
        check_equal(insert_sql(sql[i],NULL,0,&affected),TURBODB_STATUS_SQL_ERROR);
        check_equal(affected,SIZE_MAX); check_equal(put_calls,0u);
        check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],base);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
      }
    }
    it("rejects unsupported statement forms before writes") {
      const char *sql[] = {
        "INSERT LOW_PRIORITY INTO items VALUES (1,2,3)",
        "INSERT INTO main.items VALUES (1,2,3)", "INSERT INTO items (id,score,weight) VALUES (1,2,3); INSERT INTO items (id,score,weight) VALUES (2,3,4)",
        "SELECT 1"};
      size_t unchanged = SIZE_MAX; put_calls = 0;
      check_equal(insert_sql("INSERT INTO items (id,score,weight) VALUES (1,id,3)", NULL, 0, &unchanged), TURBODB_STATUS_SQL_ERROR);
      check_equal(unchanged, SIZE_MAX); check_equal(put_calls, 0u);
      const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
        size_t affected = SIZE_MAX; put_calls = 0;
        check_equal(insert_sql(sql[i], NULL, 0, &affected), TURBODB_STATUS_UNSUPPORTED);
        check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
      }
    }
    it("reports column and row shape errors without staging earlier rows") {
      const char *sql[] = {"INSERT INTO absent (id,score,weight) VALUES (1,2,3)",
        "INSERT INTO items (id,id,weight) VALUES (1,2,3)",
        "INSERT INTO items (id,missing,weight) VALUES (1,2,3)",
        "INSERT INTO items SET id=1,id=2,weight=3",
        "INSERT INTO items SET id=1,missing=2,weight=3",
        "INSERT INTO items (id,score,weight) VALUES (1,2,?),(2,3)", "INSERT INTO items (id,score,weight) VALUES (1,2,?,4)"};
      const turbodb_value_t weight = turbodb_f64(1.5);
      for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
        size_t affected = SIZE_MAX; put_calls = 0;
        check_equal(insert_sql(sql[i], i >= 5 ? &weight : NULL, i >= 5 ? 1 : 0, &affected), TURBODB_STATUS_SQL_ERROR);
        check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u); check_equal(version(&owner, "items"), 1u);
      }
    }
    it("converts booleans and rejects later invalid assignments arithmetic overflow and invalid dead branch parameters") {
      const turbodb_value_t weights[] = {turbodb_f64(1.5), turbodb_f64(2.5)};
      size_t affected=SIZE_MAX;
      check_equal(insert_sql("INSERT INTO items (id,score,weight) VALUES (1,2,?),(2,TRUE,?)",
          weights,sizeof(weights)/sizeof(weights[0]),&affected),TURBODB_STATUS_OK);
      check_equal(affected,2u);
      const char *sql[] = {"INSERT INTO items (id,score,weight) VALUES (3,2,?),(NULL,3,?)",
        "INSERT INTO items (id,score,weight) VALUES (1,2,?),(2,9223372036854775807+1,?)",
        "INSERT INTO items (id,score,weight) VALUES (1,2,?),(2,'text',?)"};
      for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
        size_t failed_affected = SIZE_MAX; put_calls = 0;
        const turbodb_status_t status = insert_sql(sql[i], weights,
            sizeof(weights) / sizeof(weights[0]), &failed_affected);
        check_not_equal(status, TURBODB_STATUS_OK);
        check_equal(failed_affected, SIZE_MAX); check_equal(put_calls, 0u);
        check_equal(version(&owner, "items"), 2u);
      }
      const turbodb_value_t invalid[] = {turbodb_f64(INFINITY), turbodb_f64(1.5)}; affected = SIZE_MAX;
      check_equal(insert_sql("INSERT INTO items (id,score,weight) VALUES (1,2,CASE WHEN FALSE THEN ? ELSE ? END)",
          invalid, sizeof(invalid) / sizeof(invalid[0]), &affected), TURBODB_STATUS_TYPE_ERROR);
      check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u);
    }
    it("requires exact parameter count and rejects SQLite documents and invalid arguments") {
      const char *sql = "INSERT INTO items (id,score,weight) VALUES (1,2,?)"; const turbodb_value_t weight = turbodb_f64(1.5);
      size_t affected = SIZE_MAX;
      check_equal(insert_sql(sql, NULL, 0, &affected), TURBODB_STATUS_SQL_ERROR);
      check_equal(insert_sql("INSERT INTO items (id,score,weight) VALUES (1,2,3)", &weight, 1, &affected), TURBODB_STATUS_SQL_ERROR);
      sqlparser_document *doc = NULL; sqlparser_error parse_error; const sqlparser_options options = {SQLPARSER_SQLITE, false};
      check_equal(sqlparser_parse_with_options(sql, strlen(sql), &options, NULL, &doc, &parse_error), SQLPARSER_OK);
      check_equal(orm_tidesdb_sql_insert_execute(doc, &owner, &weight, 1, DEPTH, 0, false, NULL, &affected, &error), TURBODB_STATUS_UNSUPPORTED);
      check_equal(orm_tidesdb_sql_insert_execute(doc, &owner, NULL, 1, DEPTH, 0, false, NULL, &affected, &error), TURBODB_STATUS_INVALID_ARGUMENT);
      check_equal(orm_tidesdb_sql_insert_execute(doc, &owner, &weight, 1, 0, 0, false, NULL, &affected, &error), TURBODB_STATUS_INVALID_ARGUMENT);
      sqlparser_document_destroy(doc); check_equal(affected, SIZE_MAX);
    }
    it("rejects both in-batch and preexisting duplicate keys with zero native puts") {
      check_equal(insert_id(&owner, "items", 9), TURBODB_STATUS_OK);
      const turbodb_value_t weights[] = {turbodb_f64(1.5), turbodb_f64(2.5)};
      const char *sql[] = {"INSERT INTO items (id,score,weight) VALUES (1,2,?),(1,3,?)", "INSERT INTO items (id,score,weight) VALUES (1,2,?),(9,3,?)"};
      for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
        size_t affected = SIZE_MAX; put_calls = 0;
        check_equal(insert_sql(sql[i], weights, sizeof(weights) / sizeof(weights[0]), &affected), TURBODB_STATUS_CONSTRAINT);
        check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u); check_equal(version(&owner, "items"), 2u);
        query_open(&owner, "items", "SELECT id FROM items"); check_equal(next().values[0].data.int64_value, 9);
        check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
      }
    }
    it("rolls back every partial native batch including the final version put") {
      check_equal(insert_id(&owner, "items", 9), TURBODB_STATUS_OK);
      const turbodb_value_t weights[] = {turbodb_f64(1.5), turbodb_f64(2.5), turbodb_f64(3.5)};
      const char *sql = "INSERT INTO items (id,score,weight) VALUES (1,2,?),(2,3,?),(3,4,?)";
      const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      enum { BATCH_WRITES = COLUMNS + 1 };
      for (size_t point = 1; point <= BATCH_WRITES; ++point) {
        size_t affected = SIZE_MAX; put_calls = 0; fail_put = point;
        check_equal(insert_sql(sql, weights, COLUMNS, &affected), TURBODB_STATUS_DATASTORE_ERROR);
        faults_clear(); check_equal(affected, SIZE_MAX); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
        check_equal(version(&owner, "items"), 2u);
        query_open(&owner, "items", "SELECT id FROM items"); check_equal(next().values[0].data.int64_value, 9);
        check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
      }
      size_t affected = 0; check_equal(insert_sql(sql, weights, COLUMNS, &affected), TURBODB_STATUS_OK);
      check_equal(affected, COLUMNS); check_equal(version(&owner, "items"), 3u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); owner_begin(&owner);
      query_open(&owner, "items", "SELECT id FROM items"); check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("refunds every fixed workspace allocation failure before any native put") {
      const char *sql = "INSERT INTO items (id,score,weight) VALUES (1,?+1,?),(2,?*2,?)";
      const turbodb_value_t parameters[] = {turbodb_i64(2), turbodb_f64(1.5), turbodb_i64(3), turbodb_f64(2.5)};
      size_t affected = 0; reserves = resizes = 0;
      check_equal(insert_sql(sql, parameters, sizeof(parameters) / sizeof(parameters[0]), &affected), TURBODB_STATUS_OK);
      const size_t reserve_points = reserves, resize_points = resizes;
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); owner_begin(&owner);
      const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for (unsigned pass = 0; pass < 2; ++pass) {
        const size_t points = pass ? resize_points : reserve_points;
        for (size_t point = 1; point <= points; ++point) {
          faults_clear(); affected = SIZE_MAX; if (pass) fail_resize = point; else fail_reserve = point;
          check_equal(insert_sql(sql, parameters, sizeof(parameters) / sizeof(parameters[0]), &affected), TURBODB_STATUS_OUT_OF_MEMORY);
          check_equal(put_calls, 0u); check_equal(affected, SIZE_MAX); check_false(owner.failed); faults_clear();
          check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
          check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
        }
      }
      check_equal(version(&owner, "items"), 1u);
    }
    it("refunds IGNORE conflict work and rolls back earlier candidates on allocation failure") {
      const char *sql = "INSERT IGNORE INTO items (id,score,weight) VALUES (1,10,?),(1,20,?)";
      const turbodb_value_t weights[] = {turbodb_f64(1.5), turbodb_f64(2.5)};
      size_t affected = SIZE_MAX; reserves = resizes = 0;
      check_equal(insert_sql(sql, weights, 2, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u);
      const size_t reserve_points = reserves, resize_points = resizes;
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
      owner_begin(&owner); const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for (unsigned pass = 0; pass < 2; ++pass) {
        const size_t points = pass ? resize_points : reserve_points;
        for (size_t point = 1; point <= points; ++point) {
          faults_clear(); affected = SIZE_MAX;
          if (pass) fail_resize = point; else fail_reserve = point;
          check_equal(insert_sql(sql, weights, 2, &affected), TURBODB_STATUS_OUT_OF_MEMORY);
          check_equal(affected, SIZE_MAX); check_false(owner.failed); faults_clear();
          check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
          check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
        }
      }
      fail_savepoint = true; affected = SIZE_MAX; put_calls = 0;
      check_equal(insert_sql(sql, weights, 2, &affected), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(put_calls, 0u); check_equal(affected, SIZE_MAX); check_false(owner.failed);
      faults_clear(); check_equal(version(&owner, "items"), 1u);
      query_open(&owner, "items", "SELECT id FROM items"); check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("refunds INSERT SELECT materialization and rolls back allocation failures") {
      const char *sql = "INSERT INTO items(id,score,weight) "
          "SELECT 1,10,? UNION ALL SELECT 2,20,?";
      const turbodb_value_t weights[] = {turbodb_f64(1.5),turbodb_f64(2.5)};
      size_t affected = SIZE_MAX; reserves = resizes = 0;
      check_equal(insert_sql(sql,weights,2,&affected),TURBODB_STATUS_OK);
      check_equal(affected,2u);
      const size_t reserve_points = reserves, resize_points = resizes;
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK);
      owner_begin(&owner); const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for (unsigned pass = 0; pass < 2; ++pass) {
        const size_t points = pass ? resize_points : reserve_points;
        for (size_t point = 1; point <= points; ++point) {
          faults_clear(); affected = SIZE_MAX;
          if (pass) fail_resize = point; else fail_reserve = point;
          check_equal(insert_sql(sql,weights,2,&affected),TURBODB_STATUS_OUT_OF_MEMORY);
          check_equal(affected,SIZE_MAX); check_false(owner.failed); faults_clear();
          check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],base);
          check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
        }
      }
      fail_savepoint = true; affected = SIZE_MAX; put_calls = 0;
      check_equal(insert_sql(sql,weights,2,&affected),TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(put_calls,0u); check_equal(affected,SIZE_MAX); check_false(owner.failed);
      faults_clear(); check_equal(version(&owner,"items"),1u);
      query_open(&owner,"items","SELECT id FROM items");
      check_equal(next().state,ORM_SQL_SCAN_DONE); query_close();
    }
    it("rejects exhausted row work execution and write budgets without partial batches") {
      const char *sql = "INSERT INTO items (id,score,weight) VALUES (1,2,?),(2,3,?)";
      const turbodb_value_t weights[] = {turbodb_f64(1.5), turbodb_f64(2.5)};
      const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_WORK_BYTES, ORM_SQL_BUDGET_MATERIALIZED_ROWS,
          ORM_SQL_BUDGET_EXECUTION_STEPS, ORM_SQL_BUDGET_WRITE_ROWS, ORM_SQL_BUDGET_WRITE_BYTES};
      const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); ++i) {
        const orm_sql_budget_resource resource = resources[i]; const uint64_t limit = budget.limits.statement.value[resource];
        budget.limits.statement.value[resource] = budget.used.value[resource]; size_t affected = SIZE_MAX; put_calls = 0;
        check_equal(insert_sql(sql, weights, sizeof(weights) / sizeof(weights[0]), &affected), TURBODB_STATUS_LIMIT_EXCEEDED);
        check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u);
        budget.limits.statement.value[resource] = limit;
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
      }
      check_equal(version(&owner, "items"), 1u);
    }
    it("blocks SQL writes while a source lives and checks typed batch dimensions") {
      query_open(&owner, "items", "SELECT id FROM items LIMIT 0");
      const turbodb_value_t weight = turbodb_f64(1.5); size_t affected = SIZE_MAX;
      check_equal(insert_sql("INSERT INTO items (id,score,weight) VALUES (1,2,?)", &weight, 1, &affected), TURBODB_STATUS_BUSY);
      check_equal(affected, SIZE_MAX); query_close();
      check_equal(orm_tidesdb_sql_relation_insert_rows(&owner, vstr_from_cstr("items"), &weight, SIZE_MAX, COLUMNS, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(orm_tidesdb_sql_relation_insert_rows(&owner, vstr_from_cstr("items"), &weight,
          SIZE_MAX / sizeof(weight) / COLUMNS + 1, COLUMNS, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(orm_tidesdb_sql_relation_insert_rows(&owner, vstr_from_cstr("items"), &weight, 0, COLUMNS, &error), TURBODB_STATUS_INVALID_ARGUMENT);
      check_equal(insert_sql("INSERT INTO items (id,score,weight) VALUES (1,2,?)", &weight, 1, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u);
    }
    it("distinguishes failed savepoint admission from poisoned batch cleanup") {
      const char *sql = "INSERT INTO items (id,score,weight) VALUES (1,2,?),(2,3,?)";
      const turbodb_value_t weights[] = {turbodb_f64(1.5), turbodb_f64(2.5)}; size_t affected = SIZE_MAX;
      fail_savepoint = true; put_calls = 0;
      check_equal(insert_sql(sql, weights, sizeof(weights) / sizeof(weights[0]), &affected), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(put_calls, 0u); check_false(owner.failed); faults_clear();
      for (unsigned pass = 0; pass < 2; ++pass) {
        if (pass) { fail_rollback = true; fail_put = 2; } else fail_release = true;
        check_equal(insert_sql(sql, weights, sizeof(weights) / sizeof(weights[0]), &affected), TURBODB_STATUS_DATASTORE_ERROR);
        check_true(owner.failed); check_equal(affected, SIZE_MAX); check_contains(error.message, "cleanup");
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u); faults_clear();
        check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_INVALID_STATE);
        check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); owner_begin(&owner);
        check_equal(version(&owner, "items"), 1u);
        query_open(&owner, "items", "SELECT id FROM items"); check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
      }
    }
    it("accounts for both materialized representations and bounds expression plans") {
      const char *sql = "INSERT INTO items (id,score,weight) VALUES (1,2,?),(2,3,?)";
      const turbodb_value_t weights[] = {turbodb_f64(1.5), turbodb_f64(2.5)};
      const uint64_t rows_limit = budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
      enum { ROWS = 2, REPRESENTATIONS = 2 };
      size_t affected = SIZE_MAX; put_calls = 0;
      budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS] = ROWS * REPRESENTATIONS - 1;
      check_equal(insert_sql(sql, weights, ROWS, &affected), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(put_calls, 0u); check_equal(affected, SIZE_MAX);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
      budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS] = ROWS * REPRESENTATIONS;
      check_equal(insert_sql(sql, weights, ROWS, &affected), TURBODB_STATUS_OK); check_equal(affected, ROWS);
      check_equal(budget.peak.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], ROWS * REPRESENTATIONS);
      budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS] = rows_limit;
      const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_AST_NODES, ORM_SQL_BUDGET_PLAN_NODES};
      for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); ++i) {
        const orm_sql_budget_resource resource = resources[i]; const uint64_t limit = budget.limits.statement.value[resource];
        budget.limits.statement.value[resource] = budget.used.value[resource]; affected = SIZE_MAX; put_calls = 0;
        check_equal(insert_sql(sql, weights, ROWS, &affected), TURBODB_STATUS_LIMIT_EXCEEDED);
        check_equal(put_calls, 0u); check_equal(affected, SIZE_MAX);
        budget.limits.statement.value[resource] = limit;
      }
    }
    it("evaluates literal-only rows without a parameter workspace") {
      create_table("CREATE TABLE integers (id BIGINT PRIMARY KEY, score BIGINT)"); size_t affected = SIZE_MAX;
      check_equal(insert_sql("INSERT INTO integers (score,id) VALUES (COALESCE(NULL,10)+2,1),"
          "(CASE WHEN TRUE THEN -4 ELSE 9 END,2)", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 2u); check_equal(version(&owner, "integers"), 2u);
      query_open(&owner, "integers", "SELECT score FROM integers");
      check_equal(next().values[0].data.int64_value, 12); check_equal(next().values[0].data.int64_value, -4);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("reports no-op duplicate updates as found when explicitly requested") {
      const turbodb_value_t weight=turbodb_f64(1.5); size_t affected=SIZE_MAX;
      check_equal(insert_sql("INSERT INTO items VALUES(1,20,?)",
          &weight,1,&affected),TURBODB_STATUS_OK);
      check_equal(insert_sql_mode("INSERT INTO items VALUES(1,99,?) "
          "ON DUPLICATE KEY UPDATE score=score",&weight,1,true,&affected),
          TURBODB_STATUS_OK);
      check_equal(affected,1u);
      check_equal(insert_sql_mode("INSERT INTO items VALUES(1,99,?) "
          "ON DUPLICATE KEY UPDATE score=score+1",&weight,1,true,&affected),
          TURBODB_STATUS_OK);
      check_equal(affected,2u);
    }
  }

  group("SQL UPDATE and DELETE") {
    it("reports selected UPDATE matches in found-rows mode") {
      seed_items(); size_t affected=SIZE_MAX;
      const uint64_t stamp=version(&owner,"items");
      check_equal(change_sql_mode("UPDATE items SET score=score ORDER BY id LIMIT 2",
          NULL,0,true,&affected),TURBODB_STATUS_OK);
      check_equal(affected,2u); check_equal(version(&owner,"items"),stamp);
      check_equal(change_sql_mode("DELETE FROM items WHERE id=1",NULL,0,true,
          &affected),TURBODB_STATUS_OK);
      check_equal(affected,1u);
    }
    it("updates matching rows and persists changes with one version increment") {
      seed_items(); size_t affected = SIZE_MAX;
      const turbodb_value_t parameters[] = {turbodb_i64(5), turbodb_i64(2)};
      check_equal(change_sql("UPDATE items SET score=score+? WHERE items.id>=?", parameters,
          sizeof(parameters) / sizeof(parameters[0]), &affected), TURBODB_STATUS_OK);
      check_equal(affected, 2u); check_equal(version(&owner, "items"), 5u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); reopen();
      query_open(&owner, "items", "SELECT score FROM items");
      check_equal(next().values[0].data.int64_value, 20);
      check_equal(next().values[0].data.int64_value, 25); check_equal(next().values[0].data.int64_value, 25);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("evaluates assignments left to right including repeated targets") {
      create_table("CREATE TABLE metrics (id BIGINT PRIMARY KEY, a BIGINT, b BIGINT)");
      size_t affected = 0;
      check_equal(insert_sql("INSERT INTO metrics (id,a,b) VALUES (1,10,0),(2,20,0)", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(change_sql("UPDATE metrics SET a=a+1,b=a*2,a=b+1 WHERE id=1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); query_open(&owner, "metrics", "SELECT a,b FROM metrics");
      orm_sql_scan_row row = next(); check_equal(row.values[0].data.int64_value, 23); check_equal(row.values[1].data.int64_value, 22);
      row = next(); check_equal(row.values[0].data.int64_value, 20); check_equal(row.values[1].data.int64_value, 0);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("deletes filtered rows and persists tombstones after reopen") {
      seed_items(); const turbodb_value_t parameter = turbodb_i64(2); size_t affected = SIZE_MAX;
      check_equal(change_sql("DELETE FROM items WHERE id<>?", &parameter, 1, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 2u); check_equal(version(&owner, "items"), 5u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); reopen();
      query_open(&owner, "items", "SELECT id FROM items"); check_equal(next().values[0].data.int64_value, 2);
      check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
      check_equal(change_sql("DELETE FROM items", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); check_equal(change_sql("DELETE FROM items", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 0u); check_equal(version(&owner, "items"), 6u);
    }
    it("keeps no-op updates and unknown WHERE results free of writes") {
      seed_items(); size_t affected = SIZE_MAX;
      const char *sql[] = {"UPDATE items SET score=score", "UPDATE items SET score=score+1,score=score-1",
          "UPDATE items SET score=score+1 WHERE NULL", "DELETE FROM items WHERE score=NULL",
          "DELETE FROM items WHERE FALSE", "UPDATE items SET score=1 WHERE id=100"};
      for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
        put_calls = delete_calls = 0;
        check_equal(change_sql(sql[i], NULL, 0, &affected), TURBODB_STATUS_OK); check_equal(affected, 0u);
        check_equal(put_calls, 0u); check_equal(delete_calls, 0u); check_equal(version(&owner, "items"), 4u);
      }
      const turbodb_value_t row[] = {turbodb_i64(4), turbodb_null(), turbodb_f64(-0.0)};
      check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr("items"), row, COLUMNS, &error), TURBODB_STATUS_OK);
      const turbodb_value_t zero = turbodb_f64(0.0); put_calls = 0;
      check_equal(change_sql("UPDATE items SET score=NULL,weight=? WHERE id=4", &zero, 1, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 0u); check_equal(put_calls, 0u); check_equal(version(&owner, "items"), 5u);
      query_open(&owner, "items", "SELECT weight FROM items WHERE id=4"); check_true(signbit(next().values[0].data.double_value));
    }
    it("binds invalid columns types and parameters even on empty tables") {
      size_t affected = SIZE_MAX; const turbodb_value_t invalid = turbodb_f64(INFINITY);
      check_equal(change_sql("UPDATE items SET missing=1 WHERE FALSE", NULL, 0, &affected), TURBODB_STATUS_SQL_ERROR);
      check_equal(change_sql("DELETE FROM items WHERE missing=1", NULL, 0, &affected), TURBODB_STATUS_SQL_ERROR);
      check_equal(change_sql("UPDATE items SET weight=1 WHERE FALSE", NULL, 0,
          &affected), TURBODB_STATUS_OK);
      check_equal(change_sql("UPDATE items SET score=?", NULL, 0, &affected), TURBODB_STATUS_SQL_ERROR);
      check_equal(change_sql("DELETE FROM items WHERE FALSE AND weight=?", &invalid, 1, &affected), TURBODB_STATUS_TYPE_ERROR);
      check_equal(change_sql("UPDATE items SET score=1 WHERE 1", NULL, 0, &affected), TURBODB_STATUS_UNSUPPORTED);
      check_equal(affected, 0u); check_equal(version(&owner, "items"), 1u);
    }
    it("rejects unsupported ordering expressions offsets and statement forms") {
      seed_items(); const char *sql[] = {
          "UPDATE items SET score=1 ORDER BY id/2", "UPDATE items SET score=1 LIMIT 1 OFFSET 0",
          "UPDATE LOW_PRIORITY items SET score=1",
          "UPDATE main.items SET score=1",
          "DELETE FROM items LIMIT 0,1", "DELETE FROM items ORDER BY 1", "DELETE LOW_PRIORITY FROM items",
          "DELETE items FROM items", "SELECT 1", "DELETE FROM items; DELETE FROM other"};
      for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
        size_t affected = SIZE_MAX; put_calls = delete_calls = 0;
        check_equal(change_sql(sql[i], NULL, 0, &affected), TURBODB_STATUS_UNSUPPORTED);
        check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
        check_equal(version(&owner, "items"), 4u);
      }
    }
    it("applies explicit implicit and missing UPDATE defaults before writes") {
      create_table("CREATE TABLE update_defaults (id BIGINT PRIMARY KEY,"
          "score BIGINT NOT NULL DEFAULT -7,optional BIGINT,required DOUBLE NOT NULL)");
      const turbodb_value_t weight = turbodb_f64(1.5); size_t affected = SIZE_MAX;
      check_equal(insert_sql("INSERT INTO update_defaults(id,required) VALUES(1,?)",
          &weight,1,&affected),TURBODB_STATUS_OK);
      check_equal(change_sql("UPDATE update_defaults SET score=9,score=DEFAULT,"
          "optional=DEFAULT WHERE id=1",NULL,0,&affected),TURBODB_STATUS_OK);
      check_equal(affected,0u);
      put_calls=0; affected=SIZE_MAX;
      check_equal(change_sql("UPDATE update_defaults SET required=DEFAULT WHERE id=1",
          NULL,0,&affected),TURBODB_STATUS_SQL_ERROR);
      check_equal(affected,SIZE_MAX); check_equal(put_calls,0u);
      query_open(&owner,"update_defaults",
          "SELECT id,score,optional,required FROM update_defaults");
      orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,1);
      check_equal(row.values[1].data.int64_value,-7);
      check_equal(row.values[2].kind,TURBODB_VALUE_NULL);
      check_equal(row.values[3].data.double_value,1.5);
      check_equal(next().state,ORM_SQL_SCAN_DONE); query_close();
    }
    it("does not stage earlier rows when a later assignment overflows or violates NOT NULL") {
      create_table("CREATE TABLE extremes (id BIGINT PRIMARY KEY, score BIGINT NOT NULL)"); size_t affected = SIZE_MAX;
      check_equal(insert_sql("INSERT INTO extremes (id,score) VALUES (1,10),(2,9223372036854775807)", NULL, 0, &affected), TURBODB_STATUS_OK);
      const char *sql[] = {"UPDATE extremes SET score=score+1",
          "UPDATE extremes SET score=CASE WHEN id=1 THEN 11 ELSE NULL END"};
      for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
        affected = SIZE_MAX; put_calls = 0;
        check_equal(change_sql(sql[i], NULL, 0, &affected),
            i ? TURBODB_STATUS_CONSTRAINT : TURBODB_STATUS_LIMIT_EXCEEDED);
        check_equal(put_calls, 0u); check_equal(affected, SIZE_MAX); check_equal(version(&owner, "extremes"), 2u);
        query_open(&owner, "extremes", "SELECT score FROM extremes"); check_equal(next().values[0].data.int64_value, 10);
        check_equal(next().values[0].data.int64_value, INT64_MAX); query_close();
      }
    }
    it("rolls back every update put including version while preserving previous statements") {
      seed_items(); enum { WRITES = COLUMNS + 1 };
      for (size_t point = 1; point <= WRITES; ++point) {
        size_t affected = SIZE_MAX; faults_clear(); fail_put = point;
        check_equal(change_sql("UPDATE items SET score=score+id", NULL, 0, &affected), TURBODB_STATUS_DATASTORE_ERROR);
        faults_clear(); check_equal(affected, SIZE_MAX); check_false(owner.failed); check_equal(version(&owner, "items"), 4u);
        query_open(&owner, "items", "SELECT score FROM items");
        for (size_t i = 0; i < COLUMNS; ++i) check_equal(next().values[0].data.int64_value, 20);
        check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
      }
    }
    it("rolls back every delete and the final version write") {
      seed_items(); enum { WRITES = COLUMNS + 1 };
      for (size_t point = 1; point <= WRITES; ++point) {
        size_t affected = SIZE_MAX; faults_clear();
        if (point == WRITES) fail_put = 1; else fail_delete = point;
        check_equal(change_sql("DELETE FROM items", NULL, 0, &affected), TURBODB_STATUS_DATASTORE_ERROR);
        faults_clear(); check_equal(affected, SIZE_MAX); check_false(owner.failed); check_equal(version(&owner, "items"), 4u);
        query_open(&owner, "items", "SELECT id FROM items");
        for (int64_t i = 1; i <= COLUMNS; ++i) check_equal(next().values[0].data.int64_value, i);
        check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
      }
    }
    it("poisons owners on delete savepoint cleanup failure until full rollback") {
      seed_items(); size_t affected = SIZE_MAX; faults_clear(); fail_delete = 2; fail_rollback = true;
      check_equal(change_sql("DELETE FROM items", NULL, 0, &affected), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(affected, SIZE_MAX); check_true(owner.failed); check_contains(error.message, "cleanup"); faults_clear();
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_INVALID_STATE);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); owner_begin(&owner);
      query_open(&owner, "items", "SELECT id FROM items"); check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("refunds every update and delete fixed workspace allocation failure") {
      seed_items(); const char *sql[] = {"UPDATE items SET score=score+? WHERE id>=? LIMIT ?", "DELETE FROM items WHERE id>=? LIMIT ?",
          "UPDATE items SET score=score+? WHERE id>=? ORDER BY id DESC LIMIT ?", "DELETE FROM items WHERE id>=? ORDER BY score,id DESC LIMIT ?",
          "UPDATE items SET score=score+? WHERE id>=? ORDER BY CASE WHEN id=2 THEN 0 ELSE score-id END,id DESC LIMIT ?",
          "DELETE FROM items WHERE id>=? ORDER BY NULLIF(score-id,18),id DESC LIMIT ?",
          "UPDATE items SET id=id+?,score=id WHERE id>=? ORDER BY id DESC LIMIT ?"};
      const turbodb_value_t parameters[] = {turbodb_i64(1), turbodb_i64(1), turbodb_i64(2)};
      for (size_t command = 0; command < sizeof(sql) / sizeof(sql[0]); ++command) {
        size_t affected = SIZE_MAX; faults_clear();
        check_equal(change_sql(sql[command], parameters, command % 2 ? 2 : 3, &affected), TURBODB_STATUS_OK);
        const size_t reserve_points = reserves, resize_points = resizes;
        check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); owner_begin(&owner); seed_items();
        const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
        for (unsigned pass = 0; pass < 2; ++pass) {
          const size_t points = pass ? resize_points : reserve_points;
          for (size_t point = 1; point <= points; ++point) {
            faults_clear(); affected = SIZE_MAX; if (pass) fail_resize = point; else fail_reserve = point;
            check_equal(change_sql(sql[command], parameters, command % 2 ? 2 : 3, &affected), TURBODB_STATUS_OUT_OF_MEMORY);
            check_equal(put_calls, 0u); check_equal(delete_calls, 0u); check_equal(affected, SIZE_MAX); check_false(owner.failed);
            faults_clear(); check_equal(owner.active_sources, 0u);
            check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
            check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
          }
        }
      }
    }
    it("rejects exhausted read write work and materialization budgets before writes") {
      seed_items(); const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_WORK_BYTES, ORM_SQL_BUDGET_MATERIALIZED_ROWS,
          ORM_SQL_BUDGET_READ_ROWS, ORM_SQL_BUDGET_READ_BYTES, ORM_SQL_BUDGET_WRITE_ROWS,
          ORM_SQL_BUDGET_WRITE_BYTES, ORM_SQL_BUDGET_EXECUTION_STEPS, ORM_SQL_BUDGET_AST_NODES, ORM_SQL_BUDGET_PLAN_NODES};
      const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for (unsigned command = 0; command < 2; ++command) for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); ++i) {
        const orm_sql_budget_resource resource = resources[i]; const uint64_t limit = budget.limits.statement.value[resource];
        budget.limits.statement.value[resource] = budget.used.value[resource]; size_t affected = SIZE_MAX; faults_clear();
        check_equal(change_sql(command ? "DELETE FROM items WHERE id>0" : "UPDATE items SET score=score+1", NULL, 0, &affected), TURBODB_STATUS_LIMIT_EXCEEDED);
        check_equal(put_calls, 0u); check_equal(delete_calls, 0u); check_equal(affected, SIZE_MAX);
        budget.limits.statement.value[resource] = limit; check_equal(owner.active_sources, 0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
      }
      check_equal(version(&owner, "items"), 4u);
    }
    it("returns scan failures before any mutation including the second pass") {
      seed_items(); const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for (int point = FAIL_NEW; point <= FAIL_VALUE + 1; ++point) {
        faults_clear(); size_t affected = SIZE_MAX;
        if (point > FAIL_VALUE) fail_new_at = 2; else fail_iterator = point;
        check_equal(change_sql("UPDATE items SET score=score+1", NULL, 0, &affected), TURBODB_STATUS_DATASTORE_ERROR);
        check_equal(put_calls, 0u); check_equal(delete_calls, 0u); check_equal(affected, SIZE_MAX); faults_clear();
        check_equal(owner.active_sources, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
        check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); owner_begin(&owner); seed_items();
      }
    }
    it("blocks writes with live sources and rejects invalid input and SQLite documents") {
      seed_items(); query_open(&owner, "items", "SELECT id FROM items LIMIT 0"); size_t affected = SIZE_MAX;
      check_equal(change_sql("UPDATE items SET score=1", NULL, 0, &affected), TURBODB_STATUS_BUSY);
      check_equal(change_sql("DELETE FROM items", NULL, 0, &affected), TURBODB_STATUS_BUSY); query_close();
      sqlparser_document *doc = NULL; sqlparser_error parse_error; const sqlparser_options options = {SQLPARSER_SQLITE, false};
      const char *sql = "DELETE FROM items";
      check_equal(sqlparser_parse_with_options(sql, strlen(sql), &options, NULL, &doc, &parse_error), SQLPARSER_OK);
      check_equal(orm_tidesdb_sql_change_execute(doc, &owner, NULL, 0, DEPTH,
          0, false, NULL, &affected, &error), TURBODB_STATUS_UNSUPPORTED);
      check_equal(orm_tidesdb_sql_change_execute(doc, &owner, NULL, 1, DEPTH,
          0, false, NULL, &affected, &error), TURBODB_STATUS_INVALID_ARGUMENT);
      check_equal(orm_tidesdb_sql_change_execute(doc, &owner, NULL, 0, 0,
          0, false, NULL, &affected, &error), TURBODB_STATUS_INVALID_ARGUMENT);
      sqlparser_document_destroy(doc); check_equal(affected, SIZE_MAX);
    }
    it("detects concurrent table mutation after a no-op read and independent-table write") {
      seed_items(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
      owner_begin(&owner); owner_begin(&other); size_t affected = SIZE_MAX;
      check_equal(change_sql("UPDATE items SET score=score WHERE id=1", NULL, 0, &affected), TURBODB_STATUS_OK); check_equal(affected, 0u);
      const turbodb_value_t row[] = {turbodb_i64(1), turbodb_i64(99), turbodb_f64(1.5)};
      check_equal(orm_tidesdb_sql_relation_update_rows(&other, vstr_from_cstr("items"), row, 1, COLUMNS, &error), TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_OK);
      check_equal(insert_id(&owner, "other", 1), TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_BUSY);
    }
    it("requires existing distinct keys and validates typed delete and replacement rows") {
      seed_items(); const turbodb_value_t keys[] = {turbodb_i64(1), turbodb_i64(1)}, missing = turbodb_i64(99), wrong = turbodb_u64(1);
      put_calls = delete_calls = 0;
      check_equal(orm_tidesdb_sql_relation_delete_rows(&owner, vstr_from_cstr("items"), keys, 2, &error), TURBODB_STATUS_CONSTRAINT);
      check_equal(orm_tidesdb_sql_relation_delete_rows(&owner, vstr_from_cstr("items"), &missing, 1, &error), TURBODB_STATUS_CONSTRAINT);
      check_equal(orm_tidesdb_sql_relation_delete_rows(&owner, vstr_from_cstr("items"), &wrong, 1, &error), TURBODB_STATUS_TYPE_ERROR);
      const turbodb_value_t row[] = {missing, turbodb_i64(1), turbodb_f64(1.5)};
      check_equal(orm_tidesdb_sql_relation_update_rows(&owner, vstr_from_cstr("items"), row, 1, COLUMNS, &error), TURBODB_STATUS_CONSTRAINT);
      check_equal(put_calls, 0u); check_equal(delete_calls, 0u); check_equal(version(&owner, "items"), 4u);
    }
    it("rolls back successful updates and deletes together then permits reinsertion") {
      seed_items(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); owner_begin(&owner);
      size_t affected = SIZE_MAX;
      check_equal(change_sql("UPDATE items SET score=99", NULL, 0, &affected), TURBODB_STATUS_OK); check_equal(affected, COLUMNS);
      check_equal(change_sql("DELETE FROM items WHERE id=2", NULL, 0, &affected), TURBODB_STATUS_OK); check_equal(affected, 1u);
      check_equal(insert_id(&owner, "items", 2), TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); reopen();
      check_equal(version(&owner, "items"), 4u); query_open(&owner, "items", "SELECT score FROM items");
      for (size_t i = 0; i < COLUMNS; ++i) check_equal(next().values[0].data.int64_value, 20);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("counts matched and encoded rows separately and refunds a second-stage quota failure") {
      seed_items(); enum { CHANGED = COLUMNS, REPRESENTATIONS = 2 };
      const uint64_t limit = budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
      const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; size_t affected = SIZE_MAX;
      budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS] = CHANGED * REPRESENTATIONS - 1;
      put_calls = delete_calls = 0;
      check_equal(change_sql("DELETE FROM items", NULL, 0, &affected), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(put_calls, 0u); check_equal(delete_calls, 0u); check_equal(affected, SIZE_MAX);
      check_equal(owner.active_sources, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
      budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS] = CHANGED * REPRESENTATIONS;
      check_equal(change_sql("DELETE FROM items", NULL, 0, &affected), TURBODB_STATUS_OK); check_equal(affected, CHANGED);
      check_equal(budget.peak.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], CHANGED * REPRESENTATIONS);
      budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS] = limit;
    }
  }

  group("SQL UPDATE DELETE LIMIT") {
    it("counts unchanged matches toward UPDATE LIMIT") {
      seed_items(); size_t affected = SIZE_MAX; faults_clear();
      /* The fixture's native key order is known; SQL without ORDER BY promises none. */
      check_equal(change_sql("UPDATE items SET score=CASE WHEN id=1 THEN score ELSE 99 END LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 0u); check_equal(put_calls, 0u); check_equal(version(&owner, "items"), 4u);
      check_equal(change_sql("UPDATE items SET score=CASE WHEN id=1 THEN score ELSE 99 END LIMIT 2", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); check_equal(version(&owner, "items"), 5u);
      query_open(&owner, "items", "SELECT score FROM items");
      check_equal(next().values[0].data.int64_value, 20); check_equal(next().values[0].data.int64_value, 99);
      check_equal(next().values[0].data.int64_value, 20); check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("binds SET WHERE and LIMIT parameters in source order and skips nonmatches") {
      seed_items(); size_t affected = SIZE_MAX;
      const turbodb_value_t update[] = {turbodb_i64(7), turbodb_i64(2), turbodb_u64(1)};
      check_equal(change_sql("UPDATE items SET score=score+? WHERE id>=? LIMIT ?", update, 3, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u);
      const turbodb_value_t deletion[] = {turbodb_i64(27), turbodb_i64(1)};
      check_equal(change_sql("DELETE FROM items WHERE score=? LIMIT ?", deletion, 2, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); check_equal(version(&owner, "items"), 6u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); reopen();
      query_open(&owner, "items", "SELECT id,score FROM items");
      orm_sql_scan_row row = next(); check_equal(row.values[0].data.int64_value, 1); check_equal(row.values[1].data.int64_value, 20);
      row = next(); check_equal(row.values[0].data.int64_value, 3); check_equal(row.values[1].data.int64_value, 20);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("does not open data iterators or write for literal and parameter zero limits") {
      seed_items(); const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      const turbodb_value_t zero = turbodb_u64(0); size_t affected = SIZE_MAX; faults_clear(); fail_iterator = FAIL_NEW;
      check_equal(change_sql("UPDATE items SET score=score+1 LIMIT 0", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 0u);
      check_equal(change_sql("DELETE FROM items LIMIT ?", &zero, 1, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 0u); check_equal(new_calls, 0u); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
      faults_clear(); check_equal(version(&owner, "items"), 4u); check_equal(owner.active_sources, 0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
    }
    it("still validates names types parameters and unsupported assignments for LIMIT zero") {
      size_t affected = SIZE_MAX; const turbodb_value_t invalid = turbodb_f64(INFINITY);
      check_equal(change_sql("UPDATE items SET missing=1 LIMIT 0", NULL, 0, &affected), TURBODB_STATUS_SQL_ERROR);
      check_equal(change_sql("DELETE FROM missing LIMIT 0", NULL, 0, &affected), TURBODB_STATUS_SQL_ERROR);
      check_equal(change_sql("DELETE FROM items WHERE missing=1 LIMIT 0", NULL, 0, &affected), TURBODB_STATUS_SQL_ERROR);
      check_equal(change_sql("UPDATE items SET weight=1 LIMIT 0", NULL, 0,
          &affected), TURBODB_STATUS_OK);
      check_equal(change_sql("UPDATE items SET score=? LIMIT 0", NULL, 0, &affected), TURBODB_STATUS_SQL_ERROR);
      check_equal(change_sql("DELETE FROM items WHERE FALSE AND weight=? LIMIT 0", &invalid, 1, &affected), TURBODB_STATUS_TYPE_ERROR);
      check_equal(affected, 0u); check_equal(version(&owner, "items"), 1u);
      check_equal(change_sql("UPDATE items SET id=id LIMIT 0", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 0u);
    }
    it("rejects invalid LIMIT values without changing output or rows") {
      seed_items(); size_t affected = SIZE_MAX;
      const turbodb_value_t invalid[] = {turbodb_i64(-1), turbodb_null(), turbodb_f64(1.0), turbodb_bool(true), turbodb_text("1")};
      const char *sql[] = {"UPDATE items SET score=1 LIMIT ?", "DELETE FROM items LIMIT ?"};
      faults_clear();
      for (size_t command = 0; command < sizeof(sql) / sizeof(sql[0]); ++command) {
        for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
          check_equal(change_sql(sql[command], &invalid[i], 1, &affected), TURBODB_STATUS_TYPE_ERROR);
        check_equal(change_sql(sql[command], NULL, 0, &affected), TURBODB_STATUS_SQL_ERROR);
      }
      const char *unsupported[] = {"UPDATE items SET score=1 LIMIT -1", "DELETE FROM items LIMIT 1.5",
          "DELETE FROM items LIMIT 1+1", "UPDATE items SET score=1 LIMIT 0,1", "DELETE FROM items LIMIT 1 OFFSET 0"};
      for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i)
        check_equal(change_sql(unsupported[i], NULL, 0, &affected), TURBODB_STATUS_UNSUPPORTED);
      check_equal(change_sql("DELETE FROM items LIMIT 18446744073709551616", NULL, 0, &affected), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
      check_equal(version(&owner, "items"), 4u);
    }
    it("accepts full unsigned bounds without allocating space for the limit") {
      seed_items(); size_t affected = SIZE_MAX; const turbodb_value_t maximum = turbodb_u64(UINT64_MAX);
      check_equal(change_sql("UPDATE items SET score=21 LIMIT 18446744073709551615", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, COLUMNS);
      check_equal(change_sql("UPDATE items SET score=22 LIMIT ?", &maximum, 1, &affected), TURBODB_STATUS_OK);
      check_equal(affected, COLUMNS);
      check_equal(change_sql("DELETE FROM items WHERE FALSE LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 0u);
      check_equal(change_sql("DELETE FROM items LIMIT 9223372036854775808", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, COLUMNS);
      check_equal(change_sql("DELETE FROM items LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK); check_equal(affected, 0u);
      check_equal(version(&owner, "items"), 7u);
    }
    it("stops both scans before reading or evaluating the next row") {
      create_table("CREATE TABLE extremes (id BIGINT PRIMARY KEY, score BIGINT NOT NULL)"); size_t affected = SIZE_MAX;
      check_equal(insert_sql("INSERT INTO extremes (id,score) VALUES (1,10),(2,9223372036854775807)", NULL, 0, &affected), TURBODB_STATUS_OK);
      faults_clear(); fail_iterator = FAIL_NEXT;
      check_equal(change_sql("UPDATE extremes SET score=score+1 LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); check_equal(new_calls, 2u); faults_clear(); fail_iterator = FAIL_NEXT;
      check_equal(change_sql("DELETE FROM extremes LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); check_equal(new_calls, 2u); faults_clear();
      query_open(&owner, "extremes", "SELECT score FROM extremes");
      check_equal(next().values[0].data.int64_value, INT64_MAX); check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("rolls back limited batches at every data and version write failure") {
      seed_items(); enum { ROWS = 2, WRITES = ROWS + 1 };
      for (unsigned deletion = 0; deletion < 2; ++deletion) for (size_t point = 1; point <= WRITES; ++point) {
        size_t affected = SIZE_MAX; faults_clear();
        if (!deletion) fail_put = point;
        else if (point == WRITES) fail_put = 1;
        else fail_delete = point;
        check_equal(change_sql(deletion ? "DELETE FROM items LIMIT 2" : "UPDATE items SET score=99 LIMIT 2",
            NULL, 0, &affected), TURBODB_STATUS_DATASTORE_ERROR);
        faults_clear(); check_equal(affected, SIZE_MAX); check_false(owner.failed); check_equal(version(&owner, "items"), 4u);
        query_open(&owner, "items", "SELECT score FROM items");
        for (size_t i = 0; i < COLUMNS; ++i) check_equal(next().values[0].data.int64_value, 20);
        check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
      }
    }
    it("does not turn a materialization quota into a smaller successful LIMIT") {
      seed_items(); enum { MATCHES = 2, REPRESENTATIONS = 2 };
      const uint64_t limit = budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
      const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for (unsigned deletion = 0; deletion < 2; ++deletion) for (size_t quota = 1; quota < MATCHES * REPRESENTATIONS; ++quota) {
        budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS] = quota;
        size_t affected = SIZE_MAX; faults_clear();
        check_equal(change_sql(deletion ? "DELETE FROM items LIMIT 2" : "UPDATE items SET score=99 LIMIT 2",
            NULL, 0, &affected), TURBODB_STATUS_LIMIT_EXCEEDED);
        check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
      }
      budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS] = MATCHES * REPRESENTATIONS;
      size_t affected = SIZE_MAX;
      check_equal(change_sql("DELETE FROM items LIMIT 2", NULL, 0, &affected), TURBODB_STATUS_OK); check_equal(affected, MATCHES);
      budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS] = limit;
      check_equal(version(&owner, "items"), 5u);
      query_open(&owner, "items", "SELECT id FROM items");
      check_equal(next().values[0].data.int64_value, 3); check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
  }

  group("SQL UPDATE DELETE ORDER BY") {
    it("sorts original column values before LIMIT and left-to-right assignments") {
      seed_items(); size_t affected = SIZE_MAX;
      const turbodb_value_t parameters[] = {turbodb_i64(10), turbodb_i64(1), turbodb_u64(2)};
      check_equal(change_sql("UPDATE items SET score=score+id+?,score=score+1 WHERE id>=? ORDER BY items.id DESC LIMIT ?",
          parameters, 3, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 2u); check_equal(version(&owner, "items"), 5u);
      query_open(&owner, "items", "SELECT score FROM items");
      check_equal(next().values[0].data.int64_value, 20); check_equal(next().values[0].data.int64_value, 33);
      check_equal(next().values[0].data.int64_value, 34); check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
      check_equal(change_sql("UPDATE items SET score=-score ORDER BY score DESC LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); reopen();
      query_open(&owner, "items", "SELECT id,score FROM items WHERE score<0");
      orm_sql_scan_row row = next(); check_equal(row.values[0].data.int64_value, 3); check_equal(row.values[1].data.int64_value, -34);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("deletes filtered rows using multiple directions and a qualified column") {
      seed_items(); size_t affected = SIZE_MAX;
      check_equal(change_sql("DELETE FROM items WHERE id>1 ORDER BY score ASC,items.id DESC LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); check_equal(version(&owner, "items"), 5u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); reopen();
      query_open(&owner, "items", "SELECT id FROM items");
      check_equal(next().values[0].data.int64_value, 1); check_equal(next().values[0].data.int64_value, 2);
      check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
      check_equal(change_sql("DELETE FROM items ORDER BY id DESC", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 2u);
      check_equal(change_sql("DELETE FROM items ORDER BY id", NULL, 0, &affected), TURBODB_STATUS_OK); check_equal(affected, 0u);
    }
    it("places NULL first ascending and last descending with secondary ordering") {
      const turbodb_value_t rows[] = {turbodb_i64(1), turbodb_null(), turbodb_f64(0.0),
          turbodb_i64(2), turbodb_i64(10), turbodb_f64(0.0), turbodb_i64(3), turbodb_null(), turbodb_f64(0.0)};
      check_equal(orm_tidesdb_sql_relation_insert_rows(&owner, vstr_from_cstr("items"), rows, COLUMNS, COLUMNS, &error), TURBODB_STATUS_OK);
      size_t affected = SIZE_MAX;
      check_equal(change_sql("UPDATE items SET score=5 ORDER BY score,id DESC LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u);
      check_equal(change_sql("DELETE FROM items ORDER BY score DESC,id DESC LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); query_open(&owner, "items", "SELECT id,score FROM items");
      orm_sql_scan_row row = next(); check_equal(row.values[0].data.int64_value, 1); check_equal(row.values[1].kind, TURBODB_VALUE_NULL);
      row = next(); check_equal(row.values[0].data.int64_value, 3); check_equal(row.values[1].data.int64_value, 5);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("orders signed unsigned and finite double endpoints without subtraction or coercion") {
      create_table("CREATE TABLE extremes (id BIGINT PRIMARY KEY, s BIGINT, u BIGINT UNSIGNED, f DOUBLE)");
      enum { ROWS = 3, COLS = 4 };
      const turbodb_value_t rows[] = {turbodb_i64(1), turbodb_i64(INT64_MAX), turbodb_u64(0), turbodb_f64(-1.0e300),
          turbodb_i64(2), turbodb_i64(INT64_MIN), turbodb_u64(UINT64_MAX), turbodb_f64(1.0e300),
          turbodb_i64(3), turbodb_i64(0), turbodb_u64(UINT64_C(9223372036854775808)), turbodb_f64(0.0)};
      check_equal(orm_tidesdb_sql_relation_insert_rows(&owner, vstr_from_cstr("extremes"), rows, ROWS, COLS, &error), TURBODB_STATUS_OK);
      size_t affected = SIZE_MAX;
      const turbodb_value_t weight = turbodb_f64(1.0e299);
      check_equal(change_sql("UPDATE extremes SET f=? ORDER BY s ASC LIMIT 1", &weight, 1, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); query_open(&owner, "extremes", "SELECT f FROM extremes WHERE id=2");
      check_equal(next().values[0].data.double_value, weight.data.double_value); query_close();
      check_equal(change_sql("UPDATE extremes SET s=1 ORDER BY u DESC LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u);
      query_open(&owner, "extremes", "SELECT id FROM extremes WHERE s=1"); check_equal(next().values[0].data.int64_value, 2);
      check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
      check_equal(change_sql("DELETE FROM extremes ORDER BY f ASC LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u);
      check_equal(change_sql("DELETE FROM extremes ORDER BY s DESC LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); query_open(&owner, "extremes", "SELECT id FROM extremes");
      check_equal(next().values[0].data.int64_value, 3); check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("treats signed zero as equal and applies the next sort key") {
      const turbodb_value_t rows[] = {turbodb_i64(1), turbodb_i64(20), turbodb_f64(-0.0),
          turbodb_i64(2), turbodb_i64(20), turbodb_f64(0.0), turbodb_i64(3), turbodb_i64(20), turbodb_f64(-1.0)};
      check_equal(orm_tidesdb_sql_relation_insert_rows(&owner, vstr_from_cstr("items"), rows, COLUMNS, COLUMNS, &error), TURBODB_STATUS_OK);
      size_t affected = SIZE_MAX;
      check_equal(change_sql("DELETE FROM items ORDER BY weight DESC,id DESC LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      query_open(&owner, "items", "SELECT id FROM items"); check_equal(next().values[0].data.int64_value, 1);
      check_equal(next().values[0].data.int64_value, 3); check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("counts unchanged sorted matches and evaluates assignments only for selected rows") {
      create_table("CREATE TABLE extremes (id BIGINT PRIMARY KEY, score BIGINT NOT NULL)"); size_t affected = SIZE_MAX;
      check_equal(insert_sql("INSERT INTO extremes (id,score) VALUES (1,9223372036854775807),(2,20),(3,30)", NULL, 0, &affected), TURBODB_STATUS_OK);
      faults_clear();
      check_equal(change_sql("UPDATE extremes SET score=CASE WHEN id=3 THEN score ELSE score+1 END ORDER BY id DESC LIMIT 1",
          NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 0u); check_equal(put_calls, 0u); check_equal(version(&owner, "extremes"), 2u);
      check_equal(change_sql("UPDATE extremes SET score=score+1 ORDER BY id DESC LIMIT 2", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 2u); faults_clear(); affected = SIZE_MAX;
      check_equal(change_sql("UPDATE extremes SET score=score+1 ORDER BY id DESC LIMIT 3", NULL, 0, &affected), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u); check_equal(version(&owner, "extremes"), 3u);
      query_open(&owner, "extremes", "SELECT score FROM extremes");
      check_equal(next().values[0].data.int64_value, INT64_MAX); check_equal(next().values[0].data.int64_value, 21);
      check_equal(next().values[0].data.int64_value, 31); check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("binds ORDER BY even for empty tables and LIMIT zero without reading data") {
      size_t affected = SIZE_MAX;
      check_equal(change_sql("UPDATE items SET score=1 ORDER BY missing LIMIT 0", NULL, 0, &affected), TURBODB_STATUS_SQL_ERROR);
      check_equal(change_sql("DELETE FROM items ORDER BY other.id LIMIT 0", NULL, 0, &affected), TURBODB_STATUS_SQL_ERROR);
      check_equal(change_sql("DELETE FROM items ORDER BY id/2 LIMIT 0", NULL, 0, &affected), TURBODB_STATUS_UNSUPPORTED);
      check_equal(change_sql("DELETE FROM items ORDER BY 1 LIMIT 0", NULL, 0, &affected), TURBODB_STATUS_UNSUPPORTED);
      const turbodb_value_t parameter = turbodb_text("1");
      check_equal(change_sql("DELETE FROM items ORDER BY ? LIMIT 0", &parameter, 1, &affected), TURBODB_STATUS_UNSUPPORTED);
      check_equal(affected, SIZE_MAX);
      seed_items(); faults_clear(); fail_iterator = FAIL_NEW; fail_sort = true;
      check_equal(change_sql("UPDATE items SET score=99 ORDER BY id DESC LIMIT 0", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 0u);
      check_equal(change_sql("DELETE FROM items ORDER BY score,id DESC LIMIT 0", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 0u); check_equal(new_calls, 0u); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
      faults_clear(); check_equal(version(&owner, "items"), 4u);
    }
    it("reads all ordering candidates even when LIMIT is one and rejects later scan failure") {
      seed_items(); size_t affected = SIZE_MAX; faults_clear(); fail_iterator = FAIL_NEXT;
      check_equal(change_sql("UPDATE items SET score=99 ORDER BY id DESC LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
      faults_clear(); check_equal(owner.active_sources, 0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
    }
    it("refunds ordering scratch and snapshots after sort failure") {
      seed_items(); const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      const char *sql[] = {"UPDATE items SET score=99 ORDER BY id DESC LIMIT 1", "DELETE FROM items ORDER BY id DESC LIMIT 1"};
      for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
        size_t affected = SIZE_MAX; faults_clear(); fail_sort = true;
        check_equal(change_sql(sql[i], NULL, 0, &affected), TURBODB_STATUS_OUT_OF_MEMORY);
        check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
        check_false(owner.failed); check_equal(owner.active_sources, 0u); faults_clear();
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u); check_equal(version(&owner, "items"), 4u);
      }
    }
    it("requires all snapshots plus selected and encoded rows even with a small LIMIT") {
      seed_items(); enum { MATCHES = 3, SELECTED = 1, PEAK = MATCHES + SELECTED * 2 };
      const uint64_t limit = budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
      const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for (unsigned deletion = 0; deletion < 2; ++deletion) for (size_t quota = 1; quota < PEAK; ++quota) {
        budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS] = quota; size_t affected = SIZE_MAX; faults_clear();
        check_equal(change_sql(deletion ? "DELETE FROM items ORDER BY id DESC LIMIT 1" : "UPDATE items SET score=99 ORDER BY id DESC LIMIT 1",
            NULL, 0, &affected), TURBODB_STATUS_LIMIT_EXCEEDED);
        check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
      }
      budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS] = PEAK; size_t affected = SIZE_MAX;
      check_equal(change_sql("DELETE FROM items ORDER BY id DESC LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, SELECTED); budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS] = limit;
      query_open(&owner, "items", "SELECT id FROM items"); check_equal(next().values[0].data.int64_value, 1);
      check_equal(next().values[0].data.int64_value, 2); check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("rolls back every ordered data and version write while preserving earlier statements") {
      seed_items(); enum { ROWS = 2, WRITES = ROWS + 1 };
      for (unsigned deletion = 0; deletion < 2; ++deletion) for (size_t point = 1; point <= WRITES; ++point) {
        size_t affected = SIZE_MAX; faults_clear();
        if (!deletion) fail_put = point;
        else if (point == WRITES) fail_put = 1;
        else fail_delete = point;
        check_equal(change_sql(deletion ? "DELETE FROM items ORDER BY id DESC LIMIT 2" : "UPDATE items SET score=99 ORDER BY id DESC LIMIT 2",
            NULL, 0, &affected), TURBODB_STATUS_DATASTORE_ERROR);
        faults_clear(); check_equal(affected, SIZE_MAX); check_false(owner.failed); check_equal(version(&owner, "items"), 4u);
        query_open(&owner, "items", "SELECT score FROM items");
        for (size_t i = 0; i < COLUMNS; ++i) check_equal(next().values[0].data.int64_value, 20);
        check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
      }
    }
    it("preserves requested row order in native update and delete batches") {
      seed_items(); size_t affected = SIZE_MAX; faults_clear(); observe_writes = true;
      check_equal(change_sql("UPDATE items SET score=99 ORDER BY id DESC", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, COLUMNS); check_equal(observed_count, COLUMNS);
      for (size_t i = 0; i < COLUMNS; ++i) check_equal(observed_ids[i], COLUMNS - i);
      faults_clear(); observe_writes = true;
      check_equal(change_sql("DELETE FROM items ORDER BY id DESC", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, COLUMNS); check_equal(observed_count, COLUMNS);
      for (size_t i = 0; i < COLUMNS; ++i) check_equal(observed_ids[i], COLUMNS - i);
      faults_clear();
    }
    it("cleans up ordered snapshots when the materialization scan cannot open") {
      seed_items(); const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      size_t affected = SIZE_MAX; faults_clear(); fail_new_at = 2;
      check_equal(change_sql("DELETE FROM items ORDER BY id DESC LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
      check_equal(owner.active_sources, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u); faults_clear();
    }
  }

  group("SQL write ORDER BY expressions") {
    it("binds parameters across SET WHERE ORDER BY and LIMIT and persists the selected row") {
      seed_items(); size_t affected = SIZE_MAX;
      const turbodb_value_t parameters[] = {turbodb_i64(7), turbodb_i64(1), turbodb_i64(2), turbodb_i64(100), turbodb_u64(1)};
      check_equal(change_sql("UPDATE items SET score=score+? WHERE id>=? ORDER BY CASE WHEN id=? THEN score+? ELSE score-id END DESC,id DESC LIMIT ?",
          parameters, sizeof(parameters) / sizeof(parameters[0]), &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); check_equal(version(&owner, "items"), 5u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); reopen();
      query_open(&owner, "items", "SELECT id,score FROM items WHERE score<>20");
      orm_sql_scan_row row = next(); check_equal(row.values[0].data.int64_value, 2); check_equal(row.values[1].data.int64_value, 27);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("retains independent computed keys from original rows until ordered writes finish") {
      seed_items(); size_t affected = SIZE_MAX; faults_clear(); observe_writes = true;
      check_equal(change_sql("UPDATE items SET score=0 ORDER BY score-id DESC,(id=2) DESC LIMIT 2", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 2u); check_equal(observed_count, 2u); check_equal(observed_ids[0], 1); check_equal(observed_ids[1], 2);
      faults_clear(); query_open(&owner, "items", "SELECT score FROM items");
      check_equal(next().values[0].data.int64_value, 0); check_equal(next().values[0].data.int64_value, 0);
      check_equal(next().values[0].data.int64_value, 20); check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("sorts nullable function results and boolean computed keys with column tie breakers") {
      seed_items(); size_t affected = SIZE_MAX;
      check_equal(change_sql("DELETE FROM items ORDER BY NULLIF(id,2),id DESC LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u);
      check_equal(change_sql("UPDATE items SET score=NULL WHERE id=1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(change_sql("UPDATE items SET score=99 ORDER BY COALESCE(score,100) DESC LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); query_open(&owner, "items", "SELECT id,score FROM items");
      orm_sql_scan_row row = next(); check_equal(row.values[0].data.int64_value, 1); check_equal(row.values[1].data.int64_value, 99);
      row = next(); check_equal(row.values[0].data.int64_value, 3); check_equal(row.values[1].data.int64_value, 20); query_close();
      check_equal(change_sql("DELETE FROM items ORDER BY (score IS NULL) ASC,(id>1) DESC,id DESC LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      query_open(&owner, "items", "SELECT id FROM items"); check_equal(next().values[0].data.int64_value, 1);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("handles all NULL and constant parameter keys without interpreting parameters as positions") {
      seed_items(); const turbodb_value_t parameter = turbodb_i64(999); size_t affected = SIZE_MAX;
      check_equal(change_sql("DELETE FROM items ORDER BY ?,NULL,id DESC LIMIT 1", &parameter, 1, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u);
      check_equal(change_sql("DELETE FROM items ORDER BY TRUE,CASE WHEN id>0 THEN NULL ELSE NULL END,id DESC LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); query_open(&owner, "items", "SELECT id FROM items");
      check_equal(next().values[0].data.int64_value, 1); check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("preserves unsigned and floating expression types and descending direction") {
      create_table("CREATE TABLE metrics (id BIGINT PRIMARY KEY, u BIGINT UNSIGNED, f DOUBLE)");
      const turbodb_value_t rows[] = {turbodb_i64(1), turbodb_u64(0), turbodb_f64(-2.0),
          turbodb_i64(2), turbodb_u64(UINT64_MAX), turbodb_f64(4.0), turbodb_i64(3), turbodb_u64(UINT64_C(9223372036854775808)), turbodb_f64(1.0)};
      check_equal(orm_tidesdb_sql_relation_insert_rows(&owner, vstr_from_cstr("metrics"), rows, COLUMNS, COLUMNS, &error), TURBODB_STATUS_OK);
      const turbodb_value_t zero = turbodb_u64(0), multiplier = turbodb_f64(-1.0); size_t affected = SIZE_MAX;
      check_equal(change_sql("DELETE FROM metrics ORDER BY u+? DESC LIMIT 1", &zero, 1, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u);
      check_equal(change_sql("DELETE FROM metrics ORDER BY f*? DESC LIMIT 1", &multiplier, 1, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); query_open(&owner, "metrics", "SELECT id FROM metrics");
      check_equal(next().values[0].data.int64_value, 3); check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("rejects overflow in any matching candidate before applying LIMIT without staging writes") {
      create_table("CREATE TABLE extremes (id BIGINT PRIMARY KEY, score BIGINT)"); size_t affected = SIZE_MAX;
      check_equal(insert_sql("INSERT INTO extremes (id,score) VALUES (1,10),(2,9223372036854775807)", NULL, 0, &affected), TURBODB_STATUS_OK);
      const char *sql[] = {"UPDATE extremes SET score=0 ORDER BY score+1 LIMIT 1", "DELETE FROM extremes ORDER BY score+1 LIMIT 1"};
      const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
        affected = SIZE_MAX; faults_clear();
        check_equal(change_sql(sql[i], NULL, 0, &affected), TURBODB_STATUS_LIMIT_EXCEEDED);
        check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u); check_equal(delete_calls, 0u); check_false(owner.failed);
        check_equal(owner.active_sources, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u); check_equal(version(&owner, "extremes"), 2u);
      }
      check_equal(change_sql("UPDATE extremes SET score=0 ORDER BY score+1 LIMIT 0", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 0u);
      check_equal(change_sql("UPDATE extremes SET score=11 WHERE id=1 ORDER BY score+1 LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u);
      check_equal(change_sql("DELETE FROM extremes ORDER BY CASE WHEN id=1 THEN score+1 ELSE 0 END LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); query_open(&owner, "extremes", "SELECT id,score FROM extremes");
      orm_sql_scan_row row = next(); check_equal(row.values[0].data.int64_value, 1); check_equal(row.values[1].data.int64_value, 11);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("binds all ordering branches at LIMIT zero and rejects unsupported result kinds") {
      size_t affected = SIZE_MAX;
      const char *unsupported[] = {"DELETE FROM items ORDER BY 'text' LIMIT 0",
          "DELETE FROM items ORDER BY CASE WHEN TRUE THEN 'a' ELSE 'b' END LIMIT 0",
          "DELETE FROM items ORDER BY id/2 LIMIT 0", "DELETE FROM items ORDER BY COUNT(id) LIMIT 0",
          "DELETE FROM items ORDER BY 1 LIMIT 0",
          "DELETE FROM items ORDER BY -1 LIMIT 0", "DELETE FROM items ORDER BY +1 LIMIT 0"};
      for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i)
        check_equal(change_sql(unsupported[i], NULL, 0, &affected), TURBODB_STATUS_UNSUPPORTED);
      check_equal(change_sql("DELETE FROM items ORDER BY (SELECT 1) LIMIT 0",
          NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(change_sql("DELETE FROM items ORDER BY CASE WHEN TRUE THEN id ELSE missing END LIMIT 0", NULL, 0, &affected), TURBODB_STATUS_SQL_ERROR);
      check_equal(change_sql("DELETE FROM items ORDER BY id+? LIMIT 0", NULL, 0, &affected), TURBODB_STATUS_SQL_ERROR);
      const turbodb_value_t invalid = turbodb_f64(INFINITY), wrong = turbodb_u64(1);
      check_equal(change_sql("DELETE FROM items ORDER BY CASE WHEN TRUE THEN weight ELSE ? END LIMIT 0", &invalid, 1, &affected), TURBODB_STATUS_TYPE_ERROR);
      check_equal(change_sql("DELETE FROM items ORDER BY id+? LIMIT 0", &wrong, 1, &affected), TURBODB_STATUS_UNSUPPORTED);
      check_equal(affected, 0u); faults_clear(); fail_iterator = FAIL_NEW;
      check_equal(change_sql("DELETE FROM items ORDER BY CASE WHEN id=1 THEN id+1 ELSE id END LIMIT 0", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 0u); check_equal(new_calls, 0u); faults_clear();
    }
    it("refunds computed keys and programs after sort failure and succeeds on retry") {
      seed_items(); const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      size_t affected = SIZE_MAX; faults_clear(); fail_sort = true;
      check_equal(change_sql("DELETE FROM items ORDER BY score-id DESC,COALESCE(score,0) LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OUT_OF_MEMORY);
      check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u); check_equal(delete_calls, 0u); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base); check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
      faults_clear(); check_equal(change_sql("DELETE FROM items ORDER BY score-id DESC,COALESCE(score,0) LIMIT 1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); check_equal(version(&owner, "items"), 5u);
    }
    it("rolls back computed-order data and version writes without losing prior rows") {
      seed_items(); enum { ROWS = 2, WRITES = ROWS + 1 };
      for (unsigned deletion = 0; deletion < 2; ++deletion) for (size_t point = 1; point <= WRITES; ++point) {
        size_t affected = SIZE_MAX; faults_clear();
        if (!deletion) fail_put = point;
        else if (point == WRITES) fail_put = 1;
        else fail_delete = point;
        check_equal(change_sql(deletion ? "DELETE FROM items ORDER BY score-id LIMIT 2" : "UPDATE items SET score=99 ORDER BY score-id LIMIT 2",
            NULL, 0, &affected), TURBODB_STATUS_DATASTORE_ERROR);
        faults_clear(); check_equal(affected, SIZE_MAX); check_false(owner.failed); check_equal(version(&owner, "items"), 4u);
        query_open(&owner, "items", "SELECT score FROM items");
        for (size_t i = 0; i < COLUMNS; ++i) check_equal(next().values[0].data.int64_value, 20);
        check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
      }
    }
  }

  group("SQL primary key updates") {
    it("moves keys in descending order with left-to-right assignments and persists after reopen") {
      seed_items(); size_t affected = SIZE_MAX; faults_clear();
      check_equal(change_sql("UPDATE items SET id=id+1,score=id ORDER BY id DESC", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, COLUMNS); check_equal(delete_calls, COLUMNS); check_equal(put_calls, COLUMNS + 1);
      check_equal(version(&owner, "items"), 5u);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); reopen();
      query_open(&owner, "items", "SELECT id,score FROM items");
      for (int64_t id = 2; id <= COLUMNS + 1; ++id) {
        orm_sql_scan_row row = next(); check_equal(row.values[0].data.int64_value, id); check_equal(row.values[1].data.int64_value, id);
      }
      check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
      check_equal(insert_id(&owner, "items", 1), TURBODB_STATUS_OK);
    }
    it("rejects occupied future keys cycles duplicate destinations and unchanged occupants before writes") {
      seed_items(); const char *sql[] = {"UPDATE items SET id=id+1 ORDER BY id ASC",
          "UPDATE items SET id=3-id WHERE id<3 ORDER BY id", "UPDATE items SET id=99",
          "UPDATE items SET id=3 WHERE id=1", "UPDATE items SET id=2 WHERE id>=2"};
      for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
        size_t affected = SIZE_MAX; faults_clear();
        check_equal(change_sql(sql[i], NULL, 0, &affected), TURBODB_STATUS_CONSTRAINT);
        check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u); check_equal(delete_calls, 0u); check_false(owner.failed);
        check_equal(version(&owner, "items"), 4u);
      }
      query_open(&owner, "items", "SELECT id FROM items");
      for (int64_t id = 1; id <= COLUMNS; ++id) check_equal(next().values[0].data.int64_value, id);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("preserves no-op key assignments repeated targets and LIMIT match counting") {
      seed_items(); const char *sql[] = {"UPDATE items SET id=id", "UPDATE items SET id=id+1,id=id-1",
          "UPDATE items SET id=99 WHERE FALSE", "UPDATE items SET id=id+1 LIMIT 0",
          "UPDATE items SET id=CASE WHEN id=3 THEN id ELSE id+10 END ORDER BY id DESC LIMIT 1"};
      for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
        size_t affected = SIZE_MAX; faults_clear();
        check_equal(change_sql(sql[i], NULL, 0, &affected), TURBODB_STATUS_OK); check_equal(affected, 0u);
        check_equal(put_calls, 0u); check_equal(delete_calls, 0u); check_equal(version(&owner, "items"), 4u);
      }
      size_t affected = SIZE_MAX; faults_clear();
      check_equal(change_sql("UPDATE items SET id=id,score=99 WHERE id=1", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); check_equal(delete_calls, 0u); check_equal(put_calls, 2u);
    }
    it("selects original keys only once and applies LIMIT before moving them") {
      seed_items(); size_t affected = SIZE_MAX;
      const turbodb_value_t parameters[] = {turbodb_i64(1), turbodb_i64(1)};
      check_equal(change_sql("UPDATE items SET id=id+? ORDER BY id DESC LIMIT ?", parameters, 2, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); query_open(&owner, "items", "SELECT id FROM items");
      check_equal(next().values[0].data.int64_value, 1); check_equal(next().values[0].data.int64_value, 2);
      check_equal(next().values[0].data.int64_value, 4); check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
      check_equal(change_sql("UPDATE items SET id=id-1 ORDER BY id ASC", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, COLUMNS); query_open(&owner, "items", "SELECT id FROM items");
      check_equal(next().values[0].data.int64_value, 0); check_equal(next().values[0].data.int64_value, 1);
      check_equal(next().values[0].data.int64_value, 3); check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("supports non-leading primary columns and exact signed and unsigned endpoints") {
      create_table("CREATE TABLE signed_keys (score BIGINT,id BIGINT PRIMARY KEY)"); size_t affected = SIZE_MAX;
      check_equal(insert_sql("INSERT INTO signed_keys (score,id) VALUES (10,0)", NULL, 0, &affected), TURBODB_STATUS_OK);
      const turbodb_value_t low = turbodb_i64(INT64_MIN), high = turbodb_i64(INT64_MAX);
      check_equal(change_sql("UPDATE signed_keys SET id=?,score=id", &low, 1, &affected), TURBODB_STATUS_OK);
      check_equal(affected, 1u); query_open(&owner, "signed_keys", "SELECT id,score FROM signed_keys");
      orm_sql_scan_row row = next(); check_equal(row.values[0].data.int64_value, INT64_MIN); check_equal(row.values[1].data.int64_value, INT64_MIN); query_close();
      check_equal(change_sql("UPDATE signed_keys SET id=?", &high, 1, &affected), TURBODB_STATUS_OK);
      create_table("CREATE TABLE unsigned_keys (id BIGINT UNSIGNED PRIMARY KEY, score BIGINT)");
      const turbodb_value_t initial[] = {turbodb_u64(0), turbodb_i64(20)};
      check_equal(orm_tidesdb_sql_relation_insert(&owner, vstr_from_cstr("unsigned_keys"), initial, 2, &error), TURBODB_STATUS_OK);
      const turbodb_value_t maximum = turbodb_u64(UINT64_MAX);
      check_equal(change_sql("UPDATE unsigned_keys SET id=?", &maximum, 1, &affected), TURBODB_STATUS_OK);
      query_open(&owner, "unsigned_keys", "SELECT id FROM unsigned_keys"); check_equal(next().values[0].data.uint64_value, UINT64_MAX);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("rejects invalid NULL range and numeric forms without deleting old rows") {
      seed_items(); size_t affected = SIZE_MAX;
      const turbodb_value_t invalid[] = {turbodb_null(), turbodb_u64(UINT64_MAX),
          turbodb_f64(INFINITY), turbodb_text("invalid")};
      const turbodb_status_t expected[] = {TURBODB_STATUS_CONSTRAINT,
          TURBODB_STATUS_OUT_OF_RANGE, TURBODB_STATUS_TYPE_ERROR,
          TURBODB_STATUS_TYPE_ERROR};
      for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        faults_clear(); check_equal(change_sql(
            "UPDATE items SET id=? WHERE id=1", &invalid[i], 1, &affected),
            expected[i]);
        check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
      }
      faults_clear(); check_equal(change_sql("UPDATE items SET id=id+9223372036854775807", NULL, 0, &affected), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u); check_equal(delete_calls, 0u); check_equal(version(&owner, "items"), 4u);
    }
    it("rolls back each old-key delete new-row put and version write") {
      seed_items();
      for (unsigned deletion = 0; deletion < 2; ++deletion) {
        const size_t points = deletion ? COLUMNS : COLUMNS + 1;
        for (size_t point = 1; point <= points; ++point) {
          size_t affected = SIZE_MAX; faults_clear(); if (deletion) fail_delete = point; else fail_put = point;
          check_equal(change_sql("UPDATE items SET id=id+1 ORDER BY id DESC", NULL, 0, &affected), TURBODB_STATUS_DATASTORE_ERROR);
          faults_clear(); check_equal(affected, SIZE_MAX); check_false(owner.failed); check_equal(version(&owner, "items"), 4u);
          query_open(&owner, "items", "SELECT id,score FROM items");
          for (int64_t id = 1; id <= COLUMNS; ++id) {
            orm_sql_scan_row row = next(); check_equal(row.values[0].data.int64_value, id); check_equal(row.values[1].data.int64_value, 20);
          }
          check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
        }
      }
    }
    it("requires full rollback after failed move savepoint cleanup") {
      seed_items(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); owner_begin(&owner);
      size_t affected = SIZE_MAX; faults_clear(); fail_put = 2; fail_rollback = true;
      check_equal(change_sql("UPDATE items SET id=id+1 ORDER BY id DESC", NULL, 0, &affected), TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(affected, SIZE_MAX); check_true(owner.failed); faults_clear();
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_INVALID_STATE);
      reopen(); query_open(&owner, "items", "SELECT id FROM items");
      for (int64_t id = 1; id <= COLUMNS; ++id) check_equal(next().values[0].data.int64_value, id);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("validates typed source keys missing rows duplicate sources and oversized batches") {
      seed_items(); const turbodb_value_t old[] = {turbodb_i64(1), turbodb_i64(1)}, wrong = turbodb_u64(1), missing = turbodb_i64(99);
      const turbodb_value_t rows[] = {turbodb_i64(7), turbodb_i64(10), turbodb_f64(1.5), turbodb_i64(8), turbodb_i64(10), turbodb_f64(1.5)};
      faults_clear();
      check_equal(orm_tidesdb_sql_relation_move_rows(&owner, vstr_from_cstr("items"), old, rows, 2, COLUMNS, &error), TURBODB_STATUS_CONSTRAINT);
      check_equal(orm_tidesdb_sql_relation_move_rows(&owner, vstr_from_cstr("items"), &missing, rows, 1, COLUMNS, &error), TURBODB_STATUS_CONSTRAINT);
      check_equal(orm_tidesdb_sql_relation_move_rows(&owner, vstr_from_cstr("items"), &wrong, rows, 1, COLUMNS, &error), TURBODB_STATUS_TYPE_ERROR);
      check_equal(orm_tidesdb_sql_relation_move_rows(&owner, vstr_from_cstr("items"), NULL, rows, 1, COLUMNS, &error), TURBODB_STATUS_INVALID_ARGUMENT);
      check_equal(orm_tidesdb_sql_relation_move_rows(&owner, vstr_from_cstr("items"), old, rows, SIZE_MAX, COLUMNS, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(put_calls, 0u); check_equal(delete_calls, 0u); check_equal(version(&owner, "items"), 4u);
    }
    it("refunds source index sorting failures and rejects write budgets before deleting") {
      seed_items(); const uint64_t base = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for (size_t point = 1; point <= 2; ++point) {
        size_t affected = SIZE_MAX; faults_clear(); fail_sort_at = point;
        check_equal(change_sql("UPDATE items SET id=id+10", NULL, 0, &affected), TURBODB_STATUS_OUT_OF_MEMORY);
        check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u); check_equal(delete_calls, 0u); faults_clear();
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], base);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS], 0u);
      }
      enum { MOVES = COLUMNS, WRITES = MOVES * 2 + 1 };
      const uint64_t limit = budget.limits.statement.value[ORM_SQL_BUDGET_WRITE_ROWS];
      budget.limits.statement.value[ORM_SQL_BUDGET_WRITE_ROWS] = budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS] + WRITES - 1;
      size_t affected = SIZE_MAX;
      check_equal(change_sql("UPDATE items SET id=id+10", NULL, 0, &affected), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(affected, SIZE_MAX); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
      budget.limits.statement.value[ORM_SQL_BUDGET_WRITE_ROWS] = budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS] + WRITES;
      check_equal(change_sql("UPDATE items SET id=id+10", NULL, 0, &affected), TURBODB_STATUS_OK); check_equal(affected, MOVES);
      budget.limits.statement.value[ORM_SQL_BUDGET_WRITE_ROWS] = limit;
    }
    it("detects concurrent moves to the same destination at commit") {
      seed_items(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); owner_begin(&owner); owner_begin(&other);
      size_t affected = SIZE_MAX;
      check_equal(change_sql("UPDATE items SET id=4,score=99 WHERE id=1", NULL, 0, &affected), TURBODB_STATUS_OK);
      const turbodb_value_t old = turbodb_i64(2), row[] = {turbodb_i64(4), turbodb_i64(88), turbodb_f64(1.5)};
      check_equal(orm_tidesdb_sql_relation_move_rows(&other, vstr_from_cstr("items"), &old, row, 1, COLUMNS, &error), TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_catalog_finish(&other, true, &error), TURBODB_STATUS_BUSY); reopen();
      query_open(&owner, "items", "SELECT score FROM items WHERE id=4"); check_equal(next().values[0].data.int64_value, 99);
      check_equal(next().state, ORM_SQL_SCAN_DONE); query_close();
      query_open(&owner, "items", "SELECT id FROM items WHERE id=2"); check_equal(next().values[0].data.int64_value, 2);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
    it("rejects corrupt source and destination records before replacing keys") {
      seed_items(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
      uint8_t key[] = {3,1,0,0,0,0,0,0,0, 1,0,0,0,0,0,0,0, 128,0,0,0,0,0,0,1};
      const uint8_t valid[] = {'R','R',1,0,3,0,0,0, 0,1,0,0,0,0,0,0,0,
          0,20,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,248,63};
      const uint8_t broken[] = {0}; const turbodb_value_t old = turbodb_i64(1);
      const turbodb_value_t replacement[] = {turbodb_i64(9), turbodb_i64(20), turbodb_f64(1.5)};
      enum { DESTINATION = 9 };
      for (unsigned destination = 0; destination < 2; ++destination) {
        key[sizeof(key) - 1] = destination ? DESTINATION : 1;
        raw_put(key, sizeof(key), broken, sizeof(broken)); owner_begin(&owner); faults_clear();
        check_equal(orm_tidesdb_sql_relation_move_rows(&owner, vstr_from_cstr("items"), &old, replacement, 1, COLUMNS, &error), TURBODB_STATUS_DATASTORE_ERROR);
        check_true(owner.failed); check_equal(put_calls, 0u); check_equal(delete_calls, 0u);
        check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
        if (!destination) raw_put(key, sizeof(key), valid, sizeof(valid));
      }
      owner_begin(&owner); check_equal(version(&owner, "items"), 4u);
    }
    it("rolls back a successful key move and reopens original keys") {
      seed_items(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); owner_begin(&owner);
      size_t affected = SIZE_MAX;
      check_equal(change_sql("UPDATE items SET id=id+1 ORDER BY id DESC", NULL, 0, &affected), TURBODB_STATUS_OK);
      check_equal(affected, COLUMNS); reopen(); check_equal(version(&owner, "items"), 4u);
      query_open(&owner, "items", "SELECT id FROM items");
      for (int64_t id = 1; id <= COLUMNS; ++id) check_equal(next().values[0].data.int64_value, id);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
  }
}
