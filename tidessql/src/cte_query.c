#include "cte_query.h"
#include "work.h"
#include "error.h"

typedef struct cte_query_proxy {
  orm_sql_row_source source;
  orm_sql_cte_frontier_reader reader;
  orm_sql_from_input input;
} cte_query_proxy;
static turbodb_status_t cte_query_error(turbodb_error_t *error,turbodb_status_t status,const char *reason) {
  tdsql_error_set(error,status,reason); return status;
}
static turbodb_status_t cte_query_steps(orm_tidesdb_sql_budget *budget,size_t steps,turbodb_error_t *error) {
  orm_sql_budget_amount charge={0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=steps;
  return orm_tidesdb_sql_budget_reserve(budget,&charge,error);
}
static turbodb_status_t cte_query_pull(void *context,const turbodb_value_t **out,turbodb_error_t *error) {
  orm_sql_compound *plan=context; orm_sql_scan_row row;
  const turbodb_status_t status=orm_tidesdb_sql_scan_next(plan->scan,&row,error);
  if(status!=TURBODB_STATUS_OK) return status;
  if(row.state==ORM_SQL_SCAN_CANCELLED)
    return cte_query_error(error,TURBODB_STATUS_INVALID_STATE,"recursive CTE query was cancelled");
  *out=row.state==ORM_SQL_SCAN_ROW?row.values:NULL; return TURBODB_STATUS_OK;
}
static turbodb_status_t cte_query_proxy_pull(void *context,const turbodb_value_t **out,turbodb_error_t *error) {
  cte_query_proxy *proxy=context;
  if(!proxy->reader.store) return cte_query_error(error,TURBODB_STATUS_INVALID_STATE,"CTE self source is outside its iteration");
  return proxy->reader.source.next(proxy->reader.source.context,out,error);
}
static turbodb_status_t cte_query_round_close(void *context,turbodb_error_t *error) {
  orm_sql_cte_query *query=context;
  turbodb_status_t status=orm_sql_compound_execution_close(&query->recursive,error);
  if(status==TURBODB_STATUS_OK && query->inputs_open) {
    status=query->rounds.close(query->rounds.context,error);
    if(status==TURBODB_STATUS_OK) query->inputs_open=false;
  }
  for(size_t i=0;status==TURBODB_STATUS_OK && i<vec_size(&query->proxies);++i)
    status=orm_sql_cte_frontier_close(&((cte_query_proxy *)vec_at(&query->proxies,i))->reader,error);
  return status;
}
static turbodb_status_t cte_query_proxy_rewind(void *context,turbodb_error_t *error) {
  cte_query_proxy *proxy=context;
  if(proxy->source.active) return cte_query_error(error,TURBODB_STATUS_BUSY,"CTE self source has an active consumer");
  return orm_sql_cte_frontier_rewind(&proxy->reader,error);
}
static turbodb_status_t cte_query_round_open(void *context,orm_sql_row_source *frontier,
    orm_sql_row_source **out,turbodb_error_t *error) {
  orm_sql_cte_query *query=context;
  turbodb_status_t status=cte_query_steps(query->budget,vec_size(&query->proxies),error);
  for(size_t i=0;status==TURBODB_STATUS_OK && i<vec_size(&query->proxies);++i)
    status=orm_sql_cte_frontier_open(frontier,&((cte_query_proxy *)vec_at(&query->proxies,i))->reader,error);
  const turbodb_value_t *parameters=vec_data_const(&query->parameters.values);
  if(status==TURBODB_STATUS_OK && query->rounds.open) {
    query->inputs_open=true;
    status=query->rounds.open(query->rounds.context,parameters,query->parameter_count,error);
  }
  const orm_sql_expr_query_sources sources={vec_data_const(&query->sources),vec_size(&query->sources),query->evaluation};
  if(status==TURBODB_STATUS_OK) status=orm_sql_compound_execution_open(&query->recursive,parameters,
      query->parameter_count,&sources,error);
  if(status==TURBODB_STATUS_OK) *out=&query->recursive_source;
  return status;
}
turbodb_status_t orm_sql_cte_query_execution_close(orm_sql_cte_query *query,turbodb_error_t *error) {
  if(!query||!query->budget||query->describe||!query->complete)
    return cte_query_error(error,TURBODB_STATUS_INVALID_STATE,"recursive CTE execution owner required");
  turbodb_status_t status=orm_sql_cte_store_close(&query->cache,error);
  if(status==TURBODB_STATUS_OK) status=cte_query_round_close(query,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_compound_execution_close(&query->initial,error);
  if(status!=TURBODB_STATUS_OK) return status;
  for(size_t i=query->marker_count;i<query->parameter_count;++i)
    *(turbodb_value_t *)vec_at(&query->parameters.values,i)=turbodb_null();
  query->execution_closed=true; return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_cte_query_execution_open(orm_sql_cte_query *query,
    const turbodb_value_t *parameters,size_t count,turbodb_error_t *error) {
  if(!query||!query->budget||query->describe||!query->complete||count!=query->parameter_count||(count&&!parameters))
    return cte_query_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid recursive CTE replay parameters");
  if(!query->execution_closed)
    return cte_query_error(error,TURBODB_STATUS_BUSY,"recursive CTE execution is already open");
  turbodb_status_t status=cte_query_steps(query->budget,count-query->marker_count,error);
  if(status!=TURBODB_STATUS_OK) return status;
  query->execution_closed=false;
  for(size_t i=query->marker_count;i<count;++i)
    *(turbodb_value_t *)vec_at(&query->parameters.values,i)=parameters[i];
  const orm_sql_expr_query_sources sources={vec_data_const(&query->sources),vec_size(&query->sources),query->evaluation};
  status=orm_sql_compound_execution_open(&query->initial,vec_data_const(&query->parameters.values),
      count,&sources,error);
  if(status==TURBODB_STATUS_OK)
    status=orm_sql_cte_store_open_recursive(&query->initial_source,&query->recursion,&query->cache,error);
  return status;
}
turbodb_status_t orm_sql_cte_query_close(orm_sql_cte_query *query,turbodb_error_t *error) {
  if(!query || !query->budget) return TURBODB_STATUS_OK;
  turbodb_status_t status=orm_tidesdb_sql_scan_close(&query->explained_page,error);
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_explain_close(&query->pagination,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_cte_store_close(&query->cache,error);
  if(status!=TURBODB_STATUS_OK) return status;
  status=orm_tidesdb_sql_compound_close(&query->recursive,error);
  if(status!=TURBODB_STATUS_OK) return status;
  status=orm_tidesdb_sql_compound_close(&query->initial,error);
  if(status!=TURBODB_STATUS_OK) return status;
  status=orm_sql_cte_schema_close(&query->schema,error);
  vec_t *vectors[]={&query->proxies,&query->sources,&query->parameters.values};
  const size_t bytes[]={query->proxy_bytes,query->source_bytes,query->parameters.bytes};
  for(size_t i=0;i<sizeof(vectors)/sizeof(vectors[0]);++i) {
    const turbodb_status_t released=orm_sql_work_release(vectors[i],bytes[i],query->budget,status==TURBODB_STATUS_OK?error:NULL);
    if(status==TURBODB_STATUS_OK) status=released;
  }
  if(query->metadata_bytes) {
    const turbodb_status_t released=orm_tidesdb_sql_budget_release(query->budget,ORM_SQL_BUDGET_WORK_BYTES,
        query->metadata_bytes,status==TURBODB_STATUS_OK?error:NULL);
    if(status==TURBODB_STATUS_OK) status=released;
  }
  *query=(orm_sql_cte_query){0}; return status;
}
static int cte_binding_compare(const void *left,const void *right) {
  const sqlparser_id a=((const orm_sql_derived_binding *)left)->node,b=((const orm_sql_derived_binding *)right)->node;
  return a<b?-1:a>b?1:0;
}
static bool cte_binding_copy(void *to,const void *from) { *(orm_sql_derived_binding *)to=*(const orm_sql_derived_binding *)from; return true; }
static void cte_binding_move(void *to,void *from) { *(orm_sql_derived_binding *)to=*(orm_sql_derived_binding *)from; }
/* Registry records borrow sources and schema; destroying sort copies releases nothing. */
static void cte_binding_destroy(void *value) { (void)value; }
static const cmeta_type_traits cte_binding_traits={
  CMETA_TRAIT_COMPARE|CMETA_TRAIT_COPY|CMETA_TRAIT_MOVE|CMETA_TRAIT_DESTROY|
    CMETA_TRAIT_TRIVIAL_COPY|CMETA_TRAIT_TRIVIAL_DESTROY,
  NULL,NULL,cte_binding_compare,cte_binding_copy,cte_binding_move,cte_binding_destroy};
static const cmeta_type_desc cte_binding_type={"cte_query_binding",sizeof(orm_sql_derived_binding),
  _Alignof(orm_sql_derived_binding),CMETA_T_OBJECT,NULL,&cte_binding_traits};

static turbodb_status_t cte_query_bindings(const orm_sql_query_scope *scope,const orm_sql_cte_shape *shape,
    orm_sql_cte_query *query,vec_t *bindings,size_t *bytes,size_t *binding_count,turbodb_error_t *error) {
  if(shape->recursive_members>SIZE_MAX-scope->derived_count)
    return cte_query_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"CTE FROM registry capacity overflow");
  const size_t capacity=shape->recursive_members+scope->derived_count;
  turbodb_status_t status=cte_query_steps(query->budget,scope->derived_count,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&query->proxies,shape->recursive_members,sizeof(cte_query_proxy),
      _Alignof(cte_query_proxy),query->budget,&query->proxy_bytes,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(bindings,capacity,sizeof(orm_sql_derived_binding),
      _Alignof(orm_sql_derived_binding),query->budget,bytes,error);
  size_t count=0;
  for(size_t i=0;status==TURBODB_STATUS_OK&&i<scope->derived_count;++i) {
    bool self=false;
    status=cte_query_steps(query->budget,shape->count,error);
    for(size_t j=0;status==TURBODB_STATUS_OK&&j<shape->count;++j)
      if(((const orm_sql_cte_shape_node *)vec_at_const(&shape->nodes,j))->self==scope->derived[i].node) self=true;
    if(status==TURBODB_STATUS_OK&&self) {
      const orm_sql_derived_binding *binding=&scope->derived[i];
      if(binding->source||!binding->schema||binding->schema->columns!=query->schema.schema.columns||
          binding->schema->count!=query->schema.schema.count)
        status=cte_query_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"recursive self registry requires the seed's metadata only");
      if(status==TURBODB_STATUS_OK) status=cte_query_steps(query->budget,i,error);
      for(size_t j=0;status==TURBODB_STATUS_OK&&j<i;++j)
        if(scope->derived[j].node==binding->node)
          status=cte_query_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"duplicate recursive self registry identity");
    }
    if(!self&&status==TURBODB_STATUS_OK) *(orm_sql_derived_binding *)vec_at(bindings,count++)=scope->derived[i];
  }
  if(status==TURBODB_STATUS_OK) status=cte_query_steps(query->budget,shape->count,error);
  size_t member=0;
  for(size_t i=0;status==TURBODB_STATUS_OK && i<shape->count;++i) {
    const orm_sql_cte_shape_node *node=vec_at_const(&shape->nodes,i); if(!node->self) continue;
    cte_query_proxy *proxy=vec_at(&query->proxies,member);
    proxy->source=(orm_sql_row_source){query->budget,vec_data_const(&query->schema.types),query->schema.schema.count,
        proxy,cte_query_proxy_pull,false};
    proxy->input=(orm_sql_from_input){.source=&proxy->source,.context=proxy,.rewind=cte_query_proxy_rewind};
    *(orm_sql_derived_binding *)vec_at(bindings,count++)=
        (orm_sql_derived_binding){node->self,&query->schema.schema,query->describe?NULL:&proxy->source,
            query->describe?NULL:&proxy->input};
    ++member;
  }
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_sort(vec_data(bindings),count,&cte_binding_type,1,query->budget,error);
  if(status==TURBODB_STATUS_OK) status=cte_query_steps(query->budget,count,error);
  for(size_t i=0;status==TURBODB_STATUS_OK && i<count;++i) {
    const orm_sql_derived_binding *binding=vec_at_const(bindings,i);
    if(!binding->node || (i && binding->node==((const orm_sql_derived_binding *)vec_at_const(bindings,i-1))->node))
      status=cte_query_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"CTE FROM registry contains missing or duplicate identity");
  }
  if(status==TURBODB_STATUS_OK) *binding_count=count;
  return status;
}
static turbodb_status_t cte_query_rules(const orm_sql_query_scope *scope,const orm_sql_cte_shape *shape,
    bool *distinct,turbodb_error_t *error) {
  if(!shape->recursive_members) return cte_query_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"recursive CTE definition required");
  turbodb_status_t status=cte_query_steps(scope->budget,shape->count,error); *distinct=false;
  for(size_t i=0;status==TURBODB_STATUS_OK && i<shape->count;++i) {
    const orm_sql_cte_shape_node *node=vec_at_const(&shape->nodes,i);
    if(node->self && node->union_before && !sqlparser_get_node(scope->document,node->union_before)->as.compound.all)
      *distinct=true;
  }
  return status;
}
/* Fold inner-to-outer whole-definition pages. A UNION ancestor means the tail
 * covers only a subset of members and cannot be applied to this single cache. */
