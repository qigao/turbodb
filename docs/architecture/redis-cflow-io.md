# Redis CFlow I/O Architecture

## Decision

Redis uses `TurboUtils::CFlow` as its only asynchronous execution and socket
I/O dependency. CoroNet types, socket ownership, coroutine waits, and
connection pools are removed from the Redis public and private implementation.

The public library remains pure C. C++ consumers use header-only wrappers.

## Boundaries

`redis_io_runtime` owns one explicitly selected native CFlow backend, one
bounded bridge Actor, one serial Executor, Source admission accounting, and
retiring socket identities. Each `redis_cflow_connection` owns one sequential
`cflow_source_from_io_actor()` adapter, including its CFlow-owned Actor, typed
completion slot, and socket/RESP reassembly state. Per-connection Actors submit
through fixed runtime slots to the bridge Actor, which assigns request IDs that
are unique across the shared native backend. `redis_pool` owns a fixed array of
those connections. RESP parsing remains protocol code and does not depend on
native socket APIs.

```text
Redis command Source
  -> RESP command encoder / incremental parser
  -> per-connection CFlow I/O Source owner
  -> capacity-one cflow_io_actor
  -> runtime bridge cflow_io_actor
  -> cflow_io_native_backend
  -> native socket
```

DNS is outside CFlow native I/O. `redis_cflow_connection_open()` uses the
default synchronous resolver as a control-plane step and copies at most the
configured number of native addresses. Applications must keep this call off a
latency-sensitive scheduler thread when hostname lookup may block.

## Data and Ownership Protocol

| Item | Contract |
|---|---|
| Data unit | One bounded SEND/RECV/CONNECT operation and one decoded top-level RESP item |
| Fact source | The per-connection CFlow I/O Source owns native completion state; the RESP reader owns protocol progress |
| Command bytes | Owned by the command Source until all partial SEND operations complete |
| Receive bytes | Written exclusively into a fixed-capacity connection buffer while RECV is pending; copied into the bounded parser buffer before the next operation |
| Decoded item | Owned by the consumer after an ITEM result and released with `redis_reply_free()` |
| Thread topology | Scheduler-affine connection/Source resume; native completion may arrive from a backend worker; one runtime serial Executor orders bridge and Source-owner driver tasks |
| Ordering | Exactly one native operation is active per connection; Redis command replies remain FIFO |
| Capacity | Attached Sources, pool connections, command bytes, receive bytes, decoded bytes, and top-level item count are hard limits |
| Backpressure | Full Source admission/pool capacity returns `TURBO_ENOBUFS`; no unbounded allocation or silent fallback |

## Connection Pool Lease Protocol

The pool is scheduler-affine: one executor thread owns all control-plane and
data-plane calls. It does not claim cross-thread safety. Each fixed slot follows
this state machine:

```text
CLOSED -> CONNECTING -> PREPARING -> IDLE -> BORROWED
                            ^                    |
                            |                    +-> IDLE
                            +------------------------ INVALID -> CLOSED
```

The pool is the sole owner of slot storage and each
`redis_cflow_connection`. A command stream borrows exactly one slot from
successful `redis_pool_command_open()` until its terminal step or explicit
destroy. A successful terminal reply returns a usable slot to `IDLE`; cancel,
transport/protocol failure, or an incomplete reply destroys the connection and
makes the slot `INVALID`. A borrowed connection pointer is never exposed.

Capacity is configured once and checked before allocation. When every usable
slot is borrowed, command admission returns `TURBO_ENOBUFS`; it never blocks,
queues an unbounded waiter, or allocates an overflow connection. `close()` stops
new admission and returns `TURBO_EBUSY` while leases remain. `destroy()` is only
valid after quiescence. Pool statistics expose idle, borrowed, invalid, rejected,
completed, and failed counts.

One connection-level I/O Source owns a move-only native operation from
preparation through completion encoding and Actor acknowledgement. Positive
downstream demand reserves exactly one completion; no second CONNECT, SEND, or
RECV is prepared until the first completion has been delivered. The returned
waitable is borrowed from that Source and remains valid until Source
cancellation/destruction. CFlow owner quiescence is authoritative for driver,
waker, callback, delivery, and acknowledgement completion.

