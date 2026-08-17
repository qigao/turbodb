/**
 * @file test_redis_sentinel.c
 * @brief Redis Sentinel configuration and protocol contract tests
 */

#include "../redis_sentinel.h"
#include "CoroNet.h"
#include "tinytest.h"
#include "turbo_error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_ASSERT_TRUE(expr) check((expr))
#define TEST_ASSERT_FALSE(expr) check(!(expr))
#define TEST_ASSERT_NULL(value) check_null((value))
#define TEST_ASSERT_NOT_NULL(value) check_not_null((value))
#define TEST_ASSERT_EQUAL(expected, actual) check((intptr_t)(actual) == (intptr_t)(expected))
#define TEST_ASSERT_EQUAL_STRING(expected, actual) check_str_eq((actual), (expected))
#define REDIS_RUN_TEST(fn, label)                                                                  \
  it(label) { fn(); }

#define SENTINEL_TEST_UNAVAILABLE_PORT 19930
#define SENTINEL_TEST_STALE_PORT 19931
#define SENTINEL_TEST_FRESH_PORT 19932
#define SENTINEL_TEST_MASTER_A_PORT 19933
#define SENTINEL_TEST_MASTER_B_PORT 19934
#define SENTINEL_TEST_BUFFER_SIZE 4096u

typedef enum {
  SENTINEL_ENDPOINT_STALE,
  SENTINEL_ENDPOINT_FRESH,
  SENTINEL_ENDPOINT_MASTER_A,
  SENTINEL_ENDPOINT_MASTER_B
} sentinel_endpoint_kind_t;

typedef struct sentinel_protocol_case_s sentinel_protocol_case_t;

typedef struct {
  sentinel_protocol_case_t *test_case;
  sentinel_endpoint_kind_t kind;
} sentinel_endpoint_t;

struct sentinel_protocol_case_s {
  coro_context_t *ctx;
  coro_socket_t *listeners[4];
  sentinel_endpoint_t endpoints[4];
  int stale_advertised_port;
  int fresh_advertised_port;
  int master_a_is_master;
  int master_b_is_master;
  int stale_queries;
  int fresh_queries;
  int master_a_sets;
  int master_b_sets;
  int master_a_readonly;
  int master_b_readonly;
  int uncertain_received_a;
  int uncertain_received_b;
  int status;
  const char *stage;
};

typedef struct {
  int called;
  int client_present;
  char value[8];
} sentinel_callback_case_t;

static int find_bytes(const char *data, size_t len, const char *needle) {
  size_t needle_len = strlen(needle);
  if (needle_len == 0 || needle_len > len) return 0;
  for (size_t i = 0; i <= len - needle_len; ++i)
    if (memcmp(data + i, needle, needle_len) == 0) return 1;
  return 0;
}

static int parse_decimal_line(const char *data, size_t len, size_t *offset, long *value) {
  char number[32];
  size_t start;
  size_t count;
  char *end = NULL;
  if (!data || !offset || !value || *offset >= len) return 0;
  start = *offset;
  while (*offset + 1u < len && !(data[*offset] == '\r' && data[*offset + 1u] == '\n'))
    (*offset)++;
  if (*offset + 1u >= len) return 0;
  count = *offset - start;
  if (count == 0 || count >= sizeof(number)) return -1;
  memcpy(number, data + start, count);
  number[count] = '\0';
  *value = strtol(number, &end, 10);
  if (*end != '\0') return -1;
  *offset += 2u;
  return 1;
}

static int resp_command_size(const char *data, size_t len, size_t *frame_len) {
  size_t offset = 1;
  long argc;
  int parsed;
  if (!data || !frame_len || len == 0) return 0;
  if (data[0] != '*') return -1;
  parsed = parse_decimal_line(data, len, &offset, &argc);
  if (parsed <= 0) return parsed;
  if (argc <= 0 || argc > 64) return -1;
  for (long i = 0; i < argc; ++i) {
    long arg_len;
    if (offset >= len) return 0;
    if (data[offset++] != '$') return -1;
    parsed = parse_decimal_line(data, len, &offset, &arg_len);
    if (parsed <= 0) return parsed;
    if (arg_len < 0 || (size_t)arg_len > len - offset) return 0;
    offset += (size_t)arg_len;
    if (offset + 1u >= len) return 0;
    if (data[offset] != '\r' || data[offset + 1u] != '\n') return -1;
    offset += 2u;
  }
  *frame_len = offset;
  return 1;
}

static int send_master_address(coro_socket_t *client, int port) {
  char reply[128];
  char port_text[16];
  int port_len = snprintf(port_text, sizeof(port_text), "%d", port);
  int len;
  if (port_len <= 0 || (size_t)port_len >= sizeof(port_text)) return TURBO_EPROTO;
  len =
      snprintf(reply, sizeof(reply), "*2\r\n$9\r\n127.0.0.1\r\n$%d\r\n%s\r\n", port_len, port_text);
  if (len <= 0 || (size_t)len >= sizeof(reply)) return TURBO_EPROTO;
  return coro_socket_send(client, reply, (size_t)len);
}

static int send_role(coro_socket_t *client, int is_master) {
  static const char master_reply[] = "*3\r\n$6\r\nmaster\r\n:0\r\n*0\r\n";
  static const char replica_reply[] = "*1\r\n$5\r\nslave\r\n";
  return coro_socket_send(client, is_master ? master_reply : replica_reply,
                          is_master ? sizeof(master_reply) - 1u : sizeof(replica_reply) - 1u);
}

static int handle_sentinel_command(coro_socket_t *client, sentinel_endpoint_t *endpoint,
                                   const char *frame, size_t frame_len) {
  sentinel_protocol_case_t *test_case = endpoint->test_case;
  int port;
  if (!find_bytes(frame, frame_len, "SENTINEL") ||
      !find_bytes(frame, frame_len, "get-master-addr-by-name"))
    return coro_socket_send(client, "-ERR unsupported\r\n", 18u);
  if (endpoint->kind == SENTINEL_ENDPOINT_STALE) {
    test_case->stale_queries++;
    port = test_case->stale_advertised_port;
  } else {
    test_case->fresh_queries++;
    port = test_case->fresh_advertised_port;
  }
  return send_master_address(client, port);
}

static int handle_master_command(coro_socket_t *client, sentinel_endpoint_t *endpoint,
                                 const char *frame, size_t frame_len) {
  sentinel_protocol_case_t *test_case = endpoint->test_case;
  int is_a = endpoint->kind == SENTINEL_ENDPOINT_MASTER_A;
  int is_master = is_a ? test_case->master_a_is_master : test_case->master_b_is_master;

  if (find_bytes(frame, frame_len, "ROLE")) return send_role(client, is_master);

  if (find_bytes(frame, frame_len, "uncertain")) {
    if (is_a) test_case->uncertain_received_a++;
    else test_case->uncertain_received_b++;
    return TURBO_EOF;
  }

  if (find_bytes(frame, frame_len, "SET")) {
    if (!is_master) {
      if (is_a) test_case->master_a_readonly++;
      else test_case->master_b_readonly++;
      return coro_socket_send(client, "-READONLY You can't write against a read only replica.\r\n",
                              sizeof("-READONLY You can't write against a read only replica.\r\n") -
                                  1u);
    }
    if (is_a) test_case->master_a_sets++;
    else test_case->master_b_sets++;
    return coro_socket_send(client, "+OK\r\n", 5u);
  }

  if (find_bytes(frame, frame_len, "GET")) {
    return coro_socket_send(client, is_a ? "$1\r\nA\r\n" : "$1\r\nB\r\n", 7u);
  }
  if (find_bytes(frame, frame_len, "DEL")) return coro_socket_send(client, ":1\r\n", 4u);
  return coro_socket_send(client, "+PONG\r\n", 7u);
}

static void fake_endpoint_connection(coro_socket_t *client, void *arg) {
  sentinel_endpoint_t *endpoint = (sentinel_endpoint_t *)arg;
  char buffer[SENTINEL_TEST_BUFFER_SIZE];
  size_t used = 0;

  for (;;) {
    char *data = NULL;
    size_t len = 0;
    int rc = coro_socket_recv(client, &data, &len);
    if (rc != TURBO_OK || !data || len == 0 || len > sizeof(buffer) - used) {
      if (data) coro_socket_free_recv(data);
      return;
    }
    memcpy(buffer + used, data, len);
    used += len;
    coro_socket_free_recv(data);

    for (;;) {
      size_t frame_len = 0;
      int parsed = resp_command_size(buffer, used, &frame_len);
      if (parsed < 0) return;
      if (parsed == 0) break;
      if (endpoint->kind == SENTINEL_ENDPOINT_STALE || endpoint->kind == SENTINEL_ENDPOINT_FRESH)
        rc = handle_sentinel_command(client, endpoint, buffer, frame_len);
      else rc = handle_master_command(client, endpoint, buffer, frame_len);
      if (rc != TURBO_OK) return;
      memmove(buffer, buffer + frame_len, used - frame_len);
      used -= frame_len;
    }
  }
}

static int start_fake_listener(sentinel_protocol_case_t *test_case, size_t index, int port,
                               sentinel_endpoint_kind_t kind) {
  test_case->endpoints[index].test_case = test_case;
  test_case->endpoints[index].kind = kind;
  test_case->listeners[index] = coro_socket_create(test_case->ctx, CORO_SOCKET_TCP_V4);
  if (!test_case->listeners[index]) return TURBO_ENOMEM;
  return coro_socket_listen_on(test_case->listeners[index], "127.0.0.1", port,
                               fake_endpoint_connection, &test_case->endpoints[index]);
}

static int reply_text_is(const redis_command_result_t *result, redis_reply_type_t type,
                         const char *text) {
  size_t length = strlen(text);
  return result && result->status == TURBO_OK && result->reply && result->reply->type == type &&
         result->reply->str && result->reply->len == length &&
         memcmp(result->reply->str, text, length) == 0;
}

static void capture_sentinel_reply(redis_client_t *client, redis_reply_t *reply, void *user_data) {
  sentinel_callback_case_t *callback = (sentinel_callback_case_t *)user_data;
  callback->called++;
  callback->client_present = client != NULL;
  if (reply && reply->str && reply->len < sizeof(callback->value)) {
    memcpy(callback->value, reply->str, reply->len);
    callback->value[reply->len] = '\0';
  }
}

