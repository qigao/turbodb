# TurboDB Salts Dependency Migration Plan

**Goal:** Finish the interrupted dependency rename so TurboDB consumes the installed Salts packages consistently in Debug and Release.

**Constraints:** Preserve TurboDB's own names and public APIs, keep first-party package discovery rooted at `SALTS_ROOT`, maintain configuration isolation, and retain the existing database-tool packaging behavior.

## Tasks

1. Audit the existing working-tree changes and separate the dependency migration from unrelated edits.
2. Replace the intermediate `Salts::*` targets with their installed `Salts::*` equivalents, including `Salts::CSTL`.
3. Change presets and exported package configs to require configuration-specific `SALTS_ROOT` paths with `NO_DEFAULT_PATH`.
4. Migrate removed Salts headers and C API identifiers to their Salts counterparts without renaming TurboDB-owned targets such as `turbo_orm`.
5. Update active documentation and repository guidance to describe Salts, CBind, CSTL, CFlow, and Executor boundaries.
6. Configure, build, test, package-contract test, and install both Windows Debug and Release presets.
7. Review the final diff, run whitespace/stale-name checks, and commit the verified migration.
