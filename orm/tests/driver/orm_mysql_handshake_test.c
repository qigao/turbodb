#include "auth.h"
#include "handshake.h"

#include <tinytest.h>

#include <stdint.h>
#include <string.h>

static const uint8_t mysql_test_nonce[MYSQL_AUTH_NONCE_BYTES] = {
    '1','2','3','4','5','6','7','8','9','0',
    '1','2','3','4','5','6','7','8','9','0'};

spec("mysql handshake v10") {
  (void)ttest_config__;

  it("parses capabilities, scramble and plugin without borrowing greeting storage") {
    static const uint8_t payload[] = {
      0x0a, '8','.','0','.','4','0',0x00,
      0x04,0x03,0x02,0x01,
      '1','2','3','4','5','6','7','8',0x00,
      0x09,0x8a,
      0x2d,
      0x02,0x00,
      0x08,0x01,
      0x15,
      0,0,0,0,0,0,0,0,0,0,
      '9','0','1','2','3','4','5','6','7','8','9','0',0x00,
      'c','a','c','h','i','n','g','_','s','h','a','2','_',
      'p','a','s','s','w','o','r','d',0x00
    };
    mysql_wire_greeting_t greeting;

    check_equal(mysql_wire_parse_greeting(payload, sizeof(payload), &greeting),
                MYSQL_WIRE_STATUS_OK);
    check_equal(greeting.protocol_version, UINT8_C(10));
    check_equal(strcmp(greeting.server_version, "8.0.40"), 0);
    check_equal(greeting.connection_id, UINT32_C(0x01020304));
    check_true((greeting.capabilities & MYSQL_WIRE_CLIENT_PROTOCOL_41) != 0u);
    check_true((greeting.capabilities & MYSQL_WIRE_CLIENT_SSL) != 0u);
    check_true((greeting.capabilities & MYSQL_WIRE_CLIENT_SECURE_CONNECTION) != 0u);
    check_true((greeting.capabilities & MYSQL_WIRE_CLIENT_PLUGIN_AUTH) != 0u);
    check_true((greeting.capabilities & MYSQL_WIRE_CLIENT_DEPRECATE_EOF) != 0u);
    check_equal(greeting.character_set, UINT8_C(0x2d));
    check_equal(greeting.status_flags, UINT16_C(2));
    check_equal(greeting.auth_plugin_data_length, (size_t)20u);
    check_equal(memcmp(greeting.auth_plugin_data, mysql_test_nonce,
                       MYSQL_AUTH_NONCE_BYTES), 0);
    check_equal(strcmp(greeting.auth_plugin, MYSQL_AUTH_CACHING_SHA2_PASSWORD), 0);
  }

  it("selects only supported server capabilities and requires TLS when requested") {
    mysql_wire_greeting_t greeting = {0};
    uint32_t caps = 0u;
    greeting.capabilities =
        MYSQL_WIRE_CLIENT_PROTOCOL_41 |
        MYSQL_WIRE_CLIENT_SSL |
        MYSQL_WIRE_CLIENT_SECURE_CONNECTION |
        MYSQL_WIRE_CLIENT_PLUGIN_AUTH |
        MYSQL_WIRE_CLIENT_TRANSACTIONS |
        MYSQL_WIRE_CLIENT_DEPRECATE_EOF;

    check_equal(mysql_wire_select_client_capabilities(
                    &greeting, true, false, &caps),
                MYSQL_WIRE_STATUS_OK);
    check_true((caps & MYSQL_WIRE_CLIENT_SSL) != 0u);
    check_true((caps & MYSQL_WIRE_CLIENT_DEPRECATE_EOF) != 0u);

    greeting.capabilities &= ~MYSQL_WIRE_CLIENT_SSL;
    check_equal(mysql_wire_select_client_capabilities(
                    &greeting, true, false, &caps),
                MYSQL_WIRE_STATUS_INVALID);
    check_equal(caps, UINT32_C(0));
  }

  it("builds the exact 32-byte SSLRequest prefix") {
    uint8_t request[32] = {0};
    size_t size = 0u;
    size_t offset = 0u;
    uint32_t caps = 0u;
    uint32_t max_packet = 0u;

    check_equal(mysql_wire_build_ssl_request(
                    MYSQL_WIRE_CLIENT_PROTOCOL_41 |
                    MYSQL_WIRE_CLIENT_SSL |
                    MYSQL_WIRE_CLIENT_SECURE_CONNECTION |
                    MYSQL_WIRE_CLIENT_PLUGIN_AUTH,
                    UINT32_C(0x01000000), UINT8_C(0x2d),
                    request, sizeof(request), &size),
                MYSQL_WIRE_STATUS_OK);
    check_equal(size, (size_t)32u);
    check_equal(mysql_wire_read_u32_le(
                    request, size, &offset, &caps),
                MYSQL_WIRE_STATUS_OK);
    check_true((caps & MYSQL_WIRE_CLIENT_SSL) != 0u);
    check_equal(mysql_wire_read_u32_le(
                    request, size, &offset, &max_packet),
                MYSQL_WIRE_STATUS_OK);
    check_equal(max_packet, UINT32_C(0x01000000));
    check_equal(request[8], UINT8_C(0x2d));
  }

  it("encodes secure-connection auth bytes and plugin identity") {
    uint8_t response[256] = {0};
    uint8_t auth[32] = {0};
    size_t auth_size = 0u;
    size_t size = 0u;
    const uint32_t caps =
        MYSQL_WIRE_CLIENT_PROTOCOL_41 |
        MYSQL_WIRE_CLIENT_SECURE_CONNECTION |
        MYSQL_WIRE_CLIENT_PLUGIN_AUTH;

    check_equal(mysql_auth_build_response(
                    MYSQL_AUTH_CACHING_SHA2_PASSWORD, "secret",
                    mysql_test_nonce, sizeof(mysql_test_nonce),
                    auth, &auth_size),
                MYSQL_WIRE_STATUS_OK);
    check_equal(mysql_wire_build_handshake_response(
                    caps, UINT32_C(0x01000000), UINT8_C(0x2d),
                    "turbodb", auth, auth_size, NULL,
                    MYSQL_AUTH_CACHING_SHA2_PASSWORD,
                    response, sizeof(response), &size),
                MYSQL_WIRE_STATUS_OK);
    check_true(size > 32u);
    check_equal(memcmp(response + 32u, "turbodb", 7u), 0);
    check_equal(response[39], UINT8_C(0));
    check_equal(response[40], UINT8_C(32));
    check_equal(memcmp(response + size - strlen(MYSQL_AUTH_CACHING_SHA2_PASSWORD) - 1u,
                       MYSQL_AUTH_CACHING_SHA2_PASSWORD,
                       strlen(MYSQL_AUTH_CACHING_SHA2_PASSWORD)), 0);
  }

  it("parses bounded AuthSwitchRequest data") {
    static const uint8_t payload[] = {
      0xfe,
      'm','y','s','q','l','_','n','a','t','i','v','e','_',
      'p','a','s','s','w','o','r','d',0x00,
      '1','2','3','4','5','6','7','8','9','0',
      '1','2','3','4','5','6','7','8','9','0',0x00
    };
    mysql_wire_auth_switch_t request;

    check_equal(mysql_wire_parse_auth_switch(
                    payload, sizeof(payload), &request),
                MYSQL_WIRE_STATUS_OK);
    check_equal(strcmp(request.plugin, MYSQL_AUTH_NATIVE_PASSWORD), 0);
    check_equal(request.data_length, (size_t)20u);
    check_equal(memcmp(request.data, mysql_test_nonce, 20u), 0);
  }
}

