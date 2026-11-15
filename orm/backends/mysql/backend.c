#include "backend.h"

#include "cursor.h"
#include "orm_mysql_render.h"
#include "parameters.h"
#include "session.h"
#include "session_cursor.h"
#include "session_transaction.h"
#include "wire/packet.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
  ORM_MYSQL_DEFAULT_PORT = 3306u,
  ORM_MYSQL_DEFAULT_TIMEOUT_MS = 5000u,
  ORM_MYSQL_OPTION_VALUE_MAX = 4096u,
  ORM_MYSQL_CONTROL_COMMAND_BYTES = 4096u
};

typedef struct orm_mysql_settings {
  tstr host;
  tstr username;
  tstr password;
  tstr database;
  tstr ca_file;
  tstr server_name;
  uint16_t port;
  uint32_t timeout_ms;
} orm_mysql_settings;

typedef struct orm_mysql_backend_state {
  orm_mysql_settings settings;
  orm_limits limits;
  int transaction_active;
} orm_mysql_backend_state;

typedef struct orm_mysql_transaction_state {
  orm_mysql_backend_state *owner;
  mysql_transaction_session_t *session;
  int active;
} orm_mysql_transaction_state;

typedef struct orm_mysql_prepared {
  uint8_t *sql;
  size_t sql_size;
  mysql_stmt_value_t *values;
  size_t value_count;
  size_t command_bytes;
} orm_mysql_prepared;

static orm_status_t orm_mysql_fail(
    orm_error_t *error, orm_status_t status, const char *message) {
  orm_error_set(error, status, message);
  return status;
}

static void orm_mysql_settings_destroy(orm_mysql_settings *settings) {
  if (settings == NULL)
    return;
  tstr_freep(&settings->host);
  tstr_freep(&settings->username);
  tstr_freep(&settings->password);
  tstr_freep(&settings->database);
  tstr_freep(&settings->ca_file);
  tstr_freep(&settings->server_name);
  memset(settings, 0, sizeof(*settings));
}

static int orm_mysql_parse_u64(
    vstr value, uint64_t maximum, uint64_t *out) {
  uint64_t parsed = 0u;
  size_t i;
  if (!orm_view_valid(value, false) || out == NULL)
    return 0;
  for (i = 0u; i < value.len; ++i) {
    const unsigned char ch = (unsigned char)value.data[i];
    const uint64_t digit =
        ch >= (unsigned char)'0' && ch <= (unsigned char)'9'
            ? (uint64_t)(ch - (unsigned char)'0')
            : UINT64_MAX;
    if (digit > 9u || parsed > (maximum - digit) / 10u)
      return 0;
    parsed = parsed * 10u + digit;
  }
  *out = parsed;
  return 1;
}

static orm_status_t orm_mysql_copy_option(
    tstr *out, vstr value, int allow_empty,
    orm_error_t *error, const char *message) {
  if (out == NULL ||
      !orm_view_valid(value, allow_empty) ||
      value.len > ORM_MYSQL_OPTION_VALUE_MAX ||
      memchr(value.data, 0, value.len) != NULL)
    return orm_mysql_fail(
        error,
        value.len > ORM_MYSQL_OPTION_VALUE_MAX
            ? ORM_STATUS_LIMIT_EXCEEDED
            : ORM_STATUS_INVALID_ARGUMENT,
        message);
  tstr_freep(out);
  *out = tstr_from_v(value);
  if (*out == NULL)
    return orm_mysql_fail(error, ORM_STATUS_OUT_OF_MEMORY, message);
  return ORM_STATUS_OK;
}

static orm_status_t orm_mysql_settings_parse(
    const orm_config_t *config, orm_mysql_settings *settings,
    orm_error_t *error) {
  uint32_t i;
  int have_host = 0;
  int have_username = 0;
  int have_password = 0;
  int have_database = 0;
  int have_ca = 0;
  int have_server_name = 0;

  if (config == NULL || settings == NULL)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "invalid MySQL connection configuration");

  memset(settings, 0, sizeof(*settings));
  settings->port = ORM_MYSQL_DEFAULT_PORT;
  settings->timeout_ms = ORM_MYSQL_DEFAULT_TIMEOUT_MS;

  for (i = 0u; i < config->option_count; ++i) {
    const orm_option_t *option = &config->options[i];
    uint32_t prior;
    uint64_t parsed;
    orm_status_t status = ORM_STATUS_OK;

    for (prior = 0u; prior < i; ++prior) {
      if (option->keyword.len == config->options[prior].keyword.len &&
          (option->keyword.len == 0u ||
           memcmp(option->keyword.data,
                  config->options[prior].keyword.data,
                  option->keyword.len) == 0)) {
        orm_mysql_settings_destroy(settings);
        return orm_mysql_fail(
            error, ORM_STATUS_INVALID_ARGUMENT,
            "duplicate MySQL connection option");
      }
    }

    if (orm_view_equal_cstr(option->keyword, "host")) {
      status = orm_mysql_copy_option(
          &settings->host, option->value, 0, error,
          "invalid MySQL host option");
      have_host = status == ORM_STATUS_OK;
    } else if (orm_view_equal_cstr(option->keyword, "username")) {
      status = orm_mysql_copy_option(
          &settings->username, option->value, 1, error,
          "invalid MySQL username option");
      have_username = status == ORM_STATUS_OK;
    } else if (orm_view_equal_cstr(option->keyword, "password")) {
      status = orm_mysql_copy_option(
          &settings->password, option->value, 1, error,
          "invalid MySQL password option");
      have_password = status == ORM_STATUS_OK;
    } else if (orm_view_equal_cstr(option->keyword, "database")) {
      status = orm_mysql_copy_option(
          &settings->database, option->value, 0, error,
          "invalid MySQL database option");
      have_database = status == ORM_STATUS_OK;
    } else if (orm_view_equal_cstr(option->keyword, "ca_file")) {
      status = orm_mysql_copy_option(
          &settings->ca_file, option->value, 0, error,
          "invalid MySQL ca_file option");
      have_ca = status == ORM_STATUS_OK;
    } else if (orm_view_equal_cstr(option->keyword, "server_name")) {
      status = orm_mysql_copy_option(
          &settings->server_name, option->value, 0, error,
          "invalid MySQL server_name option");
      have_server_name = status == ORM_STATUS_OK;
    } else if (orm_view_equal_cstr(option->keyword, "port")) {
      if (!orm_mysql_parse_u64(option->value, UINT16_MAX, &parsed) ||
          parsed == 0u)
        status = orm_mysql_fail(
            error, ORM_STATUS_INVALID_ARGUMENT,
            "MySQL port must be in [1, 65535]");
      else
        settings->port = (uint16_t)parsed;
    } else if (orm_view_equal_cstr(option->keyword, "timeout_ms")) {
      if (!orm_mysql_parse_u64(option->value, UINT32_MAX, &parsed) ||
          parsed == 0u)
        status = orm_mysql_fail(
            error, ORM_STATUS_INVALID_ARGUMENT,
            "MySQL timeout_ms must be positive");
      else
        settings->timeout_ms = (uint32_t)parsed;
    } else {
      status = orm_mysql_fail(
          error, ORM_STATUS_INVALID_ARGUMENT,
          "unknown MySQL connection option");
    }

    if (status != ORM_STATUS_OK) {
      orm_mysql_settings_destroy(settings);
      return status;
    }
  }

  if (!have_host || !have_username || !have_password ||
      !have_database || !have_ca || !have_server_name) {
    orm_mysql_settings_destroy(settings);
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "MySQL requires host, username, password, database, ca_file and server_name");
  }
  return ORM_STATUS_OK;
}

