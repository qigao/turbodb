# TurboDB ORM

TurboDB ORM is a typed, demand-driven database facade for C11. Query results
are exposed only as CFlow Sources: drivers publish one row-local CSerde reader,
CBind decodes it into an owning CMeta value, and downstream demand controls
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
  -> CBind typed value
  -> CFlow Source / Graph / Sink
```

Row queries use `orm_query_open_flow`. Mutating or DDL queries use
`orm_query_open_command_flow`, which emits one `orm_command_result_t` with
`CFLOW_STEP_VALUE_AND_DONE`. Opening a Source does not execute a command;
execution begins on the first unit of demand.

The active cursor is the fact source. No second result matrix is built in the
ORM. PostgreSQL uses libpq single-row mode, MongoDB uses its native cursor,
Redis parses the network RESP array one owned row at a time, SQLite advances
the prepared statement, and TidesDB advances its iterator.

Each backend selects CSerde token kinds from its own native metadata. CBind
does not guess numeric or boolean values from arbitrary strings. PostgreSQL
uses field OIDs; Redis uses the declared CMeta field kind for typeless RESP bulk
strings. A malformed declared scalar fails the Source instead of being returned
as text.

## C API

Load the installed Orm package, include `orm.h`, and link `Orm::C`. Consumers
never find backend packages directly. The installed shared Orm closes backend
linkage and runtime packaging inside the library boundary.

```cmake
find_package(Orm CONFIG REQUIRED)
target_link_libraries(app PRIVATE Orm::C)
```

```c
#include <orm.h>

orm_error_t error;
orm_config_t config;
orm_option_t filename;
orm_connection_t *connection = NULL;

orm_error_init(&error);
orm_config(&config);
filename.keyword = orm_view("filename");
filename.value = orm_view(":memory:");
config.driver = orm_view("sqlite");
config.options = &filename;
config.option_count = 1u;

if (orm_connect(&config, &connection, &error) != ORM_STATUS_OK) {
  /* error.status and error.message describe the failed boundary. */
}
```

### Typed row Source

Row execution requires a `cmeta_data_desc`. Generate production descriptors
with TurboParser TBE `--cbind-output`; small tests may define a descriptor
directly with CMeta. The descriptor field names are matched against driver row
keys.

```c
orm_query_t *query = NULL;
orm_flow_config_t flow_config;
cflow_source source = {0};
my_row row = {0};
cflow_step step;

orm_query_create(connection, orm_view("person"), &query, &error);
orm_query_add_column(query, orm_view("id"), &error);
orm_query_add_column(query, orm_view("name"), &error);
orm_query_order_by(query, orm_view("id"), ORM_ORDER_ASCENDING, &error);

orm_flow_config(&flow_config, my_row_cbind_data());
if (orm_query_open_flow(query, &flow_config, &source, &error) == ORM_STATUS_OK) {
  while ((step = cflow_source_resume(&source, NULL, &row)).kind ==
         CFLOW_STEP_VALUE) {
    consume_row(&row);
  }
  if (step.kind == CFLOW_STEP_ERROR) {
    consume_error(step.error);
  }
  cflow_source_destroy(&source);
}
orm_query_destroy(query);
```

For graph execution, move the Source into a `cflow_run` and request bounded
downstream demand. Direct `cflow_source_resume` is useful for synchronous
adapters and tests; it does not replace scheduler-driven execution for Sources
that can return `CFLOW_STEP_WAIT`.

### Command Source

```c
orm_query_t *command = NULL;
cflow_source source = {0};
orm_command_result_t result = ORM_COMMAND_RESULT_INIT;

orm_insert(connection, orm_view("person"), &command, &error);
orm_query_set(command, orm_view("id"), orm_i64(7), &error);
orm_query_set(command, orm_view("name"), orm_text("Alice"), &error);

if (orm_query_open_command_flow(command, &source, &error) == ORM_STATUS_OK) {
  cflow_step step = cflow_source_resume(&source, NULL, &result);
  if (step.kind == CFLOW_STEP_VALUE_AND_DONE) {
    observe_affected_rows(result.affected_rows);
  }
  cflow_source_destroy(&source);
}
orm_query_destroy(command);
```

Cancelling or destroying a command Source before demand prevents execution.
After demand begins, backend-specific uncertainty rules apply; Redis mutation
transport failures are reported as an unknown outcome and must not be retried
blindly.

### Ownership and limits

- `out_source` must be zero-initialized and unoccupied.
- A successful open transfers the driver cursor to the Source.
- The query, connection, row descriptor, and reachable descriptor metadata must
  outlive the Source. A transaction Source also borrows its transaction.
- Each successfully moved cursor is destroyed exactly once by Source teardown.
- Row token views expire before the next cursor advance. CBind output is an
  owning value governed by its CMeta traits.
- `max_result_rows`, `max_result_bytes`, `max_columns`, CBind scratch size,
  nesting depth, container items, and buffer bytes are hard limits.
- Errors are fail-fast; there is no schema-less or eager fallback.

## C++ wrapper

`orm.hpp` does not implement another ORM. It owns C handles, converts failed C
status codes to `orm::status_error`, and retains the query for the lifetime of
the returned Source.

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
    .open<person_row>(*person_row_cbind_data());

person_row row{};
for (cflow_step step = rows.next(row);
     step.kind == CFLOW_STEP_VALUE;
     step = rows.next(row)) {
  consume(row);
}
```

`query::open` and `query::execute` are rvalue-qualified because ownership of the
query moves into `orm::source<Row>`. The connection and any transaction still
must outlive that Source.

## Backends

- SQLite: row and command Sources; explicit transactions and savepoints.
- PostgreSQL: libpq single-row row Source, direct command completion and
  affected-row parsing; explicit transactions and savepoints. Field OIDs select
  supported scalar token kinds; arbitrary-precision and unknown types remain
  strings instead of being narrowed.
- MongoDB: native row cursor and direct insert/update/delete commands;
  transactions require a deployment that supports MongoDB sessions.
- TidesDB: iterator-backed row Source and direct commands. Stateful ordering,
  grouping and aggregation are not executed eagerly by the backend; express
  them as bounded CFlow operators when pushdown cannot preserve semantics.
  SELECT requires an explicit projection. Iterator scans are capped by
  `max_scan_rows` and `max_scan_bytes`; transaction commit/rollback returns
  `ORM_STATUS_BUSY` while a transaction row Source is open.
- Redis: Redis Query Engine row Source and direct mutation commands. ORM-level
  `MULTI/EXEC` is currently unsupported because affected rows are unavailable
  until commit; it requires a future commit-aware Source protocol. SELECT is
  lazy at first demand and does not materialize the complete RESP reply.
  Projected CMeta scalar kinds drive strict conversion of RESP bulk strings;
  non-canonical numeric or boolean representations fail at the cursor boundary.
  Destroying a Source before completion disconnects that client so unread
  response bytes cannot corrupt the next command.

Backend options are validated at connection creation. Unknown options are
rejected instead of silently enabling a fallback.

### PostgreSQL component

PostgreSQL is an explicit optional component rather than part of
`turbo_orm`. It is exported by the same Orm package, so consumers still find
only Orm and link the component target when needed:

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

Backend CMake options are `ORM_WITH_SQLITE`, `ORM_WITH_PGSQL`,
`ORM_WITH_REDIS`, `ORM_WITH_MONGODB`, and `ORM_WITH_TIDESDB`. Optional backends
remain opt-in.

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

The cursor and Source lifecycle tests use TinyTest; driver boundary tests use
TinyMock where a native server is unnecessary. TidesDB also has a public
end-to-end temporary-database test.

See `docs/architecture/orm-cflow-c-core.md` for the ownership, backpressure,
failure, and migration decision.
