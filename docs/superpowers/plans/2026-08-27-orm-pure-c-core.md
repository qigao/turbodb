# ORM Pure C Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `orm_c` a C11-only library whose only C++ code is the installed inline `orm.hpp` wrapper and its consumer test.

**Architecture:** Replace the C++ planner/backend class graph with an owning C plan and versioned backend ops. Keep the existing C cursor-to-CFlow adapters, migrate one backend at a time, and delete every implementation `.cpp`/`.hpp` after its C replacement passes.

**Tech Stack:** C11, TurboUtils `tstr`/`turbo_vec_t`, CFlow, CSerde, CBind, TinyTest, CMake Presets

**Spec:** `docs/architecture/orm-pure-c-core.md`

## Global Constraints

- No `.cpp`, `.cc`, `.cxx`, or implementation `.hpp` may appear in `orm_c` sources.
- `orm.hpp` may contain only inline ownership/error wrappers over `orm.h`.
- Compatibility is intentionally not preserved; unused planner features are deleted.
- All copied values and growing containers obey connection limits.
- A failed backend/source open leaves caller output zero and ownership unchanged.
- No backend may fall back to an eager result or deleted C++ path.

---

### Task 1: Pure-C build contract and reduced public plan

**Files:**
- Modify: `orm/include/orm/orm.h`
- Modify: `orm/CMakeLists.txt`
- Create: `orm/tests/flow/orm_pure_c_build_test.c`

**Interfaces:**
- Consumes: existing opaque handles and Source entry points.
- Produces: the supported plan surface documented in the spec and a build target that rejects non-C ORM sources.

- [ ] **Step 1: Write the failing build-contract test**

  Add a C11 test that exercises every retained public query-builder function,
  opens a SQLite command/query Source, and links without a C++ test source.

- [ ] **Step 2: Verify RED**

  Configure `win-dev-user`, build `orm_pure_c_build_test`, and confirm the new
  CMake source-extension assertion fails because `ORM_C_SOURCES` contains
  `src/abi/orm_c.cpp`.

- [ ] **Step 3: Remove unsupported declarations and add the C-only source gate**

  Delete distinct/aggregate/expression/join/subquery/group/having declarations.
  Iterate over library sources in CMake and issue `FATAL_ERROR` unless each path
  ends in `.c`; declare `c_std_11` and remove C++ properties from `orm_c`.

- [ ] **Step 4: Keep RED focused on the missing C core**

  Point the ABI source at `src/abi/orm_core.c`; reconfigure and confirm failure
  is now the missing C implementation rather than a C++ source leak.

### Task 2: C handle, plan, backend, and error core

**Files:**
- Create: `orm/src/abi/orm_internal.h`
- Create: `orm/src/abi/orm_core.c`
- Create: `orm/src/abi/orm_plan.c`
- Modify: `orm/tests/flow/orm_pure_c_build_test.c`
- Delete: `orm/src/abi/orm_c.cpp`
- Delete: `orm/src/abi/orm_c_internal.hpp`

**Interfaces:**
- Consumes: `orm.h`, `orm_cbind_source_init`, `orm_command_source_init`, `tstr`, and `turbo_vec_t`.
- Produces: `orm_backend_ops`, `orm_backend`, `orm_transaction_backend`, and an immutable `orm_query_plan` passed to backend calls.

- [ ] **Step 1: Verify retained builder ownership fails without the C core**

  The test copies table, column, text, and blob inputs from temporary buffers,
  invalidates those buffers, then executes successfully. It also checks configured
  item and byte limits.

- [ ] **Step 2: Implement opaque owners and bounded plan records**

  Store plan strings as owning `tstr`, records in `turbo_vec_t`, and values in a
  destructor-aware C record. Use one `goto cleanup` path for partial creation.

- [ ] **Step 3: Implement public lifecycle and Source opening**

  Route row opens through backend cursor ops and `orm_cbind_source_init`; route
  commands through a heap-owned lazy command driver. Retain connection/query
  state through Source destruction with explicit C reference counts.

- [ ] **Step 4: Verify GREEN**

  Build and run `orm_pure_c_build_test`, `orm_cbind_source_test`, and the public
  header tests.

### Task 3: Shared C SQL renderer and SQLite vertical slice

**Files:**
- Create: `orm/src/dbs/sql/orm_sql_render.h`
- Create: `orm/src/dbs/sql/orm_sql_render.c`
- Create: `orm/src/dbs/sqlite/backend.c`
- Modify: `orm/tests/flow/orm_public_flow_test.c`
- Delete: `orm/src/dbs/sqlite/backend.cpp`

**Interfaces:**
- Consumes: `orm_query_plan`, backend placeholder callback, SQLite cursor adapter.
- Produces: rendered SQL plus ordered bound parameters, and SQLite backend ops.

- [ ] **Step 1: Add failing structured SQLite cases**

  Cover insert/select/update/delete, flat predicates, order/limit/offset, raw
  binds, transaction commit/rollback, and command affected rows.

- [ ] **Step 2: Implement bounded SQL rendering**

  Quote identifiers, emit placeholders, append clauses in plan order, and reject
  output above `max_query_bytes`. The renderer owns its SQL and parameter view
  until backend execution returns.

- [ ] **Step 3: Implement SQLite backend and transaction ops in C**

  Bind copied plan values, transfer prepared SELECT statements to
  `orm_sqlite_cursor_from_statement`, execute commands directly, and map SQLite
  status at the adapter boundary.

- [ ] **Step 4: Verify SQLite GREEN**

  Run the pure-C build, SQLite cursor, public flow, and C++ wrapper tests.

### Task 4: PostgreSQL C backend

**Files:**
- Create: `orm/src/dbs/postgres/backend.c`
- Modify: `orm/tests/flow/orm_postgres_libpq_test.c`
- Delete: `orm/src/dbs/postgres/backend.cpp`

**Interfaces:**
- Consumes: shared SQL renderer and existing pure-C libpq cursor adapter.
- Produces: PostgreSQL backend/transaction ops with `$n` placeholders.

- [ ] **Step 1: Add failing backend command/query ownership tests**

  Verify parameter encoding, cursor transfer, error status preservation,
  transaction control, and cancellation drain.

- [ ] **Step 2: Implement libpq connection and backend ops in C**

  Copy configuration into libpq keyword arrays, use the existing cursor for row
  and command drain, and implement transaction statements through the same path.

- [ ] **Step 3: Verify PostgreSQL GREEN**

  Build with `ORM_WITH_PGSQL=ON` and run both PostgreSQL flow targets.

### Task 5: MongoDB native-plan C backend

**Files:**
- Create: `orm/src/dbs/mongo/backend.c`
- Create: `orm/src/dbs/mongo/query.c`
- Create: `orm/src/dbs/mongo/query.h`
- Delete: `orm/src/dbs/mongo/backend.cpp`
- Delete: `orm/src/dbs/mongo/query.cpp`
- Delete: `orm/src/dbs/mongo/query.hpp`

**Interfaces:**
- Consumes: simple C plan and existing Mongo cursor adapter.
- Produces: bounded BSON filter/projection/sort and direct CRUD commands.

- [ ] **Step 1: Add failing native-plan tests**

  Cover equality/range predicates, projection, order/limit/offset, copied binary
  values, mutation counts, session transaction ownership, and native errors.

- [ ] **Step 2: Implement BSON builders and backend ops in C**

  Keep BSON ownership inside the adapter call, transfer native cursors on
  success, and map driver/server errors once.

- [ ] **Step 3: Verify MongoDB GREEN**

  Build with `ORM_WITH_MONGODB=ON` and run Mongo cursor plus backend tests.

### Task 6: TidesDB native-plan C backend

