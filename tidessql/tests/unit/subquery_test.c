#include "subquery.h"
#include "binding.h"
#include "select.h"
#include <tinytest.h>
#include <math.h>
#include <string.h>

static size_t reserves,resizes,fail_reserve,fail_resize;
static stl_status subquery_test_reserve(vec_t *v,size_t n) { return ++reserves==fail_reserve?STL_OUT_OF_MEMORY:vec_reserve(v,n); }
static stl_status subquery_test_resize(vec_t *v,size_t n) { return ++resizes==fail_resize?STL_OUT_OF_MEMORY:vec_resize(v,n); }
#define vec_reserve subquery_test_reserve
#define vec_resize subquery_test_resize
#include "../../src/work.c"
#include "../../src/rows.c"
#include "../../src/expr.c"
#include "../../src/binding.c"
#include "../../src/scan.c"
#include "../../src/subquery.c"
#include "../../src/join.c"
#include "../../src/from.c"
#include "../../src/from_run.c"
#include "../../src/select.c"
#undef vec_reserve
#undef vec_resize

enum { TEST_ROWS=12,TEST_COLUMNS=2,TEST_LIMIT=65536,TEST_WORK=4*1024*1024,TEST_BYTES=3,TEST_QUERIES=4,TEST_DEPTH=32 };
typedef struct fixture_source {
  turbodb_value_t values[TEST_ROWS][TEST_COLUMNS],buffer[TEST_COLUMNS];
  unsigned char payload[TEST_BYTES];
  size_t count,position,calls,fail_at;
  bool reenter;
} fixture_source;
static fixture_source fixture;
static orm_sql_type types[TEST_COLUMNS];
static orm_sql_row_source source;
static orm_sql_subquery run,other;
static orm_tidesdb_sql_budget budget;
static orm_sql_budget_limits limits;
static turbodb_error_t error;
static sqlparser_document *document;
static sqlparser_id expression_root;
static orm_sql_expr expression,comparison;
static orm_sql_expr_run evaluation,second_evaluation;
static orm_sql_expr_query_binding query_bindings[TEST_QUERIES];
static orm_sql_expr_query_source *query_sources[TEST_QUERIES];
static orm_sql_expr_input value_bindings[TEST_QUERIES];
static size_t query_count,value_count;
enum { BOUND_FILTER, BOUND_OUTPUT, BOUND_ORDER, BOUND_COUNT };
typedef struct bound_expression {
  orm_sql_expr program;
  vec_t slots,queries;
  size_t slot_bytes,query_bytes;
} bound_expression;
static bound_expression bound[BOUND_COUNT];
static vec_t parameter_offsets;
static size_t parameter_offset_bytes;
static orm_sql_scan outer_scan;
static const orm_sql_type outer_type={TURBODB_VALUE_INT64,true};
static const size_t outer_projection[]={0};
static orm_sql_select selected_plan;
static orm_sql_select_run selected_run;
static orm_sql_from from_plan;
static orm_sql_from_run from_execution;
typedef struct outer_input {
  turbodb_value_t values[TEST_ROWS];
  size_t count,position,calls;
} outer_input;
static outer_input outer_rows,right_rows;
static orm_sql_row_source outer_source,right_source;
static const orm_sql_schema_column outer_column={{"id",2},{TURBODB_VALUE_INT64,true},{0}};
static const orm_sql_table_schema outer_schema={{"t",1},&outer_column,1},right_schema={{"u",1},&outer_column,1};
static turbodb_status_t outer_next(void *context,const turbodb_value_t **out,turbodb_error_t *e) {
  outer_input *input=context; ++input->calls;
  if(input->position==input->count) { *out=NULL; return TURBODB_STATUS_OK; }
  orm_sql_budget_amount amount={0}; amount.value[ORM_SQL_BUDGET_READ_ROWS]=1;
  amount.value[ORM_SQL_BUDGET_READ_BYTES]=sizeof(turbodb_value_t);
  const turbodb_status_t status=orm_tidesdb_sql_budget_reserve(&budget,&amount,e);
  if(status==TURBODB_STATUS_OK) *out=&input->values[input->position++];
  return status;
}
static orm_sql_query_scope query_scope(const orm_sql_type *parameters,size_t count) {
  return (orm_sql_query_scope){document,sqlparser_statements(document).first,parameters,count,TEST_DEPTH,
    &budget,query_bindings,query_count};
}
static turbodb_status_t bind_selected(const orm_sql_type *parameters,size_t count) {
  const orm_sql_query_scope scope=query_scope(parameters,count);
  const turbodb_status_t status=orm_tidesdb_sql_select_bind_at(&scope,&outer_schema,NULL,&selected_plan,&error);
  if(status!=TURBODB_STATUS_OK) info("SELECT query binding: %s",error.message);
  return status;
}
static turbodb_status_t open_selected(const turbodb_value_t *parameters,size_t count) {
  const orm_sql_expr_query_sources queries={query_sources,query_count};
  return orm_tidesdb_sql_select_open_source_queries(&selected_plan,&outer_source,parameters,count,&queries,&selected_run,&error);
}
static orm_sql_scan_row selected_next(void) {
  orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_scan_next(&selected_run.scan,&row,&error),TURBODB_STATUS_OK); return row;
}
static turbodb_status_t bind_joined(void) {
  const orm_sql_query_scope scope=query_scope(NULL,0);
  const orm_sql_table_schema *schemas[]={&outer_schema,&right_schema};
  turbodb_status_t status=orm_tidesdb_sql_from_bind_at(&scope,schemas,2,&from_plan,&error);
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_select_bind_at(&scope,
      &orm_tidesdb_sql_from_at(&from_plan,0)->schema,&from_plan,&selected_plan,&error);
  return status;
}
static turbodb_status_t open_joined(void) {
  const orm_sql_expr_query_sources queries={query_sources,query_count};
  orm_sql_row_source *sources[]={&outer_source,&right_source};
  turbodb_status_t status=orm_tidesdb_sql_from_open_queries(&from_plan,sources,2,NULL,0,&queries,&from_execution,&error);
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_select_open_source_queries(&selected_plan,
      from_execution.source,NULL,0,&queries,&selected_run,&error);
  return status;
}
static void release_bound(bound_expression *entry) {
  check_equal(orm_tidesdb_sql_expr_destroy(&entry->program,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_work_release(&entry->slots,entry->slot_bytes,&budget,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_work_release(&entry->queries,entry->query_bytes,&budget,&error),TURBODB_STATUS_OK);
  *entry=(bound_expression){0};
}
static turbodb_status_t bind_node(size_t index,sqlparser_id root,bool predicate,const orm_sql_type *parameters,size_t count) {
  const orm_sql_schema_column column={{"id",2},{TURBODB_VALUE_INT64,true},{0}};
  const orm_sql_table_schema schema={{"t",1},&column,1};
  const orm_sql_binding_scope scope={.document=document,.schema=&schema,.qualifier={"t",1},
    .parameter_types=parameters,.parameter_count=count,.parameter_offsets=vec_data_const(&parameter_offsets),
    .budget=&budget,.queries=query_bindings,.query_count=query_count};
  bound_expression *entry=&bound[index];
  return orm_sql_bind_expression(&scope,root,TEST_DEPTH,
      (orm_sql_expression_target){&entry->program,&entry->slots,&entry->slot_bytes,&entry->queries,&entry->query_bytes},predicate,&error);
}
static orm_sql_scan_expression bound_scan_expression(size_t index) {
  const bound_expression *entry=&bound[index];
  return (orm_sql_scan_expression){entry->program.budget?&bound[index].program:NULL,
      vec_data_const(&entry->slots),vec_size(&entry->slots),vec_data_const(&entry->queries),vec_size(&entry->queries)};
}
static orm_sql_scan_spec bound_scan_spec(void) {
  const bound_expression *entry=&bound[BOUND_FILTER];
  return (orm_sql_scan_spec){.projection=outer_projection,.projection_count=1,.limit=UINT64_MAX,
      .filter=entry->program.budget?&bound[BOUND_FILTER].program:NULL,
      .filter_slots=vec_data_const(&entry->slots),.filter_count=vec_size(&entry->slots),
      .filter_query_slots=vec_data_const(&entry->queries),.filter_query_count=vec_size(&entry->queries),
      .queries=query_sources,.query_count=query_count};
}
static turbodb_status_t compile_expression(void) {
  const orm_sql_expr_bindings bindings={value_bindings,value_count,query_bindings,query_count};
  return orm_tidesdb_sql_expr_compile_queries(document,expression_root,&bindings,false,TEST_DEPTH,&budget,&expression,&error);
}
static void parse_expression(const char *sql) {
  sqlparser_document_destroy(document); document=NULL; query_count=value_count=0;
  sqlparser_error parse_error;
  check_equal(sqlparser_parse(sql,strlen(sql),NULL,&document,&parse_error),SQLPARSER_OK);
  const sqlparser_node *select=sqlparser_get_node(document,sqlparser_statements(document).first);
  expression_root=select->kind==SQLPARSER_SELECT ?
      sqlparser_get_node(document,select->as.select.columns.first)->as.projection.expression : 0;
  for(size_t i=1;i<=sqlparser_node_count(document);++i) {
    const sqlparser_node *node=sqlparser_get_node(document,(sqlparser_id)i);
    if(node->kind==SQLPARSER_SUBQUERY || (node->kind==SQLPARSER_IN && node->as.in.query) ||
        (node->kind==SQLPARSER_UNARY && node->as.unary.op==SQLPARSER_OP_EXISTS)) {
      check_less(query_count,(size_t)TEST_QUERIES);
      query_sources[query_count]=orm_tidesdb_sql_subquery_source(&run);
      query_bindings[query_count++]=(orm_sql_expr_query_binding){(sqlparser_id)i,run.source.type};
    } else if(node->kind==SQLPARSER_PARAMETER) {
      check_less(value_count,(size_t)TEST_QUERIES);
      value_bindings[value_count++]=(orm_sql_expr_input){(sqlparser_id)i,{TURBODB_VALUE_INT64,true},false};
    }
  }
}
static void open_expression(void) {
  check_equal(compile_expression(),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_expr_run_open_queries(&expression,query_sources,query_count,&evaluation,&error),TURBODB_STATUS_OK);
  sqlparser_document_destroy(document); document=NULL;
}
static turbodb_status_t eval_expression(const turbodb_value_t *inputs,turbodb_value_t *out) {
  return orm_tidesdb_sql_expr_run_eval(&evaluation,inputs,value_count,out,&error);
}
static void cleared_registers(void) {
  const turbodb_value_t zero={0};
  for(size_t i=0;i<vec_size(&evaluation.registers);++i)
    check_equal(memcmp(vec_at_const(&evaluation.registers,i),&zero,sizeof(zero)),0);
}

static turbodb_status_t input_next(void *context,const turbodb_value_t **out,turbodb_error_t *e) {
  fixture_source *input=context; ++input->calls;
  if(input->reenter) {
    turbodb_value_t value=turbodb_i64(99);
    check_equal(run.source.eval(run.source.context,NULL,NULL,NULL,0,&value,NULL),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_subquery_eval(&run,NULL,&value,NULL),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_subquery_cancel(&run,NULL),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_subquery_close(&run,NULL),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_expr_run_eval(&evaluation,NULL,0,&value,NULL),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_expr_run_close(&evaluation,NULL),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_expr_destroy(&expression,NULL),TURBODB_STATUS_BUSY);
    check_equal(value.data.int64_value,99);
  }
  if(input->calls==input->fail_at) { tdsql_error_set(e,TURBODB_STATUS_DATASTORE_ERROR,"subquery input failure"); return TURBODB_STATUS_DATASTORE_ERROR; }
  memset(input->payload,'!',sizeof(input->payload)); memset(input->buffer,0,sizeof(input->buffer));
  if(input->position==input->count) { *out=NULL; return TURBODB_STATUS_OK; }
  orm_sql_budget_amount amount={0}; amount.value[ORM_SQL_BUDGET_READ_ROWS]=1;
  amount.value[ORM_SQL_BUDGET_READ_BYTES]=sizeof(input->buffer);
  const turbodb_status_t status=orm_tidesdb_sql_budget_reserve(&budget,&amount,e);
  if(status!=TURBODB_STATUS_OK) return status;
  memcpy(input->buffer,input->values[input->position],sizeof(input->buffer));
  input->payload[0]=(unsigned char)('a'+input->position++); input->payload[1]=0; input->payload[2]='z';
  for(size_t i=0;i<TEST_COLUMNS;++i) {
    if(input->buffer[i].kind==TURBODB_VALUE_TEXT) input->buffer[i]=turbodb_text_v((vstr){(const char *)input->payload,TEST_BYTES});
    if(input->buffer[i].kind==TURBODB_VALUE_BLOB) input->buffer[i]=turbodb_blob(input->payload,TEST_BYTES);
  }
  *out=input->buffer; return TURBODB_STATUS_OK;
}
static turbodb_status_t open_query(orm_sql_subquery_kind kind, const orm_sql_type *probe) {
  return orm_tidesdb_sql_subquery_open(&source,kind,probe,&run,&error);
}
static turbodb_value_t evaluate(const turbodb_value_t *probe) {
  turbodb_value_t value=turbodb_i64(99); check_equal(orm_tidesdb_sql_subquery_eval(&run,probe,&value,&error),TURBODB_STATUS_OK); return value;
}
static void clean(void) {
  fail_reserve=fail_resize=0;
  check_equal(orm_tidesdb_sql_select_close(&selected_run,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_select_destroy(&selected_plan,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_from_close(&from_execution,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_from_destroy(&from_plan,&error),TURBODB_STATUS_OK);
  check_false(outer_source.active); check_false(right_source.active);
  check_equal(orm_tidesdb_sql_scan_close(&outer_scan,&error),TURBODB_STATUS_OK);
  for(size_t i=0;i<BOUND_COUNT;++i) release_bound(&bound[i]);
  check_equal(orm_sql_work_release(&parameter_offsets,parameter_offset_bytes,&budget,&error),TURBODB_STATUS_OK);
  parameter_offset_bytes=0;
  check_equal(orm_tidesdb_sql_expr_run_close(&second_evaluation,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_expr_run_close(&evaluation,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_expr_destroy(&comparison,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_expr_destroy(&expression,&error),TURBODB_STATUS_OK);
  sqlparser_document_destroy(document); document=NULL;
  check_equal(orm_tidesdb_sql_subquery_close(&other,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_subquery_close(&run,&error),TURBODB_STATUS_OK); check_false(source.active);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
  check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
}
static void reset(void) {
  clean(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_init(&budget,&limits,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
  fixture.position=fixture.calls=0; reserves=resizes=0;
}
static void locked(turbodb_status_t status,const turbodb_value_t *probe) {
  const size_t calls=fixture.calls;
  const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
  turbodb_value_t value=turbodb_i64(99);
  check_equal(orm_tidesdb_sql_subquery_eval(&run,probe,&value,&error),status); check_equal(value.data.int64_value,99);
  check_equal(run.state,ORM_SQL_SUBQUERY_FAILED);
  check_equal(orm_tidesdb_sql_subquery_cancel(&run,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_subquery_eval(&run,probe,&value,NULL),status); check_equal(value.data.int64_value,99);
  check_equal(fixture.calls,calls); check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
  check_true(source.active);
}
typedef struct query_callback_fixture {
  turbodb_value_t value;
  turbodb_status_t status;
  size_t calls;
  turbodb_value_t outer_first;
  size_t outer_count;
  bool return_outer;
  bool reenter_scan;
  bool reenter_select;
  bool reenter_from;
} query_callback_fixture;
static turbodb_status_t query_callback(void *context,const turbodb_value_t *probe,const orm_sql_predicate *predicate,
    const turbodb_value_t *outer_row,size_t outer_count,turbodb_value_t *out,turbodb_error_t *e) {
  query_callback_fixture *callback=context; ++callback->calls; check_null(probe); check_null(predicate);
  callback->outer_count=outer_count;
  callback->outer_first=outer_count?outer_row[0]:turbodb_null();
  if(callback->reenter_scan) {
    orm_sql_scan_row row={.state=ORM_SQL_SCAN_CANCELLED,.count=TEST_ROWS};
    check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,NULL),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_scan_cancel(&outer_scan,NULL),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_scan_close(&outer_scan,NULL),TURBODB_STATUS_BUSY);
    check_equal(row.state,ORM_SQL_SCAN_CANCELLED); check_equal(row.count,(size_t)TEST_ROWS);
  }
  if(callback->reenter_select) {
    check_equal(orm_tidesdb_sql_select_close(&selected_run,NULL),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_select_destroy(&selected_plan,NULL),TURBODB_STATUS_BUSY);
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_scan_next(&selected_run.scan,&row,NULL),TURBODB_STATUS_BUSY);
  }
  if(callback->reenter_from) {
    check_equal(orm_tidesdb_sql_from_close(&from_execution,NULL),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_from_destroy(&from_plan,NULL),TURBODB_STATUS_BUSY);
    from_run_node *root=vec_at(&from_execution.nodes,0);
    orm_sql_scan_row row={.state=ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_join_next(&root->join,&row,NULL),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_join_close(&root->join,NULL),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_join_cancel(&root->join,NULL),TURBODB_STATUS_BUSY);
    check_equal(row.state,ORM_SQL_SCAN_CANCELLED);
  }
  *out=callback->return_outer?callback->outer_first:callback->value;
  if(callback->status!=TURBODB_STATUS_OK) tdsql_error_set(e,callback->status,"query callback failure");
  return callback->status;
}
spec("TidesDB bounded noncorrelated subquery values") {
  before_each() {
    reserves=resizes=fail_reserve=fail_resize=0; tdsql_error_init(&error);
    run=(orm_sql_subquery){0}; other=(orm_sql_subquery){0}; fixture=(fixture_source){.count=3};
    limits=(orm_sql_budget_limits){0};
    for(size_t i=0;i<ORM_SQL_BUDGET_RESOURCE_COUNT;++i) limits.statement.value[i]=TEST_LIMIT;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=TEST_WORK;
    limits.transaction=(orm_sql_transaction_budget_amount){TEST_LIMIT,TEST_LIMIT,TEST_LIMIT};
    check_equal(orm_tidesdb_sql_budget_init(&budget,&limits,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
    for(size_t i=0;i<TEST_COLUMNS;++i) types[i]=(orm_sql_type){TURBODB_VALUE_INT64,true};
    for(size_t i=0;i<TEST_ROWS;++i) { fixture.values[i][0]=turbodb_i64((int64_t)i+1); fixture.values[i][1]=turbodb_i64(10); }
    source=(orm_sql_row_source){&budget,types,1,&fixture,input_next,false};
    outer_rows=(outer_input){.values={turbodb_i64(3),turbodb_i64(1),turbodb_i64(2)},.count=3};
    right_rows=(outer_input){.values={turbodb_i64(2),turbodb_i64(5)},.count=2};
    outer_source=(orm_sql_row_source){&budget,&outer_type,1,&outer_rows,outer_next,false};
    right_source=(orm_sql_row_source){&budget,&outer_type,1,&right_rows,outer_next,false};
  }
  after_each() { clean(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); }
  it("opens lazily and widens empty scalar results even for a NOT NULL column") {
    fixture.count=0; types[0].nullable=false;
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK); check_equal(fixture.calls,0u);
    check_true(source.active); check_equal(run.state,ORM_SQL_SUBQUERY_PENDING);
    check_equal(run.result.kind,TURBODB_VALUE_INT64); check_true(run.result.nullable);
    check_equal(evaluate(NULL).kind,TURBODB_VALUE_NULL); check_equal(fixture.calls,1u);
    check_equal(evaluate(NULL).kind,TURBODB_VALUE_NULL); check_equal(fixture.calls,1u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
  }
  it("copies the single scalar before EOF invalidates its borrowed bytes") {
    fixture.count=1; types[0]=(orm_sql_type){TURBODB_VALUE_TEXT,false}; fixture.values[0][0]=turbodb_text("placeholder");
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    turbodb_value_t value=evaluate(NULL); check_equal(value.kind,TURBODB_VALUE_TEXT); check_equal(value.data.text_value.len,TEST_BYTES);
    check_equal(memcmp(value.data.text_value.data,"a\0z",TEST_BYTES),0); check_equal(fixture.payload[0],'!');
    check_equal(fixture.calls,2u); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],1u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],1u);
    reserves=resizes=0; fail_reserve=fail_resize=1;
    value=evaluate(NULL); check_equal(memcmp(value.data.text_value.data,"a\0z",TEST_BYTES),0);
    check_equal(reserves,0u); check_equal(resizes,0u); check_equal(fixture.calls,2u);
  }
  it("rejects two scalar rows even when equal or NULL and never publishes the first") {
    for(size_t pass=0;pass<2;++pass) {
      fixture.values[0][0]=fixture.values[1][0]=pass?turbodb_null():turbodb_i64(7);
      check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
      turbodb_value_t value=turbodb_i64(99);
      check_equal(orm_tidesdb_sql_subquery_eval(&run,NULL,&value,&error),TURBODB_STATUS_SQL_ERROR);
      check_equal(value.data.int64_value,99); check_equal(fixture.calls,2u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],2u); locked(TURBODB_STATUS_SQL_ERROR,NULL); reset();
    }
  }
  it("caches EXISTS and NOT EXISTS after at most one row without materialization") {
    source.columns=TEST_COLUMNS; fixture.values[0][0]=fixture.values[0][1]=turbodb_null();
    for(size_t negated=0;negated<2;++negated) for(size_t empty=0;empty<2;++empty) {
      fixture.count=empty?0:3; fixture.fail_at=2;
      check_equal(open_query(negated?ORM_SQL_SUBQUERY_NOT_EXISTS:ORM_SQL_SUBQUERY_EXISTS,NULL),TURBODB_STATUS_OK);
      check_false(run.result.nullable); check_equal(run.result.kind,TURBODB_VALUE_BOOLEAN);
      turbodb_value_t value=evaluate(NULL); check_equal(value.kind,TURBODB_VALUE_BOOLEAN);
      check_equal(value.data.boolean_value,negated?empty:!empty);
      check_equal(evaluate(NULL).data.boolean_value,value.data.boolean_value); check_equal(fixture.calls,1u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u); reset();
    }
  }
  it("implements IN and NOT IN empty NULL unmatched and matched truth tables") {
    const turbodb_value_t probes[]={turbodb_null(),turbodb_i64(2),turbodb_i64(9)};
    const orm_sql_type probe={TURBODB_VALUE_INT64,true};
    for(size_t negated=0;negated<2;++negated) for(size_t shape=0;shape<3;++shape) {
      fixture.count=shape?3:0; fixture.values[0][0]=shape==2?turbodb_null():turbodb_i64(1);
      check_equal(open_query(negated?ORM_SQL_SUBQUERY_NOT_IN:ORM_SQL_SUBQUERY_IN,&probe),TURBODB_STATUS_OK);
      for(size_t i=0;i<sizeof(probes)/sizeof(probes[0]);++i) {
        const turbodb_value_t value=evaluate(&probes[i]);
        const bool unknown=shape && (i==0 || (i==2 && shape==2));
        check_equal(value.kind,unknown?TURBODB_VALUE_NULL:TURBODB_VALUE_BOOLEAN);
        if(!unknown) { const bool match=shape && i==1; check_equal(value.data.boolean_value,negated?!match:match); }
        check_equal(fixture.calls,fixture.count+1);
      }
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],fixture.count);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],fixture.count); reset();
    }
  }
  it("keeps I64 U64 comparisons exact above double precision and across the sign boundary") {
    types[0]=(orm_sql_type){TURBODB_VALUE_UINT64,false}; fixture.count=3;
    fixture.values[0][0]=turbodb_u64(UINT64_MAX); fixture.values[1][0]=turbodb_u64(INT64_MAX);
    fixture.values[2][0]=turbodb_u64(UINT64_C(9007199254740993));
    const orm_sql_type probe={TURBODB_VALUE_INT64,false};
    check_equal(open_query(ORM_SQL_SUBQUERY_IN,&probe),TURBODB_STATUS_OK); check_false(run.result.nullable);
    const turbodb_value_t values[]={turbodb_i64(-1),turbodb_i64(INT64_MAX),turbodb_i64(INT64_C(9007199254740992)),turbodb_i64(INT64_C(9007199254740993))};
    for(size_t i=0;i<sizeof(values)/sizeof(values[0]);++i) check_equal(evaluate(&values[i]).data.boolean_value,i==1 || i==3);
    check_equal(fixture.calls,4u);
  }
  it("compares scalar query identity by result type without an unused membership descriptor") {
    fixture.count=1;check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT (SELECT 7)");check_equal(compile_expression(),TURBODB_STATUS_OK);
    const orm_sql_expr_bindings bindings={value_bindings,value_count,query_bindings,query_count};
    check_equal(orm_tidesdb_sql_expr_compile_queries(document,expression_root,&bindings,false,
        TEST_DEPTH,&budget,&comparison,&error),TURBODB_STATUS_OK);
    bool same=false;
    check_equal(orm_tidesdb_sql_expr_same(&expression,NULL,&comparison,NULL,&same,&error),TURBODB_STATUS_OK);check_true(same);
    expr_instruction *instruction=vec_at(&comparison.code,0);check_equal(instruction->opcode,EXPR_QUERY);check_false(instruction->binary);
    instruction->query.comparison=(orm_sql_predicate){.op=ORM_SQL_OR,.real_comparison=true};
    check_equal(orm_tidesdb_sql_expr_same(&expression,NULL,&comparison,NULL,&same,&error),TURBODB_STATUS_OK);check_true(same);
    instruction->query.result_type.kind=TURBODB_VALUE_DOUBLE;
    check_equal(orm_tidesdb_sql_expr_same(&expression,NULL,&comparison,NULL,&same,&error),TURBODB_STATUS_OK);check_false(same);
  }

  it("uses real membership comparisons without changing the retained source kinds or rereading") {
    for(size_t reverse=0;reverse<2;++reverse) {
      types[0]=(orm_sql_type){reverse?TURBODB_VALUE_DOUBLE:TURBODB_VALUE_UINT64,true};fixture.count=3;
      fixture.values[0][0]=reverse?turbodb_f64(9007199254740992.0):turbodb_u64(UINT64_C(9007199254740993));
      fixture.values[1][0]=turbodb_null();
      fixture.values[2][0]=reverse?turbodb_f64(18446744073709551616.0):turbodb_u64(UINT64_MAX);
      const orm_sql_type probe_type={reverse?TURBODB_VALUE_UINT64:TURBODB_VALUE_DOUBLE,false};
      check_equal(open_query(ORM_SQL_SUBQUERY_IN,&probe_type),TURBODB_STATUS_OK);
      turbodb_value_t probe=reverse?turbodb_u64(UINT64_C(9007199254740993)):turbodb_f64(9007199254740992.0);
      check_true(evaluate(&probe).data.boolean_value);
      check_equal(orm_sql_rows_at(&run.cache,0)->kind,types[0].kind);
      const size_t calls=fixture.calls;reserves=resizes=0;fail_reserve=fail_resize=1;
      probe=reverse?turbodb_u64(UINT64_MAX):turbodb_f64(18446744073709551616.0);check_true(evaluate(&probe).data.boolean_value);
      probe=reverse?turbodb_u64(UINT64_C(9007199254740994)):turbodb_f64(9007199254740994.0);
      check_equal(evaluate(&probe).kind,TURBODB_VALUE_NULL);
      check_equal(fixture.calls,calls);check_equal(reserves,0u);check_equal(resizes,0u);
      fail_reserve=fail_resize=0;reset();
    }
  }

  it("copies TEXT and BLOB membership values including embedded NUL across source reuse") {
    for(size_t blob=0;blob<2;++blob) {
      types[0]=(orm_sql_type){blob?TURBODB_VALUE_BLOB:TURBODB_VALUE_TEXT,false};
      for(size_t i=0;i<fixture.count;++i) fixture.values[i][0]=blob?turbodb_blob("x",1):turbodb_text("x");
      check_equal(open_query(ORM_SQL_SUBQUERY_IN,&types[0]),TURBODB_STATUS_OK);
      turbodb_value_t probe=blob?turbodb_blob("a\0z",TEST_BYTES):turbodb_text_v((vstr){"a\0z",TEST_BYTES});
      check_equal(evaluate(&probe).data.boolean_value,1); check_equal(fixture.payload[0],'!');
      probe=blob?turbodb_blob("c\0z",TEST_BYTES):turbodb_text_v((vstr){"c\0z",TEST_BYTES});
      check_equal(evaluate(&probe).data.boolean_value,1);
      probe=blob?turbodb_blob("a",1):turbodb_text("a"); check_equal(evaluate(&probe).data.boolean_value,0);
      check_equal(fixture.calls,4u); reset();
    }
  }
  it("supports BOOL signed zero and NULL-only sets with the existing value rules") {
    fixture.count=2; types[0]=(orm_sql_type){TURBODB_VALUE_DOUBLE,false};
    fixture.values[0][0]=turbodb_f64(-0.0); fixture.values[1][0]=turbodb_f64(3.5);
    turbodb_value_t probe=turbodb_f64(0.0); check_equal(open_query(ORM_SQL_SUBQUERY_IN,&types[0]),TURBODB_STATUS_OK);
    check_equal(evaluate(&probe).data.boolean_value,1); reset();
    types[0]=(orm_sql_type){TURBODB_VALUE_BOOLEAN,false}; fixture.values[0][0]=turbodb_bool(false); fixture.values[1][0]=turbodb_bool(true);
    probe=turbodb_bool(true); check_equal(open_query(ORM_SQL_SUBQUERY_IN,&types[0]),TURBODB_STATUS_OK);
    check_equal(evaluate(&probe).data.boolean_value,1); reset();
    types[0]=(orm_sql_type){TURBODB_VALUE_NULL,true}; fixture.values[0][0]=fixture.values[1][0]=turbodb_null();
    probe=turbodb_null(); check_equal(open_query(ORM_SQL_SUBQUERY_IN,&types[0]),TURBODB_STATUS_OK);
    check_equal(evaluate(&probe).kind,TURBODB_VALUE_NULL);
  }
  it("validates probes before reading even empty sources and never weakens nullability") {
    fixture.count=0;
    const orm_sql_type declared[]={{TURBODB_VALUE_DOUBLE,false},{TURBODB_VALUE_BOOLEAN,false},{TURBODB_VALUE_TEXT,false},{TURBODB_VALUE_INT64,false}};
    turbodb_value_t bad[]={turbodb_f64(NAN),turbodb_bool(true),turbodb_text_v((vstr){"\xc0\x80",2}),turbodb_null()};
    bad[1].data.boolean_value=2;
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);++i) {
      types[0]=declared[i]; check_equal(open_query(ORM_SQL_SUBQUERY_IN,&declared[i]),TURBODB_STATUS_OK);
      turbodb_value_t value=turbodb_i64(99);
      check_equal(orm_tidesdb_sql_subquery_eval(&run,&bad[i],&value,&error),TURBODB_STATUS_TYPE_ERROR);
      check_equal(value.data.int64_value,99); check_equal(fixture.calls,0u); locked(TURBODB_STATUS_TYPE_ERROR,&bad[i]); reset();
    }
  }
  it("preserves output aliasing and allocates nothing during cached membership probes") {
    check_equal(open_query(ORM_SQL_SUBQUERY_IN,&types[0]),TURBODB_STATUS_OK); turbodb_value_t value=turbodb_i64(2);
    check_equal(orm_tidesdb_sql_subquery_eval(&run,&value,&value,&error),TURBODB_STATUS_OK); check_equal(value.data.boolean_value,1);
    reserves=resizes=0; fail_reserve=fail_resize=1;
    value=turbodb_i64(9); check_equal(orm_tidesdb_sql_subquery_eval(&run,&value,&value,&error),TURBODB_STATUS_OK);
    check_equal(value.data.boolean_value,0); check_equal(reserves,0u); check_equal(resizes,0u);
    check_equal(fixture.calls,4u);
  }
  it("locks source failures before a complete scalar or membership cache is published") {
    const turbodb_value_t probe=turbodb_i64(1);
    for(size_t mode=0;mode<2;++mode) for(size_t point=1;point<=(mode?4:2);++point) {
      fixture.fail_at=point; fixture.count=mode?3:1;
      check_equal(open_query(mode?ORM_SQL_SUBQUERY_IN:ORM_SQL_SUBQUERY_SCALAR,mode?&types[0]:NULL),TURBODB_STATUS_OK);
      turbodb_value_t value=turbodb_i64(99);
      check_equal(orm_tidesdb_sql_subquery_eval(&run,mode?&probe:NULL,&value,&error),TURBODB_STATUS_DATASTORE_ERROR);
      check_equal(value.data.int64_value,99); check_equal(fixture.calls,point);
      locked(TURBODB_STATUS_DATASTORE_ERROR,mode?&probe:NULL); reset();
    }
  }
  it("cancels pending and ready executions without more reads and keeps the lease until close") {
    const turbodb_value_t probe=turbodb_i64(1);
    for(size_t ready=0;ready<2;++ready) {
      check_equal(open_query(ORM_SQL_SUBQUERY_IN,&types[0]),TURBODB_STATUS_OK);
      if(ready) check_equal(evaluate(&probe).data.boolean_value,1);
      const size_t calls=fixture.calls;
      check_equal(orm_tidesdb_sql_subquery_cancel(&run,&error),TURBODB_STATUS_OK);
      check_equal(orm_tidesdb_sql_subquery_cancel(&run,&error),TURBODB_STATUS_OK); check_true(source.active);
      turbodb_value_t value=turbodb_i64(99);
      check_equal(orm_tidesdb_sql_subquery_eval(&run,&probe,&value,&error),TURBODB_STATUS_INVALID_STATE);
      check_equal(value.data.int64_value,99); check_equal(fixture.calls,calls); reset();
    }
  }
  it("rejects invalid shapes types argument combinations and occupied outputs before reading") {
    const orm_sql_type text={TURBODB_VALUE_TEXT,true};
    check_equal(open_query(ORM_SQL_SUBQUERY_IN,&text),TURBODB_STATUS_UNSUPPORTED); check_null(run.budget);
    check_equal(open_query(ORM_SQL_SUBQUERY_IN,NULL),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,&types[0]),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(open_query((orm_sql_subquery_kind)99,NULL),TURBODB_STATUS_INVALID_ARGUMENT);
    source.columns=TEST_COLUMNS;
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_SQL_ERROR);
    check_equal(open_query(ORM_SQL_SUBQUERY_IN,&types[0]),TURBODB_STATUS_SQL_ERROR);
    source.columns=1; types[0]=(orm_sql_type){TURBODB_VALUE_NULL,false};
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_TYPE_ERROR); check_null(run.budget);
    types[0]=(orm_sql_type){TURBODB_VALUE_INT64,true};
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_subquery_open(&source,ORM_SQL_SUBQUERY_EXISTS,NULL,&other,&error),TURBODB_STATUS_BUSY);
    check_null(other.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    turbodb_value_t value=turbodb_i64(99);
    check_equal(orm_tidesdb_sql_subquery_eval(&run,&value,&value,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_tidesdb_sql_subquery_eval(&run,NULL,NULL,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(run.state,ORM_SQL_SUBQUERY_PENDING); check_equal(fixture.calls,0u);
  }
  it("honors materialization capacity for scalar and sets while EXISTS requires no cache rows") {
    fixture.count=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=0;
    turbodb_value_t value=turbodb_i64(99);
    check_equal(orm_tidesdb_sql_subquery_eval(&run,NULL,&value,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(value.data.int64_value,99); locked(TURBODB_STATUS_LIMIT_EXCEEDED,NULL); reset();
    fixture.count=3; const turbodb_value_t probe=turbodb_i64(1);
    check_equal(open_query(ORM_SQL_SUBQUERY_IN,&types[0]),TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=2;
    check_equal(orm_tidesdb_sql_subquery_eval(&run,&probe,&value,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(value.data.int64_value,99); locked(TURBODB_STATUS_LIMIT_EXCEEDED,&probe); reset();
    budget.limits.statement.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS]=0;
    check_equal(open_query(ORM_SQL_SUBQUERY_EXISTS,NULL),TURBODB_STATUS_OK); check_equal(evaluate(NULL).data.boolean_value,1);
  }
  it("rejects invalid scalar source values and locks errors on ready set probes") {
    fixture.count=1; fixture.values[0][0]=turbodb_null(); types[0].nullable=false;
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK); turbodb_value_t value=turbodb_i64(99);
    check_equal(orm_tidesdb_sql_subquery_eval(&run,NULL,&value,&error),TURBODB_STATUS_TYPE_ERROR);
    check_equal(value.data.int64_value,99); locked(TURBODB_STATUS_TYPE_ERROR,NULL); reset();
    fixture.values[0][0]=turbodb_i64(1); check_equal(open_query(ORM_SQL_SUBQUERY_IN,&types[0]),TURBODB_STATUS_OK);
    turbodb_value_t probe=turbodb_i64(1); check_equal(evaluate(&probe).data.boolean_value,1); probe=turbodb_null();
    check_equal(orm_tidesdb_sql_subquery_eval(&run,&probe,&value,&error),TURBODB_STATUS_TYPE_ERROR);
    check_equal(value.data.int64_value,99); locked(TURBODB_STATUS_TYPE_ERROR,&probe);
  }
  it("refunds every intercepted open allocation and leaves failed output empty") {
    for(size_t mode=0;mode<2;++mode) {
      const orm_sql_subquery_kind kind=mode?ORM_SQL_SUBQUERY_IN:ORM_SQL_SUBQUERY_SCALAR;
      reserves=resizes=0; check_equal(open_query(kind,mode?&types[0]:NULL),TURBODB_STATUS_OK);
      const size_t allocations[]={reserves,resizes}; reset();
      for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
        if(pass) fail_resize=point; else fail_reserve=point;
        check_equal(open_query(kind,mode?&types[0]:NULL),TURBODB_STATUS_OUT_OF_MEMORY);
        check_null(run.budget); check_false(source.active); check_equal(fixture.calls,0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u); reset();
      }
    }
  }
  it("refunds every cache allocation failure including registry growth and borrowed payload copies") {
    for(size_t mode=0;mode<4;++mode) {
      const bool membership=(mode%2)!=0,bytes=mode>=2;
      fixture.count=membership?TEST_ROWS:1;
      types[0]=(orm_sql_type){bytes?TURBODB_VALUE_TEXT:TURBODB_VALUE_INT64,true};
      for(size_t i=0;i<fixture.count;++i) fixture.values[i][0]=bytes?turbodb_text("x"):turbodb_i64((int64_t)i+1);
      const turbodb_value_t probe=bytes?turbodb_text_v((vstr){"a\0z",TEST_BYTES}):turbodb_i64(1);
      const orm_sql_subquery_kind kind=membership?ORM_SQL_SUBQUERY_IN:ORM_SQL_SUBQUERY_SCALAR;
      check_equal(open_query(kind,membership?&types[0]:NULL),TURBODB_STATUS_OK); reserves=resizes=0;
      evaluate(membership?&probe:NULL); const size_t allocations[]={reserves,resizes}; reset();
      for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
        check_equal(open_query(kind,membership?&types[0]:NULL),TURBODB_STATUS_OK); reserves=resizes=0;
        if(pass) fail_resize=point; else fail_reserve=point;
        turbodb_value_t value=turbodb_i64(99);
        check_equal(orm_tidesdb_sql_subquery_eval(&run,membership?&probe:NULL,&value,&error),TURBODB_STATUS_OUT_OF_MEMORY);
        check_equal(value.data.int64_value,99); locked(TURBODB_STATUS_OUT_OF_MEMORY,membership?&probe:NULL); reset();
      }
    }
  }
  it("locks every first-evaluation step boundary without publishing a partial result") {
    const turbodb_value_t probe=turbodb_i64(9);
    for(size_t mode=0;mode<3;++mode) {
      const orm_sql_subquery_kind kind=mode==0?ORM_SQL_SUBQUERY_SCALAR:mode==1?ORM_SQL_SUBQUERY_EXISTS:ORM_SQL_SUBQUERY_IN;
      fixture.count=mode?3:1; const bool membership=mode==2;
      check_equal(open_query(kind,membership?&types[0]:NULL),TURBODB_STATUS_OK);
      const uint64_t before=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]; evaluate(membership?&probe:NULL);
      const uint64_t needed=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-before; reset();
      for(uint64_t steps=0;steps<needed;++steps) {
        check_equal(open_query(kind,membership?&types[0]:NULL),TURBODB_STATUS_OK);
        budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+steps;
        turbodb_value_t value=turbodb_i64(99);
        check_equal(orm_tidesdb_sql_subquery_eval(&run,membership?&probe:NULL,&value,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
        check_equal(value.data.int64_value,99); locked(TURBODB_STATUS_LIMIT_EXCEEDED,membership?&probe:NULL); reset();
      }
    }
  }
  it("propagates physical read capacity without double charging or retrying the source") {
    fixture.count=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS]=0;
    turbodb_value_t value=turbodb_i64(99);
    check_equal(orm_tidesdb_sql_subquery_eval(&run,NULL,&value,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(value.data.int64_value,99); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
    locked(TURBODB_STATUS_LIMIT_EXCEEDED,NULL); reset();
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_READ_ROWS]=1;
    check_equal(evaluate(NULL).data.int64_value,1); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],1u);
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_subquery_eval(&run,NULL,&value,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(value.data.int64_value,99); locked(TURBODB_STATUS_LIMIT_EXCEEDED,NULL);
  }
  it("skips scalar subqueries in CASE COALESCE and IFNULL without triggering cardinality errors") {
    const char *sql[]={"SELECT CASE WHEN FALSE THEN (SELECT id FROM t) ELSE 7 END",
      "SELECT CASE 1 WHEN 1 THEN 7 ELSE (SELECT id FROM t) END",
      "SELECT COALESCE(7,(SELECT id FROM t))", "SELECT IFNULL(7,(SELECT id FROM t))"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK); parse_expression(sql[i]); open_expression();
      const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
      turbodb_value_t value=turbodb_i64(99); check_equal(eval_expression(NULL,&value),TURBODB_STATUS_OK); check_equal(value.data.int64_value,7);
      check_equal(fixture.calls,0u); check_equal(run.state,ORM_SQL_SUBQUERY_PENDING);
      check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); cleared_registers(); reset();
    }
  }
  it("skips decisive boolean branches but evaluates UNKNOWN AND OR operands lazily") {
    const char *sql[]={"SELECT FALSE AND EXISTS(SELECT id FROM t)","SELECT TRUE OR EXISTS(SELECT id FROM t)",
      "SELECT NULL AND EXISTS(SELECT id FROM t)","SELECT NULL OR EXISTS(SELECT id FROM t)"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      check_equal(open_query(ORM_SQL_SUBQUERY_EXISTS,NULL),TURBODB_STATUS_OK); parse_expression(sql[i]); open_expression();
      turbodb_value_t value=turbodb_i64(99); check_equal(eval_expression(NULL,&value),TURBODB_STATUS_OK);
      check_equal(fixture.calls,i<2?0u:1u);
      if(i==2) check_equal(value.kind,TURBODB_VALUE_NULL); else check_equal(value.data.boolean_value,i!=0);
      cleared_registers(); reset();
    }
  }
  it("defers scalar errors until a parameter selects the branch and retains the first source failure") {
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT CASE WHEN ? THEN (SELECT id FROM t) ELSE 7 END"); value_bindings[0].type=(orm_sql_type){TURBODB_VALUE_BOOLEAN,false};
    open_expression(); turbodb_value_t input=turbodb_bool(false),value=turbodb_i64(99);
    check_equal(eval_expression(&input,&value),TURBODB_STATUS_OK); check_equal(value.data.int64_value,7); check_equal(fixture.calls,0u);
    input=turbodb_bool(true); value=turbodb_i64(99);
    check_equal(eval_expression(&input,&value),TURBODB_STATUS_SQL_ERROR); check_equal(value.data.int64_value,99); check_equal(fixture.calls,2u);
    check_not_null(strstr(error.message,"at byte")); cleared_registers();
    check_equal(eval_expression(&input,&value),TURBODB_STATUS_SQL_ERROR); check_equal(fixture.calls,2u); check_equal(value.data.int64_value,99);
  }
  it("falls through empty scalar COALESCE and retains owned TEXT after AST destruction") {
    fixture.count=0; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT COALESCE((SELECT id FROM t),7)"); open_expression(); turbodb_value_t value=turbodb_i64(99);
    check_equal(eval_expression(NULL,&value),TURBODB_STATUS_OK); check_equal(value.data.int64_value,7); reset();
    fixture.count=1; fixture.values[0][0]=turbodb_text("x"); types[0]=(orm_sql_type){TURBODB_VALUE_TEXT,false};
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT COALESCE((SELECT label FROM t),'fallback')"); open_expression();
    check_equal(eval_expression(NULL,&value),TURBODB_STATUS_OK); check_equal(value.kind,TURBODB_VALUE_TEXT);
    check_equal(memcmp(value.data.text_value.data,"a\0z",TEST_BYTES),0); cleared_registers();
    check_equal(orm_tidesdb_sql_expr_run_close(&evaluation,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_expr_destroy(&expression,&error),TURBODB_STATUS_OK);
    check_equal(memcmp(value.data.text_value.data,"a\0z",TEST_BYTES),0);
  }
  it("compiles IN probe arithmetic once and preserves NOT IN NULL results") {
    for(size_t negated=0;negated<2;++negated) {
      const orm_sql_type probe={TURBODB_VALUE_INT64,true}; fixture.values[0][0]=turbodb_null();
      check_equal(open_query(negated?ORM_SQL_SUBQUERY_NOT_IN:ORM_SQL_SUBQUERY_IN,&probe),TURBODB_STATUS_OK);
      parse_expression(negated?"SELECT ?+1 NOT IN (SELECT id FROM t)":"SELECT ?+1 IN (SELECT id FROM t)"); open_expression();
      turbodb_value_t input=turbodb_i64(1),value=turbodb_i64(99);
      check_equal(eval_expression(&input,&value),TURBODB_STATUS_OK); check_equal(value.data.boolean_value,!negated);
      input=turbodb_i64(8); check_equal(eval_expression(&input,&value),TURBODB_STATUS_OK); check_equal(value.kind,TURBODB_VALUE_NULL);
      check_equal(fixture.calls,4u); cleared_registers(); reset();
    }
  }
  it("observes SELECT cardinality without leasing projection or ordering queries") {
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT (SELECT id FROM t) AS n FROM t ORDER BY (SELECT id FROM t)");
    check_equal(bind_selected(NULL,0),TURBODB_STATUS_OK);
    const orm_sql_expr_query_sources queries={query_sources,query_count};
    check_equal(orm_sql_select_open_cardinality(&selected_plan,&outer_source,NULL,0,&queries,&selected_run,&error),TURBODB_STATUS_OK);
    check_equal(run.source.active_runs,0u); check_equal(selected_next().state,ORM_SQL_SCAN_ROW);
    check_equal(fixture.calls,0u); check_equal(outer_rows.calls,1u);
    check_equal(orm_tidesdb_sql_select_close(&selected_run,&error),TURBODB_STATUS_OK);
    outer_rows.position=outer_rows.calls=0; check_equal(open_selected(NULL,0),TURBODB_STATUS_OK);
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_scan_next(&selected_run.scan,&row,&error),TURBODB_STATUS_SQL_ERROR);
  }
  it("prunes aggregate-only query calls while retaining implicit grouped cardinality") {
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT COUNT((SELECT id FROM t)) AS n FROM t HAVING COUNT(*)>0");
    check_equal(bind_selected(NULL,0),TURBODB_STATUS_OK);
    const orm_sql_expr_query_sources queries={query_sources,query_count};
    check_equal(orm_sql_select_open_cardinality(&selected_plan,&outer_source,NULL,0,&queries,&selected_run,&error),TURBODB_STATUS_OK);
    check_equal(run.source.active_runs,0u); check_equal(selected_next().state,ORM_SQL_SCAN_ROW);
    check_equal(selected_next().state,ORM_SQL_SCAN_DONE); check_equal(fixture.calls,0u);
  }
  it("retains query dependencies used by HAVING in cardinality mode") {
    fixture.count=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT MIN((SELECT id FROM t)) AS n FROM t HAVING n>0");
    check_equal(bind_selected(NULL,0),TURBODB_STATUS_OK);
    const orm_sql_expr_query_sources queries={query_sources,query_count};
    check_equal(orm_sql_select_open_cardinality(&selected_plan,&outer_source,NULL,0,&queries,&selected_run,&error),TURBODB_STATUS_OK);
    check_equal(run.source.active_runs,1u); check_equal(selected_next().state,ORM_SQL_SCAN_ROW);
    check_equal(selected_next().state,ORM_SQL_SCAN_DONE); check_equal(fixture.calls,2u);
    check_equal(orm_tidesdb_sql_select_close(&selected_run,&error),TURBODB_STATUS_OK); check_equal(run.source.active_runs,0u);
  }
  it("shares a compiled IN set across signed unsigned and nullable probe types") {
    check_equal(orm_sql_subquery_open_set(&source,ORM_SQL_SUBQUERY_IN,&run,&error),TURBODB_STATUS_OK);
    turbodb_value_t value=turbodb_i64(99),input=turbodb_i64(2);
    check_equal(orm_tidesdb_sql_subquery_eval(&run,&input,&value,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(fixture.calls,0u);
    parse_expression("SELECT ? IN (SELECT id FROM t)"); open_expression();
    check_equal(eval_expression(&input,&value),TURBODB_STATUS_OK); check_equal(value.data.boolean_value,1);
    check_equal(fixture.calls,4u);
    check_equal(orm_tidesdb_sql_expr_run_close(&evaluation,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_expr_destroy(&expression,&error),TURBODB_STATUS_OK);
    parse_expression("SELECT ? IN (SELECT id FROM t)");
    value_bindings[0].type=(orm_sql_type){TURBODB_VALUE_UINT64,true};
    check_equal(compile_expression(),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_expr_run_open_queries(&expression,query_sources,query_count,&evaluation,&error),TURBODB_STATUS_OK);
    input=turbodb_u64(UINT64_MAX);
    check_equal(eval_expression(&input,&value),TURBODB_STATUS_OK); check_equal(value.data.boolean_value,0);
    input=turbodb_u64(3); check_equal(eval_expression(&input,&value),TURBODB_STATUS_OK); check_equal(value.data.boolean_value,1);
    input=turbodb_null(); check_equal(eval_expression(&input,&value),TURBODB_STATUS_OK); check_equal(value.kind,TURBODB_VALUE_NULL);
    check_equal(fixture.calls,4u);
  }
  it("rejects changed IN source element metadata before acquiring a consumer lease") {
    check_equal(orm_sql_subquery_open_set(&source,ORM_SQL_SUBQUERY_IN,&run,&error),TURBODB_STATUS_OK);
    parse_expression("SELECT ? IN (SELECT id FROM t)"); check_equal(compile_expression(),TURBODB_STATUS_OK);
    const orm_sql_type element=run.source.type.element; run.source.type.element.kind=TURBODB_VALUE_UINT64;
    check_equal(orm_tidesdb_sql_expr_run_open_queries(&expression,query_sources,query_count,&evaluation,&error),TURBODB_STATUS_TYPE_ERROR);
    run.source.type.element=element; check_equal(run.source.active_runs,0u); check_equal(fixture.calls,0u);
    check_equal(orm_tidesdb_sql_expr_run_open_queries(&expression,query_sources,query_count,&evaluation,&error),TURBODB_STATUS_OK);
  }
  it("shares one cache between expression consumers and copies the source pointer array") {
    fixture.count=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT (SELECT id FROM t)+(SELECT id FROM t)"); open_expression();
    check_equal(run.source.active_runs,2u);
    check_equal(orm_tidesdb_sql_expr_run_open_queries(&expression,query_sources,query_count,&second_evaluation,&error),TURBODB_STATUS_OK);
    check_equal(run.source.active_runs,4u); query_sources[0]=query_sources[1]=NULL;
    turbodb_value_t value=turbodb_i64(99);
    check_equal(orm_tidesdb_sql_subquery_eval(&run,NULL,&value,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_subquery_cancel(&run,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_subquery_close(&run,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_expr_destroy(&expression,&error),TURBODB_STATUS_BUSY);
    check_equal(eval_expression(NULL,&value),TURBODB_STATUS_OK); check_equal(value.data.int64_value,2);
    check_equal(orm_tidesdb_sql_expr_run_eval(&second_evaluation,NULL,0,&value,&error),TURBODB_STATUS_OK); check_equal(value.data.int64_value,2);
    check_equal(fixture.calls,2u);
    check_equal(orm_tidesdb_sql_expr_run_close(&evaluation,&error),TURBODB_STATUS_OK); check_equal(run.source.active_runs,2u);
    check_equal(orm_tidesdb_sql_expr_run_close(&second_evaluation,&error),TURBODB_STATUS_OK); check_equal(run.source.active_runs,0u);
    check_equal(orm_tidesdb_sql_subquery_eval(&run,NULL,&value,&error),TURBODB_STATUS_OK);
  }
  it("rejects missing or mismatched query sources even in skipped branches without touching rows") {
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT COALESCE(7,(SELECT id FROM t))"); check_equal(compile_expression(),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_tidesdb_sql_expr_run_open(&expression,&evaluation,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    query_sources[0]=NULL;
    check_equal(orm_tidesdb_sql_expr_run_open_queries(&expression,query_sources,query_count,&evaluation,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    query_sources[0]=&run.source; const orm_sql_expr_query_type original=run.source.type;
    run.source.type.result.kind=TURBODB_VALUE_TEXT;
    check_equal(orm_tidesdb_sql_expr_run_open_queries(&expression,query_sources,query_count,&evaluation,&error),TURBODB_STATUS_TYPE_ERROR);
    run.source.type=original; run.source.type.kind=ORM_SQL_SUBQUERY_EXISTS;
    check_equal(orm_tidesdb_sql_expr_run_open_queries(&expression,query_sources,query_count,&evaluation,&error),TURBODB_STATUS_TYPE_ERROR);
    run.source.type=original;
    check_equal(run.source.active_runs,0u); check_equal(fixture.calls,0u); check_null(evaluation.program);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("rejects malformed declarations and bindings outside the outer expression") {
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT (SELECT id FROM t)"); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    query_bindings[0].type.result.nullable=false;
    check_equal(compile_expression(),TURBODB_STATUS_TYPE_ERROR); query_bindings[0].type=run.source.type;
    query_bindings[0].type.kind=ORM_SQL_SUBQUERY_EXISTS;
    check_equal(compile_expression(),TURBODB_STATUS_INVALID_ARGUMENT); query_bindings[0].type=run.source.type;
    const sqlparser_id original=query_bindings[0].node; query_bindings[0].node=sqlparser_statements(document).first;
    check_equal(compile_expression(),TURBODB_STATUS_INVALID_ARGUMENT); query_bindings[0].node=original;
    query_bindings[1]=query_bindings[0]; query_count=2;
    check_equal(compile_expression(),TURBODB_STATUS_INVALID_ARGUMENT); query_count=1;
    check_equal(orm_tidesdb_sql_expr_compile_value(document,expression_root,NULL,0,TEST_DEPTH,&budget,&expression,&error),TURBODB_STATUS_UNSUPPORTED);
    parse_expression("SELECT 7+(SELECT id FROM t)");
    expression_root=sqlparser_get_node(document,expression_root)->as.binary.left;
    check_equal(compile_expression(),TURBODB_STATUS_INVALID_ARGUMENT); check_null(expression.budget);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_equal(fixture.calls,0u);
  }
  it("rejects probe type mismatches and supports an explicit NOT EXISTS binding") {
    const orm_sql_type probe={TURBODB_VALUE_INT64,false};
    check_equal(open_query(ORM_SQL_SUBQUERY_IN,&probe),TURBODB_STATUS_OK); parse_expression("SELECT ? IN (SELECT id FROM t)");
    value_bindings[0].type=(orm_sql_type){TURBODB_VALUE_TEXT,true};
    check_equal(compile_expression(),TURBODB_STATUS_UNSUPPORTED); reset();
    check_equal(open_query(ORM_SQL_SUBQUERY_NOT_EXISTS,NULL),TURBODB_STATUS_OK); parse_expression("SELECT NOT EXISTS(SELECT id FROM t)");
    query_bindings[0].node=expression_root; open_expression(); turbodb_value_t value=turbodb_i64(99);
    check_equal(eval_expression(NULL,&value),TURBODB_STATUS_OK); check_equal(value.data.boolean_value,0); check_equal(fixture.calls,1u);
  }
  it("refunds every lazy-expression compile and open allocation including shared source leases") {
    fixture.count=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT COALESCE((SELECT id FROM t),(SELECT id FROM t),7)");
    const uint64_t base=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; reserves=resizes=0;
    check_equal(compile_expression(),TURBODB_STATUS_OK); const size_t compilation[]={reserves,resizes};
    check_equal(orm_tidesdb_sql_expr_destroy(&expression,&error),TURBODB_STATUS_OK);
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=compilation[pass];++point) {
      reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(compile_expression(),TURBODB_STATUS_OUT_OF_MEMORY); fail_resize=fail_reserve=0;
      check_null(expression.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],base);
    }
    check_equal(compile_expression(),TURBODB_STATUS_OK); reserves=resizes=0;
    check_equal(orm_tidesdb_sql_expr_run_open_queries(&expression,query_sources,query_count,&evaluation,&error),TURBODB_STATUS_OK);
    const size_t opening[]={reserves,resizes}; check_equal(orm_tidesdb_sql_expr_run_close(&evaluation,&error),TURBODB_STATUS_OK);
    const uint64_t bound_work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=opening[pass];++point) {
      reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(orm_tidesdb_sql_expr_run_open_queries(&expression,query_sources,query_count,&evaluation,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      fail_resize=fail_reserve=0; check_null(evaluation.program); check_equal(run.source.active_runs,0u);
      check_equal(expression.active_runs,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],bound_work);
    }
    run.source.active_runs=SIZE_MAX-1;
    check_equal(orm_tidesdb_sql_expr_run_open_queries(&expression,query_sources,query_count,&evaluation,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(run.source.active_runs,SIZE_MAX-1); run.source.active_runs=0;
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],bound_work); check_equal(fixture.calls,0u);
  }
  it("evaluates a lazy scalar IN probe exactly once per outer evaluation") {
    const orm_sql_type probe={TURBODB_VALUE_INT64,true}; check_equal(open_query(ORM_SQL_SUBQUERY_IN,&probe),TURBODB_STATUS_OK);
    parse_expression("SELECT (SELECT 2) IN (SELECT id FROM t)");
    query_callback_fixture callback={.value=turbodb_i64(2)};
    orm_sql_expr_query_source scalar={&budget,{ORM_SQL_SUBQUERY_SCALAR,{TURBODB_VALUE_INT64,true},{TURBODB_VALUE_NULL,true}},&callback,query_callback,0};
    for(size_t i=0;i<query_count;++i) if(sqlparser_get_node(document,query_bindings[i].node)->kind==SQLPARSER_SUBQUERY) {
      query_bindings[i].type=scalar.type; query_sources[i]=&scalar;
    }
    open_expression(); turbodb_value_t value=turbodb_i64(99);
    check_equal(eval_expression(NULL,&value),TURBODB_STATUS_OK); check_equal(value.data.boolean_value,1); check_equal(callback.calls,1u);
    check_equal(eval_expression(NULL,&value),TURBODB_STATUS_OK); check_equal(value.data.boolean_value,1); check_equal(callback.calls,2u);
    check_equal(fixture.calls,4u); cleared_registers();
    check_equal(orm_tidesdb_sql_expr_run_close(&evaluation,&error),TURBODB_STATUS_OK); check_equal(scalar.active_runs,0u);
  }
  it("validates callback output against its declared schema before publishing registers") {
    fixture.count=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT (SELECT id FROM t)");
    query_callback_fixture callback={.value=turbodb_null()};
    orm_sql_expr_query_source scalar={&budget,{ORM_SQL_SUBQUERY_SCALAR,{TURBODB_VALUE_INT64,false},{TURBODB_VALUE_NULL,true}},&callback,query_callback,0};
    query_sources[0]=&scalar; open_expression(); turbodb_value_t value=turbodb_i64(99);
    check_equal(eval_expression(NULL,&value),TURBODB_STATUS_TYPE_ERROR); check_equal(value.data.int64_value,99); cleared_registers();
    callback.value=turbodb_i64(3); callback.status=TURBODB_STATUS_SQL_ERROR;
    check_equal(eval_expression(NULL,&value),TURBODB_STATUS_SQL_ERROR); check_equal(value.data.int64_value,99); cleared_registers();
    check_not_null(strstr(error.message,"query callback failure"));
    callback.status=TURBODB_STATUS_OK;
    check_equal(eval_expression(NULL,&value),TURBODB_STATUS_OK); check_equal(value.data.int64_value,3); cleared_registers();
    check_equal(fixture.calls,0u);
    check_equal(orm_tidesdb_sql_expr_run_close(&evaluation,&error),TURBODB_STATUS_OK); check_equal(scalar.active_runs,0u);
  }
  it("rejects malformed callback bytes and source budgets without evaluating a row") {
    types[0]=(orm_sql_type){TURBODB_VALUE_TEXT,true}; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT (SELECT label FROM t)"); check_equal(compile_expression(),TURBODB_STATUS_OK);
    query_callback_fixture callback={.value=turbodb_text_v((vstr){"\xc0\x80",2})};
    orm_sql_expr_query_source scalar={NULL,run.source.type,&callback,query_callback,0}; query_sources[0]=&scalar;
    check_equal(orm_tidesdb_sql_expr_run_open_queries(&expression,query_sources,query_count,&evaluation,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    scalar.budget=&budget;
    check_equal(orm_tidesdb_sql_expr_run_open_queries(&expression,query_sources,query_count,&evaluation,&error),TURBODB_STATUS_OK);
    turbodb_value_t value=turbodb_i64(99); check_equal(eval_expression(NULL,&value),TURBODB_STATUS_TYPE_ERROR);
    check_equal(value.data.int64_value,99); cleared_registers(); check_equal(fixture.calls,0u);
    check_equal(orm_tidesdb_sql_expr_run_close(&evaluation,&error),TURBODB_STATUS_OK); check_equal(scalar.active_runs,0u);
  }
  it("guards expression and subquery lifetime against reentry through a row source callback") {
    fixture.count=1; fixture.reenter=true;
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT (SELECT id FROM t)+1"); open_expression(); turbodb_value_t value=turbodb_i64(99);
    check_equal(eval_expression(NULL,&value),TURBODB_STATUS_OK); check_equal(value.data.int64_value,2);
    check_equal(fixture.calls,2u); check_false(run.evaluating); check_false(evaluation.evaluating); cleared_registers();
  }
  it("keeps query identity in expression equality instead of merging different query nodes") {
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT (SELECT id FROM t)+(SELECT id FROM t)"); check_equal(query_count,2u);
    const orm_sql_expr_bindings first={NULL,0,&query_bindings[0],1},second={NULL,0,&query_bindings[1],1};
    check_equal(orm_tidesdb_sql_expr_compile_queries(document,query_bindings[0].node,&first,false,TEST_DEPTH,&budget,&expression,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_expr_compile_queries(document,query_bindings[1].node,&second,false,TEST_DEPTH,&budget,&comparison,&error),TURBODB_STATUS_OK);
    bool same=true; check_equal(orm_tidesdb_sql_expr_same(&expression,NULL,&comparison,NULL,&same,&error),TURBODB_STATUS_OK); check_false(same);
    check_equal(orm_tidesdb_sql_expr_destroy(&comparison,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_expr_compile_queries(document,query_bindings[0].node,&first,false,TEST_DEPTH,&budget,&comparison,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_expr_same(&expression,NULL,&comparison,NULL,&same,&error),TURBODB_STATUS_OK); check_true(same);
    check_equal(fixture.calls,0u);
  }
  it("preserves output and clears registers at every lazy-evaluation step boundary") {
    const char *sql="SELECT COALESCE((SELECT id FROM t),7)+1"; fixture.count=1;
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK); parse_expression(sql); open_expression();
    const uint64_t before=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]; turbodb_value_t value=turbodb_i64(99);
    check_equal(eval_expression(NULL,&value),TURBODB_STATUS_OK); check_equal(value.data.int64_value,2);
    const uint64_t needed=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-before; reset();
    for(uint64_t step=0;step<needed;++step) {
      check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK); parse_expression(sql); open_expression();
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+step;
      value=turbodb_i64(99); check_equal(eval_expression(NULL,&value),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(value.data.int64_value,99); check_false(evaluation.evaluating); cleared_registers(); reset();
    }
  }
  it("binds outer columns and document parameter ordinals without entering query bodies") {
    fixture.count=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT id+(SELECT missing FROM hidden WHERE k=?)+? FROM t WHERE ?=1");
    const orm_sql_type parameter_types[]={outer_type,outer_type,outer_type};
    const turbodb_value_t parameters[]={turbodb_i64(111),turbodb_i64(10),turbodb_i64(1)},rows[]={turbodb_i64(3),turbodb_i64(1)};
    check_equal(orm_sql_bind_parameter_offsets(document,3,&budget,&parameter_offsets,&parameter_offset_bytes,&error),TURBODB_STATUS_OK);
    check_equal(bind_node(BOUND_OUTPUT,expression_root,false,parameter_types,3),TURBODB_STATUS_OK);
    check_equal(vec_size(&bound[BOUND_OUTPUT].slots),2u); check_equal(vec_size(&bound[BOUND_OUTPUT].queries),1u);
    check_equal(*(const size_t *)vec_at_const(&bound[BOUND_OUTPUT].slots,0),0u);
    check_equal(*(const size_t *)vec_at_const(&bound[BOUND_OUTPUT].slots,1),2u);
    orm_sql_scan_expression output=bound_scan_expression(BOUND_OUTPUT); orm_sql_scan_spec spec=bound_scan_spec();
    spec.expressions=&output; spec.parameters=parameters; spec.parameter_types=parameter_types; spec.parameter_count=3;
    const orm_sql_memory_source input={rows,2,1,&outer_type};
    check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_OK);
    check_equal(run.source.active_runs,1u); check_equal(fixture.calls,0u);
    sqlparser_document_destroy(document); document=NULL;
    memset(query_sources,0,sizeof(query_sources)); output=(orm_sql_scan_expression){0};
    check_equal(orm_sql_work_release(&bound[BOUND_OUTPUT].slots,bound[BOUND_OUTPUT].slot_bytes,&budget,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_work_release(&bound[BOUND_OUTPUT].queries,bound[BOUND_OUTPUT].query_bytes,&budget,&error),TURBODB_STATUS_OK);
    bound[BOUND_OUTPUT].slot_bytes=bound[BOUND_OUTPUT].query_bytes=0;
    orm_sql_scan_row row={0};
    check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,&error),TURBODB_STATUS_OK); check_equal(row.values[0].data.int64_value,14);
    check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,&error),TURBODB_STATUS_OK); check_equal(row.values[0].data.int64_value,12);
    check_equal(fixture.calls,2u);
  }
  it("selects only immediate query dependencies from a registry containing nested queries") {
    fixture.count=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT (SELECT (SELECT missing FROM hidden) FROM hidden)+id FROM t");
    check_equal(query_count,2u);
    check_equal(bind_node(BOUND_OUTPUT,expression_root,false,NULL,0),TURBODB_STATUS_OK);
    check_equal(bound[BOUND_OUTPUT].program.query_count,1u); check_equal(vec_size(&bound[BOUND_OUTPUT].slots),1u);
    check_equal(*(const size_t *)vec_at_const(&bound[BOUND_OUTPUT].queries,0),1u);
    orm_sql_scan_expression output=bound_scan_expression(BOUND_OUTPUT); orm_sql_scan_spec spec=bound_scan_spec(); spec.expressions=&output;
    const turbodb_value_t rows[]={turbodb_i64(4)}; const orm_sql_memory_source input={rows,1,1,&outer_type};
    query_sources[0]=NULL;
    check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_OK);
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.values[0].data.int64_value,5); check_equal(fixture.calls,2u);
  }
  it("binds an IN probe in the outer scope and filters with one cached membership set") {
    fixture.count=2; check_equal(open_query(ORM_SQL_SUBQUERY_IN,&outer_type),TURBODB_STATUS_OK);
    parse_expression("SELECT id FROM t WHERE id IN (SELECT missing FROM hidden)");
    const sqlparser_node *select=sqlparser_get_node(document,sqlparser_statements(document).first);
    check_equal(bind_node(BOUND_FILTER,select->as.select.where,true,NULL,0),TURBODB_STATUS_OK);
    const turbodb_value_t rows[]={turbodb_i64(3),turbodb_i64(1),turbodb_i64(2)}; const orm_sql_memory_source input={rows,3,1,&outer_type};
    orm_sql_scan_spec spec=bound_scan_spec(); orm_sql_scan_row row={0};
    check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_OK);
    for(int64_t expected=1;expected<=2;++expected) {
      check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,expected);
    }
    check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
    check_equal(fixture.calls,3u); check_equal(run.source.active_runs,1u);
    check_equal(orm_tidesdb_sql_subquery_close(&run,&error),TURBODB_STATUS_BUSY);
  }
  it("shares a lazy scalar cache across independent filter projection and order mappings") {
    fixture.count=1; fixture.values[0][0]=turbodb_i64(3);
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT id+(SELECT v FROM hidden) FROM t WHERE id<=(SELECT v FROM hidden) ORDER BY id*(SELECT v FROM hidden)");
    const sqlparser_node *select=sqlparser_get_node(document,sqlparser_statements(document).first);
    check_equal(bind_node(BOUND_FILTER,select->as.select.where,true,NULL,0),TURBODB_STATUS_OK);
    check_equal(bind_node(BOUND_OUTPUT,expression_root,false,NULL,0),TURBODB_STATUS_OK);
    check_equal(bind_node(BOUND_ORDER,sqlparser_get_node(document,select->as.select.order_by.first)->as.order.expression,false,NULL,0),TURBODB_STATUS_OK);
    orm_sql_scan_expression output=bound_scan_expression(BOUND_OUTPUT);
    const orm_sql_scan_order order={.expression=bound_scan_expression(BOUND_ORDER)};
    orm_sql_scan_spec spec=bound_scan_spec(); spec.expressions=&output; spec.orders=&order; spec.order_count=1;
    const turbodb_value_t rows[]={turbodb_i64(3),turbodb_i64(1),turbodb_i64(2),turbodb_i64(4)}; const orm_sql_memory_source input={rows,4,1,&outer_type};
    check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_OK);
    check_equal(run.source.active_runs,3u); check_equal(fixture.calls,0u);
    orm_sql_scan_row row={0};
    for(int64_t expected=4;expected<=6;++expected) {
      check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,expected);
    }
    check_equal(fixture.calls,2u); check_equal(orm_tidesdb_sql_scan_close(&outer_scan,&error),TURBODB_STATUS_OK);
    check_equal(run.source.active_runs,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],1u);
  }
  it("skips projection queries for empty input LIMIT zero cancellation and exhausted OFFSET") {
    fixture.fail_at=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT (SELECT v FROM hidden) FROM t");
    check_equal(bind_node(BOUND_OUTPUT,expression_root,false,NULL,0),TURBODB_STATUS_OK);
    const orm_sql_scan_expression output=bound_scan_expression(BOUND_OUTPUT); const turbodb_value_t rows[]={turbodb_i64(1)};
    for(size_t mode=0;mode<4;++mode) {
      const orm_sql_memory_source input={rows,mode?1:0,1,&outer_type};
      orm_sql_scan_spec spec=bound_scan_spec(); spec.expressions=&output;
      if(mode==1) spec.limit=0;
      if(mode==3) spec.offset=1;
      check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_OK);
      if(mode==2) check_equal(orm_tidesdb_sql_scan_cancel(&outer_scan,&error),TURBODB_STATUS_OK);
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,mode==2?ORM_SQL_SCAN_CANCELLED:ORM_SQL_SCAN_DONE); check_equal(fixture.calls,0u);
      check_equal(run.source.active_runs,1u); check_equal(orm_tidesdb_sql_scan_close(&outer_scan,&error),TURBODB_STATUS_OK);
    }
    check_equal(run.state,ORM_SQL_SUBQUERY_PENDING);
  }
  it("preserves filter short circuit without eagerly collecting EXISTS results") {
    fixture.fail_at=1; check_equal(open_query(ORM_SQL_SUBQUERY_EXISTS,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT id FROM t WHERE TRUE OR EXISTS(SELECT missing FROM hidden)");
    const sqlparser_node *select=sqlparser_get_node(document,sqlparser_statements(document).first);
    check_equal(bind_node(BOUND_FILTER,select->as.select.where,true,NULL,0),TURBODB_STATUS_OK);
    const turbodb_value_t rows[]={turbodb_i64(8)}; const orm_sql_memory_source input={rows,1,1,&outer_type};
    const orm_sql_scan_spec spec=bound_scan_spec(); orm_sql_scan_row row={0};
    check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.values[0].data.int64_value,8); check_equal(fixture.calls,0u);
  }
  it("delays scalar cardinality errors until a selected row reaches the CASE branch") {
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT CASE WHEN id=1 THEN 7 ELSE (SELECT missing FROM hidden) END FROM t");
    check_equal(bind_node(BOUND_OUTPUT,expression_root,false,NULL,0),TURBODB_STATUS_OK);
    const orm_sql_scan_expression output=bound_scan_expression(BOUND_OUTPUT); orm_sql_scan_spec spec=bound_scan_spec(); spec.expressions=&output;
    const turbodb_value_t rows[]={turbodb_i64(1),turbodb_i64(2)}; const orm_sql_memory_source input={rows,2,1,&outer_type};
    check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_OK);
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.values[0].data.int64_value,7); check_equal(fixture.calls,0u);
    const orm_sql_scan_row sentinel={ORM_SQL_SCAN_CANCELLED,rows,2}; row=sentinel;
    check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,&error),TURBODB_STATUS_SQL_ERROR);
    check_true(row.values==sentinel.values); check_equal(row.state,sentinel.state); check_equal(row.count,sentinel.count);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,&error),TURBODB_STATUS_SQL_ERROR);
    check_equal(orm_tidesdb_sql_scan_cancel(&outer_scan,&error),TURBODB_STATUS_OK);
    check_equal(fixture.calls,2u); check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
    check_equal(run.source.active_runs,1u); check_false(outer_scan.evaluating);
  }
  it("rejects missing malformed and mismatched dependency mappings even for LIMIT zero") {
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT COALESCE(7,(SELECT missing FROM hidden)) FROM t");
    check_equal(bind_node(BOUND_OUTPUT,expression_root,false,NULL,0),TURBODB_STATUS_OK);
    orm_sql_scan_expression output=bound_scan_expression(BOUND_OUTPUT); orm_sql_scan_spec spec=bound_scan_spec();
    spec.expressions=&output; spec.limit=0; const orm_sql_memory_source input={NULL,0,1,&outer_type};
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; const size_t invalid=1;
    output.query_slots=&invalid;
    check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    output.query_slots=NULL;
    check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    output=bound_scan_expression(BOUND_OUTPUT); output.query_count=0;
    check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    output=bound_scan_expression(BOUND_OUTPUT); query_sources[0]=NULL;
    check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    query_sources[0]=&run.source; run.source.type.result.kind=TURBODB_VALUE_TEXT;
    check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_TYPE_ERROR);
    run.source.type.result.kind=TURBODB_VALUE_INT64;
    check_null(outer_scan.budget); check_equal(run.source.active_runs,0u); check_equal(fixture.calls,0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("validates Binder query registries and keeps unbound subqueries closed") {
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT (SELECT x FROM hidden)+(SELECT y FROM hidden) FROM t");
    const sqlparser_id saved=query_bindings[1].node; query_bindings[1].node=query_bindings[0].node;
    check_equal(bind_node(BOUND_OUTPUT,expression_root,false,NULL,0),TURBODB_STATUS_INVALID_ARGUMENT); release_bound(&bound[BOUND_OUTPUT]);
    query_bindings[1].node=0;
    check_equal(bind_node(BOUND_OUTPUT,expression_root,false,NULL,0),TURBODB_STATUS_INVALID_ARGUMENT); release_bound(&bound[BOUND_OUTPUT]);
    query_bindings[1].node=saved; query_count=0;
    check_equal(bind_node(BOUND_OUTPUT,expression_root,false,NULL,0),TURBODB_STATUS_UNSUPPORTED); release_bound(&bound[BOUND_OUTPUT]);
    query_count=2; query_bindings[0].type.kind=ORM_SQL_SUBQUERY_EXISTS;
    check_equal(bind_node(BOUND_OUTPUT,expression_root,false,NULL,0),TURBODB_STATUS_INVALID_ARGUMENT); release_bound(&bound[BOUND_OUTPUT]);
    check_equal(fixture.calls,0u);
  }
  it("refunds every Binder and Scan open allocation including partially borrowed queries") {
    fixture.count=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT id+COALESCE((SELECT x FROM hidden),(SELECT y FROM hidden),7) FROM t");
    const uint64_t base=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; reserves=resizes=0;
    check_equal(bind_node(BOUND_OUTPUT,expression_root,false,NULL,0),TURBODB_STATUS_OK);
    const size_t binding_allocations[]={reserves,resizes}; release_bound(&bound[BOUND_OUTPUT]);
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=binding_allocations[pass];++point) {
      reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(bind_node(BOUND_OUTPUT,expression_root,false,NULL,0),TURBODB_STATUS_OUT_OF_MEMORY);
      fail_resize=fail_reserve=0; release_bound(&bound[BOUND_OUTPUT]);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],base);
    }
    check_equal(bind_node(BOUND_OUTPUT,expression_root,false,NULL,0),TURBODB_STATUS_OK);
    const orm_sql_scan_expression output=bound_scan_expression(BOUND_OUTPUT);
    const orm_sql_scan_order order={.expression=output}; orm_sql_scan_spec spec=bound_scan_spec();
    spec.expressions=&output; spec.orders=&order; spec.order_count=1;
    const orm_sql_memory_source input={NULL,0,1,&outer_type}; reserves=resizes=0;
    check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_OK);
    const size_t opening[]={reserves,resizes}; check_equal(run.source.active_runs,4u);
    check_equal(orm_tidesdb_sql_scan_close(&outer_scan,&error),TURBODB_STATUS_OK);
    const uint64_t compiled=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=opening[pass];++point) {
      reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      fail_resize=fail_reserve=0; check_null(outer_scan.budget); check_equal(run.source.active_runs,0u);
      check_equal(bound[BOUND_OUTPUT].program.active_runs,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],compiled);
    }
    check_equal(fixture.calls,0u);
  }
  it("releases partial query leases at every Scan opening step limit") {
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT id+(SELECT x FROM hidden) FROM t");
    check_equal(bind_node(BOUND_OUTPUT,expression_root,false,NULL,0),TURBODB_STATUS_OK);
    const orm_sql_scan_expression output=bound_scan_expression(BOUND_OUTPUT);
    const orm_sql_scan_order order={.expression=output}; orm_sql_scan_spec spec=bound_scan_spec();
    spec.expressions=&output; spec.orders=&order; spec.order_count=1;
    const orm_sql_memory_source input={NULL,0,1,&outer_type};
    const uint64_t before=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_OK);
    const uint64_t needed=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-before;
    check_equal(orm_tidesdb_sql_scan_close(&outer_scan,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(uint64_t step=0;step<needed;++step) {
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+step;
      check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_null(outer_scan.budget); check_equal(run.source.active_runs,0u); check_equal(bound[BOUND_OUTPUT].program.active_runs,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    check_equal(fixture.calls,0u);
  }
  it("retains scalar byte results across ordered rows until the Scan releases its lease") {
    fixture.count=1; types[0]=(orm_sql_type){TURBODB_VALUE_TEXT,false}; fixture.values[0][0]=turbodb_text("placeholder");
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT (SELECT label FROM hidden) FROM t ORDER BY id");
    check_equal(bind_node(BOUND_OUTPUT,expression_root,false,NULL,0),TURBODB_STATUS_OK);
    const orm_sql_scan_expression output=bound_scan_expression(BOUND_OUTPUT); const orm_sql_scan_order order={.slot=0};
    orm_sql_scan_spec spec=bound_scan_spec(); spec.expressions=&output; spec.orders=&order; spec.order_count=1;
    const turbodb_value_t rows[]={turbodb_i64(3),turbodb_i64(1),turbodb_i64(2)}; const orm_sql_memory_source input={rows,3,1,&outer_type};
    check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document=NULL;
    orm_sql_scan_row row={0};
    for(size_t i=0;i<input.rows;++i) {
      check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_ROW);
      check_equal(row.values[0].kind,TURBODB_VALUE_TEXT); check_equal(row.values[0].data.text_value.len,TEST_BYTES);
      check_equal(memcmp(row.values[0].data.text_value.data,"a\0z",TEST_BYTES),0);
    }
    check_equal(fixture.calls,2u); check_equal(fixture.payload[0],'!');
    check_equal(orm_tidesdb_sql_subquery_cancel(&run,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_scan_close(&outer_scan,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_subquery_cancel(&run,&error),TURBODB_STATUS_OK);
  }
  it("locks query failure during sort without publishing a prefix") {
    fixture.fail_at=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT id FROM t ORDER BY id+(SELECT x FROM hidden)");
    const sqlparser_node *select=sqlparser_get_node(document,sqlparser_statements(document).first);
    check_equal(bind_node(BOUND_ORDER,sqlparser_get_node(document,select->as.select.order_by.first)->as.order.expression,false,NULL,0),TURBODB_STATUS_OK);
    const orm_sql_scan_order order={.expression=bound_scan_expression(BOUND_ORDER)};
    orm_sql_scan_spec spec=bound_scan_spec(); spec.orders=&order; spec.order_count=1;
    const turbodb_value_t rows[]={turbodb_i64(3),turbodb_i64(1)}; const orm_sql_memory_source input={rows,2,1,&outer_type};
    check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_OK);
    orm_sql_scan_row row={ORM_SQL_SCAN_CANCELLED,rows,TEST_ROWS};
    check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,&error),TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(row.state,ORM_SQL_SCAN_CANCELLED); check_true(row.values==rows); check_equal(row.count,(size_t)TEST_ROWS);
    check_equal(outer_scan.state,ORM_SQL_SCAN_ERROR); check_false(outer_scan.evaluating);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,&error),TURBODB_STATUS_DATASTORE_ERROR);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps); check_equal(fixture.calls,1u);
  }
  it("prevents Scan reentry and destruction while a query callback is evaluating") {
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT id+(SELECT x FROM hidden) FROM t");
    check_equal(bind_node(BOUND_OUTPUT,expression_root,false,NULL,0),TURBODB_STATUS_OK);
    query_callback_fixture callback={.value=turbodb_i64(2),.reenter_scan=true};
    orm_sql_expr_query_source scalar={&budget,run.source.type,&callback,query_callback,0}; query_sources[0]=&scalar;
    const orm_sql_scan_expression output=bound_scan_expression(BOUND_OUTPUT); orm_sql_scan_spec spec=bound_scan_spec(); spec.expressions=&output;
    const turbodb_value_t rows[]={turbodb_i64(1)}; const orm_sql_memory_source input={rows,1,1,&outer_type};
    check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_OK);
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.values[0].data.int64_value,3); check_false(outer_scan.evaluating); check_equal(callback.calls,1u);
    check_equal(orm_tidesdb_sql_scan_close(&outer_scan,&error),TURBODB_STATUS_OK); check_equal(scalar.active_runs,0u);
  }
  it("passes the current source row to every query dependency evaluation") {
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT id+(SELECT x FROM hidden) FROM t");
    check_equal(bind_node(BOUND_OUTPUT,expression_root,false,NULL,0),TURBODB_STATUS_OK);
    query_callback_fixture callback={.return_outer=true};
    orm_sql_expr_query_source scalar={&budget,run.source.type,&callback,query_callback,0}; query_sources[0]=&scalar;
    const orm_sql_scan_expression output=bound_scan_expression(BOUND_OUTPUT);
    orm_sql_scan_spec spec=bound_scan_spec(); spec.expressions=&output;
    const turbodb_value_t rows[]={turbodb_i64(1),turbodb_i64(3)};
    const orm_sql_memory_source input={rows,2,1,&outer_type};
    check_equal(orm_tidesdb_sql_scan_open(&input,&spec,&budget,&outer_scan,&error),TURBODB_STATUS_OK);
    orm_sql_scan_row row={0};
    check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.values[0].data.int64_value,2); check_equal(callback.outer_count,1u);
    check_equal(callback.outer_first.data.int64_value,1);
    check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.values[0].data.int64_value,6); check_equal(callback.outer_count,1u);
    check_equal(callback.outer_first.data.int64_value,3); check_equal(callback.calls,2u);
    check_equal(orm_tidesdb_sql_scan_next(&outer_scan,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_DONE);
  }
  it("keeps inner aggregate calls and aliases outside the outer SELECT scope") {
    fixture.count=1; fixture.values[0][0]=turbodb_i64(3); check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT id,(SELECT SUM(v) FROM hidden) AS q FROM t WHERE id<(SELECT COUNT(*) FROM hidden) ORDER BY id+(SELECT SUM(q) FROM hidden)");
    check_equal(bind_selected(NULL,0),TURBODB_STATUS_OK); check_false(selected_plan.grouped);
    check_equal(open_selected(NULL,0),TURBODB_STATUS_OK); check_equal(run.source.active_runs,3u); check_equal(fixture.calls,0u);
    sqlparser_document_destroy(document); document=NULL; memset(query_sources,0,sizeof(query_sources));
    for(int64_t expected=1;expected<=2;++expected) {
      const orm_sql_scan_row row=selected_next(); check_equal(row.count,2u);
      check_equal(row.values[0].data.int64_value,expected); check_equal(row.values[1].data.int64_value,3);
    }
    check_equal(selected_next().state,ORM_SQL_SCAN_DONE); check_equal(fixture.calls,2u);
  }
  it("evaluates group keys aggregate arguments WHERE and HAVING against their own row scopes") {
    fixture.count=1; fixture.values[0][0]=turbodb_i64(3); check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT id+(SELECT MAX(v) FROM hidden) AS k,COUNT((SELECT SUM(v) FROM hidden)) AS n FROM t WHERE id<=(SELECT MAX(v) FROM hidden) GROUP BY 1 HAVING n<=(SELECT COUNT(*) FROM hidden) ORDER BY k DESC");
    check_equal(bind_selected(NULL,0),TURBODB_STATUS_OK); check_true(selected_plan.grouped);
    check_equal(vec_size(&selected_plan.aggregate_items),1u); check_equal(vec_size(&selected_plan.group_keys),1u);
    check_equal(open_selected(NULL,0),TURBODB_STATUS_OK); check_equal(run.source.active_runs,4u); check_equal(fixture.calls,0u);
    for(int64_t expected=6;expected>=4;--expected) {
      const orm_sql_scan_row row=selected_next(); check_equal(row.count,2u);
      check_equal(row.values[0].data.int64_value,expected); check_equal(row.values[1].data.uint64_value,1u);
    }
    check_equal(selected_next().state,ORM_SQL_SCAN_DONE); check_equal(fixture.calls,2u);
  }
  it("does not evaluate aggregate argument queries for empty input or LIMIT zero") {
    fixture.fail_at=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT COUNT((SELECT SUM(v) FROM hidden)) AS n FROM t");
    check_equal(bind_selected(NULL,0),TURBODB_STATUS_OK); outer_rows.count=0;
    check_equal(open_selected(NULL,0),TURBODB_STATUS_OK);
    const orm_sql_scan_row row=selected_next(); check_equal(row.count,1u); check_equal(row.values[0].data.uint64_value,0u);
    check_equal(fixture.calls,0u); check_equal(orm_tidesdb_sql_select_close(&selected_run,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_destroy(&selected_plan,&error),TURBODB_STATUS_OK);
    parse_expression("SELECT COUNT((SELECT SUM(v) FROM hidden)) AS n FROM t LIMIT 0");
    check_equal(bind_selected(NULL,0),TURBODB_STATUS_OK); outer_rows.count=3; outer_rows.calls=0;
    check_equal(open_selected(NULL,0),TURBODB_STATUS_OK); check_equal(selected_next().state,ORM_SQL_SCAN_DONE);
    check_equal(fixture.calls,0u); check_equal(outer_rows.calls,0u);
  }
  it("shares a query cache with DISTINCT output and its ORDER BY alias") {
    fixture.count=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT DISTINCT (SELECT COUNT(*) FROM hidden) AS n FROM t ORDER BY n");
    check_equal(bind_selected(NULL,0),TURBODB_STATUS_OK); check_false(selected_plan.grouped);
    check_equal(open_selected(NULL,0),TURBODB_STATUS_OK); check_equal(run.source.active_runs,2u);
    check_equal(selected_next().values[0].data.int64_value,1); check_equal(selected_next().state,ORM_SQL_SCAN_DONE);
    check_equal(fixture.calls,2u);
  }
  it("preserves outer SELECT parameter ordinals across opaque query parameters") {
    fixture.count=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT id+(SELECT x FROM hidden WHERE x=?)+? AS n FROM t WHERE id>=? ORDER BY id");
    const orm_sql_type parameter_types[]={outer_type,outer_type,outer_type};
    const turbodb_value_t values[]={turbodb_i64(100),turbodb_i64(10),turbodb_i64(2)};
    check_equal(bind_selected(parameter_types,3),TURBODB_STATUS_OK); check_equal(open_selected(values,3),TURBODB_STATUS_OK);
    check_equal(selected_next().values[0].data.int64_value,13); check_equal(selected_next().values[0].data.int64_value,14);
    check_equal(selected_next().state,ORM_SQL_SCAN_DONE); check_equal(fixture.calls,2u);
  }
  it("unwinds grouped input query leases when HAVING source validation fails") {
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT COUNT((SELECT SUM(v) FROM hidden)) AS n FROM t HAVING n>(SELECT COUNT(*) FROM hidden)");
    check_equal(bind_selected(NULL,0),TURBODB_STATUS_OK); check_equal(query_count,2u);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; query_sources[1]=NULL;
    check_equal(open_selected(NULL,0),TURBODB_STATUS_INVALID_ARGUMENT);
    check_null(selected_run.program); check_null(selected_run.scan.budget); check_false(selected_run.group_run.initialized);
    check_equal(selected_plan.active_runs,0u); check_equal(run.source.active_runs,0u); check_false(outer_source.active);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_equal(fixture.calls,0u); check_equal(outer_rows.calls,0u);
  }
  it("refunds every grouped SELECT bind and open allocation with query dependencies") {
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=TEST_WORK;
    fixture.count=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT id+(SELECT MAX(v) FROM hidden) AS k,COUNT((SELECT SUM(v) FROM hidden)) AS n FROM t GROUP BY 1 HAVING n>=(SELECT COUNT(*) FROM hidden) ORDER BY k");
    const uint64_t base=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; reserves=resizes=0;
    check_equal(bind_selected(NULL,0),TURBODB_STATUS_OK); const size_t compilation[]={reserves,resizes};
    check_equal(orm_tidesdb_sql_select_destroy(&selected_plan,&error),TURBODB_STATUS_OK);
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=compilation[pass];++point) {
      reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(bind_selected(NULL,0),TURBODB_STATUS_OUT_OF_MEMORY); fail_resize=fail_reserve=0;
      check_null(selected_plan.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],base);
    }
    check_equal(bind_selected(NULL,0),TURBODB_STATUS_OK); reserves=resizes=0;
    check_equal(open_selected(NULL,0),TURBODB_STATUS_OK); const size_t opening[]={reserves,resizes};
    check_equal(orm_tidesdb_sql_select_close(&selected_run,&error),TURBODB_STATUS_OK);
    const uint64_t compiled=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=opening[pass];++point) {
      reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(open_selected(NULL,0),TURBODB_STATUS_OUT_OF_MEMORY); fail_resize=fail_reserve=0;
      check_null(selected_run.program); check_false(outer_source.active); check_equal(run.source.active_runs,0u);
      check_equal(selected_plan.active_runs,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],compiled);
    }
    check_equal(fixture.calls,0u); check_equal(outer_rows.calls,0u);
  }
  it("preserves independent query slots through RIGHT JOIN physical column remapping") {
    fixture.count=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT t.id AS a,u.id AS b,(SELECT SUM(v) FROM hidden) AS q FROM t RIGHT JOIN u ON t.id+(SELECT MAX(v) FROM hidden)=u.id WHERE u.id<=(SELECT COUNT(*) FROM hidden)+5 ORDER BY u.id");
    check_equal(bind_joined(),TURBODB_STATUS_OK); check_false(selected_plan.grouped);
    check_equal(open_joined(),TURBODB_STATUS_OK); check_equal(run.source.active_runs,3u); check_equal(fixture.calls,0u);
    sqlparser_document_destroy(document); document=NULL;
    orm_sql_scan_row row=selected_next(); check_equal(row.count,3u);
    check_equal(row.values[0].data.int64_value,1); check_equal(row.values[1].data.int64_value,2); check_equal(row.values[2].data.int64_value,1);
    row=selected_next(); check_equal(row.values[0].kind,TURBODB_VALUE_NULL); check_equal(row.values[1].data.int64_value,5);
    check_equal(row.values[2].data.int64_value,1); check_equal(selected_next().state,ORM_SQL_SCAN_DONE); check_equal(fixture.calls,2u);
    check_equal(orm_tidesdb_sql_from_close(&from_execution,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_tidesdb_sql_select_close(&selected_run,&error),TURBODB_STATUS_OK); check_equal(run.source.active_runs,1u);
    check_equal(orm_tidesdb_sql_from_close(&from_execution,&error),TURBODB_STATUS_OK); check_equal(run.source.active_runs,0u);
  }
  it("keeps LEFT JOIN ON short circuit lazy while emitting NULL extended rows") {
    fixture.fail_at=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT t.id AS a,u.id AS b FROM t LEFT JOIN u ON FALSE AND t.id=(SELECT SUM(v) FROM hidden)");
    check_equal(bind_joined(),TURBODB_STATUS_OK); check_equal(open_joined(),TURBODB_STATUS_OK);
    for(size_t i=0;i<outer_rows.count;++i) {
      const orm_sql_scan_row row=selected_next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
      check_equal(row.values[0].data.int64_value,outer_rows.values[i].data.int64_value); check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
    }
    check_equal(selected_next().state,ORM_SQL_SCAN_DONE); check_equal(fixture.calls,0u); check_equal(run.source.active_runs,1u);
  }
  it("skips ON query evaluation when the right input is empty") {
    fixture.fail_at=1; right_rows.count=0; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT t.id AS a,u.id AS b FROM t LEFT JOIN u ON t.id=(SELECT SUM(v) FROM hidden)");
    check_equal(bind_joined(),TURBODB_STATUS_OK); check_equal(open_joined(),TURBODB_STATUS_OK);
    for(size_t i=0;i<outer_rows.count;++i) { const orm_sql_scan_row row=selected_next(); check_equal(row.values[1].kind,TURBODB_VALUE_NULL); }
    check_equal(fixture.calls,0u); check_equal(selected_next().state,ORM_SQL_SCAN_DONE);
  }
  it("propagates ON scalar cardinality failure without publishing a joined row") {
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT t.id AS a,u.id AS b FROM t JOIN u ON t.id=(SELECT SUM(v) FROM hidden)");
    check_equal(bind_joined(),TURBODB_STATUS_OK); check_equal(open_joined(),TURBODB_STATUS_OK);
    orm_sql_scan_row row={.state=ORM_SQL_SCAN_CANCELLED,.count=TEST_ROWS};
    check_equal(orm_tidesdb_sql_scan_next(&selected_run.scan,&row,&error),TURBODB_STATUS_SQL_ERROR);
    check_equal(row.state,ORM_SQL_SCAN_CANCELLED); check_equal(row.count,(size_t)TEST_ROWS); check_equal(fixture.calls,2u);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_scan_next(&selected_run.scan,&row,&error),TURBODB_STATUS_SQL_ERROR);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps); check_equal(run.source.active_runs,1u);
  }
  it("protects the SELECT owner against destruction during a query callback") {
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT id+(SELECT SUM(v) FROM hidden) AS n FROM t");
    check_equal(bind_selected(NULL,0),TURBODB_STATUS_OK);
    query_callback_fixture callback={.value=turbodb_i64(2),.reenter_select=true};
    orm_sql_expr_query_source scalar={&budget,run.source.type,&callback,query_callback,0}; query_sources[0]=&scalar;
    check_equal(open_selected(NULL,0),TURBODB_STATUS_OK); check_equal(selected_next().values[0].data.int64_value,5);
    check_equal(selected_plan.active_runs,1u); check_equal(scalar.active_runs,1u);
    check_equal(orm_tidesdb_sql_select_close(&selected_run,&error),TURBODB_STATUS_OK); check_equal(scalar.active_runs,0u);
  }
  it("does not substitute an outer HAVING alias into an inner aggregate argument") {
    fixture.count=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT COUNT(*)+1 AS n FROM t HAVING COUNT(*)>(SELECT SUM(n) FROM hidden)");
    check_equal(bind_selected(NULL,0),TURBODB_STATUS_OK); check_true(selected_plan.grouped);
    check_equal(open_selected(NULL,0),TURBODB_STATUS_OK); check_equal(selected_next().values[0].data.uint64_value,4u);
    check_equal(selected_next().state,ORM_SQL_SCAN_DONE); check_equal(fixture.calls,2u);
  }
  it("keeps a nested UNION tail outside outer ORDER BY alias validation") {
    fixture.count=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT id+1 AS q FROM t ORDER BY id+(SELECT SUM(v) AS q FROM hidden UNION ALL SELECT MAX(v) AS q FROM hidden ORDER BY q LIMIT 1)");
    check_equal(bind_selected(NULL,0),TURBODB_STATUS_OK); check_false(selected_plan.grouped);
    check_equal(open_selected(NULL,0),TURBODB_STATUS_OK);
    for(int64_t expected=2;expected<=4;++expected) check_equal(selected_next().values[0].data.int64_value,expected);
    check_equal(selected_next().state,ORM_SQL_SCAN_DONE); check_equal(fixture.calls,2u);
  }
  it("passes query sources into standalone compound tail sorting") {
    fixture.count=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT id FROM t UNION ALL SELECT id FROM t ORDER BY id+(SELECT SUM(id) FROM hidden)");
    const orm_sql_query_scope scope=query_scope(NULL,0);
    const sqlparser_node *compound=sqlparser_get_node(document,scope.root);
    check_equal(orm_tidesdb_sql_select_bind_tail(&scope,&outer_schema,compound->as.compound.order_by,
        compound->as.compound.limit,&selected_plan,&error),TURBODB_STATUS_OK);
    check_equal(open_selected(NULL,0),TURBODB_STATUS_OK);
    for(int64_t expected=1;expected<=3;++expected) check_equal(selected_next().values[0].data.int64_value,expected);
    check_equal(selected_next().state,ORM_SQL_SCAN_DONE); check_equal(fixture.calls,2u);
  }
  it("refunds every FROM bind and RIGHT JOIN open allocation with query sources") {
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=TEST_WORK;
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT t.id FROM t RIGHT JOIN u ON t.id+(SELECT SUM(v) FROM hidden)=u.id");
    const orm_sql_query_scope scope=query_scope(NULL,0); const orm_sql_table_schema *schemas[]={&outer_schema,&right_schema};
    const uint64_t base=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; reserves=resizes=0;
    check_equal(orm_tidesdb_sql_from_bind_at(&scope,schemas,2,&from_plan,&error),TURBODB_STATUS_OK);
    const size_t compilation[]={reserves,resizes}; check_equal(orm_tidesdb_sql_from_destroy(&from_plan,&error),TURBODB_STATUS_OK);
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=compilation[pass];++point) {
      reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(orm_tidesdb_sql_from_bind_at(&scope,schemas,2,&from_plan,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      fail_resize=fail_reserve=0; check_null(from_plan.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],base);
    }
    check_equal(orm_tidesdb_sql_from_bind_at(&scope,schemas,2,&from_plan,&error),TURBODB_STATUS_OK);
    const orm_sql_expr_query_sources queries={query_sources,query_count}; orm_sql_row_source *inputs[]={&outer_source,&right_source};
    reserves=resizes=0;
    check_equal(orm_tidesdb_sql_from_open_queries(&from_plan,inputs,2,NULL,0,&queries,&from_execution,&error),TURBODB_STATUS_OK);
    const size_t opening[]={reserves,resizes}; check_equal(run.source.active_runs,1u);
    check_equal(orm_tidesdb_sql_from_close(&from_execution,&error),TURBODB_STATUS_OK);
    const uint64_t compiled=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=opening[pass];++point) {
      reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(orm_tidesdb_sql_from_open_queries(&from_plan,inputs,2,NULL,0,&queries,&from_execution,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      fail_resize=fail_reserve=0; check_null(from_execution.plan); check_equal(from_plan.active_runs,0u);
      check_equal(run.source.active_runs,0u); check_false(outer_source.active); check_false(right_source.active);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],compiled);
    }
    check_equal(fixture.calls,0u); check_equal(outer_rows.calls,0u); check_equal(right_rows.calls,0u);
  }
  it("rejects the old FROM open when its ON program requires query dependencies") {
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT t.id FROM t JOIN u ON t.id=(SELECT SUM(v) FROM hidden)");
    check_equal(bind_joined(),TURBODB_STATUS_OK); orm_sql_row_source *inputs[]={&outer_source,&right_source};
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_tidesdb_sql_from_open(&from_plan,inputs,2,NULL,0,&from_execution,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_null(from_execution.plan); check_equal(from_plan.active_runs,0u); check_equal(run.source.active_runs,0u);
    check_false(outer_source.active); check_false(right_source.active); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("protects JOIN and FROM owners while a directly pulled query callback is active") {
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    parse_expression("SELECT t.id FROM t JOIN u ON t.id=(SELECT SUM(v) FROM hidden)");
    check_equal(bind_joined(),TURBODB_STATUS_OK);
    query_callback_fixture callback={.value=turbodb_i64(3),.reenter_from=true};
    orm_sql_expr_query_source scalar={&budget,run.source.type,&callback,query_callback,0}; query_sources[0]=&scalar;
    const orm_sql_expr_query_sources queries={query_sources,query_count}; orm_sql_row_source *inputs[]={&outer_source,&right_source};
    check_equal(orm_tidesdb_sql_from_open_queries(&from_plan,inputs,2,NULL,0,&queries,&from_execution,&error),TURBODB_STATUS_OK);
    const turbodb_value_t *row=NULL;
    check_equal(from_execution.source->next(from_execution.source->context,&row,&error),TURBODB_STATUS_OK);
    check_not_null(row); check_equal(row[0].data.int64_value,3); check_equal(callback.calls,1u);
    check_equal(orm_tidesdb_sql_from_close(&from_execution,&error),TURBODB_STATUS_OK); check_equal(scalar.active_runs,0u);
  }
  it("bounds opening and first-copy work and validates closed lifecycle calls") {
    turbodb_value_t value=turbodb_i64(99);
    check_equal(orm_tidesdb_sql_subquery_eval(&run,NULL,&value,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(orm_tidesdb_sql_subquery_cancel(&run,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(orm_tidesdb_sql_subquery_close(NULL,&error),TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=sizeof(run);
    check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(run.budget); reset();
    fixture.count=1; check_equal(open_query(ORM_SQL_SUBQUERY_SCALAR,NULL),TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_tidesdb_sql_subquery_eval(&run,NULL,&value,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(value.data.int64_value,99); locked(TURBODB_STATUS_LIMIT_EXCEEDED,NULL);
  }
}
