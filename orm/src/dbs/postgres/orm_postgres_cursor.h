#ifndef ORM_POSTGRES_CURSOR_H
#define ORM_POSTGRES_CURSOR_H

#include "orm_cbind_source.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ORM_POSTGRES_COMMAND_OPS_ABI_VERSION 1u
#define ORM_POSTGRES_RESULT_OPS_ABI_VERSION 2u
#define ORM_POSTGRES_CURSOR_CONFIG_ABI_VERSION 1u

typedef enum orm_postgres_result_status {
  ORM_POSTGRES_RESULT_SINGLE_ROW = 1,
  ORM_POSTGRES_RESULT_TUPLES_DONE = 2,
  ORM_POSTGRES_RESULT_COMMAND_DONE = 3,
  ORM_POSTGRES_RESULT_ERROR = 4
} orm_postgres_result_status;

typedef struct orm_postgres_query_request {
  const char *sql;
  int parameter_count;
  const uint32_t *parameter_types;
  const char *const *parameter_values;
  const int *parameter_lengths;
  const int *parameter_formats;
  int result_format;
} orm_postgres_query_request;

typedef struct orm_postgres_command_ops {
  size_t struct_size;
  uint32_t abi_version;
  int (*send_query)(void *context,
                    const orm_postgres_query_request *request);
  int (*enable_single_row)(void *context);
  void *(*next_result)(void *context);
  void (*release_result)(void *result);
  const char *(*connection_error)(void *context);
} orm_postgres_command_ops;

typedef struct orm_postgres_result_ops {
  size_t struct_size;
  uint32_t abi_version;
  orm_postgres_result_status (*status)(const void *result);
  int64_t (*rows)(const void *result);
  int64_t (*columns)(const void *result);
  const char *(*column_name)(const void *result, size_t column);
  uint32_t (*column_type)(const void *result, size_t column);
  int (*is_null)(const void *result, size_t row, size_t column);
  const char *(*value)(const void *result, size_t row, size_t column);
  int64_t (*length)(const void *result, size_t row, size_t column);
  const char *(*command_tuples)(const void *result);
  const char *(*error)(const void *result);
  const char *(*sqlstate)(const void *result);
} orm_postgres_result_ops;

typedef struct orm_postgres_driver {
  const orm_postgres_command_ops *command;
  const orm_postgres_result_ops *result;
  void *context;
} orm_postgres_driver;

typedef struct orm_postgres_cursor_config {
  size_t struct_size;
  uint32_t abi_version;
  size_t max_columns;
  uint64_t max_result_rows;
  size_t max_result_bytes;
  size_t *column_count;
  uint64_t *affected_rows;
  orm_error_t *runtime_error;
} orm_postgres_cursor_config;

#define ORM_POSTGRES_CURSOR_CONFIG_INIT(max_columns_, max_result_rows_,     \
                                        max_result_bytes_, column_count_,   \
                                        affected_rows_, runtime_error_)     \
  {                                                                        \
    sizeof(orm_postgres_cursor_config),                                    \
        ORM_POSTGRES_CURSOR_CONFIG_ABI_VERSION, (max_columns_),             \
        (max_result_rows_), (max_result_bytes_), (column_count_),           \
        (affected_rows_),                                                   \
        (runtime_error_)                                                    \
  }

/*
 * Starts one query and moves it into out_cursor on success. The driver and
 * its context are borrowed until cursor destruction. Request pointers are
 * borrowed only for this call. A failed start leaves out_cursor empty.
 */
orm_status_t orm_postgres_cursor_start(
    orm_row_cursor *out_cursor, const orm_postgres_driver *driver,
    const orm_postgres_query_request *request,
    const orm_postgres_cursor_config *config, orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif
