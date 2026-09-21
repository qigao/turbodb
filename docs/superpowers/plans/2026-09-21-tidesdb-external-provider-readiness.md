# Plan: TidesDB external-provider readiness

Tracking: #48  
Contract: #46  
Durable pointer publication: qigao/salts#308

## Task 1 — Prove transaction scope

- [ ] add real TidesDB test with application key + provider metadata key in one column family;
- [ ] commit and reopen: both new values visible;
- [ ] rollback and reopen: neither new value visible;
- [ ] do not claim multi-column-family atomicity unless separately proven.

## Task 2 — Prove commit-hook boundary

- [ ] install a failing commit hook;
- [ ] commit data + provider metadata inside the transaction;
- [ ] verify hook runs only after durable commit;
- [ ] verify hook failure does not roll back committed state;
- [ ] document that hooks cannot implement ATOMIC_STATE_METADATA.

## Task 3 — Ordered apply classification

TDD first:

- [ ] APPLIED for the exact next logical batch;
- [ ] REPLAYED for an already-committed exact identity;
- [ ] CONFLICT for same position with changed identity/payload;
- [ ] GAP for missing predecessor;
- [ ] bounded operation/key/value/metadata budgets checked before transaction begin.

Implementation stores provider progress and identity as normal TidesDB keys in the same transaction as application mutation.

No Raft-specific field names/types in public API.

## Task 4 — Checkpoint qualification

- [ ] characterize `tidesdb_checkpoint()` ownership and destination requirements;
- [ ] build a multi-file store;
- [ ] create checkpoint into a fresh directory;
- [ ] reopen the checkpoint and verify committed state;
- [ ] prove checkpoint does not require O(database size) memory;
- [ ] prove incomplete checkpoint directories are not treated as published generations.

Initial capability: FILE_BACKED_CHECKPOINT only.

## Task 5 — Generation-root local layout

Implement a provider-local root contract:

```
ACTIVE
generations/<id>/
```

- [ ] bounded generation IDs;
- [ ] safe path construction/no traversal;
- [ ] ACTIVE parser rejects malformed/trailing data;
- [ ] open resolves only the generation named by ACTIVE;
- [ ] old generation handles remain valid after ACTIVE changes.

## Task 6 — Staged restore

- [ ] create fresh generation directory;
- [ ] populate from checkpoint;
- [ ] open and validate new TidesDB generation;
- [ ] close validation handles;
- [ ] write staging ACTIVE pointer;
- [ ] publish pointer through Salts #308;
- [ ] preserve NOT_PUBLISHED / PUBLISHED_DURABLE / DURABILITY_UNKNOWN exactly;
- [ ] reconcile unknown publication by re-reading ACTIVE.

## Task 7 — Garbage collection

- [ ] never remove current ACTIVE generation;
- [ ] never remove a generation with retained/live handles;
- [ ] safely delete known inactive generations;
- [ ] interruption leaves at worst extra inactive generations, not missing authoritative state.

## Task 8 — Capability descriptor

After behavior is green:

- [ ] ATOMIC_STATE_METADATA;
- [ ] ORDERED_REPLAY_CLASSIFICATION;
- [ ] BOUNDED_BATCH with truthful limits;
- [ ] FILE_BACKED_CHECKPOINT;
- [ ] STAGED_RESTORE;
- [ ] RECONCILE only if implemented read-only;
- [ ] do not advertise AMBIGUOUS_COMMIT or STREAMING_CHECKPOINT without evidence.

## Platform qualification

Record exact TurboDB/Salts/vcpkg heads and full applicable test results on Linux and Windows.

#48 remains open until ordered apply, checkpoint, staged restore, durable ACTIVE publication, and capability advertisement are all exact-head qualified.
