#include "config.h"
#include <gmssl/hex.h>
#include <gmssl/mem.h>
#include <cmeta_fs.h>
#include <toml.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

enum { TIDESSQLD_ERROR_TEXT = 256, TIDESSQLD_MAX_NAME_BYTES = 128,
       TIDESSQLD_MAX_PATH_BYTES = 4096,
       TIDESSQLD_MAX_VERSION_BYTES = MYSQL_WIRE_SERVER_VERSION_CAPACITY - 1u };

static turbodb_status_t config_error(turbodb_error_t *error, turbodb_status_t status,
                                     const char *reason) {
  if (error && error->struct_size >= sizeof(*error)) {
    error->status = status;
    (void)snprintf(error->message, sizeof(error->message), "tidessqld config: %s", reason);
  }
  return status;
}

static bool key_is(const char *key, int length, const char *expected) {
  return length >= 0 && (size_t)length == strlen(expected) &&
         memcmp(key, expected, (size_t)length) == 0;
}

static bool table_has(const toml_table_t *table, const char *name) {
  const int count = toml_table_len(table);
  for (int i = 0; i < count; ++i) {
    int length = 0;
    const char *key = toml_table_key(table, i, &length);
    if (key && key_is(key, length, name)) return true;
  }
  return false;
}

static turbodb_status_t known_keys(const toml_table_t *table, const char *const *allowed,
                                   size_t allowed_count, turbodb_error_t *error) {
  const int count = toml_table_len(table);
  for (int i = 0; i < count; ++i) {
    int length = 0;
    const char *key = toml_table_key(table, i, &length);
    bool known = false;
    for (size_t j = 0; key && j < allowed_count; ++j) {
      if (key_is(key, length, allowed[j])) { known = true; break; }
    }
    if (!known) return config_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                                    "unknown key or table");
  }
  return TURBODB_STATUS_OK;
}

static turbodb_status_t string_value(const toml_table_t *table, const char *key, size_t limit,
                                     bool required, tstr *out, turbodb_error_t *error) {
  if (!table_has(table, key)) {
    return required ? config_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                                   "required string is missing") : TURBODB_STATUS_OK;
  }
  toml_value_t value = toml_table_string(table, key);
  if (!value.ok) return config_error(error, TURBODB_STATUS_TYPE_ERROR,
                                     "string field has the wrong type");
  turbodb_status_t status = TURBODB_STATUS_OK;
  if (value.u.sl <= 0 || (size_t)value.u.sl > limit ||
      strlen(value.u.s) != (size_t)value.u.sl) {
    status = config_error(error, value.u.sl > 0 ? TURBODB_STATUS_LIMIT_EXCEEDED
                                                : TURBODB_STATUS_INVALID_ARGUMENT,
                          "string field is empty, too long, or contains NUL");
  } else {
    tstr copy = tstr_dup_len(value.u.s, (size_t)value.u.sl);
    if (!copy) status = config_error(error, TURBODB_STATUS_OUT_OF_MEMORY,
                                     "copy string field");
    else { tstr_free(*out); *out = copy; }
  }
  free(value.u.s);
  return status;
}

static turbodb_status_t integer_value(const toml_table_t *table, const char *key, int64_t minimum,
                                      int64_t maximum, bool required, int64_t *out,
                                      turbodb_error_t *error) {
  if (!table_has(table, key)) {
    return required ? config_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                                   "required integer is missing") : TURBODB_STATUS_OK;
  }
  const toml_value_t value = toml_table_int(table, key);
  if (!value.ok) return config_error(error, TURBODB_STATUS_TYPE_ERROR,
                                     "integer field has the wrong type");
  if (value.u.i < minimum || value.u.i > maximum)
    return config_error(error, TURBODB_STATUS_OUT_OF_RANGE, "integer field is out of range");
  *out = value.u.i;
  return TURBODB_STATUS_OK;
}

static turbodb_status_t boolean_value(const toml_table_t *table, const char *key, bool required,
                                      bool *out, turbodb_error_t *error) {
  if (!table_has(table, key)) {
    return required ? config_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                                   "required boolean is missing") : TURBODB_STATUS_OK;
  }
  const toml_value_t value = toml_table_bool(table, key);
  if (!value.ok) return config_error(error, TURBODB_STATUS_TYPE_ERROR,
                                     "boolean field has the wrong type");
  *out = value.u.b;
  return TURBODB_STATUS_OK;
}

