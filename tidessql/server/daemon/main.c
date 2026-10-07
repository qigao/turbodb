#include "runtime.h"
#include <gmssl/mem.h>
#include <salts/clock.h>
#include <tlog.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#endif

#ifndef TIDESSQLD_VERSION
#define TIDESSQLD_VERSION "0.0.0"
#endif

enum { TIDESSQLD_LOG_BUFFER_BYTES = 65536, TIDESSQLD_LOG_POOL_BYTES = 32768,
       TIDESSQLD_ENV_NAME_BYTES = 128 };

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number) {
  (void)signal_number;
  stop_requested = 1;
}

#if defined(_WIN32)
static BOOL WINAPI handle_console_control(DWORD control) {
  if (control == CTRL_C_EVENT || control == CTRL_BREAK_EVENT ||
      control == CTRL_CLOSE_EVENT || control == CTRL_SHUTDOWN_EVENT) {
    stop_requested = 1;
    return TRUE;
  }
  return FALSE;
}
#endif

static void print_help(FILE *output) {
  (void)fprintf(output,
      "Usage:\n"
      "  tidessqld serve --config <absolute-path>\n"
      "  tidessqld check-config --config <absolute-path>\n"
      "  tidessqld hash-password --password-env <name> [--iterations <count>]\n"
      "  tidessqld --help\n"
      "  tidessqld --version\n");
}

static const char *option_value(int argc, char **argv, const char *name) {
  for (int i = 2; i + 1 < argc; ++i) if (strcmp(argv[i], name) == 0) return argv[i + 1];
  return NULL;
}

static bool exact_options(int argc, char **argv, const char *first, const char *second) {
  bool seen_first = false, seen_second = second == NULL;
  for (int i = 2; i < argc; i += 2) {
    if (i + 1 >= argc) return false;
    if (strcmp(argv[i], first) == 0 && !seen_first) seen_first = true;
    else if (second && strcmp(argv[i], second) == 0 && !seen_second) seen_second = true;
    else return false;
  }
  return seen_first && seen_second;
}

static int report_error(const turbodb_error_t *error) {
  (void)fprintf(stderr, "%s\n", error && error->message[0] ? error->message : "tidessqld failed");
  return 1;
}

static tlog_t *logger_create(void) {
  const tlog_config_t config = {SALTS_LOG_LEVEL_INFO, TIDESSQLD_LOG_BUFFER_BYTES,
                                TIDESSQLD_LOG_POOL_BYTES};
  tlog_t *logger = tlog_create(&config);
  if (!logger) return NULL;
  const cmeta_console_sink_opts_t options = {stderr, 0, SALTS_LOG_DEFAULT_PATTERN};
  cmeta_log_sink_t *sink = cmeta_sink_console_create(&options);
  if (!sink || tlog_add_sink(logger, sink) != 0) {
    if (sink) cmeta_sink_destroy(sink);
    tlog_destroy(logger);
    return NULL;
  }
  return logger;
}

static void log_message(tlog_t *logger, cmeta_log_level_t level, const char *message) {
  cmeta_log_str(logger, level, vstr_from_cstr("tidessqld"), vstr_from_buf(NULL, 0), 0,
                vstr_from_cstr(message));
}

static int check_config(const char *path) {
  turbodb_error_t error; turbodb_error_init(&error);
  tidessqld_config *config = NULL;
  const turbodb_status_t status = tidessqld_config_load(path, &config, &error);
  if (status != TURBODB_STATUS_OK) return report_error(&error);
  (void)fprintf(stdout, "configuration is valid\n");
  tidessqld_config_destroy(config);
  return 0;
}

static int serve(const char *path) {
  turbodb_error_t error; turbodb_error_init(&error);
  tidessqld_config *config = NULL;
  turbodb_status_t status = tidessqld_config_load(path, &config, &error);
  if (status != TURBODB_STATUS_OK) return report_error(&error);
  tlog_t *logger = logger_create();
  if (!logger) { tidessqld_config_destroy(config); (void)fprintf(stderr, "initialize logger\n"); return 1; }
  tidessqld_runtime runtime = {0};
  status = tidessqld_runtime_start(&runtime, config, &error);
  if (status != TURBODB_STATUS_OK) {
    log_message(logger, SALTS_LOG_LEVEL_ERROR, error.message);
    tlog_flush(logger); tlog_destroy(logger); tidessqld_config_destroy(config); return 1;
  }
  uint16_t port = 0;
  status = tidessqld_runtime_port(&runtime, &port, &error);
  char message[256];
  if (status == TURBODB_STATUS_OK) {
    (void)snprintf(message, sizeof(message),
        "listening host=%s port=%u databases=%zu accounts=%zu",
        config->server.host, (unsigned)port, config->database_count, config->account_count);
    log_message(logger, SALTS_LOG_LEVEL_INFO, message);
  }
  if (signal(SIGINT, handle_signal) == SIG_ERR || signal(SIGTERM, handle_signal) == SIG_ERR) {
    status = TURBODB_STATUS_INTERNAL_ERROR;
    (void)snprintf(error.message, sizeof(error.message), "tidessqld: install signal handlers");
  }
#if defined(_WIN32)
  if (status == TURBODB_STATUS_OK && !SetConsoleCtrlHandler(handle_console_control, TRUE)) {
    status = TURBODB_STATUS_INTERNAL_ERROR;
    (void)snprintf(error.message, sizeof(error.message), "tidessqld: install console control handler");
  }
#endif
  while (status == TURBODB_STATUS_OK && !stop_requested) {
    size_t events = 0;
    status = tidessqld_runtime_poll(&runtime, &events, &error);
  }
  if (status != TURBODB_STATUS_OK) log_message(logger, SALTS_LOG_LEVEL_ERROR, error.message);
  const uint64_t accepted = runtime.server.accepted, closed = runtime.server.closed;
  const uint64_t transport_failures = runtime.server.transport_failures;
  const uint64_t protocol_failures = runtime.server.protocol_failures;
  const uint64_t cleanup_failures = runtime.server.cleanup_failures;
  turbodb_error_t stop_error; turbodb_error_init(&stop_error);
  const turbodb_status_t stopped = tidessqld_runtime_stop(&runtime, &stop_error);
  if (stopped != TURBODB_STATUS_OK) {
    log_message(logger, SALTS_LOG_LEVEL_ERROR, stop_error.message);
    if (status == TURBODB_STATUS_OK) status = stopped;
  } else {
    (void)snprintf(message, sizeof(message),
        "stopped accepted=%llu closed=%llu transport_failures=%llu protocol_failures=%llu cleanup_failures=%llu",
        (unsigned long long)accepted, (unsigned long long)closed,
        (unsigned long long)transport_failures, (unsigned long long)protocol_failures,
        (unsigned long long)cleanup_failures);
    log_message(logger, SALTS_LOG_LEVEL_INFO, message);
  }
  tlog_flush(logger); tlog_destroy(logger); tidessqld_config_destroy(config);
  return status == TURBODB_STATUS_OK ? 0 : 1;
}

