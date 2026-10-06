#include "join.h"
#include "work.h"
#include "error.h"
#include <string.h>
#include <stdio.h>

static const char join_busy[] = "SQL join output still active";
static turbodb_status_t join_error(turbodb_error_t *error, turbodb_status_t status, const char *message) {
  tdsql_error_set(error,status,message); return status;
}
static void join_clear(orm_sql_join *run) {
  if (vec_size(&run->output)) memset(vec_data(&run->output),0,vec_size(&run->output)*sizeof(turbodb_value_t));
  if (vec_size(&run->inputs)) memset(vec_data(&run->inputs),0,vec_size(&run->inputs)*sizeof(turbodb_value_t));
}
static turbodb_status_t join_right_close(orm_sql_join *run, turbodb_error_t *error) {
  turbodb_status_t status=orm_tidesdb_sql_scan_close(&run->right,error);
  if(status!=TURBODB_STATUS_OK) return status;
  if(run->right_bound) {
    status=run->right_binding.close(run->right_binding.context,error);
    if(status!=TURBODB_STATUS_OK) return status;
    run->right_bound=false;
  }
  status=orm_sql_rows_close(&run->rows,error);
  if(status==TURBODB_STATUS_OK) run->rows=(orm_sql_rows){.budget=run->budget};
  return status;
}
turbodb_status_t orm_tidesdb_sql_join_close(orm_sql_join *run, turbodb_error_t *error) {
  if (!run || !run->budget) return TURBODB_STATUS_OK;
  if (run->source.active || run->evaluating) return join_error(error,TURBODB_STATUS_BUSY,join_busy);
  join_clear(run);
  if(run->right_binding.open) {
    turbodb_error_t cause; tdsql_error_init(&cause);
    run->evaluating=true;
    const turbodb_status_t status=join_right_close(run,&cause);
    run->evaluating=false;
    if(status!=TURBODB_STATUS_OK) {
      if(run->state!=ORM_SQL_SCAN_ERROR) tdsql_error_set(&run->failure,status,cause.message);
      run->state=ORM_SQL_SCAN_ERROR; run->pending=NULL;
      return join_error(error,status,cause.message);
    }
  }
  turbodb_status_t status = orm_tidesdb_sql_expr_run_close(&run->condition,error);
  turbodb_status_t released = orm_tidesdb_sql_scan_close(&run->left,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  released = orm_tidesdb_sql_scan_close(&run->right,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  released = orm_sql_rows_close(&run->rows,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = released;
  vec_t *vectors[] = {&run->types,&run->output,&run->slots,&run->inputs,&run->parameters.values,&run->right_types,&run->keys,&run->comparisons};
  const size_t bytes[] = {run->type_bytes,run->output_bytes,run->slot_bytes,run->input_bytes,run->parameters.bytes,run->right_type_bytes,run->key_bytes,run->comparison_bytes};
  for (size_t i = 0; i < sizeof(vectors)/sizeof(vectors[0]); ++i) {
    released = orm_sql_work_release(vectors[i],bytes[i],run->budget,status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  if (run->metadata_bytes) {
    released = orm_tidesdb_sql_budget_release(run->budget,ORM_SQL_BUDGET_WORK_BYTES,run->metadata_bytes,
        status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  *run = (orm_sql_join){0}; return status;
}
static turbodb_status_t join_validate(orm_sql_row_source *left, orm_sql_row_source *right,
    const orm_sql_join_spec *spec, turbodb_error_t *error) {
  if (!left || !right || left == right || !left->budget || right->budget != left->budget ||
      !left->types || !right->types || !left->columns || !right->columns || !spec ||
      (spec->count && !spec->slots) || (spec->parameter_count && (!spec->parameters || !spec->parameter_types)) ||
      (spec->queries.count && !spec->queries.items))
    return join_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL join specification");
  if((spec->right_binding.open==NULL)!=(spec->right_binding.close==NULL) ||
      (!spec->right_binding.open && spec->right_binding.context))
    return join_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"dependent join requires paired right callbacks");
  if (left->active || right->active) return join_error(error,TURBODB_STATUS_BUSY,"SQL join input already active");
  if (spec->kind < ORM_SQL_JOIN_INNER || spec->kind > ORM_SQL_JOIN_CROSS)
    return join_error(error,TURBODB_STATUS_UNSUPPORTED,"unsupported SQL join kind");
  if (spec->match != ORM_SQL_JOIN_MATCH_ON && spec->match != ORM_SQL_JOIN_MATCH_USING)
    return join_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL join matching mode");
  if ((spec->match == ORM_SQL_JOIN_MATCH_ON &&
      ((spec->kind == ORM_SQL_JOIN_CROSS) != (spec->condition == NULL) || spec->keys || spec->key_count)) ||
      (spec->match == ORM_SQL_JOIN_MATCH_USING && (spec->condition || spec->kind == ORM_SQL_JOIN_CROSS ||
          spec->count || spec->query_count || (spec->key_count && !spec->keys))) ||
      (!spec->condition && (spec->count || spec->query_count)))
    return join_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"SQL join requires ON except CROSS");
  if (spec->condition && (spec->condition->budget != left->budget ||
      (spec->condition->result.kind != TURBODB_VALUE_BOOLEAN && spec->condition->result.kind != TURBODB_VALUE_NULL)))
    return join_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"SQL join ON requires BOOL or NULL in the same budget");
  const uint64_t limit = left->budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES];
  if (left->columns > SIZE_MAX-right->columns || spec->parameter_count > SIZE_MAX-left->columns-right->columns ||
      left->columns+right->columns+spec->parameter_count > limit || spec->count > limit || spec->queries.count > limit || spec->key_count > limit)
    return join_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"SQL join shape exceeds plan capacity");
  orm_sql_budget_amount steps = {0};
  steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = left->columns+right->columns+spec->parameter_count;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(left->budget,&steps,error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < spec->key_count; ++i) {
    steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
    status = orm_tidesdb_sql_budget_reserve(left->budget,&steps,error);
    if (status != TURBODB_STATUS_OK) break;
    const orm_sql_join_key key = spec->keys[i];
    if (key.left >= left->columns || key.right >= right->columns)
      return join_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"USING key exceeds child width");
    orm_sql_predicate comparison;
    status = orm_tidesdb_sql_predicate_bind(ORM_SQL_EQUAL,left->types[key.left],&right->types[key.right],&comparison,error);
  }
  for(size_t i=0;status==TURBODB_STATUS_OK && spec->right_binding.open && i<right->columns;++i) {
    orm_sql_predicate validator;
    status=orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,right->types[i],NULL,&validator,error);
  }
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < spec->parameter_count; ++i) {
    orm_sql_predicate validator; turbodb_value_t ignored;
    status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,spec->parameter_types[i],NULL,&validator,error);
    if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_predicate_eval(&validator,&spec->parameters[i],NULL,
        left->budget,&ignored,error);
  }
  return status;
}
static turbodb_status_t join_input(orm_sql_row_source *source, orm_sql_scan *out, turbodb_error_t *error) {
  vec_t projection = {0}; size_t bytes = 0;
  turbodb_status_t status = orm_sql_work_zero(&projection,source->columns,sizeof(size_t),_Alignof(size_t),
      source->budget,&bytes,error);
  if (status == TURBODB_STATUS_OK) {
    for (size_t i = 0; i < source->columns; ++i) *(size_t *)vec_at(&projection,i) = i;
    const orm_sql_scan_spec spec = {.projection=vec_data_const(&projection),.projection_count=source->columns,.limit=UINT64_MAX};
    status = orm_tidesdb_sql_scan_open_source(source,&spec,source->budget,out,error);
  }
  const turbodb_status_t released = orm_sql_work_release(&projection,bytes,source->budget,status == TURBODB_STATUS_OK ? error : NULL);
  return status == TURBODB_STATUS_OK ? released : status;
}
static turbodb_status_t join_pull(void *context, const turbodb_value_t **out, turbodb_error_t *error);
turbodb_status_t orm_tidesdb_sql_join_open(orm_sql_row_source *left, orm_sql_row_source *right,
    const orm_sql_join_spec *spec, orm_sql_join *out, turbodb_error_t *error) {
  if (!out || out->budget) return join_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"SQL join requires empty output");
  turbodb_status_t status = join_validate(left,right,spec,error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_join run = {.budget=left->budget,.kind=spec->kind,.state=ORM_SQL_SCAN_OPEN,.rows={.budget=left->budget},
      .right_binding=spec->right_binding,.right_source=right};
  tdsql_error_init(&run.failure);
  const size_t width = left->columns+right->columns;
  status = orm_tidesdb_sql_budget_reserve_capacity(run.budget,1,sizeof(run),0,&run.metadata_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run.types,width,sizeof(orm_sql_type),
      _Alignof(orm_sql_type),run.budget,&run.type_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run.output,width,sizeof(turbodb_value_t),
      _Alignof(turbodb_value_t),run.budget,&run.output_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run.slots,spec->count,sizeof(size_t),
      _Alignof(size_t),run.budget,&run.slot_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run.inputs,spec->count,sizeof(turbodb_value_t),
      _Alignof(turbodb_value_t),run.budget,&run.input_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run.keys,spec->key_count,sizeof(orm_sql_join_key),
      _Alignof(orm_sql_join_key),run.budget,&run.key_bytes,error);
  if (status == TURBODB_STATUS_OK) status = orm_sql_work_zero(&run.comparisons,spec->key_count,sizeof(orm_sql_predicate),
      _Alignof(orm_sql_predicate),run.budget,&run.comparison_bytes,error);
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < spec->key_count; ++i) {
    orm_sql_budget_amount steps = {0}; steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1;
    status = orm_tidesdb_sql_budget_reserve(run.budget,&steps,error);
    if (status != TURBODB_STATUS_OK) break;
    const orm_sql_join_key key = spec->keys[i]; *(orm_sql_join_key *)vec_at(&run.keys,i) = key;
    status = orm_tidesdb_sql_predicate_bind(ORM_SQL_EQUAL,left->types[key.left],&right->types[key.right],vec_at(&run.comparisons,i),error);
  }
  if(status==TURBODB_STATUS_OK && run.right_binding.open) {
    status=orm_sql_work_zero(&run.right_types,right->columns,sizeof(orm_sql_type),
        _Alignof(orm_sql_type),run.budget,&run.right_type_bytes,error);
    if(status==TURBODB_STATUS_OK) memcpy(vec_data(&run.right_types),right->types,right->columns*sizeof(orm_sql_type));
  }
  if (status == TURBODB_STATUS_OK && spec->parameter_count) status = orm_sql_snapshot_copy(&run.parameters,
      spec->parameters,spec->parameter_count,0,run.budget,error);
  if (status == TURBODB_STATUS_OK) {
    memcpy(vec_data(&run.types),left->types,left->columns*sizeof(orm_sql_type));
    memcpy((orm_sql_type *)vec_data(&run.types)+left->columns,right->types,right->columns*sizeof(orm_sql_type));
    if (spec->count) memcpy(vec_data(&run.slots),spec->slots,spec->count*sizeof(size_t));
    if (spec->condition) {
      const orm_sql_expr_input_layout layout = {vec_data_const(&run.types),spec->parameter_types,width,spec->parameter_count};
      status = orm_tidesdb_sql_expr_check_inputs(spec->condition,&layout,spec->slots,spec->count,error);
      if (status == TURBODB_STATUS_OK) status = orm_tidesdb_sql_expr_run_open_mapped(spec->condition,spec->query_slots,spec->query_count,&spec->queries,&run.condition,error);
    }
    if (spec->kind == ORM_SQL_JOIN_LEFT)
      for (size_t i = left->columns; i < width; ++i) ((orm_sql_type *)vec_at(&run.types,i))->nullable = true;
  }
  if (status == TURBODB_STATUS_OK) status = join_input(left,&run.left,error);
  if (status == TURBODB_STATUS_OK && !run.right_binding.open) status = join_input(right,&run.right,error);
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t released = orm_tidesdb_sql_join_close(&run,NULL);
    return released == TURBODB_STATUS_OK ? status : released;
  }
  *out = run;
  out->source = (orm_sql_row_source){out->budget,vec_data_const(&out->types),width,out,join_pull,false};
  return TURBODB_STATUS_OK;
}
static turbodb_status_t join_read(orm_sql_scan *scan, const turbodb_value_t **out, turbodb_error_t *error) {
  orm_sql_scan_row row;
  const turbodb_status_t status = orm_tidesdb_sql_scan_next(scan,&row,error);
  if (status != TURBODB_STATUS_OK) return status;
  if (row.state != ORM_SQL_SCAN_ROW && row.state != ORM_SQL_SCAN_DONE)
    return join_error(error,TURBODB_STATUS_INVALID_STATE,"SQL join input cancelled");
  *out = row.state == ORM_SQL_SCAN_ROW ? row.values : NULL; return TURBODB_STATUS_OK;
}
static turbodb_status_t join_right_open(orm_sql_join *run, turbodb_error_t *error) {
  orm_sql_row_source *source=run->right_source;
  if(source->active) return join_error(error,TURBODB_STATUS_BUSY,"dependent join right source already active");
  turbodb_status_t status=run->right_binding.open(run->right_binding.context,run->pending,run->left.source.columns,error);
  if(status!=TURBODB_STATUS_OK) return status;
  run->right_bound=true;
  if(source->budget!=run->budget || source->columns!=vec_size(&run->right_types) || !source->types || !source->next)
    return join_error(error,TURBODB_STATUS_INVALID_STATE,"dependent join right source changed shape");
  if(source->active) return join_error(error,TURBODB_STATUS_BUSY,"dependent join callback retained a source lease");
  orm_sql_budget_amount steps={0}; steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=source->columns;
  status=orm_tidesdb_sql_budget_reserve(run->budget,&steps,error);
  for(size_t i=0;status==TURBODB_STATUS_OK && i<source->columns;++i) {
    const orm_sql_type *bound=vec_at_const(&run->right_types,i);
    if(source->types[i].kind!=bound->kind || source->types[i].nullable!=bound->nullable)
      return join_error(error,TURBODB_STATUS_TYPE_ERROR,"dependent join right source changed types");
  }
  return status==TURBODB_STATUS_OK?join_input(source,&run->right,error):status;
}
static turbodb_status_t join_materialize(orm_sql_join *run, turbodb_error_t *error) {
  turbodb_status_t status=TURBODB_STATUS_OK;
  if(run->right_binding.open) status=join_right_open(run,error);
  while(status==TURBODB_STATUS_OK) {
    const turbodb_value_t *row; turbodb_value_t *copy;
    status=join_read(&run->right,&row,error);
    if(status!=TURBODB_STATUS_OK || !row) break;
    status=orm_sql_rows_append(&run->rows,row,run->right.source.columns,0,&copy,error);
  }
  return status;
}
static turbodb_status_t join_prepare(orm_sql_join *run, turbodb_error_t *error) {
  if (run->ready) return TURBODB_STATUS_OK;
  turbodb_status_t status = join_read(&run->left,&run->pending,error);
  if (status != TURBODB_STATUS_OK) return status;
  if(run->pending) status=join_materialize(run,error);
  if (status == TURBODB_STATUS_OK) run->ready = true;
  return status;
}
static turbodb_status_t join_candidate(orm_sql_join *run, const turbodb_value_t *right, bool *matches, turbodb_error_t *error) {
  const size_t left_columns = run->left.source.columns, width = vec_size(&run->output);
  orm_sql_budget_amount amount = {0}; amount.value[ORM_SQL_BUDGET_JOIN_PAIRS] = right ? 1 : 0;
  amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = width;
  turbodb_status_t status = orm_tidesdb_sql_budget_reserve(run->budget,&amount,error);
  if (status != TURBODB_STATUS_OK) return status;
  turbodb_value_t *row = vec_data(&run->output);
  memcpy(row,run->pending,left_columns*sizeof(*row));
  if (right) memcpy(row+left_columns,right,(width-left_columns)*sizeof(*row));
  else memset(row+left_columns,0,(width-left_columns)*sizeof(*row));
  *matches = true;
  if (!right) return TURBODB_STATUS_OK;
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < vec_size(&run->keys); ++i) {
    const orm_sql_join_key *key = vec_at_const(&run->keys,i); turbodb_value_t value;
    status = orm_tidesdb_sql_predicate_eval(vec_at_const(&run->comparisons,i),&row[key->left],&row[left_columns+key->right],run->budget,&value,error);
    if (status == TURBODB_STATUS_OK && (value.kind != TURBODB_VALUE_BOOLEAN || !value.data.boolean_value)) { *matches = false; return TURBODB_STATUS_OK; }
  }
  if (status != TURBODB_STATUS_OK || !run->condition.program) return status;
  for (size_t i = 0; i < vec_size(&run->slots); ++i) {
    const size_t slot = *(const size_t *)vec_at_const(&run->slots,i);
    *(turbodb_value_t *)vec_at(&run->inputs,i) = slot < width ? row[slot] :
        *(const turbodb_value_t *)vec_at_const(&run->parameters.values,slot-width);
  }
  turbodb_value_t value;
  status = orm_tidesdb_sql_expr_run_eval_row(&run->condition,vec_data_const(&run->inputs),
      vec_size(&run->inputs),row,width,&value,error);
  if (vec_size(&run->inputs)) memset(vec_data(&run->inputs),0,vec_size(&run->inputs)*sizeof(turbodb_value_t));
  if (status == TURBODB_STATUS_OK) *matches = value.kind == TURBODB_VALUE_BOOLEAN && value.data.boolean_value;
  return status;
}
static turbodb_status_t join_advance(orm_sql_join *run, orm_sql_scan_row *out, turbodb_error_t *error) {
  turbodb_status_t status = join_prepare(run,error);
  while (status == TURBODB_STATUS_OK && run->pending) {
    if (run->position < vec_size(&run->rows.snapshots)) {
      bool matches;
      status = join_candidate(run,orm_sql_rows_at(&run->rows,run->position),&matches,error);
      if (status != TURBODB_STATUS_OK) break;
      ++run->position;
      if (!matches) continue;
      run->matched = true;
    } else if (run->kind == ORM_SQL_JOIN_LEFT && !run->matched) {
      bool matches;
      status = join_candidate(run,NULL,&matches,error);
      if (status != TURBODB_STATUS_OK) break;
      run->matched = true;
    } else {
      if (!run->right_binding.open && !vec_size(&run->rows.snapshots) && run->kind != ORM_SQL_JOIN_LEFT) {
        run->pending = NULL; break;
      }
      if(run->right_binding.open) {
        join_clear(run);
        status=join_right_close(run,error);
        if(status!=TURBODB_STATUS_OK) break;
      }
      status = join_read(&run->left,&run->pending,error);
      run->position = 0; run->matched = false;
      if(status==TURBODB_STATUS_OK && run->pending && run->right_binding.open) status=join_materialize(run,error);
      continue;
    }
    run->state = ORM_SQL_SCAN_ROW;
    *out = (orm_sql_scan_row){run->state,vec_data_const(&run->output),vec_size(&run->output)};
    return TURBODB_STATUS_OK;
  }
  if (status == TURBODB_STATUS_OK) {
    run->state = ORM_SQL_SCAN_DONE; join_clear(run);
    *out = (orm_sql_scan_row){run->state,NULL,0};
  }
  return status;
}
static turbodb_status_t join_next(orm_sql_join *run, orm_sql_scan_row *out, turbodb_error_t *error) {
  if (run->evaluating) return join_error(error,TURBODB_STATUS_BUSY,join_busy);
  join_clear(run);
  if (run->state == ORM_SQL_SCAN_ERROR) return join_error(error,run->failure.status,run->failure.message);
  if (run->state == ORM_SQL_SCAN_DONE || run->state == ORM_SQL_SCAN_CANCELLED) {
    *out = (orm_sql_scan_row){run->state,NULL,0}; return TURBODB_STATUS_OK;
  }
  turbodb_error_t cause; tdsql_error_init(&cause);
  run->evaluating = true;
  const turbodb_status_t status = join_advance(run,out,&cause);
  run->evaluating = false;
  if (status != TURBODB_STATUS_OK) {
    char message[TURBODB_ERROR_MESSAGE_CAPACITY];
    (void)snprintf(message,sizeof(message),"TidesDB SQL join at candidate %zu: %s",run->position,cause.message);
    tdsql_error_set(&run->failure,status,message); tdsql_error_set(error,status,message);
    run->state = ORM_SQL_SCAN_ERROR; run->pending = NULL; join_clear(run);
  }
  return status;
}
turbodb_status_t orm_tidesdb_sql_join_next(orm_sql_join *run, orm_sql_scan_row *out, turbodb_error_t *error) {
  if (!run || !run->budget || !out) return join_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL join next");
  if (run->source.active || run->evaluating) return join_error(error,TURBODB_STATUS_BUSY,join_busy);
  return join_next(run,out,error);
}
static turbodb_status_t join_pull(void *context, const turbodb_value_t **out, turbodb_error_t *error) {
  orm_sql_scan_row row;
  const turbodb_status_t status = join_next(context,&row,error);
  if (status == TURBODB_STATUS_OK) *out = row.state == ORM_SQL_SCAN_ROW ? row.values : NULL;
  return status;
}
orm_sql_row_source *orm_tidesdb_sql_join_source(orm_sql_join *run) {
  return run && run->budget ? &run->source : NULL;
}
turbodb_status_t orm_tidesdb_sql_join_cancel(orm_sql_join *run, turbodb_error_t *error) {
  if (!run || !run->budget) return join_error(error,TURBODB_STATUS_INVALID_ARGUMENT,"invalid SQL join cancel");
  if (run->source.active || run->evaluating) return join_error(error,TURBODB_STATUS_BUSY,join_busy);
  if (run->state == ORM_SQL_SCAN_OPEN || run->state == ORM_SQL_SCAN_ROW) run->state = ORM_SQL_SCAN_CANCELLED;
  run->pending = NULL; join_clear(run); return TURBODB_STATUS_OK;
}
