#include <auth/auth.h>
#include <tinytest.h>
#include <string.h>

spec("MySQL authentication boundaries") {
  (void)ttest_config__;

  it("preserves the independent SHA-256 and SHA-1 protocol vectors with Salts") {
    const uint8_t nonce[MYSQL_AUTH_NONCE_BYTES] = {
      '1','2','3','4','5','6','7','8','9','0','1','2','3','4','5','6','7','8','9','0'};
    const uint8_t sha2[32] = {
      0x51,0xec,0xd6,0xde,0xdb,0xd3,0x4d,0x54,0x45,0xc0,0xa1,0x90,0xd4,0xf5,0x1a,0xcf,
      0x0d,0x23,0xb9,0x4d,0xb6,0x6c,0x91,0xf3,0xf7,0x89,0xfa,0xa9,0x19,0x37,0x51,0xcd};
    const uint8_t sha1[20] = {
      0x0f,0x8b,0x90,0x33,0xe0,0x89,0x7c,0x0a,0x83,0x38,
      0xeb,0xe3,0xde,0xa9,0x01,0x0d,0xda,0x47,0xab,0x56};
    uint8_t response[MYSQL_AUTH_RESPONSE_CAPACITY]; size_t size=0;
    check_equal(mysql_auth_build_response(MYSQL_AUTH_CACHING_SHA2_PASSWORD,"secret",
      nonce,sizeof(nonce),response,&size),MYSQL_WIRE_STATUS_OK);
    check_equal(size,sizeof(sha2)); check_equal(response,sha2,sizeof(sha2));
    memset(response,0xa5,sizeof(response));
    check_equal(mysql_auth_build_response(MYSQL_AUTH_NATIVE_PASSWORD,"secret",
      nonce,sizeof(nonce),response,&size),MYSQL_WIRE_STATUS_OK);
    check_equal(size,sizeof(sha1)); check_equal(response,sha1,sizeof(sha1));
    const uint8_t zero[MYSQL_AUTH_RESPONSE_CAPACITY]={0};
    check_equal(response+sizeof(sha1),zero,sizeof(response)-sizeof(sha1));
  }

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
