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
    orm_error_init(error);
    orm_error_set(error, ORM_STATUS_UNSUPPORTED,
                  "bad contract fixture must never be invoked");
  }
  return ORM_STATUS_UNSUPPORTED;
}

static const TurboDb_Driver_vtable bad_contract_vtable = {
    .implementation = "bad-contract-version",
    .capabilities = 0u,
    .create = bad_contract_create};

static TurboDb_Driver bad_contract_driver = {
    NULL, &bad_contract_vtable};

static const salts_plugin_export bad_contract_exports[] = {{
    .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
    .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
    .contract_version = ORM_DRIVER_INTERFACE_CONTRACT_VERSION + 1u,
    .capabilities = 0u,
    .export_id = ORM_DRIVER_PLUGIN_EXPORT_ID,
    .contract_id = ORM_DRIVER_INTERFACE_CONTRACT_ID,
    .value.interface = {&TurboDb_Driver_interface_meta, &bad_contract_driver}}};

static const salts_plugin_manifest bad_contract_manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = SALTS_PLUGIN_ABI_VERSION,
    .plugin_id = "badcontract",
    .version = {1u, 0u, 0u},
    .exports = bad_contract_exports,
    .export_count = 1u};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
  return host_abi == SALTS_PLUGIN_ABI_VERSION ? &bad_contract_manifest : NULL;
}
