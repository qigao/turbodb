#ifndef ORM_DRIVER_PLAN_VIEW_H
#define ORM_DRIVER_PLAN_VIEW_H

#include "orm_internal.h"
#include <orm_driver_plan.h>

/* Internal candidate adapter, not an ABI 4 export. The caller owns an
 * initialized plan and keeps it frozen and alive until every returned byte
 * view has been consumed. This function acquires no #28 execution lease.
 *
 * borrow clears its complete output before validation. Accessor DTO outputs
 * must have an ABI 1 header with struct_size >= the known DTO size and <= the
 * descriptor limit, and enough writable storage for that declared size.
 * Invalid output headers are not modified. After a valid header, errors clear
 * the known payload while preserving the caller header and unknown tail.
 * Unversioned byte outputs are fully cleared on error. All buffers, including
 * error, must be disjoint from the plan and each other. Error may be NULL.
 *
 * parameter_bytes is host budget bookkeeping, not query semantics. It stays
 * in the host plan; per-operation limits travel separately in the SDK.
 */
orm_status_t orm_driver_plan_borrow(const orm_query_plan *plan,
    orm_driver_plan_view_v1 *out, orm_error_t *error);

#endif
