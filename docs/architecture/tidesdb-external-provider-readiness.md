# TidesDB readiness for external replicated-state adapters

Status: design review  
Tracking: #45, #46, #48  
Publication prerequisite: qigao/salts#308  
Baseline: `master@2e0c0d5138063ff3f23455932ad133f7cf52cef3`

## Purpose

Qualify TidesDB as a local replicated-state storage backend without adding Raft or distributed-topology concepts to TurboDB.

The local contract remains the #46 capability model. TurboFabric will combine these local facts with TurboRaft later.

## Existing facts

TidesDB already exposes:

- explicit transactions;
- transaction put/get/delete;
- transaction commit/rollback;
- `tidesdb_checkpoint(db, checkpoint_dir)`;
- a compaction pause gate used while backup/checkpoint file sets are copied;
- commit hooks that run only after WAL write, memtable application, and commit-status marking.

The commit-hook contract explicitly states that hook failure does not roll back the durable commit. Therefore commit hooks are observability/integration hooks only; they MUST NOT hold caller progress metadata that is supposed to be atomic with application state.

## Boundary decisions

### Atomic state + metadata

Provider progress metadata must be written inside the same native TidesDB transaction as application state.

For initial qualification, application state and provider metadata MUST reside in one column family unless cross-column-family commit atomicity is separately proven by tests. #48 will not infer cross-CF atomicity from the API shape.

A reserved provider-neutral key namespace may be used inside the same column family, for example:

```
application keys
__provider/meta/applied
__provider/meta/identity/<position>
```

TurboDB does not interpret these as Raft fields.

### Replay / conflict classification

Plain TidesDB transactions do not themselves provide logical replay classification.

#48 therefore needs a small local ordered-apply primitive layered on top of one TidesDB transaction:

1. read durable progress metadata;
2. compare requested logical predecessor/position;
3. verify stored identity for an already-applied position;
4. classify APPLIED / REPLAYED / GAP / CONFLICT;
5. for APPLIED, write application mutation + identity + new progress in one transaction;
6. commit once.

This local logical position is caller-owned opaque ordering metadata. It is not a Raft index in the TidesDB API.

The primitive does not claim COMMIT_UNKNOWN unless a real ambiguous native commit outcome can be observed and reconciled. Do not invent that semantic class.

## Checkpoint qualification

`tidesdb_checkpoint()` is a strong existing primitive because it produces a filesystem checkpoint while TidesDB coordinates compaction/file-set consistency.

For #48 it must be qualified as **FILE_BACKED_CHECKPOINT** only after tests prove:

- a checkpoint made during database activity opens successfully;
- all committed state at the completed checkpoint boundary is present;
- no unbounded in-memory database copy is required;
- incomplete checkpoint directories are never advertised as final checkpoints;
- ownership/cleanup of the checkpoint directory is explicit.

#48 does not initially claim STREAMING_CHECKPOINT. TurboFabric/DataStream can stream the completed checkpoint tree in bounded chunks later.

## Staged restore: generation directories

Do not atomically replace an in-use TidesDB directory tree.

Use immutable generation directories plus one small durable pointer file:

```
provider-root/
  ACTIVE
  generations/
    g-000001/
    g-000002/
    ...
```

`ACTIVE` contains only a bounded validated generation identifier.

Open flow:

1. read and validate `ACTIVE`;
2. resolve the corresponding generation directory;
3. open TidesDB there.

Restore flow:

1. allocate a fresh generation identifier;
2. build/copy the checkpoint into `generations/<new>/`;
3. open the new generation as TidesDB;
4. run validation/read probes;
5. close all validation handles;
6. create a staging ACTIVE pointer;
7. durably replace `ACTIVE` using qigao/salts#308;
8. future opens use the new generation;
9. old generations are retained until their handles are gone, then may be garbage-collected.

This converts a hard cross-platform directory-swap problem into one durable small-file publication.

## Why the ACTIVE pointer belongs in the local adapter

The pointer chooses a local durable database generation only.

It does not encode:

- leader;
- term;
- quorum;
- group;
- membership;
- shard placement.

Therefore it remains a TurboDB local-storage concern rather than a TurboFabric topology decision.

## Restore publication semantics

The result from Salts #308 must be preserved:

- NOT_PUBLISHED: the old ACTIVE generation remains authoritative;
- PUBLISHED_DURABLE: the new generation is authoritative;
- DURABILITY_UNKNOWN: do not blindly publish another generation; reconcile by reading/validating ACTIVE and the referenced generation.

A failed restore before ACTIVE publication simply deletes or quarantines the new unpublished generation according to explicit cleanup policy.

## Ordered apply metadata

The initial ordered-apply record should be backend-neutral and bounded:

- applied logical position;
- bounded operation identity/digest;
- optional format/version field.

Exact encoding is implementation detail but must be versioned and bounded.

For a request at or below the durable applied position:

- exact stored identity match => REPLAYED;
- mismatch => CONFLICT.

For a request ahead of the permitted next position => GAP.

For the exact next position, application state + identity + progress advance in one native transaction => APPLIED.

Do not invoke the TidesDB commit hook to finish or repair this metadata.

## Capability outcome

After exact-head qualification the TidesDB adapter may advertise:

- ATOMIC_STATE_METADATA;
- ORDERED_REPLAY_CLASSIFICATION;
- BOUNDED_BATCH, if explicit operation/byte limits are enforced;
- FILE_BACKED_CHECKPOINT;
- STAGED_RESTORE;
- RECONCILE only if a separate read-only reconcile operation is implemented.

It must not advertise:

- AMBIGUOUS_COMMIT without a real ambiguous outcome contract;
- STREAMING_CHECKPOINT merely because checkpoint files can later be read.

Machine-readable advertisement uses #46/#63's optional storage capability descriptor when that driver-SDK stack is available.

## Limits

The provider-facing local primitive must expose bounded limits for:

- operations per batch;
- aggregate key bytes;
- aggregate value bytes;
- provider metadata bytes;
- generation identifier bytes;
- ACTIVE pointer bytes.

The adapter rejects over-budget work before starting a native transaction.

## Tests

### Atomic/replay tests

Using one real TidesDB column family:

- first ordered batch => APPLIED;
- exact same batch => REPLAYED;
- same logical position with changed identity/payload => CONFLICT;
- future position with missing predecessor => GAP;
- application state + provider progress both survive reopen after commit;
- rollback/failure before commit exposes neither.

### Commit-hook boundary

Install a hook that deliberately fails after a native commit and prove:

- committed data remains durable;
- provider progress was already part of the transaction;
- hook failure does not alter the ordered-apply receipt.

### Checkpoint tests

- create a multi-file/multi-SSTable store;
- produce checkpoint to fresh directory;
- reopen checkpoint and verify state;
- exercise checkpoint while compaction coordination is active where deterministic;
- prove failed/incomplete checkpoint is not published as a final generation.

### Restore generation tests

- restore a valid checkpoint into a new generation;
- validate before ACTIVE publication;
- failed validation leaves old ACTIVE unchanged;
- successful durable pointer replacement switches future opens;
- DURABILITY_UNKNOWN is reconciled by reading ACTIVE;
- old open generation remains valid until its owner closes;
- garbage collection never removes an active generation.

## Non-goals

- in-place replacement of an open TidesDB directory;
- Raft-aware metadata types;
- provider auto-fallback;
- network snapshot transport;
- claiming streaming checkpoint before a bounded stream API exists;
- using post-commit hooks as durable progress storage.