static mysql_session_config_t orm_mysql_session_config(
    const orm_mysql_settings *settings) {
  mysql_session_config_t config;
  memset(&config, 0, sizeof(config));
  config.host = settings->host;
  config.port = settings->port;
  config.username = settings->username;
  config.password = settings->password;
  config.database = settings->database;
  config.ca_file = settings->ca_file;
  config.server_name = settings->server_name;
  config.timeout_ms = settings->timeout_ms;
  return config;
}

static orm_status_t orm_mysql_session_status(
    mysql_session_status_t status,
    const mysql_session_error_t *native,
    orm_error_t *error) {
  orm_status_t mapped;
  const char *message =
      native != NULL && native->message[0] != '\0'
          ? native->message
          : NULL;

  if (status == MYSQL_SESSION_OK) {
    orm_error_set(error, ORM_STATUS_OK, NULL);
    return ORM_STATUS_OK;
  }
  if (status == MYSQL_SESSION_COMMIT_UNKNOWN) {
    mapped = ORM_STATUS_COMMIT_UNKNOWN;
  } else if (native != NULL && native->server_error != 0u) {
    switch (native->server_error) {
      case 1048u:
      case 1062u:
      case 1451u:
      case 1452u:
      case 3819u:
        mapped = ORM_STATUS_CONSTRAINT;
        break;
      default:
        mapped = ORM_STATUS_SQL_ERROR;
        break;
    }
  } else {
    switch (status) {
      case MYSQL_SESSION_INVALID:
        mapped = ORM_STATUS_INVALID_ARGUMENT;
        break;
      case MYSQL_SESSION_IO:
      case MYSQL_SESSION_AUTH:
      case MYSQL_SESSION_TIMEOUT:
        mapped = ORM_STATUS_CONNECTION_ERROR;
        break;
      case MYSQL_SESSION_PROTOCOL:
        mapped = ORM_STATUS_DATASTORE_ERROR;
        break;
      default:
        mapped = ORM_STATUS_INTERNAL_ERROR;
        break;
    }
  }

  orm_error_set(
      error, mapped,
      message != NULL
          ? message
          : (mapped == ORM_STATUS_COMMIT_UNKNOWN
                 ? "MySQL COMMIT acknowledgement is unknown"
                 : "MySQL Driver operation failed"));
  return mapped;
}

static size_t orm_mysql_lenenc_size(size_t size) {
  if (size < UINT8_C(0xfb))
    return 1u;
  if (size <= UINT16_MAX)
    return 3u;
  if (size <= UINT32_C(0x00ffffff))
    return 4u;
  return 9u;
}

static orm_status_t orm_mysql_map_value(
    const orm_owned_value *input, mysql_stmt_value_t *out,
    size_t *encoded_bytes, orm_error_t *error) {
  size_t bytes = 0u;
  if (input == NULL || out == NULL || encoded_bytes == NULL)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "invalid MySQL parameter value");

  memset(out, 0, sizeof(*out));
  switch (input->kind) {
    case ORM_VALUE_NULL:
      out->kind = MYSQL_STMT_VALUE_NULL;
      break;
    case ORM_VALUE_INT64:
      out->kind = MYSQL_STMT_VALUE_SINT64;
      out->data.sint64_value = input->data.int64_value;
      bytes = 8u;
      break;
    case ORM_VALUE_UINT64:
      out->kind = MYSQL_STMT_VALUE_UINT64;
      out->data.uint64_value = input->data.uint64_value;
      bytes = 8u;
      break;
    case ORM_VALUE_DOUBLE:
      out->kind = MYSQL_STMT_VALUE_DOUBLE;
      out->data.double_value = input->data.double_value;
      bytes = 8u;
      break;
    case ORM_VALUE_BOOLEAN:
      if (input->data.boolean_value > 1u)
        return orm_mysql_fail(
            error, ORM_STATUS_INVALID_ARGUMENT,
            "invalid MySQL boolean parameter");
      out->kind = MYSQL_STMT_VALUE_BOOL;
      out->data.bool_value = input->data.boolean_value;
      bytes = 1u;
      break;
    case ORM_VALUE_TEXT:
    case ORM_VALUE_BLOB: {
      const size_t size = tstr_len(input->bytes);
      if (size > SIZE_MAX - orm_mysql_lenenc_size(size))
        return orm_mysql_fail(
            error, ORM_STATUS_LIMIT_EXCEEDED,
            "MySQL parameter size exceeds platform range");
      out->kind = input->kind == ORM_VALUE_TEXT
                      ? MYSQL_STMT_VALUE_TEXT
                      : MYSQL_STMT_VALUE_BLOB;
      out->data.bytes.data =
          (const uint8_t *)input->bytes;
      out->data.bytes.size = size;
      bytes = orm_mysql_lenenc_size(size) + size;
      break;
    }
    default:
      return orm_mysql_fail(
          error, ORM_STATUS_INVALID_ARGUMENT,
          "unknown MySQL parameter kind");
  }
  *encoded_bytes = bytes;
  return ORM_STATUS_OK;
}

static void orm_mysql_prepared_destroy(orm_mysql_prepared *prepared) {
  if (prepared == NULL)
    return;
  free(prepared->values);
  free(prepared->sql);
  memset(prepared, 0, sizeof(*prepared));
}

static orm_status_t orm_mysql_prepare_raw(
    const orm_query_plan *plan, const orm_limits *limits,
    orm_mysql_prepared *out, orm_error_t *error) {
  mysql_parameter_lower_options_t options;
  mysql_parameter_lower_result_t lowered;
  uint32_t *bind_order = NULL;
  size_t bind_capacity;
  size_t raw_count;
  size_t sql_size;
  size_t execute_bytes = 10u;
  size_t max_index = 0u;
  size_t i;
  mysql_wire_status_t wire_status;
  orm_status_t status = ORM_STATUS_OK;

  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (plan == NULL || limits == NULL || out == NULL ||
      plan->kind != ORM_QUERY_RAW || plan->raw_sql == NULL)
    return orm_mysql_fail(
        error, ORM_STATUS_UNSUPPORTED,
        "MySQL Driver currently accepts RAW_SQL plans only");

  sql_size = tstr_len(plan->raw_sql);
  raw_count = vec_size(&plan->raw_parameters);
  if (sql_size == 0u || sql_size > limits->max_query_bytes ||
      raw_count > limits->max_parameters ||
      limits->max_parameters == 0u)
    return orm_mysql_fail(
        error,
        sql_size > limits->max_query_bytes ||
                raw_count > limits->max_parameters
            ? ORM_STATUS_LIMIT_EXCEEDED
            : ORM_STATUS_INVALID_ARGUMENT,
        "MySQL RAW SQL exceeds configured bounds");

  if (sql_size == SIZE_MAX)
    return orm_mysql_fail(
        error, ORM_STATUS_LIMIT_EXCEEDED,
        "MySQL SQL size exceeds platform range");
  out->sql = (uint8_t *)malloc(sql_size + 1u);
  if (out->sql == NULL)
    return orm_mysql_fail(
        error, ORM_STATUS_OUT_OF_MEMORY,
        "allocate MySQL lowered SQL");

  bind_capacity = sql_size < limits->max_parameters
                      ? sql_size
                      : limits->max_parameters;
  bind_order = (uint32_t *)calloc(
      bind_capacity != 0u ? bind_capacity : 1u,
      sizeof(*bind_order));
  if (bind_order == NULL) {
    status = orm_mysql_fail(
        error, ORM_STATUS_OUT_OF_MEMORY,
        "allocate MySQL parameter order");
    goto fail;
  }

  memset(&options, 0, sizeof(options));
  options.max_parameters =
      limits->max_parameters > UINT32_MAX
          ? UINT32_MAX
          : (uint32_t)limits->max_parameters;
  wire_status = mysql_parameter_lower(
      (const uint8_t *)plan->raw_sql, sql_size,
      &options, out->sql, sql_size, &out->sql_size,
      bind_order, bind_capacity, &lowered);
  if (wire_status != MYSQL_WIRE_STATUS_OK) {
    status = orm_mysql_fail(
        error,
        wire_status == MYSQL_WIRE_STATUS_LIMIT
            ? ORM_STATUS_LIMIT_EXCEEDED
            : ORM_STATUS_INVALID_ARGUMENT,
        "invalid MySQL RAW SQL parameter markers");
    goto fail;
  }
  out->sql[out->sql_size] = 0u;

  if (lowered.style == MYSQL_PARAMETER_STYLE_NATIVE &&
      lowered.bind_count != raw_count) {
    status = orm_mysql_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "native MySQL parameter count does not match bound values");
    goto fail;
  }
  if (lowered.bind_count == 0u && raw_count != 0u) {
    status = orm_mysql_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "MySQL RAW SQL has bound values but no parameter markers");
    goto fail;
  }

  for (i = 0u; i < lowered.bind_count; ++i) {
    const size_t index = (size_t)bind_order[i];
    if (index == 0u || index > raw_count) {
      status = orm_mysql_fail(
          error, ORM_STATUS_INVALID_ARGUMENT,
          "MySQL parameter marker references a missing bound value");
      goto fail;
    }
    if (index > max_index)
      max_index = index;
  }
  if (lowered.style == MYSQL_PARAMETER_STYLE_PORTABLE &&
      max_index != raw_count) {
    status = orm_mysql_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "portable MySQL parameter bindings contain unused trailing values");
    goto fail;
  }

  out->value_count = lowered.bind_count;
  if (out->value_count != 0u) {
    if (out->value_count > SIZE_MAX / sizeof(*out->values)) {
      status = orm_mysql_fail(
          error, ORM_STATUS_LIMIT_EXCEEDED,
          "MySQL parameter array exceeds platform range");
      goto fail;
    }
    out->values = (mysql_stmt_value_t *)calloc(
        out->value_count, sizeof(*out->values));
    if (out->values == NULL) {
      status = orm_mysql_fail(
          error, ORM_STATUS_OUT_OF_MEMORY,
          "allocate MySQL parameters");
      goto fail;
    }

    if (execute_bytes > SIZE_MAX - (out->value_count + 7u) / 8u - 1u ||
        execute_bytes + (out->value_count + 7u) / 8u + 1u >
            SIZE_MAX - out->value_count * 2u) {
      status = orm_mysql_fail(
          error, ORM_STATUS_LIMIT_EXCEEDED,
          "MySQL execute command exceeds platform range");
      goto fail;
    }
    execute_bytes +=
        (out->value_count + 7u) / 8u + 1u + out->value_count * 2u;

    for (i = 0u; i < out->value_count; ++i) {
      const orm_owned_value *value =
          (const orm_owned_value *)vec_at_const(
              &plan->raw_parameters, (size_t)bind_order[i] - 1u);
      size_t encoded = 0u;
      status = orm_mysql_map_value(
          value, &out->values[i], &encoded, error);
      if (status != ORM_STATUS_OK)
        goto fail;
      if (encoded > SIZE_MAX - execute_bytes) {
        status = orm_mysql_fail(
            error, ORM_STATUS_LIMIT_EXCEEDED,
            "MySQL execute payload exceeds platform range");
        goto fail;
      }
      execute_bytes += encoded;
    }
  }

  out->command_bytes =
      out->sql_size + 1u > execute_bytes
          ? out->sql_size + 1u
          : execute_bytes;
  if (out->command_bytes < ORM_MYSQL_CONTROL_COMMAND_BYTES)
    out->command_bytes = ORM_MYSQL_CONTROL_COMMAND_BYTES;
  if (out->command_bytes > (size_t)MYSQL_WIRE_PACKET_MAX_PAYLOAD) {
    status = orm_mysql_fail(
        error, ORM_STATUS_LIMIT_EXCEEDED,
        "MySQL prepared command exceeds one protocol packet");
    goto fail;
  }

  free(bind_order);
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;

fail:
  free(bind_order);
  orm_mysql_prepared_destroy(out);
  return status;
}


static orm_status_t orm_mysql_prepare_structured(
    const orm_query_plan *plan, const orm_limits *limits,
    orm_mysql_prepared *out, orm_error_t *error) {
  orm_mysql_rendered_query rendered;
  size_t execute_bytes = 10u;
  size_t sql_size;
  size_t i;
  orm_status_t status;

  if (out != NULL)
    memset(out, 0, sizeof(*out));
  if (plan == NULL || limits == NULL || out == NULL ||
      plan->kind == ORM_QUERY_RAW)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "invalid MySQL structured prepare request");

  memset(&rendered, 0, sizeof(rendered));
  status = orm_mysql_render_plan(
      plan, limits, &rendered, error);
  if (status != ORM_STATUS_OK)
    return status;

  sql_size = tstr_len(rendered.text);
  if (sql_size == 0u || sql_size > limits->max_query_bytes ||
      sql_size == SIZE_MAX) {
    status = orm_mysql_fail(
        error,
        sql_size > limits->max_query_bytes
            ? ORM_STATUS_LIMIT_EXCEEDED
            : ORM_STATUS_INVALID_ARGUMENT,
        "MySQL structured SQL exceeds configured bounds");
    goto fail;
  }

  out->sql = (uint8_t *)malloc(sql_size + 1u);
  if (out->sql == NULL) {
    status = orm_mysql_fail(
        error, ORM_STATUS_OUT_OF_MEMORY,
        "allocate MySQL structured SQL");
    goto fail;
  }
  memcpy(out->sql, rendered.text, sql_size);
  out->sql[sql_size] = 0u;
  out->sql_size = sql_size;
  out->value_count = rendered.parameter_count;

  if (out->value_count != 0u) {
    if (out->value_count > limits->max_parameters ||
        out->value_count > SIZE_MAX / sizeof(*out->values)) {
      status = orm_mysql_fail(
          error, ORM_STATUS_LIMIT_EXCEEDED,
          "MySQL structured parameter count exceeds configured bounds");
      goto fail;
    }
    out->values = (mysql_stmt_value_t *)calloc(
        out->value_count, sizeof(*out->values));
    if (out->values == NULL) {
      status = orm_mysql_fail(
          error, ORM_STATUS_OUT_OF_MEMORY,
          "allocate MySQL structured parameters");
      goto fail;
    }

    if (execute_bytes >
            SIZE_MAX - (out->value_count + 7u) / 8u - 1u ||
        execute_bytes + (out->value_count + 7u) / 8u + 1u >
            SIZE_MAX - out->value_count * 2u) {
      status = orm_mysql_fail(
          error, ORM_STATUS_LIMIT_EXCEEDED,
          "MySQL structured execute command exceeds platform range");
      goto fail;
    }
    execute_bytes +=
        (out->value_count + 7u) / 8u + 1u +
        out->value_count * 2u;

    for (i = 0u; i < out->value_count; ++i) {
      size_t encoded = 0u;
      status = orm_mysql_map_value(
          rendered.parameters[i], &out->values[i],
          &encoded, error);
      if (status != ORM_STATUS_OK)
        goto fail;
      if (encoded > SIZE_MAX - execute_bytes) {
        status = orm_mysql_fail(
            error, ORM_STATUS_LIMIT_EXCEEDED,
            "MySQL structured execute payload exceeds platform range");
        goto fail;
      }
      execute_bytes += encoded;
    }
  }

  out->command_bytes =
      out->sql_size + 1u > execute_bytes
          ? out->sql_size + 1u
          : execute_bytes;
  if (out->command_bytes < ORM_MYSQL_CONTROL_COMMAND_BYTES)
    out->command_bytes = ORM_MYSQL_CONTROL_COMMAND_BYTES;
  if (out->command_bytes >
      (size_t)MYSQL_WIRE_PACKET_MAX_PAYLOAD) {
    status = orm_mysql_fail(
        error, ORM_STATUS_LIMIT_EXCEEDED,
        "MySQL structured prepared command exceeds one protocol packet");
    goto fail;
  }

  orm_mysql_rendered_query_destroy(&rendered);
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;

fail:
  orm_mysql_rendered_query_destroy(&rendered);
  orm_mysql_prepared_destroy(out);
  return status;
}

static orm_status_t orm_mysql_guard_managed_transaction_plan(
    const orm_query_plan *plan, orm_error_t *error) {
  if (plan == NULL)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "MySQL query plan is required");
  if (plan->kind != ORM_QUERY_RAW)
    return ORM_STATUS_OK;
  if (plan->raw_sql == NULL)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "MySQL RAW SQL is required");
  if (!mysql_sql_reject_managed_transaction(
          (const uint8_t *)plan->raw_sql,
          tstr_len(plan->raw_sql)))
    return ORM_STATUS_OK;
  return orm_mysql_fail(
      error, ORM_STATUS_UNSUPPORTED,
      "MySQL managed transactions reject implicit-commit or transaction-control RAW SQL");
}

static orm_status_t orm_mysql_prepare_plan(
    const orm_query_plan *plan, const orm_limits *limits,
    orm_mysql_prepared *out, orm_error_t *error) {
  if (plan == NULL)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "MySQL query plan is required");
  return plan->kind == ORM_QUERY_RAW
             ? orm_mysql_prepare_raw(plan, limits, out, error)
             : orm_mysql_prepare_structured(
                   plan, limits, out, error);
}

static size_t orm_mysql_size_limit(uint64_t value) {
  return value > (uint64_t)SIZE_MAX ? SIZE_MAX : (size_t)value;
}

static orm_status_t orm_mysql_open_impl(
    orm_mysql_backend_state *state,
    const orm_query_plan *plan, const orm_limits *limits,
    int allow_transaction,
    orm_row_cursor *out_cursor, orm_error_t *error) {
  orm_mysql_prepared prepared;
  mysql_session_config_t config;
  mysql_session_cursor_limits_t source_limits;
  mysql_cursor_source_t source = {0};
  const mysql_column_definition_t *columns = NULL;
  size_t column_count = 0u;
  mysql_cursor_config_t cursor_config;
  mysql_session_error_t native_error;
  mysql_session_status_t native_status;
  orm_status_t status;

  if (state == NULL || plan == NULL || limits == NULL ||
      out_cursor == NULL)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "invalid MySQL cursor request");
  memset(out_cursor, 0, sizeof(*out_cursor));
  if (!allow_transaction && state->transaction_active)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_STATE,
        "use the MySQL transaction handle while a transaction is active");

  status = orm_mysql_prepare_plan(plan, limits, &prepared, error);
  if (status != ORM_STATUS_OK)
    return status;

  config = orm_mysql_session_config(&state->settings);
  source_limits.max_result_rows = limits->max_result_rows;
  source_limits.max_columns = limits->max_columns;
  source_limits.max_metadata_bytes =
      orm_mysql_size_limit(
          limits->max_result_bytes < (uint64_t)limits->max_query_bytes
              ? limits->max_result_bytes
              : (uint64_t)limits->max_query_bytes);
  source_limits.max_row_bytes =
      orm_mysql_size_limit(limits->max_result_bytes);
  source_limits.max_command_bytes = prepared.command_bytes;
  if (source_limits.max_columns == 0u ||
      source_limits.max_metadata_bytes == 0u ||
      source_limits.max_row_bytes == 0u) {
    orm_mysql_prepared_destroy(&prepared);
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "MySQL cursor limits must be nonzero");
  }

  memset(&native_error, 0, sizeof(native_error));
  native_status = mysql_session_open_prepared_source(
      &config, prepared.sql, prepared.sql_size,
      prepared.values, prepared.value_count,
      &source_limits, &source, &columns, &column_count,
      &native_error);
  orm_mysql_prepared_destroy(&prepared);
  if (native_status != MYSQL_SESSION_OK)
    return orm_mysql_session_status(
        native_status, &native_error, error);

  cursor_config = (mysql_cursor_config_t)MYSQL_CURSOR_CONFIG_INIT(
      limits->max_result_rows, limits->max_result_bytes,
      limits->max_columns,
      source_limits.max_metadata_bytes,
      source_limits.max_row_bytes);
  status = mysql_cursor_start(
      out_cursor, &source, columns, column_count,
      &cursor_config, error);
  if (status != ORM_STATUS_OK &&
      source.ops != NULL && source.context != NULL)
    source.ops->destroy(source.context);
  return status;
}

static orm_status_t orm_mysql_execute_with_session(
    const orm_mysql_settings *settings,
    const orm_query_plan *plan, const orm_limits *limits,
    mysql_transaction_session_t *transaction,
    uint64_t *affected_rows, orm_error_t *error) {
  orm_mysql_prepared prepared;
  mysql_session_command_result_t result;
  mysql_session_error_t native_error;
  mysql_session_status_t native_status;
  orm_status_t status;

  if (affected_rows != NULL)
    *affected_rows = 0u;
  status = orm_mysql_prepare_plan(plan, limits, &prepared, error);
  if (status != ORM_STATUS_OK)
    return status;

  memset(&native_error, 0, sizeof(native_error));
  memset(&result, 0, sizeof(result));
  if (transaction != NULL) {
    native_status = mysql_transaction_session_execute_prepared(
        transaction, prepared.sql, prepared.sql_size,
        prepared.values, prepared.value_count,
        &result, &native_error);
  } else {
    const mysql_session_config_t config =
        orm_mysql_session_config(settings);
    native_status = mysql_session_execute_prepared(
        &config, prepared.sql, prepared.sql_size,
        prepared.values, prepared.value_count,
        prepared.command_bytes, &result, &native_error);
  }
  orm_mysql_prepared_destroy(&prepared);

  if (native_status != MYSQL_SESSION_OK)
    return orm_mysql_session_status(
        native_status, &native_error, error);
  if (affected_rows != NULL)
    *affected_rows = result.affected_rows;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

static void orm_mysql_backend_destroy(void *context) {
  orm_mysql_backend_state *state =
      (orm_mysql_backend_state *)context;
  if (state == NULL)
    return;
  orm_mysql_settings_destroy(&state->settings);
  free(state);
}

static orm_status_t orm_mysql_backend_open(
    void *context, const orm_query_plan *plan,
    const orm_limits *limits, orm_row_cursor *out_cursor,
    orm_error_t *error) {
  return orm_mysql_open_impl(
      (orm_mysql_backend_state *)context,
      plan, limits, 0, out_cursor, error);
}

static orm_status_t orm_mysql_backend_execute(
    void *context, const orm_query_plan *plan,
    const orm_limits *limits, uint64_t *affected_rows,
    orm_error_t *error) {
  orm_mysql_backend_state *state =
      (orm_mysql_backend_state *)context;
  if (state == NULL)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "invalid MySQL command request");
  if (state->transaction_active)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_STATE,
        "use the MySQL transaction handle while a transaction is active");
  return orm_mysql_execute_with_session(
      &state->settings, plan, limits, NULL,
      affected_rows, error);
}