static void run_sentinel_protocol_contract(coro_t *co, void *arg) {
  sentinel_protocol_case_t *test_case = (sentinel_protocol_case_t *)arg;
  const char *hosts[] = {"127.0.0.1", "127.0.0.1", "127.0.0.1"};
  uint16_t ports[] = {SENTINEL_TEST_UNAVAILABLE_PORT, SENTINEL_TEST_STALE_PORT,
                      SENTINEL_TEST_FRESH_PORT};
  redis_sentinel_config_t config = REDIS_SENTINEL_CONFIG_DEFAULT;
  redis_sentinel_t *sentinel = NULL;
  redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
  redis_sentinel_stats_t stats;
  const char *set_argv[] = {"SET", "route", "value"};
  const char *uncertain_argv[] = {"SET", "uncertain", "value"};
  sentinel_callback_case_t callback = {0};
  char master_host[32];
  uint16_t master_port = 0;

  (void)co;
  test_case->status = TURBO_EIO;
  test_case->stage = "start stale Sentinel";
  if (start_fake_listener(test_case, 0, SENTINEL_TEST_STALE_PORT, SENTINEL_ENDPOINT_STALE) !=
      TURBO_OK)
    goto cleanup;
  test_case->stage = "start fresh Sentinel";
  if (start_fake_listener(test_case, 1, SENTINEL_TEST_FRESH_PORT, SENTINEL_ENDPOINT_FRESH) !=
      TURBO_OK)
    goto cleanup;
  test_case->stage = "start master A";
  if (start_fake_listener(test_case, 2, SENTINEL_TEST_MASTER_A_PORT, SENTINEL_ENDPOINT_MASTER_A) !=
      TURBO_OK)
    goto cleanup;
  test_case->stage = "start master B";
  if (start_fake_listener(test_case, 3, SENTINEL_TEST_MASTER_B_PORT, SENTINEL_ENDPOINT_MASTER_B) !=
      TURBO_OK)
    goto cleanup;
  coro_yield();

  test_case->stale_advertised_port = SENTINEL_TEST_MASTER_A_PORT;
  test_case->fresh_advertised_port = SENTINEL_TEST_MASTER_A_PORT;
  test_case->master_a_is_master = 1;
  test_case->master_b_is_master = 0;

  config.sentinel_hosts = hosts;
  config.sentinel_ports = ports;
  config.sentinel_count = 3;
  config.service_name = "turbonet-primary";
  config.min_connections = 1;
  config.max_connections = 1;
  config.sentinel_connect_timeout_ms = 100;
  config.sentinel_command_timeout_ms = 500;
  config.connect_timeout_ms = 500;
  config.command_timeout_ms = 500;
  config.topology_refresh_ms = 0;

  test_case->stage = "connect through fallback Sentinel";
  sentinel = redis_sentinel_create(&config);
  if (!sentinel || redis_sentinel_connect(sentinel) != TURBO_OK ||
      !redis_sentinel_is_healthy(sentinel))
    goto cleanup;
  if (redis_sentinel_get_master(sentinel, master_host, sizeof(master_host), &master_port) !=
          TURBO_OK ||
      strcmp(master_host, "127.0.0.1") != 0 || master_port != SENTINEL_TEST_MASTER_A_PORT)
    goto cleanup;

  test_case->stage = "route initial command to master A";
  if (redis_sentinel_get(sentinel, "route", capture_sentinel_reply, &callback) != TURBO_OK ||
      callback.called != 1 || !callback.client_present || strcmp(callback.value, "A") != 0)
    goto cleanup;

  test_case->stage = "reject stale Sentinel and switch to master B";
  test_case->master_a_is_master = 0;
  test_case->master_b_is_master = 1;
  test_case->stale_advertised_port = SENTINEL_TEST_MASTER_A_PORT;
  test_case->fresh_advertised_port = SENTINEL_TEST_MASTER_B_PORT;
  if (redis_sentinel_refresh(sentinel) != TURBO_OK ||
      redis_sentinel_get_master(sentinel, master_host, sizeof(master_host), &master_port) !=
          TURBO_OK ||
      master_port != SENTINEL_TEST_MASTER_B_PORT)
    goto cleanup;

  test_case->stage = "refresh and safely retry READONLY on master A";
  test_case->master_a_is_master = 1;
  test_case->master_b_is_master = 0;
  test_case->stale_advertised_port = SENTINEL_TEST_MASTER_A_PORT;
  test_case->fresh_advertised_port = SENTINEL_TEST_MASTER_A_PORT;
  if (redis_sentinel_commandv_result(sentinel, 3, set_argv, NULL, &result) != TURBO_OK ||
      !reply_text_is(&result, REDIS_REPLY_STRING, "OK") || test_case->master_b_readonly != 1 ||
      test_case->master_a_sets != 1)
    goto cleanup;
  redis_command_result_clear(&result);

  test_case->stage = "refresh but do not retry an uncertain write";
  test_case->master_a_is_master = 0;
  test_case->master_b_is_master = 1;
  test_case->stale_advertised_port = SENTINEL_TEST_MASTER_B_PORT;
  test_case->fresh_advertised_port = SENTINEL_TEST_MASTER_B_PORT;
  if (redis_sentinel_commandv_result(sentinel, 3, uncertain_argv, NULL, &result) == TURBO_OK ||
      result.outcome != REDIS_COMMAND_REPLY_UNKNOWN || test_case->uncertain_received_a != 1 ||
      test_case->uncertain_received_b != 0)
    goto cleanup;
  if (redis_sentinel_get_master(sentinel, master_host, sizeof(master_host), &master_port) !=
          TURBO_OK ||
      master_port != SENTINEL_TEST_MASTER_B_PORT)
    goto cleanup;

  redis_sentinel_get_stats(sentinel, &stats);
  if (stats.failovers != 3 || stats.safe_retries != 1 || stats.discovery_attempts < 6 ||
      stats.commands_sent != 2 || stats.commands_failed != 1)
    goto cleanup;

  test_case->stage = "complete";
  test_case->status = TURBO_OK;

cleanup:
  redis_command_result_clear(&result);
  redis_sentinel_destroy(sentinel);
  for (size_t i = 0; i < 4; ++i) {
    coro_socket_destroy(test_case->listeners[i]);
    test_case->listeners[i] = NULL;
  }
}

static redis_sentinel_config_t valid_config(const char **hosts, uint16_t *ports) {
  redis_sentinel_config_t config = REDIS_SENTINEL_CONFIG_DEFAULT;
  config.sentinel_hosts = hosts;
  config.sentinel_ports = ports;
  config.sentinel_count = 1;
  config.service_name = "primary";
  return config;
}

