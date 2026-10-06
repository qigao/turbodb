#include "process_fixture.h"

#include <tinytest.h>

#include <stdio.h>
#include <stdlib.h>

enum {
  TEST_TIMEOUT_MS = 30000,
  TEST_OUTPUT_BYTES = 65536
};

spec("standalone tidessqld process with MySQL Connector/J") {
  it("serves an authenticated TLS transaction through the real connector") {
    char *directory = tt_make_temp_dir("tidessqld-connector-j");
    check_not_null(directory);
    char *config = tidessqld_test_join_path(directory, "tidessqld.toml");
    char *database = tidessqld_test_join_path(directory, "database");
    check_not_null(config);
    check_not_null(database);
    check_equal(tidessqld_test_write_config(
                    config, database, TEST_TLS_CERT, TEST_TLS_KEY),
                0);

    tidessqld_test_daemon daemon = {0};
    const int started = tidessqld_test_daemon_start(
        &daemon, tidessqld_test_executable(TEST_TIDESSQLD), config);
    uint16_t port = 0;
    const int ready = started == 0
                          ? tidessqld_test_daemon_wait_for_port(
                                &daemon, TEST_TIMEOUT_MS, &port)
                          : -1;
    char port_text[16] = {0};
    (void)snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);
    const char *arguments[] = {
        "-Djdk.tls.client.enableStatusRequestExtension=false",
        "--class-path", TEST_CONNECTOR_J, TEST_CONNECTOR_J_PROBE,
        port_text, TEST_TLS_CA};
    char output[TEST_OUTPUT_BYTES] = {0};
    int exit_code = -1;
    const int invoked = ready == 0
                            ? tidessqld_test_run(
                                  TEST_JAVA, arguments,
                                  sizeof(arguments) / sizeof(arguments[0]),
                                  NULL, NULL, TEST_TIMEOUT_MS,
                                  output, sizeof(output), &exit_code)
                            : -1;
    char daemon_output[TEST_OUTPUT_BYTES] = {0};
    size_t daemon_output_size = 0;
    const int daemon_read = started == 0
                                ? tidessqld_test_daemon_read(
                                      &daemon, daemon_output,
                                      sizeof(daemon_output),
                                      &daemon_output_size)
                                : -1;
    const int stopped = started == 0
                            ? tidessqld_test_daemon_stop(
                                  &daemon, TEST_TIMEOUT_MS)
                            : -1;
    tidessqld_test_daemon_force_cleanup(&daemon, TEST_TIMEOUT_MS);
    const int removed = tt_remove_tree(directory);
    if (invoked != 0 || exit_code != 0)
      (void)fprintf(stderr,
                    "Connector/J probe exit=%d output=%s\n"
                    "tidessqld read=%d output=%s\n",
                    exit_code, output, daemon_read, daemon_output);
    free(database);
    free(config);
    free(directory);

    check_equal(started, 0);
    check_equal(ready, 0);
    check_greater(port, 0u);
    check_equal(invoked, 0);
    check_equal(exit_code, 0);
    check_contains(output, "connector-j-ok");
    check_equal(stopped, 0);
    check_equal(removed, 0);
  }
}
