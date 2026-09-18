#include "orm_driver_contract.h"

#include <tinytest.h>

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(orm_driver_header_v1) == 8u, "driver prefix size");
_Static_assert(offsetof(orm_driver_header_v1, struct_size) == 0u,
               "driver size offset");
_Static_assert(offsetof(orm_driver_header_v1, abi_version) == 4u,
               "driver version offset");
_Static_assert(sizeof(orm_status_t) == sizeof(int32_t), "status width");

spec("driver prefix bounds") {
  it("rejects every physically short allocation without reading a header") {
    for (uint32_t n = 0u; n < ORM_DRIVER_HEADER_BYTES; ++n) {
      unsigned char *p = (unsigned char *)malloc(n == 0u ? 1u : n);
      uint32_t out = UINT32_MAX;
      orm_status_t status;
      check_not_null(p);
      memset(p, 0, n == 0u ? 1u : n);
      status = orm_driver_check_prefix(p, n, ORM_DRIVER_ABI_VERSION,
                                      ORM_DRIVER_HEADER_BYTES, &out);
      free(p);
      check_equal(status, ORM_STATUS_ABI_MISMATCH);
      check_equal(out, UINT32_C(0));
    }
  }

  it("accepts an exact fixed header") {
    const orm_driver_header_v1 header = {ORM_DRIVER_HEADER_BYTES,
                                         ORM_DRIVER_ABI_VERSION};
    uint32_t out = 0u;
    check_equal(orm_driver_check_prefix(&header, sizeof(header),
                    ORM_DRIVER_ABI_VERSION, ORM_DRIVER_HEADER_BYTES, &out),
                ORM_STATUS_OK);
    check_equal(out, ORM_DRIVER_HEADER_BYTES);
  }

  it("reads an unaligned header through bytes rather than a typed pointer") {
    const orm_driver_header_v1 header = {ORM_DRIVER_HEADER_BYTES,
                                         ORM_DRIVER_ABI_VERSION};
    unsigned char *p = (unsigned char *)malloc(sizeof(header) + 1u);
    uint32_t out = 0u;
    orm_status_t status;
    check_not_null(p);
    memcpy(p + 1u, &header, sizeof(header));
    status = orm_driver_check_prefix(p + 1u, sizeof(header),
               ORM_DRIVER_ABI_VERSION, ORM_DRIVER_HEADER_BYTES, &out);
    free(p);
    check_equal(status, ORM_STATUS_OK);
    check_equal(out, ORM_DRIVER_HEADER_BYTES);
  }

  it("clears the output when the input buffer is null") {
    uint32_t out = UINT32_MAX;
    check_equal(orm_driver_check_prefix(NULL, 0u, ORM_DRIVER_ABI_VERSION,
                    ORM_DRIVER_HEADER_BYTES, &out), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(out, UINT32_C(0));
  }

  it("rejects null output without reading the input") {
    const unsigned char byte = 0u;
    check_equal(orm_driver_check_prefix(&byte, 0u, ORM_DRIVER_ABI_VERSION,
                    ORM_DRIVER_HEADER_BYTES, NULL), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(orm_driver_check_prefix(NULL, 0u, ORM_DRIVER_ABI_VERSION,
                    ORM_DRIVER_HEADER_BYTES, NULL), ORM_STATUS_INVALID_ARGUMENT);
  }

  it("rejects a required prefix shorter than the fixed header") {
    const unsigned char byte = 0u;
    for (uint32_t n = 0u; n < ORM_DRIVER_HEADER_BYTES; ++n) {
      uint32_t out = UINT32_MAX;
      check_equal(orm_driver_check_prefix(&byte, 0u, ORM_DRIVER_ABI_VERSION,
                                          n, &out), ORM_STATUS_INVALID_ARGUMENT);
      check_equal(out, UINT32_C(0));
    }
  }

  it("rejects self-reported sizes below the required prefix") {
    for (uint32_t n = 0u; n < ORM_DRIVER_HEADER_BYTES; ++n) {
      const orm_driver_header_v1 header = {n, ORM_DRIVER_ABI_VERSION};
      uint32_t out = UINT32_MAX;
      check_equal(orm_driver_check_prefix(&header, sizeof(header),
                      ORM_DRIVER_ABI_VERSION, ORM_DRIVER_HEADER_BYTES, &out),
                  ORM_STATUS_ABI_MISMATCH);
      check_equal(out, UINT32_C(0));
    }
  }

  it("rejects a caller-required suffix absent from the descriptor") {
    const orm_driver_header_v1 header = {ORM_DRIVER_HEADER_BYTES,
                                         ORM_DRIVER_ABI_VERSION};
    uint32_t out = UINT32_MAX;
    check_equal(orm_driver_check_prefix(&header, sizeof(header),
                    ORM_DRIVER_ABI_VERSION, ORM_DRIVER_HEADER_BYTES + 1u, &out),
                ORM_STATUS_ABI_MISMATCH);
    check_equal(out, UINT32_C(0));
  }

  it("rejects self-reported size beyond the actual readable span") {
    const uint32_t sizes[] = {ORM_DRIVER_HEADER_BYTES + 1u, UINT32_MAX};
    for (size_t i = 0u; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
      const orm_driver_header_v1 header = {sizes[i], ORM_DRIVER_ABI_VERSION};
      uint32_t out = UINT32_MAX;
      check_equal(orm_driver_check_prefix(&header, sizeof(header),
                      ORM_DRIVER_ABI_VERSION, ORM_DRIVER_HEADER_BYTES, &out),
                  ORM_STATUS_ABI_MISMATCH);
      check_equal(out, UINT32_C(0));
    }
  }

  it("rejects incompatible versions and clears stale output") {
    const uint32_t versions[] = {0u, ORM_DRIVER_ABI_VERSION + 1u, UINT32_MAX};
    for (size_t i = 0u; i < sizeof(versions) / sizeof(versions[0]); ++i) {
      const orm_driver_header_v1 header = {ORM_DRIVER_HEADER_BYTES, versions[i]};
      uint32_t out = UINT32_MAX;
      check_equal(orm_driver_check_prefix(&header, sizeof(header),
                      ORM_DRIVER_ABI_VERSION, ORM_DRIVER_HEADER_BYTES, &out),
                  ORM_STATUS_ABI_MISMATCH);
      check_equal(out, UINT32_C(0));
    }
  }

  it("uses the requested version rather than hardcoding version one") {
    const orm_driver_header_v1 header = {ORM_DRIVER_HEADER_BYTES, 7u};
    uint32_t out = 0u;
    check_equal(orm_driver_check_prefix(&header, sizeof(header), 7u,
                    ORM_DRIVER_HEADER_BYTES, &out), ORM_STATUS_OK);
    check_equal(out, ORM_DRIVER_HEADER_BYTES);
  }

  it("accepts uninterpreted tail bytes but reports only declared size") {
    unsigned char buffer[16];
    orm_driver_header_v1 header = {sizeof(buffer), ORM_DRIVER_ABI_VERSION};
    uint32_t out = 0u;
    memset(buffer, 0xff, sizeof(buffer));
    memcpy(buffer, &header, sizeof(header));
    check_equal(orm_driver_check_prefix(buffer, sizeof(buffer),
                    ORM_DRIVER_ABI_VERSION, ORM_DRIVER_HEADER_BYTES, &out),
                ORM_STATUS_OK);
    check_equal(out, (uint32_t)sizeof(buffer));
    header.struct_size = ORM_DRIVER_HEADER_BYTES;
    memcpy(buffer, &header, sizeof(header));
    check_equal(orm_driver_check_prefix(buffer, sizeof(buffer),
                    ORM_DRIVER_ABI_VERSION, ORM_DRIVER_HEADER_BYTES, &out),
                ORM_STATUS_OK);
    check_equal(out, ORM_DRIVER_HEADER_BYTES);
  }

  it("accepts 65536 declared bytes and rejects 65537 with a limit error") {
    const uint32_t sizes[] = {ORM_DRIVER_DESCRIPTOR_MAX_BYTES,
                             ORM_DRIVER_DESCRIPTOR_MAX_BYTES + 1u};
    for (size_t i = 0u; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
      const orm_driver_header_v1 header = {sizes[i], ORM_DRIVER_ABI_VERSION};
      unsigned char *buffer = (unsigned char *)calloc(sizes[i], 1u);
      uint32_t out = UINT32_MAX;
      orm_status_t status;
      check_not_null(buffer);
      memcpy(buffer, &header, sizeof(header));
      status = orm_driver_check_prefix(buffer, sizes[i], ORM_DRIVER_ABI_VERSION,
                                       ORM_DRIVER_HEADER_BYTES, &out);
      free(buffer);
      check_equal(status, i == 0u ? ORM_STATUS_OK : ORM_STATUS_LIMIT_EXCEEDED);
      check_equal(out, i == 0u ? sizes[i] : UINT32_C(0));
    }
  }
}

spec("driver byte view bounds") {
  it("accepts either null or nonnull empty views at a zero budget") {
    const unsigned char byte = 0u;
    const orm_driver_bytes_v1 null_view = {NULL, 0u};
    const orm_driver_bytes_v1 empty_view = {&byte, 0u};
    check_equal(orm_driver_check_bytes(null_view, 0u), ORM_STATUS_OK);
    check_equal(orm_driver_check_bytes(empty_view, 0u), ORM_STATUS_OK);
  }

  it("rejects a null nonempty view before testing the budget") {
    const orm_driver_bytes_v1 value = {NULL, 1u};
    check_equal(orm_driver_check_bytes(value, 0u), ORM_STATUS_INVALID_ARGUMENT);
    check_equal(orm_driver_check_bytes(value, UINT64_MAX),
                ORM_STATUS_INVALID_ARGUMENT);
  }

  it("accepts exact payload budget and rejects a smaller one") {
    const unsigned char payload[] = {'a', 0, 'b'};
    const orm_driver_bytes_v1 value = {payload, sizeof(payload)};
    check_equal(orm_driver_check_bytes(value, sizeof(payload)), ORM_STATUS_OK);
    check_equal(orm_driver_check_bytes(value, sizeof(payload) - 1u),
                ORM_STATUS_LIMIT_EXCEEDED);
    check_equal(orm_driver_check_bytes(value, 0u), ORM_STATUS_LIMIT_EXCEEDED);
  }

  it("checks UINT64_MAX numerically without dereferencing the payload") {
    /* A nominal extent tests arithmetic only; check_bytes never reads data. */
    const unsigned char byte = 0u;
    const orm_driver_bytes_v1 value = {&byte, UINT64_MAX};
    check_equal(orm_driver_check_bytes(value, UINT64_MAX - UINT64_C(1)),
                ORM_STATUS_LIMIT_EXCEEDED);
#if SIZE_MAX < UINT64_MAX
    check_equal(orm_driver_check_bytes(value, UINT64_MAX),
                ORM_STATUS_LIMIT_EXCEEDED);
#else
    check_equal(orm_driver_check_bytes(value, UINT64_MAX), ORM_STATUS_OK);
#endif
  }

  it("enforces the native size_t conversion boundary") {
    const unsigned char byte = 0u;
    orm_driver_bytes_v1 value = {&byte, (uint64_t)SIZE_MAX};
    check_equal(orm_driver_check_bytes(value, UINT64_MAX), ORM_STATUS_OK);
#if SIZE_MAX < UINT64_MAX
    value.size = (uint64_t)SIZE_MAX + UINT64_C(1);
    check_equal(orm_driver_check_bytes(value, UINT64_MAX),
                ORM_STATUS_LIMIT_EXCEEDED);
#endif
  }
}
