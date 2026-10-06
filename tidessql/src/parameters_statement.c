#include "parameters.h"
#include "runtime.h"
#include "insert.h"
#include "change.h"
#include "name.h"
#include "store_internal.h"
#include "work.h"
#include "error.h"
#include "window.h"
#include <stdio.h>
#include <string.h>

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
static turbodb_status_t statement_released(statement_parameters *s,
    turbodb_status_t status,turbodb_status_t released);

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
static bool statement_inside(const sqlparser_node *node,const sqlparser_node *root) {
  return node&&root&&node->span.offset>=root->span.offset&&
      node->span.offset-root->span.offset<=root->span.length&&
      node->span.length<=root->span.length-(node->span.offset-root->span.offset);
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
  /* Marker-free and already-resolved structural wrappers need no scalar
   * inference; the final Binder validates their complete semantics. */
  for(size_t i=0;i<count;++i) {
    const uint64_t offset=*(const uint64_t *)vec_at_const(&s->parameters->offsets,i);
    if(offset<node->span.offset||offset-node->span.offset>=node->span.length) continue;
    if(!*(const bool *)vec_at_const(&s->parameters->resolved,i))
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
static turbodb_status_t statement_order_at(statement_parameters *s,const orm_sql_binding_scope *local,
    sqlparser_list list,const orm_sql_type *context) {
  sqlparser_id id=list.first;
  for(size_t i=0;i<list.count;++i) {
    const sqlparser_node *node=statement_node(s,id);
    if(!node||node->kind!=SQLPARSER_ORDER)
      return statement_error(s,node,TURBODB_STATUS_INVALID_ARGUMENT,"missing ORDER node");
    const turbodb_status_t status=statement_expression(s,local,node->as.order.expression,context);
    if(status!=TURBODB_STATUS_OK) return status;
    id=node->next;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t statement_order(statement_parameters *s,sqlparser_list list) {
  return statement_order_at(s,&s->local,list,NULL);
}
static turbodb_status_t statement_expressions(statement_parameters *s,const orm_sql_binding_scope *local,
    sqlparser_list list,const orm_sql_type *context) {
  sqlparser_id id=list.first;
  for(size_t i=0;i<list.count;++i) {
    const sqlparser_node *node=statement_node(s,id);
    if(!node) return statement_error(s,node,TURBODB_STATUS_INVALID_ARGUMENT,"missing expression list node");
    const turbodb_status_t status=statement_expression(s,local,id,context);
    if(status!=TURBODB_STATUS_OK) return status;
    id=node->next;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t statement_join_conditions(statement_parameters *s,const sqlparser_node *from) {
  if(!from) return TURBODB_STATUS_OK;
  const orm_sql_type boolean={TURBODB_VALUE_BOOLEAN,false};
  const size_t count=sqlparser_node_count(s->scope.document);
  turbodb_status_t status=statement_steps(s,count);
  for(size_t i=1;status==TURBODB_STATUS_OK&&i<=count;++i) {
    const sqlparser_node *node=statement_node(s,(sqlparser_id)i);
    if(node->kind==SQLPARSER_JOIN&&statement_inside(node,from))
      status=statement_expression(s,&s->local,node->as.join.condition,&boolean);
  }
  return status;
}
static turbodb_status_t statement_aggregate_arguments(statement_parameters *s,const sqlparser_node *select) {
  const size_t count=sqlparser_node_count(s->scope.document);
  turbodb_status_t status=statement_steps(s,count);
  for(size_t i=1;status==TURBODB_STATUS_OK&&i<=count;++i) {
    const sqlparser_node *call=statement_node(s,(sqlparser_id)i); orm_sql_aggregate_kind kind;
    if(!statement_inside(call,select)||!orm_sql_bind_aggregate_kind(s->scope.document,call,&kind)) continue;
    const orm_sql_type context={orm_sql_aggregate_bit_kind(kind)?TURBODB_VALUE_UINT64:
        kind==ORM_SQL_COUNT_VALUE&&!call->as.call.distinct?TURBODB_VALUE_TEXT:TURBODB_VALUE_DOUBLE,true};
    status=statement_expressions(s,&s->local,call->as.call.arguments,&context);
  }
  return status;
}
static turbodb_status_t statement_window_arguments(statement_parameters *s,const sqlparser_node *select) {
  const orm_sql_type integer={TURBODB_VALUE_UINT64,false};
  const size_t count=sqlparser_node_count(s->scope.document);
  turbodb_status_t status=statement_steps(s,count);
  for(size_t i=1;status==TURBODB_STATUS_OK&&i<=count;++i) {
    const sqlparser_node *window=statement_node(s,(sqlparser_id)i);
    if(window->kind!=SQLPARSER_WINDOW||!statement_inside(window,select)) continue;
    const sqlparser_node *call=statement_node(s,window->as.window.call); orm_sql_window_kind kind;
    if(!orm_sql_window_kind_at(s->scope.document,call,&kind)||orm_sql_window_aggregate_kind(kind)) continue;
    sqlparser_id argument=call->as.call.arguments.first;
    for(size_t position=0;status==TURBODB_STATUS_OK&&position<call->as.call.arguments.count;++position) {
      const bool bounded=kind==ORM_SQL_NTILE||(kind==ORM_SQL_NTH_VALUE&&position==1)||
          (orm_sql_window_offset_kind(kind)&&position==1);
      status=statement_expression(s,&s->local,argument,bounded?&integer:NULL);
      if(status==TURBODB_STATUS_OK) argument=statement_node(s,argument)->next;
    }
  }
  return status;
}
static turbodb_status_t statement_window_frames(statement_parameters *s,const sqlparser_node *select) {
  const orm_sql_type numeric={TURBODB_VALUE_UINT64,false};
  const size_t count=sqlparser_node_count(s->scope.document);
  turbodb_status_t status=statement_steps(s,count);
  for(size_t i=1;status==TURBODB_STATUS_OK&&i<=count;++i) {
    const sqlparser_node *frame=statement_node(s,(sqlparser_id)i);
    if(frame->kind!=SQLPARSER_WINDOW_FRAME||!statement_inside(frame,select)) continue;
    const sqlparser_id bounds[]={frame->as.frame.start,frame->as.frame.end};
    for(size_t j=0;status==TURBODB_STATUS_OK&&j<sizeof(bounds)/sizeof(bounds[0]);++j) {
      if(!bounds[j]) continue;
      const sqlparser_node *boundary=statement_node(s,bounds[j]);
      if(!boundary||boundary->kind!=SQLPARSER_WINDOW_BOUNDARY)
        return statement_error(s,boundary,TURBODB_STATUS_INVALID_ARGUMENT,"missing window boundary");
      status=statement_expression(s,&s->local,boundary->as.boundary.value,&numeric);
    }
  }
  return status;
}
static turbodb_status_t statement_windows(statement_parameters *s,const sqlparser_node *select) {
  const orm_sql_type numeric={TURBODB_VALUE_DOUBLE,true};
  const size_t count=sqlparser_node_count(s->scope.document);
  turbodb_status_t status=statement_window_arguments(s,select);
  if(status==TURBODB_STATUS_OK) status=statement_window_frames(s,select);
  if(status==TURBODB_STATUS_OK) status=statement_steps(s,count);
  for(size_t i=1;status==TURBODB_STATUS_OK&&i<=count;++i) {
    const sqlparser_node *window=statement_node(s,(sqlparser_id)i);
    if(!statement_inside(window,select)) continue;
    sqlparser_list partitions={0},orders={0};
    if(window->kind==SQLPARSER_WINDOW) {
      partitions=window->as.window.partition_by; orders=window->as.window.order_by;
    } else if(window->kind==SQLPARSER_WINDOW_DEFINITION) {
      partitions=window->as.window_definition.partition_by; orders=window->as.window_definition.order_by;
    } else continue;
    status=statement_expressions(s,&s->local,partitions,&numeric);
    if(status==TURBODB_STATUS_OK) status=statement_order_at(s,&s->local,orders,&numeric);
  }
  return status;
}
static turbodb_status_t statement_select(statement_parameters *s,const sqlparser_node *node) {
  const sqlparser_node *from=statement_node(s,node->as.select.from);
  if(from&&from->kind!=SQLPARSER_TABLE&&from->kind!=SQLPARSER_JOIN)
    return statement_error(s,from,TURBODB_STATUS_UNSUPPORTED,"unsupported SELECT source inference (#206)");
  const orm_sql_table_schema *schema=NULL; vstr qualifier={0};
  orm_sql_query frame={0};
  turbodb_status_t status=orm_sql_runtime_schema_open(&s->scope,s->owner,&frame,&schema,&qualifier,s->error);
  if(status==TURBODB_STATUS_OK&&schema) { s->local.schema=schema; s->local.qualifier=qualifier; }
  const orm_sql_type boolean={TURBODB_VALUE_BOOLEAN,false};
  const orm_sql_type numeric={TURBODB_VALUE_DOUBLE,true};
  if(status==TURBODB_STATUS_OK) status=statement_join_conditions(s,from);
  if(status==TURBODB_STATUS_OK) status=statement_aggregate_arguments(s,node);
  if(status==TURBODB_STATUS_OK) status=statement_windows(s,node);
  if(status==TURBODB_STATUS_OK) status=statement_expressions(s,&s->local,node->as.select.group_by,&numeric);
  bool output_contexts=s->scope.parameter_output_root==s->scope.root&&
      s->scope.parameter_output_count==node->as.select.columns.count;
  sqlparser_id inspected=node->as.select.columns.first;
  if(status==TURBODB_STATUS_OK&&output_contexts)
    status=statement_steps(s,node->as.select.columns.count);
  for(size_t i=0;status==TURBODB_STATUS_OK&&output_contexts&&i<node->as.select.columns.count;++i) {
    const sqlparser_node *column=statement_node(s,inspected);
    const sqlparser_node *expression=column&&column->kind==SQLPARSER_PROJECTION?
        statement_node(s,column->as.projection.expression):NULL;
    if(!expression) return statement_error(s,column,TURBODB_STATUS_INVALID_ARGUMENT,"missing projection");
    if(expression->kind==SQLPARSER_STAR) output_contexts=false;
    inspected=column->next;
  }
  sqlparser_id projection=node->as.select.columns.first;
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<node->as.select.columns.count;++i) {
    const sqlparser_node *column=statement_node(s,projection);
    if(!column||column->kind!=SQLPARSER_PROJECTION)
      return statement_error(s,column,TURBODB_STATUS_INVALID_ARGUMENT,"missing projection");
    const orm_sql_type *context=output_contexts?&s->scope.parameter_output_types[i]:NULL;
    if(context&&context->kind==TURBODB_VALUE_NULL) context=NULL;
    status=statement_expression(s,&s->local,column->as.projection.expression,context);
    projection=column->next;
  }
  if(status==TURBODB_STATUS_OK) status=statement_expression(s,&s->local,node->as.select.where,&boolean);
  if(status==TURBODB_STATUS_OK) status=statement_expression(s,&s->local,node->as.select.having,&boolean);
  if(status==TURBODB_STATUS_OK) status=statement_order(s,node->as.select.order_by);
  if(status==TURBODB_STATUS_OK) status=statement_page(s,node->as.select.limit);
  const turbodb_status_t released=orm_tidesdb_sql_runtime_close(&frame,
      status==TURBODB_STATUS_OK?s->error:NULL);
  return statement_released(s,status,released);
}
static bool statement_unresolved_in(const statement_parameters *s,const sqlparser_node *node) {
  const size_t count=vec_size(&s->parameters->offsets);
  for(size_t i=0;i<count;++i) {
    if(*(const bool *)vec_at_const(&s->parameters->resolved,i)) continue;
    const uint64_t offset=*(const uint64_t *)vec_at_const(&s->parameters->offsets,i);
    if(offset>=node->span.offset&&offset-node->span.offset<node->span.length) return true;
  }
  return false;
}
static bool statement_output_parameters_pending(const orm_sql_query_scope *scope,void *context) {
  const statement_parameters *s=context;
  if(!scope||!s||scope->document!=s->parameters->document) return false;
  const sqlparser_node *select=sqlparser_get_node(scope->document,scope->root);
  if(!select||select->kind!=SQLPARSER_SELECT) return false;
  sqlparser_id projection=select->as.select.columns.first;
  for(size_t i=0;i<select->as.select.columns.count;++i) {
    const sqlparser_node *column=sqlparser_get_node(scope->document,projection);
    const sqlparser_node *expression=column&&column->kind==SQLPARSER_PROJECTION?
        sqlparser_get_node(scope->document,column->as.projection.expression):NULL;
    if(!expression) return false;
    if(statement_unresolved_in(s,expression)) return true;
    projection=column->next;
  }
  return false;
}
static turbodb_status_t statement_query_parameters_prepare(orm_sql_query_scope *scope,
    void *context,turbodb_error_t *error) {
  statement_parameters *s=context;
  if(!scope||!s||!s->parameters||scope->document!=s->parameters->document||
      scope->budget!=s->parameters->budget||scope->parameter_count!=vec_size(&s->parameters->types)||
      scope->parameter_types!=vec_data_const(&s->parameters->types)||
      scope->parameter_resolved!=vec_data_const(&s->parameters->resolved)||
      (scope->parameter_output_count&&!scope->parameter_output_types)) {
    tdsql_error_set(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid query parameter inference callback");
    return TURBODB_STATUS_INVALID_ARGUMENT;
  }
  const sqlparser_node *node=sqlparser_get_node(scope->document,scope->root);
  if(!node) {
    tdsql_error_set(error,TURBODB_STATUS_INVALID_ARGUMENT,"missing query parameter inference root");
    return TURBODB_STATUS_INVALID_ARGUMENT;
  }
  const orm_sql_query_scope saved_scope=s->scope;
  const orm_sql_binding_scope saved_local=s->local;
  turbodb_error_t *saved_error=s->error;
  const orm_sql_table_schema empty={0};
  s->scope=*scope; s->error=error;
  s->local=(orm_sql_binding_scope){.document=scope->document,.schema=&empty,
      .outer_schema=scope->outer_schema,.outer_qualifier=scope->outer_qualifier,
      .queries=scope->queries,.query_count=scope->query_count,.budget=scope->budget};
  turbodb_status_t status=TURBODB_STATUS_OK;
  if(node->kind==SQLPARSER_SELECT) status=statement_select(s,node);
  else if(node->kind==SQLPARSER_UNION||node->kind==SQLPARSER_QUERY_GROUP)
    status=statement_page(s,node->kind==SQLPARSER_UNION?node->as.compound.limit:node->as.query_group.limit);
  else if(statement_unresolved_in(s,node))
    status=statement_error(s,node,TURBODB_STATUS_UNSUPPORTED,
        "unsupported query block parameter inference (#206)");
  s->scope=saved_scope; s->local=saved_local; s->error=saved_error;
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
turbodb_status_t orm_sql_parameters_show(const sqlparser_document *document,
    orm_sql_catalog_store *owner,vstr database_name,orm_sql_evaluation evaluation,
    size_t max_depth,orm_sql_parameters *out,orm_sql_query *metadata,
    turbodb_error_t *error) {
  if(!document||!owner||!max_depth||max_depth==SIZE_MAX||!out||out->budget||
      !metadata||metadata->owner) {
    tdsql_error_set(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SHOW parameter inference inputs");
    return TURBODB_STATUS_INVALID_ARGUMENT;
  }
  turbodb_status_t status=orm_sql_store_ready(owner,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_parameters_open(document,owner->budget,out,error);
  const sqlparser_node *node=status==TURBODB_STATUS_OK?
      sqlparser_get_node(document,sqlparser_statements(document).first):NULL;
  if(status==TURBODB_STATUS_OK&&(!node||node->kind!=SQLPARSER_SHOW)) {
    tdsql_error_set(error,TURBODB_STATUS_INVALID_ARGUMENT,"SHOW parameter inference requires a SHOW statement");
    status=TURBODB_STATUS_INVALID_ARGUMENT;
  }
  size_t bytes=0;
  if(status==TURBODB_STATUS_OK)
    status=orm_tidesdb_sql_budget_reserve_capacity(owner->budget,1,sizeof(*metadata),0,&bytes,error);
  if(status==TURBODB_STATUS_OK) {
    *metadata=(orm_sql_query){.owner=owner,.kind=ORM_SQL_QUERY_SHOW,.metadata_bytes=bytes,
        .execution_closed=true,.evaluation=evaluation};
    memset(&metadata->as,0,sizeof(metadata->as));
    status=orm_sql_show_open_evaluation(document,owner,database_name,evaluation,
        &metadata->as.show.source,error);
  }
  if(status==TURBODB_STATUS_OK) metadata->columns=metadata->as.show.source.schema.count;
  const orm_sql_table_schema empty={0};
  statement_parameters s={.parameters=out,.owner=owner,.error=error,
    .scope={.document=document,.root=sqlparser_statements(document).first,
        .budget=owner->budget,.max_depth=max_depth},
    .local={.document=document,.schema=status==TURBODB_STATUS_OK?
        &metadata->as.show.source.schema:&empty,.budget=owner->budget,
        .ascii_insensitive_names=true}};
  if(status==TURBODB_STATUS_OK&&node->as.show.where)
    status=statement_expression(&s,&s.local,node->as.show.where,NULL);
  if(status==TURBODB_STATUS_OK)
    status=orm_sql_parameters_types(out,&s.local.parameter_types,&s.local.parameter_count,error);
  s.local.parameter_offsets=vec_data_const(&out->offsets);
  if(status==TURBODB_STATUS_OK&&node->as.show.where)
    status=orm_sql_bind_expression(&s.local,node->as.show.where,max_depth,
        (orm_sql_expression_target){.program=&metadata->as.show.filter,
            .slots=&metadata->as.show.filter_slots,
            .slot_bytes=&metadata->as.show.filter_bytes},true,error);
  if(status!=TURBODB_STATUS_OK) {
    turbodb_status_t released=orm_tidesdb_sql_runtime_close(metadata,NULL);
    if(released!=TURBODB_STATUS_OK) { owner->failed=true; status=released; }
    released=orm_sql_parameters_close(out,NULL);
    if(released!=TURBODB_STATUS_OK) { owner->failed=true; status=released; }
  }
  return status;
}
turbodb_status_t orm_sql_parameters_statement(const sqlparser_document *document,
    orm_sql_catalog_store *owner,size_t max_depth,uint64_t max_iterations,
    orm_sql_parameters *out,turbodb_error_t *error) {
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
    .scope={.document=document,.root=sqlparser_statements(document).first,.budget=owner->budget,
      .max_depth=max_depth,.max_iterations=max_iterations},
    .local={.document=document,.schema=&empty,.budget=owner->budget}};
  const sqlparser_node *node=statement_node(&s,s.scope.root);
  const bool fixed_metadata_query=node&&(node->kind==SQLPARSER_SHOW||node->kind==SQLPARSER_EXPLAIN)&&
      vec_size(&out->offsets)==0;
  const bool explain=node&&node->kind==SQLPARSER_EXPLAIN;
  const sqlparser_node *inferred=explain?statement_node(&s,node->as.explain.statement):node;
  if(explain&&!fixed_metadata_query) s.scope.root=node->as.explain.statement;
  const bool query=inferred&&(inferred->kind==SQLPARSER_SELECT||inferred->kind==SQLPARSER_WITH||
      inferred->kind==SQLPARSER_UNION||inferred->kind==SQLPARSER_QUERY_GROUP);
  if(query&&!fixed_metadata_query) {
    s.scope.parameter_types=vec_data_const(&out->types);
    s.scope.parameter_count=vec_size(&out->types);
    s.scope.parameter_resolved=vec_data_const(&out->resolved);
    s.scope.prepare_parameters=statement_query_parameters_prepare;
    s.scope.parameter_context=&s;
    s.scope.output_parameters_pending=statement_output_parameters_pending;
    status=orm_sql_runtime_query_bind(&s.scope,owner,&s.frame,error);
    if(status==TURBODB_STATUS_OK)
      status=orm_sql_parameters_types(out,&s.scope.parameter_types,&s.scope.parameter_count,error);
    status=statement_release(&s,status);
  } else {
    status=fixed_metadata_query?TURBODB_STATUS_OK:statement_admit(&s);
    if(status==TURBODB_STATUS_OK&&!fixed_metadata_query) {
      if(inferred&&inferred->kind==SQLPARSER_INSERT) status=statement_insert(&s,inferred);
      else if(inferred&&(inferred->kind==SQLPARSER_UPDATE||inferred->kind==SQLPARSER_DELETE))
        status=statement_change(&s,inferred);
      else status=statement_error(&s,node,TURBODB_STATUS_UNSUPPORTED,
          "statement inference supports queries and INSERT/UPDATE/DELETE (#206)");
    }
    status=statement_release(&s,status);
    if(status==TURBODB_STATUS_OK)
      status=orm_sql_parameters_types(out,&s.scope.parameter_types,&s.scope.parameter_count,error);
    if(status==TURBODB_STATUS_OK&&!fixed_metadata_query) {
      if(inferred&&inferred->kind==SQLPARSER_INSERT) status=orm_sql_insert_bind(&s.scope,owner,error);
      else status=orm_sql_change_bind(&s.scope,owner,error);
    }
  }
  if(status!=TURBODB_STATUS_OK) {
    const turbodb_status_t released=orm_sql_parameters_close(out,NULL);
    if(released!=TURBODB_STATUS_OK) { owner->failed=true; status=released; }
  }
  return status;
}
