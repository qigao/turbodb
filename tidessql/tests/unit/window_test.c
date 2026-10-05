#include "window.h"
#include <tinytest.h>
#include <cstl/sort.h>
#include <math.h>
#include <float.h>
#include <string.h>

static size_t reserve_calls, resize_calls, fail_reserve, fail_resize;
static bool fail_sort;
static stl_status window_test_reserve(vec_t *v, size_t count) {
  return ++reserve_calls == fail_reserve ? STL_OUT_OF_MEMORY : vec_reserve(v,count);
}
static stl_status window_test_resize(vec_t *v, size_t count) {
  return ++resize_calls == fail_resize ? STL_OUT_OF_MEMORY : vec_resize(v,count);
}
static stl_status window_test_sort(void *base, size_t count, const cmeta_type_desc *type, size_t bytes) {
  return fail_sort ? STL_OUT_OF_MEMORY : stable_sort(base,count,type,bytes);
}
#define vec_reserve window_test_reserve
#define vec_resize window_test_resize
#define stable_sort window_test_sort
#include "../../src/work.c"
#include "../../src/rows.c"
#include "../../src/scan.c"
#include "../../src/window.c"
#undef vec_reserve
#undef vec_resize
#undef stable_sort

enum { TEST_ROWS=9, TEST_COLUMNS=3, TEST_LIMIT=65536, TEST_WORK=4*1024*1024 };
static orm_tidesdb_sql_budget budget;
static orm_sql_budget_limits limits;
static turbodb_error_t error;
static orm_sql_window window, second;
static orm_sql_scan consumer;
static orm_sql_type types[TEST_COLUMNS];
static turbodb_value_t rows[TEST_ROWS][TEST_COLUMNS], buffer[TEST_COLUMNS];
static char payload[2];
static size_t input_rows, input_position, calls, fail_read;
static bool reenter, bare_error;
static orm_sql_row_source source;
static size_t partitions[TEST_COLUMNS];
static orm_sql_scan_order orders[TEST_COLUMNS];
static orm_sql_window_spec spec_value;

