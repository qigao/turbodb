# External Replicated-State Storage Capability Contract

Status: active contract implementation  
Tracking: #45, #46, #47, #48, #49  
Runtime publication: canonical `Salts::Plugin` + CMeta `TurboDb.Driver`

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

The machine-readable descriptor uses versioned, size-prefixed data and a storage-specific capability bitset. It is published by the canonical CMeta `TurboDb.Driver` interface; TurboDB does not create a second loader, registry, or backend-name switch.

The SQL/CRUD capability word already carried by the Driver vtable remains separate. Storage durability capabilities describe different semantics and MUST NOT be packed into the SQL capability namespace.

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

The descriptor also exposes limits needed for admission. A capability bit without the limits that apply to its execution model is invalid.

`ORM_DRIVER_STORAGE_LIMIT_CONFIGURED` means the Driver has a real finite bound, but its exact value comes from connection/operation configuration rather than from a module-wide constant. It never means unlimited.

`STAGED_RESTORE` has two valid local forms:
- a Driver that ingests restore payload chunks publishes a non-zero `max_restore_chunk_bytes`;
- a publication-only Driver accepts a pre-materialized immutable generation and publishes `max_restore_chunk_bytes == 0`, meaning no restore payload bytes cross the Driver maintenance interface.

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
2. either consume bounded input or accept a caller-owned pre-materialized immutable staging generation;
3. validate completion/integrity before authority changes;
4. atomically publish or switch to the restored state;
5. preserve explicit ownership of unpublished staging state on abort/failure.

An API that writes directly into the authoritative database while bytes are still arriving does not satisfy `STAGED_RESTORE`.

## Current capability matrix

This matrix records the currently claimed semantics. “Primitive” means useful building blocks exist but the provider-grade contract is not yet proven. A backend does not publish a capability bit until that backend's current implementation and tests qualify it.

| Backend | Local transaction | Atomic state + caller metadata | Replay/conflict classes | Commit ambiguity | Checkpoint export | Staged restore | Qualification |
| --- | --- | --- | --- | --- | --- | --- | --- |
| SQLite ORM | yes | qualified; machine-readable | no | no explicit class | qualified file-backed checkpoint | qualified staged restore | #47 |
| TidesDB | yes | qualified; machine-readable | APPLIED / REPLAYED / GAP / CONFLICT | no explicit ambiguous class | qualified file-backed checkpoint | immutable-generation ACTIVE publication | #48 / #145 |
| Redis ordered apply | specialized atomic Lua path | yes for current ordered-apply metadata/state operation | APPLIED / REPLAYED / GAP / CONFLICT / PENDING | COMMIT_UNKNOWN | no generic snapshot capability claimed | no generic restore capability claimed | machine-readable via Driver v3; #49 |
| PostgreSQL ORM | yes | primitive only | no | no explicit class | no provider-grade path in current scope | no | informational / future |

### SQLite

Current ORM code exposes explicit transactions including serializable mode. The retained provider atomicity test proves application state and caller-owned progress rows commit or roll back together across reopen. The SQLite Driver therefore publishes `ATOMIC_STATE_METADATA`; its metadata budget is connection/operation configured.

#47 is complete on current master through #143. The SQLite Driver publishes:
- `ATOMIC_STATE_METADATA`;
- `FILE_BACKED_CHECKPOINT`;
- `STAGED_RESTORE`.

The maintenance interface is a typed secondary CMeta export from the same SQLite Driver Plugin. It uses bounded Online Backup, validates staging with `quick_check`, and publishes through `salts_fs_replace_durable()`. Historical #68 remains reference material only.

### TidesDB

TidesDB already exposes transactions and `tidesdb_checkpoint()`. Commit hooks run after WAL write, memtable apply, and commit marking, and hook failure does not roll back the durable commit. Therefore a commit hook MUST NOT be used to implement atomic progress metadata.

