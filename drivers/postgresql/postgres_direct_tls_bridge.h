#ifndef ORM_POSTGRES_DIRECT_TLS_BRIDGE_H
#define ORM_POSTGRES_DIRECT_TLS_BRIDGE_H

#include "postgres_transport_policy.h"

#include <cnet/cnet.h>

#ifndef CNET_TLS_SERVER_END_POINT_MAX_BYTES
#error "TurboDB PostgreSQL direct TLS requires Salts CNet RFC5929 binding API"
#endif

#include <stddef.h>
#include <stdint.h>

typedef int32_t orm_postgres_bridge_state;
enum {
  ORM_POSTGRES_BRIDGE_INIT = 0,
  ORM_POSTGRES_BRIDGE_LISTENING,
  ORM_POSTGRES_BRIDGE_LOCAL_ACCEPTED,
  ORM_POSTGRES_BRIDGE_REMOTE_CONNECTING,
  ORM_POSTGRES_BRIDGE_TLS_HANDSHAKING,
  ORM_POSTGRES_BRIDGE_REMOTE_TLS_READY,
  ORM_POSTGRES_BRIDGE_FORWARDING,
  ORM_POSTGRES_BRIDGE_CLOSING,
  ORM_POSTGRES_BRIDGE_CLOSED,
  ORM_POSTGRES_BRIDGE_FAILED
};

typedef struct orm_postgres_direct_tls_bridge {
  cnet_listener listener;
  cnet_client client;
  cnet_connection local;
  cnet_connection remote;
  orm_postgres_transport_policy policy;
  orm_postgres_bridge_state state;
  uint8_t channel_binding[CNET_TLS_SERVER_END_POINT_MAX_BYTES];
  size_t channel_binding_size;
  uint16_t local_port;
  int local_live;
  int remote_live;
  int remote_tls_ready;
  int forwarding_enabled;
  int receive_started;
  int failure_status;
} orm_postgres_direct_tls_bridge;

#define ORM_POSTGRES_DIRECT_TLS_BRIDGE_INIT {0}

int orm_postgres_direct_tls_bridge_init(
    orm_postgres_direct_tls_bridge *bridge,
    const orm_postgres_transport_policy *policy);
int orm_postgres_direct_tls_bridge_progress(
    orm_postgres_direct_tls_bridge *bridge, uint32_t timeout_ms);
int orm_postgres_direct_tls_bridge_channel_binding(
    const orm_postgres_direct_tls_bridge *bridge,
    uint8_t *output, size_t capacity, size_t *out_size);
int orm_postgres_direct_tls_bridge_enable_forwarding(
    orm_postgres_direct_tls_bridge *bridge);
int orm_postgres_direct_tls_bridge_close(
    orm_postgres_direct_tls_bridge *bridge);
int orm_postgres_direct_tls_bridge_destroy(
    orm_postgres_direct_tls_bridge *bridge);

#endif
