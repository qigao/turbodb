#include <session_async.h>
#include <session_transaction.h>
#include <salts/clock.h>
#include <salts/thread.h>
#include <salts_fs.h>
#include <tinytest.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

enum { TEST_CONFIG_BYTES = 16384, TEST_LOG_BYTES = 4096, TEST_TIMEOUT_MS = 15000 };
static const char password[] = "test-password";

typedef struct daemon_process {
#if defined(_WIN32)
  PROCESS_INFORMATION process;
  HANDLE log_read;
#else
  pid_t pid;
  int log_read;
#endif
} daemon_process;

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

static int write_config(const char *path, const char *database) {
  char *portable_config = portable_path(path), *portable_database = portable_path(database);
  if (!portable_config || !portable_database) { free(portable_config); free(portable_database); return -1; }
  char *text = (char *)malloc(TEST_CONFIG_BYTES);
  if (!text) { free(portable_config); free(portable_database); return -1; }
  const int length = snprintf(text, TEST_CONFIG_BYTES,
      "version = 1\n"
      "[server]\n"
      "host = \"127.0.0.1\"\n"
      "port = 0\n"
      "poll_timeout_ms = 1\n"
      "shutdown_timeout_ms = 5000\n"
      "certificate_file = \"%s\"\n"
      "private_key_file = \"%s\"\n"
      "[[database]]\n"
      "name = \"tenant\"\n"
      "path = \"%s\"\n"
      "column_family = \"server\"\n"
      "initialize = true\n"
      "[[account]]\n"
      "username = \"alice\"\n"
      "password_iterations = 600000\n"
      "password_salt_hex = \"000102030405060708090a0b0c0d0e0f\"\n"
      "password_hash_hex = \"cae9c801374596f17de48ba4ed7061692b5d0ab433932a7d3cdf698dfd8bb3e8\"\n"
      "databases = [\"tenant\"]\n"
      "default_database = \"tenant\"\n", TEST_TLS_CERT, TEST_TLS_KEY, portable_database);
  const salts_fs_buf_t bytes = {text, length > 0 ? (size_t)length : 0u};
  const int result = length <= 0 || length >= TEST_CONFIG_BYTES ? -1
      : salts_fs_write_file(portable_config, &bytes);
  free(text); free(portable_database); free(portable_config);
  return result;
}

