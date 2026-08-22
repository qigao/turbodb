#include "orm.hpp"

#include <mongoc/mongoc.h>
#include <tinytest.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

constexpr const char* live_test_environment = "TURBODB_MONGO_LIVE";
constexpr const char* uri_environment = "TURBODB_MONGO_URI";
constexpr const char* database_environment = "TURBODB_MONGO_DATABASE";
constexpr const char* default_uri =
    "mongodb://127.0.0.1:27017/?serverSelectionTimeoutMS=5000";
constexpr const char* default_database = "turbodb_orm_bdd";
constexpr const char* entity_table = "person";

struct client_deleter {
    void operator()(mongoc_client_t* client) const noexcept
    {
        mongoc_client_destroy(client);
    }
};

struct bson_deleter {
    void operator()(bson_t* document) const noexcept
    {
        bson_destroy(document);
    }
};

using client_handle = std::unique_ptr<mongoc_client_t, client_deleter>;
using bson_handle = std::unique_ptr<bson_t, bson_deleter>;

[[noreturn]] void fail(std::string message)
{
    throw std::runtime_error(std::move(message));
}

void require(bool condition, const char* message)
{
    if (!condition)
        fail(message);
}

struct live_report {
    std::string uri = default_uri;
    std::string database = default_database;
    std::string stage = "not started";
    std::string error;
    bool orm_crud_verified = false;
    bool orm_aggregate_verified = false;
    bool orm_transaction_verified = false;
    bool orm_transaction_supported = true;
};

class database_cleanup final {
public:
    database_cleanup(mongoc_client_t* client, std::string database) noexcept
        : client_(client), database_(std::move(database))
    {
    }

    ~database_cleanup() noexcept
    {
        bson_error_t error;
        mongoc_database_t* database =
            mongoc_client_get_database(client_, database_.c_str());
        if (database != nullptr) {
            (void)mongoc_database_drop(database, &error);
            mongoc_database_destroy(database);
        }
    }

private:
    mongoc_client_t* client_;
    std::string database_;
};

orm::config mongo_config(const live_report& report)
{
    orm::config configuration("mongo");
    configuration.option("uri", report.uri)
        .option("database", report.database)
        .option("id_column", "id");
    return configuration;
}

void insert_people(orm::connection& connection)
{
    require(connection.insert(entity_table)
                    .set("id", 1)
                    .set("name", "Alice")
                    .set("status", "active")
                    .set("score", 10.0)
                    .set("country", "US")
                    .set("version", 0u)
                    .execute()
                    .affected_rows() == 1,
            "first MongoDB ORM insert failed");
    require(connection.insert(entity_table)
                    .set("id", 2)
                    .set("name", "Bob")
                    .set("status", "inactive")
                    .set("score", 20.0)
                    .set("country", "UK")
                    .set("version", 0u)
                    .execute()
                    .affected_rows() == 1,
            "second MongoDB ORM insert failed");
    require(connection.insert(entity_table)
                    .set("id", 3)
                    .set("name", "Cara")
                    .set("status", "active")
                    .set("score", 30.0)
                    .set("country", "US")
                    .set("version", 0u)
                    .execute()
                    .affected_rows() == 1,
            "third MongoDB ORM insert failed");
}

std::uint64_t row_count(orm::connection& connection, std::int64_t id)
{
    return connection.select(entity_table)
        .column("id")
        .where("id", orm::comparison::equal, id)
        .execute()
        .rows();
}

void verify_crud(live_report& report, orm::connection& connection)
{
    insert_people(connection);

    bool duplicate_rejected = false;
    try {
        (void)connection.insert(entity_table)
            .set("id", 1)
            .set("name", "Duplicate")
            .execute();
    } catch (const orm::status_error& error) {
        duplicate_rejected = error.status() == ORM_STATUS_DATASTORE_ERROR;
    }
    require(duplicate_rejected, "duplicate _id insert was not rejected");

    const auto selected = connection.select(entity_table)
        .column("id")
        .column("name")
        .column("score")
        .where("status", orm::comparison::equal, "active")
        .order_by("score", orm::sort_order::descending)
        .execute();
    require(selected.rows() == 2 && selected.columns() == 3 &&
                selected.int64(0, 0) == 3 && selected.text(0, 1) == "Cara" &&
                selected.real(0, 2) == 30.0 && selected.int64(1, 0) == 1,
            "MongoDB ORM selection returned unexpected rows");

    const auto liked = connection.select(entity_table)
        .column("name")
        .where("name", orm::comparison::like, "A%")
        .execute();
    require(liked.rows() == 1 && liked.text(0, 0) == "Alice",
            "MongoDB LIKE selection returned unexpected rows");

    require(connection.update(entity_table)
                    .set("score", 25.0)
                    .set("version", 1u)
                    .where("id", orm::comparison::equal, 2)
                    .where("version", orm::comparison::equal, 0u)
                    .execute()
                    .affected_rows() == 1,
            "MongoDB ORM update failed");
    require(connection.update(entity_table)
                    .set("score", 99.0)
                    .set("version", 1u)
                    .where("id", orm::comparison::equal, 2)
                    .where("version", orm::comparison::equal, 0u)
                    .execute()
                    .affected_rows() == 0,
            "MongoDB stale version update was not rejected");
    require(connection.delete_from(entity_table)
                    .where("id", orm::comparison::equal, 3)
                    .where("version", orm::comparison::equal, 0u)
                    .execute()
                    .affected_rows() == 1,
            "MongoDB ORM delete failed");
    report.orm_crud_verified = true;
}

