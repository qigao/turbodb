#include "../redis_cflow.h"

#include "tinytest.h"
#include "turbo_error.h"

#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET redis_cflow_test_socket;
  #define REDIS_CFLOW_TEST_INVALID INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
typedef int redis_cflow_test_socket;
  #define REDIS_CFLOW_TEST_INVALID (-1)
#endif

static int redis_cflow_test_socket_error(void) {
#if defined(_WIN32)
  return -WSAGetLastError();
#else
  return -errno;
#endif
}

static void redis_cflow_test_close(redis_cflow_test_socket socket_value) {
  if (socket_value == REDIS_CFLOW_TEST_INVALID) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static int redis_cflow_test_nonblocking(redis_cflow_test_socket socket_value) {
#if defined(_WIN32)
  u_long enabled = 1u;
  return ioctlsocket(socket_value, FIONBIO, &enabled) == 0 ? TURBO_OK
                                                           : redis_cflow_test_socket_error();
#else
  int flags = fcntl(socket_value, F_GETFL);
  if (flags < 0) return -errno;
  return fcntl(socket_value, F_SETFL, flags | O_NONBLOCK) == 0 ? TURBO_OK : -errno;
#endif
}

static int redis_cflow_test_pair(redis_cflow_test_socket sockets[2]) {
  redis_cflow_test_socket listener = REDIS_CFLOW_TEST_INVALID;
  struct sockaddr_in address;
#if defined(_WIN32)
  int address_size = (int)sizeof(address);
#else
  socklen_t address_size = (socklen_t)sizeof(address);
#endif
  int status = TURBO_OK;
  sockets[0] = REDIS_CFLOW_TEST_INVALID;
  sockets[1] = REDIS_CFLOW_TEST_INVALID;
  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == REDIS_CFLOW_TEST_INVALID) return redis_cflow_test_socket_error();
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listener, (const struct sockaddr *)&address, (int)sizeof(address)) != 0 ||
      getsockname(listener, (struct sockaddr *)&address, &address_size) != 0 ||
      listen(listener, 1) != 0)
    status = redis_cflow_test_socket_error();
  if (status == TURBO_OK) {
    sockets[0] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockets[0] == REDIS_CFLOW_TEST_INVALID) status = redis_cflow_test_socket_error();
  }
  if (status == TURBO_OK &&
      connect(sockets[0], (const struct sockaddr *)&address, (int)sizeof(address)) != 0)
    status = redis_cflow_test_socket_error();
  if (status == TURBO_OK) {
    sockets[1] = accept(listener, NULL, NULL);
    if (sockets[1] == REDIS_CFLOW_TEST_INVALID) status = redis_cflow_test_socket_error();
  }
  redis_cflow_test_close(listener);
  if (status == TURBO_OK) status = redis_cflow_test_nonblocking(sockets[0]);
  if (status == TURBO_OK) status = redis_cflow_test_nonblocking(sockets[1]);
  return status;
}

static int redis_cflow_test_listener(redis_cflow_test_socket *listener, uint16_t *port) {
  struct sockaddr_in address;
#if defined(_WIN32)
  int address_size = (int)sizeof(address);
#else
  socklen_t address_size = (socklen_t)sizeof(address);
#endif
  *listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (*listener == REDIS_CFLOW_TEST_INVALID) return redis_cflow_test_socket_error();
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(*listener, (const struct sockaddr *)&address, (int)sizeof(address)) != 0 ||
      getsockname(*listener, (struct sockaddr *)&address, &address_size) != 0 ||
      listen(*listener, 1) != 0) {
    int status = redis_cflow_test_socket_error();
    redis_cflow_test_close(*listener);
    *listener = REDIS_CFLOW_TEST_INVALID;
    return status;
  }
  *port = ntohs(address.sin_port);
  return TURBO_OK;
}

static cflow_io_native_backend_kind redis_cflow_test_backend(void) {
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

typedef struct redis_cflow_connect_waker {
  redis_cflow_connection *connection;
  redis_cflow_connect_step step;
} redis_cflow_connect_waker;

typedef struct redis_cflow_cancel_waker {
  redis_cflow_stream *stream;
  int status;
} redis_cflow_cancel_waker;

static void redis_cflow_test_connect_next(void *user) {
  redis_cflow_connect_waker *state = (redis_cflow_connect_waker *)user;
  state->step = redis_cflow_connection_connect_next(state->connection);
}

static void redis_cflow_test_cancel_stream(void *user) {
  redis_cflow_cancel_waker *state = (redis_cflow_cancel_waker *)user;
  state->status = redis_cflow_stream_cancel(state->stream);
}

suite("redis CFlow RESP stream") {
  it("connects a resolved endpoint through the CFlow native backend") {
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {redis_cflow_test_backend(), 1u, 1u};
    redis_cflow_test_socket listener = REDIS_CFLOW_TEST_INVALID;
    redis_cflow_test_socket accepted = REDIS_CFLOW_TEST_INVALID;
    uint16_t port = 0u;
    redis_cflow_connection connection = {0};
    redis_cflow_open_config config;
    redis_cflow_connect_step step;

    check_equal(redis_io_runtime_init(&runtime, &runtime_config), TURBO_OK);
    check_equal(redis_cflow_test_listener(&listener, &port), TURBO_OK);
    config = (redis_cflow_open_config){&runtime, "127.0.0.1",         port, 2u, 1024u, 8u, 128u,
                                       16u,      UINT64_C(5000000000)};
    check_equal(redis_cflow_connection_open(&connection, &config), TURBO_OK);
    step = redis_cflow_connection_connect_next(&connection);
    check_equal(step.kind, REDIS_CFLOW_CONNECT_WAIT);
    check_true(cflow_waitable_valid(&step.waitable));
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), TURBO_OK);
    step = redis_cflow_connection_connect_next(&connection);
    check_equal(step.status, TURBO_OK);
    check_equal(step.kind, REDIS_CFLOW_CONNECT_DONE);
    check_true(redis_cflow_connection_usable(&connection));
    accepted = accept(listener, NULL, NULL);
    check_true(accepted != REDIS_CFLOW_TEST_INVALID);

    check_equal(redis_cflow_connection_destroy(&connection), TURBO_OK);
    redis_cflow_test_close(accepted);
    redis_cflow_test_close(listener);
    check_equal(redis_io_runtime_close(&runtime), TURBO_OK);
    check_equal(redis_io_runtime_destroy(&runtime), TURBO_OK);
  }

  it("fails fast when connect cleanup reenters from its wake callback") {
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {redis_cflow_test_backend(), 1u, 1u};
    redis_cflow_test_socket listener = REDIS_CFLOW_TEST_INVALID;
    uint16_t port = 0u;
    redis_cflow_connection connection = {0};
    redis_cflow_open_config config;
    redis_cflow_connect_step initial;
    redis_cflow_connect_step retry;
    redis_cflow_connect_waker state = {0};

    check_equal(redis_io_runtime_init(&runtime, &runtime_config), TURBO_OK);
    check_equal(redis_cflow_test_listener(&listener, &port), TURBO_OK);
    redis_cflow_test_close(listener);
    listener = REDIS_CFLOW_TEST_INVALID;
    config = (redis_cflow_open_config){&runtime, "127.0.0.1",       port, 1u, 1024u, 8u, 128u,
                                       16u,      UINT64_C(10000000)};
    check_equal(redis_cflow_connection_open(&connection, &config), TURBO_OK);
    initial = redis_cflow_connection_connect_next(&connection);
    check_equal(initial.kind, REDIS_CFLOW_CONNECT_WAIT);
    state.connection = &connection;
    state.step.kind = REDIS_CFLOW_CONNECT_WAIT;
    check_true(cflow_waitable_arm(&initial.waitable,
                                  (cflow_waker){redis_cflow_test_connect_next, &state}));
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), TURBO_OK);
    check_equal(state.step.kind, REDIS_CFLOW_CONNECT_ERROR);
#if defined(__linux__)
    check_equal(state.step.status, TURBO_EBUSY);
#else
    check_not_equal(state.step.status, TURBO_ETIMEDOUT);
#endif
    retry = redis_cflow_connection_connect_next(&connection);
    check_equal(retry.kind, REDIS_CFLOW_CONNECT_ERROR);
    check_not_equal(retry.status, TURBO_EBUSY);
    check_equal(redis_cflow_connection_destroy(&connection), TURBO_OK);
    check_equal(redis_io_runtime_close(&runtime), TURBO_OK);
    check_equal(redis_io_runtime_destroy(&runtime), TURBO_OK);
  }

  it("turns partial network RESP into demand-driven WAIT and ITEM steps") {
    static const char *arguments[] = {"PING"};
    static const char expected_command[] = "*1\r\n$4\r\nPING\r\n";
    static const char first_reply[] = "*2\r\n:7\r\n$3\r\nf";
    static const char second_reply[] = "oo\r\n";
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {redis_cflow_test_backend(), 1u, 1u};
    redis_cflow_test_socket sockets[2];
    redis_cflow_connection connection = {0};
    redis_cflow_connection_config connection_config;
    redis_cflow_stream stream = {0};
    redis_cflow_stream_step step;
    char command[sizeof(expected_command)] = {0};
    int received;

    check_equal(redis_io_runtime_init(&runtime, &runtime_config), TURBO_OK);
    check_equal(redis_cflow_test_pair(sockets), TURBO_OK);
    connection_config = (redis_cflow_connection_config){
        &runtime, (uintptr_t)sockets[0], 1024u, 8u, 128u, 16u, UINT64_C(5000000000), 1};
    check_equal(redis_cflow_connection_init_attached(&connection, &connection_config), TURBO_OK);
    check_equal(redis_cflow_stream_open(&connection, 1, arguments, NULL, 1024u, 2u, &stream),
                TURBO_OK);

    step = redis_cflow_stream_next(&stream);
    check_equal(step.kind, REDIS_CFLOW_STREAM_WAIT);
    check_equal(step.outcome, REDIS_COMMAND_SEND_UNCERTAIN);
    check_true(cflow_waitable_valid(&step.waitable));
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), TURBO_OK);
    step = redis_cflow_stream_next(&stream);
    check_equal(step.kind, REDIS_CFLOW_STREAM_WAIT);
    received = recv(sockets[1], command, (int)sizeof(command), 0);
    check_equal(received, (int)(sizeof(expected_command) - 1u));
    check_equal(command, expected_command, sizeof(expected_command) - 1u);

    check_equal(send(sockets[1], first_reply, (int)(sizeof(first_reply) - 1u), 0),
                (int)(sizeof(first_reply) - 1u));
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), TURBO_OK);
    step = redis_cflow_stream_next(&stream);
    check_equal(step.kind, REDIS_CFLOW_STREAM_ITEM);
    check_not_null(step.item);
    check_equal(step.item->type, REDIS_REPLY_INTEGER);
    check_equal(step.item->integer, 7);
    redis_reply_free(step.item);

    step = redis_cflow_stream_next(&stream);
    check_equal(step.kind, REDIS_CFLOW_STREAM_WAIT);
    check_equal(send(sockets[1], second_reply, (int)(sizeof(second_reply) - 1u), 0),
                (int)(sizeof(second_reply) - 1u));
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), TURBO_OK);
    step = redis_cflow_stream_next(&stream);
    check_equal(step.kind, REDIS_CFLOW_STREAM_ITEM);
    check_equal(step.item->type, REDIS_REPLY_BULK_STRING);
    check_equal(step.item->len, 3u);
    check_equal(step.item->str, "foo", 3u);
    redis_reply_free(step.item);
    step = redis_cflow_stream_next(&stream);
    check_equal(step.kind, REDIS_CFLOW_STREAM_DONE);

    check_equal(redis_cflow_stream_destroy(&stream), TURBO_OK);
    check_equal(redis_cflow_command_open(&connection, 1, arguments, NULL, 1024u, &stream),
                TURBO_OK);
    step = redis_cflow_stream_next(&stream);
    check_equal(step.kind, REDIS_CFLOW_STREAM_WAIT);
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), TURBO_OK);
    step = redis_cflow_stream_next(&stream);
    check_equal(step.kind, REDIS_CFLOW_STREAM_WAIT);
    memset(command, 0, sizeof(command));
    received = recv(sockets[1], command, (int)sizeof(command), 0);
    check_equal(received, (int)(sizeof(expected_command) - 1u));
    check_equal(command, expected_command, sizeof(expected_command) - 1u);
    check_equal(send(sockets[1], "+PONG\r\n", 7, 0), 7);
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), TURBO_OK);
    step = redis_cflow_stream_next(&stream);
    check_equal(step.kind, REDIS_CFLOW_STREAM_ITEM);
    check_equal(step.item->type, REDIS_REPLY_STRING);
    check_equal(step.item->str, "PONG", 4u);
    redis_reply_free(step.item);
    step = redis_cflow_stream_next(&stream);
    check_equal(step.kind, REDIS_CFLOW_STREAM_DONE);
    check_equal(redis_cflow_stream_destroy(&stream), TURBO_OK);
    check_equal(redis_cflow_connection_destroy(&connection), TURBO_OK);
    redis_cflow_test_close(sockets[1]);
    check_equal(redis_io_runtime_close(&runtime), TURBO_OK);
    check_equal(redis_io_runtime_destroy(&runtime), TURBO_OK);
  }

  it("invalidates a connection when an in-flight partial reply is cancelled") {
    static const char *arguments[] = {"PING"};
    static const char partial_reply[] = "*1\r\n$3\r\nx";
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {redis_cflow_test_backend(), 2u, 2u};
    redis_cflow_test_socket sockets[2];
    redis_cflow_connection connection = {0};
    redis_cflow_connection_config connection_config;
    redis_cflow_stream stream = {0};
    redis_cflow_stream rejected = {0};
    redis_cflow_stream_step step;
    char command[32];

    check_equal(redis_io_runtime_init(&runtime, &runtime_config), TURBO_OK);
    check_equal(redis_cflow_test_pair(sockets), TURBO_OK);
    connection_config = (redis_cflow_connection_config){
        &runtime, (uintptr_t)sockets[0], 1024u, 8u, 128u, 16u, UINT64_C(5000000000), 1};
    check_equal(redis_cflow_connection_init_attached(&connection, &connection_config), TURBO_OK);
    check_equal(redis_cflow_stream_open(&connection, 1, arguments, NULL, 1024u, 1u, &stream),
                TURBO_OK);
    step = redis_cflow_stream_next(&stream);
    check_equal(step.kind, REDIS_CFLOW_STREAM_WAIT);
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), TURBO_OK);
    step = redis_cflow_stream_next(&stream);
    check_equal(step.kind, REDIS_CFLOW_STREAM_WAIT);
    check_true(recv(sockets[1], command, (int)sizeof(command), 0) > 0);
    check_equal(send(sockets[1], partial_reply, (int)(sizeof(partial_reply) - 1u), 0),
                (int)(sizeof(partial_reply) - 1u));
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), TURBO_OK);
    step = redis_cflow_stream_next(&stream);
    check_equal(step.kind, REDIS_CFLOW_STREAM_WAIT);

    check_equal(redis_cflow_stream_cancel(&stream), TURBO_OK);
    check_false(redis_cflow_connection_usable(&connection));
    check_equal(redis_cflow_stream_open(&connection, 1, arguments, NULL, 1024u, 1u, &rejected),
                TURBO_ENOTCONN);
    check_equal(redis_cflow_stream_destroy(&stream), TURBO_OK);
    check_equal(redis_cflow_connection_destroy(&connection), TURBO_OK);
    redis_cflow_test_close(sockets[1]);
    check_equal(redis_io_runtime_close(&runtime), TURBO_OK);
    check_equal(redis_io_runtime_destroy(&runtime), TURBO_OK);
  }

  it("keeps cancellation pending when a wake callback cannot close its Publisher owner") {
    static const char *arguments[] = {"PING"};
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {redis_cflow_test_backend(), 1u, 1u};
    redis_cflow_test_socket sockets[2];
    redis_cflow_connection connection = {0};
    redis_cflow_connection_config connection_config;
    redis_cflow_stream stream = {0};
    redis_cflow_stream_step step;
    redis_cflow_cancel_waker state = {0};

    check_equal(redis_io_runtime_init(&runtime, &runtime_config), TURBO_OK);
    check_equal(redis_cflow_test_pair(sockets), TURBO_OK);
    connection_config = (redis_cflow_connection_config){
        &runtime, (uintptr_t)sockets[0], 1024u, 8u, 128u, 16u, UINT64_C(5000000000), 1};
    check_equal(redis_cflow_connection_init_attached(&connection, &connection_config), TURBO_OK);
    check_equal(redis_cflow_command_open(&connection, 1, arguments, NULL, 1024u, &stream),
                TURBO_OK);
    step = redis_cflow_stream_next(&stream);
    check_equal(step.kind, REDIS_CFLOW_STREAM_WAIT);
    state.stream = &stream;
    state.status = TURBO_OK;
    check_true(
        cflow_waitable_arm(&step.waitable, (cflow_waker){redis_cflow_test_cancel_stream, &state}));
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), TURBO_OK);
    check_equal(state.status, TURBO_EBUSY);
    check_equal(redis_cflow_stream_cancel(&stream), TURBO_OK);
    check_false(redis_cflow_connection_usable(&connection));
    check_equal(redis_cflow_stream_destroy(&stream), TURBO_OK);
    check_equal(redis_cflow_connection_destroy(&connection), TURBO_OK);
    redis_cflow_test_close(sockets[1]);
    check_equal(redis_io_runtime_close(&runtime), TURBO_OK);
    check_equal(redis_io_runtime_destroy(&runtime), TURBO_OK);
  }

  it("rejects command encoding beyond the connection buffer limit") {
    static const char *arguments[] = {"0123456789"};
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {redis_cflow_test_backend(), 1u, 1u};
    redis_cflow_test_socket sockets[2];
    redis_cflow_connection connection = {0};
    redis_cflow_connection_config connection_config;
    redis_cflow_stream stream = {0};

    check_equal(redis_io_runtime_init(&runtime, &runtime_config), TURBO_OK);
    check_equal(redis_cflow_test_pair(sockets), TURBO_OK);
    connection_config = (redis_cflow_connection_config){.runtime = &runtime,
                                                        .socket = (uintptr_t)sockets[0],
                                                        .max_command_bytes = 16u,
                                                        .initial_buffer_bytes = 8u,
                                                        .max_buffer_bytes = 128u,
                                                        .receive_chunk_bytes = 8u,
                                                        .cancel_timeout_ns = UINT64_C(5000000000),
                                                        .take_socket_ownership = 1};
    check_equal(redis_cflow_connection_init_attached(&connection, &connection_config), TURBO_OK);
    check_equal(redis_cflow_command_open(&connection, 1, arguments, NULL, 1024u, &stream),
                TURBO_ENOBUFS);
    check_equal(redis_cflow_connection_destroy(&connection), TURBO_OK);
    redis_cflow_test_close(sockets[1]);
    check_equal(redis_io_runtime_close(&runtime), TURBO_OK);
    check_equal(redis_io_runtime_destroy(&runtime), TURBO_OK);
  }

  it("transfers a server error reply exactly once") {
    static const char *arguments[] = {"GET", "key"};
    static const char server_error[] = "-MOVED 1 127.0.0.1:6380\r\n";
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {redis_cflow_test_backend(), 2u, 2u};
    redis_cflow_test_socket sockets[2];
    redis_cflow_connection connection = {0};
    redis_cflow_connection_config connection_config;
    redis_cflow_stream stream = {0};
    redis_cflow_stream_step step;
    char command[64];

    check_equal(redis_io_runtime_init(&runtime, &runtime_config), TURBO_OK);
    check_equal(redis_cflow_test_pair(sockets), TURBO_OK);
    connection_config = (redis_cflow_connection_config){
        &runtime, (uintptr_t)sockets[0], 1024u, 8u, 128u, 16u, UINT64_C(5000000000), 1};
    check_equal(redis_cflow_connection_init_attached(&connection, &connection_config), TURBO_OK);
    check_equal(redis_cflow_command_open(&connection, 2, arguments, NULL, 1024u, &stream),
                TURBO_OK);
    step = redis_cflow_stream_next(&stream);
    check_equal(step.kind, REDIS_CFLOW_STREAM_WAIT);
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), TURBO_OK);
    step = redis_cflow_stream_next(&stream);
    check_equal(step.kind, REDIS_CFLOW_STREAM_WAIT);
    check_true(recv(sockets[1], command, (int)sizeof(command), 0) > 0);
    check_equal(send(sockets[1], server_error, (int)(sizeof(server_error) - 1u), 0),
                (int)(sizeof(server_error) - 1u));
    do {
      check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), TURBO_OK);
      step = redis_cflow_stream_next(&stream);
    } while (step.kind == REDIS_CFLOW_STREAM_WAIT);
    check_equal(step.kind, REDIS_CFLOW_STREAM_ERROR);
    check_equal(step.outcome, REDIS_COMMAND_REPLIED);
    check_not_null(step.item);
    redis_reply_free(step.item);
    step = redis_cflow_stream_next(&stream);
    check_equal(step.kind, REDIS_CFLOW_STREAM_ERROR);
    check_null(step.item);

    check_equal(redis_cflow_stream_destroy(&stream), TURBO_OK);
    check_equal(redis_cflow_connection_destroy(&connection), TURBO_OK);
    redis_cflow_test_close(sockets[1]);
    check_equal(redis_io_runtime_close(&runtime), TURBO_OK);
    check_equal(redis_io_runtime_destroy(&runtime), TURBO_OK);
  }

  it("applies Publisher capacity when connections attach") {
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {redis_cflow_test_backend(), 1u, 1u};
    redis_cflow_test_socket first_sockets[2];
    redis_cflow_test_socket second_sockets[2];
    redis_cflow_connection first = {0};
    redis_cflow_connection second = {0};
    redis_cflow_connection_config first_config;
    redis_cflow_connection_config second_config;

    check_equal(redis_io_runtime_init(&runtime, &runtime_config), TURBO_OK);
    check_equal(redis_cflow_test_pair(first_sockets), TURBO_OK);
    check_equal(redis_cflow_test_pair(second_sockets), TURBO_OK);
    first_config = (redis_cflow_connection_config){
        &runtime, (uintptr_t)first_sockets[0], 1024u, 8u, 128u, 16u, UINT64_C(5000000000), 1};
    second_config = (redis_cflow_connection_config){
        &runtime, (uintptr_t)second_sockets[0], 1024u, 8u, 128u, 16u, UINT64_C(5000000000), 1};

    check_equal(redis_cflow_connection_init_attached(&first, &first_config), TURBO_OK);
    check_equal(redis_cflow_connection_init_attached(&second, &second_config), TURBO_ENOBUFS);
    check_equal(redis_cflow_connection_destroy(&first), TURBO_OK);
    check_equal(redis_cflow_connection_init_attached(&second, &second_config), TURBO_OK);
    check_equal(redis_cflow_connection_destroy(&second), TURBO_OK);
    redis_cflow_test_close(first_sockets[1]);
    redis_cflow_test_close(second_sockets[1]);
    check_equal(redis_io_runtime_close(&runtime), TURBO_OK);
    check_equal(redis_io_runtime_destroy(&runtime), TURBO_OK);
  }
}
