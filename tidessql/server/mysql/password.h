#ifndef TDSQL_MYSQL_SERVER_PASSWORD_H
#define TDSQL_MYSQL_SERVER_PASSWORD_H
#include "handshake.h"

enum { TDSQL_MYSQL_PASSWORD_BYTES=1024,TDSQL_MYSQL_PASSWORD_SALT_BYTES=16,
       TDSQL_MYSQL_PASSWORD_HASH_BYTES=32,TDSQL_MYSQL_PASSWORD_MIN_ITERATIONS=600000,
       TDSQL_MYSQL_PASSWORD_MAX_ITERATIONS=2000000 };
/* Private owned scalar verifier, immutable while any session borrows it.
 * No persistence format, account authorization or fast-auth cache here. */
typedef struct tdsql_mysql_password_record {
  uint32_t iterations;
  uint8_t salt[TDSQL_MYSQL_PASSWORD_SALT_BYTES],hash[TDSQL_MYSQL_PASSWORD_HASH_BYTES];
} tdsql_mysql_password_record;
/* Nonempty bounded bytes without embedded NUL. Random salt/KDF use GmSSL;
 * out is unchanged on failure. Caller owns and securely clears password/record.
 * Iterations must be within the named range, not downgraded for tests or load. */
turbodb_status_t tdsql_mysql_password_make(vstr,uint32_t iterations,tdsql_mysql_password_record *,turbodb_error_t *);
/* Complete caching_sha2 full-auth payload: password + exactly one final NUL.
 * tls true only from successful transport TLS establishment. Input borrows
 * this call; caller wipes owned receive storage afterward. Failure preserves
 * verified; successful constant-time mismatch sets false, not an error. */
turbodb_status_t tdsql_mysql_password_verify(const tdsql_mysql_password_record *,const uint8_t *,size_t,
    bool tls,bool *verified,turbodb_error_t *);
/* Protocol full-auth request payload (01 04), atomic bounded output. This is
 * the first-version policy for every login, not a failed-fast-auth fallback. */
tdsql_mysql_status tdsql_mysql_full_auth_encode(tdsql_mysql_output *);
#endif
