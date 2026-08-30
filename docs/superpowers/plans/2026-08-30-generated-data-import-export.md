# Generated Data Import/Export Tools Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 由一份 TBE database schema 和一个选定 driver 生成 schema-specific、无 ORM 的数据导入/导出 executable。

**Architecture:** TurboParser 的 `tbe_compiler` 生成 versioned pure-C model adapter；TurboDB 提供 record source/sink、单线程有界 transfer core 和安装后的 CMake helper。generated target 才链接 `TurboParser::DataBind`，普通 TurboDB/ORM/schema tools 不新增该依赖。

**Tech Stack:** C11、TBE database IR、DataBind streaming/typed descriptors、SQLite3、libpq、TurboUtils Core、CMake Presets、TinyTest。

**Specs:** TurboDB `docs/architecture/driver-data-tools.md`; TurboParser `docs/architecture/tbe-database-ddl-generation.md`.

## Global constraints

- 这是 schema tools 完成后的独立第二项目；第一阶段接口未完成前不得暴露 `data import/export` help。
- TurboParser 不链接 TurboDB；它只生成消费 `dbtool_model_v1` 的 C source。
- generated production sources、core 和 drivers 都是 C11；C++ 只允许测试或 header wrapper。
- import 默认单 transaction/all-or-nothing；export 默认 temp-file + atomic replace。
- format、driver 或 field kind 不支持时 fail fast，不通过 JSON/字符串 fallback。

### Task 1: Define the versioned model/record contracts in TurboDB

**Repository: TurboDB**

**Files:**
- Create: `dbtools/include/turbodb/dbtool_model.h`
- Create: `dbtools/src/data/dbtool_record.h`
- Create: `dbtools/src/data/dbtool_transfer.h`
- Create: `dbtools/src/data/dbtool_transfer.c`
- Create: `dbtools/tests/dbtool_transfer_test.c`
- Modify: `dbtools/CMakeLists.txt`

**Interfaces:**
- `dbtool_model_v1`: immutable table/column descriptors plus input-decode/output-encode callbacks.
- `dbtool_record_sink_ops`: begin/write/commit/rollback/close.
- `dbtool_record_source_ops`: open/next/close with borrowed cells.

- [x] **Step 1: Write failing fake model/source/sink tests**

Test one transaction success, parse failure before first row, sink failure after one row, commit failure, rollback failure preserving the primary error, source failure, and exactly-once close. Assert borrowed input/cells are consumed before callback/next returns.

- [x] **Step 2: Write capacity tests**

Cover 0/1/exact/+1/max for chunk, rows, columns, cell bytes, record bytes and output bytes, including checked multiplication/addition. No test computes expected limits through production helpers.

- [x] **Step 3: Run RED**

Expected compile failure because the contracts do not exist.

- [x] **Step 4: Implement the single-threaded state machines**

Document and enforce `NEW -> OPEN -> ACTIVE -> COMMITTED|ROLLED_BACK -> CLOSED`. A successful begin reaches exactly one commit/rollback. The core never retains model callback views or driver cursor cells across their validity boundary.

- [x] **Step 5: Run GREEN and commit**

```text
git add dbtools/include dbtools/src/data dbtools/tests/dbtool_transfer_test.c dbtools/CMakeLists.txt
git commit -m "feat(dbtools): add generated model transfer contracts"
```

### Task 2: Extend TBE database projection IR

**Repository: TurboParser**

**Files:**
- Modify: `tbe/tbe_compiler/database_schema.h`
- Modify: `tbe/tbe_compiler/database_schema.c`
- Modify: `tbe/tbe_compiler/test_tbe_compiler.c`
- Modify: `docs/architecture/tbe-database-ddl-generation.md`

**Interfaces:**
- Adds stable model metadata beside existing SQL fragments: message/field name, raw table/column name, normalized scalar kind, optional/default/generated flags and deterministic indexes.
- Preserves existing built-in/custom DDL template output byte-for-byte.

- [x] **Step 1: Add failing IR contract tests**

Use a literal schema covering signed/unsigned widths, bool, float/double, string, bytes, UUID, enum, optional/default/generated and `db_ignore`. Assert exact IR values and that ignored/non-table fields are absent.

- [x] **Step 2: Run RED**

Run only `test_tbe_compiler`; expected missing IR fields.

- [x] **Step 3: Populate normalized metadata once**

Reuse the same validated field/type decisions that produce DDL. Do not reparse annotation strings in templates or maintain a second type-mapping table. Keep SQL names and logical names distinct.

