#include "postgres_direct_tls_bridge.h"

#include <salts_error.h>
#include <tinytest.h>

#include <string.h>

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

  it("withholds channel binding and forwarding before verified TLS") {
    orm_postgres_direct_tls_bridge bridge = ORM_POSTGRES_DIRECT_TLS_BRIDGE_INIT;
    uint8_t binding[CNET_TLS_SERVER_END_POINT_MAX_BYTES] = {0};
    size_t binding_size = 123u;

    check_equal(
        orm_postgres_direct_tls_bridge_channel_binding(
            &bridge, binding, sizeof(binding), &binding_size),
        SALTS_ENOTCONN);
    check_equal(binding_size, (size_t)0u);
    check_equal(
        orm_postgres_direct_tls_bridge_enable_forwarding(&bridge),
        SALTS_ENOTCONN);
    check_equal(bridge.forwarding_enabled, 0);
  }

  it("copies a verified binding before forwarding is admitted") {
    orm_postgres_direct_tls_bridge bridge = ORM_POSTGRES_DIRECT_TLS_BRIDGE_INIT;
    uint8_t copied[CNET_TLS_SERVER_END_POINT_MAX_BYTES] = {0};
    uint8_t short_copy[16] = {0};
    size_t copied_size = 0u;
    size_t index;

    bridge.state = ORM_POSTGRES_BRIDGE_REMOTE_TLS_READY;
    bridge.remote_tls_ready = 1;
    bridge.channel_binding_size = 32u;
    for (index = 0u; index < bridge.channel_binding_size; ++index)
      bridge.channel_binding[index] = (uint8_t)(index + 1u);

    check_equal(
        orm_postgres_direct_tls_bridge_channel_binding(
            &bridge, short_copy, sizeof(short_copy), &copied_size),
        SALTS_EMSGSIZE);
    check_equal(copied_size, (size_t)32u);

    copied_size = 0u;
    check_equal(
        orm_postgres_direct_tls_bridge_channel_binding(
            &bridge, copied, sizeof(copied), &copied_size),
        SALTS_OK);
    check_equal(copied_size, (size_t)32u);
    check_equal(memcmp(copied, bridge.channel_binding, copied_size), 0);

    check_equal(
        orm_postgres_direct_tls_bridge_enable_forwarding(&bridge),
        SALTS_OK);
    check_equal(bridge.forwarding_enabled, 1);
    check_equal(bridge.receive_started, 0);
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