static bool power_of_two(size_t value) { return value && (value & (value - 1u)) == 0u; }

static bool numeric_host(const char *host) {
  struct in_addr ipv4;
  struct in6_addr ipv6;
  return inet_pton(AF_INET, host, &ipv4) == 1 || inet_pton(AF_INET6, host, &ipv6) == 1;
}

native_io_backend_kind tidessqld_native_backend(void) {
#if defined(_WIN32)
  return NATIVE_IO_BACKEND_IOCP;
#elif defined(__linux__)
  return NATIVE_IO_BACKEND_EPOLL;
#else
  return NATIVE_IO_BACKEND_KQUEUE;
#endif
}

cnet_client_config tidessqld_transport_config(const tidessqld_server_config *config) {
  return (cnet_client_config){
    .backend = tidessqld_native_backend(),
    .connection_capacity = config->max_connections,
    .command_capacity = config->command_capacity,
    .request_capacity = config->request_capacity,
    .completion_batch_capacity = config->completion_capacity,
    .event_capacity = config->event_capacity,
    .max_send_bytes = config->output_bytes,
    .receive_buffer_bytes = config->input_bytes,
    .read_timeout_ms = config->read_timeout_ms,
    .write_timeout_ms = config->write_timeout_ms,
    .tls_io_buffer_bytes = config->tls_io_bytes,
    .tls_handshake_timeout_ms = config->tls_handshake_timeout_ms,
    .command_buffer_bytes = config->command_buffer_bytes,
    .event_buffer_bytes = config->event_buffer_bytes
  };
}

static turbodb_status_t server_defaults(tidessqld_server_config *server,
                                        turbodb_error_t *error) {
  server->host = tstr_dup("127.0.0.1");
  server->server_version = tstr_dup("8.0.0-TidesSQL-1.3");
  if (!server->host || !server->server_version)
    return config_error(error, TURBODB_STATUS_OUT_OF_MEMORY, "create server defaults");
  server->port = 3306;
  server->backlog = 128;
  server->max_connections = 128;
  server->poll_timeout_ms = 10;
  server->shutdown_timeout_ms = 5000;
  server->input_bytes = 65536;
  server->scratch_bytes = 65536;
  server->output_bytes = 65536;
  server->command_capacity = 256;
  server->request_capacity = 128;
  server->completion_capacity = 128;
  server->event_capacity = 256;
  server->read_timeout_ms = 30000;
  server->write_timeout_ms = 30000;
  server->tls_handshake_timeout_ms = 10000;
  server->tls_io_bytes = CNET_TLS_MIN_IO_BUFFER_BYTES;
  return TURBODB_STATUS_OK;
}

static turbodb_status_t parse_server(const toml_table_t *root, tidessqld_config *config,
                                     turbodb_error_t *error) {
  static const char *const keys[] = {
    "host", "port", "backlog", "max_connections", "poll_timeout_ms",
    "shutdown_timeout_ms", "input_bytes", "scratch_bytes", "output_bytes",
    "command_capacity", "request_capacity", "completion_batch_capacity", "event_capacity",
    "command_buffer_bytes", "event_buffer_bytes", "read_timeout_ms", "write_timeout_ms",
    "tls_handshake_timeout_ms", "tls_io_buffer_bytes", "server_version", "certificate_file",
    "private_key_file"
  };
  toml_table_t *table = toml_table_table(root, "server");
  if (!table) return config_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                                  "[server] table is required");
  turbodb_status_t status = known_keys(table, keys, sizeof(keys) / sizeof(keys[0]), error);
#define READ_SIZE(field, low, high) do { int64_t n=(int64_t)config->server.field; \
  if(status==TURBODB_STATUS_OK) status=integer_value(table,#field,(low),(high),false,&n,error); \
  config->server.field=(size_t)n; } while(0)
#define READ_U32(field, low, high) do { int64_t n=(int64_t)config->server.field; \
  if(status==TURBODB_STATUS_OK) status=integer_value(table,#field,(low),(high),false,&n,error); \
  config->server.field=(uint32_t)n; } while(0)
  if (status == TURBODB_STATUS_OK) status = string_value(table, "host", 64, false,
                                                         &config->server.host, error);
  int64_t port = config->server.port;
  if (status == TURBODB_STATUS_OK) status = integer_value(table, "port", 0, UINT16_MAX,
                                                           false, &port, error);
  config->server.port = (uint16_t)port;
  READ_SIZE(backlog, 1, 65535); READ_SIZE(max_connections, 1, TDSQL_MYSQL_SERVER_MAX_CONNECTIONS);
  READ_U32(poll_timeout_ms, 1, 60000); READ_U32(shutdown_timeout_ms, 1, 600000);
  READ_SIZE(input_bytes, TDSQL_MYSQL_SERVER_MIN_INPUT_BYTES, INT32_MAX);
  READ_SIZE(scratch_bytes, TDSQL_MYSQL_MIN_REPLY_BYTES, INT32_MAX);
  READ_SIZE(output_bytes, TDSQL_MYSQL_SERVER_MIN_OUTPUT_BYTES, INT32_MAX);
  READ_SIZE(command_capacity, 1, 1048576); READ_SIZE(request_capacity, 1, 1048576);
  { int64_t n=(int64_t)config->server.completion_capacity;
    if(status==TURBODB_STATUS_OK) status=integer_value(table,"completion_batch_capacity",1,1048576,false,&n,error);
    config->server.completion_capacity=(size_t)n; }
  READ_SIZE(event_capacity, 2, 1048576);
  READ_SIZE(command_buffer_bytes, 0, INT32_MAX); READ_SIZE(event_buffer_bytes, 0, INT32_MAX);
  READ_U32(read_timeout_ms, 1, 600000); READ_U32(write_timeout_ms, 1, 600000);
  READ_U32(tls_handshake_timeout_ms, 1, 600000);
  { int64_t n=(int64_t)config->server.tls_io_bytes;
    if(status==TURBODB_STATUS_OK) status=integer_value(table,"tls_io_buffer_bytes",CNET_TLS_MIN_IO_BUFFER_BYTES,INT32_MAX,false,&n,error);
    config->server.tls_io_bytes=(size_t)n; }
#undef READ_U32
#undef READ_SIZE
  if (status == TURBODB_STATUS_OK) status = string_value(table, "server_version",
      TIDESSQLD_MAX_VERSION_BYTES, false, &config->server.server_version, error);
  if (status == TURBODB_STATUS_OK) status = string_value(table, "certificate_file",
      TIDESSQLD_MAX_PATH_BYTES, true, &config->server.certificate_file, error);
  if (status == TURBODB_STATUS_OK) status = string_value(table, "private_key_file",
      TIDESSQLD_MAX_PATH_BYTES, true, &config->server.private_key_file, error);
  if (status == TURBODB_STATUS_OK &&
      (!power_of_two(config->server.command_capacity) ||
       !power_of_two(config->server.event_capacity) ||
       config->server.completion_capacity > config->server.request_capacity))
    status = config_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                          "queue capacities violate CNet bounds");
  if (status == TURBODB_STATUS_OK && !numeric_host(config->server.host))
    status = config_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                          "server host must be a numeric IPv4 or IPv6 address");
  if (status == TURBODB_STATUS_OK &&
      (!cmeta_fs_path_is_absolute(config->server.certificate_file) ||
       !cmeta_fs_path_is_absolute(config->server.private_key_file)))
    status = config_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                          "TLS paths must be absolute");
  return status;
}

static turbodb_status_t parse_databases(const toml_table_t *root, tidessqld_config *config,
                                        turbodb_error_t *error) {
  static const char *const keys[] = {"name", "path", "column_family", "initialize"};
  toml_array_t *array = toml_table_array(root, "database");
  const int count = array ? toml_array_len(array) : 0;
  if (count <= 0 || count > TIDESSQLD_MAX_DATABASES)
    return config_error(error, count > 0 ? TURBODB_STATUS_LIMIT_EXCEEDED
                                         : TURBODB_STATUS_INVALID_ARGUMENT,
                        "one to 64 [[database]] entries are required");
  for (int i = 0; i < count; ++i) {
    toml_table_t *table = toml_array_table(array, i);
    if (!table) return config_error(error, TURBODB_STATUS_TYPE_ERROR,
                                    "database entry must be a table");
    tidessqld_database_config *database = &config->databases[i];
    turbodb_status_t status = known_keys(table, keys, sizeof(keys) / sizeof(keys[0]), error);
    if (status == TURBODB_STATUS_OK) status = string_value(table, "name", TIDESSQLD_MAX_NAME_BYTES,
                                                           true, &database->name, error);
    if (status == TURBODB_STATUS_OK) status = string_value(table, "path", TIDESSQLD_MAX_PATH_BYTES,
                                                           true, &database->path, error);
    if (status == TURBODB_STATUS_OK) status = string_value(table, "column_family",
        TIDESSQLD_MAX_NAME_BYTES, true, &database->column_family, error);
    if (status == TURBODB_STATUS_OK) status = boolean_value(table, "initialize", true,
                                                            &database->initialize, error);
    if (status == TURBODB_STATUS_OK && !cmeta_fs_path_is_absolute(database->path))
      status = config_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                            "database path must be absolute");
    for (int j = 0; status == TURBODB_STATUS_OK && j < i; ++j) {
      if (strcmp(database->name, config->databases[j].name) == 0)
        status = config_error(error, TURBODB_STATUS_CONSTRAINT, "duplicate database name");
    }
    if (status != TURBODB_STATUS_OK) return status;
    config->database_count = (size_t)i + 1u;
  }
  return TURBODB_STATUS_OK;
}

static turbodb_status_t decode_hex(const toml_table_t *table, const char *key, uint8_t *out,
                                   size_t size, turbodb_error_t *error) {
  tstr text = NULL;
  turbodb_status_t status = string_value(table, key, size * 2u, true, &text, error);
  size_t decoded = size;
  if (status == TURBODB_STATUS_OK &&
      (tstr_len(text) != size * 2u || hex_to_bytes(text, tstr_len(text), out, &decoded) != 1 ||
       decoded != size))
    status = config_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "invalid verifier hex field");
  tstr_free(text);
  return status;
}

static turbodb_status_t parse_accounts(const toml_table_t *root, tidessqld_config *config,
                                       turbodb_error_t *error) {
  static const char *const keys[] = {"username", "password_iterations", "password_salt_hex", "password_hash_hex",
                                     "databases", "default_database"};
  toml_array_t *array = toml_table_array(root, "account");
  const int count = array ? toml_array_len(array) : 0;
  if (count <= 0 || count > TIDESSQLD_MAX_ACCOUNTS)
    return config_error(error, count > 0 ? TURBODB_STATUS_LIMIT_EXCEEDED
                                         : TURBODB_STATUS_INVALID_ARGUMENT,
                        "one to 1024 [[account]] entries are required");
  for (int i = 0; i < count; ++i) {
    toml_table_t *table = toml_array_table(array, i);
    if (!table) return config_error(error, TURBODB_STATUS_TYPE_ERROR,
                                    "account entry must be a table");
    tidessqld_account_config *account = &config->accounts[i];
    turbodb_status_t status = known_keys(table, keys, sizeof(keys) / sizeof(keys[0]), error);
    if (status == TURBODB_STATUS_OK) status = string_value(table, "username", TIDESSQLD_MAX_NAME_BYTES,
                                                           true, &account->username, error);
    int64_t iterations = TDSQL_MYSQL_PASSWORD_MIN_ITERATIONS;
    if (status == TURBODB_STATUS_OK) status = integer_value(table, "password_iterations",
        TDSQL_MYSQL_PASSWORD_MIN_ITERATIONS, TDSQL_MYSQL_PASSWORD_MAX_ITERATIONS, true,
        &iterations, error);
    account->password.iterations = (uint32_t)iterations;
    if (status == TURBODB_STATUS_OK) status = decode_hex(table, "password_salt_hex", account->password.salt,
                                                         sizeof(account->password.salt), error);
    if (status == TURBODB_STATUS_OK) status = decode_hex(table, "password_hash_hex", account->password.hash,
                                                         sizeof(account->password.hash), error);
    if (status == TURBODB_STATUS_OK) status = string_value(table, "default_database",
        TIDESSQLD_MAX_NAME_BYTES, true, &account->default_database, error);
    toml_array_t *grants = toml_table_array(table, "databases");
    const int grant_count = grants ? toml_array_len(grants) : 0;
    if (status == TURBODB_STATUS_OK && (grant_count <= 0 || grant_count > TIDESSQLD_MAX_DATABASES))
      status = config_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                            "account databases must contain one to 64 names");
    for (int j = 0; status == TURBODB_STATUS_OK && j < grant_count; ++j) {
      toml_value_t grant = toml_array_string(grants, j);
      if (!grant.ok || grant.u.sl <= 0 || grant.u.sl > TIDESSQLD_MAX_NAME_BYTES ||
          strlen(grant.u.s) != (size_t)grant.u.sl) {
        if (grant.ok) free(grant.u.s);
        status = config_error(error, TURBODB_STATUS_TYPE_ERROR,
                              "account database grant must be a bounded string");
        break;
      }
      account->databases[j] = tstr_dup_len(grant.u.s, (size_t)grant.u.sl);
      free(grant.u.s);
      if (!account->databases[j]) status = config_error(error, TURBODB_STATUS_OUT_OF_MEMORY,
                                                        "copy account database grant");
      else account->database_count = (size_t)j + 1u;
    }
    for (int j = 0; status == TURBODB_STATUS_OK && j < i; ++j) {
      if (strcmp(account->username, config->accounts[j].username) == 0)
        status = config_error(error, TURBODB_STATUS_CONSTRAINT, "duplicate account username");
    }
    if (status != TURBODB_STATUS_OK) return status;
    config->account_count = (size_t)i + 1u;
  }
  return TURBODB_STATUS_OK;
}

