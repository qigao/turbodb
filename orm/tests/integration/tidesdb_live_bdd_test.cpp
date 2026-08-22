#include "orm.hpp"

#include <tinytest.hpp>
#include <turbo_thread.h>

#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

  constexpr const char *entity_table = "person";
  constexpr std::uint32_t expiration_poll_interval_ms = 100;
  constexpr std::uint32_t expiration_poll_attempts = 30;

  class temp_directory final {
  public:
    temp_directory() : path_(tt_make_temp_dir("orm-tidesdb")) {}

    ~temp_directory() noexcept {
      if (path_ != nullptr) {
        (void)tt_remove_tree(path_);
        std::free(path_);
      }
    }

    temp_directory(const temp_directory &) = delete;
    temp_directory &operator=(const temp_directory &) = delete;

    const char *path() const noexcept { return path_; }

  private:
    char *path_;
  };

  orm::config tidesdb_config(const char *path, const char *max_scan_rows = "128",
                             const char *ttl_seconds = "0") {
    orm::config config("tidesdb");
    config.option("path", path)
        .option("column_family", "orm_bdd")
        .option("ttl_seconds", ttl_seconds)
        .option("max_scan_rows", max_scan_rows)
        .option("max_scan_bytes", "1048576");
    return config;
  }

  std::uint64_t row_count(orm::connection &connection, std::int64_t id) {
    return connection.select(entity_table)
        .column("id")
        .where("id", orm::comparison::equal, id)
        .execute()
        .rows();
  }

  void insert_people(orm::connection &connection) {
    check_equal(connection.insert(entity_table)
                      .set("id", 1)
                      .set("name", "Alice")
                      .set("status", "active")
                      .set("score", 10.0)
                      .set("country", "US")
                      .set("version", 0u)
                      .execute()
                      .affected_rows(),
                  1);
    check_equal(connection.insert(entity_table)
                      .set("id", 2)
                      .set("name", "Bob")
                      .set("status", "inactive")
                      .set("score", 20.0)
                      .set("country", "UK")
                      .set("version", 0u)
                      .execute()
                      .affected_rows(),
                  1);
    check_equal(connection.insert(entity_table)
                      .set("id", 3)
                      .set("name", "Cara")
                      .set("status", "active")
                      .set("score", 30.0)
                      .set("country", "US")
                      .set("version", 0u)
                      .execute()
                      .affected_rows(),
                  1);
  }

} // namespace

spec("TidesDB ORM embedded integration") {

  given("a fresh embedded TidesDB directory") {
    when("ORM CRUD, snapshot selection, and aggregation execute") {
      then("typed rows are transactional, bounded, and durable across reopen") {
        temp_directory directory;
        check_not_null(directory.path());
        if (directory.path() == nullptr) return;

        {
          orm::connection connection(tidesdb_config(directory.path()));
          insert_people(connection);

          bool duplicate_rejected = false;
          try {
            (void)connection.insert(entity_table).set("id", 1).set("name", "Duplicate").execute();
          } catch (const orm::status_error &error) {
            duplicate_rejected = error.status() == ORM_STATUS_DATASTORE_ERROR;
          }
          check(duplicate_rejected);

          const auto selected = connection.select(entity_table)
                                    .column("id")
                                    .column("name")
                                    .column("score")
                                    .where("status", orm::comparison::equal, "active")
                                    .order_by("score", orm::sort_order::descending)
                                    .execute();
          check_equal(selected.rows(), 2);
          check_equal(selected.columns(), 3);
          check_equal(selected.int64(0, 0), 3);
          check(selected.text(0, 1) == "Cara");
          check_within(selected.real(0, 2), 30.0, 0.000001);
          check_equal(selected.int64(1, 0), 1);

          const auto grouped =
              connection.select(entity_table)
                  .column("country")
                  .aggregate(orm::aggregate_function::count_all, {}, "total")
                  .aggregate(orm::aggregate_function::sum, "score", "score_sum")
                  .group_by("country")
                  .having(orm::aggregate_function::count_all, {}, orm::comparison::greater, 0u)
                  .order_by("country", orm::sort_order::ascending)
                  .execute();
          check_equal(grouped.rows(), 2);
          check_equal(grouped.columns(), 3);
          check(grouped.text(0, 0) == "UK");
          check_equal(grouped.uint64(0, 1), 1);
          check_within(grouped.real(0, 2), 20.0, 0.000001);
          check(grouped.text(1, 0) == "US");
          check_equal(grouped.uint64(1, 1), 2);
          check_within(grouped.real(1, 2), 40.0, 0.000001);

          check_equal(connection.update(entity_table)
                            .set("score", 25.0)
                            .set("version", 1u)
                            .where("id", orm::comparison::equal, 2)
                            .where("version", orm::comparison::equal, 0u)
                            .execute()
                            .affected_rows(),
                        1);
          check_equal(connection.update(entity_table)
                            .set("score", 99.0)
                            .set("version", 1u)
                            .where("id", orm::comparison::equal, 2)
                            .where("version", orm::comparison::equal, 0u)
                            .execute()
                            .affected_rows(),
                        0);
          check_equal(connection.delete_from(entity_table)
                            .where("id", orm::comparison::equal, 3)
                            .where("version", orm::comparison::equal, 0u)
                            .execute()
                            .affected_rows(),
                        1);
        }

        {
          orm::connection reopened(tidesdb_config(directory.path()));
          const auto persisted = reopened.select(entity_table)
                                     .column("id")
                                     .column("score")
                                     .order_by("id", orm::sort_order::ascending)
                                     .execute();
          check_equal(persisted.rows(), 2);
          check_equal(persisted.int64(0, 0), 1);
          check_equal(persisted.int64(1, 0), 2);
          check_within(persisted.real(1, 1), 25.0, 0.000001);
        }
      }
    }

    when("an explicit transaction uses savepoints and rollback") {
      then("only committed operations become visible") {
        temp_directory directory;
        check_not_null(directory.path());
        if (directory.path() == nullptr) return;

        orm::connection connection(tidesdb_config(directory.path()));
        auto transaction = connection.begin_transaction();
        check_equal(connection.insert(entity_table)
                          .set("id", 10)
                          .set("name", "Committed")
                          .execute(transaction)
                          .affected_rows(),
                      1);
        const auto pending = connection.select(entity_table)
                                 .column("name")
                                 .where("id", orm::comparison::equal, 10)
                                 .execute(transaction);
        check_equal(pending.rows(), 1);
        check(pending.text(0, 0) == "Committed");
        transaction.savepoint("released").release("released");
        transaction.savepoint("before_discard");
        check_equal(connection.insert(entity_table)
                          .set("id", 11)
                          .set("name", "Discarded")
                          .execute(transaction)
                          .affected_rows(),
                      1);
        transaction.rollback_to("before_discard");
        transaction.commit();

        check_equal(row_count(connection, 10), 1);
        check_equal(row_count(connection, 11), 0);

        auto rolled_back = connection.begin_transaction(orm::isolation_level::read_committed);
        check_equal(connection.update(entity_table)
                          .set("name", "Not visible")
                          .where("id", orm::comparison::equal, 10)
                          .execute(rolled_back)
                          .affected_rows(),
                      1);
        rolled_back.rollback();

        const auto retained = connection.select(entity_table)
                                  .column("name")
                                  .where("id", orm::comparison::equal, 10)
                                  .execute();
        check_equal(retained.rows(), 1);
        check(retained.text(0, 0) == "Committed");

        {
          auto abandoned = connection.begin_transaction();
          (void)connection.insert(entity_table)
              .set("id", 12)
              .set("name", "Auto rollback")
              .execute(abandoned);
        }
        check_equal(row_count(connection, 12), 0);
      }
    }

    when("all v9 isolation levels are requested") {
      then("valid levels begin and cross-connection execution is rejected") {
        temp_directory first_directory;
        temp_directory second_directory;
        check_not_null(first_directory.path());
        check_not_null(second_directory.path());
        if (first_directory.path() == nullptr || second_directory.path() == nullptr) return;

        orm::connection first(tidesdb_config(first_directory.path()));
        orm::connection second(tidesdb_config(second_directory.path()));
        const orm::isolation_level levels[] = {
            orm::isolation_level::read_uncommitted, orm::isolation_level::read_committed,
            orm::isolation_level::repeatable_read, orm::isolation_level::snapshot,
            orm::isolation_level::serializable};
        for (const orm::isolation_level level : levels) {
          auto transaction = first.begin_transaction(level);
          transaction.rollback();
        }

        auto transaction = first.begin_transaction();
        bool rejected = false;
        try {
          (void)second.select(entity_table).column("id").execute(transaction);
        } catch (const orm::status_error &error) {
          rejected = error.status() == ORM_STATUS_INVALID_ARGUMENT;
        }
        check(rejected);
        transaction.rollback();
      }
    }

    when("a connection enforces the single active transaction contract") {
      then("nested transactions and connection-level queries are rejected") {
        temp_directory directory;
        check_not_null(directory.path());
        if (directory.path() == nullptr) return;

        orm::connection connection(tidesdb_config(directory.path()));
        auto transaction = connection.begin_transaction();

        bool nested_rejected = false;
        try {
          auto nested = connection.begin_transaction();
          (void)nested;
        } catch (const orm::status_error &error) {
          nested_rejected = error.status() == ORM_STATUS_INVALID_STATE;
        }
        check(nested_rejected);

        bool direct_insert_rejected = false;
        try {
          (void)connection.insert(entity_table).set("id", 20).set("name", "Direct").execute();
        } catch (const orm::status_error &error) {
          direct_insert_rejected = error.status() == ORM_STATUS_INVALID_STATE;
        }
        check(direct_insert_rejected);

        bool direct_select_rejected = false;
        try {
          (void)connection.select(entity_table).column("id").execute();
        } catch (const orm::status_error &error) {
          direct_select_rejected = error.status() == ORM_STATUS_INVALID_STATE;
        }
        check(direct_select_rejected);

        check_equal(connection.insert(entity_table)
                          .set("id", 20)
                          .set("name", "Committed")
                          .execute(transaction)
                          .affected_rows(),
                      1);
        transaction.commit();

        check_equal(row_count(connection, 20), 1);
      }
    }

    when("binary values round-trip") {
      then("embedded bytes survive insert and select") {
        temp_directory directory;
        check_not_null(directory.path());
        if (directory.path() == nullptr) return;

        orm::connection connection(tidesdb_config(directory.path()));
        const std::vector<std::uint8_t> payload = {0x00, 0x01, 0x00, 0xFF, 0x00, 0x80};
        (void)connection.insert(entity_table)
            .set("id", 45)
            .set("name", "Blob")
            .set("payload", orm::value(payload))
            .execute();

        const auto selected = connection.select(entity_table)
                                  .column("payload")
                                  .where("id", orm::comparison::equal, 45)
                                  .execute();
        check_equal(selected.rows(), 1);
        check(selected.blob(0, 0) == payload);
      }
    }

    when("TTL and scan limits are configured") {
      then("expired rows disappear and oversized scans fail fast") {
        temp_directory ttl_directory;
        check_not_null(ttl_directory.path());
        if (ttl_directory.path() == nullptr) return;

        orm::connection expiring(tidesdb_config(ttl_directory.path(), "128", "1"));
        (void)expiring.insert(entity_table).set("id", 30).set("name", "Temporary").execute();
        check_equal(row_count(expiring, 30), 1);

        bool expired = false;
        for (std::uint32_t attempt = 0; attempt < expiration_poll_attempts && !expired; ++attempt) {
          turbo_sleep_ms(expiration_poll_interval_ms);
          expired = row_count(expiring, 30) == 0;
        }
        check(expired);

        temp_directory limited_directory;
        check_not_null(limited_directory.path());
        if (limited_directory.path() == nullptr) return;
        orm::connection limited(tidesdb_config(limited_directory.path(), "2"));
        insert_people(limited);

        bool bounded = false;
        try {
          (void)limited.select(entity_table).column("id").execute();
        } catch (const orm::status_error &error) {
          bounded = error.status() == ORM_STATUS_LIMIT_EXCEEDED;
        }
        check(bounded);
      }
    }
  }
}
