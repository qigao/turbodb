#include "redis_sentinel.h"

#include "turbo_error.h"
#include "turbo_str.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef enum redis_sentinel_phase {
  REDIS_SENTINEL_PHASE_DISCOVERY_CONNECT = 0,
  REDIS_SENTINEL_PHASE_DISCOVERY_COMMAND,
  REDIS_SENTINEL_PHASE_DISCOVERY_CLEANUP,
  REDIS_SENTINEL_PHASE_MASTER_CONNECT,
  REDIS_SENTINEL_PHASE_READY,
  REDIS_SENTINEL_PHASE_FAILED,
  REDIS_SENTINEL_PHASE_CLOSING,
  REDIS_SENTINEL_PHASE_CLOSED
} redis_sentinel_phase;

typedef struct redis_sentinel_impl {
  redis_io_runtime *runtime;
  tstr *sentinel_hosts;
  uint16_t *sentinel_ports;
  size_t sentinel_count;
  tstr service_name;
  tstr sentinel_username;
  tstr sentinel_password;
  tstr username;
  tstr password;
  int database;
  size_t connection_capacity;
  size_t address_capacity;
  size_t max_command_bytes;
  size_t initial_buffer_bytes;
  size_t max_buffer_bytes;
  size_t receive_chunk_bytes;
  size_t discovery_reply_bytes;
  uint64_t cancel_timeout_ns;
  tstr master_host;
  uint16_t master_port;
  redis_pool discovery_pool;
  redis_pool_stream discovery_stream;
  redis_pool master_pool;
  int discovery_item_seen;
  redis_sentinel_phase phase;
} redis_sentinel_impl;

static redis_sentinel_impl *redis_sentinel_get(redis_sentinel *sentinel) {
  return sentinel ? (redis_sentinel_impl *)sentinel->impl : NULL;
}

static const redis_sentinel_impl *redis_sentinel_get_const(const redis_sentinel *sentinel) {
  return sentinel ? (const redis_sentinel_impl *)sentinel->impl : NULL;
}

static tstr redis_sentinel_copy(const char *value) {
  return value ? tstr_new_len(value, strlen(value)) : NULL;
}

static redis_sentinel_connect_step redis_sentinel_result(redis_sentinel_connect_step_kind kind,
                                                         int status, cflow_waitable waitable) {
  redis_sentinel_connect_step step;
  memset(&step, 0, sizeof(step));
  step.kind = kind;
  step.status = status;
  step.waitable = waitable;
  return step;
}

static redis_pool_config redis_sentinel_pool_config(const redis_sentinel_impl *impl,
                                                    const char *host, uint16_t port,
                                                    const char *username, const char *password,
                                                    int database, size_t capacity) {
  redis_pool_config config = REDIS_POOL_CONFIG_INIT;
  config.runtime = impl->runtime;
  config.host = host;
  config.port = port;
  config.username = username;
  config.password = password;
  config.database = database;
  config.connection_capacity = capacity;
  config.address_capacity = impl->address_capacity;
  config.max_command_bytes = impl->max_command_bytes;
  config.initial_buffer_bytes = impl->initial_buffer_bytes;
  config.max_buffer_bytes = impl->max_buffer_bytes;
  config.receive_chunk_bytes = impl->receive_chunk_bytes;
  config.prepare_reply_bytes = impl->discovery_reply_bytes;
  config.cancel_timeout_ns = impl->cancel_timeout_ns;
  return config;
}

static int redis_sentinel_parse_master(redis_sentinel_impl *impl, const redis_reply_t *reply) {
  const redis_reply_t *host_reply;
  const redis_reply_t *port_reply;
  char *end = NULL;
  unsigned long port;
  tstr host;
  if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->element_count != 2u) return TURBO_EPROTO;
  host_reply = reply->elements[0];
  port_reply = reply->elements[1];
  if (!host_reply || !port_reply ||
      (host_reply->type != REDIS_REPLY_BULK_STRING && host_reply->type != REDIS_REPLY_STRING) ||
      (port_reply->type != REDIS_REPLY_BULK_STRING && port_reply->type != REDIS_REPLY_STRING) ||
      !host_reply->str || host_reply->len == 0u || !port_reply->str || port_reply->len == 0u)
    return TURBO_EPROTO;
  errno = 0;
  port = strtoul(port_reply->str, &end, 10);
  if (errno != 0 || end != port_reply->str + port_reply->len || port == 0u || port > UINT16_MAX)
    return TURBO_EPROTO;
  host = tstr_new_len(host_reply->str, host_reply->len);
  if (!host) return TURBO_ENOMEM;
  tstr_free(impl->master_host);
  impl->master_host = host;
  impl->master_port = (uint16_t)port;
  return TURBO_OK;
}

