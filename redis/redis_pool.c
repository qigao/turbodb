/**
 * @file redis_pool.c
 * @brief Redis Connection Pool Implementation
 */

#include "redis_pool.h"
#include "redis_internal.h"
#include "CoroNet/turbo_connection_pool.h"
#include "CoroNet/turbo_coro_context.h"
#include "CoroNet/turbo_coro_socket.h"
#include "object_pool.h"
#include "turbo_str.h"
#include <fmt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct redis_pool_conn_s {
    void *free_link;
    struct redis_pool_conn_s *next_all;
    redis_client_t *client;
    coro_socket_t *socket;
    coro_pool_t *owner_pool;
    int is_replica;
    int registered;
};

struct redis_pool_s {
    redis_pool_config_t config;
    coro_context_t *ctx;
    coro_pool_t *master_pool;
    coro_pool_t **replica_pools;
    object_pool_t *conn_wrapper_pool;
    object_pool_t *cmd_ctx_pool;
    object_pool_t *stream_ctx_pool;
    redis_pool_conn_t *all_conns;
    size_t next_replica;
    size_t observed_total;
    int running;
    redis_pool_stats_t stats;
};

struct redis_pipeline_s {
    redis_pool_t *pool;
    redis_pool_conn_t *conn;

    struct {
        char *command;
        size_t command_len;
        redis_command_cb_t callback;
        void *user_data;
    } *commands;
    size_t command_count;
    size_t command_capacity;
};

typedef struct {
    redis_pool_t *pool;
    redis_pool_conn_t *conn;
    redis_command_cb_t user_callback;
    void *user_data;
} pool_cmd_ctx_t;

typedef struct {
    redis_pool_t *pool;
    redis_pool_conn_t *conn;
    redis_stream_cb_t user_callback;
    void *user_data;
} pool_stream_ctx_t;

#ifdef REDIS_TESTING
static redis_pool_test_send_hook_t redis_pool_test_send_hook = NULL;
static redis_pool_test_recv_hook_t redis_pool_test_recv_hook = NULL;
static redis_pool_test_free_recv_hook_t redis_pool_test_free_recv_hook = NULL;
#endif

static size_t redis_pool_endpoint_count(const redis_pool_config_t *config) {
    return config ? (size_t)1 + config->replica_count : 1;
}

static size_t redis_pool_wrapper_capacity(const redis_pool_config_t *config) {
    size_t endpoints;
    size_t max_connections;

    if (!config) {
        return 16;
    }

    endpoints = redis_pool_endpoint_count(config);
    max_connections = config->max_connections ? config->max_connections : 10;
    return endpoints * max_connections;
}

static int init_runtime_pools(redis_pool_t *pool) {
    object_pool_config_t config;
    size_t wrapper_capacity;

    if (!pool) {
        return -1;
    }

    wrapper_capacity = redis_pool_wrapper_capacity(&pool->config);

    config.object_size = sizeof(redis_pool_conn_t);
    config.initial_capacity = wrapper_capacity > 0 ? wrapper_capacity : 16;
    config.max_capacity = wrapper_capacity > 0 ? wrapper_capacity : 0;
    config.zero_on_alloc = false;
    pool->conn_wrapper_pool = object_pool_create(&config);
    if (!pool->conn_wrapper_pool) {
        return -1;
    }

    config.object_size = sizeof(pool_cmd_ctx_t);
    config.initial_capacity = wrapper_capacity > 0 ? wrapper_capacity : 16;
    config.max_capacity = 0;
    config.zero_on_alloc = true;
    pool->cmd_ctx_pool = object_pool_create(&config);
    if (!pool->cmd_ctx_pool) {
        return -1;
    }

    config.object_size = sizeof(pool_stream_ctx_t);
    config.initial_capacity = wrapper_capacity > 0 ? wrapper_capacity : 16;
    config.max_capacity = 0;
    config.zero_on_alloc = true;
    pool->stream_ctx_pool = object_pool_create(&config);
    if (!pool->stream_ctx_pool) {
        return -1;
    }

    return 0;
}

static void destroy_runtime_pools(redis_pool_t *pool) {
    if (!pool) {
        return;
    }

    object_pool_destroy(pool->conn_wrapper_pool);
    object_pool_destroy(pool->cmd_ctx_pool);
    object_pool_destroy(pool->stream_ctx_pool);
    pool->conn_wrapper_pool = NULL;
    pool->cmd_ctx_pool = NULL;
    pool->stream_ctx_pool = NULL;
}

static int redis_pool_socket_send(coro_socket_t *socket, const char *data, size_t len) {
#ifdef REDIS_TESTING
    if (redis_pool_test_send_hook) {
        return redis_pool_test_send_hook(socket, data, len);
    }
#endif
    return coro_socket_send(socket, data, len);
}

static int redis_pool_socket_recv(coro_socket_t *socket, char **data, size_t *len) {
#ifdef REDIS_TESTING
    if (redis_pool_test_recv_hook) {
        return redis_pool_test_recv_hook(socket, data, len);
    }
#endif
    return coro_socket_recv(socket, data, len);
}

static void redis_pool_socket_free_recv(void *data) {
#ifdef REDIS_TESTING
    if (redis_pool_test_free_recv_hook) {
        redis_pool_test_free_recv_hook(data);
        return;
    }
#endif
    coro_socket_free_recv(data);
}

static int pipeline_recv_reply(redis_client_t *client, redis_reply_t **reply) {
    for (;;) {
        int parsed = redis_parse_resp_reply(client, reply);
        if (parsed < 0) {
            return -1;
        }
        if (parsed > 0) {
            memmove(client->recv_buffer,
                    client->recv_buffer + parsed,
                    client->recv_buffer_used - (size_t)parsed);
            client->recv_buffer_used -= (size_t)parsed;
            return 0;
        }

        char *data = NULL;
        size_t len = 0;
        if (redis_pool_socket_recv(client->socket, &data, &len) != 0) {
            if (data) {
                redis_pool_socket_free_recv(data);
            }
            client->is_connected = 0;
            return -1;
        }

        if (redis_recv_buffer_append(client, data, len) != 0) {
            redis_pool_socket_free_recv(data);
            return -1;
        }
        redis_pool_socket_free_recv(data);
    }
}

static int pipeline_count_args(const char *command) {
    int argc = 0;
    const char *p = command;

    if (!command) {
        return -1;
    }

    while (*p) {
        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        argc++;
        while (*p && *p != ' ') {
            p++;
        }
    }

    return argc;
}

static int pipeline_size_add(size_t *total, size_t amount) {
    if (!total || amount > SIZE_MAX - *total) {
        return -1;
    }
    *total += amount;
    return 0;
}

static int pipeline_write_resp_command(char *dest, size_t capacity, const char *command, size_t *written) {
    char *copy;
    char *p;
    int argc;
    int offset = 0;

    if (!dest || !command || !written) {
        return -1;
    }

    argc = pipeline_count_args(command);
    if (argc <= 0) {
        return -1;
    }

    copy = tstr_dup(command);
    if (!copy) {
        return -1;
    }

    offset += fmt(dest + offset, capacity - (size_t)offset, "*{}\r\n", argc);
    p = copy;
    while (*p) {
        char *start;
        size_t len;

        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }

        start = p;
        while (*p && *p != ' ') {
            p++;
        }
        len = (size_t)(p - start);

        offset += fmt(dest + offset, capacity - (size_t)offset, "${}\r\n", len);
        memcpy(dest + offset, start, len);
        offset += (int)len;
        dest[offset++] = '\r';
        dest[offset++] = '\n';
    }

    tstr_free(copy);
    *written = (size_t)offset;
    return 0;
}

static char *pipeline_build_batch_buffer(redis_pipeline_t *pipeline, size_t *out_len) {
    size_t total = 0;
    char *buffer;
    size_t offset = 0;

    if (!pipeline || !out_len || pipeline->command_count == 0) {
        return NULL;
    }

    for (size_t i = 0; i < pipeline->command_count; i++) {
        const char *command = pipeline->commands[i].command;
        int argc = pipeline_count_args(command);
        const char *p = command;

        if (argc <= 0) {
            return NULL;
        }

        total += 1 + 20 + 2;
        while (*p) {
            size_t len;
            while (*p == ' ') {
                p++;
            }
            if (!*p) {
                break;
            }
            {
                const char *start = p;
                while (*p && *p != ' ') {
                    p++;
                }
                len = (size_t)(p - start);
            }
            total += 1 + 20 + 2;
            total += len + 2;
        }
    }

    buffer = malloc(total);
    if (!buffer) {
        return NULL;
    }

    for (size_t i = 0; i < pipeline->command_count; i++) {
        size_t written = 0;
        if (pipeline_write_resp_command(buffer + offset, total - offset,
                                        pipeline->commands[i].command, &written) != 0) {
            free(buffer);
            return NULL;
        }
        offset += written;
    }

    *out_len = offset;
    return buffer;
}

static char *pipeline_build_resp_commandv(int argc, const char **argv,
                                          const size_t *argvlen,
                                          size_t *out_len) {
    size_t total = 0;
    char *cmd;
    char *p;

    if (argc <= 0 || !argv || !out_len) {
        return NULL;
    }

    if (pipeline_size_add(&total, 1 + 20 + 2) != 0) {
        return NULL;
    }
    for (int i = 0; i < argc; i++) {
        size_t len;

        if (!argv[i]) {
            return NULL;
        }

        len = argvlen ? argvlen[i] : strlen(argv[i]);
        if (pipeline_size_add(&total, 1 + 20 + 2) != 0 ||
            len > SIZE_MAX - 2 || pipeline_size_add(&total, len + 2) != 0) {
            return NULL;
        }
    }

    cmd = malloc(total);
    if (!cmd) {
        return NULL;
    }

    p = cmd;
    {
        int written = fmt(p, total, "*{}\r\n", argc);
        if (written < 0 || (size_t)written >= total) {
            free(cmd);
            return NULL;
        }
        p += written;
    }
    for (int i = 0; i < argc; i++) {
        size_t len = argvlen ? argvlen[i] : strlen(argv[i]);
        {
            size_t remaining = total - (size_t)(p - cmd);
            int written = fmt(p, remaining, "${}\r\n", len);
            if (written < 0 || (size_t)written >= remaining) {
                free(cmd);
                return NULL;
            }
            p += written;
        }
        memcpy(p, argv[i], len);
        p += len;
        *p++ = '\r';
        *p++ = '\n';
    }

    *out_len = (size_t)(p - cmd);
    return cmd;
}

static int pipeline_ensure_capacity(redis_pipeline_t *pipeline, size_t additional) {
    size_t required;
    size_t new_capacity;
    void *new_commands;

    if (!pipeline) {
        return -1;
    }

    if (!pipeline->pool || additional > pipeline->pool->config.pipeline_max ||
        pipeline->command_count > pipeline->pool->config.pipeline_max - additional ||
        additional > SIZE_MAX - pipeline->command_count) {
        return -1;
    }
    required = pipeline->command_count + additional;
    if (required <= pipeline->command_capacity) {
        return 0;
    }

    new_capacity = pipeline->command_capacity ? pipeline->command_capacity : 32;
    while (new_capacity < required) {
        if (new_capacity > SIZE_MAX / 2) {
            return -1;
        }
        new_capacity *= 2;
    }

    if (new_capacity > SIZE_MAX / sizeof(*pipeline->commands)) {
        return -1;
    }

    new_commands = realloc(pipeline->commands, new_capacity * sizeof(*pipeline->commands));
    if (!new_commands) {
        return -1;
    }

    pipeline->commands = new_commands;
    pipeline->command_capacity = new_capacity;
    return 0;
}

static void pipeline_reset_commands(redis_pipeline_t *pipeline) {
    if (!pipeline || !pipeline->commands) {
        return;
    }

    for (size_t i = 0; i < pipeline->command_count; i++) {
        free(pipeline->commands[i].command);
        pipeline->commands[i].command = NULL;
        pipeline->commands[i].command_len = 0;
        pipeline->commands[i].callback = NULL;
        pipeline->commands[i].user_data = NULL;
    }

    pipeline->command_count = 0;
}

static int pipeline_append_encoded_command(redis_pipeline_t *pipeline,
                                           char *command,
                                           size_t command_len,
                                           redis_command_cb_t callback,
                                           void *user_data) {
    size_t idx;

    if (!pipeline || !command || command_len == 0) {
        free(command);
        return -1;
    }

    if (pipeline_ensure_capacity(pipeline, 1) != 0) {
        free(command);
        return -1;
    }

    idx = pipeline->command_count++;
    pipeline->commands[idx].command = command;
    pipeline->commands[idx].command_len = command_len;
    pipeline->commands[idx].callback = callback;
    pipeline->commands[idx].user_data = user_data;
    return 0;
}

static int pipeline_execute_prepared(redis_pipeline_t *pipeline,
                                     redis_client_t *client,
                                     redis_reply_t ***out_replies,
                                     size_t *out_reply_count,
                                     redis_command_outcome_t *out_outcome) {
    size_t total = 0;
    char *batch;
    size_t offset = 0;
    redis_reply_t **replies = NULL;

    if (out_replies) {
        *out_replies = NULL;
    }
    if (out_reply_count) {
        *out_reply_count = 0;
    }
    if (out_outcome) {
        *out_outcome = REDIS_COMMAND_NOT_SENT;
    }

    if (!pipeline || !client || !client->socket || pipeline->command_count == 0) {
        return -1;
    }

    if (out_replies) {
        replies = calloc(pipeline->command_count, sizeof(*replies));
        if (!replies) {
            return -1;
        }
    }

    for (size_t i = 0; i < pipeline->command_count; i++) {
        if (!pipeline->commands[i].command || pipeline->commands[i].command_len == 0) {
            free(replies);
            return -1;
        }
        if (pipeline_size_add(&total, pipeline->commands[i].command_len) != 0) {
            free(replies);
            return -1;
        }
    }

    batch = malloc(total);
    if (!batch) {
        free(replies);
        return -1;
    }

    for (size_t i = 0; i < pipeline->command_count; i++) {
        memcpy(batch + offset,
               pipeline->commands[i].command,
               pipeline->commands[i].command_len);
        offset += pipeline->commands[i].command_len;
    }

    if (redis_pool_socket_send(client->socket, batch, offset) != 0) {
        free(batch);
        free(replies);
        client->is_connected = 0;
        if (out_outcome) {
            *out_outcome = REDIS_COMMAND_SEND_UNCERTAIN;
        }
        return -1;
    }
    free(batch);

    for (size_t i = 0; i < pipeline->command_count; i++) {
        redis_reply_t *reply = NULL;

        if (pipeline_recv_reply(client, &reply) != 0) {
            if (replies) {
                for (size_t j = 0; j < i; j++) {
                    redis_reply_free(replies[j]);
                }
                free(replies);
            }
            if (out_outcome) {
                *out_outcome = REDIS_COMMAND_REPLY_UNKNOWN;
            }
            return -1;
        }

        if (replies) {
            replies[i] = reply;
        }

        if (pipeline->commands[i].callback) {
            pipeline->commands[i].callback(client, reply, pipeline->commands[i].user_data);
        }

        if (!replies) {
            redis_reply_free(reply);
        }
    }

    if (out_replies) {
        *out_replies = replies;
    }
    if (out_reply_count) {
        *out_reply_count = pipeline->command_count;
    }
    if (out_outcome) {
        *out_outcome = REDIS_COMMAND_REPLIED;
    }
    return 0;
}