static turbodb_status_t input_next(void *context, const turbodb_value_t **out, turbodb_error_t *e) {
  (void)context; ++calls;
  if (reenter) {
    turbodb_error_t busy; tdsql_error_init(&busy);
    const orm_sql_scan_row saved={ORM_SQL_SCAN_DONE,rows[0],SIZE_MAX}; orm_sql_scan_row result=saved;
    check_equal(orm_tidesdb_sql_window_next(&window,&result,&busy),TURBODB_STATUS_BUSY);
    check_equal((const void *)result.values,(const void *)saved.values); check_equal(result.count,saved.count);
    check_equal(orm_tidesdb_sql_window_cancel(&window,&busy),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_window_close(&window,&busy),TURBODB_STATUS_BUSY);
    check_true(source.active); check_true(window.evaluating);
  }
  if (calls == fail_read) {
    if (!bare_error) tdsql_error_set(e,TURBODB_STATUS_DATASTORE_ERROR,"window test read failure");
    return TURBODB_STATUS_DATASTORE_ERROR;
  }
  memset(buffer,0,sizeof(buffer)); memset(payload,0,sizeof(payload));
  if (input_position == input_rows) { *out=NULL; return TURBODB_STATUS_OK; }
  orm_sql_budget_amount amount={0}; amount.value[ORM_SQL_BUDGET_READ_ROWS]=1;
  amount.value[ORM_SQL_BUDGET_READ_BYTES]=sizeof(buffer);
  const turbodb_status_t status=orm_tidesdb_sql_budget_reserve(&budget,&amount,e);
  if (status != TURBODB_STATUS_OK) return status;
  memcpy(buffer,rows[input_position],sizeof(buffer));
  if (types[2].kind==TURBODB_VALUE_TEXT || types[2].kind==TURBODB_VALUE_BLOB) {
    payload[0]=(char)('a'+input_position);
    buffer[2]=types[2].kind==TURBODB_VALUE_TEXT ? turbodb_text_v((vstr){payload,1}) : turbodb_blob(payload,1);
  }
  ++input_position; *out=buffer; return TURBODB_STATUS_OK;
}
static turbodb_status_t open_window(void) {
  return orm_tidesdb_sql_window_open(&source,&spec_value,&window,&error);
}
static orm_sql_scan_row next_window(void) {
  orm_sql_scan_row out={0}; check_equal(orm_tidesdb_sql_window_next(&window,&out,&error),TURBODB_STATUS_OK); return out;
}
static void clean(void) {
  fail_reserve=fail_resize=0; fail_sort=false;
  check_equal(orm_tidesdb_sql_scan_close(&consumer,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_window_close(&second,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_window_close(&window,&error),TURBODB_STATUS_OK);
  check_false(source.active);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
  check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
  check_equal(budget.used.value[ORM_SQL_BUDGET_GROUPS],0u);
}
static void reset(void) {
  clean(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_init(&budget,&limits,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
  input_position=calls=fail_read=0; reserve_calls=resize_calls=0;
}
static void expect_integer(const int64_t *expected, size_t count) {
  for(size_t i=0;i<count;++i) {
    const orm_sql_scan_row row=next_window(); check_equal(row.state,ORM_SQL_SCAN_ROW);
    check_equal(row.count,TEST_COLUMNS+1u); check_equal(row.values[TEST_COLUMNS].kind,TURBODB_VALUE_INT64);
    check_equal(row.values[TEST_COLUMNS].data.int64_value,expected[i]);
  }
  check_equal(next_window().state,ORM_SQL_SCAN_DONE);
  const size_t reads=calls; const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
  check_equal(next_window().state,ORM_SQL_SCAN_DONE); check_equal(calls,reads);
  check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
}
static void expect_distribution(const double *expected, size_t count) {
  for(size_t i=0;i<count;++i) {
    const orm_sql_scan_row row=next_window(); check_equal(row.state,ORM_SQL_SCAN_ROW);
    check_equal(row.values[TEST_COLUMNS].kind,TURBODB_VALUE_DOUBLE);
    check_equal(row.values[TEST_COLUMNS].data.double_value,expected[i]);
  }
  check_equal(next_window().state,ORM_SQL_SCAN_DONE);
}
static void expect_locked(turbodb_status_t expected) {
  const orm_sql_scan_row saved={ORM_SQL_SCAN_DONE,rows[0],SIZE_MAX}; orm_sql_scan_row row=saved;
  check_equal(orm_tidesdb_sql_window_next(&window,&row,&error),expected);
  check_equal((const void *)row.values,(const void *)saved.values); check_equal(row.count,saved.count); check_equal(row.state,saved.state);
  check_equal(error.status,expected); check_equal(window.state,ORM_SQL_SCAN_ERROR);
  const size_t reads=calls; const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
  char message[TURBODB_ERROR_MESSAGE_CAPACITY]; memcpy(message,error.message,sizeof(message));
  fail_read=0; fail_reserve=fail_resize=0; fail_sort=false;
  check_equal(orm_tidesdb_sql_window_next(&window,&row,&error),expected);
  check_equal(error.message,message); check_equal(calls,reads);
  check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
  check_equal(orm_tidesdb_sql_window_cancel(&window,&error),TURBODB_STATUS_OK);
  check_equal(window.state,ORM_SQL_SCAN_ERROR);
}

spec("TidesDB bounded window ranks and distributions") {
  before_each() {
    tdsql_error_init(&error); reserve_calls=resize_calls=fail_reserve=fail_resize=0; fail_sort=false;
    window=(orm_sql_window){0}; second=(orm_sql_window){0}; consumer=(orm_sql_scan){0};
    limits=(orm_sql_budget_limits){0};
    for(size_t i=0;i<ORM_SQL_BUDGET_RESOURCE_COUNT;++i) limits.statement.value[i]=TEST_LIMIT;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=TEST_WORK;
    limits.transaction=(orm_sql_transaction_budget_amount){TEST_LIMIT,TEST_LIMIT,TEST_LIMIT};
    check_equal(orm_tidesdb_sql_budget_init(&budget,&limits,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
    const int64_t values[]={3,1,4,3,2,5,1,4,3};
    for(size_t i=0;i<TEST_COLUMNS;++i) types[i]=(orm_sql_type){TURBODB_VALUE_INT64,true};
    for(size_t i=0;i<TEST_ROWS;++i) {
      rows[i][0]=turbodb_i64(values[i]); rows[i][1]=turbodb_i64(0); rows[i][2]=turbodb_i64((int64_t)i);
    }
    partitions[0]=1; partitions[1]=2; partitions[2]=0;
    for(size_t i=0;i<TEST_COLUMNS;++i) orders[i]=(orm_sql_scan_order){.slot=i};
    spec_value=(orm_sql_window_spec){.kind=ORM_SQL_ROW_NUMBER,.partitions=partitions,.partition_count=0,
        .orders=orders,.order_count=1};
    input_rows=TEST_ROWS; input_position=calls=fail_read=0; reenter=bare_error=false;
    source=(orm_sql_row_source){&budget,types,TEST_COLUMNS,&input_position,input_next,false};
  }
  after_each() { clean(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); }

  group("numeric bit window frames") {
    it("ignores NULL prefix inputs while preserving AND OR XOR identities") {
      input_rows=4; orders[0].slot=2;
      rows[0][0]=turbodb_null(); rows[1][0]=turbodb_i64(14); rows[2][0]=turbodb_i64(13); rows[3][0]=turbodb_i64(11);
      const uint64_t expected[][4]={{UINT64_MAX,14,12,8},{0,14,15,15},{0,14,3,8}};
      for(size_t mode=0;mode<3;++mode) {
        reset(); spec_value.kind=(orm_sql_window_kind)(ORM_SQL_WINDOW_BIT_AND+mode);
        check_equal(open_window(),TURBODB_STATUS_OK); const orm_sql_type type=window.source.types[TEST_COLUMNS];
        check_equal(type.kind,TURBODB_VALUE_UINT64); check_false(type.nullable);
        for(size_t i=0;i<input_rows;++i) {
          const orm_sql_scan_row row=next_window(); check_equal(row.values[TEST_COLUMNS].kind,TURBODB_VALUE_UINT64);
          check_equal(row.values[TEST_COLUMNS].data.uint64_value,expected[mode][i]);
        }
        check_equal(next_window().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_GROUPS],0u);
      }
    }
    it("recomputes moving starts without retaining bits from previous frames") {
      input_rows=4; orders[0].slot=2; const int64_t values[]={14,13,11,7};
      for(size_t i=0;i<input_rows;++i) rows[i][0]=turbodb_i64(values[i]);
      spec_value.frame=(orm_sql_window_frame){ORM_SQL_FRAME_ROWS,{{ORM_SQL_BOUND_PRECEDING,turbodb_i64(1)},{ORM_SQL_BOUND_CURRENT_ROW,{0}}}};
      const uint64_t expected[][4]={{14,12,9,3},{14,15,15,15},{14,3,6,12}};
      for(size_t mode=0;mode<3;++mode) {
        reset(); spec_value.kind=(orm_sql_window_kind)(ORM_SQL_WINDOW_BIT_AND+mode); check_equal(open_window(),TURBODB_STATUS_OK);
        for(size_t i=0;i<input_rows;++i) check_equal(next_window().values[TEST_COLUMNS].data.uint64_value,expected[mode][i]);
        check_equal(next_window().state,ORM_SQL_SCAN_DONE);
      }
    }
    it("reuses a whole partition XOR result without folding peer frames twice") {
      input_rows=3; types[0]=(orm_sql_type){TURBODB_VALUE_UINT64,false}; spec_value.order_count=0;
      rows[0][0]=turbodb_u64(UINT64_MAX); rows[1][0]=turbodb_u64(UINT64_MAX-1); rows[2][0]=turbodb_u64(2);
      spec_value.kind=ORM_SQL_WINDOW_BIT_XOR; check_equal(open_window(),TURBODB_STATUS_OK);
      for(size_t i=0;i<input_rows;++i) check_equal(next_window().values[TEST_COLUMNS].data.uint64_value,3u);
      check_equal(next_window().state,ORM_SQL_SCAN_DONE);
    }
    it("resets all NULL partition identities and includes default RANGE peers") {
      input_rows=4; spec_value.partition_count=1; orders[0].slot=2;
      rows[0][0]=rows[1][0]=turbodb_null(); rows[2][0]=turbodb_i64(6); rows[3][0]=turbodb_i64(3);
      rows[2][1]=rows[3][1]=turbodb_i64(1); rows[2][2]=rows[3][2]=turbodb_i64(2);
      const uint64_t expected[]={2,7,5};
      for(size_t mode=0;mode<3;++mode) {
        reset(); spec_value.kind=(orm_sql_window_kind)(ORM_SQL_WINDOW_BIT_AND+mode); check_equal(open_window(),TURBODB_STATUS_OK);
        for(size_t i=0;i<input_rows;++i) check_equal(next_window().values[TEST_COLUMNS].data.uint64_value,i<2?(mode?0:UINT64_MAX):expected[mode]);
        check_equal(next_window().state,ORM_SQL_SCAN_DONE);
      }
    }
    it("locks DOUBLE conversion errors before publishing any prefix result") {
      input_rows=2; orders[0].slot=2; types[0]=(orm_sql_type){TURBODB_VALUE_DOUBLE,false}; rows[0][0]=turbodb_f64(2.5);
      for(size_t mode=0;mode<3;++mode) {
        for(size_t input=0;input<2;++input) {
          reset(); spec_value.kind=(orm_sql_window_kind)(ORM_SQL_WINDOW_BIT_AND+mode);
          rows[1][0]=turbodb_f64(input?INFINITY:-(double)INT64_MIN); check_equal(open_window(),TURBODB_STATUS_OK);
          expect_locked(input?TURBODB_STATUS_TYPE_ERROR:TURBODB_STATUS_LIMIT_EXCEEDED);
        }
      }
    }
  }
  group("statistical window frames") {
    it("keeps population and sample prefix results independent of cached means") {
      input_rows=4; orders[0].slot=2;
      for(size_t i=0;i<input_rows;++i) rows[i][0]=turbodb_i64((int64_t)i+1);
      const double population[]={0,0.25,2.0/3.0,1.25},sample[]={0,0.5,1,5.0/3.0};
      for(size_t kind=ORM_SQL_WINDOW_VAR_POP;kind<=ORM_SQL_WINDOW_STDDEV_SAMP;++kind) {
        reset(); spec_value.kind=(orm_sql_window_kind)kind; check_equal(open_window(),TURBODB_STATUS_OK);
        const bool sampled=kind==ORM_SQL_WINDOW_VAR_SAMP || kind==ORM_SQL_WINDOW_STDDEV_SAMP;
        for(size_t i=0;i<input_rows;++i) {
          const turbodb_value_t result=next_window().values[TEST_COLUMNS];
          if(!i && sampled) check_equal(result.kind,TURBODB_VALUE_NULL);
          else {
            double expected=sampled?sample[i]:population[i]; if(kind>=ORM_SQL_WINDOW_STDDEV_POP) expected=sqrt(expected);
            check_equal(result.kind,TURBODB_VALUE_DOUBLE); check_less_equal(fabs(result.data.double_value-expected),1e-12);
          }
        }
        check_equal(next_window().state,ORM_SQL_SCAN_DONE);
      }
    }
    it("resets moments for moving ROWS frames rather than reversing floating updates") {
      input_rows=4; orders[0].slot=2; types[0]=(orm_sql_type){TURBODB_VALUE_DOUBLE,false};
      const double values[]={1e12,1e12+2,1e12+6,1e12+8},variance[]={0,1,4,1};
      for(size_t i=0;i<input_rows;++i) rows[i][0]=turbodb_f64(values[i]);
      spec_value.kind=ORM_SQL_WINDOW_VAR_POP;
      spec_value.frame=(orm_sql_window_frame){.unit=ORM_SQL_FRAME_ROWS,.boundaries={
          {ORM_SQL_BOUND_PRECEDING,turbodb_i64(1)},{ORM_SQL_BOUND_CURRENT_ROW,{0}}}};
      check_equal(open_window(),TURBODB_STATUS_OK); expect_distribution(variance,input_rows);
    }
    it("reuses full partition standard deviation without replacing its moments by the result") {
      input_rows=3; spec_value.order_count=0; spec_value.kind=ORM_SQL_WINDOW_STDDEV_POP;
      rows[0][0]=turbodb_i64(2); rows[1][0]=turbodb_i64(2); rows[2][0]=turbodb_i64(5);
      check_equal(open_window(),TURBODB_STATUS_OK);
      for(size_t i=0;i<input_rows;++i) {
        const turbodb_value_t result=next_window().values[TEST_COLUMNS]; check_equal(result.kind,TURBODB_VALUE_DOUBLE);
        check_less_equal(fabs(result.data.double_value-1.4142135623730951),1e-12);
      }
      check_equal(next_window().state,ORM_SQL_SCAN_DONE);
    }
    it("includes peers in a default frame and ignores NULL samples in each partition") {
      input_rows=4; spec_value.value_slot=2; spec_value.partition_count=1; spec_value.kind=ORM_SQL_WINDOW_VAR_POP;
      rows[0][0]=rows[1][0]=turbodb_i64(1); rows[2][0]=rows[3][0]=turbodb_i64(1);
      rows[0][2]=turbodb_i64(2); rows[1][2]=turbodb_i64(4); rows[2][2]=turbodb_null(); rows[3][2]=turbodb_i64(10);
      rows[2][1]=rows[3][1]=turbodb_i64(1);
      check_equal(open_window(),TURBODB_STATUS_OK); const double expected[]={1,1,0,0}; expect_distribution(expected,input_rows);
    }
    it("locks a statistical overflow before publishing a prepared window row") {
      input_rows=2; orders[0].slot=2; types[0]=(orm_sql_type){TURBODB_VALUE_DOUBLE,false};
      rows[0][0]=turbodb_f64(DBL_MAX); rows[1][0]=turbodb_f64(-DBL_MAX);
      for(size_t kind=ORM_SQL_WINDOW_VAR_POP;kind<=ORM_SQL_WINDOW_STDDEV_SAMP;++kind) {
        reset(); spec_value.kind=(orm_sql_window_kind)kind; check_equal(open_window(),TURBODB_STATUS_OK);
        expect_locked(TURBODB_STATUS_LIMIT_EXCEEDED);
      }
    }
  }
  group("window aggregate frames") {
    it("counts all rows through default RANGE peers without admitting groups") {
      spec_value.kind=ORM_SQL_WINDOW_COUNT_ALL;
      check_equal(open_window(),TURBODB_STATUS_OK);
      const int64_t expected[]={2,2,3,6,6,6,8,8,9}; expect_integer(expected,TEST_ROWS);
      check_equal(budget.used.value[ORM_SQL_BUDGET_GROUPS],0u);
    }
    it("ignores NULL values in COUNT while including every peer in the frame") {
      spec_value.kind=ORM_SQL_WINDOW_COUNT_VALUE; spec_value.value_slot=2;
      rows[6][2]=rows[3][2]=turbodb_null(); check_equal(open_window(),TURBODB_STATUS_OK);
      const int64_t expected[]={1,1,2,4,4,4,6,6,7}; expect_integer(expected,TEST_ROWS);
    }
    it("folds moving ROWS minima and maxima from each new frame start") {
      orders[0].slot=2; spec_value.value_slot=0;
      spec_value.frame=(orm_sql_window_frame){.unit=ORM_SQL_FRAME_ROWS,.boundaries={
          {ORM_SQL_BOUND_PRECEDING,turbodb_i64(1)},{ORM_SQL_BOUND_CURRENT_ROW,{0}}}};
      const int64_t minimum[]={3,1,1,3,2,2,1,1,3}, maximum[]={3,3,4,4,3,5,5,4,4};
      for(size_t mode=0;mode<2;++mode) {
        reset(); spec_value.kind=mode?ORM_SQL_WINDOW_MAX:ORM_SQL_WINDOW_MIN;
        check_equal(open_window(),TURBODB_STATUS_OK); expect_integer(mode?maximum:minimum,TEST_ROWS);
      }
    }
    it("includes numeric RANGE neighbors under ascending and descending ordering") {
      const int64_t ascending[]={3,3,6,6,6,6,6,6,3},descending[]={3,6,6,6,6,6,6,3,3};
      spec_value.kind=ORM_SQL_WINDOW_COUNT_ALL;
      spec_value.frame=(orm_sql_window_frame){.unit=ORM_SQL_FRAME_RANGE,.boundaries={
          {ORM_SQL_BOUND_PRECEDING,turbodb_i64(1)},{ORM_SQL_BOUND_FOLLOWING,turbodb_i64(1)}}};
      for(size_t mode=0;mode<2;++mode) {
        reset(); orders[0].descending=mode!=0; check_equal(open_window(),TURBODB_STATUS_OK);
        expect_integer(mode?descending:ascending,TEST_ROWS);
      }
    }
    it("preserves BOOL extrema without numeric conversion") {
      input_rows=3; orders[0].slot=2; types[0]=(orm_sql_type){TURBODB_VALUE_BOOLEAN,true};
      rows[0][0]=turbodb_null(); rows[1][0]=turbodb_bool(true); rows[2][0]=turbodb_bool(false);
      for(size_t mode=0;mode<2;++mode) {
        reset(); spec_value.kind=mode?ORM_SQL_WINDOW_MAX:ORM_SQL_WINDOW_MIN; check_equal(open_window(),TURBODB_STATUS_OK);
        check_equal(next_window().values[TEST_COLUMNS].kind,TURBODB_VALUE_NULL);
        const turbodb_value_t second_value=next_window().values[TEST_COLUMNS];
        check_equal(second_value.kind,TURBODB_VALUE_BOOLEAN); check_true(second_value.data.boolean_value);
        const turbodb_value_t last=next_window().values[TEST_COLUMNS]; check_equal(last.kind,TURBODB_VALUE_BOOLEAN);
        check_equal(last.data.boolean_value,mode!=0); check_equal(next_window().state,ORM_SQL_SCAN_DONE);
      }
    }
    it("keeps prefix SUM state separate from each AVG result") {
      input_rows=4; orders[0].slot=2; types[0]=(orm_sql_type){TURBODB_VALUE_DOUBLE,true};
      rows[0][0]=turbodb_f64(2); rows[1][0]=turbodb_f64(4); rows[2][0]=turbodb_null(); rows[3][0]=turbodb_f64(8);
      const double sums[]={2,6,6,14}, means[]={2,3,3,14.0/3.0};
      for(size_t mode=0;mode<2;++mode) {
        reset(); spec_value.kind=mode?ORM_SQL_WINDOW_AVG:ORM_SQL_WINDOW_SUM;
        check_equal(open_window(),TURBODB_STATUS_OK); expect_distribution(mode?means:sums,input_rows);
      }
    }
    it("reuses a whole partition AVG without dividing the cached sum repeatedly") {
      input_rows=3; spec_value.order_count=0; types[0]=(orm_sql_type){TURBODB_VALUE_DOUBLE,false};
      rows[0][0]=turbodb_f64(2); rows[1][0]=turbodb_f64(4); rows[2][0]=turbodb_f64(9);
      spec_value.kind=ORM_SQL_WINDOW_AVG; check_equal(open_window(),TURBODB_STATUS_OK);
      const double expected[]={5,5,5}; expect_distribution(expected,input_rows);
    }
    it("preserves fresh left to right DOUBLE sums when a moving frame loses a large value") {
      input_rows=5; orders[0].slot=2; types[0]=(orm_sql_type){TURBODB_VALUE_DOUBLE,false};
      const double values[]={1e16,1,-1e16,2,4}, expected[]={1e16,1e16,-1e16,-9999999999999998.0,6};
      for(size_t i=0;i<input_rows;++i) rows[i][0]=turbodb_f64(values[i]);
      spec_value.kind=ORM_SQL_WINDOW_SUM;
      spec_value.frame=(orm_sql_window_frame){.unit=ORM_SQL_FRAME_ROWS,.boundaries={
          {ORM_SQL_BOUND_PRECEDING,turbodb_i64(1)},{ORM_SQL_BOUND_CURRENT_ROW,{0}}}};
      check_equal(open_window(),TURBODB_STATUS_OK); expect_distribution(expected,input_rows);
    }
    it("returns COUNT zero bit identities and other NULL results for empty frames") {
      orders[0].slot=2; types[0]=(orm_sql_type){TURBODB_VALUE_DOUBLE,true};
      for(size_t i=0;i<TEST_ROWS;++i) rows[i][0]=turbodb_f64((double)i);
      spec_value.frame=(orm_sql_window_frame){.unit=ORM_SQL_FRAME_ROWS,.boundaries={
          {ORM_SQL_BOUND_PRECEDING,turbodb_i64(1)},{ORM_SQL_BOUND_PRECEDING,turbodb_i64(2)}}};
      for(size_t mode=ORM_SQL_WINDOW_COUNT_ALL;mode<=ORM_SQL_WINDOW_BIT_XOR;++mode) {
        reset(); spec_value.kind=(orm_sql_window_kind)mode; check_equal(open_window(),TURBODB_STATUS_OK);
        for(size_t i=0;i<TEST_ROWS;++i) {
          const turbodb_value_t result=next_window().values[TEST_COLUMNS];
          if(mode<=ORM_SQL_WINDOW_COUNT_VALUE) { check_equal(result.kind,TURBODB_VALUE_INT64); check_equal(result.data.int64_value,0); }
          else if(mode>=ORM_SQL_WINDOW_BIT_AND) {
            check_equal(result.kind,TURBODB_VALUE_UINT64); check_equal(result.data.uint64_value,mode==ORM_SQL_WINDOW_BIT_AND?UINT64_MAX:0);
          }
          else check_equal(result.kind,TURBODB_VALUE_NULL);
        }
        check_equal(next_window().state,ORM_SQL_SCAN_DONE);
      }
    }
    it("returns NULL for all NULL numeric inputs and resets reduction across partitions") {
      input_rows=4; orders[0].slot=2; spec_value.partition_count=1;
      types[0]=(orm_sql_type){TURBODB_VALUE_DOUBLE,true};
      rows[0][0]=rows[1][0]=turbodb_null(); rows[2][0]=turbodb_f64(6); rows[3][0]=turbodb_f64(10);
      rows[2][1]=rows[3][1]=turbodb_i64(1);
      spec_value.kind=ORM_SQL_WINDOW_AVG; check_equal(open_window(),TURBODB_STATUS_OK);
      for(size_t i=0;i<2;++i) check_equal(next_window().values[TEST_COLUMNS].kind,TURBODB_VALUE_NULL);
      check_equal(next_window().values[TEST_COLUMNS].data.double_value,6.0);
      check_equal(next_window().values[TEST_COLUMNS].data.double_value,8.0);
      check_equal(next_window().state,ORM_SQL_SCAN_DONE);
    }
    it("counts deep owned byte inputs without interpreting their contents") {
      types[2]=(orm_sql_type){TURBODB_VALUE_TEXT,false}; spec_value.order_count=0;
      spec_value.kind=ORM_SQL_WINDOW_COUNT_VALUE; spec_value.value_slot=2;
      check_equal(open_window(),TURBODB_STATUS_OK);
      const orm_sql_scan_row first=next_window(); check_equal(first.values[TEST_COLUMNS].data.int64_value,TEST_ROWS);
      check_equal(first.values[2].data.text_value.data[0],'a');
      for(size_t i=1;i<TEST_ROWS;++i) check_equal(next_window().values[TEST_COLUMNS].data.int64_value,TEST_ROWS);
      check_equal(first.values[2].data.text_value.data[0],'a');
    }
    it("rejects integer SUM AVG and byte MIN MAX at opening before reading") {
      for(size_t mode=0;mode<4;++mode) {
        spec_value.kind=mode<2 ? (mode?ORM_SQL_WINDOW_AVG:ORM_SQL_WINDOW_SUM) :
            (mode==2?ORM_SQL_WINDOW_MIN:ORM_SQL_WINDOW_MAX);
        types[2]=(orm_sql_type){mode<2?TURBODB_VALUE_INT64:TURBODB_VALUE_TEXT,true}; spec_value.value_slot=2;
        check_equal(open_window(),TURBODB_STATUS_UNSUPPORTED); check_null(window.budget); check_equal(calls,0u);
      }
    }
    it("locks intermediate DOUBLE overflow before publishing any partition result") {
      input_rows=2; orders[0].slot=2; types[0]=(orm_sql_type){TURBODB_VALUE_DOUBLE,false};
      rows[0][0]=rows[1][0]=turbodb_f64(DBL_MAX);
      for(size_t mode=0;mode<2;++mode) {
        reset(); spec_value.kind=mode?ORM_SQL_WINDOW_AVG:ORM_SQL_WINDOW_SUM;
        check_equal(open_window(),TURBODB_STATUS_OK); expect_locked(TURBODB_STATUS_LIMIT_EXCEEDED);
      }
    }
    it("refunds every aggregate construction and snapshot allocation failure") {
      types[2]=(orm_sql_type){TURBODB_VALUE_DOUBLE,true}; spec_value.order_count=1; spec_value.value_slot=2;
      spec_value.partition_count=1;
      for(size_t i=0;i<TEST_ROWS;++i) rows[i][2]=turbodb_f64((double)i);
      for(size_t kind=ORM_SQL_WINDOW_COUNT_ALL;kind<=ORM_SQL_WINDOW_BIT_XOR;++kind) {
        reset(); spec_value.kind=(orm_sql_window_kind)kind; spec_value.value_slot=kind==ORM_SQL_WINDOW_COUNT_ALL?0:2;
        check_equal(open_window(),TURBODB_STATUS_OK); check_equal(next_window().state,ORM_SQL_SCAN_ROW);
        const size_t reserves=reserve_calls,resizes=resize_calls;
        for(size_t point=1;point<=reserves+resizes;++point) {
          reset(); if(point<=reserves) fail_reserve=point; else fail_resize=point-reserves;
          const turbodb_status_t status=open_window();
          if(status==TURBODB_STATUS_OK) expect_locked(TURBODB_STATUS_OUT_OF_MEMORY);
          else { check_equal(status,TURBODB_STATUS_OUT_OF_MEMORY); check_null(window.budget); }
        }
      }
    }
    it("locks every aggregate preparation execution step and releases its resources") {
      types[2]=(orm_sql_type){TURBODB_VALUE_DOUBLE,true}; orders[0].slot=0;
      for(size_t i=0;i<TEST_ROWS;++i) rows[i][2]=turbodb_f64((double)i);
      spec_value.frame=(orm_sql_window_frame){.unit=ORM_SQL_FRAME_RANGE,.boundaries={
          {ORM_SQL_BOUND_PRECEDING,turbodb_i64(1)},{ORM_SQL_BOUND_FOLLOWING,turbodb_i64(1)}}};
      for(size_t kind=ORM_SQL_WINDOW_COUNT_ALL;kind<=ORM_SQL_WINDOW_BIT_XOR;++kind) {
        reset(); spec_value.kind=(orm_sql_window_kind)kind; spec_value.value_slot=kind==ORM_SQL_WINDOW_COUNT_ALL?0:2;
        check_equal(open_window(),TURBODB_STATUS_OK);
        const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]; check_equal(next_window().state,ORM_SQL_SCAN_ROW);
        const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
        for(uint64_t point=0;point<steps;++point) {
          reset(); check_equal(open_window(),TURBODB_STATUS_OK);
          budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=
              budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
          expect_locked(TURBODB_STATUS_LIMIT_EXCEEDED);
        }
      }
    }
  }
  group("frame value functions") {
    it("matches the official two-subject FIRST LAST and second fourth value sample") {
      const int64_t values[]={10,9,25,20,0,10,5,30,25};
      const int64_t expected[][TEST_ROWS]={{10,10,10,10,0,0,0,0,0},{10,9,25,20,0,10,5,30,25},
        {-1,9,9,9,-1,10,10,10,10},{-1,-1,-1,20,-1,-1,-1,30,30}};
      for(size_t i=0;i<input_rows;++i) { rows[i][0]=turbodb_i64(values[i]); rows[i][1]=turbodb_i64(i<4 ? 0 : 1); }
      orders[0].slot=2; spec_value.partition_count=1; spec_value.value_slot=0;
      spec_value.frame=(orm_sql_window_frame){ORM_SQL_FRAME_ROWS,{{ORM_SQL_BOUND_UNBOUNDED_PRECEDING,{0}},{ORM_SQL_BOUND_CURRENT_ROW,{0}}}};
      for(size_t sample=0;sample<sizeof(expected)/sizeof(expected[0]);++sample) {
        reset(); spec_value.kind=sample==0 ? ORM_SQL_FIRST_VALUE : sample==1 ? ORM_SQL_LAST_VALUE : ORM_SQL_NTH_VALUE;
        spec_value.offset=sample<2 ? 0 : sample==2 ? 2 : 4;
        check_equal(open_window(),TURBODB_STATUS_OK);
        for(size_t i=0;i<input_rows;++i) {
          const turbodb_value_t value=next_window().values[TEST_COLUMNS];
          if(expected[sample][i]<0) check_equal(value.kind,TURBODB_VALUE_NULL);
          else { check_equal(value.kind,TURBODB_VALUE_INT64); check_equal(value.data.int64_value,expected[sample][i]); }
        }
        check_equal(next_window().state,ORM_SQL_SCAN_DONE);
      }
    }
    it("includes current peers in ordered default frames and the whole unordered partition") {
      const int64_t expected[][TEST_ROWS]={{1,1,1,1,1,1,1,1,1},{6,6,4,8,8,8,7,7,5},{-1,-1,4,4,4,4,4,4,4}};
      for(size_t kind=0;kind<3;++kind) {
        reset(); spec_value.kind=(orm_sql_window_kind)(ORM_SQL_FIRST_VALUE+kind);
        spec_value.value_slot=2; spec_value.offset=kind==2 ? 3 : 0;
        check_equal(open_window(),TURBODB_STATUS_OK); check_true(window.source.types[TEST_COLUMNS].nullable);
        for(size_t i=0;i<TEST_ROWS;++i) {
          const turbodb_value_t value=next_window().values[TEST_COLUMNS];
          if(expected[kind][i]<0) check_equal(value.kind,TURBODB_VALUE_NULL);
          else { check_equal(value.kind,TURBODB_VALUE_INT64); check_equal(value.data.int64_value,expected[kind][i]); }
        }
        check_equal(next_window().state,ORM_SQL_SCAN_DONE);
        reset(); spec_value.order_count=0; check_equal(open_window(),TURBODB_STATUS_OK);
        for(size_t i=0;i<TEST_ROWS;++i) check_equal(next_window().values[TEST_COLUMNS].data.int64_value,kind==0 ? 0 : kind==1 ? 8 : 2);
        check_equal(next_window().state,ORM_SQL_SCAN_DONE); spec_value.order_count=1;
      }
    }
    it("clips rolling ROWS at partition ends and counts N from the frame start") {
      input_rows=5; orders[0].slot=2; spec_value.value_slot=2;
      spec_value.frame=(orm_sql_window_frame){ORM_SQL_FRAME_ROWS,{{ORM_SQL_BOUND_PRECEDING,turbodb_u64(1)},{ORM_SQL_BOUND_FOLLOWING,turbodb_i64(1)}}};
      for(size_t kind=0;kind<3;++kind) {
        reset(); spec_value.kind=(orm_sql_window_kind)(ORM_SQL_FIRST_VALUE+kind); spec_value.offset=kind==2 ? 3 : 0;
        check_equal(open_window(),TURBODB_STATUS_OK);
        for(size_t i=0;i<input_rows;++i) {
          const turbodb_value_t value=next_window().values[TEST_COLUMNS];
          if(kind==2 && (i==0 || i+1==input_rows)) check_equal(value.kind,TURBODB_VALUE_NULL);
          else check_equal(value.data.int64_value,(int64_t)(kind==0 ? (i ? i-1 : 0) : (i+1<input_rows ? i+1 : i)));
        }
        check_equal(next_window().state,ORM_SQL_SCAN_DONE);
      }
    }
    it("uses preceding following current and unbounded endpoints in both order directions") {
      input_rows=5; orders[0].slot=2; spec_value.value_slot=2;
      const orm_sql_frame_boundary boundaries[][ORM_SQL_FRAME_BOUNDARIES]={
        {{ORM_SQL_BOUND_UNBOUNDED_PRECEDING,{0}},{ORM_SQL_BOUND_CURRENT_ROW,{0}}},
        {{ORM_SQL_BOUND_CURRENT_ROW,{0}},{ORM_SQL_BOUND_UNBOUNDED_FOLLOWING,{0}}},
        {{ORM_SQL_BOUND_PRECEDING,turbodb_i64(2)},{ORM_SQL_BOUND_PRECEDING,turbodb_i64(1)}},
        {{ORM_SQL_BOUND_FOLLOWING,turbodb_i64(1)},{ORM_SQL_BOUND_FOLLOWING,turbodb_i64(2)}},
        {{ORM_SQL_BOUND_CURRENT_ROW,{0}},{ORM_SQL_BOUND_CURRENT_ROW,{0}}}};
      const int64_t expected[][2][5]={{{0,0,0,0,0},{0,1,2,3,4}},{{0,1,2,3,4},{4,4,4,4,4}},
        {{-1,0,0,1,2},{-1,0,1,2,3}},{{1,2,3,4,-1},{2,3,4,4,-1}},{{0,1,2,3,4},{0,1,2,3,4}}};
      for(size_t unit=0;unit<2;++unit) for(size_t desc=0;desc<2;++desc)
        for(size_t sample=0;sample<sizeof(boundaries)/sizeof(boundaries[0]);++sample) for(size_t last=0;last<2;++last) {
          reset(); orders[0].descending=desc!=0; spec_value.kind=last ? ORM_SQL_LAST_VALUE : ORM_SQL_FIRST_VALUE;
          spec_value.frame.unit=unit ? ORM_SQL_FRAME_RANGE : ORM_SQL_FRAME_ROWS;
          memcpy(spec_value.frame.boundaries,boundaries[sample],sizeof(spec_value.frame.boundaries));
          check_equal(open_window(),TURBODB_STATUS_OK);
          for(size_t i=0;i<input_rows;++i) {
            const turbodb_value_t value=next_window().values[TEST_COLUMNS]; const int64_t target=expected[sample][last][i];
            if(target<0) check_equal(value.kind,TURBODB_VALUE_NULL); else check_equal(value.data.int64_value,desc ? 4-target : target);
          }
          check_equal(next_window().state,ORM_SQL_SCAN_DONE);
        }
    }
    it("uses every order column for CURRENT ROW peers with compound RANGE keys") {
      input_rows=4; spec_value.value_slot=2; spec_value.order_count=2;
      for(size_t i=0;i<input_rows;++i) { rows[i][0]=turbodb_i64(1); rows[i][1]=turbodb_i64((int64_t)(i/2)); }
      spec_value.frame=(orm_sql_window_frame){ORM_SQL_FRAME_RANGE,{{ORM_SQL_BOUND_CURRENT_ROW,{0}},{ORM_SQL_BOUND_CURRENT_ROW,{0}}}};
      for(size_t last=0;last<2;++last) {
        reset(); spec_value.kind=last ? ORM_SQL_LAST_VALUE : ORM_SQL_FIRST_VALUE; check_equal(open_window(),TURBODB_STATUS_OK);
        const int64_t expected[][4]={{0,0,2,2},{1,1,3,3}}; expect_integer(expected[last],input_rows);
      }
    }
    it("keeps unbounded endpoints for NULL RANGE references instead of restricting the whole frame to peers") {
      input_rows=4; spec_value.kind=ORM_SQL_FIRST_VALUE; spec_value.value_slot=2;
      const turbodb_value_t keys[]={turbodb_i64(1),turbodb_null(),turbodb_i64(2),turbodb_null()};
      for(size_t i=0;i<input_rows;++i) rows[i][0]=keys[i];
      spec_value.frame=(orm_sql_window_frame){ORM_SQL_FRAME_RANGE,{{ORM_SQL_BOUND_UNBOUNDED_PRECEDING,{0}},{ORM_SQL_BOUND_PRECEDING,turbodb_i64(0)}}};
      orders[0].descending=true; check_equal(open_window(),TURBODB_STATUS_OK);
      const int64_t first[]={2,2,2,2}; expect_integer(first,input_rows);
      reset(); spec_value.kind=ORM_SQL_LAST_VALUE; orders[0].descending=false;
      spec_value.frame=(orm_sql_window_frame){ORM_SQL_FRAME_RANGE,{{ORM_SQL_BOUND_FOLLOWING,turbodb_i64(0)},{ORM_SQL_BOUND_UNBOUNDED_FOLLOWING,{0}}}};
      check_equal(open_window(),TURBODB_STATUS_OK); const int64_t last[]={2,2,2,2}; expect_integer(last,input_rows);
    }
    it("returns NULL for empty same-direction and far-outside ROWS frames without overflow") {
      spec_value.kind=ORM_SQL_LAST_VALUE; spec_value.value_slot=2;
      const orm_sql_window_frame frames[]={
        {ORM_SQL_FRAME_ROWS,{{ORM_SQL_BOUND_PRECEDING,turbodb_i64(1)},{ORM_SQL_BOUND_PRECEDING,turbodb_i64(2)}}},
        {ORM_SQL_FRAME_ROWS,{{ORM_SQL_BOUND_PRECEDING,turbodb_u64(UINT64_MAX)},{ORM_SQL_BOUND_PRECEDING,turbodb_u64(UINT64_MAX)}}},
        {ORM_SQL_FRAME_ROWS,{{ORM_SQL_BOUND_FOLLOWING,turbodb_u64(UINT64_MAX)},{ORM_SQL_BOUND_UNBOUNDED_FOLLOWING,{0}}}}};
      for(size_t f=0;f<sizeof(frames)/sizeof(frames[0]);++f) {
        reset(); spec_value.frame=frames[f]; check_equal(open_window(),TURBODB_STATUS_OK);
        for(size_t i=0;i<input_rows;++i) check_equal(next_window().values[TEST_COLUMNS].kind,TURBODB_VALUE_NULL);
        check_equal(next_window().state,ORM_SQL_SCAN_DONE);
      }
    }
    it("respects NULL values and never selects across a partition boundary") {
      input_rows=4; orders[0].slot=2; spec_value.value_slot=0; spec_value.partition_count=1;
      for(size_t i=0;i<input_rows;++i) { rows[i][0]=i%2 ? turbodb_i64((int64_t)i) : turbodb_null(); rows[i][1]=turbodb_i64((int64_t)(i/2)); }
      spec_value.kind=ORM_SQL_FIRST_VALUE; check_equal(open_window(),TURBODB_STATUS_OK);
      for(size_t i=0;i<input_rows;++i) check_equal(next_window().values[TEST_COLUMNS].kind,TURBODB_VALUE_NULL);
      check_equal(next_window().state,ORM_SQL_SCAN_DONE);
      reset(); spec_value.kind=ORM_SQL_NTH_VALUE; spec_value.offset=2; check_equal(open_window(),TURBODB_STATUS_OK);
      for(size_t i=0;i<input_rows;++i) {
        const turbodb_value_t value=next_window().values[TEST_COLUMNS];
        if(i%2) check_equal(value.data.int64_value,(int64_t)i); else check_equal(value.kind,TURBODB_VALUE_NULL);
      }
      check_equal(next_window().state,ORM_SQL_SCAN_DONE);
    }
    it("locates numeric RANGE endpoints in both directions while keeping NULL peers") {
      input_rows=6; const turbodb_value_t keys[]={turbodb_null(),turbodb_i64(1),turbodb_i64(2),turbodb_null(),turbodb_i64(2),turbodb_i64(4)};
      for(size_t i=0;i<input_rows;++i) rows[i][0]=keys[i];
      const int64_t expected[][6]={{0,0,1,1,1,5},{3,3,4,4,4,5},{5,2,2,2,0,0},{5,1,1,1,3,3}};
      spec_value.value_slot=2;
      spec_value.frame=(orm_sql_window_frame){ORM_SQL_FRAME_RANGE,{{ORM_SQL_BOUND_PRECEDING,turbodb_i64(1)},{ORM_SQL_BOUND_FOLLOWING,turbodb_u64(1)}}};
      for(size_t sample=0;sample<4;++sample) {
        reset(); orders[0].descending=sample>=2; spec_value.kind=sample%2 ? ORM_SQL_LAST_VALUE : ORM_SQL_FIRST_VALUE;
        check_equal(open_window(),TURBODB_STATUS_OK); expect_integer(expected[sample],input_rows);
      }
    }
    it("compares integer RANGE distances exactly at signed and unsigned extrema") {
      input_rows=5; spec_value.kind=ORM_SQL_NTH_VALUE; spec_value.value_slot=2; spec_value.offset=2;
      spec_value.frame=(orm_sql_window_frame){ORM_SQL_FRAME_RANGE,{{ORM_SQL_BOUND_PRECEDING,turbodb_i64(1)},{ORM_SQL_BOUND_FOLLOWING,turbodb_i64(1)}}};
      const int64_t signed_keys[]={INT64_MIN,INT64_MIN+1,0,INT64_MAX-1,INT64_MAX};
      const uint64_t unsigned_keys[]={0,1,UINT64_C(1)<<53,(UINT64_C(1)<<53)+1,UINT64_MAX};
      const int64_t expected[][5]={{1,1,-1,4,4},{1,1,3,3,-1}};
      for(size_t sample=0;sample<2;++sample) {
        reset(); types[0].kind=sample ? TURBODB_VALUE_UINT64 : TURBODB_VALUE_INT64;
        for(size_t i=0;i<input_rows;++i) rows[i][0]=sample ? turbodb_u64(unsigned_keys[i]) : turbodb_i64(signed_keys[i]);
        check_equal(open_window(),TURBODB_STATUS_OK);
        for(size_t i=0;i<input_rows;++i) {
          const turbodb_value_t value=next_window().values[TEST_COLUMNS];
          if(expected[sample][i]<0) check_equal(value.kind,TURBODB_VALUE_NULL);
          else check_equal(value.data.int64_value,expected[sample][i]);
        }
        check_equal(next_window().state,ORM_SQL_SCAN_DONE);
      }
    }
    it("supports fractional numeric RANGE boundaries and finite keys near double overflow") {
      input_rows=4; spec_value.kind=ORM_SQL_LAST_VALUE; spec_value.value_slot=2; types[0].kind=TURBODB_VALUE_DOUBLE;
      const double keys[]={0,0.5,1.5,2}; const int64_t expected[]={1,1,3,3};
      for(size_t i=0;i<input_rows;++i) rows[i][0]=turbodb_f64(keys[i]);
      spec_value.frame=(orm_sql_window_frame){ORM_SQL_FRAME_RANGE,{{ORM_SQL_BOUND_CURRENT_ROW,{0}},{ORM_SQL_BOUND_FOLLOWING,turbodb_f64(0.5)}}};
      check_equal(open_window(),TURBODB_STATUS_OK); expect_integer(expected,input_rows);
      reset(); rows[2][0]=turbodb_f64(DBL_MAX); rows[3][0]=turbodb_f64(DBL_MAX);
      spec_value.frame.boundaries[1].distance=turbodb_f64(DBL_MAX); check_equal(open_window(),TURBODB_STATUS_OK);
      const int64_t overflow[]={3,3,3,3}; expect_integer(overflow,input_rows);
    }
    it("borrows frame byte values from owned snapshots after source reuse") {
      const size_t ids[]={1,6,4,0,3,8,2,7,5};
      for(size_t kind=0;kind<3;++kind) for(size_t blob=0;blob<2;++blob) {
        reset(); types[2].kind=blob ? TURBODB_VALUE_BLOB : TURBODB_VALUE_TEXT;
        spec_value.kind=(orm_sql_window_kind)(ORM_SQL_FIRST_VALUE+kind); spec_value.value_slot=2;
        spec_value.offset=kind==2 ? 1 : 0;
        spec_value.frame=(orm_sql_window_frame){ORM_SQL_FRAME_ROWS,{{ORM_SQL_BOUND_CURRENT_ROW,{0}},{ORM_SQL_BOUND_CURRENT_ROW,{0}}}};
        check_equal(open_window(),TURBODB_STATUS_OK);
        for(size_t i=0;i<input_rows;++i) {
          const turbodb_value_t value=next_window().values[TEST_COLUMNS]; memset(payload,'!',sizeof(payload));
          check_equal(value.kind,types[2].kind);
          const char *bytes=blob ? value.data.blob_value.data : value.data.text_value.data;
          check_not_equal((const void *)bytes,(const void *)payload); check_equal(bytes[0],(char)('a'+ids[i]));
        }
        check_equal(next_window().state,ORM_SQL_SCAN_DONE);
      }
    }
    it("validates frame units distances directions and positions before source admission") {
      spec_value.kind=ORM_SQL_NTH_VALUE; spec_value.value_slot=2;
      check_equal(open_window(),TURBODB_STATUS_INVALID_ARGUMENT);
      spec_value.offset=ORM_SQL_WINDOW_MAX_NTH+1; check_equal(open_window(),TURBODB_STATUS_INVALID_ARGUMENT);
      spec_value.offset=1;
      spec_value.frame=(orm_sql_window_frame){ORM_SQL_FRAME_ROWS,{{ORM_SQL_BOUND_PRECEDING,turbodb_f64(1)},{ORM_SQL_BOUND_CURRENT_ROW,{0}}}};
      check_equal(open_window(),TURBODB_STATUS_INVALID_ARGUMENT);
      spec_value.frame.boundaries[0].distance=turbodb_i64(-1); check_equal(open_window(),TURBODB_STATUS_INVALID_ARGUMENT);
      spec_value.frame.boundaries[0].kind=ORM_SQL_BOUND_FOLLOWING; check_equal(open_window(),TURBODB_STATUS_INVALID_ARGUMENT);
      spec_value.frame.boundaries[0].kind=(orm_sql_frame_boundary_kind)-1; check_equal(open_window(),TURBODB_STATUS_INVALID_ARGUMENT);
      spec_value.frame.unit=(orm_sql_frame_unit)-1; check_equal(open_window(),TURBODB_STATUS_INVALID_ARGUMENT);
      spec_value.frame=(orm_sql_window_frame){ORM_SQL_FRAME_RANGE,{{ORM_SQL_BOUND_PRECEDING,turbodb_f64(INFINITY)},{ORM_SQL_BOUND_CURRENT_ROW,{0}}}};
      check_equal(open_window(),TURBODB_STATUS_INVALID_ARGUMENT);
      spec_value.frame.boundaries[0].distance=turbodb_u64(1); spec_value.order_count=0;
      check_equal(open_window(),TURBODB_STATUS_INVALID_ARGUMENT); check_false(source.active); check_equal(calls,0u);
    }
    it("refunds every allocation during frame construction and byte preparation") {
      types[2].kind=TURBODB_VALUE_TEXT; spec_value.value_slot=2;
      for(size_t kind=0;kind<3;++kind) {
        reset(); spec_value.kind=(orm_sql_window_kind)(ORM_SQL_FIRST_VALUE+kind); spec_value.offset=kind==2 ? 2 : 0;
        check_equal(open_window(),TURBODB_STATUS_OK); const size_t construction[]={reserve_calls,resize_calls};
        check_equal(next_window().state,ORM_SQL_SCAN_ROW); const size_t total[]={reserve_calls,resize_calls};
        for(size_t pass=0;pass<sizeof(total)/sizeof(total[0]);++pass) for(size_t point=1;point<=total[pass];++point) {
          reset(); if(pass) fail_resize=point; else fail_reserve=point;
          const turbodb_status_t status=open_window();
          if(point<=construction[pass]) { check_equal(status,TURBODB_STATUS_OUT_OF_MEMORY); check_null(window.budget); check_false(source.active); }
          else { check_equal(status,TURBODB_STATUS_OK); expect_locked(TURBODB_STATUS_OUT_OF_MEMORY); }
        }
      }
    }
    it("locks each failed execution step including RANGE binary searches without publishing a prefix") {
      spec_value.value_slot=2;
      spec_value.frame=(orm_sql_window_frame){ORM_SQL_FRAME_RANGE,{{ORM_SQL_BOUND_PRECEDING,turbodb_i64(1)},{ORM_SQL_BOUND_FOLLOWING,turbodb_i64(1)}}};
      for(size_t kind=0;kind<3;++kind) {
        limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=TEST_LIMIT; reset();
        spec_value.kind=(orm_sql_window_kind)(ORM_SQL_FIRST_VALUE+kind); spec_value.offset=kind==2 ? 2 : 0;
        check_equal(open_window(),TURBODB_STATUS_OK); const uint64_t construction=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
        check_equal(next_window().state,ORM_SQL_SCAN_ROW); const uint64_t total=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
        for(uint64_t point=0;point<total;++point) {
          reset(); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=point;
          const turbodb_status_t status=open_window();
          if(point<construction) { check_equal(status,TURBODB_STATUS_LIMIT_EXCEEDED); check_null(window.budget); check_false(source.active); }
          else { check_equal(status,TURBODB_STATUS_OK); expect_locked(TURBODB_STATUS_LIMIT_EXCEEDED); }
        }
      }
    }
  }

  it("returns the official preceding and following series values with omitted NULL defaults") {
    const int64_t values[]={100,125,132,145,140,150,200}; input_rows=sizeof(values)/sizeof(values[0]);
    for(size_t i=0;i<input_rows;++i) rows[i][0]=turbodb_i64(values[i]);
    orders[0].slot=2; spec_value.value_slot=0; spec_value.offset=1;
    const orm_sql_window_kind kinds[]={ORM_SQL_LAG,ORM_SQL_LEAD};
    for(size_t pass=0;pass<sizeof(kinds)/sizeof(kinds[0]);++pass) {
      reset(); spec_value.kind=kinds[pass]; check_equal(open_window(),TURBODB_STATUS_OK);
      check_equal(source.active,true); check_equal(calls,0u);
      check_equal(window.source.types[TEST_COLUMNS].kind,TURBODB_VALUE_INT64);
      check_true(window.source.types[TEST_COLUMNS].nullable);
      for(size_t i=0;i<input_rows;++i) {
        const orm_sql_scan_row row=next_window();
        if(pass ? i+1==input_rows : i==0) check_equal(row.values[TEST_COLUMNS].kind,TURBODB_VALUE_NULL);
        else check_equal(row.values[TEST_COLUMNS].data.int64_value,values[pass?i+1:i-1]);
      }
      check_equal(next_window().state,ORM_SQL_SCAN_DONE);
    }
  }
  it("uses current-row defaults for out-of-partition offsets and returns the current value for zero") {
    const uint64_t offsets[]={0,ORM_SQL_WINDOW_MAX_OFFSET}; orders[0].slot=2;
    spec_value.value_slot=0; spec_value.default_slot=2; spec_value.has_default=true;
    for(size_t kind=0;kind<2;++kind) for(size_t pass=0;pass<sizeof(offsets)/sizeof(offsets[0]);++pass) {
      reset(); spec_value.kind=kind?ORM_SQL_LEAD:ORM_SQL_LAG; spec_value.offset=offsets[pass];
      check_equal(open_window(),TURBODB_STATUS_OK);
      for(size_t i=0;i<TEST_ROWS;++i) {
        const orm_sql_scan_row row=next_window();
        check_equal(row.values[TEST_COLUMNS].data.int64_value,rows[i][pass?2:0].data.int64_value);
      }
      check_equal(next_window().state,ORM_SQL_SCAN_DONE);
    }
  }
  it("keeps target NULL values and never crosses a partition to find the previous row") {
    orders[0].slot=2; spec_value=(orm_sql_window_spec){.kind=ORM_SQL_LAG,.partitions=partitions,.partition_count=1,
        .orders=orders,.order_count=1,.offset=1,.value_slot=0,.default_slot=2,.has_default=true};
    for(size_t i=0;i<TEST_ROWS;++i) { rows[i][0]=turbodb_i64((int64_t)i*10); rows[i][1]=turbodb_i64(i<4?0:1); }
    rows[1][0]=turbodb_null(); check_equal(open_window(),TURBODB_STATUS_OK);
    const int64_t expected[]={0,0,0,20,4,40,50,60,70};
    for(size_t i=0;i<TEST_ROWS;++i) {
      const orm_sql_scan_row row=next_window();
      if(i==2) check_equal(row.values[TEST_COLUMNS].kind,TURBODB_VALUE_NULL);
      else { check_equal(row.values[TEST_COLUMNS].kind,TURBODB_VALUE_INT64); check_equal(row.values[TEST_COLUMNS].data.int64_value,expected[i]); }
    }
    check_equal(next_window().state,ORM_SQL_SCAN_DONE);
  }
  it("borrows byte results from owned target snapshots after the input payload is overwritten") {
    const size_t ids[]={1,6,4,0,3,8,2,7,5};
    for(size_t kind=0;kind<2;++kind) for(size_t blob=0;blob<2;++blob) {
      reset(); types[2]=(orm_sql_type){blob?TURBODB_VALUE_BLOB:TURBODB_VALUE_TEXT,false};
      spec_value.kind=kind?ORM_SQL_LEAD:ORM_SQL_LAG; spec_value.value_slot=2; spec_value.offset=1;
      check_equal(open_window(),TURBODB_STATUS_OK);
      for(size_t i=0;i<TEST_ROWS;++i) {
        const orm_sql_scan_row row=next_window(); memset(payload,'!',sizeof(payload));
        const turbodb_value_t value=row.values[TEST_COLUMNS];
        if(kind?i+1==TEST_ROWS:i==0) { check_equal(value.kind,TURBODB_VALUE_NULL); continue; }
        check_equal(value.kind,types[2].kind);
        const char *bytes=blob?value.data.blob_value.data:value.data.text_value.data;
        const size_t size=blob?value.data.blob_value.size:value.data.text_value.len;
        check_equal(size,1u); check_not_equal((const void *)bytes,(const void *)payload);
        check_equal(bytes[0],(char)('a'+ids[kind?i+1:i-1]));
      }
      check_equal(next_window().state,ORM_SQL_SCAN_DONE);
    }
  }
  it("rejects offset bounds slots and incompatible default types before taking an input lease") {
    spec_value.kind=ORM_SQL_LAG; spec_value.value_slot=TEST_COLUMNS;
    check_equal(open_window(),TURBODB_STATUS_INVALID_ARGUMENT);
    spec_value.value_slot=0; spec_value.has_default=true; spec_value.default_slot=TEST_COLUMNS;
    check_equal(open_window(),TURBODB_STATUS_INVALID_ARGUMENT);
    spec_value.default_slot=2; types[2].kind=TURBODB_VALUE_TEXT;
    check_equal(open_window(),TURBODB_STATUS_UNSUPPORTED);
    spec_value.has_default=false; spec_value.offset=ORM_SQL_WINDOW_MAX_OFFSET+1;
    check_equal(open_window(),TURBODB_STATUS_INVALID_ARGUMENT); check_equal(calls,0u); check_false(source.active);
    orm_sql_type result={TURBODB_VALUE_BOOLEAN,false};
    const orm_sql_type integer={TURBODB_VALUE_INT64,false},null_type={TURBODB_VALUE_NULL,true};
    check_equal(orm_sql_window_offset_type(integer,types[2],&result,&error),TURBODB_STATUS_UNSUPPORTED);
    check_equal(result.kind,TURBODB_VALUE_BOOLEAN); check_false(result.nullable);
    check_equal(orm_sql_window_offset_type(null_type,types[2],&result,&error),TURBODB_STATUS_OK);
    check_equal(result.kind,TURBODB_VALUE_TEXT); check_true(result.nullable);
  }
  it("refunds every allocation while constructing and preparing byte offset windows") {
    types[2]=(orm_sql_type){TURBODB_VALUE_TEXT,false}; spec_value.value_slot=2; spec_value.offset=1;
    for(size_t kind=0;kind<2;++kind) {
      reset(); spec_value.kind=kind?ORM_SQL_LEAD:ORM_SQL_LAG;
      check_equal(open_window(),TURBODB_STATUS_OK); const size_t construction[]={reserve_calls,resize_calls};
      check_equal(next_window().state,ORM_SQL_SCAN_ROW); const size_t total[]={reserve_calls,resize_calls};
      for(size_t pass=0;pass<sizeof(total)/sizeof(total[0]);++pass) for(size_t point=1;point<=total[pass];++point) {
        reset(); if(pass) fail_resize=point; else fail_reserve=point;
        const turbodb_status_t status=open_window();
        if(point<=construction[pass]) { check_equal(status,TURBODB_STATUS_OUT_OF_MEMORY); check_null(window.budget); check_false(source.active); }
        else { check_equal(status,TURBODB_STATUS_OK); expect_locked(TURBODB_STATUS_OUT_OF_MEMORY); }
      }
    }
  }
  it("refunds every execution step while preparing offset windows and byte result views") {
    types[2]=(orm_sql_type){TURBODB_VALUE_BLOB,false}; spec_value.value_slot=2; spec_value.offset=1;
    for(size_t kind=0;kind<2;++kind) {
      limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=TEST_LIMIT; reset();
      spec_value.kind=kind?ORM_SQL_LEAD:ORM_SQL_LAG; check_equal(open_window(),TURBODB_STATUS_OK);
      const uint64_t construction=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(next_window().state,ORM_SQL_SCAN_ROW); const uint64_t total=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      for(uint64_t point=0;point<total;++point) {
        reset(); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=point;
        const turbodb_status_t status=open_window();
        if(point<construction) { check_equal(status,TURBODB_STATUS_LIMIT_EXCEEDED); check_null(window.budget); check_false(source.active); }
        else { check_equal(status,TURBODB_STATUS_OK); expect_locked(TURBODB_STATUS_LIMIT_EXCEEDED); }
      }
    }
  }
  it("numbers the official peer sequence with stable carried row identities") {
    check_equal(open_window(),TURBODB_STATUS_OK); check_equal(calls,0u); check_true(source.active);
    const int64_t sorted[]={1,1,2,3,3,3,4,4,5}, ids[]={1,6,4,0,3,8,2,7,5};
    for(size_t i=0;i<TEST_ROWS;++i) {
      const orm_sql_scan_row row=next_window(); check_equal(row.state,ORM_SQL_SCAN_ROW);
      check_equal(row.values[0].data.int64_value,sorted[i]); check_equal(row.values[2].data.int64_value,ids[i]);
      check_equal(row.values[3].data.int64_value,(int64_t)i+1);
    }
    check_equal(next_window().state,ORM_SQL_SCAN_DONE); check_equal(calls,TEST_ROWS+1u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],TEST_ROWS);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],TEST_ROWS*2u);
    const orm_sql_row_source *view=orm_tidesdb_sql_window_source(&window);
    check_equal(view->columns,TEST_COLUMNS+1u); check_false(view->types[3].nullable);
  }
  it("ranks peers with the official gaps") {
    spec_value.kind=ORM_SQL_RANK; check_equal(open_window(),TURBODB_STATUS_OK);
    const int64_t expected[]={1,1,3,4,4,4,7,7,9}; expect_integer(expected,TEST_ROWS);
  }
  it("gives consecutive official dense ranks") {
    spec_value.kind=ORM_SQL_DENSE_RANK; check_equal(open_window(),TURBODB_STATUS_OK);
    const int64_t expected[]={1,1,2,3,3,3,4,4,5}; expect_integer(expected,TEST_ROWS);
  }
  it("computes the official percentage ranks") {
    spec_value.kind=ORM_SQL_PERCENT_RANK; check_equal(open_window(),TURBODB_STATUS_OK);
    const double expected[]={0,0,.25,.375,.375,.375,.75,.75,1}; expect_distribution(expected,TEST_ROWS);
    check_equal(window.source.types[3].kind,TURBODB_VALUE_DOUBLE); check_false(window.source.types[3].nullable);
  }
  it("computes the official cumulative peer distribution") {
    spec_value.kind=ORM_SQL_CUME_DIST; check_equal(open_window(),TURBODB_STATUS_OK);
    const double expected[]={2.0/9,2.0/9,3.0/9,6.0/9,6.0/9,6.0/9,8.0/9,8.0/9,1};
    expect_distribution(expected,TEST_ROWS);
  }
  it("tiles the official sequence into two unequal buckets") {
    spec_value.kind=ORM_SQL_NTILE; spec_value.buckets=2; check_equal(open_window(),TURBODB_STATUS_OK);
    const int64_t expected[]={1,1,1,1,1,2,2,2,2}; expect_integer(expected,TEST_ROWS);
  }
  it("tiles the official sequence into four buckets with larger buckets first") {
    spec_value.kind=ORM_SQL_NTILE; spec_value.buckets=4; check_equal(open_window(),TURBODB_STATUS_OK);
    const int64_t expected[]={1,1,1,2,2,3,3,4,4}; expect_integer(expected,TEST_ROWS);
  }
  it("accepts one bucket and the largest allowed bucket count without allocating buckets") {
    spec_value.kind=ORM_SQL_NTILE; spec_value.buckets=1; check_equal(open_window(),TURBODB_STATUS_OK);
    const int64_t one[]={1,1,1,1,1,1,1,1,1}; expect_integer(one,TEST_ROWS);
    const size_t peak=budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES]; reset();
    spec_value.buckets=UINT64_C(1)<<63; check_equal(open_window(),TURBODB_STATUS_OK);
    const int64_t many[]={1,2,3,4,5,6,7,8,9}; expect_integer(many,TEST_ROWS);
    check_equal(budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES],peak);
  }
  it("resets ranks and distributions at each partition") {
    for(size_t i=0;i<TEST_ROWS;++i) rows[i][1]=turbodb_i64(rows[i][0].data.int64_value<=2 ? 1 : 2);
    spec_value.partition_count=1; spec_value.kind=ORM_SQL_RANK; check_equal(open_window(),TURBODB_STATUS_OK);
    const int64_t expected[]={1,1,3,1,1,1,4,4,6}; expect_integer(expected,TEST_ROWS); reset();
    spec_value.kind=ORM_SQL_CUME_DIST; check_equal(open_window(),TURBODB_STATUS_OK);
    const double cumulative[]={2.0/3,2.0/3,1,.5,.5,.5,5.0/6,5.0/6,1}; expect_distribution(cumulative,TEST_ROWS);
  }
  it("resets bucket remainders independently in each partition") {
    for(size_t i=0;i<TEST_ROWS;++i) rows[i][1]=turbodb_i64(rows[i][0].data.int64_value<=2 ? 1 : 2);
    spec_value.partition_count=1; spec_value.kind=ORM_SQL_NTILE; spec_value.buckets=2;
    check_equal(open_window(),TURBODB_STATUS_OK);
    const int64_t expected[]={1,1,2,1,1,1,2,2,2}; expect_integer(expected,TEST_ROWS);
  }
  it("treats all unordered partition rows as peers") {
    spec_value.order_count=0;
    for(orm_sql_window_kind kind=ORM_SQL_RANK;kind<=ORM_SQL_CUME_DIST;++kind) {
      spec_value.kind=kind; check_equal(open_window(),TURBODB_STATUS_OK);
      for(size_t i=0;i<TEST_ROWS;++i) {
        const orm_sql_scan_row row=next_window(); check_equal(row.state,ORM_SQL_SCAN_ROW);
        if(kind<=ORM_SQL_DENSE_RANK) check_equal(row.values[3].data.int64_value,1);
        else check_equal(row.values[3].data.double_value,kind==ORM_SQL_CUME_DIST ? 1.0 : 0.0);
      }
      reset();
    }
  }
  it("emits no empty partition and gives defined singleton distributions") {
    for(orm_sql_window_kind kind=ORM_SQL_ROW_NUMBER;kind<=ORM_SQL_NTILE;++kind) {
      spec_value.kind=kind; spec_value.buckets=kind==ORM_SQL_NTILE ? 4 : 0; input_rows=0;
      check_equal(open_window(),TURBODB_STATUS_OK); check_equal(next_window().state,ORM_SQL_SCAN_DONE); reset();
      input_rows=1; check_equal(open_window(),TURBODB_STATUS_OK); const orm_sql_scan_row row=next_window();
      check_equal(row.state,ORM_SQL_SCAN_ROW);
      if(kind==ORM_SQL_PERCENT_RANK) check_equal(row.values[3].data.double_value,0.0);
      else if(kind==ORM_SQL_CUME_DIST) check_equal(row.values[3].data.double_value,1.0);
      else check_equal(row.values[3].data.int64_value,1);
      check_equal(next_window().state,ORM_SQL_SCAN_DONE); reset();
    }
  }
  it("handles descending order with NULL peers last") {
    input_rows=5; const turbodb_value_t values[]={turbodb_null(),turbodb_i64(1),turbodb_i64(2),turbodb_null(),turbodb_i64(2)};
    for(size_t i=0;i<input_rows;++i) rows[i][0]=values[i];
    spec_value.kind=ORM_SQL_RANK; orders[0].descending=true; check_equal(open_window(),TURBODB_STATUS_OK);
    const int64_t expected[]={1,1,3,4,4}; expect_integer(expected,input_rows);
  }
  it("handles ascending NULL peers first") {
    input_rows=5; const turbodb_value_t values[]={turbodb_null(),turbodb_i64(1),turbodb_i64(2),turbodb_null(),turbodb_i64(2)};
    for(size_t i=0;i<input_rows;++i) rows[i][0]=values[i];
    spec_value.kind=ORM_SQL_DENSE_RANK; check_equal(open_window(),TURBODB_STATUS_OK);
    const int64_t expected[]={1,1,2,3,3}; expect_integer(expected,input_rows);
  }
  it("compares unsigned extrema without signed conversion") {
    input_rows=4; types[0]=(orm_sql_type){TURBODB_VALUE_UINT64,false};
    const uint64_t values[]={UINT64_MAX,0,UINT64_MAX,(UINT64_C(1)<<63)};
    for(size_t i=0;i<input_rows;++i) rows[i][0]=turbodb_u64(values[i]);
    spec_value.kind=ORM_SQL_RANK; check_equal(open_window(),TURBODB_STATUS_OK);
    const int64_t expected[]={1,2,3,3}; expect_integer(expected,input_rows);
  }
  it("groups signed zeros as peers and rejects nonfinite keys") {
    input_rows=3; types[0]=(orm_sql_type){TURBODB_VALUE_DOUBLE,false};
    rows[0][0]=turbodb_f64(-0.0); rows[1][0]=turbodb_f64(0.0); rows[2][0]=turbodb_f64(1.0);
    spec_value.kind=ORM_SQL_RANK; check_equal(open_window(),TURBODB_STATUS_OK);
    const int64_t expected[]={1,1,3}; expect_integer(expected,input_rows); reset();
    rows[2][0]=turbodb_f64(INFINITY); check_equal(open_window(),TURBODB_STATUS_OK); expect_locked(TURBODB_STATUS_TYPE_ERROR);
  }
  it("uses all order columns to determine peers including boolean keys") {
    input_rows=4; types[2]=(orm_sql_type){TURBODB_VALUE_BOOLEAN,false};
    for(size_t i=0;i<input_rows;++i) { rows[i][0]=turbodb_i64(1); rows[i][2]=turbodb_bool(i%2!=0); }
    orders[1]=(orm_sql_scan_order){.slot=2,.descending=true}; spec_value.order_count=2; spec_value.kind=ORM_SQL_RANK;
    check_equal(open_window(),TURBODB_STATUS_OK); const int64_t expected[]={1,1,3,3}; expect_integer(expected,input_rows);
  }
  it("copies multi-column partition and order specifications at open") {
    input_rows=4; partitions[0]=1; partitions[1]=2; spec_value.partition_count=2;
    for(size_t i=0;i<input_rows;++i) { rows[i][0]=turbodb_i64(1); rows[i][1]=i<2 ? turbodb_null() : turbodb_i64(1); rows[i][2]=turbodb_i64(0); }
    check_equal(open_window(),TURBODB_STATUS_OK); partitions[0]=SIZE_MAX; partitions[1]=SIZE_MAX;
    orders[0].slot=SIZE_MAX; spec_value.kind=ORM_SQL_NTILE; spec_value.buckets=0;
    const int64_t expected[]={1,2,1,2}; expect_integer(expected,input_rows);
  }
  it("owns carried TEXT and BLOB bytes across source reuse and EOF") {
    const size_t sorted_ids[]={1,6,4,0,3,8,2,7,5};
    for(turbodb_value_kind_t kind=TURBODB_VALUE_TEXT;kind<=TURBODB_VALUE_BLOB;++kind) {
      types[2]=(orm_sql_type){kind,false}; check_equal(open_window(),TURBODB_STATUS_OK);
      for(size_t i=0;i<TEST_ROWS;++i) {
        const orm_sql_scan_row row=next_window(); check_equal(row.values[2].kind,kind);
        if(kind==TURBODB_VALUE_TEXT) { check_equal(row.values[2].data.text_value.len,1u); check_equal(row.values[2].data.text_value.data[0],(char)('a'+sorted_ids[i])); }
        else { check_equal(row.values[2].data.blob_value.size,1u); check_equal(((const char *)row.values[2].data.blob_value.data)[0],(char)('a'+sorted_ids[i])); }
      }
      reset();
    }
  }
  it("chains different window orders while preserving the prior result on each row") {
    check_equal(open_window(),TURBODB_STATUS_OK);
    orm_sql_scan_order reversed={.slot=0,.descending=true};
    const orm_sql_window_spec second_spec={.kind=ORM_SQL_RANK,.orders=&reversed,.order_count=1};
    check_equal(orm_tidesdb_sql_window_open(orm_tidesdb_sql_window_source(&window),&second_spec,&second,&error),TURBODB_STATUS_OK);
    const int64_t previous[]={9,7,8,4,5,6,3,1,2}, ranks[]={1,2,2,4,4,4,7,8,8};
    for(size_t i=0;i<TEST_ROWS;++i) {
      orm_sql_scan_row row; check_equal(orm_tidesdb_sql_window_next(&second,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.count,5u); check_equal(row.values[3].data.int64_value,previous[i]); check_equal(row.values[4].data.int64_value,ranks[i]);
    }
    check_equal(calls,TEST_ROWS+1u); check_true(window.source.active);
    check_equal(orm_tidesdb_sql_window_close(&window,&error),TURBODB_STATUS_BUSY);
  }
  it("computes windows before downstream offset and limit") {
    check_equal(open_window(),TURBODB_STATUS_OK); const size_t projection[]={0,3};
    const orm_sql_scan_spec page={.projection=projection,.projection_count=2,.offset=2,.limit=1};
    check_equal(orm_tidesdb_sql_scan_open_source(orm_tidesdb_sql_window_source(&window),&page,&budget,&consumer,&error),TURBODB_STATUS_OK);
    orm_sql_scan_row row; check_equal(orm_tidesdb_sql_scan_next(&consumer,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,2); check_equal(row.values[1].data.int64_value,3);
    check_equal(calls,TEST_ROWS+1u); check_equal(orm_tidesdb_sql_scan_next(&consumer,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_DONE);
  }
  it("holds owners while a downstream consumer is active and reads nothing for limit zero") {
    check_equal(open_window(),TURBODB_STATUS_OK); const size_t projection[]={3};
    const orm_sql_scan_spec page={.projection=projection,.projection_count=1,.limit=0};
    check_equal(orm_tidesdb_sql_scan_open_source(orm_tidesdb_sql_window_source(&window),&page,&budget,&consumer,&error),TURBODB_STATUS_OK);
    orm_sql_scan_row row={0}; const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_tidesdb_sql_window_next(&window,&row,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_window_cancel(&window,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_window_close(&window,&error),TURBODB_STATUS_BUSY);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_true(source.active);
    check_equal(orm_tidesdb_sql_scan_next(&consumer,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
    check_equal(calls,0u); check_equal(orm_tidesdb_sql_scan_close(&consumer,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_window_cancel(&window,&error),TURBODB_STATUS_OK);
    check_equal(next_window().state,ORM_SQL_SCAN_CANCELLED); check_equal(calls,0u);
  }
  it("rejects input callback reentry without altering the running operation") {
    reenter=true; check_equal(open_window(),TURBODB_STATUS_OK);
    const int64_t expected[]={1,2,3,4,5,6,7,8,9}; expect_integer(expected,TEST_ROWS);
  }
  it("locks a source error before publishing any window row") {
    fail_read=4; check_equal(open_window(),TURBODB_STATUS_OK); expect_locked(TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(calls,4u); check_true(source.active);
  }
  it("preserves a bare source status even when the callback omits diagnostic text") {
    fail_read=4; bare_error=true; check_equal(open_window(),TURBODB_STATUS_OK); expect_locked(TURBODB_STATUS_DATASTORE_ERROR);
  }
  it("owns and cleans partial unsorted snapshots when the source fails") {
    spec_value.order_count=0; fail_read=4; check_equal(open_window(),TURBODB_STATUS_OK);
    expect_locked(TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(vec_size(&window.rows.snapshots),3u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],3u);
  }
  it("cancels after publication without additional source reads and preserves DONE") {
    check_equal(open_window(),TURBODB_STATUS_OK); check_equal(next_window().state,ORM_SQL_SCAN_ROW);
    const size_t reads=calls; const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_tidesdb_sql_window_cancel(&window,&error),TURBODB_STATUS_OK);
    check_equal(next_window().state,ORM_SQL_SCAN_CANCELLED); check_equal(next_window().state,ORM_SQL_SCAN_CANCELLED);
    check_equal(calls,reads); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    reset(); input_rows=0; check_equal(open_window(),TURBODB_STATUS_OK); check_equal(next_window().state,ORM_SQL_SCAN_DONE);
    check_equal(orm_tidesdb_sql_window_cancel(&window,&error),TURBODB_STATUS_OK); check_equal(next_window().state,ORM_SQL_SCAN_DONE);
  }
  it("validates carried types on empty input and enforces the appended output plan width") {
    input_rows=0; types[2]=(orm_sql_type){(turbodb_value_kind_t)-1,false};
    check_equal(open_window(),TURBODB_STATUS_TYPE_ERROR); check_false(source.active); check_null(window.budget);
    types[2]=(orm_sql_type){TURBODB_VALUE_INT64,false};
    limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES]=TEST_COLUMNS; reset();
    check_equal(open_window(),TURBODB_STATUS_LIMIT_EXCEEDED); check_false(source.active); check_equal(calls,0u);
    limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES]=TEST_COLUMNS+1u; reset();
    check_equal(open_window(),TURBODB_STATUS_OK); check_equal(next_window().state,ORM_SQL_SCAN_DONE);
  }
  it("treats NULL-only order keys as peers and exposes no source before open") {
    check_null(orm_tidesdb_sql_window_source(&window)); types[0]=(orm_sql_type){TURBODB_VALUE_NULL,true};
    for(size_t i=0;i<TEST_ROWS;++i) rows[i][0]=turbodb_null();
    spec_value.kind=ORM_SQL_RANK; check_equal(open_window(),TURBODB_STATUS_OK);
    const int64_t expected[]={1,1,1,1,1,1,1,1,1}; expect_integer(expected,TEST_ROWS);
  }
  it("rejects invalid function bucket key and width specifications before source admission") {
    spec_value.kind=(orm_sql_window_kind)-1; check_equal(open_window(),TURBODB_STATUS_UNSUPPORTED); check_false(source.active);
    spec_value.kind=ORM_SQL_NTILE; check_equal(open_window(),TURBODB_STATUS_INVALID_ARGUMENT);
    spec_value.buckets=(UINT64_C(1)<<63)+1; check_equal(open_window(),TURBODB_STATUS_INVALID_ARGUMENT);
    spec_value.kind=ORM_SQL_ROW_NUMBER; spec_value.buckets=1; check_equal(open_window(),TURBODB_STATUS_INVALID_ARGUMENT);
    spec_value.buckets=0; orders[0].slot=TEST_COLUMNS; check_equal(open_window(),TURBODB_STATUS_INVALID_ARGUMENT);
    orders[0].slot=0; orders[0].expression.count=1; check_equal(open_window(),TURBODB_STATUS_UNSUPPORTED);
    orders[0].expression.count=0; types[0]=(orm_sql_type){TURBODB_VALUE_TEXT,true}; check_equal(open_window(),TURBODB_STATUS_UNSUPPORTED);
    types[0]=(orm_sql_type){TURBODB_VALUE_INT64,true}; source.columns=SIZE_MAX; check_equal(open_window(),TURBODB_STATUS_LIMIT_EXCEEDED);
    source.columns=TEST_COLUMNS; spec_value.partition_count=SIZE_MAX; spec_value.order_count=1;
    check_equal(open_window(),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(calls,0u); check_false(source.active);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
  }
  it("rejects an already leased input without releasing its existing consumer") {
    check_equal(open_window(),TURBODB_STATUS_OK); const orm_sql_window_spec other={.kind=ORM_SQL_RANK};
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_tidesdb_sql_window_open(&source,&other,&second,&error),TURBODB_STATUS_BUSY);
    check_true(source.active); check_null(second.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("recovers every constructor allocation and resize failure without leaking admission") {
    spec_value.partition_count=1; check_equal(open_window(),TURBODB_STATUS_OK);
    const size_t reserves=reserve_calls, resizes=resize_calls; reset();
    for(size_t mode=0;mode<2;++mode) {
      const size_t count=mode ? resizes : reserves;
      for(size_t i=1;i<=count;++i) {
        if(mode) fail_resize=i; else fail_reserve=i;
        check_equal(open_window(),TURBODB_STATUS_OUT_OF_MEMORY); check_null(window.budget);
        check_false(source.active); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u); reset();
      }
    }
  }
  it("locks every materialization allocation and resize failure and releases partial snapshots") {
    check_equal(open_window(),TURBODB_STATUS_OK); const size_t before_reserves=reserve_calls, before_resizes=resize_calls;
    check_equal(next_window().state,ORM_SQL_SCAN_ROW);
    const size_t reserves=reserve_calls-before_reserves, resizes=resize_calls-before_resizes; reset();
    for(size_t mode=0;mode<2;++mode) {
      const size_t count=mode ? resizes : reserves;
      for(size_t i=1;i<=count;++i) {
        check_equal(open_window(),TURBODB_STATUS_OK);
        if(mode) fail_resize=resize_calls+i; else fail_reserve=reserve_calls+i;
        expect_locked(TURBODB_STATUS_OUT_OF_MEMORY); reset();
      }
    }
  }
  it("does not publish a sorted prefix if CSTL scratch allocation fails") {
    check_equal(open_window(),TURBODB_STATUS_OK); fail_sort=true; expect_locked(TURBODB_STATUS_OUT_OF_MEMORY);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],TEST_ROWS);
  }
  it("refunds materialized rows when either input sort or output snapshots exceed the shared quota") {
    for(size_t cap=1;cap<TEST_ROWS*2u;++cap) {
      limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=cap; reset();
      check_equal(open_window(),TURBODB_STATUS_OK); expect_locked(TURBODB_STATUS_LIMIT_EXCEEDED); reset();
    }
    limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=TEST_ROWS*2u; reset();
    check_equal(open_window(),TURBODB_STATUS_OK); check_equal(next_window().state,ORM_SQL_SCAN_ROW);
  }
  it("recovers every constructor and first-result execution step boundary") {
    spec_value.partition_count=1; check_equal(open_window(),TURBODB_STATUS_OK);
    const uint64_t construction=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(next_window().state,ORM_SQL_SCAN_ROW);
    const uint64_t first=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    for(uint64_t cap=1;cap<first;++cap) {
      limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=cap; reset();
      const turbodb_status_t status=open_window();
      if(cap<construction) { check_equal(status,TURBODB_STATUS_LIMIT_EXCEEDED); check_null(window.budget); check_false(source.active); }
      else { check_equal(status,TURBODB_STATUS_OK); expect_locked(TURBODB_STATUS_LIMIT_EXCEEDED); }
    }
    limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=first; reset();
    check_equal(open_window(),TURBODB_STATUS_OK); check_equal(next_window().state,ORM_SQL_SCAN_ROW);
    expect_locked(TURBODB_STATUS_LIMIT_EXCEEDED);
  }
  it("refunds every insufficient work-byte budget including sorting scratch and payload copies") {
    types[2]=(orm_sql_type){TURBODB_VALUE_TEXT,false}; check_equal(open_window(),TURBODB_STATUS_OK);
    check_equal(next_window().state,ORM_SQL_SCAN_ROW); const uint64_t peak=budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(uint64_t cap=1;cap<peak;++cap) {
      limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=cap; reset();
      const turbodb_status_t status=open_window();
      if(status==TURBODB_STATUS_OK) expect_locked(TURBODB_STATUS_LIMIT_EXCEEDED);
      else { check_equal(status,TURBODB_STATUS_LIMIT_EXCEEDED); check_null(window.budget); check_false(source.active); }
    }
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=peak; reset();
    check_equal(open_window(),TURBODB_STATUS_OK); check_equal(next_window().state,ORM_SQL_SCAN_ROW);
  }
}
