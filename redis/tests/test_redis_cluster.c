/**
 * @file test_redis_cluster.c
 * @brief Unit tests for Redis Cluster Client
 */

#include "../redis_cluster.h"
#include "CoroNet.h"
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

// =============================================================================
// Hash Slot Tests
// =============================================================================

void test_keyslot_simple(void) {
    /* Known hash slot values */
    uint16_t slot = redis_cluster_keyslot("foo", 3);
    TEST_ASSERT_LESS_THAN(REDIS_CLUSTER_SLOTS, slot);
}

void test_keyslot_consistency(void) {
    /* Same key should always return same slot */
    uint16_t slot1 = redis_cluster_keyslot("mykey", 5);
    uint16_t slot2 = redis_cluster_keyslot("mykey", 5);
    TEST_ASSERT_EQUAL(slot1, slot2);
}

void test_keyslot_hash_tag(void) {
    /* Keys with same hash tag go to same slot */
    uint16_t slot1 = redis_cluster_keyslot("user:{123}:profile", 18);
    uint16_t slot2 = redis_cluster_keyslot("user:{123}:settings", 19);
    uint16_t slot3 = redis_cluster_keyslot("user:{123}:orders", 17);

    TEST_ASSERT_EQUAL(slot1, slot2);
    TEST_ASSERT_EQUAL(slot2, slot3);
}

void test_keyslot_different_tags(void) {
    /* Different hash tags should (likely) go to different slots */
    uint16_t slot1 = redis_cluster_keyslot("user:{100}:data", 15);
    uint16_t slot2 = redis_cluster_keyslot("user:{200}:data", 15);

    /* Not guaranteed different, but very likely */
    /* Just verify they're valid slots */
    TEST_ASSERT_LESS_THAN(REDIS_CLUSTER_SLOTS, slot1);
    TEST_ASSERT_LESS_THAN(REDIS_CLUSTER_SLOTS, slot2);
}

void test_keyslot_empty_tag(void) {
    /* Empty hash tag {} should use full key */
    uint16_t slot1 = redis_cluster_keyslot("foo{}bar", 8);
    uint16_t slot2 = redis_cluster_keyslot("foo{}bar", 8);
    TEST_ASSERT_EQUAL(slot1, slot2);
}

void test_keyslot_no_closing_brace(void) {
    /* No closing brace - use full key */
    uint16_t slot1 = redis_cluster_keyslot("foo{bar", 7);
    uint16_t slot2 = redis_cluster_keyslot("foo{bar", 7);
    TEST_ASSERT_EQUAL(slot1, slot2);
}

void test_keyslot_empty_key(void) {
    uint16_t slot = redis_cluster_keyslot("", 0);
    TEST_ASSERT_LESS_THAN(REDIS_CLUSTER_SLOTS, slot);
}

// =============================================================================
// Configuration Tests
// =============================================================================

void test_cluster_create_null_config(void) {
    redis_cluster_t *cluster = redis_cluster_create(NULL);
    TEST_ASSERT_NULL(cluster);
}

void test_cluster_create_no_seeds(void) {
    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    redis_cluster_t *cluster = redis_cluster_create(&config);
    TEST_ASSERT_NULL(cluster);
}

void test_cluster_create_with_seeds(void) {
    const char *hosts[] = {"127.0.0.1", "127.0.0.2"};
    uint16_t ports[] = {7000, 7001};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 2;

    redis_cluster_t *cluster = redis_cluster_create(&config);
    TEST_ASSERT_NOT_NULL(cluster);

    redis_cluster_destroy(cluster);
}

void test_cluster_create_custom_config(void) {
    const char *hosts[] = {"redis1.example.com"};
    uint16_t ports[] = {6379};

    redis_cluster_config_t config = {
        .seed_hosts = hosts,
        .seed_ports = ports,
        .seed_count = 1,
        .username = "worker",
        .password = "secret123",
        .connections_per_node = 10,
        .connect_timeout_ms = 10000,
        .command_timeout_ms = 8000,
        .topology_refresh_ms = 60000,
        .max_redirections = 10,
        .route_reads_to_replicas = 0
    };

    redis_cluster_t *cluster = redis_cluster_create(&config);
    TEST_ASSERT_NOT_NULL(cluster);

    redis_cluster_destroy(cluster);
}

void test_cluster_rejects_username_without_password(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};
    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;
    config.username = "worker";
    TEST_ASSERT_NULL(redis_cluster_create(&config));
}

void test_cluster_rejects_missing_seed_ports(void) {
    const char *hosts[] = {"127.0.0.1"};
    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_count = 1;
    TEST_ASSERT_NULL(redis_cluster_create(&config));
}

void test_cluster_accepts_replica_routing(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};
    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;
    config.route_reads_to_replicas = 1;
    redis_cluster_t *cluster = redis_cluster_create(&config);
    TEST_ASSERT_NOT_NULL(cluster);
    redis_cluster_destroy(cluster);
}

void test_cluster_destroy_null(void) {
    /* Should not crash */
    redis_cluster_destroy(NULL);
    TEST_PASS();
}

// =============================================================================
// Lifecycle Tests
// =============================================================================

