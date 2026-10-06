#include "scan.h"
#include <tinytest.h>
#include <string.h>

static size_t reserve_calls, resize_calls, fail_reserve, fail_resize;
static stl_status scan_test_reserve(vec_t *v, size_t count) {
  return ++reserve_calls == fail_reserve ? STL_OUT_OF_MEMORY : vec_reserve(v, count);
}
static stl_status scan_test_resize(vec_t *v, size_t count) {
  return ++resize_calls == fail_resize ? STL_OUT_OF_MEMORY : vec_resize(v, count);
}
#define vec_reserve scan_test_reserve
#define vec_resize scan_test_resize
#include "../../src/work.c"
#include "../../src/expr.c"
#include "../../src/rows.c"
#include "../../src/scan.c"
#undef vec_reserve
#undef vec_resize

enum { TEST_ROWS = 5, TEST_COLUMNS = 3, TEST_LIMIT = 4096,
       TEST_WORK = 4 * 1024 * 1024, TEST_DEPTH = 32, MAX_ALLOCATIONS = 16 };
static orm_tidesdb_sql_budget budget;
static orm_sql_budget_limits limits;
static orm_sql_expr filter;
static orm_sql_scan scan;
static turbodb_error_t error;
static turbodb_value_t rows[TEST_ROWS][TEST_COLUMNS];
static orm_sql_type types[TEST_COLUMNS];
static size_t mapping[1], projection[2];
static orm_sql_memory_source source;
static orm_sql_scan_spec spec;

static void build_expression(const char *sql, bool predicate) {
  sqlparser_document *document = NULL;
  sqlparser_error parse_error;
  check_equal(sqlparser_parse(sql, strlen(sql), NULL, &document, &parse_error), SQLPARSER_OK);
  const sqlparser_node *statement = sqlparser_get_node(document, sqlparser_statements(document).first);
  const sqlparser_node *item = sqlparser_get_node(document, statement->as.select.columns.first);
  orm_sql_expr_input binding = {0};
  for (size_t i = 1; i <= sqlparser_node_count(document); ++i)
    if (sqlparser_get_node(document, (sqlparser_id)i)->kind == SQLPARSER_PARAMETER)
      binding = (orm_sql_expr_input){(sqlparser_id)i, {TURBODB_VALUE_INT64, true}};
  const turbodb_status_t status = predicate ?
      orm_tidesdb_sql_expr_compile(document, item->as.projection.expression, &binding, 1,
          TEST_DEPTH, &budget, &filter, &error) :
      orm_tidesdb_sql_expr_compile_value(document, item->as.projection.expression, &binding, 1,
          TEST_DEPTH, &budget, &filter, &error);
  check_equal(status, TURBODB_STATUS_OK);
  sqlparser_document_destroy(document);
}
static void build_filter(void) { build_expression("SELECT ? >= 20", true); }

