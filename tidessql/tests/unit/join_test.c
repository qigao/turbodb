#include "join.h"
#include <tinytest.h>
#include <string.h>

static size_t reserve_calls, resize_calls, fail_reserve, fail_resize;
static stl_status join_test_reserve(vec_t *v, size_t count) {
  return ++reserve_calls == fail_reserve ? STL_OUT_OF_MEMORY : vec_reserve(v,count);
}
static stl_status join_test_resize(vec_t *v, size_t count) {
  return ++resize_calls == fail_resize ? STL_OUT_OF_MEMORY : vec_resize(v,count);
}
#define vec_reserve join_test_reserve
#define vec_resize join_test_resize
#include "../../src/work.c"
#include "../../src/rows.c"
#include "../../src/expr.c"
#include "../../src/scan.c"
#include "../../src/join.c"
#undef vec_reserve
#undef vec_resize

enum { TEST_ROWS=10, TEST_COLUMNS=2, TEST_WIDTH=4, TEST_LIMIT=65536, TEST_WORK=4*1024*1024, TEST_DEPTH=32 };
typedef struct test_source {
  turbodb_value_t rows[TEST_ROWS][TEST_COLUMNS], buffer[TEST_COLUMNS];
  char text[2];
  size_t count, position, calls, fail_at;
} test_source;
static orm_tidesdb_sql_budget budget;
static orm_sql_budget_limits limits;
static turbodb_error_t error;
static orm_sql_join run, parent;
static orm_sql_scan downstream;
static orm_sql_expr condition, where;
static orm_sql_type types[TEST_COLUMNS];
static test_source left, right;
static orm_sql_row_source lsource, rsource;
static size_t slots[2];
enum { DEPENDENT_ROWS=2 };
typedef struct dependent_right {
  const turbodb_value_t *borrowed;
  orm_sql_type types[TEST_COLUMNS];
  size_t opens,closes,fail_open,fail_close;
  int64_t empty_key;
  unsigned drift;
  bool active;
} dependent_right;
static dependent_right dependent;

