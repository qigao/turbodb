#include "expr.h"
#include <tinytest.h>
#include <string.h>
#include <math.h>

/* Selected container call failures stay within this test TU. Production and
 * Salts allocators are unchanged; this is not an all-allocation fault matrix. */
static size_t reserve_calls, fail_reserve, resize_calls, fail_resize;
static stl_status test_reserve(vec_t *vector, size_t count) {
  return ++reserve_calls == fail_reserve ? STL_OUT_OF_MEMORY : vec_reserve(vector, count);
}
static stl_status test_resize(vec_t *vector, size_t count) {
  return ++resize_calls == fail_resize ? STL_OUT_OF_MEMORY : vec_resize(vector, count);
}
#define vec_reserve test_reserve
#define vec_resize test_resize
#include "../../src/work.c"
#include "../../src/expr.c"
#undef vec_reserve
#undef vec_resize

enum { TEST_NODES = 4096, TEST_BYTES = 4 * 1024 * 1024, TEST_DEPTH = 1024,
       TEST_STEPS = 100000, TEST_INPUTS = 8, SENTINEL = 73 };
static orm_tidesdb_sql_budget budget;
static orm_sql_budget_limits limits;
static orm_sql_expr program;
static turbodb_error_t error;
static sqlparser_document *document;
static sqlparser_id root;
static orm_sql_expr_input bindings[TEST_INPUTS];
static size_t binding_count;
static orm_sql_diagnostics numeric_diagnostics;

