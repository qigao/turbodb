#include "config.h"
#include "runtime.h"
#include <salts_fs.h>
#include <tinytest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TEST_CONFIG_BYTES = 16384 };
static char *directory, *database_path, *config_path;
static tidessqld_config *config;
static turbodb_error_t error;

static char *portable_path(const char *path) {
  const size_t length = strlen(path);
  char *copy = (char *)malloc(length + 1u);
  if (!copy) return NULL;
  memcpy(copy, path, length + 1u);
  for (size_t i = 0; i < length; ++i) if (copy[i] == '\\') copy[i] = '/';
  return copy;
}

static char *join_path(const char *base, const char *leaf) {
  const size_t size = strlen(base) + strlen(leaf) + 2u;
  char *path = (char *)malloc(size);
  if (path) (void)snprintf(path, size, "%s/%s", base, leaf);
  return path;
}

static int write_config_with_extra_database(
    const char *extra_server, const char *extra_database,
    const char *account_default, const char *salt, bool grant_known) {
  char *portable_database = portable_path(database_path);
  char *portable_config = portable_path(config_path);
  if (!portable_database || !portable_config) { free(portable_database); free(portable_config); return -1; }
  char *text = (char *)malloc(TEST_CONFIG_BYTES);
  if (!text) { free(portable_database); free(portable_config); return -1; }
  const int length = snprintf(text, TEST_CONFIG_BYTES,
      "version = 1\n"
      "[server]\n"
      "host = \"127.0.0.1\"\n"
      "port = 0\n"
      "certificate_file = \"%s\"\n"
      "private_key_file = \"%s\"\n"
      "%s\n"
      "[[database]]\n"
      "name = \"tenant\"\n"
      "path = \"%s\"\n"
      "column_family = \"server\"\n"
      "initialize = true\n"
      "%s"
      "[[account]]\n"
      "username = \"alice\"\n"
      "password_iterations = 600000\n"
      "password_salt_hex = \"%s\"\n"
      "password_hash_hex = \"cae9c801374596f17de48ba4ed7061692b5d0ab433932a7d3cdf698dfd8bb3e8\"\n"
      "databases = [\"%s\"]\n"
      "default_database = \"%s\"\n",
      TEST_TLS_CERT, TEST_TLS_KEY, extra_server ? extra_server : "",
      portable_database, extra_database ? extra_database : "", salt,
      grant_known ? "tenant" : "missing", account_default);
  const salts_fs_buf_t bytes = {text, length > 0 ? (size_t)length : 0u};
  const int result = length <= 0 || length >= TEST_CONFIG_BYTES ? -1
      : salts_fs_write_file(portable_config, &bytes);
  free(text); free(portable_config); free(portable_database);
  return result;
}

static int write_config(const char *extra_server, const char *account_default,
                        const char *salt, bool grant_known) {
  return write_config_with_extra_database(
      extra_server, NULL, account_default, salt, grant_known);
}

static int resize_config(size_t size, bool embed_nul) {
  salts_fs_buf_t source = {0};
  if (salts_fs_read_file(config_path, &source) != 0 || size < source.len) {
    salts_fs_buf_free(&source);
    return -1;
  }
  char *bytes = (char *)malloc(size);
  if (!bytes) {
    salts_fs_buf_free(&source);
    return -1;
  }
  memcpy(bytes, source.base, source.len);
  memset(bytes + source.len, ' ', size - source.len);
  if (embed_nul && size) bytes[size / 2u] = '\0';
  const salts_fs_buf_t output = {bytes, size};
  const int result = salts_fs_write_file(config_path, &output);
  free(bytes);
  salts_fs_buf_free(&source);
  return result;
}

spec("tidessqld strict owning configuration and runtime") {
  before_each() {
    directory = tt_make_temp_dir("tidessqld-config"); check_not_null(directory);
    database_path = join_path(directory, "database"); config_path = join_path(directory, "tidessqld.toml");
    check_not_null(database_path); check_not_null(config_path);
    config = NULL; turbodb_error_init(&error);
  }
  after_each() {
    tidessqld_config_destroy(config); config = NULL;
    check_equal(tt_remove_tree(directory), 0);
    free(config_path); free(database_path); free(directory);
    config_path = database_path = directory = NULL;
  }
  it("loads the versioned config and owns all borrowed policy strings") {
    check_equal(write_config(NULL, "tenant", "000102030405060708090a0b0c0d0e0f", true), 0);
    check_equal(tidessqld_config_load(config_path, &config, &error), TURBODB_STATUS_OK);
    check_not_null(config); check_equal(config->version, 1u);
    check_equal(config->database_count, 1u); check_equal(config->account_count, 1u);
    check_equal(strcmp(config->databases[0].name, "tenant"), 0);
    check_equal(config->accounts[0].password.iterations, 600000u);
  }
  it("rejects unknown keys before any database is opened") {
    check_equal(write_config("mystery = 1", "tenant", "000102030405060708090a0b0c0d0e0f", true), 0);
    check_equal(tidessqld_config_load(config_path, &config, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_null(config);
  }
  it("rejects malformed verifier hex") {
    check_equal(write_config(NULL, "tenant", "00010203", true), 0);
    check_equal(tidessqld_config_load(config_path, &config, &error), TURBODB_STATUS_INVALID_ARGUMENT);
    check_null(config);
  }
  it("matches frontend scratch and server version boundaries") {
    char server[TEST_CONFIG_BYTES];
    int length = snprintf(server, sizeof(server), "scratch_bytes = %u",
                          (unsigned)(TDSQL_MYSQL_MIN_REPLY_BYTES - 1u));
    check_greater(length, 0);
    check_less((size_t)length, sizeof(server));
    check_equal(write_config(server, "tenant",
                             "000102030405060708090a0b0c0d0e0f", true),
                0);
    check_equal(tidessqld_config_load(config_path, &config, &error),
                TURBODB_STATUS_OUT_OF_RANGE);
    check_null(config);

    length = snprintf(server, sizeof(server), "scratch_bytes = %u",
                      (unsigned)TDSQL_MYSQL_MIN_REPLY_BYTES);
    check_greater(length, 0);
    check_less((size_t)length, sizeof(server));
    check_equal(write_config(server, "tenant",
                             "000102030405060708090a0b0c0d0e0f", true),
                0);
    turbodb_error_init(&error);
    check_equal(tidessqld_config_load(config_path, &config, &error),
                TURBODB_STATUS_OK);
    check_equal(config->server.scratch_bytes,
                (size_t)TDSQL_MYSQL_MIN_REPLY_BYTES);
    tidessqld_config_destroy(config);
    config = NULL;

    char version[MYSQL_WIRE_SERVER_VERSION_CAPACITY + 1u];
    memset(version, 'v', sizeof(version));
    version[MYSQL_WIRE_SERVER_VERSION_CAPACITY - 1u] = 0;
    length = snprintf(server, sizeof(server), "server_version = \"%s\"", version);
    check_greater(length, 0);
    check_less((size_t)length, sizeof(server));
    check_equal(write_config(server, "tenant",
                             "000102030405060708090a0b0c0d0e0f", true),
                0);
    turbodb_error_init(&error);
    check_equal(tidessqld_config_load(config_path, &config, &error),
                TURBODB_STATUS_OK);
    check_equal(tstr_len(config->server.server_version),
                (size_t)MYSQL_WIRE_SERVER_VERSION_CAPACITY - 1u);
    tidessqld_config_destroy(config);
    config = NULL;

    version[MYSQL_WIRE_SERVER_VERSION_CAPACITY - 1u] = 'v';
    version[MYSQL_WIRE_SERVER_VERSION_CAPACITY] = 0;
    length = snprintf(server, sizeof(server), "server_version = \"%s\"", version);
    check_greater(length, 0);
    check_less((size_t)length, sizeof(server));
    check_equal(write_config(server, "tenant",
                             "000102030405060708090a0b0c0d0e0f", true),
                0);
    turbodb_error_init(&error);
    check_equal(tidessqld_config_load(config_path, &config, &error),
                TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(config);
  }
  it("rejects every zero or undersized server bound and invalid queue shape") {
    static const struct {
      const char *field;
      long long value;
    } range_cases[] = {
      {"backlog", 0},
      {"max_connections", 0},
      {"poll_timeout_ms", 0},
      {"shutdown_timeout_ms", 0},
      {"input_bytes", TDSQL_MYSQL_SERVER_MIN_INPUT_BYTES - 1},
      {"scratch_bytes", TDSQL_MYSQL_MIN_REPLY_BYTES - 1},
      {"output_bytes", TDSQL_MYSQL_SERVER_MIN_OUTPUT_BYTES - 1},
      {"command_capacity", 0},
      {"request_capacity", 0},
      {"completion_batch_capacity", 0},
      {"event_capacity", 0},
      {"event_capacity", 1},
      {"command_buffer_bytes", -1},
      {"event_buffer_bytes", -1},
      {"read_timeout_ms", 0},
      {"write_timeout_ms", 0},
      {"tls_handshake_timeout_ms", 0},
      {"tls_io_buffer_bytes", CNET_TLS_MIN_IO_BUFFER_BYTES - 1}
    };
    char server[TEST_CONFIG_BYTES];
    for (size_t i = 0u; i < sizeof(range_cases) / sizeof(range_cases[0]); ++i) {
      const int length = snprintf(server, sizeof(server), "%s = %lld",
                                  range_cases[i].field, range_cases[i].value);
      check_greater(length, 0);
      check_less((size_t)length, sizeof(server));
      check_equal(write_config(server, "tenant",
                               "000102030405060708090a0b0c0d0e0f", true),
                  0);
      turbodb_error_init(&error);
      check_equal(tidessqld_config_load(config_path, &config, &error),
                  TURBODB_STATUS_OUT_OF_RANGE);
      check_null(config);
    }

    static const char *const shape_cases[] = {
      "command_capacity = 3",
      "event_capacity = 3",
      "request_capacity = 1\ncompletion_batch_capacity = 2",
      "command_buffer_bytes = 1",
      "input_bytes = 64\nevent_buffer_bytes = 63"
    };
    for (size_t i = 0u; i < sizeof(shape_cases) / sizeof(shape_cases[0]); ++i) {
      check_equal(write_config(shape_cases[i], "tenant",
                               "000102030405060708090a0b0c0d0e0f", true),
                  0);
      turbodb_error_init(&error);
      check_equal(tidessqld_config_load(config_path, &config, &error),
                  TURBODB_STATUS_INVALID_ARGUMENT);
      check_null(config);
    }
  }
  it("accepts the exact minimum bounded server configuration") {
    char server[TEST_CONFIG_BYTES];
    const int length = snprintf(
        server, sizeof(server),
        "backlog = 1\n"
        "max_connections = 1\n"
        "poll_timeout_ms = 1\n"
        "shutdown_timeout_ms = 1\n"
        "input_bytes = %u\n"
        "scratch_bytes = %u\n"
        "output_bytes = %u\n"
        "command_capacity = 1\n"
        "request_capacity = 1\n"
        "completion_batch_capacity = 1\n"
        "event_capacity = 2\n"
        "command_buffer_bytes = 0\n"
        "event_buffer_bytes = 0\n"
        "read_timeout_ms = 1\n"
        "write_timeout_ms = 1\n"
        "tls_handshake_timeout_ms = 1\n"
        "tls_io_buffer_bytes = %u",
        (unsigned)TDSQL_MYSQL_SERVER_MIN_INPUT_BYTES,
        (unsigned)TDSQL_MYSQL_MIN_REPLY_BYTES,
        (unsigned)TDSQL_MYSQL_SERVER_MIN_OUTPUT_BYTES,
        (unsigned)CNET_TLS_MIN_IO_BUFFER_BYTES);
    check_greater(length, 0);
    check_less((size_t)length, sizeof(server));
    check_equal(write_config(server, "tenant",
                             "000102030405060708090a0b0c0d0e0f", true),
                0);
    check_equal(tidessqld_config_load(config_path, &config, &error),
                TURBODB_STATUS_OK);
    check_equal(config->server.max_connections, 1u);
    check_equal(config->server.scratch_bytes,
                (size_t)TDSQL_MYSQL_MIN_REPLY_BYTES);
    check_equal(config->server.tls_io_bytes,
                (size_t)CNET_TLS_MIN_IO_BUFFER_BYTES);
  }
  it("accepts exactly one MiB and rejects embedded NUL or one byte more") {
    check_equal(write_config(NULL, "tenant", "000102030405060708090a0b0c0d0e0f", true), 0);
    check_equal(resize_config(TIDESSQLD_CONFIG_MAX_BYTES, false), 0);
    check_equal(tidessqld_config_load(config_path, &config, &error), TURBODB_STATUS_OK);
    check_not_null(config);
    tidessqld_config_destroy(config); config = NULL;

    check_equal(resize_config(TIDESSQLD_CONFIG_MAX_BYTES + 1u, false), 0);
    turbodb_error_init(&error);
    check_equal(tidessqld_config_load(config_path, &config, &error),
                TURBODB_STATUS_LIMIT_EXCEEDED);
    check_null(config);

    check_equal(write_config(NULL, "tenant", "000102030405060708090a0b0c0d0e0f", true), 0);
    check_equal(resize_config(TEST_CONFIG_BYTES, true), 0);
    turbodb_error_init(&error);
    check_equal(tidessqld_config_load(config_path, &config, &error),
                TURBODB_STATUS_INVALID_ARGUMENT);
    check_null(config);
  }
  it("rejects unknown grants and ungranted defaults") {
    check_equal(write_config(NULL, "tenant", "000102030405060708090a0b0c0d0e0f", false), 0);
    check_equal(tidessqld_config_load(config_path, &config, &error), TURBODB_STATUS_CONSTRAINT);
    check_null(config);
  }
  it("opens all databases then starts and cleanly stops the bounded listener") {
    check_equal(write_config(NULL, "tenant", "000102030405060708090a0b0c0d0e0f", true), 0);
    check_equal(tidessqld_config_load(config_path, &config, &error), TURBODB_STATUS_OK);
    tidessqld_runtime runtime = {0};
    check_equal(tidessqld_runtime_start(&runtime, config, &error), TURBODB_STATUS_OK);
    uint16_t port = 0;
    check_equal(tidessqld_runtime_port(&runtime, &port, &error), TURBODB_STATUS_OK);
    check_greater(port, 0u);
    check_equal(tidessqld_runtime_stop(&runtime, &error), TURBODB_STATUS_OK);
    check_false(runtime.initialized);
  }
  it("closes earlier databases when a later database cannot open") {
    char *missing_path = join_path(directory, "missing/database");
    char *portable_missing = portable_path(missing_path);
    check_not_null(missing_path); check_not_null(portable_missing);
    char extra_database[TEST_CONFIG_BYTES];
    const int length = snprintf(
        extra_database, sizeof(extra_database),
        "[[database]]\n"
        "name = \"unavailable\"\n"
        "path = \"%s\"\n"
        "column_family = \"server\"\n"
        "initialize = false\n",
        portable_missing);
    check_greater(length, 0);
    check_less((size_t)length, sizeof(extra_database));
    check_equal(write_config_with_extra_database(
                    NULL, extra_database, "tenant",
                    "000102030405060708090a0b0c0d0e0f", true),
                0);
    check_equal(tidessqld_config_load(config_path, &config, &error),
                TURBODB_STATUS_OK);

    tidessqld_runtime runtime = {0};
    check_not_equal(tidessqld_runtime_start(&runtime, config, &error),
                    TURBODB_STATUS_OK);
    check_false(runtime.initialized);
    check_null(runtime.config);
    check_equal(runtime.database_count, 0u);
    check_null(runtime.databases[0]);
    check_null(runtime.databases[1]);
    check_equal(tt_remove_tree(database_path), 0);

    free(portable_missing);
    free(missing_path);
  }
}
