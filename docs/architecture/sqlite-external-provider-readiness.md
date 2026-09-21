# SQLite readiness for external replicated-state adapters

Status: design review  
Tracking: #45, #46, #47  
Prerequisite for crash-durable publication: qigao/salts#308  
Baseline: `master@56da9f939a9e11930cfc995d62c34463203275d9`

## Purpose

Qualify SQLite as a local durable-state backend that an external composition layer can use without importing any Raft, leader, term, quorum, group, membership, shard, or placement concepts into TurboDB.

The storage contract is the one defined by #46:

- atomic application state + caller-owned durable metadata;
- bounded/file-backed checkpoint creation;
- staged restore with complete publication;
- explicit ownership/error/lifetime boundaries.

TurboFabric remains responsible for interpreting replicated commands and deciding which local capabilities are required.

## Existing facts

TurboDB already has a SQLite ORM backend with:

- explicit transactions;
- commit/rollback;
- savepoints;
- serializable isolation spelling;
- one active transaction per ORM connection;
- rollback on destruction of an active transaction.

This is sufficient to build and test **ATOMIC_STATE_METADATA** without adding a second transaction API.

What does not exist today:

- a public provider-grade SQLite checkpoint API;
- a public provider-grade staged restore API;
- a crash-durable file publication primitive in Salts;
- exact-head tests proving application state and caller progress metadata advance or roll back together.

## Boundary decision

Do not create a second SQLite connection/runtime abstraction.

The public extension belongs to the existing ORM package:

```
#include <orm/orm_sqlite.h>
target_link_libraries(app PRIVATE Orm::C)
```

The implementation may open short-lived SQLite maintenance connections internally, but no native `sqlite3 *` crosses the public ABI.

Normal application mutation remains:

```
orm_connection_t
  -> orm_transaction_begin()
  -> application mutations
  -> caller-owned metadata mutation
  -> orm_transaction_commit()
```

Checkpoint/restore maintenance is file-oriented and uses independent SQLite connections, so it does not borrow or expose the live ORM backend handle.

## Capability qualification

### ATOMIC_STATE_METADATA

Use the existing ORM transaction API.

The provider chooses its own metadata schema. TurboDB does not name or interpret fields as index/term/group.

Qualification requires tests proving:

1. application mutation + progress metadata in one transaction are both visible after commit;
2. explicit rollback exposes neither;
3. destruction/connection failure before commit does not publish one without the other;
4. metadata update is not deferred to an after-commit callback.

No new generic “replicated apply” API is required for SQLite.

### FILE_BACKED_CHECKPOINT

Add a backend-specific maintenance API based on SQLite's Online Backup API.

The Online Backup API can incrementally copy a live database into a separate file while retaining a consistent completed snapshot. It does not require whole-database memory materialization.

The API must use a caller-provided staging path and final checkpoint path:

```
source database
      |
      | sqlite3_backup_step(pages_per_step)
      v
checkpoint staging database
      |
      | validate + durable publish
      v
final checkpoint file
```

A final checkpoint path must never intentionally expose an incomplete copy.

### STREAMING_CHECKPOINT

Not claimed in #47.

A file-backed checkpoint is sufficient for the current R1 contract. A future TurboFabric DataStream layer can stream the completed checkpoint file with bounded I/O.

### STAGED_RESTORE

Restore is offline with respect to the destination database path.

The caller must ensure there is no live ORM/SQLite connection using the destination during publication. This is necessary for portable behavior, especially on Windows.

Restore flow:

```
checkpoint file
      |
      | open read-only
      | sqlite3_backup_step()
      v
restore staging database
      |
      | SQLite validation
      | close all handles
      v
crash-durable atomic replacement
      |
      v
authoritative database path
```

The restored database becomes authoritative only after the staging database is complete and validated.

## Proposed public API

Exact names may be adjusted during implementation, but the ABI shape is fixed by this design.

```c
typedef struct orm_sqlite_file_copy_config {
  uint32_t struct_size;
  uint32_t abi_version;

  orm_string_view_t source_path;
  orm_string_view_t staging_path;
  orm_string_view_t destination_path;

  uint32_t busy_timeout_ms;
  uint32_t pages_per_step;
} orm_sqlite_file_copy_config;

ORM_C_API void ORM_C_CALL
orm_sqlite_file_copy_config_init(orm_sqlite_file_copy_config *config);

ORM_C_API orm_status_t ORM_C_CALL
orm_sqlite_checkpoint_create(const orm_sqlite_file_copy_config *config,
                             orm_error_t *error);

ORM_C_API orm_status_t ORM_C_CALL
orm_sqlite_restore_publish(const orm_sqlite_file_copy_config *config,
                           orm_error_t *error);
```

The final implementation may use distinct checkpoint/restore config types if their validation rules diverge, but both remain size/versioned bounded C structs.

## Path contract

All path views:

- are borrowed only for the call;
- must be non-empty and contain no embedded NUL;
- have an explicit byte limit;
- are copied to bounded local storage before native APIs require NUL termination.

For checkpoint:

- source, staging, and final checkpoint must be distinct;
- staging and final checkpoint must be on the same filesystem for publication;
- staging must not be silently reused if stale state exists unless the documented policy explicitly removes it before starting.

For restore:

- checkpoint, staging, and destination must be distinct;
- staging and destination must be on the same filesystem;
- destination is an offline authoritative database path;
- cross-filesystem publication must fail, not degrade to copy+delete.

## Incremental backup behavior

Use `sqlite3_backup_init`, repeated bounded `sqlite3_backup_step`, and exactly one `sqlite3_backup_finish`.

`pages_per_step`:

- must be non-zero;
- bounds work performed per step;
- must not imply that total completion time is bounded;
- may encounter BUSY/LOCKED and retry only under a documented bounded policy using SQLite busy timeout.

The destination maintenance connection is never exposed to another thread/caller while backup is active.

## Validation

Before publishing a staging database:

- the backup operation must have completed successfully;
- all SQLite handles must be closed;
- the staging database must pass a bounded validation query such as `PRAGMA quick_check` under a documented result/size budget.

A failed validation leaves the staging file caller-owned/cleanup-defined and never replaces the authoritative destination.

## Durable publication prerequisite

Plain `salts_fs_rename()` is not enough for the crash-durable contract.

#47 depends on qigao/salts#308 for a primitive that distinguishes:

- not published;
- published and durable;
- publication/durability outcome uncertain.

The SQLite API must preserve that distinction instead of flattening it to generic OK/error.

Checkpoint publication and restore publication both use this primitive.

Until #308 is implemented and qualified on Linux/Windows, #47 cannot claim crash-durable staged publication complete.

## Failure semantics

Errors before namespace publication are deterministic: the final destination remains unchanged.

After the durable-publication primitive begins, the returned state must preserve whether publication is known durable or outcome is uncertain.

Do not automatically retry an uncertain replace operation.

SQLite BUSY/LOCKED during backup is distinct from file-publication uncertainty.

## Snapshot ownership

On success:

- final checkpoint/destination is caller-owned;
- staging path no longer names the published file;
- no SQLite handles remain open.

On pre-publication failure:

- final destination remains unchanged;
- staging cleanup policy is explicit; the implementation may remove a staging file only when it knows it was not published.

No API returns borrowed SQLite page memory.

## Tests

### Atomic transaction tests

Use a temporary file-backed SQLite DB with:

- application table;
- caller progress table.

Verify commit, explicit rollback, and destroy-before-commit.

Tests read state through a fresh connection after the transition.

### Checkpoint tests

- create a multi-page DB;
- mutate while checkpoint proceeds where deterministic test control permits;
- resulting final checkpoint opens and reflects one consistent completed state;
- staging/final path rules are enforced;
- no whole-database buffer allocation is used;
- interrupted/failed stage does not publish the final checkpoint.

### Restore tests

- valid checkpoint restores into staging and publishes a complete database;
- invalid/corrupt checkpoint is rejected before publication;
- publication failure leaves old destination or reports explicit uncertain state;
- destination open/in-use precondition is documented and tested where platform behavior permits;
- successful publication survives reopen and validation.

## Machine-readable capability mapping

Once #47 behavior is qualified, the SQLite runtime driver descriptor may advertise:

- `ATOMIC_STATE_METADATA`;
- `FILE_BACKED_CHECKPOINT`;
- `STAGED_RESTORE`.

It must not advertise:

- `ORDERED_REPLAY_CLASSIFICATION` unless a separate SQLite ordered-apply protocol is implemented;
- `AMBIGUOUS_COMMIT` merely because filesystem errors exist;
- `STREAMING_CHECKPOINT` for a file-backed checkpoint;
- `RECONCILE` without an actual read-only reconciliation contract.

Descriptor exposure is through #46/#63's optional storage capability table, not a second registry.

## Non-goals

- interpreting replicated commands;
- storing Raft term/index/group fields as special TurboDB concepts;
- network snapshot transport;
- streaming checkpoint API in this issue;
- online in-place restore into an active ORM connection;
- cross-filesystem replacement fallback;
- a second SQLite ORM/backend implementation;
- exposing native SQLite handles.
