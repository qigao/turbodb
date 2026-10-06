#ifndef TIDESSQL_TESTS_DAEMON_PROCESS_FIXTURE_H
#define TIDESSQL_TESTS_DAEMON_PROCESS_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

typedef struct tidessqld_test_daemon {
#if defined(_WIN32)
  void *process;
  void *thread;
  void *log_read;
#else
  int pid;
  int log_read;
#endif
} tidessqld_test_daemon;

const char *tidessqld_test_executable(const char *fallback);
char *tidessqld_test_join_path(const char *base, const char *leaf);
int tidessqld_test_write_config(
    const char *path, const char *database,
    const char *certificate_file, const char *private_key_file);
int tidessqld_test_run(
    const char *executable, const char *const *arguments,
    size_t argument_count, const char *environment_name,
    const char *environment_value, uint32_t timeout_ms,
    char *output, size_t output_capacity, int *exit_code);
int tidessqld_test_daemon_start(
    tidessqld_test_daemon *daemon,
    const char *executable, const char *config);
int tidessqld_test_daemon_wait_for_port(
    tidessqld_test_daemon *daemon,
    uint32_t timeout_ms, uint16_t *port);
int tidessqld_test_daemon_read(
    tidessqld_test_daemon *daemon, char *output,
    size_t output_capacity, size_t *output_size);
int tidessqld_test_daemon_stop(
    tidessqld_test_daemon *daemon, uint32_t timeout_ms);
void tidessqld_test_daemon_force_cleanup(
    tidessqld_test_daemon *daemon, uint32_t timeout_ms);

#endif
