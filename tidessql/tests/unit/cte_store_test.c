#include "cte_store.h"
#include "join.h"
#include <tinytest.h>
#include <string.h>

static size_t reserves,resizes,fail_reserve,fail_resize;
static stl_status cte_test_reserve(vec_t *v,size_t n) { return ++reserves==fail_reserve?STL_OUT_OF_MEMORY:vec_reserve(v,n); }
static stl_status cte_test_resize(vec_t *v,size_t n) { return ++resizes==fail_resize?STL_OUT_OF_MEMORY:vec_resize(v,n); }
#define vec_reserve cte_test_reserve
#define vec_resize cte_test_resize
#include "../../src/work.c"
#include "../../src/rows.c"
#include "../../src/scan.c"
#include "../../src/cte_store.c"
#undef vec_reserve
#undef vec_resize

enum { COLUMNS=3,READERS=3,PAYLOAD=3,TEST_LIMIT=65536,TEST_WORK=4*1024*1024,GROW_ROWS=20 };
static orm_tidesdb_sql_budget budget;
static orm_sql_budget_limits limits;
static orm_sql_cte_store store,other;
static orm_sql_cte_reader readers[READERS];
static orm_sql_scan consumer;
static orm_sql_join joined;
static orm_sql_type types[COLUMNS];
static orm_sql_row_source source;
static turbodb_error_t error;
static struct {
  size_t count,position,calls,fail_at;
  bool reenter,bad_type;
  unsigned char bytes[PAYLOAD];
  turbodb_value_t row[COLUMNS];
} fixture;

