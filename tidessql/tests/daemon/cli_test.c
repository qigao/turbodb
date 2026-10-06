#include "process_fixture.h"

#include <tinytest.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  TEST_TIMEOUT_MS = 15000,
  TEST_OUTPUT_BYTES = 4096
};

static int run_cli(const char *const *arguments, size_t argument_count,
                   const char *environment_name,
                   const char *environment_value, char *output,
                   int *exit_code) {
  return tidessqld_test_run(
      tidessqld_test_executable(TEST_TIDESSQLD), arguments, argument_count,
      environment_name, environment_value, TEST_TIMEOUT_MS, output,
      TEST_OUTPUT_BYTES, exit_code);
}

static int has_hex_field(const char *output, const char *prefix,
                         size_t digits) {
  const char *value = strstr(output, prefix);
  if (value == NULL) return 0;
  value += strlen(prefix);
  for (size_t i = 0u; i < digits; ++i)
    if (!isxdigit((unsigned char)value[i]) ||
        (value[i] >= 'A' && value[i] <= 'F'))
      return 0;
  if (value[digits] != '"') return 0;
  return value[digits + 1u] == '\n' ||
         (value[digits + 1u] == '\r' && value[digits + 2u] == '\n');
}

spec("tidessqld command line") {
  it("reports its stable commands and version") {
    const char *help_arguments[] = {"--help"};
    const char *version_arguments[] = {"--version"};
    char output[TEST_OUTPUT_BYTES];
    int exit_code = -1;

    check_equal(run_cli(help_arguments, 1u, NULL, NULL, output, &exit_code),
                0);
    check_equal(exit_code, 0);
    check_contains(output, "Usage:");
    check_contains(output, "serve --config");
    check_contains(output, "check-config --config");
    check_contains(output, "hash-password --password-env");

    check_equal(
        run_cli(version_arguments, 1u, NULL, NULL, output, &exit_code), 0);
    check_equal(exit_code, 0);
    check_true(strncmp(output, "tidessqld ", strlen("tidessqld ")) == 0);
  }

  it("checks valid configuration and reports missing files") {
    char *directory = tt_make_temp_dir("tidessqld-cli-config");
    check_not_null(directory);
    char *config = tidessqld_test_join_path(directory, "tidessqld.toml");
    char *database = tidessqld_test_join_path(directory, "database");
    char *missing = tidessqld_test_join_path(directory, "missing.toml");
    check_not_null(config);
    check_not_null(database);
    check_not_null(missing);
    check_equal(tidessqld_test_write_config(
                    config, database, TEST_TLS_CERT, TEST_TLS_KEY),
                0);
    const char *valid_arguments[] = {"check-config", "--config", config};
    const char *missing_arguments[] = {"check-config", "--config", missing};
    char output[TEST_OUTPUT_BYTES];
    int exit_code = -1;

    check_equal(
        run_cli(valid_arguments, 3u, NULL, NULL, output, &exit_code), 0);
    check_equal(exit_code, 0);
    check_true(strcmp(output, "configuration is valid\n") == 0 ||
               strcmp(output, "configuration is valid\r\n") == 0);

    check_equal(
        run_cli(missing_arguments, 3u, NULL, NULL, output, &exit_code), 0);
    check_equal(exit_code, 1);
    check_greater(strlen(output), 0u);
    check_null(strstr(output, "configuration is valid"));

    const int removed = tt_remove_tree(directory);
    free(missing);
    free(database);
    free(config);
    free(directory);
    check_equal(removed, 0);
  }

  it("rejects unknown duplicate and incomplete options") {
    const char *unknown_arguments[] = {"unknown"};
    const char *duplicate_arguments[] = {
        "check-config", "--config", "first", "--config", "second"};
    const char *incomplete_arguments[] = {"check-config", "--config"};
    const char *const *cases[] = {
        unknown_arguments, duplicate_arguments, incomplete_arguments};
    const size_t counts[] = {1u, 5u, 2u};
    char output[TEST_OUTPUT_BYTES];

    for (size_t i = 0u; i < sizeof(cases) / sizeof(cases[0]); ++i) {
      int exit_code = -1;
      check_equal(run_cli(cases[i], counts[i], NULL, NULL, output,
                          &exit_code),
                  0);
      check_equal(exit_code, 2);
      check_contains(output, "Usage:");
    }
  }

  it("hashes an environment password without echoing the secret") {
    static const char environment_name[] =
        "TIDESSQLD_CLI_TEST_PASSWORD_7ED87D09";
    static const char password[] = "cli-secret-password";
    const char *arguments[] = {"hash-password", "--password-env",
                               environment_name, "--iterations", "600000"};
    char output[TEST_OUTPUT_BYTES];
    int exit_code = -1;

    check_equal(run_cli(arguments, 5u, environment_name, password, output,
                        &exit_code),
                0);
    check_equal(exit_code, 0);
    check_contains(output, "password_iterations = 600000");
    check_true(has_hex_field(output, "password_salt_hex = \"", 32u));
    check_true(has_hex_field(output, "password_hash_hex = \"", 64u));
    check_null(strstr(output, password));

    check_equal(run_cli(arguments, 5u, environment_name, NULL, output,
                        &exit_code),
                0);
    check_equal(exit_code, 2);
    check_contains(output, "password environment is empty");
    check_null(strstr(output, "password_hash_hex"));
  }

  it("rejects invalid password iterations before producing a verifier") {
    static const char environment_name[] =
        "TIDESSQLD_CLI_TEST_PASSWORD_04DF96B7";
    const char *arguments[] = {"hash-password", "--password-env",
                               environment_name, "--iterations", "1"};
    char output[TEST_OUTPUT_BYTES];
    int exit_code = -1;

    check_equal(run_cli(arguments, 5u, environment_name, "unused-secret",
                        output, &exit_code),
                0);
    check_equal(exit_code, 2);
    check_contains(output, "iterations must be between");
    check_null(strstr(output, "password_hash_hex"));
  }
}
