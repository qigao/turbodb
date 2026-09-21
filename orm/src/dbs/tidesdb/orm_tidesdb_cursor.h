#ifndef ORM_TIDESDB_CURSOR_H
#define ORM_TIDESDB_CURSOR_H

#include "orm_row_publisher.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  ORM_TIDESDB_DRIVER_OPS_ABI_VERSION = 1u,
  ORM_TIDESDB_CURSOR_CONFIG_ABI_VERSION = 1u
};

typedef enum orm_tidesdb_driver_step_kind {
  ORM_TIDESDB_DRIVER_DONE = 0,
  ORM_TIDESDB_DRIVER_ROW,
  ORM_TIDESDB_DRIVER_ERROR
} orm_tidesdb_driver_step_kind;

typedef struct orm_tidesdb_driver_step {
  orm_tidesdb_driver_step_kind kind;
  orm_status_t status;
  const char *message;
} orm_tidesdb_driver_step;

#define ORM_TIDESDB_DRIVER_STEP_INIT \
  { ORM_TIDESDB_DRIVER_DONE, ORM_STATUS_OK, NULL }

typedef struct orm_tidesdb_driver_ops {
  size_t struct_size;
  uint32_t abi_version;
  orm_tidesdb_driver_step (*next)(void *context,
                                  const unsigned char **row,
                                  size_t *row_size);
  void (*release_row)(void *context, const unsigned char *row);
  void (*destroy)(void *context);
} orm_tidesdb_driver_ops;

typedef struct orm_tidesdb_driver {
  const orm_tidesdb_driver_ops *ops;
  void *context;
} orm_tidesdb_driver;

typedef struct orm_tidesdb_cursor_config {
  size_t struct_size;
  uint32_t abi_version;
  size_t max_rows;
  size_t max_row_bytes;
  size_t max_fields;
} orm_tidesdb_cursor_config;

#define ORM_TIDESDB_CURSOR_CONFIG_INIT(max_rows_, max_row_bytes_, max_fields_) \
  { sizeof(orm_tidesdb_cursor_config),                                      \
    ORM_TIDESDB_CURSOR_CONFIG_ABI_VERSION, (max_rows_), (max_row_bytes_),    \
    (max_fields_) }

/* Success moves and clears driver. Each row remains borrowed until next(). */
orm_status_t orm_tidesdb_cursor_start(
    orm_row_cursor *out_cursor, orm_tidesdb_driver *driver,
    const orm_tidesdb_cursor_config *config, orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif
