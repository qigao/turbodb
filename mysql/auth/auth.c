#include "auth.h"

#include <cmeta_crypto.h>

#include <string.h>

static mysql_wire_status_t mysql_auth_caching_sha2(
    const char *password, const uint8_t *nonce,
    uint8_t out[MYSQL_AUTH_RESPONSE_CAPACITY]) {
  uint8_t first[32];
  uint8_t second[32];
  uint8_t challenge[32];
  uint8_t challenge_input[32 + MYSQL_AUTH_NONCE_BYTES];
  size_t password_size = strlen(password);
  size_t i;

  if (cmeta_sha256(password, password_size, first) != 0)
    return MYSQL_WIRE_STATUS_INVALID;
  if (cmeta_sha256(first, sizeof(first), second) != 0)
    return MYSQL_WIRE_STATUS_INVALID;

  memcpy(challenge_input, second, sizeof(second));
  memcpy(challenge_input + sizeof(second), nonce, MYSQL_AUTH_NONCE_BYTES);
  if (cmeta_sha256(challenge_input, sizeof(challenge_input), challenge) != 0)
    return MYSQL_WIRE_STATUS_INVALID;

  for (i = 0u; i < sizeof(first); ++i)
    out[i] = (uint8_t)(first[i] ^ challenge[i]);
  return MYSQL_WIRE_STATUS_OK;
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

  if (cmeta_sha1(password, password_size, first) != 0)
    return MYSQL_WIRE_STATUS_INVALID;
  if (cmeta_sha1(first, sizeof(first), second) != 0)
    return MYSQL_WIRE_STATUS_INVALID;

  memcpy(challenge_input, nonce, MYSQL_AUTH_NONCE_BYTES);
  memcpy(challenge_input + MYSQL_AUTH_NONCE_BYTES, second, sizeof(second));
  if (cmeta_sha1(challenge_input, sizeof(challenge_input), challenge) != 0)
    return MYSQL_WIRE_STATUS_INVALID;

  for (i = 0u; i < sizeof(first); ++i)
    out[i] = (uint8_t)(first[i] ^ challenge[i]);
  return MYSQL_WIRE_STATUS_OK;
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
