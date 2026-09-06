# Redis Lua Apply Reconciliation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a lost Redis reply recoverable without duplicating the Raft Stream outbox event.

**Architecture:** The existing batch EVAL uses `metadata.applied_index` as the durable commit marker. This change gives every outbox event the deterministic Stream ID `<raft-index>-0`, so a prepared event is verified rather than appended again. A read-only reconciliation EVAL validates the metadata-backed committed prefix and returns either `REPLAYED`, `PENDING`, `GAP`, or `CONFLICT`; callers may retry the identical batch only after `PENDING`.

**Tech Stack:** C11, TurboDB Redis CFlow client, Redis 8 Lua EVAL and Streams, TinyTest, CMake Presets.

**Spec:** `docs/architecture/redis-command-journal-recovery.md`

## Global Constraints

- Keep `metadata.applied_index` as the sole Raft application-state commit marker.
- All four Redis keys require one equal non-empty Cluster hash tag; missing dependencies and invalid input fail fast.
- Preserve existing public enum values by appending any new receipt kind after `REDIS_LUA_APPLY_ERROR`.
- Do not add CMake compatibility paths, DLL copy rules, or new build helpers.
- `redis_lua_apply_batch_open` callers retain borrowed records only until the open call returns; CFlow stream ownership remains internal to the operation.

---

### Task 1: Specify recovery results and deterministic outbox identity

**Files:**
- Create: `docs/architecture/redis-command-journal-recovery.md`
- Modify: `redis/redis_lua_apply_batch.h`
- Test: `redis/tests/test_redis_lua_apply_batch.c`

**Interfaces:**
- Consumes: `redis_lua_apply_batch_request`, `redis_lua_apply_batch`, and `redis_lua_apply_batch_step`.
- Produces: `REDIS_LUA_APPLY_PENDING` and `redis_lua_apply_batch_reconcile_open(connection, request, operation)`.

- [ ] **Step 1: Write the failing live Redis test**

Add a prepared Stream event with ID `42-0`, metadata `applied_index=41`, and the first record of a two-record request. Require this sequence:

```c
check_equal(redis_lua_apply_batch_reconcile_open(&connection, &request,
                                                  &operation), SALTS_OK);
step = redis_lua_apply_batch_test_complete(&operation, &runtime);
check_equal(step.receipt.kind, REDIS_LUA_APPLY_PENDING);
check_equal(step.receipt.applied_index, UINT64_C(41));
check_equal(redis_lua_apply_batch_destroy(&operation), SALTS_OK);
```

- [ ] **Step 2: Run the focused test to verify it fails**

Run: `TURBODB_REDIS_TEST_PORT=6391 ctest --preset win-release-user -R "^test_redis_lua_apply_batch$" --output-on-failure`

Expected: compilation fails because `redis_lua_apply_batch_reconcile_open` and `REDIS_LUA_APPLY_PENDING` do not exist.

- [ ] **Step 3: Declare the minimal public contract**

Append `REDIS_LUA_APPLY_PENDING` after `REDIS_LUA_APPLY_ERROR` and declare:

```c
REDIS_API int redis_lua_apply_batch_reconcile_open(
    redis_cflow_connection *connection,
    const redis_lua_apply_batch_request *request,
    redis_lua_apply_batch *out_operation);
```

Document that `PENDING` permits retrying the same request and that `COMMIT_UNKNOWN` itself never permits blind retry.

- [ ] **Step 4: Re-run the focused test to verify the link failure**

Run: `TURBODB_REDIS_TEST_PORT=6391 ctest --preset win-release-user -R "^test_redis_lua_apply_batch$" --output-on-failure`

Expected: link failure because the declared reconciliation entry point has no implementation.

- [ ] **Step 5: Commit**

```bash
git add redis/redis_lua_apply_batch.h redis/tests/test_redis_lua_apply_batch.c \
        docs/architecture/redis-command-journal-recovery.md
git commit -m "test(redis): specify batch reconciliation"
```

### Task 2: Implement idempotent outbox emission and reconciliation

**Files:**
- Modify: `redis/redis_lua_apply_batch.c`
- Test: `redis/tests/test_redis_lua_apply_batch.c`

**Interfaces:**
- Consumes: the Task 1 public declaration and the existing batch request validation/parser.
- Produces: a read-only reconcile EVAL and idempotent deterministic Stream event publication.

- [ ] **Step 1: Make the prepared-event test fail at runtime**

Implement only enough reconciliation submission and receipt parsing for the test to execute, leaving the current `XADD ... *` publication unchanged.

Expected: the prepared-event test reaches the outbox assertion and fails because `XLEN` is `3`, proving that auto-generated Stream IDs duplicate a prepared event.

- [ ] **Step 2: Implement deterministic, verified Stream publication**

For each new record, use `stream_id = index .. '-0'`. Before `XADD`, query exactly that ID with `XRANGE`. If absent, append the prescribed four fields. If present, require the exact ID and exact ordered fields `index`, `term`, `command_id`, and `payload`; otherwise return `CONFLICT` before metadata advances.

- [ ] **Step 3: Implement reconciliation EVAL**

Reuse request validation and operation polling. The script must perform no writes and return:

```text
REPLAYED  when every requested index is at or below metadata.applied_index
          and stored identity/payload values match exactly
PENDING   when the committed prefix matches and the first uncommitted index is
          exactly metadata.applied_index + 1
GAP       when the requested range cannot follow metadata.applied_index
CONFLICT  when any committed journal/identity value differs
```

- [ ] **Step 4: Run the focused live test to verify it passes**

Run: `TURBODB_REDIS_TEST_PORT=6391 ctest --preset win-release-user -R "^test_redis_lua_apply_batch$" --output-on-failure`

Expected: PASS; the prepared `42-0` event is retained and the finished two-record batch has `XLEN=2`.

- [ ] **Step 5: Commit**

```bash
git add redis/redis_lua_apply_batch.c redis/tests/test_redis_lua_apply_batch.c
git commit -m "feat(redis): reconcile Lua journal batches"
```

### Task 3: Verify recovery contract and package delivery

**Files:**
- Modify: `docs/architecture/redis-command-journal-recovery.md`
- Test: `redis/tests/test_redis_lua_apply_batch.c`

**Interfaces:**
- Consumes: completed reconciliation and deterministic outbox API.
- Produces: evidence that installed TurboDB exposes the recovery API.

- [ ] **Step 1: Add the final state assertions**

After a successful retry, require a reconciliation call to return `REPLAYED` at index `43`; require a request with changed committed payload to return `CONFLICT` and retain `XLEN=2`.

- [ ] **Step 2: Run the focused live test and package build**

Run: `TURBODB_REDIS_TEST_PORT=6391 ctest --preset win-release-user -R "^test_redis_lua_apply_batch$" --output-on-failure`

Run: `cmake --build --preset install-win-release-user`

Expected: both commands pass and the installed `include/redis/redis_lua_apply_batch.h` contains `redis_lua_apply_batch_reconcile_open`.

- [ ] **Step 3: Run the full Release suite**

Run: `TURBODB_REDIS_TEST_PORT=6391 ctest --preset win-release-user --output-on-failure`

Expected: all tests pass.

- [ ] **Step 4: Commit**

```bash
git add docs/architecture/redis-command-journal-recovery.md redis/tests/test_redis_lua_apply_batch.c
git commit -m "docs(redis): define batch recovery protocol"
```
