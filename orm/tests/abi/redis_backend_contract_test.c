#include "orm.h"
#include <orm_runtime.h>
#include "salts_error.h"
#include "salts_thread.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET orm_redis_test_socket;
#define ORM_REDIS_TEST_INVALID_SOCKET INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int orm_redis_test_socket;
#define ORM_REDIS_TEST_INVALID_SOCKET (-1)
#endif

enum { ORM_REDIS_TEST_RECEIVE_TIMEOUT_MS = 5000 };

typedef struct orm_redis_test_server {
  orm_redis_test_socket listener;
  salts_thread_t thread;
  uint16_t port;
  int accepted;
  int valid_command;
  int sent_reply;
  int client_closed;
} orm_redis_test_server;

static orm_string_view_t view(const char *text) {
  return orm_view(text);
}

static const char *orm_redis_plugin_path(void) {
  const char *path = getenv("ORM_REDIS_PLUGIN");
  return path != NULL ? path : "";
}

static orm_runtime_t *orm_redis_test_runtime(orm_error_t *error) {
  orm_runtime_config_t config;
  orm_runtime_t *runtime = NULL;
  orm_driver_load_config_t load;
  orm_driver_info_t info;
  const uint64_t required =
      ORM_DRIVER_CAP_SELECT | ORM_DRIVER_CAP_INSERT |
      ORM_DRIVER_CAP_UPDATE | ORM_DRIVER_CAP_DELETE |
      ORM_DRIVER_CAP_INCREMENTAL_ROWS;
  const uint64_t forbidden =
      ORM_DRIVER_CAP_TRANSACTION | ORM_DRIVER_CAP_RAW_SQL;

  if (orm_redis_plugin_path()[0] == '\0') {
    fprintf(stderr, "ORM_REDIS_PLUGIN is not configured\n");
    return NULL;
  }

  orm_runtime_config_init(&config);
  orm_error_init(error);
  if (orm_runtime_create(&config, &runtime, error) != ORM_STATUS_OK ||
      runtime == NULL) {
    fprintf(stderr, "create Redis Plugin runtime failed: %s\n", error->message);
    return NULL;
  }

  memset(&load, 0, sizeof(load));
  load.struct_size = (uint32_t)sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.module_path = view(orm_redis_plugin_path());
  load.expected_driver_id = view("redis");
  if (orm_runtime_load_driver(runtime, &load, error) != ORM_STATUS_OK) {
    fprintf(stderr, "load Redis Driver Plugin failed: %s\n", error->message);
    (void)orm_runtime_close(runtime, error);
    orm_runtime_release(runtime);
    return NULL;
  }

  memset(&info, 0, sizeof(info));
  if (orm_runtime_driver_info(runtime, view("redis"), &info, error) !=
          ORM_STATUS_OK ||
      info.canonical_id_size != 5u ||
      memcmp(info.canonical_id, "redis", 5u) != 0 ||
      (info.capabilities & required) != required ||
      (info.capabilities & forbidden) != 0u) {
    fprintf(stderr, "Redis Driver capability contract mismatch: %s\n",
            error->message);
    (void)orm_runtime_close(runtime, error);
    orm_runtime_release(runtime);
    return NULL;
  }
  return runtime;
}

static int orm_redis_test_socket_runtime_init(void) {
#if defined(_WIN32)
  WSADATA data;
  return WSAStartup(MAKEWORD(2, 2), &data) == 0 ? 0 : -1;
#else
  return 0;
#endif
}

static void orm_redis_test_socket_runtime_destroy(void) {
#if defined(_WIN32)
  (void)WSACleanup();
#endif
}

