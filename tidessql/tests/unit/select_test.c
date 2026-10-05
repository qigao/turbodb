#include "select.h"
#include "explain.h"
#include <tinytest.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <cstl/sort.h>

static size_t reserve_calls, resize_calls, fail_reserve, fail_resize;
static size_t sort_calls, fail_sort_at;
static bool fail_sort;
static stl_status select_test_sort(void *base, size_t count, const cmeta_type_desc *type, size_t bytes) {
  return (++sort_calls == fail_sort_at || fail_sort) ? STL_OUT_OF_MEMORY : stable_sort(base, count, type, bytes);
}
static stl_status select_test_reserve(vec_t *v, size_t count) {
  return ++reserve_calls == fail_reserve ? STL_OUT_OF_MEMORY : vec_reserve(v, count);
}
static stl_status select_test_resize(vec_t *v, size_t count) {
  return ++resize_calls == fail_resize ? STL_OUT_OF_MEMORY : vec_resize(v, count);
}
#define vec_reserve select_test_reserve
#define vec_resize select_test_resize
#define stable_sort select_test_sort
#include "../../src/work.c"
#include "../../src/expr.c"
#include "../../src/rows.c"
#include "../../src/scan.c"
#include "../../src/select.c"
#undef vec_reserve
#undef vec_resize
#undef stable_sort

enum { TEST_ROWS = 5, TEST_COLUMNS = 3, TEST_LIMIT = 65536,
       TEST_WORK = 4 * 1024 * 1024, TEST_DEPTH = 32 };
static orm_tidesdb_sql_budget budget;
static orm_sql_budget_limits limits;
static orm_sql_select plan;
static orm_sql_select_run run;
static turbodb_error_t error;
static sqlparser_document *document;
static orm_sql_schema_column columns[TEST_COLUMNS];
static orm_sql_table_schema schema;
static turbodb_value_t rows[TEST_ROWS][TEST_COLUMNS];
static size_t cardinality_position, cardinality_count, cardinality_calls;
static orm_sql_type cardinality_types[TEST_COLUMNS];
static orm_sql_row_source cardinality_source;
static turbodb_status_t cardinality_pull(void *context, const turbodb_value_t **out, turbodb_error_t *e) {
  (void)context; (void)e; ++cardinality_calls;
  *out=cardinality_position<cardinality_count ? rows[cardinality_position++] : NULL;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t open_cardinality(size_t count,const turbodb_value_t *parameters,size_t parameter_count) {
  cardinality_position=cardinality_calls=0; cardinality_count=count;
  for(size_t i=0;i<TEST_COLUMNS;++i) cardinality_types[i]=columns[i].type;
  cardinality_source=(orm_sql_row_source){&budget,cardinality_types,TEST_COLUMNS,&cardinality_position,cardinality_pull,false};
  return orm_sql_select_open_cardinality(&plan,&cardinality_source,parameters,parameter_count,NULL,&run,&error);
}
typedef struct order_source { turbodb_value_t row[TEST_COLUMNS]; char name[2]; size_t calls, count, fail_at; } order_source;
static turbodb_status_t order_source_next(void *context, const turbodb_value_t **out, turbodb_error_t *e) {
  order_source *source = context;
  ++source->calls;
  if (source->calls == source->fail_at) { tdsql_error_set(e,TURBODB_STATUS_DATASTORE_ERROR,"test source failed"); return TURBODB_STATUS_DATASTORE_ERROR; }
  source->name[0]='!';
  if (source->calls > source->count) { *out=NULL; return TURBODB_STATUS_OK; }
  orm_sql_budget_amount amount={0}; amount.value[ORM_SQL_BUDGET_READ_ROWS]=1;
  amount.value[ORM_SQL_BUDGET_READ_BYTES]=sizeof(source->row)+1;
  turbodb_status_t status=orm_tidesdb_sql_budget_reserve(&budget,&amount,e);
  if (status != TURBODB_STATUS_OK) return status;
  source->name[0]=(char)('a'+source->calls-1); source->name[1]=0;
  source->row[0]=turbodb_i64((int64_t)source->calls);
  source->row[1]=turbodb_i64((int64_t)(source->count-source->calls));
  source->row[2]=turbodb_text(source->name); *out=source->row;
  return TURBODB_STATUS_OK;
}

static void parse_mode(const char *sql, bool no_backslash) {
  sqlparser_document_destroy(document); document = NULL;
  sqlparser_error parse_error;
  const sqlparser_options options = {SQLPARSER_MYSQL, no_backslash};
  check_equal(sqlparser_parse_with_options(sql, strlen(sql), &options, NULL, &document, &parse_error), SQLPARSER_OK);
}
static void parse(const char *sql) { parse_mode(sql, false); }
static turbodb_status_t bind_select(void) {
  const turbodb_status_t status=orm_tidesdb_sql_select_bind(document, &schema, TEST_DEPTH, &budget, &plan, &error);
  if(status!=TURBODB_STATUS_OK) info("SELECT binding: %s",error.message);
  return status;
}
static turbodb_status_t bind_with(const orm_sql_type *types, size_t count) {
  return orm_tidesdb_sql_select_bind_parameters(document, &schema, types, count, TEST_DEPTH, &budget, &plan, &error);
}
static turbodb_status_t open_with(const turbodb_value_t *values, size_t count) {
  return orm_tidesdb_sql_select_open_parameters(&plan, &rows[0][0], TEST_ROWS, values, count, &run, &error);
}
static void reset(void) {
  fail_sort = false;
  sort_calls = fail_sort_at = 0;
  fail_reserve = fail_resize = 0;
  check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_select_destroy(&plan, &error), TURBODB_STATUS_OK);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
  check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
}
static void open_rows(void) {
  check_equal(orm_tidesdb_sql_select_open(&plan, &rows[0][0], TEST_ROWS, &run, &error), TURBODB_STATUS_OK);
}
static orm_sql_scan_row next(void) {
  orm_sql_scan_row row = {0};
  check_equal(orm_tidesdb_sql_scan_next(&run.scan, &row, &error), TURBODB_STATUS_OK);
  return row;
}
static void reject(const char *sql, turbodb_status_t expected, const char *message) {
  reset(); parse(sql);
  const turbodb_status_t status = bind_select();
  if (status != expected) fprintf(stderr,"SQL rejection mismatch: %s: %s\n",sql,error.message);
  check_equal(status, expected);
  check_contains(error.message, message);
  check_null(plan.budget);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
  check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
}
static void double_scores(void) {
  columns[1].type.kind=TURBODB_VALUE_DOUBLE;
  for(size_t i=0;i<TEST_ROWS;++i)
    if(rows[i][1].kind==TURBODB_VALUE_INT64) rows[i][1]=turbodb_f64((double)rows[i][1].data.int64_value);
}

