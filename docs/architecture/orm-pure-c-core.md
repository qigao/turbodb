# ORM Pure C Core Architecture

## Status

Accepted as an intentionally incompatible replacement on 2026-08-27.

## Context

The former public ORM contract was C11 but `orm_c` compiled a C++ query
planner and C++ backend classes. Consequently a nominal C library required a
C++ compiler, C++ runtime linkage, exceptions, STL allocation, and C++ driver
glue. The installed `orm.hpp` is now the only C++ implementation the package
needs.

This decision removes every `.cpp` implementation unit from `orm_c`. Source
compatibility is not preserved. Planner features without a current typed
reactive contract—joins, subqueries, grouping, aggregates, scalar expressions,
distinct, and nested boolean groups—are removed instead of being translated.

## Candidates

1. Keep the private C++ core and call it through the C ABI. This minimizes work
   but violates the requirement that the library be implemented and linked as C.
2. Mechanically translate the complete legacy planner. This preserves unused
   surface area but carries the old abstraction and ownership complexity into C.
3. Build a small C query plan and versioned backend ops around the proven cursor
   protocol. This is selected because it makes the C ABI, ownership, and driver
   boundary identical to the implementation boundary.

## Decision

The implementation has four one-way modules:

```text
public opaque C handles
    -> bounded owning C query plan
    -> versioned backend ops + opaque backend context
    -> driver row cursor / command result
    -> CBind Publisher / command Publisher
```

`orm_connection_t` owns one backend handle and immutable limits.
`orm_query_t` borrows its connection and owns copied table/SQL/column,
assignment, predicate, bind, and ordering data. `orm_transaction_t` borrows the
connection and owns an optional backend transaction handle. A successfully
opened Publisher owns its cursor, while the query, connection, and transaction (if
used) remain borrowed until Publisher destruction, matching the public contract.

Backends implement a versioned C ops table. SQL backends share one C renderer;
native backends consume the immutable C plan directly. Backend-specific public
library types never enter the core plan.

## Supported plan surface

- SELECT with explicit columns or `select_all`.
- INSERT/UPDATE assignments.
- DELETE.
- Flat AND predicates using the public comparison enum.
- One column ordering, limit, and offset.
- Raw SQL plus positional binds for SQL backends.
- Typed row Publisher and lazy command Publisher, including transaction variants.

Unsupported plan shapes fail at the API boundary; there is no compatibility
fallback and no hidden eager result.

## Data and memory protocol

- Data unit: one copied string/value in a plan, one CSerde row reader from a
  cursor, or one `orm_command_result_t`.
- Fact source: the query plan before execution and the active driver cursor
  after Publisher open.
- Ownership: strings and blobs are copied into plan-owned `tstr` values;
  `turbo_vec_t` owns fixed-size plan records whose nested `tstr` values are
  released by the query destructor.
- Topology: one connection owner and one active execution on its coroutine or
  thread domain. No internal worker threads or shared mutable global registry.
- Capacity: every vector is bounded by `orm_config_t`; copied bytes are checked
  against `max_query_bytes` or `max_parameter_bytes` before allocation.
- Backpressure: row cursor advancement occurs only under CFlow demand.
- Failure: the first status and bounded error text propagate to the public
  boundary; partially initialized owners follow one cleanup path.
- Shutdown: cancel/destroy Publishers, destroy queries/transactions, then disconnect
  the connection. Backend destroy occurs exactly once.

### TidesDB adapter protocol

- Persisted rows retain the existing `ORMTDB` version-1 binary envelope. The
  migration changes implementation language, not stored bytes.
- A materialized row owns a bounded `vec_t` of fields; every field owns its name
  and payload `tstr`. Decode commits the row only after the complete envelope,
  duplicate names, kinds, numeric encodings, and trailing bytes are validated.
- Iterator keys and values are transient borrowed views valid only until the
  next iterator movement or iterator destruction. Matching and projection
  finish before that movement. The driver owns one encoded projected row until
  the cursor calls `release_row` before the next resume.
- Scans are single-threaded and demand-driven. `max_scan_rows`,
  `max_scan_bytes`, `max_result_rows`, `max_result_bytes`, field count, and row
  bytes are independent hard limits; reaching one returns
  `ORM_STATUS_LIMIT_EXCEEDED` without eager continuation.
- Explicit transaction state retains the native transaction while a row Publisher
  is open. Commit and rollback return `ORM_STATUS_BUSY` until all transaction
  Publishers close; destruction requests a deferred rollback rather than freeing
  a native transaction still referenced by an iterator.

### Redis adapter protocol

- Data unit: one direct child of a RESP array at the Redis library boundary and
  one projected Query Engine row at the ORM cursor boundary. The response root
  is never materialized.
- The command stream owns its encoded request and sends it only on first
  demand. Each `next` parses buffered bytes first and submits another CFlow
  native receive only when the current item is incomplete.
- Native receive memory is copied into the bounded Redis receive buffer and
  immediately released. A parsed item is owned until the ORM cursor calls
  `release_row` before its next resume; CSerde views into that item are
  transient.
- One connection permits one active RESP stream in its scheduler-affine domain.
  `max_result_rows`, cumulative payload bytes, top-level item count, and
  retained unparsed network bytes are hard bounds.
- Destroying a stream before its reply is complete closes the connection and
  clears buffered bytes. Draining behind cancelled CFlow demand is forbidden:
  disconnect is required to prevent unread reply bytes from becoming the next
  command's response.
- Redis mutations remain atomic Lua commands. Transport states that may have
  reached the server propagate as an unknown mutation outcome and are never
  retried automatically.

## Build and ABI consequences

`turbo_orm` declares only C sources and `c_std_11`; its shared-library link language
must be C. `orm_cpp` remains an INTERFACE target exporting `orm.hpp` and C++17
only to C++ consumers. C++ test executables may remain because they verify the
header wrapper, but no C++ object may be linked into `turbo_orm`.

## Migration and rollback

The migration proceeds SQLite first, then PostgreSQL, MongoDB, TidesDB, and
Redis. Each backend is enabled and verified independently. Until a backend's C
adapter passes its focused suite it is excluded from that feature configuration;
there is no runtime fallback to the deleted C++ implementation.

Rollback is source-only: revert the pure-C migration patch. No persisted data or
wire format changes are introduced.

## Verification

- Configure-time assertion that `ORM_C_SOURCES` contains only `.c` paths.
- Link-language assertion through generated build metadata and a C consumer.
- Public C and C++ wrapper flow tests.
- Cursor lifecycle tests for every driver.
- SQLite raw/query/command/transaction tests first, then optional backend build
  matrices for PostgreSQL, MongoDB, TidesDB, and Redis.
- ASan development suite and `git diff --check` before completion.
