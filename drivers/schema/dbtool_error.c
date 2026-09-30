#include "dbtool_error.h"

#include <stdio.h>
#include <string.h>

void dbtool_error_init(dbtool_error *error) {
  if (error == NULL)
    return;
  memset(error, 0, sizeof(*error));
  error->status = DBTOOL_STATUS_OK;
}

void dbtool_error_set(dbtool_error *error, dbtool_status status,
                      const char *stage, int native_code,
                      const char *message) {
  if (error == NULL)
    return;
  dbtool_error_init(error);
  error->status = status;
  error->native_code = native_code;
  if (stage != NULL)
    (void)snprintf(error->stage, sizeof(error->stage), "%s", stage);
  if (message != NULL)
    (void)snprintf(error->message, sizeof(error->message), "%s", message);
}

const char *dbtool_status_name(dbtool_status status) {
  switch (status) {
    case DBTOOL_STATUS_OK: return "ok";
    case DBTOOL_STATUS_HELP: return "help";
    case DBTOOL_STATUS_INVALID_ARGUMENT: return "invalid-argument";
    case DBTOOL_STATUS_LIMIT_EXCEEDED: return "limit-exceeded";
    case DBTOOL_STATUS_FILE_ERROR: return "file-error";
    case DBTOOL_STATUS_CONNECTION_ERROR: return "connection-error";
    case DBTOOL_STATUS_SQL_ERROR: return "sql-error";
    case DBTOOL_STATUS_UNSUPPORTED: return "unsupported";
    case DBTOOL_STATUS_OUT_OF_MEMORY: return "out-of-memory";
    case DBTOOL_STATUS_INTERNAL_ERROR: return "internal-error";
    default: return "unknown";
  }
}

int dbtool_status_exit_code(dbtool_status status) {
  switch (status) {
    case DBTOOL_STATUS_OK:
    case DBTOOL_STATUS_HELP: return 0;
    case DBTOOL_STATUS_INVALID_ARGUMENT: return 2;
    case DBTOOL_STATUS_FILE_ERROR: return 3;
    case DBTOOL_STATUS_CONNECTION_ERROR: return 4;
    case DBTOOL_STATUS_SQL_ERROR: return 5;
    case DBTOOL_STATUS_LIMIT_EXCEEDED: return 6;
    case DBTOOL_STATUS_UNSUPPORTED: return 7;
    case DBTOOL_STATUS_OUT_OF_MEMORY: return 8;
    case DBTOOL_STATUS_INTERNAL_ERROR:
    default: return 70;
  }
}
