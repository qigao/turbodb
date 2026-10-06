#include "cte_store.h"
#include "select.h"
#include "union.h"
#include <tinytest.h>
#include <string.h>

static size_t reserves,resizes,fail_reserve,fail_resize;
static stl_status recursive_reserve(vec_t *v,size_t n) { return ++reserves==fail_reserve?STL_OUT_OF_MEMORY:vec_reserve(v,n); }
static stl_status recursive_resize(vec_t *v,size_t n) { return ++resizes==fail_resize?STL_OUT_OF_MEMORY:vec_resize(v,n); }
#define vec_reserve recursive_reserve
#define vec_resize recursive_resize
#include "../../src/work.c"
#include "../../src/rows.c"
#include "../../src/scan.c"
#include "../../src/cte_store.c"
#undef vec_reserve
#undef vec_resize

enum { COLUMNS=3,SEED_ROWS=4,READERS=3,MEMBERS=2,ITERATIONS=16,DEPTH=32,STEPS=65536,WORK=4*1024*1024 };
static orm_tidesdb_sql_budget budget;
static orm_sql_budget_limits limits;
static orm_sql_cte_store store;
static orm_sql_cte_reader readers[READERS];
static orm_sql_row_source seed;
static orm_sql_type seed_types[COLUMNS];
static turbodb_error_t error;
static struct {
  turbodb_value_t values[SEED_ROWS][COLUMNS];
  size_t count,position,calls;
  char payload[sizeof("abc")];
  bool poison_payload;
} anchor;
static struct {
  orm_sql_select plan;
  orm_sql_select_run run;
  orm_sql_row_source source,*frontier;
  orm_sql_type types[COLUMNS];
  size_t opens,closes,pulls,fail_pull,fail_close;
  bool fail_open,reenter,wrong_width,wrong_type,no_source,identity;
  bool multiple,advance_default,leave_reader,probe_rewind;
  orm_sql_select other_plan;
  orm_sql_select_run other_run;
  orm_sql_row_source other_source;
  orm_sql_cte_frontier_reader forks[MEMBERS];
  orm_sql_union combined;
} member;

