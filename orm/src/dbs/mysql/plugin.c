#include <orm_driver_plugin.h>

#include "backend.h"
#include "orm_driver_backend_bridge.h"

#define ORM_MYSQL_DRIVER_CAPABILITIES                                      \
  (ORM_DRIVER_CAP_RAW_SQL | ORM_DRIVER_CAP_TRANSACTION |                  \
   ORM_DRIVER_CAP_SAVEPOINT | ORM_DRIVER_CAP_INCREMENTAL_ROWS |           \
   ORM_DRIVER_CAP_READ_COMMITTED | ORM_DRIVER_CAP_REPEATABLE_READ)

static int mysql_driver_identity;

static uint64_t ORM_DRIVER_CALL mysql_driver_execution_models(
    void *self) {
  return self == &mysql_driver_identity
             ? (uint64_t)ORM_DRIVER_EXEC_CALLER_BLOCKING
             : UINT64_C(0);
}

static orm_status_t ORM_DRIVER_CALL mysql_driver_create(
    void *self, const orm_config_t *config,
    const orm_driver_limits_v1 *limits,
    orm_driver_connection_v1 *out_connection,
    orm_error_t *error) {
  if (self != &mysql_driver_identity)
    return ORM_STATUS_INVALID_ARGUMENT;
  return orm_driver_backend_connection_create(
      orm_mysql_backend_create,
      config, limits, out_connection, error);
}

static const TurboDb_Driver_vtable mysql_driver_vtable = {
    .implementation = "mysql",
    .capabilities = ORM_MYSQL_DRIVER_CAPABILITIES,
    .create = mysql_driver_create,
    .execution_models = mysql_driver_execution_models};

static TurboDb_Driver mysql_driver = {
    &mysql_driver_identity, &mysql_driver_vtable};

static const salts_plugin_export mysql_exports[] = {{
    .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
    .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
    .contract_version = ORM_DRIVER_INTERFACE_CONTRACT_VERSION,
    .capabilities = ORM_MYSQL_DRIVER_CAPABILITIES,
    .export_id = ORM_DRIVER_PLUGIN_EXPORT_ID,
    .contract_id = ORM_DRIVER_INTERFACE_CONTRACT_ID,
    .value.interface = {
        &TurboDb_Driver_interface_meta, &mysql_driver}}};

static const salts_plugin_manifest mysql_manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = SALTS_PLUGIN_ABI_VERSION,
    .plugin_id = "mysql",
    .version = {1u, 0u, 0u},
    .exports = mysql_exports,
    .export_count = 1u};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
  return host_abi == SALTS_PLUGIN_ABI_VERSION
             ? &mysql_manifest
             : NULL;
}
