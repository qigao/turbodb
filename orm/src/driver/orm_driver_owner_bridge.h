#ifndef ORM_DRIVER_OWNER_BRIDGE_H
#define ORM_DRIVER_OWNER_BRIDGE_H

#include <orm_driver_ops.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Host service table backed by the #28 query dependent owner. The acquire
 * parent is the opaque token supplied in orm_driver_plan_view_v1.context. */
const orm_driver_lifetime_ops_v1 *orm_driver_owner_services_v1(void);

#ifdef __cplusplus
}
#endif
#endif
