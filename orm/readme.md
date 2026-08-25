# Orm: a QueryDSL-inspired ORM for C and C++

Orm is a database-neutral query library for C11 and C++17. It provides a
stable C ABI, a fluent C DSL, and a typed C++ DSL over the same bounded query
model. SQLite is available in the core build. PostgreSQL is an explicitly
linked driver component; optional Redis Query Engine and embedded TidesDB
adapters cover indexed remote documents and durable local key/value storage.

The DSL is inspired by [QueryDSL](https://querydsl.com/): schemas, columns,
predicates, joins, and aggregates are represented explicitly instead of being
assembled from SQL fragments. Orm is an independent implementation; it does
not depend on QueryDSL. The manual query DSL does not require generated source;
the optional schema workflow generates typed C and C++ model facades.

## Highlights

- One backend-neutral connection, query, and result API
- C11 chaining with `ORM_EQ`, `ORM_AND`, `ORM_OR`, and bounded expression values
- C++17 typed tables, columns, predicates, joins, aggregates, and RAII ownership
- SELECT, DISTINCT, INSERT, UPDATE, DELETE, INNER/LEFT JOIN, GROUP BY, HAVING, and pagination
- Typed and C expression helpers for NULL checks, BETWEEN ranges, and IN membership
- Bound values for structured and raw queries; values are never interpolated into SQL
- Explicit limits for query size, parameters, predicates, nesting, and result retention
- Opaque C handles and no backend names in exported function symbols
- TurboUtils `vstr` views, `tstr` helpers, typed formatting, and optional `tlog` diagnostics
- SQLite plus explicitly built PostgreSQL, Redis Query Engine, and TidesDB drivers selected through runtime configuration

## Choose an interface

| Language | Header | CMake target | Query style |
| --- | --- | --- | --- |
| C11 | `orm.h` or generated `*.orm.h` | `TurboDB::ORM` | Fluent query DSL or generated typed CRUD |
| C++17 | `orm.hpp` or generated `*.orm.hpp` | `Orm::Cpp` | Typed query DSL and generated entity manager |

Both interfaces use the same C ABI implementation and backend adapters.
`orm.hpp` includes `orm.h`, so C++ applications only need the C++ header.

## Build and install

TurboUtils and SQLite development packages are required by the default build.
PostgreSQL/libpq is required only when `ORM_WITH_PGSQL=ON` builds the separate
`OrmPostgreSQL::Driver` component.

```sh
cmake -S . -B build -DORM_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/orm
```

Enable the Redis adapter with `-DORM_WITH_REDIS=ON`. It additionally requires
the installed `TurboDB::Redis` target and its TurboNet dependencies. The
generated `OrmConfig.cmake` asks for TurboDB only when this option was enabled.

Enable the embedded adapter with `-DORM_WITH_TIDESDB=ON`. It requires the
independently installed `TidesDB::tidesdb` package. The generated
`OrmConfig.cmake` asks for TidesDB only when this option was enabled, so ORM and
TidesDB retain separate install prefixes and package lifecycles.

Run these commands from `shared/orm`. Consume the installed package with:

```cmake
find_package(TurboDB CONFIG REQUIRED)

target_link_libraries(c_application PRIVATE TurboDB::ORM)
target_link_libraries(cpp_application PRIVATE Orm::Cpp)
```

The core `Orm` package never discovers or links PostgreSQL. A product that
needs PostgreSQL opts in explicitly:

```cmake
find_package(OrmPostgreSQL CONFIG REQUIRED)
target_link_libraries(control_service PRIVATE OrmPostgreSQL::Driver)
```

Before its first PostgreSQL connection, call
`orm_postgresql_register(&error)`. Registration is thread-safe and
idempotent. Without that component and registration,
`orm_connect(driver="postgresql")` returns `ORM_STATUS_UNSUPPORTED`; other ORM
consumers do not compile or link libpq.

`Orm::C` propagates `TurboUtils::Core`. C callers can pass `vstr` directly as
`orm_string_view_t`, or use `orm_view_tstr()` / `orm_text_tstr()` for owned
`tstr` values. Borrowed views remain valid only while their source storage is
unchanged and alive.

The SQL backend renderer uses TurboUtils Mustache4C for the structured
`SELECT`/`INSERT`/`UPDATE`/`DELETE` skeletons. The installed TurboUtils package
does not export a Mustache target, so the ORM build and `OrmConfig.cmake` both
re-discover `mustache.lib` / `mustache.dll` relative to `TurboUtils_DIR`. Keep
`mustache.dll` on the runtime `PATH` together with `turbo_utils.dll`.

### Build-time ORM schema tools

`ORM_BUILD_SCHEMA_TOOLS=ON` builds `orm_schema_validate` and
`orm_schema_generate`. Both tools reuse the public TurboParser TBE parser. A
single owning `orm::schema::schema_model` normalizes the TBE AST; the validator
checks that model and the generator consumes the same validated values before it renders
`tools/templates/orm_cpp_metadata.mustache` with `TurboParser::Mustache`.
The template is embedded in the build-time tool, so generated-header consumers
do not deploy a template or compiler. These tools are not linked into the ORM
runtime.

ORM validation is opt-in with `[orm(1)]` on the schema declaration:

```text
schema Store [orm(1), cpp_namespace("app::model")];

[table(users)] message User {
  [id(1)] uint64 id;
  [version(1), column(entity_version)] uint64 version;
  [relation(Order), mapped_by(user_id), cascade(all), orphan_removal(1), fetch(lazy)]
  Order order;
  [column(display_name)] string name;
}

[table(orders)] message Order {
  [id(1)] uint64 id;
  uint64 user_id;
}
```

Every message in an ORM profile is an entity and must declare one table and
exactly one `[id(1)]` or `[primary_key(1)]` field. Field and resolved column
names must be unique. Relation targets and optional `mapped_by`/`foreign_key`
fields must exist; mapping types must match the owner primary key. Supported
cascade values are `none`, `persist`, `remove`, `merge`, `refresh`, `detach`,
`all`, and `orphan_remove`.
Orphan removal and cascade metadata are accepted only on relation fields.
An entity may declare at most one `[version(1)]` scalar integer field. Its
resolved column participates in optimistic UPDATE and managed DELETE checks.
Primary-key and version fields cannot be optional. TBE's native field
qualifier is the ORM nullability source of truth:

```text
enum ProfileState <uint8> {
  inactive = 0;
  active = 1;
}

[table(profiles)] message Profile {
  [id(1)] uint64 id;
  ProfileState state;
  optional string nickname;
}
```

The matching C++ members are `ProfileState state` and
`std::optional<std::string> nickname`. Generated headers emit C++17
`static_assert` checks for optional member types, enum member types, and enum
underlying integer types. This makes schema/C++ drift a consumer compile error
before a query runs. Optional non-relation fields are limited to the scalar,
string, bytes, and enum types supported by ORM value binding; SQL `NULL`
round-trips as `std::nullopt`.

Embeddable value types and composite identifiers use explicit schema metadata:

```text
[embeddable(1)] message OrderKey {
  [column(tenant_id)] uint64 tenant;
  [column(order_number)] uint64 number;
}

[table(orders)] message Order {
  [embedded_id(1)] OrderKey key;
  [version(1)] uint64 version;
  string label;
}

[embeddable(1)] message Address {
  string city;
  string postal_code;
}

[table(customers)] message Customer {
  [id(1)] uint64 id;
  [embedded(1)] Address address;
  string name;
}
```

An embeddable has no table, primary key, version, relations, or lifecycle
callbacks. Its scalar columns are flattened into the owning entity; duplicate
flattened column names are rejected. An entity may use multiple direct
`[id(1)]` fields or exactly one `[embedded_id(1)]`, but cannot mix both forms.
Generated models expose ordered `primary_keys()` metadata and an `id(entity)`
tuple. Repository and entity-manager lookup accept either a same-order
`std::tuple` or the generated embeddable key type:

```cpp
auto by_value = repository.find_by_id(OrderKey{7, 42});
auto by_tuple = repository.find_by_id(std::make_tuple(7u, 42u));
auto managed = entity_manager.find<Order>(OrderKey{7, 42});
```

Composite identity components remain distinct in the identity map, participate
in every UPDATE/DELETE predicate, and cannot change while managed. Generated C
facades currently require one direct primary key and reject embedded/composite
identity schemas before emitting code. Generated relation descriptors currently
require a single-component owner key.

Single-table inheritance uses one root-owned table and discriminator contract:

```text
[table(payments), inheritance(single_table),
 discriminator_column(payment_kind), discriminator_value(base)] message Payment {
  [id(1)] uint64 id;
  string label;
}

[extends(Payment), discriminator_value(card)] message CardPayment {
  string card_last4;
}
```

The bound C++ subtype must use unambiguous public inheritance from the schema
base; the generated header verifies both the base relationship and pointer
convertibility. A subtype inherits the root table,
identifier, version, fields, relations, and lifecycle callbacks, so it must not
redeclare a table, id, version, inheritance strategy, or discriminator column.
Field/column hiding, cycles, missing bases, unsupported strategies, modeled
discriminator columns, and duplicate discriminator values fail validation.

`repository<CardPayment>` automatically adds `payment_kind = "card"` to reads,
updates, and deletes, and writes that value on insert. `repository<Payment>`
addresses only root rows; it never slices subtype rows. Query a whole hierarchy
through the generated variant contract:

```cpp
orm::polymorphic_repository<Payment> payments(connection);
std::optional<std::variant<Payment, CardPayment>> payment =
    payments.find_by_id(7);
std::vector<std::variant<Payment, CardPayment>> all = payments.find_all();
payments.update(*payment);
payments.remove(*payment);
```

`find_by_id` first reads the discriminator and then materializes the matching
concrete model; use its `transaction&` overload when both reads require one
consistent snapshot. `find_all` validates all discriminator values and then
issues one filtered query per concrete type, so it executes `h + 1` reads and
has O(h + n) local work, where `h` is hierarchy width and `n` is returned rows.
Results are grouped by generated hierarchy order rather than global table order.
Unknown discriminator values fail with `ORM_STATUS_DATASTORE_ERROR`; they are
never silently omitted. Joined and table-per-class inheritance are rejected
instead of approximated. The generated C facade also rejects inheritance
schemas because C structs do not provide the required base-subobject layout.

```sh
orm_schema_validate path/to/store.schema
orm_schema_generate path/to/store.schema generated/store.orm.hpp
```

The generated header binds schema metadata to existing C++ structs; it does not
generate a second business type. Declare the structs first, then include the
generated header:

```cpp
#include <cstdint>
#include <optional>
#include <string>

namespace app::model {

enum class ProfileState : std::uint8_t { inactive = 0, active = 1 };

struct Order {
  std::uint64_t id;
  std::uint64_t user_id;
};

struct User {
  std::uint64_t id;
  std::uint64_t version;
  Order order;
  std::string name;
};

struct Profile {
  std::uint64_t id;
  ProfileState state;
  std::optional<std::string> nickname;
};

}  // namespace app::model

#include "generated/store.orm.hpp"
```

For C11, `tbe_compiler` remains the single source of owning business models and
the ORM generator adds a header-only CRUD facade over those `Type_t` values:

```sh
tbe_compiler path/to/store.schema --lang c \
  --output generated/store.tbe.h \
  --source-output generated/store.tbe.c
orm_schema_generate --language c --model-header store.tbe.h \
  path/to/store.schema generated/store.orm.h
```

The generated C API uses schema-qualified symbols such as
`Store_User_orm_find`, `Store_User_orm_insert`, `Store_User_orm_update`, and
`Store_User_orm_remove`. It maps column aliases, enums, nullable presence bits,
owning `tstr` strings, bytes, primary keys, and optional optimistic version
columns. TBE's `[c(member_name)]` field attribute is honored by both the owning
model generator and the ORM facade, so the schema name and C member name may
differ without duplicating mapping metadata. A find destination must first be
initialized with `User_init()`; after a successful find it owns its strings and
bytes until `User_clear()`:

```c
#include "generated/store.orm.h"

User_t user;
uint8_t found = 0;
orm_error_t error;
orm_status_t status;

User_init(&user);
orm_error_init(&error);
status = Store_User_orm_find(connection, 7u, &user, &found, &error);
if (status != ORM_STATUS_OK) {
  /* report error.message at the application boundary */
} else if (found != 0) {
  /* use user */
}
User_clear(&user);
```

The C facade is deliberately repository-shaped: it provides typed CRUD and
optimistic locking without exposing C++ templates or exceptions. Identity-map
sessions, dirty checking, lazy relation proxies, entity graphs, and lifecycle
cascades remain C++17 entity-manager capabilities. Relation fields are excluded
from generated C column CRUD instead of being flattened into database values.

The schema message name selects the C++ type, schema field names select C++
members, `[column(...)]` supplies database column names, and the annotated
primary-key field supplies the repository/session key column. The generated
`orm::model::entity_model<T>` is the only mapping contract: it owns the column
members, table, key, version, relation factories, and fetch accessors. Relation
members are excluded from the column tuple. A direct entity field emits a
`relation::one()` factory; a `list<T>` field emits `relation::many()` and maps
to a `std::vector<T>` member. Generated models also expose a `relations()`
tuple, which the normal lifecycle API consumes automatically:

```cpp
orm::entity_manager unit_of_work(connection); // alias of orm::session
auto managed = unit_of_work.persist(user);
unit_of_work.remove(managed);
```

`persist`, `merge`, `remove`, `refresh`, and `detach` apply generated cascade
policies to the materialized in-memory graph. `find<T>(id)` is the JPA-style
load path and initializes generated `fetch(eager)` relations; `load<T>(id)` is
the lower-level identity-map lookup without relation initialization. Explicit
`*_graph()` overloads and factories such as `user_model::order()` remain
available for handwritten models and deliberate graph subsets.

Entity-level lifecycle callbacks are also generated as static model metadata:

```text
[table(users),
 pre_persist(before_create), post_persist(after_create),
 pre_update(before_update), post_update(after_update),
 pre_remove(before_remove), post_remove(after_remove),
 post_load(after_load)] message User {
  [id(1)] uint64 id;
  string name;
}
```

Each attribute names a zero-argument member function returning `void`. Callback
names are validated as C++ identifiers, while a missing member is reported by
the generated header at consumer compile time. `pre_*` runs before its SQL
operation; `post_*` runs after that operation but before transaction commit, so
an exception rolls back the whole flush. In-memory callback changes remain
visible and dirty after a failed flush; use `discard()` to restore the last
committed snapshot. A callback must not change the primary key. `post_load`
runs after new row materialization and `refresh()`, but not on an identity-map
hit. Retried flushes invoke the corresponding callbacks again, so callbacks
that perform external side effects must be idempotent or manage compensation.

`mapped_by(...)` and `foreign_key(...)` resolve to target C++ member pointers;
the owner primary-key member is bound before each child is staged. `cascade`
and `orphan_removal` become the descriptor's `cascade_policy` bits. The
optional `cpp_namespace("a::b")` schema attribute wraps all generated metadata
in that C++17 namespace. Namespace segments, entity names, and field names are
validated as C++ identifiers before a header is written. Relation collection
forms other than `list<T>` are rejected because the runtime descriptor API
uses `std::vector` as its collection representation.

An explicitly mapped relation may declare `fetch(lazy)` or `fetch(eager)`.
The generator emits `entity_model<T>::fetch_<field>(session, owner)`: lazy accessors
return a session-bound `lazy_one`/`lazy_many` proxy, while eager accessors
return the managed to-one pointer or to-many vector immediately. Generated
`load_eager()` and `eager_relations()` metadata let `find<T>()` populate eager
fields recursively while leaving lazy fields untouched. The session records
the exact generated relation mask materialized for each managed identity.
Lifecycle cascades consume only that descriptor set, so an uninitialized lazy
to-one field is never mistaken for a real child. Generated entity graphs are
typed by their owner and compose at compile time:

```cpp
using user_model = orm::model::entity_model<User>;
auto user = unit_of_work.find<User>(id, user_model::order_graph());
user_model::initialize_order(unit_of_work, user); // idempotent
auto full = user_model::order_graph() | user_model::orders_graph();
```

Graphs from different entity types cannot be combined or passed to the wrong
`find<T>()`. A graph relation without `fetch(...)` metadata is rejected before
the session marks it loaded. Fetch requires `mapped_by(...)` or
`foreign_key(...)`; the generated query uses the target field's resolved
`[column(...)]` name rather than assuming its C++ member name. Unannotated
relations keep their existing behavior and emit no fetch accessor.

Generated `one` and `list` relations use the existing model's value members,
while the session identity map remains the authoritative managed state. Before
`flush()`, a changed relation value is synchronized into the canonical child
with the same identity and therefore participates in normal dirty checking.
An unchanged relation value never overwrites a child edited through the
session. If both copies were changed to different values, `flush()` fails with
`ORM_STATUS_INVALID_STATE` instead of choosing an order-dependent winner.
`discard()` restores both canonical entities and their relation values to the
last successfully accepted snapshots. Collection additions still require
`cascade(persist)`, and removals delete children only with `orphan_removal`.

Handwritten `shared_ptr<T>` and `vector<shared_ptr<T>>` descriptors use the
same identity rule. Once a related child is managed, the relation pointer is
rebound to an alias of the identity-map entry rather than retaining a second
mutable object. Replacing it with a different pointer carrying the same key is
supported: changed modeled fields are synchronized and the pointer is rebound
again at `flush()`. Trackers keep owned value snapshots, so conflict detection
and `discard()` do not depend on the external pointer's control block. Editing
an uncascaded shared child that is not managed fails with
`ORM_STATUS_INVALID_STATE`; unchanged references remain valid and are not
implicitly persisted. Replacing any value or shared relation with a different
identity follows the same rule: the new child must already be managed by the
session or the descriptor must include `cascade(persist)`. Validation happens
before orphan removal, so a rejected replacement cannot delete the previously
accepted child.

To-many relations require each child identity to appear exactly once. `flush()`
rejects duplicate primary keys before synchronizing child state, cascading
inserts, or applying orphan removal; `discard()` then restores the last accepted
collection snapshot.

For an in-tree CMake build, register validation as a dependency of the target
that consumes generated ORM metadata:

```cmake
orm_add_schema_validation(validate_store_schema
                          ${CMAKE_CURRENT_SOURCE_DIR}/store.schema)
add_dependencies(store_model validate_store_schema)

orm_add_schema_generation(
    ${CMAKE_CURRENT_BINARY_DIR}/generated/store.orm.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/store.schema)
target_sources(store_model PRIVATE
               ${CMAKE_CURRENT_BINARY_DIR}/generated/store.orm.hpp)

orm_add_c_schema_generation(
    ${CMAKE_CURRENT_BINARY_DIR}/generated/store.orm.h
    ${CMAKE_CURRENT_BINARY_DIR}/generated/store.tbe.h
    ${CMAKE_CURRENT_BINARY_DIR}/generated/store.tbe.c
    ${CMAKE_CURRENT_SOURCE_DIR}/store.schema)
target_sources(store_c_model PRIVATE
               ${CMAKE_CURRENT_BINARY_DIR}/generated/store.tbe.c
               ${CMAKE_CURRENT_BINARY_DIR}/generated/store.orm.h)
target_include_directories(store_c_model PRIVATE
                           ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(store_c_model PRIVATE Orm::C TurboParser::DataBind)
```

The custom target writes a stamp only after validation succeeds. Editing the
schema or rebuilding `orm_schema_validate` invalidates the stamp; a validation
error stops the dependent build target. `orm_add_c_schema_generation` locates
the installed `tbe_compiler`, runs both generators in dependency order, and
fails configuration when the compiler or required imported targets are absent.

The tools do not extend TBE syntax. Validation leaves schemas without `[orm(1)]`
unchanged, while metadata generation rejects them. Relation collection forms
must first be representable by the installed TBE grammar. Generated member
pointers make missing or incompatible C++ members a compile-time error.

## C++17 quick start

A schema descriptor plays the same role as a QueryDSL Q-type while remaining
ordinary C++17. Column members own their table and column names, so expressions
do not borrow the descriptor's storage.

```cpp
#include "orm.hpp"

#include <cstdint>
#include <string>

struct person_row {};

struct person_table final : orm::table<person_row> {
  person_table()
      : orm::table<person_row>("person"), id(*this, "id"),
        name(*this, "name"), active(*this, "active") {}

  orm::column<std::int64_t> id;
  orm::column<std::string> name;
  orm::column<bool> active;
};

int main() {
  const person_table person;

  orm::config configuration("sqlite");
  configuration.option("filename", ":memory:")
      .option("open_mode", "read_write_create");
  orm::connection connection(configuration);

  (void)connection
      .raw("create table person(id integer primary key, "
           "name text not null, active integer not null)")
      .execute();

  (void)connection.insert(person)
      .set(person.id, 1)
      .set(person.name, "Alice")
      .set(person.active, true)
      .execute();

  const auto rows = connection.select(person.id, person.name)
      .from(person)
      .where(person.id >= 1 &&
             (person.active == true || person.name.like("A%")))
      .order_by(person.id.asc())
      .limit(20)
      .fetch();

  return rows.rows() == 1 && rows.text(0, 1) == "Alice" ? 0 : 1;
}
```

### C++ query grammar

The typed API supports:

- Comparisons: `==`, `!=`, `<`, `<=`, `>`, `>=`, `like`, and `not_like`
- Boolean composition: `&&` and `||`, including nested groups
- Ordering: `column.asc()` and `column.desc()`
- Aggregates: `count_all`, `count`, `sum`, `avg`, `min`, and `max`
- Joins: `inner_join(...).on(...)` and `left_join(...).on(...)`
- DML: typed `insert`, `update`, and `delete_from`

The same chain can project `orm::count_all()` or `orm::avg(column)`, join a
second descriptor with `left_join(table).on(left == right)`, group by typed
columns, and filter aggregates with `having`. The complete regression example
is in `tests/abi/cpp_api_test.cpp`.

`&&` and `||` build an immutable predicate tree. The typed API translates that
tree to the C query model, so typed chains do not require manual begin/end group
calls. An aggregate predicate is accepted only by `having`; using one in
`where` fails before the query is modified.

### C++ ownership and errors

- `connection`, `query`, and `result` are move-only RAII values.
- A query retains its underlying connection and may outlive the originating
  `connection` object.
- Text parameters are copied before crossing the C ABI.
- Result text is returned as an owning `std::string`.
- C status failures become `orm::status_error` with the original status code.
- One connection and its queries form a single-thread domain; serialize shared
  access or use independent connections.

Chain execution throws `orm::status_error` on failure. Production code should
catch it to report the status and diagnostic (the quick start above omits the
`try`/`catch` for brevity):

```cpp
try {
  const auto rows = connection.select(person.id, person.name)
      .from(person)
      .where(person.id >= 1)
      .fetch();
  // ...
} catch (const orm::status_error& error) {
  std::cerr << "ORM error: " << error.what()
            << " (status: " << error.status() << ")\n";
}
```

The lower-level string builder remains available for runtime schemas through
`connection.select("table")`, `query.column(...)`, `query.where(...)`, and
`query.execute()`.

### Model-based typed fetch

A typed projection infers its tuple result directly from the selected columns
and aggregate expressions:

```cpp
auto people = connection.select(person.id, person.name, person.score)
    .from(person)
    .order_by(person.id.asc())
    .fetch_typed();

// decltype(people) is
// std::vector<std::tuple<std::int64_t, std::string, double>>
```

Existing `fetch()` continues to return the generic `orm::result` object.
Explicit `fetch<T>()` remains available for entity and DTO mapping.
`fetch_one_typed()` returns `std::optional<row_type>` for zero or one row and
reports `ORM_STATUS_INVALID_STATE` when the query returns multiple rows.
Declare nullable projections as `column<std::optional<T>>`; the inferred tuple
then preserves that nullability. Nullable columns accept either `T`,
`std::optional<T>`, `std::nullopt`, or `nullptr` in typed assignments and
predicates. Equality and inequality with an empty optional lower to SQL
`IS NULL` and `IS NOT NULL` through the existing C ABI.

Queries with exactly one projection also expose `fetch_scalars()` and
`fetch_one_scalar()`. They return `std::vector<T>` and `std::optional<T>`
respectively; template substitution removes these overloads for multi-column
projections:

```cpp
auto names = connection.select(person.name)
    .from(person)
    .order_by(person.id.asc())
    .fetch_scalars();
```

Numeric columns can form typed scalar projections with `+`, `-`, `*`, and
`/`. Operations are stored as a validated postfix token stream in the shared C
query model; literals remain bound parameters rather than SQL text:

```cpp
auto adjusted = connection.select((person.score * 2.0 + 5.0).as("adjusted"))
    .from(person)
    .fetch_scalars(); // std::vector<double>
```

SQL backends render these expressions with explicit parentheses. Backends that
execute a native query plan currently report `ORM_STATUS_UNSUPPORTED` instead
of silently changing the expression semantics. The C ABI exposes the same
model through `orm_query_add_expression` and `orm_scalar_expression_t`.

Constructor projections use `fetch_mapped(factory)` or
`fetch_one_mapped(factory)`. The factory parameters must match the inferred
projection fields, and its return type does not need `ORM_MODEL` or a default
constructor:

```cpp
auto summaries = connection.select(person.id, person.name)
    .from(person)
    .fetch_mapped([](std::int64_t id, std::string name) {
      return person_summary{id, std::move(name)};
    });
```

Typed projections retain their table provenance until execution. Every
selected column or column aggregate must belong to the `FROM` table or to a
table whose join has completed through `.on(...)`; otherwise fetch fails with
`std::invalid_argument` before calling the backend. `count_all()` remains
table-neutral.

Call `.distinct()` on either a typed or untyped SELECT query to remove duplicate
projection rows. Pass `false` to disable it again while composing a query. SQL
backends render `SELECT DISTINCT`; Redis, MongoDB, and TidesDB currently reject
DISTINCT with `ORM_STATUS_UNSUPPORTED` instead of silently returning duplicates.

SELECT subqueries can be attached through `.exists()` and
`.not_exists()`:

```cpp
auto ids = connection.select(person.id)
    .from(person)
    .where(connection.select(department.id)
        .from(department)
        .where(department.id.eq_column(person.department_id) &&
               department.name == "Sales")
        .exists())
    .fetch_scalars();
```

Quantified comparisons use `where_any` or `where_all`:

```cpp
auto peer_scores = connection.select(person.score).from(person);
auto ids = connection.select(person.id)
    .from(person)
    .where_all(person.score, orm::comparison::greater_equal, peer_scores)
    .fetch_scalars();
```

`ANY` and `ALL` require backend SQL support; SQLite does not implement these
quantifiers, while PostgreSQL does.

Dynamic C++ queries expose the same operation without typed projection checks:

```cpp
auto scores = connection.select("person");
scores.column("score");
auto people = connection.select("person");
people.column("id").where_quantified_subquery(
    "score", orm::comparison::greater, orm::subquery_quantifier::any, scores);
```

The outer query stores an owning snapshot when the predicate is attached, so
destroying or modifying the original subquery cannot change it. Both queries
must originate from the same connection. Parameter counts, payload limits, and
subquery depth are charged to the outer query. SQL backends are supported;
native-plan backends currently return `ORM_STATUS_UNSUPPORTED`. Column-to-column
predicates use `eq_column`, `ne_column`, `lt_column`, `le_column`, `gt_column`,
or `ge_column`; unlike the overloaded comparison operators used by JOIN, these
methods produce WHERE predicates and can reference an outer query column.

Single-column SELECT queries can also be used with `where_in` and
`where_not_in`:

```cpp
auto sales_department_ids = connection.select(department.id)
    .from(department)
    .where(department.name == "Sales");

auto people = connection.select(person.id, person.name)
    .from(person)
    .where_in(person.department_id, sales_department_ids)
    .fetch_typed();
```

For typed queries, the subquery must have exactly one projection whose type is
compatible with the tested column; incompatible or multi-column queries are
removed by C++17 substitution. The C ABI performs the single-projection check
at attachment time. `NOT IN` retains normal SQL NULL semantics.

Scalar comparisons use the same owning snapshot and require exactly one
compatible projection:

```cpp
auto average_score = connection.select(orm::avg(person.score)).from(person);
auto ids = connection.select(person.id)
    .from(person)
    .where(person.score, orm::comparison::greater, average_score)
    .fetch_scalars();
```

Include `orm.hpp`, describe an owning result type with `ORM_MODEL`, and pass
that type explicitly to `fetch<T>()`:

```cpp
struct person_result {
  std::int64_t id;
  std::string name;
  std::optional<std::string> note;
};

ORM_MODEL(person_result, id, name, note)

auto people = connection.select(person.id, person.name, person.note)
    .from(person)
    .order_by(person.id.asc())
    .fetch<person_result>();
```

The same materialization is available as `query.fetch<T>()`,
`result.as<T>()`, and `result.row<T>(index)`. It is backend-neutral and works
over every backend that implements the generic result accessors.

For entity-oriented code, `orm::repository<T>` builds the same projection and
provides explicit CRUD commands:

```cpp
orm::repository<person_result> people(connection);
auto all = people.find_all();
auto one = people.find_by_id(7);
people.insert(person_result{7, "Alice", std::nullopt});
people.update(person_result{7, "Alice Cooper", std::nullopt});
people.delete_by_id(7);
```

Repository entities must expose exactly one modeled primary-key field.
`ORM_MODEL` defaults that field to `id`; generated metadata and
`ORM_MODEL_ENTITY` may select another column. The model table name is
used by every backend. Every operation also has a `transaction&` overload.
The repository itself is stateless. Use `orm::session` when an application
needs a bounded unit of work with identity-map and snapshot dirty checking:

```cpp
orm::session unit_of_work(connection);
auto alice = unit_of_work.load<person_result>(7);
auto same_alice = unit_of_work.load<person_result>(7); // same address
if (alice) {
  alice->name = "Alice Cooper";
  unit_of_work.flush(); // one explicit transaction; snapshots advance on commit

  unit_of_work.detach(alice);
  auto managed_alice = unit_of_work.merge(*alice); // use the returned managed copy
  unit_of_work.refresh(managed_alice);              // reload mapped columns
}

auto deferred_alice = unit_of_work.defer<person_result>(7);
if (deferred_alice.has_value()) {
  deferred_alice->name = "Updated lazily";
}
unit_of_work.flush(); // also tracks every relation materialized by this session

unit_of_work.discard(); // explicitly restore all managed snapshots
unit_of_work.clear();   // rejects dirty objects instead of silently losing edits
```

`session` is single-threaded and borrows the connection. The identity key is
the modeled entity type plus a normalized integral, enum, string, or fixed
`char[N]` primary key. `load<T>(id)` returns an aliasing `std::shared_ptr<T>`;
the session owns the managed entry, so handles remain valid until released.
For generated models, prefer `find<T>(id)` when schema eager-fetch semantics
should be applied.
`flush()` updates only entries whose mapped snapshots differ. If any update
or commit fails, the transaction is rolled back and snapshots remain dirty;
there is no implicit flush in the destructor.

For an application-owned transaction boundary, use `transactional()` instead
of opening a second transaction around `flush()`:

```cpp
auto count = unit_of_work.transactional([](orm::entity_manager& entity_manager) {
  auto alice = entity_manager.find<person_result>(7);
  if (alice) alice->name = "Alice Cooper";
  entity_manager.flush(); // writes now; commit remains owned by the outer scope
  return alice ? 1 : 0;
});
```

The default `transaction_propagation::required` opens one physical transaction
or joins the transaction already owned by the same session. If a joined scope
throws, the owner becomes rollback-only even when its caller catches that
exception. `mandatory` requires an active scope. `supports`, `not_supported`,
and `never` run without creating a transaction when no scope is active.
`requires_new` starts a transaction only when there is no active scope.
Non-transactional propagation does not auto-flush; an explicit `flush()` keeps
its normal standalone transaction behavior.
Because one ORM connection supports one active transaction, `requires_new` and
`not_supported` fail with `ORM_STATUS_UNSUPPORTED` when they would need to
suspend an existing transaction; `never` fails with
`ORM_STATUS_INVALID_STATE`. The ORM never emulates suspension with a savepoint.

Session loads, eager/lazy relation queries, refresh, and explicit `flush()`
inside the callback use the owned transaction. A successful outer scope
auto-flushes once and commits. A callback, flush, rollback-only, or commit
failure rolls back the database and restores the session graph to its state at
scope entry, including detaching identities first loaded or persisted inside
the failed scope. The callback returns `void` or a nothrow-movable value;
returning a reference is rejected at compile time. Transactional session state
is single-threaded, like the rest of `session`. Entering an owned scope takes
an O(n) checkpoint of the bounded managed graph (`n <= max_entities`) so
rollback can restore both identity membership and entity snapshots.

`contains(handle)` and `state(handle)` distinguish exact managed handles from
detached objects; the public states are `detached`, `managed`, `added`, and
`removed`. `detach(handle)` releases that identity-map entry and any
materialized relations whose policy includes `detach`. Existing handles and
their values remain valid, while unflushed edits and pending insert/remove work
for detached entries are no longer part of the unit of work.

`merge(value)` copies a detached value into the identity map and returns the
managed instance; the input object remains detached and must not be used as the
managed result. An existing row starts from a fresh datastore snapshot, while a
missing row enters the `added` state. Versioned stale values still fail the
optimistic check at flush. `refresh(handle)` requires a clean/dirty managed row,
reloads mapped columns, and resets its snapshot. It intentionally preserves
relation members excluded from the column model, and rejects detached, added,
removed, or missing entities. Schema-generated models apply lifecycle cascades
automatically. Handwritten macro models do not declare relations, so use
`merge_graph()`, `refresh_graph()`, or `detach_graph()` with descriptors whose
policy contains the corresponding operation.

Optimistic locking is opt-in. Generated schemas use `[version(1)]`; handwritten
models use:

```cpp
struct account {
  std::uint64_t id;
  std::uint64_t version;
  std::string name;
};

ORM_MODEL_VERSIONED_ENTITY(account, "accounts", id, version,
                           id, version, name)
```

For a mutable versioned entity, `repository::update()` increments the version
and executes `UPDATE ... WHERE primary_key = ? AND version = ?`. A managed
`session` applies the same condition to updates and removals. Exactly one row
must match; otherwise `ORM_STATUS_INVALID_STATE` reports an optimistic-lock
conflict. Failed flushes restore the in-memory version, retain user edits, and
leave the session dirty. Applications can reload or discard explicitly; the
ORM does not merge a stale object automatically. Direct `delete_by_id()` stays
an unconditional command because it has no expected version argument.

SQLite, PostgreSQL, MongoDB, and TidesDB can preserve the conditional mutation
contract. Redis currently rejects versioned UPDATE/DELETE with
`ORM_STATUS_UNSUPPORTED`: queued transaction results are unavailable until
`EXEC`, so the current Redis transaction model cannot abort the complete unit
of work after observing a stale version.

Lazy loading is explicit: `defer<T>(id)` creates a primary-key to-one proxy,
`defer_one<T>(foreign_key, value)` creates a foreign-key to-one proxy, and
`defer_many<T>(foreign_key, value)` creates a cached to-many proxy. No query is
issued until `get()`, `has_value()`, `value()`, `size()`, `empty()`, or `at()`
first accesses the proxy. Materialized targets enter the session identity map,
so modifying a loaded relation participates in the same transactional
`flush()`. Proxies are bound to a session lifetime token and report
`ORM_STATUS_INVALID_STATE` after that session is destroyed, even when they had
already cached a value.
`find_one<T>(foreign_key, value)` is the eager to-one counterpart. It returns
an empty pointer for no row and fails with `ORM_STATUS_DATASTORE_ERROR` before
changing the identity map when the relation query violates to-one cardinality.

For a known set of to-one keys, `session::load_many<T>(ids)` performs one
query-builder predicate batch, reuses already managed entries, preserves input
order, and skips missing rows. Duplicate keys produce the same managed pointer.
The current portable query uses an OR group of primary-key equality predicates,
not a backend-native `IN` or join fetch. Requests are limited by
`session::default_prefetch_batch_limit` (256) unless an explicit limit is
supplied.

This provides update cascading for the materialized unit-of-work graph.
Handwritten models attach insert/delete cascading with explicit relationship
descriptors:

```cpp
struct order_line {
  std::int64_t id;
  std::int64_t order_id;
  std::string sku;
};
ORM_MODEL_WITH_NAME(order_line, "order_lines", id, order_id, sku)

struct order_record {
  std::int64_t id;
  std::string customer;
  std::vector<order_line> lines;
};
// Relationship containers are deliberately omitted from the column model.
ORM_MODEL_WITH_NAME(order_record, "orders", id, customer)

const auto lines = orm::relation::many(
    &order_record::lines, &order_record::id, &order_line::order_id,
    orm::relation::cascade_policy::all |
        orm::relation::cascade_policy::orphan_remove);

auto order = unit_of_work.persist_graph(
    order_record{42, "Alice", {{420, 42, "SKU-1"}}}, lines);
unit_of_work.flush();             // parent INSERT, then child INSERT
unit_of_work.remove_graph(order, lines);
unit_of_work.flush();             // child DELETE, then parent DELETE
```

`cascade_policy` supports `none`, `persist`, `remove`, `merge`, `refresh`,
`detach`, `all`, and the explicit `orphan_remove` policy. `all` covers the five
entity lifecycle operations but deliberately excludes orphan removal; combine it with
`orphan_remove` when replacing or resetting a tracked to-one relation, or when
removing an element from a tracked collection, should delete the previous child
row.
`relation::one()` describes an embedded, `std::optional`, or
`std::shared_ptr` to-one value; `relation::many()` accepts either
`std::vector<Child>` or `std::vector<std::shared_ptr<Child>>`. An overload can
bind `Owner::id` to `Child::owner_id`, as in the example above. The binding
runs before each child is staged, so the foreign key is owned by
the parent identity rather than copied from potentially stale input. Nested
descriptors form deeper graphs. Flush orders persist operations from parent to
child and removals from child to parent, then executes the complete graph in
one backend transaction. A failure rolls back the transaction without
accepting pending snapshots.

`orphan_remove` is supported for registered `one()` and `many()` relations.
For to-one relations, replacing a value or resetting an optional/shared_ptr
deletes the previous child on flush. All orphan tracking requires the relation
to have been registered by generated `persist()`/`merge()` metadata or an
explicit `persist_graph()`/`merge_graph()`.
When `persist` is also enabled, children appended to a registered collection
are staged on the next flush and receive the bound parent foreign key.

Sessions default to at most `session::default_entity_limit` (4096) managed
entities, accept a custom limit in the constructor, and reject relation depth
above 64 with `ORM_STATUS_LIMIT_EXCEEDED`. Backends without the required
transaction semantics report their existing `ORM_STATUS_UNSUPPORTED` error;
the session does not silently degrade to partial cascade writes.

Mapping follows projection order, not database column names. The projection
column count must exactly match the flattened modeled field count. Supported
owning fields are signed and unsigned integers, floating-point values, `bool`,
enums, `std::string`, `std::vector<std::uint8_t>`, `std::optional<T>`, fixed
`char[N]` arrays, and nested modeled records. SQL `NULL` maps to
`std::nullopt`; assigning it to any other field reports
`ORM_STATUS_NULL_VALUE`. Borrowed fields such as `std::string_view` are rejected
because the returned objects outlive the source `result`.

## C11 quick start

The C DSL uses value expressions rather than operator overloading. Expressions
are applied with `where_expr` and `having_expr`, so normal C queries do not need
manual `begin_where`/`end_where` calls.

```c
#include "orm.h"

#include <stdint.h>

int main(void) {
  orm_option_t options[2];
  orm_config_t config;
  orm_error_t error;
  orm_connection_t *connection = NULL;
  orm_query_t *query = NULL;
  orm_result_t *result = NULL;
  orm_chain_t chain;
  orm_status_t status = ORM_STATUS_OK;
  uint64_t rows = 0;

  orm_error_init(&error);
  orm_config(&config);
  options[0].keyword = orm_view("filename");
  options[0].value = orm_view(":memory:");
  options[1].keyword = orm_view("open_mode");
  options[1].value = orm_view("read_write_create");
  config.driver = orm_view("sqlite");
  config.options = options;
  config.option_count = 2;

  status = orm_connect(&config, &connection, &error);
  if (status != ORM_STATUS_OK) goto cleanup;

  status = orm_raw(
      connection,
      orm_view("create table person(id integer, name text, active integer)"),
      &query, &error);
  if (status != ORM_STATUS_OK) goto cleanup;
  status = orm_query_execute(query, &result, &error);
  if (status != ORM_STATUS_OK) goto cleanup;
  orm_result_destroy(result);
  result = NULL;
  orm_query_destroy(query);
  query = NULL;

  status = orm_insert(connection, orm_view("person"), &query, &error);
  if (status != ORM_STATUS_OK) goto cleanup;
  chain = orm_chain(query, &error);
  status = chain.set(&chain, orm_view("id"), orm_i64(1))
      ->set(&chain, orm_view("name"), orm_text("Alice"))
      ->set(&chain, orm_view("active"), orm_bool(1))
      ->execute(&chain, &result);
  if (status != ORM_STATUS_OK) goto cleanup;
  orm_result_destroy(result);
  result = NULL;
  orm_query_destroy(query);
  query = NULL;

  status = orm_query_create(connection, orm_view("person"), &query, &error);
  if (status != ORM_STATUS_OK) goto cleanup;
  chain = orm_chain(query, &error);
  status = chain.select.all(&chain)
      ->where_expr(
          &chain,
          ORM_AND(
              ORM_GE("id", orm_i64(1)),
              ORM_OR(
                  ORM_EQ("active", orm_bool(1)),
                  ORM_LIKE("name", orm_text("A%")))))
      ->execute(&chain, &result);
  if (status != ORM_STATUS_OK) goto cleanup;
  status = orm_result_row_count(result, &rows, &error);

cleanup:
  orm_result_destroy(result);
  orm_query_destroy(query);
  orm_disconnect(connection);
  return status == ORM_STATUS_OK && rows == 1 ? 0 : 1;
}
```

### C expression grammar

| Purpose | API |
| --- | --- |
| Equality and ordering | `ORM_EQ`, `ORM_NE`, `ORM_LT`, `ORM_LE`, `ORM_GT`, `ORM_GE` |
| Text matching | `ORM_LIKE`, `ORM_NOT_LIKE` |
| Boolean groups | `ORM_AND`, `ORM_OR` |
| Aggregate HAVING | `ORM_AGG_EQ`, `ORM_AGG_NE`, `ORM_AGG_LT`, `ORM_AGG_LE`, `ORM_AGG_GT`, `ORM_AGG_GE` |
| Bound values | `orm_i64`, `orm_u64`, `orm_f64`, `orm_bool`, `orm_text`, `orm_null` |

`orm_expression_t` is a source-only bounded value and performs no allocation.
Its column and text views are borrowed until `where_expr` or `having_expr`
returns; the query copies them during that call. The default capacity is 64
tokens. Define `ORM_C_EXPRESSION_CAPACITY` before including `orm.h` to select a
different local limit. Overflow returns `ORM_STATUS_LIMIT_EXCEEDED` before the
query is modified.

`orm_chain_t` is also source-only. It owns no resources and must remain inside
one consuming module. Its `query` and `error` pointers are borrowed. Each method
receives the address of the same chain as its explicit `self`; intermediate
methods return that pointer. The first error is sticky and prevents later
methods from changing the query.

The procedural grouping functions remain available when a query must be built
incrementally from runtime input:

- `orm_query_begin_where_group` / `orm_query_end_where_group`
- `orm_query_begin_having_group` / `orm_query_end_having_group`

## Structured queries and raw SQL

Structured queries validate portable ASCII identifiers and bind every value.
They support:

- selected columns and aggregate projections
- INSERT and UPDATE assignments
- nested WHERE and HAVING conditions
- INNER and LEFT JOIN conditions
- GROUP BY, one ORDER BY expression, LIMIT, and OFFSET

Use `orm_raw` only for trusted SQL that cannot be represented by the structured
builder. A raw query accepts ordered values through `orm_query_bind` or the C
chain's `bind` method. Raw SQL is limited to one statement and uses the selected
backend's native placeholder syntax.

Structured SQL skeletons are rendered through TurboUtils Mustache4C templates:
the template controls clause order and optional sections, while recursive
WHERE/HAVING trees and comma-separated projection/assignment lists are
pre-rendered by the builder. Parameter binding and `max_query_bytes` checks
remain outside the template engine.

## Backend configuration

Backend selection is data, not part of the public function names. After
initializing `orm_config_t` with `orm_config`, set `config.driver` to
`orm_view("sqlite")`, `orm_view("postgresql")`, or, when built,
`orm_view("redis")` / `orm_view("tidesdb")`.

Common SQLite options:

| Option | Values |
| --- | --- |
| `filename` | Database filename or `:memory:` |
| `open_mode` | `read_only`, `read_write`, or `read_write_create` |
| `busy_timeout_ms` | Finite lock wait in milliseconds |

SQLite stores integers as signed 64-bit values, so `ORM_VALUE_UINT64`
parameters greater than `INT64_MAX` are rejected with
`ORM_STATUS_OUT_OF_RANGE`; the PostgreSQL, Redis, and TidesDB backends store
the full unsigned range.

Common PostgreSQL options use libpq connection keywords such as `host`, `port`,
`user`, `password`, and `dbname`. Configuration strings are borrowed only by
`orm_connect` and are never retained by the connection.

For a preassembled libpq connection string, pass the ORM option `conninfo`.
The driver forwards it as libpq's expandable `dbname`; keep `password` as a
separate option. `conninfo` cannot be combined with `host`, `hostaddr`, `port`,
`dbname`, `user`, or `service`, because mixed coordinate precedence would be
ambiguous.

The real PostgreSQL gate is opt-in and therefore does not affect the normal
core-only build:

```powershell
cmake --preset win-dev-pg-user -DORM_POSTGRES_LIVE_TESTS=ON
cmake --build --preset win-dev-pg-user --target orm_postgres_live_test
$env:TURBODB_ORM_PGSQL_TEST_CONNINFO =
  'host=127.0.0.1 port=15432 dbname=test user=test sslmode=disable connect_timeout=5'
$env:PGPASSWORD = '<resolved outside logs>'
ctest --preset win-dev-pg-user -R '^orm_postgres_live$' --output-on-failure
```

With `ORM_POSTGRES_LIVE_TESTS=OFF` (the default), CTest lists this gate as
`Disabled`. The test creates a process-scoped `orm_pg_live_*` schema, exercises
parameters, BYTEA, SQLSTATE mapping, statement timeout, and result bounds, and
drops the schema in teardown. Neither the test nor the ORM logs credentials,
connection strings, or bound values.

**Large result sets**: libpq's `PQexecParams` buffers the complete result
before the ORM checks `max_result_rows`/`max_result_bytes`, so those limits are
safety upper bounds, not streaming thresholds. For unbounded or very large
queries, use server-side cursors (`DECLARE`/`FETCH`) or a streaming layer
outside ORM.

### Unified explicit transactions

PostgreSQL, SQLite, Redis, and TidesDB share the same owning transaction API.
Queries must be executed through the transaction handle while it is active;
executing them directly through the same connection fails with
`ORM_STATUS_INVALID_STATE`. Transactions and queries must originate from the
same connection.

```cpp
auto transaction = connection.begin_transaction(
    orm::isolation_level::serializable);
auto inserted = connection.insert("person")
    .set("id", 1)
    .set("name", "Alice")
    .execute(transaction);
transaction.commit();
```

Transactions are move-only owning handles. Destroying an active handle performs
a best-effort rollback. Commit failure is terminal because a storage or
transport failure may have crossed the native commit point; the failed handle
can only be destroyed.

| Backend | Isolation mapping | Savepoints | Result availability |
| --- | --- | --- | --- |
| PostgreSQL | `READ_UNCOMMITTED` -> `READ_COMMITTED`; `SNAPSHOT` -> `REPEATABLE_READ`; other levels native | Native | Immediate |
| SQLite | All requests use SQLite's stronger serializable transaction | Native | Immediate |
| Redis | `SERIALIZABLE` only (`MULTI`/`EXEC`) | Unsupported | Deferred until successful `commit()` |
| TidesDB v9 | All five levels native | Native v9 savepoints | Immediate |

PostgreSQL's [transaction isolation](https://www.postgresql.org/docs/current/transaction-iso.html)
mapping follows its MVCC model: `READ UNCOMMITTED` behaves as `READ COMMITTED`,
and `REPEATABLE READ` is snapshot isolation. SQLite
[serializes transactions](https://www.sqlite.org/isolation.html) and isolates
separate connections, while reads on the transaction's own connection see
preceding uncommitted writes.

Redis [queues transaction commands](https://redis.io/docs/latest/develop/using-commands/transactions/)
and receives only `QUEUED` before `EXEC`.
Consequently, a Redis result returned by `execute(transaction)` is owned but
deferred: reading its row count, columns, cells, or affected-row count before
commit returns `ORM_STATUS_INVALID_STATE`. After a successful commit the same
handle exposes the corresponding ordered `EXEC` reply. Rollback uses `DISCARD`
and leaves deferred results unavailable. Redis does not roll back other queued
commands when one command fails at execution time, so commit reports
`ORM_STATUS_DATASTORE_ERROR` with an explicit partial-execution warning.

### Redis Query Engine adapter

The Redis adapter follows the same separation used by Spring Data Redis:
entity keys and mapping are distinct from derived query indexes. ORM entities
are Redis hashes named `orm:{<table>}:<id>` by default; their Query Engine
index is `idx:<table>`. The application or deployment owns index creation and
schema migration. For example:

```redis
FT.CREATE idx:person ON HASH PREFIX 1 "orm:{person}:" SCHEMA \
  id TAG name TEXT status TAG age NUMERIC score NUMERIC country TAG
```

The adapter maps explicit projection, predicates, sort, and pagination to
`FT.SEARCH`, and maps `GROUP BY` plus `COUNT(*)`, `SUM`, `AVG`, `MIN`, and `MAX`
reducers to `FT.AGGREGATE`. Every generated query declares `DIALECT 2`, so the
adapter requires a RediSearch build that supports DIALECT 2. Text equality
expects a `TAG` field, `LIKE` expects a `TEXT` field, and numeric comparisons
expect `NUMERIC`. `SELECT *`, joins, raw SQL, SQL-null predicates, `HAVING`,
and `COUNT(column)` return `ORM_STATUS_UNSUPPORTED`; the adapter never hides a
missing index by falling back to `SCAN`.

`LIKE`/`NOT LIKE` are translated to DIALECT 2 wildcard patterns
(`%` becomes `*`, `_` becomes `?`; literal `*`, `?`, quotes, and backslashes
are backslash-escaped so SQL treats them as plain characters). Wildcard
patterns require RediSearch 2.6 or later. Numeric `!=` is rendered as
`(@field:[-inf +inf] -@field:[x x])` and `NOT LIKE` first requires the field to
contain at least one term, so rows whose field is absent (SQL NULL) are
excluded like they are in SQL. Text/tag `!=` uses plain negation and therefore
still includes rows whose field is absent; use an explicit `IS NULL`-style
filter or a separate existence predicate when that distinction matters.

Double parameters are rendered in fixed-point decimal form because RediSearch
numeric ranges do not accept C scientific notation; INSERT/UPDATE hash values
use the same encoding so stored values and query bounds stay consistent.
Entity ids must be non-empty: an empty-string text id is rejected with
`ORM_STATUS_INVALID_ARGUMENT` (TidesDB accepts empty text ids).

INSERT and id-targeted UPDATE use cached Lua scripts and hashes; DELETE uses
Redis' atomic `DEL`. A definite `NOSCRIPT` reply reloads and retries once.
`SEND_UNCERTAIN` or `REPLY_UNKNOWN` is returned as a connection failure with an
explicit do-not-retry diagnostic because the mutation may already have run.
All Lua keys use the table hash tag, so one mutation stays in one Redis Cluster
slot. `ttl_seconds` applies a fixed TTL after INSERT and UPDATE.

Redis options are:

| Option | Default | Meaning |
| --- | --- | --- |
| `host`, `port` | `127.0.0.1`, `6379` | Redis endpoint |
| `username`, `password`, `database` | empty, empty, `0` | Authentication and logical database |
| `timeout_ms`, `command_timeout_ms` | `5000`, `5000` | Finite connect and command timeouts |
| `id_column` | `id` | Required entity identity field |
| `key_prefix`, `index_prefix` | `orm:`, `idx:` | Entity-key and Query Engine index prefixes |
| `ttl_seconds` | `0` | Fixed TTL; zero disables expiry |
| `transaction_command_limit` | `100` | Maximum queued commands per explicit transaction; range `1..65536` |

The Redis client is coroutine-native, so `orm_connect` and later query calls
for this driver must run in the same active CoroNet coroutine. Connections and
queries remain single-thread domains. For batching below the ORM layer,
`redis_pipeline_execute_result` preserves ordered replies and the same
transport outcome states; pipelining improves round trips but is not atomic.

The live TinyTest BDD is opt-in so ordinary builds do not depend on a local
service. It uses only dedicated `turbodb:bdd:*`/`turbodb_bdd:*` keys and never
calls `FLUSHDB`:

```powershell
$env:TURBODB_REDIS_LIVE = "1"
$env:TURBODB_REDIS_HOST = "127.0.0.1"
$env:TURBODB_REDIS_PORT = "6379"
ctest --test-dir <orm-build-dir> -R "^orm_redis_live_bdd$" -V
```

The test always verifies ordered pipeline replies and `SCRIPT LOAD` plus
`EVALSHA`. When `FT._LIST` is available it additionally exercises ORM insert,
search, aggregation, update, delete, and native explicit transactions; otherwise
it verifies that connecting the Redis ORM reports `ORM_STATUS_UNSUPPORTED`
instead of scanning keys. A deterministic fake-protocol contract test covers
the deferred `MULTI`/`EXEC` result lifecycle on systems without Query Engine.

Design references: [Redis Query Engine](https://redis.io/docs/latest/develop/ai/search-and-query/query/),
[Redis aggregation pipelines](https://redis.io/docs/latest/develop/ai/search-and-query/advanced-concepts/aggregations/),
[Spring Data Redis object mapping](https://docs.spring.io/spring-data/redis/reference/redis/redis-repositories/mapping.html),
and [Spring Data Redis secondary indexes](https://docs.spring.io/spring-data/redis/reference/redis/redis-repositories/indexes.html).

### Embedded TidesDB adapter

The TidesDB adapter follows the same backend-neutral query plan as the SQL and
Redis adapters, but does not claim a secondary-index capability that TidesDB
does not provide. Each entity is stored as a versioned, length-delimited row under a
key derived from `key_prefix`, table, value kind, and the configured id column.
INSERT and id-equality UPDATE/DELETE use serializable transactions. SELECT,
GROUP BY, aggregate, HAVING, ordering, and pagination use a snapshot prefix
scan and an in-memory bounded result.

TidesDB explicit transactions cover all five v9 isolation levels and enforce
the same single-active-transaction rule as the SQL and Redis backends: starting
a second transaction on a connection, or running a connection-level query while
a transaction is active, returns `ORM_STATUS_INVALID_STATE`. A query is
executed in a transaction by passing the transaction to `execute()`/`fetch()`;
queries and transactions must originate from the same connection:

```cpp
auto transaction = connection.begin_transaction(orm::isolation_level::serializable);
connection.insert("person")
    .set("id", 1)
    .set("name", "Alice")
    .execute(transaction);
transaction.savepoint("before_update");
connection.update("person")
    .set("name", "Alicia")
    .where("id", orm::comparison::equal, 1)
    .execute(transaction);
transaction.rollback_to("before_update");
transaction.commit();
```

TidesDB `rollback_to` removes the target savepoint and every later savepoint,
while `release` preserves writes and only removes the savepoint marker.

TidesDB queries require explicit projections. Joins and raw SQL return
`ORM_STATUS_UNSUPPORTED`. General selection and aggregation are O(n) in the
number of entities in one table; `max_scan_rows` and `max_scan_bytes` fail fast
before the scan becomes unbounded. This is suitable for embedded datasets and
correctness-first local persistence. A future index layer should be a separate
derived-data adapter rather than a second source of truth.

Database administration remains outside the portable ORM contract. Applications
that need TidesDB backup, checkpoint, compaction, flush, purge, runtime tuning,
statistics, replica promotion, or object-store controls should use the separately
linked `TidesDB::tidesdb` API after all ORM handles for that database directory
have been destroyed. Keeping these operations out of query builders prevents
backend-specific lifecycle and global state changes from masquerading as portable
row operations or racing the ORM-owned database handle.

TidesDB options are:

| Option | Default | Meaning |
| --- | --- | --- |
| `path` | required | Database directory |
| `column_family` | `orm` | ORM-owned column family |
| `create_if_missing` | `true` | Create the column family when absent |
| `id_column` | `id` | Required entity identity field |
| `key_prefix` | `orm:` | Entity-key namespace |
| `ttl_seconds` | `0` | Fixed TTL after INSERT/UPDATE; zero disables expiry |
| `max_scan_rows` | `100000` | Maximum visited rows per SELECT |
| `max_scan_bytes` | `67108864` | Maximum visited key/value bytes per SELECT |

The connection state owns the embedded database handle and TidesDB's process-level
directory lock. Queries and transactions retain that state, so releasing the
connection handle does not invalidate them. Iterator keys and values are decoded
or copied before the iterator advances; no borrowed storage escapes a query. The
integration BDD uses fresh temporary directories and verifies CRUD, aggregation,
explicit transaction state, savepoints, conflict handling, TTL, scan bounds, and
reopen durability without requiring an external service:

```powershell
ctest --test-dir <orm-build-dir> -R "^orm_tidesdb_live_bdd$" -V
```

## Ownership, limits, and thread model

- The caller owns connection, query, transaction, and result handles and releases
  them with `orm_disconnect`, `orm_query_destroy`, `orm_transaction_destroy`, and
  `orm_result_destroy`.
- Queries and transactions retain connection state, so disconnecting the original
  connection handle does not invalidate existing handles.
- A result owns its snapshot. Text returned by `orm_result_get_text` is a
  borrowed view that expires at `orm_result_destroy`.
- `orm_config_t` limits parameters, columns, predicates, joins, assignments,
  condition depth, query bytes, parameter bytes, result rows, and result bytes.
- Limit failures are explicit; the library does not fall back to unbounded
  allocation.
- Connections and their derived queries are single-thread domains. Independent
  connections may run concurrently.

SQLite statements are finalized before query execution returns, and rows are
copied into a bounded result snapshot. PostgreSQL result limits are checked
after libpq returns its buffered result; use a cursor or streaming layer when
the server-side result must be bounded before buffering.

## ABI compatibility

The exported ABI uses opaque handles and plain C data structures. It does not
expose STL types, libpq handles, SQLite handles, or compiler-specific C++
layouts.

ABI v2 provides the generic connection surface:

- `orm_config_t` / `orm_config`
- `orm_connect` / `orm_disconnect`
- backend-neutral query and result functions

ABI v1 names are not provided. Existing v1 callers must update their source and
rebuild. Migration notes for v1 callers:

- Replace v1 execution entry points with `orm_query_*` builders plus
  `orm_query_execute`.
- v2 results are fully independent snapshots; the result owns its data and
  survives the statement/connection that produced it.
- Execute queries that belong to an explicit transaction through
  `orm_query_execute_in_transaction`; direct connection execution while a
  transaction is active returns `ORM_STATUS_INVALID_STATE`.

The C and C++ QueryDSL-inspired layers are source-level adapters over
ABI v2; adding DSL helpers does not add exported symbols or change opaque
handle layouts.

## Typed PostgreSQL compatibility adapter

`include/orm/dbs/postgres/` contains the backend-specific C++17 adapter. It
uses the same `ORM_MODEL*` metadata as generic `fetch<T>()`; there is no second
metadata contract. `ORM_FIELD` and `ORM_TYPE` remain query-adapter helpers.

The adapter remains backend-specific. Its offline regression test is
`tests/dbs/postgres/test.cpp`.

## Binary data handling

ORM v2.1.0 does not provide a native `ORM_VALUE_BLOB` type. Binary data can be
stored using one of three approaches:

1. **Hex encoding**: Universal, simple, +100% storage overhead
2. **Base64 encoding**: More compact (+33%), requires encoder
3. **Native SQL**: Backend-specific (PostgreSQL `BYTEA`, SQLite `BLOB`), zero overhead

For complete examples, implementation guidance, and performance benchmarks, see
[examples/BINARY_DATA_GUIDE.md](examples/BINARY_DATA_GUIDE.md).

Quick example (hex encoding):
```cpp
// Encode binary data to hex string
const std::vector<uint8_t> binary = {0x89, 0x50, 0x4E, 0x47};
std::string hex = hex_encode(binary);  // "89504e47"

// Insert
conn.insert(files)
    .set(files.id, 1)
    .set(files.data_hex, hex)
    .execute();

// Retrieve and decode
auto result = conn.select(files.data_hex)
    .from(files)
    .where(files.id == 1)
    .fetch();
auto decoded = hex_decode(result.text(0, 0));
```

Recommended strategy:
- Files < 10KB: hex or base64 encoding
- Files 10KB-1MB: base64 encoding
- Files > 1MB: store file path, use object storage (S3/OSS)

## Repository layout

- `include/orm/orm.h`: public C11 ABI and C DSL
- `include/orm/orm.hpp`: public C++17 typed DSL
- `include/orm/model.hpp`: canonical C++17 entity-model contract
- `include/orm/dbs/`: backend-specific compatibility adapters
- `src/abi/`: query model, validation, rendering, and exported ABI
- `src/dbs/`: backend implementations
- `tests/abi/`: C and C++ cross-backend API tests
- `tests/dbs/`: backend-specific compatibility tests
- `cmake/`: installed package configuration
- `examples/`: usage examples and guides

## License

Orm is distributed under the [MIT License](LICENSE).
