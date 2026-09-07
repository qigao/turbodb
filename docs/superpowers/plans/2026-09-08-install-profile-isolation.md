# TurboDB install profile isolation plan

## Goal

Prevent PostgreSQL-only builds from overwriting the full SQLite/PostgreSQL
package, and make reused CMake build trees restore the capabilities declared by
their named preset.

## Invariants

- Full development and release presets explicitly enable both ORM backends.
- PostgreSQL-only presets explicitly disable SQLite for ORM and dbtools.
- Full and PostgreSQL-only presets never share an install prefix.
- The staged `OrmConfig.cmake` capability markers match the build that produced
  it, and a consumer requiring an absent backend fails during configure.
- There is no fallback to another build tree, package prefix, backend, or DLL.

## Steps

1. Add a source-level preset contract and observe it fail against the current
   inherited/default configuration.
2. Make full and PostgreSQL-only preset capabilities and install prefixes
   explicit on Windows and Linux.
3. Extend the existing staged package contract to validate exported capability
   markers, runtime SQLite behavior, and the missing-SQLite configure failure.
4. Verify reused and clean Windows release trees, install both profiles, and
   prove that installing PostgreSQL-only leaves the full package intact.
5. Re-run TurboFlow's complete Release suite against the restored full package.
