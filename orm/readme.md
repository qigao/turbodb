# TurboDB ORM

TurboDB ORM is a typed, demand-driven database facade for C11. Query results
are exposed only as CFlow Publishers: drivers publish one row-local CSerde reader,
DataBind decodes it into an owning CMeta value, and downstream demand controls
cursor advancement. The C++17 header is a small RAII and exception wrapper over
the same C API.

This API is intentionally incompatible with the former eager ORM. There is no
`orm_result_t`, index-based cell access, chain/JPA/model facade, repository
runtime, or schema-less materialized row.

## Execution model

```text
C query plan
  -> database cursor
  -> CSerde row reader
  -> DataBind typed value
  -> CFlow Publisher / Graph / Subscriber
```

Row queries use `orm_query_open_flow`. Mutating or DDL queries use
`orm_query_open_command_flow`, which emits one `orm_command_result_t` with
`CFLOW_STEP_VALUE_AND_DONE`. Opening a Publisher does not execute a command;
execution begins on the first unit of demand.

The active cursor is the fact source. No second result matrix is built in the
ORM. PostgreSQL uses libpq single-row mode, SQLite advances the prepared
statement, and TidesDB advances its iterator.

Each backend selects CSerde token kinds from its own native metadata. DataBind
does not guess numeric or boolean values from arbitrary strings. PostgreSQL
uses field OIDs; a malformed declared scalar fails the Publisher instead of
being returned as text.

## C API

Add TurboDB to the build with `add_subdirectory`, include `orm.h` /
`orm_runtime.h`, and link `Orm::C`. The generic shared core owns query
planning, result/Publisher plumbing and the canonical Driver runtime.
Database-native dependencies stay in Driver modules rather than the generic
core.

```cmake
target_link_libraries(app PRIVATE Orm::C)
```

New connections use an explicit `orm_runtime_t` and an explicitly supplied
Driver module path. Driver deployment is independent of the core library.
The build no longer generates an `OrmConfig.cmake` package; `Orm::C` and
`Orm::Cpp` are targets for source-tree integration.

### Plugin architecture and ownership

The runtime uses the installed `Salts::Plugin` implementation and its ABI 4
contract. `SALTS_ROOT` and `SALTS_UTILS_ROOT` in the selected user preset select
the SDKs. The build requires both `Salts::Plugin` and `Salts::PluginABI`; it does
not compile a private copy of the loader. `Orm::DriverABI` publishes the ORM
driver headers and their `Salts::PluginABI`, Core, CFlow and CSerde dependencies.
It does not link the host loader or a native database library.

Plugin ABI 4 is an exact admission epoch, including the CMeta reflection layouts
exposed by exports. The SDK rejects other Plugin epochs at compile time; the
host rejects old modules before consuming their descriptors. There is no ABI 3
negotiation, retry or compatibility path. Rebuild the host and all modules with
the same current Salts SDK, then deploy them together after active leases drain.
Clean compiled objects when upgrading the SDK epoch; an incremental relink can
retain descriptors compiled with the previous CMeta layout.
An admission failure leaves the runtime registration unchanged. Rollback requires
reverting and rebuilding the whole host/module deployment; do not mix epochs.
This cutover uses the existing loader and ownership architecture, avoiding a
second layout adapter or an alternate SDK lookup. Validate it with the C/C++
interface, real-module admission, registry race and database plugin tests.
The separate ORM Driver DTO ABI and database contract versions below retain
their own meanings; Plugin epoch numbers do not renumber those DTOs.

The ORM driver boundary accepts only Driver ABI 2 and `TurboDb.Driver` contract
version 4. Rebuild and deploy the core and all drivers together; older plugins
are rejected. `orm_driver_connection_ops_v2` must contain the complete operation
table, including a nullable `open_async_cursor` callback.