typedef struct orm_mysql_async_input {
  orm_mysql_prepared prepared;
  mysql_session_config_t config;
} orm_mysql_async_input;

static void orm_mysql_async_input_release(void *context) {
  orm_mysql_async_input *input = context;
  orm_mysql_prepared_destroy(&input->prepared);
  free(input);
}

static orm_status_t orm_mysql_backend_open_async(
    void *context, const orm_query_plan *plan, const orm_limits *limits,
    const orm_async_config_t *async_config, orm_row_cursor *out,
    orm_error_t *error) {
  orm_mysql_backend_state *backend = context;
  if (backend == NULL || plan == NULL || limits == NULL || async_config == NULL || out == NULL)
    return orm_mysql_fail(error, ORM_STATUS_INVALID_ARGUMENT, "invalid MySQL async query");
  if (backend->transaction_active)
    return orm_mysql_fail(error, ORM_STATUS_BUSY, "MySQL transaction already active");
  orm_mysql_async_input *input = calloc(1u, sizeof(*input));
  if (input == NULL)
    return orm_mysql_fail(error, ORM_STATUS_OUT_OF_MEMORY, "allocate MySQL async input");
  orm_status_t status = orm_mysql_prepare_plan(plan, limits, &input->prepared, error);
  if (status != ORM_STATUS_OK) { free(input); return status; }
  input->config = orm_mysql_session_config(&backend->settings);
  const mysql_session_cursor_limits_t source_limits = {
      limits->max_result_rows, limits->max_columns,
      orm_mysql_size_limit(limits->max_result_bytes < limits->max_query_bytes
          ? limits->max_result_bytes : limits->max_query_bytes),
      orm_mysql_size_limit(limits->max_result_bytes), input->prepared.command_bytes};
  mysql_async_source source = {0};
  mysql_session_error_t native_error = {0};
  const mysql_session_status_t native_status = mysql_session_start_async_source(
      &input->config, input->prepared.sql, input->prepared.sql_size,
      input->prepared.values, input->prepared.value_count, &source_limits,
      &source, &native_error);
  if (native_status != MYSQL_SESSION_OK) {
    orm_mysql_async_input_release(input);
    return orm_mysql_session_status(native_status, &native_error, error);
  }
  const mysql_cursor_config_t cursor_config = MYSQL_CURSOR_CONFIG_INIT(
      limits->max_result_rows, limits->max_result_bytes, limits->max_columns,
      source_limits.max_metadata_bytes, source_limits.max_row_bytes);
  const mysql_cursor_async_owner owner = {input, orm_mysql_async_input_release};
  status = mysql_cursor_start_async(out, &source, &cursor_config, async_config, owner, error);
  if (status != ORM_STATUS_OK) {
    mysql_session_async_destroy(&source);
    orm_mysql_async_input_release(input);
  }
  return status;
}

static void orm_mysql_transaction_destroy(void *context) {
  orm_mysql_transaction_state *transaction =
      (orm_mysql_transaction_state *)context;
  if (transaction == NULL)
    return;
  mysql_transaction_session_destroy(transaction->session);
  if (transaction->owner != NULL)
    transaction->owner->transaction_active = 0;
  free(transaction);
}

static orm_status_t orm_mysql_transaction_open(
    void *context, const orm_query_plan *plan,
    const orm_limits *limits, orm_row_cursor *out_cursor,
    orm_error_t *error) {
  orm_mysql_transaction_state *transaction =
      (orm_mysql_transaction_state *)context;
  orm_mysql_prepared prepared;
  mysql_session_cursor_limits_t source_limits;
  mysql_cursor_source_t source = {0};
  const mysql_column_definition_t *columns = NULL;
  size_t column_count = 0u;
  mysql_cursor_config_t cursor_config;
  mysql_session_error_t native_error;
  mysql_session_status_t native_status;
  orm_status_t status;

  if (out_cursor != NULL)
    memset(out_cursor, 0, sizeof(*out_cursor));
  if (transaction == NULL || !transaction->active ||
      transaction->session == NULL || plan == NULL ||
      limits == NULL || out_cursor == NULL)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_STATE,
        "MySQL transaction is no longer active");

  status = orm_mysql_guard_managed_transaction_plan(
      plan, error);
  if (status != ORM_STATUS_OK)
    return status;

  status = orm_mysql_prepare_plan(
      plan, limits, &prepared, error);
  if (status != ORM_STATUS_OK)
    return status;

  memset(&source_limits, 0, sizeof(source_limits));
  source_limits.max_result_rows = limits->max_result_rows;
  source_limits.max_columns = limits->max_columns;
  source_limits.max_metadata_bytes =
      orm_mysql_size_limit(
          limits->max_result_bytes < (uint64_t)limits->max_query_bytes
              ? limits->max_result_bytes
              : (uint64_t)limits->max_query_bytes);
  source_limits.max_row_bytes =
      orm_mysql_size_limit(limits->max_result_bytes);
  source_limits.max_command_bytes = prepared.command_bytes;
  if (source_limits.max_result_rows == 0u ||
      source_limits.max_columns == 0u ||
      source_limits.max_metadata_bytes == 0u ||
      source_limits.max_row_bytes == 0u) {
    orm_mysql_prepared_destroy(&prepared);
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "MySQL transaction cursor limits must be nonzero");
  }

  memset(&native_error, 0, sizeof(native_error));
  native_status =
      mysql_transaction_session_open_prepared_source(
          transaction->session,
          prepared.sql, prepared.sql_size,
          prepared.values, prepared.value_count,
          &source_limits, &source, &columns,
          &column_count, &native_error);
  orm_mysql_prepared_destroy(&prepared);
  if (native_status != MYSQL_SESSION_OK)
    return orm_mysql_session_status(
        native_status, &native_error, error);

  cursor_config = (mysql_cursor_config_t)MYSQL_CURSOR_CONFIG_INIT(
      limits->max_result_rows, limits->max_result_bytes,
      limits->max_columns,
      source_limits.max_metadata_bytes,
      source_limits.max_row_bytes);
  status = mysql_cursor_start(
      out_cursor, &source, columns, column_count,
      &cursor_config, error);
  if (status != ORM_STATUS_OK &&
      source.ops != NULL && source.context != NULL)
    source.ops->destroy(source.context);
  return status;
}

