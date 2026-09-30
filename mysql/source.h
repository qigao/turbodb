#ifndef TURBODB_MYSQL_SOURCE_H
#define TURBODB_MYSQL_SOURCE_H

#include "session.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { MYSQL_CURSOR_SOURCE_OPS_ABI_VERSION = 1u };

/* Views returned by next expire at the next next/cancel/destroy call.
 * Source operations are single-thread-owned. Destroy exactly once. */
typedef enum mysql_cursor_source_step_kind_t {
  MYSQL_CURSOR_SOURCE_DONE = 0,
  MYSQL_CURSOR_SOURCE_ROW,
  MYSQL_CURSOR_SOURCE_ERROR
} mysql_cursor_source_step_kind_t;

typedef struct mysql_cursor_source_step_t {
  mysql_cursor_source_step_kind_t kind;
  mysql_session_status_t status;
  const char *message;
  const uint8_t *row;
  size_t row_size;
} mysql_cursor_source_step_t;

#define MYSQL_CURSOR_SOURCE_STEP_INIT   { MYSQL_CURSOR_SOURCE_DONE, MYSQL_SESSION_OK, NULL, NULL, 0u }

typedef struct mysql_cursor_source_ops_t {
  size_t struct_size;
  uint32_t abi_version;
  mysql_cursor_source_step_t (*next)(void *context);
  void (*cancel)(void *context);
  void (*destroy)(void *context);
} mysql_cursor_source_ops_t;

typedef struct mysql_cursor_source_t {
  const mysql_cursor_source_ops_t *ops;
  void *context;
} mysql_cursor_source_t;

#ifdef __cplusplus
}
#endif

#endif
