#include "cte_store.h"
#include "work.h"
#include "error.h"

static turbodb_status_t cte_store_error(turbodb_error_t *error, turbodb_status_t status, const char *message) {
  tdsql_error_set(error,status,message); return status;
}
typedef struct cte_index_row { const turbodb_value_t *values; size_t columns; } cte_index_row;
static int cte_index_compare(const void *left, const void *right) {
  const cte_index_row *a=left, *b=right;
  for(size_t i=0;i<a->columns;++i) {
    const int result=orm_sql_value_order(&a->values[i],&b->values[i]);
    if(result) return result;
  }
  return 0;
}
static bool cte_index_copy(void *to,const void *from) { *(cte_index_row *)to=*(const cte_index_row *)from; return true; }
static void cte_index_move(void *to,void *from) { *(cte_index_row *)to=*(cte_index_row *)from; }
/* Index and sort scratch borrow rows owned exclusively by store.rows. */
static void cte_index_destroy(void *value) { (void)value; }
static const cmeta_type_traits cte_index_traits={
  CMETA_TRAIT_COMPARE|CMETA_TRAIT_COPY|CMETA_TRAIT_MOVE|CMETA_TRAIT_DESTROY|
    CMETA_TRAIT_TRIVIAL_COPY|CMETA_TRAIT_TRIVIAL_DESTROY,
  NULL,NULL,cte_index_compare,cte_index_copy,cte_index_move,cte_index_destroy};
static const cmeta_type_desc cte_index_type={"sql_cte_index_row",sizeof(cte_index_row),
  _Alignof(cte_index_row),CMETA_T_OBJECT,NULL,&cte_index_traits};
