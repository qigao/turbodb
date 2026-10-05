#include "process_fixture.h"

#include <salts/clock.h>
#include <salts/thread.h>
#include <salts_fs.h>

#include <errno.h>
#include <stdint.h>
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

enum {
  TIDESSQLD_TEST_CONFIG_BYTES = 16384,
  TIDESSQLD_TEST_LOG_BYTES = 4096
};

static char *portable_path(const char *path) {
  size_t length;
  char *copy;
  if (path == NULL) return NULL;
  length = strlen(path);
  if (length == SIZE_MAX) return NULL;
  copy = (char *)malloc(length + 1u);
  if (copy == NULL) return NULL;
  memcpy(copy, path, length + 1u);
  for (size_t i = 0u; i < length; ++i)
    if (copy[i] == '\\') copy[i] = '/';
  return copy;
}

char *tidessqld_test_join_path(const char *base, const char *leaf) {
  size_t base_size, leaf_size, capacity;
  char *path;
  if (base == NULL || leaf == NULL) return NULL;
  base_size = strlen(base);
  leaf_size = strlen(leaf);
  if (base_size > SIZE_MAX - 2u ||
      leaf_size > SIZE_MAX - base_size - 2u)
    return NULL;
  capacity = base_size + leaf_size + 2u;
  path = (char *)malloc(capacity);
  if (path != NULL) (void)snprintf(path, capacity, "%s/%s", base, leaf);
  return path;
}

int tidessqld_test_write_config(
    const char *path, const char *database,
    const char *certificate_file, const char *private_key_file) {
  char *config = portable_path(path), *data = portable_path(database);
  char *certificate = portable_path(certificate_file), *key = portable_path(private_key_file);
  char *text = NULL;
  int result = -1, length;
  if (config == NULL || data == NULL || certificate == NULL || key == NULL) goto cleanup;
  text = (char *)malloc(TIDESSQLD_TEST_CONFIG_BYTES);
  if (text == NULL) goto cleanup;
  length = snprintf(text, TIDESSQLD_TEST_CONFIG_BYTES,
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
      "default_database = \"tenant\"\n",
      certificate, key, data);
  if (length > 0 && length < TIDESSQLD_TEST_CONFIG_BYTES) {
    const salts_fs_buf_t bytes = {text, (size_t)length};
    result = salts_fs_write_file(config, &bytes);
  }
cleanup:
  free(text); free(key); free(certificate); free(data); free(config);
  return result;
}

