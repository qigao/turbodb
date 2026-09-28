#include "backend.h"
#include "orm_mysql_cursor.h"
#include "orm_mysql_sql.h"

#include <mysql.h>

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  ORM_MYSQL_OPTION_VALUE_MAX = 4096u
};

typedef struct orm_mysql_backend_state {
  MYSQL *connection;
  int transaction_active;
} orm_mysql_backend_state;

typedef struct orm_mysql_transaction_state {
  orm_mysql_backend_state *owner;
  int active;
} orm_mysql_transaction_state;

typedef union orm_mysql_scalar {
  int64_t sint;
  uint64_t uint;
  double real;
  signed char boolean;
} orm_mysql_scalar;

typedef struct orm_mysql_parameters {
  MYSQL_BIND *binds;
  orm_mysql_scalar *scalars;
  unsigned long *lengths;
  bool *is_null;
  size_t count;
} orm_mysql_parameters;

typedef struct orm_mysql_connect_options {
  char *host;
  char *user;
  char *password;
  char *database;
  char *unix_socket;
  char *charset;
  unsigned int port;
  unsigned int connect_timeout;
} orm_mysql_connect_options;

static orm_status_t orm_mysql_fail(
    orm_error_t *error, orm_status_t status,
    const char *operation, const char *detail) {
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  (void)snprintf(message, sizeof(message), "%s: %s", operation,
                 detail != NULL && detail[0] != '\0'
                     ? detail : orm_status_message(status));
  orm_error_set(error, status, message);
  return status;
}

static orm_status_t orm_mysql_sqlstate_status(
    const char *sqlstate, unsigned int native_error) {
  if (native_error == 1205u || native_error == 1213u)
    return ORM_STATUS_BUSY;
  if (sqlstate != NULL && strlen(sqlstate) == 5u) {
    if (sqlstate[0] == '0' && sqlstate[1] == '8')
      return ORM_STATUS_CONNECTION_ERROR;
    if (sqlstate[0] == '2' && sqlstate[1] == '3')
      return ORM_STATUS_CONSTRAINT;
    if (sqlstate[0] == '4' && sqlstate[1] == '0')
      return ORM_STATUS_BUSY;
  }
  return ORM_STATUS_SQL_ERROR;
}

static orm_status_t orm_mysql_connection_error(
    MYSQL *connection, const char *operation, orm_error_t *error) {
  const orm_status_t status = orm_mysql_sqlstate_status(
      mysql_sqlstate(connection), mysql_errno(connection));
  return orm_mysql_fail(error, status, operation, mysql_error(connection));
}

static orm_status_t orm_mysql_statement_error(
    MYSQL_STMT *statement, const char *operation, orm_error_t *error) {
  const orm_status_t status = orm_mysql_sqlstate_status(
      mysql_stmt_sqlstate(statement), mysql_stmt_errno(statement));
  return orm_mysql_fail(error, status, operation, mysql_stmt_error(statement));
}

static void orm_mysql_parameters_destroy(orm_mysql_parameters *parameters) {
  if (parameters == NULL)
    return;
  free(parameters->binds);
  free(parameters->scalars);
  free(parameters->lengths);
  free(parameters->is_null);
  memset(parameters, 0, sizeof(*parameters));
}

