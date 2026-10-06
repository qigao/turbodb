#include "dependencies.h"
#include "runtime.h"
#include "subquery.h"
#include "cte_bind.h"
#include "cte_store.h"
#include "cte_query.h"
#include "work.h"
#include "name.h"
#include "error.h"
#include <string.h>

enum { DEPENDENCY_EXPLAIN_SEED, DEPENDENCY_EXPLAIN_MEMBER, DEPENDENCY_EXPLAIN_PAGE };
typedef enum dependency_mode { DEPENDENCY_EXECUTE,DEPENDENCY_DESCRIBE,
  DEPENDENCY_LATERAL_METADATA,DEPENDENCY_BIND } dependency_mode;
typedef struct dependency_capture { size_t slot; bool used; } dependency_capture;
typedef struct dependency_node {
  sqlparser_id ast, query_ast;
  size_t parent, child, sibling;
  orm_sql_query query;
  orm_sql_subquery scalar;
  orm_sql_rows correlated_results;
  orm_sql_expr_query_source correlated_source;
  orm_sql_row_source source;
  orm_sql_type type;
  orm_sql_snapshot arguments;
  size_t marker_count, outer_count;
  orm_sql_subquery_kind subquery_kind;
  bool existence;
  turbodb_value_t witness;
  bool derived, cte, reference, ordered, replay, self, recursive, correlated;
  bool correlated_input,root_input,root_capture,callback_reader;
  bool lexical,frame_ready,lateral;
  bool lateral_bound,lateral_evaluating;
  orm_sql_from_input input;
  sqlparser_id lexical_select,lexical_from;
  orm_sql_query lexical_source;
  orm_sql_lateral_schema lateral_frame;
  vec_t lateral_prefixes;
  size_t lateral_prefix_bytes;
  orm_sql_table_schema frame;
  vec_t frame_columns;
  size_t frame_bytes,immediate_count,callback_count,inherited_count;
  vec_t captures;
  size_t capture_bytes;
  unsigned explained_part;
  orm_sql_dependencies *graph;
  size_t index,round_owner;
  size_t recursive_owner;
  orm_sql_cte_parts recursive_part;
  vec_t recursion;
  size_t recursion_bytes;
  sqlparser_id definition_ast;
  size_t definition, depth;
  orm_sql_cte_store cache;
  orm_sql_cte_schema cte_schema;
  orm_sql_cte_reader reader;
  size_t slot;
  char name[ORM_SQL_SELECT_NAME_BYTES+1];
  orm_sql_table_schema schema;
  vec_t columns, types;
  size_t column_bytes, type_bytes;
} dependency_node;
typedef struct dependency_query_visit { sqlparser_id ast; size_t depth; } dependency_query_visit;

