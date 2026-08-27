# ORM CFlow C Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace eager C/C++ ORM results with a public typed CFlow Source API and remove every legacy compatibility result/facade.

**Architecture:** A C driver cursor moves directly into a typed CFlow Source and decodes each available row synchronously into an owning CMeta value. The eager result API and schema-less materialized Source are deleted; the remaining C++ header contains only ownership/error wrappers over the new C entry points.

**Tech Stack:** C11, CFlow, CMeta, CSerde, CBind, TinyTest, CMake Presets

**Spec:** `docs/architecture/orm-cflow-c-core.md`

## Global Constraints

- API and ABI compatibility are intentionally not preserved.
- Keep query pushdown separate from CFlow downstream demand.
- Bound scratch, nesting, container items, and value bytes.
- Keep cursor/source/run activity inside the connection's single-thread domain.
- Never retain a CSerde transient view beyond one synchronous decode.

---

### Task 1: CBind cursor-to-Source bridge

**Files:**
- Create: `orm/src/flow/orm_cbind_source.h`
- Create: `orm/src/flow/orm_cbind_source.c`
- Create: `orm/tests/flow/orm_cbind_source_test.c`
- Modify: `orm/CMakeLists.txt`

**Interfaces:**
- Consumes: `cflow_source`, `cserde_reader`, `cmeta_data_desc`, and `cbind_decode`.
- Produces: `orm_cbind_source_init(cflow_source *, orm_row_cursor *, const orm_cbind_source_config *, orm_error_t *)` with move-on-success cursor ownership.

- [x] **Step 1: Write the failing source contract test**

  Add a C11 TinyTest fake cursor whose rows are literal CSerde token arrays.
  Assert a valid map row decodes into a literal reflected struct, a final row
  returns `CFLOW_STEP_VALUE_AND_DONE`, and source destruction destroys the
  cursor once.

- [x] **Step 2: Run the focused test and verify RED**

  Run `cmake --fresh --preset win-release-user`, build target
  `orm_cbind_source_test`, and run `ctest --preset win-release-user -R
  orm_cbind_source`. The expected failure is a missing adapter header/symbol.

- [x] **Step 3: Implement the minimal bridge**

  Define a versioned cursor ops table with `next`, `cancel`, and `destroy`.
  Allocate bounded scratch before moving the cursor. Implement a CFlow Source
  that maps ROW/ROW_AND_DONE/WAIT/DONE/ERROR and calls `cbind_decode` only for
  row steps.

- [x] **Step 4: Add failure and lifecycle cases**

  Add tests for invalid config preserving cursor ownership, WAIT propagation,
  binding failure cancelling the cursor, explicit cancellation, and exactly-once
  destruction. Implement only the validation and cleanup required by each RED.

- [x] **Step 5: Verify GREEN and adjacent regression**

  Build `orm_cbind_source_test orm_sqlite_c_api_test orm_cpp_api_test
  orm_typed_fetch_test`; run the matching CTest filter and confirm clean output.

### Task 2: SQLite cursor vertical slice

**Files:**
- Create: `orm/src/dbs/sqlite/orm_sqlite_cursor.h`
- Create: `orm/src/dbs/sqlite/orm_sqlite_cursor.c`
- Create: `orm/tests/flow/orm_sqlite_flow_test.c`
- Modify: `orm/CMakeLists.txt`

**Interfaces:**
- Consumes: the Task 1 `orm_row_cursor` protocol and one prepared
  `sqlite3_stmt` transferred by pointer.
- Produces: a SQLite cursor that yields one row reader per `sqlite3_step` and
  finalizes the statement exactly once on cancel/destroy.

- [x] **Step 1: Write a failing demand test**

  Select two literal rows from an in-memory SQLite database, request one
  downstream value, and assert only one row reaches the sink before more demand.

- [x] **Step 2: Verify RED**

  Build and run only `orm_sqlite_flow_test`; expect failure because SQLite does
  not yet expose a cursor.

- [x] **Step 3: Implement SQLite row polling**

  Keep statement and connection retention in the cursor owner. Convert the
  current SQLite column values directly to row-local CSerde map tokens without
  building `result_backend` cells.

