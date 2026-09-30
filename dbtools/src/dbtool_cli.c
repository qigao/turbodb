#include "dbtool_cli.h"

#include "dbtool_file.h"
#include "dbtool_plugin.h"

#include <cmd_arger.h>
#include <tstr.h>

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  DBTOOL_DEFAULT_MAX_SCRIPT_BYTES = 16u * 1024u * 1024u,
  DBTOOL_DEFAULT_BUSY_TIMEOUT_MS = 5000u,
  DBTOOL_CLI_OPTION_CAPACITY = 5u
};

typedef struct dbtool_cli_options {
  const char *file;
  const char *database;
  const char *conninfo_environment;
  const char *plugin;
  size_t max_script_bytes;
  uint32_t busy_timeout_ms;
} dbtool_cli_options;

static dbtool_status dbtool_cli_fail(dbtool_error *error,
                                     const char *message) {
  dbtool_error_set(error, DBTOOL_STATUS_INVALID_ARGUMENT, "parse-options", 0,
                   message);
  return DBTOOL_STATUS_INVALID_ARGUMENT;
}

static int dbtool_parse_decimal(const char *text, uint64_t maximum,
                                uint64_t *out) {
  uint64_t value = 0u;
  const unsigned char *current = (const unsigned char *)text;
  if (text == NULL || text[0] == '\0' || out == NULL)
    return 0;
  while (*current != 0u) {
    const uint64_t digit = (uint64_t)(*current - (unsigned char)'0');
    if (*current < (unsigned char)'0' || *current > (unsigned char)'9' ||
        value > (maximum - digit) / 10u)
      return 0;
    value = value * 10u + digit;
    ++current;
  }
  if (value == 0u)
    return 0;
  *out = value;
  return 1;
}

/* CmdParser's installed API exits on syntax errors. Validate against the same
 * descriptors first so this library entry still returns dbtool_status. Keep
 * --name=value normalization private: it protects literal @file values from
 * CmdParser response-file expansion and leaves caller argv immutable. */
static dbtool_status dbtool_cli_read_options(
    int argc, const char *const *argv, CmdArgerDesc *descriptors,
    uint32_t count, dbtool_error *error) {
  const char *original[DBTOOL_CLI_OPTION_CAPACITY] = {0};
  tstr normalized[DBTOOL_CLI_OPTION_CAPACITY] = {0};
  char *parser_argv[DBTOOL_CLI_OPTION_CAPACITY + 1u] = {"schema apply"};
  int parser_argc = 1;
  dbtool_status status = DBTOOL_STATUS_OK;
  char message[DBTOOL_ERROR_MESSAGE_CAPACITY];

  for (int i = 3; i < argc; i += 2) {
    const char *name = argv[i];
    uint32_t index;
    if (name == NULL || i + 1 >= argc || argv[i + 1] == NULL)
      return dbtool_cli_fail(error, "option requires one value");
    if (argv[i + 1][0] == '\0')
      return dbtool_cli_fail(error, "option value cannot be empty");
    for (index = 0u; index < count; ++index)
      if (strncmp(name, "--", 2u) == 0 && strcmp(name + 2, descriptors[index].name) == 0)
        break;
    if (index == count)
      return dbtool_cli_fail(error, "unknown option");
    if (original[index] != NULL) {
      (void)snprintf(message, sizeof(message), "duplicate option: --%s", descriptors[index].name);
      return dbtool_cli_fail(error, message);
    }
    original[index] = argv[i + 1];
  }
  for (uint32_t i = 0u; i < count; ++i) {
    if (descriptors[i].is_required && original[i] == NULL) {
      (void)snprintf(message, sizeof(message), "missing required option: --%s", descriptors[i].name);
      return dbtool_cli_fail(error, message);
    }
  }

  for (uint32_t i = 0u; i < count; ++i) {
    if (original[i] == NULL) continue;
    const size_t prefix_size = strlen(descriptors[i].name) + 3u;
    const size_t value_size = strlen(original[i]);
    if (value_size >= SIZE_MAX - prefix_size) {
      status = DBTOOL_STATUS_LIMIT_EXCEEDED;
      dbtool_error_set(error, status, "parse-options", 0, "option exceeds addressable storage");
      goto cleanup;
    }
    const size_t size = prefix_size + value_size;
    normalized[i] = tstr_new_len(NULL, size);
    if (normalized[i] == NULL) {
      status = DBTOOL_STATUS_OUT_OF_MEMORY;
      dbtool_error_set(error, status, "parse-options", 0, "allocate command line option");
      goto cleanup;
    }
    (void)snprintf(normalized[i], size + 1u, "--%s=%s", descriptors[i].name, original[i]);
    parser_argv[parser_argc++] = normalized[i];
  }
  cmd_arger_parse(descriptors, count, NULL, 0u, parser_argc, parser_argv,
                  "TurboDB schema tools", cmd_arger_false);
  /* Parsed strings borrow normalized argv. Restore the corresponding caller
   * views before releasing that storage; optional outputs remain NULL. */
  for (uint32_t i = 0u; i < count; ++i) {
    char **out = descriptors[i].value_out;
    if (*out != NULL) {
      if (normalized[i] == NULL || *out != normalized[i] + strlen(descriptors[i].name) + 3u) {
        status = DBTOOL_STATUS_INTERNAL_ERROR;
        dbtool_error_set(error, status, "parse-options", 0, "unexpected CmdParser string ownership");
        goto cleanup;
      }
      *out = (char *)original[i];
    }
  }
cleanup:
  for (uint32_t i = 0u; i < count; ++i) tstr_free(normalized[i]);
  return status;
}

