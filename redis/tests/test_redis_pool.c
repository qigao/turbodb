/**
 * @file test_redis_pool.c
 * @brief Unit tests for Redis Connection Pool
 */

#include "../redis_pool.h"
#include "CoroNet/turbo_coro_context.h"
#include "tinytest.h"
#include "turbo_error.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TEST_PASS() do { check(1); return; } while (0)
#define TEST_ASSERT_TRUE(expr) check((expr))
#define TEST_ASSERT_NULL(value) check_null((value))
#define TEST_ASSERT_NOT_NULL(value) check_not_null((value))
#define TEST_ASSERT_EQUAL(expected, actual) check((intptr_t)(actual) == (intptr_t)(expected))
#define TEST_ASSERT_EQUAL_STRING(expected, actual) check_str_eq((actual), (expected))
#define TEST_ASSERT_GREATER_THAN(threshold, actual) check((long long)(actual) > (long long)(threshold))
#define TEST_ASSERT_LESS_THAN(limit, actual) check((long long)(actual) < (long long)(limit))
#define REDIS_RUN_TEST(fn, label) it(label) { fn(); }

void setUp(void) {
}

void tearDown(void) {
}

#ifdef REDIS_TESTING
static int g_pipeline_send_count = 0;
static char *g_pipeline_sent_data = NULL;
static size_t g_pipeline_sent_len = 0;
static int g_pipeline_recv_count = 0;
static int g_pipeline_send_result = 0;
static const char *g_pipeline_reply_blob = "+OK\r\n+PONG\r\n";

static void pipeline_test_reset_state(void) {
    g_pipeline_send_count = 0;
    g_pipeline_recv_count = 0;
    g_pipeline_send_result = 0;
    free(g_pipeline_sent_data);
    g_pipeline_sent_data = NULL;
    g_pipeline_sent_len = 0;
    g_pipeline_reply_blob = "+OK\r\n+PONG\r\n";
}

static int pipeline_test_send_hook(coro_socket_t *socket, const char *data, size_t len) {
    (void)socket;
    g_pipeline_send_count++;
    free(g_pipeline_sent_data);
    g_pipeline_sent_data = malloc(len + 1);
    check_not_null(g_pipeline_sent_data);
    memcpy(g_pipeline_sent_data, data, len);
    g_pipeline_sent_data[len] = '\0';
    g_pipeline_sent_len = len;
    return g_pipeline_send_result;
}

static int pipeline_test_recv_hook(coro_socket_t *socket, char **data, size_t *len) {
    size_t blob_len;
    char *copy;
    (void)socket;

    if (g_pipeline_recv_count > 0) {
        return -1;
    }

    blob_len = strlen(g_pipeline_reply_blob);
    copy = malloc(blob_len + 1);
    check_not_null(copy);
    memcpy(copy, g_pipeline_reply_blob, blob_len + 1);
    *data = copy;
    *len = blob_len;
    g_pipeline_recv_count++;
    return 0;
}

static void pipeline_test_free_recv_hook(void *data) {
    free(data);
}
#endif

// =============================================================================
// Configuration Tests
// =============================================================================

void test_pool_create_default(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    TEST_ASSERT_NOT_NULL(pool);
    redis_pool_destroy(pool);
}

void test_pool_create_custom(void) {
    redis_pool_config_t config = {
        .master_host = "redis.example.com",
        .master_port = 6380,
        .username = "worker",
        .password = "secret123",
        .database = 5,
        .min_connections = 5,
        .max_connections = 20,
        .connect_timeout_ms = 10000,
        .command_timeout_ms = 8000,
        .idle_timeout_ms = 120000,
        .health_check_ms = 60000,
        .pipeline_max = 200,
        .pipeline_timeout_ms = 20
    };

    redis_pool_t *pool = redis_pool_create(&config);
    TEST_ASSERT_NOT_NULL(pool);
    redis_pool_destroy(pool);
}

void test_pool_rejects_username_without_password(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    config.username = "worker";
    TEST_ASSERT_NULL(redis_pool_create(&config));
}

void test_pool_rejects_invalid_database(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    config.database = 16;
    TEST_ASSERT_NULL(redis_pool_create(&config));
}

void test_pool_create_null_config(void) {
    redis_pool_t *pool = redis_pool_create(NULL);
    TEST_ASSERT_NULL(pool);
}

void test_pool_create_null_host(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    config.master_host = NULL;
    redis_pool_t *pool = redis_pool_create(&config);
    TEST_ASSERT_NULL(pool);
}

void test_pool_destroy_null(void) {
    /* Should not crash */
    redis_pool_destroy(NULL);
    TEST_PASS();
}

// =============================================================================
// Pool Lifecycle Tests
// =============================================================================

void test_pool_stop_null(void) {
    /* Should not crash */
    redis_pool_stop(NULL);
    TEST_PASS();
}

void test_pool_start_null(void) {
    int result = redis_pool_start(NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_pool_start_without_coro_context_fails(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    config.min_connections = 2;
    redis_pool_t *pool = redis_pool_create(&config);
    int result;

    TEST_ASSERT_NOT_NULL(pool);

    result = redis_pool_start(pool);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_stop_twice(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    TEST_ASSERT_NOT_NULL(pool);

    redis_pool_stop(pool);
    redis_pool_stop(pool);  /* Should not crash */

    redis_pool_destroy(pool);
}

// =============================================================================
// Connection Acquisition Tests
// =============================================================================

void test_pool_acquire_null(void) {
    redis_pool_conn_t *conn = redis_pool_acquire(NULL, 0);
    TEST_ASSERT_NULL(conn);
}

void test_pool_acquire_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    TEST_ASSERT_NOT_NULL(pool);

    /* Pool not started */
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 0);
    TEST_ASSERT_NULL(conn);

    redis_pool_destroy(pool);
}

void test_pool_release_null(void) {
    /* Should not crash */
    redis_pool_release(NULL, NULL);

    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    redis_pool_release(pool, NULL);
    redis_pool_release(NULL, (redis_pool_conn_t *)0x1);  /* fake pointer */
    redis_pool_destroy(pool);
    TEST_PASS();
}

void test_pool_conn_client_null(void) {
    redis_client_t *client = redis_pool_conn_client(NULL);
    TEST_ASSERT_NULL(client);
}

// =============================================================================
// Pool Command Tests (without connection)
// =============================================================================

void test_pool_command_null(void) {
    int result = redis_pool_command(NULL, NULL, NULL, "PING");
    TEST_ASSERT_EQUAL(-1, result);
}

void test_pool_command_null_format(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    int result = redis_pool_command(pool, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_commandv_null(void) {
    const char *argv[] = {"SET", "key", "value"};
    int result = redis_pool_commandv(NULL, 3, argv, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_pool_commandv_null_argv(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    int result = redis_pool_commandv(pool, 3, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_commandv_zero_argc(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    const char *argv[] = {"SET"};
    int result = redis_pool_commandv(pool, 0, argv, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_command_result_reports_not_connected(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
    const char *argv[] = {"PING"};
    TEST_ASSERT_NOT_NULL(pool);
    TEST_ASSERT_EQUAL(TURBO_ENOTCONN,
                      redis_pool_commandv_result(pool, 0, 1, argv, NULL, &result));
    TEST_ASSERT_EQUAL(REDIS_COMMAND_NOT_SENT, result.outcome);
    redis_command_result_clear(&result);
    redis_pool_destroy(pool);
}

void test_pool_read_command_null(void) {
    int result = redis_pool_read_command(NULL, NULL, NULL, "GET key");
    TEST_ASSERT_EQUAL(-1, result);
}

// =============================================================================
// Convenience Function Tests
// =============================================================================

void test_pool_set_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    int result = redis_pool_set(pool, "key", "value", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_get_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    int result = redis_pool_get(pool, "key", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_del_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    const char *keys[] = {"key1", "key2"};
    int result = redis_pool_del(pool, 2, keys, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_expire_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    int result = redis_pool_expire(pool, "key", 3600, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_hset_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    int result = redis_pool_hset(pool, "hash", "field", "value", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_hget_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    int result = redis_pool_hget(pool, "hash", "field", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_xadd_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    const char *fields[] = {"f1"};
    const char *values[] = {"v1"};
    int result = redis_pool_xadd(pool, "stream", 1000, 1, fields, values, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

void test_pool_xreadgroup_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    const char *keys[] = {"stream"};
    const char *ids[] = {">"};
    int result = redis_pool_xreadgroup(pool, "group", "consumer", 10, 1000, 1, keys, ids, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pool_destroy(pool);
}

// =============================================================================
// Pipeline Tests
// =============================================================================

void test_pipeline_create_null(void) {
    redis_pipeline_t *pipeline = redis_pool_pipeline_create(NULL);
    TEST_ASSERT_NULL(pipeline);
}

void test_pipeline_create_destroy(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    redis_pipeline_t *pipeline = redis_pool_pipeline_create(pool);
    TEST_ASSERT_NOT_NULL(pipeline);

    redis_pipeline_destroy(pipeline);
    redis_pool_destroy(pool);
}

void test_pipeline_destroy_null(void) {
    /* Should not crash */
    redis_pipeline_destroy(NULL);
    TEST_PASS();
}

void test_pipeline_add_null(void) {
    int result = redis_pipeline_add(NULL, NULL, NULL, "PING");
    TEST_ASSERT_EQUAL(-1, result);
}

void test_pipeline_add_null_format(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    redis_pipeline_t *pipeline = redis_pool_pipeline_create(pool);

    int result = redis_pipeline_add(pipeline, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_pipeline_destroy(pipeline);
    redis_pool_destroy(pool);
}

void test_pipeline_add_commands(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    redis_pipeline_t *pipeline = redis_pool_pipeline_create(pool);

    int result = redis_pipeline_add(pipeline, NULL, NULL, "SET key1 value1");
    TEST_ASSERT_EQUAL(0, result);

    result = redis_pipeline_add(pipeline, NULL, NULL, "SET key2 value2");
    TEST_ASSERT_EQUAL(0, result);

    result = redis_pipeline_add(pipeline, NULL, NULL, "GET key1");
    TEST_ASSERT_EQUAL(0, result);

    redis_pipeline_destroy(pipeline);
    redis_pool_destroy(pool);
}

void test_pipeline_addv_null(void) {
    const char *argv[] = {"PING"};
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    redis_pipeline_t *pipeline = redis_pool_pipeline_create(pool);

    TEST_ASSERT_EQUAL(-1, redis_pipeline_addv(NULL, NULL, NULL, 1, argv, NULL));
    TEST_ASSERT_EQUAL(-1, redis_pipeline_addv(pipeline, NULL, NULL, 0, argv, NULL));
    TEST_ASSERT_EQUAL(-1, redis_pipeline_addv(pipeline, NULL, NULL, 1, NULL, NULL));

    redis_pipeline_destroy(pipeline);
    redis_pool_destroy(pool);
}

void test_pipeline_execute_null(void) {
    int result = redis_pipeline_execute(NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_pipeline_sync_null(void) {
    redis_reply_t **replies = NULL;
    size_t reply_count = 0;

    TEST_ASSERT_EQUAL(-1, redis_pipeline_sync(NULL));
    TEST_ASSERT_EQUAL(-1, redis_pipeline_sync_and_return_all(NULL, &replies, &reply_count));
}

void test_pipeline_execute_empty(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    redis_pipeline_t *pipeline = redis_pool_pipeline_create(pool);

    int result = redis_pipeline_execute(pipeline);
    TEST_ASSERT_EQUAL(-1, result);  /* Empty pipeline */

    redis_pipeline_destroy(pipeline);
    redis_pool_destroy(pool);
}

void test_pipeline_grow_capacity(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    redis_pipeline_t *pipeline = redis_pool_pipeline_create(pool);

    /* Add more than initial capacity (32) to test growth */
    for (int i = 0; i < 50; i++) {
        int result = redis_pipeline_add(pipeline, NULL, NULL, "PING");
        TEST_ASSERT_EQUAL(0, result);
    }

    redis_pipeline_destroy(pipeline);
    redis_pool_destroy(pool);
}

void test_pipeline_enforces_configured_capacity(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    config.pipeline_max = 3;
    redis_pool_t *pool = redis_pool_create(&config);
    redis_pipeline_t *pipeline = redis_pool_pipeline_create(pool);

    TEST_ASSERT_NOT_NULL(pool);
    TEST_ASSERT_NOT_NULL(pipeline);
    TEST_ASSERT_EQUAL(0, redis_pipeline_add(pipeline, NULL, NULL, "PING"));
    TEST_ASSERT_EQUAL(0, redis_pipeline_add(pipeline, NULL, NULL, "PING"));
    TEST_ASSERT_EQUAL(0, redis_pipeline_add(pipeline, NULL, NULL, "PING"));
    TEST_ASSERT_EQUAL(-1, redis_pipeline_add(pipeline, NULL, NULL, "PING"));

    redis_pipeline_destroy(pipeline);
    redis_pool_destroy(pool);
}

void test_pipeline_clear_allows_reuse(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    redis_pipeline_t *pipeline = redis_pool_pipeline_create(pool);

    TEST_ASSERT_EQUAL(0, redis_pipeline_add(pipeline, NULL, NULL, "PING"));
    redis_pipeline_clear(pipeline);
    TEST_ASSERT_EQUAL(-1, redis_pipeline_execute(pipeline));
    TEST_ASSERT_EQUAL(0, redis_pipeline_add(pipeline, NULL, NULL, "PING"));

    redis_pipeline_destroy(pipeline);
    redis_pool_destroy(pool);
}

#ifdef REDIS_TESTING
void test_pipeline_batch_exec_sends_single_resp_batch(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    const char *commands[] = {"SET key1 value1", "PING"};

    TEST_ASSERT_NOT_NULL(client);
    client->socket = (coro_socket_t *)0x1;
    client->is_connected = 1;

    pipeline_test_reset_state();

    redis_pool_test_set_socket_hooks(pipeline_test_send_hook,
                                     pipeline_test_recv_hook,
                                     pipeline_test_free_recv_hook);

    TEST_ASSERT_EQUAL(0, redis_pool_test_execute_batch(client, commands, 2));
    TEST_ASSERT_EQUAL(1, g_pipeline_send_count);
    TEST_ASSERT_GREATER_THAN(0, g_pipeline_sent_len);
    TEST_ASSERT_EQUAL_STRING(
        "*3\r\n$3\r\nSET\r\n$4\r\nkey1\r\n$6\r\nvalue1\r\n*1\r\n$4\r\nPING\r\n",
        g_pipeline_sent_data);

    redis_pool_test_set_socket_hooks(NULL, NULL, NULL);
    free(g_pipeline_sent_data);
    g_pipeline_sent_data = NULL;
    redis_client_destroy(client);
}

void test_pipeline_execute_collect_returns_ordered_replies(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    const char *commands[] = {"SET key1 value1", "PING"};
    redis_reply_t **replies = NULL;
    size_t reply_count = 0;

    TEST_ASSERT_NOT_NULL(client);
    client->socket = (coro_socket_t *)0x1;
    client->is_connected = 1;

    pipeline_test_reset_state();

    redis_pool_test_set_socket_hooks(pipeline_test_send_hook,
                                     pipeline_test_recv_hook,
                                     pipeline_test_free_recv_hook);

    TEST_ASSERT_EQUAL(0, redis_pool_test_execute_batch_collect(client, commands, 2, &replies, &reply_count));
    TEST_ASSERT_EQUAL(2, reply_count);
    TEST_ASSERT_NOT_NULL(replies);
    TEST_ASSERT_NOT_NULL(replies[0]);
    TEST_ASSERT_NOT_NULL(replies[1]);
    TEST_ASSERT_EQUAL(REDIS_REPLY_STRING, replies[0]->type);
    TEST_ASSERT_EQUAL(REDIS_REPLY_STRING, replies[1]->type);
    TEST_ASSERT_EQUAL_STRING("OK", replies[0]->str);
    TEST_ASSERT_EQUAL_STRING("PONG", replies[1]->str);

    redis_pipeline_replies_free(replies, reply_count);
    redis_pool_test_set_socket_hooks(NULL, NULL, NULL);
    free(g_pipeline_sent_data);
    g_pipeline_sent_data = NULL;
    redis_client_destroy(client);
}

void test_pipeline_addv_binary_safe_sends_single_batch(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    redis_pipeline_t *pipeline = redis_pool_pipeline_create(pool);
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    const char payload[] = {'a', '\0', 'b'};
    const char *argv[] = {"SET", "bin", payload};
    const size_t argvlen[] = {3, 3, sizeof(payload)};
    redis_reply_t **replies = NULL;
    size_t reply_count = 0;
    const char expected[] = "*3\r\n$3\r\nSET\r\n$3\r\nbin\r\n$3\r\na\0b\r\n*1\r\n$4\r\nPING\r\n";

    TEST_ASSERT_NOT_NULL(pool);
    TEST_ASSERT_NOT_NULL(pipeline);
    TEST_ASSERT_NOT_NULL(client);
    client->socket = (coro_socket_t *)0x1;
    client->is_connected = 1;

    TEST_ASSERT_EQUAL(0, redis_pipeline_addv(pipeline, NULL, NULL, 3, argv, argvlen));
    TEST_ASSERT_EQUAL(0, redis_pipeline_add(pipeline, NULL, NULL, "PING"));

    pipeline_test_reset_state();
    redis_pool_test_set_socket_hooks(pipeline_test_send_hook,
                                     pipeline_test_recv_hook,
                                     pipeline_test_free_recv_hook);

    TEST_ASSERT_EQUAL(0, redis_pool_test_execute_pipeline_collect(pipeline, client, &replies, &reply_count));
    TEST_ASSERT_EQUAL(1, g_pipeline_send_count);
    TEST_ASSERT_EQUAL(2, reply_count);
    TEST_ASSERT_EQUAL((int)(sizeof(expected) - 1), (int)g_pipeline_sent_len);
    TEST_ASSERT_TRUE(memcmp(g_pipeline_sent_data, expected, sizeof(expected) - 1) == 0);

    redis_pipeline_replies_free(replies, reply_count);
    redis_pool_test_set_socket_hooks(NULL, NULL, NULL);
    free(g_pipeline_sent_data);
    g_pipeline_sent_data = NULL;
    redis_client_destroy(client);
    redis_pipeline_destroy(pipeline);
    redis_pool_destroy(pool);
}

void test_pipeline_result_preserves_completion_state(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    redis_pipeline_t *pipeline = redis_pool_pipeline_create(pool);
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    redis_pipeline_result_t result = REDIS_PIPELINE_RESULT_INIT;

    TEST_ASSERT_NOT_NULL(pool);
    TEST_ASSERT_NOT_NULL(pipeline);
    TEST_ASSERT_NOT_NULL(client);
    client->socket = (coro_socket_t *)0x1;
    client->is_connected = 1;
    TEST_ASSERT_EQUAL(0, redis_pipeline_add(pipeline, NULL, NULL, "PING"));
    redis_pool_test_set_socket_hooks(pipeline_test_send_hook,
                                     pipeline_test_recv_hook,
                                     pipeline_test_free_recv_hook);

    pipeline_test_reset_state();
    g_pipeline_reply_blob = "-NOSCRIPT missing script\r\n";
    TEST_ASSERT_EQUAL(TURBO_EIO,
                      redis_pool_test_execute_pipeline_result(pipeline, client, &result));
    TEST_ASSERT_EQUAL(REDIS_COMMAND_REPLIED, result.outcome);
    TEST_ASSERT_EQUAL(1, result.reply_count);
    TEST_ASSERT_EQUAL(REDIS_SERVER_ERROR_NO_SCRIPT,
                      redis_server_error_classify(result.replies[0]));
    redis_pipeline_result_clear(&result);

    pipeline_test_reset_state();
    g_pipeline_send_result = TURBO_EIO;
    TEST_ASSERT_EQUAL(TURBO_EIO,
                      redis_pool_test_execute_pipeline_result(pipeline, client, &result));
    TEST_ASSERT_EQUAL(REDIS_COMMAND_SEND_UNCERTAIN, result.outcome);
    redis_pipeline_result_clear(&result);

    pipeline_test_reset_state();
    g_pipeline_reply_blob = "$5\r\nabc";
    TEST_ASSERT_EQUAL(TURBO_EPROTO,
                      redis_pool_test_execute_pipeline_result(pipeline, client, &result));
    TEST_ASSERT_EQUAL(REDIS_COMMAND_REPLY_UNKNOWN, result.outcome);
    redis_pipeline_result_clear(&result);

    redis_pool_test_set_socket_hooks(NULL, NULL, NULL);
    free(g_pipeline_sent_data);
    g_pipeline_sent_data = NULL;
    redis_client_destroy(client);
    redis_pipeline_destroy(pipeline);
    redis_pool_destroy(pool);
}
#endif

void test_pipeline_replies_free_null_safe(void) {
    redis_pipeline_replies_free(NULL, 0);
    TEST_PASS();
}

// =============================================================================
// Statistics Tests
// =============================================================================

void test_pool_get_stats_null(void) {
    redis_pool_stats_t stats = {0};

    /* Should not crash */
    redis_pool_get_stats(NULL, &stats);
    redis_pool_get_stats(NULL, NULL);

    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);
    redis_pool_get_stats(pool, NULL);
    redis_pool_destroy(pool);

    TEST_PASS();
}

void test_pool_get_stats(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    redis_pool_stats_t stats;
    redis_pool_get_stats(pool, &stats);

    TEST_ASSERT_EQUAL(0, stats.commands_sent);
    TEST_ASSERT_EQUAL(0, stats.commands_failed);

    redis_pool_destroy(pool);
}

void test_pool_reset_stats_null(void) {
    /* Should not crash */
    redis_pool_reset_stats(NULL);
    TEST_PASS();
}

void test_pool_reset_stats(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    redis_pool_reset_stats(pool);

    redis_pool_stats_t stats;
    redis_pool_get_stats(pool, &stats);
    TEST_ASSERT_EQUAL(0, stats.commands_sent);
    TEST_ASSERT_EQUAL(0, stats.commands_failed);

    redis_pool_destroy(pool);
}

// =============================================================================
// Health Tests
// =============================================================================

void test_pool_is_healthy_null(void) {
    int healthy = redis_pool_is_healthy(NULL);
    TEST_ASSERT_EQUAL(0, healthy);
}

void test_pool_is_healthy_not_running(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    int healthy = redis_pool_is_healthy(pool);
    TEST_ASSERT_EQUAL(0, healthy);

    redis_pool_destroy(pool);
}

void test_pool_available_null(void) {
    size_t available = redis_pool_available(NULL);
    TEST_ASSERT_EQUAL(0, available);
}

void test_pool_available_not_started(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = redis_pool_create(&config);

    size_t available = redis_pool_available(pool);
    TEST_ASSERT_EQUAL(0, available);

    redis_pool_destroy(pool);
}

// =============================================================================
// Replica Configuration Tests
// =============================================================================

void test_pool_with_replicas(void) {
    const char *replica_hosts[] = {"replica1.example.com", "replica2.example.com"};
    uint16_t replica_ports[] = {6379, 6380};

    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    config.replica_hosts = replica_hosts;
    config.replica_ports = replica_ports;
    config.replica_count = 2;

    redis_pool_t *pool = redis_pool_create(&config);
    TEST_ASSERT_NOT_NULL(pool);

    redis_pool_destroy(pool);
}

void test_pool_replica_hosts_null_with_count(void) {
    redis_pool_config_t config = REDIS_POOL_CONFIG_DEFAULT;
    config.replica_hosts = NULL;
    config.replica_count = 2;  /* Count but no hosts */

    TEST_ASSERT_NULL(redis_pool_create(&config));
}

// =============================================================================
// Live Redis Contract Tests
// =============================================================================

#define REDIS_POOL_LIVE_DATABASE 14
#define REDIS_POOL_LIVE_KEY "turbonet:test:pool:selected-db"
#define REDIS_POOL_LIVE_VALUE "pool-select-ok"

typedef struct {
    int status;
    const char *stage;
} redis_pool_live_case_t;

static int live_reply_equals(const redis_reply_t *reply,
                             redis_reply_type_t type,
                             const char *expected) {
    size_t expected_len = strlen(expected);
    return reply != NULL && reply->str != NULL && reply->type == type &&
           reply->len == expected_len &&
           memcmp(reply->str, expected, expected_len) == 0;
}

static void run_pool_selected_db_contract(coro_t *co, void *arg) {
    redis_pool_live_case_t *test_case = (redis_pool_live_case_t *)arg;
    redis_pool_config_t pool_config = REDIS_POOL_CONFIG_DEFAULT;
    redis_pool_t *pool = NULL;
    redis_client_t *db0 = NULL;
    redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
    const char *del_argv[] = {"DEL", REDIS_POOL_LIVE_KEY};
    const char *set_argv[] = {"SET", REDIS_POOL_LIVE_KEY, REDIS_POOL_LIVE_VALUE};
    const char *get_argv[] = {"GET", REDIS_POOL_LIVE_KEY};
    int status;

    (void)co;
    test_case->status = TURBO_EIO;
    test_case->stage = "create pool";

    pool_config.database = REDIS_POOL_LIVE_DATABASE;
    pool_config.min_connections = 1;
    pool_config.max_connections = 2;
    pool_config.connect_timeout_ms = 2000;
    pool_config.command_timeout_ms = 2000;

    pool = redis_pool_create(&pool_config);
    if (!pool) goto cleanup;

    test_case->stage = "start pool";
    if (redis_pool_start(pool) != TURBO_OK) goto cleanup;

    test_case->stage = "connect db0 client";
    db0 = redis_client_create("127.0.0.1", 6379);
    if (!db0 || redis_client_connect(db0, NULL, NULL) != TURBO_OK) goto cleanup;

    test_case->stage = "clear db0 key";
    status = redis_commandv_result(db0, 2, del_argv, NULL, &result);
    redis_command_result_clear(&result);
    if (status != TURBO_OK) goto cleanup;

    test_case->stage = "set through selected pool database";
    status = redis_pool_commandv_result(pool, 0, 3, set_argv, NULL, &result);
    if (status != TURBO_OK ||
        !live_reply_equals(result.reply, REDIS_REPLY_STRING, "OK")) {
        redis_command_result_clear(&result);
        goto cleanup;
    }
    redis_command_result_clear(&result);

    test_case->stage = "get through selected pool database";
    status = redis_pool_commandv_result(pool, 1, 2, get_argv, NULL, &result);
    if (status != TURBO_OK ||
        !live_reply_equals(result.reply, REDIS_REPLY_BULK_STRING,
                           REDIS_POOL_LIVE_VALUE)) {
        redis_command_result_clear(&result);
        goto cleanup;
    }
    redis_command_result_clear(&result);

    test_case->stage = "verify db0 isolation";
    status = redis_commandv_result(db0, 2, get_argv, NULL, &result);
    if (status != TURBO_OK || !result.reply ||
        result.reply->type != REDIS_REPLY_NULL) {
        redis_command_result_clear(&result);
        goto cleanup;
    }
    redis_command_result_clear(&result);

    test_case->stage = "delete selected database key";
    status = redis_pool_commandv_result(pool, 0, 2, del_argv, NULL, &result);
    redis_command_result_clear(&result);
    if (status != TURBO_OK) goto cleanup;

    test_case->stage = "complete";
    test_case->status = TURBO_OK;

cleanup:
    redis_command_result_clear(&result);
    redis_client_destroy(db0);
    redis_pool_stop(pool);
    redis_pool_destroy(pool);
}

void test_pool_live_applies_selected_database(void) {
    const char *enabled = getenv("TURBONET_REDIS_LIVE");
    redis_pool_live_case_t test_case = {TURBO_EIO, "create context"};
    coro_context_t *ctx;

    if (!enabled || strcmp(enabled, "1") != 0) {
        TEST_PASS();
    }

    ctx = coro_context_create(NULL);
    TEST_ASSERT_NOT_NULL(ctx);
    TEST_ASSERT_EQUAL(TURBO_OK,
                      coro_context_spawn(ctx, run_pool_selected_db_contract,
                                         &test_case));
    coro_context_run(ctx, TURBO_RUN_DEFAULT);
    coro_context_destroy(ctx);

    if (test_case.status != TURBO_OK) {
        fprintf(stderr, "live Redis pool contract failed at stage: %s\n",
                test_case.stage);
    }
    TEST_ASSERT_EQUAL(TURBO_OK, test_case.status);
}

suite("redis_pool") {
    before_each() {
        setUp();
    }

    after_each() {
        tearDown();
    }

    group("Configuration") {
        REDIS_RUN_TEST(test_pool_create_default, "should create default pool");
        REDIS_RUN_TEST(test_pool_create_custom, "should create custom pool");
        REDIS_RUN_TEST(test_pool_create_null_config, "should reject null config");
        REDIS_RUN_TEST(test_pool_create_null_host, "should reject null host");
        REDIS_RUN_TEST(test_pool_rejects_username_without_password, "should reject ACL username without password");
        REDIS_RUN_TEST(test_pool_rejects_invalid_database, "should reject invalid database");
        REDIS_RUN_TEST(test_pool_destroy_null, "should destroy null pool safely");
    }

    group("Lifecycle") {
        REDIS_RUN_TEST(test_pool_stop_null, "should stop null pool safely");
        REDIS_RUN_TEST(test_pool_start_null, "should reject null pool start");
        REDIS_RUN_TEST(test_pool_start_without_coro_context_fails, "should fail start without coroutine context");
        REDIS_RUN_TEST(test_pool_stop_twice, "should stop pool twice safely");
    }

    group("Acquisition") {
        REDIS_RUN_TEST(test_pool_acquire_null, "should reject acquire on null pool");
        REDIS_RUN_TEST(test_pool_acquire_not_running, "should reject acquire on stopped pool");
        REDIS_RUN_TEST(test_pool_release_null, "should release null safely");
        REDIS_RUN_TEST(test_pool_conn_client_null, "should return null client for null connection");
    }

    group("Commands") {
        REDIS_RUN_TEST(test_pool_command_null, "should reject null pool command");
        REDIS_RUN_TEST(test_pool_command_null_format, "should reject null command format");
        REDIS_RUN_TEST(test_pool_commandv_null, "should reject null pool commandv");
        REDIS_RUN_TEST(test_pool_commandv_null_argv, "should reject null argv");
        REDIS_RUN_TEST(test_pool_commandv_zero_argc, "should reject zero argc");
        REDIS_RUN_TEST(test_pool_command_result_reports_not_connected, "should report a result rejected before pool acquisition");
        REDIS_RUN_TEST(test_pool_read_command_null, "should reject null read command");
    }

    group("Convenience Commands") {
        REDIS_RUN_TEST(test_pool_set_not_running, "should reject set when pool not running");
        REDIS_RUN_TEST(test_pool_get_not_running, "should reject get when pool not running");
        REDIS_RUN_TEST(test_pool_del_not_running, "should reject del when pool not running");
        REDIS_RUN_TEST(test_pool_expire_not_running, "should reject expire when pool not running");
        REDIS_RUN_TEST(test_pool_hset_not_running, "should reject hset when pool not running");
        REDIS_RUN_TEST(test_pool_hget_not_running, "should reject hget when pool not running");
        REDIS_RUN_TEST(test_pool_xadd_not_running, "should reject xadd when pool not running");
        REDIS_RUN_TEST(test_pool_xreadgroup_not_running, "should reject xreadgroup when pool not running");
    }

    group("Pipeline") {
        REDIS_RUN_TEST(test_pipeline_create_null, "should reject null pipeline create");
        REDIS_RUN_TEST(test_pipeline_create_destroy, "should create and destroy pipeline");
        REDIS_RUN_TEST(test_pipeline_destroy_null, "should destroy null pipeline safely");
        REDIS_RUN_TEST(test_pipeline_add_null, "should reject add on null pipeline");
        REDIS_RUN_TEST(test_pipeline_add_null_format, "should reject null pipeline format");
        REDIS_RUN_TEST(test_pipeline_add_commands, "should add commands to pipeline");
        REDIS_RUN_TEST(test_pipeline_addv_null, "should reject invalid addv arguments");
        REDIS_RUN_TEST(test_pipeline_execute_null, "should reject execute on null pipeline");
        REDIS_RUN_TEST(test_pipeline_sync_null, "should reject sync on null pipeline");
        REDIS_RUN_TEST(test_pipeline_execute_empty, "should reject empty pipeline execute");
        REDIS_RUN_TEST(test_pipeline_grow_capacity, "should grow pipeline capacity");
        REDIS_RUN_TEST(test_pipeline_enforces_configured_capacity, "should enforce configured pipeline capacity");
        REDIS_RUN_TEST(test_pipeline_clear_allows_reuse, "should clear queued commands and allow reuse");
        REDIS_RUN_TEST(test_pipeline_replies_free_null_safe, "should free null reply arrays safely");
#ifdef REDIS_TESTING
        REDIS_RUN_TEST(test_pipeline_batch_exec_sends_single_resp_batch, "should send pipeline commands as a single RESP batch");
        REDIS_RUN_TEST(test_pipeline_execute_collect_returns_ordered_replies, "should collect pipeline replies in order");
        REDIS_RUN_TEST(test_pipeline_addv_binary_safe_sends_single_batch, "should preserve binary-safe addv arguments in a single batch");
        REDIS_RUN_TEST(test_pipeline_result_preserves_completion_state, "should preserve pipeline completion state");
#endif
    }

    group("Statistics") {
        REDIS_RUN_TEST(test_pool_get_stats_null, "should read stats safely for null pool");
        REDIS_RUN_TEST(test_pool_get_stats, "should expose zeroed stats for fresh pool");
        REDIS_RUN_TEST(test_pool_reset_stats_null, "should reset null pool stats safely");
        REDIS_RUN_TEST(test_pool_reset_stats, "should reset pool stats");
    }

    group("Health") {
        REDIS_RUN_TEST(test_pool_is_healthy_null, "should report null pool unhealthy");
        REDIS_RUN_TEST(test_pool_is_healthy_not_running, "should report stopped pool unhealthy");
        REDIS_RUN_TEST(test_pool_available_null, "should report zero available for null pool");
        REDIS_RUN_TEST(test_pool_available_not_started, "should report zero available before start");
    }

    group("Replicas") {
        REDIS_RUN_TEST(test_pool_with_replicas, "should create pool with replicas");
        REDIS_RUN_TEST(test_pool_replica_hosts_null_with_count, "should reject replica count without endpoints");
    }

    group("Live Contract") {
        REDIS_RUN_TEST(test_pool_live_applies_selected_database,
                       "should initialize every physical connection with the selected database");
    }
}
