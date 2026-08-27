# ORM CFlow C Core Architecture

## Status

Accepted as an intentionally incompatible replacement on 2026-08-27.

The supported execution contract is a typed CFlow Source decoded through
CSerde and CBind. The eager `orm_result_t` API, the schema-less materialized-row
Source, the header-only C chain/JPA/model facades, and their C++ result wrapper
are migration artifacts and are removed rather than deprecated. The public C++
API is allowed only as a thin owner/error wrapper over the public C reactive
API; it must not contain a second query or result implementation. Private
query-plan and driver glue may remain C++ while it is migrated independently,
but it is not a supported C++ ORM layer.

## Context

The replaced ORM exposed a C ABI while query execution and every database
backend were implemented through a C++ eager result matrix. Its second C++ ORM
layer added repositories, lazy relations, and collection semantics. Those
paths prevented database cursors from participating directly in CFlow demand,
waiting, and cancellation, so they were deleted instead of deprecated.

The remaining `orm.hpp` is an ownership and error adapter over `orm.h`; it has
no independent query planner, result type, repository, or model runtime.

## Decision

The execution path is split into four one-way layers:

```text
immutable C query plan
    -> driver cursor
    -> CFlow Source (demand / WAIT / cancellation)
    -> row-local CSerde reader
         -> typed: CBind + CMeta owning value
    -> CFlow Graph -> Sink
```

Database filtering, joins, grouping, ordering, aggregation, limit, and offset
remain query-plan operations and are pushed down when a driver can preserve
their semantics. CFlow operators transform the returned row stream; a CFlow
request count is downstream-output demand and is never rewritten as SQL LIMIT.

The public execution entry accepts a versioned CBind row configuration and
moves a driver cursor into a `cflow_source`. Successful open transfers Source
ownership to the caller; failed open leaves the output zero. Query, connection,
transaction, row descriptor, and execution-info storage are borrowed through
Source destruction.

`CSerde` is required at the driver boundary: it is the format-neutral row token
protocol. `CBind` is required by every row-producing execution because an
explicit CMeta row shape is the only supported public result contract. There is
no schema-less index-based result and no universal dynamic row fallback.

Token semantics are normalized by the backend adapter, not by CBind. CBind
therefore remains strict: a `CSERDE_STRING` is never guessed to be a number or
boolean. Backends with native value kinds map those kinds directly. PostgreSQL
uses the libpq field OID to distinguish boolean, signed integer, OID, floating
point, `bytea`, and text tokens. Redis RESP bulk strings have no scalar type, so
the internal cursor borrows the configured CMeta row shape and parses only the
projected field's declared scalar kind; RESP integers are similarly checked for
boolean and unsigned targets. Invalid or non-canonical representations fail at
the backend reader boundary instead of falling back to text. Decimal parsing
uses an immutable process-lifetime C numeric locale, so application locale
changes cannot alter database token semantics.

## Data-path protocol

- Data unit: one owning `cmeta_data_desc::storage_type` object.
- Fact source: the active driver cursor. No materialized result is authoritative
  while a cursor execution is active.
- Ownership: successful source initialization moves the cursor into the Source;
  failed initialization leaves it caller-owned. CFlow moves the Source into a
  Run. Source destruction destroys the cursor exactly once.
- Row lifetime: a cursor-produced `cserde_reader` and every transient token view
  are borrowed only through the synchronous decode/materialize call. They
  cannot be retained across `resume`, `WAIT`, callbacks, cancellation, or
  cursor advance.
- Output lifetime: successful decode constructs an owning CMeta value in CFlow's
  output storage. CFlow applies the storage type's copy/move/destroy traits.
- Topology: one producer and one CFlow Run in the connection's existing
  single-thread domain. Drivers with coroutine affinity keep polling, waking,
  cancellation, and destruction on their bound executor/event loop.
