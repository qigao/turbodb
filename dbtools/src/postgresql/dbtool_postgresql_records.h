#ifndef TURBODB_DBTOOL_POSTGRESQL_RECORDS_H
#define TURBODB_DBTOOL_POSTGRESQL_RECORDS_H

#include "data/dbtool_record.h"

const dbtool_record_sink_ops *dbtool_postgresql_record_sink(void);
const dbtool_record_source_ops *dbtool_postgresql_record_source(void);

#endif
