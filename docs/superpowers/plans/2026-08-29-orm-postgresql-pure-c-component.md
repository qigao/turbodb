# ORM Pure-C PostgreSQL Component Implementation Plan

> **Superseded package contract:** This completed implementation plan records the
> original split-package approach. The accepted final contract is defined by
> [`orm-cmake-package-boundary.md`](../../architecture/orm-cmake-package-boundary.md):
> Orm is shared-only, `Orm::PostgreSQL` is exported from the single `Orm` package,
> and consumers never call `find_package(OrmPostgreSQL)`.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在最新 pure-C ORM 上交付可选 PostgreSQL component、真实 PostgreSQL live gate，以及原子 composite-key query contract。

**Architecture:** `Orm::C` 保持数据库无关的 connection/query/result/CFlow core；`Orm::PostgreSQL` 是显式 connector 和 libpq 薄适配器，不使用全局 registry。复合键是一次性复制进 query plan 的有界 key-part batch，typed/generated adapters 位于 ORM 边界之外。

**Tech Stack:** C11、CFlow、CSerde/CBind、CSTL vec、libpq、CMake Presets、TinyTest、header-only C++17 wrapper。

**Spec:** `docs/architecture/orm-postgresql-pure-c-component.md`

## Global Constraints

- 生产 ORM 和 PostgreSQL component 只编译 `.c`；C++ 只存在于 `.hpp` wrapper 和测试。
- 不引入进程级 driver registry、backend fallback 或 TurboParser runtime dependency。
- 所有可增长 predicate/result 路径受现有 `orm_limits` 约束。
- PostgreSQL live test 默认关闭；显式启用但缺少 conninfo 时 configure 必须失败。
- 每项行为严格执行 RED → GREEN → focused regression。

---

### Task 1: Atomic composite-key predicates

**Files:**
- Modify: `orm/include/orm/orm.h`
- Modify: `orm/include/orm/orm.hpp`
- Modify: `orm/src/abi/orm_internal.h`
- Modify: `orm/src/abi/orm_plan.c`
- Modify: `orm/src/abi/orm_core.c`
- Modify: `orm/tests/flow/orm_public_flow_test.c`

**Interfaces:**
- Produces: `orm_key_part_t`, `orm_query_where_key(query, parts, count, error)` and `orm::query::where_key(parts, count)`.
- Ownership: the API copies all column names and text/blob payloads; input is borrowed only for the call.

- [x] **Step 1: Write the failing public behavior test**

Add TinyTest cases that create a SQLite table with `primary key(domain_id,user_id,group_id)`, insert two rows, and use this literal input:

```c
const orm_key_part_t key[] = {
    {orm_view("domain_id"), orm_text("domain-a")},
    {orm_view("user_id"), orm_text("user-a")},
    {orm_view("group_id"), orm_text("group-a")}};
check_equal(orm_query_where_key(query, key, 3u, &error), ORM_STATUS_OK);
```

Assert exactly one selected row. Add a second case whose second key column is `bad;column`; assert `ORM_STATUS_INVALID_ARGUMENT`, then execute the same query and observe both rows, proving no partial first predicate remains. Add a duplicate-column case and assert the same atomic failure.

- [x] **Step 2: Verify RED**

Run through VS environment:

```text
cmake --build --preset win-release-user --target orm_public_flow_test
```

Expected: compile failure because `orm_key_part_t` and `orm_query_where_key` do not exist.

- [x] **Step 3: Add the public value type and batch API**

Declare the exact interface from the spec in `orm.h`. Add this inline helper without owning memory:

```c
static inline orm_key_part_t orm_key_part(orm_string_view_t column,
                                          orm_value_t value) {
  orm_key_part_t part = {column, value};
  return part;
}
```

- [x] **Step 4: Implement transactional plan append**

Add internal `orm_plan_add_key()` that validates non-empty input, remaining predicate capacity, every identifier, and duplicate column bytes before mutation. Store `vec_size(&plan->predicates)` and `plan->parameter_bytes`; append equality predicates with `orm_plan_add_predicate()`. On failure, repeatedly `vec_pop()` into an `orm_predicate`, destroy its column/value, and restore parameter bytes.

- [x] **Step 5: Add the header-only C++ forwarding wrapper**

Add `query::where_key(const orm_key_part_t *parts, std::uint32_t count)`; it calls the C API through the existing `call()` error boundary and returns `*this`.

- [x] **Step 6: Verify GREEN and focused regressions**

Run:

```text
cmake --build --preset win-release-user --target orm_public_flow_test orm_cpp_flow_test
ctest --preset win-release-user -R "orm_(public|cpp)_flow" --output-on-failure
```

Expected: both tests pass and the invalid batch leaves observable query behavior unchanged.

- [x] **Step 7: Commit the composite-key slice**

