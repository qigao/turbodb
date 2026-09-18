/* @internal @incomplete Test-only Task3 RED input (#30).
 * Replace after the handshake behavior assertions have actually failed;
 * never linked into production or installed as a driver. */
#include "orm_driver_fixture.h"
#include <string.h>
static const uint8_t bundle[ORM_DRIVER_BUNDLE_ID_BYTES] = {17u, 29u, 43u};
void orm_driver_fixture_reset(void) { /* RED has no owned state. */ }
orm_driver_fixture_stats orm_driver_fixture_stats_get(void) {
  orm_driver_fixture_stats stats = {0}; return stats;
}
const uint8_t *orm_driver_fixture_bundle(void) { return bundle; }
orm_driver_host_v1 orm_driver_fixture_host(void) {
  orm_driver_host_v1 host = {0};
  host.header = (orm_driver_header_v1){sizeof(host), ORM_DRIVER_ABI_VERSION};
  memcpy(host.bundle_id, bundle, sizeof(bundle));
  /* Readable table for the negative host case, never called by RED entry. */
  static const orm_driver_lifetime_ops_v1 life = {{sizeof(life), ORM_DRIVER_ABI_VERSION}, NULL, NULL};
  host.lifetime = (orm_driver_table_v1){&life, sizeof(life), 0u};
  return host;
}
void orm_driver_fixture_fail_next(uint32_t point) { (void)point; }
orm_status_t orm_driver_fixture_set_value(orm_driver_connection_v1 *c, orm_driver_bytes_v1 v) {
  (void)c; (void)v; return ORM_STATUS_UNSUPPORTED;
}
ORM_DRIVER_EXPORT int32_t ORM_DRIVER_CALL orm_driver_get_api_v1(
    const orm_driver_host_v1 *host, uint32_t bytes,
    const orm_driver_api_v1 **out, uint32_t *out_bytes) {
  (void)host; (void)bytes; (void)out; (void)out_bytes;
  return ORM_STATUS_UNSUPPORTED;
}
