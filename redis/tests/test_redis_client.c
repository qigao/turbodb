/**
 * @file test_redis_client.c
 * @brief Unit tests for Redis Client (RESP Protocol)
 */

#include "../redis_client.h"
#include "../redis_internal.h"
#include "turbo_str.h"
#include "tinytest.h"
#include "turbo_error.h"
#include <string.h>
#include <stdio.h>

#define TEST_PASS() do { check(1); return; } while (0)
#define TEST_ASSERT_TRUE(expr) check((expr))
#define TEST_ASSERT_NULL(value) check_null((value))
#define TEST_ASSERT_NOT_NULL(value) check_not_null((value))
#define TEST_ASSERT_EQUAL(expected, actual) check((intptr_t)(actual) == (intptr_t)(expected))
#define TEST_ASSERT_EQUAL_STRING(expected, actual) check_equal((actual), (expected))
#define TEST_ASSERT_GREATER_THAN(threshold, actual) check((long long)(actual) > (long long)(threshold))
#define TEST_ASSERT_LESS_THAN(limit, actual) check((long long)(actual) < (long long)(limit))
#define REDIS_RUN_TEST(fn, label) it(label) { fn(); }

void setUp(void) {
}

void tearDown(void) {
}

static void on_connect_status(redis_client_t *client, int status, void *user_data) {
    int *captured = (int *)user_data;
    (void)client;
    if (captured) {
        *captured = status;
    }
}

// =============================================================================
// Configuration Tests
// =============================================================================

void test_create_client_default(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(6379, client->config.port);
    TEST_ASSERT_EQUAL_STRING("127.0.0.1", client->config.host);
    redis_client_destroy(client);
}

void test_create_client_with_config(void) {
    redis_config_t config = {
        .host = "redis.example.com",
        .port = 6380,
        .username = "worker",
        .password = "secret123",
        .database = 5,
        .timeout_ms = 10000,
        .command_timeout_ms = 8000,
        .max_pipeline = 50
    };

    redis_client_t *client = redis_client_create_with_config(&config);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL_STRING("redis.example.com", client->config.host);
    TEST_ASSERT_EQUAL(6380, client->config.port);
    TEST_ASSERT_EQUAL_STRING("worker", client->config.username);
    TEST_ASSERT_EQUAL_STRING("secret123", client->config.password);
    TEST_ASSERT_EQUAL(5, client->config.database);
    TEST_ASSERT_EQUAL(10000, client->config.timeout_ms);
    TEST_ASSERT_EQUAL(8000, client->config.command_timeout_ms);
    TEST_ASSERT_EQUAL(50, client->config.max_pipeline);
    redis_client_destroy(client);
}

void test_create_client_null_host(void) {
    redis_config_t config = {
        .host = NULL,
        .port = 6379
    };

    redis_client_t *client = redis_client_create_with_config(&config);
    TEST_ASSERT_NULL(client);
}

void test_create_client_normalizes_command_timeout(void) {
    redis_config_t config = {
        .host = "127.0.0.1",
        .port = 6379,
        .timeout_ms = 2500
    };
    redis_client_t *client = redis_client_create_with_config(&config);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(2500, client->config.command_timeout_ms);
    redis_client_destroy(client);
}

void test_create_client_rejects_username_without_password(void) {
    redis_config_t config = {
        .host = "127.0.0.1",
        .port = 6379,
        .username = "worker"
    };
    TEST_ASSERT_NULL(redis_client_create_with_config(&config));
}

void test_destroy_null_client(void) {
    /* Should not crash */
    redis_client_destroy(NULL);
    TEST_PASS();
}

void test_disconnect_null_client(void) {
    /* Should not crash */
    redis_client_disconnect(NULL);
    TEST_PASS();
}

void test_interrupt_without_connection(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(TURBO_EINVAL, redis_client_interrupt(NULL, TURBO_ESHUTDOWN));
    TEST_ASSERT_EQUAL(TURBO_ENOTCONN, redis_client_interrupt(client, TURBO_ESHUTDOWN));
    redis_client_destroy(client);
}

// =============================================================================
// Initial State Tests
// =============================================================================

void test_client_initial_state(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);

    TEST_ASSERT_EQUAL(0, client->is_connected);
    TEST_ASSERT_EQUAL(0, client->is_authenticated);
    TEST_ASSERT_EQUAL(0, client->queued_commands);
    TEST_ASSERT_NULL(client->command_queue);
    TEST_ASSERT_NULL(client->command_queue_tail);
    TEST_ASSERT_EQUAL(0, client->is_subscriber);

    redis_client_destroy(client);
}

void test_client_recv_buffer_allocated(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_NOT_NULL(client->recv_buffer);
    TEST_ASSERT_GREATER_THAN(0, client->recv_buffer_size);
    TEST_ASSERT_EQUAL(0, client->recv_buffer_used);
    redis_client_destroy(client);
}

void test_client_connect_without_coro_context_fails(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int status = 123;
    int result;

    TEST_ASSERT_NOT_NULL(client);
    result = redis_client_connect(client, on_connect_status, &status);

    TEST_ASSERT_EQUAL(TURBO_EINVAL, result);
    TEST_ASSERT_EQUAL(TURBO_EINVAL, status);
    TEST_ASSERT_EQUAL(0, client->is_connected);

    redis_client_destroy(client);
}

void test_client_prepare_requires_connection(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(TURBO_EINVAL, redis_client_prepare(NULL));
    TEST_ASSERT_EQUAL(TURBO_ENOTCONN, redis_client_prepare(client));
    redis_client_destroy(client);
}

// =============================================================================
// Reply Structure Tests
// =============================================================================

void test_reply_free_null(void) {
    /* Should not crash */
    redis_reply_free(NULL);
    TEST_PASS();
}

void test_reply_type_values(void) {
    TEST_ASSERT_EQUAL(0, REDIS_REPLY_STRING);
    TEST_ASSERT_EQUAL(1, REDIS_REPLY_ERROR);
    TEST_ASSERT_EQUAL(2, REDIS_REPLY_INTEGER);
    TEST_ASSERT_EQUAL(3, REDIS_REPLY_BULK_STRING);
    TEST_ASSERT_EQUAL(4, REDIS_REPLY_ARRAY);
    TEST_ASSERT_EQUAL(5, REDIS_REPLY_NULL);
}

static int parse_blob(redis_client_t *client, const void *data, size_t len,
                      redis_reply_t **reply) {
    if (redis_recv_buffer_append(client, data, len) != 0) return -1;
    return redis_parse_resp_reply(client, reply);
}

void test_resp_parser_parses_nested_binary_reply(void) {
    static const char reply_data[] =
        "*3\r\n+OK\r\n$3\r\na\0b\r\n:42\r\n";
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    redis_reply_t *reply = NULL;
    int consumed;

    TEST_ASSERT_NOT_NULL(client);
    consumed = parse_blob(client, reply_data, sizeof(reply_data) - 1u, &reply);
    TEST_ASSERT_EQUAL((int)(sizeof(reply_data) - 1u), consumed);
    TEST_ASSERT_NOT_NULL(reply);
    TEST_ASSERT_EQUAL(REDIS_REPLY_ARRAY, reply->type);
    TEST_ASSERT_EQUAL(3, reply->element_count);
    TEST_ASSERT_EQUAL_STRING("OK", reply->elements[0]->str);
    TEST_ASSERT_EQUAL(3, reply->elements[1]->len);
    TEST_ASSERT_EQUAL(3, tstr_len((tstr)reply->elements[1]->str));
    check_equal(reply->elements[1]->str, "a\0b", 3u);
    TEST_ASSERT_EQUAL(42, reply->elements[2]->integer);

    redis_reply_free(reply);
    redis_client_destroy(client);
}

void test_resp_parser_retries_incomplete_reply(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    redis_reply_t *reply = (redis_reply_t *)(uintptr_t)1u;
    int consumed;

    TEST_ASSERT_NOT_NULL(client);
    consumed = parse_blob(client, "$5\r\nhe", 6u, &reply);
    TEST_ASSERT_EQUAL(0, consumed);
    TEST_ASSERT_NULL(reply);
    consumed = parse_blob(client, "llo\r\n", 5u, &reply);
    TEST_ASSERT_EQUAL(11, consumed);
    TEST_ASSERT_NOT_NULL(reply);
    TEST_ASSERT_EQUAL(REDIS_REPLY_BULK_STRING, reply->type);
    TEST_ASSERT_EQUAL_STRING("hello", reply->str);

    redis_reply_free(reply);
    redis_client_destroy(client);
}

void test_resp_parser_rejects_malformed_headers(void) {
    static const char *invalid[] = {
        ":12x\r\n",
        ":9223372036854775808\r\n",
        "$-2\r\n",
        "*-2\r\n",
        "+OK\n",
        "?wat\r\n",
        "$3\r\nabcXX"
    };

    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        redis_client_t *client = redis_client_create("127.0.0.1", 6379);
        redis_reply_t *reply = NULL;
        TEST_ASSERT_NOT_NULL(client);
        TEST_ASSERT_EQUAL(-1, parse_blob(client, invalid[i], strlen(invalid[i]), &reply));
        TEST_ASSERT_NULL(reply);
        redis_client_destroy(client);
    }
}

void test_resp_array_reader_yields_fragmented_top_level_items(void) {
    static const char first[] = "*3\r\n:2\r\n$3\r\na";
    static const char second[] = "bc\r\n*2\r\n:7\r\n:9\r\n";
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    redis_resp_array_reader reader;
    redis_reply_t *item = NULL;

    TEST_ASSERT_NOT_NULL(client);
    redis_resp_array_reader_init(&reader, 3u, 512u);
    TEST_ASSERT_EQUAL(0, redis_recv_buffer_append_bounded(
                             client, first, sizeof(first) - 1u, 64u));
    TEST_ASSERT_EQUAL(REDIS_RESP_ARRAY_ITEM,
                      redis_resp_array_reader_next(client, &reader, &item));
    TEST_ASSERT_EQUAL(REDIS_REPLY_INTEGER, item->type);
    TEST_ASSERT_EQUAL(2, item->integer);
    redis_reply_free(item);
    item = NULL;

    TEST_ASSERT_EQUAL(REDIS_RESP_ARRAY_NEED_MORE,
                      redis_resp_array_reader_next(client, &reader, &item));
    TEST_ASSERT_NULL(item);
    TEST_ASSERT_EQUAL(0, redis_recv_buffer_append_bounded(
                             client, second, sizeof(second) - 1u, 64u));
    TEST_ASSERT_EQUAL(REDIS_RESP_ARRAY_ITEM,
                      redis_resp_array_reader_next(client, &reader, &item));
    TEST_ASSERT_EQUAL(REDIS_REPLY_BULK_STRING, item->type);
    TEST_ASSERT_EQUAL(3, item->len);
    check_equal(item->str, "abc", 3u);
    redis_reply_free(item);
    item = NULL;
    TEST_ASSERT_EQUAL(REDIS_RESP_ARRAY_ITEM,
                      redis_resp_array_reader_next(client, &reader, &item));
    TEST_ASSERT_EQUAL(REDIS_REPLY_ARRAY, item->type);
    TEST_ASSERT_EQUAL(2, item->element_count);
    TEST_ASSERT_EQUAL(7, item->elements[0]->integer);
    TEST_ASSERT_EQUAL(9, item->elements[1]->integer);
    redis_reply_free(item);
    item = NULL;
    TEST_ASSERT_EQUAL(REDIS_RESP_ARRAY_DONE,
                      redis_resp_array_reader_next(client, &reader, &item));
    redis_client_destroy(client);
}

void test_resp_bounded_append_rejects_oversized_fragment(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(0, redis_recv_buffer_append_bounded(client, "1234", 4u, 4u));
    TEST_ASSERT_EQUAL(-2, redis_recv_buffer_append_bounded(client, "5", 1u, 4u));
    TEST_ASSERT_EQUAL(4, client->recv_buffer_used);
    redis_client_destroy(client);
}

void test_resp_array_reader_rejects_reply_allocation_over_budget(void) {
    static const char reply_data[] = "*1\r\n$32\r\n0123456789abcdefghijklmnopqrstuv\r\n";
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    redis_resp_array_reader reader;
    redis_reply_t *item = NULL;

    TEST_ASSERT_NOT_NULL(client);
    redis_resp_array_reader_init(&reader, 1u, 32u);
    TEST_ASSERT_EQUAL(0, redis_recv_buffer_append_bounded(
                             client, reply_data, sizeof(reply_data) - 1u,
                             sizeof(reply_data)));
    TEST_ASSERT_EQUAL(REDIS_RESP_ARRAY_LIMIT,
                      redis_resp_array_reader_next(client, &reader, &item));
    TEST_ASSERT_NULL(item);
    redis_client_destroy(client);
}

void test_command_stream_reserves_transport_until_destroyed(void) {
    const char *arguments[] = {"PING"};
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    redis_command_stream_t *first = NULL;
    redis_command_stream_t *second = NULL;

    TEST_ASSERT_NOT_NULL(client);
    client->is_connected = 1;
    client->socket = (coro_socket_t *)(uintptr_t)1u;
    TEST_ASSERT_EQUAL(TURBO_OK,
                      redis_commandv_stream_open(client, 1, arguments, NULL,
                                                 256u, 1u, &first));
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL(TURBO_EBUSY,
                      redis_commandv_stream_open(client, 1, arguments, NULL,
                                                 256u, 1u, &second));
    TEST_ASSERT_NULL(second);

    redis_command_stream_destroy(first);
    TEST_ASSERT_NULL(client->active_stream);
    TEST_ASSERT_EQUAL(1, client->is_connected);
    client->socket = NULL;
    client->is_connected = 0;
    redis_client_destroy(client);
}

// =============================================================================
// Stream Structure Tests
// =============================================================================

void test_stream_entry_free_null(void) {
    /* Should not crash */
    redis_stream_entry_free(NULL);
    TEST_PASS();
}

void test_stream_entry_free_partial_allocation(void) {
    redis_stream_entry_t entry = {0};
    entry.field_count = 1;
    entry.fields = calloc(1, sizeof(*entry.fields));
    TEST_ASSERT_NOT_NULL(entry.fields);

    /* A later allocation may fail while fields already belong to the entry. */
    redis_stream_entry_free(&entry);
    TEST_PASS();
}

void test_stream_entry_take_value_transfers_binary_and_empty_values(void) {
    static const unsigned char binary[] = {0x00u, 0x7fu, 0xffu};
    redis_stream_entry_t entry = {0};
    char *binary_value = NULL;
    char *empty_value = NULL;
    size_t binary_len = 0;
    size_t empty_len = 1;

    entry.field_count = 2;
    entry.values = calloc(entry.field_count, sizeof(*entry.values));
    entry.value_lens = calloc(entry.field_count, sizeof(*entry.value_lens));
    TEST_ASSERT_NOT_NULL(entry.values);
    TEST_ASSERT_NOT_NULL(entry.value_lens);
    entry.values[0] = malloc(sizeof(binary));
    entry.values[1] = malloc(1u);
    TEST_ASSERT_NOT_NULL(entry.values[0]);
    TEST_ASSERT_NOT_NULL(entry.values[1]);
    memcpy(entry.values[0], binary, sizeof(binary));
    entry.values[1][0] = '\0';
    entry.value_lens[0] = sizeof(binary);

    TEST_ASSERT_EQUAL(TURBO_OK,
                      redis_stream_entry_take_value(&entry, 0u, &binary_value, &binary_len));
    TEST_ASSERT_NOT_NULL(binary_value);
    TEST_ASSERT_EQUAL(sizeof(binary), binary_len);
    check_equal(binary_value, binary, sizeof(binary));
    TEST_ASSERT_NULL(entry.values[0]);
    TEST_ASSERT_EQUAL(0, entry.value_lens[0]);

    TEST_ASSERT_EQUAL(TURBO_OK,
                      redis_stream_entry_take_value(&entry, 1u, &empty_value, &empty_len));
    TEST_ASSERT_NOT_NULL(empty_value);
    TEST_ASSERT_EQUAL(0, empty_len);
    TEST_ASSERT_NULL(entry.values[1]);

    redis_stream_entry_free(&entry);
    redis_stream_value_free(binary_value);
    redis_stream_value_free(empty_value);
}

void test_stream_entry_take_value_rejects_invalid_or_absent_values(void) {
    redis_stream_entry_t entry = {0};
    char *value = (char *)(uintptr_t)1u;
    size_t value_len = 7u;

    entry.field_count = 1;
    entry.values = calloc(1u, sizeof(*entry.values));
    entry.value_lens = calloc(1u, sizeof(*entry.value_lens));
    TEST_ASSERT_NOT_NULL(entry.values);
    TEST_ASSERT_NOT_NULL(entry.value_lens);

    TEST_ASSERT_EQUAL(TURBO_EINVAL,
                      redis_stream_entry_take_value(NULL, 0u, &value, &value_len));
    TEST_ASSERT_NULL(value);
    TEST_ASSERT_EQUAL(0, value_len);
    TEST_ASSERT_EQUAL(TURBO_ERANGE,
                      redis_stream_entry_take_value(&entry, 1u, &value, &value_len));
    TEST_ASSERT_EQUAL(TURBO_ENOENT,
                      redis_stream_entry_take_value(&entry, 0u, &value, &value_len));
    TEST_ASSERT_EQUAL(TURBO_EINVAL,
                      redis_stream_entry_take_value(&entry, 0u, NULL, &value_len));
    redis_stream_value_free(NULL);
    redis_stream_entry_free(&entry);
}

void test_stream_result_free_null(void) {
    /* Should not crash */
    redis_stream_result_free(NULL, 0);
    redis_stream_result_free(NULL, 5);
    TEST_PASS();
}

// =============================================================================
// Command API Tests (without connection)
// =============================================================================

void test_command_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);

    /* Commands should fail when not connected */
    int result = redis_set(client, "key", "value", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    result = redis_get(client, "key", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_commandv_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);

    const char *argv[] = {"SET", "key", "value"};
    int result = redis_commandv(client, 3, argv, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_commandv_null_client(void) {
    const char *argv[] = {"SET", "key", "value"};
    int result = redis_commandv(NULL, 3, argv, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_command_result_distinguishes_not_sent(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
    const char *argv[] = {"PING"};
    TEST_ASSERT_NOT_NULL(client);
    TEST_ASSERT_EQUAL(TURBO_ENOTCONN,
                      redis_commandv_result(client, 1, argv, NULL, &result));
    TEST_ASSERT_EQUAL(TURBO_ENOTCONN, result.status);
    TEST_ASSERT_EQUAL(REDIS_COMMAND_NOT_SENT, result.outcome);
    TEST_ASSERT_EQUAL(REDIS_SERVER_ERROR_NONE, result.server_error);
    TEST_ASSERT_NULL(result.reply);
    redis_command_result_clear(&result);
    redis_client_destroy(client);
}

void test_script_results_preserve_not_sent(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
    const char *keys[] = {"{person}:1"};
    const char *args[] = {"name", "Ada"};
    TEST_ASSERT_NOT_NULL(client);

    TEST_ASSERT_EQUAL(TURBO_ENOTCONN,
                      redis_eval_result(client, "return 1", 1, keys, 2, args, &result));
    TEST_ASSERT_EQUAL(REDIS_COMMAND_NOT_SENT, result.outcome);
    redis_command_result_clear(&result);

    TEST_ASSERT_EQUAL(TURBO_ENOTCONN,
                      redis_evalsha_result(client, "0123456789abcdef", 1, keys,
                                           2, args, &result));
    TEST_ASSERT_EQUAL(REDIS_COMMAND_NOT_SENT, result.outcome);
    redis_command_result_clear(&result);

    TEST_ASSERT_EQUAL(TURBO_ENOTCONN,
                      redis_script_load_result(client, "return 1", &result));
    TEST_ASSERT_EQUAL(REDIS_COMMAND_NOT_SENT, result.outcome);
    redis_command_result_clear(&result);
    redis_client_destroy(client);
}

void test_server_error_classification(void) {
    redis_reply_t reply = {0};
    reply.type = REDIS_REPLY_ERROR;
    reply.str = "BUSYGROUP Consumer Group name already exists";
    reply.len = strlen(reply.str);
    TEST_ASSERT_EQUAL(REDIS_SERVER_ERROR_BUSY_GROUP,
                      redis_server_error_classify(&reply));
    reply.str = "NOGROUP No such key or consumer group";
    reply.len = strlen(reply.str);
    TEST_ASSERT_EQUAL(REDIS_SERVER_ERROR_NO_GROUP,
                      redis_server_error_classify(&reply));
    reply.str = "some future redis error";
    reply.len = strlen(reply.str);
    TEST_ASSERT_EQUAL(REDIS_SERVER_ERROR_UNKNOWN,
                      redis_server_error_classify(&reply));
}

// =============================================================================
// Stream API Tests (without connection)
// =============================================================================

void test_xadd_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);

    const char *fields[] = {"field1"};
    const char *values[] = {"value1"};

    int result = redis_xadd(client, "stream", 1000,
                            1, fields, values, NULL,
                            NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_xadd_null_params(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);

    /* NULL key */
    int result = redis_xadd(client, NULL, 0, 1, NULL, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    /* NULL fields */
    result = redis_xadd(client, "stream", 0, 1, NULL, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    /* Zero field count */
    const char *fields[] = {"f"};
    const char *values[] = {"v"};
    result = redis_xadd(client, "stream", 0, 0, fields, values, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_xread_null_params(void) {
    int result = redis_xread(NULL, 10, 1000, 1, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_t *client = redis_client_create("127.0.0.1", 6379);

    result = redis_xread(client, 10, 1000, 0, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_xreadgroup_null_params(void) {
    int result = redis_xreadgroup(NULL, "group", "consumer",
                                   10, 1000, 1, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_t *client = redis_client_create("127.0.0.1", 6379);

    result = redis_xreadgroup(client, NULL, "consumer",
                               10, 1000, 1, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    result = redis_xreadgroup(client, "group", NULL,
                               10, 1000, 1, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_xgroup_create_null_params(void) {
    int result = redis_xgroup_create(NULL, "stream", "group", "0", 1, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_t *client = redis_client_create("127.0.0.1", 6379);

    result = redis_xgroup_create(client, NULL, "group", "0", 1, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    result = redis_xgroup_create(client, "stream", NULL, "0", 1, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_xack_null_params(void) {
    int result = redis_xack(NULL, "stream", "group", 1, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_t *client = redis_client_create("127.0.0.1", 6379);

    result = redis_xack(client, NULL, "group", 1, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    result = redis_xack(client, "stream", NULL, 1, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    result = redis_xack(client, "stream", "group", 0, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_xdel_null_params(void) {
    int result = redis_xdel(NULL, "stream", 1, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_t *client = redis_client_create("127.0.0.1", 6379);

    result = redis_xdel(client, NULL, 1, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    result = redis_xdel(client, "stream", 0, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

// =============================================================================
// Pub/Sub API Tests (without connection)
// =============================================================================

void test_publish_null_params(void) {
    int result = redis_publish(NULL, "channel", "msg", 3, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_t *client = redis_client_create("127.0.0.1", 6379);

    result = redis_publish(client, NULL, "msg", 3, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_subscribe_null_params(void) {
    int result = redis_subscribe(NULL, 1, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_t *client = redis_client_create("127.0.0.1", 6379);

    result = redis_subscribe(client, 0, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_psubscribe_null_params(void) {
    int result = redis_psubscribe(NULL, 1, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_t *client = redis_client_create("127.0.0.1", 6379);

    result = redis_psubscribe(client, 0, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_client_destroy(client);
}

void test_unsubscribe_null_client(void) {
    int result = redis_unsubscribe(NULL, 0, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_punsubscribe_null_client(void) {
    int result = redis_punsubscribe(NULL, 0, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

// =============================================================================
// Convenience Command Tests (without connection)
// =============================================================================

void test_ping_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_ping(client, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_del_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    const char *keys[] = {"key1", "key2"};
    int result = redis_del(client, 2, keys, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_exists_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_exists(client, "key", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_expire_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_expire(client, "key", 3600, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_incr_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_incr(client, "counter", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_lpush_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    const char *values[] = {"a", "b"};
    int result = redis_lpush(client, "list", 2, values, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_rpush_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    const char *values[] = {"a", "b"};
    int result = redis_rpush(client, "list", 2, values, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_lpop_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_lpop(client, "list", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_rpop_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_rpop(client, "list", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_hset_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_hset(client, "hash", "field", "value", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_hget_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_hget(client, "hash", "field", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_sadd_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    const char *members[] = {"m1", "m2"};
    int result = redis_sadd(client, "set", 2, members, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_smembers_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_smembers(client, "set", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_xlen_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_xlen(client, "stream", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

void test_xtrim_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    int result = redis_xtrim(client, "stream", 1000, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
    redis_client_destroy(client);
}

// =============================================================================
// Error Message Tests
// =============================================================================

void test_get_error_null_client(void) {
    const char *err = redis_client_get_error(NULL);
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL_STRING("Invalid client", err);
}

void test_get_error_valid_client(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    const char *err = redis_client_get_error(client);
    TEST_ASSERT_NOT_NULL(err);
    redis_client_destroy(client);
}

void test_ext_commands_not_connected(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);

    TEST_ASSERT_EQUAL(-1, redis_mget(client, 1, (const char*[]){"k"}, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_setnx(client, "k", "v", NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_setex(client, "k", 10, "v", NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_incrby(client, "k", 5, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_lrange(client, "k", 0, -1, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_hgetall(client, "k", NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_sismember(client, "k", "m", NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_zcard(client, "k", NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_ttl(client, "k", NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_select(client, 1, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_multi(client, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_eval(client, "return nil", 0, NULL, 0, NULL, NULL, NULL));

    // HyperLogLog
    TEST_ASSERT_EQUAL(-1, redis_pfadd(client, "hll", 1, (const char*[]){"e"}, NULL, NULL));
    // Geo
    TEST_ASSERT_EQUAL(-1, redis_geodist(client, "geo", "m1", "m2", "km", NULL, NULL));
    // Bitmaps
    TEST_ASSERT_EQUAL(-1, redis_getbit(client, "bitmap", 10, NULL, NULL));
    // Client/Connection
    TEST_ASSERT_EQUAL(-1, redis_auth(client, "user", "pass", NULL, NULL));

    redis_client_destroy(client);
}

void test_ext_commands_null_params(void) {
    redis_client_t *client = redis_client_create("127.0.0.1", 6379);
    TEST_ASSERT_NOT_NULL(client);

    TEST_ASSERT_EQUAL(-1, redis_mset(NULL, 1, NULL, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_mset(client, 0, NULL, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_mget(client, 0, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_lmove(client, NULL, NULL, NULL, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_hmset(client, "k", 0, NULL, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_zadd(client, "k", 0, NULL, NULL, NULL, NULL));

    TEST_ASSERT_EQUAL(-1, redis_xrange(client, NULL, NULL, NULL, 0, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_xrevrange(client, NULL, NULL, NULL, 0, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_xpending(client, NULL, NULL, NULL, NULL, 0, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_xclaim(client, NULL, NULL, NULL, 0, 0, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_xautoclaim(client, NULL, NULL, NULL, 0, NULL, 0, NULL, NULL));
    TEST_ASSERT_EQUAL(-1, redis_xgroup_setid(client, NULL, NULL, NULL, NULL, NULL));

    // HyperLogLog
    TEST_ASSERT_EQUAL(-1, redis_pfadd(client, NULL, 0, NULL, NULL, NULL));
    // Geo
    TEST_ASSERT_EQUAL(-1, redis_geoadd(client, "geo", 0, NULL, NULL, NULL, NULL, NULL));
    // Bitmaps
    TEST_ASSERT_EQUAL(-1, redis_bitop(client, NULL, NULL, 0, NULL, NULL, NULL));
    // Client/Connection
    TEST_ASSERT_EQUAL(-1, redis_auth(client, NULL, NULL, NULL, NULL));

    redis_client_destroy(client);
}


suite("redis_client") {
    before_each() {
        setUp();
    }

    after_each() {
        tearDown();
    }

    group("Configuration") {
        REDIS_RUN_TEST(test_create_client_default, "should create client with defaults");
        REDIS_RUN_TEST(test_create_client_with_config, "should create client with custom config");
        REDIS_RUN_TEST(test_create_client_null_host, "should handle null host");
        REDIS_RUN_TEST(test_create_client_normalizes_command_timeout, "should default command timeout to connect timeout");
        REDIS_RUN_TEST(test_create_client_rejects_username_without_password, "should reject ACL username without password");
        REDIS_RUN_TEST(test_destroy_null_client, "should destroy null client safely");
        REDIS_RUN_TEST(test_disconnect_null_client, "should disconnect null client safely");
        REDIS_RUN_TEST(test_interrupt_without_connection, "should report absent socket on interrupt");
    }

    group("Initial State") {
        REDIS_RUN_TEST(test_client_initial_state, "should start disconnected with empty compatibility queue state");
        REDIS_RUN_TEST(test_client_recv_buffer_allocated, "should allocate recv buffer");
        REDIS_RUN_TEST(test_client_connect_without_coro_context_fails, "should fail connect without coroutine context");
        REDIS_RUN_TEST(test_client_prepare_requires_connection, "should prepare only an attached connection");
    }

    group("Reply Structure") {
        REDIS_RUN_TEST(test_reply_free_null, "should free null reply safely");
        REDIS_RUN_TEST(test_reply_type_values, "should expose stable reply type values");
        REDIS_RUN_TEST(test_resp_parser_parses_nested_binary_reply, "should parse nested binary replies with pooled nodes and tstr values");
        REDIS_RUN_TEST(test_resp_parser_retries_incomplete_reply, "should retry an incomplete RESP reply");
        REDIS_RUN_TEST(test_resp_parser_rejects_malformed_headers, "should reject malformed RESP headers and framing");
        REDIS_RUN_TEST(test_resp_array_reader_yields_fragmented_top_level_items, "should yield top-level RESP items across arbitrary fragments");
        REDIS_RUN_TEST(test_resp_bounded_append_rejects_oversized_fragment, "should reject a fragment beyond the configured buffer bound");
        REDIS_RUN_TEST(test_resp_array_reader_rejects_reply_allocation_over_budget, "should reject decoded reply allocation beyond the configured bound");
    }

    group("Stream Structure") {
        REDIS_RUN_TEST(test_stream_entry_free_null, "should free null stream entry safely");
        REDIS_RUN_TEST(test_stream_entry_free_partial_allocation, "should free partially allocated stream entries safely");
        REDIS_RUN_TEST(test_stream_entry_take_value_transfers_binary_and_empty_values, "should transfer binary and empty stream values independently");
        REDIS_RUN_TEST(test_stream_entry_take_value_rejects_invalid_or_absent_values, "should reject invalid or already transferred stream values");
        REDIS_RUN_TEST(test_stream_result_free_null, "should free null stream result safely");
    }

    group("Command API") {
        REDIS_RUN_TEST(test_command_not_connected, "should reject commands when not connected");
        REDIS_RUN_TEST(test_commandv_not_connected, "should reject argv commands when not connected");
        REDIS_RUN_TEST(test_commandv_null_client, "should reject argv commands for null client");
        REDIS_RUN_TEST(test_command_result_distinguishes_not_sent, "should report commands rejected before send");
        REDIS_RUN_TEST(test_script_results_preserve_not_sent, "should preserve scripting commands rejected before send");
        REDIS_RUN_TEST(test_server_error_classification, "should classify stable Redis server errors");
        REDIS_RUN_TEST(test_command_stream_reserves_transport_until_destroyed, "should reject a second stream and release an unsent stream cleanly");
    }

    group("Stream API") {
        REDIS_RUN_TEST(test_xadd_not_connected, "should reject xadd when not connected");
        REDIS_RUN_TEST(test_xadd_null_params, "should validate xadd parameters");
        REDIS_RUN_TEST(test_xread_null_params, "should validate xread parameters");
        REDIS_RUN_TEST(test_xreadgroup_null_params, "should validate xreadgroup parameters");
        REDIS_RUN_TEST(test_xgroup_create_null_params, "should validate xgroup create parameters");
        REDIS_RUN_TEST(test_xack_null_params, "should validate xack parameters");
        REDIS_RUN_TEST(test_xdel_null_params, "should validate xdel parameters");
    }

    group("PubSub API") {
        REDIS_RUN_TEST(test_publish_null_params, "should validate publish parameters");
        REDIS_RUN_TEST(test_subscribe_null_params, "should validate subscribe parameters");
        REDIS_RUN_TEST(test_psubscribe_null_params, "should validate psubscribe parameters");
        REDIS_RUN_TEST(test_unsubscribe_null_client, "should reject unsubscribe for null client");
        REDIS_RUN_TEST(test_punsubscribe_null_client, "should reject punsubscribe for null client");
    }

    group("Convenience Commands") {
        REDIS_RUN_TEST(test_ping_not_connected, "should reject ping when not connected");
        REDIS_RUN_TEST(test_del_not_connected, "should reject del when not connected");
        REDIS_RUN_TEST(test_exists_not_connected, "should reject exists when not connected");
        REDIS_RUN_TEST(test_expire_not_connected, "should reject expire when not connected");
        REDIS_RUN_TEST(test_incr_not_connected, "should reject incr when not connected");
        REDIS_RUN_TEST(test_lpush_not_connected, "should reject lpush when not connected");
        REDIS_RUN_TEST(test_rpush_not_connected, "should reject rpush when not connected");
        REDIS_RUN_TEST(test_lpop_not_connected, "should reject lpop when not connected");
        REDIS_RUN_TEST(test_rpop_not_connected, "should reject rpop when not connected");
        REDIS_RUN_TEST(test_hset_not_connected, "should reject hset when not connected");
        REDIS_RUN_TEST(test_hget_not_connected, "should reject hget when not connected");
        REDIS_RUN_TEST(test_sadd_not_connected, "should reject sadd when not connected");
        REDIS_RUN_TEST(test_smembers_not_connected, "should reject smembers when not connected");
        REDIS_RUN_TEST(test_xlen_not_connected, "should reject xlen when not connected");
        REDIS_RUN_TEST(test_xtrim_not_connected, "should reject xtrim when not connected");
    }

    group("Error Messages") {
        REDIS_RUN_TEST(test_get_error_null_client, "should return invalid client for null error query");
        REDIS_RUN_TEST(test_get_error_valid_client, "should return error string for valid client");
    }

    group("Extended Commands") {
        REDIS_RUN_TEST(test_ext_commands_not_connected, "should reject extended commands when not connected");
        REDIS_RUN_TEST(test_ext_commands_null_params, "should validate extended command parameters");
    }
}