| Boundary | Responsibility |
| --- | --- |
| `src/abi` | Public C handles, plans, results and retained ownership |
| `src/flow` | Demand, cancellation, typed row decoding and command Publishers |
| `src/driver` | Driver contract validation and plan/backend adapters |
| `src/runtime/orm_runtime.c` | Runtime budgets, references and shutdown state |
| `src/runtime/orm_runtime_plugin.c` | Driver admission, metadata snapshots and extension leases |
| `src/runtime/orm_runtime_connection.c` | Leased connections, cursors and transactions |
| `src/sql` | Database-independent SQL rendering |
| `../drivers/<database>` | One native backend and its plugin CMake target |
| `../mysql` | Standalone `TurboDB::MySQL` client library |
| `driver-sdk` | The source-tree `Orm::DriverABI` target |

Each implementation directory declares its own sources in `CMakeLists.txt`.
The repository root builds `orm/`, then `drivers/`, then `orm/tests/`.
`orm/CMakeLists.txt` declares only the generic core, C++ facade and driver ABI.
The `drivers/` dispatcher builds SQLite, PostgreSQL, MySQL and TidesDB
from their own directories. Native MySQL code belongs to the root `mysql/`
library; its ORM adapter is in `drivers/mysql/`.
The shared core owns no native database dependency; each plugin owns its
native SDK links and install rule.
The private test core recompiles the production target's source selection with
the test ABI, so splitting a production module cannot silently drop it from
ownership tests. Unit/integration tests and E2E tests retain separate switches
and database directories.

Salts owns module identity, generation checks, lifecycle and leases. The ORM
stores a bounded index of admitted drivers plus copied metadata, rather than a
second module registry. Admission is serialized and validates the manifest ID,
`TurboDb.Driver` contract version and CMeta shape before publishing a slot.
All metadata copies occur while the admission lease is valid. A stored binding
is borrowed; invoking it requires a new live lease.

A connection retains a runtime dependent and a plugin lease. Cursors,
transactions and Publishers retain the connection through the existing owner
protocol. Destruction releases native resources first, then the plugin lease,
then the runtime dependent. Extension handles similarly retain a lease until
explicit release. Runtime counters and transitions use the runtime mutex;
plugin callbacks execute outside that lock. Limits remain in
`orm_runtime_config_t`; exhaustion returns the existing limit or busy status.

Shutdown stops admission and processes plugins in reverse registration order:
request stop, poll quiescence, unload. Active or pending operations return
`ORM_STATUS_BUSY`. A quiescing plugin leaves shutdown resumable; a cleanup
failure quarantines the runtime under the existing error callback policy.

This separation replaces the monolithic runtime without introducing another
loader or a new connection protocol. Keeping the monolith would preserve the
same coupling between control state and cursor execution; splitting into
separate public libraries would add ABI and deployment changes unnecessarily.
The chosen internal boundaries add no per-row dispatch or allocation. Existing
runtime APIs, module filenames, contract versions and caller-blocking
execution semantics remain unchanged. PostgreSQL connections use the runtime
plugin; the direct connector compatibility component has been removed.

Consumers rebuild against the installed SDKs. Applications using the removed
PostgreSQL direct connector must load the PostgreSQL plugin and connect through
`orm_runtime_connect`; database formats do not change. A rollback restores the previous
source layout and matching SDK installation together. Verification uses the
runtime registry/race tests, SQLite/PostgreSQL plugin tests, ownership tests and
the independent PostgreSQL E2E build. Live E2E execution additionally requires
the database environment described below.


### 客户端按需使用驱动

构建端提供完整的四个驱动；客户端链接 `Orm::C`（或 `Orm::Cpp`），只部署
并显式加载需要的插件。一个 runtime 可以加载多个驱动；未加载的驱动不连接、
不初始化，也不要求客户端链接其原生数据库库。选定插件使用的原生运行库仍需
由客户端部署环境提供。

插件文件名、`TurboDb.Driver` 契约及安装路径保持不变。每个插件拥有独立安装
component：`OrmSqliteDriver`、`OrmPostgresqlDriver`、`OrmMysqlDriver`、
`OrmTidesdbDriver`。这些 component 只选择插件产物，
不会自动打包 ORM、Salts 或数据库原生运行库。

本次目录调整选择将具体数据库实现移出 ORM，保留公共契约与通用适配代码在
ORM 内。继续放在 `src/dbs/` 会让核心继续承担驱动构建职责；拆成独立仓库或
新公共库则会引入当前不需要的版本与发布边界。目录调整不改变查询算法、
资源状态归属、错误码、插件 ABI 或运行时开销。