- Capacity: typed scratch bytes, nesting depth, container item count, and
  per-value buffer bytes are hard limits. One row is decoded at a time. Checked
  allocation rejects overflow; initialization allocates required scratch before
  cursor ownership moves.
- Backpressure: CFlow downstream demand controls cursor polling. `WAIT` returns
  the driver's waitable; there is no unbounded queue and no silent buffering.
- Failure: malformed rows and binding failures terminate the Source, cancel the
  cursor, restore the output to semantic zero, and surface one stable error.
- Type ownership: the backend owns native-to-CSerde token selection. The row
  descriptor remains borrowed and immutable; an optional internal cursor
  configuration hook receives it before cursor ownership moves. The common
  Source and CBind layers do not coerce backend strings.
- Shutdown: stop new demand, cancel the Run, close the Run, then destroy the
  scheduler/driver executor and borrowed graph state.

## API replacement and migration

There is no compatibility promise. `orm_query_execute`, `orm_result_*`,
`orm_chain`, JPA/model/repository helpers, and the materialized-row Source are
deleted from both headers and implementation. Existing consumers must move to
the typed Source API and run it with an explicit CFlow Graph, Scheduler, Sink,
and downstream demand.

PostgreSQL uses a pure C Adapter around libpq's async command API. Each
downstream demand advances by at most one `PGRES_SINGLE_TUPLE`. Cancellation
and every failure drain `PQgetResult` to NULL before the connection can be
reused. `bytea` text is decoded into bounded transient `CSERDE_BYTES` before
the row is published. Scalar text returned by libpq is converted only when its
field OID selects a supported exact CSerde kind. Arbitrary-precision `numeric`
and unknown OIDs remain strings rather than being narrowed silently.

MongoDB maps its native cursor to demand directly. Redis owns one bounded reply
tree returned by its client and exposes rows incrementally without copying a
second result matrix. Redis bulk-string scalar conversion is driven by the
projected CMeta field; its accepted boolean and numeric grammars are strict and
malformed data terminates the Source. TidesDB streams plans that preserve scan
order; ordering, grouping, and aggregate plans are rejected until they can be
represented by bounded CFlow stateful operators without restoring an eager
result object.

Commands are also Sources. The first demand invokes the backend exactly once
and emits one `orm_command_result_t` with `VALUE_AND_DONE`; cancellation before
demand prevents execution. PostgreSQL command completion is derived from its
cursor terminal result, while SQLite, MongoDB, TidesDB, and Redis use direct
native command paths. Redis `MULTI/EXEC` is deliberately unsupported at the ORM
transaction boundary because its command results do not exist until commit; a
commit-aware CFlow protocol is required before that feature can return.

Rollback is source-only because persisted database formats remain unchanged;
there is deliberately no runtime compatibility fallback. The row-shape hook is
an internal cursor ABI revision and does not change the public ORM ABI, query
syntax, database schema, or stored data. Rolling back the revision removes the
hook and backend token mappings together; typed PostgreSQL and Redis scalar
rows then return the former binding error rather than changing stored values.

## Verification

The adapter tests cover move-on-success, ownership retention on failure, empty,
one-row and multi-row execution, WAIT propagation, binding failure,
cancellation, exactly-once destruction, owning row copies, and bounded row
payloads. Public SQLite and TidesDB tests cover lazy command demand, affected
rows, and a subsequent typed read. PostgreSQL tests use TinyMock for the C
command/result protocol and a linked libpq fake for ABI mapping; they cover
one-result-per-demand, cancellation drain, exact result release, `bytea`,
cumulative encoded-byte limits, terminal affected rows, malformed command
counts, and a fatal error following an already-delivered row. MongoDB and Redis
cursor tests cover native error propagation and strict reply validation.
PostgreSQL flow tests additionally verify OID-selected boolean, signed,
unsigned, floating, text, and byte tokens. Redis flow tests verify schema-driven
bulk strings, shape-aware RESP integers, and rejection of non-canonical numeric
text.
