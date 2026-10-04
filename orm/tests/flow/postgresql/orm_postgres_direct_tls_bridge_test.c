#include "postgres_direct_tls_bridge.h"

#include <salts_error.h>
#include <tinytest.h>

spec("PostgreSQL direct TLS bridge lifecycle") {
  it("rejects non-direct-TLS policies without publishing ownership") {
    orm_postgres_direct_tls_bridge bridge = ORM_POSTGRES_DIRECT_TLS_BRIDGE_INIT;
    orm_postgres_transport_policy policy = ORM_POSTGRES_TRANSPORT_POLICY_INIT;

    check_equal(
        orm_postgres_direct_tls_bridge_init(&bridge, &policy),
        SALTS_EINVAL);
    check_null(bridge.client.impl);
    check_null(bridge.listener.impl);
  }

  it("rejects incomplete direct-TLS identity before opening sockets") {
    orm_postgres_direct_tls_bridge bridge = ORM_POSTGRES_DIRECT_TLS_BRIDGE_INIT;
    orm_postgres_transport_policy policy = ORM_POSTGRES_TRANSPORT_POLICY_INIT;

    policy.mode = ORM_POSTGRES_TRANSPORT_DIRECT_TLS;
    policy.remote_port = 5432u;

    check_equal(
        orm_postgres_direct_tls_bridge_init(&bridge, &policy),
        SALTS_EINVAL);
    check_null(bridge.client.impl);
    check_null(bridge.listener.impl);
  }

  it("destroys an empty bridge idempotently at the ownership boundary") {
    orm_postgres_direct_tls_bridge bridge = ORM_POSTGRES_DIRECT_TLS_BRIDGE_INIT;

    check_equal(
        orm_postgres_direct_tls_bridge_destroy(&bridge),
        SALTS_OK);
    check_equal(bridge.state, ORM_POSTGRES_BRIDGE_CLOSED);
    check_null(bridge.client.impl);
    check_null(bridge.listener.impl);
  }
}