Current retained tests qualify:
- same-column-family application state + caller metadata in one durable transaction;
- APPLIED / REPLAYED / GAP / CONFLICT classification that survives reopen and does not mutate rejected paths;
- a filesystem checkpoint that reopens at the completed checkpoint boundary and refuses to overwrite a non-empty destination.

The TidesDB Driver publishes `ATOMIC_STATE_METADATA`, `ORDERED_REPLAY_CLASSIFICATION`, and `FILE_BACKED_CHECKPOINT`. #145 adds publication-only `STAGED_RESTORE` through a typed `TurboDb.TidesMaintenance` export:
- callers pre-materialize one closed immutable generation under `provider-root/generations/<id>`;
- the Driver validates the candidate before authority changes;
- ACTIVE is published through `salts_fs_replace_durable()`;
- `DURABILITY_UNKNOWN` maps to `ORM_STATUS_COMMIT_UNKNOWN` and is reconciled by reading ACTIVE;
- the maintenance interface accepts no restore payload bytes, so `max_restore_chunk_bytes == 0`.

TidesDB still does not claim `BOUNDED_BATCH`, `AMBIGUOUS_COMMIT`, `RECONCILE`, or `STREAMING_CHECKPOINT`.

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

The current Redis Driver publishes:

- `ATOMIC_STATE_METADATA`;
- `ORDERED_REPLAY_CLASSIFICATION`;
- `AMBIGUOUS_COMMIT`;
- `BOUNDED_BATCH`;
- `RECONCILE`.

Its batch-operation ceiling is the existing `REDIS_LUA_APPLY_BATCH_MAX_RECORDS`. Batch-byte and progress-metadata limits are connection/operation configured and are reported with `ORM_DRIVER_STORAGE_LIMIT_CONFIGURED`. Redis does not claim generic checkpoint or restore capabilities.

### PostgreSQL

The ORM PostgreSQL backend supports normal transaction commit/rollback and serializable transactions. No provider-grade replay classification, ambiguous-commit reconciliation contract, checkpoint export, or staged restore is currently claimed.

PostgreSQL remains transaction-capable but outside the initial R1 provider-readiness set unless a later issue explicitly adds those local primitives.

## Machine-readable exposure

There is one publication path:

```text
Salts::Plugin
  -> reflected TurboDb.Driver contract v3
       -> storage_capabilities()
            -> orm_driver_storage_capabilities_v1
  -> orm_runtime_load_driver() validates + copies the descriptor
  -> orm_runtime_driver_storage_info() returns the cached value
```

Rules:

- `storage_capabilities()` returns module-owned borrowed static data while the Plugin admission lease is held;
- runtime admission validates the complete descriptor before registering the Driver;
- the runtime copies the descriptor, so later capability queries do not retain a raw pointer into Plugin memory;
- malformed bit/limit combinations fail admission and leave no partial registration;
- no second global registry exists;
- generic code contains no backend-name switch;
- SQL/CRUD Driver capability bits and storage durability capability bits remain separate namespaces;
- unsupported storage semantics are represented by absent bits, never by fallback or emulation;
- SQLite publishes its qualified atomic metadata, file-backed checkpoint, and staged-restore facts;
- TidesDB publishes qualified atomic metadata, ordered replay classification, file-backed checkpoint, and publication-only staged restore after #145 qualification;
- PostgreSQL, MySQL, and MongoDB publish a valid empty storage descriptor until provider-grade semantics are separately qualified;
- Redis publishes only the already-qualified ordered-apply/reconciliation semantics.

TurboFabric may select a provider profile by checking required capability bits and limits at startup. That selection policy remains outside TurboDB.

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

This contract reuses the current runtime/Plugin lease ownership model and must not create a second independent lifetime system. Historical #28/#29 material remains reference evidence only.

## Acceptance for #46

#46 is complete only when:

1. this capability vocabulary and matrix are reviewed;
2. each R1 backend issue (#47/#48/#49) maps its implementation to these semantic capabilities;
3. the runtime Driver SDK has one machine-readable CMeta exposure point for the capabilities needed by external providers;
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
