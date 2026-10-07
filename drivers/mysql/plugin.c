#include <orm_driver_plugin.h>

#include "backend.h"
#include "schema.h"
#include "orm_driver_backend_bridge.h"

#define ORM_MYSQL_DRIVER_CAPABILITIES                                      \
  (ORM_DRIVER_CAP_SELECT | ORM_DRIVER_CAP_INSERT |                         \
   ORM_DRIVER_CAP_UPDATE | ORM_DRIVER_CAP_DELETE |                         \
   ORM_DRIVER_CAP_RAW_SQL | ORM_DRIVER_CAP_TRANSACTION |                   \
   ORM_DRIVER_CAP_SAVEPOINT | ORM_DRIVER_CAP_INCREMENTAL_ROWS |            \
   ORM_DRIVER_CAP_READ_COMMITTED | ORM_DRIVER_CAP_REPEATABLE_READ |         \
   ORM_DRIVER_CAP_SERIALIZABLE)

static int mysql_driver_identity;

static const orm_driver_storage_capabilities_v1 mysql_storage_capabilities =
    ORM_DRIVER_STORAGE_CAPABILITIES_NONE_INIT;

static const orm_driver_storage_capabilities_v1 *ORM_DRIVER_CALL
mysql_driver_storage_capabilities(void *self) {
  return self == &mysql_driver_identity ? &mysql_storage_capabilities : NULL;
}

static uint64_t ORM_DRIVER_CALL mysql_driver_execution_models(
    void *self) {
  return self == &mysql_driver_identity
             ? (uint64_t)(ORM_DRIVER_EXEC_CALLER_BLOCKING | ORM_DRIVER_EXEC_NATIVE_WAIT)
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
    .execution_models = mysql_driver_execution_models,
    .storage_capabilities = mysql_driver_storage_capabilities};

static TurboDb_Driver mysql_driver = {
    &mysql_driver_identity, &mysql_driver_vtable};

static const cmeta_plugin_export mysql_exports[] = {{
    .struct_size = CMETA_PLUGIN_EXPORT_SIZE,
    .kind = CMETA_PLUGIN_EXPORT_INTERFACE,
    .contract_version = ORM_DRIVER_INTERFACE_CONTRACT_VERSION,
    .capabilities = ORM_MYSQL_DRIVER_CAPABILITIES,
    .export_id = ORM_DRIVER_PLUGIN_EXPORT_ID,
    .contract_id = ORM_DRIVER_INTERFACE_CONTRACT_ID,
    .value.interface = {
        &TurboDb_Driver_interface_meta, &mysql_driver}},
    {
    .struct_size = CMETA_PLUGIN_EXPORT_SIZE,
    .kind = CMETA_PLUGIN_EXPORT_INTERFACE,
    .contract_version = DBTOOL_SCHEMA_CONTRACT_VERSION,
    .export_id = DBTOOL_SCHEMA_EXPORT_ID,
    .contract_id = DBTOOL_SCHEMA_CONTRACT_ID,
    .value.interface = {&TurboDb_SchemaApply_interface_meta, &dbtool_mysql_schema}}};

static const cmeta_plugin_manifest mysql_manifest = {
    .struct_size = CMETA_PLUGIN_MANIFEST_SIZE,
    .abi_version = CMETA_PLUGIN_ABI_VERSION,
    .plugin_id = "mysql",
    .version = {1u, 0u, 0u},
    .exports = mysql_exports,
    .export_count = sizeof(mysql_exports) / sizeof(mysql_exports[0])};

CMETA_PLUGIN_QUERY_EXPORT const cmeta_plugin_manifest *CMETA_PLUGIN_CALL
cmeta_plugin_query(uint32_t host_abi) {
  return host_abi == CMETA_PLUGIN_ABI_VERSION
             ? &mysql_manifest
             : NULL;
}
