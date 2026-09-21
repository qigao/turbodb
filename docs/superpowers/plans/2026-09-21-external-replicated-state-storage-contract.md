# Plan: External Replicated-State Storage Capability Contract

Tracking: #45, #46, #47, #48, #49  
Baseline: `faf7ac2edb8646aa10fbf9afb8bf1c2efc74c36c`

## Goal

Turn the reviewed capability vocabulary into a machine-readable, testable TurboDB local-storage contract, then qualify SQLite, TidesDB, and Redis independently. Keep the TurboFabric composition layer outside this repository.

## Task 1 — Review and freeze semantics (#46)

- [ ] Review `docs/architecture/external-replicated-state-storage-contract.md`.
- [ ] Confirm capability names describe local storage facts only.
- [ ] Confirm COMMIT_UNKNOWN/reconcile semantics preserve Redis behavior.
- [ ] Confirm post-commit hooks do not count as atomic progress metadata.
- [ ] Confirm checkpoint export and staged restore are separate capabilities.

No production API is added before this review closes.

## Task 2 — Join capability exposure to the runtime-driver SDK (#46, #30)

Do not create a second loader or registry.

- [ ] Add a size/versioned storage-capability descriptor to the public driver/backend descriptor defined by #30, or one extension referenced from it.
- [ ] Define capability bits plus required limits/callback invariants.
- [ ] Add contract tests for short struct, unknown version, missing callback, impossible bit combinations, zero/overflow limits, and absent optional tail fields.
- [ ] Ensure generic code never switches on backend name.
- [ ] Ensure unsupported capability requests fail explicitly with no fallback.

## Task 3 — SQLite qualification (#47)

RED first:

- [ ] prove application mutation and caller progress metadata cannot yet be qualified as one explicit provider operation;
- [ ] prove no provider-grade database-scale restore contract exists.

GREEN:

- [ ] implement one atomic transaction path for bounded state mutation + caller metadata;
- [ ] implement file-backed/bounded checkpoint export;
- [ ] implement staged restore and atomic publication;
- [ ] crash/failure tests around pre-commit, post-commit, interrupted staging, validation failure, and publication.

Do not expose Raft index/term/group types.

## Task 4 — TidesDB qualification (#48)

RED/contract:

- [ ] prove post-commit hook is not used as progress metadata durability;
- [ ] characterize existing transaction and `tidesdb_checkpoint` ownership/consistency guarantees.

GREEN:

- [ ] include caller progress metadata inside the same TidesDB transaction/batch as application mutation;
- [ ] expose exact batch limits;
- [ ] qualify checkpoint output as file-backed or streaming;
- [ ] add staged restore commit/abort semantics;
- [ ] verify replay/conflict behavior only if the backend can actually distinguish it.

## Task 5 — Redis capability mapping (#49)

Preserve the current public behavior; do not rewrite the Lua protocol merely to look generic.

- [ ] map APPLIED/REPLAYED/GAP/CONFLICT/PENDING/COMMIT_UNKNOWN to the generic semantic vocabulary;
- [ ] verify state mutation + applied metadata remain one Lua atomic transition;
- [ ] keep outbox capability separate and optional;
- [ ] document that COMMIT_UNKNOWN requires read-only reconciliation and never blind retry;
- [ ] expose exact payload/key/hash-tag limits required for admission;
- [ ] explicitly report that generic snapshot/restore capabilities are absent unless separately implemented.

## Task 6 — Cross-backend capability tests (#46)

Build a table-driven contract suite that consumes descriptors, not backend names.

Cases:

- [ ] required capability present;
- [ ] required capability absent;
- [ ] invalid descriptor bit/callback combination;
- [ ] operation/byte limit rejection;
- [ ] deterministic failure vs ambiguous commit;
- [ ] no auto-fallback to another installed backend.

## Task 7 — TurboFabric handoff

Once TurboFabric repository/provider layer exists:

- [ ] define provider profiles there as combinations of TurboDB local capabilities and TurboRaft transport/consensus requirements;
- [ ] consume the capability descriptor at provider startup;
- [ ] implement Redis replacement for TurboRaft's current Redis-specific state-machine adapter;
- [ ] run ordered apply / replay / gap / conflict / COMMIT_UNKNOWN compatibility tests;
- [ ] only then start TurboRaft #32 deprecation work.

TurboDB must remain free of TurboRaft/TurboFabric dependencies throughout.

## Evidence requirements

For each implementation PR record:

- exact source SHA;
- exact Salts/SaltsUtils pins;
- Linux/Windows configuration when applicable;
- focused test run IDs;
- full unfiltered CTest result;
- real backend version for integration tests;
- PASSED / FAILED / SKIPPED / NOT RUN distinctions.

Do not claim a capability from source presence alone.

## Merge order

```
#46 semantic contract
       |
       +----> #47 SQLite
       |
       +----> #48 TidesDB
       |
       +----> #49 Redis
       |
       +----> #30/#31 runtime descriptor exposure
                    |
                    v
             TurboFabric provider
                    |
                    v
         TurboRaft #32 deprecation
```
