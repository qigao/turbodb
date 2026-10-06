#include "runtime.h"
#include "subquery.h"
#include "wire.h"
#include "cte_store.h"
#include "insert.h"
#include "change.h"
#include "parameters.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static size_t reserves, resizes, fail_reserve, fail_resize;
static stl_status probe_reserve(vec_t *v, size_t n) { return ++reserves == fail_reserve ? STL_OUT_OF_MEMORY : vec_reserve(v, n); }
static stl_status probe_resize(vec_t *v, size_t n) { return ++resizes == fail_resize ? STL_OUT_OF_MEMORY : vec_resize(v, n); }
#define vec_reserve probe_reserve
#define vec_resize probe_resize
#include "../../src/work.c"
#undef vec_reserve
#undef vec_resize

enum { MAX_RECORD = 4096, WORK = 4 * 1024 * 1024, LIMIT = 1000000, DEPTH = 32 };
static const char family_name[] = "sql-runtime";
static const char ddl[] = "CREATE TABLE items (id BIGINT PRIMARY KEY, score BIGINT)";
static char *directory;
static orm_tidesdb_database_t *database;
static orm_tidesdb_column_family_t *family;
static orm_tidesdb_sql_budget budget;
static orm_sql_catalog_store owner;
static orm_sql_query query, other;
static orm_sql_subquery subquery;
static orm_sql_row_source subquery_source;
static orm_sql_type subquery_type;
static turbodb_error_t error;
static orm_sql_diagnostics evaluation_diagnostics;
static struct {
  orm_sql_cte_store store;
  orm_sql_cte_reader reader;
  orm_sql_row_source proxy, member, *frontier;
  orm_sql_type input_type, output_type;
  size_t opens, closes;
} recursion;

static void database_open(void) {
  orm_tidesdb_config_t config = orm_tidesdb_default_config(); config.db_path = directory;
  check_equal(orm_tidesdb_open(&config, &database), ORM_TDB_SUCCESS);
}
static void begin(void) {
  check_equal(orm_tidesdb_sql_catalog_begin(database, family, MAX_RECORD, &budget, &owner, &error), TURBODB_STATUS_OK);
}
static sqlparser_document *parse(const char *sql) {
  info("runtime SQL: %s", sql);
  sqlparser_document *doc = NULL; sqlparser_error parse_error;
  check_equal(sqlparser_parse(sql, strlen(sql), NULL, &doc, &parse_error), SQLPARSER_OK); return doc;
}
static turbodb_status_t execute_iterations(const char *sql, const turbodb_value_t *params,
    size_t count, uint64_t max_iterations, size_t *affected) {
  sqlparser_document *doc = parse(sql);
  const turbodb_status_t status = orm_tidesdb_sql_runtime_execute(doc, &owner,
      params, count, DEPTH, max_iterations, false, NULL, affected, &error);
  sqlparser_document_destroy(doc); return status;
}
static turbodb_status_t execute(const char *sql, const turbodb_value_t *params,
    size_t count, size_t *affected) {
  return execute_iterations(sql, params, count, 0, affected);
}
static turbodb_status_t bind_write_iterations(const char *sql, const orm_sql_type *types,
    size_t count, uint64_t max_iterations) {
  sqlparser_document *document = parse(sql);
  const sqlparser_id root = sqlparser_statements(document).first;
  const orm_sql_query_scope scope = {.document=document,.root=root,.parameter_types=types,
      .parameter_count=count,.max_depth=DEPTH,.max_iterations=max_iterations,.budget=&budget,
      .evaluation={.diagnostics=&evaluation_diagnostics,
          .session={.valid=true,.read_only=true,.autocommit=false}}};
  const orm_sql_budget_amount before = budget.used;
  orm_tidesdb_transaction_t *transaction = owner.transaction;
  const uint64_t warnings = evaluation_diagnostics.total;
  const turbodb_status_t status = sqlparser_get_node(document,root)->kind == SQLPARSER_INSERT ?
      orm_sql_insert_bind(&scope,&owner,&error) : orm_sql_change_bind(&scope,&owner,&error);
  sqlparser_document_destroy(document);
  check_true(owner.transaction == transaction); check_equal(owner.active_sources,0u);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],before.value[ORM_SQL_BUDGET_WORK_BYTES]);
  check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],before.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS],before.value[ORM_SQL_BUDGET_WRITE_ROWS]);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_BYTES],before.value[ORM_SQL_BUDGET_WRITE_BYTES]);
  check_equal(evaluation_diagnostics.total,warnings); check_false(owner.failed);
  return status;
}
static turbodb_status_t bind_write(const char *sql, const orm_sql_type *types, size_t count) {
  return bind_write_iterations(sql,types,count,0);
}
static void statement_metadata(const char *sql,const turbodb_value_kind_t *kinds,
    size_t count,turbodb_status_t expected) {
  sqlparser_document *document=parse(sql); orm_sql_parameters parameters={0};
  const orm_sql_budget_amount before=budget.used;
  orm_tidesdb_transaction_t *transaction=owner.transaction;
  const uint64_t warnings=evaluation_diagnostics.total;
  const turbodb_status_t status=orm_sql_parameters_statement(document,&owner,DEPTH,DEPTH,&parameters,&error);
  if(status!=expected) info("statement inference status %d: %s",status,error.message);
  check_equal(status,expected); sqlparser_document_destroy(document);
  if(status==TURBODB_STATUS_OK) {
    const orm_sql_type *types=NULL; size_t actual=0;
    check_equal(orm_sql_parameters_types(&parameters,&types,&actual,&error),TURBODB_STATUS_OK);
    check_equal(actual,count);
    for(size_t i=0;i<actual&&i<count;++i) { check_equal(types[i].kind,kinds[i]); check_true(types[i].nullable); }
  } else {
    check_null(parameters.budget); check_false(parameters.offsets.initialized);
    check_false(parameters.types.initialized); check_false(parameters.resolved.initialized);
  }
  check_equal(orm_sql_parameters_close(&parameters,&error),TURBODB_STATUS_OK);
  check_true(owner.transaction==transaction); check_equal(owner.active_sources,0u); check_false(owner.failed);
  check_equal(evaluation_diagnostics.total,warnings);
  const orm_sql_budget_resource unchanged[]={ORM_SQL_BUDGET_WORK_BYTES,ORM_SQL_BUDGET_MATERIALIZED_ROWS,
    ORM_SQL_BUDGET_WRITE_ROWS,ORM_SQL_BUDGET_WRITE_BYTES};
  for(size_t i=0;i<sizeof(unchanged)/sizeof(unchanged[0]);++i)
    check_equal(budget.used.value[unchanged[i]],before.value[unchanged[i]]);
}
static uint64_t table_version(void) {
  orm_sql_table_definition definition = {0}; uint64_t id = 0, version = 0; bool found = false;
  check_equal(orm_tidesdb_sql_catalog_lookup(&owner,vstr_from_cstr("items"),&definition,
      &id,&version,&found,&error),TURBODB_STATUS_OK); check_true(found);
  check_equal(orm_tidesdb_sql_catalog_destroy(&definition,&error),TURBODB_STATUS_OK);
  return version;
}
static turbodb_status_t open_query(const char *sql, const turbodb_value_t *params, size_t count, orm_sql_query *out) {
  sqlparser_document *doc = parse(sql);
  const turbodb_status_t status = orm_tidesdb_sql_runtime_open(doc, &owner, vstr_from_cstr("app"), params, count, DEPTH, out, &error);
  sqlparser_document_destroy(doc); return status;
}
static turbodb_status_t bind_query(const char *sql, const orm_sql_type *types, size_t count) {
  sqlparser_document *document = parse(sql);
  const orm_sql_query_scope scope = {.document=document,.root=sqlparser_statements(document).first,
      .parameter_types=types,.parameter_count=count,.max_depth=DEPTH,.budget=&budget,
      .evaluation={.diagnostics=&evaluation_diagnostics,
          .session={.valid=true,.read_only=true,.autocommit=false}}};
  const uint64_t writes = budget.used.value[ORM_SQL_BUDGET_WRITE_BYTES];
  const uint64_t materialized = budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
  const uint64_t warnings = evaluation_diagnostics.total;
  orm_tidesdb_transaction_t *transaction = owner.transaction;
  const turbodb_status_t status = orm_sql_runtime_query_bind(&scope,&owner,&query,&error);
  sqlparser_document_destroy(document);
  check_true(owner.transaction == transaction); check_equal(evaluation_diagnostics.total,warnings);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_BYTES],writes); check_false(owner.failed);
  check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],materialized);
  return status;
}
static void corrupt_first_row(void) {
  orm_sql_relation_source source = {0};
  check_equal(orm_tidesdb_sql_relation_open(&owner,vstr_from_cstr("items"),&source,&error),TURBODB_STATUS_OK);
  uint8_t key[ORM_SQL_RELATION_KEY_BYTES]; memcpy(key,source.prefix,ORM_SQL_RELATION_PREFIX_BYTES);
  orm_sql_wire_order_write(key+ORM_SQL_RELATION_PREFIX_BYTES,orm_sql_wire_signed_order(1));
  check_equal(orm_tidesdb_sql_relation_close(&source,&error),TURBODB_STATUS_OK);
  const uint8_t corrupt[] = {0};
  check_equal(orm_tidesdb_txn_put(owner.transaction,family,key,sizeof(key),corrupt,sizeof(corrupt),0),ORM_TDB_SUCCESS);
}
static turbodb_status_t open_evaluation_iterations_query(const char *sql,
    orm_sql_evaluation_mode mode,uint64_t iterations) {
  sqlparser_document *doc=parse(sql);
  const orm_sql_evaluation evaluation={&evaluation_diagnostics,mode};
  const turbodb_status_t status=orm_sql_runtime_open_evaluation(doc,&owner,vstr_from_cstr("app"),
      NULL,0,DEPTH,iterations,evaluation,&query,&error);
  sqlparser_document_destroy(doc); return status;
}
static turbodb_status_t open_evaluation_query(const char *sql, orm_sql_evaluation_mode mode) {
  return open_evaluation_iterations_query(sql,mode,0);
}
static turbodb_status_t open_session_parameters_query(const char *sql,orm_sql_session_snapshot session,
    const turbodb_value_t *parameters,size_t count) {
  sqlparser_document *doc=parse(sql);
  const orm_sql_evaluation evaluation={.session=session};
  const turbodb_status_t status=orm_sql_runtime_open_evaluation(doc,&owner,vstr_from_cstr("app"),
      parameters,count,DEPTH,0,evaluation,&query,&error);
  sqlparser_document_destroy(doc); return status;
}
static turbodb_status_t open_session_query(const char *sql, orm_sql_session_snapshot session) {
  return open_session_parameters_query(sql,session,NULL,0);
}
static turbodb_status_t subquery_pull(void *context,const turbodb_value_t **out,turbodb_error_t *e) {
  orm_sql_scan_row row;
  const turbodb_status_t status=orm_tidesdb_sql_runtime_next(context,&row,e);
  if(status!=TURBODB_STATUS_OK) return status;
  if(row.state==ORM_SQL_SCAN_CANCELLED) { tdsql_error_set(e,TURBODB_STATUS_INVALID_STATE,"test subquery source cancelled"); return TURBODB_STATUS_INVALID_STATE; }
  *out=row.state==ORM_SQL_SCAN_ROW?row.values:NULL; return TURBODB_STATUS_OK;
}
static turbodb_status_t recursive_pull(void *context,const turbodb_value_t **out,turbodb_error_t *e) {
  check_true(context==&recursion); check_not_null(recursion.frontier);
  return recursion.frontier->next(recursion.frontier->context,out,e);
}
static turbodb_status_t recursive_open(void *context,orm_sql_row_source *frontier,orm_sql_row_source **out,turbodb_error_t *e) {
  check_true(context==&recursion); ++recursion.opens; recursion.frontier=frontier;
  const turbodb_status_t status=orm_sql_runtime_execution_open(&query,NULL,0,NULL,e);
  if(status==TURBODB_STATUS_OK) *out=&recursion.member;
  return status;
}
static turbodb_status_t recursive_close(void *context,turbodb_error_t *e) {
  check_true(context==&recursion); ++recursion.closes;
  const turbodb_status_t status=orm_sql_runtime_execution_close(&query,e);
  if(status==TURBODB_STATUS_OK) recursion.frontier=NULL;
  return status;
}
static void open_subquery(orm_sql_subquery_kind kind,const orm_sql_type *probe) {
  orm_sql_schema_column column; check_equal(query.columns,1u);
  check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK);
  subquery_type=column.type;
  subquery_source=(orm_sql_row_source){&budget,&subquery_type,1,&query,subquery_pull,false};
  check_equal(orm_tidesdb_sql_subquery_open(&subquery_source,kind,probe,&subquery,&error),TURBODB_STATUS_OK);
}
static void close_query(orm_sql_query *out) { check_equal(orm_tidesdb_sql_runtime_close(out, &error), TURBODB_STATUS_OK); }
static void resume_query(void) {
  check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_runtime_execution_resume(&query,&error),TURBODB_STATUS_OK);
}
static void next_statement(void) {
  const orm_sql_transaction_budget_amount cumulative = budget.transaction_used;
  check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
  check_false(budget.statement_active);
  check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], budget.retained_work_bytes);
  check_equal(budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES], budget.retained_work_bytes);
  check_equal(budget.transaction_used.read_rows, cumulative.read_rows);
  check_equal(budget.transaction_used.read_bytes, cumulative.read_bytes);
  check_equal(budget.transaction_used.write_bytes, cumulative.write_bytes);
}
static orm_sql_scan_row next(void) {
  orm_sql_scan_row row = {0}; check_equal(orm_tidesdb_sql_runtime_next(&query, &row, &error), TURBODB_STATUS_OK); return row;
}
static void seed(void) {
  size_t affected = 99;
  check_equal(execute(ddl, NULL, 0, &affected), TURBODB_STATUS_OK); check_equal(affected, 0u);
  check_equal(execute("INSERT INTO items(id,score) VALUES(1,10),(2,20),(3,NULL)", NULL, 0, &affected), TURBODB_STATUS_OK);
  check_equal(affected, 3u);
}
static void text_is(vstr value, const char *expected) {
  check_equal(value.len, strlen(expected)); check_equal(memcmp(value.data, expected, value.len), 0);
}
static void reopen(void) {
  close_query(&query); close_query(&other);
  check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_close(database), ORM_TDB_SUCCESS); database = NULL;
  database_open(); family = orm_tidesdb_get_column_family(database, family_name); check_not_null(family); begin();
}
spec("TidesDB private unified SQL runtime") {
  before_each() {
    memset(&recursion,0,sizeof(recursion));
    reserves = resizes = fail_reserve = fail_resize = 0; tdsql_error_init(&error);
    owner = (orm_sql_catalog_store){0}; query = other = (orm_sql_query){0};
    subquery=(orm_sql_subquery){0}; subquery_source=(orm_sql_row_source){0};
    orm_sql_budget_limits limits = {0};
    for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) limits.statement.value[i] = LIMIT;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = WORK;
    limits.transaction = (orm_sql_transaction_budget_amount){LIMIT, LIMIT, LIMIT};
    check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    directory = tt_make_temp_dir("orm-sql-runtime"); check_not_null(directory); database_open();
    orm_tidesdb_column_family_config_t config = orm_tidesdb_default_column_family_config(); config.sync_mode = ORM_TDB_SYNC_FULL;
    check_equal(orm_tidesdb_create_column_family(database, family_name, &config), ORM_TDB_SUCCESS);
    family = orm_tidesdb_get_column_family(database, family_name); check_not_null(family);
    check_equal(orm_tidesdb_sql_catalog_initialize(database, family, MAX_RECORD, &budget, &error), TURBODB_STATUS_OK); begin();
  }
  after_each() {
    fail_reserve = fail_resize = 0;
    check_equal(orm_sql_cte_reader_close(&recursion.reader,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_store_close(&recursion.store,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_subquery_close(&subquery,&error),TURBODB_STATUS_OK);
    close_query(&query); close_query(&other);
    orm_sql_diagnostics_destroy(&evaluation_diagnostics);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    check_equal(budget.retained_work_bytes, 0u);
    if (budget.statement_active) check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_close(database), ORM_TDB_SUCCESS); database = NULL; family = NULL;
    check_equal(tt_remove_tree(directory), 0); free(directory); directory = NULL;
  }
  group("type-only write binding for #206") {
    it("rejects invalid scopes mismatched owners and poisoned owners before binding work") {
      seed(); sqlparser_document *document = parse("UPDATE items SET score=?");
      orm_sql_query_scope scope = {.document=document,.root=sqlparser_statements(document).first,
          .parameter_count=1,.max_depth=DEPTH,.budget=&budget};
      typedef turbodb_status_t (*write_binder)(const orm_sql_query_scope *,orm_sql_catalog_store *,turbodb_error_t *);
      const write_binder binders[] = {orm_sql_insert_bind,orm_sql_change_bind};
      for (size_t i = 0; i < sizeof(binders)/sizeof(binders[0]); ++i) {
        const uint64_t steps = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
        check_equal(binders[i](NULL,&owner,&error),TURBODB_STATUS_INVALID_ARGUMENT);
        check_equal(binders[i](&scope,NULL,&error),TURBODB_STATUS_INVALID_ARGUMENT);
        check_equal(binders[i](&scope,&owner,&error),TURBODB_STATUS_INVALID_ARGUMENT);
        orm_tidesdb_sql_budget different = {0}; scope.budget = &different;
        check_equal(binders[i](&scope,&owner,&error),TURBODB_STATUS_INVALID_ARGUMENT); scope.budget = &budget;
        owner.failed = true;
        check_equal(binders[i](&scope,&owner,&error),TURBODB_STATUS_INVALID_STATE); owner.failed = false;
        check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
      }
      sqlparser_document_destroy(document);
    }
    it("compiles INSERT VALUES and SET without evaluating errors or applying conversions") {
      seed(); const uint64_t version = table_version();
      const char *const sql[] = {"INSERT INTO items VALUES(4,1.0/0.0)",
        "INSERT INTO items SET id=4,score=CAST('invalid' AS SIGNED)",
        "INSERT INTO items VALUES(4,'invalid')", "INSERT INTO items VALUES(NULL,20)"};
      for (size_t i = 0; i < sizeof(sql)/sizeof(sql[0]); ++i)
        check_equal(bind_write(sql[i],NULL,0),TURBODB_STATUS_OK);
      check_equal(table_version(),version);
      size_t affected = SIZE_MAX;
      check_equal(execute(sql[0],NULL,0,&affected),TURBODB_STATUS_SQL_ERROR);
      check_equal(affected,SIZE_MAX); check_equal(table_version(),version);
    }
    it("compiles UPDATE and DELETE expressions without evaluating predicates or assignments") {
      seed(); const uint64_t version = table_version();
      const char *const sql[] = {"UPDATE items SET score=1.0/0.0 WHERE id=1",
        "DELETE FROM items WHERE id=1.0/0.0", "UPDATE items SET score=DEFAULT WHERE id=1",
        "UPDATE items SET score=25 ORDER BY score+1.0/0.0 LIMIT 1"};
      for (size_t i = 0; i < sizeof(sql)/sizeof(sql[0]); ++i)
        check_equal(bind_write(sql[i],NULL,0),TURBODB_STATUS_OK);
      check_equal(table_version(),version);
    }
    it("validates replacement ignore duplicate-key assignments and aliases without warnings") {
      seed(); check_equal(orm_sql_diagnostics_init(&evaluation_diagnostics,2,&error),TURBODB_STATUS_OK);
      check_equal(orm_sql_diagnostics_add(&evaluation_diagnostics,ORM_SQL_DIAGNOSTIC_CAST_TRUNCATED,
          "retained warning",&error),TURBODB_STATUS_OK);
      const uint64_t version = table_version();
      const char *const sql[] = {"REPLACE INTO items VALUES(1,1.0/0.0)",
        "INSERT IGNORE INTO items(score) VALUES(1.0/0.0)",
        "INSERT INTO items VALUES(1,20) ON DUPLICATE KEY UPDATE score=1.0/0.0",
        "INSERT INTO items VALUES(1,20) AS incoming ON DUPLICATE KEY UPDATE score=incoming.score",
        "INSERT INTO items VALUES(1,20) ON DUPLICATE KEY UPDATE score=VALUES(score)",
        "INSERT IGNORE INTO items SET score=DEFAULT"};
      for (size_t i = 0; i < sizeof(sql)/sizeof(sql[0]); ++i)
        check_equal(bind_write(sql[i],NULL,0),TURBODB_STATUS_OK);
      check_equal(evaluation_diagnostics.total,1u); check_equal(table_version(),version);
      check_equal(orm_sql_diagnostics_at(&evaluation_diagnostics,0)->message,"retained warning");
    }
    it("binds nullable parameter types without values or NULL placeholders") {
      seed();
      const orm_sql_type types[] = {{TURBODB_VALUE_NULL,true},{TURBODB_VALUE_INT64,true},
        {TURBODB_VALUE_UINT64,true},{TURBODB_VALUE_DOUBLE,true},
        {TURBODB_VALUE_BOOLEAN,true},{TURBODB_VALUE_TEXT,true}};
      const orm_sql_type key = {TURBODB_VALUE_INT64,false};
      for (size_t i = 0; i < sizeof(types)/sizeof(types[0]); ++i) {
        const orm_sql_type pair[] = {key,types[i]};
        check_equal(bind_write("INSERT INTO items VALUES(?,?)",pair,2),TURBODB_STATUS_OK);
        check_equal(bind_write("UPDATE items SET score=?",&types[i],1),TURBODB_STATUS_OK);
      }
      const orm_sql_type blob = {TURBODB_VALUE_BLOB,true};
      check_equal(bind_write("UPDATE items SET score=?",&blob,1),TURBODB_STATUS_OK);
      check_equal(bind_write("DELETE FROM items WHERE ? IS NULL",&blob,1),TURBODB_STATUS_OK);
    }
    it("checks LIMIT type at binding and leaves value range checks to execution") {
      seed(); const orm_sql_type integral[] = {{TURBODB_VALUE_INT64,true},{TURBODB_VALUE_UINT64,true}};
      for (size_t i = 0; i < sizeof(integral)/sizeof(integral[0]); ++i) {
        check_equal(bind_write("DELETE FROM items LIMIT ?",&integral[i],1),TURBODB_STATUS_OK);
        check_equal(bind_write("UPDATE items SET score=20 LIMIT ?",&integral[i],1),TURBODB_STATUS_OK);
      }
      const orm_sql_type wrong = {TURBODB_VALUE_BOOLEAN,false};
      check_equal(bind_write("DELETE FROM items LIMIT ?",&wrong,1),TURBODB_STATUS_TYPE_ERROR);
      check_equal(bind_write("DELETE FROM items LIMIT -1",NULL,0),TURBODB_STATUS_UNSUPPORTED);
      const turbodb_value_t negative = turbodb_i64(-1); size_t affected = SIZE_MAX;
      check_equal(execute("DELETE FROM items LIMIT ?",&negative,1,&affected),TURBODB_STATUS_TYPE_ERROR);
      check_equal(affected,SIZE_MAX);
    }
    it("rejects wrong columns defaults row shapes marker counts and parameter types") {
      seed(); const uint64_t version = table_version();
      const char *const invalid[] = {"INSERT INTO items(id,missing) VALUES(4,20)",
        "INSERT INTO items(id,id) VALUES(4,4)","INSERT INTO items(score) VALUES(20)",
        "INSERT INTO items VALUES(4)","INSERT INTO items SET missing=20",
        "UPDATE items SET missing=20", "DELETE FROM items WHERE missing=1",
        "INSERT INTO items VALUES(4,20) ON DUPLICATE KEY UPDATE missing=1",
        "INSERT INTO missing VALUES(4,20)"};
      for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i)
        check_equal(bind_write(invalid[i],NULL,0),TURBODB_STATUS_SQL_ERROR);
      check_equal(bind_write("UPDATE items SET score=?",NULL,0),TURBODB_STATUS_SQL_ERROR);
      const orm_sql_type wrong = {(turbodb_value_kind_t)INT32_MAX,false};
      check_equal(bind_write("UPDATE items SET score=?",&wrong,1),TURBODB_STATUS_TYPE_ERROR);
      check_equal(table_version(),version);
    }
    it("rejects query values outside the existing INSERT execution subset") {
      seed();
      const char *const sql[] = {"INSERT INTO items VALUES(4,(SELECT score FROM items WHERE id=1))",
        "INSERT INTO items SET id=4,score=(SELECT 10)",
        "INSERT INTO items VALUES(4,20) ON DUPLICATE KEY UPDATE score=(SELECT 10)"};
      for (size_t i = 0; i < sizeof(sql)/sizeof(sql[0]); ++i)
        check_equal(bind_write(sql[i],NULL,0),TURBODB_STATUS_UNSUPPORTED);
      size_t affected = 0;
      check_equal(execute("INSERT INTO items SELECT id+10,score FROM items",NULL,0,&affected),TURBODB_STATUS_OK);
      check_equal(affected,3u);
    }
    it("does not read corrupted business rows while binding target metadata") {
      seed(); corrupt_first_row();
      check_equal(bind_write("UPDATE items SET score=25 WHERE id=1",NULL,0),TURBODB_STATUS_OK);
      check_equal(bind_write("DELETE FROM items WHERE id=1",NULL,0),TURBODB_STATUS_OK);
      check_equal(bind_write("REPLACE INTO items VALUES(1,20)",NULL,0),TURBODB_STATUS_OK);
      check_equal(open_query("SELECT id FROM items",NULL,0,&query),TURBODB_STATUS_OK);
      orm_sql_scan_row row = {0};
      check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_DATASTORE_ERROR);
    }
    it("refunds every partial reserve and resize failure during type-only binding") {
      seed(); const orm_sql_type types[] = {{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,true}};
      const char *const sql[] = {"INSERT INTO items VALUES(?,?) ON DUPLICATE KEY UPDATE score=VALUES(score)",
        "UPDATE items SET score=? WHERE id=? ORDER BY score+1 LIMIT 1"};
      for (size_t i = 0; i < sizeof(sql)/sizeof(sql[0]); ++i) {
        const size_t reserve_before = reserves, resize_before = resizes;
        check_equal(bind_write(sql[i],types,2),TURBODB_STATUS_OK);
        const size_t reserve_count = reserves-reserve_before, resize_count = resizes-resize_before;
        check_greater(reserve_count,0u); check_greater(resize_count,0u);
        for (size_t allocation = 1; allocation <= reserve_count; ++allocation) {
          fail_reserve = reserves+allocation;
          check_equal(bind_write(sql[i],types,2),TURBODB_STATUS_OUT_OF_MEMORY); fail_reserve = 0;
        }
        for (size_t allocation = 1; allocation <= resize_count; ++allocation) {
          fail_resize = resizes+allocation;
          check_equal(bind_write(sql[i],types,2),TURBODB_STATUS_OUT_OF_MEMORY); fail_resize = 0;
        }
      }
    }
    it("fails at the existing step and work budget boundaries without poisoning the owner") {
      seed(); const orm_sql_budget_limits limits = budget.limits;
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(bind_write("UPDATE items SET score=20",NULL,0),TURBODB_STATUS_LIMIT_EXCEEDED);
      budget.limits = limits;
      budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(bind_write("INSERT INTO items VALUES(4,20)",NULL,0),TURBODB_STATUS_LIMIT_EXCEEDED);
      budget.limits = limits;
      check_equal(bind_write("INSERT INTO items VALUES(4,20)",NULL,0),TURBODB_STATUS_OK);
    }
  }
  group("type-only query metadata for #206") {
    it("owns all seven parameter result kinds after the AST and type inputs expire") {
      orm_sql_type types[] = {{TURBODB_VALUE_NULL,true},{TURBODB_VALUE_INT64,false},
        {TURBODB_VALUE_UINT64,true},{TURBODB_VALUE_DOUBLE,true},{TURBODB_VALUE_BOOLEAN,false},
        {TURBODB_VALUE_TEXT,true},{TURBODB_VALUE_BLOB,true}};
      const char *const names[] = {"n","i","u","d","b","t","bytes"};
      check_equal(bind_query("SELECT ? AS n,? AS i,? AS u,? AS d,? AS b,? AS t,? AS bytes",
          types,sizeof(types)/sizeof(types[0])),TURBODB_STATUS_OK);
      check_equal(query.columns,sizeof(types)/sizeof(types[0])); check_true(query.execution_closed);
      check_false(query.parameters.values.initialized); check_false(query.statement_parameters);
      for (size_t i = 0; i < sizeof(types)/sizeof(types[0]); ++i) {
        const orm_sql_type expected = types[i]; types[i] = (orm_sql_type){TURBODB_VALUE_NULL,true};
        orm_sql_schema_column column = {0};
        check_equal(orm_tidesdb_sql_runtime_column(&query,i,&column,&error),TURBODB_STATUS_OK);
        text_is(column.name,names[i]); check_equal(column.type.kind,expected.kind);
        check_equal(column.type.nullable,expected.nullable);
      }
      orm_sql_scan_row row = {.state=ORM_SQL_SCAN_ROW,.count=SIZE_MAX};
      check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_INVALID_STATE);
      check_equal(row.count,SIZE_MAX); check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_INVALID_STATE);
    }
    it("compiles computed projection and WHERE without executing division or resetting warnings") {
      seed(); check_equal(orm_sql_diagnostics_init(&evaluation_diagnostics,2,&error),TURBODB_STATUS_OK);
      check_equal(orm_sql_diagnostics_add(&evaluation_diagnostics,ORM_SQL_DIAGNOSTIC_CAST_TRUNCATED,
          "existing warning",&error),TURBODB_STATUS_OK);
      const uint64_t version = table_version();
      const orm_sql_type type = {TURBODB_VALUE_INT64,true};
      check_equal(bind_query("SELECT score+? AS value,1.0/0.0 AS division FROM items WHERE id>0",
          &type,1),TURBODB_STATUS_OK); check_equal(query.columns,2u);
      check_null(query.as.select.source.iterator); check_true(query.execution_closed);
      orm_sql_schema_column column = {0};
      check_equal(orm_tidesdb_sql_runtime_column(&query,1,&column,&error),TURBODB_STATUS_OK);
      check_equal(column.type.kind,TURBODB_VALUE_DOUBLE); check_equal(evaluation_diagnostics.total,1u);
      check_equal(table_version(),version);
    }
    it("returns ordinary JOIN output metadata with outer-join nullability") {
      seed(); check_equal(bind_query("SELECT a.id AS left_id,b.id AS right_id FROM items AS a "
          "LEFT JOIN items AS b ON a.id=b.id",NULL,0),TURBODB_STATUS_OK);
      check_equal(query.columns,2u); orm_sql_schema_column column = {0};
      check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK);
      text_is(column.name,"left_id"); check_equal(column.type.kind,TURBODB_VALUE_INT64); check_false(column.type.nullable);
      check_equal(orm_tidesdb_sql_runtime_column(&query,1,&column,&error),TURBODB_STATUS_OK);
      text_is(column.name,"right_id"); check_true(column.type.nullable);
      check_true(query.execution_closed); check_null(query.as.select.from_run.plan);
    }
    it("binds grouping and parameter window frames without actual parameter values") {
      seed(); check_equal(bind_query("SELECT id,COUNT(*) AS count,MIN(score) AS low FROM items GROUP BY id",
          NULL,0),TURBODB_STATUS_OK); check_equal(query.columns,3u); close_query(&query);
      const orm_sql_type types[] = {{TURBODB_VALUE_INT64,true},{TURBODB_VALUE_INT64,true}};
      check_equal(bind_query("SELECT COUNT(*) OVER(ORDER BY id ROWS ? PRECEDING) AS count FROM items LIMIT ?",
          types,2),TURBODB_STATUS_OK); check_equal(query.columns,1u); check_true(query.execution_closed);
      orm_sql_schema_column column = {0};
      check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK);
      text_is(column.name,"count"); check_equal(column.type.kind,TURBODB_VALUE_INT64);
    }
    it("binds nested set queries and their parameter tails with the actual business metadata") {
      const orm_sql_type types[] = {{TURBODB_VALUE_INT64,true},{TURBODB_VALUE_DOUBLE,false},
        {TURBODB_VALUE_INT64,false}};
      check_equal(bind_query("(SELECT ? AS value UNION ALL SELECT ? AS value) ORDER BY value LIMIT ?",
          types,3),TURBODB_STATUS_OK); check_equal(query.columns,1u); check_true(query.execution_closed);
      orm_sql_schema_column column = {0};
      check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK);
      text_is(column.name,"value"); check_equal(column.type.kind,TURBODB_VALUE_DOUBLE); check_true(column.type.nullable);
    }
    it("rejects unsupported set comparison types and branch shape during binding") {
      const char *const sql[] = {"SELECT 'a' AS n UNION SELECT 'b' AS n",
        "SELECT 'a' AS n INTERSECT SELECT 'b' AS n", "SELECT 'a' AS n EXCEPT SELECT 'b' AS n"};
      const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for (size_t i = 0; i < sizeof(sql)/sizeof(sql[0]); ++i) {
        check_equal(bind_query(sql[i],NULL,0),TURBODB_STATUS_UNSUPPORTED); check_null(query.owner);
        check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      check_equal(bind_query("SELECT 1 AS n UNION ALL SELECT 2 AS n,3 AS m",NULL,0),TURBODB_STATUS_SQL_ERROR);
      check_null(query.owner); check_equal(owner.active_sources,0u);
    }
    it("validates marker counts and types without inventing missing values") {
      seed(); const orm_sql_type type = {TURBODB_VALUE_INT64,false};
      const orm_sql_type invalid = {(turbodb_value_kind_t)INT32_MAX,false};
      check_equal(bind_query("SELECT ? AS n",NULL,0),TURBODB_STATUS_SQL_ERROR);
      check_equal(bind_query("SELECT id FROM items",&type,1),TURBODB_STATUS_SQL_ERROR);
      check_equal(bind_query("SELECT ? AS n",&invalid,1),TURBODB_STATUS_TYPE_ERROR);
      check_equal(bind_query("SELECT missing FROM items",NULL,0),TURBODB_STATUS_SQL_ERROR);
      check_null(query.owner); check_equal(owner.active_sources,0u);
    }
    it("requires explicit execution parameters and keeps real LIMIT checks in execution") {
      seed(); const orm_sql_type type = {TURBODB_VALUE_INT64,true};
      check_equal(bind_query("SELECT id FROM items ORDER BY id LIMIT ?",&type,1),TURBODB_STATUS_OK);
      check_equal(orm_sql_runtime_execution_open(&query,NULL,1,NULL,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      check_true(query.execution_closed);
      const turbodb_value_t negative = turbodb_i64(-1);
      check_equal(orm_sql_runtime_execution_open(&query,&negative,1,NULL,&error),TURBODB_STATUS_TYPE_ERROR);
      close_query(&query);
      check_equal(bind_query("SELECT id FROM items ORDER BY id LIMIT ?",&type,1),TURBODB_STATUS_OK);
      const turbodb_value_t limit = turbodb_i64(1);
      check_equal(orm_sql_runtime_execution_open(&query,&limit,1,NULL,&error),TURBODB_STATUS_OK);
      orm_sql_scan_row row = {0};
      check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,1);
      check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
    }
    it("binds INSERT SELECT and duplicate-key expressions without materializing or writing rows") {
      seed(); const uint64_t version = table_version();
      const orm_sql_type types[] = {{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_DOUBLE,true}};
      const char *const sql[] = {"INSERT INTO items SELECT id+10,score FROM items",
        "INSERT INTO items SELECT ? AS id,? AS score", "REPLACE INTO items SELECT id,score FROM items",
        "INSERT IGNORE INTO items SELECT id,1.0/0.0 AS score FROM items",
        "INSERT INTO items SELECT id,score FROM items ON DUPLICATE KEY UPDATE score=1.0/0.0",
        "INSERT INTO items SELECT 4 AS id,20 AS score UNION ALL SELECT 5 AS id,25 AS score"};
      for (size_t i = 0; i < sizeof(sql)/sizeof(sql[0]); ++i)
        check_equal(bind_write(sql[i],i==1?types:NULL,i==1?2:0),TURBODB_STATUS_OK);
      check_equal(table_version(),version);
      check_equal(bind_write("INSERT INTO items SELECT id FROM items",NULL,0),TURBODB_STATUS_SQL_ERROR);
      check_equal(bind_write("INSERT INTO items(id) SELECT id,score FROM items",NULL,0),TURBODB_STATUS_SQL_ERROR);
    }
    it("does not read corrupted Data during query or INSERT SELECT binding") {
      seed(); corrupt_first_row(); const uint64_t version = table_version();
      const char *const sql[] = {"SELECT id,score FROM items", "SELECT COUNT(*) AS count FROM items",
        "SELECT a.id AS id FROM items AS a JOIN items AS b ON a.id=b.id",
        "SELECT id FROM items UNION ALL SELECT id FROM items"};
      for (size_t i = 0; i < sizeof(sql)/sizeof(sql[0]); ++i) {
        check_equal(bind_query(sql[i],NULL,0),TURBODB_STATUS_OK); close_query(&query);
      }
      check_equal(bind_write("INSERT INTO items SELECT id+10,score FROM items",NULL,0),TURBODB_STATUS_OK);
      check_equal(table_version(),version);
      check_equal(open_query(sql[0],NULL,0,&query),TURBODB_STATUS_OK);
      orm_sql_scan_row row = {0};
      check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_DATASTORE_ERROR);
    }
    it("rejects non-query forms and preserves an existing output owner") {
      seed(); const char *const sql[] = {"SHOW COLUMNS FROM items","UPDATE items SET score=10"};
      for (size_t i = 0; i < sizeof(sql)/sizeof(sql[0]); ++i) {
        check_equal(bind_query(sql[i],NULL,0),TURBODB_STATUS_UNSUPPORTED); check_null(query.owner);
      }
      check_equal(bind_query("SELECT id FROM items",NULL,0),TURBODB_STATUS_OK);
      orm_sql_catalog_store *original = query.owner;
      check_equal(bind_query("SELECT id FROM items",NULL,0),TURBODB_STATUS_INVALID_ARGUMENT);
      check_true(query.owner == original);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_BUSY);
      close_query(&query); check_equal(owner.active_sources,0u);
    }
    it("refunds partial allocation failures including JOIN compound and INSERT SELECT binding") {
      seed(); const orm_sql_type type = {TURBODB_VALUE_INT64,true};
      const char *const sql[] = {"SELECT a.id AS id FROM items AS a JOIN items AS b ON a.id=b.id WHERE a.id>?",
        "SELECT ? AS id UNION ALL SELECT id FROM items"};
      for (size_t i = 0; i < sizeof(sql)/sizeof(sql[0]); ++i) {
        const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
        const size_t reserve_before = reserves, resize_before = resizes;
        check_equal(bind_query(sql[i],&type,1),TURBODB_STATUS_OK); close_query(&query);
        const size_t reserve_count = reserves-reserve_before, resize_count = resizes-resize_before;
        for (size_t allocation = 1; allocation <= reserve_count; ++allocation) {
          fail_reserve = reserves+allocation;
          check_equal(bind_query(sql[i],&type,1),TURBODB_STATUS_OUT_OF_MEMORY); fail_reserve = 0;
          close_query(&query); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
          check_equal(owner.active_sources,0u);
        }
        for (size_t allocation = 1; allocation <= resize_count; ++allocation) {
          fail_resize = resizes+allocation;
          check_equal(bind_query(sql[i],&type,1),TURBODB_STATUS_OUT_OF_MEMORY); fail_resize = 0;
          close_query(&query); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
          check_equal(owner.active_sources,0u);
        }
      }
      const char *const insert = "INSERT INTO items SELECT id+?,score FROM items ON DUPLICATE KEY UPDATE score=VALUES(score)";
      const size_t before = reserves;
      check_equal(bind_write(insert,&type,1),TURBODB_STATUS_OK);
      const size_t allocations = reserves-before;
      for (size_t allocation = 1; allocation <= allocations; ++allocation) {
        fail_reserve = reserves+allocation;
        check_equal(bind_write(insert,&type,1),TURBODB_STATUS_OUT_OF_MEMORY); fail_reserve = 0;
      }
    }
    it("fails at work and step capacity without retaining plans or leases") {
      seed(); const orm_sql_budget_limits limits = budget.limits;
      const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = work;
      check_equal(bind_query("SELECT id FROM items",NULL,0),TURBODB_STATUS_LIMIT_EXCEEDED);
      budget.limits = limits;
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(bind_query("SELECT id FROM items",NULL,0),TURBODB_STATUS_LIMIT_EXCEEDED);
      budget.limits = limits; check_null(query.owner); check_equal(owner.active_sources,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  group("type-only dependency metadata for #206") {
    it("owns all seven result types through a derived table with no parameter snapshots") {
      orm_sql_type types[] = {{TURBODB_VALUE_NULL,true},{TURBODB_VALUE_INT64,false},
        {TURBODB_VALUE_UINT64,true},{TURBODB_VALUE_DOUBLE,true},{TURBODB_VALUE_BOOLEAN,false},
        {TURBODB_VALUE_TEXT,true},{TURBODB_VALUE_BLOB,true}};
      check_equal(bind_query("SELECT d.* FROM (SELECT ? AS n,? AS i,? AS u,? AS d,? AS b,? AS t,? AS bytes) d",
          types,sizeof(types)/sizeof(types[0])),TURBODB_STATUS_OK);
      check_equal(query.columns,sizeof(types)/sizeof(types[0])); check_true(query.execution_closed);
      check_true(query.dependencies.binding_only); check_true(query.dependencies.describe);
      check_false(query.parameters.values.initialized); check_false(query.statement_parameters);
      check_false(query.dependencies.explained.budget);
      for(size_t i=0;i<sizeof(types)/sizeof(types[0]);++i) {
        const orm_sql_type expected=types[i]; types[i]=(orm_sql_type){TURBODB_VALUE_NULL,true};
        orm_sql_schema_column column={0};
        check_equal(orm_tidesdb_sql_runtime_column(&query,i,&column,&error),TURBODB_STATUS_OK);
        check_equal(column.type.kind,expected.kind); check_equal(column.type.nullable,expected.nullable);
      }
      for(size_t i=0;i<query.dependencies.derived_count;++i) {
        const orm_sql_derived_binding *binding=vec_at_const(&query.dependencies.derived,i);
        check_not_null(binding->schema); check_null(binding->source); check_null(binding->input);
      }
      orm_sql_scan_row row={.count=SIZE_MAX};
      check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_INVALID_STATE);
      check_equal(row.count,SIZE_MAX);
      check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_INVALID_STATE);
      check_equal(orm_sql_runtime_execution_open(&query,NULL,0,NULL,&error),TURBODB_STATUS_UNSUPPORTED);
      check_equal(orm_sql_runtime_execution_resume(&query,&error),TURBODB_STATUS_UNSUPPORTED);
    }
    it("binds scalar IN and EXISTS parameters with their actual output kinds") {
      const orm_sql_type types[]={{TURBODB_VALUE_DOUBLE,true},{TURBODB_VALUE_INT64,true},
        {TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,true}};
      check_equal(bind_query("SELECT (SELECT ?) AS scalar_value,? IN (SELECT ?) AS member,"
          "EXISTS(SELECT 1 WHERE ? > 0) AS present",types,4),TURBODB_STATUS_OK);
      const turbodb_value_kind_t kinds[]={TURBODB_VALUE_DOUBLE,TURBODB_VALUE_BOOLEAN,TURBODB_VALUE_BOOLEAN};
      const bool nullable[]={true,true,false}; check_equal(query.columns,3u);
      check_equal(query.dependencies.query_count,3u);
      for(size_t i=0;i<sizeof(kinds)/sizeof(kinds[0]);++i) {
        orm_sql_schema_column column={0};
        check_equal(orm_tidesdb_sql_runtime_column(&query,i,&column,&error),TURBODB_STATUS_OK);
        check_equal(column.type.kind,kinds[i]); check_equal(column.type.nullable,nullable[i]);
        check_null(*(orm_sql_expr_query_source *const *)vec_at_const(&query.dependencies.sources,i));
      }
      close_query(&query);
      const orm_sql_type limit={TURBODB_VALUE_INT64,true};
      check_equal(bind_query("SELECT (SELECT 1 LIMIT ?) AS value",&limit,1),TURBODB_STATUS_OK);
      check_true(query.execution_closed);
    }
    it("binds shared nonrecursive CTE schemas without opening their row caches") {
      seed(); const uint64_t version=table_version();
      const orm_sql_type type={TURBODB_VALUE_INT64,true};
      check_equal(bind_query("WITH q(k,v) AS(SELECT id,score+? FROM items) "
          "SELECT a.k AS id,b.v AS score,(SELECT v FROM q WHERE k=a.k) AS nested "
          "FROM q a LEFT JOIN q b ON a.k=b.k",&type,1),TURBODB_STATUS_OK);
      check_equal(query.columns,3u); check_true(query.dependencies.derived_count>=3);
      check_true(query.dependencies.binding_only); check_equal(table_version(),version);
      orm_sql_schema_column column={0};
      check_equal(orm_tidesdb_sql_runtime_column(&query,1,&column,&error),TURBODB_STATUS_OK);
      text_is(column.name,"score"); check_equal(column.type.kind,TURBODB_VALUE_INT64); check_true(column.type.nullable);
    }
    it("propagates nested correlated captures through grouped business columns") {
      seed(); const orm_sql_type type={TURBODB_VALUE_INT64,true};
      const char *const sql[]={
        "SELECT a.id,(SELECT (SELECT a.score+?)) AS value FROM items a",
        "SELECT a.id,(SELECT a.id+?) AS value,COUNT(*) AS count FROM items a GROUP BY a.id",
        "SELECT a.id,(SELECT b.score+? FROM items b WHERE b.id=a.id) AS value FROM items a"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        check_equal(bind_query(sql[i],&type,1),TURBODB_STATUS_OK); check_true(query.dependencies.binding_only);
        orm_sql_schema_column column={0};
        check_equal(orm_tidesdb_sql_runtime_column(&query,1,&column,&error),TURBODB_STATUS_OK);
        check_equal(column.type.kind,TURBODB_VALUE_INT64); check_true(column.type.nullable); close_query(&query);
      }
      check_equal(bind_query("SELECT a.id,(SELECT a.score) AS value,COUNT(*) AS count "
          "FROM items a GROUP BY a.id",NULL,0),TURBODB_STATUS_SQL_ERROR);
      check_null(query.owner); check_equal(owner.active_sources,0u);
    }
    it("orders lateral schemas and preserves whole-document marker types") {
      const orm_sql_type types[]={{TURBODB_VALUE_BOOLEAN,false},{TURBODB_VALUE_INT64,true},
        {TURBODB_VALUE_DOUBLE,false},{TURBODB_VALUE_INT64,true}};
      check_equal(bind_query("SELECT ? AS flag,e.n FROM (SELECT ? AS n) a,"
          "LATERAL (SELECT ?+a.n AS n) d,LATERAL (SELECT ?+d.n AS n) e",types,4),TURBODB_STATUS_OK);
      check_equal(query.columns,2u); check_equal(query.dependencies.derived_count,3u);
      orm_sql_schema_column column={0};
      check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK);
      check_equal(column.type.kind,TURBODB_VALUE_BOOLEAN); check_false(column.type.nullable);
      check_equal(orm_tidesdb_sql_runtime_column(&query,1,&column,&error),TURBODB_STATUS_OK);
      check_equal(column.type.kind,TURBODB_VALUE_DOUBLE); check_true(column.type.nullable);
    }
    it("compiles dependency arithmetic without evaluating or reading corrupted business rows") {
      seed(); corrupt_first_row(); const uint64_t version=table_version();
      check_equal(orm_sql_diagnostics_init(&evaluation_diagnostics,2,&error),TURBODB_STATUS_OK);
      check_equal(orm_sql_diagnostics_add(&evaluation_diagnostics,ORM_SQL_DIAGNOSTIC_CAST_TRUNCATED,
          "existing warning",&error),TURBODB_STATUS_OK);
      const char *const sql[]={"SELECT (SELECT score FROM items) AS value",
        "WITH q AS(SELECT id,score FROM items) SELECT score FROM q",
        "SELECT d.value FROM items a,LATERAL (SELECT a.score+9223372036854775807 AS value) d",
        "SELECT (SELECT 1.0/0.0) AS value", "SELECT d.value FROM (SELECT CAST('bad' AS SIGNED) AS value) d",
        "SELECT (SELECT b.score FROM items b WHERE b.id=a.id) AS value FROM items a"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        check_equal(bind_query(sql[i],NULL,0),TURBODB_STATUS_OK); close_query(&query);
        check_equal(owner.active_sources,0u); check_equal(evaluation_diagnostics.total,1u);
      }
      check_equal(table_version(),version);
      check_equal(open_query("SELECT score FROM items",NULL,0,&query),TURBODB_STATUS_OK);
      orm_sql_scan_row row={0};
      check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_DATASTORE_ERROR);
    }
    it("rejects invalid dependency output shapes names and lexical capture order") {
      seed(); const char *const sql[]={"SELECT (SELECT id,score FROM items) AS value",
        "SELECT 1 IN(SELECT id,score FROM items) AS value", "SELECT 1 IN(SELECT id FROM items LIMIT 1) AS value",
        "SELECT d.value FROM (SELECT 1 AS n) d",
        "SELECT d.n FROM LATERAL (SELECT a.n AS n) d,(SELECT 2 AS n) a",
        "SELECT d.n FROM (SELECT 2 AS n) a,(SELECT a.n AS n) d"};
      const turbodb_status_t statuses[]={TURBODB_STATUS_SQL_ERROR,TURBODB_STATUS_SQL_ERROR,
        TURBODB_STATUS_UNSUPPORTED,TURBODB_STATUS_SQL_ERROR,TURBODB_STATUS_SQL_ERROR,TURBODB_STATUS_SQL_ERROR};
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        check_equal(bind_query(sql[i],NULL,0),statuses[i]); check_null(query.owner);
        check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
    }
    it("binds recursive definitions only with a positive iteration budget") {
      const char *const sql="WITH RECURSIVE q(n) AS(SELECT 1 UNION ALL SELECT n+1 FROM q WHERE n<3) SELECT n FROM q";
      check_equal(bind_query(sql,NULL,0),TURBODB_STATUS_UNSUPPORTED);
      sqlparser_document *document=parse(sql);
      const orm_sql_query_scope scope={.document=document,.root=sqlparser_statements(document).first,
        .max_depth=DEPTH,.max_iterations=DEPTH,.budget=&budget};
      check_equal(orm_sql_runtime_query_bind(&scope,&owner,&query,&error),TURBODB_STATUS_OK);
      sqlparser_document_destroy(document);
      close_query(&query); check_equal(owner.active_sources,0u);
    }
    it("refunds every allocation failure in correlated CTE and lateral metadata graphs") {
      seed(); const orm_sql_type type={TURBODB_VALUE_INT64,true};
      const char *const sql[]={"WITH q AS(SELECT id,score+? AS score FROM items) "
          "SELECT (SELECT score FROM q WHERE id=a.id) AS value FROM items a",
        "SELECT d.n FROM (SELECT ? AS n) a,LATERAL (SELECT (SELECT a.n+1) AS n) d"};
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        next_statement(); reserves=resizes=0;
        check_equal(bind_query(sql[i],&type,1),TURBODB_STATUS_OK);
        const size_t allocations[]={reserves,resizes}; close_query(&query);
        for(size_t pass=0;pass<sizeof(allocations)/sizeof(allocations[0]);++pass) {
          for(size_t point=1;point<=allocations[pass];++point) {
            next_statement(); reserves=resizes=0;
            if(pass) fail_resize=point; else fail_reserve=point;
            check_equal(bind_query(sql[i],&type,1),TURBODB_STATUS_OUT_OF_MEMORY);
            fail_reserve=fail_resize=0; close_query(&query);
            check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_equal(owner.active_sources,0u);
          }
        }
      }
    }
    it("fails at dependency plan work and step limits with complete cleanup") {
      seed(); const char *const sql="SELECT (SELECT b.score FROM items b WHERE b.id=a.id) AS value FROM items a";
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      const orm_sql_budget_limits limits=budget.limits;
      const orm_sql_budget_resource resources[]={ORM_SQL_BUDGET_PLAN_NODES,ORM_SQL_BUDGET_WORK_BYTES,
        ORM_SQL_BUDGET_EXECUTION_STEPS};
      for(size_t i=0;i<sizeof(resources)/sizeof(resources[0]);++i) {
        next_statement(); budget.limits.statement.value[resources[i]]=budget.used.value[resources[i]];
        check_equal(bind_query(sql,NULL,0),TURBODB_STATUS_LIMIT_EXCEEDED); budget.limits=limits;
        check_null(query.owner); check_equal(owner.active_sources,0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
    }
  }
  group("type-only write dependencies for #206") {
    it("binds nonrecursive CTEs in assignments predicates and computed ordering without writes") {
      seed(); const uint64_t version=table_version();
      const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,true}};
      const char *const sql[]={
        "WITH c(k) AS(SELECT id FROM items WHERE id>=?) UPDATE items SET score=id*10+? WHERE id IN(SELECT k FROM c)",
        "WITH c(v) AS(SELECT ?) UPDATE items SET score=(SELECT v FROM c) WHERE id=?",
        "WITH c(k) AS(SELECT id FROM items WHERE score=?) DELETE FROM items WHERE id IN(SELECT k FROM c) LIMIT ?",
        "WITH c(k) AS(SELECT id FROM items WHERE id>=?) UPDATE items SET score=? ORDER BY (SELECT k FROM c) LIMIT 1"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i)
        check_equal(bind_write(sql[i],types,2),TURBODB_STATUS_OK);
      check_equal(table_version(),version);
      check_equal(open_query("SELECT id,score FROM items ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(next().values[1].data.int64_value,10); check_equal(next().values[1].data.int64_value,20);
      check_equal(next().values[1].kind,TURBODB_VALUE_NULL); check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("binds target aliases nested lexical captures and lateral query dependencies") {
      seed(); const char *const sql[]={
        "UPDATE items o SET score=(SELECT (SELECT o.id+i.id) FROM items i WHERE i.id=2)",
        "DELETE FROM items o WHERE EXISTS(SELECT 1 FROM items i WHERE i.id=o.id AND EXISTS(SELECT 1 WHERE o.id=i.id))",
        "UPDATE items o SET score=(SELECT d.n FROM LATERAL (SELECT o.id+10 AS n) d)",
        "DELETE FROM items o WHERE EXISTS(SELECT 1 FROM LATERAL (SELECT o.id AS n WHERE o.id=2) d)",
        "UPDATE items o SET score=25 ORDER BY (SELECT o.score) LIMIT 1"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i)
        check_equal(bind_write(sql[i],NULL,0),TURBODB_STATUS_OK);
    }
    it("uses source ordered heterogeneous marker types across CTE captures and limits") {
      seed(); const orm_sql_type types[]={{TURBODB_VALUE_DOUBLE,true},{TURBODB_VALUE_BOOLEAN,false},
        {TURBODB_VALUE_TEXT,true},{TURBODB_VALUE_UINT64,false}};
      check_equal(bind_write("WITH c(v) AS(SELECT ?) UPDATE items o SET score=(SELECT v FROM c) "
          "WHERE EXISTS(SELECT 1 WHERE ? AND ? IS NULL AND o.id=1) LIMIT ?",types,4),TURBODB_STATUS_OK);
      const orm_sql_type swapped[]={{TURBODB_VALUE_BOOLEAN,false},{TURBODB_VALUE_DOUBLE,true},
        {TURBODB_VALUE_TEXT,true},{TURBODB_VALUE_UINT64,false}};
      check_equal(bind_write("WITH c(v) AS(SELECT ?) UPDATE items o SET score=(SELECT v FROM c) "
          "WHERE EXISTS(SELECT 1 WHERE ? AND ? IS NULL AND o.id=1) LIMIT ?",swapped,4),TURBODB_STATUS_UNSUPPORTED);
      check_equal(bind_write("UPDATE items SET score=(SELECT ?)",types,0),TURBODB_STATUS_SQL_ERROR);
    }
    it("binds INSERT SELECT CTE derived lateral and scalar dependencies including conflict modifiers") {
      seed(); const orm_sql_type type={TURBODB_VALUE_INT64,true};
      const char *const sql[]={
        "INSERT INTO items WITH c AS(SELECT id,score+? AS score FROM items) SELECT id+10,score FROM c",
        "REPLACE INTO items SELECT id,score FROM (SELECT id,score+? AS score FROM items) d",
        "INSERT IGNORE INTO items SELECT a.id,d.n FROM items a,LATERAL (SELECT a.score+? AS n) d",
        "INSERT INTO items SELECT id,(SELECT score+? FROM items b WHERE b.id=a.id) FROM items a ON DUPLICATE KEY UPDATE score=VALUES(score)",
        "INSERT INTO items SELECT id,score FROM items WHERE EXISTS(SELECT 1 WHERE ? IS NULL) UNION ALL SELECT 9,99"};
      const uint64_t version=table_version();
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i)
        check_equal(bind_write(sql[i],&type,1),TURBODB_STATUS_OK);
      check_equal(table_version(),version);
    }
    it("keeps corrupted business rows and value dependent expression failures unevaluated") {
      seed(); corrupt_first_row(); const uint64_t version=table_version();
      check_equal(orm_sql_diagnostics_init(&evaluation_diagnostics,2,&error),TURBODB_STATUS_OK);
      check_equal(orm_sql_diagnostics_add(&evaluation_diagnostics,ORM_SQL_DIAGNOSTIC_CAST_TRUNCATED,
          "existing warning",&error),TURBODB_STATUS_OK);
      const char *const sql[]={
        "WITH c AS(SELECT score FROM items) UPDATE items SET score=(SELECT score FROM c)",
        "DELETE FROM items o WHERE EXISTS(SELECT 1 FROM items i WHERE i.id=o.id)",
        "INSERT INTO items WITH c AS(SELECT id,score FROM items) SELECT id+10,score FROM c",
        "UPDATE items SET score=(SELECT 1.0/0.0)",
        "UPDATE IGNORE items o SET score=(SELECT CAST('bad' AS SIGNED)+o.score)",
        "INSERT INTO items SELECT 4,d.n FROM (SELECT 9223372036854775807+1 AS n) d"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        check_equal(bind_write(sql[i],NULL,0),TURBODB_STATUS_OK); check_equal(evaluation_diagnostics.total,1u);
      }
      check_equal(table_version(),version);
      check_equal(open_query("SELECT score FROM items",NULL,0,&query),TURBODB_STATUS_OK);
      orm_sql_scan_row row={0};
      check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_DATASTORE_ERROR);
    }
    it("rejects invalid output shapes unknown names capture order and unsupported query clauses") {
      seed(); const char *const sql[]={
        "UPDATE items SET score=(SELECT id,score FROM items)",
        "DELETE FROM items WHERE id IN(SELECT id,score FROM items)",
        "UPDATE items o SET score=(SELECT items.score)",
        "UPDATE items o SET score=(SELECT missing)",
        "WITH c(n) AS(SELECT missing FROM items) DELETE FROM items WHERE id=1",
        "UPDATE items SET score=(SELECT d.n FROM LATERAL (SELECT a.n AS n) d,(SELECT 2 AS n) a)",
        "INSERT INTO items SELECT id FROM (SELECT id FROM items) d",
        "DELETE FROM items WHERE id IN(SELECT id FROM items LIMIT 1)"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i)
        check_equal(bind_write(sql[i],NULL,0),i+1==sizeof(sql)/sizeof(sql[0])?
            TURBODB_STATUS_UNSUPPORTED:TURBODB_STATUS_SQL_ERROR);
      sqlparser_document *document=parse("WITH c AS(SELECT 1 AS n) UPDATE items SET score=1");
      const sqlparser_node *with=sqlparser_get_node(document,sqlparser_statements(document).first);
      const orm_sql_query_scope scope={.document=document,.root=with->as.with.body,.max_depth=DEPTH,.budget=&budget};
      check_equal(orm_sql_change_bind(&scope,&owner,&error),TURBODB_STATUS_UNSUPPORTED);
      sqlparser_document_destroy(document);
    }
    it("binds bounded recursive INSERT SELECT but rejects recursive mutation dependencies") {
      seed(); const char *const sql[]={
        "WITH RECURSIVE c(n) AS(SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3) UPDATE items SET score=1 WHERE id IN(SELECT n FROM c)",
        "WITH RECURSIVE c(n) AS(SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3) DELETE FROM items WHERE id IN(SELECT n FROM c)",
        "INSERT INTO items WITH RECURSIVE c(n) AS(SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3) SELECT n,n FROM c"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        check_equal(bind_write(sql[i],NULL,0),TURBODB_STATUS_UNSUPPORTED);
        check_equal(bind_write_iterations(sql[i],NULL,0,DEPTH),i==2?
            TURBODB_STATUS_OK:TURBODB_STATUS_UNSUPPORTED);
      }
    }
    it("refunds every partial reserve and resize failure in write dependency binding") {
      seed(); const orm_sql_type type={TURBODB_VALUE_INT64,true};
      const char *const sql[]={
        "WITH c AS(SELECT id,score+? AS score FROM items) UPDATE items o SET score=(SELECT score FROM c WHERE id=o.id)",
        "DELETE FROM items o WHERE EXISTS(SELECT 1 FROM LATERAL (SELECT o.id+? AS n) d)",
        "INSERT INTO items WITH c AS(SELECT id,score+? AS score FROM items) SELECT id,score FROM c ON DUPLICATE KEY UPDATE score=VALUES(score)"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        next_statement(); reserves=resizes=0;
        check_equal(bind_write(sql[i],&type,1),TURBODB_STATUS_OK);
        const size_t allocations[]={reserves,resizes};
        for(size_t pass=0;pass<2;++pass) {
          check_greater(allocations[pass],0u);
          for(size_t point=1;point<=allocations[pass];++point) {
            next_statement(); reserves=resizes=0;
            if(pass==0) fail_reserve=point; else fail_resize=point;
            check_equal(bind_write(sql[i],&type,1),TURBODB_STATUS_OUT_OF_MEMORY);
            fail_reserve=fail_resize=0;
          }
        }
      }
    }
    it("fails at work step and plan limits with no retained write dependency resources") {
      seed(); const orm_sql_budget_limits limits=budget.limits;
      const orm_sql_budget_resource resources[]={ORM_SQL_BUDGET_PLAN_NODES,ORM_SQL_BUDGET_WORK_BYTES,
        ORM_SQL_BUDGET_EXECUTION_STEPS};
      const char *const sql[]={"UPDATE items o SET score=(SELECT o.score)",
        "INSERT INTO items SELECT id,score FROM (SELECT id,score FROM items) d"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i)
        for(size_t j=0;j<sizeof(resources)/sizeof(resources[0]);++j) {
          next_statement(); budget.limits.statement.value[resources[j]]=budget.used.value[resources[j]];
          check_equal(bind_write(sql[i],NULL,0),TURBODB_STATUS_LIMIT_EXCEEDED); budget.limits=limits;
        }
    }
    it("executes the same CTE and correlated writes normally after preparation has closed") {
      seed(); const orm_sql_type type={TURBODB_VALUE_INT64,false}; const turbodb_value_t value=turbodb_i64(5);
      const char *const update="WITH c AS(SELECT id,score+? AS score FROM items) UPDATE items o "
          "SET score=(SELECT score FROM c WHERE id=o.id) WHERE id=1";
      check_equal(bind_write(update,&type,1),TURBODB_STATUS_OK);
      size_t affected=99; check_equal(execute(update,&value,1,&affected),TURBODB_STATUS_OK); check_equal(affected,1u);
      const char *const insert="INSERT INTO items SELECT id+10,d.n FROM items a,LATERAL (SELECT a.score+? AS n) d";
      check_equal(bind_write(insert,&type,1),TURBODB_STATUS_OK);
      check_equal(execute(insert,&value,1,&affected),TURBODB_STATUS_OK); check_equal(affected,3u);
      const char *const deletion="DELETE FROM items o WHERE EXISTS(SELECT 1 WHERE o.id=?)";
      check_equal(bind_write(deletion,&type,1),TURBODB_STATUS_OK);
      const turbodb_value_t key=turbodb_i64(2);
      check_equal(execute(deletion,&key,1,&affected),TURBODB_STATUS_OK); check_equal(affected,1u);
      check_equal(open_query("SELECT id,score FROM items WHERE id IN(1,11) ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(next().values[1].data.int64_value,15); check_equal(next().values[1].data.int64_value,20);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
  }
  it("uses SQL and Catalog schema alone to infer SELECT and UPDATE parameters without business reads") {
    seed(); corrupt_first_row(); const uint64_t version=table_version();
    const orm_sql_budget_amount before=budget.used;
    orm_tidesdb_transaction_t *transaction=owner.transaction;
    check_equal(orm_sql_diagnostics_init(&evaluation_diagnostics,2,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_diagnostics_add(&evaluation_diagnostics,ORM_SQL_DIAGNOSTIC_CAST_TRUNCATED,
        "existing warning",&error),TURBODB_STATUS_OK);
    orm_sql_table_definition definition={0}; orm_sql_table_schema schema={0};
    uint64_t id=0,read_version=0; bool found=false;
    check_equal(orm_tidesdb_sql_catalog_lookup(&owner,vstr_from_cstr("items"),&definition,
        &id,&read_version,&found,&error),TURBODB_STATUS_OK); check_true(found);
    check_equal(orm_tidesdb_sql_catalog_schema(&definition,&schema,&error),TURBODB_STATUS_OK);
    sqlparser_document *document=parse("SELECT ? AS label,?+score AS value,CAST(? AS DOUBLE) AS real FROM items");
    orm_sql_parameters parameters={0};
    check_equal(orm_sql_parameters_open(document,&budget,&parameters,&error),TURBODB_STATUS_OK);
    orm_sql_binding_scope local={.document=document,.schema=&schema,.qualifier={"items",5},.budget=&budget};
    const sqlparser_id root=sqlparser_statements(document).first;
    const sqlparser_node *statement=sqlparser_get_node(document,root);
    sqlparser_id projection=statement->as.select.columns.first;
    for(size_t i=0;i<statement->as.select.columns.count;++i) {
      const sqlparser_node *column=sqlparser_get_node(document,projection);
      check_equal(orm_sql_parameters_infer(&parameters,&local,column->as.projection.expression,
          NULL,DEPTH,&error),TURBODB_STATUS_OK); projection=column->next;
    }
    const orm_sql_type *types=NULL; size_t count=0;
    check_equal(orm_sql_parameters_types(&parameters,&types,&count,&error),TURBODB_STATUS_OK);
    check_equal(count,3u); check_equal(types[0].kind,TURBODB_VALUE_TEXT);
    check_equal(types[1].kind,TURBODB_VALUE_INT64); check_equal(types[2].kind,TURBODB_VALUE_DOUBLE);
    const orm_sql_query_scope query_scope={.document=document,.root=root,.parameter_types=types,
      .parameter_count=count,.max_depth=DEPTH,.budget=&budget,.evaluation={.diagnostics=&evaluation_diagnostics}};
    check_equal(orm_sql_runtime_query_bind(&query_scope,&owner,&query,&error),TURBODB_STATUS_OK);
    sqlparser_document_destroy(document);
    check_equal(orm_sql_parameters_close(&parameters,&error),TURBODB_STATUS_OK);
    const turbodb_value_kind_t kinds[]={TURBODB_VALUE_TEXT,TURBODB_VALUE_INT64,TURBODB_VALUE_DOUBLE};
    check_equal(query.columns,3u);
    for(size_t i=0;i<query.columns;++i) {
      orm_sql_schema_column column={0};
      check_equal(orm_tidesdb_sql_runtime_column(&query,i,&column,&error),TURBODB_STATUS_OK);
      check_equal(column.type.kind,kinds[i]); check_true(column.type.nullable);
    }
    close_query(&query);
    document=parse("UPDATE items SET score=?+? WHERE id=? LIMIT ?"); local.document=document;
    check_equal(orm_sql_parameters_open(document,&budget,&parameters,&error),TURBODB_STATUS_OK);
    const sqlparser_id write_root=sqlparser_statements(document).first;
    statement=sqlparser_get_node(document,write_root);
    const sqlparser_node *assignment=sqlparser_get_node(document,statement->as.update.assignments.first);
    check_equal(orm_sql_parameters_infer(&parameters,&local,assignment->as.assignment.value,
        &schema.columns[1].type,DEPTH,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_parameters_infer(&parameters,&local,statement->as.update.where,
        NULL,DEPTH,&error),TURBODB_STATUS_OK);
    const orm_sql_type pagination={TURBODB_VALUE_UINT64,false};
    const sqlparser_node *limit=sqlparser_get_node(document,statement->as.update.limit);
    check_equal(orm_sql_parameters_infer(&parameters,&local,limit->as.limit.count,
        &pagination,DEPTH,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_parameters_types(&parameters,&types,&count,&error),TURBODB_STATUS_OK);
    check_equal(count,4u);
    for(size_t i=0;i<count;++i) check_equal(types[i].kind,i+1==count?TURBODB_VALUE_UINT64:TURBODB_VALUE_INT64);
    const orm_sql_query_scope write_scope={.document=document,.root=write_root,.parameter_types=types,
      .parameter_count=count,.max_depth=DEPTH,.budget=&budget,.evaluation={.diagnostics=&evaluation_diagnostics}};
    check_equal(orm_sql_change_bind(&write_scope,&owner,&error),TURBODB_STATUS_OK);
    sqlparser_document_destroy(document);
    check_equal(orm_sql_parameters_close(&parameters,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_destroy(&definition,&error),TURBODB_STATUS_OK);
    check_true(owner.transaction==transaction); check_false(owner.failed); check_equal(owner.active_sources,0u);
    check_equal(table_version(),version); check_equal(evaluation_diagnostics.total,1u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],before.value[ORM_SQL_BUDGET_WORK_BYTES]);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],before.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS],before.value[ORM_SQL_BUDGET_WRITE_ROWS]);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_BYTES],before.value[ORM_SQL_BUDGET_WRITE_BYTES]);
    check_equal(open_query("SELECT score FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    orm_sql_scan_row row={0};
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_DATASTORE_ERROR);
  }
  group("whole statement unknown parameter inference") {
    it("infers SELECT projections comparisons ordering and both pagination markers in source order") {
      seed();
      const turbodb_value_kind_t unit[]={TURBODB_VALUE_TEXT,TURBODB_VALUE_DOUBLE,TURBODB_VALUE_DOUBLE,
        TURBODB_VALUE_INT64,TURBODB_VALUE_UINT64,TURBODB_VALUE_UINT64};
      statement_metadata("SELECT ? AS label,?+? AS ratio,CAST(? AS SIGNED) AS n LIMIT ? OFFSET ?",
          unit,sizeof(unit)/sizeof(unit[0]),TURBODB_STATUS_OK);
      const turbodb_value_kind_t table[]={TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,
        TURBODB_VALUE_UINT64,TURBODB_VALUE_UINT64};
      statement_metadata("SELECT i.*,?+i.score AS n FROM items i WHERE i.id=? ORDER BY i.score+? DESC LIMIT ?,?",
          table,sizeof(table)/sizeof(table[0]),TURBODB_STATUS_OK);
      statement_metadata("SELECT id FROM items WHERE id=? AND score=?",
          table,2,TURBODB_STATUS_OK);
      const turbodb_value_kind_t compound[]={TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,
        TURBODB_VALUE_INT64,TURBODB_VALUE_INT64};
      statement_metadata("SELECT id FROM items WHERE id BETWEEN ? AND ? AND score IN (?,?)",
          compound,sizeof(compound)/sizeof(compound[0]),TURBODB_STATUS_OK);
      const turbodb_value_kind_t conditional[]={TURBODB_VALUE_INT64,TURBODB_VALUE_INT64};
      statement_metadata("SELECT CASE WHEN id=? THEN ? ELSE score END AS value FROM items",
          conditional,sizeof(conditional)/sizeof(conditional[0]),TURBODB_STATUS_OK);
      const turbodb_value_kind_t functions[]={TURBODB_VALUE_DOUBLE,TURBODB_VALUE_INT64,
        TURBODB_VALUE_DOUBLE,TURBODB_VALUE_INT64,TURBODB_VALUE_INT64};
      statement_metadata("SELECT ABS(?) AS absolute,COALESCE(?,score) AS selected,"
          "ROUND(?,?) AS rounded FROM items WHERE id=?",functions,
          sizeof(functions)/sizeof(functions[0]),TURBODB_STATUS_OK);
    }
    it("maps reordered multirow INSERT targets SET defaults REPLACE and duplicate assignments to real columns") {
      seed(); const turbodb_value_kind_t types[]={TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,
        TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,TURBODB_VALUE_INT64};
      statement_metadata("INSERT INTO items(score,id) VALUES(?+?,?),(CAST(? AS SIGNED),?) "
          "ON DUPLICATE KEY UPDATE score=score+?",types,sizeof(types)/sizeof(types[0]),TURBODB_STATUS_OK);
      statement_metadata("INSERT IGNORE INTO items SET score=?,id=?",types,2,TURBODB_STATUS_OK);
      statement_metadata("REPLACE INTO items VALUES(?,?)",types,2,TURBODB_STATUS_OK);
      statement_metadata("INSERT INTO items(id) VALUES(?)",types,1,TURBODB_STATUS_OK);
      statement_metadata("INSERT INTO items VALUES(?,DEFAULT)",types,1,TURBODB_STATUS_OK);
      size_t affected=0;
      check_equal(execute("CREATE TABLE mixed(id BIGINT PRIMARY KEY,u BIGINT UNSIGNED,d DOUBLE)",
          NULL,0,&affected),TURBODB_STATUS_OK);
      const turbodb_value_kind_t mixed[]={TURBODB_VALUE_DOUBLE,TURBODB_VALUE_UINT64,TURBODB_VALUE_INT64,
        TURBODB_VALUE_DOUBLE,TURBODB_VALUE_UINT64,TURBODB_VALUE_INT64};
      statement_metadata("INSERT INTO mixed(d,u,id) VALUES(?,?,?),(?,?,?)",mixed,6,TURBODB_STATUS_OK);
      const turbodb_value_kind_t assignments[]={TURBODB_VALUE_DOUBLE,TURBODB_VALUE_DOUBLE,
        TURBODB_VALUE_UINT64,TURBODB_VALUE_INT64};
      statement_metadata("UPDATE mixed SET d=?+?,u=? WHERE id=?",assignments,4,TURBODB_STATUS_OK);
    }
    it("derives aliased UPDATE and DELETE assignment predicate order and LIMIT contexts without applying changes") {
      seed(); const uint64_t version=table_version();
      const turbodb_value_kind_t update[]={TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,
        TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,TURBODB_VALUE_UINT64};
      statement_metadata("UPDATE items i SET score=?+?,id=CAST(? AS SIGNED) WHERE i.id=? ORDER BY score+? LIMIT ?",
          update,sizeof(update)/sizeof(update[0]),TURBODB_STATUS_OK);
      const turbodb_value_kind_t deletion[]={TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,TURBODB_VALUE_UINT64};
      statement_metadata("DELETE FROM items i WHERE i.id=? ORDER BY i.score+? LIMIT ?",
          deletion,sizeof(deletion)/sizeof(deletion[0]),TURBODB_STATUS_OK);
      check_equal(table_version(),version);
    }
    it("leaves marker free functions aggregates and predicates to the ordinary full statement Binder") {
      seed();
      statement_metadata("SELECT COUNT(*) AS n FROM items",NULL,0,TURBODB_STATUS_OK);
      statement_metadata("SELECT id FROM items WHERE id IN(1,2) ORDER BY id",NULL,0,TURBODB_STATUS_OK);
      const turbodb_value_kind_t type=TURBODB_VALUE_TEXT;
      statement_metadata("SELECT ? AS label,COUNT(*) AS n FROM items",&type,1,TURBODB_STATUS_OK);
      statement_metadata("UPDATE items SET score=DEFAULT WHERE id=1",NULL,0,TURBODB_STATUS_OK);
      statement_metadata("DELETE FROM items WHERE id=1 LIMIT 0",NULL,0,TURBODB_STATUS_OK);
    }
    it("infers ordinary JOIN grouping aggregate and window control markers before full binding") {
      seed();
      const turbodb_value_kind_t grouped[]={TURBODB_VALUE_DOUBLE,TURBODB_VALUE_INT64,
        TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,TURBODB_VALUE_UINT64};
      statement_metadata("SELECT a.id,SUM(?) AS total FROM items a JOIN items b ON a.id=b.id+? "
          "WHERE a.score>? GROUP BY a.id HAVING a.id>? ORDER BY a.id+? LIMIT ?",
          grouped,sizeof(grouped)/sizeof(grouped[0]),TURBODB_STATUS_OK);
      const turbodb_value_kind_t windowed[]={TURBODB_VALUE_UINT64,TURBODB_VALUE_INT64,
        TURBODB_VALUE_INT64,TURBODB_VALUE_UINT64,TURBODB_VALUE_UINT64,
        TURBODB_VALUE_TEXT,TURBODB_VALUE_UINT64};
      statement_metadata("SELECT NTILE(?) OVER(PARTITION BY id+? ORDER BY score+? "
          "ROWS BETWEEN ? PRECEDING AND ? FOLLOWING) AS bucket,"
          "LAG(?) OVER(ORDER BY id) AS carried FROM items LIMIT ?",
          windowed,sizeof(windowed)/sizeof(windowed[0]),TURBODB_STATUS_OK);
      const turbodb_value_kind_t named[]={TURBODB_VALUE_TEXT,TURBODB_VALUE_INT64,
        TURBODB_VALUE_INT64,TURBODB_VALUE_UINT64};
      statement_metadata("SELECT COUNT(?) OVER w AS count FROM items WINDOW w AS "
          "(PARTITION BY id+? ORDER BY score+? ROWS ? PRECEDING)",
          named,sizeof(named)/sizeof(named[0]),TURBODB_STATUS_OK);
    }
    it("infers CTE and derived query-block markers before binding their owners") {
      seed();
      const turbodb_value_kind_t one[]={TURBODB_VALUE_INT64};
      statement_metadata("SELECT d.n FROM (SELECT score+? AS n FROM items) d",
          one,sizeof(one)/sizeof(one[0]),TURBODB_STATUS_OK);
      statement_metadata("WITH q AS(SELECT score+? AS n FROM items) SELECT n FROM q",
          one,sizeof(one)/sizeof(one[0]),TURBODB_STATUS_OK);
      const turbodb_value_kind_t text[]={TURBODB_VALUE_TEXT};
      statement_metadata("WITH q AS(SELECT 1 AS n) SELECT ? AS label FROM q",
          text,sizeof(text)/sizeof(text[0]),TURBODB_STATUS_OK);
    }
    it("infers markers against correlated outer query schemas") {
      seed(); const turbodb_value_kind_t one[]={TURBODB_VALUE_INT64};
      statement_metadata("SELECT (SELECT a.score+?) AS n FROM items a",
          one,sizeof(one)/sizeof(one[0]),TURBODB_STATUS_OK);
    }
    it("keeps unrelated root markers unresolved while binding an earlier scalar child") {
      seed();
      const turbodb_value_kind_t nested[]={TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,
        TURBODB_VALUE_UINT64};
      statement_metadata("SELECT (SELECT score+? FROM items) AS n FROM items WHERE id>? LIMIT ?",
          nested,sizeof(nested)/sizeof(nested[0]),TURBODB_STATUS_OK);
    }
    it("infers markers from scalar IN and compound query dependencies") {
      seed();
      statement_metadata("SELECT (SELECT 1) AS n",NULL,0,TURBODB_STATUS_OK);
      statement_metadata("SELECT id FROM items WHERE id IN(SELECT id FROM items)",NULL,0,TURBODB_STATUS_OK);
      statement_metadata("SELECT id FROM items UNION SELECT id FROM items",NULL,0,TURBODB_STATUS_OK);
      const turbodb_value_kind_t scalar[]={TURBODB_VALUE_INT64,TURBODB_VALUE_UINT64};
      statement_metadata("SELECT ?+(SELECT id FROM items LIMIT 1) AS n LIMIT ?",scalar,2,TURBODB_STATUS_OK);
      const turbodb_value_kind_t membership[]={TURBODB_VALUE_INT64};
      statement_metadata("SELECT ? IN(SELECT id FROM items) AS matched",membership,1,TURBODB_STATUS_OK);
      const turbodb_value_kind_t compound[]={TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,
        TURBODB_VALUE_UINT64};
      statement_metadata("SELECT ?+1 AS n UNION ALL SELECT score+? AS n FROM items LIMIT ?",
          compound,sizeof(compound)/sizeof(compound[0]),TURBODB_STATUS_OK);
    }
    it("binds recursive CTE seed and member metadata without parameter values or iteration") {
      seed();
      const turbodb_value_kind_t recursive[]={TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,
        TURBODB_VALUE_UINT64,TURBODB_VALUE_INT64};
      statement_metadata("WITH RECURSIVE c(n) AS(SELECT 1 UNION ALL "
          "SELECT n+? FROM c WHERE n<? LIMIT ?) SELECT n+? AS n FROM c",
          recursive,sizeof(recursive)/sizeof(recursive[0]),TURBODB_STATUS_OK);
      sqlparser_document *document=parse("WITH RECURSIVE c(n) AS(SELECT 1 UNION ALL "
          "SELECT n+? FROM c WHERE n<?) SELECT n FROM c");
      orm_sql_parameters parameters={0};
      check_equal(orm_sql_parameters_statement(document,&owner,DEPTH,0,&parameters,&error),
          TURBODB_STATUS_UNSUPPORTED);
      check_null(parameters.budget); sqlparser_document_destroy(document);
    }
    it("rejects statement level name shape predicate and modifier errors after partial inference and refunds all work") {
      seed();
      const struct { const char *sql; turbodb_status_t status; } cases[]={
        {"SELECT ? AS n FROM absent",TURBODB_STATUS_SQL_ERROR},
        {"SELECT ? AS n,missing FROM items",TURBODB_STATUS_SQL_ERROR},
        {"INSERT INTO items(id,id) VALUES(?,?)",TURBODB_STATUS_SQL_ERROR},
        {"INSERT INTO items VALUES(?)",TURBODB_STATUS_SQL_ERROR},
        {"UPDATE items SET absent=?",TURBODB_STATUS_SQL_ERROR},
        {"UPDATE items SET score=? WHERE 1",TURBODB_STATUS_UNSUPPORTED},
        {"SELECT ? AS n FROM items WHERE score",TURBODB_STATUS_UNSUPPORTED},
        {"REPLACE INTO items VALUES(?,?) ON DUPLICATE KEY UPDATE score=?",TURBODB_STATUS_UNSUPPORTED},
        {"UPDATE items SET score=? LIMIT 1.5",TURBODB_STATUS_UNSUPPORTED}};
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i)
        statement_metadata(cases[i].sql,NULL,0,cases[i].status);
    }
    it("explicitly rejects unimplemented inference frames expressions dialects and batches") {
      seed(); const char *const sql[]={
        "SELECT UNKNOWN_FUNCTION(?) AS n",
        "INSERT INTO items SELECT ?,?", "INSERT INTO items VALUES(?,?) AS incoming",
        "CREATE TABLE other(id BIGINT PRIMARY KEY)",
        "SELECT ?; SELECT ?"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i)
        statement_metadata(sql[i],NULL,0,TURBODB_STATUS_UNSUPPORTED);
      sqlparser_document *document=NULL; sqlparser_error parse_error; const char sqlite[]="SELECT ?";
      check_equal(sqlparser_parse_dialect(sqlite,strlen(sqlite),SQLPARSER_SQLITE,NULL,&document,&parse_error),SQLPARSER_OK);
      orm_sql_parameters parameters={0};
      check_equal(orm_sql_parameters_statement(document,&owner,DEPTH,0,&parameters,&error),TURBODB_STATUS_UNSUPPORTED);
      check_null(parameters.budget); sqlparser_document_destroy(document);
    }
    it("preserves occupied ready metadata and fails fast on invalid input inactive owner and exhausted quotas") {
      seed(); sqlparser_document *document=parse("UPDATE items SET score=?+? WHERE id=?");
      orm_sql_parameters parameters={0};
      check_equal(orm_sql_parameters_statement(NULL,&owner,DEPTH,DEPTH,&parameters,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      check_equal(orm_sql_parameters_statement(document,NULL,DEPTH,DEPTH,&parameters,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      check_equal(orm_sql_parameters_statement(document,&owner,0,DEPTH,&parameters,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      check_equal(orm_sql_parameters_statement(document,&owner,SIZE_MAX,DEPTH,&parameters,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      check_equal(orm_sql_parameters_statement(document,&owner,DEPTH,DEPTH,NULL,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      orm_sql_catalog_store closed={0}; const uint64_t before_work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(orm_sql_parameters_statement(document,&closed,DEPTH,DEPTH,&parameters,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      owner.failed=true;
      check_equal(orm_sql_parameters_statement(document,&owner,DEPTH,DEPTH,&parameters,&error),TURBODB_STATUS_INVALID_STATE);
      check_true(owner.failed); owner.failed=false;
      check_null(parameters.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],before_work);
      check_equal(orm_sql_parameters_statement(document,&owner,DEPTH,DEPTH,&parameters,&error),TURBODB_STATUS_OK);
      const orm_sql_type *view=NULL,*same=NULL; size_t count=0,same_count=0;
      check_equal(orm_sql_parameters_types(&parameters,&view,&count,&error),TURBODB_STATUS_OK);
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(orm_sql_parameters_statement(document,&owner,DEPTH,DEPTH,&parameters,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      check_equal(orm_sql_parameters_types(&parameters,&same,&same_count,&error),TURBODB_STATUS_OK);
      check_true(same==view); check_equal(same_count,count); check_equal(view[0].kind,TURBODB_VALUE_INT64);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      check_equal(orm_sql_parameters_close(&parameters,&error),TURBODB_STATUS_OK);
      check_equal(orm_sql_parameters_statement(document,&owner,1,DEPTH,&parameters,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_null(parameters.budget); sqlparser_document_destroy(document);
      const orm_sql_budget_limits limits=budget.limits;
      const orm_sql_budget_resource resources[]={ORM_SQL_BUDGET_WORK_BYTES,ORM_SQL_BUDGET_EXECUTION_STEPS,ORM_SQL_BUDGET_PLAN_NODES};
      for(size_t i=0;i<sizeof(resources)/sizeof(resources[0]);++i) {
        next_statement(); budget.limits.statement.value[resources[i]]=budget.used.value[resources[i]];
        statement_metadata("UPDATE items SET score=?+? WHERE id=?",NULL,0,TURBODB_STATUS_LIMIT_EXCEEDED);
        budget.limits=limits;
      }
      next_statement(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK);
      document=parse("SELECT ?");
      check_equal(orm_sql_parameters_statement(document,&owner,DEPTH,DEPTH,&parameters,&error),TURBODB_STATUS_INVALID_STATE);
      check_null(parameters.budget); sqlparser_document_destroy(document);
      check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
    }
    it("refunds every injected WORK Vec allocation failure across statement inference and final validation") {
      seed(); const char *const sql[]={"INSERT INTO items(score,id) VALUES(?+?,?) ON DUPLICATE KEY UPDATE score=score+?",
        "SELECT ?+score AS n FROM items WHERE id=? LIMIT ?",
        "WITH q AS(SELECT score+? AS n FROM items) SELECT n FROM q WHERE n>?"};
      const turbodb_value_kind_t insert[]={TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,TURBODB_VALUE_INT64};
      const turbodb_value_kind_t select[]={TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,TURBODB_VALUE_UINT64};
      const turbodb_value_kind_t dependency[]={TURBODB_VALUE_INT64,TURBODB_VALUE_INT64};
      const turbodb_value_kind_t *const kinds[]={insert,select,dependency};
      const size_t counts[]={sizeof(insert)/sizeof(insert[0]),sizeof(select)/sizeof(select[0]),
        sizeof(dependency)/sizeof(dependency[0])};
      for(size_t form=0;form<sizeof(sql)/sizeof(sql[0]);++form) {
        next_statement(); reserves=resizes=0;
        statement_metadata(sql[form],kinds[form],counts[form],TURBODB_STATUS_OK);
        const size_t reserve_points=reserves,resize_points=resizes;
        check_true(reserve_points>0); check_true(resize_points>0);
        for(size_t mode=0;mode<2;++mode) {
          const size_t points=mode?resize_points:reserve_points;
          for(size_t point=1;point<=points;++point) {
            next_statement(); reserves=resizes=0;
            fail_reserve=mode?0:point; fail_resize=mode?point:0;
            statement_metadata(sql[form],NULL,0,TURBODB_STATUS_OUT_OF_MEMORY);
            fail_reserve=fail_resize=0;
          }
        }
      }
    }
    it("infers and validates complete statements against corrupt business data without reading it or emitting warnings") {
      seed(); corrupt_first_row(); const uint64_t version=table_version();
      check_equal(orm_sql_diagnostics_init(&evaluation_diagnostics,2,&error),TURBODB_STATUS_OK);
      check_equal(orm_sql_diagnostics_add(&evaluation_diagnostics,ORM_SQL_DIAGNOSTIC_CAST_TRUNCATED,
          "existing warning",&error),TURBODB_STATUS_OK);
      const turbodb_value_kind_t types[]={TURBODB_VALUE_INT64,TURBODB_VALUE_INT64};
      statement_metadata("SELECT score+? AS n FROM items WHERE id=?",types,2,TURBODB_STATUS_OK);
      statement_metadata("INSERT INTO items VALUES(?,?)",types,2,TURBODB_STATUS_OK);
      statement_metadata("UPDATE items SET score=? WHERE id=?",types,2,TURBODB_STATUS_OK);
      statement_metadata("DELETE FROM items WHERE id=?",types,1,TURBODB_STATUS_OK);
      const turbodb_value_kind_t text=TURBODB_VALUE_TEXT;
      statement_metadata("SELECT ? AS n,1e0/0e0 AS dangerous FROM items",&text,1,TURBODB_STATUS_OK);
      check_equal(table_version(),version); check_equal(evaluation_diagnostics.total,1u);
      check_equal(open_query("SELECT score FROM items",NULL,0,&query),TURBODB_STATUS_OK);
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_DATASTORE_ERROR);
    }
  }
  group("session snapshot ownership") {
    it("rejects actual variable evaluation without context and refunds construction work") {
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      const char *const sql[]={"SELECT @@autocommit AS n", "SHOW VARIABLES", "SHOW SESSION VARIABLES LIKE 'AUTO%'"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_UNSUPPORTED);
        check_null(query.owner); check_equal(owner.active_sources,0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_false(owner.failed);
      }
    }
    it("owns SHOW pattern bytes after AST release and copies the session with complete TEXT metadata") {
      orm_sql_session_snapshot session={.valid=true,.autocommit=false,.read_only=true};
      check_equal(open_session_query("SHOW SESSION VARIABLES LIKE 'TRANSACTION%'",session),TURBODB_STATUS_OK);
      session=(orm_sql_session_snapshot){0};
      check_equal(query.columns,2u); orm_sql_schema_column column={0};
      check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK);
      text_is(column.name,"Variable_name"); check_equal(column.type.kind,TURBODB_VALUE_TEXT); check_false(column.type.nullable);
      check_equal(orm_tidesdb_sql_runtime_column(&query,1,&column,&error),TURBODB_STATUS_OK); text_is(column.name,"Value");
      orm_sql_scan_row row=next(); text_is(row.values[0].data.text_value,"transaction_isolation"); text_is(row.values[1].data.text_value,"SERIALIZABLE");
      row=next(); text_is(row.values[0].data.text_value,"transaction_read_only"); text_is(row.values[1].data.text_value,"ON");
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query); check_equal(owner.active_sources,0u);
    }
    it("refunds SHOW pattern allocation failures and preserves the Catalog owner") {
      const char sql[]="SHOW LOCAL VARIABLES LIKE 'AUTO%'";
      const orm_sql_session_snapshot session={.valid=true,.autocommit=true};
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      reserves=resizes=0; check_equal(open_session_query(sql,session),TURBODB_STATUS_OK);
      const size_t allocations[]={reserves,resizes}; close_query(&query);
      for(size_t pass=0;pass<sizeof(allocations)/sizeof(allocations[0]);++pass) {
        for(size_t point=1;point<=allocations[pass];++point) {
          next_statement(); reserves=resizes=0;
          if(pass) fail_resize=point; else fail_reserve=point;
          check_equal(open_session_query(sql,session),TURBODB_STATUS_OUT_OF_MEMORY);
          fail_reserve=fail_resize=0; check_null(query.owner); check_false(owner.failed);
          check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
        }
      }
      next_statement(); check_equal(open_session_query(sql,session),TURBODB_STATUS_OK);
      text_is(next().values[1].data.text_value,"ON"); close_query(&query);
    }
    it("stops SHOW iteration at its execution budget and closes its borrowed views") {
      const orm_sql_session_snapshot session={.valid=true,.autocommit=true};
      check_equal(open_session_query("SHOW VARIABLES LIKE '%'",session),TURBODB_STATUS_OK);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      orm_sql_scan_row row={0};
      check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      close_query(&query); check_equal(owner.active_sources,0u); check_false(owner.failed);
    }
  }
  group("SHOW admission") {
    it("streams table and column LIKE matches after destroying the AST") {
      seed();
      check_equal(open_query("SHOW TABLES LIKE 'it%'",NULL,0,&query),TURBODB_STATUS_OK);
      text_is(next().values[0].data.text_value,"items"); check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      check_equal(open_query("SHOW TABLES LIKE 'IT%'",NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      check_equal(open_query("SHOW COLUMNS FROM items LIKE 'SCO%'",NULL,0,&query),TURBODB_STATUS_OK);
      orm_sql_scan_row row=next(); text_is(row.values[0].data.text_value,"score");
      check_equal(row.values[4].kind,TURBODB_VALUE_NULL); check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    }
    it("binds displayed column labels and owns WHERE parameter payloads through close") {
      seed(); char name[]="score"; turbodb_value_t parameter=turbodb_text(name);
      check_equal(open_query("SHOW COLUMNS FROM items WHERE fIeLd=? AND `Null`='YES'",
          &parameter,1,&query),TURBODB_STATUS_OK);
      name[0]='X'; parameter=turbodb_null();
      text_is(next().values[0].data.text_value,"score"); check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      check_equal(open_query("SHOW FULL TABLES WHERE tables_IN_app='items' AND Table_type='BASE TABLE'",
          NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(query.columns,2u); text_is(next().values[0].data.text_value,"items");
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      const turbodb_value_t unique=turbodb_i64(0);
      check_equal(open_query("SHOW INDEX FROM items WHERE non_UNIQUE=? AND Cardinality IS NULL",
          &unique,1,&query),TURBODB_STATUS_OK);
      orm_sql_scan_row row=next(); text_is(row.values[2].data.text_value,"PRIMARY");
      check_equal(row.values[1].kind,TURBODB_VALUE_INT64); check_equal(row.values[1].data.int64_value,0);
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    }
    it("shares WHERE evaluation with the copied session snapshot") {
      const orm_sql_session_snapshot session={.valid=true,.autocommit=false,
          .read_only=true,.max_allowed_packet=4096};
      check_equal(open_session_query("SHOW VARIABLES WHERE Value='ON' AND @@autocommit=0",session),TURBODB_STATUS_OK);
      text_is(next().values[0].data.text_value,"transaction_read_only");
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query); check_equal(owner.active_sources,0u);
      check_equal(open_session_query("SHOW VARIABLES WHERE Variable_name='max_allowed_packet' AND Value='4096'",session),TURBODB_STATUS_OK);
      text_is(next().values[0].data.text_value,"max_allowed_packet");
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query); check_equal(owner.active_sources,0u);
    }
    it("validates WHERE on empty sources and releases rejected constructions") {
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      const char *const sql[]={"SHOW TABLES WHERE missing=1", "SHOW TABLES WHERE Tables_in_app=?",
          "SHOW TABLES WHERE FALSE AND @@unknown=1", "SHOW TABLES WHERE COUNT(*)>0",
          "SHOW TABLES WHERE EXISTS(SELECT 1)","SHOW TABLES LIKE 'é%'"};
      const turbodb_status_t expected[]={TURBODB_STATUS_SQL_ERROR,TURBODB_STATUS_SQL_ERROR,TURBODB_STATUS_UNSUPPORTED,
          TURBODB_STATUS_UNSUPPORTED,TURBODB_STATUS_UNSUPPORTED,TURBODB_STATUS_UNSUPPORTED};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        check_equal(open_query(sql[i],NULL,0,&query),expected[i]); check_null(query.owner);
        check_equal(owner.active_sources,0u); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      sqlparser_document *doc=parse("SHOW TABLES WHERE NOT NOT (Tables_in_app='items')");
      check_equal(orm_tidesdb_sql_runtime_open(doc,&owner,vstr_from_cstr("app"),NULL,0,1,&query,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      sqlparser_document_destroy(doc); check_null(query.owner);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_equal(owner.active_sources,0u);
    }
    it("refunds every WHERE allocation failure including retained parameters and filter runs") {
      const char sql[]="SHOW VARIABLES WHERE Variable_name=? AND @@autocommit=1";
      const orm_sql_session_snapshot session={.valid=true,.autocommit=true};
      const turbodb_value_t parameter=turbodb_text("autocommit");
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      reserves=resizes=0;
      check_equal(open_session_parameters_query(sql,session,&parameter,1),TURBODB_STATUS_OK);
      const size_t allocations[]={reserves,resizes}; close_query(&query);
      for(size_t pass=0;pass<sizeof(allocations)/sizeof(allocations[0]);++pass) {
        for(size_t point=1;point<=allocations[pass];++point) {
          next_statement(); reserves=resizes=0;
          if(pass) fail_resize=point; else fail_reserve=point;
          check_equal(open_session_parameters_query(sql,session,&parameter,1),TURBODB_STATUS_OUT_OF_MEMORY);
          fail_reserve=fail_resize=0; check_null(query.owner); check_false(owner.failed);
          check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
        }
      }
      next_statement(); check_equal(open_session_parameters_query(sql,session,&parameter,1),TURBODB_STATUS_OK);
      text_is(next().values[0].data.text_value,"autocommit"); close_query(&query);
    }
    it("cancels WHERE before reading and latches a later execution step failure") {
      seed(); const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
      check_equal(open_query("SHOW COLUMNS FROM items WHERE Field='score'",NULL,0,&query),TURBODB_STATUS_OK);
      const uint64_t admitted_reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS]; check_true(admitted_reads>=reads);
      check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_OK);
      check_equal(next().state,ORM_SQL_SCAN_CANCELLED);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],admitted_reads); close_query(&query);
      check_equal(open_query("SHOW TABLES WHERE FALSE",NULL,0,&query),TURBODB_STATUS_OK);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      orm_sql_scan_row row={0};
      check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      close_query(&query); check_equal(owner.active_sources,0u); check_false(owner.failed);
    }
    it("preserves invalid warning LIMIT errors and refunds every partial owner") {
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      const char *const sql[]={"SHOW WARNINGS LIMIT 1.5",
          "SHOW WARNINGS LIMIT 18446744073709551616",
          "SHOW WARNINGS LIMIT 18446744073709551616,1"};
      const turbodb_status_t expected[]={TURBODB_STATUS_UNSUPPORTED,
          TURBODB_STATUS_LIMIT_EXCEEDED,TURBODB_STATUS_LIMIT_EXCEEDED};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        check_equal(open_query(sql[i],NULL,0,&query),expected[i]);
        check_null(query.owner); check_equal(owner.active_sources,0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_false(owner.failed);
      }
      check_equal(open_query("SHOW WARNINGS LIMIT 1",NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      check_equal(owner.active_sources,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  group("division and modulo statement context") {
    it("rejects invalid statement contexts before query or metadata construction") {
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      const char *sql[]={"SELECT 1 AS n","EXPLAIN SELECT 1 AS n","SHOW TABLES"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        check_equal(open_evaluation_query(sql[i],ORM_SQL_EVALUATION_QUERY),TURBODB_STATUS_INVALID_ARGUMENT);
        check_null(query.owner); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      enum { RECORDS = 2 };
      check_equal(orm_sql_diagnostics_init(&evaluation_diagnostics,RECORDS,&error),TURBODB_STATUS_OK);
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        check_equal(open_evaluation_query(sql[i],(orm_sql_evaluation_mode)(ORM_SQL_EVALUATION_IGNORE_WRITE+1)),TURBODB_STATUS_INVALID_ARGUMENT);
        check_null(query.owner); check_equal(owner.active_sources,0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_equal(evaluation_diagnostics.total,0u);
      }
    }
    it("keeps query diagnostics across native reopen without retaining the AST") {
      enum { RECORDS = 2 };
      check_equal(orm_sql_diagnostics_init(&evaluation_diagnostics,RECORDS,&error),TURBODB_STATUS_OK);
      check_equal(open_evaluation_query("SELECT -7 DIV 3 AS q,MOD(-7,3) AS r,7.5/2.0 AS v,7 DIV 0 AS z",ORM_SQL_EVALUATION_QUERY),TURBODB_STATUS_OK);
      for(size_t round=0;round<2;++round) {
        const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
        check_equal(row.values[0].data.int64_value,-2); check_equal(row.values[1].data.int64_value,-1);
        check_equal(row.values[2].data.double_value,3.75); check_equal(row.values[3].kind,TURBODB_VALUE_NULL);
        check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(evaluation_diagnostics.total,round+1);
        if(!round) resume_query();
      }
      check_equal(orm_sql_diagnostics_at(&evaluation_diagnostics,0)->code,ORM_SQL_DIAGNOSTIC_DIVISION_BY_ZERO);
    }
    it("replays the recursive cache without reevaluating division diagnostics") {
      enum { RECORDS = 2, ITERATIONS = 5, ROWS = 3 };
      check_equal(orm_sql_diagnostics_init(&evaluation_diagnostics,RECORDS,&error),TURBODB_STATUS_OK);
      check_equal(open_evaluation_iterations_query("WITH RECURSIVE c(n,z) AS"
          "(SELECT 1,MOD(7,0) UNION ALL SELECT n+1,MOD(n,0) FROM c WHERE n<3) SELECT n,z FROM c",
          ORM_SQL_EVALUATION_QUERY,ITERATIONS),TURBODB_STATUS_OK);
      for(size_t round=0;round<2;++round) {
        for(size_t r=0;r<ROWS;++r) {
          const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
          check_equal(row.values[0].data.int64_value,(int64_t)r+1); check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
        }
        check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(evaluation_diagnostics.total,ROWS);
        if(!round) resume_query();
      }
      close_query(&query); check_equal(owner.active_sources,0u);
    }
    it("propagates the same receiver through CTE derived scalar lateral and JOIN queries") {
      enum { RECORDS = 2 };
      seed(); check_equal(orm_sql_diagnostics_init(&evaluation_diagnostics,RECORDS,&error),TURBODB_STATUS_OK);
      const char *sql[]={"WITH q AS(SELECT MOD(7,0) AS n) SELECT n FROM q",
        "SELECT n FROM (SELECT 7 DIV 0 AS n) d", "SELECT (SELECT MOD(7,0)) AS n",
        "SELECT d.n FROM items i,LATERAL(SELECT MOD(i.id,0) AS n) d",
        "SELECT a.id FROM items a JOIN items b ON a.id DIV 0=b.id",
        "SELECT COUNT(*) AS n FROM items GROUP BY MOD(id,0)"};
      const size_t rows[]={1,1,1,3,0,1},warnings[]={1,1,1,3,9,3};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        check_equal(orm_sql_diagnostics_reset(&evaluation_diagnostics,&error),TURBODB_STATUS_OK);
        check_equal(open_evaluation_query(sql[i],ORM_SQL_EVALUATION_QUERY),TURBODB_STATUS_OK);
        for(size_t r=0;r<rows[i];++r) { const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
          if(i!=5) check_equal(row.values[0].kind,TURBODB_VALUE_NULL); else check_equal(row.values[0].data.int64_value,3); }
        check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(evaluation_diagnostics.total,warnings[i]);
        close_query(&query);
      }
    }
    it("propagates strict mode to nested calls and preserves error output and source leases") {
      enum { RECORDS = 2 };
      seed(); check_equal(orm_sql_diagnostics_init(&evaluation_diagnostics,RECORDS,&error),TURBODB_STATUS_OK);
      const char *sql[]={"SELECT MOD(id,0) AS n FROM items",
        "WITH q AS(SELECT MOD(id,0) AS n FROM items) SELECT n FROM q",
        "SELECT (SELECT MOD(i.id,0)) AS n FROM items i"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
        check_equal(open_evaluation_query(sql[i],ORM_SQL_EVALUATION_WRITE),TURBODB_STATUS_OK);
        orm_sql_scan_row row={.count=99}; check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_SQL_ERROR);
        check_equal(row.count,99u); const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
        check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_SQL_ERROR);
        check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); check_equal(evaluation_diagnostics.total,0u);
        close_query(&query); check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
    }
    it("skips division diagnostics for NULL lazy outputs explanation limits and EXISTS projection") {
      enum { RECORDS = 2 };
      seed(); check_equal(orm_sql_diagnostics_init(&evaluation_diagnostics,RECORDS,&error),TURBODB_STATUS_OK);
      const char *sql[]={"SELECT MOD(NULL,0) AS n", "SELECT CASE WHEN TRUE THEN 7 ELSE 5 DIV 0 END AS n",
        "SELECT MOD(id,0) AS n FROM items LIMIT 0", "EXPLAIN SELECT MOD(id,0) AS n FROM items",
        "SELECT EXISTS(SELECT MOD(id,0) FROM items) AS n"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        check_equal(open_evaluation_query(sql[i],ORM_SQL_EVALUATION_QUERY),TURBODB_STATUS_OK);
        orm_sql_scan_row row; do { row=next(); } while(row.state==ORM_SQL_SCAN_ROW);
        check_equal(evaluation_diagnostics.total,0u); close_query(&query);
      }
    }
  }

  group("common numeric set results") {
    it("aggregates all numeric branch orders before binding and preserves metadata across resume") {
      const turbodb_value_t values[]={turbodb_i64(-1),turbodb_u64(UINT64_MAX),turbodb_f64(0.5)};
      const size_t permutations[][3]={{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
      const double expected[]={-1.0,18446744073709551616.0,0.5};
      const char *sql="SELECT ? AS first_name UNION ALL SELECT ? AS other_name UNION ALL SELECT ?";
      for(size_t order=0;order<sizeof(permutations)/sizeof(permutations[0]);++order) {
        turbodb_value_t params[3]; for(size_t i=0;i<3;++i) params[i]=values[permutations[order][i]];
        check_equal(open_query(sql,params,3,&query),TURBODB_STATUS_OK);
        orm_sql_schema_column column; check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK);
        text_is(column.name,"first_name"); check_equal(column.type.kind,TURBODB_VALUE_DOUBLE); check_false(column.type.nullable);
        for(size_t round=0;round<2;++round) {
          for(size_t i=0;i<3;++i) {
            const orm_sql_scan_row row=next(); check_equal(row.values[0].kind,TURBODB_VALUE_DOUBLE);
            check_equal(row.values[0].data.double_value,expected[permutations[order][i]]);
          }
          check_equal(next().state,ORM_SQL_SCAN_DONE); if(!round) resume_query();
        }
        close_query(&query);
      }
      const turbodb_value_t nullable_real=turbodb_f64(0.5);
      check_equal(open_query("SELECT NULL AS n UNION ALL SELECT 1 UNION ALL SELECT ?",&nullable_real,1,&query),TURBODB_STATUS_OK);
      orm_sql_schema_column column; check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK);
      check_equal(column.type.kind,TURBODB_VALUE_DOUBLE); check_true(column.type.nullable);
      check_equal(next().values[0].kind,TURBODB_VALUE_NULL); check_equal(next().values[0].data.double_value,1.0);
      check_equal(next().values[0].data.double_value,0.5); check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("uses late real branches before earlier distinct intersection and difference membership") {
      const turbodb_value_t real=turbodb_f64(9007199254740992.0);
      const char *sql[]={
        "SELECT 9007199254740992 AS n UNION SELECT 9007199254740993 UNION ALL SELECT ?",
        "SELECT 9007199254740992 AS n UNION ALL SELECT 9007199254740993 UNION SELECT ?",
        "SELECT 9007199254740992 AS n INTERSECT SELECT 9007199254740993 INTERSECT ALL SELECT ?",
        "SELECT 9007199254740992 AS n EXCEPT ALL SELECT 9007199254740993 EXCEPT SELECT ?",
        "SELECT 9007199254740992 AS n UNION SELECT 9007199254740993 UNION ALL SELECT ? ORDER BY n LIMIT 1 OFFSET 1"};
      const size_t counts[]={2,1,1,0,1};
      for(size_t mode=0;mode<sizeof(sql)/sizeof(sql[0]);++mode) {
        check_equal(open_query(sql[mode],&real,1,&query),TURBODB_STATUS_OK);
        orm_sql_schema_column column; check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK);
        check_equal(column.type.kind,TURBODB_VALUE_DOUBLE);
        for(size_t i=0;i<counts[mode];++i) {
          const orm_sql_scan_row row=next(); check_equal(row.values[0].kind,TURBODB_VALUE_DOUBLE);
          check_equal(row.values[0].data.double_value,real.data.double_value);
        }
        check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      }
      check_equal(open_query("SELECT 1 EXCEPT SELECT 1.0",NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("unifies kinds across both identity and bounded groups before their duplicate rules") {
      const turbodb_value_t real=turbodb_f64(9007199254740992.0);
      const char *sql[]={
        "(SELECT 9007199254740992 AS n UNION SELECT 9007199254740993) UNION ALL SELECT ?",
        "((SELECT 9007199254740992 AS n UNION SELECT 9007199254740993)) UNION ALL SELECT ? ORDER BY n",
        "(SELECT 9007199254740992 AS n UNION SELECT 9007199254740993 LIMIT 2) UNION ALL SELECT ?"};
      const size_t counts[]={2,2,2};
      for(size_t mode=0;mode<sizeof(sql)/sizeof(sql[0]);++mode) {
        check_equal(open_query(sql[mode],&real,1,&query),TURBODB_STATUS_OK);
        for(size_t i=0;i<counts[mode];++i) {
          const orm_sql_scan_row row=next(); check_equal(row.values[0].kind,TURBODB_VALUE_DOUBLE);
          check_equal(row.values[0].data.double_value,real.data.double_value);
        }
        check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      }
      const turbodb_value_t params[]={turbodb_i64(-1),turbodb_u64(UINT64_MAX),real};
      check_equal(open_query("(SELECT ? AS n UNION ALL SELECT ?) UNION ALL SELECT ?",params,3,&query),TURBODB_STATUS_OK);
      for(size_t i=0;i<3;++i) check_equal(next().values[0].kind,TURBODB_VALUE_DOUBLE);
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      check_equal(open_query("(SELECT ? AS n UNION ALL SELECT ? LIMIT 2) UNION ALL SELECT ?",params,3,&query),TURBODB_STATUS_OK);
      for(size_t i=0;i<3;++i) check_equal(next().values[0].kind,TURBODB_VALUE_DOUBLE);
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query); check_equal(owner.active_sources,0u);
    }
    it("unifies mixed operations and right nests while preserving independent derived and CTE domains") {
      const turbodb_value_t real=turbodb_f64(0.5);
      const char *sql[]={
        "SELECT 9007199254740992 AS n INTERSECT SELECT 9007199254740993 UNION ALL SELECT ?",
        "SELECT ? AS n UNION ALL (SELECT 9007199254740992 INTERSECT SELECT 9007199254740993)",
        "SELECT 9007199254740992 AS n EXCEPT (SELECT 9007199254740993 EXCEPT SELECT ?)",
        "SELECT 9007199254740992 AS n EXCEPT SELECT 9007199254740993 UNION ALL SELECT ?",
        "SELECT d.n FROM(SELECT 9007199254740992 AS n UNION SELECT 9007199254740993) d UNION ALL SELECT ?",
        "WITH c(n) AS(SELECT 9007199254740992 UNION SELECT 9007199254740993) SELECT n FROM c UNION ALL SELECT ?"};
      const size_t counts[]={2,2,0,1,3,3};
      for(size_t mode=0;mode<sizeof(sql)/sizeof(sql[0]);++mode) {
        check_equal(open_query(sql[mode],&real,1,&query),TURBODB_STATUS_OK);
        size_t count=0; orm_sql_scan_row row;
        while((row=next()).state==ORM_SQL_SCAN_ROW) { check_equal(row.values[0].kind,TURBODB_VALUE_DOUBLE); ++count; }
        check_equal(count,counts[mode]); close_query(&query);
      }
    }
    it("reports operation-specific nullable metadata independently of the common numeric kind") {
      const turbodb_value_t real=turbodb_f64(1.0);
      const char *sql[]={"SELECT NULL AS n UNION SELECT ?","SELECT NULL AS n INTERSECT SELECT ?",
        "SELECT 1 AS n EXCEPT SELECT NULL UNION ALL SELECT ?",
        "SELECT NULL AS n EXCEPT SELECT ?"};
      const bool nullable[]={true,false,false,true};
      const size_t counts[]={2,0,2,1};
      for(size_t mode=0;mode<sizeof(sql)/sizeof(sql[0]);++mode) {
        check_equal(open_query(sql[mode],&real,1,&query),TURBODB_STATUS_OK);
        orm_sql_schema_column column; check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK);
        check_equal(column.type.kind,TURBODB_VALUE_DOUBLE); check_equal(column.type.nullable,nullable[mode]);
        size_t count=0; while(next().state==ORM_SQL_SCAN_ROW) ++count;
        check_equal(count,counts[mode]); close_query(&query);
      }
    }
    it("shares converted set types with CTE correlated membership and existence demand") {
      seed(); const turbodb_value_t real=turbodb_f64(9007199254740992.0);
      const char *sql="WITH c(n) AS(SELECT 9007199254740992 UNION SELECT 9007199254740993 UNION ALL SELECT ?) "
        "SELECT o.id,(SELECT COUNT(*) FROM c WHERE c.n=9007199254740993+o.id-1) AS n "
        "FROM items o WHERE o.id=1";
      check_equal(open_query(sql,&real,1,&query),TURBODB_STATUS_OK);
      const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,1);
      check_equal(row.values[1].data.int64_value,2); check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      const char *existence[]={
        "SELECT EXISTS(SELECT 9007199254740992 INTERSECT SELECT 9007199254740993 INTERSECT SELECT ?) AS n",
        "SELECT EXISTS(SELECT 9007199254740992 UNION SELECT 9007199254740993 UNION ALL SELECT ? LIMIT 1 OFFSET 2) AS n",
        "SELECT 9007199254740993 IN(SELECT 9007199254740992 INTERSECT SELECT ?) AS n"};
      const bool expected[]={true,false,true};
      for(size_t i=0;i<sizeof(existence)/sizeof(existence[0]);++i) {
        check_equal(open_query(existence[i],&real,1,&query),TURBODB_STATUS_OK);
        const orm_sql_scan_row output=next(); check_equal(output.values[0].kind,TURBODB_VALUE_BOOLEAN);
        check_equal(output.values[0].data.boolean_value,expected[i]); check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      }
      check_equal(open_query("EXPLAIN SELECT 9223372036854775807+1 AS n UNION SELECT 2 UNION ALL SELECT ?",&real,1,&query),TURBODB_STATUS_OK);
      size_t count=0; while(next().state==ORM_SQL_SCAN_ROW) ++count; check_equal(count,5u);
    }
    it("rolls back an INSERT SELECT prefix when a later converted real exceeds BIGINT") {
      seed(); const turbodb_value_t real=turbodb_f64(0.5); size_t affected=99;
      const char *sql="INSERT INTO items SELECT 4,10 UNION ALL SELECT 5,18446744073709551615 UNION ALL SELECT 6,?";
      check_equal(execute(sql,&real,1,&affected),TURBODB_STATUS_OUT_OF_RANGE); check_false(owner.failed);
      check_equal(open_query("SELECT id,score FROM items ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
      for(int64_t id=1;id<=3;++id) check_equal(next().values[0].data.int64_value,id);
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    }
  }
  group("common numeric conditional results") {
    it("keeps real result metadata across parameter branch selection and reusable execution") {
      turbodb_value_t params[]={turbodb_i64(0),turbodb_i64(INT64_C(9007199254740993)),turbodb_u64(UINT64_MAX),turbodb_f64(0.5)};
      const char *sql="SELECT CASE ? WHEN 0 THEN ? WHEN 1 THEN ? ELSE ? END AS n";
      check_equal(open_query(sql,params,4,&query),TURBODB_STATUS_OK);
      orm_sql_schema_column column;check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK);
      check_equal(column.type.kind,TURBODB_VALUE_DOUBLE);check_false(column.type.nullable);
      for(size_t round=0;round<2;++round) {
        const orm_sql_scan_row row=next();check_equal(row.values[0].kind,TURBODB_VALUE_DOUBLE);
        check_equal(row.values[0].data.double_value,9007199254740992.0);check_equal(next().state,ORM_SQL_SCAN_DONE);
        if(!round) resume_query();
      }
      close_query(&query);params[0]=turbodb_i64(1);
      check_equal(open_query(sql,params,4,&query),TURBODB_STATUS_OK);
      check_equal(next().values[0].data.double_value,18446744073709551616.0);check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("uses converted results in CTE correlated queries grouping and window SUM") {
      seed();
      check_equal(open_query("WITH c AS(SELECT id,COALESCE(score,0.5) AS n FROM items) "
          "SELECT o.id,(SELECT CASE WHEN o.id=1 THEN o.id ELSE c.n END FROM c WHERE c.id=o.id) AS n "
          "FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
      const double expected[]={1.0,20.0,0.5};
      for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) {
        const orm_sql_scan_row row=next();check_equal(row.values[1].kind,TURBODB_VALUE_DOUBLE);
        check_equal(row.values[1].data.double_value,expected[i]);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);close_query(&query);
      check_equal(open_query("SELECT CASE WHEN score IS NULL THEN 0.5 ELSE score END AS k,COUNT(*) AS n "
          "FROM items GROUP BY CASE WHEN score IS NULL THEN 0.5 ELSE score END HAVING k>0 ORDER BY k",NULL,0,&query),TURBODB_STATUS_OK);
      const double keys[]={0.5,10.0,20.0};
      for(size_t i=0;i<sizeof(keys)/sizeof(keys[0]);++i) {
        const orm_sql_scan_row row=next();check_equal(row.values[0].kind,TURBODB_VALUE_DOUBLE);
        check_equal(row.values[0].data.double_value,keys[i]);check_equal(row.values[1].data.int64_value,1);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);close_query(&query);
      check_equal(open_query("SELECT id,SUM(COALESCE(score,0.5)) OVER(ORDER BY id ROWS UNBOUNDED PRECEDING) AS n "
          "FROM items ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
      const double totals[]={10.0,30.0,30.5};
      for(size_t i=0;i<sizeof(totals)/sizeof(totals[0]);++i) {
        const orm_sql_scan_row row=next();check_equal(row.values[1].kind,TURBODB_VALUE_DOUBLE);
        check_equal(row.values[1].data.double_value,totals[i]);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
  }

  group("mixed real predicate SQL execution") {
    it("uses promoted comparison through JOIN correlated IN grouping and window arguments") {
      seed();check_equal(open_query("SELECT a.id FROM items a JOIN items b ON a.id=b.id+0.0 "
          "WHERE a.id IN(SELECT b.id+0.0) ORDER BY a.id",NULL,0,&query),TURBODB_STATUS_OK);
      for(size_t i=0;i<3;++i)check_equal(next().values[0].data.int64_value,(int64_t)i+1);
      check_equal(next().state,ORM_SQL_SCAN_DONE);close_query(&query);
      check_equal(open_query("SELECT CASE id WHEN 2.0 THEN 1 ELSE 0 END AS k,COUNT(*) AS n "
          "FROM items GROUP BY CASE id WHEN 2.0 THEN 1 ELSE 0 END HAVING COUNT(*)>0.0 ORDER BY k",NULL,0,&query),TURBODB_STATUS_OK);
      orm_sql_scan_row row=next();check_equal(row.values[0].data.int64_value,0);check_equal(row.values[1].data.int64_value,2);
      row=next();check_equal(row.values[0].data.int64_value,1);check_equal(row.values[1].data.int64_value,1);
      check_equal(next().state,ORM_SQL_SCAN_DONE);close_query(&query);
      check_equal(open_query("SELECT id,SUM(CASE WHEN id>1.0 THEN 1.0 ELSE 0.0 END) "
          "OVER(ORDER BY id ROWS UNBOUNDED PRECEDING) AS n FROM items ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
      for(size_t i=0;i<3;++i)check_equal(next().values[1].data.double_value,(double)i);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("retains promoted BETWEEN mode when a compiled query resumes with new parameters") {
      const turbodb_value_t params[]={turbodb_i64(INT64_C(9007199254740992)),turbodb_u64(UINT64_C(9007199254740993)),turbodb_f64(9007199254740994.0)};
      check_equal(open_query("SELECT ? BETWEEN ? AND ? AS n",params,3,&query),TURBODB_STATUS_OK);
      for(size_t round=0;round<2;++round) {
        const orm_sql_scan_row row=next();check_equal(row.values[0].kind,TURBODB_VALUE_BOOLEAN);check_true(row.values[0].data.boolean_value);
        check_equal(next().state,ORM_SQL_SCAN_DONE);if(!round)resume_query();
      }
      close_query(&query);turbodb_value_t changed[3];memcpy(changed,params,sizeof(params));changed[2]=turbodb_f64(1.0);
      check_equal(open_query("SELECT ? BETWEEN ? AND ? AS n",changed,3,&query),TURBODB_STATUS_OK);
      check_false(next().values[0].data.boolean_value);check_equal(next().state,ORM_SQL_SCAN_DONE);
      close_query(&query);check_equal(open_query("SELECT NULL BETWEEN TRUE AND 2.0 AS n",NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(next().values[0].kind,TURBODB_VALUE_NULL);check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
  }

  group("mixed real SQL execution") {
    it("computes DOUBLE outputs from integer columns and retains runtime NULLs") {
      seed(); check_equal(open_query("SELECT id+0.5 AS a,MOD(score,3.5) AS b,id/2.0 AS c,"
          "2.0*score AS d,10.0-id AS e FROM items ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
      for(size_t i=0;i<query.columns;++i) {orm_sql_schema_column column;
        check_equal(orm_tidesdb_sql_runtime_column(&query,i,&column,&error),TURBODB_STATUS_OK); check_equal(column.type.kind,TURBODB_VALUE_DOUBLE);}
      for(size_t i=0;i<3;++i) {
        const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
        check_equal(row.values[0].data.double_value,(double)i+1.5);
        check_equal(row.values[2].data.double_value,((double)i+1.0)*0.5);
        check_equal(row.values[4].data.double_value,9.0-(double)i);
        if(i==2) {check_equal(row.values[1].kind,TURBODB_VALUE_NULL);check_equal(row.values[3].kind,TURBODB_VALUE_NULL);}
        else {check_equal(row.values[1].data.double_value,i?2.5:3.0);check_equal(row.values[3].data.double_value,i?40.0:20.0);}
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("shares real arithmetic across grouping windows and compiled replay") {
      seed(); check_equal(open_query("SELECT MOD(id,2.0) AS k,SUM(score*0.5) AS s,COUNT(*) AS n "
          "FROM items GROUP BY MOD(id,2.0) ORDER BY k",NULL,0,&query),TURBODB_STATUS_OK);
      for(size_t round=0;round<2;++round) {
        orm_sql_scan_row row=next(); check_equal(row.values[0].data.double_value,0.0); check_equal(row.values[1].data.double_value,10.0);check_equal(row.values[2].data.int64_value,1);
        row=next();check_equal(row.values[0].data.double_value,1.0);check_equal(row.values[1].data.double_value,5.0);check_equal(row.values[2].data.int64_value,2);
        check_equal(next().state,ORM_SQL_SCAN_DONE); if(!round) resume_query();
      }
      close_query(&query); check_equal(open_query("SELECT id,SUM(id*0.5) OVER(ORDER BY id ROWS UNBOUNDED PRECEDING) AS s FROM items ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
      const double expected[]={0.5,1.5,3.0};
      for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) {const orm_sql_scan_row row=next();check_equal(row.values[1].data.double_value,expected[i]);}
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("promotes nested CTE and correlated results using original captured types") {
      seed(); check_equal(open_query("WITH c AS(SELECT id,score/2.0 AS n FROM items) "
          "SELECT c.id,(SELECT c.n+0.5) AS v FROM c ORDER BY c.id",NULL,0,&query),TURBODB_STATUS_OK);
      for(size_t i=0;i<3;++i) {const orm_sql_scan_row row=next();check_equal(row.values[0].data.int64_value,(int64_t)i+1);
        if(i==2) check_equal(row.values[1].kind,TURBODB_VALUE_NULL);else {check_equal(row.values[1].kind,TURBODB_VALUE_DOUBLE);check_equal(row.values[1].data.double_value,i?10.5:5.5);}}
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
  }

  group("numeric scalar SQL execution") {
    it("evaluates parameters integer extremes DOUBLE and NULL after releasing the AST") {
      const turbodb_value_t params[]={turbodb_i64(-7),turbodb_u64(UINT64_MAX),turbodb_f64(-1.25),turbodb_null()};
      check_equal(open_query("SELECT ABS(?) AS a,SIGN(?) AS s,FLOOR(?) AS f,CEILING(?) AS n",params,4,&query),TURBODB_STATUS_OK);
      const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
      check_equal(row.values[0].data.int64_value,7);
      check_equal(row.values[1].kind,TURBODB_VALUE_INT64); check_equal(row.values[1].data.int64_value,1);
      check_equal(row.values[2].kind,TURBODB_VALUE_DOUBLE); check_equal(row.values[2].data.double_value,-2.0);
      check_equal(row.values[3].kind,TURBODB_VALUE_NULL); check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("shares numeric programs across grouping HAVING sorting and equivalent CEILING keys") {
      seed();
      check_equal(open_query("SELECT CEIL(score) AS k,COUNT(*) AS n FROM items GROUP BY CEILING(score) HAVING SIGN(CEILING(score))=1 ORDER BY ABS(CEIL(score)) DESC",NULL,0,&query),TURBODB_STATUS_OK);
      orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,20); check_equal(row.values[1].data.uint64_value,1u);
      row=next(); check_equal(row.values[0].data.int64_value,10); check_equal(row.values[1].data.uint64_value,1u);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("runs nested CTE JOIN subquery UNION and window argument functions") {
      seed();
      check_equal(open_query("WITH c AS (SELECT ABS(-id) AS id FROM items) SELECT ABS(a.id) AS n,(SELECT SIGN(a.id)) AS s,LAG(ABS(a.id),1,0) OVER(ORDER BY FLOOR(a.id)) AS p FROM c a JOIN items b ON ABS(-a.id)=b.id ORDER BY n",NULL,0,&query),TURBODB_STATUS_OK);
      for(size_t i=0;i<3;++i) {
        const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,(int64_t)i+1);
        check_equal(row.values[1].data.int64_value,1); check_equal(row.values[2].data.int64_value,(int64_t)i);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      check_equal(open_query("SELECT FLOOR(-1.25) AS n UNION ALL SELECT CEILING(1.25) ORDER BY n",NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(next().values[0].data.double_value,-2.0); check_equal(next().values[0].data.double_value,2.0);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("keeps overflow lazy for branches EXPLAIN LIMIT zero and cancellation") {
      seed();
      check_equal(open_query("SELECT CASE WHEN TRUE THEN 7 ELSE ABS(-9223372036854775808) END AS n FROM items LIMIT 1",NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(next().values[0].data.int64_value,7); close_query(&query);
      const char *sql[]={"EXPLAIN SELECT ABS(-9223372036854775808) AS n FROM items",
        "SELECT ABS(-9223372036854775808) AS n FROM items LIMIT 0",
        "SELECT ABS(-9223372036854775808) AS n FROM items"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_OK);
        const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
        if(i==2) check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_OK);
        const orm_sql_scan_row row=next(); check_equal(row.state,i==0?ORM_SQL_SCAN_ROW:i==1?ORM_SQL_SCAN_DONE:ORM_SQL_SCAN_CANCELLED);
        check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); close_query(&query);
      }
      check_equal(open_query(sql[2],NULL,0,&query),TURBODB_STATUS_OK);
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_contains(error.message,"at byte"); check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    }
    it("rejects unsupported coercion modifiers and bad arity before scanning") {
      seed();
      const char *sql[]={"SELECT ABS() AS n FROM items","SELECT SIGN(1,2) AS n FROM items",
        "SELECT FLOOR(TRUE) AS n FROM items","SELECT CEIL('1') AS n FROM items",
        "SELECT ABS(DISTINCT id) AS n FROM items"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
        check_equal(open_query(sql[i],NULL,0,&query),i<2?TURBODB_STATUS_SQL_ERROR:TURBODB_STATUS_UNSUPPORTED);
        check_null(query.owner); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      const char *invalid="SELECT ABS(id) OVER() AS n FROM items";
      sqlparser_document *doc=NULL; sqlparser_error parse_error;
      check_equal(sqlparser_parse(invalid,strlen(invalid),NULL,&doc,&parse_error),SQLPARSER_SYNTAX_ERROR);
      check_null(doc);
    }
  }
  group("USING and NATURAL SQL execution") {
    it("coalesces native outer common columns and orders bare stars independently of physical slots") {
      seed();
      const char *sql[]={
        "SELECT * FROM items a LEFT JOIN (SELECT 2 AS id,200 AS y UNION ALL SELECT 4,400) b USING(id) ORDER BY id",
        "SELECT * FROM items a NATURAL LEFT JOIN (SELECT 2 AS id,200 AS y UNION ALL SELECT 4,400) b ORDER BY id",
        "SELECT * FROM items a RIGHT JOIN (SELECT 2 AS id,200 AS y UNION ALL SELECT 4,400) b USING(id) ORDER BY id",
        "SELECT * FROM items a NATURAL RIGHT JOIN (SELECT 2 AS id,200 AS y UNION ALL SELECT 4,400) b ORDER BY id"};
      for(size_t mode=0;mode<sizeof(sql)/sizeof(sql[0]);++mode) {
        check_equal(open_query(sql[mode],NULL,0,&query),TURBODB_STATUS_OK); check_equal(query.columns,3u);
        orm_sql_schema_column column; check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK); text_is(column.name,"id");
        check_equal(orm_tidesdb_sql_runtime_column(&query,1,&column,&error),TURBODB_STATUS_OK); text_is(column.name,mode<2?"score":"y");
        const size_t rows=mode<2?3:2;
        for(size_t i=0;i<rows;++i) {
          const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
          const int64_t id=mode<2?(int64_t)i+1:(int64_t)(i+1)*2;
          check_equal(row.values[0].data.int64_value,id);
          if(mode<2) { check_equal(row.values[2].kind,id==2?TURBODB_VALUE_INT64:TURBODB_VALUE_NULL); }
          else { check_equal(row.values[1].data.int64_value,id*100); check_equal(row.values[2].kind,id==2?TURBODB_VALUE_INT64:TURBODB_VALUE_NULL); }
        }
        check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      }
    }
    it("retains original qualified columns and captures the merged name across nested queries and LATERAL") {
      seed();
      check_equal(open_query("SELECT id,(SELECT id) AS copied,a.id AS aid,b.id AS bid FROM items a RIGHT JOIN (SELECT 2 AS id UNION ALL SELECT 4) b USING(id) ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
      orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,2); check_equal(row.values[1].data.int64_value,2);
      row=next(); check_equal(row.values[0].data.int64_value,4); check_equal(row.values[1].data.int64_value,4);
      check_equal(row.values[2].kind,TURBODB_VALUE_NULL); check_equal(row.values[3].data.int64_value,4);
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      check_equal(open_query("SELECT id,d.p FROM items a LEFT JOIN LATERAL (SELECT id,score AS p) d USING(id) ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
      for(size_t i=0;i<3;++i) { row=next(); check_equal(row.values[0].data.int64_value,(int64_t)i+1); check_equal(row.values[1].kind,i==2?TURBODB_VALUE_NULL:TURBODB_VALUE_INT64); }
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      check_equal(open_query("SELECT a.*,b.id AS bid FROM items a RIGHT JOIN (SELECT 4 AS id) b USING(id)",NULL,0,&query),TURBODB_STATUS_OK);
      row=next(); check_equal(row.count,3u); check_equal(row.values[0].kind,TURBODB_VALUE_NULL); check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
      check_equal(row.values[2].data.int64_value,4); check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("uses ordinary NULL equality and retains duplicate matches for multiple common keys") {
      seed(); size_t affected=0; check_equal(execute("INSERT INTO items VALUES(4,20)",NULL,0,&affected),TURBODB_STATUS_OK);
      check_equal(open_query("SELECT score,COUNT(*) AS n FROM items a JOIN items b USING(score) GROUP BY score ORDER BY score",NULL,0,&query),TURBODB_STATUS_OK);
      orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,10); check_equal(row.values[1].data.uint64_value,1u);
      row=next(); check_equal(row.values[0].data.int64_value,20); check_equal(row.values[1].data.uint64_value,4u);
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      check_equal(open_query("SELECT * FROM items a NATURAL INNER JOIN items b ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
      const int64_t ids[]={1,2,4}; for(size_t i=0;i<sizeof(ids)/sizeof(ids[0]);++i) { row=next(); check_equal(row.count,2u); check_equal(row.values[0].data.int64_value,ids[i]); }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("propagates common names through chains CTE grouping windows and ON parents") {
      seed();
      check_equal(open_query("SELECT id FROM items a LEFT JOIN items b USING(id) RIGHT JOIN items c USING(id) ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
      for(size_t i=0;i<3;++i) check_equal(next().values[0].data.int64_value,(int64_t)i+1);
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      check_equal(open_query("WITH c AS (SELECT id FROM items) SELECT id,ROW_NUMBER() OVER(ORDER BY id) AS n FROM c a NATURAL JOIN items b ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
      for(size_t i=0;i<3;++i) { const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,(int64_t)i+1); check_equal(row.values[1].data.uint64_value,i+1); }
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      check_equal(open_query("SELECT * FROM items a JOIN (SELECT id FROM items) b USING(id) JOIN (SELECT id AS k FROM items) c ON id=k ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(query.columns,3u); for(size_t i=0;i<3;++i) { const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,(int64_t)i+1); check_equal(row.values[2].data.int64_value,(int64_t)i+1); }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("keeps no-common NATURAL outer semantics and skips all reads at LIMIT zero or cancellation") {
      seed();
      check_equal(open_query("SELECT * FROM items a NATURAL RIGHT JOIN (SELECT 9 AS k) b ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
      for(size_t i=0;i<3;++i) { const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,9); check_equal(row.values[1].data.int64_value,(int64_t)i+1); }
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      check_equal(open_query("SELECT * FROM items a NATURAL LEFT JOIN (SELECT 9 AS k WHERE FALSE) b ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
      for(size_t i=0;i<3;++i) { const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,(int64_t)i+1); check_equal(row.values[2].kind,TURBODB_VALUE_NULL); }
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      const char *sql[]={"SELECT * FROM items a NATURAL JOIN items b LIMIT 0","SELECT * FROM items a JOIN items b USING(id,score)"};
      for(size_t mode=0;mode<sizeof(sql)/sizeof(sql[0]);++mode) {
        check_equal(open_query(sql[mode],NULL,0,&query),TURBODB_STATUS_OK); const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
        if(mode) check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_OK);
        check_equal(next().state,mode?ORM_SQL_SCAN_CANCELLED:ORM_SQL_SCAN_DONE);
        check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); close_query(&query);
      }
    }
    it("explains common joins without reading business rows and rejects invalid operands without owners") {
      seed();
      check_equal(open_query("EXPLAIN SELECT * FROM items a NATURAL RIGHT JOIN items b",NULL,0,&query),TURBODB_STATUS_OK);
      const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
      while(next().state==ORM_SQL_SCAN_ROW) {}
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); close_query(&query);
      const char *sql[]={"SELECT * FROM items a JOIN items b USING(missing)","SELECT * FROM items a JOIN items b USING(id,id)",
        "SELECT id FROM items a JOIN items b ON TRUE JOIN items c USING(id)"};
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_SQL_ERROR); check_null(query.owner); check_equal(owner.active_sources,0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      const turbodb_value_t value=turbodb_u64(2);
      check_equal(open_query("SELECT * FROM items a JOIN (SELECT ? AS id) b USING(id)",&value,1,&query),TURBODB_STATUS_TYPE_ERROR);
      check_null(query.owner); check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  group("compound INTERSECT and EXCEPT SQL execution") {
    it("preserves ALL counts and DISTINCT tuple membership including NULL") {
      seed(); size_t affected=0;
      check_equal(execute("INSERT INTO items VALUES(4,10),(5,NULL)",NULL,0,&affected),TURBODB_STATUS_OK);
      const char *sql[]={
        "SELECT score AS k FROM items INTERSECT ALL SELECT score FROM items WHERE id<=3 ORDER BY k",
        "SELECT score AS k FROM items INTERSECT SELECT score FROM items WHERE id<=3 ORDER BY k",
        "SELECT score AS k FROM items INTERSECT DISTINCT SELECT score FROM items WHERE id<=3 ORDER BY k",
        "SELECT score AS k FROM items EXCEPT ALL SELECT score FROM items WHERE id<=3 ORDER BY k",
        "SELECT score AS k FROM items EXCEPT SELECT score FROM items WHERE id<=3 ORDER BY k",
        "SELECT score AS k FROM items EXCEPT DISTINCT SELECT score FROM items WHERE id<=3 ORDER BY k"};
      const size_t counts[]={3,3,3,2,0,0};
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(size_t mode=0;mode<sizeof(sql)/sizeof(sql[0]);++mode) {
        check_equal(open_query(sql[mode],NULL,0,&query),TURBODB_STATUS_OK);
        orm_sql_schema_column column; check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK);
        text_is(column.name,"k"); check_equal(column.type.kind,TURBODB_VALUE_INT64); check_true(column.type.nullable);
        for(size_t i=0;i<counts[mode];++i) {
          const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
          check_equal(row.values[0].kind,i?TURBODB_VALUE_INT64:TURBODB_VALUE_NULL);
          if(i) check_equal(row.values[0].data.int64_value,i==1?10:20);
        }
        check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
        check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
    }
    it("compares every output column rather than just the leading key") {
      check_equal(open_query("(SELECT 1 AS id,10 AS score UNION ALL SELECT 1,20) EXCEPT ALL SELECT 1,10",NULL,0,&query),TURBODB_STATUS_OK);
      orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.count,2u);
      check_equal(row.values[0].data.int64_value,1); check_equal(row.values[1].data.int64_value,20);
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      check_equal(open_query("(SELECT NULL AS n,1 AS k UNION ALL SELECT NULL,2) INTERSECT ALL SELECT NULL,2",NULL,0,&query),TURBODB_STATUS_OK);
      row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].kind,TURBODB_VALUE_NULL);
      check_equal(row.values[1].data.int64_value,2); check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("executes mixed precedence left association and independent branch tails") {
      const char *sql[]={"SELECT 1 AS n UNION SELECT 2 INTERSECT SELECT 2 EXCEPT SELECT 1",
        "SELECT 1 AS n EXCEPT SELECT 1 UNION SELECT 2",
        "(SELECT 3 AS n UNION ALL SELECT 2 ORDER BY n LIMIT 1) INTERSECT (SELECT 2 UNION ALL SELECT 1 ORDER BY 1 DESC LIMIT 1) ORDER BY n"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_OK);
        const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,2);
        check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      }
    }
    it("retains complete set operands when EXISTS asks for only one witness") {
      check_equal(open_query("SELECT EXISTS(SELECT 1 INTERSECT SELECT 2) AS a,EXISTS(SELECT 1 EXCEPT SELECT 1) AS b,EXISTS(SELECT 1 EXCEPT SELECT 2) AS c,EXISTS(SELECT 1,2 INTERSECT SELECT 1,3) AS d",NULL,0,&query),TURBODB_STATUS_OK);
      const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
      const bool expected[]={false,false,true,false};
      for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) {
        check_equal(row.values[i].kind,TURBODB_VALUE_BOOLEAN); check_equal(row.values[i].data.boolean_value,expected[i]);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("applies EXISTS OFFSET after computing ALL duplicate counts") {
      check_equal(open_query("SELECT EXISTS((SELECT 1 UNION ALL SELECT 1) EXCEPT ALL SELECT 1 LIMIT 1 OFFSET 1) AS a,EXISTS((SELECT 1 UNION ALL SELECT 1) INTERSECT ALL (SELECT 1 UNION ALL SELECT 1) LIMIT 1 OFFSET 1) AS b",NULL,0,&query),TURBODB_STATUS_OK);
      const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
      check_false(row.values[0].data.boolean_value); check_true(row.values[1].data.boolean_value);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("does not prune failing operand projections under EXISTS") {
      check_equal(open_query("SELECT EXISTS(SELECT 9223372036854775807+1 INTERSECT SELECT 1) AS n",NULL,0,&query),TURBODB_STATUS_OK);
      orm_sql_scan_row row={.count=99};
      check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u);
      check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u);
      check_false(owner.failed);
    }
    it("rebinds correlated scalar and EXISTS set operands for each outer row") {
      seed(); check_equal(open_query("SELECT o.id,EXISTS(SELECT o.id INTERSECT SELECT 2) AS hit,(SELECT o.id EXCEPT SELECT 2) AS n FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
      for(int64_t i=1;i<=3;++i) {
        const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
        check_equal(row.values[0].data.int64_value,i); check_equal(row.values[1].data.boolean_value,i==2);
        check_equal(row.values[2].kind,i==2?TURBODB_VALUE_NULL:TURBODB_VALUE_INT64);
        if(i!=2) check_equal(row.values[2].data.int64_value,i);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("composes set definitions with CTE derived and LATERAL sources") {
      seed(); check_equal(open_query("WITH c(n) AS (SELECT id FROM items EXCEPT SELECT 2) SELECT d.n FROM (SELECT n FROM c INTERSECT SELECT id FROM items) d ORDER BY d.n",NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(next().values[0].data.int64_value,1); check_equal(next().values[0].data.int64_value,3);
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
      check_equal(open_query("SELECT o.id,d.n FROM items o JOIN LATERAL (SELECT o.id AS n INTERSECT SELECT 2) d ON TRUE",NULL,0,&query),TURBODB_STATUS_OK);
      const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
      check_equal(row.values[0].data.int64_value,2); check_equal(row.values[1].data.int64_value,2);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("explains each set branch and result without reading or evaluating operands") {
      seed(); check_equal(open_query("EXPLAIN SELECT id+9223372036854775807 AS n FROM items UNION SELECT 2 INTERSECT ALL SELECT 2 EXCEPT SELECT 1",NULL,0,&query),TURBODB_STATUS_OK);
      const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS], materialized=budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
      const char *types[]={"PRIMARY","UNION","INTERSECT","INTERSECT RESULT","UNION RESULT","EXCEPT","EXCEPT RESULT"};
      for(size_t i=0;i<sizeof(types)/sizeof(types[0]);++i) {
        const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW); text_is(row.values[1].data.text_value,types[i]);
        check_equal(row.values[0].kind,i==3||i==4||i==6?TURBODB_VALUE_NULL:TURBODB_VALUE_INT64);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],materialized);
    }
    it("preserves set mode and copied parameters across execution resume after AST destruction") {
      turbodb_value_t params[]={turbodb_u64(UINT64_MAX),turbodb_u64(UINT64_MAX)};
      check_equal(open_query("SELECT ? AS n INTERSECT ALL SELECT ?",params,2,&query),TURBODB_STATUS_OK);
      params[0]=turbodb_u64(0); params[1]=turbodb_u64(1);
      for(size_t round=0;round<3;++round) {
        if(round) resume_query();
        const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
        check_equal(row.values[0].kind,TURBODB_VALUE_UINT64); check_equal(row.values[0].data.uint64_value,UINT64_MAX);
        check_equal(next().state,ORM_SQL_SCAN_DONE);
      }
    }
    it("keeps zero-limit and cancellation lazy while retaining transaction leases until close") {
      seed(); const char *sql[]={"SELECT id FROM items INTERSECT SELECT id FROM items LIMIT 0",
        "SELECT id FROM items EXCEPT ALL SELECT id FROM items"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_OK);
        const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
        if(i) { check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_CANCELLED); }
        else check_equal(next().state,ORM_SQL_SCAN_DONE);
        check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
        check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_BUSY);
        close_query(&query); check_equal(owner.active_sources,0u);
      }
    }
    it("refunds each intercepted first-pull allocation failure without publishing a set row") {
      const char *sql[]={"(SELECT 1 AS n UNION ALL SELECT 1) INTERSECT ALL SELECT 1",
        "(SELECT 1 AS n UNION ALL SELECT 1) INTERSECT SELECT 1",
        "(SELECT 1 AS n UNION ALL SELECT 1) EXCEPT ALL SELECT 2",
        "(SELECT 1 AS n UNION ALL SELECT 1) EXCEPT SELECT 2"};
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(size_t mode=0;mode<sizeof(sql)/sizeof(sql[0]);++mode) {
        check_equal(open_query(sql[mode],NULL,0,&query),TURBODB_STATUS_OK); reserves=resizes=0;
        check_equal(next().state,ORM_SQL_SCAN_ROW); const size_t allocations[]={reserves,resizes}; close_query(&query);
        for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
          next_statement(); check_equal(open_query(sql[mode],NULL,0,&query),TURBODB_STATUS_OK); reserves=resizes=0;
          if(pass) fail_resize=point; else fail_reserve=point;
          orm_sql_scan_row row={.count=99};
          check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_OUT_OF_MEMORY); check_equal(row.count,99u);
          fail_reserve=fail_resize=0; close_query(&query); check_false(owner.failed); check_equal(owner.active_sources,0u);
          check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
          check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
        }
      }
    }
    it("bounds both materialized operands and every preparation step before the first result") {
      const char sql[]="(SELECT 1 AS n UNION ALL SELECT 1) EXCEPT ALL SELECT 2";
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK);
      const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]; check_equal(next().state,ORM_SQL_SCAN_ROW);
      const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start; close_query(&query);
      for(uint64_t point=0;point<steps;++point) {
        next_statement(); check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK);
        budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
        orm_sql_scan_row row={.count=99};
        check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u);
        close_query(&query); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=LIMIT;
        check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u); check_false(owner.failed);
      }
      next_statement(); check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK);
      budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=2;
      orm_sql_scan_row row={.count=99};
      check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u);
      close_query(&query); budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=LIMIT;
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u); check_equal(owner.active_sources,0u);
    }
    it("rejects incompatible widths types and byte membership before publishing queries") {
      const char *sql[]={"SELECT 1 INTERSECT SELECT 1,2","SELECT 'a' EXCEPT ALL SELECT 'a'",
        "EXPLAIN SELECT 'a' INTERSECT SELECT 'a'","SELECT 1 EXCEPT SELECT TRUE"};
      const turbodb_status_t statuses[]={TURBODB_STATUS_SQL_ERROR,TURBODB_STATUS_UNSUPPORTED,TURBODB_STATUS_UNSUPPORTED,TURBODB_STATUS_UNSUPPORTED};
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        check_equal(open_query(sql[i],NULL,0,&query),statuses[i]); check_null(query.owner);
        check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
    }
  }
  it("reopens a bound unit query and copies each execution's parameter bytes") {
    char payload[]="owned"; turbodb_value_t params[]={turbodb_text(payload),turbodb_i64(7)};
    check_equal(open_query("SELECT ? AS label, ? AS n",params,2,&query),TURBODB_STATUS_OK);
    const orm_sql_select *plan=orm_sql_runtime_plan(&query);
    const uint64_t ast=budget.used.value[ORM_SQL_BUDGET_AST_NODES];
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    for(size_t round=0;round<3;++round) {
      if(round) {
        check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
        check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
        orm_sql_scan_row preserved={.state=ORM_SQL_SCAN_CANCELLED};
        check_equal(orm_tidesdb_sql_runtime_next(&query,&preserved,&error),TURBODB_STATUS_INVALID_STATE);
        check_equal(preserved.state,ORM_SQL_SCAN_CANCELLED);
        payload[0]='o'; params[1]=turbodb_i64(7);
        check_equal(orm_sql_runtime_execution_open(&query,params,2,NULL,&error),TURBODB_STATUS_OK);
      }
      payload[0]='X'; params[1]=turbodb_i64(99);
      const orm_sql_scan_row row=next(); text_is(row.values[0].data.text_value,"owned");
      check_equal(row.values[1].data.int64_value,7); check_equal(next().state,ORM_SQL_SCAN_DONE);
      check_true(orm_sql_runtime_plan(&query)==plan); check_equal(owner.active_sources,1u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_AST_NODES],ast);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    }
  }
  it("reopens native JOIN grouped and compound plans without AST or metadata rebinding") {
    seed();
    const struct { const char *sql; int64_t values[3]; size_t count; } cases[]={
      {"SELECT id FROM items ORDER BY id DESC",{3,2,1},3},
      {"SELECT COUNT(*) AS n FROM items GROUP BY id ORDER BY id",{1,1,1},3},
      {"SELECT a.id FROM items a LEFT JOIN items b ON a.id=b.id ORDER BY a.id DESC",{3,2,1},3},
      {"SELECT a.id FROM items a RIGHT JOIN items b ON a.id=b.id ORDER BY a.id DESC",{3,2,1},3},
      {"(SELECT id FROM items ORDER BY id DESC LIMIT 2) UNION SELECT 1 ORDER BY id DESC LIMIT 2 OFFSET 1",{2,1},2},
      {"SELECT id FROM items WHERE id<3 UNION ALL SELECT 3 ORDER BY id DESC",{3,2,1},3}};
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
      check_equal(open_query(cases[i].sql,NULL,0,&query),TURBODB_STATUS_OK);
      const orm_sql_select *plan=orm_sql_runtime_plan(&query);
      const uint64_t ast=budget.used.value[ORM_SQL_BUDGET_AST_NODES];
      check_equal(next().values[0].data.int64_value,cases[i].values[0]);
      for(size_t round=0;round<2;++round) {
        check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
        check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
        const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
        check_equal(orm_sql_runtime_execution_open(&query,NULL,0,NULL,&error),TURBODB_STATUS_OK);
        check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
        check_equal(budget.used.value[ORM_SQL_BUDGET_AST_NODES],ast);
        check_true(orm_sql_runtime_plan(&query)==plan);
        for(size_t j=0;j<cases[i].count;++j) check_equal(next().values[0].data.int64_value,cases[i].values[j]);
        check_equal(next().state,ORM_SQL_SCAN_DONE);
      }
      close_query(&query);
    }
  }
  it("retains original parameter numbering through UNION and group pagination on reopen") {
    seed(); const turbodb_value_t params[]={turbodb_i64(2),turbodb_i64(2),turbodb_i64(9),turbodb_i64(2),turbodb_i64(1)};
    check_equal(open_query("(SELECT id FROM items WHERE id>=? ORDER BY id DESC LIMIT ?) UNION ALL SELECT ? ORDER BY id DESC LIMIT ? OFFSET ?",
        params,sizeof(params)/sizeof(params[0]),&query),TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_execution_open(&query,params,sizeof(params)/sizeof(params[0]),NULL,&error),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,3); check_equal(next().values[0].data.int64_value,2);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("preserves cardinality demand across compound execution rounds") {
    sqlparser_document *doc=parse("SELECT 9223372036854775807+1 AS ignored UNION ALL SELECT 2 LIMIT 1 OFFSET 1");
    const orm_sql_query_scope scope={.document=doc,.root=sqlparser_statements(doc).first,
        .max_depth=DEPTH,.budget=&budget,.demand=ORM_SQL_QUERY_CARDINALITY};
    check_equal(orm_sql_runtime_scope_open(&scope,&owner,NULL,false,NULL,&query,&error),TURBODB_STATUS_OK);
    sqlparser_document_destroy(doc);
    check_equal(orm_sql_runtime_execution_resume(&query,&error),TURBODB_STATUS_UNSUPPORTED);
    for(size_t round=0;round<2;++round) {
      check_equal(next().state,ORM_SQL_SCAN_ROW); check_equal(next().state,ORM_SQL_SCAN_DONE);
      check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
      check_equal(orm_sql_runtime_execution_open(&query,NULL,0,NULL,&error),TURBODB_STATUS_OK);
    }
  }
  it("requires execution close and rejects reentry without changing an active run") {
    check_equal(orm_sql_runtime_execution_close(NULL,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(orm_sql_runtime_execution_open(&query,NULL,0,NULL,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(open_query("SELECT 1",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_execution_open(&query,NULL,0,NULL,&error),TURBODB_STATUS_BUSY);
    query.as.select.run.scan.evaluating=true;
    check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_BUSY);
    query.as.select.run.scan.evaluating=false; check_equal(next().state,ORM_SQL_SCAN_ROW);
    check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_execution_open(&query,NULL,0,NULL,&error),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_ROW);
  }
  it("does not reopen metadata queries") {
    seed(); const char *sql[]={"SHOW TABLES","EXPLAIN SELECT id FROM items",
        "EXPLAIN SELECT 1 UNION SELECT 2"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_UNSUPPORTED);
      check_equal(orm_sql_runtime_execution_resume(&query,&error),TURBODB_STATUS_UNSUPPORTED);
      check_false(query.execution_closed); check_equal(next().state,ORM_SQL_SCAN_ROW); close_query(&query);
    }
  }
  it("keeps execution failures latched after releasing SELECT or UNION runs") {
    const char *sql[]={"SELECT 9223372036854775807+1 AS n","SELECT 9223372036854775807+1 AS n UNION ALL SELECT 2"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_OK); orm_sql_scan_row row={0};
      const turbodb_status_t failure=orm_tidesdb_sql_runtime_next(&query,&row,&error); check_not_equal(failure,TURBODB_STATUS_OK);
      const turbodb_error_t cause=error;
      check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
      check_equal(orm_sql_runtime_execution_open(&query,NULL,0,NULL,&error),failure);
      check_equal(strcmp(error.message,cause.message),0); check_true(query.execution_closed); close_query(&query);
    }
  }
  it("releases a partially reopened compound when parameter validation fails") {
    const turbodb_value_t valid=turbodb_i64(1),invalid=turbodb_text("bad");
    check_equal(open_query("SELECT ? AS n UNION ALL SELECT 2",&valid,1,&query),TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_sql_runtime_execution_open(&query,&invalid,1,NULL,&error),TURBODB_STATUS_TYPE_ERROR);
    check_true(query.execution_closed); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    check_equal(orm_sql_runtime_execution_open(&query,&valid,1,NULL,&error),TURBODB_STATUS_TYPE_ERROR);
  }
  it("cleans up every intercepted allocation failure while reopening a compiled compound") {
    seed(); const char *sql="SELECT a.id FROM items a JOIN items b ON a.id=b.id UNION SELECT 4 ORDER BY id DESC";
    check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
    reserves=resizes=0; check_equal(orm_sql_runtime_execution_open(&query,NULL,0,NULL,&error),TURBODB_STATUS_OK);
    const size_t allocations[]={reserves,resizes}; close_query(&query);
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
      next_statement(); check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(orm_sql_runtime_execution_open(&query,NULL,0,NULL,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      fail_reserve=fail_resize=0;
      check_true(query.execution_closed); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      check_equal(orm_sql_runtime_execution_open(&query,NULL,0,NULL,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      close_query(&query); check_equal(owner.active_sources,0u);
    }
  }
  it("refunds execution workspace at every reopen step boundary") {
    seed(); const char *sql="SELECT id FROM items UNION SELECT 4 ORDER BY id DESC";
    check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_sql_runtime_execution_open(&query,NULL,0,NULL,&error),TURBODB_STATUS_OK);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start; close_query(&query);
    for(uint64_t point=0;point<steps;++point) {
      next_statement(); check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      check_equal(orm_sql_runtime_execution_open(&query,NULL,0,NULL,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_true(query.execution_closed); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=LIMIT;
      close_query(&query); check_equal(owner.active_sources,0u);
    }
  }
  it("reuses the statement's external scalar dependency registry without losing its leases") {
    check_equal(open_query("SELECT 7 AS n",NULL,0,&query),TURBODB_STATUS_OK);
    open_subquery(ORM_SQL_SUBQUERY_SCALAR,NULL);
    orm_sql_expr_query_source *dependency=orm_tidesdb_sql_subquery_source(&subquery);
    const orm_sql_expr_query_sources sources={&dependency,1};
    sqlparser_document *doc=parse("SELECT (SELECT 7) AS n UNION ALL SELECT 0");
    sqlparser_id identity=0;
    for(sqlparser_id id=1;id<=sqlparser_node_count(doc);++id)
      if(sqlparser_get_node(doc,id)->kind==SQLPARSER_SUBQUERY) identity=id;
    check_not_equal(identity,0u);
    const orm_sql_expr_query_binding binding={identity,dependency->type};
    const orm_sql_query_scope scope={.document=doc,.root=sqlparser_statements(doc).first,.max_depth=DEPTH,
        .budget=&budget,.queries=&binding,.query_count=1};
    check_equal(orm_sql_runtime_scope_open(&scope,&owner,NULL,false,&sources,&other,&error),TURBODB_STATUS_OK);
    sqlparser_document_destroy(doc); check_equal(subquery.state,ORM_SQL_SUBQUERY_PENDING);
    for(size_t round=0;round<2;++round) {
      check_greater(dependency->active_runs,0u); orm_sql_scan_row row={0};
      check_equal(orm_tidesdb_sql_runtime_next(&other,&row,&error),TURBODB_STATUS_OK); check_equal(row.values[0].data.int64_value,7);
      check_equal(orm_tidesdb_sql_runtime_next(&other,&row,&error),TURBODB_STATUS_OK); check_equal(row.values[0].data.int64_value,0);
      check_equal(orm_tidesdb_sql_runtime_next(&other,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
      check_equal(subquery.state,ORM_SQL_SUBQUERY_READY);
      check_equal(orm_sql_runtime_execution_close(&other,&error),TURBODB_STATUS_OK); check_equal(dependency->active_runs,0u);
      check_equal(orm_sql_runtime_execution_open(&other,NULL,0,&sources,&error),TURBODB_STATUS_OK);
    }
    close_query(&other); check_equal(dependency->active_runs,0u);
  }
  it("uses runtime execution reopening as a recursive factory with an external self source") {
    seed(); check_equal(open_query("SELECT 1 AS n",NULL,0,&other),TURBODB_STATUS_OK);
    orm_sql_schema_column seed_column;
    check_equal(orm_tidesdb_sql_runtime_column(&other,0,&seed_column,&error),TURBODB_STATUS_OK);
    subquery_type=seed_column.type;
    subquery_source=(orm_sql_row_source){&budget,&subquery_type,1,&other,subquery_pull,false};
    recursion.input_type=seed_column.type; recursion.input_type.nullable=true;
    recursion.proxy=(orm_sql_row_source){&budget,&recursion.input_type,1,&recursion,recursive_pull,false};
    const orm_sql_schema_column column={vstr_from_cstr("n"),recursion.input_type};
    const orm_sql_table_schema schema={vstr_from_cstr("r"),&column,1};
    sqlparser_document *doc=parse("SELECT r.n+i.id AS n FROM r JOIN items i ON i.id=1 WHERE r.n<4");
    const sqlparser_id root=sqlparser_statements(doc).first;
    const sqlparser_node *statement=sqlparser_get_node(doc,root);
    const sqlparser_node *join=sqlparser_get_node(doc,statement->as.select.from);
    const orm_sql_derived_binding binding={join->as.join.left,&schema,&recursion.proxy};
    const orm_sql_query_scope scope={.document=doc,.root=root,.max_depth=DEPTH,.budget=&budget,
        .derived=&binding,.derived_count=1};
    check_equal(orm_sql_runtime_scope_open(&scope,&owner,NULL,false,NULL,&query,&error),TURBODB_STATUS_OK);
    sqlparser_document_destroy(doc);
    check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK); check_false(recursion.proxy.active);
    recursion.output_type=orm_tidesdb_sql_select_column_at(orm_sql_runtime_plan(&query),0)->type;
    recursion.member=(orm_sql_row_source){&budget,&recursion.output_type,1,&query,subquery_pull,false};
    const orm_sql_cte_recursion spec={&recursion,recursive_open,recursive_close,DEPTH,false};
    check_equal(orm_sql_cte_store_open_recursive(&subquery_source,&spec,&recursion.store,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_reader_open(&recursion.store,&recursion.reader,&error),TURBODB_STATUS_OK);
    check_equal(recursion.opens,0u);
    orm_sql_row_source *result=orm_sql_cte_reader_source(&recursion.reader);
    for(int64_t n=1;n<=4;++n) {
      const turbodb_value_t *row=NULL; check_equal(result->next(result->context,&row,&error),TURBODB_STATUS_OK);
      check_not_null(row); check_equal(row[0].data.int64_value,n);
    }
    const turbodb_value_t *row=NULL; check_equal(result->next(result->context,&row,&error),TURBODB_STATUS_OK); check_null(row);
    check_equal(recursion.opens,4u); check_equal(recursion.closes,recursion.opens);
    check_true(query.execution_closed); check_false(recursion.proxy.active); check_null(recursion.frontier);
  }
  it("resumes the original statement with owned TEXT and BLOB parameters after their buffers change") {
    char text[]="saved"; unsigned char bytes[]={0,1,255};
    turbodb_value_t params[]={turbodb_text(text),turbodb_blob(bytes,sizeof(bytes))};
    check_equal(open_query("SELECT d.label,d.payload FROM (SELECT ? AS label,? AS payload) d",params,2,&query),TURBODB_STATUS_OK);
    memset(text,'X',sizeof(text)-1); memset(bytes,7,sizeof(bytes)); params[0]=turbodb_null(); params[1]=turbodb_null();
    const unsigned char expected[]={0,1,255};
    for(size_t round=0;round<3;++round) {
      resume_query(); const orm_sql_scan_row row=next(); text_is(row.values[0].data.text_value,"saved");
      check_equal(row.values[1].data.blob_value.size,sizeof(expected));
      check_equal(memcmp(row.values[1].data.blob_value.data,expected,sizeof(expected)),0);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
  }
  it("reopens nested streaming derived queries in dependency order") {
    seed(); check_equal(open_query("SELECT d.n FROM (SELECT e.id AS n FROM (SELECT id FROM items) e) d ORDER BY d.n",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t ast=budget.used.value[ORM_SQL_BUDGET_AST_NODES];
    check_equal(next().values[0].data.int64_value,1);
    for(size_t round=0;round<3;++round) {
      resume_query(); const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
      for(int64_t n=1;n<=3;++n) check_equal(next().values[0].data.int64_value,n);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads+3);
      check_equal(budget.used.value[ORM_SQL_BUDGET_AST_NODES],ast); check_equal(owner.active_sources,1u);
    }
  }
  it("rewinds outer CTE readers without reopening derived producers owned by the shared cache") {
    seed(); check_equal(open_query("WITH c AS (SELECT d.id FROM (SELECT id FROM items) d) "
        "SELECT a.id FROM c a JOIN (SELECT id FROM c) b ON a.id=b.id ORDER BY a.id",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t before=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    for(size_t round=0;round<3;++round) {
      if(round) resume_query();
      for(int64_t n=1;n<=3;++n) check_equal(next().values[0].data.int64_value,n);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],before+3);
    }
  }
  it("keeps scalar caches across rounds while reopening sibling derived table scans") {
    seed(); check_equal(open_query("SELECT d.id,(SELECT MAX(n.id) FROM (SELECT id FROM items) n) AS hi "
        "FROM (SELECT id FROM items) d ORDER BY d.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(size_t round=0;round<3;++round) {
      if(round) resume_query(); const uint64_t before=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
      for(int64_t n=1;n<=3;++n) {
        const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,n); check_equal(row.values[1].data.int64_value,3);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],before+(round?3:6));
    }
  }
  it("retains IN and EXISTS caches and their nested derived sources on resume") {
    seed(); check_equal(open_query("SELECT 2 IN (SELECT d.id FROM (SELECT id FROM items) d) AS present,"
        "EXISTS(SELECT e.id FROM (SELECT id FROM items) e) AS any_row",NULL,0,&query),TURBODB_STATUS_OK);
    orm_sql_scan_row row=next(); check_true(row.values[0].data.boolean_value); check_true(row.values[1].data.boolean_value);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    for(size_t round=0;round<2;++round) {
      resume_query(); row=next(); check_true(row.values[0].data.boolean_value); check_true(row.values[1].data.boolean_value);
      check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    }
  }
  it("keeps unused CTE dependencies lazy across cancellation and zero-limit rounds") {
    seed(); check_equal(open_query("WITH c AS (SELECT id+9223372036854775807 AS n FROM items) "
        "SELECT d.n FROM (SELECT n FROM c) d LIMIT 0",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_OK);
    for(size_t round=0;round<3;++round) {
      resume_query(); check_equal(next().state,ORM_SQL_SCAN_DONE);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    }
  }
  it("restores derived UNION ordering and pagination from the original parameter snapshot") {
    seed(); turbodb_value_t params[]={turbodb_i64(2),turbodb_i64(2),turbodb_i64(9),turbodb_i64(2),turbodb_i64(1)};
    check_equal(open_query("SELECT d.id FROM ((SELECT id FROM items WHERE id>=? ORDER BY id DESC LIMIT ?) "
        "UNION ALL SELECT ?) d ORDER BY d.id DESC LIMIT ? OFFSET ?",params,sizeof(params)/sizeof(params[0]),&query),TURBODB_STATUS_OK);
    memset(params,0,sizeof(params));
    for(size_t round=0;round<2;++round) {
      resume_query(); check_equal(next().values[0].data.int64_value,3);
      check_equal(next().values[0].data.int64_value,2); check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
  }
  it("rejects replacement parameters for an owned dependency graph and requires execution close") {
    check_equal(orm_sql_runtime_execution_resume(NULL,&error),TURBODB_STATUS_INVALID_STATE);
    const turbodb_value_t param=turbodb_i64(7);
    check_equal(open_query("SELECT (SELECT ?) AS n",&param,1,&query),TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_execution_resume(&query,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_execution_open(&query,&param,1,NULL,&error),TURBODB_STATUS_UNSUPPORTED);
    check_equal(orm_sql_runtime_execution_resume(&query,&error),TURBODB_STATUS_OK); check_equal(next().values[0].data.int64_value,7);
  }
  it("preserves first dependency failures when execution is closed and resumed") {
    const char *sql[]={"SELECT d.n FROM (SELECT 9223372036854775807+1 AS n) d",
        "WITH c AS (SELECT 9223372036854775807+1 AS n) SELECT n FROM c",
        "SELECT (SELECT 9223372036854775807+1) AS n"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_OK); orm_sql_scan_row row={0};
      const turbodb_status_t status=orm_tidesdb_sql_runtime_next(&query,&row,&error); check_not_equal(status,TURBODB_STATUS_OK);
      const turbodb_error_t cause=error;
      check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
      check_equal(orm_sql_runtime_execution_resume(&query,&error),status);
      check_equal(strcmp(error.message,cause.message),0); close_query(&query);
    }
  }
  it("cleans up every intercepted dependency reopen allocation failure") {
    seed(); const char *sql="WITH c AS (SELECT id FROM items) SELECT d.id FROM (SELECT a.id FROM c a "
        "JOIN (SELECT id FROM items) b ON a.id=b.id) d ORDER BY d.id";
    check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK); check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
    reserves=resizes=0; check_equal(orm_sql_runtime_execution_resume(&query,&error),TURBODB_STATUS_OK);
    const size_t allocations[]={reserves,resizes}; close_query(&query);
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
      next_statement(); check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(orm_sql_runtime_execution_resume(&query,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      fail_reserve=fail_resize=0;
      check_true(query.execution_closed); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      check_equal(orm_sql_runtime_execution_resume(&query,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      close_query(&query); check_equal(owner.active_sources,0u);
    }
  }
  it("refunds every dependency resume step failure without resetting shared caches") {
    seed(); const char *sql="WITH c AS (SELECT id FROM items) SELECT d.id FROM (SELECT id FROM c) d ORDER BY d.id";
    check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK); check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_sql_runtime_execution_resume(&query,&error),TURBODB_STATUS_OK);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start; close_query(&query);
    for(uint64_t point=0;point<steps;++point) {
      next_statement(); check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      check_equal(orm_sql_runtime_execution_resume(&query,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_true(query.execution_closed); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=LIMIT; close_query(&query);
    }
  }
  it("keeps materialized CTE caches intact when resuming the consumer exceeds its step budget") {
    seed(); check_equal(open_query("WITH c AS (SELECT id FROM items) SELECT d.id FROM (SELECT id FROM c) d",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,1);
    check_equal(orm_sql_runtime_execution_close(&query,&error),TURBODB_STATUS_OK);
    const uint64_t rows=budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]; check_greater(rows,0u);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_sql_runtime_execution_resume(&query,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_true(query.execution_closed); check_false(owner.failed);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],rows);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=LIMIT;
    check_equal(orm_sql_runtime_execution_resume(&query,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    close_query(&query); check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
  }
  it("resumes derived scans in the original transaction after a competing commit") {
    seed(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_OK); begin();
    check_equal(open_query("SELECT d.id,(SELECT MAX(id) FROM items) AS hi FROM (SELECT id FROM items) d ORDER BY d.id",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,1);
    orm_sql_catalog_store writer={0};
    check_equal(orm_tidesdb_sql_catalog_begin(database,family,MAX_RECORD,&budget,&writer,&error),TURBODB_STATUS_OK);
    sqlparser_document *doc=parse("INSERT INTO items(id,score) VALUES(4,40)"); size_t affected=0;
    check_equal(orm_tidesdb_sql_runtime_execute(doc,&writer,NULL,0,DEPTH,0,false,NULL,&affected,&error),TURBODB_STATUS_OK);
    sqlparser_document_destroy(doc); check_equal(affected,1u);
    check_equal(orm_tidesdb_sql_catalog_finish(&writer,true,&error),TURBODB_STATUS_OK);
    resume_query();
    for(int64_t n=1;n<=3;++n) {
      const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,n);
      check_equal(row.values[1].data.int64_value,3);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_BUSY);
  }
  it("resumes all UNION branches over one cached CTE chain with saved root pagination") {
    seed(); turbodb_value_t params[]={turbodb_i64(3),turbodb_i64(1)};
    check_equal(open_query("WITH c AS (SELECT id FROM items), e AS (SELECT id FROM c) "
        "SELECT id FROM e UNION ALL SELECT d.id FROM (SELECT id FROM e) d ORDER BY id DESC LIMIT ? OFFSET ?",
        params,sizeof(params)/sizeof(params[0]),&query),TURBODB_STATUS_OK);
    memset(params,0,sizeof(params)); const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    const int64_t expected[]={3,2,2};
    for(size_t round=0;round<3;++round) {
      if(round) resume_query();
      for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) check_equal(next().values[0].data.int64_value,expected[i]);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads+3);
    }
  }
  it("returns a single constant row without any Catalog reads and owns parameter payloads") {
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS], bytes=budget.used.value[ORM_SQL_BUDGET_READ_BYTES];
    char text[]="owned"; turbodb_value_t params[]={turbodb_i64(7),turbodb_text(text),turbodb_null()};
    check_equal(open_query("SELECT 1, ?, ? AS label, NULL AS absent, TRUE AS flag",params,3,&query),TURBODB_STATUS_SQL_ERROR);
    check_equal(open_query("SELECT 1, ?, ? AS label, NULL AS absent, TRUE AS flag",params,2,&query),TURBODB_STATUS_OK);
    text[0]='X'; params[0]=turbodb_i64(99);
    check_equal(query.columns,5u); orm_sql_schema_column column;
    check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK); text_is(column.name,"1");
    check_equal(orm_tidesdb_sql_runtime_column(&query,1,&column,&error),TURBODB_STATUS_OK); text_is(column.name,"?");
    orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
    check_equal(row.values[0].data.int64_value,1); check_equal(row.values[1].data.int64_value,7);
    text_is(row.values[2].data.text_value,"owned"); check_equal(row.values[3].kind,TURBODB_VALUE_NULL);
    check_true(row.values[4].data.boolean_value); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_BYTES],bytes);
  }
  it("applies filtering sorting distinct and pagination to the one row input") {
    const char *empty[]={"SELECT 1 WHERE FALSE","SELECT 1 WHERE NULL","SELECT 1 LIMIT 0",
      "SELECT 1 LIMIT 1 OFFSET 1","SELECT DISTINCT 1 ORDER BY 1 LIMIT 1 OFFSET 1"};
    for(size_t i=0;i<sizeof(empty)/sizeof(empty[0]);++i) {
      check_equal(open_query(empty[i],NULL,0,&query),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    }
    check_equal(open_query("SELECT DISTINCT 1+1 ORDER BY 1 DESC LIMIT 1",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,2); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("aggregates the unit row and preserves empty global aggregate semantics") {
    check_equal(open_query("SELECT COUNT(*) AS n,COUNT(NULL) AS z,MIN(7) AS lo",NULL,0,&query),TURBODB_STATUS_OK);
    orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,1); check_equal(row.values[1].data.int64_value,0);
    check_equal(row.values[2].data.int64_value,7); check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT COUNT(*) AS n,MAX(7) AS hi WHERE FALSE",NULL,0,&query),TURBODB_STATUS_OK);
    row=next(); check_equal(row.values[0].data.int64_value,0); check_equal(row.values[1].kind,TURBODB_VALUE_NULL); close_query(&query);
    check_equal(open_query("SELECT 7 AS unit,COUNT(*) AS n GROUP BY unit HAVING n=1 ORDER BY unit",NULL,0,&query),TURBODB_STATUS_OK);
    row=next(); check_equal(row.values[0].data.int64_value,7); check_equal(row.values[1].data.int64_value,1);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT 7 AS n WHERE FALSE GROUP BY n",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("composes constant branches with UNION derived tables and named CTE columns") {
    check_equal(open_query("SELECT 2 UNION SELECT 1 UNION ALL SELECT 2 ORDER BY 1",NULL,0,&query),TURBODB_STATUS_OK);
    const int64_t expected[]={1,2,2};
    for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) check_equal(next().values[0].data.int64_value,expected[i]);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    const turbodb_value_t params[]={turbodb_i64(4),turbodb_i64(9)};
    check_equal(open_query("WITH c(x) AS (SELECT ? UNION ALL SELECT ?) SELECT a.x,b.x AS y FROM c a JOIN (SELECT 9 AS x) b ON a.x=b.x",params,2,&query),TURBODB_STATUS_OK);
    orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,9); check_equal(row.values[1].data.int64_value,9);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("supports scalar IN and EXISTS unit subqueries including ignored projections") {
    check_equal(open_query("SELECT (SELECT 7) AS n,2 IN (SELECT 2) AS yes,EXISTS(SELECT 9223372036854775807+1) AS present,NOT EXISTS(SELECT 1 WHERE FALSE) AS absent",NULL,0,&query),TURBODB_STATUS_OK);
    orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,7);
    for(size_t i=1;i<4;++i) check_true(row.values[i].data.boolean_value);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT EXISTS(SELECT COUNT(9223372036854775807+1)) AS yes",NULL,0,&query),TURBODB_STATUS_OK);
    check_true(next().values[0].data.boolean_value); close_query(&query);
    check_equal(open_query("SELECT (SELECT 1 WHERE FALSE) AS n",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].kind,TURBODB_VALUE_NULL); close_query(&query);
    seed(); check_equal(open_query("SELECT id FROM items WHERE id IN (SELECT 2)",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,2); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("never exposes the unit witness through names stars predicates or aggregate arguments") {
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *invalid[]={"SELECT unit","SELECT *","SELECT unit.*","SELECT unit+1 AS n",
      "SELECT 1 WHERE unit","SELECT COUNT(unit) AS n","SELECT 1 AS n ORDER BY unit",
      "SELECT COUNT(*) AS n HAVING unit","SELECT 1 AS n GROUP BY unit"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
      check_equal(open_query(invalid[i],NULL,0,&query),TURBODB_STATUS_SQL_ERROR); check_null(query.owner);
      check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    check_equal(open_query("SELECT 1 AS unit ORDER BY unit",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,1);
  }
  it("keeps the unit transaction lease through EOF cancellation and evaluation failures") {
    const char *sql[]={"SELECT 1","SELECT 1","SELECT 9223372036854775807+1"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_OK); check_equal(owner.active_sources,1u);
      if(!i) { check_equal(next().state,ORM_SQL_SCAN_ROW); check_equal(next().state,ORM_SQL_SCAN_DONE); }
      else if(i==1) { check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_CANCELLED); }
      else { orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); }
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_BUSY);
      close_query(&query); check_equal(owner.active_sources,0u); check_false(owner.failed);
    }
  }
  it("explains constant queries without evaluating expressions or inventing a table") {
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(open_query("EXPLAIN SELECT 9223372036854775807+1",NULL,0,&query),TURBODB_STATUS_OK);
    orm_sql_schema_column column; check_equal(orm_tidesdb_sql_runtime_column(&query,2,&column,&error),TURBODB_STATUS_OK); check_true(column.type.nullable);
    orm_sql_scan_row row=next(); check_equal(row.values[2].kind,TURBODB_VALUE_NULL); check_equal(row.values[4].kind,TURBODB_VALUE_NULL);
    text_is(row.values[11].data.text_value,"No tables used"); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_BUSY);
    close_query(&query); seed();
    check_equal(open_query("EXPLAIN SELECT id FROM items UNION ALL SELECT 1",NULL,0,&query),TURBODB_STATUS_OK);
    text_is(next().values[2].data.text_value,"items"); row=next(); check_equal(row.values[2].kind,TURBODB_VALUE_NULL);
    text_is(row.values[11].data.text_value,"No tables used"); check_equal(next().state,ORM_SQL_SCAN_ROW);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("EXPLAIN SELECT (SELECT 1) AS n FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    text_is(next().values[2].data.text_value,"items"); check_equal(next().values[2].kind,TURBODB_VALUE_NULL);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("bounds automatic output labels and unit source execution without leaking leases") {
    enum { PREFIX_BYTES=sizeof("SELECT '")-1, SUFFIX_BYTES=sizeof("'") };
    char sql[PREFIX_BYTES+ORM_SQL_SELECT_NAME_BYTES+SUFFIX_BYTES];
    memcpy(sql,"SELECT '",PREFIX_BYTES); memset(sql+PREFIX_BYTES,'a',ORM_SQL_SELECT_NAME_BYTES);
    memcpy(sql+PREFIX_BYTES+ORM_SQL_SELECT_NAME_BYTES,"'",SUFFIX_BYTES);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(query.owner);
    check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    check_equal(open_query("SELECT 1",NULL,0,&query),TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    orm_sql_scan_row row={.state=ORM_SQL_SCAN_CANCELLED,.count=99};
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u);
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    close_query(&query); check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=LIMIT;
    owner.active_sources=SIZE_MAX;
    check_equal(open_query("SELECT 1",NULL,0,&query),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(query.owner);
    check_equal(owner.active_sources,SIZE_MAX); owner.active_sources=0;
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("executes DDL and DML then queries the same transaction and persists on explicit commit") {
    seed(); size_t affected = 99; const turbodb_value_t params[] = {turbodb_i64(7), turbodb_i64(2)};
    check_equal(execute("UPDATE items SET score=score+? WHERE id=?", params, 2, &affected), TURBODB_STATUS_OK); check_equal(affected, 1u);
    check_equal(execute("DELETE FROM items WHERE id=1", NULL, 0, &affected), TURBODB_STATUS_OK); check_equal(affected, 1u);
    for (size_t pass = 0; pass < 2; ++pass) {
      check_equal(open_query("SELECT i.id, i.score AS points FROM `items` AS i", NULL, 0, &query), TURBODB_STATUS_OK);
      check_equal(query.columns, 2u); orm_sql_schema_column column = {0};
      check_equal(orm_tidesdb_sql_runtime_column(&query, 1, &column, &error), TURBODB_STATUS_OK);
      text_is(column.name, "points"); check_equal(column.type.kind, TURBODB_VALUE_INT64); check_true(column.type.nullable);
      orm_sql_scan_row row = next(); check_equal(row.state, ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value, 2);
      check_equal(row.values[1].data.int64_value, 27);
      row = next(); check_equal(row.values[0].data.int64_value, 3); check_equal(row.values[1].kind, TURBODB_VALUE_NULL);
      check_equal(next().state, ORM_SQL_SCAN_DONE); close_query(&query);
      if (!pass) { check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK); reopen(); }
    }
  }
  it("rolls back CREATE and INSERT together without an implicit DDL commit") {
    seed(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK); reopen();
    check_equal(open_query("SHOW TABLES", NULL, 0, &query), TURBODB_STATUS_OK);
    check_equal(next().state, ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT * FROM items", NULL, 0, &query), TURBODB_STATUS_SQL_ERROR); check_null(query.owner);
    seed(); check_equal(open_query("SELECT id FROM items LIMIT 1", NULL, 0, &query), TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value, 1);
  }
  it("shares SELECT and SHOW metadata and iteration contracts") {
    seed(); check_equal(open_query("SHOW FULL TABLES", NULL, 0, &query), TURBODB_STATUS_OK);
    check_equal(query.columns, 2u); orm_sql_schema_column column = {0};
    check_equal(orm_tidesdb_sql_runtime_column(&query, 0, &column, &error), TURBODB_STATUS_OK); text_is(column.name, "Tables_in_app");
    orm_sql_scan_row row = next(); check_equal(row.state, ORM_SQL_SCAN_ROW); text_is(row.values[0].data.text_value, "items");
    text_is(row.values[1].data.text_value, "BASE TABLE"); check_equal(next().state, ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SHOW FIELDS FROM items", NULL, 0, &query), TURBODB_STATUS_OK); check_equal(query.columns, 6u);
    check_equal(orm_tidesdb_sql_runtime_column(&query, 4, &column, &error), TURBODB_STATUS_OK); text_is(column.name, "Default");
    check_true(column.type.nullable); row = next(); text_is(row.values[0].data.text_value, "id");
    text_is(row.values[3].data.text_value, "PRI"); check_equal(row.values[4].kind, TURBODB_VALUE_NULL);
    row = next(); text_is(row.values[0].data.text_value, "score"); check_equal(next().state, ORM_SQL_SCAN_DONE);
  }
  it("publishes the complete typed SHOW INDEX schema before pulling rows") {
    seed(); check_equal(open_query("SHOW INDEX FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    static const char *const names[]={"Table","Non_unique","Key_name","Seq_in_index","Column_name",
      "Collation","Cardinality","Sub_part","Packed","Null","Index_type","Comment","Index_comment","Visible","Expression"};
    check_equal(query.columns,sizeof(names)/sizeof(names[0]));
    for(size_t i=0;i<query.columns;++i) {
      orm_sql_schema_column column={0};
      check_equal(orm_tidesdb_sql_runtime_column(&query,i,&column,&error),TURBODB_STATUS_OK); text_is(column.name,names[i]);
      check_equal(column.type.kind,(i==1 || i==3 || i==6 || i==7)?TURBODB_VALUE_INT64:TURBODB_VALUE_TEXT);
      check_equal(column.type.nullable,i==6 || i==7 || i==8 || i==14);
    }
    orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.count,query.columns);
    text_is(row.values[2].data.text_value,"PRIMARY"); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("owns AST and parameter payloads including source-ordered projection WHERE LIMIT and OFFSET") {
    seed(); char tag[] = "kept";
    turbodb_value_t params[] = {turbodb_text(tag), turbodb_i64(1), turbodb_i64(1), turbodb_i64(1)};
    check_equal(open_query("SELECT ? AS tag, a.id FROM items a WHERE a.id>=? LIMIT ? OFFSET ?", params, 4, &query), TURBODB_STATUS_OK);
    memset(tag, 'x', sizeof(tag) - 1); for (size_t i = 0; i < 4; ++i) params[i] = turbodb_null();
    orm_sql_scan_row row = next(); check_equal(row.state, ORM_SQL_SCAN_ROW);
    text_is(row.values[0].data.text_value, "kept"); check_equal(row.values[1].data.int64_value, 2);
    check_equal(next().state, ORM_SQL_SCAN_DONE);
  }
  it("keeps SELECT and SHOW leases until close even after EOF and cancellation") {
    seed(); const char *queries[] = {"SELECT id FROM items LIMIT 0", "SHOW TABLES", "SHOW INDEX FROM items", "SHOW COLUMNS FROM items", "SHOW CREATE TABLE items"};
    for (size_t i = 0; i < sizeof(queries)/sizeof(queries[0]); ++i) {
      check_equal(open_query(queries[i], NULL, 0, &query), TURBODB_STATUS_OK);
      if (i) check_equal(orm_tidesdb_sql_runtime_cancel(&query, &error), TURBODB_STATUS_OK);
      check_equal(next().state, i ? ORM_SQL_SCAN_CANCELLED : ORM_SQL_SCAN_DONE);
      size_t affected = 99;
      check_equal(execute("UPDATE items SET score=30", NULL, 0, &affected), TURBODB_STATUS_BUSY); check_equal(affected, 99u);
      check_equal(execute("CREATE TABLE more (id BIGINT PRIMARY KEY)", NULL, 0, &affected), TURBODB_STATUS_BUSY);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_BUSY);
      close_query(&query); check_equal(owner.active_sources, 0u);
      check_equal(execute("UPDATE items SET score=30 WHERE id=1", NULL, 0, &affected), TURBODB_STATUS_OK);
    }
  }
  it("keeps simultaneous independent queries pinned without replacing an occupied output") {
    seed(); check_equal(open_query("SELECT id FROM items", NULL, 0, &query), TURBODB_STATUS_OK);
    check_equal(open_query("SHOW TABLES", NULL, 0, &other), TURBODB_STATUS_OK);
    const size_t sources = owner.active_sources; const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(open_query("SHOW COLUMNS FROM items", NULL, 0, &query), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(owner.active_sources, sources); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
    check_equal(next().values[0].data.int64_value, 1); close_query(&other);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_BUSY);
    check_equal(next().values[0].data.int64_value, 2); close_query(&query); check_equal(owner.active_sources, 0u);
  }
  it("rejects command/query routing mistakes and unsupported transaction statements without side effects") {
    const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], writes = budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS];
    const char *commands[] = {"SELECT * FROM items", "SHOW TABLES", "BEGIN", "COMMIT", "ROLLBACK", "SET autocommit=1", "DROP TEMPORARY TABLE items",
      "CREATE TABLE batch (id BIGINT PRIMARY KEY); INSERT INTO batch(id) VALUES(1)"};
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
      size_t affected = 99; check_equal(execute(commands[i], NULL, 0, &affected), TURBODB_STATUS_UNSUPPORTED); check_equal(affected, 99u);
    }
    const char *queries[] = {ddl, "INSERT INTO items(id) VALUES(1)", "UPDATE items SET score=1", "DELETE FROM items", "SHOW TABLES; SHOW TABLES"};
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); ++i) {
      check_equal(open_query(queries[i], NULL, 0, &query), TURBODB_STATUS_UNSUPPORTED); check_null(query.owner);
    }
    check_equal(owner.active_sources, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS], writes);
  }
  it("preserves affected on command errors and lets a valid statement follow a constraint failure") {
    seed(); size_t affected = 99;
    check_equal(execute(ddl, NULL, 0, &affected), TURBODB_STATUS_CONSTRAINT); check_equal(affected, 99u);
    check_equal(execute("INSERT INTO items(id,score) VALUES(4,40),(1,99)", NULL, 0, &affected), TURBODB_STATUS_CONSTRAINT);
    check_equal(affected, 99u); check_false(owner.failed);
    check_equal(execute("CREATE TABLE IF NOT EXISTS items (id BIGINT PRIMARY KEY)", NULL, 0, &affected), TURBODB_STATUS_OK); check_equal(affected, 0u);
    check_equal(execute("INSERT INTO items(id,score) VALUES(4,40)", NULL, 0, &affected), TURBODB_STATUS_OK); check_equal(affected, 1u);
    check_equal(open_query("SELECT score FROM items WHERE id=1", NULL, 0, &query), TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value, 10);
  }
  it("uses nonrecursive CTEs and scalar query dependencies in UPDATE and DELETE") {
    seed(); size_t affected = 99;
    const turbodb_value_t update[] = {turbodb_i64(2), turbodb_i64(5)};
    const turbodb_status_t update_status = execute(
        "WITH c(k) AS (SELECT id FROM items WHERE id>=?) "
        "UPDATE items SET score=id*10+? WHERE id IN (SELECT k FROM c)",
        update, 2, &affected);
    check_equal(update_status, TURBODB_STATUS_OK);
    check_equal(affected, 2u);
    const turbodb_value_t scalar[] = {turbodb_i64(77), turbodb_i64(1)};
    check_equal(execute("WITH c(v) AS (SELECT ?) UPDATE items "
        "SET score=(SELECT v FROM c) WHERE id=?", scalar, 2, &affected),
        TURBODB_STATUS_OK);
    check_equal(affected, 1u);
    const turbodb_value_t deletion = turbodb_i64(25);
    check_equal(execute("WITH c(k) AS (SELECT id FROM items WHERE score=?) "
        "DELETE FROM items WHERE id IN (SELECT k FROM c)", &deletion, 1,
        &affected), TURBODB_STATUS_OK);
    check_equal(affected, 1u);
    const char recursive[] = "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL "
        "SELECT n+1 FROM c WHERE n<2) UPDATE items SET score=score+1 "
        "WHERE id IN (SELECT n FROM c)";
    check_equal(execute(recursive, NULL, 0, &affected),
        TURBODB_STATUS_UNSUPPORTED);
    check_equal(execute_iterations(recursive, NULL, 0, 3, &affected),
        TURBODB_STATUS_OK);
    check_equal(affected, 1u);
    check_equal(open_query("SELECT id,score FROM items ORDER BY id", NULL, 0,
        &query), TURBODB_STATUS_OK);
    orm_sql_scan_row row = next();
    check_equal(row.values[0].data.int64_value, 1);
    check_equal(row.values[1].data.int64_value, 78);
    row = next(); check_equal(row.values[0].data.int64_value, 3);
    check_equal(row.values[1].data.int64_value, 35);
    check_equal(next().state, ORM_SQL_SCAN_DONE);
  }
  it("rebinds direct correlated dependencies for UPDATE and DELETE rows") {
    seed(); size_t affected = 99;
    check_equal(execute("UPDATE items SET score=(SELECT id*100) "
        "WHERE EXISTS(SELECT 1 WHERE id=2)", NULL, 0, &affected),
        TURBODB_STATUS_OK);
    check_equal(affected, 1u);
    check_equal(execute("DELETE FROM items WHERE items.id IN "
        "(SELECT items.id WHERE items.id=3)", NULL, 0, &affected),
        TURBODB_STATUS_OK);
    check_equal(affected, 1u);
    check_equal(open_query("SELECT id,score FROM items ORDER BY id", NULL, 0,
        &query), TURBODB_STATUS_OK);
    orm_sql_scan_row row = next();
    check_equal(row.values[0].data.int64_value, 1);
    check_equal(row.values[1].data.int64_value, 10);
    row = next();
    check_equal(row.values[0].data.int64_value, 2);
    check_equal(row.values[1].data.int64_value, 200);
    check_equal(next().state, ORM_SQL_SCAN_DONE);
  }
  it("uses write target aliases as the only visible table qualifier") {
    seed(); size_t affected = 99;
    check_equal(execute("UPDATE items AS i SET score=(SELECT i.id*100) "
        "WHERE i.id>=2 ORDER BY i.id ASC LIMIT 1", NULL, 0, &affected),
        TURBODB_STATUS_OK);
    check_equal(affected, 1u);
    check_equal(execute("UPDATE items AS i SET score=1 WHERE items.id=1",
        NULL, 0, &affected), TURBODB_STATUS_SQL_ERROR);
    check_equal(execute("DELETE FROM items target WHERE EXISTS"
        "(SELECT 1 WHERE target.id=3)", NULL, 0, &affected), TURBODB_STATUS_OK);
    check_equal(affected, 1u);
    check_equal(open_query("SELECT id,score FROM items ORDER BY id", NULL, 0,
        &query), TURBODB_STATUS_OK);
    orm_sql_scan_row row = next();
    check_equal(row.values[0].data.int64_value, 1);
    check_equal(row.values[1].data.int64_value, 10);
    row = next();
    check_equal(row.values[0].data.int64_value, 2);
    check_equal(row.values[1].data.int64_value, 200);
    check_equal(next().state, ORM_SQL_SCAN_DONE);
  }
  it("releases partial SELECT and SHOW queries on binding and parameter failures") {
    seed(); const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *invalid[] = {"SELECT absent FROM items", "SELECT id FROM absent", "SELECT id FROM items WHERE id=?", "SHOW COLUMNS FROM absent"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      check_equal(open_query(invalid[i], NULL, 0, &query), TURBODB_STATUS_SQL_ERROR);
      check_null(query.owner); check_equal(query.kind, ORM_SQL_QUERY_CLOSED);
      check_equal(owner.active_sources, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
    }
    const turbodb_value_t bad_limit = turbodb_i64(-1);
    check_equal(open_query("SELECT id FROM items LIMIT ?", &bad_limit, 1, &query), TURBODB_STATUS_TYPE_ERROR);
    check_equal(open_query("SHOW TABLES", &bad_limit, 1, &query), TURBODB_STATUS_SQL_ERROR);
    size_t affected = 99; check_equal(execute(ddl, &bad_limit, 1, &affected), TURBODB_STATUS_SQL_ERROR); check_equal(affected, 99u);
    check_equal(owner.active_sources, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
  }
  it("runs independent self-join cursors with source-ordered copied parameters") {
    seed(); turbodb_value_t params[]={turbodb_i64(10),turbodb_i64(1),turbodb_i64(0),turbodb_i64(2)};
    check_equal(open_query("SELECT a.id+? AS id,b.score FROM items a JOIN items b ON a.id+?=b.id "
      "WHERE a.id>? ORDER BY id LIMIT ?",params,4,&query),TURBODB_STATUS_OK);
    check_equal(owner.active_sources,2u);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    for(size_t i=0;i<4;++i) params[i]=turbodb_null();
    orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,11); check_equal(row.values[1].data.int64_value,20);
    row=next(); check_equal(row.values[0].data.int64_value,12); check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS]-reads,6u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_JOIN_PAIRS],9u);
    close_query(&query); check_equal(owner.active_sources,0u);
  }
  it("binds direct correlated dependencies to joined outer rows") {
    seed();
    check_equal(open_query("SELECT a.id,(SELECT a.id+b.id) AS total FROM items a JOIN items b "
        "ON EXISTS(SELECT 1 WHERE a.id=b.id) ORDER BY a.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id*2);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    size_t affected=0;
    check_equal(execute("CREATE TABLE extra(id BIGINT PRIMARY KEY)",NULL,0,&affected),TURBODB_STATUS_OK);
    check_equal(execute("INSERT INTO extra(id) VALUES(2),(4)",NULL,0,&affected),TURBODB_STATUS_OK);
    check_equal(open_query("SELECT a.id,(SELECT b.id) AS bid FROM items a LEFT JOIN extra b "
        "ON a.id=b.id ORDER BY a.id",NULL,0,&query),TURBODB_STATUS_OK);
    orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,1);
    check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
    row=next(); check_equal(row.values[0].data.int64_value,2);
    check_equal(row.values[1].data.int64_value,2);
    row=next(); check_equal(row.values[0].data.int64_value,3);
    check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("SELECT a.id FROM items a JOIN items b ON a.id=b.id "
        "WHERE EXISTS(SELECT 1 FROM items c WHERE c.id=a.id AND b.id=c.id) "
        "ORDER BY a.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) check_equal(next().values[0].data.int64_value,id);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("reopens joined sources for each correlated subquery row") {
    seed();
    const turbodb_value_t parameters[]={turbodb_i64(0),turbodb_i64(0)};
    check_equal(open_query("SELECT o.id,(SELECT COUNT(*) FROM items x JOIN items y "
        "ON x.id+?=y.id AND y.id=o.id) AS n FROM items o WHERE o.id>? ORDER BY o.id",
        parameters,2,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,1);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("SELECT o.id FROM items o WHERE o.id IN "
        "(SELECT x.id FROM items x LEFT JOIN items y ON x.id=y.id AND y.id=o.id) "
        "ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id)
      check_equal(next().values[0].data.int64_value,id);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("SELECT o.id,(SELECT COUNT(*) FROM items o JOIN items y "
        "ON o.id=y.id) AS n FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,3);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT o.id,(SELECT COUNT(*) FROM items o JOIN items y "
        "ON o.missing=y.id) AS n FROM items o",NULL,0,&query),TURBODB_STATUS_SQL_ERROR);
    check_null(query.owner); check_equal(owner.active_sources,0u);
  }
  it("reopens direct correlated compound and derived subqueries for each outer row") {
    seed();
    const turbodb_value_t parameters[]={turbodb_i64(10),turbodb_i64(20)};
    check_equal(open_query("SELECT o.id,(SELECT o.id+? UNION SELECT o.id+? "
        "ORDER BY 1 LIMIT 1) AS n FROM items o ORDER BY o.id",parameters,2,&query),
        TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id+10);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("SELECT o.id FROM items o WHERE o.id IN "
        "(SELECT o.id UNION ALL SELECT -1) ORDER BY o.id",NULL,0,&query),
        TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id)
      check_equal(next().values[0].data.int64_value,id);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("SELECT o.id FROM items o WHERE EXISTS"
        "(SELECT 1 WHERE o.id=2 UNION ALL SELECT 1 WHERE o.id=3) ORDER BY o.id",
        NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,2);
    check_equal(next().values[0].data.int64_value,3);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("SELECT o.id,(SELECT x.id FROM items x WHERE x.id=o.id "
        "UNION ALL SELECT -1 LIMIT 1) AS n FROM items o ORDER BY o.id",NULL,0,&query),
        TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("SELECT o.id,(SELECT d.x FROM (SELECT o.id AS x) d) AS n "
        "FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("SELECT o.id,(SELECT o.id+d.x FROM (SELECT 10 AS x) d) AS n "
        "FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id+10);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("SELECT o.id,(SELECT d.x FROM "
        "(SELECT e.x FROM (SELECT o.id AS x) e) d) AS n FROM items o ORDER BY o.id",
        NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("SELECT o.id,(SELECT d.x FROM "
        "(SELECT o.id AS x UNION SELECT o.id) d JOIN items i ON i.id=d.x) AS n "
        "FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
  }
  it("rebuilds correlated CTE stores and rewinds outer CTE readers per row") {
    seed();
    check_equal(open_query("SELECT o.id,(WITH c AS (SELECT o.id AS x) "
        "SELECT x FROM c) AS n FROM items o ORDER BY o.id",NULL,0,&query),
        TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("WITH outer_rows AS (SELECT id FROM items) "
        "SELECT o.id,(SELECT o.id) AS n FROM outer_rows o ORDER BY o.id",
        NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("WITH outer_rows AS (SELECT id FROM items) "
        "SELECT o.id FROM outer_rows o WHERE o.id IN (SELECT o.id) "
        "AND EXISTS(SELECT 1 WHERE o.id>1) ORDER BY o.id",NULL,0,&query),
        TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,2);
    check_equal(next().values[0].data.int64_value,3);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("WITH outer_rows AS (SELECT id FROM items) "
        "SELECT o.id,(SELECT o.id+i.id) AS n FROM outer_rows o "
        "JOIN items i ON o.id=i.id ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id*2);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("WITH outer_rows AS (SELECT id FROM items) "
        "SELECT o.id FROM outer_rows o JOIN items i ON o.id=i.id "
        "WHERE o.id IN (SELECT i.id WHERE i.id=o.id) "
        "AND EXISTS(SELECT 1 WHERE o.id>1) ORDER BY o.id",NULL,0,&query),
        TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,2);
    check_equal(next().values[0].data.int64_value,3);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("WITH outer_rows AS (SELECT id FROM items) "
        "SELECT a.id,(SELECT a.id+b.id) AS n FROM outer_rows a "
        "JOIN outer_rows b ON a.id=b.id ORDER BY a.id",NULL,0,&query),
        TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id*2);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("SELECT o.id,(WITH c AS (SELECT o.id AS x) "
        "SELECT a.x FROM c a JOIN c b ON a.x=b.x) AS n "
        "FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("WITH c AS (SELECT id FROM items) "
        "SELECT o.id,(SELECT c.id FROM c WHERE c.id=o.id) AS n "
        "FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("SELECT o.id,(WITH c(x) AS "
        "(SELECT o.id UNION SELECT -1) SELECT MAX(x) FROM c) AS n "
        "FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("SELECT o.id,(WITH c AS "
        "(SELECT id FROM items WHERE id=1) SELECT id FROM c) AS n "
        "FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,1);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("SELECT o.id FROM items o WHERE o.id IN "
        "(WITH c AS (SELECT o.id AS x) SELECT x FROM c) AND EXISTS"
        "(WITH c AS (SELECT o.id AS x) SELECT 1 FROM c WHERE x>1) "
        "ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,2);
    check_equal(next().values[0].data.int64_value,3);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("SELECT o.id,(SELECT d.x FROM "
        "(WITH c AS (SELECT id) SELECT id AS x FROM c) d) AS n "
        "FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
  }
  it("captures parent and grandparent rows in nested scalar queries") {
    seed();
    const turbodb_status_t status=open_query("SELECT o.id,(SELECT "
        "(SELECT i.id+o.id) FROM items i WHERE i.id=2) AS n "
        "FROM items o ORDER BY o.id",NULL,0,&query);
    check_equal(status,TURBODB_STATUS_OK);
    if(status==TURBODB_STATUS_OK) {
      for(int64_t id=1;id<=3;++id) {
        const orm_sql_scan_row row=next();
        check_equal(row.values[0].data.int64_value,id);
        check_equal(row.values[1].data.int64_value,id+2);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
  }
  it("captures three active lexical frames and preserves SQL parameter slots") {
    seed(); const turbodb_value_t parameter=turbodb_i64(7);
    check_equal(open_query("SELECT o.id,(SELECT (SELECT "
        "(SELECT i.id+j.id+o.id+?) FROM items j WHERE j.id=1) "
        "FROM items i WHERE i.id=2) AS n FROM items o ORDER BY o.id",
        &parameter,1,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id+10);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT (SELECT (SELECT i.id) FROM items i "
        "WHERE i.id=2) AS n",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,2);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("resolves nested outer names in the nearest lexical frame") {
    seed();
    const char *queries[]={
      "SELECT o.id,(SELECT (SELECT id) FROM items i WHERE i.id=2) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(SELECT (SELECT o.id) FROM items o WHERE o.id=2) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(SELECT (SELECT i.id FROM items i WHERE i.id=2) FROM items i WHERE i.id=1) AS n FROM items o ORDER BY o.id"
    };
    for(size_t i=0;i<sizeof(queries)/sizeof(queries[0]);++i) {
      check_equal(open_query(queries[i],NULL,0,&query),TURBODB_STATUS_OK);
      for(int64_t id=1;id<=3;++id) {
        const orm_sql_scan_row row=next();
        check_equal(row.values[0].data.int64_value,id);
        check_equal(row.values[1].data.int64_value,2);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    }
    size_t affected=0;
    check_equal(execute("CREATE TABLE keys_only(k BIGINT PRIMARY KEY)",NULL,0,&affected),TURBODB_STATUS_OK);
    check_equal(open_query("SELECT o.id,(SELECT (SELECT o.id) FROM keys_only o) "
        "AS n FROM items o",NULL,0,&query),TURBODB_STATUS_SQL_ERROR);
    check_true(strstr(error.message,"unknown column")!=NULL);
    check_equal(open_query("SELECT o.id,(SELECT (SELECT id) FROM items i "
        "JOIN items j ON i.id=j.id) AS n FROM items o",NULL,0,&query),TURBODB_STATUS_SQL_ERROR);
    check_true(strstr(error.message,"ambiguous")!=NULL);
  }
  it("evaluates nested IN and EXISTS against both lexical rows") {
    seed();
    const char *queries[]={
      "SELECT o.id FROM items o WHERE EXISTS(SELECT 1 FROM items i WHERE i.id=o.id AND i.id IN (SELECT o.id WHERE i.id>1)) ORDER BY o.id",
      "SELECT o.id FROM items o WHERE o.id IN (SELECT i.id FROM items i WHERE EXISTS(SELECT 1 WHERE i.id=o.id AND o.id>1)) ORDER BY o.id",
      "SELECT o.id FROM items o WHERE NOT EXISTS(SELECT 1 FROM items i WHERE i.id=o.id AND NOT EXISTS(SELECT 1 WHERE i.id=o.id AND o.id>1)) ORDER BY o.id"
    };
    for(size_t i=0;i<sizeof(queries)/sizeof(queries[0]);++i) {
      check_equal(open_query(queries[i],NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(next().values[0].data.int64_value,2);
      check_equal(next().values[0].data.int64_value,3);
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    }
  }
  it("retains nested byte results and propagates NULL from parent rows") {
    seed();
    turbodb_value_t parameter=turbodb_text("captured");
    check_equal(open_query("SELECT o.id,(SELECT (SELECT ? WHERE i.id=o.id) "
        "FROM items i WHERE i.id=o.id) AS label FROM items o ORDER BY o.id",
        &parameter,1,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      text_is(row.values[1].data.text_value,"captured");
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT o.id,(SELECT (SELECT i.score) FROM items i "
        "WHERE i.id=o.id) AS n FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      if(id==3) check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
      else check_equal(row.values[1].data.int64_value,id*10);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("captures nested frames through JOIN derived CTE and UNION inputs") {
    seed();
    const struct { const char *sql; int64_t shift; } cases[]={
      {"SELECT o.id,(SELECT (SELECT i.id+j.id+o.id) FROM items i JOIN items j ON i.id=j.id WHERE i.id=2) AS n FROM items o ORDER BY o.id",4},
      {"SELECT o.id,(SELECT (SELECT i.id+o.id) FROM (SELECT id FROM items) i WHERE i.id=2) AS n FROM items o ORDER BY o.id",2},
      {"SELECT o.id,(WITH c AS (SELECT id FROM items) SELECT (SELECT i.id+o.id) FROM c i WHERE i.id=2) AS n FROM items o ORDER BY o.id",2},
      {"SELECT o.id,(SELECT (SELECT d.x FROM (SELECT i.id+o.id AS x) d) FROM items i WHERE i.id=2) AS n FROM items o ORDER BY o.id",2},
      {"SELECT o.id,(SELECT (SELECT i.id+o.id) FROM items i WHERE i.id=2 UNION SELECT (SELECT j.id+o.id) FROM items j WHERE j.id=2) AS n FROM items o ORDER BY o.id",2}
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
      const turbodb_status_t status=open_query(cases[i].sql,NULL,0,&query);
      check_equal(status,TURBODB_STATUS_OK);
      for(int64_t id=1;id<=3;++id) {
        const orm_sql_scan_row row=next();
        check_equal(row.values[0].data.int64_value,id);
        check_equal(row.values[1].data.int64_value,id+cases[i].shift);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    }
  }
  it("captures local and outer rows across query definitions") {
    seed();
    const char *queries[]={
      "SELECT o.id,(SELECT d.x FROM (SELECT (SELECT i.id+o.id) AS x FROM items i WHERE i.id=2) d) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(WITH c AS (SELECT (SELECT i.id+o.id) AS x FROM items i WHERE i.id=2) SELECT x FROM c) AS n FROM items o ORDER BY o.id",
      "SELECT d.id,d.x FROM (SELECT i.id,(SELECT i.id+2) AS x FROM items i) d ORDER BY d.id",
      "WITH c AS (SELECT i.id,(SELECT i.id+2) AS x FROM items i) SELECT id,x FROM c ORDER BY id",
      "SELECT o.id,(SELECT d.x FROM (SELECT e.x FROM (SELECT (SELECT i.id+o.id) AS x FROM items i WHERE i.id=2) e) d) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(SELECT d.x FROM (SELECT (SELECT i.id+o.id) AS x FROM items i WHERE i.id=2 GROUP BY i.id) d) AS n FROM items o GROUP BY o.id ORDER BY o.id",
      "SELECT o.id,(SELECT d.x FROM (SELECT (SELECT i.id+o.id) AS x FROM items i WHERE i.id=2 UNION SELECT (SELECT j.id+o.id) AS x FROM items j WHERE j.id=2) d) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(WITH c AS (SELECT (SELECT i.id+o.id) AS x FROM items i WHERE i.id=2) SELECT (SELECT x FROM c)) AS n FROM items o ORDER BY o.id"
    };
    for(size_t i=0;i<sizeof(queries)/sizeof(queries[0]);++i) {
      const turbodb_status_t status=open_query(queries[i],NULL,0,&query);
      check_equal(status,TURBODB_STATUS_OK);
      if(status==TURBODB_STATUS_OK) {
        for(int64_t id=1;id<=3;++id) {
          const orm_sql_scan_row row=next();
          check_equal(row.values[0].data.int64_value,id);
          check_equal(row.values[1].data.int64_value,id+2);
        }
        check_equal(next().state,ORM_SQL_SCAN_DONE);
      }
      close_query(&query);
    }
  }
  it("captures through dependent CTE definitions and nested IN EXISTS") {
    seed();
    check_equal(open_query("SELECT o.id,(WITH c AS (SELECT (SELECT i.id+o.id) AS x "
        "FROM items i WHERE i.id=2), d AS (SELECT (SELECT c.x+o.id) AS y FROM c) "
        "SELECT (SELECT y FROM d)) AS n FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id+id+2);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    const char *queries[]={
      "SELECT o.id FROM items o WHERE EXISTS(SELECT d.x FROM (SELECT i.id AS x FROM items i WHERE EXISTS(SELECT 1 WHERE i.id=o.id AND o.id>1)) d) ORDER BY o.id",
      "SELECT o.id FROM items o WHERE o.id IN (WITH c AS (SELECT i.id AS x FROM items i WHERE i.id IN (SELECT o.id WHERE i.id>1)) SELECT x FROM c) ORDER BY o.id"
    };
    for(size_t i=0;i<sizeof(queries)/sizeof(queries[0]);++i) {
      check_equal(open_query(queries[i],NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(next().values[0].data.int64_value,2);
      check_equal(next().values[0].data.int64_value,3);
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    }
  }
  it("keeps nested definition readers attached to the correct materialization") {
    seed();
    const struct { const char *sql; int64_t shift; } cases[]={
      {"SELECT o.id,(WITH c AS (SELECT (SELECT o.id+2) AS x) SELECT (SELECT a.x FROM c a JOIN c b ON a.x=b.x)) AS n FROM items o ORDER BY o.id",2},
      {"SELECT o.id,(WITH c AS (SELECT (SELECT o.id+2) AS x) SELECT (SELECT (SELECT x FROM c))) AS n FROM items o ORDER BY o.id",2},
      {"SELECT o.id,(WITH c AS (SELECT (SELECT o.id+2) AS x) SELECT (SELECT x FROM c) FROM items o WHERE o.id=2) AS n FROM items o ORDER BY o.id",2},
      {"WITH c AS (SELECT 2 AS x) SELECT o.id,(SELECT (SELECT x FROM c)+o.id) AS n FROM items o ORDER BY o.id",2},
      {"SELECT o.id,(WITH c AS (SELECT 2 AS x) SELECT (SELECT x FROM c)+o.id) AS n FROM items o ORDER BY o.id",2},
      {"SELECT o.id,(WITH c AS (SELECT (SELECT o.id+2) AS x) SELECT (WITH c AS (SELECT (SELECT o.id+3) AS x) SELECT (SELECT x FROM c))) AS n FROM items o ORDER BY o.id",3},
      {"SELECT o.id,(SELECT d.x+o.id FROM (SELECT (SELECT i.id) AS x FROM items i WHERE i.id=2) d) AS n FROM items o ORDER BY o.id",2}
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
      check_equal(open_query(cases[i].sql,NULL,0,&query),TURBODB_STATUS_OK);
      for(int64_t id=1;id<=3;++id) {
        const orm_sql_scan_row row=next();
        check_equal(row.values[0].data.int64_value,id);
        check_equal(row.values[1].data.int64_value,id+cases[i].shift);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    }
  }
  it("reopens definition captures with the original statement parameters") {
    seed(); const turbodb_value_t parameter=turbodb_i64(7);
    const char *queries[]={
      "SELECT o.id,(WITH c AS (SELECT (SELECT o.id+?) AS x) SELECT (SELECT x FROM c)) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(WITH c AS (SELECT ? AS x) SELECT (SELECT x FROM c)+o.id) AS n FROM items o ORDER BY o.id"
    };
    for(size_t i=0;i<sizeof(queries)/sizeof(queries[0]);++i) {
      check_equal(open_query(queries[i],&parameter,1,&query),TURBODB_STATUS_OK);
      check_equal(next().values[1].data.int64_value,8);
      resume_query();
      for(int64_t id=1;id<=3;++id) {
        const orm_sql_scan_row row=next();
        check_equal(row.values[0].data.int64_value,id);
        check_equal(row.values[1].data.int64_value,id+7);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    }
    check_equal(owner.active_sources,0u);
  }
  it("retains markers TEXT and NULL across query definitions") {
    seed();
    char label[]="definition";
    const turbodb_value_t parameters[]={turbodb_text(label),turbodb_i64(2)};
    check_equal(open_query("SELECT o.id,(WITH c AS (SELECT (SELECT ? WHERE i.id=o.id) AS x "
        "FROM items i WHERE i.id=?) SELECT (SELECT x FROM c)) AS n FROM items o",
        parameters,sizeof(parameters)/sizeof(parameters[0]),&query),TURBODB_STATUS_OK);
    memset(label,'x',sizeof(label)-1);
    vstr retained={0};
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,id);
      if(id==2) {
        check_equal(row.values[1].kind,TURBODB_VALUE_TEXT);
        retained=row.values[1].data.text_value; text_is(retained,"definition");
      } else check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); text_is(retained,"definition"); close_query(&query);
    check_equal(open_query("SELECT o.id,(SELECT d.x FROM (SELECT (SELECT o.score) AS x) d) AS n "
        "FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      if(id==3) check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
      else check_equal(row.values[1].data.int64_value,id*10);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("uses definition captures in writes and EXPLAIN without reading rows") {
    seed(); size_t affected=0;
    check_equal(execute("UPDATE items o SET score=(WITH c AS (SELECT (SELECT i.id+o.id) AS x "
        "FROM items i WHERE i.id=2) SELECT (SELECT x FROM c))",NULL,0,&affected),TURBODB_STATUS_OK);
    check_equal(affected,3u);
    check_equal(open_query("SELECT id,score FROM items ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) check_equal(next().values[1].data.int64_value,id+2);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("EXPLAIN SELECT o.id,(WITH c AS (SELECT (SELECT i.id+o.id) AS x "
        "FROM items i WHERE i.id=2) SELECT (SELECT x FROM c)) AS n FROM items o",
        NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    while(next().state==ORM_SQL_SCAN_ROW) {}
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); close_query(&query);
    check_equal(execute("DELETE FROM items o WHERE EXISTS(SELECT d.x FROM "
        "(SELECT i.id AS x FROM items i WHERE EXISTS(SELECT 1 WHERE i.id=o.id AND o.id=2)) d)",
        NULL,0,&affected),TURBODB_STATUS_OK);
    check_equal(affected,1u);
  }
  it("keeps non-LATERAL definition rows isolated from sibling inputs") {
    seed(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *queries[]={
      "SELECT i.id,d.x FROM items i JOIN (SELECT (SELECT i.id) AS x) d ON i.id>0",
      "SELECT o.id,(SELECT d.x FROM items j JOIN (SELECT (SELECT i.id+j.id+o.id) AS x FROM items i WHERE i.id=1) d ON j.id=1) AS n FROM items o",
      "WITH c AS (SELECT (SELECT o.id) AS x) SELECT c.x FROM items o JOIN c ON o.id>0"
    };
    for(size_t i=0;i<sizeof(queries)/sizeof(queries[0]);++i) {
      check_equal(open_query(queries[i],NULL,0,&query),TURBODB_STATUS_SQL_ERROR);
      check_contains(error.message,"unknown");
      check_null(query.owner); check_equal(owner.active_sources,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("executes lateral native aggregation and grouped parent paging through raw runtime") {
    seed();
    check_equal(open_query("SELECT d.n AS n,COUNT(*) AS hits FROM items a JOIN LATERAL "
        "(SELECT b.id AS n FROM items b WHERE b.id<=a.id) d ON TRUE "
        "GROUP BY d.n HAVING COUNT(*)>1 ORDER BY hits DESC LIMIT 1 OFFSET 1",NULL,0,&query),TURBODB_STATUS_OK);
    orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
    check_equal(row.values[0].data.int64_value,2); check_equal(row.values[1].data.int64_value,2);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT d.n FROM items a,LATERAL (SELECT MAX(a.id) AS n) d ORDER BY a.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) check_equal(next().values[0].data.int64_value,id);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("owns lateral TEXT marker snapshots and ancestor byte captures through statement replay") {
    seed(); char text[]="lateral-tag"; turbodb_value_t parameter=turbodb_text(text);
    check_equal(open_query("SELECT d.tag,d.id FROM (SELECT id,? AS tag FROM items) a,LATERAL "
        "(SELECT (SELECT a.tag) AS tag,a.id AS id) d ORDER BY d.id",&parameter,1,&query),TURBODB_STATUS_OK);
    memset(text,'x',sizeof(text)-1); parameter=turbodb_null();
    for(size_t pass=0;pass<2;++pass) {
      if(pass) resume_query();
      for(int64_t id=1;id<=3;++id) {
        const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
        check_equal(row.values[0].kind,TURBODB_VALUE_TEXT); text_is(row.values[0].data.text_value,"lateral-tag");
        check_equal(row.values[1].data.int64_value,id);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
  }
  it("rejects illegal lateral scope and join directions before retaining any runtime lease") {
    seed(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *sql[]={
      "SELECT d.n FROM LATERAL (SELECT a.id AS n) d,items a",
      "SELECT d.n FROM items a RIGHT JOIN LATERAL (SELECT a.id AS n) d ON TRUE",
      "SELECT d.n FROM LATERAL (SELECT a.id AS n) d LEFT JOIN items a ON TRUE",
      "SELECT d.n FROM items a,(SELECT a.id AS n) d",
      "SELECT d.n FROM LATERAL (SELECT d.n AS n) d",
      "EXPLAIN SELECT d.n FROM LATERAL (SELECT missing.id AS n) d"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_SQL_ERROR);
      check_null(query.owner); check_equal(owner.active_sources,0u); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("skips lateral evaluation on an empty native prefix and latches a later arithmetic failure") {
    size_t affected=0; check_equal(execute(ddl,NULL,0,&affected),TURBODB_STATUS_OK);
    const char sql[]="SELECT d.n FROM items a,LATERAL (SELECT a.id+9223372036854775807 AS n) d";
    check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(execute("INSERT INTO items VALUES(0,10),(1,20)",NULL,0,&affected),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK);
    const orm_sql_scan_row first=next(); check_equal(first.state,ORM_SQL_SCAN_ROW); check_equal(first.values[0].data.int64_value,INT64_MAX);
    orm_sql_scan_row row={.state=ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(row.state,ORM_SQL_SCAN_CANCELLED); const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps); close_query(&query);
    check_equal(owner.active_sources,0u); check_false(owner.failed);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("uses lateral nested query dependencies in UPDATE and DELETE before the write phase") {
    seed(); size_t affected=0;
    check_equal(execute("UPDATE items o SET score=(SELECT d.n FROM LATERAL (SELECT o.id+10 AS n) d)",NULL,0,&affected),TURBODB_STATUS_OK);
    check_equal(affected,3u); check_equal(open_query("SELECT id,score FROM items ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) check_equal(next().values[1].data.int64_value,id+10);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(execute("DELETE FROM items o WHERE EXISTS(SELECT 1 FROM LATERAL (SELECT o.id AS n WHERE o.id=2) d)",NULL,0,&affected),TURBODB_STATUS_OK);
    check_equal(affected,1u); check_equal(owner.active_sources,0u); check_false(owner.failed);
  }
  it("uses nested lexical captures in writes and binds EXPLAIN without row reads") {
    seed(); size_t affected=0;
    check_equal(execute("UPDATE items o SET score=(SELECT (SELECT o.id+i.id) "
        "FROM items i WHERE i.id=2)",NULL,0,&affected),TURBODB_STATUS_OK);
    check_equal(affected,3u);
    check_equal(open_query("SELECT id,score FROM items ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next(); check_equal(row.values[1].data.int64_value,id+2);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("EXPLAIN SELECT o.id,(SELECT (SELECT i.id+o.id) "
        "FROM items i WHERE i.id=2) AS n FROM items o",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    while(next().state==ORM_SQL_SCAN_ROW) {}
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); close_query(&query);
    check_equal(execute("DELETE FROM items AS o WHERE EXISTS(SELECT 1 FROM items i "
        "WHERE i.id=o.id AND EXISTS(SELECT 1 WHERE o.id=i.id AND i.id=2))",
        NULL,0,&affected),TURBODB_STATUS_OK);
    check_equal(affected,1u);
  }
  it("releases nested lexical executions at every allocation and step failure") {
    seed(); budget.limits.transaction.read_bytes=UINT64_MAX;
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *cases[]={
      "SELECT o.id,(SELECT (SELECT i.id+o.id) FROM items i WHERE i.id=o.id) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(SELECT (SELECT i.id+o.id) FROM items i WHERE i.id=o.id GROUP BY i.id) AS n FROM items o GROUP BY o.id ORDER BY o.id",
      "SELECT o.id,(WITH c AS (SELECT (SELECT i.id+o.id) AS x FROM items i WHERE i.id=o.id) SELECT (SELECT x FROM c)) AS n FROM items o ORDER BY o.id"
    };
    for(size_t scenario=0;scenario<sizeof(cases)/sizeof(cases[0]);++scenario) {
      const char *sql=cases[scenario]; next_statement();
      check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK);
      reserves=resizes=0;
      const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(next().state,ORM_SQL_SCAN_ROW);
      const size_t allocations[]={reserves,resizes};
      const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
      close_query(&query);
      for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
        next_statement(); check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK);
        reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
        orm_sql_scan_row row={.state=ORM_SQL_SCAN_CANCELLED,.count=99};
        check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_OUT_OF_MEMORY);
        check_equal(row.count,99u);
        fail_reserve=fail_resize=0; close_query(&query);
        check_equal(owner.active_sources,0u); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      for(uint64_t point=0;point<steps;++point) {
        next_statement(); check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK);
        budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=
            budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
        orm_sql_scan_row row={.state=ORM_SQL_SCAN_CANCELLED,.count=99};
        check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
        check_equal(row.count,99u);
        budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=LIMIT;
        close_query(&query); check_equal(owner.active_sources,0u); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
    }
  }
  it("captures a grouped parent key and a grandparent row") {
    seed();
    check_equal(open_query("SELECT o.id,(SELECT (SELECT i.id+o.id) FROM items i "
        "GROUP BY i.id ORDER BY i.id LIMIT 1) AS n FROM items o ORDER BY o.id",
        NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id+1);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("EXPLAIN SELECT o.id,(SELECT (SELECT i.id+o.id) FROM items i "
        "GROUP BY i.id LIMIT 1) AS n FROM items o",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    while(next().state==ORM_SQL_SCAN_ROW) {}
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
  }
  it("maps grouped captures to reordered repeated aliased and ordinal keys") {
    seed(); const turbodb_value_t parameter=turbodb_i64(7);
    const char *queries[]={
      "SELECT o.id,(SELECT o.id+?) AS n FROM items o GROUP BY o.id ORDER BY o.id",
      "SELECT o.id,(SELECT o.id+?) AS n FROM items o GROUP BY o.score,o.id ORDER BY o.id",
      "SELECT o.id,(SELECT o.id+?) AS n FROM items o GROUP BY o.id,o.id ORDER BY o.id",
      "SELECT o.id AS k,(SELECT o.id+?) AS n FROM items o GROUP BY k ORDER BY k",
      "SELECT o.id,(SELECT o.id+?) AS n FROM items o GROUP BY 1 ORDER BY o.id",
      "SELECT o.id,(SELECT o.id+?) AS n FROM (SELECT id FROM items) o GROUP BY o.id ORDER BY o.id",
      "WITH c AS (SELECT id FROM items) SELECT o.id,(SELECT o.id+?) AS n FROM c o GROUP BY o.id ORDER BY o.id",
      "SELECT o.id,(SELECT d.x FROM (SELECT o.id+? AS x) d) AS n FROM items o GROUP BY o.id ORDER BY o.id"
    };
    for(size_t i=0;i<sizeof(queries)/sizeof(queries[0]);++i) {
      const turbodb_status_t status=open_query(queries[i],&parameter,1,&query);
      check_equal(status,TURBODB_STATUS_OK);
      if(status==TURBODB_STATUS_OK) {
        for(int64_t id=1;id<=3;++id) {
          const orm_sql_scan_row row=next();
          check_equal(row.values[0].data.int64_value,id);
          check_equal(row.values[1].data.int64_value,id+7);
        }
        check_equal(next().state,ORM_SQL_SCAN_DONE);
      }
      close_query(&query);
    }
  }
  it("rejects ungrouped captures including references in descendant queries") {
    seed(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *queries[]={
      "SELECT COUNT(*) AS n,(SELECT o.id) AS bad FROM items o",
      "SELECT o.id,(SELECT o.score) AS bad FROM items o GROUP BY o.id",
      "SELECT o.id,(SELECT (SELECT o.score+i.id) FROM items i WHERE i.id=1) AS bad FROM items o GROUP BY o.id",
      "SELECT o.id+1 AS k,(SELECT o.id) AS bad FROM items o GROUP BY o.id+1",
      "EXPLAIN SELECT o.id,(SELECT o.score) AS bad FROM items o GROUP BY o.id",
      "SELECT o.id,(WITH c AS (SELECT (SELECT o.score) AS x) SELECT (SELECT x FROM c)) AS bad FROM items o GROUP BY o.id",
      "SELECT o.id,(SELECT d.x FROM (SELECT (SELECT i.score+o.id) AS x FROM items i GROUP BY i.id) d) AS bad FROM items o",
      "EXPLAIN SELECT o.id,(WITH c AS (SELECT (SELECT o.score) AS x) SELECT (SELECT x FROM c)) AS bad FROM items o GROUP BY o.id"
    };
    for(size_t i=0;i<sizeof(queries)/sizeof(queries[0]);++i) {
      check_equal(open_query(queries[i],NULL,0,&query),TURBODB_STATUS_SQL_ERROR);
      check_contains(error.message,"not a GROUP BY key");
      check_null(query.owner); check_equal(owner.active_sources,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("uses grouped captures in HAVING ORDER BY IN and EXISTS") {
    seed();
    const char *queries[]={
      "SELECT o.id,COUNT(*) AS n FROM items o GROUP BY o.id HAVING EXISTS(SELECT 1 WHERE o.id>1) ORDER BY (SELECT o.id) DESC",
      "SELECT o.id,COUNT(*) AS n FROM items o GROUP BY o.id HAVING o.id IN (SELECT o.id WHERE o.id>1) ORDER BY o.id DESC",
      "SELECT o.id,COUNT(*) AS n FROM items o GROUP BY o.id HAVING NOT EXISTS(SELECT 1 WHERE o.id=1) ORDER BY o.id DESC"
    };
    for(size_t i=0;i<sizeof(queries)/sizeof(queries[0]);++i) {
      const turbodb_status_t status=open_query(queries[i],NULL,0,&query);
      check_equal(status,TURBODB_STATUS_OK);
      if(status==TURBODB_STATUS_OK) {
        for(int64_t id=3;id>=2;--id) {
          const orm_sql_scan_row row=next();
          check_equal(row.values[0].data.int64_value,id);
          check_equal(row.values[1].data.int64_value,1);
        }
        check_equal(next().state,ORM_SQL_SCAN_DONE);
      }
      close_query(&query);
    }
  }
  it("keeps original rows for pre-group query captures") {
    seed();
    const char *queries[]={
      "SELECT COUNT(*) AS n FROM items o WHERE EXISTS(SELECT 1 WHERE o.id>1)",
      "SELECT COUNT((SELECT o.id)) AS n FROM items o WHERE o.id>1",
      "SELECT COUNT(*) AS n FROM items o WHERE o.id>1 GROUP BY (SELECT 1 WHERE o.id>1)"
    };
    for(size_t i=0;i<sizeof(queries)/sizeof(queries[0]);++i) {
      const turbodb_status_t status=open_query(queries[i],NULL,0,&query);
      check_equal(status,TURBODB_STATUS_OK);
      if(status==TURBODB_STATUS_OK) {
        check_equal(next().values[0].data.int64_value,2);
        check_equal(next().state,ORM_SQL_SCAN_DONE);
      }
      close_query(&query);
    }
  }
  it("captures nullable and joined group keys and global aggregate ancestors") {
    seed(); size_t affected=0;
    check_equal(execute("INSERT INTO items(id,score) VALUES(4,20)",NULL,0,&affected),TURBODB_STATUS_OK);
    check_equal(open_query("SELECT o.score AS k,(SELECT o.score) AS n,COUNT(*) AS c "
        "FROM items o GROUP BY o.score ORDER BY k",NULL,0,&query),TURBODB_STATUS_OK);
    const orm_sql_scan_row null_group=next();
    check_equal(null_group.values[0].kind,TURBODB_VALUE_NULL);
    check_equal(null_group.values[1].kind,TURBODB_VALUE_NULL);
    check_equal(null_group.values[2].data.int64_value,1);
    for(int64_t key=10;key<=20;key+=10) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,key);
      check_equal(row.values[1].data.int64_value,key);
      check_equal(row.values[2].data.int64_value,key/10);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT a.id,(SELECT a.id+b.id) AS n FROM items a "
        "JOIN items b ON a.id=b.id GROUP BY b.id,a.id ORDER BY a.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=4;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id+id);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT o.id,(SELECT (SELECT o.id)+COUNT(*) FROM items i) AS n "
        "FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=4;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id+4);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT o.id,(SELECT o.id FROM items i GROUP BY o.id) AS n "
        "FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=4;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("resolves unqualified outer columns after inner FROM lookup") {
    seed(); size_t affected=0;
    check_equal(execute("CREATE TABLE keys_only(k BIGINT PRIMARY KEY)",NULL,0,&affected),
        TURBODB_STATUS_OK);
    check_equal(execute("INSERT INTO keys_only(k) VALUES(1)",NULL,0,&affected),
        TURBODB_STATUS_OK);
    check_equal(open_query("SELECT o.id,(SELECT id FROM keys_only WHERE k=1) AS copied "
        "FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("SELECT o.id,(SELECT id FROM keys_only x JOIN keys_only y "
        "ON x.k=y.k LIMIT 1) AS copied FROM items o ORDER BY o.id",NULL,0,&query),
        TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);

    check_equal(open_query("SELECT o.id,(SELECT id FROM items i WHERE i.id=1) AS local_id "
        "FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,1);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("executes LEFT and RIGHT joins with logical metadata null extension and WHERE after ON") {
    seed(); size_t affected=0;
    check_equal(execute("CREATE TABLE extra(id BIGINT PRIMARY KEY,score BIGINT)",NULL,0,&affected),TURBODB_STATUS_OK);
    check_equal(execute("INSERT INTO extra(id,score) VALUES(2,200),(4,400)",NULL,0,&affected),TURBODB_STATUS_OK);
    check_equal(open_query("SELECT a.id AS aid,b.id AS bid FROM items a LEFT JOIN extra b ON a.id=b.id ORDER BY aid",
      NULL,0,&query),TURBODB_STATUS_OK);
    orm_sql_schema_column col;
    check_equal(orm_tidesdb_sql_runtime_column(&query,1,&col,&error),TURBODB_STATUS_OK); check_true(col.type.nullable);
    orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,1); check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
    row=next(); check_equal(row.values[0].data.int64_value,2); check_equal(row.values[1].data.int64_value,2);
    row=next(); check_equal(row.values[0].data.int64_value,3); check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT a.id AS aid,b.id AS bid FROM items a RIGHT JOIN extra b ON a.id=b.id ORDER BY bid",
      NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_runtime_column(&query,0,&col,&error),TURBODB_STATUS_OK); check_true(col.type.nullable);
    check_equal(orm_tidesdb_sql_runtime_column(&query,1,&col,&error),TURBODB_STATUS_OK); check_false(col.type.nullable);
    row=next(); check_equal(row.values[0].data.int64_value,2); check_equal(row.values[1].data.int64_value,2);
    row=next(); check_equal(row.values[0].kind,TURBODB_VALUE_NULL); check_equal(row.values[1].data.int64_value,4);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT a.id FROM items a LEFT JOIN extra b ON a.id=b.id WHERE b.id IS NULL ORDER BY a.id",
      NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,1); check_equal(next().values[0].data.int64_value,3);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("composes mixed three-table joins and restores columns after an outer RIGHT rewrite") {
    seed(); size_t affected=0;
    check_equal(execute("CREATE TABLE extra(id BIGINT PRIMARY KEY)",NULL,0,&affected),TURBODB_STATUS_OK);
    check_equal(execute("INSERT INTO extra(id) VALUES(2),(4)",NULL,0,&affected),TURBODB_STATUS_OK);
    check_equal(open_query("SELECT a.id AS aid,b.id AS bid,c.id AS cid FROM items a LEFT JOIN extra b ON a.id=b.id "
      "RIGHT JOIN extra c ON b.id=c.id ORDER BY cid",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    orm_sql_scan_row row=next();
    for(size_t i=0;i<3;++i) check_equal(row.values[i].data.int64_value,2);
    row=next(); check_equal(row.values[0].kind,TURBODB_VALUE_NULL); check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
    check_equal(row.values[2].data.int64_value,4); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS]-reads,7u);
    close_query(&query); check_equal(owner.active_sources,0u);
  }
  it("combines CROSS JOIN with aggregation DISTINCT ordering and empty-table outer joins") {
    seed(); check_equal(open_query("SELECT COUNT(*) AS n FROM items a CROSS JOIN items b",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,9); check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT DISTINCT a.id FROM items a,items b ORDER BY a.id DESC LIMIT 2",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,3); check_equal(next().values[0].data.int64_value,2);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT a.id,COUNT(b.id) AS n FROM items a LEFT JOIN items b ON a.id+1=b.id "
      "GROUP BY a.id HAVING n>=0 ORDER BY a.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t i=1;i<=3;++i) { const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,i);
      check_equal(row.values[1].data.int64_value,i==3?0:1); }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    size_t affected=0; check_equal(execute("CREATE TABLE empty_table(id BIGINT PRIMARY KEY)",NULL,0,&affected),TURBODB_STATUS_OK);
    check_equal(open_query("SELECT a.id,b.id AS bid FROM items a LEFT JOIN empty_table b ON a.id=b.id ORDER BY a.id",
      NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t i=1;i<=3;++i) { const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,i);
      check_equal(row.values[1].kind,TURBODB_VALUE_NULL); }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("keeps all table leases while LIMIT zero and early cancellation avoid native row reads") {
    seed(); const char *sql[]={"SELECT a.id FROM items a JOIN items b ON a.id=b.id LIMIT 0",
      "SELECT a.id FROM items a RIGHT JOIN items b ON a.id=b.id"};
    for(size_t i=0;i<2;++i) {
      check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_OK);
      const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
      if(i) check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_OK);
      check_equal(next().state,i?ORM_SQL_SCAN_CANCELLED:ORM_SQL_SCAN_DONE);
      check_equal(owner.active_sources,2u); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
      for(size_t j=0;j<2;++j) check_null(((orm_sql_relation_source *)vec_at(&query.as.select.relations,j))->iterator);
      check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_BUSY);
      check_equal(orm_tidesdb_sql_from_close(&query.as.select.from_run,&error),TURBODB_STATUS_BUSY);
      check_equal(orm_tidesdb_sql_from_destroy(&query.as.select.from,&error),TURBODB_STATUS_BUSY);
      close_query(&query); check_equal(owner.active_sources,0u);
    }
  }
  it("locks JOIN pair budget failures preserves output and releases every source for the next statement") {
    seed(); check_equal(open_query("SELECT a.id FROM items a CROSS JOIN items b",NULL,0,&query),TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_JOIN_PAIRS]=1;
    check_equal(next().state,ORM_SQL_SCAN_ROW); orm_sql_scan_row row={.count=99};
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); check_false(owner.failed);
    close_query(&query); check_equal(owner.active_sources,0u); next_statement();
    budget.limits.statement.value[ORM_SQL_BUDGET_JOIN_PAIRS]=LIMIT;
    check_equal(open_query("SELECT COUNT(*) AS n FROM items a JOIN items b ON a.id=b.id",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,3);
  }
  it("rejects joined ambiguous names and incomplete Catalog sources without retaining leases") {
    seed(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *sql[]={"SELECT id FROM items a JOIN items b ON a.id=b.id",
      "SELECT a.id FROM items a JOIN absent b ON a.id=b.id",
      "SELECT a.id FROM items a JOIN items b ON id=b.id",
      "SELECT a.id FROM items a JOIN items a ON TRUE",
      "SELECT a.id FROM items a JOIN items b ON a.id=?",
      "SELECT * FROM items a JOIN items b ON TRUE",
      "SELECT a.id FROM items a,items b JOIN items c ON a.id=c.id"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_SQL_ERROR);
      check_null(query.owner); check_equal(owner.active_sources,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_false(owner.failed);
    }
  }
  it("explains each joined table in physical order without evaluating ON or pulling stored rows") {
    seed(); check_equal(open_query("EXPLAIN FORMAT=TRADITIONAL SELECT a.id AS aid,c.id AS cid FROM items a "
      "LEFT JOIN items b ON a.id=b.id RIGHT JOIN items c ON b.id+9223372036854775807=c.id ORDER BY cid",
      NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(owner.active_sources,3u); check_equal(query.as.select.from.active_runs,0u);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS]=reads;
    budget.limits.statement.value[ORM_SQL_BUDGET_JOIN_PAIRS]=0;
    const char *aliases[]={"c","a","b"};
    for(size_t i=0;i<3;++i) {
      const orm_sql_scan_row row=next(); text_is(row.values[2].data.text_value,aliases[i]);
      text_is(row.values[4].data.text_value,"ALL"); check_equal(row.values[9].kind,TURBODB_VALUE_NULL);
      check_contains(row.values[11].data.text_value.data,"RIGHT JOIN via LEFT");
      if(i) check_contains(row.values[11].data.text_value.data,"Materialized input");
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    for(size_t i=0;i<3;++i) check_null(((orm_sql_relation_source *)vec_at(&query.as.select.relations,i))->iterator);
    close_query(&query); check_equal(owner.active_sources,0u);
  }
  it("executes UNION ALL with first-branch names source-ordered parameters and global pagination") {
    seed(); turbodb_value_t params[]={turbodb_i64(10),turbodb_i64(1),turbodb_i64(20),turbodb_i64(2),turbodb_i64(2),turbodb_i64(1)};
    check_equal(open_query("SELECT id+? AS n FROM items WHERE id>=? UNION ALL SELECT id+? AS other FROM items WHERE id>=? "
      "ORDER BY n DESC LIMIT ? OFFSET ?",params,6,&query),TURBODB_STATUS_OK);
    check_equal(query.kind,ORM_SQL_QUERY_COMPOUND); check_equal(owner.active_sources,2u);
    orm_sql_schema_column column; check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK);
    text_is(column.name,"n"); for(size_t i=0;i<6;++i) params[i]=turbodb_null();
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(next().values[0].data.int64_value,22); check_equal(next().values[0].data.int64_value,13);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS]-reads,6u);
    close_query(&query); check_equal(owner.active_sources,0u);
  }
  it("deduplicates UNION tuples including NULL and respects mixed ALL DISTINCT associativity") {
    seed(); check_equal(open_query("SELECT score AS n FROM items UNION SELECT score AS other FROM items ORDER BY n",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].kind,TURBODB_VALUE_NULL); check_equal(next().values[0].data.int64_value,10);
    check_equal(next().values[0].data.int64_value,20); check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT id FROM items UNION ALL SELECT id FROM items UNION DISTINCT SELECT id FROM items ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t i=1;i<=3;++i) check_equal(next().values[0].data.int64_value,i);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT id FROM items UNION SELECT id FROM items UNION ALL SELECT id FROM items ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t i=1;i<=3;++i) { check_equal(next().values[0].data.int64_value,i); check_equal(next().values[0].data.int64_value,i); }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("keeps branch query groups and outer ORDER LIMIT independent") {
    seed(); const turbodb_value_t params[]={turbodb_i64(1),turbodb_i64(2),turbodb_i64(2)};
    check_equal(open_query("(SELECT id FROM items ORDER BY id DESC LIMIT ?) UNION ALL "
      "(SELECT id FROM items ORDER BY id LIMIT ?) ORDER BY id DESC LIMIT ?",params,3,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,3); check_equal(next().values[0].data.int64_value,2);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("(SELECT id FROM items UNION ALL SELECT id FROM items ORDER BY id DESC LIMIT 3) ORDER BY id LIMIT 1",
      NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,2); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("isolates branch aggregate discovery and combines joined grouped and ordinary branches") {
    seed(); check_equal(open_query("SELECT id AS n FROM items WHERE id=1 UNION ALL SELECT COUNT(*) AS total FROM items ORDER BY n",
      NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,1); check_equal(next().values[0].data.int64_value,3);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT COUNT(b.id) AS n FROM items a LEFT JOIN items b ON a.id+1=b.id GROUP BY a.id "
      "UNION SELECT MAX(id) AS m FROM items ORDER BY n",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,0); check_equal(next().values[0].data.int64_value,1);
    check_equal(next().values[0].data.int64_value,3); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("widens NULL-only result types and validates inactive branches for LIMIT zero") {
    seed(); check_equal(open_query("SELECT NULL AS n FROM items UNION SELECT id FROM items ORDER BY n",NULL,0,&query),TURBODB_STATUS_OK);
    orm_sql_schema_column column; check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK);
    check_equal(column.type.kind,TURBODB_VALUE_INT64); check_true(column.type.nullable);
    check_equal(next().values[0].kind,TURBODB_VALUE_NULL);
    for(int64_t i=1;i<=3;++i) check_equal(next().values[0].data.int64_value,i);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT id FROM items UNION SELECT id+9223372036854775807 AS n FROM items LIMIT 0",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_BUSY); close_query(&query);
    check_equal(open_query("SELECT id FROM items UNION SELECT missing FROM items LIMIT 0",NULL,0,&query),TURBODB_STATUS_SQL_ERROR);
    check_null(query.owner); check_equal(owner.active_sources,0u);
  }
  it("bounds UNION materialization and preserves first failure with no DISTINCT prefix") {
    seed(); const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(open_query("SELECT id FROM items UNION SELECT id FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=2;
    orm_sql_scan_row row={.count=99}; check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(row.count,99u); const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); close_query(&query);
    check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained); check_false(owner.failed);
    next_statement(); check_equal(open_query("SELECT id FROM items UNION ALL SELECT id FROM items LIMIT 1",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,1); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("rejects UNION shape type scope and parameter errors and refunds every partial branch") {
    seed(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *invalid[]={"SELECT id FROM items UNION SELECT id,score FROM items",
      "SELECT id AS first_name FROM items UNION ALL SELECT id AS second_name FROM items ORDER BY second_name",
      "SELECT id FROM items UNION SELECT id FROM absent", "SELECT id FROM items UNION SELECT ? AS n FROM items",
      "SELECT id FROM items UNION SELECT id FROM items ORDER BY items.id"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
      check_equal(open_query(invalid[i],NULL,0,&query),TURBODB_STATUS_SQL_ERROR); check_null(query.owner);
      check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    const turbodb_value_t boolean=turbodb_bool(true);
    check_equal(open_query("SELECT id FROM items UNION ALL SELECT ? AS n FROM items",&boolean,1,&query),TURBODB_STATUS_UNSUPPORTED);
    check_equal(open_query("SELECT 'a' AS n FROM items UNION SELECT 'b' AS n FROM items",NULL,0,&query),TURBODB_STATUS_UNSUPPORTED);
    const turbodb_value_t negative=turbodb_i64(-1);
    check_equal(open_query("SELECT id FROM items UNION ALL SELECT id FROM items LIMIT ?",&negative,1,&query),TURBODB_STATUS_TYPE_ERROR);
    check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_false(owner.failed);
  }
  it("copies UNION ALL byte parameters and cancels before reading any branch") {
    seed(); char first[]="first",second[]="second"; const turbodb_value_t params[]={turbodb_text(first),turbodb_text(second)};
    check_equal(open_query("SELECT ? AS label FROM items WHERE id=1 UNION ALL SELECT ? AS label FROM items WHERE id=2",params,2,&query),TURBODB_STATUS_OK);
    first[0]=second[0]='!'; text_is(next().values[0].data.text_value,"first"); text_is(next().values[0].data.text_value,"second");
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT id FROM items UNION SELECT id FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_CANCELLED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); close_query(&query); check_equal(owner.active_sources,0u);
  }
  it("bounds compound tree depth before Catalog access and keeps failed output empty") {
    seed(); sqlparser_document *doc=parse("SELECT id FROM items UNION ALL SELECT id FROM items UNION SELECT id FROM items");
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS],work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_tidesdb_sql_runtime_open(doc,&owner,vstr_from_cstr("app"),NULL,0,1,&query,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    sqlparser_document_destroy(doc); check_null(query.owner); check_equal(owner.active_sources,0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    check_false(owner.failed);
  }
  it("explains mixed UNION and joined branches in postorder without business execution") {
    seed(); const uint64_t pairs=budget.used.value[ORM_SQL_BUDGET_JOIN_PAIRS];
    const uint64_t materialized=budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
    const turbodb_value_t params[]={turbodb_i64(INT64_MAX),turbodb_i64(1),turbodb_i64(2)};
    check_equal(open_query("EXPLAIN FORMAT=TRADITIONAL SELECT a.id+? AS n FROM items a RIGHT JOIN items b ON a.id+9223372036854775807=b.id "
      "UNION ALL SELECT COUNT(*) AS total FROM items WHERE id>? UNION SELECT id FROM items ORDER BY n LIMIT ?",params,3,&query),TURBODB_STATUS_OK);
    check_equal(query.kind,ORM_SQL_QUERY_COMPOUND); check_true(query.as.compound.describe);
    check_equal(query.columns,ORM_SQL_EXPLAIN_COLUMNS); check_equal(query.as.compound.plan->active_runs,0u);
    orm_sql_schema_column column; check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK);
    text_is(column.name,"id"); check_equal(column.type.kind,TURBODB_VALUE_INT64); check_true(column.type.nullable);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS]=reads;
    budget.limits.statement.value[ORM_SQL_BUDGET_JOIN_PAIRS]=pairs;
    budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=materialized;
    const char *tables[]={"b","a","items","union_result","items","union_result"};
    const char *types[]={"PRIMARY","PRIMARY","UNION","UNION RESULT","UNION","UNION RESULT"};
    const int64_t ids[]={1,1,2,0,3,0};
    for(size_t i=0;i<sizeof(ids)/sizeof(ids[0]);++i) {
      const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
      text_is(row.values[1].data.text_value,types[i]); text_is(row.values[2].data.text_value,tables[i]);
      if(ids[i]) check_equal(row.values[0].data.int64_value,ids[i]); else check_equal(row.values[0].kind,TURBODB_VALUE_NULL);
      if(i==2) text_is(row.values[11].data.text_value,"Using where; Global aggregate");
      if(i==3) text_is(row.values[11].data.text_value,"UNION ALL");
      if(i==5) text_is(row.values[11].data.text_value,"Using temporary; Using filesort; Limit; UNION DISTINCT; Using temporary; Using filesort");
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    check_equal(budget.used.value[ORM_SQL_BUDGET_JOIN_PAIRS],pairs);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],materialized);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_BUSY);
    close_query(&query); check_equal(owner.active_sources,0u); check_false(owner.failed);
  }
  it("explains each parenthesized tail and zero limit after the AST and parameters expire") {
    seed(); turbodb_value_t params[]={turbodb_i64(1),turbodb_i64(0),turbodb_i64(2)};
    check_equal(open_query("EXPLAIN (SELECT id FROM items LIMIT ?) UNION ALL (SELECT id FROM items LIMIT ?) ORDER BY id LIMIT ?",
      params,3,&query),TURBODB_STATUS_OK);
    params[0]=params[1]=params[2]=turbodb_i64(-1);
    orm_sql_scan_row row=next(); text_is(row.values[11].data.text_value,"Limit");
    row=next(); text_is(row.values[1].data.text_value,"QUERY GROUP"); text_is(row.values[11].data.text_value,"Query group");
    row=next(); check_equal(row.values[0].data.int64_value,2); check_equal(row.values[4].kind,TURBODB_VALUE_NULL);
    check_equal(row.values[9].data.int64_value,0); text_is(row.values[11].data.text_value,"Zero limit");
    row=next(); text_is(row.values[1].data.text_value,"QUERY GROUP");
    row=next(); text_is(row.values[11].data.text_value,"Using temporary; Using filesort; Limit; UNION ALL");
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("EXPLAIN (SELECT id FROM items UNION SELECT id FROM items ORDER BY id LIMIT 1) LIMIT 0",NULL,0,&query),TURBODB_STATUS_OK);
    for(size_t i=0;i<3;++i) check_equal(next().state,ORM_SQL_SCAN_ROW);
    row=next(); text_is(row.values[1].data.text_value,"QUERY GROUP"); text_is(row.values[11].data.text_value,"Zero limit; Query group");
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("rejects invalid compound EXPLAIN plans and values before publishing any metadata") {
    seed(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *invalid[]={"EXPLAIN SELECT id FROM items UNION SELECT id,score FROM items",
      "EXPLAIN SELECT id AS a FROM items UNION SELECT id AS b FROM items ORDER BY b",
      "EXPLAIN SELECT id FROM items UNION SELECT missing FROM items LIMIT 0",
      "EXPLAIN SELECT id FROM items UNION SELECT id FROM absent",
      "EXPLAIN SELECT ? AS n FROM items UNION SELECT id FROM items"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
      check_equal(open_query(invalid[i],NULL,0,&query),TURBODB_STATUS_SQL_ERROR);
      check_null(query.owner); check_equal(owner.active_sources,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    const turbodb_value_t boolean=turbodb_bool(true),negative=turbodb_i64(-1),bad=turbodb_f64(NAN);
    check_equal(open_query("EXPLAIN SELECT id FROM items UNION ALL SELECT ? AS n FROM items",&boolean,1,&query),TURBODB_STATUS_UNSUPPORTED);
    check_equal(open_query("EXPLAIN SELECT 'a' AS n FROM items UNION SELECT 'b' AS n FROM items",NULL,0,&query),TURBODB_STATUS_UNSUPPORTED);
    check_equal(open_query("EXPLAIN SELECT id FROM items UNION ALL SELECT id FROM items LIMIT ?",&negative,1,&query),TURBODB_STATUS_TYPE_ERROR);
    const turbodb_value_t bad_params[]={bad,bad};
    check_equal(open_query("EXPLAIN SELECT ? AS n FROM items UNION ALL SELECT ? AS n FROM items",bad_params,2,&query),TURBODB_STATUS_TYPE_ERROR);
    check_equal(open_query("EXPLAIN FORMAT=JSON SELECT id FROM items UNION SELECT id FROM items",NULL,0,&query),TURBODB_STATUS_UNSUPPORTED);
    check_equal(open_query("EXPLAIN SELECT 'a' AS n FROM items UNION ALL SELECT 'b' AS n FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    for(size_t i=0;i<3;++i) check_equal(next().state,ORM_SQL_SCAN_ROW);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_false(owner.failed);
  }
  it("cancels compound EXPLAIN and locks step failures while retaining owner admission until close") {
    seed(); const char *sql="EXPLAIN SELECT id FROM items UNION ALL SELECT id FROM items";
    check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_CANCELLED);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_BUSY); close_query(&query);
    check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(next().state,ORM_SQL_SCAN_ROW);
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    orm_sql_scan_row row={.count=99};
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u);
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); close_query(&query);
    check_equal(owner.active_sources,0u); check_false(owner.failed);
  }
  it("refunds compound EXPLAIN at every execution step boundary across metadata sources") {
    seed(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *sql="EXPLAIN (SELECT a.id FROM items a LEFT JOIN items b ON a.id=b.id LIMIT 1) UNION SELECT id FROM items ORDER BY id";
    check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t before=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    while(next().state==ORM_SQL_SCAN_ROW) {}
    const uint64_t needed=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-before; close_query(&query);
    for(uint64_t step=0;step<needed;++step) {
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=LIMIT; next_statement();
      check_equal(open_query(sql,NULL,0,&query),TURBODB_STATUS_OK);
      const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+step;
      turbodb_status_t status; orm_sql_scan_row row;
      do { row=(orm_sql_scan_row){.count=99}; status=orm_tidesdb_sql_runtime_next(&query,&row,&error); }
      while(status==TURBODB_STATUS_OK && row.state==ORM_SQL_SCAN_ROW);
      check_equal(status,TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u);
      check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),status); check_equal(row.count,99u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
      close_query(&query); check_equal(owner.active_sources,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_false(owner.failed);
    }
  }
  it("evaluates cached scalar cardinality over native SELECT without changing its snapshot owner") {
    seed(); check_equal(open_query("SELECT score FROM items WHERE id=2",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t before=budget.used.value[ORM_SQL_BUDGET_READ_ROWS]; open_subquery(ORM_SQL_SUBQUERY_SCALAR,NULL);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],before);
    turbodb_value_t value=turbodb_i64(99); check_equal(orm_tidesdb_sql_subquery_eval(&subquery,NULL,&value,&error),TURBODB_STATUS_OK);
    check_equal(value.data.int64_value,20); const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS]=reads;
    check_equal(orm_tidesdb_sql_subquery_eval(&subquery,NULL,&value,&error),TURBODB_STATUS_OK);
    check_equal(value.data.int64_value,20); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_subquery_close(&subquery,&error),TURBODB_STATUS_OK); close_query(&query);
    budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS]=LIMIT;
    check_equal(open_query("SELECT id FROM items LIMIT 2",NULL,0,&query),TURBODB_STATUS_OK); open_subquery(ORM_SQL_SUBQUERY_SCALAR,NULL);
    value=turbodb_i64(99); check_equal(orm_tidesdb_sql_subquery_eval(&subquery,NULL,&value,&error),TURBODB_STATUS_SQL_ERROR);
    check_equal(value.data.int64_value,99); check_false(owner.failed);
    check_equal(orm_tidesdb_sql_subquery_close(&subquery,&error),TURBODB_STATUS_OK); close_query(&query);
    check_equal(open_query("SELECT id FROM items WHERE id=9",NULL,0,&query),TURBODB_STATUS_OK); open_subquery(ORM_SQL_SUBQUERY_SCALAR,NULL);
    check_equal(orm_tidesdb_sql_subquery_eval(&subquery,NULL,&value,&error),TURBODB_STATUS_OK); check_equal(value.kind,TURBODB_VALUE_NULL);
  }
  it("materializes native UNION membership once and evaluates a constant EXISTS witness lazily") {
    seed(); check_equal(open_query("SELECT score AS n FROM items UNION ALL SELECT id AS other FROM items WHERE id=1",NULL,0,&query),TURBODB_STATUS_OK);
    const orm_sql_type probe_type={TURBODB_VALUE_INT64,true}; open_subquery(ORM_SQL_SUBQUERY_IN,&probe_type);
    turbodb_value_t probe=turbodb_i64(1),value=turbodb_i64(99);
    check_equal(orm_tidesdb_sql_subquery_eval(&subquery,&probe,&value,&error),TURBODB_STATUS_OK); check_equal(value.data.boolean_value,1);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS]; probe=turbodb_i64(99);
    check_equal(orm_tidesdb_sql_subquery_eval(&subquery,&probe,&value,&error),TURBODB_STATUS_OK); check_equal(value.kind,TURBODB_VALUE_NULL);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    check_equal(orm_tidesdb_sql_subquery_close(&subquery,&error),TURBODB_STATUS_OK); close_query(&query);
    check_equal(open_query("SELECT TRUE AS witness FROM items WHERE id=2",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t before=budget.used.value[ORM_SQL_BUDGET_READ_ROWS]; open_subquery(ORM_SQL_SUBQUERY_EXISTS,NULL);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],before);
    check_equal(orm_tidesdb_sql_subquery_eval(&subquery,NULL,&value,&error),TURBODB_STATUS_OK); check_equal(value.data.boolean_value,1);
    check_equal(orm_tidesdb_sql_subquery_cancel(&subquery,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_subquery_close(&subquery,&error),TURBODB_STATUS_OK); close_query(&query);
    check_equal(owner.active_sources,0u); check_false(owner.failed);
  }
  it("executes a scalar subquery lazily once per query on the same Catalog snapshot") {
    seed(); next_statement(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(open_query("SELECT id,(SELECT MAX(score) FROM items) AS hi FROM items ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(query.dependencies.count,1u); check_equal(owner.active_sources,2u);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    orm_sql_schema_column column;
    check_equal(orm_tidesdb_sql_runtime_column(&query,1,&column,&error),TURBODB_STATUS_OK);
    check_equal(column.type.kind,TURBODB_VALUE_INT64); check_true(column.type.nullable);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next(); check_equal(row.count,2u);
      check_equal(row.values[0].data.int64_value,id); check_equal(row.values[1].data.int64_value,20);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS]-reads,6u);
    close_query(&query); check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("rebinds direct scalar IN and EXISTS dependencies for every outer row") {
    seed();
    check_equal(open_query("SELECT o.id,(SELECT i.score FROM items i WHERE i.id=o.id) AS own,"
        "o.id IN (SELECT i.id FROM items i WHERE i.id=o.id) AS member,"
        "EXISTS(SELECT 1 FROM items i WHERE i.id=o.id AND i.score=20) AS matched "
        "FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,id);
      if(id<3) check_equal(row.values[1].data.int64_value,id*10); else check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
      check_equal(row.values[2].data.boolean_value,1);
      check_equal(row.values[3].data.boolean_value,id==2);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("resolves unqualified outer names in direct unit subqueries") {
    seed();
    check_equal(open_query("SELECT o.id,(SELECT id+100) AS shifted "
        "FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.int64_value,id+100);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("retains correlated TEXT results through the expression consumer lifetime") {
    seed();
    check_equal(open_query("SELECT o.id,(SELECT 'matched' WHERE o.id=2) AS label "
        "FROM items o ORDER BY o.id",NULL,0,&query),TURBODB_STATUS_OK);
    orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,1);
    check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
    row=next(); check_equal(row.values[0].data.int64_value,2);
    text_is(row.values[1].data.text_value,"matched");
    row=next(); check_equal(row.values[0].data.int64_value,3);
    check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("bounds retained correlated TEXT payloads and releases them on failure") {
    seed();
    check_equal(open_query("SELECT (SELECT 'held' WHERE o.id=o.id) AS label "
        "FROM items o LIMIT 2",NULL,0,&query),TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=2;
    text_is(next().values[0].data.text_value,"held");
    orm_sql_scan_row row={.state=ORM_SQL_SCAN_CANCELLED,.count=99};
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),
        TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(row.state,ORM_SQL_SCAN_CANCELLED); check_equal(row.count,99u);
    close_query(&query);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=LIMIT;
  }
  it("opens nested scalar dependencies before consumers and retains no parser document") {
    seed();
    check_equal(open_query("SELECT id+(SELECT MAX(id)+(SELECT MIN(id) FROM items) FROM items) AS n FROM items ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(query.dependencies.count,2u); check_equal(owner.active_sources,3u);
    for(int64_t n=5;n<=7;++n) check_equal(next().values[0].data.int64_value,n);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query); check_equal(owner.active_sources,0u);
  }
  it("returns NULL for empty scalar queries and delays multirow failure until the selected branch") {
    seed();
    check_equal(open_query("SELECT (SELECT id FROM items WHERE FALSE) AS n FROM items LIMIT 1",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].kind,TURBODB_VALUE_NULL); close_query(&query);
    check_equal(open_query("SELECT CASE WHEN id=1 THEN 7 ELSE (SELECT id FROM items) END AS n FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,7);
    orm_sql_scan_row row={.state=ORM_SQL_SCAN_CANCELLED,.count=99};
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_SQL_ERROR);
    check_equal(row.state,ORM_SQL_SCAN_CANCELLED); check_equal(row.count,99u); check_contains(error.message,"scalar");
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS],steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_SQL_ERROR);
    check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
    check_false(owner.failed);
  }
  it("keeps scalar query parameter numbering and copies inner byte parameters before return") {
    seed(); const turbodb_value_t numbers[]={turbodb_i64(10),turbodb_i64(2),turbodb_i64(100)};
    check_equal(open_query("SELECT id+(SELECT id+? FROM items WHERE id=?)+? AS n FROM items ORDER BY id",numbers,3,&query),TURBODB_STATUS_OK);
    for(int64_t n=113;n<=115;++n) check_equal(next().values[0].data.int64_value,n);
    close_query(&query); char text[]="kept"; turbodb_value_t values[]={turbodb_text(text),turbodb_i64(2),turbodb_i64(2)};
    check_equal(open_query("SELECT (SELECT ? FROM items WHERE id=?) AS tag FROM items LIMIT ?",values,3,&query),TURBODB_STATUS_OK);
    memset(text,'x',sizeof(text)-1); for(size_t i=0;i<3;++i) values[i]=turbodb_null();
    text_is(next().values[0].data.text_value,"kept"); text_is(next().values[0].data.text_value,"kept");
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("supports scalar dependencies across grouping and RIGHT JOIN without inner aggregate pollution") {
    seed();
    check_equal(open_query("SELECT id+(SELECT MIN(id) FROM items) AS k,COUNT((SELECT MAX(score) FROM items)) AS n FROM items WHERE id<=(SELECT MAX(id) FROM items) GROUP BY 1 HAVING n<(SELECT COUNT(*) FROM items) ORDER BY k DESC",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t k=4;k>=2;--k) { const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,k); check_equal(row.values[1].data.uint64_value,1u); }
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT a.id AS a,b.id AS b FROM items a RIGHT JOIN items b ON a.id+(SELECT MIN(id) FROM items)=b.id ORDER BY b.id",NULL,0,&query),TURBODB_STATUS_OK);
    orm_sql_scan_row row=next(); check_equal(row.values[0].kind,TURBODB_VALUE_NULL); check_equal(row.values[1].data.int64_value,1);
    for(int64_t b=2;b<=3;++b) { row=next(); check_equal(row.values[0].data.int64_value,b-1); check_equal(row.values[1].data.int64_value,b); }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("supports UNION scalar sources and scalar calls in compound branches and tails") {
    seed();
    check_equal(open_query("SELECT (SELECT MAX(id) FROM items UNION ALL SELECT MIN(id) FROM items LIMIT 1) AS n FROM items LIMIT 1",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,3); close_query(&query);
    check_equal(open_query("SELECT (SELECT MIN(id) FROM items) AS n FROM items LIMIT 1",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,1); close_query(&query);
    check_equal(open_query("(SELECT id+(SELECT MAX(id) FROM items) AS id FROM items LIMIT 1) UNION ALL SELECT id FROM items ORDER BY id+(SELECT MIN(id) FROM items)",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=4;++id) check_equal(next().values[0].data.int64_value,id);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("validates scalar names width and correlation even in unreachable branches") {
    seed(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *sql[]={"SELECT CASE WHEN TRUE THEN 1 ELSE (SELECT id FROM absent) END AS n FROM items",
      "SELECT COALESCE(1,(SELECT missing FROM items)) AS n FROM items",
      "SELECT (SELECT id,score FROM items) AS n FROM items",
      "SELECT (SELECT * FROM items) AS n FROM items",
      "SELECT (SELECT z.id FROM items b) AS n FROM items a"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_SQL_ERROR); check_null(query.owner);
      check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_false(owner.failed);
    }
  }
  it("keeps cancelled and LIMIT zero scalar dependencies unread but pinned until close") {
    seed(); next_statement();
    const char *sql[]={"SELECT (SELECT id FROM items) AS n FROM items LIMIT 0","SELECT (SELECT id FROM items) AS n FROM items"};
    for(size_t i=0;i<2;++i) {
      check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_OK);
      const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
      if(i) check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_OK);
      check_equal(next().state,i?ORM_SQL_SCAN_CANCELLED:ORM_SQL_SCAN_DONE);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); check_equal(owner.active_sources,2u);
      size_t affected=99; check_equal(execute("DELETE FROM items",NULL,0,&affected),TURBODB_STATUS_BUSY); check_equal(affected,99u);
      close_query(&query); check_equal(owner.active_sources,0u);
    }
  }
  it("explains scalar and UNION dependencies without evaluating cardinality or arithmetic") {
    seed(); next_statement();
    check_equal(open_query("EXPLAIN SELECT (SELECT id+9223372036854775807 FROM items UNION ALL SELECT id FROM items) AS n FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(query.columns,ORM_SQL_EXPLAIN_COLUMNS); check_equal(query.dependencies.count,1u);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS]=reads;
    const char *kinds[]={"PRIMARY","SUBQUERY","UNION","UNION RESULT"};
    for(size_t i=0;i<4;++i) {
      const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW); text_is(row.values[1].data.text_value,kinds[i]);
      if(i<3) check_equal(row.values[0].data.int64_value,(int64_t)i+1); else check_equal(row.values[0].kind,TURBODB_VALUE_NULL);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u); check_false(owner.failed);
  }
  it("numbers nested scalar EXPLAIN blocks before their children and later siblings") {
    seed(); next_statement();
    check_equal(open_query("EXPLAIN SELECT (SELECT MAX(id)+(SELECT MIN(id) FROM items c) FROM items b) AS x,(SELECT MAX(id) FROM items d) AS y FROM items a",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS]=reads;
    const char *tables[]={"a","b","c","d"};
    for(size_t i=0;i<4;++i) {
      const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,(int64_t)i+1);
      text_is(row.values[1].data.text_value,i?"SUBQUERY":"PRIMARY"); text_is(row.values[2].data.text_value,tables[i]);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
  }
  it("materializes an IN source once and preserves empty and nullable NOT IN semantics") {
    seed(); check_equal(open_query("SELECT id,id IN (SELECT id FROM items WHERE id<3) AS member FROM items ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    for(int64_t id=1;id<=3;++id) { const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,id); check_equal(row.values[1].data.boolean_value,id<3); }
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS]-reads,6u); close_query(&query);
    check_equal(open_query("SELECT score NOT IN (SELECT score FROM items) AS absent,NULL IN (SELECT id FROM items WHERE FALSE) AS empty_in,NULL NOT IN (SELECT id FROM items WHERE FALSE) AS empty_not FROM items ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
    for(size_t i=0;i<3;++i) { const orm_sql_scan_row row=next(); if(i<2) check_equal(row.values[0].data.boolean_value,0); else check_equal(row.values[0].kind,TURBODB_VALUE_NULL); check_equal(row.values[1].data.boolean_value,0); check_equal(row.values[2].data.boolean_value,1); }
    close_query(&query); check_equal(open_query("SELECT 99 NOT IN (SELECT score FROM items) AS absent FROM items LIMIT 1",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].kind,TURBODB_VALUE_NULL);
  }
  it("binds nested membership dependencies and scalar probes without cross-scope names") {
    seed(); check_equal(open_query("SELECT id FROM items WHERE id IN (SELECT id FROM items WHERE id IN (SELECT MAX(id) FROM items))",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(query.dependencies.count,2u); check_equal(next().values[0].data.int64_value,3); check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT (SELECT MIN(id) FROM items) IN (SELECT id FROM items) AS member FROM items LIMIT 1",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.boolean_value,1); close_query(&query);
    check_equal(open_query("SELECT id IN (SELECT (SELECT MAX(id) FROM items) FROM items) AS member FROM items ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
    for(size_t i=0;i<3;++i) check_equal(next().values[0].data.boolean_value,i==2);
    close_query(&query);
    check_equal(open_query("SELECT id IN (SELECT (SELECT id FROM items ORDER BY id LIMIT 1) FROM items) AS member FROM items ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
    for(size_t i=0;i<3;++i) check_equal(next().values[0].data.boolean_value,i==0);
  }
  it("uses IN in group keys aggregate arguments HAVING JOIN and UNION") {
    seed(); check_equal(open_query("SELECT id IN (SELECT id FROM items WHERE id<3) AS k,COUNT(id IN (SELECT id FROM items)) AS n FROM items GROUP BY 1 HAVING n IN (SELECT COUNT(*) FROM items WHERE id<3) ORDER BY k",NULL,0,&query),TURBODB_STATUS_OK);
    orm_sql_scan_row row=next(); check_equal(row.values[0].data.boolean_value,1); check_equal(row.values[1].data.uint64_value,2u); check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT a.id AS a,b.id AS b FROM items a RIGHT JOIN items b ON a.id=b.id AND a.id IN (SELECT id FROM items WHERE id>1) ORDER BY b.id",NULL,0,&query),TURBODB_STATUS_OK);
    row=next(); check_equal(row.values[0].kind,TURBODB_VALUE_NULL); check_equal(row.values[1].data.int64_value,1);
    for(int64_t id=2;id<=3;++id) check_equal(next().values[0].data.int64_value,id); close_query(&query);
    check_equal(open_query("SELECT id FROM items WHERE id IN (SELECT MIN(id) FROM items UNION SELECT MAX(id) FROM items) UNION SELECT id FROM items WHERE id=2 ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) check_equal(next().values[0].data.int64_value,id); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("binds IN signed unsigned and byte parameters using document order and owned payloads") {
    seed(); const turbodb_value_t numbers[]={turbodb_u64(UINT64_MAX),turbodb_u64(3)};
    check_equal(open_query("SELECT ? IN (SELECT id FROM items) AS absent,? IN (SELECT id FROM items) AS present FROM items LIMIT 1",numbers,2,&query),TURBODB_STATUS_OK);
    orm_sql_scan_row row=next(); check_equal(row.values[0].data.boolean_value,0); check_equal(row.values[1].data.boolean_value,1); close_query(&query);
    char text[]="kept"; turbodb_value_t params[]={turbodb_text(text),turbodb_text(text),turbodb_i64(2)};
    check_equal(open_query("SELECT ? IN (SELECT ? FROM items WHERE id=?) AS present FROM items LIMIT 1",params,3,&query),TURBODB_STATUS_OK);
    memset(text,'x',sizeof(text)-1); for(size_t i=0;i<3;++i) params[i]=turbodb_null();
    check_equal(next().values[0].data.boolean_value,1);
  }
  it("rejects IN shape type correlation and MySQL LIMIT restrictions before execution") {
    seed(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *sql[]={"SELECT TRUE OR id IN (SELECT missing FROM items) AS n FROM items",
      "SELECT id IN (SELECT id,score FROM items) AS n FROM items",
      "SELECT id IN (SELECT z.id FROM items b) AS n FROM items a",
      "SELECT TRUE OR id IN (SELECT 'bad' FROM items) AS n FROM items",
      "SELECT id IN (SELECT id FROM items LIMIT 1) AS n FROM items",
      "SELECT id IN (SELECT id FROM items UNION SELECT id FROM items LIMIT 1) AS n FROM items",
      "SELECT id IN ((SELECT id FROM items LIMIT 1) UNION SELECT id FROM items) AS n FROM items"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      check_equal(open_query(sql[i],NULL,0,&query),i<3?TURBODB_STATUS_SQL_ERROR:TURBODB_STATUS_UNSUPPORTED);
      check_null(query.owner); check_equal(owner.active_sources,0u); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("keeps skipped IN sources lazy and locks selected source arithmetic failures") {
    seed(); check_equal(open_query("SELECT CASE WHEN id=1 THEN TRUE ELSE id IN (SELECT id+9223372036854775807 FROM items) END AS n FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.boolean_value,1); orm_sql_scan_row row={.count=99};
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); close_query(&query); check_false(owner.failed);
    check_equal(open_query("SELECT id IN (SELECT id+9223372036854775807 FROM items) AS n FROM items LIMIT 0",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t before=budget.used.value[ORM_SQL_BUDGET_READ_ROWS]; check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],before);
  }
  it("bounds raw IN materialization and refunds its cache after a latched failure") {
    seed(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(open_query("SELECT id IN (SELECT id FROM items) AS member FROM items LIMIT 1",NULL,0,&query),TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]+1;
    orm_sql_scan_row row={.count=99};
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); check_false(owner.failed);
    close_query(&query); check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("explains IN queries without reading or materializing their set") {
    seed(); check_equal(open_query("EXPLAIN SELECT id FROM items WHERE id NOT IN (SELECT id+9223372036854775807 FROM items UNION SELECT id FROM items)",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS]; budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS]=reads;
    const uint64_t materialized=budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
    const char *kinds[]={"PRIMARY","SUBQUERY","UNION","UNION RESULT"};
    for(size_t i=0;i<4;++i) text_is(next().values[1].data.text_value,kinds[i]); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],materialized);
  }
  it("evaluates EXISTS once and exposes a nonnullable boolean for arbitrary output widths") {
    seed(); next_statement();
    check_equal(open_query("SELECT id,EXISTS(SELECT *,id+9223372036854775807 FROM items) AS present,NOT EXISTS(SELECT NULL,NULL FROM items WHERE FALSE) AS absent FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    orm_sql_schema_column column;
    check_equal(orm_tidesdb_sql_runtime_column(&query,1,&column,&error),TURBODB_STATUS_OK);
    check_equal(column.type.kind,TURBODB_VALUE_BOOLEAN); check_false(column.type.nullable);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
      check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].data.boolean_value,1); check_equal(row.values[2].data.boolean_value,1);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS]-reads,7u);
  }
  it("skips EXISTS projection ordering and unused scalar dependencies but validates names") {
    seed();
    check_equal(open_query("SELECT EXISTS(SELECT DISTINCT id+9223372036854775807,(SELECT id FROM items) FROM items ORDER BY id+9223372036854775807) AS n FROM items LIMIT 1",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(next().values[0].data.boolean_value,1);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS]-reads,2u); close_query(&query);
    check_equal(open_query("SELECT EXISTS(SELECT missing FROM items) AS n FROM items",NULL,0,&query),TURBODB_STATUS_SQL_ERROR);
    check_null(query.owner); check_equal(owner.active_sources,0u);
    check_equal(open_query("SELECT EXISTS(SELECT z.id FROM items b) AS n FROM items a",NULL,0,&query),TURBODB_STATUS_SQL_ERROR);
    check_null(query.owner); check_equal(owner.active_sources,0u);
  }
  it("preserves EXISTS LIMIT OFFSET and DISTINCT cardinality") {
    seed();
    const char *sql[]={
      "SELECT EXISTS(SELECT NULL FROM items) AS n FROM items LIMIT 1",
      "SELECT EXISTS(SELECT id FROM items LIMIT 0) AS n FROM items LIMIT 1",
      "SELECT EXISTS(SELECT id FROM items LIMIT 1 OFFSET 2) AS n FROM items LIMIT 1",
      "SELECT EXISTS(SELECT id FROM items LIMIT 1 OFFSET 3) AS n FROM items LIMIT 1",
      "SELECT EXISTS(SELECT DISTINCT 1 FROM items LIMIT 1 OFFSET 1) AS n FROM items LIMIT 1",
      "SELECT EXISTS(SELECT DISTINCT score FROM items LIMIT 1 OFFSET 2) AS n FROM items LIMIT 1"};
    const bool expected[]={true,false,true,false,false,true};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(next().values[0].data.boolean_value,expected[i]); close_query(&query);
    }
  }
  it("retains global and grouped aggregate cardinality inside EXISTS") {
    seed(); const char *sql[]={
      "SELECT EXISTS(SELECT MAX(id+9223372036854775807) FROM items WHERE FALSE) AS n FROM items LIMIT 1",
      "SELECT EXISTS(SELECT MAX(id+9223372036854775807) FROM items) AS n FROM items LIMIT 1",
      "SELECT EXISTS(SELECT COUNT(*) FROM items WHERE FALSE HAVING COUNT(*)>0) AS n FROM items LIMIT 1",
      "SELECT EXISTS(SELECT MAX(id+9223372036854775807) FROM items GROUP BY score HAVING COUNT(*)>0 LIMIT 1 OFFSET 2) AS n FROM items LIMIT 1"};
    const bool expected[]={true,true,false,true};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(next().values[0].data.boolean_value,expected[i]); close_query(&query);
    }
  }
  it("keeps UNION pagination and duplicate elimination when EXISTS needs row counts") {
    seed(); const char *sql[]={
      "SELECT EXISTS(SELECT id+9223372036854775807 FROM items UNION SELECT id FROM items) AS n FROM items LIMIT 1",
      "SELECT EXISTS(SELECT 1 FROM items UNION SELECT 1 FROM items LIMIT 1 OFFSET 1) AS n FROM items LIMIT 1",
      "SELECT EXISTS(SELECT 1 FROM items UNION ALL SELECT 1 FROM items LIMIT 1 OFFSET 5) AS n FROM items LIMIT 1",
      "SELECT EXISTS(SELECT 1 FROM items UNION ALL SELECT 1 FROM items LIMIT 1 OFFSET 6) AS n FROM items LIMIT 1",
      "SELECT EXISTS((SELECT 1 FROM items UNION SELECT 1 FROM items) LIMIT 1 OFFSET 1) AS n FROM items LIMIT 1",
      "SELECT EXISTS((SELECT DISTINCT 1 FROM items) UNION ALL SELECT 1 FROM items WHERE FALSE LIMIT 1 OFFSET 1) AS n FROM items LIMIT 1",
      "SELECT EXISTS(SELECT 1,2 FROM items WHERE FALSE UNION ALL SELECT 3,4 FROM items) AS n FROM items LIMIT 1"};
    const bool expected[]={true,false,true,false,false,false,true};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_OK);
      check_equal(next().values[0].data.boolean_value,expected[i]); close_query(&query);
    }
    check_equal(open_query("SELECT EXISTS(SELECT id,score FROM items UNION SELECT id FROM items) AS n FROM items",NULL,0,&query),TURBODB_STATUS_SQL_ERROR);
    check_null(query.owner); check_equal(owner.active_sources,0u);
  }
  it("uses EXISTS in join conditions HAVING and nested query dependencies") {
    seed();
    check_equal(open_query("SELECT a.id FROM items a JOIN items b ON a.id=b.id AND EXISTS(SELECT NULL FROM items WHERE id=2) WHERE NOT EXISTS(SELECT id FROM items WHERE FALSE) ORDER BY a.id",NULL,0,&query),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=3;++id) check_equal(next().values[0].data.int64_value,id);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT COUNT(*) AS n FROM items HAVING EXISTS(SELECT id FROM items WHERE id IN (SELECT MAX(id) FROM items))",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,3); close_query(&query);
    check_equal(open_query("SELECT (SELECT EXISTS(SELECT NULL FROM items WHERE id=3) FROM items LIMIT 1) AS n FROM items LIMIT 1",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.boolean_value,1);
  }
  it("copies EXISTS parameters and defers evaluation through lazy branches") {
    seed(); turbodb_value_t params[]={turbodb_i64(3),turbodb_i64(1),turbodb_i64(0)};
    check_equal(open_query("SELECT EXISTS(SELECT ? FROM items LIMIT ? OFFSET ?) AS n FROM items LIMIT 1",params,3,&query),TURBODB_STATUS_OK);
    for(size_t i=0;i<3;++i) params[i]=turbodb_null();
    check_equal(next().values[0].data.boolean_value,1); close_query(&query);
    check_equal(open_query("SELECT CASE WHEN id=1 THEN FALSE ELSE EXISTS(SELECT id FROM items WHERE id+9223372036854775807>0) END AS n FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.boolean_value,0);
    orm_sql_scan_row row={.state=ORM_SQL_SCAN_CANCELLED,.count=99};
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(row.count,99u); check_equal(row.state,ORM_SQL_SCAN_CANCELLED);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); check_false(owner.failed);
  }
  it("propagates EXISTS HAVING and required UNION projection failures") {
    seed(); const char *sql[]={
      "SELECT EXISTS(SELECT MAX(id) FROM items HAVING MAX(id+9223372036854775807)>0) AS n FROM items",
      "SELECT EXISTS(SELECT id+9223372036854775807 FROM items UNION SELECT id FROM items LIMIT 1 OFFSET 1) AS n FROM items"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_OK);
      orm_sql_scan_row row={.count=99};
      check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(row.count,99u); close_query(&query); check_equal(owner.active_sources,0u); check_false(owner.failed);
    }
  }
  it("explains EXISTS without executing predicates and cancels before source reads") {
    seed();
    check_equal(open_query("EXPLAIN SELECT id FROM items WHERE EXISTS(SELECT id FROM items WHERE id+9223372036854775807>0 UNION SELECT id FROM items)",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS]=reads;
    const char *kinds[]={"PRIMARY","SUBQUERY","UNION","UNION RESULT"};
    for(size_t i=0;i<4;++i) text_is(next().values[1].data.text_value,kinds[i]);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    close_query(&query); budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS]=LIMIT;
    check_equal(open_query("SELECT EXISTS(SELECT id FROM items) AS n FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t before=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_CANCELLED); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],before);
  }
  it("streams typed derived rows after releasing the AST and parameter payloads") {
    seed(); char tag[]="owned"; turbodb_value_t params[]={turbodb_text(tag),turbodb_i64(1),turbodb_i64(1),turbodb_i64(1)};
    check_equal(open_query("SELECT d.id,d.tag FROM (SELECT id,? AS tag FROM items WHERE id>? ORDER BY id DESC LIMIT ?) d WHERE d.id>?",params,4,&query),TURBODB_STATUS_OK);
    memset(tag,'x',sizeof(tag)-1); for(size_t i=0;i<4;++i) params[i]=turbodb_null();
    check_equal(query.dependencies.derived_count,1u); check_equal(query.dependencies.query_count,0u);
    orm_sql_schema_column column;
    check_equal(orm_tidesdb_sql_runtime_column(&query,1,&column,&error),TURBODB_STATUS_OK); text_is(column.name,"tag");
    const orm_sql_scan_row row=next(); check_equal(row.count,2u); check_equal(row.values[0].data.int64_value,3);
    text_is(row.values[1].data.text_value,"owned"); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("streams derived stars without eager materialization and isolates IN pagination rules") {
    seed();
    check_equal(open_query("SELECT d.* FROM (SELECT id,score FROM items) d LIMIT 1",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_sql_dependencies_close(&query.dependencies,&error),TURBODB_STATUS_BUSY);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    const orm_sql_scan_row row=next(); check_equal(row.count,2u); check_equal(row.values[0].data.int64_value,1);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS]-reads,1u); close_query(&query);
    check_equal(open_query("SELECT id FROM items WHERE id IN (SELECT d.id FROM (SELECT id FROM items ORDER BY id DESC LIMIT 1) d)",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,3); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("binds derived empty-result types and preserves aggregate cardinality") {
    seed();
    check_equal(open_query("SELECT d.score FROM (SELECT score FROM items WHERE FALSE) d",NULL,0,&query),TURBODB_STATUS_OK);
    orm_sql_schema_column column;
    check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK);
    check_equal(column.type.kind,TURBODB_VALUE_INT64); check_true(column.type.nullable);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT d.n FROM (SELECT COUNT(*) AS n FROM items WHERE FALSE) d",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,0); close_query(&query);
    check_equal(open_query("SELECT MAX(d.n) AS n FROM (SELECT score,COUNT(*) AS n FROM items GROUP BY score HAVING COUNT(*)>0) d",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,1);
  }
  it("opens nested derived and expression dependencies in one order") {
    seed();
    check_equal(open_query("SELECT d.id,(SELECT MAX(x.id) FROM (SELECT id FROM items) x) AS hi FROM (SELECT q.id FROM (SELECT id FROM items WHERE EXISTS(SELECT NULL FROM items)) q) d WHERE d.id IN (SELECT MIN(id) FROM items) ORDER BY d.id",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(query.dependencies.derived_count,3u); check_equal(query.dependencies.query_count,3u);
    const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,1); check_equal(row.values[1].data.int64_value,3);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query); check_equal(owner.active_sources,0u);
  }
  it("preserves derived UNION pagination and inherited output names") {
    seed();
    check_equal(open_query("SELECT d.k FROM (SELECT id AS k FROM items UNION SELECT id AS ignored FROM items ORDER BY k DESC LIMIT 2) d ORDER BY d.k",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,2); check_equal(next().values[0].data.int64_value,3);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT d.id FROM ((SELECT id FROM items LIMIT 1) UNION ALL SELECT id FROM items WHERE id=3) d ORDER BY d.id",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,1); check_equal(next().values[0].data.int64_value,3); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("joins independent derived sources and restores RIGHT JOIN logical order") {
    seed();
    check_equal(open_query("SELECT a.id AS a_id,b.id AS b_id FROM (SELECT id FROM items WHERE id=1) a RIGHT JOIN (SELECT id FROM items WHERE id<=2) b ON a.id=b.id ORDER BY b.id",NULL,0,&query),TURBODB_STATUS_OK);
    orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,1); check_equal(row.values[1].data.int64_value,1);
    row=next(); check_equal(row.values[0].kind,TURBODB_VALUE_NULL); check_equal(row.values[1].data.int64_value,2);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("SELECT a.id,b.score FROM items a LEFT JOIN (SELECT id,score FROM items WHERE id=1) b ON a.id=b.id ORDER BY a.id",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[1].data.int64_value,10);
    for(size_t i=0;i<2;++i) check_equal(next().values[1].kind,TURBODB_VALUE_NULL);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("rejects invalid derived schemas and cross-scope references before publishing") {
    seed(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *sql[]={"SELECT d.id FROM (SELECT id,id FROM items) d",
      "SELECT d.missing FROM (SELECT id FROM items) d",
      "SELECT d.id FROM items a JOIN (SELECT a.id FROM items b) d ON a.id=d.id",
      "SELECT d.id FROM (SELECT id FROM items) d JOIN items d ON d.id=d.id",
      "SELECT items.id FROM (SELECT id FROM items) d"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_SQL_ERROR);
      check_null(query.owner); check_equal(owner.active_sources,0u); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("defers derived errors and refunds cancelled source leases") {
    seed();
    check_equal(open_query("SELECT d.n FROM (SELECT id+9223372036854775807 AS n FROM items) d LIMIT 0",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); close_query(&query);
    check_equal(open_query("SELECT d.n FROM (SELECT id+9223372036854775807 AS n FROM items) d",NULL,0,&query),TURBODB_STATUS_OK);
    orm_sql_scan_row row={.count=99};
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u);
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); close_query(&query); check_false(owner.failed);
    check_equal(open_query("SELECT d.id FROM (SELECT id FROM items) d",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_CANCELLED);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_BUSY);
    close_query(&query); check_equal(owner.active_sources,0u);
  }
  it("explains nested derived UNION sources without reading business rows") {
    seed();
    check_equal(open_query("EXPLAIN SELECT d.n FROM (SELECT id+9223372036854775807 AS n FROM items UNION ALL SELECT q.id FROM (SELECT id FROM items) q) d",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS]; budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS]=reads;
    const char *kinds[]={"PRIMARY","DERIVED","UNION","UNION RESULT","DERIVED"};
    for(size_t i=0;i<sizeof(kinds)/sizeof(kinds[0]);++i) text_is(next().values[1].data.text_value,kinds[i]);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
  }
  it("materializes a CTE once for independent self join references") {
    seed();
    check_equal(open_query("WITH c AS (SELECT id,score FROM items) SELECT a.id,b.score FROM c a JOIN c b ON a.id=b.id ORDER BY a.id",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    for(int64_t id=1;id<=3;++id) {
      const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,id);
      if(id==3) check_equal(row.values[1].kind,TURBODB_VALUE_NULL); else check_equal(row.values[1].data.int64_value,id*10);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS]-reads,3u);
    close_query(&query); check_equal(owner.active_sources,0u);
  }
  it("opens earlier CTE definitions before later dependents and copies renamed column metadata") {
    seed(); char text[]="kept"; turbodb_value_t params[]={turbodb_i64(10),turbodb_text(text),turbodb_i64(11)};
    check_equal(open_query("WITH a(k,tag) AS (SELECT id+?,? FROM items), b AS (SELECT k,tag FROM a WHERE k>?) SELECT b.k,b.tag FROM b ORDER BY b.k",params,3,&query),TURBODB_STATUS_OK);
    memset(text,'x',sizeof(text)-1); for(size_t i=0;i<3;++i) params[i]=turbodb_null();
    orm_sql_schema_column column; check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK); text_is(column.name,"k");
    for(int64_t id=12;id<=13;++id) { const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,id); text_is(row.values[1].data.text_value,"kept"); }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("resolves inner CTE shadowing and access to earlier outer definitions") {
    seed();
    check_equal(open_query("WITH c AS (SELECT id FROM items WHERE id=1) SELECT d.id FROM (WITH c AS (SELECT id FROM items WHERE id=2) SELECT id FROM c) d UNION ALL SELECT id FROM c",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,2); check_equal(next().values[0].data.int64_value,1); check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("WITH c AS (SELECT id FROM items WHERE id=3) SELECT d.id FROM (WITH q AS (SELECT id FROM c) SELECT id FROM q) d",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,3); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("resolves CTE names before Catalog names while respecting declaration visibility") {
    seed();
    check_equal(open_query("WITH items AS (SELECT id FROM items WHERE id=2) SELECT id FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,2); check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("WITH a AS (SELECT id FROM items WHERE id=1) SELECT d.id FROM (WITH b AS (SELECT id FROM a), a AS (SELECT id FROM items WHERE id=2) SELECT id FROM b) d",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,1); check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(open_query("(WITH c AS (SELECT id FROM items WHERE id=2) SELECT id FROM c) UNION ALL SELECT id FROM items WHERE id=3",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,2); check_equal(next().values[0].data.int64_value,3); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("uses CTE sources inside scalar IN and EXISTS dependencies") {
    seed();
    check_equal(open_query("WITH c AS (SELECT id FROM items WHERE id>1) SELECT (SELECT MAX(id) FROM c) AS hi FROM items WHERE id IN (SELECT id FROM c) AND EXISTS(SELECT NULL FROM c) ORDER BY id",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(next().values[0].data.int64_value,3); check_equal(next().values[0].data.int64_value,3); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS]-reads,6u); close_query(&query);
    check_equal(open_query("SELECT id FROM items WHERE id IN (WITH c AS (SELECT MAX(id) AS id FROM items) SELECT id FROM c)",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,3); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("preserves CTE UNION grouping pagination and empty-result types") {
    seed();
    check_equal(open_query("WITH c(k) AS (SELECT id FROM items UNION SELECT id FROM items ORDER BY id DESC LIMIT 2) SELECT MIN(k) AS lo,COUNT(*) AS n FROM c",NULL,0,&query),TURBODB_STATUS_OK);
    const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,2); check_equal(row.values[1].data.int64_value,2); close_query(&query);
    check_equal(open_query("WITH RECURSIVE c AS (SELECT score FROM items WHERE FALSE) SELECT score FROM c",NULL,0,&query),TURBODB_STATUS_OK);
    orm_sql_schema_column column; check_equal(orm_tidesdb_sql_runtime_column(&query,0,&column,&error),TURBODB_STATUS_OK);
    check_equal(column.type.kind,TURBODB_VALUE_INT64); check_true(column.type.nullable); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("rejects invalid CTE names widths forward references and recursion without leaking") {
    seed(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *sql[]={"WITH c AS (SELECT id FROM items), c AS (SELECT score FROM items) SELECT id FROM c",
      "WITH c(x,x) AS (SELECT id,score FROM items) SELECT x FROM c",
      "WITH c(x) AS (SELECT id,score FROM items) SELECT x FROM c",
      "WITH a AS (SELECT id FROM b), b AS (SELECT id FROM items) SELECT id FROM a",
      "WITH a AS (SELECT id FROM a) SELECT id FROM a",
      "WITH a AS (SELECT id,id FROM items) SELECT id FROM a"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      check_equal(open_query(sql[i],NULL,0,&query),TURBODB_STATUS_SQL_ERROR); check_null(query.owner);
      check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    check_equal(open_query("WITH RECURSIVE c AS (SELECT id FROM items UNION ALL SELECT id FROM c) SELECT id FROM c",NULL,0,&query),TURBODB_STATUS_UNSUPPORTED);
    check_null(query.owner); check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("delays unused CTE evaluation and latches shared materialization errors") {
    seed();
    check_equal(open_query("WITH c AS (SELECT id+9223372036854775807 AS n FROM items) SELECT id FROM items LIMIT 1",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(next().values[0].data.int64_value,1); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS]-reads,1u); close_query(&query);
    check_equal(open_query("WITH c AS (SELECT id+9223372036854775807 AS n FROM items) SELECT a.n FROM c a JOIN c b ON a.n=b.n",NULL,0,&query),TURBODB_STATUS_OK);
    orm_sql_scan_row row={.count=99}; check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u);
    const uint64_t failed_reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],failed_reads);
    check_false(owner.failed);
  }
  it("bounds CTE materialization and releases cancelled reference leases") {
    seed();
    check_equal(open_query("WITH c AS (SELECT id FROM items) SELECT id FROM c",NULL,0,&query),TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=1;
    orm_sql_scan_row row={.count=99};
    check_equal(orm_tidesdb_sql_runtime_next(&query,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u);
    close_query(&query); check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=LIMIT;
    check_equal(open_query("WITH c AS (SELECT id FROM items) SELECT a.id FROM c a JOIN c b ON a.id=b.id",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_CANCELLED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_BUSY);
    close_query(&query); check_equal(owner.active_sources,0u); check_false(owner.failed);
  }
  it("explains one CTE definition for several references without materializing it") {
    seed();
    check_equal(open_query("EXPLAIN WITH c AS (SELECT id+9223372036854775807 AS n FROM items) SELECT a.n FROM c a JOIN c b ON a.n=b.n",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS]; budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS]=reads;
    const uint64_t rows=budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
    text_is(next().values[1].data.text_value,"PRIMARY"); text_is(next().values[1].data.text_value,"PRIMARY");
    text_is(next().values[1].data.text_value,"DERIVED"); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],rows);
  }
  it("bounds dependency depth and refunds each construction step boundary") {
    seed(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    budget.limits.transaction.read_bytes=UINT64_MAX;
    const char *sql[]={"SELECT (SELECT (SELECT MIN(id) FROM items) FROM items LIMIT 1) AS n FROM items",
      "EXPLAIN SELECT (SELECT MAX(id) FROM items UNION ALL SELECT MIN(id) FROM items) AS n FROM items",
      "SELECT id IN (SELECT id FROM items WHERE id IN (SELECT MAX(id) FROM items)) AS n FROM items",
      "EXPLAIN SELECT id FROM items WHERE id IN (SELECT MIN(id) FROM items UNION SELECT MAX(id) FROM items)",
      "SELECT o.id,(SELECT o.id UNION SELECT o.id) AS n FROM items o",
      "SELECT EXISTS((SELECT DISTINCT 1 FROM items) UNION SELECT 2 FROM items LIMIT 1 OFFSET 1) AS n FROM items",
      "EXPLAIN SELECT EXISTS(SELECT id FROM items UNION ALL SELECT id FROM items) AS n FROM items",
      "SELECT d.id FROM (SELECT id FROM items WHERE id IN (SELECT MAX(id) FROM items)) d",
      "EXPLAIN SELECT d.id FROM (SELECT id FROM items UNION SELECT id FROM items) d",
      "SELECT o.id,(SELECT d.x FROM (SELECT o.id AS x UNION SELECT o.id) d) AS n "
      "FROM items o",
      "SELECT o.id,(WITH c AS (SELECT o.id AS x UNION SELECT -1) "
      "SELECT MAX(x) FROM c) AS n FROM items o",
      "WITH outer_rows AS (SELECT id FROM items) "
      "SELECT o.id,(SELECT o.id) AS n FROM outer_rows o",
      "WITH outer_rows AS (SELECT id FROM items) SELECT o.id,"
      "(SELECT o.id+i.id) AS n FROM outer_rows o JOIN items i ON o.id=i.id",
      "SELECT o.id,(SELECT (SELECT i.id+o.id) FROM items i WHERE i.id=2) AS n FROM items o",
      "EXPLAIN SELECT o.id,(SELECT (SELECT i.id+o.id) FROM items i WHERE i.id=2) AS n FROM items o",
      "SELECT o.id,(SELECT (SELECT i.id+o.id) FROM items i GROUP BY i.id ORDER BY i.id LIMIT 1) AS n FROM items o GROUP BY o.id",
      "EXPLAIN SELECT o.id,(SELECT (SELECT i.id+o.id) FROM items i GROUP BY i.id ORDER BY i.id LIMIT 1) AS n FROM items o GROUP BY o.id",
      "SELECT o.id,(WITH c AS (SELECT (SELECT i.id+o.id) AS x FROM items i WHERE i.id=2) SELECT (SELECT x FROM c)) AS n FROM items o",
      "EXPLAIN SELECT o.id,(WITH c AS (SELECT (SELECT i.id+o.id) AS x FROM items i WHERE i.id=2) SELECT (SELECT x FROM c)) AS n FROM items o",
      "SELECT o.id,(SELECT d.x FROM (SELECT (SELECT i.id+o.id) AS x FROM items i WHERE i.id=2) d) AS n FROM items o",
      "WITH c AS (SELECT id FROM items) SELECT a.id FROM c a JOIN c b ON a.id=b.id",
      "EXPLAIN WITH c(x) AS (SELECT id FROM items UNION SELECT id FROM items) SELECT x FROM c",
      "SELECT 1", "EXPLAIN SELECT 1", "WITH c(x) AS (SELECT 1 UNION SELECT 2) SELECT x FROM c",
      "SELECT EXISTS(SELECT 1 EXCEPT ALL SELECT 2) AS n",
      "EXPLAIN SELECT 1 INTERSECT SELECT 2 EXCEPT ALL SELECT 3",
      "SELECT * FROM items a NATURAL RIGHT JOIN items b",
      "EXPLAIN SELECT * FROM items a JOIN items b USING(id,score)",
      "SELECT ABS(CEIL(id)) AS n FROM items GROUP BY CEILING(id) HAVING SIGN(CEIL(id))=1 ORDER BY ABS(CEILING(id))",
      "EXPLAIN SELECT ABS(CEIL(id)) AS n FROM items GROUP BY CEILING(id) HAVING SIGN(CEIL(id))=1 ORDER BY ABS(CEILING(id))",
      "((SELECT 1 AS n UNION SELECT 2)) UNION ALL SELECT 0.5",
      "EXPLAIN SELECT 1 AS n UNION ALL SELECT 18446744073709551615 UNION ALL SELECT 0.5"};
    sqlparser_document *doc=parse(sql[0]);
    check_equal(orm_tidesdb_sql_runtime_open(doc,&owner,vstr_from_cstr("app"),NULL,0,2,&query,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    sqlparser_document_destroy(doc); check_contains(error.message,"dependency depth");
    check_null(query.owner); check_equal(owner.active_sources,0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    for(size_t mode=0;mode<sizeof(sql)/sizeof(sql[0]);++mode) {
      next_statement(); check_equal(open_query(sql[mode],NULL,0,&query),TURBODB_STATUS_OK);
      const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]; close_query(&query);
      for(uint64_t point=0;point<steps;++point) {
        next_statement(); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=point;
        check_equal(open_query(sql[mode],NULL,0,&query),TURBODB_STATUS_LIMIT_EXCEEDED);
        check_null(query.owner); check_equal(owner.active_sources,0u); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
        budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=LIMIT;
      }
    }
  }
  it("executes ordinary DISTINCT tuple counts without retaining state after close") {
    seed(); size_t affected=0;
    check_equal(execute("INSERT INTO items VALUES(4,10),(5,NULL)",NULL,0,&affected),TURBODB_STATUS_OK);
    check_equal(affected,2u); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(open_query("SELECT COUNT(DISTINCT score,id>0) AS n,COUNT(score) AS present FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
    check_equal(row.values[0].data.int64_value,2); check_equal(row.values[1].data.int64_value,3);
    check_equal(next().state,ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    check_equal(open_query("SELECT COUNT(DISTINCT score,id) AS n FROM items WHERE FALSE",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,0); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("retains unsupported SELECT shape and modifier rejection") {
    seed(); const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *sql[] = {"SELECT id FROM app.items", "SELECT a.id FROM items a JOIN items b ON UNSUPPORTED_FN(a.id)=b.id",
      "SELECT COUNT(DISTINCT id) OVER() AS n FROM items",
      "SHOW EXTENDED INDEX FROM items", "SELECT id / 2 FROM items", "SELECT id DIV 2.0 FROM items",
      "EXPLAIN SELECT id / 2 FROM items", "SELECT 3.0 DIV 2.0 FROM items LIMIT 0"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
      check_equal(open_query(sql[i], NULL, 0, &query), TURBODB_STATUS_UNSUPPORTED); check_null(query.owner);
      check_equal(owner.active_sources, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
    }
  }
  it("locks query evaluation errors and preserves caller output without poisoning valid stored rows") {
    seed(); check_equal(open_query("SELECT id+9223372036854775807 AS overflowed FROM items", NULL, 0, &query), TURBODB_STATUS_OK);
    orm_sql_scan_row row = {.state = ORM_SQL_SCAN_CANCELLED, .count = 99};
    check_equal(orm_tidesdb_sql_runtime_next(&query, &row, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(row.count, 99u); check_equal(row.state, ORM_SQL_SCAN_CANCELLED);
    const uint64_t reads = budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(orm_tidesdb_sql_runtime_cancel(&query, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_runtime_next(&query, &row, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], reads); check_false(owner.failed);
    close_query(&query); check_equal(owner.active_sources, 0u);
    check_equal(open_query("SELECT id FROM items LIMIT 1", NULL, 0, &query), TURBODB_STATUS_OK); check_equal(next().state, ORM_SQL_SCAN_ROW);
  }
  it("explains the bound SELECT plan without opening a row iterator or consuming physical rows") {
    seed(); const uint64_t writes=budget.used.value[ORM_SQL_BUDGET_WRITE_BYTES];
    budget.limits.statement.value[ORM_SQL_BUDGET_WRITE_BYTES]=writes;
    check_equal(open_query("EXPLAIN FORMAT=TRADITIONAL SELECT DISTINCT score AS k,COUNT(*) AS n FROM items t "
      "WHERE id>0 GROUP BY k HAVING n>0 ORDER BY n DESC LIMIT 2 OFFSET 1",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(query.kind,ORM_SQL_QUERY_EXPLAIN); check_equal(query.columns,ORM_SQL_EXPLAIN_COLUMNS);
    check_null(query.as.select.source.iterator); check_equal(query.as.select.plan.active_runs,0u);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS]=reads;
    const char *names[]={"id","select_type","table","partitions","type","possible_keys","key","key_len","ref","rows","filtered","Extra"};
    for(size_t i=0;i<ORM_SQL_EXPLAIN_COLUMNS;++i) {
      orm_sql_schema_column column={0}; check_equal(orm_tidesdb_sql_runtime_column(&query,i,&column,&error),TURBODB_STATUS_OK);
      text_is(column.name,names[i]);
    }
    const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,1);
    text_is(row.values[1].data.text_value,"SIMPLE"); text_is(row.values[2].data.text_value,"t");
    text_is(row.values[4].data.text_value,"ALL");
    const size_t nulls[]={3,5,6,7,8,9,10};
    for(size_t i=0;i<sizeof(nulls)/sizeof(nulls[0]);++i) check_equal(row.values[nulls[i]].kind,TURBODB_VALUE_NULL);
    text_is(row.values[11].data.text_value,"Using where; Using temporary; Using filesort; Group aggregate; Using having; Distinct; Limit; Offset");
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_BYTES],writes);
    check_null(query.as.select.source.iterator);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner,true,&error),TURBODB_STATUS_BUSY);
    close_query(&query); check_equal(owner.active_sources,0u);
  }
  it("explains zero limits and unsafe business expressions without evaluating them") {
    seed(); const turbodb_value_t zero=turbodb_i64(0);
    check_equal(open_query("EXPLAIN SELECT id+9223372036854775807 AS overflowed FROM items LIMIT ?",&zero,1,&query),TURBODB_STATUS_OK);
    const orm_sql_scan_row row=next(); check_equal(row.values[4].kind,TURBODB_VALUE_NULL);
    check_equal(row.values[9].data.int64_value,0); text_is(row.values[11].data.text_value,"Zero limit"); close_query(&query);
    check_equal(open_query("EXPLAIN SELECT id+9223372036854775807 AS overflowed FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().values[11].kind,TURBODB_VALUE_NULL); close_query(&query);
    check_equal(open_query("EXPLAIN SELECT COUNT(*) AS n FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    text_is(next().values[11].data.text_value,"Global aggregate");
    check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_CANCELLED);
  }
  it("validates EXPLAIN names types and parameters while refusing unsupported formats and writes") {
    seed(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *unsupported[]={"EXPLAIN FORMAT=JSON SELECT id FROM items","EXPLAIN FORMAT=TREE SELECT id FROM items",
      "EXPLAIN DELETE FROM items","EXPLAIN UPDATE items SET score=0","EXPLAIN INSERT INTO items(id) VALUES(9)"};
    for(size_t i=0;i<sizeof(unsupported)/sizeof(unsupported[0]);++i) {
      const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
      check_equal(open_query(unsupported[i],NULL,0,&query),TURBODB_STATUS_UNSUPPORTED); check_null(query.owner);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    }
    check_equal(open_query("EXPLAIN SELECT missing FROM items",NULL,0,&query),TURBODB_STATUS_SQL_ERROR);
    check_equal(open_query("EXPLAIN SELECT id FROM absent",NULL,0,&query),TURBODB_STATUS_SQL_ERROR);
    check_equal(open_query("EXPLAIN SELECT ? AS n FROM items",NULL,0,&query),TURBODB_STATUS_SQL_ERROR);
    const turbodb_value_t bad[]={turbodb_f64(NAN),turbodb_text_v((vstr){"\xc0\x80",2})};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i)
      check_equal(open_query("EXPLAIN SELECT ? AS n FROM items",&bad[i],1,&query),TURBODB_STATUS_TYPE_ERROR);
    const turbodb_value_t negative=turbodb_i64(-1);
    check_equal(open_query("EXPLAIN SELECT id FROM items LIMIT ?",&negative,1,&query),TURBODB_STATUS_TYPE_ERROR);
    size_t affected=99; check_equal(execute("EXPLAIN SELECT id FROM items",NULL,0,&affected),TURBODB_STATUS_UNSUPPORTED);
    check_equal(affected,99u); check_equal(owner.active_sources,0u); check_false(owner.failed);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("keeps EXPLAIN cancellation and step failures within the query without leaking its lease") {
    seed(); check_equal(open_query("EXPLAIN SELECT id FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(orm_tidesdb_sql_runtime_cancel(&query,&error),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_CANCELLED); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    close_query(&query);
    check_equal(open_query("EXPLAIN SELECT id FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_runtime_next(&query,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.state,ORM_SQL_SCAN_CANCELLED);
    check_equal(orm_tidesdb_sql_runtime_next(&query,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    close_query(&query); check_equal(owner.active_sources,0u); check_false(owner.failed);
  }

  it("refunds every intercepted fixed-workspace allocation failure during query construction") {
    seed(); const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    /* Repeated fault attempts share a transaction; isolate allocation errors
     * from its cumulative Catalog read quota. */
    budget.limits.transaction.read_bytes=UINT64_MAX;
    const char *sql[] = {"SELECT id+? AS changed FROM items WHERE id>? LIMIT ?",
      "EXPLAIN SELECT score+? AS k,COUNT(*) AS n FROM items WHERE id>? GROUP BY k ORDER BY n LIMIT ?",
      "SELECT a.id+? AS id FROM items a LEFT JOIN items b ON a.id=b.id RIGHT JOIN items c ON b.id=c.id WHERE c.id>? LIMIT ?",
      "EXPLAIN SELECT a.id+? AS id FROM items a RIGHT JOIN items b ON a.id=b.id WHERE b.id>? LIMIT ?",
      "(SELECT id+? AS id FROM items ORDER BY id LIMIT 2) UNION ALL SELECT COUNT(*) AS n FROM items WHERE id>? UNION SELECT id FROM items ORDER BY id LIMIT ?",
      "EXPLAIN (SELECT a.id+? AS id FROM items a RIGHT JOIN items b ON a.id=b.id ORDER BY a.id LIMIT 2) UNION ALL SELECT COUNT(*) AS n FROM items WHERE id>? UNION SELECT id FROM items ORDER BY id LIMIT ?",
      "SELECT id+(SELECT MAX(id)+(SELECT MIN(id)+? FROM items) FROM items) AS n FROM items WHERE id>? ORDER BY n LIMIT ?",
      "EXPLAIN SELECT (SELECT id+? FROM items WHERE id>?) AS n FROM items UNION ALL SELECT id FROM items LIMIT ?",
      "SELECT id FROM items WHERE id+? IN (SELECT id+(SELECT MIN(id) FROM items) FROM items WHERE id>?) LIMIT ?",
      "EXPLAIN SELECT id IN (SELECT id+? FROM items WHERE id>?) AS n FROM items LIMIT ?",
      "SELECT EXISTS(SELECT DISTINCT id+? FROM items UNION SELECT id FROM items LIMIT 1 OFFSET ?) AS n FROM items LIMIT ?",
      "EXPLAIN SELECT EXISTS(SELECT MAX(id+?) FROM items HAVING COUNT(*)>? UNION ALL SELECT id FROM items) AS n FROM items LIMIT ?",
      "SELECT d.id FROM (SELECT id+? AS id FROM items WHERE id>?) d LIMIT ?",
      "EXPLAIN SELECT d.id FROM (SELECT id+? AS id FROM items WHERE id>? UNION SELECT id FROM items) d LIMIT ?",
      "WITH c(x) AS (SELECT id+? FROM items WHERE id>?) SELECT a.x FROM c a JOIN c b ON a.x=b.x LIMIT ?",
      "EXPLAIN WITH c(x) AS (SELECT id+? FROM items WHERE id>?) SELECT x FROM c LIMIT ?",
      "SELECT a.id+(SELECT a.id+b.id) AS id FROM items a JOIN items b "
      "ON EXISTS(SELECT 1 WHERE a.id+?=b.id) WHERE b.id>? LIMIT ?",
      "SELECT o.id+(SELECT COUNT(*) FROM items x JOIN items y "
      "ON x.id+?=y.id AND y.id=o.id) AS id FROM items o WHERE o.id>? LIMIT ?",
      "SELECT o.id,(SELECT o.id+? UNION SELECT o.id+? ORDER BY 1 LIMIT 1) AS n "
      "FROM items o ORDER BY o.id LIMIT ?",
      "SELECT o.id,(SELECT d.x+? FROM (SELECT o.id AS x UNION SELECT o.id) d) AS n "
      "FROM items o WHERE o.id>? LIMIT ?",
      "SELECT o.id,(WITH c(x) AS (SELECT o.id+? UNION SELECT -1) "
      "SELECT MAX(x) FROM c) AS n FROM items o WHERE o.id>? LIMIT ?",
      "WITH outer_rows AS (SELECT id FROM items WHERE id>?) "
      "SELECT o.id+(SELECT o.id+?) AS n FROM outer_rows o LIMIT ?",
      "WITH outer_rows AS (SELECT id FROM items WHERE id>?) "
      "SELECT o.id+(SELECT o.id+i.id+?) AS n FROM outer_rows o "
      "JOIN items i ON o.id=i.id LIMIT ?",
      "SELECT o.id,(SELECT (SELECT i.id+o.id+?) FROM items i WHERE i.id=2) AS n FROM items o WHERE o.id>? LIMIT ?",
      "EXPLAIN SELECT o.id,(SELECT (SELECT i.id+o.id+?) FROM items i WHERE i.id=2) AS n FROM items o WHERE o.id>? LIMIT ?",
      "SELECT o.id,(SELECT (SELECT i.id+o.id+?) FROM items i GROUP BY i.id ORDER BY i.id LIMIT 1) AS n FROM items o WHERE o.id>? GROUP BY o.id LIMIT ?",
      "EXPLAIN SELECT o.id,(SELECT (SELECT i.id+o.id+?) FROM items i GROUP BY i.id ORDER BY i.id LIMIT 1) AS n FROM items o WHERE o.id>? GROUP BY o.id LIMIT ?",
      "SELECT o.id,(WITH c AS (SELECT (SELECT i.id+o.id+?) AS x FROM items i WHERE i.id=2) SELECT (SELECT x FROM c)) AS n FROM items o WHERE o.id>? LIMIT ?",
      "EXPLAIN SELECT o.id,(WITH c AS (SELECT (SELECT i.id+o.id+?) AS x FROM items i WHERE i.id=2) SELECT (SELECT x FROM c)) AS n FROM items o WHERE o.id>? LIMIT ?",
      "SELECT o.id,(SELECT d.x FROM (SELECT (SELECT i.id+o.id+?) AS x FROM items i WHERE i.id=2) d) AS n FROM items o WHERE o.id>? LIMIT ?",
      "SELECT ?+? LIMIT ?", "EXPLAIN SELECT ?+? LIMIT ?",
      "WITH c(x) AS (SELECT ? UNION SELECT ?) SELECT a.x FROM c a JOIN c b ON a.x=b.x LIMIT ?",
      "SELECT EXISTS(SELECT id+? FROM items EXCEPT ALL SELECT id FROM items WHERE id>?) AS n FROM items LIMIT ?",
      "EXPLAIN WITH c(n) AS (SELECT id+? FROM items INTERSECT SELECT id FROM items WHERE id>?) SELECT n FROM c LIMIT ?",
      "SELECT id+? AS n FROM items a NATURAL RIGHT JOIN items b WHERE id>? LIMIT ?",
      "EXPLAIN SELECT id+? AS n FROM items a LEFT JOIN items b USING(id,score) WHERE id>? LIMIT ?",
      "SELECT ABS(CEIL(id)) AS n FROM items WHERE SIGN(score)>? GROUP BY CEILING(id) HAVING SIGN(CEIL(id))>? ORDER BY ABS(CEILING(id)) LIMIT ?",
      "EXPLAIN SELECT ABS(CEIL(id)) AS n FROM items WHERE SIGN(score)>? GROUP BY CEILING(id) HAVING SIGN(CEIL(id))>? ORDER BY ABS(CEILING(id)) LIMIT ?",
      "SELECT MOD(score DIV ?,?) AS n FROM items WHERE id>?",
      "EXPLAIN SELECT MOD(score DIV ?,?) AS n FROM items WHERE id>?",
      "SELECT MOD(score/2.0,?)+? AS n FROM items WHERE id>?",
      "EXPLAIN SELECT MOD(score/2.0,?)+? AS n FROM items WHERE id>?",
      "SELECT CASE WHEN id BETWEEN ? AND ?+0.0 THEN 1 ELSE 0 END AS n FROM items WHERE score>?+0.0",
      "EXPLAIN SELECT CASE WHEN id BETWEEN ? AND ?+0.0 THEN 1 ELSE 0 END AS n FROM items WHERE score>?+0.0",
      "SELECT CASE WHEN id>? THEN COALESCE(score,?+0.0) ELSE ? END AS n FROM items",
      "EXPLAIN SELECT CASE WHEN id>? THEN COALESCE(score,?+0.0) ELSE ? END AS n FROM items",
      "SELECT ? AS n UNION SELECT ? UNION ALL SELECT ?+0.0",
      "EXPLAIN SELECT ? AS n UNION SELECT ? UNION ALL SELECT ?+0.0",
      "SELECT ? AS n INTERSECT SELECT ? INTERSECT ALL SELECT ?+0.0",
      "EXPLAIN SELECT ? AS n EXCEPT SELECT ? EXCEPT ALL SELECT ?+0.0",
      "((SELECT ? AS n UNION SELECT ?)) UNION ALL SELECT ?+0.0",
      "EXPLAIN ((SELECT ? AS n UNION SELECT ?)) UNION ALL SELECT ?+0.0",
      "SELECT ? AS n UNION ALL (SELECT ? INTERSECT SELECT ?+0.0)",
      "EXPLAIN (SELECT ? AS n UNION SELECT ? LIMIT 2) EXCEPT ALL SELECT ?+0.0",
      "SELECT CAST(CAST(id+? AS UNSIGNED INTEGER) AS DOUBLE) AS n FROM items WHERE id>? LIMIT ?",
      "EXPLAIN SELECT CAST(CAST(id+? AS UNSIGNED INTEGER) AS DOUBLE) AS n FROM items WHERE id>? LIMIT ?",
      "SELECT CAST(id+? AS FLOAT(00024)) AS n FROM items WHERE id>? LIMIT ?",
      "EXPLAIN SELECT CAST(id+? AS FLOAT(00024)) AS n FROM items WHERE id>? LIMIT ?",
      "SELECT ROUND(id+?) AS n FROM items WHERE id>? LIMIT ?",
      "EXPLAIN SELECT ROUND(id+?) AS n FROM items WHERE id>? LIMIT ?",
      "SELECT TRUNCATE(ROUND(id+?,?),0) AS n FROM items WHERE id>?",
      "EXPLAIN SELECT TRUNCATE(ROUND(id+?,?),0) AS n FROM items WHERE id>?",
      "SHOW FULL TABLES", "SHOW COLUMNS FROM items", "SHOW INDEX FROM items", "SHOW CREATE TABLE items"};
    const turbodb_value_t params[] = {turbodb_i64(4), turbodb_i64(1), turbodb_i64(2)};
    for (size_t mode = 0; mode < sizeof(sql) / sizeof(sql[0]); ++mode) {
      const size_t count = mode < sizeof(sql)/sizeof(sql[0])-4 ? 3 : 0; reserves = resizes = 0;
      check_equal(open_query(sql[mode], params, count, &query), TURBODB_STATUS_OK);
      const size_t allocations[] = {reserves, resizes}; close_query(&query);
      for (size_t pass = 0; pass < 2; ++pass) for (size_t point = 1; point <= allocations[pass]; ++point) {
        next_statement(); reserves = resizes = 0; if (pass) fail_resize = point; else fail_reserve = point;
        check_equal(open_query(sql[mode], params, count, &query), TURBODB_STATUS_OUT_OF_MEMORY);
        fail_reserve = fail_resize = 0; check_null(query.owner); check_equal(query.kind, ORM_SQL_QUERY_CLOSED);
        check_equal(owner.active_sources, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work); check_false(owner.failed);
      }
    }
  }
  it("bounds runtime metadata and partial source construction before publishing the query") {
    seed(); const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *sql[] = {"SELECT id FROM items", "SHOW COLUMNS FROM items", "SHOW TABLES", "EXPLAIN SELECT id FROM items",
      "SELECT 1", "EXPLAIN SELECT 1", "SHOW INDEX FROM items", "SHOW CREATE TABLE items"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) for (size_t pass = 0; pass < 2; ++pass) {
      budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = work + (pass ? sizeof(query) : 0);
      check_equal(open_query(sql[i], NULL, 0, &query), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_null(query.owner); check_equal(owner.active_sources, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
      budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = WORK;
    }
  }
  it("does not reset statement or transaction counters between runtime calls") {
    seed(); const uint64_t writes = budget.used.value[ORM_SQL_BUDGET_WRITE_BYTES];
    const uint64_t transaction_writes = budget.transaction_used.write_bytes;
    budget.limits.statement.value[ORM_SQL_BUDGET_WRITE_BYTES] = writes;
    size_t affected = 99;
    check_equal(execute("UPDATE items SET score=12 WHERE id=1", NULL, 0, &affected), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(affected, 99u); check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_BYTES], writes);
    check_equal(budget.transaction_used.write_bytes, transaction_writes); check_false(owner.failed);
    check_equal(open_query("SELECT score FROM items WHERE id=1", NULL, 0, &query), TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value, 10);
  }
  it("validates runtime lifecycle inputs and rejects SQLite without publishing results") {
    orm_sql_scan_row row = {.count = 99}; orm_sql_schema_column column = {.name = vstr_from_cstr("kept")};
    check_equal(orm_tidesdb_sql_runtime_next(&query, &row, &error), TURBODB_STATUS_INVALID_STATE); check_equal(row.count, 99u);
    check_equal(orm_tidesdb_sql_runtime_cancel(&query, &error), TURBODB_STATUS_INVALID_STATE);
    check_equal(orm_tidesdb_sql_runtime_column(&query, 0, &column, &error), TURBODB_STATUS_INVALID_STATE); text_is(column.name, "kept");
    sqlparser_document *doc = parse("SHOW TABLES"); size_t affected = 99;
    check_equal(orm_tidesdb_sql_runtime_execute(doc, &owner, NULL, 0, 0, 0, false, NULL, &affected, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_runtime_execute(doc, &owner, NULL, 1, DEPTH, 0, false, NULL, &affected, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(affected, 99u); sqlparser_document_destroy(doc); doc = NULL;
    const char sql[] = "SELECT 1"; const sqlparser_options options = {.dialect = SQLPARSER_SQLITE}; sqlparser_error parse_error;
    check_equal(sqlparser_parse_with_options(sql, sizeof(sql) - 1, &options, NULL, &doc, &parse_error), SQLPARSER_OK);
    check_equal(orm_tidesdb_sql_runtime_open(doc, &owner, vstr_from_cstr("app"), NULL, 0, DEPTH, &query, &error), TURBODB_STATUS_UNSUPPORTED);
    sqlparser_document_destroy(doc); check_equal(open_query("SHOW TABLES", NULL, 0, &query), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_runtime_column(&query, query.columns, &column, &error), TURBODB_STATUS_INVALID_ARGUMENT); text_is(column.name, "kept");
    close_query(&query); close_query(&query);
  }
  it("propagates corrupt-row failures through runtime and requires owner rollback") {
    seed(); check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    /* Test-only corruption of table ID 1, generation 1, signed PK 1. */
    enum { TABLE_ID_OFFSET = 1, GENERATION_OFFSET = 9, SIGN_BIT = 0x80 };
    uint8_t key[ORM_SQL_RELATION_KEY_BYTES] = {3}, bad = 0;
    orm_sql_wire_write(key + TABLE_ID_OFFSET, ORM_SQL_WIRE_U64, 1);
    orm_sql_wire_write(key + GENERATION_OFFSET, ORM_SQL_WIRE_U64, 1);
    key[ORM_SQL_RELATION_PREFIX_BYTES] = SIGN_BIT; key[sizeof(key) - 1] = 1;
    orm_tidesdb_transaction_t *native = NULL;
    check_equal(orm_tidesdb_txn_begin_with_isolation(database, ORM_TDB_ISOLATION_SERIALIZABLE, &native), ORM_TDB_SUCCESS);
    check_equal(orm_tidesdb_txn_put(native, family, key, sizeof(key), &bad, sizeof(bad), 0), ORM_TDB_SUCCESS);
    check_equal(orm_tidesdb_txn_commit(native), ORM_TDB_SUCCESS); orm_tidesdb_txn_free(native); begin();
    check_equal(open_query("EXPLAIN SELECT id FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_ROW); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_null(query.as.select.source.iterator); check_false(owner.failed); close_query(&query);
    check_equal(open_query("EXPLAIN SELECT a.id FROM items a JOIN items b ON a.id=b.id",NULL,0,&query),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_ROW); check_equal(next().state,ORM_SQL_SCAN_ROW);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_false(owner.failed); close_query(&query);
    check_equal(open_query("EXPLAIN SELECT id FROM items UNION SELECT id FROM items",NULL,0,&query),TURBODB_STATUS_OK);
    for(size_t i=0;i<3;++i) check_equal(next().state,ORM_SQL_SCAN_ROW);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_false(owner.failed); close_query(&query);
    check_equal(open_query("SELECT a.id FROM items a JOIN items b ON a.id=b.id", NULL, 0, &query), TURBODB_STATUS_OK);
    orm_sql_scan_row row = {.count = 99};
    check_equal(orm_tidesdb_sql_runtime_next(&query, &row, &error), TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(row.count, 99u); check_true(owner.failed);
    const uint64_t reads = budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(orm_tidesdb_sql_runtime_next(&query, &row, &error), TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], reads); close_query(&query);
    check_equal(owner.active_sources, 0u);
    next_statement(); check_true(owner.failed);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_INVALID_STATE);
    check_equal(open_query("SHOW TABLES", NULL, 0, &query), TURBODB_STATUS_INVALID_STATE);
    size_t affected = 99; check_equal(execute("DELETE FROM items", NULL, 0, &affected), TURBODB_STATUS_INVALID_STATE); check_equal(affected, 99u);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
  }
  it("gives consecutive SQL statements fresh limits while keeping one native transaction") {
    next_statement(); enum { MAX_STATEMENT_WRITES = 3 };
    budget.limits.statement.value[ORM_SQL_BUDGET_WRITE_ROWS] = MAX_STATEMENT_WRITES;
    orm_tidesdb_transaction_t *native = owner.transaction; size_t affected = 99;
    check_equal(execute(ddl, NULL, 0, &affected), TURBODB_STATUS_OK); check_equal(affected, 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS], MAX_STATEMENT_WRITES);
    next_statement(); check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS], 0u); check_true(owner.transaction == native);
    check_equal(execute("INSERT INTO items(id,score) VALUES(1,10),(2,20)", NULL, 0, &affected), TURBODB_STATUS_OK);
    check_equal(affected, 2u); check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS], MAX_STATEMENT_WRITES);
    next_statement(); check_true(owner.transaction == native);
    check_equal(execute("UPDATE items SET score=score+1", NULL, 0, &affected), TURBODB_STATUS_OK); check_equal(affected, 2u);
    next_statement(); check_true(owner.transaction == native);
    check_equal(open_query("SELECT score FROM items", NULL, 0, &query), TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value, 11); check_equal(next().values[0].data.int64_value, 21);
    check_equal(next().state, ORM_SQL_SCAN_DONE); close_query(&query);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, true, &error), TURBODB_STATUS_OK);
    check_false(budget.statement_active); check_equal(budget.retained_work_bytes, 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK); reopen();
    check_equal(open_query("SELECT score FROM items", NULL, 0, &query), TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value, 11); check_equal(next().values[0].data.int64_value, 21);
  }
  it("rolls back all successful statements even after multiple budget boundaries") {
    size_t affected = 99;
    check_equal(execute(ddl, NULL, 0, &affected), TURBODB_STATUS_OK); next_statement();
    check_equal(execute("INSERT INTO items(id,score) VALUES(1,10)", NULL, 0, &affected), TURBODB_STATUS_OK); next_statement();
    check_equal(execute("UPDATE items SET score=30", NULL, 0, &affected), TURBODB_STATUS_OK); next_statement();
    check_equal(open_query("SELECT score FROM items", NULL, 0, &query), TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value, 30); close_query(&query);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    const uint64_t charged = budget.transaction_used.write_bytes;
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    check_equal(budget.transaction_used.write_bytes, charged);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK); reopen();
    check_equal(open_query("SHOW TABLES", NULL, 0, &query), TURBODB_STATUS_OK); check_equal(next().state, ORM_SQL_SCAN_DONE);
  }
  it("requires query close after EOF cancellation or error before ending the statement") {
    seed(); const char *sql[] = {"SELECT id FROM items LIMIT 0", "SHOW TABLES", "SELECT id FROM items",
      "SELECT id+9223372036854775807 AS overflowed FROM items"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
      check_equal(open_query(sql[i], NULL, 0, &query), TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_BUSY);
      if (i == 2) {
        check_equal(orm_tidesdb_sql_runtime_cancel(&query, &error), TURBODB_STATUS_OK); check_equal(next().state, ORM_SQL_SCAN_CANCELLED);
      } else if (i == 3) {
        orm_sql_scan_row row = {0}; check_equal(orm_tidesdb_sql_runtime_next(&query, &row, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
      } else {
        if (i == 1) check_equal(next().state, ORM_SQL_SCAN_ROW);
        check_equal(next().state, ORM_SQL_SCAN_DONE);
      }
      const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_BUSY); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
      check_true(budget.statement_active); close_query(&query); next_statement();
    }
  }
  it("keeps an independently owned schema within its original statement") {
    seed(); orm_sql_table_definition definition = {0}; uint64_t id = 0, version = 0; bool found = false;
    check_equal(orm_tidesdb_sql_catalog_lookup(&owner, vstr_from_cstr("items"), &definition, &id, &version, &found, &error), TURBODB_STATUS_OK);
    check_true(found); check_equal(owner.active_sources, 0u);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_BUSY);
    orm_sql_table_schema schema = {0}; check_equal(orm_tidesdb_sql_catalog_schema(&definition, &schema, &error), TURBODB_STATUS_OK);
    text_is(schema.name, "items"); check_equal(schema.count, 2u);
    check_equal(orm_tidesdb_sql_catalog_destroy(&definition, &error), TURBODB_STATUS_OK); next_statement();
    check_equal(open_query("SELECT id FROM items LIMIT 1", NULL, 0, &query), TURBODB_STATUS_OK); check_equal(next().state, ORM_SQL_SCAN_ROW);
  }
  it("does not admit runtime operations in the gap between statements") {
    seed(); check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    orm_tidesdb_transaction_t *native = owner.transaction;
    const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], writes = budget.transaction_used.write_bytes;
    size_t affected = 99;
    check_equal(execute("UPDATE items SET score=77", NULL, 0, &affected), TURBODB_STATUS_INVALID_STATE); check_equal(affected, 99u);
    check_equal(open_query("SELECT id FROM items", NULL, 0, &query), TURBODB_STATUS_INVALID_STATE);
    check_equal(open_query("SHOW TABLES", NULL, 0, &query), TURBODB_STATUS_INVALID_STATE); check_null(query.owner);
    check_true(owner.transaction == native); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
    check_equal(budget.transaction_used.write_bytes, writes);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    check_equal(open_query("SELECT score FROM items WHERE id=1", NULL, 0, &query), TURBODB_STATUS_OK); check_equal(next().values[0].data.int64_value, 10);
  }
  it("cannot bypass cumulative transaction write limits by starting another statement") {
    seed(); next_statement(); size_t affected = 99;
    check_equal(execute("UPDATE items SET score=11 WHERE id=1", NULL, 0, &affected), TURBODB_STATUS_OK);
    const uint64_t one_statement = budget.used.value[ORM_SQL_BUDGET_WRITE_BYTES], cumulative = budget.transaction_used.write_bytes;
    check_greater(one_statement, 0u);
    budget.limits.statement.value[ORM_SQL_BUDGET_WRITE_BYTES] = one_statement;
    budget.limits.transaction.write_bytes = cumulative;
    next_statement(); affected = 99;
    for (size_t i = 0; i < 2; ++i) {
      check_equal(execute("UPDATE items SET score=21 WHERE id=2", NULL, 0, &affected), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(affected, 99u); check_contains(error.message, "transaction write bytes"); check_false(owner.failed);
      check_equal(budget.transaction_used.write_bytes, cumulative);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_BYTES], 0u);
      check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_budget_reset_transaction(&budget, &error), TURBODB_STATUS_BUSY);
      check_equal(budget.transaction_used.write_bytes, cumulative);
      check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    }
    check_equal(open_query("SELECT score FROM items WHERE id=2", NULL, 0, &query), TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value, 20); close_query(&query);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_finish(&owner, false, &error), TURBODB_STATUS_OK);
    check_equal(budget.transaction_used.write_bytes, cumulative);
    check_equal(orm_tidesdb_sql_budget_reset_transaction(&budget, &error), TURBODB_STATUS_OK);
    check_equal(budget.transaction_used.write_bytes, 0u);
  }
  it("can finish a failed statement and reuse the same transaction after allocation or constraint errors") {
    seed(); orm_tidesdb_transaction_t *native = owner.transaction;
    reserves = 0; fail_reserve = 1;
    check_equal(open_query("SELECT id FROM items", NULL, 0, &query), TURBODB_STATUS_OUT_OF_MEMORY); fail_reserve = 0;
    check_null(query.owner); next_statement(); check_true(owner.transaction == native);
    size_t affected = 99;
    check_equal(execute("INSERT INTO items(id,score) VALUES(1,99)", NULL, 0, &affected), TURBODB_STATUS_CONSTRAINT);
    check_equal(affected, 99u); next_statement(); check_true(owner.transaction == native); check_false(owner.failed);
    check_equal(execute("UPDATE items SET score=12 WHERE id=1", NULL, 0, &affected), TURBODB_STATUS_OK); next_statement();
    check_equal(open_query("SELECT score FROM items WHERE id=1", NULL, 0, &query), TURBODB_STATUS_OK); check_equal(next().values[0].data.int64_value, 12);
  }

}
