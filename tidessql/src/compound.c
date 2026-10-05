#include "compound.h"
#include "runtime.h"
#include "union.h"
#include "work.h"
#include "error.h"

typedef struct compound_link { sqlparser_id ast; size_t left,right,parent; bool leaf,group,omit_tail; } compound_link;
typedef struct compound_visit { sqlparser_id ast; size_t parent,depth; bool right; } compound_visit;
enum { COMPOUND_BIND_LEAVES,COMPOUND_BIND_TAILS,COMPOUND_BIND_PHASES };
typedef struct compound_cte_index { size_t output,depth; } compound_cte_index;
typedef struct compound_node {
  compound_link link;
  orm_sql_query query;
  orm_sql_union combined;
  orm_sql_select plan;
  orm_sql_select_run run;
  orm_sql_scan cardinality;
  orm_sql_query_demand demand;
  orm_sql_union_kind kind;
  turbodb_value_t witness;
  orm_sql_explain_source explain;
  orm_sql_row_source source;
  vec_t schema,types;
  size_t schema_bytes,type_bytes;
} compound_node;
static turbodb_status_t compound_error(turbodb_error_t *error, turbodb_status_t status, const char *reason) {
  tdsql_error_set(error,status,reason); return status;
}
static const orm_sql_type compound_witness_type={TURBODB_VALUE_BOOLEAN,false};
static turbodb_status_t compound_type_step(orm_sql_compound *run,turbodb_error_t *error) {
  orm_sql_budget_amount amount={0};amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=1;
  return orm_tidesdb_sql_budget_reserve(run->budget,&amount,error);
}
static const orm_sql_select *compound_plan(const compound_node *node) {
  return node->link.leaf ? &node->query.as.select.plan : &node->plan;
}
static orm_sql_scan *compound_scan(compound_node *node) {
  return node->link.leaf ? &node->query.as.select.run.scan :
      node->demand == ORM_SQL_QUERY_VALUES ? &node->run.scan : &node->cardinality;
}
static turbodb_status_t compound_pull(void *context, const turbodb_value_t **out, turbodb_error_t *error) {
  compound_node *node = context; orm_sql_scan_row row;
  const turbodb_status_t status = orm_tidesdb_sql_scan_next(compound_scan(context),&row,error);
  if (status == TURBODB_STATUS_OK) *out = row.state == ORM_SQL_SCAN_ROW ?
      (node->demand == ORM_SQL_QUERY_VALUES ? row.values : &node->witness) : NULL;
  return status;
}
turbodb_status_t orm_tidesdb_sql_compound_close(orm_sql_compound *run, turbodb_error_t *error) {
  if (!run || !run->budget) return TURBODB_STATUS_OK;
  if ((run->scan && run->scan->evaluating) || run->explained.evaluating)
    return compound_error(error,TURBODB_STATUS_BUSY,"compound query is evaluating");
  if (vec_size(&run->nodes) && ((compound_node *)vec_at(&run->nodes,0))->source.active)
    return compound_error(error,TURBODB_STATUS_BUSY,"compound query still has a consumer");
  turbodb_status_t status = orm_tidesdb_sql_scan_close(&run->explained,error);
  for (size_t i = 0; i < vec_size(&run->nodes); ++i) {
    compound_node *node = vec_at(&run->nodes,i);
    turbodb_status_t released = orm_tidesdb_sql_scan_close(&node->cardinality,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
    released = orm_tidesdb_sql_select_close(&node->run,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
    released = orm_tidesdb_sql_select_destroy(&node->plan,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
    released = orm_tidesdb_sql_union_close(&node->combined,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
    released = orm_tidesdb_sql_explain_close(&node->explain,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
    released = orm_tidesdb_sql_runtime_close(&node->query,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
    vec_t *vectors[] = {&node->schema,&node->types}; const size_t bytes[] = {node->schema_bytes,node->type_bytes};
    for (size_t j = 0; j < sizeof(vectors)/sizeof(vectors[0]); ++j) {
      released = orm_sql_work_release(vectors[j],bytes[j],run->budget,status == TURBODB_STATUS_OK ? error : NULL);
      if (status == TURBODB_STATUS_OK) status = released;
    }
  }
  turbodb_status_t released = orm_sql_work_release(&run->nodes,run->node_bytes,run->budget,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  if (run->metadata_bytes) {
    released = orm_tidesdb_sql_budget_release(run->budget,ORM_SQL_BUDGET_WORK_BYTES,run->metadata_bytes,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  *run = (orm_sql_compound){0}; return status;
}
static turbodb_status_t compound_tree(const orm_sql_query_scope *scope, orm_sql_compound *run, turbodb_error_t *error) {
  const size_t capacity = sqlparser_node_count(scope->document);
  vec_t stack = {0}, links = {0}; size_t stack_bytes = 0, link_bytes = 0, pending = 0, count = 0;
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_AST_NODES] = capacity;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(run->budget,&amount,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&stack,capacity,sizeof(compound_visit),
      _Alignof(compound_visit),run->budget,&stack_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&links,capacity,sizeof(compound_link),
      _Alignof(compound_link),run->budget,&link_bytes,error);
  if (status == TURBODB_STATUS_OK) *(compound_visit *)vec_at(&stack,pending++) = (compound_visit){scope->root,SIZE_MAX,1,false};
  while (status == TURBODB_STATUS_OK && pending) {
    const compound_visit visit = *(const compound_visit *)vec_at_const(&stack,--pending);
    if (visit.depth > scope->max_depth || count == capacity) {
      status = compound_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"compound query depth or capacity exceeded"); break;
    }
    sqlparser_id identity = visit.ast;
    const sqlparser_node *ast = sqlparser_get_node(scope->document,identity);
    if (ast && ast->kind == SQLPARSER_WITH) {
      identity = ast->as.with.body; ast = sqlparser_get_node(scope->document,identity);
    }
    if (!ast) { status = compound_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"missing compound query node"); break; }
    const bool leaf = ast->kind == SQLPARSER_SELECT, group = ast->kind == SQLPARSER_QUERY_GROUP;
    if (!leaf && !group && (ast->kind != SQLPARSER_UNION ||
        ast->as.compound.kind < SQLPARSER_COMPOUND_UNION || ast->as.compound.kind > SQLPARSER_COMPOUND_EXCEPT)) {
      status = compound_error(error,TURBODB_STATUS_UNSUPPORTED,"compound query requires SELECT set operations or query groups"); break;
    }
    amount = (orm_sql_budget_amount){0}; amount.value[ORM_SQL_BUDGET_PLAN_NODES] = 1;
    amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
    status = orm_tidesdb_sql_budget_reserve(run->budget,&amount,error);
    if (status != TURBODB_STATUS_OK) break;
    const size_t index = count++; compound_link *link = vec_at(&links,index);
    *link = (compound_link){.ast=identity,.parent=visit.parent,.leaf=leaf,.group=group};
    if (visit.parent != SIZE_MAX) {
      compound_link *parent = vec_at(&links,visit.parent);
      if (visit.right) parent->right = index; else parent->left = index;
    }
    if (leaf) continue;
    const size_t children = group ? 1 : ORM_SQL_UNION_INPUTS;
    if (capacity < children || pending > capacity-children || visit.depth == SIZE_MAX) {
      status = compound_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"compound query traversal overflow"); break;
    }
    if (!group) *(compound_visit *)vec_at(&stack,pending++) = (compound_visit){ast->as.compound.right,index,visit.depth+1,true};
    *(compound_visit *)vec_at(&stack,pending++) = (compound_visit){group ? ast->as.query_group.query : ast->as.compound.left,index,visit.depth+1,false};
  }
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run->nodes,count,sizeof(compound_node),
      _Alignof(compound_node),run->budget,&run->node_bytes,error);
  if (status == TURBODB_STATUS_OK) {
    run->count = count;
    for (size_t i = 0; i < count; ++i) ((compound_node *)vec_at(&run->nodes,i))->link = *(const compound_link *)vec_at_const(&links,i);
  }
  turbodb_status_t released = orm_sql_work_release(&stack,stack_bytes,run->budget,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  released = orm_sql_work_release(&links,link_bytes,run->budget,status == TURBODB_STATUS_OK ? error : NULL);
  return status == TURBODB_STATUS_OK ? released : status;
}
/* Keep preorder (parents close first), using identity groups at cut edges. This
 * preserves original AST ids and all pure-part tails without editing the AST. */
static turbodb_status_t compound_cte_tree(const orm_sql_query_scope *scope, const orm_sql_cte_shape *shape,
    orm_sql_cte_parts part, orm_sql_compound *run, turbodb_error_t *error) {
  vec_t indices = {0}; size_t bytes = 0, count = 0;
  orm_sql_budget_amount charge = {0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = shape->count;
  charge.value[ORM_SQL_BUDGET_AST_NODES] = sqlparser_node_count(scope->document);
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(run->budget,&charge,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&indices,shape->count,sizeof(compound_cte_index),
      _Alignof(compound_cte_index),run->budget,&bytes,error);
  if (status == TURBODB_STATUS_OK) for (size_t i = 0; i < shape->count; ++i) {
    const orm_sql_cte_shape_node *node = vec_at_const(&shape->nodes,i);
    compound_cte_index *index = vec_at(&indices,i);
    index->output = SIZE_MAX;
    if (!(node->parts & part)) continue;
    const size_t parent_depth = node->parent == SIZE_MAX ? 0 :
        ((const compound_cte_index *)vec_at_const(&indices,node->parent))->depth;
    if (parent_depth >= scope->max_depth) {
      status = compound_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"CTE part query depth exceeded"); break;
    }
    index->output = count++; index->depth = parent_depth+1;
  }
  charge = (orm_sql_budget_amount){0}; charge.value[ORM_SQL_BUDGET_PLAN_NODES] = count;
  charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = shape->count;
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_reserve(run->budget,&charge,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run->nodes,count,sizeof(compound_node),
      _Alignof(compound_node),run->budget,&run->node_bytes,error);
  if (status == TURBODB_STATUS_OK) {
    run->count = count;
    for (size_t i = 0; i < shape->count; ++i) {
      const size_t index = ((const compound_cte_index *)vec_at_const(&indices,i))->output;
      if (index == SIZE_MAX) continue;
      const orm_sql_cte_shape_node *node = vec_at_const(&shape->nodes,i);
      const sqlparser_node *ast = sqlparser_get_node(scope->document,node->ast);
      size_t left = node->left == SIZE_MAX ? SIZE_MAX : ((const compound_cte_index *)vec_at_const(&indices,node->left))->output;
      const size_t right = node->right == SIZE_MAX ? SIZE_MAX : ((const compound_cte_index *)vec_at_const(&indices,node->right))->output;
      const bool leaf = ast->kind == SQLPARSER_SELECT, group = !leaf && (left == SIZE_MAX || right == SIZE_MAX);
      if (left == SIZE_MAX) left = right;
      ((compound_node *)vec_at(&run->nodes,index))->link = (compound_link){
          .ast=node->ast,.left=left,.right=group ? SIZE_MAX : right,
          .parent=node->parent == SIZE_MAX ? SIZE_MAX : ((const compound_cte_index *)vec_at_const(&indices,node->parent))->output,
          .leaf=leaf,.group=group,.omit_tail=node->parts == ORM_SQL_CTE_MIXED_PARTS || ast->kind == SQLPARSER_WITH};
    }
  }
  const turbodb_status_t released = orm_sql_work_release(&indices,bytes,run->budget,status == TURBODB_STATUS_OK ? error : NULL);
  return status == TURBODB_STATUS_OK ? released : status;
}
static sqlparser_id compound_limit(const sqlparser_node *ast, const compound_link *link) {
  return link->omit_tail ? 0 : link->group ? ast->as.query_group.limit : ast->as.compound.limit;
}
static turbodb_status_t compound_demand(const orm_sql_query_scope *scope, orm_sql_compound *run, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = run->count;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(run->budget,&amount,error);
  if (status != TURBODB_STATUS_OK) return status;
  ((compound_node *)vec_at(&run->nodes,0))->demand = scope->demand;
  for (size_t i = 0; i < run->count; ++i) {
    compound_node *node = vec_at(&run->nodes,i); node->witness = turbodb_bool(true);
    if (node->link.leaf) continue;
    const sqlparser_node *ast = sqlparser_get_node(scope->document,node->link.ast);
    const sqlparser_id limit = compound_limit(ast,&node->link);
    const bool distinct = !node->link.group && !ast->as.compound.all &&
        (node->demand != ORM_SQL_QUERY_EXISTENCE || limit);
    const bool set = !node->link.group && ast->as.compound.kind != SQLPARSER_COMPOUND_UNION;
    node->kind = set ? ast->as.compound.kind == SQLPARSER_COMPOUND_INTERSECT ?
        (ast->as.compound.all ? ORM_SQL_INTERSECT_ALL : ORM_SQL_INTERSECT_DISTINCT) :
        (ast->as.compound.all ? ORM_SQL_EXCEPT_ALL : ORM_SQL_EXCEPT_DISTINCT) :
        distinct ? ORM_SQL_UNION_DISTINCT : ORM_SQL_UNION_ALL;
    /* Membership depends on complete tuples even when the consumer needs only
     * one witness. Simplify values only after this node computes membership. */
    const orm_sql_query_demand child = node->demand == ORM_SQL_QUERY_VALUES || distinct || set ? ORM_SQL_QUERY_VALUES :
        limit ? ORM_SQL_QUERY_CARDINALITY : node->demand;
    ((compound_node *)vec_at(&run->nodes,node->link.left))->demand = child;
    if (!node->link.group) ((compound_node *)vec_at(&run->nodes,node->link.right))->demand = child;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t compound_cardinality(compound_node *node, orm_sql_row_source *source,
    const turbodb_value_t *parameters, size_t count, turbodb_error_t *error) {
  uint64_t offset = 0, limit = 0;
  turbodb_status_t status = orm_tidesdb_sql_select_validate_parameters(&node->plan,parameters,count,&offset,&limit,error);
  if (status != TURBODB_STATUS_OK) return status;
  if (node->demand == ORM_SQL_QUERY_EXISTENCE && limit > 1) limit = 1;
  const size_t projection = 0;
  const orm_sql_scan_spec spec = {.projection=&projection,.projection_count=1,.offset=offset,.limit=limit};
  return orm_tidesdb_sql_scan_open_source(source,&spec,node->plan.budget,&node->cardinality,error);
}
static turbodb_status_t compound_execution_node(orm_sql_compound *run, compound_node *node,
    const turbodb_value_t *parameters, size_t count, const orm_sql_expr_query_sources *queries, turbodb_error_t *error) {
  compound_node *left = vec_at(&run->nodes,node->link.left);
  orm_sql_row_source *source = &left->source;
  turbodb_status_t status = TURBODB_STATUS_OK;
  if (!node->link.group) {
    compound_node *right = vec_at(&run->nodes,node->link.right);
    const orm_sql_type *types=left->demand==ORM_SQL_QUERY_VALUES && right->demand==ORM_SQL_QUERY_VALUES ?
        vec_data_const(&node->types) : NULL;
    status = orm_sql_union_open_as(source,&right->source,
        node->kind,types,&node->combined,error);
    if (status == TURBODB_STATUS_OK) source = orm_tidesdb_sql_union_source(&node->combined);
  }
  if (status == TURBODB_STATUS_OK) status = node->demand == ORM_SQL_QUERY_VALUES ?
      orm_tidesdb_sql_select_open_source_queries(&node->plan,source,parameters,count,queries,&node->run,error) :
      compound_cardinality(node,source,parameters,count,error);
  return status;
}
/* Leaf plans are prepared first. The root's existing schema stores the common
 * kind for this query expression before any set stage binds its sort/dedup. */
static turbodb_status_t compound_common_schema(orm_sql_compound *run,turbodb_error_t *error) {
  compound_node *root=vec_at(&run->nodes,0);
  if(root->schema.initialized) return TURBODB_STATUS_OK;
  const orm_sql_select *first=NULL;
  turbodb_status_t status=TURBODB_STATUS_OK;
  for(size_t i=0;status==TURBODB_STATUS_OK && i<run->count;++i) {
    status=compound_type_step(run,error);
    const compound_node *node=vec_at_const(&run->nodes,i);
    if(status==TURBODB_STATUS_OK && node->link.leaf) {
      const orm_sql_select *plan=compound_plan(node);
      if(!first) first=plan;
      else if(vec_size(&first->columns)!=vec_size(&plan->columns))
        status=compound_error(error,TURBODB_STATUS_SQL_ERROR,"set query column counts differ");
    }
  }
  if(status!=TURBODB_STATUS_OK) return status;
  if(!first) return compound_error(error,TURBODB_STATUS_INVALID_STATE,"set query has no query block");
  const size_t width=vec_size(&first->columns);
  status=orm_sql_work_zero(&root->schema,width,sizeof(orm_sql_schema_column),
      _Alignof(orm_sql_schema_column),run->budget,&root->schema_bytes,error);
  for(size_t column=0;status==TURBODB_STATUS_OK && column<width;++column) {
    bool real=false;
    for(size_t i=0;status==TURBODB_STATUS_OK && i<run->count;++i) {
      status=compound_type_step(run,error);
      const compound_node *node=vec_at_const(&run->nodes,i);
      if(node->link.leaf) real=real || orm_tidesdb_sql_select_column_at(compound_plan(node),column)->type.kind==TURBODB_VALUE_DOUBLE;
    }
    const orm_sql_select_column *left=orm_tidesdb_sql_select_column_at(first,column);
    orm_sql_schema_column *output=vec_at(&root->schema,column);
    *output=(orm_sql_schema_column){.name=vstr_from_cstr(left->name),.type=left->type};
    if(status==TURBODB_STATUS_OK && real)
      status=orm_tidesdb_sql_union_type(output->type,(orm_sql_type){TURBODB_VALUE_DOUBLE,false},&output->type,error);
    for(size_t i=0;status==TURBODB_STATUS_OK && i<run->count;++i) {
      status=compound_type_step(run,error);
      const compound_node *node=vec_at_const(&run->nodes,i);
      if(status==TURBODB_STATUS_OK && node->link.leaf)
        status=orm_tidesdb_sql_union_type(output->type,
            orm_tidesdb_sql_select_column_at(compound_plan(node),column)->type,&output->type,error);
    }
  }
  return status;
}
static turbodb_status_t compound_schema(orm_sql_compound *run,
    compound_node *node,const orm_sql_select *first,const orm_sql_select *second,turbodb_error_t *error) {
  const size_t width=vec_size(&first->columns);
  compound_node *root=vec_at(&run->nodes,0);
  turbodb_status_t status=compound_common_schema(run,error);
  if(status==TURBODB_STATUS_OK && root!=node)
    status=orm_sql_work_zero(&node->schema,width,sizeof(orm_sql_schema_column),
        _Alignof(orm_sql_schema_column),run->budget,&node->schema_bytes,error);
  for(size_t column=0;status==TURBODB_STATUS_OK && column<width;++column) {
    status=compound_type_step(run,error); if(status!=TURBODB_STATUS_OK) break;
    const orm_sql_select_column *left=orm_tidesdb_sql_select_column_at(first,column);
    const turbodb_value_kind_t common=((const orm_sql_schema_column *)vec_at_const(&root->schema,column))->type.kind;
    orm_sql_type type=left->type;
    if(second) {
      type.kind=common; type.nullable=orm_sql_union_nullable(node->kind,type,orm_tidesdb_sql_select_column_at(second,column)->type);
    }
    *(orm_sql_schema_column *)vec_at(&node->schema,column)=(orm_sql_schema_column){.name=vstr_from_cstr(left->name),.type=type};
  }
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&node->types,width,sizeof(orm_sql_type),
      _Alignof(orm_sql_type),run->budget,&node->type_bytes,error);
  for(size_t column=0;status==TURBODB_STATUS_OK && column<width;++column)
    *(orm_sql_type *)vec_at(&node->types,column)=((const orm_sql_schema_column *)vec_at_const(&node->schema,column))->type;
  return status;
}
static turbodb_status_t compound_tail(const orm_sql_query_scope *scope, const turbodb_value_t *parameters,
    orm_sql_compound *run, compound_node *node, const orm_sql_expr_query_sources *queries, turbodb_error_t *error) {
  compound_node *left = vec_at(&run->nodes,node->link.left);
  compound_node *right = node->link.group ? NULL : vec_at(&run->nodes,node->link.right);
  const orm_sql_select *first = compound_plan(left), *second = right ? compound_plan(right) : NULL;
  const size_t width = vec_size(&first->columns);
  if (second && width != vec_size(&second->columns)) return compound_error(error,TURBODB_STATUS_SQL_ERROR,"UNION branch column counts differ");
  turbodb_status_t status = compound_schema(run,node,first,second,error);
  const sqlparser_node *ast = sqlparser_get_node(scope->document,node->link.ast);
  const sqlparser_list orders = node->link.omit_tail ? (sqlparser_list){0} :
      node->link.group ? ast->as.query_group.order_by : ast->as.compound.order_by;
  const sqlparser_id limit = compound_limit(ast,&node->link);
  if (scope->in_query && limit) return compound_error(error,TURBODB_STATUS_UNSUPPORTED,"LIMIT in IN subqueries is not supported");
  const orm_sql_table_schema schema = {vstr_from_cstr("union_result"),vec_data_const(&node->schema),width};
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_select_bind_tail(scope,&schema,orders,limit,&node->plan,error);
  if (status == TURBODB_STATUS_OK)
    node->plan.correlated=node->plan.correlated||first->correlated||(second&&second->correlated);
  orm_sql_row_source *source = &left->source;
  const orm_sql_union_kind kind = node->kind;
  if (status == TURBODB_STATUS_OK && right && (run->describe || scope->defer_execution))
    status = orm_sql_union_validate_as(source,&right->source,kind,
        left->demand==ORM_SQL_QUERY_VALUES && right->demand==ORM_SQL_QUERY_VALUES ? vec_data_const(&node->types) : NULL,error);
  if (run->describe) {
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_explain_open(&node->plan,
        vstr_from_cstr(right ? "union_result" : "query_group"),parameters,vec_size(&node->plan.parameter_types),&node->explain,error);
    if (status == TURBODB_STATUS_OK) {
      const orm_sql_explain_block blocks[] = {ORM_SQL_EXPLAIN_UNION_ALL,ORM_SQL_EXPLAIN_UNION_DISTINCT,
          ORM_SQL_EXPLAIN_INTERSECT_ALL,ORM_SQL_EXPLAIN_INTERSECT_DISTINCT,
          ORM_SQL_EXPLAIN_EXCEPT_ALL,ORM_SQL_EXPLAIN_EXCEPT_DISTINCT};
      status = orm_tidesdb_sql_explain_block(&node->explain,
          right ? blocks[kind] : ORM_SQL_EXPLAIN_QUERY_GROUP,1,error);
    }
    return status;
  }
  if (status == TURBODB_STATUS_OK && !scope->defer_execution)
    status = compound_execution_node(run,node,parameters,scope->parameter_count,queries,error);
  return status;
}
turbodb_status_t orm_sql_compound_execution_close(orm_sql_compound *run, turbodb_error_t *error) {
  if (!run || !run->budget || run->describe)
    return compound_error(error,TURBODB_STATUS_INVALID_STATE,"compound execution plan required");
  if ((run->scan && run->scan->evaluating) || ((compound_node *)vec_at(&run->nodes,0))->source.active)
    return compound_error(error,TURBODB_STATUS_BUSY,"compound execution has an active consumer");
  for (size_t i = 0; i < run->count; ++i) {
    compound_node *node = vec_at(&run->nodes,i);
    turbodb_status_t status = orm_tidesdb_sql_scan_close(&node->cardinality,error);
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_select_close(&node->run,error);
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_union_close(&node->combined,error);
    if (status == TURBODB_STATUS_OK && node->link.leaf) status = orm_sql_runtime_execution_close(&node->query,error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_compound_execution_open(orm_sql_compound *run, const turbodb_value_t *parameters,
    size_t count, const orm_sql_expr_query_sources *sources, turbodb_error_t *error) {
  if (!run || !run->budget || run->describe)
    return compound_error(error,TURBODB_STATUS_INVALID_STATE,"compound execution plan required");
  orm_sql_budget_amount charge = {0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = run->count;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(run->budget,&charge,error);
  for (size_t i = run->count; status == TURBODB_STATUS_OK && i; --i) {
    compound_node *node = vec_at(&run->nodes,i-1);
    status = node->link.leaf ? orm_sql_runtime_execution_open(&node->query,parameters,count,sources,error) :
        compound_execution_node(run,node,parameters,count,sources,error);
  }
  return status;
}
/* Postorder over the existing fixed tree: no extra stack, row copies or data
 * executors. Leaf JOIN explanations retain their physical table order. Each
 * edge is visited at most twice, so traversal is O(nodes + explanation rows). */
static turbodb_status_t compound_explain_advance(orm_sql_compound *run, bool first, turbodb_error_t *error) {
  size_t index = first ? 0 : ((const compound_node *)vec_at_const(&run->nodes,run->position))->link.parent;
  size_t steps = 1;
  bool descend = first;
  if (!first && index != SIZE_MAX) {
    const compound_node *parent = vec_at_const(&run->nodes,index);
    descend = !parent->link.group && parent->link.left == run->position;
    if (descend) index = parent->link.right;
  }
  if (descend) while (!((const compound_node *)vec_at_const(&run->nodes,index))->link.leaf) {
    index = ((const compound_node *)vec_at_const(&run->nodes,index))->link.left; ++steps;
  }
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = steps;
  const turbodb_status_t status = orm_tidesdb_sql_budget_reserve(run->budget,&amount,error);
  if (status == TURBODB_STATUS_OK) run->position = index == SIZE_MAX ? run->count : index;
  return status;
}
static turbodb_status_t compound_explain_pull(void *context, const turbodb_value_t **out, turbodb_error_t *error) {
  orm_sql_compound *run = context;
  while (run->position < run->count) {
    compound_node *node = vec_at(&run->nodes,run->position);
    const turbodb_value_t *values = NULL;
    turbodb_status_t status;
    if (node->link.leaf) {
      orm_sql_scan_row row;
      status = orm_tidesdb_sql_runtime_next(&node->query,&row,error);
      if (status == TURBODB_STATUS_OK && row.state == ORM_SQL_SCAN_ROW) values = row.values;
    } else status = node->explain.source.next(node->explain.source.context,&values,error);
    if (status != TURBODB_STATUS_OK) return status;
    if (values) { *out = values; return TURBODB_STATUS_OK; }
    status = compound_explain_advance(run,false,error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  *out = NULL; return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_compound_explain_ids(orm_sql_compound *run, orm_sql_explain_block block,
    int64_t *next_id, turbodb_error_t *error) {
  if (!run || !run->budget || !run->describe || !next_id || *next_id < 0)
    return compound_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"EXPLAIN compound and identifier required");
  bool first = true;
  for (size_t i = 0; i < run->count; ++i) {
    compound_node *node = vec_at(&run->nodes,i);
    if (!node->link.leaf) continue;
    if (*next_id == INT64_MAX) return compound_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"EXPLAIN SELECT identifier overflow");
    orm_sql_explain_block branch = block;
    if (!first && block != ORM_SQL_EXPLAIN_RECURSIVE_BRANCH) {
      branch = ORM_SQL_EXPLAIN_UNION_BRANCH;
      size_t child = i, parent = node->link.parent, visited = 0;
      while (parent != SIZE_MAX) {
        const compound_node *ancestor = vec_at_const(&run->nodes,parent); ++visited;
        if (!ancestor->link.group && ancestor->link.right == child) {
          if (ancestor->kind == ORM_SQL_INTERSECT_ALL || ancestor->kind == ORM_SQL_INTERSECT_DISTINCT)
            branch = ORM_SQL_EXPLAIN_INTERSECT_BRANCH;
          else if (ancestor->kind == ORM_SQL_EXCEPT_ALL || ancestor->kind == ORM_SQL_EXCEPT_DISTINCT)
            branch = ORM_SQL_EXPLAIN_EXCEPT_BRANCH;
          break;
        }
        child = parent; parent = ancestor->link.parent;
      }
      orm_sql_budget_amount charge = {0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = visited;
      const turbodb_status_t status = orm_tidesdb_sql_budget_reserve(run->budget,&charge,error);
      if (status != TURBODB_STATUS_OK) return status;
    }
    const turbodb_status_t status = orm_tidesdb_sql_explain_block(&node->query.as.select.explain.source,
        branch,*next_id+1,error);
    if (status != TURBODB_STATUS_OK) return status;
    ++*next_id; first = false;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t compound_explain_open(orm_sql_compound *run, turbodb_error_t *error) {
  int64_t id = 0;
  turbodb_status_t status = orm_sql_compound_explain_ids(run,ORM_SQL_EXPLAIN_PRIMARY,&id,error);
  compound_node *root = vec_at(&run->nodes,0);
  const orm_sql_explain_source *source = root->link.leaf ? &root->query.as.select.explain.source : &root->explain;
  run->columns = source->columns;
  run->explanation = (orm_sql_row_source){run->budget,source->types,ORM_SQL_EXPLAIN_COLUMNS,run,compound_explain_pull,false};
  size_t projection[ORM_SQL_EXPLAIN_COLUMNS];
  for (size_t i = 0; i < ORM_SQL_EXPLAIN_COLUMNS; ++i) projection[i] = i;
  const orm_sql_scan_spec spec = {.projection=projection,.projection_count=ORM_SQL_EXPLAIN_COLUMNS,.limit=UINT64_MAX};
  if (status == TURBODB_STATUS_OK) status = compound_explain_advance(run,true,error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_scan_open_source(&run->explanation,&spec,run->budget,&run->explained,error);
  if (status == TURBODB_STATUS_OK) run->scan = &run->explained;
  return status;
}
static turbodb_status_t compound_open(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters, bool explain,
    const orm_sql_expr_query_sources *queries, const orm_sql_cte_shape *shape, orm_sql_cte_parts part,
    orm_sql_compound *out, turbodb_error_t *error) {
  if (!scope || !scope->document || !scope->root || !scope->max_depth || !owner || !scope->budget ||
      scope->budget != owner->budget || !out || out->budget ||
      (scope->parameter_count && (!scope->parameter_types ||
          (!parameters && (!scope->defer_execution || explain)))))
    return compound_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid compound query inputs");
  if (sqlparser_get_dialect(scope->document) != SQLPARSER_MYSQL || sqlparser_statements(scope->document).count != 1)
    return compound_error(error,TURBODB_STATUS_UNSUPPORTED,"compound runtime requires one MySQL statement");
  *out = (orm_sql_compound){.budget=scope->budget,.describe=explain};
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve_capacity(out->budget,1,sizeof(*out),0,&out->metadata_bytes,error);
  if (status == TURBODB_STATUS_OK) status = shape ? compound_cte_tree(scope,shape,part,out,error) : compound_tree(scope,out,error);
  if (status == TURBODB_STATUS_OK) status = compound_demand(scope,out,error);
  for (size_t phase=COMPOUND_BIND_LEAVES;status==TURBODB_STATUS_OK && phase<COMPOUND_BIND_PHASES;++phase) {
    for (size_t i = out->count; status == TURBODB_STATUS_OK && i; --i) {
      compound_node *node = vec_at(&out->nodes,i-1);
      if(node->link.leaf!=(phase==COMPOUND_BIND_LEAVES)) continue;
      orm_sql_query_scope block = *scope; block.root = node->link.ast;
      block.demand = node->demand;
      status = node->link.leaf ? orm_sql_runtime_scope_open(&block,owner,parameters,explain,queries,&node->query,error) :
          compound_tail(&block,parameters,out,node,queries,error);
      if (status != TURBODB_STATUS_OK) break;
      const orm_sql_select *plan = compound_plan(node);
      const size_t width = node->demand == ORM_SQL_QUERY_VALUES ? vec_size(&plan->columns) : 1;
      if(node->demand==ORM_SQL_QUERY_VALUES && !node->types.initialized)
        status = orm_sql_work_zero(&node->types,width,sizeof(orm_sql_type),_Alignof(orm_sql_type),out->budget,&node->type_bytes,error);
      for (size_t j = 0; status == TURBODB_STATUS_OK && node->demand==ORM_SQL_QUERY_VALUES && j < width; ++j)
        *(orm_sql_type *)vec_at(&node->types,j) = orm_tidesdb_sql_select_column_at(plan,j)->type;
      if (status == TURBODB_STATUS_OK) node->source = (orm_sql_row_source){out->budget,
          node->demand==ORM_SQL_QUERY_VALUES ? vec_data_const(&node->types) : &compound_witness_type,width,node,compound_pull,false};
    }
  }
  if (status == TURBODB_STATUS_OK && explain) status = compound_explain_open(out,error);
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t released = orm_tidesdb_sql_compound_close(out,NULL);
    return released == TURBODB_STATUS_OK ? status : released;
  }
  compound_node *root = vec_at(&out->nodes,0);
  if (!explain) out->scan = compound_scan(root);
  out->plan = compound_plan(root);
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_sql_compound_open_queries(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters, bool explain,
    const orm_sql_expr_query_sources *queries, orm_sql_compound *out, turbodb_error_t *error) {
  return compound_open(scope,owner,parameters,explain,queries,NULL,ORM_SQL_CTE_INITIAL_PART,out,error);
}
turbodb_status_t orm_sql_compound_open_cte_part(const orm_sql_query_scope *scope,
    const orm_sql_cte_shape *shape, orm_sql_cte_parts part,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters, bool explain,
    const orm_sql_expr_query_sources *queries, orm_sql_compound *out, turbodb_error_t *error) {
  if (!scope || !shape || !shape->budget || shape->budget != scope->budget || !shape->count ||
      shape->count > vec_size(&shape->nodes) || scope->demand != ORM_SQL_QUERY_VALUES ||
      (part != ORM_SQL_CTE_INITIAL_PART && part != ORM_SQL_CTE_RECURSIVE_PART) ||
      !(part == ORM_SQL_CTE_INITIAL_PART ? shape->initial_members : shape->recursive_members) ||
      ((const orm_sql_cte_shape_node *)vec_at_const(&shape->nodes,0))->ast != scope->root)
    return compound_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"validated CTE shape and nonempty query part required");
  return compound_open(scope,owner,parameters,explain,queries,shape,part,out,error);
}

turbodb_status_t orm_tidesdb_sql_compound_open(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters, bool explain, orm_sql_compound *out, turbodb_error_t *error) {
  return orm_sql_compound_open_queries(scope,owner,parameters,explain,NULL,out,error);
}
