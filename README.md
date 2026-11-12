# TurboDB

**Typed and bounded database/storage infrastructure for the Salts ecosystem.**

TurboDB provides C database components, C++ header-only wrappers, native-driver database tooling, Redis integrations, and optional ORM capabilities. It uses the released SaltsBase, CMeta, SaltsUtils, and CNet SDKs as its systems foundation while keeping storage semantics, drivers, durability behavior, and database-specific contracts owned by TurboDB.

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
| sqlparser | MySQL/SQLite syntax parsing and AST |
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

`dbtools --driver sqlite|pg|mysql` executes database bootstrap/schema SQL through
dynamically loaded database drivers. Salts Plugin validates the module ABI and
the CMeta-reflected `TurboDb.SchemaApply` contract before opening a connection.

The tool supports:

- command: `schema apply`;
- no migration diff/history engine;
- explicit driver-plugin loading via `--plugin` or the selected driver's environment variable;
- no implicit outer transaction;
- no stdin execution;
- no automatic retry or backend fallback.

### Build

Modules are assembled unconditionally; Windows also builds TurboDB Studio.
TidesDB has been removed. The remaining `tidessql/` sources are archived and
are outside the build, installation, and exported package. Module switches
(`TURBODB_BUILD_*`) have been removed. Tests use `BUILD_TESTS`, benchmarks use
`BUILD_BENCHMARK`, and examples use `BUILD_EXAMPLE`.

`CMakeUserPresets.json` owns configure/build/test/install entry points and
parent-environment SDK roots. Hidden definitions in `CMakePresets.json` and
`presets/` supply compiler and platform profiles. Use CMake 3.27+, Ninja,
Python 3.12+, PowerShell 7, Git, and an authenticated GitHub CLI.
Windows MSVC also needs Visual Studio C++ tools. Studio uses the pinned
OpenWTL submodule for ATL and WTL. Initialize it with
`git submodule update --init external/OpenWTL` before configuring.

#### Protocol benchmarks

Redis reserves its command's `tstr` capacity once and formats framing directly
into it, publishing the length only after the complete frame is written. RESP
scanning borrows counted `vstr` ranges; reply strings still own their copies.
MySQL parameter lowering uses `simd.h` for complete 16-byte code blocks and
`vstr` scans for quoted/comment spans. Short spans finish before vector setup;
all loads are bounded, and overlapping in-place output remains supported.

After restoring the SDK environment and entering the MSVC developer environment:

```powershell
cmake --preset msvc-benchmark-user
cmake --build --preset msvc-benchmark-user
ctest --preset msvc-benchmark-user -L benchmark -V
```

`BUILD_BENCHMARK` owns these targets independently of `BUILD_TESTS`. Their sources
are [Redis protocol benchmarks](redis/tests/protocol_benchmark.c) and
[MySQL parameter benchmarks](mysql/tests/parameters/parameter_benchmark.c).

Local MSVC Release measurements on AMD Ryzen 9 7940HX, using default CPU
scheduling: median of three alternating before/after runs, each with 200 samples
of 1,024 operations. These measure protocol CPU work, including reply ownership
and command allocation where applicable; they do not measure database/network
throughput.

| Workload | Before (µs/op) | After (µs/op) | Speedup |
| --- | ---: | ---: | ---: |
| Redis 4 KiB simple reply, parse/own/free | 4.489 | 2.524 | 1.78× |
| Redis command, 16 × 8-byte arguments, build/free | 1.797 | 1.347 | 1.33× |
| Redis command, 16 × 4 KiB arguments, build/free | 2.574 | 2.198 | 1.17× |
| MySQL typical SELECT parameter lowering | 0.262 | 0.146 | 1.80× |
| MySQL dense parameter INSERT lowering | 0.205 | 0.175 | 1.17× |
| MySQL 4 KiB ordinary code span lowering | 14.149 | 7.054 | 2.01× |
| MySQL 4 KiB quoted span lowering | 7.102 | 0.276 | 25.69× |

The 8/64-byte RESP reply workloads were within 4% of baseline; the short mixed
quote/comment SQL workload was within 2%. SIMD results have not been measured
on ARM64. Correctness tests cover delimiter positions, binary bytes, split CRLF,
guard-page input bounds, output capacity and in-place lowering.

#### Upstream dependency versions

Select exact published versions through the parent environment or `.env`.
The current set is [vcpkg-cache 0.1.3](https://github.com/qigao/vcpkg-cache/releases/tag/v0.1.3),
[SaltsBase 0.1.5](https://github.com/qigao/salts-base/releases/tag/v0.1.5),
[CMeta 0.1.5](https://github.com/qigao/cmeta/releases/tag/v0.1.5),
[SaltsUtils 0.1.2](https://github.com/qigao/salts-utils/releases/tag/v0.1.2), and
[CNet 0.1.11](https://github.com/qigao/salts-cnet/releases/tag/v0.1.11).
`.env.example` lists the version variables and infrastructure paths; CI uses
repository variables with the same names. The infrastructure checkout must
match the selected release. Upgrades use separate versioned SDK directories.

The restore entry point checks release asset SHA-256 digests and exports
`SALTS_BASE_ROOT`, `CMETA_ROOT`, `SALTS_UTILS_ROOT`, `SALTSCORE_ROOT`,
`VCPKG_INSTALLED_DIR`, and `RE2C_ROOT` to the calling PowerShell process.
The shared vcpkg-cache `resolve-release` and `locked` operations restore the
selected preinstalled archive. The canonical manifest toolchain then resolves
TurboDB's `vcpkg.json`, including Studio dependencies on Windows.

Copy `.env.example` to `.env`, set the paths for your machine, and run these
Windows MSVC commands in one PowerShell session:

```powershell
./cmake/ci/restore-native-sdks.ps1 -Rid windows-x64 -Local
./cmake/WithVs.cmd cmake --fresh --preset msvc-release-user
./cmake/WithVs.cmd cmake --build --preset msvc-release-user
./cmake/WithVs.cmd ctest --preset msvc-release-user -LE e2e
./cmake/WithVs.cmd cmake --build --preset install-msvc-release-user
```

`-Local` explicitly loads build settings from `.env`; CMake never loads it implicitly.
GitHub authentication comes from the active `gh` login or parent token environment. Use `--fresh`
when changing SDKs or toolchains. Runtime startup uses the preset `PATH`/`LD_LIBRARY_PATH`. Windows retains the
existing `TurboDBRuntime` task to stage target runtime dependencies into the
build executable directory, which is a trusted search location for Plugin.

Linux Clang uses `-Rid linux-x64` and `llvm-native-release-user`. Linux GCC uses
`-Profile linux-gcc-x64` and `gcc-release-user`. Windows llvm-mingw uses
`-Profile windows-llvm-mingw-x64` and `llvm-mingw-x64-release-user` with its
matching compiler environment. Android uses `-Rid android-arm64-v8a -HostRid
linux-x64` and `android-arm64-release-user`; its tests need a device runner.
CNet 0.1.11 includes the native Linux ARM64 SDK. Its producer CI runs the
complete native CTest suite on `ubuntu-24.04-arm` before publication.

CI builds the complete configured graph. External database tests remain
registered with the `e2e` label; run them with `ctest --preset <profile> -L e2e`
after providing their database environment. `-LE e2e` selects local tests.

Rebuild TurboDB, consumers, and driver plugins together for the split SDK ABI.
`Salts::Core` comes from SaltsBase; `CMeta::CMeta`, `CMeta::CFlow`, `CMeta::CSTL`,
and `CMeta::Plugin` come from CMeta; DataBind comes from SaltsUtils; CNet and
Crypto come from the CNet SDK. Native headers use `core/`, `memory/`, `file/`,
and `threads/` paths. The migration adds no compatibility aliases and does not
change database file formats.

### SQLite

```powershell
dbtools --driver sqlite schema apply `
  --plugin C:/sdk/lib/turbodb/drivers/turbodb_driver_sqlite.dll `
  --database .\app.db `
  --file .\bootstrap.sqlite.sql
```

### PostgreSQL

Connection information can be provided through a caller-selected environment variable so credentials do not need to appear in ordinary command-line arguments:

```powershell
$env:APP_PG_CONNINFO = 'host=127.0.0.1 dbname=app user=app'

dbtools --driver pg schema apply `
  --plugin C:/sdk/lib/turbodb/drivers/turbodb_driver_postgresql.dll `
  --file .\bootstrap.postgresql.sql `
  --conninfo-env APP_PG_CONNINFO
```

Without `--conninfo-env`, libpq's standard environment, service, and `.pgpass` behavior applies.

Use `--driver mysql` for MySQL. Plugin paths must be absolute; omit `--plugin`
only when `TURBODB_SQLITE_PLUGIN`, `TURBODB_POSTGRESQL_PLUGIN`, or
`TURBODB_MYSQL_PLUGIN` supplies the selected module path. See
[dbtools usage and deployment](dbtools/readme.md) for MySQL connection settings.

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

TurboDB consumes exact released SDK versions selected through the environment.
Its exported package discovers `SaltsBase`, `CMeta`, `SaltsUtils`, and `SaltsCore`
from explicit roots while retaining the public `TurboDB::*` and `Orm::*` targets.
The packaged manifest records producer versions and the Base runtime SONAME.

CMeta owns reflection, Plugin, CFlow, and CSTL. SaltsBase owns native systems
primitives; SaltsUtils owns Schema, IDL, and DataBind; CNet owns networking and
crypto. TurboDB owns database-domain semantics on those contracts.

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
