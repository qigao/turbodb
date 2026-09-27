#include <salts/plugin.h>

#include <cmeta/interface.h>

#include <stdint.h>

#if SALTS_PLUGIN_ABI_VERSION != 2u
#error "TurboDB requires SaltsUtils Plugin ABI 2"
#endif

int main(void) {
  int state = 0;
  const cmeta_interface_desc host_desc = {
      sizeof(cmeta_interface_desc), "TurboDb.Driver", NULL, 0u};
  const cmeta_interface_desc plugin_desc = {
      sizeof(cmeta_interface_desc), "TurboDb.Driver", NULL, 0u};
  const salts_plugin_export entry = {
      .struct_size = SALTS_PLUGIN_EXPORT_SIZE,
      .kind = SALTS_PLUGIN_EXPORT_INTERFACE,
      .contract_version = 1u,
      .capabilities = UINT64_C(0),
      .export_id = "driver",
      .contract_id = "TurboDb.Driver",
      .value.interface = {.desc = &plugin_desc, .value = &state}};

  if (!cmeta_interface_desc_valid(&host_desc) ||
      !cmeta_interface_desc_valid(&plugin_desc))
    return 1;
  if (!cmeta_interface_desc_equal(&host_desc, &plugin_desc))
    return 2;
  if (salts_plugin_export_require_interface(
          &entry, "TurboDb.Driver", 1u, UINT64_C(0), &host_desc) !=
      SALTS_PLUGIN_OK)
    return 3;
  return 0;
}
