#include "../../redis_io.h"
#include "../../redis_io_internal.h"

#include <salts/clock.h>
#include <tinymock.h>

TINYMOCk_MOCK(int, test_forget_socket, cflow_io_native_backend *, uintptr_t)
TINYMOCk_MOCK0(uint64_t, test_hrtime)

/* Intercept only retirement and time in this test translation unit. The real
 * runtime owns its bounded slots, executor and backend until teardown. No socket
 * is opened; both mocks are called only by the test thread. */
#define cflow_io_native_backend_forget_socket test_forget_socket
#define salts_hrtime test_hrtime
#include "../../redis_io.c"
#undef cflow_io_native_backend_forget_socket
#undef salts_hrtime

enum { TEST_SOCKET = 1234, NEXT_SOCKET = 5678, TEST_TIMEOUT_NS = 20,
       TEST_START_NS = 100, TEST_TICK_NS = 10 };

spec("Redis socket retirement with TinyMock") {
  (void)ttest_config__;
  static redis_io_runtime runtime;

  before_each() {
    const redis_io_runtime_config config = {redis_io_default_backend_kind(), 1u, 1u};
    runtime = (redis_io_runtime){0};
    mock_test_forget_socket_set_default_return(TINYMOCk_RETURN(SALTS_EIO));
    mock_test_hrtime_set_default_return(TINYMOCk_RETURN(UINT64_MAX));
    check_equal(redis_io_runtime_init(&runtime, &config), SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, TEST_SOCKET), SALTS_OK);
  }

  after_each() {
    mock_test_forget_socket_verify();
    mock_test_hrtime_verify();
    check_equal(redis_io_runtime_close(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_OK);
    check_false(redis_io_runtime_valid(&runtime));
  }

  it("keeps a busy identity reserved until the backend confirms removal") {
    mock_test_forget_socket_expect(TINYMOCk_ANY, TINYMOCk_ARG((uintptr_t)TEST_SOCKET),
                                  TINYMOCk_RETURN(SALTS_EBUSY));
    mock_test_forget_socket_expect(TINYMOCk_ANY, TINYMOCk_ARG((uintptr_t)TEST_SOCKET),
                                  TINYMOCk_RETURN(SALTS_OK));
    check_equal(redis_io_runtime_forget_socket(&runtime, TEST_SOCKET), SALTS_EBUSY);
    check_equal(redis_io_runtime_retire_socket(&runtime, TEST_SOCKET), SALTS_EBUSY);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_ENOBUFS);
    check_equal(redis_io_runtime_forget_socket(&runtime, TEST_SOCKET), SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_OK);
    tinymock_mock_verify_times(&tinymock_test_forget_socket, 2u);
  }

  it("releases capacity when the backend already forgot the socket") {
    mock_test_forget_socket_expect(TINYMOCk_ANY, TINYMOCk_ARG((uintptr_t)TEST_SOCKET),
                                  TINYMOCk_RETURN(SALTS_ENOENT));
    check_equal(redis_io_runtime_forget_socket(&runtime, TEST_SOCKET), SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_OK);
    tinymock_mock_verify_times(&tinymock_test_forget_socket, 1u);
  }

  it("preserves the reservation and native error on backend failure") {
    mock_test_forget_socket_expect(TINYMOCk_ANY, TINYMOCk_ARG((uintptr_t)TEST_SOCKET),
                                  TINYMOCk_RETURN(SALTS_EIO));
    check_equal(redis_io_runtime_forget_socket(&runtime, TEST_SOCKET), SALTS_EIO);
    check_equal(redis_io_runtime_retire_socket(&runtime, TEST_SOCKET), SALTS_EBUSY);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_ENOBUFS);
    tinymock_mock_verify_times(&tinymock_test_forget_socket, 1u);
  }

  it("retries transient busy and releases capacity before the deadline") {
    mock_test_hrtime_expect(TINYMOCk_RETURN((uint64_t)TEST_START_NS));
    mock_test_hrtime_expect(TINYMOCk_RETURN((uint64_t)(TEST_START_NS + TEST_TICK_NS)));
    mock_test_forget_socket_expect(TINYMOCk_ANY, TINYMOCk_ARG((uintptr_t)TEST_SOCKET),
                                  TINYMOCk_RETURN(SALTS_EBUSY));
    mock_test_forget_socket_expect(TINYMOCk_ANY, TINYMOCk_ARG((uintptr_t)TEST_SOCKET),
                                  TINYMOCk_RETURN(SALTS_OK));
    check_equal(redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, TEST_TIMEOUT_NS),
                SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_OK);
    tinymock_mock_verify_times(&tinymock_test_forget_socket, 2u);
    tinymock_mock_verify_times(&tinymock_test_hrtime, 2u);
  }

  it("times out exactly at the deadline and permits a later cleanup attempt") {
    mock_test_hrtime_expect(TINYMOCk_RETURN((uint64_t)TEST_START_NS));
    mock_test_hrtime_expect(TINYMOCk_RETURN((uint64_t)(TEST_START_NS + TEST_TICK_NS)));
    mock_test_hrtime_expect(TINYMOCk_RETURN((uint64_t)(TEST_START_NS + TEST_TIMEOUT_NS)));
    mock_test_forget_socket_expect(TINYMOCk_ANY, TINYMOCk_ARG((uintptr_t)TEST_SOCKET),
                                  TINYMOCk_RETURN(SALTS_EBUSY));
    mock_test_forget_socket_expect(TINYMOCk_ANY, TINYMOCk_ARG((uintptr_t)TEST_SOCKET),
                                  TINYMOCk_RETURN(SALTS_EBUSY));
    mock_test_forget_socket_expect(TINYMOCk_ANY, TINYMOCk_ARG((uintptr_t)TEST_SOCKET),
                                  TINYMOCk_RETURN(SALTS_OK));
    check_equal(redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, TEST_TIMEOUT_NS),
                SALTS_ETIMEDOUT);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_ENOBUFS);
    check_equal(redis_io_runtime_forget_socket(&runtime, TEST_SOCKET), SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_OK);
    tinymock_mock_verify_times(&tinymock_test_forget_socket, 3u);
    tinymock_mock_verify_times(&tinymock_test_hrtime, 3u);
  }

  it("returns a permanent error without retrying or releasing capacity") {
    mock_test_hrtime_expect(TINYMOCk_RETURN((uint64_t)TEST_START_NS));
    mock_test_forget_socket_expect(TINYMOCk_ANY, TINYMOCk_ARG((uintptr_t)TEST_SOCKET),
                                  TINYMOCk_RETURN(SALTS_EIO));
    check_equal(redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, TEST_TIMEOUT_NS),
                SALTS_EIO);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_ENOBUFS);
    tinymock_mock_verify_times(&tinymock_test_forget_socket, 1u);
    tinymock_mock_verify_times(&tinymock_test_hrtime, 1u);
  }

  it("rejects blocking cleanup from an I/O callback before touching the backend") {
    int status;
    redis_io_runtime_enter_callback();
    status = redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, TEST_TIMEOUT_NS);
    redis_io_runtime_leave_callback();
    check_equal(status, SALTS_EBUSY);
    tinymock_mock_verify_never(&tinymock_test_forget_socket);
    tinymock_mock_verify_never(&tinymock_test_hrtime);
  }

  it("does not release another identity when cleanup reports an unknown socket") {
    mock_test_forget_socket_expect(TINYMOCk_ANY, TINYMOCk_ARG((uintptr_t)NEXT_SOCKET),
                                  TINYMOCk_RETURN(SALTS_ENOENT));
    check_equal(redis_io_runtime_forget_socket(&runtime, NEXT_SOCKET), SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, TEST_SOCKET), SALTS_EBUSY);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_ENOBUFS);
    tinymock_mock_verify_times(&tinymock_test_forget_socket, 1u);
  }

  it("stops retrying when a busy backend develops a permanent error") {
    mock_test_hrtime_expect(TINYMOCk_RETURN((uint64_t)TEST_START_NS));
    mock_test_hrtime_expect(TINYMOCk_RETURN((uint64_t)(TEST_START_NS + TEST_TICK_NS)));
    mock_test_forget_socket_expect(TINYMOCk_ANY, TINYMOCk_ARG((uintptr_t)TEST_SOCKET),
                                  TINYMOCk_RETURN(SALTS_EBUSY));
    mock_test_forget_socket_expect(TINYMOCk_ANY, TINYMOCk_ARG((uintptr_t)TEST_SOCKET),
                                  TINYMOCk_RETURN(SALTS_EIO));
    check_equal(redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, TEST_TIMEOUT_NS),
                SALTS_EIO);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_ENOBUFS);
    tinymock_mock_verify_times(&tinymock_test_forget_socket, 2u);
    tinymock_mock_verify_times(&tinymock_test_hrtime, 2u);
  }

  it("releases a busy reservation when a retry finds the socket already forgotten") {
    mock_test_hrtime_expect(TINYMOCk_RETURN((uint64_t)TEST_START_NS));
    mock_test_hrtime_expect(TINYMOCk_RETURN((uint64_t)(TEST_START_NS + TEST_TICK_NS)));
    mock_test_forget_socket_expect(TINYMOCk_ANY, TINYMOCk_ARG((uintptr_t)TEST_SOCKET),
                                  TINYMOCk_RETURN(SALTS_EBUSY));
    mock_test_forget_socket_expect(TINYMOCk_ANY, TINYMOCk_ARG((uintptr_t)TEST_SOCKET),
                                  TINYMOCk_RETURN(SALTS_ENOENT));
    check_equal(redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, TEST_TIMEOUT_NS),
                SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_OK);
    tinymock_mock_verify_times(&tinymock_test_forget_socket, 2u);
    tinymock_mock_verify_times(&tinymock_test_hrtime, 2u);
  }

  it("measures elapsed timeout across an unsigned clock wrap") {
    const uint64_t started = UINT64_MAX - TEST_TICK_NS;
    mock_test_hrtime_expect(TINYMOCk_RETURN(started));
    mock_test_hrtime_expect(TINYMOCk_RETURN((uint64_t)0u));
    mock_test_hrtime_expect(TINYMOCk_RETURN((uint64_t)(TEST_TICK_NS - 1u)));
    mock_test_forget_socket_expect(TINYMOCk_ANY, TINYMOCk_ARG((uintptr_t)TEST_SOCKET),
                                  TINYMOCk_RETURN(SALTS_EBUSY));
    mock_test_forget_socket_expect(TINYMOCk_ANY, TINYMOCk_ARG((uintptr_t)TEST_SOCKET),
                                  TINYMOCk_RETURN(SALTS_EBUSY));
    check_equal(redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, TEST_TIMEOUT_NS),
                SALTS_ETIMEDOUT);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_ENOBUFS);
    tinymock_mock_verify_times(&tinymock_test_forget_socket, 2u);
    tinymock_mock_verify_times(&tinymock_test_hrtime, 3u);
  }

  it("keeps blocking cleanup forbidden until the outer callback returns") {
    int nested_status;
    int outer_status;
    redis_io_runtime_enter_callback();
    redis_io_runtime_enter_callback();
    nested_status = redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, TEST_TIMEOUT_NS);
    redis_io_runtime_leave_callback();
    outer_status = redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, TEST_TIMEOUT_NS);
    redis_io_runtime_leave_callback();
    check_equal(nested_status, SALTS_EBUSY);
    check_equal(outer_status, SALTS_EBUSY);
    check_false(redis_io_runtime_in_callback());
    tinymock_mock_verify_never(&tinymock_test_forget_socket);
    tinymock_mock_verify_never(&tinymock_test_hrtime);
    mock_test_hrtime_expect(TINYMOCk_RETURN((uint64_t)TEST_START_NS));
    mock_test_forget_socket_expect(TINYMOCk_ANY, TINYMOCk_ARG((uintptr_t)TEST_SOCKET),
                                  TINYMOCk_RETURN(SALTS_OK));
    check_equal(redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, TEST_TIMEOUT_NS),
                SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_OK);
    tinymock_mock_verify_times(&tinymock_test_forget_socket, 1u);
    tinymock_mock_verify_times(&tinymock_test_hrtime, 1u);
  }

  it("rejects a zero timeout without starting backend cleanup") {
    check_equal(redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, 0u), SALTS_EINVAL);
    tinymock_mock_verify_never(&tinymock_test_forget_socket);
    tinymock_mock_verify_never(&tinymock_test_hrtime);
  }

  it("rejects a missing runtime without consulting the backend or clock") {
    check_equal(redis_io_runtime_forget_socket_wait(NULL, TEST_SOCKET, TEST_TIMEOUT_NS),
                SALTS_EINVAL);
    tinymock_mock_verify_never(&tinymock_test_forget_socket);
    tinymock_mock_verify_never(&tinymock_test_hrtime);
  }
}
