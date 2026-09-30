#include "../../../drivers/sqlite/schema.h"
#include <salts/plugin.h>

static const salts_plugin_export exports[] = {{
    .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
    .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
    .contract_version = DBTOOL_SCHEMA_CONTRACT_VERSION,
    .export_id = DBTOOL_SCHEMA_EXPORT_ID,
    .contract_id = DBTOOL_SCHEMA_CONTRACT_ID,
    .value.interface = {&TurboDb_SchemaApply_interface_meta, &dbtool_sqlite_schema}}};
static const salts_plugin_manifest manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = SALTS_PLUGIN_ABI_VERSION,
    .plugin_id = "sqlite", .version = {1u, 0u, 0u},
    .exports = exports, .export_count = 1u};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
  return host_abi == SALTS_PLUGIN_ABI_VERSION ? &manifest : NULL;
}
