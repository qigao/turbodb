#ifndef TURBODB_DBTOOL_SQLITE_H
#define TURBODB_DBTOOL_SQLITE_H

#include "dbtool_schema_driver.h"

#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

const dbtool_schema_driver_ops *dbtool_sqlite_schema_driver(void);

/** Internal connection view shared by standalone SQLite driver components. */
sqlite3 *dbtool_sqlite_native_database(void *context);

dbtool_status dbtool_sqlite_error(sqlite3 *database, dbtool_status status, const char *stage,
                                  const char *operation, const char *detail, dbtool_error *error);

#ifdef __cplusplus
}
#endif

#endif
