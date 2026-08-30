# Driver Schema Tools Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 交付不依赖 ORM 的 `turbodb-sqlite` 与 `turbodb-postgresql`，通过 native driver 批量应用 TBE bootstrap SQL。

**Architecture:** 共用纯 C CLI/file/error core；每个 executable 静态绑定一个最小 schema-driver ops table。SQLite 使用 `sqlite3_exec`，PostgreSQL 使用 libpq simple query 并 drain 所有 results。工具不链接 ORM/CFlow/TurboParser。

**Tech Stack:** C11、SQLite3、libpq、TurboUtils Core/turbo_fs、CMake Presets、TinyTest。

**Spec:** `docs/architecture/driver-data-tools.md`

## Global constraints

- Production target sources 只能是 `.c`；测试 double 可独立存在于 `dbtools/tests/`。
- `ORM_WITH_*` 和 ORM public API/behavior 不改变。
- 所有参数、文件和消息有 hard limit；错误只有一个 CLI 消费边界。
- 每项行为按 RED -> GREEN -> focused regression 实施。

### Task 1: CMake feature and target boundary

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `CMakeOptions.cmake`
- Modify: `CMakeUserPresets.json`
- Create: `dbtools/CMakeLists.txt`

**Interfaces:**
- Produces: `TURBODB_BUILD_DBTOOLS`, `TURBODB_DBTOOLS_WITH_SQLITE`, `TURBODB_DBTOOLS_WITH_PGSQL`, `TURBODB_DBTOOLS_PG_LIVE_TESTS`.
- Preserves: `ORM_WITH_*` defaults and existing targets.

- [x] **Step 1: Add a configure contract test**

Add a CMake script test that configures a small matrix and checks: ordinary Release does not require PostgreSQL; PG tool ON finds PostgreSQL; live gate ON without PG/test conninfo fails with the named prerequisite.

- [x] **Step 2: Run RED**

```text
cmake --fresh --preset win-release-user
ctest --preset win-release-user -R dbtools_configure_contract --output-on-failure
```

Expected: the test/option/target does not exist.

- [x] **Step 3: Add independent options and subdirectory**

Before `project()`, append vcpkg `postgresql` when either ORM PostgreSQL or dbtool PostgreSQL is enabled. After project configuration, add `dbtools/` only when `TURBODB_BUILD_DBTOOLS=ON`; default it OFF while cross-compiling and ON for host builds. Do not call `find_package(TurboParser)`.

- [x] **Step 4: Add empty-but-valid target scaffold**

Create only private object/static support targets needed by later tasks. Do not install a public library or expose placeholder commands. Assert every production source extension is `.c` at configure time.

- [x] **Step 5: Run GREEN and existing configure regression**

Run the contract test plus `cmake --list-presets`, build-preset list and test-preset list.

- [ ] **Step 6: Commit**

```text
git add CMakeLists.txt CMakeOptions.cmake CMakeUserPresets.json dbtools/CMakeLists.txt
git commit -m "build(dbtools): add independent driver tool gates"
```

### Task 2: Bounded CLI/file/error core

**Files:**
- Create: `dbtools/src/dbtool_error.h`
- Create: `dbtools/src/dbtool_error.c`
- Create: `dbtools/src/dbtool_schema_driver.h`
- Create: `dbtools/src/dbtool_cli.h`
- Create: `dbtools/src/dbtool_cli.c`
- Create: `dbtools/src/dbtool_file.h`
- Create: `dbtools/src/dbtool_file.c`
- Create: `dbtools/tests/dbtool_cli_test.c`
- Create: `dbtools/tests/dbtool_file_test.c`
- Modify: `dbtools/CMakeLists.txt`

**Interfaces:**
- Consumes: argv, `turbo_fs`, statically supplied `dbtool_schema_driver_ops`.
- Produces: one `schema apply` invocation and a stable process exit mapping.

- [ ] **Step 1: Write failing CLI behavior tests**

Use a complete fake schema driver and literal argv arrays. Cover exact success forwarding, unknown/duplicate/missing options, driver-specific required config, invalid decimal/zero/overflow limits, driver open failure, apply failure, and exactly-once close. Assert no SQL or connection secret appears in formatted errors.

- [ ] **Step 2: Write failing bounded-file tests**

Use TinyTest temp helpers. Cover empty file rejection, one byte, exactly limit, limit+1, unreadable path, and `size + 1` overflow guard through a narrow injected stat/read seam. Expectations are literal status/stage values, not mirror helper output.

- [ ] **Step 3: Run RED**

Build `dbtool_cli_test` and `dbtool_file_test`; expected compile failure because the core is absent.

- [ ] **Step 4: Implement minimal core**

Define one internal status enum and `dbtool_error { status, native_code, stage, message }`. Parse without fallback. File load performs `turbo_fs_stat` and checked bound validation before `turbo_fs_read_file`; invocation owns the buffer until `apply()` returns and releases it on one cleanup path.

- [ ] **Step 5: Run GREEN and mutation checks**

Run both tests. Mentally/locally mutate required-option handling, limit comparison, close call and secret formatting; at least one named test must fail for each mutation.

- [ ] **Step 6: Commit**

```text
git add dbtools/src dbtools/tests/dbtool_cli_test.c dbtools/tests/dbtool_file_test.c dbtools/CMakeLists.txt
git commit -m "feat(dbtools): add bounded schema tool core"
```

### Task 3: SQLite schema driver and executable

**Files:**
- Create: `dbtools/src/sqlite/dbtool_sqlite.c`
- Create: `dbtools/src/sqlite/dbtool_sqlite.h`
- Create: `dbtools/src/sqlite/main.c`
- Create: `dbtools/tests/dbtool_sqlite_test.c`
- Modify: `dbtools/CMakeLists.txt`

**Interfaces:**
- Produces: `turbodb-sqlite schema apply --database ... --file ...`.
- Ownership: driver context owns one `sqlite3*`; apply borrows SQL only for the call.

- [ ] **Step 1: Write failing real-SQLite tests**

Use a TinyTest temp database. Verify two CREATE statements succeed and catalog contains both tables; a valid first statement plus invalid second statement leaves neither table; explicit `BEGIN`, `COMMIT` and `SAVEPOINT` in input are rejected and leave no table; exact/over script limit is enforced before open; busy timeout validates range.

- [ ] **Step 2: Run RED**

Build/run `dbtool_sqlite_test`; expected missing driver symbols.

- [ ] **Step 3: Implement open/apply/close**

Open with explicit SQLite flags and busy timeout. Begin outer transaction, install an authorizer that denies transaction/savepoint opcodes only while executing the borrowed input, call `sqlite3_exec`, remove the authorizer, and commit or rollback. Copy native detail before `sqlite3_free`. Preserve the first useful error if rollback also fails.

- [ ] **Step 4: Add the real executable**

`main.c` passes the SQLite ops directly to common CLI. Build only when the SQLite tool gate is ON. Install to `CMAKE_INSTALL_BINDIR` without exporting it as a link target.

- [ ] **Step 5: Run GREEN and CLI smoke**

Run the unit test and execute the built binary against a temp DB with a two-table TBE-shaped DDL file; inspect catalog from the test helper, not by grepping output.

- [ ] **Step 6: Commit**

```text
git add dbtools/src/sqlite dbtools/tests/dbtool_sqlite_test.c dbtools/CMakeLists.txt
git commit -m "feat(dbtools): add atomic SQLite schema apply"
```

### Task 4: PostgreSQL schema driver and executable

**Files:**
- Create: `dbtools/src/postgresql/dbtool_postgresql.c`
- Create: `dbtools/src/postgresql/dbtool_postgresql.h`
- Create: `dbtools/src/postgresql/main.c`
- Create: `dbtools/tests/fake_libpq.c`
- Create: `dbtools/tests/fake_libpq.h`
- Create: `dbtools/tests/dbtool_postgresql_test.c`
- Create: `dbtools/tests/postgresql_live_test.c`
- Modify: `dbtools/CMakeLists.txt`
- Modify: `CMakeUserPresets.json`

**Interfaces:**
- Produces: `turbodb-postgresql schema apply --file ... [--conninfo-env NAME]`.
- Secret boundary: conninfo is read at invocation time and never copied into status/output text.

- [ ] **Step 1: Write failing libpq contract tests**

The test-only fake returns multiple literal `PGresult` states. Verify the driver calls simple-query send once, drains/releases every result, rejects an intermediate fatal result, handles a null result plus connection error, rejects row-producing results for schema apply, and never includes fake password text in errors.

- [ ] **Step 2: Run RED**

Build/run `dbtool_postgresql_test`; expected missing driver symbols.

- [ ] **Step 3: Implement libpq driver**

Connect with `PQconnectdb`, validate `PQstatus`, send the full script using `PQsendQuery`, and loop `PQgetResult` to exhaustion. Accept command/empty results only, preserve the first error while still draining, clear each result exactly once, and close `PGconn` exactly once.

- [ ] **Step 4: Add CLI and fail-fast live gate**

Read a caller-named environment variable or let libpq use its standard defaults. Add a live test only when build tests, PG tool and non-empty `TURBODB_DBTOOLS_PG_TEST_CONNINFO` are all present; otherwise explicit live enable fails configure rather than runtime-skip.

- [ ] **Step 5: Run GREEN against fake and isolated server**

Run the fake test, then use a uniquely named PostgreSQL container. Apply a generated multi-table DDL, inspect `pg_catalog`, test a constraint, and remove only the exact container in cleanup.

- [ ] **Step 6: Commit**

```text
git add dbtools/src/postgresql dbtools/tests/fake_libpq.* dbtools/tests/dbtool_postgresql_test.c dbtools/tests/postgresql_live_test.c dbtools/CMakeLists.txt CMakeUserPresets.json
git commit -m "feat(dbtools): add PostgreSQL schema apply"
```

### Task 5: Documentation, install and package smoke

**Files:**
- Modify: `README.md`
- Modify: `docs/TURBODB_LINUX_REMOTE_TEST_RUNBOOK.md`
- Modify: `docs/architecture/driver-data-tools.md`
- Create: `dbtools/tests/package/run_dbtools_package_contract.cmake`
- Modify: `dbtools/CMakeLists.txt`

- [ ] **Step 1: Add installed-binary behavior test**

Install through preset, locate each enabled binary below install `bin`, run `--help`, and apply a real SQLite script. The test checks exit status/effects, not source text.

- [ ] **Step 2: Document exact commands and failure semantics**

Document driver-specific transaction behavior, hard limits, connection secret handling, lack of ORM/TurboParser dependency, and that schema apply is bootstrap execution rather than migration management.

- [ ] **Step 3: Extend EU runbook**

Add SQLite and optional PostgreSQL live phases with run-scoped paths/container names, JUnit output, SHA-256 evidence and precise cleanup.

- [ ] **Step 4: Verify full first-stage slice**

```text
cmake --fresh --preset win-release-user
cmake --build --preset win-release-user
ctest --preset win-release-user --output-on-failure
cmake --build --preset install-win-release-user
git diff --check
```

Repeat PG live through its preset and EU Docker runbook. Confirm `turbodb-sqlite`/`turbodb-postgresql` do not link `turbo_orm`, CFlow or TurboParser and contain no production `.cpp` sources.

- [ ] **Step 5: Commit**

```text
git add README.md docs dbtools
git commit -m "docs(dbtools): document standalone schema tools"
```
