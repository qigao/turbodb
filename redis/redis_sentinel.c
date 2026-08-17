/**
 * @file redis_sentinel.c
 * @brief Redis Sentinel service discovery and failover implementation
 */

#include "redis_sentinel.h"
#include "redis_pool.h"
#include "turbo_error.h"
#include "turbo_str.h"
#include <stdlib.h>
#include <string.h>

#define REDIS_SENTINEL_ENDPOINT_MAX 255u
#define REDIS_SENTINEL_NS_PER_MS UINT64_C(1000000)

typedef struct redis_sentinel_generation_s {
  redis_pool_t *pool;
  tstr_t host;
  uint16_t port;
  size_t active_leases;
  int retired;
  struct redis_sentinel_generation_s *next;
} redis_sentinel_generation_t;

typedef struct {
  redis_sentinel_generation_t *generation;
  redis_pool_conn_t *connection;
  redis_client_t *client;
} redis_sentinel_attempt_t;

struct redis_sentinel_s {
  redis_sentinel_config_t config;
  redis_sentinel_generation_t *current;
  redis_sentinel_generation_t *retired;
  size_t preferred_sentinel;
  uint64_t last_topology_refresh_ns;
  uint64_t control_epoch;
  size_t active_calls;
  int connected;
  int refreshing;
  int closing;
  int destroy_pending;
  redis_sentinel_stats_t stats;
};

static void redis_sentinel_config_clear(redis_sentinel_config_t *config) {
  if (!config) return;
  if (config->sentinel_hosts) {
    for (size_t i = 0; i < config->sentinel_count; ++i)
      tstr_free((tstr_t)config->sentinel_hosts[i]);
    free((char **)config->sentinel_hosts);
  }
  free(config->sentinel_ports);
  tstr_free((tstr_t)config->service_name);
  tstr_free((tstr_t)config->sentinel_username);
  tstr_free((tstr_t)config->sentinel_password);
  tstr_free((tstr_t)config->username);
  tstr_free((tstr_t)config->password);
  memset(config, 0, sizeof(*config));
}

static int redis_sentinel_copy_optional(const char *source, const char **target) {
  if (!target) return TURBO_EINVAL;
  *target = source ? tstr_dup(source) : NULL;
  return source && !*target ? TURBO_ENOMEM : TURBO_OK;
}

static int redis_sentinel_config_copy(redis_sentinel_config_t *target,
                                      const redis_sentinel_config_t *source) {
  char **hosts;
  uint16_t *ports;
  int rc = TURBO_ENOMEM;

  if (!target || !source) return TURBO_EINVAL;
  memset(target, 0, sizeof(*target));
  *target = *source;
  target->sentinel_hosts = NULL;
  target->sentinel_ports = NULL;
  target->service_name = NULL;
  target->sentinel_username = NULL;
  target->sentinel_password = NULL;
  target->username = NULL;
  target->password = NULL;

  hosts = calloc(source->sentinel_count, sizeof(*hosts));
  ports = calloc(source->sentinel_count, sizeof(*ports));
  if (!hosts || !ports) {
    free(hosts);
    free(ports);
    goto fail;
  }
  target->sentinel_hosts = (const char **)hosts;
  target->sentinel_ports = ports;

  for (size_t i = 0; i < source->sentinel_count; ++i) {
    hosts[i] = tstr_dup(source->sentinel_hosts[i]);
    if (!hosts[i]) goto fail;
    ports[i] = source->sentinel_ports[i];
  }

  target->service_name = tstr_dup(source->service_name);
  if (!target->service_name) goto fail;
  if (redis_sentinel_copy_optional(source->sentinel_username, &target->sentinel_username) !=
          TURBO_OK ||
      redis_sentinel_copy_optional(source->sentinel_password, &target->sentinel_password) !=
          TURBO_OK ||
      redis_sentinel_copy_optional(source->username, &target->username) != TURBO_OK ||
      redis_sentinel_copy_optional(source->password, &target->password) != TURBO_OK)
    goto fail;

  if (target->min_connections == 0) target->min_connections = 2;
  if (target->max_connections == 0) target->max_connections = 10;
  if (target->min_connections > target->max_connections)
    target->min_connections = target->max_connections;
  if (target->sentinel_connect_timeout_ms == 0) target->sentinel_connect_timeout_ms = 1000;
  if (target->sentinel_command_timeout_ms == 0) target->sentinel_command_timeout_ms = 2000;
  if (target->connect_timeout_ms == 0) target->connect_timeout_ms = 5000;
  if (target->command_timeout_ms == 0) target->command_timeout_ms = target->connect_timeout_ms;
  if (target->idle_timeout_ms == 0) target->idle_timeout_ms = 60000;
  return TURBO_OK;

fail:
  redis_sentinel_config_clear(target);
  return rc;
}

