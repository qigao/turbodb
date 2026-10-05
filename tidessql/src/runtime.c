#include "runtime.h"
#include "insert.h"
#include "change.h"
#include "index_store.h"
#include "table_clear.h"
#include "table_alter.h"
#include "name.h"
#include "store_internal.h"
#include "work.h"
#include "cte_bind.h"
#include "error.h"
#include <string.h>

static turbodb_status_t runtime_error(turbodb_error_t *error, turbodb_status_t status, const char *reason) {
  tdsql_error_set(error, status, reason); return status;
}
static turbodb_status_t runtime_statement(const sqlparser_document *document, orm_sql_catalog_store *owner,
    const turbodb_value_t *parameters, size_t count, size_t max_depth, const sqlparser_node **out, turbodb_error_t *error) {
  if (!document || !max_depth || (count && !parameters))
    return runtime_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid SQL runtime inputs");
  turbodb_status_t status = orm_sql_store_ready(owner, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (sqlparser_get_dialect(document) != SQLPARSER_MYSQL || sqlparser_statements(document).count != 1)
    return runtime_error(error, TURBODB_STATUS_UNSUPPORTED, "SQL runtime requires exactly one MySQL statement");
  *out = sqlparser_get_node(document, sqlparser_statements(document).first);
  return *out ? TURBODB_STATUS_OK : runtime_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "SQL runtime missing statement");
}
turbodb_status_t orm_tidesdb_sql_runtime_execute(const sqlparser_document *document,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters, size_t count,
    size_t max_depth, uint64_t max_iterations, bool client_found_rows,
    orm_sql_diagnostics *diagnostics, size_t *affected,
    turbodb_error_t *error) {
  return orm_sql_runtime_execute_evaluation(document,owner,parameters,count,max_depth,
      max_iterations,client_found_rows,(orm_sql_evaluation){.diagnostics=diagnostics},affected,error);
}
turbodb_status_t orm_sql_runtime_execute_evaluation(const sqlparser_document *document,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters, size_t count,
    size_t max_depth, uint64_t max_iterations, bool client_found_rows,
    orm_sql_evaluation evaluation, size_t *affected, turbodb_error_t *error) {
  if (!affected) return runtime_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "SQL runtime requires affected output");
  if (evaluation.mode < ORM_SQL_EVALUATION_QUERY || evaluation.mode > ORM_SQL_EVALUATION_IGNORE_WRITE ||
      (evaluation.diagnostics && !evaluation.diagnostics->max_records))
    return runtime_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL statement evaluation context");
  const sqlparser_node *statement = NULL;
  turbodb_status_t status = runtime_statement(document, owner, parameters, count, max_depth, &statement, error);
  if (status != TURBODB_STATUS_OK) return status;
  const sqlparser_node *body = statement->kind == SQLPARSER_WITH ?
      sqlparser_get_node(document, statement->as.with.body) : statement;
  if (!body)
    return runtime_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
        "SQL runtime statement body is missing");
  switch (body->kind) {
    case SQLPARSER_INSERT:
      return orm_sql_insert_execute_evaluation(document, owner, parameters, count,
          max_depth, max_iterations, client_found_rows, evaluation, affected,
          error);
    case SQLPARSER_UPDATE: case SQLPARSER_DELETE:
      return orm_sql_change_execute_evaluation(document, owner, parameters, count,
          max_depth, max_iterations, client_found_rows, evaluation, affected,
          error);
    case SQLPARSER_CREATE_INDEX: {
      if (count) return runtime_error(error, TURBODB_STATUS_SQL_ERROR, "SQL CREATE INDEX accepts no parameters");
      uint64_t id = 0;
      status = orm_tidesdb_sql_index_build(document, owner, &id, error);
      if (status == TURBODB_STATUS_OK) *affected = 0;
      return status;
    }
    case SQLPARSER_DROP_INDEX: {
      if (count) return runtime_error(error, TURBODB_STATUS_SQL_ERROR, "SQL DROP INDEX accepts no parameters");
      uint64_t id = 0;
      status = orm_tidesdb_sql_index_drop(document, owner, &id, error);
      if (status == TURBODB_STATUS_OK) *affected = 0;
      return status;
    }
    case SQLPARSER_DROP_TABLE: case SQLPARSER_TRUNCATE_TABLE:
      if (count) return runtime_error(error, TURBODB_STATUS_SQL_ERROR, "SQL table DDL accepts no parameters");
      status = orm_sql_table_clear(document, owner, error);
      if (status == TURBODB_STATUS_OK) *affected = 0;
      return status;
    case SQLPARSER_ALTER_TABLE:
      if (count) return runtime_error(error, TURBODB_STATUS_SQL_ERROR, "SQL ALTER TABLE accepts no parameters");
      status = orm_sql_table_alter(document, owner, error);
      if (status == TURBODB_STATUS_OK) *affected = 0;
      return status;
    case SQLPARSER_CREATE_TABLE: break;
    default: return runtime_error(error, TURBODB_STATUS_UNSUPPORTED, "SQL runtime command requires supported table/index DDL, INSERT, UPDATE or DELETE");
  }
  if (count) return runtime_error(error, TURBODB_STATUS_SQL_ERROR, "SQL CREATE TABLE accepts no parameters");
  status = orm_sql_store_writable(owner, error);
  orm_sql_create_definition definition = {0}; uint64_t id = 0; bool created = false;
  if (status == TURBODB_STATUS_OK) status = orm_sql_catalog_bind_create_all(document, owner->budget, &definition, error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_store_create_all(owner, &definition, &id, &created, error);
  const turbodb_status_t released = orm_sql_catalog_destroy_all(&definition, status == TURBODB_STATUS_OK ? error : NULL);
  if (released != TURBODB_STATUS_OK) owner->failed = true;
  if (status == TURBODB_STATUS_OK) status = released;
  if (status == TURBODB_STATUS_OK) *affected = 0;
  return status;
}
static turbodb_status_t runtime_table(const sqlparser_document *document, const sqlparser_node *statement,
    vstr *out, turbodb_error_t *error) {
  const sqlparser_node *table = sqlparser_get_node(document, statement->as.select.from);
  if (!table || table->kind != SQLPARSER_TABLE || table->as.table.query || table->as.table.group ||
      table->as.table.arguments.count || table->as.table.indexed_by || table->as.table.table_function || table->as.table.not_indexed)
    return runtime_error(error, TURBODB_STATUS_UNSUPPORTED, "SQL runtime SELECT requires one plain table");
  const char *reason = NULL;
  const turbodb_status_t status = orm_sql_name_node(document, table->as.table.name, out, &reason);
  return status == TURBODB_STATUS_OK ? status : runtime_error(error, status, reason);
}
static bool runtime_inside(const sqlparser_node *node,const sqlparser_node *root) {
  return node&&root&&node->span.offset>=root->span.offset&&
      node->span.offset-root->span.offset<=root->span.length&&
      node->span.length<=root->span.length-(node->span.offset-root->span.offset);
}
static sqlparser_id runtime_dependency_body(const sqlparser_node *node) {
  return node->kind==SQLPARSER_SUBQUERY?node->as.subquery.query:
      node->kind==SQLPARSER_IN?node->as.in.query:
      node->kind==SQLPARSER_UNARY&&node->as.unary.op==SQLPARSER_OP_EXISTS?node->as.unary.operand:0;
}
static turbodb_status_t runtime_has_possible_dependency_reference(const orm_sql_query_scope *scope,
    vstr qualifier,bool *out,turbodb_error_t *error) {
  const size_t count=sqlparser_node_count(scope->document);
  orm_sql_budget_amount amount={0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=count;
  turbodb_status_t status=orm_tidesdb_sql_budget_reserve(scope->budget,&amount,error);
  size_t names=0,dependencies=0; *out=false;
  for(size_t i=1;status==TURBODB_STATUS_OK&&i<=count;++i) {
    const sqlparser_node *name=sqlparser_get_node(scope->document,(sqlparser_id)i);
    if(name&&runtime_dependency_body(name)) ++dependencies;
    if(!name||name->kind!=SQLPARSER_NAME) continue;
    vstr text={sqlparser_text(scope->document,name->span),name->span.length},part={0}; const char *reason=NULL;
    if(orm_sql_name_part(&text,&part,&reason)!=TURBODB_STATUS_OK||
        (text.len&&(text.data[0]!='.'||part.len!=qualifier.len||
          memcmp(part.data,qualifier.data,part.len)))) continue;
    ++names;
  }
  if(status!=TURBODB_STATUS_OK||!names||!dependencies) return status;
  if(names>UINT64_MAX/dependencies)
    return runtime_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"correlation discovery exceeds step capacity");
  amount=(orm_sql_budget_amount){0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=(uint64_t)names*dependencies;
  status=orm_tidesdb_sql_budget_reserve(scope->budget,&amount,error);
  for(size_t i=1;status==TURBODB_STATUS_OK&&!*out&&i<=count;++i) {
    const sqlparser_node *name=sqlparser_get_node(scope->document,(sqlparser_id)i);
    if(!name||name->kind!=SQLPARSER_NAME) continue;
    vstr text={sqlparser_text(scope->document,name->span),name->span.length},part={0}; const char *reason=NULL;
    if(orm_sql_name_part(&text,&part,&reason)!=TURBODB_STATUS_OK||
        (text.len&&(text.data[0]!='.'||part.len!=qualifier.len||
          memcmp(part.data,qualifier.data,part.len)))) continue;
    for(size_t j=1;j<=count;++j) {
      const sqlparser_node *dependency=sqlparser_get_node(scope->document,(sqlparser_id)j);
      const sqlparser_id body=dependency?runtime_dependency_body(dependency):0;
      const sqlparser_node *query=body?sqlparser_get_node(scope->document,body):NULL;
      if(query&&query->kind==SQLPARSER_WITH)
        query=sqlparser_get_node(scope->document,query->as.with.body);
      if(query&&runtime_inside(name,query)) { *out=true; break; }
    }
  }
  return status;
}
static turbodb_status_t runtime_from_qualifier(const sqlparser_document *document,
    const orm_sql_from_table *table,vstr *out,turbodb_error_t *error) {
  const sqlparser_node *ast=table?sqlparser_get_node(document,table->ast):NULL;
  if(!ast||ast->kind!=SQLPARSER_TABLE)
    return runtime_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"JOIN table is missing");
  const char *reason=NULL;
  const turbodb_status_t status=orm_sql_name_node(document,ast->as.table.alias?
      ast->as.table.alias:ast->as.table.name,out,&reason);
  return status==TURBODB_STATUS_OK?status:runtime_error(error,status,reason);
}
static turbodb_status_t runtime_join_candidate(const orm_sql_query_scope *scope,
    const vec_t *names,bool *out,turbodb_error_t *error) {
  *out=false;
  for(size_t i=0;!*out&&i<vec_size(names);++i) {
    vstr qualifier={0};
    turbodb_status_t status=runtime_from_qualifier(scope->document,vec_at_const(names,i),&qualifier,error);
    if(status==TURBODB_STATUS_OK) status=runtime_has_possible_dependency_reference(scope,qualifier,out,error);
    if(status!=TURBODB_STATUS_OK) return status;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t runtime_relation_rewind(void *context,turbodb_error_t *error) {
  return orm_sql_relation_rewind(context,error);
}
static turbodb_status_t runtime_lateral_names(const orm_sql_query_scope *scope,const vec_t *names,
    bool *out,turbodb_error_t *error) {
  *out=false;
  orm_sql_budget_amount charge={0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=vec_size(names);
  const turbodb_status_t status=orm_tidesdb_sql_budget_reserve(scope->budget,&charge,error);
  if(status!=TURBODB_STATUS_OK) return status;
  for(size_t i=0;i<vec_size(names);++i) {
    const orm_sql_from_table *name=vec_at_const(names,i);
    const sqlparser_node *node=sqlparser_get_node(scope->document,name->ast);
    if(node&&node->kind==SQLPARSER_TABLE&&node->as.table.lateral) { *out=true; break; }
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t runtime_join_inputs(const orm_sql_query_scope *scope,
    const vec_t *names,orm_sql_query *query,vec_t *schemas,size_t *schema_bytes,
    turbodb_error_t *error) {
  orm_tidesdb_sql_budget *budget=query->owner->budget;
  const size_t tables=vec_size(names);
  if(query->as.select.relations.initialized||query->as.select.inputs.initialized||
      schemas->initialized||*schema_bytes)
    return runtime_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"empty JOIN inputs required");
  bool lateral=false;
  turbodb_status_t status=runtime_lateral_names(scope,names,&lateral,error);
  const bool dependent=!query->execution_closed&&query->kind!=ORM_SQL_QUERY_EXPLAIN&&lateral;
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&query->as.select.relations,tables,
      sizeof(orm_sql_relation_source),_Alignof(orm_sql_relation_source),budget,
      &query->as.select.relation_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(schemas,tables,
      sizeof(orm_sql_table_schema *),_Alignof(orm_sql_table_schema *),budget,
      schema_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&query->as.select.inputs,tables,
      sizeof(orm_sql_row_source *),_Alignof(orm_sql_row_source *),budget,
      &query->as.select.input_bytes,error);
  if(status==TURBODB_STATUS_OK&&dependent)
    status=orm_sql_work_zero(&query->as.select.dependent_inputs,tables,sizeof(orm_sql_from_input),
        _Alignof(orm_sql_from_input),budget,&query->as.select.dependent_input_bytes,error);
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<tables;++i) {
    const orm_sql_from_table *name=vec_at_const(names,i);
    const orm_sql_derived_binding *binding=NULL;
    for(size_t j=0;j<scope->derived_count;++j) {
      orm_sql_budget_amount amount={0};
      amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
      status=orm_tidesdb_sql_budget_reserve(budget,&amount,error);
      if(status!=TURBODB_STATUS_OK) break;
      if(scope->derived[j].node==name->ast) { binding=&scope->derived[j]; break; }
    }
    if(status!=TURBODB_STATUS_OK) break;
    if(binding||name->derived) {
      /* Lexical metadata owners are closed before any execution can begin;
       * recursive self entries supply schema here and frontier sources later. */
      if(!binding||!binding->schema||(!binding->source&&query->kind!=ORM_SQL_QUERY_EXPLAIN&&!query->execution_closed))
        status=runtime_error(error,TURBODB_STATUS_UNSUPPORTED,"FROM query requires a prepared dependency");
      if(status==TURBODB_STATUS_OK) {
        *(const orm_sql_table_schema **)vec_at(schemas,i)=binding->schema;
        *(orm_sql_row_source **)vec_at(&query->as.select.inputs,i)=binding->source;
        if(dependent) {
          if(!binding->input||binding->input->source!=binding->source)
            status=runtime_error(error,TURBODB_STATUS_UNSUPPORTED,"dependent FROM query requires a rewind or lateral provider");
          else *(orm_sql_from_input *)vec_at(&query->as.select.dependent_inputs,i)=*binding->input;
        }
      }
      continue;
    }
    orm_sql_relation_source *source=vec_at(&query->as.select.relations,i);
    status=orm_tidesdb_sql_relation_open(query->owner,vstr_from_cstr(name->name),source,error);
    if(status==TURBODB_STATUS_OK) {
      *(const orm_sql_table_schema **)vec_at(schemas,i)=&source->schema;
      *(orm_sql_row_source **)vec_at(&query->as.select.inputs,i)=&source->source;
      if(dependent) *(orm_sql_from_input *)vec_at(&query->as.select.dependent_inputs,i)=
          (orm_sql_from_input){.source=&source->source,.context=source,.rewind=runtime_relation_rewind};
    }
  }
  return status;
}
static turbodb_status_t runtime_prepare_dependency_schema(orm_sql_query_scope *scope,
    orm_sql_query *query,bool dependencies_ready,turbodb_error_t *error) {
  const sqlparser_node *statement=sqlparser_get_node(scope->document,scope->root);
  sqlparser_id body_root=scope->root;
  if(statement&&statement->kind==SQLPARSER_WITH) {
    vec_t references={0}; size_t bytes=0;
    turbodb_status_t status=scope->max_iterations?
        orm_sql_cte_resolve(scope,&references,&bytes,error):
        orm_sql_cte_bind(scope,&references,&bytes,error);
    body_root=statement->as.with.body;
    statement=sqlparser_get_node(scope->document,body_root);
    const sqlparser_node *from=statement&&statement->kind==SQLPARSER_SELECT?
        sqlparser_get_node(scope->document,statement->as.select.from):NULL;
    bool cte_source=false;
    const size_t count=sqlparser_node_count(scope->document);
    for(size_t i=1;status==TURBODB_STATUS_OK&&!cte_source&&from&&i<=count;++i) {
      const sqlparser_node *node=sqlparser_get_node(scope->document,(sqlparser_id)i);
      if(runtime_inside(node,from)&&
          *(const sqlparser_id *)vec_at_const(&references,i-1)) cte_source=true;
    }
    const turbodb_status_t released=orm_sql_work_release(&references,bytes,scope->budget,
        status==TURBODB_STATUS_OK?error:NULL);
    if(status==TURBODB_STATUS_OK) status=released;
    if(status!=TURBODB_STATUS_OK||(cte_source&&!dependencies_ready)) return status;
  }
  orm_sql_query_scope body=*scope;
  body.root=body_root;
  if(!statement||statement->kind!=SQLPARSER_SELECT||!statement->as.select.from) return TURBODB_STATUS_OK;
  const sqlparser_node *from=sqlparser_get_node(scope->document,statement->as.select.from);
  if(from&&from->kind==SQLPARSER_JOIN) {
    vec_t names={0},schemas={0}; size_t name_bytes=0,schema_bytes=0;
    turbodb_status_t status=orm_sql_from_subtree_tables_at(&body,statement->as.select.from,&names,&name_bytes,error);
    bool plain=true,correlated=scope->force_dependency_schema;
    for(size_t i=0;status==TURBODB_STATUS_OK&&i<vec_size(&names);++i)
      if(((const orm_sql_from_table *)vec_at_const(&names,i))->derived) plain=false;
    const bool inputs_ready=plain||dependencies_ready;
    if(status==TURBODB_STATUS_OK&&inputs_ready&&!correlated)
      status=runtime_join_candidate(&body,&names,&correlated,error);
    if(status==TURBODB_STATUS_OK&&inputs_ready&&correlated)
      status=runtime_join_inputs(&body,&names,query,&schemas,&schema_bytes,error);
    bool lateral=false;
    if(status==TURBODB_STATUS_OK&&inputs_ready&&correlated)
      status=runtime_lateral_names(&body,&names,&lateral,error);
    if(status==TURBODB_STATUS_OK&&inputs_ready&&correlated)
      status=lateral?
          orm_sql_from_subtree_schema_at(&body,statement->as.select.from,vec_data_const(&schemas),
              vec_size(&schemas),&query->as.select.from,error):
          orm_tidesdb_sql_from_bind_schema_at(&body,vec_data_const(&schemas),vec_size(&schemas),&query->as.select.from,error);
    if(status==TURBODB_STATUS_OK&&inputs_ready&&correlated) {
      scope->dependency_schema=&orm_tidesdb_sql_from_at(&query->as.select.from,0)->schema;
      scope->dependency_qualifier=(vstr){0};
    }
    const turbodb_status_t schemas_released=orm_sql_work_release(&schemas,schema_bytes,
        scope->budget,status==TURBODB_STATUS_OK?error:NULL);
    if(status==TURBODB_STATUS_OK) status=schemas_released;
    const turbodb_status_t names_released=orm_sql_work_release(&names,name_bytes,
        scope->budget,status==TURBODB_STATUS_OK?error:NULL);
    return status==TURBODB_STATUS_OK?names_released:status;
  }
  if(!from||from->kind!=SQLPARSER_TABLE||from->as.table.query||from->as.table.group||
      from->as.table.arguments.count||from->as.table.indexed_by||from->as.table.table_function||from->as.table.not_indexed)
    return TURBODB_STATUS_OK;
  vstr table={0};
  turbodb_status_t status=runtime_table(scope->document,statement,&table,error);
  vstr qualifier=table;
  const char *reason=NULL;
  if(status==TURBODB_STATUS_OK&&from->as.table.alias)
    status=orm_sql_name_node(scope->document,from->as.table.alias,&qualifier,&reason);
  if(status!=TURBODB_STATUS_OK&&reason) tdsql_error_set(error,status,reason);
  bool correlated=scope->force_dependency_schema;
  if(status==TURBODB_STATUS_OK&&!correlated) status=runtime_has_possible_dependency_reference(&body,qualifier,&correlated,error);
  if(status==TURBODB_STATUS_OK&&!correlated) return TURBODB_STATUS_OK;
  if(status==TURBODB_STATUS_OK&&!query->as.select.source.owner)
    status=orm_tidesdb_sql_relation_open(query->owner,table,&query->as.select.source,error);
  if(status==TURBODB_STATUS_OK) {
    scope->dependency_schema=&query->as.select.source.schema;
    scope->dependency_qualifier=qualifier;
  }
  return status;
}
static turbodb_status_t runtime_dependency_schema_prepare(orm_sql_query_scope *scope,
    void *context,turbodb_error_t *error) {
  return runtime_prepare_dependency_schema(scope,context,true,error);
}
static turbodb_status_t runtime_schema_open(const orm_sql_query_scope *scope,sqlparser_id subtree,
    orm_sql_catalog_store *owner,orm_sql_query *out,
    const orm_sql_table_schema **schema,vstr *qualifier,turbodb_error_t *error) {
  const sqlparser_node *statement=scope&&scope->document?
      sqlparser_get_node(scope->document,scope->root):NULL;
  if(!statement||statement->kind!=SQLPARSER_SELECT||!owner||
      scope->budget!=owner->budget||!out||out->owner||!schema||!qualifier)
    return runtime_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid lexical schema inputs");
  *schema=NULL; *qualifier=(vstr){0};
  if(!statement->as.select.from && !subtree) return TURBODB_STATUS_OK;
  size_t bytes=0;
  turbodb_status_t status=orm_tidesdb_sql_budget_reserve_capacity(owner->budget,
      1,sizeof(*out),0,&bytes,error);
  if(status!=TURBODB_STATUS_OK) return status;
  *out=(orm_sql_query){.owner=owner,.kind=ORM_SQL_QUERY_SELECT,
      .metadata_bytes=bytes,.execution_closed=true};
  vec_t names={0},schemas={0}; size_t name_bytes=0,schema_bytes=0;
  status=subtree?orm_sql_from_subtree_tables_at(scope,subtree,&names,&name_bytes,error):
      orm_tidesdb_sql_from_tables_at(scope,&names,&name_bytes,error);
  if(status==TURBODB_STATUS_OK) status=runtime_join_inputs(scope,&names,out,
      &schemas,&schema_bytes,error);
  if(status==TURBODB_STATUS_OK) status=subtree?
      orm_sql_from_subtree_schema_at(scope,subtree,vec_data_const(&schemas),vec_size(&schemas),&out->as.select.from,error):
      orm_tidesdb_sql_from_bind_schema_at(scope,vec_data_const(&schemas),vec_size(&schemas),&out->as.select.from,error);
  if(status==TURBODB_STATUS_OK) *schema=&orm_tidesdb_sql_from_at(&out->as.select.from,0)->schema;
  const turbodb_status_t a=orm_sql_work_release(&schemas,schema_bytes,scope->budget,
      status==TURBODB_STATUS_OK?error:NULL);
  if(status==TURBODB_STATUS_OK) status=a;
  const turbodb_status_t b=orm_sql_work_release(&names,name_bytes,scope->budget,
      status==TURBODB_STATUS_OK?error:NULL);
  if(status==TURBODB_STATUS_OK) status=b;
  if(status!=TURBODB_STATUS_OK) {
    *schema=NULL; *qualifier=(vstr){0};
    const turbodb_status_t released=orm_tidesdb_sql_runtime_close(out,NULL);
    if(released!=TURBODB_STATUS_OK) return released;
  }
  return status;
}
turbodb_status_t orm_sql_runtime_schema_open(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner,orm_sql_query *out,
    const orm_sql_table_schema **schema,vstr *qualifier,turbodb_error_t *error) {
  return runtime_schema_open(scope,0,owner,out,schema,qualifier,error);
}
turbodb_status_t orm_sql_runtime_subtree_schema_open(const orm_sql_query_scope *scope,
    sqlparser_id subtree,orm_sql_catalog_store *owner,orm_sql_query *out,
    const orm_sql_table_schema **schema,vstr *qualifier,turbodb_error_t *error) {
  if(!subtree) return runtime_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"FROM subtree identity required");
  return runtime_schema_open(scope,subtree,owner,out,schema,qualifier,error);
}
turbodb_status_t orm_sql_runtime_lateral_schema_close(orm_sql_lateral_schema *frame,turbodb_error_t *error) {
  if(!frame) return runtime_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"LATERAL schema owner required");
  if(!frame->budget) return TURBODB_STATUS_OK;
  for(size_t i=vec_size(&frame->inputs);i>0;--i) {
    const turbodb_status_t status=orm_tidesdb_sql_runtime_close(vec_at(&frame->inputs,i-1),error);
    if(status!=TURBODB_STATUS_OK) return status;
  }
  turbodb_status_t status=orm_sql_work_release(&frame->columns,frame->column_bytes,frame->budget,error);
  if(status==TURBODB_STATUS_OK) frame->column_bytes=0;
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_release(&frame->inputs,frame->input_bytes,frame->budget,error);
  if(status==TURBODB_STATUS_OK) frame->input_bytes=0;
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_release(&frame->prefixes,frame->prefix_bytes,frame->budget,error);
  if(status==TURBODB_STATUS_OK) frame->prefix_bytes=0;
  if(status==TURBODB_STATUS_OK&&frame->metadata_bytes)
    status=orm_tidesdb_sql_budget_release(frame->budget,ORM_SQL_BUDGET_WORK_BYTES,frame->metadata_bytes,error);
  if(status==TURBODB_STATUS_OK) memset(frame,0,sizeof(*frame));
  return status;
}
turbodb_status_t orm_sql_runtime_lateral_schema_open(const orm_sql_query_scope *scope,
    sqlparser_id lateral,orm_sql_catalog_store *owner,orm_sql_lateral_schema *out,turbodb_error_t *error) {
  if(!scope||!scope->budget||!owner||scope->budget!=owner->budget||!out||out->budget)
    return runtime_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid LATERAL schema owner");
  *out=(orm_sql_lateral_schema){.budget=scope->budget};
  turbodb_status_t status=orm_sql_from_lateral_prefixes_at(scope,lateral,&out->prefixes,&out->prefix_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_budget_reserve_capacity(scope->budget,
      1,sizeof(*out),0,&out->metadata_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&out->inputs,vec_size(&out->prefixes),
      sizeof(orm_sql_query),_Alignof(orm_sql_query),scope->budget,&out->input_bytes,error);
  size_t count=0;
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<vec_size(&out->inputs);++i) {
    const orm_sql_table_schema *input=NULL; vstr qualifier={0};
    status=orm_sql_runtime_subtree_schema_open(scope,*(const sqlparser_id *)vec_at_const(&out->prefixes,i),
        owner,vec_at(&out->inputs,i),&input,&qualifier,error);
    if(status!=TURBODB_STATUS_OK) break;
    if(!input||input->count>SIZE_MAX-count) {
      status=runtime_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"LATERAL frame width overflow"); break;
    }
    count+=input->count;
    if(count>scope->budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
      status=runtime_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"LATERAL frame exceeds plan capacity");
  }
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&out->columns,count,sizeof(orm_sql_schema_column),
      _Alignof(orm_sql_schema_column),scope->budget,&out->column_bytes,error);
  size_t position=0;
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<vec_size(&out->inputs);++i) {
    const orm_sql_query *input=vec_at_const(&out->inputs,i);
    const orm_sql_table_schema *input_schema=&orm_tidesdb_sql_from_at(&input->as.select.from,0)->schema;
    /* Different prefix roots must not publish the same table qualifier. Names
     * within a root have already passed FROM's duplicate qualifier validation. */
    for(size_t column=0;status==TURBODB_STATUS_OK&&column<input_schema->count;++column) {
      const orm_sql_schema_column *source=&input_schema->columns[column];
      orm_sql_budget_amount charge={0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=position+1;
      status=orm_tidesdb_sql_budget_reserve(scope->budget,&charge,error);
      for(size_t earlier=0;status==TURBODB_STATUS_OK&&earlier<position;++earlier) {
        const orm_sql_schema_column *previous=vec_at_const(&out->columns,earlier);
        if(source->qualifier.len==previous->qualifier.len&&
            !memcmp(source->qualifier.data,previous->qualifier.data,source->qualifier.len))
          status=runtime_error(error,TURBODB_STATUS_SQL_ERROR,"duplicate LATERAL frame table qualifier");
      }
    }
    if(status==TURBODB_STATUS_OK) {
      memcpy((orm_sql_schema_column *)vec_data(&out->columns)+position,
          input_schema->columns,input_schema->count*sizeof(orm_sql_schema_column));
      position+=input_schema->count;
    }
  }
  if(status==TURBODB_STATUS_OK) out->schema=(orm_sql_table_schema){.columns=vec_data_const(&out->columns),.count=count};
  else {
    const turbodb_status_t released=orm_sql_runtime_lateral_schema_close(out,NULL);
    if(released!=TURBODB_STATUS_OK) return released;
  }
  return status;
}
static turbodb_status_t runtime_parameter_types(const turbodb_value_t *parameters, size_t count,
    orm_tidesdb_sql_budget *budget, vec_t *types, size_t *bytes, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = count;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(budget,&amount,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(types,count,sizeof(orm_sql_type),_Alignof(orm_sql_type),budget,bytes,error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < count; ++i)
    *(orm_sql_type *)vec_at(types,i) = (orm_sql_type){parameters[i].kind,parameters[i].kind == TURBODB_VALUE_NULL};
  return status;
}
static void lateral_query_clear(orm_sql_lateral_query *run) {
  for(size_t i=0;i<run->capture_count;++i)
    *(turbodb_value_t *)vec_at(&run->arguments.values,run->marker_count+i)=turbodb_null();
}
static turbodb_status_t lateral_query_round_close(void *context,turbodb_error_t *error) {
  orm_sql_lateral_query *run=context;
  if(run->source.active)
    return runtime_error(error,TURBODB_STATUS_BUSY,"LATERAL query source has a consumer");
  const turbodb_status_t status=orm_sql_runtime_execution_close(&run->query,error);
  if(status==TURBODB_STATUS_OK) { lateral_query_clear(run); run->bound=false; }
  return status;
}
static turbodb_status_t lateral_query_round_open(void *context,const turbodb_value_t *row,
    size_t columns,turbodb_error_t *error) {
  orm_sql_lateral_query *run=context;
  if(run->bound||run->source.active||!run->query.execution_closed)
    return runtime_error(error,TURBODB_STATUS_BUSY,"close LATERAL query round before reopening");
  if(columns!=run->capture_count||(columns&&!row))
    return runtime_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"LATERAL query capture width mismatch");
  if(run->query.execution_failure.status!=TURBODB_STATUS_OK)
    return runtime_error(error,run->query.execution_failure.status,run->query.execution_failure.message);
  orm_sql_budget_amount charge={0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=columns;
  turbodb_status_t status=orm_tidesdb_sql_budget_reserve(run->budget,&charge,error);
  if(status!=TURBODB_STATUS_OK) return status;
  if(columns) memcpy((turbodb_value_t *)vec_data(&run->arguments.values)+run->marker_count,
      row,columns*sizeof(turbodb_value_t));
  status=orm_sql_runtime_execution_open(&run->query,
      vec_data_const(&run->arguments.values),run->marker_count+run->capture_count,&run->queries,error);
  if(status==TURBODB_STATUS_OK) run->bound=true;
  else if(run->query.execution_closed) lateral_query_clear(run);
  return status;
}
static turbodb_status_t lateral_query_pull(void *context,const turbodb_value_t **out,turbodb_error_t *error) {
  orm_sql_lateral_query *run=context;
  if(!run->bound) return runtime_error(error,TURBODB_STATUS_INVALID_STATE,"LATERAL query round is closed");
  orm_sql_scan_row row;
  const turbodb_status_t status=orm_tidesdb_sql_runtime_next(&run->query,&row,error);
  if(status!=TURBODB_STATUS_OK) return status;
  if(row.state!=ORM_SQL_SCAN_ROW&&row.state!=ORM_SQL_SCAN_DONE)
    return runtime_error(error,TURBODB_STATUS_INVALID_STATE,"LATERAL query round cancelled");
  *out=row.state==ORM_SQL_SCAN_ROW?row.values:NULL;
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_lateral_query_close(orm_sql_lateral_query *run,turbodb_error_t *error) {
  if(!run||!run->budget) return TURBODB_STATUS_OK;
  if(run->source.active)
    return runtime_error(error,TURBODB_STATUS_BUSY,"LATERAL query source has a consumer");
  turbodb_status_t status=orm_tidesdb_sql_runtime_close(&run->query,error);
  if(status==TURBODB_STATUS_OK) run->bound=false;
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_release(&run->arguments.values,
      run->arguments.bytes,run->budget,error);
  if(status==TURBODB_STATUS_OK) run->arguments.bytes=0;
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_release(&run->types,run->type_bytes,run->budget,error);
  if(status==TURBODB_STATUS_OK) run->type_bytes=0;
  if(status==TURBODB_STATUS_OK&&run->metadata_bytes)
    status=orm_tidesdb_sql_budget_release(run->budget,ORM_SQL_BUDGET_WORK_BYTES,run->metadata_bytes,error);
  if(status==TURBODB_STATUS_OK) memset(run,0,sizeof(*run));
  return status;
}
turbodb_status_t orm_sql_lateral_query_open(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner,const turbodb_value_t *parameters,
    const orm_sql_expr_query_sources *queries,orm_sql_lateral_query *out,turbodb_error_t *error) {
  if(!scope||!scope->budget||!owner||scope->budget!=owner->budget||!out||out->budget||
      (scope->parameter_count&&(!parameters||!scope->parameter_types))||
      (scope->outer_schema&&scope->outer_schema->count&&!scope->outer_schema->columns)||
      (queries&&queries->count&&!queries->items))
    return runtime_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid LATERAL query owner");
  const size_t captures=scope->outer_schema?scope->outer_schema->count:0;
  if(captures>SIZE_MAX-scope->parameter_count||captures+scope->parameter_count>
      scope->budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
    return runtime_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"LATERAL query argument capacity exceeded");
  *out=(orm_sql_lateral_query){.budget=scope->budget,.marker_count=scope->parameter_count,
      .capture_count=captures,.queries=queries?*queries:(orm_sql_expr_query_sources){0}};
  turbodb_status_t status=orm_tidesdb_sql_budget_reserve_capacity(out->budget,1,sizeof(*out),0,&out->metadata_bytes,error);
  if(status==TURBODB_STATUS_OK) status=scope->parameter_count?
      orm_sql_snapshot_copy(&out->arguments,parameters,scope->parameter_count,captures,out->budget,error):
      orm_sql_work_zero(&out->arguments.values,captures,sizeof(turbodb_value_t),_Alignof(turbodb_value_t),
          out->budget,&out->arguments.bytes,error);
  orm_sql_query_scope child=*scope; child.defer_execution=true;
  if(status==TURBODB_STATUS_OK) status=orm_sql_runtime_scope_open(&child,owner,parameters,false,
      queries,&out->query,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&out->types,out->query.columns,sizeof(orm_sql_type),
      _Alignof(orm_sql_type),out->budget,&out->type_bytes,error);
  orm_sql_budget_amount charge={0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=out->query.columns;
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_budget_reserve(out->budget,&charge,error);
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<out->query.columns;++i) {
    orm_sql_schema_column column;
    status=orm_tidesdb_sql_runtime_column(&out->query,i,&column,error);
    if(status==TURBODB_STATUS_OK) *(orm_sql_type *)vec_at(&out->types,i)=column.type;
  }
  if(status==TURBODB_STATUS_OK) {
    out->source=(orm_sql_row_source){out->budget,vec_data_const(&out->types),out->query.columns,out,lateral_query_pull,false};
    out->binding=(orm_sql_join_right_binding){out,lateral_query_round_open,lateral_query_round_close};
  } else {
    const turbodb_status_t released=orm_sql_lateral_query_close(out,NULL);
    if(released!=TURBODB_STATUS_OK) return released;
  }
  return status;
}
static turbodb_status_t runtime_explain_scan(orm_sql_query *query, turbodb_error_t *error) {
  size_t projection[ORM_SQL_EXPLAIN_COLUMNS];
  for (size_t i = 0; i < ORM_SQL_EXPLAIN_COLUMNS; ++i) projection[i] = i;
  const orm_sql_scan_spec spec = {.projection=projection,.projection_count=ORM_SQL_EXPLAIN_COLUMNS,.limit=UINT64_MAX};
  return orm_tidesdb_sql_scan_open_source(&query->as.select.explain.source.source,&spec,
      query->owner->budget,&query->as.select.explain.scan,error);
}
static turbodb_status_t runtime_from_open(orm_sql_query *query,const turbodb_value_t *parameters,size_t count,
    const orm_sql_expr_query_sources *queries,turbodb_error_t *error) {
  return query->as.select.from.contains_lateral?
      orm_sql_from_open_dependent(&query->as.select.from,vec_data_const(&query->as.select.dependent_inputs),
          vec_size(&query->as.select.dependent_inputs),parameters,count,queries,&query->as.select.from_run,error):
      orm_tidesdb_sql_from_open_queries(&query->as.select.from,vec_data_const(&query->as.select.inputs),
          vec_size(&query->as.select.inputs),parameters,count,queries,&query->as.select.from_run,error);
}
static turbodb_status_t runtime_joined(const orm_sql_query_scope *scope, const turbodb_value_t *parameters,
    const orm_sql_expr_query_sources *queries, orm_sql_query *query, turbodb_error_t *error) {
  orm_tidesdb_sql_budget *budget = query->owner->budget;
  vec_t names = {0}, schemas = {0};
  size_t name_bytes = 0, schema_bytes = 0;
  turbodb_status_t status=TURBODB_STATUS_OK;
  if(query->as.select.from.budget)
    status=orm_tidesdb_sql_from_bind_conditions_at(scope,&query->as.select.from,error);
  else {
    bool metadata=query->kind==ORM_SQL_QUERY_EXPLAIN;
    const sqlparser_id subtree=sqlparser_get_node(scope->document,scope->root)->as.select.from;
    status=orm_sql_from_subtree_tables_at(scope,subtree,&names,&name_bytes,error);
    bool lateral=false;
    if(status==TURBODB_STATUS_OK) status=runtime_lateral_names(scope,&names,&lateral,error);
    if(lateral) metadata=true;
    if(status==TURBODB_STATUS_OK) status=runtime_join_inputs(scope,&names,query,&schemas,&schema_bytes,error);
    if(status==TURBODB_STATUS_OK) status=metadata?
        orm_sql_from_subtree_schema_at(scope,subtree,vec_data_const(&schemas),vec_size(&schemas),&query->as.select.from,error):
        orm_tidesdb_sql_from_bind_at(scope,vec_data_const(&schemas),vec_size(&schemas),&query->as.select.from,error);
    if(status==TURBODB_STATUS_OK&&metadata)
      status=orm_tidesdb_sql_from_bind_conditions_at(scope,&query->as.select.from,error);
  }
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_select_bind_at(scope,
      &orm_tidesdb_sql_from_at(&query->as.select.from,0)->schema,&query->as.select.from,&query->as.select.plan,error);
  if(status==TURBODB_STATUS_OK&&scope->defer_execution) query->execution_closed=true;
  else if (status == TURBODB_STATUS_OK && query->kind == ORM_SQL_QUERY_EXPLAIN) {
    status = orm_tidesdb_sql_explain_open_from(&query->as.select.plan,&query->as.select.from,
        parameters,vec_size(&query->as.select.plan.parameter_types),&query->as.select.explain.source,error);
    if (status == TURBODB_STATUS_OK) status = runtime_explain_scan(query,error);
  } else if (status == TURBODB_STATUS_OK) {
    status = runtime_from_open(query,parameters,vec_size(&query->as.select.plan.parameter_types),queries,error);
    if (status == TURBODB_STATUS_OK) status = orm_sql_select_open_demand(&query->as.select.plan,
        query->as.select.from_run.source,parameters,scope->parameter_count,queries,scope->demand,&query->as.select.run,error);
  }
  vec_t *vectors[] = {&names,&schemas};
  const size_t bytes[] = {name_bytes,schema_bytes};
  for (size_t i = 0; i < sizeof(vectors)/sizeof(vectors[0]); ++i) {
    const turbodb_status_t released = orm_sql_work_release(vectors[i],bytes[i],budget,status == TURBODB_STATUS_OK ? error : NULL);
    if (released != TURBODB_STATUS_OK) query->owner->failed = true;
    if (status == TURBODB_STATUS_OK) status = released;
  }
  return status;
}
static turbodb_status_t runtime_unit_next(void *context, const turbodb_value_t **out, turbodb_error_t *error) {
  orm_sql_query *query = context;
  turbodb_status_t status = orm_sql_store_ready(query->owner,error);
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_reserve(query->owner->budget,&amount,error);
  if (status != TURBODB_STATUS_OK) return status;
  *out = query->as.select.unit.done ? NULL : &query->as.select.unit.value;
  query->as.select.unit.done = true; return TURBODB_STATUS_OK;
}
static turbodb_status_t runtime_unit(const orm_sql_query_scope *scope, const turbodb_value_t *parameters,
    const orm_sql_expr_query_sources *queries, orm_sql_query *query, turbodb_error_t *error) {
  if (query->owner->active_sources == SIZE_MAX)
    return runtime_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"SQL unit source count overflow");
  query->as.select.unit.type = (orm_sql_type){TURBODB_VALUE_BOOLEAN,false};
  query->as.select.unit.value = turbodb_bool(true);
  query->as.select.unit.source = (orm_sql_row_source){query->owner->budget,&query->as.select.unit.type,1,
      query,runtime_unit_next,false};
  ++query->owner->active_sources;
  const orm_sql_schema_column column = {vstr_from_cstr("unit"),query->as.select.unit.type};
  const orm_sql_table_schema schema = {vstr_from_cstr("unit"),&column,1};
  orm_sql_query_scope input = *scope; input.unit_input = true;
  turbodb_status_t status = orm_tidesdb_sql_select_bind_at(&input,&schema,NULL,&query->as.select.plan,error);
  if(status==TURBODB_STATUS_OK&&scope->defer_execution) { query->execution_closed=true; return TURBODB_STATUS_OK; }
  if (status == TURBODB_STATUS_OK && query->kind == ORM_SQL_QUERY_EXPLAIN) {
    status = orm_tidesdb_sql_explain_open(&query->as.select.plan,(vstr){0},parameters,vec_size(&query->as.select.plan.parameter_types),
        &query->as.select.explain.source,error);
    if (status == TURBODB_STATUS_OK) status = runtime_explain_scan(query,error);
  } else if (status == TURBODB_STATUS_OK) status = orm_sql_select_open_demand(&query->as.select.plan,
      &query->as.select.unit.source,parameters,scope->parameter_count,queries,scope->demand,&query->as.select.run,error);
  return status;
}
static turbodb_status_t runtime_select(const orm_sql_query_scope *scope, const sqlparser_node *statement,
    const turbodb_value_t *parameters, const orm_sql_expr_query_sources *queries, orm_sql_query *query, turbodb_error_t *error) {
  if (!statement->as.select.from) return runtime_unit(scope,parameters,queries,query,error);
  const sqlparser_node *from = sqlparser_get_node(scope->document,statement->as.select.from);
  if (from && (from->kind == SQLPARSER_JOIN || (from->kind == SQLPARSER_TABLE && (from->as.table.query || scope->derived_count))))
    return runtime_joined(scope,parameters,queries,query,error);
  vstr table = {0};
  turbodb_status_t status = runtime_table(scope->document,statement,&table,error);
  if (status == TURBODB_STATUS_OK && !query->as.select.source.owner)
    status = orm_tidesdb_sql_relation_open(query->owner,table,&query->as.select.source,error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_select_bind_at(scope,&query->as.select.source.schema,NULL,&query->as.select.plan,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_index_lookup_plan(scope,&query->as.select.plan,&query->as.select.source,error);
  if (status == TURBODB_STATUS_OK && query->as.select.source.lookup.budget && !scope->defer_execution) {
    uint64_t offset = 0, limit = 0;
    status = orm_tidesdb_sql_select_validate_parameters(&query->as.select.plan,parameters,scope->parameter_count,&offset,&limit,error);
    if (status == TURBODB_STATUS_OK) status = orm_sql_index_lookup_bind(&query->as.select.source.lookup,parameters,scope->parameter_count,error);
  }
  if(status==TURBODB_STATUS_OK&&scope->defer_execution) query->execution_closed=true;
  else if (status == TURBODB_STATUS_OK && query->kind == ORM_SQL_QUERY_EXPLAIN) {
    const char *reason = NULL;
    if (from->as.table.alias) status = orm_sql_name_node(scope->document,from->as.table.alias,&table,&reason);
    if (status != TURBODB_STATUS_OK) tdsql_error_set(error,status,reason);
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_explain_open(&query->as.select.plan,table,parameters,vec_size(&query->as.select.plan.parameter_types),
        &query->as.select.explain.source,error);
    if (status == TURBODB_STATUS_OK && query->as.select.source.lookup.budget) {
      const orm_sql_index_lookup *lookup = &query->as.select.source.lookup;
      status = orm_sql_explain_index(&query->as.select.explain.source, &lookup->record.definition,
          lookup->bound_bytes, lookup->access, lookup->contains_null, error);
    }
    if (status == TURBODB_STATUS_OK) status = runtime_explain_scan(query,error);
  } else if (status == TURBODB_STATUS_OK) status = orm_sql_select_open_demand(&query->as.select.plan,
      &query->as.select.source.source,parameters,scope->parameter_count,queries,scope->demand,&query->as.select.run,error);
  return status;
}
const orm_sql_select *orm_sql_runtime_plan(const orm_sql_query *query) {
  if (!query || !query->owner) return NULL;
  return query->kind == ORM_SQL_QUERY_COMPOUND ? query->as.compound.plan :
      query->kind == ORM_SQL_QUERY_SELECT || query->kind == ORM_SQL_QUERY_EXPLAIN ? &query->as.select.plan : NULL;
}
static sqlparser_id runtime_body(const sqlparser_document *document, sqlparser_id root) {
  const sqlparser_node *node = sqlparser_get_node(document,root);
  return node && node->kind == SQLPARSER_WITH ? node->as.with.body : root;
}
static turbodb_status_t runtime_build(const orm_sql_query_scope *scope, const turbodb_value_t *parameters,
    bool explain, const orm_sql_expr_query_sources *sources, orm_sql_query *query, turbodb_error_t *error) {
  query->demand = scope->demand;
  orm_sql_query_scope body = *scope; body.root = runtime_body(scope->document,scope->root); scope = &body;
  const sqlparser_node *statement = sqlparser_get_node(scope->document,scope->root);
  if (!statement || (statement->kind != SQLPARSER_SELECT && statement->kind != SQLPARSER_UNION && statement->kind != SQLPARSER_QUERY_GROUP))
    return runtime_error(error,TURBODB_STATUS_UNSUPPORTED,"runtime block requires SELECT UNION or query group");
  if (scope->in_query && statement->kind == SQLPARSER_SELECT && statement->as.select.limit)
    return runtime_error(error,TURBODB_STATUS_UNSUPPORTED,"LIMIT in IN subqueries is not supported");
  turbodb_status_t status = statement->kind == SQLPARSER_SELECT ? runtime_select(scope,statement,parameters,sources,query,error) :
      orm_sql_compound_open_queries(scope,query->owner,parameters,explain,sources,&query->as.compound,error);
  if(status==TURBODB_STATUS_OK&&statement->kind!=SQLPARSER_SELECT&&scope->defer_execution)
    query->execution_closed=true;
  if (status == TURBODB_STATUS_OK) query->columns = explain ? ORM_SQL_EXPLAIN_COLUMNS : vec_size(&orm_sql_runtime_plan(query)->columns);
  return status;
}
turbodb_status_t orm_sql_runtime_scope_open(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters, bool explain,
    const orm_sql_expr_query_sources *sources, orm_sql_query *out, turbodb_error_t *error) {
  if (!scope || !scope->document || !scope->root || !owner || scope->budget != owner->budget || !out || out->owner ||
      out->kind != ORM_SQL_QUERY_CLOSED || !scope->max_depth ||
      (scope->binding_only&&(!scope->defer_execution||explain)) ||
      (scope->parameter_count && (!scope->parameter_types ||
          (!parameters && (!scope->defer_execution || explain)))))
    return runtime_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid query block inputs");
  orm_sql_query_scope body = *scope; body.root = runtime_body(scope->document,scope->root); scope = &body;
  const sqlparser_node *statement = sqlparser_get_node(scope->document,scope->root);
  if (!statement || (statement->kind != SQLPARSER_SELECT && statement->kind != SQLPARSER_UNION && statement->kind != SQLPARSER_QUERY_GROUP))
    return runtime_error(error,TURBODB_STATUS_UNSUPPORTED,"runtime block requires SELECT UNION or query group");
  turbodb_status_t status = orm_sql_store_ready(owner,error); size_t bytes = 0;
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_reserve_capacity(owner->budget,1,sizeof(*out),0,&bytes,error);
  if (status != TURBODB_STATUS_OK) return status;
  *out = (orm_sql_query){.owner=owner,.metadata_bytes=bytes,.kind=statement->kind != SQLPARSER_SELECT ?
      ORM_SQL_QUERY_COMPOUND : explain ? ORM_SQL_QUERY_EXPLAIN : ORM_SQL_QUERY_SELECT,
      .evaluation=scope->evaluation,.execution_closed=scope->binding_only};
  memset(&out->as,0,sizeof(out->as));
  const orm_sql_expr_query_sources execution = sources ? *sources :
      (orm_sql_expr_query_sources){.evaluation=scope->evaluation};
  status = runtime_build(scope,parameters,explain,&execution,out,error);
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t released = orm_tidesdb_sql_runtime_close(out,NULL);
    if (released != TURBODB_STATUS_OK) return released;
  }
  return status;
}
turbodb_status_t orm_tidesdb_sql_runtime_block_open(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters, bool explain, orm_sql_query *out, turbodb_error_t *error) {
  const sqlparser_node *node = scope && scope->document ? sqlparser_get_node(scope->document,scope->root) : NULL;
  if (!node || node->kind != SQLPARSER_SELECT) return runtime_error(error,TURBODB_STATUS_UNSUPPORTED,"runtime block requires SELECT");
  return orm_sql_runtime_scope_open(scope,owner,parameters,explain,NULL,out,error);
}
static turbodb_status_t runtime_query_construct(const orm_sql_query_scope *input,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters,
    bool binding_only,orm_sql_query *out, turbodb_error_t *error) {
  if (!input || !input->document || !input->root || !owner ||
      input->budget != owner->budget || !input->max_depth || !out || out->owner ||
      out->kind != ORM_SQL_QUERY_CLOSED ||
      (input->parameter_count && (!input->parameter_types || (!parameters&&!binding_only))))
    return runtime_error(error,TURBODB_STATUS_INVALID_ARGUMENT,
        "invalid embedded SQL query inputs");
  if (sqlparser_get_dialect(input->document) != SQLPARSER_MYSQL)
    return runtime_error(error,TURBODB_STATUS_UNSUPPORTED,
        "embedded SQL query requires MySQL dialect");
  orm_sql_query_scope scope = *input;
  if(binding_only) { scope.defer_execution=true; scope.binding_only=true; }
  const sqlparser_node *statement = sqlparser_get_node(scope.document,
      runtime_body(scope.document,scope.root));
  const bool compound = statement && (statement->kind == SQLPARSER_UNION ||
      statement->kind == SQLPARSER_QUERY_GROUP);
  if (!statement || (statement->kind != SQLPARSER_SELECT && !compound))
    return runtime_error(error,TURBODB_STATUS_UNSUPPORTED,
        "embedded SQL query requires SELECT UNION or query group");
  turbodb_status_t status = orm_sql_store_ready(owner,error); size_t bytes = 0;
  if (status == TURBODB_STATUS_OK)
    status = orm_tidesdb_sql_budget_reserve_capacity(owner->budget,1,
        sizeof(*out),0,&bytes,error);
  if (status != TURBODB_STATUS_OK) return status;
  *out = (orm_sql_query){.owner=owner,.metadata_bytes=bytes,
      .kind=compound ? ORM_SQL_QUERY_COMPOUND : ORM_SQL_QUERY_SELECT,.evaluation=scope.evaluation,
      .execution_closed=binding_only};
  memset(&out->as,0,sizeof(out->as));
  scope.prepare_dependency_schema=runtime_dependency_schema_prepare;
  scope.dependency_schema_context=out;
  status=runtime_prepare_dependency_schema(&scope,out,false,error);
  if(status==TURBODB_STATUS_OK) status = binding_only ?
      orm_sql_dependencies_bind(&scope,owner,&out->dependencies,error) : scope.max_iterations ?
      orm_sql_dependencies_open_recursive(&scope,owner,parameters,false,
          scope.max_iterations,&out->dependencies,error) :
      orm_sql_dependencies_open(&scope,owner,parameters,false,
          &out->dependencies,error);
  scope.queries = vec_data_const(&out->dependencies.bindings);
  scope.query_count = out->dependencies.query_count;
  scope.derived = vec_data_const(&out->dependencies.derived);
  scope.derived_count = out->dependencies.derived_count;
  const orm_sql_expr_query_sources sources = {
      vec_data_const(&out->dependencies.sources),out->dependencies.query_count,scope.evaluation};
  if (status == TURBODB_STATUS_OK)
    status = runtime_build(&scope,parameters,false,&sources,out,error);
  if (status == TURBODB_STATUS_OK && scope.parameter_count&&!binding_only)
    status = orm_sql_snapshot_copy(&out->parameters,parameters,
        scope.parameter_count,0,owner->budget,error);
  if (status == TURBODB_STATUS_OK) {
    out->parameter_count = scope.parameter_count;
    out->statement_parameters = !binding_only;
  } else {
    const turbodb_status_t cleanup = orm_tidesdb_sql_runtime_close(out,NULL);
    if (cleanup != TURBODB_STATUS_OK)
      return runtime_error(error,cleanup,
          "embedded SQL query cleanup failed; owner requires rollback");
  }
  return status;
}
turbodb_status_t orm_sql_runtime_query_open(const orm_sql_query_scope *input,
    orm_sql_catalog_store *owner,const turbodb_value_t *parameters,
    orm_sql_query *out,turbodb_error_t *error) {
  return runtime_query_construct(input,owner,parameters,false,out,error);
}
turbodb_status_t orm_sql_runtime_query_bind(const orm_sql_query_scope *input,
    orm_sql_catalog_store *owner,orm_sql_query *out,turbodb_error_t *error) {
  if (!input || !owner || input->budget != owner->budget || !out || out->owner ||
      out->kind != ORM_SQL_QUERY_CLOSED)
    return runtime_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid type-only query output or owner");
  if (input->query_count || input->derived_count || input->outer_schema ||
      input->dependency_schema || input->demand != ORM_SQL_QUERY_VALUES)
    return runtime_error(error,TURBODB_STATUS_UNSUPPORTED,"type-only query requires an independent value-producing scope (#206)");
  turbodb_status_t status = orm_sql_store_ready(owner,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_bind_statement_scope(input,error);
  return status==TURBODB_STATUS_OK?runtime_query_construct(input,owner,NULL,true,out,error):status;
}
turbodb_status_t orm_sql_runtime_explain_ids(orm_sql_query *query, orm_sql_explain_block block, int64_t *next_id, turbodb_error_t *error) {
  if (query->kind == ORM_SQL_QUERY_COMPOUND) return orm_sql_compound_explain_ids(&query->as.compound,block,next_id,error);
  if (query->kind != ORM_SQL_QUERY_EXPLAIN || !next_id || *next_id < 0)
    return runtime_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"EXPLAIN query and identifier required");
  if (*next_id == INT64_MAX) return runtime_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"EXPLAIN identifier overflow");
  const turbodb_status_t status = orm_tidesdb_sql_explain_block(&query->as.select.explain.source,
      block,*next_id+1,error);
  if (status == TURBODB_STATUS_OK) ++*next_id;
  return status;
}
static turbodb_status_t runtime_show(const sqlparser_document *document, vstr database_name,
    const turbodb_value_t *parameters,const orm_sql_type *types,size_t count,size_t max_depth,
    orm_sql_evaluation evaluation, orm_sql_query *query,
    turbodb_error_t *error) {
  const sqlparser_node *statement=sqlparser_get_node(document,sqlparser_statements(document).first);
  if (!statement->as.show.where && count)
    return runtime_error(error,TURBODB_STATUS_SQL_ERROR,"SQL SHOW parameter count does not match markers");
  turbodb_status_t status = orm_sql_show_open_evaluation(document,
      query->owner,database_name,evaluation,&query->as.show.source,error);
  if (status != TURBODB_STATUS_OK) return status;
  vec_t offsets={0}; size_t offset_bytes=0;
  if (statement->as.show.where) {
    status=orm_sql_bind_parameter_offsets(document,count,query->owner->budget,&offsets,&offset_bytes,error);
    const orm_sql_binding_scope scope={.document=document,.schema=&query->as.show.source.schema,
        .parameter_types=types,.parameter_offsets=vec_data_const(&offsets),.parameter_count=count,
        .budget=query->owner->budget,.ascii_insensitive_names=true};
    if (status==TURBODB_STATUS_OK) status=orm_sql_bind_expression(&scope,statement->as.show.where,max_depth,
        (orm_sql_expression_target){.program=&query->as.show.filter,
            .slots=&query->as.show.filter_slots,.slot_bytes=&query->as.show.filter_bytes},true,error);
  }
  const turbodb_status_t released=orm_sql_work_release(&offsets,offset_bytes,query->owner->budget,status==TURBODB_STATUS_OK?error:NULL);
  if (released!=TURBODB_STATUS_OK) query->owner->failed=true;
  if (status==TURBODB_STATUS_OK) status=released;
  if (status!=TURBODB_STATUS_OK) return status;
  size_t projection[ORM_SQL_SHOW_MAX_COLUMNS];
  query->columns = query->as.show.source.schema.count;
  for (size_t i = 0; i < query->columns; ++i) projection[i] = i;
  const orm_sql_scan_spec spec = {.projection = projection,
      .projection_count = query->columns,
      .filter=query->as.show.filter.budget?&query->as.show.filter:NULL,
      .filter_slots=vec_data_const(&query->as.show.filter_slots),.filter_count=vec_size(&query->as.show.filter_slots),
      .parameters=parameters,.parameter_types=types,.parameter_count=count,.evaluation=evaluation,
      .offset=query->as.show.source.offset,.limit=query->as.show.source.limit};
  return orm_tidesdb_sql_scan_open_source(&query->as.show.source.source, &spec, query->owner->budget, &query->as.show.scan, error);
}
turbodb_status_t orm_sql_runtime_open_evaluation(const sqlparser_document *document,
    orm_sql_catalog_store *owner, vstr database_name, const turbodb_value_t *parameters,
    size_t count, size_t max_depth, uint64_t max_iterations,
    orm_sql_evaluation evaluation, orm_sql_query *out,
    turbodb_error_t *error) {
  if (!out || out->owner || out->kind != ORM_SQL_QUERY_CLOSED)
    return runtime_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "empty SQL runtime query required");
  if(evaluation.mode<ORM_SQL_EVALUATION_QUERY||evaluation.mode>ORM_SQL_EVALUATION_IGNORE_WRITE||
      (evaluation.diagnostics&&!evaluation.diagnostics->max_records))
    return runtime_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL statement evaluation context");
  const sqlparser_node *statement = NULL;
  turbodb_status_t status = runtime_statement(document, owner, parameters, count, max_depth, &statement, error);
  if (status != TURBODB_STATUS_OK) return status;
  sqlparser_id root = sqlparser_statements(document).first;
  const bool explain = statement->kind == SQLPARSER_EXPLAIN;
  if (explain) {
    if (statement->as.explain.query_plan || (statement->as.explain.format != SQLPARSER_EXPLAIN_DEFAULT &&
        statement->as.explain.format != SQLPARSER_EXPLAIN_TRADITIONAL))
      return runtime_error(error,TURBODB_STATUS_UNSUPPORTED,"EXPLAIN requires default or TRADITIONAL format");
    root = statement->as.explain.statement;
    statement = sqlparser_get_node(document,root);
    if (!statement || (statement->kind != SQLPARSER_SELECT && statement->kind != SQLPARSER_UNION && statement->kind != SQLPARSER_QUERY_GROUP && statement->kind != SQLPARSER_WITH))
      return runtime_error(error,TURBODB_STATUS_UNSUPPORTED,"EXPLAIN supports SELECT only");
  }
  statement = sqlparser_get_node(document,runtime_body(document,root));
  const bool compound = statement->kind == SQLPARSER_UNION || statement->kind == SQLPARSER_QUERY_GROUP;
  if (statement->kind != SQLPARSER_SELECT && statement->kind != SQLPARSER_SHOW && !compound)
    return runtime_error(error, TURBODB_STATUS_UNSUPPORTED, "SQL runtime query requires SELECT, SHOW or EXPLAIN SELECT");
  size_t bytes = 0;
  status = orm_tidesdb_sql_budget_reserve_capacity(owner->budget, 1, sizeof(*out), 0, &bytes, error);
  if (status != TURBODB_STATUS_OK) return status;
  /* Embedded plans and sources retain addresses. Build in the final storage;
   * failed construction is private to this synchronous call and fully closed. */
  *out = (orm_sql_query){.owner = owner, .metadata_bytes = bytes,
      .kind = compound ? ORM_SQL_QUERY_COMPOUND : explain ? ORM_SQL_QUERY_EXPLAIN : statement->kind == SQLPARSER_SELECT ? ORM_SQL_QUERY_SELECT : ORM_SQL_QUERY_SHOW,
      .evaluation=evaluation};
  /* Aggregate initialization only initializes the first union member; larger
   * query variants must also start with empty embedded owners. */
  memset(&out->as,0,sizeof(out->as));
  vec_t types = {0}; size_t type_bytes = 0;
  if (out->kind == ORM_SQL_QUERY_SHOW) {
    status=runtime_parameter_types(parameters,count,owner->budget,&types,&type_bytes,error);
    if(status==TURBODB_STATUS_OK) status=runtime_show(document,database_name,parameters,
        vec_data_const(&types),count,max_depth,evaluation,out,error);
  }
  else {
    status = runtime_parameter_types(parameters,count,owner->budget,&types,&type_bytes,error);
    orm_sql_query_scope scope = {.document=document,.root=root,
        .parameter_types=vec_data_const(&types),.parameter_count=count,
        .max_depth=max_depth,.budget=owner->budget,
        .max_iterations=max_iterations,
        .prepare_dependency_schema=explain?NULL:runtime_dependency_schema_prepare,
        .dependency_schema_context=explain?NULL:out,.evaluation=evaluation};
    if(status==TURBODB_STATUS_OK&&!explain)
      status=runtime_prepare_dependency_schema(&scope,out,false,error);
    if (status == TURBODB_STATUS_OK) status = max_iterations ?
        orm_sql_dependencies_open_recursive(&scope,owner,parameters,explain,max_iterations,&out->dependencies,error) :
        orm_sql_dependencies_open(&scope,owner,parameters,explain,&out->dependencies,error);
    scope.queries = vec_data_const(&out->dependencies.bindings); scope.query_count = out->dependencies.query_count;
    scope.derived = vec_data_const(&out->dependencies.derived); scope.derived_count = out->dependencies.derived_count;
    const orm_sql_expr_query_sources sources = {vec_data_const(&out->dependencies.sources),out->dependencies.query_count,evaluation};
    if (status == TURBODB_STATUS_OK) status = runtime_build(&scope,parameters,explain,&sources,out,error);
    if (status == TURBODB_STATUS_OK && explain) status = orm_sql_dependencies_explain(out,error);
  }
  if (status == TURBODB_STATUS_OK && !explain && out->kind != ORM_SQL_QUERY_SHOW) {
    if (count) status = orm_sql_snapshot_copy(&out->parameters,parameters,count,0,owner->budget,error);
    if (status == TURBODB_STATUS_OK) { out->parameter_count = count; out->statement_parameters = true; }
  }
  const turbodb_status_t released = orm_sql_work_release(&types,type_bytes,owner->budget,status == TURBODB_STATUS_OK ? error : NULL);
  if (released != TURBODB_STATUS_OK) owner->failed = true;
  if (status == TURBODB_STATUS_OK) status = released;
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t cleanup = orm_tidesdb_sql_runtime_close(out, NULL);
    if (cleanup != TURBODB_STATUS_OK) return runtime_error(error, cleanup, "SQL runtime open cleanup failed; owner requires rollback");
  }
  return status;
}
turbodb_status_t orm_tidesdb_sql_runtime_open(const sqlparser_document *document,
    orm_sql_catalog_store *owner, vstr database_name, const turbodb_value_t *parameters,
    size_t count, size_t max_depth, orm_sql_query *out, turbodb_error_t *error) {
  return orm_sql_runtime_open_evaluation(document,owner,database_name,parameters,count,max_depth,0,
      (orm_sql_evaluation){0},out,error);
}
turbodb_status_t orm_tidesdb_sql_runtime_open_diagnostics(
    const sqlparser_document *document, orm_sql_catalog_store *owner,
    vstr database_name, const turbodb_value_t *parameters, size_t count,
    size_t max_depth, orm_sql_diagnostics *diagnostics,
    orm_sql_query *out, turbodb_error_t *error) {
  return orm_sql_runtime_open_evaluation(document,owner,database_name,parameters,count,max_depth,0,
      (orm_sql_evaluation){.diagnostics=diagnostics},out,error);
}
turbodb_status_t orm_sql_runtime_open_recursive(const sqlparser_document *document,
    orm_sql_catalog_store *owner, vstr database_name, const turbodb_value_t *parameters,
    size_t count, size_t max_depth, uint64_t max_iterations, orm_sql_query *out, turbodb_error_t *error) {
  if (!max_iterations) return runtime_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"recursive query iteration limit must be positive");
  return orm_sql_runtime_open_evaluation(document,owner,database_name,parameters,count,max_depth,
      max_iterations,(orm_sql_evaluation){0},out,error);
}
turbodb_status_t orm_tidesdb_sql_runtime_column(const orm_sql_query *query,
    size_t ordinal, orm_sql_schema_column *out, turbodb_error_t *error) {
  if (!query || !out) return runtime_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "SQL runtime metadata requires query and output");
  if (!query->owner) return runtime_error(error, TURBODB_STATUS_INVALID_STATE, "SQL runtime query is closed");
  if (ordinal >= query->columns) return runtime_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "SQL runtime column is out of range");
  if (query->dependencies.explained.budget) *out = query->dependencies.columns[ordinal];
  else if (query->kind == ORM_SQL_QUERY_COMPOUND && query->as.compound.describe)
    *out = query->as.compound.columns[ordinal];
  else if (query->kind == ORM_SQL_QUERY_SELECT || query->kind == ORM_SQL_QUERY_COMPOUND) {
    const orm_sql_select *plan = query->kind == ORM_SQL_QUERY_COMPOUND ? query->as.compound.plan : &query->as.select.plan;
    const orm_sql_select_column *column = orm_tidesdb_sql_select_column_at(plan, ordinal);
    *out = (orm_sql_schema_column){vstr_from_cstr(column->name), column->type};
  } else if (query->kind == ORM_SQL_QUERY_EXPLAIN) *out = query->as.select.explain.source.columns[ordinal];
  else *out = query->as.show.source.schema.columns[ordinal];
  return TURBODB_STATUS_OK;
}
orm_sql_scan *orm_sql_runtime_base_scan(orm_sql_query *query) {
  if (!query || !query->owner || query->execution_closed) return NULL;
  return query->kind == ORM_SQL_QUERY_COMPOUND ? query->as.compound.scan : query->kind == ORM_SQL_QUERY_SELECT ? &query->as.select.run.scan :
      query->kind == ORM_SQL_QUERY_EXPLAIN ? &query->as.select.explain.scan : &query->as.show.scan;
}
static orm_sql_scan *runtime_scan(orm_sql_query *query) {
  if (query && query->dependencies.explained.budget) return &query->dependencies.explained;
  return orm_sql_runtime_base_scan(query);
}
static turbodb_status_t runtime_execution_check(orm_sql_query *query, turbodb_error_t *error) {
  if (!query || !query->owner) return runtime_error(error,TURBODB_STATUS_INVALID_STATE,"SQL query is closed");
  if ((query->kind != ORM_SQL_QUERY_SELECT && query->kind != ORM_SQL_QUERY_COMPOUND) ||
      (query->kind == ORM_SQL_QUERY_COMPOUND && query->as.compound.describe))
    return runtime_error(error,TURBODB_STATUS_UNSUPPORTED,"execution reuse requires a SELECT query");
  orm_sql_scan *scan = orm_sql_runtime_base_scan(query);
  return scan && scan->evaluating ? runtime_error(error,TURBODB_STATUS_BUSY,"SQL query is evaluating") : TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_runtime_execution_close(orm_sql_query *query, turbodb_error_t *error) {
  turbodb_status_t status = runtime_execution_check(query,error);
  if (status != TURBODB_STATUS_OK || query->execution_closed) return status;
  const orm_sql_scan *scan = orm_sql_runtime_base_scan(query);
  if (scan && scan->failure.status != TURBODB_STATUS_OK && query->execution_failure.status == TURBODB_STATUS_OK)
    query->execution_failure = scan->failure;
  if (query->kind == ORM_SQL_QUERY_COMPOUND) status = orm_sql_compound_execution_close(&query->as.compound,error);
  else {
    status = orm_tidesdb_sql_select_close(&query->as.select.run,error);
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_from_close(&query->as.select.from_run,error);
  }
  if (status == TURBODB_STATUS_OK) status = orm_sql_dependencies_execution_close(&query->dependencies,error);
  if (status == TURBODB_STATUS_OK) query->execution_closed = true;
  return status;
}
static turbodb_status_t runtime_execution_select_open(orm_sql_query *query, const turbodb_value_t *parameters,
    size_t count, const orm_sql_expr_query_sources *sources, turbodb_error_t *error) {
  turbodb_status_t status = TURBODB_STATUS_OK;
  orm_sql_row_source *input = NULL;
  if (query->as.select.from.budget) {
    orm_sql_budget_amount charge = {0};
    charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = vec_size(&query->as.select.relations);
    status = orm_tidesdb_sql_budget_reserve(query->owner->budget,&charge,error);
    for (size_t i = 0; status == TURBODB_STATUS_OK && !query->as.select.from.contains_lateral&&i < vec_size(&query->as.select.relations); ++i) {
      orm_sql_relation_source *relation = vec_at(&query->as.select.relations,i);
      if (relation->owner) status = orm_sql_relation_rewind(relation,error);
    }
    if (status == TURBODB_STATUS_OK) status = runtime_from_open(query,parameters,count,sources,error);
    input = query->as.select.from_run.source;
  } else if (query->as.select.unit.source.budget) {
    query->as.select.unit.done = false; input = &query->as.select.unit.source;
  } else {
    status = orm_sql_relation_rewind(&query->as.select.source,error); input = &query->as.select.source.source;
    if (status == TURBODB_STATUS_OK && query->as.select.source.lookup.budget) {
      uint64_t offset = 0, limit = 0;
      status = orm_tidesdb_sql_select_validate_parameters(&query->as.select.plan,parameters,count,&offset,&limit,error);
      if (status == TURBODB_STATUS_OK) status = orm_sql_index_lookup_bind(&query->as.select.source.lookup,parameters,count,error);
    }
  }
  if (status == TURBODB_STATUS_OK) status = orm_sql_select_open_demand(&query->as.select.plan,input,
      parameters,count,sources,query->demand,&query->as.select.run,error);
  return status;
}
static turbodb_status_t runtime_execution_open(orm_sql_query *query, const turbodb_value_t *parameters,
    size_t count, const orm_sql_expr_query_sources *sources, turbodb_error_t *error) {
  turbodb_status_t status = runtime_execution_check(query,error);
  if (status != TURBODB_STATUS_OK) return status;
  if(query->kind==ORM_SQL_QUERY_SELECT && !query->as.select.plan.budget)
    return runtime_error(error,TURBODB_STATUS_INVALID_STATE,"metadata-only FROM query is not executable");
  if (!query->execution_closed) return runtime_error(error,TURBODB_STATUS_BUSY,"close query execution before reopening");
  if (query->execution_failure.status != TURBODB_STATUS_OK)
    return runtime_error(error,query->execution_failure.status,query->execution_failure.message);
  if (count && !parameters) return runtime_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"query parameters are missing");
  turbodb_error_t cause; tdsql_error_init(&cause);
  status = orm_sql_store_ready(query->owner,&cause);
  orm_sql_budget_amount charge = {0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_reserve(query->owner->budget,&charge,&cause);
  query->execution_closed = false;
  if (status == TURBODB_STATUS_OK) status = orm_sql_dependencies_execution_open(&query->dependencies,parameters,count,&cause);
  if (status == TURBODB_STATUS_OK) status = query->kind == ORM_SQL_QUERY_COMPOUND ?
      orm_sql_compound_execution_open(&query->as.compound,parameters,count,sources,&cause) :
      runtime_execution_select_open(query,parameters,count,sources,&cause);
  if (status != TURBODB_STATUS_OK) {
    query->execution_failure = cause;
    if (orm_sql_runtime_execution_close(query,NULL) != TURBODB_STATUS_OK) query->owner->failed = true;
    tdsql_error_set(error,status,cause.message);
  }
  return status;
}
turbodb_status_t orm_sql_runtime_execution_open(orm_sql_query *query, const turbodb_value_t *parameters,
    size_t count, const orm_sql_expr_query_sources *sources, turbodb_error_t *error) {
  if (query && query->dependencies.budget)
    return runtime_error(error,TURBODB_STATUS_UNSUPPORTED,"use statement resume for owned dependencies");
  return runtime_execution_open(query,parameters,count,sources,error);
}
turbodb_status_t orm_sql_runtime_execution_resume(orm_sql_query *query, turbodb_error_t *error) {
  turbodb_status_t status = runtime_execution_check(query,error);
  if (status != TURBODB_STATUS_OK) return status;
  if (!query->statement_parameters)
    return runtime_error(error,TURBODB_STATUS_UNSUPPORTED,"statement resume requires runtime-owned parameters");
  const orm_sql_expr_query_sources sources = {vec_data_const(&query->dependencies.sources),query->dependencies.query_count,query->evaluation};
  return runtime_execution_open(query,vec_data_const(&query->parameters.values),query->parameter_count,&sources,error);
}
turbodb_status_t orm_tidesdb_sql_runtime_next(orm_sql_query *query, orm_sql_scan_row *out, turbodb_error_t *error) {
  orm_sql_scan *scan = runtime_scan(query);
  return scan ? orm_tidesdb_sql_scan_next(scan, out, error) : runtime_error(error, TURBODB_STATUS_INVALID_STATE, "SQL runtime query is closed");
}
turbodb_status_t orm_tidesdb_sql_runtime_cancel(orm_sql_query *query, turbodb_error_t *error) {
  orm_sql_scan *scan = runtime_scan(query);
  return scan ? orm_tidesdb_sql_scan_cancel(scan, error) : runtime_error(error, TURBODB_STATUS_INVALID_STATE, "SQL runtime query is closed");
}
turbodb_status_t orm_tidesdb_sql_runtime_close(orm_sql_query *query, turbodb_error_t *error) {
  if (!query || !query->owner) return TURBODB_STATUS_OK;
  orm_sql_scan *scan = runtime_scan(query);
  if (scan && scan->evaluating) return runtime_error(error,TURBODB_STATUS_BUSY,"SQL query is evaluating");
  orm_sql_catalog_store *owner = query->owner; turbodb_status_t status, released;
  status = orm_tidesdb_sql_scan_close(&query->dependencies.explained,error);
  if (status != TURBODB_STATUS_OK) return status;
  if (query->kind == ORM_SQL_QUERY_COMPOUND) {
    status = orm_tidesdb_sql_compound_close(&query->as.compound,error); released = TURBODB_STATUS_OK;
  } else if (query->kind == ORM_SQL_QUERY_SELECT || query->kind == ORM_SQL_QUERY_EXPLAIN) {
    if (query->kind == ORM_SQL_QUERY_EXPLAIN) {
      status = orm_tidesdb_sql_scan_close(&query->as.select.explain.scan,error);
      released = orm_tidesdb_sql_explain_close(&query->as.select.explain.source,status == TURBODB_STATUS_OK ? error : NULL);
      if (status == TURBODB_STATUS_OK) status = released;
    } else status = orm_tidesdb_sql_select_close(&query->as.select.run, error);
    if(status==TURBODB_STATUS_BUSY) return status;
    released = orm_tidesdb_sql_from_close(&query->as.select.from_run,status == TURBODB_STATUS_OK ? error : NULL);
    if(released==TURBODB_STATUS_BUSY) return released;
    if (status == TURBODB_STATUS_OK) status = released;
    released = orm_tidesdb_sql_select_destroy(&query->as.select.plan, status == TURBODB_STATUS_OK ? error : NULL);
    if(released==TURBODB_STATUS_BUSY) return released;
    if (status == TURBODB_STATUS_OK) status = released;
    released = orm_tidesdb_sql_from_destroy(&query->as.select.from,status == TURBODB_STATUS_OK ? error : NULL);
    if(released==TURBODB_STATUS_BUSY) return released;
    if (status == TURBODB_STATUS_OK) status = released;
    for (size_t i = 0; i < vec_size(&query->as.select.relations); ++i) {
      released = orm_tidesdb_sql_relation_close(vec_at(&query->as.select.relations,i),status == TURBODB_STATUS_OK ? error : NULL);
      if(released==TURBODB_STATUS_BUSY) return released;
      if (status == TURBODB_STATUS_OK) status = released;
    }
    released = orm_sql_work_release(&query->as.select.relations,query->as.select.relation_bytes,owner->budget,
        status == TURBODB_STATUS_OK ? error : NULL);
    query->as.select.relation_bytes=0;
    if (status == TURBODB_STATUS_OK) status = released;
    released = orm_sql_work_release(&query->as.select.inputs,query->as.select.input_bytes,owner->budget,
        status == TURBODB_STATUS_OK ? error : NULL);
    query->as.select.input_bytes=0;
    if (status == TURBODB_STATUS_OK) status = released;
    released = orm_sql_work_release(&query->as.select.dependent_inputs,query->as.select.dependent_input_bytes,owner->budget,
        status == TURBODB_STATUS_OK ? error : NULL);
    query->as.select.dependent_input_bytes=0;
    if (status == TURBODB_STATUS_OK) status = released;
    released = orm_tidesdb_sql_relation_close(&query->as.select.source, status == TURBODB_STATUS_OK ? error : NULL);
    if(released==TURBODB_STATUS_BUSY) return released;
    if (query->as.select.unit.source.budget) {
      --owner->active_sources; query->as.select.unit.source=(orm_sql_row_source){0};
    }
  } else {
    status = orm_tidesdb_sql_scan_close(&query->as.show.scan, error);
    if (status==TURBODB_STATUS_BUSY) return status;
    released=orm_tidesdb_sql_expr_destroy(&query->as.show.filter,status==TURBODB_STATUS_OK?error:NULL);
    if(released==TURBODB_STATUS_BUSY) return released;
    if(status==TURBODB_STATUS_OK) status=released;
    released=orm_sql_work_release(&query->as.show.filter_slots,query->as.show.filter_bytes,owner->budget,
        status==TURBODB_STATUS_OK?error:NULL);
    query->as.show.filter_bytes=0;
    if(status==TURBODB_STATUS_OK) status=released;
    released = orm_tidesdb_sql_show_close(&query->as.show.source, status == TURBODB_STATUS_OK ? error : NULL);
  }
  if(status==TURBODB_STATUS_BUSY||released==TURBODB_STATUS_BUSY) return TURBODB_STATUS_BUSY;
  if (status == TURBODB_STATUS_OK) status = released;
  released = orm_sql_dependencies_close(&query->dependencies,status == TURBODB_STATUS_OK ? error : NULL);
  if(released==TURBODB_STATUS_BUSY) return released;
  if (status == TURBODB_STATUS_OK) status = released;
  released = orm_sql_work_release(&query->parameters.values,query->parameters.bytes,owner->budget,
      status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  released = orm_tidesdb_sql_budget_release(owner->budget, ORM_SQL_BUDGET_WORK_BYTES, query->metadata_bytes, status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  if (status != TURBODB_STATUS_OK) owner->failed = true;
  *query = (orm_sql_query){0}; return status;
}
