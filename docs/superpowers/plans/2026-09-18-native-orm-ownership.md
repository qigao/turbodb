# Native ORM ownership (#28) implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans task-by-task. Preserve RED and GREEN evidence separately; a failed configure/build is not behavior RED.

**Goal:** Supply the unique native query/connection/transaction ownership and execution-freeze contract required by SDK Task 6, without a consumer-side registry.

**Architecture:** Embed retained state in the native owners. Child handles, Publishers and in-flight native operations acquire leases on their actual parent, using the same synchronization boundary as checked-close and mutation admission. Resource close and memory release remain distinct; final cleanup runs after native work/callback completion, not when cancellation is merely requested.

**Tech Stack:** C11, public Salts synchronization/CFlow/CSerde/CBind, TinyTest, existing native CMake presets, C++17 wrappers.

**Spec:** `docs/superpowers/specs/2026-09-18-runtime-driver-factory-design.md`, especially sections 7, 8 and 11; issue #28 and `docs/superpowers/plans/2026-09-18-runtime-driver-sdk.md` Task 6.

**Baseline:** `0ce199c74be820146171b0a3621f8a2ffdd55411` (Task5). Its run 35363593025 passed all four Linux/Windows x64 Debug/Release jobs: core 4/4 CTest and SDK 7/7. This is not full-core or native-ownership acceptance.

## Constraints and staging

- This plan implements the already-approved retained ownership + checked-close decision. It does not choose a different public contract or close #28/#30.
- No new driver loader, registry, MySQL implementation, connection pool or worker pool. SDK Task 6 waits until this plan's real-owner gate is satisfied.
- Public ownership families use the specified `orm_connection_close`, `orm_query_close`, `orm_transaction_close`, and matching retain/release APIs. Their target release is ORM 3.0 / facade ABI 5, not an in-place semantic replacement of the released ABI 4 binary.
- During this implementation branch, do not publish/install a partially implemented ownership facade. Keep the stable ABI 4 artifact identity unchanged until the complete family and migration tests are ready for the explicit cutover. No branch is merged by this plan.
- No CMake install/export/verify code is added. Use normal build/CTest and existing dependency flows; do not disable backends, warnings, sanitizers or tests to obtain GREEN.
- No mirrored connection/query registry. A test observer may count calls but cannot supply a missing reference, lease, freeze or cleanup action.
- No sleep-based concurrency tests. Threads hold valid references; use the public Salts condition/barrier primitives and bounded waits to control admission and completion.
- State changes are serialized. BUSY leaves the owner, dependency graph and native resources unchanged. Failed irreversible cleanup does not reopen the object.
- Callback/native I/O/cleanup must not run while holding the owner control lock. Child release precedes parent release; native callback completion precedes dropping its final execution hold.
- Runtime control budgets from the spec apply when connected in #31. In #28, internal owner/lease storage remains bounded by explicit configuration and is owned by the native object, not by a global lookup table.

## Current call sites and impact

`orm/src/abi/orm_internal.h` currently stores only a backend in connection, a raw connection pointer and plan in query, and raw connection/backend/state in transaction. `orm_core.c` directly frees connection/query; its lazy command state borrows query and backend. `orm_cbind_publisher.c` owns cursor and bind scratch but not query lifetime. `orm_command_publisher.c` owns the command driver state. The native C++ wrappers call the corresponding void destructors.

Consequently, a live query must keep its connection; a lazy Publisher must both keep its query and freeze its plan; a transaction Publisher additionally holds its transaction. A plain retain alone does not implement execution freeze. Row bytes and row-shape metadata retain their existing separate borrowing rules.

## Task 1: Real-core, non-dereferencing ownership regressions

**Files:** create `orm/tests/ownership/orm_owner_regression_test.c` and `orm/tests/ownership/CMakeLists.txt`; add this test subdirectory to root `CMakeLists.txt`; extend the existing `driver-sdk-package.yml` branch/target selection only.

**Consumes:** current `orm_connect`, `orm_raw`, `orm_query_destroy`, `orm_disconnect`, `orm_query_open_command_flow`, `orm_query_bind`, and CFlow cancel/destroy. No absent API or UNSUPPORTED stub is invented.

**Produces:** CTest `orm_owner_regression`, linked to the existing real `orm_c_test_static` and a real SQLite in-memory backend. A test-only destroy observer delegates every destruction to the original SQLite callback and counts only observations; it never retains or substitutes native resources.

