#include <orm_driver_plugin.h>

#include <string.h>

static orm_status_t ORM_DRIVER_CALL bad_contract_create(
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

static const orm_driver_storage_capabilities_v1 bad_contract_storage =
    ORM_DRIVER_STORAGE_CAPABILITIES_NONE_INIT;

static const orm_driver_storage_capabilities_v1 *ORM_DRIVER_CALL
bad_contract_storage_capabilities(void *self) {
  return self != NULL ? &bad_contract_storage : NULL;
}

static uint64_t ORM_DRIVER_CALL
bad_contract_execution_models(void *self) {
  return self != NULL ? ORM_DRIVER_EXEC_CALLER_BLOCKING : UINT64_C(0);
}

static const TurboDb_Driver_vtable bad_contract_vtable = {
    .implementation = "bad-contract-version",
    .capabilities = 0u,
    .create = bad_contract_create,
    .execution_models = bad_contract_execution_models,
    .storage_capabilities = bad_contract_storage_capabilities};

static unsigned bad_contract_state;
static TurboDb_Driver bad_contract_driver = {
    &bad_contract_state, &bad_contract_vtable};

static const cmeta_plugin_export bad_contract_exports[] = {{
    .struct_size = CMETA_PLUGIN_EXPORT_SIZE,
    .kind = CMETA_PLUGIN_EXPORT_INTERFACE,
    .contract_version = ORM_DRIVER_INTERFACE_CONTRACT_VERSION - 1u,
    .capabilities = 0u,
    .export_id = ORM_DRIVER_PLUGIN_EXPORT_ID,
    .contract_id = ORM_DRIVER_INTERFACE_CONTRACT_ID,
    .value.interface = {&TurboDb_Driver_interface_meta, &bad_contract_driver}}};

static const cmeta_plugin_manifest bad_contract_manifest = {
    .struct_size = CMETA_PLUGIN_MANIFEST_SIZE,
    .abi_version = CMETA_PLUGIN_ABI_VERSION,
    .plugin_id = "badcontract",
    .version = {1u, 0u, 0u},
    .exports = bad_contract_exports,
    .export_count = 1u};

CMETA_PLUGIN_QUERY_EXPORT const cmeta_plugin_manifest *CMETA_PLUGIN_CALL
cmeta_plugin_query(uint32_t host_abi) {
  return host_abi == CMETA_PLUGIN_ABI_VERSION ? &bad_contract_manifest : NULL;
}