static int pipeline_execute_batch(redis_client_t *client,
                                  const char **commands,
                                  redis_command_cb_t *callbacks,
                                  void **user_datas,
                                  size_t command_count,
                                  redis_reply_t ***out_replies,
                                  size_t *out_reply_count) {
    size_t total = 0;
    char *batch;
    size_t offset = 0;
    redis_reply_t **replies = NULL;

    if (!client || !client->socket || !commands || command_count == 0) {
        return -1;
    }

    if (out_replies) {
        replies = calloc(command_count, sizeof(*replies));
        if (!replies) {
            return -1;
        }
    }

    for (size_t i = 0; i < command_count; i++) {
        const char *command = commands[i];
        int argc = pipeline_count_args(command);
        const char *p = command;

        if (argc <= 0) {
            free(replies);
            return -1;
        }

        total += 1 + 20 + 2;
        while (*p) {
            while (*p == ' ') {
                p++;
            }
            if (!*p) {
                break;
            }
            {
                const char *start = p;
                while (*p && *p != ' ') {
                    p++;
                }
                total += 1 + 20 + 2;
                total += (size_t)(p - start) + 2;
            }
        }
    }

    batch = malloc(total);
    if (!batch) {
        free(replies);
        return -1;
    }

    for (size_t i = 0; i < command_count; i++) {
        size_t written = 0;
        if (pipeline_write_resp_command(batch + offset, total - offset, commands[i], &written) != 0) {
            free(batch);
            free(replies);
            return -1;
        }
        offset += written;
    }

    if (redis_pool_socket_send(client->socket, batch, offset) != 0) {
        free(batch);
        free(replies);
        client->is_connected = 0;
        return -1;
    }
    free(batch);

    for (size_t i = 0; i < command_count; i++) {
        redis_reply_t *reply = NULL;
        if (pipeline_recv_reply(client, &reply) != 0) {
            if (replies) {
                for (size_t j = 0; j < i; j++) {
                    redis_reply_free(replies[j]);
                }
                free(replies);
            }
            return -1;
        }
        if (replies) {
            replies[i] = reply;
        }
        if (callbacks && callbacks[i]) {
            callbacks[i](client, reply, user_datas ? user_datas[i] : NULL);
        }
        if (!replies) {
            redis_reply_free(reply);
        }
    }

    if (out_replies) {
        *out_replies = replies;
    }
    if (out_reply_count) {
        *out_reply_count = command_count;
    }
    return 0;
}

static int redis_pool_strings_equal(const char *lhs, const char *rhs) {
    const char *left = lhs ? lhs : "";
    const char *right = rhs ? rhs : "";
    return strcmp(left, right) == 0;
}

static void register_conn_wrapper(redis_pool_t *pool, redis_pool_conn_t *conn) {
    if (!pool || !conn || conn->registered) {
        return;
    }

    conn->next_all = pool->all_conns;
    pool->all_conns = conn;
    conn->registered = 1;
}

static int reconfigure_pooled_client(redis_pool_conn_t *conn, const redis_config_t *config) {
    tstr new_host = NULL;
    tstr new_username = NULL;
    tstr new_password = NULL;
    int replace_username;
    int replace_password;

    if (!conn || !config) {
        return -1;
    }

    if (!conn->client) {
        conn->client = redis_client_create_with_config(config);
        return conn->client ? 0 : -1;
    }

    if (!redis_pool_strings_equal(conn->client->config.host, config->host)) {
        new_host = tstr_dup(config->host);
        if (!new_host) {
            return -1;
        }
    }

    replace_username = !redis_pool_strings_equal(conn->client->config.username, config->username);
    if (replace_username && config->username) {
        new_username = tstr_dup(config->username);
        if (!new_username) {
            tstr_free(new_host);
            return -1;
        }
    }

    replace_password = !redis_pool_strings_equal(conn->client->config.password, config->password);
    if (replace_password && config->password) {
        new_password = tstr_dup(config->password);
        if (!new_password) {
            tstr_free(new_host);
            tstr_free(new_username);
            return -1;
        }
    }

    if (new_host) {
        tstr_free((tstr)conn->client->config.host);
        conn->client->config.host = new_host;
    }

    if (replace_username) {
        tstr_free((tstr)conn->client->config.username);
        conn->client->config.username = new_username;
    }

    if (replace_password) {
        tstr_free((tstr)conn->client->config.password);
        conn->client->config.password = new_password;
    }

    conn->client->config.port = config->port;
    conn->client->config.database = config->database;
    conn->client->config.timeout_ms = config->timeout_ms;
    conn->client->config.command_timeout_ms = config->command_timeout_ms;
    conn->client->config.max_pipeline = config->max_pipeline;
    conn->client->config.cluster_readonly = config->cluster_readonly;
    return 0;
}

static void destroy_pooled_clients(redis_pool_t *pool) {
    redis_pool_conn_t *conn;

    if (!pool) {
        return;
    }

    conn = pool->all_conns;
    while (conn) {
        if (conn->client) {
            redis_client_destroy(conn->client);
            conn->client = NULL;
        }
        conn = conn->next_all;
    }
}

static void refresh_pool_stats(redis_pool_t *pool) {
    size_t active = 0;
    size_t idle = 0;
    size_t total = 0;

    if (!pool) {
        return;
    }

    if (pool->master_pool) {
        active += coro_pool_borrowed_count(pool->master_pool);
        idle += coro_pool_idle_count(pool->master_pool);
        total += coro_pool_size(pool->master_pool);
    }

    for (size_t i = 0; i < pool->config.replica_count; i++) {
        if (!pool->replica_pools || !pool->replica_pools[i]) {
            continue;
        }
        active += coro_pool_borrowed_count(pool->replica_pools[i]);
        idle += coro_pool_idle_count(pool->replica_pools[i]);
        total += coro_pool_size(pool->replica_pools[i]);
    }

    if (total > pool->observed_total) {
        pool->stats.connections_created += (uint64_t)(total - pool->observed_total);
    } else if (pool->observed_total > total) {
        pool->stats.connections_closed += (uint64_t)(pool->observed_total - total);
    }

    pool->observed_total = total;
    pool->stats.total_connections = total;
    pool->stats.active_connections = active;
    pool->stats.idle_connections = idle;
}

