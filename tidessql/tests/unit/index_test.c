#include "index.h"
#include "catalog.h"
#include <tinytest.h>
#include <stdio.h>
#include <string.h>

static size_t reserves, resizes, fail_reserve, fail_resize;
static stl_status index_test_reserve(vec_t *v, size_t n) {
  return ++reserves == fail_reserve ? STL_OUT_OF_MEMORY : vec_reserve(v, n);
}
static stl_status index_test_resize(vec_t *v, size_t n) {
  return ++resizes == fail_resize ? STL_OUT_OF_MEMORY : vec_resize(v, n);
}
#define vec_reserve index_test_reserve
#define vec_resize index_test_resize
#include "../../src/work.c"
#include "../../src/index.c"
#undef vec_reserve
#undef vec_resize

enum { TEST_LIMIT = 65536, TEST_WORK = 4 * 1024 * 1024, TEST_SQL_BYTES = 512 };
static const orm_sql_schema_column columns[] = {
  {{"id", 2}, {TURBODB_VALUE_INT64, false}},
  {{"score", 5}, {TURBODB_VALUE_INT64, true}},
  {{"amount", 6}, {TURBODB_VALUE_UINT64, true}},
  {{"weight", 6}, {TURBODB_VALUE_DOUBLE, true}}
};
static const orm_sql_table_schema schema = {{"items", 5}, columns, sizeof(columns) / sizeof(columns[0])};
static const char basic_ddl[] = "CREATE UNIQUE INDEX ix ON items (amount DESC, score ASC, id)";
static orm_tidesdb_sql_budget budget;
static orm_sql_budget_limits limits;
static orm_sql_index_definition definition;
static sqlparser_document *document;
static turbodb_error_t error;

static void parse_dialect(const char *sql, sqlparser_dialect dialect) {
  sqlparser_document_destroy(document); document = NULL;
  sqlparser_error parse_error;
  const sqlparser_options options = {dialect, false};
  const sqlparser_status status = sqlparser_parse_with_options(sql, strlen(sql), &options, NULL, &document, &parse_error);
  if (status != SQLPARSER_OK) info("SQL: %s; byte %zu: %s", sql, parse_error.offset, parse_error.message);
  check_equal(status, SQLPARSER_OK);
}
static void parse(const char *sql) { parse_dialect(sql, SQLPARSER_MYSQL); }
static turbodb_status_t bind_create(void) {
  return orm_tidesdb_sql_index_bind_create(document, &schema, &budget, &definition, &error);
}
static void reset(void) {
  fail_reserve = fail_resize = 0;
  check_equal(orm_tidesdb_sql_index_destroy(&definition, &error), TURBODB_STATUS_OK);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
  check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
  reserves = resizes = 0;
}
static void reject(const char *sql, turbodb_status_t expected) {
  reset(); parse(sql); check_equal(bind_create(), expected);
  check_contains(error.message, "at byte"); check_null(definition.budget);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
}
static const orm_sql_index_part *part(size_t ordinal) { return vec_at_const(&definition.parts, ordinal); }

