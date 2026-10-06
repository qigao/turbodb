#include "relational_backend.h"
#include <tidessql/tidessql.h>
#include "orm_mysql_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tlog.h>

typedef enum rel_reader_phase { REL_MAP_BEGIN, REL_KEY, REL_VALUE, REL_MAP_END, REL_READER_DONE } rel_reader_phase;
typedef struct rel_cursor {
  tdsql_result *result;
  tdsql_row row;
  size_t column;
  rel_reader_phase phase;
  orm_error_t error;
} rel_cursor;
typedef struct rel_input {
  const orm_query_plan *plan;
  orm_mysql_rendered_query rendered;
  tdsql_request request;
} rel_input;

static orm_status_t rel_error(orm_error_t *error, orm_status_t status, const char *message) {
  orm_error_set(error, status, message); return status;
}
static tdsql_limits rel_limits(const orm_limits *limits) {
  return (tdsql_limits){limits->max_parameters, limits->max_columns,
      limits->max_query_bytes, limits->max_parameter_bytes,
      limits->max_result_rows, limits->max_result_bytes};
}
static orm_value_t rel_parameter(const orm_owned_value *v) {
  switch (v->kind) {
    case ORM_VALUE_INT64: return orm_i64(v->data.int64_value);
    case ORM_VALUE_UINT64: return orm_u64(v->data.uint64_value);
    case ORM_VALUE_DOUBLE: return orm_f64(v->data.double_value);
    case ORM_VALUE_BOOLEAN: return orm_bool(v->data.boolean_value != 0);
    case ORM_VALUE_TEXT: return orm_text_v((vstr){v->bytes, tstr_len(v->bytes)});
    case ORM_VALUE_BLOB: return orm_blob(v->bytes, tstr_len(v->bytes));
    default: return orm_null();
  }
}
static orm_value_t TDSQL_CALL rel_parameter_at(const void *context, size_t index) {
  const rel_input *input = context;
  return rel_parameter(input->plan->kind == ORM_QUERY_RAW ?
      vec_at_const(&input->plan->raw_parameters, index) : input->rendered.parameters[index]);
}
static orm_status_t rel_input_open(const orm_query_plan *plan, const orm_limits *limits,
    rel_input *input, orm_error_t *error) {
  input->plan = plan;
  if (plan->kind != ORM_QUERY_RAW) {
    if (plan->kind != ORM_QUERY_SELECT && (plan->ordering.present || plan->has_limit || plan->has_offset))
      return rel_error(error, ORM_STATUS_UNSUPPORTED, "relational structured command does not accept ordering or pagination");
    const orm_status_t status = orm_mysql_render_plan(plan, limits, &input->rendered, error);
    if (status != ORM_STATUS_OK) return status;
  }
  const tstr sql = plan->kind == ORM_QUERY_RAW ? plan->raw_sql : input->rendered.text;
  input->request = (tdsql_request){.struct_size=sizeof(tdsql_request),.abi_version=TDSQL_ABI_VERSION,.sql={sql,tstr_len(sql)},
      .parameter_count=plan->kind == ORM_QUERY_RAW ? vec_size(&plan->raw_parameters) : input->rendered.parameter_count,
      .read_parameter=rel_parameter_at,.parameter_context=input,.limits=rel_limits(limits),
      .result_context_bytes=sizeof(rel_cursor),.unique_column_names=true};
  return ORM_STATUS_OK;
}

