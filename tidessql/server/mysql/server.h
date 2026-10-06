#ifndef TDSQL_MYSQL_SERVER_H
#define TDSQL_MYSQL_SERVER_H
#include "frontend.h"
#include <cnet/cnet.h>
#include <cstl/vec.h>

enum {
  TDSQL_MYSQL_SERVER_MAX_CONNECTIONS=1024,
  TDSQL_MYSQL_SERVER_MIN_INPUT_BYTES=64,
  TDSQL_MYSQL_SERVER_MIN_OUTPUT_BYTES=
      MYSQL_WIRE_SERVER_VERSION_CAPACITY+MYSQL_WIRE_AUTH_PLUGIN_NAME_CAPACITY+68
};

/* Private, single-owner CNet adapter. Every capacity is a hard bound. The
 * policy and its databases remain stable through stop. TLS and CNet configs
 * are copied by their owners during init; host/server_version are consumed
 * synchronously. Poll, callbacks, SQL execution and stop stay on one thread. */
typedef struct tdsql_mysql_server_config {
  const tdsql_mysql_auth_policy *policy;
  tdsql_mysql_registry_config registry;
  vstr server_version;
  cnet_listener_config listener;
  cnet_client_config transport;
  cnet_tls_server_config tls;
  size_t max_connections;
  size_t input_bytes;
  size_t scratch_bytes;
  size_t output_bytes;
} tdsql_mysql_server_config;

typedef struct tdsql_mysql_server {
  const tdsql_mysql_auth_policy *policy;
  tdsql_mysql_registry_config registry;
  vstr server_version;
  cnet_listener listener;
  cnet_client client;
  cnet_tls_server tls;
  vec_t slots;
  size_t max_connections,input_bytes,scratch_bytes,output_bytes,active;
  uint64_t accepted,closed,transport_failures,protocol_failures,cleanup_failures;
  uint32_t next_connection_id;
  int network_failure;
  bool initialized,stopping;
} tdsql_mysql_server;

/* Initializes all bounded control-plane owners atomically from the caller's
 * perspective. On failure it releases partial CNet/TLS/listener/vector state. */
turbodb_status_t tdsql_mysql_server_init(tdsql_mysql_server *,const tdsql_mysql_server_config *,
    turbodb_error_t *);
/* Accepts at most one peer, then performs one bounded CNet poll. Application
 * protocol failures close only that peer. Owner/progress failure is returned. */
turbodb_status_t tdsql_mysql_server_poll(tdsql_mysql_server *,uint32_t timeout_ms,size_t *events,
    turbodb_error_t *);
turbodb_status_t tdsql_mysql_server_port(const tdsql_mysql_server *,uint16_t *,turbodb_error_t *);
/* Stops admission, drains terminal callbacks and releases all owners. Timeout
 * is retryable and preserves the server. Cleanup failure is quarantined and
 * reported; successful stop zeros the server. */
turbodb_status_t tdsql_mysql_server_stop(tdsql_mysql_server *,uint32_t timeout_ms,turbodb_error_t *);

#endif
