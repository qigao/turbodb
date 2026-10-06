#include "from.h"
#include "work.h"
#include "error.h"

typedef struct from_run_node {
  orm_sql_join join;
  orm_sql_scan projection;
  orm_sql_row_source adapter, *source;
  vec_t types;
  size_t type_bytes;
} from_run_node;
static turbodb_status_t from_run_error(turbodb_error_t *error, turbodb_status_t status, const char *reason) {
  tdsql_error_set(error,status,reason); return status;
}
static turbodb_status_t from_run_pull(void *context, const turbodb_value_t **out, turbodb_error_t *error) {
  orm_sql_scan_row row;
  const turbodb_status_t status = orm_tidesdb_sql_scan_next(context,&row,error);
  if (status == TURBODB_STATUS_OK) *out = row.state == ORM_SQL_SCAN_ROW ? row.values : NULL;
  return status;
}
turbodb_status_t orm_tidesdb_sql_from_close(orm_sql_from_run *run, turbodb_error_t *error) {
  if (!run || !run->plan) return TURBODB_STATUS_OK;
  if(run->dependent_state.initialized||run->dependent_bytes)
    return orm_sql_from_dependent_close(run,error);
  if (run->source && run->source->active)
    return from_run_error(error,TURBODB_STATUS_BUSY,"FROM run has an active consumer");
  for (size_t i = 0; i < vec_size(&run->nodes); ++i) {
    const from_run_node *node = vec_at_const(&run->nodes,i);
    if (node->join.evaluating || node->projection.evaluating)
      return from_run_error(error,TURBODB_STATUS_BUSY,"FROM run is evaluating");
  }
  orm_tidesdb_sql_budget *budget = run->plan->budget;
  turbodb_status_t status = TURBODB_STATUS_OK;
  /* Preorder closes parent input leases before their child operators. */
  for (size_t i = 0; i < vec_size(&run->nodes); ++i) {
    from_run_node *node = vec_at(&run->nodes,i);
    turbodb_status_t released = orm_tidesdb_sql_scan_close(&node->projection,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
    released = orm_tidesdb_sql_join_close(&node->join,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
    released = orm_sql_work_release(&node->types,node->type_bytes,budget,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  turbodb_status_t released = orm_sql_work_release(&run->nodes,run->node_bytes,budget,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  if (run->metadata_bytes) {
    released = orm_tidesdb_sql_budget_release(budget,ORM_SQL_BUDGET_WORK_BYTES,run->metadata_bytes,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  --run->plan->active_runs; *run = (orm_sql_from_run){0}; return status;
}
static turbodb_status_t from_run_projection(orm_sql_from_run *run, const orm_sql_from_node *plan,
    from_run_node *node, orm_sql_row_source *source, turbodb_error_t *error) {
  orm_tidesdb_sql_budget *budget = run->plan->budget;
  const size_t width = plan->schema.count;
  vec_t mapping = {0}; size_t bytes = 0;
  turbodb_status_t status = orm_sql_work_zero(&mapping,width,sizeof(size_t),_Alignof(size_t),budget,&bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&node->types,width,sizeof(orm_sql_type),
      _Alignof(orm_sql_type),budget,&node->type_bytes,error);
  const size_t left = plan->leaf ? width : orm_tidesdb_sql_from_at(run->plan,plan->left)->schema.count;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < width; ++i) {
    *(size_t *)vec_at(&mapping,i) = plan->leaf ? i : i < left ? width-left+i : i-left;
    *(orm_sql_type *)vec_at(&node->types,i) = plan->schema.columns[i].type;
  }
  const orm_sql_scan_spec spec = {.projection=vec_data_const(&mapping),.projection_count=width,.limit=UINT64_MAX};
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_scan_open_source(source,&spec,budget,&node->projection,error);
  const turbodb_status_t released = orm_sql_work_release(&mapping,bytes,budget,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  if (status == TURBODB_STATUS_OK) {
    node->adapter = (orm_sql_row_source){budget,vec_data_const(&node->types),width,&node->projection,from_run_pull,false};
    node->source = &node->adapter;
  }
  return status;
}
static turbodb_status_t from_run_validate(orm_sql_from *plan, orm_sql_row_source *const *sources,
    size_t count, const turbodb_value_t *parameters, size_t parameter_count, turbodb_error_t *error) {
  if (!plan || !plan->budget || !plan->count || !sources || count != plan->tables ||
      parameter_count != vec_size(&plan->parameter_types) || (parameter_count && !parameters))
    return from_run_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid FROM run inputs");
  if(!plan->conditions_bound)
    return from_run_error(error,TURBODB_STATUS_INVALID_STATE,"FROM condition binding is incomplete");
  if(plan->contains_lateral)
    return from_run_error(error,TURBODB_STATUS_UNSUPPORTED,"LATERAL execution is not supported");
  if (plan->active_runs == SIZE_MAX) return from_run_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"FROM run lease overflow");
  for (size_t i = 0; i < parameter_count; ++i) {
    orm_sql_predicate validator; turbodb_value_t ignored;
    turbodb_status_t status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,
        *(const orm_sql_type *)vec_at_const(&plan->parameter_types,i),NULL,&validator,error);
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_predicate_eval(&validator,&parameters[i],NULL,plan->budget,&ignored,error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  for (size_t i = 0; i < plan->count; ++i) {
    const orm_sql_from_node *node = orm_tidesdb_sql_from_at(plan,i);
    if (!node->leaf) continue;
    orm_sql_row_source *source = sources[node->table];
    if (!source || !source->next || !source->types || source->budget != plan->budget || source->columns != node->schema.count)
      return from_run_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"FROM source differs from bound schema");
    if (source->active) return from_run_error(error,TURBODB_STATUS_BUSY,"FROM source already consumed");
    for (size_t j = 0; j < node->table; ++j) {
      orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
      const turbodb_status_t status = orm_tidesdb_sql_budget_reserve(plan->budget,&amount,error);
      if (status != TURBODB_STATUS_OK) return status;
      if (source == sources[j]) return from_run_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"FROM requires independent table sources");
    }
    orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = source->columns;
    const turbodb_status_t status = orm_tidesdb_sql_budget_reserve(plan->budget,&amount,error);
    if (status != TURBODB_STATUS_OK) return status;
    for (size_t j = 0; j < source->columns; ++j)
      if (source->types[j].kind != node->schema.columns[j].type.kind || source->types[j].nullable != node->schema.columns[j].type.nullable)
        return from_run_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"FROM source type differs from bound schema");
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t from_open(orm_sql_from *plan,
    orm_sql_row_source *const *sources, size_t count, const turbodb_value_t *parameters,
    size_t parameter_count, const orm_sql_expr_query_sources *queries, orm_sql_from_run *out, turbodb_error_t *error) {
  if (!out || out->plan) return from_run_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"empty FROM run required");
  turbodb_status_t status = from_run_validate(plan,sources,count,parameters,parameter_count,error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_from_run run = {.plan=plan}; ++plan->active_runs;
  status = orm_tidesdb_sql_budget_reserve_capacity(plan->budget,1,sizeof(run),0,&run.metadata_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run.nodes,plan->count,sizeof(from_run_node),
      _Alignof(from_run_node),plan->budget,&run.node_bytes,error);
  for (size_t i = plan->count; status == TURBODB_STATUS_OK && i; --i) {
    const orm_sql_from_node *bound = orm_tidesdb_sql_from_at(plan,i-1);
    from_run_node *node = vec_at(&run.nodes,i-1);
    if (bound->leaf) status = from_run_projection(&run,bound,node,sources[bound->table],error);
    else {
      from_run_node *left = vec_at(&run.nodes,bound->reversed ? bound->right : bound->left);
      from_run_node *right = vec_at(&run.nodes,bound->reversed ? bound->left : bound->right);
      const orm_sql_join_spec spec = {bound->kind,bound->condition.budget ? (orm_sql_expr *)&bound->condition : NULL,
        vec_data_const(&bound->slots),vec_size(&bound->slots),parameters,vec_data_const(&plan->parameter_types),parameter_count,
        queries ? *queries : (orm_sql_expr_query_sources){0},vec_data_const(&bound->query_slots),vec_size(&bound->query_slots),
        .match=bound->common ? ORM_SQL_JOIN_MATCH_USING : ORM_SQL_JOIN_MATCH_ON,.keys=vec_data_const(&bound->keys),.key_count=bound->key_count};
      status = orm_tidesdb_sql_join_open(left->source,right->source,&spec,&node->join,error);
      if (status == TURBODB_STATUS_OK) node->source = orm_tidesdb_sql_join_source(&node->join);
      if (status == TURBODB_STATUS_OK && bound->reversed) status = from_run_projection(&run,bound,node,node->source,error);
    }
  }
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t released = orm_tidesdb_sql_from_close(&run,NULL);
    return released == TURBODB_STATUS_OK ? status : released;
  }
  run.source = ((from_run_node *)vec_at(&run.nodes,0))->source; *out = run; return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_from_open(orm_sql_from *plan,
    orm_sql_row_source *const *sources, size_t count, const turbodb_value_t *parameters,
    size_t parameter_count, orm_sql_from_run *out, turbodb_error_t *error) {
  return from_open(plan,sources,count,parameters,parameter_count,NULL,out,error);
}
turbodb_status_t orm_tidesdb_sql_from_open_queries(orm_sql_from *plan,
    orm_sql_row_source *const *sources, size_t count, const turbodb_value_t *parameters,
    size_t parameter_count, const orm_sql_expr_query_sources *queries, orm_sql_from_run *out, turbodb_error_t *error) {
  return from_open(plan,sources,count,parameters,parameter_count,queries,out,error);
}