#if defined(_WIN32)
static int daemon_start(daemon_process *daemon, const char *config) {
  SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
  HANDLE write_end = NULL;
  if (!CreatePipe(&daemon->log_read, &write_end, &security, 0) ||
      !SetHandleInformation(daemon->log_read, HANDLE_FLAG_INHERIT, 0)) return -1;
  STARTUPINFOA startup = {0}; startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE); startup.hStdOutput = write_end;
  startup.hStdError = write_end;
  const size_t command_size = strlen(TEST_TIDESSQLD) + strlen(config) + 32u;
  char *command = (char *)malloc(command_size);
  if (!command) { CloseHandle(write_end); CloseHandle(daemon->log_read); return -1; }
  (void)snprintf(command, command_size, "\"%s\" serve --config \"%s\"", TEST_TIDESSQLD, config);
  const BOOL started = CreateProcessA(TEST_TIDESSQLD, command, NULL, NULL, TRUE,
      CREATE_NO_WINDOW, NULL, NULL, &startup, &daemon->process);
  free(command); CloseHandle(write_end);
  if (!started) { CloseHandle(daemon->log_read); daemon->log_read = NULL; return -1; }
  return 0;
}
static int daemon_read(daemon_process *daemon, char *output, size_t capacity, size_t *size) {
  DWORD available = 0;
  if (!PeekNamedPipe(daemon->log_read, NULL, 0, NULL, &available, NULL)) return -1;
  if (!available) return 0;
  const size_t room = capacity - *size - 1u;
  DWORD read_size = 0, request = available < room ? available : (DWORD)room;
  if (!request || !ReadFile(daemon->log_read, output + *size, request, &read_size, NULL)) return -1;
  *size += read_size; output[*size] = 0; return 1;
}
static int daemon_running(daemon_process *daemon) {
  return WaitForSingleObject(daemon->process.hProcess, 0) == WAIT_TIMEOUT;
}
static int daemon_stop(daemon_process *daemon) {
  /* CTest has no shared Windows console, so its isolated child cannot receive
   * GenerateConsoleCtrlEvent. Runtime cleanup is covered by config_test. */
  if (!TerminateProcess(daemon->process.hProcess, 0)) return -1;
  if (WaitForSingleObject(daemon->process.hProcess, TEST_TIMEOUT_MS) != WAIT_OBJECT_0) return -1;
  CloseHandle(daemon->log_read); CloseHandle(daemon->process.hThread); CloseHandle(daemon->process.hProcess);
  *daemon = (daemon_process){0}; return 0;
}
static void daemon_force_cleanup(daemon_process *daemon) {
  if (daemon->process.hProcess) {
    (void)TerminateProcess(daemon->process.hProcess, 1);
    (void)WaitForSingleObject(daemon->process.hProcess, TEST_TIMEOUT_MS);
    CloseHandle(daemon->process.hThread); CloseHandle(daemon->process.hProcess);
  }
  if (daemon->log_read) CloseHandle(daemon->log_read);
  *daemon = (daemon_process){0};
}
#else
static int daemon_start(daemon_process *daemon, const char *config) {
  int pipes[2]; if (pipe(pipes) != 0) return -1;
  const pid_t child = fork();
  if (child < 0) { close(pipes[0]); close(pipes[1]); return -1; }
  if (child == 0) {
    (void)dup2(pipes[1], STDOUT_FILENO); (void)dup2(pipes[1], STDERR_FILENO);
    close(pipes[0]); close(pipes[1]);
    execl(TEST_TIDESSQLD, TEST_TIDESSQLD, "serve", "--config", config, (char *)NULL);
    _exit(127);
  }
  close(pipes[1]); daemon->pid = child; daemon->log_read = pipes[0];
  (void)fcntl(daemon->log_read, F_SETFL, fcntl(daemon->log_read, F_GETFL) | O_NONBLOCK);
  return 0;
}
static int daemon_read(daemon_process *daemon, char *output, size_t capacity, size_t *size) {
  const ssize_t count = read(daemon->log_read, output + *size, capacity - *size - 1u);
  if (count > 0) { *size += (size_t)count; output[*size] = 0; return 1; }
  return count < 0 && errno != EAGAIN && errno != EWOULDBLOCK ? -1 : 0;
}
static int daemon_running(daemon_process *daemon) {
  int status = 0; return waitpid(daemon->pid, &status, WNOHANG) == 0;
}
static int daemon_stop(daemon_process *daemon) {
  if (kill(daemon->pid, SIGTERM) != 0) return -1;
  const uint64_t deadline = salts_monotonic_ms() + TEST_TIMEOUT_MS;
  int status = 0;
  while (salts_monotonic_ms() < deadline) {
    if (waitpid(daemon->pid, &status, WNOHANG) == daemon->pid) {
      close(daemon->log_read); *daemon = (daemon_process){0};
      return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
    }
    salts_sleep_ms(10);
  }
  return -1;
}
static void daemon_force_cleanup(daemon_process *daemon) {
  if (daemon->pid > 0) { (void)kill(daemon->pid, SIGKILL); (void)waitpid(daemon->pid, NULL, 0); }
  if (daemon->log_read > 0) close(daemon->log_read);
  *daemon = (daemon_process){0};
}
#endif

static int wait_for_port(daemon_process *daemon, uint16_t *port) {
  char log[TEST_LOG_BYTES] = {0}; size_t size = 0;
  const uint64_t deadline = salts_monotonic_ms() + TEST_TIMEOUT_MS;
  while (salts_monotonic_ms() < deadline && daemon_running(daemon)) {
    if (daemon_read(daemon, log, sizeof(log), &size) < 0) return -1;
    const char *marker = strstr(log, "port=");
    if (marker) {
      char *end = NULL; const unsigned long value = strtoul(marker + 5, &end, 10);
      if (end != marker + 5 && value > 0 && value <= UINT16_MAX) { *port = (uint16_t)value; return 0; }
    }
    salts_sleep_ms(10);
  }
  return -1;
}

