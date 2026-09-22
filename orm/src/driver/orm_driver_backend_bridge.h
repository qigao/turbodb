#ifndef ORM_DRIVER_BACKEND_BRIDGE_H
#define ORM_DRIVER_BACKEND_BRIDGE_H

#include "../abi/orm_internal.h"

#include <orm_driver_plan.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Private in-repo driver support. The input side is exclusively the public
 * Driver ABI plan/limits surface; the output is module-local owned state used
 * by the existing backend implementation. No host-private plan pointer is
 * dereferenced or retained.
 */
orm_status_t orm_driver_backend_plan_materialize(
    const orm_driver_plan_view_v1 *view,
    const orm_driver_limits_v1 *limits,
    orm_query_plan *out_plan,
    orm_error_t *error);

void orm_driver_backend_plan_destroy(orm_query_plan *plan);

/*
 * Transfer a backend created by an existing in-repo factory into Driver ABI
 * connection ownership. The bridge allocates only wrapper state. The backend
 * implementation and all native database handles remain module-local.
 */
orm_status_t orm_driver_backend_connection_create(
    orm_backend_factory_v1 factory,
    const orm_config_t *config,
    const orm_driver_limits_v1 *limits,
    orm_driver_connection_v1 *out_connection,
    orm_error_t *error);

#ifdef __cplusplus
}
#endif

#endif
