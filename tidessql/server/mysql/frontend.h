#ifndef TDSQL_MYSQL_SERVER_FRONTEND_H
#define TDSQL_MYSQL_SERVER_FRONTEND_H
#include "authenticate.h"
#include "dispatch.h"

enum { TDSQL_MYSQL_FRONT_SEND_GREETING=1,TDSQL_MYSQL_FRONT_WAIT_SSL=2,
       TDSQL_MYSQL_FRONT_WAIT_TLS=3,TDSQL_MYSQL_FRONT_WAIT_LOGIN=4,
       TDSQL_MYSQL_FRONT_SEND_AUTH=5,TDSQL_MYSQL_FRONT_WAIT_PASSWORD=6,
       TDSQL_MYSQL_FRONT_SEND_AUTH_RESULT=7,TDSQL_MYSQL_FRONT_COMMAND=8,
       TDSQL_MYSQL_FRONT_SEND_COMMAND=9,TDSQL_MYSQL_FRONT_FAILED=10,
       TDSQL_MYSQL_FRONT_CLOSED=11 };
enum { TDSQL_MYSQL_FRONT_NONE=0,TDSQL_MYSQL_FRONT_START_TLS=1,
       TDSQL_MYSQL_FRONT_SEND=2,TDSQL_MYSQL_FRONT_RECEIVE=3,
       TDSQL_MYSQL_FRONT_DISCONNECT=4 };
typedef struct tdsql_mysql_frontend_config {
  const tdsql_mysql_auth_policy *policy;
  tdsql_mysql_registry_config registry;
  vstr server_version;
  uint32_t connection_id;
} tdsql_mysql_frontend_config;
static inline tdsql_mysql_frontend_config tdsql_mysql_frontend_config_default(void) {
  return (tdsql_mysql_frontend_config){.registry=tdsql_mysql_registry_config_default(),
      .server_version={"TidesSQL",8}};
}
/* One TCP connection's synchronous protocol owner. Caller supplies distinct,
 * stable fixed input/reply scratch buffers. Network callbacks feed borrowed
 * bytes and immediately obey returned action; no views survive feed. Exactly
 * one send may be outstanding and ACK means CNet send terminal, not peer ACK. */
typedef struct tdsql_mysql_frontend {
  const tdsql_mysql_auth_policy *policy;
  tdsql_mysql_registry_config registry;
  tdsql_mysql_input input;
  tdsql_mysql_negotiation negotiation;
  tdsql_mysql_auth auth;
  tdsql_mysql_dispatch dispatch;
  tdsql_connection *connection;
  mysql_wire_greeting_t greeting;
  uint8_t *scratch;
  size_t scratch_capacity;
  turbodb_status_t failure,cleanup_failure;
  int phase,send_source;
  bool send_pending,tls;
} tdsql_mysql_frontend;
/* Initializes greeting/nonce and bounded framing; no SDK session/network call.
 * Failure preserves out. connection_id must be nonzero and unique among live
 * service connections. Policy/config/buffers remain stable until dispose. */
turbodb_status_t tdsql_mysql_frontend_init(tdsql_mysql_frontend *,const tdsql_mysql_frontend_config *,
    uint8_t *input,size_t input_capacity,uint8_t *scratch,size_t scratch_capacity,turbodb_error_t *);
/* Emits exactly one complete framed packet. SEND_GREETING/AUTH/COMMAND only.
 * Capacity failure is retryable without rerunning KDF/SQL/row advancement.
 * Successful emit requires acknowledge after transport-owned copy completes. */
turbodb_status_t tdsql_mysql_frontend_emit(tdsql_mysql_frontend *,tdsql_mysql_output *,int *action,turbodb_error_t *);
turbodb_status_t tdsql_mysql_frontend_acknowledge(tdsql_mysql_frontend *,int *action,turbodb_error_t *);
/* Copies fragments into the bounded message assembler until one message ends.
 * consumed is always set. Plaintext bytes after SSLRequest, pipelining while a
 * reply is active, malformed framing or excess capacity latch FAILED. */
turbodb_status_t tdsql_mysql_frontend_feed(tdsql_mysql_frontend *,const uint8_t *,size_t,size_t *consumed,
    int *action,turbodb_error_t *);
/* Called exactly once for the actual CNet TLS CONNECTED completion. */
turbodb_status_t tdsql_mysql_frontend_tls_ready(tdsql_mysql_frontend *,int *action,turbodb_error_t *);
/* Stop receives/sends first. Disposes dispatcher before connection, or auth
 * before handoff. BUSY retains resources; other cleanup failure quarantines. */
turbodb_status_t tdsql_mysql_frontend_dispose(tdsql_mysql_frontend *,turbodb_error_t *);
#endif
