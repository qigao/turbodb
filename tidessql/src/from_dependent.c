#include "from.h"
#include "work.h"
#include "error.h"
#include <string.h>

typedef struct dependent_state {
  orm_sql_from_run *owner;
  orm_sql_snapshot parameters;
  vec_t queries;
  size_t query_bytes;
  turbodb_error_t failure;
  bool closing;
} dependent_state;
typedef struct dependent_node {
  dependent_state *state;
  size_t ordinal,parent,end;
  orm_sql_from_input input;
  orm_sql_row_source source;
  orm_sql_scan projection;
  orm_sql_join join;
  vec_t types,routes,captures;
  size_t type_bytes,route_bytes,capture_bytes;
  const turbodb_value_t *prefix;
  bool contains_lateral,opened,provider_bound,evaluating;
} dependent_node;

static turbodb_status_t dependent_error(turbodb_error_t *error,turbodb_status_t status,const char *reason) {
  tdsql_error_set(error,status,reason); return status;
}
static turbodb_status_t dependent_steps(dependent_state *state,size_t count,turbodb_error_t *error) {
  orm_sql_budget_amount charge={0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=count;
  return orm_tidesdb_sql_budget_reserve(state->owner->plan->budget,&charge,error);
}
static size_t dependent_first(const orm_sql_from_node *node) { return node->reversed?node->right:node->left; }
static size_t dependent_second(const orm_sql_from_node *node) { return node->reversed?node->left:node->right; }
static dependent_node *dependent_at(dependent_state *state,size_t ordinal) {
  return vec_at(&state->owner->nodes,ordinal);
}
static turbodb_status_t dependent_subtree_close(dependent_state *state,size_t root,turbodb_error_t *error) {
  const size_t end=dependent_at(state,root)->end;
  /* A JOIN closes its dependent right subtree before releasing its left scan.
   * The outer iteration then sees already-closed descendants, never twice-owned
   * operators. Fixed routing metadata survives until every provider has closed. */
  for(size_t i=root;i<end;++i) {
    dependent_node *node=dependent_at(state,i);
    if(node->source.active)
      return dependent_error(error,TURBODB_STATUS_BUSY,"dependent FROM subtree has a consumer");
    turbodb_status_t status=orm_tidesdb_sql_scan_close(&node->projection,error);
    if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_join_close(&node->join,error);
    if(status==TURBODB_STATUS_OK&&node->provider_bound) {
      node->evaluating=true;
      status=node->input.binding.close(node->input.binding.context,error);
      node->evaluating=false;
      if(status==TURBODB_STATUS_OK) node->provider_bound=false;
    }
    if(status!=TURBODB_STATUS_OK) return status;
    if(vec_size(&node->captures)) memset(vec_data(&node->captures),0,vec_size(&node->captures)*sizeof(turbodb_value_t));
    node->opened=false; node->prefix=NULL;
  }
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_from_dependent_close(orm_sql_from_run *run,turbodb_error_t *error) {
  if(!run||!run->plan) return TURBODB_STATUS_OK;
  if(run->source&&run->source->active)
    return dependent_error(error,TURBODB_STATUS_BUSY,"dependent FROM has a consumer");
  dependent_state *state=vec_size(&run->dependent_state)?vec_data(&run->dependent_state):NULL;
  for(size_t i=0;i<vec_size(&run->nodes);++i) {
    const dependent_node *node=vec_at_const(&run->nodes,i);
    if(node->evaluating||node->projection.evaluating||node->join.evaluating)
      return dependent_error(error,TURBODB_STATUS_BUSY,"dependent FROM is evaluating");
  }
  orm_tidesdb_sql_budget *budget=run->plan->budget;
  turbodb_status_t status=TURBODB_STATUS_OK;
  if(state) state->closing=true;
  if(state&&vec_size(&run->nodes)) status=dependent_subtree_close(state,0,error);
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<vec_size(&run->nodes);++i) {
    dependent_node *node=vec_at(&run->nodes,i);
    vec_t *vectors[]={&node->types,&node->routes,&node->captures};
    size_t *bytes[]={&node->type_bytes,&node->route_bytes,&node->capture_bytes};
    for(size_t j=0;status==TURBODB_STATUS_OK&&j<sizeof(vectors)/sizeof(vectors[0]);++j) {
      status=orm_sql_work_release(vectors[j],*bytes[j],budget,error);
      if(status==TURBODB_STATUS_OK) *bytes[j]=0;
    }
  }
  if(status==TURBODB_STATUS_OK&&state) {
    status=orm_sql_work_release(&state->parameters.values,state->parameters.bytes,budget,error);
    if(status==TURBODB_STATUS_OK) state->parameters.bytes=0;
    if(status==TURBODB_STATUS_OK) status=orm_sql_work_release(&state->queries,state->query_bytes,budget,error);
    if(status==TURBODB_STATUS_OK) state->query_bytes=0;
  }
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_release(&run->nodes,run->node_bytes,budget,error);
  if(status==TURBODB_STATUS_OK) run->node_bytes=0;
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_release(&run->dependent_state,run->dependent_bytes,budget,error);
  if(status==TURBODB_STATUS_OK) run->dependent_bytes=0;
  if(status==TURBODB_STATUS_OK&&run->metadata_bytes)
    status=orm_tidesdb_sql_budget_release(budget,ORM_SQL_BUDGET_WORK_BYTES,run->metadata_bytes,error);
  if(status==TURBODB_STATUS_OK) { --run->plan->active_runs; *run=(orm_sql_from_run){0}; }
  return status;
}
static turbodb_status_t dependent_right_open(void *context,const turbodb_value_t *row,size_t columns,turbodb_error_t *error) {
  dependent_node *node=context;
  const orm_sql_from *plan=node->state->owner->plan;
  const orm_sql_from_node *bound=orm_tidesdb_sql_from_at(plan,node->ordinal);
  const size_t right=dependent_second(bound);
  if(!row||columns!=orm_tidesdb_sql_from_at(plan,dependent_first(bound))->schema.count)
    return dependent_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"dependent FROM prefix width differs");
  if(node->prefix||dependent_at(node->state,right)->opened)
    return dependent_error(error,TURBODB_STATUS_INVALID_STATE,"dependent FROM right subtree was not closed");
  node->prefix=row;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t dependent_right_close(void *context,turbodb_error_t *error) {
  dependent_node *node=context;
  const orm_sql_from_node *bound=orm_tidesdb_sql_from_at(node->state->owner->plan,node->ordinal);
  const turbodb_status_t status=dependent_subtree_close(node->state,dependent_second(bound),error);
  if(status==TURBODB_STATUS_OK) node->prefix=NULL;
  return status;
}
static turbodb_status_t dependent_source_check(dependent_node *node,turbodb_error_t *error) {
  const orm_sql_row_source *source=node->input.source;
  if(!source||source->budget!=node->source.budget||source->columns!=node->source.columns||
      !source->types||!source->next||!source->context||source->active)
    return dependent_error(error,source&&source->active?TURBODB_STATUS_BUSY:TURBODB_STATUS_INVALID_ARGUMENT,
        "dependent FROM provider source differs from bound schema");
  turbodb_status_t status=dependent_steps(node->state,source->columns,error);
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<source->columns;++i)
    if(source->types[i].kind!=node->source.types[i].kind||source->types[i].nullable!=node->source.types[i].nullable)
      status=dependent_error(error,TURBODB_STATUS_TYPE_ERROR,"dependent FROM provider source changed types");
  return status;
}
static turbodb_status_t dependent_capture(dependent_node *node,turbodb_error_t *error) {
  dependent_state *state=node->state; const orm_sql_from *plan=state->owner->plan;
  turbodb_status_t status=dependent_steps(state,vec_size(&node->captures),error);
  size_t position=0;
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<vec_size(&node->routes);++i) {
    const size_t parent=*(const size_t *)vec_at_const(&node->routes,i);
    const dependent_node *prefix=dependent_at(state,parent);
    const orm_sql_from_node *ancestor=orm_tidesdb_sql_from_at(plan,parent);
    const size_t width=orm_tidesdb_sql_from_at(plan,dependent_first(ancestor))->schema.count;
    if(!prefix->prefix) return dependent_error(error,TURBODB_STATUS_INVALID_STATE,"dependent FROM prefix row is unavailable");
    memcpy((turbodb_value_t *)vec_data(&node->captures)+position,prefix->prefix,width*sizeof(turbodb_value_t));
    position+=width;
  }
  const size_t outer=vec_size(&plan->parameter_types)-plan->parameter_marker_count;
  if(status==TURBODB_STATUS_OK&&outer)
    memcpy((turbodb_value_t *)vec_data(&node->captures)+position,
        (const turbodb_value_t *)vec_data_const(&state->parameters.values)+plan->parameter_marker_count,
        outer*sizeof(turbodb_value_t));
  return status;
}
static turbodb_status_t dependent_projection(dependent_node *node,orm_sql_row_source *source,
    bool reversed,turbodb_error_t *error) {
  const orm_sql_from_node *bound=orm_tidesdb_sql_from_at(node->state->owner->plan,node->ordinal);
  const size_t width=bound->schema.count;
  const size_t left=reversed?orm_tidesdb_sql_from_at(node->state->owner->plan,bound->left)->schema.count:0;
  vec_t mapping={0}; size_t bytes=0;
  turbodb_status_t status=orm_sql_work_zero(&mapping,width,sizeof(size_t),_Alignof(size_t),node->source.budget,&bytes,error);
  if(status==TURBODB_STATUS_OK) status=dependent_steps(node->state,width,error);
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<width;++i)
    *(size_t *)vec_at(&mapping,i)=reversed?(i<left?width-left+i:i-left):i;
  const orm_sql_scan_spec spec={.projection=vec_data_const(&mapping),.projection_count=width,.limit=UINT64_MAX};
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_scan_open_source(source,&spec,node->source.budget,&node->projection,error);
  const turbodb_status_t released=orm_sql_work_release(&mapping,bytes,node->source.budget,status==TURBODB_STATUS_OK?error:NULL);
  return status==TURBODB_STATUS_OK?released:status;
}
static turbodb_status_t dependent_node_open(dependent_node *node,turbodb_error_t *error) {
  dependent_state *state=node->state; orm_sql_from *plan=state->owner->plan;
  const orm_sql_from_node *bound=orm_tidesdb_sql_from_at(plan,node->ordinal);
  turbodb_status_t status=dependent_steps(state,1,error);
  if(status!=TURBODB_STATUS_OK) return status;
  if(bound->leaf) {
    if(bound->lateral) {
      status=dependent_capture(node,error);
      if(status==TURBODB_STATUS_OK) status=node->input.binding.open(node->input.binding.context,
          vec_data_const(&node->captures),vec_size(&node->captures),error);
      if(status==TURBODB_STATUS_OK) node->provider_bound=true;
    } else status=node->input.rewind(node->input.context,error);
    if(status==TURBODB_STATUS_OK) status=dependent_source_check(node,error);
    if(status==TURBODB_STATUS_OK) status=dependent_projection(node,node->input.source,false,error);
  } else {
    dependent_node *left=dependent_at(state,dependent_first(bound));
    dependent_node *right=dependent_at(state,dependent_second(bound));
    const orm_sql_join_spec spec={.kind=bound->kind,.condition=bound->condition.budget?(orm_sql_expr *)&bound->condition:NULL,
        .slots=vec_data_const(&bound->slots),.count=vec_size(&bound->slots),
        .parameters=vec_data_const(&state->parameters.values),.parameter_types=vec_data_const(&plan->parameter_types),
        .parameter_count=vec_size(&plan->parameter_types),.queries={vec_data_const(&state->queries),vec_size(&state->queries)},
        .query_slots=vec_data_const(&bound->query_slots),.query_count=vec_size(&bound->query_slots),
        .right_binding=right->contains_lateral?
            (orm_sql_join_right_binding){node,dependent_right_open,dependent_right_close}:(orm_sql_join_right_binding){0},
        .match=bound->common ? ORM_SQL_JOIN_MATCH_USING : ORM_SQL_JOIN_MATCH_ON,.keys=vec_data_const(&bound->keys),.key_count=bound->key_count};
    status=orm_tidesdb_sql_join_open(&left->source,&right->source,&spec,&node->join,error);
    if(status==TURBODB_STATUS_OK&&bound->reversed)
      status=dependent_projection(node,orm_tidesdb_sql_join_source(&node->join),true,error);
  }
  if(status==TURBODB_STATUS_OK) node->opened=true;
  return status;
}
static turbodb_status_t dependent_pull(void *context,const turbodb_value_t **out,turbodb_error_t *error) {
  dependent_node *node=context; dependent_state *state=node->state;
  if(state->failure.status!=TURBODB_STATUS_OK)
    return dependent_error(error,state->failure.status,state->failure.message);
  if(state->closing) return dependent_error(error,TURBODB_STATUS_INVALID_STATE,"dependent FROM is closing");
  if(node->evaluating) return dependent_error(error,TURBODB_STATUS_BUSY,"dependent FROM source is evaluating");
  turbodb_error_t cause; tdsql_error_init(&cause); node->evaluating=true;
  turbodb_status_t status=node->opened?TURBODB_STATUS_OK:dependent_node_open(node,&cause);
  orm_sql_scan_row row={0};
  if(status==TURBODB_STATUS_OK) status=node->projection.budget?
      orm_tidesdb_sql_scan_next(&node->projection,&row,&cause):orm_tidesdb_sql_join_next(&node->join,&row,&cause);
  if(status==TURBODB_STATUS_OK&&row.state!=ORM_SQL_SCAN_ROW&&row.state!=ORM_SQL_SCAN_DONE)
    status=dependent_error(&cause,TURBODB_STATUS_INVALID_STATE,"dependent FROM input cancelled");
  node->evaluating=false;
  if(status==TURBODB_STATUS_OK) *out=row.state==ORM_SQL_SCAN_ROW?row.values:NULL;
  else { tdsql_error_set(&state->failure,status,cause.message); tdsql_error_set(error,status,cause.message); }
  return status;
}
static turbodb_status_t dependent_routes(dependent_node *node,turbodb_error_t *error) {
  dependent_state *state=node->state; const orm_sql_from *plan=state->owner->plan;
  size_t routes=0,width=vec_size(&plan->parameter_types)-plan->parameter_marker_count;
  for(size_t child=node->ordinal;dependent_at(state,child)->parent!=SIZE_MAX;) {
    const size_t parent=dependent_at(state,child)->parent;
    const orm_sql_from_node *bound=orm_tidesdb_sql_from_at(plan,parent);
    const turbodb_status_t status=dependent_steps(state,1,error);
    if(status!=TURBODB_STATUS_OK) return status;
    if(dependent_second(bound)==child) {
      const size_t columns=orm_tidesdb_sql_from_at(plan,dependent_first(bound))->schema.count;
      if(columns>SIZE_MAX-width) return dependent_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"dependent FROM capture width overflow");
      width+=columns; ++routes;
    }
    child=parent;
  }
  if(width!=node->input.capture_count||(width&&!node->input.capture_types))
    return dependent_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"dependent FROM capture layout differs");
  if(width>plan->budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
    return dependent_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"dependent FROM capture capacity exceeded");
  turbodb_status_t status=orm_sql_work_zero(&node->routes,routes,sizeof(size_t),_Alignof(size_t),plan->budget,&node->route_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&node->captures,width,sizeof(turbodb_value_t),_Alignof(turbodb_value_t),
      plan->budget,&node->capture_bytes,error);
  for(size_t child=node->ordinal;status==TURBODB_STATUS_OK&&dependent_at(state,child)->parent!=SIZE_MAX;) {
    const size_t parent=dependent_at(state,child)->parent;
    status=dependent_steps(state,1,error);
    if(dependent_second(orm_tidesdb_sql_from_at(plan,parent))==child)
      *(size_t *)vec_at(&node->routes,--routes)=parent;
    child=parent;
  }
  if(status==TURBODB_STATUS_OK) status=dependent_steps(state,width,error);
  size_t position=0;
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<vec_size(&node->routes);++i) {
    const orm_sql_from_node *parent=orm_tidesdb_sql_from_at(plan,*(const size_t *)vec_at_const(&node->routes,i));
    const orm_sql_table_schema *prefix=&orm_tidesdb_sql_from_at(plan,dependent_first(parent))->schema;
    for(size_t j=0;j<prefix->count;++j,++position)
      if(prefix->columns[j].type.kind!=node->input.capture_types[position].kind||
          prefix->columns[j].type.nullable!=node->input.capture_types[position].nullable)
        return dependent_error(error,TURBODB_STATUS_TYPE_ERROR,"dependent FROM prefix capture types differ");
  }
  for(size_t i=plan->parameter_marker_count;status==TURBODB_STATUS_OK&&i<vec_size(&plan->parameter_types);++i,++position) {
    const orm_sql_type *outer=vec_at_const(&plan->parameter_types,i);
    if(outer->kind!=node->input.capture_types[position].kind||outer->nullable!=node->input.capture_types[position].nullable)
      status=dependent_error(error,TURBODB_STATUS_TYPE_ERROR,"dependent FROM outer capture types differ");
  }
  node->input.capture_types=NULL;
  return status;
}
turbodb_status_t orm_sql_from_open_dependent(orm_sql_from *plan,const orm_sql_from_input *inputs,size_t count,
    const turbodb_value_t *parameters,size_t parameter_count,const orm_sql_expr_query_sources *queries,
    orm_sql_from_run *out,turbodb_error_t *error) {
  if(!plan||!plan->budget||!plan->count||!inputs||count!=plan->tables||!out||out->plan||
      parameter_count!=vec_size(&plan->parameter_types)||(parameter_count&&!parameters)||
      (queries&&queries->count&&!queries->items))
    return dependent_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid dependent FROM inputs");
  if(!plan->conditions_bound) return dependent_error(error,TURBODB_STATUS_INVALID_STATE,"dependent FROM conditions are incomplete");
  if(plan->root!=plan->statement_from) return dependent_error(error,TURBODB_STATUS_UNSUPPORTED,"dependent FROM requires the complete FROM tree");
  if(plan->parameter_marker_count>parameter_count)
    return dependent_error(error,TURBODB_STATUS_INVALID_STATE,"dependent FROM parameter layout is invalid");
  if(plan->active_runs==SIZE_MAX) return dependent_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"dependent FROM run lease overflow");
  if(queries&&queries->count>plan->budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
    return dependent_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"dependent FROM query registry capacity exceeded");
  *out=(orm_sql_from_run){.plan=plan}; ++plan->active_runs;
  turbodb_status_t status=orm_tidesdb_sql_budget_reserve_capacity(plan->budget,1,sizeof(*out),0,&out->metadata_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&out->dependent_state,1,sizeof(dependent_state),_Alignof(dependent_state),
      plan->budget,&out->dependent_bytes,error);
  dependent_state *state=status==TURBODB_STATUS_OK?vec_data(&out->dependent_state):NULL;
  if(state) { state->owner=out; tdsql_error_init(&state->failure); }
  if(status==TURBODB_STATUS_OK&&parameter_count)
    status=orm_sql_snapshot_copy(&state->parameters,parameters,parameter_count,0,plan->budget,error);
  if(status==TURBODB_STATUS_OK&&queries&&queries->count) {
    status=orm_sql_work_zero(&state->queries,queries->count,sizeof(orm_sql_expr_query_source *),
        _Alignof(orm_sql_expr_query_source *),plan->budget,&state->query_bytes,error);
    if(status==TURBODB_STATUS_OK) status=dependent_steps(state,queries->count,error);
    if(status==TURBODB_STATUS_OK) memcpy(vec_data(&state->queries),queries->items,queries->count*sizeof(orm_sql_expr_query_source *));
  }
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<parameter_count;++i) {
    orm_sql_predicate validator; turbodb_value_t ignored;
    status=orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,*(const orm_sql_type *)vec_at_const(&plan->parameter_types,i),NULL,&validator,error);
    if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_predicate_eval(&validator,&parameters[i],NULL,plan->budget,&ignored,error);
  }
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&out->nodes,plan->count,sizeof(dependent_node),_Alignof(dependent_node),
      plan->budget,&out->node_bytes,error);
  if(status==TURBODB_STATUS_OK) status=dependent_steps(state,plan->count,error);
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<plan->count;++i) {
    dependent_node *node=dependent_at(state,i); node->parent=SIZE_MAX; node->end=i+1; node->ordinal=i; node->state=state;
  }
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<plan->count;++i) {
    const orm_sql_from_node *bound=orm_tidesdb_sql_from_at(plan,i);
    dependent_node *node=dependent_at(state,i);
    if(i&&node->parent==SIZE_MAX) { status=dependent_error(error,TURBODB_STATUS_INVALID_STATE,"dependent FROM tree is disconnected"); break; }
    if(!bound->leaf) {
      if(bound->left<=i||bound->right<=i||bound->left>=plan->count||bound->right>=plan->count||bound->left==bound->right||
          dependent_at(state,bound->left)->parent!=SIZE_MAX||dependent_at(state,bound->right)->parent!=SIZE_MAX) {
        status=dependent_error(error,TURBODB_STATUS_INVALID_STATE,"dependent FROM child ordinals are invalid"); break;
      }
      dependent_at(state,bound->left)->parent=i; dependent_at(state,bound->right)->parent=i;
    } else {
      if(bound->table>=count) { status=dependent_error(error,TURBODB_STATUS_INVALID_STATE,"dependent FROM table ordinal is invalid"); break; }
      node->input=inputs[bound->table];
      if(bound->lateral?(!node->input.binding.open||!node->input.binding.close):
          (!node->input.rewind||node->input.binding.open||node->input.binding.close||node->input.binding.context||
           node->input.capture_count||node->input.capture_types)) {
        status=dependent_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"dependent FROM requires complete provider callbacks"); break;
      }
      for(size_t j=0;status==TURBODB_STATUS_OK&&j<bound->table;++j) {
        status=dependent_steps(state,1,error);
        if(node->input.source==inputs[j].source) status=dependent_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"dependent FROM requires independent sources");
      }
    }
    if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&node->types,bound->schema.count,sizeof(orm_sql_type),_Alignof(orm_sql_type),
        plan->budget,&node->type_bytes,error);
    if(status==TURBODB_STATUS_OK) status=dependent_steps(state,bound->schema.count,error);
    for(size_t j=0;status==TURBODB_STATUS_OK&&j<bound->schema.count;++j)
      *(orm_sql_type *)vec_at(&node->types,j)=bound->schema.columns[j].type;
    node->source=(orm_sql_row_source){plan->budget,vec_data_const(&node->types),bound->schema.count,node,dependent_pull,false};
    if(status==TURBODB_STATUS_OK&&bound->leaf) status=dependent_source_check(node,error);
  }
  for(size_t i=plan->count;status==TURBODB_STATUS_OK&&i;--i) {
    dependent_node *node=dependent_at(state,i-1); const orm_sql_from_node *bound=orm_tidesdb_sql_from_at(plan,i-1);
    if(bound->leaf) node->contains_lateral=bound->lateral;
    else {
      const dependent_node *left=dependent_at(state,bound->left),*right=dependent_at(state,bound->right);
      if(bound->left!=i||bound->right!=left->end) {
        status=dependent_error(error,TURBODB_STATUS_INVALID_STATE,"dependent FROM preorder is invalid"); break;
      }
      node->end=right->end; node->contains_lateral=left->contains_lateral||right->contains_lateral;
    }
  }
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<plan->count;++i)
    if(orm_tidesdb_sql_from_at(plan,i)->lateral) status=dependent_routes(dependent_at(state,i),error);
  if(status==TURBODB_STATUS_OK) out->source=&dependent_at(state,0)->source;
  else {
    const turbodb_status_t released=orm_sql_from_dependent_close(out,NULL);
    if(released!=TURBODB_STATUS_OK) return released;
  }
  return status;
}