static void free_config_copy(redis_pool_t *pool) {
    if (!pool) {
        return;
    }

    tstr_free((tstr)pool->config.master_host);
    tstr_free((tstr)pool->config.username);
    tstr_free((tstr)pool->config.password);

    if (pool->config.replica_hosts) {
        for (size_t i = 0; i < pool->config.replica_count; i++) {
            tstr_free((tstr)pool->config.replica_hosts[i]);
        }
        free((char **)pool->config.replica_hosts);
    }

    free(pool->config.replica_ports);
    pool->config.master_host = NULL;
    pool->config.username = NULL;
    pool->config.password = NULL;
    pool->config.replica_hosts = NULL;
    pool->config.replica_ports = NULL;
    pool->config.replica_count = 0;
}

static int build_pool_url(char *buffer, size_t size, const char *host, uint16_t port) {
    if (!buffer || size == 0 || !host) {
        return -1;
    }
    return fmt(buffer, size, "tcp://{}:{}", host, (unsigned int)port) > 0 ? 0 : -1;
}

static int redis_pool_initialize_connection(coro_socket_t *socket, void *user_data) {
    redis_pool_t *pool = (redis_pool_t *)user_data;
    redis_client_t *client = NULL;
    redis_config_t config;
    int rc;

    if (!socket || !pool || !pool->ctx) return TURBO_EINVAL;
    memset(&config, 0, sizeof(config));
    config.host = pool->config.master_host;
    config.port = pool->config.master_port;
    config.username = pool->config.username;
    config.password = pool->config.password;
    config.database = pool->config.database;
    config.timeout_ms = pool->config.connect_timeout_ms;
    config.command_timeout_ms = pool->config.command_timeout_ms;
    config.max_pipeline = pool->config.pipeline_max;
    config.cluster_readonly = pool->config.cluster_readonly;
    client = redis_client_create_with_config(&config);
    if (!client) return TURBO_ENOMEM;
    rc = redis_client_attach_socket(client, pool->ctx, socket, 0);
    if (rc == TURBO_OK) rc = redis_client_prepare(client);
    (void)redis_client_detach_socket(client);
    redis_client_destroy(client);
    return rc;
}

static coro_pool_t *create_coro_pool(redis_pool_t *pool,
                                     const redis_pool_config_t *config) {
    coro_pool_config_t pool_config = CORO_POOL_CONFIG_DEFAULT;
    coro_pool_t *result;

    if (!pool || !pool->ctx || !config) {
        return NULL;
    }

    pool_config.min_size = config->min_connections;
    pool_config.max_size = config->max_connections;
    pool_config.connect_timeout_ms = config->connect_timeout_ms;
    pool_config.idle_timeout_ms = config->idle_timeout_ms;
    result = coro_pool_create(pool->ctx, &pool_config);
    if (!result) return NULL;
    if (coro_pool_set_connection_initializer(result, redis_pool_initialize_connection,
                                             pool) != TURBO_OK) {
        coro_pool_destroy(result);
        return NULL;
    }
    return result;
}

static void destroy_replica_pools(redis_pool_t *pool) {
    if (!pool || !pool->replica_pools) {
        return;
    }

    for (size_t i = 0; i < pool->config.replica_count; i++) {
        if (pool->replica_pools[i]) {
            coro_pool_destroy(pool->replica_pools[i]);
            pool->replica_pools[i] = NULL;
        }
    }

    free(pool->replica_pools);
    pool->replica_pools = NULL;
}

static redis_pool_conn_t *wrap_borrowed_socket(redis_pool_t *pool,
                                               coro_pool_t *owner_pool,
                                               coro_socket_t *socket,
                                               const char *host,
                                               uint16_t port,
                                               int is_replica) {
    redis_pool_conn_t *conn;
    redis_config_t config;

    if (!pool || !owner_pool || !socket || !host) {
        return NULL;
    }

    conn = object_pool_alloc(pool->conn_wrapper_pool);
    if (!conn) {
        coro_pool_return(owner_pool, socket);
        return NULL;
    }
    register_conn_wrapper(pool, conn);

    memset(&config, 0, sizeof(config));
    config.host = host;
    config.port = port;
    config.username = pool->config.username;
    config.password = pool->config.password;
    config.database = pool->config.database;
    config.timeout_ms = pool->config.connect_timeout_ms;
    config.command_timeout_ms = pool->config.command_timeout_ms;
    config.max_pipeline = pool->config.pipeline_max;
    config.cluster_readonly = pool->config.cluster_readonly;

    if (reconfigure_pooled_client(conn, &config) != 0) {
        coro_pool_return(owner_pool, socket);
        object_pool_free(pool->conn_wrapper_pool, conn);
        return NULL;
    }

    if (redis_client_attach_socket(conn->client, pool->ctx, socket, 0) != 0) {
        coro_pool_return(owner_pool, socket);
        object_pool_free(pool->conn_wrapper_pool, conn);
        return NULL;
    }
    conn->client->is_authenticated = pool->config.password ? 1 : 0;
    conn->client->selected_db = pool->config.database;
    conn->client->is_cluster_readonly = pool->config.cluster_readonly ? 1 : 0;

    conn->socket = socket;
    conn->owner_pool = owner_pool;
    conn->is_replica = is_replica;
    return conn;
}

static void destroy_pool_conn(redis_pool_t *pool, redis_pool_conn_t *conn) {
    if (!conn) {
        return;
    }

    if (conn->client) {
        redis_client_disconnect(conn->client);
    }
    conn->socket = NULL;
    conn->owner_pool = NULL;
    conn->is_replica = 0;
    if (pool && pool->conn_wrapper_pool) {
        object_pool_free(pool->conn_wrapper_pool, conn);
    }
}

static int pool_release_conn(redis_pool_t *pool, redis_pool_conn_t *conn) {
    coro_pool_t *owner_pool;
    coro_socket_t *socket;
    int reusable;

    if (!pool || !conn) {
        return -1;
    }

    owner_pool = conn->owner_pool;
    reusable = conn->client && conn->client->is_connected;
    socket = redis_client_detach_socket(conn->client);
    destroy_pool_conn(pool, conn);

    if (socket && owner_pool) {
        if (reusable) {
            coro_pool_return(owner_pool, socket);
        } else {
            (void)coro_pool_discard(owner_pool, socket);
        }
    }

    refresh_pool_stats(pool);
    return 0;
}

static char *format_command(const char *format, va_list ap) {
    tstr buffer;

    if (!format) {
        return NULL;
    }

    buffer = tstr_new();
    if (!buffer) {
        return NULL;
    }

    buffer = tstr_cat_vfmt(buffer, format, ap);
    return buffer;
}

static pool_cmd_ctx_t *create_cmd_ctx(redis_pool_t *pool, redis_pool_conn_t *conn,
                                      redis_command_cb_t callback, void *user_data) {
    pool_cmd_ctx_t *ctx;

    if (!pool || !pool->cmd_ctx_pool) {
        return NULL;
    }

    ctx = object_pool_alloc(pool->cmd_ctx_pool);
    if (!ctx) {
        return NULL;
    }
    ctx->pool = pool;
    ctx->conn = conn;
    ctx->user_callback = callback;
    ctx->user_data = user_data;
    return ctx;
}

