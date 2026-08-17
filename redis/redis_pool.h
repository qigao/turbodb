/**
 * @file redis_pool.h
 * @brief Redis Connection Pool for High-Throughput Scenarios
 *
 * Features:
 * - Connection pooling with configurable size
 * - Automatic connection health checks
 * - Read/write splitting (master + replicas)
 * - Pipeline batching for efficiency
 * - Thread-safe connection acquisition
 */

#ifndef REDIS_POOL_H
#define REDIS_POOL_H

#include "redis_client.h"
#include <stdint.h>
#include <stddef.h>
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct redis_pool_s redis_pool_t;

/**
 * Pool configuration
 */
typedef struct {
    /* Master connection */
    const char *master_host;
    uint16_t master_port;
    const char *username;
    const char *password;
    int database;

    /* Pool settings */
    size_t min_connections;      /**< Minimum idle connections (default: 2) */
    size_t max_connections;      /**< Maximum connections (default: 10) */
    uint32_t connect_timeout_ms; /**< Connection timeout (default: 5000) */
    uint32_t command_timeout_ms; /**< Command timeout (default: connect timeout) */
    uint32_t idle_timeout_ms;    /**< Close idle connections after (default: 60000) */
    uint32_t health_check_ms;    /**< Health check interval (default: 30000) */
    int cluster_readonly;        /**< Prepare every physical connection with READONLY */

    /* Read replicas (optional) */
    const char **replica_hosts;  /**< Array of replica hostnames */
    uint16_t *replica_ports;     /**< Array of replica ports */
    size_t replica_count;        /**< Number of replicas */

    /* Pipeline settings */
    size_t pipeline_max;         /**< Max commands before flush (default: 100) */
    uint32_t pipeline_timeout_ms;/**< Auto-flush timeout (default: 10) */
} redis_pool_config_t;

/**
 * Default configuration
 */
#define REDIS_POOL_CONFIG_DEFAULT { \
    .master_host = "127.0.0.1", \
    .master_port = 6379, \
    .username = NULL, \
    .password = NULL, \
    .database = 0, \
    .min_connections = 2, \
    .max_connections = 10, \
    .connect_timeout_ms = 5000, \
    .command_timeout_ms = 5000, \
    .idle_timeout_ms = 60000, \
    .health_check_ms = 30000, \
    .cluster_readonly = 0, \
    .replica_hosts = NULL, \
    .replica_ports = NULL, \
    .replica_count = 0, \
    .pipeline_max = 100, \
    .pipeline_timeout_ms = 10 \
}

/**
 * Pool statistics
 */
typedef struct {
    size_t total_connections;    /**< Total connections created */
    size_t active_connections;   /**< Currently in-use connections */
    size_t idle_connections;     /**< Available connections */
    size_t waiting_requests;     /**< Requests waiting for connection */
    uint64_t commands_sent;      /**< Total commands executed */
    uint64_t commands_failed;    /**< Failed commands */
    uint64_t connections_created;/**< Connections created since start */
    uint64_t connections_closed; /**< Connections closed since start */
    uint64_t health_checks;      /**< Health checks performed */
    uint64_t health_failures;    /**< Failed health checks */
} redis_pool_stats_t;

/**
 * Connection handle (for explicit acquire/release)
 */
typedef struct redis_pool_conn_s redis_pool_conn_t;

/* =============================================================================
 * Pool Lifecycle
 * =============================================================================
 */

/**
 * Create connection pool
 *
 * @param config Pool configuration
 * @return Pool instance or NULL on failure
 */
CXX_C_API redis_pool_t *redis_pool_create(const redis_pool_config_t *config);

/**
 * Start pool (creates min_connections)
 *
 * @param pool Pool instance
 * @return 0 on success, -1 on failure
 */
CXX_C_API int redis_pool_start(redis_pool_t *pool);

/**
 * Stop pool and close all connections
 *
 * @param pool Pool instance
 */
CXX_C_API void redis_pool_stop(redis_pool_t *pool);

/**
 * Destroy pool and free resources
 *
 * @param pool Pool instance
 */
CXX_C_API void redis_pool_destroy(redis_pool_t *pool);

/* =============================================================================
 * Simple Command API (auto-acquire connection)
 * =============================================================================
 */

/**
 * Execute command using pool (auto connection management)
 *
 * @param pool Pool instance
 * @param callback Response callback
 * @param user_data User data for callback
 * @param format Command format string
 * @return 0 on success, -1 on failure
 */
CXX_C_API int redis_pool_command(redis_pool_t *pool, redis_command_cb_t callback,
                       void *user_data, const char *format, ...);

/**
 * Execute command with argv
 */
CXX_C_API int redis_pool_commandv(redis_pool_t *pool, int argc, const char **argv,
                        const size_t *argvlen, redis_command_cb_t callback,
                        void *user_data);

/**
 * Execute an argv command and retain exact Redis/transport completion state.
 *
 * The pool releases its connection before returning. `out` owns the reply and
 * must be cleared with redis_command_result_clear().
 */
CXX_C_API int redis_pool_commandv_result(redis_pool_t *pool, int read_only,
                                         int argc, const char **argv,
                                         const size_t *argvlen,
                                         redis_command_result_t *out);

/**
 * Execute read-only command (may use replica)
 *
 * @param pool Pool instance
 * @param callback Response callback
 * @param user_data User data
 * @param format Command format
 * @return 0 on success
 */
CXX_C_API int redis_pool_read_command(redis_pool_t *pool, redis_command_cb_t callback,
                            void *user_data, const char *format, ...);

/* =============================================================================
 * Explicit Connection Management
 * =============================================================================
 */

/**
 * Acquire connection from pool
 *
 * @param pool Pool instance
 * @param read_only Request read-only connection (may use replica)
 * @return Connection handle or NULL if pool exhausted
 */
CXX_C_API redis_pool_conn_t *redis_pool_acquire(redis_pool_t *pool, int read_only);

/**
 * Release connection back to pool
 *
 * @param pool Pool instance
 * @param conn Connection to release
 */
CXX_C_API void redis_pool_release(redis_pool_t *pool, redis_pool_conn_t *conn);

/**
 * Get underlying redis_client from connection
 *
 * @param conn Connection handle
 * @return Redis client (do not destroy directly)
 */
CXX_C_API redis_client_t *redis_pool_conn_client(redis_pool_conn_t *conn);

/* =============================================================================
 * Pipeline API
 * =============================================================================
 */

/**
 * Pipeline handle for batching commands
 */
typedef struct redis_pipeline_s redis_pipeline_t;

/**
 * Owned completion state for one pipeline flush.
 *
 * REPLIED means every queued command has a reply. SEND_UNCERTAIN and
 * REPLY_UNKNOWN mean mutating commands must not be retried blindly. `replies`
 * stays in command order and is released by redis_pipeline_result_clear().
 */
typedef struct {
    int status;
    redis_command_outcome_t outcome;
    redis_reply_t **replies;
    size_t reply_count;
} redis_pipeline_result_t;

#define REDIS_PIPELINE_RESULT_INIT \
    { 0, REDIS_COMMAND_NOT_SENT, NULL, 0 }

/**
 * Create pipeline for batching commands
 *
 * @param pool Pool instance
 * @return Pipeline handle
 */
CXX_C_API redis_pipeline_t *redis_pool_pipeline_create(redis_pool_t *pool);

/**
 * Add command to pipeline
 *
 * @param pipeline Pipeline handle
 * @param callback Per-command callback
 * @param user_data User data
 * @param format Command format
 * @return 0 on success
 */
CXX_C_API int redis_pipeline_add(redis_pipeline_t *pipeline, redis_command_cb_t callback,
                       void *user_data, const char *format, ...);

/**
 * Add a pre-tokenized command to pipeline.
 *
 * This is binary-safe and avoids reparsing command text during execution.
 *
 * @param pipeline Pipeline handle
 * @param callback Per-command callback
 * @param user_data User data
 * @param argc Argument count
 * @param argv Argument array
 * @param argvlen Optional argument lengths
 * @return 0 on success
 */
CXX_C_API int redis_pipeline_addv(redis_pipeline_t *pipeline, redis_command_cb_t callback,
                                  void *user_data, int argc, const char **argv,
                                  const size_t *argvlen);

/**
 * Clear queued commands but keep pipeline allocated for reuse.
 *
 * @param pipeline Pipeline handle
 */
CXX_C_API void redis_pipeline_clear(redis_pipeline_t *pipeline);

/**
 * Execute all pipelined commands
 *
 * @param pipeline Pipeline handle
 * @return 0 on success
 */
CXX_C_API int redis_pipeline_execute(redis_pipeline_t *pipeline);

/**
 * Java-style alias for execute + clear on success.
 *
 * @param pipeline Pipeline handle
 * @return 0 on success
 */
CXX_C_API int redis_pipeline_sync(redis_pipeline_t *pipeline);

/**
 * Execute all pipelined commands and collect replies in-order.
 *
 * Ownership of the returned reply array and each reply transfers to the caller.
 * Free them with `redis_pipeline_replies_free()`.
 *
 * @param pipeline Pipeline handle
 * @param replies Output array of replies
 * @param reply_count Output number of replies
 * @return 0 on success
 */
CXX_C_API int redis_pipeline_execute_collect(redis_pipeline_t *pipeline,
                                             redis_reply_t ***replies,
                                             size_t *reply_count);

/** Execute a pipeline without collapsing transport uncertainty or errors. */
CXX_C_API int redis_pipeline_execute_result(redis_pipeline_t *pipeline,
                                             redis_pipeline_result_t *out);

