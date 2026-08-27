#include "../redis_io.h"

#include "tinytest.h"
#include "turbo_error.h"
#include "turbo_thread.h"

#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET redis_test_socket;
#define REDIS_TEST_INVALID_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int redis_test_socket;
#define REDIS_TEST_INVALID_SOCKET (-1)
#endif

typedef struct redis_io_wake_probe {
  volatile int wakes;
} redis_io_wake_probe;

typedef struct redis_io_blocking_waker {
  turbo_mutex_t gate;
  turbo_cond_t changed;
  cflow_waitable waitable;
  int entered;
  int release;
  int cancel_started;
  int cancel_done;
} redis_io_blocking_waker;

typedef struct redis_io_wait_thread {
  redis_io_runtime *runtime;
  int status;
} redis_io_wait_thread;

typedef struct redis_io_resubmit_waker {
  redis_io_runtime *runtime;
  redis_io_request *completed_request;
  cflow_io_native_operation next_operation;
  redis_io_request next_request;
  redis_io_request_poll_status poll_status;
  int acknowledge_status;
  redis_io_submit_status submit_status;
} redis_io_resubmit_waker;

typedef struct redis_io_destroy_waker {
  redis_io_runtime *runtime;
  redis_io_request *request;
  redis_io_request_poll_status poll_status;
  int acknowledge_status;
  int close_status;
  int destroy_status;
} redis_io_destroy_waker;

typedef struct redis_io_wait_idle_waker {
  redis_io_runtime *runtime;
  redis_io_request *request;
  redis_io_request_poll_status poll_status;
  int acknowledge_status;
  int wait_idle_status;
} redis_io_wait_idle_waker;

typedef struct redis_io_reuse_race {
  turbo_mutex_t gate;
  turbo_cond_t changed;
  redis_io_runtime *runtime;
  redis_io_request *completed_request;
  cflow_io_native_operation next_operation;
  redis_io_request next_request;
  int entered;
  int proceed;
  int cancel_started;
  int resubmitted;
  int release_after_submit;
  int block_after_submit;
  redis_io_request_poll_status poll_status;
  int acknowledge_status;
  redis_io_submit_status submit_status;
  int reuse_completed_request;
} redis_io_reuse_race;

typedef struct redis_io_cancel_race {
  redis_io_reuse_race *state;
  redis_io_request *request;
  cflow_waitable waitable;
  int use_waitable;
  int done;
  cflow_io_cancel_status status;
} redis_io_cancel_race;

static void redis_test_wake(void *user) {
  redis_io_wake_probe *probe = (redis_io_wake_probe *)user;
  ++probe->wakes;
}

static void redis_test_blocking_wake(void *user) {
  redis_io_blocking_waker *state = (redis_io_blocking_waker *)user;
  turbo_mutex_lock(&state->gate);
  state->entered = 1;
  turbo_cond_broadcast(&state->changed);
  while (!state->release) turbo_cond_wait(&state->changed, &state->gate);
  turbo_mutex_unlock(&state->gate);
}

static void redis_test_drive_until_idle(void *user) {
  redis_io_wait_thread *state = (redis_io_wait_thread *)user;
  state->status = redis_io_runtime_wait_idle(state->runtime,
                                             UINT64_C(5000000000));
}

static void redis_test_cancel_waitable(void *user) {
  redis_io_blocking_waker *state = (redis_io_blocking_waker *)user;
  turbo_mutex_lock(&state->gate);
  state->cancel_started = 1;
  turbo_cond_broadcast(&state->changed);
  turbo_mutex_unlock(&state->gate);
  cflow_waitable_cancel(&state->waitable);
  turbo_mutex_lock(&state->gate);
  state->cancel_done = 1;
  turbo_cond_broadcast(&state->changed);
  turbo_mutex_unlock(&state->gate);
}

static void redis_test_acknowledge_and_resubmit(void *user) {
  redis_io_resubmit_waker *state = (redis_io_resubmit_waker *)user;
  cflow_io_completion completion = {0};
  state->poll_status =
      redis_io_request_poll(state->completed_request, &completion);
  state->acknowledge_status =
      redis_io_request_acknowledge(state->completed_request);
  state->submit_status = redis_io_runtime_try_submit(
      state->runtime, 22u, &state->next_operation, &state->next_request);
}

static void redis_test_close_and_destroy_from_waker(void *user) {
  redis_io_destroy_waker *state = (redis_io_destroy_waker *)user;
  cflow_io_completion completion = {0};
  state->poll_status = redis_io_request_poll(state->request, &completion);
  state->acknowledge_status = redis_io_request_acknowledge(state->request);
  state->close_status = redis_io_runtime_close(state->runtime);
  state->destroy_status = redis_io_runtime_destroy(state->runtime);
}

static void redis_test_wait_idle_from_waker(void *user) {
  redis_io_wait_idle_waker *state = (redis_io_wait_idle_waker *)user;
  cflow_io_completion completion = {0};
  state->poll_status = redis_io_request_poll(state->request, &completion);
  state->acknowledge_status = redis_io_request_acknowledge(state->request);
  state->wait_idle_status = redis_io_runtime_wait_idle(
      state->runtime, UINT64_C(10000000));
}

static void redis_test_block_then_resubmit(void *user) {
  redis_io_reuse_race *state = (redis_io_reuse_race *)user;
  cflow_io_completion completion = {0};
  turbo_mutex_lock(&state->gate);
  state->entered = 1;
  turbo_cond_broadcast(&state->changed);
  while (!state->proceed) turbo_cond_wait(&state->changed, &state->gate);
  turbo_mutex_unlock(&state->gate);
  state->poll_status =
      redis_io_request_poll(state->completed_request, &completion);
  state->acknowledge_status =
      redis_io_request_acknowledge(state->completed_request);
  state->submit_status = redis_io_runtime_try_submit(
      state->runtime, 42u, &state->next_operation,
      state->reuse_completed_request ? state->completed_request
                                     : &state->next_request);
  turbo_mutex_lock(&state->gate);
  state->resubmitted = 1;
  turbo_cond_broadcast(&state->changed);
  while (state->block_after_submit && !state->release_after_submit)
    turbo_cond_wait(&state->changed, &state->gate);
  turbo_mutex_unlock(&state->gate);
}

static void redis_test_cancel_stale_request(void *user) {
  redis_io_cancel_race *cancel = (redis_io_cancel_race *)user;
  turbo_mutex_lock(&cancel->state->gate);
  ++cancel->state->cancel_started;
  turbo_cond_broadcast(&cancel->state->changed);
  turbo_mutex_unlock(&cancel->state->gate);
  if (cancel->use_waitable) {
    cflow_waitable_cancel(&cancel->waitable);
    cancel->status = CFLOW_IO_CANCEL_INVALID_ARGUMENT;
  } else {
    cancel->status = redis_io_request_cancel(cancel->request);
  }
  turbo_mutex_lock(&cancel->state->gate);
  cancel->done = 1;
  turbo_cond_broadcast(&cancel->state->changed);
  turbo_mutex_unlock(&cancel->state->gate);
}

static void redis_test_close_socket(redis_test_socket socket_value) {
  if (socket_value == REDIS_TEST_INVALID_SOCKET) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static int redis_test_forget_socket(redis_io_runtime *runtime,
                                    uintptr_t socket_value) {
  return redis_io_runtime_forget_socket_wait(runtime, socket_value,
                                             UINT64_C(5000000000));
}

static int redis_test_socket_error(void) {
#if defined(_WIN32)
  return -WSAGetLastError();
#else
  return -errno;
#endif
}

static int redis_test_set_nonblocking(redis_test_socket socket_value) {
#if defined(_WIN32)
  u_long enabled = 1u;
  return ioctlsocket(socket_value, FIONBIO, &enabled) == 0
             ? TURBO_OK
             : redis_test_socket_error();
#else
  int flags = fcntl(socket_value, F_GETFL);
  if (flags < 0) return -errno;
  return fcntl(socket_value, F_SETFL, flags | O_NONBLOCK) == 0
             ? TURBO_OK
             : -errno;
#endif
}

static int redis_test_socket_pair(redis_test_socket sockets[2]) {
  redis_test_socket listener = REDIS_TEST_INVALID_SOCKET;
  struct sockaddr_in address;
#if defined(_WIN32)
  int address_size = (int)sizeof(address);
#else
  socklen_t address_size = (socklen_t)sizeof(address);
#endif
  int status = TURBO_OK;
  sockets[0] = REDIS_TEST_INVALID_SOCKET;
  sockets[1] = REDIS_TEST_INVALID_SOCKET;
  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == REDIS_TEST_INVALID_SOCKET) return redis_test_socket_error();
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listener, (const struct sockaddr *)&address,
           (int)sizeof(address)) != 0 ||
      getsockname(listener, (struct sockaddr *)&address, &address_size) != 0 ||
      listen(listener, 1) != 0)
    status = redis_test_socket_error();
  if (status == TURBO_OK) {
    sockets[0] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockets[0] == REDIS_TEST_INVALID_SOCKET)
      status = redis_test_socket_error();
  }
  if (status == TURBO_OK &&
      connect(sockets[0], (const struct sockaddr *)&address,
              (int)sizeof(address)) != 0)
    status = redis_test_socket_error();
  if (status == TURBO_OK) {
    sockets[1] = accept(listener, NULL, NULL);
    if (sockets[1] == REDIS_TEST_INVALID_SOCKET)
      status = redis_test_socket_error();
  }
  redis_test_close_socket(listener);
  if (status == TURBO_OK) status = redis_test_set_nonblocking(sockets[0]);
  if (status == TURBO_OK) status = redis_test_set_nonblocking(sockets[1]);
  if (status != TURBO_OK) {
    redis_test_close_socket(sockets[0]);
    redis_test_close_socket(sockets[1]);
    sockets[0] = REDIS_TEST_INVALID_SOCKET;
    sockets[1] = REDIS_TEST_INVALID_SOCKET;
  }
  return status;
}

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

suite("redis cflow io runtime") {
  it("rejects zero capacity without publishing a runtime") {
    redis_io_runtime runtime = {0};
    redis_io_runtime_config config = {
        redis_test_backend_kind(), 0u, 1u, 1u};
    check_equal(redis_io_runtime_init(&runtime, &config), TURBO_EINVAL);
    check_false(redis_io_runtime_valid(&runtime));
  }

  it("rejects admission while a socket identity is retiring") {
    static const uintptr_t socket_identity = (uintptr_t)1234u;
    unsigned char received = 0u;
    redis_io_runtime runtime = {0};
    redis_io_runtime_config config = {
        redis_test_backend_kind(), 1u, 2u, 1u};
    redis_io_request request = {0};
    cflow_io_native_operation operation = {
        .kind = CFLOW_IO_NATIVE_TCP_RECV,
        .socket = socket_identity,
        .buffer = &received,
        .length = 1u};

    check_equal(redis_io_runtime_init(&runtime, &config), TURBO_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, socket_identity),
                TURBO_OK);
    check_equal(redis_io_runtime_retire_socket(&runtime, socket_identity),
                TURBO_EBUSY);
    check_equal(redis_io_runtime_try_submit(&runtime, 1u, &operation, &request),
                REDIS_IO_SUBMIT_LEASE_IN_USE);
    check_equal(redis_test_forget_socket(&runtime, socket_identity), TURBO_OK);
    check_equal(redis_io_runtime_close(&runtime), TURBO_OK);
    check_equal(redis_io_runtime_destroy(&runtime), TURBO_OK);
  }

  it("wakes and publishes one completion for each accepted operation") {
    static const unsigned char payload[] = {0x52u, 0x45u, 0x53u, 0x50u};
    redis_io_runtime runtime = {0};
    redis_io_runtime_config config = {
        redis_test_backend_kind(), 2u, 4u, 2u};
    redis_test_socket sockets[2];
    unsigned char received[sizeof(payload)] = {0};
    redis_io_request receive = {0};
    redis_io_request send_request = {0};
    redis_io_wake_probe receive_probe = {0};
    redis_io_wake_probe send_probe = {0};
    cflow_waitable receive_waitable;
    cflow_waitable send_waitable;
    cflow_io_completion receive_completion = {0};
    cflow_io_completion send_completion = {0};
    cflow_io_native_operation receive_operation = {
        .kind = CFLOW_IO_NATIVE_TCP_RECV,
        .socket = (uintptr_t)REDIS_TEST_INVALID_SOCKET,
        .buffer = received,
        .length = sizeof(received)};
    cflow_io_native_operation send_operation = {
        .kind = CFLOW_IO_NATIVE_TCP_SEND,
        .socket = (uintptr_t)REDIS_TEST_INVALID_SOCKET,
        .buffer = (void *)payload,
        .length = sizeof(payload)};

    check_equal(redis_io_runtime_init(&runtime, &config), TURBO_OK);
    check_equal(redis_test_socket_pair(sockets), TURBO_OK);
    receive_operation.socket = (uintptr_t)sockets[1];
    send_operation.socket = (uintptr_t)sockets[0];
    check_equal(redis_io_runtime_try_submit(&runtime, 1u, &receive_operation,
                                            &receive),
                REDIS_IO_SUBMIT_ACCEPTED);
    check_equal(redis_io_runtime_try_submit(&runtime, 2u, &send_operation,
                                            &send_request),
                REDIS_IO_SUBMIT_ACCEPTED);
    check_equal(redis_io_runtime_retire_socket(&runtime,
                                               (uintptr_t)sockets[0]),
                TURBO_EBUSY);
    check_equal(redis_io_runtime_retire_socket(&runtime,
                                               (uintptr_t)sockets[1]),
                TURBO_EBUSY);

    receive_waitable = redis_io_request_waitable(&receive);
    send_waitable = redis_io_request_waitable(&send_request);
    check_true(cflow_waitable_arm(
        &receive_waitable, (cflow_waker){redis_test_wake, &receive_probe}));
    check_true(cflow_waitable_arm(
        &send_waitable, (cflow_waker){redis_test_wake, &send_probe}));
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)),
                TURBO_OK);

    check_equal(receive_probe.wakes, 1);
    check_equal(send_probe.wakes, 1);
    check_equal(redis_io_request_poll(&receive, &receive_completion),
                REDIS_IO_REQUEST_COMPLETED);
    check_equal(redis_io_request_poll(&send_request, &send_completion),
                REDIS_IO_REQUEST_COMPLETED);
    check_equal(receive_completion.kind, CFLOW_IO_COMPLETION_OK);
    check_equal(receive_completion.bytes, sizeof(payload));
    check_equal(send_completion.kind, CFLOW_IO_COMPLETION_OK);
    check_equal(send_completion.bytes, sizeof(payload));
    check_equal(received, payload, sizeof(payload));
    check_equal(redis_io_request_acknowledge(&receive), TURBO_OK);
    check_equal(redis_io_request_acknowledge(&send_request), TURBO_OK);

    redis_test_close_socket(sockets[0]);
    redis_test_close_socket(sockets[1]);
    check_equal(redis_test_forget_socket(&runtime,
                                               (uintptr_t)sockets[0]),
                TURBO_OK);
    check_equal(redis_test_forget_socket(&runtime,
                                               (uintptr_t)sockets[1]),
                TURBO_OK);
    check_equal(redis_io_runtime_close(&runtime), TURBO_OK);
    check_equal(redis_io_runtime_destroy(&runtime), TURBO_OK);
  }

  it("waitable cancellation synchronizes with an in-flight wake callback") {
    static const unsigned char payload = 0x52u;
    redis_io_runtime runtime = {0};
    redis_io_runtime_config config = {
        redis_test_backend_kind(), 1u, 2u, 1u};
    redis_test_socket sockets[2];
    unsigned char received = 0u;
    redis_io_request request = {0};
    redis_io_blocking_waker blocker = {0};
    redis_io_wait_thread driver = {&runtime, TURBO_EINVAL};
    turbo_thread_t driver_thread = NULL;
    turbo_thread_t cancel_thread = NULL;
    cflow_io_completion completion = {0};
    cflow_io_native_operation receive_operation = {
        .kind = CFLOW_IO_NATIVE_TCP_RECV,
        .socket = (uintptr_t)REDIS_TEST_INVALID_SOCKET,
        .buffer = &received,
        .length = 1u};

    turbo_mutex_init(&blocker.gate);
    turbo_cond_init(&blocker.changed);
    check_equal(redis_io_runtime_init(&runtime, &config), TURBO_OK);
    check_equal(redis_test_socket_pair(sockets), TURBO_OK);
    receive_operation.socket = (uintptr_t)sockets[0];
    check_equal(redis_io_runtime_try_submit(&runtime, 11u,
                                            &receive_operation, &request),
                REDIS_IO_SUBMIT_ACCEPTED);
    blocker.waitable = redis_io_request_waitable(&request);
    check_true(cflow_waitable_arm(
        &blocker.waitable,
        (cflow_waker){redis_test_blocking_wake, &blocker}));
    check_equal(send(sockets[1], (const char *)&payload, 1, 0), 1);
    check_equal(turbo_thread_create(&driver_thread,
                                    redis_test_drive_until_idle, &driver),
                TURBO_OK);
    turbo_mutex_lock(&blocker.gate);
    while (!blocker.entered)
      turbo_cond_wait(&blocker.changed, &blocker.gate);
    turbo_mutex_unlock(&blocker.gate);
    check_equal(turbo_thread_create(&cancel_thread,
                                    redis_test_cancel_waitable, &blocker),
                TURBO_OK);
    turbo_mutex_lock(&blocker.gate);
    while (!blocker.cancel_started)
      turbo_cond_wait(&blocker.changed, &blocker.gate);
    turbo_mutex_unlock(&blocker.gate);
    turbo_sleep_ms(20u);
    turbo_mutex_lock(&blocker.gate);
    check_false(blocker.cancel_done);
    blocker.release = 1;
    turbo_cond_broadcast(&blocker.changed);
    turbo_mutex_unlock(&blocker.gate);
    check_equal(turbo_thread_join(&cancel_thread), TURBO_OK);
    check_equal(turbo_thread_join(&driver_thread), TURBO_OK);
    turbo_thread_destroy(&cancel_thread);
    turbo_thread_destroy(&driver_thread);
    check_equal(driver.status, TURBO_OK);
    check_true(blocker.cancel_done);
    check_equal(redis_io_request_poll(&request, &completion),
                REDIS_IO_REQUEST_COMPLETED);
    check_equal(redis_io_request_acknowledge(&request), TURBO_OK);

    redis_test_close_socket(sockets[0]);
    redis_test_close_socket(sockets[1]);
    check_equal(redis_test_forget_socket(&runtime,
                                               (uintptr_t)sockets[0]),
                TURBO_OK);
    check_equal(redis_test_forget_socket(&runtime,
                                               (uintptr_t)sockets[1]),
                TURBO_OK);
    check_equal(redis_io_runtime_close(&runtime), TURBO_OK);
    check_equal(redis_io_runtime_destroy(&runtime), TURBO_OK);
    turbo_cond_destroy(&blocker.changed);
    turbo_mutex_destroy(&blocker.gate);
  }

  it("reuses a released capacity slot from inside its wake callback") {
    static const unsigned char inbound = 0x52u;
    static const unsigned char outbound = 0x53u;
    redis_io_runtime runtime = {0};
    redis_io_runtime_config config = {
        redis_test_backend_kind(), 1u, 4u, 1u};
    redis_test_socket sockets[2];
    unsigned char received = 0u;
    unsigned char observed = 0u;
    redis_io_request request = {0};
    redis_io_resubmit_waker state = {0};
    cflow_waitable waitable;
    cflow_io_completion completion = {0};
    cflow_io_native_operation receive_operation = {
        .kind = CFLOW_IO_NATIVE_TCP_RECV,
        .socket = (uintptr_t)REDIS_TEST_INVALID_SOCKET,
        .buffer = &received,
        .length = 1u};

    check_equal(redis_io_runtime_init(&runtime, &config), TURBO_OK);
    check_equal(redis_test_socket_pair(sockets), TURBO_OK);
    receive_operation.socket = (uintptr_t)sockets[0];
    state.runtime = &runtime;
    state.completed_request = &request;
    state.poll_status = REDIS_IO_REQUEST_INVALID;
    state.acknowledge_status = TURBO_EINVAL;
    state.submit_status = REDIS_IO_SUBMIT_INVALID_ARGUMENT;
    state.next_operation = (cflow_io_native_operation){
        .kind = CFLOW_IO_NATIVE_TCP_SEND,
        .socket = (uintptr_t)sockets[0],
        .buffer = (void *)&outbound,
        .length = 1u};
    check_equal(redis_io_runtime_try_submit(&runtime, 21u,
                                            &receive_operation, &request),
                REDIS_IO_SUBMIT_ACCEPTED);
    waitable = redis_io_request_waitable(&request);
    check_true(cflow_waitable_arm(
        &waitable,
        (cflow_waker){redis_test_acknowledge_and_resubmit, &state}));
    check_equal(send(sockets[1], (const char *)&inbound, 1, 0), 1);
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)),
                TURBO_OK);

    check_equal(state.poll_status, REDIS_IO_REQUEST_COMPLETED);
    check_equal(state.acknowledge_status, TURBO_OK);
    check_equal(state.submit_status, REDIS_IO_SUBMIT_ACCEPTED);
    check_equal(redis_io_request_poll(&state.next_request, &completion),
                REDIS_IO_REQUEST_COMPLETED);
    check_equal(completion.kind, CFLOW_IO_COMPLETION_OK);
    check_equal(completion.bytes, 1u);
    check_equal(recv(sockets[1], (char *)&observed, 1, 0), 1);
    check_equal(observed, outbound);
    check_equal(redis_io_request_acknowledge(&state.next_request), TURBO_OK);

    redis_test_close_socket(sockets[0]);
    redis_test_close_socket(sockets[1]);
    check_equal(redis_test_forget_socket(&runtime,
                                               (uintptr_t)sockets[0]),
                TURBO_OK);
    check_equal(redis_test_forget_socket(&runtime,
                                               (uintptr_t)sockets[1]),
                TURBO_OK);
    check_equal(redis_io_runtime_close(&runtime), TURBO_OK);
    check_equal(redis_io_runtime_destroy(&runtime), TURBO_OK);
  }

  it("rolls back a callback-reused slot when Actor admission rejects") {
    static const unsigned char blocker_payload = 0x61u;
    static const unsigned char callback_payload = 0x62u;
    static const unsigned char recovery_payload = 0x63u;
    redis_io_runtime runtime = {0};
    redis_io_runtime_config config = {
        redis_test_backend_kind(), 2u, 4u, 2u};
    redis_test_socket blocker_sockets[2];
    redis_test_socket callback_sockets[2];
    redis_io_request blocker = {0};
    redis_io_request callback_request = {0};
    redis_io_request recovery = {0};
    redis_io_resubmit_waker state = {0};
    cflow_waitable waitable;
    cflow_io_completion completion = {0};
    cflow_io_native_operation blocker_operation = {
        .kind = CFLOW_IO_NATIVE_TCP_SEND,
        .socket = (uintptr_t)REDIS_TEST_INVALID_SOCKET,
        .buffer = (void *)&blocker_payload,
        .length = 1u};
    cflow_io_native_operation callback_operation = {
        .kind = CFLOW_IO_NATIVE_TCP_SEND,
        .socket = (uintptr_t)REDIS_TEST_INVALID_SOCKET,
        .buffer = (void *)&callback_payload,
        .length = 1u};
    cflow_io_native_operation recovery_operation = {
        .kind = CFLOW_IO_NATIVE_TCP_SEND,
        .socket = (uintptr_t)REDIS_TEST_INVALID_SOCKET,
        .buffer = (void *)&recovery_payload,
        .length = 1u};

    check_equal(redis_io_runtime_init(&runtime, &config), TURBO_OK);
    check_equal(redis_test_socket_pair(blocker_sockets), TURBO_OK);
    check_equal(redis_test_socket_pair(callback_sockets), TURBO_OK);
    blocker_operation.socket = (uintptr_t)blocker_sockets[0];
    callback_operation.socket = (uintptr_t)callback_sockets[0];
    recovery_operation.socket = (uintptr_t)callback_sockets[0];
    state.runtime = &runtime;
    state.completed_request = &callback_request;
    state.next_operation = recovery_operation;
    state.poll_status = REDIS_IO_REQUEST_INVALID;
    state.acknowledge_status = TURBO_EINVAL;
    state.submit_status = REDIS_IO_SUBMIT_INVALID_ARGUMENT;

    check_equal(redis_io_runtime_try_submit(&runtime, 22u,
                                            &blocker_operation, &blocker),
                REDIS_IO_SUBMIT_ACCEPTED);
    check_equal(redis_io_runtime_try_submit(&runtime, 21u,
                                            &callback_operation,
                                            &callback_request),
                REDIS_IO_SUBMIT_ACCEPTED);
    waitable = redis_io_request_waitable(&callback_request);
    check_true(cflow_waitable_arm(
        &waitable,
        (cflow_waker){redis_test_acknowledge_and_resubmit, &state}));
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)),
                TURBO_OK);
    check_equal(state.poll_status, REDIS_IO_REQUEST_COMPLETED);
    check_equal(state.acknowledge_status, TURBO_OK);
    check_equal(state.submit_status, REDIS_IO_SUBMIT_LEASE_IN_USE);
    check_false(redis_io_request_valid(&state.next_request));

    check_equal(redis_io_runtime_try_submit(&runtime, 23u,
                                            &recovery_operation, &recovery),
                REDIS_IO_SUBMIT_ACCEPTED);
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)),
                TURBO_OK);
    check_equal(redis_io_request_poll(&blocker, &completion),
                REDIS_IO_REQUEST_COMPLETED);
    check_equal(redis_io_request_acknowledge(&blocker), TURBO_OK);
    check_equal(redis_io_request_poll(&recovery, &completion),
                REDIS_IO_REQUEST_COMPLETED);
    check_equal(redis_io_request_acknowledge(&recovery), TURBO_OK);

    redis_test_close_socket(blocker_sockets[0]);
    redis_test_close_socket(blocker_sockets[1]);
    redis_test_close_socket(callback_sockets[0]);
    redis_test_close_socket(callback_sockets[1]);
    check_equal(redis_test_forget_socket(
                    &runtime, (uintptr_t)blocker_sockets[0]),
                TURBO_OK);
    check_equal(redis_test_forget_socket(
                    &runtime, (uintptr_t)blocker_sockets[1]),
                TURBO_OK);
    check_equal(redis_test_forget_socket(
                    &runtime, (uintptr_t)callback_sockets[0]),
                TURBO_OK);
    check_equal(redis_test_forget_socket(
                    &runtime, (uintptr_t)callback_sockets[1]),
                TURBO_OK);
    check_equal(redis_io_runtime_close(&runtime), TURBO_OK);
    check_equal(redis_io_runtime_destroy(&runtime), TURBO_OK);
  }

  it("rejects runtime destruction while a wake callback is active") {
    static const unsigned char payload = 0x52u;
    redis_io_runtime runtime = {0};
    redis_io_runtime_config config = {
        redis_test_backend_kind(), 1u, 2u, 1u};
    redis_test_socket sockets[2];
    unsigned char received = 0u;
    redis_io_request request = {0};
    redis_io_destroy_waker state = {0};
    cflow_waitable waitable;
    cflow_io_native_operation receive_operation = {
        .kind = CFLOW_IO_NATIVE_TCP_RECV,
        .socket = (uintptr_t)REDIS_TEST_INVALID_SOCKET,
        .buffer = &received,
        .length = 1u};

    check_equal(redis_io_runtime_init(&runtime, &config), TURBO_OK);
    check_equal(redis_test_socket_pair(sockets), TURBO_OK);
    receive_operation.socket = (uintptr_t)sockets[0];
    state.runtime = &runtime;
    state.request = &request;
    state.poll_status = REDIS_IO_REQUEST_INVALID;
    state.acknowledge_status = TURBO_EINVAL;
    state.close_status = TURBO_EINVAL;
    state.destroy_status = TURBO_OK;
    check_equal(redis_io_runtime_try_submit(&runtime, 31u,
                                            &receive_operation, &request),
                REDIS_IO_SUBMIT_ACCEPTED);
    waitable = redis_io_request_waitable(&request);
    check_true(cflow_waitable_arm(
        &waitable,
        (cflow_waker){redis_test_close_and_destroy_from_waker, &state}));
    check_equal(send(sockets[1], (const char *)&payload, 1, 0), 1);
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)),
                TURBO_OK);

    check_equal(state.poll_status, REDIS_IO_REQUEST_COMPLETED);
    check_equal(state.acknowledge_status, TURBO_OK);
    check_equal(state.close_status, TURBO_OK);
    check_equal(state.destroy_status, TURBO_EBUSY);
    redis_test_close_socket(sockets[0]);
    redis_test_close_socket(sockets[1]);
    check_equal(redis_test_forget_socket(&runtime,
                                               (uintptr_t)sockets[0]),
                TURBO_OK);
    check_equal(redis_test_forget_socket(&runtime,
                                               (uintptr_t)sockets[1]),
                TURBO_OK);
    check_equal(redis_io_runtime_destroy(&runtime), TURBO_OK);
  }

  it("rejects wait_idle reentry from a wake callback without waiting") {
    static const unsigned char payload = 0x53u;
    redis_io_runtime runtime = {0};
    redis_io_runtime_config config = {
        redis_test_backend_kind(), 1u, 2u, 1u};
    redis_test_socket sockets[2];
    unsigned char received = 0u;
    redis_io_request request = {0};
    redis_io_wait_idle_waker state = {0};
    cflow_waitable waitable;
    cflow_io_native_operation receive_operation = {
        .kind = CFLOW_IO_NATIVE_TCP_RECV,
        .socket = (uintptr_t)REDIS_TEST_INVALID_SOCKET,
        .buffer = &received,
        .length = 1u};

    check_equal(redis_io_runtime_init(&runtime, &config), TURBO_OK);
    check_equal(redis_test_socket_pair(sockets), TURBO_OK);
    receive_operation.socket = (uintptr_t)sockets[0];
    state.runtime = &runtime;
    state.request = &request;
    state.poll_status = REDIS_IO_REQUEST_INVALID;
    state.acknowledge_status = TURBO_EINVAL;
    state.wait_idle_status = TURBO_OK;
    check_equal(redis_io_runtime_try_submit(&runtime, 32u,
                                            &receive_operation, &request),
                REDIS_IO_SUBMIT_ACCEPTED);
    waitable = redis_io_request_waitable(&request);
    check_true(cflow_waitable_arm(
        &waitable, (cflow_waker){redis_test_wait_idle_from_waker, &state}));
    check_equal(send(sockets[1], (const char *)&payload, 1, 0), 1);
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)),
                TURBO_OK);

    check_equal(state.poll_status, REDIS_IO_REQUEST_COMPLETED);
    check_equal(state.acknowledge_status, TURBO_OK);
    check_equal(state.wait_idle_status, TURBO_EBUSY);
    redis_test_close_socket(sockets[0]);
    redis_test_close_socket(sockets[1]);
    check_equal(redis_test_forget_socket(&runtime,
                                               (uintptr_t)sockets[0]),
                TURBO_OK);
    check_equal(redis_test_forget_socket(&runtime,
                                               (uintptr_t)sockets[1]),
                TURBO_OK);
    check_equal(redis_io_runtime_close(&runtime), TURBO_OK);
    check_equal(redis_io_runtime_destroy(&runtime), TURBO_OK);
  }

  it("does not let a stale cancel target a slot reused by its callback") {
    static const unsigned char inbound = 0x52u;
    static const unsigned char outbound = 0x53u;
    redis_io_runtime runtime = {0};
    redis_io_runtime_config config = {
        redis_test_backend_kind(), 1u, 4u, 1u};
    redis_test_socket sockets[2];
    unsigned char received = 0u;
    redis_io_request request = {0};
    redis_io_request stale_request = {0};
    redis_io_reuse_race state = {0};
    redis_io_cancel_race cancel = {0};
    redis_io_cancel_race waitable_cancel = {0};
    redis_io_wait_thread driver = {&runtime, TURBO_EINVAL};
    turbo_thread_t driver_thread = NULL;
    turbo_thread_t cancel_thread = NULL;
    turbo_thread_t waitable_cancel_thread = NULL;
    cflow_waitable waitable;
    cflow_io_completion completion = {0};
    cflow_io_native_operation receive_operation = {
        .kind = CFLOW_IO_NATIVE_TCP_RECV,
        .socket = (uintptr_t)REDIS_TEST_INVALID_SOCKET,
        .buffer = &received,
        .length = 1u};

    turbo_mutex_init(&state.gate);
    turbo_cond_init(&state.changed);
    check_equal(redis_io_runtime_init(&runtime, &config), TURBO_OK);
    check_equal(redis_test_socket_pair(sockets), TURBO_OK);
    receive_operation.socket = (uintptr_t)sockets[0];
    state.runtime = &runtime;
    state.completed_request = &request;
    state.poll_status = REDIS_IO_REQUEST_INVALID;
    state.acknowledge_status = TURBO_EINVAL;
    state.submit_status = REDIS_IO_SUBMIT_INVALID_ARGUMENT;
    state.reuse_completed_request = 1;
    state.block_after_submit = 1;
    state.next_operation = (cflow_io_native_operation){
        .kind = CFLOW_IO_NATIVE_TCP_SEND,
        .socket = (uintptr_t)sockets[0],
        .buffer = (void *)&outbound,
        .length = 1u};
    check_equal(redis_io_runtime_try_submit(&runtime, 41u,
                                            &receive_operation, &request),
                REDIS_IO_SUBMIT_ACCEPTED);
    stale_request = request;
    cancel.state = &state;
    cancel.request = &stale_request;
    cancel.status = CFLOW_IO_CANCEL_ACCEPTED;
    waitable = redis_io_request_waitable(&request);
    waitable_cancel.state = &state;
    waitable_cancel.waitable = waitable;
    waitable_cancel.use_waitable = 1;
    waitable_cancel.status = CFLOW_IO_CANCEL_ACCEPTED;
    check_true(cflow_waitable_arm(
        &waitable, (cflow_waker){redis_test_block_then_resubmit, &state}));
    check_equal(send(sockets[1], (const char *)&inbound, 1, 0), 1);
    check_equal(turbo_thread_create(&driver_thread,
                                    redis_test_drive_until_idle, &driver),
                TURBO_OK);
    turbo_mutex_lock(&state.gate);
    while (!state.entered) turbo_cond_wait(&state.changed, &state.gate);
    turbo_mutex_unlock(&state.gate);
    check_equal(turbo_thread_create(&cancel_thread,
                                    redis_test_cancel_stale_request, &cancel),
                TURBO_OK);
    check_equal(turbo_thread_create(&waitable_cancel_thread,
                                    redis_test_cancel_stale_request,
                                    &waitable_cancel),
                TURBO_OK);
    turbo_mutex_lock(&state.gate);
    while (state.cancel_started != 2)
      turbo_cond_wait(&state.changed, &state.gate);
    state.proceed = 1;
    turbo_cond_broadcast(&state.changed);
    while (!state.resubmitted)
      turbo_cond_wait(&state.changed, &state.gate);
    turbo_mutex_unlock(&state.gate);
    turbo_sleep_ms(20u);
    turbo_mutex_lock(&state.gate);
    check_false(cancel.done);
    check_false(waitable_cancel.done);
    state.release_after_submit = 1;
    turbo_cond_broadcast(&state.changed);
    turbo_mutex_unlock(&state.gate);
    check_equal(turbo_thread_join(&cancel_thread), TURBO_OK);
    check_equal(turbo_thread_join(&waitable_cancel_thread), TURBO_OK);
    check_equal(turbo_thread_join(&driver_thread), TURBO_OK);
    turbo_thread_destroy(&cancel_thread);
    turbo_thread_destroy(&waitable_cancel_thread);
    turbo_thread_destroy(&driver_thread);

    check_equal(driver.status, TURBO_OK);
    check_equal(cancel.status, CFLOW_IO_CANCEL_INVALID_ARGUMENT);
    check_equal(waitable_cancel.status, CFLOW_IO_CANCEL_INVALID_ARGUMENT);
    check_equal(state.poll_status, REDIS_IO_REQUEST_COMPLETED);
    check_equal(state.acknowledge_status, TURBO_OK);
    check_equal(state.submit_status, REDIS_IO_SUBMIT_ACCEPTED);
    check_equal(redis_io_request_poll(&request, &completion),
                REDIS_IO_REQUEST_COMPLETED);
    check_equal(completion.kind, CFLOW_IO_COMPLETION_OK);
    check_equal(redis_io_request_acknowledge(&request), TURBO_OK);

    redis_test_close_socket(sockets[0]);
    redis_test_close_socket(sockets[1]);
    check_equal(redis_test_forget_socket(&runtime,
                                               (uintptr_t)sockets[0]),
                TURBO_OK);
    check_equal(redis_test_forget_socket(&runtime,
                                               (uintptr_t)sockets[1]),
                TURBO_OK);
    check_equal(redis_io_runtime_close(&runtime), TURBO_OK);
    check_equal(redis_io_runtime_destroy(&runtime), TURBO_OK);
    turbo_cond_destroy(&state.changed);
    turbo_mutex_destroy(&state.gate);
  }
}
