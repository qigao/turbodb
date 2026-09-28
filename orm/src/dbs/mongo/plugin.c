#include <orm_driver_plugin.h>

#include "backend.h"
#include "orm_driver_backend_bridge.h"

#include <mongoc/mongoc.h>

#define ORM_MONGODB_DRIVER_CAPABILITIES                                     \
  (ORM_DRIVER_CAP_SELECT | ORM_DRIVER_CAP_INSERT |                         \
   ORM_DRIVER_CAP_UPDATE | ORM_DRIVER_CAP_DELETE |                         \
   ORM_DRIVER_CAP_TRANSACTION | ORM_DRIVER_CAP_INCREMENTAL_ROWS |          \
   ORM_DRIVER_CAP_READ_UNCOMMITTED | ORM_DRIVER_CAP_READ_COMMITTED |       \
   ORM_DRIVER_CAP_REPEATABLE_READ | ORM_DRIVER_CAP_SNAPSHOT |              \
   ORM_DRIVER_CAP_SERIALIZABLE)

static int mongodb_driver_identity;

static const orm_driver_storage_capabilities_v1 mongodb_storage_capabilities =
    ORM_DRIVER_STORAGE_CAPABILITIES_NONE_INIT;

static const orm_driver_storage_capabilities_v1 *ORM_DRIVER_CALL
mongodb_driver_storage_capabilities(void *self) {
  return self == &mongodb_driver_identity ? &mongodb_storage_capabilities : NULL;
}

typedef struct mongodb_plugin_lifecycle {
  int started;
  int stopping;
} mongodb_plugin_lifecycle;

static mongodb_plugin_lifecycle mongodb_lifecycle;

static salts_plugin_status SALTS_PLUGIN_CALL mongodb_plugin_start(void *self) {
  mongodb_plugin_lifecycle *state = (mongodb_plugin_lifecycle *)self;
  if (state != &mongodb_lifecycle)
    return SALTS_PLUGIN_INVALID_ARGUMENT;
  mongoc_init();
  state->started = 1;
  state->stopping = 0;
  return SALTS_PLUGIN_OK;
}

static salts_plugin_status SALTS_PLUGIN_CALL
mongodb_plugin_request_stop(void *self) {
  mongodb_plugin_lifecycle *state = (mongodb_plugin_lifecycle *)self;
  if (state != &mongodb_lifecycle)
    return SALTS_PLUGIN_INVALID_ARGUMENT;
  state->stopping = 1;
  return SALTS_PLUGIN_OK;
}

static bool SALTS_PLUGIN_CALL mongodb_plugin_is_quiescent(const void *self) {
  const mongodb_plugin_lifecycle *state =
      (const mongodb_plugin_lifecycle *)self;
  return state == &mongodb_lifecycle && state->stopping != 0;
}

static void SALTS_PLUGIN_CALL mongodb_plugin_destroy(void *self) {
  mongodb_plugin_lifecycle *state = (mongodb_plugin_lifecycle *)self;
  if (state != &mongodb_lifecycle || !state->started)
    return;
  /*
   * Salts invokes destroy only after all Driver leases are gone and before
   * unloading the DSO. MongoDB is the sole mongoc consumer in TurboDB, so the
   * Driver owns the matching process-runtime cleanup.
   */
  mongoc_cleanup();
  state->started = 0;
  state->stopping = 0;
}

static uint64_t ORM_DRIVER_CALL mongodb_driver_execution_models(
    void *self) {
  return self == &mongodb_driver_identity
             ? (uint64_t)(ORM_DRIVER_EXEC_CALLER_BLOCKING)
             : UINT64_C(0);
}

static orm_status_t ORM_DRIVER_CALL mongodb_driver_create(
    void *self, const orm_config_t *config,
    const orm_driver_limits_v1 *limits,
    orm_driver_connection_v1 *out_connection,
    orm_error_t *error) {
  if (self != &mongodb_driver_identity)
    return ORM_STATUS_INVALID_ARGUMENT;
  return orm_driver_backend_connection_create(
      orm_mongo_backend_create, config, limits, out_connection, error);
}

static const TurboDb_Driver_vtable mongodb_driver_vtable = {
    .implementation = "mongodb",
    .capabilities = ORM_MONGODB_DRIVER_CAPABILITIES,
    .create = mongodb_driver_create,
    .execution_models = mongodb_driver_execution_models,
    .storage_capabilities = mongodb_driver_storage_capabilities};

static TurboDb_Driver mongodb_driver = {
    &mongodb_driver_identity, &mongodb_driver_vtable};

static const salts_plugin_export mongodb_exports[] = {{
    .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
    .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
    .contract_version = ORM_DRIVER_INTERFACE_CONTRACT_VERSION,
    .capabilities = ORM_MONGODB_DRIVER_CAPABILITIES,
    .export_id = ORM_DRIVER_PLUGIN_EXPORT_ID,
    .contract_id = ORM_DRIVER_INTERFACE_CONTRACT_ID,
    .value.interface = {&TurboDb_Driver_interface_meta, &mongodb_driver}}};

static const salts_plugin_manifest mongodb_manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = SALTS_PLUGIN_ABI_VERSION,
    .plugin_id = "mongodb",
    .version = {1u, 0u, 0u},
    .exports = mongodb_exports,
    .export_count = 1u,
    .self = &mongodb_lifecycle,
    .start = mongodb_plugin_start,
    .request_stop = mongodb_plugin_request_stop,
    .is_quiescent = mongodb_plugin_is_quiescent,
    .destroy = mongodb_plugin_destroy};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
  return host_abi == SALTS_PLUGIN_ABI_VERSION ? &mongodb_manifest : NULL;
}
