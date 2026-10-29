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

## M1b: explicit revision-bound FTS5 projection (experimental)

The opt-in `TURBODB_REVISION_OPEN_FTS5` profile upgrades an admitted v1
database to **v2** only with explicit writable `OPEN_CREATE`, in one SQLite
transaction. FTS5 is mandatory for this mode: unsupported builds error rather
than falling back to LIKE, and unprofiled v1 clients reject the v2 DB.

`turbodb_revision_write_project_text` attaches a UTF-8 full-text entry to
the exact un-deleted `(namespace, binary-key, commit_seq)` created by
`write_put` earlier in the **same** writer transaction. Text documents, FTS5
tokens, Graph-style edge keys and a Generation Head are published together by
the existing `turbodb_revision_write_publish` commit and request_id.

`turbodb_revision_read_search_text` returns bounded candidates only when the
FTS row is from the latest retained revision of that exact key *as of* the
parent read's historical sequence; an earlier matching revision is never
returned as the current content. It uses the existing owned-value/borrrowed-key
scan cursor and scoped namespace, with row/bytes/VM work caps. It does not
offer global rank scores or authority grants.

The v2 DDL is **fixed, library-owned and typed**. Strict schema admission
compares the entire SQLite schema, including FTS5 shadow tables, with a fresh
canonical in-memory schema. Unknown triggers/views or forged same-name objects
are rejected. No raw SQLite pointer, arbitrary SQL registration, second
parser, or dynamic model-provided schema is exposed to the Agent.

### M1b limitations

- Graph adjacency is stored as **host-encoded revisioned B-tree keys**, not
  an unrestricted Graph query-language engine. Agent still owns SourceRevision,
  Graph edge provenance, ACL, source catalog and generation interpretation.
- Graph/FTS queries in a multi-tenant deployment still require Host authority
  filtering, audit and end-to-end data isolation; namespace is a trusted
  storage scope, not an authorization service.
- Existing v1 retained values are **not** silently retroactively FTS-indexed.
  Re-extraction/reindex must be an explicit Agent operation with a new revision.
- M1b is not a complete TurboAgent migration; it does not provide vector/HNSW,
  durable domain schema APIs, corpus import, GC, or production cutover.

## Not yet implemented; do not use as TurboAgent's production index owner

- Arbitrary domain projection registration; M1b intentionally provides only a
  **typed built-in** revision-bound FTS5 projection and generic Graph keys.
  TurboAgent must still validate its generation/evidence protocol.
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