static turbodb_status_t input_next(void *context, const turbodb_value_t **out, turbodb_error_t *e) {
  test_source *source=context; ++source->calls;
  if(source->calls==source->fail_at) { tdsql_error_set(e,TURBODB_STATUS_DATASTORE_ERROR,"join source failed"); return TURBODB_STATUS_DATASTORE_ERROR; }
  source->text[0]='!'; memset(source->buffer,0,sizeof(source->buffer));
  if(source->position==source->count) { *out=NULL; return TURBODB_STATUS_OK; }
  orm_sql_budget_amount charge={0}; charge.value[ORM_SQL_BUDGET_READ_ROWS]=1;
  charge.value[ORM_SQL_BUDGET_READ_BYTES]=sizeof(source->buffer)+1;
  turbodb_status_t status=orm_tidesdb_sql_budget_reserve(&budget,&charge,e);
  if(status!=TURBODB_STATUS_OK) return status;
  memcpy(source->buffer,source->rows[source->position],sizeof(source->buffer));
  source->text[0]=(char)('a'+source->position++); source->text[1]=0;
  if(types[1].kind==TURBODB_VALUE_TEXT) source->buffer[1]=turbodb_text(source->text);
  else if(types[1].kind==TURBODB_VALUE_BLOB) source->buffer[1]=turbodb_blob(source->text,1);
  *out=source->buffer; return TURBODB_STATUS_OK;
}
static void compile(const char *sql, const orm_sql_type *inputs, size_t count, orm_sql_expr *out) {
  sqlparser_document *document=NULL; sqlparser_error e;
  check_equal(sqlparser_parse(sql,strlen(sql),NULL,&document,&e),SQLPARSER_OK);
  const sqlparser_node *statement=sqlparser_get_node(document,sqlparser_statements(document).first);
  const sqlparser_node *projection=sqlparser_get_node(document,statement->as.select.columns.first);
  orm_sql_expr_input bindings[TEST_WIDTH]; size_t used=0;
  for(size_t i=1;i<=sqlparser_node_count(document);++i)
    if(sqlparser_get_node(document,(sqlparser_id)i)->kind==SQLPARSER_PARAMETER) {
      check_true(used<count); bindings[used]=(orm_sql_expr_input){(sqlparser_id)i,inputs[used]}; ++used;
    }
  check_equal(used,count);
  check_equal(orm_tidesdb_sql_expr_compile(document,projection->as.projection.expression,bindings,count,
      TEST_DEPTH,&budget,out,&error),TURBODB_STATUS_OK);
  sqlparser_document_destroy(document);
}
static void equality(const char *sql) {
  const orm_sql_type inputs[]={types[0],types[0]}; compile(sql,inputs,2,&condition);
}
static turbodb_status_t open_join(orm_sql_join_kind kind) {
  const orm_sql_join_spec spec={.kind=kind,.condition=kind==ORM_SQL_JOIN_CROSS?NULL:&condition,
    .slots=slots,.count=kind==ORM_SQL_JOIN_CROSS?0:condition.input_count};
  return orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error);
}
static turbodb_status_t dependent_open(void *context,const turbodb_value_t *row,size_t count,turbodb_error_t *e) {
  dependent_right *state=context;
  check_false(state->active); check_false(rsource.active); check_equal(count,TEST_COLUMNS);
  check_true(lsource.active); check_true(row==vec_data_const(&run.left.output));
  ++state->opens;
  orm_sql_scan_row unused={0};
  check_equal(orm_tidesdb_sql_join_next(&run,&unused,e),TURBODB_STATUS_BUSY);
  check_equal(orm_tidesdb_sql_join_cancel(&run,e),TURBODB_STATUS_BUSY);
  check_equal(orm_tidesdb_sql_join_close(&run,e),TURBODB_STATUS_BUSY);
  if(state->opens==state->fail_open) {
    tdsql_error_set(e,TURBODB_STATUS_DATASTORE_ERROR,"dependent right open failed"); return TURBODB_STATUS_DATASTORE_ERROR;
  }
  state->active=true; state->borrowed=row;
  right.position=0;
  right.count=row[0].kind==TURBODB_VALUE_NULL || row[0].data.int64_value==state->empty_key?0:DEPENDENT_ROWS;
  for(size_t i=0;i<right.count;++i) right.rows[i][0]=turbodb_i64(row[0].data.int64_value*10+(int64_t)i);
  memcpy(state->types,types,sizeof(types));
  rsource=(orm_sql_row_source){&budget,state->types,TEST_COLUMNS,&right,input_next,false};
  if(state->drift==1) rsource.columns=1;
  if(state->drift==2) state->types[1].nullable=!state->types[1].nullable;
  if(state->drift==3) rsource.budget=NULL;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t dependent_close(void *context,turbodb_error_t *e) {
  dependent_right *state=context;
  check_true(state->active); check_false(rsource.active); check_true(lsource.active);
  check_true(state->borrowed==vec_data_const(&run.left.output));
  ++state->closes;
  if(state->closes==state->fail_close) {
    tdsql_error_set(e,TURBODB_STATUS_DATASTORE_ERROR,"dependent right close failed"); return TURBODB_STATUS_DATASTORE_ERROR;
  }
  state->active=false; state->borrowed=NULL;
  rsource=(orm_sql_row_source){&budget,types,TEST_COLUMNS,&right,input_next,false};
  return TURBODB_STATUS_OK;
}
static turbodb_status_t open_dependent(orm_sql_join_kind kind) {
  const orm_sql_join_spec spec={.kind=kind,.condition=kind==ORM_SQL_JOIN_CROSS?NULL:&condition,
      .slots=slots,.count=kind==ORM_SQL_JOIN_CROSS?0:condition.input_count,
      .right_binding={&dependent,dependent_open,dependent_close}};
  return orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error);
}
static orm_sql_scan_row next(void) {
  orm_sql_scan_row out={0}; check_equal(orm_tidesdb_sql_join_next(&run,&out,&error),TURBODB_STATUS_OK); return out;
}
static void clean(void) {
  fail_reserve=fail_resize=0;
  check_equal(orm_tidesdb_sql_scan_close(&downstream,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_join_close(&parent,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_join_close(&run,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_expr_destroy(&condition,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_expr_destroy(&where,&error),TURBODB_STATUS_OK);
  check_false(lsource.active); check_false(rsource.active);
  check_false(dependent.active); check_null(dependent.borrowed);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
  check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
}
static void reset(void) {
  clean(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_init(&budget,&limits,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
  left.position=left.calls=left.fail_at=right.position=right.calls=right.fail_at=0;
  dependent=(dependent_right){0};
}
static void locked(turbodb_status_t expected) {
  orm_sql_scan_row out={.state=ORM_SQL_SCAN_CANCELLED,.count=99};
  check_equal(orm_tidesdb_sql_join_next(&run,&out,&error),expected);
  check_equal(out.count,99u); check_equal(out.state,ORM_SQL_SCAN_CANCELLED);
  const size_t reads=left.calls+right.calls;
  const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
  check_equal(orm_tidesdb_sql_join_next(&run,&out,&error),expected);
  check_equal(left.calls+right.calls,reads); check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
}

spec("TidesDB bounded nested-loop join") {
  before_each() {
    reserve_calls=resize_calls=fail_reserve=fail_resize=0; tdsql_error_init(&error);
    run=(orm_sql_join){0}; parent=(orm_sql_join){0}; downstream=(orm_sql_scan){0};
    condition=(orm_sql_expr){0}; where=(orm_sql_expr){0}; limits=(orm_sql_budget_limits){0};
    for(size_t i=0;i<ORM_SQL_BUDGET_RESOURCE_COUNT;++i) limits.statement.value[i]=TEST_LIMIT;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=TEST_WORK;
    limits.transaction=(orm_sql_transaction_budget_amount){TEST_LIMIT,TEST_LIMIT,TEST_LIMIT};
    check_equal(orm_tidesdb_sql_budget_init(&budget,&limits,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
    types[0]=(orm_sql_type){TURBODB_VALUE_INT64,true}; types[1]=(orm_sql_type){TURBODB_VALUE_TEXT,false};
    dependent=(dependent_right){0};
    left=(test_source){.count=3}; right=(test_source){.count=4};
    for(size_t i=0;i<TEST_ROWS;++i) {
      left.rows[i][0]=turbodb_i64((int64_t)i+1); right.rows[i][0]=turbodb_i64((int64_t)i);
    }
    left.rows[2][0]=turbodb_null(); right.rows[0][0]=right.rows[1][0]=turbodb_i64(2); right.rows[3][0]=turbodb_null();
    lsource=(orm_sql_row_source){&budget,types,TEST_COLUMNS,&left,input_next,false};
    rsource=(orm_sql_row_source){&budget,types,TEST_COLUMNS,&right,input_next,false};
    slots[0]=0; slots[1]=TEST_COLUMNS;
  }
  after_each() { clean(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); }

  group("typed USING key matching") {
    it("copies keys and preserves duplicate matches and UNKNOWN NULL comparisons") {
      orm_sql_join_key key={0,0}; const orm_sql_join_spec spec={.kind=ORM_SQL_JOIN_LEFT,.match=ORM_SQL_JOIN_MATCH_USING,.keys=&key,.key_count=1};
      check_equal(orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error),TURBODB_STATUS_OK); key.left=key.right=SIZE_MAX;
      check_equal(left.calls+right.calls,0u); orm_sql_scan_row row=next();
      check_equal(row.values[0].data.int64_value,1); check_equal(row.values[2].kind,TURBODB_VALUE_NULL);
      for(size_t i=0;i<3;++i) { row=next(); check_equal(row.values[0].data.int64_value,2); check_equal(row.values[2].data.int64_value,2); }
      row=next(); check_equal(row.values[0].kind,TURBODB_VALUE_NULL); check_equal(row.values[2].kind,TURBODB_VALUE_NULL);
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("requires every key including strict TEXT byte equality to match") {
      const orm_sql_join_key keys[]={{0,0},{1,1}};
      const orm_sql_join_spec spec={.kind=ORM_SQL_JOIN_INNER,.match=ORM_SQL_JOIN_MATCH_USING,.keys=keys,.key_count=2};
      check_equal(orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error),TURBODB_STATUS_OK);
      const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,2); check_equal(row.values[2].data.int64_value,2);
      check_equal(row.values[1].data.text_value.data[0],'b'); check_equal(row.values[3].data.text_value.data[0],'b');
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("supports empty common keys and null extension against an empty right side") {
      right.count=0; const orm_sql_join_spec spec={.kind=ORM_SQL_JOIN_LEFT,.match=ORM_SQL_JOIN_MATCH_USING};
      check_equal(orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error),TURBODB_STATUS_OK);
      for(size_t i=0;i<3;++i) { const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[2].kind,TURBODB_VALUE_NULL); }
      check_equal(next().state,ORM_SQL_SCAN_DONE);
    }
    it("rejects invalid keys and matching modes before acquiring or reading sources") {
      orm_sql_join_key key={TEST_COLUMNS,0}; orm_sql_join_spec spec={.kind=ORM_SQL_JOIN_INNER,.match=ORM_SQL_JOIN_MATCH_USING,.keys=&key,.key_count=1};
      check_equal(orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      key.left=0; key.right=TEST_COLUMNS; check_equal(orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      key.right=0; spec.match=(orm_sql_join_match)(ORM_SQL_JOIN_MATCH_USING+1);
      check_equal(orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      check_false(lsource.active); check_false(rsource.active); check_null(run.budget); check_equal(left.calls+right.calls,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    }
    it("rejects every insufficient USING construction or execution step allowance") {
      const orm_sql_join_key key={0,0}; const orm_sql_join_spec spec={.kind=ORM_SQL_JOIN_LEFT,.match=ORM_SQL_JOIN_MATCH_USING,.keys=&key,.key_count=1};
      check_equal(orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error),TURBODB_STATUS_OK);
      const uint64_t construction=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      while(next().state==ORM_SQL_SCAN_ROW) {}
      const uint64_t total=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      for(uint64_t point=0;point<total;++point) {
        reset(); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=point;
        const turbodb_status_t opened=orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error);
        if(point<construction) {
          check_equal(opened,TURBODB_STATUS_LIMIT_EXCEEDED); check_null(run.budget);
          check_false(lsource.active); check_false(rsource.active); check_equal(left.calls+right.calls,0u);
          check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
        } else {
          check_equal(opened,TURBODB_STATUS_OK); orm_sql_scan_row row; turbodb_status_t status;
          do { status=orm_tidesdb_sql_join_next(&run,&row,&error); } while(status==TURBODB_STATUS_OK && row.state==ORM_SQL_SCAN_ROW);
          check_equal(status,TURBODB_STATUS_LIMIT_EXCEEDED); locked(TURBODB_STATUS_LIMIT_EXCEEDED);
        }
      }
    }
    it("refunds every USING constructor allocation before acquiring sources") {
      const orm_sql_join_key key={0,0}; const orm_sql_join_spec spec={.kind=ORM_SQL_JOIN_LEFT,.match=ORM_SQL_JOIN_MATCH_USING,.keys=&key,.key_count=1};
      reserve_calls=resize_calls=0; check_equal(orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error),TURBODB_STATUS_OK);
      const size_t counts[]={reserve_calls,resize_calls}; clean();
      for(size_t phase=0;phase<2;++phase) for(size_t point=1;point<=counts[phase];++point) {
        reset(); reserve_calls=resize_calls=0; if(phase) fail_resize=point; else fail_reserve=point;
        check_equal(orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error),TURBODB_STATUS_OUT_OF_MEMORY);
        fail_reserve=fail_resize=0; check_null(run.budget); check_false(lsource.active); check_false(rsource.active);
        check_equal(left.calls+right.calls,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
      }
    }
  }
  it("opens dependent right rows per left row and releases captures before advancing") {
    check_equal(open_dependent(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK);
    check_equal(dependent.opens,0u); check_false(rsource.active); check_true(lsource.active);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(size_t outer=0;outer<2;++outer) for(size_t inner=0;inner<DEPENDENT_ROWS;++inner) {
      const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
      check_equal(row.values[0].data.int64_value,(int64_t)outer+1);
      check_equal(row.values[2].data.int64_value,((int64_t)outer+1)*10+(int64_t)inner);
      check_equal(row.values[1].data.text_value.data[0],(char)('a'+outer));
      check_equal(row.values[3].data.text_value.data[0],(char)('a'+inner));
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],DEPENDENT_ROWS);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(dependent.opens,3u); check_equal(dependent.closes,3u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_JOIN_PAIRS],4u);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(dependent.opens,3u);
  }

  it("continues INNER and CROSS after an empty dependent right round") {
    for(size_t mode=0;mode<2;++mode) {
      if(mode) reset();
      compile("SELECT TRUE",NULL,0,&condition); dependent.empty_key=1;
      check_equal(open_dependent(mode?ORM_SQL_JOIN_INNER:ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK);
      for(size_t i=0;i<DEPENDENT_ROWS;++i) {
        const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,2);
        check_equal(row.values[2].data.int64_value,20+(int64_t)i);
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(dependent.opens,3u);
      check_equal(dependent.closes,3u);
    }
  }

  it("owns dependent BLOB results across right buffer reuse and round replacement") {
    types[1].kind=TURBODB_VALUE_BLOB; check_equal(open_dependent(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK);
    for(size_t outer=0;outer<2;++outer) for(size_t inner=0;inner<DEPENDENT_ROWS;++inner) {
      const orm_sql_scan_row row=next(); check_equal(row.values[1].kind,TURBODB_VALUE_BLOB);
      check_equal(row.values[3].kind,TURBODB_VALUE_BLOB); check_equal(row.values[3].data.blob_value.size,1u);
      check_equal(((const char *)row.values[1].data.blob_value.data)[0],(char)('a'+outer));
      check_equal(((const char *)row.values[3].data.blob_value.data)[0],(char)('a'+inner));
      check_not_equal(row.values[3].data.blob_value.data,(const void *)right.text);
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_false(dependent.active);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
  }

  it("null extends each dependent LEFT row when its right query is empty or ON is false") {
    for(size_t mode=0;mode<2;++mode) {
      if(mode) reset(); compile(mode?"SELECT FALSE":"SELECT TRUE",NULL,0,&condition);
      dependent.empty_key=1; check_equal(open_dependent(ORM_SQL_JOIN_LEFT),TURBODB_STATUS_OK);
      const orm_sql_scan_row first=next(); check_equal(first.values[0].data.int64_value,1);
      check_equal(first.values[2].kind,TURBODB_VALUE_NULL); check_equal(first.values[3].kind,TURBODB_VALUE_NULL);
      const size_t matches=mode?1:DEPENDENT_ROWS;
      for(size_t i=0;i<matches;++i) {
        const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,2);
        check_equal(row.values[2].kind,mode?TURBODB_VALUE_NULL:TURBODB_VALUE_INT64);
      }
      const orm_sql_scan_row last=next(); check_equal(last.values[0].kind,TURBODB_VALUE_NULL);
      check_equal(last.values[2].kind,TURBODB_VALUE_NULL); check_equal(next().state,ORM_SQL_SCAN_DONE);
      check_equal(budget.used.value[ORM_SQL_BUDGET_JOIN_PAIRS],DEPENDENT_ROWS);
      check_equal(dependent.opens,3u); check_equal(dependent.closes,3u);
    }
  }

  it("does not invoke dependent callbacks for an empty or pre-cancelled left scan") {
    for(size_t mode=0;mode<2;++mode) {
      if(mode) reset(); left.count=0; check_equal(open_dependent(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK);
      if(mode) check_equal(orm_tidesdb_sql_join_cancel(&run,&error),TURBODB_STATUS_OK);
      check_equal(next().state,mode?ORM_SQL_SCAN_CANCELLED:ORM_SQL_SCAN_DONE);
      check_equal(dependent.opens,0u); check_equal(dependent.closes,0u); check_equal(right.calls,0u);
    }
  }

  it("validates dependent callback pairs and metadata before any row or callback") {
    orm_sql_join_spec spec={.kind=ORM_SQL_JOIN_CROSS,.right_binding={&dependent,dependent_open,NULL}};
    check_equal(orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    spec.right_binding=(orm_sql_join_right_binding){&dependent,NULL,dependent_close};
    check_equal(orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    types[1].kind=(turbodb_value_kind_t)99;
    check_equal(open_dependent(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_TYPE_ERROR);
    check_equal(left.calls+right.calls+dependent.opens,0u); check_null(run.budget);
  }

  it("rejects changing dependent source schema and closes the successful callback") {
    const turbodb_status_t expected[]={TURBODB_STATUS_INVALID_STATE,TURBODB_STATUS_TYPE_ERROR,TURBODB_STATUS_INVALID_STATE};
    for(size_t mode=0;mode<sizeof(expected)/sizeof(expected[0]);++mode) {
      if(mode) reset(); dependent.drift=(unsigned)mode+1;
      check_equal(open_dependent(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK); locked(expected[mode]);
      check_equal(dependent.opens,1u); check_equal(right.calls,0u); check_true(dependent.active);
      check_equal(orm_tidesdb_sql_join_close(&run,&error),TURBODB_STATUS_OK); check_equal(dependent.closes,1u);
    }
  }

  it("locks dependent callback and input failures without publishing a partial row") {
    for(size_t mode=0;mode<2;++mode) {
      if(mode) reset();
      if(mode) right.fail_at=2; else dependent.fail_open=1;
      check_equal(open_dependent(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK); locked(TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(dependent.opens,1u); check_equal(dependent.active,mode!=0);
      check_equal(orm_tidesdb_sql_join_close(&run,&error),TURBODB_STATUS_OK); check_equal(dependent.closes,mode?1u:0u);
    }
  }

  it("retains the left borrow when dependent close fails and permits explicit close retry") {
    check_equal(open_dependent(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK); (void)next();
    dependent.fail_close=1;
    check_equal(orm_tidesdb_sql_join_close(&run,&error),TURBODB_STATUS_DATASTORE_ERROR);
    check_not_null(run.budget); check_true(lsource.active); check_false(rsource.active);
    check_true(dependent.active); check_not_null(dependent.borrowed);
    locked(TURBODB_STATUS_DATASTORE_ERROR); check_equal(dependent.closes,1u);
    check_equal(orm_tidesdb_sql_join_close(&run,&error),TURBODB_STATUS_OK);
    check_equal(dependent.closes,2u); check_false(lsource.active);
  }

  it("releases dependent captures after cancellation and respects downstream leases") {
    check_equal(open_dependent(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK); (void)next();
    const size_t projection[]={0}; const orm_sql_scan_spec spec={.projection=projection,.projection_count=1,.limit=UINT64_MAX};
    check_equal(orm_tidesdb_sql_scan_open_source(&run.source,&spec,&budget,&downstream,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_join_close(&run,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_scan_close(&downstream,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_join_cancel(&run,&error),TURBODB_STATUS_OK);
    const size_t reads=left.calls+right.calls;
    check_equal(next().state,ORM_SQL_SCAN_CANCELLED); check_equal(left.calls+right.calls,reads);
    check_equal(orm_tidesdb_sql_join_close(&run,&error),TURBODB_STATUS_OK); check_false(dependent.active);
  }

  it("does not advance the left scan after a dependent round close failure") {
    check_equal(open_dependent(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK);
    (void)next(); (void)next(); dependent.fail_close=1;
    locked(TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(left.position,1u); check_equal(dependent.opens,1u); check_equal(dependent.closes,1u);
    check_true(dependent.active); check_true(lsource.active); check_false(rsource.active);
    check_equal(orm_tidesdb_sql_join_close(&run,&error),TURBODB_STATUS_OK); check_equal(dependent.closes,2u);
  }

  it("preserves the first dependent evaluation error when cleanup itself fails") {
    check_equal(open_dependent(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_JOIN_PAIRS]=0;
    locked(TURBODB_STATUS_LIMIT_EXCEEDED); dependent.fail_close=1;
    check_equal(orm_tidesdb_sql_join_close(&run,&error),TURBODB_STATUS_DATASTORE_ERROR);
    locked(TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(dependent.opens,1u); check_equal(dependent.closes,1u);
    check_equal(orm_tidesdb_sql_join_close(&run,&error),TURBODB_STATUS_OK); check_false(dependent.active);
  }

  it("enforces per-round materialized and cumulative pair capacity without leaking captures") {
    for(size_t mode=0;mode<2;++mode) {
      if(mode) reset(); check_equal(open_dependent(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK);
      if(mode) {
        budget.limits.statement.value[ORM_SQL_BUDGET_JOIN_PAIRS]=DEPENDENT_ROWS;
        (void)next(); (void)next();
      } else budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=DEPENDENT_ROWS-1;
      locked(TURBODB_STATUS_LIMIT_EXCEEDED);
      check_true(dependent.active);
      check_equal(orm_tidesdb_sql_join_close(&run,&error),TURBODB_STATUS_OK);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u); check_false(dependent.active);
    }
  }

  it("refunds every dependent constructor allocation before reading or invoking callbacks") {
    reserve_calls=resize_calls=0; check_equal(open_dependent(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK);
    const size_t counts[]={reserve_calls,resize_calls};
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=counts[pass];++point) {
      reset(); reserve_calls=resize_calls=0;
      if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(open_dependent(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OUT_OF_MEMORY);
      fail_resize=fail_reserve=0;
      check_null(run.budget); check_equal(left.calls+right.calls+dependent.opens,0u);
      check_false(lsource.active); check_false(rsource.active);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    }
  }

  it("refunds every dependent execution allocation including later right rounds") {
    check_equal(open_dependent(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK); reserve_calls=resize_calls=0;
    while(next().state==ORM_SQL_SCAN_ROW) {}
    const size_t counts[]={reserve_calls,resize_calls};
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=counts[pass];++point) {
      reset(); check_equal(open_dependent(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK); reserve_calls=resize_calls=0;
      if(pass) fail_resize=point; else fail_reserve=point;
      orm_sql_scan_row row={0}; turbodb_status_t status;
      do {
        row=(orm_sql_scan_row){.count=99}; status=orm_tidesdb_sql_join_next(&run,&row,&error);
      } while(status==TURBODB_STATUS_OK && row.state==ORM_SQL_SCAN_ROW);
      check_equal(status,TURBODB_STATUS_OUT_OF_MEMORY); check_equal(row.count,99u);
      fail_resize=fail_reserve=0; locked(status);
      check_equal(orm_tidesdb_sql_join_close(&run,&error),TURBODB_STATUS_OK);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
      check_false(lsource.active); check_false(rsource.active); check_false(dependent.active);
    }
  }

  it("enforces every dependent execution step boundary across empty and nonempty rounds") {
    compile("SELECT FALSE",NULL,0,&condition); check_equal(open_dependent(ORM_SQL_JOIN_LEFT),TURBODB_STATUS_OK);
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    while(next().state==ORM_SQL_SCAN_ROW) {}
    const uint64_t cost=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    for(uint64_t allowance=0;allowance<cost;++allowance) {
      reset(); compile("SELECT FALSE",NULL,0,&condition); check_equal(open_dependent(ORM_SQL_JOIN_LEFT),TURBODB_STATUS_OK);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+allowance;
      orm_sql_scan_row row={0}; turbodb_status_t status;
      do {
        row=(orm_sql_scan_row){.count=99}; status=orm_tidesdb_sql_join_next(&run,&row,&error);
      } while(status==TURBODB_STATUS_OK && row.state==ORM_SQL_SCAN_ROW);
      check_equal(status,TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(row.count,99u); locked(status);
      check_equal(orm_tidesdb_sql_join_close(&run,&error),TURBODB_STATUS_OK);
      check_false(dependent.active);
      check_equal(orm_tidesdb_sql_expr_destroy(&condition,&error),TURBODB_STATUS_OK);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    }
  }

  it("preserves duplicate INNER matches and counts rejected and unknown pairs") {
    equality("SELECT ?=?"); check_equal(open_join(ORM_SQL_JOIN_INNER),TURBODB_STATUS_OK);
    check_equal(left.calls+right.calls,0u);
    for(size_t i=0;i<3;++i) { /* Right fixture has three copies of key 2. */
      const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.count,TEST_WIDTH);
      check_equal(row.values[0].data.int64_value,2); check_equal(row.values[2].data.int64_value,2);
      check_equal(row.values[3].data.text_value.data[0],(char)('a'+i));
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(next().state,ORM_SQL_SCAN_DONE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_JOIN_PAIRS],12u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],7u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],4u);
  }
  it("extends unmatched LEFT rows once including UNKNOWN and makes only right outputs nullable") {
    equality("SELECT ?=?"); check_equal(open_join(ORM_SQL_JOIN_LEFT),TURBODB_STATUS_OK);
    const orm_sql_row_source *source=orm_tidesdb_sql_join_source(&run);
    check_false(source->types[1].nullable); check_true(source->types[3].nullable);
    const orm_sql_scan_row first=next(); check_equal(first.values[0].data.int64_value,1);
    check_equal(first.values[2].kind,TURBODB_VALUE_NULL); check_equal(first.values[3].kind,TURBODB_VALUE_NULL);
    for(size_t i=0;i<3;++i) check_equal(next().values[2].data.int64_value,2);
    const orm_sql_scan_row last=next(); check_equal(last.values[0].kind,TURBODB_VALUE_NULL);
    check_equal(last.values[2].kind,TURBODB_VALUE_NULL); check_equal(last.values[3].kind,TURBODB_VALUE_NULL);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_JOIN_PAIRS],12u);
  }
  it("distinguishes NULL-safe equality from ordinary equality") {
    equality("SELECT ?<=>?"); check_equal(open_join(ORM_SQL_JOIN_LEFT),TURBODB_STATUS_OK);
    (void)next(); for(size_t i=0;i<3;++i) (void)next();
    const orm_sql_scan_row row=next(); check_equal(row.values[0].kind,TURBODB_VALUE_NULL);
    check_equal(row.values[2].kind,TURBODB_VALUE_NULL); check_equal(row.values[3].kind,TURBODB_VALUE_TEXT);
    check_equal(row.values[3].data.text_value.data[0],'d'); check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("cross joins all rows and owns reused TEXT and BLOB payloads across registry growth") {
    for(size_t mode=0;mode<2;++mode) {
      reset(); types[1].kind=mode?TURBODB_VALUE_BLOB:TURBODB_VALUE_TEXT; right.count=TEST_ROWS;
      check_equal(open_join(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK);
      for(size_t i=0;i<left.count;++i) for(size_t j=0;j<right.count;++j) {
        const orm_sql_scan_row row=next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
        const char *payload=mode?row.values[3].data.blob_value.data:row.values[3].data.text_value.data;
        check_equal(payload[0],(char)('a'+j));
      }
      check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(right.calls,TEST_ROWS+1u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_JOIN_PAIRS],left.count*right.count);
    }
  }
  it("does not pull right for empty left and handles empty right for each join kind") {
    for(size_t mode=0;mode<3;++mode) {
      reset(); compile("SELECT TRUE",NULL,0,&condition); left.count=0;
      check_equal(open_join((orm_sql_join_kind)mode),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_DONE);
      check_equal(left.calls,1u); check_equal(right.calls,0u);
      reset(); compile("SELECT TRUE",NULL,0,&condition); left.count=3; right.count=0;
      check_equal(open_join((orm_sql_join_kind)mode),TURBODB_STATUS_OK);
      if(mode==ORM_SQL_JOIN_LEFT) for(size_t i=0;i<left.count;++i) check_equal(next().values[3].kind,TURBODB_VALUE_NULL);
      check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(right.calls,1u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_JOIN_PAIRS],0u);
    }
  }
  it("preserves original nonnullable ON types while exposing nullable right output") {
    types[0].nullable=false; left.count=2; right.count=3;
    equality("SELECT ?=?"); check_equal(open_join(ORM_SQL_JOIN_LEFT),TURBODB_STATUS_OK);
    check_equal(next().values[2].kind,TURBODB_VALUE_NULL);
    check_equal(next().values[2].data.int64_value,2);
  }
  it("copies ON parameters and mapping and leases its program after document destruction") {
    const orm_sql_type inputs[]={{TURBODB_VALUE_TEXT,false},{TURBODB_VALUE_TEXT,false}};
    compile("SELECT ?=?",inputs,2,&condition); char label[]="b";
    turbodb_value_t parameter=turbodb_text(label); const size_t mapping[]={3,TEST_WIDTH};
    const orm_sql_join_spec spec={ORM_SQL_JOIN_LEFT,&condition,mapping,2,&parameter,inputs,1};
    check_equal(orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error),TURBODB_STATUS_OK);
    label[0]='!'; check_equal(orm_tidesdb_sql_expr_destroy(&condition,&error),TURBODB_STATUS_BUSY);
    for(size_t i=0;i<left.count;++i) {
      const orm_sql_scan_row row=next(); check_equal(row.values[3].data.text_value.data[0],'b');
    }
    check_equal(next().state,ORM_SQL_SCAN_DONE);
  }
  it("applies downstream WHERE after ON matches instead of creating a replacement NULL row") {
    equality("SELECT ?=?"); check_equal(open_join(ORM_SQL_JOIN_LEFT),TURBODB_STATUS_OK);
    const orm_sql_type input={TURBODB_VALUE_INT64,true}; compile("SELECT ? IS NULL",&input,1,&where);
    const size_t mapping=2, projection[]={0,2};
    const orm_sql_scan_spec spec={.filter=&where,.filter_slots=&mapping,.filter_count=1,
      .projection=projection,.projection_count=2,.limit=UINT64_MAX};
    check_equal(orm_tidesdb_sql_scan_open_source(orm_tidesdb_sql_join_source(&run),&spec,&budget,&downstream,&error),TURBODB_STATUS_OK);
    orm_sql_scan_row out; check_equal(orm_tidesdb_sql_scan_next(&downstream,&out,&error),TURBODB_STATUS_OK);
    check_equal(out.values[0].data.int64_value,1);
    check_equal(orm_tidesdb_sql_scan_next(&downstream,&out,&error),TURBODB_STATUS_OK); check_equal(out.values[0].kind,TURBODB_VALUE_NULL);
    check_equal(orm_tidesdb_sql_scan_next(&downstream,&out,&error),TURBODB_STATUS_OK); check_equal(out.state,ORM_SQL_SCAN_DONE);
  }
  it("respects downstream leases LIMIT zero and early cancellation without reading inputs") {
    check_equal(open_join(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK); const size_t projection=0;
    const orm_sql_scan_spec spec={.projection=&projection,.projection_count=1,.limit=0};
    check_equal(orm_tidesdb_sql_scan_open_source(orm_tidesdb_sql_join_source(&run),&spec,&budget,&downstream,&error),TURBODB_STATUS_OK);
    orm_sql_scan_row out={.count=99};
    check_equal(orm_tidesdb_sql_join_next(&run,&out,&error),TURBODB_STATUS_BUSY); check_equal(out.count,99u);
    check_equal(orm_tidesdb_sql_join_cancel(&run,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_join_close(&run,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_scan_next(&downstream,&out,&error),TURBODB_STATUS_OK); check_equal(out.state,ORM_SQL_SCAN_DONE);
    check_equal(orm_tidesdb_sql_scan_close(&downstream,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_join_cancel(&run,&error),TURBODB_STATUS_OK);
    check_equal(next().state,ORM_SQL_SCAN_CANCELLED); check_equal(next().state,ORM_SQL_SCAN_CANCELLED);
    check_equal(left.calls+right.calls,0u); check_true(lsource.active); check_true(rsource.active);
  }
  it("composes multi-level joins while each original source alone charges physical reads") {
    check_equal(open_join(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK);
    test_source third=right; orm_sql_row_source third_source={&budget,types,TEST_COLUMNS,&third,input_next,false};
    const orm_sql_join_spec spec={.kind=ORM_SQL_JOIN_CROSS};
    check_equal(orm_tidesdb_sql_join_open(orm_tidesdb_sql_join_source(&run),&third_source,&spec,&parent,&error),TURBODB_STATUS_OK);
    size_t count=0; orm_sql_scan_row out;
    do { check_equal(orm_tidesdb_sql_join_next(&parent,&out,&error),TURBODB_STATUS_OK); if(out.state==ORM_SQL_SCAN_ROW) ++count; }
    while(out.state==ORM_SQL_SCAN_ROW);
    check_equal(count,48u); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],11u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_JOIN_PAIRS],60u);
    check_equal(orm_tidesdb_sql_join_close(&run,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_join_close(&parent,&error),TURBODB_STATUS_OK); check_false(third_source.active);
  }
  it("enforces candidate limits including rejected pairs and keeps consumed charges after close") {
    compile("SELECT FALSE",NULL,0,&condition); check_equal(open_join(ORM_SQL_JOIN_LEFT),TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_JOIN_PAIRS]=3;
    locked(TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(budget.used.value[ORM_SQL_BUDGET_JOIN_PAIRS],3u);
    clean(); check_equal(budget.used.value[ORM_SQL_BUDGET_JOIN_PAIRS],3u);
    reset(); compile("SELECT FALSE",NULL,0,&condition); check_equal(open_join(ORM_SQL_JOIN_LEFT),TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_JOIN_PAIRS]=12;
    for(size_t i=0;i<left.count;++i) check_equal(next().values[2].kind,TURBODB_VALUE_NULL);
    check_equal(next().state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_JOIN_PAIRS],12u);
  }
  it("locks right materialization and later left source errors without retry") {
    for(size_t mode=0;mode<2;++mode) {
      reset(); if(mode) left.fail_at=2; else right.fail_at=3;
      check_equal(open_join(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK);
      if(mode) for(size_t i=0;i<right.count;++i) (void)next();
      locked(TURBODB_STATUS_DATASTORE_ERROR);
    }
  }
  it("locks ON evaluation errors and rejects corrupt source values") {
    const orm_sql_type input=types[0]; compile("SELECT ?+1>0",&input,1,&condition);
    left.rows[0][0]=turbodb_i64(INT64_MAX); check_equal(open_join(ORM_SQL_JOIN_INNER),TURBODB_STATUS_OK);
    locked(TURBODB_STATUS_LIMIT_EXCEEDED);
    reset(); left.rows[0][0]=turbodb_bool(true); check_equal(open_join(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK);
    locked(TURBODB_STATUS_TYPE_ERROR);
  }
  it("fails right materialization quota and work capacity before emitting a prefix") {
    for(size_t mode=0;mode<2;++mode) {
      reset(); check_equal(open_join(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK);
      if(mode) budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      else budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=right.count-1;
      locked(TURBODB_STATUS_LIMIT_EXCEEDED);
    }
  }
  it("validates kinds sources mappings and unused parameters without reading") {
    equality("SELECT ?=?"); orm_sql_join_spec spec={ORM_SQL_JOIN_INNER,&condition,slots,2};
    check_equal(orm_tidesdb_sql_join_open(&lsource,&lsource,&spec,&run,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    rsource.active=true; check_equal(open_join(ORM_SQL_JOIN_INNER),TURBODB_STATUS_BUSY); rsource.active=false;
    check_equal(open_join((orm_sql_join_kind)99),TURBODB_STATUS_UNSUPPORTED);
    slots[1]=TEST_WIDTH; check_equal(open_join(ORM_SQL_JOIN_INNER),TURBODB_STATUS_INVALID_ARGUMENT); slots[1]=2;
    const orm_sql_type parameter_type={TURBODB_VALUE_INT64,false}; const turbodb_value_t value=turbodb_null();
    spec.parameters=&value; spec.parameter_types=&parameter_type; spec.parameter_count=1;
    check_equal(orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error),TURBODB_STATUS_TYPE_ERROR);
    spec=(orm_sql_join_spec){.kind=ORM_SQL_JOIN_INNER};
    check_equal(orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_null(run.budget); check_false(lsource.active); check_false(rsource.active); check_equal(left.calls+right.calls,0u);
  }
  it("refunds every open allocation and partial source or expression lease") {
    equality("SELECT ?=?"); reserve_calls=resize_calls=0; check_equal(open_join(ORM_SQL_JOIN_LEFT),TURBODB_STATUS_OK);
    const size_t reserves=reserve_calls,resizes=resize_calls;
    check_equal(orm_tidesdb_sql_join_close(&run,&error),TURBODB_STATUS_OK);
    const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(size_t i=1;i<=reserves+resizes;++i) {
      reserve_calls=resize_calls=0; if(i<=reserves) fail_reserve=i; else fail_resize=i-reserves;
      check_equal(open_join(ORM_SQL_JOIN_LEFT),TURBODB_STATUS_OUT_OF_MEMORY); check_null(run.budget);
      check_false(lsource.active); check_false(rsource.active); check_equal(condition.active_runs,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained); fail_reserve=fail_resize=0;
    }
  }
  it("refunds every right snapshot allocation including registry growth") {
    right.count=TEST_ROWS; check_equal(open_join(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK); reserve_calls=resize_calls=0;
    check_equal(next().state,ORM_SQL_SCAN_ROW); const size_t reserves=reserve_calls,resizes=resize_calls;
    for(size_t i=1;i<=reserves+resizes;++i) {
      reset(); check_equal(open_join(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK); reserve_calls=resize_calls=0;
      if(i<=reserves) fail_reserve=i; else fail_resize=i-reserves;
      locked(TURBODB_STATUS_OUT_OF_MEMORY);
    }
  }
  it("refunds parameter snapshot failures and never charges parameters as materialized rows") {
    const orm_sql_type inputs[]={{TURBODB_VALUE_TEXT,false},{TURBODB_VALUE_TEXT,false}};
    compile("SELECT ?=?",inputs,2,&condition);
    const turbodb_value_t parameter=turbodb_text("b"); const size_t mapping[]={3,TEST_WIDTH};
    const orm_sql_join_spec spec={ORM_SQL_JOIN_LEFT,&condition,mapping,2,&parameter,inputs,1};
    const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    reserve_calls=resize_calls=0;
    check_equal(orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error),TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    const size_t reserves=reserve_calls,resizes=resize_calls;
    check_equal(orm_tidesdb_sql_join_close(&run,&error),TURBODB_STATUS_OK);
    for(size_t i=1;i<=reserves+resizes;++i) {
      reserve_calls=resize_calls=0; if(i<=reserves) fail_reserve=i; else fail_resize=i-reserves;
      check_equal(orm_tidesdb_sql_join_open(&lsource,&rsource,&spec,&run,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      check_null(run.budget); check_false(lsource.active); check_false(rsource.active); check_equal(condition.active_runs,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u); fail_reserve=fail_resize=0;
    }
  }
  it("allocates no pair workspace after right capture and preserves terminal and control semantics") {
    equality("SELECT ?=?"); check_equal(open_join(ORM_SQL_JOIN_LEFT),TURBODB_STATUS_OK);
    const orm_sql_scan_row row=next(); check_equal(row.values[0].data.int64_value,1);
    check_equal(open_join(ORM_SQL_JOIN_LEFT),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_join_next(&run,NULL,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(row.values[0].data.int64_value,1);
    reserve_calls=resize_calls=0; fail_reserve=fail_resize=1;
    while(next().state==ORM_SQL_SCAN_ROW) {}
    check_equal(reserve_calls,0u); check_equal(resize_calls,0u);
    check_equal(orm_tidesdb_sql_join_cancel(&run,&error),TURBODB_STATUS_OK); check_equal(next().state,ORM_SQL_SCAN_DONE);
    reset(); check_equal(open_join(ORM_SQL_JOIN_CROSS),TURBODB_STATUS_OK); const orm_sql_scan_row borrowed=next();
    check_equal(orm_tidesdb_sql_join_cancel(&run,&error),TURBODB_STATUS_OK);
    for(size_t i=0;i<borrowed.count;++i) check_equal(borrowed.values[i].kind,TURBODB_VALUE_NULL);
    const size_t reads=left.calls+right.calls;
    check_equal(next().state,ORM_SQL_SCAN_CANCELLED); check_equal(left.calls+right.calls,reads);
  }
  it("keeps snapshot pointers stable and refuses further access after failed append") {
    orm_sql_rows store={.budget=&budget}; char payload[]="x";
    turbodb_value_t values[]={turbodb_text(payload),turbodb_blob(payload,1)}; turbodb_value_t *first=NULL,*copy=NULL;
    check_equal(orm_sql_rows_append(&store,values,2,1,&first,&error),TURBODB_STATUS_OK);
    check_equal(first[2].kind,TURBODB_VALUE_NULL); payload[0]='y';
    for(size_t i=1;i<TEST_ROWS;++i) check_equal(orm_sql_rows_append(&store,values,2,0,&copy,&error),TURBODB_STATUS_OK);
    check_true(orm_sql_rows_at(&store,0)==first); check_equal(first[0].data.text_value.data[0],'x');
    check_equal(((const char *)first[1].data.blob_value.data)[0],'x'); check_null(orm_sql_rows_at(&store,TEST_ROWS));
    budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=TEST_ROWS;
    copy=first; check_equal(orm_sql_rows_append(&store,values,2,0,&copy,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_true(copy==first); check_null(orm_sql_rows_at(&store,0));
    check_equal(orm_sql_rows_append(&store,values,2,0,&copy,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(orm_sql_rows_close(&store,&error),TURBODB_STATUS_OK); check_equal(orm_sql_rows_close(&store,&error),TURBODB_STATUS_OK);
  }
  it("rejects snapshot shape payload and allocation arithmetic overflow before copying") {
    orm_sql_snapshot snapshot={0}; turbodb_value_t value=turbodb_null();
    check_equal(orm_sql_snapshot_copy(&snapshot,&value,SIZE_MAX,0,&budget,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_sql_snapshot_copy(&snapshot,&value,1,SIZE_MAX,&budget,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    value=turbodb_blob(NULL,1);
    check_equal(orm_sql_snapshot_copy(&snapshot,&value,1,0,&budget,&error),TURBODB_STATUS_TYPE_ERROR);
    const char payload='x'; value=turbodb_blob(&payload,SIZE_MAX);
    check_equal(orm_sql_snapshot_copy(&snapshot,&value,1,0,&budget,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_false(snapshot.values.initialized); check_equal(snapshot.bytes,0u);
  }
  it("enforces every execution-step boundary including null extension without leaking") {
    compile("SELECT FALSE",NULL,0,&condition); check_equal(open_join(ORM_SQL_JOIN_LEFT),TURBODB_STATUS_OK);
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    while(next().state==ORM_SQL_SCAN_ROW) {}
    const uint64_t cost=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    for(uint64_t allowance=0;allowance<cost;++allowance) {
      reset(); compile("SELECT FALSE",NULL,0,&condition); check_equal(open_join(ORM_SQL_JOIN_LEFT),TURBODB_STATUS_OK);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+allowance;
      orm_sql_scan_row out; turbodb_status_t status;
      do { out=(orm_sql_scan_row){.count=99}; status=orm_tidesdb_sql_join_next(&run,&out,&error); }
      while(status==TURBODB_STATUS_OK && out.state==ORM_SQL_SCAN_ROW);
      check_equal(status,TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(out.count,99u); locked(status);
    }
  }
}
