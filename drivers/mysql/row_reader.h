#ifndef TURBODB_MYSQL_ROW_READER_H
#define TURBODB_MYSQL_ROW_READER_H

#include "wire/row.h"

#include <cserde/reader.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mysql_row_reader_phase_t {
  MYSQL_ROW_READER_MAP_BEGIN = 0,
  MYSQL_ROW_READER_KEY,
  MYSQL_ROW_READER_VALUE,
  MYSQL_ROW_READER_MAP_END,
  MYSQL_ROW_READER_DONE
} mysql_row_reader_phase_t;

typedef struct mysql_row_reader_state {
  const mysql_column_definition_t *columns;
  const mysql_binary_value_t *values;
  size_t column_count;
  size_t column;
  mysql_row_reader_phase_t phase;
  unsigned char temporal[64];
  size_t temporal_size;
} mysql_row_reader_state;

cserde_status mysql_row_reader_init(
    mysql_row_reader_state *state,
    const mysql_column_definition_t *columns,
    const mysql_binary_value_t *values,
    size_t column_count,
    cserde_reader *out_reader);

#ifdef __cplusplus
}
#endif

#endif
