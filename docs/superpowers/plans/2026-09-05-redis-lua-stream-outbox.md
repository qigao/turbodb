# Redis Lua indexed commit and Stream outbox implementation plan

> Scope: TurboDB issue [#13](https://github.com/qigao/turbodb/issues/13).  The
> first slice is deliberately a bounded Redis-native primitive; it does not
> claim to implement arbitrary ORM transaction replay.

**Goal:** provide one deterministic Redis Lua operation that atomically applies
an ordered state mutation, advances a durable applied index, and appends an
outbox event only for the first successful application.

**Architecture:** keep Redis wire protocol ownership in `tedis`.  A small
typed request/receipt facade compiles a fixed `EVAL` call and consumes the
single RESP reply through the existing `redis_cflow_connection` command stream.
The first mutation vocabulary is a single hash-field write.  It establishes
the ordering/idempotency/outbox contract without exposing arbitrary raw Redis
commands through ORM APIs.

**Compatibility:** this adds an opt-in Redis-specific API only. Existing ORM
transactions remain unchanged and continue to report Redis transaction support
as unsupported. All data keys must share one Redis Cluster hash tag; violation
fails before dispatch. Transport failure after command submission is reported
as `COMMIT_UNKNOWN`, never retried implicitly.

## 1. Define the receipt contract and write protocol-level red tests

**Files:**
- Create: `redis/include/redis_lua_apply.h`
- Modify: `redis/CMakeLists.txt`
- Modify: `redis/tests/CMakeLists.txt`
- Create: `redis/tests/test_redis_lua_apply.c`

1. Add a public request made only of borrowed byte views and bounded integer
   fields: metadata key, state key, outbox key, hash field/value, Raft index,
   term, and command id.
2. Define explicit terminal outcomes: `APPLIED`, `REPLAYED`, `GAP`,
   `VALIDATION_ERROR`, `SERVER_ERROR`, and `COMMIT_UNKNOWN`.
3. Use the existing loopback fake-server test pattern to prove the generated
   `EVAL` request contains all keys and arguments in the documented order.
4. Make tests initially fail because the public facade and script encoder do
   not yet exist.

## 2. Implement bounded Lua invocation and reply decoding

**Files:**
- Create: `redis/redis_lua_apply.c`
- Modify: `redis/CMakeLists.txt`
- Modify: `redis/include/redis_lua_apply.h`

1. Embed one constant Lua script. It reads `applied_index` from the metadata
   hash; returns `REPLAYED` when already applied; returns `GAP` unless the next
   index is exactly expected; otherwise performs `HSET`, advances metadata and
   performs `XADD` in the same script invocation.
2. Validate non-empty keys, bounded argument sizes, strictly positive index,
   and a common Redis Cluster hash tag before opening the command stream.
3. Build a bounded `EVAL` command using `redis_cflow_command_open`, then drive
   its terminal result through the existing CFlow wait/next semantics.
4. Decode only the fixed script reply shape. A malformed reply is a protocol
   failure, not a guessed success. If transport reports send-uncertain or the
   response is unavailable after dispatch, return `COMMIT_UNKNOWN`.

## 3. Complete deterministic and failure-path tests

**Files:**
- Modify: `redis/tests/test_redis_lua_apply.c`

1. Verify all script outcomes map exactly once to the public receipt.
2. Verify invalid request and mismatched hash tags fail without writing bytes
   to the connection.
3. Verify a sent-but-unanswered command becomes `COMMIT_UNKNOWN`; callers must
   reconcile by reading the authoritative applied index rather than retrying.
4. Run the focused `test_redis_lua_apply` target, then the `redis` CTest label
   and the full `win-release-user` CTest preset.

## 4. Prepare the ORM integration seam without changing ORM transaction semantics

**Files:**
- Modify: `orm/readme.md`
- Modify: `README.md`

1. Document the first-slice limits: fixed mutation vocabulary, same-slot keys,
   durable applied-index reconciliation, and the outbox's at-least-once nature.
2. Document that an ORM transaction wrapper is a follow-up once a commit-aware
   publisher/receipt API is defined; do not change `orm_transaction_begin` in
   this slice.
3. Record verification commands and link issue #13.

## Rollback

The feature is additive. Consumers can avoid the new header/API; reverting the
new source, test and documentation files returns the previous behavior without
changing existing Redis or ORM data. Data created by the script consists only
of explicitly chosen user keys and is not removed automatically.
