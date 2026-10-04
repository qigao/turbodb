#include "postgres_direct_tls_bridge.h"

#include <libpq-fe.h>
#include <salts/clock.h>
#include <salts_error.h>
#include <tinytest.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int wait_for_binding(orm_postgres_direct_tls_bridge *bridge,
                            uint8_t *binding, size_t capacity,
                            size_t *binding_size) {
  const uint64_t deadline = salts_monotonic_ms() + UINT64_C(5000);
  int status;

  for (;;) {
    status = orm_postgres_direct_tls_bridge_channel_binding(
        bridge, binding, capacity, binding_size);
    if (status == SALTS_OK)
      return SALTS_OK;
    if (status != SALTS_ENOTCONN)
      return status;
    status = orm_postgres_direct_tls_bridge_progress(bridge, 1u);
    if (status != SALTS_OK)
      return status;
    if (salts_monotonic_ms() >= deadline)
      return SALTS_ETIMEDOUT;
  }
}

static int connect_with_mismatched_binding(
    const char *remote_port, const char *ca_file) {
  orm_postgres_direct_tls_bridge bridge = ORM_POSTGRES_DIRECT_TLS_BRIDGE_INIT;
  orm_postgres_transport_policy policy = ORM_POSTGRES_TRANSPORT_POLICY_INIT;
  uint8_t binding[CNET_TLS_SERVER_END_POINT_MAX_BYTES] = {0};
  size_t binding_size = 0u;
  PGconn *connection = NULL;
  char local_port[16];
  char *end = NULL;
  unsigned long parsed_port;
  const uint64_t deadline = salts_monotonic_ms() + UINT64_C(5000);
  PostgresPollingStatusType poll_status = PGRES_POLLING_ACTIVE;
  int status = 1;

  if (remote_port == NULL || remote_port[0] == '\0' ||
      ca_file == NULL || ca_file[0] == '\0')
    return 2;

  parsed_port = strtoul(remote_port, &end, 10);
  if (end == remote_port || *end != '\0' || parsed_port == 0u ||
      parsed_port > 65535u)
    return 3;

  policy.mode = ORM_POSTGRES_TRANSPORT_DIRECT_TLS;
  policy.channel_binding = ORM_POSTGRES_CHANNEL_BINDING_REQUIRE;
  policy.remote_port = (uint16_t)parsed_port;
  policy.connect_timeout_ms = 5000u;
  policy.handshake_timeout_ms = 5000u;
  policy.idle_timeout_ms = 5000u;
  policy.ingress_buffer_bytes = 65536u;
  policy.egress_buffer_bytes = 65536u;
  (void)snprintf(policy.remote_host, sizeof(policy.remote_host), "%s",
                 "127.0.0.1");
  (void)snprintf(policy.server_name, sizeof(policy.server_name), "%s",
                 "localhost");
  (void)snprintf(policy.ca_file, sizeof(policy.ca_file), "%s", ca_file);

  if (orm_postgres_direct_tls_bridge_init(&bridge, &policy) != SALTS_OK)
    goto cleanup;
  if (wait_for_binding(&bridge, binding, sizeof(binding), &binding_size) !=
          SALTS_OK ||
      binding_size == 0u)
    goto cleanup;

  /* Keep the transport valid but bind SASL to the wrong certificate digest. */
  binding[0] ^= UINT8_C(1);

  if (snprintf(local_port, sizeof(local_port), "%u",
               (unsigned)bridge.local_port) <= 0)
    goto cleanup;

  {
    const char *const keywords[] = {
        "host", "port", "dbname", "user", "password",
        "sslmode", "channel_binding", "connect_timeout", NULL};
    const char *const values[] = {
        "127.0.0.1", local_port, "turbodb", "turbodb", "turbodb",
        "disable", "require", "5", NULL};

    connection = PQconnectStartParams(keywords, values, 0);
  }
  if (connection == NULL)
    goto cleanup;
#ifndef LIBPQ_HAS_EXTERNAL_CHANNEL_BINDING
#error "mismatch qualification requires libpq external channel-binding ABI"
#endif
  if (PQsetExternalChannelBinding(connection, "tls-server-end-point",
                                  binding, binding_size) != 1)
    goto cleanup;
  memset(binding, 0, sizeof(binding));
  binding_size = 0u;

  if (orm_postgres_direct_tls_bridge_enable_forwarding(&bridge) != SALTS_OK)
    goto cleanup;

  while (salts_monotonic_ms() < deadline) {
    poll_status = PQconnectPoll(connection);
    if (poll_status == PGRES_POLLING_FAILED) {
      status = 0;
      break;
    }
    if (poll_status == PGRES_POLLING_OK) {
      status = 4;
      break;
    }
    if (orm_postgres_direct_tls_bridge_progress(&bridge, 1u) != SALTS_OK) {
      status = 5;
      break;
    }
  }

cleanup:
  memset(binding, 0, sizeof(binding));
  if (connection != NULL)
    PQfinish(connection);
  if (bridge.client.impl != NULL || bridge.listener.impl != NULL)
    (void)orm_postgres_direct_tls_bridge_destroy(&bridge);
  return status;
}

spec("PostgreSQL SCRAM PLUS external binding mismatch") {
  it("rejects a binding that does not match the verified TLS certificate") {
    const char *port = getenv("TURBODB_PG_TLS_PORT");
    const char *ca = getenv("TURBODB_PG_TLS_CA");

    check_not_null(port);
    check_not_null(ca);
    if (port == NULL || ca == NULL)
      return;

    check_equal(connect_with_mismatched_binding(port, ca), 0);
  }
}