static dbtool_status dbtool_cli_parse(int argc, const char *const *argv,
                                      dbtool_driver_kind driver,
                                      dbtool_cli_options *options,
                                      dbtool_error *error) {
  char *file = NULL, *plugin = NULL, *maximum = NULL, *connection = NULL, *busy = NULL;
  CmdArgerDesc descriptors[DBTOOL_CLI_OPTION_CAPACITY] = {
      cmd_arger_required(cmd_arger_desc_string(&file, "file", "SQL script path")),
      cmd_arger_desc_string(&plugin, "plugin", "Absolute driver module path"),
      cmd_arger_desc_string(&maximum, "max-script-bytes", "Maximum SQL script size")};
  uint32_t count = 3u;
  uint64_t number;
  dbtool_status status;
  if (argc == 2 && argv != NULL && argv[1] != NULL && strcmp(argv[1], "--help") == 0)
    return DBTOOL_STATUS_HELP;
  if (argc < 3 || argv == NULL || argv[0] == NULL || argv[1] == NULL ||
      argv[2] == NULL || strcmp(argv[1], "schema") != 0 || strcmp(argv[2], "apply") != 0)
    return dbtool_cli_fail(error, "expected command: schema apply");
  if (driver == DBTOOL_DRIVER_SQLITE) {
    descriptors[count++] = cmd_arger_required(
        cmd_arger_desc_string(&connection, "database", "SQLite database path"));
    descriptors[count++] = cmd_arger_desc_string(&busy, "busy-timeout-ms", "SQLite busy timeout");
  } else if (driver == DBTOOL_DRIVER_POSTGRESQL || driver == DBTOOL_DRIVER_MYSQL) {
    descriptors[count] = cmd_arger_desc_string(&connection, "conninfo-env", "Connection environment name");
    descriptors[count++].is_required = driver == DBTOOL_DRIVER_MYSQL;
  } else {
    return dbtool_cli_fail(error, "unknown database driver");
  }
  status = dbtool_cli_read_options(argc, argv, descriptors, count, error);
  if (status != DBTOOL_STATUS_OK) return status;

  memset(options, 0, sizeof(*options));
  options->file = file;
  options->plugin = plugin;
  if (driver == DBTOOL_DRIVER_SQLITE) options->database = connection;
  else options->conninfo_environment = connection;
  options->max_script_bytes = DBTOOL_DEFAULT_MAX_SCRIPT_BYTES;
  options->busy_timeout_ms = DBTOOL_DEFAULT_BUSY_TIMEOUT_MS;
  /* CmdParser's integer descriptor uses strtol, which is 32-bit on Windows.
   * Keep the size_t range and strict positive-decimal contract here. */
  if (maximum != NULL) {
    if (!dbtool_parse_decimal(maximum, (uint64_t)SIZE_MAX, &number))
      return dbtool_cli_fail(error, "invalid decimal: --max-script-bytes");
    options->max_script_bytes = (size_t)number;
  }
  if (busy != NULL) {
    if (!dbtool_parse_decimal(busy, (uint64_t)INT_MAX, &number))
      return dbtool_cli_fail(error, "invalid decimal: --busy-timeout-ms");
    options->busy_timeout_ms = (uint32_t)number;
  }
  return DBTOOL_STATUS_OK;
}

dbtool_status dbtool_cli_execute(int argc, const char *const *argv,
                                 dbtool_driver_kind driver,
                                 const dbtool_schema_driver_ops *ops,
                                 dbtool_apply_result *result,
                                 dbtool_error *error) {
  dbtool_cli_options options;
  dbtool_connection_config config = {0};
  dbtool_file script = DBTOOL_FILE_INIT;
  void *context = NULL;
  dbtool_status status;
  int opened = 0;
  dbtool_plugin plugin = {0};
  if (result != NULL)
    *result = (dbtool_apply_result)DBTOOL_APPLY_RESULT_INIT;
  dbtool_error_init(error);
  if (result == NULL || (ops != NULL && (ops->struct_size < sizeof(*ops) ||
      ops->abi_version != DBTOOL_SCHEMA_DRIVER_ABI_VERSION ||
      ops->open == NULL || ops->apply == NULL || ops->close == NULL))) {
    dbtool_error_set(error, DBTOOL_STATUS_INVALID_ARGUMENT, "validate-driver",
                     0, "invalid schema driver contract");
    return DBTOOL_STATUS_INVALID_ARGUMENT;
  }
  status = dbtool_cli_parse(argc, argv, driver, &options, error);
  if (status != DBTOOL_STATUS_OK)
    return status;

  config.database = options.database;
  config.busy_timeout_ms = options.busy_timeout_ms;
  if (options.conninfo_environment != NULL) {
    config.conninfo = getenv(options.conninfo_environment);
    if (config.conninfo == NULL || config.conninfo[0] == '\0')
      return dbtool_cli_fail(error,
                             "conninfo environment variable is not set");
  }
  status = dbtool_file_read(options.file, options.max_script_bytes, &script,
                            error);
  if (status != DBTOOL_STATUS_OK)
    return status;
  if (ops == NULL) {
    const char *driver_id = driver == DBTOOL_DRIVER_SQLITE ? "sqlite" :
                            driver == DBTOOL_DRIVER_MYSQL ? "mysql" : "postgresql";
    const char *path = options.plugin != NULL ? options.plugin :
        getenv(driver == DBTOOL_DRIVER_SQLITE ? "TURBODB_SQLITE_PLUGIN" :
               driver == DBTOOL_DRIVER_MYSQL ? "TURBODB_MYSQL_PLUGIN" :
                                               "TURBODB_POSTGRESQL_PLUGIN");
    status = dbtool_plugin_load(&plugin, path, driver_id, error);
    if (status != DBTOOL_STATUS_OK)
      goto cleanup;
    ops = plugin.ops;
  }
  status = ops->open(&context, &config, error);
  if (status != DBTOOL_STATUS_OK)
    goto cleanup;
  if (context == NULL) {
    status = DBTOOL_STATUS_INTERNAL_ERROR;
    dbtool_error_set(error, status, "open-driver", 0,
                     "driver returned no connection context");
    goto cleanup;
  }
  opened = 1;
  status = ops->apply(context, script.data, script.size, result, error);

cleanup:
  dbtool_file_release(&script);
  if (opened)
    ops->close(context);
  {
    dbtool_error close_error = DBTOOL_ERROR_INIT;
    const dbtool_status closed = dbtool_plugin_close(&plugin, &close_error);
    if (closed != DBTOOL_STATUS_OK) {
      if (status == DBTOOL_STATUS_OK) {
        status = closed;
        if (error != NULL) *error = close_error;
      } else {
        dbtool_cli_print_error(&close_error);
      }
    }
  }
  return status;
}

void dbtool_cli_print_help(dbtool_driver_kind driver, const char *program) {
  const char *name = program != NULL ? program : "turbodb-driver";
  if (driver == DBTOOL_DRIVER_SQLITE) {
    (void)fprintf(stdout,
                  "Usage: %s schema apply --database <path> --file <sql> "
                  "[--max-script-bytes <n>] [--busy-timeout-ms <n>] "
                  "[--plugin <absolute-path>]\n"
                  "Plugin: --plugin or TURBODB_SQLITE_PLUGIN is required.\n",
                  name);
  } else if (driver == DBTOOL_DRIVER_MYSQL) {
    (void)fprintf(stdout,
                  "Usage: %s schema apply --file <sql> --conninfo-env <name> "
                  "[--max-script-bytes <n>] [--plugin <absolute-path>]\n"
                  "Plugin: --plugin or TURBODB_MYSQL_PLUGIN is required.\n"
                  "Connection: JSON with host, username, password, database, "
                  "ca_file, server_name; optional port and timeout_ms.\n",
                  name);
  } else {
    (void)fprintf(stdout,
                  "Usage: %s schema apply --file <sql> "
                  "[--conninfo-env <name>] [--max-script-bytes <n>] "
                  "[--plugin <absolute-path>]\n"
                  "Plugin: --plugin or TURBODB_POSTGRESQL_PLUGIN is required.\n",
                  name);
  }
}

void dbtool_cli_print_error(const dbtool_error *error) {
  if (error == NULL)
    return;
  (void)fprintf(stderr, "dbtool: %s: %s (status=%s, native=%d)\n",
                error->stage[0] != '\0' ? error->stage : "unknown-stage",
                error->message[0] != '\0' ? error->message : "operation failed",
                dbtool_status_name(error->status), error->native_code);
}