/** Release replies owned by a pipeline result and reset it to NOT_SENT. */
CXX_C_API void redis_pipeline_result_clear(redis_pipeline_result_t *result);

/**
 * Java-style alias for execute_collect + clear on success.
 *
 * @param pipeline Pipeline handle
 * @param replies Output array of replies
 * @param reply_count Output number of replies
 * @return 0 on success
 */
CXX_C_API int redis_pipeline_sync_and_return_all(redis_pipeline_t *pipeline,
                                                 redis_reply_t ***replies,
                                                 size_t *reply_count);

/**
 * Free replies returned by `redis_pipeline_execute_collect()`.
 *
 * @param replies Reply array
 * @param reply_count Number of replies
 */
CXX_C_API void redis_pipeline_replies_free(redis_reply_t **replies, size_t reply_count);

/**
 * Destroy pipeline
 *
 * @param pipeline Pipeline handle
 */
CXX_C_API void redis_pipeline_destroy(redis_pipeline_t *pipeline);

/* =============================================================================
 * Convenience Functions
 * =============================================================================
 */

/* String commands */
CXX_C_API int redis_pool_set(redis_pool_t *pool, const char *key, const char *value,
                   redis_command_cb_t callback, void *user_data);
CXX_C_API int redis_pool_get(redis_pool_t *pool, const char *key,
                   redis_command_cb_t callback, void *user_data);
CXX_C_API int redis_pool_del(redis_pool_t *pool, int key_count, const char **keys,
                   redis_command_cb_t callback, void *user_data);
CXX_C_API int redis_pool_expire(redis_pool_t *pool, const char *key, int seconds,
                      redis_command_cb_t callback, void *user_data);

/* Hash commands */
CXX_C_API int redis_pool_hset(redis_pool_t *pool, const char *key, const char *field,
                    const char *value, redis_command_cb_t callback, void *user_data);
CXX_C_API int redis_pool_hget(redis_pool_t *pool, const char *key, const char *field,
                    redis_command_cb_t callback, void *user_data);

/* Stream commands */
CXX_C_API int redis_pool_xadd(redis_pool_t *pool, const char *key, size_t maxlen,
                    size_t field_count, const char **fields,
                    const char **values, const size_t *value_lens,
                    redis_command_cb_t callback, void *user_data);

CXX_C_API int redis_pool_xreadgroup(redis_pool_t *pool, const char *group,
                          const char *consumer, size_t count, int block_ms,
                          size_t stream_count, const char **keys, const char **ids,
                          redis_stream_cb_t callback, void *user_data);

#ifdef REDIS_TESTING
typedef int (*redis_pool_test_send_hook_t)(coro_socket_t *socket, const char *data, size_t len);
typedef int (*redis_pool_test_recv_hook_t)(coro_socket_t *socket, char **data, size_t *len);
typedef void (*redis_pool_test_free_recv_hook_t)(void *data);

CXX_C_API void redis_pool_test_set_socket_hooks(redis_pool_test_send_hook_t send_hook,
                                                redis_pool_test_recv_hook_t recv_hook,
                                                redis_pool_test_free_recv_hook_t free_recv_hook);
CXX_C_API int redis_pool_test_execute_batch(redis_client_t *client,
                                            const char **commands,
                                            size_t command_count);
CXX_C_API int redis_pool_test_execute_batch_collect(redis_client_t *client,
                                                    const char **commands,
                                                    size_t command_count,
                                                    redis_reply_t ***replies,
                                                    size_t *reply_count);
CXX_C_API int redis_pool_test_execute_pipeline_collect(redis_pipeline_t *pipeline,
                                                       redis_client_t *client,
                                                       redis_reply_t ***replies,
                                                       size_t *reply_count);
CXX_C_API int redis_pool_test_execute_pipeline_result(redis_pipeline_t *pipeline,
                                                      redis_client_t *client,
                                                      redis_pipeline_result_t *out);
#endif

/* =============================================================================
 * Statistics & Health
 * =============================================================================
 */

/**
 * Get pool statistics
 *
 * @param pool Pool instance
 * @param stats Output statistics
 */
CXX_C_API void redis_pool_get_stats(redis_pool_t *pool, redis_pool_stats_t *stats);

/**
 * Reset pool statistics
 *
 * @param pool Pool instance
 */
CXX_C_API void redis_pool_reset_stats(redis_pool_t *pool);

/**
 * Check pool health
 *
 * @param pool Pool instance
 * @return 1 if healthy, 0 if degraded
 */
CXX_C_API int redis_pool_is_healthy(redis_pool_t *pool);

/**
 * Get number of available connections
 *
 * @param pool Pool instance
 * @return Number of idle connections
 */
CXX_C_API size_t redis_pool_available(redis_pool_t *pool);

#ifdef __cplusplus
}
#endif

#endif /* REDIS_POOL_H */
