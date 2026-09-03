#include "../redis_sentinel.h"

#include "tinytest.h"
#include "salts_error.h"
#include "salts_thread.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET redis_sentinel_test_socket;
  #define REDIS_SENTINEL_TEST_INVALID INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
typedef int redis_sentinel_test_socket;
  #define REDIS_SENTINEL_TEST_INVALID (-1)
#endif

typedef struct redis_sentinel_test_server {
  redis_sentinel_test_socket listener;
  salts_thread_t thread;
  uint16_t port;
  int connections;
} redis_sentinel_test_server;

static void redis_sentinel_test_close(redis_sentinel_test_socket value) {
  if (value == REDIS_SENTINEL_TEST_INVALID) return;
#if defined(_WIN32)
  (void)closesocket(value);
#else
  (void)close(value);
#endif
}

static int redis_sentinel_test_listen(redis_sentinel_test_server *server) {
  struct sockaddr_in address;
#if defined(_WIN32)
  int address_size = (int)sizeof(address);
#else
  socklen_t address_size = (socklen_t)sizeof(address);
#endif
  server->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server->listener == REDIS_SENTINEL_TEST_INVALID) return -1;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(server->listener, (const struct sockaddr *)&address, (int)sizeof(address)) != 0 ||
      getsockname(server->listener, (struct sockaddr *)&address, &address_size) != 0 ||
      listen(server->listener, 2) != 0)
    return -1;
  server->port = ntohs(address.sin_port);
  return 0;
}

static void redis_sentinel_test_server_main(void *argument) {
  redis_sentinel_test_server *server = (redis_sentinel_test_server *)argument;
  char discovery[128];
  char port_text[6];
  int port_size = snprintf(port_text, sizeof(port_text), "%u", (unsigned)server->port);
  int discovery_size = snprintf(discovery, sizeof(discovery),
                                "*2\r\n$9\r\n127.0.0.1\r\n$%d\r\n%s\r\n", port_size, port_text);
  while (server->connections < 2) {
    redis_sentinel_test_socket client = accept(server->listener, NULL, NULL);
    char command[256];
    int received;
    if (client == REDIS_SENTINEL_TEST_INVALID) return;
    server->connections++;
    received = recv(client, command, (int)sizeof(command), 0);
    if (received > 0) {
      if (server->connections == 1) (void)send(client, discovery, discovery_size, 0);
      else (void)send(client, "+PONG\r\n", 7, 0);
    }
    if (server->connections == 1) {
      char extra;
      while (recv(client, &extra, 1, 0) > 0) {
      }
    }
    redis_sentinel_test_close(client);
  }
}

static cflow_io_native_backend_kind redis_sentinel_test_backend(void) {
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

suite("redis CFlow Sentinel") {
  it("discovers a master and delegates commands to its bounded pool") {
    static const char *ping[] = {"PING"};
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[1];
    redis_sentinel_test_server server = {0};
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {redis_sentinel_test_backend(), 8u, 8u};
    redis_sentinel sentinel = {0};
    redis_sentinel_config config = REDIS_SENTINEL_CONFIG_INIT;
    redis_sentinel_connect_step connected;
    redis_pool_stream command = {0};
    redis_cflow_stream_step step;
    redis_sentinel_master master;

    server.listener = REDIS_SENTINEL_TEST_INVALID;
    check_equal(redis_io_runtime_init(&runtime, &runtime_config), SALTS_OK);
    check_equal(redis_sentinel_test_listen(&server), 0);
    ports[0] = server.port;
    check_equal(salts_thread_create(&server.thread, redis_sentinel_test_server_main, &server),
                SALTS_OK);
    config.runtime = &runtime;
    config.sentinel_hosts = hosts;
    config.sentinel_ports = ports;
    config.sentinel_count = 1u;
    config.service_name = "primary";
    config.connection_capacity = 1u;
    check_equal(redis_sentinel_init(&sentinel, &config), SALTS_OK);
    connected = redis_sentinel_connect_next(&sentinel);
    while (connected.kind == REDIS_SENTINEL_CONNECT_WAIT) {
      check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), SALTS_OK);
      connected = redis_sentinel_connect_next(&sentinel);
    }
    check_equal(connected.kind, REDIS_SENTINEL_CONNECT_DONE);
    check_equal(connected.status, SALTS_OK);
    check_equal(redis_sentinel_get_master(&sentinel, &master), SALTS_OK);
    check_equal(master.host, "127.0.0.1");
    check_equal(master.port, server.port);
    check_equal(redis_sentinel_command_open(&sentinel, 1, ping, NULL, 1024u, &command), SALTS_OK);
    do {
      step = redis_pool_stream_next(&command);
      if (step.kind == REDIS_CFLOW_STREAM_WAIT)
        check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), SALTS_OK);
    } while (step.kind == REDIS_CFLOW_STREAM_WAIT);
    check_equal(step.kind, REDIS_CFLOW_STREAM_ITEM);
    check_equal(step.item->str, "PONG", 4u);
    redis_reply_free(step.item);
    check_equal(redis_pool_stream_next(&command).kind, REDIS_CFLOW_STREAM_DONE);
    check_equal(redis_pool_stream_destroy(&command), SALTS_OK);
    check_equal(redis_sentinel_close(&sentinel), SALTS_OK);
    check_equal(redis_sentinel_destroy(&sentinel), SALTS_OK);
    check_equal(redis_io_runtime_close(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_OK);
    check_equal(salts_thread_join(&server.thread), SALTS_OK);
    salts_thread_destroy(&server.thread);
    redis_sentinel_test_close(server.listener);
    check_equal(server.connections, 2);
  }
}