spec("mysql authentication scrambles") {
  (void)ttest_config__;

  it("matches the caching_sha2_password SHA-256 challenge vector") {
    static const uint8_t expected[32] = {
      0x51,0xec,0xd6,0xde,0xdb,0xd3,0x4d,0x54,
      0x45,0xc0,0xa1,0x90,0xd4,0xf5,0x1a,0xcf,
      0x0d,0x23,0xb9,0x4d,0xb6,0x6c,0x91,0xf3,
      0xf7,0x89,0xfa,0xa9,0x19,0x37,0x51,0xcd
    };
    uint8_t actual[MYSQL_AUTH_RESPONSE_CAPACITY] = {0};
    size_t size = 0u;

    check_equal(mysql_auth_build_response(
                    MYSQL_AUTH_CACHING_SHA2_PASSWORD, "secret",
                    mysql_test_nonce, sizeof(mysql_test_nonce),
                    actual, &size),
                MYSQL_WIRE_STATUS_OK);
    check_equal(size, sizeof(expected));
    check_equal(memcmp(actual, expected, sizeof(expected)), 0);
  }

  it("matches the mysql_native_password SHA-1 compatibility vector") {
    static const uint8_t expected[20] = {
      0x0f,0x8b,0x90,0x33,0xe0,0x89,0x7c,0x0a,0x83,0x38,
      0xeb,0xe3,0xde,0xa9,0x01,0x0d,0xda,0x47,0xab,0x56
    };
    uint8_t actual[MYSQL_AUTH_RESPONSE_CAPACITY] = {0};
    size_t size = 0u;

    check_equal(mysql_auth_build_response(
                    MYSQL_AUTH_NATIVE_PASSWORD, "secret",
                    mysql_test_nonce, sizeof(mysql_test_nonce),
                    actual, &size),
                MYSQL_WIRE_STATUS_OK);
    check_equal(size, sizeof(expected));
    check_equal(memcmp(actual, expected, sizeof(expected)), 0);
  }

  it("uses an empty auth response for an empty password") {
    uint8_t actual[MYSQL_AUTH_RESPONSE_CAPACITY] = {0xff};
    size_t size = SIZE_MAX;
    check_equal(mysql_auth_build_response(
                    MYSQL_AUTH_CACHING_SHA2_PASSWORD, "",
                    mysql_test_nonce, sizeof(mysql_test_nonce),
                    actual, &size),
                MYSQL_WIRE_STATUS_OK);
    check_equal(size, (size_t)0u);
  }

  it("rejects unknown plugins rather than silently falling back") {
    uint8_t actual[MYSQL_AUTH_RESPONSE_CAPACITY] = {0};
    size_t size = 0u;
    check_equal(mysql_auth_build_response(
                    "unknown_auth", "secret",
                    mysql_test_nonce, sizeof(mysql_test_nonce),
                    actual, &size),
                MYSQL_WIRE_STATUS_INVALID);
  }
}
