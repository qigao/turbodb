#include "../redis_lua_apply.h"

#include "tinytest.h"
#include "salts_error.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define REDIS_LUA_APPLY_TEST_WAIT_TIMEOUT_NS UINT64_C(5000000000)
#define REDIS_LUA_APPLY_TEST_MAX_STEPS 16u

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
typedef SOCKET redis_lua_apply_test_socket;
  #define REDIS_LUA_APPLY_TEST_INVALID INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
typedef int redis_lua_apply_test_socket;
  #define REDIS_LUA_APPLY_TEST_INVALID (-1)
#endif

static int redis_lua_apply_test_socket_error(void) {
#if defined(_WIN32)
  return -WSAGetLastError();
#else
  return -errno;
#endif
}

static void redis_lua_apply_test_close(redis_lua_apply_test_socket socket_value) {
  if (socket_value == REDIS_LUA_APPLY_TEST_INVALID) return;
#if defined(_WIN32)
  (void)closesocket(socket_value);
#else
  (void)close(socket_value);
#endif
}

static int redis_lua_apply_test_nonblocking(redis_lua_apply_test_socket socket_value) {
#if defined(_WIN32)
  u_long enabled = 1u;
  return ioctlsocket(socket_value, FIONBIO, &enabled) == 0 ? SALTS_OK
                                                           : redis_lua_apply_test_socket_error();
#else
  int flags = fcntl(socket_value, F_GETFL);
  if (flags < 0) return -errno;
  return fcntl(socket_value, F_SETFL, flags | O_NONBLOCK) == 0 ? SALTS_OK : -errno;
#endif
}

static int redis_lua_apply_test_pair(redis_lua_apply_test_socket sockets[2]) {
  redis_lua_apply_test_socket listener = REDIS_LUA_APPLY_TEST_INVALID;
  struct sockaddr_in address;
#if defined(_WIN32)
  int address_size = (int)sizeof(address);
#else
  socklen_t address_size = (socklen_t)sizeof(address);
#endif
  int status = SALTS_OK;
  sockets[0] = REDIS_LUA_APPLY_TEST_INVALID;
  sockets[1] = REDIS_LUA_APPLY_TEST_INVALID;
  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == REDIS_LUA_APPLY_TEST_INVALID) return redis_lua_apply_test_socket_error();
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listener, (const struct sockaddr *)&address, (int)sizeof(address)) != 0 ||
      getsockname(listener, (struct sockaddr *)&address, &address_size) != 0 ||
      listen(listener, 1) != 0)
    status = redis_lua_apply_test_socket_error();
  if (status == SALTS_OK) {
    sockets[0] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockets[0] == REDIS_LUA_APPLY_TEST_INVALID) status = redis_lua_apply_test_socket_error();
  }
  if (status == SALTS_OK &&
      connect(sockets[0], (const struct sockaddr *)&address, (int)sizeof(address)) != 0)
    status = redis_lua_apply_test_socket_error();
  if (status == SALTS_OK) {
    sockets[1] = accept(listener, NULL, NULL);
    if (sockets[1] == REDIS_LUA_APPLY_TEST_INVALID) status = redis_lua_apply_test_socket_error();
  }
  redis_lua_apply_test_close(listener);
  if (status == SALTS_OK) status = redis_lua_apply_test_nonblocking(sockets[0]);
  if (status == SALTS_OK) status = redis_lua_apply_test_nonblocking(sockets[1]);
  return status;
}