static orm_status_t orm_mysql_parameters_init(
    const orm_sql_query *query,
    orm_mysql_parameters *parameters,
    orm_error_t *error) {
  size_t index;
  if (query == NULL || parameters == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid MySQL parameter list");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(parameters, 0, sizeof(*parameters));
  parameters->count = vec_size(&query->parameters);
  if (parameters->count == 0u)
    return ORM_STATUS_OK;
  if (parameters->count > (size_t)UINT_MAX) {
    orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                  "MySQL parameter count exceeds client range");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }

  parameters->binds = (MYSQL_BIND *)calloc(
      parameters->count, sizeof(*parameters->binds));
  parameters->scalars = (orm_mysql_scalar *)calloc(
      parameters->count, sizeof(*parameters->scalars));
  parameters->lengths = (unsigned long *)calloc(
      parameters->count, sizeof(*parameters->lengths));
  parameters->is_null = (bool *)calloc(
      parameters->count, sizeof(*parameters->is_null));
  if (parameters->binds == NULL || parameters->scalars == NULL ||
      parameters->lengths == NULL || parameters->is_null == NULL) {
    orm_mysql_parameters_destroy(parameters);
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "allocate MySQL parameter bindings");
    return ORM_STATUS_OUT_OF_MEMORY;
  }

  for (index = 0u; index < parameters->count; ++index) {
    const orm_owned_value *value = *(const orm_owned_value *const *)
        vec_at_const(&query->parameters, index);
    MYSQL_BIND *bind = &parameters->binds[index];
    size_t size;
    if (value == NULL) {
      orm_mysql_parameters_destroy(parameters);
      orm_error_set(error, ORM_STATUS_INTERNAL_ERROR,
                    "MySQL parameter storage is invalid");
      return ORM_STATUS_INTERNAL_ERROR;
    }
    bind->length = &parameters->lengths[index];
    bind->is_null = &parameters->is_null[index];
    switch (value->kind) {
      case ORM_VALUE_NULL:
        parameters->is_null[index] = true;
        bind->buffer_type = MYSQL_TYPE_NULL;
        break;
      case ORM_VALUE_INT64:
        parameters->scalars[index].sint = value->data.int64_value;
        bind->buffer_type = MYSQL_TYPE_LONGLONG;
        bind->buffer = &parameters->scalars[index].sint;
        bind->buffer_length = sizeof(parameters->scalars[index].sint);
        break;
      case ORM_VALUE_UINT64:
        parameters->scalars[index].uint = value->data.uint64_value;
        bind->buffer_type = MYSQL_TYPE_LONGLONG;
        bind->is_unsigned = true;
        bind->buffer = &parameters->scalars[index].uint;
        bind->buffer_length = sizeof(parameters->scalars[index].uint);
        break;
      case ORM_VALUE_DOUBLE:
        if (!isfinite(value->data.double_value)) {
          orm_mysql_parameters_destroy(parameters);
          orm_error_set(error, ORM_STATUS_OUT_OF_RANGE,
                        "MySQL double parameter must be finite");
          return ORM_STATUS_OUT_OF_RANGE;
        }
        parameters->scalars[index].real = value->data.double_value;
        bind->buffer_type = MYSQL_TYPE_DOUBLE;
        bind->buffer = &parameters->scalars[index].real;
        bind->buffer_length = sizeof(parameters->scalars[index].real);
        break;
      case ORM_VALUE_BOOLEAN:
        parameters->scalars[index].boolean =
            value->data.boolean_value != 0u ? 1 : 0;
        bind->buffer_type = MYSQL_TYPE_TINY;
        bind->buffer = &parameters->scalars[index].boolean;
        bind->buffer_length = sizeof(parameters->scalars[index].boolean);
        break;
      case ORM_VALUE_TEXT:
      case ORM_VALUE_BLOB:
        size = tstr_len(value->bytes);
        if (size > (size_t)ULONG_MAX) {
          orm_mysql_parameters_destroy(parameters);
          orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                        "MySQL parameter exceeds client length range");
          return ORM_STATUS_LIMIT_EXCEEDED;
        }
        parameters->lengths[index] = (unsigned long)size;
        bind->buffer_type =
            value->kind == ORM_VALUE_BLOB ? MYSQL_TYPE_BLOB
                                          : MYSQL_TYPE_STRING;
        bind->buffer = value->bytes;
        bind->buffer_length = (unsigned long)size;
        break;
      default:
        orm_mysql_parameters_destroy(parameters);
        orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                      "unknown MySQL parameter kind");
        return ORM_STATUS_INVALID_ARGUMENT;
    }
  }
  return ORM_STATUS_OK;
}

