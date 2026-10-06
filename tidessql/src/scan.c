#include "scan.h"
#include "work.h"
#include "error.h"
#include <stdio.h>
#include <string.h>

typedef struct scan_projection {
  size_t slot;
  orm_sql_predicate validator;
  orm_sql_expr_run expression;
  vec_t slots, inputs;
  size_t slot_bytes, input_bytes;
} scan_projection;

typedef struct scan_order { scan_projection value; bool descending; } scan_order;
typedef struct scan_sorted_row {
  const turbodb_value_t *row;
  const scan_order *orders;
  size_t columns, count, outputs;
} scan_sorted_row;

static int scan_compare_rows(const void *left, const void *right) {
  const scan_sorted_row *a = left, *b = right;
  for (size_t i = 0; i < a->count; ++i) {
    const int compared = orm_sql_value_order(&a->row[a->columns+i], &b->row[b->columns+i]);
    if (compared) return a->orders[i].descending ? -compared : compared;
  }
  return 0;
}
static int scan_compare_outputs(const void *left, const void *right) {
  const scan_sorted_row *a = left, *b = right;
  for (size_t i = 0; i < a->outputs; ++i) {
    const int compared = orm_sql_value_order(&a->row[a->columns+a->count+i],
        &b->row[b->columns+b->count+i]);
    if (compared) return compared;
  }
  return 0;
}
static bool scan_sorted_copy(void *to, const void *from) { *(scan_sorted_row *)to = *(const scan_sorted_row *)from; return true; }
static void scan_sorted_move(void *to, void *from) { *(scan_sorted_row *)to = *(const scan_sorted_row *)from; }
/* Only the snapshot registry owns rows; sort records and scratch borrow them. */
static void scan_sorted_destroy(void *value) { (void)value; }
static const cmeta_type_traits scan_sorted_traits = {
  CMETA_TRAIT_COMPARE | CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY |
    CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY,
  NULL, NULL, scan_compare_rows, scan_sorted_copy, scan_sorted_move, scan_sorted_destroy};
static const cmeta_type_desc scan_sorted_type = {"sql_scan_sorted_row", sizeof(scan_sorted_row),
  _Alignof(scan_sorted_row), CMETA_T_OBJECT, NULL, &scan_sorted_traits};
static const cmeta_type_traits scan_distinct_traits = {
  CMETA_TRAIT_COMPARE | CMETA_TRAIT_COPY | CMETA_TRAIT_MOVE | CMETA_TRAIT_DESTROY |
    CMETA_TRAIT_TRIVIAL_COPY | CMETA_TRAIT_TRIVIAL_DESTROY,
  NULL, NULL, scan_compare_outputs, scan_sorted_copy, scan_sorted_move, scan_sorted_destroy};
static const cmeta_type_desc scan_distinct_type = {"sql_scan_distinct_row", sizeof(scan_sorted_row),
  _Alignof(scan_sorted_row), CMETA_T_OBJECT, NULL, &scan_distinct_traits};

static turbodb_status_t scan_error(turbodb_error_t *error, turbodb_status_t status, const char *message) {
  tdsql_error_set(error, status, message);
  return status;
}

static void scan_clear(vec_t *values) {
  if (vec_size(values)) memset(vec_data(values), 0, vec_size(values) * sizeof(turbodb_value_t));
}

static turbodb_status_t scan_vector(vec_t *values, size_t count, size_t size, size_t align,
    orm_tidesdb_sql_budget *budget, size_t *bytes, turbodb_error_t *error) {
  turbodb_status_t status = orm_sql_work_allocate(values, count, size, align, 0, budget, bytes, error);
  if (status != TURBODB_STATUS_OK) return status;
  const stl_status resized = vec_resize(values, count);
  if (resized == STL_OK) {
    memset(vec_data(values), 0, count * size);
    return TURBODB_STATUS_OK;
  }
  return scan_error(error, resized == STL_OUT_OF_MEMORY ? TURBODB_STATUS_OUT_OF_MEMORY :
      resized == STL_CAPACITY_EXCEEDED ? TURBODB_STATUS_LIMIT_EXCEEDED : TURBODB_STATUS_INTERNAL_ERROR,
      "TidesDB SQL scan workspace allocation failed");
}

turbodb_status_t orm_tidesdb_sql_scan_close(orm_sql_scan *run, turbodb_error_t *error) {
  if (!run || !run->budget) return TURBODB_STATUS_OK;
  if (run->evaluating) return scan_error(error, TURBODB_STATUS_BUSY, "SQL scan is evaluating");
  scan_clear(&run->inputs); scan_clear(&run->output); scan_clear(&run->parameters);
  turbodb_status_t status = orm_tidesdb_sql_expr_run_close(&run->expression, error);
  for (size_t i = 0; i < vec_size(&run->projection) + vec_size(&run->orders); ++i) {
    scan_projection *p = i < vec_size(&run->projection) ? vec_at(&run->projection, i) :
      &((scan_order *)vec_at(&run->orders, i - vec_size(&run->projection)))->value;
    scan_clear(&p->inputs);
    turbodb_status_t released = orm_tidesdb_sql_expr_run_close(&p->expression, status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
    released = orm_sql_work_release(&p->slots, p->slot_bytes, run->budget, status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
    released = orm_sql_work_release(&p->inputs, p->input_bytes, run->budget, status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  const turbodb_status_t snapshots_released = orm_sql_rows_close(&run->snapshots,status == TURBODB_STATUS_OK ? error : NULL);
  if (status == TURBODB_STATUS_OK) status = snapshots_released;
  vec_t *vectors[] = {&run->slots, &run->inputs, &run->projection, &run->output,
                     &run->parameters, &run->parameter_payload, &run->orders, &run->sorted};
  const size_t bytes[] = {run->slot_bytes, run->input_bytes, run->projection_bytes, run->output_bytes,
                         run->parameter_bytes, run->payload_bytes, run->order_bytes, run->sorted_bytes};
  for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
    const turbodb_status_t released = orm_sql_work_release(vectors[i], bytes[i], run->budget,
                                                       status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  if (run->metadata_bytes) {
    const turbodb_status_t released = orm_tidesdb_sql_budget_release(run->budget,
        ORM_SQL_BUDGET_WORK_BYTES, run->metadata_bytes, status == TURBODB_STATUS_OK ? error : NULL);
    if (status == TURBODB_STATUS_OK) status = released;
  }
  if (run->pull) run->pull->active = false;
  *run = (orm_sql_scan){0};
  return status;
}

static turbodb_status_t scan_open_expression(orm_sql_expr *program, const size_t *slots, size_t count,
    const orm_sql_scan_spec *spec, orm_sql_expr_run *out, turbodb_error_t *error) {
  const orm_sql_expr_query_sources sources = {spec->queries,spec->query_count,spec->evaluation};
  return orm_tidesdb_sql_expr_run_open_mapped(program,slots,count,&sources,out,error);
}

static turbodb_status_t scan_validate(const orm_sql_memory_source *source,
    const orm_sql_scan_spec *spec, orm_tidesdb_sql_budget *budget, turbodb_error_t *error) {
  if (!source || !spec || !budget || !source->columns || !source->types ||
      (source->rows && !source->values) || !spec->projection_count || !spec->projection ||
      (spec->filter_count && !spec->filter_slots) || (!spec->filter && spec->filter_count) ||
      (spec->parameter_count && (!spec->parameters || !spec->parameter_types)) ||
      (spec->order_count && !spec->orders) || (spec->query_count && !spec->queries) ||
      (!spec->filter && spec->filter_query_count))
    return scan_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid TidesDB SQL scan specification");
  if (source->columns > SIZE_MAX / sizeof(turbodb_value_t) ||
      source->rows > SIZE_MAX / (source->columns * sizeof(turbodb_value_t)) ||
      source->columns > budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES] ||
      spec->projection_count > budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES] ||
      spec->order_count > budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES] ||
      spec->query_count > budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES] ||
      spec->order_count > SIZE_MAX - spec->projection_count ||
      spec->parameter_count > SIZE_MAX - source->columns ||
      spec->parameter_count > SIZE_MAX / sizeof(turbodb_value_t) ||
      source->columns + spec->parameter_count > budget->limits.statement.value[ORM_SQL_BUDGET_PLAN_NODES])
    return scan_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "TidesDB SQL scan shape exceeds bounds");
  if(spec->coerce_types) {
    orm_sql_budget_amount amount={0};amount.value[ORM_SQL_BUDGET_EXECUTION_STEPS]=source->columns;
    const turbodb_status_t status=orm_tidesdb_sql_budget_reserve(budget,&amount,error);
    if(status!=TURBODB_STATUS_OK) return status;
  }
  for (size_t i = 0; i < source->columns; ++i) {
    orm_sql_predicate validator;
    turbodb_status_t status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,
        source->types[i], NULL, &validator, error);
    if (status == TURBODB_STATUS_OK && spec->coerce_types) {
      const orm_sql_type from=source->types[i],to=spec->coerce_types[i];
      status=orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL,to,NULL,&validator,error);
      if(status==TURBODB_STATUS_OK && from.nullable && !to.nullable)
        status=scan_error(error,TURBODB_STATUS_TYPE_ERROR,"SQL result conversion narrows nullability");
      if(status==TURBODB_STATUS_OK && from.kind!=to.kind && from.kind!=TURBODB_VALUE_NULL &&
          !(to.kind==TURBODB_VALUE_DOUBLE && (from.kind==TURBODB_VALUE_INT64 || from.kind==TURBODB_VALUE_UINT64)))
        status=scan_error(error,TURBODB_STATUS_UNSUPPORTED,"SQL result conversion requires compatible numeric types");
    }
    if (status != TURBODB_STATUS_OK) return status;
  }
  const orm_sql_expr_input_layout layout = {source->types, spec->parameter_types,
                                           source->columns, spec->parameter_count};
  for (size_t i = 0; i < spec->projection_count + spec->order_count; ++i) {
    const bool order = i >= spec->projection_count;
    const orm_sql_scan_order *key = order ? &spec->orders[i-spec->projection_count] : NULL;
    const size_t slot = order ? key->slot : spec->projection[i];
    const orm_sql_scan_expression *expression = order ? &key->expression : spec->expressions ? &spec->expressions[i] : NULL;
    if (expression && expression->program) {
      if (expression->program->budget != budget)
        return scan_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "TidesDB SQL projection uses a different budget");
      turbodb_status_t status = orm_tidesdb_sql_expr_check_inputs(expression->program, &layout,
          expression->slots, expression->count, error);
      if (status != TURBODB_STATUS_OK) return status;
    } else if ((expression && (expression->count || expression->query_count)) || slot >= source->columns) {
      return scan_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "TidesDB SQL projection outside schema");
    }
    const orm_sql_type type = expression && expression->program ? expression->program->result :
        spec->coerce_types ? spec->coerce_types[slot] : source->types[slot];
    if (order && (type.kind == TURBODB_VALUE_TEXT || type.kind == TURBODB_VALUE_BLOB))
      return scan_error(error, TURBODB_STATUS_UNSUPPORTED, "SQL ORDER BY requires numeric, BOOL or NULL keys");
    if (!order && spec->distinct && (type.kind == TURBODB_VALUE_TEXT || type.kind == TURBODB_VALUE_BLOB))
      return scan_error(error, TURBODB_STATUS_UNSUPPORTED, "SQL DISTINCT requires numeric, BOOL or NULL outputs");
  }
  if (spec->filter) {
    if (spec->filter->budget != budget)
      return scan_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "TidesDB SQL filter uses a different budget");
    if (spec->filter->result.kind != TURBODB_VALUE_BOOLEAN && spec->filter->result.kind != TURBODB_VALUE_NULL)
      return scan_error(error, TURBODB_STATUS_UNSUPPORTED, "TidesDB SQL filter must return BOOL or NULL");
    return orm_tidesdb_sql_expr_check_inputs(spec->filter, &layout, spec->filter_slots, spec->filter_count, error);
  }
  return TURBODB_STATUS_OK;
}