static void redis_sentinel_free_impl(redis_sentinel_impl *impl) {
  size_t index;
  if (!impl) return;
  for (index = 0u; index < impl->sentinel_count; ++index)
    tstr_free(impl->sentinel_hosts ? impl->sentinel_hosts[index] : NULL);
  tstr_free(impl->master_host);
  tstr_free(impl->password);
  tstr_free(impl->username);
  tstr_free(impl->sentinel_password);
  tstr_free(impl->sentinel_username);
  tstr_free(impl->service_name);
  free(impl->sentinel_ports);
  free(impl->sentinel_hosts);
  free(impl);
}

int redis_sentinel_init(redis_sentinel *sentinel, const redis_sentinel_config *config) {
  redis_sentinel_impl *impl;
  size_t index;
  if (!sentinel || sentinel->impl || !config || !redis_io_runtime_valid(config->runtime) ||
      !config->sentinel_hosts || !config->sentinel_ports || config->sentinel_count == 0u ||
      !config->service_name || config->service_name[0] == '\0' || config->database < 0 ||
      config->connection_capacity == 0u || config->address_capacity == 0u ||
      config->max_command_bytes == 0u || config->initial_buffer_bytes == 0u ||
      config->max_buffer_bytes < config->initial_buffer_bytes ||
      config->receive_chunk_bytes == 0u || config->discovery_reply_bytes == 0u ||
      config->cancel_timeout_ns == 0u || config->sentinel_count > SIZE_MAX / sizeof(tstr) ||
      config->sentinel_count > SIZE_MAX / sizeof(uint16_t))
    return TURBO_EINVAL;
  impl = (redis_sentinel_impl *)calloc(1, sizeof(*impl));
  if (!impl) return TURBO_ENOMEM;
  impl->sentinel_hosts = (tstr *)calloc(config->sentinel_count, sizeof(tstr));
  impl->sentinel_ports = (uint16_t *)calloc(config->sentinel_count, sizeof(uint16_t));
  impl->service_name = redis_sentinel_copy(config->service_name);
  impl->sentinel_username = redis_sentinel_copy(config->sentinel_username);
  impl->sentinel_password = redis_sentinel_copy(config->sentinel_password);
  impl->username = redis_sentinel_copy(config->username);
  impl->password = redis_sentinel_copy(config->password);
  impl->sentinel_count = config->sentinel_count;
  if (!impl->sentinel_hosts || !impl->sentinel_ports || !impl->service_name ||
      (config->sentinel_username && !impl->sentinel_username) ||
      (config->sentinel_password && !impl->sentinel_password) ||
      (config->username && !impl->username) || (config->password && !impl->password))
    goto no_memory;
  for (index = 0u; index < config->sentinel_count; ++index) {
    if (!config->sentinel_hosts[index] || config->sentinel_hosts[index][0] == '\0' ||
        config->sentinel_ports[index] == 0u)
      goto invalid;
    impl->sentinel_hosts[index] = redis_sentinel_copy(config->sentinel_hosts[index]);
    if (!impl->sentinel_hosts[index]) goto no_memory;
    impl->sentinel_ports[index] = config->sentinel_ports[index];
  }
  impl->runtime = config->runtime;
  impl->database = config->database;
  impl->connection_capacity = config->connection_capacity;
  impl->address_capacity = config->address_capacity;
  impl->max_command_bytes = config->max_command_bytes;
  impl->initial_buffer_bytes = config->initial_buffer_bytes;
  impl->max_buffer_bytes = config->max_buffer_bytes;
  impl->receive_chunk_bytes = config->receive_chunk_bytes;
  impl->discovery_reply_bytes = config->discovery_reply_bytes;
  impl->cancel_timeout_ns = config->cancel_timeout_ns;
  impl->phase = REDIS_SENTINEL_PHASE_DISCOVERY_CONNECT;
  {
    redis_pool_config pool_config =
        redis_sentinel_pool_config(impl, impl->sentinel_hosts[0], impl->sentinel_ports[0],
                                   impl->sentinel_username, impl->sentinel_password, 0, 1u);
    if (redis_pool_init(&impl->discovery_pool, &pool_config) != TURBO_OK) goto invalid;
  }
  sentinel->impl = impl;
  return TURBO_OK;

invalid:
  redis_sentinel_free_impl(impl);
  return TURBO_EINVAL;
no_memory:
  redis_sentinel_free_impl(impl);
  return TURBO_ENOMEM;
}

redis_sentinel_connect_step redis_sentinel_connect_next(redis_sentinel *sentinel) {
  redis_sentinel_impl *impl = redis_sentinel_get(sentinel);
  cflow_waitable empty_waitable;
  int cleanup_status = TURBO_OK;
  memset(&empty_waitable, 0, sizeof(empty_waitable));
  if (!impl)
    return redis_sentinel_result(REDIS_SENTINEL_CONNECT_ERROR, TURBO_EINVAL, empty_waitable);
  if (impl->phase == REDIS_SENTINEL_PHASE_READY)
    return redis_sentinel_result(REDIS_SENTINEL_CONNECT_DONE, TURBO_OK, empty_waitable);
  if (impl->phase == REDIS_SENTINEL_PHASE_DISCOVERY_CONNECT) {
    redis_pool_connect_step connected = redis_pool_connect_next(&impl->discovery_pool);
    if (connected.kind == REDIS_POOL_CONNECT_WAIT)
      return redis_sentinel_result(REDIS_SENTINEL_CONNECT_WAIT, TURBO_OK, connected.waitable);
    if (connected.kind == REDIS_POOL_CONNECT_ERROR) {
      if (connected.status != TURBO_EBUSY) impl->phase = REDIS_SENTINEL_PHASE_FAILED;
      return redis_sentinel_result(REDIS_SENTINEL_CONNECT_ERROR, connected.status, empty_waitable);
    }
    {
      const char *arguments[] = {"SENTINEL", "get-master-addr-by-name", impl->service_name};
      int status = redis_pool_command_open(&impl->discovery_pool, 3, arguments, NULL,
                                           impl->discovery_reply_bytes, &impl->discovery_stream);
      if (status != TURBO_OK) {
        impl->phase = REDIS_SENTINEL_PHASE_FAILED;
        return redis_sentinel_result(REDIS_SENTINEL_CONNECT_ERROR, status, empty_waitable);
      }
    }
    impl->phase = REDIS_SENTINEL_PHASE_DISCOVERY_COMMAND;
  }
  if (impl->phase == REDIS_SENTINEL_PHASE_DISCOVERY_COMMAND) {
    for (;;) {
      redis_cflow_stream_step discovered = redis_pool_stream_next(&impl->discovery_stream);
      if (discovered.kind == REDIS_CFLOW_STREAM_WAIT)
        return redis_sentinel_result(REDIS_SENTINEL_CONNECT_WAIT, TURBO_OK, discovered.waitable);
      if (discovered.kind == REDIS_CFLOW_STREAM_ERROR) {
        redis_reply_free(discovered.item);
        impl->phase = REDIS_SENTINEL_PHASE_FAILED;
        return redis_sentinel_result(REDIS_SENTINEL_CONNECT_ERROR, discovered.status,
                                     empty_waitable);
      }
      if (discovered.kind == REDIS_CFLOW_STREAM_ITEM) {
        int status = redis_sentinel_parse_master(impl, discovered.item);
        redis_reply_free(discovered.item);
        if (status != TURBO_OK) {
          impl->phase = REDIS_SENTINEL_PHASE_FAILED;
          return redis_sentinel_result(REDIS_SENTINEL_CONNECT_ERROR, status, empty_waitable);
        }
        impl->discovery_item_seen = 1;
        continue;
      }
      if (!impl->discovery_item_seen) {
        impl->phase = REDIS_SENTINEL_PHASE_FAILED;
        return redis_sentinel_result(REDIS_SENTINEL_CONNECT_ERROR, TURBO_EPROTO, empty_waitable);
      }
      impl->phase = REDIS_SENTINEL_PHASE_DISCOVERY_CLEANUP;
      break;
    }
  }
  if (impl->phase == REDIS_SENTINEL_PHASE_DISCOVERY_CLEANUP) {
    if (impl->discovery_stream.impl) {
      cleanup_status = redis_pool_stream_destroy(&impl->discovery_stream);
      if (cleanup_status != TURBO_OK) goto cleanup_error;
    }
    if (impl->discovery_pool.impl) {
      cleanup_status = redis_pool_close(&impl->discovery_pool);
      if (cleanup_status != TURBO_OK) goto cleanup_error;
      cleanup_status = redis_pool_destroy(&impl->discovery_pool);
      if (cleanup_status != TURBO_OK) goto cleanup_error;
    }
    {
      redis_pool_config pool_config =
          redis_sentinel_pool_config(impl, impl->master_host, impl->master_port, impl->username,
                                     impl->password, impl->database, impl->connection_capacity);
      cleanup_status = redis_pool_init(&impl->master_pool, &pool_config);
      if (cleanup_status != TURBO_OK) goto cleanup_error;
    }
    impl->phase = REDIS_SENTINEL_PHASE_MASTER_CONNECT;
  }
  if (impl->phase == REDIS_SENTINEL_PHASE_MASTER_CONNECT) {
    redis_pool_connect_step connected = redis_pool_connect_next(&impl->master_pool);
    if (connected.kind == REDIS_POOL_CONNECT_WAIT)
      return redis_sentinel_result(REDIS_SENTINEL_CONNECT_WAIT, TURBO_OK, connected.waitable);
    if (connected.kind == REDIS_POOL_CONNECT_ERROR) {
      if (connected.status != TURBO_EBUSY) impl->phase = REDIS_SENTINEL_PHASE_FAILED;
      return redis_sentinel_result(REDIS_SENTINEL_CONNECT_ERROR, connected.status, empty_waitable);
    }
    impl->phase = REDIS_SENTINEL_PHASE_READY;
    return redis_sentinel_result(REDIS_SENTINEL_CONNECT_DONE, TURBO_OK, empty_waitable);
  }
  return redis_sentinel_result(REDIS_SENTINEL_CONNECT_ERROR, TURBO_ESHUTDOWN, empty_waitable);

cleanup_error:
  if (cleanup_status != TURBO_EBUSY) impl->phase = REDIS_SENTINEL_PHASE_FAILED;
  return redis_sentinel_result(REDIS_SENTINEL_CONNECT_ERROR, cleanup_status, empty_waitable);
}