static cserde_status rel_reader_next(void *context, cserde_token *out) {
  rel_cursor *c = context;
  if (!c || !out || !c->row.values) return CSERDE_INVALID_ARGUMENT;
  memset(out, 0, sizeof(*out));
  switch (c->phase) {
    case REL_MAP_BEGIN: out->kind = CSERDE_MAP_BEGIN; c->phase = c->row.count ? REL_KEY : REL_MAP_END; break;
    case REL_KEY: {
      tdsql_column column;
      if (tdsql_result_column(c->result, c->column, &column, NULL) != ORM_STATUS_OK) return CSERDE_SOURCE_ERROR;
      out->kind = CSERDE_STRING; out->value.slice.data = (const unsigned char *)column.name.data; out->value.slice.size = column.name.len;
      out->value.slice.lifetime = CSERDE_VIEW_TRANSIENT; c->phase = REL_VALUE; break;
    }
    case REL_VALUE: {
      const orm_value_t *v = &c->row.values[c->column];
      switch (v->kind) {
        case ORM_VALUE_NULL: out->kind = CSERDE_NULL; break;
        case ORM_VALUE_INT64: out->kind = CSERDE_SINT; out->value.sint = v->data.int64_value; break;
        case ORM_VALUE_UINT64: out->kind = CSERDE_UINT; out->value.uint = v->data.uint64_value; break;
        case ORM_VALUE_DOUBLE: out->kind = CSERDE_FLOAT; out->value.floating = v->data.double_value; break;
        case ORM_VALUE_BOOLEAN: out->kind = CSERDE_BOOL; out->value.boolean = v->data.boolean_value != 0; break;
        case ORM_VALUE_TEXT:
          out->kind = CSERDE_STRING; out->value.slice.data = (const unsigned char *)v->data.text_value.data;
          out->value.slice.size = v->data.text_value.len; out->value.slice.lifetime = CSERDE_VIEW_TRANSIENT; break;
        case ORM_VALUE_BLOB:
          out->kind = CSERDE_BYTES; out->value.slice.data = v->data.blob_value.data;
          out->value.slice.size = v->data.blob_value.size; out->value.slice.lifetime = CSERDE_VIEW_TRANSIENT; break;
        default: return CSERDE_SOURCE_ERROR;
      }
      c->phase = ++c->column == c->row.count ? REL_MAP_END : REL_KEY; break;
    }
    case REL_MAP_END: out->kind = CSERDE_MAP_END; c->phase = REL_READER_DONE; break;
    case REL_READER_DONE: return CSERDE_DONE;
    default: return CSERDE_SOURCE_ERROR;
  }
  return CSERDE_OK;
}
static const cserde_reader_ops rel_reader_ops = {sizeof(cserde_reader_ops), CSERDE_READER_OPS_ABI_VERSION, rel_reader_next};
static orm_row_cursor_step rel_cursor_next(void *context, cserde_reader *out) {
  rel_cursor *c = context; orm_row_cursor_step step = ORM_ROW_CURSOR_STEP_INIT;
  c->row = (tdsql_row){0};
  orm_status_t status = tdsql_result_next(c->result, &c->row, &c->error);
  if (status == ORM_STATUS_OK && c->row.state != TDSQL_ROW) return step;
  if (status == ORM_STATUS_OK) {
    c->column = 0; c->phase = REL_MAP_BEGIN;
    if (cserde_reader_init(out, &rel_reader_ops, c) != CSERDE_OK)
      status = rel_error(&c->error, ORM_STATUS_INTERNAL_ERROR, "initialize relational result reader");
  }
  if (status != ORM_STATUS_OK) {
    step.kind = ORM_ROW_CURSOR_ERROR; step.status = status; step.message = c->error.message;
  } else step.kind = ORM_ROW_CURSOR_ROW;
  return step;
}
static orm_status_t rel_cursor_cancel_checked(void *context, orm_error_t *error) {
  rel_cursor *c = context;
  c->row = (tdsql_row){0};
  return tdsql_result_cancel(c->result, error);
}
static void rel_cursor_cancel(void *context) { (void)rel_cursor_cancel_checked(context, NULL); }
static void rel_cursor_destroy(void *context) {
  rel_cursor *c = context;
  if (c) tdsql_result_destroy(c->result);
}
static orm_status_t rel_column_count(void *context, uint64_t *out) {
  if (!context || !out) return ORM_STATUS_INVALID_ARGUMENT;
  *out = tdsql_result_columns(((rel_cursor *)context)->result); return ORM_STATUS_OK;
}
static const orm_row_cursor_ops rel_cursor_ops = {sizeof(orm_row_cursor_ops), ORM_ROW_CURSOR_OPS_ABI_VERSION,
  "tidesdb-relational", rel_cursor_next, rel_cursor_cancel, rel_cursor_destroy, NULL, rel_column_count};
static void rel_result_publish(tdsql_result *result, orm_row_cursor *out) {
  rel_cursor *cursor = tdsql_result_context(result);
  cursor->result = result; orm_error_init(&cursor->error);
  out->ops = &rel_cursor_ops; out->context = cursor; out->cancel_checked = rel_cursor_cancel_checked;
}

static orm_status_t rel_execute(void *context, const orm_query_plan *plan,
    const orm_limits *limits, uint64_t *affected, orm_error_t *error) {
  rel_input input = {0};
  orm_status_t status = tdsql_transaction_prepare(context, true, error);
  if (status == ORM_STATUS_OK) status = rel_input_open(plan, limits, &input, error);
  if (status == ORM_STATUS_OK) status = tdsql_transaction_execute(context, &input.request, affected, error);
  orm_mysql_rendered_query_destroy(&input.rendered); return status;
}
static orm_status_t rel_open(void *context, const orm_query_plan *plan,
    const orm_limits *limits, orm_row_cursor *out, orm_error_t *error) {
  rel_input input = {0}; tdsql_result *result = NULL;
  orm_status_t status = tdsql_transaction_prepare(context, false, error);
  if (status == ORM_STATUS_OK) status = rel_input_open(plan, limits, &input, error);
  if (status == ORM_STATUS_OK) status = tdsql_transaction_query(context, &input.request, &result, error);
  orm_mysql_rendered_query_destroy(&input.rendered);
  if (status == ORM_STATUS_OK) rel_result_publish(result, out);
  return status;
}
static orm_status_t rel_commit(void *context, orm_error_t *error) { return tdsql_transaction_finish(context, true, error); }
static orm_status_t rel_rollback(void *context, orm_error_t *error) { return tdsql_transaction_finish(context, false, error); }
static orm_status_t rel_savepoint(void *context, vstr name, orm_error_t *error) { return tdsql_transaction_savepoint(context, name, TDSQL_SAVEPOINT_CREATE, error); }
static orm_status_t rel_rollback_to(void *context, vstr name, orm_error_t *error) { return tdsql_transaction_savepoint(context, name, TDSQL_SAVEPOINT_ROLLBACK, error); }
static orm_status_t rel_release_savepoint(void *context, vstr name, orm_error_t *error) { return tdsql_transaction_savepoint(context, name, TDSQL_SAVEPOINT_RELEASE, error); }
static void rel_transaction_release(void *context) { tdsql_transaction_release(context); }
static const orm_transaction_backend_ops rel_transaction_ops = {
  sizeof(orm_transaction_backend_ops), ORM_TRANSACTION_BACKEND_OPS_ABI_VERSION,
  rel_transaction_release, rel_open, rel_execute, rel_commit, rel_rollback,
  rel_savepoint, rel_rollback_to, rel_release_savepoint
};
static orm_status_t rel_backend_begin(void *context, orm_isolation_t isolation,
    orm_transaction_backend *out, orm_error_t *error) {
  if (isolation != ORM_ISOLATION_SERIALIZABLE)
    return rel_error(error, ORM_STATUS_UNSUPPORTED, "relational transactions require SERIALIZABLE isolation");
  tdsql_transaction *transaction = NULL;
  const orm_status_t status = tdsql_connection_begin(context, &transaction, error);
  if (status == ORM_STATUS_OK) { out->ops = &rel_transaction_ops; out->context = transaction; }
  return status;
}
static orm_status_t rel_backend_execute(void *context, const orm_query_plan *plan,
    const orm_limits *limits, uint64_t *affected, orm_error_t *error) {
  rel_input input = {0};
  orm_status_t status = tdsql_connection_prepare(context, true, error);
  if (status == ORM_STATUS_OK) status = rel_input_open(plan, limits, &input, error);
  if (status == ORM_STATUS_OK) status = tdsql_connection_execute(context, &input.request, affected, error);
  orm_mysql_rendered_query_destroy(&input.rendered); return status;
}
static orm_status_t rel_backend_open(void *context, const orm_query_plan *plan,
    const orm_limits *limits, orm_row_cursor *out, orm_error_t *error) {
  rel_input input = {0}; tdsql_result *result = NULL;
  orm_status_t status = tdsql_connection_prepare(context, false, error);
  if (status == ORM_STATUS_OK) status = rel_input_open(plan, limits, &input, error);
  if (status == ORM_STATUS_OK) status = tdsql_connection_query(context, &input.request, &result, error);
  orm_mysql_rendered_query_destroy(&input.rendered);
  if (status == ORM_STATUS_OK) rel_result_publish(result, out);
  return status;
}
static void rel_backend_destroy(void *context) {
  orm_error_t error; orm_error_init(&error);
  const orm_status_t status = tdsql_connection_close(context, &error);
  if (status != ORM_STATUS_OK) {
    char message[ORM_C_ERROR_MESSAGE_CAPACITY];
    (void)snprintf(message, sizeof(message), "SQL session rollback during connection destroy failed: status=%d, reason=%s; cannot continue",
        (int)status, error.message);
    SALTS_LOG_FATAL(tlog_get_default(), "tidesdb-relational", message);
    tlog_flush(tlog_get_default()); abort();
  }
}
static const orm_backend_ops rel_backend_ops = {sizeof(orm_backend_ops), ORM_BACKEND_OPS_ABI_VERSION,
  rel_backend_destroy, rel_backend_open, rel_backend_execute, rel_backend_begin, NULL};
orm_status_t orm_tidesdb_relational_create(const orm_config_t *config,
    const orm_limits *limits, orm_backend *out, orm_error_t *error) {
  if (!config || !limits || !out || (config->option_count && !config->options))
    return rel_error(error, ORM_STATUS_INVALID_ARGUMENT, "invalid relational backend request");
  *out = (orm_backend){0};
  tdsql_config engine_config = tdsql_config_default();
  engine_config.options = config->options; engine_config.option_count = config->option_count;
  engine_config.limits = rel_limits(limits);
  tdsql_connection *connection = NULL;
  const orm_status_t status = tdsql_connection_open(&engine_config, &connection, error);
  if (status == ORM_STATUS_OK) { out->ops = &rel_backend_ops; out->context = connection; }
  return status;
}
