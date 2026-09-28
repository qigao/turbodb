#ifndef TURBODB_ORM_MYSQL_ROW_READER_H
#define TURBODB_ORM_MYSQL_ROW_READER_H

#include "wire/row.h"

#include <cserde/reader.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mysql_row_reader_state mysql_row_reader_state;

size_t mysql_row_reader_state_size(void);

int mysql_row_reader_init(
    void *state_storage, size_t state_storage_size,
    const mysql_column_definition_t *columns,
    const mysql_binary_value_t *values,
    size_t column_count,
    cserde_reader *out_reader);

#ifdef __cplusplus
}
#endif

#endif
