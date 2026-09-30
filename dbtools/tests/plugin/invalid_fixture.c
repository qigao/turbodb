#include "dbtool_schema_driver.h"
#include <salts/plugin.h>

/* Deliberately incomplete operations exercise host admission before open. */
static const dbtool_schema_driver_ops invalid_ops = {
    sizeof(dbtool_schema_driver_ops), DBTOOL_SCHEMA_DRIVER_ABI_VERSION,
    NULL, NULL, NULL};
static const TurboDb_SchemaApply_vtable vtable = {
    .implementation = "invalid", .operations = dbtool_schema_operations};
static TurboDb_SchemaApply binding = {(void *)&invalid_ops, &vtable};

static const salts_plugin_export exports[] = {{
    .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
    .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
#if defined(DBTOOL_FIXTURE_CONTRACT)
    .contract_version = DBTOOL_SCHEMA_CONTRACT_VERSION + 1u,
#else
    .contract_version = DBTOOL_SCHEMA_CONTRACT_VERSION,
#endif
#if defined(DBTOOL_FIXTURE_MISSING)
    .export_id = "unrelated",
#else
    .export_id = DBTOOL_SCHEMA_EXPORT_ID,
#endif
    .contract_id = DBTOOL_SCHEMA_CONTRACT_ID,
    .value.interface = {&TurboDb_SchemaApply_interface_meta, &binding}}};
static const salts_plugin_manifest manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
#if defined(DBTOOL_FIXTURE_ABI)
    .abi_version = SALTS_PLUGIN_ABI_VERSION + 1u,
#else
    .abi_version = SALTS_PLUGIN_ABI_VERSION,
#endif
    .plugin_id = "sqlite", .version = {1u, 0u, 0u},
    .exports = exports, .export_count = 1u};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
  return host_abi == SALTS_PLUGIN_ABI_VERSION ? &manifest : NULL;
}
