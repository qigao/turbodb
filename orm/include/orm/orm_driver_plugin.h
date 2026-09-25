#ifndef ORM_DRIVER_PLUGIN_H
#define ORM_DRIVER_PLUGIN_H

#include "orm_driver_ops.h"

#include <salts/plugin.h>

#define ORM_DRIVER_PLUGIN_CONTRACT_ID "TurboDb.Driver"
#define ORM_DRIVER_PLUGIN_CONTRACT_VERSION UINT32_C(1)

/*
 * Canonical reflected database-driver capability.
 *
 * Salts::Plugin owns DSO publication, registry, lifecycle and leases.
 * This interface owns only TurboDB database semantics. The runtime resolves it
 * once on the control path and caches the typed binding for query hot paths.
 */
CMETA_LOCAL const cmeta_type_identity orm_driver_plugin_status_identity =
    CMETA_TYPE_ID_ATOM_INIT("TurboDb.orm_status");
CMETA_LOCAL const cmeta_type_desc orm_driver_plugin_status_type = {
    "orm_status_t", sizeof(orm_status_t), CMETA_ALIGNOF(orm_status_t),
    CMETA_T_INTEGER, NULL, NULL, &orm_driver_plugin_status_identity
};

CMETA_LOCAL const cmeta_type_identity orm_driver_plugin_execution_models_identity =
    CMETA_TYPE_ID_ATOM_INIT("TurboDb.Driver.ExecutionModels");
CMETA_LOCAL const cmeta_type_desc orm_driver_plugin_execution_models_type = {
    "TurboDb.Driver.ExecutionModels", sizeof(uint64_t), CMETA_ALIGNOF(uint64_t),
    CMETA_T_INTEGER, NULL, NULL, &orm_driver_plugin_execution_models_identity
};

CMETA_LOCAL const cmeta_type_identity orm_driver_plugin_config_identity =
    CMETA_TYPE_ID_ATOM_INIT("TurboDb.orm_config.v4");
CMETA_LOCAL const cmeta_type_identity orm_driver_plugin_config_ptr_identity =
    CMETA_TYPE_ID_POINTER_INIT(&orm_driver_plugin_config_identity);
CMETA_LOCAL const cmeta_type_desc orm_driver_plugin_config_type = {
    "orm_config_t", sizeof(orm_config_t), CMETA_ALIGNOF(orm_config_t),
    CMETA_T_OBJECT, NULL, NULL, &orm_driver_plugin_config_identity
};
CMETA_LOCAL const cmeta_type_desc orm_driver_plugin_config_ptr_type = {
    "const orm_config_t *", sizeof(const orm_config_t *),
    CMETA_ALIGNOF(const orm_config_t *), CMETA_T_POINTER,
    &orm_driver_plugin_config_type, NULL, &orm_driver_plugin_config_ptr_identity
};

CMETA_LOCAL const cmeta_type_identity orm_driver_plugin_limits_identity =
    CMETA_TYPE_ID_ATOM_INIT("TurboDb.Driver.Limits.v1");
CMETA_LOCAL const cmeta_type_identity orm_driver_plugin_limits_ptr_identity =
    CMETA_TYPE_ID_POINTER_INIT(&orm_driver_plugin_limits_identity);
CMETA_LOCAL const cmeta_type_desc orm_driver_plugin_limits_type = {
    "orm_driver_limits_v1", sizeof(orm_driver_limits_v1),
    CMETA_ALIGNOF(orm_driver_limits_v1), CMETA_T_OBJECT, NULL, NULL,
    &orm_driver_plugin_limits_identity
};
CMETA_LOCAL const cmeta_type_desc orm_driver_plugin_limits_ptr_type = {
    "const orm_driver_limits_v1 *", sizeof(const orm_driver_limits_v1 *),
    CMETA_ALIGNOF(const orm_driver_limits_v1 *), CMETA_T_POINTER,
    &orm_driver_plugin_limits_type, NULL, &orm_driver_plugin_limits_ptr_identity
};

CMETA_LOCAL const cmeta_type_identity orm_driver_plugin_connection_identity =
    CMETA_TYPE_ID_ATOM_INIT("TurboDb.Driver.Connection.v1");
CMETA_LOCAL const cmeta_type_identity orm_driver_plugin_connection_ptr_identity =
    CMETA_TYPE_ID_POINTER_INIT(&orm_driver_plugin_connection_identity);
CMETA_LOCAL const cmeta_type_desc orm_driver_plugin_connection_type = {
    "orm_driver_connection_v1", sizeof(orm_driver_connection_v1),
    CMETA_ALIGNOF(orm_driver_connection_v1), CMETA_T_OBJECT, NULL, NULL,
    &orm_driver_plugin_connection_identity
};
CMETA_LOCAL const cmeta_type_desc orm_driver_plugin_connection_ptr_type = {
    "orm_driver_connection_v1 *", sizeof(orm_driver_connection_v1 *),
    CMETA_ALIGNOF(orm_driver_connection_v1 *), CMETA_T_POINTER,
    &orm_driver_plugin_connection_type, NULL,
    &orm_driver_plugin_connection_ptr_identity
};

CMETA_LOCAL const cmeta_type_identity orm_driver_plugin_error_identity =
    CMETA_TYPE_ID_ATOM_INIT("TurboDb.orm_error.v4");
CMETA_LOCAL const cmeta_type_identity orm_driver_plugin_error_ptr_identity =
    CMETA_TYPE_ID_POINTER_INIT(&orm_driver_plugin_error_identity);
CMETA_LOCAL const cmeta_type_desc orm_driver_plugin_error_type = {
    "orm_error_t", sizeof(orm_error_t), CMETA_ALIGNOF(orm_error_t),
    CMETA_T_OBJECT, NULL, NULL, &orm_driver_plugin_error_identity
};
CMETA_LOCAL const cmeta_type_desc orm_driver_plugin_error_ptr_type = {
    "orm_error_t *", sizeof(orm_error_t *), CMETA_ALIGNOF(orm_error_t *),
    CMETA_T_POINTER, &orm_driver_plugin_error_type, NULL,
    &orm_driver_plugin_error_ptr_identity
};

#define TURBODB_DRIVER_METHODS(X, I) X(I,F0,uint64_t,execution_models,value,&orm_driver_plugin_execution_models_type,CMETA_ABI_SCALAR) X(I,F4,orm_status_t,connect,io,&orm_driver_plugin_status_type,CMETA_ABI_SCALAR,(const orm_config_t *,config,CMETA_PARAM_IN | CMETA_PARAM_BORROWED,&orm_driver_plugin_config_ptr_type,CMETA_ABI_OBJECT_POINTER),(const orm_driver_limits_v1 *,limits,CMETA_PARAM_IN | CMETA_PARAM_BORROWED,&orm_driver_plugin_limits_ptr_type,CMETA_ABI_OBJECT_POINTER),(orm_driver_connection_v1 *,out_connection,CMETA_PARAM_OUT,&orm_driver_plugin_connection_ptr_type,CMETA_ABI_OBJECT_POINTER),(orm_error_t *,error,CMETA_PARAM_OUT | CMETA_PARAM_NULLABLE,&orm_driver_plugin_error_ptr_type,CMETA_ABI_OBJECT_POINTER))

CMETA_INTERFACE(turbodb_driver, TURBODB_DRIVER_METHODS);

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