static cflow_io_native_backend_kind redis_lua_apply_test_backend(void) {
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

static redis_lua_apply_request redis_lua_apply_test_request(void) {
  redis_lua_apply_request request = REDIS_LUA_APPLY_REQUEST_INIT;
  request.metadata_key = "raft:{orders}:meta";
  request.metadata_key_length = strlen(request.metadata_key);
  request.state_key = "raft:{orders}:state";
  request.state_key_length = strlen(request.state_key);
  request.outbox_key = "raft:{orders}:outbox";
  request.outbox_key_length = strlen(request.outbox_key);
  request.index = UINT64_C(42);
  request.term = UINT64_C(7);
  request.command_id = "cmd-42";
  request.command_id_length = strlen(request.command_id);
  request.field = "status";
  request.field_length = strlen(request.field);
  request.value = "paid";
  request.value_length = strlen(request.value);
  return request;
}

static redis_lua_apply_step redis_lua_apply_test_complete(
    redis_lua_apply *operation, redis_io_runtime *runtime) {
  redis_lua_apply_step step = REDIS_LUA_APPLY_STEP_INIT;
  size_t attempt;
  for (attempt = 0u; attempt < REDIS_LUA_APPLY_TEST_MAX_STEPS; ++attempt) {
    step = redis_lua_apply_next(operation);
    if (step.kind != REDIS_LUA_APPLY_WAIT) return step;
    if (redis_io_runtime_wait_idle(runtime, REDIS_LUA_APPLY_TEST_WAIT_TIMEOUT_NS) != SALTS_OK) {
      step.kind = REDIS_LUA_APPLY_STEP_ERROR;
      step.receipt.kind = REDIS_LUA_APPLY_ERROR;
      step.receipt.status = SALTS_ETIMEDOUT;
      return step;
    }
  }
  step.kind = REDIS_LUA_APPLY_STEP_ERROR;
  step.receipt.kind = REDIS_LUA_APPLY_ERROR;
  step.receipt.status = SALTS_ETIMEDOUT;
  return step;
}

static redis_reply_t *redis_lua_apply_test_command(
    redis_cflow_connection *connection, redis_io_runtime *runtime, int argc,
    const char **argv) {
  redis_cflow_stream stream = {0};
  redis_cflow_stream_step step;
  size_t attempt;
  if (redis_cflow_command_open(connection, argc, argv, NULL, 4096u, &stream) != SALTS_OK)
    return NULL;
  for (attempt = 0u; attempt < REDIS_LUA_APPLY_TEST_MAX_STEPS; ++attempt) {
    step = redis_cflow_stream_next(&stream);
    if (step.kind == REDIS_CFLOW_STREAM_WAIT) {
      if (redis_io_runtime_wait_idle(runtime, REDIS_LUA_APPLY_TEST_WAIT_TIMEOUT_NS) != SALTS_OK)
        break;
      continue;
    }
    if (step.kind == REDIS_CFLOW_STREAM_ITEM) {
      redis_reply_t *reply = step.item;
      step = redis_cflow_stream_next(&stream);
      if (step.kind == REDIS_CFLOW_STREAM_DONE) {
        (void)redis_cflow_stream_destroy(&stream);
        return reply;
      }
      redis_reply_free(reply);
      redis_reply_free(step.item);
      break;
    }
    redis_reply_free(step.item);
    break;
  }
  (void)redis_cflow_stream_destroy(&stream);
  return NULL;
}

suite("redis Lua indexed apply") {
  it("emits a bounded EVAL and exposes an applied receipt") {
    static const char reply[] = "*2\r\n+APPLIED\r\n:42\r\n";
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {redis_lua_apply_test_backend(), 1u, 1u};
    redis_lua_apply_test_socket sockets[2];
    redis_cflow_connection connection = {0};
    redis_cflow_connection_config connection_config;
    redis_lua_apply_request request = redis_lua_apply_test_request();
    redis_lua_apply operation = {0};
    redis_lua_apply_step step;
    char command[2048] = {0};
    int received;

    check_equal(redis_io_runtime_init(&runtime, &runtime_config), SALTS_OK);
    check_equal(redis_lua_apply_test_pair(sockets), SALTS_OK);
    connection_config = (redis_cflow_connection_config){
        &runtime, (uintptr_t)sockets[0], 4096u, 64u, 4096u, 64u, UINT64_C(5000000000), 1};
    check_equal(redis_cflow_connection_init_attached(&connection, &connection_config), SALTS_OK);
    check_equal(redis_lua_apply_open(&connection, &request, &operation), SALTS_OK);

    step = redis_lua_apply_next(&operation);
    check_equal(step.kind, REDIS_LUA_APPLY_WAIT);
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), SALTS_OK);
    step = redis_lua_apply_next(&operation);
    check_equal(step.kind, REDIS_LUA_APPLY_WAIT);
    received = recv(sockets[1], command, (int)(sizeof(command) - 1u), 0);
    check_true(received > 0);
    command[received] = '\0';
    check_not_null(strstr(command, "EVAL"));
    check_not_null(strstr(command, "applied_index"));
    check_not_null(strstr(command, "decimal_increment"));
    check_not_null(strstr(command, "raft:{orders}:outbox"));

    check_equal(send(sockets[1], reply, (int)(sizeof(reply) - 1u), 0),
                (int)(sizeof(reply) - 1u));
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), SALTS_OK);
    step = redis_lua_apply_next(&operation);
    check_equal(step.kind, REDIS_LUA_APPLY_DONE);
    check_equal(step.receipt.kind, REDIS_LUA_APPLY_APPLIED);
    check_equal(step.receipt.applied_index, UINT64_C(42));

    check_equal(redis_lua_apply_destroy(&operation), SALTS_OK);
    check_equal(redis_cflow_connection_destroy(&connection), SALTS_OK);
    redis_lua_apply_test_close(sockets[1]);
    check_equal(redis_io_runtime_close(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_OK);
  }

  it("rejects cross-slot data keys before dispatch") {
    redis_lua_apply_request request = redis_lua_apply_test_request();
    redis_cflow_connection connection = {0};
    redis_lua_apply operation = {0};
    request.metadata_key = "raft:{one}:meta";
    request.metadata_key_length = strlen(request.metadata_key);
    request.state_key = "raft:{two}:state";
    request.state_key_length = strlen(request.state_key);
    request.outbox_key = "raft:{one}:outbox";
    request.outbox_key_length = strlen(request.outbox_key);
    check_equal(redis_lua_apply_open(&connection, &request, &operation), SALTS_EINVAL);
  }

  it("maps a replayed command without reopening the transport") {
    static const char reply[] = "*2\r\n+REPLAYED\r\n:42\r\n";
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {redis_lua_apply_test_backend(), 1u, 1u};
    redis_lua_apply_test_socket sockets[2];
    redis_cflow_connection connection = {0};
    redis_cflow_connection_config connection_config;
    redis_lua_apply_request request = redis_lua_apply_test_request();
    redis_lua_apply operation = {0};
    redis_lua_apply_step step;

    check_equal(redis_io_runtime_init(&runtime, &runtime_config), SALTS_OK);
    check_equal(redis_lua_apply_test_pair(sockets), SALTS_OK);
    connection_config = (redis_cflow_connection_config){
        &runtime, (uintptr_t)sockets[0], 4096u, 64u, 4096u, 64u, UINT64_C(5000000000), 1};
    check_equal(redis_cflow_connection_init_attached(&connection, &connection_config), SALTS_OK);
    check_equal(redis_lua_apply_open(&connection, &request, &operation), SALTS_OK);
    check_equal(redis_lua_apply_next(&operation).kind, REDIS_LUA_APPLY_WAIT);
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), SALTS_OK);
    check_equal(redis_lua_apply_next(&operation).kind, REDIS_LUA_APPLY_WAIT);
    check_equal(send(sockets[1], reply, (int)(sizeof(reply) - 1u), 0),
                (int)(sizeof(reply) - 1u));
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), SALTS_OK);
    step = redis_lua_apply_next(&operation);
    check_equal(step.kind, REDIS_LUA_APPLY_DONE);
    check_equal(step.receipt.kind, REDIS_LUA_APPLY_REPLAYED);
    check_equal(step.receipt.applied_index, UINT64_C(42));

    check_equal(redis_lua_apply_destroy(&operation), SALTS_OK);
    check_equal(redis_cflow_connection_destroy(&connection), SALTS_OK);
    redis_lua_apply_test_close(sockets[1]);
    check_equal(redis_io_runtime_close(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_OK);
  }

  it("returns a gap when the requested index is not the next index") {
    static const char reply[] = "*2\r\n+GAP\r\n:42\r\n";
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {redis_lua_apply_test_backend(), 1u, 1u};
    redis_lua_apply_test_socket sockets[2];
    redis_cflow_connection connection = {0};
    redis_cflow_connection_config connection_config;
    redis_lua_apply_request request = redis_lua_apply_test_request();
    redis_lua_apply operation = {0};
    redis_lua_apply_step step;

    request.index = UINT64_C(44);
    check_equal(redis_io_runtime_init(&runtime, &runtime_config), SALTS_OK);
    check_equal(redis_lua_apply_test_pair(sockets), SALTS_OK);
    connection_config = (redis_cflow_connection_config){
        &runtime, (uintptr_t)sockets[0], 4096u, 64u, 4096u, 64u, UINT64_C(5000000000), 1};
    check_equal(redis_cflow_connection_init_attached(&connection, &connection_config), SALTS_OK);
    check_equal(redis_lua_apply_open(&connection, &request, &operation), SALTS_OK);
    check_equal(redis_lua_apply_next(&operation).kind, REDIS_LUA_APPLY_WAIT);
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), SALTS_OK);
    check_equal(redis_lua_apply_next(&operation).kind, REDIS_LUA_APPLY_WAIT);
    check_equal(send(sockets[1], reply, (int)(sizeof(reply) - 1u), 0),
                (int)(sizeof(reply) - 1u));
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), SALTS_OK);
    step = redis_lua_apply_next(&operation);
    check_equal(step.kind, REDIS_LUA_APPLY_DONE);
    check_equal(step.receipt.kind, REDIS_LUA_APPLY_GAP);
    check_equal(step.receipt.applied_index, UINT64_C(42));

    check_equal(redis_lua_apply_destroy(&operation), SALTS_OK);
    check_equal(redis_cflow_connection_destroy(&connection), SALTS_OK);
    redis_lua_apply_test_close(sockets[1]);
    check_equal(redis_io_runtime_close(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_OK);
  }

  it("marks a sent command with an unavailable reply as commit unknown") {
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {redis_lua_apply_test_backend(), 1u, 1u};
    redis_lua_apply_test_socket sockets[2];
    redis_cflow_connection connection = {0};
    redis_cflow_connection_config connection_config;
    redis_lua_apply_request request = redis_lua_apply_test_request();
    redis_lua_apply operation = {0};
    redis_lua_apply_step step;

    check_equal(redis_io_runtime_init(&runtime, &runtime_config), SALTS_OK);
    check_equal(redis_lua_apply_test_pair(sockets), SALTS_OK);
    connection_config = (redis_cflow_connection_config){
        &runtime, (uintptr_t)sockets[0], 4096u, 64u, 4096u, 64u, UINT64_C(5000000000), 1};
    check_equal(redis_cflow_connection_init_attached(&connection, &connection_config), SALTS_OK);
    check_equal(redis_lua_apply_open(&connection, &request, &operation), SALTS_OK);
    check_equal(redis_lua_apply_next(&operation).kind, REDIS_LUA_APPLY_WAIT);
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), SALTS_OK);
    check_equal(redis_lua_apply_next(&operation).kind, REDIS_LUA_APPLY_WAIT);
    redis_lua_apply_test_close(sockets[1]);
    sockets[1] = REDIS_LUA_APPLY_TEST_INVALID;
    check_equal(redis_io_runtime_wait_idle(&runtime, UINT64_C(5000000000)), SALTS_OK);
    step = redis_lua_apply_next(&operation);
    check_equal(step.kind, REDIS_LUA_APPLY_DONE);
    check_equal(step.receipt.kind, REDIS_LUA_APPLY_COMMIT_UNKNOWN);
    check_equal(step.receipt.outcome, REDIS_COMMAND_REPLY_UNKNOWN);

    check_equal(redis_lua_apply_destroy(&operation), SALTS_OK);
    check_equal(redis_cflow_connection_destroy(&connection), SALTS_OK);
    check_equal(redis_io_runtime_close(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_OK);
  }

  it("executes ordered apply and outbox semantics against a configured Redis") {
    const char *port_text = getenv("TURBODB_REDIS_TEST_PORT");
    redis_io_runtime runtime = {0};
    redis_io_runtime_config runtime_config = {redis_lua_apply_test_backend(), 1u, 1u};
    redis_cflow_connection connection = {0};
    redis_cflow_open_config connection_config;
    redis_cflow_connect_step connect_step;
    redis_lua_apply_request request = redis_lua_apply_test_request();
    redis_lua_apply operation = {0};
    redis_lua_apply_step step;
    redis_reply_t *reply;
    char *port_end = NULL;
    unsigned long port;
    static const char *delete_command[] = {
        "DEL", "raft:{orders}:meta", "raft:{orders}:state", "raft:{orders}:outbox"};
    static const char *seed_command[] = {
        "HSET", "raft:{orders}:meta", "applied_index", "18446744073709551614",
        "term", "7", "command_id", "integration-seed"};
    static const char *state_command[] = {"HGET", "raft:{orders}:state", "status"};
    static const char *outbox_command[] = {"XLEN", "raft:{orders}:outbox"};

    if (port_text == NULL || port_text[0] == '\0') return;
    port = strtoul(port_text, &port_end, 10);
    check_true(port_end != port_text && *port_end == '\0' && port > 0u && port <= UINT16_MAX);
    check_equal(redis_io_runtime_init(&runtime, &runtime_config), SALTS_OK);
    connection_config = (redis_cflow_open_config){
        &runtime, "127.0.0.1", (uint16_t)port, 1u, 4096u, 64u, 4096u, 64u,
        REDIS_LUA_APPLY_TEST_WAIT_TIMEOUT_NS};
    check_equal(redis_cflow_connection_open(&connection, &connection_config), SALTS_OK);
    connect_step = redis_cflow_connection_connect_next(&connection);
    check_equal(connect_step.kind, REDIS_CFLOW_CONNECT_WAIT);
    check_equal(redis_io_runtime_wait_idle(&runtime, REDIS_LUA_APPLY_TEST_WAIT_TIMEOUT_NS),
                SALTS_OK);
    connect_step = redis_cflow_connection_connect_next(&connection);
    check_equal(connect_step.kind, REDIS_CFLOW_CONNECT_DONE);
    check_true(redis_cflow_connection_usable(&connection));

    reply = redis_lua_apply_test_command(&connection, &runtime, 4, delete_command);
    check_not_null(reply);
    redis_reply_free(reply);
    reply = redis_lua_apply_test_command(&connection, &runtime, 8, seed_command);
    check_not_null(reply);
    check_equal(reply->type, REDIS_REPLY_INTEGER);
    check_equal(reply->integer, 3);
    redis_reply_free(reply);

    request.index = UINT64_MAX;
    request.term = UINT64_C(7);
    request.command_id = "integration-1";
    request.command_id_length = strlen(request.command_id);
    check_equal(redis_lua_apply_open(&connection, &request, &operation), SALTS_OK);
    step = redis_lua_apply_test_complete(&operation, &runtime);
    check_equal(step.kind, REDIS_LUA_APPLY_DONE);
    check_equal(step.receipt.kind, REDIS_LUA_APPLY_APPLIED);
    check_equal(redis_lua_apply_destroy(&operation), SALTS_OK);

    check_equal(redis_lua_apply_open(&connection, &request, &operation), SALTS_OK);
    step = redis_lua_apply_test_complete(&operation, &runtime);
    check_equal(step.kind, REDIS_LUA_APPLY_DONE);
    check_equal(step.receipt.kind, REDIS_LUA_APPLY_REPLAYED);
    check_equal(redis_lua_apply_destroy(&operation), SALTS_OK);

    request.index = UINT64_C(1);
    check_equal(redis_lua_apply_open(&connection, &request, &operation), SALTS_OK);
    step = redis_lua_apply_test_complete(&operation, &runtime);
    check_equal(step.kind, REDIS_LUA_APPLY_DONE);
    check_equal(step.receipt.kind, REDIS_LUA_APPLY_GAP);
    check_equal(redis_lua_apply_destroy(&operation), SALTS_OK);

    request.index = UINT64_MAX;
    request.term = UINT64_C(8);
    request.command_id = "integration-conflict";
    request.command_id_length = strlen(request.command_id);
    check_equal(redis_lua_apply_open(&connection, &request, &operation), SALTS_OK);
    step = redis_lua_apply_test_complete(&operation, &runtime);
    check_equal(step.kind, REDIS_LUA_APPLY_DONE);
    check_equal(step.receipt.kind, REDIS_LUA_APPLY_CONFLICT);
    check_equal(redis_lua_apply_destroy(&operation), SALTS_OK);

    reply = redis_lua_apply_test_command(&connection, &runtime, 3, state_command);
    check_not_null(reply);
    check_equal(reply->type, REDIS_REPLY_BULK_STRING);
    check_equal(reply->str, "paid", 4u);
    redis_reply_free(reply);
    reply = redis_lua_apply_test_command(&connection, &runtime, 2, outbox_command);
    check_not_null(reply);
    check_equal(reply->type, REDIS_REPLY_INTEGER);
    check_equal(reply->integer, 1);
    redis_reply_free(reply);

    check_equal(redis_cflow_connection_destroy(&connection), SALTS_OK);
    check_equal(redis_io_runtime_close(&runtime), SALTS_OK);
    check_equal(redis_io_runtime_destroy(&runtime), SALTS_OK);
  }
}
