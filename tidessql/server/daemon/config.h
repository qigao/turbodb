#ifndef TIDESSQLD_CONFIG_H
#define TIDESSQLD_CONFIG_H

#include "../mysql/server.h"
#include <cnet/cnet.h>

enum {
  TIDESSQLD_CONFIG_VERSION = 1,
  TIDESSQLD_CONFIG_MAX_BYTES = 1024 * 1024,
  TIDESSQLD_MAX_DATABASES = TDSQL_MYSQL_MAX_DATABASES,
  TIDESSQLD_MAX_ACCOUNTS = TDSQL_MYSQL_MAX_ACCOUNTS
};

typedef struct tidessqld_server_config {
  tstr host, server_version, certificate_file, private_key_file;
  uint16_t port;
  size_t backlog, max_connections, input_bytes, scratch_bytes, output_bytes;
  size_t command_capacity, request_capacity, completion_capacity, event_capacity;
  size_t command_buffer_bytes, event_buffer_bytes, tls_io_bytes;
  uint32_t poll_timeout_ms, shutdown_timeout_ms, read_timeout_ms, write_timeout_ms;
  uint32_t tls_handshake_timeout_ms;
} tidessqld_server_config;

typedef struct tidessqld_database_config {
  tstr name, path, column_family;
  bool initialize;
} tidessqld_database_config;

typedef struct tidessqld_account_config {
  tstr username, default_database;
  tdsql_mysql_password_record password;
  tstr databases[TIDESSQLD_MAX_DATABASES];
  size_t database_count;
} tidessqld_account_config;

typedef struct tidessqld_config {
  uint32_t version;
  tidessqld_server_config server;
  tidessqld_database_config databases[TIDESSQLD_MAX_DATABASES];
  size_t database_count;
  tidessqld_account_config accounts[TIDESSQLD_MAX_ACCOUNTS];
  size_t account_count;
} tidessqld_config;

turbodb_status_t tidessqld_config_load(const char *path, tidessqld_config **out,
                                       turbodb_error_t *error);
void tidessqld_config_destroy(tidessqld_config *config);
native_io_backend_kind tidessqld_native_backend(void);
cnet_client_config tidessqld_transport_config(const tidessqld_server_config *config);

#endif
