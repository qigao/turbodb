# Orm CMake Package Boundary

## Status

Accepted as an intentionally incompatible package migration on 2026-08-29.

## Context

Orm supports SQLite, PostgreSQL, Redis, MongoDB, and TidesDB behind one public
`orm.h` API. The former CMake contract exposed the same library through the
unrelated `TurboDB::ORM` target and `TurboDB` package component. It also made a
shared Orm consumer discover some enabled backend development packages even
though those targets were private to the shared library.

The public headers intentionally remain visible: applications may configure
and use Orm directly. `orm_postgresql.h` is an Orm-owned connector API when
that component is enabled; backend-native headers and types remain
implementation details under `orm/src/dbs`.

## Candidates

1. Keep the `TurboDB` component facade. This preserves old CMake callers but
   leaves two package owners and makes dependency recursion possible for the
   Redis backend.
2. Require every consumer to find its selected backend packages. This exposes
   implementation details and makes application CMake depend on how Orm was
   built rather than on the Orm API.
3. Make `Orm` the only package and publish only a shared target whose backend
   closure is complete before installation. This is selected because package
   ownership matches the public header and target without leaking backend
   development packages.

## Decision

Consumers use only:

```cmake
find_package(Orm CONFIG REQUIRED)
target_link_libraries(app PRIVATE Orm::C)
```

`Orm::Cpp` remains the optional C++ header wrapper target. There is no
`TurboDB::ORM` target or `TurboDB` ORM component.

An SDK built with PostgreSQL also exports `Orm::PostgreSQL` from the same
`Orm` package. Consumers still call only `find_package(Orm)`; there is no
driver-specific CMake package and no consumer-side PostgreSQL discovery.

Backend targets are private implementation dependencies and are never
discovered by `OrmConfig.cmake`. `TurboUtils::Core` remains public because
`orm.h` exposes TurboUtils CBind, CFlow, string-view, and metadata types.
Runtime libraries that are dynamically linked remain deployment artifacts,
not consumer CMake packages. A static clone exists only inside the test build;
there is no installable static Orm SDK contract.

## Consequences and compatibility

- External callers using `find_package(TurboDB COMPONENTS ORM)` or
  `TurboDB::ORM` must migrate. No compatibility alias or fallback is provided.
- Builds requesting an installable static Orm must migrate to the shared
  target. Backend closure is owned by the Orm artifact, not the final consumer.
- `orm.h`, its C ABI, backend selection, errors, persisted data, and runtime
  behavior are unchanged.
- Flowie finds `Orm` directly and propagates `Orm::C` because its public API
  contains `orm_config_t`. FlowMQ has no Orm API or implementation use, so it
  carries no Orm package dependency.
- The installed TurboDB package continues to own `TurboDB::Redis` for direct
  Redis users; an installed Orm package never finds it from consumer CMake.

## Migration and rollback

Publish and install TurboDB/Orm first, then migrate and rebuild FlowMQ and
Flowie from clean configure trees. Downstream external consumers migrate by
replacing the package and target names; no source or data migration is needed.

Rollback is source-only: revert the package-boundary commits in reverse
consumer order and reinstall the prior SDK. Database files and wire protocols
need no rollback action.

## Verification

- Stage the real shared Orm installation, then configure and link a C consumer
  while backend package discovery is explicitly disabled. The Windows staging
  install removes build-tree and vcpkg paths from `PATH` and verifies that the
  generated install rule names the backend runtime `bin` directory.
- Build and test TurboDB/Orm, FlowMQ, and Flowie in dependency order.
- Configure and link installed package consumers from clean build directories.
- Inspect shared runtime dependencies separately from CMake development
  dependencies on Windows and Linux.
