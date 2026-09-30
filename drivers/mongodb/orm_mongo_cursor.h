#ifndef ORM_MONGO_CURSOR_H
#define ORM_MONGO_CURSOR_H

#include "orm_row_publisher.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  ORM_MONGO_DRIVER_OPS_ABI_VERSION = 1u,
  ORM_MONGO_CURSOR_CONFIG_ABI_VERSION = 1u
};

typedef enum orm_mongo_value_kind {
  ORM_MONGO_VALUE_NULL = 0,
  ORM_MONGO_VALUE_BOOL,
  ORM_MONGO_VALUE_SINT,
  ORM_MONGO_VALUE_UINT,
  ORM_MONGO_VALUE_FLOAT,
  ORM_MONGO_VALUE_STRING,
  ORM_MONGO_VALUE_BYTES
} orm_mongo_value_kind;

typedef struct orm_mongo_slice {
  const unsigned char *data;
  size_t size;
} orm_mongo_slice;

typedef struct orm_mongo_value {
  orm_mongo_value_kind kind;
  union {
    bool boolean;
    int64_t sint;
    uint64_t uint;
    double floating;
    orm_mongo_slice slice;
  } data;
} orm_mongo_value;

#define ORM_MONGO_VALUE_INIT { ORM_MONGO_VALUE_NULL, {0} }

typedef enum orm_mongo_driver_step_kind {
  ORM_MONGO_DRIVER_DONE = 0,
  ORM_MONGO_DRIVER_ROW,
  ORM_MONGO_DRIVER_ERROR
} orm_mongo_driver_step_kind;

typedef struct orm_mongo_driver_step {
  orm_mongo_driver_step_kind kind;
  orm_status_t status;
  const char *message;
} orm_mongo_driver_step;

#define ORM_MONGO_DRIVER_STEP_INIT \
  { ORM_MONGO_DRIVER_DONE, ORM_STATUS_OK, NULL }

typedef struct orm_mongo_driver_ops {
  size_t struct_size;
  uint32_t abi_version;
  orm_mongo_driver_step (*next)(void *context, const void **document);
  orm_status_t (*find)(void *context, const void *document,
                       const unsigned char *path, size_t path_size,
                       orm_mongo_value *out);
  void (*destroy)(void *context);
} orm_mongo_driver_ops;

typedef struct orm_mongo_driver {
  const orm_mongo_driver_ops *ops;
  void *context;
} orm_mongo_driver;

typedef struct orm_mongo_field {
  orm_mongo_slice output_name;
  orm_mongo_slice source_path;
} orm_mongo_field;

typedef struct orm_mongo_cursor_config {
  size_t struct_size;
  uint32_t abi_version;
  uint64_t max_rows;
  uint64_t max_result_bytes;
  size_t max_field_name_bytes;
} orm_mongo_cursor_config;

#define ORM_MONGO_CURSOR_CONFIG_INIT(max_rows_, max_result_bytes_,           \
                                     max_field_name_bytes_)                  \
  { sizeof(orm_mongo_cursor_config), ORM_MONGO_CURSOR_CONFIG_ABI_VERSION,    \
    (max_rows_), (max_result_bytes_), (max_field_name_bytes_) }

/* Success moves and clears driver and copies every field name/path. */
orm_status_t orm_mongo_cursor_start(
    orm_row_cursor *out_cursor, orm_mongo_driver *driver,
    const orm_mongo_field *fields, size_t field_count,
    const orm_mongo_cursor_config *config, orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif
