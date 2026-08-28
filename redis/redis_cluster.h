#ifndef REDIS_CLUSTER_H
#define REDIS_CLUSTER_H

#include "redis_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REDIS_CLUSTER_SLOT_COUNT 16384u

typedef struct redis_cluster {
  void *impl;
} redis_cluster;

/** Borrowed topology view, invalidated by close/destroy. */
typedef struct redis_cluster_node {
  const char *host;
  uint16_t port;
  const char *node_id;
  uint16_t slot_start;
  uint16_t slot_end;
} redis_cluster_node;

typedef struct redis_cluster_config {
  redis_io_runtime *runtime;
  const char **seed_hosts;
  const uint16_t *seed_ports;
  size_t seed_count;
  const char *username;
  const char *password;
  size_t connections_per_node;
  size_t max_nodes;
  size_t address_capacity;
  size_t max_command_bytes;
  size_t initial_buffer_bytes;
  size_t max_buffer_bytes;
  size_t receive_chunk_bytes;
  size_t topology_reply_bytes;
  uint64_t cancel_timeout_ns;
} redis_cluster_config;

#define REDIS_CLUSTER_CONFIG_INIT                                         \
  {                                                                      \
    NULL, NULL, NULL, 0u, NULL, NULL, 2u, 64u, 8u,                      \
        8u * 1024u * 1024u, 4096u, 8u * 1024u * 1024u, 16u * 1024u,    \
        4u * 1024u * 1024u,                                             \
        UINT64_C(5000000000)                                             \
  }

typedef enum redis_cluster_connect_step_kind {
  REDIS_CLUSTER_CONNECT_WAIT = 0,
  REDIS_CLUSTER_CONNECT_DONE,
  REDIS_CLUSTER_CONNECT_ERROR
} redis_cluster_connect_step_kind;

typedef struct redis_cluster_connect_step {
  redis_cluster_connect_step_kind kind;
  cflow_waitable waitable;
  int status;
  size_t node_count;
} redis_cluster_connect_step;

/** Copy seeds and allocate bounded node/slot storage without performing I/O. */
REDIS_API int redis_cluster_init(redis_cluster *cluster,
                                 const redis_cluster_config *config);

/**
 * Discover one immutable `CLUSTER SLOTS` snapshot from the first seed, then
 * connect every discovered primary pool. WAIT carries the exact underlying
 * CFlow waitable. A changed topology requires close/destroy followed by init;
 * command streams report MOVED/ASK as ordinary Redis server errors. An ERROR
 * with TURBO_EBUSY preserves the current phase and is retryable after the
 * current wake/driver callback returns.
 */
REDIS_API redis_cluster_connect_step redis_cluster_connect_next(
    redis_cluster *cluster);

/**
 * Route one binary-safe command by key and open a pool-backed command stream.
 * The returned stream follows `redis_pool_stream_*` ownership rules.
 */
REDIS_API int redis_cluster_command_open(
    redis_cluster *cluster, const char *key, size_t key_length, int argc,
    const char **argv, const size_t *argvlen, size_t max_reply_bytes,
    redis_pool_stream *out_stream);

REDIS_API uint16_t redis_cluster_keyslot(const char *key, size_t length);
REDIS_API const redis_cluster_node *redis_cluster_node_for_slot(
    const redis_cluster *cluster, uint16_t slot);
REDIS_API int redis_cluster_ready(const redis_cluster *cluster);
REDIS_API int redis_cluster_close(redis_cluster *cluster);
REDIS_API int redis_cluster_destroy(redis_cluster *cluster);

#ifdef __cplusplus
}
#endif

#endif
