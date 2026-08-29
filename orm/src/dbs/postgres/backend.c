#include "orm_internal.h"
#include "orm_postgres_cursor.h"
#include "orm_postgres_libpq.h"
#include "orm_sql_render.h"

#include <libpq-fe.h>

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  ORM_POSTGRES_OPTION_KEY_MAX = 63u,
  ORM_POSTGRES_OPTION_VALUE_MAX = 4096u,
  ORM_POSTGRES_OPTION_TOTAL_MAX = 65536u
};

typedef struct orm_postgres_backend_state {
  PGconn *connection;
  int transaction_active;
} orm_postgres_backend_state;

typedef struct orm_postgres_transaction_state {
  orm_postgres_backend_state *owner;
  int active;
} orm_postgres_transaction_state;

typedef struct orm_postgres_parameters {
  const char **values;
  tstr *owned;
  size_t count;
} orm_postgres_parameters;

static orm_status_t orm_postgres_fail(orm_error_t *error,
                                      orm_status_t status,
                                      const char *operation,
                                      const char *detail) {
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "%s: %s", operation,
                 detail != NULL && detail[0] != '\0'
                     ? detail
                     : orm_status_message(status));
  orm_error_set(error, status, message);
  return status;
}

static int orm_postgres_identifier(vstr value) {
  size_t index;
  if (!orm_view_valid(value, false) ||
      value.len > ORM_POSTGRES_OPTION_KEY_MAX)
    return 0;
  for (index = 0u; index < value.len; ++index) {
    const unsigned char next = (unsigned char)value.data[index];
    const int alpha = (next >= (unsigned char)'a' &&
                       next <= (unsigned char)'z') ||
                      (next >= (unsigned char)'A' &&
                       next <= (unsigned char)'Z') ||
                      next == (unsigned char)'_';
    if ((index == 0u && !alpha) ||
        (index != 0u && !alpha &&
         !(next >= (unsigned char)'0' && next <= (unsigned char)'9')))
      return 0;
  }
  return 1;
}

static void orm_postgres_parameters_destroy(
    orm_postgres_parameters *parameters) {
  size_t index;
  if (parameters == NULL)
    return;
  for (index = 0u; index < parameters->count; ++index)
    tstr_free(parameters->owned != NULL ? parameters->owned[index] : NULL);
  free(parameters->owned);
  free(parameters->values);
  memset(parameters, 0, sizeof(*parameters));
}

static orm_status_t orm_postgres_hex_blob(const orm_owned_value *value,
                                          tstr *output,
                                          orm_error_t *error) {
  static const char digits[] = "0123456789abcdef";
  const size_t size = tstr_len(value->bytes);
  size_t index;
  char *cursor;
  if (size > (SIZE_MAX - 2u) / 2u) {
    orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                  "PostgreSQL blob encoding exceeds the platform range");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  *output = tstr_new_len(NULL, 2u + size * 2u);
  if (*output == NULL) {
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "encode PostgreSQL blob parameter");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  cursor = *output;
  cursor[0] = '\\';
  cursor[1] = 'x';
  for (index = 0u; index < size; ++index) {
    const unsigned char next = (unsigned char)value->bytes[index];
    cursor[2u + index * 2u] = digits[next >> 4u];
    cursor[3u + index * 2u] = digits[next & 0x0fu];
  }
  return ORM_STATUS_OK;
}