#if defined(_WIN32)
int tidessqld_test_daemon_start(
    tidessqld_test_daemon *daemon, const char *executable, const char *config) {
  SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
  HANDLE read_end = NULL, write_end = NULL;
  STARTUPINFOA startup = {0}; PROCESS_INFORMATION process = {0};
  size_t executable_size, config_size, command_size;
  char *command;
  BOOL started;
  if (daemon == NULL || executable == NULL || config == NULL) return -1;
  *daemon = (tidessqld_test_daemon){0};
  if (!CreatePipe(&read_end, &write_end, &security, 0) ||
      !SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0)) {
    if (read_end != NULL) CloseHandle(read_end);
    if (write_end != NULL) CloseHandle(write_end);
    return -1;
  }
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = write_end; startup.hStdError = write_end;
  executable_size = strlen(executable);
  config_size = strlen(config);
  if (executable_size > SIZE_MAX - 32u ||
      config_size > SIZE_MAX - executable_size - 32u) {
    CloseHandle(write_end); CloseHandle(read_end); return -1;
  }
  command_size = executable_size + config_size + 32u;
  command = (char *)malloc(command_size);
  if (command == NULL) { CloseHandle(write_end); CloseHandle(read_end); return -1; }
  (void)snprintf(command, command_size,
                 "\"%s\" serve --config \"%s\"", executable, config);
  started = CreateProcessA(executable, command, NULL, NULL, TRUE,
      CREATE_NO_WINDOW, NULL, NULL, &startup, &process);
  free(command); CloseHandle(write_end);
  if (!started) { CloseHandle(read_end); return -1; }
  daemon->process = process.hProcess; daemon->thread = process.hThread;
  daemon->log_read = read_end; return 0;
}
static int daemon_read(tidessqld_test_daemon *daemon, char *output,
                       size_t capacity, size_t *size) {
  DWORD available = 0, read_size = 0, request; size_t room;
  if (!PeekNamedPipe((HANDLE)daemon->log_read, NULL, 0, NULL, &available, NULL)) return -1;
  if (available == 0u) return 0;
  room = capacity - *size - 1u; request = available < room ? available : (DWORD)room;
  if (request == 0u || !ReadFile((HANDLE)daemon->log_read, output + *size,
                                  request, &read_size, NULL)) return -1;
  *size += read_size; output[*size] = 0; return 1;
}
static int daemon_running(tidessqld_test_daemon *daemon) {
  return WaitForSingleObject((HANDLE)daemon->process, 0) == WAIT_TIMEOUT;
}
int tidessqld_test_daemon_stop(tidessqld_test_daemon *daemon, uint32_t timeout_ms) {
  if (daemon == NULL || daemon->process == NULL) return -1;
  /* CTest has no shared console. Runtime tests cover bounded graceful cleanup. */
  if (!TerminateProcess((HANDLE)daemon->process, 0) ||
      WaitForSingleObject((HANDLE)daemon->process, timeout_ms) != WAIT_OBJECT_0) return -1;
  CloseHandle((HANDLE)daemon->log_read); CloseHandle((HANDLE)daemon->thread);
  CloseHandle((HANDLE)daemon->process); *daemon = (tidessqld_test_daemon){0}; return 0;
}
void tidessqld_test_daemon_force_cleanup(tidessqld_test_daemon *daemon, uint32_t timeout_ms) {
  if (daemon == NULL) return;
  if (daemon->process != NULL) {
    (void)TerminateProcess((HANDLE)daemon->process, 1);
    (void)WaitForSingleObject((HANDLE)daemon->process, timeout_ms);
    CloseHandle((HANDLE)daemon->thread); CloseHandle((HANDLE)daemon->process);
  }
  if (daemon->log_read != NULL) CloseHandle((HANDLE)daemon->log_read);
  *daemon = (tidessqld_test_daemon){0};
}
#else
int tidessqld_test_daemon_start(
    tidessqld_test_daemon *daemon, const char *executable, const char *config) {
  int pipes[2];
  int flags;
  pid_t child;
  if (daemon == NULL || executable == NULL || config == NULL) return -1;
  *daemon = (tidessqld_test_daemon){0};
  if (pipe(pipes) != 0) return -1;
  child = fork();
  if (child < 0) { close(pipes[0]); close(pipes[1]); return -1; }
  if (child == 0) {
    (void)dup2(pipes[1], STDOUT_FILENO); (void)dup2(pipes[1], STDERR_FILENO);
    close(pipes[0]); close(pipes[1]);
    execl(executable, executable, "serve", "--config", config, (char *)NULL); _exit(127);
  }
  close(pipes[1]);
  daemon->pid = (int)child;
  daemon->log_read = pipes[0];
  flags = fcntl(daemon->log_read, F_GETFL);
  if (flags < 0 ||
      fcntl(daemon->log_read, F_SETFL, flags | O_NONBLOCK) != 0) {
    (void)kill(child, SIGKILL);
    (void)waitpid(child, NULL, 0);
    close(daemon->log_read);
    *daemon = (tidessqld_test_daemon){0};
    return -1;
  }
  return 0;
}
static int daemon_read(tidessqld_test_daemon *daemon, char *output,
                       size_t capacity, size_t *size) {
  const ssize_t count = read(daemon->log_read, output + *size, capacity - *size - 1u);
  if (count > 0) { *size += (size_t)count; output[*size] = 0; return 1; }
  return count < 0 && errno != EAGAIN && errno != EWOULDBLOCK ? -1 : 0;
}
static int daemon_running(tidessqld_test_daemon *daemon) {
  int status = 0; return waitpid((pid_t)daemon->pid, &status, WNOHANG) == 0;
}
int tidessqld_test_daemon_stop(tidessqld_test_daemon *daemon, uint32_t timeout_ms) {
  uint64_t deadline; int status = 0;
  if (daemon == NULL || daemon->pid <= 0 || kill((pid_t)daemon->pid, SIGTERM) != 0) return -1;
  deadline = salts_monotonic_ms() + timeout_ms;
  while (salts_monotonic_ms() < deadline) {
    if (waitpid((pid_t)daemon->pid, &status, WNOHANG) == (pid_t)daemon->pid) {
      close(daemon->log_read); *daemon = (tidessqld_test_daemon){0};
      return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
    }
    salts_sleep_ms(10);
  }
  return -1;
}
void tidessqld_test_daemon_force_cleanup(tidessqld_test_daemon *daemon, uint32_t timeout_ms) {
  (void)timeout_ms; if (daemon == NULL) return;
  if (daemon->pid > 0) { (void)kill((pid_t)daemon->pid, SIGKILL); (void)waitpid((pid_t)daemon->pid, NULL, 0); }
  if (daemon->log_read > 0) close(daemon->log_read);
  *daemon = (tidessqld_test_daemon){0};
}
#endif

int tidessqld_test_daemon_wait_for_port(
    tidessqld_test_daemon *daemon, uint32_t timeout_ms, uint16_t *port) {
  char log[TIDESSQLD_TEST_LOG_BYTES] = {0}; size_t size = 0u;
  uint64_t deadline;
  if (daemon == NULL || port == NULL || timeout_ms == 0u) return -1;
  *port = 0u; deadline = salts_monotonic_ms() + timeout_ms;
  while (salts_monotonic_ms() < deadline && daemon_running(daemon)) {
    const char *marker; char *end = NULL; unsigned long value;
    if (daemon_read(daemon, log, sizeof(log), &size) < 0) return -1;
    marker = strstr(log, "port=");
    if (marker != NULL) {
      value = strtoul(marker + 5, &end, 10);
      if (end != marker + 5 && value > 0u && value <= UINT16_MAX) {
        *port = (uint16_t)value; return 0;
      }
    }
    salts_sleep_ms(10);
  }
  return -1;
}