static orm_status_t orm_mysql_prepare(
    orm_mysql_backend_state *state,
    const orm_query_plan *plan,
    const orm_limits *limits,
    MYSQL_STMT **out_statement,
    uint64_t *affected_rows,
    orm_error_t *error) {
  orm_sql_query rendered;
  orm_mysql_parameters parameters;
  MYSQL_STMT *statement = NULL;
  orm_status_t status;

  if (out_statement != NULL)
    *out_statement = NULL;
  if (state == NULL || state->connection == NULL || plan == NULL ||
      limits == NULL || out_statement == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid MySQL query request");
    return ORM_STATUS_INVALID_ARGUMENT;
  }

  status = orm_mysql_sql_render(plan, limits, &rendered, error);
  if (status != ORM_STATUS_OK)
    return status;
  if (tstr_len(rendered.text) > (size_t)ULONG_MAX) {
    orm_sql_query_destroy(&rendered);
    orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                  "MySQL SQL text exceeds client length range");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }

  statement = mysql_stmt_init(state->connection);
  if (statement == NULL) {
    orm_sql_query_destroy(&rendered);
    return orm_mysql_connection_error(
        state->connection, "initialize MySQL prepared statement", error);
  }
  if (mysql_stmt_prepare(statement, rendered.text,
                         (unsigned long)tstr_len(rendered.text)) != 0) {
    status = orm_mysql_statement_error(statement, "prepare MySQL statement",
                                       error);
    (void)mysql_stmt_close(statement);
    orm_sql_query_destroy(&rendered);
    return status;
  }

  status = orm_mysql_parameters_init(&rendered, &parameters, error);
  if (status != ORM_STATUS_OK) {
    (void)mysql_stmt_close(statement);
    orm_sql_query_destroy(&rendered);
    return status;
  }
  if ((size_t)mysql_stmt_param_count(statement) != parameters.count) {
    orm_mysql_parameters_destroy(&parameters);
    (void)mysql_stmt_close(statement);
    orm_sql_query_destroy(&rendered);
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "MySQL placeholder count does not match bound parameters");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  if (parameters.count != 0u &&
      mysql_stmt_bind_param(statement, parameters.binds)) {
    status = orm_mysql_statement_error(
        statement, "bind MySQL statement parameters", error);
    orm_mysql_parameters_destroy(&parameters);
    (void)mysql_stmt_close(statement);
    orm_sql_query_destroy(&rendered);
    return status;
  }
  if (mysql_stmt_execute(statement) != 0) {
    status = orm_mysql_statement_error(statement, "execute MySQL statement",
                                       error);
    orm_mysql_parameters_destroy(&parameters);
    (void)mysql_stmt_close(statement);
    orm_sql_query_destroy(&rendered);
    return status;
  }
  if (affected_rows != NULL)
    *affected_rows = mysql_stmt_affected_rows(statement);

  orm_mysql_parameters_destroy(&parameters);
  orm_sql_query_destroy(&rendered);
  *out_statement = statement;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

static orm_status_t orm_mysql_open_impl(
    orm_mysql_backend_state *state,
    const orm_query_plan *plan,
    const orm_limits *limits,
    int allow_transaction,
    uint64_t *affected_rows,
    orm_row_cursor *out_cursor,
    orm_error_t *error) {
  MYSQL_STMT *statement = NULL;
  orm_mysql_cursor_config cursor_config;
  orm_status_t status;

  if (state == NULL || state->connection == NULL || out_cursor == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid MySQL cursor request");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(out_cursor, 0, sizeof(*out_cursor));
  if (!allow_transaction && state->transaction_active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "use the transaction handle while a transaction is active");
    return ORM_STATUS_INVALID_STATE;
  }
  status = orm_mysql_prepare(
      state, plan, limits, &statement, affected_rows, error);
  if (status != ORM_STATUS_OK)
    return status;

  cursor_config = (orm_mysql_cursor_config)ORM_MYSQL_CURSOR_CONFIG_INIT(
      limits->max_columns, limits->max_result_rows,
      limits->max_result_bytes, affected_rows);
  status = orm_mysql_cursor_from_statement(
      out_cursor, &statement, &cursor_config, error);
  if (status != ORM_STATUS_OK && statement != NULL)
    (void)mysql_stmt_close(statement);
  return status;
}

