#include "parameters.h"
#include <tinytest.h>
#include <string.h>

/* Only the existing SQL WORK Vec allocation points are injected. */
static size_t reserves,resizes,fail_reserve,fail_resize;
static stl_status probe_reserve(vec_t *v,size_t n) { return ++reserves==fail_reserve?STL_OUT_OF_MEMORY:vec_reserve(v,n); }
static stl_status probe_resize(vec_t *v,size_t n) { return ++resizes==fail_resize?STL_OUT_OF_MEMORY:vec_resize(v,n); }
#define vec_reserve probe_reserve
#define vec_resize probe_resize
#include "../../src/work.c"
#undef vec_reserve
#undef vec_resize

enum { NODES=4096,WORK=4*1024*1024,STEPS=1000000,DEPTH=64,OUTPUT_SENTINEL=71,INPUTS=16 };
static orm_tidesdb_sql_budget budget;
static orm_sql_parameters parameters;
static sqlparser_document *document;
static turbodb_error_t error;
static const orm_sql_schema_column columns[]={
  {.name={"i",1},.type={TURBODB_VALUE_INT64,false}},
  {.name={"u",1},.type={TURBODB_VALUE_UINT64,false}},
  {.name={"d",1},.type={TURBODB_VALUE_DOUBLE,true}},
  {.name={"b",1},.type={TURBODB_VALUE_BOOLEAN,false}},
  {.name={"t",1},.type={TURBODB_VALUE_TEXT,true}},
  {.name={"x",1},.type={TURBODB_VALUE_BLOB,true}}};
