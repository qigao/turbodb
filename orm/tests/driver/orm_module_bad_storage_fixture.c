#include <orm_driver_plugin.h>

#include <string.h>

static orm_status_t ORM_DRIVER_CALL bad_storage_create(
    void *self, const orm_config_t *config,
    const orm_driver_limits_v1 *limits,
    orm_driver_connection_v1 *out_connection, orm_error_t *error) {
  (void)self;
  (void)config;
  (void)limits;
  if (out_connection != NULL)
    memset(out_connection, 0, sizeof(*out_connection));
  if (error != NULL) {
    memset(error, 0, sizeof(*error));
    error->struct_size = (uint32_t)sizeof(*error);
    error->status = ORM_STATUS_UNSUPPORTED;
  }
  return ORM_STATUS_UNSUPPORTED;
}

static uint64_t ORM_DRIVER_CALL bad_storage_execution_models(void *self) {
  return self != NULL ? ORM_DRIVER_EXEC_CALLER_BLOCKING : UINT64_C(0);
}

/* Deliberately invalid: ambiguous commit requires ordered replay + reconcile. */
static const orm_driver_storage_capabilities_v1 bad_storage_capabilities = {
    .header = {(uint32_t)sizeof(orm_driver_storage_capabilities_v1),
               ORM_DRIVER_STORAGE_ABI_VERSION},
    .capabilities = ORM_DRIVER_STORAGE_CAP_AMBIGUOUS_COMMIT,
    .max_batch_operations = 0u,
    .max_batch_bytes = 0u,
    .max_progress_metadata_bytes = 0u,
    .max_checkpoint_chunk_bytes = 0u,
    .max_restore_chunk_bytes = 0u};

static const orm_driver_storage_capabilities_v1 *ORM_DRIVER_CALL
bad_storage_get_capabilities(void *self) {
  return self != NULL ? &bad_storage_capabilities : NULL;
}

static const TurboDb_Driver_vtable bad_storage_vtable = {
    .implementation = "bad-storage",
    .capabilities = 0u,
    .create = bad_storage_create,
    .execution_models = bad_storage_execution_models,
    .storage_capabilities = bad_storage_get_capabilities};

static unsigned bad_storage_state;
static TurboDb_Driver bad_storage_driver = {
    &bad_storage_state, &bad_storage_vtable};

static const salts_plugin_export bad_storage_exports[] = {{
    .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
    .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
    .contract_version = ORM_DRIVER_INTERFACE_CONTRACT_VERSION,
    .capabilities = 0u,
    .export_id = ORM_DRIVER_PLUGIN_EXPORT_ID,
    .contract_id = ORM_DRIVER_INTERFACE_CONTRACT_ID,
    .value.interface = {&TurboDb_Driver_interface_meta, &bad_storage_driver}}};

static const salts_plugin_manifest bad_storage_manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = SALTS_PLUGIN_ABI_VERSION,
    .plugin_id = "badstorage",
    .version = {1u, 0u, 0u},
    .exports = bad_storage_exports,
    .export_count = 1u};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
  return host_abi == SALTS_PLUGIN_ABI_VERSION ? &bad_storage_manifest : NULL;
}