static orm_status_t orm_mysql_drain_command(
    orm_row_cursor *cursor, uint64_t *affected_rows,
    orm_error_t *error) {
  orm_status_t status = ORM_STATUS_OK;
  uint64_t columns = 0u;
  if (cursor == NULL || cursor->ops == NULL)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_ARGUMENT, "drain MySQL command", NULL);
  if (cursor->ops->column_count != NULL &&
      cursor->ops->column_count(cursor->context, &columns) != ORM_STATUS_OK)
    return orm_mysql_fail(
        error, ORM_STATUS_INTERNAL_ERROR, "inspect MySQL command result", NULL);

  for (;;) {
    cserde_reader row = {0};
    const orm_row_cursor_step step =
        cursor->ops->next(cursor->context, &row);
    if (step.kind == ORM_ROW_CURSOR_DONE)
      break;
    if (step.kind == ORM_ROW_CURSOR_ERROR) {
      status = step.status != ORM_STATUS_OK
                   ? step.status : ORM_STATUS_DATASTORE_ERROR;
      orm_error_set(error, status, step.message);
      break;
    }
    if (step.kind == ORM_ROW_CURSOR_ROW ||
        step.kind == ORM_ROW_CURSOR_ROW_AND_DONE) {
      status = ORM_STATUS_UNSUPPORTED;
      orm_error_set(error, status,
                    "MySQL row queries must be opened as a row Publisher");
      break;
    }
    status = ORM_STATUS_INTERNAL_ERROR;
    orm_error_set(error, status,
                  "MySQL cursor returned an invalid step");
    break;
  }
  cursor->ops->destroy(cursor->context);
  memset(cursor, 0, sizeof(*cursor));
  if (status == ORM_STATUS_OK && columns != 0u) {
    status = ORM_STATUS_UNSUPPORTED;
    orm_error_set(error, status,
                  "MySQL row queries must be opened as a row Publisher");
  }
  (void)affected_rows;
  return status;
}

static orm_status_t orm_mysql_execute_impl(
    orm_mysql_backend_state *state,
    const orm_query_plan *plan,
    const orm_limits *limits,
    int allow_transaction,
    uint64_t *affected_rows,
    orm_error_t *error) {
  orm_row_cursor cursor = {0};
  orm_status_t status = orm_mysql_open_impl(
      state, plan, limits, allow_transaction,
      affected_rows, &cursor, error);
  if (status != ORM_STATUS_OK)
    return status;
  return orm_mysql_drain_command(&cursor, affected_rows, error);
}

static orm_status_t orm_mysql_control(
    orm_mysql_backend_state *state,
    const char *sql,
    orm_error_t *error) {
  if (state == NULL || state->connection == NULL || sql == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid MySQL control request");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  if (mysql_real_query(
          state->connection, sql, (unsigned long)strlen(sql)) != 0)
    return orm_mysql_connection_error(
        state->connection, "execute MySQL control statement", error);
  if (mysql_field_count(state->connection) != 0u) {
    MYSQL_RES *result = mysql_store_result(state->connection);
    if (result != NULL)
      mysql_free_result(result);
  }
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

static int orm_mysql_identifier(vstr value) {
  size_t index;
  if (!orm_view_valid(value, false) || value.len > 128u)
    return 0;
  for (index = 0u; index < value.len; ++index) {
    const unsigned char next = (unsigned char)value.data[index];
    const int alpha =
        (next >= (unsigned char)'a' && next <= (unsigned char)'z') ||
        (next >= (unsigned char)'A' && next <= (unsigned char)'Z') ||
        next == (unsigned char)'_';
    if ((index == 0u && !alpha) ||
        (index != 0u && !alpha &&
         !(next >= (unsigned char)'0' && next <= (unsigned char)'9')))
      return 0;
  }
  return 1;
}

