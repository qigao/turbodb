#ifndef TIDESSQLD_RUNTIME_H
#define TIDESSQLD_RUNTIME_H

#include "config.h"

typedef struct tidessqld_runtime {
  const tidessqld_config *config;
  tdsql_database *databases[TIDESSQLD_MAX_DATABASES];
  tdsql_mysql_database_binding bindings[TIDESSQLD_MAX_DATABASES];
  tdsql_mysql_account accounts[TIDESSQLD_MAX_ACCOUNTS];
  tdsql_mysql_auth_policy policy;
  tdsql_mysql_server server;
  size_t database_count;
  bool initialized;
} tidessqld_runtime;

turbodb_status_t tidessqld_runtime_start(tidessqld_runtime *runtime,
                                         const tidessqld_config *config,
                                         turbodb_error_t *error);
turbodb_status_t tidessqld_runtime_poll(tidessqld_runtime *runtime, size_t *events,
                                        turbodb_error_t *error);
turbodb_status_t tidessqld_runtime_port(const tidessqld_runtime *runtime, uint16_t *port,
                                        turbodb_error_t *error);
turbodb_status_t tidessqld_runtime_stop(tidessqld_runtime *runtime,
                                        turbodb_error_t *error);

#endif