static int finish_cmd_submit(redis_pool_t *pool, redis_pool_conn_t *conn,
                             pool_cmd_ctx_t *ctx, int result) {
    if (result == 0) {
        return 0;
    }

    pool_release_conn(pool, conn);
    object_pool_free(pool->cmd_ctx_pool, ctx);
    pool->stats.commands_failed++;
    return -1;
}

static pool_stream_ctx_t *create_stream_ctx(redis_pool_t *pool, redis_pool_conn_t *conn,
                                            redis_stream_cb_t callback, void *user_data) {
    pool_stream_ctx_t *ctx;

    if (!pool || !pool->stream_ctx_pool) {
        return NULL;
    }

    ctx = object_pool_alloc(pool->stream_ctx_pool);
    if (!ctx) {
        return NULL;
    }
    ctx->pool = pool;
    ctx->conn = conn;
    ctx->user_callback = callback;
    ctx->user_data = user_data;
    return ctx;
}

static int finish_stream_submit(redis_pool_t *pool, redis_pool_conn_t *conn,
                                pool_stream_ctx_t *ctx, int result) {
    if (result == 0) {
        return 0;
    }

    pool_release_conn(pool, conn);
    object_pool_free(pool->stream_ctx_pool, ctx);
    pool->stats.commands_failed++;
    return -1;
}

redis_pool_t *redis_pool_create(const redis_pool_config_t *config) {
    redis_pool_t *pool;

    if (!config || !config->master_host || !config->master_host[0] ||
        config->master_port == 0 || config->database < 0 || config->database > 15 ||
        (config->username && !config->password) ||
        (config->replica_count > 0 && (!config->replica_hosts || !config->replica_ports))) {
        return NULL;
    }
    for (size_t i = 0; i < config->replica_count; ++i) {
        if (!config->replica_hosts[i] || !config->replica_hosts[i][0] ||
            config->replica_ports[i] == 0)
            return NULL;
    }

    pool = calloc(1, sizeof(*pool));
    if (!pool) {
        return NULL;
    }

    pool->config = *config;
    pool->config.master_host = NULL;
    pool->config.username = NULL;
    pool->config.password = NULL;
    pool->config.replica_hosts = NULL;
    pool->config.replica_ports = NULL;
    pool->config.replica_count = 0;
    if (pool->config.min_connections == 0) pool->config.min_connections = 2;
    if (pool->config.max_connections == 0) pool->config.max_connections = 10;
    if (pool->config.min_connections > pool->config.max_connections) {
        pool->config.min_connections = pool->config.max_connections;
    }
    if (pool->config.connect_timeout_ms == 0) pool->config.connect_timeout_ms = 5000;
    if (pool->config.command_timeout_ms == 0)
        pool->config.command_timeout_ms = pool->config.connect_timeout_ms;
    if (pool->config.idle_timeout_ms == 0) pool->config.idle_timeout_ms = 60000;
    if (pool->config.health_check_ms == 0) pool->config.health_check_ms = 30000;
    if (pool->config.pipeline_max == 0) pool->config.pipeline_max = 100;

    pool->config.master_host = tstr_dup(config->master_host);
    if (!pool->config.master_host) {
        redis_pool_destroy(pool);
        return NULL;
    }

    if (config->username) {
        pool->config.username = tstr_dup(config->username);
        if (!pool->config.username) {
            redis_pool_destroy(pool);
            return NULL;
        }
    }

    if (config->password) {
        pool->config.password = tstr_dup(config->password);
        if (!pool->config.password) {
            redis_pool_destroy(pool);
            return NULL;
        }
    }

    if (config->replica_count > 0) {
        char **hosts = calloc(config->replica_count, sizeof(*hosts));
        uint16_t *ports = calloc(config->replica_count, sizeof(*ports));
        if (!hosts || !ports) {
            free(hosts);
            free(ports);
            redis_pool_destroy(pool);
            return NULL;
        }

        for (size_t i = 0; i < config->replica_count; i++) {
            hosts[i] = tstr_dup(config->replica_hosts[i]);
            if (!hosts[i]) {
                for (size_t j = 0; j < i; j++) tstr_free(hosts[j]);
                free(hosts);
                free(ports);
                redis_pool_destroy(pool);
                return NULL;
            }
            ports[i] = config->replica_ports[i];
        }

        pool->config.replica_hosts = (const char **)hosts;
        pool->config.replica_ports = ports;
        pool->config.replica_count = config->replica_count;
    } else {
        pool->config.replica_hosts = NULL;
        pool->config.replica_ports = NULL;
        pool->config.replica_count = 0;
    }

    if (init_runtime_pools(pool) != 0) {
        redis_pool_destroy(pool);
        return NULL;
    }

    return pool;
}

int redis_pool_start(redis_pool_t *pool) {
    coro_context_t *ctx;

    if (!pool) {
        return -1;
    }

    ctx = coro_context_current();
    if (!ctx) {
        return -1;
    }

    redis_pool_stop(pool);
    pool->ctx = ctx;

    pool->master_pool = create_coro_pool(pool, &pool->config);
    if (!pool->master_pool) {
        return -1;
    }

    if (coro_pool_open(pool->master_pool, pool->config.master_host, (int)pool->config.master_port, CORO_SOCKET_TCP_V4) != 0) {
        coro_pool_destroy(pool->master_pool);
        pool->master_pool = NULL;
        return -1;
    }

    if (pool->config.replica_count > 0) {
        pool->replica_pools = calloc(pool->config.replica_count, sizeof(*pool->replica_pools));
        if (!pool->replica_pools) {
            redis_pool_stop(pool);
            return -1;
        }

        for (size_t i = 0; i < pool->config.replica_count; i++) {
            pool->replica_pools[i] = create_coro_pool(pool, &pool->config);
            if (!pool->replica_pools[i]) {
                continue;
            }

            if (coro_pool_open(pool->replica_pools[i], pool->config.replica_hosts[i], (int)pool->config.replica_ports[i], CORO_SOCKET_TCP_V4) != 0) {
                coro_pool_destroy(pool->replica_pools[i]);
                pool->replica_pools[i] = NULL;
            }
        }
    }

    pool->running = 1;
    pool->observed_total = 0;
    refresh_pool_stats(pool);
    return 0;
}

void redis_pool_stop(redis_pool_t *pool) {
    if (!pool) {
        return;
    }

    pool->running = 0;

    if (pool->master_pool) {
        coro_pool_destroy(pool->master_pool);
        pool->master_pool = NULL;
    }

    destroy_replica_pools(pool);
    pool->ctx = NULL;
    pool->next_replica = 0;
    pool->observed_total = 0;
    pool->stats.total_connections = 0;
    pool->stats.active_connections = 0;
    pool->stats.idle_connections = 0;
}

void redis_pool_destroy(redis_pool_t *pool) {
    if (!pool) {
        return;
    }

    redis_pool_stop(pool);
    destroy_pooled_clients(pool);
    free_config_copy(pool);
    destroy_runtime_pools(pool);
    free(pool);
}

