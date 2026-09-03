#include "../redis_pool.h"

#include "tinytest.h"
#include "salts_error.h"
#include "salts_thread.h"

#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET redis_pool_test_socket;
#define REDIS_POOL_TEST_INVALID INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int redis_pool_test_socket;
#define REDIS_POOL_TEST_INVALID (-1)
#endif

typedef struct redis_pool_test_server {
  redis_pool_test_socket listener;
  salts_thread_t thread;
  int accepted;
  int commands;
} redis_pool_test_server;

static void redis_pool_test_close_socket(redis_pool_test_socket socket_value) {
  if (socket_value == REDIS_POOL_TEST_INVALID) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static int redis_pool_test_listener(redis_pool_test_socket *listener,
                                    uint16_t *port) {
  struct sockaddr_in address;
#if defined(_WIN32)
  int address_size = (int)sizeof(address);
#else
  socklen_t address_size = (socklen_t)sizeof(address);
#endif
  *listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (*listener == REDIS_POOL_TEST_INVALID) return -1;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(*listener, (const struct sockaddr *)&address,
           (int)sizeof(address)) != 0 ||
      getsockname(*listener, (struct sockaddr *)&address, &address_size) != 0 ||
      listen(*listener, 1) != 0) {
    redis_pool_test_close_socket(*listener);
    *listener = REDIS_POOL_TEST_INVALID;
    return -1;
  }
  *port = ntohs(address.sin_port);
  return 0;
}

static void redis_pool_test_server_main(void *argument) {
  static const char pong[] = "+PONG\r\n";
  redis_pool_test_server *server = (redis_pool_test_server *)argument;
  redis_pool_test_socket client = accept(server->listener, NULL, NULL);
  char command[128];
  if (client == REDIS_POOL_TEST_INVALID) return;
  server->accepted = 1;
  while (server->commands < 2) {
    int received = recv(client, command, (int)sizeof(command), 0);
    if (received <= 0) break;
    server->commands++;
    if (send(client, pong, (int)(sizeof(pong) - 1u), 0) !=
        (int)(sizeof(pong) - 1u))
      break;
  }
  redis_pool_test_close_socket(client);
}

static void redis_pool_recovery_server_main(void *argument) {
  static const char pong[] = "+PONG\r\n";
  redis_pool_test_server *server = (redis_pool_test_server *)argument;
  while (server->accepted < 2) {
    redis_pool_test_socket client = accept(server->listener, NULL, NULL);
    char command[128];
    int received;
    if (client == REDIS_POOL_TEST_INVALID) return;
    server->accepted++;
    received = recv(client, command, (int)sizeof(command), 0);
    if (received > 0) {
      server->commands++;
      if (server->accepted == 2)
        (void)send(client, pong, (int)(sizeof(pong) - 1u), 0);
    }
    redis_pool_test_close_socket(client);
  }
}

static cflow_io_native_backend_kind redis_pool_test_backend(void) {
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

static redis_pool_connect_step redis_pool_test_connect(redis_pool *pool,
                                                       redis_io_runtime *runtime) {
  redis_pool_connect_step step;
  do {
    step = redis_pool_connect_next(pool);
    if (step.kind == REDIS_POOL_CONNECT_WAIT)
      check_equal(redis_io_runtime_wait_idle(runtime,
                                             UINT64_C(5000000000)),
                  SALTS_OK);
  } while (step.kind == REDIS_POOL_CONNECT_WAIT);
  return step;
}

static redis_cflow_stream_step redis_pool_test_next(redis_pool_stream *stream,
                                                    redis_io_runtime *runtime) {
  redis_cflow_stream_step step;
  do {
    step = redis_pool_stream_next(stream);
    if (step.kind == REDIS_CFLOW_STREAM_WAIT)
      check_equal(redis_io_runtime_wait_idle(runtime,
                                             UINT64_C(5000000000)),
                  SALTS_OK);
  } while (step.kind == REDIS_CFLOW_STREAM_WAIT);
  return step;
}

suite("redis CFlow connection pool") {
  it("rejects invalid fixed-capacity configuration") {
    redis_pool pool = {0};
    redis_pool_config config = REDIS_POOL_CONFIG_INIT;
    check_equal(redis_pool_init(&pool, &config), SALTS_EINVAL);
    config.connection_capacity = 0u;
    check_equal(redis_pool_init(&pool, &config), SALTS_EINVAL);
  }

  it("holds one bounded lease until the command stream reaches terminal") {
    static const char *ping[] = {"PING"};
    redis_pool_test_server server = {0};
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {
        redis_pool_test_backend(), 4u, 4u};
    redis_pool pool = {0};
    redis_pool_config config = REDIS_POOL_CONFIG_INIT;
    redis_pool_stream first = {0};
    redis_pool_stream rejected = {0};
    redis_pool_stream second = {0};
    redis_pool_connect_step connected;
    redis_cflow_stream_step step;
    redis_pool_stats stats;
    uint16_t port = 0u;

    server.listener = REDIS_POOL_TEST_INVALID;
    check_equal(redis_io_runtime_init(&runtime, &runtime_config), SALTS_OK);
    check_equal(redis_pool_test_listener(&server.listener, &port), 0);
    check_equal(salts_thread_create(&server.thread,
                                    redis_pool_test_server_main, &server),
                SALTS_OK);
    config.runtime = &runtime;
    config.host = "127.0.0.1";
    config.port = port;
    config.connection_capacity = 1u;
    check_equal(redis_pool_init(&pool, &config), SALTS_OK);
    connected = redis_pool_test_connect(&pool, &runtime);
    check_equal(connected.kind, REDIS_POOL_CONNECT_DONE);
    check_equal(connected.status, SALTS_OK);
    check_equal(connected.connected_connections, 1u);

    check_equal(redis_pool_command_open(&pool, 1, ping, NULL, 1024u, &first),
                SALTS_OK);
    check_equal(redis_pool_command_open(&pool, 1, ping, NULL, 1024u,
                                        &rejected),
                SALTS_ENOBUFS);
    step = redis_pool_test_next(&first, &runtime);
    check_equal(step.kind, REDIS_CFLOW_STREAM_ITEM);
    check_equal(step.item->type, REDIS_REPLY_STRING);
    check_equal(step.item->str, "PONG", 4u);
    redis_reply_free(step.item);
    check_equal(redis_pool_command_open(&pool, 1, ping, NULL, 1024u,
                                        &rejected),
                SALTS_ENOBUFS);
    step = redis_pool_test_next(&first, &runtime);
    check_equal(step.kind, REDIS_CFLOW_STREAM_DONE);
    check_equal(redis_pool_stream_destroy(&first), SALTS_OK);

    check_equal(redis_pool_command_open(&pool, 1, ping, NULL, 1024u, &second),
                SALTS_OK);
    step = redis_pool_test_next(&second, &runtime);
    check_equal(step.kind, REDIS_CFLOW_STREAM_ITEM);
    redis_reply_free(step.item);
    step = redis_pool_test_next(&second, &runtime);
    check_equal(step.kind, REDIS_CFLOW_STREAM_DONE);
    check_equal(redis_pool_stream_destroy(&second), SALTS_OK);

    redis_pool_get_stats(&pool, &stats);
    check_equal(stats.connection_capacity, 1u);
    check_equal(stats.idle_connections, 1u);
    check_equal(stats.borrowed_connections, 0u);
    check_equal(stats.rejected_commands, UINT64_C(2));
    check_equal(stats.completed_commands, UINT64_C(2));
    check_equal(redis_pool_close(&pool), SALTS_OK);
    check_equal(redis_pool_destroy(&pool), SALTS_OK);
    check_equal(redis_io_runtime_close(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_OK);
    check_equal(salts_thread_join(&server.thread), SALTS_OK);
    salts_thread_destroy(&server.thread);
    redis_pool_test_close_socket(server.listener);
    check_equal(server.accepted, 1);
    check_equal(server.commands, 2);
  }

  it("reconnects an invalid slot through the pool connect state machine") {
    static const char *ping[] = {"PING"};
    redis_pool_test_server server = {0};
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {
        redis_pool_test_backend(), 4u, 4u};
    redis_pool pool = {0};
    redis_pool_config config = REDIS_POOL_CONFIG_INIT;
    redis_pool_stream cancelled = {0};
    redis_pool_stream recovered = {0};
    redis_pool_connect_step connected;
    redis_cflow_stream_step step;
    uint16_t port = 0u;

    server.listener = REDIS_POOL_TEST_INVALID;
    check_equal(redis_io_runtime_init(&runtime, &runtime_config), SALTS_OK);
    check_equal(redis_pool_test_listener(&server.listener, &port), 0);
    check_equal(salts_thread_create(&server.thread,
                                    redis_pool_recovery_server_main, &server),
                SALTS_OK);
    config.runtime = &runtime;
    config.host = "127.0.0.1";
    config.port = port;
    config.connection_capacity = 1u;
    check_equal(redis_pool_init(&pool, &config), SALTS_OK);
    connected = redis_pool_test_connect(&pool, &runtime);
    check_equal(connected.kind, REDIS_POOL_CONNECT_DONE);

    check_equal(redis_pool_command_open(&pool, 1, ping, NULL, 1024u,
                                        &cancelled),
                SALTS_OK);
    step = redis_pool_stream_next(&cancelled);
    check_equal(step.kind, REDIS_CFLOW_STREAM_WAIT);
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)),
                SALTS_OK);
    step = redis_pool_stream_next(&cancelled);
    check_equal(step.kind, REDIS_CFLOW_STREAM_WAIT);
    check_equal(redis_pool_stream_cancel(&cancelled), SALTS_OK);
    check_equal(redis_pool_stream_destroy(&cancelled), SALTS_OK);

    connected = redis_pool_test_connect(&pool, &runtime);
    check_equal(connected.kind, REDIS_POOL_CONNECT_DONE);
    check_equal(connected.status, SALTS_OK);
    {
      int opened = redis_pool_command_open(&pool, 1, ping, NULL, 1024u,
                                           &recovered);
      check_equal(opened, SALTS_OK);
      if (opened == SALTS_OK) {
        step = redis_pool_test_next(&recovered, &runtime);
        check_equal(step.kind, REDIS_CFLOW_STREAM_ITEM);
        check_equal(step.item->type, REDIS_REPLY_STRING);
        redis_reply_free(step.item);
        step = redis_pool_test_next(&recovered, &runtime);
        check_equal(step.kind, REDIS_CFLOW_STREAM_DONE);
        check_equal(redis_pool_stream_destroy(&recovered), SALTS_OK);
      }
    }

    check_equal(redis_pool_close(&pool), SALTS_OK);
    check_equal(redis_pool_destroy(&pool), SALTS_OK);
    check_equal(redis_io_runtime_close(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_OK);
    redis_pool_test_close_socket(server.listener);
    server.listener = REDIS_POOL_TEST_INVALID;
    check_equal(salts_thread_join(&server.thread), SALTS_OK);
    salts_thread_destroy(&server.thread);
    check_equal(server.accepted, 2);
    check_equal(server.commands, 2);
  }
}
