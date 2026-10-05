#include "runtime.h"
#include <gmssl/mem.h>
#include <stdio.h>
#include <string.h>

static turbodb_status_t runtime_error(turbodb_error_t *error, turbodb_status_t status,
                                      const char *reason) {
  if (error && error->struct_size >= sizeof(*error)) {
    error->status = status;
    (void)snprintf(error->message, sizeof(error->message), "tidessqld runtime: %s", reason);
  }
  return status;
}

static size_t database_index(const tidessqld_config *config, const char *name) {
  for (size_t i = 0; i < config->database_count; ++i)
    if (strcmp(config->databases[i].name, name) == 0) return i;
  return SIZE_MAX;
}

static turbodb_status_t close_databases(tidessqld_runtime *runtime,
                                        turbodb_error_t *error) {
  turbodb_status_t first = TURBODB_STATUS_OK;
  for (size_t i = runtime->database_count; i > 0; --i) {
    const size_t index = i - 1u;
    if (!runtime->databases[index]) continue;
    turbodb_error_t current; turbodb_error_init(&current);
    const turbodb_status_t status = tdsql_database_close(runtime->databases[index], &current);
    if (status != TURBODB_STATUS_BUSY) runtime->databases[index] = NULL;
    if (first == TURBODB_STATUS_OK && status != TURBODB_STATUS_OK) {
      first = status;
      if (error && error->struct_size >= sizeof(*error)) *error = current;
    }
  }
  if (first == TURBODB_STATUS_OK) runtime->database_count = 0;
  return first;
}

static turbodb_status_t open_databases(tidessqld_runtime *runtime,
                                       turbodb_error_t *error) {
  for (size_t i = 0; i < runtime->config->database_count; ++i) {
    const tidessqld_database_config *source = &runtime->config->databases[i];
    const turbodb_option_t options[] = {
      {turbodb_view("path"), turbodb_view_tstr(source->path)},
      {turbodb_view("column_family"), turbodb_view_tstr(source->column_family)},
      {turbodb_view("sql_initialize"), turbodb_view(source->initialize ? "true" : "false")}
    };
    tdsql_config database_config = tdsql_config_default();
    database_config.options = options;
    database_config.option_count = sizeof(options) / sizeof(options[0]);
    turbodb_status_t status = tdsql_database_open(&database_config, &runtime->databases[i], error);
    if (status != TURBODB_STATUS_OK) return status;
    runtime->database_count = i + 1u;
    runtime->bindings[i] = (tdsql_mysql_database_binding){
      turbodb_view_tstr(source->name), runtime->databases[i]
    };
  }
  return TURBODB_STATUS_OK;
}

static turbodb_status_t build_policy(tidessqld_runtime *runtime,
                                     turbodb_error_t *error) {
  const tidessqld_config *config = runtime->config;
  for (size_t i = 0; i < config->account_count; ++i) {
    const tidessqld_account_config *source = &config->accounts[i];
    uint64_t grants = 0;
    for (size_t j = 0; j < source->database_count; ++j) {
      const size_t index = database_index(config, source->databases[j]);
      if (index == SIZE_MAX) return runtime_error(error, TURBODB_STATUS_INVALID_STATE,
                                                  "validated grant disappeared");
      grants |= UINT64_C(1) << index;
    }
    const size_t default_database = database_index(config, source->default_database);
    if (default_database == SIZE_MAX || !(grants & (UINT64_C(1) << default_database)))
      return runtime_error(error, TURBODB_STATUS_INVALID_STATE,
                           "validated default database disappeared");
    runtime->accounts[i] = (tdsql_mysql_account){
      .username = turbodb_view_tstr(source->username),
      .password = source->password,
      .databases = grants,
      .default_database = default_database
    };
  }
  tdsql_mysql_auth_policy requested = tdsql_mysql_auth_policy_default();
  requested.accounts = runtime->accounts;
  requested.account_count = config->account_count;
  requested.max_accounts = TIDESSQLD_MAX_ACCOUNTS;
  requested.databases = runtime->bindings;
  requested.database_count = config->database_count;
  return tdsql_mysql_auth_policy_init(&runtime->policy, &requested, error);
}

turbodb_status_t tidessqld_runtime_start(tidessqld_runtime *runtime,
                                         const tidessqld_config *config,
                                         turbodb_error_t *error) {
  if (!runtime || runtime->initialized || !config)
    return runtime_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                         "empty runtime and validated config are required");
  runtime->config = config;
  turbodb_status_t status = open_databases(runtime, error);
  if (status == TURBODB_STATUS_OK) status = build_policy(runtime, error);
  const tidessqld_server_config *source = &config->server;
  const native_io_backend_kind backend = tidessqld_native_backend();
  const cnet_client_config transport = tidessqld_transport_config(source);
  tdsql_mysql_server_config server_config = {
    .policy = &runtime->policy,
    .registry = tdsql_mysql_registry_config_default(),
    .server_version = turbodb_view_tstr(source->server_version),
    .listener = {.backend = backend, .host = source->host, .port = source->port,
                 .backlog = source->backlog},
    .transport = transport,
    .tls = {.size = sizeof(cnet_tls_server_config), .cert_file = source->certificate_file,
            .key_file = source->private_key_file},
    .max_connections = source->max_connections,
    .input_bytes = source->input_bytes,
    .scratch_bytes = source->scratch_bytes,
    .output_bytes = source->output_bytes
  };
  if (status == TURBODB_STATUS_OK)
    status = tdsql_mysql_server_init(&runtime->server, &server_config, error);
  if (status != TURBODB_STATUS_OK) {
    const turbodb_status_t cleanup = close_databases(runtime, NULL);
    gmssl_secure_clear(runtime->accounts, sizeof(runtime->accounts));
    *runtime = (tidessqld_runtime){0};
    if (cleanup != TURBODB_STATUS_OK)
      return runtime_error(error, TURBODB_STATUS_CLEANUP_FAILED,
                           "database cleanup after failed startup");
    return status;
  }
  runtime->initialized = true;
  return TURBODB_STATUS_OK;
}

turbodb_status_t tidessqld_runtime_poll(tidessqld_runtime *runtime, size_t *events,
                                        turbodb_error_t *error) {
  if (!runtime || !runtime->initialized)
    return runtime_error(error, TURBODB_STATUS_INVALID_STATE, "runtime is not started");
  return tdsql_mysql_server_poll(&runtime->server, runtime->config->server.poll_timeout_ms,
                                 events, error);
}

turbodb_status_t tidessqld_runtime_port(const tidessqld_runtime *runtime, uint16_t *port,
                                        turbodb_error_t *error) {
  if (!runtime || !runtime->initialized)
    return runtime_error(error, TURBODB_STATUS_INVALID_STATE, "runtime is not started");
  return tdsql_mysql_server_port(&runtime->server, port, error);
}

turbodb_status_t tidessqld_runtime_stop(tidessqld_runtime *runtime,
                                        turbodb_error_t *error) {
  if (!runtime || !runtime->initialized) return TURBODB_STATUS_OK;
  turbodb_status_t status = tdsql_mysql_server_stop(&runtime->server,
      runtime->config->server.shutdown_timeout_ms, error);
  if (status != TURBODB_STATUS_OK) return status;
  status = close_databases(runtime, error);
  if (status == TURBODB_STATUS_BUSY) return status;
  gmssl_secure_clear(runtime->accounts, sizeof(runtime->accounts));
  if (status == TURBODB_STATUS_OK) *runtime = (tidessqld_runtime){0};
  else runtime->initialized = false;
  return status;
}