spec("TidesDB CREATE INDEX definition") {
  before_each() {
    reserves = resizes = fail_reserve = fail_resize = 0;
    definition = (orm_sql_index_definition){0}; document = NULL; tdsql_error_init(&error);
    limits = (orm_sql_budget_limits){0};
    for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) limits.statement.value[i] = TEST_LIMIT;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = TEST_WORK;
    limits.transaction = (orm_sql_transaction_budget_amount){TEST_LIMIT, TEST_LIMIT, TEST_LIMIT};
    check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
  }
  after_each() {
    fail_reserve = fail_resize = 0;
    check_equal(orm_tidesdb_sql_index_destroy(&definition, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_ROWS], 0u);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document = NULL;
  }
  it("preserves composite order signedness direction and nullable uniqueness") {
    parse(basic_ddl); check_equal(bind_create(), TURBODB_STATUS_OK);
    check_true(definition.unique); check_equal(vec_size(&definition.parts), 3u);
    check_equal(part(0)->column, 2u); check_equal(part(0)->type.kind, TURBODB_VALUE_UINT64);
    check_true(part(0)->descending); check_true(part(0)->type.nullable);
    check_equal(part(1)->column, 1u); check_equal(part(1)->type.kind, TURBODB_VALUE_INT64);
    check_false(part(1)->descending); check_true(part(1)->type.nullable);
    check_equal(part(2)->column, 0u); check_false(part(2)->descending); check_false(part(2)->type.nullable);
    check_equal(budget.used.value[ORM_SQL_BUDGET_PLAN_NODES], 4u);
  }
  it("binds an ordinary index and preserves quoted names") {
    parse("CREATE INDEX `Index_Name` ON `items` (`score` DESC)"); check_equal(bind_create(), TURBODB_STATUS_OK);
    check_false(definition.unique); check_equal(definition.name_size, 10u);
    check_equal(strcmp(definition.name, "Index_Name"), 0); check_equal(strcmp(definition.table, "items"), 0);
    check_equal(part(0)->column, 1u); check_true(part(0)->descending);
  }
  it("owns definitions after the AST and real Catalog schema are destroyed") {
    orm_sql_table_definition table = {0}; orm_sql_table_schema borrowed = {0};
    parse("CREATE TABLE items (id BIGINT PRIMARY KEY, score BIGINT, amount BIGINT UNSIGNED)");
    check_equal(orm_tidesdb_sql_catalog_bind_create(document, &budget, &table, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_schema(&table, &borrowed, &error), TURBODB_STATUS_OK);
    parse(basic_ddl);
    check_equal(orm_tidesdb_sql_index_bind_create(document, &borrowed, &budget, &definition, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_destroy(&table, &error), TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document = NULL;
    check_equal(strcmp(definition.name, "ix"), 0); check_equal(strcmp(definition.table, "items"), 0);
    check_equal(definition.table_size, 5u); check_equal(part(0)->column, 2u);
    check_equal(part(0)->type.kind, TURBODB_VALUE_UINT64); check_true(part(0)->type.nullable);
  }
  it("rejects unknown repeated and case-mismatched key columns") {
    reject("CREATE INDEX ix ON items (missing)", TURBODB_STATUS_SQL_ERROR);
    reject("CREATE INDEX ix ON items (score, score DESC)", TURBODB_STATUS_SQL_ERROR);
    reject("CREATE INDEX ix ON items (`score`, score)", TURBODB_STATUS_SQL_ERROR);
    reject("CREATE INDEX ix ON items (Score)", TURBODB_STATUS_SQL_ERROR);
  }
  it("rejects mismatched tables and the reserved primary index name") {
    reject("CREATE INDEX ix ON other (id)", TURBODB_STATUS_SQL_ERROR);
    reject("CREATE INDEX ix ON Items (id)", TURBODB_STATUS_SQL_ERROR);
    reject("CREATE UNIQUE INDEX `PrImArY` ON items (id)", TURBODB_STATUS_SQL_ERROR);
  }
  it("rejects expressions prefixes collations parameters and qualified table names") {
    const char *sql[] = {"CREATE INDEX ix ON items ((score+1))", "CREATE INDEX ix ON items (score(2))",
      "CREATE INDEX ix ON items ((score COLLATE binary))", "CREATE INDEX ix ON items ((?))",
      "CREATE INDEX ix ON items ((items.score))", "CREATE INDEX ix ON items ((score))",
      "CREATE INDEX ix ON db.items (id)", "CREATE INDEX ix ON items ((1))"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) reject(sql[i], TURBODB_STATUS_UNSUPPORTED);
  }
  it("binds DOUBLE and integer composite keys with unique NULL and direction semantics") {
    parse("CREATE UNIQUE INDEX ix ON items (weight DESC,amount,score)");
    check_equal(bind_create(), TURBODB_STATUS_OK); check_true(definition.unique);
    const orm_sql_index_part *part = vec_at_const(&definition.parts, 0);
    check_equal(part->column, 3u); check_equal(part->type.kind, TURBODB_VALUE_DOUBLE);
    check_true(part->type.nullable); check_true(part->descending); check_equal(vec_size(&definition.parts), 3u);
  }
  it("rejects nonnumeric and unverified key types") {
    const turbodb_value_kind_t kinds[] = {TURBODB_VALUE_TEXT, TURBODB_VALUE_BLOB, TURBODB_VALUE_BOOLEAN, TURBODB_VALUE_NULL};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
      reset(); parse("CREATE INDEX ix ON items (id)");
      const orm_sql_schema_column column = {{"id", 2}, {kinds[i], true}};
      const orm_sql_table_schema other = {{"items", 5}, &column, 1};
      check_equal(orm_tidesdb_sql_index_bind_create(document, &other, &budget, &definition, &error), TURBODB_STATUS_UNSUPPORTED);
      check_null(definition.budget);
    }
  }
  it("rejects SQLite dialect and non-index or multiple statements") {
    parse_dialect("CREATE INDEX IF NOT EXISTS ix ON items (id) WHERE id > 0", SQLPARSER_SQLITE);
    check_equal(bind_create(), TURBODB_STATUS_UNSUPPORTED);
    reject("SELECT 1", TURBODB_STATUS_UNSUPPORTED);
    reject("CREATE INDEX ix ON items (id); CREATE INDEX iy ON items (score)", TURBODB_STATUS_UNSUPPORTED);
  }
  it("enforces the shared identifier capacity at the exact boundary") {
    char name[ORM_SQL_SELECT_NAME_BYTES + 2], sql[TEST_SQL_BYTES];
    memset(name, 'a', sizeof(name)); name[ORM_SQL_SELECT_NAME_BYTES] = 0;
    (void)snprintf(sql, sizeof(sql), "CREATE INDEX `%s` ON items (id)", name);
    parse(sql); check_equal(bind_create(), TURBODB_STATUS_OK);
    check_equal(definition.name_size, (size_t)ORM_SQL_SELECT_NAME_BYTES); check_equal(strcmp(definition.name, name), 0);
    name[ORM_SQL_SELECT_NAME_BYTES] = 'a'; name[ORM_SQL_SELECT_NAME_BYTES + 1] = 0;
    (void)snprintf(sql, sizeof(sql), "CREATE INDEX `%s` ON items (id)", name);
    reject(sql, TURBODB_STATUS_LIMIT_EXCEEDED);
  }
  it("preserves occupied output and handles invalid inputs without allocation") {
    parse(basic_ddl); check_equal(bind_create(), TURBODB_STATUS_OK);
    const void *parts = vec_data_const(&definition.parts);
    const uint64_t work = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(bind_create(), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(vec_data_const(&definition.parts), parts); check_true(definition.unique);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], work);
    reset();
    check_equal(orm_tidesdb_sql_index_bind_create(NULL, &schema, &budget, &definition, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_index_bind_create(document, NULL, &budget, &definition, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_index_bind_create(document, &schema, NULL, &definition, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_index_bind_create(document, &schema, &budget, NULL, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(reserves, 0u);
  }
  it("refunds WORK for every allocation and resize failure") {
    parse(basic_ddl); check_equal(bind_create(), TURBODB_STATUS_OK);
    const size_t reserve_count = reserves, resize_count = resizes;
    check_true(reserve_count > 0); check_true(resize_count > 0);
    for (size_t i = 1; i <= reserve_count; ++i) {
      reset(); fail_reserve = i;
      check_equal(bind_create(), TURBODB_STATUS_OUT_OF_MEMORY); check_null(definition.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    for (size_t i = 1; i <= resize_count; ++i) {
      reset(); fail_resize = i;
      check_equal(bind_create(), TURBODB_STATUS_OUT_OF_MEMORY); check_null(definition.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
  }
  it("enforces every AST PLAN WORK and STEP boundary and accepts exact capacity") {
    parse(basic_ddl); check_equal(bind_create(), TURBODB_STATUS_OK);
    const orm_sql_budget_amount used = budget.used;
    const orm_sql_budget_limits original = limits;
    const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_AST_NODES, ORM_SQL_BUDGET_PLAN_NODES,
      ORM_SQL_BUDGET_WORK_BYTES, ORM_SQL_BUDGET_EXECUTION_STEPS};
    for (size_t r = 0; r < sizeof(resources) / sizeof(resources[0]); ++r) {
      const orm_sql_budget_resource resource = resources[r];
      for (uint64_t capacity = 1; capacity < used.value[resource]; ++capacity) {
        limits = original; limits.statement.value[resource] = capacity; reset();
        check_equal(bind_create(), TURBODB_STATUS_LIMIT_EXCEEDED); check_null(definition.budget);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
      }
      limits = original; limits.statement.value[resource] = used.value[resource]; reset();
      check_equal(bind_create(), TURBODB_STATUS_OK);
    }
  }
  it("allows optional diagnostics and idempotent destroy without refunding cumulative charges") {
    parse(basic_ddl);
    check_equal(orm_tidesdb_sql_index_bind_create(document, &schema, &budget, &definition, NULL), TURBODB_STATUS_OK);
    const uint64_t steps = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_index_destroy(&definition, NULL), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_index_destroy(&definition, NULL), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_index_destroy(NULL, NULL), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], steps);
  }
}
