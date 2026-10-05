#include "cte_bind.h"
#include "name.h"
#include "work.h"
#include "select.h"
#include "error.h"
#include <string.h>

static turbodb_status_t cte_bind_error(turbodb_error_t *error, turbodb_status_t status, const char *message) {
  tdsql_error_set(error,status,message); return status;
}
static turbodb_status_t cte_bind_step(const orm_sql_query_scope *scope, size_t steps, turbodb_error_t *error) {
  orm_sql_budget_amount amount={0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=steps;
  return orm_tidesdb_sql_budget_reserve(scope->budget,&amount,error);
}
static bool cte_bind_inside(const sqlparser_node *node, const sqlparser_node *root) {
  return node && root && node->span.offset>=root->span.offset &&
      node->span.offset-root->span.offset<=root->span.length &&
      node->span.length<=root->span.length-(node->span.offset-root->span.offset);
}
static turbodb_status_t cte_bind_name(const orm_sql_query_scope *scope, sqlparser_id id, vstr *out, turbodb_error_t *error) {
  const char *reason=NULL;
  const turbodb_status_t status=orm_sql_name_node(scope->document,id,out,&reason);
  return status==TURBODB_STATUS_OK?status:cte_bind_error(error,status,reason);
}
static bool cte_bind_equal(vstr a, vstr b) { return a.len==b.len && !memcmp(a.data,b.data,a.len); }
static turbodb_status_t cte_bind_names(const orm_sql_query_scope *scope, sqlparser_list list,
    bool definitions, turbodb_error_t *error) {
  sqlparser_id id=list.first;
  for(size_t i=0;i<list.count;++i) {
    const sqlparser_node *node=sqlparser_get_node(scope->document,id);
    if(!node || (definitions && node->kind!=SQLPARSER_CTE))
      return cte_bind_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid CTE name list");
    vstr name={0};
    turbodb_status_t status=cte_bind_step(scope,i+1,error);
    if(status==TURBODB_STATUS_OK) status=cte_bind_name(scope,definitions?node->as.cte.name:id,&name,error);
    if(status!=TURBODB_STATUS_OK) return status;
    sqlparser_id previous=list.first;
    for(size_t j=0;j<i;++j) {
      const sqlparser_node *other=sqlparser_get_node(scope->document,previous); vstr compared;
      status=cte_bind_name(scope,definitions?other->as.cte.name:previous,&compared,error);
      if(status!=TURBODB_STATUS_OK) return status;
      if(cte_bind_equal(name,compared)) return cte_bind_error(error,TURBODB_STATUS_SQL_ERROR,
          definitions?"duplicate CTE name":"duplicate CTE column name");
      previous=other->next;
    }
    if(definitions) {
      status=cte_bind_names(scope,node->as.cte.columns,false,error);
      if(status!=TURBODB_STATUS_OK) return status;
    }
    id=node->next;
  }
  return id?cte_bind_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid CTE list length"):TURBODB_STATUS_OK;
}
static turbodb_status_t cte_bind_table(const orm_sql_query_scope *scope, const sqlparser_node *table,
    const sqlparser_node *root, size_t count, bool recursive, sqlparser_id *out, turbodb_error_t *error) {
  const sqlparser_node *name=sqlparser_get_node(scope->document,table->as.table.name);
  if(!name || name->kind!=SQLPARSER_NAME || name->as.name.parts!=1 || table->as.table.query || table->as.table.table_function)
    return TURBODB_STATUS_OK;
  vstr wanted; turbodb_status_t status=cte_bind_name(scope,table->as.table.name,&wanted,error);
  if(status!=TURBODB_STATUS_OK) return status;
  size_t shortest=SIZE_MAX; bool self=false;
  status=cte_bind_step(scope,count,error); if(status!=TURBODB_STATUS_OK) return status;
  for(size_t i=1;i<=count;++i) {
    const sqlparser_node *with=sqlparser_get_node(scope->document,(sqlparser_id)i);
    if(with->kind!=SQLPARSER_WITH || !cte_bind_inside(with,root) || !cte_bind_inside(table,with) || with->span.length>=shortest) continue;
    sqlparser_id id=with->as.with.bindings.first;
    for(size_t j=0;j<with->as.with.bindings.count;++j) {
      const sqlparser_node *cte=sqlparser_get_node(scope->document,id);
      const bool own=cte_bind_inside(table,sqlparser_get_node(scope->document,cte->as.cte.query));
      status=cte_bind_step(scope,1,error); if(status!=TURBODB_STATUS_OK) return status;
      if(own && !with->as.with.recursive) break;
      vstr candidate; status=cte_bind_name(scope,cte->as.cte.name,&candidate,error);
      if(status!=TURBODB_STATUS_OK) return status;
      if(cte_bind_equal(candidate,wanted)) { *out=id; shortest=with->span.length; self=own; break; }
      if(own) break;
      id=cte->next;
    }
  }
  return self && !recursive?cte_bind_error(error,TURBODB_STATUS_UNSUPPORTED,"recursive CTE execution is not supported"):TURBODB_STATUS_OK;
}
static turbodb_status_t cte_bind(const orm_sql_query_scope *scope,bool recursive,vec_t *references,size_t *bytes,turbodb_error_t *error) {
  if(!scope || !scope->document || !scope->root || !scope->budget || !references || references->initialized || !bytes || *bytes)
    return cte_bind_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid CTE binding inputs");
  const size_t count=sqlparser_node_count(scope->document);
  const sqlparser_node *root=sqlparser_get_node(scope->document,scope->root);
  bool present=false;
  turbodb_status_t status=cte_bind_step(scope,count,error);
  for(size_t i=1;status==TURBODB_STATUS_OK && i<=count;++i) {
    const sqlparser_node *node=sqlparser_get_node(scope->document,(sqlparser_id)i);
    if(node->kind==SQLPARSER_WITH && cte_bind_inside(node,root)) {
      present=true; status=cte_bind_names(scope,node->as.with.bindings,true,error);
    }
  }
  if(status!=TURBODB_STATUS_OK || !present) return status;
  status=orm_sql_work_zero(references,count,sizeof(sqlparser_id),_Alignof(sqlparser_id),scope->budget,bytes,error);
  for(size_t i=1;status==TURBODB_STATUS_OK && i<=count;++i) {
    const sqlparser_node *node=sqlparser_get_node(scope->document,(sqlparser_id)i);
    if(node->kind==SQLPARSER_TABLE && cte_bind_inside(node,root))
      status=cte_bind_table(scope,node,root,count,recursive,vec_at(references,i-1),error);
  }
  if(status!=TURBODB_STATUS_OK) {
    const turbodb_status_t released=orm_sql_work_release(references,*bytes,scope->budget,NULL); *bytes=0;
    if(released!=TURBODB_STATUS_OK) return released;
  }
  return status;
}
turbodb_status_t orm_sql_cte_bind(const orm_sql_query_scope *scope,vec_t *references,size_t *bytes,turbodb_error_t *error) {
  return cte_bind(scope,false,references,bytes,error);
}
turbodb_status_t orm_sql_cte_resolve(const orm_sql_query_scope *scope,vec_t *references,size_t *bytes,turbodb_error_t *error) {
  return cte_bind(scope,true,references,bytes,error);
}

turbodb_status_t orm_sql_cte_schema_close(orm_sql_cte_schema *schema,turbodb_error_t *error) {
  if(!schema || !schema->budget) return TURBODB_STATUS_OK;
  vec_t *vectors[]={&schema->columns,&schema->types,&schema->names};
  const size_t bytes[]={schema->column_bytes,schema->type_bytes,schema->name_bytes};
  turbodb_status_t status=TURBODB_STATUS_OK;
  for(size_t i=0;i<sizeof(vectors)/sizeof(vectors[0]);++i) {
    const turbodb_status_t released=orm_sql_work_release(vectors[i],bytes[i],schema->budget,status==TURBODB_STATUS_OK?error:NULL);
    if(status==TURBODB_STATUS_OK) status=released;
  }
  if(schema->metadata_bytes) {
    const turbodb_status_t released=orm_tidesdb_sql_budget_release(schema->budget,ORM_SQL_BUDGET_WORK_BYTES,
        schema->metadata_bytes,status==TURBODB_STATUS_OK?error:NULL);
    if(status==TURBODB_STATUS_OK) status=released;
  }
  *schema=(orm_sql_cte_schema){0}; return status;
}
static turbodb_status_t query_schema_bind(const orm_sql_query_scope *scope,sqlparser_id definition,
    const orm_sql_select *seed,bool recursive,bool derived,orm_sql_cte_schema *out,turbodb_error_t *error) {
  if(!scope || !scope->document || !scope->budget || !seed || seed->budget!=scope->budget ||
      !vec_size(&seed->columns) || !out || out->budget)
    return cte_bind_error(error,TURBODB_STATUS_INVALID_ARGUMENT,derived?
        "invalid derived query schema inputs":"invalid CTE seed schema inputs");
  const sqlparser_node *cte=sqlparser_get_node(scope->document,definition);
  if(!cte || cte->kind!=(derived?SQLPARSER_TABLE:SQLPARSER_CTE) ||
      !cte_bind_inside(cte,sqlparser_get_node(scope->document,scope->root)) || (derived && !cte->as.table.query))
    return cte_bind_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"query source definition is outside binding scope");
  const sqlparser_list aliases=derived?cte->as.table.column_aliases:cte->as.cte.columns;
  const size_t count=vec_size(&seed->columns);
  if(aliases.count && aliases.count!=count)
    return cte_bind_error(error,TURBODB_STATUS_SQL_ERROR,derived?
        "derived column list width differs from query":"CTE column list width differs from query");
  vstr name; turbodb_status_t status=cte_bind_name(scope,derived?cte->as.table.alias:cte->as.cte.name,&name,error);
  if(status==TURBODB_STATUS_OK && !derived) status=cte_bind_names(scope,aliases,false,error);
  if(status==TURBODB_STATUS_OK) status=cte_bind_step(scope,count,error);
  if(status!=TURBODB_STATUS_OK) return status;
  *out=(orm_sql_cte_schema){.budget=scope->budget};
  memcpy(out->name,name.data,name.len); out->name[name.len]=0;
  typedef struct cte_column_name { char text[ORM_SQL_SELECT_NAME_BYTES+1]; } cte_column_name;
  status=orm_tidesdb_sql_budget_reserve_capacity(scope->budget,1,sizeof(*out),0,&out->metadata_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&out->columns,count,sizeof(orm_sql_schema_column),
      _Alignof(orm_sql_schema_column),scope->budget,&out->column_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&out->types,count,sizeof(orm_sql_type),
      _Alignof(orm_sql_type),scope->budget,&out->type_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&out->names,count,sizeof(cte_column_name),
      _Alignof(cte_column_name),scope->budget,&out->name_bytes,error);
  sqlparser_id alias=aliases.first;
  for(size_t i=0;status==TURBODB_STATUS_OK && i<count;++i) {
    const orm_sql_select_column *column=orm_tidesdb_sql_select_column_at(seed,i);
    vstr column_name=vstr_from_cstr(column->name);
    if(alias) status=cte_bind_name(scope,alias,&column_name,error);
    orm_sql_predicate validator;
    if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,column->type,NULL,&validator,error);
    if(status!=TURBODB_STATUS_OK) break;
    cte_column_name *owned=vec_at(&out->names,i);
    memcpy(owned->text,column_name.data,column_name.len); owned->text[column_name.len]=0;
    if(derived) {
      status=cte_bind_step(scope,i+1,error);
      for(size_t j=0;status==TURBODB_STATUS_OK && j<i;++j) {
        const cte_column_name *previous=vec_at_const(&out->names,j);
        if(cte_bind_equal(column_name,vstr_from_cstr(previous->text)))
          status=cte_bind_error(error,TURBODB_STATUS_SQL_ERROR,"duplicate derived column name");
      }
      if(status!=TURBODB_STATUS_OK) break;
    }
    orm_sql_type type=column->type; if(recursive) type.nullable=true;
    *(orm_sql_type *)vec_at(&out->types,i)=type;
    *(orm_sql_schema_column *)vec_at(&out->columns,i)=(orm_sql_schema_column){vstr_from_cstr(owned->text),type};
    if(alias) alias=sqlparser_get_node(scope->document,alias)->next;
  }
  if(status==TURBODB_STATUS_OK) out->schema=(orm_sql_table_schema){vstr_from_cstr(out->name),vec_data_const(&out->columns),count};
  else {
    const turbodb_status_t released=orm_sql_cte_schema_close(out,NULL);
    if(released!=TURBODB_STATUS_OK) return released;
  }
  return status;
}
turbodb_status_t orm_sql_cte_schema_bind(const orm_sql_query_scope *scope,sqlparser_id definition,
    const orm_sql_select *seed,bool recursive,orm_sql_cte_schema *out,turbodb_error_t *error) {
  return query_schema_bind(scope,definition,seed,recursive,false,out,error);
}
turbodb_status_t orm_sql_derived_schema_bind(const orm_sql_query_scope *scope,sqlparser_id definition,
    const orm_sql_select *query,orm_sql_cte_schema *out,turbodb_error_t *error) {
  return query_schema_bind(scope,definition,query,false,true,out,error);
}
turbodb_status_t orm_sql_cte_schema_member(const orm_sql_cte_schema *schema,const orm_sql_select *member,turbodb_error_t *error) {
  if(!schema || !schema->budget || !member || member->budget!=schema->budget)
    return cte_bind_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid recursive member schema inputs");
  if(vec_size(&member->columns)!=schema->schema.count)
    return cte_bind_error(error,TURBODB_STATUS_SQL_ERROR,"recursive member width differs from seed");
  orm_sql_budget_amount charge={0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=schema->schema.count;
  turbodb_status_t status=orm_tidesdb_sql_budget_reserve(schema->budget,&charge,error);
  for(size_t i=0;status==TURBODB_STATUS_OK && i<schema->schema.count;++i) {
    const orm_sql_type type=orm_tidesdb_sql_select_column_at(member,i)->type;
    orm_sql_predicate validator;
    status=orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,type,NULL,&validator,error);
    if(status==TURBODB_STATUS_OK && type.kind!=TURBODB_VALUE_NULL && type.kind!=schema->schema.columns[i].type.kind)
      status=cte_bind_error(error,TURBODB_STATUS_TYPE_ERROR,"recursive member type differs from seed");
  }
  return status;
}

