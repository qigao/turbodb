# Redis Journal Compaction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a bounded, Redis-native operation that removes an applied Raft command-journal range only after a durable snapshot identity has been verified.

**Architecture:** `redis_lua_apply_batch_compact_open` and its reconciliation companion use the established batch CFlow operation and receipt types.  One Redis Lua `EVAL` validates four same-slot keys, the metadata applied marker, the snapshot term in the identity hash, and every requested journal entry before deleting journal and identity fields; it advances `metadata.journal_floor` only after those deletes.  The outbox Stream is intentionally neither read for retention nor trimmed.

**Tech Stack:** C11, TurboDB Redis CFlow client, Redis 8 Lua `EVAL`, TinyTest, CMake Presets.

**Spec:** `docs/architecture/redis-command-journal-recovery.md`

## Global Constraints

- The public operation is opt-in and preserves existing batch-apply behavior and receipt values.
- Every request carries metadata, journal, identity, and outbox keys with one equal non-empty Redis Cluster hash tag.
- A range has an explicit inclusive `first_index` and `last_index`, has no more than `REDIS_LUA_APPLY_BATCH_MAX_RECORDS` entries, and is never split or retried implicitly.
- Metadata is the only retention fact: `journal_floor`, `journal_compaction_first_index`, `journal_compaction_snapshot_index`, and `journal_compaction_snapshot_term` are written after journal and identity deletion.
- Any malformed metadata, snapshot identity mismatch, incomplete journal range, non-contiguous retention request, or applied-index violation returns a terminal non-success receipt before mutation.
- The Lua script must not call `XDEL`, `XTRIM`, or any other Stream retention command.
- CMake uses the existing `tedis` source/header list and `cmake_add_test()`; no compatibility wrapper, DLL-copy rule, or fallback path is introduced.

---

### Task 1: Specify a bounded compaction request

**Files:**
- Create: `redis/redis_lua_apply_batch_compact.h`
- Modify: `redis/CMakeLists.txt`
- Modify: `redis/tests/test_redis_lua_apply_batch.c`

**Interfaces:**
- Consumes: `redis_lua_apply_batch`, `redis_lua_apply_batch_step`, and `REDIS_LUA_APPLY_BATCH_MAX_RECORDS` from `redis_lua_apply_batch.h`.
- Produces: `redis_lua_apply_batch_compact_request`, `redis_lua_apply_batch_compact_open`, and `redis_lua_apply_batch_compact_reconcile_open`.

- [ ] **Step 1: Write failing public-contract tests**

Add a request helper whose four keys are `raft:{orders}:meta`, `:journal`, `:identity`, and `:outbox`, with range `42..43` and snapshot `(43, 123456)`.  Require invalid zero snapshot term, a range larger than `REDIS_LUA_APPLY_BATCH_MAX_RECORDS`, and a mismatched hash tag to fail `open` with `SALTS_EINVAL` before a CFlow command opens.

- [ ] **Step 2: Verify RED**

Run `cmake --build --preset win-release-user --target test_redis_lua_apply_batch`.  Expected: compilation fails because `redis_lua_apply_batch_compact.h` and the public request/open declarations do not exist.

- [ ] **Step 3: Declare the public contract**

Create the header with:

```c
typedef struct redis_lua_apply_batch_compact_request {
  const char *metadata_key; size_t metadata_key_length;
  const char *journal_key; size_t journal_key_length;
  const char *identity_key; size_t identity_key_length;
  const char *outbox_key; size_t outbox_key_length;
  uint64_t first_index; uint64_t last_index;
  uint64_t snapshot_index; uint64_t snapshot_term;
} redis_lua_apply_batch_compact_request;

REDIS_API int redis_lua_apply_batch_compact_open(
    redis_cflow_connection *, const redis_lua_apply_batch_compact_request *,
    redis_lua_apply_batch *);
REDIS_API int redis_lua_apply_batch_compact_reconcile_open(
    redis_cflow_connection *, const redis_lua_apply_batch_compact_request *,
    redis_lua_apply_batch *);
```

Document that the receipt index is the compacted-through index, `PENDING` alone permits an identical retry, and callers must keep snapshot state durable until `APPLIED` or `REPLAYED`.  Install the header through the existing `REDIS_HEADERS` list.

- [ ] **Step 4: Verify declaration build failure**

Run `cmake --build --preset win-release-user --target test_redis_lua_apply_batch`.  Expected: link failure for the two declared compaction entry points.

