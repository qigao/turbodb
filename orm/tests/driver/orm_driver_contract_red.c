/* Test-only RED link fixture for #30 Task 1. Removed by the GREEN commit.
 * No production implementation exists at this revision. Returning UNSUPPORTED
 * makes the behavioral assertions fail without faking any dependency headers.
 */
#include "orm_driver_contract.h"

orm_status_t ORM_DRIVER_CALL orm_driver_check_prefix(
    const void *buffer, uint32_t buffer_bytes, uint32_t expected_version,
    uint32_t required_bytes, uint32_t *out_struct_bytes) {
  (void)buffer;
  (void)buffer_bytes;
  (void)expected_version;
  (void)required_bytes;
  (void)out_struct_bytes;
  return ORM_STATUS_UNSUPPORTED;
}

orm_status_t ORM_DRIVER_CALL orm_driver_check_bytes(orm_driver_bytes_v1 value,
                                                   uint64_t max_bytes) {
  (void)value;
  (void)max_bytes;
  return ORM_STATUS_UNSUPPORTED;
}