- [ ] Create tests for one/two queries holding a released connection, a lazy Publisher retaining the query/connection chain, parameter mutation rejection while one/two Publishers exist, cancellation not releasing a Publisher's hold, normal reverse destruction, and failed-open not leaking a hold.
- [ ] Guard the RED path: after a premature backend-destroy observation, fail the case before any freed connection access. Teardown uses still-owned query handles and unconsumed Publisher destructors; never resume a Publisher after its expected retention assertion failed.

```c
orm_disconnect(connection);
connection = NULL; /* caller consumed its own reference */
check_equal(backend_destroy_calls, 0u); /* actual old core fails here */
orm_query_destroy(query);
query = NULL;
check_equal(backend_destroy_calls, 1u);
```

- [ ] Build the root native targets with the existing platform preset. Linux Debug: `cmake --preset linux-dev-user`; `cmake --build --preset linux-dev-user --target orm_owner_regression_test orm_driver_plan_view_test orm_sql_render_test orm_public_flow_test orm_cpp_flow_test`; `ctest --preset linux-dev-user -R '^(orm_owner_regression|orm_driver_plan_view|orm_sql_render|orm_public_flow|orm_cpp_flow)$' --verbose --no-tests=error`. Windows uses `win-dev-user`; Release uses the corresponding release presets.
- [ ] Record the actual failing counts/statuses, assert no sanitizer error occurs in the diagnostic RED cleanup, and preserve the four existing core suites. Normal-destruction and failed-open cases are positive controls, not fabricated failures.
- [ ] Commit the tests and evidence. This is an incomplete issue with a deliberately failing test branch, not a released feature; no production ownership behavior changes in Task 1.

## Task 2: Native connection/query retention and checked-close

**Files:** `orm/src/abi/orm_internal.h`, `orm/src/abi/orm_core.c`; create private `orm/src/abi/orm_owner.h/.c` and `orm/tests/ownership/orm_owner_test.c`; candidate declarations initially remain private. Update the existing static-core test clone sources together with their production source list when integration is complete.

**Consumes:** Task 1's real destruction observer and the spec's close/release state graph.

**Produces:** native retained connection/query state and complete checked-close/retain/release behavior; no separate SDK-side counters. Public signatures are those already specified: `orm_status_t orm_connection_close(orm_connection_t *, orm_error_t *)`, `orm_status_t orm_query_close(orm_query_t *, orm_error_t *)`, and `void orm_connection_retain/release(orm_connection_t *)`, `void orm_query_retain/release(orm_query_t *)`.

- [ ] Before implementation add checked-close cases: query existence makes connection_close BUSY without destroying its backend; close succeeds after all query references are released; duplicate close on a still-held closed handle returns OK; newly admitted work on a closed owner returns INVALID_STATE.
- [ ] Add boundary tests for reference/lease budget exhaustion and failed query creation. Neither path may leave a parent hold or permit count overflow.
- [ ] Embed the owner state in the actual native structs. A newly created query acquires its connection hold before native work and releases it only after its own plan/resources finish cleanup. Final external release with children enters RELEASE_PENDING; it does not free the owner.
- [ ] Serialize close/admission using the same owner lock. Return an explicit pending cleanup action to the owning context rather than invoking backend destruction under that lock. Destruction happens once; closed handle memory remains until its last valid reference ends.
- [ ] Retest Task 1's connection cases and new close cases under ASan. Remaining Publisher-freeze failures remain visible until Task 3; do not mark the complete suite GREEN prematurely.

## Task 3: Publisher leases and plan mutation admission

**Files:** `orm_core.c`, `orm_owner.h/.c`, `orm_cbind_publisher.h/.c`, `orm_command_publisher.h/.c`, plus `orm/tests/ownership/orm_publisher_owner_test.c`.

**Consumes:** native owner holds from Task 2, not the test observer or the driver fixture's fake lease tickets.

**Produces:** operation leases owned by row/command Publishers and their subscriptions. Every plan mutator shares the same admission guard as Publisher creation. The private driver bridge will receive only a native owner token supplied by host execution.

- [ ] Add tests for all builder entry points: select-all, column, assignment, predicate, composite key, raw bind, ordering, limit and offset. While a Publisher holds the plan, each returns BUSY before a plan allocation or mutation, preserving exact plan bytes/counts and parameter ownership.
- [ ] Add row and lazy-command lifetime tests; execute the retained Publisher only after destruction-count assertions establish that parents are still live. Moving a Publisher transfers rather than duplicates its lease. Failed open returns no Publisher and no remaining hold.

```c
check_equal(orm_query_bind(query, orm_i64(2), &error), ORM_STATUS_BUSY);
cflow_publisher_cancel(&publisher);
check_equal(orm_query_close(query, &error), ORM_STATUS_BUSY);
cflow_publisher_destroy(&publisher);
check_equal(orm_query_close(query, &error), ORM_STATUS_OK);
```

- [ ] Acquire the execution lease before cursor creation/lazy command allocation and transfer it exactly once into the successful Publisher. Failure paths unwind only what they own. Cancel only requests cancellation; the hold survives until cursor/WAIT/native completion and Publisher teardown are complete.
- [ ] Freeze spans all of resume, including synchronous CBind decoding and delivered reader bytes. Preserve the existing row-shape lifetime contract; do not guess how to deep-copy schema metadata.
- [ ] Run positive/negative tests for normal completion, binding failure, terminal error, cancellation, unconsumed Publisher and multiple Publishers. Re-run the same unmodified Task 1 tests; complete GREEN is required before claiming this vertical slice works.

## Task 4: Transactions, native completion and cleanup failures

**Files:** owner/core files above; affected backend transaction/cursor implementations; add `orm/tests/ownership/orm_transaction_owner_test.c` and `orm_owner_race_test.c`.

**Produces:** `orm_transaction_close/retain/release` with the exact spec semantics, shared-connection validation, in-flight operation holds, and cleanup error propagation to the owning policy. No OS module unload is introduced.

- [ ] For real SQLite and a real backend with explicit active-cursor protection (PostgreSQL with a provisioned CI service), require BUSY while dependent Publishers/subscriptions exist. After they end, an active transaction still requires explicit commit/rollback before checked-close; final release may roll back but never commit.
- [ ] Record native rollback/cleanup errors rather than preserving the current ignored-error path. Distinguish a safe pre-cleanup rejection from irreversible CLOSE_FAILED. A bounded internal cleanup-error policy defaults to fail-fast and later accepts the #31 runtime handler, avoiding a dependency cycle on an unimplemented runtime.
- [ ] Add a real admitted operation held behind a Salts condition barrier. Cancel it, verify close remains BUSY, permit native completion, and verify cleanup occurs only after the completion callback has returned to its host. No worker is introduced purely to claim existing synchronous paths were asynchronous.
- [ ] Race valid-reference close against operation admission; either admission wins and close is BUSY unchanged, or close wins and admission rejects. Include held query mutation and parent release. Run ASan, the declared platform sanitizers and a thread-sanitizer configuration only when actually supported by the dependency closure.
- [ ] Preserve unknown write/commit results; never retry/replay a potentially committed operation or turn a cleanup rollback result into proof of non-commit. Backend-specific unsupported behavior remains explicit.

## Task 5: Complete C/C++ facade, migration and #28 acceptance

**Files:** `orm/include/orm/orm.h`, `orm.hpp`, owner/core implementation and normal library target/version definitions; C and C++ consumers under `orm/tests/ownership/`; update the verified architecture documentation.

- [ ] Expose the entire close/retain/release family only after Tasks 2–4 pass. Preserve move-only C++ wrappers; `.close()` reports a status, destructors release without throwing. Do not add a C++ ownership registry.
- [ ] Apply the approved major/ABI transition with header/binary identity tests. ABI 4 binaries keep their old identity; legacy void destructors in the new family explicitly consume references, not silently pretend synchronous close. Construction/runtime migration remains coordinated with #35, not a hidden default runtime.
- [ ] Build C/C++ consumers and execute early-release/explicit-close cases against the complete candidate. Verify real SQLite and PostgreSQL as above; mock-only behavior cannot close this task.
- [ ] Run root full CTest plus focused owner tests and the existing SDK suites in Linux/Windows Debug/Release. Report all failed/unsupported/not-run cases separately; do not replace full regressions with a permissive regex.
- [ ] Record exact head, dependency/service versions, commands, test inventories, assertions, actual sanitizer flags, and complete error-path logs. Do not describe staged references, syntax checks or documentation as native safety evidence.

## Handoff

Only after Task 5 and all issue #28 acceptance items have evidence may SDK Task 6 bind `orm_driver_lifetime_ops_v1` to these native operation leases. #31 separately supplies module pins that persist through callback return; #36 verifies their composition. Keep #28's TurboFlow #8/#72 dependencies and #30/#29 tracking intact.

The first implementation delivery from this plan is intentionally Task 1's real-core RED suite. Its observation counters are test diagnostics only, not an ownership implementation. Tasks 2–5 remain incomplete until their own code and runtime tests exist.
