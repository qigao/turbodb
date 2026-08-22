#include "orm.hpp"

#include <tinytest.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

  enum class access_level : std::uint8_t { reader = 1, writer = 2 };

  struct reflected_person {
    std::int32_t id;
    std::string name;
    access_level access;
    bool active;
    float score;
    std::optional<std::string> note;
    char code[8];
    std::vector<std::uint8_t> payload;
  };

  ORM_MODEL_WITH_NAME(reflected_person, "typed_people", id, name, access, active, score, note,
                      code, payload)

  struct reflected_child {
    std::int32_t id;
    std::int32_t parent_id;
    std::string value;
  };

  ORM_MODEL_WITH_NAME(reflected_child, "typed_children", id, parent_id, value)

  struct cascade_child {
    std::int32_t id;
    std::int32_t parent_id;
    std::string value;
  };

  ORM_MODEL_WITH_NAME(cascade_child, "cascade_children", id, parent_id, value)

  struct cascade_parent {
    std::int32_t id;
    std::string name;
    std::vector<cascade_child> children;
  };

  ORM_MODEL_WITH_NAME(cascade_parent, "cascade_parents", id, name)

  static_assert(!orm::model::has_relations_v<cascade_parent>,
                "handwritten column models must not imply graph metadata");

  const auto cascade_children = orm::relation::many(
      &cascade_parent::children, &cascade_parent::id, &cascade_child::parent_id,
      orm::relation::cascade_policy::all | orm::relation::cascade_policy::orphan_remove);
  const auto cascade_children_unbound = orm::relation::many(
      &cascade_parent::children, orm::relation::cascade_policy::all);

  struct cascade_profile {
    std::int32_t id;
    std::int32_t account_id;
    std::string label;
  };

  ORM_MODEL_WITH_NAME(cascade_profile, "cascade_profiles", id, account_id, label)

  struct cascade_account {
    std::int32_t id;
    std::string name;
    cascade_profile profile;
  };

  ORM_MODEL_WITH_NAME(cascade_account, "cascade_accounts", id, name)

  const auto cascade_account_profile = orm::relation::one(
      &cascade_account::profile, &cascade_account::id, &cascade_profile::account_id,
      orm::relation::cascade_policy::all);

  struct pointer_child {
    std::int32_t id;
    std::int32_t owner_id;
    std::string value;
  };

  ORM_MODEL_WITH_NAME(pointer_child, "pointer_children", id, owner_id, value)

  struct pointer_parent {
    std::int32_t id;
    std::string name;
    std::optional<pointer_child> optional_child;
    std::shared_ptr<pointer_child> shared_child;
    std::vector<std::shared_ptr<pointer_child>> shared_children;
  };

  ORM_MODEL_WITH_NAME(pointer_parent, "pointer_parents", id, name)

  const auto optional_child_relation = orm::relation::one(
      &pointer_parent::optional_child, &pointer_parent::id, &pointer_child::owner_id,
      orm::relation::cascade_policy::all | orm::relation::cascade_policy::orphan_remove);
  const auto shared_child_relation = orm::relation::one(
      &pointer_parent::shared_child, &pointer_parent::id, &pointer_child::owner_id,
      orm::relation::cascade_policy::all | orm::relation::cascade_policy::orphan_remove);
  const auto shared_children_relation = orm::relation::many(
      &pointer_parent::shared_children, &pointer_parent::id, &pointer_child::owner_id,
      orm::relation::cascade_policy::all | orm::relation::cascade_policy::orphan_remove);
  const auto shared_child_without_cascade = orm::relation::one(
      &pointer_parent::shared_child, &pointer_parent::id,
      &pointer_child::owner_id, orm::relation::cascade_policy::none);
  const auto shared_child_orphan_without_persist = orm::relation::one(
      &pointer_parent::shared_child, &pointer_parent::id,
      &pointer_child::owner_id, orm::relation::cascade_policy::orphan_remove);

  struct required_note {
    std::string note;
  };

  ORM_MODEL(required_note, note)

  struct narrow_id {
    std::uint8_t id;
  };

  ORM_MODEL(narrow_id, id)

  struct short_code {
    char code[4];
  };

  ORM_MODEL(short_code, code)

  struct custom_key_entity {
    std::int32_t entity_key;
    std::string label;
  };

  ORM_MODEL_ENTITY(custom_key_entity, "custom_key_entities", entity_key, entity_key, label)

  struct versioned_entity {
    std::int32_t id;
    std::uint64_t version;
    std::string label;
  };

  ORM_MODEL_VERSIONED_ENTITY(versioned_entity, "versioned_entities", id, version,
                             id, version, label)

  struct reflected_person_entity {};

  struct person_summary final {
    person_summary(std::int32_t input_id, std::string input_name)
        : id(input_id), name(std::move(input_name)) {}

    std::int32_t id;
    std::string name;
  };

  struct person_summary_mapper final {
    person_summary operator()(std::int32_t id, std::string name) const {
      return person_summary(id, std::move(name));
    }
  };

  struct incompatible_summary_mapper final {
    person_summary operator()(std::string name) const {
      return person_summary(0, std::move(name));
    }
  };

  static_assert(!std::is_default_constructible_v<person_summary>);

  struct reflected_person_table final : orm::table<reflected_person_entity> {
    reflected_person_table()
        : orm::table<reflected_person_entity>("typed_people"), id(*this, "id"),
          name(*this, "name"), access(*this, "access"), active(*this, "active"),
          score(*this, "score"), note(*this, "note"), nullable_note(*this, "note"),
          code(*this, "code"), payload(*this, "payload") {}

    orm::column<std::int32_t> id;
    orm::column<std::string> name;
    orm::column<access_level> access;
    orm::column<bool> active;
    orm::column<float> score;
    orm::column<std::string> note;
    orm::column<std::optional<std::string>> nullable_note;
    orm::column<std::string> code;
    orm::column<std::vector<std::uint8_t>> payload;
  };

  const reflected_person_table people;

  using inferred_person_projection =
      decltype(std::declval<const orm::connection &>()
                   .select(people.id, people.name, people.score)
                   .from(people));
  static_assert(std::is_same_v<typename inferred_person_projection::row_type,
                               std::tuple<std::int32_t, std::string, float>>);
  using inferred_aggregate_projection =
      decltype(std::declval<const orm::connection &>()
                   .select(people.name, orm::count(people.id), orm::avg(people.score))
                   .from(people));
  static_assert(std::is_same_v<typename inferred_aggregate_projection::row_type,
                               std::tuple<std::string, std::uint64_t, double>>);
  static_assert(std::is_same_v<decltype(people.nullable_note == std::string("review")),
                               orm::predicate>);
  static_assert(std::is_same_v<decltype(people.nullable_note == std::nullopt), orm::predicate>);

  template <typename Query, typename = void> struct has_scalar_fetch : std::false_type {};
  template <typename Query>
  struct has_scalar_fetch<Query,
                          std::void_t<decltype(std::declval<Query &>().fetch_scalars())>>
      : std::true_type {};

  using inferred_single_projection =
      decltype(std::declval<const orm::connection &>().select(people.name).from(people));
  static_assert(has_scalar_fetch<inferred_single_projection>::value);
  static_assert(!has_scalar_fetch<inferred_person_projection>::value);

  template <typename Query, typename Mapper, typename = void>
  struct has_mapped_fetch : std::false_type {};
  template <typename Query, typename Mapper>
  struct has_mapped_fetch<
      Query, Mapper,
      std::void_t<decltype(std::declval<Query &>().fetch_mapped(std::declval<Mapper>()))>>
      : std::true_type {};

  using inferred_summary_projection =
      decltype(std::declval<const orm::connection &>()
                   .select(people.id, people.name)
                   .from(people));
  static_assert(has_mapped_fetch<inferred_summary_projection, person_summary_mapper>::value);
  static_assert(
      !has_mapped_fetch<inferred_summary_projection, incompatible_summary_mapper>::value);

  orm::config sqlite_config() {
    orm::config configuration("sqlite");
    configuration.option("filename", ":memory:").option("open_mode", "read_write_create");
    return configuration;
  }

  orm::connection populated_connection() {
    orm::connection connection(sqlite_config());
    (void)connection
        .raw("create table typed_people("
             "id integer primary key, name text not null, access integer not null, "
             "active integer not null, score real not null, note text, "
             "code text not null, payload blob not null)")
        .execute();

    (void)connection.insert("typed_people")
        .set("id", 1)
        .set("name", "Alice")
        .set("access", access_level::writer)
        .set("active", true)
        .set("score", 9.5)
        .set("note", nullptr)
        .set("code", "A1")
        .set("payload", std::vector<std::uint8_t>{0, 1, 255})
        .execute();

    (void)connection.insert("typed_people")
        .set("id", 2)
        .set("name", "Bob")
        .set("access", access_level::reader)
        .set("active", false)
        .set("score", 7.25)
        .set("note", "review")
        .set("code", "B2")
        .set("payload", std::vector<std::uint8_t>{4, 5})
        .execute();
    return connection;
  }

  void create_pointer_schema(orm::connection &connection) {
    (void)connection.raw("pragma foreign_keys = on").execute();
    (void)connection
        .raw("create table pointer_parents(id integer primary key, name text not null)")
        .execute();
    (void)connection
        .raw("create table pointer_children(id integer primary key, owner_id integer not null "
             "references pointer_parents(id), value text not null)")
        .execute();
  }

  template <typename Operation> orm_status_t caught_status(Operation &&operation) {
    try {
      std::forward<Operation>(operation)();
    } catch (const orm::status_error &error) {
      return error.status();
    }
    return ORM_STATUS_OK;
  }

} // namespace

