# JPA-style C++ ORM architecture

## Background

The ORM already has a stable C ABI, typed C++ queries, repositories, a managed
entity session, identity mapping, dirty checking, optimistic locking, generated
relations, entity graphs, lazy relation handles, cascades, and orphan removal.
The remaining JPA-style areas span the TBE schema toolchain, generated metadata,
runtime state management, SQL planning, and tests. They therefore cannot be
added safely as unrelated runtime helpers.

The design goal is JPA-like usage and unit-of-work semantics without copying
Java's runtime reflection, bytecode enhancement, or unbounded proxy behavior.
The C facade remains repository-shaped; richer managed-entity behavior belongs
to the C++17 API.

## Candidate designs

### Runtime reflection and dynamic proxies

This most closely resembles Java ORM internals, but C++17 has no standard
runtime reflection or portable bytecode enhancement. It would add allocations,
indirect dispatch, difficult ownership rules, and platform-specific machinery.
It also conflicts with the current static model as the mapping source of truth.

### TBE schema plus generated static metadata

The existing parser produces one AST, the ORM normalizer owns validated values,
and generators bind those values to existing C/C++ types. C++ templates consume
the generated model with compile-time member and type checks. Runtime state is
limited to connection, session, identity map, snapshots, relation-load state,
and explicitly bounded caches. This is the selected design.

### Adopt an external full ORM

An external ORM could provide a broader feature list, but would introduce a
second mapping language and transaction model, change package dependencies, and
make the stable C ABI and existing database adapters secondary. Its migration
cost and behavioral risk exceed incremental extension of the current model.

## Layering and state ownership

```text
TBE parser AST
  -> owning ORM schema model
  -> ORM validation
  -> generated C++ entity_model<T> / generated C facade
  -> typed query planner and database adapters
  -> C++ entity_manager unit of work
```

The schema is the mapping fact source. Generated metadata is an immutable
compiled projection. The database is the persistent fact source. A C++
`entity_manager` owns first-level identity, snapshots, dirty state, loaded
relation masks, and transaction-local lifecycle. A future second-level cache is
only a bounded, invalidatable derivative of committed database state.

External database formats stay inside adapters. Query-language parsing will
produce the existing typed predicate/query representation rather than adding a
second execution engine.

## Delivery sequence

1. Lifecycle callbacks (delivered): generate static dispatch for persist,
   update, remove, and load events, with rollback on callback failure.
2. Embedded values and composite identifiers (delivered): generate structural
   key metadata, component-wise hashing, binding, equality, multi-column
   predicates, and flattened value mappings.
3. Transaction propagation (delivered): explicit REQUIRED, REQUIRES_NEW,
   MANDATORY, SUPPORTS, NOT_SUPPORTED, and NEVER scopes share the session RAII
   boundary. Joined failures mark the owner rollback-only. A single connection
   cannot suspend its physical transaction, so modes that require suspension
   fail explicitly instead of being approximated with savepoints.
4. Inheritance mapping (single-table delivered): the root owns the table,
   discriminator column, identifier, and version. Generated subtype models
   flatten inherited members and verify the C++ base relationship. Concrete
   repositories inject discriminator predicates; a generated `std::variant`
   hierarchy plus `polymorphic_repository` provides slicing-free CRUD and
   lookup. Polymorphic lookup uses discriminator discovery followed by concrete
   materialization, while `find_all` runs one query per concrete case. Joined
   and table-per-class strategies remain rejected because they require separate
   query and write planners.
5. Second-level cache: opt-in, size-bounded, immutable snapshots keyed by model
   type plus identifier, invalidated only after successful commit.
6. Schema migration: build a separate plan/diff/apply tool. Migration never runs
   implicitly when a connection is opened.
7. JPQL subset and Criteria facade: parse into one backend-neutral query AST and
   reject unsupported expressions before execution. Criteria is a typed facade
   over the same AST, not a parallel planner.

Portable runtime bytecode enhancement is intentionally excluded. Generated
entity graphs, explicit lazy handles, and static lifecycle dispatch are the C++
substitutes.

## Error and transaction semantics

Validation and generation fail before writing unusable metadata. Standalone
session flush prepares every dirty entry, executes all SQL and lifecycle
callbacks in one transaction, commits once, and advances snapshots only after
commit. Failure rolls back SQL and preserves dirty in-memory state for
inspection or retry; `discard()` restores committed snapshots. A transactional
session scope may flush more than once in the same physical transaction; its
transaction-entry graph checkpoint restores managed state if the scope rolls
back. Primary keys are immutable while an entity is managed.

Migration failures leave the schema version unchanged unless a backend can
atomically commit the complete plan. Cache publication and invalidation happen
only after database commit. A query parser failure never falls back to raw SQL.

## Compatibility, cost, and rollback

Existing `ORM_MODEL*` mappings remain valid and simply have no generated
callbacks or relations they do not declare. Schema attributes are opt-in. The C
ABI receives no C++ template, exception, or proxy types. Each feature is added
as a separate schema/model/runtime vertical slice and can be rolled back by
removing its optional metadata while keeping repository CRUD intact.

Static metadata increases generated-header size and compile time in proportion
to mapped fields and relations, but avoids per-field runtime reflection and
virtual dispatch. Runtime costs that can grow with data—identity maps, relation
graphs, prefetch batches, and future caches—must keep explicit configured
limits. Each slice is verified first with generator/validator tests, then real
SQLite unit-of-work tests, followed by the relevant preset test set.
