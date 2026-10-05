#include "parameters.h"
#include "runtime.h"
#include "insert.h"
#include "change.h"
#include "name.h"
#include "store_internal.h"
#include "work.h"
#include "error.h"
#include <stdio.h>

typedef struct statement_parameters {
  orm_sql_parameters *parameters;
  orm_sql_catalog_store *owner;
  orm_sql_query_scope scope;
  orm_sql_binding_scope local;
  orm_sql_table_definition definition;
  orm_sql_table_schema schema;
  orm_sql_query frame;
  vec_t targets;
  size_t target_bytes;
  turbodb_error_t *error;
} statement_parameters;

static turbodb_status_t statement_error(statement_parameters *s,
    const sqlparser_node *node,turbodb_status_t status,const char *reason) {
  char message[TURBODB_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message,sizeof(message),"TidesSQL statement parameter inference at byte %zu: %s",
      node?node->span.offset:0,reason);
  tdsql_error_set(s->error,status,message); return status;
}
static turbodb_status_t statement_steps(statement_parameters *s,size_t count) {
  orm_sql_budget_amount charge={0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=count;
  return orm_tidesdb_sql_budget_reserve(s->scope.budget,&charge,s->error);
}
static const sqlparser_node *statement_node(const statement_parameters *s,sqlparser_id id) {
  return sqlparser_get_node(s->scope.document,id);
}
static turbodb_status_t statement_name(statement_parameters *s,sqlparser_id id,vstr *out) {
  const char *reason=NULL;
  const turbodb_status_t status=orm_sql_name_node(s->scope.document,id,out,&reason);
  return status==TURBODB_STATUS_OK?status:statement_error(s,statement_node(s,id),status,reason);
}
static turbodb_status_t statement_expression(statement_parameters *s,
    const orm_sql_binding_scope *local,sqlparser_id root,const orm_sql_type *context) {
  if(!root) return TURBODB_STATUS_OK;
  const sqlparser_node *node=statement_node(s,root);
  if(!node) return statement_error(s,node,TURBODB_STATUS_INVALID_ARGUMENT,"missing expression");
  const size_t count=vec_size(&s->parameters->offsets);
  turbodb_status_t status=statement_steps(s,count);
  if(status!=TURBODB_STATUS_OK) return status;
  /* Marker-free trees need no inference; the final Binder validates their full
   * supported semantics, including functions and DEFAULT. No fake input types. */
  for(size_t i=0;i<count;++i) {
    const uint64_t offset=*(const uint64_t *)vec_at_const(&s->parameters->offsets,i);
    if(offset>=node->span.offset&&offset-node->span.offset<node->span.length)
      return orm_sql_parameters_infer(s->parameters,local,root,context,s->scope.max_depth,s->error);
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t statement_page(statement_parameters *s,sqlparser_id id) {
  if(!id) return TURBODB_STATUS_OK;
  const sqlparser_node *page=statement_node(s,id);
  if(!page||page->kind!=SQLPARSER_LIMIT)
    return statement_error(s,page,TURBODB_STATUS_INVALID_ARGUMENT,"missing LIMIT node");
  const orm_sql_type type={TURBODB_VALUE_UINT64,false};
  turbodb_status_t status=statement_expression(s,&s->local,page->as.limit.count,&type);
  if(status==TURBODB_STATUS_OK) status=statement_expression(s,&s->local,page->as.limit.offset,&type);
  return status;
}
static turbodb_status_t statement_order(statement_parameters *s,sqlparser_list list) {
  sqlparser_id id=list.first;
  for(size_t i=0;i<list.count;++i) {
    const sqlparser_node *node=statement_node(s,id);
    if(!node||node->kind!=SQLPARSER_ORDER)
      return statement_error(s,node,TURBODB_STATUS_INVALID_ARGUMENT,"missing ORDER node");
    const turbodb_status_t status=statement_expression(s,&s->local,node->as.order.expression,NULL);
    if(status!=TURBODB_STATUS_OK) return status;
    id=node->next;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t statement_select(statement_parameters *s,const sqlparser_node *node) {
  if(node->as.select.group_by.count||node->as.select.having||node->as.select.windows.count)
    return statement_error(s,node,TURBODB_STATUS_UNSUPPORTED,"group/window parameter inference pending #206");
  const sqlparser_node *from=statement_node(s,node->as.select.from);
  if(from&&(from->kind!=SQLPARSER_TABLE||from->as.table.query||from->as.table.group||from->as.table.lateral))
    return statement_error(s,from,TURBODB_STATUS_UNSUPPORTED,"inference requires no FROM or one plain table (#206)");
  const orm_sql_table_schema *schema=NULL; vstr qualifier={0};
  turbodb_status_t status=orm_sql_runtime_schema_open(&s->scope,s->owner,&s->frame,&schema,&qualifier,s->error);
  if(status==TURBODB_STATUS_OK&&schema) { s->local.schema=schema; s->local.qualifier=qualifier; }
  sqlparser_id projection=node->as.select.columns.first;
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<node->as.select.columns.count;++i) {
    const sqlparser_node *column=statement_node(s,projection);
    if(!column||column->kind!=SQLPARSER_PROJECTION)
      return statement_error(s,column,TURBODB_STATUS_INVALID_ARGUMENT,"missing projection");
    status=statement_expression(s,&s->local,column->as.projection.expression,NULL);
    projection=column->next;
  }
  if(status==TURBODB_STATUS_OK) status=statement_expression(s,&s->local,node->as.select.where,NULL);
  if(status==TURBODB_STATUS_OK) status=statement_order(s,node->as.select.order_by);
  if(status==TURBODB_STATUS_OK) status=statement_page(s,node->as.select.limit);
  return status;
}
static turbodb_status_t statement_table(statement_parameters *s,sqlparser_id table,sqlparser_id alias) {
  vstr name={0}; uint64_t id=0,version=0; bool found=false;
  turbodb_status_t status=statement_name(s,table,&name);
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_catalog_lookup(s->owner,name,&s->definition,
      &id,&version,&found,s->error);
  if(status==TURBODB_STATUS_OK&&!found)
    status=statement_error(s,statement_node(s,table),TURBODB_STATUS_SQL_ERROR,"table does not exist");
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_catalog_schema(&s->definition,&s->schema,s->error);
  if(status==TURBODB_STATUS_OK) { s->local.schema=&s->schema; s->local.qualifier=name; }
  if(status==TURBODB_STATUS_OK&&alias) status=statement_name(s,alias,&s->local.qualifier);
  return status;
}
static turbodb_status_t statement_assignments(statement_parameters *s,
    sqlparser_list list,bool input_columns) {
  const orm_sql_table_schema empty={0}; orm_sql_binding_scope values=s->local;
  if(!input_columns) { values.schema=&empty; values.qualifier=(vstr){0}; }
  sqlparser_id id=list.first;
  for(size_t i=0;i<list.count;++i) {
    const sqlparser_node *node=statement_node(s,id); size_t slot=0;
    if(!node||node->kind!=SQLPARSER_ASSIGNMENT)
      return statement_error(s,node,TURBODB_STATUS_INVALID_ARGUMENT,"missing assignment");
    turbodb_status_t status=orm_sql_bind_column(&s->local,node->as.assignment.name,&slot,s->error);
    if(status==TURBODB_STATUS_OK) status=statement_expression(s,&values,node->as.assignment.value,
        &s->local.schema->columns[slot].type);
    if(status!=TURBODB_STATUS_OK) return status;
    id=node->next;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t statement_insert(statement_parameters *s,const sqlparser_node *node) {
  if(node->as.insert.query||node->as.insert.row_alias||node->as.insert.column_aliases.count)
    return statement_error(s,node,TURBODB_STATUS_UNSUPPORTED,"INSERT query/incoming alias inference pending #206");
  turbodb_status_t status=statement_table(s,node->as.insert.table,0);
  if(status!=TURBODB_STATUS_OK) return status;
  if(node->as.insert.assignments.count)
    status=statement_assignments(s,node->as.insert.assignments,false);
  else {
    const size_t count=node->as.insert.columns_specified?node->as.insert.columns.count:s->schema.count;
    if(count>s->schema.count)
      return statement_error(s,node,TURBODB_STATUS_SQL_ERROR,"too many INSERT target columns");
    status=orm_sql_work_zero(&s->targets,count,sizeof(size_t),_Alignof(size_t),s->scope.budget,&s->target_bytes,s->error);
    sqlparser_id column=node->as.insert.columns.first;
    for(size_t i=0;status==TURBODB_STATUS_OK&&i<count;++i) {
      size_t slot=i;
      status=statement_steps(s,1);
      if(status==TURBODB_STATUS_OK&&node->as.insert.columns_specified) {
        status=orm_sql_bind_column(&s->local,column,&slot,s->error);
        if(status==TURBODB_STATUS_OK) column=statement_node(s,column)->next;
      }
      if(status==TURBODB_STATUS_OK) *(size_t *)vec_at(&s->targets,i)=slot;
    }
    const orm_sql_table_schema empty={0}; orm_sql_binding_scope values=s->local;
    values.schema=&empty; values.qualifier=(vstr){0};
    sqlparser_id row=node->as.insert.rows.first;
    for(size_t r=0;status==TURBODB_STATUS_OK&&r<node->as.insert.rows.count;++r) {
      const sqlparser_node *record=statement_node(s,row);
      if(!record||record->kind!=SQLPARSER_ROW||record->as.row.values.count!=count)
        return statement_error(s,record,TURBODB_STATUS_SQL_ERROR,"INSERT row does not match target columns");
      sqlparser_id value=record->as.row.values.first;
      for(size_t i=0;status==TURBODB_STATUS_OK&&i<count;++i) {
        const size_t slot=*(const size_t *)vec_at_const(&s->targets,i);
        status=statement_expression(s,&values,value,&s->schema.columns[slot].type);
        if(status==TURBODB_STATUS_OK) value=statement_node(s,value)->next;
      }
      row=record->next;
    }
  }
  if(status==TURBODB_STATUS_OK) status=statement_assignments(s,node->as.insert.duplicate_assignments,true);
  return status;
}
static turbodb_status_t statement_change(statement_parameters *s,const sqlparser_node *node) {
  const bool deleting=node->kind==SQLPARSER_DELETE;
  turbodb_status_t status=statement_table(s,deleting?node->as.delete_stmt.table:node->as.update.table,
      deleting?node->as.delete_stmt.alias:node->as.update.alias);
  if(status==TURBODB_STATUS_OK&&!deleting) status=statement_assignments(s,node->as.update.assignments,true);
  if(status==TURBODB_STATUS_OK) status=statement_expression(s,&s->local,
      deleting?node->as.delete_stmt.where:node->as.update.where,NULL);
  if(status==TURBODB_STATUS_OK) status=statement_order(s,
      deleting?node->as.delete_stmt.order_by:node->as.update.order_by);
  if(status==TURBODB_STATUS_OK) status=statement_page(s,deleting?node->as.delete_stmt.limit:node->as.update.limit);
  return status;
}
static turbodb_status_t statement_released(statement_parameters *s,
    turbodb_status_t status,turbodb_status_t released) {
  if(released!=TURBODB_STATUS_OK) s->owner->failed=true;
  return status==TURBODB_STATUS_OK?released:status;
}
static turbodb_status_t statement_release(statement_parameters *s,turbodb_status_t status) {
  turbodb_status_t released=orm_tidesdb_sql_runtime_close(&s->frame,status==TURBODB_STATUS_OK?s->error:NULL);
  status=statement_released(s,status,released);
  released=orm_tidesdb_sql_catalog_destroy(&s->definition,status==TURBODB_STATUS_OK?s->error:NULL);
  status=statement_released(s,status,released);
  released=orm_sql_work_release(&s->targets,s->target_bytes,s->scope.budget,status==TURBODB_STATUS_OK?s->error:NULL);
  status=statement_released(s,status,released);
  return status;
}
static turbodb_status_t statement_admit(statement_parameters *s) {
  const size_t count=sqlparser_node_count(s->scope.document);
  turbodb_status_t status=statement_steps(s,count);
  for(size_t i=1;status==TURBODB_STATUS_OK&&i<=count;++i) {
    const sqlparser_node *node=statement_node(s,(sqlparser_id)i);
    if(node->kind==SQLPARSER_WITH||node->kind==SQLPARSER_UNION||node->kind==SQLPARSER_QUERY_GROUP||
        node->kind==SQLPARSER_SUBQUERY||
        (node->kind==SQLPARSER_IN&&(node->as.in.query||node->as.in.table)))
      status=statement_error(s,node,TURBODB_STATUS_UNSUPPORTED,"query dependency inference pending #206");
  }
  return status;
}
turbodb_status_t orm_sql_parameters_statement(const sqlparser_document *document,
    orm_sql_catalog_store *owner,size_t max_depth,orm_sql_parameters *out,turbodb_error_t *error) {
  if(!document||!owner||!max_depth||max_depth==SIZE_MAX||!out||out->budget) {
    tdsql_error_set(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid statement parameter inference inputs");
    return TURBODB_STATUS_INVALID_ARGUMENT;
  }
  turbodb_status_t status=orm_sql_store_ready(owner,error);
  if(status!=TURBODB_STATUS_OK) return status;
  status=orm_sql_parameters_open(document,owner->budget,out,error);
  if(status!=TURBODB_STATUS_OK) return status;
  const orm_sql_table_schema empty={0};
  statement_parameters s={.parameters=out,.owner=owner,.error=error,
    .scope={.document=document,.root=sqlparser_statements(document).first,.budget=owner->budget,.max_depth=max_depth},
    .local={.document=document,.schema=&empty,.budget=owner->budget}};
  const sqlparser_node *node=statement_node(&s,s.scope.root);
  status=statement_admit(&s);
  if(status==TURBODB_STATUS_OK) {
    if(node->kind==SQLPARSER_SELECT) status=statement_select(&s,node);
    else if(node->kind==SQLPARSER_INSERT) status=statement_insert(&s,node);
    else if(node->kind==SQLPARSER_UPDATE||node->kind==SQLPARSER_DELETE) status=statement_change(&s,node);
    else status=statement_error(&s,node,TURBODB_STATUS_UNSUPPORTED,"statement inference supports SELECT/INSERT/UPDATE/DELETE (#206)");
  }
  status=statement_release(&s,status);
  if(status==TURBODB_STATUS_OK) status=orm_sql_parameters_types(out,&s.scope.parameter_types,&s.scope.parameter_count,error);
  if(status==TURBODB_STATUS_OK) {
    if(node->kind==SQLPARSER_SELECT) status=orm_sql_runtime_query_bind(&s.scope,owner,&s.frame,error);
    else if(node->kind==SQLPARSER_INSERT) status=orm_sql_insert_bind(&s.scope,owner,error);
    else status=orm_sql_change_bind(&s.scope,owner,error);
    const turbodb_status_t released=orm_tidesdb_sql_runtime_close(&s.frame,status==TURBODB_STATUS_OK?error:NULL);
    if(released!=TURBODB_STATUS_OK) { owner->failed=true; if(status==TURBODB_STATUS_OK) status=released; }
  }
  if(status!=TURBODB_STATUS_OK) {
    const turbodb_status_t released=orm_sql_parameters_close(out,NULL);
    if(released!=TURBODB_STATUS_OK) { owner->failed=true; status=released; }
  }
  return status;
}
