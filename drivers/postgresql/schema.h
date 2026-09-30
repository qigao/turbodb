#ifndef TURBODB_DBTOOL_POSTGRESQL_H
#define TURBODB_DBTOOL_POSTGRESQL_H

#include "dbtool_schema_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

const dbtool_schema_driver_ops *dbtool_postgresql_schema_driver(void);

extern TurboDb_SchemaApply dbtool_postgresql_schema;

#ifdef __cplusplus
}
#endif

#endif
