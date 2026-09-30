#ifndef TURBODB_DBTOOL_SCHEMA_DRIVER_H
#define TURBODB_DBTOOL_SCHEMA_DRIVER_H

#include "dbtool_error.h"
#include <cmeta/interface.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBTOOL_SCHEMA_DRIVER_ABI_VERSION 1u
#define DBTOOL_SCHEMA_EXPORT_ID "schema.apply"
#define DBTOOL_SCHEMA_CONTRACT_ID "TurboDb.SchemaApply"
#define DBTOOL_SCHEMA_CONTRACT_VERSION UINT32_C(1)

typedef enum dbtool_driver_kind {
  DBTOOL_DRIVER_SQLITE = 1,
  DBTOOL_DRIVER_POSTGRESQL = 2,
  DBTOOL_DRIVER_MYSQL = 3
} dbtool_driver_kind;

typedef struct dbtool_connection_config {
  const char *database;
  const char *conninfo;
  uint32_t busy_timeout_ms;
} dbtool_connection_config;

typedef struct dbtool_apply_result {
  uint64_t statements;
} dbtool_apply_result;

#define DBTOOL_APPLY_RESULT_INIT                                              \
  { 0u }

typedef struct dbtool_schema_driver_ops {
  size_t struct_size;
  uint32_t abi_version;
  dbtool_status (*open)(void **out_context,
                        const dbtool_connection_config *config,
                        dbtool_error *error);
  /* sql is borrowed for this call and sql[sql_size] is guaranteed to be NUL. */
  dbtool_status (*apply)(void *context, const char *sql, size_t sql_size,
                         dbtool_apply_result *result, dbtool_error *error);
  void (*close)(void *context);
} dbtool_schema_driver_ops;

/* The module owns the operations table, contexts and their allocations. The
 * host must hold its Plugin lease from operations() through the final close().
 * All calls are synchronous and single-threaded per context. Input strings are
 * borrowed for the call only; error/result storage belongs to the caller.
 * apply executes the whole script with native transaction semantics: it does
 * not insert BEGIN, split statements, retry or undo earlier committed work.
 * Failed open publishes no context. close consumes a successful open exactly
 * once and rolls back an unfinished transaction through the native connection.
 * Contract version 1 freezes these DTO/operation layouts and status values. */
CMETA_LOCAL const cmeta_type_identity dbtool_schema_ops_identity =
    CMETA_TYPE_ID_ATOM_INIT("TurboDb.SchemaApply.Operations.v1");
CMETA_LOCAL const cmeta_type_identity dbtool_schema_ops_const_identity =
    CMETA_TYPE_ID_CONST_INIT(&dbtool_schema_ops_identity);
CMETA_LOCAL const cmeta_type_desc dbtool_schema_ops_const_type = {
    "const dbtool_schema_driver_ops", sizeof(dbtool_schema_driver_ops),
    CMETA_ALIGNOF(dbtool_schema_driver_ops), CMETA_T_OBJECT, NULL, NULL,
    &dbtool_schema_ops_const_identity};
CMETA_LOCAL const cmeta_type_identity dbtool_schema_ops_ptr_identity =
    CMETA_TYPE_ID_POINTER_INIT(&dbtool_schema_ops_const_identity);
CMETA_LOCAL const cmeta_type_desc dbtool_schema_ops_ptr_type = {
    "const dbtool_schema_driver_ops *", sizeof(const dbtool_schema_driver_ops *),
    CMETA_ALIGNOF(const dbtool_schema_driver_ops *), CMETA_T_POINTER,
    &dbtool_schema_ops_const_type, NULL, &dbtool_schema_ops_ptr_identity};

#define DBTOOL_SCHEMA_METHODS(X, I) \
  X(I, F0, const dbtool_schema_driver_ops *, operations, value, \
    &dbtool_schema_ops_ptr_type, CMETA_ABI_OBJECT_POINTER)

CMETA_INTERFACE(TurboDb_SchemaApply, DBTOOL_SCHEMA_METHODS);

static inline const dbtool_schema_driver_ops *dbtool_schema_operations(void *self) {
  return (const dbtool_schema_driver_ops *)self;
}

#ifdef __cplusplus
}
#endif

#endif
