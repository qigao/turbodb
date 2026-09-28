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


/* Runtime-only validation for typed objects returned by an admitted
 * TurboDb.Driver Interface. These checks do not load modules, discover
 * exports, or participate in Plugin admission. */
orm_status_t ORM_DRIVER_CALL orm_driver_validate_connection_v1(
    const void *object, uint32_t bytes, uint64_t capabilities,
    orm_error_t *error);
orm_status_t ORM_DRIVER_CALL orm_driver_validate_transaction_v1(
    const void *object, uint32_t bytes, uint64_t capabilities,
    orm_error_t *error);
orm_status_t ORM_DRIVER_CALL orm_driver_validate_cursor_v1(
    const void *object, uint32_t bytes, uint64_t capabilities,
    orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif
