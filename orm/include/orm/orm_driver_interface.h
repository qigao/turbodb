#ifndef ORM_DRIVER_INTERFACE_H
#define ORM_DRIVER_INTERFACE_H

#include "orm_driver_ops.h"

#include <cmeta/interface.h>

/*
 * Canonical reflected database-driver capability.
 *
 * This header defines database semantics only. It deliberately has no Plugin
 * loader/registry/lifecycle dependency. Salts::Plugin publishes this typed
 * interface, while TurboDB caches the {self,vtable} binding for hot-path use.
 */
CMETA_LOCAL const cmeta_type_identity orm_driver_interface_status_identity =
    CMETA_TYPE_ID_ATOM_INIT("TurboDb.orm_status");
CMETA_LOCAL const cmeta_type_desc orm_driver_interface_status_type = {
    "orm_status_t", sizeof(orm_status_t), CMETA_ALIGNOF(orm_status_t),
    CMETA_T_INTEGER, NULL, NULL, &orm_driver_interface_status_identity
};

CMETA_LOCAL const cmeta_type_identity orm_driver_interface_execution_models_identity =
    CMETA_TYPE_ID_ATOM_INIT("TurboDb.Driver.ExecutionModels");
CMETA_LOCAL const cmeta_type_desc orm_driver_interface_execution_models_type = {
    "TurboDb.Driver.ExecutionModels", sizeof(uint64_t), CMETA_ALIGNOF(uint64_t),
    CMETA_T_INTEGER, NULL, NULL,
    &orm_driver_interface_execution_models_identity
};

CMETA_LOCAL const cmeta_type_identity orm_driver_interface_config_identity =
    CMETA_TYPE_ID_ATOM_INIT("TurboDb.orm_config.v4");
CMETA_LOCAL const cmeta_type_identity orm_driver_interface_const_config_identity =
    CMETA_TYPE_ID_CONST_INIT(&orm_driver_interface_config_identity);
CMETA_LOCAL const cmeta_type_identity orm_driver_interface_config_ptr_identity =
    CMETA_TYPE_ID_POINTER_INIT(&orm_driver_interface_const_config_identity);
CMETA_LOCAL const cmeta_type_desc orm_driver_interface_config_type = {
    "orm_config_t", sizeof(orm_config_t), CMETA_ALIGNOF(orm_config_t),
    CMETA_T_OBJECT, NULL, NULL, &orm_driver_interface_config_identity
};
CMETA_LOCAL const cmeta_type_desc orm_driver_interface_const_config_type = {
    "const orm_config_t", sizeof(orm_config_t), CMETA_ALIGNOF(orm_config_t),
    CMETA_T_OBJECT, NULL, NULL, &orm_driver_interface_const_config_identity
};
CMETA_LOCAL const cmeta_type_desc orm_driver_interface_config_ptr_type = {
    "const orm_config_t *", sizeof(const orm_config_t *),
    CMETA_ALIGNOF(const orm_config_t *), CMETA_T_POINTER,
    &orm_driver_interface_const_config_type, NULL,
    &orm_driver_interface_config_ptr_identity
};

CMETA_LOCAL const cmeta_type_identity orm_driver_interface_limits_identity =
    CMETA_TYPE_ID_ATOM_INIT("TurboDb.Driver.Limits.v1");
CMETA_LOCAL const cmeta_type_identity orm_driver_interface_const_limits_identity =
    CMETA_TYPE_ID_CONST_INIT(&orm_driver_interface_limits_identity);
CMETA_LOCAL const cmeta_type_identity orm_driver_interface_limits_ptr_identity =
    CMETA_TYPE_ID_POINTER_INIT(&orm_driver_interface_const_limits_identity);