static int redis_pool_acquire_ex(redis_pool_t *pool, int read_only,
                                 redis_pool_conn_t **out) {
    coro_socket_t *socket = NULL;
    redis_pool_conn_t *conn;
    int rc;

    if (!out) return TURBO_EINVAL;
    *out = NULL;
    if (!pool || !pool->running || !pool->master_pool) {
        return TURBO_ENOTCONN;
    }

    if (read_only && pool->replica_pools) {
        for (size_t attempt = 0; attempt < pool->config.replica_count; attempt++) {
            size_t idx = (pool->next_replica + attempt) % pool->config.replica_count;
            if (!pool->replica_pools[idx]) {
                continue;
            }
            rc = coro_pool_borrow(pool->replica_pools[idx], &socket);
            if (rc == 0) {
                pool->next_replica = idx + 1;
                refresh_pool_stats(pool);
                conn = wrap_borrowed_socket(pool, pool->replica_pools[idx], socket,
                                            pool->config.replica_hosts[idx],
                                            pool->config.replica_ports[idx], 1);
                if (!conn) return TURBO_ENOMEM;
                *out = conn;
                return TURBO_OK;
            }
        }
    }

    rc = coro_pool_borrow(pool->master_pool, &socket);
    if (rc != 0) {
        pool->stats.waiting_requests++;
        refresh_pool_stats(pool);
        return rc;
    }

    refresh_pool_stats(pool);
    conn = wrap_borrowed_socket(pool, pool->master_pool, socket,
                                pool->config.master_host, pool->config.master_port, 0);
    if (!conn) return TURBO_ENOMEM;
    *out = conn;
    return TURBO_OK;
}

redis_pool_conn_t *redis_pool_acquire(redis_pool_t *pool, int read_only) {
    redis_pool_conn_t *conn = NULL;
    return redis_pool_acquire_ex(pool, read_only, &conn) == TURBO_OK ? conn : NULL;
}

void redis_pool_release(redis_pool_t *pool, redis_pool_conn_t *conn) {
    if (!pool || !conn) {
        return;
    }
    pool_release_conn(pool, conn);
}

redis_client_t *redis_pool_conn_client(redis_pool_conn_t *conn) {
    return conn ? conn->client : NULL;
}

static void on_pool_command_done(redis_client_t *client, redis_reply_t *reply, void *user_data) {
    pool_cmd_ctx_t *ctx = (pool_cmd_ctx_t *)user_data;
    (void)client;

    if (ctx->user_callback) {
        ctx->user_callback(client, reply, ctx->user_data);
    }

    pool_release_conn(ctx->pool, ctx->conn);
    ctx->pool->stats.commands_sent++;
    object_pool_free(ctx->pool->cmd_ctx_pool, ctx);
}

static int pool_vcommand(redis_pool_t *pool, int read_only, redis_command_cb_t callback,
                         void *user_data, const char *format, va_list ap) {
    redis_pool_conn_t *conn;
    pool_cmd_ctx_t *ctx;
    char *command;
    int result;

    if (!pool || !format) {
        return -1;
    }

    conn = redis_pool_acquire(pool, read_only);
    if (!conn) {
        return -1;
    }

    ctx = create_cmd_ctx(pool, conn, callback, user_data);
    if (!ctx) {
        pool_release_conn(pool, conn);
        return -1;
    }

    command = format_command(format, ap);
    if (!command) {
        pool_release_conn(pool, conn);
        object_pool_free(pool->cmd_ctx_pool, ctx);
        pool->stats.commands_failed++;
        return -1;
    }

    result = redis_command(conn->client, on_pool_command_done, ctx, "%s", command);
    tstr_free(command);
    return finish_cmd_submit(pool, conn, ctx, result);
}

int redis_pool_command(redis_pool_t *pool, redis_command_cb_t callback,
                       void *user_data, const char *format, ...) {
    va_list ap;
    int result;

    if (!pool || !format) {
        return -1;
    }

    va_start(ap, format);
    result = pool_vcommand(pool, 0, callback, user_data, format, ap);
    va_end(ap);
    return result;
}

int redis_pool_commandv(redis_pool_t *pool, int argc, const char **argv,
                        const size_t *argvlen, redis_command_cb_t callback,
                        void *user_data) {
    redis_pool_conn_t *conn;
    pool_cmd_ctx_t *ctx;
    int result;

    if (!pool || !argv || argc <= 0) {
        return -1;
    }

    conn = redis_pool_acquire(pool, 0);
    if (!conn) {
        return -1;
    }

    ctx = create_cmd_ctx(pool, conn, callback, user_data);
    if (!ctx) {
        pool_release_conn(pool, conn);
        return -1;
    }

    result = redis_commandv(conn->client, argc, argv, argvlen, on_pool_command_done, ctx);
    return finish_cmd_submit(pool, conn, ctx, result);
}

int redis_pool_commandv_result(redis_pool_t *pool, int read_only,
                               int argc, const char **argv,
                               const size_t *argvlen,
                               redis_command_result_t *out) {
    redis_pool_conn_t *conn;
    int rc;

    if (!out) return TURBO_EINVAL;
    *out = (redis_command_result_t)REDIS_COMMAND_RESULT_INIT;
    if (!pool || !argv || argc <= 0) {
        out->status = TURBO_EINVAL;
        return out->status;
    }
    if (!pool->running) {
        out->status = TURBO_ENOTCONN;
        return out->status;
    }
    rc = redis_pool_acquire_ex(pool, read_only ? 1 : 0, &conn);
    if (rc != TURBO_OK) {
        out->status = rc;
        pool->stats.commands_failed++;
        return out->status;
    }
    rc = redis_commandv_result(conn->client, argc, argv, argvlen, out);
    if (out->outcome == REDIS_COMMAND_REPLIED) pool->stats.commands_sent++;
    if (rc != TURBO_OK) pool->stats.commands_failed++;
    pool_release_conn(pool, conn);
    return rc;
}

int redis_pool_read_command(redis_pool_t *pool, redis_command_cb_t callback,
                            void *user_data, const char *format, ...) {
    va_list ap;
    int result;

    if (!pool || !format) {
        return -1;
    }

    va_start(ap, format);
    result = pool_vcommand(pool, 1, callback, user_data, format, ap);
    va_end(ap);
    return result;
}

int redis_pool_set(redis_pool_t *pool, const char *key, const char *value,
                   redis_command_cb_t callback, void *user_data) {
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 0);
    pool_cmd_ctx_t *ctx;
    int result;

    if (!conn) return -1;
    ctx = create_cmd_ctx(pool, conn, callback, user_data);
    if (!ctx) {
        pool_release_conn(pool, conn);
        return -1;
    }
    result = redis_set(conn->client, key, value, on_pool_command_done, ctx);
    return finish_cmd_submit(pool, conn, ctx, result);
}

int redis_pool_get(redis_pool_t *pool, const char *key,
                   redis_command_cb_t callback, void *user_data) {
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 1);
    pool_cmd_ctx_t *ctx;
    int result;

    if (!conn) return -1;
    ctx = create_cmd_ctx(pool, conn, callback, user_data);
    if (!ctx) {
        pool_release_conn(pool, conn);
        return -1;
    }
    result = redis_get(conn->client, key, on_pool_command_done, ctx);
    return finish_cmd_submit(pool, conn, ctx, result);
}

