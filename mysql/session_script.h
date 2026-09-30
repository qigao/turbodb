#ifndef TURBODB_MYSQL_SESSION_SCRIPT_H
#define TURBODB_MYSQL_SESSION_SCRIPT_H

#include "session.h"
#include "wire/packet.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reserve COM_QUERY and avoid a full-size packet requiring a continuation. */
#define MYSQL_SESSION_SCRIPT_MAX_BYTES (MYSQL_WIRE_PACKET_MAX_PAYLOAD - 2u)

/* Executes a complete SQL script in one dedicated TLS-verified session.
 * Inputs are borrowed until return; single-threaded, caller-driven progress.
 * max_script_bytes bounds SQL size, also capped at SCRIPT_MAX_BYTES. Temporary
 * send storage is bounded by two (sql_size + header + command) buffers; replies
 * use the session's fixed control buffer. Capacity/OOM/transport errors fail
 * without retries; the connection is always destroyed before return.
 * Success publishes the number of OK responses; failure leaves it zero. Server
 * SQL errors, unsupported row/local-file results and protocol errors are distinct.
 * No splitting, implicit transaction or rollback of already committed work.
 * MySQL DDL may commit implicitly; loss of acknowledgement is not retry-safe.
 * DELIMITER is a mysql client command, not server SQL, and is not interpreted.
 * error is optional; out_statements is required.
 */
mysql_session_status_t mysql_session_execute_script(
    const mysql_session_config_t *config, const uint8_t *sql, size_t sql_size,
    size_t max_script_bytes, uint64_t *out_statements, mysql_session_error_t *error);

#ifdef __cplusplus
}
#endif
#endif
