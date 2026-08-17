# Redis Sentinel Design

## Context

The Redis module supports a direct client, a static master/replica pool, and
Redis Cluster. A Sentinel deployment is different from Cluster: it preserves a
single authoritative data set and uses a service name to discover the current
master after failover. It has no hash slots or MOVED/ASK routing.

The public pool configuration stores fixed endpoints. Extending that public
structure with Sentinel fields would change its binary contract and would mix
static endpoint ownership with dynamic topology ownership.

## Decision

Sentinel support is exposed as an opaque `redis_sentinel_t` facade in
`redis_sentinel.h`. The facade owns Sentinel discovery configuration and one or
more internal `redis_pool_t` generations. Existing client, pool, and Cluster
APIs are unchanged.

Discovery follows this committed-state flow:

```text
configured Sentinels
  -> SENTINEL get-master-addr-by-name
  -> connect to the reported Redis endpoint
  -> require ROLE == master
  -> start and validate a candidate pool generation
  -> atomically publish the candidate
  -> retire and drain the previous generation
```

The first configured Sentinel is tried first. After a successful discovery,
that Sentinel becomes the preferred first endpoint for later refreshes. A
connection failure, malformed response, failed authentication, invalid port,
failed pool startup, or non-master ROLE rejects only that candidate and moves
to the next configured Sentinel.

## Considered Alternatives

### Add fields and a mode flag to `redis_pool_config_t`

Rejected because the configuration structure is public, has no size/version
field, and represents a fixed topology. Appending fields would risk ABI
incompatibility with already-compiled callers and would make the pool own both
data connections and Sentinel control-plane state.

### Reuse `redis_cluster_t`

Rejected because Cluster owns a slot map and server-directed MOVED/ASK state.
Sentinel owns one service-name-to-master mapping. Sharing that public type would
couple unrelated command routing and error semantics.

### Mutate an existing pool endpoint in place

Rejected because idle and borrowed sockets can belong to different endpoints
during failover. In-place mutation cannot state which endpoint a borrowed
socket belongs to and makes safe destruction dependent on timing.

## Ownership And State

`redis_sentinel_t` is the sole owner of copied configuration, topology state,
statistics, the current generation, and the retired-generation list. It is
affine to one CoroNet event loop and is not thread-safe.

Each generation owns one `redis_pool_t`, a copied host, and its port. Commands
retain a generation lease before borrowing a pool connection and release the
lease after returning the connection. A topology switch performs:

```text
CURRENT -> RETIRED -> DESTROYED
              |
              +-- remains alive while active_leases > 0
```

Retiring a generation stops new pool borrows immediately. CoroNet keeps already
borrowed sockets valid until return; the Sentinel generation additionally keeps
the Redis pool wrappers and configuration alive. Destruction occurs only after
the last lease is released.

Control-plane operations are cooperative within the event loop. Only one
refresh may be active. A control epoch prevents a refresh that was suspended in
network I/O from committing after disconnect or destroy.

## Error And Retry Semantics

A failed discovery never publishes a partial candidate and never modifies the
current generation. If a previously validated generation exists, commands may
continue using it; otherwise operations return `TURBO_ENOTCONN`.

Command retry is based on `redis_command_result_t`:

| Outcome | Refresh topology | Automatic retry after a switch |
|---------|------------------|--------------------------------|
| `REDIS_COMMAND_NOT_SENT` | Yes | Once |
| `READONLY` reply | Yes | Once |
| `MASTERDOWN` reply | Yes | Once |
| `REDIS_COMMAND_SEND_UNCERTAIN` | Yes | Never |
| `REDIS_COMMAND_REPLY_UNKNOWN` | Yes | Never |

An uncertain write may already have committed, so the original exact result is
returned unchanged even when refresh successfully installs a new master.

## Authentication And Validation

Sentinel ACL credentials and Redis data-node credentials are separate. Sentinel
connections authenticate only to issue discovery commands. Candidate data
connections use the configured Redis ACL and database. Every reported endpoint
must answer `ROLE` as `master` before it can become current.

External responses are bounded to a 255-byte endpoint and a valid non-zero
16-bit port. Unknown services, null replies, unexpected RESP shapes, and role
mismatches fail closed.

## Compatibility And Migration

Existing standalone pool and Cluster users require no migration. Sentinel users
create `redis_sentinel_config_t`, call `redis_sentinel_connect()` inside a
CoroNet coroutine, and issue binary-safe commands through
`redis_sentinel_commandv_result()` or its callback variant.

Removing Sentinel support is a source-level rollback: remove the new source,
header, test target, and documentation entries. It does not require a data or
configuration migration and does not change existing persisted formats.

## Scope And Validation

The initial implementation is master-only. It intentionally excludes
`SENTINEL replicas` read routing and `+switch-master` Pub/Sub subscriptions.
Those features add replica-staleness policy and long-lived subscription
shutdown state and require separate design decisions.

Protocol tests use in-process CoroNet servers and cover unavailable Sentinel
fallback, stale Sentinel role rejection, candidate publication, explicit and
failure-triggered failover, old-generation drain, callback routing, safe retry,
and preservation of uncertain writes. Adjacent Redis client, pool, and Cluster
tests protect existing behavior.
