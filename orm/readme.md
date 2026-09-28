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
ORM. PostgreSQL uses libpq single-row mode, MongoDB uses its native cursor,
Redis parses the network RESP array one owned row at a time, SQLite advances
the prepared statement, and TidesDB advances its iterator.

Each backend selects CSerde token kinds from its own native metadata. DataBind
does not guess numeric or boolean values from arbitrary strings. PostgreSQL
uses field OIDs; Redis uses the declared CMeta field kind for typeless RESP bulk
strings. A malformed declared scalar fails the Publisher instead of being returned
as text.

## C API

Load the installed Orm package, include `orm.h` /
`orm_runtime.h`, and link `Orm::C`. The generic shared core owns query
planning, result/Publisher plumbing and the canonical Driver runtime.
Database-native dependencies stay in Driver modules rather than the generic
core.

```cmake
find_package(Orm CONFIG REQUIRED)
target_link_libraries(app PRIVATE Orm::C)
```

New connections use an explicit `orm_runtime_t` and an explicitly supplied
Driver module path. The installed `Orm` CMake package does not advertise
SQLite/PostgreSQL capability flags because Driver deployment is independent of
the core package.


### Runtime-loaded Drivers

SQLite, PostgreSQL, Redis, TidesDB, and MongoDB ORM adapters are explicit `TurboDb.Driver` Plugin modules.
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
load.module_path = orm_view("/absolute/path/to/turbodb_driver_redis");
load.expected_driver_id = orm_view("redis");
orm_runtime_load_driver(runtime, &load, &error);

orm_config(&config);
config.driver = orm_view("redis");
/* configure host/port/options */
orm_runtime_connect(runtime, &config, &connection, &error);

/* destroy all Publishers/queries/transactions, then disconnect first */
orm_disconnect(connection);
orm_runtime_close(runtime, &error);
orm_runtime_release(runtime);
```

The Driver keeps its Plugin lease while connection-owned native work is live.
MongoDB's native process runtime is owned by the managed MongoDB Plugin:
`mongoc_init()` runs from Plugin start and `mongoc_cleanup()` runs only after
all Driver leases quiesce and before module unload. No process-global
`atexit` callback is registered by the ORM adapter.
Unsupported Redis transaction/raw-SQL operations remain explicit backend
errors; they are not emulated by another database.

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
After demand begins, backend-specific uncertainty rules apply; Redis mutation
transport failures are reported as an unknown outcome and must not be retried
blindly.

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
- MongoDB: native row cursor and direct insert/update/delete commands;
  transactions require a deployment that supports MongoDB sessions.
- TidesDB: iterator-backed row Publisher and direct commands. Stateful ordering,
  grouping and aggregation are not executed eagerly by the backend; express
  them as bounded CFlow operators when pushdown cannot preserve semantics.
  SELECT requires an explicit projection. Iterator scans are capped by
  `max_scan_rows` and `max_scan_bytes`; transaction commit/rollback returns
  `ORM_STATUS_BUSY` while a transaction row Publisher is open.
- Redis: Redis Query Engine row Publisher and direct mutation commands. ORM-level
  `MULTI/EXEC` remains unsupported because affected rows are unavailable until
  commit; it requires a future commit-aware Publisher protocol. The additive
  `tedis` `redis_lua_apply` API is instead a fixed Redis-native ordered-write
  primitive for replicated state adapters: it commits one hash mutation,
  applied-index metadata, and a Stream outbox entry atomically. It is not an
  ORM transaction or an arbitrary-query escape hatch. SELECT is lazy at first
  demand and does not materialize the complete RESP reply.
  Projected CMeta scalar kinds drive strict conversion of RESP bulk strings;
  non-canonical numeric or boolean representations fail at the cursor boundary.
  Destroying a Publisher before completion disconnects that client so unread
  response bytes cannot corrupt the next command.

Backend options are validated at connection creation. Unknown options are
rejected instead of silently enabling a fallback.

### Legacy PostgreSQL compatibility component (2.1.x)

For 2.1.x source compatibility, `ORM_BUILD_LEGACY_POSTGRESQL_COMPONENT=ON`
may additionally export `Orm::PostgreSQL` and `orm_postgresql_connect()`.
This is not the runtime Driver architecture and new code should not use it.
It is retained only as a migration component and is scheduled for removal at
the 3.0 connection-API cutover:

```cmake
find_package(Orm CONFIG REQUIRED)
target_link_libraries(app PRIVATE Orm::PostgreSQL)
```

```c
#include <orm_postgresql.h>

orm_config_t config;
orm_connection_t *connection = NULL;
orm_error_t error;

orm_config(&config);
config.driver = orm_view("postgresql");
/* Supply a borrowed "conninfo" option for this call. */
if (orm_postgresql_connect(&config, &connection, &error) != ORM_STATUS_OK) {
  /* Consume the error at the application boundary. */
}
```

C++ consumers include `orm_postgresql.hpp` and call
`orm::postgresql_connection(config)`. This is an inline wrapper over the same C
connector; there is no C++ implementation library.

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

Runtime Driver build options are `ORM_BUILD_SQLITE_DRIVER`,
`ORM_BUILD_POSTGRESQL_DRIVER`, `ORM_BUILD_REDIS_DRIVER`,
`ORM_BUILD_TIDESDB_DRIVER`, and `ORM_BUILD_MONGODB_DRIVER`. The optional
`ORM_BUILD_LEGACY_POSTGRESQL_COMPONENT` switch controls only the 2.1.x direct
connector compatibility target. Driver options do not change the generic
`Orm::C` package contract or put native database libraries into its link
closure.

```sh
cmake --preset win-dev-user
cmake --build --preset win-dev-user
ctest --preset win-dev-user --output-on-failure
```

The PostgreSQL live gate is opt-in and fail-fast. Set a non-empty conninfo in
the environment before configuring the dedicated preset; the value is never
printed by the test:

```powershell
$env:TURBODB_ORM_PGSQL_TEST_CONNINFO = 'host=127.0.0.1 port=5432 dbname=turbodb user=turbodb password=...'
cmake --preset win-release-pg-live-user --fresh
cmake --build --preset win-release-pg-live-user
ctest --preset win-release-pg-live-user -R '^orm_postgres_live$' --output-on-failure
```

The cursor and Publisher lifecycle tests use TinyTest; driver boundary tests use
TinyMock where a native server is unnecessary. TidesDB also has a public
end-to-end temporary-database test.

See `docs/architecture/orm-cflow-c-core.md` for the ownership, backpressure,
failure, and migration decision.
