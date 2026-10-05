#include "table.h"
#include "catalog.h"
#include "row.h"
#include <tinytest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t reserves, resizes, fail_reserve, fail_resize, native_calls;
static int fail_native;
enum { FAIL_NEW = 1, FAIL_SEEK, FAIL_NEXT, FAIL_KEY, FAIL_VALUE };
static stl_status probe_reserve(vec_t *v, size_t n) { return ++reserves == fail_reserve ? STL_OUT_OF_MEMORY : vec_reserve(v, n); }
static stl_status probe_resize(vec_t *v, size_t n) { return ++resizes == fail_resize ? STL_OUT_OF_MEMORY : vec_resize(v, n); }
static int probe_new(orm_tidesdb_transaction_t *tx, orm_tidesdb_column_family_t *cf, orm_tidesdb_iterator_t **it) {
  ++native_calls; return fail_native == FAIL_NEW ? ORM_TDB_ERR_IO : orm_tidesdb_iter_new(tx, cf, it);
}
static int probe_seek(orm_tidesdb_iterator_t *it, const uint8_t *key, size_t n) {
  ++native_calls; return fail_native == FAIL_SEEK ? ORM_TDB_ERR_IO : orm_tidesdb_iter_seek(it, key, n);
}
static int probe_next(orm_tidesdb_iterator_t *it) {
  ++native_calls; return fail_native == FAIL_NEXT ? ORM_TDB_ERR_IO : orm_tidesdb_iter_next(it);
}
static int probe_key(orm_tidesdb_iterator_t *it, uint8_t **key, size_t *n) {
  ++native_calls; return fail_native == FAIL_KEY ? ORM_TDB_ERR_IO : orm_tidesdb_iter_key(it, key, n);
}
static int probe_value(orm_tidesdb_iterator_t *it, uint8_t **value, size_t *n) {
  ++native_calls; return fail_native == FAIL_VALUE ? ORM_TDB_ERR_IO : orm_tidesdb_iter_value(it, value, n);
}
#define vec_reserve probe_reserve
#define vec_resize probe_resize
#define orm_tidesdb_iter_new probe_new
#define orm_tidesdb_iter_seek probe_seek
#define orm_tidesdb_iter_next probe_next
#define orm_tidesdb_iter_key probe_key
#define orm_tidesdb_iter_value probe_value
#include "../../../../../tidessql/src/work.c"
#include "../../../../../drivers/tidesdb/sql/table.c"
#undef vec_reserve
#undef vec_resize
#undef orm_tidesdb_iter_new
#undef orm_tidesdb_iter_seek
#undef orm_tidesdb_iter_next
#undef orm_tidesdb_iter_key
#undef orm_tidesdb_iter_value

enum { COLUMNS = 3, MAX_ROW = 4096, WORK = 4 * 1024 * 1024, LIMIT = 100000, DEPTH = 32, KEY_BYTES = 64 };
static const char family_name[] = "sql-table-test";
static const char table_prefix[] = "orm:items:";
static char *directory;
static orm_tidesdb_database_t *database;
static orm_tidesdb_column_family_t *family;
static orm_tidesdb_transaction_t *reader, *writer;
static orm_tidesdb_sql_budget budget;
static orm_sql_table_source table;
static orm_sql_select plan;
static orm_sql_select_run run;
static orm_error_t error;
static orm_sql_schema_column columns[COLUMNS];
static orm_sql_table_schema schema;
static uint64_t seeded_bytes;

