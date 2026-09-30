#include <auth/auth.h>
#include <tinytest.h>
#include <string.h>

spec("MySQL authentication boundaries") {
  (void)ttest_config__;

  it("rejects short and long nonces without modifying caller storage") {
    uint8_t nonce[MYSQL_AUTH_NONCE_BYTES + 1u] = {0};
    uint8_t response[MYSQL_AUTH_RESPONSE_CAPACITY];
    uint8_t original[MYSQL_AUTH_RESPONSE_CAPACITY];
    const size_t invalid_sizes[] = {0u, MYSQL_AUTH_NONCE_BYTES - 1u,
                                   MYSQL_AUTH_NONCE_BYTES + 1u};
    memset(original, 0xa5, sizeof(original));
    for (size_t i = 0u; i < sizeof(invalid_sizes) / sizeof(invalid_sizes[0]); ++i) {
      size_t size = sizeof(response);
      memcpy(response, original, sizeof(response));
      check_equal(mysql_auth_build_response(MYSQL_AUTH_CACHING_SHA2_PASSWORD,
                      "secret", nonce, invalid_sizes[i], response, &size),
                  MYSQL_WIRE_STATUS_INVALID);
      check_equal(size, sizeof(response));
      check_equal(memcmp(response, original, sizeof(response)), 0);
    }
  }

  it("rejects an unknown plugin even when the password is empty") {
    uint8_t nonce[MYSQL_AUTH_NONCE_BYTES] = {0};
    uint8_t response[MYSQL_AUTH_RESPONSE_CAPACITY];
    size_t size = sizeof(response);
    check_equal(mysql_auth_build_response("unsupported_auth", "", nonce,
                    sizeof(nonce), response, &size), MYSQL_WIRE_STATUS_INVALID);
    check_equal(size, (size_t)0u);
  }

  it("clears a reused response buffer for both supported empty-password methods") {
    const char *plugins[] = {MYSQL_AUTH_CACHING_SHA2_PASSWORD, MYSQL_AUTH_NATIVE_PASSWORD};
    uint8_t nonce[MYSQL_AUTH_NONCE_BYTES] = {0};
    const uint8_t empty[MYSQL_AUTH_RESPONSE_CAPACITY] = {0};
    for (size_t i = 0u; i < sizeof(plugins) / sizeof(plugins[0]); ++i) {
      uint8_t response[MYSQL_AUTH_RESPONSE_CAPACITY];
      size_t size = sizeof(response);
      memset(response, 0xa5, sizeof(response));
      check_equal(mysql_auth_build_response(plugins[i], "", nonce, sizeof(nonce),
                                            response, &size), MYSQL_WIRE_STATUS_OK);
      check_equal(size, (size_t)0u);
      check_equal(memcmp(response, empty, sizeof(response)), 0);
    }
  }

  it("rejects missing output parameters before hashing") {
    uint8_t nonce[MYSQL_AUTH_NONCE_BYTES] = {0};
    uint8_t response[MYSQL_AUTH_RESPONSE_CAPACITY];
    size_t size = 0u;
    check_equal(mysql_auth_build_response(MYSQL_AUTH_NATIVE_PASSWORD, "secret",
                    nonce, sizeof(nonce), NULL, &size), MYSQL_WIRE_STATUS_INVALID);
    check_equal(mysql_auth_build_response(MYSQL_AUTH_NATIVE_PASSWORD, "secret",
                    nonce, sizeof(nonce), response, NULL), MYSQL_WIRE_STATUS_INVALID);
  }
}
