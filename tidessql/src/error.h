#ifndef TIDESSQL_ERROR_H
#define TIDESSQL_ERROR_H

#include <turbodb/types.h>
#include <cstl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Engine diagnostics use the shared DTO without a runtime dependency. */
static inline void tdsql_error_init(turbodb_error_t *error) { turbodb_error_init(error); }

/* Every error producer supplies its diagnostic; undersized DTOs stay intact. */
static inline void tdsql_error_set(turbodb_error_t *error, turbodb_status_t status,
    const char *message) {
  if (!error || error->struct_size < sizeof(*error)) return;
  error->status = status;
  (void)snprintf(error->message, sizeof(error->message), "%s",
      status == TURBODB_STATUS_OK ? "" : message);
}

#endif