- [x] **Step 4: Prove DDL compatibility**

Run every existing SQLite/PostgreSQL golden test and compare outputs byte-for-byte. Custom template stable fields remain valid.

- [x] **Step 5: Commit in TurboParser**

```text
git add tbe/tbe_compiler docs/architecture/tbe-database-ddl-generation.md
git commit -m "feat(tbe): expose database projection metadata"
```

### Task 3: Generate the pure-C model adapter

**Repository: TurboParser**

**Files:**
- Modify: `tbe/tbe_compiler/main.c`
- Modify: `tbe/tbe_compiler/generator.h`
- Modify: `tbe/tbe_compiler/generator.c`
- Create: `tbe/tbe_compiler/database_tool_generator.c`
- Create: `tbe/tbe_compiler/database_tool_generator.h`
- Modify: `tbe/tbe_compiler/test_tbe_compiler.c`
- Modify: `tbe/tbe_compiler/CLI_OPTIONS.md`
- Modify: `docs/architecture/tbe-database-ddl-generation.md`

**Interfaces:**
- Adds `--db-tool-source <file>` valid only with `--lang sqlite|postgresql|postgres` and `--output`.
- Generated file exports exactly one `dbtool_generated_model_v1()` symbol and includes installed `turbodb/dbtool_model.h` plus DataBind typed APIs.

- [x] **Step 1: Write failing CLI/golden tests**

Cover missing `--output`, non-database language, duplicate output paths, unknown dialect, deterministic generated C, escaped identifiers/default strings and no-table schema. Invalid combinations must not leave partial output files.

- [x] **Step 2: Run RED**

Expected CLI rejection because `--db-tool-source` is unknown.

- [x] **Step 3: Generate projection structs/descriptors and callbacks**

Emit one projection type per `db_table`, containing only persisted scalar fields plus explicit presence state. Import callbacks use DataBind streaming/typed conversion; export callbacks serialize a projection filled from database cells. Generate dialect conversions for SQLite canonical decimal `uint64`, PostgreSQL numeric, BLOB/bytea, bool and UUID. Unsupported kinds were already rejected by database IR and receive no fallback branch.

- [x] **Step 4: Enforce atomic output and production-source rules**

Use the compiler's existing same-directory temp + replace path. Generated output is complete C11 with no unresolved placeholder/TODO and compiles under strict warnings.

- [x] **Step 5: Run generated-code compile/round-trip tests**

Compile generated source against installed TurboDB/TurboParser headers. Round-trip literal min/max/null/bytes/UUID values through every generated callback without a database.

- [x] **Step 6: Commit in TurboParser**

```text
git add tbe/tbe_compiler docs/architecture/tbe-database-ddl-generation.md
git commit -m "feat(tbe): generate database data-tool adapters"
```

### Task 4: Implement SQLite record source/sink

**Repository: TurboDB**

**Files:**
- Create: `dbtools/src/sqlite/dbtool_sqlite_records.c`
- Create: `dbtools/src/sqlite/dbtool_sqlite_records.h`
- Create: `dbtools/tests/dbtool_sqlite_records_test.c`
- Modify: `dbtools/CMakeLists.txt`

- [x] **Step 1: Write failing real-SQLite round-trip tests**

Cover each supported scalar, NULL, generated/default fields, canonical `uint64` max, bytes with NUL, constraint failure, malformed decimal text, sink rollback after a later bad row, and cursor cell invalidation on next step.

- [x] **Step 2: Run RED**

Expected missing record driver symbols.

- [x] **Step 3: Implement prepared/bound writes and cursor reads**

Quote only generated/validated identifiers. Prepare insert/select once per table operation, bind using generated scalar kinds, step row-by-row, and finalize exactly once. One import transaction owns all writes until commit/rollback.

- [x] **Step 4: Run GREEN and commit**

```text
git add dbtools/src/sqlite/dbtool_sqlite_records.* dbtools/tests/dbtool_sqlite_records_test.c dbtools/CMakeLists.txt
git commit -m "feat(dbtools): add SQLite record transfer driver"
```

### Task 5: Implement PostgreSQL record source/sink

**Repository: TurboDB**

**Files:**
- Create: `dbtools/src/postgresql/dbtool_postgresql_records.c`
- Create: `dbtools/src/postgresql/dbtool_postgresql_records.h`
- Create: `dbtools/tests/dbtool_postgresql_records_test.c`
- Create: `dbtools/tests/postgresql_data_live_test.c`
- Modify: `dbtools/tests/fake_libpq.c`
- Modify: `dbtools/CMakeLists.txt`