static turbodb_status_t cte_query_page(const orm_sql_query_scope *scope,const orm_sql_cte_shape *shape,
    const turbodb_value_t *parameters,orm_sql_cte_query *query,orm_sql_cte_recursion *recursion,turbodb_error_t *error) {
  orm_sql_select tail={0}; turbodb_status_t status=cte_query_steps(scope->budget,shape->count,error);
  uint64_t offset=0,limit=UINT64_MAX;
  for(size_t i=shape->count;status==TURBODB_STATUS_OK && i;--i) {
    const orm_sql_cte_shape_node *node=vec_at_const(&shape->nodes,i-1);
    if(node->parts!=ORM_SQL_CTE_MIXED_PARTS) continue;
    const sqlparser_node *ast=sqlparser_get_node(scope->document,node->ast);
    const sqlparser_id page=ast->kind==SQLPARSER_UNION?ast->as.compound.limit:
        ast->kind==SQLPARSER_QUERY_GROUP?ast->as.query_group.limit:0;
    if(!page) continue;
    for(size_t parent=node->parent;status==TURBODB_STATUS_OK && parent!=SIZE_MAX;) {
      status=cte_query_steps(scope->budget,1,error);
      const orm_sql_cte_shape_node *ancestor=vec_at_const(&shape->nodes,parent);
      if(status==TURBODB_STATUS_OK && sqlparser_get_node(scope->document,ancestor->ast)->kind==SQLPARSER_UNION)
        status=cte_query_error(error,TURBODB_STATUS_UNSUPPORTED,"recursive pagination must cover the complete CTE");
      parent=ancestor->parent;
    }
    if(status!=TURBODB_STATUS_OK) break;
    status=orm_tidesdb_sql_select_destroy(&tail,error);
    orm_sql_query_scope child=*scope; child.root=node->ast; child.anonymous_output=true;
    if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_select_bind_tail(&child,&query->schema.schema,
        (sqlparser_list){0},page,&tail,error);
    uint64_t skip=0,take=0;
    if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_select_validate_parameters(&tail,parameters,vec_size(&tail.parameter_types),&skip,&take,error);
    if(status!=TURBODB_STATUS_OK) break;
    if(skip>limit) skip=limit;
    limit-=skip; if(take<limit) limit=take;
    if(!limit) offset=0;
    else if(skip>UINT64_MAX-offset) status=cte_query_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"recursive pagination offset overflow");
    else offset+=skip;
    recursion->paged=true;
  }
  recursion->offset=offset; recursion->limit=limit;
  if(status==TURBODB_STATUS_OK && recursion->paged && query->describe) {
    tail.offset=(orm_sql_select_bound){.literal=offset}; tail.limit=(orm_sql_select_bound){.literal=limit};
    status=orm_tidesdb_sql_explain_open(&tail,(vstr){0},parameters,vec_size(&tail.parameter_types),&query->pagination,error);
    if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_explain_block(&query->pagination,ORM_SQL_EXPLAIN_QUERY_GROUP,1,error);
    size_t projection[ORM_SQL_EXPLAIN_COLUMNS];
    for(size_t i=0;i<ORM_SQL_EXPLAIN_COLUMNS;++i) projection[i]=i;
    const orm_sql_scan_spec spec={.projection=projection,.projection_count=ORM_SQL_EXPLAIN_COLUMNS,.limit=UINT64_MAX};
    if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_scan_open_source(&query->pagination.source,&spec,scope->budget,&query->explained_page,error);
  }
  const turbodb_status_t released=orm_tidesdb_sql_select_destroy(&tail,status==TURBODB_STATUS_OK?error:NULL);
  return status==TURBODB_STATUS_OK?released:status;
}
turbodb_status_t orm_sql_cte_query_seed_open(const orm_sql_query_scope *scope,sqlparser_id definition,
    const vec_t *references,orm_sql_catalog_store *owner,const turbodb_value_t *parameters,
    const orm_sql_cte_query_spec *spec,orm_sql_cte_query *out,turbodb_error_t *error) {
  if(!scope || !scope->document || !scope->budget || !owner || scope->budget!=owner->budget ||
      !spec || !spec->max_iterations || !out || out->budget || (scope->parameter_count && (!parameters || !scope->parameter_types)) ||
      (scope->outer_schema&&scope->outer_schema->count&&!parameters) ||
      (scope->derived_count && !scope->derived) ||
      ((spec->rounds.open || spec->rounds.close || spec->rounds.context) &&
       (!spec->rounds.open || !spec->rounds.close || !spec->rounds.context)) ||
      (!spec->describe && scope->derived_count && !spec->rounds.open) ||
      (!spec->describe && scope->query_count && (!spec->sources || spec->sources->count!=scope->query_count || !spec->sources->items)))
    return cte_query_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid recursive CTE query inputs");
  orm_sql_cte_shape shape={0}; bool distinct=false;
  turbodb_status_t status=orm_sql_cte_shape_bind(scope,definition,references,&shape,error);
  if(status==TURBODB_STATUS_OK) status=cte_query_rules(scope,&shape,&distinct,error);
  if(status==TURBODB_STATUS_OK) {
    *out=(orm_sql_cte_query){.budget=scope->budget,.describe=spec->describe,.rounds=spec->rounds,
        .evaluation=spec->sources?spec->sources->evaluation:scope->evaluation,
        .marker_count=scope->parameter_count,.binding_document=scope->document,.binding_definition=definition,
        .recursion={.max_iterations=spec->max_iterations},.execution_closed=true};
    status=orm_tidesdb_sql_budget_reserve_capacity(out->budget,1,sizeof(*out),0,&out->metadata_bytes,error);
  }
  orm_sql_query_scope child=*scope;
  if(status==TURBODB_STATUS_OK) {
    const sqlparser_node *cte=sqlparser_get_node(scope->document,definition);
    child.root=cte->as.cte.query; child.demand=ORM_SQL_QUERY_VALUES; child.scalar_output=false;
    child.anonymous_output=cte->as.cte.columns.count!=0; child.in_query=false;
    child.defer_execution=!spec->describe;
    status=orm_sql_compound_open_cte_part(&child,&shape,ORM_SQL_CTE_INITIAL_PART,owner,parameters,
        spec->describe,spec->sources,&out->initial,error);
  }
  if(status==TURBODB_STATUS_OK) status=orm_sql_cte_schema_bind(scope,definition,out->initial.plan,true,&out->schema,error);
  const turbodb_status_t released=orm_sql_cte_shape_close(&shape,status==TURBODB_STATUS_OK?error:NULL);
  if(status==TURBODB_STATUS_OK) status=released;
  if(status!=TURBODB_STATUS_OK) {
    const turbodb_status_t closed=orm_sql_cte_query_close(out,NULL);
    if(closed!=TURBODB_STATUS_OK) return closed;
  }
  return status;
}
turbodb_status_t orm_sql_cte_query_member_open(const orm_sql_query_scope *scope,sqlparser_id definition,
    const vec_t *references,orm_sql_catalog_store *owner,const turbodb_value_t *parameters,
    const orm_sql_expr_query_sources *sources,orm_sql_cte_query *out,turbodb_error_t *error) {
  if(!scope||!out||out->complete||out->budget!=scope->budget||!owner||owner->budget!=out->budget||
      scope->document!=out->binding_document||definition!=out->binding_definition||
      scope->parameter_count!=out->marker_count||!out->schema.schema.columns||
      (scope->parameter_count&&(!parameters||!scope->parameter_types))||
      (scope->outer_schema&&scope->outer_schema->count&&!parameters)||
      (scope->derived_count&&!scope->derived)||
      (!out->describe&&scope->derived_count&&!out->rounds.open)||
      (!out->describe&&scope->query_count&&(!sources||sources->count!=scope->query_count||!sources->items)))
    return cte_query_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid recursive CTE member binding phase");
  orm_sql_cte_shape shape={0}; vec_t bindings={0}; size_t bytes=0,binding_count=0; bool distinct=false;
  turbodb_status_t status=orm_sql_cte_shape_bind(scope,definition,references,&shape,error);
  if(status==TURBODB_STATUS_OK) status=cte_query_rules(scope,&shape,&distinct,error);
  orm_sql_query_scope child=*scope;
  const sqlparser_node *cte=sqlparser_get_node(scope->document,definition);
  child.root=cte->as.cte.query; child.demand=ORM_SQL_QUERY_VALUES; child.scalar_output=false;
  child.anonymous_output=cte->as.cte.columns.count!=0; child.in_query=false; child.defer_execution=!out->describe;
  if(status==TURBODB_STATUS_OK) status=cte_query_bindings(scope,&shape,out,&bindings,&bytes,&binding_count,error);
  if(status==TURBODB_STATUS_OK) {
    child.derived=vec_data_const(&bindings); child.derived_count=binding_count;
    status=orm_sql_compound_open_cte_part(&child,&shape,ORM_SQL_CTE_RECURSIVE_PART,owner,parameters,
        out->describe,sources,&out->recursive,error);
  }
  if(status==TURBODB_STATUS_OK) status=orm_sql_cte_schema_member(&out->schema,out->recursive.plan,error);
  if(status==TURBODB_STATUS_OK&&(vec_size(&out->initial.plan->parameter_types)<scope->parameter_count||
      vec_size(&out->initial.plan->parameter_types)!=vec_size(&out->recursive.plan->parameter_types)))
    status=cte_query_error(error,TURBODB_STATUS_INVALID_STATE,"recursive CTE parameter layouts differ");
  orm_sql_cte_recursion recursion={out,cte_query_round_open,cte_query_round_close,out->recursion.max_iterations,distinct};
  if(status==TURBODB_STATUS_OK) status=cte_query_page(scope,&shape,parameters,out,&recursion,error);
  if(status==TURBODB_STATUS_OK && !out->describe) {
    out->recursion=recursion;
    out->parameter_count=vec_size(&out->initial.plan->parameter_types);
    const size_t captured=out->parameter_count-out->marker_count;
    status=orm_sql_compound_execution_close(&out->recursive,error);
    if(status==TURBODB_STATUS_OK && out->marker_count)
      status=orm_sql_snapshot_copy(&out->parameters,parameters,out->marker_count,captured,out->budget,error);
    else if(status==TURBODB_STATUS_OK && captured)
      status=orm_sql_work_zero(&out->parameters.values,captured,sizeof(turbodb_value_t),
          _Alignof(turbodb_value_t),out->budget,&out->parameters.bytes,error);
    if(status==TURBODB_STATUS_OK) status=cte_query_steps(out->budget,captured,error);
    for(size_t i=0;status==TURBODB_STATUS_OK&&i<captured;++i)
      *(turbodb_value_t *)vec_at(&out->parameters.values,out->marker_count+i)=parameters[out->marker_count+i];
    if(status==TURBODB_STATUS_OK && scope->query_count) {
      status=cte_query_steps(out->budget,scope->query_count,error);
      if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&out->sources,scope->query_count,sizeof(orm_sql_expr_query_source *),
          _Alignof(orm_sql_expr_query_source *),out->budget,&out->source_bytes,error);
      if(status==TURBODB_STATUS_OK) for(size_t i=0;i<scope->query_count;++i)
        *(orm_sql_expr_query_source **)vec_at(&out->sources,i)=sources->items[i];
    }
    out->initial_source=(orm_sql_row_source){out->budget,vec_data_const(&out->schema.types),out->schema.schema.count,
        &out->initial,cte_query_pull,false};
    out->recursive_source=(orm_sql_row_source){out->budget,vec_data_const(&out->schema.types),out->schema.schema.count,
        &out->recursive,cte_query_pull,false};
    if(status==TURBODB_STATUS_OK&&!scope->defer_execution)
      status=orm_sql_compound_execution_open(&out->initial,vec_data_const(&out->parameters.values),
          out->parameter_count,sources,error);
    if(status==TURBODB_STATUS_OK) status=orm_sql_cte_store_open_recursive(&out->initial_source,&recursion,&out->cache,error);
  }
  turbodb_status_t released=orm_sql_work_release(&bindings,bytes,scope->budget,status==TURBODB_STATUS_OK?error:NULL);
  if(status==TURBODB_STATUS_OK) status=released;
  if(status==TURBODB_STATUS_OK) { out->complete=true; out->binding_document=NULL; out->execution_closed=false; }
  released=orm_sql_cte_shape_close(&shape,status==TURBODB_STATUS_OK?error:NULL);
  if(status==TURBODB_STATUS_OK) status=released;
  if(status!=TURBODB_STATUS_OK) {
    released=orm_sql_cte_query_close(out,NULL); if(released!=TURBODB_STATUS_OK) return released;
  }
  return status;
}
turbodb_status_t orm_sql_cte_query_open(const orm_sql_query_scope *scope,sqlparser_id definition,
    const vec_t *references,orm_sql_catalog_store *owner,const turbodb_value_t *parameters,
    const orm_sql_cte_query_spec *spec,orm_sql_cte_query *out,turbodb_error_t *error) {
  turbodb_status_t status=orm_sql_cte_query_seed_open(scope,definition,references,owner,parameters,spec,out,error);
  if(status==TURBODB_STATUS_OK)
    status=orm_sql_cte_query_member_open(scope,definition,references,owner,parameters,spec->sources,out,error);
  return status;
}
