/**
 * @file redis_cluster.h
 * @brief Redis Cluster Client for Horizontal Scaling
 *
 * Features:
 * - Automatic slot routing (CRC16 hash)
 * - Topology discovery via CLUSTER SHARDS with Redis < 7 SLOTS compatibility
 * - MOVED/ASK redirection handling
 * - Per-node connection pooling
 * - Automatic failover on node failure
 */

#ifndef REDIS_CLUSTER_H
#define REDIS_CLUSTER_H

#include "redis_export.h"
#include "redis_client.h"
#include <stdint.h>
#include <stddef.h>
#include "platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Total hash slots in Redis Cluster */
#define REDIS_CLUSTER_SLOTS 16384

typedef struct redis_cluster_s redis_cluster_t;

/**
 * Cluster node info
 */
typedef struct {
    char *host;
    uint16_t port;
    char *node_id;          /**< 40-char node ID */
    int is_master;
    int slot_start;         /**< Minimum mapped slot, or -1 (masters only) */
    int slot_end;           /**< Maximum mapped slot, or -1 (masters only) */
} redis_cluster_node_t;

/**
 * Cluster configuration
 */
typedef struct {
    /* Seed nodes (at least one required) */
    const char **seed_hosts;
    uint16_t *seed_ports;
    size_t seed_count;

    /* Authentication */
    const char *username;
    const char *password;

    /* Connection settings */
    size_t connections_per_node;  /**< Pool size per routable node (default: 5) */
    uint32_t connect_timeout_ms;  /**< Connection timeout (default: 5000) */
    uint32_t command_timeout_ms;  /**< Command timeout (default: 5000) */

    /* Cluster settings */
    uint32_t topology_refresh_ms; /**< Refresh interval (default: 30000) */
    int max_redirections;         /**< Max MOVED/ASK follows (default: 5) */
    int route_reads_to_replicas;  /**< Route declared read-only commands to replicas */
} redis_cluster_config_t;

/**
 * Default cluster configuration
 */
#define REDIS_CLUSTER_CONFIG_DEFAULT { \
    .seed_hosts = NULL, \
    .seed_ports = NULL, \
    .seed_count = 0, \
    .username = NULL, \
    .password = NULL, \
    .connections_per_node = 5, \
    .connect_timeout_ms = 5000, \
    .command_timeout_ms = 5000, \
    .topology_refresh_ms = 30000, \
    .max_redirections = 5, \
    .route_reads_to_replicas = 0 \
}

/**
 * Cluster statistics
 */
typedef struct {
    size_t master_count;          /**< Number of master nodes */
    size_t replica_count;         /**< Number of replica nodes */
    size_t total_connections;     /**< Total open connections */
    uint64_t commands_sent;       /**< Total commands executed */
    uint64_t commands_failed;     /**< Failed commands */
    uint64_t redirections;        /**< MOVED/ASK redirections */
    uint64_t topology_refreshes;  /**< Topology refresh count */
} redis_cluster_stats_t;

/* =============================================================================
 * Cluster Lifecycle
 * =============================================================================
 */

/**
 * Create cluster client
 *
 * @param config Cluster configuration
 * @return Cluster instance or NULL for invalid endpoints, invalid authentication
 *         configuration, or allocation failure
 */
REDIS_API redis_cluster_t *redis_cluster_create(const redis_cluster_config_t *config);

/**
 * Connect to cluster and discover topology
 *
 * @param cluster Cluster instance
 * @return 0 on success, -1 on failure
 */
REDIS_API int redis_cluster_connect(redis_cluster_t *cluster);

/**
 * Disconnect from all nodes
 *
 * @param cluster Cluster instance
 */
REDIS_API void redis_cluster_disconnect(redis_cluster_t *cluster);

/**
 * Destroy cluster client
 *
 * @param cluster Cluster instance
 */
REDIS_API void redis_cluster_destroy(redis_cluster_t *cluster);

/**
 * Refresh cluster topology
 *
 * @param cluster Cluster instance
 * @return 0 on success, -1 on failure
 */
REDIS_API int redis_cluster_refresh(redis_cluster_t *cluster);