static turbodb_status_t cte_store_steps(orm_sql_cte_store *store,size_t steps,turbodb_error_t *error) {
  orm_sql_budget_amount charge={0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=steps;
  return orm_tidesdb_sql_budget_reserve(store->budget,&charge,error);
}
static turbodb_status_t cte_round_close(orm_sql_cte_store *store,turbodb_error_t *error) {
  turbodb_status_t status=orm_tidesdb_sql_scan_close(&store->iteration,error);
  if(status!=TURBODB_STATUS_OK || !store->round_open) return status;
  status=store->recursion.close(store->recursion.context,error);
  if(status!=TURBODB_STATUS_OK) return status;
  if(store->frontier.active || store->frontier_readers)
    return cte_store_error(error,TURBODB_STATUS_BUSY,"recursive member retains frontier consumer or reader");
  store->round_open=false; return TURBODB_STATUS_OK;
}
static turbodb_status_t cte_store_ready(orm_sql_cte_store *store, turbodb_error_t *error) {
  if (!store || !store->budget) return cte_store_error(error,TURBODB_STATUS_INVALID_STATE,"CTE store is closed");
  if (store->evaluating) return cte_store_error(error,TURBODB_STATUS_BUSY,"CTE store is evaluating");
  if (store->state == ORM_SQL_CTE_FAILED)
    return cte_store_error(error,store->failure.status,store->failure.message);
  if (store->state == ORM_SQL_CTE_CANCELLED)
    return cte_store_error(error,TURBODB_STATUS_INVALID_STATE,"CTE store is cancelled");
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_cte_store_close(orm_sql_cte_store *store, turbodb_error_t *error) {
  if (!store || !store->budget) return TURBODB_STATUS_OK;
  if (store->readers || store->evaluating)
    return cte_store_error(error,TURBODB_STATUS_BUSY,"close CTE readers before store");
  /* Failed factory cleanup must leave its borrowed frontier and rows intact. */
  store->evaluating=true;
  turbodb_status_t status=cte_round_close(store,error);
  store->evaluating=false;
  if(status!=TURBODB_STATUS_OK) return status;
  status = orm_tidesdb_sql_scan_close(&store->input,error);
  turbodb_status_t released = orm_sql_rows_close(&store->rows,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  released = orm_sql_work_release(&store->types,store->type_bytes,store->budget,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  released = orm_sql_work_release(&store->index,store->index_bytes,store->budget,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  if (store->metadata_bytes) {
    released = orm_tidesdb_sql_budget_release(store->budget,ORM_SQL_BUDGET_WORK_BYTES,
        store->metadata_bytes,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  *store = (orm_sql_cte_store){0}; return status;
}
static turbodb_status_t cte_store_open(orm_sql_row_source *source,const orm_sql_cte_recursion *recursion,
    orm_sql_cte_store *out, turbodb_error_t *error) {
  if (!source || !source->budget || !source->types || !source->columns || !source->context || !source->next || !out || out->budget)
    return cte_store_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid CTE store input");
  if (source->active) return cte_store_error(error,TURBODB_STATUS_BUSY,"CTE producer already consumed");
  if (source->columns > source->budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
    return cte_store_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"CTE schema exceeds plan capacity");
  *out = (orm_sql_cte_store){.budget=source->budget,.rows={.budget=source->budget},.state=ORM_SQL_CTE_PENDING};
  if(recursion) out->recursion=*recursion;
  tdsql_error_init(&out->failure);
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve_capacity(out->budget,1,sizeof(*out),0,&out->metadata_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&out->types,source->columns,sizeof(orm_sql_type),
      _Alignof(orm_sql_type),out->budget,&out->type_bytes,error);
  vec_t projection = {0}; size_t bytes = 0;
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&projection,source->columns,sizeof(size_t),
      _Alignof(size_t),out->budget,&bytes,error);
  orm_sql_budget_amount charge = {0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = source->columns;
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_budget_reserve(out->budget,&charge,error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < source->columns; ++i) {
    *(orm_sql_type *)vec_at(&out->types,i) = source->types[i];
    if(recursion) ((orm_sql_type *)vec_at(&out->types,i))->nullable=true;
    *(size_t *)vec_at(&projection,i) = i;
  }
  const orm_sql_scan_spec spec = {.projection=vec_data_const(&projection),.projection_count=source->columns,.limit=UINT64_MAX,
      .distinct=recursion && recursion->distinct};
  if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_scan_open_source(source,&spec,out->budget,&out->input,error);
  const turbodb_status_t released = orm_sql_work_release(&projection,bytes,out->budget,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t cleanup = orm_sql_cte_store_close(out,NULL);
    if (cleanup != TURBODB_STATUS_OK) return cleanup;
  }
  return status;
}
turbodb_status_t orm_sql_cte_store_open(orm_sql_row_source *source,
    orm_sql_cte_store *out, turbodb_error_t *error) {
  return cte_store_open(source,NULL,out,error);
}
static turbodb_status_t cte_frontier_pull(void *context,const turbodb_value_t **out,turbodb_error_t *error) {
  orm_sql_cte_store *store=context;
  if(!store || !store->budget || !out || !store->evaluating || !store->round_open)
    return cte_store_error(error,TURBODB_STATUS_INVALID_STATE,"recursive frontier is outside its iteration");
  const turbodb_status_t status=cte_store_steps(store,1,error);
  if(status!=TURBODB_STATUS_OK) return status;
  *out=store->frontier_position==store->frontier_end?NULL:orm_sql_rows_at(&store->rows,store->frontier_position++);
  return TURBODB_STATUS_OK;
}
static turbodb_status_t cte_frontier_reader_pull(void *context,const turbodb_value_t **out,turbodb_error_t *error) {
  orm_sql_cte_frontier_reader *reader=context;
  orm_sql_cte_store *store=reader?reader->store:NULL;
  if(!store || !store->budget || !out || !store->evaluating || !store->round_open || reader->iteration!=store->iterations)
    return cte_store_error(error,TURBODB_STATUS_INVALID_STATE,"recursive frontier reader is outside its iteration");
  const turbodb_status_t status=cte_store_steps(store,1,error);
  if(status==TURBODB_STATUS_OK) *out=reader->position==reader->end?NULL:orm_sql_rows_at(&store->rows,reader->position++);
  return status;
}
turbodb_status_t orm_sql_cte_frontier_open(orm_sql_row_source *frontier,orm_sql_cte_frontier_reader *out,turbodb_error_t *error) {
  if(!frontier || frontier->next!=cte_frontier_pull || !frontier->context || !out || out->store)
    return cte_store_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"empty frontier reader and recursive frontier required");
  orm_sql_cte_store *store=frontier->context;
  if(!store->budget || frontier!=&store->frontier || !store->evaluating || !store->round_open)
    return cte_store_error(error,TURBODB_STATUS_INVALID_STATE,"recursive frontier is outside its iteration");
  if(store->frontier_readers==SIZE_MAX)
    return cte_store_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"recursive frontier reader count overflow");
  turbodb_status_t status=cte_store_steps(store,1,error); size_t bytes=0;
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_budget_reserve_capacity(store->budget,1,sizeof(*out),0,&bytes,error);
  if(status!=TURBODB_STATUS_OK) return status;
  *out=(orm_sql_cte_frontier_reader){.store=store,.position=store->frontier_first,.end=store->frontier_end,
      .metadata_bytes=bytes,.iteration=store->iterations};
  out->source=(orm_sql_row_source){store->budget,frontier->types,frontier->columns,out,cte_frontier_reader_pull,false};
  ++store->frontier_readers; return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_cte_frontier_rewind(orm_sql_cte_frontier_reader *reader,turbodb_error_t *error) {
  orm_sql_cte_store *store=reader?reader->store:NULL;
  if(!store||!store->budget||!store->evaluating||!store->round_open||reader->iteration!=store->iterations)
    return cte_store_error(error,TURBODB_STATUS_INVALID_STATE,"recursive frontier reader is outside its iteration");
  if(reader->source.active)
    return cte_store_error(error,TURBODB_STATUS_BUSY,"recursive frontier reader has an active consumer");
  const turbodb_status_t status=cte_store_steps(store,1,error);
  if(status==TURBODB_STATUS_OK) reader->position=store->frontier_first;
  return status;
}
turbodb_status_t orm_sql_cte_frontier_close(orm_sql_cte_frontier_reader *reader,turbodb_error_t *error) {
  if(!reader || !reader->store) return TURBODB_STATUS_OK;
  if(reader->source.active) return cte_store_error(error,TURBODB_STATUS_BUSY,"recursive frontier reader has an active consumer");
  const turbodb_status_t status=orm_tidesdb_sql_budget_release(reader->store->budget,ORM_SQL_BUDGET_WORK_BYTES,
      reader->metadata_bytes,error);
  --reader->store->frontier_readers; *reader=(orm_sql_cte_frontier_reader){0}; return status;
}
turbodb_status_t orm_sql_cte_store_open_recursive(orm_sql_row_source *seed,
    const orm_sql_cte_recursion *recursion,orm_sql_cte_store *out,turbodb_error_t *error) {
  if(!recursion || !recursion->context || !recursion->open || !recursion->close || !recursion->max_iterations)
    return cte_store_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"recursive CTE requires a bounded member factory");
  const turbodb_status_t status=cte_store_open(seed,recursion,out,error);
  if(status==TURBODB_STATUS_OK) out->frontier=(orm_sql_row_source){out->budget,vec_data_const(&out->types),
      vec_size(&out->types),out,cte_frontier_pull,false};
  return status;
}
static turbodb_status_t cte_index_build(orm_sql_cte_store *store,turbodb_error_t *error) {
  turbodb_status_t status=orm_sql_work_release(&store->index,store->index_bytes,store->budget,error);
  store->index_bytes=0;
  const size_t count=vec_size(&store->rows.snapshots),columns=vec_size(&store->types);
  if(status==TURBODB_STATUS_OK) status=cte_store_steps(store,count,error);
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&store->index,count,sizeof(cte_index_row),
      _Alignof(cte_index_row),store->budget,&store->index_bytes,error);
  if(status!=TURBODB_STATUS_OK) return status;
  for(size_t i=0;i<count;++i) *(cte_index_row *)vec_at(&store->index,i)=(cte_index_row){orm_sql_rows_at(&store->rows,i),columns};
  return orm_sql_work_sort(vec_data(&store->index),count,&cte_index_type,columns,store->budget,error);
}
static turbodb_status_t cte_index_contains(orm_sql_cte_store *store,const turbodb_value_t *row,bool *found,turbodb_error_t *error) {
  size_t first=0,last=vec_size(&store->index);
  const cte_index_row candidate={row,vec_size(&store->types)}; *found=false;
  while(first<last) {
    const turbodb_status_t status=cte_store_steps(store,candidate.columns,error);
    if(status!=TURBODB_STATUS_OK) return status;
    const size_t middle=first+(last-first)/2;
    const int compared=cte_index_compare(&candidate,vec_at_const(&store->index,middle));
    if(!compared) { *found=true; break; }
    if(compared<0) last=middle; else first=middle+1;
  }
  return TURBODB_STATUS_OK;
}
static bool cte_page_full(const orm_sql_cte_store *store) {
  if(!store->recursion.paged) return false;
  const uint64_t count=vec_size(&store->rows.snapshots);
  return !store->recursion.limit || (count>=store->recursion.offset && count-store->recursion.offset>=store->recursion.limit);
}
static turbodb_status_t cte_store_batch(orm_sql_cte_store *store,orm_sql_scan *input,turbodb_error_t *error) {
  while (!cte_page_full(store)) {
    orm_sql_scan_row row;
    turbodb_status_t status = orm_tidesdb_sql_scan_next(input,&row,error);
    if (status != TURBODB_STATUS_OK) return status;
    if (row.state == ORM_SQL_SCAN_CANCELLED)
      return cte_store_error(error,TURBODB_STATUS_INVALID_STATE,"CTE producer cancelled");
    if (row.state == ORM_SQL_SCAN_DONE) return TURBODB_STATUS_OK;
    if(store->recursion.distinct) {
      bool found;
      status=cte_index_contains(store,row.values,&found,error);
      if(status!=TURBODB_STATUS_OK) return status;
      if(found) continue;
    }
    turbodb_value_t *copy = NULL;
    status = orm_sql_rows_append(&store->rows,row.values,row.count,0,&copy,error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t cte_iteration_open(orm_sql_cte_store *store,turbodb_error_t *error) {
  orm_sql_row_source *producer=NULL;
  store->round_open=true;
  turbodb_status_t status=store->recursion.open(store->recursion.context,&store->frontier,&producer,error);
  if(status!=TURBODB_STATUS_OK) return status;
  const size_t columns=vec_size(&store->types);
  if(!producer || producer->budget!=store->budget ||
      !producer->types || producer->columns!=columns)
    return cte_store_error(error,TURBODB_STATUS_SQL_ERROR,"recursive member width or source differs from seed");
  status=cte_store_steps(store,columns,error);
  for(size_t i=0;status==TURBODB_STATUS_OK && i<columns;++i) {
    const orm_sql_type *type=vec_at_const(&store->types,i);
    if(producer->types[i].kind!=TURBODB_VALUE_NULL && type->kind!=producer->types[i].kind)
      status=cte_store_error(error,TURBODB_STATUS_TYPE_ERROR,"recursive member type differs from seed");
  }
  vec_t projection={0}; size_t bytes=0;
  if(status==TURBODB_STATUS_OK) status=orm_sql_work_zero(&projection,columns,sizeof(size_t),_Alignof(size_t),store->budget,&bytes,error);
  for(size_t i=0;status==TURBODB_STATUS_OK && i<columns;++i) *(size_t *)vec_at(&projection,i)=i;
  const orm_sql_scan_spec spec={.projection=vec_data_const(&projection),.projection_count=columns,
      .limit=UINT64_MAX,.distinct=store->recursion.distinct};
  if(status==TURBODB_STATUS_OK) status=orm_tidesdb_sql_scan_open_source(producer,&spec,store->budget,&store->iteration,error);
  const turbodb_status_t released=orm_sql_work_release(&projection,bytes,store->budget,status==TURBODB_STATUS_OK?error:NULL);
  return status==TURBODB_STATUS_OK?released:status;
}
static turbodb_status_t cte_store_materialize(orm_sql_cte_store *store,turbodb_error_t *error) {
  turbodb_status_t status=cte_store_batch(store,&store->input,error);
  size_t first=0;
  while(status==TURBODB_STATUS_OK && store->recursion.open && !cte_page_full(store) && first<vec_size(&store->rows.snapshots)) {
    if(store->iterations==store->recursion.max_iterations)
      return cte_store_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"recursive CTE iteration limit exceeded");
    status=cte_store_steps(store,1,error);
    if(status==TURBODB_STATUS_OK && store->recursion.distinct) status=cte_index_build(store,error);
    if(status!=TURBODB_STATUS_OK) return status;
    store->frontier_first=store->frontier_position=first; store->frontier_end=vec_size(&store->rows.snapshots);
    first=store->frontier_end; ++store->iterations;
    status=cte_iteration_open(store,error);
    if(status==TURBODB_STATUS_OK) status=cte_store_batch(store,&store->iteration,error);
    const turbodb_status_t released=cte_round_close(store,status==TURBODB_STATUS_OK?error:NULL);
    if(status==TURBODB_STATUS_OK) status=released;
  }
  if(status==TURBODB_STATUS_OK) store->state=ORM_SQL_CTE_READY;
  return status;
}
static turbodb_status_t cte_reader_pull(void *context, const turbodb_value_t **out, turbodb_error_t *error) {
  orm_sql_cte_reader *reader = context;
  if (!reader || !reader->store || !out)
    return cte_store_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid CTE reader pull");
  orm_sql_cte_store *store = reader->store;
  turbodb_status_t status = cte_store_ready(store,error);
  if (status != TURBODB_STATUS_OK) return status;
  store->evaluating = true;
  turbodb_error_t cause; tdsql_error_init(&cause);
  orm_sql_budget_amount charge = {0}; charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
  status = orm_tidesdb_sql_budget_reserve(store->budget,&charge,&cause);
  if (status == TURBODB_STATUS_OK && store->state == ORM_SQL_CTE_PENDING) status = cte_store_materialize(store,&cause);
  store->evaluating = false;
  if (status != TURBODB_STATUS_OK) {
    store->state = ORM_SQL_CTE_FAILED;
    tdsql_error_set(&store->failure,status,cause.message);
    return cte_store_error(error,status,store->failure.message);
  }
  if(store->recursion.paged && reader->position<store->recursion.offset) {
    const size_t count=vec_size(&store->rows.snapshots);
    reader->position=store->recursion.offset<count?(size_t)store->recursion.offset:count;
  }
  if (reader->position == vec_size(&store->rows.snapshots)) *out = NULL;
  else *out = orm_sql_rows_at(&store->rows,reader->position++);
  return TURBODB_STATUS_OK;
}
turbodb_status_t orm_sql_cte_reader_open(orm_sql_cte_store *store,
    orm_sql_cte_reader *out, turbodb_error_t *error) {
  if (!out || out->store) return cte_store_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"empty CTE reader required");
  turbodb_status_t status = cte_store_ready(store,error);
  if (status != TURBODB_STATUS_OK) return status;
  if (store->readers == SIZE_MAX) return cte_store_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"CTE reader count overflow");
  size_t bytes = 0;
  status = orm_tidesdb_sql_budget_reserve_capacity(store->budget,1,sizeof(*out),0,&bytes,error);
  if (status != TURBODB_STATUS_OK) return status;
  *out = (orm_sql_cte_reader){.store=store,.metadata_bytes=bytes};
  out->source = (orm_sql_row_source){store->budget,vec_data_const(&store->types),vec_size(&store->types),out,cte_reader_pull,false};
  ++store->readers; return TURBODB_STATUS_OK;
}
orm_sql_row_source *orm_sql_cte_reader_source(orm_sql_cte_reader *reader) {
  return reader && reader->store ? &reader->source : NULL;
}
turbodb_status_t orm_sql_cte_reader_close(orm_sql_cte_reader *reader, turbodb_error_t *error) {
  if (!reader || !reader->store) return TURBODB_STATUS_OK;
  if (reader->source.active || reader->store->evaluating)
    return cte_store_error(error,TURBODB_STATUS_BUSY,"CTE reader has an active consumer");
  const turbodb_status_t status = orm_tidesdb_sql_budget_release(reader->store->budget,ORM_SQL_BUDGET_WORK_BYTES,
      reader->metadata_bytes,error);
  --reader->store->readers; *reader = (orm_sql_cte_reader){0}; return status;
}
turbodb_status_t orm_sql_cte_reader_rewind(orm_sql_cte_reader *reader, turbodb_error_t *error) {
  if (!reader || !reader->store) return cte_store_error(error,TURBODB_STATUS_INVALID_STATE,"CTE reader is closed");
  if (reader->source.active) return cte_store_error(error,TURBODB_STATUS_BUSY,"CTE reader has an active consumer");
  turbodb_status_t status = cte_store_ready(reader->store,error);
  if (status == TURBODB_STATUS_OK) status = cte_store_steps(reader->store,1,error);
  if (status == TURBODB_STATUS_OK) reader->position = 0;
  return status;
}
turbodb_status_t orm_sql_cte_store_cancel(orm_sql_cte_store *store, turbodb_error_t *error) {
  if (!store || !store->budget) return cte_store_error(error,TURBODB_STATUS_INVALID_STATE,"CTE store is closed");
  if (store->evaluating) return cte_store_error(error,TURBODB_STATUS_BUSY,"CTE store is evaluating");
  if (store->state == ORM_SQL_CTE_FAILED || store->state == ORM_SQL_CTE_CANCELLED) return TURBODB_STATUS_OK;
  const turbodb_status_t status = orm_tidesdb_sql_scan_cancel(&store->input,error);
  if (status == TURBODB_STATUS_OK) store->state = ORM_SQL_CTE_CANCELLED;
  return status;
}
