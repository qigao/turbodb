# Redis capability mapping for external replicated-state adapters

Status: design/qualification mapping  
Tracking: #45, #46, #49  
Depends on: #61 capability vocabulary  
Baseline: `master@faf7ac2edb8646aa10fbf9afb8bf1c2efc74c36c`

## Purpose

Map the existing Redis ordered-apply and reconciliation primitives onto the provider-neutral local-storage capability vocabulary without changing Redis behavior or introducing TurboRaft/TurboFabric dependencies.

This document is descriptive. It does not rename or replace the public Redis API.

## Existing primitives

The current public API provides:

- `redis_lua_apply_open` for one ordered state mutation;
- `redis_lua_apply_batch_open` for one contiguous ordered batch;
- `redis_lua_apply_batch_reconcile_open` for read-only reconciliation;
- bounded batch size `REDIS_LUA_APPLY_BATCH_MAX_RECORDS = 1024`;
- explicit receipts:
  - `REDIS_LUA_APPLY_APPLIED`
  - `REDIS_LUA_APPLY_REPLAYED`
  - `REDIS_LUA_APPLY_GAP`
  - `REDIS_LUA_APPLY_CONFLICT`
  - `REDIS_LUA_APPLY_COMMIT_UNKNOWN`
  - `REDIS_LUA_APPLY_PENDING`
  - `REDIS_LUA_APPLY_ERROR`.

All keys participating in one Lua operation must use one equal, non-empty Redis Cluster hash tag so the operation stays within one Redis Cluster slot.

## Semantic receipt mapping

| Redis receipt | Generic storage semantic | Rule |
| --- | --- | --- |
| `APPLIED` | APPLIED | First successful durable application. |
| `REPLAYED` | REPLAYED | Exact logical operation/range already committed. |
| `GAP` | GAP | Durable prefix does not permit the requested next position/range. |
| `CONFLICT` | CONFLICT | Stored identity/payload or compaction facts disagree with the request. |
| `COMMIT_UNKNOWN` | COMMIT_UNKNOWN | Outcome may have committed; reconciliation is mandatory before retry. |
| `PENDING` | RETRYABLE after reconciliation only | The read-only reconcile path verified the committed prefix and exact request identity, so the same request may be retried. |
| `ERROR` | ERROR | Deterministic/local failure not represented by a stronger semantic class. |

### Important distinction: PENDING is not COMMIT_UNKNOWN

`COMMIT_UNKNOWN` never authorizes a retry by itself.

Only a later successful `redis_lua_apply_batch_reconcile_open` may return `PENDING`, and only that state authorizes retrying the exact same request.

Changing the record range, term, command id, or payload after an ambiguous outcome is not a retry and must not be accepted as one.

## Capability mapping

### ATOMIC_STATE_METADATA

**Supported by the existing ordered-apply Lua path, with Redis-specific constraints.**

The Lua scripts update durable progress metadata and the associated state/journal facts in one Redis script invocation. The batch script performs all validation that can fail before its first write and writes the metadata commit marker last.

The generic capability must not imply that every arbitrary Redis command is atomic with progress metadata. Qualification is specific to these ordered-apply primitives.

### ORDERED_REPLAY_CLASSIFICATION

**Supported.**

The existing API distinguishes:

- first application;
- exact replay;
- gap;
- conflict.

The generic adapter must preserve these classes instead of reducing them to OK/error.

### AMBIGUOUS_COMMIT

**Supported.**

Server/transport failure after command submission may be reported as `COMMIT_UNKNOWN`.

The provider must:

1. stop blind replay;
2. reconnect if needed;
3. run read-only reconciliation;
4. continue only from the resulting REPLAYED / PENDING / GAP / CONFLICT state.

### RECONCILE

**Supported for the batch/journal path.**

`redis_lua_apply_batch_reconcile_open` performs no Redis writes.

Its contract is stronger than a health check: it verifies the exact ordered request against durable metadata, identities, and stored payloads.

### BOUNDED_BATCH

**Supported.**

The public hard bound is:

```
REDIS_LUA_APPLY_BATCH_MAX_RECORDS = 1024
```

Additional admission is constrained by the connection's `max_command_bytes`, because the complete Lua command framing and request payload must fit within the configured bound.

A future machine-readable descriptor must expose both operation-count and byte-budget limits. A generic provider must not rely only on the record-count constant.

### FILE_BACKED_CHECKPOINT / STREAMING_CHECKPOINT / STAGED_RESTORE

**Not claimed by the current Redis ordered-apply API.**

Redis persistence, replication, RDB, AOF, cluster backup, or server administration features are not implicitly promoted into the TurboDB local provider contract.

If a future Redis provider needs database-scale snapshot transfer, that must be designed and qualified separately.

## Outbox boundary

The current Lua ordered-apply implementations can append Stream outbox records as part of the same Redis atomic operation.

That behavior remains Redis-specific.

The generic storage capability contract must not require an outbox. A future TurboFabric provider may:

- consume the outbox as an optional Redis-specific facility;
- ignore it when not part of the provider profile; or
- use a future outbox-neutral Redis primitive if one is added.

No existing public Redis API is changed by this mapping.

## Durable metadata and identity

The current batch path persists separate facts for:

- applied progress;
- per-position payload;
- per-position identity;
- optional outbox event.

Exact replay requires the persisted identity and payload to match.

A conflict must remain observable even when the requested logical position is at or below the current applied prefix.

This prevents an external adapter from treating “index already passed” as sufficient replay proof.

## Journal compaction

The current compaction path maintains a durable `journal_floor` and records the snapshot identity used to justify compaction.

The same semantic rules apply:

- APPLIED — compaction was first committed;
- REPLAYED — the exact compaction was already committed;
- GAP — required source range/progress is missing;
- CONFLICT — snapshot identity/range/type facts disagree;
- COMMIT_UNKNOWN — reconcile before further action;
- PENDING — reconcile verified that the exact compaction request may be retried.

Compaction does not itself provide a generic database snapshot transport capability.

## External provider admission requirements

A future TurboFabric Redis provider must reject startup unless all required Redis-side facts are available:

- ordered apply primitive;
- read-only reconcile primitive when ambiguous commits are possible;
- operation-count bound;
- command-byte bound;
- stable metadata/state key configuration;
- cluster-slot/hash-tag compatibility for all keys in one atomic operation.

There is no implicit fallback to a different Redis key layout or another storage backend.

## Compatibility requirements

#49 must preserve:

- existing public symbols and request/receipt structures;
- exact APPLIED/REPLAYED/GAP/CONFLICT/COMMIT_UNKNOWN/PENDING meanings;
- same-slot Redis Cluster validation;
- source/behavior compatibility for current Redis users;
- fail-fast reconciliation rules.

The generic mapping must not:

- rename COMMIT_UNKNOWN into a retryable transport error;
- map PENDING to success;
- hide GAP/CONFLICT;
- add TurboRaft types to Redis headers;
- require an outbox in the generic capability vocabulary;
- claim snapshot/restore capabilities that are not implemented and tested.

## Existing evidence to preserve

The current test suite already includes cases for:

- invalid/empty/non-contiguous batches rejected before I/O;
- mismatched Redis Cluster hash tags rejected before I/O;
- configured live Redis two-record apply;
- PENDING reconciliation before exact retry;
- APPLIED first application;
- REPLAYED exact replay;
- CONFLICT on changed committed payload;
- journal compaction APPLIED/REPLAYED/CONFLICT behavior;
- uint64 boundary behavior.

#49 completion should bind exact-head CI evidence to these behaviors and add only missing generic-capability assertions. Source presence alone is not acceptance.

## #49 completion criteria

- [ ] this mapping is reviewed against #46;
- [ ] existing Redis behavior remains source/ABI compatible;
- [ ] focused tests prove the semantic mapping at exact head;
- [ ] COMMIT_UNKNOWN -> reconcile -> PENDING/REPLAYED/CONFLICT/GAP behavior remains explicit;
- [ ] operation and command-byte limits are exposed through the eventual #30 capability descriptor;
- [ ] generic capability tests do not depend on backend-name switches;
- [ ] no Redis snapshot/restore capability is advertised without separate implementation and evidence.
