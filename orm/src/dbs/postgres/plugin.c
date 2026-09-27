#include <orm_driver_plugin.h>

#include "orm_driver_backend_bridge.h"
#include "orm_internal.h"

#define ORM_POSTGRESQL_DRIVER_CAPABILITIES                                  \
  (ORM_DRIVER_CAP_SELECT | ORM_DRIVER_CAP_INSERT |                         \
   ORM_DRIVER_CAP_UPDATE | ORM_DRIVER_CAP_DELETE |                         \
   ORM_DRIVER_CAP_RAW_SQL | ORM_DRIVER_CAP_TRANSACTION |                   \
   ORM_DRIVER_CAP_SAVEPOINT | ORM_DRIVER_CAP_INCREMENTAL_ROWS |            \
   ORM_DRIVER_CAP_READ_COMMITTED | ORM_DRIVER_CAP_REPEATABLE_READ |        \
   ORM_DRIVER_CAP_SERIALIZABLE)

static int postgresql_driver_identity;

static orm_status_t ORM_DRIVER_CALL postgresql_driver_create(
    void *self, const orm_config_t *config,
    const orm_driver_limits_v1 *limits,
    orm_driver_connection_v1 *out_connection,
    orm_error_t *error) {
  if (self != &postgresql_driver_identity)
    return ORM_STATUS_INVALID_ARGUMENT;
  return orm_driver_backend_connection_create(
      orm_postgres_backend_create, config, limits, out_connection, error);
}

static const TurboDb_Driver_vtable postgresql_driver_vtable = {
    .implementation = "postgresql",
    .capabilities = ORM_POSTGRESQL_DRIVER_CAPABILITIES,
    .create = postgresql_driver_create};

static TurboDb_Driver postgresql_driver = {
    &postgresql_driver_identity, &postgresql_driver_vtable};

static const salts_plugin_export postgresql_exports[] = {{
    .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
    .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
    .contract_version = ORM_DRIVER_INTERFACE_CONTRACT_VERSION,
    .capabilities = ORM_POSTGRESQL_DRIVER_CAPABILITIES,
    .export_id = ORM_DRIVER_PLUGIN_EXPORT_ID,
    .contract_id = ORM_DRIVER_INTERFACE_CONTRACT_ID,
    .value.interface = {&TurboDb_Driver_interface_meta, &postgresql_driver}}};

static const salts_plugin_manifest postgresql_manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = SALTS_PLUGIN_ABI_VERSION,
    .plugin_id = "postgresql",
    .version = {1u, 0u, 0u},
    .exports = postgresql_exports,
    .export_count = 1u};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
  return host_abi == SALTS_PLUGIN_ABI_VERSION ? &postgresql_manifest : NULL;
}
