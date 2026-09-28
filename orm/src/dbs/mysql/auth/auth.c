#include "auth.h"

#include <openssl/evp.h>

#include <string.h>

static mysql_wire_status_t mysql_auth_digest(
    const EVP_MD *md, const void *first, size_t first_size,
    const void *second, size_t second_size,
    uint8_t *out, unsigned int expected_size) {
  EVP_MD_CTX *context;
  unsigned int actual_size = 0u;
  int ok = 1;

  if (md == NULL || out == NULL ||
      (first == NULL && first_size != 0u) ||
      (second == NULL && second_size != 0u))
    return MYSQL_WIRE_STATUS_INVALID;

  context = EVP_MD_CTX_new();
  if (context == NULL)
    return MYSQL_WIRE_STATUS_LIMIT;
  ok = EVP_DigestInit_ex(context, md, NULL);
  if (ok == 1 && first_size != 0u)
    ok = EVP_DigestUpdate(context, first, first_size);
  if (ok == 1 && second_size != 0u)
    ok = EVP_DigestUpdate(context, second, second_size);
  if (ok == 1)
    ok = EVP_DigestFinal_ex(context, out, &actual_size);
  EVP_MD_CTX_free(context);

  return ok == 1 && actual_size == expected_size
             ? MYSQL_WIRE_STATUS_OK
             : MYSQL_WIRE_STATUS_INVALID;
}

static mysql_wire_status_t mysql_auth_caching_sha2(
    const char *password, const uint8_t *nonce,
    uint8_t out[MYSQL_AUTH_RESPONSE_CAPACITY]) {
  uint8_t first[32];
  uint8_t second[32];
  uint8_t challenge[32];
  size_t password_size = strlen(password);
  size_t i;
  mysql_wire_status_t status;

  status = mysql_auth_digest(EVP_sha256(), password, password_size,
                             NULL, 0u, first, 32u);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_auth_digest(EVP_sha256(), first, sizeof(first),
                             NULL, 0u, second, 32u);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_auth_digest(EVP_sha256(), second, sizeof(second),
                             nonce, MYSQL_AUTH_NONCE_BYTES,
                             challenge, 32u);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

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
  size_t password_size = strlen(password);
  size_t i;
  mysql_wire_status_t status;

  status = mysql_auth_digest(EVP_sha1(), password, password_size,
                             NULL, 0u, first, 20u);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_auth_digest(EVP_sha1(), first, sizeof(first),
                             NULL, 0u, second, 20u);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;
  status = mysql_auth_digest(EVP_sha1(), nonce, MYSQL_AUTH_NONCE_BYTES,
                             second, sizeof(second), challenge, 20u);
  if (status != MYSQL_WIRE_STATUS_OK)
    return status;

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