### Task 2: Execute and reconcile the bounded Redis transaction

**Files:**
- Modify: `redis/redis_lua_apply_batch.c`
- Test: `redis/tests/test_redis_lua_apply_batch.c`

**Interfaces:**
- Consumes: the Task 1 request and the existing CFlow batch operation ownership contract.
- Produces: a compaction `EVAL`, a read-only reconciliation `EVAL`, `APPLIED`, `REPLAYED`, `PENDING`, `GAP`, and `CONFLICT` receipts.

- [ ] **Step 1: Write a failing real-Redis compaction test**

Seed `metadata.applied_index=43`, write batch records `42` and `43`, then compact `42..43` for snapshot term `123456`.  Assert `APPLIED 43`, `HGET metadata journal_floor` is `43`, both hashes no longer contain `42` or `43`, and `XLEN outbox` remains `2`.  Reopen the same request through reconciliation and require `REPLAYED 43`.

- [ ] **Step 2: Verify RED**

Run `$env:TURBODB_REDIS_TEST_PORT=6391; ctest --preset win-release-user -R "^test_redis_lua_apply_batch$" --output-on-failure`.  Expected: link failure until the public entry points are implemented.

- [ ] **Step 3: Implement one shared command-open path and the Lua scripts**

Factor the existing operation submission so both batch apply and compaction allocate an owned CFlow stream through the same `EVAL` helper.  The compaction script shall:

```text
validate keys, all u64 arguments, range <= 1024, applied_index >= snapshot_index,
and journal_floor + 1 == first_index;
verify identity[snapshot_index] starts with snapshot_term + NUL;
verify every index in [first_index,last_index] exists in both hashes;
HDEL the journal fields, HDEL the identity fields;
HSET metadata journal_floor last_index,
     journal_compaction_first_index first_index,
     journal_compaction_snapshot_index snapshot_index,
     journal_compaction_snapshot_term snapshot_term;
return APPLIED last_index.
```

The reconcile script does no writes: matching the exact persisted floor and snapshot tuple returns `REPLAYED`; the pre-commit floor with a valid snapshot identity returns `PENDING`; malformed/missing facts return `GAP` or `CONFLICT`.  It must not issue Stream mutation commands.

- [ ] **Step 4: Verify GREEN**

Run `$env:TURBODB_REDIS_TEST_PORT=6391; ctest --preset win-release-user -R "^test_redis_lua_apply_batch$" --output-on-failure`.  Expected: PASS.

### Task 3: Cover retention boundaries and document recovery

**Files:**
- Modify: `redis/tests/test_redis_lua_apply_batch.c`
- Modify: `docs/architecture/redis-command-journal-recovery.md`

**Interfaces:**
- Consumes: implemented compaction and reconciliation functions.
- Produces: documented caller state machine and regression coverage for unsafe retention attempts.

- [ ] **Step 1: Add live boundary tests**

Using literal Redis fixtures, require each behavior independently: wrong snapshot term returns `CONFLICT` without deletion; `snapshot_index > applied_index` returns `GAP`; an identity or journal gap returns `GAP`; a pre-commit reconciliation returns `PENDING`; and a `UINT64_MAX` one-record range with matching metadata/identity compacts successfully.

- [ ] **Step 2: Document the state and failure contract**

Add a “Snapshot-aware journal compaction” section to the architecture document.  State ownership (`metadata`), mutation order, exact retry rule, non-recoverable outcomes, four-key Cluster requirement, maximum range, outbox non-retention, and the requirement that TurboRaft quiesce writes for the compacted range.

- [ ] **Step 3: Run focused and full verification**

Run:

```powershell
$env:TURBODB_REDIS_TEST_PORT=6391
ctest --preset win-release-user -R "^test_redis_lua_apply_batch$" --output-on-failure
cmake --build --preset install-win-release-user
ctest --preset win-release-user --output-on-failure
```

Expected: all selected tests pass and installed `include/redis/redis_lua_apply_batch_compact.h` declares both public functions.

- [ ] **Step 4: Commit**

```bash
git add redis/redis_lua_apply_batch.c redis/redis_lua_apply_batch_compact.h \
        redis/CMakeLists.txt redis/tests/test_redis_lua_apply_batch.c \
        docs/architecture/redis-command-journal-recovery.md \
        docs/superpowers/plans/2026-09-06-redis-journal-compaction.md
git commit -m "feat(redis): compact Raft command journals"
```