## Command Source State Machine

```text
NEW
  -> CONNECT_PENDING -> PREPARE_PENDING -> SEND_PENDING
  -> RECV_PENDING -> ITEM_READY -> RECV_PENDING
  -> DONE

Any nonterminal state -> CANCELLING -> DONE
Any protocol/transport/limit failure -> ERROR
```

`resume()` never blocks. It consumes an already published completion, advances
the RESP parser, submits at most the next required native operation with
downstream demand one, and returns `CFLOW_STEP_WAIT` while completion is
outstanding. Backend drive notifications only schedule coalesced runtime tasks;
they never synchronously re-enter `cflow_io_source_owner_run_ready()`.
Completion encoding and the currently armed Source waker run outside owner
locks. A waker schedules a later resume and must not recursively resume the
same Source from the completion callback stack.

The RESP reader retains its byte offset, top-level header scan cursor,
nested-array frame stack, and partial bulk header across RECV fragments.
Appending one byte therefore advances from the last inspected position instead
of rebuilding the reply tree from byte zero. Partial trees remain bounded by
`max_reply_bytes` and are released when the stream is cancelled or destroyed.

Downstream demand counts decoded RESP values, not network fragments. No RECV
is submitted merely to read ahead after a value has been emitted.

## Cancellation and Shutdown

Cancellation follows this order:

1. Destroy/cancel the connection I/O Source and stop new operation admission.
2. Drive its owner until Actor commands, native completion, delivery, and
   acknowledgement are quiescent.
3. If any command bytes were sent and the reply is incomplete, mark the
   connection non-reusable and close it after native completion settles.
4. Close the Source owner, then release the connection lease and Source state.

Runtime shutdown stops pool admission, destroys connection Sources, closes and
forgets their socket identities, verifies that no Source remains attached,
closes the bridge Actor, and then shuts down the native backend and serial
Executor. Each Source owner destroys its own Actor and typed completion slot
before it detaches from the runtime.

## Error Semantics

- Invalid configuration and arithmetic overflow fail before resource creation.
- Source admission full, pool full, timeout, cancellation, EOF, protocol error,
  and server RESP error remain distinguishable.
- A partial SEND reports an uncertain command outcome.
- EOF before a complete reply reports an unknown reply outcome and makes the
  connection non-reusable.
- No backend, resolver, or connection strategy silently falls back.

## ORM Integration

The Redis row driver adds `WAIT` plus a `cflow_waitable`. The generic ORM row
cursor already understands `ORM_ROW_CURSOR_WAIT`, so the Redis adapter passes
the native command waitable through rather than suspending a CoroNet
coroutine. Cursor cancellation immediately cancels the Redis command Source.
`command_timeout_ms` supplies the CFlow timeout wrapper for row WAIT states;
synchronous AUTH/SELECT/control commands use one absolute monotonic deadline
across all of their network fragments.

The Redis row adapter continues to expose each row as a `cserde_reader`; CBind
constructs the typed row in caller-provided CFlow output storage. RESP and
database headers do not leak through the public ORM API.

## Migration and Rollback

The feature branch may pass through intermediate commits, but no released
target contains both reactors on one socket. The final change deletes CoroNet
entry points and package dependencies. Rollback is performed by reverting the
complete migration commit set; runtime fallback between CFlow and CoroNet is
not supported.

## Verification

- Unit: Source admission, partial SEND, fragmented RECV, WAIT/wake,
  cancellation, limits, EOF, and owner quiescence.
- Integration: local TCP fragmented RESP, AUTH/SELECT preparation, connection
  reuse, cancellation disconnect, pool saturation and recovery, static
  `CLUSTER SLOTS` routing, and startup Sentinel discovery.
- Build: Redis and ORM targets link without TurboNet; installed package can be
  consumed with `TurboUtils::CFlow` only.
- Safety: deterministic tests cover single-transfer reply ownership,
  capacity-one completion sequencing, Source admission recovery, reentrant
  blocking-call rejection, callback-quiescent cancellation, and
  byte-fragmented top-level and nested parsing; sanitizer profiles remain an
  additional CI validation layer.
