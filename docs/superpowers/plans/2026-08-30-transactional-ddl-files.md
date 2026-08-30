# Transactional DDL Files Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 TBE 生成的 SQLite/PostgreSQL DDL 文件自带标准事务边界，并让 TurboDB DBTools 正确执行包含 `CREATE` 或 `DROP` 的文件内事务。

**Architecture:** TurboParser 是生成 DDL 的事实源，在双方言模板中输出 `BEGIN; ... COMMIT;`。DBTools 不生成、解析、重写或包装 SQL，SQLite 与 PostgreSQL driver 都直接执行文件定义的事务。Flowie 删除调用方的重复事务包装。

**Tech Stack:** C11、SQLite3、libpq、Mustache、TinyTest、CMake Presets。

**Spec:** 用户要求按标准 DDL 实现：DDL 文件本身包含事务，`CREATE` 与 `DROP` 均在同一原子边界内。

## Global Constraints

- 不在 DBTools 中自行解析或生成 schema/DDL。
- 不为无事务旧脚本提供隐式外层事务或 fallback。
- 文件自带事务失败后由文件事务与连接关闭保证不留下已提交的部分 DDL。
- Flowie 运行时不得在标准 DDL 外再嵌套事务。
- 不新增 migration、drop planner 或隐式 destructive 操作。

---

### Task 1: TBE 标准事务 DDL

**Files:**
- Modify: `tbe/tbe_compiler/templates/sqlite_schema.mustache`
- Modify: `tbe/tbe_compiler/templates/postgresql_schema.mustache`
- Test: `tbe/tbe_compiler/test_tbe_compiler.c`

**Interfaces:**
- Produces: SQLite/PostgreSQL schema output beginning with `BEGIN;` and ending with `COMMIT;`.

- [x] **Step 1: Write failing golden assertions**
- [x] **Step 2: Run `test_tbe_compiler` and observe missing transaction boundaries**
- [x] **Step 3: Add the minimal template prefix/suffix**
- [x] **Step 4: Run focused and full TurboParser tests**

### Task 2: DBTools 直接执行标准 DDL

**Files:**
- Modify: `dbtools/src/sqlite/dbtool_sqlite.c`
- Test: `dbtools/tests/dbtool_sqlite_test.c`
- Modify: `dbtools/tests/fixtures/sqlite_schema.sql`

**Interfaces:**
- Consumes: a borrowed, NUL-terminated SQL file buffer.
- Produces: direct execution of the transaction semantics present in the DDL file.

- [x] **Step 1: Replace the old rejection test with real BEGIN/CREATE/DROP/COMMIT behavior and failure rollback tests**
- [x] **Step 2: Run `dbtool_sqlite_test` and observe the expected unsupported failure**
- [x] **Step 3: Remove the outer transaction and transaction-control authorizer; execute the file directly**
- [x] **Step 4: Close the connection after apply so an unsuccessful file transaction is rolled back by the database**
- [x] **Step 5: Run SQLite DBTools and package contract tests**

### Task 3: PostgreSQL standard DDL contract and documentation

**Files:**
- Test: `dbtools/tests/dbtool_postgresql_test.c`
- Test: `dbtools/tests/postgresql_live_test.c`
- Modify: `README.md`
- Modify: `docs/architecture/driver-data-tools.md`

**Interfaces:**
- Consumes: transaction-bearing DDL through one libpq simple query.
- Produces: drained results and non-zero status on any failed DDL statement.

- [x] **Step 1: Add transaction-bearing create/drop fixtures and result assertions**
- [x] **Step 2: Run focused PostgreSQL fake test**
- [x] **Step 3: Document that generated files own transactions and DBTools do not add atomicity**

### Task 4: Flowie consumption and end-to-end verification

**Files:**
- Modify: `control/flowie_control_store.c`
- Test: `control/tests/test_flowie_control_store_schema.c`
- Test: `control/tests/test_flowie_control_store.c`

**Interfaces:**
- Consumes: generated DDL that owns `BEGIN/COMMIT`.
- Produces: one schema execution call; rollback cleanup only after failed execution.

- [x] **Step 1: Update schema contract tests to execute the standard file directly**
- [x] **Step 2: Remove the outer runtime BEGIN/COMMIT/ROLLBACK entirely**
- [x] **Step 3: Reinstall TurboParser, regenerate Flowie DDL and run focused Control tests**
- [x] **Step 4: Run full TurboParser, TurboDB and Flowie test suites plus scoped `git diff --check`**

No commits, pushes or merges are performed unless explicitly requested.