```text
git add orm/include/orm/orm.h orm/include/orm/orm.hpp orm/src/abi/orm_internal.h orm/src/abi/orm_plan.c orm/src/abi/orm_core.c orm/tests/flow/orm_public_flow_test.c
git commit -m "feat(orm): add atomic composite key predicates"
```

### Task 2: Pure-C PostgreSQL component

**Files:**
- Create: `orm/include/orm/orm_postgresql.h`
- Create: `orm/include/orm/orm_postgresql.hpp`
- Create: `orm/src/dbs/postgres/component.c`
- Create: `orm/cmake/OrmPostgreSQLConfig.cmake.in`
- Modify: `orm/include/orm/orm.hpp`
- Modify: `orm/src/abi/orm_internal.h`
- Modify: `orm/src/abi/orm_core.c`
- Modify: `orm/src/dbs/sql/orm_sql_render.h`
- Modify: `orm/CMakeLists.txt`
- Create: `orm/tests/package_consumer/postgresql/CMakeLists.txt`
- Create: `orm/tests/package_consumer/postgresql/main.c`
- Create: `orm/tests/package_consumer/postgresql/main.cpp`

**Interfaces:**
- Consumes: current `orm_backend` factory contract and PostgreSQL backend sources.
- Produces: `Orm::PostgreSQL`, installed `OrmPostgreSQL` package, `orm_postgresql_connect()` and header-only `orm::postgresql_connection()`.

- [x] **Step 1: Write failing component consumers**

The C consumer includes `<orm_postgresql.h>`, builds a default config with driver `postgresql`, and takes the address of `orm_postgresql_connect`. The C++ consumer includes `<orm_postgresql.hpp>` and verifies at compile time that `orm::postgresql_connection(const orm::config&)` returns `orm::connection`.

- [x] **Step 2: Verify RED**

Configure with PostgreSQL feature and build the two consumer targets. Expected failure: headers and `Orm::PostgreSQL` target are absent.

- [x] **Step 3: Add a versioned private connector hook**

Move connection limit validation and allocation into:

```c
typedef orm_status_t (*orm_backend_factory_v1)(
    const orm_config_t *, const orm_limits *, orm_backend *, orm_error_t *);

ORM_C_API orm_status_t ORM_C_CALL orm_connect_with_factory_v1(
    const orm_config_t *config, orm_backend_factory_v1 factory,
    orm_connection_t **out_connection, orm_error_t *error);
```

The declaration remains in `orm_internal.h`; `orm_connect()` invokes it with the existing built-in factory after PostgreSQL is removed from that factory. Mark exactly `orm_error_set`, `orm_view_valid`, `orm_view_equal_cstr`, `orm_sql_render`, `orm_sql_query_destroy`, and `orm_connect_with_factory_v1` with `ORM_C_API` in private headers so the lockstep component can link without exposing them in installed public headers.

- [x] **Step 4: Implement the component connector**

`component.c` validates `config->driver` is `postgres` or `postgresql`, then calls `orm_connect_with_factory_v1(config, orm_postgres_backend_create, ...)`. It defines no global mutable state.

- [x] **Step 5: Split CMake targets**

Remove PostgreSQL files from `ORM_C_SOURCES`/`ORM_FLOW_SOURCES`. Under `ORM_WITH_PGSQL`, build `orm_postgresql` from `component.c`, `backend.c`, `orm_postgres_libpq.c`, and `orm_postgres_cursor.c`; enforce the `.c` source invariant; link `Orm::C` publicly and libpq/CFlow/CSerde/STL privately; use export name `PostgreSQL`, export set `OrmPostgreSQLTargets`, and namespace/build-tree alias `Orm::PostgreSQL`. Shared builds assign the C++ linker only to this component when libpq's private archives require it, and install its runtime dependency set separately; `turbo_orm` keeps the C linker.

- [x] **Step 6: Add independent package configuration**

`OrmPostgreSQLConfig.cmake.in` finds `Orm` from its sibling install directory. It calls `find_dependency(PostgreSQL)` only for static component builds, then includes `OrmPostgreSQLTargets.cmake`. `OrmConfig.cmake.in` no longer contains PostgreSQL discovery. The package-consumer CMake project calls both `find_package(Orm)` and `find_package(OrmPostgreSQL)`, then links its C and C++ executables only to `Orm::PostgreSQL`.

- [x] **Step 7: Add header-only C++ connector**

Refactor `orm::connection` to accept a C connector function pointer while preserving the existing constructor. `orm_postgresql.hpp` implements only:

```cpp
inline connection postgresql_connection(const config &configuration) {
  return connection(configuration, orm_postgresql_connect);
}
```

- [x] **Step 8: Verify GREEN**

Run the PG-enabled configure/build, the C/C++ component consumer tests, `orm_postgres_flow`, `orm_postgres_libpq`, and install preset. Inspect installed headers and both package configs.

- [x] **Step 9: Commit the component slice**

