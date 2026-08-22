#include "jpa.hpp"

#include <tinytest.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

  void require(bool condition, const char *message) {
    check(condition, message);
  }

  orm::config sqlite_config() {
    orm::config configuration("sqlite");
    configuration.option("filename", ":memory:")
        .option("open_mode", "read_write_create");
    return configuration;
  }

  struct person {
    std::int64_t id = 0;
    std::string name;
  };

  ORM_MODEL_WITH_NAME(person, "jpa_people", id, name)

  void test_jpa_repository_bridge() {
    orm::connection connection(sqlite_config());
    (void)connection.raw("create table jpa_people(id integer primary key, name text not null)")
             .execute();

    orm::jpa::repository<person> repository(connection);
    require(repository.count() == 0, "JPA facade reported non-empty table");

    auto inserted = repository.save({1, "Alice"});
    require(inserted.affected_rows() == 1, "JPA repository.save failed");
    auto upserted = repository.upsert({1, "Alice Updated"});
    require(upserted.affected_rows() == 1,
            "JPA repository upsert path should update existing rows");

    const auto by_id = repository.findById(1);
    require(by_id.has_value(), "JPA repository.findById returned no row");
    require(by_id->name == "Alice Updated", "JPA repository.save used insert instead of update");

    require(repository.save({2, "Bob"}).affected_rows() == 1,
            "JPA repository.save should insert row 2");
    require(repository.save({3, "Cora"}).affected_rows() == 1,
            "JPA repository.save should insert row 3");
    require(repository.count() == 3, "JPA repository.count mismatch");
    require(repository.existsById(2), "JPA repository.existsById returned false");
    const auto by_id_alias = repository.getById(std::int64_t{3});
    require(by_id_alias.has_value() && by_id_alias->name == "Cora",
            "JPA repository.getById should resolve existing row");

    auto batch = repository.saveAll(std::vector<person>{
        {4, "Dale"},
        {5, "Eve"},
    });
    require(batch.size() == 2 && batch[0].affected_rows() == 1 &&
                batch[1].affected_rows() == 1,
            "JPA repository.saveAll should insert all rows");
    require(repository.count() == 5, "JPA repository.saveAll count mismatch");

    auto batch2 = repository.saveAllAndFlush(std::vector<person>{
        {6, "Fay"},
        {7, "Gus"},
    });
    require(batch2.size() == 2 && batch2[0].affected_rows() == 1 &&
                batch2[1].affected_rows() == 1,
            "JPA repository.saveAllAndFlush should insert all rows");
    require(repository.count() == 7, "JPA repository.saveAllAndFlush count mismatch");

    repository.deleteAll(repository.findAllById(std::vector<std::int64_t>{6, 7}));
    require(repository.count() == 5,
            "JPA repository.deleteAll(entity_vector) should remove provided rows");

    const auto batch_deleted = repository.deleteAllById(std::vector<std::int64_t>{4, 5, 999});
    require(batch_deleted.size() == 3, "JPA repository.deleteAllById should accept id collection");
    require(batch_deleted[0].affected_rows() == 1 && batch_deleted[1].affected_rows() == 1 &&
                batch_deleted[2].affected_rows() == 0,
            "JPA repository.deleteAllById should report deletion outcome for missing ids");
    require(repository.count() == 3,
            "JPA repository.deleteAllById should remove only existing rows");

    orm::jpa::specification<person> name_spec(
        [](orm::select_query &query) {
          query.where("name", orm::comparison::equal, "Bob");
        });
    const auto by_name = repository.findOne(name_spec);
    require(by_name.has_value() && by_name->id == 2, "JPA repository.findOne(spec) failed");
    require(repository.count(name_spec) == 1, "JPA repository.count(spec) returned wrong value");

    const auto found = repository.findAllById(std::vector<std::int64_t>{3, 1, 99});
    require(found.size() == 2, "JPA repository.findAllById should skip missing ids");

    require(repository.deleteById(std::int64_t{2}).affected_rows() == 1,
            "JPA repository.deleteById deleted no row");
    require(!repository.existsById(2), "JPA repository delete should remove row");

    orm::jpa::specification<person> keep_prefix(
        [](orm::select_query &query) {
          query.where("name", orm::comparison::like, "A%");
        });
    repository.delete_(keep_prefix);
    require(repository.count() == 1, "JPA repository.delete(spec) removed too many rows");

    const auto all_deleted = repository.deleteAll();
    require(all_deleted.size() == 1, "JPA repository.deleteAll should remove all rows");
    require(repository.count() == 0, "JPA repository.deleteAll() should remove every row");
  }

  void test_jpa_entity_manager_bridge() {
    orm::connection connection(sqlite_config());
    (void)connection.raw("create table jpa_people(id integer primary key, name text not null)")
        .execute();
    orm::jpa::entity_manager manager(connection);

    auto first = manager.persist(person{10, "Ann"});
    require(first != nullptr, "JPA entity_manager.persist should return managed entity");
    require(manager.state(first) == orm::entity_state::added,
            "JPA entity_manager.persist should mark entity as added");
    require(manager.contains(first), "JPA entity_manager.contains should recognize persisted entity");
    require(manager.dirty(), "JPA entity_manager should track pending persist as dirty");
    manager.flush();
    require(!manager.dirty(), "JPA entity_manager.flush should clear dirty state");

    auto reference = manager.getReference<person>(10);
    require(reference != nullptr, "JPA entity_manager.getReference should return entity");
    manager.detach(reference);
    require(!manager.contains(reference),
            "JPA entity_manager.detach should remove entity from context");

    auto loaded = manager.find<person>(10);
    require(loaded != nullptr && loaded->name == "Ann", "JPA entity_manager.find failed");
    require(manager.state(loaded) == orm::entity_state::managed,
            "JPA entity_manager.state should report managed entity");
    manager.refresh<person>(loaded);

    auto tx_result =
        manager.transactional([](orm::jpa::entity_manager &em) -> std::string {
          auto found = em.find<person>(10);
          return found != nullptr ? found->name : std::string{};
        });
    require(tx_result == "Ann", "JPA entity_manager.transactional callback did not run");

    auto copy = manager.merge(*loaded);
    require(copy != nullptr && copy->id == loaded->id, "JPA entity_manager.merge failed");
    manager.remove(copy);
    manager.flush();
    require(!manager.find<person>(10), "JPA entity_manager.remove + flush should delete row");

    manager.remove<person>(std::int64_t{10});
    manager.flush();
    require(!manager.find<person>(10), "JPA entity_manager.remove(id) should be safe when entity is missing");
    manager.clear();
    require(manager.transaction_active() == false, "JPA entity_manager should report no active transaction");
  }

} // namespace

suite("ORM JPA facade") {
  it("bridges repository-style operations to C++ typed DSL") {
    test_jpa_repository_bridge();
  }

  it("bridges entity-manager operations to session semantics") {
    test_jpa_entity_manager_bridge();
  }
}