static turbodb_status_t pull(size_t reader,const turbodb_value_t **out) {
  orm_sql_row_source *s=orm_sql_cte_reader_source(&readers[reader]);
  check_not_null(s); return s->next(s->context,out,&error);
}
static const turbodb_value_t *next(size_t reader) {
  const turbodb_value_t *row=NULL; check_equal(pull(reader,&row),TURBODB_STATUS_OK); return row;
}
static turbodb_status_t seed_pull(void *context,const turbodb_value_t **out,turbodb_error_t *e) {
  (void)e; check_true(context==&anchor); ++anchor.calls;
  if(anchor.poison_payload && anchor.position==anchor.count) memset(anchor.payload,'x',sizeof(anchor.payload)-1);
  *out=anchor.position==anchor.count?NULL:anchor.values[anchor.position++]; return TURBODB_STATUS_OK;
}
static void reentry(void) {
  const turbodb_value_t *row=anchor.values[0]; turbodb_error_t nested;
  check_equal(pull(0,&row),TURBODB_STATUS_BUSY); check_true(row==anchor.values[0]);
  check_equal(orm_sql_cte_store_cancel(&store,&nested),TURBODB_STATUS_BUSY);
  check_equal(orm_sql_cte_store_close(&store,&nested),TURBODB_STATUS_BUSY);
  check_equal(orm_sql_cte_reader_open(&store,&readers[2],&nested),TURBODB_STATUS_BUSY);
  check_equal(orm_sql_cte_reader_close(&readers[1],&nested),TURBODB_STATUS_BUSY);
}
static turbodb_status_t member_pull(void *context,const turbodb_value_t **out,turbodb_error_t *e) {
  check_true(context==&member); ++member.pulls;
  if(member.fail_pull && member.pulls==member.fail_pull) {
    tdsql_error_set(e,TURBODB_STATUS_DATASTORE_ERROR,"recursive member pull failed"); return TURBODB_STATUS_DATASTORE_ERROR;
  }
  if(member.reenter) reentry();
  orm_sql_scan_row row;
  const turbodb_status_t status=orm_tidesdb_sql_scan_next(&member.run.scan,&row,e);
  if(status==TURBODB_STATUS_OK) *out=row.state==ORM_SQL_SCAN_ROW?row.values:NULL;
  return status;
}
static turbodb_status_t other_member_pull(void *context,const turbodb_value_t **out,turbodb_error_t *e) {
  orm_sql_select_run *run=context; orm_sql_scan_row row;
  const turbodb_status_t status=orm_tidesdb_sql_scan_next(&run->scan,&row,e);
  if(status==TURBODB_STATUS_OK) *out=row.state==ORM_SQL_SCAN_ROW?row.values:NULL;
  return status;
}
static turbodb_status_t multiple_open(orm_sql_row_source *frontier,orm_sql_row_source **out,turbodb_error_t *e) {
  turbodb_status_t status=TURBODB_STATUS_OK;
  if(member.advance_default) { const turbodb_value_t *row=NULL; status=frontier->next(frontier->context,&row,e); }
  for(size_t i=0;status==TURBODB_STATUS_OK && i<MEMBERS;++i) status=orm_sql_cte_frontier_open(frontier,&member.forks[i],e);
  if(status==TURBODB_STATUS_OK&&member.probe_rewind) {
    orm_sql_cte_frontier_reader *reader=&member.forks[0]; const size_t other=member.forks[1].position;
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],steps=budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    const turbodb_value_t *first=NULL,*again=NULL;
    check_equal(reader->source.next(reader->source.context,&first,e),TURBODB_STATUS_OK); check_not_null(first);
    const size_t position=reader->position; budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_sql_cte_frontier_rewind(reader,e),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(reader->position,position);
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=steps;
    --reader->iteration; check_equal(orm_sql_cte_frontier_rewind(reader,e),TURBODB_STATUS_INVALID_STATE); ++reader->iteration;
    check_equal(reader->position,position); check_equal(orm_sql_cte_frontier_rewind(reader,e),TURBODB_STATUS_OK);
    check_equal(reader->source.next(reader->source.context,&again,e),TURBODB_STATUS_OK); check_true(first==again);
    check_equal(member.forks[1].position,other); check_equal(orm_sql_cte_frontier_rewind(reader,e),TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_equal(store.frontier_readers,MEMBERS);
  }
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_select_open_source(&member.plan,&member.forks[0].source,NULL,0,&member.run,e);
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_select_open_source(&member.other_plan,&member.forks[1].source,NULL,0,&member.other_run,e);
  if(status==TURBODB_STATUS_OK) {
    turbodb_error_t nested;
    check_equal(orm_sql_cte_frontier_close(&member.forks[0],&nested),TURBODB_STATUS_BUSY);
    if(member.probe_rewind) check_equal(orm_sql_cte_frontier_rewind(&member.forks[0],&nested),TURBODB_STATUS_BUSY);
    check_equal(orm_sql_cte_frontier_open(frontier,&member.forks[0],&nested),TURBODB_STATUS_INVALID_ARGUMENT);
    member.source=(orm_sql_row_source){&budget,member.types,seed.columns,&member,member_pull,false};
    member.other_source=(orm_sql_row_source){&budget,member.types,seed.columns,&member.other_run,other_member_pull,false};
    status=orm_tidesdb_sql_union_open(&member.source,&member.other_source,ORM_SQL_UNION_ALL,&member.combined,e);
  }
  if(status==TURBODB_STATUS_OK) *out=orm_tidesdb_sql_union_source(&member.combined);
  return status;
}
static turbodb_status_t member_open(void *context,orm_sql_row_source *frontier,orm_sql_row_source **out,turbodb_error_t *e) {
  check_true(context==&member); ++member.opens; member.frontier=frontier;
  if(member.reenter) reentry();
  if(member.multiple) return multiple_open(frontier,out,e);
  if(member.identity) { *out=frontier; return TURBODB_STATUS_OK; }
  turbodb_status_t status=orm_tidesdb_sql_select_open_source(&member.plan,frontier,NULL,0,&member.run,e);
  if(status!=TURBODB_STATUS_OK) return status;
  if(member.fail_open) { tdsql_error_set(e,TURBODB_STATUS_DATASTORE_ERROR,"recursive member open failed"); return TURBODB_STATUS_DATASTORE_ERROR; }
  member.source=(orm_sql_row_source){&budget,member.types,member.wrong_width?COLUMNS+1:vec_size(&member.plan.columns),
      &member,member_pull,false};
  if(member.wrong_type) member.types[0]=(orm_sql_type){TURBODB_VALUE_BOOLEAN,true};
  *out=member.no_source?NULL:&member.source; return TURBODB_STATUS_OK;
}
static turbodb_status_t member_close(void *context,turbodb_error_t *e) {
  check_true(context==&member); ++member.closes;
  if(member.reenter && store.readers) reentry();
  if(member.fail_close) { --member.fail_close; tdsql_error_set(e,TURBODB_STATUS_BUSY,"recursive member close blocked"); return TURBODB_STATUS_BUSY; }
  turbodb_status_t status=orm_tidesdb_sql_union_close(&member.combined,e);
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_select_close(&member.run,e);
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_select_close(&member.other_run,e);
  for(size_t i=0;status==TURBODB_STATUS_OK && i<MEMBERS;++i) {
    if(member.leave_reader && !i) continue;
    status=orm_sql_cte_frontier_close(&member.forks[i],e);
  }
  return status;
}
static orm_sql_cte_recursion recursion(bool distinct,uint64_t iterations) {
  return (orm_sql_cte_recursion){&member,member_open,member_close,iterations,distinct};
}
static void setup(void) {
  reserves=resizes=fail_reserve=fail_resize=0;
  memset(&anchor,0,sizeof(anchor)); memset(&member,0,sizeof(member)); tdsql_error_init(&error);
  limits=(orm_sql_budget_limits){0};
  for(size_t i=0;i<ORM_SQL_BUDGET_RESOURCE_COUNT;++i) limits.statement.value[i]=STEPS;
  limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=WORK;
  limits.transaction=(orm_sql_transaction_budget_amount){STEPS,STEPS,STEPS};
  check_equal(orm_tidesdb_sql_budget_init(&budget,&limits,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
  for(size_t i=0;i<COLUMNS;++i) seed_types[i]=(orm_sql_type){TURBODB_VALUE_INT64,false};
  seed=(orm_sql_row_source){&budget,seed_types,1,&anchor,seed_pull,false};
  anchor.count=1; anchor.values[0][0]=turbodb_i64(1);
}
static void clean(void) {
  fail_reserve=fail_resize=0; member.fail_close=0; member.reenter=false; member.leave_reader=false;
  for(size_t i=0;i<READERS;++i) check_equal(orm_sql_cte_reader_close(&readers[i],&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_cte_store_close(&store,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_select_destroy(&member.plan,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_select_destroy(&member.other_plan,&error),TURBODB_STATUS_OK);
  check_false(seed.active); check_null(member.run.program);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
  check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
}
static void reset(void) { clean(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); setup(); }
static void prepare(const char *sql) {
  sqlparser_document *doc=NULL; sqlparser_error e;
  check_equal(sqlparser_parse(sql,strlen(sql),NULL,&doc,&e),SQLPARSER_OK);
  const char *names[]={"n","p","q"}; orm_sql_schema_column columns[COLUMNS];
  for(size_t i=0;i<seed.columns;++i) {
    columns[i]=(orm_sql_schema_column){vstr_from_cstr(names[i]),seed_types[i]}; columns[i].type.nullable=true;
  }
  const orm_sql_table_schema schema={vstr_from_cstr("c"),columns,seed.columns};
  check_equal(orm_tidesdb_sql_select_bind(doc,&schema,DEPTH,&budget,&member.plan,&error),TURBODB_STATUS_OK);
  sqlparser_document_destroy(doc);
  check_true(vec_size(&member.plan.columns)<=COLUMNS);
  for(size_t i=0;i<vec_size(&member.plan.columns);++i) member.types[i]=orm_tidesdb_sql_select_column_at(&member.plan,i)->type;
}
static turbodb_status_t open_store(bool distinct,uint64_t iterations) {
  const orm_sql_cte_recursion spec=recursion(distinct,iterations);
  return orm_sql_cte_store_open_recursive(&seed,&spec,&store,&error);
}
static void prepare_multiple(const char *second) {
  prepare("SELECT n+1 AS n FROM c WHERE n<3");
  sqlparser_document *doc=NULL; sqlparser_error e;
  check_equal(sqlparser_parse(second,strlen(second),NULL,&doc,&e),SQLPARSER_OK);
  const orm_sql_schema_column column={vstr_from_cstr("n"),{TURBODB_VALUE_INT64,true}};
  const orm_sql_table_schema schema={vstr_from_cstr("c"),&column,1};
  check_equal(orm_tidesdb_sql_select_bind(doc,&schema,DEPTH,&budget,&member.other_plan,&error),TURBODB_STATUS_OK);
  sqlparser_document_destroy(doc); member.multiple=true;
}
static void open_readers(bool distinct,uint64_t iterations) {
  check_equal(open_store(distinct,iterations),TURBODB_STATUS_OK);
  check_equal(orm_sql_cte_reader_open(&store,&readers[0],&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_cte_reader_open(&store,&readers[1],&error),TURBODB_STATUS_OK);
}
static void open_page(bool distinct,uint64_t iterations,uint64_t offset,uint64_t limit) {
  orm_sql_cte_recursion spec=recursion(distinct,iterations);
  spec.paged=true; spec.offset=offset; spec.limit=limit;
  check_equal(orm_sql_cte_store_open_recursive(&seed,&spec,&store,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_cte_reader_open(&store,&readers[0],&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_cte_reader_open(&store,&readers[1],&error),TURBODB_STATUS_OK);
}

spec("TidesDB private recursive CTE rounds") {
  before_each() { setup(); }
  after_each() { clean(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); }
  it("returns a zero page without pulling seed or opening a recursive round") {
    prepare("SELECT n+1 AS n FROM c"); open_page(false,1,UINT64_MAX,0);
    check_null(next(0)); check_null(next(1)); check_equal(anchor.calls,0u); check_equal(member.opens,0u);
    check_equal(store.state,ORM_SQL_CTE_READY); check_equal(vec_size(&store.rows.snapshots),0u);
  }
  it("stops inside the seed without another pull or iteration-limit probe") {
    anchor.count=SEED_ROWS; prepare("SELECT n+1 AS n FROM c"); member.fail_open=true;
    open_page(false,1,0,1); check_equal(next(0)[0].data.int64_value,1); check_null(next(0));
    check_equal(anchor.calls,1u); check_equal(member.opens,0u); check_equal(store.iterations,0u);
  }
  it("keeps skipped rows in the frontier and independently replays the visible page") {
    prepare("SELECT n+1 AS n FROM c"); open_page(false,3,2,2);
    for(size_t r=0;r<2;++r) {
      check_equal(next(r)[0].data.int64_value,3); check_equal(next(r)[0].data.int64_value,4); check_null(next(r));
    }
    check_equal(store.iterations,3u); check_equal(member.opens,3u); check_equal(member.closes,3u);
    check_equal(vec_size(&store.rows.snapshots),4u);
    check_equal(orm_sql_cte_reader_rewind(&readers[0],&error),TURBODB_STATUS_OK);
    check_equal(next(0)[0].data.int64_value,3); check_equal(member.opens,3u);
  }
  it("stops mid-round and closes all consumers at the accepted row cap") {
    anchor.count=2; anchor.values[1][0]=turbodb_i64(10); prepare("SELECT n+1 AS n FROM c");
    member.fail_pull=2; open_page(false,1,0,3);
    const int64_t expected[]={1,10,2};
    for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) check_equal(next(0)[0].data.int64_value,expected[i]);
    check_null(next(0)); check_equal(member.pulls,1u); check_equal(member.closes,1u); check_false(store.round_open);
    check_false(store.frontier.active); check_equal(store.frontier_readers,0u);
  }
  it("does not overflow offset plus limit and returns empty after natural convergence") {
    prepare("SELECT n+1 AS n FROM c WHERE n<3"); open_page(false,ITERATIONS,UINT64_MAX,UINT64_MAX);
    check_null(next(0)); check_null(next(1)); check_equal(vec_size(&store.rows.snapshots),3u);
    check_equal(store.state,ORM_SQL_CTE_READY); check_equal(member.opens,3u);
  }
  it("counts accepted DISTINCT rows rather than repeated candidates toward the page") {
    anchor.count=2; anchor.values[1][0]=turbodb_i64(1);
    prepare("SELECT n+1 AS n FROM c WHERE n<3"); open_page(true,2,1,2);
    check_equal(next(0)[0].data.int64_value,2); check_equal(next(0)[0].data.int64_value,3); check_null(next(0));
    check_equal(vec_size(&store.rows.snapshots),3u); check_equal(member.opens,2u);
  }
  it("does not publish a completed page when final round cleanup fails") {
    prepare("SELECT n+1 AS n FROM c"); member.fail_close=1; open_page(false,1,0,2);
    const turbodb_value_t sentinel=turbodb_i64(99),*row=&sentinel;
    check_equal(pull(0,&row),TURBODB_STATUS_BUSY); check_true(row==&sentinel); check_equal(store.state,ORM_SQL_CTE_FAILED);
    check_equal(pull(1,&row),TURBODB_STATUS_BUSY); check_true(row==&sentinel);
  }
  it("still fails without a partial page when iteration quota expires before the row cap") {
    prepare("SELECT n+1 AS n FROM c"); open_page(false,1,1,2);
    const turbodb_value_t sentinel=turbodb_i64(99),*row=&sentinel;
    check_equal(pull(0,&row),TURBODB_STATUS_LIMIT_EXCEEDED); check_true(row==&sentinel);
    check_equal(pull(1,&row),TURBODB_STATUS_LIMIT_EXCEEDED); check_true(row==&sentinel);
    check_equal(store.state,ORM_SQL_CTE_FAILED); check_equal(member.opens,1u); check_equal(member.closes,1u);
  }
  it("gives compiled recursive UNION members independent fixed-range frontier readers") {
    anchor.count=2; anchor.values[1][0]=turbodb_i64(10);
    prepare_multiple("SELECT n+10 AS n FROM c WHERE n<3"); member.advance_default=true;
    open_readers(false,ITERATIONS);
    check_equal(orm_sql_cte_frontier_open(&store.frontier,&member.forks[0],&error),TURBODB_STATUS_INVALID_STATE);
    const int64_t expected[]={1,10,2,11,3,12};
    for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) check_equal(next(0)[0].data.int64_value,expected[i]);
    check_null(next(0)); check_equal(member.opens,3u); check_equal(member.closes,member.opens);
    check_equal(store.frontier_readers,0u); check_null(member.forks[0].store); check_null(member.forks[1].store);
    check_equal(vec_size(&store.rows.snapshots),sizeof(expected)/sizeof(expected[0]));
  }
  it("deduplicates overlapping recursive members against one cumulative result") {
    prepare_multiple("SELECT n+1 AS n FROM c WHERE n<3"); open_readers(true,ITERATIONS);
    for(int64_t n=1;n<=3;++n) check_equal(next(0)[0].data.int64_value,n);
    check_null(next(0)); check_equal(member.opens,3u); check_equal(store.frontier_readers,0u);
    check_equal(vec_size(&store.rows.snapshots),3u);
  }
  it("rewinds one immutable frontier cursor without changing peers leases or a failed cursor") {
    anchor.count=2; anchor.values[1][0]=turbodb_i64(10);
    prepare_multiple("SELECT n+10 AS n FROM c WHERE n<3"); member.probe_rewind=true;
    check_equal(orm_sql_cte_frontier_rewind(NULL,&error),TURBODB_STATUS_INVALID_STATE);
    open_readers(false,ITERATIONS);
    const int64_t expected[]={1,10,2,11,3,12};
    for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) check_equal(next(0)[0].data.int64_value,expected[i]);
    check_null(next(0)); check_equal(member.opens,3u); check_equal(store.frontier_readers,0u);
    check_equal(orm_sql_cte_frontier_rewind(&member.forks[0],&error),TURBODB_STATUS_INVALID_STATE);
  }
  it("retains the store when a factory leaks a frontier reader and permits later cleanup") {
    prepare_multiple("SELECT n+10 AS n FROM c WHERE n<3"); member.leave_reader=true;
    open_readers(false,ITERATIONS); const turbodb_value_t *row=anchor.values[0];
    check_equal(pull(0,&row),TURBODB_STATUS_BUSY); check_true(row==anchor.values[0]);
    check_equal(store.state,ORM_SQL_CTE_FAILED); check_true(store.round_open); check_equal(store.frontier_readers,1u);
    check_equal(member.forks[0].source.next(member.forks[0].source.context,&row,&error),TURBODB_STATUS_INVALID_STATE);
    check_true(row==anchor.values[0]);
    for(size_t i=0;i<READERS;++i) check_equal(orm_sql_cte_reader_close(&readers[i],&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_store_close(&store,&error),TURBODB_STATUS_BUSY); check_not_null(store.budget);
    check_equal(orm_sql_cte_frontier_close(&member.forks[0],&error),TURBODB_STATUS_OK);
    check_equal(store.frontier_readers,0u); check_equal(orm_sql_cte_store_close(&store,&error),TURBODB_STATUS_OK);
    check_false(seed.active);
  }
  it("refunds all fixed allocations while constructing and executing multiple frontier consumers") {
    prepare_multiple("SELECT n+10 AS n FROM c WHERE n<3"); reserves=resizes=0;
    open_readers(false,ITERATIONS); check_not_null(next(0)); const size_t counts[]={reserves,resizes};
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=counts[pass];++point) {
      reset(); prepare_multiple("SELECT n+10 AS n FROM c WHERE n<3"); reserves=resizes=0;
      if(pass) fail_resize=point; else fail_reserve=point;
      turbodb_status_t status=open_store(false,ITERATIONS);
      if(status==TURBODB_STATUS_OK) status=orm_sql_cte_reader_open(&store,&readers[0],&error);
      const turbodb_value_t *row=NULL; if(status==TURBODB_STATUS_OK) status=pull(0,&row);
      check_equal(status,TURBODB_STATUS_OUT_OF_MEMORY); check_null(row); fail_reserve=fail_resize=0;
      check_equal(store.frontier_readers,0u); check_equal(member.opens,member.closes); clean();
    }
  }
  it("bounds every construction and execution step with independent frontier cursors") {
    prepare_multiple("SELECT n+10 AS n FROM c WHERE n<3"); const uint64_t before=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    open_readers(false,ITERATIONS); check_not_null(next(0)); const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-before;
    for(uint64_t point=0;point<steps;++point) {
      reset(); prepare_multiple("SELECT n+10 AS n FROM c WHERE n<3");
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      turbodb_status_t status=open_store(false,ITERATIONS);
      if(status==TURBODB_STATUS_OK) status=orm_sql_cte_reader_open(&store,&readers[0],&error);
      const turbodb_value_t *row=NULL; if(status==TURBODB_STATUS_OK) status=pull(0,&row);
      check_equal(status,TURBODB_STATUS_LIMIT_EXCEEDED); check_null(row);
      check_equal(store.frontier_readers,0u); check_equal(member.opens,member.closes); clean();
    }
  }
  it("runs a compiled SELECT to convergence and caches one result for independent references") {
    prepare("SELECT n+1 AS n FROM c WHERE n<4"); open_readers(false,ITERATIONS);
    check_equal(anchor.calls,0u); check_equal(member.opens,0u); check_true(seed.active);
    check_true(readers[0].source.types[0].nullable);
    for(int64_t i=1;i<=4;++i) check_equal(next(0)[0].data.int64_value,i);
    check_null(next(0)); check_equal(member.opens,4u); check_equal(member.closes,4u); check_equal(store.iterations,4u);
    check_false(store.frontier.active); check_false(member.source.active);
    for(int64_t i=1;i<=4;++i) check_equal(next(1)[0].data.int64_value,i);
    check_null(next(1)); check_equal(member.opens,4u); check_equal(anchor.calls,2u);
    check_equal(orm_sql_cte_reader_open(&store,&readers[2],&error),TURBODB_STATUS_OK); check_equal(next(2)[0].data.int64_value,1);
  }
  it("limits each recursive member to the previous multirow delta") {
    anchor.count=2; anchor.values[1][0]=turbodb_i64(10);
    prepare("SELECT n+1 AS n FROM c WHERE n<3 OR (n>=10 AND n<12)"); open_readers(false,ITERATIONS);
    const int64_t expected[]={1,10,2,11,3,12};
    for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) check_equal(next(0)[0].data.int64_value,expected[i]);
    check_null(next(0)); check_equal(store.iterations,3u); check_equal(member.opens,member.closes);
  }
  it("produces a Fibonacci tuple with values from the previous row only") {
    seed.columns=COLUMNS; anchor.values[0][1]=turbodb_i64(0); anchor.values[0][2]=turbodb_i64(1);
    prepare("SELECT n+1 AS n,q AS p,p+q AS q FROM c WHERE n<7"); open_readers(false,ITERATIONS);
    const int64_t fibonacci[]={0,1,1,2,3,5,8};
    for(size_t i=0;i<sizeof(fibonacci)/sizeof(fibonacci[0]);++i) {
      const turbodb_value_t *row=next(0); check_equal(row[0].data.int64_value,(int64_t)i+1); check_equal(row[1].data.int64_value,fibonacci[i]);
    }
    check_null(next(0)); check_equal(store.iterations,7u);
  }
  it("does not invoke the recursive member for an empty seed") {
    anchor.count=0; prepare("SELECT n+1 AS n FROM c"); open_readers(false,ITERATIONS);
    check_null(next(0)); check_null(next(1)); check_equal(member.opens,0u); check_equal(member.closes,0u);
    check_equal(store.state,ORM_SQL_CTE_READY); check_true(readers[0].source.types[0].nullable);
  }
  it("preserves duplicate seed rows in UNION ALL") {
    anchor.count=2; anchor.values[1][0]=turbodb_i64(1);
    prepare("SELECT n+1 AS n FROM c WHERE n<2"); open_readers(false,ITERATIONS);
    const int64_t expected[]={1,1,2,2};
    for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) check_equal(next(0)[0].data.int64_value,expected[i]);
    check_null(next(0)); check_equal(store.iterations,2u);
  }
  it("deduplicates seed and repeated recursive rows to terminate a cycle") {
    anchor.count=3; anchor.values[1][0]=turbodb_i64(2); anchor.values[2][0]=turbodb_i64(1);
    prepare("SELECT CASE WHEN n=1 THEN 2 ELSE 1 END AS n FROM c"); open_readers(true,ITERATIONS);
    check_equal(next(0)[0].data.int64_value,1); check_equal(next(0)[0].data.int64_value,2); check_null(next(0));
    check_equal(store.iterations,1u); check_equal(vec_size(&store.rows.snapshots),2u);
  }
  it("deduplicates newly converging rows within a round and checks earlier rounds") {
    anchor.count=2; anchor.values[1][0]=turbodb_i64(2);
    prepare("SELECT CASE WHEN n<3 THEN 3 ELSE 1 END AS n FROM c"); open_readers(true,ITERATIONS);
    for(int64_t i=1;i<=3;++i) check_equal(next(0)[0].data.int64_value,i);
    check_null(next(0)); check_equal(store.iterations,2u); check_equal(vec_size(&store.rows.snapshots),3u);
  }
  it("treats NULL tuples as duplicates and permits nullable recursive output") {
    prepare("SELECT NULL AS n FROM c"); open_readers(true,ITERATIONS);
    check_equal(next(0)[0].data.int64_value,1); check_equal(next(0)[0].kind,TURBODB_VALUE_NULL); check_null(next(0));
    check_equal(store.iterations,2u); check_equal(readers[0].source.types[0].kind,TURBODB_VALUE_INT64);
  }
  it("accepts an identity frontier as a recursive member and terminates DISTINCT") {
    prepare("SELECT n FROM c"); member.identity=true; open_readers(true,ITERATIONS);
    check_equal(next(0)[0].data.int64_value,1); check_null(next(0)); check_equal(store.iterations,1u);
    check_equal(member.opens,1u); check_equal(member.closes,1u); check_false(store.frontier.active);
  }
  it("retains owned TEXT and BLOB payloads across recursive run teardown") {
    seed.columns=COLUMNS; seed_types[1]=(orm_sql_type){TURBODB_VALUE_TEXT,false}; seed_types[2]=(orm_sql_type){TURBODB_VALUE_BLOB,false};
    memcpy(anchor.payload,"abc",sizeof(anchor.payload)); anchor.poison_payload=true;
    anchor.values[0][1]=turbodb_text(anchor.payload); anchor.values[0][2]=turbodb_blob(anchor.payload,sizeof(anchor.payload)-1);
    prepare("SELECT n+1 AS n,p,q FROM c WHERE n<3"); open_readers(false,ITERATIONS);
    for(int64_t i=1;i<=3;++i) {
      const turbodb_value_t *row=next(0); check_equal(row[0].data.int64_value,i);
      check_equal(row[1].data.text_value.len,sizeof(anchor.payload)-1); check_equal(memcmp(row[1].data.text_value.data,"abc",sizeof(anchor.payload)-1),0);
      check_equal(row[2].data.blob_value.size,sizeof(anchor.payload)-1); check_equal(memcmp(row[2].data.blob_value.data,"abc",sizeof(anchor.payload)-1),0);
    }
    check_null(next(0)); check_equal(member.opens,member.closes); check_null(member.run.program);
    check_equal(memcmp(anchor.payload,"xxx",sizeof(anchor.payload)-1),0);
    check_equal(memcmp(next(1)[1].data.text_value.data,"abc",sizeof(anchor.payload)-1),0);
  }
  it("deduplicates signed floating zero and BOOL values using the existing value order") {
    for(size_t pass=0;pass<2;++pass) {
      if(pass) reset(); seed_types[0]=(orm_sql_type){pass?TURBODB_VALUE_BOOLEAN:TURBODB_VALUE_DOUBLE,false};
      anchor.count=2; anchor.values[0][0]=pass?turbodb_bool(true):turbodb_f64(-0.0); anchor.values[1][0]=pass?turbodb_bool(true):turbodb_f64(0.0);
      prepare("SELECT n FROM c"); open_readers(true,ITERATIONS);
      check_not_null(next(0)); check_null(next(0)); check_equal(vec_size(&store.rows.snapshots),1u); check_equal(store.iterations,1u);
    }
  }
  it("compares complete tuples rather than only the first column") {
    seed.columns=2; anchor.count=2; anchor.values[0][1]=turbodb_i64(1); anchor.values[1][0]=turbodb_i64(1); anchor.values[1][1]=turbodb_i64(2);
    prepare("SELECT n,p+1 AS p FROM c WHERE p<3"); open_readers(true,ITERATIONS);
    for(int64_t i=1;i<=3;++i) { const turbodb_value_t *row=next(0); check_equal(row[0].data.int64_value,1); check_equal(row[1].data.int64_value,i); }
    check_null(next(0)); check_equal(store.iterations,2u);
  }
  it("bounds iterations without publishing a prefix and preserves the first error") {
    prepare("SELECT n+1 AS n FROM c"); open_readers(false,2);
    const turbodb_value_t *row=anchor.values[0]; check_equal(pull(0,&row),TURBODB_STATUS_LIMIT_EXCEEDED); check_true(row==anchor.values[0]);
    check_contains(error.message,"iteration limit"); check_equal(member.opens,2u); check_equal(member.closes,2u);
    check_equal(vec_size(&store.rows.snapshots),3u); check_equal(store.state,ORM_SQL_CTE_FAILED);
    check_equal(orm_sql_cte_store_cancel(&store,&error),TURBODB_STATUS_OK);
    check_equal(pull(1,&row),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(member.opens,2u);
    check_equal(orm_sql_cte_reader_open(&store,&readers[2],&error),TURBODB_STATUS_LIMIT_EXCEEDED);
  }
  it("counts the final empty iteration at the exact recursion boundary") {
    prepare("SELECT n+1 AS n FROM c WHERE n<2"); open_readers(false,2);
    check_not_null(next(0)); check_equal(store.iterations,2u); check_equal(store.state,ORM_SQL_CTE_READY);
    reset(); prepare("SELECT n+1 AS n FROM c WHERE n<2"); open_readers(false,1);
    const turbodb_value_t *row=NULL; check_equal(pull(0,&row),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(row);
  }
  it("closes a partially opened recursive member and shares its error") {
    prepare("SELECT n+1 AS n FROM c"); member.fail_open=true; open_readers(false,ITERATIONS);
    const turbodb_value_t *row=anchor.values[0]; check_equal(pull(0,&row),TURBODB_STATUS_DATASTORE_ERROR); check_true(row==anchor.values[0]);
    check_contains(error.message,"open failed"); check_equal(member.opens,1u); check_equal(member.closes,1u);
    check_false(store.frontier.active); check_false(store.round_open); check_null(member.run.program);
    check_equal(pull(1,&row),TURBODB_STATUS_DATASTORE_ERROR); check_equal(member.opens,1u);
  }
  it("closes a failing recursive producer without revealing earlier rows") {
    prepare("SELECT n+1 AS n FROM c"); member.fail_pull=3; open_readers(false,ITERATIONS);
    const turbodb_value_t *row=anchor.values[0]; check_equal(pull(0,&row),TURBODB_STATUS_DATASTORE_ERROR); check_true(row==anchor.values[0]);
    check_contains(error.message,"pull failed"); check_equal(member.opens,2u); check_equal(member.closes,2u);
    check_false(store.frontier.active); check_false(store.round_open);
  }
  it("retains a failed cleanup lease until close succeeds without replacing the execution error") {
    prepare("SELECT n+1 AS n FROM c"); member.fail_open=true; member.fail_close=2; open_readers(false,ITERATIONS);
    const turbodb_value_t *row=NULL; check_equal(pull(0,&row),TURBODB_STATUS_DATASTORE_ERROR); check_contains(error.message,"open failed");
    check_true(store.frontier.active); check_true(store.round_open);
    for(size_t i=0;i<2;++i) check_equal(orm_sql_cte_reader_close(&readers[i],&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_sql_cte_store_close(&store,&error),TURBODB_STATUS_BUSY); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    check_true(store.frontier.active); check_true(seed.active);
    check_equal(orm_sql_cte_store_close(&store,&error),TURBODB_STATUS_OK); check_false(seed.active); check_equal(member.closes,3u);
  }
  it("rejects mismatched member schemas and missing outputs before consuming them") {
    for(size_t mode=0;mode<3;++mode) {
      if(mode) reset(); prepare("SELECT n+1 AS n FROM c"); member.wrong_type=mode==0; member.wrong_width=mode==1; member.no_source=mode==2;
      open_readers(false,ITERATIONS); const turbodb_value_t *row=NULL;
      check_equal(pull(0,&row),mode==0?TURBODB_STATUS_TYPE_ERROR:TURBODB_STATUS_SQL_ERROR); check_null(row);
      check_equal(member.pulls,0u); check_equal(member.closes,1u); check_false(store.frontier.active);
    }
  }
  it("does not widen a NULL seed using a typed recursive member") {
    seed_types[0]=(orm_sql_type){TURBODB_VALUE_NULL,true}; anchor.values[0][0]=turbodb_null();
    prepare("SELECT 1 AS n FROM c"); open_readers(false,ITERATIONS);
    check_equal(readers[0].source.types[0].kind,TURBODB_VALUE_NULL); const turbodb_value_t *row=NULL;
    check_equal(pull(0,&row),TURBODB_STATUS_TYPE_ERROR); check_null(row); check_equal(member.closes,1u);
  }
  it("rejects reentry throughout factory open pull and close") {
    prepare("SELECT n+1 AS n FROM c WHERE n<3"); member.reenter=true; open_readers(false,ITERATIONS);
    check_not_null(next(0)); check_equal(store.state,ORM_SQL_CTE_READY); check_equal(member.opens,3u);
    const turbodb_value_t *row=anchor.values[0];
    check_equal(member.frontier->next(member.frontier->context,&row,&error),TURBODB_STATUS_INVALID_STATE); check_true(row==anchor.values[0]);
  }
  it("cancels a pending recursion without opening any recursive member") {
    prepare("SELECT n+1 AS n FROM c"); open_readers(false,ITERATIONS);
    check_equal(orm_sql_cte_store_cancel(&store,&error),TURBODB_STATUS_OK); const turbodb_value_t *row=NULL;
    check_equal(pull(0,&row),TURBODB_STATUS_INVALID_STATE); check_equal(anchor.calls,0u); check_equal(member.opens,0u);
  }
  it("fails on row capacity and closes the active recursive member") {
    prepare("SELECT n+1 AS n FROM c"); open_readers(false,ITERATIONS);
    budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=2;
    const turbodb_value_t *row=NULL; check_equal(pull(0,&row),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(row);
    check_equal(member.opens,2u); check_equal(member.closes,2u); check_false(store.frontier.active);
  }
  it("latches work exhaustion while preparing the next round without publishing seed rows") {
    prepare("SELECT n+1 AS n FROM c WHERE n<3"); open_readers(false,ITERATIONS);
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const turbodb_value_t *row=anchor.values[0]; check_equal(pull(0,&row),TURBODB_STATUS_LIMIT_EXCEEDED); check_true(row==anchor.values[0]);
    check_equal(pull(1,&row),TURBODB_STATUS_LIMIT_EXCEEDED); check_equal(store.state,ORM_SQL_CTE_FAILED);
    check_equal(member.opens,member.closes); budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=WORK;
  }
  it("refunds every intercepted allocation failure through DISTINCT rounds and index rebuilding") {
    prepare("SELECT n+1 AS n FROM c WHERE n<4"); reserves=resizes=0;
    open_readers(true,ITERATIONS); check_not_null(next(0)); const size_t counts[]={reserves,resizes};
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=counts[pass];++point) {
      reset(); prepare("SELECT n+1 AS n FROM c WHERE n<4"); reserves=resizes=0;
      if(pass) fail_resize=point; else fail_reserve=point;
      turbodb_status_t status=open_store(true,ITERATIONS);
      for(size_t i=0;status==TURBODB_STATUS_OK && i<2;++i) status=orm_sql_cte_reader_open(&store,&readers[i],&error);
      const turbodb_value_t *row=anchor.values[0]; if(status==TURBODB_STATUS_OK) status=pull(0,&row);
      check_equal(status,TURBODB_STATUS_OUT_OF_MEMORY); check_true(row==anchor.values[0]);
      fail_reserve=fail_resize=0; check_equal(member.opens,member.closes); clean();
    }
  }
  it("refunds every step boundary through construction and recursive evaluation") {
    prepare("SELECT n+1 AS n FROM c WHERE n<3"); const uint64_t before=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    open_readers(true,ITERATIONS); check_not_null(next(0)); const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-before;
    for(uint64_t point=0;point<steps;++point) {
      reset(); prepare("SELECT n+1 AS n FROM c WHERE n<3");
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      turbodb_status_t status=open_store(true,ITERATIONS);
      for(size_t i=0;status==TURBODB_STATUS_OK && i<2;++i) status=orm_sql_cte_reader_open(&store,&readers[i],&error);
      const turbodb_value_t *row=NULL; if(status==TURBODB_STATUS_OK) status=pull(0,&row);
      check_equal(status,TURBODB_STATUS_LIMIT_EXCEEDED); check_null(row); check_equal(member.opens,member.closes); clean();
    }
  }
  it("validates recursion configuration and rejects DISTINCT byte payloads before reading") {
    prepare("SELECT n FROM c"); orm_sql_cte_recursion spec=recursion(false,0);
    check_equal(orm_sql_cte_store_open_recursive(&seed,&spec,&store,&error),TURBODB_STATUS_INVALID_ARGUMENT); check_false(seed.active);
    spec.max_iterations=ITERATIONS; spec.open=NULL;
    check_equal(orm_sql_cte_store_open_recursive(&seed,&spec,&store,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    seed_types[0]=(orm_sql_type){TURBODB_VALUE_TEXT,false};
    check_equal(open_store(true,ITERATIONS),TURBODB_STATUS_UNSUPPORTED); check_false(seed.active); check_null(store.budget); check_equal(anchor.calls,0u);
  }
}
