#ifndef ORM_CBIND_SOURCE_H
#define ORM_CBIND_SOURCE_H

#include <cbind/cbind.h>
#include <cflow/cflow.h>
#include <orm.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { ORM_ROW_CURSOR_OPS_ABI_VERSION = 1u };
enum { ORM_CBIND_SOURCE_CONFIG_ABI_VERSION = 1u };

typedef enum orm_row_cursor_step_kind {
  ORM_ROW_CURSOR_ROW = 0,
  ORM_ROW_CURSOR_ROW_AND_DONE,
  ORM_ROW_CURSOR_WAIT,
  ORM_ROW_CURSOR_DONE,
  ORM_ROW_CURSOR_ERROR
} orm_row_cursor_step_kind;

typedef struct orm_row_cursor_step {
  orm_row_cursor_step_kind kind;
  /* Valid only for WAIT; owned by the cursor until armed/cancelled by CFlow. */
  cflow_waitable waitable;
  orm_status_t status;
  /* Borrowed until next(), cancel(), or destroy(); the adapter copies errors. */
  const char *message;
} orm_row_cursor_step;

#define ORM_ROW_CURSOR_STEP_INIT \
  { ORM_ROW_CURSOR_DONE, {0}, ORM_STATUS_OK, NULL }

typedef orm_row_cursor_step (*orm_row_cursor_next_fn)(void *context,
                                                       cserde_reader *out_row);
typedef void (*orm_row_cursor_cancel_fn)(void *context);
typedef void (*orm_row_cursor_destroy_fn)(void *context);

typedef struct orm_row_cursor_ops {
  size_t struct_size;
  uint32_t abi_version;
  const char *name;
  orm_row_cursor_next_fn next;
  orm_row_cursor_cancel_fn cancel;
  orm_row_cursor_destroy_fn destroy;
} orm_row_cursor_ops;

typedef struct orm_row_cursor {
  const orm_row_cursor_ops *ops;
  void *context;
} orm_row_cursor;

typedef struct orm_cbind_source_config {
  size_t struct_size;
  uint32_t abi_version;
  const cmeta_data_desc *row_shape;
  size_t scratch_bytes;
  size_t max_depth;
  size_t max_container_items;
  size_t max_buffer_bytes;
} orm_cbind_source_config;

#define ORM_CBIND_SOURCE_CONFIG_INIT(row_shape_, scratch_bytes_, max_depth_, \
                                     max_container_items_, max_buffer_bytes_) \
  { sizeof(orm_cbind_source_config), ORM_CBIND_SOURCE_CONFIG_ABI_VERSION,     \
    (row_shape_), (scratch_bytes_), (max_depth_), (max_container_items_),    \
    (max_buffer_bytes_) }

/*
 * On success, moves cursor into out_source and clears cursor. On failure,
 * out_source stays zero and cursor remains caller-owned. The Source owns its
 * CBind scratch storage and destroys the moved cursor exactly once.
 *
 * next() initializes out_row only for ROW/ROW_AND_DONE. The reader, its
 * context, and transient token slices remain valid until the enclosing Source
 * resume() returns; decoding happens synchronously after next() returns. A
 * WAIT result must contain a valid waitable whose backing state remains live
 * until CFlow arms/cancels it or the cursor is cancelled. row_shape and all
 * metadata reachable from it are borrowed through Source destruction.
 */
orm_status_t orm_cbind_source_init(cflow_source *out_source,
                                   orm_row_cursor *cursor,
                                   const orm_cbind_source_config *config,
                                   orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif
