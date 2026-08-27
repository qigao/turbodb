#ifndef ORM_TIDESDB_ROW_H
#define ORM_TIDESDB_ROW_H

#include "orm_internal.h"

typedef struct orm_tidesdb_cell {
  orm_value_kind_t kind;
  bool is_null;
  tstr bytes;
} orm_tidesdb_cell;

typedef struct orm_tidesdb_field {
  tstr name;
  orm_tidesdb_cell value;
} orm_tidesdb_field;

typedef struct orm_tidesdb_row {
  vec_t fields;
} orm_tidesdb_row;

orm_status_t orm_tidesdb_row_init(orm_tidesdb_row *row, size_t max_fields,
                                  orm_error_t *error);
void orm_tidesdb_row_destroy(orm_tidesdb_row *row);

orm_status_t orm_tidesdb_cell_from_value(orm_tidesdb_cell *out_cell,
                                         const orm_owned_value *value,
                                         orm_error_t *error);
orm_status_t orm_tidesdb_row_set(orm_tidesdb_row *row, vstr name,
                                 const orm_tidesdb_cell *cell,
                                 orm_error_t *error);
const orm_tidesdb_cell *orm_tidesdb_row_find(const orm_tidesdb_row *row,
                                             vstr name);

orm_status_t orm_tidesdb_row_encode(const orm_tidesdb_row *row,
                                    size_t max_bytes, tstr *out_bytes,
                                    orm_error_t *error);
orm_status_t orm_tidesdb_row_decode(const unsigned char *data, size_t size,
                                    size_t max_bytes, size_t max_fields,
                                    orm_tidesdb_row *out_row,
                                    orm_error_t *error);

orm_status_t orm_tidesdb_row_matches(const orm_tidesdb_row *row,
                                     const orm_query_plan *plan,
                                     bool *out_matches,
                                     orm_error_t *error);
orm_status_t orm_tidesdb_row_project(const orm_tidesdb_row *row,
                                     const orm_query_plan *plan,
                                     const orm_limits *limits,
                                     orm_tidesdb_row *out_row,
                                     orm_error_t *error);

#endif
