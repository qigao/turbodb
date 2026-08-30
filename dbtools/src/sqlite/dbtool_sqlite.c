#include "sqlite/dbtool_sqlite.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

typedef struct dbtool_sqlite_state {
  sqlite3 *database;
} dbtool_sqlite_state;

typedef struct dbtool_sqlite_apply_guard {
  uint64_t statements;
  int denied_transaction_control;
} dbtool_sqlite_apply_guard;

dbtool_status dbtool_sqlite_error(sqlite3 *database, dbtool_status status,
                                  const char *stage, const char *operation,
                                  const char *detail, dbtool_error *error) {
  char message[DBTOOL_ERROR_MESSAGE_CAPACITY];
  const char *native_detail = detail;
  int native_code = 0;
  if (database != NULL) {
    native_code = sqlite3_extended_errcode(database);
    if (native_detail == NULL)
      native_detail = sqlite3_errmsg(database);
  }
  (void)snprintf(message, sizeof(message), "%s: %s", operation,
                 native_detail != NULL ? native_detail : "SQLite failure");
  dbtool_error_set(error, status, stage, native_code, message);
  return status;
}

static int dbtool_sqlite_authorize(void *context, int action,
                                   const char *argument1,
                                   const char *argument2,
                                   const char *database_name,
                                   const char *trigger_name) {
  dbtool_sqlite_apply_guard *guard =
      (dbtool_sqlite_apply_guard *)context;
  (void)argument1;
  (void)argument2;
  (void)database_name;
  (void)trigger_name;
  if (action == SQLITE_TRANSACTION || action == SQLITE_SAVEPOINT) {
    guard->denied_transaction_control = 1;
    return SQLITE_DENY;
  }
  return SQLITE_OK;
}

static int dbtool_sqlite_trace(unsigned trace_type, void *context,
                               void *statement, void *expanded_sql) {
  dbtool_sqlite_apply_guard *guard =
      (dbtool_sqlite_apply_guard *)context;
  (void)statement;
  (void)expanded_sql;
  if (trace_type == SQLITE_TRACE_STMT && guard->statements != UINT64_MAX)
    ++guard->statements;
  return 0;
}

static int dbtool_sqlite_control(sqlite3 *database, const char *sql,
                                 char **detail) {
  return sqlite3_exec(database, sql, NULL, NULL, detail);
}

static dbtool_status dbtool_sqlite_open(
    void **out_context, const dbtool_connection_config *config,
    dbtool_error *error) {
  dbtool_sqlite_state *state;
  int code;
  if (out_context != NULL)
    *out_context = NULL;
  dbtool_error_init(error);
  if (out_context == NULL || config == NULL || config->database == NULL ||
      config->database[0] == '\0' || config->busy_timeout_ms == 0u ||
      config->busy_timeout_ms > (uint32_t)INT_MAX) {
    dbtool_error_set(error, DBTOOL_STATUS_INVALID_ARGUMENT, "open-driver", 0,
                     "invalid SQLite connection configuration");
    return DBTOOL_STATUS_INVALID_ARGUMENT;
  }
  state = (dbtool_sqlite_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    dbtool_error_set(error, DBTOOL_STATUS_OUT_OF_MEMORY, "open-driver", 0,
                     "cannot allocate SQLite driver state");
    return DBTOOL_STATUS_OUT_OF_MEMORY;
  }
  code = sqlite3_open_v2(config->database, &state->database,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                             SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX,
                         NULL);
  if (code != SQLITE_OK) {
    (void)dbtool_sqlite_error(state->database,
                              DBTOOL_STATUS_CONNECTION_ERROR, "open-driver",
                              "open SQLite database", NULL, error);
    if (state->database != NULL)
      (void)sqlite3_close_v2(state->database);
    free(state);
    return DBTOOL_STATUS_CONNECTION_ERROR;
  }
  (void)sqlite3_extended_result_codes(state->database, 1);
  code = sqlite3_busy_timeout(state->database, (int)config->busy_timeout_ms);
  if (code != SQLITE_OK) {
    (void)dbtool_sqlite_error(state->database,
                              DBTOOL_STATUS_CONNECTION_ERROR, "open-driver",
                              "configure SQLite busy timeout", NULL, error);
    (void)sqlite3_close_v2(state->database);
    free(state);
    return DBTOOL_STATUS_CONNECTION_ERROR;
  }
  *out_context = state;
  return DBTOOL_STATUS_OK;
}

static dbtool_status dbtool_sqlite_apply(void *context, const char *sql,
                                         size_t sql_size,
                                         dbtool_apply_result *result,
                                         dbtool_error *error) {
  dbtool_sqlite_state *state = (dbtool_sqlite_state *)context;
  dbtool_sqlite_apply_guard guard = {0u, 0};
  char *detail = NULL;
  dbtool_status status = DBTOOL_STATUS_OK;
  int code;
  int transaction_active = 0;
  if (result != NULL)
    *result = (dbtool_apply_result)DBTOOL_APPLY_RESULT_INIT;
  dbtool_error_init(error);
  if (state == NULL || state->database == NULL || sql == NULL ||
      sql_size == 0u || result == NULL) {
    dbtool_error_set(error, DBTOOL_STATUS_INVALID_ARGUMENT, "apply-schema", 0,
                     "invalid SQLite schema input");
    return DBTOOL_STATUS_INVALID_ARGUMENT;
  }
  if (memchr(sql, '\0', sql_size) != NULL) {
    dbtool_error_set(error, DBTOOL_STATUS_INVALID_ARGUMENT, "apply-schema", 0,
                     "SQLite schema input contains embedded NUL");
    return DBTOOL_STATUS_INVALID_ARGUMENT;
  }

  code = dbtool_sqlite_control(state->database, "BEGIN IMMEDIATE", &detail);
  if (code != SQLITE_OK) {
    status = dbtool_sqlite_error(state->database, DBTOOL_STATUS_SQL_ERROR,
                                 "begin-schema", "begin SQLite schema apply",
                                 detail, error);
    sqlite3_free(detail);
    return status;
  }
  transaction_active = 1;
  (void)sqlite3_trace_v2(state->database, SQLITE_TRACE_STMT,
                         dbtool_sqlite_trace, &guard);
  code = sqlite3_set_authorizer(state->database, dbtool_sqlite_authorize,
                                &guard);
  if (code != SQLITE_OK) {
    status = dbtool_sqlite_error(
        state->database, DBTOOL_STATUS_SQL_ERROR, "apply-schema",
        "install SQLite schema authorizer", NULL, error);
    goto cleanup;
  }
  code = sqlite3_exec(state->database, sql, NULL, NULL, &detail);
  (void)sqlite3_set_authorizer(state->database, NULL, NULL);
  (void)sqlite3_trace_v2(state->database, 0u, NULL, NULL);
  if (code != SQLITE_OK) {
    status = dbtool_sqlite_error(
        state->database,
        guard.denied_transaction_control ? DBTOOL_STATUS_UNSUPPORTED
                                         : DBTOOL_STATUS_SQL_ERROR,
        "apply-schema",
        guard.denied_transaction_control
            ? "SQLite schema transaction control is unsupported"
            : "execute SQLite schema",
        detail, error);
    sqlite3_free(detail);
    detail = NULL;
    goto cleanup;
  }
  code = dbtool_sqlite_control(state->database, "COMMIT", &detail);
  if (code != SQLITE_OK) {
    status = dbtool_sqlite_error(state->database, DBTOOL_STATUS_SQL_ERROR,
                                 "commit-schema",
                                 "commit SQLite schema apply", detail, error);
    sqlite3_free(detail);
    detail = NULL;
    goto cleanup;
  }
  transaction_active = 0;
  result->statements = guard.statements;
  return DBTOOL_STATUS_OK;

cleanup:
  (void)sqlite3_set_authorizer(state->database, NULL, NULL);
  (void)sqlite3_trace_v2(state->database, 0u, NULL, NULL);
  if (transaction_active) {
    char *rollback_detail = NULL;
    (void)dbtool_sqlite_control(state->database, "ROLLBACK", &rollback_detail);
    sqlite3_free(rollback_detail);
  }
  return status;
}

static void dbtool_sqlite_close(void *context) {
  dbtool_sqlite_state *state = (dbtool_sqlite_state *)context;
  if (state == NULL)
    return;
  if (state->database != NULL)
    (void)sqlite3_close_v2(state->database);
  free(state);
}

static const dbtool_schema_driver_ops dbtool_sqlite_ops = {
    sizeof(dbtool_schema_driver_ops), DBTOOL_SCHEMA_DRIVER_ABI_VERSION,
    dbtool_sqlite_open, dbtool_sqlite_apply, dbtool_sqlite_close};

const dbtool_schema_driver_ops *dbtool_sqlite_schema_driver(void) {
  return &dbtool_sqlite_ops;
}

sqlite3 *dbtool_sqlite_native_database(void *context) {
  dbtool_sqlite_state *state = (dbtool_sqlite_state *)context;
  return state != NULL ? state->database : NULL;
}