spec("ORM reflected typed fetch") {
  group("materialization") {
    it("maps every supported field through the generic query") {
      auto connection = populated_connection();
      auto query = connection.select("typed_people");
      query.column("id")
          .column("name")
          .column("access")
          .column("active")
          .column("score")
          .column("note")
          .column("code")
          .column("payload")
          .order_by("id", orm::sort_order::ascending);

      const auto rows = query.fetch<reflected_person>();
      check_equal(rows.size(), 2u);
      check_equal(rows[0].id, 1);
      check_equal(rows[0].name.c_str(), "Alice");
      check_equal(static_cast<int>(rows[0].access), static_cast<int>(access_level::writer));
      check_true(rows[0].active);
      check_within(rows[0].score, 9.5f, 0.0001f);
      check_false(rows[0].note.has_value());
      check_equal(rows[0].code, "A1");
      const std::uint8_t expected_payload[] = {0, 1, 255};
      check_equal(rows[0].payload.size(), 3u);
      check_equal(rows[0].payload.data(), expected_payload, 3u);
      check_equal(rows[1].note.value().c_str(), "review");
    }

    it("maps a typed select projection and a single result row") {
      auto connection = populated_connection();
      const auto rows = connection
                            .select(people.id, people.name, people.access, people.active,
                                    people.score, people.note, people.code, people.payload)
                            .from(people)
                            .where(people.id == 2)
                            .fetch<reflected_person>();
      check_equal(rows.size(), 1u);
      check_equal(rows[0].id, 2);
      check_false(rows[0].active);

      auto query = connection.select("typed_people");
      query.column("id").column("name").where("id", orm::comparison::equal, 1);
      const auto result = query.execute();
      const auto row = result.row<std::tuple<std::int32_t, std::string>>(0);
      check_equal(std::get<0>(row), 1);
      check_equal(std::get<1>(row).c_str(), "Alice");
    }

    it("infers a tuple row from the selected projections") {
      auto connection = populated_connection();
      const auto rows = connection.select(people.id, people.name, people.score)
                            .from(people)
                            .where(people.id == 1)
                            .fetch_typed();

      check_equal(rows.size(), 1u);
      check_equal(std::get<0>(rows[0]), 1);
      check_equal(std::get<1>(rows[0]).c_str(), "Alice");
      check_within(std::get<2>(rows[0]), 9.5f, 0.0001f);
    }

    it("preserves nullable column types in inferred tuples") {
      auto connection = populated_connection();
      const auto rows = connection.select(people.id, people.nullable_note)
                            .from(people)
                            .order_by(people.id.asc())
                            .fetch_typed();

      check_equal(rows.size(), 2u);
      check_false(std::get<1>(rows[0]).has_value());
      check_true(std::get<1>(rows[1]).has_value());
      check_equal(std::get<1>(rows[1])->c_str(), "review");
    }

    it("uses optional values in typed assignments and predicates") {
      auto connection = populated_connection();
      const std::optional<std::string> updated_note("restored");
      check_equal(connection.update(people)
                        .set(people.nullable_note, updated_note)
                        .where(people.id == 1)
                        .execute()
                        .affected_rows(),
                    1u);

      const auto restored = connection.select(people.id, people.nullable_note)
                                .from(people)
                                .where(people.nullable_note == updated_note)
                                .fetch_one_typed();
      check_true(restored.has_value());
      check_equal(std::get<0>(*restored), 1);
      check_equal(std::get<1>(*restored)->c_str(), "restored");

      check_equal(connection.update(people)
                        .set(people.nullable_note, std::nullopt)
                        .where(people.id == 1)
                        .execute()
                        .affected_rows(),
                    1u);
      const auto null_rows = connection.select(people.id)
                                 .from(people)
                                 .where(people.nullable_note == std::nullopt)
                                 .order_by(people.id.asc())
                                 .fetch_typed();
      check_equal(null_rows.size(), 1u);
      check_equal(std::get<0>(null_rows[0]), 1);
    }

    it("returns an optional inferred row for zero or one result") {
      auto connection = populated_connection();
      const auto alice = connection.select(people.id, people.name)
                             .from(people)
                             .where(people.id == 1)
                             .fetch_one_typed();
      check_true(alice.has_value());
      check_equal(std::get<0>(*alice), 1);
      check_equal(std::get<1>(*alice).c_str(), "Alice");

      const auto missing = connection.select(people.id, people.name)
                               .from(people)
                               .where(people.id == 99)
                               .fetch_one_typed();
      check_false(missing.has_value());
    }

    it("rejects multiple rows in a typed single-result query") {
      auto connection = populated_connection();
      check_equal(caught_status([&] {
                     (void)connection.select(people.id)
                         .from(people)
                         .order_by(people.id.asc())
                         .fetch_one_typed();
                   }),
                   ORM_STATUS_INVALID_STATE);
    }

    it("materializes one-column projections as scalar values") {
      auto connection = populated_connection();
      const auto names = connection.select(people.name)
                             .from(people)
                             .order_by(people.id.asc())
                             .fetch_scalars();
      check_equal(names.size(), 2u);
      check_equal(names[0].c_str(), "Alice");
      check_equal(names[1].c_str(), "Bob");

      const auto note = connection.select(people.nullable_note)
                            .from(people)
                            .where(people.id == 2)
                            .fetch_one_scalar();
      check_true(note.has_value());
      check_true(note->has_value());
      check_equal(note->value().c_str(), "review");

      const auto count = connection.select(orm::count_all())
                             .from(people)
                             .fetch_one_scalar();
      check_true(count.has_value());
      check_equal(*count, 2u);
    }

    it("constructs non-modeled DTOs from inferred projection fields") {
      auto connection = populated_connection();
      const auto summaries = connection.select(people.id, people.name)
                                 .from(people)
                                 .order_by(people.id.asc())
                                 .fetch_mapped(person_summary_mapper{});
      check_equal(summaries.size(), 2u);
      check_equal(summaries[0].id, 1);
      check_equal(summaries[0].name.c_str(), "Alice");
      check_equal(summaries[1].id, 2);
      check_equal(summaries[1].name.c_str(), "Bob");

      const auto summary = connection.select(people.id, people.name)
                               .from(people)
                               .where(people.id == 2)
                               .fetch_one_mapped(person_summary_mapper{});
      check_true(summary.has_value());
      check_equal(summary->id, 2);
      check_equal(summary->name.c_str(), "Bob");
    }

    it("builds the projection from reflected metadata") {
      auto connection = populated_connection();
      const auto rows = connection.select<reflected_person>()
                            .order_by(people.id.asc())
                            .fetch<reflected_person>();
      check_equal(rows.size(), 2u);
      check_equal(rows[0].id, 1);
      check_equal(rows[0].name.c_str(), "Alice");
      check_equal(rows[1].id, 2);
      check_equal(rows[1].code, "B2");
    }
  }

  group("repository") {
    it("increments a version and rejects a stale repository update") {
      orm::connection connection(sqlite_config());
      (void)connection
          .raw("create table versioned_entities("
               "id integer primary key, version integer not null, label text not null)")
          .execute();
      const orm::repository<versioned_entity> repository(connection);
      check_equal(repository.insert({1, 0, "original"}).affected_rows(), 1u);

      auto first = repository.find_by_id(1);
      auto stale = repository.find_by_id(1);
      check_true(first.has_value());
      check_true(stale.has_value());
      first->label = "first";
      check_equal(repository.update(*first).affected_rows(), 1u);
      check_equal(first->version, 1u);

      stale->label = "stale";
      check_equal(caught_status([&] { (void)repository.update(*stale); }),
                   ORM_STATUS_INVALID_STATE);
      check_equal(stale->version, 0u);
      const auto stored = repository.find_by_id(1);
      check_true(stored.has_value());
      check_equal(stored->version, 1u);
      check_equal(stored->label.c_str(), "first");
    }

    it("uses reflected custom primary-key metadata across repository and session") {
      orm::connection connection(sqlite_config());
      (void)connection
          .raw("create table custom_key_entities("
               "entity_key integer primary key, label text not null)")
          .execute();
      const orm::repository<custom_key_entity> repository(connection);

      check_equal(repository.insert({10, "ten"}).affected_rows(), 1u);
      check_equal(repository.insert({20, "twenty"}).affected_rows(), 1u);
      const auto found = repository.find_by_id(20);
      check_true(found.has_value());
      check_equal(found->label.c_str(), "twenty");

      orm::session unit_of_work(connection);
      const auto loaded =
          unit_of_work.load_many<custom_key_entity>(std::vector<std::int32_t>{20, 10});
      check_equal(loaded.size(), 2u);
      check_equal(loaded[0]->entity_key, 20);
      check_equal(loaded[1]->entity_key, 10);
      loaded[0]->label = "updated";
      check_equal(unit_of_work.flush(), 1u);
      const auto updated = repository.find_by_id(20);
      check_true(updated.has_value());
      check_equal(updated->label.c_str(), "updated");

      check_equal(repository.delete_by_id(10).affected_rows(), 1u);
      check_false(repository.find_by_id(10).has_value());
    }

    it("finds entities by reflected table and primary-key metadata") {
      auto connection = populated_connection();
      const orm::repository<reflected_person> repository(connection);

      const auto all = repository.find_all();
      check_equal(all.size(), 2u);

      const auto found = repository.find_by_id(2);
      check_true(found.has_value());
      check_equal(found->name.c_str(), "Bob");
      check_equal(static_cast<int>(found->access), static_cast<int>(access_level::reader));

      const auto missing = repository.find_by_id(99);
      check_false(missing.has_value());
    }

    it("inserts updates and deletes a complete reflected entity") {
      auto connection = populated_connection();
      const orm::repository<reflected_person> repository(connection);
      reflected_person entity{3,
                              "Cara",
                              access_level::reader,
                              true,
                              8.0f,
                              std::optional<std::string>("new"),
                              {'C', '3', '\0'},
                              {7, 8, 9}};

      check_equal(repository.insert(entity).affected_rows(), 1u);
      auto inserted = repository.find_by_id(entity.id);
      check_true(inserted.has_value());
      check_equal(inserted->code, "C3");
      check_equal(inserted->payload.size(), 3u);

      entity.name = "Carol";
      entity.active = false;
      entity.note.reset();
      entity.payload = {10, 11};
      check_equal(repository.update(entity).affected_rows(), 1u);

      const auto updated = repository.find_by_id(entity.id);
      check_true(updated.has_value());
      check_equal(updated->name.c_str(), "Carol");
      check_false(updated->active);
      check_false(updated->note.has_value());
      check_equal(updated->payload.size(), 2u);

      check_equal(repository.delete_by_id(entity.id).affected_rows(), 1u);
      check_false(repository.find_by_id(entity.id).has_value());
    }

    it("keeps repository reads and writes on an explicit transaction") {
      auto connection = populated_connection();
      const orm::repository<reflected_person> repository(connection);
      auto transaction = connection.begin_transaction();
      reflected_person entity{4,
                              "Dan",
                              access_level::reader,
                              true,
                              6.0f,
                              std::nullopt,
                              {'D', '4', '\0'},
                              {12}};

      check_equal(repository.insert(entity, transaction).affected_rows(), 1u);
      check_true(repository.find_by_id(4, transaction).has_value());
      entity.name = "Daniel";
      check_equal(repository.update(entity, transaction).affected_rows(), 1u);
      const auto transaction_updated = repository.find_by_id(4, transaction);
      check_true(transaction_updated.has_value());
      check_equal(transaction_updated->name.c_str(), "Daniel");
      const auto typed_transaction_row = connection.select(people.id, people.name)
                                             .from(people)
                                             .where(people.id == 4)
                                             .fetch_one_typed(transaction);
      check_true(typed_transaction_row.has_value());
      check_equal(std::get<0>(*typed_transaction_row), 4);
      check_equal(std::get<1>(*typed_transaction_row).c_str(), "Daniel");
      const auto typed_transaction_name = connection.select(people.name)
                                              .from(people)
                                              .where(people.id == 4)
                                              .fetch_one_scalar(transaction);
      check_true(typed_transaction_name.has_value());
      check_equal(typed_transaction_name->c_str(), "Daniel");
      const auto typed_transaction_summary =
          connection.select(people.id, people.name)
              .from(people)
              .where(people.id == 4)
              .fetch_one_mapped(person_summary_mapper{}, transaction);
      check_true(typed_transaction_summary.has_value());
      check_equal(typed_transaction_summary->id, 4);
      check_equal(typed_transaction_summary->name.c_str(), "Daniel");
      check_equal(repository.delete_by_id(4, transaction).affected_rows(), 1u);
      transaction.commit();
      check_false(repository.find_by_id(4).has_value());
    }
  }

  group("session") {
    it("joins session reads and explicit flushes to one owned transaction") {
      auto connection = populated_connection();
      orm::session unit_of_work(connection);

      const int result = unit_of_work.transactional([](orm::session &current) {
        check_true(current.transaction_active());
        check_false(current.is_rollback_only());
        auto alice = current.load<reflected_person>(1);
        check_not_null(alice.get());
        alice->name = "transactional Alice";
        check_equal(current.flush(), 1u);
        check_false(current.dirty());

        auto bob = current.load<reflected_person>(2);
        check_not_null(bob.get());
        check_equal(bob->name.c_str(), "Bob");
        bob->name = "auto-flushed Bob";
        return 42;
      });

      check_equal(result, 42);
      check_false(unit_of_work.transaction_active());
      const auto stored =
          orm::repository<reflected_person>(connection).find_by_id(1);
      check_true(stored.has_value());
      check_equal(stored->name.c_str(), "transactional Alice");
      const auto auto_flushed =
          orm::repository<reflected_person>(connection).find_by_id(2);
      check_true(auto_flushed.has_value());
      check_equal(auto_flushed->name.c_str(), "auto-flushed Bob");
    }

    it("rolls back database and managed state when an owned scope throws") {
      auto connection = populated_connection();
      orm::session unit_of_work(connection);
      auto alice = unit_of_work.load<reflected_person>(1);
      check_not_null(alice.get());
      std::shared_ptr<reflected_person> scoped_bob;
      bool caught = false;

      try {
        unit_of_work.transactional([&](orm::session &current) {
          auto same_alice = current.load<reflected_person>(1);
          check_equal(same_alice.get(), alice.get());
          same_alice->name = "must roll back";
          check_equal(current.flush(), 1u);
          scoped_bob = current.load<reflected_person>(2);
          check_not_null(scoped_bob.get());
          throw std::runtime_error("application failure");
        });
      } catch (const std::runtime_error &) {
        caught = true;
      }

      check_true(caught);
      check_false(unit_of_work.transaction_active());
      check_equal(unit_of_work.size(), 1u);
      check_true(unit_of_work.contains(alice));
      check_false(unit_of_work.contains(scoped_bob));
      check_false(unit_of_work.dirty());
      check_equal(alice->name.c_str(), "Alice");
      const auto stored =
          orm::repository<reflected_person>(connection).find_by_id(1);
      check_true(stored.has_value());
      check_equal(stored->name.c_str(), "Alice");
    }

    it("marks a joined REQUIRED scope rollback-only after an inner failure") {
      auto connection = populated_connection();
      orm::session unit_of_work(connection);

      const auto status = caught_status([&] {
        unit_of_work.transactional([](orm::session &outer) {
          auto alice = outer.load<reflected_person>(1);
          check_not_null(alice.get());
          alice->name = "outer edit";
          try {
            outer.transactional([](orm::session &inner) {
              check_true(inner.transaction_active());
              throw std::runtime_error("inner failure");
            });
          } catch (const std::runtime_error &) {
          }
          check_true(outer.is_rollback_only());
        });
      });

      check_equal(status, ORM_STATUS_INVALID_STATE);
      check_false(unit_of_work.transaction_active());
      check_equal(unit_of_work.size(), 0u);
      const auto stored =
          orm::repository<reflected_person>(connection).find_by_id(1);
      check_true(stored.has_value());
      check_equal(stored->name.c_str(), "Alice");
    }

    it("enforces propagation modes without faking transaction suspension") {
      auto connection = populated_connection();
      orm::session unit_of_work(connection);

      check_equal(
          caught_status([&] {
            unit_of_work.transactional(
                [](orm::session &) {}, orm::transaction_propagation::mandatory);
          }),
          ORM_STATUS_INVALID_STATE);
      check_equal(
          unit_of_work.transactional(
              [](orm::session &current) {
                check_false(current.transaction_active());
                return 7;
              },
              orm::transaction_propagation::supports),
          7);
      check_equal(
          unit_of_work.transactional(
              [](orm::session &current) {
                check_true(current.transaction_active());
                return 9;
              },
              orm::transaction_propagation::requires_new),
          9);

      unit_of_work.transactional([](orm::session &outer) {
        check_equal(
            caught_status([&] {
              outer.transactional(
                  [](orm::session &) {},
                  orm::transaction_propagation::requires_new);
            }),
            ORM_STATUS_UNSUPPORTED);
        check_equal(
            caught_status([&] {
              outer.transactional(
                  [](orm::session &) {},
                  orm::transaction_propagation::not_supported);
            }),
            ORM_STATUS_UNSUPPORTED);
        check_equal(
            caught_status([&] {
              outer.transactional(
                  [](orm::session &) {}, orm::transaction_propagation::never);
            }),
            ORM_STATUS_INVALID_STATE);
        check_false(outer.is_rollback_only());
      });

      check_equal(
          caught_status([&] {
            unit_of_work.transactional([](orm::session &current) {
              current.set_rollback_only();
              check_true(current.is_rollback_only());
              check_equal(caught_status([&] { (void)current.flush(); }),
                           ORM_STATUS_INVALID_STATE);
            });
          }),
          ORM_STATUS_INVALID_STATE);
    }

    it("keeps a stale managed entity dirty after optimistic lock conflict") {
      orm::connection connection(sqlite_config());
      (void)connection
          .raw("create table versioned_entities("
               "id integer primary key, version integer not null, label text not null)")
          .execute();
      const orm::repository<versioned_entity> repository(connection);
      check_equal(repository.insert({1, 0, "original"}).affected_rows(), 1u);

      orm::session first_session(connection);
      orm::session stale_session(connection);
      auto first = first_session.load<versioned_entity>(1);
      auto stale = stale_session.load<versioned_entity>(1);
      check_not_null(first.get());
      check_not_null(stale.get());

      first->label = "committed";
      check_equal(first_session.flush(), 1u);
      check_equal(first->version, 1u);

      stale->label = "rejected";
      check_equal(caught_status([&] { (void)stale_session.flush(); }),
                   ORM_STATUS_INVALID_STATE);
      check_equal(stale->version, 0u);
      check_true(stale_session.dirty());
      const auto stored = repository.find_by_id(1);
      check_true(stored.has_value());
      check_equal(stored->label.c_str(), "committed");
      check_equal(stored->version, 1u);
    }

    it("rejects removing a stale managed entity") {
      orm::connection connection(sqlite_config());
      (void)connection
          .raw("create table versioned_entities("
               "id integer primary key, version integer not null, label text not null)")
          .execute();
      const orm::repository<versioned_entity> repository(connection);
      check_equal(repository.insert({1, 0, "original"}).affected_rows(), 1u);

      orm::session writer(connection);
      orm::session stale_session(connection);
      auto current = writer.load<versioned_entity>(1);
      auto stale = stale_session.load<versioned_entity>(1);
      check_not_null(current.get());
      check_not_null(stale.get());
      current->label = "newer";
      check_equal(writer.flush(), 1u);

      stale_session.remove(stale);
      check_equal(caught_status([&] { (void)stale_session.flush(); }),
                   ORM_STATUS_INVALID_STATE);
      check_true(stale_session.dirty());
      const auto retained = repository.find_by_id(1);
      check_true(retained.has_value());
      check_equal(retained->version, 1u);
      check_equal(retained->label.c_str(), "newer");
    }

    it("keeps a stale detached merge dirty after optimistic lock conflict") {
      orm::connection connection(sqlite_config());
      (void)connection
          .raw("create table versioned_entities("
               "id integer primary key, version integer not null, label text not null)")
          .execute();
      const orm::repository<versioned_entity> repository(connection);
      check_equal(repository.insert({1, 0, "original"}).affected_rows(), 1u);
      const auto detached = repository.find_by_id(1);
      check_true(detached.has_value());

      auto current = repository.find_by_id(1);
      check_true(current.has_value());
      current->label = "newer";
      check_equal(repository.update(*current).affected_rows(), 1u);

      orm::session unit_of_work(connection);
      auto managed = unit_of_work.merge(*detached);
      check_not_null(managed.get());
      managed->label = "stale merge";
      check_equal(caught_status([&] { (void)unit_of_work.flush(); }),
                   ORM_STATUS_INVALID_STATE);
      check_equal(managed->version, 0u);
      check_true(unit_of_work.dirty());
      const auto retained = repository.find_by_id(1);
      check_true(retained.has_value());
      check_equal(retained->label.c_str(), "newer");
      check_equal(retained->version, 1u);
    }

    it("returns one managed instance and flushes only dirty fields") {
      auto connection = populated_connection();
      orm::session unit_of_work(connection);

      auto first = unit_of_work.load<reflected_person>(1);
      auto second = unit_of_work.load<reflected_person>(1);
      check_true(first != nullptr);
      check_true(first.get() == second.get());
      check_equal(unit_of_work.size(), 1u);
      check_false(unit_of_work.dirty());

      first->name = "Alice updated";
      first->note = "tracked";
      check_true(unit_of_work.dirty());
      check_equal(unit_of_work.flush(), 1u);
      check_false(unit_of_work.dirty());
      check_equal(unit_of_work.flush(), 0u);

      const orm::repository<reflected_person> repository(connection);
      const auto persisted = repository.find_by_id(1);
      check_true(persisted.has_value());
      check_equal(persisted->name.c_str(), "Alice updated");
      check_true(persisted->note.has_value());
      check_equal(persisted->note->c_str(), "tracked");

      unit_of_work.clear();
      check_equal(unit_of_work.size(), 0u);
    }

    it("detaches without destroying the entity and merges a managed copy") {
      auto connection = populated_connection();
      const orm::repository<reflected_person> repository(connection);
      orm::session unit_of_work(connection);

      auto detached = unit_of_work.load<reflected_person>(1);
      check_not_null(detached.get());
      check_true(unit_of_work.contains(detached));
      check_equal(static_cast<int>(unit_of_work.state(detached)),
                   static_cast<int>(orm::entity_state::managed));
      detached->name = "detached edit";

      unit_of_work.detach(detached);
      check_false(unit_of_work.contains(detached));
      check_equal(static_cast<int>(unit_of_work.state(detached)),
                   static_cast<int>(orm::entity_state::detached));
      check_equal(unit_of_work.size(), 0u);
      check_false(unit_of_work.dirty());
      check_equal(unit_of_work.flush(), 0u);
      check_equal(detached->name.c_str(), "detached edit");
      const auto stored_before_merge = repository.find_by_id(1);
      check_true(stored_before_merge.has_value());
      check_equal(stored_before_merge->name.c_str(), "Alice");

      auto managed = unit_of_work.merge(*detached);
      check_not_null(managed.get());
      check_true(managed.get() != detached.get());
      check_true(unit_of_work.contains(managed));
      check_false(unit_of_work.contains(detached));
      check_equal(static_cast<int>(unit_of_work.state(managed)),
                   static_cast<int>(orm::entity_state::managed));
      check_true(unit_of_work.dirty());
      check_equal(unit_of_work.flush(), 1u);
      const auto stored_after_merge = repository.find_by_id(1);
      check_true(stored_after_merge.has_value());
      check_equal(stored_after_merge->name.c_str(), "detached edit");
    }

    it("reports added and removed states and lets detach cancel pending work") {
      orm::connection connection(sqlite_config());
      (void)connection
          .raw("create table custom_key_entities("
               "entity_key integer primary key, label text not null)")
          .execute();
      const orm::repository<custom_key_entity> repository(connection);
      check_equal(repository.insert({1, "stored"}).affected_rows(), 1u);
      orm::session unit_of_work(connection);

      auto added = unit_of_work.merge(custom_key_entity{2, "new"});
      check_equal(static_cast<int>(unit_of_work.state(added)),
                   static_cast<int>(orm::entity_state::added));
      unit_of_work.detach(added);
      check_equal(static_cast<int>(unit_of_work.state(added)),
                   static_cast<int>(orm::entity_state::detached));
      check_equal(unit_of_work.flush(), 0u);
      check_false(repository.find_by_id(2).has_value());

      auto removed = unit_of_work.load<custom_key_entity>(1);
      check_not_null(removed.get());
      unit_of_work.remove(removed);
      check_equal(static_cast<int>(unit_of_work.state(removed)),
                   static_cast<int>(orm::entity_state::removed));
      unit_of_work.detach(removed);
      check_equal(unit_of_work.flush(), 0u);
      check_true(repository.find_by_id(1).has_value());
    }

    it("refreshes mapped fields without replacing relation members") {
      orm::connection connection(sqlite_config());
      (void)connection
          .raw("create table cascade_parents(id integer primary key, name text not null)")
          .execute();
      const orm::repository<cascade_parent> repository(connection);
      check_equal(repository.insert({20, "stored", {}}).affected_rows(), 1u);
      orm::session unit_of_work(connection);

      auto parent = unit_of_work.load<cascade_parent>(20);
      check_not_null(parent.get());
      parent->name = "local edit";
      parent->children.push_back({200, 20, "kept relation"});
      check_true(unit_of_work.dirty());
      check_equal(repository.update({20, "database edit", {}}).affected_rows(), 1u);

      unit_of_work.refresh(parent);
      check_equal(parent->name.c_str(), "database edit");
      check_equal(parent->children.size(), 1u);
      check_equal(parent->children.front().id, 200);
      check_false(unit_of_work.dirty());

      unit_of_work.detach(parent);
      check_equal(caught_status([&] { unit_of_work.refresh(parent); }),
                   ORM_STATUS_INVALID_STATE);
    }

    it("cascades merge refresh and detach across an explicit relation graph") {
      orm::connection connection(sqlite_config());
      (void)connection
          .raw("create table cascade_parents(id integer primary key, name text not null)")
          .execute();
      (void)connection
          .raw("create table cascade_children(id integer primary key, parent_id integer not null, "
               "value text not null)")
          .execute();
      const orm::repository<cascade_parent> parents(connection);
      const orm::repository<cascade_child> children(connection);
      check_equal(parents.insert({20, "stored parent", {}}).affected_rows(), 1u);
      check_equal(children.insert({200, 20, "stored child"}).affected_rows(), 1u);

      orm::session unit_of_work(connection);
      auto parent = unit_of_work.merge_graph(
          cascade_parent{20, "merged parent", {{200, 0, "merged child"}}},
          cascade_children);
      auto child = unit_of_work.load<cascade_child>(200);
      check_not_null(parent.get());
      check_not_null(child.get());
      check_equal(parent->children.front().parent_id, 20);
      check_equal(unit_of_work.size(), 2u);
      check_equal(unit_of_work.flush(), 2u);
      const auto merged_parent = parents.find_by_id(20);
      const auto merged_child = children.find_by_id(200);
      check_true(merged_parent.has_value());
      check_true(merged_child.has_value());
      check_equal(merged_parent->name.c_str(), "merged parent");
      check_equal(merged_child->value.c_str(), "merged child");

      check_equal(parents.update({20, "database parent", {}}).affected_rows(), 1u);
      check_equal(children.update({200, 20, "database child"}).affected_rows(), 1u);
      parent->name = "local parent";
      parent->children.front().value = "local relation child";
      child->value = "local managed child";
      unit_of_work.refresh_graph(parent, cascade_children);
      check_equal(parent->name.c_str(), "database parent");
      check_equal(parent->children.front().value.c_str(), "database child");
      check_equal(child->value.c_str(), "database child");
      check_false(unit_of_work.dirty());

      unit_of_work.detach_graph(parent, cascade_children);
      check_false(unit_of_work.contains(parent));
      check_false(unit_of_work.contains(child));
      check_equal(unit_of_work.size(), 0u);
    }

    it("restores the complete managed graph when lifecycle cascade fails") {
      orm::connection connection(sqlite_config());
      (void)connection
          .raw("create table cascade_parents(id integer primary key, name text not null)")
          .execute();
      (void)connection
          .raw("create table cascade_children(id integer primary key, parent_id integer not null, "
               "value text not null)")
          .execute();
      const orm::repository<cascade_parent> parents(connection);
      const orm::repository<cascade_child> children(connection);
      check_equal(parents.insert({20, "stored parent", {}}).affected_rows(), 1u);
      check_equal(children.insert({200, 20, "stored child"}).affected_rows(), 1u);

      orm::session bounded(connection, 1);
      auto parent = bounded.load<cascade_parent>(20);
      check_not_null(parent.get());
      parent->name = "preexisting edit";
      check_equal(
          caught_status([&] {
            (void)bounded.merge_graph(
                cascade_parent{20, "incoming", {{200, 0, "incoming child"}}},
                cascade_children);
          }),
          ORM_STATUS_LIMIT_EXCEEDED);

      check_equal(bounded.size(), 1u);
      check_true(bounded.contains(parent));
      check_equal(parent->name.c_str(), "preexisting edit");
      check_true(parent->children.empty());
      check_true(bounded.dirty());
    }

    it("keeps the snapshot dirty when a flush fails and supports discard") {
      auto connection = populated_connection();
      orm::session unit_of_work(connection);
      auto entity = unit_of_work.load<reflected_person>(1);
      check_true(entity != nullptr);
      entity->name = "will not commit";
      (void)connection.raw("drop table typed_people").execute();

      check_equal(caught_status([&] { (void)unit_of_work.flush(); }), ORM_STATUS_SQL_ERROR);
      check_true(unit_of_work.dirty());
      unit_of_work.discard();
      check_false(unit_of_work.dirty());
      check_equal(entity->name.c_str(), "Alice");
      check_equal(caught_status([&] { unit_of_work.clear(); }), ORM_STATUS_OK);
    }

    it("rejects primary-key changes that would invalidate the identity map") {
      auto connection = populated_connection();
      orm::session unit_of_work(connection);
      auto entity = unit_of_work.load<reflected_person>(1);
      check_true(entity != nullptr);
      entity->id = 99;

      check_equal(caught_status([&] { (void)unit_of_work.flush(); }),
                   ORM_STATUS_INVALID_STATE);
      check_true(unit_of_work.dirty());
      unit_of_work.discard();
      check_equal(entity->id, 1);
    }

    it("loads explicit lazy relations into the identity map on first access") {
      auto connection = populated_connection();
      (void)connection
          .raw("create table typed_children(id integer primary key, parent_id integer not null, "
               "value text not null)")
          .execute();
      (void)connection.insert("typed_children")
          .set("id", 10)
          .set("parent_id", 1)
          .set("value", "first")
          .execute();
      (void)connection.insert("typed_children")
          .set("id", 11)
          .set("parent_id", 1)
          .set("value", "second")
          .execute();

      std::optional<orm::lazy_one<reflected_child, std::int32_t>> expired;
      {
        orm::session unit_of_work(connection);
        auto one = unit_of_work.defer<reflected_child>(10);
        auto many = unit_of_work.defer_many<reflected_child>("parent_id", 1);
        check_false(one.loaded());
        check_false(many.loaded());
        check_equal(unit_of_work.size(), 0u);

        auto child = one.get();
        check_true(child != nullptr);
        check_true(one.loaded());
        check_equal(unit_of_work.size(), 1u);
        check_equal(many.size(), 2u);
        check_true(many.loaded());
        check_true(many.at(0).get() == child.get());

        child->value = "changed";
        check_equal(unit_of_work.flush(), 1u);
        expired.emplace(std::move(one));
      }

      check_equal(caught_status([&] { (void)expired->get(); }), ORM_STATUS_INVALID_STATE);
    }

    it("loads a lazy to-one relation by foreign key and rejects duplicate rows") {
      auto connection = populated_connection();
      (void)connection
          .raw("create table typed_children(id integer primary key, parent_id integer not null, "
               "value text not null)")
          .execute();
      (void)connection.insert("typed_children")
          .set("id", 10)
          .set("parent_id", 1)
          .set("value", "first")
          .execute();

      orm::session unit_of_work(connection);
      auto one = unit_of_work.defer_one<reflected_child>("parent_id", 1);
      check_false(one.loaded());
      check_equal(unit_of_work.size(), 0u);
      const auto child = one.get();
      check_true(one.loaded());
      check_not_null(child.get());
      check_equal(child->id, 10);
      check_equal(unit_of_work.size(), 1u);

      (void)connection.insert("typed_children")
          .set("id", 11)
          .set("parent_id", 2)
          .set("value", "duplicate-a")
          .execute();
      (void)connection.insert("typed_children")
          .set("id", 12)
          .set("parent_id", 2)
          .set("value", "duplicate-b")
          .execute();
      orm::session duplicate_reader(connection);
      check_equal(
          caught_status(
              [&] { (void)duplicate_reader.find_one<reflected_child>("parent_id", 2); }),
          ORM_STATUS_DATASTORE_ERROR);
      check_equal(duplicate_reader.size(), 0u);
    }

    it("prefetches missing primary keys once and preserves identity order") {
      auto connection = populated_connection();
      orm::session unit_of_work(connection);
      const std::vector<std::int32_t> ids{1, 2, 1, 99};

      const auto prefetched = unit_of_work.load_many<reflected_person>(ids);
      check_equal(prefetched.size(), 3u);
      check_equal(unit_of_work.size(), 2u);
      check_true(prefetched[0].get() == prefetched[2].get());
      check_equal(prefetched[0]->id, 1);
      check_equal(prefetched[1]->id, 2);

      auto deferred = unit_of_work.defer<reflected_person>(1);
      check_true(deferred.get().get() == prefetched[0].get());
      check_equal(
          caught_status([&] {
            (void)unit_of_work.load_many<reflected_person>(ids, 2);
          }),
          ORM_STATUS_LIMIT_EXCEEDED);
    }

    it("leaves the identity map unchanged when prefetch exceeds its capacity") {
      auto connection = populated_connection();
      orm::session unit_of_work(connection, 1);
      const std::vector<std::int32_t> ids{1, 2};

      check_equal(
          caught_status([&] { (void)unit_of_work.load_many<reflected_person>(ids); }),
          ORM_STATUS_LIMIT_EXCEEDED);
      check_equal(unit_of_work.size(), 0u);

      const auto loaded = unit_of_work.load<reflected_person>(1);
      check_true(loaded != nullptr);
      check_equal(loaded->id, 1);
      check_equal(unit_of_work.size(), 1u);
    }

    it("persists and removes an explicit relation graph in dependency order") {
      auto connection = populated_connection();
      (void)connection.raw("pragma foreign_keys = on").execute();
      (void)connection
          .raw("create table cascade_parents(id integer primary key, name text not null)")
          .execute();
      (void)connection
          .raw("create table cascade_children(id integer primary key, parent_id integer not null "
               "references cascade_parents(id), value text not null)")
          .execute();

      orm::session unit_of_work(connection);
      auto parent = unit_of_work.persist_graph(
          cascade_parent{20,
                         "root",
                         {{200, 0, "first"}, {201, 0, "second"}}},
          cascade_children);
      check_equal(unit_of_work.flush(), 3u);

      const orm::repository<cascade_parent> parents(connection);
      const orm::repository<cascade_child> children(connection);
      check_true(parents.find_by_id(20).has_value());
      const auto first_child = children.find_by_id(200);
      check_true(first_child.has_value());
      check_equal(first_child->parent_id, 20);
      check_true(children.find_by_id(201).has_value());

      parent->children.pop_back();
      check_equal(unit_of_work.flush(), 1u);
      check_false(children.find_by_id(201).has_value());

      parent->children.push_back({202, 0, "added later"});
      check_equal(unit_of_work.flush(), 1u);
      const auto added_child = children.find_by_id(202);
      check_true(added_child.has_value());
      check_equal(added_child->parent_id, 20);

      unit_of_work.remove_graph(parent, cascade_children);
      check_equal(unit_of_work.flush(), 3u);
      check_false(parents.find_by_id(20).has_value());
      check_false(children.find_by_id(200).has_value());
      check_false(children.find_by_id(202).has_value());
      check_false(children.find_by_id(201).has_value());
    }

    it("rejects duplicate identities in value and shared relation collections") {
      auto connection = populated_connection();
      (void)connection.raw("pragma foreign_keys = on").execute();
      (void)connection
          .raw("create table cascade_parents(id integer primary key, name text not null)")
          .execute();
      (void)connection
          .raw("create table cascade_children(id integer primary key, parent_id integer not null "
               "references cascade_parents(id), value text not null)")
          .execute();
      create_pointer_schema(connection);

      orm::session unit_of_work(connection);
      auto value_parent = unit_of_work.persist_graph(
          cascade_parent{21, "value", {{210, 0, "first"}}}, cascade_children);
      auto shared_parent = unit_of_work.persist_graph(
          pointer_parent{51,
                         "shared",
                         std::nullopt,
                         nullptr,
                         {std::make_shared<pointer_child>(
                             pointer_child{510, 0, "first"})}},
          shared_children_relation);
      check_equal(unit_of_work.flush(), 4u);

      value_parent->children.push_back(value_parent->children.front());
      check_equal(caught_status([&] { (void)unit_of_work.flush(); }),
                   ORM_STATUS_INVALID_STATE);
      check_true(orm::repository<cascade_child>(connection).find_by_id(210).has_value());
      unit_of_work.discard();
      check_equal(value_parent->children.size(), 1u);

      shared_parent->shared_children.push_back(
          shared_parent->shared_children.front());
      check_equal(caught_status([&] { (void)unit_of_work.flush(); }),
                   ORM_STATUS_INVALID_STATE);
      check_true(orm::repository<pointer_child>(connection).find_by_id(510).has_value());
      unit_of_work.discard();
      check_equal(shared_parent->shared_children.size(), 1u);
    }

    it("rolls back the complete persisted graph when a child violates its foreign key") {
      auto connection = populated_connection();
      (void)connection.raw("pragma foreign_keys = on").execute();
      (void)connection
          .raw("create table cascade_parents(id integer primary key, name text not null)")
          .execute();
      (void)connection
          .raw("create table cascade_children(id integer primary key, parent_id integer not null "
               "references cascade_parents(id), value text not null)")
          .execute();

      orm::session unit_of_work(connection);
      (void)unit_of_work.persist_graph(
          cascade_parent{30, "invalid", {{300, 999, "orphan"}}},
          cascade_children_unbound);
      check_equal(caught_status([&] { (void)unit_of_work.flush(); }), ORM_STATUS_SQL_ERROR);
      check_true(unit_of_work.dirty());

      const orm::repository<cascade_parent> parents(connection);
      check_false(parents.find_by_id(30).has_value());
      unit_of_work.discard();
      check_false(unit_of_work.dirty());
      check_equal(unit_of_work.size(), 0u);
    }

    it("cascades a to-one relation and enforces the configured entity limit") {
      auto connection = populated_connection();
      (void)connection.raw("pragma foreign_keys = on").execute();
      (void)connection
          .raw("create table cascade_accounts(id integer primary key, name text not null)")
          .execute();
      (void)connection
          .raw("create table cascade_profiles(id integer primary key, account_id integer not null "
               "references cascade_accounts(id), label text not null)")
          .execute();

      orm::session unit_of_work(connection);
      auto account = unit_of_work.persist_graph(
          cascade_account{40, "account", {400, 0, "profile"}}, cascade_account_profile);
      check_equal(unit_of_work.flush(), 2u);

      const orm::repository<cascade_account> accounts(connection);
      const orm::repository<cascade_profile> profiles(connection);
      check_true(accounts.find_by_id(40).has_value());
      const auto profile = profiles.find_by_id(400);
      check_true(profile.has_value());
      check_equal(profile->account_id, 40);

      unit_of_work.remove_graph(account, cascade_account_profile);
      check_equal(unit_of_work.flush(), 2u);
      check_false(accounts.find_by_id(40).has_value());
      check_false(profiles.find_by_id(400).has_value());

      orm::session bounded(connection, 1);
      check_equal(
          caught_status([&] {
            (void)bounded.persist_graph(
                cascade_account{41, "too large", {401, 41, "profile"}},
                cascade_account_profile);
          }),
          ORM_STATUS_LIMIT_EXCEEDED);
      check_false(bounded.dirty());
      check_equal(bounded.size(), 0u);
    }

    it("supports optional and shared relations with automatic foreign keys") {
      auto connection = populated_connection();
      create_pointer_schema(connection);

      orm::session unit_of_work(connection);
      auto parent = unit_of_work.persist_graph(
          pointer_parent{50,
                         "pointers",
                         pointer_child{500, 0, "optional"},
                         std::make_shared<pointer_child>(pointer_child{501, 0, "shared"}),
                         {std::make_shared<pointer_child>(pointer_child{502, 0, "many-a"}),
                          std::make_shared<pointer_child>(pointer_child{503, 0, "many-b"})}},
          optional_child_relation, shared_child_relation, shared_children_relation);
      check_equal(unit_of_work.flush(), 5u);

      const orm::repository<pointer_child> children(connection);
      for (const std::int32_t id : {500, 501, 502, 503}) {
        const auto child = children.find_by_id(id);
        check_true(child.has_value());
        check_equal(child->owner_id, 50);
      }

      auto canonical_shared = unit_of_work.load<pointer_child>(501);
      auto canonical_many = unit_of_work.load<pointer_child>(502);
      check_equal(parent->shared_child.get(), canonical_shared.get());
      check_equal(parent->shared_children.front().get(), canonical_many.get());
      parent->shared_child->value = "shared-updated";
      parent->shared_children.front()->value = "many-updated";
      check_true(unit_of_work.dirty());
      check_equal(unit_of_work.flush(), 2u);
      const auto updated_shared = children.find_by_id(501);
      const auto updated_many = children.find_by_id(502);
      check_true(updated_shared.has_value());
      check_true(updated_many.has_value());
      check_equal(updated_shared->value.c_str(), "shared-updated");
      check_equal(updated_many->value.c_str(), "many-updated");

      parent->shared_children.pop_back();
      check_equal(unit_of_work.flush(), 1u);
      check_false(children.find_by_id(503).has_value());

      parent->optional_child = pointer_child{504, 0, "optional replacement"};
      parent->shared_child = std::make_shared<pointer_child>(
          pointer_child{505, 0, "shared replacement"});
      check_equal(unit_of_work.flush(), 4u);
      check_false(children.find_by_id(500).has_value());
      check_false(children.find_by_id(501).has_value());
      check_true(children.find_by_id(504).has_value());
      check_true(children.find_by_id(505).has_value());

      auto canonical_replacement = unit_of_work.load<pointer_child>(505);
      check_equal(parent->shared_child.get(), canonical_replacement.get());
      parent->shared_child = std::make_shared<pointer_child>(*canonical_replacement);
      parent->shared_child->value = "detached-copy-update";
      check_true(unit_of_work.dirty());
      check_equal(unit_of_work.flush(), 1u);
      check_equal(parent->shared_child.get(), canonical_replacement.get());
      const auto updated_replacement = children.find_by_id(505);
      check_true(updated_replacement.has_value());
      check_equal(updated_replacement->value.c_str(), "detached-copy-update");

      parent->shared_child = std::make_shared<pointer_child>(*canonical_replacement);
      parent->shared_child->value = "relation-conflict";
      canonical_replacement->value = "canonical-conflict";
      check_equal(caught_status([&] { (void)unit_of_work.flush(); }),
                   ORM_STATUS_INVALID_STATE);
      unit_of_work.discard();
      check_false(unit_of_work.dirty());
      check_equal(parent->shared_child.get(), canonical_replacement.get());
      check_equal(parent->shared_child->value.c_str(), "detached-copy-update");

      parent->optional_child.reset();
      parent->shared_child.reset();
      check_equal(unit_of_work.flush(), 2u);
      check_false(children.find_by_id(504).has_value());
      check_false(children.find_by_id(505).has_value());

      unit_of_work.remove_graph(parent, optional_child_relation, shared_child_relation,
                                shared_children_relation);
      check_equal(unit_of_work.flush(), 2u);
      for (const std::int32_t id : {500, 501, 502, 503, 504, 505})
        check_false(children.find_by_id(id).has_value());
    }

    it("restores an uncascaded shared relation from its owned snapshot") {
      auto connection = populated_connection();
      create_pointer_schema(connection);

      orm::session unit_of_work(connection);
      auto transient_child =
          std::make_shared<pointer_child>(pointer_child{601, 60, "transient"});
      auto parent = unit_of_work.persist_graph(
          pointer_parent{60, "uncascaded", std::nullopt, transient_child, {}},
          shared_child_without_cascade);
      check_equal(unit_of_work.flush(), 1u);
      check_equal(parent->shared_child.get(), transient_child.get());

      parent->shared_child->value = "not-managed";
      check_true(unit_of_work.dirty());
      check_equal(caught_status([&] { (void)unit_of_work.flush(); }),
                   ORM_STATUS_INVALID_STATE);
      unit_of_work.discard();
      check_false(unit_of_work.dirty());
      check_equal(parent->shared_child.get(), transient_child.get());
      check_equal(parent->shared_child->value.c_str(), "transient");
      const orm::repository<pointer_child> children(connection);
      check_false(children.find_by_id(601).has_value());

      parent->shared_child =
          std::make_shared<pointer_child>(pointer_child{602, 0, "replacement"});
      check_equal(caught_status([&] { (void)unit_of_work.flush(); }),
                   ORM_STATUS_INVALID_STATE);
      unit_of_work.discard();
      check_equal(parent->shared_child.get(), transient_child.get());

      check_equal(children.insert({603, 60, "managed replacement"})
                        .affected_rows(),
                    1u);
      auto managed_replacement = unit_of_work.load<pointer_child>(603);
      check_not_null(managed_replacement.get());
      parent->shared_child =
          std::make_shared<pointer_child>(*managed_replacement);
      check_equal(unit_of_work.flush(), 0u);
      check_equal(parent->shared_child.get(), managed_replacement.get());
    }

    it("rejects an unmanaged replacement before orphan removal") {
      auto connection = populated_connection();
      create_pointer_schema(connection);
      const orm::repository<pointer_parent> parents(connection);
      const orm::repository<pointer_child> children(connection);
      check_equal(
          parents.insert({61, "orphan owner", std::nullopt, nullptr, {}})
              .affected_rows(),
          1u);
      check_equal(children.insert({610, 61, "stored child"}).affected_rows(),
                    1u);

      orm::session unit_of_work(connection);
      auto managed_child = unit_of_work.load<pointer_child>(610);
      check_not_null(managed_child.get());
      auto parent = unit_of_work.merge_graph(
          pointer_parent{61,
                         "orphan owner",
                         std::nullopt,
                         std::make_shared<pointer_child>(*managed_child),
                         {}},
          shared_child_orphan_without_persist);
      check_equal(unit_of_work.flush(), 0u);
      check_equal(parent->shared_child.get(), managed_child.get());

      parent->shared_child =
          std::make_shared<pointer_child>(pointer_child{611, 0, "unmanaged"});
      check_equal(caught_status([&] { (void)unit_of_work.flush(); }),
                   ORM_STATUS_INVALID_STATE);
      check_true(children.find_by_id(610).has_value());
      check_false(children.find_by_id(611).has_value());
      unit_of_work.discard();
      check_equal(parent->shared_child.get(), managed_child.get());
    }
  }

  group("mapping errors") {
    it("rejects a projection whose column count differs from the reflected type") {
      auto connection = populated_connection();
      auto query = connection.select("typed_people");
      query.column("id");
      check_equal(caught_status([&] { (void)query.fetch<reflected_person>(); }),
                   ORM_STATUS_TYPE_ERROR);
    }

    it("rejects NULL for a non-optional field") {
      auto connection = populated_connection();
      auto query = connection.select("typed_people");
      query.column("note").where("id", orm::comparison::equal, 1);
      check_equal(caught_status([&] { (void)query.fetch<required_note>(); }),
                   ORM_STATUS_NULL_VALUE);
    }

    it("rejects integers outside the reflected destination range") {
      auto connection = populated_connection();
      auto query = connection.raw("select 300");
      check_equal(caught_status([&] { (void)query.fetch<narrow_id>(); }),
                   ORM_STATUS_OUT_OF_RANGE);
    }

    it("rejects text that cannot be NUL-terminated in a fixed array") {
      auto connection = populated_connection();
      auto query = connection.raw("select 'toolong'");
      check_equal(caught_status([&] { (void)query.fetch<short_code>(); }),
                   ORM_STATUS_LIMIT_EXCEEDED);
    }
  }
}
