#ifndef REDIS_POOL_H
#define REDIS_POOL_H

#include "redis_cflow.h"
#include "redis_export.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Scheduler-affine, fixed-capacity pool. The structure owns an opaque impl. */
typedef struct redis_pool {
  void *impl;
} redis_pool;

/** One command stream holding exactly one pool connection lease. */
typedef struct redis_pool_stream {
  void *impl;
} redis_pool_stream;

typedef struct redis_pool_config {
  /** Borrowed runtime; it must outlive the pool and every pool stream. */
  redis_io_runtime *runtime;
  const char *host;
  uint16_t port;
  const char *username;
  const char *password;
  int database;
  int readonly;
  size_t connection_capacity;
  size_t address_capacity;
  size_t max_command_bytes;
  size_t initial_buffer_bytes;
  size_t max_buffer_bytes;
  size_t receive_chunk_bytes;
  size_t prepare_reply_bytes;
  uint64_t cancel_timeout_ns;
} redis_pool_config;

#define REDIS_POOL_CONFIG_INIT                                             \
  {                                                                       \
    NULL, "127.0.0.1", 6379u, NULL, NULL, 0, 0, 1u, 8u,                 \
        8u * 1024u * 1024u, 4096u, 8u * 1024u * 1024u,                  \
        16u * 1024u, 64u * 1024u,                                       \
        UINT64_C(5000000000)                                              \
  }

typedef enum redis_pool_connect_step_kind {
  REDIS_POOL_CONNECT_WAIT = 0,
  REDIS_POOL_CONNECT_DONE,
  REDIS_POOL_CONNECT_ERROR
} redis_pool_connect_step_kind;

typedef struct redis_pool_connect_step {
  redis_pool_connect_step_kind kind;
  cflow_waitable waitable;
  int status;
  size_t connected_connections;
} redis_pool_connect_step;

typedef struct redis_pool_stats {
  size_t connection_capacity;
  size_t idle_connections;
  size_t borrowed_connections;
  size_t invalid_connections;
  uint64_t admitted_commands;
  uint64_t rejected_commands;
  uint64_t completed_commands;
  uint64_t failed_commands;
  uint64_t cancelled_commands;
} redis_pool_stats;

/**
 * Copy configuration and allocate fixed slot storage without doing I/O.
 *
 * The pool is scheduler-affine. Every function, including stream functions,
 * must run on the same executor thread. Returns SALTS_EINVAL for invalid or
 * zero-capacity configuration and SALTS_ENOMEM on allocation failure.
 */
REDIS_API int redis_pool_init(redis_pool *pool,
                              const redis_pool_config *config);

/**
 * Advance connection and AUTH/SELECT/READONLY preparation by one step.
 *
 * On WAIT, arm the returned waitable through a CFlow run or drive the borrowed
 * runtime, then call again. During startup, DONE means every fixed slot is
 * IDLE. After a command invalidates a slot, destroy that command stream and
 * call this function again to reconnect invalid slots in place. Recovery is
 * explicit and never allocates beyond the configured capacity. EBUSY keeps
 * the current slot CONNECTING; schedule a retry after the current wake/driver
 * callback returns.
 */
REDIS_API redis_pool_connect_step redis_pool_connect_next(redis_pool *pool);

/** True only after all slots have connected and preparation has completed. */
REDIS_API int redis_pool_ready(const redis_pool *pool);

/**
 * Try to acquire one connection and open a binary-safe single-reply command.
 *
 * `argv` is copied by the RESP command source. On success, `out_stream` owns a
 * lease until terminal or destroy. When all usable slots are borrowed this
 * returns SALTS_ENOBUFS; no waiter or overflow connection is created.
 */
REDIS_API int redis_pool_command_open(redis_pool *pool, int argc,
                                      const char **argv,
                                      const size_t *argvlen,
                                      size_t max_reply_bytes,
                                      redis_pool_stream *out_stream);

/** Advance the leased command source. ITEM ownership transfers to the caller. */
REDIS_API redis_cflow_stream_step redis_pool_stream_next(
    redis_pool_stream *stream);

/** Cancel an active command, invalidate its connection, and release the lease. */
REDIS_API int redis_pool_stream_cancel(redis_pool_stream *stream);

/**
 * Destroy the command source and release its lease exactly once.
 * Safe after DONE/ERROR; active destruction performs cancellation first.
 */
REDIS_API int redis_pool_stream_destroy(redis_pool_stream *stream);

/** Stop admission and close connections; EBUSY while any stream owner lives. */
REDIS_API int redis_pool_close(redis_pool *pool);

/** Destroy a closed/quiescent pool. Active leases return SALTS_EBUSY. */
REDIS_API int redis_pool_destroy(redis_pool *pool);

REDIS_API void redis_pool_get_stats(const redis_pool *pool,
                                    redis_pool_stats *stats);

#ifdef __cplusplus
}
#endif

#endif
