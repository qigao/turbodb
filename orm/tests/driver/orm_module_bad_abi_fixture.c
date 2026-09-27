#include <salts/plugin.h>

static const salts_plugin_manifest bad_manifest = {
    .struct_size = SALTS_PLUGIN_MANIFEST_SIZE,
    .abi_version = SALTS_PLUGIN_ABI_VERSION - 1u,
    .plugin_id = "badabi",
    .version = {1u, 0u, 0u},
    .exports = NULL,
    .export_count = 0u};

SALTS_PLUGIN_QUERY_EXPORT const salts_plugin_manifest *SALTS_PLUGIN_CALL
salts_plugin_query(uint32_t host_abi) {
  (void)host_abi;
  return &bad_manifest;
}
