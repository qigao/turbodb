#include "orm_driver_plan_view.h"

/* Task 5 / #30 explicit test-only RED input. Never linked into turbo_orm.
 * Replace with the complete adapter after recording behavior assertion RED. */
orm_status_t orm_driver_plan_borrow(const orm_query_plan *plan,
    orm_driver_plan_view_v1 *out, orm_error_t *error) {
  (void)plan; (void)out; (void)error;
  return ORM_STATUS_UNSUPPORTED;
}
