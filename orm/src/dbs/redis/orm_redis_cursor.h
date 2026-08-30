#ifndef ORM_REDIS_CURSOR_H
#define ORM_REDIS_CURSOR_H

#include "orm_cbind_publisher.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  ORM_REDIS_REPLY_OPS_ABI_VERSION = 1u,
  ORM_REDIS_ROW_DRIVER_OPS_ABI_VERSION = 2u,
  ORM_REDIS_CURSOR_CONFIG_ABI_VERSION = 2u
};

typedef enum orm_redis_reply_kind {
  ORM_REDIS_REPLY_INVALID = -1,
  ORM_REDIS_REPLY_NULL = 0,
  ORM_REDIS_REPLY_INTEGER,
  ORM_REDIS_REPLY_STRING,
  ORM_REDIS_REPLY_ARRAY
} orm_redis_reply_kind;

typedef struct orm_redis_reply_ops {
  size_t struct_size;
  uint32_t abi_version;
  orm_redis_reply_kind (*kind)(void *context, const void *reply);
  int64_t (*integer)(void *context, const void *reply);
  const unsigned char *(*bytes)(void *context, const void *reply,
                                size_t *size);
  size_t (*child_count)(void *context, const void *reply);
  const void *(*child)(void *context, const void *reply, size_t index);
} orm_redis_reply_ops;

typedef enum orm_redis_driver_step_kind {
  ORM_REDIS_DRIVER_DONE = 0,
  ORM_REDIS_DRIVER_ROW,
  ORM_REDIS_DRIVER_WAIT,
  ORM_REDIS_DRIVER_ERROR
} orm_redis_driver_step_kind;

typedef struct orm_redis_driver_step {
  orm_redis_driver_step_kind kind;
  /* Valid only for WAIT; borrowed until the cursor is resumed or cancelled. */
  cflow_waitable waitable;
  orm_status_t status;
  const char *message;
} orm_redis_driver_step;

#define ORM_REDIS_DRIVER_STEP_INIT \
  { ORM_REDIS_DRIVER_DONE, {0}, ORM_STATUS_OK, NULL }

typedef struct orm_redis_row_driver_ops {
  size_t struct_size;
  uint32_t abi_version;
  orm_redis_driver_step (*next)(void *context, void **row);
  void (*cancel)(void *context);
  void (*release_row)(void *context, void *row);
  void (*destroy)(void *context);
} orm_redis_row_driver_ops;

typedef struct orm_redis_row_driver {
  const orm_redis_row_driver_ops *ops;
  const orm_redis_reply_ops *reply_ops;
  void *context;
} orm_redis_row_driver;

typedef struct orm_redis_field_view {
  const unsigned char *data;
  size_t size;
} orm_redis_field_view;

typedef struct orm_redis_cursor_config {
  size_t struct_size;
  uint32_t abi_version;
  size_t max_rows;
  size_t max_field_name_bytes;
  uint64_t wait_timeout_ns;
} orm_redis_cursor_config;

#define ORM_REDIS_CURSOR_CONFIG_INIT(max_rows_, max_field_name_bytes_,       \
                                     wait_timeout_ns_)                       \
  { sizeof(orm_redis_cursor_config), ORM_REDIS_CURSOR_CONFIG_ABI_VERSION,    \
    (max_rows_), (max_field_name_bytes_), (wait_timeout_ns_) }

/* Success moves and clears driver. A row is borrowed until the next resume. */
orm_status_t orm_redis_cursor_start(
    orm_row_cursor *out_cursor, orm_redis_row_driver *driver,
    const orm_redis_field_view *fields, size_t field_count,
    const orm_redis_cursor_config *config, orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif
