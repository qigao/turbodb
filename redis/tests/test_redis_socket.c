#include "../redis_socket.h"

#include "tinytest.h"
#include "turbo_error.h"

#include <stdint.h>

suite("redis native socket adapter") {
  before_each() {
    check_equal(redis_socket_platform_init(), TURBO_OK);
  }

  after_each() {
    redis_socket_platform_shutdown();
  }

  it("resolves a numeric hostname into bounded native addresses") {
    redis_socket_address addresses[2];
    size_t count = 0u;
    check_equal(redis_socket_resolve("127.0.0.1", 6379u, addresses, 2u,
                                     &count),
                TURBO_OK);
    check_equal(count, (size_t)1u);
    check_true(addresses[0].length > 0u);
    check_true(addresses[0].family == REDIS_SOCKET_IPV4);
  }

  it("rejects an address set that has no storage") {
    size_t count = 9u;
    check_equal(redis_socket_resolve("127.0.0.1", 6379u, NULL, 0u, &count),
                TURBO_EINVAL);
    check_equal(count, (size_t)0u);
  }

  it("opens a nonblocking socket for a resolved address") {
    redis_socket_address address;
    uintptr_t socket_value = REDIS_SOCKET_INVALID;
    size_t count = 0u;
    check_equal(redis_socket_resolve("127.0.0.1", 6379u, &address, 1u,
                                     &count),
                TURBO_OK);
    check_equal(redis_socket_open(&address, &socket_value), TURBO_OK);
    check_not_equal(socket_value, (uintptr_t)REDIS_SOCKET_INVALID);
    check_equal(redis_socket_close(socket_value), TURBO_OK);
  }
}
