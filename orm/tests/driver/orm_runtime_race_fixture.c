#include <orm_driver_abi.h>
#include <salts/clock.h>
#include <salts/thread.h>

#include <stdint.h>
#include <string.h>

#define RACE_HEADER(T) {(uint32_t)sizeof(T), ORM_DRIVER_ABI_VERSION}
#define RACE_TABLE(p) {(p), (uint32_t)sizeof(*(p)), 0u}
#define RACE_TIMEOUT_MS UINT64_C(5000)

enum {
  ORM_RUNTIME_RACE_GATE_NONE = 0u,
  ORM_RUNTIME_RACE_GATE_INITIALIZE = 1u,
  ORM_RUNTIME_RACE_GATE_CONNECT = 2u,
  ORM_RUNTIME_RACE_GATE_FINALIZE = 3u
};

typedef struct race_gate {
  salts_mutex_t mutex;
  salts_cond_t condition;
  uint32_t initialized;
  uint32_t phase;
  uint32_t entered;
  uint32_t released;
} race_gate;

typedef struct race_module {
  uint32_t live_connections;
  uint32_t live;
} race_module;

typedef struct race_connection {
  race_module *module;
  uint32_t live;
} race_connection;

static race_gate gate;
static race_module module_context;
static race_connection connection_contexts[4];
static const uint8_t bundle[ORM_DRIVER_BUNDLE_ID_BYTES] =
    ORM_DRIVER_BUNDLE_ID_INIT;
#ifndef ORM_RUNTIME_RACE_DRIVER_ID
#define ORM_RUNTIME_RACE_DRIVER_ID "race"
#endif
static const char driver_id[] = ORM_RUNTIME_RACE_DRIVER_ID;

static void race_error(orm_error_t *error, orm_status_t status) {
  if (error == NULL) return;
  memset(error, 0, sizeof(*error));
  error->struct_size = (uint32_t)sizeof(*error);
  error->status = status;
}

static int race_gate_ensure(void) {
  if (gate.initialized != 0u) return 1;
  salts_mutex_init(&gate.mutex);
  if (gate.mutex == NULL) return 0;
  salts_cond_init(&gate.condition);
  if (gate.condition == NULL) {
    salts_mutex_destroy(&gate.mutex);
    gate.mutex = NULL;
    return 0;
  }
  gate.initialized = 1u;
  return 1;
}

static void race_gate_pause(uint32_t phase) {
  if (!race_gate_ensure()) return;
  salts_mutex_lock(&gate.mutex);
  if (gate.phase == phase) {
    gate.entered = 1u;
    salts_cond_broadcast(&gate.condition);
    const uint64_t started = salts_monotonic_ms();
    while (gate.released == 0u) {
      const uint64_t elapsed = salts_monotonic_ms() - started;
      if (elapsed >= RACE_TIMEOUT_MS) break;
      const uint64_t remaining = RACE_TIMEOUT_MS - elapsed;
      if (salts_cond_timedwait(&gate.condition, &gate.mutex,
                               salts_ms_to_ns(remaining)) != 0 &&
          salts_monotonic_ms() - started >= RACE_TIMEOUT_MS)
        break;
    }
    gate.phase = ORM_RUNTIME_RACE_GATE_NONE;
  }
  salts_mutex_unlock(&gate.mutex);
}

ORM_DRIVER_EXPORT int32_t ORM_DRIVER_CALL
orm_runtime_race_gate_arm(uint32_t phase) {
  if (!race_gate_ensure() ||
      phase < ORM_RUNTIME_RACE_GATE_INITIALIZE ||
      phase > ORM_RUNTIME_RACE_GATE_FINALIZE)
    return ORM_STATUS_INVALID_ARGUMENT;
  salts_mutex_lock(&gate.mutex);
  gate.phase = phase;
  gate.entered = 0u;
  gate.released = 0u;
  salts_mutex_unlock(&gate.mutex);
  return ORM_STATUS_OK;
}

ORM_DRIVER_EXPORT int32_t ORM_DRIVER_CALL
orm_runtime_race_gate_wait_entered(uint64_t timeout_ms) {
  if (!race_gate_ensure() || timeout_ms == 0u) return 0;
  salts_mutex_lock(&gate.mutex);
  const uint64_t started = salts_monotonic_ms();
  while (gate.entered == 0u) {
    const uint64_t elapsed = salts_monotonic_ms() - started;
    if (elapsed >= timeout_ms) break;
    const uint64_t remaining = timeout_ms - elapsed;
    if (salts_cond_timedwait(&gate.condition, &gate.mutex,
                             salts_ms_to_ns(remaining)) != 0 &&
        salts_monotonic_ms() - started >= timeout_ms)
      break;
  }
  const int entered = gate.entered != 0u;
  salts_mutex_unlock(&gate.mutex);
  return entered;
}

ORM_DRIVER_EXPORT void ORM_DRIVER_CALL orm_runtime_race_gate_release(void) {
  if (!race_gate_ensure()) return;
  salts_mutex_lock(&gate.mutex);
  gate.released = 1u;
  salts_cond_broadcast(&gate.condition);
  salts_mutex_unlock(&gate.mutex);
}

