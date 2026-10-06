#include "aggregate.h"
#include <tinytest.h>
#include <cstl/sort.h>
#include <math.h>
#include <float.h>
#include <string.h>

static size_t reserve_calls, resize_calls, fail_reserve, fail_resize;
static bool fail_sort;
static stl_status aggregate_test_reserve(vec_t *v, size_t count) {
  return ++reserve_calls == fail_reserve ? STL_OUT_OF_MEMORY : vec_reserve(v,count);
}
static stl_status aggregate_test_resize(vec_t *v, size_t count) {
  return ++resize_calls == fail_resize ? STL_OUT_OF_MEMORY : vec_resize(v,count);
}
static stl_status aggregate_test_sort(void *base, size_t count, const cmeta_type_desc *type, size_t bytes) {
  return fail_sort ? STL_OUT_OF_MEMORY : stable_sort(base,count,type,bytes);
}
#define vec_reserve aggregate_test_reserve
#define vec_resize aggregate_test_resize
#define stable_sort aggregate_test_sort
#include "../../src/work.c"
#include "../../src/rows.c"
#include "../../src/scan.c"
#include "../../src/aggregate.c"
#undef vec_reserve
#undef vec_resize
#undef stable_sort

enum { TEST_ROWS=9, TEST_COLUMNS=3, TEST_LIMIT=65536, TEST_WORK=4*1024*1024, TEST_DEPTH=32 };
static orm_tidesdb_sql_budget budget;
static orm_sql_budget_limits limits;
static turbodb_error_t error;
static orm_sql_aggregate aggregate;
static orm_sql_scan downstream;
static orm_sql_expr having;
static orm_sql_type types[TEST_COLUMNS];
static turbodb_value_t rows[TEST_ROWS][TEST_COLUMNS], buffer[TEST_COLUMNS];
static size_t input_rows, input_position, calls, fail_read;
static orm_sql_row_source source;
static size_t keys[TEST_COLUMNS];
static orm_sql_aggregate_item items[4];