spec("TidesDB schema bound memory SELECT") {
  before_each() {
    fail_sort = false;
    sort_calls = fail_sort_at = 0;
    reserve_calls = resize_calls = fail_reserve = fail_resize = 0;
    tdsql_error_init(&error);
    limits = (orm_sql_budget_limits){0};
    for (size_t i = 0; i < ORM_SQL_BUDGET_RESOURCE_COUNT; ++i) limits.statement.value[i] = TEST_LIMIT;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = TEST_WORK;
    limits.transaction = (orm_sql_transaction_budget_amount){TEST_LIMIT, TEST_LIMIT, TEST_LIMIT};
    check_equal(orm_tidesdb_sql_budget_init(&budget, &limits, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
    plan = (orm_sql_select){0}; run = (orm_sql_select_run){0}; document = NULL;
    columns[0] = (orm_sql_schema_column){{"id", 2}, {TURBODB_VALUE_INT64, false}};
    columns[1] = (orm_sql_schema_column){{"score", 5}, {TURBODB_VALUE_INT64, true}};
    columns[2] = (orm_sql_schema_column){{"name", 4}, {TURBODB_VALUE_TEXT, false}};
    schema = (orm_sql_table_schema){{"items", 5}, columns, TEST_COLUMNS};
    const int scores[] = {0, 20, 10, 30, 40};
    const char *names[] = {"a", "b", "c", "d", "e"};
    for (size_t i = 0; i < TEST_ROWS; ++i) {
      rows[i][0] = turbodb_i64((int64_t)i + 1);
      rows[i][1] = i ? turbodb_i64(scores[i]) : turbodb_null();
      rows[i][2] = turbodb_text(names[i]);
    }
  }
  after_each() {
    fail_sort = false;
    fail_sort_at = 0;
    fail_reserve = fail_resize = 0;
    check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_destroy(&plan, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document = NULL;
  }

  group("numeric scalar binding") {
    it("keeps numeric call types nullable input and output ordering independent of the AST") {
      double_scores(); rows[1][1]=turbodb_f64(-1.25); rows[2][1]=turbodb_f64(1.25);
      parse("SELECT id,ABS(score) AS a,SIGN(score) AS s,FLOOR(score) AS f,CEILING(score) AS c FROM items WHERE id<=3 ORDER BY id");
      check_equal(bind_select(),TURBODB_STATUS_OK);
      sqlparser_document_destroy(document); document=NULL; open_rows();
      orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,1);
      for(size_t i=1;i<row.count;++i) check_equal(row.values[i].kind,TURBODB_VALUE_NULL);
      for(size_t i=0;i<2;++i) {
        row=next(); check_equal(row.values[1].kind,TURBODB_VALUE_DOUBLE); check_equal(row.values[1].data.double_value,1.25);
        check_equal(row.values[2].kind,TURBODB_VALUE_INT64); check_equal(row.values[2].data.int64_value,i?1:-1);
        check_equal(row.values[3].data.double_value,i?1.0:-2.0); check_equal(row.values[4].data.double_value,i?2.0:-1.0);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("recognizes CEIL and CEILING as equivalent grouped expressions") {
      double_scores(); rows[1][1]=turbodb_f64(1.25); rows[2][1]=turbodb_f64(1.75);
      parse("SELECT CEIL(score) AS k,COUNT(*) AS n FROM items GROUP BY CEILING(score) HAVING SIGN(CEIL(score))=1 ORDER BY ABS(CEILING(score))");
      const turbodb_status_t status=bind_select(); if(status!=TURBODB_STATUS_OK) info("numeric GROUP binding: %s",error.message);
      check_equal(status,TURBODB_STATUS_OK); open_rows();
      const orm_sql_scan_row row=next(); check_equal(row.values[0].kind,TURBODB_VALUE_DOUBLE);
      check_equal(row.values[0].data.double_value,2.0); check_equal(row.values[1].data.uint64_value,2u);
      check_equal(next().values[0].data.double_value,30.0); check_equal(next().values[0].data.double_value,40.0);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("uses nested arithmetic group values without exposing their ungrouped source columns") {
      parse("SELECT ABS(score+1) AS a,(score+1)*2 AS b FROM items GROUP BY score+1 HAVING score+1>10 ORDER BY (score+1)*2");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      const int64_t values[]={11,21,31,41};
      for(size_t i=0;i<sizeof(values)/sizeof(values[0]);++i) {
        const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,values[i]);
        check_equal(row.values[1].data.int64_value,values[i]*2);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
      reject("SELECT ABS(score) AS n FROM items GROUP BY score+1",TURBODB_STATUS_SQL_ERROR,"column");
    }
    it("substitutes nested numeric group keys in window values and window ordering") {
      double_scores(); rows[1][1]=turbodb_f64(-1.25); rows[2][1]=turbodb_f64(1.25);
      parse("SELECT ABS(CEIL(score)) AS k,LAG(ABS(CEIL(score)),1,0.0) OVER(ORDER BY ABS(CEILING(score))) AS p "
          "FROM items GROUP BY CEILING(score) HAVING SIGN(CEIL(score))>0 ORDER BY k");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      const double values[]={2.0,30.0,40.0};
      for(size_t i=0;i<sizeof(values)/sizeof(values[0]);++i) {
        const orm_sql_scan_row row=next(); check_equal(row.values[0].data.double_value,values[i]);
        check_equal(row.values[1].data.double_value,i?values[i-1]:0.0);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
  }
  group("ordinary DISTINCT aggregate binding") {
    it("keeps tuple count single count and ordinary count separate") {
      rows[2][1]=turbodb_i64(20); rows[4][1]=turbodb_null();
      parse("SELECT COUNT(DISTINCT score,id>0) AS tuples,COUNT(DISTINCT score) AS unique_scores,COUNT(score) AS present FROM items");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,2); check_equal(row.values[1].data.int64_value,2); check_equal(row.values[2].data.int64_value,3);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("canonicalizes ordered tuple arguments without merging distinct and plain functions") {
      rows[2][1]=turbodb_i64(20); rows[4][1]=turbodb_null();
      parse("SELECT DISTINCT COUNT(DISTINCT id,score) AS n FROM items ORDER BY COUNT(DISTINCT items.id,items.score)");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().values[0].data.int64_value,3);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
      reject("SELECT DISTINCT COUNT(DISTINCT id,score) AS n FROM items ORDER BY COUNT(DISTINCT score,id)",TURBODB_STATUS_SQL_ERROR,"DISTINCT");
      reject("SELECT DISTINCT COUNT(DISTINCT score) AS n FROM items ORDER BY COUNT(score)",TURBODB_STATUS_SQL_ERROR,"DISTINCT");
    }
    it("folds DOUBLE SUM AVG DISTINCT and normalizes MIN MAX DISTINCT") {
      double_scores(); rows[2][1]=rows[1][1]; rows[4][1]=turbodb_null();
      parse("SELECT SUM(DISTINCT score) AS s,AVG(DISTINCT score) AS a,MIN(DISTINCT score) AS low,MAX(DISTINCT score) AS high,COUNT(score) AS n FROM items");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.double_value,50.0); check_equal(row.values[1].data.double_value,25.0);
      check_equal(row.values[2].data.double_value,20.0); check_equal(row.values[3].data.double_value,30.0); check_equal(row.values[4].data.int64_value,3);
      reset(); parse("SELECT DISTINCT MIN(DISTINCT score) AS n FROM items ORDER BY MIN(score)");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().values[0].data.double_value,20.0);
    }
    it("composes grouped DISTINCT HAVING with windows and final ordering") {
      rows[2][1]=turbodb_i64(20); rows[4][1]=turbodb_null();
      parse("SELECT score AS k,COUNT(DISTINCT id,id>0) AS n,MAX(COUNT(DISTINCT id,id>0)) OVER() AS largest "
            "FROM items GROUP BY score HAVING n>1 ORDER BY k");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      for(size_t i=0;i<2;++i) {
        const orm_sql_scan_row row=next(); check_equal(row.values[1].data.int64_value,2); check_equal(row.values[2].data.int64_value,2);
        if(!i) check_equal(row.values[0].kind,TURBODB_VALUE_NULL); else check_equal(row.values[0].data.int64_value,20);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("owns flattened tuple marker programs after AST release and caller mutation") {
      parse("SELECT COUNT(DISTINCT id+?,score+?) AS n FROM items WHERE id>? HAVING n>? ORDER BY COUNT(DISTINCT id+?,score+?)");
      orm_sql_type types[6]; turbodb_value_t parameters[6];
      for(size_t i=0;i<6;++i) { types[i]=(orm_sql_type){TURBODB_VALUE_INT64,false}; parameters[i]=turbodb_i64(0); }
      check_equal(bind_with(types,6),TURBODB_STATUS_OK); check_equal(open_with(parameters,6),TURBODB_STATUS_OK);
      sqlparser_document_destroy(document); document=NULL;
      for(size_t i=0;i<6;++i) parameters[i]=turbodb_i64(INT64_MAX);
      check_equal(next().values[0].data.int64_value,4); check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("prunes every unused tuple argument but retains tuples used in HAVING") {
      parse("SELECT COUNT(DISTINCT id+9223372036854775807,score+9223372036854775807) AS unused,COUNT(DISTINCT score) AS n "
            "FROM items HAVING n>0");
      check_equal(bind_select(),TURBODB_STATUS_OK); check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_OK);
      check_equal(next().state,ORM_SQL_SCAN_ROW); check_equal(next().state,ORM_SQL_SCAN_DONE);
      reset(); parse("SELECT COUNT(DISTINCT id,score+9223372036854775807) AS n FROM items HAVING n>0");
      check_equal(bind_select(),TURBODB_STATUS_OK); check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_OK);
      orm_sql_scan_row row={.count=99}; check_equal(orm_tidesdb_sql_scan_next(&run.scan,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u);
    }
    it("returns empty global identities and avoids all reads under LIMIT zero") {
      double_scores(); const char *sql[]={
        "SELECT COUNT(DISTINCT score,id) AS n,SUM(DISTINCT score) AS s,AVG(DISTINCT score) AS a FROM items WHERE FALSE",
        "SELECT COUNT(DISTINCT score,id) AS n,SUM(DISTINCT score) AS s,AVG(DISTINCT score) AS a FROM items LIMIT 0"};
      for(size_t i=0;i<2;++i) {
        reset(); parse(sql[i]); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); const orm_sql_scan_row row=next();
        if(!i) { check_equal(row.values[0].data.int64_value,0); check_equal(row.values[1].kind,TURBODB_VALUE_NULL); check_equal(row.values[2].kind,TURBODB_VALUE_NULL); }
        else { check_equal(row.state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u); }
      }
    }
    it("rejects unsupported DISTINCT domains arity and windows before reading") {
      reject("SELECT COUNT(DISTINCT id,name) AS n FROM items LIMIT 0",TURBODB_STATUS_UNSUPPORTED,"numeric");
      reject("SELECT COUNT(id,score) AS n FROM items",TURBODB_STATUS_SQL_ERROR,"argument");
      reject("SELECT SUM(DISTINCT score) AS n FROM items",TURBODB_STATUS_UNSUPPORTED,"DECIMAL");
      reject("SELECT COUNT(DISTINCT id,score) OVER() AS n FROM items",TURBODB_STATUS_UNSUPPORTED,"DISTINCT");
      reject("SELECT MIN(DISTINCT id) OVER() AS n FROM items",TURBODB_STATUS_UNSUPPORTED,"DISTINCT");
      sqlparser_document *invalid=NULL; sqlparser_error parse_error;
      const char invalid_sql[]="SELECT COUNT(DISTINCT *) AS n FROM items";
      check_equal(sqlparser_parse(invalid_sql,strlen(invalid_sql),NULL,&invalid,&parse_error),SQLPARSER_SYNTAX_ERROR);
      check_null(invalid);
    }
    it("refunds every tuple binding execution allocation and execution-step failure") {
      double_scores();
      parse("SELECT COUNT(DISTINCT score,id>0) AS n,SUM(DISTINCT score) AS s,AVG(DISTINCT score) AS a FROM items GROUP BY id>0 HAVING n>0 ORDER BY s");
      reserve_calls=resize_calls=0; const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      while(next().state==ORM_SQL_SCAN_ROW) {}
      const uint64_t counts[]={reserve_calls,resize_calls,budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start};
      for(size_t phase=0;phase<3;++phase) {
        for(uint64_t point=phase==2?0:1;phase==2?point<counts[phase]:point<=counts[phase];++point) {
          reset(); reserve_calls=resize_calls=0;
          if(!phase) fail_reserve=(size_t)point;
          else if(phase==1) fail_resize=(size_t)point;
          else budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=point;
          turbodb_status_t status=bind_select();
          if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_select_open(&plan,&rows[0][0],TEST_ROWS,&run,&error);
          orm_sql_scan_row row={0};
          while(status==TURBODB_STATUS_OK) {
            status=orm_tidesdb_sql_scan_next(&run.scan,&row,&error);
            if(status==TURBODB_STATUS_OK && row.state!=ORM_SQL_SCAN_ROW) break;
          }
          check_equal(status,phase==2?TURBODB_STATUS_LIMIT_EXCEEDED:TURBODB_STATUS_OUT_OF_MEMORY);
        }
      }
    }
    it("locks each group DISTINCT and final sort failure and work exhaustion without a prefix") {
      double_scores();
      parse("SELECT COUNT(DISTINCT score,id>0) AS n,SUM(DISTINCT score) AS s FROM items GROUP BY id>0 ORDER BY s");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); sort_calls=0; check_equal(next().state,ORM_SQL_SCAN_ROW);
      const size_t sorts=sort_calls; check_greater(sorts,2u);
      for(size_t point=1;point<=sorts+1;++point) {
        reset(); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); sort_calls=0;
        if(point<=sorts) fail_sort_at=point;
        else budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
        orm_sql_scan_row row={.count=99};
        const turbodb_status_t expected=point<=sorts?TURBODB_STATUS_OUT_OF_MEMORY:TURBODB_STATUS_LIMIT_EXCEEDED;
        check_equal(orm_tidesdb_sql_scan_next(&run.scan,&row,&error),expected); check_equal(row.count,99u);
        const size_t calls=sort_calls;
        check_equal(orm_tidesdb_sql_scan_next(&run.scan,&row,&error),expected); check_equal(sort_calls,calls);
      }
    }
  }
  it("observes SELECT row count without projection or ordering evaluation") {
    parse("SELECT id+9223372036854775807 AS n FROM items WHERE id>1 ORDER BY n LIMIT 2 OFFSET 1");
    check_equal(bind_select(),TURBODB_STATUS_OK); check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_OK);
    check_equal(cardinality_calls,0u); check_equal(next().state,ORM_SQL_SCAN_ROW); check_equal(next().state,ORM_SQL_SCAN_ROW);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(cardinality_calls,4u);
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK); check_false(cardinality_source.active);
    open_rows(); orm_sql_scan_row row={.count=99};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u);
  }
  it("keeps empty global aggregation while pruning unused aggregate arguments") {
    parse("SELECT COUNT(id+9223372036854775807) AS n FROM items"); check_equal(bind_select(),TURBODB_STATUS_OK);
    for(size_t pass=0;pass<2;++pass) {
      check_equal(open_cardinality(pass?TEST_ROWS:0,NULL,0),TURBODB_STATUS_OK);
      check_equal(next().state,ORM_SQL_SCAN_ROW); check_equal(next().state,ORM_SQL_SCAN_DONE);
      check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK);
    }
    open_rows(); orm_sql_scan_row row={0};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
  }
  it("preserves HAVING live aggregate slots while pruning independent overflowing arguments") {
    parse("SELECT MAX(id+9223372036854775807) AS unused,COUNT(*) AS n FROM items HAVING n>4 ORDER BY unused");
    check_equal(bind_select(),TURBODB_STATUS_OK); check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_ROW); check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); parse("SELECT COUNT(id+9223372036854775807) AS n FROM items HAVING n>0");
    check_equal(bind_select(),TURBODB_STATUS_OK); check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_OK);
    orm_sql_scan_row row={.count=99}; check_equal(orm_tidesdb_sql_scan_next(&run.scan,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(row.count,99u); const size_t calls=cardinality_calls;
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(cardinality_calls,calls);
  }
  it("preserves grouped keys HAVING and OFFSET in cardinality mode") {
    parse("SELECT score,MAX(id+9223372036854775807) AS unused FROM items GROUP BY score HAVING score>10 LIMIT 1 OFFSET 2");
    check_equal(bind_select(),TURBODB_STATUS_OK); check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_ROW); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK);
    check_equal(open_cardinality(0,NULL,0),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("retains DISTINCT tuple computation where duplicates affect OFFSET") {
    parse("SELECT DISTINCT 1 AS n FROM items LIMIT 1 OFFSET 1"); check_equal(bind_select(),TURBODB_STATUS_OK);
    check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); parse("SELECT DISTINCT id+9223372036854775807 AS n FROM items"); check_equal(bind_select(),TURBODB_STATUS_OK);
    check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_OK); orm_sql_scan_row row={0};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
  }
  it("keeps cardinality parameter ownership LIMIT zero and cancellation lazy") {
    parse("SELECT COUNT(id+9223372036854775807) AS n FROM items HAVING COUNT(*)>? LIMIT ?");
    const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false}};
    check_equal(bind_with(types,2),TURBODB_STATUS_OK); turbodb_value_t parameters[]={turbodb_i64(4),turbodb_i64(1)};
    check_equal(open_cardinality(TEST_ROWS,parameters,2),TURBODB_STATUS_OK); parameters[0]=turbodb_i64(99);
    check_equal(next().state,ORM_SQL_SCAN_ROW); check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK);
    parameters[1]=turbodb_i64(0); check_equal(open_cardinality(TEST_ROWS,parameters,2),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(cardinality_calls,0u);
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK); parameters[1]=turbodb_i64(1);
    check_equal(open_cardinality(TEST_ROWS,parameters,2),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_scan_cancel(&run.scan,&error),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_CANCELLED);
    check_equal(cardinality_calls,0u);
  }
  it("refunds every cardinality opening step boundary without reading input") {
    parse("SELECT MAX(id+9223372036854775807) AS unused,COUNT(*) AS n FROM items HAVING n>0");
    check_equal(bind_select(),TURBODB_STATUS_OK); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_OK);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK);
    for(uint64_t point=0;point<steps;++point) {
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_null(run.program); check_false(cardinality_source.active); check_equal(cardinality_calls,0u);
      check_equal(plan.active_runs,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=TEST_LIMIT;
  }
  it("refunds each derived cardinality workspace allocation failure and keeps the plan reusable") {
    parse("SELECT MAX(id+9223372036854775807) AS unused,COUNT(*) AS n FROM items HAVING n>0");
    check_equal(bind_select(),TURBODB_STATUS_OK); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    reserve_calls=resize_calls=0; check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_OK);
    const size_t allocations[]={reserve_calls,resize_calls}; check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK);
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
      reserve_calls=resize_calls=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_OUT_OF_MEMORY); fail_resize=fail_reserve=0;
      check_null(run.program); check_false(cardinality_source.active); check_equal(plan.active_runs,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_ROW);
  }
  it("owns EXPLAIN text independently of its plan document and table label") {
    parse("SELECT id FROM items WHERE id>0"); check_equal(bind_select(),TURBODB_STATUS_OK);
    orm_sql_explain_source explanation={0}; char table[]="items";
    check_equal(orm_tidesdb_sql_explain_open(&plan,vstr_from_cstr(table),NULL,0,&explanation,&error),TURBODB_STATUS_OK);
    table[0]='x'; sqlparser_document_destroy(document); document=NULL;
    check_equal(orm_tidesdb_sql_select_destroy(&plan,&error),TURBODB_STATUS_OK);
    enum { TABLE_COLUMN=2, EXTRA_COLUMN=11 };
    const size_t projection[]={TABLE_COLUMN,EXTRA_COLUMN};
    const orm_sql_scan_spec spec={.projection=projection,.projection_count=2,.limit=1}; orm_sql_scan scan={0};
    check_equal(orm_tidesdb_sql_scan_open_source(&explanation.source,&spec,&budget,&scan,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_explain_close(&explanation,&error),TURBODB_STATUS_BUSY);
    orm_sql_scan_row out={0}; check_equal(orm_tidesdb_sql_scan_next(&scan,&out,&error),TURBODB_STATUS_OK);
    check_equal(out.values[0].data.text_value.len,5u); check_equal(memcmp(out.values[0].data.text_value.data,"items",5),0);
    check_equal(out.values[1].data.text_value.len,strlen("Using where"));
    check_equal(memcmp(out.values[1].data.text_value.data,"Using where",strlen("Using where")),0);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
    check_equal(orm_tidesdb_sql_scan_close(&scan,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_explain_close(&explanation,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_explain_close(&explanation,&error),TURBODB_STATUS_OK);
  }
  it("refunds EXPLAIN metadata when formatting work or step admission fails") {
    parse("SELECT id FROM items"); check_equal(bind_select(),TURBODB_STATUS_OK);
    const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; orm_sql_explain_source explanation={0};
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=retained;
    check_equal(orm_tidesdb_sql_explain_open(&plan,vstr_from_cstr("items"),NULL,0,&explanation,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(explanation.source.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=TEST_WORK;
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_explain_open(&plan,vstr_from_cstr("items"),NULL,0,&explanation,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(explanation.source.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
  }

  it("groups equivalent arithmetic expressions through source-qualified column bindings") {
    rows[2][1]=turbodb_i64(20); rows[4][1]=turbodb_null();
    parse("SELECT score+1 AS k,COUNT(*) AS n FROM items GROUP BY items.score + 01 ORDER BY score+1");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    orm_sql_scan_row out=next(); check_equal(out.values[0].kind,TURBODB_VALUE_NULL); check_equal(out.values[1].data.int64_value,2);
    out=next(); check_equal(out.values[0].data.int64_value,21); check_equal(out.values[1].data.int64_value,2);
    out=next(); check_equal(out.values[0].data.int64_value,31); check_equal(out.values[1].data.int64_value,1);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],TEST_ROWS);
  }
  it("resolves expression aliases and positions and permits HAVING to read the grouped output alias") {
    const char *sql[]={
      "SELECT CASE WHEN id>2 THEN 1 ELSE 0 END AS k,COUNT(*) AS n FROM items GROUP BY k HAVING k>0 ORDER BY n",
      "SELECT CASE WHEN id>2 THEN 1 ELSE 0 END AS k,COUNT(*) AS n FROM items GROUP BY 1 HAVING k>0 ORDER BY n",
      "SELECT CASE WHEN id>2 THEN 1 ELSE 0 END AS k,COUNT(*) AS n FROM items GROUP BY CASE WHEN items.id>2 THEN 1 ELSE 0 END HAVING k>0 ORDER BY n"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      reset(); parse(sql[i]); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      const orm_sql_scan_row out=next(); check_equal(out.values[0].data.int64_value,1); check_equal(out.values[1].data.int64_value,3);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    reset(); parse("SELECT score AS k FROM items GROUP BY k ORDER BY k");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().values[0].kind,TURBODB_VALUE_NULL);
    reset(); parse("SELECT score FROM items GROUP BY 1 ORDER BY 1");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().values[0].kind,TURBODB_VALUE_NULL);
  }
  it("groups scalar functions predicates and mixed keys while comparing complete expressions") {
    const char *sql[]={
      "SELECT COALESCE(score,20) AS k,COUNT(*) AS n FROM items GROUP BY k ORDER BY k",
      "SELECT id IN (1,2) AS k,COUNT(*) AS n FROM items GROUP BY id IN (1,2) ORDER BY k",
      "SELECT id BETWEEN 1 AND 2 AS k,COUNT(*) AS n FROM items GROUP BY k ORDER BY k",
      "SELECT NOT(id>2) AS k,COUNT(*) AS n FROM items GROUP BY k ORDER BY k"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      reset(); parse(sql[i]); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      const orm_sql_scan_row out=next();
      check_equal(out.values[0].kind,i?TURBODB_VALUE_BOOLEAN:TURBODB_VALUE_INT64);
      check_equal(out.values[1].data.int64_value,i?3:1);
    }
    reset(); parse("SELECT id>2 AS high,score,COUNT(*) AS n FROM items GROUP BY high,2 ORDER BY score");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[2].data.int64_value,1);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("keeps GROUP BY source names ahead of aliases and rejects ungrouped dependencies") {
    reject("SELECT id>2 AS score,COUNT(*) AS n FROM items GROUP BY score",TURBODB_STATUS_SQL_ERROR,"column");
    reject("SELECT score+1 AS k,score FROM items GROUP BY k",TURBODB_STATUS_SQL_ERROR,"column");
    reject("SELECT score+1 AS k,k FROM items GROUP BY k",TURBODB_STATUS_SQL_ERROR,"column");
    reject("SELECT score+2 AS k FROM items GROUP BY score+1",TURBODB_STATUS_SQL_ERROR,"column");
    reject("SELECT (score+2)*2 AS k FROM items GROUP BY score+1",TURBODB_STATUS_SQL_ERROR,"column");
    reject("SELECT score+1 AS k FROM items GROUP BY k HAVING score+2>0",TURBODB_STATUS_SQL_ERROR,"column");
    reset(); parse("SELECT score+1 AS score,COUNT(*) AS n FROM items GROUP BY score HAVING score>10 ORDER BY score");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().values[0].data.int64_value,21);
  }
  it("rejects invalid group positions aggregate keys and unsupported key types") {
    reject("SELECT score FROM items GROUP BY 0",TURBODB_STATUS_SQL_ERROR,"position");
    reject("SELECT score FROM items GROUP BY 2",TURBODB_STATUS_SQL_ERROR,"position");
    reject("SELECT COUNT(*) AS n FROM items GROUP BY n",TURBODB_STATUS_SQL_ERROR,"GROUP BY");
    reject("SELECT COUNT(*) AS n FROM items GROUP BY 1",TURBODB_STATUS_SQL_ERROR,"GROUP BY");
    reject("SELECT id FROM items GROUP BY COUNT(id)",TURBODB_STATUS_SQL_ERROR,"GROUP BY");
    reject("SELECT score AS k FROM items GROUP BY items.k",TURBODB_STATUS_SQL_ERROR,"column");
    reject("SELECT COALESCE(name,'x') AS k FROM items GROUP BY k",TURBODB_STATUS_UNSUPPORTED,"GROUP BY");
    reject("SELECT id FROM items GROUP BY missing",TURBODB_STATUS_SQL_ERROR,"alias");
    reject("SELECT id FROM items GROUP BY LENGTH(name)",TURBODB_STATUS_UNSUPPORTED,"function");
  }
  it("preserves parameter identity for grouped expressions and snapshots shared alias arguments") {
    parse("SELECT CASE WHEN id>? THEN 1 ELSE 0 END AS k,COUNT(*) AS n FROM items WHERE id>? GROUP BY k HAVING n>? ORDER BY k LIMIT ?");
    const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false}};
    turbodb_value_t parameters[]={turbodb_i64(2),turbodb_i64(0),turbodb_i64(1),turbodb_i64(2)};
    check_equal(bind_with(types,4),TURBODB_STATUS_OK); check_equal(open_with(parameters,4),TURBODB_STATUS_OK);
    parameters[0]=turbodb_i64(99); sqlparser_document_destroy(document); document=NULL;
    check_equal(next().values[1].data.int64_value,2); check_equal(next().values[1].data.int64_value,3);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); parse("SELECT id+? AS k FROM items GROUP BY id+?");
    check_equal(bind_with(types,2),TURBODB_STATUS_SQL_ERROR); check_null(plan.budget);
    reset(); parse("SELECT ? AS k,COUNT(*) AS n FROM items GROUP BY k");
    check_equal(bind_with(types,1),TURBODB_STATUS_OK); check_equal(open_with(parameters,1),TURBODB_STATUS_OK);
    const orm_sql_scan_row out=next(); check_equal(out.values[0].data.int64_value,99); check_equal(out.values[1].data.int64_value,5);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("retains nullable constant groups and applies DISTINCT to repeated group counts") {
    parse("SELECT NULL AS k,COUNT(*) AS n FROM items GROUP BY k");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); const orm_sql_scan_row out=next();
    check_equal(out.values[0].kind,TURBODB_VALUE_NULL); check_equal(out.values[1].data.int64_value,5);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); parse("SELECT NULL AS k,COUNT(*) AS n FROM items GROUP BY k"); check_equal(bind_select(),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_open(&plan,NULL,0,&run,&error),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); parse("SELECT DISTINCT id>2 AS k FROM items GROUP BY id>2 ORDER BY items.id>2 DESC");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_true(next().values[0].data.boolean_value);
    check_false(next().values[0].data.boolean_value); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("evaluates grouping keys after WHERE and locks key overflow before publishing a group") {
    rows[1][0]=turbodb_i64(INT64_MAX);
    parse("SELECT id+1 AS k FROM items GROUP BY k LIMIT 0"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
    reset(); parse("SELECT id+1 AS k FROM items WHERE id<10 GROUP BY k ORDER BY k");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().values[0].data.int64_value,2);
    reset(); parse("SELECT id+1 AS k FROM items GROUP BY k"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.state,ORM_SQL_SCAN_CANCELLED); const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
  }
  it("refunds every expression-key binding allocation including temporary comparison programs") {
    parse("SELECT COALESCE(score,0) AS k,COUNT(*) AS n FROM items GROUP BY k HAVING k>=0 ORDER BY COALESCE(items.score,0)");
    reserve_calls=resize_calls=0; check_equal(bind_select(),TURBODB_STATUS_OK);
    const size_t reserves=reserve_calls,resizes=resize_calls;
    for(size_t i=1;i<=reserves+resizes;++i) {
      reset(); reserve_calls=resize_calls=0;
      if(i<=reserves) fail_reserve=i; else fail_resize=i-reserves;
      check_equal(bind_select(),TURBODB_STATUS_OUT_OF_MEMORY); check_null(plan.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    }
  }

  it("reduces global counts extrema and scalar aggregate expressions with typed metadata") {
    parse("SELECT COUNT(*) AS n,COUNT(score) AS present,MIN(score) AS lo,MAX(score) AS hi,COUNT(name)+1 AS adjusted FROM items");
    check_equal(bind_select(),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_column_at(&plan,0)->type.kind,TURBODB_VALUE_INT64);
    check_false(orm_tidesdb_sql_select_column_at(&plan,0)->type.nullable);
    check_true(orm_tidesdb_sql_select_column_at(&plan,2)->type.nullable);
    open_rows(); const orm_sql_scan_row out=next();
    const int expected[]={5,4,10,40,6};
    for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) check_equal(out.values[i].data.int64_value,expected[i]);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],TEST_ROWS);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_GROUPS],1u);
  }
  it("binds DOUBLE SUM AVG and arithmetic aggregate arguments with nullable output metadata") {
    double_scores(); parse("SELECT SUM(score) AS total,AVG(score) AS mean,SUM(score+score) AS twice,COUNT(score) AS n FROM items");
    check_equal(bind_select(),TURBODB_STATUS_OK);
    for(size_t i=0;i<3;++i) {
      check_equal(orm_tidesdb_sql_select_column_at(&plan,i)->type.kind,TURBODB_VALUE_DOUBLE);
      check_true(orm_tidesdb_sql_select_column_at(&plan,i)->type.nullable);
    }
    open_rows(); const orm_sql_scan_row out=next();
    check_equal(out.values[0].data.double_value,100.0); check_equal(out.values[1].data.double_value,25.0);
    check_equal(out.values[2].data.double_value,200.0); check_equal(out.values[3].data.int64_value,4);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],TEST_ROWS);
  }
  it("uses average aliases in HAVING and hidden sums for ordering expression groups") {
    double_scores(); parse("SELECT id>2 AS k,AVG(score) AS mean FROM items GROUP BY k "
      "HAVING mean IS NOT NULL ORDER BY SUM(score) DESC LIMIT 1 OFFSET 1");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); const orm_sql_scan_row out=next();
    check_equal(out.values[0].data.boolean_value,0); check_equal(out.values[1].data.double_value,20.0);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); rows[3][1]=rows[4][1]=turbodb_f64(20.0);
    parse("SELECT DISTINCT AVG(score) AS mean FROM items GROUP BY id ORDER BY AVG(items.score) DESC");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    check_equal(next().values[0].data.double_value,20.0); check_equal(next().values[0].data.double_value,10.0);
    check_equal(next().values[0].kind,TURBODB_VALUE_NULL); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("owns DOUBLE aggregate parameters and distinguishes SUM from AVG of identical arguments") {
    double_scores(); parse("SELECT SUM(score+?) AS total,AVG(score+?) AS mean FROM items HAVING mean>? ORDER BY total");
    const orm_sql_type types[]={{TURBODB_VALUE_DOUBLE,false},{TURBODB_VALUE_DOUBLE,false},{TURBODB_VALUE_DOUBLE,false}};
    turbodb_value_t values[]={turbodb_f64(2.0),turbodb_f64(2.0),turbodb_f64(26.0)};
    check_equal(bind_with(types,3),TURBODB_STATUS_OK); check_equal(open_with(values,3),TURBODB_STATUS_OK);
    values[0]=turbodb_f64(999.0); values[1]=turbodb_f64(999.0); sqlparser_document_destroy(document); document=NULL;
    const orm_sql_scan_row out=next(); check_equal(out.state,ORM_SQL_SCAN_ROW);
    check_equal(out.values[0].data.double_value,108.0); check_equal(out.values[1].data.double_value,27.0);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("returns NULL for filtered and all NULL aggregates and avoids reads under LIMIT zero") {
    double_scores();
    const char *sql[]={"SELECT SUM(score) AS s,AVG(score) AS a FROM items WHERE FALSE",
      "SELECT SUM(score) AS s,AVG(score) AS a FROM items WHERE score IS NULL",
      "SELECT SUM(NULL) AS s,AVG(NULL) AS a FROM items"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      reset(); parse(sql[i]); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      const orm_sql_scan_row out=next(); check_equal(out.state,ORM_SQL_SCAN_ROW);
      check_equal(out.values[0].kind,TURBODB_VALUE_NULL); check_equal(out.values[1].kind,TURBODB_VALUE_NULL);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    reset(); parse("SELECT SUM(score) AS s,AVG(score) AS a FROM items LIMIT 0");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
  }
  it("rejects precise input unsupported conversion and invalid aggregate placement before reads") {
    reject("SELECT SUM(id) AS s FROM items LIMIT 0",TURBODB_STATUS_UNSUPPORTED,"DECIMAL");
    reject("SELECT AVG(id) AS a FROM items WHERE FALSE",TURBODB_STATUS_UNSUPPORTED,"DECIMAL");
    reject("SELECT AVG(id>0) AS a FROM items",TURBODB_STATUS_UNSUPPORTED,"DECIMAL");
    reject("SELECT SUM(name) AS s FROM items",TURBODB_STATUS_UNSUPPORTED,"DOUBLE");
    double_scores();
    reject("SELECT SUM(DISTINCT score) OVER() AS s FROM items",TURBODB_STATUS_UNSUPPORTED,"DISTINCT");
    reject("SELECT AVG(score,score) AS a FROM items",TURBODB_STATUS_SQL_ERROR,"argument");
    reject("SELECT AVG(SUM(score)) AS a FROM items",TURBODB_STATUS_UNSUPPORTED,"function");
    reject("SELECT id FROM items WHERE SUM(score) IS NULL",TURBODB_STATUS_SQL_ERROR,"WHERE");
    reject("SELECT id FROM items GROUP BY AVG(score)",TURBODB_STATUS_SQL_ERROR,"aggregate");
  }
  it("locks overflow for global and grouped averages before publishing a partial row") {
    double_scores(); rows[1][1]=rows[2][1]=turbodb_f64(DBL_MAX);
    const char *sql[]={"SELECT AVG(score) AS a FROM items",
      "SELECT SUM(score) AS s FROM items GROUP BY id>0 ORDER BY s"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      reset(); parse(sql[i]); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); orm_sql_scan_row out={.count=99};
      check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(out.count,99u); const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
      check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    }
  }
  it("emits an empty global aggregate but no empty keyed group and applies HAVING afterward") {
    const char *sql[]={
      "SELECT COUNT(*) AS n,MIN(score) AS lo,MAX(score) AS hi FROM items WHERE FALSE",
      "SELECT COUNT(*) AS n,MIN(score) AS lo,MAX(score) AS hi FROM items"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      reset(); parse(sql[i]); check_equal(bind_select(),TURBODB_STATUS_OK);
      if(i) check_equal(orm_tidesdb_sql_select_open(&plan,NULL,0,&run,&error),TURBODB_STATUS_OK); else open_rows();
      const orm_sql_scan_row out=next(); check_equal(out.values[0].data.int64_value,0);
      check_equal(out.values[1].kind,TURBODB_VALUE_NULL); check_equal(out.values[2].kind,TURBODB_VALUE_NULL);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    reset(); parse("SELECT COUNT(*) AS n FROM items WHERE FALSE HAVING n>0");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); parse("SELECT score,COUNT(*) AS n FROM items WHERE FALSE GROUP BY score");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("groups NULL and repeated column keys before HAVING ordering and pagination") {
    rows[2][1]=turbodb_i64(20); rows[4][1]=turbodb_null();
    parse("SELECT score AS k,COUNT(*) AS n,COUNT(score) AS present,MAX(id) AS hi FROM items "
          "GROUP BY items.score HAVING n>1 ORDER BY hi DESC LIMIT 1 OFFSET 1");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); const orm_sql_scan_row out=next();
    check_equal(out.values[0].data.int64_value,20); check_equal(out.values[1].data.int64_value,2);
    check_equal(out.values[2].data.int64_value,2); check_equal(out.values[3].data.int64_value,3);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],TEST_ROWS);
    check_equal(budget.used.value[ORM_SQL_BUDGET_GROUPS],3u);
    reset(); parse("SELECT score,COUNT(*) AS n,COUNT(score) AS present FROM items GROUP BY score ORDER BY score");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); const orm_sql_scan_row null_group=next();
    check_equal(null_group.values[0].kind,TURBODB_VALUE_NULL); check_equal(null_group.values[1].data.int64_value,2);
    check_equal(null_group.values[2].data.int64_value,0);
  }
  it("allows key only multicolumn grouping and aggregates hidden from the projection") {
    rows[2][1]=turbodb_i64(20); rows[4][1]=turbodb_null();
    parse("SELECT score FROM items GROUP BY score HAVING COUNT(*)>1 ORDER BY MAX(id) DESC");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().values[0].kind,TURBODB_VALUE_NULL);
    check_equal(next().values[0].data.int64_value,20); check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); parse("SELECT score,id FROM items GROUP BY score,id ORDER BY id");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[1].data.int64_value,i+1);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); parse("SELECT score AS k FROM items GROUP BY score HAVING k IS NULL");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().values[0].kind,TURBODB_VALUE_NULL);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("canonicalizes repeated aggregates for DISTINCT and keeps arguments in source scope") {
    rows[2][1]=turbodb_i64(20); rows[4][1]=turbodb_null();
    parse("SELECT DISTINCT COUNT(id) AS id FROM items GROUP BY score ORDER BY COUNT(items.id) DESC");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().values[0].data.int64_value,2);
    check_equal(next().values[0].data.int64_value,1); check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); parse("SELECT COUNT(id) AS id FROM items HAVING COUNT(id)=5 ORDER BY COUNT(id)");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().values[0].data.int64_value,5);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("does not confuse scalar function names with computed HAVING aliases") {
    parse("SELECT MAX(id)+1 AS COALESCE FROM items HAVING COALESCE(MAX(id),0)>1 ORDER BY COALESCE(MAX(id),0)");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().values[0].data.int64_value,6);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("copies aggregate parameters in SQL order and survives document destruction") {
    parse("SELECT MAX(score+?) AS n FROM items WHERE id>? GROUP BY score HAVING n>? ORDER BY MAX(id+?) DESC LIMIT ? OFFSET ?");
    const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false},
      {TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false}};
    turbodb_value_t parameters[]={turbodb_i64(2),turbodb_i64(1),turbodb_i64(15),turbodb_i64(1),turbodb_i64(2),turbodb_i64(1)};
    check_equal(bind_with(types,6),TURBODB_STATUS_OK); check_equal(open_with(parameters,6),TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document=NULL; parameters[0]=turbodb_i64(999);
    check_equal(next().values[0].data.int64_value,32); check_equal(next().values[0].data.int64_value,22);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("rejects ungrouped references and unsupported grouping forms before reading") {
    reject("SELECT id,COUNT(*) AS n FROM items",TURBODB_STATUS_SQL_ERROR,"column");
    reject("SELECT id FROM items GROUP BY score",TURBODB_STATUS_SQL_ERROR,"column");
    reject("SELECT score FROM items GROUP BY score HAVING id>1",TURBODB_STATUS_SQL_ERROR,"column");
    reject("SELECT score FROM items GROUP BY score ORDER BY id",TURBODB_STATUS_SQL_ERROR,"column");
    reject("SELECT COUNT(*) AS n FROM items WHERE COUNT(*)>0",TURBODB_STATUS_SQL_ERROR,"WHERE");
    reject("SELECT COUNT(DISTINCT name) AS n FROM items",TURBODB_STATUS_UNSUPPORTED,"numeric");
    reject("SELECT COUNT() AS n FROM items",TURBODB_STATUS_SQL_ERROR,"argument");
    reject("SELECT MAX(id,score) AS n FROM items",TURBODB_STATUS_SQL_ERROR,"argument");
    reject("SELECT MAX(COUNT(id)) AS n FROM items",TURBODB_STATUS_UNSUPPORTED,"function");
    reject("SELECT MIN(name) AS n FROM items",TURBODB_STATUS_UNSUPPORTED,"MIN/MAX");
    reject("SELECT name FROM items GROUP BY name",TURBODB_STATUS_UNSUPPORTED,"GROUP BY");
    reject("SELECT * FROM items GROUP BY id",TURBODB_STATUS_UNSUPPORTED,"star");
    reject("SELECT score FROM items GROUP BY score+1",TURBODB_STATUS_SQL_ERROR,"column");
    reject("SELECT COUNT(*)+1 AS n FROM items HAVING n>1",TURBODB_STATUS_UNSUPPORTED,"direct");
  }
  it("does not read grouped sources under LIMIT zero or cancellation and releases leases") {
    const char *sql[]={"SELECT COUNT(*) AS n FROM items LIMIT 0",
      "SELECT score,COUNT(*) AS n FROM items GROUP BY score ORDER BY n LIMIT 0"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      reset(); parse(sql[i]); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      check_equal(orm_tidesdb_sql_select_destroy(&plan,&error),TURBODB_STATUS_BUSY);
      check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
    }
    reset(); parse("SELECT score,COUNT(*) AS n FROM items GROUP BY score"); check_equal(bind_select(),TURBODB_STATUS_OK);
    order_source input={.count=TEST_ROWS};
    orm_sql_row_source source={.context=&input,.next=order_source_next,.budget=&budget,
      .types=vec_data_const(&plan.types),.columns=TEST_COLUMNS};
    check_equal(orm_tidesdb_sql_select_open_source(&plan,&source,NULL,0,&run,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_scan_cancel(&run.scan,&error),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_CANCELLED); check_equal(input.calls,0u);
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK); check_false(source.active);
    check_equal(orm_tidesdb_sql_select_open_source(&plan,&source,NULL,0,&run,&error),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_ROW); check_equal(input.calls,TEST_ROWS+1);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],TEST_ROWS);
  }
  it("locks aggregate argument failures and enforces shared group and materialization budgets") {
    parse("SELECT MAX(id+1) AS n FROM items"); rows[1][0]=turbodb_i64(INT64_MAX);
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.state,ORM_SQL_SCAN_CANCELLED);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
    reset(); parse("SELECT score,COUNT(*) AS n FROM items GROUP BY score ORDER BY n");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); budget.limits.statement.value[ORM_SQL_BUDGET_GROUPS]=1;
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.state,ORM_SQL_SCAN_CANCELLED);
    reset(); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=TEST_ROWS;
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.state,ORM_SQL_SCAN_CANCELLED);
  }
  it("releases every partially bound grouped plan after allocation failures") {
    parse("SELECT score,COUNT(*) AS n,MAX(id+1) AS hi FROM items WHERE id>0 GROUP BY score HAVING n>0 ORDER BY hi");
    reserve_calls=resize_calls=0; check_equal(bind_select(),TURBODB_STATUS_OK);
    const size_t reserves=reserve_calls,resizes=resize_calls;
    for(size_t i=1;i<=reserves+resizes;++i) {
      reset(); reserve_calls=resize_calls=0;
      if(i<=reserves) fail_reserve=i; else fail_resize=i-reserves;
      check_equal(bind_select(),TURBODB_STATUS_OUT_OF_MEMORY); check_null(plan.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    }
  }
  it("releases every partially opened grouped pipeline after allocation failures") {
    parse("SELECT COALESCE(score,0) AS k,COUNT(*) AS n,MAX(id+1) AS hi FROM items WHERE id>0 GROUP BY k HAVING n>0 ORDER BY hi");
    check_equal(bind_select(),TURBODB_STATUS_OK); const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    reserve_calls=resize_calls=0; open_rows(); const size_t reserves=reserve_calls,resizes=resize_calls;
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK);
    for(size_t i=1;i<=reserves+resizes;++i) {
      reserve_calls=resize_calls=fail_reserve=fail_resize=0;
      if(i<=reserves) fail_reserve=i; else fail_resize=i-reserves;
      check_equal(orm_tidesdb_sql_select_open(&plan,&rows[0][0],TEST_ROWS,&run,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(run.program); check_false(run.group_run.initialized); check_equal(plan.active_runs,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
    }
    fail_reserve=fail_resize=0; open_rows(); check_equal(next().state,ORM_SQL_SCAN_ROW);
  }
  it("cleans grouped capture and both sort stages on allocation or sorting failures") {
    parse("SELECT score,COUNT(*) AS n FROM items GROUP BY score ORDER BY n");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); reserve_calls=resize_calls=sort_calls=0;
    check_equal(next().state,ORM_SQL_SCAN_ROW);
    const size_t reserves=reserve_calls,resizes=resize_calls,sorts=sort_calls; check_equal(sorts,2u);
    for(size_t i=1;i<=reserves+resizes+sorts;++i) {
      reset(); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); reserve_calls=resize_calls=sort_calls=0;
      if(i<=reserves) fail_reserve=i;
      else if(i<=reserves+resizes) fail_resize=i-reserves;
      else fail_sort_at=i-reserves-resizes;
      orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED};
      check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      check_equal(out.state,ORM_SQL_SCAN_CANCELLED); check_equal(run.scan.state,ORM_SQL_SCAN_ERROR);
    }
  }

  it("deduplicates complete output tuples including NULL before sorting and pagination") {
    rows[2][1]=turbodb_i64(20); rows[4][1]=turbodb_null();
    parse("SELECT DISTINCT score FROM items ORDER BY score DESC LIMIT 2 OFFSET 1");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    check_equal(next().values[0].data.int64_value,20); check_equal(next().values[0].kind,TURBODB_VALUE_NULL);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],TEST_ROWS);
    reset(); parse("SELECT DISTINCT score,id>2 AS high FROM items ORDER BY 1,2");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    const int scores[]={0,0,20,20,30}, high[]={0,1,0,1,1};
    for(size_t i=0;i<TEST_ROWS;++i) {
      const orm_sql_scan_row out=next();
      check_equal(out.values[0].kind,i<2?TURBODB_VALUE_NULL:TURBODB_VALUE_INT64);
      if(i>=2) check_equal(out.values[0].data.int64_value,scores[i]);
      check_equal(out.values[1].data.boolean_value,high[i]);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("retains duplicates with ALL and eliminates them without an ordering clause") {
    for(size_t i=0;i<TEST_ROWS;++i) rows[i][1]=turbodb_i64(20);
    parse("SELECT ALL score FROM items"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[0].data.int64_value,20);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); parse("SELECT DISTINCT score FROM items"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    check_equal(next().values[0].data.int64_value,20); check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); parse("SELECT DISTINCT NULL AS n FROM items"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    check_equal(next().values[0].kind,TURBODB_VALUE_NULL); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("supports DISTINCT stars aliases selected expressions and expressions of selected source columns") {
    schema.count=2; const turbodb_value_t numeric[]={turbodb_i64(2),turbodb_i64(10),turbodb_i64(2),turbodb_i64(10),turbodb_i64(1),turbodb_i64(10)};
    parse("SELECT DISTINCT items.* FROM items ORDER BY 1 DESC"); check_equal(bind_select(),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_open(&plan,numeric,3,&run,&error),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,2); check_equal(next().values[0].data.int64_value,1);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); schema.count=TEST_COLUMNS;
    parse("SELECT DISTINCT id AS score FROM items ORDER BY items.id+1 DESC LIMIT 1");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().values[0].data.int64_value,5);
    reset(); parse("SELECT DISTINCT id+1 AS label FROM items ORDER BY items.id + 01 DESC LIMIT 1");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().values[0].data.int64_value,6);
  }
  it("compares compiled CASE IN BETWEEN null functions and predicates for DISTINCT ordering") {
    const char *sql[]={
      "SELECT DISTINCT CASE WHEN id>2 THEN 1 ELSE 0 END AS k FROM items ORDER BY CASE WHEN items.id>2 THEN 1 ELSE 0 END DESC",
      "SELECT DISTINCT id IN (1,2) AS k FROM items ORDER BY items.id IN (1,2) DESC",
      "SELECT DISTINCT id BETWEEN 2 AND 4 AS k FROM items ORDER BY items.id BETWEEN 2 AND 4 DESC",
      "SELECT DISTINCT COALESCE(score,0) AS k FROM items ORDER BY coalesce(items.score,0) DESC",
      "SELECT DISTINCT NULLIF(id,2) AS k FROM items ORDER BY NULLIF(items.id,2) DESC",
      "SELECT DISTINCT id>2 AND score IS NOT NULL AS k FROM items ORDER BY items.id>2 AND items.score IS NOT NULL DESC",
      "SELECT DISTINCT name LIKE 'a%' AS k FROM items ORDER BY items.name LIKE 'a%' DESC"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      reset(); parse(sql[i]); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      check_equal(next().state,ORM_SQL_SCAN_ROW);
    }
  }
  it("rejects DISTINCT ordering by unselected inputs or different compiled expressions") {
    const char *sql[]={
      "SELECT DISTINCT score FROM items ORDER BY id LIMIT 0",
      "SELECT DISTINCT id AS score FROM items ORDER BY items.score LIMIT 0",
      "SELECT DISTINCT id+1 AS k FROM items ORDER BY id+2",
      "SELECT DISTINCT id+1 AS k FROM items ORDER BY score+1",
      "SELECT DISTINCT id+1 AS k FROM items ORDER BY id-1",
      "SELECT DISTINCT CASE WHEN id>2 THEN 1 ELSE 0 END AS k FROM items ORDER BY CASE WHEN id>3 THEN 1 ELSE 0 END",
      "SELECT DISTINCT name LIKE 'a%' AS k FROM items ORDER BY name LIKE 'b%'",
      "SELECT DISTINCT name LIKE 'a%' AS k FROM items ORDER BY name NOT LIKE 'a%'"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) reject(sql[i],TURBODB_STATUS_SQL_ERROR,"outside");
    reject("SELECT DISTINCT name FROM items LIMIT 0",TURBODB_STATUS_UNSUPPORTED,"DISTINCT");
    reject("SELECT DISTINCT 'x' AS label FROM items",TURBODB_STATUS_UNSUPPORTED,"DISTINCT");
    columns[2].type=(orm_sql_type){TURBODB_VALUE_BLOB,false};
    reject("SELECT DISTINCT name FROM items",TURBODB_STATUS_UNSUPPORTED,"DISTINCT");
  }
  it("copies DISTINCT parameters and paginates unique outputs after discarding its document") {
    parse("SELECT DISTINCT COALESCE(score,?) AS k FROM items WHERE id>? ORDER BY k DESC LIMIT ? OFFSET ?");
    rows[2][1]=turbodb_i64(20); rows[4][1]=turbodb_null();
    const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false}};
    turbodb_value_t parameters[]={turbodb_i64(99),turbodb_i64(0),turbodb_i64(2),turbodb_i64(1)};
    check_equal(bind_with(types,4),TURBODB_STATUS_OK); check_equal(open_with(parameters,4),TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document=NULL; parameters[0]=turbodb_i64(-1);
    check_equal(next().values[0].data.int64_value,30); check_equal(next().values[0].data.int64_value,20);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); parse("SELECT DISTINCT id+? AS k FROM items ORDER BY id+?");
    check_equal(bind_with(types,2),TURBODB_STATUS_SQL_ERROR); check_null(plan.budget);
    reset(); parse("SELECT DISTINCT id FROM items ORDER BY id+?");
    check_equal(bind_with(types,1),TURBODB_STATUS_OK); check_equal(open_with(parameters,1),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value,1);
  }
  it("compares DISTINCT exact numeric endpoints and signed zero without lossy conversion") {
    columns[1].type=(orm_sql_type){TURBODB_VALUE_UINT64,true};
    rows[0][1]=turbodb_u64(UINT64_MAX); rows[1][1]=turbodb_u64(INT64_MAX); rows[2][1]=turbodb_u64(UINT64_MAX);
    rows[3][1]=turbodb_null(); rows[4][1]=turbodb_null();
    parse("SELECT DISTINCT score FROM items ORDER BY 1 DESC"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    check_equal(next().values[0].data.uint64_value,UINT64_MAX); check_equal(next().values[0].data.uint64_value,(uint64_t)INT64_MAX);
    check_equal(next().values[0].kind,TURBODB_VALUE_NULL); check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); columns[1].type=(orm_sql_type){TURBODB_VALUE_DOUBLE,true};
    rows[0][1]=turbodb_f64(-0.0); rows[1][1]=turbodb_f64(0.0); rows[2][1]=turbodb_f64(1.0);
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    check_equal(next().values[0].data.double_value,1.0); check_equal(next().values[0].data.double_value,0.0);
    check_equal(next().values[0].kind,TURBODB_VALUE_NULL); check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); columns[1].type=(orm_sql_type){TURBODB_VALUE_INT64,true};
    rows[0][1]=turbodb_i64(INT64_MIN); rows[1][1]=turbodb_i64(INT64_MAX); rows[2][1]=turbodb_i64(INT64_MIN);
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    check_equal(next().values[0].data.int64_value,INT64_MAX); check_equal(next().values[0].data.int64_value,INT64_MIN);
    check_equal(next().values[0].kind,TURBODB_VALUE_NULL); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("evaluates all DISTINCT outputs before pagination and locks late expression failures") {
    parse("SELECT DISTINCT CASE WHEN id=5 THEN 9223372036854775807+id ELSE 0 END AS k FROM items LIMIT 1 OFFSET 9");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.state,ORM_SQL_SCAN_CANCELLED); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],TEST_ROWS);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
  }
  it("does not read DISTINCT LIMIT zero cancelled or empty inputs and releases retained state on close") {
    const char *sql[]={"SELECT DISTINCT score FROM items LIMIT 0","SELECT DISTINCT score FROM items"};
    for(size_t i=0;i<2;++i) {
      reset(); parse(sql[i]); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      if(i) check_equal(orm_tidesdb_sql_scan_cancel(&run.scan,&error),TURBODB_STATUS_OK);
      reserve_calls=resize_calls=0; check_equal(next().state,i?ORM_SQL_SCAN_CANCELLED:ORM_SQL_SCAN_DONE);
      check_equal(reserve_calls,0u); check_equal(resize_calls,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
    }
    reset(); check_equal(bind_select(),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_open(&plan,NULL,0,&run,&error),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK); open_rows(); check_equal(next().state,ORM_SQL_SCAN_ROW);
    check_equal(orm_tidesdb_sql_scan_cancel(&run.scan,&error),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_CANCELLED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],TEST_ROWS);
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK); check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
  }
  it("bounds DISTINCT by all candidate rows even when all outputs are duplicates") {
    parse("SELECT DISTINCT 1 AS k FROM items LIMIT 1"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=TEST_ROWS-1;
    orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(out.state,ORM_SQL_SCAN_CANCELLED);
    reset(); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=TEST_ROWS;
    check_equal(next().values[0].data.int64_value,1); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("owns DISTINCT values from a reusable pull buffer across registry growth") {
    parse("SELECT DISTINCT id>6 AS k FROM items ORDER BY k DESC"); check_equal(bind_select(),TURBODB_STATUS_OK);
    const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,true},{TURBODB_VALUE_TEXT,false}};
    order_source data={.count=13}; orm_sql_row_source source={&budget,types,TEST_COLUMNS,&data,order_source_next,false};
    check_equal(orm_tidesdb_sql_select_open_source(&plan,&source,NULL,0,&run,&error),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.boolean_value,1); check_equal(next().values[0].data.boolean_value,0);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(data.calls,14u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],13u);
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK); check_false(source.active);
  }
  it("cleans DISTINCT registry growth allocations and either sort failure before publishing") {
    parse("SELECT DISTINCT id>6 AS k FROM items ORDER BY k DESC"); check_equal(bind_select(),TURBODB_STATUS_OK);
    const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,true},{TURBODB_VALUE_TEXT,false}};
    order_source data={.count=13}; orm_sql_row_source source={&budget,types,TEST_COLUMNS,&data,order_source_next,false};
    check_equal(orm_tidesdb_sql_select_open_source(&plan,&source,NULL,0,&run,&error),TURBODB_STATUS_OK);
    reserve_calls=resize_calls=sort_calls=0; check_equal(next().state,ORM_SQL_SCAN_ROW);
    const size_t reserves=reserve_calls,resizes=resize_calls,sorts=sort_calls; check_equal(sorts,2u);
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK);
    for(size_t i=1;i<=reserves+resizes+sorts;++i) {
      data=(order_source){.count=13};
      check_equal(orm_tidesdb_sql_select_open_source(&plan,&source,NULL,0,&run,&error),TURBODB_STATUS_OK);
      reserve_calls=resize_calls=sort_calls=0;
      if(i<=reserves) fail_reserve=i; else if(i<=reserves+resizes) fail_resize=i-reserves;
      else fail_sort_at=i-reserves-resizes;
      orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED};
      check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_OUT_OF_MEMORY); check_equal(out.state,ORM_SQL_SCAN_CANCELLED);
      fail_reserve=fail_resize=fail_sort_at=0;
      check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK); check_false(source.active);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    }
  }
  it("charges every DISTINCT execution stage and releases all workspace at each step limit") {
    parse("SELECT DISTINCT id>2 AS k FROM items ORDER BY k DESC LIMIT 1"); check_equal(bind_select(),TURBODB_STATUS_OK);
    const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; open_rows();
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]; check_equal(next().state,ORM_SQL_SCAN_ROW);
    const uint64_t cost=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK);
    for(uint64_t allowance=0;allowance<cost;++allowance) {
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=UINT64_MAX;
      open_rows(); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=
          budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+allowance;
      orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED};
      check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(out.state,ORM_SQL_SCAN_CANCELLED); check_equal(run.scan.state,ORM_SQL_SCAN_ERROR);
      check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    }
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=UINT64_MAX;
    open_rows(); budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.state,ORM_SQL_SCAN_CANCELLED);
  }
  it("rejects nonfinite DISTINCT outputs and does not publish before source EOF") {
    columns[1].type=(orm_sql_type){TURBODB_VALUE_DOUBLE,true};
    for(size_t i=0;i<TEST_ROWS;++i) rows[i][1]=turbodb_f64(i==TEST_ROWS-1?NAN:0.0);
    parse("SELECT DISTINCT score FROM items LIMIT 1"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_TYPE_ERROR); check_equal(out.state,ORM_SQL_SCAN_CANCELLED);
    reset(); columns[1].type=(orm_sql_type){TURBODB_VALUE_INT64,true};
    parse("SELECT DISTINCT 1 AS k FROM items LIMIT 1"); check_equal(bind_select(),TURBODB_STATUS_OK);
    const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,true},{TURBODB_VALUE_TEXT,false}};
    order_source data={.count=13,.fail_at=10}; orm_sql_row_source source={&budget,types,TEST_COLUMNS,&data,order_source_next,false};
    check_equal(orm_tidesdb_sql_select_open_source(&plan,&source,NULL,0,&run,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_DATASTORE_ERROR); check_equal(out.state,ORM_SQL_SCAN_CANCELLED);
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_DATASTORE_ERROR); check_equal(data.calls,10u);
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK); check_false(source.active);
  }
  it("charges compiled expression comparison and preserves its output on admission failure") {
    parse("SELECT DISTINCT name LIKE 'a%' AS k FROM items ORDER BY items.name LIKE 'a%'");
    check_equal(bind_select(),TURBODB_STATUS_OK);
    const orm_sql_scan_expression *output=vec_at_const(&plan.expressions,0);
    const orm_sql_scan_order *order=vec_at_const(&plan.orders,0);
    bool same=false;
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_expr_same(output->program,output->slots,order->expression.program,order->expression.slots,
        &same,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_false(same);
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=TEST_LIMIT;
    check_equal(orm_tidesdb_sql_expr_same(output->program,output->slots,order->expression.program,order->expression.slots,
        &same,&error),TURBODB_STATUS_OK); check_true(same);
  }
  it("orders nullable keys in both directions and preserves input order for ties") {
    rows[4][1]=turbodb_i64(20);
    parse("SELECT id FROM items ORDER BY score ASC"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    const int ascending[]={1,3,2,5,4};
    for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[0].data.int64_value,ascending[i]);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],TEST_ROWS);
    reset(); parse("SELECT id FROM items ORDER BY score DESC,id DESC"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    const int descending[]={4,5,2,3,1};
    for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[0].data.int64_value,descending[i]);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("resolves output aliases before source names and qualified names only in source scope") {
    parse("SELECT -id AS score,id FROM items ORDER BY score ASC LIMIT 2 OFFSET 1");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().values[1].data.int64_value,4);
    check_equal(next().values[1].data.int64_value,3); check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); parse("SELECT -id AS score,id FROM items ORDER BY items.score ASC LIMIT 1");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().values[1].data.int64_value,1);
    reset(); parse("SELECT items.* FROM items ORDER BY 2 DESC,1 ASC LIMIT 1");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().values[0].data.int64_value,5);
  }
  it("binds parameters across output filter sort and pagination before discarding the document") {
    parse("SELECT id+? AS label FROM items WHERE id>? ORDER BY score+? DESC,id LIMIT ? OFFSET ?");
    const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false},
      {TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false}};
    turbodb_value_t parameters[]={turbodb_i64(10),turbodb_i64(1),turbodb_i64(2),turbodb_i64(2),turbodb_i64(1)};
    check_equal(bind_with(types,5),TURBODB_STATUS_OK); sqlparser_document_destroy(document); document=NULL;
    check_equal(open_with(parameters,5),TURBODB_STATUS_OK); parameters[0]=turbodb_i64(99);
    check_equal(next().values[0].data.int64_value,14); check_equal(next().values[0].data.int64_value,12);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("evaluates output expressions only after sorted pagination but evaluates all ordering keys") {
    parse("SELECT 9223372036854775807+id AS bad FROM items ORDER BY id LIMIT 1 OFFSET 5");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); parse("SELECT id FROM items ORDER BY 9223372036854775807+id LIMIT 1");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.state,ORM_SQL_SCAN_CANCELLED); check_equal(run.scan.state,ORM_SQL_SCAN_ERROR);
  }
  it("rejects invalid ordering positions names and collation-dependent keys even with LIMIT zero") {
    reject("SELECT id FROM items ORDER BY 0 LIMIT 0",TURBODB_STATUS_SQL_ERROR,"position");
    reject("SELECT id FROM items ORDER BY 2 LIMIT 0",TURBODB_STATUS_SQL_ERROR,"position");
    reject("SELECT id FROM items ORDER BY missing LIMIT 0",TURBODB_STATUS_SQL_ERROR,"column");
    reject("SELECT id FROM items ORDER BY name LIMIT 0",TURBODB_STATUS_UNSUPPORTED,"numeric");
    reject("SELECT name AS label FROM items ORDER BY label LIMIT 0",TURBODB_STATUS_UNSUPPORTED,"numeric");
    reject("SELECT -id AS score FROM items ORDER BY score+1 LIMIT 0",TURBODB_STATUS_UNSUPPORTED,"standalone");
    reject("SELECT id AS label FROM items ORDER BY label+1 LIMIT 0",TURBODB_STATUS_UNSUPPORTED,"standalone");
    reject("SELECT id FROM items ORDER BY -1 LIMIT 0",TURBODB_STATUS_UNSUPPORTED,"signed");
  }
  it("performs no sorting allocations or reads for LIMIT zero or cancellation before next") {
    parse("SELECT id FROM items ORDER BY score LIMIT 0"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    reserve_calls=resize_calls=0; fail_reserve=fail_resize=1;
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(reserve_calls,0u); check_equal(resize_calls,0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
    reset(); parse("SELECT id FROM items ORDER BY score"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    check_equal(orm_tidesdb_sql_scan_cancel(&run.scan,&error),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_CANCELLED); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
  }
  it("owns every mutable pull-source row and payload through registry growth and sorting") {
    parse("SELECT id,name FROM items ORDER BY score"); check_equal(bind_select(),TURBODB_STATUS_OK);
    order_source data={.count=13}; const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,true},{TURBODB_VALUE_TEXT,false}};
    orm_sql_row_source source={&budget,types,TEST_COLUMNS,&data,order_source_next,false};
    check_equal(orm_tidesdb_sql_select_open_source(&plan,&source,NULL,0,&run,&error),TURBODB_STATUS_OK);
    check_equal(data.calls,0u);
    for(size_t i=0;i<data.count;++i) {
      orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,(int64_t)(data.count-i));
      check_equal(row.values[1].data.text_value.len,1u); check_equal(row.values[1].data.text_value.data[0],(char)('a'+data.count-i-1));
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(data.calls,data.count+1);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],data.count);
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK); check_false(source.active);
  }
  it("locks a later source error without publishing a sorted prefix or retrying reads") {
    parse("SELECT id FROM items ORDER BY score LIMIT 1"); check_equal(bind_select(),TURBODB_STATUS_OK);
    order_source data={.count=13,.fail_at=10}; const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,true},{TURBODB_VALUE_TEXT,false}};
    orm_sql_row_source source={&budget,types,TEST_COLUMNS,&data,order_source_next,false};
    check_equal(orm_tidesdb_sql_select_open_source(&plan,&source,NULL,0,&run,&error),TURBODB_STATUS_OK);
    orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(out.state,ORM_SQL_SCAN_CANCELLED); check_equal(data.calls,10u);
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_DATASTORE_ERROR); check_equal(data.calls,10u);
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK); check_false(source.active);
  }
  it("enforces materialized row capacity on all matches before applying LIMIT") {
    parse("SELECT id FROM items ORDER BY score LIMIT 1"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=TEST_ROWS-1;
    orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.state,ORM_SQL_SCAN_CANCELLED); reset();
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=TEST_ROWS;
    check_equal(next().values[0].data.int64_value,1); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("refunds each ordering bind allocation failure and leaves no borrowed expression behind") {
    parse("SELECT id+1 AS label FROM items ORDER BY label,score+2 DESC");
    reserve_calls=resize_calls=0; check_equal(bind_select(),TURBODB_STATUS_OK);
    const size_t reserves=reserve_calls,resizes=resize_calls;
    for(size_t i=1;i<=reserves+resizes;++i) {
      reset(); reserve_calls=resize_calls=0;
      if(i<=reserves) fail_reserve=i; else fail_resize=i-reserves;
      check_equal(bind_select(),TURBODB_STATUS_OUT_OF_MEMORY); check_null(plan.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    }
  }
  it("refunds every ordering run-open allocation failure and permits plan reuse") {
    parse("SELECT id+1 AS label FROM items ORDER BY label,score+2 DESC"); check_equal(bind_select(),TURBODB_STATUS_OK);
    const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    reserve_calls=resize_calls=0; open_rows(); const size_t reserves=reserve_calls,resizes=resize_calls;
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK);
    for(size_t i=1;i<=reserves+resizes;++i) {
      reserve_calls=resize_calls=0;
      if(i<=reserves) fail_reserve=i; else fail_resize=i-reserves;
      check_equal(orm_tidesdb_sql_select_open(&plan,&rows[0][0],TEST_ROWS,&run,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(run.program); check_equal(plan.active_runs,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
      fail_reserve=fail_resize=0;
    }
    open_rows(); check_equal(next().state,ORM_SQL_SCAN_ROW);
  }
  it("refunds every sort capture allocation and scratch failure after close") {
    parse("SELECT id,name FROM items ORDER BY score+1 DESC"); check_equal(bind_select(),TURBODB_STATUS_OK);
    const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,true},{TURBODB_VALUE_TEXT,false}};
    order_source data={.count=13}; orm_sql_row_source source={&budget,types,TEST_COLUMNS,&data,order_source_next,false};
    check_equal(orm_tidesdb_sql_select_open_source(&plan,&source,NULL,0,&run,&error),TURBODB_STATUS_OK);
    reserve_calls=resize_calls=0; check_equal(next().state,ORM_SQL_SCAN_ROW);
    const size_t reserves=reserve_calls,resizes=resize_calls;
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK);
    for(size_t i=0;i<=reserves+resizes;++i) {
      data=(order_source){.count=13};
      check_equal(orm_tidesdb_sql_select_open_source(&plan,&source,NULL,0,&run,&error),TURBODB_STATUS_OK);
      reserve_calls=resize_calls=0;
      if(!i) fail_sort=true; else if(i<=reserves) fail_reserve=i; else fail_resize=i-reserves;
      orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED};
      check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      check_equal(out.state,ORM_SQL_SCAN_CANCELLED);
      fail_sort=false; fail_reserve=fail_resize=0;
      check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u); check_false(source.active);
    }
  }
  it("sorts exact integer endpoints finite doubles signed zero and nullable boolean keys") {
    rows[0][0]=turbodb_i64(INT64_MIN); rows[4][0]=turbodb_i64(INT64_MAX);
    parse("SELECT id FROM items ORDER BY id DESC LIMIT 1"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    check_equal(next().values[0].data.int64_value,INT64_MAX);
    reset(); columns[1].type=(orm_sql_type){TURBODB_VALUE_UINT64,true};
    rows[1][1]=turbodb_u64(UINT64_MAX); rows[2][1]=turbodb_u64(0); rows[3][1]=turbodb_u64(1); rows[4][1]=turbodb_u64(INT64_MAX);
    parse("SELECT score FROM items ORDER BY score DESC"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    check_equal(next().values[0].data.uint64_value,UINT64_MAX); check_equal(next().values[0].data.uint64_value,(uint64_t)INT64_MAX);
    reset(); columns[1].type=(orm_sql_type){TURBODB_VALUE_DOUBLE,true};
    rows[1][1]=turbodb_f64(0.0); rows[2][1]=turbodb_f64(-0.0); rows[3][1]=turbodb_f64(-1.0); rows[4][1]=turbodb_f64(2.0);
    parse("SELECT id FROM items ORDER BY score"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    check_equal(next().values[0].data.int64_value,INT64_MIN); check_equal(next().values[0].data.int64_value,4);
    check_equal(next().values[0].data.int64_value,2); check_equal(next().values[0].data.int64_value,3);
    reset(); columns[1].type=(orm_sql_type){TURBODB_VALUE_BOOLEAN,true};
    for(size_t i=1;i<TEST_ROWS;++i) rows[i][1]=turbodb_bool(i%2!=0);
    parse("SELECT id FROM items ORDER BY score DESC,id"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    check_equal(next().values[0].data.int64_value,2); check_equal(next().values[0].data.int64_value,4);
  }
  it("locks nonfinite key and work or execution quota failures before returning a row") {
    for(size_t mode=0;mode<3;++mode) {
      reset(); columns[1].type=(orm_sql_type){TURBODB_VALUE_DOUBLE,true};
      for(size_t i=0;i<TEST_ROWS;++i) rows[i][1]=turbodb_f64((double)i);
      parse("SELECT id FROM items ORDER BY score LIMIT 1"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      if(!mode) rows[4][1]=turbodb_f64(INFINITY);
      else {
        const orm_sql_budget_resource resource=mode==1?ORM_SQL_BUDGET_WORK_BYTES:ORM_SQL_BUDGET_EXECUTION_STEPS;
        budget.limits.statement.value[resource]=budget.used.value[resource];
      }
      orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED};
      const turbodb_status_t expected=mode?TURBODB_STATUS_LIMIT_EXCEEDED:TURBODB_STATUS_TYPE_ERROR;
      check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),expected); check_equal(out.state,ORM_SQL_SCAN_CANCELLED);
      check_equal(orm_tidesdb_sql_scan_next(&run.scan,&out,&error),expected);
    }
  }
  it("checks ordering on an empty source and releases cancellation after materialization") {
    parse("SELECT id FROM items ORDER BY score"); check_equal(bind_select(),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_open(&plan,NULL,0,&run,&error),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK); open_rows();
    check_equal(next().state,ORM_SQL_SCAN_ROW); check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],TEST_ROWS);
    check_equal(orm_tidesdb_sql_scan_cancel(&run.scan,&error),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_CANCELLED);
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
  }
  it("binds qualified aliases and executes after document and schema destruction") {
    parse("SELECT `t` . `name` AS label, t.id AS ident FROM items AS t "
          "WHERE t.score >= 20 AND id < 5 LIMIT 1 OFFSET 1");
    check_equal(bind_select(), TURBODB_STATUS_OK);
    check_equal(vec_size(&plan.columns), 2u);
    check_equal(strcmp(orm_tidesdb_sql_select_column_at(&plan, 0)->name, "label"), 0);
    check_equal(orm_tidesdb_sql_select_column_at(&plan, 1)->type.kind, TURBODB_VALUE_INT64);
    sqlparser_document_destroy(document); document = NULL;
    memset(columns, 0, sizeof(columns)); memset(&schema, 0, sizeof(schema));
    open_rows();
    orm_sql_scan_row row = next();
    check_equal(row.state, ORM_SQL_SCAN_ROW); check_equal(row.count, 2u);
    check_equal(row.values[0].data.text_value.len, 1u);
    check_equal(row.values[0].data.text_value.data[0], 'd');
    check_equal(row.values[1].data.int64_value, 4);
    check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 4u);
    check_null(orm_tidesdb_sql_select_column_at(&plan, 2));
  }

  it("expands stars in schema order and preserves declared nullability") {
    const char *sql[] = {"SELECT * FROM items", "SELECT t.* FROM items t", "SELECT `items` . * FROM `items`"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
      reset(); parse(sql[i]); check_equal(bind_select(), TURBODB_STATUS_OK); open_rows();
      orm_sql_scan_row row = next();
      check_equal(row.count, TEST_COLUMNS); check_equal(row.values[0].data.int64_value, 1);
      check_equal(row.values[1].kind, TURBODB_VALUE_NULL);
      check_equal(strcmp(orm_tidesdb_sql_select_column_at(&plan, 1)->name, "score"), 0);
      check_equal(orm_tidesdb_sql_select_column_at(&plan, 1)->type.nullable, true);
    }
  }

  it("normalizes both LIMIT forms and supports maximum unsigned offsets without overflow") {
    const char *sql[] = {"SELECT id FROM items LIMIT 1, 2", "SELECT id FROM items LIMIT 2 OFFSET 1"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
      reset(); parse(sql[i]); check_equal(bind_select(), TURBODB_STATUS_OK); open_rows();
      check_equal(next().values[0].data.int64_value, 2);
      check_equal(next().values[0].data.int64_value, 3);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 3u);
    }
    reset(); parse("SELECT id FROM items LIMIT 18446744073709551615 OFFSET 18446744073709551615");
    check_equal(bind_select(), TURBODB_STATUS_OK); check_equal(plan.limit.literal, UINT64_MAX); check_equal(plan.offset.literal, UINT64_MAX);
    open_rows(); check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], TEST_ROWS);
  }

  it("resolves WHERE against source columns including repeated occurrences and alias shadowing") {
    parse("SELECT id AS score FROM items WHERE score >= 20 AND score < 40 AND id != 0");
    check_equal(bind_select(), TURBODB_STATUS_OK); check_equal(vec_size(&plan.slots), 3u); open_rows();
    check_equal(next().values[0].data.int64_value, 2);
    check_equal(next().values[0].data.int64_value, 4);
    check_equal(next().state, ORM_SQL_SCAN_DONE);
    reject("SELECT id AS answer FROM items WHERE answer = 1", TURBODB_STATUS_SQL_ERROR, "unknown column");
    reject("SELECT items.id FROM items t", TURBODB_STATUS_SQL_ERROR, "qualifier");
    reject("SELECT t.* FROM items", TURBODB_STATUS_SQL_ERROR, "qualifier");
  }

  it("rejects unknown names and duplicate outputs before opening even for LIMIT zero") {
    reject("SELECT missing FROM items LIMIT 0", TURBODB_STATUS_SQL_ERROR, "unknown column");
    reject("SELECT id FROM absent LIMIT 0", TURBODB_STATUS_SQL_ERROR, "unknown table");
    reject("SELECT id FROM items WHERE FALSE AND missing = 0 LIMIT 0", TURBODB_STATUS_SQL_ERROR, "unknown column");
    reject("SELECT id, id FROM items", TURBODB_STATUS_SQL_ERROR, "duplicate output");
    reject("SELECT t.*, id FROM items t", TURBODB_STATUS_SQL_ERROR, "duplicate output");
    reject("SELECT id AS same, score AS same FROM items", TURBODB_STATUS_SQL_ERROR, "duplicate output");
    reset(); parse("SELECT id, id AS other FROM items LIMIT 0");
    check_equal(bind_select(), TURBODB_STATUS_OK); open_rows(); check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
  }

  it("validates empty-table schemas without inferring types from data") {
    parse("SELECT score FROM items WHERE score IS NULL"); check_equal(bind_select(), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_open(&plan, NULL, 0, &run, &error), TURBODB_STATUS_OK);
    check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(orm_tidesdb_sql_select_column_at(&plan, 0)->type.kind, TURBODB_VALUE_INT64);
    reset(); columns[1].name = columns[0].name;
    check_equal(bind_select(), TURBODB_STATUS_SQL_ERROR); check_contains(error.message, "duplicate schema");
    reset(); columns[1].name = (vstr){"score", 5}; columns[1].type.kind = (turbodb_value_kind_t)-1;
    check_equal(bind_select(), TURBODB_STATUS_TYPE_ERROR);
  }

  it("runs constant filters without input slots and reuses an immutable bound plan") {
    const char *sql[] = {"SELECT id FROM items WHERE FALSE", "SELECT id FROM items WHERE NULL",
                        "SELECT id FROM items WHERE TRUE LIMIT 1"};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
      reset(); parse(sql[i]); check_equal(bind_select(), TURBODB_STATUS_OK);
      check_equal(vec_size(&plan.slots), 0u);
      const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for (unsigned repeat = 0; repeat < 2; ++repeat) {
        open_rows(); check_equal(next().state, i == 2 ? ORM_SQL_SCAN_ROW : ORM_SQL_SCAN_DONE);
        check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
      }
    }
  }

  it("fails explicitly on unsupported clauses projections and parameter positions") {
    const char *sql[] = {
      "SELECT SQL_CALC_FOUND_ROWS id FROM items",
      "EXPLAIN SELECT id FROM items",
      "SELECT COUNT(DISTINCT name) AS n FROM items", "SELECT id FROM items HAVING id > 0", "SELECT id FROM items JOIN items t ON items.id = t.id",
      "SELECT id FROM (SELECT id FROM items) t", "SELECT 1", "SELECT id + 1 FROM items", "SELECT COUNT(*) FROM items",
      "SELECT id FROM items UNION SELECT id FROM items", "SELECT id FROM items; SELECT id FROM items",
      "SELECT id FROM items WHERE TRUE OR LENGTH(name) > 1", "SELECT id FROM items WHERE id = '1'",
      "SELECT id FROM items LIMIT -1", "SELECT id FROM items LIMIT 1.5", "SELECT id FROM items LIMIT 1+1"
    };
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) reject(sql[i], TURBODB_STATUS_UNSUPPORTED, "");
    reject("SELECT ? FROM items", TURBODB_STATUS_SQL_ERROR, "parameter count");
    reject("SELECT id FROM items LIMIT 18446744073709551616", TURBODB_STATUS_LIMIT_EXCEEDED, "integer");
  }

  it("enforces the declared ASCII case-sensitive identifier subset") {
    reject("SELECT ID FROM items", TURBODB_STATUS_SQL_ERROR, "unknown column");
    reject("SELECT id FROM Items", TURBODB_STATUS_SQL_ERROR, "unknown table");
    reject("SELECT id AS 'label' FROM items", TURBODB_STATUS_UNSUPPORTED, "identifier");
    reject("SELECT id AS `a``b` FROM items", TURBODB_STATUS_UNSUPPORTED, "identifier");
    reject("SELECT items /*comment*/ . id FROM items", TURBODB_STATUS_UNSUPPORTED, "identifier");
    reject("SELECT db.items.id FROM items", TURBODB_STATUS_SQL_ERROR, "qualifier");
    reset(); parse("SELECT id FROM items");
    char long_name[ORM_SQL_SELECT_NAME_BYTES + 1]; memset(long_name, 'x', sizeof(long_name));
    columns[0].name = (vstr){long_name, sizeof(long_name)};
    check_equal(bind_select(), TURBODB_STATUS_LIMIT_EXCEEDED);
  }

  it("holds no-WHERE plans until all runs close including cancelled and completed runs") {
    parse("SELECT id FROM items LIMIT 1"); check_equal(bind_select(), TURBODB_STATUS_OK); open_rows();
    orm_sql_select_run second = {0};
    check_equal(orm_tidesdb_sql_select_open(&plan, NULL, 0, &second, &error), TURBODB_STATUS_OK);
    check_equal(plan.active_runs, 2u);
    check_equal(orm_tidesdb_sql_select_destroy(&plan, &error), TURBODB_STATUS_BUSY);
    check_equal(next().state, ORM_SQL_SCAN_ROW); check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_scan_cancel(&second.scan, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_destroy(&plan, &error), TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_select_close(&second, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_close(&second, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_destroy(&plan, &error), TURBODB_STATUS_OK);
  }

  it("retains the first runtime type error and clears borrowed output") {
    parse("SELECT name FROM items WHERE score >= 20"); check_equal(bind_select(), TURBODB_STATUS_OK); open_rows();
    rows[1][1] = turbodb_text("invalid");
    orm_sql_scan_row output = {.state = ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &output, &error), TURBODB_STATUS_TYPE_ERROR);
    check_equal(output.state, ORM_SQL_SCAN_CANCELLED);
    const uint64_t steps = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &output, &error), TURBODB_STATUS_TYPE_ERROR);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], steps);
    check_equal(orm_tidesdb_sql_select_destroy(&plan, &error), TURBODB_STATUS_BUSY);
  }

  it("honors AST plan work step and depth bounds with complete cleanup") {
    parse("SELECT id FROM items WHERE NOT (id > 0)");
    check_equal(orm_tidesdb_sql_select_bind(document, &schema, 1, &budget, &plan, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    const orm_sql_budget_resource resources[] = {ORM_SQL_BUDGET_AST_NODES, ORM_SQL_BUDGET_PLAN_NODES,
        ORM_SQL_BUDGET_WORK_BYTES, ORM_SQL_BUDGET_EXECUTION_STEPS};
    for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); ++i) {
      const uint64_t saved = limits.statement.value[resources[i]];
      limits.statement.value[resources[i]] = 1; reset();
      check_equal(bind_select(), TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
      limits.statement.value[resources[i]] = saved;
    }
    reset(); check_equal(bind_select(), TURBODB_STATUS_OK);
  }

  it("retains the window owner through BUSY close and resumes cleanup after its consumer closes") {
    parse("SELECT id,ROW_NUMBER() OVER(ORDER BY id DESC) AS n FROM items ORDER BY id");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    select_window_run *stage=vec_at(&run.window_run,0);
    orm_sql_window *window=vec_at(&stage->stages,0);
    orm_sql_row_source *source=orm_tidesdb_sql_window_source(window);
    check_equal(orm_tidesdb_sql_scan_close(&run.scan,&error),TURBODB_STATUS_OK);
    const size_t projection[]={0,source->columns-1};
    const orm_sql_scan_spec spec={.projection=projection,.projection_count=2,.limit=UINT64_MAX};
    orm_sql_scan consumer={0}; check_equal(orm_tidesdb_sql_scan_open_source(source,&spec,&budget,&consumer,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_BUSY);
    check_true(run.program==&plan); check_equal(plan.active_runs,1u); check_true(source->active);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    check_equal(orm_tidesdb_sql_select_destroy(&plan,&error),TURBODB_STATUS_BUSY);
    orm_sql_scan_row output={0}; check_equal(orm_tidesdb_sql_scan_next(&consumer,&output,&error),TURBODB_STATUS_OK);
    check_equal(output.state,ORM_SQL_SCAN_ROW); check_equal(output.values[0].data.int64_value,5);
    check_equal(output.values[1].data.int64_value,1);
    check_equal(orm_tidesdb_sql_scan_close(&consumer,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK); check_equal(plan.active_runs,0u);
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK);
  }
  it("refunds every window binding allocation including grouped hidden expressions") {
    const char *sql[]={"SELECT score,LAG(COUNT(*),1,0) OVER last+RANK() OVER ranked+"
        "FIRST_VALUE(COUNT(*)) OVER last+LAST_VALUE(COUNT(*)) OVER last+NTH_VALUE(COUNT(*),1) OVER last+"
        "MAX(COUNT(*)) OVER last+MIN(COUNT(*)) OVER last+COUNT(*) OVER last+COUNT(score) OVER last AS n,"
        "SUM(1.0) OVER last AS total,AVG(2.0) OVER last AS mean,"
        "VAR_POP(VAR_POP(score)) OVER last AS vp,VAR_SAMP(score) OVER last AS vs,"
        "STDDEV_POP(score) OVER last AS dp,STDDEV_SAMP(score) OVER last AS ds "
        "FROM items GROUP BY score HAVING score>0 WINDOW last AS (first ORDER BY COUNT(*) DESC ROWS BETWEEN 1 PRECEDING AND CURRENT ROW),"
        "first AS (),ranked AS (ORDER BY score RANGE CURRENT ROW),unused AS (PARTITION BY score) ORDER BY n",
        "SELECT id,BIT_AND(BIT_AND(score)) OVER w AS a,BIT_OR(BIT_OR(score)) OVER w AS o,"
        "BIT_XOR(BIT_XOR(score)) OVER w AS x FROM items GROUP BY id HAVING id>0 "
        "WINDOW w AS(ORDER BY id ROWS 1 PRECEDING) ORDER BY id"};
    for(size_t query=0;query<sizeof(sql)/sizeof(sql[0]);++query) {
      reset(); parse(sql[query]);
      reserve_calls=resize_calls=0;
      const turbodb_status_t bind_status=bind_select();
      info("window bind: %s",error.message); check_equal(bind_status,TURBODB_STATUS_OK);
      const size_t counts[]={reserve_calls,resize_calls};
      for(size_t pass=0;pass<sizeof(counts)/sizeof(counts[0]);++pass) {
        for(size_t point=1;point<=counts[pass];++point) {
          reset(); reserve_calls=resize_calls=0;
          if(pass) fail_resize=point; else fail_reserve=point;
          check_equal(bind_select(),TURBODB_STATUS_OUT_OF_MEMORY); check_null(plan.budget);
          check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
          check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
        }
      }
      reset(); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().state,ORM_SQL_SCAN_ROW);
    }
  }
  it("restores the plan waterline on every chained window open allocation failure") {
    parse("SELECT id,LAG(score,1,0) OVER(ORDER BY id)+LEAD(id,2,0) OVER(PARTITION BY id>2 ORDER BY id DESC)+"
        "COUNT(*) OVER(ORDER BY id)+COUNT(score) OVER(ORDER BY id)+MIN(id) OVER(ORDER BY id)+MAX(id) OVER(ORDER BY id) AS n,"
        "SUM(1.0) OVER(ORDER BY id) AS total,AVG(2.0) OVER(ORDER BY id ROWS 1 PRECEDING) AS mean "
        "FROM items WHERE id>0 ORDER BY id"); check_equal(bind_select(),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    reserve_calls=resize_calls=0; open_rows(); const size_t counts[]={reserve_calls,resize_calls};
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK);
    for(size_t pass=0;pass<sizeof(counts)/sizeof(counts[0]);++pass) {
      for(size_t point=1;point<=counts[pass];++point) {
        reserve_calls=resize_calls=0;
        if(pass) fail_resize=point; else fail_reserve=point;
        check_equal(open_with(NULL,0),TURBODB_STATUS_OUT_OF_MEMORY); fail_reserve=fail_resize=0;
        check_null(run.program); check_equal(plan.active_runs,0u);
        check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
    }
    open_rows(); check_equal(next().state,ORM_SQL_SCAN_ROW);
  }
  it("latches every window materialization allocation and sort failure without publishing a row") {
    const char *sql[]={"SELECT id,ROW_NUMBER() OVER(ORDER BY score)+RANK() OVER(ORDER BY id DESC)+"
        "FIRST_VALUE(id) OVER(ORDER BY id RANGE 1 PRECEDING)+LAST_VALUE(id) OVER(ORDER BY id ROWS 1 PRECEDING)+"
        "NTH_VALUE(id,1) OVER(ORDER BY id RANGE 1 PRECEDING)+COUNT(*) OVER(ORDER BY id)+COUNT(score) OVER(ORDER BY id)+"
        "MIN(id) OVER(ORDER BY id ROWS 1 PRECEDING)+MAX(id) OVER(ORDER BY id ROWS 1 PRECEDING) AS n,"
        "SUM(1.0) OVER(ORDER BY id) AS total,AVG(2.0) OVER(ORDER BY id ROWS 1 PRECEDING) AS mean,"
        "VAR_POP(score) OVER(ORDER BY id) AS vp,VAR_SAMP(score) OVER(ORDER BY id) AS vs,"
        "STDDEV_POP(score) OVER(ORDER BY id ROWS 1 PRECEDING) AS dp,STDDEV_SAMP(score) OVER(ORDER BY id) AS ds FROM items ORDER BY id",
        "SELECT id,BIT_AND(score) OVER w AS a,BIT_OR(score) OVER w AS o,BIT_XOR(score) OVER w AS x "
        "FROM items WINDOW w AS(ORDER BY id ROWS 1 PRECEDING) ORDER BY id"};
    for(size_t query=0;query<sizeof(sql)/sizeof(sql[0]);++query) {
      reset(); parse(sql[query]);
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); reserve_calls=resize_calls=sort_calls=0;
      check_equal(next().state,ORM_SQL_SCAN_ROW); const size_t counts[]={reserve_calls,resize_calls,sort_calls};
      for(size_t pass=0;pass<sizeof(counts)/sizeof(counts[0]);++pass) {
        for(size_t point=1;point<=counts[pass];++point) {
          reset(); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); reserve_calls=resize_calls=sort_calls=0;
          if(pass==0) fail_reserve=point; else if(pass==1) fail_resize=point; else fail_sort_at=point;
          orm_sql_scan_row output={.state=ORM_SQL_SCAN_CANCELLED};
          check_equal(orm_tidesdb_sql_scan_next(&run.scan,&output,&error),TURBODB_STATUS_OUT_OF_MEMORY);
          check_equal(output.state,ORM_SQL_SCAN_CANCELLED);
          const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
          check_equal(orm_tidesdb_sql_scan_next(&run.scan,&output,&error),TURBODB_STATUS_OUT_OF_MEMORY);
          check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
          check_equal(output.state,ORM_SQL_SCAN_CANCELLED);
        }
      }
      reset(); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().state,ORM_SQL_SCAN_ROW);
    }
  }
  it("refunds every first-result window execution step failure and permits a fresh run") {
    parse("SELECT id,ROW_NUMBER() OVER(ORDER BY score)+RANK() OVER(ORDER BY id DESC)+"
        "COUNT(*) OVER(ORDER BY id ROWS 1 PRECEDING)+COUNT(score) OVER(ORDER BY id)+"
        "MIN(id) OVER(ORDER BY id)+MAX(id) OVER(ORDER BY id) AS n,"
        "SUM(1.0) OVER(ORDER BY id) AS total,AVG(2.0) OVER(ORDER BY id ROWS 1 PRECEDING) AS mean,"
        "VAR_POP(score) OVER(ORDER BY id) AS vp,VAR_SAMP(score) OVER(ORDER BY id) AS vs,"
        "STDDEV_POP(score) OVER(ORDER BY id ROWS 1 PRECEDING) AS dp,STDDEV_SAMP(score) OVER(ORDER BY id) AS ds,"
        "BIT_AND(id) OVER(ORDER BY id) AS ba,BIT_OR(score) OVER(ORDER BY id ROWS 1 PRECEDING) AS bo,"
        "BIT_XOR(id) OVER(ORDER BY id RANGE 1 PRECEDING) AS bx FROM items ORDER BY id");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]; check_equal(next().state,ORM_SQL_SCAN_ROW);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    for(uint64_t point=0;point<steps;++point) {
      reset(); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      orm_sql_scan_row output={.state=ORM_SQL_SCAN_CANCELLED};
      check_equal(orm_tidesdb_sql_scan_next(&run.scan,&output,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(output.state,ORM_SQL_SCAN_CANCELLED);
      const uint64_t used=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(orm_tidesdb_sql_scan_next(&run.scan,&output,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],used);
      check_equal(orm_tidesdb_sql_select_destroy(&plan,&error),TURBODB_STATUS_BUSY);
    }
    reset(); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().state,ORM_SQL_SCAN_ROW);
  }
  it("cleans every selected binding reserve and resize failure then allows retry") {
    parse("SELECT t.name AS label, t.id FROM items t WHERE t.score > 0 AND id < 5 LIMIT 2");
    reserve_calls = resize_calls = 0; check_equal(bind_select(), TURBODB_STATUS_OK);
    const size_t reserves = reserve_calls, resizes = resize_calls;
    for (size_t i = 1; i <= reserves + resizes; ++i) {
      reset(); reserve_calls = resize_calls = 0;
      if (i <= reserves) fail_reserve = i; else fail_resize = i - reserves;
      check_equal(bind_select(), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(plan.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
    }
    reset(); check_equal(bind_select(), TURBODB_STATUS_OK); open_rows(); check_equal(next().state, ORM_SQL_SCAN_ROW);
  }

  it("restores the bound plan waterline on every selected run-open allocation failure") {
    parse("SELECT id FROM items WHERE score > 0"); check_equal(bind_select(), TURBODB_STATUS_OK);
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    reserve_calls = resize_calls = 0; open_rows();
    const size_t reserves = reserve_calls, resizes = resize_calls;
    check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
    for (size_t i = 1; i <= reserves + resizes; ++i) {
      reserve_calls = resize_calls = fail_reserve = fail_resize = 0;
      if (i <= reserves) fail_reserve = i; else fail_resize = i - reserves;
      check_equal(orm_tidesdb_sql_select_open(&plan, &rows[0][0], TEST_ROWS, &run, &error), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(run.program); check_equal(plan.active_runs, 0u); check_equal(plan.filter.active_runs, 0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
    }
    fail_reserve = fail_resize = 0; open_rows();
    reserve_calls = resize_calls = 0; fail_reserve = fail_resize = 1;
    check_equal(next().values[0].data.int64_value, 2); check_equal(next().values[0].data.int64_value, 3);
    check_equal(reserve_calls, 0u); check_equal(resize_calls, 0u);
  }

  it("rejects wrong dialect occupied outputs invalid sources and inactive budgets") {
    parse("SELECT id FROM items"); check_equal(bind_select(), TURBODB_STATUS_OK);
    check_equal(bind_select(), TURBODB_STATUS_INVALID_ARGUMENT);
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_tidesdb_sql_select_open(&plan, NULL, 1, &run, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
    open_rows(); check_equal(orm_tidesdb_sql_select_open(&plan, NULL, 0, &run, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    reset(); sqlparser_document_destroy(document); document = NULL;
    const char sql[] = "SELECT id FROM items"; sqlparser_error parse_error;
    check_equal(sqlparser_parse_dialect(sql, sizeof(sql) - 1, SQLPARSER_SQLITE, NULL, &document, &parse_error), SQLPARSER_OK);
    check_equal(bind_select(), TURBODB_STATUS_UNSUPPORTED);
    parse(sql); check_equal(orm_tidesdb_sql_budget_end(&budget, &error), TURBODB_STATUS_OK);
    check_equal(bind_select(), TURBODB_STATUS_INVALID_STATE);
    check_equal(orm_tidesdb_sql_budget_begin(&budget, &error), TURBODB_STATUS_OK);
  }

  it("binds WHERE and both LIMIT forms by marker source order") {
    const char *sql[] = {"SELECT id FROM items WHERE score >= ? AND id < ? LIMIT ?, ?",
                        "SELECT id FROM items WHERE score >= ? AND id < ? LIMIT ? OFFSET ?"};
    orm_sql_type types[] = {{TURBODB_VALUE_INT64, false}, {TURBODB_VALUE_INT64, false},
                           {TURBODB_VALUE_UINT64, false}, {TURBODB_VALUE_UINT64, false}};
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
      reset(); parse(sql[i]); check_equal(bind_with(types, 4), TURBODB_STATUS_OK);
      turbodb_value_t values[] = {turbodb_i64(10), turbodb_i64(5), turbodb_u64(i ? 2 : 1), turbodb_u64(i ? 1 : 2)};
      sqlparser_document_destroy(document); document = NULL;
      check_equal(open_with(values, 4), TURBODB_STATUS_OK);
      memset(values, 0, sizeof(values));
      check_equal(next().values[0].data.int64_value, 3);
      check_equal(next().values[0].data.int64_value, 4);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 4u);
    }
  }

  it("owns separate TEXT snapshots for simultaneous runs and permits plan reuse") {
    parse("SELECT id FROM items WHERE name = ?");
    orm_sql_type type = {TURBODB_VALUE_TEXT, false}; check_equal(bind_with(&type, 1), TURBODB_STATUS_OK);
    type.kind = TURBODB_VALUE_BLOB;
    char text[] = "b"; turbodb_value_t value = turbodb_text(text);
    check_equal(open_with(&value, 1), TURBODB_STATUS_OK);
    text[0] = 'd';
    orm_sql_select_run second = {0};
    check_equal(orm_tidesdb_sql_select_open_parameters(&plan, &rows[0][0], TEST_ROWS, &value, 1, &second, &error), TURBODB_STATUS_OK);
    text[0] = 'z'; value = turbodb_null();
    check_equal(next().values[0].data.int64_value, 2); check_equal(next().state, ORM_SQL_SCAN_DONE);
    orm_sql_scan_row row = {0};
    check_equal(orm_tidesdb_sql_scan_next(&second.scan, &row, &error), TURBODB_STATUS_OK);
    check_equal(row.values[0].data.int64_value, 4);
    check_equal(orm_tidesdb_sql_select_close(&second, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
    value = turbodb_text("e"); check_equal(open_with(&value, 1), TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value, 5);
  }

  it("copies binary and embedded-NUL parameters without retaining caller bytes") {
    parse("SELECT id FROM items WHERE ? <=> ? LIMIT 1");
    const turbodb_value_kind_t kinds[] = {TURBODB_VALUE_TEXT, TURBODB_VALUE_BLOB};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
      reset(); orm_sql_type types[] = {{kinds[i], false}, {kinds[i], false}};
      check_equal(bind_with(types, 2), TURBODB_STATUS_OK);
      char left[] = {'a', '\0', 'b'}, right[] = {'a', '\0', 'b'};
      turbodb_value_t values[] = {i ? turbodb_blob(left, sizeof(left)) : turbodb_text_v((vstr){left, sizeof(left)}),
                             i ? turbodb_blob(right, sizeof(right)) : turbodb_text_v((vstr){right, sizeof(right)})};
      check_equal(open_with(values, 2), TURBODB_STATUS_OK);
      memset(left, 'x', sizeof(left)); memset(right, 'y', sizeof(right)); memset(values, 0, sizeof(values));
      reserve_calls = resize_calls = 0; fail_reserve = fail_resize = 1;
      check_equal(next().values[0].data.int64_value, 1); check_equal(next().state, ORM_SQL_SCAN_DONE);
      check_equal(reserve_calls, 0u); check_equal(resize_calls, 0u);
    }
  }

  it("checks exact parameter counts and rejects implicit type conversions") {
    parse("SELECT id FROM items WHERE id = ?");
    orm_sql_type types[] = {{TURBODB_VALUE_INT64, true}, {TURBODB_VALUE_INT64, true}};
    check_equal(bind_select(), TURBODB_STATUS_SQL_ERROR);
    check_equal(bind_with(types, 2), TURBODB_STATUS_SQL_ERROR);
    check_equal(bind_with(NULL, 1), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(bind_with(types, 1), TURBODB_STATUS_OK);
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    turbodb_value_t values[] = {turbodb_text("1"), turbodb_i64(1)};
    check_equal(open_with(NULL, 0), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(open_with(values, 2), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(open_with(NULL, 1), TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(open_with(values, 1), TURBODB_STATUS_TYPE_ERROR);
    check_contains(error.message, "parameter 1");
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
    check_equal(plan.active_runs, 0u); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
    values[0] = turbodb_null(); check_equal(open_with(values, 1), TURBODB_STATUS_OK); check_equal(next().state, ORM_SQL_SCAN_DONE);
    reset(); types[0] = (orm_sql_type){TURBODB_VALUE_TEXT, false};
    check_equal(bind_with(types, 1), TURBODB_STATUS_UNSUPPORTED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    parse("SELECT ? FROM items"); check_equal(bind_with(types, 1), TURBODB_STATUS_UNSUPPORTED);
  }

  it("validates every parameter for dead branches LIMIT zero and empty sources") {
    const char invalid_utf8[] = "\xc0\x80";
    const orm_sql_type types[] = {{TURBODB_VALUE_TEXT, false}, {TURBODB_VALUE_BOOLEAN, false},
        {TURBODB_VALUE_DOUBLE, false}, {TURBODB_VALUE_INT64, false}, {TURBODB_VALUE_BLOB, false}};
    turbodb_value_t invalid[] = {turbodb_text_v((vstr){invalid_utf8, sizeof(invalid_utf8) - 1}), turbodb_bool(1),
        turbodb_f64(NAN), turbodb_i64(1), turbodb_blob(NULL, 1)};
    invalid[1].data.boolean_value = 2; invalid[3].reserved = 1;
    const char *sql[] = {"SELECT id FROM items WHERE TRUE OR ? IS NULL",
                        "SELECT id FROM items WHERE ? IS NULL LIMIT 0"};
    for (size_t shape = 0; shape < sizeof(sql) / sizeof(sql[0]); ++shape) {
      parse(sql[shape]);
      for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        reset(); check_equal(bind_with(&types[i], 1), TURBODB_STATUS_OK);
        const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
        check_equal(orm_tidesdb_sql_select_open_parameters(&plan, NULL, 0, &invalid[i], 1, &run, &error), TURBODB_STATUS_TYPE_ERROR);
        check_null(run.program); check_equal(plan.filter.active_runs, 0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
        check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
      }
    }
  }

  it("requires nonnegative integer pagination values and preserves unsigned extremes") {
    parse("SELECT id FROM items LIMIT ? OFFSET ?");
    orm_sql_type types[] = {{TURBODB_VALUE_INT64, true}, {TURBODB_VALUE_UINT64, false}};
    check_equal(bind_with(types, 2), TURBODB_STATUS_OK);
    turbodb_value_t values[] = {turbodb_i64(-1), turbodb_u64(0)};
    const turbodb_value_t bad[] = {turbodb_i64(-1), turbodb_null(), turbodb_bool(1), turbodb_f64(1), turbodb_text("1")};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
      values[0] = bad[i]; check_equal(open_with(values, 2), TURBODB_STATUS_TYPE_ERROR);
      check_contains(error.message, "at byte"); check_null(run.program);
    }
    values[0] = turbodb_i64(0); values[1] = turbodb_i64(0);
    check_equal(open_with(values, 2), TURBODB_STATUS_TYPE_ERROR);
    values[1] = turbodb_u64(UINT64_MAX); check_equal(open_with(values, 2), TURBODB_STATUS_OK);
    check_equal(next().state, ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
    reset(); types[0] = (orm_sql_type){TURBODB_VALUE_UINT64, false}; check_equal(bind_with(types, 2), TURBODB_STATUS_OK);
    values[0] = values[1] = turbodb_u64(UINT64_MAX); check_equal(open_with(values, 2), TURBODB_STATUS_OK);
    check_equal(next().state, ORM_SQL_SCAN_DONE);
    reset(); types[0] = (orm_sql_type){TURBODB_VALUE_TEXT, false}; check_equal(bind_with(types, 2), TURBODB_STATUS_TYPE_ERROR);
    reset(); parse("SELECT id FROM items LIMIT ? + 1"); check_equal(bind_with(types, 1), TURBODB_STATUS_UNSUPPORTED);
  }

  it("checks parameter byte overflow work admission and validation step exhaustion before reading") {
    parse("SELECT id FROM items WHERE ? = ?");
    const orm_sql_type types[] = {{TURBODB_VALUE_BLOB, false}, {TURBODB_VALUE_BLOB, false}};
    check_equal(bind_with(types, 2), TURBODB_STATUS_OK);
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    turbodb_value_t values[] = {turbodb_blob("x", SIZE_MAX), turbodb_blob("x", 1)};
    check_equal(open_with(values, 2), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_contains(error.message, "overflow");
    values[1] = turbodb_blob(NULL, 0); check_equal(open_with(values, 2), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
    values[0] = turbodb_blob(NULL, 0);
    orm_sql_budget_amount full = {0};
    full.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] -
        budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_budget_reserve(&budget, &full, &error), TURBODB_STATUS_OK);
    check_equal(open_with(values, 2), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u); check_null(run.program);
  }

  it("cleans parameter ordering and every selected bind allocation failure") {
    parse("SELECT id FROM items WHERE name = ? LIMIT ? OFFSET ?");
    const orm_sql_type types[] = {{TURBODB_VALUE_TEXT, false}, {TURBODB_VALUE_UINT64, false}, {TURBODB_VALUE_UINT64, false}};
    reserve_calls = resize_calls = 0; check_equal(bind_with(types, 3), TURBODB_STATUS_OK);
    const size_t reserves = reserve_calls, resizes = resize_calls;
    for (size_t i = 1; i <= reserves + resizes; ++i) {
      reset(); reserve_calls = resize_calls = 0;
      if (i <= reserves) fail_reserve = i; else fail_resize = i - reserves;
      check_equal(bind_with(types, 3), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(plan.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    reset(); fail_sort = true; check_equal(bind_with(types, 3), TURBODB_STATUS_OUT_OF_MEMORY);
    check_null(plan.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    reset(); check_equal(bind_with(types, 3), TURBODB_STATUS_OK);
  }

  it("cleans every selected parameter snapshot allocation failure then executes without growth") {
    parse("SELECT id FROM items WHERE name = ? LIMIT ?");
    const orm_sql_type types[] = {{TURBODB_VALUE_TEXT, false}, {TURBODB_VALUE_INT64, false}};
    const turbodb_value_t values[] = {turbodb_text("b"), turbodb_i64(1)};
    check_equal(bind_with(types, 2), TURBODB_STATUS_OK);
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    reserve_calls = resize_calls = 0; check_equal(open_with(values, 2), TURBODB_STATUS_OK);
    const size_t reserves = reserve_calls, resizes = resize_calls;
    check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
    for (size_t i = 1; i <= reserves + resizes; ++i) {
      reserve_calls = resize_calls = fail_reserve = fail_resize = 0;
      if (i <= reserves) fail_reserve = i; else fail_resize = i - reserves;
      check_equal(open_with(values, 2), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(run.program); check_equal(plan.active_runs, 0u); check_equal(plan.filter.active_runs, 0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
    }
    fail_reserve = fail_resize = 0; check_equal(open_with(values, 2), TURBODB_STATUS_OK);
    reserve_calls = resize_calls = 0; fail_reserve = fail_resize = 1;
    check_equal(next().values[0].data.int64_value, 2); check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(reserve_calls, 0u); check_equal(resize_calls, 0u);
  }

  it("executes TEXT literal and parameter predicates after releasing the source document") {
    parse("SELECT id FROM items WHERE name = 'a''b' OR name = ? LIMIT 2");
    const orm_sql_type type = {TURBODB_VALUE_TEXT, false};
    check_equal(bind_with(&type, 1), TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document = NULL;
    rows[0][2] = turbodb_text("a'b");
    const turbodb_value_t value = turbodb_text("c");
    check_equal(open_with(&value, 1), TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value, 1);
    check_equal(next().values[0].data.int64_value, 3);
    check_equal(next().state, ORM_SQL_SCAN_DONE);
  }

  it("uses the parsed lexical mode for TEXT constants in the SELECT pipeline") {
    const char sql[] = "SELECT id FROM items WHERE name = 'a\\nb' LIMIT 1";
    for (unsigned mode = 0; mode < 2; ++mode) {
      reset(); parse_mode(sql, mode != 0); check_equal(bind_select(), TURBODB_STATUS_OK);
      rows[0][2] = turbodb_text(mode ? "a\\nb" : "a\nb");
      open_rows(); check_equal(next().values[0].data.int64_value, 1);
      check_equal(next().state, ORM_SQL_SCAN_DONE);
    }
  }

  it("binds compound list and range parameters in source order through pagination") {
    parse("SELECT t.id FROM items t WHERE t.score IN (?, 30, ?) AND t.id BETWEEN ? AND ? LIMIT ? OFFSET ?");
    enum { PARAMS = 6 };
    orm_sql_type types[PARAMS];
    for (size_t i = 0; i < PARAMS; ++i) types[i] = (orm_sql_type){TURBODB_VALUE_INT64, true};
    check_equal(bind_with(types, PARAMS), TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document = NULL;
    turbodb_value_t values[] = {turbodb_i64(20), turbodb_null(), turbodb_i64(2), turbodb_i64(4), turbodb_i64(1), turbodb_i64(1)};
    check_equal(open_with(values, PARAMS), TURBODB_STATUS_OK);
    values[0] = turbodb_i64(40);
    reserve_calls = resize_calls = 0; fail_reserve = fail_resize = 1;
    check_equal(next().values[0].data.int64_value, 4); check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(reserve_calls, 0u); check_equal(resize_calls, 0u);
  }

  it("filters UNKNOWN from NOT IN and combines TEXT ranges with lists") {
    parse("SELECT id FROM items WHERE score NOT IN (20, NULL)");
    check_equal(bind_select(), TURBODB_STATUS_OK); open_rows(); check_equal(next().state, ORM_SQL_SCAN_DONE);
    reset(); parse("SELECT id FROM items WHERE name BETWEEN 'b' AND 'd' AND name NOT IN ('c', 'C')");
    check_equal(bind_select(), TURBODB_STATUS_OK); open_rows();
    check_equal(next().values[0].data.int64_value, 2);
    check_equal(next().values[0].data.int64_value, 4); check_equal(next().state, ORM_SQL_SCAN_DONE);
    reject("SELECT id FROM items WHERE id IN (1, missing) LIMIT 0", TURBODB_STATUS_SQL_ERROR, "column");
    reject("SELECT id FROM items WHERE id BETWEEN 6 AND missing LIMIT 0", TURBODB_STATUS_SQL_ERROR, "column");
    reject("SELECT id FROM items WHERE id IN (SELECT 1)", TURBODB_STATUS_UNSUPPORTED, "IN");
  }

  it("cleans every selected compound binding allocation failure") {
    parse("SELECT id FROM items WHERE name IN ('a', ?) AND id NOT BETWEEN ? AND 5");
    const orm_sql_type types[] = {{TURBODB_VALUE_TEXT, false}, {TURBODB_VALUE_INT64, false}};
    reserve_calls = resize_calls = 0; check_equal(bind_with(types, 2), TURBODB_STATUS_OK);
    const size_t reserves = reserve_calls, resizes = resize_calls;
    for (size_t i = 1; i <= reserves + resizes; ++i) {
      reset(); reserve_calls = resize_calls = 0;
      if (i <= reserves) fail_reserve = i; else fail_resize = i - reserves;
      check_equal(bind_with(types, 2), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(plan.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    reset(); check_equal(bind_with(types, 2), TURBODB_STATUS_OK);
    const turbodb_value_t values[] = {turbodb_text("b"), turbodb_i64(3)};
    check_equal(open_with(values, 2), TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value, 1);
    check_equal(next().values[0].data.int64_value, 2); check_equal(next().state, ORM_SQL_SCAN_DONE);
  }

  it("binds CASE conditions results and pagination parameters in source order") {
    parse("SELECT t.id FROM items t WHERE (CASE WHEN t.score IS NULL THEN ? "
          "WHEN t.name = ? THEN ? ELSE ? END) BETWEEN ? AND ? LIMIT ? OFFSET ?");
    enum { PARAMS = 8 };
    orm_sql_type types[PARAMS];
    for (size_t i = 0; i < PARAMS; ++i) types[i] = (orm_sql_type){TURBODB_VALUE_INT64, false};
    types[1].kind = TURBODB_VALUE_TEXT;
    check_equal(bind_with(types, PARAMS), TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document = NULL;
    turbodb_value_t values[] = {turbodb_i64(10), turbodb_text("b"), turbodb_i64(20), turbodb_i64(30),
      turbodb_i64(10), turbodb_i64(20), turbodb_i64(1), turbodb_i64(1)};
    check_equal(open_with(values, PARAMS), TURBODB_STATUS_OK);
    values[1] = turbodb_text("c"); values[2] = turbodb_i64(40);
    reserve_calls = resize_calls = 0; fail_reserve = fail_resize = 1;
    check_equal(next().values[0].data.int64_value, 2); check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(reserve_calls, 0u); check_equal(resize_calls, 0u);
  }

  it("filters missing ELSE UNKNOWN and does not match NULL in simple CASE") {
    parse("SELECT id FROM items WHERE CASE score WHEN NULL THEN TRUE WHEN 20 THEN TRUE END");
    check_equal(bind_select(), TURBODB_STATUS_OK); open_rows();
    check_equal(next().values[0].data.int64_value, 2); check_equal(next().state, ORM_SQL_SCAN_DONE);
    reset(); parse("SELECT id FROM items WHERE CASE name WHEN 'a' THEN FALSE WHEN 'c' THEN TRUE ELSE FALSE END");
    check_equal(bind_select(), TURBODB_STATUS_OK); open_rows();
    check_equal(next().values[0].data.int64_value, 3); check_equal(next().state, ORM_SQL_SCAN_DONE);
  }

  it("resolves unreachable CASE branches and enforces types before LIMIT zero") {
    reject("SELECT id FROM items WHERE CASE WHEN TRUE THEN TRUE ELSE missing END LIMIT 0", TURBODB_STATUS_SQL_ERROR, "column");
    reject("SELECT id FROM items WHERE CASE id WHEN 1 THEN TRUE WHEN missing THEN FALSE END LIMIT 0", TURBODB_STATUS_SQL_ERROR, "column");
    reject("SELECT id FROM items WHERE CASE WHEN id THEN TRUE ELSE FALSE END LIMIT 0", TURBODB_STATUS_UNSUPPORTED, "BOOL");
    reject("SELECT id FROM items WHERE (CASE WHEN TRUE THEN 1 ELSE name END) = 1 LIMIT 0", TURBODB_STATUS_UNSUPPORTED, "result conversion");
  }

  it("validates unselected CASE parameters even for empty input and LIMIT zero") {
    parse("SELECT id FROM items WHERE (CASE WHEN TRUE THEN 'ok' ELSE ? END) = 'ok' LIMIT 0");
    const orm_sql_type type = {TURBODB_VALUE_TEXT, false};
    check_equal(bind_with(&type, 1), TURBODB_STATUS_OK);
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const turbodb_value_t invalid = turbodb_text_v((vstr){"\xc0\x80", 2});
    check_equal(orm_tidesdb_sql_select_open_parameters(&plan, NULL, 0, &invalid, 1, &run, &error), TURBODB_STATUS_TYPE_ERROR);
    check_null(run.program); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
  }

  it("cleans every selected CASE binding allocation failure before executing") {
    parse("SELECT id FROM items WHERE (CASE name WHEN ? THEN 'yes' ELSE ? END) = 'yes'");
    const orm_sql_type types[] = {{TURBODB_VALUE_TEXT, false}, {TURBODB_VALUE_TEXT, false}};
    reserve_calls = resize_calls = 0; check_equal(bind_with(types, 2), TURBODB_STATUS_OK);
    const size_t reserves = reserve_calls, resizes = resize_calls;
    for (size_t i = 1; i <= reserves + resizes; ++i) {
      reset(); reserve_calls = resize_calls = 0;
      if (i <= reserves) fail_reserve = i; else fail_resize = i - reserves;
      check_equal(bind_with(types, 2), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(plan.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    reset(); check_equal(bind_with(types, 2), TURBODB_STATUS_OK);
    const turbodb_value_t values[] = {turbodb_text("b"), turbodb_text("no")};
    check_equal(open_with(values, 2), TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value, 2); check_equal(next().state, ORM_SQL_SCAN_DONE);
  }

  it("binds conditional function arguments with columns and pagination in source order") {
    parse("SELECT t.id FROM items t WHERE COALESCE(NULLIF(t.score, ?), ?) BETWEEN ? AND ? "
          "AND IFNULL(t.name, ?) <> ? LIMIT ? OFFSET ?");
    enum { PARAMS = 8 };
    orm_sql_type types[PARAMS];
    for (size_t i = 0; i < PARAMS; ++i) types[i] = (orm_sql_type){TURBODB_VALUE_INT64, false};
    types[4].kind = types[5].kind = TURBODB_VALUE_TEXT;
    check_equal(bind_with(types, PARAMS), TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document = NULL;
    turbodb_value_t values[] = {turbodb_i64(20), turbodb_i64(15), turbodb_i64(10), turbodb_i64(20),
      turbodb_text("none"), turbodb_text("c"), turbodb_i64(1), turbodb_i64(1)};
    check_equal(open_with(values, PARAMS), TURBODB_STATUS_OK);
    values[0] = turbodb_i64(10); values[5] = turbodb_text("b");
    reserve_calls = resize_calls = 0; fail_reserve = fail_resize = 1;
    check_equal(next().values[0].data.int64_value, 2); check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(reserve_calls, 0u); check_equal(resize_calls, 0u);
  }

  it("treats function names as structural names and still resolves all argument columns") {
    parse("SELECT id FROM items WHERE coalesce(NULL, score) = 20 AND nullif(name, 'a') IS NOT NULL");
    check_equal(bind_select(), TURBODB_STATUS_OK); open_rows();
    check_equal(next().values[0].data.int64_value, 2); check_equal(next().state, ORM_SQL_SCAN_DONE);
    reject("SELECT id FROM items WHERE COALESCE(TRUE, missing) LIMIT 0", TURBODB_STATUS_SQL_ERROR, "column");
    reject("SELECT id FROM items WHERE IFNULL(TRUE, 1) LIMIT 0", TURBODB_STATUS_UNSUPPORTED, "conversion");
    reject("SELECT id FROM items WHERE UNSUPPORTED_FN(missing) = 1", TURBODB_STATUS_UNSUPPORTED, "function");
    reject("SELECT id FROM items WHERE COALESCE()", TURBODB_STATUS_SQL_ERROR, "argument count");
    reject("SELECT id FROM items WHERE NULLIF(TRUE)", TURBODB_STATUS_SQL_ERROR, "argument count");
    reject("SELECT id FROM items WHERE COALESCE(DISTINCT TRUE)", TURBODB_STATUS_UNSUPPORTED, "DISTINCT");
  }

  it("validates skipped conditional function parameters at open even with LIMIT zero") {
    parse("SELECT id FROM items WHERE IFNULL('ok', ?) = 'ok' LIMIT 0");
    const orm_sql_type type = {TURBODB_VALUE_TEXT, false}; check_equal(bind_with(&type, 1), TURBODB_STATUS_OK);
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const turbodb_value_t invalid = turbodb_text_v((vstr){"\xc0\x80", 2});
    check_equal(orm_tidesdb_sql_select_open_parameters(&plan, NULL, 0, &invalid, 1, &run, &error), TURBODB_STATUS_TYPE_ERROR);
    check_null(run.program); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
  }

  it("cleans conditional function binding failures before reusing the plan") {
    parse("SELECT id FROM items WHERE COALESCE(NULLIF(name, ?), ?) = 'b'");
    const orm_sql_type types[] = {{TURBODB_VALUE_TEXT, false}, {TURBODB_VALUE_TEXT, false}};
    reserve_calls = resize_calls = 0; check_equal(bind_with(types, 2), TURBODB_STATUS_OK);
    const size_t reserves = reserve_calls, resizes = resize_calls;
    for (size_t i = 1; i <= reserves + resizes; ++i) {
      reset(); reserve_calls = resize_calls = 0;
      if (i <= reserves) fail_reserve = i; else fail_resize = i - reserves;
      check_equal(bind_with(types, 2), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(plan.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    reset(); check_equal(bind_with(types, 2), TURBODB_STATUS_OK);
    const turbodb_value_t values[] = {turbodb_text("a"), turbodb_text("b")}; check_equal(open_with(values, 2), TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value, 1);
    check_equal(next().values[0].data.int64_value, 2); check_equal(next().state, ORM_SQL_SCAN_DONE);
  }

  it("executes typed arithmetic columns and parameters through filtering and pagination") {
    parse("SELECT t.id FROM items t WHERE (t.score + ?) * ? >= ? AND -(t.id - ?) <= ? LIMIT ? OFFSET ?");
    enum { PARAMS = 7 };
    orm_sql_type types[PARAMS];
    for (size_t i = 0; i < PARAMS; ++i) types[i] = (orm_sql_type){TURBODB_VALUE_INT64, false};
    check_equal(bind_with(types, PARAMS), TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document = NULL;
    turbodb_value_t values[] = {turbodb_i64(1), turbodb_i64(2), turbodb_i64(40), turbodb_i64(5), turbodb_i64(3), turbodb_i64(1), turbodb_i64(1)};
    check_equal(open_with(values, PARAMS), TURBODB_STATUS_OK); values[0] = turbodb_i64(INT64_MAX);
    reserve_calls = resize_calls = 0; fail_reserve = fail_resize = 1;
    check_equal(next().values[0].data.int64_value, 4); check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(reserve_calls, 0u); check_equal(resize_calls, 0u);
  }

  it("keeps arithmetic overflow terminal without consuming another row or changing output") {
    parse("SELECT id FROM items WHERE score + 1 > 0"); check_equal(bind_select(), TURBODB_STATUS_OK);
    rows[1][1] = turbodb_i64(INT64_MAX); open_rows();
    orm_sql_scan_row output = {.state = ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &output, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_contains(error.message, "arithmetic result"); check_equal(output.state, ORM_SQL_SCAN_CANCELLED);
    check_equal(run.scan.state, ORM_SQL_SCAN_ERROR);
    const uint64_t reads = budget.used.value[ORM_SQL_BUDGET_READ_ROWS], steps = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(reads, 2u);
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &output, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], reads);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], steps);
  }

  it("binds arithmetic in dead branches but evaluates overflow only when rows reach it") {
    reject("SELECT id FROM items WHERE TRUE OR name + 1 = 0 LIMIT 0", TURBODB_STATUS_UNSUPPORTED, "arithmetic");
    reject("SELECT id FROM items WHERE TRUE OR missing * 1 = 0 LIMIT 0", TURBODB_STATUS_SQL_ERROR, "column");
    reset(); parse("SELECT id FROM items WHERE CASE WHEN score IS NULL THEN FALSE ELSE score + 1 > 0 END LIMIT 0");
    check_equal(bind_select(), TURBODB_STATUS_OK); rows[0][1] = turbodb_i64(INT64_MAX); open_rows();
    check_equal(next().state, ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
    reset(); parse("SELECT id FROM items WHERE TRUE OR score * 2 > 0 LIMIT 1");
    check_equal(bind_select(), TURBODB_STATUS_OK); open_rows(); check_equal(next().values[0].data.int64_value, 1);
    check_equal(next().state, ORM_SQL_SCAN_DONE);
  }

  it("cleans arithmetic binding failures and preserves the existing plan lifecycle") {
    parse("SELECT id FROM items WHERE COALESCE(score, ?) + id * ? > ?");
    const orm_sql_type types[] = {{TURBODB_VALUE_INT64, false}, {TURBODB_VALUE_INT64, false}, {TURBODB_VALUE_INT64, false}};
    reserve_calls = resize_calls = 0; check_equal(bind_with(types, 3), TURBODB_STATUS_OK);
    const size_t reserves = reserve_calls, resizes = resize_calls;
    for (size_t i = 1; i <= reserves + resizes; ++i) {
      reset(); reserve_calls = resize_calls = 0;
      if (i <= reserves) fail_reserve = i; else fail_resize = i - reserves;
      check_equal(bind_with(types, 3), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(plan.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
    reset(); check_equal(bind_with(types, 3), TURBODB_STATUS_OK);
    const turbodb_value_t values[] = {turbodb_i64(0), turbodb_i64(2), turbodb_i64(40)}; check_equal(open_with(values, 3), TURBODB_STATUS_OK);
    check_equal(next().values[0].data.int64_value, 5); check_equal(next().state, ORM_SQL_SCAN_DONE);
  }

  it("projects scalar expressions alongside stars with owned metadata and literal bytes") {
    parse("SELECT t.*, t.score + 1 AS adjusted, COALESCE(t.score, 0) AS total, "
          "CASE WHEN t.score IS NULL THEN 'a\\0b' ELSE t.name END AS label, "
          "t.score >= 20 AS eligible, NULL AS missing FROM items t LIMIT 2");
    check_equal(bind_select(), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_column_at(&plan, 3)->type.kind, TURBODB_VALUE_INT64);
    check_equal(orm_tidesdb_sql_select_column_at(&plan, 3)->type.nullable, true);
    check_equal(orm_tidesdb_sql_select_column_at(&plan, 4)->type.nullable, false);
    check_equal(orm_tidesdb_sql_select_column_at(&plan, 5)->type.kind, TURBODB_VALUE_TEXT);
    check_equal(orm_tidesdb_sql_select_column_at(&plan, 6)->type.kind, TURBODB_VALUE_BOOLEAN);
    check_equal(orm_tidesdb_sql_select_column_at(&plan, 7)->type.kind, TURBODB_VALUE_NULL);
    check_equal(strcmp(orm_tidesdb_sql_select_column_at(&plan, 3)->name, "adjusted"), 0);
    sqlparser_document_destroy(document); document = NULL;
    memset(columns, 0, sizeof(columns)); memset(&schema, 0, sizeof(schema));
    open_rows();
    check_equal(orm_tidesdb_sql_select_destroy(&plan, &error), TURBODB_STATUS_BUSY);
    reserve_calls = resize_calls = 0; fail_reserve = fail_resize = 1;
    orm_sql_scan_row row = next();
    check_equal(row.count, 8u); check_equal(row.values[0].data.int64_value, 1);
    check_equal(row.values[3].kind, TURBODB_VALUE_NULL); check_equal(row.values[4].data.int64_value, 0);
    check_equal(row.values[5].data.text_value.len, 3u);
    check_equal(memcmp(row.values[5].data.text_value.data, "a\0b", 3), 0);
    check_equal(row.values[6].kind, TURBODB_VALUE_NULL); check_equal(row.values[7].kind, TURBODB_VALUE_NULL);
    row = next(); check_equal(row.values[3].data.int64_value, 21); check_equal(row.values[4].data.int64_value, 20);
    check_equal(row.values[5].data.text_value.data, rows[1][2].data.text_value.data);
    check_equal(row.values[6].data.boolean_value, true);
    check_equal(next().state, ORM_SQL_SCAN_DONE); check_equal(reserve_calls, 0u); check_equal(resize_calls, 0u);
  }

  it("orders projection filter and pagination parameters and snapshots their payloads") {
    parse("SELECT score + ? AS adjusted, IFNULL(?, name) AS label, ? AS payload "
          "FROM items WHERE id >= ? LIMIT ? OFFSET ?");
    const orm_sql_type types[] = {{TURBODB_VALUE_INT64, false}, {TURBODB_VALUE_TEXT, true}, {TURBODB_VALUE_BLOB, false},
        {TURBODB_VALUE_INT64, false}, {TURBODB_VALUE_UINT64, false}, {TURBODB_VALUE_INT64, false}};
    check_equal(bind_with(types, 6), TURBODB_STATUS_OK);
    char text[] = {'x', 0, 'y'}; unsigned char blob[] = {0, 255, 7};
    turbodb_value_t values[] = {turbodb_i64(2), turbodb_text_v((vstr){text, sizeof(text)}), turbodb_blob(blob, sizeof(blob)),
        turbodb_i64(2), turbodb_u64(1), turbodb_i64(1)};
    check_equal(open_with(values, 6), TURBODB_STATUS_OK);
    memset(text, 0, sizeof(text)); memset(blob, 0, sizeof(blob)); values[0] = turbodb_i64(INT64_MAX);
    orm_sql_scan_row row = next(); check_equal(row.values[0].data.int64_value, 12);
    check_equal(memcmp(row.values[1].data.text_value.data, "x\0y", 3), 0);
    const unsigned char expected[] = {0, 255, 7};
    check_equal(row.values[2].kind, TURBODB_VALUE_BLOB);
    check_equal(memcmp(row.values[2].data.blob_value.data, expected, sizeof(expected)), 0);
    check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 3u);
  }

  it("does not evaluate projections for filtered offset or out of limit rows") {
    parse("SELECT score + 1 AS adjusted FROM items WHERE id >= 2 LIMIT 1 OFFSET 1");
    rows[0][1] = rows[1][1] = rows[3][1] = turbodb_i64(INT64_MAX);
    check_equal(bind_select(), TURBODB_STATUS_OK); open_rows();
    check_equal(next().values[0].data.int64_value, 11); check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 3u);
    reset(); parse("SELECT 9223372036854775807 + 1 AS overflowed FROM items LIMIT 0");
    check_equal(bind_select(), TURBODB_STATUS_OK); open_rows(); check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
    reset(); parse("SELECT CASE WHEN TRUE THEN 1 ELSE 9223372036854775807 + 1 END AS safe FROM items LIMIT 1");
    check_equal(bind_select(), TURBODB_STATUS_OK); open_rows(); check_equal(next().values[0].data.int64_value, 1);
  }

  it("discards all computed outputs when a later projection fails and locks the first error") {
    parse("SELECT id, IFNULL(name, 'x') AS label, score + 1 AS adjusted FROM items");
    rows[1][1] = turbodb_i64(INT64_MAX); check_equal(bind_select(), TURBODB_STATUS_OK); open_rows();
    orm_sql_scan_row row = next(); const turbodb_value_t *borrowed = row.values;
    check_equal(row.values[2].kind, TURBODB_VALUE_NULL);
    orm_sql_scan_row sentinel = {.state = ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &sentinel, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(sentinel.state, ORM_SQL_SCAN_CANCELLED); check_equal(borrowed[0].kind, TURBODB_VALUE_NULL);
    check_equal(borrowed[1].kind, TURBODB_VALUE_NULL); check_equal(borrowed[2].kind, TURBODB_VALUE_NULL);
    check_equal(run.scan.state, ORM_SQL_SCAN_ERROR); check_contains(error.message, "row 1");
    const uint64_t steps = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &sentinel, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], steps);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 2u);
    for (size_t i = 0; i < vec_size(&run.scan.projection); ++i) {
      const scan_projection *p = vec_at_const(&run.scan.projection, i);
      for (size_t j = 0; j < vec_size(&p->inputs); ++j)
        check_equal(((const turbodb_value_t *)vec_at_const(&p->inputs, j))->kind, TURBODB_VALUE_NULL);
    }
  }

  it("validates every computed expression and keeps aliases outside source scope") {
    reject("SELECT name + 1 AS bad FROM items LIMIT 0", TURBODB_STATUS_UNSUPPORTED, "arithmetic");
    reject("SELECT CASE WHEN TRUE THEN 1 ELSE missing END AS bad FROM items LIMIT 0", TURBODB_STATUS_SQL_ERROR, "column");
    reject("SELECT SUM(id) AS n FROM items LIMIT 0", TURBODB_STATUS_UNSUPPORTED, "DECIMAL");
    reject("SELECT id + 1 AS n, n + 1 AS other FROM items", TURBODB_STATUS_SQL_ERROR, "column");
    reject("SELECT id + 1 AS n FROM items WHERE n > 0", TURBODB_STATUS_SQL_ERROR, "column");
    reject("SELECT id + 1 AS id, id FROM items", TURBODB_STATUS_SQL_ERROR, "duplicate output");
    reject("SELECT t.*, 1 AS score FROM items t", TURBODB_STATUS_SQL_ERROR, "duplicate output");
    reject("SELECT id + 1 FROM items", TURBODB_STATUS_UNSUPPORTED, "alias");
    reset(); parse("SELECT id + 1 AS score, score + 1 AS original FROM items LIMIT 1");
    check_equal(bind_select(), TURBODB_STATUS_OK); open_rows(); const orm_sql_scan_row row = next();
    check_equal(row.values[0].data.int64_value, 2); check_equal(row.values[1].kind, TURBODB_VALUE_NULL);
  }

  it("checks projection parameter values on LIMIT zero and empty sources") {
    const char *sql[] = {"SELECT ? AS value FROM items LIMIT 0", "SELECT ? AS value FROM items"};
    const orm_sql_type type = {TURBODB_VALUE_TEXT, false};
    const turbodb_value_t bad = turbodb_text_v((vstr){"\xc0\x80", 2});
    for (size_t i = 0; i < sizeof(sql) / sizeof(sql[0]); ++i) {
      reset(); parse(sql[i]); check_equal(bind_with(&type, 1), TURBODB_STATUS_OK);
      const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(orm_tidesdb_sql_select_open_parameters(&plan, NULL, 0, &bad, 1, &run, &error), TURBODB_STATUS_TYPE_ERROR);
      check_null(run.program); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
    }
  }

  it("preserves U64 F64 and BLOB computed metadata and independent run snapshots") {
    parse("SELECT ? + ? AS unsigned_sum, ? * ? AS product, NULLIF(?, ?) AS bytes FROM items LIMIT 1");
    const orm_sql_type types[] = {{TURBODB_VALUE_UINT64, false}, {TURBODB_VALUE_UINT64, false},
        {TURBODB_VALUE_DOUBLE, false}, {TURBODB_VALUE_DOUBLE, false}, {TURBODB_VALUE_BLOB, false}, {TURBODB_VALUE_BLOB, false}};
    unsigned char bytes[] = {0, 255};
    turbodb_value_t values[] = {turbodb_u64(UINT64_MAX - 1), turbodb_u64(1), turbodb_f64(0.5), turbodb_f64(2),
        turbodb_blob(bytes, sizeof(bytes)), turbodb_blob(NULL, 0)};
    check_equal(bind_with(types, 6), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_column_at(&plan, 0)->type.kind, TURBODB_VALUE_UINT64);
    check_equal(orm_tidesdb_sql_select_column_at(&plan, 1)->type.kind, TURBODB_VALUE_DOUBLE);
    check_equal(orm_tidesdb_sql_select_column_at(&plan, 2)->type.kind, TURBODB_VALUE_BLOB);
    check_equal(orm_tidesdb_sql_select_column_at(&plan, 2)->type.nullable, true);
    check_equal(open_with(values, 6), TURBODB_STATUS_OK);
    orm_sql_select_run other = {0}; values[0] = turbodb_u64(0); bytes[1] = 7;
    check_equal(orm_tidesdb_sql_select_open_parameters(&plan, &rows[0][0], TEST_ROWS,
        values, 6, &other, &error), TURBODB_STATUS_OK);
    orm_sql_scan_row row = next(); check_equal(row.values[0].data.uint64_value, UINT64_MAX);
    check_equal(row.values[1].data.double_value, 1.0);
    check_equal(((const unsigned char *)row.values[2].data.blob_value.data)[1], 255);
    check_equal(orm_tidesdb_sql_scan_next(&other.scan, &row, &error), TURBODB_STATUS_OK);
    check_equal(row.values[0].data.uint64_value, 1u);
    check_equal(((const unsigned char *)row.values[2].data.blob_value.data)[1], 7);
    check_equal(orm_tidesdb_sql_select_close(&other, &error), TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_destroy(&plan, &error), TURBODB_STATUS_BUSY);
  }

  it("enforces computed binding depth and exact work capacity with rollback") {
    parse("SELECT COALESCE(score, 0) + id AS total, IFNULL(name, 'x') AS label FROM items");
    check_equal(orm_tidesdb_sql_select_bind(document, &schema, 1, &budget, &plan, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    reset(); check_equal(bind_select(), TURBODB_STATUS_OK);
    const uint64_t peak = budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES];
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = peak - 1; reset();
    check_equal(bind_select(), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(plan.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = peak; reset();
    check_equal(bind_select(), TURBODB_STATUS_OK);
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES] = retained;
    check_equal(orm_tidesdb_sql_select_open(&plan, &rows[0][0], TEST_ROWS, &run, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(run.program); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
  }

  it("releases all partially bound computed programs on reserve and resize failures") {
    parse("SELECT id, score + ? AS adjusted, IFNULL(?, 'x') AS label FROM items WHERE id > 0");
    const orm_sql_type types[] = {{TURBODB_VALUE_INT64, false}, {TURBODB_VALUE_TEXT, true}};
    reserve_calls = resize_calls = 0; check_equal(bind_with(types, 2), TURBODB_STATUS_OK);
    const size_t reserves = reserve_calls, resizes = resize_calls;
    for (size_t i = 1; i <= reserves + resizes; ++i) {
      reset(); reserve_calls = resize_calls = 0;
      if (i <= reserves) fail_reserve = i; else fail_resize = i - reserves;
      check_equal(bind_with(types, 2), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(plan.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], 0u);
    }
  }

  it("releases all partially opened computed runs and can reopen and cancel") {
    parse("SELECT score + ? AS adjusted, IFNULL(?, 'x') AS label FROM items");
    const orm_sql_type types[] = {{TURBODB_VALUE_INT64, false}, {TURBODB_VALUE_TEXT, true}};
    const turbodb_value_t values[] = {turbodb_i64(1), turbodb_text("abc")};
    check_equal(bind_with(types, 2), TURBODB_STATUS_OK);
    const uint64_t retained = budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    reserve_calls = resize_calls = 0; check_equal(open_with(values, 2), TURBODB_STATUS_OK);
    const size_t reserves = reserve_calls, resizes = resize_calls;
    check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
    for (size_t i = 1; i <= reserves + resizes; ++i) {
      reserve_calls = resize_calls = fail_reserve = fail_resize = 0;
      if (i <= reserves) fail_reserve = i; else fail_resize = i - reserves;
      check_equal(open_with(values, 2), TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(run.program); check_equal(plan.active_runs, 0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
      for (size_t j = 0; j < vec_size(&plan.computations); ++j)
        check_equal(((const select_computation *)vec_at_const(&plan.computations, j))->program.active_runs, 0u);
    }
    fail_reserve = fail_resize = 0;
    check_equal(open_with(values, 2), TURBODB_STATUS_OK); check_equal(next().state, ORM_SQL_SCAN_ROW);
    check_equal(orm_tidesdb_sql_scan_cancel(&run.scan, &error), TURBODB_STATUS_OK);
    check_equal(next().state, ORM_SQL_SCAN_CANCELLED);
    check_equal(orm_tidesdb_sql_select_close(&run, &error), TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES], retained);
    check_equal(open_with(values, 2), TURBODB_STATUS_OK); check_equal(next().state, ORM_SQL_SCAN_ROW);
  }

  it("executes LIKE in filters and computed columns using snapshotted patterns") {
    parse("SELECT id, name LIKE ? ESCAPE '|' AS matched FROM items "
          "WHERE name NOT LIKE ? LIMIT ? OFFSET ?");
    const orm_sql_type types[] = {{TURBODB_VALUE_TEXT, false}, {TURBODB_VALUE_TEXT, false},
        {TURBODB_VALUE_INT64, false}, {TURBODB_VALUE_INT64, false}};
    check_equal(bind_with(types, 4), TURBODB_STATUS_OK);
    char pattern[] = "c";
    turbodb_value_t values[] = {turbodb_text(pattern), turbodb_text("a%"), turbodb_i64(2), turbodb_i64(1)};
    check_equal(open_with(values, 4), TURBODB_STATUS_OK); pattern[0] = 'z';
    sqlparser_document_destroy(document); document = NULL;
    reserve_calls = resize_calls = 0; fail_reserve = fail_resize = 1;
    orm_sql_scan_row row = next(); check_equal(row.values[0].data.int64_value, 3);
    check_equal(row.values[1].data.boolean_value, true);
    row = next(); check_equal(row.values[0].data.int64_value, 4); check_equal(row.values[1].data.boolean_value, false);
    check_equal(next().state, ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 4u);
    check_equal(reserve_calls, 0u); check_equal(resize_calls, 0u);
  }

  it("rejects LIKE binding errors under LIMIT zero and locks runtime pattern errors") {
    reject("SELECT id FROM items WHERE id LIKE '1%' LIMIT 0", TURBODB_STATUS_UNSUPPORTED, "TEXT");
    reject("SELECT id FROM items WHERE name LIKE '%' ESCAPE 'xx' LIMIT 0", TURBODB_STATUS_UNSUPPORTED, "ESCAPE");
    reject("SELECT name LIKE '%' ESCAPE name AS matched FROM items LIMIT 0", TURBODB_STATUS_UNSUPPORTED, "ESCAPE");
    reset(); parse("SELECT id, name LIKE '%' AS matched FROM items"); check_equal(bind_select(), TURBODB_STATUS_OK);
    rows[1][2] = turbodb_text("\xc3\xa9"); open_rows(); check_equal(next().values[1].data.boolean_value, true);
    orm_sql_scan_row out = {.state = ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &out, &error), TURBODB_STATUS_UNSUPPORTED);
    check_equal(out.state, ORM_SQL_SCAN_CANCELLED); check_contains(error.message, "ASCII");
    const uint64_t steps = budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &out, &error), TURBODB_STATUS_UNSUPPORTED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 2u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS], steps);
  }

  it("reports LIKE retry budget exhaustion without publishing a partial row") {
    parse("SELECT id, name LIKE '%aaaaab' AS matched FROM items");
    check_equal(bind_select(), TURBODB_STATUS_OK); rows[0][2] = turbodb_text("aaaaaaaaaaaa"); open_rows();
    enum { REMAINING_STEPS = 70 };
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS] =
        budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS] + REMAINING_STEPS;
    orm_sql_scan_row out = {.state = ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&run.scan, &out, &error), TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.state, ORM_SQL_SCAN_CANCELLED); check_equal(run.scan.state, ORM_SQL_SCAN_ERROR);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 1u);
    check_equal(((const turbodb_value_t *)vec_at_const(&run.scan.output, 0))->kind, TURBODB_VALUE_NULL);
  }

  it("rejects malformed literal UTF-8 and unsupported conversions before LIMIT zero") {
    reject("SELECT id FROM items WHERE TRUE OR name = '\xc0\x80' LIMIT 0", TURBODB_STATUS_TYPE_ERROR, "at byte");
    reject("SELECT id FROM items WHERE id = '1' LIMIT 0", TURBODB_STATUS_UNSUPPORTED, "conversion");
    reject("SELECT id FROM items WHERE name = X'61' LIMIT 0", TURBODB_STATUS_UNSUPPORTED, "predicate");
    reset(); parse("SELECT id FROM items WHERE name = '' LIMIT 0");
    check_equal(bind_select(), TURBODB_STATUS_OK); open_rows(); check_equal(next().state, ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS], 0u);
  }

  it("binds lag and lead values with omitted defaults and current-row explicit defaults") {
    parse("SELECT id,LAG(score) OVER(ORDER BY id) AS previous,LEAD(score,1,id+100) OVER(ORDER BY id) AS following "
        "FROM items ORDER BY id"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    const int64_t previous[]={0,0,20,10,30},following[]={20,10,30,40,105};
    for(size_t i=0;i<TEST_ROWS;++i) {
      const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
      check_equal(row.values[0].data.int64_value,(int64_t)i+1);
      if(i<2) check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
      else check_equal(row.values[1].data.int64_value,previous[i]);
      check_equal(row.values[2].data.int64_value,following[i]);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("preserves target NULL values and limits lag lookups to their partition") {
    parse("SELECT id,LAG(score,1,0) OVER(PARTITION BY id>3 ORDER BY id) AS n FROM items ORDER BY id");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); const int64_t expected[]={0,0,20,0,30};
    for(size_t i=0;i<TEST_ROWS;++i) {
      const orm_sql_scan_row row=next();
      if(i==1) check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
      else check_equal(row.values[1].data.int64_value,expected[i]);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("owns globally ordered offset and default markers after AST and input arguments die") {
    parse("SELECT ?+COALESCE(LAG(score,?,?) OVER(ORDER BY id),0) AS n FROM items WHERE id>? ORDER BY id");
    const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_UINT64,false},{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false}};
    check_equal(bind_with(types,sizeof(types)/sizeof(types[0])),TURBODB_STATUS_OK);
    turbodb_value_t parameters[]={turbodb_i64(10),turbodb_u64(2),turbodb_i64(99),turbodb_i64(0)};
    check_equal(open_with(parameters,sizeof(parameters)/sizeof(parameters[0])),TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document=NULL; memset(parameters,0,sizeof(parameters));
    const int64_t expected[]={109,109,10,30,20};
    for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[0].data.int64_value,expected[i]);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("carries typed text lag results and owns embedded-zero default marker payloads") {
    parse("SELECT id,LAG(name,1,?) OVER(ORDER BY id) AS previous,LEAD(name) OVER(ORDER BY id) AS following FROM items ORDER BY id");
    const orm_sql_type type={TURBODB_VALUE_TEXT,false}; check_equal(bind_with(&type,1),TURBODB_STATUS_OK);
    check_false(orm_tidesdb_sql_select_column_at(&plan,1)->type.nullable);
    check_true(orm_tidesdb_sql_select_column_at(&plan,2)->type.nullable);
    char payload[]={'x',0,'y'}; turbodb_value_t parameter=turbodb_text_v((vstr){payload,sizeof(payload)});
    check_equal(open_with(&parameter,1),TURBODB_STATUS_OK); memset(payload,'!',sizeof(payload));
    sqlparser_document_destroy(document); document=NULL;
    for(size_t i=0;i<TEST_ROWS;++i) {
      const orm_sql_scan_row row=next(); check_equal(row.values[1].kind,TURBODB_VALUE_TEXT);
      if(!i) { check_equal(row.values[1].data.text_value.len,3u); check_equal(memcmp(row.values[1].data.text_value.data,"x\0y",3),0); }
      else { check_equal(row.values[1].data.text_value.len,1u); check_equal(row.values[1].data.text_value.data[0],(char)('a'+i-1)); }
      if(i+1==TEST_ROWS) check_equal(row.values[2].kind,TURBODB_VALUE_NULL);
      else { check_equal(row.values[2].kind,TURBODB_VALUE_TEXT); check_equal(row.values[2].data.text_value.data[0],(char)('a'+i+1)); }
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("preserves DOUBLE BOOL U64 and BLOB offset result kinds without coercion") {
    const unsigned char bytes[]={0,255,7},fallback_bytes[]={8,0};
    const turbodb_value_t values[]={turbodb_f64(1.5),turbodb_bool(true),turbodb_u64(UINT64_MAX),turbodb_blob(bytes,sizeof(bytes))};
    const turbodb_value_t defaults[]={turbodb_f64(2.5),turbodb_bool(false),turbodb_u64(0),turbodb_blob(fallback_bytes,sizeof(fallback_bytes))};
    for(size_t kind=0;kind<sizeof(values)/sizeof(values[0]);++kind) {
      reset(); parse("SELECT LAG(?,1,?) OVER(ORDER BY id) AS n FROM items ORDER BY id");
      const orm_sql_type types[]={{values[kind].kind,false},{values[kind].kind,false}};
      check_equal(bind_with(types,sizeof(types)/sizeof(types[0])),TURBODB_STATUS_OK);
      const turbodb_value_t parameters[]={values[kind],defaults[kind]}; check_equal(open_with(parameters,2),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_select_column_at(&plan,0)->type.kind,values[kind].kind);
      check_false(orm_tidesdb_sql_select_column_at(&plan,0)->type.nullable);
      for(size_t i=0;i<TEST_ROWS;++i) {
        const turbodb_value_t result=next().values[0],expected=i?values[kind]:defaults[kind];
        check_equal(result.kind,expected.kind);
        if(kind==0) check_equal(result.data.double_value,expected.data.double_value);
        else if(kind==1) check_equal(result.data.boolean_value,expected.data.boolean_value);
        else if(kind==2) check_equal(result.data.uint64_value,expected.data.uint64_value);
        else { check_equal(result.data.blob_value.size,expected.data.blob_value.size);
          check_equal(memcmp(result.data.blob_value.data,expected.data.blob_value.data,expected.data.blob_value.size),0); }
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
  }
  it("joins a static NULL value to the typed current default while respecting in-range NULL targets") {
    parse("SELECT LAG(NULL,1,name) OVER(ORDER BY id) AS n FROM items ORDER BY id");
    check_equal(bind_select(),TURBODB_STATUS_OK); check_equal(orm_tidesdb_sql_select_column_at(&plan,0)->type.kind,TURBODB_VALUE_TEXT);
    check_true(orm_tidesdb_sql_select_column_at(&plan,0)->type.nullable); open_rows();
    const turbodb_value_t first=next().values[0]; check_equal(first.kind,TURBODB_VALUE_TEXT);
    check_equal(first.data.text_value.len,1u); check_equal(first.data.text_value.data[0],'a');
    for(size_t i=1;i<TEST_ROWS;++i) check_equal(next().values[0].kind,TURBODB_VALUE_NULL);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("binds computed group keys as lag values after HAVING filters groups") {
    parse("SELECT COALESCE(score,0) AS k,LAG(COALESCE(score,0),1,0) OVER(ORDER BY COALESCE(score,0)) AS n "
        "FROM items GROUP BY COALESCE(score,0) HAVING k>=10 ORDER BY k");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); const int64_t values[]={10,20,30,40},previous[]={0,10,20,30};
    for(size_t i=0;i<sizeof(values)/sizeof(values[0]);++i) {
      const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,values[i]);
      check_equal(row.values[1].data.int64_value,previous[i]);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("validates offset markers before LIMIT zero and accepts zero and the largest offset") {
    const char *sql[]={"SELECT LAG(score,?) OVER(ORDER BY id) AS n FROM items LIMIT 0",
                      "SELECT LEAD(score,?) OVER(ORDER BY id) AS n FROM items LIMIT 0"};
    const orm_sql_type type={TURBODB_VALUE_UINT64,true};
    for(size_t kind=0;kind<sizeof(sql)/sizeof(sql[0]);++kind) {
      reset(); parse(sql[kind]); check_equal(bind_with(&type,1),TURBODB_STATUS_OK);
      const turbodb_value_t invalid[]={turbodb_null(),turbodb_u64(ORM_SQL_WINDOW_MAX_OFFSET+1)};
      for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        check_equal(open_with(&invalid[i],1),i?TURBODB_STATUS_SQL_ERROR:TURBODB_STATUS_TYPE_ERROR); check_null(run.program);
      }
      turbodb_value_t value=turbodb_u64(ORM_SQL_WINDOW_MAX_OFFSET); check_equal(open_with(&value,1),TURBODB_STATUS_OK);
      check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
      check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK);
      value=turbodb_u64(0); check_equal(open_with(&value,1),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    reset(); parse("SELECT LAG(score,0) OVER(ORDER BY id) AS n FROM items ORDER BY id");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    for(size_t i=0;i<TEST_ROWS;++i) {
      const orm_sql_scan_row row=next(); check_equal(row.values[0].kind,rows[i][1].kind);
      if(i) check_equal(row.values[0].data.int64_value,rows[i][1].data.int64_value);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("distinguishes lag value default and direction programs in DISTINCT window equivalence") {
    parse("SELECT DISTINCT LAG(id,1,0) OVER(ORDER BY id) AS n FROM items ORDER BY LAG(id,1,0) OVER(ORDER BY id)");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[0].data.int64_value,(int64_t)i);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
    reject("SELECT DISTINCT LAG(id,1,0) OVER(ORDER BY id) AS n FROM items ORDER BY LAG(score,1,0) OVER(ORDER BY id)",
        TURBODB_STATUS_SQL_ERROR,"DISTINCT");
    reject("SELECT DISTINCT LAG(id,1,0) OVER(ORDER BY id) AS n FROM items ORDER BY LAG(id,1,99) OVER(ORDER BY id)",
        TURBODB_STATUS_SQL_ERROR,"DISTINCT");
  }
  it("rejects invalid offset arity row-dependent offsets nested windows and default coercions") {
    reject("SELECT LAG() OVER() AS n FROM items",TURBODB_STATUS_SQL_ERROR,"arguments");
    reject("SELECT LEAD(id,1,0,2) OVER() AS n FROM items",TURBODB_STATUS_SQL_ERROR,"arguments");
    reject("SELECT LAG(id,id) OVER() AS n FROM items",TURBODB_STATUS_SQL_ERROR,"offset");
    reject("SELECT LAG(id,-1) OVER() AS n FROM items",TURBODB_STATUS_SQL_ERROR,"offset");
    reject("SELECT LAG(id,9223372036854775809) OVER() AS n FROM items",TURBODB_STATUS_SQL_ERROR,"offset");
    reject("SELECT LAG(ROW_NUMBER() OVER()) OVER() AS n FROM items",TURBODB_STATUS_SQL_ERROR,"nested");
    reject("SELECT LAG(id,1,'x') OVER() AS n FROM items",TURBODB_STATUS_UNSUPPORTED,"same type");
    reject("SELECT LEAD(name,1,0) OVER() AS n FROM items",TURBODB_STATUS_UNSUPPORTED,"same type");
  }
  it("executes six ranking windows before final ordering and pagination") {
    parse("SELECT id, ROW_NUMBER() OVER (ORDER BY score) AS rn, RANK() OVER (ORDER BY score) AS r, "
          "DENSE_RANK() OVER (ORDER BY score) AS d, PERCENT_RANK() OVER (ORDER BY score) AS p, "
          "CUME_DIST() OVER (ORDER BY score) AS c, NTILE(2) OVER (ORDER BY score) AS b "
          "FROM items ORDER BY id LIMIT 3 OFFSET 1");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    const int64_t ranks[]={3,2,4}, buckets[]={1,1,2}; const double percentages[]={.5,.25,.75}, cumulative[]={.6,.4,.8};
    for(size_t i=0;i<3;++i) {
      const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.count,7u);
      check_equal(row.values[0].data.int64_value,(int64_t)i+2);
      for(size_t j=1;j<=3;++j) check_equal(row.values[j].data.int64_value,ranks[i]);
      check_equal(row.values[4].data.double_value,percentages[i]); check_equal(row.values[5].data.double_value,cumulative[i]);
      check_equal(row.values[6].data.int64_value,buckets[i]);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("expands stars without exposing hidden keys or window result slots") {
    parse("SELECT items.*, ROW_NUMBER() OVER (ORDER BY id DESC) AS n FROM items ORDER BY id");
    check_equal(bind_select(),TURBODB_STATUS_OK); check_equal(vec_size(&plan.columns),4u); open_rows();
    for(size_t i=0;i<TEST_ROWS;++i) {
      const orm_sql_scan_row row=next(); check_equal(row.count,4u);
      check_equal(row.values[0].data.int64_value,(int64_t)i+1); check_equal(row.values[3].data.int64_value,(int64_t)(TEST_ROWS-i));
      check_equal(row.values[2].data.text_value.data[0],(char)('a'+i));
    }
  }
  it("evaluates scalar window keys after WHERE and permits scalar arithmetic on results") {
    parse("SELECT id,ROW_NUMBER() OVER (PARTITION BY score>20 ORDER BY id)+10 AS n FROM items WHERE id>1 ORDER BY id");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); const int64_t expected[]={11,12,11,12};
    for(size_t i=0;i<4;++i) { const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,(int64_t)i+2); check_equal(row.values[1].data.int64_value,expected[i]); }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("evaluates windows after HAVING over grouped aggregate values") {
    parse("SELECT score,COUNT(*) AS n,RANK() OVER (ORDER BY COUNT(*) DESC,score DESC) AS r "
          "FROM items GROUP BY score HAVING score>=20 ORDER BY score");
    rows[2][1]=turbodb_i64(20); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    const int64_t scores[]={20,30,40}, counts[]={2,1,1}, ranks[]={1,3,2};
    for(size_t i=0;i<3;++i) {
      const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,scores[i]);
      check_equal(row.values[1].data.int64_value,counts[i]); check_equal(row.values[2].data.int64_value,ranks[i]);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("binds expression group keys inside window partition and order expressions") {
    parse("SELECT COALESCE(score,0) AS k,ROW_NUMBER() OVER (ORDER BY COALESCE(score,0) DESC) AS n "
          "FROM items GROUP BY COALESCE(score,0) ORDER BY k");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); const int64_t keys[]={0,10,20,30,40};
    for(size_t i=0;i<TEST_ROWS;++i) { const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,keys[i]); check_equal(row.values[1].data.int64_value,(int64_t)(TEST_ROWS-i)); }
  }
  it("uses windows found only in final ORDER BY") {
    parse("SELECT id FROM items ORDER BY RANK() OVER (ORDER BY score) DESC");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); const int64_t expected[]={5,4,2,3,1};
    for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[0].data.int64_value,expected[i]);
  }
  it("canonicalizes equal window expressions for DISTINCT") {
    parse("SELECT DISTINCT RANK() OVER (ORDER BY score) AS n FROM items ORDER BY RANK() OVER (ORDER BY score)");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[0].data.int64_value,(int64_t)i+1);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("copies marker values and preserves complete-document parameter ordering") {
    parse("SELECT ROW_NUMBER() OVER (ORDER BY id)+? AS n,NTILE(?) OVER (ORDER BY score) AS b "
          "FROM items WHERE id>? ORDER BY n LIMIT ?");
    const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false}};
    check_equal(bind_with(types,4),TURBODB_STATUS_OK); turbodb_value_t values[]={turbodb_i64(10),turbodb_i64(2),turbodb_i64(1),turbodb_i64(2)};
    check_equal(open_with(values,4),TURBODB_STATUS_OK); values[0]=turbodb_i64(99); values[1]=turbodb_i64(0);
    sqlparser_document_destroy(document); document=NULL;
    const orm_sql_scan_row first=next(); check_equal(first.values[0].data.int64_value,11); check_equal(first.values[1].data.int64_value,1);
    const orm_sql_scan_row second=next(); check_equal(second.values[0].data.int64_value,12); check_equal(second.values[1].data.int64_value,1);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("checks NTILE parameters even for zero limit without reading rows") {
    parse("SELECT NTILE(?) OVER () AS n FROM items LIMIT 0"); const orm_sql_type type={TURBODB_VALUE_UINT64,true};
    check_equal(bind_with(&type,1),TURBODB_STATUS_OK);
    const turbodb_value_t bad[]={turbodb_null(),turbodb_u64(0),turbodb_u64((UINT64_C(1)<<63)+1)};
    const turbodb_status_t status[]={TURBODB_STATUS_TYPE_ERROR,TURBODB_STATUS_SQL_ERROR,TURBODB_STATUS_SQL_ERROR};
    for(size_t i=0;i<3;++i) { check_equal(open_with(&bad[i],1),status[i]); check_null(run.program); }
    const turbodb_value_t valid=turbodb_u64(UINT64_C(1)<<63); check_equal(open_with(&valid,1),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
  }
  it("prunes unobserved windows for cardinality but keeps domain validation") {
    parse("SELECT ROW_NUMBER() OVER (ORDER BY id+9223372036854775807) AS n FROM items WHERE id>1 LIMIT 2");
    check_equal(bind_select(),TURBODB_STATUS_OK); check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_ROW); check_equal(next().state,ORM_SQL_SCAN_ROW); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_false(run.window_run.initialized); check_equal(cardinality_calls,3u);
  }
  it("resolves named partitions and orders for ranking and offset functions") {
    parse("SELECT id,ROW_NUMBER() OVER (w ORDER BY id) AS n,RANK() OVER ranked AS r,"
        "LAG(id,1,0) OVER (w ORDER BY id) AS previous FROM items "
        "WINDOW ranked AS (w ORDER BY id DESC),w AS (PARTITION BY id>2) ORDER BY id");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    const int64_t numbers[]={1,2,1,2,3}, ranks[]={2,1,3,2,1}, previous[]={0,1,0,3,4};
    for(size_t i=0;i<TEST_ROWS;++i) {
      const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
      check_equal(row.values[0].data.int64_value,(int64_t)i+1);
      check_equal(row.values[1].data.int64_value,numbers[i]);
      check_equal(row.values[2].data.int64_value,ranks[i]);
      check_equal(row.values[3].data.int64_value,previous[i]);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }

  it("resolves forward and backward window inheritance with case insensitive quoted names") {
    parse("SELECT id,RANK() OVER `Last` AS n FROM items WINDOW `last` AS (middle),"
        "first AS (ORDER BY id DESC),middle AS (FIRST) ORDER BY id");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[1].data.int64_value,(int64_t)(TEST_ROWS-i));
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }

  it("retains named-window marker order after AST and caller parameters are released") {
    parse("SELECT id,LAG(id,?,?) OVER(w ORDER BY id) AS n FROM items "
        "WINDOW w AS (PARTITION BY id>?) ORDER BY id LIMIT ?");
    const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false},
        {TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false}};
    check_equal(bind_with(types,sizeof(types)/sizeof(types[0])),TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document=NULL;
    turbodb_value_t parameters[]={turbodb_i64(1),turbodb_i64(9),turbodb_i64(3),turbodb_i64(4)};
    check_equal(open_with(parameters,sizeof(parameters)/sizeof(parameters[0])),TURBODB_STATUS_OK);
    memset(parameters,0,sizeof(parameters));
    const int64_t expected[]={9,1,2,9};
    for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) check_equal(next().values[1].data.int64_value,expected[i]);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }

  it("shares equivalent named and inline window results for DISTINCT ordering") {
    parse("SELECT DISTINCT ROW_NUMBER() OVER w AS n FROM items WINDOW w AS (ORDER BY id) "
        "ORDER BY ROW_NUMBER() OVER (W),ROW_NUMBER() OVER(ORDER BY id)");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[0].data.int64_value,(int64_t)i+1);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }

  it("validates unused definitions without executing their key expressions") {
    parse("SELECT id FROM items WINDOW unused AS (ORDER BY id+9223372036854775807),empty AS () ORDER BY id");
    check_equal(bind_select(),TURBODB_STATUS_OK); check_equal(vec_size(&plan.windows),0u); open_rows();
    for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[0].data.int64_value,(int64_t)i+1);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
    reject("SELECT id FROM items WINDOW unused AS (ORDER BY missing) LIMIT 0",TURBODB_STATUS_SQL_ERROR,"column");
    reject("SELECT id FROM items WINDOW unused AS (ORDER BY name) LIMIT 0",TURBODB_STATUS_UNSUPPORTED,"window keys");
    reject("SELECT id AS x FROM items WINDOW unused AS (ORDER BY x)",TURBODB_STATUS_SQL_ERROR,"column");
  }

  it("matches computed GROUP BY keys in used and unused named definitions") {
    parse("SELECT COALESCE(score,0) AS k,RANK() OVER w AS n FROM items GROUP BY COALESCE(score,0) "
        "WINDOW w AS (ORDER BY COALESCE(score,0)),unused AS (PARTITION BY COALESCE(score,0)) ORDER BY k");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    const int64_t keys[]={0,10,20,30,40};
    for(size_t i=0;i<TEST_ROWS;++i) {
      const orm_sql_scan_row output=next(); check_equal(output.state,ORM_SQL_SCAN_ROW);
      check_equal(output.values[0].data.int64_value,keys[i]);
      check_equal(output.values[1].data.int64_value,(int64_t)i+1);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }

  it("refunds every named-window binding step and permits a fresh bind") {
    parse("SELECT id,RANK() OVER last+FIRST_VALUE(id) OVER last+LAST_VALUE(id) OVER last+NTH_VALUE(id,1) OVER last AS n "
        "FROM items WINDOW last AS (middle ROWS CURRENT ROW),middle AS (first ORDER BY id),"
        "first AS (PARTITION BY id>2),unused AS () ORDER BY FIRST_VALUE(id) OVER last,id");
    check_equal(bind_select(),TURBODB_STATUS_OK);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    for(uint64_t point=0;point<steps;++point) {
      reset(); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=point;
      check_equal(bind_select(),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(plan.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
    }
    reset(); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); check_equal(next().state,ORM_SQL_SCAN_ROW);
  }

  group("numeric bit aggregate SELECT binding") {
    it("binds numeric bit groups and BOOL inputs as nonnullable U64") {
      parse("SELECT bit_and(score) AS a,BiT_Or(score) AS o,BIT_XOR(score) AS x,BIT_XOR(id>2) AS flags FROM items");
      check_equal(bind_select(),TURBODB_STATUS_OK); check_true(plan.grouped); open_rows(); const orm_sql_scan_row row=next();
      const uint64_t expected[]={0,62,40,1};
      for(size_t i=0;i<4;++i) { check_equal(row.values[i].kind,TURBODB_VALUE_UINT64); check_equal(row.values[i].data.uint64_value,expected[i]); }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("keeps bit windows in row scope and starts NULL prefixes at their identities") {
      parse("SELECT id,BIT_AND(score) OVER w AS a,BIT_OR(score) OVER w AS o,BIT_XOR(score) OVER w AS x "
          "FROM items WINDOW w AS(ORDER BY id) ORDER BY id");
      check_equal(bind_select(),TURBODB_STATUS_OK); check_false(plan.grouped); open_rows();
      const uint64_t expected[][3]={{UINT64_MAX,0,0},{20,20,20},{0,30,30},{0,30,0},{0,62,40}};
      for(size_t i=0;i<TEST_ROWS;++i) {
        const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,(int64_t)i+1);
        for(size_t column=0;column<3;++column) {
          check_equal(row.values[column+1].kind,TURBODB_VALUE_UINT64); check_equal(row.values[column+1].data.uint64_value,expected[i][column]);
        }
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("feeds ordinary unsigned bit results to windows after HAVING") {
      parse("SELECT id,BIT_OR(score) AS bits,BIT_XOR(BIT_OR(score)) OVER() AS together FROM items GROUP BY id HAVING id>1 ORDER BY id");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); const uint64_t values[]={20,10,30,40};
      for(size_t i=0;i<4;++i) {
        const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,(int64_t)i+2);
        check_equal(row.values[1].data.uint64_value,values[i]); check_equal(row.values[2].data.uint64_value,40u);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("distinguishes bit operation kinds and frames during DISTINCT output binding") {
      parse("SELECT DISTINCT BIT_AND(id) OVER() AS a,BIT_OR(id) OVER() AS o,BIT_XOR(id) OVER() AS x FROM items");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.uint64_value,0u); check_equal(row.values[1].data.uint64_value,7u); check_equal(row.values[2].data.uint64_value,1u);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
      reset(); parse("SELECT id,BIT_OR(id) OVER(ORDER BY id ROWS CURRENT ROW) AS current_bits,BIT_OR(id) OVER(ORDER BY id) AS prefix_bits FROM items ORDER BY id");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); const uint64_t prefix[]={1,3,3,7,7};
      for(size_t i=0;i<TEST_ROWS;++i) {
        const orm_sql_scan_row value=next(); check_equal(value.values[1].data.uint64_value,i+1); check_equal(value.values[2].data.uint64_value,prefix[i]);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("owns bit marker types and values after AST release and caller mutation") {
      parse("SELECT id,BIT_XOR(?) OVER(ORDER BY id) AS bits FROM items ORDER BY id");
      const orm_sql_type type={TURBODB_VALUE_UINT64,false}; check_equal(bind_with(&type,1),TURBODB_STATUS_OK);
      sqlparser_document_destroy(document); document=NULL; turbodb_value_t input=turbodb_u64(UINT64_MAX);
      check_equal(open_with(&input,1),TURBODB_STATUS_OK); input=turbodb_u64(0);
      for(size_t i=0;i<TEST_ROWS;++i) {
        const orm_sql_scan_row row=next(); check_equal(row.values[1].data.uint64_value,i%2?0:UINT64_MAX);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("prunes unused bit argument conversion but preserves live HAVING errors") {
      double_scores(); rows[1][1]=turbodb_f64(DBL_MAX);
      parse("SELECT BIT_OR(score) AS bits FROM items"); check_equal(bind_select(),TURBODB_STATUS_OK);
      check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_ROW); check_equal(next().state,ORM_SQL_SCAN_DONE);
      reset(); parse("SELECT BIT_OR(score) AS bits FROM items HAVING bits IS NOT NULL"); check_equal(bind_select(),TURBODB_STATUS_OK);
      check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_OK); orm_sql_scan_row row={.count=99};
      check_equal(orm_tidesdb_sql_scan_next(&run.scan,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u);
    }
    it("rejects byte domains DISTINCT arity and illegal bit aggregate placements") {
      reject("SELECT BIT_OR(name) AS bits FROM items",TURBODB_STATUS_UNSUPPORTED,"numeric");
      reject("SELECT BIT_XOR(name) OVER() AS bits FROM items LIMIT 0",TURBODB_STATUS_UNSUPPORTED,"numeric");
      reject("SELECT BIT_AND(DISTINCT id) AS bits FROM items",TURBODB_STATUS_UNSUPPORTED,"DISTINCT");
      reject("SELECT BIT_XOR(DISTINCT id) OVER() AS bits FROM items",TURBODB_STATUS_UNSUPPORTED,"DISTINCT");
      reject("SELECT BIT_OR(id,score) AS bits FROM items",TURBODB_STATUS_SQL_ERROR,"argument");
      reject("SELECT BIT_XOR() OVER() AS bits FROM items",TURBODB_STATUS_SQL_ERROR,"arguments");
      reject("SELECT id FROM items WHERE BIT_AND(score)>0",TURBODB_STATUS_SQL_ERROR,"WHERE");
      reject("SELECT BIT_OR(BIT_XOR(id) OVER()) AS bits FROM items",TURBODB_STATUS_SQL_ERROR,"window functions");
    }
  }
  group("statistical aggregate SELECT binding") {
    it("binds ordinary statistical aliases as DOUBLE without requiring DECIMAL") {
      parse("SELECT VAR_POP(score) AS p,VAR_SAMP(score) AS s,STDDEV_POP(score) AS dp,STDDEV_SAMP(score) AS ds,"
          "variance(score) AS v,STD(score) AS a,StDdEv(score) AS b FROM items");
      check_equal(bind_select(),TURBODB_STATUS_OK); check_true(plan.grouped); open_rows(); const orm_sql_scan_row row=next();
      const double expected[]={125,500.0/3.0,11.180339887498949,12.909944487358056,125,11.180339887498949,11.180339887498949};
      for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) {
        check_equal(row.values[i].kind,TURBODB_VALUE_DOUBLE); check_less_equal(fabs(row.values[i].data.double_value-expected[i]),1e-10);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("keeps statistical windows in row scope and ignores NULL prefix samples") {
      parse("SELECT id,VAR_POP(score) OVER w AS p,VAR_SAMP(score) OVER w AS s,STDDEV(score) OVER w AS d FROM items WINDOW w AS(ORDER BY id) ORDER BY id");
      check_equal(bind_select(),TURBODB_STATUS_OK); check_false(plan.grouped); open_rows();
      const double population[]={0,0,25,200.0/3.0,125},sample[]={0,0,50,100,500.0/3.0};
      for(size_t i=0;i<TEST_ROWS;++i) {
        const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,(int64_t)i+1);
        if(!i) for(size_t column=1;column<4;++column) check_equal(row.values[column].kind,TURBODB_VALUE_NULL);
        else {
          check_less_equal(fabs(row.values[1].data.double_value-population[i]),1e-10);
          if(i==1) check_equal(row.values[2].kind,TURBODB_VALUE_NULL);
          else check_less_equal(fabs(row.values[2].data.double_value-sample[i]),1e-10);
          check_less_equal(fabs(row.values[3].data.double_value-sqrt(population[i])),1e-10);
        }
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("feeds ordinary moment outputs into windows after HAVING") {
      parse("SELECT id,VAR_POP(score) AS v,VAR_POP(VAR_POP(score)) OVER() AS across_groups FROM items GROUP BY id HAVING v IS NOT NULL ORDER BY id");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      for(size_t i=1;i<TEST_ROWS;++i) {
        const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,(int64_t)i+1);
        check_equal(row.values[1].data.double_value,0.0); check_equal(row.values[2].data.double_value,0.0);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("canonicalizes standard deviation aliases but distinguishes frames and sample kinds") {
      parse("SELECT DISTINCT STD(score) OVER() AS p,STDDEV_POP(score) OVER() AS same FROM items ORDER BY STDDEV(score) OVER()");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.double_value,row.values[1].data.double_value); check_equal(next().state,ORM_SQL_SCAN_DONE);
      reset(); parse("SELECT id,VAR_POP(id) OVER(ORDER BY id ROWS CURRENT ROW) AS p,VAR_SAMP(id) OVER(ORDER BY id) AS s FROM items ORDER BY id");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows(); const double expected[]={0,0.5,1,5.0/3.0,2.5};
      for(size_t i=0;i<TEST_ROWS;++i) {
        const orm_sql_scan_row value=next(); check_equal(value.values[1].data.double_value,0.0);
        if(!i) check_equal(value.values[2].kind,TURBODB_VALUE_NULL);
        else check_less_equal(fabs(value.values[2].data.double_value-expected[i]),1e-12);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("owns a statistical marker after AST release and caller mutation") {
      parse("SELECT id,VAR_POP(score+?) OVER(ORDER BY id ROWS 1 PRECEDING) AS p FROM items ORDER BY id");
      const orm_sql_type type={TURBODB_VALUE_INT64,false}; check_equal(bind_with(&type,1),TURBODB_STATUS_OK);
      sqlparser_document_destroy(document); document=NULL; turbodb_value_t value=turbodb_i64(10);
      check_equal(open_with(&value,1),TURBODB_STATUS_OK); value=turbodb_i64(INT64_MAX); const double expected[]={0,0,25,100,25};
      for(size_t i=0;i<TEST_ROWS;++i) {
        const orm_sql_scan_row row=next(); if(!i) check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
        else check_equal(row.values[1].data.double_value,expected[i]);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("prunes unused statistical arguments for cardinality but preserves HAVING errors") {
      parse("SELECT VAR_POP(id+9223372036854775807) AS p FROM items");
      check_equal(bind_select(),TURBODB_STATUS_OK); check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_OK);
      check_equal(next().state,ORM_SQL_SCAN_ROW); check_equal(next().state,ORM_SQL_SCAN_DONE);
      reset(); parse("SELECT VAR_POP(id+9223372036854775807) AS p FROM items HAVING p>0.0");
      check_equal(bind_select(),TURBODB_STATUS_OK); check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_OK);
      orm_sql_scan_row row={.count=99}; check_equal(orm_tidesdb_sql_scan_next(&run.scan,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u);
    }
    it("validates statistical types DISTINCT arity and illegal placement before reading") {
      reject("SELECT VAR_POP(name) AS p FROM items",TURBODB_STATUS_UNSUPPORTED,"numeric");
      reject("SELECT STDDEV(name) OVER() AS p FROM items LIMIT 0",TURBODB_STATUS_UNSUPPORTED,"numeric");
      reject("SELECT VAR_POP(DISTINCT id) OVER() AS p FROM items",TURBODB_STATUS_UNSUPPORTED,"DISTINCT");
      reject("SELECT VAR_POP(DISTINCT id) AS p FROM items",TURBODB_STATUS_UNSUPPORTED,"DISTINCT");
      reject("SELECT STDDEV_SAMP(id,score) AS p FROM items",TURBODB_STATUS_SQL_ERROR,"argument");
      reject("SELECT STD() OVER() AS p FROM items",TURBODB_STATUS_SQL_ERROR,"arguments");
      reject("SELECT id FROM items WHERE VARIANCE(score)>0",TURBODB_STATUS_SQL_ERROR,"WHERE");
      reject("SELECT VAR_SAMP(VAR_POP(id) OVER()) AS p FROM items",TURBODB_STATUS_SQL_ERROR,"window functions");
    }
  }
  group("window aggregate SELECT binding") {
    it("preserves source rows and counts NULL values through default peers") {
      parse("SELECT id,COUNT(*) OVER(ORDER BY score) AS n,COUNT(score) OVER(ORDER BY score) AS present FROM items ORDER BY id");
      check_equal(bind_select(),TURBODB_STATUS_OK); check_false(plan.grouped); open_rows();
      const int64_t count[]={1,3,2,4,5}, present[]={0,2,1,3,4};
      for(size_t i=0;i<TEST_ROWS;++i) {
        const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,(int64_t)i+1);
        check_equal(row.values[1].data.int64_value,count[i]); check_equal(row.values[2].data.int64_value,present[i]);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_GROUPS],0u);
    }
    it("keeps COUNT star hidden work slots out of a source star projection") {
      parse("SELECT items.*,COUNT(*) OVER() AS n,COUNT(name) OVER() AS text_count FROM items ORDER BY id");
      check_equal(bind_select(),TURBODB_STATUS_OK); check_false(plan.grouped); open_rows();
      for(size_t i=0;i<TEST_ROWS;++i) {
        const orm_sql_scan_row row=next(); check_equal(row.count,TEST_COLUMNS+2u);
        check_equal(row.values[2].kind,TURBODB_VALUE_TEXT); check_equal(row.values[3].data.int64_value,TEST_ROWS);
        check_equal(row.values[4].data.int64_value,TEST_ROWS);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("computes moving nullable MIN MAX in the named ROWS frame") {
      parse("SELECT id,MIN(score) OVER w AS low,MAX(score) OVER w AS high FROM items WINDOW w AS(ORDER BY id ROWS 1 PRECEDING) ORDER BY id");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      const int64_t low[]={0,20,10,10,30},high[]={0,20,20,30,40};
      for(size_t i=0;i<TEST_ROWS;++i) {
        const orm_sql_scan_row row=next();
        if(!i) { check_equal(row.values[1].kind,TURBODB_VALUE_NULL); check_equal(row.values[2].kind,TURBODB_VALUE_NULL); }
        else { check_equal(row.values[1].data.int64_value,low[i]); check_equal(row.values[2].data.int64_value,high[i]); }
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("computes DOUBLE SUM AVG after WHERE before final pagination") {
      double_scores();
      parse("SELECT id,SUM(score) OVER(ORDER BY id) AS total,AVG(score) OVER(ORDER BY id) AS mean FROM items WHERE id>1 ORDER BY id LIMIT 2 OFFSET 1");
      check_equal(bind_select(),TURBODB_STATUS_OK); check_false(plan.grouped); open_rows();
      const double sums[]={30,60}, means[]={15,20};
      for(size_t i=0;i<2;++i) {
        const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,(int64_t)i+3);
        check_equal(row.values[1].kind,TURBODB_VALUE_DOUBLE); check_equal(row.values[1].data.double_value,sums[i]);
        check_equal(row.values[2].data.double_value,means[i]);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("feeds ordinary COUNT groups into window aggregates after HAVING") {
      rows[3][1]=turbodb_i64(20);
      parse("SELECT score,COUNT(*) AS n,MAX(COUNT(*)) OVER() AS largest,COUNT(COUNT(*)) OVER() AS groups_count FROM items GROUP BY score HAVING score>0 ORDER BY score");
      const turbodb_status_t status=bind_select(); info("grouped windows: %s",error.message);
      check_equal(status,TURBODB_STATUS_OK); check_true(plan.grouped); open_rows();
      const int64_t scores[]={10,20,40},counts[]={1,2,1};
      for(size_t i=0;i<3;++i) {
        const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,scores[i]);
        check_equal(row.values[1].data.int64_value,counts[i]); check_equal(row.values[2].data.int64_value,2);
        check_equal(row.values[3].data.int64_value,3);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("distinguishes aggregate frames and value expressions in DISTINCT ordering") {
      parse("SELECT DISTINCT COUNT(*) OVER(ORDER BY id ROWS CURRENT ROW) AS one,COUNT(*) OVER(ORDER BY id) AS n FROM items ORDER BY n DESC");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      for(size_t i=0;i<TEST_ROWS;++i) {
        const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,1);
        check_equal(row.values[1].data.int64_value,(int64_t)(TEST_ROWS-i));
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
      reset(); parse("SELECT DISTINCT MAX(id) OVER() AS high,MIN(id) OVER() AS low FROM items ORDER BY high");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,5); check_equal(row.values[1].data.int64_value,1);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("preserves frame and value markers after AST release and caller mutation") {
      parse("SELECT id,MIN(id+?) OVER(ORDER BY id ROWS ? PRECEDING) AS low FROM items ORDER BY id");
      const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false}};
      check_equal(bind_with(types,2),TURBODB_STATUS_OK); sqlparser_document_destroy(document); document=NULL;
      turbodb_value_t values[]={turbodb_i64(10),turbodb_i64(1)}; check_equal(open_with(values,2),TURBODB_STATUS_OK);
      values[0]=turbodb_i64(0); values[1]=turbodb_i64(0);
      const int64_t expected[]={11,11,12,13,14};
      for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[1].data.int64_value,expected[i]);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("validates aggregate frame markers even under LIMIT zero EXPLAIN and cardinality pruning") {
      parse("SELECT COUNT(*) OVER(ORDER BY id ROWS ? PRECEDING) AS n FROM items LIMIT 0");
      const orm_sql_type type={TURBODB_VALUE_INT64,true}; check_equal(bind_with(&type,1),TURBODB_STATUS_OK);
      const turbodb_value_t invalid[]={turbodb_i64(-1),turbodb_null()};
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        check_equal(open_with(&invalid[i],1),TURBODB_STATUS_TYPE_ERROR); check_null(run.program);
        check_equal(open_cardinality(TEST_ROWS,&invalid[i],1),TURBODB_STATUS_TYPE_ERROR); check_equal(cardinality_calls,0u);
        orm_sql_explain_source explain={0};
        check_equal(orm_tidesdb_sql_explain_open(&plan,vstr_from_cstr("items"),&invalid[i],1,&explain,&error),TURBODB_STATUS_TYPE_ERROR);
        check_equal(orm_tidesdb_sql_explain_close(&explain,&error),TURBODB_STATUS_OK);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
    }
    it("keeps an empty window input empty without creating a global group") {
      parse("SELECT COUNT(*) OVER() AS n FROM items WHERE id<0");
      check_equal(bind_select(),TURBODB_STATUS_OK); check_false(plan.grouped); open_rows(); check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("prunes unobserved aggregate values for cardinality without changing row count") {
      parse("SELECT COUNT(id+9223372036854775807) OVER() AS n FROM items LIMIT 2");
      check_equal(bind_select(),TURBODB_STATUS_OK); check_equal(open_cardinality(TEST_ROWS,NULL,0),TURBODB_STATUS_OK);
      check_equal(next().state,ORM_SQL_SCAN_ROW); check_equal(next().state,ORM_SQL_SCAN_ROW); check_equal(next().state,ORM_SQL_SCAN_DONE);
      check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK); open_rows();
      orm_sql_scan_row row={.count=99}; check_equal(orm_tidesdb_sql_scan_next(&run.scan,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(row.count,99u);
    }
    it("rejects unsupported aggregate types DISTINCT arity and nested windows") {
      reject("SELECT SUM(score) OVER() AS n FROM items",TURBODB_STATUS_UNSUPPORTED,"DECIMAL");
      reject("SELECT AVG(id) OVER() AS n FROM items LIMIT 0",TURBODB_STATUS_UNSUPPORTED,"DECIMAL");
      reject("SELECT MIN(name) OVER() AS n FROM items",TURBODB_STATUS_UNSUPPORTED,"numeric");
      reject("SELECT COUNT(DISTINCT id) OVER() AS n FROM items",TURBODB_STATUS_UNSUPPORTED,"DISTINCT");
      reject("SELECT COUNT() OVER() AS n FROM items",TURBODB_STATUS_SQL_ERROR,"arguments");
      reject("SELECT MIN(id,score) OVER() AS n FROM items",TURBODB_STATUS_SQL_ERROR,"arguments");
      reject("SELECT MAX(ROW_NUMBER() OVER()) OVER() AS n FROM items",TURBODB_STATUS_SQL_ERROR,"window functions");
      reject("SELECT COUNT(MAX(id) OVER()) AS n FROM items",TURBODB_STATUS_SQL_ERROR,"aggregate arguments");
    }
  }
  group("frame value SELECT binding") {
    it("executes default frames with peers for all three value functions") {
      const int64_t scores[]={10,10,20,30,30},last[]={2,2,3,5,5};
      for(size_t i=0;i<TEST_ROWS;++i) rows[i][1]=turbodb_i64(scores[i]);
      parse("SELECT id,FIRST_VALUE(id) OVER w AS f,LAST_VALUE(id) OVER w AS l,NTH_VALUE(id,3) OVER w AS n "
          "FROM items WINDOW w AS(ORDER BY score) ORDER BY id");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      for(size_t i=0;i<TEST_ROWS;++i) {
        const orm_sql_scan_row output=next(); check_equal(output.state,ORM_SQL_SCAN_ROW);
        check_equal(output.values[1].data.int64_value,1); check_equal(output.values[2].data.int64_value,last[i]);
        if(i<2) check_equal(output.values[3].kind,TURBODB_VALUE_NULL); else check_equal(output.values[3].data.int64_value,3);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("executes rolling ROWS named frames after AST and parameter storage are released") {
      parse("SELECT id,NTH_VALUE(id+?,?) OVER child AS n FROM items WINDOW child AS "
          "(base ROWS BETWEEN ? PRECEDING AND ? FOLLOWING),base AS(PARTITION BY id>? ORDER BY id) ORDER BY id LIMIT ?");
      const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_UINT64,false},
          {TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false}};
      check_equal(bind_with(types,sizeof(types)/sizeof(types[0])),TURBODB_STATUS_OK);
      sqlparser_document_destroy(document); document=NULL;
      turbodb_value_t parameters[]={turbodb_i64(10),turbodb_i64(2),turbodb_u64(1),turbodb_i64(1),turbodb_i64(3),turbodb_i64(TEST_ROWS)};
      check_equal(open_with(parameters,sizeof(parameters)/sizeof(parameters[0])),TURBODB_STATUS_OK); memset(parameters,0,sizeof(parameters));
      const int64_t expected[]={12,12,13,15,15};
      for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[1].data.int64_value,expected[i]);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("executes numeric RANGE frames in descending order with marker distances") {
      parse("SELECT id,FIRST_VALUE(id) OVER w AS f,LAST_VALUE(id) OVER w AS l FROM items "
          "WINDOW w AS(ORDER BY id DESC RANGE BETWEEN ? PRECEDING AND ? FOLLOWING) ORDER BY id");
      const orm_sql_type types[]={{TURBODB_VALUE_DOUBLE,false},{TURBODB_VALUE_UINT64,false}};
      const turbodb_value_t parameters[]={turbodb_f64(1.5),turbodb_u64(1)};
      check_equal(bind_with(types,2),TURBODB_STATUS_OK); check_equal(open_with(parameters,2),TURBODB_STATUS_OK);
      for(size_t i=0;i<TEST_ROWS;++i) {
        const orm_sql_scan_row output=next();
        check_equal(output.values[1].data.int64_value,(int64_t)(i+2<TEST_ROWS ? i+2 : TEST_ROWS));
        check_equal(output.values[2].data.int64_value,(int64_t)(i ? i : 1));
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("returns NULL for empty frames and for a valid position larger than the frame") {
      parse("SELECT FIRST_VALUE(id) OVER w AS f,LAST_VALUE(id) OVER w AS l,NTH_VALUE(id,9223372036854775807) OVER() AS n "
          "FROM items WINDOW w AS(ORDER BY id ROWS BETWEEN 1 PRECEDING AND 2 PRECEDING)");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      for(size_t i=0;i<TEST_ROWS;++i) {
        const orm_sql_scan_row output=next();
        for(size_t j=0;j<3;++j) check_equal(output.values[j].kind,TURBODB_VALUE_NULL);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("preserves nullable and TEXT values over the complete unordered partition") {
      parse("SELECT FIRST_VALUE(score) OVER() AS f,LAST_VALUE(name) OVER() AS l,NTH_VALUE(name,2) OVER() AS n FROM items");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      for(size_t i=0;i<TEST_ROWS;++i) {
        const orm_sql_scan_row output=next(); check_equal(output.values[0].kind,TURBODB_VALUE_NULL);
        check_equal(output.values[1].kind,TURBODB_VALUE_TEXT); check_equal(output.values[1].data.text_value.data[0],'e');
        check_equal(output.values[2].kind,TURBODB_VALUE_TEXT); check_equal(output.values[2].data.text_value.data[0],'b');
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("evaluates value frames after grouped expressions and HAVING") {
      parse("SELECT score,COUNT(*) AS c,FIRST_VALUE(score) OVER(ORDER BY score ROWS 1 PRECEDING) AS f "
          "FROM items GROUP BY score HAVING score IS NOT NULL ORDER BY score");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      const int64_t expected[]={10,10,20,30};
      for(size_t i=0;i<TEST_ROWS-1;++i) { const orm_sql_scan_row output=next(); check_equal(output.values[2].data.int64_value,expected[i]); }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("canonicalizes default RANGE frames but keeps differing frame expressions separate") {
      parse("SELECT DISTINCT LAST_VALUE(id) OVER(ORDER BY id) AS n FROM items "
          "ORDER BY LAST_VALUE(id) OVER(ORDER BY id RANGE UNBOUNDED PRECEDING)");
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[0].data.int64_value,(int64_t)i+1);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
      reset(); parse("SELECT DISTINCT LAST_VALUE(id) OVER(ORDER BY id ROWS CURRENT ROW) AS n FROM items "
          "ORDER BY LAST_VALUE(id) OVER(ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING)");
      check_equal(bind_select(),TURBODB_STATUS_SQL_ERROR); check_null(plan.budget);
      reset(); parse("SELECT DISTINCT FIRST_VALUE(id) OVER(ORDER BY id ROWS ? PRECEDING) AS n FROM items "
          "ORDER BY FIRST_VALUE(id) OVER(ORDER BY id ROWS ? PRECEDING)");
      const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false}};
      check_equal(bind_with(types,2),TURBODB_STATUS_SQL_ERROR); check_null(plan.budget);
    }
    it("validates NTH_VALUE positions under zero limit explanation and existence pruning") {
      parse("SELECT NTH_VALUE(id,?) OVER(ORDER BY id) AS n FROM items LIMIT 0");
      const orm_sql_type type={TURBODB_VALUE_UINT64,true}; check_equal(bind_with(&type,1),TURBODB_STATUS_OK);
      const turbodb_value_t invalid[]={turbodb_null(),turbodb_u64(0),turbodb_u64(ORM_SQL_WINDOW_MAX_NTH+1)};
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
        const turbodb_status_t status=i ? TURBODB_STATUS_SQL_ERROR : TURBODB_STATUS_TYPE_ERROR;
        check_equal(open_with(&invalid[i],1),status); check_null(run.program);
        check_equal(open_cardinality(TEST_ROWS,&invalid[i],1),status); check_equal(cardinality_calls,0u);
        orm_sql_explain_source explain={0};
        const vstr table={"items",5};
        check_equal(orm_tidesdb_sql_explain_open(&plan,table,&invalid[i],1,&explain,&error),status);
        check_equal(orm_tidesdb_sql_explain_close(&explain,&error),TURBODB_STATUS_OK);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
      }
      const turbodb_value_t valid=turbodb_u64(ORM_SQL_WINDOW_MAX_NTH); check_equal(open_with(&valid,1),TURBODB_STATUS_OK);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("rejects invalid value function arities nested windows and row-dependent positions") {
      const char *invalid[]={"SELECT FIRST_VALUE() OVER() AS n FROM items","SELECT LAST_VALUE(id,1) OVER() AS n FROM items",
          "SELECT NTH_VALUE(id) OVER() AS n FROM items","SELECT NTH_VALUE(id,1,2) OVER() AS n FROM items"};
      for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) reject(invalid[i],TURBODB_STATUS_SQL_ERROR,"arguments");
      reject("SELECT NTH_VALUE(id,score) OVER() AS n FROM items",TURBODB_STATUS_SQL_ERROR,"integer literal");
      reject("SELECT NTH_VALUE(id,0) OVER() AS n FROM items",TURBODB_STATUS_SQL_ERROR,"position");
      reject("SELECT NTH_VALUE(id,9223372036854775808) OVER() AS n FROM items",TURBODB_STATUS_SQL_ERROR,"position");
      reject("SELECT FIRST_VALUE(LAST_VALUE(id) OVER()) OVER() AS n FROM items",TURBODB_STATUS_SQL_ERROR,"nested");
      reset(); parse("SELECT NTH_VALUE(id,?) OVER() AS n FROM items");
      const orm_sql_type type={TURBODB_VALUE_DOUBLE,false}; check_equal(bind_with(&type,1),TURBODB_STATUS_TYPE_ERROR);
    }
  }

  it("keeps all eight nonframing functions on the complete partition with an explicit frame") {
    parse("SELECT id,ROW_NUMBER() OVER w AS rn,RANK() OVER w AS r,DENSE_RANK() OVER w AS d,"
        "PERCENT_RANK() OVER w AS p,CUME_DIST() OVER w AS c,NTILE(2) OVER w AS bucket,"
        "LAG(id,1,99) OVER w AS previous,LEAD(id,1,88) OVER w AS following FROM items "
        "WINDOW w AS (ORDER BY id ROWS BETWEEN CURRENT ROW AND CURRENT ROW) ORDER BY id");
    check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    for(size_t i=0;i<TEST_ROWS;++i) {
      const orm_sql_scan_row output=next(); check_equal(output.state,ORM_SQL_SCAN_ROW);
      for(size_t column=0;column<4;++column) check_equal(output.values[column].data.int64_value,(int64_t)i+1);
      check_equal(output.values[4].data.double_value,(double)i/(TEST_ROWS-1));
      check_equal(output.values[5].data.double_value,(double)(i+1)/TEST_ROWS);
      check_equal(output.values[6].data.int64_value,i<3?1:2);
      check_equal(output.values[7].data.int64_value,i?(int64_t)i:99);
      check_equal(output.values[8].data.int64_value,i+1<TEST_ROWS?(int64_t)i+2:88);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }

  it("admits empty same-direction frames and numeric RANGE offsets without changing ranks") {
    const char *frames[]={"ROWS BETWEEN 1 PRECEDING AND 2 PRECEDING",
        "ROWS BETWEEN 2 FOLLOWING AND 1 FOLLOWING","ROWS 18446744073709551615 PRECEDING",
        "RANGE BETWEEN 1.5 PRECEDING AND 2e0 FOLLOWING","RANGE UNBOUNDED PRECEDING",
        "RANGE BETWEEN CURRENT ROW AND UNBOUNDED FOLLOWING"};
    enum { SQL_CAPACITY=256 }; char sql[SQL_CAPACITY];
    for(size_t sample=0;sample<sizeof(frames)/sizeof(frames[0]);++sample) {
      reset(); const int length=snprintf(sql,sizeof(sql),"SELECT id,RANK() OVER(ORDER BY id %s) AS n FROM items ORDER BY id",frames[sample]);
      check_true(length>0 && (size_t)length<sizeof(sql)); parse(sql);
      check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
      for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[1].data.int64_value,(int64_t)i+1);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
  }

  it("canonicalizes nonframing DISTINCT results across different valid frame specifications") {
    parse("SELECT DISTINCT ROW_NUMBER() OVER(ORDER BY id ROWS CURRENT ROW) AS n FROM items "
        "ORDER BY ROW_NUMBER() OVER(ORDER BY id RANGE UNBOUNDED PRECEDING)");
    check_equal(bind_select(),TURBODB_STATUS_OK); check_equal(vec_size(&plan.window_frames),2u); open_rows();
    for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[0].data.int64_value,(int64_t)i+1);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }

  it("owns frame markers in global SQL order after the document and caller arguments die") {
    parse("SELECT id,LAG(id,?,?) OVER w AS n FROM items WINDOW w AS "
        "(PARTITION BY id>? ORDER BY id ROWS BETWEEN ? PRECEDING AND ? FOLLOWING) ORDER BY id LIMIT ?");
    const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false},
        {TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_UINT64,false},{TURBODB_VALUE_INT64,false}};
    check_equal(bind_with(types,sizeof(types)/sizeof(types[0])),TURBODB_STATUS_OK);
    check_equal(vec_size(&plan.window_frames),1u); sqlparser_document_destroy(document); document=NULL;
    turbodb_value_t parameters[]={turbodb_i64(1),turbodb_i64(9),turbodb_i64(3),turbodb_i64(0),turbodb_u64(UINT64_MAX),turbodb_i64(TEST_ROWS)};
    check_equal(open_with(parameters,sizeof(parameters)/sizeof(parameters[0])),TURBODB_STATUS_OK);
    memset(parameters,0,sizeof(parameters)); const int64_t expected[]={9,1,2,9,4};
    for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[1].data.int64_value,expected[i]);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }

  it("validates unused frame offsets under zero limit explanation and cardinality pruning") {
    parse("SELECT id FROM items WINDOW unused AS (ORDER BY id ROWS ? PRECEDING) LIMIT 0");
    const orm_sql_type type={TURBODB_VALUE_INT64,true}; check_equal(bind_with(&type,1),TURBODB_STATUS_OK);
    check_equal(vec_size(&plan.windows),0u);
    const turbodb_value_t invalid[]={turbodb_i64(-1),turbodb_null()}; const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
      check_equal(open_with(&invalid[i],1),TURBODB_STATUS_TYPE_ERROR); check_null(run.program);
      check_equal(open_cardinality(TEST_ROWS,&invalid[i],1),TURBODB_STATUS_TYPE_ERROR); check_equal(cardinality_calls,0u);
      orm_sql_explain_source explain={0};
      const vstr table={"items",5};
      check_equal(orm_tidesdb_sql_explain_open(&plan,table,&invalid[i],1,&explain,&error),TURBODB_STATUS_TYPE_ERROR);
      check_equal(orm_tidesdb_sql_explain_close(&explain,&error),TURBODB_STATUS_OK);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
    }
    const turbodb_value_t valid=turbodb_i64(0); check_equal(open_with(&valid,1),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }

  it("requires exact integer ROWS markers but admits finite DOUBLE RANGE markers") {
    parse("SELECT RANK() OVER(ORDER BY id ROWS ? PRECEDING) AS n FROM items LIMIT 0");
    const orm_sql_type type={TURBODB_VALUE_DOUBLE,true}; check_equal(bind_with(&type,1),TURBODB_STATUS_TYPE_ERROR);
    check_contains(error.message,"ROWS requires an integer"); check_null(plan.budget);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    reset(); parse("SELECT RANK() OVER(ORDER BY id RANGE ? PRECEDING) AS n FROM items ORDER BY id");
    check_equal(bind_with(&type,1),TURBODB_STATUS_OK);
    const turbodb_value_t invalid[]={turbodb_f64(-0.5),turbodb_null(),turbodb_f64(NAN),turbodb_f64(INFINITY)};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
      check_equal(open_with(&invalid[i],1),TURBODB_STATUS_TYPE_ERROR); check_null(run.program);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
    }
    const turbodb_value_t valid=turbodb_f64(0.5); check_equal(open_with(&valid,1),TURBODB_STATUS_OK);
    for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[0].data.int64_value,(int64_t)i+1);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }

  it("refunds every frame parameter validation and zero-limit open step boundary") {
    parse("SELECT id,NTILE(?) OVER w AS n FROM items WINDOW w AS "
        "(ORDER BY id ROWS BETWEEN ? PRECEDING AND ? FOLLOWING),unused AS "
        "(ORDER BY id RANGE ? PRECEDING) LIMIT 0");
    const orm_sql_type types[]={{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false},
        {TURBODB_VALUE_INT64,false},{TURBODB_VALUE_INT64,false}};
    const turbodb_value_t parameters[]={turbodb_i64(2),turbodb_i64(0),turbodb_i64(3),turbodb_i64(1)};
    check_equal(bind_with(types,sizeof(types)/sizeof(types[0])),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(open_with(parameters,sizeof(parameters)/sizeof(parameters[0])),TURBODB_STATUS_OK);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    check_equal(orm_tidesdb_sql_select_close(&run,&error),TURBODB_STATUS_OK);
    for(uint64_t point=0;point<steps;++point) {
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      check_equal(open_with(parameters,sizeof(parameters)/sizeof(parameters[0])),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_null(run.program); check_equal(plan.active_runs,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
    }
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=TEST_LIMIT;
    check_equal(open_with(parameters,sizeof(parameters)/sizeof(parameters[0])),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }

  it("rejects illegal frame directions temporal ROWS and nonnumeric RANGE orders before reading") {
    const char *invalid[]={
        "SELECT RANK() OVER(ROWS BETWEEN UNBOUNDED FOLLOWING AND CURRENT ROW) AS n FROM items",
        "SELECT RANK() OVER(ROWS BETWEEN CURRENT ROW AND UNBOUNDED PRECEDING) AS n FROM items",
        "SELECT RANK() OVER(ROWS BETWEEN CURRENT ROW AND 1 PRECEDING) AS n FROM items",
        "SELECT RANK() OVER(ROWS BETWEEN 1 FOLLOWING AND CURRENT ROW) AS n FROM items",
        "SELECT RANK() OVER(ROWS BETWEEN 1 FOLLOWING AND 1 PRECEDING) AS n FROM items"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) reject(invalid[i],TURBODB_STATUS_SQL_ERROR,"boundary order");
    reject("SELECT RANK() OVER(ROWS 1.0 PRECEDING) AS n FROM items",TURBODB_STATUS_SQL_ERROR,"integer");
    reject("SELECT RANK() OVER(ROWS INTERVAL 1 DAY PRECEDING) AS n FROM items",TURBODB_STATUS_SQL_ERROR,"INTERVAL");
    reject("SELECT RANK() OVER(ORDER BY id RANGE INTERVAL 1 DAY PRECEDING) AS n FROM items",TURBODB_STATUS_UNSUPPORTED,"temporal");
    reject("SELECT RANK() OVER(RANGE 1 PRECEDING) AS n FROM items",TURBODB_STATUS_SQL_ERROR,"one numeric");
    reject("SELECT RANK() OVER(ORDER BY id,score RANGE 1 PRECEDING) AS n FROM items",TURBODB_STATUS_SQL_ERROR,"one numeric");
    reject("SELECT RANK() OVER(ORDER BY NULL RANGE 1 PRECEDING) AS n FROM items",TURBODB_STATUS_SQL_ERROR,"numeric ORDER BY type");
    reject("SELECT id FROM items WINDOW w AS (ORDER BY NULL),unused AS(w RANGE 1 PRECEDING) LIMIT 0",TURBODB_STATUS_SQL_ERROR,"numeric ORDER BY type");
  }

  it("permits direct framed window references and forbids inheriting their explicit frames") {
    parse("SELECT id,RANK() OVER child AS n FROM items WINDOW child AS (base ROWS CURRENT ROW),"
        "base AS (ORDER BY id DESC) ORDER BY id"); check_equal(bind_select(),TURBODB_STATUS_OK); open_rows();
    for(size_t i=0;i<TEST_ROWS;++i) check_equal(next().values[1].data.int64_value,(int64_t)(TEST_ROWS-i));
    check_equal(next().state,ORM_SQL_SCAN_DONE);
    reject("SELECT RANK() OVER(w) AS n FROM items WINDOW w AS(ROWS CURRENT ROW)",TURBODB_STATUS_SQL_ERROR,"explicit frame");
    reject("SELECT id FROM items WINDOW w AS(ROWS CURRENT ROW),unused AS(w)",TURBODB_STATUS_SQL_ERROR,"explicit frame");
    reject("SELECT RANK() OVER(w RANGE CURRENT ROW) AS n FROM items WINDOW w AS(ROWS CURRENT ROW)",TURBODB_STATUS_SQL_ERROR,"explicit frame");
  }

  it("rejects unknown duplicate cyclic and conflicting window inheritance before reading") {
    reject("SELECT id FROM items WINDOW w AS (missing)",TURBODB_STATUS_SQL_ERROR,"unknown named");
    reject("SELECT id FROM items WINDOW w AS (),W AS ()",TURBODB_STATUS_SQL_ERROR,"duplicate named");
    reject("SELECT id FROM items WINDOW w AS (w)",TURBODB_STATUS_SQL_ERROR,"cycle");
    reject("SELECT id FROM items WINDOW w AS (x),x AS (y),y AS (w)",TURBODB_STATUS_SQL_ERROR,"cycle");
    reject("SELECT id FROM items WINDOW w AS (),x AS (w PARTITION BY id)",TURBODB_STATUS_SQL_ERROR,"PARTITION BY");
    reject("SELECT RANK() OVER (w PARTITION BY id) AS n FROM items WINDOW w AS ()",TURBODB_STATUS_SQL_ERROR,"PARTITION BY");
    reject("SELECT id FROM items WINDOW w AS (ORDER BY id),x AS (w ORDER BY score)",TURBODB_STATUS_SQL_ERROR,"ORDER BY");
    reject("SELECT RANK() OVER (w ORDER BY score) AS n FROM items WINDOW w AS (ORDER BY id)",TURBODB_STATUS_SQL_ERROR,"ORDER BY");
    reject("SELECT id FROM items WINDOW w AS (ORDER BY RANK() OVER ())",TURBODB_STATUS_SQL_ERROR,"only in SELECT");
  }

  it("rejects invalid window placements arguments names types and same-select aliases before execution") {
    reject("SELECT id FROM items WHERE ROW_NUMBER() OVER ()>1",TURBODB_STATUS_SQL_ERROR,"only in SELECT");
    reject("SELECT id FROM items GROUP BY RANK() OVER ()",TURBODB_STATUS_SQL_ERROR,"only in SELECT");
    reject("SELECT ROW_NUMBER() OVER () AS n FROM items GROUP BY n",TURBODB_STATUS_SQL_ERROR,"GROUP BY");
    reject("SELECT COUNT(*) AS n FROM items HAVING RANK() OVER ()>1",TURBODB_STATUS_SQL_ERROR,"only in SELECT");
    reject("SELECT score,ROW_NUMBER() OVER () AS n FROM items GROUP BY score HAVING n>1",TURBODB_STATUS_SQL_ERROR,"HAVING");
    reject("SELECT COUNT(RANK() OVER ()) AS n FROM items",TURBODB_STATUS_SQL_ERROR,"aggregate arguments");
    reject("SELECT RANK() OVER (ORDER BY ROW_NUMBER() OVER ()) AS n FROM items",TURBODB_STATUS_SQL_ERROR,"nested");
    reject("SELECT ROW_NUMBER(1) OVER () AS n FROM items",TURBODB_STATUS_SQL_ERROR,"arguments");
    reject("SELECT NTILE(0) OVER () AS n FROM items",TURBODB_STATUS_SQL_ERROR,"bucket");
    reject("SELECT NTILE(id) OVER () AS n FROM items",TURBODB_STATUS_SQL_ERROR,"integer literal");
    reject("SELECT RANK() OVER w AS n FROM items",TURBODB_STATUS_SQL_ERROR,"unknown named");
    reject("SELECT RANK() OVER (w ORDER BY id) AS n FROM items",TURBODB_STATUS_SQL_ERROR,"unknown named");
    reject("SELECT JSON_ARRAYAGG(score) OVER () AS n FROM items",TURBODB_STATUS_UNSUPPORTED,"window function");
    reject("SELECT RANK() OVER (ORDER BY name) AS n FROM items",TURBODB_STATUS_UNSUPPORTED,"window keys");
    reject("SELECT id AS x,RANK() OVER (ORDER BY x) AS n FROM items",TURBODB_STATUS_SQL_ERROR,"column");
  }
}
