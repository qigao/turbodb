#ifndef ORM_DRIVER_PLUGIN_H
#define ORM_DRIVER_PLUGIN_H

#include "orm_driver_interface.h"

#include <salts/plugin.h>

#define ORM_DRIVER_PLUGIN_CONTRACT_ID "TurboDb.Driver"
#define ORM_DRIVER_PLUGIN_CONTRACT_VERSION UINT32_C(1)

typedef struct orm_driver_plugin_binding {
  turbodb_driver driver;
  uint64_t capabilities;
  uint64_t execution_models;
} orm_driver_plugin_binding;

CMETA_INLINE bool orm_driver_plugin_binding_valid(
    const orm_driver_plugin_binding *binding) {
  return binding != NULL &&
         turbodb_driver_valid(&binding->driver) &&
         binding->capabilities == turbodb_driver_capabilities(&binding->driver) &&
         binding->execution_models != 0u &&
         (binding->execution_models & ~ORM_DRIVER_EXEC_KNOWN_MASK) == 0u;
}

/*
 * Control-plane admission. The returned binding copies only the typed
 * {self,vtable} handle and validated metadata. The caller must independently
 * retain the Salts::Plugin lease for as long as this binding or any object
 * created through it can execute module code.
 */
CMETA_INLINE salts_plugin_status orm_driver_plugin_bind_export(
    const salts_plugin_export *entry,
    uint64_t required_capabilities,
    orm_driver_plugin_binding *out_binding) {
  salts_plugin_status status;
  turbodb_driver *driver;
  uint64_t execution_models;

  if (out_binding == NULL)
    return SALTS_PLUGIN_INVALID_ARGUMENT;

  out_binding->driver.self = NULL;
  out_binding->driver.vtable = NULL;
  out_binding->capabilities = 0u;
  out_binding->execution_models = 0u;

  if ((required_capabilities & ~ORM_DRIVER_CAP_KNOWN_MASK) != 0u)
    return SALTS_PLUGIN_INVALID_ARGUMENT;

  status = salts_plugin_export_require_interface(
      entry, ORM_DRIVER_PLUGIN_CONTRACT_ID,
      ORM_DRIVER_PLUGIN_CONTRACT_VERSION, required_capabilities,
      turbodb_driver_interface());
  if (status != SALTS_PLUGIN_OK)
    return status;

  if ((entry->capabilities & ~ORM_DRIVER_CAP_KNOWN_MASK) != 0u)
    return SALTS_PLUGIN_INCOMPATIBLE_CONTRACT;

  driver = (turbodb_driver *)entry->value.interface.value;
  if (!turbodb_driver_valid(driver) ||
      turbodb_driver_capabilities(driver) != entry->capabilities)
    return SALTS_PLUGIN_INCOMPATIBLE_CONTRACT;

  execution_models = turbodb_driver_execution_models(driver);
  if (execution_models == 0u ||
      (execution_models & ~ORM_DRIVER_EXEC_KNOWN_MASK) != 0u)
    return SALTS_PLUGIN_INCOMPATIBLE_CONTRACT;

  out_binding->driver = *driver;
  out_binding->capabilities = entry->capabilities;
  out_binding->execution_models = execution_models;
  return SALTS_PLUGIN_OK;
}

#endif
