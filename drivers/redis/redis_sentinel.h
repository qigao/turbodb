#ifndef REDIS_SENTINEL_H
#define REDIS_SENTINEL_H

#include "redis_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct redis_sentinel {
  void *impl;
} redis_sentinel;

/** Borrowed master endpoint view, invalidated by close/destroy. */
typedef struct redis_sentinel_master {
  const char *host;
  uint16_t port;
} redis_sentinel_master;

typedef struct redis_sentinel_config {
  redis_io_runtime *runtime;
  const char **sentinel_hosts;
  const uint16_t *sentinel_ports;
  size_t sentinel_count;
  const char *service_name;
  const char *sentinel_username;
  const char *sentinel_password;
  const char *username;
  const char *password;
  int database;
  size_t connection_capacity;
  size_t address_capacity;
  size_t max_command_bytes;
  size_t initial_buffer_bytes;
  size_t max_buffer_bytes;
  size_t receive_chunk_bytes;
  size_t discovery_reply_bytes;
  uint64_t cancel_timeout_ns;
} redis_sentinel_config;

#define REDIS_SENTINEL_CONFIG_INIT                                        \
  {                                                                      \
    NULL, NULL, NULL, 0u, NULL, NULL, NULL, NULL, NULL, 0, 2u, 8u,      \
        8u * 1024u * 1024u, 4096u, 8u * 1024u * 1024u,                 \
        16u * 1024u, 64u * 1024u,                                      \
        UINT64_C(5000000000)                                             \
  }

typedef enum redis_sentinel_connect_step_kind {
  REDIS_SENTINEL_CONNECT_WAIT = 0,
  REDIS_SENTINEL_CONNECT_DONE,
  REDIS_SENTINEL_CONNECT_ERROR
} redis_sentinel_connect_step_kind;

typedef struct redis_sentinel_connect_step {
  redis_sentinel_connect_step_kind kind;
  cflow_waitable waitable;
  int status;
} redis_sentinel_connect_step;

/** Copy configuration and initialize a bounded discovery state machine. */
REDIS_API int redis_sentinel_init(redis_sentinel *sentinel,
                                  const redis_sentinel_config *config);

/**
 * Query the first configured endpoint with `SENTINEL
 * get-master-addr-by-name`, then connect the discovered master pool. WAIT
 * carries the exact underlying CFlow waitable. Discovery is startup-only; a
 * failover requires close/destroy followed by init. An ERROR with SALTS_EBUSY
 * preserves the current phase and is retryable after the current wake/driver
 * callback returns.
 */
REDIS_API redis_sentinel_connect_step redis_sentinel_connect_next(
    redis_sentinel *sentinel);

/** Open one command against the currently discovered master pool. */
REDIS_API int redis_sentinel_command_open(
    redis_sentinel *sentinel, int argc, const char **argv,
    const size_t *argvlen, size_t max_reply_bytes,
    redis_pool_stream *out_stream);

REDIS_API int redis_sentinel_get_master(const redis_sentinel *sentinel,
                                        redis_sentinel_master *master);
REDIS_API int redis_sentinel_ready(const redis_sentinel *sentinel);
REDIS_API int redis_sentinel_close(redis_sentinel *sentinel);
REDIS_API int redis_sentinel_destroy(redis_sentinel *sentinel);

#ifdef __cplusplus
}
#endif

#endif
