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

/*
 * Current Driver ABI 1 development compatibility bundle.
 * SHA-256("TurboDB|DriverABI=1|ORM_C_ABI=4|CFlowABI=4|CSerdeReaderABI=1|CMetaDataABI=1").
 * #35 owns the release manifest/cutover; host and modules must compile against
 * the same SDK bundle rather than inventing fixture-local IDs.
 */
#define ORM_DRIVER_BUNDLE_ID_INIT { \
  UINT8_C(0xab), UINT8_C(0x7a), UINT8_C(0x72), UINT8_C(0xf6), \
  UINT8_C(0x8f), UINT8_C(0x24), UINT8_C(0x34), UINT8_C(0xde), \
  UINT8_C(0x9c), UINT8_C(0x06), UINT8_C(0x51), UINT8_C(0xbf), \
  UINT8_C(0xfa), UINT8_C(0x23), UINT8_C(0x83), UINT8_C(0x9c), \
  UINT8_C(0x81), UINT8_C(0x44), UINT8_C(0xf7), UINT8_C(0x87), \
  UINT8_C(0x0a), UINT8_C(0xc2), UINT8_C(0xfe), UINT8_C(0x94), \
  UINT8_C(0xf1), UINT8_C(0x25), UINT8_C(0x04), UINT8_C(0x12), \
  UINT8_C(0x3c), UINT8_C(0x39), UINT8_C(0x9f), UINT8_C(0x8e) \
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
