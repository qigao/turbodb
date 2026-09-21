# External Replicated-State Storage Capability Contract

Status: design review  
Tracking: #45, #46, #47, #48, #49  
Baseline: `master@faf7ac2edb8646aa10fbf9afb8bf1c2efc74c36c`

## Purpose

TurboDB needs to expose stable local durability primitives that an external distributed runtime can compose without importing Raft, placement, shard, membership, or leader semantics into TurboDB.

This document defines the storage-side contract only. The component that combines these primitives with TurboRaft belongs in TurboFabric.

The boundary is:

```
TurboRaft                     TurboDB
consensus / ordering          local durable state
group / membership            transactions
replication                   replay classification
snapshot transfer             checkpoint / restore
        \                     /
         \                   /
          TurboFabric provider
```

TurboDB MUST NOT gain `leader`, `term`, `quorum`, `group_id`, `membership`, `shard`, or Raft message types.

## Design principles

1. Capabilities are explicit. Missing semantics fail at startup; there is no provider auto-fallback.
2. Application state and caller-owned durable progress metadata advance atomically when that capability is claimed.
3. Ambiguous commit results are not normalized into success or failure.
4. Replay and conflict classification remain observable where the backend can provide it.
5. Snapshot/checkpoint export is distinct from restore publication.
6. Database-scale paths must be bounded independently of database size.
7. Backend-specific strengths are preserved instead of being hidden behind a weakest-common-denominator interface.
8. TurboDB exposes local facts only. TurboFabric decides whether those facts satisfy a distributed provider profile.

## Capability vocabulary

The future machine-readable descriptor SHALL use versioned size-prefixed data and capability bits. Exact public symbol placement is intentionally coordinated with #29/#30 so TurboDB does not create a second runtime/loader registry.

Semantic capabilities:

- **ATOMIC_STATE_METADATA**  
  One durable local commit can mutate application state and caller-owned progress metadata atomically.

- **ORDERED_REPLAY_CLASSIFICATION**  
  The backend can distinguish first application from exact replay and can detect a gap or conflicting history.

- **AMBIGUOUS_COMMIT**  
  The backend can report that a request may have committed but the caller cannot yet know the outcome. This state MUST require reconciliation; blind retry is forbidden unless reconciliation proves retry is safe.

- **BOUNDED_BATCH**  
  The backend accepts an explicitly bounded mutation batch with documented operation/byte limits.

- **FILE_BACKED_CHECKPOINT**  
  A checkpoint/snapshot can be exported without materializing the complete database in memory.

- **STREAMING_CHECKPOINT**  
  Checkpoint bytes can be produced through bounded incremental reads.

- **STAGED_RESTORE**  
  Restore/install has an explicit staging phase and publishes only a complete valid state.

- **RECONCILE**  
  A read-only operation can determine whether a previously ambiguous or repeated logical operation is already committed, retryable, conflicting, or missing.

The descriptor MUST also expose limits needed for admission. A capability bit without its required limits or callbacks is invalid.

## Result semantics

Provider-facing storage code needs more information than a generic OK/error status.

The generic semantic result classes are:

- **APPLIED** — the requested mutation became durable for the first time.
- **REPLAYED** — the exact logical mutation was already durable.
- **GAP** — durable progress is behind the requested predecessor/order.
- **CONFLICT** — durable state at the requested logical position differs from the request.
- **COMMIT_UNKNOWN** — the operation may have committed; reconciliation is required.
- **RETRYABLE** — no commit occurred and retry of the exact request is safe.
- **ERROR** — deterministic local failure that is not one of the semantic classes above.

Not every backend must implement every class. A backend that cannot distinguish them MUST NOT claim `ORDERED_REPLAY_CLASSIFICATION` or `AMBIGUOUS_COMMIT`.

## Durable progress metadata

The external provider owns the meaning of progress metadata. TurboDB treats it as caller-owned local bytes/fields with these requirements:

- metadata and application mutation participate in the same durable local transition;
- metadata is not interpreted as a Raft index, term, group, or shard by TurboDB;
- a backend may impose format/size constraints, but they must be queryable;
- updating metadata in a post-commit callback does **not** satisfy `ATOMIC_STATE_METADATA`;
- hooks invoked after durability are observability/integration hooks, not atomic progress storage.

## Checkpoint and restore contract

Checkpoint export and restore publication are separate capabilities.

### Export

A provider-grade export must identify a stable point-in-time local state. It may be:

- a file/directory checkpoint owned by the caller after success; or
- a bounded read-at/stream source.

The export API must define ownership, lifetime, cleanup, and whether concurrent writes are allowed while the checkpoint is being created.

### Restore

A provider-grade restore must:

1. create or open staging state;
2. consume bounded input;
3. validate completion/integrity;
4. atomically publish or switch to the restored state;
5. discard staging state on abort/failure.

An API that writes directly into the authoritative database while bytes are still arriving does not satisfy `STAGED_RESTORE`.

## Current capability matrix

This matrix records facts at the baseline head. “Primitive” means useful building blocks exist but the provider-grade contract is not yet proven.

| Backend | Local transaction | Atomic state + caller metadata | Replay/conflict classes | Commit ambiguity | Checkpoint export | Staged restore | Qualification |
| --- | --- | --- | --- | --- | --- | --- | --- |
| SQLite ORM | yes | primitive only | no | no explicit class | missing provider-grade path | missing | #47 |
| TidesDB | yes | primitive/candidate | no generic classification | no explicit class | `tidesdb_checkpoint` exists | not yet provider-grade | #48 |
| Redis ordered apply | specialized atomic Lua path | yes for current ordered-apply metadata/state operation | APPLIED / REPLAYED / GAP / CONFLICT / PENDING | COMMIT_UNKNOWN | no generic snapshot capability claimed | no generic restore capability claimed | #49 |
| PostgreSQL ORM | yes | primitive only | no | no explicit class | no provider-grade path in current scope | no | informational / future |

### SQLite

Current ORM code exposes explicit transactions including serializable mode. This is sufficient to build an atomic state+metadata primitive, but the repository does not yet expose a dedicated caller-owned durable-progress operation or database-scale provider snapshot/restore contract.

#47 owns:

- atomic mutation + progress metadata in one SQLite transaction;
- bounded/file-backed checkpoint creation;
- staged restore with complete-state publication;
- crash tests around commit and restore boundaries.

### TidesDB

TidesDB already exposes transactions and `tidesdb_checkpoint()`. Commit hooks run after WAL write, memtable apply, and commit marking, and hook failure does not roll back the durable commit. Therefore a commit hook MUST NOT be used to implement atomic progress metadata.

#48 must qualify metadata as part of the transaction itself and establish checkpoint ownership/restore semantics. Existing checkpoint support is a strong primitive, not automatic proof of `STREAMING_CHECKPOINT` or `STAGED_RESTORE`.

### Redis

The existing Lua ordered-apply path is the strongest current replicated-state primitive.

The public receipt model already preserves:

- `REDIS_LUA_APPLY_APPLIED`
- `REDIS_LUA_APPLY_REPLAYED`
- `REDIS_LUA_APPLY_GAP`
- `REDIS_LUA_APPLY_CONFLICT`
- `REDIS_LUA_APPLY_COMMIT_UNKNOWN`
- `REDIS_LUA_APPLY_PENDING`

The operation atomically advances durable applied metadata and mutates state; the outbox is an optional Redis-specific addition and MUST NOT become a required generic capability.

#49 maps these existing facts into the generic capability vocabulary without weakening reconciliation or COMMIT_UNKNOWN semantics.

### PostgreSQL

The ORM PostgreSQL backend supports normal transaction commit/rollback and serializable transactions. No provider-grade replay classification, ambiguous-commit reconciliation contract, checkpoint export, or staged restore is currently claimed.

PostgreSQL remains transaction-capable but outside the initial R1 provider-readiness set unless a later issue explicitly adds those local primitives.

## Machine-readable exposure

TurboDB needs one capability truth source, but it must not duplicate #29/#30's runtime driver registry.

Rules:

- capability data SHALL attach to the versioned driver/backend descriptor once #30 freezes the public driver SDK, or to a single provider-neutral extension referenced from that descriptor;
- no second global registry;
- no backend-name switch in generic code;
- descriptor is size/version checked before optional fields are read;
- capability bits and callbacks/limits must be self-consistent;
- unsupported capabilities are absent, not emulated by fallback.

TurboFabric will select a provider profile by checking required capability bits and limits at startup.

## Provider profiles are external

TurboDB does not define “Raft provider” profiles.

TurboFabric may later define profiles such as:

- ordered durable state only;
- ordered state + replay reconciliation;
- ordered state + database-scale snapshot;
- full local replicated-state provider.

Those profiles are combinations of TurboDB capabilities plus TurboRaft requirements. They do not belong in this repository.

## Error and ambiguity rules

- deterministic validation failures never become COMMIT_UNKNOWN;
- pre-dispatch transport failure is retryable only when the backend contract proves nothing was submitted;
- post-dispatch loss of outcome is COMMIT_UNKNOWN when the backend cannot prove the result;
- COMMIT_UNKNOWN never permits blind re-application;
- reconciliation must be read-only;
- CONFLICT is fail-fast and must preserve enough detail for diagnosis without leaking secrets;
- provider code must not map GAP or CONFLICT to generic retry.

## Ownership and lifetime

All adapter-facing handles must document:

- borrowed vs owned storage;
- whether a handle survives connection close;
- whether close returns BUSY while a checkpoint/query/native operation is active;
- exact release responsibility on success, failure, and cancel;
- bounded memory ownership for checkpoint/export/restore paths.

This contract reuses the ownership work already tracked by #28/#29 and must not create a second independent lifetime system.

## Acceptance for #46

#46 is complete only when:

1. this capability vocabulary and matrix are reviewed;
2. each R1 backend issue (#47/#48/#49) maps its implementation to these semantic capabilities;
3. the runtime driver SDK has one machine-readable exposure point for the capabilities needed by external providers;
4. startup can reject a backend that lacks a required capability without trying another backend implicitly;
5. tests prove descriptor/limit consistency and reject invalid capability combinations;
6. no public TurboDB API contains Raft/TurboFabric topology concepts.

## Non-goals

- distributed consensus;
- leader election;
- shard placement;
- cross-backend distributed transactions;
- provider auto-selection/fallback;
- snapshot network transport;
- hot plugin upgrade;
- introducing TurboRaft or TurboFabric dependencies into TurboDB.
