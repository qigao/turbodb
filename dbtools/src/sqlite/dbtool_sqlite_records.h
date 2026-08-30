#ifndef TURBODB_DBTOOL_SQLITE_RECORDS_H
#define TURBODB_DBTOOL_SQLITE_RECORDS_H

#include "data/dbtool_record.h"

#ifdef __cplusplus
extern "C" {
#endif

const dbtool_record_sink_ops *dbtool_sqlite_record_sink(void);
const dbtool_record_source_ops *dbtool_sqlite_record_source(void);

#ifdef __cplusplus
}
#endif

#endif /* TURBODB_DBTOOL_SQLITE_RECORDS_H */