static turbodb_status_t input_next(void *context, const turbodb_value_t **out, turbodb_error_t *e) {
  (void)context; ++calls;
  if (calls == fail_read) { tdsql_error_set(e,TURBODB_STATUS_DATASTORE_ERROR,"aggregate test read failure"); return TURBODB_STATUS_DATASTORE_ERROR; }
  memset(buffer,0,sizeof(buffer));
  if (input_position == input_rows) { *out=NULL; return TURBODB_STATUS_OK; }
  orm_sql_budget_amount amount={0}; amount.value[ORM_SQL_BUDGET_READ_ROWS]=1;
  amount.value[ORM_SQL_BUDGET_READ_BYTES]=sizeof(buffer);
  turbodb_status_t status=orm_tidesdb_sql_budget_reserve(&budget,&amount,e);
  if (status != TURBODB_STATUS_OK) return status;
  memcpy(buffer,rows[input_position++],sizeof(buffer)); *out=buffer; return TURBODB_STATUS_OK;
}
static turbodb_status_t open_aggregate(size_t key_count, size_t item_count) {
  const orm_sql_aggregate_spec spec={keys,key_count,items,item_count};
  return orm_tidesdb_sql_aggregate_open(&source,&spec,&aggregate,&error);
}
static orm_sql_scan_row next(void) {
  orm_sql_scan_row out={0}; check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_OK); return out;
}
static void clean(void) {
  fail_reserve=fail_resize=0; fail_sort=false;
  check_equal(orm_tidesdb_sql_scan_close(&downstream,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_aggregate_close(&aggregate,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_expr_destroy(&having,&error),TURBODB_STATUS_OK);
  check_false(source.active);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
  check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
  check_equal(budget.used.value[ORM_SQL_BUDGET_GROUPS],0u);
}
static void reset(void) {
  clean(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_init(&budget,&limits,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
  input_position=calls=fail_read=0;
}
static void double_items(void) {
  types[1]=(orm_sql_type){TURBODB_VALUE_DOUBLE,true};
  for(size_t i=0;i<TEST_ROWS;++i)
    if(rows[i][1].kind==TURBODB_VALUE_INT64) rows[i][1]=turbodb_f64((double)rows[i][1].data.int64_value);
  items[0]=(orm_sql_aggregate_item){ORM_SQL_SUM,1}; items[1]=(orm_sql_aggregate_item){ORM_SQL_AVG,1};
  items[2]=(orm_sql_aggregate_item){ORM_SQL_COUNT_VALUE,1}; items[3]=(orm_sql_aggregate_item){ORM_SQL_MAX,1};
}
static void moment_items(void) {
  for(size_t i=0;i<4;++i) items[i]=(orm_sql_aggregate_item){(orm_sql_aggregate_kind)(ORM_SQL_VAR_POP+i),1};
}
static void bit_items(void) {
  for(size_t i=0;i<3;++i) items[i]=(orm_sql_aggregate_item){(orm_sql_aggregate_kind)(ORM_SQL_BIT_AND+i),1};
}
static void fold_every_step(size_t item_count) {
  for(size_t key_count=0;key_count<=1;++key_count) {
    reset(); check_equal(open_aggregate(key_count,item_count),TURBODB_STATUS_OK);
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    while(next().state==ORM_SQL_SCAN_ROW) {}
    const uint64_t cost=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    for(uint64_t point=0;point<cost;++point) {
      reset(); check_equal(open_aggregate(key_count,item_count),TURBODB_STATUS_OK);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      orm_sql_scan_row out; turbodb_status_t status;
      do {
        out=(orm_sql_scan_row){.state=ORM_SQL_SCAN_CANCELLED,.count=99};
        status=orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error);
      } while(status==TURBODB_STATUS_OK && out.state==ORM_SQL_SCAN_ROW);
      check_equal(status,TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(out.count,99u); check_equal(out.state,ORM_SQL_SCAN_CANCELLED);
      const size_t reads=calls;
      check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(calls,reads);
    }
  }
}
static void expect_group(int64_t key, int64_t count, int64_t nonnull, int64_t minimum, int64_t maximum) {
  const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.count,5u);
  check_equal(row.values[0].data.int64_value,key); check_equal(row.values[1].data.int64_value,count);
  check_equal(row.values[2].data.int64_value,nonnull);
  if (nonnull) {
    check_equal(row.values[3].data.int64_value,minimum); check_equal(row.values[4].data.int64_value,maximum);
  } else {
    check_equal(row.values[3].kind,TURBODB_VALUE_NULL); check_equal(row.values[4].kind,TURBODB_VALUE_NULL);
  }
}
static void build_having(void) {
  const char sql[]="SELECT ? > 1"; sqlparser_document *document=NULL; sqlparser_error e;
  check_equal(sqlparser_parse(sql,strlen(sql),NULL,&document,&e),SQLPARSER_OK);
  const sqlparser_node *statement=sqlparser_get_node(document,sqlparser_statements(document).first);
  const sqlparser_node *projection=sqlparser_get_node(document,statement->as.select.columns.first);
  orm_sql_expr_input input={0};
  for(size_t i=1;i<=sqlparser_node_count(document);++i) {
    if(sqlparser_get_node(document,(sqlparser_id)i)->kind==SQLPARSER_PARAMETER)
      input=(orm_sql_expr_input){(sqlparser_id)i,{TURBODB_VALUE_INT64,false}};
  }
  check_equal(orm_tidesdb_sql_expr_compile(document,projection->as.projection.expression,&input,1,
      TEST_DEPTH,&budget,&having,&error),TURBODB_STATUS_OK);
  sqlparser_document_destroy(document);
}

spec("TidesDB bounded numeric group reduction") {
  before_each() {
    tdsql_error_init(&error); reserve_calls=resize_calls=fail_reserve=fail_resize=0; fail_sort=false;
    aggregate=(orm_sql_aggregate){0}; downstream=(orm_sql_scan){0}; having=(orm_sql_expr){0};
    limits=(orm_sql_budget_limits){0};
    for(size_t i=0;i<ORM_SQL_BUDGET_RESOURCE_COUNT;++i) limits.statement.value[i]=TEST_LIMIT;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=TEST_WORK;
    limits.transaction=(orm_sql_transaction_budget_amount){TEST_LIMIT,TEST_LIMIT,TEST_LIMIT};
    check_equal(orm_tidesdb_sql_budget_init(&budget,&limits,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
    types[0]=(orm_sql_type){TURBODB_VALUE_INT64,true}; types[1]=(orm_sql_type){TURBODB_VALUE_INT64,true};
    types[2]=(orm_sql_type){TURBODB_VALUE_BOOLEAN,false};
    const int64_t groups[]={2,1,2,3,1,2,3,1,2};
    const int64_t values[]={20,10,5,0,0,30,0,12,5};
    for(size_t i=0;i<TEST_ROWS;++i) {
      rows[i][0]=turbodb_i64(groups[i]); rows[i][1]=values[i]?turbodb_i64(values[i]):turbodb_null(); rows[i][2]=turbodb_bool(i%2!=0);
    }
    keys[0]=0; keys[1]=2; keys[2]=1;
    items[0]=(orm_sql_aggregate_item){ORM_SQL_COUNT_ALL,SIZE_MAX};
    items[1]=(orm_sql_aggregate_item){ORM_SQL_COUNT_VALUE,1};
    items[2]=(orm_sql_aggregate_item){ORM_SQL_MIN,1}; items[3]=(orm_sql_aggregate_item){ORM_SQL_MAX,1};
    input_rows=TEST_ROWS; input_position=calls=fail_read=0;
    source=(orm_sql_row_source){&budget,types,TEST_COLUMNS,&input_position,input_next,false};
  }
  after_each() { clean(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); }

  group("ordinary DISTINCT aggregates") {
    it("counts complete tuples excluding any NULL while preserving ordinary COUNT") {
      items[0]=(orm_sql_aggregate_item){ORM_SQL_COUNT_VALUE,0,2,true};
      items[1]=(orm_sql_aggregate_item){ORM_SQL_COUNT_VALUE,1,1,true};
      items[2]=(orm_sql_aggregate_item){ORM_SQL_COUNT_VALUE,1};
      rows[0][0]=turbodb_null();
      check_equal(open_aggregate(0,3),TURBODB_STATUS_OK); const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,4); check_equal(row.values[1].data.int64_value,5);
      check_equal(row.values[2].data.int64_value,6); check_false(aggregate.source.types[0].nullable);
      check_equal(vec_size(&aggregate.distinct_rows.snapshots),0u); check_equal(vec_size(&aggregate.distinct_order),0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("deduplicates DOUBLE sums and averages independently of ordinary counts") {
      double_items(); items[0].distinct=items[1].distinct=true;
      const double values[]={1,1,2,3,3}; input_rows=6;
      for(size_t i=0;i<5;++i) rows[i][1]=turbodb_f64(values[i]); rows[5][1]=turbodb_null();
      check_equal(open_aggregate(0,3),TURBODB_STATUS_OK); const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.double_value,6.0); check_equal(row.values[1].data.double_value,2.0);
      check_equal(row.values[2].data.int64_value,5);
    }
    it("resets snapshots and reducer state between keyed groups including all NULL") {
      double_items(); items[0].distinct=items[1].distinct=true; items[2].distinct=true;
      check_equal(open_aggregate(1,3),TURBODB_STATUS_OK);
      const double sums[]={22,55}; const int64_t counts[]={2,3};
      for(size_t i=0;i<3;++i) {
        const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,i+1);
        check_equal(row.values[3].data.int64_value,i<2?counts[i]:0);
        if(i<2) { check_equal(row.values[1].data.double_value,sums[i]); check_equal(row.values[2].data.double_value,sums[i]/counts[i]); }
        else { check_equal(row.values[1].kind,TURBODB_VALUE_NULL); check_equal(row.values[2].kind,TURBODB_VALUE_NULL); }
        check_equal(vec_size(&aggregate.distinct_rows.snapshots),0u); check_equal(vec_size(&aggregate.distinct_order),0u);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("returns zero count and NULL sum average for empty global inputs") {
      double_items(); items[0].distinct=items[1].distinct=items[2].distinct=true;
      input_rows=0; check_equal(open_aggregate(0,3),TURBODB_STATUS_OK); const orm_sql_scan_row row=next();
      check_equal(row.values[0].kind,TURBODB_VALUE_NULL); check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
      check_equal(row.values[2].data.int64_value,0); check_equal(next().state,ORM_SQL_SCAN_DONE);
      reset(); check_equal(open_aggregate(1,3),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("preserves U64 extremes and treats signed DOUBLE zero as one tuple") {
      types[0]=(orm_sql_type){TURBODB_VALUE_UINT64,false}; types[1]=(orm_sql_type){TURBODB_VALUE_DOUBLE,true};
      input_rows=4; const uint64_t values[]={UINT64_MAX,UINT64_MAX,UINT64_MAX-1,UINT64_MAX-1};
      for(size_t i=0;i<4;++i) { rows[i][0]=turbodb_u64(values[i]); rows[i][1]=turbodb_f64(i%2?-0.0:0.0); }
      items[0]=(orm_sql_aggregate_item){ORM_SQL_COUNT_VALUE,0,2,true};
      check_equal(open_aggregate(0,1),TURBODB_STATUS_OK); check_equal(next().values[0].data.int64_value,2);
    }
    it("normalizes MIN MAX DISTINCT without any deduplication allocation") {
      items[0]=(orm_sql_aggregate_item){ORM_SQL_MIN,1,1,true}; items[1]=(orm_sql_aggregate_item){ORM_SQL_MAX,1,1,true};
      check_equal(open_aggregate(0,2),TURBODB_STATUS_OK); check_equal(vec_size(&aggregate.distinct_slots),0u);
      const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,5); check_equal(row.values[1].data.int64_value,30);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    }
    it("excludes statically NULL tuple members and deduplicates BOOL arguments") {
      types[0]=(orm_sql_type){TURBODB_VALUE_NULL,true};
      for(size_t i=0;i<TEST_ROWS;++i) rows[i][0]=turbodb_null();
      items[0]=(orm_sql_aggregate_item){ORM_SQL_COUNT_VALUE,0,3,true};
      items[1]=(orm_sql_aggregate_item){ORM_SQL_COUNT_VALUE,2,1,true};
      check_equal(open_aggregate(0,2),TURBODB_STATUS_OK); const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,0); check_equal(row.values[1].data.int64_value,2);
    }
    it("rejects combined DISTINCT argument capacity before allocating or reading") {
      items[0]=(orm_sql_aggregate_item){ORM_SQL_COUNT_VALUE,0,2,true};
      items[1]=(orm_sql_aggregate_item){ORM_SQL_COUNT_VALUE,1,2,true};
      budget.limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES]=TEST_COLUMNS;
      check_equal(open_aggregate(0,2),TURBODB_STATUS_LIMIT_EXCEEDED); check_false(source.active);
      check_equal(reserve_calls,0u); check_equal(calls,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    }
    it("avoids duplicate overflow but still rejects nonfinite unique intermediate sums") {
      double_items(); items[0].distinct=true; input_rows=2; rows[0][1]=rows[1][1]=turbodb_f64(DBL_MAX);
      check_equal(open_aggregate(0,1),TURBODB_STATUS_OK); check_equal(next().values[0].data.double_value,DBL_MAX);
      reset(); input_rows=3; rows[0][1]=turbodb_f64(-DBL_MAX); rows[1][1]=turbodb_f64(-DBL_MAX/2); rows[2][1]=turbodb_f64(DBL_MAX);
      check_equal(open_aggregate(0,1),TURBODB_STATUS_OK); orm_sql_scan_row out={.count=99};
      check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(out.count,99u);
      const size_t reads=calls; check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(calls,reads);
    }
    it("validates every tuple type and argument range before acquiring the source") {
      items[0]=(orm_sql_aggregate_item){ORM_SQL_COUNT_VALUE,1,2,true}; types[2]=(orm_sql_type){TURBODB_VALUE_TEXT,false};
      check_equal(open_aggregate(0,1),TURBODB_STATUS_UNSUPPORTED); check_false(source.active);
      types[2]=(orm_sql_type){TURBODB_VALUE_NULL,false}; check_equal(open_aggregate(0,1),TURBODB_STATUS_TYPE_ERROR);
      types[2]=(orm_sql_type){TURBODB_VALUE_BOOLEAN,false}; items[0].argument_count=3;
      check_equal(open_aggregate(0,1),TURBODB_STATUS_INVALID_ARGUMENT);
      items[0]=(orm_sql_aggregate_item){ORM_SQL_BIT_OR,1,1,true}; check_equal(open_aggregate(0,1),TURBODB_STATUS_UNSUPPORTED);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u); check_equal(calls,0u);
    }
    it("charges simultaneous group sorting and DISTINCT snapshots against materialized rows") {
      items[0]=(orm_sql_aggregate_item){ORM_SQL_COUNT_VALUE,1,1,true};
      check_equal(open_aggregate(1,1),TURBODB_STATUS_OK);
      budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=TEST_ROWS;
      orm_sql_scan_row out={.count=99}; check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(out.count,99u); clean();
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    }
    it("refunds each DISTINCT construction capture and sort allocation failure") {
      double_items(); items[0].distinct=items[1].distinct=true;
      for(size_t keyed=0;keyed<=1;++keyed) {
        reset(); reserve_calls=resize_calls=0; check_equal(open_aggregate(keyed,3),TURBODB_STATUS_OK);
        while(next().state==ORM_SQL_SCAN_ROW) {}
        const size_t reserves=reserve_calls,resizes=resize_calls;
        for(size_t point=1;point<=reserves+resizes;++point) {
          reset(); reserve_calls=resize_calls=0;
          if(point<=reserves) fail_reserve=point; else fail_resize=point-reserves;
          turbodb_status_t status=open_aggregate(keyed,3); orm_sql_scan_row out={.count=99};
          while(status==TURBODB_STATUS_OK) {
            status=orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error);
            if(status==TURBODB_STATUS_OK && out.state!=ORM_SQL_SCAN_ROW) break;
          }
          check_equal(status,TURBODB_STATUS_OUT_OF_MEMORY); clean();
        }
        reset(); check_equal(open_aggregate(keyed,3),TURBODB_STATUS_OK); fail_sort=true; orm_sql_scan_row out={.count=99};
        check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_OUT_OF_MEMORY); check_equal(out.count,99u);
      }
    }
    it("enforces every execution-step boundary for tuple count sum and average") {
      double_items(); items[0].distinct=items[1].distinct=true;
      items[2]=(orm_sql_aggregate_item){ORM_SQL_COUNT_VALUE,0,2,true}; fold_every_step(3);
    }
  }
  group("numeric bit aggregates") {
    it("folds three bit operations as nonnullable U64 without counting NULL") {
      bit_items(); input_rows=4; const int64_t values[]={14,13,11};
      for(size_t i=0;i<3;++i) rows[i][1]=turbodb_i64(values[i]); rows[3][1]=turbodb_null();
      check_equal(open_aggregate(0,3),TURBODB_STATUS_OK); const orm_sql_scan_row row=next(); const uint64_t expected[]={8,15,8};
      for(size_t i=0;i<3;++i) {
        check_equal(aggregate.source.types[i].kind,TURBODB_VALUE_UINT64); check_false(aggregate.source.types[i].nullable);
        check_equal(row.values[i].kind,TURBODB_VALUE_UINT64); check_equal(row.values[i].data.uint64_value,expected[i]);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("retains neutral values for empty all NULL and statically NULL input") {
      bit_items();
      for(size_t mode=0;mode<3;++mode) {
        reset(); input_rows=mode?1:0; rows[0][1]=turbodb_null();
        types[1]=(orm_sql_type){mode==2?TURBODB_VALUE_NULL:TURBODB_VALUE_INT64,true};
        check_equal(open_aggregate(0,3),TURBODB_STATUS_OK); const orm_sql_scan_row row=next();
        for(size_t i=0;i<3;++i) {
          check_equal(row.values[i].kind,TURBODB_VALUE_UINT64); check_equal(row.values[i].data.uint64_value,i?0:UINT64_MAX);
        }
        check_equal(next().state,ORM_SQL_SCAN_DONE);
      }
    }
    it("preserves signed negative bits U64 high bits and BOOL values") {
      bit_items(); input_rows=2; const uint64_t high=(uint64_t)INT64_MAX+1u;
      const uint64_t expected[][3]={{high,UINT64_MAX,(uint64_t)INT64_MAX},{UINT64_MAX-1,UINT64_MAX,1},{0,1,1}};
      for(size_t mode=0;mode<3;++mode) {
        reset(); types[1]=(orm_sql_type){mode==0?TURBODB_VALUE_INT64:mode==1?TURBODB_VALUE_UINT64:TURBODB_VALUE_BOOLEAN,false};
        rows[0][1]=mode==0?turbodb_i64(INT64_MIN):mode==1?turbodb_u64(UINT64_MAX-1):turbodb_bool(false);
        rows[1][1]=mode==0?turbodb_i64(-1):mode==1?turbodb_u64(UINT64_MAX):turbodb_bool(true);
        check_equal(open_aggregate(0,3),TURBODB_STATUS_OK); const orm_sql_scan_row row=next();
        for(size_t i=0;i<3;++i) check_equal(row.values[i].data.uint64_value,expected[mode][i]);
      }
    }
    it("resets bit identities between numeric and all NULL groups") {
      bit_items(); check_equal(open_aggregate(1,3),TURBODB_STATUS_OK);
      const uint64_t expected[][3]={{8,14,6},{4,31,10},{UINT64_MAX,0,0}};
      for(size_t group=0;group<3;++group) {
        const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,(int64_t)group+1);
        for(size_t i=0;i<3;++i) check_equal(row.values[i+1].data.uint64_value,expected[group][i]);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("uses real expression rounding rather than column assignment rounding") {
      bit_items(); input_rows=1; types[1]=(orm_sql_type){TURBODB_VALUE_DOUBLE,false};
      const double values[]={1.5,2.5,-1.5,-2.5,-0.0,(double)INT64_MIN,nextafter(-(double)INT64_MIN,0.0)};
      const uint64_t expected[]={2,2,UINT64_MAX-1,UINT64_MAX-1,0,(uint64_t)INT64_MAX+1u,(uint64_t)INT64_MAX-1023u};
      for(size_t sample=0;sample<sizeof(values)/sizeof(values[0]);++sample) {
        reset(); rows[0][1]=turbodb_f64(values[sample]); check_equal(open_aggregate(0,3),TURBODB_STATUS_OK);
        const orm_sql_scan_row row=next(); for(size_t i=0;i<3;++i) check_equal(row.values[i].data.uint64_value,expected[sample]);
      }
    }
    it("preserves scalar bit state when DOUBLE conversion is outside signed capacity") {
      const double values[]={-(double)INT64_MIN,nextafter((double)INT64_MIN,-INFINITY),DBL_MAX,-DBL_MAX};
      for(size_t kind=ORM_SQL_BIT_AND;kind<=ORM_SQL_BIT_XOR;++kind) {
        for(size_t i=0;i<sizeof(values)/sizeof(values[0]);++i) {
          orm_sql_reduction state=orm_sql_reduction_begin((orm_sql_aggregate_kind)kind);
          const turbodb_value_t first=turbodb_f64(6),invalid=turbodb_f64(values[i]); turbodb_value_t result=turbodb_null();
          check_equal(orm_sql_reduction_add((orm_sql_aggregate_kind)kind,&state,&first,&error),TURBODB_STATUS_OK);
          check_equal(orm_sql_reduction_add((orm_sql_aggregate_kind)kind,&state,&invalid,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
          check_equal(orm_sql_reduction_result((orm_sql_aggregate_kind)kind,&state,&result,&error),TURBODB_STATUS_OK);
          check_equal(result.kind,TURBODB_VALUE_UINT64); check_equal(result.data.uint64_value,6u);
        }
      }
      bit_items(); input_rows=1; types[1]=(orm_sql_type){TURBODB_VALUE_DOUBLE,false}; rows[0][1]=turbodb_f64(NAN);
      check_equal(open_aggregate(0,3),TURBODB_STATUS_OK); orm_sql_scan_row row={.count=99};
      check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&row,&error),TURBODB_STATUS_TYPE_ERROR); check_equal(row.count,99u);
    }
    it("rejects TEXT and binary string evaluation before reading any input") {
      for(size_t kind=ORM_SQL_BIT_AND;kind<=ORM_SQL_BIT_XOR;++kind) {
        for(size_t mode=0;mode<2;++mode) {
          const orm_sql_type input={mode?TURBODB_VALUE_BLOB:TURBODB_VALUE_TEXT,true}; orm_sql_type out={TURBODB_VALUE_BOOLEAN,false};
          check_equal(orm_tidesdb_sql_aggregate_type((orm_sql_aggregate_kind)kind,&input,&out,&error),TURBODB_STATUS_UNSUPPORTED);
          check_equal(out.kind,TURBODB_VALUE_BOOLEAN); check_false(out.nullable);
        }
      }
      check_equal(calls,0u);
    }
    it("refunds every global and grouped bit execution step failure") {
      bit_items(); fold_every_step(3);
    }
  }
  group("variance and standard deviation") {
    it("computes population and sample moments from integer inputs as nullable DOUBLE") {
      moment_items(); input_rows=4;
      for(size_t i=0;i<input_rows;++i) rows[i][1]=turbodb_i64((int64_t)i+1);
      check_equal(open_aggregate(0,4),TURBODB_STATUS_OK); const orm_sql_scan_row row=next();
      const double expected[]={1.25,5.0/3.0,1.1180339887498948,1.2909944487358056};
      for(size_t i=0;i<4;++i) {
        check_equal(aggregate.source.types[i].kind,TURBODB_VALUE_DOUBLE); check_true(aggregate.source.types[i].nullable);
        check_equal(row.values[i].kind,TURBODB_VALUE_DOUBLE); check_less_equal(fabs(row.values[i].data.double_value-expected[i]),1e-12);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("distinguishes empty all NULL and singleton sample results") {
      moment_items();
      for(size_t mode=0;mode<3;++mode) {
        reset(); input_rows=mode?1:0; rows[0][1]=mode==2?turbodb_i64(7):turbodb_null();
        check_equal(open_aggregate(0,4),TURBODB_STATUS_OK); const orm_sql_scan_row row=next();
        for(size_t i=0;i<4;++i) {
          if(mode==2 && i%2==0) { check_equal(row.values[i].kind,TURBODB_VALUE_DOUBLE); check_equal(row.values[i].data.double_value,0.0); }
          else check_equal(row.values[i].kind,TURBODB_VALUE_NULL);
        }
        check_equal(next().state,ORM_SQL_SCAN_DONE);
      }
    }
    it("preserves small variance around a large DOUBLE mean") {
      moment_items(); input_rows=4; types[1]=(orm_sql_type){TURBODB_VALUE_DOUBLE,false};
      for(size_t i=0;i<input_rows;++i) rows[i][1]=turbodb_f64(1e12+(double)i);
      check_equal(open_aggregate(0,4),TURBODB_STATUS_OK); const orm_sql_scan_row row=next();
      check_equal(row.values[0].data.double_value,1.25); check_equal(row.values[1].data.double_value,5.0/3.0);
    }
    it("converts U64 and BOOL inputs to DOUBLE and documents lost integer low bits") {
      moment_items(); input_rows=2;
      for(size_t mode=0;mode<3;++mode) {
        reset(); types[1]=(orm_sql_type){mode==2?TURBODB_VALUE_BOOLEAN:TURBODB_VALUE_UINT64,false};
        rows[0][1]=mode==2?turbodb_bool(false):turbodb_u64(mode?UINT64_MAX-1:0);
        rows[1][1]=mode==2?turbodb_bool(true):turbodb_u64(mode?UINT64_MAX:2);
        check_equal(open_aggregate(0,4),TURBODB_STATUS_OK); const orm_sql_scan_row row=next();
        const double variance=mode==2?0.25:mode?0.0:1.0;
        check_equal(row.values[0].data.double_value,variance); check_equal(row.values[1].data.double_value,variance*2.0);
      }
    }
    it("ignores NULL samples and resets authoritative moments between groups") {
      moment_items(); check_equal(open_aggregate(1,4),TURBODB_STATUS_OK);
      const orm_sql_scan_row first=next(); check_equal(first.values[0].data.int64_value,1);
      check_equal(first.values[1].data.double_value,1.0); check_equal(first.values[2].data.double_value,2.0);
      const orm_sql_scan_row second_row=next(); check_equal(second_row.values[0].data.int64_value,2);
      check_less_equal(fabs(second_row.values[1].data.double_value-112.5),1e-12);
      check_less_equal(fabs(second_row.values[2].data.double_value-150.0),1e-12);
      const orm_sql_scan_row last=next(); check_equal(last.values[0].data.int64_value,3);
      for(size_t i=1;i<last.count;++i) check_equal(last.values[i].kind,TURBODB_VALUE_NULL);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("does not overflow constant DBL_MAX moments but locks overflowing deviations before publication") {
      moment_items(); input_rows=2; types[1]=(orm_sql_type){TURBODB_VALUE_DOUBLE,false};
      rows[0][1]=rows[1][1]=turbodb_f64(DBL_MAX); check_equal(open_aggregate(0,4),TURBODB_STATUS_OK);
      const orm_sql_scan_row row=next(); for(size_t i=0;i<4;++i) check_equal(row.values[i].data.double_value,0.0);
      reset(); rows[1][1]=turbodb_f64(-DBL_MAX); check_equal(open_aggregate(0,4),TURBODB_STATUS_OK);
      orm_sql_scan_row out={.count=99};
      check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(out.count,99u);
      const size_t reads=calls; check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(calls,reads); check_equal(out.count,99u);
    }
    it("preserves mean squared deviation and count on non NULL count overflow") {
      moment_items(); check_equal(open_aggregate(0,4),TURBODB_STATUS_OK); aggregate.pending=rows[0];
      check_equal(aggregate_begin(&aggregate,&error),TURBODB_STATUS_OK);
      orm_sql_reduction *state=vec_at(&aggregate.moments,0);
      state->count=UINT64_MAX; state->mean=3; state->squared_deviation=4;
      check_equal(aggregate_fold(&aggregate,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(state->count,UINT64_MAX); check_equal(state->mean,3.0); check_equal(state->squared_deviation,4.0);
    }
    it("rejects byte statistics at open and preserves the caller type on failure") {
      for(size_t i=0;i<4;++i) {
        const orm_sql_type input={TURBODB_VALUE_TEXT,true}; orm_sql_type output={TURBODB_VALUE_BOOLEAN,false};
        check_equal(orm_tidesdb_sql_aggregate_type((orm_sql_aggregate_kind)(ORM_SQL_VAR_POP+i),&input,&output,&error),TURBODB_STATUS_UNSUPPORTED);
        check_equal(output.kind,TURBODB_VALUE_BOOLEAN); check_false(output.nullable);
      }
      check_equal(calls,0u);
    }
    it("refunds each statistics state allocation when AVG and moments share a group") {
      double_items(); items[2].kind=ORM_SQL_VAR_POP; items[3].kind=ORM_SQL_STDDEV_SAMP;
      reserve_calls=resize_calls=0; check_equal(open_aggregate(1,4),TURBODB_STATUS_OK);
      const size_t reserves=reserve_calls,resizes=resize_calls; clean();
      for(size_t point=1;point<=reserves+resizes;++point) {
        reserve_calls=resize_calls=0;
        if(point<=reserves) fail_reserve=point; else fail_resize=point-reserves;
        check_equal(open_aggregate(1,4),TURBODB_STATUS_OUT_OF_MEMORY); check_null(aggregate.budget); check_false(source.active);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u); clean();
      }
    }
    it("enforces each moment initialization fold and result execution step boundary") {
      moment_items(); fold_every_step(4);
    }
  }
  it("sums doubles and resets nonnull average counts between groups") {
    double_items(); check_equal(open_aggregate(1,4),TURBODB_STATUS_OK);
    const orm_sql_row_source *view=orm_tidesdb_sql_aggregate_source(&aggregate);
    check_equal(view->types[1].kind,TURBODB_VALUE_DOUBLE); check_true(view->types[1].nullable);
    check_equal(view->types[2].kind,TURBODB_VALUE_DOUBLE); check_true(view->types[2].nullable);
    const double totals[]={22.0,60.0}, means[]={11.0,15.0}; const int64_t counts[]={2,4};
    for(size_t i=0;i<2;++i) {
      const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
      check_equal(row.values[0].data.int64_value,(int64_t)i+1);
      check_equal(row.values[1].data.double_value,totals[i]); check_equal(row.values[2].data.double_value,means[i]);
      check_equal(row.values[3].data.int64_value,counts[i]);
    }
    const orm_sql_scan_row nulls=next(); check_equal(nulls.values[0].data.int64_value,3);
    check_equal(nulls.values[1].kind,TURBODB_VALUE_NULL); check_equal(nulls.values[2].kind,TURBODB_VALUE_NULL);
    check_equal(nulls.values[3].data.int64_value,0); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("keeps independent average denominators and performs no global pull allocations") {
    double_items(); types[2]=(orm_sql_type){TURBODB_VALUE_DOUBLE,true};
    for(size_t i=0;i<TEST_ROWS;++i) rows[i][2]=i==0?turbodb_f64(9.0):turbodb_null();
    items[2]=(orm_sql_aggregate_item){ORM_SQL_AVG,2};
    check_equal(open_aggregate(0,4),TURBODB_STATUS_OK); reserve_calls=resize_calls=0; fail_reserve=fail_resize=1;
    const orm_sql_scan_row row=next(); check_equal(row.values[0].data.double_value,82.0);
    check_equal(row.values[1].data.double_value,82.0/6.0); check_equal(row.values[2].data.double_value,9.0);
    check_equal(row.values[3].data.double_value,30.0); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(reserve_calls,0u); check_equal(resize_calls,0u);
  }
  it("returns NULL for empty and all NULL doubles and statically NULL inputs") {
    double_items(); input_rows=0; check_equal(open_aggregate(0,2),TURBODB_STATUS_OK);
    const orm_sql_scan_row empty=next(); check_equal(empty.state,ORM_SQL_SCAN_ROW);
    check_equal(empty.values[0].kind,TURBODB_VALUE_NULL); check_equal(empty.values[1].kind,TURBODB_VALUE_NULL);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); check_equal(open_aggregate(1,2),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_DONE);
    for(size_t mode=0;mode<2;++mode) {
      reset(); input_rows=TEST_ROWS;
      if(mode) types[1].kind=TURBODB_VALUE_NULL;
      for(size_t i=0;i<TEST_ROWS;++i) rows[i][1]=turbodb_null();
      check_equal(open_aggregate(0,2),TURBODB_STATUS_OK); const orm_sql_scan_row row=next();
      check_equal(row.values[0].kind,TURBODB_VALUE_NULL); check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
  }
  it("preserves source order for floating addition and counts zero values") {
    double_items(); input_rows=3;
    rows[0][1]=turbodb_f64(DBL_MAX); rows[1][1]=turbodb_f64(-DBL_MAX); rows[2][1]=turbodb_f64(3.0);
    check_equal(open_aggregate(0,2),TURBODB_STATUS_OK); const orm_sql_scan_row row=next();
    check_equal(row.values[0].data.double_value,3.0); check_equal(row.values[1].data.double_value,1.0);
    reset(); rows[0][1]=turbodb_f64(-0.0); rows[1][1]=turbodb_f64(0.0); rows[2][1]=turbodb_null();
    check_equal(open_aggregate(0,3),TURBODB_STATUS_OK); const orm_sql_scan_row zero=next();
    check_equal(zero.values[0].kind,TURBODB_VALUE_DOUBLE); check_equal(zero.values[1].kind,TURBODB_VALUE_DOUBLE);
    check_equal(zero.values[0].data.double_value,0.0); check_equal(zero.values[1].data.double_value,0.0);
    check_equal(zero.values[2].data.int64_value,2);
  }
  it("locks finite sum overflow and rejects nonfinite inputs without publishing a group") {
    double_items(); input_rows=2;
    for(size_t mode=0;mode<4;++mode) {
      reset(); items[0].kind=mode%2?ORM_SQL_AVG:ORM_SQL_SUM;
      rows[0][1]=turbodb_f64(mode==2?NAN:mode==3?INFINITY:DBL_MAX); rows[1][1]=turbodb_f64(DBL_MAX);
      check_equal(open_aggregate(0,1),TURBODB_STATUS_OK); orm_sql_scan_row out={.count=99};
      const turbodb_status_t expected=mode<2?TURBODB_STATUS_LIMIT_EXCEEDED:TURBODB_STATUS_TYPE_ERROR;
      check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),expected); check_equal(out.count,99u);
      const size_t reads=calls; check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),expected);
      check_equal(calls,reads); check_equal(out.count,99u);
    }
  }
  it("checks average counter overflow before changing its sum or count") {
    double_items(); items[0].kind=ORM_SQL_AVG; check_equal(open_aggregate(0,1),TURBODB_STATUS_OK);
    aggregate.pending=rows[0]; check_equal(aggregate_begin(&aggregate,&error),TURBODB_STATUS_OK);
    *(uint64_t *)vec_at(&aggregate.counts,0)=UINT64_MAX;
    check_equal(aggregate_fold(&aggregate,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(((turbodb_value_t *)vec_at(&aggregate.output,0))->kind,TURBODB_VALUE_NULL);
    check_equal(*(uint64_t *)vec_at(&aggregate.counts,0),UINT64_MAX);
  }
  it("rejects exact SUM AVG and text conversion before reading and preserves type output on failure") {
    const turbodb_value_kind_t kinds[]={TURBODB_VALUE_INT64,TURBODB_VALUE_UINT64,TURBODB_VALUE_BOOLEAN,TURBODB_VALUE_TEXT,TURBODB_VALUE_BLOB};
    for(size_t i=0;i<sizeof(kinds)/sizeof(kinds[0]);++i) {
      for(size_t avg=0;avg<2;++avg) {
        types[1]=(orm_sql_type){kinds[i],true}; items[0]=(orm_sql_aggregate_item){avg?ORM_SQL_AVG:ORM_SQL_SUM,1};
        orm_sql_type out={TURBODB_VALUE_BOOLEAN,false};
        check_equal(orm_tidesdb_sql_aggregate_type(items[0].kind,&types[1],&out,&error),TURBODB_STATUS_UNSUPPORTED);
        check_equal(out.kind,TURBODB_VALUE_BOOLEAN); check_false(out.nullable);
        check_equal(open_aggregate(0,1),TURBODB_STATUS_UNSUPPORTED); check_null(aggregate.budget);
      }
    }
    check_equal(calls,0u);
  }

  it("reduces unsorted input including duplicates NULL arguments and all NULL groups") {
    check_equal(open_aggregate(1,4),TURBODB_STATUS_OK); check_equal(calls,0u); check_true(source.active);
    expect_group(1,3,2,10,12); expect_group(2,4,4,5,30); expect_group(3,2,0,0,0);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(calls,TEST_ROWS+1u); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],TEST_ROWS);
    check_equal(budget.used.value[ORM_SQL_BUDGET_GROUPS],3u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],TEST_ROWS);
  }
  it("folds one global group without materializing input or allocating after open") {
    check_equal(open_aggregate(0,4),TURBODB_STATUS_OK); reserve_calls=resize_calls=0; fail_reserve=fail_resize=1;
    const orm_sql_scan_row row=next(); check_equal(row.count,4u);
    check_equal(row.values[0].data.int64_value,TEST_ROWS); check_equal(row.values[1].data.int64_value,6);
    check_equal(row.values[2].data.int64_value,5); check_equal(row.values[3].data.int64_value,30);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(reserve_calls,0u); check_equal(resize_calls,0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u); check_equal(budget.used.value[ORM_SQL_BUDGET_GROUPS],1u);
  }
  it("distinguishes empty global input from empty grouped input") {
    input_rows=0; check_equal(open_aggregate(0,4),TURBODB_STATUS_OK); const orm_sql_scan_row row=next();
    check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,0);
    check_equal(row.values[1].data.int64_value,0); check_equal(row.values[2].kind,TURBODB_VALUE_NULL);
    check_equal(row.values[3].kind,TURBODB_VALUE_NULL); check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); check_equal(open_aggregate(1,4),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_GROUPS],0u);
  }
  it("groups composite keys and supports grouping without aggregate items") {
    check_equal(open_aggregate(2,0),TURBODB_STATUS_OK);
    const int64_t groups[]={1,1,2,2,3,3}; const int flags[]={0,1,0,1,0,1};
    for(size_t i=0;i<6;++i) {
      const orm_sql_scan_row row=next(); check_equal(row.count,2u);
      check_equal(row.values[0].data.int64_value,groups[i]); check_equal(row.values[1].data.boolean_value,flags[i]);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("groups NULL keys and preserves exact signed and unsigned endpoints") {
    rows[0][0]=turbodb_null(); rows[3][0]=turbodb_null(); check_equal(open_aggregate(1,1),TURBODB_STATUS_OK);
    const orm_sql_scan_row row=next(); check_equal(row.values[0].kind,TURBODB_VALUE_NULL); check_equal(row.values[1].data.int64_value,2);
    reset(); types[0]=(orm_sql_type){TURBODB_VALUE_UINT64,false};
    for(size_t i=0;i<TEST_ROWS;++i) rows[i][0]=turbodb_u64(i%2?UINT64_MAX:(uint64_t)INT64_MAX);
    items[0]=(orm_sql_aggregate_item){ORM_SQL_MIN,0}; items[1]=(orm_sql_aggregate_item){ORM_SQL_MAX,0};
    check_equal(open_aggregate(0,2),TURBODB_STATUS_OK); const orm_sql_scan_row extrema=next();
    check_equal(extrema.values[0].data.uint64_value,(uint64_t)INT64_MAX); check_equal(extrema.values[1].data.uint64_value,UINT64_MAX);
    reset(); types[0]=(orm_sql_type){TURBODB_VALUE_INT64,false};
    for(size_t i=0;i<TEST_ROWS;++i) rows[i][0]=turbodb_i64(i%2?INT64_MAX:INT64_MIN);
    check_equal(open_aggregate(0,2),TURBODB_STATUS_OK); const orm_sql_scan_row signed_values=next();
    check_equal(signed_values.values[0].data.int64_value,INT64_MIN); check_equal(signed_values.values[1].data.int64_value,INT64_MAX);
  }
  it("groups positive and negative floating zero together and compares finite extrema") {
    types[0]=(orm_sql_type){TURBODB_VALUE_DOUBLE,false};
    for(size_t i=0;i<TEST_ROWS;++i) rows[i][0]=turbodb_f64(i%2?0.0:-0.0);
    rows[0][0]=turbodb_f64(-1.0); rows[8][0]=turbodb_f64(2.0);
    items[1]=(orm_sql_aggregate_item){ORM_SQL_MIN,0}; items[2]=(orm_sql_aggregate_item){ORM_SQL_MAX,0};
    check_equal(open_aggregate(1,3),TURBODB_STATUS_OK);
    check_equal(next().values[0].data.double_value,-1.0);
    const orm_sql_scan_row zero=next(); check_equal(zero.values[0].data.double_value,0.0); check_equal(zero.values[1].data.int64_value,7);
    check_equal(zero.values[2].data.double_value,0.0); check_equal(zero.values[3].data.double_value,0.0);
    check_equal(next().values[0].data.double_value,2.0); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("counts TEXT and BLOB nullness without retaining byte outputs or ordering their payloads") {
    types[1]=(orm_sql_type){TURBODB_VALUE_TEXT,true};
    for(size_t i=0;i<TEST_ROWS;++i) rows[i][1]=i%2?turbodb_text(""):turbodb_null();
    check_equal(open_aggregate(0,2),TURBODB_STATUS_OK); check_equal(next().values[1].data.int64_value,4);
    reset(); types[1]=(orm_sql_type){TURBODB_VALUE_BLOB,true};
    for(size_t i=0;i<TEST_ROWS;++i) rows[i][1]=i%2?turbodb_blob(NULL,0):turbodb_null();
    check_equal(open_aggregate(0,2),TURBODB_STATUS_OK); check_equal(next().values[1].data.int64_value,4);
  }
  it("copies specification and exposes nullable extrema metadata independent of input nullability") {
    types[1].nullable=false;
    for(size_t i=0;i<TEST_ROWS;++i) rows[i][1]=turbodb_i64((int64_t)i);
    check_equal(open_aggregate(1,4),TURBODB_STATUS_OK); keys[0]=SIZE_MAX; items[2].slot=SIZE_MAX;
    orm_sql_row_source *view=orm_tidesdb_sql_aggregate_source(&aggregate); check_not_null(view);
    check_equal(view->columns,5u); check_equal(view->types[1].kind,TURBODB_VALUE_INT64); check_false(view->types[1].nullable);
    check_equal(view->types[3].kind,TURBODB_VALUE_INT64); check_true(view->types[3].nullable);
    check_equal(next().values[3].data.int64_value,1);
  }
  it("reduces boolean extrema and handles statically NULL input types") {
    items[0]=(orm_sql_aggregate_item){ORM_SQL_MIN,2}; items[1]=(orm_sql_aggregate_item){ORM_SQL_MAX,2};
    check_equal(open_aggregate(0,2),TURBODB_STATUS_OK); const orm_sql_scan_row flags=next();
    check_equal(flags.values[0].kind,TURBODB_VALUE_BOOLEAN); check_equal(flags.values[0].data.boolean_value,0);
    check_equal(flags.values[1].data.boolean_value,1);
    reset(); types[1]=(orm_sql_type){TURBODB_VALUE_NULL,true};
    for(size_t i=0;i<TEST_ROWS;++i) rows[i][1]=turbodb_null();
    items[0]=(orm_sql_aggregate_item){ORM_SQL_COUNT_VALUE,1}; items[1]=(orm_sql_aggregate_item){ORM_SQL_MIN,1};
    items[2]=(orm_sql_aggregate_item){ORM_SQL_MAX,1}; keys[0]=1;
    check_equal(open_aggregate(1,3),TURBODB_STATUS_OK); const orm_sql_scan_row nulls=next();
    check_equal(nulls.values[0].kind,TURBODB_VALUE_NULL); check_equal(nulls.values[1].data.int64_value,0);
    check_equal(nulls.values[2].kind,TURBODB_VALUE_NULL); check_equal(nulls.values[3].kind,TURBODB_VALUE_NULL);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("validates arguments and collation boundaries without reading input") {
    orm_sql_aggregate_spec spec={keys,0,items,0};
    check_equal(orm_tidesdb_sql_aggregate_open(&source,&spec,&aggregate,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_aggregate_open(NULL,&spec,&aggregate,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    keys[0]=TEST_COLUMNS; check_equal(open_aggregate(1,1),TURBODB_STATUS_INVALID_ARGUMENT); keys[0]=0;
    items[0]=(orm_sql_aggregate_item){ORM_SQL_COUNT_VALUE,TEST_COLUMNS}; check_equal(open_aggregate(0,1),TURBODB_STATUS_INVALID_ARGUMENT);
    items[0]=(orm_sql_aggregate_item){(orm_sql_aggregate_kind)99,0}; check_equal(open_aggregate(0,1),TURBODB_STATUS_UNSUPPORTED);
    types[0]=(orm_sql_type){TURBODB_VALUE_TEXT,false}; check_equal(open_aggregate(1,0),TURBODB_STATUS_UNSUPPORTED);
    items[0]=(orm_sql_aggregate_item){ORM_SQL_MIN,0}; check_equal(open_aggregate(0,1),TURBODB_STATUS_UNSUPPORTED);
    items[0]=(orm_sql_aggregate_item){ORM_SQL_COUNT_ALL,SIZE_MAX};
    source.active=true; check_equal(open_aggregate(0,1),TURBODB_STATUS_BUSY); source.active=false;
    spec=(orm_sql_aggregate_spec){keys,SIZE_MAX,items,1};
    check_equal(orm_tidesdb_sql_aggregate_open(&source,&spec,&aggregate,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(calls,0u); check_null(aggregate.budget);
  }
  it("checks count overflow before changing its accumulator") {
    check_equal(open_aggregate(0,1),TURBODB_STATUS_OK);
    aggregate.pending=rows[0]; check_equal(aggregate_begin(&aggregate,&error),TURBODB_STATUS_OK);
    ((turbodb_value_t *)vec_at(&aggregate.output,0))->data.int64_value=INT64_MAX;
    check_equal(aggregate_fold(&aggregate,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(((turbodb_value_t *)vec_at(&aggregate.output,0))->data.int64_value,INT64_MAX);
  }
  it("locks source errors without publishing an unfinished global group") {
    fail_read=TEST_ROWS; check_equal(open_aggregate(0,4),TURBODB_STATUS_OK); orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED,.count=99};
    check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(out.state,ORM_SQL_SCAN_CANCELLED); check_equal(out.count,99u); check_equal(calls,TEST_ROWS);
    check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_DATASTORE_ERROR); check_equal(calls,TEST_ROWS);
    check_equal(orm_tidesdb_sql_aggregate_cancel(&aggregate,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_DATASTORE_ERROR);
  }
  it("rejects nonfinite keys and mistyped aggregate values instead of folding them") {
    for(size_t mode=0;mode<2;++mode) {
      reset(); if(mode) rows[0][1]=turbodb_bool(true);
      else { types[0]=(orm_sql_type){TURBODB_VALUE_DOUBLE,true}; rows[0][0]=turbodb_f64(NAN); }
      check_equal(open_aggregate(mode?0:1,4),TURBODB_STATUS_OK); orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED};
      check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_TYPE_ERROR);
      check_equal(out.state,ORM_SQL_SCAN_CANCELLED);
      types[0]=(orm_sql_type){TURBODB_VALUE_INT64,true}; rows[0][0]=turbodb_i64(2);
    }
  }
  it("cancels without pulling and retains a completed or cancelled terminal") {
    check_equal(open_aggregate(1,4),TURBODB_STATUS_OK); check_equal(orm_tidesdb_sql_aggregate_cancel(&aggregate,&error),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_CANCELLED); check_equal(next().state,ORM_SQL_SCAN_CANCELLED); check_equal(calls,0u);
    reset(); input_rows=0; check_equal(open_aggregate(1,4),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(orm_tidesdb_sql_aggregate_cancel(&aggregate,&error),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("composes HAVING filtering sorting and pagination without charging group rows as physical reads") {
    check_equal(open_aggregate(1,1),TURBODB_STATUS_OK); build_having();
    const size_t filter_slot=1, projection[]={0,1}; const orm_sql_scan_order order={.slot=1,.descending=true};
    const orm_sql_scan_spec spec={.filter=&having,.filter_slots=&filter_slot,.filter_count=1,
        .projection=projection,.projection_count=2,.orders=&order,.order_count=1,.offset=1,.limit=1};
    check_equal(orm_tidesdb_sql_scan_open_source(orm_tidesdb_sql_aggregate_source(&aggregate),&spec,&budget,&downstream,&error),TURBODB_STATUS_OK);
    orm_sql_scan_row out={0}; check_equal(orm_tidesdb_sql_scan_next(&downstream,&out,&error),TURBODB_STATUS_OK);
    check_equal(out.state,ORM_SQL_SCAN_ROW); check_equal(out.values[0].data.int64_value,1); check_equal(out.values[1].data.int64_value,3);
    check_equal(orm_tidesdb_sql_scan_next(&downstream,&out,&error),TURBODB_STATUS_OK); check_equal(out.state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],TEST_ROWS); check_equal(calls,TEST_ROWS+1u);
  }
  it("holds input and output leases until downstream closes even after EOF") {
    check_equal(open_aggregate(0,1),TURBODB_STATUS_OK); const size_t projection=0;
    const orm_sql_scan_spec spec={.projection=&projection,.projection_count=1,.limit=UINT64_MAX};
    check_equal(orm_tidesdb_sql_scan_open_source(orm_tidesdb_sql_aggregate_source(&aggregate),&spec,&budget,&downstream,&error),TURBODB_STATUS_OK);
    orm_sql_scan_row out={.count=99};
    check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_BUSY); check_equal(out.count,99u);
    check_equal(orm_tidesdb_sql_aggregate_cancel(&aggregate,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_aggregate_close(&aggregate,&error),TURBODB_STATUS_BUSY); check_true(source.active);
    check_equal(orm_tidesdb_sql_scan_next(&downstream,&out,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_scan_next(&downstream,&out,&error),TURBODB_STATUS_OK); check_equal(out.state,ORM_SQL_SCAN_DONE);
    check_equal(orm_tidesdb_sql_aggregate_close(&aggregate,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_scan_close(&downstream,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_aggregate_close(&aggregate,&error),TURBODB_STATUS_OK); check_false(source.active);
    check_null(orm_tidesdb_sql_aggregate_source(&aggregate));
  }
  it("does no input reads when downstream LIMIT is zero") {
    check_equal(open_aggregate(1,1),TURBODB_STATUS_OK); const size_t projection=0;
    const orm_sql_scan_spec spec={.projection=&projection,.projection_count=1,.limit=0};
    check_equal(orm_tidesdb_sql_scan_open_source(orm_tidesdb_sql_aggregate_source(&aggregate),&spec,&budget,&downstream,&error),TURBODB_STATUS_OK);
    orm_sql_scan_row out={0}; check_equal(orm_tidesdb_sql_scan_next(&downstream,&out,&error),TURBODB_STATUS_OK);
    check_equal(out.state,ORM_SQL_SCAN_DONE); check_equal(calls,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_GROUPS],0u);
  }
  it("enforces total admitted group capacity and refunds it only at close") {
    budget.limits.statement.value[ORM_SQL_BUDGET_GROUPS]=2;
    check_equal(open_aggregate(1,4),TURBODB_STATUS_OK); expect_group(1,3,2,10,12); expect_group(2,4,4,5,30);
    orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(out.state,ORM_SQL_SCAN_CANCELLED); check_equal(budget.used.value[ORM_SQL_BUDGET_GROUPS],2u);
    clean(); check_equal(budget.used.value[ORM_SQL_BUDGET_GROUPS],0u);
  }
  it("counts an empty global group against the shared group budget") {
    orm_sql_budget_amount full={0}; full.value[ORM_SQL_BUDGET_GROUPS]=limits.statement.value[ORM_SQL_BUDGET_GROUPS];
    check_equal(orm_tidesdb_sql_budget_reserve(&budget,&full,&error),TURBODB_STATUS_OK);
    input_rows=0; check_equal(open_aggregate(0,1),TURBODB_STATUS_OK); orm_sql_scan_row out={.count=99};
    check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(out.count,99u);
    check_equal(orm_tidesdb_sql_budget_release(&budget,ORM_SQL_BUDGET_GROUPS,full.value[ORM_SQL_BUDGET_GROUPS],&error),TURBODB_STATUS_OK);
  }
  it("rejects sorting materialization exhaustion before returning a group") {
    budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=TEST_ROWS-1;
    check_equal(open_aggregate(1,4),TURBODB_STATUS_OK); orm_sql_scan_row out={.count=99};
    check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(out.count,99u);
  }
  it("preserves an emitted row and leases when control calls are rejected") {
    check_equal(open_aggregate(1,4),TURBODB_STATUS_OK); const orm_sql_scan_row row=next();
    check_equal(open_aggregate(1,4),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,NULL,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(row.values[0].data.int64_value,1); check_true(source.active);
    check_equal(orm_tidesdb_sql_aggregate_cancel(&aggregate,&error),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_CANCELLED); check_true(source.active);
    check_equal(budget.used.value[ORM_SQL_BUDGET_GROUPS],1u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],TEST_ROWS);
  }
  it("enforces each execution-step boundary through keyed and global folding without leaking resources") {
    double_items(); fold_every_step(4);
  }
  it("fails workspace exhaustion during grouped materialization and can reopen after close") {
    check_equal(open_aggregate(1,4),TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    orm_sql_scan_row out={.count=99};
    check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,NULL),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(out.count,99u);
    check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_contains(error.message,"aggregate"); reset(); check_equal(open_aggregate(1,4),TURBODB_STATUS_OK);
    expect_group(1,3,2,10,12);
  }
  it("refunds each open allocation failure and never leaves the source active") {
    double_items();
    reserve_calls=resize_calls=0; check_equal(open_aggregate(1,4),TURBODB_STATUS_OK);
    const size_t reserves=reserve_calls,resizes=resize_calls; clean();
    for(size_t i=1;i<=reserves+resizes;++i) {
      reserve_calls=resize_calls=0;
      if(i<=reserves) fail_reserve=i; else fail_resize=i-reserves;
      check_equal(open_aggregate(1,4),TURBODB_STATUS_OUT_OF_MEMORY); check_null(aggregate.budget); check_false(source.active);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u); check_equal(calls,0u);
      fail_reserve=fail_resize=0;
    }
  }
  it("refunds each grouped scan capture allocation and stable sort failure") {
    check_equal(open_aggregate(1,4),TURBODB_STATUS_OK); reserve_calls=resize_calls=0; check_equal(next().state,ORM_SQL_SCAN_ROW);
    const size_t reserves=reserve_calls,resizes=resize_calls; clean();
    for(size_t i=0;i<=reserves+resizes;++i) {
      input_position=calls=0; check_equal(open_aggregate(1,4),TURBODB_STATUS_OK); reserve_calls=resize_calls=0;
      if(!i) fail_sort=true; else if(i<=reserves) fail_reserve=i; else fail_resize=i-reserves;
      orm_sql_scan_row out={.count=99};
      check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_OUT_OF_MEMORY); check_equal(out.count,99u);
      const size_t reads=calls, allocations=reserve_calls;
      check_equal(orm_tidesdb_sql_aggregate_next(&aggregate,&out,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      check_equal(calls,reads); check_equal(reserve_calls,allocations);
      clean();
    }
  }
}
