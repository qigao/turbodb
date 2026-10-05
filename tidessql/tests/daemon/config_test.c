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

static int write_config(const char *extra_server, const char *account_default,
                        const char *salt, bool grant_known) {
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
      "[[account]]\n"
      "username = \"alice\"\n"
      "password_iterations = 600000\n"
      "password_salt_hex = \"%s\"\n"
      "password_hash_hex = \"cae9c801374596f17de48ba4ed7061692b5d0ab433932a7d3cdf698dfd8bb3e8\"\n"
      "databases = [\"%s\"]\n"
      "default_database = \"%s\"\n",
      TEST_TLS_CERT, TEST_TLS_KEY, extra_server ? extra_server : "",
      portable_database, salt, grant_known ? "tenant" : "missing", account_default);
  const salts_fs_buf_t bytes = {text, length > 0 ? (size_t)length : 0u};
  const int result = length <= 0 || length >= TEST_CONFIG_BYTES ? -1
      : salts_fs_write_file(portable_config, &bytes);
  free(text); free(portable_config); free(portable_database);
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
}