static void orm_mysql_backend_destroy(void *context) {
  orm_mysql_backend_state *state =
      (orm_mysql_backend_state *)context;
  if (state == NULL)
    return;
  if (state->connection != NULL)
    mysql_close(state->connection);
  free(state);
}

static orm_status_t orm_mysql_backend_open(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    orm_row_cursor *out_cursor, orm_error_t *error) {
  return orm_mysql_open_impl(
      (orm_mysql_backend_state *)context, plan, limits,
      0, NULL, out_cursor, error);
}

static orm_status_t orm_mysql_backend_execute(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    uint64_t *affected_rows, orm_error_t *error) {
  return orm_mysql_execute_impl(
      (orm_mysql_backend_state *)context, plan, limits,
      0, affected_rows, error);
}

static orm_status_t orm_mysql_transaction_open(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    orm_row_cursor *out_cursor, orm_error_t *error) {
  orm_mysql_transaction_state *transaction =
      (orm_mysql_transaction_state *)context;
  if (transaction == NULL || !transaction->active)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_STATE,
        "open MySQL transaction query", "transaction is no longer active");
  return orm_mysql_open_impl(
      transaction->owner, plan, limits, 1, NULL, out_cursor, error);
}

static orm_status_t orm_mysql_transaction_execute(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    uint64_t *affected_rows, orm_error_t *error) {
  orm_mysql_transaction_state *transaction =
      (orm_mysql_transaction_state *)context;
  if (transaction == NULL || !transaction->active)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_STATE,
        "execute MySQL transaction query", "transaction is no longer active");
  return orm_mysql_execute_impl(
      transaction->owner, plan, limits, 1, affected_rows, error);
}

static orm_status_t orm_mysql_transaction_finish(
    orm_mysql_transaction_state *transaction,
    int commit,
    orm_error_t *error) {
  bool failed;
  if (transaction == NULL || !transaction->active)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_STATE,
        "finish MySQL transaction", "transaction is no longer active");
  failed = commit ? mysql_commit(transaction->owner->connection)
                  : mysql_rollback(transaction->owner->connection);
  if (failed)
    return orm_mysql_connection_error(
        transaction->owner->connection,
        commit ? "commit MySQL transaction" : "rollback MySQL transaction",
        error);
  transaction->active = 0;
  transaction->owner->transaction_active = 0;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

static orm_status_t orm_mysql_transaction_commit(
    void *context, orm_error_t *error) {
  return orm_mysql_transaction_finish(
      (orm_mysql_transaction_state *)context, 1, error);
}

static orm_status_t orm_mysql_transaction_rollback(
    void *context, orm_error_t *error) {
  return orm_mysql_transaction_finish(
      (orm_mysql_transaction_state *)context, 0, error);
}

static orm_status_t orm_mysql_savepoint_sql(
    orm_mysql_transaction_state *transaction,
    const char *prefix, vstr name,
    orm_error_t *error) {
  char sql[256];
  int length;
  if (transaction == NULL || !transaction->active)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_STATE,
        "execute MySQL savepoint", "transaction is no longer active");
  if (!orm_mysql_identifier(name)) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid MySQL savepoint identifier");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  length = snprintf(sql, sizeof(sql), "%s `%.*s`", prefix,
                    (int)name.len, name.data);
  if (length <= 0 || (size_t)length >= sizeof(sql))
    return orm_mysql_fail(
        error, ORM_STATUS_LIMIT_EXCEEDED,
        "format MySQL savepoint", NULL);
  return orm_mysql_control(transaction->owner, sql, error);
}

static orm_status_t orm_mysql_transaction_savepoint(
    void *context, vstr name, orm_error_t *error) {
  return orm_mysql_savepoint_sql(
      (orm_mysql_transaction_state *)context, "savepoint", name, error);
}

static orm_status_t orm_mysql_transaction_rollback_to_savepoint(
    void *context, vstr name, orm_error_t *error) {
  return orm_mysql_savepoint_sql(
      (orm_mysql_transaction_state *)context, "rollback to savepoint",
      name, error);
}

