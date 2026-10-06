#include "prepared.h"
#include "runtime.h"
#include "name.h"
#include "store_internal.h"
#include "work.h"
#include "error.h"
#include "table_alter.h"
#include "table_clear.h"
#include "index_store.h"
#include "explain.h"
#include "cte_bind.h"
#include <string.h>

static turbodb_status_t prepared_error(turbodb_error_t *error,turbodb_status_t status,const char *reason) {
  tdsql_error_set(error,status,reason); return status;
}
static turbodb_status_t prepared_steps(orm_tidesdb_sql_budget *budget,size_t count,turbodb_error_t *error) {
  orm_sql_budget_amount charge={0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=count;
  return orm_tidesdb_sql_budget_reserve(budget,&charge,error);
}
static turbodb_status_t prepared_copy(orm_sql_prepared *p,vec_t *out,size_t count,size_t size,
    size_t align,const void *data,size_t *bytes,turbodb_error_t *error) {
  turbodb_status_t status=prepared_steps(&p->budget,count,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(out,count,size,align,&p->budget,bytes,error);
  if(status==TURBODB_STATUS_OK&&count) memcpy(vec_data(out),data,count*size);
  return status;
}
turbodb_status_t orm_sql_prepared_close(orm_sql_prepared *p,turbodb_error_t *error) {
  if(!p) return TURBODB_STATUS_OK;
  vec_t *vectors[]={&p->sql,&p->types,&p->columns,&p->names,&p->tables,&p->schema};
  const size_t bytes[]={p->sql_bytes,p->type_bytes,p->column_bytes,p->name_bytes,p->table_bytes,p->schema_bytes};
  turbodb_status_t status=TURBODB_STATUS_OK;
  for(size_t i=0;i<sizeof(vectors)/sizeof(vectors[0]);++i) {
    const turbodb_status_t released=orm_sql_work_release(vectors[i],bytes[i],&p->budget,status==TURBODB_STATUS_OK?error:NULL);
    if(status==TURBODB_STATUS_OK) status=released;
  }
  if(p->owner_bytes) {
    const turbodb_status_t released=orm_tidesdb_sql_budget_release(&p->budget,ORM_SQL_BUDGET_WORK_BYTES,
        p->owner_bytes,status==TURBODB_STATUS_OK?error:NULL);
    if(status==TURBODB_STATUS_OK) status=released;
  }
  if(p->budget.statement_active) {
    const turbodb_status_t ended=orm_tidesdb_sql_budget_end(&p->budget,status==TURBODB_STATUS_OK?error:NULL);
    if(status==TURBODB_STATUS_OK) status=ended;
  }
  *p=(orm_sql_prepared){0}; return status;
}
static bool prepared_inside(const sqlparser_node *node,const sqlparser_node *root) {
  return node&&root&&node->span.offset>=root->span.offset&&
      node->span.offset-root->span.offset<=root->span.length&&
      node->span.length<=root->span.length-(node->span.offset-root->span.offset);
}
static turbodb_status_t prepared_table_name(const sqlparser_document *doc,sqlparser_id id,
    orm_sql_prepared_table *table,turbodb_error_t *error) {
  vstr name={0}; const char *reason=NULL;
  const turbodb_status_t status=orm_sql_name_node(doc,id,&name,&reason);
  if(status!=TURBODB_STATUS_OK) return prepared_error(error,status,reason);
  if(name.len>ORM_SQL_SELECT_NAME_BYTES)
    return prepared_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"prepared table name capacity exceeded");
  memcpy(table->name,name.data,name.len); table->name[name.len]=0; return TURBODB_STATUS_OK;
}
static bool prepared_query_kind(sqlparser_kind kind) {
  return kind==SQLPARSER_SELECT||kind==SQLPARSER_UNION||kind==SQLPARSER_QUERY_GROUP||kind==SQLPARSER_WITH;
}
static turbodb_status_t prepared_catalog_table(const sqlparser_document *doc,const sqlparser_node *root,
    const vec_t *references,size_t ordinal,bool *out,turbodb_error_t *error) {
  const sqlparser_node *table=sqlparser_get_node(doc,(sqlparser_id)ordinal); *out=false;
  if(!table||table->kind!=SQLPARSER_TABLE||!prepared_inside(table,root)) return TURBODB_STATUS_OK;
  if(vec_size(references)&&*(const sqlparser_id *)vec_at_const(references,ordinal-1)) return TURBODB_STATUS_OK;
  if(table->as.table.query||table->as.table.group||table->as.table.lateral) return TURBODB_STATUS_OK;
  if(table->as.table.table_function||table->as.table.arguments.count)
    return prepared_error(error,TURBODB_STATUS_UNSUPPORTED,
        "prepared schema tracking requires Catalog table sources");
  *out=true; return TURBODB_STATUS_OK;
}
static turbodb_status_t prepared_tables(const sqlparser_document *doc,orm_sql_prepared *p,
    size_t max_depth,uint64_t max_iterations,turbodb_error_t *error) {
  const sqlparser_id statement=sqlparser_statements(doc).first;
  const sqlparser_node *root=sqlparser_get_node(doc,statement);
  sqlparser_id id=0,scope_root=0;
  switch(root->kind) {
    case SQLPARSER_SELECT:
    case SQLPARSER_UNION:
    case SQLPARSER_QUERY_GROUP: p->rows=true; scope_root=statement; break;
    case SQLPARSER_WITH: {
      const sqlparser_node *body=sqlparser_get_node(doc,root->as.with.body);
      if(!body||!prepared_query_kind(body->kind))
        return prepared_error(error,TURBODB_STATUS_UNSUPPORTED,"unsupported durable prepared WITH command");
      p->rows=true; scope_root=statement; break;
    }
    case SQLPARSER_SHOW: p->rows=true; break;
    case SQLPARSER_EXPLAIN: {
      p->rows=true;
      const sqlparser_node *query=sqlparser_get_node(doc,root->as.explain.statement);
      if(!query||!prepared_query_kind(query->kind))
        return prepared_error(error,TURBODB_STATUS_UNSUPPORTED,"unsupported durable prepared EXPLAIN query");
      scope_root=root->as.explain.statement;
      break;
    }
    case SQLPARSER_INSERT: id=root->as.insert.table; scope_root=statement; break;
    case SQLPARSER_UPDATE: id=root->as.update.table; scope_root=statement; break;
    case SQLPARSER_DELETE: id=root->as.delete_stmt.table; scope_root=statement; break;
    default: return prepared_error(error,TURBODB_STATUS_UNSUPPORTED,"unsupported durable prepared statement");
  }
  size_t count=id?1:0; const sqlparser_node *scope_node=sqlparser_get_node(doc,scope_root);
  const size_t nodes=sqlparser_node_count(doc);
  turbodb_status_t status=prepared_steps(&p->budget,nodes,error);
  if(status!=TURBODB_STATUS_OK) return status;
  vec_t references={0}; size_t reference_bytes=0;
  if(scope_root) {
    const orm_sql_query_scope scope={.document=doc,.root=scope_root,.budget=&p->budget,
        .max_depth=max_depth,.max_iterations=max_iterations};
    status=max_iterations?orm_sql_cte_resolve(&scope,&references,&reference_bytes,error):
        orm_sql_cte_bind(&scope,&references,&reference_bytes,error);
  }
  for(size_t i=1;status==TURBODB_STATUS_OK&&i<=nodes;++i) {
    bool catalog=false; status=prepared_catalog_table(doc,scope_node,&references,i,&catalog,error);
    if(status==TURBODB_STATUS_OK&&catalog) {
      if(count==SIZE_MAX)
        status=prepared_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"prepared table count overflow");
      else ++count;
    }
  }
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&p->tables,count,sizeof(orm_sql_prepared_table),
      _Alignof(orm_sql_prepared_table),&p->budget,&p->table_bytes,error);
  size_t index=0;
  if(status==TURBODB_STATUS_OK&&id)
    status=prepared_table_name(doc,id,vec_at(&p->tables,index++),error);
  for(size_t i=1;status==TURBODB_STATUS_OK&&i<=nodes;++i) {
    bool catalog=false; status=prepared_catalog_table(doc,scope_node,&references,i,&catalog,error);
    if(status==TURBODB_STATUS_OK&&catalog) {
      const sqlparser_node *table=sqlparser_get_node(doc,(sqlparser_id)i);
      status=prepared_table_name(doc,table->as.table.name,vec_at(&p->tables,index++),error);
    }
  }
  const turbodb_status_t released=orm_sql_work_release(&references,reference_bytes,&p->budget,
      status==TURBODB_STATUS_OK?error:NULL);
  if(status==TURBODB_STATUS_OK) status=released;
  return status;
}
static turbodb_status_t prepared_schema(vstr name,orm_sql_catalog_store *owner,
    orm_sql_table_definition *definition,vec_t *wire,size_t *bytes,uint64_t *id,bool *found,turbodb_error_t *error) {
  uint64_t version=0;
  turbodb_status_t status=orm_tidesdb_sql_catalog_lookup(owner,name,definition,id,&version,found,error);
  if(status==TURBODB_STATUS_OK&&*found)
    status=orm_tidesdb_sql_catalog_encode(definition,owner->max_record_bytes,wire,bytes,error);
  return status;
}
static turbodb_status_t prepared_source_release(orm_sql_catalog_store *owner,
    turbodb_status_t status,turbodb_status_t released) {
  if(released!=TURBODB_STATUS_OK) owner->failed=true;
  return status==TURBODB_STATUS_OK?released:status;
}
static turbodb_status_t prepared_snapshot_release(orm_sql_catalog_store *owner,
    orm_sql_table_definition *definition,vec_t *wire,size_t bytes,turbodb_status_t status,
    turbodb_error_t *error) {
  turbodb_status_t released=orm_sql_work_release(wire,bytes,owner->budget,
      status==TURBODB_STATUS_OK?error:NULL);
  status=prepared_source_release(owner,status,released);
  released=orm_tidesdb_sql_catalog_destroy(definition,status==TURBODB_STATUS_OK?error:NULL);
  return prepared_source_release(owner,status,released);
}
static turbodb_status_t prepared_snapshots(orm_sql_prepared *p,orm_sql_catalog_store *owner,
    turbodb_error_t *error) {
  size_t total=0; turbodb_status_t status=TURBODB_STATUS_OK;
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<vec_size(&p->tables);++i) {
    orm_sql_prepared_table *table=vec_at(&p->tables,i);
    orm_sql_table_definition definition={0}; vec_t wire={0}; size_t bytes=0; bool found=false;
    status=prepared_schema(vstr_from_cstr(table->name),owner,&definition,&wire,&bytes,&table->id,&found,error);
    if(status==TURBODB_STATUS_OK&&!found)
      status=prepared_error(error,TURBODB_STATUS_SQL_ERROR,"prepared table does not exist");
    if(status==TURBODB_STATUS_OK&&(vec_size(&wire)>SIZE_MAX-total))
      status=prepared_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"prepared schema snapshot overflow");
    if(status==TURBODB_STATUS_OK) { table->schema_offset=total; table->schema_size=vec_size(&wire); total+=vec_size(&wire); }
    status=prepared_snapshot_release(owner,&definition,&wire,bytes,status,error);
  }
  if(status==TURBODB_STATUS_OK)
    status=orm_sql_work_zero(&p->schema,total,1,_Alignof(uint8_t),&p->budget,&p->schema_bytes,error);
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<vec_size(&p->tables);++i) {
    orm_sql_prepared_table *table=vec_at(&p->tables,i); uint64_t id=0;
    orm_sql_table_definition definition={0}; vec_t wire={0}; size_t bytes=0; bool found=false;
    status=prepared_schema(vstr_from_cstr(table->name),owner,&definition,&wire,&bytes,&id,&found,error);
    if(status==TURBODB_STATUS_OK&&(!found||id!=table->id||vec_size(&wire)!=table->schema_size))
      status=prepared_error(error,TURBODB_STATUS_INVALID_STATE,"prepared schema changed while taking metadata snapshot");
    if(status==TURBODB_STATUS_OK&&table->schema_size)
      memcpy((uint8_t *)vec_data(&p->schema)+table->schema_offset,vec_data_const(&wire),table->schema_size);
    status=prepared_snapshot_release(owner,&definition,&wire,bytes,status,error);
  }
  return status;
}
static turbodb_status_t prepared_columns(orm_sql_prepared *p,orm_sql_query *query,
    const tdsql_limits *limits,turbodb_error_t *error) {
  const size_t count=query->columns;
  if(count>limits->max_columns)
    return prepared_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"prepared output column capacity exceeded");
  size_t names=0;
  for(size_t i=0;i<count;++i) {
    orm_sql_schema_column column={0};
    const turbodb_status_t status=orm_tidesdb_sql_runtime_column(query,i,&column,error);
    if(status!=TURBODB_STATUS_OK) return status;
    if(names==SIZE_MAX||column.name.len>SIZE_MAX-names-1)
      return prepared_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"prepared output name overflow");
    names+=column.name.len+1;
  }
  turbodb_status_t status=orm_sql_work_zero(&p->columns,count,sizeof(orm_sql_schema_column),
      _Alignof(orm_sql_schema_column),&p->budget,&p->column_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&p->names,names,sizeof(char),_Alignof(char),&p->budget,&p->name_bytes,error);
  if(status==TURBODB_STATUS_OK) status=prepared_steps(&p->budget,names,error);
  size_t offset=0;
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<count;++i) {
    orm_sql_schema_column column={0}; status=orm_tidesdb_sql_runtime_column(query,i,&column,error);
    if(status!=TURBODB_STATUS_OK) break;
    char *name=(char *)vec_data(&p->names)+offset;
    memcpy(name,column.name.data,column.name.len); offset+=column.name.len+1;
    *(orm_sql_schema_column *)vec_at(&p->columns,i)=
        (orm_sql_schema_column){.name={name,column.name.len},.type=column.type};
  }
  return status;
}
static turbodb_status_t prepared_explain_columns(orm_sql_prepared *p,const tdsql_limits *limits,
    turbodb_error_t *error) {
  if(ORM_SQL_EXPLAIN_COLUMNS>limits->max_columns)
    return prepared_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"prepared output column capacity exceeded");
  orm_sql_schema_column columns[ORM_SQL_EXPLAIN_COLUMNS]; size_t names=0;
  for(size_t i=0;i<ORM_SQL_EXPLAIN_COLUMNS;++i) {
    if(!orm_sql_explain_column_at(i,&columns[i])||names==SIZE_MAX||columns[i].name.len>SIZE_MAX-names-1)
      return prepared_error(error,TURBODB_STATUS_INTERNAL_ERROR,"invalid fixed EXPLAIN metadata");
    names+=columns[i].name.len+1;
  }
  turbodb_status_t status=orm_sql_work_zero(&p->columns,ORM_SQL_EXPLAIN_COLUMNS,
      sizeof(orm_sql_schema_column),_Alignof(orm_sql_schema_column),&p->budget,&p->column_bytes,error);
  if(status==TURBODB_STATUS_OK)
    status=orm_sql_work_zero(&p->names,names,sizeof(char),_Alignof(char),&p->budget,&p->name_bytes,error);
  if(status==TURBODB_STATUS_OK) status=prepared_steps(&p->budget,names,error);
  size_t offset=0;
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<ORM_SQL_EXPLAIN_COLUMNS;++i) {
    char *name=(char *)vec_data(&p->names)+offset;
    memcpy(name,columns[i].name.data,columns[i].name.len); offset+=columns[i].name.len+1;
    *(orm_sql_schema_column *)vec_at(&p->columns,i)=
        (orm_sql_schema_column){.name={name,columns[i].name.len},.type=columns[i].type};
  }
  return status;
}
static bool prepared_is_ddl(sqlparser_kind kind) {
  return kind==SQLPARSER_CREATE_TABLE || kind==SQLPARSER_ALTER_TABLE || kind==SQLPARSER_DROP_TABLE ||
      kind==SQLPARSER_TRUNCATE_TABLE || kind==SQLPARSER_CREATE_INDEX || kind==SQLPARSER_DROP_INDEX;
}
static turbodb_status_t prepared_ddl(const sqlparser_document *doc,orm_sql_catalog_store *owner,turbodb_error_t *error) {
  const size_t nodes=sqlparser_node_count(doc);
  turbodb_status_t status=prepared_steps(owner->budget,nodes,error);
  orm_sql_budget_amount charge={0}; charge.value[ORM_SQL_BUDGET_AST_NODES]=nodes;
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_budget_reserve(owner->budget,&charge,error);
  if(status!=TURBODB_STATUS_OK) return status;
  for(size_t i=1;i<=nodes;++i)
    if(sqlparser_get_node(doc,(sqlparser_id)i)->kind==SQLPARSER_PARAMETER)
      return prepared_error(error,TURBODB_STATUS_UNSUPPORTED,"DDL parameters are unsupported");
  const sqlparser_node *root=sqlparser_get_node(doc,sqlparser_statements(doc).first);
  switch(root->kind) {
    case SQLPARSER_CREATE_TABLE: return orm_sql_catalog_validate_create(doc,owner->budget,error);
    case SQLPARSER_ALTER_TABLE: return orm_sql_table_alter_validate(doc,owner,error);
    case SQLPARSER_DROP_TABLE:
    case SQLPARSER_TRUNCATE_TABLE: return orm_sql_table_clear_validate(doc,owner,error);
    case SQLPARSER_CREATE_INDEX: return orm_sql_index_create_validate(doc,owner,error);
    case SQLPARSER_DROP_INDEX: return orm_sql_index_drop_validate(doc,owner,error);
    default: return prepared_error(error,TURBODB_STATUS_UNSUPPORTED,"unsupported prepared DDL");
  }
}
turbodb_status_t orm_sql_prepared_open(const sqlparser_document *document,vstr sql,vstr database_name,
    orm_sql_evaluation evaluation,orm_sql_catalog_store *owner,const orm_sql_budget_limits *metadata_limits,
    const tdsql_limits *limits,size_t max_depth,uint64_t max_iterations,size_t owner_bytes,
    orm_sql_prepared *out,turbodb_error_t *error) {
  if(!document||!sql.data||!sql.len||sql.len==SIZE_MAX||!owner||!metadata_limits||!limits||
      !owner_bytes||!out||out->budget.statement_active)
    return prepared_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid durable prepared metadata inputs");
  if(sqlparser_get_dialect(document)!=SQLPARSER_MYSQL || sqlparser_statements(document).count!=1)
    return prepared_error(error,TURBODB_STATUS_UNSUPPORTED,"prepared metadata requires one MySQL statement");
  const sqlparser_node *root=sqlparser_get_node(document,sqlparser_statements(document).first);
  if(!root) return prepared_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"prepared statement root required");
  const bool ddl=prepared_is_ddl(root->kind);
  turbodb_status_t status=orm_sql_store_ready(owner,error);
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_budget_init(&out->budget,metadata_limits,error);
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_budget_begin(&out->budget,error);
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_budget_reserve_capacity(&out->budget,1,owner_bytes,0,&out->owner_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&out->sql,sql.len+1,sizeof(char),_Alignof(char),&out->budget,&out->sql_bytes,error);
  if(status==TURBODB_STATUS_OK) { memcpy(vec_data(&out->sql),sql.data,sql.len); status=prepared_steps(&out->budget,sql.len,error); }
  orm_sql_parameters parameters={0}; orm_sql_query query={0};
  const orm_sql_type *types=NULL; size_t count=0;
  if(status==TURBODB_STATUS_OK) status=ddl?prepared_ddl(document,owner,error):
      root->kind==SQLPARSER_SHOW?
      orm_sql_parameters_show(document,owner,database_name,evaluation,max_depth,&parameters,&query,error):
      orm_sql_parameters_statement(document,owner,max_depth,max_iterations,&parameters,error);
  if(status==TURBODB_STATUS_OK&&!ddl) status=orm_sql_parameters_types(&parameters,&types,&count,error);
  if(status==TURBODB_STATUS_OK&&count>limits->max_parameters)
    status=prepared_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"prepared parameter capacity exceeded");
  if(status==TURBODB_STATUS_OK) status=prepared_copy(out,&out->types,count,sizeof(orm_sql_type),_Alignof(orm_sql_type),types,&out->type_bytes,error);
  if(status==TURBODB_STATUS_OK&&!ddl) status=prepared_tables(document,out,max_depth,max_iterations,error);
  if(status==TURBODB_STATUS_OK&&!ddl) status=prepared_snapshots(out,owner,error);
  if(status==TURBODB_STATUS_OK&&out->rows) {
    const orm_sql_query_scope scope={.document=document,.root=sqlparser_statements(document).first,
      .parameter_types=types,.parameter_count=count,.budget=owner->budget,.max_depth=max_depth,
      .max_iterations=max_iterations};
    if(root->kind==SQLPARSER_SHOW) status=prepared_columns(out,&query,limits,error);
    else if(root->kind==SQLPARSER_EXPLAIN&&count) status=prepared_explain_columns(out,limits,error);
    else {
      status=prepared_query_kind(root->kind)?orm_sql_runtime_query_bind(&scope,owner,&query,error):
          orm_tidesdb_sql_runtime_open(document,owner,database_name,NULL,0,max_depth,&query,error);
      if(status==TURBODB_STATUS_OK) status=prepared_columns(out,&query,limits,error);
    }
  }
  turbodb_status_t released=orm_tidesdb_sql_runtime_close(&query,status==TURBODB_STATUS_OK?error:NULL);
  status=prepared_source_release(owner,status,released);
  released=orm_sql_parameters_close(&parameters,status==TURBODB_STATUS_OK?error:NULL);
  status=prepared_source_release(owner,status,released);
  if(status!=TURBODB_STATUS_OK) {
    released=orm_sql_prepared_close(out,NULL);
    if(released!=TURBODB_STATUS_OK) return released;
  }
  return status;
}
turbodb_status_t orm_sql_prepared_check(orm_sql_prepared *p,orm_sql_catalog_store *owner,turbodb_error_t *error) {
  if(!p||!p->budget.statement_active||!owner)
    return prepared_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid prepared schema check");
  if(p->invalidated) return prepared_error(error,TURBODB_STATUS_INVALID_STATE,"prepared schema invalidated; close and prepare again");
  turbodb_status_t status=orm_sql_store_ready(owner,error);
  if(status!=TURBODB_STATUS_OK||!vec_size(&p->tables)) return status;
  bool changed=false;
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<vec_size(&p->tables);++i) {
    const orm_sql_prepared_table *table=vec_at_const(&p->tables,i);
    orm_sql_table_definition definition={0}; vec_t wire={0}; size_t bytes=0;
    uint64_t id=0; bool found=false;
    status=prepared_schema(vstr_from_cstr(table->name),owner,&definition,&wire,&bytes,&id,&found,error);
    if(status==TURBODB_STATUS_OK) {
      status=prepared_steps(owner->budget,vec_size(&wire),error);
      if(status==TURBODB_STATUS_OK) changed=changed||!found||id!=table->id||
          vec_size(&wire)!=table->schema_size||
          memcmp(vec_data_const(&wire),(const uint8_t *)vec_data_const(&p->schema)+table->schema_offset,
              table->schema_size)!=0;
    }
    status=prepared_snapshot_release(owner,&definition,&wire,bytes,status,error);
  }
  if(status==TURBODB_STATUS_OK&&changed) {
    p->invalidated=true;
    status=prepared_error(error,TURBODB_STATUS_INVALID_STATE,"prepared source schema changed; close and prepare again");
  }
  return status;
}