static const orm_sql_table_schema schema={.name={"items",5},.columns=columns,.count=sizeof(columns)/sizeof(columns[0])};
static orm_sql_binding_scope scope(void) {
  return (orm_sql_binding_scope){.document=document,.schema=&schema,.qualifier={"items",5},.budget=&budget};
}
static void parse(const char *sql) {
  info("parameter inference SQL: %s",sql);
  sqlparser_error e;
  check_equal(sqlparser_parse(sql,strlen(sql),NULL,&document,&e),SQLPARSER_OK);
}
static sqlparser_id projection(size_t position) {
  const sqlparser_node *select=sqlparser_get_node(document,sqlparser_statements(document).first);
  check_equal(select->kind,SQLPARSER_SELECT);
  sqlparser_id id=select->as.select.columns.first;
  for(size_t i=0;i<position;++i) id=sqlparser_get_node(document,id)->next;
  return sqlparser_get_node(document,id)->as.projection.expression;
}
static void metadata_open(void) { check_equal(orm_sql_parameters_open(document,&budget,&parameters,&error),TURBODB_STATUS_OK); }
static turbodb_status_t infer(size_t position,const orm_sql_type *context,size_t depth) {
  const orm_sql_binding_scope local=scope(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
  const turbodb_status_t status=orm_sql_parameters_infer(&parameters,&local,projection(position),context,depth,&error);
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); return status;
}
static void expect(const turbodb_value_kind_t *kinds,size_t expected) {
  const orm_sql_type *types=NULL; size_t count=OUTPUT_SENTINEL;
  check_equal(orm_sql_parameters_types(&parameters,&types,&count,&error),TURBODB_STATUS_OK); check_equal(count,expected);
  for(size_t i=0;i<count;++i) { check_equal(types[i].kind,kinds[i]); check_true(types[i].nullable); }
}
static void reset(void) {
  fail_reserve=fail_resize=0;
  check_equal(orm_sql_parameters_close(&parameters,&error),TURBODB_STATUS_OK);
  sqlparser_document_destroy(document); document=NULL;
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
  check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
}
spec("TidesSQL scalar unknown parameter inference") {
  before_each() {
    reserves=resizes=fail_reserve=fail_resize=0; tdsql_error_init(&error);
    parameters=(orm_sql_parameters){0}; document=NULL;
    orm_sql_budget_limits limits={0};
    for(size_t i=0;i<ORM_SQL_BUDGET_RESOURCE_COUNT;++i) limits.statement.value[i]=NODES;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=WORK;
    limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=STEPS;
    limits.transaction=(orm_sql_transaction_budget_amount){NODES,NODES,NODES};
    check_equal(orm_tidesdb_sql_budget_init(&budget,&limits,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
  }
  after_each() {
    reset(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK);
  }
  it("admits only a single MySQL document and an empty owner with an active budget") {
    parse("SELECT ?");
    check_equal(orm_sql_parameters_open(NULL,&budget,&parameters,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_sql_parameters_open(document,NULL,&parameters,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_sql_parameters_open(document,&budget,NULL,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    const orm_sql_budget_limits limits=budget.limits;
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=0;
    check_equal(orm_sql_parameters_open(document,&budget,&parameters,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(parameters.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u); budget.limits=limits;
    check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_parameters_open(document,&budget,&parameters,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
    metadata_open();
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_sql_parameters_open(document,&budget,&parameters,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); reset();
    parse("SELECT ?; SELECT ?");
    check_equal(orm_sql_parameters_open(document,&budget,&parameters,&error),TURBODB_STATUS_UNSUPPORTED); reset();
    sqlparser_error parse_error; const char sql[]="SELECT ?";
    check_equal(sqlparser_parse_dialect(sql,strlen(sql),SQLPARSER_SQLITE,NULL,&document,&parse_error),SQLPARSER_OK);
    check_equal(orm_sql_parameters_open(document,&budget,&parameters,&error),TURBODB_STATUS_UNSUPPORTED);
    check_equal(orm_sql_parameters_close(NULL,&error),TURBODB_STATUS_OK);
    const orm_sql_type *view=NULL; size_t count=OUTPUT_SENTINEL;
    check_equal(orm_sql_parameters_types(NULL,&view,&count,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(count,(size_t)OUTPUT_SENTINEL); check_null(view);
  }
  it("keeps unresolved markers separate from NULL and publishes only complete source ordered metadata") {
    parse("SELECT ? AS a,'?' AS literal,?+1 AS b"); metadata_open();
    const orm_sql_type sentinel={TURBODB_VALUE_BLOB,false}; const orm_sql_type *view=&sentinel; size_t count=OUTPUT_SENTINEL;
    check_equal(orm_sql_parameters_types(&parameters,&view,&count,&error),TURBODB_STATUS_INVALID_STATE);
    check_true(view==&sentinel); check_equal(count,(size_t)OUTPUT_SENTINEL);
    check_equal(infer(2,NULL,DEPTH),TURBODB_STATUS_OK);
    check_equal(orm_sql_parameters_types(&parameters,&view,&count,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_OK);
    const turbodb_value_kind_t kinds[]={TURBODB_VALUE_TEXT,TURBODB_VALUE_INT64}; expect(kinds,2);
    check_equal(orm_sql_parameters_types(&parameters,&view,&count,&error),TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document=NULL;
    expect(kinds,2); check_equal(view[0].kind,TURBODB_VALUE_TEXT);
  }
  it("fails binding before unresolved type storage can be consumed") {
    parse("SELECT ?+i"); metadata_open(); orm_sql_binding_scope local=scope();
    local.parameter_types=vec_data_const(&parameters.types);
    local.parameter_offsets=vec_data_const(&parameters.offsets);
    local.parameter_count=vec_size(&parameters.types);
    local.parameter_resolved=vec_data_const(&parameters.resolved);
    orm_sql_expr program={0}; vec_t slots={0}; size_t slot_bytes=0;
    const orm_sql_expression_target target={.program=&program,.slots=&slots,.slot_bytes=&slot_bytes};
    const turbodb_status_t status=orm_sql_bind_expression(&local,projection(0),DEPTH,target,false,&error);
    check_equal(status,TURBODB_STATUS_INVALID_STATE);
    check_equal(orm_tidesdb_sql_expr_destroy(&program,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_work_release(&slots,slot_bytes,&budget,&error),TURBODB_STATUS_OK);
  }
  it("derives direct comparison and arithmetic markers from their peer") {
    const char *const sql[]={"SELECT ?+1","SELECT 1+?","SELECT ?+1.5","SELECT ?=TRUE","SELECT 'text'=?","SELECT ?=?","SELECT ?=NULL",
      "SELECT ?=18446744073709551615","SELECT ?=-9223372036854775808"};
    const turbodb_value_kind_t expected[]={TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,TURBODB_VALUE_DOUBLE,
      TURBODB_VALUE_BOOLEAN,TURBODB_VALUE_TEXT,TURBODB_VALUE_TEXT,TURBODB_VALUE_TEXT,TURBODB_VALUE_UINT64,TURBODB_VALUE_INT64};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse(sql[i]); metadata_open(); check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_OK);
      const turbodb_value_kind_t kinds[]={expected[i],expected[i]}; expect(kinds,i==5?2:1); reset();
    }
  }
  it("uses DOUBLE for arithmetic without context including arithmetic inside a comparison") {
    const char *const sql[]={"SELECT ?+(?+?)","SELECT -(?+?)","SELECT ?/(?+?)","SELECT ?=(?+?)","SELECT (?+?)=(?+?)"};
    const size_t counts[]={3,2,3,3,4};
    const turbodb_value_kind_t kinds[]={TURBODB_VALUE_DOUBLE,TURBODB_VALUE_DOUBLE,TURBODB_VALUE_DOUBLE,TURBODB_VALUE_DOUBLE};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse(sql[i]); metadata_open(); check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_OK); expect(kinds,counts[i]); reset();
    }
  }
  it("derives local column types with exact qualification including arbitrary bytes") {
    const char *const sql[]={"SELECT ?=items.i","SELECT u=?","SELECT ?+d","SELECT b=?","SELECT ?=t","SELECT x=?"};
    const turbodb_value_kind_t kinds[]={TURBODB_VALUE_INT64,TURBODB_VALUE_UINT64,TURBODB_VALUE_DOUBLE,
      TURBODB_VALUE_BOOLEAN,TURBODB_VALUE_TEXT,TURBODB_VALUE_BLOB};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse(sql[i]); metadata_open(); check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_OK); expect(&kinds[i],1); reset();
    }
  }
  it("derives correlated marker types from an immutable outer schema") {
    parse("SELECT items.i+?"); metadata_open();
    const orm_sql_table_schema empty={0}; orm_sql_binding_scope local=scope();
    local.schema=&empty; local.qualifier=(vstr){0}; local.outer_schema=&schema;
    local.outer_qualifier=schema.name;
    check_equal(orm_sql_parameters_infer(&parameters,&local,projection(0),NULL,DEPTH,&error),
        TURBODB_STATUS_OK);
    const turbodb_value_kind_t kind=TURBODB_VALUE_INT64; expect(&kind,1);
  }
  it("uses real assignment contexts and numeric CAST targets without executing conversion") {
    for(size_t i=0;i<schema.count;++i) {
      parse("SELECT ?"); metadata_open(); check_equal(infer(0,&columns[i].type,DEPTH),TURBODB_STATUS_OK);
      expect(&columns[i].type.kind,1); reset();
    }
    const char *const sql[]={"SELECT CAST(? AS SIGNED)","SELECT CAST(? AS UNSIGNED)","SELECT CAST(? AS DOUBLE)","SELECT CAST(? AS FLOAT)"};
    const turbodb_value_kind_t kinds[]={TURBODB_VALUE_INT64,TURBODB_VALUE_UINT64,TURBODB_VALUE_DOUBLE,TURBODB_VALUE_DOUBLE};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse(sql[i]); metadata_open(); check_equal(infer(0,&columns[4].type,DEPTH),TURBODB_STATUS_OK); expect(&kinds[i],1); reset();
    }
  }
  it("propagates assignment context through wholly untyped arithmetic but not comparisons") {
    parse("SELECT ?+(?+?)"); metadata_open(); check_equal(infer(0,&columns[0].type,DEPTH),TURBODB_STATUS_OK);
    const turbodb_value_kind_t numeric[]={TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,TURBODB_VALUE_INT64}; expect(numeric,3); reset();
    parse("SELECT ?=?"); metadata_open(); check_equal(infer(0,&columns[0].type,DEPTH),TURBODB_STATUS_OK);
    const turbodb_value_kind_t strings[]={TURBODB_VALUE_TEXT,TURBODB_VALUE_TEXT}; expect(strings,2);
  }
  it("derives LIKE text and boolean logic markers without evaluating") {
    const char *const sql[]={"SELECT t LIKE ?","SELECT ? NOT LIKE t ESCAPE '!'",
      "SELECT (t LIKE ?) AND (? OR NOT ?)","SELECT (?=i) OR (? AND NOT ?)"};
    const size_t counts[]={1,1,3,3};
    const turbodb_value_kind_t expected[][3]={
      {TURBODB_VALUE_TEXT},{TURBODB_VALUE_TEXT},
      {TURBODB_VALUE_TEXT,TURBODB_VALUE_BOOLEAN,TURBODB_VALUE_BOOLEAN},
      {TURBODB_VALUE_INT64,TURBODB_VALUE_BOOLEAN,TURBODB_VALUE_BOOLEAN}};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse(sql[i]); metadata_open(); check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_OK);
      expect(expected[i],counts[i]); reset();
    }
  }
  it("derives scalar IN and BETWEEN markers from one comparison domain") {
    const char *const sql[]={"SELECT i BETWEEN ? AND ?","SELECT ? BETWEEN i AND u",
      "SELECT ? BETWEEN 1 AND 1.5","SELECT ? BETWEEN ? AND ?",
      "SELECT i IN (?,?+1,CAST(? AS UNSIGNED))","SELECT ? IN (?,d,?)",
      "SELECT ? NOT IN ('a',?)","SELECT t IN (?,NULL)",
      "SELECT (?+?) IN (?,?)","SELECT ? BETWEEN (?+?) AND ?"};
    const size_t counts[]={2,1,1,3,3,3,2,1,4,4};
    const turbodb_value_kind_t expected[][4]={
      {TURBODB_VALUE_INT64,TURBODB_VALUE_INT64},
      {TURBODB_VALUE_INT64},{TURBODB_VALUE_INT64},
      {TURBODB_VALUE_TEXT,TURBODB_VALUE_TEXT,TURBODB_VALUE_TEXT},
      {TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,TURBODB_VALUE_UINT64},
      {TURBODB_VALUE_DOUBLE,TURBODB_VALUE_DOUBLE,TURBODB_VALUE_DOUBLE},
      {TURBODB_VALUE_TEXT,TURBODB_VALUE_TEXT},{TURBODB_VALUE_TEXT},
      {TURBODB_VALUE_DOUBLE,TURBODB_VALUE_DOUBLE,TURBODB_VALUE_DOUBLE,TURBODB_VALUE_DOUBLE},
      {TURBODB_VALUE_DOUBLE,TURBODB_VALUE_DOUBLE,TURBODB_VALUE_DOUBLE,TURBODB_VALUE_DOUBLE}};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse(sql[i]); metadata_open(); check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_OK);
      expect(expected[i],counts[i]); reset();
    }
  }
  it("derives searched and simple CASE condition and result domains") {
    const char *const sql[]={"SELECT CASE WHEN ? THEN i ELSE i END",
      "SELECT CASE WHEN ? THEN ? ELSE i END",
      "SELECT CASE ? WHEN 1 THEN ? WHEN 2 THEN ? ELSE ? END",
      "SELECT CASE ? WHEN i THEN d ELSE ? END",
      "SELECT CASE WHEN ? THEN ?+? ELSE d END",
      "SELECT CASE WHEN ? THEN CASE ? WHEN i THEN ? ELSE i END ELSE i END",
      "SELECT CASE WHEN ? THEN i WHEN ? THEN u ELSE ? END",
      "SELECT CASE ? WHEN ? THEN i ELSE i END"};
    const size_t counts[]={1,2,4,2,3,3,3,2};
    const turbodb_value_kind_t expected[][4]={
      {TURBODB_VALUE_BOOLEAN},
      {TURBODB_VALUE_BOOLEAN,TURBODB_VALUE_INT64},
      {TURBODB_VALUE_INT64,TURBODB_VALUE_TEXT,TURBODB_VALUE_TEXT,TURBODB_VALUE_TEXT},
      {TURBODB_VALUE_INT64,TURBODB_VALUE_DOUBLE},
      {TURBODB_VALUE_BOOLEAN,TURBODB_VALUE_DOUBLE,TURBODB_VALUE_DOUBLE},
      {TURBODB_VALUE_BOOLEAN,TURBODB_VALUE_INT64,TURBODB_VALUE_INT64},
      {TURBODB_VALUE_BOOLEAN,TURBODB_VALUE_BOOLEAN,TURBODB_VALUE_DOUBLE},
      {TURBODB_VALUE_TEXT,TURBODB_VALUE_TEXT}};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse(sql[i]); metadata_open(); check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_OK);
      expect(expected[i],counts[i]); reset();
    }
    parse("SELECT CASE WHEN ? THEN ? ELSE ? END"); metadata_open();
    check_equal(infer(0,&columns[1].type,DEPTH),TURBODB_STATUS_OK);
    const turbodb_value_kind_t context[]={TURBODB_VALUE_BOOLEAN,TURBODB_VALUE_UINT64,TURBODB_VALUE_UINT64};
    expect(context,3);
  }
  it("derives supported scalar function arguments from function and peer domains") {
    const char *const sql[]={"SELECT COALESCE(?,i,?)","SELECT IFNULL(?,d)",
      "SELECT COALESCE(?,?,?)","SELECT NULLIF(?,i)","SELECT NULLIF(?,?)",
      "SELECT COALESCE(i,u,?)","SELECT ABS(?)","SELECT SIGN(?)",
      "SELECT FLOOR(?)","SELECT CEILING(?)","SELECT MOD(?,i)","SELECT MOD(?,?)",
      "SELECT ROUND(?)","SELECT ROUND(?,?)","SELECT TRUNCATE(i,?)",
      "SELECT ABS(COALESCE(?,d))","SELECT COALESCE(ABS(?),d)"};
    const size_t counts[]={2,1,3,1,2,1,1,1,1,1,1,2,1,2,1,1,1};
    const turbodb_value_kind_t expected[][3]={
      {TURBODB_VALUE_INT64,TURBODB_VALUE_INT64},{TURBODB_VALUE_DOUBLE},
      {TURBODB_VALUE_TEXT,TURBODB_VALUE_TEXT,TURBODB_VALUE_TEXT},{TURBODB_VALUE_INT64},
      {TURBODB_VALUE_TEXT,TURBODB_VALUE_TEXT},{TURBODB_VALUE_DOUBLE},
      {TURBODB_VALUE_DOUBLE},{TURBODB_VALUE_DOUBLE},{TURBODB_VALUE_DOUBLE},
      {TURBODB_VALUE_DOUBLE},{TURBODB_VALUE_INT64},
      {TURBODB_VALUE_DOUBLE,TURBODB_VALUE_DOUBLE},{TURBODB_VALUE_DOUBLE},
      {TURBODB_VALUE_DOUBLE,TURBODB_VALUE_INT64},{TURBODB_VALUE_INT64},
      {TURBODB_VALUE_DOUBLE},{TURBODB_VALUE_DOUBLE}};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse(sql[i]); metadata_open(); check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_OK);
      expect(expected[i],counts[i]); reset();
    }
  }
  it("propagates an outer scalar context through selectable and numeric functions") {
    const char *const sql[]={"SELECT COALESCE(?,?)","SELECT NULLIF(?,?)","SELECT ABS(?)","SELECT ROUND(?,?)"};
    const size_t counts[]={2,2,1,2};
    const turbodb_value_kind_t expected[][2]={
      {TURBODB_VALUE_UINT64,TURBODB_VALUE_UINT64},
      {TURBODB_VALUE_UINT64,TURBODB_VALUE_UINT64},
      {TURBODB_VALUE_UINT64},
      {TURBODB_VALUE_UINT64,TURBODB_VALUE_INT64}};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse(sql[i]); metadata_open(); check_equal(infer(0,&columns[1].type,DEPTH),TURBODB_STATUS_OK);
      expect(expected[i],counts[i]); reset();
    }
  }
  it("does not use NULL literals as unknown types or evaluate dangerous known subexpressions") {
    const char *const sql[]={"SELECT NULL+?","SELECT ?+(9223372036854775807+1)","SELECT ?+(1.0/0.0)"};
    const turbodb_value_kind_t kinds[]={TURBODB_VALUE_DOUBLE,TURBODB_VALUE_INT64,TURBODB_VALUE_DOUBLE};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse(sql[i]); metadata_open(); check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_OK); expect(&kinds[i],1); reset();
    }
    parse("SELECT NULL"); metadata_open(); check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_OK); expect(NULL,0);
  }
  it("retains previously inferred metadata when final expression validation fails") {
    parse("SELECT ? AS a,?/? AS b"); metadata_open(); check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_OK);
    check_equal(infer(1,&columns[0].type,DEPTH),TURBODB_STATUS_UNSUPPORTED);
    check_true(*(const bool *)vec_at_const(&parameters.resolved,0));
    check_false(*(const bool *)vec_at_const(&parameters.resolved,1));
    check_false(*(const bool *)vec_at_const(&parameters.resolved,2));
    check_equal(infer(1,NULL,DEPTH),TURBODB_STATUS_OK);
    const turbodb_value_kind_t kinds[]={TURBODB_VALUE_TEXT,TURBODB_VALUE_DOUBLE,TURBODB_VALUE_DOUBLE}; expect(kinds,3);
  }
  it("rejects conflicting reinference while retaining a stable published view") {
    parse("SELECT ?"); metadata_open(); check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_OK);
    const orm_sql_type *view=NULL; size_t count=0;
    check_equal(orm_sql_parameters_types(&parameters,&view,&count,&error),TURBODB_STATUS_OK);
    check_equal(infer(0,&columns[0].type,DEPTH),TURBODB_STATUS_SQL_ERROR);
    check_equal(view[0].kind,TURBODB_VALUE_TEXT); check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_OK);
    const orm_sql_type *again=NULL;
    check_equal(orm_sql_parameters_types(&parameters,&again,&count,&error),TURBODB_STATUS_OK); check_true(again==view);
  }
  it("rejects unsupported AST scopes instead of guessing types") {
    const char *const sql[]={"SELECT UNKNOWN_FUNCTION(?)","SELECT ? IN(SELECT i FROM items)",
      "SELECT (SELECT ?)","SELECT ?/1","SELECT 'a' LIKE 'a' ESCAPE ?"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse(sql[i]); metadata_open(); check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_UNSUPPORTED); reset();
    }
    parse("SELECT ?+missing"); metadata_open(); check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_SQL_ERROR); reset();
    parse("SELECT ?=other.i"); metadata_open(); check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_SQL_ERROR);
  }
  it("rejects incompatible compound comparison domains without publishing candidates") {
    const char *const sql[]={"SELECT ? IN ('a',1)","SELECT ? BETWEEN TRUE AND 2.0",
      "SELECT CASE WHEN ? THEN 'a' ELSE 1 END",
      "SELECT CASE ? WHEN 'a' THEN 1 WHEN 2 THEN 2 ELSE 3 END",
      "SELECT CASE WHEN 1 THEN ? ELSE i END","SELECT COALESCE(?,TRUE,1)",
      "SELECT ROUND(?,1.5)"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse(sql[i]); metadata_open(); check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_UNSUPPORTED);
      const orm_sql_type *types=NULL; size_t count=OUTPUT_SENTINEL;
      check_equal(orm_sql_parameters_types(&parameters,&types,&count,&error),TURBODB_STATUS_INVALID_STATE);
      check_null(types); check_equal(count,(size_t)OUTPUT_SENTINEL); reset();
    }
  }
  it("rejects invalid admission and NULL inference context without allocation") {
    parse("SELECT ?"); metadata_open(); orm_sql_binding_scope local=scope();
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_sql_parameters_infer(NULL,&local,projection(0),NULL,DEPTH,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_sql_parameters_infer(&parameters,NULL,projection(0),NULL,DEPTH,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_sql_parameters_infer(&parameters,&local,0,NULL,DEPTH,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_sql_parameters_infer(&parameters,&local,(sqlparser_id)(sqlparser_node_count(document)+1),NULL,DEPTH,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(infer(0,NULL,0),TURBODB_STATUS_INVALID_ARGUMENT);
    const orm_sql_type null_context={TURBODB_VALUE_NULL,true};
    check_equal(infer(0,&null_context,DEPTH),TURBODB_STATUS_TYPE_ERROR);
    local.parameter_count=1;
    check_equal(orm_sql_parameters_infer(&parameters,&local,projection(0),NULL,DEPTH,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    local=scope(); local.query_count=1;
    check_equal(orm_sql_parameters_infer(&parameters,&local,projection(0),NULL,DEPTH,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    orm_sql_expr_query_binding query={.node=(sqlparser_id)(sqlparser_node_count(document)+1)};
    local.queries=&query;
    check_equal(orm_sql_parameters_infer(&parameters,&local,projection(0),NULL,DEPTH,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    orm_sql_expr_query_binding duplicate[]={
      {.node=projection(0)},{.node=projection(0)}};
    local.queries=duplicate; local.query_count=2;
    check_equal(orm_sql_parameters_infer(&parameters,&local,projection(0),NULL,DEPTH,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("enforces depth work steps and plan limits without publishing partial types") {
    parse("SELECT ?+(?+1)"); metadata_open(); check_equal(infer(0,NULL,2),TURBODB_STATUS_LIMIT_EXCEEDED);
    const orm_sql_budget_limits limits=budget.limits;
    const orm_sql_budget_resource resources[]={ORM_SQL_BUDGET_WORK_BYTES,ORM_SQL_BUDGET_EXECUTION_STEPS,ORM_SQL_BUDGET_PLAN_NODES};
    for(size_t i=0;i<sizeof(resources)/sizeof(resources[0]);++i) {
      budget.limits.statement.value[resources[i]]=budget.used.value[resources[i]];
      check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_LIMIT_EXCEEDED); budget.limits=limits;
      const orm_sql_type *view=NULL; size_t count=OUTPUT_SENTINEL;
      check_equal(orm_sql_parameters_types(&parameters,&view,&count,&error),TURBODB_STATUS_INVALID_STATE);
    }
    check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_OK);
  }
  it("refunds every injected reserve and resize failure in metadata and inference construction") {
    const char *const sql[]={"SELECT CAST(? AS SIGNED)+i","SELECT ?+(?+?)",
      "SELECT (t LIKE ?) AND (? OR NOT ?)","SELECT (i BETWEEN ? AND ?) AND t IN (?,?)",
      "SELECT CASE WHEN ? THEN ?+? ELSE d END","SELECT ROUND(COALESCE(?,d),?)"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse(sql[i]); reserves=resizes=0; metadata_open(); check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_OK);
      const size_t allocations[]={reserves,resizes}; reset();
      for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
        parse(sql[i]); reserves=resizes=0;
        if(pass==0) fail_reserve=point; else fail_resize=point;
        turbodb_status_t status=orm_sql_parameters_open(document,&budget,&parameters,&error);
        if(status==TURBODB_STATUS_OK) status=infer(0,NULL,DEPTH);
        check_equal(status,TURBODB_STATUS_OUT_OF_MEMORY); reset();
      }
    }
  }
  it("passes inferred types to the ordinary expression binder and accepts actual nullable values") {
    parse("SELECT ?/?"); metadata_open(); check_equal(infer(0,NULL,DEPTH),TURBODB_STATUS_OK);
    const orm_sql_type *types=NULL; size_t count=0;
    check_equal(orm_sql_parameters_types(&parameters,&types,&count,&error),TURBODB_STATUS_OK);
    orm_sql_binding_scope local=scope(); local.parameter_types=types;
    local.parameter_count=count; local.parameter_offsets=vec_data_const(&parameters.offsets);
    orm_sql_expr program={0}; vec_t slots={0}; size_t slot_bytes=0;
    const turbodb_status_t bound=orm_sql_bind_expression(&local,projection(0),DEPTH,
        (orm_sql_expression_target){.program=&program,.slots=&slots,.slot_bytes=&slot_bytes},false,&error);
    check_equal(bound,TURBODB_STATUS_OK);
    const turbodb_value_t values[]={turbodb_f64(3.0),turbodb_f64(2.0)};
    turbodb_value_t arguments[INPUTS]; check_true(program.input_count <= (size_t)INPUTS);
    for(size_t i=0;i<program.input_count;++i) arguments[i]=values[*(const size_t *)vec_at_const(&slots,i)-schema.count];
    turbodb_value_t result=turbodb_null();
    check_equal(orm_tidesdb_sql_expr_eval(&program,arguments,program.input_count,&result,&error),TURBODB_STATUS_OK);
    check_equal(result.kind,TURBODB_VALUE_DOUBLE); check_equal(result.data.double_value,1.5);
    for(size_t i=0;i<program.input_count;++i) arguments[i]=turbodb_null();
    check_equal(orm_tidesdb_sql_expr_eval(&program,arguments,program.input_count,&result,&error),TURBODB_STATUS_OK);
    check_equal(result.kind,TURBODB_VALUE_NULL); check_equal(types[0].kind,TURBODB_VALUE_DOUBLE);
    check_equal(orm_tidesdb_sql_expr_destroy(&program,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_work_release(&slots,slot_bytes,&budget,&error),TURBODB_STATUS_OK);
  }
}
