# TurboDB

**Typed and bounded database/storage infrastructure for the Salts ecosystem.**

TurboDB provides C database components, C++ header-only wrappers, native-driver database tooling, Redis/TidesDB integrations, and optional ORM capabilities. It uses the installed [Salts](https://github.com/qigao/salts) SDK as its systems foundation while keeping storage semantics, drivers, durability behavior, and database-specific contracts owned by TurboDB.

**Tags:** C11 · C++17 · database · storage · ORM · Redis · SQLite · PostgreSQL · durability · async-io

## Built on Salts

TurboDB does not treat Salts as a generic utility dependency. Selected runtime-facing components reuse Salts so database work participates in the same type, ownership, execution, and error model as the rest of the ecosystem.

Depending on the component, TurboDB can reuse:

- **CMeta** for stable typed metadata and semantic identity at API boundaries.
- **CFlow** for bounded execution, waitables, and explicit asynchronous progress.
- **NativeIO / Platform / Core** for lower-level runtime and operating-system primitives.
- **CSTL / CSerde / binding layers** where typed storage or serialization contracts require them.

Not every TurboDB executable links every Salts subsystem. Standalone DDL tools intentionally keep a much narrower dependency closure.

## Ecosystem role

Salts provides the systems foundation. [SaltsUtils](https://github.com/qigao/salts-utils) contains DataBind as one of its components; [salts-net](https://github.com/qigao/salts-net) provides networking components.

TurboDB supplies durable storage providers and application data infrastructure for [TurboFlow](https://github.com/qigao/turbo-flow) and other higher-level systems.

TurboDB is the **storage/data infrastructure layer**. It owns database-specific behavior. Salts owns the shared systems semantics underneath it.

## Main areas

| Area | Responsibility |
| --- | --- |
| ORM | Optional database-facing object/record mapping |
| Redis | Redis-native primitives and durable/ordered adapter support |
| TidesDB | TidesDB integration |
| sqlparser | MySQL/SQLite syntax parsing and AST |
| TidesSQL | C SDK v1 for SQL execution over local TidesDB plus the optional `tidessqld` MySQL/TLS service; [SDK, daemon and module boundary](tidessql/readme.md) |
| SQLite | Native driver support and standalone schema application |
| PostgreSQL | Native driver support and standalone schema application |
| dbtools | Explicit standalone database tools |
| C++ wrappers | Header-only convenience APIs over C components where applicable |

Build options keep these areas independently selectable.

## Redis ordered apply and Stream outbox

The `redis_lua_apply.h` API exposes a Redis-native, bounded primitive for replicated-state adapters.

A single Lua `EVAL` atomically:

1. performs one hash-field write;
2. advances `applied_index` / term / command-id metadata;
3. appends the same command to a Redis Stream outbox.

This is additive to the `tedis` API and is independent of the SQL-oriented ORM.

### Key ownership and cluster requirements

Metadata, state, outbox keys, command-id, field, and value are borrowed until `redis_lua_apply_open` returns.

The three Redis keys must use the same non-empty Cluster hash tag, for example:

```text
raft:{orders}:meta
raft:{orders}:state
raft:{orders}:outbox
```

Missing or mismatched tags fail before dispatch.

### Progress model

Drive the operation with `redis_lua_apply_next` until it stops returning `REDIS_LUA_APPLY_WAIT`. Wait on the supplied CFlow waitable between calls, then destroy the operation with `redis_lua_apply_destroy`.

Terminal receipts:

- `APPLIED` — first durable application;
- `REPLAYED` — same current index, term, and command-id;
- `GAP` — index is not the next valid transition;
- `CONFLICT` — the current index is occupied by another term or command-id;
- `COMMIT_UNKNOWN` — the command may have been sent, but no response can be trusted.

For `COMMIT_UNKNOWN`, read the authoritative applied index before choosing a retry.

Indexes are compared and incremented as canonical decimal strings rather than Lua floating-point values, preserving the complete unsigned 64-bit Raft-index range.

Stream delivery remains at-least-once. Consumers must project idempotently and acknowledge only after projection succeeds.

## Standalone DDL SQL tools

`turbodb-sqlite` and `turbodb-postgresql` execute database bootstrap/schema SQL directly through native drivers.

They are intentionally narrow:

- command: `schema apply`;
- no migration diff/history engine;
- no runtime driver-plugin loading;
- no implicit outer transaction;
- no stdin execution;
- no automatic retry or backend fallback.

### Build

Windows development and Release use `win-dev-user` and `win-release-user`.
Both build ORM, database drivers, `tidessqld`, and SQLite/PostgreSQL/MySQL tools:

```powershell
cmake --fresh --preset win-release-user
cmake --build --preset win-release-user
ctest --preset win-release-user --output-on-failure
cmake --build --preset install-win-release-user
```

Use targets and CTest filters to select individual tools and tests:

```powershell
cmake --build --preset win-release-user --target turbodb-postgresql
ctest --preset win-release-user -R "^dbtool_" --output-on-failure
```

Consumers must select the intended package explicitly. Profiles do not fall back to each other. Runtime ORM Drivers are deployment artifacts under `turbodb/drivers`; changing Driver deployment does not require relinking `Orm::C`.

### SQLite

```powershell
turbodb-sqlite schema apply `
  --database .\app.db `
  --file .\bootstrap.sqlite.sql
```

### PostgreSQL

Connection information can be provided through a caller-selected environment variable so credentials do not need to appear in ordinary command-line arguments:

```powershell
$env:APP_PG_CONNINFO = 'host=127.0.0.1 dbname=app user=app'

turbodb-postgresql schema apply `
  --file .\bootstrap.postgresql.sql `
  --conninfo-env APP_PG_CONNINFO
```

Without `--conninfo-env`, libpq's standard environment, service, and `.pgpass` behavior applies.

The default SQL script limit is 16 MiB and can be changed explicitly with `--max-script-bytes`. SQLite also supports `--busy-timeout-ms`, defaulting to 5000 ms.

Input files must be non-empty, fully readable, and contain no embedded NUL.

Transaction boundaries belong to the SQL file itself:

```sql
BEGIN;
-- schema statements
COMMIT;
```

SQLite executes the file through one `sqlite3_exec()`. PostgreSQL uses one libpq simple-query sequence and releases all `PGresult` values.

### Exit codes

| Code | Meaning |
| ---: | --- |
| 0 | success / help |
| 2 | argument error |
| 3 | file error |
| 4 | connection error |
| 5 | SQL error |
| 6 | configured limit exceeded |
| 7 | unsupported operation |
| 8 | out of memory |
| 70 | internal error |

The standalone DDL tools intentionally do **not** link the broader ORM/CFlow/binding stack when it is not required. This preserves a small and auditable runtime closure.

See [driver-data-tools.md](docs/architecture/driver-data-tools.md) for the detailed design boundary.

## Build and package model

TurboDB consumes the latest published stable **Salts.Native** and **SaltsUtils.Native** producer SDKs. Callers provide the resolved install roots through `SALTS_ROOT` and `SALTS_UTILS_ROOT`.

The top-level CMake configuration resolves both packages with `NO_DEFAULT_PATH` semantics and fails if either configured root is absent or invalid. Host presets do not replace those roots with an ambient SDK; package selection happens before CMake and compatibility is enforced by exported targets and ABI/capability checks.

Salts owns canonical CMeta reflection plus Plugin ABI 4 publication, loading, lifecycle, and leases. SaltsUtils owns IDL/Schema/DataBind. TurboDB builds database-domain capabilities on those public contracts rather than copying reflection metadata, binding engines, or maintaining a second generic plugin runtime.

The `TURBODB_BUILD_ORM`, `TURBODB_BUILD_REDIS` and `TURBODB_BUILD_DBTOOLS` switches select modules. ORM builds SQLite, PostgreSQL, MySQL and TidesDB drivers together; consumers choose which plugins to load at runtime. Redis remains an independently selectable native client because it does not provide the SQL contract required by the ORM. MongoDB is not included. The generic `Orm::C` target does not link database clients.

## Design principles

- **Database semantics stay in TurboDB.** Salts provides systems primitives, not database policy.
- **No hidden fallback.** A failed database/provider path does not silently change storage engines.
- **Bounded async state.** Runtime-facing operations make waiting, cancellation, ownership, and terminal state explicit.
- **Provider-neutral upper layers.** Higher-level systems should depend on stable TurboDB contracts rather than raw driver/runtime state.
- **Narrow dependency closure.** Tools that do not need CFlow, ORM, binding, or parser layers should not link them.
- **Exact data representation matters.** Integer ranges, durable indexes, database value domains, and replay identity are preserved intentionally.

## Relationship to higher layers

TurboDB can provide durable storage to systems such as [TurboFlow](https://github.com/qigao/turbo-flow), but those systems own workflow/inbox/execution policy. TurboDB owns storage behavior and durable database-facing contracts.

This boundary is intentional:

```text
TurboFlow / applications
        ↓
stable TurboDB provider/API boundary
        ↓
TurboDB storage semantics
        ↓
native database driver
```

Raw driver state should not leak upward as application runtime identity.

---

**Salts provides the typed systems foundation. TurboDB provides the storage semantics.**