static turbodb_status_t pull(size_t reader,const turbodb_value_t **out) {
  orm_sql_row_source *s=orm_sql_cte_reader_source(&readers[reader]);
  check_not_null(s); return s->next(s->context,out,&error);
}
static turbodb_status_t input_next(void *context,const turbodb_value_t **out,turbodb_error_t *e) {
  check_true(context==&fixture); ++fixture.calls;
  if(fixture.reenter) {
    fixture.reenter=false;
    const turbodb_value_t *kept=fixture.row; turbodb_error_t nested;
    orm_sql_row_source *s=orm_sql_cte_reader_source(&readers[1]);
    check_equal(s->next(s->context,&kept,&nested),TURBODB_STATUS_BUSY); check_true(kept==fixture.row);
    check_equal(orm_sql_cte_store_cancel(&store,&nested),TURBODB_STATUS_BUSY);
    check_equal(orm_sql_cte_store_close(&store,&nested),TURBODB_STATUS_BUSY);
    check_equal(orm_sql_cte_reader_close(&readers[1],&nested),TURBODB_STATUS_BUSY);
    check_equal(orm_sql_cte_reader_rewind(&readers[1],&nested),TURBODB_STATUS_BUSY);
    check_equal(orm_sql_cte_reader_open(&store,&readers[2],&nested),TURBODB_STATUS_BUSY);
  }
  if(fixture.fail_at && fixture.calls==fixture.fail_at) {
    tdsql_error_set(e,TURBODB_STATUS_DATASTORE_ERROR,"test CTE source failure"); return TURBODB_STATUS_DATASTORE_ERROR;
  }
  if(fixture.position==fixture.count) { *out=NULL; return TURBODB_STATUS_OK; }
  orm_sql_budget_amount amount={0}; amount.value[ORM_SQL_BUDGET_READ_ROWS]=1;
  amount.value[ORM_SQL_BUDGET_READ_BYTES]=sizeof(fixture.row)+sizeof(fixture.bytes);
  const turbodb_status_t status=orm_tidesdb_sql_budget_reserve(&budget,&amount,e);
  if(status!=TURBODB_STATUS_OK) return status;
  memset(fixture.bytes,(int)('a'+fixture.position),sizeof(fixture.bytes));
  fixture.row[0]=fixture.bad_type?turbodb_bool(true):turbodb_i64((int64_t)fixture.position);
  fixture.row[1]=turbodb_text_v((vstr){(const char *)fixture.bytes,sizeof(fixture.bytes)});
  fixture.row[2]=turbodb_blob(fixture.bytes,sizeof(fixture.bytes));
  ++fixture.position; *out=fixture.row; return TURBODB_STATUS_OK;
}
static void clean(void) {
  fail_reserve=fail_resize=0;
  check_equal(orm_tidesdb_sql_scan_close(&consumer,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_join_close(&joined,&error),TURBODB_STATUS_OK);
  for(size_t i=0;i<READERS;++i) check_equal(orm_sql_cte_reader_close(&readers[i],&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_cte_store_close(&store,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_cte_store_close(&other,&error),TURBODB_STATUS_OK);
  check_false(source.active); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
  check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
}
static void setup(void) {
  reserves=resizes=fail_reserve=fail_resize=0; memset(&fixture,0,sizeof(fixture)); tdsql_error_init(&error);
  limits=(orm_sql_budget_limits){0};
  for(size_t i=0;i<ORM_SQL_BUDGET_RESOURCE_COUNT;++i) limits.statement.value[i]=TEST_LIMIT;
  limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=TEST_WORK;
  limits.transaction=(orm_sql_transaction_budget_amount){TEST_LIMIT,TEST_LIMIT,TEST_LIMIT};
  check_equal(orm_tidesdb_sql_budget_init(&budget,&limits,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
  types[0]=(orm_sql_type){TURBODB_VALUE_INT64,false}; types[1]=(orm_sql_type){TURBODB_VALUE_TEXT,true};
  types[2]=(orm_sql_type){TURBODB_VALUE_BLOB,false};
  source=(orm_sql_row_source){&budget,types,COLUMNS,&fixture,input_next,false};
}
static void reset(void) { clean(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); setup(); }
static void open_readers(size_t count) {
  fixture.count=count; check_equal(orm_sql_cte_store_open(&source,&store,&error),TURBODB_STATUS_OK);
  for(size_t i=0;i<2;++i) check_equal(orm_sql_cte_reader_open(&store,&readers[i],&error),TURBODB_STATUS_OK);
}
static const turbodb_value_t *next(size_t reader) {
  const turbodb_value_t *row=NULL; check_equal(pull(reader,&row),TURBODB_STATUS_OK); return row;
}

spec("TidesDB private shared CTE materialization") {
  before_each() { setup(); }
  after_each() { clean(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); }
  it("opens without reading and gives each reference an independent position") {
    open_readers(3); check_equal(fixture.calls,0u); check_true(source.active); check_equal(store.readers,2u);
    check_equal(next(0)[0].data.int64_value,0); check_equal(fixture.calls,4u);
    check_equal(next(0)[0].data.int64_value,1); check_equal(next(1)[0].data.int64_value,0);
    check_equal(next(1)[0].data.int64_value,1); check_equal(next(1)[0].data.int64_value,2);
    check_null(next(1)); check_equal(next(0)[0].data.int64_value,2); check_null(next(0));
    check_equal(fixture.calls,4u); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],3u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],3u);
  }
  it("replays a READY result from the beginning to late readers without input reads") {
    open_readers(2); check_equal(next(0)[0].data.int64_value,0); check_equal(next(0)[0].data.int64_value,1); check_null(next(0));
    check_equal(orm_sql_cte_reader_close(&readers[0],&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_reader_open(&store,&readers[0],&error),TURBODB_STATUS_OK);
    check_equal(next(0)[0].data.int64_value,0); check_equal(fixture.calls,3u);
  }
  it("rewinds only one reference while preserving cached rows and leases") {
    open_readers(2); const turbodb_value_t *first=next(0); check_not_null(first);
    check_equal(next(0)[0].data.int64_value,1); check_null(next(0)); check_not_null(next(1));
    orm_sql_row_source *stable=orm_sql_cte_reader_source(&readers[0]); const orm_sql_type *schema=stable->types;
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    const size_t allocations=reserves;
    check_equal(orm_sql_cte_reader_rewind(&readers[0],&error),TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps+1);
    check_equal(reserves,allocations); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    check_true(orm_sql_cte_reader_source(&readers[0])==stable); check_true(stable->types==schema);
    check_equal(store.readers,2u); check_equal(readers[1].position,1u);
    check_true(next(0)==first); check_equal(next(1)[0].data.int64_value,1);
    check_equal(memcmp(first[1].data.text_value.data,"aaa",PAYLOAD),0);
    check_equal(fixture.calls,3u); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],2u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],2u);
  }
  it("keeps PENDING readers lazy and replays empty results without rerunning input") {
    check_equal(orm_sql_cte_reader_rewind(NULL,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(orm_sql_cte_reader_rewind(&readers[0],&error),TURBODB_STATUS_INVALID_STATE);
    open_readers(0);
    check_equal(orm_sql_cte_reader_rewind(&readers[0],&error),TURBODB_STATUS_OK);
    check_equal(store.state,ORM_SQL_CTE_PENDING); check_equal(fixture.calls,0u);
    check_null(next(0)); check_equal(fixture.calls,1u);
    check_equal(orm_sql_cte_reader_rewind(&readers[0],&error),TURBODB_STATUS_OK);
    check_null(next(0)); check_equal(fixture.calls,1u);
  }
  it("requires consumer close before rewind after EOF or cancellation") {
    open_readers(1); const size_t projection=0;
    const orm_sql_scan_spec spec={.projection=&projection,.projection_count=1,.limit=UINT64_MAX};
    check_equal(orm_tidesdb_sql_scan_open_source(orm_sql_cte_reader_source(&readers[0]),&spec,&budget,&consumer,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_reader_rewind(&readers[0],&error),TURBODB_STATUS_BUSY);
    orm_sql_scan_row row={0};
    check_equal(orm_tidesdb_sql_scan_next(&consumer,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_ROW);
    check_equal(orm_tidesdb_sql_scan_next(&consumer,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
    check_equal(orm_sql_cte_reader_rewind(&readers[0],&error),TURBODB_STATUS_BUSY); check_equal(readers[0].position,1u);
    check_equal(orm_tidesdb_sql_scan_cancel(&consumer,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_reader_rewind(&readers[0],&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_scan_close(&consumer,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_reader_rewind(&readers[0],&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_scan_open_source(orm_sql_cte_reader_source(&readers[0]),&spec,&budget,&consumer,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_scan_next(&consumer,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_ROW);
    check_equal(row.values[0].data.int64_value,0); check_equal(fixture.calls,2u);
  }
  it("preserves reader position and shared state when the rewind step is exhausted") {
    open_readers(2); check_not_null(next(0));
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=steps;
    check_equal(orm_sql_cte_reader_rewind(&readers[0],&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(readers[0].position,1u); check_equal(store.state,ORM_SQL_CTE_READY);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=TEST_LIMIT;
    check_equal(next(0)[0].data.int64_value,1); check_equal(next(1)[0].data.int64_value,0);
  }
  it("retains declared nullable types for empty results and caches EOF") {
    open_readers(0); orm_sql_row_source *s=orm_sql_cte_reader_source(&readers[0]);
    check_equal(s->columns,COLUMNS); check_true(s->types!=types); check_equal(s->types[1].kind,TURBODB_VALUE_TEXT); check_true(s->types[1].nullable);
    check_null(next(0)); check_null(next(1)); check_equal(fixture.calls,1u); check_equal(store.state,ORM_SQL_CTE_READY);
  }
  it("deep copies TEXT and BLOB from the same reused producer buffer") {
    open_readers(3); const turbodb_value_t *first=next(0); check_not_null(first);
    memset(fixture.bytes,'z',sizeof(fixture.bytes)); const turbodb_value_t *second=next(0);
    check_equal(memcmp(first[1].data.text_value.data,"aaa",PAYLOAD),0);
    check_equal(memcmp(first[2].data.blob_value.data,"aaa",PAYLOAD),0);
    check_equal(memcmp(second[1].data.text_value.data,"bbb",PAYLOAD),0);
    check_true(next(1)==first); check_equal(memcmp(first[1].data.text_value.data,"aaa",PAYLOAD),0);
  }
  it("feeds a self CROSS JOIN from two readers with one producer execution") {
    open_readers(2); const orm_sql_join_spec spec={.kind=ORM_SQL_JOIN_CROSS};
    check_equal(orm_tidesdb_sql_join_open(orm_sql_cte_reader_source(&readers[0]),orm_sql_cte_reader_source(&readers[1]),&spec,&joined,&error),TURBODB_STATUS_OK);
    orm_sql_row_source *s=orm_tidesdb_sql_join_source(&joined);
    for(int64_t left=0;left<2;++left) for(int64_t right=0;right<2;++right) {
      const turbodb_value_t *row=NULL; check_equal(s->next(s->context,&row,&error),TURBODB_STATUS_OK); check_not_null(row);
      check_equal(row[0].data.int64_value,left); check_equal(row[COLUMNS].data.int64_value,right);
    }
    const turbodb_value_t *row=NULL; check_equal(s->next(s->context,&row,&error),TURBODB_STATUS_OK); check_null(row);
    check_equal(fixture.calls,3u); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],2u);
  }
  it("keeps producer and reader leases until consumers close") {
    open_readers(1); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_sql_cte_store_open(&source,&other,&error),TURBODB_STATUS_BUSY); check_null(other.budget);
    check_equal(orm_sql_cte_store_close(&store,&error),TURBODB_STATUS_BUSY);
    const size_t projection=0; const orm_sql_scan_spec spec={.projection=&projection,.projection_count=1,.limit=0};
    check_equal(orm_tidesdb_sql_scan_open_source(orm_sql_cte_reader_source(&readers[0]),&spec,&budget,&consumer,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_reader_close(&readers[0],&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_scan_cancel(&consumer,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_reader_close(&readers[0],&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_scan_close(&consumer,&error),TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_equal(fixture.calls,0u);
    check_equal(next(1)[0].data.int64_value,0); check_equal(store.state,ORM_SQL_CTE_READY);
  }
  it("shares the first producer failure without publishing a partial result or retrying") {
    open_readers(3); fixture.fail_at=2; const turbodb_value_t *row=fixture.row;
    check_equal(pull(0,&row),TURBODB_STATUS_DATASTORE_ERROR); check_true(row==fixture.row); check_contains(error.message,"test CTE source failure");
    check_equal(store.state,ORM_SQL_CTE_FAILED); check_equal(vec_size(&store.rows.snapshots),1u);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_sql_cte_store_cancel(&store,&error),TURBODB_STATUS_OK);
    check_equal(pull(1,&row),TURBODB_STATUS_DATASTORE_ERROR); check_true(row==fixture.row);
    check_equal(orm_sql_cte_reader_open(&store,&readers[2],&error),TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(orm_sql_cte_reader_rewind(&readers[0],&error),TURBODB_STATUS_DATASTORE_ERROR);
    check_contains(error.message,"test CTE source failure"); check_equal(readers[0].position,0u);
    check_equal(fixture.calls,2u); check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
  }
  it("validates every producer value against the declared type before caching") {
    open_readers(1); fixture.bad_type=true; const turbodb_value_t *row=fixture.row;
    check_equal(pull(0,&row),TURBODB_STATUS_TYPE_ERROR); check_true(row==fixture.row);
    check_equal(vec_size(&store.rows.snapshots),0u); check_equal(pull(1,&row),TURBODB_STATUS_TYPE_ERROR); check_equal(fixture.calls,1u);
  }
  it("cancels PENDING or READY stores for all readers without freeing borrowed rows") {
    for(size_t pass=0;pass<2;++pass) {
      if(pass) reset(); open_readers(2); const turbodb_value_t *retained=pass?next(0):NULL;
      const size_t calls=fixture.calls; const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(orm_sql_cte_store_cancel(&store,&error),TURBODB_STATUS_OK); const turbodb_value_t *row=fixture.row;
      check_equal(pull(0,&row),TURBODB_STATUS_INVALID_STATE); check_true(row==fixture.row);
      check_equal(pull(1,&row),TURBODB_STATUS_INVALID_STATE); check_equal(fixture.calls,calls);
      check_equal(orm_sql_cte_reader_open(&store,&readers[2],&error),TURBODB_STATUS_INVALID_STATE);
      const size_t position=readers[0].position;
      check_equal(orm_sql_cte_reader_rewind(&readers[0],&error),TURBODB_STATUS_INVALID_STATE);
      check_equal(readers[0].position,position); check_equal(store.state,ORM_SQL_CTE_CANCELLED);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      if(retained) check_equal(retained[0].data.int64_value,0);
    }
  }
  it("rejects callback reentry without disturbing materialization") {
    open_readers(2); fixture.reenter=true; check_equal(next(0)[0].data.int64_value,0);
    check_equal(store.state,ORM_SQL_CTE_READY); check_equal(next(1)[0].data.int64_value,0);
    check_equal(fixture.calls,3u); check_equal(store.readers,2u);
  }
  it("bounds cached rows and exposes no partial result when materialization exceeds the cap") {
    open_readers(3); budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=1;
    const turbodb_value_t *row=fixture.row; check_equal(pull(0,&row),TURBODB_STATUS_LIMIT_EXCEEDED); check_true(row==fixture.row);
    check_equal(pull(1,&row),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(fixture.calls,2u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],1u);
  }
  it("charges reader metadata and prevents an extra reader when work is full") {
    open_readers(1); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=work;
    check_equal(orm_sql_cte_reader_open(&store,&readers[2],&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(readers[2].store); check_equal(store.readers,2u); check_equal(fixture.calls,0u);
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=TEST_WORK;
  }
  it("latches step exhaustion while reading an already cached result") {
    open_readers(1); check_not_null(next(0));
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    const turbodb_value_t *row=fixture.row; check_equal(pull(1,&row),TURBODB_STATUS_LIMIT_EXCEEDED); check_true(row==fixture.row);
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=TEST_LIMIT;
    check_equal(pull(0,&row),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(fixture.calls,2u);
  }
  it("fails bounded work during materialization and preserves all reader outputs") {
    open_readers(2); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=work;
    const turbodb_value_t *row=fixture.row;
    check_equal(pull(0,&row),TURBODB_STATUS_LIMIT_EXCEEDED); check_true(row==fixture.row);
    check_equal(pull(1,&row),TURBODB_STATUS_LIMIT_EXCEEDED); check_true(row==fixture.row);
    check_equal(fixture.calls,1u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=TEST_WORK;
  }
  it("refunds leases and workspace at every construction and materialization step boundary") {
    open_readers(2); check_not_null(next(0));
    const uint64_t total=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    for(uint64_t point=0;point<total;++point) {
      reset(); fixture.count=2; budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=point;
      turbodb_status_t status=orm_sql_cte_store_open(&source,&store,&error);
      if(status==TURBODB_STATUS_OK) status=orm_sql_cte_reader_open(&store,&readers[0],&error);
      const turbodb_value_t *row=fixture.row;
      if(status==TURBODB_STATUS_OK) status=pull(0,&row);
      check_equal(status,TURBODB_STATUS_LIMIT_EXCEEDED); check_true(row==fixture.row);
      clean();
    }
  }
  it("refunds every reserve and resize failure in construction and row-cache growth") {
    open_readers(GROW_ROWS); check_not_null(next(0)); const size_t calls[]={reserves,resizes};
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=calls[pass];++point) {
      reset(); fixture.count=GROW_ROWS; if(pass) fail_resize=point; else fail_reserve=point;
      turbodb_status_t status=orm_sql_cte_store_open(&source,&store,&error);
      if(status==TURBODB_STATUS_OK) status=orm_sql_cte_reader_open(&store,&readers[0],&error);
      const turbodb_value_t *row=fixture.row;
      if(status==TURBODB_STATUS_OK) status=pull(0,&row);
      check_equal(status,TURBODB_STATUS_OUT_OF_MEMORY); check_true(row==fixture.row);
      fail_reserve=fail_resize=0; clean();
    }
  }
  it("rejects occupied outputs and invalid schemas without consuming the producer") {
    check_equal(orm_sql_cte_store_open(NULL,&store,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    source.columns=0; check_equal(orm_sql_cte_store_open(&source,&store,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    source.columns=SIZE_MAX; check_equal(orm_sql_cte_store_open(&source,&store,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    source.columns=COLUMNS; types[0]=(orm_sql_type){TURBODB_VALUE_NULL,false};
    check_equal(orm_sql_cte_store_open(&source,&store,&error),TURBODB_STATUS_TYPE_ERROR); check_false(source.active); check_null(store.budget);
    types[0]=(orm_sql_type){TURBODB_VALUE_INT64,false}; open_readers(1);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_sql_cte_store_open(&source,&store,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_sql_cte_reader_open(&store,&readers[0],&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_equal(fixture.calls,0u);
  }
}
