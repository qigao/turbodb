#include "runtime.h"
#include "cte_store.h"
#include "cte_query.h"
#include "subquery.h"
#include "work.h"
#include <tinytest.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static size_t reserves,resizes,fail_reserve,fail_resize;
static stl_status probe_reserve(vec_t *v,size_t n) { return ++reserves==fail_reserve?STL_OUT_OF_MEMORY:vec_reserve(v,n); }
static stl_status probe_resize(vec_t *v,size_t n) { return ++resizes==fail_resize?STL_OUT_OF_MEMORY:vec_resize(v,n); }
#define vec_reserve probe_reserve
#define vec_resize probe_resize
#include "../../src/work.c"
#undef vec_reserve
#undef vec_resize

enum { RECORD=4096,WORK=4*1024*1024,LIMIT=10000000,DEPTH=32,MEMBERS=2,PARAMETERS=4,NATIVE_INPUTS=3,GRAPH_FAULT_PHASES=3 };
static char *directory;
static orm_tidesdb_database_t *database;
static orm_tidesdb_column_family_t *family;
static orm_tidesdb_sql_budget budget;
static orm_sql_catalog_store owner;
static turbodb_error_t error;
static sqlparser_document *document;
static orm_sql_query_scope scope;
static sqlparser_id definition;
static vec_t references;
static size_t reference_bytes;
static orm_sql_cte_shape shape;
static orm_sql_cte_schema schema;
static orm_sql_compound initial,recursive;
static orm_sql_cte_store store;
static orm_sql_cte_query owned;
static orm_sql_diagnostics evaluation_diagnostics;
static orm_sql_query external;
static orm_sql_query graph_root;
static orm_sql_dependencies graph;
static orm_sql_lateral_schema lateral_frame;
static orm_sql_lateral_query lateral_query;
static orm_sql_relation_source native_inputs[NATIVE_INPUTS];
static orm_sql_from native_from;
static orm_sql_from_run native_run;
static orm_sql_select native_select;
static orm_sql_select_run native_selected;
static orm_sql_join lateral_join;
static orm_sql_expr lateral_condition;
static orm_sql_scan lateral_consumer;
static orm_sql_subquery external_scalar;
static orm_sql_row_source external_source;
static orm_sql_type external_type;
static struct { size_t opens,closes; bool fail_open,fail_close; } hooks;
static orm_sql_cte_reader reader;
static orm_sql_derived_binding bindings[MEMBERS];
static struct { orm_sql_row_source source; orm_sql_cte_frontier_reader reader; } proxies[MEMBERS];
static turbodb_value_t parameters[PARAMETERS];
static orm_sql_type parameter_types[PARAMETERS],initial_type,recursive_type;
static orm_sql_row_source initial_source,recursive_source;
static size_t member_count,opens,closes;
static const char sequence[]="WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3) SELECT n FROM c";
static const char multiple[]="WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT 10 UNION ALL SELECT n+1 FROM c WHERE n<3 UNION ALL SELECT n+10 FROM c WHERE n<3) SELECT n FROM c";