spec("standalone tidessqld process with the existing MySQL driver") {
  it("serves a TLS prepared query from an isolated daemon process") {
    char *directory = tt_make_temp_dir("tidessqld-process"); check_not_null(directory);
    char *config = join_path(directory, "tidessqld.toml");
    char *database = join_path(directory, "database");
    check_not_null(config); check_not_null(database); check_equal(write_config(config, database), 0);
    daemon_process daemon = {0}; const int started = daemon_start(&daemon, config);
    uint16_t port = 0; const int ready = started == 0 ? wait_for_port(&daemon, &port) : -1;
    mysql_session_config_t client = {.host="127.0.0.1", .port=port, .username="alice",
      .password=password, .database="tenant", .ca_file=TEST_TLS_CA, .server_name="localhost",
      .timeout_ms=5000};
    mysql_session_cursor_limits_t limits = {.max_result_rows=8, .max_columns=8,
      .max_metadata_bytes=65536, .max_row_bytes=65536, .max_command_bytes=65536};
    mysql_session_error_t client_error = {0};
    mysql_transaction_session_t *transaction = NULL;
    mysql_session_command_result_t command = {0};
    mysql_session_status_t transaction_status = ready == 0
        ? mysql_transaction_session_begin(&client, MYSQL_ISOLATION_SERIALIZABLE, 65536,
                                          &transaction, &client_error)
        : MYSQL_SESSION_INVALID_STATE;
    static const char create_sql[] = "CREATE TABLE process_items(id BIGINT PRIMARY KEY,value BIGINT NOT NULL)";
    static const char insert_sql[] = "INSERT INTO process_items VALUES(1,40)";
    static const char update_sql[] = "UPDATE process_items SET value=42 WHERE id=1";
    static const uint8_t savepoint[] = "verified";
    if (transaction_status == MYSQL_SESSION_OK)
      transaction_status = mysql_transaction_session_execute_prepared(transaction,
          (const uint8_t *)create_sql, sizeof(create_sql)-1u, NULL, 0, &command, &client_error);
    if (transaction_status == MYSQL_SESSION_OK)
      transaction_status = mysql_transaction_session_execute_prepared(transaction,
          (const uint8_t *)insert_sql, sizeof(insert_sql)-1u, NULL, 0, &command, &client_error);
    if (transaction_status == MYSQL_SESSION_OK)
      transaction_status = mysql_transaction_session_savepoint(transaction, savepoint,
          sizeof(savepoint)-1u, &client_error);
    if (transaction_status == MYSQL_SESSION_OK)
      transaction_status = mysql_transaction_session_execute_prepared(transaction,
          (const uint8_t *)update_sql, sizeof(update_sql)-1u, NULL, 0, &command, &client_error);
    if (transaction_status == MYSQL_SESSION_OK)
      transaction_status = mysql_transaction_session_release_savepoint(transaction, savepoint,
          sizeof(savepoint)-1u, &client_error);
    if (transaction_status == MYSQL_SESSION_OK)
      transaction_status = mysql_transaction_session_commit(transaction, &client_error);
    mysql_transaction_session_destroy(transaction); transaction = NULL;
    mysql_async_source source = {0};
    const uint8_t query[] = "SELECT value FROM process_items WHERE id=1";
    const mysql_session_status_t opened = transaction_status == MYSQL_SESSION_OK
        ? mysql_session_start_async_source(&client, query, sizeof(query)-1u, NULL, 0,
                                            &limits, &source, &client_error)
        : MYSQL_SESSION_INVALID_STATE;
    bool metadata = false, row = false, done = false, failed = false;
    mysql_session_status_t step_status = MYSQL_SESSION_OK;
    char step_message[256] = {0};
    const uint64_t deadline = salts_monotonic_ms() + TEST_TIMEOUT_MS;
    while (opened == MYSQL_SESSION_OK && !done && salts_monotonic_ms() < deadline) {
      const mysql_async_step step = mysql_session_async_next(&source);
      if (step.kind == MYSQL_ASYNC_METADATA) metadata = step.column_count == 1u;
      else if (step.kind == MYSQL_ASYNC_ROW) row = step.row_size > 0u;
      else if (step.kind == MYSQL_ASYNC_DONE) done = true;
      else if (step.kind == MYSQL_ASYNC_ERROR) {
        failed = true; step_status = step.status;
        if (step.message) (void)snprintf(step_message, sizeof(step_message), "%s", step.message);
        break;
      }
      if (!done) salts_sleep_ms(1);
    }
    const mysql_session_status_t closed = source.context
        ? mysql_session_async_close(&source, &client_error) : MYSQL_SESSION_OK;
    const int stopped = started == 0 ? daemon_stop(&daemon) : -1;
    daemon_force_cleanup(&daemon);
    const int removed = tt_remove_tree(directory);
    if (failed || opened != MYSQL_SESSION_OK || closed != MYSQL_SESSION_OK)
      (void)fprintf(stderr, "client status=%d step=%d stage=%s message=%s step_message=%s\n",
                    (int)client_error.status, (int)step_status, client_error.stage,
                    client_error.message, step_message);
    free(database); free(config); free(directory);
    check_equal(started, 0); check_equal(ready, 0); check_greater(port, 0u);
    check_equal(transaction_status, MYSQL_SESSION_OK);
    check_equal(opened, MYSQL_SESSION_OK); check_false(failed);
    check_true(metadata); check_true(row); check_true(done);
    check_equal(closed, MYSQL_SESSION_OK);
    check_equal(stopped, 0);
    check_equal(removed, 0);
  }
}