static void orm_redis_test_close_socket(orm_redis_test_socket socket_value) {
  if (socket_value == ORM_REDIS_TEST_INVALID_SOCKET) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static int orm_redis_test_listen(orm_redis_test_server *server) {
  struct sockaddr_in address;
#if defined(_WIN32)
  int address_size = (int)sizeof(address);
#else
  socklen_t address_size = (socklen_t)sizeof(address);
#endif
  server->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server->listener == ORM_REDIS_TEST_INVALID_SOCKET) return -1;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(server->listener, (const struct sockaddr *)&address,
           (int)sizeof(address)) != 0 ||
      getsockname(server->listener, (struct sockaddr *)&address,
                  &address_size) != 0 ||
      listen(server->listener, 1) != 0) {
    orm_redis_test_close_socket(server->listener);
    server->listener = ORM_REDIS_TEST_INVALID_SOCKET;
    return -1;
  }
  server->port = ntohs(address.sin_port);
  return 0;
}

static int orm_redis_test_send_all(orm_redis_test_socket client,
                                   const char *bytes, size_t length) {
  size_t sent = 0u;
  while (sent < length) {
    int result = send(client, bytes + sent, (int)(length - sent), 0);
    if (result <= 0) return -1;
    sent += (size_t)result;
  }
  return 0;
}

static int orm_redis_test_set_receive_timeout(orm_redis_test_socket client) {
#if defined(_WIN32)
  DWORD timeout = ORM_REDIS_TEST_RECEIVE_TIMEOUT_MS;
  return setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
                    (int)sizeof(timeout));
#else
  struct timeval timeout = {ORM_REDIS_TEST_RECEIVE_TIMEOUT_MS / 1000,
                            (ORM_REDIS_TEST_RECEIVE_TIMEOUT_MS % 1000) * 1000};
  return setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                    (socklen_t)sizeof(timeout));
#endif
}

static void orm_redis_missing_command_server_main(void *argument) {
  static const char expected_command[] =
      "*3\r\n$7\r\nCOMMAND\r\n$4\r\nINFO\r\n$9\r\nFT.SEARCH\r\n";
  static const char missing_command_reply[] = "*1\r\n$-1\r\n";
  orm_redis_test_server *server = (orm_redis_test_server *)argument;
  orm_redis_test_socket client = accept(server->listener, NULL, NULL);
  char command[sizeof(expected_command) - 1u];
  size_t received_bytes = 0u;
  if (client == ORM_REDIS_TEST_INVALID_SOCKET) return;
  server->accepted = 1;
  if (orm_redis_test_set_receive_timeout(client) != 0) {
    orm_redis_test_close_socket(client);
    return;
  }
  while (received_bytes < sizeof(command)) {
    int received =
        recv(client, command + received_bytes,
             (int)(sizeof(command) - received_bytes), 0);
    if (received <= 0) break;
    received_bytes += (size_t)received;
  }
  server->valid_command =
      received_bytes == sizeof(command) &&
      memcmp(command, expected_command, sizeof(command)) == 0;
  if (server->valid_command &&
      orm_redis_test_send_all(client, missing_command_reply,
                              sizeof(missing_command_reply) - 1u) == 0)
    server->sent_reply = 1;
  orm_redis_test_close_socket(client);
}


static void orm_redis_available_command_server_main(void *argument) {
  static const char expected_command[] =
      "*3\r\n$7\r\nCOMMAND\r\n$4\r\nINFO\r\n$9\r\nFT.SEARCH\r\n";
  /* The ORM only requires COMMAND INFO to contain one array entry for
   * FT.SEARCH; an empty command-info row is sufficient for this transport
   * contract and keeps the fixture independent from a Redis server binary. */
  static const char available_command_reply[] = "*1\r\n*0\r\n";
  orm_redis_test_server *server = (orm_redis_test_server *)argument;
  orm_redis_test_socket client = accept(server->listener, NULL, NULL);
  char command[sizeof(expected_command) - 1u];
  char tail;
  size_t received_bytes = 0u;

  if (client == ORM_REDIS_TEST_INVALID_SOCKET) return;
  server->accepted = 1;
  if (orm_redis_test_set_receive_timeout(client) != 0) {
    orm_redis_test_close_socket(client);
    return;
  }
  while (received_bytes < sizeof(command)) {
    int received =
        recv(client, command + received_bytes,
             (int)(sizeof(command) - received_bytes), 0);
    if (received <= 0) break;
    received_bytes += (size_t)received;
  }
  server->valid_command =
      received_bytes == sizeof(command) &&
      memcmp(command, expected_command, sizeof(command)) == 0;
  if (server->valid_command &&
      orm_redis_test_send_all(client, available_command_reply,
                              sizeof(available_command_reply) - 1u) == 0)
    server->sent_reply = 1;

  if (server->sent_reply) {
    const int received = recv(client, &tail, 1, 0);
    server->client_closed = received == 0;
  }
  orm_redis_test_close_socket(client);
}

static int orm_redis_test_missing_query_engine(orm_runtime_t *runtime) {
  orm_redis_test_server server = {0};
  orm_config_t config;
  orm_error_t error;
  orm_connection_t *connection = NULL;
  orm_option_t options[2];
  orm_status_t status;
  char port[6];
  int failed = 0;

  server.listener = ORM_REDIS_TEST_INVALID_SOCKET;
  if (orm_redis_test_socket_runtime_init() != 0) {
    fprintf(stderr, "initialize Redis test socket runtime failed\n");
    return 1;
  }
  if (orm_redis_test_listen(&server) != 0) {
    fprintf(stderr, "create Redis test listener failed\n");
    orm_redis_test_socket_runtime_destroy();
    return 1;
  }
  if (salts_thread_create(&server.thread,
                          orm_redis_missing_command_server_main,
                          &server) != SALTS_OK) {
    fprintf(stderr, "create Redis test server thread failed\n");
    orm_redis_test_close_socket(server.listener);
    orm_redis_test_socket_runtime_destroy();
    return 1;
  }

  (void)snprintf(port, sizeof(port), "%u", (unsigned int)server.port);
  options[0] = (orm_option_t){view("host"), view("127.0.0.1")};
  options[1] = (orm_option_t){view("port"), view(port)};
  orm_config(&config);
  orm_error_init(&error);
  config.driver = view("redis");
  config.options = options;
  config.option_count = sizeof(options) / sizeof(options[0]);
  status = orm_runtime_connect(runtime, &config, &connection, &error);
  orm_disconnect(connection);

  orm_redis_test_close_socket(server.listener);
  server.listener = ORM_REDIS_TEST_INVALID_SOCKET;
  if (salts_thread_join(&server.thread) != SALTS_OK) {
    fprintf(stderr, "join Redis test server thread failed\n");
    failed = 1;
  }
  salts_thread_destroy(&server.thread);
  orm_redis_test_socket_runtime_destroy();

  if (!server.accepted || !server.valid_command || !server.sent_reply) {
    fprintf(stderr, "Redis ORM capability probe did not complete\n");
    failed = 1;
  }
  if (status != ORM_STATUS_UNSUPPORTED || connection != NULL ||
      strstr(error.message, "FT.SEARCH") == NULL) {
    fprintf(stderr, "Redis ORM accepted a missing Query Engine: status=%d, %s\n",
            (int)status, error.message);
    failed = 1;
  }
  return failed;
}

static int orm_redis_test_supported_connection(orm_runtime_t *runtime) {
  orm_redis_test_server server = {0};
  orm_config_t config;
  orm_error_t error;
  orm_connection_t *connection = NULL;
  orm_transaction_t *transaction = NULL;
  orm_option_t options[2];
  orm_status_t status;
  char port[6];
  int failed = 0;

  server.listener = ORM_REDIS_TEST_INVALID_SOCKET;
  if (orm_redis_test_socket_runtime_init() != 0) {
    fprintf(stderr, "initialize Redis success-fixture socket runtime failed\n");
    return 1;
  }
  if (orm_redis_test_listen(&server) != 0) {
    fprintf(stderr, "create Redis success-fixture listener failed\n");
    orm_redis_test_socket_runtime_destroy();
    return 1;
  }
  if (salts_thread_create(&server.thread,
                          orm_redis_available_command_server_main,
                          &server) != SALTS_OK) {
    fprintf(stderr, "create Redis success-fixture thread failed\n");
    orm_redis_test_close_socket(server.listener);
    orm_redis_test_socket_runtime_destroy();
    return 1;
  }

  (void)snprintf(port, sizeof(port), "%u", (unsigned int)server.port);
  options[0] = (orm_option_t){view("host"), view("127.0.0.1")};
  options[1] = (orm_option_t){view("port"), view(port)};
  orm_config(&config);
  orm_error_init(&error);
  config.driver = view("redis");
  config.options = options;
  config.option_count = sizeof(options) / sizeof(options[0]);

  status = orm_runtime_connect(runtime, &config, &connection, &error);
  if (status != ORM_STATUS_OK || connection == NULL) {
    fprintf(stderr, "Redis Driver failed a supported connection: status=%d, %s\n",
            (int)status, error.message);
    failed = 1;
  } else {
    orm_error_init(&error);
    status = orm_transaction_begin(connection, ORM_ISOLATION_SERIALIZABLE,
                                   &transaction, &error);
    if (status != ORM_STATUS_UNSUPPORTED || transaction != NULL ||
        strstr(error.message, "unsupported") == NULL) {
      fprintf(stderr, "Redis Driver did not reject ORM transactions: status=%d, %s\n",
              (int)status, error.message);
      orm_transaction_destroy(transaction);
      transaction = NULL;
      failed = 1;
    }

    orm_error_init(&error);
    if (orm_runtime_close(runtime, &error) != ORM_STATUS_BUSY) {
      fprintf(stderr, "Redis live connection did not retain the Plugin lease: %s\n",
              error.message);
      failed = 1;
    }

    orm_disconnect(connection);
    connection = NULL;
  }

  orm_redis_test_close_socket(server.listener);
  server.listener = ORM_REDIS_TEST_INVALID_SOCKET;
  if (salts_thread_join(&server.thread) != SALTS_OK) {
    fprintf(stderr, "join Redis success-fixture thread failed\n");
    failed = 1;
  }
  salts_thread_destroy(&server.thread);
  orm_redis_test_socket_runtime_destroy();

  if (!server.accepted || !server.valid_command || !server.sent_reply ||
      !server.client_closed) {
    fprintf(stderr, "Redis supported connection fixture did not complete cleanly\n");
    failed = 1;
  }
  return failed;
}

int main(void) {
  orm_runtime_t *runtime;
  orm_config_t config;
  orm_error_t error;
  orm_connection_t *connection = NULL;
  orm_option_t invalid_option = {view("scan_fallback"), view("true")};
  int failed = 0;

  runtime = orm_redis_test_runtime(&error);
  if (runtime == NULL)
    return 1;

  orm_config(&config);
  orm_error_init(&error);
  config.driver = view("redis");
  config.options = &invalid_option;
  config.option_count = 1;
  if (orm_runtime_connect(runtime, &config, &connection, &error) !=
          ORM_STATUS_INVALID_ARGUMENT ||
      connection != NULL) {
    fprintf(stderr, "Redis ORM accepted an unknown/fallback option: %s\n",
            error.message);
    orm_disconnect(connection);
    connection = NULL;
    failed = 1;
  }

  invalid_option.keyword = view("port");
  invalid_option.value = view("1");
  config.options = &invalid_option;
  config.option_count = 1;
  orm_error_init(&error);
  if (orm_runtime_connect(runtime, &config, &connection, &error) !=
          ORM_STATUS_CONNECTION_ERROR ||
      connection != NULL || strstr(error.message, "CFlow") == NULL) {
    fprintf(stderr, "Redis ORM did not report its CFlow connect failure: %s\n",
            error.message);
    orm_disconnect(connection);
    connection = NULL;
    failed = 1;
  }

  if (orm_redis_test_missing_query_engine(runtime) != 0)
    failed = 1;

  /* Run the successful connection last: its BUSY close check intentionally
   * transitions the runtime into resumable closing state. */
  if (orm_redis_test_supported_connection(runtime) != 0)
    failed = 1;

  orm_error_init(&error);
  if (orm_runtime_close(runtime, &error) != ORM_STATUS_OK) {
    fprintf(stderr, "close Redis Plugin runtime failed: %s\n", error.message);
    failed = 1;
  }
  orm_runtime_release(runtime);
  return failed;
}
