#ifndef ORM_ROW_PUBLISHER_H
#define ORM_ROW_PUBLISHER_H

#include <cserde/cserde.h>
#include <cflow/cflow.h>
#include <orm.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { ORM_ROW_CURSOR_OPS_ABI_VERSION = 3u };
enum { ORM_ROW_PUBLISHER_CONFIG_ABI_VERSION = 1u };

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
typedef orm_status_t (*orm_row_cursor_configure_shape_fn)(
    void *context, const cmeta_data_desc *row_shape, orm_error_t *error);
typedef orm_status_t (*orm_row_cursor_column_count_fn)(void *context,
                                                       uint64_t *out_count);

typedef struct orm_row_cursor_ops {
  size_t struct_size;
  uint32_t abi_version;
  const char *name;
  orm_row_cursor_next_fn next;
  orm_row_cursor_cancel_fn cancel;
  orm_row_cursor_destroy_fn destroy;
  /* Optional backend-specific normalization of native scalar tokens. */
  orm_row_cursor_configure_shape_fn configure_shape;
  /* Optional schema metadata, valid through cursor disposal. */
  orm_row_cursor_column_count_fn column_count;
} orm_row_cursor_ops;

typedef struct orm_row_cursor {
  const orm_row_cursor_ops *ops;
  void *context;
  /* Zero disables CFlow WAIT timeout wrapping for this backend cursor. */
  uint64_t wait_timeout_ns;
#if defined(ORM_NATIVE_OWNER_CANDIDATE)
  /* Optional idempotent cancellation with an observable result. It finishes
   * native draining before returning; error is copied into caller storage.
   * A successful cancel does not release any owner or prove query completion. */
  orm_status_t (*cancel_checked)(void *, orm_error_t *);
  void *owner;
  void (*release_owner)(void *);
  /* Host-only hooks borrow owner for the cursor lifetime. They do not touch
   * native I/O or consume its hold. A terminal native connection failure must
   * block sibling execution before the next Publisher resume. */
  orm_status_t (*owner_status)(void *, orm_error_t *);
  void (*report_owner_error)(void *, orm_status_t);
  void *transaction_owner;
  void (*release_transaction_owner)(void *);
#endif
} orm_row_cursor;

/*
 * Backend open_cursor contract: the caller supplies a zero cursor. Success
 * returns a complete ABI-compatible cursor; failure leaves it zero and the
 * backend releases every partial resource. A backend must never publish an
 * opaque context without a valid destroy operation because no upper layer can
 * recover that ownership information.
 */
int orm_row_cursor_valid(const orm_row_cursor *cursor);
void orm_row_cursor_dispose(orm_row_cursor *cursor);

typedef struct orm_row_publisher_config {
  size_t struct_size;
  uint32_t abi_version;
  const cmeta_data_desc *row_shape;
  size_t scratch_bytes;
  size_t max_depth;
  size_t max_container_items;
  size_t max_buffer_bytes;
} orm_row_publisher_config;

#define ORM_ROW_PUBLISHER_CONFIG_INIT(row_shape_, scratch_bytes_, max_depth_, \
                                     max_container_items_, max_buffer_bytes_) \
  { sizeof(orm_row_publisher_config), ORM_ROW_PUBLISHER_CONFIG_ABI_VERSION,     \
    (row_shape_), (scratch_bytes_), (max_depth_), (max_container_items_),    \
    (max_buffer_bytes_) }

/*
 * On success, moves cursor into out_publisher and clears cursor. On failure,
 * out_publisher stays zero and cursor remains caller-owned. The Publisher owns its
 * DataBind native workspace and destroys the moved cursor exactly once.
 *
 * next() initializes out_row only for ROW/ROW_AND_DONE. The reader, its
 * context, and transient token slices remain valid until the enclosing Publisher
 * resume() returns; decoding happens synchronously after next() returns. A
 * WAIT result must contain a valid waitable whose backing state remains live
 * until CFlow arms/cancels it or the cursor is cancelled. row_shape and all
 * metadata reachable from it are borrowed through Publisher destruction.
 */
orm_status_t orm_row_publisher_init(cflow_publisher *out_publisher,
                                   orm_row_cursor *cursor,
                                   const orm_row_publisher_config *config,
                                   orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif
