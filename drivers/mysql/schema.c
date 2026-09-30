#include "schema.h"
#include <session_script.h>
#include <json_parser.h>
#include <salts_error.h>

#include <stdlib.h>
#include <string.h>

enum {
  DBTOOL_MYSQL_CONFIG_MAX_BYTES = 32768u,
  DBTOOL_MYSQL_OPTION_MAX_BYTES = 4096u,
  DBTOOL_MYSQL_DEFAULT_PORT = 3306u,
  DBTOOL_MYSQL_DEFAULT_TIMEOUT_MS = 5000u
};

typedef struct dbtool_mysql_state {
  json_value_t *document;
  mysql_session_config_t config;
} dbtool_mysql_state;

static dbtool_status mysql_schema_fail(dbtool_error *error, dbtool_status status,
                                       const char *stage, const char *message) {
  dbtool_error_set(error, status, stage, 0, message);
  return status;
}

static void dbtool_mysql_close(void *context) {
  dbtool_mysql_state *state = context;
  if (state == NULL) return;
  json_free(state->document);
  free(state);
}

static int mysql_schema_number(const json_value_t *value, uint32_t maximum,
                                uint32_t *out) {
  size_t length = 0u;
  const char *text = json_number_text(value, &length);
  uint32_t parsed = 0u;
  if (text == NULL || length == 0u) return 0;
  for (size_t i = 0u; i < length; ++i) {
    const unsigned char ch = (unsigned char)text[i];
    if (ch < '0' || ch > '9') return 0;
    const uint32_t digit = (uint32_t)(ch - '0');
    if (parsed > (maximum - digit) / 10u) return 0;
    parsed = parsed * 10u + digit;
  }
  if (parsed == 0u) return 0;
  *out = parsed;
  return 1;
}

static int mysql_schema_config(dbtool_mysql_state *state) {
  const size_t count = json_object_size(state->document);
  state->config.port = DBTOOL_MYSQL_DEFAULT_PORT;
  state->config.timeout_ms = DBTOOL_MYSQL_DEFAULT_TIMEOUT_MS;
  for (size_t i = 0u; i < count; ++i) {
    const char *key = json_object_key(state->document, i);
    const json_value_t *value = json_object_value(state->document, i);
    const char **destination = NULL;
    uint32_t number;
    if (strlen(key) != json_object_key_len(state->document, i)) return 0;
    if (strcmp(key, "port") == 0) {
      if (!mysql_schema_number(value, UINT16_MAX, &number)) return 0;
      state->config.port = (uint16_t)number;
      continue;
    }
    if (strcmp(key, "timeout_ms") == 0) {
      if (!mysql_schema_number(value, UINT32_MAX, &number)) return 0;
      state->config.timeout_ms = number;
      continue;
    }
    if (strcmp(key, "host") == 0) destination = &state->config.host;
    else if (strcmp(key, "username") == 0) destination = &state->config.username;
    else if (strcmp(key, "password") == 0) destination = &state->config.password;
    else if (strcmp(key, "database") == 0) destination = &state->config.database;
    else if (strcmp(key, "ca_file") == 0) destination = &state->config.ca_file;
    else if (strcmp(key, "server_name") == 0) destination = &state->config.server_name;
    else return 0;
    if (json_type(value) != JSON_STRING) return 0;
    const char *text = json_string(value);
    const size_t length = json_string_len(value);
    if (text == NULL || length > DBTOOL_MYSQL_OPTION_MAX_BYTES ||
        strlen(text) != length || (length == 0u && destination != &state->config.password))
      return 0;
    *destination = text;
  }
  return state->config.host != NULL && state->config.username != NULL &&
         state->config.password != NULL && state->config.database != NULL &&
         state->config.ca_file != NULL && state->config.server_name != NULL;
}

static dbtool_status dbtool_mysql_open(void **out_context,
    const dbtool_connection_config *config, dbtool_error *error) {
  dbtool_mysql_state *state;
  dbtool_error_init(error);
  if (out_context != NULL) *out_context = NULL;
  if (out_context == NULL || config == NULL || config->conninfo == NULL ||
      config->conninfo[0] == '\0')
    return mysql_schema_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "open-driver",
                              "MySQL requires a JSON connection configuration");
  if (strlen(config->conninfo) > DBTOOL_MYSQL_CONFIG_MAX_BYTES)
    return mysql_schema_fail(error, DBTOOL_STATUS_LIMIT_EXCEEDED, "open-driver",
                              "MySQL connection configuration exceeds its byte limit");
  state = calloc(1u, sizeof(*state));
  if (state == NULL)
    return mysql_schema_fail(error, DBTOOL_STATUS_OUT_OF_MEMORY, "open-driver",
                              "allocate MySQL schema context");
  state->document = json_parse(config->conninfo, strlen(config->conninfo));
  if (state->document == NULL || json_type(state->document) != JSON_OBJECT ||
      !mysql_schema_config(state)) {
    dbtool_mysql_close(state);
    /* Parser diagnostics may quote secrets from the connection document. */
    return mysql_schema_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "open-driver",
                              "invalid MySQL connection fields, numeric bounds or TLS configuration");
  }
  /* The owned JSON document keeps every config string alive until close.
   * apply owns the dedicated network session and closes it before returning. */
  *out_context = state;
  return DBTOOL_STATUS_OK;
}

static dbtool_status dbtool_mysql_apply(void *context, const char *sql, size_t sql_size,
    dbtool_apply_result *result, dbtool_error *error) {
  dbtool_mysql_state *state = context;
  mysql_session_error_t native = {0};
  mysql_session_status_t status;
  dbtool_status mapped;
  dbtool_error_init(error);
  if (result != NULL) *result = (dbtool_apply_result)DBTOOL_APPLY_RESULT_INIT;
  if (state == NULL || sql == NULL || sql_size == 0u || result == NULL)
    return mysql_schema_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "apply-schema",
                              "invalid MySQL schema input");
  if (sql_size > MYSQL_SESSION_SCRIPT_MAX_BYTES)
    return mysql_schema_fail(error, DBTOOL_STATUS_LIMIT_EXCEEDED, "apply-schema",
                              "MySQL schema exceeds the single-command byte limit");
  if (memchr(sql, 0, sql_size) != NULL)
    return mysql_schema_fail(error, DBTOOL_STATUS_INVALID_ARGUMENT, "apply-schema",
                              "MySQL schema contains embedded NUL");
  status = mysql_session_execute_script(&state->config, (const uint8_t *)sql,
      sql_size, MYSQL_SESSION_SCRIPT_MAX_BYTES, &result->statements, &native);
  if (status == MYSQL_SESSION_OK) return DBTOOL_STATUS_OK;
  result->statements = 0u;
  switch (status) {
    case MYSQL_SESSION_INVALID: mapped = DBTOOL_STATUS_INVALID_ARGUMENT; break;
    case MYSQL_SESSION_UNSUPPORTED: mapped = DBTOOL_STATUS_UNSUPPORTED; break;
    case MYSQL_SESSION_SQL_ERROR: mapped = DBTOOL_STATUS_SQL_ERROR; break;
    default: mapped = native.cnet_status == SALTS_ENOMEM
                       ? DBTOOL_STATUS_OUT_OF_MEMORY : DBTOOL_STATUS_CONNECTION_ERROR; break;
  }
  dbtool_error_set(error, mapped, "apply-schema",
      native.server_error != 0u ? (int)native.server_error : (int)status,
      native.message[0] != '\0' ? native.message : "MySQL schema execution failed");
  return mapped;
}

static const dbtool_schema_driver_ops dbtool_mysql_ops = {
    sizeof(dbtool_schema_driver_ops), DBTOOL_SCHEMA_DRIVER_ABI_VERSION,
    dbtool_mysql_open, dbtool_mysql_apply, dbtool_mysql_close};

const dbtool_schema_driver_ops *dbtool_mysql_schema_driver(void) {
  return &dbtool_mysql_ops;
}

static const TurboDb_SchemaApply_vtable schema_vtable = {
    .implementation = "mysql", .operations = dbtool_schema_operations};
TurboDb_SchemaApply dbtool_mysql_schema = {(void *)&dbtool_mysql_ops, &schema_vtable};
