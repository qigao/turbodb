/* #30 Task 2 RED only. Removed when real validators replace this test input.
 * Not linked to the SDK library or exposed by the stable ORM facade. */
#include <orm_driver_abi.h>

orm_status_t ORM_DRIVER_CALL orm_driver_validate_host_v1(const void *h, uint32_t n, const uint8_t b[ORM_DRIVER_BUNDLE_ID_BYTES], orm_error_t *e) {
  (void)h; (void)n; (void)b; (void)e;
  return ORM_STATUS_UNSUPPORTED;
}

orm_status_t ORM_DRIVER_CALL orm_driver_validate_api_v1(const void *a, uint32_t n, const uint8_t b[ORM_DRIVER_BUNDLE_ID_BYTES], orm_driver_bytes_v1 id, uint32_t m, orm_error_t *e) {
  (void)a; (void)n; (void)b; (void)id; (void)m; (void)e;
  return ORM_STATUS_UNSUPPORTED;
}

orm_status_t ORM_DRIVER_CALL orm_driver_validate_connection_v1(const void *o, uint32_t n, uint64_t c, orm_error_t *e) {
  (void)o; (void)n; (void)c; (void)e;
  return ORM_STATUS_UNSUPPORTED;
}

orm_status_t ORM_DRIVER_CALL orm_driver_validate_transaction_v1(const void *o, uint32_t n, uint64_t c, orm_error_t *e) {
  (void)o; (void)n; (void)c; (void)e;
  return ORM_STATUS_UNSUPPORTED;
}

orm_status_t ORM_DRIVER_CALL orm_driver_validate_cursor_v1(const void *o, uint32_t n, uint64_t c, orm_error_t *e) {
  (void)o; (void)n; (void)c; (void)e;
  return ORM_STATUS_UNSUPPORTED;
}