static orm_status_t orm_postgres_encode_value(const orm_owned_value *value,
                                              tstr *output,
                                              orm_error_t *error) {
  char buffer[128];
  int length;
  if (value == NULL || output == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid PostgreSQL parameter");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  *output = NULL;
  switch (value->kind) {
    case ORM_VALUE_NULL:
      return ORM_STATUS_OK;
    case ORM_VALUE_INT64:
      length = snprintf(buffer, sizeof(buffer), "%" PRId64,
                        value->data.int64_value);
      break;
    case ORM_VALUE_UINT64:
      length = snprintf(buffer, sizeof(buffer), "%" PRIu64,
                        value->data.uint64_value);
      break;
    case ORM_VALUE_DOUBLE:
      if (!isfinite(value->data.double_value)) {
        orm_error_set(error, ORM_STATUS_OUT_OF_RANGE,
                      "PostgreSQL double parameter must be finite");
        return ORM_STATUS_OUT_OF_RANGE;
      }
      length = snprintf(buffer, sizeof(buffer), "%.17g",
                        value->data.double_value);
      break;
    case ORM_VALUE_BOOLEAN:
      *output = tstr_dup(value->data.boolean_value != 0u ? "true" : "false");
      return *output != NULL ? ORM_STATUS_OK
                             : orm_postgres_fail(
                                   error, ORM_STATUS_OUT_OF_MEMORY,
                                   "encode PostgreSQL boolean", NULL);
    case ORM_VALUE_TEXT:
      if (memchr(value->bytes, '\0', tstr_len(value->bytes)) != NULL) {
        orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                      "PostgreSQL text parameters cannot contain NUL bytes");
        return ORM_STATUS_INVALID_ARGUMENT;
      }
      *output = tstr_clone(value->bytes);
      return *output != NULL ? ORM_STATUS_OK
                             : orm_postgres_fail(
                                   error, ORM_STATUS_OUT_OF_MEMORY,
                                   "copy PostgreSQL text parameter", NULL);
    case ORM_VALUE_BLOB:
      return orm_postgres_hex_blob(value, output, error);
    default:
      orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                    "unknown PostgreSQL parameter kind");
      return ORM_STATUS_INVALID_ARGUMENT;
  }
  if (length <= 0 || (size_t)length >= sizeof(buffer)) {
    orm_error_set(error, ORM_STATUS_TYPE_ERROR,
                  "format PostgreSQL parameter failed");
    return ORM_STATUS_TYPE_ERROR;
  }
  *output = tstr_dup_len(buffer, (size_t)length);
  if (*output == NULL) {
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "allocate PostgreSQL parameter");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  return ORM_STATUS_OK;
}

static orm_status_t orm_postgres_parameters_init(
    const orm_sql_query *query, orm_postgres_parameters *parameters,
    orm_error_t *error) {
  size_t index;
  if (query == NULL || parameters == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid PostgreSQL parameter list");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(parameters, 0, sizeof(*parameters));
  parameters->count = vec_size(&query->parameters);
  if (parameters->count > (size_t)INT_MAX) {
    orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                  "PostgreSQL parameter count exceeds libpq range");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  if (parameters->count == 0u)
    return ORM_STATUS_OK;
  parameters->values = (const char **)calloc(parameters->count,
                                             sizeof(*parameters->values));
  parameters->owned = (tstr *)calloc(parameters->count,
                                     sizeof(*parameters->owned));
  if (parameters->values == NULL || parameters->owned == NULL) {
    orm_postgres_parameters_destroy(parameters);
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "allocate PostgreSQL parameter arrays");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  for (index = 0u; index < parameters->count; ++index) {
    const orm_owned_value *value = *(const orm_owned_value *const *)
        vec_at_const(&query->parameters, index);
    const orm_status_t status =
        orm_postgres_encode_value(value, &parameters->owned[index], error);
    if (status != ORM_STATUS_OK) {
      orm_postgres_parameters_destroy(parameters);
      return status;
    }
    parameters->values[index] = parameters->owned[index];
  }
  return ORM_STATUS_OK;
}

static orm_status_t orm_postgres_open_impl(
    orm_postgres_backend_state *state, const orm_query_plan *plan,
    const orm_limits *limits, int allow_transaction,
    size_t *column_count, uint64_t *affected_rows,
    orm_row_cursor *out_cursor, orm_error_t *error) {
  orm_sql_query rendered;
  orm_postgres_parameters parameters;
  orm_postgres_driver driver;
  orm_postgres_query_request request;
  orm_postgres_cursor_config cursor_config;
  orm_status_t status;
  if (state == NULL || state->connection == NULL || plan == NULL ||
      limits == NULL || out_cursor == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid PostgreSQL query request");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(out_cursor, 0, sizeof(*out_cursor));
  if (!allow_transaction && state->transaction_active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "use the transaction handle while a transaction is active");
    return ORM_STATUS_INVALID_STATE;
  }
  status = orm_sql_render(plan, limits, ORM_SQL_POSTGRES, &rendered, error);
  if (status != ORM_STATUS_OK)
    return status;
  status = orm_postgres_parameters_init(&rendered, &parameters, error);
  if (status != ORM_STATUS_OK) {
    orm_sql_query_destroy(&rendered);
    return status;
  }
  driver = orm_postgres_libpq_driver(state->connection);
  request.sql = rendered.text;
  request.parameter_count = (int)parameters.count;
  request.parameter_types = NULL;
  request.parameter_values = parameters.values;
  request.parameter_lengths = NULL;
  request.parameter_formats = NULL;
  request.result_format = 0;
  cursor_config = (orm_postgres_cursor_config)
      ORM_POSTGRES_CURSOR_CONFIG_INIT(
          limits->max_columns, limits->max_result_rows,
          (size_t)limits->max_result_bytes,
          column_count, affected_rows, NULL);
  status = orm_postgres_cursor_start(out_cursor, &driver, &request,
                                     &cursor_config, error);
  orm_postgres_parameters_destroy(&parameters);
  orm_sql_query_destroy(&rendered);
  return status;
}

static orm_status_t orm_postgres_drain_command(orm_row_cursor *cursor,
                                               size_t columns,
                                               uint64_t affected,
                                               uint64_t *affected_rows,
                                               orm_error_t *error) {
  orm_status_t status = ORM_STATUS_OK;
  for (;;) {
    cserde_reader row = {0};
    const orm_row_cursor_step step = cursor->ops->next(cursor->context, &row);
    if (step.kind == ORM_ROW_CURSOR_DONE)
      break;
    if (step.kind == ORM_ROW_CURSOR_ERROR) {
      status = step.status != ORM_STATUS_OK ? step.status
                                           : ORM_STATUS_DATASTORE_ERROR;
      orm_error_set(error, status, step.message);
      break;
    }
    if (step.kind == ORM_ROW_CURSOR_ROW) {
      status = ORM_STATUS_UNSUPPORTED;
      orm_error_set(error, status,
                    "PostgreSQL row queries must be opened as a row Source");
      break;
    }
    status = ORM_STATUS_INTERNAL_ERROR;
    orm_error_set(error, status,
                  "PostgreSQL cursor returned an invalid step");
    break;
  }
  cursor->ops->destroy(cursor->context);
  if (status == ORM_STATUS_OK && columns != 0u) {
    status = ORM_STATUS_UNSUPPORTED;
    orm_error_set(error, status,
                  "PostgreSQL row queries must be opened as a row Source");
  }
  if (status == ORM_STATUS_OK && affected_rows != NULL)
    *affected_rows = affected;
  return status;
}

static orm_status_t orm_postgres_execute_impl(
    orm_postgres_backend_state *state, const orm_query_plan *plan,
    const orm_limits *limits, int allow_transaction,
    uint64_t *affected_rows, orm_error_t *error) {
  orm_row_cursor cursor = {0};
  size_t columns = 0u;
  uint64_t affected = 0u;
  const orm_status_t status = orm_postgres_open_impl(
      state, plan, limits, allow_transaction, &columns, &affected, &cursor,
      error);
  if (status != ORM_STATUS_OK)
    return status;
  return orm_postgres_drain_command(&cursor, columns, affected, affected_rows,
                                    error);
}

static orm_status_t orm_postgres_control(orm_postgres_backend_state *state,
                                         const char *sql,
                                         orm_error_t *error) {
  orm_row_cursor cursor = {0};
  orm_postgres_driver driver;
  orm_postgres_query_request request;
  orm_postgres_cursor_config cursor_config;
  size_t columns = 0u;
  uint64_t affected = 0u;
  orm_status_t status;
  if (state == NULL || state->connection == NULL || sql == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid PostgreSQL control request");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  driver = orm_postgres_libpq_driver(state->connection);
  memset(&request, 0, sizeof(request));
  request.sql = sql;
  cursor_config = (orm_postgres_cursor_config)
      ORM_POSTGRES_CURSOR_CONFIG_INIT(1u, 1u, 1u, &columns, &affected, NULL);
  status = orm_postgres_cursor_start(&cursor, &driver, &request,
                                     &cursor_config, error);
  if (status != ORM_STATUS_OK)
    return status;
  return orm_postgres_drain_command(&cursor, columns, affected, NULL, error);
}

static void orm_postgres_backend_destroy(void *context) {
  orm_postgres_backend_state *state =
      (orm_postgres_backend_state *)context;
  if (state == NULL)
    return;
  if (state->connection != NULL)
    PQfinish(state->connection);
  free(state);
}

static orm_status_t orm_postgres_backend_open(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    orm_row_cursor *out_cursor, orm_error_t *error) {
  return orm_postgres_open_impl((orm_postgres_backend_state *)context, plan,
                                limits, 0, NULL, NULL, out_cursor, error);
}

static orm_status_t orm_postgres_backend_execute(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    uint64_t *affected_rows, orm_error_t *error) {
  return orm_postgres_execute_impl((orm_postgres_backend_state *)context,
                                   plan, limits, 0, affected_rows, error);
}

static orm_status_t orm_postgres_transaction_open(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    orm_row_cursor *out_cursor, orm_error_t *error) {
  orm_postgres_transaction_state *transaction =
      (orm_postgres_transaction_state *)context;
  if (transaction == NULL || !transaction->active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "PostgreSQL transaction is no longer active");
    return ORM_STATUS_INVALID_STATE;
  }
  return orm_postgres_open_impl(transaction->owner, plan, limits, 1, NULL,
                                NULL, out_cursor, error);
}

static orm_status_t orm_postgres_transaction_execute(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    uint64_t *affected_rows, orm_error_t *error) {
  orm_postgres_transaction_state *transaction =
      (orm_postgres_transaction_state *)context;
  if (transaction == NULL || !transaction->active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "PostgreSQL transaction is no longer active");
    return ORM_STATUS_INVALID_STATE;
  }
  return orm_postgres_execute_impl(transaction->owner, plan, limits, 1,
                                   affected_rows, error);
}

static orm_status_t orm_postgres_transaction_finish(
    orm_postgres_transaction_state *transaction, const char *sql,
    orm_error_t *error) {
  orm_status_t status;
  if (transaction == NULL || !transaction->active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "PostgreSQL transaction is no longer active");
    return ORM_STATUS_INVALID_STATE;
  }
  status = orm_postgres_control(transaction->owner, sql, error);
  if (status == ORM_STATUS_OK) {
    transaction->active = 0;
    transaction->owner->transaction_active = 0;
  }
  return status;
}

static orm_status_t orm_postgres_transaction_commit(void *context,
                                                    orm_error_t *error) {
  return orm_postgres_transaction_finish(
      (orm_postgres_transaction_state *)context, "commit", error);
}

static orm_status_t orm_postgres_transaction_rollback(void *context,
                                                      orm_error_t *error) {
  return orm_postgres_transaction_finish(
      (orm_postgres_transaction_state *)context, "rollback", error);
}

static orm_status_t orm_postgres_savepoint_sql(
    orm_postgres_transaction_state *transaction, const char *prefix,
    vstr name, orm_error_t *error) {
  char sql[192];
  int length;
  if (transaction == NULL || !transaction->active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "PostgreSQL transaction is no longer active");
    return ORM_STATUS_INVALID_STATE;
  }
  if (!orm_postgres_identifier(name)) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid PostgreSQL savepoint identifier");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  length = snprintf(sql, sizeof(sql), "%s \"%.*s\"", prefix,
                    (int)name.len, name.data);
  if (length <= 0 || (size_t)length >= sizeof(sql)) {
    orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                  "PostgreSQL savepoint command is too long");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  return orm_postgres_control(transaction->owner, sql, error);
}

static orm_status_t orm_postgres_transaction_savepoint(void *context,
                                                       vstr name,
                                                       orm_error_t *error) {
  return orm_postgres_savepoint_sql(
      (orm_postgres_transaction_state *)context, "savepoint", name, error);
}

static orm_status_t orm_postgres_transaction_rollback_to(
    void *context, vstr name, orm_error_t *error) {
  return orm_postgres_savepoint_sql(
      (orm_postgres_transaction_state *)context, "rollback to savepoint",
      name, error);
}

static orm_status_t orm_postgres_transaction_release(void *context,
                                                     vstr name,
                                                     orm_error_t *error) {
  return orm_postgres_savepoint_sql(
      (orm_postgres_transaction_state *)context, "release savepoint", name,
      error);
}

