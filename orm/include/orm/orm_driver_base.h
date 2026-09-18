#ifndef ORM_DRIVER_BASE_H
#define ORM_DRIVER_BASE_H

#include <stdint.h>

#if defined(_WIN32)
#define ORM_DRIVER_CALL __cdecl
#else
#define ORM_DRIVER_CALL
#endif

#define ORM_DRIVER_ABI_VERSION UINT32_C(1)
#define ORM_DRIVER_HEADER_BYTES UINT32_C(8)
#define ORM_DRIVER_DESCRIPTOR_MAX_BYTES UINT32_C(65536)
#define ORM_DRIVER_ID_MAX_BYTES UINT32_C(63)
#define ORM_DRIVER_BUNDLE_ID_BYTES UINT32_C(32)

#ifdef __cplusplus
extern "C" {
#endif

/* Native C layout, not a serialized format or a proof of pointer validity. */
typedef struct orm_driver_header_v1 {
  uint32_t struct_size;
  uint32_t abi_version;
} orm_driver_header_v1;

typedef struct orm_driver_bytes_v1 {
  const void *data;
  uint64_t size;
} orm_driver_bytes_v1;

typedef struct orm_driver_table_v1 {
  const void *data;
  uint32_t bytes;
  uint32_t reserved;
} orm_driver_table_v1;

#ifdef __cplusplus
}
#endif

#endif