int redis_pool_del(redis_pool_t *pool, int key_count, const char **keys,
                   redis_command_cb_t callback, void *user_data) {
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 0);
    pool_cmd_ctx_t *ctx;
    int result;

    if (!conn) return -1;
    ctx = create_cmd_ctx(pool, conn, callback, user_data);
    if (!ctx) {
        pool_release_conn(pool, conn);
        return -1;
    }
    result = redis_del(conn->client, key_count, keys, on_pool_command_done, ctx);
    return finish_cmd_submit(pool, conn, ctx, result);
}

int redis_pool_expire(redis_pool_t *pool, const char *key, int seconds,
                      redis_command_cb_t callback, void *user_data) {
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 0);
    pool_cmd_ctx_t *ctx;
    int result;

    if (!conn) return -1;
    ctx = create_cmd_ctx(pool, conn, callback, user_data);
    if (!ctx) {
        pool_release_conn(pool, conn);
        return -1;
    }
    result = redis_expire(conn->client, key, seconds, on_pool_command_done, ctx);
    return finish_cmd_submit(pool, conn, ctx, result);
}

int redis_pool_hset(redis_pool_t *pool, const char *key, const char *field,
                    const char *value, redis_command_cb_t callback, void *user_data) {
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 0);
    pool_cmd_ctx_t *ctx;
    int result;

    if (!conn) return -1;
    ctx = create_cmd_ctx(pool, conn, callback, user_data);
    if (!ctx) {
        pool_release_conn(pool, conn);
        return -1;
    }
    result = redis_hset(conn->client, key, field, value, on_pool_command_done, ctx);
    return finish_cmd_submit(pool, conn, ctx, result);
}

int redis_pool_hget(redis_pool_t *pool, const char *key, const char *field,
                    redis_command_cb_t callback, void *user_data) {
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 1);
    pool_cmd_ctx_t *ctx;
    int result;

    if (!conn) return -1;
    ctx = create_cmd_ctx(pool, conn, callback, user_data);
    if (!ctx) {
        pool_release_conn(pool, conn);
        return -1;
    }
    result = redis_hget(conn->client, key, field, on_pool_command_done, ctx);
    return finish_cmd_submit(pool, conn, ctx, result);
}

int redis_pool_xadd(redis_pool_t *pool, const char *key, size_t maxlen,
                    size_t field_count, const char **fields,
                    const char **values, const size_t *value_lens,
                    redis_command_cb_t callback, void *user_data) {
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 0);
    pool_cmd_ctx_t *ctx;
    int result;

    if (!conn) return -1;
    ctx = create_cmd_ctx(pool, conn, callback, user_data);
    if (!ctx) {
        pool_release_conn(pool, conn);
        return -1;
    }
    result = redis_xadd(conn->client, key, maxlen, field_count,
                        fields, values, value_lens, on_pool_command_done, ctx);
    return finish_cmd_submit(pool, conn, ctx, result);
}

static void on_pool_stream_done(redis_client_t *client, redis_stream_result_t *results,
                                size_t result_count, void *user_data) {
    pool_stream_ctx_t *ctx = (pool_stream_ctx_t *)user_data;
    (void)client;

    if (ctx->user_callback) {
        ctx->user_callback(client, results, result_count, ctx->user_data);
    }

    pool_release_conn(ctx->pool, ctx->conn);
    ctx->pool->stats.commands_sent++;
    object_pool_free(ctx->pool->stream_ctx_pool, ctx);
}

int redis_pool_xreadgroup(redis_pool_t *pool, const char *group,
                          const char *consumer, size_t count, int block_ms,
                          size_t stream_count, const char **keys, const char **ids,
                          redis_stream_cb_t callback, void *user_data) {
    redis_pool_conn_t *conn = redis_pool_acquire(pool, 0);
    pool_stream_ctx_t *ctx;
    int result;

    if (!conn) return -1;
    ctx = create_stream_ctx(pool, conn, callback, user_data);
    if (!ctx) {
        pool_release_conn(pool, conn);
        return -1;
    }
    result = redis_xreadgroup(conn->client, group, consumer, count, block_ms,
                              stream_count, keys, ids, on_pool_stream_done, ctx);
    return finish_stream_submit(pool, conn, ctx, result);
}

redis_pipeline_t *redis_pool_pipeline_create(redis_pool_t *pool) {
    redis_pipeline_t *pipeline;

    if (!pool) {
        return NULL;
    }

    pipeline = calloc(1, sizeof(*pipeline));
    if (!pipeline) {
        return NULL;
    }

    pipeline->pool = pool;
    pipeline->command_capacity = 32;
    pipeline->commands = calloc(pipeline->command_capacity, sizeof(*pipeline->commands));
    if (!pipeline->commands) {
        free(pipeline);
        return NULL;
    }

    return pipeline;
}

int redis_pipeline_add(redis_pipeline_t *pipeline, redis_command_cb_t callback,
                       void *user_data, const char *format, ...) {
    va_list ap;
    char *command_text;
    char *command;
    size_t command_len = 0;

    if (!pipeline || !format) {
        return -1;
    }

    va_start(ap, format);
    command_text = format_command(format, ap);
    va_end(ap);
    if (!command_text) {
        return -1;
    }

    {
        int argc = pipeline_count_args(command_text);

        if (argc <= 0) {
            tstr_free(command_text);
            return -1;
        }

        command_len = 1 + 20 + 2;
        {
            const char *p = command_text;
            while (*p) {
                while (*p == ' ') {
                    p++;
                }
                if (!*p) {
                    break;
                }
                {
                    const char *start = p;
                    while (*p && *p != ' ') {
                        p++;
                    }
                    command_len += 1 + 20 + 2;
                    command_len += (size_t)(p - start) + 2;
                }
            }
        }

        command = malloc(command_len);
        if (!command) {
            tstr_free(command_text);
            return -1;
        }

        if (pipeline_write_resp_command(command, command_len, command_text, &command_len) != 0) {
            tstr_free(command_text);
            free(command);
            return -1;
        }
    }

    tstr_free(command_text);
    return pipeline_append_encoded_command(pipeline, command, command_len, callback, user_data);
}

int redis_pipeline_addv(redis_pipeline_t *pipeline, redis_command_cb_t callback,
                        void *user_data, int argc, const char **argv,
                        const size_t *argvlen) {
    char *command;
    size_t command_len;

    if (!pipeline || argc <= 0 || !argv) {
        return -1;
    }

    command = pipeline_build_resp_commandv(argc, argv, argvlen, &command_len);
    if (!command) {
        return -1;
    }

    return pipeline_append_encoded_command(pipeline, command, command_len, callback, user_data);
}

void redis_pipeline_clear(redis_pipeline_t *pipeline) {
    pipeline_reset_commands(pipeline);
}

int redis_pipeline_execute(redis_pipeline_t *pipeline) {
    redis_client_t *client;
    int result;

    if (!pipeline || pipeline->command_count == 0) {
        return -1;
    }

    pipeline->conn = redis_pool_acquire(pipeline->pool, 0);
    if (!pipeline->conn) {
        return -1;
    }

    client = pipeline->conn->client;
    result = pipeline_execute_prepared(pipeline, client, NULL, NULL, NULL);
    if (result != 0) {
        pipeline->pool->stats.commands_failed++;
        pool_release_conn(pipeline->pool, pipeline->conn);
        pipeline->conn = NULL;
        return -1;
    }

    pipeline->pool->stats.commands_sent += pipeline->command_count;
    pool_release_conn(pipeline->pool, pipeline->conn);
    pipeline->conn = NULL;
    return 0;
}

