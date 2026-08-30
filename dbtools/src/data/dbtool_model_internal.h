#ifndef TURBODB_DBTOOL_MODEL_INTERNAL_H
#define TURBODB_DBTOOL_MODEL_INTERNAL_H

#include <turbodb/dbtool_model.h>

int dbtool_column_storage_matches(const dbtool_column_v1 *column);
int dbtool_table_metadata_valid(const dbtool_table_v1 *table,
                                size_t table_index);
const dbtool_table_v1 *dbtool_model_table_valid(
    const dbtool_model_v1 *model, size_t table_index);

#endif
