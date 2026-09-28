#include <orm_driver_plugin.h>

#include "backend.h"
#include "bridge.h"
#include "orm_driver_backend_bridge.h"

#define ORM_TIDESDB_DRIVER_CAPABILITIES                                     \
  (ORM_DRIVER_CAP_SELECT | ORM_DRIVER_CAP_INSERT |                         \
   ORM_DRIVER_CAP_UPDATE | ORM_DRIVER_CAP_DELETE |                         \
   ORM_DRIVER_CAP_TRANSACTION | ORM_DRIVER_CAP_SAVEPOINT |                 \
   ORM_DRIVER_CAP_INCREMENTAL_ROWS |                                       \
   ORM_DRIVER_CAP_READ_UNCOMMITTED | ORM_DRIVER_CAP_READ_COMMITTED |       \
   ORM_DRIVER_CAP_REPEATABLE_READ | ORM_DRIVER_CAP_SNAPSHOT |              \
   ORM_DRIVER_CAP_SERIALIZABLE)

static int tidesdb_driver_identity;

typedef struct tidesdb_plugin_lifecycle {
  int started;
  int stopping;
} tidesdb_plugin_lifecycle;

static tidesdb_plugin_lifecycle tidesdb_lifecycle;

static salts_plugin_status SALTS_PLUGIN_CALL tidesdb_plugin_start(void *self) {
  tidesdb_plugin_lifecycle *state = (tidesdb_plugin_lifecycle *)self;
  if (state != &tidesdb_lifecycle)
    return SALTS_PLUGIN_INVALID_ARGUMENT;
  state->started = 1;
  state->stopping = 0;
  return SALTS_PLUGIN_OK;
}

static salts_plugin_status SALTS_PLUGIN_CALL
tidesdb_plugin_request_stop(void *self) {
  tidesdb_plugin_lifecycle *state = (tidesdb_plugin_lifecycle *)self;
  if (state != &tidesdb_lifecycle)
    return SALTS_PLUGIN_INVALID_ARGUMENT;
  state->stopping = 1;
  return SALTS_PLUGIN_OK;
}

static bool SALTS_PLUGIN_CALL tidesdb_plugin_is_quiescent(const void *self) {
  const tidesdb_plugin_lifecycle *state =
      (const tidesdb_plugin_lifecycle *)self;
  return state == &tidesdb_lifecycle && state->stopping != 0;
}

static void SALTS_PLUGIN_CALL tidesdb_plugin_destroy(void *self) {
  tidesdb_plugin_lifecycle *state = (tidesdb_plugin_lifecycle *)self;
  if (state != &tidesdb_lifecycle)
    return;
  /*
   * Salts calls destroy() after leases/callbacks quiesce and before closing
   * the DSO. This is the last safe point to remove Win32 FLS callbacks that
   * are implemented by code statically linked into this Driver module.
   */
  orm_tidesdb_module_cleanup();
  state->started = 0;
  state->stopping = 0;
}

static uint64_t ORM_DRIVER_CALL tidesdb_driver_execution_models(
    void *self) {
  return self == &tidesdb_driver_identity
             ? (uint64_t)(ORM_DRIVER_EXEC_CALLER_BLOCKING)
             : UINT64_C(0);
}

static orm_status_t ORM_DRIVER_CALL tidesdb_driver_create(
    void *self, const orm_config_t *config,
    const orm_driver_limits_v1 *limits,
    orm_driver_connection_v1 *out_connection,
    orm_error_t *error) {
  if (self != &tidesdb_driver_identity)
    return ORM_STATUS_INVALID_ARGUMENT;
  return orm_driver_backend_connection_create(
      orm_tidesdb_backend_create, config, limits, out_connection, error);
}

static const TurboDb_Driver_vtable tidesdb_driver_vtable = {
    .implementation = "tidesdb",
    .capabilities = ORM_TIDESDB_DRIVER_CAPABILITIES,
    .create = tidesdb_driver_create,
    .execution_models = tidesdb_driver_execution_models};

static TurboDb_Driver tidesdb_driver = {
    &tidesdb_driver_identity, &tidesdb_driver_vtable};

static const salts_plugin_export tidesdb_exports[] = {{
    .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
    .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
    .contract_version = ORM_DRIVER_INTERFACE_CONTRACT_VERSION,
    .capabilities = ORM_TIDESDB_DRIVER_CAPABILITIES,
    .export_id = ORM_DRIVER_PLUGIN_EXPORT_ID,
    .contract_id = ORM_DRIVER_INTERFACE_CONTRACT_ID,
    .value.interface = {&TurboDb_Driver_interface_meta, &tidesdb_driver}}};

static const salts_plugin_manifest tidesdb_manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = SALTS_PLUGIN_ABI_VERSION,
    .plugin_id = "tidesdb",
    .version = {1u, 0u, 0u},
    .exports = tidesdb_exports,
    .export_count = 1u,
    .self = &tidesdb_lifecycle,
    .start = tidesdb_plugin_start,
    .request_stop = tidesdb_plugin_request_stop,
    .is_quiescent = tidesdb_plugin_is_quiescent,
    .destroy = tidesdb_plugin_destroy};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
  return host_abi == SALTS_PLUGIN_ABI_VERSION ? &tidesdb_manifest : NULL;
}