static orm_status_t orm_mysql_transaction_release_savepoint(
    void *context, vstr name, orm_error_t *error) {
  return orm_mysql_savepoint_sql(
      (orm_mysql_transaction_state *)context, "release savepoint",
      name, error);
}

static void orm_mysql_transaction_destroy(void *context) {
  orm_mysql_transaction_state *transaction =
      (orm_mysql_transaction_state *)context;
  if (transaction == NULL)
    return;
  if (transaction->active) {
    (void)mysql_rollback(transaction->owner->connection);
    transaction->owner->transaction_active = 0;
  }
  free(transaction);
}

static const orm_transaction_backend_ops orm_mysql_transaction_ops = {
    sizeof(orm_transaction_backend_ops),
    ORM_TRANSACTION_BACKEND_OPS_ABI_VERSION,
    orm_mysql_transaction_destroy,
    orm_mysql_transaction_open,
    orm_mysql_transaction_execute,
    orm_mysql_transaction_commit,
    orm_mysql_transaction_rollback,
    orm_mysql_transaction_savepoint,
    orm_mysql_transaction_rollback_to_savepoint,
    orm_mysql_transaction_release_savepoint};

static orm_status_t orm_mysql_backend_begin_transaction(
    void *context, orm_isolation_t isolation,
    orm_transaction_backend *out_transaction,
    orm_error_t *error) {
  orm_mysql_backend_state *state =
      (orm_mysql_backend_state *)context;
  orm_mysql_transaction_state *transaction;
  const char *isolation_sql;
  orm_status_t status;

  if (state == NULL || state->connection == NULL ||
      out_transaction == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid MySQL transaction request");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(out_transaction, 0, sizeof(*out_transaction));
  if (state->transaction_active) {
    orm_error_set(error, ORM_STATUS_BUSY,
                  "MySQL transaction is already active");
    return ORM_STATUS_BUSY;
  }

  switch (isolation) {
    case ORM_ISOLATION_READ_UNCOMMITTED:
      isolation_sql =
          "set transaction isolation level read uncommitted";
      break;
    case ORM_ISOLATION_READ_COMMITTED:
      isolation_sql =
          "set transaction isolation level read committed";
      break;
    case ORM_ISOLATION_REPEATABLE_READ:
      isolation_sql =
          "set transaction isolation level repeatable read";
      break;
    case ORM_ISOLATION_SERIALIZABLE:
      isolation_sql =
          "set transaction isolation level serializable";
      break;
    case ORM_ISOLATION_SNAPSHOT:
    default:
      orm_error_set(error, ORM_STATUS_UNSUPPORTED,
                    "MySQL does not support requested isolation mode");
      return ORM_STATUS_UNSUPPORTED;
  }

  status = orm_mysql_control(state, isolation_sql, error);
  if (status == ORM_STATUS_OK)
    status = orm_mysql_control(state, "start transaction", error);
  if (status != ORM_STATUS_OK)
    return status;

  transaction = (orm_mysql_transaction_state *)calloc(
      1u, sizeof(*transaction));
  if (transaction == NULL) {
    (void)mysql_rollback(state->connection);
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "allocate MySQL transaction");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  transaction->owner = state;
  transaction->active = 1;
  state->transaction_active = 1;
  out_transaction->ops = &orm_mysql_transaction_ops;
  out_transaction->context = transaction;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

static const orm_backend_ops orm_mysql_backend_ops = {
    sizeof(orm_backend_ops), ORM_BACKEND_OPS_ABI_VERSION,
    orm_mysql_backend_destroy,
    orm_mysql_backend_open,
    orm_mysql_backend_execute,
    orm_mysql_backend_begin_transaction};

static int orm_mysql_option_known(vstr key) {
  return orm_view_equal_cstr(key, "host") ||
         orm_view_equal_cstr(key, "user") ||
         orm_view_equal_cstr(key, "password") ||
         orm_view_equal_cstr(key, "database") ||
         orm_view_equal_cstr(key, "port") ||
         orm_view_equal_cstr(key, "unix_socket") ||
         orm_view_equal_cstr(key, "charset") ||
         orm_view_equal_cstr(key, "connect_timeout");
}

static orm_status_t orm_mysql_copy_option(
    const orm_option_t *option, int allow_empty,
    char **out, orm_error_t *error) {
  char *copy;
  if (out == NULL || option == NULL ||
      !orm_view_valid(option->value, allow_empty) ||
      option->value.len > ORM_MYSQL_OPTION_VALUE_MAX ||
      memchr(option->value.data, '\0', option->value.len) != NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid MySQL connection option");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  copy = (char *)malloc(option->value.len + 1u);
  if (copy == NULL) {
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "copy MySQL connection option");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  if (option->value.len != 0u)
    memcpy(copy, option->value.data, option->value.len);
  copy[option->value.len] = '\0';
  *out = copy;
  return ORM_STATUS_OK;
}

static orm_status_t orm_mysql_parse_u32(
    const orm_option_t *option, unsigned int maximum,
    unsigned int *out, orm_error_t *error) {
  uint64_t value = 0u;
  size_t index;
  if (option == NULL || out == NULL ||
      !orm_view_valid(option->value, false)) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid MySQL numeric option");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  for (index = 0u; index < option->value.len; ++index) {
    const unsigned char next =
        (unsigned char)option->value.data[index];
    const uint64_t digit =
        next >= (unsigned char)'0' && next <= (unsigned char)'9'
            ? (uint64_t)(next - (unsigned char)'0')
            : UINT64_MAX;
    if (digit == UINT64_MAX) {
      orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                    "MySQL numeric option is not decimal");
      return ORM_STATUS_INVALID_ARGUMENT;
    }
    if (value > ((uint64_t)maximum - digit) / UINT64_C(10)) {
      orm_error_set(error, ORM_STATUS_OUT_OF_RANGE,
                    "MySQL numeric option exceeds supported range");
      return ORM_STATUS_OUT_OF_RANGE;
    }
    value = value * UINT64_C(10) + digit;
  }
  *out = (unsigned int)value;
  return ORM_STATUS_OK;
}

static void orm_mysql_connect_options_destroy(
    orm_mysql_connect_options *options) {
  if (options == NULL)
    return;
  free(options->host);
  free(options->user);
  free(options->password);
  free(options->database);
  free(options->unix_socket);
  free(options->charset);
  memset(options, 0, sizeof(*options));
}

static orm_status_t orm_mysql_connect_options_init(
    const orm_config_t *config,
    orm_mysql_connect_options *options,
    orm_error_t *error) {
  uint32_t index;
  uint32_t seen = 0u;
  if (config == NULL || options == NULL ||
      config->struct_size < sizeof(*config) ||
      config->abi_version != ORM_C_ABI_VERSION ||
      (config->option_count != 0u && config->options == NULL) ||
      !orm_view_equal_cstr(config->driver, "mysql")) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid MySQL connection config");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(options, 0, sizeof(*options));

  for (index = 0u; index < config->option_count; ++index) {
    const orm_option_t *option = &config->options[index];
    uint32_t bit;
    orm_status_t status = ORM_STATUS_OK;
    if (!orm_view_valid(option->keyword, false) ||
        !orm_mysql_option_known(option->keyword)) {
      orm_mysql_connect_options_destroy(options);
      orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                    "unknown MySQL connection option");
      return ORM_STATUS_INVALID_ARGUMENT;
    }
    if (orm_view_equal_cstr(option->keyword, "host"))
      bit = UINT32_C(1) << 0;
    else if (orm_view_equal_cstr(option->keyword, "user"))
      bit = UINT32_C(1) << 1;
    else if (orm_view_equal_cstr(option->keyword, "password"))
      bit = UINT32_C(1) << 2;
    else if (orm_view_equal_cstr(option->keyword, "database"))
      bit = UINT32_C(1) << 3;
    else if (orm_view_equal_cstr(option->keyword, "port"))
      bit = UINT32_C(1) << 4;
    else if (orm_view_equal_cstr(option->keyword, "unix_socket"))
      bit = UINT32_C(1) << 5;
    else if (orm_view_equal_cstr(option->keyword, "charset"))
      bit = UINT32_C(1) << 6;
    else
      bit = UINT32_C(1) << 7;
    if ((seen & bit) != 0u) {
      orm_mysql_connect_options_destroy(options);
      orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                    "duplicate MySQL connection option");
      return ORM_STATUS_INVALID_ARGUMENT;
    }
    seen |= bit;

    if (bit == (UINT32_C(1) << 0))
      status = orm_mysql_copy_option(option, 0, &options->host, error);
    else if (bit == (UINT32_C(1) << 1))
      status = orm_mysql_copy_option(option, 0, &options->user, error);
    else if (bit == (UINT32_C(1) << 2))
      status = orm_mysql_copy_option(option, 1, &options->password, error);
    else if (bit == (UINT32_C(1) << 3))
      status = orm_mysql_copy_option(option, 0, &options->database, error);
    else if (bit == (UINT32_C(1) << 4))
      status = orm_mysql_parse_u32(option, 65535u, &options->port, error);
    else if (bit == (UINT32_C(1) << 5))
      status = orm_mysql_copy_option(
          option, 0, &options->unix_socket, error);
    else if (bit == (UINT32_C(1) << 6))
      status = orm_mysql_copy_option(option, 0, &options->charset, error);
    else
      status = orm_mysql_parse_u32(
          option, UINT_MAX, &options->connect_timeout, error);
    if (status != ORM_STATUS_OK) {
      orm_mysql_connect_options_destroy(options);
      return status;
    }
  }

  if (options->host == NULL && options->unix_socket == NULL) {
    options->host = (char *)malloc(sizeof("127.0.0.1"));
    if (options->host == NULL) {
      orm_mysql_connect_options_destroy(options);
      orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                    "allocate default MySQL host");
      return ORM_STATUS_OUT_OF_MEMORY;
    }
    memcpy(options->host, "127.0.0.1", sizeof("127.0.0.1"));
  }
  return ORM_STATUS_OK;
}

