#ifndef TDSQL_MYSQL_SERVER_REGISTRY_H
#define TDSQL_MYSQL_SERVER_REGISTRY_H
#include "wire.h"
#include <tidessql/tidessql.h>
#include <cstl/vec.h>

enum { TDSQL_MYSQL_DEFAULT_STATEMENTS=64, TDSQL_MYSQL_DEFAULT_REGISTRY_BYTES=16777216 };
typedef struct tdsql_mysql_registry_config {
  size_t max_statements,max_wire_bytes,max_command_bytes;
  tdsql_limits limits;
} tdsql_mysql_registry_config;
static inline tdsql_mysql_registry_config tdsql_mysql_registry_config_default(void) {
  return (tdsql_mysql_registry_config){TDSQL_MYSQL_DEFAULT_STATEMENTS,
      TDSQL_MYSQL_DEFAULT_REGISTRY_BYTES,MYSQL_WIRE_PACKET_MAX_PAYLOAD,tdsql_limits_default()};
}

/* Private single-owner, stable-address resource. No copying/reentrancy. SDK
 * connection borrows until disposal consumes the registry; BUSY retains it.
 * Slots never grow or move.
 * Only this registry closes its handles. Results belong to its caller. */
typedef struct tdsql_mysql_registry {
  tdsql_connection *connection;
  tdsql_mysql_registry_config config;
  vec_t slots;
  uint32_t last_id;
  turbodb_status_t failure;
  bool closing;
} tdsql_mysql_registry;

/* Zero-state out; positive bounded configuration. Includes owner, slots and
 * Salts receipt/alignment requests in wire byte quota. Failure preserves out. */
turbodb_status_t tdsql_mysql_registry_init(tdsql_mysql_registry *,tdsql_connection *,
    const tdsql_mysql_registry_config *,turbodb_error_t *);
/* Complete validated command payload borrows through the call. SDK owns SQL
 * and metadata; successful prepare alone publishes ID/counts. Failure preserves
 * output and consumes no ID. All protocol counts fit uint16_t. */
turbodb_status_t tdsql_mysql_registry_prepare(tdsql_mysql_registry *,const tdsql_mysql_command *,
    mysql_stmt_prepare_ok_t *,turbodb_error_t *);
/* Execute native wire values directly, no interpolation/reprepare/replay.
 * Initialized response with no live result required; failure preserves it.
 * Types persist after valid payload, even on SQL errors. Values clear on return;
 * result owns copies and must be checked-destroyed, including after EOF/cancel. */
turbodb_status_t tdsql_mysql_registry_execute(tdsql_mysql_registry *,const tdsql_mysql_command *,
    tdsql_response *,turbodb_error_t *);
/* Metadata names borrow the SDK handle until close/dispose. Execute result's
 * metadata remains authoritative for that execution, including dynamic types. */
turbodb_status_t tdsql_mysql_registry_parameter(tdsql_mysql_registry *,uint32_t,size_t,tdsql_column *,turbodb_error_t *);
turbodb_status_t tdsql_mysql_registry_column(tdsql_mysql_registry *,uint32_t,size_t,tdsql_column *,turbodb_error_t *);
/* RESET preserves cache/ID/SQL and transaction. CLOSE has no wire response;
 * unknown nonzero close is a no-op per MySQL, other unknown IDs reject.
 * BUSY close retains handle; other close failures consume and latch failure. */
turbodb_status_t tdsql_mysql_registry_reset(tdsql_mysql_registry *,uint32_t,turbodb_error_t *);
turbodb_status_t tdsql_mysql_registry_close(tdsql_mysql_registry *,uint32_t,turbodb_error_t *);
/* Read-only count derived from owned slots, O(max_statements). */
size_t tdsql_mysql_registry_count(const tdsql_mysql_registry *);
bool tdsql_mysql_registry_contains(const tdsql_mysql_registry *,uint32_t);
/* Stop admission, release caller-owned results, dispose then close connection.
 * Attempts every handle. BUSY retains remaining resources and closing state;
 * clear results then call again. Other failures consume handles, propagate and
 * never restore admission. NULL/zero registry is a successful no-op. */
turbodb_status_t tdsql_mysql_registry_dispose(tdsql_mysql_registry *,turbodb_error_t *);
#endif
