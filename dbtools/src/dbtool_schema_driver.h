#ifndef TURBODB_DBTOOL_SCHEMA_DRIVER_H
#define TURBODB_DBTOOL_SCHEMA_DRIVER_H

#include "dbtool_error.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DBTOOL_SCHEMA_DRIVER_ABI_VERSION 1u

typedef enum dbtool_driver_kind {
  DBTOOL_DRIVER_SQLITE = 1,
  DBTOOL_DRIVER_POSTGRESQL = 2
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

#ifdef __cplusplus
}
#endif

#endif
