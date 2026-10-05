#ifndef TDSQL_MYSQL_SERVER_HANDSHAKE_H
#define TDSQL_MYSQL_SERVER_HANDSHAKE_H
#include "wire.h"
#include <wire/handshake.h>

#define TDSQL_MYSQL_AUTH_PLUGIN "caching_sha2_password"
enum { TDSQL_MYSQL_SSL_BYTES=32,TDSQL_MYSQL_NONCE_BYTES=20,
       TDSQL_MYSQL_AUTH_BYTES=32,TDSQL_MYSQL_USERNAME_BYTES=128,
       TDSQL_MYSQL_DATABASE_BYTES=128,TDSQL_MYSQL_BINARY_CHARSET=63,
       TDSQL_MYSQL_WAIT_SSL=1,TDSQL_MYSQL_WAIT_TLS=2,TDSQL_MYSQL_WAIT_LOGIN=3,
       TDSQL_MYSQL_WAIT_CREDENTIALS=4,TDSQL_MYSQL_HANDSHAKE_FAILED=5 };
typedef struct tdsql_mysql_login_header {
  uint32_t capabilities,max_packet_size;
  uint8_t character_set;
} tdsql_mysql_login_header;
typedef struct tdsql_mysql_login {
  tdsql_mysql_login_header header;
  mysql_wire_bytes_t username,database,auth_response,plugin;
} tdsql_mysql_login;
/* Private single-owner gate. No secrets/views/SQL handles retained. Successful
 * login is still unauthenticated and must pass the separate account boundary. */
typedef struct tdsql_mysql_negotiation {
  tdsql_mysql_login_header header;
  uint32_t advertised;
  int phase;
} tdsql_mysql_negotiation;

uint32_t tdsql_mysql_server_capabilities(bool database_selection);
/* Complete Protocol10 DTO, fixed caching_sha2 plugin and 20-byte CSPRNG nonce.
 * Caller supplies nonce from its crypto provider; this codec generates no entropy.
 * Only implemented flags, binary charset, initial AUTOCOMMIT accepted. Version
 * and plugin arrays must terminate. Same atomic output/measurement as wire.h. */
tdsql_mysql_status tdsql_mysql_greeting_encode(const mysql_wire_greeting_t *,tdsql_mysql_output *);
/* Complete payload only, fixed profile and output unchanged on error.
 * Strict positive max packet, zero filler, binary charset; only advertised bits.
 * Login views borrow input until release, not across asynchronous TLS/auth work. */
tdsql_mysql_status tdsql_mysql_ssl_decode(const uint8_t *,size_t,uint32_t advertised,tdsql_mysql_login_header *);
tdsql_mysql_status tdsql_mysql_login_decode(const uint8_t *,size_t,uint32_t advertised,tdsql_mysql_login *);
/* Initialize after a validated greeting has transferred to the owned transport.
 * TLS support must already be configured. No plaintext credentials admitted. */
tdsql_mysql_status tdsql_mysql_negotiation_init(tdsql_mysql_negotiation *,uint32_t advertised);
/* Sequence is the complete message's first packet ID, not a guessed counter.
 * false tls for SSLRequest, true only from a successful transport TLS event.
 * WAIT_TLS rejects further input; fatal errors latch FAILED. Only login success
 * changes output, after checking it matches the earlier SSLRequest header. */
tdsql_mysql_status tdsql_mysql_negotiation_accept(tdsql_mysql_negotiation *,const uint8_t *,size_t,
    uint8_t sequence,bool tls,tdsql_mysql_login *);
/* Only actual transport TLS establishment calls this, exactly once. Successful
 * SSLRequest alone never reaches WAIT_LOGIN or authorizes a command. */
tdsql_mysql_status tdsql_mysql_negotiation_tls_ready(tdsql_mysql_negotiation *);
#endif