static void reset_budget(void) {
  check_equal(orm_tidesdb_sql_expr_destroy(&program, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
}

static void parse_mode(const char *sql, bool no_backslash) {
  sqlparser_document_destroy(document); document = NULL;
  sqlparser_error parse_error;
  const sqlparser_options options = {SQLPARSER_MYSQL, no_backslash};
  check_equal(sqlparser_parse_with_options(sql, strlen(sql), &options, NULL, &document, &parse_error), SQLPARSER_OK);
  const sqlparser_node *statement = sqlparser_get_node(document, sqlparser_statements(document).first);
  check_not_null(statement);
  check_equal(statement->kind, SQLPARSER_SELECT);
  const sqlparser_node *projection = sqlparser_get_node(document, statement->as.select.columns.first);
  check_not_null(projection);
  root = projection->as.projection.expression;
  binding_count = 0;
  bool function_names[TEST_NODES + 1] = {0};
  check_equal(sqlparser_node_count(document) <= TEST_NODES, true);
  for (size_t i = 1; i <= sqlparser_node_count(document); ++i) {
    const sqlparser_node *node = sqlparser_get_node(document, (sqlparser_id)i);
    if (node->kind == SQLPARSER_CALL) function_names[node->as.call.name] = true;
    if (node->kind == SQLPARSER_TYPE) function_names[node->as.type.name] = true;
  }
  for (size_t i = 1; i <= sqlparser_node_count(document); ++i) {
    const sqlparser_node *node = sqlparser_get_node(document, (sqlparser_id)i);
    if ((node->kind == SQLPARSER_NAME && !function_names[i]) || node->kind == SQLPARSER_PARAMETER) {
      check_less(binding_count, (size_t)TEST_INPUTS);
      bindings[binding_count++] = (orm_sql_expr_input){(sqlparser_id)i, {TURBODB_VALUE_BOOLEAN, true}};
    }
  }
}
static void parse(const char *sql) { parse_mode(sql, true); }

static turbodb_status_t compile(void) {
  return orm_tidesdb_sql_expr_compile(document, root, bindings, binding_count,
                                      TEST_DEPTH, &budget, &program, &error);
}

static turbodb_value_t run(const turbodb_value_t *values, size_t count) {
  turbodb_value_t result = turbodb_i64(SENTINEL);
  check_equal(orm_tidesdb_sql_expr_eval(&program, values, count, &result, &error), TURBODB_STATUS_OK);
  return result;
}

static void expect_truth(turbodb_value_t result, int truth) {
  if (truth < 0) check_equal(result.kind, TURBODB_VALUE_NULL);
  else {
    check_equal(result.kind, TURBODB_VALUE_BOOLEAN);
    check_equal(result.data.boolean_value, truth);
  }
}

spec("TidesDB bounded AST predicate program") {
  before_each() {
    tdsql_error_init(&error);
    reserve_calls = fail_reserve = resize_calls = fail_resize = 0;
    limits = (orm_sql_budget_limits){0};
    for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) limits.statement.value[i] = TEST_NODES;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = TEST_BYTES;
    limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = TEST_STEPS;
    limits.transaction = (orm_sql_transaction_budget_amount){TEST_NODES, TEST_NODES, TEST_NODES};
    check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    program = (orm_sql_expr){0};
  }
  after_each() {
    orm_sql_diagnostics_destroy(&numeric_diagnostics);
    fail_reserve = fail_resize = 0;
    check_equal(orm_tidesdb_sql_expr_destroy(&program, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document = NULL;
  }

  it("reads a copied session snapshot after the AST dies and refuses missing context") {
    parse("SELECT @@SESSION.autocommit + @@LOCAL.transaction_read_only");
    check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
    check_true(program.uses_session); check_equal(program.input_count,0u);
    turbodb_value_t output=turbodb_i64(SENTINEL);
    check_equal(orm_tidesdb_sql_expr_eval(&program,NULL,0,&output,&error),TURBODB_STATUS_UNSUPPORTED);
    check_equal(output.data.int64_value,SENTINEL); check_equal(program.active_runs,0u);
    sqlparser_document_destroy(document); document=NULL;
    orm_sql_evaluation evaluation={.session={.valid=true,.autocommit=true,.read_only=true}};
    orm_sql_expr_run run={0};
    check_equal(orm_tidesdb_sql_expr_run_open_evaluation(&program,evaluation,&run,&error),TURBODB_STATUS_OK);
    evaluation.session=(orm_sql_session_snapshot){0};
    check_equal(orm_tidesdb_sql_expr_run_eval(&run,NULL,0,&output,&error),TURBODB_STATUS_OK);
    check_equal(output.kind,TURBODB_VALUE_INT64); check_equal(output.data.int64_value,2);
    check_equal(orm_tidesdb_sql_expr_run_close(&run,&error),TURBODB_STATUS_OK);
  }
  it("binds supported variable spellings and rejects unknown variables in dead branches") {
    const char *const accepted[]={"SELECT @@AuToCoMmIt=1","SELECT @@SESSION . `transaction_read_only`=0",
      "SELECT @@LoCaL.transaction_isolation='SERIALIZABLE'"};
    for (size_t i=0;i<sizeof(accepted)/sizeof(accepted[0]);++i) {
      reset_budget(); parse(accepted[i]); check_equal(binding_count,0u); check_equal(compile(),TURBODB_STATUS_OK);
      turbodb_value_t output=turbodb_null();
      const orm_sql_evaluation evaluation={.session={.valid=true,.autocommit=true}};
      check_equal(orm_tidesdb_sql_expr_eval_evaluation(&program,NULL,0,
          evaluation,&output,&error),TURBODB_STATUS_OK);
      expect_truth(output,1);
    }
    const char *const rejected[]={"SELECT @@GLOBAL.autocommit=1","SELECT @autocommit=1",
      "SELECT CASE WHEN FALSE THEN @@sql_mode ELSE 1 END", "SELECT COALESCE(1,@@unknown)"};
    for (size_t i=0;i<sizeof(rejected)/sizeof(rejected[0]);++i) {
      reset_budget(); parse(rejected[i]);
      check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_UNSUPPORTED);
      check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    }
  }
  it("exposes the Connector/J initialization aliases and metadata values") {
    const char *const sql[]={
      "SELECT @@tx_isolation='SERIALIZABLE'",
      "SELECT @@query_cache_size=0",
      "SELECT @@query_cache_type='OFF'",
      "SELECT @@sql_mode='NO_BACKSLASH_ESCAPES,STRICT_TRANS_TABLES'",
      "SELECT @@max_allowed_packet=4096",
      "SELECT @@character_set_results IS NULL"
    };
    const orm_sql_evaluation evaluation={
      .session={.valid=true,.autocommit=true,.max_allowed_packet=4096}
    };
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      reset_budget(); parse(sql[i]); check_equal(binding_count,0u);
      check_equal(compile(),TURBODB_STATUS_OK);
      turbodb_value_t output=turbodb_null();
      check_equal(orm_tidesdb_sql_expr_eval_evaluation(&program,NULL,0,
          evaluation,&output,&error),TURBODB_STATUS_OK);
      expect_truth(output,1);
    }
  }
  it("includes resolved variable identity in expression equality") {
    parse("SELECT @@autocommit=1"); check_equal(compile(),TURBODB_STATUS_OK);
    orm_sql_expr other={0}; bool same=true;
    parse("SELECT @@transaction_read_only=1");
    check_equal(orm_tidesdb_sql_expr_compile(document,root,NULL,0,TEST_DEPTH,&budget,&other,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_expr_same(&program,NULL,&other,NULL,&same,&error),TURBODB_STATUS_OK); check_false(same);
    check_equal(orm_tidesdb_sql_expr_destroy(&other,&error),TURBODB_STATUS_OK);
    parse("SELECT @@LOCAL.autocommit=1");
    check_equal(orm_tidesdb_sql_expr_compile(document,root,NULL,0,TEST_DEPTH,&budget,&other,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_expr_same(&program,NULL,&other,NULL,&same,&error),TURBODB_STATUS_OK); check_true(same);
    check_equal(orm_tidesdb_sql_expr_destroy(&other,&error),TURBODB_STATUS_OK);
  }

  group("numeric CAST programs") {
    it("selects FLOAT or DOUBLE for every binary precision endpoint and owns its metadata") {
      enum { SQL_CAPACITY=96 };
      char sql[SQL_CAPACITY];
      for(unsigned p=0;p<=ORM_SQL_CAST_DOUBLE_PRECISION;++p) {
        reset_budget();
        (void)snprintf(sql,sizeof(sql),"SELECT CAST(16777217 AS FLOAT(%u))",p);
        parse(sql); check_equal(binding_count,(size_t)0);
        check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
        check_equal(program.result.kind,TURBODB_VALUE_DOUBLE); check_false(program.result.nullable);
        sqlparser_document_destroy(document); document=NULL;
        turbodb_value_t value=run(NULL,0);
        check_equal(value.data.double_value,p<=ORM_SQL_CAST_FLOAT_PRECISION ? 16777216.0 : 16777217.0);
      }
      const struct { const char *sql; double expected; } cases[]={
        {"SELECT CAST(16777217 AS FLOAT)",16777216.0},
        {"SELECT CAST(16777217 AS FLOAT(00025))",16777217.0},
        {"SELECT CAST(16777217 AS DOUBLE /* precision */ PRECISION)",16777217.0},
        {"SELECT CAST(CAST(16777217 AS FLOAT) AS DOUBLE)",16777216.0}
      };
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        reset_budget(); parse(cases[i].sql);
        check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
        check_equal(run(NULL,0).data.double_value,cases[i].expected);
      }
    }
    it("rejects invalid FLOAT precision even when runtime evaluation is pruned") {
      const char *sql[]={"SELECT CAST(NULL AS FLOAT(54))","SELECT CAST(1 AS FLOAT(-1))",
        "SELECT CAST(1 AS FLOAT(1,2))","SELECT CAST(1 AS FLOAT(?))","SELECT CAST(1 AS FLOAT(1+2))",
        "SELECT CAST(1 AS FLOAT(2.5))","SELECT CAST(1 AS FLOAT(999999999999999999999999999999999999))",
        "SELECT CASE WHEN TRUE THEN 7.0 ELSE CAST(1 AS FLOAT(54)) END"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        reset_budget(); parse(sql[i]);
        check_equal(orm_tidesdb_sql_expr_compile_value(document,root,bindings,binding_count,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_SQL_ERROR);
        check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],UINT64_C(0));
      }
      reset_budget(); parse("SELECT CAST(1e39 AS FLOAT)");
      check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
      turbodb_value_t out=turbodb_i64(SENTINEL);
      const orm_sql_evaluation ignore={NULL,ORM_SQL_EVALUATION_IGNORE_WRITE};
      check_equal(orm_tidesdb_sql_expr_eval_evaluation(&program,NULL,0,ignore,&out,&error),TURBODB_STATUS_OUT_OF_RANGE);
      check_equal(out.data.int64_value,(int64_t)SENTINEL);
    }
    it("compares physical CAST precision rather than just the F64 result kind") {
      const struct { const char *left,*right; bool same; } cases[]={
        {"SELECT CAST(16777217 AS FLOAT)","SELECT CAST(16777217 AS DOUBLE)",false},
        {"SELECT CAST(16777217 AS FLOAT(0))","SELECT CAST(16777217 AS FLOAT(24))",true},
        {"SELECT CAST(16777217 AS FLOAT(25))","SELECT CAST(16777217 AS DOUBLE PRECISION)",true}
      };
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        reset_budget(); parse(cases[i].left);
        check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
        orm_sql_expr left=program; program=(orm_sql_expr){0}; parse(cases[i].right);
        check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
        bool same=false;
        check_equal(orm_tidesdb_sql_expr_same(&left,NULL,&program,NULL,&same,&error),TURBODB_STATUS_OK); check_equal(same,cases[i].same);
        check_equal(orm_tidesdb_sql_expr_destroy(&left,&error),TURBODB_STATUS_OK);
      }
    }
    it("charges precision bytes before decoding and preserves the target below admission") {
      parse("SELECT CAST(1 AS FLOAT(00025))");
      const sqlparser_node *cast=sqlparser_get_node(document,root);
      const uint64_t before=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      enum { PRECISION_WORK=sizeof("00025") };
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=before+PRECISION_WORK-1;
      orm_sql_cast_target target=ORM_SQL_CAST_SIGNED;
      check_equal(orm_tidesdb_sql_expr_resolve_cast(document,cast,&budget,&target,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(target,ORM_SQL_CAST_SIGNED); check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],before);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=before+PRECISION_WORK;
      check_equal(orm_tidesdb_sql_expr_resolve_cast(document,cast,&budget,&target,&error),TURBODB_STATUS_OK);
      check_equal(target,ORM_SQL_CAST_DOUBLE);
      check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],before+PRECISION_WORK);
    }
    it("owns CAST constants after AST destruction and binds explicit result metadata") {
      const struct { const char *sql; turbodb_value_kind_t kind; int64_t integer; double real; } cases[]={
        {"SELECT CAST('12.9e3' AS SIGNED INTEGER)",TURBODB_VALUE_INT64,12,0},
        {"SELECT CAST(-1 AS UNSIGNED /* word */ INTEGER)",TURBODB_VALUE_UINT64,-1,0},
        {"SELECT CAST(CAST(-1 AS UNSIGNED) AS SIGNED)",TURBODB_VALUE_INT64,-1,0},
        {"SELECT CAST('12.9e3' AS DOUBLE)",TURBODB_VALUE_DOUBLE,0,12900.0},
        {"SELECT CAST(TRUE AS REAL)",TURBODB_VALUE_DOUBLE,0,1.0},
        {"SELECT CAST(NULL AS SIGNED)",TURBODB_VALUE_NULL,0,0}
      };
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        reset_budget(); parse(cases[i].sql); check_equal(binding_count,(size_t)0);
        check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
        sqlparser_document_destroy(document); document=NULL;
        turbodb_value_t value=run(NULL,0); check_equal(value.kind,cases[i].kind);
        if(value.kind==TURBODB_VALUE_INT64) check_equal(value.data.int64_value,cases[i].integer);
        if(value.kind==TURBODB_VALUE_UINT64) check_equal(value.data.uint64_value,(uint64_t)cases[i].integer);
        if(value.kind==TURBODB_VALUE_DOUBLE) check_equal(value.data.double_value,cases[i].real);
      }
    }
    it("promotes only truncation in strict writes and counts both query warnings") {
      check_equal(orm_sql_diagnostics_init(&numeric_diagnostics,1,&error),TURBODB_STATUS_OK);
      parse("SELECT CAST(? AS UNSIGNED)"); bindings[0].type=(orm_sql_type){TURBODB_VALUE_TEXT,false};
      check_equal(orm_tidesdb_sql_expr_compile_value(document,root,bindings,binding_count,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
      turbodb_value_t value=turbodb_text("-1.9"),out=turbodb_i64(SENTINEL);
      const orm_sql_evaluation query={&numeric_diagnostics,ORM_SQL_EVALUATION_QUERY};
      const orm_sql_evaluation strict={NULL,ORM_SQL_EVALUATION_WRITE},
          observed_strict={&numeric_diagnostics,ORM_SQL_EVALUATION_WRITE};
      check_equal(orm_tidesdb_sql_expr_eval_evaluation(&program,&value,1,query,&out,&error),TURBODB_STATUS_OK);
      check_equal(out.data.uint64_value,UINT64_MAX); check_equal(numeric_diagnostics.total,UINT64_C(2));
      check_equal(orm_sql_diagnostics_at(&numeric_diagnostics,0)->code,(uint32_t)1292);
      check_equal(orm_sql_diagnostics_reset(&numeric_diagnostics,&error),TURBODB_STATUS_OK);
      out=turbodb_i64(SENTINEL);
      check_equal(orm_tidesdb_sql_expr_eval_evaluation(&program,&value,1,
          strict,&out,&error),TURBODB_STATUS_SQL_ERROR);
      check_equal(out.data.int64_value,(int64_t)SENTINEL);
      value=turbodb_text("-1");
      check_equal(orm_tidesdb_sql_expr_eval_evaluation(&program,&value,1,
          observed_strict,&out,&error),TURBODB_STATUS_OK);
      check_equal(out.data.uint64_value,UINT64_MAX);
      check_equal(orm_sql_diagnostics_at(&numeric_diagnostics,0)->code,(uint32_t)1105);
      numeric_diagnostics.total=UINT64_MAX; out=turbodb_i64(SENTINEL);
      check_equal(orm_tidesdb_sql_expr_eval_evaluation(&program,&value,1,query,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(out.data.int64_value,(int64_t)SENTINEL);
    }
    it("skips conversion warnings in lazy branches but rejects unsupported targets while binding") {
      const char *sql[]={"SELECT CASE WHEN TRUE THEN 7 ELSE CAST('bad' AS SIGNED) END=7",
        "SELECT COALESCE(7,CAST('bad' AS SIGNED))=7","SELECT FALSE AND CAST('bad' AS SIGNED)=0"};
      check_equal(orm_sql_diagnostics_init(&numeric_diagnostics,2,&error),TURBODB_STATUS_OK);
      const orm_sql_evaluation strict={&numeric_diagnostics,ORM_SQL_EVALUATION_WRITE};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        reset_budget(); parse(sql[i]); check_equal(compile(),TURBODB_STATUS_OK);
        turbodb_value_t out=turbodb_null();
        check_equal(orm_tidesdb_sql_expr_eval_evaluation(&program,NULL,0,
            strict,&out,&error),TURBODB_STATUS_OK);
        expect_truth(out,i<2); check_equal(numeric_diagnostics.total,UINT64_C(0));
      }
      const char *invalid[]={"SELECT CASE WHEN TRUE THEN 7 ELSE CAST(1 AS CHAR) END",
        "SELECT CAST(1 AS SIGNED(1))","SELECT CAST(1 AS DOUBLE UNSIGNED)"};
      for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        reset_budget(); parse(invalid[i]);
        check_equal(orm_tidesdb_sql_expr_compile_value(document,root,bindings,binding_count,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_UNSUPPORTED);
        check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],UINT64_C(0));
      }
    }
    it("refunds CAST compiler allocation failures and preserves output below execution admission") {
      const char *sql[]={"SELECT CAST(CAST(? AS UNSIGNED INTEGER) AS SIGNED)=12",
        "SELECT CAST(CAST(? AS FLOAT(00024)) AS DOUBLE PRECISION)=12.0"};
      for(size_t mode=0;mode<sizeof(sql)/sizeof(sql[0]);++mode) {
        reset_budget(); parse(sql[mode]);
        bindings[0].type=(orm_sql_type){TURBODB_VALUE_TEXT,false};
        reserve_calls=resize_calls=0; check_equal(compile(),TURBODB_STATUS_OK);
        const size_t counts[]={reserve_calls,resize_calls};
        for(size_t phase=0;phase<2;++phase) for(size_t point=1;point<=counts[phase];++point) {
          fail_reserve=fail_resize=0; reset_budget(); reserve_calls=resize_calls=0;
          if(phase) fail_resize=point; else fail_reserve=point;
          check_equal(compile(),TURBODB_STATUS_OUT_OF_MEMORY); check_null(program.budget);
          check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],UINT64_C(0));
        }
        fail_reserve=fail_resize=0; reset_budget(); check_equal(compile(),TURBODB_STATUS_OK);
        turbodb_value_t value=turbodb_text("12"),out=turbodb_i64(SENTINEL);
        budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
        check_equal(orm_tidesdb_sql_expr_eval(&program,&value,1,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
        check_equal(out.data.int64_value,(int64_t)SENTINEL);
        budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=TEST_STEPS; expect_truth(run(&value,1),1);
      }
    }
  }

  group("mixed numeric conditional results") {
    it("aggregates all numeric branch orders and returns stable real metadata after AST destruction") {
      const turbodb_value_t numbers[]={turbodb_i64(-7),turbodb_u64(UINT64_MAX),turbodb_f64(2.5)};
      const double expected[]={-7.0,18446744073709551616.0,2.5};
      const size_t orders[][3]={{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
      for(size_t mode=0;mode<2;++mode) for(size_t order=0;order<sizeof(orders)/sizeof(orders[0]);++order) {
        reset_budget();parse(mode?"SELECT COALESCE(?,?,?)":"SELECT CASE ? WHEN 0 THEN ? WHEN 1 THEN ? ELSE ? END");
        if(!mode) bindings[0].type=(orm_sql_type){TURBODB_VALUE_INT64,false};
        for(size_t i=0;i<3;++i) bindings[i+(mode?0:1)].type=(orm_sql_type){numbers[orders[order][i]].kind,true};
        check_equal(orm_tidesdb_sql_expr_compile_value(document,root,bindings,binding_count,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
        check_equal(program.result.kind,TURBODB_VALUE_DOUBLE);check_true(program.result.nullable);
        sqlparser_document_destroy(document);document=NULL;
        for(size_t chosen=0;chosen<=3;++chosen) {
          turbodb_value_t values[4]={0};
          if(!mode) values[0]=turbodb_i64((int64_t)chosen);
          for(size_t i=0;i<3;++i) values[i+(mode?0:1)]=i!=chosen?turbodb_null():numbers[orders[order][i]];
          const turbodb_value_t value=run(values,binding_count);
          check_equal(value.kind,chosen==3?TURBODB_VALUE_NULL:TURBODB_VALUE_DOUBLE);
          if(chosen<3) check_equal(value.data.double_value,expected[orders[order][chosen]]);
        }
      }
    }
    it("preserves NULL rules nested selection and exact NULLIF result types") {
      const struct { const char *sql; double expected; bool nullable; bool null_result; } cases[]={
        {"SELECT CASE WHEN TRUE THEN 9007199254740993 ELSE 0.0 END",9007199254740992.0,false,false},
        {"SELECT CASE WHEN FALSE THEN 7 WHEN FALSE THEN 2.5 END",0.0,true,true},
        {"SELECT CASE WHEN TRUE THEN NULL ELSE 2.5 END",0.0,true,true},
        {"SELECT COALESCE(NULL,7,18446744073709551615,0.0)",7.0,false,false},
        {"SELECT IFNULL(18446744073709551615,0.0)",18446744073709551616.0,false,false},
        {"SELECT COALESCE(CASE WHEN TRUE THEN 7 ELSE NULL END,IFNULL(NULL,2.5))",7.0,false,false},
        {"SELECT CASE 7.0 WHEN 7 THEN IFNULL(NULL,CASE WHEN TRUE THEN 3 ELSE 0.5 END) ELSE 0 END",3.0,false,false},
        {"SELECT IFNULL(NULLIF(7,8.0),0.5)",7.0,false,false}
      };
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        reset_budget();parse(cases[i].sql);
        check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
        check_equal(program.result.kind,TURBODB_VALUE_DOUBLE);check_equal(program.result.nullable,cases[i].nullable);
        const turbodb_value_t value=run(NULL,0);
        check_equal(value.kind,cases[i].null_result?TURBODB_VALUE_NULL:TURBODB_VALUE_DOUBLE);
        if(!cases[i].null_result) check_equal(value.data.double_value,cases[i].expected);
      }
      reset_budget();parse("SELECT NULLIF(7,8.0)");
      check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
      check_equal(program.result.kind,TURBODB_VALUE_INT64);check_equal(run(NULL,0).data.int64_value,7);
      const char *rejected[]={"SELECT COALESCE(-1,18446744073709551615)",
        "SELECT CASE WHEN TRUE THEN -1 WHEN FALSE THEN 18446744073709551615 END",
        "SELECT CASE WHEN TRUE THEN 7 ELSE '7' END","SELECT COALESCE(7,TRUE,0.0)",
        "SELECT IFNULL(1.5,'7')","SELECT COALESCE(7,0.0,UNSUPPORTED_FN(1))"};
      for(size_t i=0;i<sizeof(rejected)/sizeof(rejected[0]);++i) {
        reset_budget();parse(rejected[i]);
        check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_UNSUPPORTED);
        check_null(program.budget);check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
      }
    }
    it("validates selected original kinds and keeps zero policies lazy without execution allocation") {
      parse("SELECT CASE WHEN ? THEN ? ELSE ? END");
      bindings[0].type=(orm_sql_type){TURBODB_VALUE_BOOLEAN,false};
      bindings[1].type=(orm_sql_type){TURBODB_VALUE_INT64,true};bindings[2].type=(orm_sql_type){TURBODB_VALUE_DOUBLE,true};
      check_equal(orm_tidesdb_sql_expr_compile_value(document,root,bindings,binding_count,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
      orm_sql_expr_run workspace={0};check_equal(orm_tidesdb_sql_expr_run_open(&program,&workspace,&error),TURBODB_STATUS_OK);
      turbodb_value_t values[]={turbodb_bool(1),turbodb_i64(7),turbodb_f64(NAN)},out=turbodb_i64(SENTINEL);
      const size_t allocations[]={reserve_calls,resize_calls};fail_reserve=reserve_calls+1;fail_resize=resize_calls+1;
      check_equal(orm_tidesdb_sql_expr_run_eval(&workspace,values,binding_count,&out,&error),TURBODB_STATUS_OK);
      check_equal(out.kind,TURBODB_VALUE_DOUBLE);check_equal(out.data.double_value,7.0);
      values[0]=turbodb_bool(0);out=turbodb_i64(SENTINEL);
      check_equal(orm_tidesdb_sql_expr_run_eval(&workspace,values,binding_count,&out,&error),TURBODB_STATUS_TYPE_ERROR);
      check_equal(out.data.int64_value,SENTINEL);
      values[0]=turbodb_bool(1);values[1]=turbodb_f64(7.0);
      check_equal(orm_tidesdb_sql_expr_run_eval(&workspace,values,binding_count,&out,&error),TURBODB_STATUS_TYPE_ERROR);
      check_equal(out.data.int64_value,SENTINEL);check_equal(reserve_calls,allocations[0]);check_equal(resize_calls,allocations[1]);
      for(size_t i=0;i<program.register_count;++i)
        check_equal(((const turbodb_value_t *)vec_at_const(&workspace.registers,i))->kind,TURBODB_VALUE_NULL);
      fail_reserve=fail_resize=0;check_equal(orm_tidesdb_sql_expr_run_close(&workspace,&error),TURBODB_STATUS_OK);
      check_equal(orm_sql_diagnostics_init(&numeric_diagnostics,1,&error),TURBODB_STATUS_OK);
      const char *sql[]={"SELECT CASE WHEN TRUE THEN 7 ELSE 7/0.0 END","SELECT COALESCE(7,7/0.0)",
        "SELECT IFNULL(7,7/0.0)","SELECT COALESCE(7/0.0,7)"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) for(size_t mode=0;mode<=ORM_SQL_EVALUATION_IGNORE_WRITE;++mode) {
        reset_budget();parse(sql[i]);check_equal(orm_sql_diagnostics_reset(&numeric_diagnostics,&error),TURBODB_STATUS_OK);
        check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
        out=turbodb_i64(SENTINEL);const bool failure=i==3&&mode==ORM_SQL_EVALUATION_WRITE;
        const orm_sql_evaluation evaluation={&numeric_diagnostics,(orm_sql_evaluation_mode)mode};
        const turbodb_status_t status=orm_tidesdb_sql_expr_eval_evaluation(&program,NULL,0,evaluation,&out,&error);
        check_equal(status,failure?TURBODB_STATUS_SQL_ERROR:TURBODB_STATUS_OK);
        if(failure) check_equal(out.data.int64_value,SENTINEL);
        else { check_equal(out.kind,TURBODB_VALUE_DOUBLE);check_equal(out.data.double_value,7.0); }
        check_equal(numeric_diagnostics.total,i==3&&!failure?1u:0u);
      }
    }
  }

  group("decimal-place numeric rounding") {
    it("compiles default and explicit precision without promoting exact integer values") {
      const struct { const char *sql; turbodb_value_t expected; } cases[]={
        {"SELECT ROUND(25,-1)",turbodb_i64(30)}, {"SELECT ROUND(25e0,-1)",turbodb_f64(20.0)},
        {"SELECT ROUND(-25,-1)",turbodb_i64(-30)}, {"SELECT ROUND(-3.5e0)",turbodb_f64(-4.0)},
        {"SELECT TRUNCATE(-1.375e0,2)",turbodb_f64(-1.37)},
        {"SELECT ROUND(18446744073709551615,18446744073709551615)",turbodb_u64(UINT64_MAX)},
        {"SELECT TRUNCATE(18446744073709551615,-1)",turbodb_u64(UINT64_C(18446744073709551610))},
        {"SELECT ROUND(TRUNCATE(123.75e0,1),-1)",turbodb_f64(120.0)},
        {"SELECT ROUND(NULL)",turbodb_null()}, {"SELECT ROUND(1,NULL)",turbodb_null()},
        {"SELECT TRUNCATE(NULL,-9223372036854775808)",turbodb_null()}
      };
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        reset_budget(); parse(cases[i].sql);
        check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
        sqlparser_document_destroy(document); document=NULL;
        const turbodb_value_t value=run(NULL,0); check_equal(value.kind,cases[i].expected.kind);
        if(value.kind==TURBODB_VALUE_INT64) check_equal(value.data.int64_value,cases[i].expected.data.int64_value);
        else if(value.kind==TURBODB_VALUE_UINT64) check_equal(value.data.uint64_value,cases[i].expected.data.uint64_value);
        else if(value.kind==TURBODB_VALUE_DOUBLE) check_equal(value.data.double_value,cases[i].expected.data.double_value);
      }
    }
    it("owns typed parameter programs after AST destruction and checks both precision inputs") {
      parse("SELECT ROUND(?,?)"); bindings[0].type=(orm_sql_type){TURBODB_VALUE_DOUBLE,true}; bindings[1].type=(orm_sql_type){TURBODB_VALUE_INT64,true};
      check_equal(orm_tidesdb_sql_expr_compile_value(document,root,bindings,binding_count,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
      check_equal(program.result.kind,TURBODB_VALUE_DOUBLE); check_true(program.result.nullable);
      sqlparser_document_destroy(document); document=NULL;
      turbodb_value_t values[]={turbodb_f64(1.375),turbodb_i64(2)};
      check_equal(run(values,2).data.double_value,1.38);
      values[0]=turbodb_null(); check_equal(run(values,2).kind,TURBODB_VALUE_NULL);
      values[1]=turbodb_f64(2.0); turbodb_value_t out=turbodb_i64(SENTINEL);
      check_equal(orm_tidesdb_sql_expr_eval(&program,values,2,&out,&error),TURBODB_STATUS_TYPE_ERROR);
      check_equal(out.data.int64_value,(int64_t)SENTINEL);
      reset_budget(); parse("SELECT TRUNCATE(value,digits)"); bindings[0].type=(orm_sql_type){TURBODB_VALUE_UINT64,false}; bindings[1].type=(orm_sql_type){TURBODB_VALUE_INT64,false};
      check_equal(orm_tidesdb_sql_expr_compile_value(document,root,bindings,binding_count,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
      check_equal(program.result.kind,TURBODB_VALUE_UINT64); check_false(program.result.nullable);
      values[0]=turbodb_u64(UINT64_MAX); values[1]=turbodb_i64(-1);
      check_equal(run(values,2).data.uint64_value,UINT64_C(18446744073709551610));
    }
    it("normalizes omitted ROUND precision but distinguishes truncation in program identity") {
      const char *sql[]={"SELECT ROUND(?)","SELECT ROUND(?,0)","SELECT TRUNCATE(?,0)"}; orm_sql_expr left={0};
      const size_t slots[]={0};
      parse(sql[0]); bindings[0].type=(orm_sql_type){TURBODB_VALUE_DOUBLE,false};
      check_equal(orm_tidesdb_sql_expr_compile_value(document,root,bindings,binding_count,TEST_DEPTH,&budget,&left,&error),TURBODB_STATUS_OK);
      for(size_t i=1;i<sizeof(sql)/sizeof(sql[0]);++i) {
        parse(sql[i]); bindings[0].type=(orm_sql_type){TURBODB_VALUE_DOUBLE,false};
        check_equal(orm_tidesdb_sql_expr_compile_value(document,root,bindings,binding_count,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
        bool same=false; check_equal(orm_tidesdb_sql_expr_same(&left,slots,&program,slots,&same,&error),TURBODB_STATUS_OK);
        check_equal(same,i==1); check_equal(orm_tidesdb_sql_expr_destroy(&program,&error),TURBODB_STATUS_OK);
      }
      check_equal(orm_tidesdb_sql_expr_destroy(&left,&error),TURBODB_STATUS_OK);
    }
    it("binds invalid calls eagerly while keeping overflow execution lazy and output atomic") {
      const char *bad_arity[]={"SELECT ROUND()","SELECT ROUND(1,2,3)","SELECT TRUNCATE(1)","SELECT TRUNCATE(1,2,3)"};
      for(size_t i=0;i<sizeof(bad_arity)/sizeof(bad_arity[0]);++i) {
        reset_budget(); parse(bad_arity[i]);
        check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_SQL_ERROR);
        check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],UINT64_C(0));
      }
      const char *unsupported[]={"SELECT ROUND(1,1.5)","SELECT TRUNCATE(TRUE,1)","SELECT ROUND('1',2)",
        "SELECT CASE WHEN TRUE THEN 1 ELSE ROUND(1,TRUE) END","SELECT ROUND(DISTINCT 1)"};
      for(size_t i=0;i<sizeof(unsupported)/sizeof(unsupported[0]);++i) {
        reset_budget(); parse(unsupported[i]);
        check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_UNSUPPORTED);
        check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],UINT64_C(0));
      }
      const char *lazy[]={"SELECT CASE WHEN TRUE THEN 7 ELSE ROUND(9223372036854775807,-1) END=7",
        "SELECT COALESCE(7,ROUND(9223372036854775807,-1))=7","SELECT FALSE AND ROUND(9223372036854775807,-1)=0"};
      for(size_t i=0;i<sizeof(lazy)/sizeof(lazy[0]);++i) { reset_budget(); parse(lazy[i]); check_equal(compile(),TURBODB_STATUS_OK); expect_truth(run(NULL,0),i<2); }
      const char *overflow[]={"SELECT ROUND(9223372036854775807,-1)","SELECT ROUND(NULL,ABS(-9223372036854775808))"};
      for(size_t i=0;i<sizeof(overflow)/sizeof(overflow[0]);++i) {
        reset_budget(); parse(overflow[i]); check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
        turbodb_value_t out=turbodb_i64(SENTINEL); check_equal(orm_tidesdb_sql_expr_eval(&program,NULL,0,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
        check_equal(out.data.int64_value,(int64_t)SENTINEL);
      }
    }
    it("refunds rounding compiler faults and checks workspace admission before execution") {
      const char *sql[]={"SELECT ROUND(ABS(?))=12","SELECT TRUNCATE(ROUND(?,?),0)=12"};
      for(size_t sample=0;sample<sizeof(sql)/sizeof(sql[0]);++sample) {
        reset_budget(); parse(sql[sample]); for(size_t i=0;i<binding_count;++i) bindings[i].type=(orm_sql_type){TURBODB_VALUE_INT64,false};
        reserve_calls=resize_calls=0; check_equal(compile(),TURBODB_STATUS_OK); const size_t counts[]={reserve_calls,resize_calls};
        for(size_t phase=0;phase<2;++phase) for(size_t point=1;point<=counts[phase];++point) {
          fail_reserve=fail_resize=0; reset_budget(); reserve_calls=resize_calls=0; if(phase) fail_resize=point; else fail_reserve=point;
          check_equal(compile(),TURBODB_STATUS_OUT_OF_MEMORY); check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],UINT64_C(0));
        }
        fail_reserve=fail_resize=0; reset_budget(); check_equal(compile(),TURBODB_STATUS_OK); const turbodb_value_t values[]={turbodb_i64(12),turbodb_i64(0)};
        turbodb_value_t out=turbodb_i64(SENTINEL); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
        check_equal(orm_tidesdb_sql_expr_eval(&program,values,binding_count,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
        check_equal(out.data.int64_value,(int64_t)SENTINEL); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=TEST_STEPS;
        expect_truth(run(values,binding_count),1);
      }
    }
  }

  group("single argument numeric call compilation") {
    it("compiles nested functions NULLs unsigned extremes and CEILING alias") {
      const char *sql[]={"SELECT ABS(-7)=7 AND SIGN(-9223372036854775808)=-1 AND SIGN(0)=0",
        "SELECT ABS(18446744073709551615)=18446744073709551615 AND SIGN(18446744073709551615)=1",
        "SELECT FLOOR(-1.25)=-2.0 AND CEILING(-1.25)=-1.0 AND CEIL(1.25)=2.0",
        "SELECT ABS(NULL) IS NULL AND FLOOR(NULL) IS NULL AND CEIL(NULL) IS NULL AND SIGN(NULL) IS NULL",
        "SELECT abs(CEILING(FLOOR(-1.25)))=2.0 AND SIGN(ABS(-7))=1"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) { reset_budget(); parse(sql[i]); check_equal(compile(),TURBODB_STATUS_OK);
        sqlparser_document_destroy(document); document=NULL; expect_truth(run(NULL,0),1); }
    }
    it("evaluates each typed input once and reuses the run after the document dies") {
      parse("SELECT ABS(?)"); bindings[0].type=(orm_sql_type){TURBODB_VALUE_INT64,true};
      check_equal(orm_tidesdb_sql_expr_compile_value(document,root,bindings,binding_count,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
      check_equal(program.input_count,1u); check_equal(program.result.kind,TURBODB_VALUE_INT64); check_true(program.result.nullable);
      sqlparser_document_destroy(document); document=NULL; orm_sql_expr_run evaluator={0};
      check_equal(orm_tidesdb_sql_expr_run_open(&program,&evaluator,&error),TURBODB_STATUS_OK);
      const turbodb_value_t values[]={turbodb_i64(-7),turbodb_null(),turbodb_i64(9)};
      for(size_t i=0;i<sizeof(values)/sizeof(values[0]);++i) { const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]; turbodb_value_t out;
        check_equal(orm_tidesdb_sql_expr_run_eval(&evaluator,&values[i],1,&out,&error),TURBODB_STATUS_OK);
        check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-steps,2u); check_equal(out.kind,values[i].kind);
        if(out.kind!=TURBODB_VALUE_NULL) check_equal(out.data.int64_value,i?9:7); }
      check_equal(orm_tidesdb_sql_expr_run_close(&evaluator,&error),TURBODB_STATUS_OK);
    }
    it("skips overflow in lazy branches but reports reached overflow without publishing a result") {
      const char *skipped[]={"SELECT TRUE OR ABS(-9223372036854775808)=0",
        "SELECT CASE WHEN TRUE THEN 7 ELSE ABS(-9223372036854775808) END=7",
        "SELECT COALESCE(1,ABS(-9223372036854775808))=1"};
      for(size_t i=0;i<sizeof(skipped)/sizeof(skipped[0]);++i) { reset_budget(); parse(skipped[i]); check_equal(compile(),TURBODB_STATUS_OK); expect_truth(run(NULL,0),1); }
      reset_budget(); parse("SELECT ABS(-9223372036854775808)=0"); check_equal(compile(),TURBODB_STATUS_OK);
      turbodb_value_t out=turbodb_i64(SENTINEL); check_equal(orm_tidesdb_sql_expr_eval(&program,NULL,0,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(out.data.int64_value,SENTINEL); check_contains(error.message,"at byte 7");
    }
    it("rejects arity DISTINCT and implicit conversions even when unreachable") {
      const char *sql[]={"SELECT ABS()=0","SELECT SIGN(1,2)=0","SELECT FLOOR(TRUE)=0","SELECT CEIL('1')=0",
        "SELECT TRUE OR ABS(DISTINCT 1)=1","SELECT CASE WHEN TRUE THEN 1 ELSE FLOOR('1') END=1"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) { reset_budget(); parse(sql[i]);
        check_equal(compile(),i<2?TURBODB_STATUS_SQL_ERROR:TURBODB_STATUS_UNSUPPORTED); check_null(program.budget);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u); }
    }
    it("compares CEIL and CEILING as identical numeric programs but distinguishes other calls") {
      const char *sql[]={"SELECT CEIL(?)","SELECT CEILING(?)","SELECT FLOOR(?)"}; orm_sql_expr left={0};
      parse(sql[0]); bindings[0].type=(orm_sql_type){TURBODB_VALUE_DOUBLE,false};
      check_equal(orm_tidesdb_sql_expr_compile_value(document,root,bindings,binding_count,TEST_DEPTH,&budget,&left,&error),TURBODB_STATUS_OK);
      const size_t slots[]={0};
      for(size_t i=1;i<sizeof(sql)/sizeof(sql[0]);++i) { parse(sql[i]); bindings[0].type=(orm_sql_type){TURBODB_VALUE_DOUBLE,false};
        check_equal(orm_tidesdb_sql_expr_compile_value(document,root,bindings,binding_count,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
        bool same=false; check_equal(orm_tidesdb_sql_expr_same(&left,slots,&program,slots,&same,&error),TURBODB_STATUS_OK); check_equal(same,i==1);
        check_equal(orm_tidesdb_sql_expr_destroy(&program,&error),TURBODB_STATUS_OK); }
      check_equal(orm_tidesdb_sql_expr_destroy(&left,&error),TURBODB_STATUS_OK);
    }
    it("refunds every numeric call compilation allocation and respects every step allowance") {
      parse("SELECT ABS(FLOOR(?))=CEILING(?) AND SIGN(?)=-1");
      bindings[0].type=bindings[1].type=(orm_sql_type){TURBODB_VALUE_DOUBLE,false}; bindings[2].type=(orm_sql_type){TURBODB_VALUE_INT64,false};
      reserve_calls=resize_calls=0; check_equal(compile(),TURBODB_STATUS_OK); const size_t counts[]={reserve_calls,resize_calls};
      const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      for(size_t phase=0;phase<2;++phase) for(size_t point=1;point<=counts[phase];++point) {
        fail_reserve=fail_resize=0; reset_budget(); reserve_calls=resize_calls=0; if(phase) fail_resize=point; else fail_reserve=point;
        check_equal(compile(),TURBODB_STATUS_OUT_OF_MEMORY); check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u); }
      for(uint64_t point=0;point<steps;++point) { fail_reserve=fail_resize=0; reset_budget(); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=point;
        check_equal(compile(),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u); }
    }
  }
  it("replaces aggregate calls only through an explicit trusted input binding") {
    parse("SELECT COUNT(*)");
    check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,
        TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_UNSUPPORTED);
    check_null(program.budget);
    orm_sql_expr_input input={root,{TURBODB_VALUE_INT64,false},false};
    check_equal(orm_tidesdb_sql_expr_compile_value(document,root,&input,1,
        TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    input.replace_expression=true;
    check_equal(orm_tidesdb_sql_expr_compile_value(document,root,&input,1,
        TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document=NULL;
    const turbodb_value_t value=turbodb_i64(8); check_equal(run(&value,1).data.int64_value,8);
    reset_budget(); parse("SELECT ?"); bindings[0].replace_expression=true;
    check_equal(compile(),TURBODB_STATUS_INVALID_ARGUMENT); check_null(program.budget);
  }

  it("replaces windows only through a trusted Binder result slot") {
    parse("SELECT ROW_NUMBER() OVER(ORDER BY id)");
    check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,
        TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_UNSUPPORTED);
    check_null(program.budget);
    orm_sql_expr_input input={root,{TURBODB_VALUE_INT64,false},false};
    check_equal(orm_tidesdb_sql_expr_compile_value(document,root,&input,1,
        TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    input.replace_expression=true;
    check_equal(orm_tidesdb_sql_expr_compile_value(document,root,&input,1,
        TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document=NULL;
    const turbodb_value_t value=turbodb_i64(8); check_equal(run(&value,1).data.int64_value,8);
  }
  it("substitutes validated whole group expressions without evaluating their row children") {
    parse("SELECT id+1");
    orm_sql_expr_input input={root,{TURBODB_VALUE_INT64,false},true};
    check_equal(orm_tidesdb_sql_expr_compile_value(document,root,&input,1,
        TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
    const turbodb_value_t value=turbodb_i64(9); check_equal(run(&value,1).data.int64_value,9);
    reset_budget(); input.node=sqlparser_statements(document).first;
    check_equal(orm_tidesdb_sql_expr_compile_value(document,input.node,&input,1,
        TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_null(program.budget);
  }

  it("compiles scalar roots while retaining the predicate entry point restriction") {
    parse("SELECT 1 + 2"); check_equal(compile(), TURBODB_STATUS_UNSUPPORTED);
    check_null(program.budget);
    check_equal(orm_tidesdb_sql_expr_compile_value(document, root, bindings, binding_count,
        TEST_DEPTH, &budget, &program, &error), TURBODB_STATUS_OK);
    check_equal(program.result.kind, TURBODB_VALUE_INT64); check_equal(program.result.nullable, false);
    check_equal(run(NULL, 0).data.int64_value, 3);
    reset_budget(); parse("SELECT 18446744073709551615");
    check_equal(orm_tidesdb_sql_expr_compile_value(document, root, NULL, 0,
        TEST_DEPTH, &budget, &program, &error), TURBODB_STATUS_OK);
    check_equal(program.result.kind, TURBODB_VALUE_UINT64); check_equal(run(NULL, 0).data.uint64_value, UINT64_MAX);
    reset_budget(); parse("SELECT NULL");
    check_equal(orm_tidesdb_sql_expr_compile_value(document, root, NULL, 0,
        TEST_DEPTH, &budget, &program, &error), TURBODB_STATUS_OK);
    check_equal(program.result.kind, TURBODB_VALUE_NULL); check_equal(run(NULL, 0).kind, TURBODB_VALUE_NULL);
  }

  it("returns literal and input byte views beyond register close without borrowing the AST") {
    parse_mode("SELECT IFNULL(NULL, 'a\\0b')", false);
    check_equal(orm_tidesdb_sql_expr_compile_value(document, root, NULL, 0,
        TEST_DEPTH, &budget, &program, &error), TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document = NULL;
    turbodb_value_t value = run(NULL, 0);
    check_equal(value.kind, TURBODB_VALUE_TEXT); check_equal(value.data.text_value.len, 3u);
    check_equal(memcmp(value.data.text_value.data, "a\0b", 3), 0);
    const unsigned char bytes[] = {0, 255, 7};
    const turbodb_value_t inputs[] = {turbodb_text_v((vstr){"x\0y", 3}), turbodb_blob(bytes, sizeof(bytes))};
    for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
      reset_budget(); parse("SELECT ?"); bindings[0].type = (orm_sql_type){inputs[i].kind, false};
      check_equal(orm_tidesdb_sql_expr_compile_value(document, root, bindings, binding_count,
          TEST_DEPTH, &budget, &program, &error), TURBODB_STATUS_OK);
      value = run(&inputs[i], 1); check_equal(program.active_runs, 0u);
      if (i) check_equal(value.data.blob_value.data, inputs[i].data.blob_value.data);
      else check_equal(value.data.text_value.data, inputs[i].data.text_value.data);
    }
  }

  it("returns exact finite floating scalar results and preserves output on overflow") {
    parse("SELECT ? + ?");
    bindings[0].type = bindings[1].type = (orm_sql_type){TURBODB_VALUE_DOUBLE, false};
    check_equal(orm_tidesdb_sql_expr_compile_value(document, root, bindings, binding_count,
        TEST_DEPTH, &budget, &program, &error), TURBODB_STATUS_OK);
    const turbodb_value_t inputs[] = {turbodb_f64(0.5), turbodb_f64(1.5)};
    check_equal(run(inputs, 2).data.double_value, 2.0);
    check_equal(program.result.kind, TURBODB_VALUE_DOUBLE); check_equal(program.result.nullable, false);
    reset_budget(); parse("SELECT 9223372036854775807 + 1");
    check_equal(orm_tidesdb_sql_expr_compile_value(document, root, NULL, 0,
        TEST_DEPTH, &budget, &program, &error), TURBODB_STATUS_OK);
    turbodb_value_t output = turbodb_i64(SENTINEL);
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_tidesdb_sql_expr_eval(&program, NULL, 0, &output, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(output.data.int64_value, SENTINEL); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
  }

  it("compiles finite decimal and exponent literals as DOUBLE values") {
    const struct { const char *sql; double expected; } cases[] = {
      {"SELECT 1.5 + 2.5", 4.0}, {"SELECT .125 * 8.0", 1.0},
      {"SELECT -2.5e1 + 5.0", -20.0}, {"SELECT 1e-400", 0.0}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      reset_budget(); parse(cases[i].sql);
      check_equal(orm_tidesdb_sql_expr_compile_value(document, root, NULL, 0,
          TEST_DEPTH, &budget, &program, &error), TURBODB_STATUS_OK);
      const turbodb_value_t result = run(NULL, 0);
      check_equal(result.kind, TURBODB_VALUE_DOUBLE);
      check_equal(result.data.double_value, cases[i].expected);
    }
    reset_budget(); parse("SELECT 1e309");
    check_equal(orm_tidesdb_sql_expr_compile_value(document, root, NULL, 0,
        TEST_DEPTH, &budget, &program, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(program.budget);
  }

  it("compiles LIKE escapes and freezes document backslash mode into the program") {
    const struct { const char *sql; bool no_backslash; int expected; } cases[] = {
      {"SELECT 'a_b' LIKE 'a\\_b'", false, 1}, {"SELECT 'a_b' LIKE 'a\\_b'", true, 0},
      {"SELECT 'a_b' LIKE 'a|_b' ESCAPE '|'", true, 1},
      {"SELECT 'a_b' NOT LIKE 'a|_b' ESCAPE '|'", false, 0},
      {"SELECT 'a_b' LIKE 'a_b' ESCAPE ''", false, 1},
      {"SELECT 'a%b' LIKE 'a\\0%b' ESCAPE '\\0'", false, 1},
      {"SELECT NULL NOT LIKE '%'", false, -1},
      {"SELECT CASE WHEN 'abc' LIKE 'a%' THEN TRUE ELSE FALSE END", false, 1}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      reset_budget(); parse_mode(cases[i].sql, cases[i].no_backslash);
      check_equal(compile(), TURBODB_STATUS_OK); sqlparser_document_destroy(document); document = NULL;
      expect_truth(run(NULL, 0), cases[i].expected);
    }
  }

  it("rejects unsupported LIKE operands and ESCAPE expressions even in dead branches") {
    const char *sql[] = {"SELECT TRUE OR 1 LIKE '1'", "SELECT TRUE OR 'a' LIKE 'a' ESCAPE 'xx'",
        "SELECT TRUE OR 'a' LIKE 'a' ESCAPE NULL", "SELECT TRUE OR 'a' LIKE 'a' ESCAPE ?",
        "SELECT TRUE OR 'a' LIKE 'a' ESCAPE ''", "SELECT TRUE OR 'a' LIKE 'a' ESCAPE '\xc3\xa9'"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
      reset_budget(); parse(sql[i]); check_equal(compile(), TURBODB_STATUS_UNSUPPORTED);
      check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
  }

  it("short circuits LIKE payload checks but reports reached non ASCII inputs") {
    parse("SELECT ? OR ? LIKE ?"); bindings[1].type.kind = bindings[2].type.kind = TURBODB_VALUE_TEXT;
    check_equal(compile(), TURBODB_STATUS_OK);
    turbodb_value_t values[] = {turbodb_bool(true), turbodb_text("\xc3\xa9"), turbodb_text("%")};
    expect_truth(run(values, 3), 1); values[0] = turbodb_bool(false);
    turbodb_value_t out = turbodb_i64(SENTINEL);
    check_equal(orm_tidesdb_sql_expr_eval(&program, values, 3, &out, &error), TURBODB_STATUS_UNSUPPORTED);
    check_equal(out.data.int64_value, SENTINEL); check_contains(error.message, "ASCII");
    values[1] = turbodb_text("abc"); expect_truth(run(values, 3), 1);
  }

  it("cleans LIKE literal compilation allocation failures before allowing retry") {
    parse("SELECT 'a_b' LIKE 'a|_b' ESCAPE '|'");
    reserve_calls = resize_calls = 0; check_equal(compile(), TURBODB_STATUS_OK);
    const size_t reserves = reserve_calls, resizes = resize_calls;
    for (size_t i = 1; i <= reserves + resizes; ++i) {
      fail_reserve = fail_resize = 0; reset_budget(); reserve_calls = resize_calls = 0;
      if (i <= reserves) fail_reserve = i; else fail_resize = i - reserves;
      check_equal(compile(), TURBODB_STATUS_OUT_OF_MEMORY); check_null(program.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    fail_reserve = fail_resize = 0; reset_budget(); check_equal(compile(), TURBODB_STATUS_OK); expect_truth(run(NULL, 0), 1);
  }

  it("owns compiled constants after AST destruction and preserves exact integer boundaries") {
    const char *cases[] = {"SELECT NOT FALSE AND (1 = +1)",
      "SELECT -9223372036854775808 < 18446744073709551615",
      "SELECT 9007199254740992 <> 9007199254740993",
      "SELECT NULL IS NULL AND 1 IS NOT NULL", "SELECT NULL <=> NULL"};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      reset_budget(); parse(cases[i]); check_equal(compile(), TURBODB_STATUS_OK);
      sqlparser_document_destroy(document); document = NULL;
      expect_truth(run(NULL, 0), 1);
    }
  }

  it("evaluates the complete AND OR truth tables including UNKNOWN branches") {
    const char *sql[] = {"SELECT ? AND ?", "SELECT ? OR ?"};
    const int expected[][3][3] = {
      {{0, 0, 0}, {0, 1, -1}, {0, -1, -1}},
      {{0, 1, -1}, {1, 1, 1}, {-1, 1, -1}}};
    const turbodb_value_t values[] = {turbodb_bool(0), turbodb_bool(1), turbodb_null()};
    for (size_t op = 0; op < 2; ++op) {
      reset_budget(); parse(sql[op]); check_equal(compile(), TURBODB_STATUS_OK);
      for (size_t a = 0; a < 3; ++a) for (size_t b = 0; b < 3; ++b) {
        const turbodb_value_t inputs[] = {values[a], values[b]};
        expect_truth(run(inputs, 2), expected[op][a][b]);
      }
    }
  }

  it("short circuits a decisive left value but executes the same RHS when needed") {
    const char *sql[] = {"SELECT ? AND (? = ?)", "SELECT ? OR (? = ?)"};
    const unsigned char malformed[] = {0xc0, 0x80};
    for (size_t op = 0; op < 2; ++op) {
      reset_budget(); parse(sql[op]);
      bindings[1].type.kind = bindings[2].type.kind = TURBODB_VALUE_TEXT;
      check_equal(compile(), TURBODB_STATUS_OK);
      const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      turbodb_value_t inputs[] = {turbodb_bool((int)op), turbodb_text_v((vstr){(const char *)malformed, sizeof(malformed)}), turbodb_text("ok")};
      expect_truth(run(inputs, 3), (int)op);
      check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], 2u);
      turbodb_value_t out = turbodb_i64(SENTINEL);
      inputs[0] = turbodb_null();
      check_equal(orm_tidesdb_sql_expr_eval(&program, inputs, 3, &out, &error), TURBODB_STATUS_TYPE_ERROR);
      check_equal(out.kind, TURBODB_VALUE_INT64); check_equal(out.data.int64_value, SENTINEL);
      check_contains(error.message, "at byte");
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
      inputs[0] = turbodb_bool(!(int)op); inputs[1] = turbodb_text("ok");
      expect_truth(run(inputs, 3), 1);
    }
  }

  it("binds every unreachable branch and refuses unsupported syntax or conversions") {
    const char *sql[] = {"SELECT TRUE OR UNSUPPORTED_FN(1) = 1", "SELECT FALSE AND (1 / 2 = 3)",
      "SELECT TRUE OR ('a' = 1)", "SELECT 1",
      "SELECT TRUE OR (TRUE = 1)"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
      reset_budget(); parse(sql[i]);
      check_equal(compile(), TURBODB_STATUS_UNSUPPORTED);
      check_null(program.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
  }

  it("uses resolved name and parameter slots without retaining binding storage") {
    parse("SELECT score >= ? AND enabled");
    check_equal(binding_count, 3u);
    bindings[0].type = (orm_sql_type){TURBODB_VALUE_INT64, false};
    bindings[1].type = (orm_sql_type){TURBODB_VALUE_UINT64, false};
    check_equal(compile(), TURBODB_STATUS_OK);
    memset(bindings, 0, sizeof(bindings));
    sqlparser_document_destroy(document); document = NULL;
    turbodb_value_t values[] = {turbodb_i64(9), turbodb_u64(9), turbodb_bool(1)};
    expect_truth(run(values, 3), 1);
    values[0] = turbodb_i64(8);
    expect_truth(run(values, 3), 0);
    check_equal(orm_tidesdb_sql_expr_eval(&program, values, 3, &values[0], &error), TURBODB_STATUS_OK);
    expect_truth(values[0], 0);
  }

  it("rejects missing duplicate unordered and unrelated input bindings") {
    parse("SELECT ? = ?");
    const orm_sql_expr_input original = bindings[1];
    bindings[1] = bindings[0];
    check_equal(compile(), TURBODB_STATUS_INVALID_ARGUMENT);
    bindings[1] = original;
    const orm_sql_expr_input first = bindings[0]; bindings[0] = bindings[1]; bindings[1] = first;
    check_equal(compile(), TURBODB_STATUS_INVALID_ARGUMENT);
    bindings[0] = first; bindings[1] = original;
    --binding_count;
    check_equal(compile(), TURBODB_STATUS_SQL_ERROR);
    reset_budget(); parse("SELECT TRUE, ?");
    check_equal(compile(), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
  }

  it("enforces exact depth and releases storage on depth or node exhaustion") {
    parse("SELECT NOT NOT TRUE");
    check_equal(orm_tidesdb_sql_expr_compile(document, root, NULL, 0, 2, &budget, &program, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    reset_budget();
    check_equal(orm_tidesdb_sql_expr_compile(document, root, NULL, 0, 3, &budget, &program, &error), TURBODB_STATUS_OK);
    expect_truth(run(NULL, 0), 1);
    limits.statement.value[ORM_SQL_BUDGET_AST_NODES] = 2;
    reset_budget(); check_equal(compile(), TURBODB_STATUS_LIMIT_EXCEEDED);
    limits.statement.value[ORM_SQL_BUDGET_AST_NODES] = TEST_NODES;
    limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES] = 2;
    reset_budget(); check_equal(compile(), TURBODB_STATUS_LIMIT_EXCEEDED);
  }

  it("admits the exact compile capacity peak and rejects one byte less without leaks") {
    parse("SELECT TRUE AND NOT FALSE"); check_equal(compile(), TURBODB_STATUS_OK);
    const uint64_t peak = budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES];
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = peak - 1;
    reset_budget(); check_equal(compile(), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = peak;
    reset_budget(); check_equal(compile(), TURBODB_STATUS_OK);
  }

  it("releases run scratch on byte or step exhaustion and leaves output unchanged") {
    parse("SELECT TRUE"); check_equal(compile(), TURBODB_STATUS_OK);
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    orm_sql_budget_amount full = {0};
    full.value[ORM_SQL_BUDGET_WORK_BYTES] = limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] - retained;
    check_equal(orm_tidesdb_sql_budget_reserve(&budget, &full, &error), TURBODB_STATUS_OK);
    turbodb_value_t out = turbodb_i64(SENTINEL);
    check_equal(orm_tidesdb_sql_expr_eval(&program, NULL, 0, &out, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.data.int64_value, SENTINEL);
    check_equal(orm_tidesdb_sql_budget_release(&budget, ORM_SQL_BUDGET_WORK_BYTES, full.value[ORM_SQL_BUDGET_WORK_BYTES], &error), TURBODB_STATUS_OK);
    full = (orm_sql_budget_amount){0}; full.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = TEST_STEPS;
    check_equal(orm_tidesdb_sql_budget_reserve(&budget, &full, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_expr_eval(&program, NULL, 0, &out, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.data.int64_value, SENTINEL);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
  }

  it("cleans up each selected container allocation failure during compile and execute") {
    parse("SELECT TRUE AND NOT FALSE");
    for (size_t failure = 1; failure <= 2; ++failure) {
      reset_budget(); reserve_calls = 0; fail_reserve = failure;
      check_equal(compile(), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(program.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    fail_reserve = 0; reset_budget(); check_equal(compile(), TURBODB_STATUS_OK);
    /* Discover the exact compile mutation count on a fresh run. */
    reset_budget(); resize_calls = 0; check_equal(compile(), TURBODB_STATUS_OK);
    const size_t mutations = resize_calls;
    check_greater(mutations, 0u);
    for (size_t failure = 1; failure <= mutations; ++failure) {
      reset_budget(); resize_calls = 0; fail_resize = failure;
      check_equal(compile(), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(program.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    fail_resize = 0; reset_budget(); check_equal(compile(), TURBODB_STATUS_OK);
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    turbodb_value_t out = turbodb_i64(SENTINEL);
    reserve_calls = 0; fail_reserve = 1;
    check_equal(orm_tidesdb_sql_expr_eval(&program, NULL, 0, &out, &error), TURBODB_STATUS_OUT_OF_MEMORY);
    fail_reserve = 0; resize_calls = 0; fail_resize = 1;
    check_equal(orm_tidesdb_sql_expr_eval(&program, NULL, 0, &out, &error), TURBODB_STATUS_OUT_OF_MEMORY);
    check_equal(out.data.int64_value, SENTINEL);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
    fail_resize = 0; expect_truth(run(NULL, 0), 1);
  }

  it("compiles and runs a deep predicate without C recursion") {
    enum { DEPTH = 512, PREFIX_BYTES = 7, NOT_BYTES = 4, TAIL_BYTES = 5 };
    char sql[PREFIX_BYTES + DEPTH * NOT_BYTES + TAIL_BYTES];
    memcpy(sql, "SELECT ", PREFIX_BYTES);
    for (size_t i = 0; i < DEPTH; ++i) memcpy(sql + PREFIX_BYTES + i * NOT_BYTES, "NOT ", NOT_BYTES);
    memcpy(sql + PREFIX_BYTES + DEPTH * NOT_BYTES, "TRUE", TAIL_BYTES);
    parse(sql); check_equal(compile(), TURBODB_STATUS_OK);
    expect_truth(run(NULL, 0), 1);
    check_equal(program.register_count, DEPTH + 1u);
  }

  it("preserves occupied outputs and requires a live statement through destruction") {
    parse("SELECT TRUE"); check_equal(compile(), TURBODB_STATUS_OK);
    const size_t count = vec_size(&program.code);
    check_equal(compile(), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(vec_size(&program.code), count);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_BUSY);
    turbodb_value_t out = turbodb_i64(SENTINEL), input = turbodb_bool(1);
    check_equal(orm_tidesdb_sql_expr_eval(&program, &input, 1, &out, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    expect_truth(run(NULL, 0), 1);
    check_equal(orm_tidesdb_sql_expr_destroy(&program, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_expr_destroy(&program, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_expr_eval(&program, NULL, 0, &out, &error), TURBODB_STATUS_INVALID_ARGUMENT);
  }

  it("keeps nested branch destinations separate and checks the surrounding predicate") {
    parse("SELECT NOT ((? OR ?) AND (? OR ?))");
    check_equal(compile(), TURBODB_STATUS_OK);
    for (unsigned mask = 0; mask < 16; ++mask) {
      turbodb_value_t values[] = {turbodb_bool(mask & 1), turbodb_bool(mask & 2),
                              turbodb_bool(mask & 4), turbodb_bool(mask & 8)};
      const int expected = !((mask & 3) && (mask & 12));
      expect_truth(run(values, 4), expected);
    }
  }

  it("rejects overflowing integers malformed root IDs and SQLite documents") {
    const char *sql[] = {"SELECT 18446744073709551616 = 0", "SELECT -9223372036854775809 = 0"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
      reset_budget(); parse(sql[i]);
      check_equal(compile(), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    reset_budget(); parse("SELECT TRUE"); root = UINT32_MAX;
    check_equal(compile(), TURBODB_STATUS_INVALID_ARGUMENT);
    sqlparser_document_destroy(document); document = NULL;
    sqlparser_error parse_error;
    const char sqlite_sql[] = "SELECT TRUE";
    check_equal(sqlparser_parse_dialect(sqlite_sql, sizeof(sqlite_sql) - 1,
      SQLPARSER_SQLITE, NULL, &document, &parse_error), SQLPARSER_OK);
    root = sqlparser_statements(document).first;
    check_equal(compile(), TURBODB_STATUS_UNSUPPORTED);
  }

  it("reuses registers across rows without allocation and blocks premature program destruction") {
    parse("SELECT ? OR ?"); check_equal(compile(), TURBODB_STATUS_OK);
    orm_sql_expr_run workspace = {0};
    check_equal(orm_tidesdb_sql_expr_run_open(&program, &workspace, &error), TURBODB_STATUS_OK);
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    reserve_calls = resize_calls = 0; fail_reserve = fail_resize = 1;
    const turbodb_value_t values[] = {turbodb_bool(0), turbodb_bool(1)};
    turbodb_value_t out = turbodb_i64(SENTINEL);
    check_equal(orm_tidesdb_sql_expr_run_eval(&workspace, values, 2, &out, &error), TURBODB_STATUS_OK);
    expect_truth(out, 1);
    check_equal(orm_tidesdb_sql_expr_run_eval(&workspace, values, 2, &out, &error), TURBODB_STATUS_OK);
    check_equal(reserve_calls, 0u); check_equal(resize_calls, 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
    check_equal(orm_tidesdb_sql_expr_destroy(&program, &error), TURBODB_STATUS_BUSY);
    for (size_t i = 0; i < vec_size(&workspace.registers); ++i)
      check_equal(((const turbodb_value_t *)vec_at_const(&workspace.registers, i))->kind, TURBODB_VALUE_NULL);
    check_equal(orm_tidesdb_sql_expr_run_close(&workspace, &error), TURBODB_STATUS_OK);
    check_equal(program.active_runs, 0u);
    check_equal(orm_tidesdb_sql_expr_run_close(&workspace, &error), TURBODB_STATUS_OK);
  }

  it("clears failed run values and allows a later row to execute within remaining budget") {
    parse("SELECT ? = ?"); bindings[0].type.kind = bindings[1].type.kind = TURBODB_VALUE_TEXT;
    check_equal(compile(), TURBODB_STATUS_OK);
    orm_sql_expr_run workspace = {0};
    check_equal(orm_tidesdb_sql_expr_run_open(&program, &workspace, &error), TURBODB_STATUS_OK);
    turbodb_value_t values[] = {turbodb_text("ok"), turbodb_i64(1)}, out = turbodb_i64(SENTINEL);
    check_equal(orm_tidesdb_sql_expr_run_eval(&workspace, values, 2, &out, &error), TURBODB_STATUS_TYPE_ERROR);
    check_equal(out.data.int64_value, SENTINEL);
    for (size_t i = 0; i < vec_size(&workspace.registers); ++i)
      check_equal(((const turbodb_value_t *)vec_at_const(&workspace.registers, i))->kind, TURBODB_VALUE_NULL);
    values[1] = turbodb_text("ok");
    check_equal(orm_tidesdb_sql_expr_run_eval(&workspace, values, 2, &out, &error), TURBODB_STATUS_OK);
    expect_truth(out, 1);
    check_equal(orm_tidesdb_sql_expr_run_close(&workspace, &error), TURBODB_STATUS_OK);
  }

  it("decodes MySQL quote and escape forms to exact TEXT bytes") {
    const struct { const char *sql; vstr expected; } cases[] = {
      {"SELECT ? = ''", {"", 0}},
      {"SELECT ? = 'a''b'", {"a'b", 3}},
      {"SELECT ? = \"a\"\"b\"", {"a\"b", 3}},
      {"SELECT ? = '\\0'", {"\0", 1}},
      {"SELECT ? = '\\b'", {"\b", 1}},
      {"SELECT ? = '\\n'", {"\n", 1}},
      {"SELECT ? = '\\r'", {"\r", 1}},
      {"SELECT ? = '\\t'", {"\t", 1}},
      {"SELECT ? = '\\Z'", {"\x1a", 1}},
      {"SELECT ? = '\\\\'", {"\\", 1}},
      {"SELECT ? = '\\''", {"'", 1}},
      {"SELECT ? = '\\\"'", {"\"", 1}},
      {"SELECT ? = '\\q\\B'", {"qB", 2}},
      {"SELECT ? = '\\%\\_'", {"\\%\\_", 4}},
      {"SELECT ? = '中文 😀'", {"中文 😀", sizeof("中文 😀") - 1}},
      {"SELECT ? = 'a  '", {"a  ", 3}}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      reset_budget(); parse_mode(cases[i].sql, false); bindings[0].type = (orm_sql_type){TURBODB_VALUE_TEXT, false};
      check_equal(compile(), TURBODB_STATUS_OK);
      sqlparser_document_destroy(document); document = NULL;
      const turbodb_value_t input = turbodb_text_v(cases[i].expected);
      expect_truth(run(&input, 1), 1);
    }
  }

  it("honors NO_BACKSLASH_ESCAPES from the document rather than runtime assumptions") {
    const char sql[] = "SELECT ? = 'a\\n\\0\\%\\q''z'";
    const vstr expected[] = {{"a\n\0\\%q'z", sizeof("a\n\0\\%q'z") - 1},
                             {"a\\n\\0\\%\\q'z", sizeof("a\\n\\0\\%\\q'z") - 1}};
    for (size_t mode = 0; mode < sizeof(expected) / sizeof(expected[0]); ++mode) {
      reset_budget(); parse_mode(sql, mode != 0); bindings[0].type = (orm_sql_type){TURBODB_VALUE_TEXT, false};
      check_equal(compile(), TURBODB_STATUS_OK);
      const turbodb_value_t input = turbodb_text_v(expected[mode]); expect_truth(run(&input, 1), 1);
    }
  }

  it("retains multiple literal views after AST destruction and reuses them across runs") {
    parse("SELECT 'first' = 'first' AND 'two' <> 'three' AND '' = '' AND 'x' < 'x '");
    check_equal(compile(), TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document = NULL;
    orm_sql_expr_run workspace = {0};
    check_equal(orm_tidesdb_sql_expr_run_open(&program, &workspace, &error), TURBODB_STATUS_OK);
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    reserve_calls = resize_calls = 0; fail_reserve = fail_resize = 1;
    for (unsigned i = 0; i < TEST_INPUTS; ++i) {
      turbodb_value_t output = turbodb_null();
      check_equal(orm_tidesdb_sql_expr_run_eval(&workspace, NULL, 0, &output, &error), TURBODB_STATUS_OK);
      expect_truth(output, 1);
    }
    check_equal(reserve_calls, 0u); check_equal(resize_calls, 0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
    check_equal(orm_tidesdb_sql_expr_destroy(&program, &error), TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_expr_run_close(&workspace, &error), TURBODB_STATUS_OK);
  }

  it("validates UTF-8 literals even in unreachable branches and preserves strict comparisons") {
    const char *invalid[] = {"SELECT TRUE OR '\xc0\x80' = 'ok'", "SELECT FALSE AND '\xed\xa0\x80' IS NULL"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
      reset_budget(); parse_mode(invalid[i], false);
      check_equal(compile(), TURBODB_STATUS_TYPE_ERROR); check_contains(error.message, "at byte");
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u); check_null(program.budget);
    }
    const char *unsupported[] = {"SELECT '1' = 1", "SELECT 'text'", "SELECT X'61' = 'a'", "SELECT 'a' COLLATE utf8_bin = 'a'"};
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
      reset_budget(); parse(unsupported[i]);
      /* COLLATE contributes a NAME node but is unsupported before input use. */
      check_equal(compile(), TURBODB_STATUS_UNSUPPORTED);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    reset_budget(); parse("SELECT 'a' <> 'A' AND 'a' <> 'a ' AND (NULL <=> 'a') = FALSE");
    check_equal(compile(), TURBODB_STATUS_OK); expect_truth(run(NULL, 0), 1);
  }

  it("enforces exact compilation step and work bounds for owned literals") {
    parse("SELECT 'text' = 'text'"); check_equal(compile(), TURBODB_STATUS_OK);
    const uint64_t peak = budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES];
    const uint64_t steps = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_greater(steps, 0u);
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = peak - 1; reset_budget();
    check_equal(compile(), TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = peak;
    limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = steps - 1; reset_budget();
    check_equal(compile(), TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = steps; reset_budget();
    check_equal(compile(), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], steps);
  }

  it("cleans owned literals at every selected compiler reserve and resize failure") {
    parse("SELECT 'one' = 'one' OR 'two' = 'three'");
    reserve_calls = resize_calls = 0; check_equal(compile(), TURBODB_STATUS_OK);
    const size_t reserves = reserve_calls, resizes = resize_calls;
    for (size_t i = 1; i <= reserves + resizes; ++i) {
      reset_budget(); reserve_calls = resize_calls = fail_reserve = fail_resize = 0;
      if (i <= reserves) fail_reserve = i; else fail_resize = i - reserves;
      check_equal(compile(), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    fail_reserve = fail_resize = 0; reset_budget(); check_equal(compile(), TURBODB_STATUS_OK); expect_truth(run(NULL, 0), 1);
  }

  it("evaluates scalar IN and NOT IN with matches duplicates and all NULL positions") {
    const char *sql[] = {"SELECT ? IN (?, ?)", "SELECT ? NOT IN (?, ?)"};
    const turbodb_value_t values[] = {turbodb_i64(1), turbodb_i64(2), turbodb_null()};
    enum { NULL_INDEX = 2, VALUE_COUNT = 3 };
    for (size_t op = 0; op < 2; ++op) {
      reset_budget(); parse(sql[op]);
      for (size_t i = 0; i < binding_count; ++i) bindings[i].type = (orm_sql_type){TURBODB_VALUE_INT64, true};
      check_equal(compile(), TURBODB_STATUS_OK);
      for (size_t a = 0; a < VALUE_COUNT; ++a)
        for (size_t b = 0; b < VALUE_COUNT; ++b) for (size_t d = 0; d < VALUE_COUNT; ++d) {
          const turbodb_value_t inputs[] = {values[a], values[b], values[d]};
          const bool matched = a != NULL_INDEX && (a == b || a == d);
          const bool unknown = a == NULL_INDEX || b == NULL_INDEX || d == NULL_INDEX;
          const int truth = matched ? 1 : unknown ? -1 : 0;
          expect_truth(run(inputs, VALUE_COUNT), truth < 0 ? -1 : op ? !truth : truth);
        }
    }
  }

  it("evaluates inclusive BETWEEN and NOT BETWEEN across NULL and reversed bounds") {
    const char *sql[] = {"SELECT ? BETWEEN ? AND ?", "SELECT ? NOT BETWEEN ? AND ?"};
    const turbodb_value_t values[] = {turbodb_i64(0), turbodb_i64(1), turbodb_null()};
    enum { NULL_INDEX = 2, VALUE_COUNT = 3 };
    for (size_t op = 0; op < 2; ++op) {
      reset_budget(); parse(sql[op]);
      for (size_t i = 0; i < binding_count; ++i) bindings[i].type = (orm_sql_type){TURBODB_VALUE_INT64, true};
      check_equal(compile(), TURBODB_STATUS_OK);
      for (size_t a = 0; a < VALUE_COUNT; ++a)
        for (size_t b = 0; b < VALUE_COUNT; ++b) for (size_t d = 0; d < VALUE_COUNT; ++d) {
          const turbodb_value_t inputs[] = {values[a], values[b], values[d]};
          const bool outside = a != NULL_INDEX &&
              ((b != NULL_INDEX && a < b) || (d != NULL_INDEX && a > d));
          const bool unknown = a == NULL_INDEX || b == NULL_INDEX || d == NULL_INDEX;
          const int truth = outside ? 0 : unknown ? -1 : 1;
          expect_truth(run(inputs, VALUE_COUNT), truth < 0 ? -1 : op ? !truth : truth);
        }
    }
  }

  it("preserves exact integers TEXT and nested compound branch targets after AST release") {
    const char *sql[] = {
      "SELECT 18446744073709551615 IN (-1, 18446744073709551615)",
      "SELECT 0 BETWEEN -9223372036854775808 AND 18446744073709551615",
      "SELECT 'b' BETWEEN 'a' AND 'b' AND 'b' NOT IN ('B', 'b ')",
      "SELECT TRUE IN (1 NOT IN (1, 2), 3 BETWEEN 2 AND 4)",
      "SELECT FALSE IN (TRUE NOT IN (TRUE, FALSE), FALSE) AND 2 NOT BETWEEN 3 AND 4",
      "SELECT 1 IN (1) AND 1 NOT IN (2) AND NULL IN (1, 2) IS NULL"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
      reset_budget(); parse(sql[i]); check_equal(compile(), TURBODB_STATUS_OK);
      sqlparser_document_destroy(document); document = NULL;
      expect_truth(run(NULL, 0), 1);
    }
    reset_budget(); parse("SELECT ? IN (?, ?) AND ? BETWEEN ? AND ?");
    for (size_t i = 0; i < binding_count; ++i) bindings[i].type = (orm_sql_type){TURBODB_VALUE_BLOB, false};
    check_equal(compile(), TURBODB_STATUS_OK);
    const turbodb_value_t blobs[] = {turbodb_blob("a\0", 2), turbodb_blob("a", 1), turbodb_blob("a\0", 2),
      turbodb_blob("b", 1), turbodb_blob("a", 1), turbodb_blob("b", 1)};
    expect_truth(run(blobs, sizeof(blobs) / sizeof(blobs[0])), 1);
  }

  it("short circuits list matches and false lower bounds but continues through UNKNOWN") {
    const char *sql[] = {"SELECT ? IN (?, ?)", "SELECT ? NOT IN (?, ?)",
      "SELECT ? BETWEEN ? AND ?", "SELECT ? NOT BETWEEN ? AND ?"};
    const char malformed[] = "\xc0\x80";
    for (size_t op = 0; op < sizeof(sql) / sizeof(sql[0]); ++op) {
      reset_budget(); parse(sql[op]);
      for (size_t i = 0; i < binding_count; ++i) bindings[i].type = (orm_sql_type){TURBODB_VALUE_TEXT, true};
      check_equal(compile(), TURBODB_STATUS_OK);
      turbodb_value_t inputs[] = {turbodb_text("a"), turbodb_text(op < 2 ? "a" : "b"),
        turbodb_text_v((vstr){malformed, sizeof(malformed) - 1})};
      expect_truth(run(inputs, binding_count), op == 0 || op == 3);
      inputs[1] = turbodb_null();
      turbodb_value_t out = turbodb_i64(SENTINEL);
      check_equal(orm_tidesdb_sql_expr_eval(&program, inputs, binding_count, &out, &error), TURBODB_STATUS_TYPE_ERROR);
      check_equal(out.data.int64_value, SENTINEL);
    }
  }

  it("rejects unsupported compound children even when a preceding result is decisive") {
    const char *sql[] = {"SELECT 1 IN (1, '1')", "SELECT 0 BETWEEN 1 AND '2'",
      "SELECT 1 IN (1, UNSUPPORTED_FN(1))", "SELECT 0 BETWEEN 1 AND UNSUPPORTED_FN(2)",
      "SELECT 1 IN (SELECT 1)"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
      reset_budget(); parse(sql[i]); check_equal(compile(), TURBODB_STATUS_UNSUPPORTED);
      check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    reset_budget(); parse("SELECT 'a' IN ('a', '\xc0\x80')");
    check_equal(compile(), TURBODB_STATUS_TYPE_ERROR);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
  }

  it("bounds wide lists by work and plan count without consuming extra expression depth") {
    enum { ITEMS = 256, PREFIX = sizeof("SELECT 0 IN (") - 1, ITEM_BYTES = 2 };
    char sql[PREFIX + ITEMS * ITEM_BYTES + 1];
    memcpy(sql, "SELECT 0 IN (", PREFIX);
    for (size_t i = 0; i < ITEMS; ++i) {
      sql[PREFIX + i * ITEM_BYTES] = '1';
      sql[PREFIX + i * ITEM_BYTES + 1] = i + 1 == ITEMS ? ')' : ',';
    }
    sql[sizeof(sql) - 1] = '\0'; parse(sql);
    check_equal(orm_tidesdb_sql_expr_compile(document, root, NULL, 0, 2, &budget, &program, &error), TURBODB_STATUS_OK);
    expect_truth(run(NULL, 0), 0);
    const uint64_t plans = budget.used.value[ORM_SQL_BUDGET_PLAN_NODES];
    reset_budget();
    check_equal(orm_tidesdb_sql_expr_compile(document, root, NULL, 0, 1, &budget, &program, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES] = plans - 1; reset_budget();
    check_equal(compile(), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES] = plans; reset_budget();
    check_equal(compile(), TURBODB_STATUS_OK);
  }

  it("evaluates the BETWEEN left input once and fails at the exact step boundary") {
    parse("SELECT ? BETWEEN 1 AND 3");
    bindings[0].type = (orm_sql_type){TURBODB_VALUE_INT64, false};
    /* One input, two constants, two comparisons, one branch, one AND. */
    enum { BETWEEN_STEPS = 7 };
    const turbodb_value_t input = turbodb_i64(2);
    limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = BETWEEN_STEPS - 1;
    reset_budget(); check_equal(compile(), TURBODB_STATUS_OK);
    turbodb_value_t out = turbodb_i64(SENTINEL);
    check_equal(orm_tidesdb_sql_expr_eval(&program, &input, 1, &out, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.data.int64_value, SENTINEL);
    limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = BETWEEN_STEPS;
    reset_budget(); check_equal(compile(), TURBODB_STATUS_OK); expect_truth(run(&input, 1), 1);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], BETWEEN_STEPS);
  }

  it("cleans compound compilation failures and evaluates without container growth") {
    parse("SELECT 'b' IN ('a', 'b', 'c') AND 'b' NOT BETWEEN 'c' AND 'd'");
    reserve_calls = resize_calls = 0; check_equal(compile(), TURBODB_STATUS_OK);
    const size_t reserves = reserve_calls, resizes = resize_calls;
    for (size_t i = 1; i <= reserves + resizes; ++i) {
      fail_reserve = fail_resize = 0; reset_budget(); reserve_calls = resize_calls = 0;
      if (i <= reserves) fail_reserve = i; else fail_resize = i - reserves;
      check_equal(compile(), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    fail_reserve = fail_resize = 0; reset_budget(); check_equal(compile(), TURBODB_STATUS_OK);
    orm_sql_expr_run workspace = {0};
    check_equal(orm_tidesdb_sql_expr_run_open(&program, &workspace, &error), TURBODB_STATUS_OK);
    reserve_calls = resize_calls = 0; fail_reserve = fail_resize = 1;
    turbodb_value_t out = turbodb_null();
    check_equal(orm_tidesdb_sql_expr_run_eval(&workspace, NULL, 0, &out, &error), TURBODB_STATUS_OK);
    expect_truth(out, 1); check_equal(reserve_calls, 0u); check_equal(resize_calls, 0u);
    check_equal(orm_tidesdb_sql_expr_run_close(&workspace, &error), TURBODB_STATUS_OK);
  }

  it("selects the first TRUE searched CASE condition and treats UNKNOWN as unmatched") {
    parse("SELECT CASE WHEN ? THEN TRUE WHEN ? THEN FALSE ELSE NULL END");
    check_equal(compile(), TURBODB_STATUS_OK);
    check_equal(program.result.kind, TURBODB_VALUE_BOOLEAN); check_equal(program.result.nullable, true);
    const turbodb_value_t conditions[] = {turbodb_bool(0), turbodb_bool(1), turbodb_null()};
    enum { CONDITIONS = 3, TRUE_INDEX = 1 };
    for (size_t a = 0; a < CONDITIONS; ++a) for (size_t b = 0; b < CONDITIONS; ++b) {
      const turbodb_value_t values[] = {conditions[a], conditions[b]};
      expect_truth(run(values, 2), a == TRUE_INDEX ? 1 : b == TRUE_INDEX ? 0 : -1);
    }
    reset_budget(); parse("SELECT CASE WHEN ? THEN TRUE END"); check_equal(compile(), TURBODB_STATUS_OK);
    check_equal(program.result.nullable, true);
    for (size_t a = 0; a < CONDITIONS; ++a) expect_truth(run(&conditions[a], 1), a == TRUE_INDEX ? 1 : -1);
    reset_budget(); parse("SELECT CASE WHEN NULL THEN FALSE ELSE TRUE END"); check_equal(compile(), TURBODB_STATUS_OK);
    check_equal(program.result.nullable, false); expect_truth(run(NULL, 0), 1);
    reset_budget(); parse("SELECT CASE WHEN TRUE THEN NULL ELSE NULL END"); check_equal(compile(), TURBODB_STATUS_OK);
    check_equal(program.result.kind, TURBODB_VALUE_NULL); check_equal(program.result.nullable, true);
    expect_truth(run(NULL, 0), -1);
  }

  it("uses ordinary equality and first-match ordering for simple CASE including NULL") {
    parse("SELECT CASE ? WHEN ? THEN TRUE WHEN ? THEN FALSE END");
    for (size_t i = 0; i < binding_count; ++i) bindings[i].type = (orm_sql_type){TURBODB_VALUE_INT64, true};
    check_equal(compile(), TURBODB_STATUS_OK);
    const turbodb_value_t candidates[] = {turbodb_i64(1), turbodb_i64(2), turbodb_null()};
    enum { CANDIDATES = 3, NULL_INDEX = 2 };
    for (size_t a = 0; a < CANDIDATES; ++a)
      for (size_t b = 0; b < CANDIDATES; ++b) for (size_t d = 0; d < CANDIDATES; ++d) {
        const turbodb_value_t values[] = {candidates[a], candidates[b], candidates[d]};
        expect_truth(run(values, 3), a == NULL_INDEX ? -1 : a == b ? 1 : a == d ? 0 : -1);
      }
    reset_budget(); parse("SELECT CASE 18446744073709551615 WHEN -1 THEN FALSE "
                          "WHEN 18446744073709551615 THEN TRUE ELSE FALSE END");
    check_equal(compile(), TURBODB_STATUS_OK); expect_truth(run(NULL, 0), 1);
  }

  it("keeps CASE result kinds exact for all supported scalar types") {
    const turbodb_value_t pairs[][2] = {{turbodb_bool(0), turbodb_bool(1)}, {turbodb_i64(INT64_MIN), turbodb_i64(INT64_MAX)},
      {turbodb_u64(0), turbodb_u64(UINT64_MAX)}, {turbodb_f64(0.5), turbodb_f64(1.5)},
      {turbodb_text_v((vstr){"a\0b", 3}), turbodb_text("a ")}, {turbodb_blob("a\0", 2), turbodb_blob("b", 1)}};
    parse("SELECT (CASE WHEN ? THEN ? ELSE ? END) <=> ?");
    for (size_t type = 0; type < sizeof(pairs) / sizeof(pairs[0]); ++type) {
      reset_budget(); bindings[0].type = (orm_sql_type){TURBODB_VALUE_BOOLEAN, false};
      for (size_t i = 1; i < binding_count; ++i) bindings[i].type = (orm_sql_type){pairs[type][0].kind, true};
      check_equal(compile(), TURBODB_STATUS_OK);
      for (unsigned condition = 0; condition < 2; ++condition) {
        turbodb_value_t values[] = {turbodb_bool(condition), pairs[type][0], pairs[type][1], pairs[type][!condition]};
        expect_truth(run(values, binding_count), 1);
        values[condition ? 1 : 2] = values[3] = turbodb_null();
        expect_truth(run(values, binding_count), 1);
      }
    }
  }

  it("skips unselected CASE conditions and results but reports selected invalid inputs") {
    parse("SELECT (CASE WHEN ? THEN 'ok' WHEN ? = 'ok' THEN ? ELSE ? END) = 'ok'");
    for (size_t i = 1; i < binding_count; ++i) bindings[i].type = (orm_sql_type){TURBODB_VALUE_TEXT, true};
    check_equal(compile(), TURBODB_STATUS_OK);
    const turbodb_value_t invalid = turbodb_text_v((vstr){"\xc0\x80", 2});
    turbodb_value_t values[] = {turbodb_bool(1), invalid, invalid, invalid};
    expect_truth(run(values, binding_count), 1);
    values[0] = turbodb_null();
    turbodb_value_t out = turbodb_i64(SENTINEL);
    check_equal(orm_tidesdb_sql_expr_eval(&program, values, binding_count, &out, &error), TURBODB_STATUS_TYPE_ERROR);
    check_equal(out.data.int64_value, SENTINEL);
    values[1] = turbodb_text("ok");
    check_equal(orm_tidesdb_sql_expr_eval(&program, values, binding_count, &out, &error), TURBODB_STATUS_TYPE_ERROR);
    values[1] = turbodb_text("other"); values[3] = turbodb_text("ok");
    expect_truth(run(values, binding_count), 1);
    reset_budget(); parse("SELECT CASE 'ok' WHEN 'ok' THEN TRUE WHEN ? THEN ? ELSE ? END");
    bindings[0].type = (orm_sql_type){TURBODB_VALUE_TEXT, true}; check_equal(compile(), TURBODB_STATUS_OK);
    const turbodb_value_t skipped[] = {invalid, turbodb_i64(1), turbodb_i64(1)};
    expect_truth(run(skipped, binding_count), 1);
  }

  it("binds all CASE conditions and result types before executing any branch") {
    const char *sql[] = {"SELECT CASE WHEN 1 THEN TRUE ELSE FALSE END",
      "SELECT CASE WHEN TRUE THEN TRUE WHEN 1 THEN FALSE END",
      "SELECT (CASE WHEN TRUE THEN 1 ELSE '1' END) = 1",
      "SELECT (CASE WHEN TRUE THEN -1 ELSE 18446744073709551615 END) = -1",
      "SELECT CASE 1 WHEN 1 THEN TRUE WHEN '2' THEN FALSE END",
      "SELECT CASE WHEN TRUE THEN TRUE ELSE UNSUPPORTED_FN(1) = 1 END"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
      reset_budget(); parse(sql[i]); check_equal(compile(), TURBODB_STATUS_UNSUPPORTED);
      check_contains(error.message, "at byte"); check_null(program.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    reset_budget(); parse("SELECT (CASE WHEN TRUE THEN 'ok' ELSE '\xc0\x80' END) = 'ok'");
    check_equal(compile(), TURBODB_STATUS_TYPE_ERROR);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
  }

  it("preserves nested CASE branch targets and literal lifetimes across reusable runs") {
    parse("SELECT (CASE CASE WHEN ? THEN 'a' ELSE 'b' END "
          "WHEN 'a' THEN CASE WHEN FALSE THEN 'bad' ELSE 'ok' END "
          "WHEN 'b' THEN CASE 2 WHEN 1 THEN 'bad' WHEN 2 THEN 'ok' END END) IN ('ok', 'other')");
    check_equal(compile(), TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document = NULL;
    orm_sql_expr_run workspace = {0};
    check_equal(orm_tidesdb_sql_expr_run_open(&program, &workspace, &error), TURBODB_STATUS_OK);
    reserve_calls = resize_calls = 0; fail_reserve = fail_resize = 1;
    const turbodb_value_t conditions[] = {turbodb_bool(1), turbodb_bool(0), turbodb_null()};
    for (size_t i = 0; i < sizeof(conditions) / sizeof(conditions[0]); ++i) {
      turbodb_value_t out = turbodb_null();
      check_equal(orm_tidesdb_sql_expr_run_eval(&workspace, &conditions[i], 1, &out, &error), TURBODB_STATUS_OK);
      expect_truth(out, 1);
      for (size_t r = 0; r < program.register_count; ++r)
        check_equal(((const turbodb_value_t *)vec_at_const(&workspace.registers, r))->kind, TURBODB_VALUE_NULL);
    }
    check_equal(reserve_calls, 0u); check_equal(resize_calls, 0u);
    check_equal(orm_tidesdb_sql_expr_destroy(&program, &error), TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_expr_run_close(&workspace, &error), TURBODB_STATUS_OK);
  }

  it("charges simple CASE operand once and preserves output on step exhaustion") {
    parse("SELECT CASE ? WHEN 1 THEN FALSE WHEN 2 THEN TRUE ELSE FALSE END");
    bindings[0].type = (orm_sql_type){TURBODB_VALUE_INT64, false};
    /* Input once; two (constant, equality, skip) groups; result and copy-jump. */
    enum { STEPS = 9 };
    limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = STEPS - 1; reset_budget();
    check_equal(compile(), TURBODB_STATUS_OK);
    const turbodb_value_t input = turbodb_i64(2);
    turbodb_value_t out = turbodb_i64(SENTINEL);
    check_equal(orm_tidesdb_sql_expr_eval(&program, &input, 1, &out, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.data.int64_value, SENTINEL);
    limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = STEPS; reset_budget();
    check_equal(compile(), TURBODB_STATUS_OK); expect_truth(run(&input, 1), 1);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], STEPS);
  }

  it("bounds wide CASE branches with exact AST plan and work admission") {
    const char prefix[] = "SELECT CASE ", branch[] = "WHEN FALSE THEN TRUE ", suffix[] = "ELSE TRUE END";
    enum { BRANCHES = 64, BRANCH_NODES = 3, ROOT_AND_ELSE = 2 };
    char sql[sizeof(prefix) - 1 + BRANCHES * (sizeof(branch) - 1) + sizeof(suffix)];
    size_t offset = sizeof(prefix) - 1; memcpy(sql, prefix, offset);
    for (size_t i = 0; i < BRANCHES; ++i) { memcpy(sql + offset, branch, sizeof(branch) - 1); offset += sizeof(branch) - 1; }
    memcpy(sql + offset, suffix, sizeof(suffix)); parse(sql);
    check_equal(orm_tidesdb_sql_expr_compile(document, root, NULL, 0, 2, &budget, &program, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_AST_NODES], BRANCHES * BRANCH_NODES + ROOT_AND_ELSE);
    const uint64_t peak = budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES];
    const uint64_t plans = budget.used.value[ORM_SQL_BUDGET_PLAN_NODES];
    expect_truth(run(NULL, 0), 1);
    const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_WORK_BYTES, ORM_SQL_BUDGET_PLAN_NODES, ORM_SQL_BUDGET_AST_NODES};
    const uint64_t exact[] = {peak, plans, BRANCHES * BRANCH_NODES + ROOT_AND_ELSE};
    for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); ++i) {
      const uint64_t original = limits.statement.value[resources[i]];
      limits.statement.value[resources[i]] = exact[i] - 1; reset_budget();
      check_equal(orm_tidesdb_sql_expr_compile(document, root, NULL, 0, 2, &budget, &program, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
      limits.statement.value[resources[i]] = exact[i]; reset_budget();
      check_equal(orm_tidesdb_sql_expr_compile(document, root, NULL, 0, 2, &budget, &program, &error), TURBODB_STATUS_OK);
      limits.statement.value[resources[i]] = original;
    }
  }

  it("compiles nested CASE without C recursion and enforces expression depth") {
    const char prefix[] = "SELECT ", branch[] = "CASE WHEN TRUE THEN ", suffix[] = " ELSE FALSE END";
    enum { DEPTH = 128 };
    char sql[sizeof(prefix) - 1 + DEPTH * (sizeof(branch) + sizeof(suffix) - 2) + sizeof("TRUE")];
    size_t offset = sizeof(prefix) - 1; memcpy(sql, prefix, offset);
    for (size_t i = 0; i < DEPTH; ++i) { memcpy(sql + offset, branch, sizeof(branch) - 1); offset += sizeof(branch) - 1; }
    memcpy(sql + offset, "TRUE", sizeof("TRUE") - 1); offset += sizeof("TRUE") - 1;
    for (size_t i = 0; i < DEPTH; ++i) { memcpy(sql + offset, suffix, sizeof(suffix) - 1); offset += sizeof(suffix) - 1; }
    sql[offset] = '\0'; parse(sql);
    check_equal(orm_tidesdb_sql_expr_compile(document, root, NULL, 0, DEPTH, &budget, &program, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u); reset_budget();
    check_equal(orm_tidesdb_sql_expr_compile(document, root, NULL, 0, DEPTH + 1, &budget, &program, &error), TURBODB_STATUS_OK);
    expect_truth(run(NULL, 0), 1);
  }

  it("cleans every selected CASE compilation allocation failure") {
    parse("SELECT (CASE 'b' WHEN 'a' THEN 'x' WHEN 'b' THEN CASE WHEN TRUE THEN 'y' END ELSE 'z' END) = 'y'");
    reserve_calls = resize_calls = 0; check_equal(compile(), TURBODB_STATUS_OK);
    const size_t reserves = reserve_calls, resizes = resize_calls;
    for (size_t i = 1; i <= reserves + resizes; ++i) {
      fail_reserve = fail_resize = 0; reset_budget(); reserve_calls = resize_calls = 0;
      if (i <= reserves) fail_reserve = i; else fail_resize = i - reserves;
      check_equal(compile(), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    fail_reserve = fail_resize = 0; reset_budget(); check_equal(compile(), TURBODB_STATUS_OK); expect_truth(run(NULL, 0), 1);
  }

  it("selects the first non-NULL value including FALSE and preserves all-NULL results") {
    const char *sql[] = {"SELECT COALESCE(?, ?, ?)", "SELECT IFNULL(?, ?)",
      "SELECT ifnull(?, cOaLeScE(?, ?))"};
    const turbodb_value_t values[] = {turbodb_bool(0), turbodb_bool(1), turbodb_null()};
    enum { VALUES = 3, NULL_INDEX = 2 };
    for (size_t op = 0; op < sizeof(sql) / sizeof(sql[0]); ++op) {
      reset_budget(); parse(sql[op]); check_equal(compile(), TURBODB_STATUS_OK);
      for (size_t a = 0; a < VALUES; ++a) for (size_t b = 0; b < VALUES; ++b)
        for (size_t d = 0; d < VALUES; ++d) {
          const turbodb_value_t inputs[] = {values[a], values[b], values[d]};
          const size_t selected = a != NULL_INDEX ? a : b != NULL_INDEX ? b : op == 1 ? NULL_INDEX : d;
          expect_truth(run(inputs, binding_count), selected == NULL_INDEX ? -1 : (int)selected);
        }
    }
    reset_budget(); parse("SELECT COALESCE(NULL)"); check_equal(compile(), TURBODB_STATUS_OK);
    check_equal(program.result.kind, TURBODB_VALUE_NULL); expect_truth(run(NULL, 0), -1);
    reset_budget(); parse("SELECT COALESCE(TRUE)"); check_equal(compile(), TURBODB_STATUS_OK); expect_truth(run(NULL, 0), 1);
  }

  it("makes COALESCE nullable only when every declared argument is nullable") {
    enum { ARGUMENTS = 3, COMBINATIONS = 1 << ARGUMENTS };
    parse("SELECT COALESCE(?, ?, ?)");
    for (unsigned mask = 0; mask < COMBINATIONS; ++mask) {
      reset_budget();
      for (size_t i = 0; i < binding_count; ++i) bindings[i].type = (orm_sql_type){TURBODB_VALUE_BOOLEAN, (mask & (1u << i)) != 0};
      check_equal(compile(), TURBODB_STATUS_OK);
      check_equal(program.result.kind, TURBODB_VALUE_BOOLEAN);
      check_equal(program.result.nullable, mask == COMBINATIONS - 1);
    }
    reset_budget(); parse("SELECT IFNULL(NULL, FALSE)"); check_equal(compile(), TURBODB_STATUS_OK);
    check_equal(program.result.nullable, false); expect_truth(run(NULL, 0), 0);
    reset_budget(); parse("SELECT IFNULL(NULL, NULL)"); check_equal(compile(), TURBODB_STATUS_OK);
    check_equal(program.result.kind, TURBODB_VALUE_NULL); check_equal(program.result.nullable, true);
  }

  it("uses ordinary NULLIF equality and returns the original first argument on UNKNOWN") {
    parse("SELECT NULLIF(?, ?)"); check_equal(compile(), TURBODB_STATUS_OK);
    check_equal(program.result.kind, TURBODB_VALUE_BOOLEAN); check_equal(program.result.nullable, true);
    const turbodb_value_t values[] = {turbodb_bool(0), turbodb_bool(1), turbodb_null()};
    enum { VALUES = 3, NULL_INDEX = 2 };
    for (size_t a = 0; a < VALUES; ++a) for (size_t b = 0; b < VALUES; ++b) {
      const turbodb_value_t inputs[] = {values[a], values[b]};
      expect_truth(run(inputs, 2), a == NULL_INDEX || a == b ? -1 : (int)a);
    }
    reset_budget(); parse("SELECT NULLIF(-1, 18446744073709551615) = -1 "
                          "AND NULLIF(18446744073709551615, -1) = 18446744073709551615");
    check_equal(compile(), TURBODB_STATUS_OK); expect_truth(run(NULL, 0), 1);
    reset_budget(); parse("SELECT NULLIF(NULL, 'x')"); check_equal(compile(), TURBODB_STATUS_OK);
    check_equal(program.result.kind, TURBODB_VALUE_NULL); expect_truth(run(NULL, 0), -1);
  }

  it("retains exact scalar kinds and bytes through NULL handling functions") {
    const turbodb_value_t pairs[][2] = {{turbodb_i64(INT64_MIN), turbodb_i64(INT64_MAX)},
      {turbodb_u64(0), turbodb_u64(UINT64_MAX)}, {turbodb_f64(0.5), turbodb_f64(1.5)},
      {turbodb_text_v((vstr){"a\0b", 3}), turbodb_text("a ")}, {turbodb_blob("a\0", 2), turbodb_blob("b", 1)}};
    const char *sql[] = {"SELECT COALESCE(?, ?) <=> ?", "SELECT IFNULL(?, ?) <=> ?", "SELECT NULLIF(?, ?) <=> ?"};
    for (size_t op = 0; op < sizeof(sql) / sizeof(sql[0]); ++op)
      for (size_t type = 0; type < sizeof(pairs) / sizeof(pairs[0]); ++type) {
        reset_budget(); parse(sql[op]);
        for (size_t i = 0; i < binding_count; ++i) bindings[i].type = (orm_sql_type){pairs[type][0].kind, true};
        check_equal(compile(), TURBODB_STATUS_OK);
        turbodb_value_t inputs[] = {pairs[type][0], pairs[type][1], pairs[type][0]};
        expect_truth(run(inputs, binding_count), 1);
        inputs[0] = turbodb_null(); inputs[2] = op == 2 ? turbodb_null() : pairs[type][1];
        expect_truth(run(inputs, binding_count), 1);
        inputs[0] = inputs[1]; inputs[2] = op == 2 ? turbodb_null() : inputs[0];
        expect_truth(run(inputs, binding_count), 1);
      }
  }

  it("short circuits NULL replacement but evaluates both NULLIF arguments") {
    const char *sql[] = {"SELECT COALESCE(?, ?) = 'ok'", "SELECT IFNULL(?, ?) = 'ok'"};
    const turbodb_value_t invalid = turbodb_text_v((vstr){"\xc0\x80", 2});
    for (size_t op = 0; op < sizeof(sql) / sizeof(sql[0]); ++op) {
      reset_budget(); parse(sql[op]);
      for (size_t i = 0; i < binding_count; ++i) bindings[i].type = (orm_sql_type){TURBODB_VALUE_TEXT, true};
      check_equal(compile(), TURBODB_STATUS_OK);
      turbodb_value_t inputs[] = {turbodb_text("ok"), invalid}; expect_truth(run(inputs, 2), 1);
      inputs[0] = turbodb_null(); turbodb_value_t out = turbodb_i64(SENTINEL);
      check_equal(orm_tidesdb_sql_expr_eval(&program, inputs, 2, &out, &error), TURBODB_STATUS_TYPE_ERROR);
      check_equal(out.data.int64_value, SENTINEL);
      inputs[1] = turbodb_text("ok"); expect_truth(run(inputs, 2), 1);
    }
    reset_budget(); parse("SELECT NULLIF(?, ?) IS NULL");
    for (size_t i = 0; i < binding_count; ++i) bindings[i].type = (orm_sql_type){TURBODB_VALUE_TEXT, true};
    check_equal(compile(), TURBODB_STATUS_OK);
    const turbodb_value_t inputs[] = {turbodb_null(), invalid}; turbodb_value_t out = turbodb_i64(SENTINEL);
    check_equal(orm_tidesdb_sql_expr_eval(&program, inputs, 2, &out, &error), TURBODB_STATUS_TYPE_ERROR);
    check_equal(out.data.int64_value, SENTINEL);
  }

  it("rejects unknown functions modifiers wrong arity and unreachable mixed types") {
    const char *unsupported[] = {"SELECT COALESCE(TRUE, 1)", "SELECT IFNULL(1, '1') = 1",
      "SELECT NULLIF(1, '1') IS NULL", "SELECT COALESCE(TRUE, UNSUPPORTED_FN(1) = 1)",
      "SELECT COALESCE(DISTINCT TRUE)", "SELECT COALESCE(*)", "SELECT `COALESCE`(TRUE)",
      "SELECT COALESCE(1, 18446744073709551615) = 1"};
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); ++i) {
      reset_budget(); parse(unsupported[i]); check_equal(compile(), TURBODB_STATUS_UNSUPPORTED);
      check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    const char *arity[] = {"SELECT COALESCE()", "SELECT IFNULL(TRUE)", "SELECT IFNULL(TRUE, FALSE, NULL)",
      "SELECT NULLIF()", "SELECT NULLIF(TRUE)", "SELECT NULLIF(TRUE, FALSE, NULL)"};
    for (size_t i = 0; i < sizeof(arity) / sizeof(arity[0]); ++i) {
      reset_budget(); parse(arity[i]); check_equal(compile(), TURBODB_STATUS_SQL_ERROR);
      check_contains(error.message, "argument count"); check_null(program.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    reset_budget(); parse("SELECT COALESCE('ok', '\xc0\x80') = 'ok'");
    check_equal(compile(), TURBODB_STATUS_TYPE_ERROR);
  }

  it("reuses nested conditional function registers after the document is destroyed") {
    parse("SELECT COALESCE(NULLIF(IFNULL(?, 'none'), 'none'), "
          "CASE WHEN ? THEN 'ok' ELSE 'bad' END) IN ('ok', 'yes')");
    bindings[0].type = (orm_sql_type){TURBODB_VALUE_TEXT, true}; check_equal(compile(), TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document = NULL;
    orm_sql_expr_run workspace = {0};
    check_equal(orm_tidesdb_sql_expr_run_open(&program, &workspace, &error), TURBODB_STATUS_OK);
    reserve_calls = resize_calls = 0; fail_reserve = fail_resize = 1;
    const turbodb_value_t inputs[][2] = {{turbodb_null(), turbodb_bool(1)}, {turbodb_text("yes"), turbodb_bool(0)}, {turbodb_text("none"), turbodb_null()}};
    const int expected[] = {1, 1, 0};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
      turbodb_value_t out = turbodb_null();
      check_equal(orm_tidesdb_sql_expr_run_eval(&workspace, inputs[i], 2, &out, &error), TURBODB_STATUS_OK);
      expect_truth(out, expected[i]);
      for (size_t r = 0; r < program.register_count; ++r)
        check_equal(((const turbodb_value_t *)vec_at_const(&workspace.registers, r))->kind, TURBODB_VALUE_NULL);
    }
    check_equal(reserve_calls, 0u); check_equal(resize_calls, 0u);
    check_equal(orm_tidesdb_sql_expr_run_close(&workspace, &error), TURBODB_STATUS_OK);
  }

  it("bounds wide COALESCE arguments without consuming additional expression depth") {
    const char prefix[] = "SELECT COALESCE(", argument[] = "NULL,", suffix[] = "TRUE)";
    enum { NULL_ARGUMENTS = 128, CALL_NAME_AND_LAST = 3 };
    char sql[sizeof(prefix) - 1 + NULL_ARGUMENTS * (sizeof(argument) - 1) + sizeof(suffix)];
    size_t offset = sizeof(prefix) - 1; memcpy(sql, prefix, offset);
    for (size_t i = 0; i < NULL_ARGUMENTS; ++i) { memcpy(sql + offset, argument, sizeof(argument) - 1); offset += sizeof(argument) - 1; }
    memcpy(sql + offset, suffix, sizeof(suffix)); parse(sql);
    check_equal(orm_tidesdb_sql_expr_compile(document, root, NULL, 0, 2, &budget, &program, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_AST_NODES], NULL_ARGUMENTS + CALL_NAME_AND_LAST);
    const uint64_t plans = budget.used.value[ORM_SQL_BUDGET_PLAN_NODES]; expect_truth(run(NULL, 0), 1);
    reset_budget();
    check_equal(orm_tidesdb_sql_expr_compile(document, root, NULL, 0, 1, &budget, &program, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES] = plans - 1; reset_budget();
    check_equal(compile(), TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES] = plans; reset_budget(); check_equal(compile(), TURBODB_STATUS_OK);
    reset_budget(); parse("SELECT IFNULL(NULL, COALESCE(NULL, IFNULL(NULL, TRUE)))");
    check_equal(orm_tidesdb_sql_expr_compile(document, root, NULL, 0, 3, &budget, &program, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    reset_budget(); check_equal(orm_tidesdb_sql_expr_compile(document, root, NULL, 0, 4, &budget, &program, &error), TURBODB_STATUS_OK);
    expect_truth(run(NULL, 0), 1);
  }

  it("charges function selection instructions and evaluates NULLIF first input once") {
    const char *sql[] = {"SELECT COALESCE(?, FALSE)", "SELECT IFNULL(?, FALSE)", "SELECT NULLIF(?, FALSE)"};
    const uint64_t steps[] = {2, 2, 4};
    const turbodb_value_t value = turbodb_bool(1);
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
      limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = steps[i] - 1;
      reset_budget(); parse(sql[i]); check_equal(compile(), TURBODB_STATUS_OK);
      turbodb_value_t out = turbodb_i64(SENTINEL);
      check_equal(orm_tidesdb_sql_expr_eval(&program, &value, 1, &out, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(out.data.int64_value, SENTINEL);
      limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = steps[i]; reset_budget();
      check_equal(compile(), TURBODB_STATUS_OK); expect_truth(run(&value, 1), 1);
      check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], steps[i]);
    }
  }

  it("cleans every selected conditional function compilation failure") {
    parse("SELECT COALESCE(NULLIF('a', 'a'), IFNULL(NULL, 'b')) = 'b'");
    reserve_calls = resize_calls = 0; check_equal(compile(), TURBODB_STATUS_OK);
    const size_t reserves = reserve_calls, resizes = resize_calls;
    for (size_t i = 1; i <= reserves + resizes; ++i) {
      fail_reserve = fail_resize = 0; reset_budget(); reserve_calls = resize_calls = 0;
      if (i <= reserves) fail_reserve = i; else fail_resize = i - reserves;
      check_equal(compile(), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    fail_reserve = fail_resize = 0; reset_budget(); check_equal(compile(), TURBODB_STATUS_OK); expect_truth(run(NULL, 0), 1);
  }

  it("compiles numeric precedence unary expressions and exact signed literal boundaries") {
    const char *sql[] = {"SELECT 2 + 3 * 4 = 14 AND (2 + 3) * 4 = 20",
      "SELECT -(1 + 2) = -3 AND +(2 * 3) = 6 AND -(-7) = 7",
      "SELECT -9223372036854775808 + 1 = -9223372036854775807",
      "SELECT -2 * 4611686018427387904 = -9223372036854775808",
      "SELECT 18446744073709551615 - 18446744073709551615 = 0",
      "SELECT NULL + 1 IS NULL AND -(NULL * NULL) IS NULL"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
      reset_budget(); parse(sql[i]); check_equal(compile(), TURBODB_STATUS_OK);
      sqlparser_document_destroy(document); document = NULL; expect_truth(run(NULL, 0), 1);
    }
  }

  it("binds typed F64 and U64 arithmetic parameters without implicit coercion") {
    parse("SELECT (? + ?) * -? < ?");
    for (size_t i = 0; i < binding_count; ++i) bindings[i].type = (orm_sql_type){TURBODB_VALUE_DOUBLE, true};
    check_equal(compile(), TURBODB_STATUS_OK);
    turbodb_value_t values[] = {turbodb_f64(1.5), turbodb_f64(0.5), turbodb_f64(2.0), turbodb_f64(-3.0)};
    expect_truth(run(values, binding_count), 1); values[0] = turbodb_null(); expect_truth(run(values, binding_count), -1);
    reset_budget(); parse("SELECT +(? - ?) = ?");
    for (size_t i = 0; i < binding_count; ++i) bindings[i].type = (orm_sql_type){TURBODB_VALUE_UINT64, false};
    check_equal(compile(), TURBODB_STATUS_OK);
    const turbodb_value_t unsigned_values[] = {turbodb_u64(UINT64_MAX), turbodb_u64(1), turbodb_u64(UINT64_MAX - 1)};
    expect_truth(run(unsigned_values, binding_count), 1);
    reset_budget(); parse("SELECT -? = 0"); bindings[0].type = (orm_sql_type){TURBODB_VALUE_UINT64, false};
    check_equal(compile(), TURBODB_STATUS_UNSUPPORTED);
  }

  it("evaluates arithmetic overflow only on reached paths and reports its source offset") {
    const char *skipped[] = {"SELECT TRUE OR 9223372036854775807 + 1 = 0",
      "SELECT NOT (FALSE AND -9223372036854775808 * -1 = 0)",
      "SELECT CASE WHEN TRUE THEN TRUE ELSE 9223372036854775807 * 2 = 0 END",
      "SELECT COALESCE(TRUE, -(-9223372036854775808) = 0)"};
    for (size_t i = 0; i < sizeof(skipped) / sizeof(skipped[0]); ++i) {
      reset_budget(); parse(skipped[i]); check_equal(compile(), TURBODB_STATUS_OK); expect_truth(run(NULL, 0), 1);
    }
    const char *reached[] = {"SELECT 9223372036854775807 + 1 = 0",
      "SELECT -9223372036854775808 * -1 = 0", "SELECT -(-9223372036854775808) = 0",
      "SELECT NULL + (9223372036854775807 + 1) IS NULL"};
    for (size_t i = 0; i < sizeof(reached) / sizeof(reached[0]); ++i) {
      reset_budget(); parse(reached[i]); check_equal(compile(), TURBODB_STATUS_OK);
      const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      turbodb_value_t out = turbodb_i64(SENTINEL);
      check_equal(orm_tidesdb_sql_expr_eval(&program, NULL, 0, &out, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(out.data.int64_value, SENTINEL); check_contains(error.message, "at byte");
      check_contains(error.message, "arithmetic result");
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
    }
  }

  it("refuses mixed arithmetic types and unsupported division even in dead branches") {
    const char *sql[] = {"SELECT FALSE AND 1 + '2' = 3", "SELECT TRUE OR TRUE * 1 = 1",
      "SELECT 18446744073709551615 - 1 = 0", "SELECT -'1' = -1", "SELECT 1 / 2 = 0", "SELECT 3.0 DIV 2.0 = 1"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
      reset_budget(); parse(sql[i]); check_equal(compile(), TURBODB_STATUS_UNSUPPORTED);
      check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
  }

  it("rejects exact slash and noninteger DIV before expression execution") {
    const char *sql[]={"SELECT 5 / 2 = 2", "SELECT 5.0 DIV 2.0 = 2",
      "SELECT TRUE OR 5 / 0 = 0", "SELECT FALSE AND 5 DIV 2.0 = 0"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      reset_budget(); parse(sql[i]); check_equal(compile(),TURBODB_STATUS_UNSUPPORTED);
      check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    }
  }

  group("mixed real predicate execution") {
    it("uses promoted comparisons in lists simple CASE and NULLIF after releasing the AST") {
      const char *sql[]={"SELECT 7=7.0","SELECT 7.5>7","SELECT -7<-6.5",
        "SELECT 9007199254740993=9007199254740992.0",
        "SELECT 9007199254740993 IN(9007199254740992,9007199254740992.0)",
        "SELECT 8 NOT IN(7.0,NULL) IS NULL",
        "SELECT CASE 7 WHEN 7.0 THEN TRUE ELSE FALSE END",
        "SELECT NULLIF(9007199254740993,9007199254740992.0) IS NULL",
        "SELECT NULLIF(7.0,8)=7.0","SELECT NULL<=>7.0=FALSE",
        "SELECT TRUE OR (7.0/0)>1"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        reset_budget();parse(sql[i]);check_equal(compile(),TURBODB_STATUS_OK);
        sqlparser_document_destroy(document);document=NULL;expect_truth(run(NULL,0),1);
      }
    }
    it("aggregates every BETWEEN operand type before either bound is evaluated") {
      const struct {const char *sql;int truth;} cases[]={
        {"SELECT 9007199254740992 BETWEEN 9007199254740993 AND 9007199254740994.0",1},
        {"SELECT 9007199254740993 BETWEEN 0.0 AND 9007199254740992",1},
        {"SELECT 9007199254740992 NOT BETWEEN 9007199254740993 AND 9007199254740994.0",0},
        {"SELECT 9007199254740992 BETWEEN 9007199254740993 AND 9007199254740994",0},
        {"SELECT 18446744073709551615 BETWEEN 0 AND 18446744073709551616.0",1},
        {"SELECT 7 BETWEEN NULL AND 8.0",-1},{"SELECT 7 BETWEEN NULL AND 6.0",0},
        {"SELECT 7 BETWEEN 8.0 AND NULL",0},{"SELECT NULL BETWEEN 1 AND 2.0",-1},
        {"SELECT NULL BETWEEN TRUE AND 2.0",-1},
        {"SELECT (7 BETWEEN 6 AND 8.0) BETWEEN FALSE AND TRUE",1}
      };
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        reset_budget();parse(cases[i].sql);check_equal(compile(),TURBODB_STATUS_OK);expect_truth(run(NULL,0),cases[i].truth);
      }
      const char *rejected[]={"SELECT 7.0 BETWEEN NULL AND TRUE","SELECT FALSE AND 7.0='7'",
        "SELECT CASE WHEN TRUE THEN 7 ELSE TRUE END=7","SELECT 7.0 IN(TRUE)"};
      for(size_t i=0;i<sizeof(rejected)/sizeof(rejected[0]);++i) {
        reset_budget();parse(rejected[i]);check_equal(compile(),TURBODB_STATUS_UNSUPPORTED);check_null(program.budget);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
      }
    }
    it("retains original parameter kinds and reuses BETWEEN comparison mode across evaluations") {
      parse("SELECT ? BETWEEN ? AND ?");
      turbodb_value_t values[]={turbodb_i64(INT64_C(9007199254740992)),turbodb_u64(UINT64_C(9007199254740993)),turbodb_f64(9007199254740994.0)};
      for(size_t i=0;i<binding_count;++i)bindings[i].type=(orm_sql_type){values[i].kind,true};
      check_equal(compile(),TURBODB_STATUS_OK);expect_truth(run(values,binding_count),1);
      values[2]=turbodb_f64(1.0);expect_truth(run(values,binding_count),0);
      values[2]=turbodb_null();expect_truth(run(values,binding_count),-1);
      values[0]=turbodb_f64(9007199254740992.0);turbodb_value_t out=turbodb_i64(SENTINEL);
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(orm_tidesdb_sql_expr_eval(&program,values,binding_count,&out,&error),TURBODB_STATUS_TYPE_ERROR);
      check_equal(out.data.int64_value,SENTINEL);check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    it("distinguishes exact and promoted comparisons when testing expression identity") {
      parse("SELECT ? BETWEEN ? AND ?");
      for(size_t i=0;i<binding_count;++i)bindings[i].type=(orm_sql_type){i==2?TURBODB_VALUE_DOUBLE:TURBODB_VALUE_INT64,false};
      check_equal(compile(),TURBODB_STATUS_OK);orm_sql_expr other={0};
      check_equal(orm_tidesdb_sql_expr_compile(document,root,bindings,binding_count,TEST_DEPTH,&budget,&other,&error),TURBODB_STATUS_OK);
      const size_t slots[]={0,1,2};bool same=false;
      check_equal(orm_tidesdb_sql_expr_same(&program,slots,&other,slots,&same,&error),TURBODB_STATUS_OK);check_true(same);
      for(size_t i=0;i<vec_size(&other.code);++i) {
        expr_instruction *instruction=vec_at(&other.code,i);
        if(instruction->opcode==EXPR_PREDICATE&&instruction->predicate.op==ORM_SQL_GREATER_EQUAL) {
          instruction->predicate.real_comparison=false;break;
        }
      }
      check_equal(orm_tidesdb_sql_expr_same(&program,slots,&other,slots,&same,&error),TURBODB_STATUS_OK);check_false(same);
      check_equal(orm_tidesdb_sql_expr_destroy(&other,&error),TURBODB_STATUS_OK);
    }
  }

  group("mixed real expression execution") {
    it("returns DOUBLE values from both operand orders after destroying the AST") {
      const struct {const char *sql; double result;} cases[]={
        {"SELECT 7+2.5",9.5},{"SELECT 2.5+7",9.5},{"SELECT 7-2.5",4.5},{"SELECT 2.5-7",-4.5},
        {"SELECT -7*2.5",-17.5},{"SELECT -7/2.0",-3.5},{"SELECT MOD(-7,2.5)",-2.0},
        {"SELECT MOD(-7.5,-2)",-1.5},{"SELECT 18446744073709551615 % 2.0",0.0}
      };
      for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        reset_budget(); parse(cases[i].sql);
        check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
        check_equal(program.result.kind,TURBODB_VALUE_DOUBLE); sqlparser_document_destroy(document); document=NULL;
        const turbodb_value_t result=run(NULL,0); check_equal(result.kind,TURBODB_VALUE_DOUBLE); check_equal(result.data.double_value,cases[i].result);
      }
    }
    it("retains the original parameter kinds in nested promoted expressions") {
      parse("SELECT MOD(?/?,?)+?"); const turbodb_value_t values[]={turbodb_i64(-15),turbodb_f64(2.0),turbodb_f64(2.0),turbodb_u64(1)};
      for(size_t i=0;i<binding_count;++i) bindings[i].type=(orm_sql_type){values[i].kind,true};
      check_equal(orm_tidesdb_sql_expr_compile_value(document,root,bindings,binding_count,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
      const turbodb_value_t result=run(values,binding_count); check_equal(result.kind,TURBODB_VALUE_DOUBLE); check_equal(result.data.double_value,-0.5);
      turbodb_value_t changed[TEST_INPUTS]; memcpy(changed,values,sizeof(values)); changed[0]=turbodb_f64(-15.0);
      turbodb_value_t out=turbodb_i64(SENTINEL); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(orm_tidesdb_sql_expr_eval(&program,changed,binding_count,&out,&error),TURBODB_STATUS_TYPE_ERROR);
      check_equal(out.data.int64_value,SENTINEL); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    it("keeps statement zero policies and lazy evaluation through promotion") {
      enum { RECORDS = 2 };
      check_equal(orm_sql_diagnostics_init(&numeric_diagnostics,RECORDS,&error),TURBODB_STATUS_OK);
      const char *sql[]={"SELECT MOD(7,0.0)","SELECT 7.5/0", "SELECT MOD(NULL,0.0)",
        "SELECT CASE WHEN TRUE THEN 1.5 ELSE 7/0.0 END"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        reset_budget(); parse(sql[i]); check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
        for(size_t mode=0;mode<=ORM_SQL_EVALUATION_IGNORE_WRITE;++mode) {
          check_equal(orm_sql_diagnostics_reset(&numeric_diagnostics,&error),TURBODB_STATUS_OK);
          const orm_sql_evaluation evaluation={&numeric_diagnostics,(orm_sql_evaluation_mode)mode}; turbodb_value_t out=turbodb_i64(SENTINEL);
          const bool fail=i<2&&mode==ORM_SQL_EVALUATION_WRITE;
          check_equal(orm_tidesdb_sql_expr_eval_evaluation(&program,NULL,0,evaluation,&out,&error),fail?TURBODB_STATUS_SQL_ERROR:TURBODB_STATUS_OK);
          if(fail) check_equal(out.data.int64_value,SENTINEL);
          else if(i!=3) check_equal(out.kind,TURBODB_VALUE_NULL); else {check_equal(out.kind,TURBODB_VALUE_DOUBLE);check_equal(out.data.double_value,1.5);}
          check_equal(numeric_diagnostics.total,i<2&&!fail?1u:0u);
        }
      }
    }
    it("refunds every mixed-expression compilation allocation and execution step failure") {
      const char *sql[]={"SELECT MOD((?+0.5)*?,?)/?=1.0","SELECT (? BETWEEN ? AND ?) AND ?=1.0",
        "SELECT (CASE ? WHEN 0 THEN ? WHEN 1 THEN ? ELSE ? END)=7.0","SELECT COALESCE(?,?,?)=7.0"};
      const turbodb_value_t values[][4]={{turbodb_i64(1),turbodb_f64(2.0),turbodb_i64(4),turbodb_i64(3)},
        {turbodb_i64(INT64_C(9007199254740992)),turbodb_i64(INT64_C(9007199254740993)),turbodb_f64(9007199254740994.0),turbodb_i64(1)},
        {turbodb_i64(0),turbodb_i64(7),turbodb_u64(UINT64_MAX),turbodb_f64(0.0)},
        {turbodb_i64(7),turbodb_u64(UINT64_MAX),turbodb_f64(0.0),turbodb_null()}};
      for(size_t mode=0;mode<sizeof(sql)/sizeof(sql[0]);++mode) {
      reset_budget();parse(sql[mode]);
      for(size_t i=0;i<binding_count;++i) bindings[i].type=(orm_sql_type){values[mode][i].kind,false};
      reserve_calls=resize_calls=0; check_equal(compile(),TURBODB_STATUS_OK); const size_t counts[]={reserve_calls,resize_calls};
      const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      for(size_t phase=0;phase<2;++phase) for(size_t point=1;point<=counts[phase];++point) {
        fail_reserve=fail_resize=0; reset_budget(); reserve_calls=resize_calls=0; if(phase) fail_resize=point; else fail_reserve=point;
        check_equal(compile(),TURBODB_STATUS_OUT_OF_MEMORY); check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
      }
      for(uint64_t point=0;point<steps;++point) {
        fail_reserve=fail_resize=0; reset_budget(); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=point;
        check_equal(compile(),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
      }
      fail_reserve=fail_resize=0; limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=TEST_STEPS;
      reset_budget();check_equal(compile(),TURBODB_STATUS_OK);
      const uint64_t before=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      expect_truth(run(values[mode],binding_count),1);
      const uint64_t eval_steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-before;
      for(uint64_t point=0;point<eval_steps;++point) {
        budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
        const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];turbodb_value_t out=turbodb_i64(SENTINEL);
        check_equal(orm_tidesdb_sql_expr_eval(&program,values[mode],binding_count,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
        check_equal(out.data.int64_value,SENTINEL);check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      }
    }
  }

  group("division and modulo expression execution") {
    it("evaluates mixed integers DOUBLE division nested MOD and signed remainder once") {
      const char *sql[]={"SELECT -7 DIV 3 = -2", "SELECT MOD(-7,3) = -1",
        "SELECT 18446744073709551615 DIV 2 = 9223372036854775807",
        "SELECT -7 % 18446744073709551615 = -7", "SELECT 7.5 / 2.0 = 3.75",
        "SELECT MOD(MOD(-7.5,2.0),2.0) = -1.5", "SELECT MOD(-9223372036854775808,-1)=0"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        reset_budget(); parse(sql[i]); check_equal(compile(),TURBODB_STATUS_OK); expect_truth(run(NULL,0),1);
      }
    }
    it("applies copied query strict and IGNORE policies using bounded diagnostics") {
      enum { RECORDS = 2, EVALUATIONS = 3 };
      check_equal(orm_sql_diagnostics_init(&numeric_diagnostics,RECORDS,&error),TURBODB_STATUS_OK);
      parse("SELECT MOD(5,0)"); check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
      const size_t slots[]={0};
      for(size_t mode=0;mode<=ORM_SQL_EVALUATION_IGNORE_WRITE;++mode) {
        check_equal(orm_sql_diagnostics_reset(&numeric_diagnostics,&error),TURBODB_STATUS_OK);
        orm_sql_expr_query_sources sources={.evaluation={&numeric_diagnostics,(orm_sql_evaluation_mode)mode}};
        orm_sql_expr_run workspace={0};
        check_equal(orm_tidesdb_sql_expr_run_open_mapped(&program,slots,0,&sources,&workspace,&error),TURBODB_STATUS_OK);
        sources.evaluation.mode=ORM_SQL_EVALUATION_WRITE;
        for(size_t i=0;i<EVALUATIONS;++i) {
          turbodb_value_t out=turbodb_i64(SENTINEL);
          check_equal(orm_tidesdb_sql_expr_run_eval(&workspace,NULL,0,&out,&error),mode==ORM_SQL_EVALUATION_WRITE?TURBODB_STATUS_SQL_ERROR:TURBODB_STATUS_OK);
          if(mode==ORM_SQL_EVALUATION_WRITE) check_equal(out.data.int64_value,SENTINEL); else check_equal(out.kind,TURBODB_VALUE_NULL);
        }
        check_equal(numeric_diagnostics.total,mode==ORM_SQL_EVALUATION_WRITE?0u:EVALUATIONS);
        check_equal(vec_size(&numeric_diagnostics.records),mode==ORM_SQL_EVALUATION_WRITE?0u:RECORDS);
        if(mode!=ORM_SQL_EVALUATION_WRITE) check_equal(orm_sql_diagnostics_at(&numeric_diagnostics,0)->code,ORM_SQL_DIAGNOSTIC_DIVISION_BY_ZERO);
        check_equal(orm_tidesdb_sql_expr_run_close(&workspace,&error),TURBODB_STATUS_OK);
      }
      numeric_diagnostics.total=UINT64_MAX;
      orm_sql_expr_run workspace={0};
      const orm_sql_evaluation evaluation={.diagnostics=&numeric_diagnostics};
      check_equal(orm_tidesdb_sql_expr_run_open_evaluation(&program,evaluation,&workspace,&error),TURBODB_STATUS_OK);
      turbodb_value_t out=turbodb_i64(SENTINEL);
      check_equal(orm_tidesdb_sql_expr_run_eval(&workspace,NULL,0,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(out.data.int64_value,SENTINEL); check_equal(numeric_diagnostics.total,UINT64_MAX);
      check_equal(vec_size(&numeric_diagnostics.records),RECORDS);
      for(size_t i=0;i<program.register_count;++i)
        check_equal(((const turbodb_value_t *)vec_at_const(&workspace.registers,i))->kind,TURBODB_VALUE_NULL);
      check_equal(orm_tidesdb_sql_expr_run_close(&workspace,&error),TURBODB_STATUS_OK);
    }
    it("does not warn for NULL or unselected branches after AST destruction") {
      enum { RECORDS = 2 };
      check_equal(orm_sql_diagnostics_init(&numeric_diagnostics,RECORDS,&error),TURBODB_STATUS_OK);
      const char *sql[]={"SELECT COALESCE(NULL DIV 0,7)","SELECT CASE WHEN TRUE THEN 7 ELSE MOD(5,0) END",
        "SELECT COALESCE(7,5 DIV 0)", "SELECT IFNULL(7,MOD(5,0))"};
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        reset_budget(); parse(sql[i]); check_equal(orm_tidesdb_sql_expr_compile_value(document,root,NULL,0,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
        sqlparser_document_destroy(document); document=NULL; turbodb_value_t out=turbodb_i64(SENTINEL);
        const orm_sql_evaluation evaluation={&numeric_diagnostics,ORM_SQL_EVALUATION_WRITE};
        check_equal(orm_tidesdb_sql_expr_eval_evaluation(&program,NULL,0,evaluation,&out,&error),TURBODB_STATUS_OK);
        check_equal(out.data.int64_value,7); check_equal(numeric_diagnostics.total,0u);
      }
    }
    it("treats MOD calls and percent expressions as equivalent compiled programs") {
      orm_sql_expr left={0}; const char *sql[]={"SELECT MOD(?,?)", "SELECT ? % ?"}; const size_t slots[]={0,1};
      parse(sql[0]); bindings[0].type=bindings[1].type=(orm_sql_type){TURBODB_VALUE_INT64,false};
      check_equal(orm_tidesdb_sql_expr_compile_value(document,root,bindings,binding_count,TEST_DEPTH,&budget,&left,&error),TURBODB_STATUS_OK);
      parse(sql[1]); bindings[0].type=bindings[1].type=(orm_sql_type){TURBODB_VALUE_INT64,false};
      check_equal(orm_tidesdb_sql_expr_compile_value(document,root,bindings,binding_count,TEST_DEPTH,&budget,&program,&error),TURBODB_STATUS_OK);
      bool same=false; check_equal(orm_tidesdb_sql_expr_same(&left,slots,&program,slots,&same,&error),TURBODB_STATUS_OK); check_true(same);
      check_equal(orm_tidesdb_sql_expr_destroy(&left,&error),TURBODB_STATUS_OK);
    }
    it("cleans every division compilation allocation and rejects invalid evaluation receivers") {
      parse("SELECT MOD(? DIV ?,?) = 1");
      for(size_t i=0;i<binding_count;++i) bindings[i].type=(orm_sql_type){TURBODB_VALUE_INT64,false};
      reserve_calls=resize_calls=0; check_equal(compile(),TURBODB_STATUS_OK); const size_t counts[]={reserve_calls,resize_calls};
      const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      for(size_t phase=0;phase<2;++phase) for(size_t point=1;point<=counts[phase];++point) {
        fail_reserve=fail_resize=0; reset_budget(); reserve_calls=resize_calls=0; if(phase) fail_resize=point; else fail_reserve=point;
        check_equal(compile(),TURBODB_STATUS_OUT_OF_MEMORY); check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
      }
      for(uint64_t point=0;point<steps;++point) {
        fail_reserve=fail_resize=0; reset_budget(); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=point;
        check_equal(compile(),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(program.budget);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
      }
      fail_reserve=fail_resize=0; limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=TEST_STEPS; reset_budget(); check_equal(compile(),TURBODB_STATUS_OK);
      orm_sql_expr_run workspace={0}; const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      const orm_sql_evaluation evaluation={.diagnostics=&numeric_diagnostics};
      check_equal(orm_tidesdb_sql_expr_run_open_evaluation(&program,evaluation,&workspace,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      check_null(workspace.program); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }

  it("charges arithmetic instruction steps exactly and reuses registers after overflow") {
    parse("SELECT ? + ? = ?");
    for (size_t i = 0; i < binding_count; ++i) bindings[i].type = (orm_sql_type){TURBODB_VALUE_INT64, false};
    enum { STEPS = 5 };
    limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = STEPS - 1; reset_budget();
    check_equal(compile(), TURBODB_STATUS_OK);
    turbodb_value_t inputs[] = {turbodb_i64(1), turbodb_i64(2), turbodb_i64(3)}, out = turbodb_i64(SENTINEL);
    check_equal(orm_tidesdb_sql_expr_eval(&program, inputs, binding_count, &out, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.data.int64_value, SENTINEL);
    limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = STEPS; reset_budget();
    check_equal(compile(), TURBODB_STATUS_OK); expect_truth(run(inputs, binding_count), 1);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], STEPS);
    limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = TEST_STEPS; reset_budget(); check_equal(compile(), TURBODB_STATUS_OK);
    orm_sql_expr_run workspace = {0}; check_equal(orm_tidesdb_sql_expr_run_open(&program, &workspace, &error), TURBODB_STATUS_OK);
    reserve_calls = resize_calls = 0; fail_reserve = fail_resize = 1;
    inputs[0] = turbodb_i64(INT64_MAX);
    check_equal(orm_tidesdb_sql_expr_run_eval(&workspace, inputs, binding_count, &out, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    for (size_t i = 0; i < program.register_count; ++i)
      check_equal(((const turbodb_value_t *)vec_at_const(&workspace.registers, i))->kind, TURBODB_VALUE_NULL);
    inputs[0] = turbodb_i64(1);
    check_equal(orm_tidesdb_sql_expr_run_eval(&workspace, inputs, binding_count, &out, &error), TURBODB_STATUS_OK);
    expect_truth(out, 1); check_equal(reserve_calls, 0u); check_equal(resize_calls, 0u);
    check_equal(orm_tidesdb_sql_expr_run_close(&workspace, &error), TURBODB_STATUS_OK);
  }

  it("cleans arithmetic compilation failures before retrying the same AST") {
    parse("SELECT COALESCE(NULL, -(2 + 3)) * 4 = -20");
    reserve_calls = resize_calls = 0; check_equal(compile(), TURBODB_STATUS_OK);
    const size_t reserves = reserve_calls, resizes = resize_calls;
    for (size_t i = 1; i <= reserves + resizes; ++i) {
      fail_reserve = fail_resize = 0; reset_budget(); reserve_calls = resize_calls = 0;
      if (i <= reserves) fail_reserve = i; else fail_resize = i - reserves;
      check_equal(compile(), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(program.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    fail_reserve = fail_resize = 0; reset_budget(); check_equal(compile(), TURBODB_STATUS_OK); expect_truth(run(NULL, 0), 1);
  }

  it("handles long literal spans and does not allocate a literal pool for scalar-only programs") {
    enum { LITERAL_LENGTH = 8192, PREFIX_LENGTH = sizeof("SELECT ? = '") - 1, SUFFIX_LENGTH = sizeof("'") };
    char sql[PREFIX_LENGTH + LITERAL_LENGTH + SUFFIX_LENGTH];
    memcpy(sql, "SELECT ? = '", PREFIX_LENGTH);
    memset(sql + PREFIX_LENGTH, 'x', LITERAL_LENGTH);
    memcpy(sql + PREFIX_LENGTH + LITERAL_LENGTH, "'", SUFFIX_LENGTH);
    parse(sql); bindings[0].type = (orm_sql_type){TURBODB_VALUE_TEXT, false}; check_equal(compile(), TURBODB_STATUS_OK);
    const turbodb_value_t input = turbodb_text_v((vstr){sql + PREFIX_LENGTH, LITERAL_LENGTH});
    expect_truth(run(&input, 1), 1);
    reset_budget(); parse("SELECT 1 = 1"); check_equal(compile(), TURBODB_STATUS_OK);
    check_equal(program.literal_work_bytes, 0u); check_equal(program.literals.initialized, false);
  }
}
