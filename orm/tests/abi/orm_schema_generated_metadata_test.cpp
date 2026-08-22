#include "orm.hpp"

#include <tinytest.hpp>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace app::model {

std::vector<std::string> lifecycle_events;
bool fail_after_update = false;
std::uint64_t fail_after_load_id = 0;

enum class ProfileState : std::uint8_t { inactive = 0, active = 1 };

struct Order {
  std::uint64_t id;
  std::uint64_t user_id;
  std::string label;
};

struct User {
  std::uint64_t id;
  std::uint64_t version;
  Order order;
  std::string name;
  std::vector<Order> orders;
};

struct Profile {
  std::uint64_t id;
  ProfileState state;
  std::optional<std::string> nickname;
};

struct LifecycleEntity {
  std::uint64_t id;
  std::uint64_t version;
  std::string name;
  bool loaded = false;

  void before_create() {
    lifecycle_events.push_back("pre_persist");
    name += ":pre_persist";
  }
  void after_create() { lifecycle_events.push_back("post_persist"); }
  void before_update() {
    lifecycle_events.push_back("pre_update");
    name += ":pre_update";
  }
  void after_update() {
    lifecycle_events.push_back("post_update");
    if (fail_after_update) throw std::runtime_error("post_update rejected");
  }
  void before_remove() { lifecycle_events.push_back("pre_remove"); }
  void after_remove() { lifecycle_events.push_back("post_remove"); }
  void after_load() {
    lifecycle_events.push_back("post_load");
    if (id == fail_after_load_id) throw std::runtime_error("post_load rejected");
    loaded = true;
  }
};

struct OrderKey {
  std::uint64_t tenant;
  std::uint64_t number;
};

struct CompositeOrder {
  OrderKey key;
  std::uint64_t version;
  std::string label;
};

struct Address {
  std::string city;
  std::string postal_code;
};

struct Customer {
  std::uint64_t id;
  Address address;
  std::string name;
};

struct DirectCompositeEntity {
  std::uint64_t region;
  std::uint64_t local;
  std::string value;
};

struct Payment {
  std::uint64_t id = 0;
  std::string label;
};

struct CardPayment : Payment {
  std::string card_last4;
};

struct BankPayment : Payment {
  std::string bank_reference;
};

struct Asset {
  std::uint64_t id = 0;
  std::string label;
};

struct ImageAsset : Asset {
  std::uint32_t width = 0;
};

struct VideoAsset : Asset {
  std::uint32_t duration_seconds = 0;
};

struct Document {
  std::uint64_t id = 0;
  std::uint64_t version = 0;
  std::string title;
};

struct PdfDocument : Document {
  std::uint32_t page_count = 0;
};

}  // namespace app::model

#include "validator_fixture.orm.hpp"

static_assert(orm::model::has_relations_v<app::model::User>,
              "generated entity models must expose relation metadata");
static_assert(orm::model::has_inheritance_hierarchy_v<app::model::Payment>,
              "generated roots must expose their static inheritance hierarchy");

namespace {

orm::config sqlite_config() {
  orm::config configuration("sqlite");
  configuration.option("filename", ":memory:").option("open_mode", "read_write_create");
  return configuration;
}

}  // namespace

