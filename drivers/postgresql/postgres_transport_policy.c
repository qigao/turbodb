#include "postgres_transport_policy.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void policy_error_set(orm_error_t *error, orm_status_t status,
                             const char *message) {
  if (error == NULL)
    return;
  error->status = status;
  if (message == NULL) {
    error->message[0] = '\0';
    return;
  }
  (void)snprintf(error->message, sizeof(error->message), "%s", message);
}

static int view_equal(orm_string_view_t value, const char *literal) {
  const size_t size = strlen(literal);
  return value.len == size &&
         (size == 0u || memcmp(value.data, literal, size) == 0);
}

static orm_status_t copy_text(orm_string_view_t value, char *out, size_t capacity,
                              const char *message, orm_error_t *error) {
  if (value.data == NULL || value.len == 0u || value.len >= capacity ||
      memchr(value.data, '\0', value.len) != NULL) {
    policy_error_set(error, ORM_STATUS_INVALID_ARGUMENT, message);
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memcpy(out, value.data, value.len);
  out[value.len] = '\0';
  return ORM_STATUS_OK;
}

static orm_status_t parse_u32(orm_string_view_t value, uint32_t min_value,
                              uint32_t max_value, uint32_t *out,
                              const char *message, orm_error_t *error) {
  char buffer[32];
  char *end = NULL;
  unsigned long parsed;
  if (value.data == NULL || value.len == 0u || value.len >= sizeof(buffer) ||
      memchr(value.data, '\0', value.len) != NULL) {
    policy_error_set(error, ORM_STATUS_INVALID_ARGUMENT, message);
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  memcpy(buffer, value.data, value.len);
  buffer[value.len] = '\0';
  errno = 0;
  parsed = strtoul(buffer, &end, 10);
  if (errno != 0 || end == buffer || *end != '\0' ||
      parsed < min_value || parsed > max_value) {
    policy_error_set(error, ORM_STATUS_OUT_OF_RANGE, message);
    return ORM_STATUS_OUT_OF_RANGE;
  }
  *out = (uint32_t)parsed;
  return ORM_STATUS_OK;
}

int orm_postgres_transport_option_name(orm_string_view_t keyword) {
  static const char prefix[] = "turbodb_pg_";
  return keyword.data != NULL && keyword.len >= sizeof(prefix) - 1u &&
         memcmp(keyword.data, prefix, sizeof(prefix) - 1u) == 0;
}

orm_status_t orm_postgres_transport_policy_parse(
    const orm_config_t *config, orm_postgres_transport_policy *out,
    orm_error_t *error) {
  uint32_t seen = 0u;
  uint32_t index;
  int has_transport = 0;
  int has_remote_host = 0;
  int has_remote_port = 0;
  int has_server_name = 0;
  int has_ca_file = 0;
  int has_ca_path = 0;

  if (config == NULL || out == NULL || error == NULL) return ORM_STATUS_INVALID_ARGUMENT;
  *out = (orm_postgres_transport_policy)ORM_POSTGRES_TRANSPORT_POLICY_INIT;

  for (index = 0u; index < config->option_count; ++index) {
    const orm_option_t *option = &config->options[index];
    uint32_t bit = 0u;
    orm_status_t status = ORM_STATUS_OK;

    if (!orm_postgres_transport_option_name(option->keyword)) continue;

    if (view_equal(option->keyword, "turbodb_pg_transport")) {
      bit = UINT32_C(1) << 0;
      has_transport = 1;
      if (view_equal(option->value, "disabled"))
        out->mode = ORM_POSTGRES_TRANSPORT_DISABLED;
      else if (view_equal(option->value, "direct_tls"))
        out->mode = ORM_POSTGRES_TRANSPORT_DIRECT_TLS;
      else {
        policy_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                      "invalid PostgreSQL transport mode");
        return ORM_STATUS_INVALID_ARGUMENT;
      }
    } else if (view_equal(option->keyword, "turbodb_pg_remote_host")) {
      bit = UINT32_C(1) << 1;
      has_remote_host = 1;
      status = copy_text(option->value, out->remote_host, sizeof(out->remote_host),
                         "invalid PostgreSQL secure remote host", error);
    } else if (view_equal(option->keyword, "turbodb_pg_remote_port")) {
      uint32_t port = 0u;
      bit = UINT32_C(1) << 2;
      has_remote_port = 1;
      status = parse_u32(option->value, 1u, 65535u, &port,
                         "invalid PostgreSQL secure remote port", error);
      out->remote_port = (uint16_t)port;
    } else if (view_equal(option->keyword, "turbodb_pg_server_name")) {
      bit = UINT32_C(1) << 3;
      has_server_name = 1;
      status = copy_text(option->value, out->server_name, sizeof(out->server_name),
                         "invalid PostgreSQL secure server name", error);
    } else if (view_equal(option->keyword, "turbodb_pg_ca_file")) {
      bit = UINT32_C(1) << 4;
      has_ca_file = 1;
      status = copy_text(option->value, out->ca_file, sizeof(out->ca_file),
                         "invalid PostgreSQL secure CA file", error);
    } else if (view_equal(option->keyword, "turbodb_pg_ca_path")) {
      bit = UINT32_C(1) << 5;
      has_ca_path = 1;
      status = copy_text(option->value, out->ca_path, sizeof(out->ca_path),
                         "invalid PostgreSQL secure CA path", error);
    } else if (view_equal(option->keyword, "turbodb_pg_connect_timeout_ms")) {
      bit = UINT32_C(1) << 6;
      status = parse_u32(option->value, 1u, UINT32_MAX, &out->connect_timeout_ms,
                         "invalid PostgreSQL secure connect timeout", error);
    } else if (view_equal(option->keyword, "turbodb_pg_handshake_timeout_ms")) {
      bit = UINT32_C(1) << 7;
      status = parse_u32(option->value, 1u, UINT32_MAX, &out->handshake_timeout_ms,
                         "invalid PostgreSQL secure handshake timeout", error);
    } else if (view_equal(option->keyword, "turbodb_pg_idle_timeout_ms")) {
      bit = UINT32_C(1) << 8;
      status = parse_u32(option->value, 1u, UINT32_MAX, &out->idle_timeout_ms,
                         "invalid PostgreSQL secure idle timeout", error);
    } else if (view_equal(option->keyword, "turbodb_pg_ingress_bytes")) {
      bit = UINT32_C(1) << 9;
      status = parse_u32(option->value, ORM_POSTGRES_TRANSPORT_MIN_BUFFER_BYTES,
                         ORM_POSTGRES_TRANSPORT_MAX_BUFFER_BYTES,
                         &out->ingress_buffer_bytes,
                         "invalid PostgreSQL secure ingress buffer size", error);
    } else if (view_equal(option->keyword, "turbodb_pg_egress_bytes")) {
      bit = UINT32_C(1) << 10;
      status = parse_u32(option->value, ORM_POSTGRES_TRANSPORT_MIN_BUFFER_BYTES,
                         ORM_POSTGRES_TRANSPORT_MAX_BUFFER_BYTES,
                         &out->egress_buffer_bytes,
                         "invalid PostgreSQL secure egress buffer size", error);
    } else {
      policy_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                    "unknown TurboDB PostgreSQL transport option");
      return ORM_STATUS_INVALID_ARGUMENT;
    }

    if (status != ORM_STATUS_OK) return status;
    if ((seen & bit) != 0u) {
      policy_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                    "duplicate TurboDB PostgreSQL transport option");
      return ORM_STATUS_INVALID_ARGUMENT;
    }
    seen |= bit;
  }

  if (!has_transport) {
    policy_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "PostgreSQL requires explicit turbodb_pg_transport");
    return ORM_STATUS_INVALID_ARGUMENT;
  }

  if (out->mode == ORM_POSTGRES_TRANSPORT_DISABLED) {
    if (seen != (UINT32_C(1) << 0)) {
      policy_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                    "plaintext PostgreSQL transport cannot include secure transport options");
      return ORM_STATUS_INVALID_ARGUMENT;
    }
    policy_error_set(error, ORM_STATUS_OK, NULL);
    return ORM_STATUS_OK;
  }

  if (!has_remote_host || !has_remote_port || !has_server_name ||
      (has_ca_file == has_ca_path)) {
    policy_error_set(error, ORM_STATUS_INVALID_ARGUMENT,
                  "direct TLS PostgreSQL transport requires host, port, server name and exactly one CA source");
    return ORM_STATUS_INVALID_ARGUMENT;
  }

  policy_error_set(error, ORM_STATUS_OK, NULL);
  return ORM_STATUS_OK;
}