**Files:**
- Create: `orm/src/dbs/tidesdb/backend.c`
- Create: `orm/src/dbs/tidesdb/row.c`
- Create: `orm/src/dbs/tidesdb/row.h`
- Delete: `orm/src/dbs/tidesdb/backend.cpp`
- Delete: `orm/src/dbs/tidesdb/row.cpp`
- Delete: `orm/src/dbs/tidesdb/row.hpp`

**Interfaces:**
- Consumes: simple C plan, TidesDB bridge, and iterator cursor adapter.
- Produces: encoded owning rows, direct CRUD commands, and iterator query Sources.

- [ ] **Step 1: Add failing typed row/transaction cases**

  Cover binary-safe row encoding, id lookup, scan bounds, update/delete,
  transaction commit/rollback, malformed rows, and iterator cancellation.

- [ ] **Step 2: Implement row codec and backend ops in C**

  Preserve one row as the maximum decode unit, reject stateful ordering not
  provided by TidesDB, and transfer iterator ownership exactly once.

- [ ] **Step 3: Verify TidesDB GREEN**

  Build with `ORM_WITH_TIDESDB=ON` and run cursor plus public integration tests.

### Task 7: Redis native-plan C backend and network RESP stream

**Files:**
- Create: `orm/src/dbs/redis/backend.c`
- Create: `orm/src/dbs/redis/query.c`
- Create: `orm/src/dbs/redis/query.h`
- Delete: `orm/src/dbs/redis/backend.cpp`
- Delete: `orm/src/dbs/redis/query.cpp`
- Delete: `orm/src/dbs/redis/query.hpp`
- Modify: `redis/redis_client.h`
- Modify: `redis/redis_client.c`
- Modify: `redis/redis_parser.c`

**Interfaces:**
- Consumes: simple C plan, CoroNet socket, RESP token scanner, and CFlow cursor.
- Produces: one-row-at-a-time FT.SEARCH/FT.AGGREGATE parsing with a bounded
  receive buffer and direct mutation commands.

- [ ] **Step 1: Add failing fragmented RESP tests**

  Feed top-level and row tokens one byte/chunk at a time; assert first-row
  delivery before the final reply arrives, bounded bulk rejection, malformed
  nesting failure, cancel interruption, and connection non-reuse after an
  incomplete reply.

- [ ] **Step 2: Implement the bounded command stream in Redis C**

  Expose container/scalar RESP events whose byte views expire on the next event;
  cap depth, bulk length, and retained receive bytes; make close either drain a
  complete reply or invalidate the connection explicitly.

- [ ] **Step 3: Implement Redis query/backend ops in C**

  Build FT commands with `tstr`/`turbo_vec_t`, translate one streamed row into a
  CSerde reader, and keep mutation uncertainty semantics unchanged.

- [ ] **Step 4: Verify Redis GREEN**

  Build with `ORM_WITH_REDIS=ON` and run Redis parser, cursor, backend contract,
  and ORM public tests.

### Task 8: Delete C++ implementation residue and verify package

**Files:**
- Modify: `orm/CMakeLists.txt`
- Modify: `orm/readme.md`
- Modify: `docs/architecture/orm-cflow-c-core.md`
- Keep: `orm/include/orm/orm.hpp`
- Keep: `orm/tests/flow/orm_cpp_flow_test.cpp`

**Interfaces:**
- Consumes: every migrated C backend.
- Produces: C-only `orm_c` and header-only C++ wrapper package targets.

- [ ] **Step 1: Delete all remaining implementation C++ files**

  `fd` under `orm/src` must return no `.cpp`, `.cc`, `.cxx`, or `.hpp` files.

- [ ] **Step 2: Verify build metadata and consumers**

  Confirm `orm_c` uses C link language, build/install a C consumer, and compile
  the C++ wrapper consumer without adding C++ objects to `orm_c`.

- [ ] **Step 3: Run the complete matrix**

  Run default ORM tests, then PostgreSQL, MongoDB, TidesDB, and Redis feature
  builds/tests independently; finish with CodeGraph sync and `git diff --check`.