static turbodb_status_t validate_relationships(const tidessqld_config *config,
                                               turbodb_error_t *error) {
  cmeta_fs_stat_t file = {0};
  if (cmeta_fs_stat(config->server.certificate_file, &file) != 0 || !file.is_file ||
      cmeta_fs_stat(config->server.private_key_file, &file) != 0 || !file.is_file)
    return config_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                        "TLS certificate and private key must be readable files");
  for (size_t i = 0; i < config->account_count; ++i) {
    const tidessqld_account_config *account = &config->accounts[i];
    bool has_default = false;
    for (size_t j = 0; j < account->database_count; ++j) {
      bool found = false;
      for (size_t k = 0; k < config->database_count; ++k) {
        if (strcmp(account->databases[j], config->databases[k].name) == 0) {
          found = true;
          if (strcmp(account->default_database, config->databases[k].name) == 0)
            has_default = true;
          break;
        }
      }
      if (!found) return config_error(error, TURBODB_STATUS_CONSTRAINT,
                                      "account grant names an unknown database");
      for (size_t k = 0; k < j; ++k) {
        if (strcmp(account->databases[j], account->databases[k]) == 0)
          return config_error(error, TURBODB_STATUS_CONSTRAINT,
                              "account contains a duplicate database grant");
      }
    }
    if (!has_default) return config_error(error, TURBODB_STATUS_CONSTRAINT,
                                          "default_database must be granted");
  }
  return TURBODB_STATUS_OK;
}

static turbodb_status_t validate_tls(const tidessqld_config *config,
                                     turbodb_error_t *error) {
  const cnet_tls_server_config requested = {
    .size = sizeof(cnet_tls_server_config),
    .cert_file = config->server.certificate_file,
    .key_file = config->server.private_key_file
  };
  cnet_tls_server tls = {0};
  const int initialized = cnet_tls_server_init(&tls, &requested);
  if (initialized != SALTS_OK)
    return config_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                        "TLS certificate or private key is invalid");
  const int destroyed = cnet_tls_server_destroy(&tls);
  return destroyed == SALTS_OK ? TURBODB_STATUS_OK
      : config_error(error, TURBODB_STATUS_CLEANUP_FAILED,
                     "release TLS configuration validation");
}

static turbodb_status_t validate_transport(const tidessqld_config *config,
                                           turbodb_error_t *error) {
  const cnet_client_config requested = tidessqld_transport_config(&config->server);
  cnet_client client = {0};
  const int initialized = cnet_client_init(&client, &requested);
  if (initialized != SALTS_OK)
    return config_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                        "CNet transport capacities are invalid");
  int released = cnet_client_stop(&client, 0);
  if (released == SALTS_OK) released = cnet_client_destroy(&client);
  return released == SALTS_OK ? TURBODB_STATUS_OK
      : config_error(error, TURBODB_STATUS_CLEANUP_FAILED,
                     "release CNet transport validation");
}

