#ifndef TURBODB_DBTOOL_SQLITE_H
#define TURBODB_DBTOOL_SQLITE_H

#include "dbtool_schema_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

const dbtool_schema_driver_ops *dbtool_sqlite_schema_driver(void);

#ifdef __cplusplus
}
#endif

#endif