int redis_pipeline_execute_collect(redis_pipeline_t *pipeline,
                                   redis_reply_t ***replies,
                                   size_t *reply_count) {
    redis_client_t *client;
    int result;

    if (!pipeline || !replies || !reply_count || pipeline->command_count == 0) {
        return -1;
    }

    *replies = NULL;
    *reply_count = 0;

    pipeline->conn = redis_pool_acquire(pipeline->pool, 0);
    if (!pipeline->conn) {
        return -1;
    }

    client = pipeline->conn->client;
    result = pipeline_execute_prepared(pipeline, client, replies, reply_count, NULL);

    if (result != 0) {
        pipeline->pool->stats.commands_failed++;
        pool_release_conn(pipeline->pool, pipeline->conn);
        pipeline->conn = NULL;
        return -1;
    }

    pipeline->pool->stats.commands_sent += pipeline->command_count;
    pool_release_conn(pipeline->pool, pipeline->conn);
    pipeline->conn = NULL;
    return 0;
}

int redis_pipeline_sync(redis_pipeline_t *pipeline) {
    int result = redis_pipeline_execute(pipeline);

    if (result == 0) {
        redis_pipeline_clear(pipeline);
    }

    return result;
}

int redis_pipeline_execute_result(redis_pipeline_t *pipeline,
                                  redis_pipeline_result_t *out) {
    redis_client_t *client;
    int result;

    if (!out) {
        return TURBO_EINVAL;
    }
    *out = (redis_pipeline_result_t)REDIS_PIPELINE_RESULT_INIT;
    if (!pipeline || pipeline->command_count == 0) {
        out->status = TURBO_EINVAL;
        return out->status;
    }

    pipeline->conn = redis_pool_acquire(pipeline->pool, 0);
    if (!pipeline->conn) {
        out->status = TURBO_EBUSY;
        return out->status;
    }

    client = pipeline->conn->client;
    result = pipeline_execute_prepared(pipeline, client, &out->replies,
                                       &out->reply_count, &out->outcome);
    if (result != 0) {
        pipeline->pool->stats.commands_failed++;
        out->status = out->outcome == REDIS_COMMAND_NOT_SENT
                          ? TURBO_EIO
                          : (out->outcome == REDIS_COMMAND_SEND_UNCERTAIN
                                 ? TURBO_EIO
                                 : TURBO_EPROTO);
        pool_release_conn(pipeline->pool, pipeline->conn);
        pipeline->conn = NULL;
        return out->status;
    }

    pipeline->pool->stats.commands_sent += pipeline->command_count;
    pool_release_conn(pipeline->pool, pipeline->conn);
    pipeline->conn = NULL;
    out->status = TURBO_OK;
    for (size_t i = 0; i < out->reply_count; ++i) {
        if (redis_server_error_classify(out->replies[i]) != REDIS_SERVER_ERROR_NONE) {
            out->status = TURBO_EIO;
            break;
        }
    }
    return out->status;
}

void redis_pipeline_result_clear(redis_pipeline_result_t *result) {
    if (!result) {
        return;
    }
    redis_pipeline_replies_free(result->replies, result->reply_count);
    *result = (redis_pipeline_result_t)REDIS_PIPELINE_RESULT_INIT;
}

int redis_pipeline_sync_and_return_all(redis_pipeline_t *pipeline,
                                       redis_reply_t ***replies,
                                       size_t *reply_count) {
    int result = redis_pipeline_execute_collect(pipeline, replies, reply_count);

    if (result == 0) {
        redis_pipeline_clear(pipeline);
    }

    return result;
}

#ifdef REDIS_TESTING
void redis_pool_test_set_socket_hooks(redis_pool_test_send_hook_t send_hook,
                                      redis_pool_test_recv_hook_t recv_hook,
                                      redis_pool_test_free_recv_hook_t free_recv_hook) {
    redis_pool_test_send_hook = send_hook;
    redis_pool_test_recv_hook = recv_hook;
    redis_pool_test_free_recv_hook = free_recv_hook;
}

int redis_pool_test_execute_batch(redis_client_t *client,
                                  const char **commands,
                                  size_t command_count) {
    return pipeline_execute_batch(client, commands, NULL, NULL, command_count, NULL, NULL);
}

int redis_pool_test_execute_batch_collect(redis_client_t *client,
                                          const char **commands,
                                          size_t command_count,
                                          redis_reply_t ***replies,
                                          size_t *reply_count) {
    return pipeline_execute_batch(client, commands, NULL, NULL, command_count,
                                  replies, reply_count);
}

int redis_pool_test_execute_pipeline_collect(redis_pipeline_t *pipeline,
                                             redis_client_t *client,
                                             redis_reply_t ***replies,
                                             size_t *reply_count) {
    return pipeline_execute_prepared(pipeline, client, replies, reply_count, NULL);
}

int redis_pool_test_execute_pipeline_result(redis_pipeline_t *pipeline,
                                            redis_client_t *client,
                                            redis_pipeline_result_t *out) {
    int result;
    if (!out) {
        return TURBO_EINVAL;
    }
    *out = (redis_pipeline_result_t)REDIS_PIPELINE_RESULT_INIT;
    result = pipeline_execute_prepared(pipeline, client, &out->replies,
                                       &out->reply_count, &out->outcome);
    if (result != 0) {
        out->status = out->outcome == REDIS_COMMAND_REPLY_UNKNOWN
                          ? TURBO_EPROTO
                          : TURBO_EIO;
        return out->status;
    }
    out->status = TURBO_OK;
    for (size_t i = 0; i < out->reply_count; ++i) {
        if (redis_server_error_classify(out->replies[i]) != REDIS_SERVER_ERROR_NONE) {
            out->status = TURBO_EIO;
            break;
        }
    }
    return out->status;
}
#endif

void redis_pipeline_destroy(redis_pipeline_t *pipeline) {
    if (!pipeline) {
        return;
    }

    pipeline_reset_commands(pipeline);
    free(pipeline->commands);
    free(pipeline);
}

void redis_pipeline_replies_free(redis_reply_t **replies, size_t reply_count) {
    if (!replies) {
        return;
    }

    for (size_t i = 0; i < reply_count; i++) {
        redis_reply_free(replies[i]);
    }
    free(replies);
}

void redis_pool_get_stats(redis_pool_t *pool, redis_pool_stats_t *stats) {
    if (!pool || !stats) {
        return;
    }
    refresh_pool_stats(pool);
    *stats = pool->stats;
}

void redis_pool_reset_stats(redis_pool_t *pool) {
    if (!pool) {
        return;
    }

    pool->stats.commands_sent = 0;
    pool->stats.commands_failed = 0;
    pool->stats.waiting_requests = 0;
    pool->stats.health_checks = 0;
    pool->stats.health_failures = 0;
    refresh_pool_stats(pool);
}

int redis_pool_is_healthy(redis_pool_t *pool) {
    if (!pool || !pool->running || !pool->master_pool) {
        return 0;
    }
    return coro_pool_is_open(pool->master_pool);
}

size_t redis_pool_available(redis_pool_t *pool) {
    if (!pool) {
        return 0;
    }
    refresh_pool_stats(pool);
    return pool->stats.idle_connections;
}