static void open_database(void) {
  orm_tidesdb_config_t config = orm_tidesdb_default_config(); config.db_path = directory;
  check_equal(orm_tidesdb_open(&config, &database), ORM_TDB_SUCCESS);
}
static void begin(orm_tidesdb_transaction_t **out) {
  check_equal(orm_tidesdb_txn_begin_with_isolation(database, ORM_TDB_ISOLATION_SNAPSHOT, out), ORM_TDB_SUCCESS);
}
static void end(orm_tidesdb_transaction_t **tx, bool commit) {
  if (!*tx) return;
  check_equal(commit ? orm_tidesdb_txn_commit(*tx) : orm_tidesdb_txn_rollback(*tx), ORM_TDB_SUCCESS);
  orm_tidesdb_txn_free(*tx); *tx = NULL;
}
static void set_cell(orm_tidesdb_row *row, const char *name, const orm_owned_value *value) {
  orm_tidesdb_cell cell = {0};
  check_equal(orm_tidesdb_cell_from_value(&cell, value, &error), ORM_STATUS_OK);
  check_equal(orm_tidesdb_row_set(row, vstr_from_cstr(name), &cell, &error), ORM_STATUS_OK);
  tstr_freep(&cell.bytes);
}
static void put_row(orm_tidesdb_transaction_t *tx, int id, const char *name, bool score_present) {
  orm_tidesdb_row row = {0}; check_equal(orm_tidesdb_row_init(&row, COLUMNS, &error), ORM_STATUS_OK);
  orm_owned_value value = {.kind = ORM_VALUE_TEXT, .bytes = tstr_dup(name)};
  set_cell(&row, "name", &value); tstr_freep(&value.bytes);
  value = (orm_owned_value){.kind = ORM_VALUE_INT64, .data.int64_value = id}; set_cell(&row, "id", &value);
  value.data.int64_value = id * 10;
  if (score_present) set_cell(&row, "score", &value);
  tstr encoded = NULL; check_equal(orm_tidesdb_row_encode(&row, MAX_ROW, &encoded, &error), ORM_STATUS_OK);
  char key[KEY_BYTES]; const int n = snprintf(key, sizeof(key), "%s%d", table_prefix, id);
  check_greater(n, 0); check_less((size_t)n, sizeof(key));
  check_equal(orm_tidesdb_txn_put(tx, family, (const uint8_t *)key, (size_t)n,
      (const uint8_t *)encoded, tstr_len(encoded), 0), ORM_TDB_SUCCESS);
  seeded_bytes += (size_t)n + tstr_len(encoded);
  tstr_free(encoded); orm_tidesdb_row_destroy(&row);
}
static void bind_sql(const char *sql, const orm_sql_type *types, size_t count) {
  sqlparser_document *doc = NULL; sqlparser_error parse_error;
  check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &parse_error), SQLPARSER_OK);
  check_equal(orm_tidesdb_sql_select_bind_parameters(doc, &schema, types, count, DEPTH, &budget, &plan, &error), ORM_STATUS_OK);
  sqlparser_document_destroy(doc);
}
static orm_status_t source_open(size_t max_row) {
  return orm_tidesdb_sql_table_open(reader, family, vstr_from_cstr(table_prefix), &schema, max_row, &budget, &table, &error);
}
static void open_run(void) {
  check_equal(orm_tidesdb_sql_select_open_source(&plan, &table.source, NULL, 0, &run, &error), ORM_STATUS_OK);
}
static orm_sql_scan_row next(void) {
  orm_sql_scan_row row = {0}; check_equal(orm_tidesdb_sql_scan_next(&run.scan, &row, &error), ORM_STATUS_OK); return row;
}
static void close_query(void) {
  fail_native = 0; fail_reserve = fail_resize = 0;
  check_equal(orm_tidesdb_sql_select_close(&run, &error), ORM_STATUS_OK);
  check_equal(orm_tidesdb_sql_table_close(&table, &error), ORM_STATUS_OK);
  check_equal(orm_tidesdb_sql_select_destroy(&plan, &error), ORM_STATUS_OK);
}

