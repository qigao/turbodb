#include "postgresql/dbtool_postgresql.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libpq-fe.h>

typedef struct dbtool_postgresql_state {
  PGconn *connection;
} dbtool_postgresql_state;

dbtool_status dbtool_postgresql_error(dbtool_status status, const char *stage,
                                      int native_code,
                                      const char *operation,
                                      const char *detail,
                                      dbtool_error *error) {
  char message[DBTOOL_ERROR_MESSAGE_CAPACITY];
  const char *reason =
      detail != NULL && detail[0] != '\0' ? detail : "PostgreSQL failure";
  (void)snprintf(message, sizeof(message), "%s: %s", operation, reason);
  dbtool_error_set(error, status, stage, native_code, message);
  return status;
}

static dbtool_status dbtool_postgresql_open(
    void **out_context, const dbtool_connection_config *config,
    dbtool_error *error) {
  dbtool_postgresql_state *state;
  PGconn *connection;
  if (out_context != NULL)
    *out_context = NULL;
  dbtool_error_init(error);
  if (out_context == NULL || config == NULL) {
    dbtool_error_set(error, DBTOOL_STATUS_INVALID_ARGUMENT, "open-driver", 0,
                     "invalid PostgreSQL connection configuration");
    return DBTOOL_STATUS_INVALID_ARGUMENT;
  }
  connection = PQconnectdb(config->conninfo != NULL ? config->conninfo : "");
  if (connection == NULL) {
    dbtool_error_set(error, DBTOOL_STATUS_OUT_OF_MEMORY, "open-driver", 0,
                     "allocate PostgreSQL connection");
    return DBTOOL_STATUS_OUT_OF_MEMORY;
  }
  if (PQstatus(connection) != CONNECTION_OK) {
    (void)dbtool_postgresql_error(
        DBTOOL_STATUS_CONNECTION_ERROR, "open-driver",
        (int)PQstatus(connection), "open PostgreSQL connection",
        PQerrorMessage(connection), error);
    PQfinish(connection);
    return DBTOOL_STATUS_CONNECTION_ERROR;
  }
  state = (dbtool_postgresql_state *)calloc(1u, sizeof(*state));
  if (state == NULL) {
    PQfinish(connection);
    dbtool_error_set(error, DBTOOL_STATUS_OUT_OF_MEMORY, "open-driver", 0,
                     "allocate PostgreSQL driver state");
    return DBTOOL_STATUS_OUT_OF_MEMORY;
  }
  state->connection = connection;
  *out_context = state;
  return DBTOOL_STATUS_OK;
}

static dbtool_status dbtool_postgresql_apply(
    void *context, const char *sql, size_t sql_size,
    dbtool_apply_result *result, dbtool_error *error) {
  dbtool_postgresql_state *state = (dbtool_postgresql_state *)context;
  dbtool_status status = DBTOOL_STATUS_OK;
  uint64_t statements = 0u;
  int saw_result = 0;
  PGresult *native_result;
  if (result != NULL)
    *result = (dbtool_apply_result)DBTOOL_APPLY_RESULT_INIT;
  dbtool_error_init(error);
  if (state == NULL || state->connection == NULL || sql == NULL ||
      sql_size == 0u || result == NULL) {
    dbtool_error_set(error, DBTOOL_STATUS_INVALID_ARGUMENT, "apply-schema", 0,
                     "invalid PostgreSQL schema input");
    return DBTOOL_STATUS_INVALID_ARGUMENT;
  }
  if (memchr(sql, '\0', sql_size) != NULL) {
    dbtool_error_set(error, DBTOOL_STATUS_INVALID_ARGUMENT, "apply-schema", 0,
                     "PostgreSQL schema input contains embedded NUL");
    return DBTOOL_STATUS_INVALID_ARGUMENT;
  }
  if (PQsendQuery(state->connection, sql) != 1) {
    return dbtool_postgresql_error(
        DBTOOL_STATUS_CONNECTION_ERROR, "send-schema", 0,
        "send PostgreSQL schema", PQerrorMessage(state->connection), error);
  }

  while ((native_result = PQgetResult(state->connection)) != NULL) {
    const ExecStatusType native_status = PQresultStatus(native_result);
    saw_result = 1;
    if (status == DBTOOL_STATUS_OK) {
      if (native_status == PGRES_COMMAND_OK) {
        ++statements;
      } else if (native_status == PGRES_EMPTY_QUERY) {
        /* Empty fragments are valid but do not represent a statement. */
      } else if (native_status == PGRES_TUPLES_OK ||
                 native_status == PGRES_SINGLE_TUPLE) {
        status = dbtool_postgresql_error(
            DBTOOL_STATUS_UNSUPPORTED, "apply-schema", (int)native_status,
            "PostgreSQL schema produced rows", NULL, error);
      } else {
        status = dbtool_postgresql_error(
            DBTOOL_STATUS_SQL_ERROR, "apply-schema", (int)native_status,
            "execute PostgreSQL schema", PQresultErrorMessage(native_result),
            error);
      }
    }
    PQclear(native_result);
  }
  if (!saw_result && status == DBTOOL_STATUS_OK) {
    return dbtool_postgresql_error(
        DBTOOL_STATUS_CONNECTION_ERROR, "receive-schema", 0,
        "receive PostgreSQL schema result", PQerrorMessage(state->connection),
        error);
  }
  if (status == DBTOOL_STATUS_OK)
    result->statements = statements;
  return status;
}

static void dbtool_postgresql_close(void *context) {
  dbtool_postgresql_state *state = (dbtool_postgresql_state *)context;
  if (state == NULL)
    return;
  if (state->connection != NULL)
    PQfinish(state->connection);
  free(state);
}

static const dbtool_schema_driver_ops dbtool_postgresql_ops = {
    sizeof(dbtool_schema_driver_ops), DBTOOL_SCHEMA_DRIVER_ABI_VERSION,
    dbtool_postgresql_open, dbtool_postgresql_apply, dbtool_postgresql_close};

const dbtool_schema_driver_ops *dbtool_postgresql_schema_driver(void) {
  return &dbtool_postgresql_ops;
}

PGconn *dbtool_postgresql_native_connection(void *context) {
  dbtool_postgresql_state *state = (dbtool_postgresql_state *)context;
  return state != NULL ? state->connection : NULL;
}