static bool valid_env_name(const char *name) {
  if (!name || !*name || strlen(name) > TIDESSQLD_ENV_NAME_BYTES) return false;
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
    if (!isalnum(*p) && *p != '_') return false;
  return true;
}

static void print_hex(const uint8_t *bytes, size_t size) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < size; ++i) {
    (void)fputc(digits[bytes[i] >> 4], stdout);
    (void)fputc(digits[bytes[i] & 15u], stdout);
  }
}

static int hash_password(const char *env_name, const char *iterations_text) {
  if (!valid_env_name(env_name)) { (void)fprintf(stderr, "invalid password environment name\n"); return 2; }
  uint32_t iterations = TDSQL_MYSQL_PASSWORD_MIN_ITERATIONS;
  if (iterations_text) {
    errno = 0; char *end = NULL;
    const unsigned long parsed = strtoul(iterations_text, &end, 10);
    if (errno || !end || *end || parsed < TDSQL_MYSQL_PASSWORD_MIN_ITERATIONS ||
        parsed > TDSQL_MYSQL_PASSWORD_MAX_ITERATIONS) {
      (void)fprintf(stderr, "iterations must be between %u and %u\n",
                    TDSQL_MYSQL_PASSWORD_MIN_ITERATIONS, TDSQL_MYSQL_PASSWORD_MAX_ITERATIONS);
      return 2;
    }
    iterations = (uint32_t)parsed;
  }
  const char *environment = getenv(env_name);
  if (!environment || !*environment) { (void)fprintf(stderr, "password environment is empty\n"); return 2; }
  tstr password = tstr_dup(environment);
  if (!password) { (void)fprintf(stderr, "copy password environment\n"); return 1; }
#if defined(_WIN32)
  (void)_putenv_s(env_name, "");
#else
  (void)unsetenv(env_name);
#endif
  turbodb_error_t error; turbodb_error_init(&error);
  tdsql_mysql_password_record record = {0};
  const turbodb_status_t status = tdsql_mysql_password_make(tstr_to_v(password), iterations,
                                                             &record, &error);
  gmssl_secure_clear(password, tstr_len(password)); tstr_free(password);
  if (status != TURBODB_STATUS_OK) { gmssl_secure_clear(&record, sizeof(record)); return report_error(&error); }
  (void)fprintf(stdout, "password_iterations = %u\npassword_salt_hex = \"", record.iterations);
  print_hex(record.salt, sizeof(record.salt));
  (void)fprintf(stdout, "\"\npassword_hash_hex = \"");
  print_hex(record.hash, sizeof(record.hash));
  (void)fprintf(stdout, "\"\n");
  gmssl_secure_clear(&record, sizeof(record));
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 2 && strcmp(argv[1], "--help") == 0) { print_help(stdout); return 0; }
  if (argc == 2 && strcmp(argv[1], "--version") == 0) {
    (void)fprintf(stdout, "tidessqld %s\n", TIDESSQLD_VERSION); return 0;
  }
  if (argc >= 2 && (strcmp(argv[1], "serve") == 0 || strcmp(argv[1], "check-config") == 0)) {
    if (argc != 4 || !exact_options(argc, argv, "--config", NULL)) { print_help(stderr); return 2; }
    const char *path = option_value(argc, argv, "--config");
    return strcmp(argv[1], "serve") == 0 ? serve(path) : check_config(path);
  }
  if (argc >= 2 && strcmp(argv[1], "hash-password") == 0) {
    if ((argc != 4 && argc != 6) ||
        !exact_options(argc, argv, "--password-env", argc == 6 ? "--iterations" : NULL)) {
      print_help(stderr); return 2;
    }
    return hash_password(option_value(argc, argv, "--password-env"),
                         option_value(argc, argv, "--iterations"));
  }
  print_help(stderr);
  return 2;
}