- [ ] **Step 1: Write failing fake/live tests**

Fake tests cover parameter OIDs/formats, NULL, bytea binary, numeric `uint64`, single-row mode, result drain and rollback. Live test covers exact scalar round trips and a later-row constraint failure leaving zero imported rows.

- [ ] **Step 2: Run RED**

Expected missing record driver symbols.

- [ ] **Step 3: Implement parameterized sink and single-row source**

Use one prepared parameterized insert per table and libpq single-row mode for export. Do not store `PQgetvalue` views after clearing the owning result. Drain terminal/error results before returning.

- [ ] **Step 4: Run GREEN and commit**

```text
git add dbtools/src/postgresql dbtools/tests dbtools/CMakeLists.txt
git commit -m "feat(dbtools): add PostgreSQL record transfer driver"
```

### Task 6: Generated executable CMake helper and format CLI

**Repository: TurboDB**

**Files:**
- Create: `cmake/TurboDBDataTools.cmake`
- Modify: `cmake/TurboDBConfig.cmake.in`
- Create: `dbtools/src/data/main.c`
- Create: `dbtools/src/data/dbtool_format.c`
- Create: `dbtools/src/data/dbtool_format.h`
- Create: `dbtools/tests/data_tool_consumer/CMakeLists.txt`
- Create: `dbtools/tests/data_tool_consumer/accounts.schema`
- Create: `dbtools/tests/data_tool_consumer/round_trip.cmake`
- Modify: `dbtools/CMakeLists.txt`

**Interfaces:**

```cmake
turbodb_add_data_tool(
  NAME accounts-sqlite
  DRIVER sqlite
  SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/accounts.schema")
```

- [ ] **Step 1: Write failing installed-consumer test**

The consumer uses only installed TurboDB/TurboParser roots, generates DDL/model source, builds a schema-specific executable, applies schema, imports literal records and exports them for behavioral comparison.

- [ ] **Step 2: Run RED**

Expected unknown `turbodb_add_data_tool`.

- [ ] **Step 3: Implement exact package discovery**

The helper validates `TURBOPARSER_ROOT`, uses `find_package(TurboParser CONFIG REQUIRED ... NO_DEFAULT_PATH)`, finds installed `tbe_compiler` only below that root, generates DDL and adapter with absolute tool path, and links only the selected driver plus `TurboParser::DataBind`.

- [ ] **Step 4: Implement bounded formats**

Expose a format only after complete implementation: JSON root array, CSV header/rows, YAML root sequence, XML records root and length-prefixed TBE binary records. Import feeds fixed chunks and uses callback-only output. Export writes a same-directory temp file and atomically replaces the target only after a complete closing delimiter/flush.

- [ ] **Step 5: Run full format/driver matrix**

For SQLite and PostgreSQL, round-trip all supported formats and values. Inject truncated input, malformed record N, output limit, disk write failure and DB constraint failure; assert rollback/no target replacement and exact row/error stage.

- [ ] **Step 6: Commit**

```text
git add cmake dbtools
git commit -m "feat(dbtools): generate schema-specific data tools"
```

### Task 7: Documentation and final cross-repository verification

**Repository: TurboDB and TurboParser**

**Files:**
- Modify: TurboDB `README.md`
- Modify: TurboDB `docs/TURBODB_LINUX_REMOTE_TEST_RUNBOOK.md`
- Modify: TurboDB `docs/architecture/driver-data-tools.md`
- Modify: TurboParser `tbe/tbe_compiler/CLI_OPTIONS.md`
- Modify: TurboParser `docs/architecture/tbe-database-ddl-generation.md`

- [ ] **Step 1: Document commands, ownership and partial-state guarantees**

Include CMake generation, installed roots, per-format framing, import transaction semantics, output replacement, hard limits, conninfo secret handling and unsupported backend capability behavior.

- [ ] **Step 2: Windows Release verification**

Fresh configure/build/test/install TurboParser, then TurboDB, then installed consumer. Run `git diff --check` in both repositories.

- [ ] **Step 3: EU Linux Docker verification**

Build/install both exact revisions, generate two tools, run SQLite locally and PostgreSQL in a uniquely named container, write JUnit/log/SHA-256 evidence, and clean only run-scoped resources.

- [ ] **Step 4: Dependency/source audit**

Confirm ordinary TurboDB and schema-only executables do not find/link TurboParser; only generated data executables link DataBind. Confirm every production target contains no `.cpp`, no ORM/CFlow link, no runtime driver registry and no unbounded input/output path.