static void orm_postgres_transaction_destroy(void *context) {
  orm_postgres_transaction_state *transaction =
      (orm_postgres_transaction_state *)context;
  if (transaction == NULL)
    return;
  if (transaction->active) {
    orm_error_t ignored;
    orm_error_init(&ignored);
    (void)orm_postgres_control(transaction->owner, "rollback", &ignored);
    transaction->owner->transaction_active = 0;
    transaction->active = 0;
  }
  free(transaction);
}

static const orm_transaction_backend_ops orm_postgres_transaction_ops = {
    sizeof(orm_transaction_backend_ops),
    ORM_TRANSACTION_BACKEND_OPS_ABI_VERSION,
    orm_postgres_transaction_destroy,
    orm_postgres_transaction_open,
    orm_postgres_transaction_execute,
    orm_postgres_transaction_commit,
    orm_postgres_transaction_rollback,
    orm_postgres_transaction_savepoint,
    orm_postgres_transaction_rollback_to,
    orm_postgres_transaction_release};

static const char *orm_postgres_begin_sql(orm_isolation_t isolation) {
  switch (isolation) {
    case ORM_ISOLATION_READ_UNCOMMITTED:
    case ORM_ISOLATION_READ_COMMITTED:
      return "begin isolation level read committed";
    case ORM_ISOLATION_REPEATABLE_READ:
    case ORM_ISOLATION_SNAPSHOT:
      return "begin isolation level repeatable read";
    case ORM_ISOLATION_SERIALIZABLE:
      return "begin isolation level serializable";
    default:
      return NULL;
  }
}