- [x] **Step 4: Cover cancellation and limits**

  Assert cancellation finalizes the statement, transient text/blob views do not
  escape decode, and configured per-value limits return a binding error.

- [x] **Step 5: Verify SQLite parity**

  Compare row values and error outcomes with `orm_query_execute` for empty,
  one-row, multi-row, NULL, text, blob, numeric, and malformed-conversion cases.

### Task 3: Remove the legacy collector and publish typed execution

**Files:**
- Delete: `orm/src/flow/orm_materialized_source.h`
- Delete: `orm/src/flow/orm_materialized_source.c`
- Modify: `orm/src/dbs/sqlite/backend.cpp`
- Modify: `orm/src/dbs/postgres/backend.cpp`
- Create: `orm/tests/flow/orm_public_flow_test.c`
- Modify: `orm/CMakeLists.txt`

**Interfaces:**
- Consumes: a driver cursor and a caller-provided CMeta/CBind row shape.
- Produces: `orm_query_open_flow` and `orm_query_open_flow_in_transaction`, each
  moving one typed Source to the caller.

- [x] **Step 1: Add a failing public typed-Source test**

  Execute an in-memory SQLite query into a literal reflected C struct, request
  one row, and assert demand, values, completion, and Source ownership.

- [x] **Step 2: Verify RED for the missing public flow entry**

  Build the focused public-flow target and confirm `orm_query_open_flow` is
  missing before implementation.

- [x] **Step 3: Return typed Sources from SQLite and PostgreSQL backends**

  Replace `result_backend` with a cursor-returning execution interface and feed
  it directly into `orm_cbind_source_init`. Preserve move-on-success and the
  first useful error.

- [x] **Step 4: Delete eager result and materialized Source APIs**

  Delete `orm_result_t`, every `orm_result_*` and eager execute entry, the
  materialized Source, old chain/JPA/model tests, and stale build/install rules.

- [x] **Step 5: Replace the C++ header with thin Source ownership wrappers**

  Provide move-only connection/query/source owners that call only public C
  functions and `cflow_source_destroy`; add C++ compile/runtime coverage.

### Task 4: Driver migration and C++ removal

**Files:**
- Modify: each enabled backend under `orm/src/dbs/`
- Modify: `orm/include/orm/orm.h`
- Modify then remove in a major release: `orm/include/orm/orm.hpp`,
  `orm/include/orm/jpa.hpp`, `orm/include/orm/model.hpp`
- Modify: `orm/CMakeLists.txt`, package config, examples, and `orm/readme.md`

**Interfaces:**
- Consumes: the proven cursor protocol, immutable C query plans, and generated
  CMeta/CBind schema sidecars.
- Produces: public C reactive query execution; later removes `Orm::Cpp` after
  deprecation and C consumer parity.

- [x] **Step 1: Migrate PostgreSQL, MongoDB, TidesDB, and Redis independently**

  Give each backend its own demand/cancel/live-integration tests. Mark plans that
  require backend materialization explicitly; never silently switch semantics.

  - [x] PostgreSQL: pure C libpq single-row cursor, TinyMock protocol tests,
    bytea parity, cancellation drain, direct command drain, and C API coverage.
  - [x] MongoDB: native cursor bridge, direct mutation commands, session-aware
    transaction execution, and TinyMock lifecycle coverage.
  - [x] TidesDB: iterator-backed cursor, direct mutation commands, transaction
    execution, strict row validation, and public reactive integration coverage.
  - [x] Redis: owned-reply cursor, bounded direct mutation commands, TinyMock
    lifecycle coverage, and explicit rejection of ORM-level transactions.

- [x] **Step 2: Complete versioned C reactive API verification**

  Add C and C++ header compile tests, ABI size/version checks, structured ORM
  error preservation, and shutdown-order tests.

- [ ] **Step 3: Generate CMeta/CBind schema sidecars**

  Reuse the existing TBE compiler sidecar route at build time. Validate
  descriptors and compile generated code as static and shared consumers.

- [x] **Step 4: Remove non-wrapper C++ facade code**

  Delete result vectors, repositories, sessions, lazy relations, JPA, model,
  and schema-generated C++ metadata. Keep only thin wrappers around the public
  C reactive API, then run all C API, backend, install, and consumer tests.