spec("TidesDB native source through relational SELECT") {
  before_each() {
    reserves = resizes = native_calls = fail_reserve = fail_resize = 0; fail_native = 0;
    table = (orm_sql_table_source){0}; plan = (orm_sql_select){0}; run = (orm_sql_select_run){0};
    orm_error_init(&error); seeded_bytes = 0;
    orm_sql_budget_limits limits = {0};
    for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) limits.statement.value[i] = LIMIT;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = WORK;
    limits.transaction = (orm_sql_transaction_budget_amount){LIMIT, LIMIT, LIMIT};
    check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), ORM_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), ORM_STATUS_OK);
    columns[0] = (orm_sql_schema_column){{"id", 2}, {ORM_VALUE_INT64, false}};
    columns[1] = (orm_sql_schema_column){{"score", 5}, {ORM_VALUE_INT64, false}};
    columns[2] = (orm_sql_schema_column){{"name", 4}, {ORM_VALUE_TEXT, false}};
    schema = (orm_sql_table_schema){{"items", 5}, columns, COLUMNS};
    directory = tt_make_temp_dir("orm-sql-native-source"); check_not_null(directory); open_database();
    orm_tidesdb_column_family_config_t config = orm_tidesdb_default_column_family_config(); config.sync_mode = ORM_TDB_SYNC_FULL;
    check_equal(orm_tidesdb_create_column_family(database, family_name, &config), ORM_TDB_SUCCESS);
    family = orm_tidesdb_get_column_family(database, family_name); check_not_null(family);
    begin(&writer); put_row(writer, 1, "alpha", true); put_row(writer, 2, "beta", true); put_row(writer, 3, "bravo", true); end(&writer, true);
    begin(&reader);
  }
  after_each() {
    close_query(); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), ORM_STATUS_OK);
    end(&reader, false); end(&writer, false);
    check_equal(orm_tidesdb_close(database), ORM_TDB_SUCCESS); database = NULL; family = NULL;
    check_equal(tt_remove_tree(directory), 0); free(directory); directory = NULL;
  }

  it("queries persisted rows after reopen with aliases parameters LIKE calculations and paging") {
    end(&reader, false); check_equal(orm_tidesdb_close(database), ORM_TDB_SUCCESS); database = NULL;
    open_database(); family = orm_tidesdb_get_column_family(database, family_name); begin(&reader);
    const orm_sql_type types[] = {{ORM_VALUE_INT64, false}, {ORM_VALUE_TEXT, false}};
    bind_sql("SELECT t.id, t.score + ? AS total, t.name AS label FROM items t WHERE t.name LIKE ? LIMIT 1 OFFSET 1", types, 2);
    check_equal(source_open(MAX_ROW), ORM_STATUS_OK); check_null(table.iterator);
    char pattern[] = "b%"; orm_value_t values[] = {orm_i64(7), orm_text(pattern)};
    check_equal(orm_tidesdb_sql_select_open_source(&plan, &table.source, values, 2, &run, &error), ORM_STATUS_OK);
    pattern[0] = 'z'; values[0] = orm_i64(0);
    check_equal(orm_tidesdb_sql_table_close(&table, &error), ORM_STATUS_BUSY);
    orm_sql_select_run other = {0};
    check_equal(orm_tidesdb_sql_select_open_source(&plan, &table.source, values, 2, &other, &error), ORM_STATUS_BUSY);
    memset(columns, 0, sizeof(columns));
    reserves = resizes = 0; fail_reserve = fail_resize = 1;
    orm_sql_scan_row row = next(); check_equal(row.count, 3u); check_equal(row.values[0].data.int64_value, 3);
    check_equal(row.values[1].data.int64_value, 37); check_equal(memcmp(row.values[2].data.text_value.data, "bravo", 5), 0);
    check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 3u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_BYTES], seeded_bytes);
    check_equal(reserves, 0u); check_equal(resizes, 0u);
  }

  it("keeps a transaction snapshot while another transaction commits a new row") {
    bind_sql("SELECT id FROM items", NULL, 0); check_equal(source_open(MAX_ROW), ORM_STATUS_OK); open_run();
    check_equal(next().values[0].data.int64_value, 1);
    begin(&writer); put_row(writer, 4, "later", true); end(&writer, true);
    check_equal(next().values[0].data.int64_value, 2); check_equal(next().values[0].data.int64_value, 3);
    check_equal(next().state, ORM_SQL_SCAN_DONE); close_query(); end(&reader, false); begin(&reader);
    put_row(reader, 5, "own", true);
    bind_sql("SELECT id FROM items", NULL, 0); check_equal(source_open(MAX_ROW), ORM_STATUS_OK); open_run();
    for (int id = 1; id <= 5; ++id) check_equal(next().values[0].data.int64_value, id);
    check_equal(next().state, ORM_SQL_SCAN_DONE); close_query();
    /* Closing the SQL source must leave the borrowed transaction usable. */
    check_equal(orm_tidesdb_txn_savepoint(reader, "after-source"), ORM_TDB_SUCCESS);
  }

  it("queries persisted numeric rows with schema derived from a CREATE definition") {
    const char key[] = "orm:metrics:1";
    const char prefix[] = "orm:metrics:";
    const char ddl[] = "CREATE TABLE metrics (id BIGINT PRIMARY KEY, score BIGINT NOT NULL)";
    end(&reader, false); begin(&writer);
    orm_tidesdb_row stored = {0}; check_equal(orm_tidesdb_row_init(&stored, 2, &error), ORM_STATUS_OK);
    orm_owned_value value = {.kind = ORM_VALUE_INT64, .data.int64_value = 20}; set_cell(&stored, "score", &value);
    value.data.int64_value = 1; set_cell(&stored, "id", &value);
    tstr encoded = NULL; check_equal(orm_tidesdb_row_encode(&stored, MAX_ROW, &encoded, &error), ORM_STATUS_OK);
    check_equal(orm_tidesdb_txn_put(writer, family, (const uint8_t *)key, sizeof(key) - 1,
        (const uint8_t *)encoded, tstr_len(encoded), 0), ORM_TDB_SUCCESS);
    tstr_free(encoded); orm_tidesdb_row_destroy(&stored); end(&writer, true);
    check_equal(orm_tidesdb_close(database), ORM_TDB_SUCCESS); database = NULL;
    open_database(); family = orm_tidesdb_get_column_family(database, family_name); begin(&reader);

    sqlparser_document *doc = NULL; sqlparser_error parse_error;
    check_equal(sqlparser_parse(ddl, sizeof(ddl) - 1, NULL, &doc, &parse_error), SQLPARSER_OK);
    orm_sql_table_definition definition = {0};
    check_equal(orm_tidesdb_sql_catalog_bind_create(doc, &budget, &definition, &error), ORM_STATUS_OK);
    sqlparser_document_destroy(doc);
    check_equal(orm_tidesdb_sql_catalog_schema(&definition, &schema, &error), ORM_STATUS_OK);
    bind_sql("SELECT m.id, m.score + 7 AS total FROM metrics m WHERE m.id = 1", NULL, 0);
    check_equal(orm_tidesdb_sql_table_open(reader, family, vstr_from_cstr(prefix), &schema,
        MAX_ROW, &budget, &table, &error), ORM_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_destroy(&definition, &error), ORM_STATUS_OK);
    schema = (orm_sql_table_schema){0};
    open_run(); orm_sql_scan_row result = next();
    check_equal(result.state, ORM_SQL_SCAN_ROW); check_equal(result.count, 2u);
    check_equal(result.values[0].data.int64_value, 1); check_equal(result.values[1].data.int64_value, 27);
    check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 1u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS], 0u);
  }

  it("does no native IO for LIMIT zero or cancellation before the first pull") {
    const char *sql[] = {"SELECT id FROM items LIMIT 0", "SELECT id FROM items"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
      bind_sql(sql[i], NULL, 0); check_equal(source_open(MAX_ROW), ORM_STATUS_OK); open_run();
      if (i) check_equal(orm_tidesdb_sql_scan_cancel(&run.scan, &error), ORM_STATUS_OK);
      check_equal(next().state, i ? ORM_SQL_SCAN_CANCELLED : ORM_SQL_SCAN_DONE);
      check_equal(native_calls, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u); close_query();
    }
  }

  it("stops at the prefix boundary and handles an empty prefix range") {
    close_query(); end(&reader, false); begin(&writer);
    const uint8_t alien[] = "orm:itemsZ:1", corrupt[] = "not a row";
    check_equal(orm_tidesdb_txn_put(writer, family, alien, sizeof(alien) - 1, corrupt, sizeof(corrupt) - 1, 0), ORM_TDB_SUCCESS);
    end(&writer, true); begin(&reader);
    bind_sql("SELECT id FROM items", NULL, 0); check_equal(source_open(MAX_ROW), ORM_STATUS_OK); open_run();
    for (int id = 1; id <= 3; ++id) check_equal(next().values[0].data.int64_value, id);
    check_equal(next().state, ORM_SQL_SCAN_DONE); close_query();
    bind_sql("SELECT id FROM items", NULL, 0);
    check_equal(orm_tidesdb_sql_table_open(reader, family, vstr_from_cstr("orm:absent:"), &schema,
        MAX_ROW, &budget, &table, &error), ORM_STATUS_OK); open_run();
    check_equal(next().state, ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 3u);
  }

  it("rejects missing stored columns instead of supplying NULL and preserves the first error") {
    end(&reader, false); begin(&writer); put_row(writer, 2, "bad", false); end(&writer, true); begin(&reader);
    bind_sql("SELECT id FROM items", NULL, 0); check_equal(source_open(MAX_ROW), ORM_STATUS_OK); open_run();
    check_equal(next().values[0].data.int64_value, 1);
    orm_sql_scan_row out = {.state = ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &out, &error), ORM_STATUS_TYPE_ERROR);
    check_equal(out.state, ORM_SQL_SCAN_CANCELLED); const size_t calls = native_calls;
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &out, &error), ORM_STATUS_TYPE_ERROR);
    check_equal(native_calls, calls); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 2u);
  }

  it("rejects unprojected stored type drift and incompatible source descriptors") {
    columns[1].type.kind = ORM_VALUE_TEXT;
    bind_sql("SELECT id FROM items", NULL, 0); check_equal(source_open(MAX_ROW), ORM_STATUS_OK); open_run();
    orm_sql_scan_row out = {.state = ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &out, &error), ORM_STATUS_TYPE_ERROR);
    check_equal(out.state, ORM_SQL_SCAN_CANCELLED); close_query();
    columns[1].type.kind = ORM_VALUE_INT64; bind_sql("SELECT id FROM items", NULL, 0);
    columns[1].type.nullable = true; check_equal(source_open(MAX_ROW), ORM_STATUS_OK);
    const size_t calls = native_calls;
    check_equal(orm_tidesdb_sql_select_open_source(&plan, &table.source, NULL, 0, &run, &error), ORM_STATUS_TYPE_ERROR);
    check_null(run.program); check_equal(table.source.active, false); check_equal(native_calls, calls);
  }

  it("reports corrupt persisted records without publishing a row") {
    end(&reader, false); begin(&writer);
    const uint8_t key[] = "orm:items:1", invalid[] = "invalid-row";
    check_equal(orm_tidesdb_txn_put(writer, family, key, sizeof(key) - 1, invalid, sizeof(invalid) - 1, 0), ORM_TDB_SUCCESS);
    end(&writer, true); begin(&reader);
    bind_sql("SELECT id FROM items", NULL, 0); check_equal(source_open(MAX_ROW), ORM_STATUS_OK); open_run();
    orm_sql_scan_row out = {.state = ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &out, &error), ORM_STATUS_DATASTORE_ERROR);
    check_equal(out.state, ORM_SQL_SCAN_CANCELLED); check_contains(error.message, "format or version");
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 1u);
  }

  it("enforces row byte read and work limits without retaining failed source state") {
    bind_sql("SELECT id FROM items", NULL, 0); check_equal(source_open(1), ORM_STATUS_OK); open_run();
    orm_sql_scan_row out = {0};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &out, &error), ORM_STATUS_LIMIT_EXCEEDED); close_query();
    bind_sql("SELECT id FROM items", NULL, 0); const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = retained;
    check_equal(source_open(MAX_ROW), ORM_STATUS_LIMIT_EXCEEDED); check_null(table.source.budget);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = WORK;
    check_equal(source_open(MAX_ROW), ORM_STATUS_OK); open_run();
    budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS] = budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &out, &error), ORM_STATUS_LIMIT_EXCEEDED);
  }

  it("reports each native iterator failure without treating it as EOF or retrying") {
    for (int stage = FAIL_NEW; stage <= FAIL_VALUE; ++stage) {
      bind_sql("SELECT id FROM items", NULL, 0); check_equal(source_open(MAX_ROW), ORM_STATUS_OK); open_run();
      if (stage == FAIL_NEXT) check_equal(next().state, ORM_SQL_SCAN_ROW);
      fail_native = stage; orm_sql_scan_row out = {.state = ORM_SQL_SCAN_CANCELLED};
      check_equal(orm_tidesdb_sql_scan_next(&run.scan, &out, &error), ORM_STATUS_DATASTORE_ERROR);
      check_contains(error.message, "native error"); check_equal(out.state, ORM_SQL_SCAN_CANCELLED);
      const size_t calls = native_calls;
      check_equal(orm_tidesdb_sql_scan_next(&run.scan, &out, &error), ORM_STATUS_DATASTORE_ERROR);
      check_equal(native_calls, calls); close_query();
    }
  }

  it("refunds every source workspace reserve and resize failure before native IO") {
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    reserves = resizes = 0; check_equal(source_open(MAX_ROW), ORM_STATUS_OK);
    const size_t reserve_count = reserves, resize_count = resizes;
    check_equal(orm_tidesdb_sql_table_close(&table, &error), ORM_STATUS_OK);
    for (size_t i = 1; i <= reserve_count + resize_count; ++i) {
      reserves = resizes = fail_reserve = fail_resize = 0;
      if (i <= reserve_count) fail_reserve = i; else fail_resize = i - reserve_count;
      check_equal(source_open(MAX_ROW), ORM_STATUS_OUT_OF_MEMORY); check_null(table.source.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained); check_equal(native_calls, 0u);
    }
  }
}
