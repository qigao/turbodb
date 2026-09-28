#include <orm_driver_plugin.h>
#include <salts/clock.h>
#include <salts/thread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RACE_HEADER(T) {(uint32_t)sizeof(T), ORM_DRIVER_ABI_VERSION}
#define RACE_TABLE(p) {(p), (uint32_t)sizeof(*(p)), 0u}
#define RACE_TIMEOUT_MS UINT64_C(5000)

enum {
  ORM_RUNTIME_RACE_GATE_NONE = 0u,
  ORM_RUNTIME_RACE_GATE_START = 1u,
  ORM_RUNTIME_RACE_GATE_CONNECT = 2u,
  ORM_RUNTIME_RACE_GATE_STOP = 3u
};

#ifndef ORM_RUNTIME_RACE_DRIVER_ID
#define ORM_RUNTIME_RACE_DRIVER_ID "race"
#endif
static const char driver_id[] = ORM_RUNTIME_RACE_DRIVER_ID;

typedef struct race_module {
  uint32_t live_connections;
  uint32_t started;
} race_module;

typedef struct race_connection {
  race_module *module;
  uint32_t live;
} race_connection;

static race_module module_context;
static race_connection connection_contexts[4];

static void race_error(orm_error_t *error, orm_status_t status) {
  if (error == NULL) return;
  memset(error, 0, sizeof(*error));
  error->struct_size = (uint32_t)sizeof(*error);
  error->status = status;
}

static int race_marker_path(
    char *out, size_t out_size, const char *kind, uint32_t phase) {
  const char *prefix = getenv("ORM_RUNTIME_RACE_GATE_PREFIX");
  if (out == NULL || out_size == 0u || prefix == NULL || prefix[0] == '\0')
    return 0;
  const int written = snprintf(
      out, out_size, "%s-%s-%u-%s", prefix, driver_id,
      (unsigned)phase, kind);
  return written > 0 && (size_t)written < out_size;
}

static int race_marker_exists(const char *path) {
  FILE *file = fopen(path, "rb");
  if (file == NULL) return 0;
  fclose(file);
  return 1;
}

static void race_marker_write(const char *path) {
  FILE *file = fopen(path, "wb");
  if (file == NULL) return;
  fputc('1', file);
  fclose(file);
}

static void race_gate_pause(uint32_t phase) {
  char arm[1024];
  char entered[1024];
  char release[1024];
  if (!race_marker_path(arm, sizeof(arm), "arm", phase) ||
      !race_marker_path(entered, sizeof(entered), "entered", phase) ||
      !race_marker_path(release, sizeof(release), "release", phase) ||
      !race_marker_exists(arm))
    return;

  race_marker_write(entered);
  const uint64_t started = salts_monotonic_ms();
  while (!race_marker_exists(release) &&
         salts_monotonic_ms() - started < RACE_TIMEOUT_MS)
    salts_sleep_ms(1u);

  (void)remove(arm);
  (void)remove(entered);
  (void)remove(release);
}

static salts_plugin_status SALTS_PLUGIN_CALL race_start(void *self) {
  race_module *module = (race_module *)self;
  if (module != &module_context)
    return SALTS_PLUGIN_INVALID_ARGUMENT;
  race_gate_pause(ORM_RUNTIME_RACE_GATE_START);
  module->started = 1u;
  return SALTS_PLUGIN_OK;
}

static salts_plugin_status SALTS_PLUGIN_CALL race_request_stop(void *self) {
  race_module *module = (race_module *)self;
  if (module != &module_context)
    return SALTS_PLUGIN_INVALID_ARGUMENT;
  race_gate_pause(ORM_RUNTIME_RACE_GATE_STOP);
  if (module->live_connections != 0u)
    return SALTS_PLUGIN_BUSY;
  module->started = 0u;
  return SALTS_PLUGIN_OK;
}

static bool SALTS_PLUGIN_CALL race_is_quiescent(const void *self) {
  const race_module *module = (const race_module *)self;
  return module == &module_context &&
         module->started == 0u &&
         module->live_connections == 0u;
}

static void SALTS_PLUGIN_CALL race_destroy(void *self) {
  if (self == &module_context)
    memset(&module_context, 0, sizeof(module_context));
}

static void ORM_DRIVER_CALL race_destroy_connection(void *context) {
  race_connection *connection = (race_connection *)context;
  if (connection == NULL || connection->live == 0u ||
      connection->module == NULL)
    return;
  if (connection->module->live_connections == 0u)
    abort();
  --connection->module->live_connections;
  memset(connection, 0, sizeof(*connection));
}

static const orm_driver_connection_ops_v1 connection_ops = {
    RACE_HEADER(orm_driver_connection_ops_v1),
    race_destroy_connection, NULL, NULL, NULL};

static orm_status_t ORM_DRIVER_CALL race_create_connection(
    void *self, const orm_config_t *config,
    const orm_driver_limits_v1 *limits, orm_driver_connection_v1 *out,
    orm_error_t *error) {
  if (out != NULL) memset(out, 0, sizeof(*out));
  race_module *module = (race_module *)self;
  if (module != &module_context || module->started == 0u ||
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
      connection_contexts[i].module = module;
      connection_contexts[i].live = 1u;
      ++module->live_connections;
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

static uint64_t ORM_DRIVER_CALL race_execution_models(void *self) {
  return self == &module_context ? ORM_DRIVER_EXEC_CALLER_BLOCKING
                                 : UINT64_C(0);
}

static const TurboDb_Driver_vtable driver_vtable = {
    .implementation = driver_id,
    .capabilities = 0u,
    .create = race_create_connection,
    .execution_models = race_execution_models};

static TurboDb_Driver driver = {
    &module_context, &driver_vtable};

static const salts_plugin_export exports[] = {{
    .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
    .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
    .contract_version = ORM_DRIVER_INTERFACE_CONTRACT_VERSION,
    .capabilities = 0u,
    .export_id = ORM_DRIVER_PLUGIN_EXPORT_ID,
    .contract_id = ORM_DRIVER_INTERFACE_CONTRACT_ID,
    .value.interface = {&TurboDb_Driver_interface_meta, &driver}}};

static const salts_plugin_manifest manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = SALTS_PLUGIN_ABI_VERSION,
    .plugin_id = driver_id,
    .version = {1u, 0u, 0u},
    .exports = exports,
    .export_count = 1u,
    .self = &module_context,
    .start = race_start,
    .request_stop = race_request_stop,
    .is_quiescent = race_is_quiescent,
    .destroy = race_destroy};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
  return host_abi == SALTS_PLUGIN_ABI_VERSION ? &manifest : NULL;
}