void test_sentinel_create_valid(void) {
  const char *hosts[] = {"127.0.0.1"};
  uint16_t ports[] = {26379};
  redis_sentinel_config_t config = valid_config(hosts, ports);
  redis_sentinel_t *sentinel = redis_sentinel_create(&config);
  TEST_ASSERT_NOT_NULL(sentinel);
  redis_sentinel_destroy(sentinel);
}

void test_sentinel_rejects_missing_service(void) {
  const char *hosts[] = {"127.0.0.1"};
  uint16_t ports[] = {26379};
  redis_sentinel_config_t config = valid_config(hosts, ports);
  config.service_name = NULL;
  TEST_ASSERT_NULL(redis_sentinel_create(&config));
}

void test_sentinel_rejects_username_without_password(void) {
  const char *hosts[] = {"127.0.0.1"};
  uint16_t ports[] = {26379};
  redis_sentinel_config_t config = valid_config(hosts, ports);
  config.sentinel_username = "sentinel-user";
  TEST_ASSERT_NULL(redis_sentinel_create(&config));
  config.sentinel_username = NULL;
  config.username = "data-user";
  TEST_ASSERT_NULL(redis_sentinel_create(&config));
}

void test_sentinel_command_reports_not_connected(void) {
  const char *hosts[] = {"127.0.0.1"};
  uint16_t ports[] = {26379};
  const char *argv[] = {"PING"};
  redis_sentinel_config_t config = valid_config(hosts, ports);
  redis_sentinel_t *sentinel = redis_sentinel_create(&config);
  redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
  TEST_ASSERT_NOT_NULL(sentinel);
  TEST_ASSERT_EQUAL(TURBO_ENOTCONN,
                    redis_sentinel_commandv_result(sentinel, 1, argv, NULL, &result));
  TEST_ASSERT_EQUAL(REDIS_COMMAND_NOT_SENT, result.outcome);
  redis_command_result_clear(&result);
  redis_sentinel_destroy(sentinel);
}

void test_sentinel_null_lifecycle_is_safe(void) {
  redis_sentinel_stats_t stats = {1};
  redis_sentinel_disconnect(NULL);
  redis_sentinel_destroy(NULL);
  redis_sentinel_get_stats(NULL, &stats);
  TEST_ASSERT_EQUAL(0, stats.commands_sent);
  TEST_ASSERT_FALSE(redis_sentinel_is_healthy(NULL));
}

void test_sentinel_protocol_contract(void) {
  sentinel_protocol_case_t test_case = {0};
  test_case.stage = "create context";
  test_case.ctx = coro_context_create(NULL);
  TEST_ASSERT_NOT_NULL(test_case.ctx);
  TEST_ASSERT_EQUAL(TURBO_OK,
                    coro_context_spawn(test_case.ctx, run_sentinel_protocol_contract, &test_case));
  coro_context_run(test_case.ctx, TURBO_RUN_DEFAULT);
  coro_context_destroy(test_case.ctx);
  if (test_case.status != TURBO_OK)
    fprintf(stderr, "Sentinel protocol contract failed at stage: %s\n", test_case.stage);
  TEST_ASSERT_EQUAL(TURBO_OK, test_case.status);
  TEST_ASSERT_TRUE(test_case.stale_queries >= 2);
  TEST_ASSERT_TRUE(test_case.fresh_queries >= 2);
}

suite("redis_sentinel") {
  group("Configuration") {
    REDIS_RUN_TEST(test_sentinel_create_valid, "should create with copied Sentinel configuration");
    REDIS_RUN_TEST(test_sentinel_rejects_missing_service, "should reject a missing service name");
    REDIS_RUN_TEST(test_sentinel_rejects_username_without_password,
                   "should keep Sentinel and data ACL validation separate");
  }

  group("Lifecycle") {
    REDIS_RUN_TEST(test_sentinel_null_lifecycle_is_safe,
                   "should handle null lifecycle and stats calls");
    REDIS_RUN_TEST(test_sentinel_command_reports_not_connected,
                   "should report commands rejected before sending");
  }

  group("Protocol") {
    REDIS_RUN_TEST(test_sentinel_protocol_contract,
                   "should discover validate switch and preserve uncertain writes");
  }
}