orm_status_t orm_mysql_backend_create(
    const orm_config_t *config,
    const orm_limits *limits,
    orm_backend *out_backend,
    orm_error_t *error) {
  orm_mysql_connect_options options;
  orm_mysql_backend_state *state = NULL;
  MYSQL *connection = NULL;
  bool report_truncation = true;
  orm_status_t status;

  (void)limits;
  if (out_backend == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "missing MySQL backend output");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(out_backend, 0, sizeof(*out_backend));
  status = orm_mysql_connect_options_init(config, &options, error);
  if (status != ORM_STATUS_OK)
    return status;

  connection = mysql_init(NULL);
  if (connection == NULL) {
    orm_mysql_connect_options_destroy(&options);
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "initialize MySQL client connection");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  if (mysql_options(connection, MYSQL_REPORT_DATA_TRUNCATION,
                    &report_truncation) ||
      (options.connect_timeout != 0u &&
       mysql_options(connection, MYSQL_OPT_CONNECT_TIMEOUT,
                     &options.connect_timeout)) ||
      (options.charset != NULL &&
       mysql_options(connection, MYSQL_SET_CHARSET_NAME,
                     options.charset))) {
    status = orm_mysql_connection_error(
        connection, "configure MySQL connection", error);
    mysql_close(connection);
    orm_mysql_connect_options_destroy(&options);
    return status;
  }

  if (mysql_real_connect(
          connection, options.host, options.user, options.password,
          options.database, options.port, options.unix_socket,
          0u) == NULL) {
    status = orm_mysql_connection_error(
        connection, "connect MySQL", error);
    mysql_close(connection);
    orm_mysql_connect_options_destroy(&options);
    return status;
  }
  orm_mysql_connect_options_destroy(&options);

  state = (orm_mysql_backend_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    mysql_close(connection);
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "allocate MySQL backend");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  state->connection = connection;
  out_backend->ops = &orm_mysql_backend_ops;
  out_backend->context = state;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}