static void reset_limits(void) {
  check_equal(orm_tidesdb_sql_scan_close(&scan, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_expr_destroy(&filter, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
  build_filter();
}

static turbodb_status_t open_scan(void) { return orm_tidesdb_sql_scan_open(&source, &spec, &budget, &scan, &error); }
static orm_sql_scan_row next_row(void) {
  orm_sql_scan_row row = {0};
  check_equal(orm_tidesdb_sql_scan_next(&scan, &row, &error), TURBODB_STATUS_OK);
  return row;
}

spec("TidesDB memory Filter Project Limit") {
  before_each() {
    reserve_calls = resize_calls = fail_reserve = fail_resize = 0;
    tdsql_error_init(&error);
    limits = (orm_sql_budget_limits){0};
    for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) limits.statement.value[i] = TEST_LIMIT;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = TEST_WORK;
    limits.transaction = (orm_sql_transaction_budget_amount){TEST_LIMIT, TEST_LIMIT, TEST_LIMIT};
    check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    filter = (orm_sql_expr){0}; scan = (orm_sql_scan){0};
    types[0] = (orm_sql_type){TURBODB_VALUE_INT64, false};
    types[1] = (orm_sql_type){TURBODB_VALUE_INT64, true};
    types[2] = (orm_sql_type){TURBODB_VALUE_TEXT, false};
    const int scores[] = {0, 20, 10, 30, 40};
    const char *names[] = {"a", "b", "c", "d", "e"};
    for (size_t i = 0; i < TEST_ROWS; ++i) {
      rows[i][0] = turbodb_i64((int64_t)i + 1);
      rows[i][1] = i ? turbodb_i64(scores[i]) : turbodb_null();
      rows[i][2] = turbodb_text(names[i]);
    }
    mapping[0] = 1; projection[0] = 2; projection[1] = 0;
    source = (orm_sql_memory_source){&rows[0][0], TEST_ROWS, TEST_COLUMNS, types};
    build_filter();
    spec = (orm_sql_scan_spec){&filter, mapping, 1, projection, 2, 0, UINT64_MAX};
  }
  after_each() {
    fail_reserve = fail_resize = 0;
    check_equal(orm_tidesdb_sql_scan_close(&scan, &error), TURBODB_STATUS_OK);
    check_equal(filter.active_runs, 0u);
    check_equal(orm_tidesdb_sql_expr_destroy(&filter, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
  }

  it("converts ordinary slots while retaining original validators and rejecting narrow targets") {
    orm_sql_type target[TEST_COLUMNS]; memcpy(target,types,sizeof(target)); target[0].kind=TURBODB_VALUE_DOUBLE;
    projection[0]=0; spec=(orm_sql_scan_spec){.projection=projection,.projection_count=1,.limit=UINT64_MAX,.coerce_types=target};
    const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    target[1].nullable=false; check_equal(open_scan(),TURBODB_STATUS_TYPE_ERROR);
    target[1]=types[1]; target[2].kind=TURBODB_VALUE_DOUBLE; check_equal(open_scan(),TURBODB_STATUS_UNSUPPORTED);
    target[2]=types[2]; check_equal(open_scan(),TURBODB_STATUS_OK);
    const size_t allocations[]={reserve_calls,resize_calls};
    const orm_sql_scan_row first=next_row(); check_equal(first.values[0].kind,TURBODB_VALUE_DOUBLE);
    check_equal(first.values[0].data.double_value,1.0); check_equal(rows[0][0].kind,TURBODB_VALUE_INT64);
    rows[1][0]=turbodb_f64(2.0); orm_sql_scan_row failed={.count=99};
    check_equal(orm_tidesdb_sql_scan_next(&scan,&failed,&error),TURBODB_STATUS_TYPE_ERROR); check_equal(failed.count,99u);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_scan_next(&scan,&failed,&error),TURBODB_STATUS_TYPE_ERROR);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
    check_equal(reserve_calls,allocations[0]); check_equal(resize_calls,allocations[1]);
    check_equal(orm_tidesdb_sql_scan_close(&scan,&error),TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
  }
  it("sorts and deduplicates converted tuples before applying pagination") {
    const int64_t edge=INT64_C(9007199254740992);
    orm_sql_type target[TEST_COLUMNS]; memcpy(target,types,sizeof(target)); target[0].kind=TURBODB_VALUE_DOUBLE;
    rows[0][0]=turbodb_i64(edge+1); rows[0][1]=turbodb_i64(0);
    rows[1][0]=turbodb_i64(edge); rows[1][1]=turbodb_i64(100);
    rows[2][0]=turbodb_i64(edge+1); rows[2][1]=turbodb_i64(100); source.rows=3;
    projection[0]=0; projection[1]=1;
    const orm_sql_scan_order order[]={{.slot=0},{.slot=1}};
    spec=(orm_sql_scan_spec){.projection=projection,.projection_count=2,.orders=order,.order_count=2,
      .distinct=true,.offset=1,.limit=1,.coerce_types=target};
    check_equal(open_scan(),TURBODB_STATUS_OK);
    const orm_sql_scan_row row=next_row(); check_equal(row.state,ORM_SQL_SCAN_ROW);
    check_equal(row.values[0].kind,TURBODB_VALUE_DOUBLE); check_equal(row.values[0].data.double_value,(double)edge);
    check_equal(row.values[1].data.int64_value,100); check_equal(next_row().state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],3u);
    check_equal(rows[0][0].data.int64_value,edge+1); check_equal(types[0].kind,TURBODB_VALUE_INT64);
  }

  it("rejects scalar filter programs even for empty sources and LIMIT zero") {
    check_equal(orm_tidesdb_sql_expr_destroy(&filter, &error), TURBODB_STATUS_OK);
    build_expression("SELECT ? + 1", false);
    source.rows = 0; spec.limit = 0;
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(open_scan(), TURBODB_STATUS_UNSUPPORTED); check_contains(error.message, "BOOL or NULL");
    check_null(scan.budget); check_equal(filter.active_runs, 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
  }

  it("validates computed mappings before opening and copies them for streaming") {
    check_equal(orm_tidesdb_sql_expr_destroy(&filter, &error), TURBODB_STATUS_OK);
    build_expression("SELECT ? + 1", false);
    orm_sql_scan_expression expressions[] = {{0}, {&filter, mapping, 1}};
    spec.filter = NULL; spec.filter_count = 0; spec.expressions = expressions;
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    mapping[0] = TEST_COLUMNS;
    check_equal(open_scan(), TURBODB_STATUS_INVALID_ARGUMENT);
    mapping[0] = 2; check_equal(open_scan(), TURBODB_STATUS_TYPE_ERROR);
    mapping[0] = 1; expressions[1].count = 0; check_equal(open_scan(), TURBODB_STATUS_INVALID_ARGUMENT);
    expressions[1].count = 1;
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
    check_equal(open_scan(), TURBODB_STATUS_OK);
    mapping[0] = SIZE_MAX; expressions[1].program = NULL;
    reserve_calls = resize_calls = 0; fail_reserve = fail_resize = 1;
    check_equal(next_row().values[1].kind, TURBODB_VALUE_NULL);
    check_equal(next_row().values[1].data.int64_value, 21);
    check_equal(reserve_calls, 0u); check_equal(resize_calls, 0u);
    check_equal(orm_tidesdb_sql_expr_destroy(&filter, &error), TURBODB_STATUS_BUSY);
  }

  it("filters UNKNOWN and FALSE before OFFSET and stops reading at LIMIT") {
    spec.offset = 1; spec.limit = 1;
    check_equal(open_scan(), TURBODB_STATUS_OK);
    const orm_sql_scan_row row = next_row();
    check_equal(row.state, ORM_SQL_SCAN_ROW); check_equal(row.count, 2u);
    check_equal(row.values[0].data.text_value.data, "d");
    check_equal(row.values[1].data.int64_value, 4);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 4u);
    check_equal(scan.position, 4u);
    check_equal(next_row().state, ORM_SQL_SCAN_DONE);
    check_equal(next_row().state, ORM_SQL_SCAN_DONE);
    check_equal(orm_tidesdb_sql_scan_cancel(&scan, &error), TURBODB_STATUS_OK);
    check_equal(next_row().state, ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 4u);
  }

  it("copies mappings at open and reuses all storage while streaming rows") {
    check_equal(open_scan(), TURBODB_STATUS_OK);
    mapping[0] = SIZE_MAX; projection[0] = SIZE_MAX;
    types[1] = (orm_sql_type){TURBODB_VALUE_BLOB, false};
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    reserve_calls = resize_calls = 0; fail_reserve = fail_resize = 1;
    const int expected_ids[] = {2, 4, 5};
    for (size_t i = 0; i < sizeof(expected_ids) / sizeof(expected_ids[0]); ++i) {
      const orm_sql_scan_row row = next_row();
      check_equal(row.state, ORM_SQL_SCAN_ROW);
      check_equal(row.values[1].data.int64_value, expected_ids[i]);
    }
    check_equal(next_row().state, ORM_SQL_SCAN_DONE);
    check_equal(reserve_calls, 0u); check_equal(resize_calls, 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
  }

  it("supports projection without a filter and preserves embedded NUL payloads") {
    const char payload[] = {'x', 0, 'y'};
    rows[0][2] = turbodb_text_v((vstr){payload, sizeof(payload)});
    spec.filter = NULL; spec.filter_count = 0; spec.limit = 1;
    check_equal(open_scan(), TURBODB_STATUS_OK);
    const orm_sql_scan_row row = next_row();
    check_equal(row.values[0].data.text_value.len, sizeof(payload));
    check_equal(memcmp(row.values[0].data.text_value.data, payload, sizeof(payload)), 0);
    check_equal(row.values[1].data.int64_value, 1);
    check_equal(next_row().state, ORM_SQL_SCAN_DONE);
  }

  it("reads no rows for LIMIT zero or an empty source but still validates schema") {
    spec.limit = 0;
    types[1].kind = TURBODB_VALUE_TEXT;
    check_equal(open_scan(), TURBODB_STATUS_TYPE_ERROR);
    types[1].kind = TURBODB_VALUE_INT64;
    check_equal(open_scan(), TURBODB_STATUS_OK);
    check_equal(next_row().state, ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
    check_equal(orm_tidesdb_sql_scan_close(&scan, &error), TURBODB_STATUS_OK);
    spec.limit = UINT64_MAX; source.rows = 0; source.values = NULL;
    check_equal(open_scan(), TURBODB_STATUS_OK);
    check_equal(next_row().state, ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], 0u);
  }

  it("does not add OFFSET and LIMIT when both are maximal") {
    spec.offset = UINT64_MAX; spec.limit = UINT64_MAX;
    check_equal(open_scan(), TURBODB_STATUS_OK);
    check_equal(next_row().state, ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], TEST_ROWS);
    check_equal(scan.offset_left, UINT64_MAX - 3);
  }

  it("charges rejected rows against read limits and holds the first error terminal") {
    limits.statement.value[ORM_SQL_BUDGET_READ_ROWS] = 1; reset_limits();
    check_equal(open_scan(), TURBODB_STATUS_OK);
    orm_sql_scan_row row = {ORM_SQL_SCAN_CANCELLED, NULL, 0};
    check_equal(orm_tidesdb_sql_scan_next(&scan, &row, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(row.state, ORM_SQL_SCAN_CANCELLED);
    check_equal(scan.position, 1u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 1u);
    const turbodb_error_t first = error;
    const uint64_t steps = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_scan_cancel(&scan, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_scan_next(&scan, &row, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(error.message, first.message);
    check_equal(scan.state, ORM_SQL_SCAN_ERROR);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], steps);
  }

  it("counts fixed slots and text payload exactly before exposing a row") {
    const uint64_t row_bytes = TEST_COLUMNS * sizeof(turbodb_value_t) + 1;
    limits.statement.value[ORM_SQL_BUDGET_READ_BYTES] = row_bytes; reset_limits();
    spec.filter = NULL; spec.filter_count = 0;
    check_equal(open_scan(), TURBODB_STATUS_OK);
    check_equal(next_row().state, ORM_SQL_SCAN_ROW);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_BYTES], row_bytes);
    orm_sql_scan_row row = {0};
    check_equal(orm_tidesdb_sql_scan_next(&scan, &row, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 1u);
    check_equal(budget.transaction_used.read_bytes, row_bytes);
  }

  it("preserves predicate errors and never resumes scanning after type failure") {
    rows[1][1] = turbodb_text("wrong type");
    check_equal(open_scan(), TURBODB_STATUS_OK);
    orm_sql_scan_row row = {0};
    check_equal(orm_tidesdb_sql_scan_next(&scan, &row, &error), TURBODB_STATUS_TYPE_ERROR);
    check_equal(scan.position, 1u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 2u);
    check_equal(orm_tidesdb_sql_scan_next(&scan, &row, NULL), TURBODB_STATUS_TYPE_ERROR);
    check_equal(scan.position, 1u);
  }

  it("does not publish a partially validated projection") {
    const unsigned char invalid[] = {0xc0, 0x80};
    rows[1][2] = turbodb_text_v((vstr){(const char *)invalid, sizeof(invalid)});
    projection[0] = 0; projection[1] = 2;
    check_equal(open_scan(), TURBODB_STATUS_OK);
    orm_sql_scan_row row = {ORM_SQL_SCAN_DONE, NULL, 0};
    check_equal(orm_tidesdb_sql_scan_next(&scan, &row, &error), TURBODB_STATUS_TYPE_ERROR);
    check_equal(row.state, ORM_SQL_SCAN_DONE);
    for (size_t i = 0; i < vec_size(&scan.output); ++i)
      check_equal(((const turbodb_value_t *)vec_at_const(&scan.output, i))->kind, TURBODB_VALUE_NULL);
  }

  it("cancels idempotently and retains the program until scan close") {
    check_equal(open_scan(), TURBODB_STATUS_OK);
    check_equal(next_row().state, ORM_SQL_SCAN_ROW);
    const uint64_t read = budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(orm_tidesdb_sql_expr_destroy(&filter, &error), TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_scan_cancel(&scan, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_scan_cancel(&scan, &error), TURBODB_STATUS_OK);
    check_equal(next_row().state, ORM_SQL_SCAN_CANCELLED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], read);
    check_equal(orm_tidesdb_sql_scan_close(&scan, &error), TURBODB_STATUS_OK);
    check_equal(filter.active_runs, 0u);
    check_equal(orm_tidesdb_sql_scan_close(&scan, &error), TURBODB_STATUS_OK);
  }

  it("cleans every selected open allocation failure and publishes no partial run") {
    check_equal(open_scan(), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_scan_close(&scan, &error), TURBODB_STATUS_OK);
    reserve_calls = resize_calls = 0;
    check_equal(open_scan(), TURBODB_STATUS_OK);
    const size_t allocations = reserve_calls, mutations = resize_calls;
    check_greater(allocations, 0u); check_less(allocations, (size_t)MAX_ALLOCATIONS);
    check_less(mutations, (size_t)MAX_ALLOCATIONS);
    check_equal(orm_tidesdb_sql_scan_close(&scan, &error), TURBODB_STATUS_OK);
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for (size_t i = 1; i <= allocations; ++i) {
      reserve_calls = 0; fail_reserve = i;
      check_equal(open_scan(), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(scan.budget); check_equal(filter.active_runs, 0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
    }
    fail_reserve = 0;
    for (size_t i = 1; i <= mutations; ++i) {
      resize_calls = 0; fail_resize = i;
      check_equal(open_scan(), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(scan.budget); check_equal(filter.active_runs, 0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
    }
    fail_resize = 0;
    check_equal(open_scan(), TURBODB_STATUS_OK);
    check_equal(next_row().state, ORM_SQL_SCAN_ROW);
  }

  it("rejects invalid shapes and projections before reading or allocating scan state") {
    projection[0] = TEST_COLUMNS;
    check_equal(open_scan(), TURBODB_STATUS_INVALID_ARGUMENT);
    projection[0] = 2; mapping[0] = TEST_COLUMNS;
    check_equal(open_scan(), TURBODB_STATUS_INVALID_ARGUMENT);
    mapping[0] = 1; source.rows = SIZE_MAX;
    check_equal(open_scan(), TURBODB_STATUS_LIMIT_EXCEEDED);
    source.rows = TEST_ROWS; types[0] = (orm_sql_type){TURBODB_VALUE_NULL, false};
    check_equal(open_scan(), TURBODB_STATUS_TYPE_ERROR);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
    check_null(scan.budget);
  }

  it("fails work admission without taking a filter lease or consuming rows") {
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const uint64_t remaining = limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] - retained;
    orm_sql_budget_amount full = {0}; full.value[ORM_SQL_BUDGET_WORK_BYTES] = remaining;
    check_equal(orm_tidesdb_sql_budget_reserve(&budget, &full, &error), TURBODB_STATUS_OK);
    check_equal(open_scan(), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(scan.budget); check_equal(filter.active_runs, 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]);
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_WORK_BYTES, remaining, &error), TURBODB_STATUS_OK);
    check_equal(open_scan(), TURBODB_STATUS_OK);
  }

  it("rejects step exhaustion atomically with read accounting") {
    limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1; reset_limits();
    check_equal(open_scan(), TURBODB_STATUS_OK);
    orm_sql_scan_row row = {0};
    check_equal(orm_tidesdb_sql_scan_next(&scan, &row, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_BYTES], 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], 0u);
    check_equal(scan.state, ORM_SQL_SCAN_ERROR);
  }

  it("rejects row-byte overflow before accessing payload memory") {
    rows[0][2] = turbodb_text_v((vstr){"x", SIZE_MAX});
    check_equal(open_scan(), TURBODB_STATUS_OK);
    orm_sql_scan_row row = {0};
    check_equal(orm_tidesdb_sql_scan_next(&scan, &row, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
    check_equal(scan.position, 0u);
  }

  it("keeps parameter input slots separate from row schema and projection bounds") {
    orm_sql_type parameter_type = {TURBODB_VALUE_INT64, false};
    turbodb_value_t parameter = turbodb_i64(30);
    spec.parameters = &parameter; spec.parameter_types = &parameter_type; spec.parameter_count = 1;
    mapping[0] = TEST_COLUMNS;
    check_equal(open_scan(), TURBODB_STATUS_OK);
    parameter = turbodb_i64(0);
    check_equal(next_row().values[1].data.int64_value, 1);
    check_equal(orm_tidesdb_sql_scan_close(&scan, &error), TURBODB_STATUS_OK);
    spec.limit = 0;
    parameter_type.kind = TURBODB_VALUE_TEXT; parameter = turbodb_text("x");
    check_equal(open_scan(), TURBODB_STATUS_TYPE_ERROR);
    parameter_type.kind = TURBODB_VALUE_INT64; parameter = turbodb_i64(30);
    mapping[0] = TEST_COLUMNS + 1;
    check_equal(open_scan(), TURBODB_STATUS_INVALID_ARGUMENT);
    mapping[0] = TEST_COLUMNS; projection[0] = TEST_COLUMNS;
    check_equal(open_scan(), TURBODB_STATUS_INVALID_ARGUMENT);
    projection[0] = 2; spec.parameter_count = SIZE_MAX;
    check_equal(open_scan(), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(scan.budget); check_equal(filter.active_runs, 0u);
  }
}
