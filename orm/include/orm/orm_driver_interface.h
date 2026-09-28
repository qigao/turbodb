#ifndef ORM_DRIVER_INTERFACE_H
#define ORM_DRIVER_INTERFACE_H

#include "orm_driver_ops.h"
#include "orm_driver_storage.h"

#include <cmeta/interface.h>

/*
 * Canonical database-driver capability contract.
 *
 * Salts::Plugin owns module publication/discovery/lifetime.  This interface
 * owns only TurboDB database semantics.  The Plugin contract ID is stable
 * across independently compiled host/plugin translation units; CMeta semantic
 * equality, never descriptor pointer identity, admits the interface shape.
 */
#define ORM_DRIVER_INTERFACE_CONTRACT_ID "TurboDb.Driver"
#define ORM_DRIVER_INTERFACE_CONTRACT_VERSION UINT32_C(3)

#ifdef __cplusplus
extern "C" {
#endif

/* Stable CMeta identities for the existing public Driver DTOs used by the
 * reflected create method.  Storage stays TU-local by design; semantic
 * equality is carried by the stable type identities below. */
CMETA_LOCAL const cmeta_type_identity orm_driver_status_type_identity =
    CMETA_TYPE_ID_ATOM_INIT("TurboDb.Status");
CMETA_LOCAL const cmeta_type_desc orm_driver_status_cmeta_type = {
    "orm_status_t", sizeof(orm_status_t), CMETA_ALIGNOF(orm_status_t),
    CMETA_T_INTEGER, NULL, NULL, &orm_driver_status_type_identity};

CMETA_LOCAL const cmeta_type_identity orm_driver_config_type_identity =
    CMETA_TYPE_ID_ATOM_INIT("TurboDb.Config");
CMETA_LOCAL const cmeta_type_identity orm_driver_config_const_type_identity =
    CMETA_TYPE_ID_CONST_INIT(&orm_driver_config_type_identity);
CMETA_LOCAL const cmeta_type_desc orm_driver_config_const_cmeta_type = {
    "const orm_config_t", sizeof(orm_config_t), CMETA_ALIGNOF(orm_config_t),
    CMETA_T_OBJECT, NULL, NULL, &orm_driver_config_const_type_identity};
CMETA_LOCAL const cmeta_type_identity orm_driver_config_ptr_type_identity =
    CMETA_TYPE_ID_POINTER_INIT(&orm_driver_config_const_type_identity);
CMETA_LOCAL const cmeta_type_desc orm_driver_config_ptr_cmeta_type = {
    "const orm_config_t *", sizeof(const orm_config_t *),
    CMETA_ALIGNOF(const orm_config_t *), CMETA_T_POINTER,
    &orm_driver_config_const_cmeta_type, NULL,
    &orm_driver_config_ptr_type_identity};

CMETA_LOCAL const cmeta_type_identity orm_driver_limits_type_identity =
    CMETA_TYPE_ID_ATOM_INIT("TurboDb.DriverLimits");
CMETA_LOCAL const cmeta_type_identity orm_driver_limits_const_type_identity =
    CMETA_TYPE_ID_CONST_INIT(&orm_driver_limits_type_identity);
CMETA_LOCAL const cmeta_type_desc orm_driver_limits_const_cmeta_type = {
    "const orm_driver_limits_v1", sizeof(orm_driver_limits_v1),
    CMETA_ALIGNOF(orm_driver_limits_v1), CMETA_T_OBJECT, NULL, NULL,
    &orm_driver_limits_const_type_identity};
CMETA_LOCAL const cmeta_type_identity orm_driver_limits_ptr_type_identity =
    CMETA_TYPE_ID_POINTER_INIT(&orm_driver_limits_const_type_identity);
CMETA_LOCAL const cmeta_type_desc orm_driver_limits_ptr_cmeta_type = {
    "const orm_driver_limits_v1 *", sizeof(const orm_driver_limits_v1 *),
    CMETA_ALIGNOF(const orm_driver_limits_v1 *), CMETA_T_POINTER,
    &orm_driver_limits_const_cmeta_type, NULL,
    &orm_driver_limits_ptr_type_identity};

CMETA_LOCAL const cmeta_type_identity orm_driver_connection_type_identity =
    CMETA_TYPE_ID_ATOM_INIT("TurboDb.DriverConnection");
CMETA_LOCAL const cmeta_type_desc orm_driver_connection_cmeta_type = {
    "orm_driver_connection_v1", sizeof(orm_driver_connection_v1),
    CMETA_ALIGNOF(orm_driver_connection_v1), CMETA_T_OBJECT, NULL, NULL,
    &orm_driver_connection_type_identity};
CMETA_LOCAL const cmeta_type_identity orm_driver_connection_ptr_type_identity =
    CMETA_TYPE_ID_POINTER_INIT(&orm_driver_connection_type_identity);
CMETA_LOCAL const cmeta_type_desc orm_driver_connection_ptr_cmeta_type = {
    "orm_driver_connection_v1 *", sizeof(orm_driver_connection_v1 *),
    CMETA_ALIGNOF(orm_driver_connection_v1 *), CMETA_T_POINTER,
    &orm_driver_connection_cmeta_type, NULL,
    &orm_driver_connection_ptr_type_identity};

CMETA_LOCAL const cmeta_type_identity orm_driver_error_type_identity =
    CMETA_TYPE_ID_ATOM_INIT("TurboDb.Error");
CMETA_LOCAL const cmeta_type_desc orm_driver_error_cmeta_type = {
    "orm_error_t", sizeof(orm_error_t), CMETA_ALIGNOF(orm_error_t),
    CMETA_T_OBJECT, NULL, NULL, &orm_driver_error_type_identity};
CMETA_LOCAL const cmeta_type_identity orm_driver_error_ptr_type_identity =
    CMETA_TYPE_ID_POINTER_INIT(&orm_driver_error_type_identity);
CMETA_LOCAL const cmeta_type_desc orm_driver_error_ptr_cmeta_type = {
    "orm_error_t *", sizeof(orm_error_t *), CMETA_ALIGNOF(orm_error_t *),
    CMETA_T_POINTER, &orm_driver_error_cmeta_type, NULL,
    &orm_driver_error_ptr_type_identity};

CMETA_LOCAL const cmeta_type_identity
    orm_driver_storage_capabilities_type_identity =
        CMETA_TYPE_ID_ATOM_INIT("TurboDb.DriverStorageCapabilities");
CMETA_LOCAL const cmeta_type_identity
    orm_driver_storage_capabilities_const_type_identity =
        CMETA_TYPE_ID_CONST_INIT(&orm_driver_storage_capabilities_type_identity);
CMETA_LOCAL const cmeta_type_desc
    orm_driver_storage_capabilities_const_cmeta_type = {
        "const orm_driver_storage_capabilities_v1",
        sizeof(orm_driver_storage_capabilities_v1),
        CMETA_ALIGNOF(orm_driver_storage_capabilities_v1),
        CMETA_T_OBJECT, NULL, NULL,
        &orm_driver_storage_capabilities_const_type_identity};
CMETA_LOCAL const cmeta_type_identity
    orm_driver_storage_capabilities_ptr_type_identity =
        CMETA_TYPE_ID_POINTER_INIT(
            &orm_driver_storage_capabilities_const_type_identity);
CMETA_LOCAL const cmeta_type_desc
    orm_driver_storage_capabilities_ptr_cmeta_type = {
        "const orm_driver_storage_capabilities_v1 *",
        sizeof(const orm_driver_storage_capabilities_v1 *),
        CMETA_ALIGNOF(const orm_driver_storage_capabilities_v1 *),
        CMETA_T_POINTER,
        &orm_driver_storage_capabilities_const_cmeta_type, NULL,
        &orm_driver_storage_capabilities_ptr_type_identity};

/*
 * Minimal stable capability: create one native Driver connection.
 *
 * Connection/query/transaction/cursor semantics continue through the existing
 * typed operation tables.  They are deliberately not flattened into generic
 * Plugin Function exports.  F4 makes FunctionDesc + FunctionAbi part of the
 * same declaration as typed vtable dispatch.
 */
#define ORM_DRIVER_INTERFACE_METHODS(X, I)                                      \
  X(I, F4, orm_status_t, create, io,                                            \
    &orm_driver_status_cmeta_type, CMETA_ABI_SCALAR,                            \
    (const orm_config_t *, config, CMETA_PARAM_IN | CMETA_PARAM_BORROWED,       \
     &orm_driver_config_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER),              \
    (const orm_driver_limits_v1 *, limits,                                      \
     CMETA_PARAM_IN | CMETA_PARAM_BORROWED,                                    \
     &orm_driver_limits_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER),              \
    (orm_driver_connection_v1 *, out_connection,                               \
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED,                                   \
     &orm_driver_connection_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER),          \
    (orm_error_t *, error,                                                      \
     CMETA_PARAM_OUT | CMETA_PARAM_BORROWED | CMETA_PARAM_NULLABLE,             \
     &orm_driver_error_ptr_cmeta_type, CMETA_ABI_OBJECT_POINTER))                \
  X(I, F0, uint64_t, execution_models, value,                                   \
    &cmeta_type_uint64, CMETA_ABI_SCALAR)                                       \
  X(I, F0, const orm_driver_storage_capabilities_v1 *, storage_capabilities,      \
    value, &orm_driver_storage_capabilities_ptr_cmeta_type,                      \
    CMETA_ABI_OBJECT_POINTER)

CMETA_INTERFACE(TurboDb_Driver, ORM_DRIVER_INTERFACE_METHODS);

typedef TurboDb_Driver orm_driver_interface_v1;
typedef TurboDb_Driver_vtable orm_driver_interface_vtable_v1;

#ifdef __cplusplus
}
#endif

#endif /* ORM_DRIVER_INTERFACE_H */
