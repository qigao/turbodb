#ifndef ORM_DRIVER_CONTRACT_H
#define ORM_DRIVER_CONTRACT_H

#include <orm.h>
#include <orm_driver_base.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * buffer_bytes is the caller-guaranteed readable span, not descriptor metadata.
 * Unaligned buffers are accepted. Only the fixed header is read; success does
 * not validate trailing fields. Every failure clears a non-NULL output.
 * buffer and out_struct_bytes must not overlap. No allocation or callbacks.
 */
orm_status_t ORM_DRIVER_CALL orm_driver_check_prefix(
    const void *buffer, uint32_t buffer_bytes, uint32_t expected_version,
    uint32_t required_bytes, uint32_t *out_struct_bytes);

/* Validate pointer/length consistency and limits without reading the payload. */
orm_status_t ORM_DRIVER_CALL orm_driver_check_bytes(orm_driver_bytes_v1 value,
                                                   uint64_t max_bytes);

#ifdef __cplusplus
}
#endif

#endif