CMETA_LOCAL const cmeta_type_desc orm_driver_interface_limits_type = {
    "orm_driver_limits_v1", sizeof(orm_driver_limits_v1),
    CMETA_ALIGNOF(orm_driver_limits_v1), CMETA_T_OBJECT, NULL, NULL,
    &orm_driver_interface_limits_identity
};
CMETA_LOCAL const cmeta_type_desc orm_driver_interface_const_limits_type = {
    "const orm_driver_limits_v1", sizeof(orm_driver_limits_v1),
    CMETA_ALIGNOF(orm_driver_limits_v1), CMETA_T_OBJECT, NULL, NULL,
    &orm_driver_interface_const_limits_identity
};
CMETA_LOCAL const cmeta_type_desc orm_driver_interface_limits_ptr_type = {
    "const orm_driver_limits_v1 *", sizeof(const orm_driver_limits_v1 *),
    CMETA_ALIGNOF(const orm_driver_limits_v1 *), CMETA_T_POINTER,
    &orm_driver_interface_const_limits_type, NULL,
    &orm_driver_interface_limits_ptr_identity
};

CMETA_LOCAL const cmeta_type_identity orm_driver_interface_connection_identity =
    CMETA_TYPE_ID_ATOM_INIT("TurboDb.Driver.Connection.v1");
CMETA_LOCAL const cmeta_type_identity orm_driver_interface_connection_ptr_identity =
    CMETA_TYPE_ID_POINTER_INIT(&orm_driver_interface_connection_identity);
CMETA_LOCAL const cmeta_type_desc orm_driver_interface_connection_type = {
    "orm_driver_connection_v1", sizeof(orm_driver_connection_v1),
    CMETA_ALIGNOF(orm_driver_connection_v1), CMETA_T_OBJECT, NULL, NULL,
    &orm_driver_interface_connection_identity
};
CMETA_LOCAL const cmeta_type_desc orm_driver_interface_connection_ptr_type = {
    "orm_driver_connection_v1 *", sizeof(orm_driver_connection_v1 *),
    CMETA_ALIGNOF(orm_driver_connection_v1 *), CMETA_T_POINTER,
    &orm_driver_interface_connection_type, NULL,
    &orm_driver_interface_connection_ptr_identity
};

CMETA_LOCAL const cmeta_type_identity orm_driver_interface_error_identity =
    CMETA_TYPE_ID_ATOM_INIT("TurboDb.orm_error.v4");
CMETA_LOCAL const cmeta_type_identity orm_driver_interface_error_ptr_identity =
    CMETA_TYPE_ID_POINTER_INIT(&orm_driver_interface_error_identity);
CMETA_LOCAL const cmeta_type_desc orm_driver_interface_error_type = {
    "orm_error_t", sizeof(orm_error_t), CMETA_ALIGNOF(orm_error_t),
    CMETA_T_OBJECT, NULL, NULL, &orm_driver_interface_error_identity
};
CMETA_LOCAL const cmeta_type_desc orm_driver_interface_error_ptr_type = {
    "orm_error_t *", sizeof(orm_error_t *), CMETA_ALIGNOF(orm_error_t *),
    CMETA_T_POINTER, &orm_driver_interface_error_type, NULL,
    &orm_driver_interface_error_ptr_identity
};

#define TURBODB_DRIVER_METHODS(X, I) X(I,F0,uint64_t,execution_models,value,&orm_driver_interface_execution_models_type,CMETA_ABI_SCALAR) X(I,F4,orm_status_t,connect,io,&orm_driver_interface_status_type,CMETA_ABI_SCALAR,(const orm_config_t *,config,CMETA_PARAM_IN | CMETA_PARAM_BORROWED,&orm_driver_interface_config_ptr_type,CMETA_ABI_OBJECT_POINTER),(const orm_driver_limits_v1 *,limits,CMETA_PARAM_IN | CMETA_PARAM_BORROWED,&orm_driver_interface_limits_ptr_type,CMETA_ABI_OBJECT_POINTER),(orm_driver_connection_v1 *,out_connection,CMETA_PARAM_OUT,&orm_driver_interface_connection_ptr_type,CMETA_ABI_OBJECT_POINTER),(orm_error_t *,error,CMETA_PARAM_OUT | CMETA_PARAM_NULLABLE,&orm_driver_interface_error_ptr_type,CMETA_ABI_OBJECT_POINTER))

CMETA_INTERFACE(turbodb_driver, TURBODB_DRIVER_METHODS);

#endif
