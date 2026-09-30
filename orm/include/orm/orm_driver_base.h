#ifndef ORM_DRIVER_BASE_H
#define ORM_DRIVER_BASE_H

#include <stdint.h>

#if defined(_WIN32)
#define ORM_DRIVER_CALL __cdecl
#else
#define ORM_DRIVER_CALL
#endif

#define ORM_DRIVER_ABI_VERSION UINT32_C(2)
#define ORM_DRIVER_HEADER_BYTES UINT32_C(8)
#define ORM_DRIVER_DESCRIPTOR_MAX_BYTES UINT32_C(65536)
#define ORM_DRIVER_ID_MAX_BYTES UINT32_C(63)
#define ORM_DRIVER_BUNDLE_ID_BYTES UINT32_C(32)

/*
 * Driver ABI 2 SDK bundle; host and all drivers require a coordinated rebuild.
 * SHA-256("TurboDB|DriverABI=2|ORM_C_ABI=4|CFlowABI=4|CSerdeReaderABI=1|CMetaDataABI=1").
 * #35 owns the release manifest/cutover; host and modules must compile against
 * the same SDK bundle rather than inventing fixture-local IDs.
 */
#define ORM_DRIVER_BUNDLE_ID_INIT { \
  UINT8_C(0xb1), UINT8_C(0x58), UINT8_C(0x42), UINT8_C(0x55), \
  UINT8_C(0xfc), UINT8_C(0xbc), UINT8_C(0x3e), UINT8_C(0x0c), \
  UINT8_C(0x5f), UINT8_C(0x8e), UINT8_C(0x99), UINT8_C(0x04), \
  UINT8_C(0xce), UINT8_C(0x34), UINT8_C(0x15), UINT8_C(0x6e), \
  UINT8_C(0x56), UINT8_C(0xeb), UINT8_C(0x5b), UINT8_C(0xec), \
  UINT8_C(0x16), UINT8_C(0x4f), UINT8_C(0xdf), UINT8_C(0x91), \
  UINT8_C(0xe6), UINT8_C(0x10), UINT8_C(0x48), UINT8_C(0x79), \
  UINT8_C(0x7e), UINT8_C(0x67), UINT8_C(0x36), UINT8_C(0xc7) \
}

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