static orm_status_t orm_mysql_transaction_execute(
    void *context, const orm_query_plan *plan,
    const orm_limits *limits, uint64_t *affected_rows,
    orm_error_t *error) {
  orm_mysql_transaction_state *transaction =
      (orm_mysql_transaction_state *)context;
  orm_status_t status;
  if (transaction == NULL || !transaction->active ||
      transaction->session == NULL)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_STATE,
        "MySQL transaction is no longer active");

  status = orm_mysql_guard_managed_transaction_plan(
      plan, error);
  if (status != ORM_STATUS_OK)
    return status;

  return orm_mysql_execute_with_session(
      &transaction->owner->settings,
      plan, limits, transaction->session,
      affected_rows, error);
}

static orm_status_t orm_mysql_transaction_finish(
    orm_mysql_transaction_state *transaction, int commit,
    orm_error_t *error) {
  mysql_session_error_t native_error;
  mysql_session_status_t native_status;
  orm_status_t status;

  if (transaction == NULL || !transaction->active ||
      transaction->session == NULL)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_STATE,
        "MySQL transaction is no longer active");

  memset(&native_error, 0, sizeof(native_error));
  native_status = commit
      ? mysql_transaction_session_commit(
            transaction->session, &native_error)
      : mysql_transaction_session_rollback(
            transaction->session, &native_error);
  status = orm_mysql_session_status(
      native_status, &native_error, error);
  if (native_status == MYSQL_SESSION_OK ||
      native_status == MYSQL_SESSION_COMMIT_UNKNOWN) {
    transaction->active = 0;
    transaction->owner->transaction_active = 0;
  }
  return status;
}

static orm_status_t orm_mysql_transaction_commit(
    void *context, orm_error_t *error) {
  return orm_mysql_transaction_finish(
      (orm_mysql_transaction_state *)context, 1, error);
}

static orm_status_t orm_mysql_transaction_rollback(
    void *context, orm_error_t *error) {
  return orm_mysql_transaction_finish(
      (orm_mysql_transaction_state *)context, 0, error);
}

static orm_status_t orm_mysql_transaction_savepoint_impl(
    orm_mysql_transaction_state *transaction, vstr name,
    int operation, orm_error_t *error) {
  mysql_session_error_t native_error;
  mysql_session_status_t native_status;

  if (transaction == NULL || !transaction->active ||
      transaction->session == NULL ||
      !orm_view_valid(name, false))
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_STATE,
        "MySQL transaction is no longer active");

  memset(&native_error, 0, sizeof(native_error));
  if (operation == 0) {
    native_status = mysql_transaction_session_savepoint(
        transaction->session,
        (const uint8_t *)name.data, name.len, &native_error);
  } else if (operation == 1) {
    native_status = mysql_transaction_session_rollback_to_savepoint(
        transaction->session,
        (const uint8_t *)name.data, name.len, &native_error);
  } else {
    native_status = mysql_transaction_session_release_savepoint(
        transaction->session,
        (const uint8_t *)name.data, name.len, &native_error);
  }
  return orm_mysql_session_status(
      native_status, &native_error, error);
}

static orm_status_t orm_mysql_transaction_savepoint(
    void *context, vstr name, orm_error_t *error) {
  return orm_mysql_transaction_savepoint_impl(
      (orm_mysql_transaction_state *)context, name, 0, error);
}

static orm_status_t orm_mysql_transaction_rollback_to(
    void *context, vstr name, orm_error_t *error) {
  return orm_mysql_transaction_savepoint_impl(
      (orm_mysql_transaction_state *)context, name, 1, error);
}

static orm_status_t orm_mysql_transaction_release(
    void *context, vstr name, orm_error_t *error) {
  return orm_mysql_transaction_savepoint_impl(
      (orm_mysql_transaction_state *)context, name, 2, error);
}

static const orm_transaction_backend_ops orm_mysql_transaction_ops = {
    sizeof(orm_transaction_backend_ops),
    ORM_TRANSACTION_BACKEND_OPS_ABI_VERSION,
    orm_mysql_transaction_destroy,
    orm_mysql_transaction_open,
    orm_mysql_transaction_execute,
    orm_mysql_transaction_commit,
    orm_mysql_transaction_rollback,
    orm_mysql_transaction_savepoint,
    orm_mysql_transaction_rollback_to,
    orm_mysql_transaction_release};

static size_t orm_mysql_command_limit(const orm_limits *limits) {
  uint64_t value;
  uint64_t parameter_bytes;
  uint64_t parameter_overhead;

  if (limits == NULL)
    return 0u;

  value = (uint64_t)limits->max_query_bytes;
  if (value < ORM_MYSQL_CONTROL_COMMAND_BYTES)
    value = ORM_MYSQL_CONTROL_COMMAND_BYTES;
  if (value >= MYSQL_WIRE_PACKET_MAX_PAYLOAD)
    return (size_t)MYSQL_WIRE_PACKET_MAX_PAYLOAD;

  if (limits->max_parameters != 0u &&
      (uint64_t)limits->max_parameter_bytes >
          UINT64_MAX / (uint64_t)limits->max_parameters)
    return (size_t)MYSQL_WIRE_PACKET_MAX_PAYLOAD;
  parameter_bytes =
      (uint64_t)limits->max_parameter_bytes *
      (uint64_t)limits->max_parameters;

  if ((uint64_t)limits->max_parameters >
      (UINT64_MAX - UINT64_C(64)) / UINT64_C(3))
    return (size_t)MYSQL_WIRE_PACKET_MAX_PAYLOAD;
  parameter_overhead =
      UINT64_C(64) + (uint64_t)limits->max_parameters * UINT64_C(3);

  if (parameter_bytes >
          (uint64_t)MYSQL_WIRE_PACKET_MAX_PAYLOAD - value ||
      parameter_overhead >
          (uint64_t)MYSQL_WIRE_PACKET_MAX_PAYLOAD -
              value -
              (parameter_bytes <=
                       (uint64_t)MYSQL_WIRE_PACKET_MAX_PAYLOAD - value
                   ? parameter_bytes
                   : 0u))
    return (size_t)MYSQL_WIRE_PACKET_MAX_PAYLOAD;

  value += parameter_bytes + parameter_overhead;
  if (value > MYSQL_WIRE_PACKET_MAX_PAYLOAD)
    value = MYSQL_WIRE_PACKET_MAX_PAYLOAD;
  return (size_t)value;
}

static orm_status_t orm_mysql_backend_begin(
    void *context, orm_isolation_t isolation,
    orm_transaction_backend *out_transaction,
    orm_error_t *error) {
  orm_mysql_backend_state *state =
      (orm_mysql_backend_state *)context;
  orm_mysql_transaction_state *transaction = NULL;
  mysql_transaction_session_t *session = NULL;
  mysql_session_error_t native_error;
  mysql_session_config_t config;
  mysql_session_status_t native_status;
  size_t command_limit;
  mysql_isolation_t native_isolation;

  if (out_transaction != NULL)
    memset(out_transaction, 0, sizeof(*out_transaction));
  if (state == NULL || out_transaction == NULL)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "invalid MySQL transaction request");
  if (state->transaction_active)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_STATE,
        "MySQL connection already has an active transaction");

  command_limit = orm_mysql_command_limit(&state->limits);
  if (command_limit == 0u)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "invalid MySQL transaction command limit");

  switch (isolation) {
    case ORM_ISOLATION_READ_UNCOMMITTED:
      native_isolation = MYSQL_ISOLATION_READ_UNCOMMITTED;
      break;
    case ORM_ISOLATION_READ_COMMITTED:
      native_isolation = MYSQL_ISOLATION_READ_COMMITTED;
      break;
    case ORM_ISOLATION_REPEATABLE_READ:
      native_isolation = MYSQL_ISOLATION_REPEATABLE_READ;
      break;
    case ORM_ISOLATION_SERIALIZABLE:
      native_isolation = MYSQL_ISOLATION_SERIALIZABLE;
      break;
    default:
      return orm_mysql_fail(
          error, ORM_STATUS_INVALID_ARGUMENT,
          isolation == ORM_ISOLATION_SNAPSHOT
              ? "MySQL does not expose ORM snapshot isolation"
              : "invalid transaction request or command bound");
  }

  config = orm_mysql_session_config(&state->settings);
  memset(&native_error, 0, sizeof(native_error));
  native_status = mysql_transaction_session_begin(
      &config, native_isolation, command_limit,
      &session, &native_error);
  if (native_status != MYSQL_SESSION_OK)
    return orm_mysql_session_status(
        native_status, &native_error, error);

  transaction = (orm_mysql_transaction_state *)calloc(
      1u, sizeof(*transaction));
  if (transaction == NULL) {
    mysql_transaction_session_destroy(session);
    return orm_mysql_fail(
        error, ORM_STATUS_OUT_OF_MEMORY,
        "allocate MySQL Driver transaction");
  }

  transaction->owner = state;
  transaction->session = session;
  transaction->active = 1;
  state->transaction_active = 1;

  out_transaction->ops = &orm_mysql_transaction_ops;
  out_transaction->context = transaction;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}

static const orm_backend_ops orm_mysql_backend_ops = {
    sizeof(orm_backend_ops),
    ORM_BACKEND_OPS_ABI_VERSION,
    orm_mysql_backend_destroy,
    orm_mysql_backend_open,
    orm_mysql_backend_execute,
    orm_mysql_backend_begin,
    orm_mysql_backend_open_async};

orm_status_t orm_mysql_backend_create(
    const orm_config_t *config,
    const orm_limits *limits,
    orm_backend *out_backend,
    orm_error_t *error) {
  orm_mysql_backend_state *state = NULL;
  mysql_session_config_t session_config;
  mysql_session_error_t native_error;
  mysql_session_status_t native_status;
  orm_status_t status;

  if (config == NULL || limits == NULL || out_backend == NULL)
    return orm_mysql_fail(
        error, ORM_STATUS_INVALID_ARGUMENT,
        "invalid MySQL Driver backend request");
  memset(out_backend, 0, sizeof(*out_backend));

  state = (orm_mysql_backend_state *)calloc(1u, sizeof(*state));
  if (state == NULL)
    return orm_mysql_fail(
        error, ORM_STATUS_OUT_OF_MEMORY,
        "allocate MySQL Driver backend");

  status = orm_mysql_settings_parse(
      config, &state->settings, error);
  if (status != ORM_STATUS_OK)
    goto fail;
  state->limits = *limits;

  session_config = orm_mysql_session_config(&state->settings);
  memset(&native_error, 0, sizeof(native_error));
  native_status = mysql_session_connect_and_ping(
      &session_config, &native_error);
  if (native_status != MYSQL_SESSION_OK) {
    status = orm_mysql_session_status(
        native_status, &native_error, error);
    goto fail;
  }

  out_backend->ops = &orm_mysql_backend_ops;
  out_backend->context = state;
  orm_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;

fail:
  orm_mysql_backend_destroy(state);
  return status;
}
