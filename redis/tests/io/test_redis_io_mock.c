#include "../../redis_io.h"
#include "../../redis_io_internal.h"

#include <salts/clock.h>
#include <tinytest.h>

#include <string.h>

enum { REDIS_IO_FAKE_SCRIPT_CAPACITY = 3u };

typedef struct redis_io_fake_state {
  int forget_results[REDIS_IO_FAKE_SCRIPT_CAPACITY];
  uint64_t times[REDIS_IO_FAKE_SCRIPT_CAPACITY];
  uintptr_t sockets[REDIS_IO_FAKE_SCRIPT_CAPACITY];
  size_t forget_result_count;
  size_t forget_result_index;
  size_t forget_call_count;
  size_t time_count;
  size_t time_index;
  size_t time_call_count;
  int default_forget_result;
  uint64_t default_time;
} redis_io_fake_state;

static redis_io_fake_state redis_io_fake;

static int test_forget_socket(cflow_io_native_backend *backend, uintptr_t socket) {
  (void)backend;
  if (redis_io_fake.forget_call_count < REDIS_IO_FAKE_SCRIPT_CAPACITY)
    redis_io_fake.sockets[redis_io_fake.forget_call_count] = socket;
  ++redis_io_fake.forget_call_count;
  if (redis_io_fake.forget_result_index < redis_io_fake.forget_result_count)
    return redis_io_fake.forget_results[redis_io_fake.forget_result_index++];
  return redis_io_fake.default_forget_result;
}

static uint64_t test_hrtime(void) {
  ++redis_io_fake.time_call_count;
  if (redis_io_fake.time_index < redis_io_fake.time_count)
    return redis_io_fake.times[redis_io_fake.time_index++];
  return redis_io_fake.default_time;
}

static void redis_io_fake_set_forget_results(const int *results, size_t count) {
  check_true(count <= REDIS_IO_FAKE_SCRIPT_CAPACITY);
  memcpy(redis_io_fake.forget_results, results, count * sizeof(*results));
  redis_io_fake.forget_result_count = count;
}

static void redis_io_fake_set_times(const uint64_t *times, size_t count) {
  check_true(count <= REDIS_IO_FAKE_SCRIPT_CAPACITY);
  memcpy(redis_io_fake.times, times, count * sizeof(*times));
  redis_io_fake.time_count = count;
}

static void redis_io_fake_check_sockets(size_t count, uintptr_t expected) {
  size_t index;
  check_equal(redis_io_fake.forget_call_count, count);
  for (index = 0u; index < count; ++index)
    check_equal(redis_io_fake.sockets[index], expected);
}

/* Intercept only retirement and time in this test translation unit. The real
 * runtime owns its bounded slots, executor and backend until teardown. The
 * bounded fake is single-threaded and preserves the per-call return sequences
 * needed to exercise retry and clock-wrap behavior. */
#define cflow_io_native_backend_forget_socket test_forget_socket
#define cmeta_hrtime test_hrtime
#include "../../redis_io.c"
#undef cflow_io_native_backend_forget_socket
#undef cmeta_hrtime

enum { TEST_SOCKET = 1234, NEXT_SOCKET = 5678, TEST_TIMEOUT_NS = 20,
       TEST_START_NS = 100, TEST_TICK_NS = 10 };

spec("Redis socket retirement") {
  (void)ttest_config__;
  static redis_io_runtime runtime;

  before_each() {
    const redis_io_runtime_config config = {redis_io_default_backend_kind(), 1u, 1u};
    runtime = (redis_io_runtime){0};
    redis_io_fake = (redis_io_fake_state){0};
    redis_io_fake.default_forget_result = SALTS_EIO;
    redis_io_fake.default_time = UINT64_MAX;
    check_equal(redis_io_runtime_init(&runtime, &config), SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, TEST_SOCKET), SALTS_OK);
  }

  after_each() {
    check_equal(redis_io_fake.forget_result_index, redis_io_fake.forget_result_count);
    check_equal(redis_io_fake.time_index, redis_io_fake.time_count);
    check_equal(redis_io_runtime_close(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_OK);
    check_false(redis_io_runtime_valid(&runtime));
  }

  it("keeps a busy identity reserved until the backend confirms removal") {
    const int results[] = {SALTS_EBUSY, SALTS_OK};
    redis_io_fake_set_forget_results(results, 2u);
    check_equal(redis_io_runtime_forget_socket(&runtime, TEST_SOCKET), SALTS_EBUSY);
    check_equal(redis_io_runtime_retire_socket(&runtime, TEST_SOCKET), SALTS_EBUSY);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_ENOBUFS);
    check_equal(redis_io_runtime_forget_socket(&runtime, TEST_SOCKET), SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_OK);
    redis_io_fake_check_sockets(2u, TEST_SOCKET);
  }

  it("releases capacity when the backend already forgot the socket") {
    const int results[] = {SALTS_ENOENT};
    redis_io_fake_set_forget_results(results, 1u);
    check_equal(redis_io_runtime_forget_socket(&runtime, TEST_SOCKET), SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_OK);
    redis_io_fake_check_sockets(1u, TEST_SOCKET);
  }

  it("preserves the reservation and native error on backend failure") {
    const int results[] = {SALTS_EIO};
    redis_io_fake_set_forget_results(results, 1u);
    check_equal(redis_io_runtime_forget_socket(&runtime, TEST_SOCKET), SALTS_EIO);
    check_equal(redis_io_runtime_retire_socket(&runtime, TEST_SOCKET), SALTS_EBUSY);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_ENOBUFS);
    redis_io_fake_check_sockets(1u, TEST_SOCKET);
  }

  it("retries transient busy and releases capacity before the deadline") {
    const uint64_t times[] = {TEST_START_NS, TEST_START_NS + TEST_TICK_NS};
    const int results[] = {SALTS_EBUSY, SALTS_OK};
    redis_io_fake_set_times(times, 2u);
    redis_io_fake_set_forget_results(results, 2u);
    check_equal(redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, TEST_TIMEOUT_NS),
                SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_OK);
    redis_io_fake_check_sockets(2u, TEST_SOCKET);
    check_equal(redis_io_fake.time_call_count, (size_t)2u);
  }

  it("times out exactly at the deadline and permits a later cleanup attempt") {
    const uint64_t times[] = {TEST_START_NS, TEST_START_NS + TEST_TICK_NS,
                              TEST_START_NS + TEST_TIMEOUT_NS};
    const int results[] = {SALTS_EBUSY, SALTS_EBUSY, SALTS_OK};
    redis_io_fake_set_times(times, 3u);
    redis_io_fake_set_forget_results(results, 3u);
    check_equal(redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, TEST_TIMEOUT_NS),
                SALTS_ETIMEDOUT);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_ENOBUFS);
    check_equal(redis_io_runtime_forget_socket(&runtime, TEST_SOCKET), SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_OK);
    redis_io_fake_check_sockets(3u, TEST_SOCKET);
    check_equal(redis_io_fake.time_call_count, (size_t)3u);
  }

  it("returns a permanent error without retrying or releasing capacity") {
    const uint64_t times[] = {TEST_START_NS};
    const int results[] = {SALTS_EIO};
    redis_io_fake_set_times(times, 1u);
    redis_io_fake_set_forget_results(results, 1u);
    check_equal(redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, TEST_TIMEOUT_NS),
                SALTS_EIO);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_ENOBUFS);
    redis_io_fake_check_sockets(1u, TEST_SOCKET);
    check_equal(redis_io_fake.time_call_count, (size_t)1u);
  }

  it("rejects blocking cleanup from an I/O callback before touching the backend") {
    int status;
    redis_io_runtime_enter_callback();
    status = redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, TEST_TIMEOUT_NS);
    redis_io_runtime_leave_callback();
    check_equal(status, SALTS_EBUSY);
    check_equal(redis_io_fake.forget_call_count, (size_t)0u);
    check_equal(redis_io_fake.time_call_count, (size_t)0u);
  }

  it("does not release another identity when cleanup reports an unknown socket") {
    const int results[] = {SALTS_ENOENT};
    redis_io_fake_set_forget_results(results, 1u);
    check_equal(redis_io_runtime_forget_socket(&runtime, NEXT_SOCKET), SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, TEST_SOCKET), SALTS_EBUSY);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_ENOBUFS);
    redis_io_fake_check_sockets(1u, NEXT_SOCKET);
  }

  it("stops retrying when a busy backend develops a permanent error") {
    const uint64_t times[] = {TEST_START_NS, TEST_START_NS + TEST_TICK_NS};
    const int results[] = {SALTS_EBUSY, SALTS_EIO};
    redis_io_fake_set_times(times, 2u);
    redis_io_fake_set_forget_results(results, 2u);
    check_equal(redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, TEST_TIMEOUT_NS),
                SALTS_EIO);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_ENOBUFS);
    redis_io_fake_check_sockets(2u, TEST_SOCKET);
    check_equal(redis_io_fake.time_call_count, (size_t)2u);
  }

  it("releases a busy reservation when a retry finds the socket already forgotten") {
    const uint64_t times[] = {TEST_START_NS, TEST_START_NS + TEST_TICK_NS};
    const int results[] = {SALTS_EBUSY, SALTS_ENOENT};
    redis_io_fake_set_times(times, 2u);
    redis_io_fake_set_forget_results(results, 2u);
    check_equal(redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, TEST_TIMEOUT_NS),
                SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_OK);
    redis_io_fake_check_sockets(2u, TEST_SOCKET);
    check_equal(redis_io_fake.time_call_count, (size_t)2u);
  }

  it("measures elapsed timeout across an unsigned clock wrap") {
    const uint64_t started = UINT64_MAX - TEST_TICK_NS;
    const uint64_t times[] = {started, 0u, TEST_TICK_NS - 1u};
    const int results[] = {SALTS_EBUSY, SALTS_EBUSY};
    redis_io_fake_set_times(times, 3u);
    redis_io_fake_set_forget_results(results, 2u);
    check_equal(redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, TEST_TIMEOUT_NS),
                SALTS_ETIMEDOUT);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_ENOBUFS);
    redis_io_fake_check_sockets(2u, TEST_SOCKET);
    check_equal(redis_io_fake.time_call_count, (size_t)3u);
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
    check_equal(redis_io_fake.forget_call_count, (size_t)0u);
    check_equal(redis_io_fake.time_call_count, (size_t)0u);
    {
      const uint64_t times[] = {TEST_START_NS};
      const int results[] = {SALTS_OK};
      redis_io_fake_set_times(times, 1u);
      redis_io_fake_set_forget_results(results, 1u);
    }
    check_equal(redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, TEST_TIMEOUT_NS),
                SALTS_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, NEXT_SOCKET), SALTS_OK);
    redis_io_fake_check_sockets(1u, TEST_SOCKET);
    check_equal(redis_io_fake.time_call_count, (size_t)1u);
  }

  it("rejects a zero timeout without starting backend cleanup") {
    check_equal(redis_io_runtime_forget_socket_wait(&runtime, TEST_SOCKET, 0u), SALTS_EINVAL);
    check_equal(redis_io_fake.forget_call_count, (size_t)0u);
    check_equal(redis_io_fake.time_call_count, (size_t)0u);
  }

  it("rejects a missing runtime without consulting the backend or clock") {
    check_equal(redis_io_runtime_forget_socket_wait(NULL, TEST_SOCKET, TEST_TIMEOUT_NS),
                SALTS_EINVAL);
    check_equal(redis_io_fake.forget_call_count, (size_t)0u);
    check_equal(redis_io_fake.time_call_count, (size_t)0u);
  }
}