void tidessqld_config_destroy(tidessqld_config *config) {
  if (!config) return;
  tstr_free(config->server.host); tstr_free(config->server.server_version);
  tstr_free(config->server.certificate_file); tstr_free(config->server.private_key_file);
  for (size_t i = 0; i < TIDESSQLD_MAX_DATABASES; ++i) {
    tstr_free(config->databases[i].name); tstr_free(config->databases[i].path);
    tstr_free(config->databases[i].column_family);
  }
  for (size_t i = 0; i < TIDESSQLD_MAX_ACCOUNTS; ++i) {
    tstr_free(config->accounts[i].username); tstr_free(config->accounts[i].default_database);
    for (size_t j = 0; j < TIDESSQLD_MAX_DATABASES; ++j)
      tstr_free(config->accounts[i].databases[j]);
    gmssl_secure_clear(&config->accounts[i].password, sizeof(config->accounts[i].password));
  }
  gmssl_secure_clear(config, sizeof(*config));
  free(config);
}

turbodb_status_t tidessqld_config_load(const char *path, tidessqld_config **out,
                                       turbodb_error_t *error) {
  if (!path || !out || *out || !cmeta_fs_path_is_absolute(path))
    return config_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                        "absolute config path and empty output are required");
  cmeta_fs_stat_t info = {0};
  if (cmeta_fs_stat(path, &info) != 0 || !info.is_file)
    return config_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "config file is not readable");
  if (info.size == 0 || info.size > TIDESSQLD_CONFIG_MAX_BYTES)
    return config_error(error, TURBODB_STATUS_LIMIT_EXCEEDED, "config file size is invalid");
  cmeta_fs_buf_t bytes = {0};
  if (cmeta_fs_read_file(path, &bytes) != 0)
    return config_error(error, TURBODB_STATUS_INVALID_ARGUMENT, "read config file");
  turbodb_status_t status = TURBODB_STATUS_OK;
  tstr input = NULL;
  tidessqld_config *config = NULL;
  toml_table_t *root = NULL;
  if (bytes.len != info.size || bytes.len > TIDESSQLD_CONFIG_MAX_BYTES)
    status = config_error(error, TURBODB_STATUS_INVALID_STATE, "config file changed while reading");
  if (status == TURBODB_STATUS_OK && memchr(bytes.base, 0, bytes.len))
    status = config_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                          "config file contains embedded NUL");
  if (status == TURBODB_STATUS_OK) {
    input = tstr_dup_len(bytes.base, bytes.len);
    if (!input) status = config_error(error, TURBODB_STATUS_OUT_OF_MEMORY, "copy config file");
  }
  cmeta_fs_buf_free(&bytes);
  char parse_error[TIDESSQLD_ERROR_TEXT] = {0};
  if (status == TURBODB_STATUS_OK) {
    root = toml_parse(input, parse_error, sizeof(parse_error));
    if (!root) status = config_error(error, TURBODB_STATUS_INVALID_ARGUMENT,
                                     parse_error[0] ? parse_error : "parse TOML");
  }
  if (status == TURBODB_STATUS_OK) {
    config = (tidessqld_config *)calloc(1, sizeof(*config));
    if (!config) status = config_error(error, TURBODB_STATUS_OUT_OF_MEMORY, "allocate config");
  }
  static const char *const root_keys[] = {"version", "server", "database", "account"};
  if (status == TURBODB_STATUS_OK) status = known_keys(root, root_keys,
      sizeof(root_keys) / sizeof(root_keys[0]), error);
  int64_t version = 0;
  if (status == TURBODB_STATUS_OK) status = integer_value(root, "version", TIDESSQLD_CONFIG_VERSION,
      TIDESSQLD_CONFIG_VERSION, true, &version, error);
  if (status == TURBODB_STATUS_OK) { config->version = (uint32_t)version; status = server_defaults(&config->server, error); }
  if (status == TURBODB_STATUS_OK) status = parse_server(root, config, error);
  if (status == TURBODB_STATUS_OK) status = parse_databases(root, config, error);
  if (status == TURBODB_STATUS_OK) status = parse_accounts(root, config, error);
  if (status == TURBODB_STATUS_OK) status = validate_relationships(config, error);
  if (status == TURBODB_STATUS_OK) status = validate_tls(config, error);
  if (status == TURBODB_STATUS_OK) status = validate_transport(config, error);
  toml_free(root); tstr_free(input);
  if (status != TURBODB_STATUS_OK) { tidessqld_config_destroy(config); return status; }
  *out = config;
  return TURBODB_STATUS_OK;
}
