#ifndef TURBODB_MYSQL_AUTH_H
#define TURBODB_MYSQL_AUTH_H

#include "../wire/codec.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MYSQL_AUTH_CACHING_SHA2_PASSWORD "caching_sha2_password"
#define MYSQL_AUTH_NATIVE_PASSWORD "mysql_native_password"
#define MYSQL_AUTH_NONCE_BYTES 20u
#define MYSQL_AUTH_RESPONSE_CAPACITY 32u

mysql_wire_status_t mysql_auth_build_response(
    const char *plugin, const char *password,
    const uint8_t *nonce, size_t nonce_size,
    uint8_t out[MYSQL_AUTH_RESPONSE_CAPACITY], size_t *out_size);

#ifdef __cplusplus
}
#endif

#endif
