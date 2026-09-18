#ifndef ORM_DRIVER_ABI_H
#define ORM_DRIVER_ABI_H

#include "orm_driver_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

struct orm_driver_api_v1 {
  orm_driver_header_v1 header;
  uint8_t bundle_id[ORM_DRIVER_BUNDLE_ID_BYTES];
  orm_driver_bytes_v1 canonical_id;
  const orm_driver_bytes_v1 *aliases;
  uint32_t alias_count;
  uint32_t reserved;
  uint64_t capabilities;
  uint64_t execution_models;
  orm_driver_table_v1 module_ops;
  orm_driver_create_fn create_connection;
  orm_driver_table_v1 connection_ops;
};
struct orm_driver_host_v1 {
  orm_driver_header_v1 header;
  uint8_t bundle_id[ORM_DRIVER_BUNDLE_ID_BYTES];
  orm_driver_table_v1 plan_metadata;
  orm_driver_table_v1 plan_values;
  orm_driver_table_v1 lifetime;
  orm_driver_table_v1 execution;
};

/* Caller guarantees readable, immutable buffers, including nested pointers and
 * alias spans, until return. Validation cannot probe arbitrary native pointers.
 * Unknown trailing bytes are ignored, not retained. Validators allocate nothing
 * and never invoke descriptor callbacks (including on failure). Optional error
 * points to a complete writable orm_error_t, disjoint from every input;
 * it is initialized on return. ID/alias validation has a 65536-byte metadata
 * budget in addition to max_aliases (array, canonical ID and alias bytes).
 * These candidate SDK helpers do not load/register drivers or acquire leases.
 * Bootstrap export and its executable fixture are introduced in Task 3. */
orm_status_t ORM_DRIVER_CALL orm_driver_validate_host_v1(
    const void *host, uint32_t bytes,
    const uint8_t expected_bundle[ORM_DRIVER_BUNDLE_ID_BYTES], orm_error_t *error);
orm_status_t ORM_DRIVER_CALL orm_driver_validate_api_v1(
    const void *api, uint32_t bytes,
    const uint8_t expected_bundle[ORM_DRIVER_BUNDLE_ID_BYTES],
    orm_driver_bytes_v1 expected_id, uint32_t max_aliases, orm_error_t *error);
orm_status_t ORM_DRIVER_CALL orm_driver_validate_connection_v1(
    const void *object, uint32_t bytes, uint64_t capabilities, orm_error_t *error);
orm_status_t ORM_DRIVER_CALL orm_driver_validate_transaction_v1(
    const void *object, uint32_t bytes, uint64_t capabilities, orm_error_t *error);
orm_status_t ORM_DRIVER_CALL orm_driver_validate_cursor_v1(
    const void *object, uint32_t bytes, uint64_t capabilities, orm_error_t *error);

#ifdef __cplusplus
}
#endif
#endif