int redis_sentinel_command_open(redis_sentinel *sentinel, int argc, const char **argv,
                                const size_t *argvlen, size_t max_reply_bytes,
                                redis_pool_stream *out_stream) {
  redis_sentinel_impl *impl = redis_sentinel_get(sentinel);
  if (!impl || impl->phase != REDIS_SENTINEL_PHASE_READY)
    return impl ? TURBO_ESHUTDOWN : TURBO_EINVAL;
  return redis_pool_command_open(&impl->master_pool, argc, argv, argvlen, max_reply_bytes,
                                 out_stream);
}

int redis_sentinel_get_master(const redis_sentinel *sentinel, redis_sentinel_master *master) {
  const redis_sentinel_impl *impl = redis_sentinel_get_const(sentinel);
  if (!impl || !master || impl->phase != REDIS_SENTINEL_PHASE_READY) return TURBO_EINVAL;
  master->host = impl->master_host;
  master->port = impl->master_port;
  return TURBO_OK;
}

int redis_sentinel_ready(const redis_sentinel *sentinel) {
  const redis_sentinel_impl *impl = redis_sentinel_get_const(sentinel);
  return impl && impl->phase == REDIS_SENTINEL_PHASE_READY;
}

int redis_sentinel_close(redis_sentinel *sentinel) {
  redis_sentinel_impl *impl = redis_sentinel_get(sentinel);
  int status;
  if (!impl) return TURBO_EINVAL;
  if (impl->phase == REDIS_SENTINEL_PHASE_CLOSED) return TURBO_OK;
  impl->phase = REDIS_SENTINEL_PHASE_CLOSING;
  if (impl->discovery_stream.impl) {
    status = redis_pool_stream_destroy(&impl->discovery_stream);
    if (status != TURBO_OK) return status;
  }
  if (impl->discovery_pool.impl) {
    status = redis_pool_close(&impl->discovery_pool);
    if (status != TURBO_OK) return status;
    status = redis_pool_destroy(&impl->discovery_pool);
    if (status != TURBO_OK) return status;
  }
  if (impl->master_pool.impl) {
    status = redis_pool_close(&impl->master_pool);
    if (status != TURBO_OK) return status;
  }
  impl->phase = REDIS_SENTINEL_PHASE_CLOSED;
  return TURBO_OK;
}

int redis_sentinel_destroy(redis_sentinel *sentinel) {
  redis_sentinel_impl *impl = redis_sentinel_get(sentinel);
  int status;
  if (!impl) return TURBO_EINVAL;
  status = redis_sentinel_close(sentinel);
  if (status != TURBO_OK) return status;
  if (impl->master_pool.impl) {
    status = redis_pool_destroy(&impl->master_pool);
    if (status != TURBO_OK) return status;
  }
  redis_sentinel_free_impl(impl);
  sentinel->impl = NULL;
  return TURBO_OK;
}