typedef struct cte_shape_visit {
  sqlparser_id ast,union_before;
  size_t parent,depth;
  bool right;
} cte_shape_visit;

turbodb_status_t orm_sql_cte_shape_close(orm_sql_cte_shape *shape,turbodb_error_t *error) {
  if(!shape || !shape->budget) return TURBODB_STATUS_OK;
  const turbodb_status_t status=orm_sql_work_release(&shape->nodes,shape->bytes,shape->budget,error);
  *shape=(orm_sql_cte_shape){0}; return status;
}
static turbodb_status_t cte_shape_tree(const orm_sql_query_scope *scope,sqlparser_id query,
    orm_sql_cte_shape *out,vec_t *members,turbodb_error_t *error) {
  const size_t capacity=sqlparser_node_count(scope->document);
  vec_t stack={0}; size_t bytes=0,pending=0;
  turbodb_status_t status=orm_sql_work_zero(&stack,capacity,sizeof(cte_shape_visit),_Alignof(cte_shape_visit),scope->budget,&bytes,error);
  if(status==TURBODB_STATUS_OK) *(cte_shape_visit *)vec_at(&stack,pending++)=(cte_shape_visit){query,0,SIZE_MAX,1,false};
  while(status==TURBODB_STATUS_OK && pending) {
    const cte_shape_visit visit=*(const cte_shape_visit *)vec_at_const(&stack,--pending);
    if(out->count==capacity || visit.depth>scope->max_depth) {
      status=cte_bind_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"CTE query tree depth or capacity exceeded"); break;
    }
    const sqlparser_node *ast=sqlparser_get_node(scope->document,visit.ast);
    if(!ast) { status=cte_bind_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"missing CTE query node"); break; }
    const bool leaf=ast->kind==SQLPARSER_SELECT,group=ast->kind==SQLPARSER_QUERY_GROUP,with=ast->kind==SQLPARSER_WITH;
    if(!leaf && !group && !with && (ast->kind!=SQLPARSER_UNION ||
        ast->as.compound.kind<SQLPARSER_COMPOUND_UNION || ast->as.compound.kind>SQLPARSER_COMPOUND_EXCEPT)) {
      status=cte_bind_error(error,TURBODB_STATUS_UNSUPPORTED,"CTE shape requires SELECT set operations or query groups"); break;
    }
    orm_sql_budget_amount amount={0}; amount.value[ORM_SQL_BUDGET_PLAN_NODES]=1; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
    status=orm_tidesdb_sql_budget_reserve(scope->budget,&amount,error); if(status!=TURBODB_STATUS_OK) break;
    const size_t index=out->count++;
    orm_sql_cte_shape_node *node=vec_at(&out->nodes,index);
    *node=(orm_sql_cte_shape_node){.ast=visit.ast,.union_before=visit.union_before,
        .parent=visit.parent,.left=SIZE_MAX,.right=SIZE_MAX};
    if(visit.parent!=SIZE_MAX) {
      orm_sql_cte_shape_node *parent=vec_at(&out->nodes,visit.parent);
      if(visit.right) parent->right=index; else parent->left=index;
    }
    if(leaf) { *(size_t *)vec_at(members,visit.ast-1)=index+1; continue; }
    enum { UNION_CHILDREN=2 };
    const size_t children=group || with?1:UNION_CHILDREN;
    if(capacity<children || pending>capacity-children || visit.depth==SIZE_MAX) {
      status=cte_bind_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"CTE traversal capacity exceeded"); break;
    }
    if(children==UNION_CHILDREN)
      *(cte_shape_visit *)vec_at(&stack,pending++)=(cte_shape_visit){ast->as.compound.right,visit.ast,index,visit.depth+1,true};
    *(cte_shape_visit *)vec_at(&stack,pending++)=(cte_shape_visit){group?ast->as.query_group.query:
        with?ast->as.with.body:ast->as.compound.left,visit.union_before,index,visit.depth+1,false};
  }
  const turbodb_status_t released=orm_sql_work_release(&stack,bytes,scope->budget,status==TURBODB_STATUS_OK?error:NULL);
  return status==TURBODB_STATUS_OK?released:status;
}
/* The nearest SELECT owns expressions/table occurrences; nested queries cannot
 * lend their self-reference or aggregate calls to the surrounding member. */