static turbodb_status_t scan_copy_parameters(orm_sql_scan *run, const orm_sql_scan_spec *spec, turbodb_error_t *error) {
  if (!spec->parameter_count) return TURBODB_STATUS_OK;
  size_t payload = 0;
  for (size_t i = 0; i < spec->parameter_count; ++i) {
    const turbodb_value_t *value = &spec->parameters[i];
    const size_t size = value->kind == TURBODB_VALUE_TEXT ? value->data.text_value.len :
        value->kind == TURBODB_VALUE_BLOB ? value->data.blob_value.size : 0;
    if (size > SIZE_MAX - payload)
      return scan_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL parameter payload size overflow");
    payload += size;
  }
  turbodb_status_t status = scan_vector(&run->parameters, spec->parameter_count, sizeof(turbodb_value_t),
      _Alignof(turbodb_value_t), run->budget, &run->parameter_bytes, error);
  if (status == TURBODB_STATUS_OK && payload)
    status = scan_vector(&run->parameter_payload, payload, sizeof(unsigned char), _Alignof(unsigned char),
        run->budget, &run->payload_bytes, error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_budget_amount copy_steps = {0}; copy_steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = payload;
  status = orm_tidesdb_sql_budget_reserve(run->budget, &copy_steps, error);
  if (status != TURBODB_STATUS_OK) return status;
  size_t offset = 0;
  for (size_t i = 0; i < spec->parameter_count; ++i) {
    orm_sql_predicate validator;
    turbodb_value_t ignored;
    turbodb_error_t cause; tdsql_error_init(&cause);
    status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL, spec->parameter_types[i], NULL, &validator, &cause);
    if (status == TURBODB_STATUS_OK)
      status = orm_tidesdb_sql_predicate_eval(&validator, &spec->parameters[i], NULL, run->budget, &ignored, &cause);
    if (status != TURBODB_STATUS_OK) {
      char message[TURBODB_ERROR_MESSAGE_CAPACITY];
      (void)snprintf(message, sizeof(message), "SQL parameter %zu: %s", i + 1, cause.message);
      return scan_error(error, status, message);
    }
    turbodb_value_t value = spec->parameters[i];
    const void *data = value.kind == TURBODB_VALUE_TEXT ? value.data.text_value.data :
        value.kind == TURBODB_VALUE_BLOB ? value.data.blob_value.data : NULL;
    const size_t size = value.kind == TURBODB_VALUE_TEXT ? value.data.text_value.len :
        value.kind == TURBODB_VALUE_BLOB ? value.data.blob_value.size : 0;
    unsigned char *owned = size ? (unsigned char *)vec_data(&run->parameter_payload) + offset : NULL;
    if (size) memcpy(owned, data, size);
    if (value.kind == TURBODB_VALUE_TEXT) value.data.text_value.data = (const char *)owned;
    else if (value.kind == TURBODB_VALUE_BLOB) value.data.blob_value.data = owned;
    *(turbodb_value_t *)vec_at(&run->parameters, i) = value;
    offset += size;
  }
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_scan_open(const orm_sql_memory_source *source,
    const orm_sql_scan_spec *spec, orm_tidesdb_sql_budget *budget,
    orm_sql_scan *out, turbodb_error_t *error) {
  if (!out || out->budget)
    return scan_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "TidesDB SQL scan requires empty output");
  turbodb_status_t status = scan_validate(source, spec, budget, error);
  if (status != TURBODB_STATUS_OK) return status;
  orm_sql_scan run = {.budget = budget, .snapshots = {.budget=budget}, .source = *source,
      .offset_left = spec->offset, .limit_left = spec->limit, .state = ORM_SQL_SCAN_OPEN,
      .distinct = spec->distinct,.coerce_types=spec->coerce_types};
  tdsql_error_init(&run.failure);
  run.source.types = NULL; /* Binding descriptors below own the needed types. */
  status = orm_tidesdb_sql_budget_reserve_capacity(budget, 1, sizeof(run), 0, &run.metadata_bytes, error);
  if (status == TURBODB_STATUS_OK && spec->filter_count)
    status = scan_vector(&run.slots, spec->filter_count, sizeof(size_t), _Alignof(size_t), budget, &run.slot_bytes, error);
  if (status == TURBODB_STATUS_OK && spec->filter_count)
    status = scan_vector(&run.inputs, spec->filter_count, sizeof(turbodb_value_t), _Alignof(turbodb_value_t), budget, &run.input_bytes, error);
  if (status == TURBODB_STATUS_OK)
    status = scan_vector(&run.projection, spec->projection_count, sizeof(scan_projection), _Alignof(scan_projection), budget, &run.projection_bytes, error);
  if (status == TURBODB_STATUS_OK)
    status = scan_vector(&run.output, spec->projection_count, sizeof(turbodb_value_t), _Alignof(turbodb_value_t), budget, &run.output_bytes, error);
  if (status == TURBODB_STATUS_OK && spec->order_count)
    status = scan_vector(&run.orders, spec->order_count, sizeof(scan_order), _Alignof(scan_order), budget, &run.order_bytes, error);
  if (status == TURBODB_STATUS_OK) status = scan_copy_parameters(&run, spec, error);
  if (status == TURBODB_STATUS_OK && spec->filter)
    status = scan_open_expression(spec->filter, spec->filter_query_slots, spec->filter_query_count, spec, &run.expression, error);
  if (status == TURBODB_STATUS_OK && spec->filter_count)
    memcpy(vec_data(&run.slots), spec->filter_slots, spec->filter_count * sizeof(size_t));
  for (size_t i = 0; status == TURBODB_STATUS_OK && i < spec->projection_count + spec->order_count; ++i) {
    const bool order = i >= spec->projection_count;
    scan_order *key = order ? vec_at(&run.orders, i-spec->projection_count) : NULL;
    const orm_sql_scan_order *input = order ? &spec->orders[i-spec->projection_count] : NULL;
    scan_projection *projection = order ? &key->value : vec_at(&run.projection, i);
    const orm_sql_scan_expression *expression = order ? &input->expression : spec->expressions ? &spec->expressions[i] : NULL;
    if (order) key->descending = input->descending;
    if (expression && expression->program) {
      if (expression->count) {
        status = scan_vector(&projection->slots, expression->count, sizeof(size_t), _Alignof(size_t),
            budget, &projection->slot_bytes, error);
        if (status == TURBODB_STATUS_OK)
          status = scan_vector(&projection->inputs, expression->count, sizeof(turbodb_value_t), _Alignof(turbodb_value_t),
              budget, &projection->input_bytes, error);
        if (status == TURBODB_STATUS_OK)
          memcpy(vec_data(&projection->slots), expression->slots, expression->count * sizeof(size_t));
      }
      if (status == TURBODB_STATUS_OK)
        status = scan_open_expression(expression->program, expression->query_slots, expression->query_count,
            spec, &projection->expression, error);
    } else {
      projection->slot = order ? input->slot : spec->projection[i];
      status = orm_tidesdb_sql_predicate_bind(ORM_SQL_IS_NULL, source->types[projection->slot], NULL,
                                             &projection->validator, error);
    }
  }
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t cleanup = orm_tidesdb_sql_scan_close(&run, NULL);
    return cleanup == TURBODB_STATUS_OK ? status : cleanup;
  }
  *out = run;
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_scan_open_source(orm_sql_row_source *source,
    const orm_sql_scan_spec *spec, orm_tidesdb_sql_budget *budget,
    orm_sql_scan *out, turbodb_error_t *error) {
  if (!source || !source->next || !source->context || source->budget != budget)
    return scan_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid SQL row source");
  if (source->active) return scan_error(error, TURBODB_STATUS_BUSY, "SQL row source already has an active scan");
  const orm_sql_memory_source shape = {NULL, 0, source->columns, source->types};
  const turbodb_status_t status = orm_tidesdb_sql_scan_open(&shape, spec, budget, out, error);
  if (status == TURBODB_STATUS_OK) { out->pull = source; source->active = true; }
  return status;
}

static turbodb_status_t scan_admit(orm_sql_scan *run, const turbodb_value_t *row, turbodb_error_t *error) {
  uint64_t bytes = run->source.columns * sizeof(turbodb_value_t);
  for (size_t i = 0; i < run->source.columns; ++i) {
    const turbodb_value_t *value = &row[i];
    size_t payload = 0;
    if (value->kind == TURBODB_VALUE_TEXT) payload = value->data.text_value.len;
    else if (value->kind == TURBODB_VALUE_BLOB) payload = value->data.blob_value.size;
    else if (value->kind < TURBODB_VALUE_NULL || value->kind > TURBODB_VALUE_BLOB)
      return scan_error(error, TURBODB_STATUS_TYPE_ERROR, "TidesDB SQL source has an invalid value kind");
    if (payload > UINT64_MAX - bytes)
      return scan_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "TidesDB SQL source row bytes overflow");
    bytes += payload;
  }
  orm_sql_budget_amount charge = {0};
  charge.value[ORM_SQL_BUDGET_READ_ROWS] = 1;
  charge.value[ORM_SQL_BUDGET_READ_BYTES] = bytes;
  charge.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = 1 + run->source.columns;
  return orm_tidesdb_sql_budget_reserve(run->budget, &charge, error);
}

static turbodb_status_t scan_fail(orm_sql_scan *run, turbodb_status_t status,
    const turbodb_error_t *cause, turbodb_error_t *error) {
  char message[TURBODB_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "TidesDB SQL scan at row %zu: %s", run->position, cause->message);
  tdsql_error_set(&run->failure, status, message);
  run->state = ORM_SQL_SCAN_ERROR;
  scan_clear(&run->inputs); scan_clear(&run->output);
  tdsql_error_set(error, status, run->failure.message);
  return status;
}

static turbodb_status_t scan_eval(orm_sql_scan *run, const turbodb_value_t *row, orm_sql_expr_run *expression,
    const vec_t *slots, vec_t *inputs, turbodb_value_t *out, turbodb_error_t *error) {
  for (size_t i = 0; i < vec_size(slots); ++i) {
    const size_t slot = *(const size_t *)vec_at_const(slots, i);
    *(turbodb_value_t *)vec_at(inputs, i) = slot < run->source.columns ? row[slot] :
        *(const turbodb_value_t *)vec_at_const(&run->parameters, slot - run->source.columns);
  }
  const turbodb_status_t status = orm_tidesdb_sql_expr_run_eval_row(expression,vec_data_const(inputs),
      vec_size(inputs),row,run->source.columns,out,error);
  scan_clear(inputs);
  return status;
}


static turbodb_status_t scan_project_value(orm_sql_scan *run, const turbodb_value_t *row,
    scan_projection *projection, turbodb_value_t *out, turbodb_error_t *error) {
  if (projection->expression.program)
    return scan_eval(run, row, &projection->expression, &projection->slots, &projection->inputs, out, error);
  turbodb_status_t status = orm_tidesdb_sql_predicate_eval(&projection->validator, &row[projection->slot],
      NULL, run->budget, out, error);
  if (status == TURBODB_STATUS_OK) {
    const turbodb_value_t *value=&row[projection->slot];
    if(run->coerce_types && run->coerce_types[projection->slot].kind==TURBODB_VALUE_DOUBLE &&
        (value->kind==TURBODB_VALUE_INT64 || value->kind==TURBODB_VALUE_UINT64))
      status=orm_tidesdb_sql_real_promote(value,run->budget,out,error);
    else *out=*value;
  }
  return status;
}
static turbodb_status_t scan_project_row(orm_sql_scan *run, const turbodb_value_t *row, turbodb_error_t *error) {
  for (size_t i = 0; i < vec_size(&run->projection); ++i) {
    const turbodb_status_t status = scan_project_value(run, row, vec_at(&run->projection, i),
        vec_at(&run->output, i), error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t scan_capture(orm_sql_scan *run, const turbodb_value_t *row, turbodb_error_t *error) {
  const size_t columns = run->source.columns, keys = vec_size(&run->orders);
  const size_t outputs = run->distinct ? vec_size(&run->projection) : 0;
  if (outputs > SIZE_MAX-keys) return scan_error(error,TURBODB_STATUS_LIMIT_EXCEEDED,"SQL sort snapshot overflow");
  turbodb_value_t *copy = NULL;
  turbodb_status_t status = orm_sql_rows_append(&run->snapshots,row,columns,keys+outputs,&copy,error);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; i < keys; ++i) {
    scan_order *order = vec_at(&run->orders, i);
    status = scan_project_value(run, copy, &order->value, &copy[columns+i], error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  for (size_t i = 0; i < outputs; ++i) {
    status = scan_project_value(run, copy, vec_at(&run->projection, i), &copy[columns+keys+i], error);
    if (status != TURBODB_STATUS_OK) return status;
  }
  return TURBODB_STATUS_OK;
}
static turbodb_status_t scan_distinct(orm_sql_scan *run, turbodb_error_t *error) {
  const size_t count = vec_size(&run->sorted), outputs = vec_size(&run->projection);
  turbodb_status_t status = orm_sql_work_sort(vec_data(&run->sorted), count, &scan_distinct_type,
      outputs, run->budget, error);
  if (status != TURBODB_STATUS_OK) return status;
  if (outputs == SIZE_MAX || count > UINT64_MAX / (outputs + 1))
    return scan_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL DISTINCT comparison steps overflow");
  orm_sql_budget_amount steps = {0}; steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = (uint64_t)count * (outputs + 1);
  status = orm_tidesdb_sql_budget_reserve(run->budget, &steps, error);
  if (status != TURBODB_STATUS_OK) return status;
  size_t unique = 0;
  for (size_t i = 0; i < count; ++i) {
    const scan_sorted_row *candidate = vec_at_const(&run->sorted, i);
    if (!unique || scan_compare_outputs(vec_at_const(&run->sorted, unique-1), candidate))
      *(scan_sorted_row *)vec_at(&run->sorted, unique++) = *candidate;
  }
  run->sorted_count = unique;
  return TURBODB_STATUS_OK;
}
static turbodb_status_t scan_prepare_sort(orm_sql_scan *run, turbodb_error_t *error) {
  while (run->pull || run->position < run->source.rows) {
    const turbodb_value_t *row = NULL;
    if (run->position == SIZE_MAX) return scan_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL sort source position overflow");
    turbodb_status_t status;
    if (run->pull) {
      status = run->pull->next(run->pull->context, &row, error);
      if (status == TURBODB_STATUS_OK && !row) break;
    } else {
      row = run->source.values + run->position * run->source.columns;
      status = scan_admit(run, row, error);
    }
    if (status != TURBODB_STATUS_OK) return status;
    bool matches = true;
    if (run->expression.program) {
      turbodb_value_t predicate;
      status = scan_eval(run, row, &run->expression, &run->slots, &run->inputs, &predicate, error);
      if (status != TURBODB_STATUS_OK) return status;
      matches = predicate.kind == TURBODB_VALUE_BOOLEAN && predicate.data.boolean_value;
    }
    if (matches) status = scan_capture(run, row, error);
    if (status != TURBODB_STATUS_OK) return status;
    ++run->position;
  }
  const size_t count = vec_size(&run->snapshots.snapshots);
  turbodb_status_t status = orm_sql_work_zero(&run->sorted, count, sizeof(scan_sorted_row), _Alignof(scan_sorted_row),
      run->budget, &run->sorted_bytes, error);
  if (status != TURBODB_STATUS_OK) return status;
  for (size_t i = 0; i < count; ++i) {
    *(scan_sorted_row *)vec_at(&run->sorted, i) = (scan_sorted_row){orm_sql_rows_at(&run->snapshots,i),
      vec_data_const(&run->orders), run->source.columns, vec_size(&run->orders), vec_size(&run->projection)};
  }
  run->sorted_count = count;
  if (run->distinct) status = scan_distinct(run, error);
  if (status == TURBODB_STATUS_OK && vec_size(&run->orders))
    status = orm_sql_work_sort(vec_data(&run->sorted), run->sorted_count, &scan_sorted_type,
        vec_size(&run->orders), run->budget, error);
  if (status == TURBODB_STATUS_OK) {
    run->sorted_position = run->offset_left > run->sorted_count ? run->sorted_count : (size_t)run->offset_left;
    run->offset_left = 0; run->sorted_ready = true;
  }
  return status;
}
static turbodb_status_t scan_next(orm_sql_scan *run, orm_sql_scan_row *out, turbodb_error_t *error) {
  scan_clear(&run->output);
  if (run->state == ORM_SQL_SCAN_ERROR) {
    tdsql_error_set(error, run->failure.status, run->failure.message);
    return run->failure.status;
  }
  if (run->state == ORM_SQL_SCAN_CANCELLED || run->state == ORM_SQL_SCAN_DONE) {
    *out = (orm_sql_scan_row){run->state, NULL, 0};
    return TURBODB_STATUS_OK;
  }
  turbodb_error_t cause; tdsql_error_init(&cause);
  if ((vec_size(&run->orders) || run->distinct) && run->limit_left) {
    turbodb_status_t status = run->sorted_ready ? TURBODB_STATUS_OK : scan_prepare_sort(run, &cause);
    if (status == TURBODB_STATUS_OK && run->sorted_position < run->sorted_count) {
      const scan_sorted_row *row = vec_at_const(&run->sorted, run->sorted_position);
      if (run->distinct) {
        orm_sql_budget_amount steps = {0}; steps.value[ORM_SQL_BUDGET_EXECUTION_STEPS] = row->outputs;
        status = orm_tidesdb_sql_budget_reserve(run->budget, &steps, &cause);
        if (status == TURBODB_STATUS_OK) memcpy(vec_data(&run->output), row->row + row->columns + row->count,
            row->outputs * sizeof(turbodb_value_t));
      } else status = scan_project_row(run, row->row, &cause);
      if (status == TURBODB_STATUS_OK) {
        ++run->sorted_position; --run->limit_left; run->state = ORM_SQL_SCAN_ROW;
        *out = (orm_sql_scan_row){ORM_SQL_SCAN_ROW, vec_data_const(&run->output), vec_size(&run->output)};
        return TURBODB_STATUS_OK;
      }
    }
    if (status != TURBODB_STATUS_OK) return scan_fail(run, status, &cause, error);
    run->limit_left = 0;
  }
  while (run->limit_left && (run->pull || run->position < run->source.rows)) {
    const turbodb_value_t *row = NULL;
    turbodb_status_t status;
    if (run->position == SIZE_MAX) {
      status = scan_error(&cause, TURBODB_STATUS_LIMIT_EXCEEDED, "SQL scan row position overflow");
    } else if (run->pull) {
      status = run->pull->next(run->pull->context, &row, &cause);
      if (status == TURBODB_STATUS_OK && !row) break;
    } else {
      row = run->source.values + run->position * run->source.columns;
      status = scan_admit(run, row, &cause);
    }
    if (status != TURBODB_STATUS_OK) return scan_fail(run, status, &cause, error);
    bool matches = true;
    if (run->expression.program) {
      turbodb_value_t predicate;
      status = scan_eval(run, row, &run->expression, &run->slots, &run->inputs, &predicate, &cause);
      if (status != TURBODB_STATUS_OK) return scan_fail(run, status, &cause, error);
      matches = predicate.kind == TURBODB_VALUE_BOOLEAN && predicate.data.boolean_value;
    }
    if (!matches || run->offset_left) {
      if (matches) --run->offset_left;
      ++run->position;
      continue;
    }
    status = scan_project_row(run, row, &cause);
    if (status != TURBODB_STATUS_OK) return scan_fail(run, status, &cause, error);
    ++run->position; --run->limit_left;
    run->state = ORM_SQL_SCAN_ROW;
    *out = (orm_sql_scan_row){ORM_SQL_SCAN_ROW, vec_data_const(&run->output), vec_size(&run->output)};
    return TURBODB_STATUS_OK;
  }
  run->state = ORM_SQL_SCAN_DONE;
  *out = (orm_sql_scan_row){ORM_SQL_SCAN_DONE, NULL, 0};
  return TURBODB_STATUS_OK;
}

turbodb_status_t orm_tidesdb_sql_scan_next(orm_sql_scan *run, orm_sql_scan_row *out, turbodb_error_t *error) {
  if (!run || !run->budget || !out)
    return scan_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid TidesDB SQL scan next");
  if (run->evaluating) return scan_error(error, TURBODB_STATUS_BUSY, "SQL scan is evaluating");
  run->evaluating = true;
  const turbodb_status_t status = scan_next(run, out, error);
  run->evaluating = false;
  return status;
}

turbodb_status_t orm_tidesdb_sql_scan_cancel(orm_sql_scan *run, turbodb_error_t *error) {
  if (!run || !run->budget)
    return scan_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid TidesDB SQL scan cancel");
  if (run->evaluating) return scan_error(error, TURBODB_STATUS_BUSY, "SQL scan is evaluating");
  if (run->state == ORM_SQL_SCAN_OPEN || run->state == ORM_SQL_SCAN_ROW)
    run->state = ORM_SQL_SCAN_CANCELLED;
  scan_clear(&run->inputs); scan_clear(&run->output);
  return TURBODB_STATUS_OK;
}
