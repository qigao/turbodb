#include "union.h"
#include <tinytest.h>
#include <string.h>
#include <math.h>
#include <cstl/sort.h>

static size_t reserves,resizes,fail_reserve,fail_resize;
static size_t sorts,fail_sort;
static stl_status union_test_sort(void *base,size_t count,const cmeta_type_desc *type,size_t bytes) {
  return ++sorts==fail_sort?STL_OUT_OF_MEMORY:stable_sort(base,count,type,bytes);
}
static stl_status union_test_reserve(vec_t *v,size_t n) { return ++reserves==fail_reserve?STL_OUT_OF_MEMORY:vec_reserve(v,n); }
static stl_status union_test_resize(vec_t *v,size_t n) { return ++resizes==fail_resize?STL_OUT_OF_MEMORY:vec_resize(v,n); }
#define vec_reserve union_test_reserve
#define vec_resize union_test_resize
#define stable_sort union_test_sort
#include "../../src/work.c"
#include "../../src/rows.c"
#include "../../src/expr.c"
#include "../../src/scan.c"
#include "../../src/union.c"
#undef vec_reserve
#undef vec_resize
#undef stable_sort

enum { TEST_SOURCES=3,TEST_ROWS=4,TEST_COLUMNS=2,TEST_LIMIT=65536,TEST_WORK=4*1024*1024 };
typedef struct fixture_source {
  turbodb_value_t values[TEST_ROWS][TEST_COLUMNS],buffer[TEST_COLUMNS];
  char text[2]; size_t count,position,calls,fail_at;
} fixture_source;
static fixture_source fixtures[TEST_SOURCES];
static orm_sql_type types[TEST_SOURCES][TEST_COLUMNS];
static orm_sql_row_source sources[TEST_SOURCES];
static orm_sql_union run,parent;
static orm_sql_scan downstream;
static orm_tidesdb_sql_budget budget;
static orm_sql_budget_limits limits;
static turbodb_error_t error;
static turbodb_status_t input_next(void *context,const turbodb_value_t **out,turbodb_error_t *e) {
  fixture_source *source=context; ++source->calls;
  if(source->calls==source->fail_at) { tdsql_error_set(e,TURBODB_STATUS_DATASTORE_ERROR,"union input failure"); return TURBODB_STATUS_DATASTORE_ERROR; }
  source->text[0]='!'; memset(source->buffer,0,sizeof(source->buffer));
  if(source->position==source->count) { *out=NULL; return TURBODB_STATUS_OK; }
  orm_sql_budget_amount amount={0}; amount.value[ORM_SQL_BUDGET_READ_ROWS]=1; amount.value[ORM_SQL_BUDGET_READ_BYTES]=sizeof(source->buffer);
  const turbodb_status_t status=orm_tidesdb_sql_budget_reserve(&budget,&amount,e);
  if(status!=TURBODB_STATUS_OK) return status;
  memcpy(source->buffer,source->values[source->position],sizeof(source->buffer));
  source->text[0]=(char)('a'+source->position++); source->text[1]=0;
  for(size_t i=0;i<TEST_COLUMNS;++i) {
    if(source->buffer[i].kind==TURBODB_VALUE_TEXT) source->buffer[i]=turbodb_text(source->text);
    if(source->buffer[i].kind==TURBODB_VALUE_BLOB) source->buffer[i]=turbodb_blob(source->text,1);
  }
  *out=source->buffer; return TURBODB_STATUS_OK;
}
static turbodb_status_t open_union(orm_sql_union_kind kind) { return orm_tidesdb_sql_union_open(&sources[0],&sources[1],kind,&run,&error); }
static orm_sql_scan_row next(void) { orm_sql_scan_row row; check_equal(orm_tidesdb_sql_union_next(&run,&row,&error),TURBODB_STATUS_OK); return row; }
static void clean(void) {
  fail_reserve=fail_resize=fail_sort=0;
  check_equal(orm_tidesdb_sql_scan_close(&downstream,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_union_close(&parent,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_union_close(&run,&error),TURBODB_STATUS_OK);
  for(size_t i=0;i<TEST_SOURCES;++i) check_false(sources[i].active);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
  check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
}
static void reset(void) {
  clean(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_init(&budget,&limits,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
  for(size_t i=0;i<TEST_SOURCES;++i) fixtures[i].position=fixtures[i].calls=0;
  reserves=resizes=sorts=0;
}
static void locked(turbodb_status_t status) {
  orm_sql_scan_row row={.count=99,.state=ORM_SQL_SCAN_CANCELLED};
  check_equal(orm_tidesdb_sql_union_next(&run,&row,&error),status); check_equal(row.count,99u);
  const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
  check_equal(orm_tidesdb_sql_union_cancel(&run,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_union_next(&run,&row,&error),status); check_equal(row.state,ORM_SQL_SCAN_CANCELLED);
  check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
}
static const int64_t precision_edge=INT64_C(9007199254740992);
static void mixed_precision(void) {
  types[0][0]=(orm_sql_type){TURBODB_VALUE_INT64,true}; types[1][0]=(orm_sql_type){TURBODB_VALUE_DOUBLE,true};
  fixtures[0].count=2; fixtures[1].count=1;
  fixtures[0].values[0][0]=turbodb_i64(precision_edge);
  fixtures[0].values[1][0]=turbodb_i64(precision_edge+1);
  fixtures[1].values[0][0]=turbodb_f64((double)precision_edge);
}
spec("TidesDB bounded UNION execution") {
  before_each() {
    reserves=resizes=fail_reserve=fail_resize=sorts=fail_sort=0; tdsql_error_init(&error);
    run=(orm_sql_union){0}; parent=(orm_sql_union){0}; downstream=(orm_sql_scan){0};
    limits=(orm_sql_budget_limits){0};
    for(size_t i=0;i<ORM_SQL_BUDGET_RESOURCE_COUNT;++i) limits.statement.value[i]=TEST_LIMIT;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=TEST_WORK;
    limits.transaction=(orm_sql_transaction_budget_amount){TEST_LIMIT,TEST_LIMIT,TEST_LIMIT};
    check_equal(orm_tidesdb_sql_budget_init(&budget,&limits,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
    for(size_t i=0;i<TEST_SOURCES;++i) {
      fixtures[i]=(fixture_source){.count=3};
      for(size_t j=0;j<TEST_COLUMNS;++j) types[i][j]=(orm_sql_type){TURBODB_VALUE_INT64,true};
      for(size_t j=0;j<TEST_ROWS;++j) { fixtures[i].values[j][0]=turbodb_i64((int64_t)j+1); fixtures[i].values[j][1]=turbodb_i64(10); }
      sources[i]=(orm_sql_row_source){&budget,types[i],TEST_COLUMNS,&fixtures[i],input_next,false};
    }
  }
  after_each() { clean(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); }
  group("common numeric set results") {
    it("admits both numeric directions and rejects narrowing before reading either input") {
      const turbodb_value_kind_t integers[]={TURBODB_VALUE_INT64,TURBODB_VALUE_UINT64};
      for(size_t kind=0;kind<2;++kind) for(size_t side=0;side<2;++side) {
        const orm_sql_type integer={integers[kind],side!=0},real={TURBODB_VALUE_DOUBLE,side==0};
        orm_sql_type result_type={0};
        check_equal(orm_tidesdb_sql_union_type(side?real:integer,side?integer:real,&result_type,&error),TURBODB_STATUS_OK);
        check_equal(result_type.kind,TURBODB_VALUE_DOUBLE); check_true(result_type.nullable);
      }
      mixed_precision(); orm_sql_type target[TEST_COLUMNS]={types[1][0],types[0][1]};
      target[0].nullable=false;
      check_equal(orm_sql_union_validate_as(&sources[0],&sources[1],ORM_SQL_UNION_ALL,target,&error),TURBODB_STATUS_TYPE_ERROR);
      target[0]=(orm_sql_type){TURBODB_VALUE_INT64,true};
      check_equal(orm_sql_union_validate_as(&sources[0],&sources[1],ORM_SQL_UNION_ALL,target,&error),TURBODB_STATUS_UNSUPPORTED);
      target[0]=(orm_sql_type){(turbodb_value_kind_t)(TURBODB_VALUE_BLOB+1),true};
      check_equal(orm_sql_union_validate_as(&sources[0],&sources[1],ORM_SQL_UNION_ALL,target,&error),TURBODB_STATUS_TYPE_ERROR);
      types[1][0].kind=TURBODB_VALUE_UINT64; fixtures[1].values[0][0]=turbodb_u64(UINT64_MAX);
      check_equal(open_union(ORM_SQL_UNION_ALL),TURBODB_STATUS_UNSUPPORTED);
      target[0]=(orm_sql_type){TURBODB_VALUE_DOUBLE,true};
      check_equal(orm_sql_union_open_as(&sources[0],&sources[1],ORM_SQL_UNION_ALL,target,&run,&error),TURBODB_STATUS_OK);
      check_equal(types[0][0].kind,TURBODB_VALUE_INT64); check_equal(types[1][0].kind,TURBODB_VALUE_UINT64);
      check_equal(fixtures[0].calls+fixtures[1].calls,0u);
      for(size_t row=0;row<3;++row) check_equal(next().values[0].kind,TURBODB_VALUE_DOUBLE);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("converts before all six duplicate rules while preserving exact pure integers") {
      const size_t rounded[]={3,1,1,1,1,0},exact[]={3,2,1,1,1,1};
      for(size_t native=0;native<2;++native) for(size_t mode=0;mode<6;++mode) {
        reset(); mixed_precision();
        if(native) { types[1][0].kind=TURBODB_VALUE_INT64; fixtures[1].values[0][0]=turbodb_i64(precision_edge); }
        check_equal(open_union((orm_sql_union_kind)mode),TURBODB_STATUS_OK);
        check_equal(run.source.types[0].kind,native?TURBODB_VALUE_INT64:TURBODB_VALUE_DOUBLE);
        size_t count=0; orm_sql_scan_row row;
        while((row=next()).state==ORM_SQL_SCAN_ROW) {
          check_equal(row.values[0].kind,native?TURBODB_VALUE_INT64:TURBODB_VALUE_DOUBLE);
          if(!native) check_equal(row.values[0].data.double_value,(double)precision_edge);
          else if(mode>=ORM_SQL_EXCEPT_ALL) check_equal(row.values[0].data.int64_value,precision_edge+1);
          ++count;
        }
        check_equal(count,native?exact[mode]:rounded[mode]); check_equal(row.state,ORM_SQL_SCAN_DONE);
        check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],3u);
        check_equal(fixtures[0].values[1][0].data.int64_value,precision_edge+1);
      }
    }
    it("sorts every converted tuple key before intersection and difference") {
      for(size_t mode=ORM_SQL_INTERSECT_ALL;mode<=ORM_SQL_EXCEPT_DISTINCT;++mode) {
        reset(); mixed_precision(); fixtures[1].count=2;
        fixtures[0].values[0][0]=turbodb_i64(precision_edge+1); fixtures[0].values[0][1]=turbodb_i64(0);
        fixtures[0].values[1][0]=turbodb_i64(precision_edge); fixtures[0].values[1][1]=turbodb_i64(100);
        fixtures[1].values[0][1]=turbodb_i64(0);
        fixtures[1].values[1][0]=turbodb_f64((double)precision_edge); fixtures[1].values[1][1]=turbodb_i64(100);
        check_equal(open_union((orm_sql_union_kind)mode),TURBODB_STATUS_OK);
        if(mode<ORM_SQL_EXCEPT_ALL) for(size_t row=0;row<2;++row) {
          const orm_sql_scan_row output=next(); check_equal(output.values[0].kind,TURBODB_VALUE_DOUBLE);
          check_equal(output.values[0].data.double_value,(double)precision_edge);
          check_equal(output.values[1].data.int64_value,row?100:0);
        }
        check_equal(next().state,ORM_SQL_SCAN_DONE);
      }
    }
    it("streams converted U64 extremes signed zero and NULL without allocating after open") {
      for(size_t side=0;side<2;++side) {
        reset(); fixtures[0].count=fixtures[1].count=3;
        types[side][0]=(orm_sql_type){TURBODB_VALUE_UINT64,true}; types[1-side][0]=(orm_sql_type){TURBODB_VALUE_DOUBLE,true};
        fixtures[side].values[0][0]=turbodb_u64(UINT64_MAX); fixtures[side].values[1][0]=turbodb_u64(0);
        fixtures[1-side].values[0][0]=turbodb_f64((double)UINT64_MAX); fixtures[1-side].values[1][0]=turbodb_f64(-0.0);
        fixtures[0].values[2][0]=fixtures[1].values[2][0]=turbodb_null();
        check_equal(open_union(ORM_SQL_UNION_ALL),TURBODB_STATUS_OK);
        const size_t allocated[]={reserves,resizes}; fail_reserve=reserves+1; fail_resize=resizes+1;
        for(size_t row=0;row<6;++row) {
          const orm_sql_scan_row output=next();
          check_equal(output.values[0].kind,row%3==2?TURBODB_VALUE_NULL:TURBODB_VALUE_DOUBLE);
          if(row%3!=2) check_equal(output.values[0].data.double_value,row%3?0.0:(double)UINT64_MAX);
        }
        check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(reserves,allocated[0]); check_equal(resizes,allocated[1]);
        reset(); check_equal(open_union(ORM_SQL_UNION_DISTINCT),TURBODB_STATUS_OK);
        check_equal(next().values[0].kind,TURBODB_VALUE_NULL); check_equal(next().values[0].data.double_value,0.0);
        check_equal(next().values[0].data.double_value,(double)UINT64_MAX); check_equal(next().state,ORM_SQL_SCAN_DONE);
      }
    }
    it("validates original kinds nullability and finite reals before conversion") {
      for(size_t failure=0;failure<3;++failure) for(size_t mode=0;mode<6;++mode) {
        reset(); mixed_precision();
        if(!failure) fixtures[0].values[0][0]=turbodb_f64((double)precision_edge);
        else if(failure==1) { types[0][0].nullable=false; fixtures[0].values[0][0]=turbodb_null(); }
        else { types[0][0].kind=TURBODB_VALUE_DOUBLE; types[1][0].kind=TURBODB_VALUE_INT64;
          fixtures[0].values[0][0]=turbodb_f64(NAN); fixtures[0].values[1][0]=turbodb_f64(1.0);
          fixtures[1].values[0][0]=turbodb_i64(1); }
        check_equal(open_union((orm_sql_union_kind)mode),TURBODB_STATUS_OK); locked(TURBODB_STATUS_TYPE_ERROR);
      }
    }
    it("determines nullable by set operation without narrowing converted inputs") {
      for(size_t mode=0;mode<6;++mode) for(size_t mask=0;mask<4;++mask) {
        reset(); mixed_precision(); types[0][0].nullable=(mask&1)!=0; types[1][0].nullable=(mask&2)!=0;
        if(types[0][0].nullable) fixtures[0].values[0][0]=turbodb_null();
        if(types[1][0].nullable) fixtures[1].values[0][0]=turbodb_null();
        const bool expected=mode<ORM_SQL_INTERSECT_ALL?mask!=0:
          mode<ORM_SQL_EXCEPT_ALL?mask==3:types[0][0].nullable;
        check_equal(open_union((orm_sql_union_kind)mode),TURBODB_STATUS_OK);
        check_equal(run.source.types[0].kind,TURBODB_VALUE_DOUBLE); check_equal(run.source.types[0].nullable,expected);
        orm_sql_scan_row row;
        while((row=next()).state==ORM_SQL_SCAN_ROW) {
          if(row.values[0].kind==TURBODB_VALUE_NULL) check_true(expected);
          else { check_equal(row.values[0].kind,TURBODB_VALUE_DOUBLE); check_equal(row.values[0].data.double_value,(double)precision_edge); }
        }
      }
    }
    it("refunds all mixed set allocation sort and conversion step failures") {
      for(size_t mode=0;mode<6;++mode) {
        reset(); mixed_precision(); types[1][0].nullable=false; check_equal(open_union((orm_sql_union_kind)mode),TURBODB_STATUS_OK);
        while(next().state==ORM_SQL_SCAN_ROW) {}
        const size_t counts[]={reserves,resizes,sorts};
        const uint64_t cost=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
        for(size_t phase=0;phase<3;++phase) for(size_t point=1;point<=counts[phase];++point) {
          reset(); mixed_precision(); types[1][0].nullable=false;
          if(!phase) fail_reserve=point; else if(phase==1) fail_resize=point; else fail_sort=point;
          turbodb_status_t status=open_union((orm_sql_union_kind)mode); orm_sql_scan_row row={0};
          while(status==TURBODB_STATUS_OK) {
            status=orm_tidesdb_sql_union_next(&run,&row,&error);
            if(status==TURBODB_STATUS_OK && row.state!=ORM_SQL_SCAN_ROW) break;
          }
          check_equal(status,TURBODB_STATUS_OUT_OF_MEMORY); clean();
        }
        for(uint64_t point=0;point<cost;++point) {
          reset(); mixed_precision(); types[1][0].nullable=false;
          budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=point;
          turbodb_status_t status=open_union((orm_sql_union_kind)mode); orm_sql_scan_row row={0};
          while(status==TURBODB_STATUS_OK) {
            status=orm_tidesdb_sql_union_next(&run,&row,&error);
            if(status==TURBODB_STATUS_OK && row.state!=ORM_SQL_SCAN_ROW) break;
          }
          check_equal(status,TURBODB_STATUS_LIMIT_EXCEEDED); clean();
        }
      }
    }
  }
  group("INTERSECT EXCEPT multiset execution") {
    it("applies ALL occurrence counts and excludes every matching EXCEPT DISTINCT tuple") {
      fixtures[0].count=3; fixtures[1].count=1;
      for(size_t row=0;row<3;++row) fixtures[0].values[row][0]=turbodb_i64(1);
      const size_t expected[]={1,1,2,0};
      for(size_t mode=0;mode<4;++mode) {
        reset(); check_equal(open_union((orm_sql_union_kind)(ORM_SQL_INTERSECT_ALL+mode)),TURBODB_STATUS_OK);
        size_t count=0; orm_sql_scan_row row;
        while((row=next()).state==ORM_SQL_SCAN_ROW) { check_equal(row.values[0].data.int64_value,1); ++count; }
        check_equal(count,expected[mode]); check_equal(row.state,ORM_SQL_SCAN_DONE);
        check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],4u);
      }
    }
    it("compares whole tuples and keeps duplicate NULL tuples for ALL") {
      fixtures[0].count=fixtures[1].count=4;
      for(size_t side=0;side<2;++side) {
        fixtures[side].values[0][0]=fixtures[side].values[1][0]=turbodb_null();
        fixtures[side].values[2][0]=fixtures[side].values[3][0]=turbodb_i64(2);
      }
      fixtures[1].values[1][1]=turbodb_i64(20); fixtures[1].values[3][1]=turbodb_i64(20);
      for(size_t mode=0;mode<4;++mode) {
        reset(); check_equal(open_union((orm_sql_union_kind)(ORM_SQL_INTERSECT_ALL+mode)),TURBODB_STATUS_OK);
        size_t nulls=0,twos=0; orm_sql_scan_row row;
        while((row=next()).state==ORM_SQL_SCAN_ROW) {
          check_equal(row.values[1].data.int64_value,10);
          if(row.values[0].kind==TURBODB_VALUE_NULL) ++nulls; else { check_equal(row.values[0].data.int64_value,2); ++twos; }
        }
        check_equal(nulls,mode==3?0u:1u); check_equal(twos,mode==3?0u:1u);
      }
    }
    it("handles empty sides while validating and preparing both inputs") {
      for(size_t empty=0;empty<2;++empty) for(size_t mode=0;mode<4;++mode) {
        reset(); fixtures[0].count=empty?3:0; fixtures[1].count=empty?0:3;
        check_equal(open_union((orm_sql_union_kind)(ORM_SQL_INTERSECT_ALL+mode)),TURBODB_STATUS_OK);
        size_t count=0; while(next().state==ORM_SQL_SCAN_ROW) ++count;
        check_equal(count,empty && mode>=2?3u:0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],3u);
      }
    }
    it("preserves exact U64 extremes and compares signed DOUBLE zero and BOOL tuples") {
      for(size_t side=0;side<2;++side) {
        types[side][0]=(orm_sql_type){TURBODB_VALUE_UINT64,false}; types[side][1]=(orm_sql_type){TURBODB_VALUE_BOOLEAN,false};
        fixtures[side].count=2; fixtures[side].values[0][0]=turbodb_u64(UINT64_MAX); fixtures[side].values[1][0]=turbodb_u64(UINT64_MAX-1);
        fixtures[side].values[0][1]=turbodb_bool(true); fixtures[side].values[1][1]=turbodb_bool(false);
      }
      check_equal(open_union(ORM_SQL_INTERSECT_ALL),TURBODB_STATUS_OK);
      const orm_sql_scan_row first=next(); check_equal(first.values[0].data.uint64_value,UINT64_MAX-1);
      check_false(first.values[1].data.boolean_value); check_equal(next().values[0].data.uint64_value,UINT64_MAX);
      check_equal(next().state,ORM_SQL_SCAN_DONE); reset();
      for(size_t side=0;side<2;++side) {
        types[side][0].kind=TURBODB_VALUE_DOUBLE;
        for(size_t row=0;row<2;++row) fixtures[side].values[row][0]=turbodb_f64(side?-0.0:0.0);
      }
      check_equal(open_union(ORM_SQL_EXCEPT_ALL),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("widens static NULL schema without changing source validation or tuple null equality") {
      types[0][0]=(orm_sql_type){TURBODB_VALUE_NULL,true};
      for(size_t row=0;row<3;++row) fixtures[0].values[row][0]=turbodb_null();
      fixtures[1].values[0][0]=turbodb_null(); check_equal(open_union(ORM_SQL_INTERSECT_ALL),TURBODB_STATUS_OK);
      check_equal(run.source.types[0].kind,TURBODB_VALUE_INT64); check_true(run.source.types[0].nullable);
      check_equal(next().values[0].kind,TURBODB_VALUE_NULL); check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("retains source and output leases through downstream EOF and rejects reentrant control") {
      check_equal(open_union(ORM_SQL_INTERSECT_DISTINCT),TURBODB_STATUS_OK);
      const size_t projection=0; const orm_sql_scan_spec spec={.projection=&projection,.projection_count=1,.limit=UINT64_MAX};
      check_equal(orm_tidesdb_sql_scan_open_source(&run.source,&spec,&budget,&downstream,&error),TURBODB_STATUS_OK);
      orm_sql_scan_row row={.count=99}; check_equal(orm_tidesdb_sql_union_next(&run,&row,&error),TURBODB_STATUS_BUSY);
      check_equal(row.count,99u); check_equal(orm_tidesdb_sql_union_cancel(&run,&error),TURBODB_STATUS_BUSY);
      do { check_equal(orm_tidesdb_sql_scan_next(&downstream,&row,&error),TURBODB_STATUS_OK); } while(row.state==ORM_SQL_SCAN_ROW);
      check_equal(row.state,ORM_SQL_SCAN_DONE);
      check_equal(orm_tidesdb_sql_union_close(&run,&error),TURBODB_STATUS_BUSY); check_true(sources[0].active); check_true(sources[1].active);
      check_equal(orm_tidesdb_sql_scan_close(&downstream,&error),TURBODB_STATUS_OK); clean();
    }
    it("makes all modes lazy under cancellation and downstream LIMIT zero") {
      for(size_t mode=0;mode<4;++mode) for(size_t cancel=0;cancel<2;++cancel) {
        reset(); check_equal(open_union((orm_sql_union_kind)(ORM_SQL_INTERSECT_ALL+mode)),TURBODB_STATUS_OK);
        if(cancel) { check_equal(orm_tidesdb_sql_union_cancel(&run,&error),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_CANCELLED); }
        else {
          const size_t projection=0; const orm_sql_scan_spec spec={.projection=&projection,.projection_count=1,.limit=0};
          check_equal(orm_tidesdb_sql_scan_open_source(&run.source,&spec,&budget,&downstream,&error),TURBODB_STATUS_OK);
          orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_scan_next(&downstream,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
        }
        check_equal(fixtures[0].calls+fixtures[1].calls,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
      }
    }
    it("rejects byte sets invalid modes and source errors without a first-row prefix") {
      for(size_t mode=0;mode<4;++mode) {
        reset(); fixtures[1].fail_at=2; check_equal(open_union((orm_sql_union_kind)(ORM_SQL_INTERSECT_ALL+mode)),TURBODB_STATUS_OK);
        locked(TURBODB_STATUS_DATASTORE_ERROR);
      }
      reset(); types[0][0]=types[1][0]=(orm_sql_type){TURBODB_VALUE_TEXT,true};
      check_equal(open_union(ORM_SQL_EXCEPT_ALL),TURBODB_STATUS_UNSUPPORTED); check_null(run.budget);
      check_equal(open_union((orm_sql_union_kind)(ORM_SQL_EXCEPT_DISTINCT+1)),TURBODB_STATUS_INVALID_ARGUMENT);
    }
    it("shares candidate row and work limits across both sorted inputs") {
      for(size_t mode=0;mode<4;++mode) for(size_t resource=0;resource<2;++resource) {
        reset(); check_equal(open_union((orm_sql_union_kind)(ORM_SQL_INTERSECT_ALL+mode)),TURBODB_STATUS_OK);
        if(resource) budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
        else budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=5;
        locked(TURBODB_STATUS_LIMIT_EXCEEDED); clean();
      }
    }
    it("refunds every set allocation and both input sort failures") {
      for(size_t mode=0;mode<4;++mode) {
        reset(); check_equal(open_union((orm_sql_union_kind)(ORM_SQL_INTERSECT_ALL+mode)),TURBODB_STATUS_OK);
        while(next().state==ORM_SQL_SCAN_ROW) {}
        const size_t counts[]={reserves,resizes,sorts};
        for(size_t phase=0;phase<3;++phase) for(size_t point=1;point<=counts[phase];++point) {
          reset(); if(!phase) fail_reserve=point; else if(phase==1) fail_resize=point; else fail_sort=point;
          turbodb_status_t status=open_union((orm_sql_union_kind)(ORM_SQL_INTERSECT_ALL+mode));
          orm_sql_scan_row row={0};
          while(status==TURBODB_STATUS_OK) {
            status=orm_tidesdb_sql_union_next(&run,&row,&error);
            if(status==TURBODB_STATUS_OK && row.state!=ORM_SQL_SCAN_ROW) break;
          }
          check_equal(status,TURBODB_STATUS_OUT_OF_MEMORY); clean();
        }
      }
    }
    it("checks every set construction preparation comparison and advancement step") {
      for(size_t mode=0;mode<4;++mode) {
        reset(); check_equal(open_union((orm_sql_union_kind)(ORM_SQL_INTERSECT_ALL+mode)),TURBODB_STATUS_OK);
        while(next().state==ORM_SQL_SCAN_ROW) {}
        const uint64_t cost=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
        for(uint64_t point=0;point<cost;++point) {
          reset(); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=point;
          turbodb_status_t status=open_union((orm_sql_union_kind)(ORM_SQL_INTERSECT_ALL+mode)); orm_sql_scan_row row={0};
          while(status==TURBODB_STATUS_OK) {
            status=orm_tidesdb_sql_union_next(&run,&row,&error);
            if(status==TURBODB_STATUS_OK && row.state!=ORM_SQL_SCAN_ROW) break;
          }
          check_equal(status,TURBODB_STATUS_LIMIT_EXCEEDED); clean();
        }
      }
    }
  }
  it("streams ALL left then right preserving duplicates without materializing or double charging reads") {
    check_equal(open_union(ORM_SQL_UNION_ALL),TURBODB_STATUS_OK); check_equal(fixtures[0].calls+fixtures[1].calls,0u);
    const size_t allocations=reserves;
    for(size_t i=0;i<6;++i) {
      const orm_sql_scan_row row=next(); check_equal(row.count,TEST_COLUMNS); check_equal(row.values[0].data.int64_value,(int64_t)(i%3+1));
      if(i<3) check_equal(fixtures[1].calls,0u);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(reserves,allocations); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],6u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u); check_true(sources[0].active); check_true(sources[1].active);
  }
  it("deduplicates complete tuples including NULL but preserves different second columns") {
    fixtures[0].values[0][0]=fixtures[1].values[0][0]=turbodb_null(); fixtures[1].values[1][1]=turbodb_i64(20);
    check_equal(open_union(ORM_SQL_UNION_DISTINCT),TURBODB_STATUS_OK);
    orm_sql_scan_row row=next(); check_equal(row.values[0].kind,TURBODB_VALUE_NULL);
    row=next(); check_equal(row.values[0].data.int64_value,2); check_equal(row.values[1].data.int64_value,10);
    row=next(); check_equal(row.values[0].data.int64_value,2); check_equal(row.values[1].data.int64_value,20);
    row=next(); check_equal(row.values[0].data.int64_value,3); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],6u); check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],6u);
  }
  it("aligns NULL-only columns with typed nullable output without changing non-NULL kinds") {
    types[0][0]=(orm_sql_type){TURBODB_VALUE_NULL,true}; types[1][0].nullable=false;
    for(size_t i=0;i<fixtures[0].count;++i) fixtures[0].values[i][0]=turbodb_null();
    check_equal(open_union(ORM_SQL_UNION_DISTINCT),TURBODB_STATUS_OK);
    check_equal(run.source.types[0].kind,TURBODB_VALUE_INT64); check_true(run.source.types[0].nullable);
    check_equal(next().values[0].kind,TURBODB_VALUE_NULL);
    for(int64_t i=1;i<=3;++i) check_equal(next().values[0].data.int64_value,i);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("treats signed floating zero as equal and preserves nullable BOOL columns") {
    for(size_t i=0;i<2;++i) {
      types[i][0]=(orm_sql_type){TURBODB_VALUE_DOUBLE,false}; types[i][1]=(orm_sql_type){TURBODB_VALUE_BOOLEAN,true}; fixtures[i].count=2;
      fixtures[i].values[0][0]=turbodb_f64(i?-0.0:0.0); fixtures[i].values[0][1]=turbodb_bool(true);
      fixtures[i].values[1][0]=turbodb_f64(1.5); fixtures[i].values[1][1]=turbodb_null();
    }
    check_equal(open_union(ORM_SQL_UNION_DISTINCT),TURBODB_STATUS_OK);
    orm_sql_scan_row row=next(); check_equal(row.values[0].data.double_value,0.0); check_equal(row.values[1].data.boolean_value,1);
    row=next(); check_equal(row.values[0].data.double_value,1.5); check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("composes mixed ALL and DISTINCT stages with DISTINCT affecting its full left subtree") {
    check_equal(open_union(ORM_SQL_UNION_ALL),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_union_open(orm_tidesdb_sql_union_source(&run),&sources[2],ORM_SQL_UNION_DISTINCT,&parent,&error),TURBODB_STATUS_OK);
    orm_sql_scan_row row;
    for(int64_t i=1;i<=3;++i) { check_equal(orm_tidesdb_sql_union_next(&parent,&row,&error),TURBODB_STATUS_OK); check_equal(row.values[0].data.int64_value,i); }
    check_equal(orm_tidesdb_sql_union_next(&parent,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
    check_equal(orm_tidesdb_sql_union_close(&run,&error),TURBODB_STATUS_BUSY); reset();
    check_equal(open_union(ORM_SQL_UNION_DISTINCT),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_union_open(orm_tidesdb_sql_union_source(&run),&sources[2],ORM_SQL_UNION_ALL,&parent,&error),TURBODB_STATUS_OK);
    for(size_t i=0;i<6;++i) { check_equal(orm_tidesdb_sql_union_next(&parent,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_ROW); }
    check_equal(orm_tidesdb_sql_union_next(&parent,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
  }
  it("handles both empty and one empty input without retaining phantom rows") {
    for(size_t mode=0;mode<2;++mode) for(size_t empty=0;empty<3;++empty) {
      reset(); fixtures[0].count=empty==1?3:0; fixtures[1].count=empty==0?3:0;
      check_equal(open_union((orm_sql_union_kind)mode),TURBODB_STATUS_OK);
      if(empty<2) for(int64_t i=1;i<=3;++i) check_equal(next().values[0].data.int64_value,i);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
  }
  it("keeps ALL byte payloads valid until the next pull and downstream snapshots own their bytes") {
    for(size_t i=0;i<2;++i) { types[i][1]=(orm_sql_type){TURBODB_VALUE_TEXT,false};
      for(size_t j=0;j<TEST_ROWS;++j) fixtures[i].values[j][1]=turbodb_text("input"); }
    check_equal(open_union(ORM_SQL_UNION_ALL),TURBODB_STATUS_OK);
    const size_t mapping[]={0,1}; const orm_sql_scan_order order={.slot=0,.descending=true};
    const orm_sql_scan_spec spec={.projection=mapping,.projection_count=2,.orders=&order,.order_count=1,.limit=UINT64_MAX};
    check_equal(orm_tidesdb_sql_scan_open_source(&run.source,&spec,&budget,&downstream,&error),TURBODB_STATUS_OK);
    orm_sql_scan_row row;
    for(size_t i=0;i<6;++i) {
      check_equal(orm_tidesdb_sql_scan_next(&downstream,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.values[1].data.text_value.data[0],(char)('c'-i/2));
    }
    check_equal(orm_tidesdb_sql_scan_next(&downstream,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
  }
  it("applies pagination after deduplication and preserves all input leases until close") {
    check_equal(open_union(ORM_SQL_UNION_DISTINCT),TURBODB_STATUS_OK);
    const size_t mapping[]={0}; const orm_sql_scan_spec spec={.projection=mapping,.projection_count=1,.offset=1,.limit=1};
    check_equal(orm_tidesdb_sql_scan_open_source(&run.source,&spec,&budget,&downstream,&error),TURBODB_STATUS_OK);
    orm_sql_scan_row row={.count=99};
    check_equal(orm_tidesdb_sql_union_next(&run,&row,&error),TURBODB_STATUS_BUSY); check_equal(row.count,99u);
    check_equal(orm_tidesdb_sql_union_cancel(&run,&error),TURBODB_STATUS_BUSY); check_equal(orm_tidesdb_sql_union_close(&run,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_scan_next(&downstream,&row,&error),TURBODB_STATUS_OK); check_equal(row.values[0].data.int64_value,2);
    check_equal(orm_tidesdb_sql_scan_next(&downstream,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
    check_true(sources[0].active); check_true(sources[1].active);
  }
  it("does not touch inputs for outer LIMIT zero or cancellation before first next") {
    for(size_t i=0;i<2;++i) {
      reset(); check_equal(open_union(ORM_SQL_UNION_DISTINCT),TURBODB_STATUS_OK);
      if(i) { check_equal(orm_tidesdb_sql_union_cancel(&run,&error),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_CANCELLED); }
      else {
        const size_t mapping[]={0}; const orm_sql_scan_spec spec={.projection=mapping,.projection_count=1,.limit=0};
        check_equal(orm_tidesdb_sql_scan_open_source(&run.source,&spec,&budget,&downstream,&error),TURBODB_STATUS_OK);
        orm_sql_scan_row row; check_equal(orm_tidesdb_sql_scan_next(&downstream,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
      }
      check_equal(fixtures[0].calls+fixtures[1].calls,0u);
    }
  }
  it("rejects mismatched widths mixed kinds malformed types and unsupported DISTINCT bytes before reading") {
    sources[1].columns=1; check_equal(open_union(ORM_SQL_UNION_ALL),TURBODB_STATUS_SQL_ERROR); sources[1].columns=TEST_COLUMNS;
    types[1][0].kind=TURBODB_VALUE_UINT64; check_equal(open_union(ORM_SQL_UNION_ALL),TURBODB_STATUS_UNSUPPORTED);
    types[1][0]=(orm_sql_type){TURBODB_VALUE_NULL,false}; check_equal(open_union(ORM_SQL_UNION_ALL),TURBODB_STATUS_TYPE_ERROR);
    types[0][0]=types[1][0]=(orm_sql_type){TURBODB_VALUE_TEXT,true}; check_equal(open_union(ORM_SQL_UNION_DISTINCT),TURBODB_STATUS_UNSUPPORTED);
    types[0][0]=types[1][0]=(orm_sql_type){TURBODB_VALUE_BLOB,true}; check_equal(open_union(ORM_SQL_UNION_DISTINCT),TURBODB_STATUS_UNSUPPORTED);
    check_null(run.budget); check_equal(fixtures[0].calls+fixtures[1].calls,0u);
    check_equal(orm_tidesdb_sql_union_open(&sources[0],&sources[0],ORM_SQL_UNION_ALL,&run,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    sources[0].active=true; check_equal(open_union(ORM_SQL_UNION_ALL),TURBODB_STATUS_BUSY); sources[0].active=false;
  }
  it("locks first input errors and never publishes a partial DISTINCT result") {
    fixtures[1].fail_at=2; check_equal(open_union(ORM_SQL_UNION_DISTINCT),TURBODB_STATUS_OK); locked(TURBODB_STATUS_DATASTORE_ERROR);
    reset(); check_equal(open_union(ORM_SQL_UNION_ALL),TURBODB_STATUS_OK);
    for(size_t i=0;i<4;++i) check_equal(next().state,ORM_SQL_SCAN_ROW);
    locked(TURBODB_STATUS_DATASTORE_ERROR);
  }
  it("validates each branch before NULL widening and locks scalar type errors") {
    types[0][0].nullable=false; fixtures[0].values[0][0]=turbodb_null();
    check_equal(open_union(ORM_SQL_UNION_ALL),TURBODB_STATUS_OK); check_true(run.source.types[0].nullable); locked(TURBODB_STATUS_TYPE_ERROR);
  }
  it("enforces materialization bounds before publishing distinct rows and refunds retained snapshots") {
    check_equal(open_union(ORM_SQL_UNION_DISTINCT),TURBODB_STATUS_OK); budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=2;
    locked(TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],2u);
  }
  it("refunds each intercepted allocation failure during open and DISTINCT execution") {
    for(size_t mode=0;mode<2;++mode) {
      reset(); check_equal(open_union((orm_sql_union_kind)mode),TURBODB_STATUS_OK);
      const size_t opening[]={reserves,resizes};
      if(mode) while(next().state==ORM_SQL_SCAN_ROW) {}
      const size_t total[]={reserves,resizes};
      for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=total[pass];++point) {
        reset(); if(pass) fail_resize=point; else fail_reserve=point;
        const turbodb_status_t status=open_union((orm_sql_union_kind)mode);
        if(point<=opening[pass]) { check_equal(status,TURBODB_STATUS_OUT_OF_MEMORY); check_null(run.budget); }
        else { check_equal(status,TURBODB_STATUS_OK); locked(TURBODB_STATUS_OUT_OF_MEMORY); }
      }
    }
  }
  it("enforces every execution step boundary without emitting an incomplete DISTINCT result") {
    check_equal(open_union(ORM_SQL_UNION_DISTINCT),TURBODB_STATUS_OK);
    const uint64_t opening=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]; check_equal(next().state,ORM_SQL_SCAN_ROW);
    const uint64_t first=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    for(uint64_t allowance=opening;allowance<first;++allowance) {
      reset(); check_equal(open_union(ORM_SQL_UNION_DISTINCT),TURBODB_STATUS_OK);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=allowance; locked(TURBODB_STATUS_LIMIT_EXCEEDED);
    }
  }
}
