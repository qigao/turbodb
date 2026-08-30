#include "dbtool_cli.h"

#include "dbtool_file.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  DBTOOL_DEFAULT_MAX_SCRIPT_BYTES = 16u * 1024u * 1024u,
  DBTOOL_DEFAULT_BUSY_TIMEOUT_MS = 5000u
};

typedef struct dbtool_cli_options {
  const char *file;
  const char *database;
  const char *conninfo_environment;
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

static dbtool_status dbtool_cli_parse(int argc, const char *const *argv,
                                      dbtool_driver_kind driver,
                                      dbtool_cli_options *options,
                                      dbtool_error *error) {
  int file_seen = 0;
  int database_seen = 0;
  int conninfo_seen = 0;
  int max_seen = 0;
  int busy_seen = 0;
  int index;
  if (argc == 2 && argv != NULL && argv[1] != NULL &&
      strcmp(argv[1], "--help") == 0)
    return DBTOOL_STATUS_HELP;
  if (argc < 3 || argv == NULL || argv[0] == NULL || argv[1] == NULL ||
      argv[2] == NULL || strcmp(argv[1], "schema") != 0 ||
      strcmp(argv[2], "apply") != 0)
    return dbtool_cli_fail(error, "expected command: schema apply");

  memset(options, 0, sizeof(*options));
  options->max_script_bytes = DBTOOL_DEFAULT_MAX_SCRIPT_BYTES;
  options->busy_timeout_ms = DBTOOL_DEFAULT_BUSY_TIMEOUT_MS;
  for (index = 3; index < argc; index += 2) {
    const char *name = argv[index];
    const char *value;
    uint64_t number;
    if (name == NULL || index + 1 >= argc || argv[index + 1] == NULL)
      return dbtool_cli_fail(error, "option requires one value");
    value = argv[index + 1];
    if (value[0] == '\0')
      return dbtool_cli_fail(error, "option value cannot be empty");
    if (strcmp(name, "--file") == 0) {
      if (file_seen)
        return dbtool_cli_fail(error, "duplicate option: --file");
      file_seen = 1;
      options->file = value;
    } else if (strcmp(name, "--database") == 0 &&
               driver == DBTOOL_DRIVER_SQLITE) {
      if (database_seen)
        return dbtool_cli_fail(error, "duplicate option: --database");
      database_seen = 1;
      options->database = value;
    } else if (strcmp(name, "--conninfo-env") == 0 &&
               driver == DBTOOL_DRIVER_POSTGRESQL) {
      if (conninfo_seen)
        return dbtool_cli_fail(error, "duplicate option: --conninfo-env");
      conninfo_seen = 1;
      options->conninfo_environment = value;
    } else if (strcmp(name, "--max-script-bytes") == 0) {
      if (max_seen)
        return dbtool_cli_fail(error,
                               "duplicate option: --max-script-bytes");
      max_seen = 1;
      if (!dbtool_parse_decimal(value, (uint64_t)SIZE_MAX, &number))
        return dbtool_cli_fail(error,
                               "invalid decimal: --max-script-bytes");
      options->max_script_bytes = (size_t)number;
    } else if (strcmp(name, "--busy-timeout-ms") == 0 &&
               driver == DBTOOL_DRIVER_SQLITE) {
      if (busy_seen)
        return dbtool_cli_fail(error,
                               "duplicate option: --busy-timeout-ms");
      busy_seen = 1;
      if (!dbtool_parse_decimal(value, (uint64_t)INT_MAX, &number))
        return dbtool_cli_fail(error,
                               "invalid decimal: --busy-timeout-ms");
      options->busy_timeout_ms = (uint32_t)number;
    } else {
      return dbtool_cli_fail(error, "unknown option");
    }
  }
  if (!file_seen)
    return dbtool_cli_fail(error, "missing required option: --file");
  if (driver == DBTOOL_DRIVER_SQLITE && !database_seen)
    return dbtool_cli_fail(error, "missing required option: --database");
  if (driver != DBTOOL_DRIVER_SQLITE &&
      driver != DBTOOL_DRIVER_POSTGRESQL)
    return dbtool_cli_fail(error, "unknown database driver");
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
  if (result != NULL)
    *result = (dbtool_apply_result)DBTOOL_APPLY_RESULT_INIT;
  dbtool_error_init(error);
  if (ops == NULL || ops->struct_size < sizeof(*ops) ||
      ops->abi_version != DBTOOL_SCHEMA_DRIVER_ABI_VERSION ||
      ops->open == NULL || ops->apply == NULL || ops->close == NULL ||
      result == NULL) {
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
  return status;
}

void dbtool_cli_print_help(dbtool_driver_kind driver, const char *program) {
  const char *name = program != NULL ? program : "turbodb-driver";
  if (driver == DBTOOL_DRIVER_SQLITE) {
    (void)fprintf(stdout,
                  "Usage: %s schema apply --database <path> --file <sql> "
                  "[--max-script-bytes <n>] [--busy-timeout-ms <n>]\n",
                  name);
  } else {
    (void)fprintf(stdout,
                  "Usage: %s schema apply --file <sql> "
                  "[--conninfo-env <name>] [--max-script-bytes <n>]\n",
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