static int redis_sentinel_config_valid(const redis_sentinel_config_t *config) {
  if (!config || !config->sentinel_hosts || !config->sentinel_ports ||
      config->sentinel_count == 0 || config->sentinel_count > REDIS_SENTINEL_MAX_ENDPOINTS ||
      !config->service_name || !config->service_name[0] || config->database < 0 ||
      config->database > 15 || (config->sentinel_username && !config->sentinel_password) ||
      (config->username && !config->password))
    return 0;
  for (size_t i = 0; i < config->sentinel_count; ++i)
    if (!config->sentinel_hosts[i] || !config->sentinel_hosts[i][0] ||
        config->sentinel_ports[i] == 0)
      return 0;
  return 1;
}

static int redis_sentinel_reply_text_is(const redis_reply_t *reply, const char *expected) {
  size_t expected_len;
  if (!reply || !expected || !reply->str ||
      (reply->type != REDIS_REPLY_STRING && reply->type != REDIS_REPLY_BULK_STRING))
    return 0;
  expected_len = strlen(expected);
  return reply->len == expected_len && memcmp(reply->str, expected, expected_len) == 0;
}

static int redis_sentinel_parse_port(const redis_reply_t *reply, uint16_t *port) {
  uint32_t value = 0;
  if (!port || !reply || !reply->str || reply->len == 0 || reply->len > 5 ||
      (reply->type != REDIS_REPLY_STRING && reply->type != REDIS_REPLY_BULK_STRING))
    return TURBO_EPROTO;
  for (size_t i = 0; i < reply->len; ++i) {
    unsigned char digit = (unsigned char)reply->str[i];
    if (digit < '0' || digit > '9') return TURBO_EPROTO;
    value = value * 10u + (uint32_t)(digit - '0');
    if (value > UINT16_MAX) return TURBO_EPROTO;
  }
  if (value == 0) return TURBO_EPROTO;
  *port = (uint16_t)value;
  return TURBO_OK;
}

static int redis_sentinel_query_master(redis_sentinel_t *sentinel, size_t sentinel_index,
                                       tstr_t *host, uint16_t *port) {
  redis_config_t config = {0};
  redis_client_t *client = NULL;
  redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
  const redis_reply_t *host_reply;
  const redis_reply_t *port_reply;
  const char *argv[] = {"SENTINEL", "get-master-addr-by-name", NULL};
  int rc = TURBO_ENOTCONN;

  if (!sentinel || !host || !port || sentinel_index >= sentinel->config.sentinel_count)
    return TURBO_EINVAL;
  *host = NULL;
  *port = 0;
  argv[2] = sentinel->config.service_name;

  config.host = sentinel->config.sentinel_hosts[sentinel_index];
  config.port = sentinel->config.sentinel_ports[sentinel_index];
  config.username = sentinel->config.sentinel_username;
  config.password = sentinel->config.sentinel_password;
  config.database = 0;
  config.timeout_ms = sentinel->config.sentinel_connect_timeout_ms;
  config.command_timeout_ms = sentinel->config.sentinel_command_timeout_ms;
  config.max_pipeline = 1;

  client = redis_client_create_with_config(&config);
  if (!client) {
    rc = TURBO_ENOMEM;
    goto cleanup;
  }
  rc = redis_client_connect(client, NULL, NULL);
  if (rc != TURBO_OK) goto cleanup;
  rc = redis_commandv_result(client, 3, argv, NULL, &result);
  if (rc != TURBO_OK || !result.reply || result.reply->type != REDIS_REPLY_ARRAY ||
      result.reply->element_count != 2) {
    rc = rc == TURBO_OK ? TURBO_EPROTO : rc;
    goto cleanup;
  }

  host_reply = result.reply->elements[0];
  port_reply = result.reply->elements[1];
  if (!host_reply || !host_reply->str || host_reply->len == 0 ||
      host_reply->len > REDIS_SENTINEL_ENDPOINT_MAX ||
      (host_reply->type != REDIS_REPLY_STRING && host_reply->type != REDIS_REPLY_BULK_STRING)) {
    rc = TURBO_EPROTO;
    goto cleanup;
  }
  rc = redis_sentinel_parse_port(port_reply, port);
  if (rc != TURBO_OK) goto cleanup;
  *host = tstr_dup_len(host_reply->str, host_reply->len);
  if (!*host) rc = TURBO_ENOMEM;

cleanup:
  redis_command_result_clear(&result);
  redis_client_destroy(client);
  if (rc != TURBO_OK) {
    tstr_free(*host);
    *host = NULL;
    *port = 0;
  }
  return rc;
}

static int redis_sentinel_pool_is_master(redis_pool_t *pool) {
  redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
  const char *argv[] = {"ROLE"};
  int rc;
  if (!pool) return TURBO_EINVAL;
  rc = redis_pool_commandv_result(pool, 0, 1, argv, NULL, &result);
  if (rc == TURBO_OK && result.reply && result.reply->type == REDIS_REPLY_ARRAY &&
      result.reply->element_count > 0 &&
      redis_sentinel_reply_text_is(result.reply->elements[0], "master")) {
    redis_command_result_clear(&result);
    return TURBO_OK;
  }
  redis_command_result_clear(&result);
  return rc == TURBO_OK ? TURBO_EPROTO : rc;
}

static int redis_sentinel_endpoint_is_master(redis_sentinel_t *sentinel, const char *host,
                                             uint16_t port) {
  redis_config_t config = {0};
  redis_client_t *client = NULL;
  redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
  const char *argv[] = {"ROLE"};
  int rc;

  config.host = host;
  config.port = port;
  config.username = sentinel->config.username;
  config.password = sentinel->config.password;
  config.database = sentinel->config.database;
  config.timeout_ms = sentinel->config.connect_timeout_ms;
  config.command_timeout_ms = sentinel->config.command_timeout_ms;
  config.max_pipeline = 1;
  client = redis_client_create_with_config(&config);
  if (!client) return TURBO_ENOMEM;
  rc = redis_client_connect(client, NULL, NULL);
  if (rc == TURBO_OK) rc = redis_commandv_result(client, 1, argv, NULL, &result);
  if (rc == TURBO_OK && (!result.reply || result.reply->type != REDIS_REPLY_ARRAY ||
                         result.reply->element_count == 0 ||
                         !redis_sentinel_reply_text_is(result.reply->elements[0], "master")))
    rc = TURBO_EPROTO;
  redis_command_result_clear(&result);
  redis_client_destroy(client);
  return rc;
}

static void redis_sentinel_generation_destroy(redis_sentinel_generation_t *generation) {
  if (!generation) return;
  redis_pool_destroy(generation->pool);
  tstr_free(generation->host);
  free(generation);
}

static redis_sentinel_generation_t *
redis_sentinel_generation_create(redis_sentinel_t *sentinel, const char *host, uint16_t port) {
  redis_pool_config_t pool_config = REDIS_POOL_CONFIG_DEFAULT;
  redis_sentinel_generation_t *generation;

  generation = calloc(1, sizeof(*generation));
  if (!generation) return NULL;
  generation->host = tstr_dup(host);
  generation->port = port;
  if (!generation->host) goto fail;

  pool_config.master_host = host;
  pool_config.master_port = port;
  pool_config.username = sentinel->config.username;
  pool_config.password = sentinel->config.password;
  pool_config.database = sentinel->config.database;
  pool_config.min_connections = sentinel->config.min_connections;
  pool_config.max_connections = sentinel->config.max_connections;
  pool_config.connect_timeout_ms = sentinel->config.connect_timeout_ms;
  pool_config.command_timeout_ms = sentinel->config.command_timeout_ms;
  pool_config.idle_timeout_ms = sentinel->config.idle_timeout_ms;
  generation->pool = redis_pool_create(&pool_config);
  if (!generation->pool || redis_pool_start(generation->pool) != TURBO_OK ||
      redis_sentinel_pool_is_master(generation->pool) != TURBO_OK)
    goto fail;
  return generation;

fail:
  redis_sentinel_generation_destroy(generation);
  return NULL;
}

static void redis_sentinel_retired_remove(redis_sentinel_t *sentinel,
                                          redis_sentinel_generation_t *generation) {
  redis_sentinel_generation_t **link;
  if (!sentinel || !generation) return;
  link = &sentinel->retired;
  while (*link) {
    if (*link == generation) {
      *link = generation->next;
      generation->next = NULL;
      return;
    }
    link = &(*link)->next;
  }
}

static void redis_sentinel_generation_release(redis_sentinel_t *sentinel,
                                              redis_sentinel_generation_t *generation) {
  if (!sentinel || !generation || generation->active_leases == 0) return;
  generation->active_leases--;
  if (generation->retired && generation->active_leases == 0) {
    redis_sentinel_retired_remove(sentinel, generation);
    redis_sentinel_generation_destroy(generation);
  }
}

static void redis_sentinel_generation_retire(redis_sentinel_t *sentinel,
                                             redis_sentinel_generation_t *generation) {
  if (!sentinel || !generation || generation->retired) return;
  generation->retired = 1;
  redis_pool_stop(generation->pool);
  if (generation->active_leases == 0) {
    redis_sentinel_generation_destroy(generation);
    return;
  }
  generation->next = sentinel->retired;
  sentinel->retired = generation;
}

static int redis_sentinel_endpoint_equal(const redis_sentinel_generation_t *generation,
                                         const char *host, uint16_t port) {
  return generation && generation->port == port && host && strcmp(generation->host, host) == 0;
}

static void redis_sentinel_commit_generation(redis_sentinel_t *sentinel,
                                             redis_sentinel_generation_t *candidate) {
  redis_sentinel_generation_t *previous = sentinel->current;
  sentinel->current = candidate;
  sentinel->connected = 1;
  sentinel->last_topology_refresh_ns = turbo_hrtime();
  sentinel->stats.topology_refreshes++;
  if (previous) sentinel->stats.failovers++;
  redis_sentinel_generation_retire(sentinel, previous);
}

