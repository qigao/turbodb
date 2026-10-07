#include "../../../drivers/sqlite/schema.h"
#include <salts/plugin.h>

static const cmeta_plugin_export exports[] = {{
    .struct_size = CMETA_PLUGIN_EXPORT_SIZE,
    .kind = CMETA_PLUGIN_EXPORT_INTERFACE,
    .contract_version = DBTOOL_SCHEMA_CONTRACT_VERSION,
    .export_id = DBTOOL_SCHEMA_EXPORT_ID,
    .contract_id = DBTOOL_SCHEMA_CONTRACT_ID,
    .value.interface = {&TurboDb_SchemaApply_interface_meta, &dbtool_sqlite_schema}}};
static const cmeta_plugin_manifest manifest = {
    .struct_size = CMETA_PLUGIN_MANIFEST_SIZE,
    .abi_version = CMETA_PLUGIN_ABI_VERSION,
    .plugin_id = "sqlite", .version = {1u, 0u, 0u},
    .exports = exports, .export_count = 1u};

CMETA_PLUGIN_QUERY_EXPORT const cmeta_plugin_manifest *CMETA_PLUGIN_CALL
cmeta_plugin_query(uint32_t host_abi) {
  return host_abi == CMETA_PLUGIN_ABI_VERSION ? &manifest : NULL;
}
