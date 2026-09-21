# Plan: SQLite external-provider readiness

Tracking: #47  
Contract: #46  
Durable publication prerequisite: qigao/salts#308  
Baseline: `56da9f939a9e11930cfc995d62c34463203275d9`

## Goal

Qualify SQLite for the local-storage subset needed by a future TurboFabric provider while reusing existing ORM transactions and adding only file-oriented checkpoint/restore maintenance.

## Task 1 — Transaction qualification RED -> GREEN

No production API change.

Add a focused SQLite test that creates:

- an application-state table;
- a caller-progress table.

RED/verification cases:

- [ ] commit updates both together;
- [ ] explicit rollback leaves both unchanged;
- [ ] destroying an active transaction rolls back both;
- [ ] reopening the DB from a fresh connection observes the same atomic result.

The test must use public ORM transaction/query APIs rather than internal `sqlite3 *`.

## Task 2 — Public SQLite maintenance header

Add `orm/include/orm/orm_sqlite.h`.

- [ ] size/versioned bounded config;
- [ ] borrowed path views copied/validated inside the call;
- [ ] busy timeout + pages-per-step bounds;
- [ ] no native handle exposure;
- [ ] install with the existing `Orm::C` public headers;
- [ ] C and C++ include/layout tests.

Do not create a new loader/registry or second SQLite connection abstraction.

## Task 3 — Checkpoint staging

TDD first.

- [ ] RED: final checkpoint must not appear when copy fails before completion.
- [ ] open source read-only and staging read/write-create using independent SQLite handles.
- [ ] use `sqlite3_backup_init/step/finish`.
- [ ] step in bounded page chunks.
- [ ] configure busy timeout.
- [ ] close all handles before publication.
- [ ] validate staging DB before publication.
- [ ] no whole-DB memory materialization.

Initial implementation may stop at “staging complete, publication prerequisite missing”; do not fake durable publication with plain rename.

## Task 4 — Salts durable publication prerequisite

Track qigao/salts#308 independently.

Required evidence before #47 completion:

- [ ] Linux durable replace GREEN;
- [ ] Windows durable replace GREEN;
- [ ] result distinguishes NOT_PUBLISHED / PUBLISHED_DURABLE / DURABILITY_UNKNOWN;
- [ ] no cross-filesystem copy fallback.

## Task 5 — Final checkpoint publication

After #308:

- [ ] publish validated checkpoint staging file through the durable replace primitive;
- [ ] preserve uncertain outcome exactly;
- [ ] successful final checkpoint opens through a fresh SQLite connection;
- [ ] failure before publication leaves previous final checkpoint unchanged.

## Task 6 — Staged restore

- [ ] source checkpoint opened read-only;
- [ ] destination staging database built via bounded backup copy;
- [ ] validation completes before publication;
- [ ] all SQLite handles closed before replace;
- [ ] destination must be offline;
- [ ] stage and destination must be same filesystem;
- [ ] durable replace publishes only complete state;
- [ ] corrupt source never replaces destination;
- [ ] uncertain publication is not automatically retried.

## Task 7 — Capability descriptor

Only after behavioral gates pass:

- [ ] SQLite driver advertises `ATOMIC_STATE_METADATA`;
- [ ] advertises `FILE_BACKED_CHECKPOINT`;
- [ ] advertises `STAGED_RESTORE`;
- [ ] sets truthful limits;
- [ ] does not advertise replay/reconcile/streaming capabilities not implemented.

Use #63's optional `orm_driver_storage_capabilities_v1` tail once that SDK stack is merged/available to the SQLite runtime driver.

## Task 8 — Platform qualification

Record one exact tuple per run:

- TurboDB SHA;
- Salts SHA;
- SQLite/vcpkg version;
- OS/compiler/configuration;
- focused checkpoint/restore/transaction test results;
- full applicable unfiltered CTest result.

Minimum:
- [ ] Linux Debug/Release;
- [ ] Windows Debug/Release;
- [ ] sanitizer profiles preserved;
- [ ] no disabled assertions or retry-to-green.

## Merge boundary

#47 may merge in slices, but the issue remains open until:

```
transaction atomicity
      +
file-backed checkpoint
      +
staged restore
      +
crash-durable publication
      +
machine-readable capability mapping
```

are all backed by exact-head evidence.

## Non-goals

Raft semantics, snapshot transport, active-connection in-place restore, cross-filesystem fallback, or a separate SQLite ORM implementation.
