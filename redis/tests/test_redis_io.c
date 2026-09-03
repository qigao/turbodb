#include "../redis_io.h"
#include "../redis_io_internal.h"

#include "tinytest.h"
#include "salts_error.h"

#include <stdint.h>

static cflow_io_native_backend_kind redis_test_backend_kind(void) {
#if defined(_WIN32)
  return CFLOW_IO_NATIVE_IOCP;
#elif defined(__linux__)
  return CFLOW_IO_NATIVE_EPOLL;
#elif defined(__APPLE__)
  return CFLOW_IO_NATIVE_KQUEUE;
#else
  return CFLOW_IO_NATIVE_POLL;
#endif
}

static void redis_test_drive(void *user) { (void)user; }

suite("redis cflow io runtime") {
  it("rejects zero Publisher capacity without publishing a runtime") {
    redis_io_runtime runtime = {0};
    redis_io_runtime_config config = {redis_test_backend_kind(), 0u, 1u};

    check_equal(redis_io_runtime_init(&runtime, &config), SALTS_EINVAL);
    check_false(redis_io_runtime_valid(&runtime));
  }

  it("applies capacity when per-connection Publishers attach") {
    redis_io_runtime runtime = {0};
    redis_io_runtime_config config = {redis_test_backend_kind(), 1u, 1u};
    redis_io_runtime_publisher publisher = {0};
    cflow_io_backend_ops backend = {0};
    void *backend_user = NULL;

    check_equal(redis_io_runtime_init(&runtime, &config), SALTS_OK);
    check_equal(redis_io_runtime_attach_publisher(&runtime, &publisher, redis_test_drive, NULL, &backend,
                                               &backend_user),
                SALTS_OK);
    check_not_null(backend_user);
    {
      redis_io_runtime_publisher rejected = {0};
      check_equal(redis_io_runtime_attach_publisher(&runtime, &rejected, redis_test_drive, NULL,
                                                 &backend, &backend_user),
                  SALTS_ENOBUFS);
    }
    check_equal(redis_io_runtime_close(&runtime), SALTS_EBUSY);

    check_equal(redis_io_runtime_detach_publisher(&publisher), SALTS_OK);
    check_equal(redis_io_runtime_close(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_OK);
    check_false(redis_io_runtime_valid(&runtime));
  }

  it("tracks one active operation for each admitted Publisher") {
    redis_io_runtime runtime = {0};
    redis_io_runtime_config config = {redis_test_backend_kind(), 1u, 1u};
    redis_io_runtime_publisher publisher = {0};
    cflow_io_backend_ops backend = {0};
    void *backend_user = NULL;

    check_equal(redis_io_runtime_init(&runtime, &config), SALTS_OK);
    check_equal(redis_io_runtime_attach_publisher(&runtime, &publisher, redis_test_drive, NULL, &backend,
                                               &backend_user),
                SALTS_OK);
    check_equal(redis_io_runtime_publisher_started(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_publisher_started(&runtime), SALTS_ENOBUFS);
    check_equal(redis_io_runtime_close(&runtime), SALTS_EBUSY);

    redis_io_runtime_publisher_finished(&runtime);
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(1000000000)), SALTS_OK);
    check_equal(redis_io_runtime_detach_publisher(&publisher), SALTS_OK);
    check_equal(redis_io_runtime_close(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_OK);
  }

  it("bounds retiring socket identities until the backend forgets them") {
    static const uintptr_t first_socket = (uintptr_t)1234u;
    static const uintptr_t second_socket = (uintptr_t)5678u;
    redis_io_runtime runtime = {0};
    redis_io_runtime_config config = {redis_test_backend_kind(), 1u, 1u};

    check_equal(redis_io_runtime_init(&runtime, &config), SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, first_socket), SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, first_socket), SALTS_EBUSY);
    check_equal(redis_io_runtime_retire_socket(&runtime, second_socket), SALTS_ENOBUFS);
    check_equal(redis_io_runtime_forget_socket(&runtime, first_socket), SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, second_socket), SALTS_OK);
    check_equal(redis_io_runtime_forget_socket(&runtime, second_socket), SALTS_OK);
    check_equal(redis_io_runtime_close(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_OK);
  }

  it("rejects a reused socket identity while its previous backend lane retires") {
    static const uintptr_t socket_identity = (uintptr_t)1234u;
    redis_io_runtime runtime = {0};
    redis_io_runtime_config config = {redis_test_backend_kind(), 1u, 1u};
    redis_io_runtime_publisher publisher = {0};
    cflow_io_backend_ops backend = {0};
    cflow_io_actor actor = {0};
    void *backend_user = NULL;
    cflow_io_native_operation operation = {.kind = CFLOW_IO_NATIVE_TCP_RECV,
                                           .socket = socket_identity,
                                           .buffer = (void *)&runtime,
                                           .length = 1u};

    check_equal(redis_io_runtime_init(&runtime, &config), SALTS_OK);
    check_equal(redis_io_runtime_attach_publisher(&runtime, &publisher, redis_test_drive, NULL, &backend,
                                               &backend_user),
                SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, socket_identity), SALTS_OK);
    check_equal(backend.submit(backend_user, &actor, 1u, 1u, &operation), SALTS_EBUSY);
    check_equal(redis_io_runtime_forget_socket(&runtime, socket_identity), SALTS_OK);
    check_equal(redis_io_runtime_detach_publisher(&publisher), SALTS_OK);
    check_equal(redis_io_runtime_close(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_OK);
  }

  it("rejects blocking runtime calls from an I/O callback") {
    redis_io_runtime runtime = {0};
    redis_io_runtime_config config = {redis_test_backend_kind(), 1u, 1u};

    check_equal(redis_io_runtime_init(&runtime, &config), SALTS_OK);
    redis_io_runtime_enter_callback();
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(1000000000)), SALTS_EBUSY);
    check_equal(
        redis_io_runtime_forget_socket_wait(&runtime, (uintptr_t)1234u, UINT64_C(1000000000)),
        SALTS_EBUSY);
    redis_io_runtime_leave_callback();
    check_equal(redis_io_runtime_close(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_OK);
  }

  it("requires close before destroy") {
    redis_io_runtime runtime = {0};
    redis_io_runtime_config config = {redis_test_backend_kind(), 1u, 1u};

    check_equal(redis_io_runtime_init(&runtime, &config), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_EBUSY);
    check_true(redis_io_runtime_valid(&runtime));
    check_equal(redis_io_runtime_close(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_OK);
  }
}
