#ifndef TURBODB_DBTOOL_POSTGRESQL_H
#define TURBODB_DBTOOL_POSTGRESQL_H

#include "dbtool_schema_driver.h"

#include <libpq-fe.h>

#ifdef __cplusplus
extern "C" {
#endif

const dbtool_schema_driver_ops *dbtool_postgresql_schema_driver(void);
PGconn *dbtool_postgresql_native_connection(void *context);
dbtool_status dbtool_postgresql_error(dbtool_status status, const char *stage,
                                      int native_code,
                                      const char *operation,
                                      const char *detail,
                                      dbtool_error *error);

#ifdef __cplusplus
}
#endif

#endif
