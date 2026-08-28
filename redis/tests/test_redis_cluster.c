#include "../redis_cluster.h"

#include "tinytest.h"
#include "turbo_error.h"
#include "turbo_thread.h"

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET redis_cluster_test_socket;
  #define REDIS_CLUSTER_TEST_INVALID INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
typedef int redis_cluster_test_socket;
  #define REDIS_CLUSTER_TEST_INVALID (-1)
#endif

typedef struct redis_cluster_test_server {
  redis_cluster_test_socket listener;
  turbo_thread_t thread;
  uint16_t port;
  int connections;
  int commands;
} redis_cluster_test_server;

static void redis_cluster_test_close(redis_cluster_test_socket socket_value) {
  if (socket_value == REDIS_CLUSTER_TEST_INVALID) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static int redis_cluster_test_listen(redis_cluster_test_server *server) {
  struct sockaddr_in address;
#if defined(_WIN32)
  int address_size = (int)sizeof(address);
#else
  socklen_t address_size = (socklen_t)sizeof(address);
#endif
  server->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server->listener == REDIS_CLUSTER_TEST_INVALID) return -1;
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

static void redis_cluster_test_server_main(void *argument) {
  redis_cluster_test_server *server = (redis_cluster_test_server *)argument;
  char command[256];
  char topology[256];
  int topology_size =
      snprintf(topology, sizeof(topology),
               "*1\r\n*3\r\n:0\r\n:16383\r\n*3\r\n$9\r\n127.0.0.1\r\n:%u\r\n$6\r\nnode-1\r\n",
               (unsigned)server->port);
  while (server->connections < 2) {
    redis_cluster_test_socket client = accept(server->listener, NULL, NULL);
    int received;
    if (client == REDIS_CLUSTER_TEST_INVALID) return;
    server->connections++;
    received = recv(client, command, (int)sizeof(command), 0);
    if (received > 0) {
      server->commands++;
      if (server->connections == 1) (void)send(client, topology, topology_size, 0);
      else (void)send(client, "$5\r\nvalue\r\n", 11, 0);
    }
    if (server->connections == 1) {
      char extra;
      while (recv(client, &extra, 1, 0) > 0) {
      }
    }
    redis_cluster_test_close(client);
  }
}

static cflow_io_native_backend_kind redis_cluster_test_backend(void) {
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

suite("redis CFlow cluster") {
  it("preserves Redis hash tags in CRC16 slot routing") {
    check_equal(redis_cluster_keyslot("123456789", 9u), 12739u);
    check_equal(redis_cluster_keyslot("user:{42}:a", 11u),
                redis_cluster_keyslot("other:{42}:b", 12u));
    check_true(redis_cluster_keyslot("user:42:a", 9u) < REDIS_CLUSTER_SLOT_COUNT);
  }

  it("discovers CLUSTER SLOTS and routes a command through a node pool") {
    static const char *get[] = {"GET", "key"};
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[1];
    redis_cluster_test_server server = {0};
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {redis_cluster_test_backend(), 8u, 8u};
    redis_cluster cluster = {0};
    redis_cluster_config config = REDIS_CLUSTER_CONFIG_INIT;
    redis_cluster_connect_step connected;
    redis_pool_stream command = {0};
    redis_cflow_stream_step step;
    const redis_cluster_node *node;

    server.listener = REDIS_CLUSTER_TEST_INVALID;
    check_equal(redis_io_runtime_init(&runtime, &runtime_config), TURBO_OK);
    check_equal(redis_cluster_test_listen(&server), 0);
    ports[0] = server.port;
    check_equal(turbo_thread_create(&server.thread, redis_cluster_test_server_main, &server),
                TURBO_OK);
    config.runtime = &runtime;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1u;
    config.connections_per_node = 1u;
    config.max_nodes = 2u;
    check_equal(redis_cluster_init(&cluster, &config), TURBO_OK);
    connected = redis_cluster_connect_next(&cluster);
    while (connected.kind == REDIS_CLUSTER_CONNECT_WAIT) {
      check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), TURBO_OK);
      connected = redis_cluster_connect_next(&cluster);
    }
    check_equal(connected.kind, REDIS_CLUSTER_CONNECT_DONE);
    check_equal(connected.status, TURBO_OK);
    check_equal(connected.node_count, 1u);
    node = redis_cluster_node_for_slot(&cluster, 0u);
    check_not_null(node);
    check_equal(node->port, server.port);

    check_equal(redis_cluster_command_open(&cluster, "key", 3u, 2, get, NULL, 1024u, &command),
                TURBO_OK);
    do {
      step = redis_pool_stream_next(&command);
      if (step.kind == REDIS_CFLOW_STREAM_WAIT)
        check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), TURBO_OK);
    } while (step.kind == REDIS_CFLOW_STREAM_WAIT);
    check_equal(step.kind, REDIS_CFLOW_STREAM_ITEM);
    check_equal(step.item->type, REDIS_REPLY_BULK_STRING);
    check_equal(step.item->str, "value", 5u);
    redis_reply_free(step.item);
    check_equal(redis_pool_stream_next(&command).kind, REDIS_CFLOW_STREAM_DONE);
    check_equal(redis_pool_stream_destroy(&command), TURBO_OK);
    check_equal(redis_cluster_close(&cluster), TURBO_OK);
    check_equal(redis_cluster_destroy(&cluster), TURBO_OK);
    check_equal(redis_io_runtime_close(&runtime), TURBO_OK);
    check_equal(redis_io_runtime_destroy(&runtime), TURBO_OK);
    check_equal(turbo_thread_join(&server.thread), TURBO_OK);
    turbo_thread_destroy(&server.thread);
    redis_cluster_test_close(server.listener);
    check_equal(server.connections, 2);
    check_equal(server.commands, 2);
  }
}
