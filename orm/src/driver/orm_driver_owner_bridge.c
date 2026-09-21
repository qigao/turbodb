#include "orm_driver_owner_bridge.h"
#include "../abi/orm_internal.h"

#define OWNER_HEADER(T) {(uint32_t)sizeof(T), ORM_DRIVER_ABI_VERSION}

static orm_status_t ORM_DRIVER_CALL owner_acquire(
    void *parent, void **out_lease, orm_error_t *error) {
  return orm_query_acquire_driver_lease(
      (const orm_query_plan *)parent, out_lease, error);
}

static void ORM_DRIVER_CALL owner_release(void *lease) {
  orm_query_release_driver_lease(lease);
}

static const orm_driver_lifetime_ops_v1 owner_services = {
    OWNER_HEADER(orm_driver_lifetime_ops_v1), owner_acquire, owner_release};

const orm_driver_lifetime_ops_v1 *orm_driver_owner_services_v1(void) {
  return &owner_services;
}
