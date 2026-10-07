#include <salts/plugin.h>

static const cmeta_plugin_manifest bad_manifest = {
    .struct_size = CMETA_PLUGIN_MANIFEST_SIZE,
    .abi_version = CMETA_PLUGIN_ABI_VERSION - 1u,
    .plugin_id = "badabi",
    .version = {1u, 0u, 0u},
    .exports = NULL,
    .export_count = 0u};

CMETA_PLUGIN_QUERY_EXPORT const cmeta_plugin_manifest *CMETA_PLUGIN_CALL
cmeta_plugin_query(uint32_t host_abi) {
  (void)host_abi;
  return &bad_manifest;
}
