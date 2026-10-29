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

## M1a — versioned SQLite B-tree prefix retrieval (proposed)

[TurboDB #222](https://github.com/qigao/turbodb/issues/222) first slice
adds a read-only, bounded `turbodb_revision_read_scan_prefix()` over the
**existing** `revision_values(namespace,item_key,commit_seq)` primary-key
B-tree, without a new table, pager, file, or SQL parser.

- A nonempty **binary** key prefix defines an ordered range; the exclusive
  lexicographic successor supports embedded NUL and all-`0xff` prefixes.
- The scan resolves each key's latest immutable version at the parent
  read's `as_of_seq`; tombstones are returned explicitly.
- The key view is borrowed until `scan_next`/`scan_close`, while the
  value is caller-owned and released via `turbodb_revision_value_destroy`.
  At most one scan per read; parent `read_close` drains the cursor.
- Explicit row, payload-byte, and SQLite VM-operation budgets. Reaching any
  limit returns `LIMIT_EXCEEDED` rather than claiming a complete search.
- This is the ordered B-tree primitive usable for application-generated Graph
  outgoing/reverse edge keys and FileCatalog prefixes. Graph identity/authority
  policy and key encoding still belong to TurboAgent.

**Not a completed M1 Graph/FTS projection.** FTS5 virtual tables, trusted
projection schema registration, atomic `index_generation`, migration, ACL
filters and Graph-specific API are still separate acceptance tasks. No
production TurboAgent owner change.

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
