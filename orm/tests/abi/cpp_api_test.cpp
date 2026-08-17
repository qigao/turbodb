#include "orm.hpp"

#include <tinytest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace {

  void require(bool condition, const char *message) {
    check(condition, message);
  }

  orm::config sqlite_config() {
    orm::config configuration("sqlite");
    configuration.option("filename", ":memory:").option("open_mode", "read_write_create");
    return configuration;
  }

  struct person_row {};
  struct department_row {};

  struct person_table final : orm::table<person_row> {
    person_table()
        : orm::table<person_row>("person"), id(*this, "id"), name(*this, "name"),
          department_id(*this, "department_id"), active(*this, "active"), score(*this, "score"),
          note(*this, "note") {}

    orm::column<std::int64_t> id;
    orm::column<std::string> name;
    orm::column<std::int64_t> department_id;
    orm::column<bool> active;
    orm::column<double> score;
    orm::column<std::string> note;
  };

  struct department_table final : orm::table<department_row> {
    department_table()
        : orm::table<department_row>("department"), id(*this, "id"), name(*this, "name") {}

    orm::column<std::int64_t> id;
    orm::column<std::string> name;
  };

  const person_table person;
  const department_table department;

  template <typename Column, typename Input, typename = void>
  struct supports_equal : std::false_type {};

  template <typename Column, typename Input>
  struct supports_equal<
      Column, Input, std::void_t<decltype(std::declval<const Column &>() == std::declval<Input>())>>
      : std::true_type {};

  static_assert(std::is_same_v<decltype(person.id == 7), orm::predicate>);
  static_assert(
      std::is_same_v<decltype(person.department_id == department.id), orm::join_condition>);
  static_assert(
      std::is_same_v<decltype(orm::count(person.id)), orm::aggregate_expression<std::uint64_t>>);
  static_assert(
      std::is_same_v<decltype(orm::sum(person.id)), orm::aggregate_expression<std::int64_t>>);
  static_assert(
      std::is_same_v<decltype(orm::avg(person.score)), orm::aggregate_expression<double>>);
  static_assert(
      std::is_same_v<decltype(orm::min(person.name)), orm::aggregate_expression<std::string>>);
  static_assert(
      std::is_same_v<decltype(orm::max(person.score)), orm::aggregate_expression<double>>);
  static_assert(!supports_equal<orm::column<std::int64_t>, std::string>::value);

  orm::query retained_query() {
    orm::connection connection(sqlite_config());
    const auto schema_result = connection
                                   .raw("create table person("
                                        "id integer primary key, name text not null, "
                                        "active integer not null, score real not null, note text)")
                                   .execute();
    (void)schema_result;

    const auto inserted = connection.insert("person")
                              .set("id", 7)
                              .set("name", "Alice")
                              .set("active", true)
                              .set("score", 1.5)
                              .set("note", nullptr)
                              .execute();
    require(inserted.affected_rows() == 1, "C++ DSL insert returned the wrong affected-row count");

    auto query = connection.select("person");
    query.column("id")
        .column("name")
        .column("active")
        .column("score")
        .column("note")
        .where("id", orm::comparison::equal, 7)
        .order_by("id", orm::sort_order::ascending);
    return query;
  }

  void test_raii_query_and_result() {
    auto query = retained_query();
    const auto result = query.execute();

    require(result.rows() == 1, "C++ DSL returned the wrong row count");
    require(result.columns() == 5, "C++ DSL returned the wrong column count");
    require(result.int64(0, 0) == 7, "C++ DSL integer decoding failed");
    require(result.text(0, 1) == "Alice", "C++ DSL text decoding failed");
    require(result.boolean(0, 2), "C++ DSL boolean decoding failed");
    require(result.real(0, 3) == 1.5, "C++ DSL floating-point decoding failed");
    require(result.is_null(0, 4), "C++ DSL SQL NULL decoding failed");
  }

  void test_typed_errors() {
    auto query = retained_query();
    const auto result = query.execute();

    try {
      (void)result.int64(1, 0);
    } catch (const orm::status_error &error) {
      require(error.status() == ORM_STATUS_OUT_OF_RANGE,
              "C++ DSL preserved the wrong C ABI status");
      require(std::string(error.what()).find("read signed integer result") != std::string::npos,
              "C++ DSL error omitted operation context");
      return;
    }
    check(false, "C++ DSL did not throw status_error");
  }

  void test_cpp_dsl() {
    orm::connection connection(sqlite_config());
    (void)connection.raw("create table department(id integer primary key, name text not null)")
        .execute();
    (void)connection
        .raw("create table person(id integer primary key, name text not null, "
             "department_id integer not null, active integer not null, "
             "score real not null, note text)")
        .execute();

    require(connection.insert(department)
                    .set(department.id, 1)
                    .set(department.name, "Engineering")
                    .execute()
                    .affected_rows() == 1,
            "typed department insert failed");
    require(connection.insert(department)
                    .set(department.id, 2)
                    .set(department.name, "Sales")
                    .execute()
                    .affected_rows() == 1,
            "typed department insert failed");

    require(connection.insert(person)
                    .set(person.id, 1)
                    .set(person.name, "Alice")
                    .set(person.department_id, 1)
                    .set(person.active, true)
                    .set(person.score, 10.0)
                    .set(person.note, nullptr)
                    .execute()
                    .affected_rows() == 1,
            "typed person insert failed");
    require(connection.insert(person)
                    .set(person.id, 2)
                    .set(person.name, "Bob")
                    .set(person.department_id, 2)
                    .set(person.active, false)
                    .set(person.score, 20.0)
                    .set(person.note, "new")
                    .execute()
                    .affected_rows() == 1,
            "typed person insert failed");
    require(connection.insert(person)
                    .set(person.id, 3)
                    .set(person.name, "Cara")
                    .set(person.department_id, 1)
                    .set(person.active, false)
                    .set(person.score, 30.0)
                    .set(person.note, nullptr)
                    .execute()
                    .affected_rows() == 1,
            "typed person insert failed");

    const auto grouped =
        connection
            .select(department.name, orm::count_all().as("total"),
                    orm::avg(person.score).as("average_score"))
            .from(person)
            .left_join(department)
            .on(person.department_id == department.id)
            .where((person.active == true || person.name.like("B%")) && person.id >= 1)
            .group_by(department.name)
            .having(orm::count_all() > 0u)
            .order_by(department.name.asc())
            .limit(10)
            .fetch();

    require(grouped.rows() == 2, "typed SELECT returned the wrong group count");
    require(grouped.columns() == 3, "typed SELECT returned the wrong projection count");
    require(grouped.text(0, 0) == "Engineering", "typed JOIN returned the wrong first group");
    require(grouped.uint64(0, 1) == 1, "typed COUNT returned the wrong first value");
    require(grouped.real(0, 2) == 10.0, "typed AVG returned the wrong first value");
    require(grouped.text(1, 0) == "Sales", "typed JOIN returned the wrong second group");
    require(grouped.uint64(1, 1) == 1, "typed COUNT returned the wrong second value");
    require(grouped.real(1, 2) == 20.0, "typed AVG returned the wrong second value");

    require(connection.update(person)
                    .set(person.score, 22.0)
                    .where((person.id == 2 || person.name == "Nobody") && person.active == false)
                    .execute()
                    .affected_rows() == 1,
            "typed UPDATE failed");
    require(connection.delete_from(person).where(person.id == 3).execute().affected_rows() == 1,
            "typed DELETE failed");

    const auto changed =
        connection.select(person.id, person.score).from(person).where(person.id == 2).fetch();
    require(changed.rows() == 1 && changed.int64(0, 0) == 2 && changed.real(0, 1) == 22.0,
            "typed DML result verification failed");

    const auto all_columns = connection.select().from(person).where(person.id == 1).fetch();
    require(all_columns.rows() == 1 && all_columns.columns() == 6,
            "empty typed projection did not select all columns");

    try {
      (void)connection.update(person).where(department.id == 1);
    } catch (const std::invalid_argument &) {
      return;
    }
    check(false, "typed DML accepted a predicate from a different table");
  }

} // namespace

suite("ORM C++ API") {
  it("retains queries after their originating connection wrapper is destroyed") {
    test_raii_query_and_result();
  }

  it("preserves typed C ABI error status and operation context") {
    test_typed_errors();
  }

  it("executes typed joins, aggregates, predicates, and DML") {
    test_cpp_dsl();
  }
}