static turbodb_status_t dependency_error(turbodb_error_t *error, turbodb_status_t status, const char *reason) {
  tdsql_error_set(error,status,reason); return status;
}
static turbodb_status_t dependency_steps(orm_tidesdb_sql_budget *budget, size_t steps, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = steps;
  return orm_tidesdb_sql_budget_reserve(budget,&amount,error);
}
static bool dependency_inside(const sqlparser_node *node, const sqlparser_node *root) {
  return node && root && node->span.offset >= root->span.offset &&
      node->span.offset-root->span.offset <= root->span.length &&
      node->span.length <= root->span.length-(node->span.offset-root->span.offset);
}
static turbodb_status_t dependency_pull(void *context, const turbodb_value_t **out, turbodb_error_t *error) {
  dependency_node *node = context; orm_sql_scan_row row;
  const turbodb_status_t status = orm_tidesdb_sql_runtime_next(&node->query,&row,error);
  if (status != TURBODB_STATUS_OK) return status;
  if (row.state == ORM_SQL_SCAN_CANCELLED)
    return dependency_error(error,TURBODB_STATUS_INVALID_STATE,"scalar query source was cancelled");
  *out = row.state == ORM_SQL_SCAN_ROW ? (node->existence ? &node->witness : row.values) : NULL; return TURBODB_STATUS_OK;
}
static turbodb_status_t dependency_frames_close(orm_sql_dependencies *run,turbodb_error_t *error) {
  turbodb_status_t status=TURBODB_STATUS_OK;
  for(size_t i=0;i<run->count;++i) {
    dependency_node *node=vec_at(&run->nodes,i);
    turbodb_status_t released=orm_tidesdb_sql_runtime_close(&node->lexical_source,
        status==TURBODB_STATUS_OK?error:NULL);
    if(status==TURBODB_STATUS_OK) status=released;
    released=orm_sql_runtime_lateral_schema_close(&node->lateral_frame,status==TURBODB_STATUS_OK?error:NULL);
    if(status==TURBODB_STATUS_OK) status=released;
    released=orm_sql_work_release(&node->lateral_prefixes,node->lateral_prefix_bytes,
        run->budget,status==TURBODB_STATUS_OK?error:NULL);
    node->lateral_prefix_bytes=0;
    if(status==TURBODB_STATUS_OK) status=released;
    released=orm_sql_work_release(&node->frame_columns,node->frame_bytes,
        run->budget,status==TURBODB_STATUS_OK?error:NULL);
    node->frame_bytes=0; node->frame.columns=NULL;
    if(status==TURBODB_STATUS_OK) status=released;
  }
  return status;
}
static turbodb_status_t dependency_recursive_release(dependency_node *node,
    orm_tidesdb_sql_budget *budget,turbodb_error_t *error) {
  turbodb_status_t status=vec_size(&node->recursion)?
      orm_sql_cte_query_close(vec_at(&node->recursion,0),error):TURBODB_STATUS_OK;
  if(status!=TURBODB_STATUS_OK) return status;
  status=orm_sql_work_release(&node->recursion,node->recursion_bytes,budget,error);
  node->recursion_bytes=0; return status;
}
turbodb_status_t orm_sql_dependencies_close(orm_sql_dependencies *run, turbodb_error_t *error) {
  if (!run || !run->budget) return TURBODB_STATUS_OK;
  if (run->explained.evaluating)
    return dependency_error(error,TURBODB_STATUS_BUSY,"query dependencies are evaluating");
  for(size_t i=0;i<run->count;++i)
    if(((const dependency_node *)vec_at_const(&run->nodes,i))->lateral_evaluating)
      return dependency_error(error,TURBODB_STATUS_BUSY,"LATERAL provider is evaluating");
  turbodb_status_t status = orm_tidesdb_sql_scan_close(&run->explained,error);
  for (size_t i = run->prepared; i; --i) {
    dependency_node *node = vec_at(&run->nodes,*(const size_t *)vec_at_const(&run->order,i-1));
    if (node->derived && node->source.active)
      return dependency_error(error,TURBODB_STATUS_BUSY,"close FROM consumers before derived dependencies");
    turbodb_status_t released = orm_tidesdb_sql_subquery_close(&node->scalar,status == TURBODB_STATUS_OK ? error : NULL);
    if (released == TURBODB_STATUS_BUSY) return dependency_error(error,released,"close query consumers before dependencies");
    if (status == TURBODB_STATUS_OK) status = released;
    if(node->correlated_source.active_runs)
      return dependency_error(error,TURBODB_STATUS_BUSY,"close query consumers before correlated dependencies");
    released=orm_sql_rows_close(&node->correlated_results,status==TURBODB_STATUS_OK?error:NULL);
    if(status==TURBODB_STATUS_OK) status=released;
    released = orm_sql_cte_reader_close(&node->reader,status == TURBODB_STATUS_OK ? error : NULL);
    if (released == TURBODB_STATUS_BUSY) return released;
    if (status == TURBODB_STATUS_OK) status = released;
    released = orm_sql_cte_store_close(&node->cache,status == TURBODB_STATUS_OK ? error : NULL);
    if (released == TURBODB_STATUS_BUSY) return released;
    if (status == TURBODB_STATUS_OK) status = released;
    released=dependency_recursive_release(node,run->budget,status==TURBODB_STATUS_OK?error:NULL);
    if(released==TURBODB_STATUS_BUSY) return released;
    if (status == TURBODB_STATUS_OK) status = released;
    released = orm_tidesdb_sql_runtime_close(&node->query,status == TURBODB_STATUS_OK ? error : NULL);
    if(released==TURBODB_STATUS_BUSY) return released;
    if (status == TURBODB_STATUS_OK) status = released;
    released=orm_sql_work_release(&node->arguments.values,node->arguments.bytes,run->budget,
        status==TURBODB_STATUS_OK?error:NULL);
    node->arguments.bytes=0;
    if(status==TURBODB_STATUS_OK) status=released;
    released = orm_sql_work_release(&node->columns,node->column_bytes,run->budget,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
    released = orm_sql_cte_schema_close(&node->cte_schema,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
    released = orm_sql_work_release(&node->types,node->type_bytes,run->budget,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
    /* A later BUSY must not release completed nodes' workspace a second time. */
    run->prepared = i-1;
  }
  /* Frames can be prepared before their query's topological turn. They own
   * metadata only, and must also close after a partial construction failure. */
  const turbodb_status_t frames=dependency_frames_close(run,status==TURBODB_STATUS_OK?error:NULL);
  if(status==TURBODB_STATUS_OK) status=frames;
  for(size_t i=0;i<run->count;++i) {
    dependency_node *node=vec_at(&run->nodes,i);
    /* A member child can prepare its definition's seed before that definition
     * reaches its final topological turn. Metadata consumers have closed above. */
    turbodb_status_t released=dependency_recursive_release(node,run->budget,status==TURBODB_STATUS_OK?error:NULL);
    if(released==TURBODB_STATUS_BUSY) return released;
    if(status==TURBODB_STATUS_OK) status=released;
    released=orm_sql_work_release(&node->arguments.values,node->arguments.bytes,run->budget,
        status==TURBODB_STATUS_OK?error:NULL);
    node->arguments.bytes=0;
    if(status==TURBODB_STATUS_OK) status=released;
    released=orm_sql_work_release(&node->captures,node->capture_bytes,
        run->budget,status==TURBODB_STATUS_OK?error:NULL);
    node->capture_bytes=0;
    if(status==TURBODB_STATUS_OK) status=released;
  }
  vec_t *vectors[] = {&run->nodes,&run->bindings,&run->sources,&run->order,&run->derived};
  const size_t bytes[] = {run->node_bytes,run->binding_bytes,run->source_bytes,run->order_bytes,run->derived_bytes};
  for (size_t i = 0; i < sizeof(vectors)/sizeof(vectors[0]); ++i) {
    const turbodb_status_t released = orm_sql_work_release(vectors[i],bytes[i],run->budget,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  if (run->metadata_bytes) {
    const turbodb_status_t released = orm_tidesdb_sql_budget_release(run->budget,ORM_SQL_BUDGET_WORK_BYTES,
        run->metadata_bytes,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  *run = (orm_sql_dependencies){0}; return status;
}
static sqlparser_id dependency_body(const sqlparser_node *node) {
  return node->kind == SQLPARSER_CTE ? node->as.cte.query :
      node->kind == SQLPARSER_SUBQUERY ? node->as.subquery.query :
      node->kind == SQLPARSER_TABLE ? node->as.table.query :
      node->kind == SQLPARSER_IN ? node->as.in.query :
      node->kind == SQLPARSER_UNARY && node->as.unary.op == SQLPARSER_OP_EXISTS ? node->as.unary.operand : 0;
}
static sqlparser_id dependency_root_from(const orm_sql_query_scope *scope) {
  const sqlparser_node *statement=sqlparser_get_node(scope->document,scope->root);
  if(statement&&statement->kind==SQLPARSER_WITH)
    statement=sqlparser_get_node(scope->document,statement->as.with.body);
  return statement&&statement->kind==SQLPARSER_SELECT?
      statement->as.select.from:0;
}
static turbodb_status_t dependency_count(const orm_sql_query_scope *scope, const vec_t *references,
    size_t *out, turbodb_error_t *error) {
  const size_t nodes = sqlparser_node_count(scope->document);
  const sqlparser_node *root = sqlparser_get_node(scope->document,scope->root);
  const turbodb_status_t status = dependency_steps(scope->budget,nodes,error);
  if (status != TURBODB_STATUS_OK) return status;
  *out = 0;
  for (size_t i = 1; i <= nodes; ++i) {
    const sqlparser_node *node = sqlparser_get_node(scope->document,(sqlparser_id)i);
    if (!dependency_inside(node,root)) continue;
    if (node->kind == SQLPARSER_IN && node->as.in.table)
      return dependency_error(error,TURBODB_STATUS_UNSUPPORTED,"runtime TABLE query dependencies are not supported");
    if (dependency_body(node) || (references->initialized && *(const sqlparser_id *)vec_at_const(references,i-1))) ++*out;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t dependency_storage(orm_sql_dependencies *run, size_t count, turbodb_error_t *error) {
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_PLAN_NODES] = count;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(run->budget,&amount,error);
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_reserve_capacity(run->budget,1,sizeof(*run),0,&run->metadata_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run->nodes,count,sizeof(dependency_node),
      _Alignof(dependency_node),run->budget,&run->node_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run->bindings,count,sizeof(orm_sql_expr_query_binding),
      _Alignof(orm_sql_expr_query_binding),run->budget,&run->binding_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run->sources,count,sizeof(orm_sql_expr_query_source *),
      _Alignof(orm_sql_expr_query_source *),run->budget,&run->source_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run->order,count,sizeof(size_t),
      _Alignof(size_t),run->budget,&run->order_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run->derived,count,sizeof(orm_sql_derived_binding),
      _Alignof(orm_sql_derived_binding),run->budget,&run->derived_bytes,error);
  if (status == TURBODB_STATUS_OK) run->count = count;
  return status;
}
/* Cache boundaries stop replay ancestry. Only streaming descendants belonging
 * to an actual recursive SELECT block are part of that definition's rounds. */
static turbodb_status_t dependency_recursive_prepare(const orm_sql_query_scope *scope,const vec_t *references,
    orm_sql_dependencies *run,turbodb_error_t *error) {
  turbodb_status_t status=dependency_steps(run->budget,run->count,error);
  for(size_t i=0;status==TURBODB_STATUS_OK && i<run->count;++i) {
    dependency_node *definition=vec_at(&run->nodes,i); if(!definition->recursive) continue;
    orm_sql_cte_shape shape={0};
    status=orm_sql_cte_shape_bind(scope,definition->ast,references,&shape,error);
    if(status==TURBODB_STATUS_OK) status=dependency_steps(run->budget,run->count,error);
    for(size_t j=0;status==TURBODB_STATUS_OK && j<run->count;++j) {
      dependency_node *node=vec_at(&run->nodes,j);
      size_t nearest=node->parent;
      while(nearest!=SIZE_MAX) {
        status=dependency_steps(run->budget,1,error); if(status!=TURBODB_STATUS_OK) break;
        const dependency_node *ancestor=vec_at_const(&run->nodes,nearest);
        if(ancestor->recursive) break;
        nearest=ancestor->parent;
      }
      if(status!=TURBODB_STATUS_OK) break;
      if(nearest==i) {
        node->recursive_owner=i;
        status=dependency_steps(run->budget,shape.count,error);
        const sqlparser_node *ast=sqlparser_get_node(scope->document,node->ast);
        for(size_t k=0;status==TURBODB_STATUS_OK&&k<shape.count;++k) {
          const orm_sql_cte_shape_node *part=vec_at_const(&shape.nodes,k);
          if(part->left==SIZE_MAX&&dependency_inside(ast,sqlparser_get_node(scope->document,part->ast)))
            node->recursive_part=part->parts;
        }
      }
      if(node->self || (!node->derived && !node->reference)) continue;
      size_t parent=node->parent,outer=j;
      while(parent!=SIZE_MAX && parent!=i) {
        status=dependency_steps(run->budget,1,error); if(status!=TURBODB_STATUS_OK) break;
        const dependency_node *ancestor=vec_at_const(&run->nodes,parent);
        if(!ancestor->derived) break;
        outer=parent; parent=ancestor->parent;
      }
      if(status!=TURBODB_STATUS_OK || parent!=i) continue;
      status=dependency_steps(run->budget,shape.count,error);
      const sqlparser_node *ast=sqlparser_get_node(scope->document,((const dependency_node *)vec_at_const(&run->nodes,outer))->ast);
      for(size_t k=0;status==TURBODB_STATUS_OK && k<shape.count;++k) {
        const orm_sql_cte_shape_node *member=vec_at_const(&shape.nodes,k);
        if(member->self && dependency_inside(ast,sqlparser_get_node(scope->document,member->ast))) {
          node->round_owner=i; break;
        }
      }
    }
    const turbodb_status_t released=orm_sql_cte_shape_close(&shape,status==TURBODB_STATUS_OK?error:NULL);
    if(status==TURBODB_STATUS_OK) status=released;
  }
  return status;
}
/* Registry order is AST identity; sibling order is source text. Neither uses
 * parser allocation order to infer parentage or dependency opening order. */
static turbodb_status_t dependency_source_order(const orm_sql_query_scope *scope,
    const orm_sql_dependencies *run,sqlparser_id from,bool *ready,size_t *depth,turbodb_error_t *error) {
  const sqlparser_node *source=sqlparser_get_node(scope->document,from);
  turbodb_status_t status=dependency_steps(run->budget,run->count,error);
  if(status!=TURBODB_STATUS_OK) return status;
  for(size_t j=0;j<run->count;++j) {
    const dependency_node *input=vec_at_const(&run->nodes,j);
    if((!input->derived&&!input->reference)||
        !dependency_inside(sqlparser_get_node(scope->document,input->ast),source)) continue;
    if(!input->ordered) *ready=false;
    if(input->ordered&&*depth<=input->depth) *depth=input->depth+1;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t dependency_tree(const orm_sql_query_scope *scope, const vec_t *references,
    bool recursive,orm_sql_dependencies *run, turbodb_error_t *error) {
  const size_t nodes = sqlparser_node_count(scope->document);
  const sqlparser_node *root = sqlparser_get_node(scope->document,scope->root);
  turbodb_status_t status = dependency_steps(run->budget,nodes,error); size_t count = 0;
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 1; i <= nodes; ++i) {
    const sqlparser_node *node = sqlparser_get_node(scope->document,(sqlparser_id)i);
    const sqlparser_id reference = references->initialized ? *(const sqlparser_id *)vec_at_const(references,i-1) : 0;
    if ((!dependency_body(node) && !reference) || !dependency_inside(node,root)) continue;
    dependency_node *entry = vec_at(&run->nodes,count);
    *entry = (dependency_node){.ast=(sqlparser_id)i,.query_ast=dependency_body(node),
        .parent=SIZE_MAX,.child=SIZE_MAX,.sibling=SIZE_MAX,.round_owner=SIZE_MAX,
        .recursive_owner=SIZE_MAX,.graph=run,.index=count};
    entry->reference = reference != 0; entry->definition_ast = reference;
    if(reference) entry->self=dependency_inside(node,sqlparser_get_node(scope->document,
        sqlparser_get_node(scope->document,reference)->as.cte.query));
    entry->cte = node->kind == SQLPARSER_CTE;
    entry->derived = node->kind == SQLPARSER_TABLE && !reference;
    entry->lateral=entry->derived&&node->as.table.lateral;
    if(entry->lateral) run->lateral_frames=true;
    entry->slot = entry->cte ? SIZE_MAX : (entry->derived || entry->reference) ? run->derived_count++ : run->query_count++;
    if (entry->derived || entry->reference) ((orm_sql_derived_binding *)vec_at(&run->derived,entry->slot))->node = (sqlparser_id)i;
    else if (!entry->cte && !entry->self) ((orm_sql_expr_query_binding *)vec_at(&run->bindings,entry->slot))->node = (sqlparser_id)i;
    ++count;
  }
  run->first = SIZE_MAX;
  for (size_t i = 0; i < run->count; ++i) {
    dependency_node *node = vec_at(&run->nodes,i);
    const sqlparser_node *ast = sqlparser_get_node(scope->document,node->ast);
    size_t shortest = SIZE_MAX;
    status = dependency_steps(run->budget,run->count,error); if (status != TURBODB_STATUS_OK) return status;
    for (size_t j = 0; j < run->count; ++j) {
      const dependency_node *candidate = vec_at_const(&run->nodes,j);
      const sqlparser_node *body = sqlparser_get_node(scope->document,candidate->query_ast);
      if (i != j && dependency_inside(ast,body) && body->span.length < shortest) {
        node->parent = j; shortest = body->span.length;
      }
    }
    size_t *link = node->parent == SIZE_MAX ? &run->first : &((dependency_node *)vec_at(&run->nodes,node->parent))->child;
    while (*link != SIZE_MAX) {
      status = dependency_steps(run->budget,1,error); if (status != TURBODB_STATUS_OK) return status;
      dependency_node *sibling = vec_at(&run->nodes,*link);
      if (sqlparser_get_node(scope->document,sibling->ast)->span.offset > ast->span.offset) break;
      link = &sibling->sibling;
    }
    node->sibling = *link; *link = i;
  }
  for (size_t i = 0; i < run->count; ++i) {
    dependency_node *node = vec_at(&run->nodes,i);
    if(!node->reference) {
      size_t parent=node->parent; node->lexical=true;
      const dependency_node *outermost=node;
      while(parent!=SIZE_MAX) {
        status=dependency_steps(run->budget,1,error);
        if(status!=TURBODB_STATUS_OK) return status;
        const dependency_node *ancestor=vec_at_const(&run->nodes,parent);
        outermost=ancestor;
        parent=ancestor->parent;
      }
      node->root_capture=!outermost->derived&&!outermost->cte&&!outermost->reference;
      if(node->lateral||(!node->derived&&!node->cte)) {
        const sqlparser_node *ast=sqlparser_get_node(scope->document,node->ast);
        const dependency_node *ancestor=node->parent!=SIZE_MAX?
            vec_at_const(&run->nodes,node->parent):NULL;
        const sqlparser_node *body=ancestor?
            sqlparser_get_node(scope->document,ancestor->query_ast):root;
        size_t shortest=SIZE_MAX;
        status=dependency_steps(run->budget,nodes,error);
        if(status!=TURBODB_STATUS_OK) return status;
        for(size_t j=1;j<=nodes;++j) {
          const sqlparser_node *select=sqlparser_get_node(scope->document,(sqlparser_id)j);
          if(select->kind==SQLPARSER_SELECT&&dependency_inside(select,body)&&
              dependency_inside(ast,select)&&select->span.length<shortest) {
            shortest=select->span.length; node->lexical_select=(sqlparser_id)j;
            node->lexical_from=select->as.select.from;
          }
        }
        if(node->lateral) {
          if(!node->lexical_select)
            return dependency_error(error,TURBODB_STATUS_SQL_ERROR,"LATERAL requires an enclosing SELECT FROM");
          orm_sql_query_scope input=*scope; input.root=node->lexical_select;
          status=orm_sql_from_lateral_prefixes_at(&input,node->ast,&node->lateral_prefixes,
              &node->lateral_prefix_bytes,error);
          if(status!=TURBODB_STATUS_OK) return status;
        }
      }
    }
    node->replay = !node->self && (node->derived || node->reference);
    size_t depth = 1, current = i;
    while (current != SIZE_MAX) {
      status = dependency_steps(run->budget,1,error); if (status != TURBODB_STATUS_OK) return status;
      if (++depth > scope->max_depth || depth-1 > run->count)
        return dependency_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"scalar query dependency depth exceeded");
      const dependency_node *ancestor = vec_at_const(&run->nodes,current);
      if (current != i && !ancestor->derived) node->replay = false;
      current = ancestor->parent;
    }
  }
  for (size_t i = 0; i < run->count; ++i) {
    dependency_node *node = vec_at(&run->nodes,i);
    if (!node->reference) continue;
    node->definition = SIZE_MAX;
    status = dependency_steps(run->budget,run->count,error); if (status != TURBODB_STATUS_OK) return status;
    for (size_t j = 0; j < run->count; ++j)
      if (((const dependency_node *)vec_at_const(&run->nodes,j))->ast == node->definition_ast) { node->definition = j; break; }
    if (node->definition == SIZE_MAX) return dependency_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"missing CTE definition dependency");
    if(node->self) ((dependency_node *)vec_at(&run->nodes,node->definition))->recursive=true;
  }
  const sqlparser_node *root_source=sqlparser_get_node(scope->document,
      dependency_root_from(scope));
  size_t root_inputs=0;
  if(root_source) {
    status=dependency_steps(run->budget,run->count,error);
    if(status!=TURBODB_STATUS_OK) return status;
    for(size_t i=0;i<run->count;++i) {
      dependency_node *source=vec_at(&run->nodes,i);
      const sqlparser_node *source_ast=sqlparser_get_node(scope->document,source->ast);
      source->root_input=(source->derived||source->reference)&&
          dependency_inside(source_ast,root_source);
      if(source->root_input) ++root_inputs;
    }
  }
  if(recursive) {
    status=dependency_recursive_prepare(scope,references,run,error);
    if(status!=TURBODB_STATUS_OK) return status;
  }
  /* At most N passes; each inherited frame scans N possible sources. LATERAL
   * may scan up to depth disjoint prefixes per ancestor: O(N^3 * depth^2),
   * versus O(N^3 * depth) without LATERAL. All checks charge the step budget. */
  size_t ordered = 0;
  while (ordered < run->count) {
    const size_t before = ordered;
    bool root_ready=true; size_t root_depth=0;
    if(root_inputs) {
      status=dependency_steps(run->budget,run->count,error);
      if(status!=TURBODB_STATUS_OK) return status;
      for(size_t i=0;i<run->count;++i) {
        const dependency_node *source=vec_at_const(&run->nodes,i);
        if(!source->root_input) continue;
        if(!source->ordered) root_ready=false;
        if(source->ordered&&root_depth<source->depth) root_depth=source->depth;
      }
    }
    for (size_t i = 0; i < run->count; ++i) {
      dependency_node *node = vec_at(&run->nodes,i);
      status = dependency_steps(run->budget,1,error); if (status != TURBODB_STATUS_OK) return status;
      if (node->ordered) continue;
      bool ready = true; size_t depth = 1;
      if(node->recursive_owner!=SIZE_MAX&&node->recursive_part==ORM_SQL_CTE_RECURSIVE_PART) {
        status=dependency_steps(run->budget,run->count,error);
        if(status!=TURBODB_STATUS_OK) return status;
        for(size_t j=0;j<run->count;++j) {
          const dependency_node *seed=vec_at_const(&run->nodes,j);
          if(seed->recursive_owner!=node->recursive_owner||seed->recursive_part!=ORM_SQL_CTE_INITIAL_PART) continue;
          if(!seed->ordered) ready=false;
          if(seed->ordered&&depth<=seed->depth) depth=seed->depth+1;
        }
      }
      const dependency_node *frame=node->lexical&&(node->lateral||(!node->derived&&!node->cte))?node:NULL;
      if(!frame&&(node->derived||node->cte)) {
        for(size_t parent=node->parent;parent!=SIZE_MAX;) {
          status=dependency_steps(run->budget,1,error);
          if(status!=TURBODB_STATUS_OK) return status;
          const dependency_node *ancestor=vec_at_const(&run->nodes,parent);
          if(ancestor->lexical&&(ancestor->lateral||(!ancestor->derived&&!ancestor->cte))) { frame=ancestor; break; }
          parent=ancestor->parent;
        }
      }
      const dependency_node *recursive_owner=node->recursive_owner==SIZE_MAX?NULL:
          vec_at_const(&run->nodes,node->recursive_owner);
      if(!frame&&recursive_owner) {
        for(size_t parent=recursive_owner->parent;parent!=SIZE_MAX;) {
          status=dependency_steps(run->budget,1,error);
          if(status!=TURBODB_STATUS_OK) return status;
          const dependency_node *ancestor=vec_at_const(&run->nodes,parent);
          if(ancestor->lexical&&(ancestor->lateral||(!ancestor->derived&&!ancestor->cte))) { frame=ancestor; break; }
          parent=ancestor->parent;
        }
      }
      if(root_inputs&&(node->root_capture||(node->parent==SIZE_MAX&&!node->derived&&
          !node->cte&&!node->reference)||(recursive_owner&&recursive_owner->root_capture))) {
        if(!root_ready) ready=false;
        if(root_ready&&depth<=root_depth) depth=root_depth+1;
      }
      /* Frame preparation walks all ancestors. Their FROM metadata, including
       * enclosing recursive frontiers, must publish before a nested seed binds.
       * Definition frames inherit only ancestors, excluding sibling inputs. */
      for(const dependency_node *enclosing=frame;enclosing;) {
        if(enclosing->lateral) {
          for(size_t prefix=0;prefix<vec_size(&enclosing->lateral_prefixes);++prefix) {
            status=dependency_source_order(scope,run,
                *(const sqlparser_id *)vec_at_const(&enclosing->lateral_prefixes,prefix),&ready,&depth,error);
            if(status!=TURBODB_STATUS_OK) return status;
          }
        }
        if(enclosing->lexical&&!enclosing->derived&&!enclosing->cte&&enclosing->lexical_from) {
          status=dependency_source_order(scope,run,enclosing->lexical_from,&ready,&depth,error);
          if(status!=TURBODB_STATUS_OK) return status;
        }
        status=dependency_steps(run->budget,1,error);
        if(status!=TURBODB_STATUS_OK) return status;
        enclosing=enclosing->parent==SIZE_MAX?NULL:vec_at_const(&run->nodes,enclosing->parent);
      }
      for (size_t child = node->child; child != SIZE_MAX;) {
        const dependency_node *input = vec_at_const(&run->nodes,child);
        status = dependency_steps(run->budget,1,error); if (status != TURBODB_STATUS_OK) return status;
        if (!input->ordered) ready = false;
        if (input->depth >= scope->max_depth) return dependency_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"query dependency depth exceeded");
        if (depth <= input->depth) depth = input->depth+1;
        child = input->sibling;
      }
      if (node->reference && !node->self) {
        const dependency_node *definition = vec_at_const(&run->nodes,node->definition);
        if (!definition->ordered) ready = false;
        if (definition->depth >= scope->max_depth) return dependency_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"CTE dependency depth exceeded");
        if (depth <= definition->depth) depth = definition->depth+1;
      }
      if (!ready) continue;
      if (depth >= scope->max_depth) return dependency_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"query dependency depth exceeded");
      node->ordered = true; node->depth = depth;
      *(size_t *)vec_at(&run->order,ordered++) = i;
    }
    if (ordered == before) return dependency_error(error,TURBODB_STATUS_UNSUPPORTED,"cyclic CTE dependencies are not supported");
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t dependency_root_reference_scope(
    const orm_sql_query_scope *scope,const dependency_node *reference,
    const dependency_node *definition,orm_sql_query_scope *out,
    turbodb_error_t *error) {
  const sqlparser_id root_from=dependency_root_from(scope);
  if(!root_from||reference->ast!=root_from) return TURBODB_STATUS_OK;
  const sqlparser_node *table=sqlparser_get_node(scope->document,root_from);
  if(!table||table->kind!=SQLPARSER_TABLE)
    return dependency_error(error,TURBODB_STATUS_INVALID_STATE,
        "root CTE reference is not a table source");
  const sqlparser_id qualifier=table->as.table.alias?
      table->as.table.alias:table->as.table.name;
  vstr name={0}; const char *reason=NULL;
  const turbodb_status_t status=orm_sql_name_node(scope->document,qualifier,&name,&reason);
  if(status!=TURBODB_STATUS_OK) return dependency_error(error,status,reason);
  out->dependency_schema=&definition->schema;
  out->dependency_qualifier=name;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t dependency_from_rewind(void *context,turbodb_error_t *error);
static turbodb_status_t dependency_lateral_open(void *context,const turbodb_value_t *row,size_t count,turbodb_error_t *error);
static turbodb_status_t dependency_lateral_close(void *context,turbodb_error_t *error);
static turbodb_status_t dependency_table_schema(const orm_sql_query_scope *scope,dependency_node *node,
    const orm_sql_select *plan,turbodb_error_t *error) {
  const sqlparser_node *ast=sqlparser_get_node(scope->document,node->ast);
  if(ast->as.table.column_aliases.count) {
    const turbodb_status_t status=orm_sql_derived_schema_bind(scope,node->ast,plan,&node->cte_schema,error);
    if(status==TURBODB_STATUS_OK) node->schema=node->cte_schema.schema;
    return status;
  }
  vstr name; const char *reason=NULL;
  turbodb_status_t status=orm_sql_name_node(scope->document,ast->as.table.alias,&name,&reason);
  if(status!=TURBODB_STATUS_OK) return dependency_error(error,status,reason);
  memcpy(node->name,name.data,name.len); node->name[name.len]=0;
  const size_t count=vec_size(&plan->columns);
  status=dependency_steps(scope->budget,count,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&node->columns,count,sizeof(orm_sql_schema_column),
      _Alignof(orm_sql_schema_column),scope->budget,&node->column_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&node->types,count,sizeof(orm_sql_type),
      _Alignof(orm_sql_type),scope->budget,&node->type_bytes,error);
  for(size_t i=0;status==TURBODB_STATUS_OK && i<count;++i) {
    const orm_sql_select_column *column=orm_tidesdb_sql_select_column_at(plan,i);
    *(orm_sql_schema_column *)vec_at(&node->columns,i)=(orm_sql_schema_column){vstr_from_cstr(column->name),column->type};
    *(orm_sql_type *)vec_at(&node->types,i)=column->type;
  }
  if(status==TURBODB_STATUS_OK)
    node->schema=(orm_sql_table_schema){vstr_from_cstr(node->name),vec_data_const(&node->columns),count};
  return status;
}
static turbodb_status_t dependency_derived(const orm_sql_query_scope *scope, dependency_node *node,
    orm_sql_dependencies *out, turbodb_error_t *error) {
  const sqlparser_node *ast = sqlparser_get_node(scope->document,node->ast);
  const orm_sql_select *plan = orm_sql_runtime_plan(&node->query);
  if (node->cte) {
    turbodb_status_t status = orm_sql_cte_schema_bind(scope,node->ast,plan,false,&node->cte_schema,error);
    if (status != TURBODB_STATUS_OK) return status;
    node->schema = node->cte_schema.schema;
    node->source = (orm_sql_row_source){scope->budget,vec_data_const(&node->cte_schema.types),
        node->schema.count,node,dependency_pull,false};
    return out->describe ? TURBODB_STATUS_OK : orm_sql_cte_store_open(&node->source,&node->cache,error);
  }
  if (!ast->as.table.alias) return dependency_error(error,TURBODB_STATUS_SQL_ERROR,"derived table requires an alias");
  const turbodb_status_t status=dependency_table_schema(scope,node,plan,error);
  if (status == TURBODB_STATUS_OK) {
    node->source=(orm_sql_row_source){scope->budget,vec_data_const(node->cte_schema.budget?
        &node->cte_schema.types:&node->types),node->schema.count,node,dependency_pull,false};
    node->input=(orm_sql_from_input){.source=&node->source,.context=node,.rewind=dependency_from_rewind};
    if(node->lateral) {
      const orm_sql_type *types=vec_data_const(&plan->parameter_types);
      node->input.rewind=NULL;
      node->input.binding=(orm_sql_join_right_binding){node,dependency_lateral_open,dependency_lateral_close};
      node->input.capture_count=node->outer_count;
      node->input.capture_types=types?types+node->marker_count:NULL;
    }
    *(orm_sql_derived_binding *)vec_at(&out->derived,node->slot) =
        (orm_sql_derived_binding){node->ast,&node->schema,out->describe?NULL:&node->source,
            out->describe?NULL:&node->input};
  }
  return status;
}
static turbodb_status_t dependency_execution_close(orm_sql_dependencies *run,size_t round_owner,turbodb_error_t *error);
static turbodb_status_t dependency_execution_open(orm_sql_dependencies *run,const turbodb_value_t *parameters,
    size_t count,size_t round_owner,turbodb_error_t *error);
static turbodb_status_t dependency_round_close(void *context,turbodb_error_t *error) {
  dependency_node *node=context;
  return dependency_execution_close(node->graph,node->index,error);
}
static turbodb_status_t dependency_round_open(void *context,const turbodb_value_t *parameters,size_t count,turbodb_error_t *error) {
  dependency_node *node=context;
  /* Also closes the construction-time runs before the first recursive round. */
  turbodb_status_t status=dependency_round_close(context,error);
  if(status==TURBODB_STATUS_OK) status=dependency_execution_open(node->graph,parameters,count,node->index,error);
  return status;
}
static turbodb_status_t dependency_recursive_open(const orm_sql_query_scope *scope,const vec_t *references,
    orm_sql_catalog_store *owner,const turbodb_value_t *parameters,const orm_sql_expr_query_sources *sources,
    dependency_node *node,turbodb_error_t *error) {
  orm_sql_cte_query *query=vec_at(&node->recursion,0);
  turbodb_status_t status=orm_sql_cte_query_member_open(scope,node->ast,references,owner,parameters,sources,query,error);
  if(status==TURBODB_STATUS_OK) {
    node->schema=query->schema.schema;
    node->correlated=query->initial.plan->correlated||query->recursive.plan->correlated;
  }
  return status;
}
static bool dependency_descendant(const orm_sql_dependencies *run,size_t index,size_t ancestor) {
  for(size_t steps=0;index!=SIZE_MAX&&steps<run->count;++steps) {
    if(index==ancestor) return true;
    index=((const dependency_node *)vec_at_const(&run->nodes,index))->parent;
  }
  return false;
}
static turbodb_status_t dependency_plain_table(const orm_sql_query_scope *scope,
    const dependency_node *owner,sqlparser_id id,const sqlparser_node *table,
    bool *out,turbodb_error_t *error) {
  *out=false;
  if(!table||table->kind!=SQLPARSER_TABLE||table->as.table.group||
      table->as.table.arguments.count||table->as.table.indexed_by||
      table->as.table.table_function||table->as.table.not_indexed) return TURBODB_STATUS_OK;
  turbodb_status_t status=dependency_steps(scope->budget,owner->graph->count,error);
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<owner->graph->count;++i) {
    const dependency_node *source=vec_at_const(&owner->graph->nodes,i);
    if(source->ast!=id) continue;
    if((source->derived||source->reference)&&
        dependency_descendant(owner->graph,i,owner->index)) *out=true;
    return status;
  }
  if(status==TURBODB_STATUS_OK&&!table->as.table.query) *out=true;
  return status;
}
static turbodb_status_t dependency_plain_source(const orm_sql_query_scope *scope,
    const dependency_node *owner,const sqlparser_node *select,bool *out,
    turbodb_error_t *error) {
  *out=false;
  if(!select->as.select.from) { *out=true; return TURBODB_STATUS_OK; }
  const sqlparser_node *from=sqlparser_get_node(scope->document,select->as.select.from);
  if(!from) return TURBODB_STATUS_OK;
  if(from->kind==SQLPARSER_TABLE)
    return dependency_plain_table(scope,owner,select->as.select.from,from,out,error);
  if(from->kind!=SQLPARSER_JOIN) return TURBODB_STATUS_OK;
  const size_t count=sqlparser_node_count(scope->document);
  turbodb_status_t status=dependency_steps(scope->budget,count,error);
  for(size_t i=1;i<=count;++i) {
    const sqlparser_node *table=sqlparser_get_node(scope->document,(sqlparser_id)i);
    if(!table||table->kind!=SQLPARSER_TABLE||!dependency_inside(table,from)) continue;
    bool plain=false;
    if(status==TURBODB_STATUS_OK)
      status=dependency_plain_table(scope,owner,(sqlparser_id)i,table,&plain,error);
    if(status!=TURBODB_STATUS_OK||!plain) return status;
  }
  *out=true;
  return status;
}
/* Admit only query blocks whose complete execution can be reopened from the
 * marker-plus-outer-row snapshot. A derived source is admissible only when its
 * dependency node is owned by the query being validated. CTE reader lifetimes
 * additionally follow their resolved definition's materialization owner. */
static turbodb_status_t dependency_correlation_shape(const orm_sql_query_scope *scope,
    const dependency_node *owner,sqlparser_id root,bool *out,turbodb_error_t *error) {
  *out=false;
  const size_t capacity=sqlparser_node_count(scope->document);
  vec_t stack={0}; size_t bytes=0,pending=0;
  turbodb_status_t status=orm_sql_work_zero(&stack,capacity,sizeof(dependency_query_visit),
      _Alignof(dependency_query_visit),scope->budget,&bytes,error);
  if(status==TURBODB_STATUS_OK)
    *(dependency_query_visit *)vec_at(&stack,pending++)=(dependency_query_visit){root,1};
  bool valid=status==TURBODB_STATUS_OK;
  while(status==TURBODB_STATUS_OK&&valid&&pending) {
    const dependency_query_visit visit=*(const dependency_query_visit *)vec_at_const(&stack,--pending);
    status=dependency_steps(scope->budget,1,error);
    if(status!=TURBODB_STATUS_OK) break;
    if(visit.depth>scope->max_depth) {
      status=dependency_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"correlated compound query depth exceeded");
      break;
    }
    const sqlparser_node *query=sqlparser_get_node(scope->document,visit.ast);
    if(!query) { valid=false; break; }
    if(query->kind==SQLPARSER_SELECT) {
      status=dependency_plain_source(scope,owner,query,&valid,error); continue;
    }
    if(query->kind==SQLPARSER_WITH) {
      if(pending==capacity||visit.depth==SIZE_MAX) {
        status=dependency_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,
            "correlated WITH traversal overflow");
        break;
      }
      *(dependency_query_visit *)vec_at(&stack,pending++)=
          (dependency_query_visit){query->as.with.body,visit.depth+1};
      continue;
    }
    const bool group=query->kind==SQLPARSER_QUERY_GROUP;
    if(!group&&(query->kind!=SQLPARSER_UNION||query->as.compound.kind<SQLPARSER_COMPOUND_UNION||
        query->as.compound.kind>SQLPARSER_COMPOUND_EXCEPT)) {
      valid=false; break;
    }
    const size_t children=group?1:2;
    if(capacity<children||pending>capacity-children||visit.depth==SIZE_MAX) {
      status=dependency_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"correlated compound traversal overflow");
      break;
    }
    if(!group)
      *(dependency_query_visit *)vec_at(&stack,pending++)=
          (dependency_query_visit){query->as.compound.right,visit.depth+1};
    *(dependency_query_visit *)vec_at(&stack,pending++)=(dependency_query_visit){
        group?query->as.query_group.query:query->as.compound.left,visit.depth+1};
  }
  const turbodb_status_t released=orm_sql_work_release(&stack,bytes,scope->budget,
      status==TURBODB_STATUS_OK?error:NULL);
  if(status==TURBODB_STATUS_OK) status=released;
  if(status==TURBODB_STATUS_OK) *out=valid;
  return status;
}
static turbodb_status_t dependency_correlation_candidate(const orm_sql_query_scope *scope,
    const dependency_node *node,bool *out,turbodb_error_t *error) {
  *out=false;
  if(!scope->dependency_schema||node->reference) return TURBODB_STATUS_OK;
  bool eligible=node->lexical||
      (node->parent==SIZE_MAX&&!node->derived&&!node->cte);
  if(!eligible) return TURBODB_STATUS_OK;
  const sqlparser_node *query=sqlparser_get_node(scope->document,node->query_ast);
  if(!query) return TURBODB_STATUS_OK;
  bool supported=false;
  turbodb_status_t status=query->kind==SQLPARSER_SELECT?
      dependency_plain_source(scope,node,query,&supported,error):
      dependency_correlation_shape(scope,node,node->query_ast,&supported,error);
  if(status!=TURBODB_STATUS_OK||!supported) return status;
  const size_t count=sqlparser_node_count(scope->document);
  status=dependency_steps(scope->budget,count,error);
  for(size_t i=1;status==TURBODB_STATUS_OK&&i<=count;++i) {
    const sqlparser_node *name=sqlparser_get_node(scope->document,(sqlparser_id)i);
    if(!name||name->kind!=SQLPARSER_NAME||!dependency_inside(name,query)) continue;
    vstr text={sqlparser_text(scope->document,name->span),name->span.length},part={0};
    const char *reason=NULL;
    if(orm_sql_name_part(&text,&part,&reason)!=TURBODB_STATUS_OK) continue;
    if(text.len&&text.data[0]=='.') {
      if(part.len==scope->dependency_qualifier.len&&
          !memcmp(part.data,scope->dependency_qualifier.data,part.len)) *out=true;
      for(size_t column=0;status==TURBODB_STATUS_OK&&!*out&&column<scope->dependency_schema->count;++column) {
        status=dependency_steps(scope->budget,1,error);
        const vstr qualifier=scope->dependency_schema->columns[column].qualifier;
        if(status==TURBODB_STATUS_OK&&part.len==qualifier.len&&qualifier.len&&
            !memcmp(part.data,qualifier.data,part.len)) *out=true;
      }
      continue;
    }
    if(text.len) continue;
    for(size_t column=0;status==TURBODB_STATUS_OK&&!*out&&column<scope->dependency_schema->count;++column) {
      status=dependency_steps(scope->budget,1,error);
      const vstr outer=scope->dependency_schema->columns[column].name;
      if(status==TURBODB_STATUS_OK&&part.len==outer.len&&!memcmp(part.data,outer.data,part.len)) *out=true;
    }
  }
  return status;
}
static turbodb_value_t dependency_default(orm_sql_type type) {
  if(type.nullable) return turbodb_null();
  switch(type.kind) {
    case TURBODB_VALUE_INT64: return turbodb_i64(0);
    case TURBODB_VALUE_UINT64: return turbodb_u64(0);
    case TURBODB_VALUE_DOUBLE: return turbodb_f64(0);
    case TURBODB_VALUE_BOOLEAN: return turbodb_bool(false);
    case TURBODB_VALUE_TEXT: return turbodb_text("");
    case TURBODB_VALUE_BLOB: return turbodb_blob(NULL,0);
    default: return turbodb_null();
  }
}
static turbodb_status_t dependency_arguments(const orm_sql_query_scope *scope,const turbodb_value_t *parameters,
    dependency_node *node,turbodb_error_t *error) {
  node->marker_count=scope->parameter_count;
  node->outer_count=scope->dependency_schema?scope->dependency_schema->count:0;
  if(node->graph->binding_only) return TURBODB_STATUS_OK;
  turbodb_status_t status=node->marker_count?orm_sql_snapshot_copy(&node->arguments,parameters,node->marker_count,
      node->outer_count,scope->budget,error):orm_sql_work_zero(&node->arguments.values,node->outer_count,
      sizeof(turbodb_value_t),_Alignof(turbodb_value_t),scope->budget,&node->arguments.bytes,error);
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<node->outer_count;++i)
    *(turbodb_value_t *)vec_at(&node->arguments.values,node->marker_count+i)=
        dependency_default(scope->dependency_schema->columns[i].type);
  return status;
}
static turbodb_status_t dependency_correlated_query_child(dependency_node *node,
    bool *out,turbodb_error_t *error) {
  *out=false;
  for(size_t child=node->child;child!=SIZE_MAX;) {
    const dependency_node *input=vec_at_const(&node->graph->nodes,child);
    const turbodb_status_t status=dependency_steps(node->graph->budget,1,error);
    if(status!=TURBODB_STATUS_OK) return status;
    if(node->frame_ready&&input->reference) {
      const dependency_node *definition=vec_at_const(&node->graph->nodes,input->definition);
      if(definition->frame_ready&&definition->correlated) {
        if(definition->frame.count>node->frame.count)
          return dependency_error(error,TURBODB_STATUS_INVALID_STATE,"CTE capture frame layout is inconsistent");
        turbodb_status_t charged=dependency_steps(node->graph->budget,definition->frame.count,error);
        if(charged!=TURBODB_STATUS_OK) return charged;
        const size_t offset=node->frame.count-definition->frame.count;
        for(size_t i=0;i<definition->frame.count;++i)
          if(((const dependency_capture *)vec_at_const(&definition->captures,i))->used)
            ((dependency_capture *)vec_at(&node->captures,offset+i))->used=true;
      }
    } else if(node->frame_ready&&input->frame_ready) {
      if(input->inherited_count!=node->frame.count)
        return dependency_error(error,TURBODB_STATUS_INVALID_STATE,"child capture frame layout is inconsistent");
      turbodb_status_t charged=dependency_steps(node->graph->budget,node->frame.count,error);
      if(charged!=TURBODB_STATUS_OK) return charged;
      for(size_t i=0;i<node->frame.count;++i) {
        const dependency_capture *capture=vec_at_const(&input->captures,input->immediate_count+i);
        if(capture->used) ((dependency_capture *)vec_at(&node->captures,i))->used=true;
      }
    } else if(input->correlated) {
      *out=true; return TURBODB_STATUS_OK;
    }
    child=input->sibling;
  }
  if(node->frame_ready) {
    turbodb_status_t charged=dependency_steps(node->graph->budget,node->frame.count,error);
    if(charged!=TURBODB_STATUS_OK) return charged;
    for(size_t i=0;i<node->frame.count;++i)
      if(((const dependency_capture *)vec_at_const(&node->captures,i))->used) *out=true;
  }
  return TURBODB_STATUS_OK;
}
static bool dependency_same_callback(const dependency_node *owner,
    const dependency_node *node) {
  for(size_t parent=node->parent;parent!=SIZE_MAX;) {
    if(parent==owner->index) return true;
    const dependency_node *ancestor=vec_at_const(&owner->graph->nodes,parent);
    if(ancestor->lateral||(!ancestor->derived&&!ancestor->cte&&!ancestor->reference)) return false;
    parent=ancestor->parent;
  }
  return false;
}
static turbodb_status_t dependency_correlated_inputs_close(dependency_node *owner,
    bool prepare,turbodb_error_t *error) {
  orm_sql_dependencies *run=owner->graph;
  for(size_t i=run->prepared;i;--i) {
    dependency_node *node=vec_at(&run->nodes,
        *(const size_t *)vec_at_const(&run->order,i-1));
    turbodb_status_t status=prepare?dependency_steps(run->budget,1,error):TURBODB_STATUS_OK;
    if(status!=TURBODB_STATUS_OK) return status;
    if(node->self) continue;
    const bool same=dependency_same_callback(owner,node);
    const dependency_node *definition=node->reference?
        vec_at_const(&run->nodes,node->definition):NULL;
    const bool owned=definition&&definition->correlated&&dependency_same_callback(owner,definition);
    if(node==owner||node->lateral||(!node->derived&&!node->cte&&!node->reference)||
        (!same&&!owned)) continue;
    /* Stable CTE caches may have consumers in cached nested expressions. Only
     * stores that capture this callback's frame belong to its rebuild lifetime. */
    if(node->cte&&!node->correlated) continue;
    if(prepare) node->correlated_input=true;
    if(node->reference) {
      if(owned) node->callback_reader=true;
      if(!owned&&!node->callback_reader) continue;
      if(node->reader.source.active)
        return dependency_error(error,TURBODB_STATUS_BUSY,
            "close correlated CTE consumers before replay");
      status=orm_sql_cte_reader_close(&node->reader,error);
      if(status!=TURBODB_STATUS_OK) return status;
      continue;
    }
    if(node->cte) {
      if(node->recursive) {
        status=orm_sql_cte_query_execution_close(vec_at(&node->recursion,0),error);
        if(status!=TURBODB_STATUS_OK) return status;
        continue;
      }
      status=orm_sql_cte_store_close(&node->cache,error);
      if(status!=TURBODB_STATUS_OK) return status;
      status=orm_sql_runtime_execution_close(&node->query,error);
      if(status!=TURBODB_STATUS_OK) return status;
      continue;
    }
    if(node->source.active)
      return dependency_error(error,TURBODB_STATUS_BUSY,
          "close correlated derived consumers before replay");
    status=orm_sql_runtime_execution_close(&node->query,error);
    if(status!=TURBODB_STATUS_OK) return status;
    if(run->lateral_frames&&node->derived)
      for(size_t slot=0;slot<node->outer_count;++slot)
        *(turbodb_value_t *)vec_at(&node->arguments.values,node->marker_count+slot)=turbodb_null();
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t dependency_correlated_inputs_open(dependency_node *owner,
    const turbodb_value_t *parameters,size_t count,turbodb_error_t *error) {
  orm_sql_dependencies *run=owner->graph;
  const orm_sql_expr_query_sources sources={vec_data_const(&run->sources),run->query_count,run->evaluation};
  turbodb_status_t status=dependency_steps(run->budget,run->prepared,error);
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<run->prepared;++i) {
    dependency_node *node=vec_at(&run->nodes,
        *(const size_t *)vec_at_const(&run->order,i));
    if(node==owner||node->lateral||!node->correlated_input||
        !dependency_same_callback(owner,node)) continue;
    if(node->reference) {
      if(node->callback_reader) {
        dependency_node *definition=vec_at(&run->nodes,node->definition);
        orm_sql_cte_store *cache=definition->recursive?
            &((orm_sql_cte_query *)vec_at(&definition->recursion,0))->cache:&definition->cache;
        status=orm_sql_cte_reader_open(cache,&node->reader,error);
      } else status=orm_sql_cte_reader_rewind(&node->reader,error);
      continue;
    }
    if(!node->derived&&!node->cte) continue;
    if(node->recursive) {
      orm_sql_cte_query *query=vec_at(&node->recursion,0);
      if(query->parameter_count>count) {
        status=dependency_error(error,TURBODB_STATUS_INVALID_STATE,"recursive capture parameter layout is inconsistent");
        break;
      }
      status=orm_sql_cte_query_execution_open(query,parameters,query->parameter_count,error);
      continue;
    }
    const orm_sql_select *plan=orm_sql_runtime_plan(&node->query);
    const size_t expected=plan?vec_size(&plan->parameter_types):SIZE_MAX;
    if(!plan||expected>count) {
      status=dependency_error(error,TURBODB_STATUS_INVALID_STATE,
          "correlated query input parameter layout is inconsistent");
      break;
    }
    if(node->derived&&run->lateral_frames&&node->outer_count) {
      status=dependency_steps(run->budget,node->outer_count,error);
      if(status!=TURBODB_STATUS_OK) break;
      memcpy((turbodb_value_t *)vec_data(&node->arguments.values)+node->marker_count,
          parameters+node->marker_count,node->outer_count*sizeof(turbodb_value_t));
    }
    status=orm_sql_runtime_execution_open(&node->query,parameters,expected,
        &sources,error);
    if(status==TURBODB_STATUS_OK&&node->cte)
      status=orm_sql_cte_store_open(&node->source,&node->cache,error);
  }
  if(status!=TURBODB_STATUS_OK)
    (void)dependency_correlated_inputs_close(owner,false,NULL);
  return status;
}
static turbodb_status_t dependency_provider_stop(dependency_node *node,turbodb_error_t *error) {
  turbodb_status_t status=orm_sql_runtime_execution_close(&node->query,error);
  if(status==TURBODB_STATUS_OK) status=dependency_correlated_inputs_close(node,false,error);
  if(status==TURBODB_STATUS_OK) {
    for(size_t i=0;i<node->outer_count;++i)
      *(turbodb_value_t *)vec_at(&node->arguments.values,node->marker_count+i)=turbodb_null();
    node->lateral_bound=false;
  }
  return status;
}
static turbodb_status_t dependency_lateral_close(void *context,turbodb_error_t *error) {
  dependency_node *node=context;
  if(node->source.active||node->lateral_evaluating)
    return dependency_error(error,TURBODB_STATUS_BUSY,"LATERAL provider has an active consumer");
  node->lateral_evaluating=true;
  const turbodb_status_t status=dependency_provider_stop(node,error);
  node->lateral_evaluating=false;
  return status;
}
static turbodb_status_t dependency_lateral_open(void *context,const turbodb_value_t *row,size_t count,turbodb_error_t *error) {
  dependency_node *node=context;
  if(node->source.active||node->lateral_bound||node->lateral_evaluating||!node->query.execution_closed)
    return dependency_error(error,TURBODB_STATUS_BUSY,"close LATERAL provider before reopening");
  if(count!=node->outer_count||(count&&!row))
    return dependency_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"LATERAL provider capture width differs");
  if(node->query.execution_failure.status!=TURBODB_STATUS_OK)
    return dependency_error(error,node->query.execution_failure.status,node->query.execution_failure.message);
  /* Refuse pre-existing input consumers before borrowing the current prefix. */
  node->lateral_evaluating=true;
  turbodb_status_t status=dependency_correlated_inputs_close(node,false,error);
  node->lateral_evaluating=false;
  if(status!=TURBODB_STATUS_OK) return status;
  status=dependency_steps(node->graph->budget,count,error);
  if(status!=TURBODB_STATUS_OK) return status;
  node->lateral_evaluating=true;
  if(count) memcpy((turbodb_value_t *)vec_data(&node->arguments.values)+node->marker_count,row,count*sizeof(turbodb_value_t));
  const turbodb_value_t *arguments=vec_data_const(&node->arguments.values);
  const size_t width=node->marker_count+node->outer_count;
  const orm_sql_expr_query_sources sources={vec_data_const(&node->graph->sources),node->graph->query_count,node->graph->evaluation};
  status=dependency_correlated_inputs_open(node,arguments,width,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_runtime_execution_open(&node->query,arguments,width,&sources,error);
  if(status==TURBODB_STATUS_OK) node->lateral_bound=true;
  else {
    const turbodb_status_t released=dependency_provider_stop(node,NULL);
    if(released!=TURBODB_STATUS_OK) {
      node->query.owner->failed=true;
      status=dependency_error(error,released,"LATERAL activation cleanup failed; rollback required");
    }
  }
  node->lateral_evaluating=false;
  return status;
}
static turbodb_status_t dependency_from_rewind(void *context,turbodb_error_t *error) {
  dependency_node *node=context;
  if(node->reference) return orm_sql_cte_reader_rewind(&node->reader,error);
  if(node->source.active)
    return dependency_error(error,TURBODB_STATUS_BUSY,"derived FROM provider has an active consumer");
  turbodb_status_t status=orm_sql_runtime_execution_close(&node->query,error);
  if(status==TURBODB_STATUS_OK) status=dependency_correlated_inputs_close(node,false,error);
  const turbodb_value_t *arguments=vec_data_const(&node->arguments.values);
  const size_t count=node->marker_count+node->outer_count;
  const orm_sql_expr_query_sources sources={vec_data_const(&node->graph->sources),node->graph->query_count,node->graph->evaluation};
  if(status==TURBODB_STATUS_OK) status=dependency_correlated_inputs_open(node,arguments,count,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_runtime_execution_open(&node->query,arguments,count,&sources,error);
  return status;
}
static turbodb_status_t dependency_correlated_eval(void *context,const turbodb_value_t *probe,
    const orm_sql_predicate *comparison,const turbodb_value_t *outer_row,size_t outer_count,
    turbodb_value_t *out,turbodb_error_t *error) {
  dependency_node *node=context;
  const size_t expected=node&&node->frame_ready?node->callback_count:node?node->outer_count:0;
  if(!node||!node->correlated||!out||outer_count!=expected||(outer_count&&!outer_row))
    return dependency_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid correlated subquery row");
  if(!node->query.execution_closed)
    return dependency_error(error,TURBODB_STATUS_BUSY,"correlated subquery execution is already open");
  const size_t immediate=node->frame_ready?node->immediate_count:outer_count;
  const orm_sql_select *plan=orm_sql_runtime_plan(&node->query);
  if(node->frame_ready&&node->parent!=SIZE_MAX) {
    const dependency_node *parent=vec_at_const(&node->graph->nodes,node->parent);
    const orm_sql_cte_query *recursive=parent->recursive?vec_at_const(&parent->recursion,0):NULL;
    const bool active=recursive?recursive->complete&&!recursive->execution_closed:
        parent->query.owner&&!parent->query.execution_closed;
    if(!active) return dependency_error(error,TURBODB_STATUS_INVALID_STATE,"lexical parent frame is not active");
  }
  if(node->frame_ready&&node->inherited_count) {
    const dependency_node *parent=vec_at_const(&node->graph->nodes,node->parent);
    const dependency_node *arguments=parent;
    while(!arguments->lateral&&(arguments->derived||arguments->cte)) {
      const turbodb_status_t charged=dependency_steps(node->graph->budget,1,error);
      if(charged!=TURBODB_STATUS_OK) return charged;
      if(arguments->parent==SIZE_MAX) { arguments=NULL; break; }
      arguments=vec_at_const(&node->graph->nodes,arguments->parent);
    }
    const turbodb_status_t charged=dependency_steps(node->graph->budget,node->inherited_count,error);
    if(charged!=TURBODB_STATUS_OK) return charged;
    for(size_t i=0;i<node->inherited_count;++i) {
      const dependency_capture *capture=vec_at_const(&node->captures,immediate+i);
      if(capture->used&&(!arguments||arguments->query.execution_closed||
          arguments->outer_count!=node->inherited_count||
          vec_size(&arguments->arguments.values)<arguments->marker_count+arguments->outer_count))
        return dependency_error(error,TURBODB_STATUS_INVALID_STATE,"lexical ancestor arguments are not active");
    }
    for(size_t i=0;i<node->inherited_count;++i) {
      const dependency_capture *capture=vec_at_const(&node->captures,immediate+i);
      const orm_sql_type *type=vec_at_const(&plan->parameter_types,node->marker_count+immediate+i);
      *(turbodb_value_t *)vec_at(&node->arguments.values,node->marker_count+immediate+i)=
          capture->used?*(const turbodb_value_t *)vec_at_const(&arguments->arguments.values,arguments->marker_count+i):
          dependency_default(*type);
    }
  }
  for(size_t i=0;i<immediate;++i) {
    const size_t slot=node->frame_ready?
        ((const dependency_capture *)vec_at_const(&node->captures,i))->slot:i;
    const orm_sql_type *type=vec_at_const(&plan->parameter_types,node->marker_count+i);
    *(turbodb_value_t *)vec_at(&node->arguments.values,node->marker_count+i)=
        slot==SIZE_MAX?dependency_default(*type):outer_row[slot];
  }
  const orm_sql_expr_query_sources sources={vec_data_const(&node->graph->sources),node->graph->query_count,node->graph->evaluation};
  const turbodb_value_t *arguments=vec_data_const(&node->arguments.values);
  const size_t argument_count=node->marker_count+node->outer_count;
  turbodb_status_t status=dependency_correlated_inputs_open(node,arguments,argument_count,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_runtime_execution_open(&node->query,arguments,
      argument_count,&sources,error);
  orm_sql_subquery run={0};
  if(status==TURBODB_STATUS_OK) status=node->subquery_kind==ORM_SQL_SUBQUERY_IN||node->subquery_kind==ORM_SQL_SUBQUERY_NOT_IN?
      orm_sql_subquery_open_set(&node->source,node->subquery_kind,&run,error):
      orm_tidesdb_sql_subquery_open(&node->source,node->subquery_kind,NULL,&run,error);
  turbodb_value_t value=turbodb_null();
  if(status==TURBODB_STATUS_OK) status=run.source.eval(run.source.context,probe,comparison,NULL,0,&value,error);
  if(status==TURBODB_STATUS_OK&&(value.kind==TURBODB_VALUE_TEXT||value.kind==TURBODB_VALUE_BLOB)) {
    turbodb_value_t *copy=NULL;
    status=orm_sql_rows_append(&node->correlated_results,&value,1,0,&copy,error);
    if(status==TURBODB_STATUS_OK) value=*copy;
  }
  const turbodb_status_t released=orm_tidesdb_sql_subquery_close(&run,status==TURBODB_STATUS_OK?error:NULL);
  if(status==TURBODB_STATUS_OK) status=released;
  const turbodb_status_t closed=orm_sql_runtime_execution_close(&node->query,status==TURBODB_STATUS_OK?error:NULL);
  if(status==TURBODB_STATUS_OK) status=closed;
  const turbodb_status_t inputs_closed=dependency_correlated_inputs_close(node,false,
      status==TURBODB_STATUS_OK?error:NULL);
  if(status==TURBODB_STATUS_OK) status=inputs_closed;
  for(size_t i=0;i<node->outer_count;++i)
    *(turbodb_value_t *)vec_at(&node->arguments.values,node->marker_count+i)=turbodb_null();
  if(status==TURBODB_STATUS_OK) *out=value;
  return status;
}
/* The consumer binds last, after every descendant has marked its captures.
 * Copy only layout metadata; the group row remains the sole value source. */
static turbodb_status_t dependency_bind_capture(void *context,const size_t *slots,
    size_t count,size_t row_count,turbodb_error_t *error) {
  dependency_node *node=context;
  if(!node->correlated||!node->frame_ready||!slots) return TURBODB_STATUS_OK;
  if(node->immediate_count&&count!=node->immediate_count)
    return dependency_error(error,TURBODB_STATUS_INVALID_STATE,"group capture input layout is inconsistent");
  turbodb_status_t status=dependency_steps(node->graph->budget,node->immediate_count,error);
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<node->immediate_count;++i) {
    dependency_capture *capture=vec_at(&node->captures,i);
    if(slots[i]==SIZE_MAX&&capture->used)
      return dependency_error(error,TURBODB_STATUS_SQL_ERROR,"correlated column is not a GROUP BY key");
    if(slots[i]!=SIZE_MAX&&slots[i]>=row_count)
      return dependency_error(error,TURBODB_STATUS_INVALID_STATE,"group capture slot is outside the row");
    capture->slot=slots[i];
  }
  if(status==TURBODB_STATUS_OK) node->callback_count=row_count;
  return status;
}
/* Source schemas are independent of SELECT output types. Prepare them from
 * ancestor to descendant while query binding remains inner to outer. This
 * avoids inventing placeholder subquery types or executing a parent to bind
 * its child. Frames own fixed metadata and borrow no business row. */
static turbodb_status_t dependency_frame_schema(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner,dependency_node *node,const orm_sql_table_schema **local,
    vstr *qualifier,turbodb_error_t *error) {
  orm_sql_query_scope input=*scope; input.root=node->lexical_select;
  input.outer_schema=NULL; input.outer_qualifier=(vstr){0};
  if(node->lateral) {
    const turbodb_status_t status=orm_sql_runtime_lateral_schema_open(&input,node->ast,owner,&node->lateral_frame,error);
    if(status==TURBODB_STATUS_OK) *local=&node->lateral_frame.schema;
    return status;
  }
  return node->graph->lateral_frames&&node->lexical_from?
      orm_sql_runtime_subtree_schema_open(&input,node->lexical_from,owner,&node->lexical_source,local,qualifier,error):
      orm_sql_runtime_schema_open(&input,owner,&node->lexical_source,local,qualifier,error);
}
static turbodb_status_t dependency_frames(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner,dependency_node *target,turbodb_error_t *error) {
  while(!target->frame_ready) {
    dependency_node *node=target;
    for(size_t parent=node->parent;parent!=SIZE_MAX;) {
      turbodb_status_t status=dependency_steps(scope->budget,1,error);
      if(status!=TURBODB_STATUS_OK) return status;
      dependency_node *ancestor=vec_at(&target->graph->nodes,parent);
      if(ancestor->frame_ready) break;
      node=ancestor; parent=ancestor->parent;
    }
    const orm_sql_table_schema *local=NULL;
    vstr qualifier={0}; size_t inherited=0;
    turbodb_status_t status=TURBODB_STATUS_OK;
    if(node->lateral) {
      status=dependency_frame_schema(scope,owner,node,&local,&qualifier,error);
      if(node->parent!=SIZE_MAX)
        inherited=((const dependency_node *)vec_at_const(&target->graph->nodes,node->parent))->frame.count;
    } else if(node->derived||node->cte) {
      if(node->parent!=SIZE_MAX)
        inherited=((const dependency_node *)vec_at_const(&target->graph->nodes,node->parent))->frame.count;
    } else if(node->parent==SIZE_MAX) {
      local=scope->dependency_schema; qualifier=scope->dependency_qualifier;
      if(!local&&node->lexical_select) {
        status=dependency_frame_schema(scope,owner,node,&local,&qualifier,error);
      }
    } else {
      if(!node->lexical_select)
        return dependency_error(error,TURBODB_STATUS_UNSUPPORTED,"nested correlation requires an enclosing SELECT frame");
      status=dependency_frame_schema(scope,owner,node,&local,&qualifier,error);
      inherited=((const dependency_node *)vec_at_const(&target->graph->nodes,node->parent))->frame.count;
    }
    if(status!=TURBODB_STATUS_OK) return status;
    node->immediate_count=local?local->count:0;
    node->callback_count=node->immediate_count?node->immediate_count:1;
    node->inherited_count=inherited;
    if(inherited>SIZE_MAX-node->immediate_count)
      return dependency_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"lexical frame width overflow");
    const size_t count=node->immediate_count+inherited;
    if(count>scope->budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
      return dependency_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"lexical frame exceeds plan capacity");
    status=orm_sql_work_zero(&node->frame_columns,count,sizeof(orm_sql_schema_column),
        _Alignof(orm_sql_schema_column),scope->budget,&node->frame_bytes,error);
    if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&node->captures,count,
        sizeof(dependency_capture),_Alignof(dependency_capture),scope->budget,&node->capture_bytes,error);
    if(status==TURBODB_STATUS_OK) status=dependency_steps(scope->budget,count,error);
    for(size_t i=0;status==TURBODB_STATUS_OK&&i<count;++i) {
      orm_sql_schema_column column;
      if(i<node->immediate_count) {
        column=local->columns[i]; column.lexical_depth=0;
        if(!column.qualifier.len) column.qualifier=qualifier;
      } else {
        const dependency_node *parent=vec_at_const(&target->graph->nodes,node->parent);
        column=parent->frame.columns[i-node->immediate_count];
        if(column.lexical_depth==SIZE_MAX)
          return dependency_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"lexical frame depth overflow");
        ++column.lexical_depth;
      }
      dependency_capture *capture=vec_at(&node->captures,i);
      capture->slot=i; column.capture_used=&capture->used;
      *(orm_sql_schema_column *)vec_at(&node->frame_columns,i)=column;
    }
    if(status!=TURBODB_STATUS_OK) return status;
    node->frame=(orm_sql_table_schema){.columns=vec_data_const(&node->frame_columns),.count=count};
    node->frame_ready=true;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t dependency_recursive_seed_prepare(const orm_sql_query_scope *scope,
    const vec_t *references,orm_sql_catalog_store *owner,const turbodb_value_t *parameters,
    const orm_sql_expr_query_sources *sources,uint64_t max_iterations,
    dependency_node *node,turbodb_error_t *error) {
  if(vec_size(&node->recursion)) return TURBODB_STATUS_OK;
  turbodb_status_t status=dependency_frames(scope,owner,node,error);
  orm_sql_query_scope child=*scope;
  child.root=scope->root; child.outer_schema=node->frame.count?&node->frame:NULL;
  child.outer_qualifier=(vstr){0}; child.defer_execution=true;
  if(status==TURBODB_STATUS_OK&&node->frame.count) {
    orm_sql_query_scope correlation=*scope; correlation.dependency_schema=&node->frame;
    status=dependency_arguments(&correlation,parameters,node,error);
    parameters=vec_data_const(&node->arguments.values);
  }
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&node->recursion,1,sizeof(orm_sql_cte_query),
      _Alignof(orm_sql_cte_query),scope->budget,&node->recursion_bytes,error);
  const orm_sql_cte_query_spec spec={.max_iterations=max_iterations,
      .describe=node->graph->describe&&!node->graph->binding_only,.sources=sources,
      .rounds={node,dependency_round_open,dependency_round_close}};
  if(status==TURBODB_STATUS_OK)
    status=orm_sql_cte_query_seed_open(&child,node->ast,references,owner,parameters,&spec,vec_at(&node->recursion,0),error);
  if(status==TURBODB_STATUS_OK) {
    node->schema=((const orm_sql_cte_query *)vec_at_const(&node->recursion,0))->schema.schema;
    status=dependency_steps(scope->budget,node->graph->count,error);
    for(size_t i=0;status==TURBODB_STATUS_OK&&i<node->graph->count;++i) {
      const dependency_node *self=vec_at_const(&node->graph->nodes,i);
      if(self->self&&self->definition==node->index)
        *(orm_sql_derived_binding *)vec_at(&node->graph->derived,self->slot)=
            (orm_sql_derived_binding){self->ast,&node->schema,NULL};
    }
  }
  return status;
}
static turbodb_status_t dependency_open(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner, const turbodb_value_t *parameters, dependency_mode mode,
    uint64_t max_iterations,orm_sql_dependencies *out, turbodb_error_t *error) {
  if (!scope || !scope->document || !scope->root || !scope->max_depth || !owner ||
      scope->budget != owner->budget || !out || out->budget||
      (scope->parameter_count&&(!scope->parameter_types||(!parameters&&mode!=DEPENDENCY_BIND))))
    return dependency_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid scalar dependency owner");
  orm_sql_query_scope effective=*scope;
  const bool describe=mode!=DEPENDENCY_EXECUTE;
  const bool binding_only=mode==DEPENDENCY_BIND;
  effective.binding_only=binding_only;
  scope=&effective;
  size_t count = 0;
  vec_t references = {0}; size_t reference_bytes = 0;
  turbodb_status_t status = max_iterations ? orm_sql_cte_resolve(scope,&references,&reference_bytes,error) :
      orm_sql_cte_bind(scope,&references,&reference_bytes,error);
  if (status == TURBODB_STATUS_OK) status = dependency_count(scope,&references,&count,error);
  if (status != TURBODB_STATUS_OK || !count) {
    const turbodb_status_t released = orm_sql_work_release(&references,reference_bytes,scope->budget,NULL);
    return released == TURBODB_STATUS_OK ? status : released;
  }
  *out = (orm_sql_dependencies){.budget=scope->budget,.describe=describe,
      .lateral_frames=mode==DEPENDENCY_LATERAL_METADATA||binding_only,
      .binding_only=binding_only,.evaluation=scope->evaluation};
  status = dependency_storage(out,count,error);
  if (status == TURBODB_STATUS_OK) status = dependency_tree(scope,&references,max_iterations!=0,out,error);
  bool lexical_frames=false;
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<out->count;++i) {
    const dependency_node *node=vec_at_const(&out->nodes,i);
    if(node->lexical) lexical_frames=true;
  }
  if(lexical_frames) effective.force_dependency_schema=true;
  /* Ordinary queries do not need lexical identities after sorting the graph. */
  if(!max_iterations) {
    const turbodb_status_t released=orm_sql_work_release(&references,reference_bytes,scope->budget,status==TURBODB_STATUS_OK?error:NULL);
    reference_bytes=0; if(status==TURBODB_STATUS_OK) status=released;
  }
  effective.derived=vec_data_const(&out->derived);
  effective.derived_count=out->derived_count;
  orm_sql_query_scope child = *scope; child.queries = vec_data_const(&out->bindings); child.query_count = out->query_count;
  child.derived = vec_data_const(&out->derived); child.derived_count = out->derived_count;
  child.prepare_dependency_schema=NULL;
  child.dependency_schema_context=NULL;
  const orm_sql_expr_query_sources sources = {vec_data_const(&out->sources),out->query_count,out->evaluation};
  bool root_scope_prepared=scope->dependency_schema!=NULL;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < count; ++i) {
    const size_t index = *(const size_t *)vec_at_const(&out->order,i);
    dependency_node *node = vec_at(&out->nodes,index); child.root = node->query_ast; ++out->prepared;
    child.outer_schema=NULL; child.outer_qualifier=(vstr){0}; child.defer_execution=binding_only;
    if(scope->dependency_schema) root_scope_prepared=true;
    if(!root_scope_prepared&&node->root_capture&&!node->derived&&!node->cte&&
        !node->reference&&scope->prepare_dependency_schema) {
      status=scope->prepare_dependency_schema(&effective,
          scope->dependency_schema_context,error);
      root_scope_prepared=true;
      if(status!=TURBODB_STATUS_OK) break;
    }
    if(node->recursive_owner!=SIZE_MAX&&node->recursive_part==ORM_SQL_CTE_RECURSIVE_PART) {
      orm_sql_query_scope seed=*scope;
      seed.queries=child.queries; seed.query_count=child.query_count;
      status=dependency_recursive_seed_prepare(&seed,&references,owner,parameters,&sources,max_iterations,
          vec_at(&out->nodes,node->recursive_owner),error);
      if(status!=TURBODB_STATUS_OK) break;
    }
    if(node->self) continue;
    if (node->reference) {
      dependency_node *definition = vec_at(&out->nodes,node->definition);
      orm_sql_cte_store *cache=definition->recursive ?
          &((orm_sql_cte_query *)vec_at(&definition->recursion,0))->cache : &definition->cache;
      if (!describe) status = orm_sql_cte_reader_open(cache,&node->reader,error);
      if (status == TURBODB_STATUS_OK) *(orm_sql_derived_binding *)vec_at(&out->derived,node->slot) =
          (orm_sql_derived_binding){node->ast,&definition->schema,describe ? NULL : orm_sql_cte_reader_source(&node->reader),
              describe?NULL:&node->input};
      if(status==TURBODB_STATUS_OK&&!describe)
        node->input=(orm_sql_from_input){.source=orm_sql_cte_reader_source(&node->reader),.context=node,.rewind=dependency_from_rewind};
      if(status==TURBODB_STATUS_OK)
        status=dependency_root_reference_scope(scope,node,definition,&effective,error);
      continue;
    }
    if(node->recursive) {
      child.root=scope->root;
      const turbodb_value_t *arguments=parameters;
      orm_sql_query_scope seed=*scope; seed.queries=child.queries; seed.query_count=child.query_count;
      status=dependency_recursive_seed_prepare(&seed,&references,owner,parameters,&sources,max_iterations,node,error);
      if(status==TURBODB_STATUS_OK&&node->frame.count) {
        arguments=vec_data_const(&node->arguments.values);
        child.outer_schema=&node->frame; child.defer_execution=!describe;
      }
      if(status==TURBODB_STATUS_OK)
        status=dependency_recursive_open(&child,&references,owner,arguments,&sources,node,error);
      bool inherited=false;
      if(status==TURBODB_STATUS_OK) status=dependency_correlated_query_child(node,&inherited,error);
      node->correlated=node->correlated||inherited;
      if(status==TURBODB_STATUS_OK&&!describe&&child.defer_execution&&!node->correlated) {
        orm_sql_cte_query *query=vec_at(&node->recursion,0);
        status=orm_sql_cte_query_execution_close(query,error);
        if(status==TURBODB_STATUS_OK)
          status=orm_sql_cte_query_execution_open(query,arguments,query->parameter_count,error);
      }
      continue;
    }
    const sqlparser_node *ast = sqlparser_get_node(scope->document,node->ast);
    child.in_query = ast->kind == SQLPARSER_IN;
    node->existence = ast->kind == SQLPARSER_UNARY; node->witness = turbodb_bool(true);
    child.scalar_output = !node->existence && !node->derived && !node->cte;
    child.anonymous_output = node->existence || (node->cte && ast->as.cte.columns.count) ||
        (node->derived && ast->as.table.column_aliases.count);
    child.demand = node->existence ? ORM_SQL_QUERY_EXISTENCE : ORM_SQL_QUERY_VALUES;
    const orm_sql_subquery_kind kind = node->existence ? ORM_SQL_SUBQUERY_EXISTS : child.in_query ?
        (ast->as.in.negated ? ORM_SQL_SUBQUERY_NOT_IN : ORM_SQL_SUBQUERY_IN) : ORM_SQL_SUBQUERY_SCALAR;
    orm_sql_query_scope correlation=*scope;
    if(lexical_frames&&node->lexical) {
      status=dependency_frames(scope,owner,node,error);
      if(status!=TURBODB_STATUS_OK) break;
      correlation.dependency_schema=&node->frame;
      correlation.dependency_qualifier=(vstr){0};
    }
    bool candidate=false,inherited=false;
    if(!describe||lexical_frames) status=dependency_correlation_candidate(&correlation,node,&candidate,error);
    if(status==TURBODB_STATUS_OK&&(!describe||lexical_frames))
      status=dependency_correlated_query_child(node,&inherited,error);
    candidate=candidate||inherited||node->lateral;
    if(status==TURBODB_STATUS_OK&&candidate) {
      child.outer_schema=correlation.dependency_schema; child.outer_qualifier=correlation.dependency_qualifier;
      child.defer_execution=binding_only||!describe;
      if(describe||(!node->derived&&!node->cte)||(!describe&&node->derived&&out->lateral_frames))
        status=dependency_arguments(&correlation,parameters,node,error);
    }
    const bool provider=!describe&&node->derived&&out->lateral_frames;
    if(status==TURBODB_STATUS_OK&&provider&&!candidate) {
      orm_sql_query_scope markers=correlation; markers.dependency_schema=NULL;
      status=dependency_arguments(&markers,parameters,node,error);
    }
    if(provider) child.defer_execution=true;
    if(status==TURBODB_STATUS_OK)
      status = orm_sql_runtime_scope_open(&child,owner,describe&&candidate?
          vec_data_const(&node->arguments.values):parameters,describe&&!binding_only,&sources,&node->query,error);
    if (status != TURBODB_STATUS_OK) break;
    const orm_sql_select *plan = orm_sql_runtime_plan(&node->query);
    if (node->derived || node->cte) {
      node->correlated=candidate&&plan&&(plan->correlated||inherited);
      status=dependency_derived(scope,node,out,error);
      if(describe) continue;
      if(status==TURBODB_STATUS_OK&&(node->correlated||node->lateral||provider))
        status=dependency_correlated_inputs_close(node,true,error);
      if(status==TURBODB_STATUS_OK&&provider&&!node->correlated&&!node->lateral) {
        const turbodb_value_t *arguments=vec_data_const(&node->arguments.values);
        status=dependency_correlated_inputs_open(node,arguments,node->marker_count+node->outer_count,error);
        if(status==TURBODB_STATUS_OK) status=orm_sql_runtime_execution_open(&node->query,
            arguments,node->marker_count+node->outer_count,&sources,error);
      } else if(status==TURBODB_STATUS_OK&&candidate&&!node->correlated&&!node->lateral&&!provider) {
        status=dependency_arguments(&correlation,parameters,node,error);
        if(status==TURBODB_STATUS_OK)
          status=orm_sql_runtime_execution_open(&node->query,
              vec_data_const(&node->arguments.values),
              node->marker_count+node->outer_count,&sources,error);
      }
      continue;
    }
    if (!plan || (!node->existence && vec_size(&plan->columns) != 1)) {
      status = dependency_error(error,TURBODB_STATUS_SQL_ERROR,"scalar and IN subqueries require exactly one output column"); break;
    }
    node->type = node->existence ? (orm_sql_type){TURBODB_VALUE_BOOLEAN,false} : orm_tidesdb_sql_select_column_at(plan,0)->type;
    orm_sql_expr_query_binding *binding = vec_at(&out->bindings,node->slot);
    binding->type = (orm_sql_expr_query_type){.kind=kind,
        .result={child.in_query ? TURBODB_VALUE_BOOLEAN : node->type.kind,!node->existence},.element=node->type};
    node->correlated=candidate&&plan&&(plan->correlated||inherited);
    binding->capture_context=node; binding->bind_capture=dependency_bind_capture;
    if (describe) continue;
    node->source = (orm_sql_row_source){scope->budget,&node->type,1,node,dependency_pull,false};
    node->subquery_kind=kind;
    if(status==TURBODB_STATUS_OK&&node->correlated) {
      status=dependency_correlated_inputs_close(node,true,error);
      node->correlated_results.budget=scope->budget;
      if(status==TURBODB_STATUS_OK) {
        node->correlated_source=(orm_sql_expr_query_source){scope->budget,binding->type,node,dependency_correlated_eval,0};
        *(orm_sql_expr_query_source **)vec_at(&out->sources,node->slot)=&node->correlated_source;
      }
    } else if(status==TURBODB_STATUS_OK) {
      if(candidate) status=orm_sql_runtime_execution_open(&node->query,vec_data_const(&node->arguments.values),
          node->marker_count+node->outer_count,&sources,error);
      if(status==TURBODB_STATUS_OK) status = child.in_query ? orm_sql_subquery_open_set(&node->source,kind,&node->scalar,error) :
          orm_tidesdb_sql_subquery_open(&node->source,kind,NULL,&node->scalar,error);
      if (status == TURBODB_STATUS_OK) *(orm_sql_expr_query_source **)vec_at(&out->sources,node->slot) = orm_tidesdb_sql_subquery_source(&node->scalar);
    }
  }
  if(status==TURBODB_STATUS_OK) status=dependency_frames_close(out,error);
  const turbodb_status_t released=orm_sql_work_release(&references,reference_bytes,scope->budget,status==TURBODB_STATUS_OK?error:NULL);
  return status==TURBODB_STATUS_OK?released:status;
}
turbodb_status_t orm_sql_dependencies_open(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner,const turbodb_value_t *parameters,bool describe,
    orm_sql_dependencies *out,turbodb_error_t *error) {
  return dependency_open(scope,owner,parameters,describe?DEPENDENCY_DESCRIBE:DEPENDENCY_EXECUTE,0,out,error);
}
turbodb_status_t orm_sql_dependencies_open_recursive(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner,const turbodb_value_t *parameters,bool describe,uint64_t max_iterations,
    orm_sql_dependencies *out,turbodb_error_t *error) {
  if(!max_iterations) return dependency_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"recursive dependency iteration limit must be positive");
  return dependency_open(scope,owner,parameters,describe?DEPENDENCY_DESCRIBE:DEPENDENCY_EXECUTE,max_iterations,out,error);
}
turbodb_status_t orm_sql_dependencies_lateral_metadata_open(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner,const turbodb_value_t *parameters,uint64_t max_iterations,
    orm_sql_dependencies *out,turbodb_error_t *error) {
  return dependency_open(scope,owner,parameters,DEPENDENCY_LATERAL_METADATA,max_iterations,out,error);
}
turbodb_status_t orm_sql_dependencies_bind(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner,orm_sql_dependencies *out,turbodb_error_t *error) {
  return dependency_open(scope,owner,NULL,DEPENDENCY_BIND,0,out,error);
}
turbodb_status_t orm_sql_dependencies_bind_recursive(const orm_sql_query_scope *scope,
    orm_sql_catalog_store *owner,uint64_t max_iterations,
    orm_sql_dependencies *out,turbodb_error_t *error) {
  if(!max_iterations)
    return dependency_error(error,TURBODB_STATUS_INVALID_ARGUMENT,
        "recursive dependency binding iteration limit must be positive");
  return dependency_open(scope,owner,NULL,DEPENDENCY_BIND,max_iterations,out,error);
}
static bool dependency_replays(const dependency_node *node,size_t round_owner) {
  if(node->lateral) return false;
  return round_owner==SIZE_MAX ? node->replay&&!node->correlated_input :
      node->round_owner==round_owner;
}
static turbodb_status_t dependency_execution_close(orm_sql_dependencies *run,size_t round_owner,turbodb_error_t *error) {
  if (!run || !run->budget) return TURBODB_STATUS_OK;
  if (run->describe) return dependency_error(error,TURBODB_STATUS_UNSUPPORTED,"EXPLAIN dependencies cannot replay");
  for (size_t i = run->prepared; i; --i) {
    dependency_node *node = vec_at(&run->nodes,*(const size_t *)vec_at_const(&run->order,i-1));
    if (!dependency_replays(node,round_owner) || !node->derived) continue;
    if (node->source.active) return dependency_error(error,TURBODB_STATUS_BUSY,"close FROM consumers before replaying dependencies");
    const turbodb_status_t status = orm_sql_runtime_execution_close(&node->query,error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t dependency_execution_open(orm_sql_dependencies *run,const turbodb_value_t *parameters,
    size_t count,size_t round_owner,turbodb_error_t *error) {
  if (!run || !run->budget) return TURBODB_STATUS_OK;
  if (run->describe) return dependency_error(error,TURBODB_STATUS_UNSUPPORTED,"EXPLAIN dependencies cannot replay");
  const orm_sql_expr_query_sources sources = {vec_data_const(&run->sources),run->query_count,run->evaluation};
  turbodb_status_t status = dependency_steps(run->budget,run->prepared,error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < run->prepared; ++i) {
    dependency_node *node = vec_at(&run->nodes,*(const size_t *)vec_at_const(&run->order,i));
    if (!dependency_replays(node,round_owner)) continue;
    if(node->reference) status=orm_sql_cte_reader_rewind(&node->reader,error);
    else {
      const orm_sql_select *plan=orm_sql_runtime_plan(&node->query);
      const size_t expected=plan?vec_size(&plan->parameter_types):SIZE_MAX;
      if(expected>count||(round_owner==SIZE_MAX&&expected!=count))
        status=dependency_error(error,TURBODB_STATUS_INVALID_STATE,"round input parameter layout is inconsistent");
      else status=orm_sql_runtime_execution_open(&node->query,parameters,expected,&sources,error);
    }
  }
  return status;
}
turbodb_status_t orm_sql_dependencies_execution_close(orm_sql_dependencies *run,turbodb_error_t *error) {
  return dependency_execution_close(run,SIZE_MAX,error);
}
turbodb_status_t orm_sql_dependencies_execution_open(orm_sql_dependencies *run,const turbodb_value_t *parameters,
    size_t count,turbodb_error_t *error) {
  return dependency_execution_open(run,parameters,count,SIZE_MAX,error);
}
static turbodb_status_t dependency_next(orm_sql_dependencies *run, size_t index, size_t *out, turbodb_error_t *error) {
  const dependency_node *node = vec_at_const(&run->nodes,index);
  size_t steps = 1, next = node->child;
  while (next == SIZE_MAX) {
    if (node->sibling != SIZE_MAX) { next = node->sibling; break; }
    if (node->parent == SIZE_MAX) break;
    node = vec_at_const(&run->nodes,node->parent); ++steps;
  }
  const turbodb_status_t status = dependency_steps(run->budget,steps,error);
  if (status == TURBODB_STATUS_OK) *out = next == SIZE_MAX ? run->count : next;
  return status;
}
static turbodb_status_t dependency_explain_pull(void *context, const turbodb_value_t **out, turbodb_error_t *error) {
  orm_sql_dependencies *run = context;
  while (run->position == SIZE_MAX || run->position < run->count) {
    if (run->position != SIZE_MAX && ((const dependency_node *)vec_at_const(&run->nodes,run->position))->reference) {
      const turbodb_status_t status = dependency_next(run,run->position,&run->position,error);
      if (status != TURBODB_STATUS_OK) return status;
      continue;
    }
    dependency_node *node = run->position == SIZE_MAX ? NULL : vec_at(&run->nodes,run->position);
    orm_sql_scan *scan;
    if (node && node->recursive) {
      orm_sql_cte_query *query = vec_at(&node->recursion,0);
      scan = node->explained_part==DEPENDENCY_EXPLAIN_SEED ? query->initial.scan :
          node->explained_part==DEPENDENCY_EXPLAIN_MEMBER ? query->recursive.scan : &query->explained_page;
    } else scan = orm_sql_runtime_base_scan(node ? &node->query : run->root);
    orm_sql_scan_row row;
    turbodb_status_t status = orm_tidesdb_sql_scan_next(scan,&row,error);
    if (status != TURBODB_STATUS_OK) return status;
    if (row.state == ORM_SQL_SCAN_ROW) { *out = row.values; return TURBODB_STATUS_OK; }
    if (node && node->recursive && node->explained_part<DEPENDENCY_EXPLAIN_PAGE) {
      ++node->explained_part;
      if(node->explained_part==DEPENDENCY_EXPLAIN_MEMBER || ((orm_sql_cte_query *)vec_at(&node->recursion,0))->explained_page.budget) continue;
    }
    if (run->position == SIZE_MAX) run->position = run->first;
    else {
      status = dependency_next(run,run->position,&run->position,error);
      if (status != TURBODB_STATUS_OK) return status;
    }
  }
  *out = NULL; return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_dependencies_explain(orm_sql_query *root, turbodb_error_t *error) {
  orm_sql_dependencies *run = &root->dependencies;
  if (!run->budget || !run->count || !run->describe) return TURBODB_STATUS_OK;
  int64_t id = 0;
  turbodb_status_t status = orm_sql_runtime_explain_ids(root,ORM_SQL_EXPLAIN_PRIMARY,&id,error);
  for (size_t i = run->first; status == TURBODB_STATUS_OK && i < run->count;) {
    dependency_node *node = vec_at(&run->nodes,i);
    if (node->recursive) {
      orm_sql_cte_query *query = vec_at(&node->recursion,0);
      status = orm_sql_compound_explain_ids(&query->initial,ORM_SQL_EXPLAIN_DERIVED,&id,error);
      if (status == TURBODB_STATUS_OK) status = orm_sql_compound_explain_ids(&query->recursive,ORM_SQL_EXPLAIN_RECURSIVE_BRANCH,&id,error);
    } else if (!node->reference) status = orm_sql_runtime_explain_ids(&node->query,(node->derived || node->cte) ? ORM_SQL_EXPLAIN_DERIVED : ORM_SQL_EXPLAIN_SUBQUERY,&id,error);
    if (status == TURBODB_STATUS_OK) status = dependency_next(run,i,&i,error);
  }
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < ORM_SQL_EXPLAIN_COLUMNS; ++i) {
    status = orm_tidesdb_sql_runtime_column(root,i,&run->columns[i],error);
    if (status == TURBODB_STATUS_OK) run->types[i] = run->columns[i].type;
  }
  enum { ID_COLUMN = 0 };
  run->types[ID_COLUMN].nullable = run->columns[ID_COLUMN].type.nullable = true;
  run->root = root; run->position = SIZE_MAX;
  run->explanation = (orm_sql_row_source){run->budget,run->types,ORM_SQL_EXPLAIN_COLUMNS,run,dependency_explain_pull,false};
  size_t projection[ORM_SQL_EXPLAIN_COLUMNS];
  for (size_t i = 0; i < ORM_SQL_EXPLAIN_COLUMNS; ++i) projection[i] = i;
  const orm_sql_scan_spec spec = {.projection=projection,.projection_count=ORM_SQL_EXPLAIN_COLUMNS,.limit=UINT64_MAX};
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_scan_open_source(&run->explanation,&spec,run->budget,&run->explained,error);
  return status;
}
