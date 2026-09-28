#include <orm_driver_plugin.h>

#include "orm_driver_backend_bridge.h"
#include "orm_internal.h"

#include <stdint.h>

#define ORM_SQLITE_DRIVER_CAPABILITIES                                      \
  (ORM_DRIVER_CAP_SELECT | ORM_DRIVER_CAP_INSERT |                         \
   ORM_DRIVER_CAP_UPDATE | ORM_DRIVER_CAP_DELETE |                         \
   ORM_DRIVER_CAP_RAW_SQL | ORM_DRIVER_CAP_TRANSACTION |                   \
   ORM_DRIVER_CAP_SAVEPOINT | ORM_DRIVER_CAP_INCREMENTAL_ROWS)

static int sqlite_driver_identity;

static const orm_driver_storage_capabilities_v1 sqlite_storage_capabilities = {
    .header = {(uint32_t)sizeof(orm_driver_storage_capabilities_v1),
               ORM_DRIVER_STORAGE_ABI_VERSION},
    .capabilities = ORM_DRIVER_STORAGE_CAP_ATOMIC_STATE_METADATA,
    .max_batch_operations = 0u,
    .max_batch_bytes = 0u,
    .max_progress_metadata_bytes = ORM_DRIVER_STORAGE_LIMIT_CONFIGURED,
    .max_checkpoint_chunk_bytes = 0u,
    .max_restore_chunk_bytes = 0u};

static const orm_driver_storage_capabilities_v1 *ORM_DRIVER_CALL
sqlite_driver_storage_capabilities(void *self) {
  return self == &sqlite_driver_identity ? &sqlite_storage_capabilities : NULL;
}

static uint64_t ORM_DRIVER_CALL sqlite_driver_execution_models(
    void *self) {
  return self == &sqlite_driver_identity
             ? (uint64_t)(ORM_DRIVER_EXEC_CALLER_BLOCKING)
             : UINT64_C(0);
}

static orm_status_t ORM_DRIVER_CALL sqlite_driver_create(
    void *self, const orm_config_t *config,
    const orm_driver_limits_v1 *limits,
    orm_driver_connection_v1 *out_connection,
    orm_error_t *error) {
  if (self != &sqlite_driver_identity)
    return ORM_STATUS_INVALID_ARGUMENT;
  return orm_driver_backend_connection_create(
      orm_sqlite_backend_create, config, limits, out_connection, error);
}

static const TurboDb_Driver_vtable sqlite_driver_vtable = {
    .implementation = "sqlite",
    .capabilities = ORM_SQLITE_DRIVER_CAPABILITIES,
    .create = sqlite_driver_create,
    .execution_models = sqlite_driver_execution_models,
    .storage_capabilities = sqlite_driver_storage_capabilities};

static TurboDb_Driver sqlite_driver = {
    &sqlite_driver_identity, &sqlite_driver_vtable};

static const salts_plugin_export sqlite_exports[] = {{
    .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
    .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
    .contract_version = ORM_DRIVER_INTERFACE_CONTRACT_VERSION,
    .capabilities = ORM_SQLITE_DRIVER_CAPABILITIES,
    .export_id = ORM_DRIVER_PLUGIN_EXPORT_ID,
    .contract_id = ORM_DRIVER_INTERFACE_CONTRACT_ID,
    .value.interface = {&TurboDb_Driver_interface_meta, &sqlite_driver}}};

static const salts_plugin_manifest sqlite_manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = SALTS_PLUGIN_ABI_VERSION,
    .plugin_id = "sqlite",
    .version = {1u, 0u, 0u},
    .exports = sqlite_exports,
    .export_count = 1u};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
  return host_abi == SALTS_PLUGIN_ABI_VERSION ? &sqlite_manifest : NULL;
}