迁移只涉及 CMake 源码引用：核心先定义，驱动随后定义，测试最后引用现有
targets。驱动构建失败会直接终止构建，不产生自动降级路径。回退时应一起恢复
目录和 CMake 引用，无须迁移数据库数据。验证覆盖四个插件构建、单驱动加载、
多驱动共存的本地契约测试、核心 DLL 依赖检查及独立 E2E 构建。

### Runtime-loaded Drivers

SQLite, PostgreSQL, MySQL and TidesDB ORM adapters are explicit `TurboDb.Driver` Plugin modules.
The application loads an exact module path into an `orm_runtime_t`, then
connects by the canonical Plugin manifest ID. Runtime loading does not scan
directories, infer aliases, retry older ABIs, or fall back to a built-in
backend.

```c
#include <orm_runtime.h>

orm_runtime_config_t runtime_config;
orm_driver_load_config_t load = {0};
orm_runtime_t *runtime = NULL;

orm_runtime_config_init(&runtime_config);
orm_runtime_create(&runtime_config, &runtime, &error);

load.struct_size = sizeof(load);
load.abi_version = ORM_RUNTIME_ABI_VERSION;
load.module_path = orm_view("/absolute/path/to/turbodb_driver_sqlite");
load.expected_driver_id = orm_view("sqlite");
orm_runtime_load_driver(runtime, &load, &error);

orm_config(&config);
config.driver = orm_view("sqlite");
/* configure filename/options */
orm_runtime_connect(runtime, &config, &connection, &error);

/* destroy all Publishers/queries/transactions, then disconnect first */
orm_disconnect(connection);
orm_runtime_close(runtime, &error);
orm_runtime_release(runtime);
```

The Driver keeps its Plugin lease while connection-owned native work is live.

### Typed row Publisher

Row execution requires a `cmeta_data_desc`. Generate production descriptors through the canonical DataBind/CMeta codegen path;
small tests may define a descriptor directly with CMeta. The descriptor field names are matched against driver row
keys.

```c
orm_query_t *query = NULL;
orm_flow_config_t flow_config;
cflow_publisher publisher = {0};
my_row row = {0};
cflow_step step;

orm_query_create(connection, orm_view("person"), &query, &error);
orm_query_add_column(query, orm_view("id"), &error);
orm_query_add_column(query, orm_view("name"), &error);
orm_query_order_by(query, orm_view("id"), ORM_ORDER_ASCENDING, &error);

orm_flow_config(&flow_config, my_row_data());
if (orm_query_open_flow(query, &flow_config, &publisher, &error) == ORM_STATUS_OK) {
  while ((step = cflow_publisher_resume(&publisher, NULL, &row)).kind ==
         CFLOW_STEP_VALUE) {
    consume_row(&row);
  }
  if (step.kind == CFLOW_STEP_ERROR) {
    consume_error(step.error);
  }
  cflow_publisher_destroy(&publisher);
}
orm_query_destroy(query);
```

For graph execution, move the Publisher into a `cflow_subscription` and request
bounded downstream demand. Direct `cflow_publisher_resume` is useful for
synchronous adapters and tests; it does not replace scheduler-driven execution
for Publishers that can return `CFLOW_STEP_WAIT`.

### Command Publisher

```c
orm_query_t *command = NULL;
cflow_publisher publisher = {0};
orm_command_result_t result = ORM_COMMAND_RESULT_INIT;

orm_insert(connection, orm_view("person"), &command, &error);
orm_query_set(command, orm_view("id"), orm_i64(7), &error);
orm_query_set(command, orm_view("name"), orm_text("Alice"), &error);

if (orm_query_open_command_flow(command, &publisher, &error) == ORM_STATUS_OK) {
  cflow_step step = cflow_publisher_resume(&publisher, NULL, &result);
  if (step.kind == CFLOW_STEP_VALUE_AND_DONE) {
    observe_affected_rows(result.affected_rows);
  }
  cflow_publisher_destroy(&publisher);
}
orm_query_destroy(command);
```

Cancelling or destroying a command Publisher before demand prevents execution.
After demand begins, backend-specific uncertainty rules apply.

### Ownership and limits

- `out_publisher` must be zero-initialized and unoccupied.
- A successful open transfers the driver cursor to the Publisher.
- The query, connection, row descriptor, and reachable descriptor metadata must
  outlive the Publisher. A transaction Publisher also borrows its transaction.
- Each successfully moved cursor is destroyed exactly once by Publisher teardown.
- Row token views expire before the next cursor advance. DataBind output is an
  owning value governed by its CMeta traits.
- `max_result_rows`, `max_result_bytes`, `max_columns`, DataBind scratch size,
  nesting depth, container items, and buffer bytes are hard limits.
- Errors are fail-fast; there is no schema-less or eager fallback.

## C++ wrapper

`orm.hpp` does not implement another ORM. It owns C handles, converts failed C
status codes to `orm::status_error`, and retains the query for the lifetime of
the returned Publisher.

```cpp
#include <orm.hpp>

orm::config cfg("sqlite");
cfg.option("filename", ":memory:");
orm::connection db(cfg);

auto command = db.raw("create table person(id integer, name text)").execute();
orm_command_result_t created = ORM_COMMAND_RESULT_INIT;
auto command_step = command.next(created);

auto rows = db.select("person")
    .column("id")
    .column("name")
    .order_by("id", ORM_ORDER_ASCENDING)
    .open<person_row>(*person_row_data());

person_row row{};
for (cflow_step step = rows.next(row);
     step.kind == CFLOW_STEP_VALUE;
     step = rows.next(row)) {
  consume(row);
}
```

`query::open` and `query::execute` are rvalue-qualified because ownership of the
query moves into `orm::publisher<Row>`. The connection and any transaction still
must outlive that Publisher.

## Backends

- SQLite: row and command Publishers; explicit transactions and savepoints.
- PostgreSQL: libpq single-row row Publisher, direct command completion and
  affected-row parsing; explicit transactions and savepoints. Field OIDs select
  supported scalar token kinds; arbitrary-precision and unknown types remain
  strings instead of being narrowed.
- TidesDB: the TidesSQL relational engine serves raw SQL and structured ORM
  queries through row and command Publishers. Connections require explicit
  `path` and `column_family`; use `sql_initialize=true` only to initialize a new
  empty column family. Tables require explicit CREATE TABLE. The driver supports
  SERIALIZABLE transactions and savepoints; live result cursors block transaction
  completion. Query, scan and transaction budgets are enforced by TidesSQL.
  The `sql_profile` selector and the ORMTDB KV format are not supported.
  See the [TidesSQL contract](../tidessql/readme.md).

MongoDB and Redis are intentionally not ORM backends: they do not implement the
SQL contract expected by this layer. Use the independently selectable
`TurboDB::Redis` client and its Redis-native APIs for Redis workloads.

Backend options are validated at connection creation. Unknown options are
rejected instead of silently enabling a fallback.

### Composite keys

Composite keys are expressed as one atomic, bounded batch:

```c
const orm_key_part_t key[] = {
    {orm_view("domain_id"), orm_text("domain-a")},
    {orm_view("user_id"), orm_text("user-a")},
    {orm_view("group_id"), orm_text("group-a")}};

orm_query_where_key(query, key, 3u, &error);
```

The call copies column names and text/blob payloads. The input array and its
borrowed views may expire after the call; an invalid part leaves the query plan
unchanged.

## Build and test

ORM builds SQLite, PostgreSQL, MySQL and TidesDB drivers together.
There are no per-database ORM build switches. `TURBODB_BUILD_ORM` and
`TURBODB_BUILD_DBTOOLS` select the ORM and database-tool modules;
`TURBODB_BUILD_REDIS` independently selects the standalone Redis client.
The database tools always support SQLite and PostgreSQL. Enabling either module
selects both vcpkg manifest features before toolchain initialization.
The standalone Redis client and TidesDB are built from their repository modules;
Salts and SaltsUtils come from the installed SDKs selected by the preset.
Native database libraries stay outside the generic `Orm::C` link interface.

The root `mysql/` directory provides the standalone `TurboDB::MySQL` static
client and installed headers, without ORM dependencies. `drivers/mysql/`
owns the `orm_mysql_driver` MODULE and links that client; its output remains
`turbodb_driver_mysql`. Both are built when `TURBODB_BUILD_ORM` is enabled.
Native API tests live in `mysql/tests/`; ORM contract and integration tests
remain under `orm/tests/`. See [MySQL client boundaries](../mysql/readme.md).

```sh
cmake --preset win-dev-user
cmake --build --preset win-dev-user
ctest --preset win-dev-user --output-on-failure
```

`BUILD_TESTS` controls unit tests and local contract/integration checks.
`orm/tests/CMakeLists.txt` dispatches to `abi/`, `driver/`, `flow/`,
`integration/`, and `ownership/`, each with its own `CMakeLists.txt`.
Driver tests are further grouped into `mysql/`, `postgresql/`, `sqlite/`,
`tidesdb/`, and `cross_driver/`, each with its own `CMakeLists.txt`.
Generic driver and runtime tests remain directly under `driver/`.
Database-specific flow and ABI tests also have their own subdirectories.
Integration tests are split into `integration/sqlite/` and
`integration/tidesdb/`. Each directory declares its target sources and test
properties directly; parent files only select and add the relevant directories.
Shared test headers are provided by the `support/` interface target.
`BUILD_E2E_TESTS` independently controls tests against external databases and
defaults to OFF. ORM E2E tests live in per-driver subdirectories of
`orm/tests/e2e/`, each with its own `CMakeLists.txt`.
Dbtools tests are organized under `dbtools/tests/sqlite/` and
`dbtools/tests/pgsql/`; PostgreSQL E2E cases have a separate `pgsql/e2e/`
directory and `CMakeLists.txt`.
Cross-database ORM cases live in `orm/tests/e2e/cross_driver/`.
All E2E tests carry the CTest `e2e` label. `BUILD_E2E_TESTS` builds the suites
for the enabled ORM and dbtools modules, independently of `BUILD_TESTS`.

Windows uses `win-dev-user` or `win-release-user` for both ordinary tests and
E2E builds. To build E2E tests, set `BUILD_E2E_TESTS` to `true` in the selected
configure preset's `cacheVariables` in `CMakeUserPresets.json`, then configure
and build with that preset. The default remains OFF. Use CTest labels and names
to select the tests to run. Connection settings are read from the environment
when the tests run, so building them needs no live server.
The ORM tests use `TURBODB_ORM_PGSQL_TEST_CONNINFO`; the dbtools test uses
`TURBODB_DBTOOLS_PG_TEST_CONNINFO`. Use disposable test databases because these
tests create and remove test tables.

```powershell
$env:TURBODB_ORM_PGSQL_TEST_CONNINFO = 'host=127.0.0.1 port=5432 dbname=turbodb user=turbodb password=...'
$env:TURBODB_DBTOOLS_PG_TEST_CONNINFO = $env:TURBODB_ORM_PGSQL_TEST_CONNINFO
cmake --preset win-release-user
cmake --build --preset win-release-user
ctest --preset win-release-user -L e2e -R '^(dbtool_postgresql_live_test|orm_postgres_live|orm_owner_postgres|orm_owner_postgres_loss|orm_sql_validation_parity)$' --output-on-failure
```

To run only the dbtools PostgreSQL E2E case, use the same preset with
`-L e2e -R '^dbtool_postgresql_live_test$'`. Use `-LE e2e` to run ordinary tests
when E2E is enabled. MySQL handshake/auth parsing remains a unit test because
it uses in-memory fixtures.

The cursor and Publisher lifecycle tests use TinyTest; driver boundary tests use
TinyMock where a native server is unnecessary. TidesDB also has a public
end-to-end temporary-database test.

Stream/Reactive 管道、MySQL/PostgreSQL 原生异步与 SQLite 后台行查询的 API、调度、
背压与关闭约束见 [Stream / Async](stream-async.md)。
