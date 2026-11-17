#include "auth.h"

#include <crypto/sha256.h>
#include <crypto/symmetric.h>

#include <string.h>

/* mysql_native_password fixes SHA-1 as part of its wire algorithm. */
static int mysql_auth_sha1(const void *data, size_t size, uint8_t digest[20]) {
  size_t written = 0;
  return salts_crypto_digest(SALTS_CRYPTO_SHA1, data, size, digest, 20,
                             &written) == SALTS_CRYPTO_OK && written == 20;
}

static mysql_wire_status_t mysql_auth_caching_sha2(
    const char *password, const uint8_t *nonce,
    uint8_t out[MYSQL_AUTH_RESPONSE_CAPACITY]) {
  uint8_t first[32];
  uint8_t second[32];
  uint8_t challenge[32];
  uint8_t challenge_input[32 + MYSQL_AUTH_NONCE_BYTES];
  size_t password_size = strlen(password);
  size_t i;
  mysql_wire_status_t status = MYSQL_WIRE_STATUS_INVALID;

  if (salts_crypto_sha256(password, password_size, first) != 0)
    goto done;
  if (salts_crypto_sha256(first, sizeof(first), second) != 0)
    goto done;

  memcpy(challenge_input, second, sizeof(second));
  memcpy(challenge_input + sizeof(second), nonce, MYSQL_AUTH_NONCE_BYTES);
  if (salts_crypto_sha256(challenge_input, sizeof(challenge_input), challenge) != 0)
    goto done;

  for (i = 0u; i < sizeof(first); ++i)
    out[i] = (uint8_t)(first[i] ^ challenge[i]);
  status = MYSQL_WIRE_STATUS_OK;
done:
  salts_crypto_clear(first, sizeof(first));
  salts_crypto_clear(second, sizeof(second));
  salts_crypto_clear(challenge, sizeof(challenge));
  salts_crypto_clear(challenge_input, sizeof(challenge_input));
  return status;
}

static mysql_wire_status_t mysql_auth_native(
    const char *password, const uint8_t *nonce,
    uint8_t out[MYSQL_AUTH_RESPONSE_CAPACITY]) {
  uint8_t first[20];
  uint8_t second[20];
  uint8_t challenge[20];
  uint8_t challenge_input[MYSQL_AUTH_NONCE_BYTES + 20u];
  size_t password_size = strlen(password);
  size_t i;
  mysql_wire_status_t status = MYSQL_WIRE_STATUS_INVALID;

  if (!mysql_auth_sha1(password, password_size, first) ||
      !mysql_auth_sha1(first, sizeof(first), second)) goto done;

  memcpy(challenge_input, nonce, MYSQL_AUTH_NONCE_BYTES);
  memcpy(challenge_input + MYSQL_AUTH_NONCE_BYTES, second, sizeof(second));
  if (!mysql_auth_sha1(challenge_input, sizeof(challenge_input), challenge)) goto done;

  for (i = 0u; i < sizeof(first); ++i)
    out[i] = (uint8_t)(first[i] ^ challenge[i]);
  status = MYSQL_WIRE_STATUS_OK;
done:
  salts_crypto_clear(first, sizeof(first));
  salts_crypto_clear(second, sizeof(second));
  salts_crypto_clear(challenge, sizeof(challenge));
  salts_crypto_clear(challenge_input, sizeof(challenge_input));
  return status;
}

mysql_wire_status_t mysql_auth_build_response(
    const char *plugin, const char *password,
    const uint8_t *nonce, size_t nonce_size,
    uint8_t out[MYSQL_AUTH_RESPONSE_CAPACITY], size_t *out_size) {
  mysql_wire_status_t status;

  if (plugin == NULL || password == NULL || nonce == NULL ||
      out == NULL || out_size == NULL ||
      nonce_size != MYSQL_AUTH_NONCE_BYTES)
    return MYSQL_WIRE_STATUS_INVALID;
  *out_size = 0u;
  memset(out, 0, MYSQL_AUTH_RESPONSE_CAPACITY);

  if (strcmp(plugin, MYSQL_AUTH_CACHING_SHA2_PASSWORD) != 0 &&
      strcmp(plugin, MYSQL_AUTH_NATIVE_PASSWORD) != 0)
    return MYSQL_WIRE_STATUS_INVALID;

  if (password[0] == '\0')
    return MYSQL_WIRE_STATUS_OK;

  if (strcmp(plugin, MYSQL_AUTH_CACHING_SHA2_PASSWORD) == 0) {
    status = mysql_auth_caching_sha2(password, nonce, out);
    if (status == MYSQL_WIRE_STATUS_OK)
      *out_size = 32u;
    return status;
  }
  if (strcmp(plugin, MYSQL_AUTH_NATIVE_PASSWORD) == 0) {
    status = mysql_auth_native(password, nonce, out);
    if (status == MYSQL_WIRE_STATUS_OK)
      *out_size = 20u;
    return status;
  }
  return MYSQL_WIRE_STATUS_INVALID;
}