static void parse_scope(const char *sql,size_t count) {
  info("CTE plan SQL: %s",sql); sqlparser_error e;
  check_equal(sqlparser_parse(sql,strlen(sql),NULL,&document,&e),SQLPARSER_OK);
  scope=(orm_sql_query_scope){.document=document,.root=sqlparser_statements(document).first,
      .max_depth=DEPTH,.budget=&budget,.parameter_count=count,.parameter_types=parameter_types,.anonymous_output=true};
}
static sqlparser_id lateral_named(const char *alias) {
  sqlparser_id result=0;
  for(size_t i=1;i<=sqlparser_node_count(document);++i) {
    const sqlparser_node *node=sqlparser_get_node(document,(sqlparser_id)i);
    if(node->kind!=SQLPARSER_TABLE||!node->as.table.lateral) continue;
    const sqlparser_node *name=sqlparser_get_node(document,node->as.table.alias);
    if(name&&name->span.length==strlen(alias)&&
        !memcmp(sqlparser_text(document,name->span),alias,name->span.length)) result=(sqlparser_id)i;
  }
  check_not_equal(result,0u); return result;
}
static void parse(const char *sql,size_t count) {
  parse_scope(sql,count);
  definition=sqlparser_get_node(document,scope.root)->as.with.bindings.first;
  check_equal(orm_sql_cte_resolve(&scope,&references,&reference_bytes,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_cte_shape_bind(&scope,definition,&references,&shape,&error),TURBODB_STATUS_OK);
}
static turbodb_status_t part_open(orm_sql_cte_parts part,bool explain) {
  orm_sql_query_scope child=scope; child.root=sqlparser_get_node(document,definition)->as.cte.query;
  return orm_sql_compound_open_cte_part(&child,&shape,part,&owner,parameters,explain,NULL,
      part==ORM_SQL_CTE_INITIAL_PART?&initial:&recursive,&error);
}
static turbodb_status_t proxy_pull(void *context,const turbodb_value_t **out,turbodb_error_t *e) {
  orm_sql_cte_frontier_reader *input=context;
  check_not_null(input->store); return input->source.next(input->source.context,out,e);
}
static void bind_schema(bool explain) {
  check_equal(orm_sql_cte_schema_bind(&scope,definition,initial.plan,true,&schema,&error),TURBODB_STATUS_OK);
  member_count=0;
  for(size_t i=0;i<shape.count;++i) {
    const orm_sql_cte_shape_node *node=vec_at_const(&shape.nodes,i); if(!node->self) continue;
    check_true(member_count<MEMBERS);
    proxies[member_count].source=(orm_sql_row_source){&budget,vec_data_const(&schema.types),schema.schema.count,
        &proxies[member_count].reader,proxy_pull,false};
    bindings[member_count]=(orm_sql_derived_binding){node->self,&schema.schema,explain?NULL:&proxies[member_count].source};
    if(member_count) check_true(bindings[member_count-1].node<node->self);
    ++member_count;
  }
  scope.derived=bindings; scope.derived_count=member_count;
}
static turbodb_status_t compound_pull(void *context,const turbodb_value_t **out,turbodb_error_t *e) {
  orm_sql_compound *plan=context; orm_sql_scan_row row;
  const turbodb_status_t status=orm_tidesdb_sql_scan_next(plan->scan,&row,e);
  if(status==TURBODB_STATUS_OK) *out=row.state==ORM_SQL_SCAN_ROW?row.values:NULL;
  return status;
}
static turbodb_status_t round_open(void *context,orm_sql_row_source *frontier,orm_sql_row_source **out,turbodb_error_t *e) {
  check_true(context==&recursive); ++opens;
  turbodb_status_t status=TURBODB_STATUS_OK;
  for(size_t i=0;status==TURBODB_STATUS_OK && i<member_count;++i)
    status=orm_sql_cte_frontier_open(frontier,&proxies[i].reader,e);
  if(status==TURBODB_STATUS_OK) status=orm_sql_compound_execution_open(&recursive,parameters,scope.parameter_count,NULL,e);
  if(status==TURBODB_STATUS_OK) *out=&recursive_source;
  return status;
}
static turbodb_status_t round_close(void *context,turbodb_error_t *e) {
  check_true(context==&recursive); ++closes;
  turbodb_status_t status=orm_sql_compound_execution_close(&recursive,e);
  for(size_t i=0;status==TURBODB_STATUS_OK && i<member_count;++i) status=orm_sql_cte_frontier_close(&proxies[i].reader,e);
  return status;
}
static void free_ast(void) {
  check_equal(orm_sql_cte_shape_close(&shape,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_work_release(&references,reference_bytes,&budget,&error),TURBODB_STATUS_OK); reference_bytes=0;
  sqlparser_document_destroy(document); document=NULL;
}
static void open_store(bool distinct) {
  check_equal(orm_sql_cte_schema_member(&schema,recursive.plan,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_compound_execution_close(&recursive,&error),TURBODB_STATUS_OK);
  initial_type=orm_tidesdb_sql_select_column_at(initial.plan,0)->type;
  recursive_type=orm_tidesdb_sql_select_column_at(recursive.plan,0)->type;
  initial_source=(orm_sql_row_source){&budget,&initial_type,1,&initial,compound_pull,false};
  recursive_source=(orm_sql_row_source){&budget,&recursive_type,1,&recursive,compound_pull,false};
  const orm_sql_cte_recursion spec={&recursive,round_open,round_close,DEPTH,distinct};
  check_equal(orm_sql_cte_store_open_recursive(&initial_source,&spec,&store,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_cte_reader_open(&store,&reader,&error),TURBODB_STATUS_OK);
  check_equal(opens,0u); free_ast();
}
static void values(const int64_t *expected,size_t count) {
  orm_sql_row_source *source=orm_sql_cte_reader_source(&reader);
  for(size_t i=0;i<count;++i) {
    const turbodb_value_t *row=NULL; check_equal(source->next(source->context,&row,&error),TURBODB_STATUS_OK);
    check_not_null(row); check_equal(row[0].kind,TURBODB_VALUE_INT64); check_equal(row[0].data.int64_value,expected[i]);
  }
  const turbodb_value_t *row=NULL; check_equal(source->next(source->context,&row,&error),TURBODB_STATUS_OK); check_null(row);
}
static void clean_plans(void) {
  fail_reserve=fail_resize=0;
  check_equal(orm_tidesdb_sql_scan_close(&lateral_consumer,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_select_close(&native_selected,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_select_destroy(&native_select,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_from_close(&native_run,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_from_destroy(&native_from,&error),TURBODB_STATUS_OK);
  for(size_t i=0;i<NATIVE_INPUTS;++i) check_equal(orm_tidesdb_sql_relation_close(&native_inputs[i],&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_join_close(&lateral_join,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_lateral_query_close(&lateral_query,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_expr_destroy(&lateral_condition,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_runtime_lateral_schema_close(&lateral_frame,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_runtime_close(&graph_root,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_dependencies_close(&graph,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_cte_reader_close(&reader,&error),TURBODB_STATUS_OK);
  hooks.fail_close=false;
  check_equal(orm_sql_cte_query_close(&owned,&error),TURBODB_STATUS_OK);
  orm_sql_diagnostics_destroy(&evaluation_diagnostics);
  check_equal(orm_tidesdb_sql_subquery_close(&external_scalar,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_runtime_close(&external,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_cte_store_close(&store,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_compound_close(&recursive,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_compound_close(&initial,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_cte_schema_close(&schema,&error),TURBODB_STATUS_OK); free_ast();
  check_equal(owner.active_sources,0u);
}
static void rewind_initial(void) {
  check_equal(orm_sql_compound_execution_close(&initial,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_compound_execution_open(&initial,parameters,scope.parameter_count,NULL,&error),TURBODB_STATUS_OK);
}
static orm_sql_scan_row initial_next(void) {
  orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_scan_next(initial.scan,&row,&error),TURBODB_STATUS_OK); return row;
}
static void execute(const char *sql) {
  sqlparser_document *doc=NULL; sqlparser_error e; size_t affected=0;
  check_equal(sqlparser_parse(sql,strlen(sql),NULL,&doc,&e),SQLPARSER_OK);
  check_equal(orm_tidesdb_sql_runtime_execute(doc,&owner,NULL,0,DEPTH,0,false,NULL,&affected,&error),TURBODB_STATUS_OK);
  sqlparser_document_destroy(doc);
}
static orm_sql_cte_query_spec owner_spec(void) { return (orm_sql_cte_query_spec){.max_iterations=DEPTH}; }
static turbodb_status_t owner_open(orm_sql_cte_query_spec spec) {
  return orm_sql_cte_query_open(&scope,definition,&references,&owner,parameters,&spec,&owned,&error);
}
static void owner_reader(void) {
  check_equal(orm_sql_cte_reader_open(&owned.cache,&reader,&error),TURBODB_STATUS_OK); free_ast();
}
static turbodb_status_t external_pull(void *context,const turbodb_value_t **out,turbodb_error_t *e) {
  orm_sql_scan_row row; const turbodb_status_t status=orm_tidesdb_sql_runtime_next(context,&row,e);
  if(status==TURBODB_STATUS_OK) *out=row.state==ORM_SQL_SCAN_ROW?row.values:NULL;
  return status;
}
static turbodb_status_t inputs_open(void *context,const turbodb_value_t *params,size_t count,turbodb_error_t *e) {
  check_true(context==&hooks); ++hooks.opens; (void)params; (void)count;
  if(hooks.fail_open) { tdsql_error_set(e,TURBODB_STATUS_LIMIT_EXCEEDED,"test round dependency failure"); return TURBODB_STATUS_LIMIT_EXCEEDED; }
  if(!external.owner) return TURBODB_STATUS_OK;
  turbodb_status_t status=orm_sql_runtime_execution_close(&external,e);
  if(status==TURBODB_STATUS_OK) status=orm_sql_runtime_execution_resume(&external,e);
  return status;
}
static turbodb_status_t inputs_close(void *context,turbodb_error_t *e) {
  check_true(context==&hooks); ++hooks.closes;
  if(hooks.fail_close) { tdsql_error_set(e,TURBODB_STATUS_BUSY,"test round dependency close busy"); return TURBODB_STATUS_BUSY; }
  return external.owner?orm_sql_runtime_execution_close(&external,e):TURBODB_STATUS_OK;
}
static void open_external(const char *sql) {
  sqlparser_document *doc=NULL; sqlparser_error e;
  check_equal(sqlparser_parse(sql,strlen(sql),NULL,&doc,&e),SQLPARSER_OK);
  check_equal(orm_tidesdb_sql_runtime_open(doc,&owner,vstr_from_cstr("app"),NULL,0,DEPTH,&external,&error),TURBODB_STATUS_OK);
  sqlparser_document_destroy(doc);
}
static turbodb_status_t graph_open(uint64_t iterations) {
  turbodb_status_t status=orm_sql_dependencies_open_recursive(&scope,&owner,parameters,false,iterations,&graph,&error);
  orm_sql_query_scope root=scope; root.anonymous_output=false;
  root.queries=vec_data_const(&graph.bindings); root.query_count=graph.query_count;
  root.derived=vec_data_const(&graph.derived); root.derived_count=graph.derived_count;
  const orm_sql_expr_query_sources sources={vec_data_const(&graph.sources),graph.query_count};
  if(status==TURBODB_STATUS_OK) status=orm_sql_runtime_scope_open(&root,&owner,parameters,false,&sources,&graph_root,&error);
  return status;
}
static turbodb_status_t lateral_metadata_open(uint64_t iterations) {
  turbodb_status_t status=orm_sql_dependencies_lateral_metadata_open(&scope,&owner,parameters,iterations,&graph,&error);
  orm_sql_query_scope root=scope; root.anonymous_output=false;
  root.queries=vec_data_const(&graph.bindings); root.query_count=graph.query_count;
  root.derived=vec_data_const(&graph.derived); root.derived_count=graph.derived_count;
  const orm_sql_expr_query_sources sources={vec_data_const(&graph.sources),graph.query_count};
  if(status==TURBODB_STATUS_OK) status=orm_sql_runtime_scope_open(&root,&owner,parameters,true,&sources,&graph_root,&error);
  if(status!=TURBODB_STATUS_OK) info("LATERAL metadata status %d: %s",status,error.message);
  return status;
}
static const orm_sql_table_schema *lateral_output(sqlparser_id id) {
  const orm_sql_table_schema *result=NULL;
  for(size_t i=0;i<graph.derived_count;++i) {
    const orm_sql_derived_binding *binding=vec_at_const(&graph.derived,i);
    if(binding->node==id) { result=binding->schema; check_null(binding->source); }
  }
  check_not_null(result); return result;
}
static turbodb_status_t lateral_child_open(void) {
  const sqlparser_id target=lateral_named("d");
  turbodb_status_t status=orm_sql_runtime_lateral_schema_open(&scope,target,&owner,&lateral_frame,&error);
  orm_sql_query_scope child=scope; child.root=sqlparser_get_node(document,target)->as.table.query;
  child.outer_schema=&lateral_frame.schema;
  if(status==TURBODB_STATUS_OK) status=orm_sql_lateral_query_open(&child,&owner,parameters,NULL,&lateral_query,&error);
  const turbodb_status_t released=orm_sql_runtime_lateral_schema_close(&lateral_frame,status==TURBODB_STATUS_OK?&error:NULL);
  return released==TURBODB_STATUS_OK?status:released;
}
static void lateral_join_open(orm_sql_join_kind kind) {
  orm_sql_schema_column column;
  check_equal(orm_tidesdb_sql_runtime_column(&external,0,&column,&error),TURBODB_STATUS_OK);
  external_type=column.type; external_source=(orm_sql_row_source){&budget,&external_type,1,&external,external_pull,false};
  if(kind!=ORM_SQL_JOIN_CROSS) {
    sqlparser_document *doc=NULL; sqlparser_error e;
    const char sql[]="SELECT TRUE";
    check_equal(sqlparser_parse(sql,strlen(sql),NULL,&doc,&e),SQLPARSER_OK);
    const sqlparser_node *statement=sqlparser_get_node(doc,sqlparser_statements(doc).first);
    const sqlparser_node *projection=sqlparser_get_node(doc,statement->as.select.columns.first);
    check_equal(orm_tidesdb_sql_expr_compile(doc,projection->as.projection.expression,NULL,0,
        DEPTH,&budget,&lateral_condition,&error),TURBODB_STATUS_OK);
    sqlparser_document_destroy(doc);
  }
  const orm_sql_join_spec spec={.kind=kind,.condition=kind==ORM_SQL_JOIN_CROSS?NULL:&lateral_condition,
      .right_binding=lateral_query.binding};
  check_equal(orm_tidesdb_sql_join_open(&external_source,&lateral_query.source,&spec,&lateral_join,&error),TURBODB_STATUS_OK);
}
static turbodb_status_t native_rewind(void *context,turbodb_error_t *e) { return orm_sql_relation_rewind(context,e); }
static void lateral_from_open(void) {
  check_equal(lateral_child_open(),TURBODB_STATUS_OK);
  const sqlparser_id root=sqlparser_get_node(document,scope.root)->as.select.from;
  vec_t names={0}; size_t bytes=0;
  check_equal(orm_sql_from_subtree_tables_at(&scope,root,&names,&bytes,&error),TURBODB_STATUS_OK);
  const size_t count=vec_size(&names); check_true(count<=NATIVE_INPUTS);
  const orm_sql_table_schema *schemas[NATIVE_INPUTS]; orm_sql_from_input inputs[NATIVE_INPUTS]={0};
  orm_sql_schema_column derived_columns[PARAMETERS]; check_true(lateral_query.query.columns<=PARAMETERS);
  for(size_t i=0;i<lateral_query.query.columns;++i)
    check_equal(orm_tidesdb_sql_runtime_column(&lateral_query.query,i,&derived_columns[i],&error),TURBODB_STATUS_OK);
  const orm_sql_table_schema derived={vstr_from_cstr("d"),derived_columns,lateral_query.query.columns};
  for(size_t i=0;i<count;++i) {
    const orm_sql_from_table *table=vec_at_const(&names,i);
    if(table->derived) {
      check_equal(table->ast,lateral_named("d")); schemas[i]=&derived;
      const orm_sql_select *child=orm_sql_runtime_plan(&lateral_query.query);
      const orm_sql_type *types=vec_data_const(&child->parameter_types);
      inputs[i]=(orm_sql_from_input){.source=&lateral_query.source,.binding=lateral_query.binding,
          .capture_types=types?types+lateral_query.marker_count:NULL,.capture_count=lateral_query.capture_count};
    } else {
      check_equal(orm_tidesdb_sql_relation_open(&owner,vstr_from_cstr(table->name),&native_inputs[i],&error),TURBODB_STATUS_OK);
      schemas[i]=&native_inputs[i].schema;
      inputs[i]=(orm_sql_from_input){.source=&native_inputs[i].source,.context=&native_inputs[i],.rewind=native_rewind};
    }
  }
  check_equal(orm_sql_from_subtree_schema_at(&scope,root,schemas,count,&native_from,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_from_bind_conditions_at(&scope,&native_from,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_select_bind_from(document,&native_from,DEPTH,&native_select,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_from_open_dependent(&native_from,inputs,count,parameters,scope.parameter_count,NULL,&native_run,&error),TURBODB_STATUS_OK);
  check_equal(orm_tidesdb_sql_select_open_source(&native_select,native_run.source,parameters,scope.parameter_count,&native_selected,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_work_release(&names,bytes,&budget,&error),TURBODB_STATUS_OK);
}
static orm_sql_scan_row native_next(void) {
  orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_scan_next(&native_selected.scan,&row,&error),TURBODB_STATUS_OK); return row;
}
static void graph_close(void) {
  check_equal(orm_tidesdb_sql_runtime_close(&graph_root,&error),TURBODB_STATUS_OK);
  check_equal(orm_sql_dependencies_close(&graph,&error),TURBODB_STATUS_OK);
}
static void graph_values(const int64_t *expected,size_t count) {
  orm_sql_scan_row row;
  for(size_t i=0;i<count;++i) {
    check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,expected[i]);
  }
  check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
}
static turbodb_status_t graph_drain(size_t *count) {
  *count=0;
  for(;;) {
    orm_sql_scan_row row={0}; const turbodb_status_t status=orm_tidesdb_sql_runtime_next(&graph_root,&row,&error);
    if(status!=TURBODB_STATUS_OK) return status;
    if(row.state!=ORM_SQL_SCAN_ROW) { check_equal(row.state,ORM_SQL_SCAN_DONE); return TURBODB_STATUS_OK; }
    ++*count;
  }
}
static void paired_values(const int64_t expected[][2],size_t count) {
  orm_sql_scan_row row={0};
  for(size_t i=0;i<count;++i) {
    check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.count,2u);
    for(size_t column=0;column<2;++column) {
      check_equal(row.values[column].kind,TURBODB_VALUE_INT64);
      check_equal(row.values[column].data.int64_value,expected[i][column]);
    }
  }
  check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
  check_equal(row.state,ORM_SQL_SCAN_DONE);
}
static turbodb_status_t bounded_open(uint64_t iterations) {
  const turbodb_status_t status=orm_sql_runtime_open_recursive(document,&owner,vstr_from_cstr("app"),parameters,
      scope.parameter_count,DEPTH,iterations,&graph_root,&error);
  if(status!=TURBODB_STATUS_OK) info("CTE open status %d: %s",status,error.message);
  return status;
}
/* Failure probes reuse one owner and retain cumulative accounting. Each attempt
 * gets headroom for resources whose limits are not the selected fault boundary. */
static void matrix_allowance(void) {
  for(size_t i=0;i<ORM_SQL_BUDGET_RESOURCE_COUNT;++i) {
    if(i==ORM_SQL_BUDGET_WORK_BYTES) continue;
    check_true(budget.used.value[i]<=UINT64_MAX-LIMIT);
    budget.limits.statement.value[i]=budget.used.value[i]+LIMIT;
  }
}
static void graph_fault_matrix(uint64_t work, const uint64_t counts[GRAPH_FAULT_PHASES]) {
  for(size_t pass=0;pass<GRAPH_FAULT_PHASES;++pass) {
    const bool steps=pass==2;
    for(uint64_t point=steps?0:1;steps?point<counts[pass]:point<=counts[pass];++point) {
      matrix_allowance(); reserves=resizes=0;
      if(steps) budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      else if(pass) fail_resize=(size_t)point; else fail_reserve=(size_t)point;
      turbodb_status_t status=bounded_open(DEPTH); size_t rows=0;
      if(status==TURBODB_STATUS_OK) status=graph_drain(&rows);
      check_equal(status,steps?TURBODB_STATUS_LIMIT_EXCEEDED:TURBODB_STATUS_OUT_OF_MEMORY);
      fail_reserve=fail_resize=0; graph_close(); check_equal(owner.active_sources,0u); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  matrix_allowance(); free_ast();
}
enum { EXPLAIN_ID=0,EXPLAIN_KIND=1,EXPLAIN_TABLE=2,EXPLAIN_EXTRA=11 };
typedef struct expected_explain { int64_t id; const char *kind,*table; bool recursive; } expected_explain;
static void text_value_is(turbodb_value_t value,const char *expected) {
  if(!expected) { check_equal(value.kind,TURBODB_VALUE_NULL); return; }
  check_equal(value.kind,TURBODB_VALUE_TEXT); check_equal(value.data.text_value.len,strlen(expected));
  check_equal(memcmp(value.data.text_value.data,expected,strlen(expected)),0);
}
static void explain_rows(const expected_explain *expected,size_t count) {
  const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
  const uint64_t materialized=budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
  size_t index=0; orm_sql_scan_row row;
  for(;;) {
    check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
    if(row.state!=ORM_SQL_SCAN_ROW) { check_equal(row.state,ORM_SQL_SCAN_DONE); break; }
    const bool marked=row.values[EXPLAIN_EXTRA].kind==TURBODB_VALUE_TEXT &&
        strstr(row.values[EXPLAIN_EXTRA].data.text_value.data,"Recursive")!=NULL;
    if(row.values[EXPLAIN_ID].kind==TURBODB_VALUE_NULL) { check_false(marked); continue; }
    check_true(index<count); if(index>=count) break;
    check_equal(row.values[EXPLAIN_ID].data.int64_value,expected[index].id);
    text_value_is(row.values[EXPLAIN_KIND],expected[index].kind);
    text_value_is(row.values[EXPLAIN_TABLE],expected[index].table);
    check_equal(marked,expected[index].recursive); ++index;
  }
  check_equal(index,count);
  check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
  check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
  check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],materialized);
}

spec("TidesDB recursive CTE plans and dependency graphs") {
  before_each() {
    tdsql_error_init(&error); reserves=resizes=fail_reserve=fail_resize=0; opens=closes=member_count=0;
    memset(&hooks,0,sizeof(hooks));
    memset(proxies,0,sizeof(proxies)); memset(bindings,0,sizeof(bindings));
    for(size_t i=0;i<PARAMETERS;++i) { parameters[i]=turbodb_i64(0); parameter_types[i]=(orm_sql_type){TURBODB_VALUE_INT64,false}; }
    orm_sql_budget_limits limits={0};
    for(size_t i=0;i<ORM_SQL_BUDGET_RESOURCE_COUNT;++i) limits.statement.value[i]=LIMIT;
    limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=WORK;
    limits.transaction=(orm_sql_transaction_budget_amount){LIMIT,LIMIT,LIMIT};
    check_equal(orm_tidesdb_sql_budget_init(&budget,&limits,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_budget_begin(&budget,&error),TURBODB_STATUS_OK);
    directory=tt_make_temp_dir("orm-cte-plan"); check_not_null(directory);
    orm_tidesdb_config_t config=orm_tidesdb_default_config(); config.db_path=directory;
    check_equal(orm_tidesdb_open(&config,&database),ORM_TDB_SUCCESS);
    orm_tidesdb_column_family_config_t cf=orm_tidesdb_default_column_family_config(); cf.sync_mode=ORM_TDB_SYNC_FULL;
    check_equal(orm_tidesdb_create_column_family(database,"cte-plan",&cf),ORM_TDB_SUCCESS);
    family=orm_tidesdb_get_column_family(database,"cte-plan"); check_not_null(family);
    check_equal(orm_tidesdb_sql_catalog_initialize(database,family,RECORD,&budget,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_catalog_begin(database,family,RECORD,&budget,&owner,&error),TURBODB_STATUS_OK);
  }
  after_each() {
    clean_plans(); check_equal(orm_tidesdb_sql_catalog_finish(&owner,false,&error),TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],0u);
    check_equal(orm_tidesdb_sql_budget_end(&budget,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_close(database),ORM_TDB_SUCCESS); database=NULL; family=NULL;
    check_equal(tt_remove_tree(directory),0); free(directory); directory=NULL;
  }

  group("set operations in CTE dependency plans") {
    it("executes pure initial set subtrees before UNION recursion") {
      parse_scope("WITH RECURSIVE c(n) AS ((SELECT 1 INTERSECT SELECT 1) UNION ALL SELECT n+1 FROM c WHERE n<3) SELECT n FROM c",0);
      check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      const int64_t expected[]={1,2,3}; graph_values(expected,3); graph_close();
      check_equal(owner.active_sources,0u);
    }
    it("preserves an EXCEPT initial seed and applies set operations to final recursive output") {
      parse_scope("WITH RECURSIVE c(n) AS ((SELECT 1 AS n UNION ALL SELECT 9 EXCEPT SELECT 9) UNION ALL SELECT n+1 FROM c WHERE n<3) SELECT n FROM c EXCEPT SELECT 2 ORDER BY n",0);
      check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      const int64_t expected[]={1,3}; graph_values(expected,2); graph_close();
      check_equal(owner.active_sources,0u);
    }
    it("rejects INTERSECT and EXCEPT nodes containing recursive references before execution") {
      const char *sql[]={"WITH RECURSIVE c(n) AS (SELECT 1 INTERSECT SELECT n+1 FROM c) SELECT n FROM c",
        "WITH RECURSIVE c(n) AS (SELECT 1 EXCEPT ALL SELECT n+1 FROM c) SELECT n FROM c",
        "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL (SELECT n+1 FROM c INTERSECT SELECT 2)) SELECT n FROM c"};
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
        parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_SQL_ERROR); graph_close(); free_ast();
        check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_false(owner.failed);
      }
    }
    it("keeps correlated set CTE caches isolated for each outer row") {
      execute("CREATE TABLE inputs(id BIGINT PRIMARY KEY)"); execute("INSERT INTO inputs VALUES(1),(2),(3)");
      parse_scope("SELECT o.id,(WITH c(n) AS (SELECT o.id INTERSECT SELECT 2) SELECT COUNT(*) FROM c) AS hit FROM inputs o ORDER BY o.id",0);
      check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      const int64_t expected[][2]={{1,0},{2,1},{3,0}}; paired_values(expected,3); graph_close();
      check_equal(owner.active_sources,0u);
    }
  }
  it("executes frame value functions in independent unit CTE derived and UNION scopes") {
    parse_scope("WITH c(n) AS(SELECT FIRST_VALUE(9) OVER w WINDOW w AS(ROWS CURRENT ROW)) "
        "SELECT d.n FROM(SELECT n FROM c) d UNION ALL SELECT LAST_VALUE(8) OVER() AS n "
        "UNION ALL SELECT NTH_VALUE(7,1) OVER(RANGE CURRENT ROW) AS n",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t expected[]={9,8,7}; graph_values(expected,3); graph_close(); check_equal(owner.active_sources,0u);
  }
  it("reopens lateral frame values from fresh local and ancestor scalar captures") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT a.id AS id,d.n FROM items a,LATERAL "
        "(SELECT FIRST_VALUE((SELECT b.id+a.id)) OVER w AS n FROM items b WHERE b.id<=a.id "
        "WINDOW w AS(ORDER BY b.id RANGE 1 PRECEDING) ORDER BY b.id DESC LIMIT 1) d ORDER BY id",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t expected[][2]={{1,2},{2,3},{3,5}}; paired_values(expected,3);
    graph_close(); check_equal(owner.active_sources,0u); check_false(owner.failed);
  }
  it("validates NTH_VALUE before EXISTS discards frame results") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2)");
    parse_scope("SELECT EXISTS(SELECT NTH_VALUE(id,?) OVER(ORDER BY id ROWS CURRENT ROW) FROM items) AS n",1);
    parameters[0]=turbodb_i64(0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_SQL_ERROR);
    graph_close(); check_equal(owner.active_sources,0u); check_false(owner.failed);
    parameters[0]=turbodb_i64(2); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].kind,TURBODB_VALUE_BOOLEAN); check_true(row.values[0].data.boolean_value);
    graph_close(); check_equal(owner.active_sources,0u);
  }

  it("owns explicit frames across unit CTE and compound query blocks after AST release") {
    parse_scope("WITH c(n) AS (SELECT LAG(9,1,0) OVER w WINDOW w AS(ROWS CURRENT ROW)) "
        "SELECT n FROM c UNION ALL SELECT RANK() OVER w AS n WINDOW w AS(RANGE CURRENT ROW)",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t expected[]={0,1}; graph_values(expected,2); graph_close(); check_equal(owner.active_sources,0u);
  }
  it("validates frame markers before EXISTS prunes unobserved window results") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2)");
    parse_scope("SELECT EXISTS(SELECT RANK() OVER(ORDER BY id ROWS ? PRECEDING) FROM items LIMIT 0) AS n",1);
    parameters[0]=turbodb_i64(-1); check_equal(bounded_open(DEPTH),TURBODB_STATUS_TYPE_ERROR);
    graph_close(); check_equal(owner.active_sources,0u); check_false(owner.failed);
    parameters[0]=turbodb_i64(0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].kind,TURBODB_VALUE_BOOLEAN); check_false(row.values[0].data.boolean_value);
    graph_close(); check_equal(owner.active_sources,0u);
  }
  it("isolates named windows across CTE derived and compound query blocks") {
    parse_scope("WITH c(n) AS (SELECT ROW_NUMBER() OVER w WINDOW w AS ()) "
        "SELECT d.n FROM (SELECT n+RANK() OVER w AS n FROM c WINDOW w AS (ORDER BY n)) d "
        "UNION ALL SELECT RANK() OVER w AS n WINDOW w AS ()",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t expected[]={2,1}; graph_values(expected,2); graph_close(); check_equal(owner.active_sources,0u);
    parse_scope("SELECT (SELECT RANK() OVER outer_w) AS n WINDOW outer_w AS ()",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_SQL_ERROR); check_contains(error.message,"unknown named");
    graph_close(); check_equal(owner.active_sources,0u);
  }
  it("binds named-window scalar keys to the grouped row capture scope") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT a.id AS id,ROW_NUMBER() OVER w+(SELECT a.id) AS n FROM items a "
        "GROUP BY a.id HAVING a.id>1 WINDOW w AS (ORDER BY (SELECT a.id) DESC) ORDER BY id",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t expected[][2]={{2,4},{3,4}}; paired_values(expected,2);
    graph_close(); check_equal(owner.active_sources,0u);
  }
  it("reopens inherited lateral windows with scalar keys and ancestor defaults") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT a.id AS id,d.n FROM items a,LATERAL "
        "(SELECT LAG((SELECT b.id),1,(SELECT a.id+100)) OVER sorted AS n FROM items b "
        "WHERE b.id<=a.id WINDOW sorted AS (base ORDER BY (SELECT b.id) ROWS BETWEEN 1 PRECEDING AND CURRENT ROW),base AS () "
        "ORDER BY b.id DESC LIMIT 1) d ORDER BY id",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t expected[][2]={{1,101},{2,1},{3,2}}; paired_values(expected,3);
    graph_close(); check_equal(owner.active_sources,0u); check_false(owner.failed);
  }
  it("reopens lateral offset windows with local value and ancestor default scalar captures") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT a.id AS id,d.n FROM items a,LATERAL "
        "(SELECT LAG((SELECT b.id),1,(SELECT a.id+100)) OVER(ORDER BY b.id) AS n FROM items b "
        "WHERE b.id<=a.id ORDER BY b.id DESC LIMIT 1) d ORDER BY id",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t expected[][2]={{1,101},{2,1},{3,2}}; paired_values(expected,3);
    graph_close(); check_equal(owner.active_sources,0u); check_false(owner.failed);
  }
  it("reads lagged recursive results without admitting windows in recursive members") {
    parse_scope("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3) "
        "SELECT n,LAG(n,1,0) OVER(ORDER BY n) AS previous FROM c ORDER BY n",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t expected[][2]={{1,0},{2,1},{3,2}}; paired_values(expected,3);
    graph_close(); check_equal(owner.active_sources,0u);
  }
  it("executes offset defaults in independent unit UNION and renamed CTE scopes") {
    const char *sql[]={"SELECT LAG(9,1,0) OVER() AS n UNION ALL SELECT LEAD(9,1,0) OVER() AS n",
        "WITH c(n) AS (SELECT LAG(9,1,0) OVER() UNION ALL SELECT LEAD(9,1,0) OVER()) SELECT n FROM c"};
    const int64_t expected[]={0,0};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      graph_values(expected,2); graph_close(); check_equal(owner.active_sources,0u);
    }
  }
  it("executes unit and nested window queries after their AST is destroyed") {
    const char *sql[]={
      "SELECT ROW_NUMBER() OVER()+NTILE(2) OVER() AS n",
      "SELECT d.n FROM (SELECT ROW_NUMBER() OVER()) d(n)",
      "WITH c(n) AS (SELECT ROW_NUMBER() OVER()) SELECT n FROM c",
      "SELECT ROW_NUMBER() OVER() AS n UNION ALL SELECT RANK() OVER() AS n"};
    const int64_t expected[][2]={{2,0},{1,0},{1,0},{1,1}};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      graph_values(expected[i],i==3?2:1); graph_close(); check_equal(owner.active_sources,0u);
    }
  }
  it("preserves scalar captures in window keys and final projections with hidden slots") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT a.id AS id,ROW_NUMBER() OVER(ORDER BY (SELECT a.id) DESC)+(SELECT a.id) AS n "
        "FROM items a WHERE EXISTS(SELECT a.id) ORDER BY id",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t expected[][2]={{1,4},{2,4},{3,4}}; paired_values(expected,3);
    graph_close(); check_equal(owner.active_sources,0u);
  }
  it("remaps grouped scalar captures through the post-HAVING window stage") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT a.id AS id,ROW_NUMBER() OVER(ORDER BY (SELECT a.id) DESC)+(SELECT a.id) AS n "
        "FROM items a GROUP BY a.id HAVING a.id>1 ORDER BY id",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t expected[][2]={{2,4},{3,4}}; paired_values(expected,2);
    graph_close(); check_equal(owner.active_sources,0u);
  }
  it("reopens correlated lateral windows without retaining the previous capture or ranking") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT a.id AS id,d.n FROM items a,LATERAL "
        "(SELECT ROW_NUMBER() OVER(ORDER BY b.id DESC)+a.id AS n FROM items b WHERE b.id<=a.id ORDER BY n LIMIT 1) d "
        "ORDER BY id",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t expected[][2]={{1,2},{2,3},{3,4}}; paired_values(expected,3);
    graph_close(); check_equal(owner.active_sources,0u); check_false(owner.failed);
  }
  it("ranks a recursive CTE result while keeping recursive member windows rejected") {
    parse_scope("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3) "
        "SELECT n,ROW_NUMBER() OVER(ORDER BY n DESC) AS r FROM c ORDER BY n",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t expected[][2]={{1,3},{2,2},{3,1}}; paired_values(expected,3); graph_close();
    parse_scope("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT ROW_NUMBER() OVER()+n FROM c WHERE n<3) SELECT n FROM c",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_SQL_ERROR); graph_close(); check_equal(owner.active_sources,0u);
  }
  it("executes aggregate windows over recursive results unit rows and UNION branches") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3) SELECT COUNT(*) OVER(ORDER BY n) AS hits FROM c ORDER BY n",
      "SELECT COUNT(*) OVER() AS n UNION ALL SELECT MAX(9) OVER() AS n",
      "WITH c(n) AS (SELECT COUNT(*) OVER() UNION ALL SELECT MIN(2) OVER()) SELECT n FROM c"};
    const int64_t expected[][3]={{1,2,3},{1,9,0},{1,2,0}};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      graph_values(expected[i],i?2:3); graph_close(); check_equal(owner.active_sources,0u);
    }
  }
  it("reopens correlated lateral aggregate windows with each new capture") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT a.id AS id,d.n FROM items a,LATERAL "
        "(SELECT COUNT(*) OVER()+MAX((SELECT a.id)) OVER() AS n FROM items b WHERE b.id<=a.id LIMIT 1) d ORDER BY id",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t expected[][2]={{1,2},{2,4},{3,6}}; paired_values(expected,3);
    graph_close(); check_equal(owner.active_sources,0u); check_false(owner.failed);
  }
  it("keeps aggregate window query blocks separate from outer grouping and rejects recursive members") {
    parse_scope("WITH c(n) AS (SELECT 1 UNION ALL SELECT 2) SELECT MAX((SELECT COUNT(*) OVER() FROM c LIMIT 1)) AS n FROM c",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast(); const int64_t expected[]={2}; graph_values(expected,1); graph_close();
    parse_scope("WITH c(n) AS (SELECT 1 UNION ALL SELECT 2) SELECT MAX((SELECT COUNT(*) OVER() FROM c LIMIT 1)) OVER() AS n FROM c",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast(); const int64_t per_row[]={2,2}; graph_values(per_row,2); graph_close();
    parse_scope("WITH RECURSIVE c(n) AS(SELECT 1 UNION ALL SELECT COUNT(*) OVER()+n FROM c WHERE n<3) SELECT n FROM c",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_SQL_ERROR); graph_close(); check_equal(owner.active_sources,0u);
  }
  it("executes DOUBLE aggregate frames in dependent CTE query graphs") {
    parse_scope("WITH c(n) AS(SELECT 1.0 UNION ALL SELECT 2.0 UNION ALL SELECT 4.0) "
        "SELECT SUM(n) OVER w AS total,AVG(n) OVER w AS mean FROM c WINDOW w AS(ORDER BY n ROWS 1 PRECEDING) ORDER BY total",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const double expected[][2]={{1,1},{3,1.5},{6,3}};
    for(size_t i=0;i<3;++i) {
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.count,2u);
      for(size_t column=0;column<2;++column) { check_equal(row.values[column].kind,TURBODB_VALUE_DOUBLE); check_equal(row.values[column].data.double_value,expected[i][column]); }
    }
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_DONE); graph_close(); check_equal(owner.active_sources,0u);
  }
  it("executes DISTINCT tuple count in unit UNION CTE and recursive-result scopes") {
    const char *sql[]={
      "SELECT COUNT(DISTINCT 1,2) AS n UNION ALL SELECT COUNT(DISTINCT NULL,2) AS n",
      "WITH c(n) AS(SELECT 1 UNION ALL SELECT 1 UNION ALL SELECT 3) SELECT COUNT(DISTINCT n,n>0) AS count FROM c",
      "WITH RECURSIVE c(n) AS(SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3) SELECT COUNT(DISTINCT n,n>0) AS count FROM c"};
    for(size_t i=0;i<3;++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,i?i+1:1);
      if(!i) { check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK); check_equal(row.values[0].data.int64_value,0); }
      check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
      graph_close(); check_equal(owner.active_sources,0u);
    }
    parse_scope("WITH RECURSIVE c(n) AS(SELECT 1 UNION ALL SELECT COUNT(DISTINCT n,n>0) FROM c) SELECT n FROM c",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_SQL_ERROR); graph_close(); check_equal(owner.active_sources,0u);
  }
  it("reopens lateral DISTINCT tuples with fresh captures for each prefix row") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT a.id AS id,d.n FROM items a,LATERAL "
        "(SELECT COUNT(DISTINCT (SELECT a.id),(SELECT b.id)) AS n FROM items b WHERE b.id<=a.id) d ORDER BY id",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    for(size_t i=0;i<3;++i) {
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,i+1); check_equal(row.values[1].data.int64_value,i+1);
    }
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_DONE); graph_close(); check_equal(owner.active_sources,0u);
  }
  it("deduplicates correlated DOUBLE SUM AVG arguments independently in a CTE") {
    parse_scope("WITH c(n) AS(SELECT 1e0 UNION ALL SELECT 1e0 UNION ALL SELECT 3e0) "
        "SELECT SUM(DISTINCT (SELECT b.n FROM c b WHERE b.n=a.n LIMIT 1)) AS sum,"
        "AVG(DISTINCT (SELECT b.n FROM c b WHERE b.n=a.n LIMIT 1)) AS mean FROM c a",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast(); orm_sql_scan_row row={0};
    check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_ROW);
    check_equal(row.values[0].kind,TURBODB_VALUE_DOUBLE); check_equal(row.values[0].data.double_value,4.0);
    check_equal(row.values[1].kind,TURBODB_VALUE_DOUBLE); check_equal(row.values[1].data.double_value,2.0);
    check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
    graph_close(); check_equal(owner.active_sources,0u);
  }
  it("refunds every correlated DISTINCT tuple dependency allocation and execution step") {
    parse_scope("WITH c(n) AS(SELECT 1 UNION ALL SELECT 2) SELECT a.n AS n,"
        "(SELECT COUNT(DISTINCT (SELECT a.n),(SELECT b.n)) FROM c b WHERE b.n<=a.n) AS tuples "
        "FROM c a ORDER BY n",0);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; matrix_allowance(); reserves=resizes=0;
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]; check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK);
    for(size_t i=0;i<2;++i) {
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,i+1); check_equal(row.values[1].data.int64_value,i+1);
    }
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
    const uint64_t counts[]={reserves,resizes,budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start}; graph_close();
    graph_fault_matrix(work,counts);
  }
  it("executes bit unit UNION and recursive-result query scopes independently") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS(SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3) SELECT BIT_OR(n) AS bits FROM c",
      "SELECT BIT_OR(-1) AS bits UNION ALL SELECT BIT_AND(NULL) AS bits",
      "WITH c(n) AS(SELECT 1 UNION ALL SELECT 3) SELECT BIT_XOR((SELECT BIT_OR(n) OVER() FROM c LIMIT 1)) AS bits FROM c"};
    const uint64_t expected[]={3,UINT64_MAX,0};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      for(size_t j=0;j<(i==1?2u:1u);++j) {
        orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
        check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].kind,TURBODB_VALUE_UINT64); check_equal(row.values[0].data.uint64_value,expected[i]);
      }
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_DONE); graph_close(); check_equal(owner.active_sources,0u);
    }
    parse_scope("WITH RECURSIVE c(n) AS(SELECT 1 UNION ALL SELECT BIT_OR(n)+n FROM c WHERE n<3) SELECT n FROM c",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_SQL_ERROR); graph_close(); check_equal(owner.active_sources,0u);
  }
  it("reopens lateral bit windows with fresh prefix captures and bit identities") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT a.id AS id,d.bits FROM items a,LATERAL "
        "(SELECT BIT_XOR(b.id+(SELECT a.id)) OVER() AS bits FROM items b WHERE b.id<=a.id LIMIT 1) d ORDER BY id",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast(); const uint64_t expected[]={2,7,7};
    for(size_t i=0;i<3;++i) {
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,(int64_t)i+1);
      check_equal(row.values[1].kind,TURBODB_VALUE_UINT64); check_equal(row.values[1].data.uint64_value,expected[i]);
    }
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_DONE); graph_close(); check_equal(owner.active_sources,0u);
  }
  it("refunds every correlated bit group and window dependency allocation and execution step") {
    parse_scope("WITH c(n) AS(SELECT 1 UNION ALL SELECT 2) SELECT a.n AS n,"
        "BIT_AND((SELECT a.n)) OVER w AS a,BIT_OR((SELECT a.n)) OVER w AS o,"
        "BIT_XOR((SELECT BIT_OR(b.n) FROM c b WHERE b.n=a.n)) OVER w AS x "
        "FROM c a WINDOW w AS(ORDER BY (SELECT a.n) ROWS 1 PRECEDING) ORDER BY n",0);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; matrix_allowance(); reserves=resizes=0;
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]; check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK);
    const uint64_t expected[][3]={{1,1,1},{0,3,3}};
    for(size_t i=0;i<2;++i) {
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,(int64_t)i+1);
      for(size_t column=0;column<3;++column) {
        check_equal(row.values[column+1].kind,TURBODB_VALUE_UINT64); check_equal(row.values[column+1].data.uint64_value,expected[i][column]);
      }
    }
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
    const uint64_t counts[]={reserves,resizes,budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start}; graph_close();
    graph_fault_matrix(work,counts);
  }
  it("executes statistical unit UNION and recursive-result query scopes independently") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS(SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3) SELECT VAR_POP(n) AS p FROM c",
      "SELECT STD(4) AS p UNION ALL SELECT VARIANCE(8) AS p",
      "WITH c(n) AS(SELECT 1 UNION ALL SELECT 3) SELECT VAR_POP((SELECT STDDEV(n) OVER() FROM c LIMIT 1)) AS p FROM c"};
    const double expected[]={2.0/3.0,0,0};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      for(size_t j=0;j<(i==1?2u:1u);++j) {
        orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
        check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].kind,TURBODB_VALUE_DOUBLE);
        check_less_equal(fabs(row.values[0].data.double_value-expected[i]),1e-12);
      }
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_DONE); graph_close(); check_equal(owner.active_sources,0u);
    }
  }
  it("reopens statistical lateral windows without retaining a prior capture or sample count") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT a.id AS id,d.p FROM items a,LATERAL "
        "(SELECT VAR_POP(b.id+(SELECT a.id)) OVER() AS p FROM items b WHERE b.id<=a.id LIMIT 1) d ORDER BY id",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast(); const double expected[]={0,0.25,2.0/3.0};
    for(size_t i=0;i<3;++i) {
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.count,2u); check_equal(row.values[0].data.int64_value,(int64_t)i+1);
      check_equal(row.values[1].kind,TURBODB_VALUE_DOUBLE); check_less_equal(fabs(row.values[1].data.double_value-expected[i]),1e-12);
    }
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_DONE); graph_close(); check_equal(owner.active_sources,0u);
  }
  it("validates statistical domains under EXPLAIN and rejects statistics in recursive members") {
    parse_scope("EXPLAIN SELECT VAR_POP(1+9223372036854775807) AS p",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast(); size_t count=0; check_equal(graph_drain(&count),TURBODB_STATUS_OK); check_equal(count,1u); graph_close();
    parse_scope("WITH RECURSIVE c(n) AS(SELECT 1 UNION ALL SELECT VAR_POP(n)+n FROM c WHERE n<3) SELECT n FROM c",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_SQL_ERROR); graph_close(); check_equal(owner.active_sources,0u);
  }
  it("refunds every allocation and execution step in correlated window dependency graphs") {
    parse_scope("WITH c(n) AS (SELECT 1 UNION ALL SELECT 2) SELECT a.n AS n,"
        "LAG((SELECT a.n),1,(SELECT a.n)) OVER sorted+(SELECT a.n)+"
        "FIRST_VALUE((SELECT a.n)) OVER sorted+LAST_VALUE((SELECT a.n)) OVER sorted+"
        "NTH_VALUE((SELECT a.n),1) OVER sorted+COUNT(*) OVER sorted+"
        "COUNT((SELECT a.n)) OVER sorted+MIN((SELECT a.n)) OVER sorted+MAX((SELECT a.n)) OVER sorted AS r FROM c a "
        "WINDOW sorted AS (base ORDER BY (SELECT a.n) ROWS CURRENT ROW),base AS () ORDER BY n",0);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; matrix_allowance(); reserves=resizes=0;
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); const int64_t expected[][2]={{1,9},{2,15}}; paired_values(expected,2);
    const uint64_t counts[]={reserves,resizes,budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start}; graph_close();
    graph_fault_matrix(work,counts);
  }
  it("refunds each statistical group and window dependency allocation and execution step") {
    parse_scope("WITH c(n) AS(SELECT 1 UNION ALL SELECT 2) SELECT a.n AS n,"
        "VAR_POP((SELECT a.n)) OVER w AS p,VAR_SAMP((SELECT a.n)) OVER w AS s,"
        "STDDEV_POP((SELECT VAR_POP(b.n) FROM c b WHERE b.n=a.n)) OVER w AS dp,STDDEV_SAMP((SELECT a.n)) OVER w AS ds "
        "FROM c a WINDOW w AS(ORDER BY (SELECT a.n) ROWS 1 PRECEDING) ORDER BY n",0);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; matrix_allowance(); reserves=resizes=0;
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]; check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK);
    for(size_t i=0;i<2;++i) {
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.count,5u); check_equal(row.values[0].data.int64_value,(int64_t)i+1);
      check_equal(row.values[1].data.double_value,i?0.25:0.0); check_equal(row.values[3].data.double_value,0.0);
      if(!i) { check_equal(row.values[2].kind,TURBODB_VALUE_NULL); check_equal(row.values[4].kind,TURBODB_VALUE_NULL); }
      else { check_equal(row.values[2].data.double_value,0.5); check_less_equal(fabs(row.values[4].data.double_value-sqrt(0.5)),1e-12); }
    }
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
    const uint64_t counts[]={reserves,resizes,budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start}; graph_close();
    graph_fault_matrix(work,counts);
  }
  it("executes a complete LEFT lateral FROM with native rows after its AST dies") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT a.id AS id,d.n AS n FROM items a LEFT JOIN LATERAL (SELECT b.id AS n FROM items b WHERE b.id=a.id AND b.id=2) d ON TRUE ORDER BY id",0);
    lateral_from_open(); free_ast();
    check_equal(orm_tidesdb_sql_from_close(&native_run,&error),TURBODB_STATUS_BUSY);
    for(int64_t key=1;key<=3;++key) {
      const orm_sql_scan_row row=native_next(); check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.count,2u);
      check_equal(row.values[0].data.int64_value,key);
      if(key==2) { check_equal(row.values[1].kind,TURBODB_VALUE_INT64); check_equal(row.values[1].data.int64_value,key); }
      else check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
    }
    check_equal(native_next().state,ORM_SQL_SCAN_DONE); check_false(lateral_query.bound);
    check_equal(((const turbodb_value_t *)vec_at_const(&lateral_query.arguments.values,0))->kind,TURBODB_VALUE_NULL);
  }
  it("reopens a native lateral subtree with ancestor RIGHT prefixes in logical column order") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2)");
    parse_scope("SELECT a.id AS aid,d.n AS n,c.id AS cid FROM items a JOIN LATERAL (SELECT a.id+c.id AS n) d ON TRUE RIGHT JOIN items c ON TRUE ORDER BY cid,aid",0);
    lateral_from_open(); check_equal(lateral_query.capture_count,2u); free_ast();
    const int64_t expected[][3]={{1,2,1},{2,3,1},{1,3,2},{2,4,2}};
    for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) {
      const orm_sql_scan_row row=native_next(); check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.count,3u);
      for(size_t column=0;column<3;++column) check_equal(row.values[column].data.int64_value,expected[i][column]);
    }
    check_equal(native_next().state,ORM_SQL_SCAN_DONE); check_false(lateral_query.bound);
  }
  it("groups native dependent outputs and applies HAVING ordering and pagination") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT d.n AS n,COUNT(*) AS hits FROM items a JOIN LATERAL (SELECT b.id AS n FROM items b WHERE b.id<=a.id) d ON TRUE GROUP BY d.n HAVING COUNT(*)>1 ORDER BY hits DESC LIMIT 1 OFFSET 1",0);
    lateral_from_open(); free_ast(); const orm_sql_scan_row row=native_next();
    check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,2);
    check_equal(row.values[1].data.int64_value,2); check_equal(native_next().state,ORM_SQL_SCAN_DONE);
    check_false(lateral_query.bound);
  }
  it("keeps full-document markers owned across native dependent FROM and SELECT") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parameters[0]=turbodb_i64(100); parameters[1]=turbodb_i64(10); parameters[2]=turbodb_i64(1); parameters[3]=turbodb_i64(1);
    parse_scope("SELECT ?+a.id AS n,d.n AS value FROM items a JOIN LATERAL (SELECT ?+a.id AS n WHERE a.id>?) d ON a.id>? ORDER BY n",PARAMETERS);
    lateral_from_open(); free_ast(); memset(parameters,0,sizeof(parameters));
    for(int64_t key=2;key<=3;++key) {
      const orm_sql_scan_row row=native_next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
      check_equal(row.values[0].data.int64_value,key+100); check_equal(row.values[1].data.int64_value,key+10);
    }
    check_equal(native_next().state,ORM_SQL_SCAN_DONE);
  }
  it("resets child aggregation between empty and matching native FROM rounds") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT a.id AS id,d.n AS n,d.hits AS hits FROM items a,LATERAL (SELECT MAX(b.id) AS n,COUNT(*) AS hits FROM items b WHERE b.id=a.id AND b.id=2) d ORDER BY id",0);
    lateral_from_open(); free_ast();
    for(int64_t key=1;key<=3;++key) {
      const orm_sql_scan_row row=native_next(); check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.count,3u);
      check_equal(row.values[0].data.int64_value,key);
      check_equal(row.values[1].kind,key==2?TURBODB_VALUE_INT64:TURBODB_VALUE_NULL);
      if(key==2) check_equal(row.values[1].data.int64_value,key);
      check_equal(row.values[2].data.int64_value,key==2?1:0);
    }
    check_equal(native_next().state,ORM_SQL_SCAN_DONE); check_false(lateral_query.bound);
  }
  it("does not evaluate a lateral child when the complete native FROM left is empty") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    parse_scope("SELECT d.n FROM items a,LATERAL (SELECT 9223372036854775807+1 AS n) d",0);
    lateral_from_open(); free_ast(); check_equal(native_next().state,ORM_SQL_SCAN_DONE);
    check_false(lateral_query.bound); check_equal(lateral_query.query.execution_failure.status,TURBODB_STATUS_OK);
  }
  it("passes completed native LEFT prefix nulls to a later lateral query") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2)");
    parse_scope("SELECT a.id AS aid,b.id AS bid,d.n AS n FROM items a LEFT JOIN items b ON b.id=a.id+1 JOIN LATERAL (SELECT a.id+b.id AS n) d ON TRUE ORDER BY aid",0);
    lateral_from_open(); free_ast();
    orm_sql_scan_row row=native_next(); check_equal(row.state,ORM_SQL_SCAN_ROW);
    check_equal(row.values[0].data.int64_value,1); check_equal(row.values[1].data.int64_value,2);
    check_equal(row.values[2].data.int64_value,3);
    row=native_next(); check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,2);
    check_equal(row.values[1].kind,TURBODB_VALUE_NULL); check_equal(row.values[2].kind,TURBODB_VALUE_NULL);
    check_equal(native_next().state,ORM_SQL_SCAN_DONE);
  }
  it("latches a later native lateral overflow after an earlier round returned a row") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(0),(1)");
    parse_scope("SELECT a.id AS id,d.n AS n FROM items a,LATERAL (SELECT 9223372036854775807+a.id AS n) d",0);
    lateral_from_open(); free_ast(); orm_sql_scan_row row=native_next();
    check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,0);
    check_equal(row.values[1].data.int64_value,INT64_MAX);
    check_equal(orm_sql_lateral_query_close(&lateral_query,&error),TURBODB_STATUS_BUSY);
    row=(orm_sql_scan_row){.state=ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_scan_next(&native_selected.scan,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(row.state,ORM_SQL_SCAN_CANCELLED); const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_scan_next(&native_selected.scan,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
    check_false(owner.failed); clean_plans(); check_equal(owner.active_sources,0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
  }
  it("refunds every native lateral FROM execution allocation across all query rounds") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char sql[]="SELECT a.id AS id,d.n AS n FROM items a,LATERAL (SELECT b.id AS n FROM items b WHERE b.id<=a.id) d ORDER BY id,n";
    parse_scope(sql,0); lateral_from_open(); free_ast(); reserves=resizes=0;
    check_equal(native_next().state,ORM_SQL_SCAN_ROW); const size_t allocated=reserves,resized=resizes;
    clean_plans(); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
    for(size_t i=1;i<=allocated+resized;++i) {
      matrix_allowance(); parse_scope(sql,0); lateral_from_open(); free_ast();
      reserves=resizes=0; fail_reserve=i<=allocated?i:0; fail_resize=i>allocated?i-allocated:0;
      orm_sql_scan_row row={.state=ORM_SQL_SCAN_CANCELLED};
      check_equal(orm_tidesdb_sql_scan_next(&native_selected.scan,&row,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      check_equal(row.state,ORM_SQL_SCAN_CANCELLED); const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(orm_tidesdb_sql_scan_next(&native_selected.scan,&row,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
      fail_reserve=fail_resize=0; clean_plans();
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained); check_equal(owner.active_sources,0u);
      check_false(owner.failed); check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    }
  }
  it("enforces every native lateral FROM execution step including later native query rounds") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    const uint64_t retained=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char sql[]="SELECT a.id AS id,d.n AS n FROM items a,LATERAL (SELECT b.id AS n FROM items b WHERE b.id<=a.id) d ORDER BY id,n";
    parse_scope(sql,0); lateral_from_open(); free_ast();
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]; check_equal(native_next().state,ORM_SQL_SCAN_ROW);
    const uint64_t cost=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    clean_plans(); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
    for(uint64_t allowance=0;allowance<cost;++allowance) {
      matrix_allowance(); parse_scope(sql,0); lateral_from_open(); free_ast();
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+allowance;
      orm_sql_scan_row row={.state=ORM_SQL_SCAN_CANCELLED};
      check_equal(orm_tidesdb_sql_scan_next(&native_selected.scan,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(row.state,ORM_SQL_SCAN_CANCELLED); const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(orm_tidesdb_sql_scan_next(&native_selected.scan,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
      clean_plans(); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],retained);
      check_equal(owner.active_sources,0u); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    }
  }
  it("accepts MySQL lateral local aggregation over captured prefix columns") {
    const char *sql[]={
        "SELECT d.n FROM (SELECT 2 AS n) a,LATERAL (SELECT MAX(a.n) AS n) d",
        "SELECT d.n FROM (SELECT 2 AS n) a,LATERAL (SELECT COUNT(a.n) AS n) d",
        "SELECT d.n FROM (SELECT 2 AS n) a,LATERAL (SELECT MIN(b.n+a.n) AS n FROM (SELECT 3 AS n) b) d",
        "SELECT d.n FROM (SELECT 2 AS n) a,LATERAL (WITH q AS (SELECT a.n AS n) SELECT MAX(n) AS n FROM q) d"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(lateral_metadata_open(0),TURBODB_STATUS_OK);
      check_equal(lateral_output(lateral_named("d"))->columns[0].type.kind,TURBODB_VALUE_INT64);
      free_ast(); graph_close(); check_equal(owner.active_sources,0u); check_false(owner.failed);
    }
  }
  it("runs a compiled lateral child per native left row and skips empty right rounds") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    open_external("SELECT id FROM items ORDER BY id");
    parameters[0]=turbodb_i64(100); parameters[1]=turbodb_i64(10); parameters[2]=turbodb_i64(1); parameters[3]=turbodb_i64(1);
    parse_scope("SELECT ? AS ignored,d.n FROM items a JOIN LATERAL (SELECT ?+a.id AS n WHERE a.id>?) d ON ?",PARAMETERS);
    check_equal(lateral_child_open(),TURBODB_STATUS_OK); check_true(lateral_query.query.execution_closed);
    check_equal(lateral_query.marker_count,PARAMETERS); check_equal(lateral_query.capture_count,1u);
    for(size_t i=0;i<PARAMETERS;++i) parameters[i]=turbodb_i64(99);
    free_ast(); lateral_join_open(ORM_SQL_JOIN_CROSS);
    for(int64_t key=2;key<=3;++key) {
      orm_sql_scan_row row;
      check_equal(orm_tidesdb_sql_join_next(&lateral_join,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,key);
      check_equal(row.values[1].data.int64_value,key+10);
    }
    orm_sql_scan_row row; check_equal(orm_tidesdb_sql_join_next(&lateral_join,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_DONE); check_false(lateral_query.bound);
    check_equal(((const turbodb_value_t *)vec_at_const(&lateral_query.arguments.values,PARAMETERS))->kind,TURBODB_VALUE_NULL);
  }
  it("null extends empty native lateral rounds in LEFT JOIN after a matching round") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    open_external("SELECT id FROM items ORDER BY id");
    parse_scope("SELECT d.n FROM items a LEFT JOIN LATERAL (SELECT b.id AS n FROM items b WHERE b.id=a.id AND b.id=2) d ON TRUE",0);
    check_equal(lateral_child_open(),TURBODB_STATUS_OK); free_ast(); lateral_join_open(ORM_SQL_JOIN_LEFT);
    for(int64_t key=1;key<=3;++key) {
      orm_sql_scan_row row; check_equal(orm_tidesdb_sql_join_next(&lateral_join,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,key);
      if(key==2) { check_equal(row.values[1].kind,TURBODB_VALUE_INT64); check_equal(row.values[1].data.int64_value,key); }
      else check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
    }
    orm_sql_scan_row row; check_equal(orm_tidesdb_sql_join_next(&lateral_join,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_DONE);
    check_equal(orm_tidesdb_sql_join_close(&lateral_join,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_runtime_close(&external,&error),TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u); check_false(lateral_query.bound);
  }
  it("resets local MAX and COUNT between matching and empty lateral native inputs") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    open_external("SELECT id FROM items ORDER BY id");
    parse_scope("SELECT d.n FROM items a,LATERAL (SELECT MAX(b.id) AS n,COUNT(*) AS hits FROM items b WHERE b.id=a.id AND b.id=2) d",0);
    check_equal(lateral_child_open(),TURBODB_STATUS_OK); free_ast(); lateral_join_open(ORM_SQL_JOIN_CROSS);
    for(int64_t key=1;key<=3;++key) {
      orm_sql_scan_row row; check_equal(orm_tidesdb_sql_join_next(&lateral_join,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,key);
      if(key==2) { check_equal(row.values[1].kind,TURBODB_VALUE_INT64); check_equal(row.values[1].data.int64_value,key); }
      else check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
      check_equal(row.values[2].kind,TURBODB_VALUE_INT64); check_equal(row.values[2].data.int64_value,key==2?1:0);
    }
    orm_sql_scan_row row; check_equal(orm_tidesdb_sql_join_next(&lateral_join,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_DONE);
  }
  it("executes MAX of the captured prefix in the lateral child for each native row") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    open_external("SELECT id FROM items ORDER BY id");
    parse_scope("SELECT a.id,d.n FROM items a,LATERAL (SELECT MAX(a.id) AS n) d",0);
    check_equal(lateral_child_open(),TURBODB_STATUS_OK); free_ast(); lateral_join_open(ORM_SQL_JOIN_CROSS);
    for(int64_t key=1;key<=3;++key) {
      orm_sql_scan_row row; check_equal(orm_tidesdb_sql_join_next(&lateral_join,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,key);
      check_equal(row.values[1].kind,TURBODB_VALUE_INT64); check_equal(row.values[1].data.int64_value,key);
    }
  }
  it("owns TEXT marker payloads across AST destruction and dependent join snapshots") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2)");
    open_external("SELECT id FROM items ORDER BY id");
    char text[]="kept"; parameters[0]=turbodb_text(text); parameter_types[0]=(orm_sql_type){TURBODB_VALUE_TEXT,false};
    parse_scope("SELECT d.n FROM items a,LATERAL (SELECT ? AS n) d",1);
    check_equal(lateral_child_open(),TURBODB_STATUS_OK); memset(text,'!',strlen(text)); free_ast();
    lateral_join_open(ORM_SQL_JOIN_CROSS);
    for(int64_t key=1;key<=2;++key) {
      orm_sql_scan_row row; check_equal(orm_tidesdb_sql_join_next(&lateral_join,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,key);
      text_value_is(row.values[1],"kept");
    }
  }
  it("does not open a potentially overflowing lateral child for an empty left input") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); open_external("SELECT id FROM items");
    parse_scope("SELECT d.n FROM items a,LATERAL (SELECT a.id+9223372036854775807 AS n) d",0);
    check_equal(lateral_child_open(),TURBODB_STATUS_OK); free_ast(); lateral_join_open(ORM_SQL_JOIN_CROSS);
    const uint64_t materialized=budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
    orm_sql_scan_row row; check_equal(orm_tidesdb_sql_join_next(&lateral_join,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_DONE); check_false(lateral_query.bound); check_true(lateral_query.query.execution_closed);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],materialized);
  }
  it("preserves capture borrows while a lateral source consumer prevents close") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    parse_scope("SELECT d.n FROM items a,LATERAL (SELECT a.id AS n) d",0);
    check_equal(lateral_child_open(),TURBODB_STATUS_OK); free_ast();
    const turbodb_value_t captured=turbodb_i64(7);
    check_equal(lateral_query.binding.open(&lateral_query,&captured,0,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(lateral_query.binding.open(&lateral_query,NULL,1,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(lateral_query.binding.open(&lateral_query,&captured,1,&error),TURBODB_STATUS_OK);
    check_equal(lateral_query.binding.open(&lateral_query,&captured,1,&error),TURBODB_STATUS_BUSY);
    const size_t projection=0; const orm_sql_scan_spec spec={.projection=&projection,.projection_count=1,.limit=UINT64_MAX};
    check_equal(orm_tidesdb_sql_scan_open_source(&lateral_query.source,&spec,&budget,&lateral_consumer,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(lateral_query.binding.close(&lateral_query,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_sql_lateral_query_close(&lateral_query,&error),TURBODB_STATUS_BUSY); check_true(lateral_query.bound);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    orm_sql_scan_row row; check_equal(orm_tidesdb_sql_scan_next(&lateral_consumer,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,7);
    check_equal(orm_tidesdb_sql_scan_close(&lateral_consumer,&error),TURBODB_STATUS_OK);
    check_equal(lateral_query.binding.close(&lateral_query,&error),TURBODB_STATUS_OK); check_false(lateral_query.bound);
    check_equal(((const turbodb_value_t *)vec_at_const(&lateral_query.arguments.values,0))->kind,TURBODB_VALUE_NULL);
    check_equal(lateral_query.binding.close(&lateral_query,&error),TURBODB_STATUS_OK);
  }
  it("latches lateral projection failure and refunds the failed JOIN round on close") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1)");
    open_external("SELECT id FROM items");
    parse_scope("SELECT d.n FROM items a,LATERAL (SELECT a.id+9223372036854775807 AS n) d",0);
    check_equal(lateral_child_open(),TURBODB_STATUS_OK); free_ast(); lateral_join_open(ORM_SQL_JOIN_CROSS);
    orm_sql_scan_row row={.state=ORM_SQL_SCAN_DONE};
    check_equal(orm_tidesdb_sql_join_next(&lateral_join,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(row.state,ORM_SQL_SCAN_DONE);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_tidesdb_sql_join_next(&lateral_join,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
    check_equal(orm_tidesdb_sql_join_close(&lateral_join,&error),TURBODB_STATUS_OK); check_false(lateral_query.bound);
    const turbodb_value_t captured=turbodb_i64(0);
    check_equal(lateral_query.binding.open(&lateral_query,&captured,1,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(((const turbodb_value_t *)vec_at_const(&lateral_query.arguments.values,0))->kind,TURBODB_VALUE_NULL);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u); check_false(owner.failed);
  }
  it("refunds every dependent query construction allocation and retained frame lease") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    parse_scope("SELECT d.n FROM items a,LATERAL (SELECT MAX(b.id+a.id+?) AS n FROM items b) d",1);
    const sqlparser_id target=lateral_named("d");
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,target,&owner,&lateral_frame,&error),TURBODB_STATUS_OK);
    orm_sql_query_scope child=scope; child.root=sqlparser_get_node(document,target)->as.table.query;
    child.outer_schema=&lateral_frame.schema;
    matrix_allowance(); reserves=resizes=0;
    check_equal(orm_sql_lateral_query_open(&child,&owner,parameters,NULL,&lateral_query,&error),TURBODB_STATUS_OK);
    const size_t counts[]={reserves,resizes};
    check_equal(orm_sql_lateral_query_close(&lateral_query,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; const size_t leases=owner.active_sources;
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=counts[pass];++point) {
      matrix_allowance(); reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(orm_sql_lateral_query_open(&child,&owner,parameters,NULL,&lateral_query,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      fail_resize=fail_reserve=0; check_null(lateral_query.budget);
      check_equal(orm_sql_lateral_query_close(&lateral_query,&error),TURBODB_STATUS_OK);
      check_equal(owner.active_sources,leases); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("refunds every dependent query round allocation and clears captures after failed open") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    parse_scope("SELECT d.n FROM items a,LATERAL (SELECT MAX(b.id+a.id+?) AS n FROM items b) d",1);
    const sqlparser_id target=lateral_named("d");
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,target,&owner,&lateral_frame,&error),TURBODB_STATUS_OK);
    orm_sql_query_scope child=scope; child.root=sqlparser_get_node(document,target)->as.table.query;
    child.outer_schema=&lateral_frame.schema; const turbodb_value_t captured=turbodb_i64(2);
    check_equal(orm_sql_lateral_query_open(&child,&owner,parameters,NULL,&lateral_query,&error),TURBODB_STATUS_OK);
    reserves=resizes=0;
    check_equal(lateral_query.binding.open(&lateral_query,&captured,1,&error),TURBODB_STATUS_OK);
    const size_t counts[]={reserves,resizes};
    check_equal(lateral_query.binding.close(&lateral_query,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_lateral_query_close(&lateral_query,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; const size_t leases=owner.active_sources;
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=counts[pass];++point) {
      matrix_allowance();
      check_equal(orm_sql_lateral_query_open(&child,&owner,parameters,NULL,&lateral_query,&error),TURBODB_STATUS_OK);
      reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(lateral_query.binding.open(&lateral_query,&captured,1,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      fail_resize=fail_reserve=0; check_false(lateral_query.bound); check_true(lateral_query.query.execution_closed);
      check_equal(((const turbodb_value_t *)vec_at_const(&lateral_query.arguments.values,1))->kind,TURBODB_VALUE_NULL);
      check_equal(lateral_query.binding.close(&lateral_query,&error),TURBODB_STATUS_OK);
      check_equal(orm_sql_lateral_query_close(&lateral_query,&error),TURBODB_STATUS_OK);
      check_equal(owner.active_sources,leases); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("enforces every dependent query round step limit without retaining captures") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    parse_scope("SELECT d.n FROM items a,LATERAL (SELECT a.id+? AS n) d",1);
    const sqlparser_id target=lateral_named("d");
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,target,&owner,&lateral_frame,&error),TURBODB_STATUS_OK);
    orm_sql_query_scope child=scope; child.root=sqlparser_get_node(document,target)->as.table.query;
    child.outer_schema=&lateral_frame.schema; const turbodb_value_t captured=turbodb_i64(2);
    check_equal(orm_sql_lateral_query_open(&child,&owner,parameters,NULL,&lateral_query,&error),TURBODB_STATUS_OK);
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(lateral_query.binding.open(&lateral_query,&captured,1,&error),TURBODB_STATUS_OK);
    const uint64_t cost=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    check_equal(lateral_query.binding.close(&lateral_query,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_lateral_query_close(&lateral_query,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; const size_t leases=owner.active_sources;
    for(uint64_t allowance=0;allowance<cost;++allowance) {
      matrix_allowance();
      check_equal(orm_sql_lateral_query_open(&child,&owner,parameters,NULL,&lateral_query,&error),TURBODB_STATUS_OK);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+allowance;
      check_equal(lateral_query.binding.open(&lateral_query,&captured,1,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_false(lateral_query.bound); check_true(lateral_query.query.execution_closed);
      check_equal(((const turbodb_value_t *)vec_at_const(&lateral_query.arguments.values,1))->kind,TURBODB_VALUE_NULL);
      check_equal(lateral_query.binding.close(&lateral_query,&error),TURBODB_STATUS_OK);
      check_equal(orm_sql_lateral_query_close(&lateral_query,&error),TURBODB_STATUS_OK);
      check_equal(owner.active_sources,leases); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("enforces each dependent query construction step limit and refunds retained plans") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    parse_scope("SELECT d.n FROM items a,LATERAL (SELECT a.id+? AS n) d",1);
    const sqlparser_id target=lateral_named("d");
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,target,&owner,&lateral_frame,&error),TURBODB_STATUS_OK);
    orm_sql_query_scope child=scope; child.root=sqlparser_get_node(document,target)->as.table.query;
    child.outer_schema=&lateral_frame.schema;
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_sql_lateral_query_open(&child,&owner,parameters,NULL,&lateral_query,&error),TURBODB_STATUS_OK);
    const uint64_t cost=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    check_equal(orm_sql_lateral_query_close(&lateral_query,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; const size_t leases=owner.active_sources;
    for(uint64_t allowance=0;allowance<cost;++allowance) {
      matrix_allowance();
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+allowance;
      check_equal(orm_sql_lateral_query_open(&child,&owner,parameters,NULL,&lateral_query,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_null(lateral_query.budget); check_equal(owner.active_sources,leases); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("executes a zero-capture dependent query and preserves closed-source output on error") {
    parse_scope("SELECT 5 AS n",0);
    check_equal(orm_sql_lateral_query_open(&scope,&owner,NULL,NULL,&lateral_query,&error),TURBODB_STATUS_OK);
    free_ast(); check_equal(lateral_query.capture_count,0u);
    const turbodb_value_t sentinel=turbodb_i64(99); const turbodb_value_t *row=&sentinel;
    check_equal(lateral_query.source.next(&lateral_query,&row,&error),TURBODB_STATUS_INVALID_STATE); check_true(row==&sentinel);
    check_equal(lateral_query.binding.open(&lateral_query,NULL,0,&error),TURBODB_STATUS_OK);
    check_equal(lateral_query.source.next(&lateral_query,&row,&error),TURBODB_STATUS_OK); check_not_null(row);
    check_equal(row[0].data.int64_value,5);
    check_equal(lateral_query.source.next(&lateral_query,&row,&error),TURBODB_STATUS_OK); check_null(row);
    check_equal(lateral_query.binding.close(&lateral_query,&error),TURBODB_STATUS_OK);
    check_equal(lateral_query.binding.open(&lateral_query,NULL,0,&error),TURBODB_STATUS_OK);
    check_equal(lateral_query.source.next(&lateral_query,&row,&error),TURBODB_STATUS_OK); check_not_null(row);
    check_equal(row[0].data.int64_value,5);
  }
  it("rejects malformed dependent frames registries and overflowing argument capacities before allocation") {
    parse_scope("SELECT 5 AS n",0);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    orm_sql_table_schema malformed={.count=1}; orm_sql_query_scope child=scope; child.outer_schema=&malformed;
    check_equal(orm_sql_lateral_query_open(&child,&owner,NULL,NULL,&lateral_query,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    const orm_sql_expr_query_sources queries={.count=1};
    check_equal(orm_sql_lateral_query_open(&scope,&owner,NULL,&queries,&lateral_query,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    const orm_sql_schema_column column={.type={TURBODB_VALUE_INT64,false}};
    malformed.columns=&column; malformed.count=SIZE_MAX; child.parameter_count=1;
    check_equal(orm_sql_lateral_query_open(&child,&owner,parameters,NULL,&lateral_query,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(lateral_query.budget); check_equal(owner.active_sources,0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("uses the full composed RIGHT-prefix capture order in a dependent query round") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    parse_scope("SELECT d.n FROM items a JOIN LATERAL (SELECT c.id*10+a.id AS n) d ON TRUE RIGHT JOIN items c ON TRUE",0);
    check_equal(lateral_child_open(),TURBODB_STATUS_OK); free_ast(); check_equal(lateral_query.capture_count,2u);
    const turbodb_value_t captured[][2]={{turbodb_i64(3),turbodb_i64(2)},{turbodb_i64(7),turbodb_i64(5)}};
    const int64_t expected[]={32,75};
    for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) {
      check_equal(lateral_query.binding.open(&lateral_query,captured[i],2,&error),TURBODB_STATUS_OK);
      const turbodb_value_t *row=NULL;
      check_equal(lateral_query.source.next(&lateral_query,&row,&error),TURBODB_STATUS_OK); check_not_null(row);
      check_equal(row[0].data.int64_value,expected[i]);
      check_equal(lateral_query.binding.close(&lateral_query,&error),TURBODB_STATUS_OK);
      for(size_t slot=0;slot<2;++slot)
        check_equal(((const turbodb_value_t *)vec_at_const(&lateral_query.arguments.values,slot))->kind,TURBODB_VALUE_NULL);
    }
  }
  it("orders lateral metadata after preceding derived outputs without evaluating expressions") {
    parse_scope("SELECT e.n FROM (SELECT 2 AS n) a,LATERAL (SELECT a.n+1 AS n) d,LATERAL (SELECT d.n+a.n AS n) e",0);
    const sqlparser_id d=lateral_named("d"),e=lateral_named("e");
    check_equal(lateral_metadata_open(0),TURBODB_STATUS_OK);
    check_true(graph.describe); check_true(graph.lateral_frames); check_equal(graph.derived_count,3u);
    check_equal(graph.prepared,graph.count); check_equal(lateral_output(d)->count,1u); check_equal(lateral_output(e)->count,1u);
    check_equal(lateral_output(e)->columns[0].type.kind,TURBODB_VALUE_INT64);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS],materialized=budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
    free_ast(); orm_sql_scan_row row={0};
    check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_ROW);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],materialized);
    check_equal(orm_sql_dependencies_execution_open(&graph,NULL,0,&error),TURBODB_STATUS_UNSUPPORTED);
    check_equal(orm_sql_dependencies_execution_close(&graph,&error),TURBODB_STATUS_UNSUPPORTED);
  }
  it("binds native RIGHT JOIN lateral prefixes and keeps completed outer nullability") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    const char *sql[]={
        "SELECT d.n FROM LATERAL (SELECT b.id AS n) d RIGHT JOIN items b ON TRUE",
        "SELECT d.n FROM items a LEFT JOIN items b ON a.id=b.id JOIN LATERAL (SELECT b.id AS n) d ON TRUE",
        "SELECT d.n FROM items a JOIN LATERAL (SELECT a.id+c.id AS n) d ON TRUE RIGHT JOIN items c ON TRUE"};
    const bool nullable[]={false,true,false};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); const sqlparser_id d=lateral_named("d");
      check_equal(lateral_metadata_open(0),TURBODB_STATUS_OK);
      check_equal(lateral_output(d)->columns[0].type.nullable,nullable[i]);
      free_ast(); graph_close(); check_equal(owner.active_sources,0u);
    }
  }
  it("propagates lateral captures through nested scalar IN and EXISTS definitions") {
    const char *sql[]={
        "SELECT d.n FROM (SELECT 2 AS n) a,LATERAL (SELECT (SELECT a.n+1) AS n) d",
        "SELECT d.n FROM (SELECT 2 AS n) a,LATERAL (SELECT (SELECT (SELECT a.n+1)) AS n) d",
        "SELECT d.n FROM (SELECT 2 AS n) a,LATERAL (SELECT a.n IN (SELECT a.n) AS n) d",
        "SELECT d.n FROM (SELECT 2 AS n) a,LATERAL (SELECT EXISTS(SELECT 1 WHERE a.n>0) AS n) d"};
    const turbodb_value_kind_t kinds[]={TURBODB_VALUE_INT64,TURBODB_VALUE_INT64,TURBODB_VALUE_BOOLEAN,TURBODB_VALUE_BOOLEAN};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); const sqlparser_id d=lateral_named("d");
      check_equal(lateral_metadata_open(0),TURBODB_STATUS_OK); check_true(graph.query_count>0);
      check_equal(lateral_output(d)->columns[0].type.kind,kinds[i]);
      free_ast(); graph_close(); check_equal(owner.active_sources,0u);
    }
  }
  it("inherits ancestor captures alongside same-level lateral columns") {
    parse_scope("SELECT (SELECT d.n FROM (SELECT 3 AS n) a,LATERAL (SELECT a.n+o.n AS n) d) AS n FROM (SELECT 2 AS n) o",0);
    const sqlparser_id d=lateral_named("d");
    check_equal(lateral_metadata_open(0),TURBODB_STATUS_OK); check_equal(lateral_output(d)->columns[0].type.kind,TURBODB_VALUE_INT64);
    free_ast(); graph_close(); check_equal(owner.active_sources,0u);
  }
  it("inherits lateral frame captures through ordinary derived and CTE boundaries") {
    const char *sql[]={
        "SELECT d.n FROM (SELECT 2 AS n) a,LATERAL (SELECT q.n FROM (SELECT a.n+1 AS n) q) d",
        "SELECT d.n FROM (SELECT 2 AS n) a,LATERAL (WITH q AS (SELECT a.n+1 AS n) SELECT n FROM q) d",
        "WITH q AS (SELECT 2 AS n) SELECT d.n FROM q a,LATERAL (SELECT a.n+1 AS n) d"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); const sqlparser_id d=lateral_named("d");
      check_equal(lateral_metadata_open(0),TURBODB_STATUS_OK); check_equal(lateral_output(d)->columns[0].type.kind,TURBODB_VALUE_INT64);
      free_ast(); graph_close(); check_equal(owner.active_sources,0u);
    }
  }
  it("prepares nested lateral definition frames before their consuming definitions") {
    parse_scope("SELECT d.n FROM (SELECT 2 AS n) a,LATERAL (SELECT e.n FROM (SELECT 3 AS n) b,LATERAL (SELECT a.n+b.n AS n) e) d",0);
    const sqlparser_id d=lateral_named("d"),e=lateral_named("e");
    check_equal(lateral_metadata_open(0),TURBODB_STATUS_OK); check_equal(lateral_output(d)->count,1u); check_equal(lateral_output(e)->count,1u);
    free_ast(); graph_close(); check_equal(owner.active_sources,0u);
  }
  it("retains full-statement marker numbering across lateral metadata dependencies") {
    parse_scope("SELECT ? AS ignored,e.n FROM (SELECT ? AS n) a,LATERAL (SELECT ?+a.n AS n) d,LATERAL (SELECT ?+d.n AS n) e",PARAMETERS);
    check_equal(lateral_metadata_open(0),TURBODB_STATUS_OK); check_equal(graph.derived_count,3u);
    free_ast(); graph_close(); check_equal(owner.active_sources,0u);
  }
  it("rejects future self and non-lateral sibling captures in metadata graph binding") {
    const char *sql[]={
        "SELECT d.n FROM LATERAL (SELECT a.n AS n) d,(SELECT 2 AS n) a",
        "SELECT d.n FROM (SELECT 2 AS n) a RIGHT JOIN LATERAL (SELECT a.n AS n) d ON TRUE",
        "SELECT d.n FROM LATERAL (SELECT d.n AS n) d",
        "SELECT d.n FROM (SELECT 2 AS n) a,(SELECT a.n AS n) d",
        "SELECT d.n FROM LATERAL (SELECT e.n AS n) d,LATERAL (SELECT d.n AS n) e"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(lateral_metadata_open(0),TURBODB_STATUS_SQL_ERROR); graph_close();
      check_equal(owner.active_sources,0u); check_false(owner.failed); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      free_ast();
    }
  }
  it("preserves same-level ambiguity and nearest-qualifier shadowing inside lateral") {
    const char *sql[]={
        "SELECT d.n FROM (SELECT 2 AS n) a JOIN LATERAL (SELECT n) d ON TRUE RIGHT JOIN (SELECT 3 AS n) c ON TRUE",
        "SELECT d.n FROM (SELECT 2 AS n) a,LATERAL (SELECT (SELECT a.n FROM (SELECT 3 AS x) a) AS n) d"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(lateral_metadata_open(0),TURBODB_STATUS_SQL_ERROR);
      check_contains(error.message,i?"unknown column":"ambiguous"); graph_close(); free_ast();
      check_equal(owner.active_sources,0u); check_false(owner.failed);
    }
  }
  it("does not evaluate arithmetic overflow or instantiate CTE caches during lateral compilation") {
    parse_scope("SELECT d.n FROM (SELECT 2 AS n) a,LATERAL (WITH q AS (SELECT a.n+9223372036854775807 AS n) SELECT (SELECT n FROM q) AS n FROM q) d",0);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS],materialized=budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
    check_equal(lateral_metadata_open(0),TURBODB_STATUS_OK);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],materialized);
    free_ast(); graph_close(); check_equal(owner.active_sources,0u);
  }
  it("preserves unsupported division rejection and closes lateral metadata frames") {
    parse_scope("SELECT d.n FROM (SELECT 2 AS n) a,LATERAL (SELECT a.n/0 AS n) d",0);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(lateral_metadata_open(0),TURBODB_STATUS_UNSUPPORTED); graph_close();
    check_equal(owner.active_sources,0u); check_false(owner.failed); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("compiles recursive CTE captures within lateral under an explicit iteration bound") {
    parse_scope("SELECT d.n FROM (SELECT 2 AS n) a,LATERAL (WITH RECURSIVE c(n) AS (SELECT a.n UNION ALL SELECT n+1 FROM c WHERE n<a.n+1) SELECT MAX(n) AS n FROM c) d",0);
    const sqlparser_id d=lateral_named("d"); const uint64_t materialized=budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
    check_equal(lateral_metadata_open(DEPTH),TURBODB_STATUS_OK); check_equal(lateral_output(d)->columns[0].type.kind,TURBODB_VALUE_INT64);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],materialized);
    free_ast(); graph_close(); check_equal(owner.active_sources,0u);
  }
  it("retains recursive rejection without a positive lateral compilation bound") {
    parse_scope("SELECT d.n FROM (SELECT 2 AS n) a,LATERAL (WITH RECURSIVE c(n) AS (SELECT a.n UNION ALL SELECT n+1 FROM c WHERE n<a.n+1) SELECT MAX(n) AS n FROM c) d",0);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(lateral_metadata_open(0),TURBODB_STATUS_UNSUPPORTED); graph_close();
    check_equal(owner.active_sources,0u); check_false(owner.failed); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("refunds each lateral dependency graph allocation and its partially prepared frames") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    const char *sql[]={
        "SELECT e.n FROM (SELECT 2 AS n) a,LATERAL (SELECT (SELECT a.n+1) AS n) d,LATERAL (SELECT d.n AS n) e",
        "SELECT d.n FROM items a JOIN LATERAL (WITH q AS (SELECT a.id+c.id AS n) SELECT n FROM q) d ON TRUE RIGHT JOIN items c ON TRUE"};
    for(size_t query=0;query<sizeof(sql)/sizeof(sql[0]);++query) {
      parse_scope(sql[query],0); matrix_allowance(); reserves=resizes=0; check_equal(lateral_metadata_open(0),TURBODB_STATUS_OK);
      const size_t counts[]={reserves,resizes}; graph_close(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=counts[pass];++point) {
        matrix_allowance(); reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
        const turbodb_status_t status=lateral_metadata_open(0);
        if(status!=TURBODB_STATUS_OUT_OF_MEMORY)
          info("allocation pass %zu point %zu/%zu, reserve calls %zu, resize calls %zu",pass,point,counts[pass],reserves,resizes);
        check_equal(status,TURBODB_STATUS_OUT_OF_MEMORY); fail_resize=fail_reserve=0;
        graph_close(); check_equal(owner.active_sources,0u); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      free_ast();
    }
  }
  it("refunds lateral graph frames at every construction step boundary") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    const char *sql[]={"SELECT d.n FROM (SELECT 2 AS n) a,LATERAL (SELECT a.n+1 AS n) d",
        "SELECT d.n FROM items a,LATERAL (SELECT (SELECT a.id+1) AS n) d"};
    for(size_t query=0;query<sizeof(sql)/sizeof(sql[0]);++query) {
      parse_scope(sql[query],0); matrix_allowance();
      const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(lateral_metadata_open(0),TURBODB_STATUS_OK);
      const uint64_t cost=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
      graph_close(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(uint64_t allowance=0;allowance<cost;++allowance) {
        matrix_allowance();
        budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+allowance;
        check_equal(lateral_metadata_open(0),TURBODB_STATUS_LIMIT_EXCEEDED); graph_close();
        check_equal(owner.active_sources,0u); check_false(owner.failed); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      free_ast();
    }
  }
  it("validates grouped root consumers after lateral schemas publish") {
    parse_scope("SELECT a.n AS a_n,(SELECT d.n) AS n FROM (SELECT 2 AS n) a,LATERAL (SELECT a.n+1 AS n) d GROUP BY a.n,d.n",0);
    check_equal(lateral_metadata_open(0),TURBODB_STATUS_OK); free_ast(); graph_close();
    parse_scope("SELECT a.n AS a_n,(SELECT d.n) AS n FROM (SELECT 2 AS n) a,LATERAL (SELECT a.n+1 AS n) d GROUP BY a.n",0);
    check_equal(lateral_metadata_open(0),TURBODB_STATUS_SQL_ERROR); check_contains(error.message,"not a GROUP BY key");
    graph_close(); free_ast(); check_equal(owner.active_sources,0u); check_false(owner.failed);
  }
  it("builds a lateral frame from completed joined metadata without evaluating any query") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1)");
    parse_scope("SELECT d.n FROM items a LEFT JOIN items b ON a.id=b.id JOIN LATERAL (SELECT a.id/0 AS n) d ON TRUE JOIN missing c ON TRUE",0);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,lateral_named("d"),&owner,&lateral_frame,&error),TURBODB_STATUS_OK);
    check_equal(vec_size(&lateral_frame.inputs),1u); check_equal(lateral_frame.schema.count,2u);
    check_equal(lateral_frame.schema.columns[0].qualifier.data[0],'a'); check_false(lateral_frame.schema.columns[0].type.nullable);
    check_equal(lateral_frame.schema.columns[1].qualifier.data[0],'b'); check_true(lateral_frame.schema.columns[1].type.nullable);
    check_equal(owner.active_sources,2u);
    const orm_sql_query *input=vec_at_const(&lateral_frame.inputs,0);
    check_true(input->execution_closed); check_null(input->as.select.plan.budget);
    for(size_t i=0;i<vec_size(&input->as.select.relations);++i)
      check_null(((const orm_sql_relation_source *)vec_at_const(&input->as.select.relations,i))->iterator);
    free_ast();
    check_equal(memcmp(lateral_frame.schema.columns[0].name.data,"id",2),0);
    check_equal(memcmp(lateral_frame.schema.columns[1].qualifier.data,"b",1),0);
    check_equal(orm_sql_runtime_lateral_schema_close(&lateral_frame,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_lateral_schema_close(&lateral_frame,&error),TURBODB_STATUS_OK);
    check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("combines ancestor RIGHT JOIN prefixes at one lateral lexical depth") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY,x BIGINT)");
    parse_scope("SELECT d.n FROM items a LEFT JOIN LATERAL (SELECT a.id+c.id AS n) d ON TRUE RIGHT JOIN items c ON TRUE",0);
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,lateral_named("d"),&owner,&lateral_frame,&error),TURBODB_STATUS_OK);
    check_equal(vec_size(&lateral_frame.inputs),2u); check_equal(lateral_frame.schema.count,4u);
    const char qualifiers[]={'c','a'};
    for(size_t i=0;i<sizeof(qualifiers)/sizeof(qualifiers[0]);++i) {
      const orm_sql_schema_column *column=&lateral_frame.schema.columns[i*2];
      check_equal(column->qualifier.data[0],qualifiers[i]); check_equal(column->lexical_depth,0u);
      check_false(column->type.nullable); check_null(column->capture_used);
      const orm_sql_schema_column *second=&lateral_frame.schema.columns[i*2+1];
      check_equal(second->qualifier.data[0],qualifiers[i]); check_equal(second->name.data[0],'x');
      check_true(second->type.nullable); check_equal(second->lexical_depth,0u);
      const orm_sql_query *input=vec_at_const(&lateral_frame.inputs,i);
      check_true(input->execution_closed);
      check_null(((const orm_sql_relation_source *)vec_at_const(&input->as.select.relations,0))->iterator);
    }
    check_equal(owner.active_sources,2u);
  }
  it("copies preceding declared lateral output into the next frame without opening its source") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    parse_scope("SELECT e.n FROM items a,LATERAL (SELECT a.id/0 AS n) d,LATERAL (SELECT d.n) e",0);
    orm_sql_schema_column output={vstr_from_cstr("n"),{TURBODB_VALUE_INT64,true}};
    orm_sql_table_schema declaration={vstr_from_cstr("d"),&output,1};
    orm_sql_derived_binding prepared={lateral_named("d"),&declaration,NULL};
    scope.derived=&prepared; scope.derived_count=1;
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,lateral_named("e"),&owner,&lateral_frame,&error),TURBODB_STATUS_OK);
    check_equal(lateral_frame.schema.count,2u); check_equal(owner.active_sources,1u);
    memset(&output,0,sizeof(output)); memset(&declaration,0,sizeof(declaration)); memset(&prepared,0,sizeof(prepared)); free_ast();
    check_equal(lateral_frame.schema.columns[0].qualifier.data[0],'a');
    check_equal(lateral_frame.schema.columns[1].qualifier.data[0],'d');
    check_equal(lateral_frame.schema.columns[1].name.data[0],'n'); check_true(lateral_frame.schema.columns[1].type.nullable);
    check_equal(lateral_frame.schema.columns[1].lexical_depth,0u);
  }
  it("keeps a first lateral frame empty and excludes later sources from Catalog lookup") {
    parse_scope("SELECT d.n FROM missing a RIGHT JOIN LATERAL (SELECT 1/0 AS n) d ON TRUE",0);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_BYTES];
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,lateral_named("d"),&owner,&lateral_frame,&error),TURBODB_STATUS_OK);
    check_equal(lateral_frame.schema.count,0u); check_null(lateral_frame.schema.columns);
    check_equal(vec_size(&lateral_frame.inputs),0u); check_equal(owner.active_sources,0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_BYTES],reads);
  }
  it("rejects duplicate table qualifiers across separate lateral prefixes") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    parse_scope("SELECT d.n FROM items a JOIN LATERAL (SELECT a.id AS n) d ON TRUE RIGHT JOIN items a ON TRUE",0);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,lateral_named("d"),&owner,&lateral_frame,&error),TURBODB_STATUS_SQL_ERROR);
    check_contains(error.message,"duplicate LATERAL frame table qualifier");
    check_null(lateral_frame.budget); check_equal(owner.active_sources,0u); check_false(owner.failed);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("closes earlier lateral prefix leases on a missing source while preserving existing consumers") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); open_external("SELECT id FROM items");
    parse_scope("SELECT d.n FROM missing a JOIN LATERAL (SELECT a.id AS n) d ON TRUE RIGHT JOIN items c ON TRUE",0);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; check_equal(owner.active_sources,1u);
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,lateral_named("d"),&owner,&lateral_frame,&error),TURBODB_STATUS_SQL_ERROR);
    check_null(lateral_frame.budget); check_equal(owner.active_sources,1u); check_false(owner.failed);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("rejects a foreign lateral identity before reading Catalog metadata") {
    parse_scope("SELECT q.n FROM missing a JOIN (SELECT d.n FROM missing b,LATERAL (SELECT 1 AS n) d) q ON TRUE",0);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],reads=budget.used.value[ORM_SQL_BUDGET_READ_BYTES];
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,lateral_named("d"),&owner,&lateral_frame,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_null(lateral_frame.budget); check_equal(owner.active_sources,0u); check_false(owner.failed);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_BYTES],reads);
  }
  it("refunds every multi-prefix lateral frame allocation failure") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    parse_scope("SELECT d.n FROM items a JOIN LATERAL (SELECT a.id+c.id AS n) d ON TRUE RIGHT JOIN items c ON TRUE",0);
    const sqlparser_id target=lateral_named("d"); reserves=resizes=0;
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,target,&owner,&lateral_frame,&error),TURBODB_STATUS_OK);
    const size_t counts[]={reserves,resizes}; check_equal(orm_sql_runtime_lateral_schema_close(&lateral_frame,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=counts[pass];++point) {
      matrix_allowance(); reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(orm_sql_runtime_lateral_schema_open(&scope,target,&owner,&lateral_frame,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      fail_resize=fail_reserve=0; check_null(lateral_frame.budget); check_equal(owner.active_sources,0u);
      check_false(owner.failed); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("refunds lateral frame work and leases at every execution-step boundary") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    parse_scope("SELECT d.n FROM items a JOIN LATERAL (SELECT a.id+c.id AS n) d ON TRUE RIGHT JOIN items c ON TRUE",0);
    const sqlparser_id target=lateral_named("d");
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,target,&owner,&lateral_frame,&error),TURBODB_STATUS_OK);
    const uint64_t cost=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    check_equal(orm_sql_runtime_lateral_schema_close(&lateral_frame,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(uint64_t allowance=0;allowance<cost;++allowance) {
      matrix_allowance();
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+allowance;
      check_equal(orm_sql_runtime_lateral_schema_open(&scope,target,&owner,&lateral_frame,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_null(lateral_frame.budget); check_equal(owner.active_sources,0u); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("enforces the exact peak WORK capacity of a multi-prefix lateral frame") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    parse_scope("SELECT d.n FROM items a JOIN LATERAL (SELECT a.id+c.id AS n) d ON TRUE RIGHT JOIN items c ON TRUE",0);
    const sqlparser_id target=lateral_named("d");
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES]=work;
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,target,&owner,&lateral_frame,&error),TURBODB_STATUS_OK);
    const uint64_t peak=budget.peak.value[ORM_SQL_BUDGET_WORK_BYTES]; check_true(peak>work);
    check_equal(orm_sql_runtime_lateral_schema_close(&lateral_frame,&error),TURBODB_STATUS_OK);
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=peak-1;
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,target,&owner,&lateral_frame,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(lateral_frame.budget); check_equal(owner.active_sources,0u); check_false(owner.failed);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    budget.limits.statement.value[ORM_SQL_BUDGET_WORK_BYTES]=peak;
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,target,&owner,&lateral_frame,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_lateral_schema_close(&lateral_frame,&error),TURBODB_STATUS_OK);
    check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("binds lateral capture slots after full-document markers and reopens the compiled child after metadata dies") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    parse_scope("SELECT ? AS ignored,d.n FROM items a JOIN LATERAL (SELECT ?+a.id+c.id AS n) d ON ? RIGHT JOIN items c ON ?",PARAMETERS);
    const sqlparser_id target=lateral_named("d");
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,target,&owner,&lateral_frame,&error),TURBODB_STATUS_OK);
    orm_sql_query_scope child=scope; child.root=sqlparser_get_node(document,target)->as.table.query;
    child.outer_schema=&lateral_frame.schema; child.defer_execution=true;
    check_equal(orm_sql_runtime_scope_open(&child,&owner,parameters,false,NULL,&graph_root,&error),TURBODB_STATUS_OK);
    check_true(graph_root.as.select.plan.correlated); check_true(graph_root.execution_closed);
    const turbodb_value_t arguments[][PARAMETERS+2]={
        {turbodb_i64(100),turbodb_i64(7),turbodb_i64(1),turbodb_i64(1),turbodb_i64(10),turbodb_i64(2)},
        {turbodb_i64(100),turbodb_i64(8),turbodb_i64(1),turbodb_i64(1),turbodb_i64(20),turbodb_i64(3)}};
    const int64_t expected[]={19,31};
    check_equal(vec_size(&graph_root.as.select.plan.parameter_types),sizeof(arguments[0])/sizeof(arguments[0][0]));
    check_equal(orm_sql_runtime_lateral_schema_close(&lateral_frame,&error),TURBODB_STATUS_OK); free_ast();
    check_equal(owner.active_sources,1u);
    for(size_t i=0;i<sizeof(expected)/sizeof(expected[0]);++i) {
      check_equal(orm_sql_runtime_execution_open(&graph_root,arguments[i],sizeof(arguments[i])/sizeof(arguments[i][0]),NULL,&error),TURBODB_STATUS_OK);
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,expected[i]);
      check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
      check_equal(orm_sql_runtime_execution_close(&graph_root,&error),TURBODB_STATUS_OK);
    }
  }
  it("rejects lateral child bindings to a future RIGHT JOIN source") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    parse_scope("SELECT d.n FROM items a RIGHT JOIN LATERAL (SELECT a.id AS n) d ON TRUE",0);
    const sqlparser_id target=lateral_named("d");
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,target,&owner,&lateral_frame,&error),TURBODB_STATUS_OK);
    check_equal(lateral_frame.schema.count,0u);
    orm_sql_query_scope child=scope; child.root=sqlparser_get_node(document,target)->as.table.query;
    child.outer_schema=&lateral_frame.schema; child.defer_execution=true;
    check_equal(orm_sql_runtime_scope_open(&child,&owner,NULL,false,NULL,&graph_root,&error),TURBODB_STATUS_SQL_ERROR);
    check_null(graph_root.owner); check_equal(owner.active_sources,0u); check_false(owner.failed);
  }
  it("keeps unqualified references ambiguous across all same-level lateral prefixes") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    parse_scope("SELECT d.n FROM items a JOIN LATERAL (SELECT id AS n) d ON TRUE RIGHT JOIN items c ON TRUE",0);
    const sqlparser_id target=lateral_named("d");
    check_equal(orm_sql_runtime_lateral_schema_open(&scope,target,&owner,&lateral_frame,&error),TURBODB_STATUS_OK);
    orm_sql_query_scope child=scope; child.root=sqlparser_get_node(document,target)->as.table.query;
    child.outer_schema=&lateral_frame.schema; child.defer_execution=true;
    check_equal(orm_sql_runtime_scope_open(&child,&owner,NULL,false,NULL,&graph_root,&error),TURBODB_STATUS_SQL_ERROR);
    check_contains(error.message,"ambiguous"); check_null(graph_root.owner); check_equal(owner.active_sources,2u);
    check_false(owner.failed);
  }
  it("prepares a native FROM prefix without binding its lateral query or later missing table") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1)");
    parse_scope("SELECT d.n FROM items a LEFT JOIN items b ON a.id=b.id JOIN LATERAL (SELECT a.id/0 AS n) d ON TRUE JOIN missing c ON TRUE",0);
    const sqlparser_id from=sqlparser_get_node(document,scope.root)->as.select.from;
    const sqlparser_id lateral_join_id=sqlparser_get_node(document,from)->as.join.left;
    const sqlparser_id subtree=sqlparser_get_node(document,lateral_join_id)->as.join.left;
    const orm_sql_table_schema *frame_schema=NULL; vstr qualifier={0};
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(orm_sql_runtime_subtree_schema_open(&scope,subtree,&owner,&graph_root,&frame_schema,&qualifier,&error),TURBODB_STATUS_OK);
    check_not_null(frame_schema); check_equal(frame_schema->count,2u); check_equal(qualifier.len,0u);
    check_equal(frame_schema->columns[0].qualifier.data[0],'a'); check_false(frame_schema->columns[0].type.nullable);
    check_equal(frame_schema->columns[1].qualifier.data[0],'b'); check_true(frame_schema->columns[1].type.nullable);
    check_equal(owner.active_sources,2u); check_true(graph_root.execution_closed);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    for(size_t i=0;i<vec_size(&graph_root.as.select.relations);++i)
      check_null(((const orm_sql_relation_source *)vec_at_const(&graph_root.as.select.relations,i))->iterator);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_sql_runtime_execution_open(&graph_root,NULL,0,NULL,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    check_true(graph_root.execution_closed); free_ast();
    check_equal(frame_schema->columns[0].name.len,2u); check_equal(frame_schema->columns[0].qualifier.data[0],'a');
    check_equal(orm_tidesdb_sql_runtime_close(&graph_root,&error),TURBODB_STATUS_OK);
    check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }

  it("combines prepared derived metadata with native input without opening business sources") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    parse_scope("SELECT d.n FROM (SELECT missing.id FROM missing) q RIGHT JOIN items a ON q.n=a.id JOIN LATERAL (SELECT a.id AS n) d ON TRUE",0);
    const sqlparser_id from=sqlparser_get_node(document,scope.root)->as.select.from;
    const sqlparser_id subtree=sqlparser_get_node(document,from)->as.join.left;
    const sqlparser_id derived=sqlparser_get_node(document,subtree)->as.join.left;
    orm_sql_schema_column column={vstr_from_cstr("n"),{TURBODB_VALUE_INT64,false}};
    orm_sql_table_schema declared={vstr_from_cstr("q"),&column,1};
    orm_sql_derived_binding binding={derived,&declared,NULL}; scope.derived=&binding; scope.derived_count=1;
    const orm_sql_table_schema *frame_schema=NULL; vstr qualifier={0};
    check_equal(orm_sql_runtime_subtree_schema_open(&scope,subtree,&owner,&graph_root,&frame_schema,&qualifier,&error),TURBODB_STATUS_OK);
    check_equal(frame_schema->count,2u); check_true(frame_schema->columns[0].type.nullable);
    check_false(frame_schema->columns[1].type.nullable); check_equal(owner.active_sources,1u);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    column=(orm_sql_schema_column){0}; declared=(orm_sql_table_schema){0}; binding=(orm_sql_derived_binding){0};
    free_ast(); check_equal(frame_schema->columns[0].name.data[0],'n'); check_equal(frame_schema->columns[0].qualifier.data[0],'q');
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    check_null(((const orm_sql_relation_source *)vec_at_const(&graph_root.as.select.relations,1))->iterator);
  }

  it("prepares lateral occurrence schema only from its published declaration") {
    parse_scope("SELECT d.n FROM missing a,LATERAL (SELECT a.id/0 AS n) d",0);
    const sqlparser_id subtree=sqlparser_get_node(document,
        sqlparser_get_node(document,scope.root)->as.select.from)->as.join.right;
    const orm_sql_schema_column column={vstr_from_cstr("n"),{TURBODB_VALUE_INT64,false}};
    const orm_sql_table_schema declared={vstr_from_cstr("d"),&column,1};
    const orm_sql_derived_binding binding={subtree,&declared,NULL}; scope.derived=&binding; scope.derived_count=1;
    const orm_sql_table_schema *frame_schema=NULL; vstr qualifier={0};
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(orm_sql_runtime_subtree_schema_open(&scope,subtree,&owner,&graph_root,&frame_schema,&qualifier,&error),TURBODB_STATUS_OK);
    check_equal(frame_schema->count,1u); check_equal(frame_schema->columns[0].qualifier.data[0],'d');
    check_true(graph_root.as.select.from.contains_lateral); check_equal(owner.active_sources,0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
  }

  it("rejects non-FROM subtree identities before Catalog reads or metadata publication") {
    parse_scope("SELECT (SELECT id FROM missing) AS n FROM missing other",0);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_BYTES];
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(size_t i=1;i<=sqlparser_node_count(document);++i) {
      const sqlparser_node *node=sqlparser_get_node(document,(sqlparser_id)i);
      if(node->kind!=SQLPARSER_TABLE && node->kind!=SQLPARSER_SELECT) continue;
      if((sqlparser_id)i==sqlparser_get_node(document,scope.root)->as.select.from) continue;
      const orm_sql_table_schema *frame_schema=NULL; vstr qualifier={0};
      check_equal(orm_sql_runtime_subtree_schema_open(&scope,(sqlparser_id)i,&owner,&graph_root,&frame_schema,&qualifier,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      check_null(frame_schema); check_equal(qualifier.len,0u); check_null(graph_root.owner);
      check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_BYTES],reads);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }

  it("refunds every native prefix metadata allocation and newly opened source lease") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    parse_scope("SELECT d.n FROM items a LEFT JOIN items b ON a.id=b.id JOIN LATERAL (SELECT a.id AS n) d ON TRUE",0);
    const sqlparser_id subtree=sqlparser_get_node(document,
        sqlparser_get_node(document,scope.root)->as.select.from)->as.join.left;
    const orm_sql_table_schema *frame_schema=NULL; vstr qualifier={0}; reserves=resizes=0;
    check_equal(orm_sql_runtime_subtree_schema_open(&scope,subtree,&owner,&graph_root,&frame_schema,&qualifier,&error),TURBODB_STATUS_OK);
    const size_t counts[]={reserves,resizes}; check_equal(orm_tidesdb_sql_runtime_close(&graph_root,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=counts[pass];++point) {
      matrix_allowance(); reserves=resizes=0; frame_schema=NULL; qualifier=(vstr){0};
      if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(orm_sql_runtime_subtree_schema_open(&scope,subtree,&owner,&graph_root,&frame_schema,&qualifier,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      fail_resize=fail_reserve=0; check_null(graph_root.owner); check_null(frame_schema); check_equal(qualifier.len,0u);
      check_equal(owner.active_sources,0u); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }

  it("enforces every native prefix metadata step limit without leaking sources or schema") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    parse_scope("SELECT d.n FROM items a LEFT JOIN items b ON a.id=b.id JOIN LATERAL (SELECT a.id AS n) d ON TRUE",0);
    const sqlparser_id subtree=sqlparser_get_node(document,
        sqlparser_get_node(document,scope.root)->as.select.from)->as.join.left;
    const orm_sql_table_schema *frame_schema=NULL; vstr qualifier={0};
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(orm_sql_runtime_subtree_schema_open(&scope,subtree,&owner,&graph_root,&frame_schema,&qualifier,&error),TURBODB_STATUS_OK);
    const uint64_t cost=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    check_equal(orm_tidesdb_sql_runtime_close(&graph_root,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(uint64_t allowance=0;allowance<cost;++allowance) {
      matrix_allowance(); frame_schema=NULL; qualifier=(vstr){0};
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+allowance;
      check_equal(orm_sql_runtime_subtree_schema_open(&scope,subtree,&owner,&graph_root,&frame_schema,&qualifier,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_null(graph_root.owner); check_null(frame_schema); check_equal(qualifier.len,0u); check_equal(owner.active_sources,0u);
      check_false(owner.failed); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("captures recursive member rows in nested scalar predicates and projections") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS (SELECT (SELECT 1) UNION ALL SELECT (SELECT c.n+1) FROM c WHERE (SELECT c.n<3)) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT (SELECT (SELECT c.n+1)) FROM c WHERE EXISTS(SELECT 1 WHERE c.n<3)) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT c.n+1 FROM c WHERE c.n IN (SELECT (SELECT c.n) WHERE c.n<3)) SELECT n FROM c"
    };
    const int64_t expected[]={1,2,3};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      graph_values(expected,sizeof(expected)/sizeof(expected[0])); graph_close();
    }
  }
  it("captures seed and recursive rows alongside ancestor rows and original markers") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parameters[0]=turbodb_i64(2);
    const char *sql[]={
      "SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT (SELECT o.id) UNION ALL SELECT (SELECT c.n+1) FROM c WHERE (SELECT c.n<o.id+?)) SELECT MAX(n) FROM c) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT (SELECT i.id+o.id-1) FROM items i WHERE i.id=1 UNION ALL SELECT (SELECT (SELECT c.n+i.id)) FROM c JOIN items i ON i.id=1 WHERE EXISTS(SELECT 1 WHERE c.n<o.id+?)) SELECT MAX(n) FROM c) AS n FROM items o GROUP BY o.id ORDER BY o.id"
    };
    const int64_t expected[][2]={{1,3},{2,4},{3,5}};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],1); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      parameters[0]=turbodb_i64(99); paired_values(expected,sizeof(expected)/sizeof(expected[0]));
      graph_close(); parameters[0]=turbodb_i64(2);
    }
  }
  it("carries recursive member captures through owned derived and CTE definition boundaries") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    const char *sql[]={
      "SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.id UNION ALL SELECT (SELECT d.x FROM (SELECT (SELECT c.n+1) AS x) d) FROM c WHERE (SELECT c.n<o.id+2)) SELECT MAX(n) FROM c) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.id UNION ALL SELECT (WITH d(x) AS (SELECT (SELECT c.n+1)) SELECT (SELECT x FROM d)) FROM c WHERE (SELECT c.n<o.id+2)) SELECT MAX(n) FROM c) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.id UNION ALL SELECT (WITH d(x) AS (SELECT c.n+1) SELECT a.x FROM d a JOIN d b ON a.x=b.x) FROM c WHERE (SELECT c.n<o.id+2)) SELECT MAX(n) FROM c) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT (SELECT o.id) UNION ALL SELECT (SELECT c.n+d.step) FROM c CROSS JOIN (SELECT (SELECT o.id-o.id+1) AS step) d WHERE (SELECT c.n<o.id+2)) SELECT MAX(n) FROM c) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT (SELECT o.id) UNION ALL (WITH d(step) AS (SELECT (SELECT o.id-o.id+1)) SELECT (SELECT c.n+d.step) FROM c CROSS JOIN d WHERE (SELECT c.n<o.id+2))) SELECT MAX(n) FROM c) AS n FROM items o ORDER BY o.id"
    };
    const int64_t expected[][2]={{1,3},{2,4},{3,5}};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      paired_values(expected,sizeof(expected)/sizeof(expected[0])); graph_close();
    }
  }
  it("nests recursive definitions while capturing each enclosing frontier row") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT (WITH RECURSIVE d(x) AS (SELECT (SELECT c.n) UNION ALL SELECT (SELECT d.x+1) FROM d WHERE (SELECT d.x<c.n+1)) SELECT MAX(x) FROM d) FROM c WHERE c.n<3) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT (WITH RECURSIVE d(x) AS (SELECT (SELECT 0) UNION ALL SELECT (SELECT d.x+1) FROM d WHERE (SELECT d.x<1)) SELECT MAX(x) FROM d) UNION ALL SELECT (SELECT c.n+1) FROM c WHERE (SELECT c.n<3)) SELECT n FROM c"
    };
    const int64_t expected[]={1,2,3};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      graph_values(expected,sizeof(expected)/sizeof(expected[0])); graph_close();
    }
  }
  it("preserves nullable TEXT captures through nested recursive callbacks") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT o.id,(WITH RECURSIVE c(n,label) AS (SELECT (SELECT o.id),(SELECT o.label) UNION ALL "
        "SELECT (SELECT c.n+1),(SELECT (SELECT c.label)) FROM c WHERE (SELECT c.n<o.id+1)) "
        "SELECT label FROM c ORDER BY n DESC LIMIT 1) AS label FROM "
        "(SELECT id,CASE id WHEN 1 THEN 'first' WHEN 2 THEN 'second' ELSE NULL END AS label FROM items) o ORDER BY o.id",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const char *labels[]={"first","second",NULL}; vstr retained={0};
    for(size_t i=0;i<sizeof(labels)/sizeof(labels[0]);++i) {
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,(int64_t)i+1);
      text_value_is(row.values[1],labels[i]); if(!i) retained=row.values[1].data.text_value;
    }
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_DONE); check_equal(retained.len,strlen(labels[0]));
    check_equal(memcmp(retained.data,labels[0],retained.len),0);
  }
  it("resolves recursive nested captures against the nearest qualified ancestor") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT o.id,(SELECT (WITH RECURSIVE c(n) AS (SELECT (SELECT o.id) UNION ALL "
        "SELECT (SELECT c.n+1) FROM c WHERE (SELECT c.n<o.id+1)) SELECT MAX(n) FROM c) "
        "FROM items o WHERE o.id=1) AS n FROM items o ORDER BY o.id",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t expected[][2]={{1,2},{2,2},{3,2}};
    paired_values(expected,sizeof(expected)/sizeof(expected[0]));
  }
  it("explains nested recursive captures without evaluating rows or expressions") {
    parse_scope("EXPLAIN WITH RECURSIVE c(n) AS (SELECT (SELECT 1) UNION ALL "
        "SELECT (SELECT c.n+9223372036854775807) FROM c WHERE (SELECT c.n<3)) SELECT n FROM c",0);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    const uint64_t materialized=budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    size_t count=0; orm_sql_scan_row row={0};
    for(;;) {
      check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      if(row.state!=ORM_SQL_SCAN_ROW) { check_equal(row.state,ORM_SQL_SCAN_DONE); break; }
      ++count;
    }
    check_true(count>1); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],materialized);
  }
  it("refunds nested recursive frame construction allocations and execution steps") {
    const char *sql[]={
      "SELECT o.x,(WITH RECURSIVE c(n) AS (SELECT (SELECT o.x) UNION ALL SELECT (WITH d(x) AS (SELECT (SELECT c.n+1)) SELECT (SELECT x FROM d)) FROM c WHERE (SELECT c.n<o.x+1)) SELECT MAX(n) FROM c) AS n FROM (SELECT 1 AS x) o",
      "EXPLAIN SELECT o.x,(WITH RECURSIVE c(n) AS (SELECT (SELECT o.x) UNION ALL SELECT (WITH d(x) AS (SELECT (SELECT c.n+1)) SELECT (SELECT x FROM d)) FROM c WHERE (SELECT c.n<o.x+1)) SELECT MAX(n) FROM c) AS n FROM (SELECT 1 AS x) o",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT (WITH RECURSIVE d(x) AS (SELECT (SELECT c.n) UNION ALL SELECT (SELECT d.x+1) FROM d WHERE (SELECT d.x<c.n+1)) SELECT MAX(x) FROM d) FROM c WHERE c.n<3) SELECT n FROM c",
      "EXPLAIN WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT (WITH RECURSIVE d(x) AS (SELECT (SELECT c.n) UNION ALL SELECT (SELECT d.x+1) FROM d WHERE (SELECT d.x<c.n+1)) SELECT MAX(x) FROM d) FROM c WHERE c.n<3) SELECT n FROM c"
    };
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      matrix_allowance();
      const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      reserves=resizes=0; check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK);
      const size_t allocations[]={reserves,resizes};
      const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start; graph_close();
      for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
        matrix_allowance();
        reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
        check_equal(bounded_open(DEPTH),TURBODB_STATUS_OUT_OF_MEMORY); fail_reserve=fail_resize=0;
        check_null(graph_root.owner); check_equal(owner.active_sources,0u); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      for(uint64_t point=0;point<steps;++point) {
        matrix_allowance();
        budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
        check_equal(bounded_open(DEPTH),TURBODB_STATUS_LIMIT_EXCEEDED);
        matrix_allowance();
        check_null(graph_root.owner); check_equal(owner.active_sources,0u); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      free_ast();
    }
  }
  it("refunds nested recursive callbacks at every allocation and execution step boundary") {
    const char *sql[]={"SELECT o.x,(WITH RECURSIVE c(n) AS (SELECT (SELECT o.x) UNION ALL "
        "SELECT (WITH d(x) AS (SELECT (SELECT c.n+1)) SELECT (SELECT x FROM d)) FROM c "
        "WHERE (SELECT c.n<o.x+1)) SELECT MAX(n) FROM c) AS n FROM (SELECT 1 AS x) o",
        "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT (WITH RECURSIVE d(x) AS (SELECT (SELECT c.n) UNION ALL SELECT (SELECT d.x+1) FROM d WHERE (SELECT d.x<c.n+1)) SELECT MAX(x) FROM d) FROM c WHERE c.n<3) SELECT n FROM c"};
    for(size_t query=0;query<sizeof(sql)/sizeof(sql[0]);++query) {
      parse_scope(sql[query],0);
      matrix_allowance();
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); reserves=resizes=0;
      const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      const size_t allocations[]={reserves,resizes};
      const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start; graph_close();
      for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
        matrix_allowance();
        check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); reserves=resizes=0;
        if(pass) fail_resize=point; else fail_reserve=point;
        row=(orm_sql_scan_row){.state=ORM_SQL_SCAN_CANCELLED,.count=99};
        check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OUT_OF_MEMORY);
        fail_reserve=fail_resize=0; check_equal(row.count,99u); graph_close();
        check_equal(owner.active_sources,0u); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      for(uint64_t point=0;point<steps;++point) {
        matrix_allowance();
        check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK);
        budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
        row=(orm_sql_scan_row){.state=ORM_SQL_SCAN_CANCELLED,.count=99};
        check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
        matrix_allowance();
        check_equal(row.count,99u); graph_close(); check_equal(owner.active_sources,0u); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      free_ast();
    }
  }
  it("keeps nested recursive writes atomic and permits reuse after an iteration failure") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)"); execute("INSERT INTO items VALUES(1,10),(2,20)");
    parse_scope("UPDATE items o SET score=(WITH RECURSIVE c(n) AS (SELECT (SELECT 1) UNION ALL "
        "SELECT (SELECT c.n+1) FROM c WHERE (SELECT c.n<o.id)) SELECT MAX(n) FROM c)",0);
    size_t affected=99;
    check_equal(orm_tidesdb_sql_runtime_execute(document,&owner,NULL,0,DEPTH,1,false,NULL,&affected,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(affected,99u); check_false(owner.failed); free_ast();
    parse_scope("SELECT score FROM items ORDER BY id",0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t unchanged[]={10,20}; graph_values(unchanged,sizeof(unchanged)/sizeof(unchanged[0])); graph_close();
    parse_scope("UPDATE items o SET score=(WITH RECURSIVE c(n) AS (SELECT (SELECT o.id) UNION ALL "
        "SELECT (SELECT c.n+1) FROM c WHERE (SELECT c.n<o.id+1)) SELECT MAX(n) FROM c)",0);
    check_equal(orm_tidesdb_sql_runtime_execute(document,&owner,NULL,0,DEPTH,DEPTH,false,NULL,&affected,&error),TURBODB_STATUS_OK);
    check_equal(affected,2u); free_ast();
    parse_scope("SELECT score FROM items ORDER BY id",0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t changed[]={2,3}; graph_values(changed,sizeof(changed)/sizeof(changed[0])); graph_close();
    parse_scope("DELETE FROM items o WHERE EXISTS(WITH RECURSIVE c(n) AS (SELECT (SELECT o.id) "
        "UNION ALL SELECT (SELECT c.n+1) FROM c WHERE (SELECT c.n<o.id+1)) SELECT n FROM c WHERE o.id=2)",0);
    check_equal(orm_tidesdb_sql_runtime_execute(document,&owner,NULL,0,DEPTH,DEPTH,false,NULL,&affected,&error),TURBODB_STATUS_OK);
    check_equal(affected,1u); free_ast();
    parse_scope("SELECT id FROM items",0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t remaining[]={1}; graph_values(remaining,sizeof(remaining)/sizeof(remaining[0]));
  }
  it("retains nested recursive plans and marker snapshots across statement execution resume") {
    parameters[0]=turbodb_i64(1); parameters[1]=turbodb_i64(3);
    parse_scope("WITH RECURSIVE c(n) AS (SELECT (SELECT ?) UNION ALL "
        "SELECT (SELECT c.n+1) FROM c WHERE (SELECT c.n<?)) SELECT n FROM c",2);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    parameters[0]=parameters[1]=turbodb_i64(99);
    const int64_t expected[]={1,2,3}; graph_values(expected,sizeof(expected)/sizeof(expected[0]));
    const uint64_t materialized=budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
    check_equal(orm_sql_runtime_execution_close(&graph_root,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_execution_resume(&graph_root,&error),TURBODB_STATUS_OK);
    graph_values(expected,sizeof(expected)/sizeof(expected[0]));
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],materialized);
  }
  it("keeps staged recursive seed metadata nonexecutable until member binding completes") {
    parse(sequence,0); orm_sql_cte_query_spec spec=owner_spec();
    spec.rounds=(orm_sql_cte_query_rounds){&hooks,inputs_open,inputs_close};
    check_equal(orm_sql_cte_query_seed_open(&scope,definition,&references,&owner,parameters,&spec,&owned,&error),TURBODB_STATUS_OK);
    check_false(owned.complete); check_true(owned.execution_closed); check_null(owned.cache.budget);
    const orm_sql_select *seed=owned.initial.plan;
    check_equal(orm_sql_cte_query_execution_close(&owned,&error),TURBODB_STATUS_INVALID_STATE);
    check_equal(orm_sql_cte_query_execution_open(&owned,parameters,0,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    for(size_t i=0;i<shape.count;++i) {
      const orm_sql_cte_shape_node *node=vec_at_const(&shape.nodes,i);
      if(node->self) bindings[0]=(orm_sql_derived_binding){node->self,&owned.schema.schema,NULL};
    }
    scope.derived=bindings; scope.derived_count=1;
    check_equal(orm_sql_cte_query_member_open(&scope,definition,&references,&owner,parameters,NULL,&owned,&error),TURBODB_STATUS_OK);
    check_true(owned.complete); check_true(owned.initial.plan==seed); owner_reader();
    const int64_t expected[]={1,2,3}; values(expected,sizeof(expected)/sizeof(expected[0]));
  }
  it("rejects invalid staged recursive self registries without leaking seed metadata") {
    parse(sequence,0); sqlparser_id self=0;
    for(size_t i=0;i<shape.count;++i) {
      const orm_sql_cte_shape_node *node=vec_at_const(&shape.nodes,i); if(node->self) self=node->self;
    }
    check_not_equal(self,0u);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    enum { INVALID_SOURCE,INVALID_SCHEMA,DUPLICATE_SELF,MISSING_REGISTRY,INVALID_CAPTURE,INVALID_COUNT };
    for(size_t i=0;i<INVALID_COUNT;++i) {
      scope.derived=NULL; scope.derived_count=0; scope.outer_schema=NULL;
      orm_sql_cte_query_spec spec=owner_spec(); spec.rounds=(orm_sql_cte_query_rounds){&hooks,inputs_open,inputs_close};
      check_equal(orm_sql_cte_query_seed_open(&scope,definition,&references,&owner,parameters,&spec,&owned,&error),TURBODB_STATUS_OK);
      orm_sql_table_schema wrong=owned.schema.schema; wrong.columns=NULL;
      bindings[0]=(orm_sql_derived_binding){self,i==INVALID_SCHEMA?&wrong:&owned.schema.schema,
          i==INVALID_SOURCE?&external_source:NULL}; bindings[1]=bindings[0];
      scope.derived=i==MISSING_REGISTRY?NULL:bindings; scope.derived_count=i==DUPLICATE_SELF?2:1;
      if(i==INVALID_CAPTURE) scope.outer_schema=&owned.schema.schema;
      check_equal(orm_sql_cte_query_member_open(&scope,definition,&references,&owner,
          i==INVALID_CAPTURE?NULL:parameters,NULL,&owned,&error),TURBODB_STATUS_INVALID_ARGUMENT);
      check_equal(orm_sql_cte_query_close(&owned,&error),TURBODB_STATUS_OK);
      check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    scope.derived=NULL; scope.derived_count=0; scope.outer_schema=NULL;
  }
  it("keeps recursive self table references out of nested queries and sibling definitions") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT (SELECT n FROM c) FROM c) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT d.x FROM c CROSS JOIN (SELECT c.n+1 AS x) d) SELECT n FROM c",
      "SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.id UNION ALL SELECT (SELECT o.score) FROM c WHERE n<2) SELECT MAX(n) FROM c) AS n FROM items o GROUP BY o.id"
    };
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)"); execute("INSERT INTO items VALUES(1,10)");
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_SQL_ERROR);
      check_null(graph_root.owner); check_equal(owner.active_sources,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); free_ast();
    }
  }
  it("captures outer rows in recursive seeds and member predicates") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)");
    execute("INSERT INTO items VALUES(1),(2),(3)");
    const char *queries[]={
      "SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.id UNION ALL SELECT n+1 FROM c WHERE n<o.id+2) SELECT MAX(n) FROM c) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<o.id+2) SELECT MAX(n) FROM c) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.id UNION ALL SELECT n+1 FROM c WHERE n<o.id+2) SELECT (SELECT MAX(n) FROM c)) AS n FROM items o ORDER BY o.id"
    };
    for(size_t i=0;i<sizeof(queries)/sizeof(queries[0]);++i) {
      parse_scope(queries[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      for(int64_t id=1;id<=3;++id) {
        orm_sql_scan_row row={0};
        check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
        check_equal(row.state,ORM_SQL_SCAN_ROW);
        check_equal(row.values[0].data.int64_value,id);
        check_equal(row.values[1].data.int64_value,id+2);
      }
      orm_sql_scan_row row={0};
      check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_DONE); graph_close();
    }
  }
  it("captures recursive frames through grouped parents JOINs and repeated readers") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parameters[0]=turbodb_i64(2);
    const char *queries[]={
      "SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.id UNION ALL SELECT n+1 FROM c WHERE n<o.id+?) SELECT MAX(n) FROM c) AS n FROM items o GROUP BY o.id ORDER BY o.id",
      "SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.id UNION ALL SELECT n+1 FROM c WHERE n<o.id+?) SELECT MAX(a.n) FROM c a JOIN c b ON a.n=b.n) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.id UNION ALL SELECT c.n+i.id FROM c JOIN items i ON i.id=1 WHERE c.n<o.id+?) SELECT MAX(n) FROM c) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.id UNION SELECT n+1 FROM c WHERE n<o.id+?) SELECT MAX(n) FROM c) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.id UNION ALL SELECT n+1 FROM c CROSS JOIN (SELECT 1 AS step) d WHERE n<o.id+?) SELECT MAX(n) FROM c) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(WITH RECURSIVE a(x) AS (SELECT o.id),c(n) AS (SELECT x FROM a UNION ALL SELECT n+1 FROM c WHERE n<o.id+?),d(x) AS (SELECT MAX(n) FROM c) SELECT (SELECT x FROM d)) AS n FROM items o ORDER BY o.id",
      "SELECT o.id,(SELECT (WITH RECURSIVE c(n) AS (SELECT i.id+o.id UNION ALL SELECT n+1 FROM c WHERE n<i.id+o.id+?) SELECT MAX(n) FROM c) FROM items i WHERE i.id=1) AS n FROM items o ORDER BY o.id"
    };
    for(size_t i=0;i<sizeof(queries)/sizeof(queries[0]);++i) {
      parse_scope(queries[i],1); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      for(int64_t id=1;id<=3;++id) {
        orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
        check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,id);
        check_equal(row.values[1].data.int64_value,id+2+(i==sizeof(queries)/sizeof(queries[0])-1));
      }
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_DONE); graph_close();
    }
  }
  it("rebuilds recursive caches that capture only through an earlier CTE") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT o.id,(WITH RECURSIVE a(x) AS (SELECT o.id),c(n) AS "
        "(SELECT x FROM a UNION ALL SELECT n+1 FROM c WHERE n<4) SELECT COUNT(*) FROM c) "
        "AS n FROM items o ORDER BY o.id",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    for(int64_t id=1;id<=3;++id) {
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.values[0].data.int64_value,id); check_equal(row.values[1].data.int64_value,5-id);
    }
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_DONE);
  }
  it("replays recursive captures with owned markers and explains without business reads") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2)");
    parameters[0]=turbodb_i64(2);
    parse_scope("SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.id UNION ALL SELECT n+1 FROM c WHERE n<o.id+?) SELECT MAX(n) FROM c) AS n FROM items o ORDER BY o.id",1);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast(); parameters[0]=turbodb_i64(99);
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.values[1].data.int64_value,3);
    check_equal(orm_sql_runtime_execution_close(&graph_root,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_execution_resume(&graph_root,&error),TURBODB_STATUS_OK);
    for(int64_t id=1;id<=2;++id) {
      check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.values[1].data.int64_value,id+2);
    }
    check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_DONE); graph_close();
    parse_scope("EXPLAIN SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.id UNION ALL SELECT n+1 FROM c WHERE n<o.id+2 LIMIT 3) SELECT MAX(n) FROM c) AS n FROM items o",0);
    const uint64_t materialized=budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],materialized);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    while(true) {
      check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      if(row.state!=ORM_SQL_SCAN_ROW) break;
    }
    check_equal(row.state,ORM_SQL_SCAN_DONE); check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
  }
  it("retains TEXT and nullable outer captures through recursive caches") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)");
    execute("INSERT INTO items VALUES(1,10),(2,20),(3,NULL)");
    parse_scope("SELECT o.id,(WITH RECURSIVE c(label,n) AS (SELECT o.label,o.id UNION ALL "
        "SELECT label,n+1 FROM c WHERE n<o.id+1) SELECT label FROM c ORDER BY n DESC LIMIT 1) "
        "AS label FROM (SELECT id,CASE id WHEN 1 THEN 'first' WHEN 2 THEN 'second' "
        "ELSE 'third' END AS label FROM items) o ORDER BY o.id",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const char *labels[]={"first","second","third"}; vstr retained={0};
    for(size_t i=0;i<sizeof(labels)/sizeof(labels[0]);++i) {
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      text_value_is(row.values[1],labels[i]); if(!i) retained=row.values[1].data.text_value;
    }
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_DONE); check_equal(retained.len,strlen(labels[0]));
    check_equal(memcmp(retained.data,labels[0],retained.len),0); graph_close();
    parse_scope("SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.score UNION ALL SELECT n+1 "
        "FROM c WHERE n<o.score+1) SELECT MAX(n) FROM c) AS n FROM items o ORDER BY o.id",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    for(int64_t id=1;id<=3;++id) {
      check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      if(id==3) check_equal(row.values[1].kind,TURBODB_VALUE_NULL);
      else check_equal(row.values[1].data.int64_value,id*10+1);
    }
    check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_DONE);
  }
  it("rejects ungrouped recursive captures and isolates iteration failure") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)"); execute("INSERT INTO items VALUES(1,10),(2,20)");
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    const char *invalid[]={
      "SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.score UNION ALL SELECT n+1 FROM c WHERE n<2) SELECT MAX(n) FROM c) AS n FROM items o GROUP BY o.id",
      "EXPLAIN SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.score UNION ALL SELECT n+1 FROM c WHERE n<2) SELECT MAX(n) FROM c) AS n FROM items o GROUP BY o.id"
    };
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
      parse_scope(invalid[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_SQL_ERROR);
      check_contains(error.message,"not a GROUP BY key"); free_ast();
      check_null(graph_root.owner); check_equal(owner.active_sources,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    parse_scope("SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.id UNION ALL SELECT n+1 FROM c) "
        "SELECT MAX(n) FROM c) AS n FROM items o",0);
    check_equal(bounded_open(1),TURBODB_STATUS_OK); free_ast();
    orm_sql_scan_row row={.state=ORM_SQL_SCAN_CANCELLED,.count=99};
    check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(row.count,99u); check_equal(row.state,ORM_SQL_SCAN_CANCELLED);
    check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    graph_close(); check_false(owner.failed); check_equal(owner.active_sources,0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("cleans recursive captures at every allocation and execution step boundary") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2)");
    parse_scope("SELECT o.id,(WITH RECURSIVE c(n) AS (SELECT o.id UNION ALL SELECT n+1 FROM c "
        "WHERE n<o.id+1) SELECT (SELECT MAX(n) FROM c)) AS n FROM items o ORDER BY o.id",0);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); reserves=resizes=0;
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    const size_t allocations[]={reserves,resizes}; graph_close();
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
      check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); reserves=resizes=0;
      if(pass) fail_resize=point; else fail_reserve=point;
      row=(orm_sql_scan_row){.state=ORM_SQL_SCAN_CANCELLED,.count=99};
      check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      fail_reserve=fail_resize=0; check_equal(row.count,99u); graph_close();
      check_equal(owner.active_sources,0u); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    for(uint64_t point=0;point<steps;++point) {
      check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      row=(orm_sql_scan_row){.state=ORM_SQL_SCAN_CANCELLED,.count=99};
      check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=LIMIT;
      check_equal(row.count,99u); graph_close(); check_equal(owner.active_sources,0u); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("keeps correlated recursive writes atomic across iteration failure") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY,score BIGINT)"); execute("INSERT INTO items VALUES(1,10),(2,20)");
    parse_scope("UPDATE items o SET score=(WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL "
        "SELECT n+1 FROM c WHERE n<o.id) SELECT MAX(n) FROM c)",0);
    size_t affected=99;
    check_equal(orm_tidesdb_sql_runtime_execute(document,&owner,NULL,0,DEPTH,1,false,NULL,&affected,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(affected,99u); check_false(owner.failed); free_ast();
    parse_scope("SELECT score FROM items ORDER BY id",0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t unchanged[]={10,20}; graph_values(unchanged,sizeof(unchanged)/sizeof(unchanged[0])); graph_close();
    parse_scope("UPDATE items o SET score=(WITH RECURSIVE c(n) AS (SELECT o.id UNION ALL "
        "SELECT n+1 FROM c WHERE n<o.id+1) SELECT MAX(n) FROM c)",0);
    check_equal(orm_tidesdb_sql_runtime_execute(document,&owner,NULL,0,DEPTH,DEPTH,false,NULL,&affected,&error),TURBODB_STATUS_OK);
    check_equal(affected,2u); free_ast();
    parse_scope("SELECT score FROM items ORDER BY id",0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t changed[]={2,3}; graph_values(changed,sizeof(changed)/sizeof(changed[0])); graph_close();
    parse_scope("DELETE FROM items o WHERE o.id IN (WITH RECURSIVE c(n) AS (SELECT o.id "
        "UNION ALL SELECT n+1 FROM c WHERE n<o.id+1) SELECT n FROM c WHERE o.id=2)",0);
    check_equal(orm_tidesdb_sql_runtime_execute(document,&owner,NULL,0,DEPTH,DEPTH,false,NULL,&affected,&error),TURBODB_STATUS_OK);
    check_equal(affected,1u);
  }
  it("refunds every correlated recursive construction step") {
    const char *sql[]={
      "SELECT o.x,(WITH RECURSIVE c(n) AS (SELECT o.x UNION ALL SELECT n+1 FROM c WHERE n<o.x+1) SELECT (SELECT MAX(n) FROM c)) AS n FROM (SELECT 1 AS x) o",
      "EXPLAIN SELECT o.x,(WITH RECURSIVE c(n) AS (SELECT o.x UNION ALL SELECT n+1 FROM c WHERE n<o.x+1) SELECT (SELECT MAX(n) FROM c)) AS n FROM (SELECT 1 AS x) o"
    };
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0);
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK);
      const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start; graph_close();
      for(uint64_t point=0;point<steps;++point) {
        budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
        check_equal(bounded_open(DEPTH),TURBODB_STATUS_LIMIT_EXCEEDED);
        budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=LIMIT;
        check_null(graph_root.owner); check_equal(owner.active_sources,0u); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      free_ast();
    }
  }
  it("keeps recursive plans and pagination stable across guarded execution reopen") {
    parse("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<4 LIMIT 2 OFFSET 1) SELECT n FROM c",0);
    check_equal(owner_open(owner_spec()),TURBODB_STATUS_OK); owner_reader();
    const orm_sql_select *seed=owned.initial.plan,*member=owned.recursive.plan;
    check_equal(orm_sql_cte_query_execution_open(&owned,NULL,0,&error),TURBODB_STATUS_BUSY);
    check_equal(orm_sql_cte_query_execution_close(&owned,&error),TURBODB_STATUS_BUSY);
    const int64_t expected[]={2,3}; values(expected,sizeof(expected)/sizeof(expected[0]));
    check_equal(orm_sql_cte_reader_close(&reader,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_query_execution_close(&owned,&error),TURBODB_STATUS_OK);
    check_true(owned.execution_closed); check_null(owned.cache.budget);
    check_true(owned.initial.plan==seed); check_true(owned.recursive.plan==member);
    check_equal(orm_sql_cte_query_execution_open(&owned,NULL,1,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_sql_cte_query_execution_open(&owned,NULL,0,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_reader_open(&owned.cache,&reader,&error),TURBODB_STATUS_OK);
    values(expected,sizeof(expected)/sizeof(expected[0]));
    check_equal(orm_sql_cte_reader_close(&reader,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_query_execution_close(&owned,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_query_execution_close(&owned,&error),TURBODB_STATUS_OK);
  }
  it("copies the diagnostic receiver across seed and member execution reopens") {
    enum { RECORDS = 2, ROWS = 3 };
    check_equal(orm_sql_diagnostics_init(&evaluation_diagnostics,RECORDS,&error),TURBODB_STATUS_OK);
    parse("WITH RECURSIVE c(n,z) AS(SELECT 1,MOD(7,0) UNION ALL "
        "SELECT n+1,MOD(n,0) FROM c WHERE n<3) SELECT n,z FROM c",0);
    scope.evaluation=(orm_sql_evaluation){.diagnostics=&evaluation_diagnostics};
    orm_sql_expr_query_sources sources={.evaluation=scope.evaluation};
    orm_sql_cte_query_spec spec=owner_spec(); spec.sources=&sources;
    check_equal(owner_open(spec),TURBODB_STATUS_OK); owner_reader();
    sources.evaluation=(orm_sql_evaluation){.mode=ORM_SQL_EVALUATION_WRITE};
    for(size_t round=0;round<2;++round) {
      orm_sql_row_source *source=orm_sql_cte_reader_source(&reader);
      for(size_t r=0;r<ROWS;++r) {
        const turbodb_value_t *row=NULL; check_equal(source->next(source->context,&row,&error),TURBODB_STATUS_OK);
        check_not_null(row); check_equal(row[0].data.int64_value,(int64_t)r+1); check_equal(row[1].kind,TURBODB_VALUE_NULL);
      }
      const turbodb_value_t *row=NULL; check_equal(source->next(source->context,&row,&error),TURBODB_STATUS_OK); check_null(row);
      check_equal(evaluation_diagnostics.total,ROWS*(round+1));
      if(!round) {
        check_equal(orm_sql_cte_reader_close(&reader,&error),TURBODB_STATUS_OK);
        check_equal(orm_sql_cte_query_execution_close(&owned,&error),TURBODB_STATUS_OK);
        check_equal(orm_sql_cte_query_execution_open(&owned,NULL,0,&error),TURBODB_STATUS_OK);
        check_equal(orm_sql_cte_reader_open(&owned.cache,&reader,&error),TURBODB_STATUS_OK);
      }
    }
  }
  it("stops an unbounded recursive statement at its global page and replays that cache") {
    parameters[0]=turbodb_i64(1); parameters[1]=turbodb_i64(1); parameters[2]=turbodb_i64(2); parameters[3]=turbodb_i64(2);
    parse_scope("WITH RECURSIVE c(n) AS (SELECT ? UNION ALL SELECT n+? FROM c LIMIT ? OFFSET ?) SELECT n FROM c",PARAMETERS);
    check_equal(bounded_open(3),TURBODB_STATUS_OK); free_ast();
    for(size_t i=0;i<PARAMETERS;++i) parameters[i]=turbodb_i64(99);
    const int64_t expected[]={3,4}; graph_values(expected,sizeof(expected)/sizeof(expected[0]));
    const uint64_t materialized=budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
    check_equal(orm_sql_runtime_execution_close(&graph_root,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_execution_resume(&graph_root,&error),TURBODB_STATUS_OK);
    graph_values(expected,sizeof(expected)/sizeof(expected[0]));
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],materialized);
  }
  it("composes complete-query parenthesized pages while retaining seed-local pagination") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS ((SELECT 1 UNION ALL SELECT n+1 FROM c LIMIT 6 OFFSET 1) LIMIT 2 OFFSET 2) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS ((SELECT 1 AS n UNION ALL SELECT 2 ORDER BY n DESC LIMIT 1) UNION ALL SELECT n+1 FROM c LIMIT 2 OFFSET 2) SELECT n FROM c"};
    const int64_t expected[]={4,5};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      graph_values(expected,sizeof(expected)/sizeof(expected[0])); graph_close();
    }
  }
  it("skips hazardous seed and recursive expressions for a zero or exhausted nested page") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS (SELECT 9223372036854775807+1 UNION ALL SELECT n+1 FROM c LIMIT 0 OFFSET 9) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS ((SELECT 9223372036854775807+1 UNION ALL SELECT n+1 FROM c LIMIT 2) LIMIT 1 OFFSET 3) SELECT n FROM c"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(1),TURBODB_STATUS_OK); free_ast();
      graph_values(NULL,0); check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u); graph_close();
    }
  }
  it("stops before unused seed branches and later recursive members are evaluated") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT 9223372036854775807+1 UNION ALL SELECT n+1 FROM c LIMIT 1) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c UNION ALL SELECT n+9223372036854775807 FROM c LIMIT 2) SELECT n FROM c"};
    const int64_t expected[]={1,2};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(1),TURBODB_STATUS_OK); free_ast();
      graph_values(expected,i?2:1); graph_close();
    }
  }
  it("applies recursive DISTINCT before counting the global page") {
    parse_scope("WITH RECURSIVE c(n) AS (SELECT 1 UNION SELECT 1 UNION SELECT n+1 FROM c WHERE n<4 UNION SELECT n FROM c LIMIT 2 OFFSET 1) SELECT n FROM c",0);
    check_equal(bounded_open(2),TURBODB_STATUS_OK); free_ast();
    const int64_t expected[]={2,3}; graph_values(expected,sizeof(expected)/sizeof(expected[0]));
  }
  it("exposes only the page to repeated references and downstream recursive definitions") {
    parse_scope("WITH RECURSIVE a(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM a LIMIT 2 OFFSET 2),b(n) AS (SELECT n FROM a UNION ALL SELECT n+1 FROM b WHERE n<5) SELECT n FROM b UNION ALL SELECT n FROM a",0);
    check_equal(bounded_open(3),TURBODB_STATUS_OK); free_ast();
    const int64_t expected[]={3,4,4,5,5,3,4}; graph_values(expected,sizeof(expected)/sizeof(expected[0]));
  }
  it("validates global page parameters even for EXPLAIN and LIMIT zero consumers") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c LIMIT ?) SELECT n FROM c LIMIT 0",
      "EXPLAIN WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c LIMIT ?) SELECT n FROM c"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parameters[0]=turbodb_i64(-1); parse_scope(sql[i],1);
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(bounded_open(1),TURBODB_STATUS_TYPE_ERROR); check_null(graph_root.owner);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); free_ast();
    }
  }
  it("explains a global page without recursion or business materialization") {
    parse_scope("EXPLAIN WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c LIMIT 2 OFFSET 1) SELECT n FROM c",0);
    check_equal(bounded_open(1),TURBODB_STATUS_OK); free_ast();
    orm_sql_scan_row row; size_t pages=0;
    for(;;) {
      check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      if(row.state!=ORM_SQL_SCAN_ROW) break;
      if(row.values[EXPLAIN_EXTRA].kind==TURBODB_VALUE_TEXT && strstr(row.values[EXPLAIN_EXTRA].data.text_value.data,"Offset")) {
        ++pages; text_value_is(row.values[EXPLAIN_KIND],"QUERY GROUP");
        check_equal(row.values[EXPLAIN_ID].kind,TURBODB_VALUE_NULL);
        check_contains(row.values[EXPLAIN_EXTRA].data.text_value.data,"Limit");
      }
    }
    check_equal(pages,1u); check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
  }
  it("refunds every paginated EXPLAIN allocation and construction step failure") {
    parameters[0]=turbodb_i64(2); parameters[1]=turbodb_i64(1);
    parse_scope("EXPLAIN WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c LIMIT ? OFFSET ?) SELECT n FROM c",2);
    reserves=resizes=0; const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(bounded_open(1),TURBODB_STATUS_OK);
    const size_t allocations[]={reserves,resizes}; const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    graph_close(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
      reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(bounded_open(1),TURBODB_STATUS_OUT_OF_MEMORY); fail_reserve=fail_resize=0;
      check_null(graph_root.owner); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    for(uint64_t point=0;point<steps;++point) {
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      check_equal(bounded_open(1),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(graph_root.owner);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=LIMIT;
    check_equal(owner.active_sources,0u);
  }
  it("composes recursive EXPLAIN definitions once with statement-wide ids after AST destruction") {
    parse_scope("EXPLAIN FORMAT=TRADITIONAL WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT 10 UNION ALL SELECT n+1 FROM c UNION ALL SELECT n+10 FROM c) SELECT n FROM c UNION ALL SELECT n FROM c",0);
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(bounded_open(1),TURBODB_STATUS_OK); free_ast();
    const expected_explain expected[]={
      {1,"PRIMARY","c",false},{2,"UNION","c",false},
      {3,"DERIVED",NULL,false},{4,"UNION",NULL,false},{5,"UNION","c",true},{6,"UNION","c",true}};
    explain_rows(expected,sizeof(expected)/sizeof(expected[0]));
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
    orm_sql_schema_column column; check_equal(orm_tidesdb_sql_runtime_column(&graph_root,EXPLAIN_ID,&column,&error),TURBODB_STATUS_OK);
    check_true(column.type.nullable);
  }
  it("marks only the first physical table of recursive JOINs including RIGHT JOIN reversal") {
    execute("CREATE TABLE items (id BIGINT PRIMARY KEY, score BIGINT)");
    execute("INSERT INTO items(id,score) VALUES(1,10)");
    const char *sql[]={
      "EXPLAIN WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT c.n+i.id FROM items i JOIN c ON i.id=1 WHERE c.n<3) SELECT n FROM c",
      "EXPLAIN WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT c.n+i.id FROM items i RIGHT JOIN c ON i.id=1 WHERE c.n<3) SELECT n FROM c"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(1),TURBODB_STATUS_OK); free_ast();
      const expected_explain expected[]={
        {1,"PRIMARY","c",false},{2,"DERIVED",NULL,false},
        {3,"UNION",i?"c":"i",true},{3,"UNION",i?"i":"c",false}};
      explain_rows(expected,sizeof(expected)/sizeof(expected[0])); graph_close();
    }
  }
  it("composes nested recursive definitions derived inputs and scalar metadata without evaluating expressions") {
    parse_scope("EXPLAIN WITH RECURSIVE a(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM a),b(n) AS (SELECT n FROM a UNION ALL SELECT b.n+d.step+(SELECT 9223372036854775807+1) FROM b CROSS JOIN (SELECT 1 AS step) d) SELECT n FROM b",0);
    check_equal(bounded_open(1),TURBODB_STATUS_OK); free_ast();
    const expected_explain expected[]={
      {1,"PRIMARY","b",false},{2,"DERIVED",NULL,false},{3,"UNION","a",true},
      {4,"DERIVED","a",false},{5,"UNION","b",true},{5,"UNION","d",false},
      {6,"SUBQUERY",NULL,false},{7,"DERIVED",NULL,false}};
    explain_rows(expected,sizeof(expected)/sizeof(expected[0]));
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
  }
  it("retains complete recursive statement parameters and dependency caches across execution resume") {
    parameters[0]=turbodb_i64(1); parameters[1]=turbodb_i64(3); parameters[2]=turbodb_i64(1);
    parse_scope("WITH RECURSIVE c(n) AS (SELECT ? UNION ALL SELECT n+1 FROM c WHERE n<?) SELECT n+? AS n FROM c",3);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    parameters[0]=parameters[1]=parameters[2]=turbodb_i64(99);
    const int64_t expected[]={2,3,4}; graph_values(expected,sizeof(expected)/sizeof(expected[0]));
    const uint64_t materialized=budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
    check_equal(orm_sql_runtime_execution_close(&graph_root,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_execution_resume(&graph_root,&error),TURBODB_STATUS_OK);
    graph_values(expected,sizeof(expected)/sizeof(expected[0]));
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],materialized);
  }
  it("validates recursive EXPLAIN formats shapes types and explicit iteration bounds before publication") {
    const char *sql[]={
      "EXPLAIN FORMAT=JSON WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c) SELECT n FROM c",
      "EXPLAIN WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT SUM(n) FROM c) SELECT n FROM c",
      "EXPLAIN WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT 'bad' FROM c) SELECT n FROM c"};
    const turbodb_status_t expected[]={TURBODB_STATUS_UNSUPPORTED,TURBODB_STATUS_SQL_ERROR,TURBODB_STATUS_TYPE_ERROR};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(bounded_open(1),expected[i]); check_null(graph_root.owner);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); free_ast();
    }
    parse_scope(sequence,0); check_equal(bounded_open(0),TURBODB_STATUS_INVALID_ARGUMENT); check_null(graph_root.owner);
    check_equal(orm_tidesdb_sql_runtime_open(document,&owner,vstr_from_cstr("app"),NULL,0,DEPTH,&graph_root,&error),TURBODB_STATUS_UNSUPPORTED);
    check_null(graph_root.owner);
  }
  it("overrides anonymous and repeated derived output names in positional order") {
    const char *sql[]={
      "SELECT d.x+d.y AS n FROM (SELECT 1,2) d(x,y)",
      "SELECT d.x+d.y AS n FROM (SELECT 1 AS same,2 AS same) AS d(x,y)",
      "SELECT e.n FROM (SELECT d.x+d.y FROM (SELECT 1,2) d(x,y)) e(n)",
      "WITH q AS (SELECT d.x+d.y AS n FROM (SELECT 1,2) d(x,y)) SELECT n FROM q",
      "SELECT d.`select`+d.y AS n FROM (SELECT 1,2) d(`select`,y)"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      const int64_t expected[]={3}; graph_values(expected,1); graph_close(); check_equal(owner.active_sources,0u);
    }
  }
  it("renames expanded stars UNION outputs and repeated column names without changing types") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    const char *sql[]={
      "SELECT d.n FROM (SELECT * FROM items) d(n) ORDER BY n",
      "SELECT d.y FROM (SELECT a.id,b.id FROM items a JOIN items b ON a.id=b.id) d(x,y) ORDER BY y",
      "SELECT d.n FROM (SELECT id FROM items UNION ALL SELECT 4) d(n) WHERE n<=3 ORDER BY n"};
    const int64_t expected[]={1,2,3};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      graph_values(expected,3); graph_close(); check_equal(owner.active_sources,0u);
    }
  }
  it("preserves inner grouping ordering and aliases while replacing the exposed derived name") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT d.result FROM (SELECT id+1 AS old FROM items GROUP BY old ORDER BY old DESC LIMIT 2) d(result)",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast(); const int64_t expected[]={4,3};
    graph_values(expected,2); graph_close();
    parse_scope("SELECT d.n FROM (SELECT MAX(id) FROM items) d(n)",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast(); const int64_t maximum[]={3}; graph_values(maximum,1); graph_close();
  }
  it("captures only renamed columns through consecutive lateral and recursive derived sources") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    const char *sql[]={
      "SELECT e.result FROM (SELECT id FROM items) a(x),LATERAL (SELECT a.x+1) d(n),LATERAL (SELECT d.n) e(result) ORDER BY result",
      "SELECT d.n FROM items a,LATERAL (SELECT e.n FROM (SELECT a.id) q(x),LATERAL (SELECT q.x+1) e(n)) d(n) ORDER BY d.n",
      "WITH RECURSIVE c(n) AS (SELECT 2 UNION ALL SELECT d.x FROM c,LATERAL (SELECT c.n+1) d(x) WHERE c.n<4) SELECT n FROM c"};
    const int64_t expected[]={2,3,4};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      graph_values(expected,3); graph_close(); check_equal(owner.active_sources,0u);
    }
  }
  it("rejects derived width duplicates and replaced names without retaining native consumers") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2)");
    const char *sql[]={
      "SELECT * FROM (SELECT 1,2) d(x)","SELECT * FROM (SELECT 1) d(x,y)",
      "SELECT * FROM (SELECT 1,2) d(x,x)","SELECT d.old FROM (SELECT 1 AS old) d(n)",
      "SELECT * FROM items a,LATERAL (SELECT a.id+9223372036854775807) d(x,y)",
      "SELECT * FROM items a,LATERAL (SELECT a.id,a.id) d(x,x)",
      "SELECT e.n FROM (SELECT id FROM items) a(x),LATERAL (SELECT a.id) e(n)"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(bounded_open(DEPTH),TURBODB_STATUS_SQL_ERROR); check_null(graph_root.owner);
      check_equal(owner.active_sources,0u); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      free_ast();
    }
  }
  it("rejects ambiguous inner ORDER GROUP and HAVING aliases even with an explicit outer name list") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2)");
    const char *invalid[]={
      "SELECT * FROM (SELECT id AS k,id+1 AS k FROM items ORDER BY k) d(x,y)",
      "SELECT * FROM (SELECT id AS k,id+1 AS k FROM items GROUP BY k) d(x,y)",
      "SELECT * FROM (SELECT MAX(id) AS k,MIN(id) AS k FROM items HAVING k>0) d(x,y)",
      "SELECT * FROM (SELECT 1 AS k,2 AS k ORDER BY k) d(x,y)"};
    for(size_t i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i) {
      parse_scope(invalid[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_SQL_ERROR);
      check_null(graph_root.owner); check_contains(error.message,"ambiguous"); free_ast();
    }
  }
  it("resolves repeated identical inner aliases and retains source precedence for GROUP BY") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2)");
    const char *sql[]={
      "SELECT d.x FROM (SELECT id AS k,id AS k FROM items ORDER BY k) d(x,y)",
      "SELECT d.x FROM (SELECT id+1 AS k,id+1 AS k FROM items ORDER BY k) d(x,y)",
      "SELECT d.x FROM (SELECT id+1 AS k,id+1 AS k FROM items GROUP BY k ORDER BY k) d(x,y)",
      "SELECT d.x FROM (SELECT id AS k,id+1 AS id FROM items GROUP BY id ORDER BY k) d(x,y)"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      const int64_t expected[]={i==1||i==2?2:1,i==1||i==2?3:2}; graph_values(expected,2); graph_close();
    }
    parse_scope("SELECT d.x FROM (SELECT MAX(id) AS k,MAX(id) AS k FROM items HAVING k>0 ORDER BY k) d(x,y)",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast(); const int64_t maximum[]={2}; graph_values(maximum,1); graph_close();
  }
  it("owns renamed TEXT parameter results across AST release and execution replay") {
    char text[]="kept"; parameters[0]=turbodb_text(text); parameter_types[0]=(orm_sql_type){TURBODB_VALUE_TEXT,false};
    parse_scope("SELECT e.payload FROM (SELECT ?) d(label),LATERAL (SELECT d.label) e(payload)",1);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast(); memset(text,'x',sizeof(text)-1); parameters[0]=turbodb_null();
    for(size_t pass=0;pass<2;++pass) {
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].kind,TURBODB_VALUE_TEXT);
      check_equal(row.values[0].data.text_value.len,4u); check_equal(memcmp(row.values[0].data.text_value.data,"kept",4),0);
      graph_values(NULL,0);
      if(!pass) {
        check_equal(orm_sql_runtime_execution_close(&graph_root,&error),TURBODB_STATUS_OK);
        check_equal(orm_sql_runtime_execution_resume(&graph_root,&error),TURBODB_STATUS_OK);
      }
    }
    graph_close(); check_equal(owner.active_sources,0u);
  }
  it("refunds every explicit derived schema allocation while binding ordinary lateral and EXPLAIN paths") {
    const char *sql[]={
      "SELECT d.x+d.y AS n FROM (SELECT 1,2) d(x,y)",
      "SELECT d.x FROM (SELECT 1 AS k,1 AS k GROUP BY k ORDER BY k) d(x,y)",
      "SELECT e.n FROM (SELECT 1) a(x),LATERAL (WITH q AS (SELECT a.x AS n) SELECT n+1 FROM q) d(y),LATERAL (SELECT d.y) e(n)",
      "EXPLAIN SELECT d.n FROM (SELECT 1) a(x),LATERAL (SELECT a.x+1) d(n)"};
    for(size_t q=0;q<sizeof(sql)/sizeof(sql[0]);++q) {
      parse_scope(sql[q],0); matrix_allowance(); reserves=resizes=0;
      check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); const size_t counts[]={reserves,resizes}; graph_close();
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=counts[pass];++point) {
        matrix_allowance(); reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
        check_equal(bounded_open(DEPTH),TURBODB_STATUS_OUT_OF_MEMORY); fail_reserve=fail_resize=0;
        check_null(graph_root.owner); check_equal(owner.active_sources,0u); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      free_ast();
    }
  }
  it("refunds every step boundary while binding explicit derived names") {
    const char *sql[]={
      "SELECT e.n FROM (SELECT 1,2) a(x,y),LATERAL (SELECT a.x+a.y) d(z),LATERAL (SELECT d.z) e(n)",
      "SELECT d.x FROM (SELECT 1 AS k,1 AS k GROUP BY k ORDER BY k) d(x,y)",
      "SELECT d.x FROM (SELECT MAX(1) AS k,MAX(1) AS k HAVING k>0 ORDER BY k) d(x,y)"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); matrix_allowance(); const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK);
      const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start; graph_close();
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(uint64_t point=0;point<steps;++point) {
        matrix_allowance(); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
        check_equal(bounded_open(DEPTH),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(graph_root.owner);
        check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      matrix_allowance(); free_ast();
    }
  }
  it("executes lateral SQL through the owning dependency graph and normal runtime") {
    const char *sql[]={
      "SELECT d.n FROM LATERAL (SELECT 1 AS n) d",
      "SELECT d.n FROM (SELECT 1 AS n) a,LATERAL (SELECT a.n AS n) d",
      "WITH q AS (SELECT d.n FROM LATERAL (SELECT 1 AS n) d) SELECT n FROM q",
      "SELECT (SELECT d.n FROM LATERAL (SELECT 1 AS n) d) AS n"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0);
      check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      const int64_t expected[]={1}; graph_values(expected,1); graph_close();
      check_equal(owner.active_sources,0u);
    }
  }
  it("executes consecutive and nested lateral definitions with complete ancestor frames") {
    const char *sql[]={
      "SELECT e.n FROM (SELECT 1 AS n) a,LATERAL (SELECT a.n+1 AS n) d,LATERAL (SELECT a.n+d.n AS n) e",
      "SELECT d.n FROM (SELECT 1 AS n) a,LATERAL (SELECT q.n+2 AS n FROM (SELECT a.n AS n) q) d",
      "SELECT d.n FROM (SELECT 1 AS n) a,LATERAL (WITH q AS (SELECT a.n AS n) SELECT n+2 AS n FROM q) d",
      "SELECT d.n FROM (SELECT 1 AS n) a,LATERAL (SELECT e.n FROM (SELECT a.n AS n) q,LATERAL (SELECT q.n+a.n+1 AS n) e) d",
      "SELECT (SELECT a.n+d.n) AS n FROM (SELECT 1 AS n) a,LATERAL (SELECT a.n+1 AS n) d",
      "SELECT d.n FROM (SELECT 1 AS n) a,LATERAL (SELECT (SELECT a.n+2) AS n) d"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      const int64_t expected[]={3}; graph_values(expected,1); graph_close(); check_equal(owner.active_sources,0u);
    }
  }
  it("rebuilds nested derived scalar CTE and recursive lateral inputs for each native row") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    const char *sql[]={
      "SELECT d.n FROM items a,LATERAL (SELECT q.n+1 AS n FROM (SELECT a.id AS n) q) d ORDER BY a.id",
      "SELECT d.n FROM items a,LATERAL (WITH q AS (SELECT a.id AS n) SELECT (SELECT n+1 FROM q) AS n FROM q) d ORDER BY a.id",
      "SELECT d.n FROM items a,LATERAL (SELECT (SELECT a.id+1) AS n) d ORDER BY a.id",
      "SELECT d.n FROM items a,LATERAL (SELECT e.n FROM (SELECT a.id AS n) q,LATERAL (SELECT q.n+1 AS n) e) d ORDER BY a.id",
      "SELECT d.n FROM items a,LATERAL (WITH RECURSIVE c(n) AS (SELECT a.id UNION ALL SELECT n+1 FROM c WHERE n<a.id+1) SELECT MAX(n) AS n FROM c) d ORDER BY a.id",
      "WITH q AS (SELECT id FROM items) SELECT d.n FROM q a,LATERAL (SELECT a.id+1 AS n) d ORDER BY a.id"};
    const int64_t expected[]={2,3,4};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      graph_values(expected,sizeof(expected)/sizeof(expected[0])); graph_close();
      check_equal(owner.active_sources,0u); check_false(owner.failed);
    }
  }
  it("replays ordinary derived and CTE siblings inside a lateral dependent RIGHT subtree") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2)");
    const char *sql[]={
      "SELECT d.n FROM (SELECT id FROM items) a JOIN LATERAL (SELECT a.id+c.id AS n) d ON TRUE RIGHT JOIN items c ON TRUE ORDER BY c.id,a.id",
      "WITH q AS (SELECT id FROM items) SELECT d.n FROM q a JOIN LATERAL (SELECT a.id+c.id AS n) d ON TRUE RIGHT JOIN items c ON TRUE ORDER BY c.id,a.id",
      "SELECT d.n FROM items a JOIN LATERAL (SELECT a.id+c.id AS n) d ON TRUE RIGHT JOIN (SELECT id FROM items) c ON TRUE ORDER BY c.id,a.id"};
    const int64_t expected[]={2,3,3,4};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      graph_values(expected,sizeof(expected)/sizeof(expected[0])); graph_close();
      check_equal(owner.active_sources,0u);
    }
  }
  it("retains complete statement marker snapshots and supports lateral statement replay") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parameters[0]=turbodb_i64(100); parameters[1]=turbodb_i64(10); parameters[2]=parameters[3]=turbodb_i64(1);
    parse_scope("SELECT ?+a.id AS n,d.n AS value FROM items a JOIN LATERAL (SELECT ?+a.id AS n WHERE a.id>?) d ON a.id>? ORDER BY n",PARAMETERS);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast(); memset(parameters,0,sizeof(parameters));
    const int64_t expected[][2]={{102,12},{103,13}};
    paired_values(expected,sizeof(expected)/sizeof(expected[0]));
    check_equal(orm_sql_runtime_execution_close(&graph_root,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_runtime_execution_resume(&graph_root,&error),TURBODB_STATUS_OK);
    paired_values(expected,sizeof(expected)/sizeof(expected[0])); graph_close(); check_equal(owner.active_sources,0u);
  }
  it("executes lateral LEFT null extension through the normal dependency owner") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT a.id AS id,d.n AS n FROM items a LEFT JOIN LATERAL (SELECT a.id AS n WHERE a.id=2) d ON TRUE ORDER BY id",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    for(int64_t id=1;id<=3;++id) {
      orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,id);
      check_equal(row.values[1].kind,id==2?TURBODB_VALUE_INT64:TURBODB_VALUE_NULL);
      if(id==2) check_equal(row.values[1].data.int64_value,id);
    }
    graph_values(NULL,0); graph_close(); check_equal(owner.active_sources,0u);
  }
  it("describes lateral query dependencies without executing their expressions") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2)");
    parse_scope("EXPLAIN SELECT d.n FROM items a,LATERAL (SELECT a.id+9223372036854775807 AS n) d",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS],materialized=budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
    orm_sql_scan_row row={0}; size_t count=0;
    do {
      check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
      if(row.state==ORM_SQL_SCAN_ROW) ++count;
    } while(row.state==ORM_SQL_SCAN_ROW);
    check_equal(row.state,ORM_SQL_SCAN_DONE); check_greater(count,0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],materialized); graph_close();
  }
  it("refunds every normal lateral statement construction allocation") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2)");
    const char *sql[]={
      "SELECT e.n FROM items a,LATERAL (SELECT (SELECT a.id+1) AS n) d,LATERAL (WITH q AS (SELECT d.n AS n) SELECT n FROM q) e",
      "EXPLAIN SELECT d.n FROM items a JOIN LATERAL (SELECT a.id+c.id AS n) d ON TRUE RIGHT JOIN (SELECT id FROM items) c ON TRUE"};
    for(size_t q=0;q<sizeof(sql)/sizeof(sql[0]);++q) {
      parse_scope(sql[q],0); matrix_allowance(); reserves=resizes=0;
      check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); const size_t counts[]={reserves,resizes}; graph_close();
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=counts[pass];++point) {
        matrix_allowance(); reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
        check_equal(bounded_open(DEPTH),TURBODB_STATUS_OUT_OF_MEMORY); fail_reserve=fail_resize=0;
        check_null(graph_root.owner); check_equal(owner.active_sources,0u); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      free_ast();
    }
  }
  it("refunds normal lateral statement construction at every step limit") {
    parse_scope("SELECT e.n FROM (SELECT 2 AS n) a,LATERAL (SELECT (SELECT a.n+1) AS n) d,LATERAL (SELECT d.n AS n) e",0);
    matrix_allowance(); const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start; graph_close();
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(uint64_t point=0;point<steps;++point) {
      matrix_allowance(); budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      check_equal(bounded_open(DEPTH),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(graph_root.owner);
      check_equal(owner.active_sources,0u); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    matrix_allowance(); free_ast();
  }
  it("refunds normal lateral execution allocations across all native rows and nested CTE readers") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT d.n FROM items a,LATERAL (WITH q AS (SELECT a.id AS n) SELECT (SELECT n+1 FROM q) AS n FROM q) d ORDER BY a.id",0);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; matrix_allowance();
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); reserves=resizes=0;
    const int64_t expected[]={2,3,4}; graph_values(expected,sizeof(expected)/sizeof(expected[0]));
    const size_t counts[]={reserves,resizes}; graph_close();
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=counts[pass];++point) {
      matrix_allowance(); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); reserves=resizes=0;
      if(pass) fail_resize=point; else fail_reserve=point; size_t count=0;
      check_equal(graph_drain(&count),TURBODB_STATUS_OUT_OF_MEMORY); fail_resize=fail_reserve=0;
      const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(graph_drain(&count),TURBODB_STATUS_OUT_OF_MEMORY);
      check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],steps);
      graph_close(); check_equal(owner.active_sources,0u); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    free_ast();
  }
  it("closes normal lateral native execution even when every available step has been consumed") {
    execute("CREATE TABLE items(id BIGINT PRIMARY KEY)"); execute("INSERT INTO items VALUES(1),(2),(3)");
    parse_scope("SELECT d.n FROM items a,LATERAL (SELECT (SELECT a.id+1) AS n) d",0);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; matrix_allowance();
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK);
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    const int64_t expected[]={2,3,4}; graph_values(expected,sizeof(expected)/sizeof(expected[0]));
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start; graph_close();
    for(uint64_t point=0;point<steps;++point) {
      matrix_allowance(); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      size_t count=0; check_equal(graph_drain(&count),TURBODB_STATUS_LIMIT_EXCEEDED);
      const uint64_t consumed=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(graph_drain(&count),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS],consumed);
      graph_close(); check_equal(owner.active_sources,0u); check_false(owner.failed);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    matrix_allowance(); free_ast();
  }
  it("executes lateral prefix captures from direct recursive member frontier inputs") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT d.n FROM c,LATERAL (SELECT c.n+1 AS n) d WHERE c.n<3) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT d.n FROM c,LATERAL (SELECT (SELECT c.n+1) AS n) d WHERE c.n<3) SELECT n FROM c"};
    const int64_t expected[]={1,2,3};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      graph_values(expected,sizeof(expected)/sizeof(expected[0])); graph_close(); check_equal(owner.active_sources,0u);
    }
  }
  it("rejects a held lateral input before borrowing a prefix and permits an explicit later activation") {
    parse_scope("SELECT d.n FROM (SELECT 1 AS n) a,LATERAL (SELECT q.n FROM (SELECT a.n AS n) q) d",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const orm_sql_derived_binding *lateral=NULL,*input=NULL;
    for(size_t i=0;i<graph_root.dependencies.derived_count;++i) {
      const orm_sql_derived_binding *binding=vec_at_const(&graph_root.dependencies.derived,i);
      if(binding->input&&binding->input->binding.open) lateral=binding;
      if(binding->schema->name.len==1&&binding->schema->name.data[0]=='q') input=binding;
    }
    check_not_null(lateral); check_not_null(input); const size_t projection[]={0};
    const orm_sql_scan_spec spec={.projection=projection,.projection_count=1,.limit=UINT64_MAX};
    check_equal(orm_tidesdb_sql_scan_open_source(input->source,&spec,&budget,&lateral_consumer,&error),TURBODB_STATUS_OK);
    const turbodb_value_t capture=turbodb_i64(77);
    check_equal(lateral->input->binding.open(lateral->input->binding.context,&capture,1,&error),TURBODB_STATUS_BUSY);
    check_false(owner.failed); check_true(input->source->active);
    check_equal(orm_tidesdb_sql_scan_close(&lateral_consumer,&error),TURBODB_STATUS_OK);
    check_equal(lateral->input->binding.open(lateral->input->binding.context,&capture,1,&error),TURBODB_STATUS_OK);
    const turbodb_value_t *row=NULL; check_equal(lateral->source->next(lateral->source->context,&row,&error),TURBODB_STATUS_OK);
    check_not_null(row); check_equal(row[0].data.int64_value,77);
    check_equal(lateral->input->binding.close(lateral->input->binding.context,&error),TURBODB_STATUS_OK);
    graph_close(); check_equal(owner.active_sources,0u); check_false(owner.failed);
  }
  it("preserves an owning runtime and dependency metadata when a lateral consumer blocks destruction") {
    parse_scope("SELECT d.n FROM LATERAL (SELECT 7 AS n) d",0);
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const orm_sql_derived_binding *binding=vec_at_const(&graph_root.dependencies.derived,0);
    check_equal(binding->input->binding.open(binding->input->binding.context,NULL,0,&error),TURBODB_STATUS_OK);
    const size_t projection[]={0}; const orm_sql_scan_spec spec={.projection=projection,.projection_count=1,.limit=UINT64_MAX};
    check_equal(orm_tidesdb_sql_scan_open_source(binding->source,&spec,&budget,&lateral_consumer,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_runtime_close(&graph_root,&error),TURBODB_STATUS_BUSY);
    check_true(graph_root.owner==&owner); check_not_null(graph_root.dependencies.budget); check_false(owner.failed);
    orm_sql_scan_row row={0}; check_equal(orm_tidesdb_sql_scan_next(&lateral_consumer,&row,&error),TURBODB_STATUS_OK);
    check_equal(row.state,ORM_SQL_SCAN_ROW); check_equal(row.values[0].data.int64_value,7);
    check_equal(orm_tidesdb_sql_scan_close(&lateral_consumer,&error),TURBODB_STATUS_OK);
    check_equal(binding->input->binding.close(binding->input->binding.context,&error),TURBODB_STATUS_OK);
    graph_close(); check_equal(owner.active_sources,0u); check_false(owner.failed);
  }
  it("refunds every allocation and step while constructing and executing recursive member lateral sources") {
    parse_scope("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT d.n FROM c,LATERAL "
        "(SELECT (SELECT c.n+1) AS n) d WHERE c.n<3) SELECT n FROM c",0);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; matrix_allowance(); reserves=resizes=0;
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK); const int64_t expected[]={1,2,3}; graph_values(expected,3);
    const uint64_t counts[]={reserves,resizes,budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start}; graph_close();
    for(size_t pass=0;pass<sizeof(counts)/sizeof(counts[0]);++pass) {
      const bool steps=pass==2;
      for(uint64_t point=steps?0:1;steps?point<counts[pass]:point<=counts[pass];++point) {
        matrix_allowance(); reserves=resizes=0;
        if(steps) budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
        else if(pass) fail_resize=(size_t)point; else fail_reserve=(size_t)point;
        turbodb_status_t status=bounded_open(DEPTH); size_t rows=0;
        if(status==TURBODB_STATUS_OK) status=graph_drain(&rows);
        check_equal(status,steps?TURBODB_STATUS_LIMIT_EXCEEDED:TURBODB_STATUS_OUT_OF_MEMORY);
        fail_reserve=fail_resize=0; graph_close(); check_equal(owner.active_sources,0u); check_false(owner.failed);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
    }
    matrix_allowance(); free_ast();
  }
  it("refunds every recursive statement and EXPLAIN allocation failure") {
    const char *sql[]={sequence,
      "EXPLAIN WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT c.n+d.step FROM c CROSS JOIN (SELECT 1 AS step) d) SELECT n FROM c",
      "SELECT o.x,(WITH RECURSIVE c(n) AS (SELECT o.x UNION ALL SELECT n+1 FROM c WHERE n<o.x+1) SELECT (SELECT MAX(n) FROM c)) AS n FROM (SELECT 1 AS x) o",
      "EXPLAIN SELECT o.x,(WITH RECURSIVE c(n) AS (SELECT o.x UNION ALL SELECT n+1 FROM c WHERE n<o.x+1) SELECT (SELECT MAX(n) FROM c)) AS n FROM (SELECT 1 AS x) o"};
    for(size_t q=0;q<sizeof(sql)/sizeof(sql[0]);++q) {
      parse_scope(sql[q],0); reserves=resizes=0; check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK);
      const size_t allocations[]={reserves,resizes}; graph_close();
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
        reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
        check_equal(bounded_open(DEPTH),TURBODB_STATUS_OUT_OF_MEMORY); fail_reserve=fail_resize=0;
        check_null(graph_root.owner); check_equal(owner.active_sources,0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      }
      free_ast();
    }
  }
  it("refunds every recursive EXPLAIN construction step failure including metadata annotations") {
    parse_scope("EXPLAIN WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT c.n+d.step FROM c CROSS JOIN (SELECT 1 AS step) d) SELECT n FROM c",0);
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(bounded_open(DEPTH),TURBODB_STATUS_OK);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start; graph_close();
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(uint64_t point=0;point<steps;++point) {
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      check_equal(bounded_open(DEPTH),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(graph_root.owner);
      check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=LIMIT;
  }
  it("latches recursive EXPLAIN pull quota errors and cancels without running recursion") {
    const char sql[]="EXPLAIN WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c) SELECT n FROM c";
    parse_scope(sql,0); check_equal(bounded_open(1),TURBODB_STATUS_OK); free_ast();
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    orm_sql_scan_row row={.state=ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(row.state,ORM_SQL_SCAN_CANCELLED);
    budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=LIMIT;
    check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); graph_close();
    parse_scope(sql,0); check_equal(bounded_open(1),TURBODB_STATUS_OK); free_ast();
    check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_ROW);
    check_equal(orm_tidesdb_sql_runtime_cancel(&graph_root,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK); check_equal(row.state,ORM_SQL_SCAN_CANCELLED);
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u);
  }
  it("constructs and executes the complete recursive dependency graph after AST destruction") {
    parse_scope(multiple,0); const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(graph_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    check_equal(graph.derived_count,3u); check_equal(graph.query_count,0u);
    size_t executable=0;
    for(size_t i=0;i<graph.derived_count;++i) {
      const orm_sql_derived_binding *binding=vec_at_const(&graph.derived,i);
      check_not_null(binding->schema); if(binding->source) ++executable;
    }
    check_equal(executable,1u);
    const int64_t expected[]={1,10,2,11,3,12}; graph_values(expected,sizeof(expected)/sizeof(expected[0]));
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    orm_sql_schema_column column; check_equal(orm_tidesdb_sql_runtime_column(&graph_root,0,&column,&error),TURBODB_STATUS_OK);
    check_true(column.type.nullable);
  }
  it("replays member streaming descendants while preserving seed scalar and CTE cache boundaries") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS (SELECT d.n FROM (SELECT 1 AS n) d UNION ALL SELECT c.n+d.step FROM c CROSS JOIN (SELECT e.step FROM (SELECT 1 AS step) e) d WHERE c.n<4) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT c.n+(SELECT x.step FROM (SELECT 1 AS step) x) FROM c WHERE n<4) SELECT n FROM c",
      "WITH RECURSIVE k(step) AS (SELECT x.step FROM (SELECT 1 AS step) x),c(n) AS (SELECT 1 UNION ALL SELECT c.n+k.step FROM c CROSS JOIN k WHERE n<4) SELECT n FROM c",
      "WITH RECURSIVE k(step) AS (SELECT 1),c(n) AS (SELECT 1 UNION ALL SELECT c.n+d.step FROM c CROSS JOIN (SELECT step FROM k) d WHERE n<4 AND EXISTS(SELECT step FROM k) AND n IN (SELECT 1 UNION SELECT 2 UNION SELECT 3)) SELECT n FROM c"};
    const int64_t expected[]={1,2,3,4};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(graph_open(DEPTH),TURBODB_STATUS_OK); free_ast();
      graph_values(expected,sizeof(expected)/sizeof(expected[0])); graph_close(); check_equal(owner.active_sources,0u);
    }
  }
  it("keeps recursive definition chains and repeated outside readers independent") {
    parse_scope("WITH RECURSIVE a(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM a WHERE n<2),b(n) AS (SELECT n FROM a UNION ALL SELECT b.n+1 FROM b JOIN a ON a.n=1 WHERE b.n<3) SELECT n FROM b UNION ALL SELECT n FROM b",0);
    check_equal(graph_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t expected[]={1,2,2,3,3,1,2,2,3,3}; graph_values(expected,sizeof(expected)/sizeof(expected[0]));
    const uint64_t materialized=budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS];
    check_equal(orm_sql_runtime_execution_close(&graph_root,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_dependencies_execution_close(&graph,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_dependencies_execution_open(&graph,NULL,0,&error),TURBODB_STATUS_OK);
    const orm_sql_expr_query_sources sources={vec_data_const(&graph.sources),graph.query_count};
    check_equal(orm_sql_runtime_execution_open(&graph_root,NULL,0,&sources,&error),TURBODB_STATUS_OK);
    graph_values(expected,sizeof(expected)/sizeof(expected[0]));
    check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],materialized);
  }
  it("binds recursive definitions inside derived scopes without capturing a shadowed outer name") {
    parse_scope("WITH c(n) AS (SELECT 99) SELECT d.n FROM (WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3) SELECT n FROM c) d",0);
    check_equal(graph_open(DEPTH),TURBODB_STATUS_OK); free_ast();
    const int64_t expected[]={1,2,3}; graph_values(expected,sizeof(expected)/sizeof(expected[0]));
  }
  it("executes native JOIN sources and copied statement parameters through the recursive graph") {
    execute("CREATE TABLE items (id BIGINT PRIMARY KEY, score BIGINT)");
    execute("INSERT INTO items(id,score) VALUES(1,10),(2,20)"); parameters[0]=turbodb_i64(1); parameters[1]=turbodb_i64(4);
    parse_scope("WITH RECURSIVE c(n) AS (SELECT ? UNION ALL SELECT c.n+i.id FROM c JOIN items i ON i.id=1 WHERE c.n<?) SELECT n FROM c",2);
    check_equal(graph_open(DEPTH),TURBODB_STATUS_OK); free_ast(); parameters[0]=turbodb_i64(99); parameters[1]=turbodb_i64(99);
    const int64_t expected[]={1,2,3,4}; graph_values(expected,sizeof(expected)/sizeof(expected[0]));
  }
  it("does not evaluate unused recursive definitions or a LIMIT zero consumer") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c) SELECT 7 AS n",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c) SELECT n FROM c LIMIT 0"};
    const int64_t expected[]={7};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); check_equal(graph_open(1),TURBODB_STATUS_OK); free_ast();
      graph_values(expected,i?0:1); check_equal(budget.used.value[ORM_SQL_BUDGET_MATERIALIZED_ROWS],0u); graph_close();
    }
  }
  it("prevalidates invalid self references instead of removing arbitrary dependency cycles") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT (SELECT n FROM c)) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT a.n FROM c a JOIN c b ON a.n=b.n) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT c.n FROM (SELECT 1 AS n) d LEFT JOIN c ON c.n=d.n) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT SUM(n) FROM c) SELECT n FROM c"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse_scope(sql[i],0); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(graph_open(DEPTH),TURBODB_STATUS_SQL_ERROR); graph_close();
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_equal(owner.active_sources,0u); free_ast();
    }
    parse_scope(sequence,0); check_equal(graph_open(0),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_sql_dependencies_open(&scope,&owner,parameters,false,&graph,&error),TURBODB_STATUS_UNSUPPORTED);
  }
  it("resumes dependency close after BUSY without refunding previously closed nodes twice") {
    parse_scope("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<3),unused(n) AS (SELECT d.n FROM (SELECT 9 AS n) d) SELECT n FROM c",0);
    check_equal(graph_open(DEPTH),TURBODB_STATUS_OK);
    const sqlparser_id body=sqlparser_get_node(document,scope.root)->as.with.body;
    const sqlparser_id table=sqlparser_get_node(document,body)->as.select.from;
    orm_sql_cte_store *cache=NULL;
    for(size_t i=0;i<graph.derived_count;++i) {
      const orm_sql_derived_binding *binding=vec_at_const(&graph.derived,i);
      /* CTE occurrences publish the CTE reader source; retain a separate cache lease. */
      if(binding->node==table) cache=((orm_sql_cte_reader *)binding->source->context)->store;
    }
    check_not_null(cache); check_equal(orm_sql_cte_reader_open(cache,&reader,&error),TURBODB_STATUS_OK);
    check_equal(orm_tidesdb_sql_runtime_close(&graph_root,&error),TURBODB_STATUS_OK);
    free_ast(); const size_t prepared=graph.prepared;
    check_equal(orm_sql_dependencies_close(&graph,&error),TURBODB_STATUS_BUSY);
    check_true(graph.prepared<prepared); check_true(graph.prepared>0);
    check_equal(orm_sql_cte_reader_close(&reader,&error),TURBODB_STATUS_OK); graph_close();
    check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],budget.retained_work_bytes);
  }
  it("propagates recursive member failure without exposing a seed prefix to the root query") {
    parse_scope("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c) SELECT n FROM c",0);
    check_equal(graph_open(1),TURBODB_STATUS_OK); free_ast(); orm_sql_scan_row row={.state=ORM_SQL_SCAN_CANCELLED};
    check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(row.state,ORM_SQL_SCAN_CANCELLED);
    check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
  }
  it("refunds every recursive dependency graph construction allocation failure") {
    parse_scope("WITH RECURSIVE k(step) AS (SELECT 1),c(n) AS (SELECT 1 UNION ALL SELECT c.n+d.step FROM c CROSS JOIN (SELECT step FROM k) d WHERE n<3) SELECT n FROM c",0);
    reserves=resizes=0; check_equal(graph_open(DEPTH),TURBODB_STATUS_OK); const size_t allocations[]={reserves,resizes}; graph_close();
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
      reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(graph_open(DEPTH),TURBODB_STATUS_OUT_OF_MEMORY); fail_reserve=fail_resize=0; graph_close();
      check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("refunds every construction step boundary in recursive dependency discovery and binding") {
    parse_scope(sequence,0); const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(graph_open(DEPTH),TURBODB_STATUS_OK); const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    graph_close(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(uint64_t point=0;point<steps;++point) {
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      check_equal(graph_open(DEPTH),TURBODB_STATUS_LIMIT_EXCEEDED); graph_close();
      check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("cleans every allocation failure while graph callbacks reopen nested member dependencies") {
    parse_scope("WITH RECURSIVE k(step) AS (SELECT 1),c(n) AS (SELECT 1 UNION ALL SELECT c.n+d.step FROM c CROSS JOIN (SELECT x.step FROM (SELECT step FROM k) x) d WHERE n<3) SELECT n FROM c",0);
    check_equal(graph_open(DEPTH),TURBODB_STATUS_OK); orm_sql_scan_row row;
    reserves=resizes=0; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
    const size_t allocations[]={reserves,resizes}; graph_close(); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
      check_equal(graph_open(DEPTH),TURBODB_STATUS_OK); row=(orm_sql_scan_row){.state=ORM_SQL_SCAN_CANCELLED};
      reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OUT_OF_MEMORY);
      fail_reserve=fail_resize=0; check_equal(row.state,ORM_SQL_SCAN_CANCELLED); graph_close();
      check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("cleans every execution step boundary across recursive graph cache and streaming inputs") {
    parse_scope("WITH RECURSIVE k(step) AS (SELECT 1),c(n) AS (SELECT 1 UNION ALL SELECT c.n+d.step FROM c CROSS JOIN (SELECT step FROM k) d WHERE n<3) SELECT n FROM c",0);
    check_equal(graph_open(DEPTH),TURBODB_STATUS_OK); const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    orm_sql_scan_row row; check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_OK);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start; graph_close();
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(uint64_t point=0;point<steps;++point) {
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=LIMIT;
      check_equal(graph_open(DEPTH),TURBODB_STATUS_OK); row=(orm_sql_scan_row){.state=ORM_SQL_SCAN_CANCELLED};
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      check_equal(orm_tidesdb_sql_runtime_next(&graph_root,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
      check_equal(row.state,ORM_SQL_SCAN_CANCELLED); graph_close();
      check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("owns seed schema member plans and independent frontiers for a multi-member recursive definition") {
    parse(multiple,0); const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(owner_open(owner_spec()),TURBODB_STATUS_OK); check_equal(owned.cache.state,ORM_SQL_CTE_PENDING);
    check_equal(owned.cache.iterations,0u); owner_reader();
    check_equal(orm_sql_cte_query_close(&owned,&error),TURBODB_STATUS_BUSY);
    const int64_t expected[]={1,10,2,11,3,12}; values(expected,sizeof(expected)/sizeof(expected[0]));
    check_equal(owned.cache.iterations,3u); check_equal(owned.cache.frontier_readers,0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    check_equal(orm_sql_cte_reader_rewind(&reader,&error),TURBODB_STATUS_OK); values(expected,sizeof(expected)/sizeof(expected[0]));
    check_equal(owned.cache.iterations,3u);
  }
  it("infers cumulative DISTINCT from recursive edges but not seed-only UNION DISTINCT") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<2 UNION ALL SELECT n+1 FROM c WHERE n<2) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<2 UNION SELECT n+1 FROM c WHERE n<2) SELECT n FROM c"};
    const int64_t expected[]={1,2,2};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse(sql[i],0); check_equal(owner_open(owner_spec()),TURBODB_STATUS_OK);
      check_equal(owned.cache.recursion.distinct,i!=0); owner_reader(); values(expected,i?2:3); clean_plans();
    }
  }
  it("owns multi-column parameter payloads across AST and input buffer destruction") {
    char label[]="owned"; unsigned char blob[]={0,1,255};
    parameters[0]=turbodb_i64(1); parameters[1]=turbodb_text(label); parameters[2]=turbodb_blob(blob,sizeof(blob)); parameters[3]=turbodb_i64(3);
    parameter_types[1]=(orm_sql_type){TURBODB_VALUE_TEXT,false}; parameter_types[2]=(orm_sql_type){TURBODB_VALUE_BLOB,false};
    parse("WITH RECURSIVE c(n,label,payload) AS (SELECT ?,?,? UNION ALL SELECT n+1,label,payload FROM c WHERE n<?) SELECT n FROM c",PARAMETERS);
    check_equal(owner_open(owner_spec()),TURBODB_STATUS_OK); owner_reader();
    memset(label,'X',sizeof(label)-1); memset(blob,0,sizeof(blob)); parameters[3]=turbodb_i64(99);
    orm_sql_row_source *source=orm_sql_cte_reader_source(&reader);
    for(int64_t n=1;n<=3;++n) {
      const turbodb_value_t *row=NULL; check_equal(source->next(source->context,&row,&error),TURBODB_STATUS_OK); check_not_null(row);
      check_equal(row[0].data.int64_value,n); check_equal(row[1].data.text_value.len,5u);
      check_equal(memcmp(row[1].data.text_value.data,"owned",5),0);
      const unsigned char expected[]={0,1,255}; check_equal(row[2].data.blob_value.size,sizeof(expected));
      check_equal(memcmp(row[2].data.blob_value.data,expected,sizeof(expected)),0);
    }
    const turbodb_value_t *row=NULL; check_equal(source->next(source->context,&row,&error),TURBODB_STATUS_OK); check_null(row);
  }
  it("returns no partial seed output when the explicit iteration limit is exhausted") {
    parse(sequence,0); orm_sql_cte_query_spec spec=owner_spec(); spec.max_iterations=1;
    check_equal(owner_open(spec),TURBODB_STATUS_OK); owner_reader();
    orm_sql_row_source *source=orm_sql_cte_reader_source(&reader); const turbodb_value_t sentinel=turbodb_i64(99),*row=&sentinel;
    check_equal(source->next(source->context,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_true(row==&sentinel);
    check_equal(owned.cache.state,ORM_SQL_CTE_FAILED); check_equal(owned.cache.frontier_readers,0u);
    check_equal(source->next(source->context,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED);
  }
  it("skips all round dependencies for an empty seed or cancellation before demand") {
    const char *sql[]={"WITH RECURSIVE c(n) AS (SELECT 1 WHERE FALSE UNION ALL SELECT n+1 FROM c) SELECT n FROM c",sequence};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse(sql[i],0); orm_sql_cte_query_spec spec=owner_spec(); spec.rounds=(orm_sql_cte_query_rounds){&hooks,inputs_open,inputs_close};
      check_equal(owner_open(spec),TURBODB_STATUS_OK); owner_reader();
      if(i) check_equal(orm_sql_cte_store_cancel(&owned.cache,&error),TURBODB_STATUS_OK);
      orm_sql_row_source *source=orm_sql_cte_reader_source(&reader); const turbodb_value_t *row=NULL;
      check_equal(source->next(source->context,&row,&error),i?TURBODB_STATUS_INVALID_STATE:TURBODB_STATUS_OK); check_null(row);
      check_equal(hooks.opens,0u); check_equal(hooks.closes,0u); check_equal(owned.cache.iterations,0u);
      clean_plans();
    }
  }
  it("rejects partial-member pagination type mismatches and unbounded recursion during construction") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS ((SELECT 1 UNION ALL SELECT n+1 FROM c LIMIT 2) UNION ALL SELECT n+10 FROM c) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT 'bad' FROM c) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n,n FROM c) SELECT n FROM c"};
    const turbodb_status_t statuses[]={TURBODB_STATUS_UNSUPPORTED,TURBODB_STATUS_TYPE_ERROR,TURBODB_STATUS_SQL_ERROR};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse(sql[i],0); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      check_equal(owner_open(owner_spec()),statuses[i]); check_null(owned.budget);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); clean_plans();
    }
    parse(sequence,0); orm_sql_cte_query_spec spec=owner_spec(); spec.max_iterations=0;
    check_equal(owner_open(spec),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(owner_open(owner_spec()),TURBODB_STATUS_OK); const void *columns=owned.schema.schema.columns;
    check_equal(owner_open(owner_spec()),TURBODB_STATUS_INVALID_ARGUMENT); check_true(owned.schema.schema.columns==columns);
  }
  it("opens schema-only EXPLAIN without a recursive cache or round callbacks") {
    parse(multiple,0); const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    orm_sql_cte_query_spec spec=owner_spec(); spec.describe=true;
    spec.rounds=(orm_sql_cte_query_rounds){&hooks,inputs_open,inputs_close};
    check_equal(owner_open(spec),TURBODB_STATUS_OK); free_ast(); check_null(owned.cache.budget);
    orm_sql_compound *plans[]={&owned.initial,&owned.recursive};
    for(size_t i=0;i<sizeof(plans)/sizeof(plans[0]);++i) {
      orm_sql_scan_row row; size_t rows=0;
      do { check_equal(orm_tidesdb_sql_scan_next(plans[i]->scan,&row,&error),TURBODB_STATUS_OK); if(row.state==ORM_SQL_SCAN_ROW) ++rows; }
      while(row.state==ORM_SQL_SCAN_ROW);
      check_true(rows>0);
    }
    check_equal(hooks.opens,0u); check_equal(hooks.closes,0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
  }
  it("replays an external derived query through explicit member dependency callbacks") {
    open_external("SELECT 1 AS step");
    orm_sql_schema_column column; check_equal(orm_tidesdb_sql_runtime_column(&external,0,&column,&error),TURBODB_STATUS_OK);
    const orm_sql_table_schema external_schema={vstr_from_cstr("d"),&column,1};
    orm_sql_row_source source={&budget,&column.type,1,&external,external_pull,false};
    parse("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT c.n+d.step FROM c JOIN (SELECT 1 AS step) d ON 1=1 WHERE c.n<4) SELECT n FROM c",0);
    sqlparser_id derived=0;
    for(size_t i=1;i<=sqlparser_node_count(document);++i) {
      const sqlparser_node *node=sqlparser_get_node(document,(sqlparser_id)i);
      if(node->kind==SQLPARSER_TABLE && node->as.table.query) derived=(sqlparser_id)i;
    }
    check_not_equal(derived,0u); orm_sql_derived_binding binding={derived,&external_schema,&source};
    scope.derived=&binding; scope.derived_count=1;
    check_equal(owner_open(owner_spec()),TURBODB_STATUS_INVALID_ARGUMENT);
    orm_sql_cte_query_spec spec=owner_spec(); spec.rounds=(orm_sql_cte_query_rounds){&hooks,inputs_open,inputs_close};
    check_equal(owner_open(spec),TURBODB_STATUS_OK); owner_reader(); binding=(orm_sql_derived_binding){0};
    const int64_t expected[]={1,2,3,4}; values(expected,sizeof(expected)/sizeof(expected[0]));
    check_equal(hooks.opens,4u); check_equal(hooks.closes,hooks.opens); check_false(source.active);
    check_true(external.execution_closed); clean_plans();
  }
  it("copies the expression registry and keeps scalar caches across recursive rounds") {
    open_external("SELECT 2 AS step"); external_type=(orm_sql_type){TURBODB_VALUE_INT64,false};
    external_source=(orm_sql_row_source){&budget,&external_type,1,&external,external_pull,false};
    check_equal(orm_tidesdb_sql_subquery_open(&external_source,ORM_SQL_SUBQUERY_SCALAR,NULL,&external_scalar,&error),TURBODB_STATUS_OK);
    parse("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n+(SELECT 2 AS step) FROM c WHERE n<6) SELECT n FROM c",0);
    orm_sql_expr_query_source *sources[]={orm_tidesdb_sql_subquery_source(&external_scalar)};
    sqlparser_id scalar=0;
    for(size_t i=1;i<=sqlparser_node_count(document);++i)
      if(sqlparser_get_node(document,(sqlparser_id)i)->kind==SQLPARSER_SUBQUERY) scalar=(sqlparser_id)i;
    check_not_equal(scalar,0u); orm_sql_expr_query_binding binding={scalar,sources[0]->type};
    scope.queries=&binding; scope.query_count=1;
    const orm_sql_expr_query_sources registry={sources,1}; orm_sql_cte_query_spec spec=owner_spec(); spec.sources=&registry;
    reserves=resizes=0; check_equal(owner_open(spec),TURBODB_STATUS_OK); const size_t allocations[]={reserves,resizes};
    check_equal(orm_sql_cte_query_close(&owned,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; const size_t leases=owner.active_sources;
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
      reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(owner_open(spec),TURBODB_STATUS_OUT_OF_MEMORY); fail_reserve=fail_resize=0;
      check_null(owned.budget); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
      check_equal(owner.active_sources,leases); check_equal(external_scalar.source.active_runs,0u);
    }
    check_equal(owner_open(spec),TURBODB_STATUS_OK); owner_reader(); sources[0]=NULL; binding=(orm_sql_expr_query_binding){0};
    const int64_t expected[]={1,3,5,7}; values(expected,sizeof(expected)/sizeof(expected[0]));
    check_equal(external_scalar.state,ORM_SQL_SUBQUERY_READY); check_equal(external_scalar.source.active_runs,0u);
    check_true(external.as.select.unit.done); clean_plans();
  }
  it("retains a failed dependency cleanup for explicit close retry") {
    parse(sequence,0); orm_sql_cte_query_spec spec=owner_spec(); spec.rounds=(orm_sql_cte_query_rounds){&hooks,inputs_open,inputs_close};
    check_equal(owner_open(spec),TURBODB_STATUS_OK); owner_reader(); hooks.fail_open=hooks.fail_close=true;
    orm_sql_row_source *source=orm_sql_cte_reader_source(&reader); const turbodb_value_t sentinel=turbodb_i64(99),*row=&sentinel;
    check_equal(source->next(source->context,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_true(row==&sentinel);
    check_true(owned.inputs_open); check_true(owned.cache.frontier_readers>0);
    check_equal(orm_sql_cte_reader_close(&reader,&error),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_query_close(&owned,&error),TURBODB_STATUS_BUSY); check_not_null(owned.budget);
    hooks.fail_close=false; check_equal(orm_sql_cte_query_close(&owned,&error),TURBODB_STATUS_OK); check_null(owned.budget);
    check_equal(hooks.opens,1u); check_equal(hooks.closes,3u);
  }
  it("refunds every allocation failure across recursive owner construction") {
    parameters[0]=turbodb_i64(1);
    parse("WITH RECURSIVE c(n) AS (SELECT ? UNION ALL SELECT 10 UNION ALL SELECT n+1 FROM c WHERE n<3 UNION ALL SELECT n+10 FROM c WHERE n<3) SELECT n FROM c",1);
    reserves=resizes=0; check_equal(owner_open(owner_spec()),TURBODB_STATUS_OK);
    const size_t allocations[]={reserves,resizes}; check_equal(orm_sql_cte_query_close(&owned,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
      reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(owner_open(owner_spec()),TURBODB_STATUS_OUT_OF_MEMORY); fail_reserve=fail_resize=0;
      check_null(owned.budget); check_equal(owner.active_sources,0u);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("refunds every construction step boundary of the recursive owner") {
    parse(sequence,0); const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    check_equal(owner_open(owner_spec()),TURBODB_STATUS_OK); const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    check_equal(orm_sql_cte_query_close(&owned,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(uint64_t point=0;point<steps;++point) {
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      check_equal(owner_open(owner_spec()),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(owned.budget);
      check_equal(owner.active_sources,0u); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
    }
  }
  it("cleans every allocation failure during production owner iteration without a partial result") {
    parse(multiple,0); check_equal(owner_open(owner_spec()),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_reader_open(&owned.cache,&reader,&error),TURBODB_STATUS_OK);
    orm_sql_row_source *source=orm_sql_cte_reader_source(&reader); const turbodb_value_t *row=NULL;
    reserves=resizes=0; check_equal(source->next(source->context,&row,&error),TURBODB_STATUS_OK);
    const size_t allocations[]={reserves,resizes};
    check_equal(orm_sql_cte_reader_close(&reader,&error),TURBODB_STATUS_OK); check_equal(orm_sql_cte_query_close(&owned,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
      check_equal(owner_open(owner_spec()),TURBODB_STATUS_OK); check_equal(orm_sql_cte_reader_open(&owned.cache,&reader,&error),TURBODB_STATUS_OK);
      source=orm_sql_cte_reader_source(&reader); const turbodb_value_t sentinel=turbodb_i64(99); row=&sentinel;
      reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
      check_equal(source->next(source->context,&row,&error),TURBODB_STATUS_OUT_OF_MEMORY); fail_reserve=fail_resize=0;
      check_true(row==&sentinel); check_equal(owned.cache.state,ORM_SQL_CTE_FAILED);
      check_equal(owned.cache.frontier_readers,0u);
      check_equal(orm_sql_cte_reader_close(&reader,&error),TURBODB_STATUS_OK); check_equal(orm_sql_cte_query_close(&owned,&error),TURBODB_STATUS_OK);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_equal(owner.active_sources,0u);
    }
  }
  it("cleans every execution step boundary of the production recursive owner") {
    parse(multiple,0); check_equal(owner_open(owner_spec()),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_reader_open(&owned.cache,&reader,&error),TURBODB_STATUS_OK);
    const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
    orm_sql_row_source *source=orm_sql_cte_reader_source(&reader); const turbodb_value_t *row=NULL;
    check_equal(source->next(source->context,&row,&error),TURBODB_STATUS_OK);
    const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
    check_equal(orm_sql_cte_reader_close(&reader,&error),TURBODB_STATUS_OK); check_equal(orm_sql_cte_query_close(&owned,&error),TURBODB_STATUS_OK);
    const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    for(uint64_t point=0;point<steps;++point) {
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=LIMIT;
      check_equal(owner_open(owner_spec()),TURBODB_STATUS_OK); check_equal(orm_sql_cte_reader_open(&owned.cache,&reader,&error),TURBODB_STATUS_OK);
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
      source=orm_sql_cte_reader_source(&reader); const turbodb_value_t sentinel=turbodb_i64(99); row=&sentinel;
      check_equal(source->next(source->context,&row,&error),TURBODB_STATUS_LIMIT_EXCEEDED); check_true(row==&sentinel);
      check_equal(owned.cache.frontier_readers,0u);
      check_equal(orm_sql_cte_reader_close(&reader,&error),TURBODB_STATUS_OK); check_equal(orm_sql_cte_query_close(&owned,&error),TURBODB_STATUS_OK);
      check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_equal(owner.active_sources,0u);
    }
  }
  it("compiles both parts from one AST and runs multiple independent recursive members after AST destruction") {
    parse(multiple,0); const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(part_open(ORM_SQL_CTE_INITIAL_PART,false),TURBODB_STATUS_OK); bind_schema(false);
    check_equal(part_open(ORM_SQL_CTE_RECURSIVE_PART,false),TURBODB_STATUS_OK); open_store(false);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    const int64_t expected[]={1,10,2,11,3,12}; values(expected,sizeof(expected)/sizeof(expected[0]));
    check_equal(opens,3u); check_equal(closes,opens);
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads);
    for(size_t i=0;i<member_count;++i) { check_false(proxies[i].source.active); check_null(proxies[i].reader.store); }
  }
  it("preserves seed grouping sorting and pagination while excluding the whole-recursion tail") {
    parse("WITH RECURSIVE c(n) AS ((SELECT 1 AS n UNION ALL SELECT 2 ORDER BY n DESC LIMIT 1) UNION ALL SELECT n+1 FROM c WHERE n<4 LIMIT 0) SELECT n FROM c",0);
    check_equal(part_open(ORM_SQL_CTE_INITIAL_PART,false),TURBODB_STATUS_OK);
    check_equal(initial_next().values[0].data.int64_value,2); check_equal(initial_next().state,ORM_SQL_SCAN_DONE);
    rewind_initial(); bind_schema(false); check_equal(part_open(ORM_SQL_CTE_RECURSIVE_PART,false),TURBODB_STATUS_OK);
    open_store(false); const int64_t expected[]={2,3,4}; values(expected,sizeof(expected)/sizeof(expected[0]));
  }
  it("retains UNION DISTINCT inside the seed and cumulative deduplication across members") {
    parse("WITH RECURSIVE c(n) AS (SELECT 1 UNION SELECT 1 UNION SELECT n+1 FROM c WHERE n<3 UNION SELECT n+1 FROM c WHERE n<3) SELECT n FROM c",0);
    check_equal(part_open(ORM_SQL_CTE_INITIAL_PART,false),TURBODB_STATUS_OK);
    check_equal(initial_next().values[0].data.int64_value,1); check_equal(initial_next().state,ORM_SQL_SCAN_DONE);
    rewind_initial(); bind_schema(false); check_equal(part_open(ORM_SQL_CTE_RECURSIVE_PART,false),TURBODB_STATUS_OK);
    open_store(true); const int64_t expected[]={1,2,3}; values(expected,sizeof(expected)/sizeof(expected[0]));
  }
  it("projects both sides of a right-nested seed boundary without losing seed rows") {
    parse("WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL (SELECT 10 UNION ALL SELECT n+1 FROM c WHERE n<3)) SELECT n FROM c",0);
    check_equal(part_open(ORM_SQL_CTE_INITIAL_PART,false),TURBODB_STATUS_OK); bind_schema(false);
    check_equal(part_open(ORM_SQL_CTE_RECURSIVE_PART,false),TURBODB_STATUS_OK); open_store(false);
    const int64_t expected[]={1,10,2,3}; values(expected,sizeof(expected)/sizeof(expected[0]));
  }
  it("keeps full-document parameter positions across seed member and outer markers") {
    parameters[0]=turbodb_i64(1); parameters[1]=turbodb_i64(2); parameters[2]=turbodb_i64(6); parameters[3]=turbodb_i64(0);
    parse("WITH RECURSIVE c(n) AS (SELECT ? UNION ALL SELECT n+? FROM c WHERE n<?) SELECT n FROM c LIMIT ?",PARAMETERS);
    check_equal(part_open(ORM_SQL_CTE_INITIAL_PART,false),TURBODB_STATUS_OK); bind_schema(false);
    check_equal(part_open(ORM_SQL_CTE_RECURSIVE_PART,false),TURBODB_STATUS_OK); open_store(false);
    const int64_t expected[]={1,3,5,7}; values(expected,sizeof(expected)/sizeof(expected[0]));
  }
  it("reopens native JOIN sources in compiled recursive members on the same transaction") {
    execute("CREATE TABLE items (id BIGINT PRIMARY KEY, score BIGINT)");
    execute("INSERT INTO items(id,score) VALUES(1,10),(2,20)");
    parse("WITH RECURSIVE c(n) AS (SELECT id FROM items WHERE id=1 UNION ALL SELECT c.n+i.id FROM c JOIN items i ON i.id=1 WHERE c.n<4) SELECT n FROM c",0);
    check_equal(part_open(ORM_SQL_CTE_INITIAL_PART,false),TURBODB_STATUS_OK); bind_schema(false);
    check_equal(part_open(ORM_SQL_CTE_RECURSIVE_PART,false),TURBODB_STATUS_OK);
    const size_t leases=owner.active_sources; check_true(leases>0); open_store(false);
    const int64_t expected[]={1,2,3,4}; values(expected,sizeof(expected)/sizeof(expected[0]));
    check_equal(opens,4u); check_equal(closes,opens); check_equal(owner.active_sources,leases);
  }
  it("retains recursive expression failures without publishing the seed prefix") {
    parse("WITH RECURSIVE c(n) AS (SELECT 9223372036854775807 UNION ALL SELECT n+1 FROM c) SELECT n FROM c",0);
    check_equal(part_open(ORM_SQL_CTE_INITIAL_PART,false),TURBODB_STATUS_OK); bind_schema(false);
    check_equal(part_open(ORM_SQL_CTE_RECURSIVE_PART,false),TURBODB_STATUS_OK); open_store(false);
    orm_sql_row_source *source=orm_sql_cte_reader_source(&reader); const turbodb_value_t sentinel=turbodb_i64(99),*row=&sentinel;
    const turbodb_status_t status=source->next(source->context,&row,&error);
    check_not_equal(status,TURBODB_STATUS_OK); check_true(row==&sentinel); check_equal(store.state,ORM_SQL_CTE_FAILED);
    check_equal(source->next(source->context,&row,&error),status); check_true(row==&sentinel);
    check_equal(opens,1u); check_equal(closes,opens); check_false(proxies[0].source.active);
  }
  it("resolves all seed UNION types before assigning recursive nullable schema") {
    parse("WITH RECURSIVE c(n) AS (SELECT NULL UNION ALL SELECT 1 UNION ALL SELECT n+1 FROM c WHERE n<2) SELECT n FROM c",0);
    check_equal(part_open(ORM_SQL_CTE_INITIAL_PART,false),TURBODB_STATUS_OK); bind_schema(false);
    check_equal(schema.schema.columns[0].type.kind,TURBODB_VALUE_INT64); check_true(schema.schema.columns[0].type.nullable);
    check_equal(part_open(ORM_SQL_CTE_RECURSIVE_PART,false),TURBODB_STATUS_OK); open_store(false);
    orm_sql_row_source *source=orm_sql_cte_reader_source(&reader); const turbodb_value_t *row=NULL;
    check_equal(source->next(source->context,&row,&error),TURBODB_STATUS_OK); check_not_null(row); check_equal(row[0].kind,TURBODB_VALUE_NULL);
    const int64_t expected[]={1,2}; values(expected,sizeof(expected)/sizeof(expected[0]));
  }
  it("binds EXPLAIN parts using schema-only self references without execution or data reads") {
    parse(multiple,0); const uint64_t reads=budget.used.value[ORM_SQL_BUDGET_READ_ROWS];
    check_equal(part_open(ORM_SQL_CTE_INITIAL_PART,true),TURBODB_STATUS_OK); bind_schema(true);
    check_equal(part_open(ORM_SQL_CTE_RECURSIVE_PART,true),TURBODB_STATUS_OK);
    check_equal(orm_sql_cte_schema_member(&schema,recursive.plan,&error),TURBODB_STATUS_OK); free_ast();
    orm_sql_compound *plans[]={&initial,&recursive};
    for(size_t i=0;i<sizeof(plans)/sizeof(plans[0]);++i) {
      size_t rows=0; orm_sql_scan_row row;
      do { check_equal(orm_tidesdb_sql_scan_next(plans[i]->scan,&row,&error),TURBODB_STATUS_OK); if(row.state==ORM_SQL_SCAN_ROW) ++rows; }
      while(row.state==ORM_SQL_SCAN_ROW);
      check_true(rows>0); check_equal(orm_sql_compound_execution_close(plans[i],&error),TURBODB_STATUS_INVALID_STATE);
    }
    check_equal(budget.used.value[ORM_SQL_BUDGET_READ_ROWS],reads); check_equal(opens,0u);
  }
  it("rejects member width and type mismatches before opening recursive storage") {
    const char *sql[]={
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT 'bad' FROM c) SELECT n FROM c",
      "WITH RECURSIVE c(n) AS (SELECT 1 UNION ALL SELECT n,n+1 FROM c) SELECT n FROM c"};
    for(size_t i=0;i<sizeof(sql)/sizeof(sql[0]);++i) {
      parse(sql[i],0); check_equal(part_open(ORM_SQL_CTE_INITIAL_PART,false),TURBODB_STATUS_OK); bind_schema(false);
      check_equal(part_open(ORM_SQL_CTE_RECURSIVE_PART,false),TURBODB_STATUS_OK);
      check_equal(orm_sql_cte_schema_member(&schema,recursive.plan,&error),i?TURBODB_STATUS_SQL_ERROR:TURBODB_STATUS_TYPE_ERROR);
      check_null(store.budget); clean_plans();
    }
  }
  it("rejects absent parts wrong roots non-value demand and occupied output without mutation") {
    parse("WITH c(n) AS (SELECT 1) SELECT n FROM c",0);
    check_equal(part_open(ORM_SQL_CTE_RECURSIVE_PART,false),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(part_open(ORM_SQL_CTE_MIXED_PARTS,false),TURBODB_STATUS_INVALID_ARGUMENT);
    check_equal(orm_sql_compound_open_cte_part(&scope,&shape,ORM_SQL_CTE_INITIAL_PART,&owner,NULL,false,NULL,&initial,&error),TURBODB_STATUS_INVALID_ARGUMENT);
    scope.demand=ORM_SQL_QUERY_EXISTENCE; check_equal(part_open(ORM_SQL_CTE_INITIAL_PART,false),TURBODB_STATUS_INVALID_ARGUMENT);
    scope.demand=ORM_SQL_QUERY_VALUES; check_equal(part_open(ORM_SQL_CTE_INITIAL_PART,false),TURBODB_STATUS_OK);
    const orm_sql_select *plan=initial.plan; const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
    check_equal(part_open(ORM_SQL_CTE_INITIAL_PART,false),TURBODB_STATUS_INVALID_ARGUMENT);
    check_true(initial.plan==plan); check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("refunds every allocation failure when constructing each CTE part") {
    parse(multiple,0); check_equal(part_open(ORM_SQL_CTE_INITIAL_PART,false),TURBODB_STATUS_OK); bind_schema(false);
    check_equal(orm_tidesdb_sql_compound_close(&initial,&error),TURBODB_STATUS_OK);
    const orm_sql_cte_parts parts[]={ORM_SQL_CTE_INITIAL_PART,ORM_SQL_CTE_RECURSIVE_PART};
    for(size_t p=0;p<sizeof(parts)/sizeof(parts[0]);++p) {
      orm_sql_compound *plan=p?&recursive:&initial;
      reserves=resizes=0; check_equal(part_open(parts[p],false),TURBODB_STATUS_OK);
      const size_t allocations[]={reserves,resizes}; check_equal(orm_tidesdb_sql_compound_close(plan,&error),TURBODB_STATUS_OK);
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(size_t pass=0;pass<2;++pass) for(size_t point=1;point<=allocations[pass];++point) {
        reserves=resizes=0; if(pass) fail_resize=point; else fail_reserve=point;
        check_equal(part_open(parts[p],false),TURBODB_STATUS_OUT_OF_MEMORY); fail_reserve=fail_resize=0;
        check_null(plan->budget); check_equal(owner.active_sources,0u);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
        for(size_t i=0;i<member_count;++i) check_false(proxies[i].source.active);
      }
    }
  }
  it("checks the current depth limit before publishing either CTE part") {
    parse(multiple,0); const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES]; scope.max_depth=1;
    check_equal(part_open(ORM_SQL_CTE_INITIAL_PART,false),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_equal(part_open(ORM_SQL_CTE_RECURSIVE_PART,false),TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(initial.budget); check_null(recursive.budget); check_equal(owner.active_sources,0u);
    check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work);
  }
  it("refunds every construction step boundary for seed and member plans") {
    parse(sequence,0); check_equal(part_open(ORM_SQL_CTE_INITIAL_PART,false),TURBODB_STATUS_OK); bind_schema(false);
    check_equal(orm_tidesdb_sql_compound_close(&initial,&error),TURBODB_STATUS_OK);
    const orm_sql_cte_parts parts[]={ORM_SQL_CTE_INITIAL_PART,ORM_SQL_CTE_RECURSIVE_PART};
    for(size_t p=0;p<sizeof(parts)/sizeof(parts[0]);++p) {
      orm_sql_compound *plan=p?&recursive:&initial;
      const uint64_t start=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS];
      check_equal(part_open(parts[p],false),TURBODB_STATUS_OK);
      const uint64_t steps=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]-start;
      check_equal(orm_tidesdb_sql_compound_close(plan,&error),TURBODB_STATUS_OK);
      const uint64_t work=budget.used.value[ORM_SQL_BUDGET_WORK_BYTES];
      for(uint64_t point=0;point<steps;++point) {
        budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=budget.used.value[ORM_SQL_BUDGET_EXECUTION_STEPS]+point;
        check_equal(part_open(parts[p],false),TURBODB_STATUS_LIMIT_EXCEEDED); check_null(plan->budget);
        check_equal(budget.used.value[ORM_SQL_BUDGET_WORK_BYTES],work); check_equal(owner.active_sources,0u);
        for(size_t i=0;i<member_count;++i) check_false(proxies[i].source.active);
      }
      budget.limits.statement.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=LIMIT;
    }
  }
}
