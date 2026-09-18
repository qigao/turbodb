#include "orm_driver_contract.h"

#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(orm_driver_header_v1) == ORM_DRIVER_HEADER_BYTES,
               "driver header must remain an eight-byte prefix");
_Static_assert(offsetof(orm_driver_header_v1, struct_size) == 0u,
               "driver size must be the first field");
_Static_assert(offsetof(orm_driver_header_v1, abi_version) == 4u,
               "driver version must be the second field");

orm_status_t ORM_DRIVER_CALL orm_driver_check_prefix(
    const void *buffer, uint32_t buffer_bytes, uint32_t expected_version,
    uint32_t required_bytes, uint32_t *out_struct_bytes) {
  orm_driver_header_v1 header;
  if (out_struct_bytes == NULL)
    return ORM_STATUS_INVALID_ARGUMENT;
  *out_struct_bytes = 0u;
  if (buffer == NULL || required_bytes < ORM_DRIVER_HEADER_BYTES)
    return ORM_STATUS_INVALID_ARGUMENT;
  if (buffer_bytes < ORM_DRIVER_HEADER_BYTES)
    return ORM_STATUS_ABI_MISMATCH;

  /* The supplied span, not the untrusted size field, admits this read. */
  memcpy(&header, buffer, sizeof(header));
  if (header.abi_version != expected_version ||
      header.struct_size < required_bytes || header.struct_size > buffer_bytes)
    return ORM_STATUS_ABI_MISMATCH;
  if (header.struct_size > ORM_DRIVER_DESCRIPTOR_MAX_BYTES)
    return ORM_STATUS_LIMIT_EXCEEDED;

  *out_struct_bytes = header.struct_size;
  return ORM_STATUS_OK;
}

orm_status_t ORM_DRIVER_CALL orm_driver_check_bytes(orm_driver_bytes_v1 value,
                                                   uint64_t max_bytes) {
  if (value.data == NULL && value.size != 0u)
    return ORM_STATUS_INVALID_ARGUMENT;
  if (value.size > max_bytes || value.size > (uint64_t)SIZE_MAX)
    return ORM_STATUS_LIMIT_EXCEEDED;
  return ORM_STATUS_OK;
}