static int redis_sentinel_refresh_internal(redis_sentinel_t *sentinel) {
  uint64_t epoch;
  int final_rc = TURBO_ENOTCONN;

  if (!sentinel || sentinel->closing) return TURBO_ECANCELED;
  if (sentinel->refreshing) return TURBO_EBUSY;
  sentinel->refreshing = 1;
  epoch = sentinel->control_epoch;

  for (size_t offset = 0; offset < sentinel->config.sentinel_count; ++offset) {
    size_t index = (sentinel->preferred_sentinel + offset) % sentinel->config.sentinel_count;
    redis_sentinel_generation_t *candidate = NULL;
    tstr_t host = NULL;
    uint16_t port = 0;
    int rc;

    sentinel->stats.discovery_attempts++;
    rc = redis_sentinel_query_master(sentinel, index, &host, &port);
    if (rc != TURBO_OK) {
      final_rc = rc;
      continue;
    }

    if (redis_sentinel_endpoint_equal(sentinel->current, host, port)) {
      rc = redis_sentinel_endpoint_is_master(sentinel, host, port);
      if (rc == TURBO_OK && !sentinel->closing && epoch == sentinel->control_epoch) {
        sentinel->preferred_sentinel = index;
        sentinel->connected = 1;
        sentinel->last_topology_refresh_ns = turbo_hrtime();
        sentinel->stats.topology_refreshes++;
        tstr_free(host);
        final_rc = TURBO_OK;
        break;
      }
      final_rc = rc;
      tstr_free(host);
      continue;
    }

    candidate = redis_sentinel_generation_create(sentinel, host, port);
    tstr_free(host);
    if (!candidate) {
      final_rc = TURBO_ENOTCONN;
      continue;
    }
    if (sentinel->closing || epoch != sentinel->control_epoch) {
      redis_sentinel_generation_destroy(candidate);
      final_rc = TURBO_ECANCELED;
      break;
    }

    sentinel->preferred_sentinel = index;
    redis_sentinel_commit_generation(sentinel, candidate);
    final_rc = TURBO_OK;
    break;
  }

  if (final_rc != TURBO_OK) sentinel->stats.discovery_failures++;
  sentinel->refreshing = 0;
  return final_rc;
}

static void redis_sentinel_free(redis_sentinel_t *sentinel) {
  if (!sentinel) return;
  redis_sentinel_config_clear(&sentinel->config);
  free(sentinel);
}

static int redis_sentinel_operation_begin(redis_sentinel_t *sentinel) {
  if (!sentinel) return TURBO_EINVAL;
  if (sentinel->closing) return TURBO_ECANCELED;
  sentinel->active_calls++;
  return TURBO_OK;
}

static void redis_sentinel_operation_end(redis_sentinel_t *sentinel) {
  if (!sentinel || sentinel->active_calls == 0) return;
  sentinel->active_calls--;
  if (sentinel->destroy_pending && sentinel->active_calls == 0) redis_sentinel_free(sentinel);
}

static int redis_sentinel_attempt_begin(redis_sentinel_t *sentinel,
                                        redis_sentinel_attempt_t *attempt,
                                        redis_command_result_t *out) {
  redis_sentinel_generation_t *generation;
  if (!sentinel || !attempt || !out) return TURBO_EINVAL;
  memset(attempt, 0, sizeof(*attempt));
  generation = sentinel->current;
  if (!sentinel->connected || !generation || generation->retired) {
    out->status = TURBO_ENOTCONN;
    return out->status;
  }
  generation->active_leases++;
  attempt->generation = generation;
  attempt->connection = redis_pool_acquire(generation->pool, 0);
  if (!attempt->connection) {
    redis_sentinel_generation_release(sentinel, generation);
    memset(attempt, 0, sizeof(*attempt));
    out->status = TURBO_ENOTCONN;
    return out->status;
  }
  attempt->client = redis_pool_conn_client(attempt->connection);
  if (!attempt->client) {
    redis_pool_release(generation->pool, attempt->connection);
    redis_sentinel_generation_release(sentinel, generation);
    memset(attempt, 0, sizeof(*attempt));
    out->status = TURBO_ENOTCONN;
    return out->status;
  }
  return TURBO_OK;
}

static void redis_sentinel_attempt_end(redis_sentinel_t *sentinel,
                                       redis_sentinel_attempt_t *attempt) {
  if (!sentinel || !attempt || !attempt->generation) return;
  if (attempt->connection) redis_pool_release(attempt->generation->pool, attempt->connection);
  redis_sentinel_generation_release(sentinel, attempt->generation);
  memset(attempt, 0, sizeof(*attempt));
}

static int redis_sentinel_should_refresh(const redis_command_result_t *result) {
  if (!result) return 0;
  if (result->outcome != REDIS_COMMAND_REPLIED) return 1;
  return result->server_error == REDIS_SERVER_ERROR_READ_ONLY ||
         result->server_error == REDIS_SERVER_ERROR_MASTER_DOWN;
}

static int redis_sentinel_retry_is_safe(const redis_command_result_t *result) {
  if (!result) return 0;
  return result->outcome == REDIS_COMMAND_NOT_SENT ||
         result->server_error == REDIS_SERVER_ERROR_READ_ONLY ||
         result->server_error == REDIS_SERVER_ERROR_MASTER_DOWN;
}

static int redis_sentinel_periodic_refresh_due(const redis_sentinel_t *sentinel) {
  uint64_t interval_ns;
  if (!sentinel || sentinel->config.topology_refresh_ms == 0 ||
      sentinel->last_topology_refresh_ns == 0)
    return 0;
  interval_ns = (uint64_t)sentinel->config.topology_refresh_ms * REDIS_SENTINEL_NS_PER_MS;
  return turbo_hrtime() - sentinel->last_topology_refresh_ns >= interval_ns;
}

static int redis_sentinel_commandv_core(redis_sentinel_t *sentinel, int argc, const char **argv,
                                        const size_t *argvlen, redis_command_cb_t callback,
                                        void *user_data, redis_command_result_t *out) {
  redis_sentinel_attempt_t attempt;
  int rc;

  if (!out) return TURBO_EINVAL;
  *out = (redis_command_result_t)REDIS_COMMAND_RESULT_INIT;
  if (!sentinel || argc <= 0 || !argv) {
    out->status = TURBO_EINVAL;
    return out->status;
  }
  for (int i = 0; i < argc; ++i) {
    if (!argv[i]) {
      out->status = TURBO_EINVAL;
      return out->status;
    }
  }
  rc = redis_sentinel_operation_begin(sentinel);
  if (rc != TURBO_OK) {
    out->status = rc;
    return rc;
  }

  if (redis_sentinel_periodic_refresh_due(sentinel))
    (void)redis_sentinel_refresh_internal(sentinel);

  rc = redis_sentinel_attempt_begin(sentinel, &attempt, out);
  if (rc != TURBO_OK) {
    sentinel->stats.commands_failed++;
    goto finish_operation;
  }
  rc = redis_commandv_result(attempt.client, argc, argv, argvlen, out);

  if (redis_sentinel_should_refresh(out)) {
    redis_sentinel_generation_t *attempt_generation = attempt.generation;
    int refresh_rc = redis_sentinel_refresh_internal(sentinel);
    if (refresh_rc == TURBO_OK && sentinel->current != attempt_generation &&
        redis_sentinel_retry_is_safe(out)) {
      redis_sentinel_attempt_end(sentinel, &attempt);
      redis_command_result_clear(out);
      sentinel->stats.safe_retries++;
      rc = redis_sentinel_attempt_begin(sentinel, &attempt, out);
      if (rc == TURBO_OK) rc = redis_commandv_result(attempt.client, argc, argv, argvlen, out);
    }
  }

  if (out->outcome == REDIS_COMMAND_REPLIED) sentinel->stats.commands_sent++;
  if (rc != TURBO_OK) sentinel->stats.commands_failed++;
  if (out->outcome == REDIS_COMMAND_REPLIED && callback)
    callback(attempt.client, out->reply, user_data);
  redis_sentinel_attempt_end(sentinel, &attempt);

finish_operation:
  redis_sentinel_operation_end(sentinel);
  return rc;
}

redis_sentinel_t *redis_sentinel_create(const redis_sentinel_config_t *config) {
  redis_sentinel_t *sentinel;
  if (!redis_sentinel_config_valid(config)) return NULL;
  sentinel = calloc(1, sizeof(*sentinel));
  if (!sentinel) return NULL;
  if (redis_sentinel_config_copy(&sentinel->config, config) != TURBO_OK) {
    free(sentinel);
    return NULL;
  }
  return sentinel;
}

int redis_sentinel_connect(redis_sentinel_t *sentinel) {
  int rc = redis_sentinel_operation_begin(sentinel);
  if (rc != TURBO_OK) return rc;
  rc = redis_sentinel_refresh_internal(sentinel);
  redis_sentinel_operation_end(sentinel);
  return rc;
}

int redis_sentinel_refresh(redis_sentinel_t *sentinel) {
  int rc = redis_sentinel_operation_begin(sentinel);
  if (rc != TURBO_OK) return rc;
  rc = redis_sentinel_refresh_internal(sentinel);
  redis_sentinel_operation_end(sentinel);
  return rc;
}

void redis_sentinel_disconnect(redis_sentinel_t *sentinel) {
  redis_sentinel_generation_t *current;
  if (!sentinel || sentinel->closing) return;
  sentinel->control_epoch++;
  sentinel->connected = 0;
  current = sentinel->current;
  sentinel->current = NULL;
  redis_sentinel_generation_retire(sentinel, current);
}

void redis_sentinel_destroy(redis_sentinel_t *sentinel) {
  redis_sentinel_generation_t *current;
  if (!sentinel || sentinel->destroy_pending) return;
  sentinel->closing = 1;
  sentinel->destroy_pending = 1;
  sentinel->control_epoch++;
  sentinel->connected = 0;
  current = sentinel->current;
  sentinel->current = NULL;
  redis_sentinel_generation_retire(sentinel, current);
  if (sentinel->active_calls == 0) redis_sentinel_free(sentinel);
}

int redis_sentinel_commandv_result(redis_sentinel_t *sentinel, int argc, const char **argv,
                                   const size_t *argvlen, redis_command_result_t *out) {
  return redis_sentinel_commandv_core(sentinel, argc, argv, argvlen, NULL, NULL, out);
}

int redis_sentinel_commandv(redis_sentinel_t *sentinel, int argc, const char **argv,
                            const size_t *argvlen, redis_command_cb_t callback, void *user_data) {
  redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
  int rc =
      redis_sentinel_commandv_core(sentinel, argc, argv, argvlen, callback, user_data, &result);
  (void)rc;
  rc = result.outcome == REDIS_COMMAND_REPLIED ? 0 : -1;
  redis_command_result_clear(&result);
  return rc;
}

int redis_sentinel_set(redis_sentinel_t *sentinel, const char *key, const char *value,
                       redis_command_cb_t callback, void *user_data) {
  const char *argv[] = {"SET", key, value};
  return redis_sentinel_commandv(sentinel, 3, argv, NULL, callback, user_data);
}

int redis_sentinel_get(redis_sentinel_t *sentinel, const char *key, redis_command_cb_t callback,
                       void *user_data) {
  const char *argv[] = {"GET", key};
  return redis_sentinel_commandv(sentinel, 2, argv, NULL, callback, user_data);
}

int redis_sentinel_del(redis_sentinel_t *sentinel, const char *key, redis_command_cb_t callback,
                       void *user_data) {
  const char *argv[] = {"DEL", key};
  return redis_sentinel_commandv(sentinel, 2, argv, NULL, callback, user_data);
}

int redis_sentinel_get_master(const redis_sentinel_t *sentinel, char *host, size_t host_size,
                              uint16_t *port) {
  size_t length;
  if (!sentinel || !host || host_size == 0 || !port) return TURBO_EINVAL;
  if (!sentinel->connected || !sentinel->current) return TURBO_ENOTCONN;
  length = tstr_len(sentinel->current->host);
  if (length >= host_size) return TURBO_ERANGE;
  memcpy(host, sentinel->current->host, length + 1u);
  *port = sentinel->current->port;
  return TURBO_OK;
}

int redis_sentinel_is_healthy(const redis_sentinel_t *sentinel) {
  return sentinel && sentinel->connected && sentinel->current &&
         redis_pool_is_healthy(sentinel->current->pool);
}

void redis_sentinel_get_stats(const redis_sentinel_t *sentinel, redis_sentinel_stats_t *stats) {
  if (!stats) return;
  if (!sentinel) {
    memset(stats, 0, sizeof(*stats));
    return;
  }
  *stats = sentinel->stats;
}

void redis_sentinel_reset_stats(redis_sentinel_t *sentinel) {
  if (!sentinel) return;
  memset(&sentinel->stats, 0, sizeof(sentinel->stats));
}