static orm_status_t ORM_DRIVER_CALL race_initialize(
    const orm_driver_host_v1 *host, void **out, orm_error_t *error) {
  if (out != NULL) *out = NULL;
  if (host == NULL || out == NULL ||
      host->header.abi_version != ORM_DRIVER_ABI_VERSION ||
      host->header.struct_size < sizeof(*host) ||
      memcmp(host->bundle_id, bundle, sizeof(bundle)) != 0) {
    race_error(error, ORM_STATUS_ABI_MISMATCH);
    return ORM_STATUS_ABI_MISMATCH;
  }
  race_gate_pause(ORM_RUNTIME_RACE_GATE_INITIALIZE);
  if (module_context.live != 0u) {
    race_error(error, ORM_STATUS_BUSY);
    return ORM_STATUS_BUSY;
  }
  memset(&module_context, 0, sizeof(module_context));
  module_context.live = 1u;
  *out = &module_context;
  race_error(error, ORM_STATUS_OK);
  return ORM_STATUS_OK;
}

static orm_status_t ORM_DRIVER_CALL race_finalize(
    void *context, orm_error_t *error) {
  if (context != &module_context || module_context.live == 0u) {
    race_error(error, ORM_STATUS_INVALID_ARGUMENT);
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  race_gate_pause(ORM_RUNTIME_RACE_GATE_FINALIZE);
  if (module_context.live_connections != 0u) {
    race_error(error, ORM_STATUS_BUSY);
    return ORM_STATUS_BUSY;
  }
  module_context.live = 0u;
  race_error(error, ORM_STATUS_OK);
  return ORM_STATUS_OK;
}

static void ORM_DRIVER_CALL race_destroy_connection(void *context) {
  race_connection *connection = context;
  if (connection == NULL || connection->live == 0u ||
      connection->module == NULL)
    return;
  --connection->module->live_connections;
  memset(connection, 0, sizeof(*connection));
}

static const orm_driver_connection_ops_v1 connection_ops = {
    RACE_HEADER(orm_driver_connection_ops_v1),
    race_destroy_connection, NULL, NULL, NULL};

static orm_status_t ORM_DRIVER_CALL race_create_connection(
    void *context, const orm_config_t *config,
    const orm_driver_limits_v1 *limits, orm_driver_connection_v1 *out,
    orm_error_t *error) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  if (context != &module_context || module_context.live == 0u ||
      config == NULL || limits == NULL || out == NULL) {
    race_error(error, ORM_STATUS_INVALID_ARGUMENT);
    return ORM_STATUS_INVALID_ARGUMENT;
  }
  race_gate_pause(ORM_RUNTIME_RACE_GATE_CONNECT);
  if (config->option_count != 0u && config->options != NULL &&
      config->options[0].keyword.data != NULL &&
      config->options[0].keyword.len == sizeof("fail_create") - 1u &&
      memcmp(config->options[0].keyword.data, "fail_create",
             sizeof("fail_create") - 1u) == 0) {
    race_error(error, ORM_STATUS_OUT_OF_MEMORY);
    return ORM_STATUS_OUT_OF_MEMORY;
  }

  for (size_t i = 0u;
       i < sizeof(connection_contexts) / sizeof(connection_contexts[0]);
       ++i) {
    if (connection_contexts[i].live == 0u) {
      memset(&connection_contexts[i], 0, sizeof(connection_contexts[i]));
      connection_contexts[i].module = &module_context;
      connection_contexts[i].live = 1u;
      ++module_context.live_connections;
      out->header =
          (orm_driver_header_v1)RACE_HEADER(orm_driver_connection_v1);
      out->context = &connection_contexts[i];
      out->ops = (orm_driver_table_v1)RACE_TABLE(&connection_ops);
      race_error(error, ORM_STATUS_OK);
      return ORM_STATUS_OK;
    }
  }
  race_error(error, ORM_STATUS_LIMIT_EXCEEDED);
  return ORM_STATUS_LIMIT_EXCEEDED;
}

static const orm_driver_module_ops_v1 module_ops = {
    RACE_HEADER(orm_driver_module_ops_v1), race_initialize, race_finalize};

static const orm_driver_api_v1 api = {
    RACE_HEADER(orm_driver_api_v1),
    ORM_DRIVER_BUNDLE_ID_INIT,
    {driver_id, sizeof(driver_id) - 1u},
    NULL,
    0u,
    0u,
    0u,
    ORM_DRIVER_EXEC_CALLER_BLOCKING,
    RACE_TABLE(&module_ops),
    race_create_connection,
    RACE_TABLE(&connection_ops)};

ORM_DRIVER_EXPORT int32_t ORM_DRIVER_CALL orm_driver_get_api_v1(
    const orm_driver_host_v1 *host, uint32_t host_bytes,
    const orm_driver_api_v1 **out_api, uint32_t *out_api_bytes) {
  if (out_api != NULL) *out_api = NULL;
  if (out_api_bytes != NULL) *out_api_bytes = 0u;
  if (host == NULL || host_bytes < sizeof(*host) ||
      out_api == NULL || out_api_bytes == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;
  *out_api = &api;
  *out_api_bytes = (uint32_t)sizeof(api);
  return ORM_STATUS_OK;
}