void verify_aggregate(live_report& report, orm::connection& connection)
{
    const auto grouped = connection.select(entity_table)
        .column("country")
        .aggregate(orm::aggregate_function::count_all, {}, "total")
        .aggregate(orm::aggregate_function::sum, "score", "score_sum")
        .group_by("country")
        .having(orm::aggregate_function::count_all, {},
                orm::comparison::greater, 0u)
        .order_by("country", orm::sort_order::ascending)
        .execute();
    require(grouped.rows() == 2 && grouped.columns() == 3,
            "MongoDB aggregation pipeline returned an unexpected shape");
    bool found_us = false;
    bool found_uk = false;
    for (std::uint64_t row = 0; row < grouped.rows(); ++row) {
        if (grouped.text(row, 0) == "US") {
            found_us = grouped.uint64(row, 1) == 2 &&
                       grouped.real(row, 2) == 40.0;
        } else if (grouped.text(row, 0) == "UK") {
            found_uk = grouped.uint64(row, 1) == 1 &&
                       grouped.real(row, 2) == 20.0;
        }
    }
    require(found_us && found_uk, "MongoDB ORM aggregate values were incorrect");
    report.orm_aggregate_verified = true;
}

void verify_transactions(live_report& report, orm::connection& connection)
{
    std::optional<orm::transaction> transaction;
    try {
        transaction.emplace(connection.begin_transaction());
    } catch (const orm::status_error& error) {
        report.orm_transaction_supported =
            error.status() == ORM_STATUS_UNSUPPORTED;
        if (!report.orm_transaction_supported)
            throw;
        return;
    }
    require(connection.insert(entity_table)
                    .set("id", 10)
                    .set("name", "Committed")
                    .execute(*transaction)
                    .affected_rows() == 1,
            "MongoDB transactional insert failed");
    const auto pending = connection.select(entity_table)
        .column("name")
        .where("id", orm::comparison::equal, 10)
        .execute(*transaction);
    require(pending.rows() == 1 && pending.text(0, 0) == "Committed",
            "MongoDB transactional read did not observe the pending write");
    transaction->commit();
    require(row_count(connection, 10) == 1,
            "committed MongoDB transaction write is not visible");

    auto rolled_back = connection.begin_transaction();
    (void)connection.insert(entity_table)
        .set("id", 11)
        .set("name", "Discarded")
        .execute(rolled_back);
    rolled_back.rollback();
    require(row_count(connection, 11) == 0,
            "rolled-back MongoDB transaction write is visible");
    report.orm_transaction_verified = true;
}

void live_mongo_task(live_report& report)
{
    report.stage = "create MongoDB client";
    client_handle client(mongoc_client_new(report.uri.c_str()));
    require(client != nullptr, "create MongoDB client failed");

    report.stage = "ping MongoDB server";
    bson_error_t error{};
    bson_handle ping(BCON_NEW("ping", BCON_INT32(1)));
    require(ping != nullptr, "create MongoDB ping command failed");
    if (!mongoc_client_command_simple(client.get(), "admin", ping.get(),
                                      nullptr, nullptr, &error)) {
        fail("ping MongoDB server failed: " + std::string(error.message));
    }
    database_cleanup cleanup(client.get(), report.database);

    report.stage = "drop MongoDB test database";
    mongoc_database_t* database =
        mongoc_client_get_database(client.get(), report.database.c_str());
    require(database != nullptr, "create MongoDB database handle failed");
    (void)mongoc_database_drop(database, &error);
    mongoc_database_destroy(database);

    report.stage = "execute MongoDB ORM CRUD";
    {
        orm::connection connection(mongo_config(report));
        verify_crud(report, connection);
        verify_aggregate(report, connection);
        report.stage = "execute MongoDB ORM transactions";
        verify_transactions(report, connection);
    }

    report.stage = "complete";
}

bool live_tests_enabled()
{
    const char* value = std::getenv(live_test_environment);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

live_report live_configuration()
{
    live_report report;
    if (const char* uri = std::getenv(uri_environment);
        uri != nullptr && uri[0] != '\0')
        report.uri = uri;
    if (const char* database = std::getenv(database_environment);
        database != nullptr && database[0] != '\0')
        report.database = database;
    return report;
}

} // namespace

spec("MongoDB ORM live integration") {
    given("an explicitly enabled MongoDB endpoint") {
        when("ORM CRUD aggregates and transactions execute against it") {
            then("typed rows round-trip and transactions honor commit and rollback") {
                if (!live_tests_enabled()) {
                    info("set TURBODB_MONGO_LIVE=1 to run the live MongoDB BDD");
                    check(true);
                    return;
                }
                live_report report = live_configuration();
                try {
                    live_mongo_task(report);
                } catch (const std::exception& error) {
                    report.error = error.what();
                } catch (...) {
                    report.error = "unknown exception";
                }
                if (!report.error.empty()) {
                    info("live MongoDB BDD failed at '%s': %s",
                         report.stage.c_str(), report.error.c_str());
                }
                check(report.error.empty());
                check(report.orm_crud_verified);
                check(report.orm_aggregate_verified);
                if (report.orm_transaction_supported) {
                    check(report.orm_transaction_verified);
                } else {
                    info("MongoDB endpoint has no replica set; "
                         "transactions were rejected with UNSUPPORTED");
                    check(true);
                }
            }
        }
    }
}
