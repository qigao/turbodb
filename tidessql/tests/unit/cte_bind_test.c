#include "cte_bind.h"
#include <tinytest.h>
#include <string.h>

static size_t reserves,resizes,fail_reserve,fail_resize;
static stl_status bind_reserve(vec_t *v,size_t n) { return ++reserves==fail_reserve?STL_OUT_OF_MEMORY:vec_reserve(v,n); }
static stl_status bind_resize(vec_t *v,size_t n) { return ++resizes==fail_resize?STL_OUT_OF_MEMORY:vec_resize(v,n); }
#define vec_reserve bind_reserve
#define vec_resize bind_resize
#include "../../src/work.c"
#include "../../src/cte_bind.c"
#undef vec_reserve
#undef vec_resize

enum { STEPS=65536,WORK=4*1024*1024,DEPTH=32 };
static orm_tidesdb_sql_budget budget;
static orm_sql_budget_limits limits;
static sqlparser_document *document;
static orm_sql_query_scope scope;
static vec_t references;
static size_t reference_bytes;
static orm_sql_cte_shape shape;
static orm_sql_cte_schema bound_schema;
static orm_sql_select seed_plan,member_plan;
static turbodb_error_t error;
static const char sequence[]="WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<5) SELECT n FROM c";

static void clean(void) {
  fail_reserve=fail_resize=0;
  check_equal(orm_tidesdb_sql_select_destroy(&member_plan,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_select_destroy(&seed_plan,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_cte_schema_close(&bound_schema,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_cte_shape_close(&shape,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_work_release(&references,reference_bytes,&budget,&error),TURBODB_STATUS_OK); reference_bytes=0;
  sqlparser_document_destroy(document); document=NULL;
  check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
}
static void setup(void) {
  reserves=resizes=fail_reserve=fail_resize=0; tdsql_error_init(&error);
  limits=(orm_sql_budget_limits){0};
  for(size_t i=0;i<ORM_SQL_BUDGET_RESOURCE_COUNT;++i) limits.statement.value[i]=STEPS;
  limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=WORK;
  limits.transaction=(orm_sql_transaction_budget_amount){STEPS,STEPS,STEPS};
  check_equal(orm_tidesdb_sql_budget_init(&budget,&limits,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
}
static void reset(void) { clean(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); setup(); }
static void parse(const char *sql) {
  info("CTE shape SQL: %s",sql); sqlparser_error e;
  check_equal(sqlparser_parse(sql,strlen(sql),NULL,&document,&e),SQLPARSER_OK);
  scope=(orm_sql_query_scope){.document=document,.root=sqlparser_statements(document).first,.max_depth=DEPTH,.budget=&budget};
}
static sqlparser_id definition(void) { return sqlparser_get_node(document,scope.root)->as.with.bindings.first; }
static turbodb_status_t bind_shape(void) { return orm_sql_cte_shape_bind(&scope,definition(),&references,&shape,&error); }
static void seed_bind(void) {
  const sqlparser_node *cte=sqlparser_get_node(document,definition());
  sqlparser_id root=cte->as.cte.query;
  const sqlparser_node *query=sqlparser_get_node(document,root);
  if(query->kind==SQLPARSER_UNION) root=query->as.compound.left;
  const orm_sql_schema_column column={vstr_from_cstr("unit"),{TURBODB_VALUE_BOOLEAN,false}};
  const orm_sql_table_schema unit={vstr_from_cstr("unit"),&column,1};
  orm_sql_query_scope child=scope; child.root=root; child.unit_input=true;
  child.anonymous_output=cte->as.cte.columns.count!=0;
  check_equal(orm_tidesdb_sql_select_bind_at(&child,&unit,NULL,&seed_plan,&error),TURBODB_STATUS_OK);
}
static turbodb_status_t schema_bind(bool recursive) {
  return orm_sql_cte_schema_bind(&scope,definition(),&seed_plan,recursive,&bound_schema,&error);
}
static void member_bind(const char *sql) {
  sqlparser_document *doc=NULL; sqlparser_error e;
  check_equal(sqlparser_parse(sql,strlen(sql),NULL,&doc,&e),SQLPARSER_OK);
  check_equal(orm_tidesdb_sql_select_bind(doc,&bound_schema.schema,DEPTH,&budget,&member_plan,&error),TURBODB_STATUS_OK);
  sqlparser_document_destroy(doc);
}
static void resolve(const char *sql) { parse(sql); check_equal(orm_sql_cte_resolve(&scope,&references,&reference_bytes,&error),TURBODB_STATUS_OK); }
static size_t count_refs(sqlparser_id cte,bool own) {
  const sqlparser_node *query=sqlparser_get_node(document,sqlparser_get_node(document,cte)->as.cte.query);
  size_t count=0;
  for(size_t i=0;i<vec_size(&references);++i)
    if(*(const sqlparser_id *)vec_at_const(&references,i)==cte &&
        (!own || cte_bind_inside(sqlparser_get_node(document,(sqlparser_id)i+1),query))) ++count;
  return count;
}
static const orm_sql_cte_shape_node *recursive_member(void) {
  for(size_t i=0;i<shape.count;++i) {
    const orm_sql_cte_shape_node *node=vec_at_const(&shape.nodes,i);
    if(node->self) return node;
  }
  return NULL;
}
static void reject(const char *sql,turbodb_status_t expected,const char *reason) {
  resolve(sql); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
  check_equal(bind_shape(),expected); check_contains(error.message,reason); check_null(shape.budget);
  check_false(shape.nodes.initialized); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
}

spec("TidesDB recursive CTE lexical and structural binding") {
  before_each() { setup(); }
  after_each() { clean(); check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK); }
  it("owns seed names and types independently of AST and seed plan lifetimes") {
    parse("WITH RECURSIVE c AS (SELECT 1 AS n,'seed' AS label UNION ALL SELECT n+1,label FROM c) SELECT n FROM c");
    seed_bind(); check_equal(schema_bind(true),TURBODB_STATUS_OK);
    check_equal(bound_schema.schema.count,2u); check_equal(bound_schema.schema.columns[0].type.kind,TURBODB_VALUE_INT64);
    check_equal(bound_schema.schema.columns[1].type.kind,TURBODB_VALUE_TEXT);
    check_true(bound_schema.schema.columns[1].name.data!=orm_tidesdb_sql_select_column_at(&seed_plan,1)->name);
    check_true(bound_schema.schema.columns[0].type.nullable); check_true(bound_schema.schema.columns[1].type.nullable);
    check_equal(orm_tidesdb_sql_select_destroy(&seed_plan,&error),TURBODB_STATUS_OK);
    sqlparser_document_destroy(document); document=NULL;
    check_equal(memcmp(bound_schema.schema.name.data,"c",1),0);
    check_equal(memcmp(bound_schema.schema.columns[1].name.data,"label",5),0);
    member_bind("SELECT n+1 AS later,label AS value FROM c");
    check_equal(orm_sql_cte_schema_member(&bound_schema,&member_plan,&error),TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u);
  }
  it("preserves seed nullability for ordinary CTEs and defaults to seed output names") {
    parse("WITH c AS (SELECT 1 AS n,NULL AS absent) SELECT n FROM c"); seed_bind();
    check_equal(schema_bind(false),TURBODB_STATUS_OK); check_false(bound_schema.schema.columns[0].type.nullable);
    check_equal(bound_schema.schema.columns[1].type.kind,TURBODB_VALUE_NULL); check_true(bound_schema.schema.columns[1].type.nullable);
    check_equal(memcmp(bound_schema.schema.columns[0].name.data,"n",1),0);
    check_equal(orm_sql_cte_schema_close(&bound_schema,&error),TURBODB_STATUS_OK);
    check_equal(schema_bind(true),TURBODB_STATUS_OK); check_true(bound_schema.schema.columns[0].type.nullable);
  }
  it("uses explicit CTE column names for recursive references instead of seed labels") {
    parse("WITH RECURSIVE c(a,b) AS (SELECT 1 AS original,NULL AS missing UNION ALL SELECT a+1,b FROM c) SELECT a FROM c");
    seed_bind(); check_equal(schema_bind(true),TURBODB_STATUS_OK);
    check_equal(bound_schema.schema.columns[0].name.len,1u); check_equal(bound_schema.schema.columns[0].name.data[0],'a');
    check_equal(bound_schema.schema.columns[1].name.len,1u); check_equal(bound_schema.schema.columns[1].name.data[0],'b');
    member_bind("SELECT a+1 AS later,b AS absent FROM c");
    check_equal(orm_sql_cte_schema_member(&bound_schema,&member_plan,&error),TURBODB_STATUS_OK);
  }
  it("checks member positions and accepts NULL without widening seed types") {
    parse("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n FROM c) SELECT n FROM c");
    seed_bind(); check_equal(schema_bind(true),TURBODB_STATUS_OK);
    member_bind("SELECT NULL AS renamed FROM c");
    check_equal(orm_sql_cte_schema_member(&bound_schema,&member_plan,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_select_destroy(&member_plan,&error),TURBODB_STATUS_OK);
    member_bind("SELECT 'wrong' AS n FROM c");
    check_equal(orm_sql_cte_schema_member(&bound_schema,&member_plan,&error),TURBODB_STATUS_TYPE_ERROR);
    check_equal(bound_schema.schema.columns[0].type.kind,TURBODB_VALUE_INT64);
    check_equal(orm_tidesdb_sql_select_destroy(&member_plan,&error),TURBODB_STATUS_OK);
    member_bind("SELECT n,n+1 AS extra FROM c");
    check_equal(orm_sql_cte_schema_member(&bound_schema,&member_plan,&error),TURBODB_STATUS_SQL_ERROR);
  }
  it("does not infer a non-NULL seed kind from recursive output") {
    parse("WITH RECURSIVE c(n) AS (SELECT NULL UNION ALL SELECT 1 FROM c) SELECT n FROM c");
    seed_bind(); check_equal(schema_bind(true),TURBODB_STATUS_OK); member_bind("SELECT 1 AS n FROM c");
    check_equal(orm_sql_cte_schema_member(&bound_schema,&member_plan,&error),TURBODB_STATUS_TYPE_ERROR);
    check_equal(bound_schema.schema.columns[0].type.kind,TURBODB_VALUE_NULL);
  }
  it("validates explicit CTE names and widths before publishing a schema") {
    const char *sql[]={"WITH c(n,n) AS (SELECT 1,2) SELECT n FROM c","WITH c(n) AS (SELECT 1,2) SELECT n FROM c"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      if(i) reset(); parse(sql[i]); seed_bind(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(schema_bind(true),TURBODB_STATUS_SQL_ERROR); check_null(bound_schema.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("rejects occupied schema storage and a definition outside its scope") {
    parse(sequence); seed_bind(); check_equal(schema_bind(true),TURBODB_STATUS_OK);
    const void *columns=bound_schema.schema.columns;
    check_equal(schema_bind(true),TURBODB_STATUS_INVALID_ARGUMENT); check_true(bound_schema.schema.columns==columns);
    check_equal(orm_sql_cte_schema_close(&bound_schema,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_schema_bind(&scope,scope.root,&seed_plan,true,&bound_schema,&error),TURBODB_STATUS_INVALID_ARGUMENT);
  }
  it("refunds every seed schema allocation failure") {
    parse(sequence); seed_bind(); reserves=resizes=0; check_equal(schema_bind(true),TURBODB_STATUS_OK);
    const size_t counts[]={reserves,resizes}; check_equal(orm_sql_cte_schema_close(&bound_schema,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=counts[pass];++point) {
      reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(schema_bind(true),TURBODB_STATUS_OUT_OF_MEMORY); check_null(bound_schema.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); fail_reserve=fail_resize=0;
    }
  }
  it("rejects every seed schema step boundary and member validation exhaustion") {
    parse(sequence); seed_bind(); const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(schema_bind(true),TURBODB_STATUS_OK); const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    check_equal(orm_sql_cte_schema_close(&bound_schema,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(uint64_t point=0;point<steps;++point) {
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      check_equal(schema_bind(true),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(bound_schema.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=STEPS;
    check_equal(schema_bind(true),TURBODB_STATUS_OK); member_bind("SELECT n+1 AS n FROM c");
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_sql_cte_schema_member(&bound_schema,&member_plan,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
  }
  it("preserves recursive lexical references while the runtime admission entry still rejects them") {
    parse(sequence);
    check_equal(orm_sql_cte_bind(&scope,&references,&reference_bytes,&error),TURBODB_STATUS_UNSUPPORTED);
    check_false(references.initialized); check_equal(reference_bytes,0u);
    check_equal(orm_sql_cte_resolve(&scope,&references,&reference_bytes,&error),TURBODB_STATUS_OK);
    check_equal(count_refs(definition(),true),1u); check_equal(count_refs(definition(),false),2u);
    check_equal(bind_shape(),TURBODB_STATUS_OK); check_equal(shape.initial_members,1u); check_equal(shape.recursive_members,1u);
    check_equal(shape.count,3u); const orm_sql_cte_shape_node *root=vec_at_const(&shape.nodes,0);
    check_equal(root->parts,ORM_SQL_CTE_MIXED_PARTS); check_equal(root->parent,SIZE_MAX);
    const orm_sql_cte_shape_node *member=recursive_member(); check_not_null(member); check_equal(member->union_before,root->ast);
    check_equal(sqlparser_get_node(document,member->self)->kind,SQLPARSER_TABLE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WRITE_BYTES],0u);
  }
  it("does not create a recursive reference without RECURSIVE or from a later definition") {
    resolve("WITH a AS (SELECT n FROM a), b AS (SELECT n FROM later), later AS (SELECT 1 AS n) SELECT n FROM a");
    check_equal(count_refs(definition(),true),0u); check_equal(bind_shape(),TURBODB_STATUS_OK); check_equal(shape.recursive_members,0u);
    const sqlparser_node *a=sqlparser_get_node(document,definition());
    check_equal(count_refs(sqlparser_get_node(document,a->next)->next,false),0u);
  }
  it("resolves the nearest nested CTE instead of mistaking its table for an outer self reference") {
    resolve("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT d.n FROM (WITH c(n) AS (SELECT 2) SELECT n FROM c) d) SELECT n FROM c");
    check_equal(count_refs(definition(),true),0u); check_equal(bind_shape(),TURBODB_STATUS_OK);
    check_equal(shape.initial_members,2u); check_equal(shape.recursive_members,0u);
  }
  it("keeps earlier sibling and outer scope references independent of self references") {
    resolve("WITH RECURSIVE a(n) AS (SELECT 1), c(n) AS (SELECT n FROM a UNION ALL SELECT c.n+1 FROM c JOIN a ON c.n=a.n) SELECT n FROM c");
    const sqlparser_id c=sqlparser_get_node(document,definition())->next;
    check_equal(count_refs(definition(),true),0u); check_equal(count_refs(c,true),1u);
    check_equal(orm_sql_cte_shape_bind(&scope,c,&references,&shape,&error),TURBODB_STATUS_OK);
    check_equal(shape.initial_members,1u); check_equal(shape.recursive_members,1u);
  }
  it("supports multiple initial and recursive query blocks without flattening their AST identities") {
    resolve("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT n+1 FROM c WHERE n<3 UNION ALL SELECT n+2 FROM c WHERE n<2) SELECT n FROM c");
    check_equal(bind_shape(),TURBODB_STATUS_OK); check_equal(shape.initial_members,2u); check_equal(shape.recursive_members,2u);
    check_equal(shape.count,7u); size_t previous=0;
    for(size_t i=0;i<shape.count;++i) {
      const orm_sql_cte_shape_node *node=vec_at_const(&shape.nodes,i);
      if(node->left!=SIZE_MAX) continue;
      const sqlparser_node *ast=sqlparser_get_node(document,node->ast);
      check_true(ast->span.offset>=previous); previous=ast->span.offset;
    }
  }
  it("preserves whole-query LIMIT markers and their original source offsets") {
    resolve("WITH RECURSIVE c(n) AS (SELECT ? UNION ALL SELECT n+? FROM c LIMIT ? OFFSET ?) SELECT n FROM c");
    check_equal(bind_shape(),TURBODB_STATUS_OK);
    const sqlparser_node *root=sqlparser_get_node(document,((const orm_sql_cte_shape_node *)vec_at_const(&shape.nodes,0))->ast);
    check_equal(root->kind,SQLPARSER_UNION); check_true(root->as.compound.limit!=0);
    const sqlparser_node *limit=sqlparser_get_node(document,root->as.compound.limit);
    const sqlparser_node *count=sqlparser_get_node(document,limit->as.limit.count),*offset=sqlparser_get_node(document,limit->as.limit.offset);
    check_equal(count->kind,SQLPARSER_PARAMETER); check_equal(offset->kind,SQLPARSER_PARAMETER); check_true(count->span.offset<offset->span.offset);
    check_equal(sqlparser_get_node(document,recursive_member()->ast)->as.select.limit,0u);
  }
  it("allows seed aggregation sorting and independent aggregate subqueries in recursive members") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS ((SELECT MAX(id) AS n FROM items ORDER BY n LIMIT 1) UNION ALL SELECT n+1 FROM c WHERE n<5) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+(SELECT MAX(id) FROM items) FROM c WHERE n<5) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT c.n FROM c JOIN (SELECT MAX(id) AS n FROM items) d ON c.n=d.n) SELECT n FROM c"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) { if(i) reset(); resolve(sql[i]); check_equal(bind_shape(),TURBODB_STATUS_OK); check_equal(shape.recursive_members,1u); }
  }
  it("allows recursive references on preserved join sides and within inner cross joins") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT c.n FROM c LEFT JOIN items i ON c.n=i.id) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT c.n FROM items i RIGHT JOIN c ON c.n=i.id) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT c.n FROM items i JOIN c ON c.n=i.id LEFT JOIN items j ON j.id=c.n) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT c.n FROM items i CROSS JOIN c) SELECT n FROM c"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) { if(i) reset(); resolve(sql[i]); check_equal(bind_shape(),TURBODB_STATUS_OK); }
  }
  it("rejects missing initial members or initial blocks following recursion") {
    reject("WITH RECURSIVE c(n) AS (SELECT n FROM c) SELECT n FROM c",TURBODB_STATUS_SQL_ERROR,"initial query"); reset();
    reject("WITH RECURSIVE c(n) AS (SELECT n FROM c UNION ALL SELECT 1) SELECT n FROM c",TURBODB_STATUS_SQL_ERROR,"initial query"); reset();
    reject("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n FROM c UNION ALL SELECT 2) SELECT n FROM c",TURBODB_STATUS_SQL_ERROR,"precede");
  }
  it("rejects repeated references within a recursive member") {
    reject("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT a.n FROM c a JOIN c b ON a.n=b.n) SELECT n FROM c",TURBODB_STATUS_SQL_ERROR,"exactly one");
  }
  it("rejects self references hidden inside scalar IN EXISTS derived or nested WITH queries") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT (SELECT n FROM c)) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT id FROM items WHERE id IN (SELECT n FROM c)) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT id FROM items WHERE EXISTS(SELECT n FROM c)) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT d.n FROM (SELECT n FROM c) d) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (WITH d AS (SELECT n FROM c) SELECT 1 UNION ALL SELECT n FROM c) SELECT n FROM c"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) { if(i) reset(); reject(sql[i],TURBODB_STATUS_SQL_ERROR,"subquery"); }
  }
  it("rejects recursive references on either spelling of the null-extended join side") {
    reject("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT c.n FROM items i LEFT JOIN c ON c.n=i.id) SELECT n FROM c",TURBODB_STATUS_SQL_ERROR,"null-extended"); reset();
    reject("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT c.n FROM c RIGHT JOIN items i ON c.n=i.id) SELECT n FROM c",TURBODB_STATUS_SQL_ERROR,"null-extended");
  }
  it("rejects current-block aggregates windows and grouping while leaving nested scopes opaque") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT COUNT(*) FROM c) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n FROM c WHERE SUM(n)>0) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n FROM c HAVING MAX(n)>0) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT ROW_NUMBER() OVER () FROM c) SELECT n FROM c"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) { if(i) reset(); reject(sql[i],TURBODB_STATUS_SQL_ERROR,"aggregate and window"); }
    reset(); reject("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n FROM c GROUP BY n) SELECT n FROM c",TURBODB_STATUS_SQL_ERROR,"GROUP BY");
    reset(); reject("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT DISTINCT n FROM c) SELECT n FROM c",TURBODB_STATUS_SQL_ERROR,"DISTINCT");
  }
  it("rejects recursive member pagination and CTE ordering while retaining outer query ordering") {
    reject("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL (SELECT n FROM c LIMIT 2)) SELECT n FROM c",TURBODB_STATUS_UNSUPPORTED,"complete recursive CTE"); reset();
    reject("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n FROM c ORDER BY n) SELECT n FROM c",TURBODB_STATUS_UNSUPPORTED,"ORDER BY"); reset();
    resolve("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3) SELECT n FROM c ORDER BY n LIMIT 1"); check_equal(bind_shape(),TURBODB_STATUS_OK);
  }
  it("supports recursive UNION DISTINCT but rejects a trailing ALL after its last recursive DISTINCT") {
    resolve("WITH RECURSIVE c(n) AS (SELECT 1 UNION SELECT n FROM c) SELECT n FROM c"); check_equal(bind_shape(),TURBODB_STATUS_OK); reset();
    resolve("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n FROM c UNION DISTINCT SELECT n+1 FROM c) SELECT n FROM c"); check_equal(bind_shape(),TURBODB_STATUS_OK); reset();
    reject("WITH RECURSIVE c(n) AS (SELECT 1 UNION SELECT n FROM c UNION ALL SELECT n+1 FROM c) SELECT n FROM c",TURBODB_STATUS_UNSUPPORTED,"DISTINCT cannot be followed"); reset();
    resolve("WITH RECURSIVE c(n) AS (SELECT 1 UNION SELECT 2 UNION ALL SELECT n FROM c) SELECT n FROM c"); check_equal(bind_shape(),TURBODB_STATUS_OK);
  }
  it("preserves flattenable right nesting and rejects mixed recursive right nesting") {
    resolve("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL (SELECT n FROM c UNION ALL SELECT n+1 FROM c)) SELECT n FROM c");
    check_equal(bind_shape(),TURBODB_STATUS_OK); check_equal(shape.recursive_members,2u); reset();
    reject("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL (SELECT n FROM c UNION DISTINCT SELECT n+1 FROM c)) SELECT n FROM c",TURBODB_STATUS_UNSUPPORTED,"right-nested");
  }
  it("bounds query depth and preserves prior output on occupied-input rejection") {
    resolve(sequence); scope.max_depth=1; check_equal(bind_shape(),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(shape.budget);
    scope.max_depth=DEPTH; check_equal(bind_shape(),TURBODB_STATUS_OK); const size_t count=shape.count;
    check_equal(bind_shape(),TURBODB_STATUS_INVALID_ARGUMENT); check_equal(shape.count,count);
    check_equal(orm_sql_cte_shape_close(&shape,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_shape_bind(&scope,scope.root,&references,&shape,&error),TURBODB_STATUS_INVALID_ARGUMENT);
  }
  it("refunds every allocation failure in reference resolution and query shape construction") {
    resolve(sequence); check_equal(bind_shape(),TURBODB_STATUS_OK); const size_t calls[]={reserves,resizes};
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=calls[pass];++point) {
      reset(); parse(sequence); if(pass) fail_resize=point; else fail_reserve=point;
      turbodb_status_t status=orm_sql_cte_resolve(&scope,&references,&reference_bytes,&error);
      if(status==TURBODB_STATUS_OK) status=bind_shape();
      check_equal(status,TURBODB_STATUS_OUT_OF_MEMORY); check_null(shape.budget); clean();
    }
  }
  it("bounds each construction step and restores all workspace after failure") {
    resolve(sequence); check_equal(bind_shape(),TURBODB_STATUS_OK); const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    for(uint64_t point=0;point<steps;++point) {
      reset(); parse(sequence); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=point;
      turbodb_status_t status=orm_sql_cte_resolve(&scope,&references,&reference_bytes,&error);
      if(status==TURBODB_STATUS_OK) status=bind_shape();
      check_equal(status,TURBODB_STATUS_LIMIT_EXCEEDED); check_null(shape.budget); clean();
    }
  }
}