/* =============================================================================
 * Command Routing
 * =============================================================================
 */

/**
 * Execute command with automatic slot routing
 *
 * @param cluster Cluster instance
 * @param callback Response callback
 * @param user_data User data
 * @param format Command format (first arg should be key for routing)
 * @return 0 on success, -1 on failure
 */
REDIS_API int redis_cluster_command(redis_cluster_t *cluster, redis_command_cb_t callback,
                          void *user_data, const char *format, ...);

/**
 * Execute command with explicit key for routing
 *
 * @param cluster Cluster instance
 * @param key Key for slot calculation
 * @param callback Response callback
 * @param user_data User data
 * @param format Command format
 * @return 0 on success, -1 on failure
 */
REDIS_API int redis_cluster_command_key(redis_cluster_t *cluster, const char *key,
                              redis_command_cb_t callback, void *user_data,
                              const char *format, ...);

/**
 * Execute command with argv
 *
 * @param cluster Cluster instance
 * @param argc Argument count
 * @param argv Arguments (argv[1] used for routing if key_index < 0)
 * @param argvlen Argument lengths (optional)
 * @param key_index Index of key argument for routing (-1 to auto-detect)
 * @param callback Response callback
 * @param user_data User data
 * @return 0 on success, -1 on failure
 */
REDIS_API int redis_cluster_commandv(redis_cluster_t *cluster, int argc, const char **argv,
                           const size_t *argvlen, int key_index,
                           redis_command_cb_t callback, void *user_data);

/**
 * Execute a binary-safe cluster command and retain its exact completion state.
 *
 * MOVED replies update the reported slot and are retried at the target. ASK
 * replies issue ASKING and the command on the same physical connection without
 * changing the cached slot owner. Only explicit MOVED/ASK replies are retried;
 * uncertain sends and unknown replies are returned to the caller unchanged.
 * The call suspends the current coroutine while waiting for I/O.
 *
 * @param cluster Connected cluster client
 * @param argc Argument count
 * @param argv Argument byte strings
 * @param argvlen Optional lengths; NULL treats arguments as null-terminated
 * @param key_index Key argument used for routing; -1 selects argv[1]
 * @param out Required owned result; clear with redis_command_result_clear()
 * @return TURBO_OK for a non-error final reply, TURBO_EIO for a Redis error,
 *         TURBO_ELOOP after max redirections, or a transport/validation error
 */
REDIS_API int redis_cluster_commandv_result(redis_cluster_t *cluster, int argc,
                                  const char **argv, const size_t *argvlen,
                                  int key_index, redis_command_result_t *out);

/**
 * Execute a command explicitly declared read-only.
 *
 * The authoritative master is used unless route_reads_to_replicas is enabled.
 * When enabled, every serving shard must have an online replica and each
 * physical replica connection is prepared with READONLY before entering its
 * pool. Callers must not pass commands that can mutate Redis state.
 */
REDIS_API int redis_cluster_read_commandv(redis_cluster_t *cluster, int argc,
                                 const char **argv, const size_t *argvlen,
                                 int key_index, redis_command_cb_t callback,
                                 void *user_data);

/** Exact-result variant of redis_cluster_read_commandv(). */
REDIS_API int redis_cluster_read_commandv_result(redis_cluster_t *cluster,
                                        int argc, const char **argv,
                                        const size_t *argvlen, int key_index,
                                        redis_command_result_t *out);

/* =============================================================================
 * Convenience Functions (auto-routed by key)
 * =============================================================================
 */

/* String commands */
REDIS_API int redis_cluster_set(redis_cluster_t *cluster, const char *key, const char *value,
                      redis_command_cb_t callback, void *user_data);

REDIS_API int redis_cluster_get(redis_cluster_t *cluster, const char *key,
                      redis_command_cb_t callback, void *user_data);

REDIS_API int redis_cluster_del(redis_cluster_t *cluster, const char *key,
                      redis_command_cb_t callback, void *user_data);

REDIS_API int redis_cluster_expire(redis_cluster_t *cluster, const char *key, int seconds,
                         redis_command_cb_t callback, void *user_data);

REDIS_API int redis_cluster_incr(redis_cluster_t *cluster, const char *key,
                       redis_command_cb_t callback, void *user_data);

/* Hash commands */
REDIS_API int redis_cluster_hset(redis_cluster_t *cluster, const char *key,
                       const char *field, const char *value,
                       redis_command_cb_t callback, void *user_data);

REDIS_API int redis_cluster_hget(redis_cluster_t *cluster, const char *key,
                       const char *field, redis_command_cb_t callback,
                       void *user_data);

REDIS_API int redis_cluster_hdel(redis_cluster_t *cluster, const char *key,
                       const char *field, redis_command_cb_t callback,
                       void *user_data);

/* Stream commands */
REDIS_API int redis_cluster_xadd(redis_cluster_t *cluster, const char *key, size_t maxlen,
                       size_t field_count, const char **fields,
                       const char **values, const size_t *value_lens,
                       redis_command_cb_t callback, void *user_data);

REDIS_API int redis_cluster_xread(redis_cluster_t *cluster, const char *key,
                        size_t count, int block_ms, const char *last_id,
                        redis_stream_cb_t callback, void *user_data);

/* =============================================================================
 * Hash Tag Support
 * =============================================================================
 */

/**
 * Calculate hash slot for key
 *
 * Supports hash tags: {tag}key routes by "tag" only
 * Example: user:{123}:profile and user:{123}:settings go to same slot
 *
 * @param key Key string
 * @param len Key length
 * @return Hash slot (0-16383)
 */
REDIS_API uint16_t redis_cluster_keyslot(const char *key, size_t len);

/**
 * Get node responsible for slot
 *
 * The returned view is owned by the cluster and remains valid only until the
 * next successful topology refresh, MOVED update, disconnect, or destroy.
 *
 * @param cluster Cluster instance
 * @param slot Hash slot
 * @return Node info or NULL if unknown
 */
REDIS_API const redis_cluster_node_t *redis_cluster_get_node(redis_cluster_t *cluster,
                                                    uint16_t slot);

/* =============================================================================
 * Multi-Key Operations (same slot required)
 * =============================================================================
 */

/**
 * Delete multiple keys (must be in same slot - use hash tags)
 *
 * @param cluster Cluster instance
 * @param key_count Number of keys
 * @param keys Key array
 * @param callback Response callback
 * @param user_data User data
 * @return 0 on success, -1 if keys cross slots
 */
REDIS_API int redis_cluster_mdelete(redis_cluster_t *cluster, int key_count,
                          const char **keys, redis_command_cb_t callback,
                          void *user_data);

/**
 * Get multiple keys (must be in same slot - use hash tags)
 *
 * @param cluster Cluster instance
 * @param key_count Number of keys
 * @param keys Key array
 * @param callback Response callback
 * @param user_data User data
 * @return 0 on success, -1 if keys cross slots
 */
REDIS_API int redis_cluster_mget(redis_cluster_t *cluster, int key_count,
                       const char **keys, redis_command_cb_t callback,
                       void *user_data);

/* =============================================================================
 * Statistics & Health
 * =============================================================================
 */

/**
 * Get cluster statistics
 *
 * @param cluster Cluster instance
 * @param stats Output statistics
 */
REDIS_API void redis_cluster_get_stats(redis_cluster_t *cluster, redis_cluster_stats_t *stats);

/**
 * Reset cluster statistics
 *
 * @param cluster Cluster instance
 */
REDIS_API void redis_cluster_reset_stats(redis_cluster_t *cluster);

/**
 * Check cluster health
 *
 * @param cluster Cluster instance
 * @return 1 if all slots covered, 0 if degraded
 */
REDIS_API int redis_cluster_is_healthy(redis_cluster_t *cluster);

/**
 * Get node count
 *
 * @param cluster Cluster instance
 * @param masters Output master count (optional)
 * @param replicas Output replica count (optional)
 */
REDIS_API void redis_cluster_node_count(redis_cluster_t *cluster, size_t *masters,
                              size_t *replicas);

#ifdef __cplusplus
}
#endif

#endif /* REDIS_CLUSTER_H */
