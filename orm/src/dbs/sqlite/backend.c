#include "orm_internal.h"
#include "orm_sql_render.h"
#include "orm_sqlite_cursor.h"

#include <sqlite3.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  ORM_SQLITE_FILENAME_MAX_BYTES = 4096u,
  ORM_SQLITE_DEFAULT_BUSY_TIMEOUT_MS = 5000u,
  ORM_SQLITE_SAVEPOINT_MAX_BYTES = 63u
};

typedef enum orm_sqlite_open_mode {
  ORM_SQLITE_READ_ONLY = 0,
  ORM_SQLITE_READ_WRITE,
  ORM_SQLITE_READ_WRITE_CREATE
} orm_sqlite_open_mode;

typedef struct orm_sqlite_backend_state {
  sqlite3 *database;
  int transaction_active;
} orm_sqlite_backend_state;

typedef struct orm_sqlite_transaction_state {
  orm_sqlite_backend_state *owner;
  int active;
} orm_sqlite_transaction_state;

static orm_status_t orm_sqlite_status(int code, orm_status_t fallback) {
  switch (code & 0xff) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED: return ORM_STATUS_BUSY;
    case SQLITE_NOMEM: return ORM_STATUS_OUT_OF_MEMORY;
    case SQLITE_TOOBIG: return ORM_STATUS_LIMIT_EXCEEDED;
    case SQLITE_RANGE: return ORM_STATUS_OUT_OF_RANGE;
    case SQLITE_READONLY: return ORM_STATUS_INVALID_STATE;
    default: return fallback;
  }
}

static orm_status_t orm_sqlite_fail(sqlite3 *database, int code,
                                    orm_status_t fallback,
                                    const char *operation,
                                    orm_error_t *error) {
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  const orm_status_t status = orm_sqlite_status(code, fallback);
  const char *detail = database != NULL ? sqlite3_errmsg(database)
                                        : sqlite3_errstr(code);
  (void)snprintf(message, sizeof(message), "%s: %s", operation,
                 detail != NULL ? detail : orm_status_message(status));
  orm_error_set(error, status, message);
  return status;
}

static int orm_sqlite_tail_empty(const char *begin, const char *end) {
  const char *current;
  for (current = begin; current != end; ++current) {
    if (isspace((unsigned char)*current) == 0)
      return 0;
  }
  return 1;
}

static int orm_sqlite_identifier(vstr value) {
  size_t index;
  if (!orm_view_valid(value, false) ||
      value.len > ORM_SQLITE_SAVEPOINT_MAX_BYTES)
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

static orm_status_t orm_sqlite_bind(sqlite3 *database,
                                    sqlite3_stmt *statement, int index,
                                    const orm_owned_value *value,
                                    orm_error_t *error) {
  const size_t size = tstr_len(value->bytes);
  int code;
  switch (value->kind) {
    case ORM_VALUE_NULL:
      code = sqlite3_bind_null(statement, index);
      break;
    case ORM_VALUE_INT64:
      code = sqlite3_bind_int64(statement, index,
                                (sqlite3_int64)value->data.int64_value);
      break;
    case ORM_VALUE_UINT64:
      if (value->data.uint64_value > (uint64_t)INT64_MAX) {
        orm_error_set(error, ORM_STATUS_OUT_OF_RANGE,
                      "unsigned parameter exceeds SQLite int64 range");
        return ORM_STATUS_OUT_OF_RANGE;
      }
      code = sqlite3_bind_int64(statement, index,
                                (sqlite3_int64)value->data.uint64_value);
      break;
    case ORM_VALUE_DOUBLE:
      code = sqlite3_bind_double(statement, index, value->data.double_value);
      break;
    case ORM_VALUE_BOOLEAN:
      code = sqlite3_bind_int(statement, index,
                              value->data.boolean_value != 0u ? 1 : 0);
      break;
    case ORM_VALUE_TEXT:
      if (size > (size_t)INT_MAX) {
        orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                      "SQLite text parameter exceeds int range");
        return ORM_STATUS_LIMIT_EXCEEDED;
      }
      code = sqlite3_bind_text(statement, index, value->bytes, (int)size,
                               SQLITE_TRANSIENT);
      break;
    case ORM_VALUE_BLOB:
      if (size > (size_t)INT_MAX) {
        orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                      "SQLite blob parameter exceeds int range");
        return ORM_STATUS_LIMIT_EXCEEDED;
      }
      code = sqlite3_bind_blob(statement, index, value->bytes, (int)size,
                               SQLITE_TRANSIENT);
      break;
    default:
      orm_error_set(error, ORM_STATUS_INTERNAL_ERROR,
                    "unknown SQLite parameter kind");
      return ORM_STATUS_INTERNAL_ERROR;
  }
  return code == SQLITE_OK
             ? ORM_STATUS_OK
             : orm_sqlite_fail(database, code, ORM_STATUS_SQL_ERROR,
                               "bind SQLite parameter", error);
}

static orm_status_t orm_sqlite_prepare(
    orm_sqlite_backend_state *state, const orm_query_plan *plan,
    const orm_limits *limits, sqlite3_stmt **out_statement,
    orm_error_t *error) {
  orm_sql_query rendered;
  sqlite3_stmt *statement = NULL;
  const char *tail = NULL;
  orm_status_t status;
  size_t index;
  int code;
  *out_statement = NULL;
  status = orm_sql_render(plan, limits, ORM_SQL_SQLITE, &rendered, error);
  if (status != ORM_STATUS_OK)
    return status;
  if (tstr_len(rendered.text) > (size_t)INT_MAX) {
    orm_sql_query_destroy(&rendered);
    orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                  "SQL query exceeds SQLite int range");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  code = sqlite3_prepare_v2(state->database, rendered.text,
                            (int)tstr_len(rendered.text), &statement, &tail);
  if (code != SQLITE_OK) {
    status = orm_sqlite_fail(state->database, code, ORM_STATUS_SQL_ERROR,
                             "prepare SQLite statement", error);
    goto cleanup;
  }
  if (statement == NULL || tail == NULL) {
    status = statement == NULL ? ORM_STATUS_SQL_ERROR
                               : ORM_STATUS_INTERNAL_ERROR;
    orm_error_set(error, status, statement == NULL
                                     ? "SQLite query has no statement"
                                     : "SQLite returned no statement tail");
    goto cleanup;
  }
  if (tail != rendered.text + tstr_len(rendered.text) &&
      !orm_sqlite_tail_empty(tail,
                             rendered.text + tstr_len(rendered.text))) {
    status = ORM_STATUS_SQL_ERROR;
    orm_error_set(error, status,
                  "SQLite query contains more than one statement");
    goto cleanup;
  }
  if (sqlite3_bind_parameter_count(statement) !=
      (int)vec_size(&rendered.parameters)) {
    status = ORM_STATUS_INVALID_ARGUMENT;
    orm_error_set(error, status,
                  "SQLite parameter count does not match SQL placeholders");
    goto cleanup;
  }
  for (index = 0u; index < vec_size(&rendered.parameters); ++index) {
    const orm_owned_value *value = *(const orm_owned_value *const *)
        vec_at_const(&rendered.parameters, index);
    status = orm_sqlite_bind(state->database, statement, (int)index + 1,
                             value, error);
    if (status != ORM_STATUS_OK)
      goto cleanup;
  }
  *out_statement = statement;
  statement = NULL;
  status = ORM_STATUS_OK;

cleanup:
  if (statement != NULL)
    (void)sqlite3_finalize(statement);
  orm_sql_query_destroy(&rendered);
  return status;
}

static orm_status_t orm_sqlite_open_impl(
    orm_sqlite_backend_state *state, const orm_query_plan *plan,
    const orm_limits *limits, int allow_transaction,
    orm_row_cursor *out_cursor, orm_error_t *error) {
  sqlite3_stmt *statement = NULL;
  orm_sqlite_cursor_config cursor_config;
  orm_status_t status;
  int columns;
  if (!allow_transaction && state->transaction_active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "use the SQLite transaction handle while active");
    return ORM_STATUS_INVALID_STATE;
  }
  status = orm_sqlite_prepare(state, plan, limits, &statement, error);
  if (status != ORM_STATUS_OK)
    return status;
  columns = sqlite3_column_count(statement);
  if (columns <= 0) {
    (void)sqlite3_finalize(statement);
    orm_error_set(error, ORM_STATUS_UNSUPPORTED,
                  "SQLite command must use a command Source");
    return ORM_STATUS_UNSUPPORTED;
  }
  if ((uint64_t)columns > (uint64_t)limits->max_columns) {
    (void)sqlite3_finalize(statement);
    orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                  "SQLite result exceeds max_columns");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  cursor_config = (orm_sqlite_cursor_config)ORM_SQLITE_CURSOR_CONFIG_INIT(
      limits->max_result_rows, limits->max_result_bytes);
  status = orm_sqlite_cursor_from_statement(out_cursor, &statement,
                                            &cursor_config, error);
  if (statement != NULL)
    (void)sqlite3_finalize(statement);
  return status;
}

static orm_status_t orm_sqlite_execute_impl(
    orm_sqlite_backend_state *state, const orm_query_plan *plan,
    const orm_limits *limits, int allow_transaction,
    uint64_t *affected_rows, orm_error_t *error) {
  sqlite3_stmt *statement = NULL;
  sqlite3_int64 before;
  sqlite3_int64 after;
  orm_status_t status;
  int code;
  if (!allow_transaction && state->transaction_active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "use the SQLite transaction handle while active");
    return ORM_STATUS_INVALID_STATE;
  }
  status = orm_sqlite_prepare(state, plan, limits, &statement, error);
  if (status != ORM_STATUS_OK)
    return status;
  if (sqlite3_column_count(statement) != 0) {
    (void)sqlite3_finalize(statement);
    orm_error_set(error, ORM_STATUS_UNSUPPORTED,
                  "SQLite row query must use a row Source");
    return ORM_STATUS_UNSUPPORTED;
  }
  before = sqlite3_total_changes64(state->database);
  code = sqlite3_step(statement);
  if (code != SQLITE_DONE)
    status = orm_sqlite_fail(state->database, code, ORM_STATUS_SQL_ERROR,
                             "execute SQLite command", error);
  after = sqlite3_total_changes64(state->database);
  (void)sqlite3_finalize(statement);
  if (status != ORM_STATUS_OK)
    return status;
  if (after < before) {
    orm_error_set(error, ORM_STATUS_INTERNAL_ERROR,
                  "SQLite total change counter moved backwards");
    return ORM_STATUS_INTERNAL_ERROR;
  }
  if (affected_rows != NULL)
    *affected_rows = plan->kind == ORM_QUERY_RAW
                         ? (uint64_t)(after - before)
                         : (uint64_t)sqlite3_changes64(state->database);
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

static orm_status_t orm_sqlite_control(orm_sqlite_backend_state *state,
                                       const char *sql,
                                       orm_error_t *error) {
  char *detail = NULL;
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  const int code = sqlite3_exec(state->database, sql, NULL, NULL, &detail);
  if (code == SQLITE_OK) {
    orm_error_set(error, ORM_STATUS_OK, NULL);
    return ORM_STATUS_OK;
  }
  (void)snprintf(message, sizeof(message),
                 "execute SQLite transaction control: %s",
                 detail != NULL ? detail : sqlite3_errmsg(state->database));
  sqlite3_free(detail);
  orm_error_set(error, orm_sqlite_status(code, ORM_STATUS_SQL_ERROR), message);
  return orm_sqlite_status(code, ORM_STATUS_SQL_ERROR);
}

static void orm_sqlite_backend_destroy(void *context) {
  orm_sqlite_backend_state *state = (orm_sqlite_backend_state *)context;
  if (state == NULL)
    return;
  if (state->database != NULL)
    (void)sqlite3_close_v2(state->database);
  free(state);
}

static orm_status_t orm_sqlite_backend_open(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    orm_row_cursor *out_cursor, orm_error_t *error) {
  return orm_sqlite_open_impl((orm_sqlite_backend_state *)context, plan,
                              limits, 0, out_cursor, error);
}

static orm_status_t orm_sqlite_backend_execute(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    uint64_t *affected_rows, orm_error_t *error) {
  return orm_sqlite_execute_impl((orm_sqlite_backend_state *)context, plan,
                                 limits, 0, affected_rows, error);
}

static void orm_sqlite_transaction_sync(
    orm_sqlite_transaction_state *transaction) {
  if (transaction->active &&
      sqlite3_get_autocommit(transaction->owner->database) != 0) {
    transaction->active = 0;
    transaction->owner->transaction_active = 0;
  }
}

static orm_status_t orm_sqlite_transaction_open(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    orm_row_cursor *out_cursor, orm_error_t *error) {
  orm_sqlite_transaction_state *transaction =
      (orm_sqlite_transaction_state *)context;
  if (transaction == NULL || !transaction->active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "SQLite transaction is no longer active");
    return ORM_STATUS_INVALID_STATE;
  }
  return orm_sqlite_open_impl(transaction->owner, plan, limits, 1,
                              out_cursor, error);
}

static orm_status_t orm_sqlite_transaction_execute(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    uint64_t *affected_rows, orm_error_t *error) {
  orm_sqlite_transaction_state *transaction =
      (orm_sqlite_transaction_state *)context;
  orm_status_t status;
  if (transaction == NULL || !transaction->active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "SQLite transaction is no longer active");
    return ORM_STATUS_INVALID_STATE;
  }
  status = orm_sqlite_execute_impl(transaction->owner, plan, limits, 1,
                                   affected_rows, error);
  if (status != ORM_STATUS_OK)
    orm_sqlite_transaction_sync(transaction);
  return status;
}

static orm_status_t orm_sqlite_transaction_finish(
    orm_sqlite_transaction_state *transaction, const char *sql,
    orm_error_t *error) {
  orm_status_t status;
  if (transaction == NULL || !transaction->active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "SQLite transaction is no longer active");
    return ORM_STATUS_INVALID_STATE;
  }
  status = orm_sqlite_control(transaction->owner, sql, error);
  if (status == ORM_STATUS_OK) {
    transaction->active = 0;
    transaction->owner->transaction_active = 0;
  } else {
    orm_sqlite_transaction_sync(transaction);
  }
  return status;
}

static orm_status_t orm_sqlite_transaction_commit(void *context,
                                                  orm_error_t *error) {
  return orm_sqlite_transaction_finish(
      (orm_sqlite_transaction_state *)context, "commit", error);
}

static orm_status_t orm_sqlite_transaction_rollback(void *context,
                                                    orm_error_t *error) {
  return orm_sqlite_transaction_finish(
      (orm_sqlite_transaction_state *)context, "rollback", error);
}

static orm_status_t orm_sqlite_savepoint(
    orm_sqlite_transaction_state *transaction, const char *prefix,
    vstr name, orm_error_t *error) {
  char command[192];
  int length;
  orm_status_t status;
  if (transaction == NULL || !transaction->active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "SQLite transaction is no longer active");
    return ORM_STATUS_INVALID_STATE;
  }
  if (!orm_sqlite_identifier(name)) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid SQLite savepoint identifier");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  length = snprintf(command, sizeof(command), "%s \"%.*s\"", prefix,
                    (int)name.len, name.data);
  if (length <= 0 || (size_t)length >= sizeof(command)) {
    orm_error_set(error, ORM_STATUS_LIMIT_EXCEEDED,
                  "SQLite savepoint command is too long");
    return ORM_STATUS_LIMIT_EXCEEDED;
  }
  status = orm_sqlite_control(transaction->owner, command, error);
  if (status != ORM_STATUS_OK)
    orm_sqlite_transaction_sync(transaction);
  return status;
}

static orm_status_t orm_sqlite_transaction_savepoint(void *context,
                                                     vstr name,
                                                     orm_error_t *error) {
  return orm_sqlite_savepoint((orm_sqlite_transaction_state *)context,
                              "savepoint", name, error);
}

static orm_status_t orm_sqlite_transaction_rollback_to(
    void *context, vstr name, orm_error_t *error) {
  return orm_sqlite_savepoint((orm_sqlite_transaction_state *)context,
                              "rollback to savepoint", name, error);
}

static orm_status_t orm_sqlite_transaction_release(void *context,
                                                   vstr name,
                                                   orm_error_t *error) {
  return orm_sqlite_savepoint((orm_sqlite_transaction_state *)context,
                              "release savepoint", name, error);
}

static void orm_sqlite_transaction_destroy(void *context) {
  orm_sqlite_transaction_state *transaction =
      (orm_sqlite_transaction_state *)context;
  if (transaction == NULL)
    return;
  if (transaction->active) {
    orm_error_t ignored;
    orm_error_init(&ignored);
    (void)orm_sqlite_control(transaction->owner, "rollback", &ignored);
    transaction->owner->transaction_active = 0;
  }
  free(transaction);
}

static const orm_transaction_backend_ops orm_sqlite_transaction_ops = {
    sizeof(orm_transaction_backend_ops),
    ORM_TRANSACTION_BACKEND_OPS_ABI_VERSION,
    orm_sqlite_transaction_destroy,
    orm_sqlite_transaction_open,
    orm_sqlite_transaction_execute,
    orm_sqlite_transaction_commit,
    orm_sqlite_transaction_rollback,
    orm_sqlite_transaction_savepoint,
    orm_sqlite_transaction_rollback_to,
    orm_sqlite_transaction_release};

static orm_status_t orm_sqlite_backend_begin(
    void *context, orm_isolation_t isolation,
    orm_transaction_backend *out_transaction, orm_error_t *error) {
  orm_sqlite_backend_state *state = (orm_sqlite_backend_state *)context;
  orm_sqlite_transaction_state *transaction;
  orm_status_t status;
  if (state == NULL || out_transaction == NULL ||
      isolation < ORM_ISOLATION_READ_UNCOMMITTED ||
      isolation > ORM_ISOLATION_SERIALIZABLE) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid SQLite transaction request");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(out_transaction, 0, sizeof(*out_transaction));
  if (state->transaction_active) {
    orm_error_set(error, ORM_STATUS_INVALID_STATE,
                  "SQLite connection already has an active transaction");
    return ORM_STATUS_INVALID_STATE;
  }
  status = orm_sqlite_control(state, "begin", error);
  if (status != ORM_STATUS_OK)
    return status;
  transaction = (orm_sqlite_transaction_state *)calloc(1u,
                                                        sizeof(*transaction));
  if (transaction == NULL) {
    orm_error_t ignored;
    orm_error_init(&ignored);
    (void)orm_sqlite_control(state, "rollback", &ignored);
    orm_error_set(error, ORM_STATUS_OUT_OF_MEMORY,
                  "allocate SQLite transaction");
    return ORM_STATUS_OUT_OF_MEMORY;
  }
  transaction->owner = state;
  transaction->active = 1;
  state->transaction_active = 1;
  out_transaction->ops = &orm_sqlite_transaction_ops;
  out_transaction->context = transaction;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

static const orm_backend_ops orm_sqlite_backend_ops = {
    sizeof(orm_backend_ops), ORM_BACKEND_OPS_ABI_VERSION,
    orm_sqlite_backend_destroy, orm_sqlite_backend_open,
    orm_sqlite_backend_execute, orm_sqlite_backend_begin};

static orm_status_t orm_sqlite_parse_u32(vstr value, uint32_t *output) {
  char buffer[32];
  char *end = NULL;
  unsigned long parsed;
  if (!orm_view_valid(value, false) || value.len >= sizeof(buffer))
    return ORM_STATUS_INVALID_ARGUMENT;
  memcpy(buffer, value.data, value.len);
  buffer[value.len] = '\0';
  errno = 0;
  parsed = strtoul(buffer, &end, 10);
  if (errno != 0 || end != buffer + value.len || parsed > (unsigned long)INT_MAX)
    return ORM_STATUS_INVALID_ARGUMENT;
  *output = (uint32_t)parsed;
  return ORM_STATUS_OK;
}

static orm_status_t orm_sqlite_lower_limit(sqlite3 *database, int category,
                                           uint64_t requested,
                                           const char *role,
                                           orm_error_t *error) {
  char message[ORM_C_ERROR_MESSAGE_CAPACITY];
  int actual;
  if (requested == 0u || requested > (uint64_t)INT_MAX) {
    (void)snprintf(message, sizeof(message), "%s must be in [1, INT_MAX]",
                   role);
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, message);
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  (void)sqlite3_limit(database, category, (int)requested);
  actual = sqlite3_limit(database, category, -1);
  if (actual < (int)requested) {
    (void)snprintf(message, sizeof(message),
                   "%s exceeds this SQLite build's hard limit", role);
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT, message);
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  return ORM_STATUS_OK;
}

orm_status_t orm_sqlite_backend_create(const orm_config_t *config,
                                       const orm_limits *limits,
                                       orm_backend *out_backend,
                                       orm_error_t *error) {
  tstr filename = NULL;
  orm_sqlite_open_mode mode = ORM_SQLITE_READ_WRITE_CREATE;
  uint32_t timeout = ORM_SQLITE_DEFAULT_BUSY_TIMEOUT_MS;
  int has_filename = 0;
  uint32_t index;
  int flags = SQLITE_OPEN_NOMUTEX;
  orm_sqlite_backend_state *state = NULL;
  orm_status_t status = ORM_STATUS_OK;
  int code;
  if (config == NULL || limits == NULL || out_backend == NULL) {
    orm_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "invalid SQLite backend request");
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memset(out_backend, 0, sizeof(*out_backend));
  for (index = 0u; index < config->option_count; ++index) {
    const orm_option_t *option = &config->options[index];
    uint32_t previous;
    for (previous = 0u; previous < index; ++previous) {
      const vstr prior = config->options[previous].keyword;
      if (option->keyword.len == prior.len &&
          (prior.len == 0u ||
           memcmp(option->keyword.data, prior.data, prior.len) == 0)) {
        status = ORM_STATUS_INVALID_ARGUMENT;
        orm_error_set(error, status, "duplicate SQLite connection option");
        goto cleanup;
      }
    }
    if (orm_view_equal_cstr(option->keyword, "filename")) {
      if (!orm_view_valid(option->value, false) ||
          option->value.len > ORM_SQLITE_FILENAME_MAX_BYTES ||
          memchr(option->value.data, '\0', option->value.len) != NULL) {
        status = option->value.len > ORM_SQLITE_FILENAME_MAX_BYTES
                     ? ORM_STATUS_LIMIT_EXCEEDED
                     : ORM_STATUS_INVALID_ARGUMENT;
        orm_error_set(error, status, "invalid SQLite filename option");
        goto cleanup;
      }
      tstr_freep(&filename);
      filename = tstr_from_v(option->value);
      has_filename = 1;
      if (filename == NULL) {
        status = ORM_STATUS_OUT_OF_MEMORY;
        orm_error_set(error, status, "copy SQLite filename");
        goto cleanup;
      }
    } else if (orm_view_equal_cstr(option->keyword, "open_mode")) {
      if (orm_view_equal_cstr(option->value, "read_only"))
        mode = ORM_SQLITE_READ_ONLY;
      else if (orm_view_equal_cstr(option->value, "read_write"))
        mode = ORM_SQLITE_READ_WRITE;
      else if (orm_view_equal_cstr(option->value, "read_write_create"))
        mode = ORM_SQLITE_READ_WRITE_CREATE;
      else {
        status = ORM_STATUS_INVALID_ARGUMENT;
        orm_error_set(error, status, "invalid SQLite open_mode option");
        goto cleanup;
      }
    } else if (orm_view_equal_cstr(option->keyword, "busy_timeout_ms")) {
      status = orm_sqlite_parse_u32(option->value, &timeout);
      if (status != ORM_STATUS_OK) {
        orm_error_set(error, status,
                      "busy_timeout_ms must be in [0, INT_MAX]");
        goto cleanup;
      }
    } else {
      status = ORM_STATUS_INVALID_ARGUMENT;
      orm_error_set(error, status, "unknown SQLite connection option");
      goto cleanup;
    }
  }
  if (!has_filename) {
    status = ORM_STATUS_INVALID_ARGUMENT;
    orm_error_set(error, status, "SQLite filename option is required");
    goto cleanup;
  }
  flags |= mode == ORM_SQLITE_READ_ONLY
               ? SQLITE_OPEN_READONLY
               : (mode == ORM_SQLITE_READ_WRITE
                      ? SQLITE_OPEN_READWRITE
                      : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
  state = (orm_sqlite_backend_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    status = ORM_STATUS_OUT_OF_MEMORY;
    orm_error_set(error, status, "allocate SQLite backend");
    goto cleanup;
  }
  code = sqlite3_open_v2(filename, &state->database, flags, NULL);
  if (code != SQLITE_OK) {
    status = orm_sqlite_fail(state->database, code,
                             ORM_STATUS_CONNECTION_ERROR,
                             "open SQLite database", error);
    goto cleanup;
  }
  (void)sqlite3_extended_result_codes(state->database, 1);
  code = sqlite3_busy_timeout(state->database, (int)timeout);
  if (code != SQLITE_OK) {
    status = orm_sqlite_fail(state->database, code,
                             ORM_STATUS_CONNECTION_ERROR,
                             "configure SQLite busy timeout", error);
    goto cleanup;
  }
  status = orm_sqlite_lower_limit(state->database, SQLITE_LIMIT_SQL_LENGTH,
                                  limits->max_query_bytes,
                                  "max_query_bytes", error);
  if (status == ORM_STATUS_OK)
    status = orm_sqlite_lower_limit(state->database,
                                    SQLITE_LIMIT_VARIABLE_NUMBER,
                                    limits->max_parameters,
                                    "max_parameters", error);
  if (status == ORM_STATUS_OK)
    status = orm_sqlite_lower_limit(state->database, SQLITE_LIMIT_COLUMN,
                                    limits->max_columns, "max_columns", error);
  if (status == ORM_STATUS_OK)
    status = orm_sqlite_lower_limit(
        state->database, SQLITE_LIMIT_LENGTH,
        limits->max_parameter_bytes > limits->max_result_bytes
            ? limits->max_parameter_bytes
            : limits->max_result_bytes,
        "SQLite value byte limit", error);
  if (status != ORM_STATUS_OK)
    goto cleanup;
  out_backend->ops = &orm_sqlite_backend_ops;
  out_backend->context = state;
  state = NULL;
  orm_error_set(error, ORM_STATUS_OK, NULL);

cleanup:
  tstr_free(filename);
  if (state != NULL)
    orm_sqlite_backend_destroy(state);
  return status;
}
