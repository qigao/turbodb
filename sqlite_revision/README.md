# TurboDB Revisioned SQLite (M0 / Draft)

C11, provider-specific optional module `TurboDB::SqliteRevision`, built using
**native SQLite**. This is a deliberately narrow first implementation for
[qigao/turbodb #220](https://github.com/qigao/turbodb/issues/220). It does not
fork SQLite, implement a second pager, or depend on ORM/Studio.

## Implemented (v1 proposed ABI)

- A single writer owns a bounded SQLite `BEGIN IMMEDIATE` transaction.
- Each committed request has a durable unique `request_id` and monotonically
  increasing `commit_seq`. One transaction can modify distinct keys.
- Historical `(namespace, binary key, commit_seq)` rows are immutable;
  `revision_heads` is updated in the **same** transaction.
- Reads at a selected durable commit sequence survive reopen/checkpoint.
  Read handles pin a separate SQLite read transaction. Callers must close
  all readers and writers before closing the database.
- Expected-head CAS: `EXPECT_ABSENT=0`, `EXPECT_ANY=UINT64_MAX`, or an exact
  prior commit sequence. Delete creates an explicit tombstone.
- Unique request IDs prevent duplicate publication. Inspect a committed
  request by opening a **clean connection** after a `COMMIT_UNKNOWN` status.
  No automatic retries, implicit commits, or unsafe fallback.
- Independent `READ_ONLY`/`CREATE`/`WAL` policies; default rollback journal
  with `synchronous=FULL`, explicit per-value and main DB page budgets.
- Every write is bounded by configured **batch operation count and cumulative namespace/key/value bytes**; an exceeded budget poisons the transaction and requires abort.
- New database identity `application_id=TDRV`, `user_version=1`. Unknown
  foreign/unversioned databases are rejected rather than modified.

## Not yet implemented; do not use as TurboAgent's production index owner

- Same-transaction domain projection registration for FTS5/Graph/vector
  indexes and atomic `index_generation` publication.
- Migration of existing KnowledgeStore/VectorStore/RuntimeStore files.
- Historical revision garbage collection, compaction and pin-aware retention.
- Fault-injected `xWrite` / `xSync`, crash-boundary and multiple-process CI.
- Cross-repository installed SDK acceptance on exact HEAD.

`read_at(commit_seq)` is persistent **application history**, not a claim that
SQLite can retain transaction snapshots indefinitely. WAL mode may use SQLite
`-wal/-shm` sidecars. The DB is one *logical* file; exporting one portable
physical snapshot is performed by the existing TurboDB SQLite maintenance
service, not by raw copying an active WAL database.

The database owner is single-threaded for handle lifecycle and writes, with
native SQLite mutexes for connections. No shared-handle concurrent mutation,
reentrancy, or cross-process multi-writer admission contract is supplied.
