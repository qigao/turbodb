# TurboDB Redis

TurboDB Redis is a pure C, incremental RESP client built on
`TurboUtils::CFlow`. It does not depend on TurboNet or expose coroutine/socket
types. C++ consumers use the same C headers or header-only wrappers supplied by
their application.

## Public layers

- `redis_io.h`: shared native CFlow backend and per-Publisher admission runtime.
- `redis_cflow.h`: one connection plus single-reply or top-level array streams.
- `redis_pool.h`: fixed-capacity scheduler-affine connection pool.
- `redis_cluster.h`: `CLUSTER SLOTS` discovery and CRC16 slot routing.
- `redis_sentinel.h`: Sentinel master discovery and bounded master pool.
- `redis_reply.h`: owned RESP values and server-error classification.

All `*_next()` data-plane calls are nonblocking state-machine steps. A `WAIT`
result carries the exact `cflow_waitable` to arm in a CFlow run. The default
hostname resolver and `redis_io_runtime_wait_idle()` are synchronous
control-plane helpers; do not call them on a latency-sensitive scheduler
thread.

Each connection owns one sequential `cflow_publisher_from_io_actor()` adapter.
Its downstream demand is one native CONNECT/SEND/RECV completion at a time;
the runtime owns a bounded bridge Actor and serial Executor in front of the
shared native backend, assigns backend-wide request identities, and bounds
attached Publishers.

## Standalone command

```c
#include <redis_cflow.h>
#include <turbo_error.h>

#include <stdint.h>

int ping(redis_io_runtime *runtime) {
  const char *argv[] = {"PING"};
  redis_cflow_connection connection = {0};
  redis_cflow_open_config open = {
      runtime, "127.0.0.1", 6379u, 8u, 8u * 1024u * 1024u, 4096u,
      8u * 1024u * 1024u, 16u * 1024u, UINT64_C(5000000000)};
  redis_cflow_stream stream = {0};
  redis_cflow_connect_step connected;
  redis_cflow_stream_step step;
  int status = redis_cflow_connection_open(&connection, &open);
  if (status != TURBO_OK) return status;

  do {
    connected = redis_cflow_connection_connect_next(&connection);
    if (connected.kind == REDIS_CFLOW_CONNECT_WAIT) {
      status = redis_io_runtime_wait_idle(runtime, UINT64_C(5000000000));
      if (status != TURBO_OK) goto cleanup_connection;
    }
  } while (connected.kind == REDIS_CFLOW_CONNECT_WAIT);
  if (connected.kind == REDIS_CFLOW_CONNECT_ERROR) {
    status = connected.status;
    goto cleanup_connection;
  }
  status = redis_cflow_command_open(&connection, 1, argv, NULL, 64u * 1024u,
                                    &stream);
  if (status != TURBO_OK) goto cleanup_connection;
  do {
    step = redis_cflow_stream_next(&stream);
    if (step.kind == REDIS_CFLOW_STREAM_WAIT) {
      status = redis_io_runtime_wait_idle(runtime, UINT64_C(5000000000));
      if (status != TURBO_OK) goto cleanup_stream;
    }
  } while (step.kind == REDIS_CFLOW_STREAM_WAIT);
  if (step.kind == REDIS_CFLOW_STREAM_ITEM) {
    redis_reply_free(step.item);
    status = TURBO_OK;
  } else {
    redis_reply_free(step.item);
    status = step.status;
  }

cleanup_stream:
  (void)redis_cflow_stream_destroy(&stream);
cleanup_connection:
  (void)redis_cflow_connection_destroy(&connection);
  return status;
}
```

The blocking example advances only after `redis_io_runtime_wait_idle()`. In a
reactive driver, a wake callback schedules the next step instead of calling it
inline. If a connection, pool, Cluster, or Sentinel connect step reports
`ERROR` with `TURBO_EBUSY`, its phase is preserved: retry after the current
wake/driver callback returns. Other `ERROR` statuses are terminal.

## Pool, Cluster, and Sentinel ownership

The pool owns a fixed array of connections. A successful
`redis_pool_command_open()` transfers one slot lease to `redis_pool_stream`.
The lease is returned only after `DONE`/`ERROR` or explicit destroy. Saturation
returns `TURBO_ENOBUFS`; there is no hidden queue or overflow allocation.
After a transport failure or cancellation, destroy the terminal command stream
and drive `redis_pool_connect_next()` again to rebuild invalid slots in place.

`redis_cluster_connect_next()` queries `CLUSTER SLOTS`, requires complete slot
coverage, and connects one bounded pool per discovered primary. Commands use
`redis_cluster_command_open()` with an explicit routing key. The topology is an
immutable startup snapshot: `MOVED`/`ASK` remain visible server errors, and a
topology change is applied by closing, destroying, and initializing a new
cluster facade.

`redis_sentinel_connect_next()` queries
`SENTINEL get-master-addr-by-name`, closes the discovery connection, and then
connects the bounded master pool. It queries the first configured Sentinel and
does not subscribe to failover events; rediscovery uses a new Sentinel facade.
Commands use `redis_sentinel_command_open()`.

These objects and their streams are scheduler-affine. Stop admission, destroy
all command streams, close the facade, then destroy it. `close()` returns
`TURBO_EBUSY` while a pool lease is still active.

## Error and reply semantics

- `ITEM` transfers an owned `redis_reply_t`; call `redis_reply_free()` once.
- RESP server errors produce `ERROR` with `outcome == REDIS_COMMAND_REPLIED`, a
  classified `server_error`, and the owned error reply in `item`.
- Partial send, incomplete reply, cancellation, protocol failure, capacity
  exhaustion, and shutdown remain distinguishable.
- `max_command_bytes` bounds the encoded outbound command; oversize commands
  fail before allocation. `max_buffer_bytes` independently bounds incremental
  reply buffering.
- RESP parsing preserves the top-level header cursor, nested frame stack, and
  byte offset across network fragments; incomplete replies are not reparsed
  from the beginning.
- Cancelling an incomplete command makes its connection non-reusable.

## Build dependency

Link `TurboDB::Redis`. Its public first-party dependencies are
`TurboUtils::Core` and `TurboUtils::CFlow`; `find_package(TurboNet)` is not
required.
