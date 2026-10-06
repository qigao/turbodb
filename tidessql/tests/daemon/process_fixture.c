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
  TIDESSQLD_TEST_LOG_BYTES = 4096,
  TIDESSQLD_TEST_READ_BYTES = 512
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

const char *tidessqld_test_executable(const char *fallback) {
  const char *override = getenv("TURBODB_TEST_TIDESSQLD");
  salts_fs_stat_t file = {0};
  if (override == NULL || override[0] == '\0') return fallback;
  if (!salts_fs_path_is_absolute(override) ||
      salts_fs_stat(override, &file) != 0 || !file.is_file)
    return NULL;
  return override;
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
static int command_append(char *command, size_t capacity, size_t *size,
                          char character) {
  if (*size >= capacity - 1u) return -1;
  command[(*size)++] = character;
  command[*size] = 0;
  return 0;
}

static int command_append_argument(char *command, size_t capacity,
                                   size_t *size, const char *argument) {
  size_t slashes = 0u;
  if (*size != 0u && command_append(command, capacity, size, ' ') != 0)
    return -1;
  if (command_append(command, capacity, size, '"') != 0) return -1;
  for (const char *current = argument;; ++current) {
    if (*current == '\\') {
      ++slashes;
      continue;
    }
    if (*current == '"' || *current == 0) {
      const size_t count = slashes * 2u + (*current == '"' ? 1u : 0u);
      for (size_t i = 0u; i < count; ++i)
        if (command_append(command, capacity, size, '\\') != 0) return -1;
      slashes = 0u;
      if (*current == 0)
        return command_append(command, capacity, size, '"');
      if (command_append(command, capacity, size, '"') != 0) return -1;
      continue;
    }
    for (size_t i = 0u; i < slashes; ++i)
      if (command_append(command, capacity, size, '\\') != 0) return -1;
    slashes = 0u;
    if (command_append(command, capacity, size, *current) != 0) return -1;
  }
}

static char *make_command(const char *executable,
                          const char *const *arguments,
                          size_t argument_count) {
  size_t capacity = 1u, size = 0u;
  for (size_t i = 0u; i <= argument_count; ++i) {
    const char *argument = i == 0u ? executable : arguments[i - 1u];
    const size_t length = strlen(argument);
    if (capacity > SIZE_MAX - 4u ||
        length > (SIZE_MAX - capacity - 4u) / 2u)
      return NULL;
    capacity += length * 2u + 4u;
  }
  char *command = (char *)malloc(capacity);
  if (command == NULL) return NULL;
  command[0] = 0;
  if (command_append_argument(command, capacity, &size, executable) != 0) {
    free(command);
    return NULL;
  }
  for (size_t i = 0u; i < argument_count; ++i) {
    if (command_append_argument(command, capacity, &size, arguments[i]) != 0) {
      free(command);
      return NULL;
    }
  }
  return command;
}

static int capture_read(HANDLE read_end, char *output,
                        size_t output_capacity, size_t *output_size) {
  char buffer[TIDESSQLD_TEST_READ_BYTES];
  DWORD available = 0u;
  if (!PeekNamedPipe(read_end, NULL, 0, NULL, &available, NULL))
    return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
  while (available != 0u) {
    DWORD count = 0u;
    const DWORD requested =
        available < sizeof(buffer) ? available : (DWORD)sizeof(buffer);
    if (!ReadFile(read_end, buffer, requested, &count, NULL))
      return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    const size_t room = output_capacity - *output_size - 1u;
    const size_t copied = count < room ? (size_t)count : room;
    if (copied != 0u) {
      memcpy(output + *output_size, buffer, copied);
      *output_size += copied;
      output[*output_size] = 0;
    }
    if (!PeekNamedPipe(read_end, NULL, 0, NULL, &available, NULL))
      return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
  }
  return 0;
}

int tidessqld_test_run(
    const char *executable, const char *const *arguments,
    size_t argument_count, const char *environment_name,
    const char *environment_value, uint32_t timeout_ms,
    char *output, size_t output_capacity, int *exit_code) {
  SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
  HANDLE read_end = NULL, write_end = NULL;
  STARTUPINFOA startup = {0};
  PROCESS_INFORMATION process = {0};
  char *command = NULL, *previous = NULL;
  size_t previous_size = 0u, output_size = 0u;
  BOOL started;
  int result = -1;
  if (executable == NULL || (argument_count != 0u && arguments == NULL) ||
      timeout_ms == 0u || output == NULL || output_capacity == 0u ||
      exit_code == NULL ||
      (environment_name == NULL && environment_value != NULL) ||
      (environment_name != NULL && environment_name[0] == 0))
    return -1;
  for (size_t i = 0u; i < argument_count; ++i)
    if (arguments[i] == NULL) return -1;
  output[0] = 0;
  *exit_code = -1;
  command = make_command(executable, arguments, argument_count);
  if (command == NULL ||
      !CreatePipe(&read_end, &write_end, &security, 0) ||
      !SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0))
    goto cleanup;
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = write_end;
  startup.hStdError = write_end;
  if (environment_name != NULL) {
    if (_dupenv_s(&previous, &previous_size, environment_name) != 0 ||
        _putenv_s(environment_name,
                  environment_value != NULL ? environment_value : "") != 0)
      goto cleanup;
  }
  started = CreateProcessA(executable, command, NULL, NULL, TRUE,
                           CREATE_NO_WINDOW, NULL, NULL, &startup, &process);
  if (environment_name != NULL) {
    const int restored = _putenv_s(
        environment_name, previous != NULL ? previous : "");
    if (previous != NULL) {
      SecureZeroMemory(previous, previous_size);
      free(previous);
      previous = NULL;
    }
    if (restored != 0) {
      if (started) {
        (void)TerminateProcess(process.hProcess, 1u);
        (void)WaitForSingleObject(process.hProcess, timeout_ms);
      }
      started = FALSE;
    }
  }
  CloseHandle(write_end);
  write_end = NULL;
  if (!started) goto cleanup;
  const uint64_t deadline = salts_monotonic_ms() + timeout_ms;
  for (;;) {
    DWORD child_code = 0u;
    if (capture_read(read_end, output, output_capacity, &output_size) != 0)
      goto terminate;
    const DWORD wait = WaitForSingleObject(process.hProcess, 0);
    if (wait == WAIT_OBJECT_0) {
      if (capture_read(read_end, output, output_capacity, &output_size) != 0 ||
          !GetExitCodeProcess(process.hProcess, &child_code))
        goto terminate;
      *exit_code = (int)child_code;
      result = 0;
      break;
    }
    if (wait != WAIT_TIMEOUT || salts_monotonic_ms() >= deadline)
      goto terminate;
    salts_sleep_ms(5);
  }
  goto cleanup;

terminate:
  (void)TerminateProcess(process.hProcess, 1u);
  (void)WaitForSingleObject(process.hProcess, timeout_ms);
cleanup:
  if (environment_name != NULL && previous != NULL) {
    (void)_putenv_s(environment_name, previous);
    SecureZeroMemory(previous, previous_size);
    free(previous);
  }
  if (process.hThread != NULL) CloseHandle(process.hThread);
  if (process.hProcess != NULL) CloseHandle(process.hProcess);
  if (write_end != NULL) CloseHandle(write_end);
  if (read_end != NULL) CloseHandle(read_end);
  free(command);
  return result;
}

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
int tidessqld_test_daemon_read(tidessqld_test_daemon *daemon, char *output,
                               size_t capacity, size_t *size) {
  DWORD available = 0, read_size = 0, request; size_t room;
  if (daemon == NULL || daemon->log_read == NULL || output == NULL ||
      size == NULL || capacity == 0u || *size >= capacity)
    return -1;
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
static int capture_read(int read_end, char *output,
                        size_t output_capacity, size_t *output_size) {
  char buffer[TIDESSQLD_TEST_READ_BYTES];
  for (;;) {
    const ssize_t count = read(read_end, buffer, sizeof(buffer));
    if (count > 0) {
      const size_t room = output_capacity - *output_size - 1u;
      const size_t copied = (size_t)count < room ? (size_t)count : room;
      if (copied != 0u) {
        memcpy(output + *output_size, buffer, copied);
        *output_size += copied;
        output[*output_size] = 0;
      }
      continue;
    }
    if (count == 0) return 0;
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
  }
}

int tidessqld_test_run(
    const char *executable, const char *const *arguments,
    size_t argument_count, const char *environment_name,
    const char *environment_value, uint32_t timeout_ms,
    char *output, size_t output_capacity, int *exit_code) {
  int pipes[2] = {-1, -1}, status = 0, result = -1;
  char **child_arguments = NULL;
  pid_t child = -1;
  size_t output_size = 0u;
  if (executable == NULL || (argument_count != 0u && arguments == NULL) ||
      timeout_ms == 0u || output == NULL || output_capacity == 0u ||
      exit_code == NULL ||
      (environment_name == NULL && environment_value != NULL) ||
      (environment_name != NULL && environment_name[0] == 0) ||
      argument_count > (SIZE_MAX / sizeof(*child_arguments)) - 2u)
    return -1;
  for (size_t i = 0u; i < argument_count; ++i)
    if (arguments[i] == NULL) return -1;
  output[0] = 0;
  *exit_code = -1;
  child_arguments =
      (char **)calloc(argument_count + 2u, sizeof(*child_arguments));
  if (child_arguments == NULL || pipe(pipes) != 0) goto cleanup;
  child_arguments[0] = (char *)executable;
  for (size_t i = 0u; i < argument_count; ++i)
    child_arguments[i + 1u] = (char *)arguments[i];
  child = fork();
  if (child < 0) goto cleanup;
  if (child == 0) {
    (void)dup2(pipes[1], STDOUT_FILENO);
    (void)dup2(pipes[1], STDERR_FILENO);
    close(pipes[0]);
    close(pipes[1]);
    if (environment_name != NULL) {
      const int changed = environment_value != NULL
                              ? setenv(environment_name, environment_value, 1)
                              : unsetenv(environment_name);
      if (changed != 0) _exit(126);
    }
    execv(executable, child_arguments);
    _exit(127);
  }
  close(pipes[1]);
  pipes[1] = -1;
  const int flags = fcntl(pipes[0], F_GETFL);
  if (flags < 0 || fcntl(pipes[0], F_SETFL, flags | O_NONBLOCK) != 0)
    goto terminate;
  const uint64_t deadline = salts_monotonic_ms() + timeout_ms;
  for (;;) {
    if (capture_read(pipes[0], output, output_capacity, &output_size) != 0)
      goto terminate;
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) {
      if (capture_read(pipes[0], output, output_capacity, &output_size) != 0)
        goto cleanup;
      *exit_code = WIFEXITED(status) ? WEXITSTATUS(status)
                                     : 128 + WTERMSIG(status);
      child = -1;
      result = 0;
      goto cleanup;
    }
    if (waited < 0 || salts_monotonic_ms() >= deadline) goto terminate;
    salts_sleep_ms(5);
  }

terminate:
  (void)kill(child, SIGKILL);
  (void)waitpid(child, NULL, 0);
  child = -1;
cleanup:
  if (child > 0) {
    (void)kill(child, SIGKILL);
    (void)waitpid(child, NULL, 0);
  }
  if (pipes[1] >= 0) close(pipes[1]);
  if (pipes[0] >= 0) close(pipes[0]);
  free(child_arguments);
  return result;
}

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
int tidessqld_test_daemon_read(tidessqld_test_daemon *daemon, char *output,
                               size_t capacity, size_t *size) {
  if (daemon == NULL || daemon->log_read < 0 || output == NULL ||
      size == NULL || capacity == 0u || *size >= capacity)
    return -1;
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
    if (tidessqld_test_daemon_read(daemon, log, sizeof(log), &size) < 0) return -1;
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