static turbodb_status_t cte_shape_owner(const orm_sql_query_scope *scope,const sqlparser_node *node,
    sqlparser_id *owner,turbodb_error_t *error) {
  const size_t count=sqlparser_node_count(scope->document);
  turbodb_status_t status=cte_bind_step(scope,count,error); if(status!=TURBODB_STATUS_OK) return status;
  size_t shortest=SIZE_MAX; *owner=0;
  for(size_t i=1;i<=count;++i) {
    const sqlparser_node *query=sqlparser_get_node(scope->document,(sqlparser_id)i);
    if(query->kind==SQLPARSER_SELECT && query->span.length<shortest && cte_bind_inside(node,query)) {
      shortest=query->span.length; *owner=(sqlparser_id)i;
    }
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t cte_shape_references(const orm_sql_query_scope *scope,sqlparser_id definition,
    const vec_t *references,const vec_t *members,orm_sql_cte_shape *shape,turbodb_error_t *error) {
  const size_t count=sqlparser_node_count(scope->document);
  const sqlparser_node *root=sqlparser_get_node(scope->document,sqlparser_get_node(scope->document,definition)->as.cte.query);
  turbodb_status_t status=cte_bind_step(scope,count,error);
  for(size_t i=1;status==TURBODB_STATUS_OK && i<=count;++i) {
    const sqlparser_node *node=sqlparser_get_node(scope->document,(sqlparser_id)i);
    if(*(const sqlparser_id *)vec_at_const(references,i-1)!=definition || !cte_bind_inside(node,root)) continue;
    sqlparser_id owner=0; status=cte_shape_owner(scope,node,&owner,error); if(status!=TURBODB_STATUS_OK) break;
    const size_t member=owner?*(const size_t *)vec_at_const(members,owner-1):0;
    if(!member) return cte_bind_error(error,TURBODB_STATUS_SQL_ERROR,"recursive CTE reference cannot occur in a subquery");
    orm_sql_cte_shape_node *output=vec_at(&shape->nodes,member-1);
    if(output->self) return cte_bind_error(error,TURBODB_STATUS_SQL_ERROR,"recursive member requires exactly one self reference");
    output->self=(sqlparser_id)i;
  }
  return status;
}
static turbodb_status_t cte_shape_from(const orm_sql_query_scope *scope,const sqlparser_node *select,
    sqlparser_id self,turbodb_error_t *error) {
  const sqlparser_node *table=sqlparser_get_node(scope->document,self);
  sqlparser_id cursor=select->as.select.from; size_t depth=0;
  while(cursor) {
    if(++depth>scope->max_depth) return cte_bind_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"recursive FROM depth exceeded");
    const turbodb_status_t status=cte_bind_step(scope,1,error); if(status!=TURBODB_STATUS_OK) return status;
    if(cursor==self) return TURBODB_STATUS_OK;
    const sqlparser_node *node=sqlparser_get_node(scope->document,cursor);
    if(!node || node->kind!=SQLPARSER_JOIN)
      return cte_bind_error(error,TURBODB_STATUS_UNSUPPORTED,"recursive reference requires a direct FROM table");
    const bool left=cte_bind_inside(table,sqlparser_get_node(scope->document,node->as.join.left));
    const bool right=cte_bind_inside(table,sqlparser_get_node(scope->document,node->as.join.right));
    if(left==right) return cte_bind_error(error,TURBODB_STATUS_SQL_ERROR,"recursive reference must be in FROM");
    if((node->as.join.kind==SQLPARSER_JOIN_LEFT && right) || (node->as.join.kind==SQLPARSER_JOIN_RIGHT && left))
      return cte_bind_error(error,TURBODB_STATUS_SQL_ERROR,"recursive reference cannot be on the null-extended side of an outer join");
    cursor=left?node->as.join.left:node->as.join.right;
  }
  return cte_bind_error(error,TURBODB_STATUS_SQL_ERROR,"recursive reference must be in FROM");
}
static turbodb_status_t cte_shape_members(const orm_sql_query_scope *scope,orm_sql_cte_shape *shape,turbodb_error_t *error) {
  size_t last_member=SIZE_MAX,last_distinct=SIZE_MAX;
  bool recursive=false;
  for(size_t i=0;i<shape->count;++i) {
    orm_sql_cte_shape_node *node=vec_at(&shape->nodes,i);
    if(node->left!=SIZE_MAX) continue;
    turbodb_status_t status=cte_bind_step(scope,1,error); if(status!=TURBODB_STATUS_OK) return status;
    const sqlparser_node *ast=sqlparser_get_node(scope->document,node->ast); last_member=i;
    const sqlparser_node *before=sqlparser_get_node(scope->document,node->union_before);
    if(before && before->as.compound.kind==SQLPARSER_COMPOUND_UNION && !before->as.compound.all) last_distinct=i;
    node->parts=node->self?ORM_SQL_CTE_RECURSIVE_PART:ORM_SQL_CTE_INITIAL_PART;
    if(!node->self) {
      if(recursive) return cte_bind_error(error,TURBODB_STATUS_SQL_ERROR,"initial CTE members must precede recursive members");
      ++shape->initial_members; continue;
    }
    recursive=true; ++shape->recursive_members;
    if(!shape->initial_members) return cte_bind_error(error,TURBODB_STATUS_SQL_ERROR,"recursive CTE requires an initial query before recursion");
    if(ast->as.select.group_by.count || ast->as.select.order_by.count || ast->as.select.distinct)
      return cte_bind_error(error,TURBODB_STATUS_SQL_ERROR,"recursive member forbids GROUP BY ORDER BY and SELECT DISTINCT");
    if(ast->as.select.limit) return cte_bind_error(error,TURBODB_STATUS_UNSUPPORTED,"LIMIT belongs on the complete recursive CTE, not an individual member");
    status=cte_shape_from(scope,ast,node->self,error); if(status!=TURBODB_STATUS_OK) return status;
  }
  if(last_distinct!=SIZE_MAX && last_distinct!=last_member &&
      ((const orm_sql_cte_shape_node *)vec_at_const(&shape->nodes,last_distinct))->self)
    return cte_bind_error(error,TURBODB_STATUS_UNSUPPORTED,"recursive UNION DISTINCT cannot be followed by UNION ALL members");
  return TURBODB_STATUS_OK;
}
static turbodb_status_t cte_shape_expressions(const orm_sql_query_scope *scope,const vec_t *members,
    const orm_sql_cte_shape *shape,turbodb_error_t *error) {
  if(!shape->recursive_members) return TURBODB_STATUS_OK;
  const size_t count=sqlparser_node_count(scope->document);
  turbodb_status_t status=cte_bind_step(scope,count,error);
  for(size_t i=1;status==TURBODB_STATUS_OK && i<=count;++i) {
    const sqlparser_node *node=sqlparser_get_node(scope->document,(sqlparser_id)i);
    orm_sql_aggregate_kind kind;
    if(node->kind!=SQLPARSER_WINDOW && !orm_sql_bind_aggregate_kind(scope->document,node,&kind)) continue;
    sqlparser_id owner=0; status=cte_shape_owner(scope,node,&owner,error); if(status!=TURBODB_STATUS_OK) break;
    const size_t member=owner?*(const size_t *)vec_at_const(members,owner-1):0;
    if(member && ((const orm_sql_cte_shape_node *)vec_at_const(&shape->nodes,member-1))->self)
      return cte_bind_error(error,TURBODB_STATUS_SQL_ERROR,"recursive member forbids aggregate and window functions");
  }
  return status;
}
static turbodb_status_t cte_shape_tails(const orm_sql_query_scope *scope,orm_sql_cte_shape *shape,turbodb_error_t *error) {
  for(size_t i=shape->count;i;--i) {
    orm_sql_cte_shape_node *node=vec_at(&shape->nodes,i-1);
    if(node->left==SIZE_MAX) continue;
    turbodb_status_t status=cte_bind_step(scope,1,error); if(status!=TURBODB_STATUS_OK) return status;
    unsigned parts=((const orm_sql_cte_shape_node *)vec_at_const(&shape->nodes,node->left))->parts;
    if(node->right!=SIZE_MAX) parts|=((const orm_sql_cte_shape_node *)vec_at_const(&shape->nodes,node->right))->parts;
    node->parts=(orm_sql_cte_parts)parts;
    if(!(parts&ORM_SQL_CTE_RECURSIVE_PART)) continue;
    const sqlparser_node *ast=sqlparser_get_node(scope->document,node->ast);
    const bool group=ast->kind==SQLPARSER_QUERY_GROUP;
    if(ast->kind==SQLPARSER_WITH) continue;
    if(!group && ast->as.compound.kind!=SQLPARSER_COMPOUND_UNION)
      return cte_bind_error(error,TURBODB_STATUS_SQL_ERROR,"recursive CTE members require UNION, not INTERSECT or EXCEPT");
    if((group?ast->as.query_group.order_by:ast->as.compound.order_by).count)
      return cte_bind_error(error,TURBODB_STATUS_UNSUPPORTED,"ORDER BY over a recursive CTE is not supported");
    if(parts==ORM_SQL_CTE_RECURSIVE_PART && (group?ast->as.query_group.limit:ast->as.compound.limit))
      return cte_bind_error(error,TURBODB_STATUS_UNSUPPORTED,"LIMIT belongs on the complete recursive CTE, not an individual member");
    if(group) continue;
    size_t child=i-1,parent=node->parent;
    while(parent!=SIZE_MAX) {
      status=cte_bind_step(scope,1,error); if(status!=TURBODB_STATUS_OK) return status;
      const orm_sql_cte_shape_node *ancestor=vec_at_const(&shape->nodes,parent);
      const sqlparser_node *outer=sqlparser_get_node(scope->document,ancestor->ast);
      if(outer->kind==SQLPARSER_UNION) {
        if(ancestor->right==child && outer->as.compound.all!=ast->as.compound.all)
          return cte_bind_error(error,TURBODB_STATUS_UNSUPPORTED,"mixed right-nested recursive UNION cannot be flattened");
        break;
      }
      child=parent; parent=ancestor->parent;
    }
  }
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_cte_shape_bind(const orm_sql_query_scope *scope,sqlparser_id definition,
    const vec_t *references,orm_sql_cte_shape *out,turbodb_error_t *error) {
  if(!scope || !scope->document || !scope->root || !scope->max_depth || !scope->budget || !out || out->budget ||
      !references || vec_size(references)!=sqlparser_node_count(scope->document))
    return cte_bind_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid recursive CTE shape inputs");
  const sqlparser_node *cte=sqlparser_get_node(scope->document,definition);
  if(!cte || cte->kind!=SQLPARSER_CTE || !cte_bind_inside(cte,sqlparser_get_node(scope->document,scope->root)))
    return cte_bind_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"CTE definition must belong to the binding scope");
  if(sqlparser_get_dialect(scope->document)!=SQLPARSER_MYSQL)
    return cte_bind_error(error,TURBODB_STATUS_UNSUPPORTED,"recursive CTE shape requires MySQL dialect");
  orm_sql_cte_shape shape={.budget=scope->budget}; vec_t members={0}; size_t bytes=0;
  const size_t count=sqlparser_node_count(scope->document);
  turbodb_status_t status=orm_sql_work_zero(&shape.nodes,count,sizeof(orm_sql_cte_shape_node),
      _Alignof(orm_sql_cte_shape_node),scope->budget,&shape.bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&members,count,sizeof(size_t),_Alignof(size_t),scope->budget,&bytes,error);
  if(status==TURBODB_STATUS_OK) status=cte_shape_tree(scope,cte->as.cte.query,&shape,&members,error);
  if(status==TURBODB_STATUS_OK) status=cte_shape_references(scope,definition,references,&members,&shape,error);
  if(status==TURBODB_STATUS_OK) status=cte_shape_members(scope,&shape,error);
  if(status==TURBODB_STATUS_OK) status=cte_shape_expressions(scope,&members,&shape,error);
  if(status==TURBODB_STATUS_OK) status=cte_shape_tails(scope,&shape,error);
  const turbodb_status_t released=orm_sql_work_release(&members,bytes,scope->budget,status==TURBODB_STATUS_OK?error:NULL);
  if(status==TURBODB_STATUS_OK) status=released;
  if(status!=TURBODB_STATUS_OK) {
    const turbodb_status_t cleanup=orm_sql_cte_shape_close(&shape,NULL);
    return cleanup==TURBODB_STATUS_OK?status:cleanup;
  }
  *out=shape; return TURBODB_STATUS_OK;
}
