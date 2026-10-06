#ifndef TDSQL_MYSQL_SERVER_AUTHENTICATE_H
#define TDSQL_MYSQL_SERVER_AUTHENTICATE_H
#include "password.h"
#include <tidessql/tidessql.h>

enum { TDSQL_MYSQL_DEFAULT_ACCOUNTS=128,TDSQL_MYSQL_MAX_ACCOUNTS=1024,
       TDSQL_MYSQL_MAX_DATABASES=64,TDSQL_MYSQL_AUTH_PAYLOAD_BYTES=128,
       TDSQL_MYSQL_AUTH_SEND_FULL=1,TDSQL_MYSQL_AUTH_WAIT_PASSWORD=2,
       TDSQL_MYSQL_AUTH_SEND_RESULT=3,TDSQL_MYSQL_AUTH_READY=4,
       TDSQL_MYSQL_AUTH_TRANSFERRED=5,TDSQL_MYSQL_AUTH_FAILED=6 };
typedef struct tdsql_mysql_account {
  vstr username;
  tdsql_mysql_password_record password;
  uint64_t databases;
  size_t default_database;
} tdsql_mysql_account;
typedef struct tdsql_mysql_database_binding {
  vstr name;
  tdsql_database *database;
} tdsql_mysql_database_binding;
/* Immutable control-plane spans, not dynamic arrays or client filesystem paths.
 * All accounts use the same work factor. Exact byte matching, no name fallback.
 * Backing names/records/databases and this policy remain stable until every
 * authentication/command owner is disposed. Caller clears records afterward. */
typedef struct tdsql_mysql_auth_policy {
  const tdsql_mysql_account *accounts;
  size_t account_count,max_accounts;
  const tdsql_mysql_database_binding *databases;
  size_t database_count;
} tdsql_mysql_auth_policy;
static inline tdsql_mysql_auth_policy tdsql_mysql_auth_policy_default(void) {
  return (tdsql_mysql_auth_policy){.max_accounts=TDSQL_MYSQL_DEFAULT_ACCOUNTS};
}
/* Validate once while quiescent. No SDK calls/allocation; failure preserves out.
 * A validated policy and backing spans must never be mutated while borrowed. */
turbodb_status_t tdsql_mysql_auth_policy_init(tdsql_mysql_auth_policy *,const tdsql_mysql_auth_policy *,turbodb_error_t *);

/* Private single-threaded owner. No copying/reentry. Login/password views are
 * borrowed only during begin/accept; policy indices survive, no network views.
 * Owns connection until final OK ACK + take. Non-BUSY close failure quarantines
 * retained connection and is never automatically retried by this owner. */
typedef struct tdsql_mysql_auth {
  const tdsql_mysql_auth_policy *policy;
  tdsql_connection *connection;
  mysql_wire_ok_packet_t ok;
  size_t account,database;
  turbodb_status_t failure,cleanup_failure;
  uint16_t error_code;
  uint8_t sequence,next_sequence;
  int phase;
  bool emitted;
} tdsql_mysql_auth;
/* Zero owner, validated frozen policy, gate WAIT_CREDENTIALS and its complete
 * login only. Begins full-auth even for unknown users; no SDK session yet. */
turbodb_status_t tdsql_mysql_auth_begin(tdsql_mysql_auth *,const tdsql_mysql_auth_policy *,
    const tdsql_mysql_negotiation *,const tdsql_mysql_login *,turbodb_error_t *);
/* After full-auth request handoff ACK, accept one seq4 password under actual
 * transport TLS. Verify then authorize then SDK connect/state exactly once.
 * OK means a final OK/ERR is planned, not authenticated. Caller immediately
 * clears owned password storage. Framing/TLS errors terminate without resync. */
turbodb_status_t tdsql_mysql_auth_accept(tdsql_mysql_auth *,const uint8_t *,size_t,uint8_t sequence,bool tls,turbodb_error_t *);
/* Atomic complete framed reply, out must have non-NULL owned storage. Capacity
 * failure permits same reply retry with no KDF/session replay. Successful emit
 * waits for exactly one ACK after transport ownership transfer, not peer ACK. */
turbodb_status_t tdsql_mysql_auth_emit(tdsql_mysql_auth *,tdsql_mysql_output *,turbodb_error_t *);
turbodb_status_t tdsql_mysql_auth_acknowledge(tdsql_mysql_auth *,turbodb_error_t *);
/* Only after final OK ACK; moves ownership once, preserves output on failure.
 * No command admission before this. Prepared dispatcher may borrow connection
 * during SEND_RESULT, but must be disposed before auth if handoff fails. */
turbodb_status_t tdsql_mysql_auth_take(tdsql_mysql_auth *,tdsql_connection **,turbodb_error_t *);
/* Prepare/transport owner failure before final OK emit replaces it with ERR.
 * Retains connection for checked cleanup; never reopens/replays SQL. */
turbodb_status_t tdsql_mysql_auth_reject(tdsql_mysql_auth *,turbodb_status_t,turbodb_error_t *);
/* Stop admission and dispose any dispatcher borrowing connection first.
 * BUSY retains closing owner; other close failure is terminal and retained.
 * Successful close clears owner. NULL/zero/transferred are safe empty disposal. */
turbodb_status_t tdsql_mysql_auth_dispose(tdsql_mysql_auth *,turbodb_error_t *);
#endif