static orm_status_t orm_postgres_backend_begin(
    void *context, orm_isolation_t isolation,
    orm_transaction_backend *out_transaction, orm_error_t *error) {
  orm_postgres_backend_state *state =
      (orm_postgres_backend_state *)context;
  orm_postgres_transaction_state *transaction;
  const char *sql = orm_postgres_begin_sql(isolation);
  orm_status_t status;
  if (state == NULL || out_transaction == NULL || sql == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid PostgreSQL transaction request");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(out_transaction, 0, sizeof(*out_transaction));
  if (state->transaction_active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "PostgreSQL connection already has an active transaction");
    return ORM_STATUS_INVALID_STATE;
  }
  status = orm_postgres_control(state, sql, error);
  if (status != ORM_STATUS_OK)
    return status;
  transaction = (orm_postgres_transaction_state *)calloc(1u,
                                                          sizeof(*transaction));
  if (transaction == NULL) {
    orm_error_t ignored;
    orm_error_init(&ignored);
    (void)orm_postgres_control(state, "rollback", &ignored);
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "allocate PostgreSQL transaction");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  transaction->owner = state;
  transaction->active = 1;
  state->transaction_active = 1;
  out_transaction->ops = &orm_postgres_transaction_ops;
  out_transaction->context = transaction;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

static const orm_backend_ops orm_postgres_backend_ops = {
    sizeof(orm_backend_ops), ORM_BACKEND_OPS_ABI_VERSION,
    orm_postgres_backend_destroy, orm_postgres_backend_open,
    orm_postgres_backend_execute, orm_postgres_backend_begin};

orm_status_t orm_postgres_backend_create(const orm_config_t *config,
                                         const orm_limits *limits,
                                         orm_backend *out_backend,
                                         orm_error_t *error) {
  orm_postgres_backend_state *state = NULL;
  tstr *keywords = NULL;
  tstr *values = NULL;
  const char **keyword_views = NULL;
  const char **value_views = NULL;
  size_t total_bytes = 0u;
  int expand_dbname = 0;
  uint32_t index;
  orm_status_t status = ORM_STATUS_OK;
  (void)limits;
  if (config == NULL || out_backend == NULL || config->option_count == 0u) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "PostgreSQL requires connection options");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(out_backend, 0, sizeof(*out_backend));
  keywords = (tstr *)calloc(config->option_count, sizeof(*keywords));
  values = (tstr *)calloc(config->option_count, sizeof(*values));
  keyword_views = (const char **)calloc((size_t)config->option_count + 1u,
                                        sizeof(*keyword_views));
  value_views = (const char **)calloc((size_t)config->option_count + 1u,
                                      sizeof(*value_views));
  if (keywords == NULL || values == NULL || keyword_views == NULL ||
      value_views == NULL) {
    status = ORM_STATUS_OUT_OF_MEMORY;
    orm_error_set(error, status, "allocate PostgreSQL connection options");
    goto cleanup;
  }
  for (index = 0u; index < config->option_count; ++index) {
    uint32_t previous;
    const orm_option_t *option = &config->options[index];
    const int conninfo_option =
        orm_view_equal_cstr(option->keyword, "conninfo");
    if (!orm_postgres_identifier(option->keyword) ||
        !orm_view_valid(option->value, true) ||
        option->value.len > ORM_POSTGRES_OPTION_VALUE_MAX ||
        memchr(option->value.data, '\0', option->value.len) != NULL ||
        option->keyword.len > ORM_POSTGRES_OPTION_TOTAL_MAX - total_bytes ||
        option->value.len > ORM_POSTGRES_OPTION_TOTAL_MAX - total_bytes -
                                option->keyword.len) {
      status = option->value.len > ORM_POSTGRES_OPTION_VALUE_MAX
                   ? ORM_STATUS_LIMIT_EXCEEDED
                   : ORM_STATUS_INVALID_ARGUMENT;
      orm_error_set(error, status, "invalid PostgreSQL connection option");
      goto cleanup;
    }
    if (conninfo_option && index != 0u) {
      status = ORM_STATUS_INVALID_ARGUMENT;
      orm_error_set(error, status,
                    "PostgreSQL conninfo must be the first connection option");
      goto cleanup;
    }
    if (orm_view_equal_cstr(option->keyword, "dbname") && expand_dbname) {
      status = ORM_STATUS_INVALID_ARGUMENT;
      orm_error_set(error, status,
                    "duplicate PostgreSQL conninfo/dbname option");
      goto cleanup;
    }
    for (previous = 0u; previous < index; ++previous) {
      const vstr prior = config->options[previous].keyword;
      if (option->keyword.len == prior.len &&
          (prior.len == 0u ||
           memcmp(option->keyword.data, prior.data, prior.len) == 0)) {
        status = ORM_STATUS_INVALID_ARGUMENT;
        orm_error_set(error, status,
                      "duplicate PostgreSQL connection option");
        goto cleanup;
      }
    }
    total_bytes += option->keyword.len + option->value.len;
    keywords[index] = conninfo_option ? tstr_dup("dbname")
                                     : tstr_from_v(option->keyword);
    values[index] = tstr_from_v(option->value);
    if (keywords[index] == NULL || values[index] == NULL) {
      status = ORM_STATUS_OUT_OF_MEMORY;
      orm_error_set(error, status, "copy PostgreSQL connection option");
      goto cleanup;
    }
    keyword_views[index] = keywords[index];
    value_views[index] = values[index];
    if (conninfo_option) expand_dbname = 1;
  }
  state = (orm_postgres_backend_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    status = ORM_STATUS_OUT_OF_MEMORY;
    orm_error_set(error, status, "allocate PostgreSQL backend");
    goto cleanup;
  }
  state->connection = PQconnectdbParams(keyword_views, value_views,
                                        expand_dbname);
  if (state->connection == NULL) {
    status = ORM_STATUS_CONNECTION_ERROR;
    orm_error_set(error, status,
                  "libpq failed to allocate a PostgreSQL connection");
    goto cleanup;
  }
  if (PQstatus(state->connection) != CONNECTION_OK) {
    status = orm_postgres_fail(error, ORM_STATUS_CONNECTION_ERROR,
                               "connect PostgreSQL",
                               PQerrorMessage(state->connection));
    goto cleanup;
  }
  out_backend->ops = &orm_postgres_backend_ops;
  out_backend->context = state;
  state = NULL;
  orm_error_set(error, ORM_STATUS_OK, NULL);

cleanup:
  if (state != NULL)
    orm_postgres_backend_destroy(state);
  for (index = 0u; index < config->option_count; ++index) {
    tstr_free(keywords != NULL ? keywords[index] : NULL);
    tstr_free(values != NULL ? values[index] : NULL);
  }
  free(keywords);
  free(values);
  free(keyword_views);
  free(value_views);
  return status;
}
