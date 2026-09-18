#ifndef ORM_DRIVER_FIXTURE_H
#define ORM_DRIVER_FIXTURE_H

#include <orm_driver_abi.h>

/* Single-threaded test driver only, not a database or the #28 owner bridge.
 * reset requires every owned context/ticket to have been released. destroy
 * consumes a context once, after its cursors; reader bytes borrow the cursor. */
#define ORM_DRIVER_FIXTURE_ID "contract_fixture"
#define ORM_DRIVER_FIXTURE_OPTION "fixture_value"
#define ORM_DRIVER_FIXTURE_VALUE_BYTES UINT32_C(64)
#define ORM_DRIVER_FIXTURE_TICKETS UINT32_C(4)
#define ORM_DRIVER_FIXTURE_CONNECTIONS UINT32_C(8)
#define ORM_DRIVER_FIXTURE_MODULES UINT32_C(4)
#define ORM_DRIVER_FIXTURE_CAPS \
  (ORM_DRIVER_CAP_SELECT | ORM_DRIVER_CAP_INCREMENTAL_ROWS)

enum orm_driver_fixture_failure {
  ORM_DRIVER_FIXTURE_NO_FAILURE = 0,
  ORM_DRIVER_FIXTURE_MODULE_BEFORE_ALLOC = 1,
  ORM_DRIVER_FIXTURE_MODULE_AFTER_ALLOC = 2,
  ORM_DRIVER_FIXTURE_CONNECTION_BEFORE_ALLOC = 3,
  ORM_DRIVER_FIXTURE_CONNECTION_AFTER_ALLOC = 4
};

typedef struct orm_driver_fixture_stats {
  uint32_t init_calls, finalize_calls, create_calls, destroy_calls;
  uint32_t live_connections, next_calls, cancel_calls, cursor_destroy_calls;
  uint32_t live_modules, live_cursors, allocations, deallocations;
  uint32_t lease_acquires, lease_releases, live_tickets;
} orm_driver_fixture_stats;

void orm_driver_fixture_reset(void);
orm_driver_fixture_stats orm_driver_fixture_stats_get(void);
const uint8_t *orm_driver_fixture_bundle(void);
orm_driver_host_v1 orm_driver_fixture_host(void);
void orm_driver_fixture_fail_next(uint32_t point);
/* Test-only mutation proves independent connection state; refuses an active
 * cursor. This helper is not an advertised command/SQL capability or export. */
orm_status_t orm_driver_fixture_set_value(orm_driver_connection_v1 *connection,
                                         orm_driver_bytes_v1 value);
#endif