void test_cluster_connect_null(void) {
    int result = redis_cluster_connect(NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_cluster_disconnect_null(void) {
    /* Should not crash */
    redis_cluster_disconnect(NULL);
    TEST_PASS();
}

void test_cluster_refresh_null(void) {
    int result = redis_cluster_refresh(NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_cluster_connect_disconnect(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);
    TEST_ASSERT_NOT_NULL(cluster);

    /* Connect may fail without actual Redis, but shouldn't crash */
    redis_cluster_connect(cluster);
    redis_cluster_disconnect(cluster);
    redis_cluster_disconnect(cluster);  /* Double disconnect */

    redis_cluster_destroy(cluster);
}

// =============================================================================
// Command Tests (without connection)
// =============================================================================

void test_cluster_command_null(void) {
    int result = redis_cluster_command(NULL, NULL, NULL, "PING");
    TEST_ASSERT_EQUAL(-1, result);
}

void test_cluster_command_null_format(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    int result = redis_cluster_command(cluster, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_cluster_destroy(cluster);
}

void test_cluster_command_key_null(void) {
    int result = redis_cluster_command_key(NULL, "key", NULL, NULL, "GET key");
    TEST_ASSERT_EQUAL(-1, result);
}

void test_cluster_commandv_null(void) {
    const char *argv[] = {"SET", "key", "value"};
    int result = redis_cluster_commandv(NULL, 3, argv, NULL, -1, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_cluster_commandv_null_argv(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    int result = redis_cluster_commandv(cluster, 3, NULL, NULL, -1, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_cluster_destroy(cluster);
}

void test_cluster_commandv_result_not_connected(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};
    const char *argv[] = {"GET", "key"};
    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;
    redis_cluster_t *cluster = redis_cluster_create(&config);
    TEST_ASSERT_NOT_NULL(cluster);
    TEST_ASSERT_EQUAL(TURBO_ENOTCONN,
                      redis_cluster_commandv_result(cluster, 2, argv, NULL, 1,
                                                    &result));
    TEST_ASSERT_EQUAL(REDIS_COMMAND_NOT_SENT, result.outcome);
    redis_command_result_clear(&result);
    redis_cluster_destroy(cluster);
}

// =============================================================================
// Convenience Function Tests
// =============================================================================

void test_cluster_set_not_connected(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    int result = redis_cluster_set(cluster, "key", "value", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_cluster_destroy(cluster);
}

void test_cluster_get_not_connected(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    int result = redis_cluster_get(cluster, "key", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_cluster_destroy(cluster);
}

void test_cluster_del_not_connected(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    int result = redis_cluster_del(cluster, "key", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_cluster_destroy(cluster);
}

void test_cluster_hset_not_connected(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    int result = redis_cluster_hset(cluster, "hash", "field", "value", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_cluster_destroy(cluster);
}

void test_cluster_xadd_null(void) {
    const char *fields[] = {"f1"};
    const char *values[] = {"v1"};

    int result = redis_cluster_xadd(NULL, "stream", 1000, 1, fields, values, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_cluster_xread_null(void) {
    int result = redis_cluster_xread(NULL, "stream", 10, 1000, "0", NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

// =============================================================================
// Multi-Key Tests
// =============================================================================

void test_cluster_mdelete_null(void) {
    const char *keys[] = {"key1", "key2"};
    int result = redis_cluster_mdelete(NULL, 2, keys, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_cluster_mdelete_no_keys(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    int result = redis_cluster_mdelete(cluster, 0, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);

    redis_cluster_destroy(cluster);
}

void test_cluster_mget_null(void) {
    const char *keys[] = {"key1", "key2"};
    int result = redis_cluster_mget(NULL, 2, keys, NULL, NULL);
    TEST_ASSERT_EQUAL(-1, result);
}

void test_cluster_mget_hash_tags(void) {
    /* Keys with same hash tag should be in same slot */
    const char *keys[] = {"{user:1}:name", "{user:1}:email", "{user:1}:age"};

    uint16_t slot0 = redis_cluster_keyslot(keys[0], strlen(keys[0]));
    uint16_t slot1 = redis_cluster_keyslot(keys[1], strlen(keys[1]));
    uint16_t slot2 = redis_cluster_keyslot(keys[2], strlen(keys[2]));

    TEST_ASSERT_EQUAL(slot0, slot1);
    TEST_ASSERT_EQUAL(slot1, slot2);
}

// =============================================================================
// Statistics Tests
// =============================================================================

void test_cluster_get_stats_null(void) {
    redis_cluster_stats_t stats = {0};

    /* Should not crash */
    redis_cluster_get_stats(NULL, &stats);
    redis_cluster_get_stats(NULL, NULL);

    TEST_PASS();
}

void test_cluster_get_stats(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    redis_cluster_stats_t stats;
    redis_cluster_get_stats(cluster, &stats);

    TEST_ASSERT_EQUAL(0, stats.commands_sent);
    TEST_ASSERT_EQUAL(0, stats.commands_failed);
    TEST_ASSERT_EQUAL(0, stats.redirections);

    redis_cluster_destroy(cluster);
}

void test_cluster_reset_stats_null(void) {
    /* Should not crash */
    redis_cluster_reset_stats(NULL);
    TEST_PASS();
}

void test_cluster_reset_stats(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    redis_cluster_reset_stats(cluster);

    redis_cluster_stats_t stats;
    redis_cluster_get_stats(cluster, &stats);
    TEST_ASSERT_EQUAL(0, stats.commands_sent);

    redis_cluster_destroy(cluster);
}

// =============================================================================
// Health Tests
// =============================================================================

void test_cluster_is_healthy_null(void) {
    int healthy = redis_cluster_is_healthy(NULL);
    TEST_ASSERT_EQUAL(0, healthy);
}

void test_cluster_is_healthy_not_connected(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    int healthy = redis_cluster_is_healthy(cluster);
    TEST_ASSERT_EQUAL(0, healthy);

    redis_cluster_destroy(cluster);
}

void test_cluster_node_count_null(void) {
    size_t masters = 99, replicas = 99;
    redis_cluster_node_count(NULL, &masters, &replicas);
    TEST_ASSERT_EQUAL(0, masters);
    TEST_ASSERT_EQUAL(0, replicas);
}

void test_cluster_node_count(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    size_t masters, replicas;
    redis_cluster_node_count(cluster, &masters, &replicas);

    /* Before connect, no nodes discovered */
    TEST_ASSERT_EQUAL(0, masters);
    TEST_ASSERT_EQUAL(0, replicas);

    redis_cluster_destroy(cluster);
}

void test_cluster_get_node_null(void) {
    const redis_cluster_node_t *node = redis_cluster_get_node(NULL, 0);
    TEST_ASSERT_NULL(node);
}

void test_cluster_get_node_invalid_slot(void) {
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {7000};

    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;

    redis_cluster_t *cluster = redis_cluster_create(&config);

    const redis_cluster_node_t *node = redis_cluster_get_node(cluster, REDIS_CLUSTER_SLOTS + 1);
    TEST_ASSERT_NULL(node);

    redis_cluster_destroy(cluster);
}

// =============================================================================
// Cluster Protocol Contract Tests
// =============================================================================

#define CLUSTER_TEST_PRIMARY_PORT 19870
#define CLUSTER_TEST_TARGET_PORT 19871
#define CLUSTER_TEST_REPLICA_PORT 19872
#define CLUSTER_TEST_BUFFER_SIZE 4096

typedef struct {
    coro_context_t *ctx;
    coro_socket_t *primary_server;
    coro_socket_t *target_server;
    int status;
    const char *stage;
    int legacy_slots_only;
    int topology_requests;
    int shards_requests;
    int slots_requests;
    int moved_replies;
    int ask_replies;
    int asking_commands;
    int final_replies;
} cluster_protocol_case_t;

typedef struct {
    int called;
    int client_present;
    char value[16];
} cluster_callback_case_t;

static int find_bytes(const char *data, size_t len, const char *needle) {
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > len) return 0;
    for (size_t i = 0; i <= len - needle_len; ++i)
        if (memcmp(data + i, needle, needle_len) == 0) return 1;
    return 0;
}

static int parse_decimal_line(const char *data, size_t len, size_t *offset,
                              long *value) {
    char number[32];
    size_t start = *offset;
    size_t digits;
    char *endptr;
    while (*offset + 1 < len &&
           !(data[*offset] == '\r' && data[*offset + 1] == '\n'))
        (*offset)++;
    if (*offset + 1 >= len) return 0;
    digits = *offset - start;
    if (digits == 0 || digits >= sizeof(number)) return -1;
    memcpy(number, data + start, digits);
    number[digits] = '\0';
    *value = strtol(number, &endptr, 10);
    if (*endptr != '\0') return -1;
    *offset += 2;
    return 1;
}

static int resp_command_size(const char *data, size_t len, size_t *frame_len) {
    size_t offset = 1;
    long argc;
    if (!data || !frame_len || len == 0) return 0;
    if (data[0] != '*') return -1;
    int parsed = parse_decimal_line(data, len, &offset, &argc);
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
        if (offset + 1 >= len) return 0;
        if (data[offset] != '\r' || data[offset + 1] != '\n') return -1;
        offset += 2;
    }
    *frame_len = offset;
    return 1;
}

static int send_cluster_slots(coro_socket_t *client,
                              cluster_protocol_case_t *test_case) {
    static const char node_id[] = "0123456789abcdef0123456789abcdef01234567";
    char reply[256];
    int len = snprintf(reply, sizeof(reply),
                       "*1\r\n*3\r\n:0\r\n:16383\r\n*3\r\n$9\r\n"
                       "127.0.0.1\r\n:%d\r\n$40\r\n%s\r\n",
                       CLUSTER_TEST_PRIMARY_PORT, node_id);
    if (len <= 0 || (size_t)len >= sizeof(reply)) return TURBO_EPROTO;
    test_case->topology_requests++;
    return coro_socket_send(client, reply, (size_t)len);
}

static int send_cluster_shards(coro_socket_t *client,
                               cluster_protocol_case_t *test_case) {
    static const char node_id[] = "0123456789abcdef0123456789abcdef01234567";
    char reply[512];
    int len = snprintf(reply, sizeof(reply),
                       "*1\r\n*4\r\n$5\r\nslots\r\n*2\r\n:0\r\n:16383\r\n"
                       "$5\r\nnodes\r\n*1\r\n*10\r\n$2\r\nid\r\n$40\r\n%s\r\n"
                       "$8\r\nendpoint\r\n$9\r\n127.0.0.1\r\n"
                       "$4\r\nport\r\n:%d\r\n$4\r\nrole\r\n$6\r\nmaster\r\n"
                       "$6\r\nhealth\r\n$6\r\nonline\r\n",
                       node_id, CLUSTER_TEST_PRIMARY_PORT);
    if (len <= 0 || (size_t)len >= sizeof(reply)) return TURBO_EPROTO;
    test_case->topology_requests++;
    return coro_socket_send(client, reply, (size_t)len);
}

static int handle_cluster_command(coro_socket_t *client,
                                  cluster_protocol_case_t *test_case,
                                  int target, int *asking,
                                  const char *frame, size_t frame_len) {
    char reply[128];
    int len;
    if (find_bytes(frame, frame_len, "CLUSTER") &&
        find_bytes(frame, frame_len, "SHARDS")) {
        test_case->shards_requests++;
        if (test_case->legacy_slots_only)
            return coro_socket_send(client,
                                    "-ERR Unknown subcommand or wrong number of arguments for 'SHARDS'. Try CLUSTER HELP.\r\n",
                                    sizeof("-ERR Unknown subcommand or wrong number of arguments for 'SHARDS'. Try CLUSTER HELP.\r\n") - 1u);
        return send_cluster_shards(client, test_case);
    }
    if (find_bytes(frame, frame_len, "CLUSTER") &&
        find_bytes(frame, frame_len, "SLOTS")) {
        test_case->slots_requests++;
        return send_cluster_slots(client, test_case);
    }

    if (target && find_bytes(frame, frame_len, "ASKING")) {
        *asking = 1;
        test_case->asking_commands++;
        return coro_socket_send(client, "+OK\r\n", 5);
    }
    if (find_bytes(frame, frame_len, "moved-key")) {
        if (target) {
            test_case->final_replies++;
            return coro_socket_send(client, "$5\r\nmoved\r\n", 11);
        }
        len = snprintf(reply, sizeof(reply), "-MOVED %u 127.0.0.1:%d\r\n",
                       redis_cluster_keyslot("moved-key", 9),
                       CLUSTER_TEST_TARGET_PORT);
        test_case->moved_replies++;
        return coro_socket_send(client, reply, (size_t)len);
    }
    if (find_bytes(frame, frame_len, "ask-key")) {
        if (target && *asking) {
            *asking = 0;
            test_case->final_replies++;
            return coro_socket_send(client, "$3\r\nask\r\n", 9);
        }
        len = snprintf(reply, sizeof(reply), "-ASK %u 127.0.0.1:%d\r\n",
                       redis_cluster_keyslot("ask-key", 7),
                       CLUSTER_TEST_TARGET_PORT);
        test_case->ask_replies++;
        return coro_socket_send(client, reply, (size_t)len);
    }
    return coro_socket_send(client, "+OK\r\n", 5);
}

static void fake_cluster_connection(coro_socket_t *client, void *arg,
                                    int target) {
    cluster_protocol_case_t *test_case = (cluster_protocol_case_t *)arg;
    char buffer[CLUSTER_TEST_BUFFER_SIZE];
    size_t used = 0;
    int asking = 0;
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
            if (parsed < 0 ||
                (parsed > 0 && handle_cluster_command(client, test_case, target,
                                                       &asking, buffer,
                                                       frame_len) != TURBO_OK))
                return;
            if (parsed == 0) break;
            memmove(buffer, buffer + frame_len, used - frame_len);
            used -= frame_len;
        }
    }
}

static void fake_cluster_primary(coro_socket_t *client, void *arg) {
    fake_cluster_connection(client, arg, 0);
}

static void fake_cluster_target(coro_socket_t *client, void *arg) {
    fake_cluster_connection(client, arg, 1);
}

static int reply_is_bulk(const redis_command_result_t *result,
                         const char *value) {
    size_t len = strlen(value);
    return result && result->status == TURBO_OK && result->reply &&
           result->reply->type == REDIS_REPLY_BULK_STRING &&
           result->reply->str && result->reply->len == len &&
           memcmp(result->reply->str, value, len) == 0;
}

static void capture_cluster_reply(redis_client_t *client, redis_reply_t *reply,
                                  void *user_data) {
    cluster_callback_case_t *callback = (cluster_callback_case_t *)user_data;
    callback->called++;
    callback->client_present = client != NULL;
    if (reply && reply->type == REDIS_REPLY_BULK_STRING && reply->str &&
        reply->len < sizeof(callback->value)) {
        memcpy(callback->value, reply->str, reply->len);
        callback->value[reply->len] = '\0';
    }
}

static void run_cluster_protocol_contract(coro_t *co, void *arg) {
    cluster_protocol_case_t *test_case = (cluster_protocol_case_t *)arg;
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {CLUSTER_TEST_PRIMARY_PORT};
    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    redis_cluster_t *cluster = NULL;
    redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
    cluster_callback_case_t callback = {0};
    const redis_cluster_node_t *node;
    const char *moved_argv[] = {"GET", "moved-key"};
    redis_cluster_stats_t stats;
    uint16_t moved_slot = redis_cluster_keyslot("moved-key", 9);
    uint16_t ask_slot = redis_cluster_keyslot("ask-key", 7);

    (void)co;
    test_case->status = TURBO_EIO;
    test_case->stage = "create primary server";
    test_case->primary_server = coro_socket_create(test_case->ctx,
                                                    CORO_SOCKET_TCP_V4);
    if (!test_case->primary_server) goto cleanup;
    if (coro_socket_listen_on(test_case->primary_server, "127.0.0.1",
                              CLUSTER_TEST_PRIMARY_PORT, fake_cluster_primary,
                              test_case) != TURBO_OK)
        goto cleanup;

    test_case->stage = "create target server";
    test_case->target_server = coro_socket_create(test_case->ctx,
                                                   CORO_SOCKET_TCP_V4);
    if (!test_case->target_server) goto cleanup;
    if (coro_socket_listen_on(test_case->target_server, "127.0.0.1",
                              CLUSTER_TEST_TARGET_PORT, fake_cluster_target,
                              test_case) != TURBO_OK)
        goto cleanup;
    coro_yield();

    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;
    config.connections_per_node = 1;
    config.connect_timeout_ms = 2000;
    config.command_timeout_ms = 2000;
    config.topology_refresh_ms = 60000;
    test_case->stage = "connect and discover topology";
    cluster = redis_cluster_create(&config);
    if (!cluster || redis_cluster_connect(cluster) != TURBO_OK ||
        !redis_cluster_is_healthy(cluster))
        goto cleanup;

    test_case->stage = "follow MOVED";
    if (redis_cluster_commandv_result(cluster, 2, moved_argv, NULL, 1,
                                      &result) != TURBO_OK ||
        !reply_is_bulk(&result, "moved"))
        goto cleanup;
    redis_command_result_clear(&result);
    node = redis_cluster_get_node(cluster, moved_slot);
    if (!node || node->port != CLUSTER_TEST_TARGET_PORT) goto cleanup;

    test_case->stage = "refresh complete topology";
    if (redis_cluster_refresh(cluster) != TURBO_OK) goto cleanup;
    node = redis_cluster_get_node(cluster, moved_slot);
    if (!node || node->port != CLUSTER_TEST_PRIMARY_PORT) goto cleanup;

    test_case->stage = "follow ASK on one connection";
    if (redis_cluster_get(cluster, "ask-key", capture_cluster_reply,
                          &callback) != TURBO_OK || callback.called != 1 ||
        !callback.client_present || strcmp(callback.value, "ask") != 0)
        goto cleanup;
    node = redis_cluster_get_node(cluster, ask_slot);
    if (!node || node->port != CLUSTER_TEST_PRIMARY_PORT) goto cleanup;

    redis_cluster_get_stats(cluster, &stats);
    if (stats.redirections != 2 || stats.commands_sent != 2 ||
        stats.topology_refreshes != 2)
        goto cleanup;

    test_case->stage = "complete";
    test_case->status = TURBO_OK;

cleanup:
    redis_command_result_clear(&result);
    redis_cluster_destroy(cluster);
    coro_socket_destroy(test_case->target_server);
    test_case->target_server = NULL;
    coro_socket_destroy(test_case->primary_server);
    test_case->primary_server = NULL;
}

void test_cluster_discovers_and_follows_redirections(void) {
    cluster_protocol_case_t test_case = {0};
    test_case.stage = "create context";
    test_case.ctx = coro_context_create(NULL);
    TEST_ASSERT_NOT_NULL(test_case.ctx);
    TEST_ASSERT_EQUAL(TURBO_OK,
                      coro_context_spawn(test_case.ctx,
                                         run_cluster_protocol_contract,
                                         &test_case));
    coro_context_run(test_case.ctx, TURBO_RUN_DEFAULT);
    coro_context_destroy(test_case.ctx);
    if (test_case.status != TURBO_OK)
        fprintf(stderr, "cluster protocol contract failed at stage: %s\n",
                test_case.stage);
    TEST_ASSERT_EQUAL(TURBO_OK, test_case.status);
    TEST_ASSERT_EQUAL(2, test_case.topology_requests);
    TEST_ASSERT_EQUAL(2, test_case.shards_requests);
    TEST_ASSERT_EQUAL(0, test_case.slots_requests);
    TEST_ASSERT_EQUAL(1, test_case.moved_replies);
    TEST_ASSERT_EQUAL(1, test_case.ask_replies);
    TEST_ASSERT_EQUAL(1, test_case.asking_commands);
    TEST_ASSERT_EQUAL(2, test_case.final_replies);
}

void test_cluster_falls_back_for_legacy_redis(void) {
    cluster_protocol_case_t test_case = {0};
    test_case.legacy_slots_only = 1;
    test_case.stage = "create context";
    test_case.ctx = coro_context_create(NULL);
    TEST_ASSERT_NOT_NULL(test_case.ctx);
    TEST_ASSERT_EQUAL(TURBO_OK,
                      coro_context_spawn(test_case.ctx,
                                         run_cluster_protocol_contract,
                                         &test_case));
    coro_context_run(test_case.ctx, TURBO_RUN_DEFAULT);
    coro_context_destroy(test_case.ctx);
    if (test_case.status != TURBO_OK)
        fprintf(stderr, "legacy cluster contract failed at stage: %s\n",
                test_case.stage);
    TEST_ASSERT_EQUAL(TURBO_OK, test_case.status);
    TEST_ASSERT_EQUAL(2, test_case.topology_requests);
    TEST_ASSERT_EQUAL(2, test_case.shards_requests);
    TEST_ASSERT_EQUAL(2, test_case.slots_requests);
}

typedef struct {
    coro_context_t *ctx;
    coro_socket_t *primary_server;
    coro_socket_t *replica_server;
    int status;
    const char *stage;
    int reject_readonly;
    int readonly_commands;
    int primary_writes;
    int primary_reads;
    int replica_reads;
} cluster_replica_case_t;

static int send_cluster_shards_with_replica(coro_socket_t *client) {
    static const char master_id[] = "0123456789abcdef0123456789abcdef01234567";
    static const char replica_id[] = "89abcdef0123456789abcdef0123456789abcdef";
    char reply[1024];
    int len = snprintf(reply, sizeof(reply),
                       "*1\r\n*4\r\n$5\r\nslots\r\n*2\r\n:0\r\n:16383\r\n"
                       "$5\r\nnodes\r\n*2\r\n"
                       "*10\r\n$2\r\nid\r\n$40\r\n%s\r\n$8\r\nendpoint\r\n"
                       "$9\r\n127.0.0.1\r\n$4\r\nport\r\n:%d\r\n$4\r\nrole\r\n"
                       "$6\r\nmaster\r\n$6\r\nhealth\r\n$6\r\nonline\r\n"
                       "*10\r\n$2\r\nid\r\n$40\r\n%s\r\n$8\r\nendpoint\r\n"
                       "$9\r\n127.0.0.1\r\n$4\r\nport\r\n:%d\r\n$4\r\nrole\r\n"
                       "$7\r\nreplica\r\n$6\r\nhealth\r\n$6\r\nonline\r\n",
                       master_id, CLUSTER_TEST_PRIMARY_PORT, replica_id,
                       CLUSTER_TEST_REPLICA_PORT);
    if (len <= 0 || (size_t)len >= sizeof(reply)) return TURBO_EPROTO;
    return coro_socket_send(client, reply, (size_t)len);
}

static int handle_replica_route_command(coro_socket_t *client,
                                        cluster_replica_case_t *test_case,
                                        int replica, int *readonly,
                                        const char *frame,
                                        size_t frame_len) {
    if (!replica && find_bytes(frame, frame_len, "CLUSTER") &&
        find_bytes(frame, frame_len, "SHARDS"))
        return send_cluster_shards_with_replica(client);
    if (replica && find_bytes(frame, frame_len, "READONLY")) {
        *readonly = 1;
        test_case->readonly_commands++;
        if (test_case->reject_readonly)
            return coro_socket_send(client, "-ERR readonly denied\r\n",
                                    sizeof("-ERR readonly denied\r\n") - 1u);
        return coro_socket_send(client, "+OK\r\n", 5);
    }
    if (find_bytes(frame, frame_len, "SET")) {
        if (replica) return coro_socket_send(client, "-READONLY replica\r\n", 19);
        test_case->primary_writes++;
        return coro_socket_send(client, "+OK\r\n", 5);
    }
    if (find_bytes(frame, frame_len, "GET")) {
        if (replica) {
            if (!*readonly)
                return coro_socket_send(client,
                                        "-MOVED 0 127.0.0.1:19870\r\n",
                                        sizeof("-MOVED 0 127.0.0.1:19870\r\n") - 1u);
            test_case->replica_reads++;
            return coro_socket_send(client, "$7\r\nreplica\r\n", 13);
        }
        test_case->primary_reads++;
        return coro_socket_send(client, "$7\r\nprimary\r\n", 13);
    }
    return coro_socket_send(client, "+OK\r\n", 5);
}

static void fake_replica_route_connection(coro_socket_t *client, void *arg,
                                          int replica) {
    cluster_replica_case_t *test_case = (cluster_replica_case_t *)arg;
    char buffer[CLUSTER_TEST_BUFFER_SIZE];
    size_t used = 0;
    int readonly = 0;
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
            if (parsed < 0 ||
                (parsed > 0 &&
                 handle_replica_route_command(client, test_case, replica,
                                              &readonly, buffer, frame_len) != TURBO_OK))
                return;
            if (parsed == 0) break;
            memmove(buffer, buffer + frame_len, used - frame_len);
            used -= frame_len;
        }
    }
}

static void fake_replica_route_primary(coro_socket_t *client, void *arg) {
    fake_replica_route_connection(client, arg, 0);
}

static void fake_replica_route_replica(coro_socket_t *client, void *arg) {
    fake_replica_route_connection(client, arg, 1);
}

static void run_cluster_replica_route_contract(coro_t *co, void *arg) {
    cluster_replica_case_t *test_case = (cluster_replica_case_t *)arg;
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {CLUSTER_TEST_PRIMARY_PORT};
    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    redis_cluster_t *cluster = NULL;
    redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
    cluster_callback_case_t callback = {0};
    const char *get_argv[] = {"GET", "route-key"};
    redis_cluster_stats_t stats;
    (void)co;

    test_case->status = TURBO_EIO;
    test_case->stage = "listen primary";
    test_case->primary_server = coro_socket_create(test_case->ctx, CORO_SOCKET_TCP_V4);
    if (!test_case->primary_server ||
        coro_socket_listen_on(test_case->primary_server, "127.0.0.1",
                              CLUSTER_TEST_PRIMARY_PORT,
                              fake_replica_route_primary, test_case) != TURBO_OK)
        goto cleanup;
    test_case->stage = "listen replica";
    test_case->replica_server = coro_socket_create(test_case->ctx, CORO_SOCKET_TCP_V4);
    if (!test_case->replica_server ||
        coro_socket_listen_on(test_case->replica_server, "127.0.0.1",
                              CLUSTER_TEST_REPLICA_PORT,
                              fake_replica_route_replica, test_case) != TURBO_OK)
        goto cleanup;
    coro_yield();

    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;
    config.connections_per_node = 1;
    config.connect_timeout_ms = 2000;
    config.command_timeout_ms = 2000;
    config.topology_refresh_ms = 60000;
    config.route_reads_to_replicas = 1;
    test_case->stage = "connect replica topology";
    cluster = redis_cluster_create(&config);
    if (!cluster) goto cleanup;
    if (test_case->reject_readonly) {
        if (redis_cluster_connect(cluster) == TURBO_OK ||
            redis_cluster_is_healthy(cluster))
            goto cleanup;
        test_case->stage = "complete";
        test_case->status = TURBO_OK;
        goto cleanup;
    }
    if (redis_cluster_connect(cluster) != TURBO_OK) goto cleanup;

    test_case->stage = "write to primary";
    if (redis_cluster_set(cluster, "route-key", "value", NULL, NULL) != TURBO_OK)
        goto cleanup;
    test_case->stage = "convenience read from replica";
    if (redis_cluster_get(cluster, "route-key", capture_cluster_reply,
                          &callback) != TURBO_OK || callback.called != 1 ||
        strcmp(callback.value, "replica") != 0)
        goto cleanup;
    test_case->stage = "explicit read from replica";
    if (redis_cluster_read_commandv_result(cluster, 2, get_argv, NULL, 1,
                                            &result) != TURBO_OK ||
        !reply_is_bulk(&result, "replica"))
        goto cleanup;
    redis_command_result_clear(&result);
    test_case->stage = "generic command remains authoritative";
    if (redis_cluster_commandv_result(cluster, 2, get_argv, NULL, 1,
                                      &result) != TURBO_OK ||
        !reply_is_bulk(&result, "primary"))
        goto cleanup;
    redis_command_result_clear(&result);
    redis_cluster_get_stats(cluster, &stats);
    if (stats.master_count != 1 || stats.replica_count != 1) goto cleanup;

    test_case->stage = "complete";
    test_case->status = TURBO_OK;

cleanup:
    redis_command_result_clear(&result);
    redis_cluster_destroy(cluster);
    coro_socket_destroy(test_case->replica_server);
    test_case->replica_server = NULL;
    coro_socket_destroy(test_case->primary_server);
    test_case->primary_server = NULL;
}

void test_cluster_routes_declared_reads_to_replica(void) {
    cluster_replica_case_t test_case = {0};
    test_case.stage = "create context";
    test_case.ctx = coro_context_create(NULL);
    TEST_ASSERT_NOT_NULL(test_case.ctx);
    TEST_ASSERT_EQUAL(TURBO_OK,
                      coro_context_spawn(test_case.ctx,
                                         run_cluster_replica_route_contract,
                                         &test_case));
    coro_context_run(test_case.ctx, TURBO_RUN_DEFAULT);
    coro_context_destroy(test_case.ctx);
    if (test_case.status != TURBO_OK)
        fprintf(stderr, "replica routing contract failed at stage: %s\n",
                test_case.stage);
    TEST_ASSERT_EQUAL(TURBO_OK, test_case.status);
    TEST_ASSERT_EQUAL(1, test_case.readonly_commands);
    TEST_ASSERT_EQUAL(1, test_case.primary_writes);
    TEST_ASSERT_EQUAL(1, test_case.primary_reads);
    TEST_ASSERT_EQUAL(2, test_case.replica_reads);
}

void test_cluster_rejects_replica_when_readonly_fails(void) {
    cluster_replica_case_t test_case = {0};
    test_case.reject_readonly = 1;
    test_case.stage = "create context";
    test_case.ctx = coro_context_create(NULL);
    TEST_ASSERT_NOT_NULL(test_case.ctx);
    TEST_ASSERT_EQUAL(TURBO_OK,
                      coro_context_spawn(test_case.ctx,
                                         run_cluster_replica_route_contract,
                                         &test_case));
    coro_context_run(test_case.ctx, TURBO_RUN_DEFAULT);
    coro_context_destroy(test_case.ctx);
    if (test_case.status != TURBO_OK)
        fprintf(stderr, "READONLY failure contract failed at stage: %s\n",
                test_case.stage);
    TEST_ASSERT_EQUAL(TURBO_OK, test_case.status);
    TEST_ASSERT_EQUAL(1, test_case.readonly_commands);
    TEST_ASSERT_EQUAL(0, test_case.primary_writes);
    TEST_ASSERT_EQUAL(0, test_case.replica_reads);
}

static void run_cluster_requires_replica(coro_t *co, void *arg) {
    cluster_protocol_case_t *test_case = (cluster_protocol_case_t *)arg;
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {CLUSTER_TEST_PRIMARY_PORT};
    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    redis_cluster_t *cluster = NULL;
    (void)co;

    test_case->status = TURBO_EIO;
    test_case->stage = "listen primary without replica";
    test_case->primary_server = coro_socket_create(test_case->ctx, CORO_SOCKET_TCP_V4);
    if (!test_case->primary_server ||
        coro_socket_listen_on(test_case->primary_server, "127.0.0.1",
                              CLUSTER_TEST_PRIMARY_PORT, fake_cluster_primary,
                              test_case) != TURBO_OK)
        goto cleanup;
    coro_yield();
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;
    config.connections_per_node = 1;
    config.connect_timeout_ms = 2000;
    config.command_timeout_ms = 2000;
    config.route_reads_to_replicas = 1;
    cluster = redis_cluster_create(&config);
    test_case->stage = "reject incomplete replica topology";
    if (!cluster || redis_cluster_connect(cluster) == TURBO_OK ||
        redis_cluster_is_healthy(cluster))
        goto cleanup;
    test_case->stage = "complete";
    test_case->status = TURBO_OK;

cleanup:
    redis_cluster_destroy(cluster);
    coro_socket_destroy(test_case->primary_server);
    test_case->primary_server = NULL;
}

void test_cluster_replica_routing_requires_complete_topology(void) {
    cluster_protocol_case_t test_case = {0};
    test_case.stage = "create context";
    test_case.ctx = coro_context_create(NULL);
    TEST_ASSERT_NOT_NULL(test_case.ctx);
    TEST_ASSERT_EQUAL(TURBO_OK,
                      coro_context_spawn(test_case.ctx,
                                         run_cluster_requires_replica,
                                         &test_case));
    coro_context_run(test_case.ctx, TURBO_RUN_DEFAULT);
    coro_context_destroy(test_case.ctx);
    if (test_case.status != TURBO_OK)
        fprintf(stderr, "replica completeness contract failed at stage: %s\n",
                test_case.stage);
    TEST_ASSERT_EQUAL(TURBO_OK, test_case.status);
    TEST_ASSERT_EQUAL(1, test_case.shards_requests);
}

typedef struct {
    int status;
} cluster_standalone_case_t;

static void run_cluster_rejects_standalone(coro_t *co, void *arg) {
    cluster_standalone_case_t *test_case = (cluster_standalone_case_t *)arg;
    const char *hosts[] = {"127.0.0.1"};
    uint16_t ports[] = {6379};
    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    redis_cluster_t *cluster;
    (void)co;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;
    config.connect_timeout_ms = 2000;
    config.command_timeout_ms = 2000;
    cluster = redis_cluster_create(&config);
    test_case->status = cluster && redis_cluster_connect(cluster) != TURBO_OK &&
                        !redis_cluster_is_healthy(cluster)
                            ? TURBO_OK
                            : TURBO_EPROTO;
    redis_cluster_destroy(cluster);
}

void test_cluster_live_rejects_standalone_redis(void) {
    const char *enabled = getenv("TURBONET_REDIS_LIVE");
    cluster_standalone_case_t test_case = {TURBO_EIO};
    coro_context_t *ctx;
    if (!enabled || strcmp(enabled, "1") != 0) TEST_PASS();
    ctx = coro_context_create(NULL);
    TEST_ASSERT_NOT_NULL(ctx);
    TEST_ASSERT_EQUAL(TURBO_OK,
                      coro_context_spawn(ctx, run_cluster_rejects_standalone,
                                         &test_case));
    coro_context_run(ctx, TURBO_RUN_DEFAULT);
    coro_context_destroy(ctx);
    TEST_ASSERT_EQUAL(TURBO_OK, test_case.status);
}

typedef struct {
    int status;
    const char *stage;
} cluster_live_case_t;

static void run_cluster_live_contract(coro_t *co, void *arg) {
    cluster_live_case_t *test_case = (cluster_live_case_t *)arg;
    const char *host = getenv("TURBONET_REDIS_CLUSTER_HOST");
    const char *port_text = getenv("TURBONET_REDIS_CLUSTER_PORT");
    const char *hosts[1];
    uint16_t ports[1];
    redis_cluster_config_t config = REDIS_CLUSTER_CONFIG_DEFAULT;
    redis_cluster_t *cluster = NULL;
    redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
    const char *set_argv[] = {"SET", "turbonet:cluster:contract", "ready"};
    const char *get_argv[] = {"GET", "turbonet:cluster:contract"};
    const char *exists_argv[] = {"EXISTS", "turbonet:cluster:contract"};
    const char *del_argv[] = {"DEL", "turbonet:cluster:contract"};
    long port = port_text ? strtol(port_text, NULL, 10) : 17000;
    (void)co;

    test_case->status = TURBO_EIO;
    test_case->stage = "validate endpoint";
    if (port <= 0 || port > UINT16_MAX) goto cleanup;
    hosts[0] = host && host[0] ? host : "127.0.0.1";
    ports[0] = (uint16_t)port;
    config.seed_hosts = hosts;
    config.seed_ports = ports;
    config.seed_count = 1;
    config.connections_per_node = 1;
    config.connect_timeout_ms = 3000;
    config.command_timeout_ms = 3000;
    config.topology_refresh_ms = 60000;
    config.route_reads_to_replicas = 1;
    test_case->stage = "connect real cluster";
    cluster = redis_cluster_create(&config);
    if (!cluster || redis_cluster_connect(cluster) != TURBO_OK ||
        !redis_cluster_is_healthy(cluster))
        goto cleanup;

    test_case->stage = "write master";
    if (redis_cluster_commandv_result(cluster, 3, set_argv, NULL, 1,
                                      &result) != TURBO_OK)
        goto cleanup;
    redis_command_result_clear(&result);
    test_case->stage = "read authoritative master";
    if (redis_cluster_commandv_result(cluster, 2, get_argv, NULL, 1,
                                      &result) != TURBO_OK ||
        !reply_is_bulk(&result, "ready"))
        goto cleanup;
    redis_command_result_clear(&result);
    test_case->stage = "read replica";
    if (redis_cluster_read_commandv_result(cluster, 2, exists_argv, NULL, 1,
                                           &result) != TURBO_OK ||
        !result.reply || result.reply->type != REDIS_REPLY_INTEGER)
        goto cleanup;
    redis_command_result_clear(&result);
    test_case->stage = "cleanup key";
    if (redis_cluster_commandv_result(cluster, 2, del_argv, NULL, 1,
                                      &result) != TURBO_OK)
        goto cleanup;
    redis_command_result_clear(&result);
    test_case->stage = "complete";
    test_case->status = TURBO_OK;

cleanup:
    redis_command_result_clear(&result);
    redis_cluster_destroy(cluster);
}

void test_cluster_live_multi_node_contract(void) {
    const char *enabled = getenv("TURBONET_REDIS_CLUSTER_LIVE");
    cluster_live_case_t test_case = {TURBO_EIO, "create context"};
    coro_context_t *ctx;
    if (!enabled || strcmp(enabled, "1") != 0) TEST_PASS();
    ctx = coro_context_create(NULL);
    TEST_ASSERT_NOT_NULL(ctx);
    TEST_ASSERT_EQUAL(TURBO_OK,
                      coro_context_spawn(ctx, run_cluster_live_contract,
                                         &test_case));
    coro_context_run(ctx, TURBO_RUN_DEFAULT);
    coro_context_destroy(ctx);
    if (test_case.status != TURBO_OK)
        fprintf(stderr, "live cluster contract failed at stage: %s\n",
                test_case.stage);
    TEST_ASSERT_EQUAL(TURBO_OK, test_case.status);
}

suite("redis_cluster") {
    before_each() {
        setUp();
    }

    after_each() {
        tearDown();
    }

    group("Hash Slots") {
        REDIS_RUN_TEST(test_keyslot_simple, "should hash simple keys");
        REDIS_RUN_TEST(test_keyslot_consistency, "should hash the same key consistently");
        REDIS_RUN_TEST(test_keyslot_hash_tag, "should hash tagged keys to the same slot");
        REDIS_RUN_TEST(test_keyslot_different_tags, "should produce valid slots for different tags");
        REDIS_RUN_TEST(test_keyslot_empty_tag, "should hash empty tag keys consistently");
        REDIS_RUN_TEST(test_keyslot_no_closing_brace, "should hash keys without closing brace consistently");
        REDIS_RUN_TEST(test_keyslot_empty_key, "should hash empty key to valid slot");
    }

    group("Configuration") {
        REDIS_RUN_TEST(test_cluster_create_null_config, "should reject null config");
        REDIS_RUN_TEST(test_cluster_create_no_seeds, "should reject config without seeds");
        REDIS_RUN_TEST(test_cluster_create_with_seeds, "should create cluster with seeds");
        REDIS_RUN_TEST(test_cluster_create_custom_config, "should create cluster with custom config");
        REDIS_RUN_TEST(test_cluster_rejects_username_without_password, "should reject ACL username without password");
        REDIS_RUN_TEST(test_cluster_rejects_missing_seed_ports, "should reject seeds without ports");
        REDIS_RUN_TEST(test_cluster_accepts_replica_routing, "should accept replica routing configuration");
        REDIS_RUN_TEST(test_cluster_destroy_null, "should destroy null cluster safely");
    }

    group("Lifecycle") {
        REDIS_RUN_TEST(test_cluster_connect_null, "should reject null cluster connect");
        REDIS_RUN_TEST(test_cluster_disconnect_null, "should disconnect null cluster safely");
        REDIS_RUN_TEST(test_cluster_refresh_null, "should reject null cluster refresh");
        REDIS_RUN_TEST(test_cluster_connect_disconnect, "should connect and disconnect without crashing");
        REDIS_RUN_TEST(test_cluster_discovers_and_follows_redirections,
                       "should discover topology and follow MOVED and ASK contracts");
        REDIS_RUN_TEST(test_cluster_falls_back_for_legacy_redis,
                       "should fall back to SLOTS only when SHARDS is unknown");
        REDIS_RUN_TEST(test_cluster_routes_declared_reads_to_replica,
                       "should prepare replica connections and route declared reads");
        REDIS_RUN_TEST(test_cluster_rejects_replica_when_readonly_fails,
                       "should reject topology when replica READONLY fails");
        REDIS_RUN_TEST(test_cluster_replica_routing_requires_complete_topology,
                       "should reject replica routing without a replica per shard");
    }

    group("Commands") {
        REDIS_RUN_TEST(test_cluster_command_null, "should reject null cluster command");
        REDIS_RUN_TEST(test_cluster_command_null_format, "should reject null cluster command format");
        REDIS_RUN_TEST(test_cluster_command_key_null, "should reject null keyed cluster command");
        REDIS_RUN_TEST(test_cluster_commandv_null, "should reject null cluster commandv");
        REDIS_RUN_TEST(test_cluster_commandv_null_argv, "should reject null argv for cluster commandv");
        REDIS_RUN_TEST(test_cluster_commandv_result_not_connected, "should report result rejected before cluster connection");
    }

    group("Convenience Commands") {
        REDIS_RUN_TEST(test_cluster_set_not_connected, "should reject set when cluster not connected");
        REDIS_RUN_TEST(test_cluster_get_not_connected, "should reject get when cluster not connected");
        REDIS_RUN_TEST(test_cluster_del_not_connected, "should reject del when cluster not connected");
        REDIS_RUN_TEST(test_cluster_hset_not_connected, "should reject hset when cluster not connected");
        REDIS_RUN_TEST(test_cluster_xadd_null, "should reject null cluster xadd");
        REDIS_RUN_TEST(test_cluster_xread_null, "should reject null cluster xread");
    }

    group("Multi Key") {
        REDIS_RUN_TEST(test_cluster_mdelete_null, "should reject null cluster mdelete");
        REDIS_RUN_TEST(test_cluster_mdelete_no_keys, "should reject mdelete without keys");
        REDIS_RUN_TEST(test_cluster_mget_null, "should reject null cluster mget");
        REDIS_RUN_TEST(test_cluster_mget_hash_tags, "should keep tagged mget keys in one slot");
    }

    group("Statistics") {
        REDIS_RUN_TEST(test_cluster_get_stats_null, "should read null cluster stats safely");
        REDIS_RUN_TEST(test_cluster_get_stats, "should expose zeroed cluster stats");
        REDIS_RUN_TEST(test_cluster_reset_stats_null, "should reset null cluster stats safely");
        REDIS_RUN_TEST(test_cluster_reset_stats, "should reset cluster stats");
    }

    group("Health") {
        REDIS_RUN_TEST(test_cluster_is_healthy_null, "should report null cluster unhealthy");
        REDIS_RUN_TEST(test_cluster_is_healthy_not_connected, "should report disconnected cluster unhealthy");
        REDIS_RUN_TEST(test_cluster_node_count_null, "should zero node counts for null cluster");
        REDIS_RUN_TEST(test_cluster_node_count, "should report zero node counts before discovery");
        REDIS_RUN_TEST(test_cluster_get_node_null, "should reject get node on null cluster");
        REDIS_RUN_TEST(test_cluster_get_node_invalid_slot, "should reject invalid slot lookup");
    }

    group("Live Contract") {
        REDIS_RUN_TEST(test_cluster_live_rejects_standalone_redis,
                       "should reject standalone Redis as cluster topology");
        REDIS_RUN_TEST(test_cluster_live_multi_node_contract,
                       "should route against a real multi-node Redis Cluster");
    }
}