suite("ORM Generated Metadata") {
  it("maps one table hierarchy without slicing polymorphic results") {
    orm::connection connection(sqlite_config());
    (void)connection
        .raw("create table payments(id integer primary key, payment_kind text not null, "
             "label text not null, card_last4 text null, bank_reference text null)")
        .execute();

    app::model::Payment base;
    base.id = 1;
    base.label = "base";
    app::model::CardPayment card;
    card.id = 2;
    card.label = "card";
    card.card_last4 = "4242";
    app::model::BankPayment bank;
    bank.id = 3;
    bank.label = "bank";
    bank.bank_reference = "wire-7";

    orm::repository<app::model::Payment> bases(connection);
    orm::repository<app::model::CardPayment> cards(connection);
    orm::repository<app::model::BankPayment> banks(connection);
    check_equal(bases.insert(base).affected_rows(), 1u);
    check_equal(cards.insert(card).affected_rows(), 1u);
    check_equal(banks.insert(bank).affected_rows(), 1u);

    check_true(bases.find_by_id(1u).has_value());
    check_false(bases.find_by_id(2u).has_value());
    check_false(cards.find_by_id(1u).has_value());
    const auto stored_card = cards.find_by_id(2u);
    check_true(stored_card.has_value());
    check_equal(stored_card->label.c_str(), "card");
    check_equal(stored_card->card_last4.c_str(), "4242");

    orm::polymorphic_repository<app::model::Payment> payments(connection);
    const auto polymorphic_card = payments.find_by_id(2u);
    check_true(polymorphic_card.has_value());
    check_true(std::holds_alternative<app::model::CardPayment>(
        *polymorphic_card));
    check_equal(
        std::get<app::model::CardPayment>(*polymorphic_card).card_last4.c_str(),
        "4242");
    const auto all = payments.find_all();
    check_equal(all.size(), 3u);

    auto changed = *polymorphic_card;
    std::get<app::model::CardPayment>(changed).label = "changed";
    check_equal(payments.update(changed).affected_rows(), 1u);
    const auto updated_card = cards.find_by_id(2u);
    check_true(updated_card.has_value());
    check_equal(updated_card->label.c_str(), "changed");
    check_equal(payments.remove(changed).affected_rows(), 1u);
    check_false(cards.find_by_id(2u).has_value());
    check_true(banks.find_by_id(3u).has_value());

    (void)connection
        .raw("insert into payments(id, payment_kind, label) "
             "values(4, 'future_type', 'unknown')")
        .execute();
    bool unknown_rejected = false;
    try {
      (void)payments.find_all();
    } catch (const orm::status_error& error) {
      unknown_rejected = error.status() == ORM_STATUS_DATASTORE_ERROR;
    }
    check_true(unknown_rejected);
  }

  it("maps each concrete class to a complete independent table") {
    orm::connection connection(sqlite_config());
    (void)connection
        .raw("create table assets(id integer primary key, label text not null)")
        .execute();
    (void)connection
        .raw("create table images(id integer primary key, label text not null, "
             "width integer not null)")
        .execute();
    (void)connection
        .raw("create table videos(id integer primary key, label text not null, "
             "duration_seconds integer not null)")
        .execute();

    app::model::Asset asset;
    asset.id = 1;
    asset.label = "asset";
    app::model::ImageAsset image;
    image.id = 2;
    image.label = "image";
    image.width = 1920;
    app::model::VideoAsset video;
    video.id = 3;
    video.label = "video";
    video.duration_seconds = 60;

    orm::repository<app::model::Asset> assets(connection);
    orm::repository<app::model::ImageAsset> images(connection);
    orm::repository<app::model::VideoAsset> videos(connection);
    check_equal(assets.insert(asset).affected_rows(), 1u);
    check_equal(images.insert(image).affected_rows(), 1u);
    check_equal(videos.insert(video).affected_rows(), 1u);
    check_false(assets.find_by_id(2u).has_value());
    const auto stored_image = images.find_by_id(2u);
    check_true(stored_image.has_value());
    check_equal(stored_image->width, 1920u);

    orm::polymorphic_repository<app::model::Asset> hierarchy(connection);
    const auto found_video = hierarchy.find_by_id(3u);
    check_true(found_video.has_value());
    check_true(std::holds_alternative<app::model::VideoAsset>(*found_video));
    check_equal(hierarchy.find_all().size(), 3u);

    app::model::ImageAsset duplicate;
    duplicate.id = 1;
    duplicate.label = "duplicate";
    duplicate.width = 640;
    check_equal(images.insert(duplicate).affected_rows(), 1u);
    bool duplicate_rejected = false;
    try {
      (void)hierarchy.find_by_id(1u);
    } catch (const orm::status_error& error) {
      duplicate_rejected = error.status() == ORM_STATUS_DATASTORE_ERROR;
    }
    check_true(duplicate_rejected);
  }

  it("writes and reads joined inheritance fragments atomically") {
    orm::connection connection(sqlite_config());
    (void)connection.raw("pragma foreign_keys = on").execute();
    (void)connection
        .raw("create table documents(id integer primary key, version integer not null, "
             "document_kind text not null, title text not null)")
        .execute();
    (void)connection
        .raw("create table pdf_documents(id integer primary key references documents(id), "
             "page_count integer not null)")
        .execute();

    app::model::Document document;
    document.id = 1;
    document.title = "plain";
    app::model::PdfDocument pdf;
    pdf.id = 2;
    pdf.title = "specification";
    pdf.page_count = 12;

    orm::repository<app::model::Document> documents(connection);
    orm::repository<app::model::PdfDocument> pdfs(connection);
    check_equal(documents.insert(document).affected_rows(), 1u);
    check_equal(pdfs.insert(pdf).affected_rows(), 1u);
    check_false(documents.find_by_id(2u).has_value());
    auto stored = pdfs.find_by_id(2u);
    check_true(stored.has_value());
    check_equal(stored->title.c_str(), "specification");
    check_equal(stored->page_count, 12u);

    stored->title = "updated";
    stored->page_count = 14;
    check_equal(pdfs.update(*stored).affected_rows(), 1u);
    check_equal(stored->version, 1u);
    const auto updated = pdfs.find_by_id(2u);
    check_true(updated.has_value());
    check_equal(updated->title.c_str(), "updated");
    check_equal(updated->page_count, 14u);

    orm::polymorphic_repository<app::model::Document> hierarchy(connection);
    const auto polymorphic = hierarchy.find_by_id(2u);
    check_true(polymorphic.has_value());
    check_true(std::holds_alternative<app::model::PdfDocument>(*polymorphic));
    check_equal(hierarchy.find_all().size(), 2u);
    check_equal(pdfs.delete_by_id(2u).affected_rows(), 1u);
    check_false(pdfs.find_by_id(2u).has_value());
    check_true(documents.find_by_id(1u).has_value());
  }

  it("uses an embedded composite id across repository and entity manager") {
    orm::connection connection(sqlite_config());
    (void)connection
        .raw("create table composite_orders(tenant_id integer not null, "
             "order_number integer not null, version integer not null, "
             "label text not null, primary key(tenant_id, order_number))")
        .execute();
    using order_model = orm::model::entity_model<app::model::CompositeOrder>;
    constexpr auto primary_keys =
        orm::model::get_primary_keys<app::model::CompositeOrder>();
    static_assert(primary_keys.size() == 2,
                  "embedded ids must expose every flattened key column");
    check_equal(primary_keys[0].data(), "tenant_id");
    check_equal(primary_keys[1].data(), "order_number");

    orm::repository<app::model::CompositeOrder> repository(connection);
    app::model::CompositeOrder order{{7, 42}, 0, "created"};
    const auto generated_id = order_model::id(order);
    check_equal(std::get<0>(generated_id), 7u);
    check_equal(std::get<1>(generated_id), 42u);
    check_equal(repository.insert(order).affected_rows(), 1u);
    auto found = repository.find_by_id(app::model::OrderKey{7, 42});
    check_true(found.has_value());
    check_equal(found->label.c_str(), "created");

    found->label = "updated";
    check_equal(repository.update(*found).affected_rows(), 1u);
    check_equal(found->version, 1u);
    const auto by_tuple = repository.find_by_id(std::make_tuple(7u, 42u));
    check_true(by_tuple.has_value());
    check_equal(by_tuple->label.c_str(), "updated");
    check_equal(repository.insert({{7, 43}, 0, "second"}).affected_rows(),
                  1u);

    orm::entity_manager manager(connection);
    const std::vector<app::model::OrderKey> ids = {{7, 42}, {7, 43}};
    auto loaded = manager.load_many<app::model::CompositeOrder>(ids);
    check_equal(loaded.size(), 2u);
    auto first = loaded.front();
    auto same = manager.find<app::model::CompositeOrder>(
        std::make_tuple(7u, 42u));
    check_not_null(first.get());
    check(first == same);
    first->label = "managed";
    check_equal(manager.flush(), 1u);
    check_equal(first->version, 2u);

    first->key.number = 99;
    bool key_change_rejected = false;
    try {
      (void)manager.flush();
    } catch (const orm::status_error& error) {
      key_change_rejected = error.status() == ORM_STATUS_INVALID_STATE;
    }
    check_true(key_change_rejected);
    manager.discard();
    check_equal(first->key.number, 42u);

    manager.remove(first);
    check_equal(manager.flush(), 1u);
    check_false(repository.find_by_id(app::model::OrderKey{7, 42})
                    .has_value());
    check_true(repository.find_by_id(app::model::OrderKey{7, 43})
                   .has_value());
  }

  it("flattens embedded values and supports direct composite ids") {
    orm::connection connection(sqlite_config());
    (void)connection
        .raw("create table customers(id integer primary key, city_name text not null, "
             "postal_code text not null, name text not null)")
        .execute();
    (void)connection
        .raw("create table direct_composite_entities(region_id integer not null, "
             "local_id integer not null, value text not null, "
             "primary key(region_id, local_id))")
        .execute();

    orm::repository<app::model::Customer> customers(connection);
    check_equal(
        customers.insert({1, {"Shanghai", "200000"}, "Alice"})
            .affected_rows(),
        1u);
    auto customer = customers.find_by_id(1u);
    check_true(customer.has_value());
    check_equal(customer->address.city.c_str(), "Shanghai");
    customer->address.postal_code = "200001";
    check_equal(customers.update(*customer).affected_rows(), 1u);
    const auto updated = customers.find_by_id(1u);
    check_true(updated.has_value());
    check_equal(updated->address.postal_code.c_str(), "200001");

    orm::repository<app::model::DirectCompositeEntity> direct(connection);
    check_equal(direct.insert({3, 9, "value"}).affected_rows(), 1u);
    const auto direct_found = direct.find_by_id(std::make_tuple(3u, 9u));
    check_true(direct_found.has_value());
    check_equal(direct_found->value.c_str(), "value");
    check_equal(direct.delete_by_id(std::make_tuple(3u, 9u)).affected_rows(),
                  1u);
  }

  it("runs generated lifecycle callbacks at unit-of-work boundaries") {
    app::model::lifecycle_events.clear();
    app::model::fail_after_update = false;
    app::model::fail_after_load_id = 0;
    orm::connection connection(sqlite_config());
    (void)connection
        .raw("create table lifecycle_entities(id integer primary key, version integer "
             "not null, name text not null)")
        .execute();

    orm::entity_manager manager(connection);
    auto entity = manager.persist(app::model::LifecycleEntity{1, 0, "new"});
    check_equal(manager.flush(), 1u);
    check_equal(entity->name.c_str(), "new:pre_persist");
    check_equal(app::model::lifecycle_events.size(), 2u);
    check_equal(app::model::lifecycle_events[0].c_str(), "pre_persist");
    check_equal(app::model::lifecycle_events[1].c_str(), "post_persist");

    entity->name = "changed";
    check_equal(manager.flush(), 1u);
    check_equal(entity->version, 1u);
    check_equal(entity->name.c_str(), "changed:pre_update");

    app::model::lifecycle_events.clear();
    orm::entity_manager reader(connection);
    auto loaded = reader.find<app::model::LifecycleEntity>(1u);
    check_not_null(loaded.get());
    check_true(loaded->loaded);
    check_equal(app::model::lifecycle_events.size(), 1u);
    check_equal(app::model::lifecycle_events.front().c_str(), "post_load");
    check(reader.find<app::model::LifecycleEntity>(1u) == loaded);
    check_equal(app::model::lifecycle_events.size(), 1u);

    (void)connection.raw("update lifecycle_entities set name = 'database'").execute();
    reader.refresh(loaded);
    check_equal(loaded->name.c_str(), "database");
    check_equal(app::model::lifecycle_events.size(), 2u);
    check_equal(app::model::lifecycle_events.back().c_str(), "post_load");

    app::model::lifecycle_events.clear();
    reader.remove(loaded);
    check_equal(reader.flush(), 1u);
    check_equal(app::model::lifecycle_events.size(), 2u);
    check_equal(app::model::lifecycle_events[0].c_str(), "pre_remove");
    check_equal(app::model::lifecycle_events[1].c_str(), "post_remove");
  }

  it("rolls back SQL when a post lifecycle callback throws") {
    app::model::lifecycle_events.clear();
    app::model::fail_after_update = false;
    app::model::fail_after_load_id = 0;
    orm::connection connection(sqlite_config());
    (void)connection
        .raw("create table lifecycle_entities(id integer primary key, version integer "
             "not null, name text not null)")
        .execute();

    orm::entity_manager manager(connection);
    auto entity = manager.persist(app::model::LifecycleEntity{2, 0, "stored"});
    check_equal(manager.flush(), 1u);
    entity->name = "rejected";
    app::model::fail_after_update = true;
    bool rejected = false;
    try {
      (void)manager.flush();
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    app::model::fail_after_update = false;

    check_true(rejected);
    check_true(manager.dirty());
    check_equal(entity->version, 0u);
    const orm::repository<app::model::LifecycleEntity> repository(connection);
    const auto stored = repository.find_by_id(2u);
    check_true(stored.has_value());
    check_equal(stored->name.c_str(), "stored:pre_persist");

    manager.discard();
    check_false(manager.dirty());
    check_equal(entity->name.c_str(), "stored:pre_persist");
  }

  it("rolls back batched materialization when post_load throws") {
    app::model::lifecycle_events.clear();
    app::model::fail_after_load_id = 0;
    orm::connection connection(sqlite_config());
    (void)connection
        .raw("create table lifecycle_entities(id integer primary key, version integer "
             "not null, name text not null)")
        .execute();
    const orm::repository<app::model::LifecycleEntity> repository(connection);
    check_equal(repository.insert({3, 0, "first"}).affected_rows(), 1u);
    check_equal(repository.insert({4, 0, "second"}).affected_rows(), 1u);

    orm::entity_manager manager(connection);
    app::model::fail_after_load_id = 4;
    bool rejected = false;
    try {
      (void)manager.load_many<app::model::LifecycleEntity, std::uint64_t>({3, 4});
    } catch (const std::runtime_error&) {
      rejected = true;
    }
    app::model::fail_after_load_id = 0;

    check_true(rejected);
    check_equal(manager.size(), 0u);
    check_false(manager.dirty());
  }

  it("round-trips generated optional and enum mappings") {
    orm::connection connection(sqlite_config());
    (void)connection
        .raw("create table profiles(id integer primary key, state integer not null, "
             "nickname text null)")
        .execute();
    const orm::repository<app::model::Profile> repository(connection);

    check_equal(repository.insert({1, app::model::ProfileState::active, std::nullopt})
                      .affected_rows(),
                  1u);
    check_equal(repository
                      .insert({2, app::model::ProfileState::inactive,
                               std::string("guest")})
                      .affected_rows(),
                  1u);

    const auto missing_name = repository.find_by_id(1);
    check_true(missing_name.has_value());
    check(missing_name->state == app::model::ProfileState::active);
    check_false(missing_name->nickname.has_value());

    const auto named = repository.find_by_id(2);
    check_true(named.has_value());
    check(named->state == app::model::ProfileState::inactive);
    check_true(named->nickname.has_value());
    check_equal(named->nickname->c_str(), "guest");
  }

  it("maps schema columns and custom primary keys onto an existing struct") {
    const std::string table_name(orm::model::get_name<app::model::User>());
    const std::string primary_key(orm::model::get_primary_key<app::model::User>());
    const std::string version(orm::model::get_version<app::model::User>());
    const std::string first_column(orm::model::get_name<app::model::User>(0));
    const std::string second_column(orm::model::get_name<app::model::User>(1));
    const std::string third_column(orm::model::get_name<app::model::User>(2));
    check_equal(table_name.c_str(), "users");
    check_equal(primary_key.c_str(), "user_id");
    check_equal(version.c_str(), "entity_version");
    check_equal(orm::model::get_array<app::model::User>().size(), 3u);
    check_equal(first_column.c_str(), "user_id");
    check_equal(second_column.c_str(), "entity_version");
    check_equal(third_column.c_str(), "display_name");

    orm::connection connection(sqlite_config());
    (void)connection
        .raw("create table users(user_id integer primary key, entity_version integer not null, "
             "display_name text not null)")
        .execute();
    const orm::repository<app::model::User> repository(connection);

    check_equal(repository.insert({7, 0, {}, "Alice", {}}).affected_rows(), 1u);
    const auto found = repository.find_by_id(7);
    check_true(found.has_value());
    check_equal(found->name.c_str(), "Alice");

    app::model::User updated{7, 0, {}, "Alice Cooper", {}};
    check_equal(repository.update(updated).affected_rows(), 1u);
    check_equal(updated.version, 1u);
    const auto changed = repository.find_by_id(7);
    check_true(changed.has_value());
    check_equal(changed->name.c_str(), "Alice Cooper");
  }

  it("generates a mapped relation descriptor for graph persistence") {
    orm::connection connection(sqlite_config());
    (void)connection.raw("pragma foreign_keys = on").execute();
    (void)connection
        .raw("create table users(user_id integer primary key, entity_version integer not null, "
             "display_name text not null)")
        .execute();
    (void)connection
        .raw("create table orders(id integer primary key, owner_id integer not null "
             "references users(user_id), item_label text not null)")
        .execute();

    orm::entity_manager unit_of_work(connection);
    using user_model = orm::model::entity_model<app::model::User>;
    auto user = unit_of_work.persist(
        app::model::User{8, 0, {80, 0, "book"}, "Bob", {}});
    check_equal(unit_of_work.flush(), 2u);

    const orm::repository<app::model::Order> orders(connection);
    const auto order = orders.find_by_id(80);
    check_true(order.has_value());
    check_equal(order->user_id, 8u);

    orm::entity_manager reader(connection);
    const auto found_user = reader.find<app::model::User>(8u);
    check_not_null(found_user.get());
    check_equal(found_user->name.c_str(), "Bob");
    check_equal(found_user->order.id, 0u);
    check_equal(found_user->orders.size(), 1u);
    check_equal(found_user->orders.front().id, 80u);
    auto fetched = user_model::fetch_order(
        reader, app::model::User{8, 0, {}, "", {}});
    check_false(fetched.loaded());
    const auto fetched_order = fetched.get();
    check_true(fetched.loaded());
    check_not_null(fetched_order.get());
    check_equal(fetched_order->id, 80u);

    user_model::initialize_order(reader, found_user);
    check_equal(found_user->order.id, 80u);
    user_model::initialize_order(reader, found_user);
    check_equal(reader.size(), 2u);
    constexpr auto complete_graph =
        user_model::order_graph() | user_model::orders_graph();
    static_assert(complete_graph.mask() == 3u,
                  "generated entity graphs must compose at compile time");
    static_assert(
        std::is_same_v<decltype(user_model::order_graph()),
                       orm::model::entity_graph<app::model::User>>,
        "generated entity graphs must retain their owner type");

    orm::entity_manager graph_reader(connection);
    const auto graph_user = graph_reader.find<app::model::User>(
        8u, user_model::order_graph());
    check_not_null(graph_user.get());
    check_equal(graph_user->order.id, 80u);
    check_equal(graph_user->orders.size(), 1u);

    unit_of_work.remove(user);
    check_equal(unit_of_work.flush(), 2u);
    check_false(orders.find_by_id(80).has_value());
  }

  it("tracks value relation updates and orphan removal") {
    orm::connection connection(sqlite_config());
    (void)connection.raw("pragma foreign_keys = on").execute();
    (void)connection
        .raw("create table users(user_id integer primary key, entity_version integer not null, "
             "display_name text not null)")
        .execute();
    (void)connection
        .raw("create table orders(id integer primary key, owner_id integer not null "
             "references users(user_id), item_label text not null)")
        .execute();

    orm::entity_manager unit_of_work(connection);
    using user_model = orm::model::entity_model<app::model::User>;
    auto user = unit_of_work.persist(
        app::model::User{9,
                         0,
                         {89, 0, "required"},
                         "Cara",
                         {{90, 0, "first"}, {91, 0, "second"}}});
    check_equal(unit_of_work.flush(), 4u);

    const orm::repository<app::model::Order> orders(connection);
    const auto first = orders.find_by_id(90);
    check_true(first.has_value());
    check_equal(first->user_id, 9u);

    user->order.label = "required-updated";
    user->orders.front().label = "first-updated";
    check_true(unit_of_work.dirty());
    bool clear_rejected = false;
    try {
      unit_of_work.clear();
    } catch (const orm::status_error &error) {
      clear_rejected = error.status() == ORM_STATUS_INVALID_STATE;
    }
    check_true(clear_rejected);
    check_equal(unit_of_work.flush(), 2u);
    check_false(unit_of_work.dirty());
    const auto updated_required = orders.find_by_id(89);
    const auto updated_first = orders.find_by_id(90);
    check_true(updated_required.has_value());
    check_true(updated_first.has_value());
    check_equal(updated_required->label.c_str(), "required-updated");
    check_equal(updated_first->label.c_str(), "first-updated");

    auto canonical_first = unit_of_work.load<app::model::Order>(90u);
    check_not_null(canonical_first.get());
    canonical_first->label = "canonical-change";
    user->orders.front().label = "relation-copy-change";
    bool conflict_rejected = false;
    try {
      (void)unit_of_work.flush();
    } catch (const orm::status_error &error) {
      conflict_rejected = error.status() == ORM_STATUS_INVALID_STATE;
    }
    check_true(conflict_rejected);
    unit_of_work.discard();
    check_false(unit_of_work.dirty());
    check_equal(canonical_first->label.c_str(), "first-updated");
    check_equal(user->orders.front().label.c_str(), "first-updated");

    orm::entity_manager reader(connection);
    const auto fetched = user_model::fetch_orders(
        reader, app::model::User{9, 0, {}, "", {}});
    check_equal(fetched.size(), 3u);
    check_equal(reader.size(), 3u);

    user->orders.pop_back();
    check_equal(unit_of_work.flush(), 1u);
    check_false(orders.find_by_id(91).has_value());

    unit_of_work.remove(user);
    check_equal(unit_of_work.flush(), 3u);
    check_false(orders.find_by_id(89).has_value());
    check_false(orders.find_by_id(90).has_value());
  }

  it("applies generated cascade metadata to merge refresh and detach") {
    orm::connection connection(sqlite_config());
    (void)connection.raw("pragma foreign_keys = on").execute();
    (void)connection
        .raw("create table users(user_id integer primary key, entity_version integer not null, "
             "display_name text not null)")
        .execute();
    (void)connection
        .raw("create table orders(id integer primary key, owner_id integer not null "
             "references users(user_id), item_label text not null)")
        .execute();

    orm::repository<app::model::User> users(connection);
    orm::repository<app::model::Order> orders(connection);
    check_equal(users.insert({10, 0, {}, "stored", {}}).affected_rows(), 1u);
    check_equal(orders.insert({100, 10, "stored-one"}).affected_rows(), 1u);
    check_equal(orders.insert({101, 10, "stored-many"}).affected_rows(), 1u);

    orm::entity_manager manager(connection);
    auto user = manager.merge(app::model::User{
        10, 0, {100, 0, "merged-one"}, "merged", {{101, 0, "merged-many"}}});
    check_equal(manager.size(), 3u);
    check_equal(manager.flush(), 3u);
    check_equal(user->order.user_id, 10u);
    check_equal(user->orders.front().user_id, 10u);

    auto stored_user = users.find_by_id(10u);
    auto stored_one = orders.find_by_id(100u);
    auto stored_many = orders.find_by_id(101u);
    check_true(stored_user.has_value());
    check_true(stored_one.has_value());
    check_true(stored_many.has_value());
    check_equal(stored_user->name.c_str(), "merged");
    check_equal(stored_one->label.c_str(), "merged-one");
    check_equal(stored_many->label.c_str(), "merged-many");

    stored_user->name = "database";
    stored_one->label = "database-one";
    stored_many->label = "database-many";
    check_equal(users.update(*stored_user).affected_rows(), 1u);
    check_equal(orders.update(*stored_one).affected_rows(), 1u);
    check_equal(orders.update(*stored_many).affected_rows(), 1u);

    user->name = "local";
    user->order.label = "local-one";
    user->orders.front().label = "local-many";
    manager.refresh(user);
    check_equal(user->name.c_str(), "database");
    check_equal(user->order.label.c_str(), "database-one");
    check_equal(user->orders.front().label.c_str(), "database-many");
    check_false(manager.dirty());

    manager.detach(user);
    check_false(manager.contains(user));
    check_equal(manager.size(), 0u);
  }

  it("removes only materialized relations after eager find") {
    orm::connection connection(sqlite_config());
    (void)connection.raw("pragma foreign_keys = on").execute();
    (void)connection
        .raw("create table users(user_id integer primary key, entity_version integer not null, "
             "display_name text not null)")
        .execute();
    (void)connection
        .raw("create table orders(id integer primary key, owner_id integer not null "
             "references users(user_id), item_label text not null)")
        .execute();

    orm::repository<app::model::User> users(connection);
    orm::repository<app::model::Order> orders(connection);
    check_equal(users.insert({11, 0, {}, "eager", {}}).affected_rows(), 1u);
    check_equal(orders.insert({110, 11, "first"}).affected_rows(), 1u);
    check_equal(orders.insert({111, 11, "second"}).affected_rows(), 1u);

    orm::entity_manager manager(connection);
    auto user = manager.find<app::model::User>(11u);
    check_not_null(user.get());
    check_equal(user->order.id, 0u);
    check_equal(user->orders.size(), 2u);
    check_equal(manager.size(), 3u);

    user->orders.pop_back();
    check_equal(manager.flush(), 1u);
    check_false(orders.find_by_id(111u).has_value());

    manager.remove(user);
    check_equal(manager.flush(), 2u);
    check_false(users.find_by_id(11u).has_value());
    check_false(orders.find_by_id(110u).has_value());
  }

  it("rolls back eager materialization when the identity map is full") {
    orm::connection connection(sqlite_config());
    (void)connection
        .raw("create table users(user_id integer primary key, entity_version integer not null, "
             "display_name text not null)")
        .execute();
    (void)connection
        .raw("create table orders(id integer primary key, owner_id integer not null, "
             "item_label text not null)")
        .execute();

    orm::repository<app::model::User> users(connection);
    orm::repository<app::model::Order> orders(connection);
    check_equal(users.insert({12, 0, {}, "bounded", {}}).affected_rows(), 1u);
    check_equal(orders.insert({120, 12, "child"}).affected_rows(), 1u);

    orm::entity_manager manager(connection, 1u);
    orm_status_t status = ORM_STATUS_OK;
    try {
      (void)manager.find<app::model::User>(12u);
    } catch (const orm::status_error& error) {
      status = error.status();
    }
    check_equal(status, ORM_STATUS_LIMIT_EXCEEDED);
    check_equal(manager.size(), 0u);
    check_false(manager.dirty());
  }
}
