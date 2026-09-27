#ifndef ORM_DRIVER_PLUGIN_H
#define ORM_DRIVER_PLUGIN_H

#include "orm_driver_interface.h"

#include <salts/plugin.h>

#if SALTS_PLUGIN_ABI_VERSION != 2u
#error "TurboDB Driver plugins require SaltsUtils Plugin ABI 2"
#endif

#define ORM_DRIVER_PLUGIN_EXPORT_ID "driver"

/*
 * Construct the canonical Plugin publication row.  The descriptor and typed
 * interface value are borrowed from the plugin module and therefore remain
 * usable only while the host retains a live Salts::Plugin lease.
 */
CMETA_INLINE salts_plugin_export orm_driver_plugin_export(
    TurboDb_Driver *driver, uint64_t capabilities) {
#ifdef __cplusplus
  salts_plugin_export entry{};
#else
  salts_plugin_export entry = {0};
#endif
  entry.struct_size = SALTS_PLUGIN_EXPORT_SIZE;
  entry.kind = SALTS_PLUGIN_EXPORT_INTERFACE;
  entry.contract_version = ORM_DRIVER_INTERFACE_CONTRACT_VERSION;
  entry.capabilities = capabilities;
  entry.export_id = ORM_DRIVER_PLUGIN_EXPORT_ID;
  entry.contract_id = ORM_DRIVER_INTERFACE_CONTRACT_ID;
  entry.value.interface.desc = TurboDb_Driver_interface();
  entry.value.interface.value = driver;
  return entry;
}

#endif /* ORM_DRIVER_PLUGIN_H */