```text
git add orm
git commit -m "refactor(orm): isolate pure C PostgreSQL component"
```

### Task 3: Explicit PostgreSQL live gate

**Files:**
- Create: `orm/tests/integration/postgres_live_test.c`
- Modify: `orm/CMakeLists.txt`
- Modify: `CMakeUserPresets.json`

**Interfaces:**
- Consumes: `orm_postgresql_connect()`, public query/result/transaction APIs and `TURBODB_ORM_PGSQL_TEST_CONNINFO`.
- Produces: `orm_postgres_live` CTest registered only when the explicit gate is valid.

- [x] **Step 1: Add configure-contract test cases**

Verify a configure with `ORM_POSTGRES_LIVE_TESTS=ON` and no conninfo fails with a message naming `TURBODB_ORM_PGSQL_TEST_CONNINFO`. Verify the ordinary Release preset still configures without PostgreSQL.

- [x] **Step 2: Implement the fail-fast CMake gate**

Add `option(ORM_POSTGRES_LIVE_TESTS ... OFF)`. When enabled, require `ORM_BUILD_TESTS`, `ORM_WITH_PGSQL`, and a non-empty environment conninfo before creating the test target.

- [x] **Step 3: Write the live test**

Use TinyTest and a single cleanup path. Open through `orm_postgresql_connect()`, create a PostgreSQL temporary table with `(domain_id,user_id,group_id)` primary key, exercise text/bytea round trip and `orm_query_where_key()`, commit one transaction, roll back another, assert SQLSTATE-to-ORM-status mapping through a duplicate-key error, and verify `max_result_rows` enforcement. Never print environment values.

- [x] **Step 4: Verify RED against an unavailable server**

Point conninfo at an unused local port and run only `orm_postgres_live`. Expected: explicit `ORM_STATUS_CONNECTION_ERROR`; the test must fail rather than skip.

- [x] **Step 5: Verify GREEN against an isolated PostgreSQL container**

Start a uniquely named PostgreSQL container, wait for `pg_isready`, export conninfo, run the live CTest, and remove the exact container in a trap. Expected: one test, zero failures.

- [x] **Step 6: Commit the live gate**

```text
git add CMakeUserPresets.json orm/CMakeLists.txt orm/tests/integration/postgres_live_test.c
git commit -m "test(orm): add explicit PostgreSQL live gate"
```

### Task 4: Documentation and EU runbook

**Files:**
- Modify: `orm/readme.md`
- Modify: `docs/TURBODB_LINUX_REMOTE_TEST_RUNBOOK.md`
- Modify: `docs/architecture/orm-postgresql-pure-c-component.md`

**Interfaces:**
- Documents: C/C++ connector usage, ownership, package discovery, composite-key input lifetime, live-test variables and Docker cleanup.

- [x] **Step 1: Update public usage examples**

Show `find_package(Orm)` for core-only consumers and `find_package(OrmPostgreSQL)` plus `Orm::PostgreSQL` for PostgreSQL consumers. C code calls `orm_postgresql_connect`; C++ code includes `orm_postgresql.hpp`.

- [x] **Step 2: Update EU commands**

Add an optional PostgreSQL live phase that creates one run-scoped DB container, passes `TURBODB_ORM_PGSQL_TEST_CONNINFO`, uses the PG-enabled preset, writes JUnit, and removes only exact run-scoped containers.

- [x] **Step 3: Validate documentation paths and commands**

Run `cmake --list-presets`, `cmake --build --list-presets`, `ctest --list-presets`, and `git diff --check`. Confirm every documented target/header/config exists in the build or install tree.

- [x] **Step 4: Commit documentation**

```text
git add docs orm/readme.md
git commit -m "docs(orm): document PostgreSQL component testing"
```

### Task 5: Final verification

**Files:**
- Verify only; no planned production edits.

**Interfaces:**
- Consumes: all preceding deliverables.
- Produces: repeatable Windows and EU evidence.

- [x] **Step 1: Windows core profile**

Run fresh configure, full build, full CTest and install through `win-release-user`; require every discovered test to pass.

- [x] **Step 2: Windows PostgreSQL component profile**

Run fresh PG-enabled configure/build, focused component/unit/package consumers and install. Verify `OrmConfig.cmake` has no PostgreSQL dependency and `OrmPostgreSQLConfig.cmake` has the conditional static dependency.

- [x] **Step 3: EU Ubuntu/PostgreSQL live profile**

Package the exact worktree using the runbook, verify its SHA-256 remotely, build Salts with epoll readiness, build TurboDB with TidesDB engine tests off, run the full TurboDB CTest plus PostgreSQL live test, and save log/JUnit/SHA-256 evidence.

- [x] **Step 4: Final repository checks**

Run `git diff --check`, inspect `git status --short`, confirm no `.cpp` production source entered `turbo_orm` or `orm_postgresql`, and review the diff against this plan and spec.
