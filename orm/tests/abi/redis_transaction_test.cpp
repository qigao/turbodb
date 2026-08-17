#include "orm.hpp"

#include "redis_fake_support.hpp"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template<typename Function>
void require_status(Function&& function,
                    orm_status_t expected,
                    const char* message)
{
    try {
        function();
    } catch (const orm::status_error& error) {
        require(error.status() == expected, message);
        return;
    }
    throw std::runtime_error(message);
}

} // namespace

int main()
{
    try {
        fake_redis_reset();
        orm::config config("redis");
        orm::connection connection(config);

        require_status(
            [&] {
                auto unsupported = connection.begin_transaction(
                    orm::isolation_level::read_committed);
                (void)unsupported;
            },
            ORM_STATUS_UNSUPPORTED,
            "Redis accepted a non-serializable isolation level");

        auto transaction = connection.begin_transaction(
            orm::isolation_level::serializable);
        auto inserted = connection.insert("person")
                            .set("id", 4)
                            .set("name", "Dora")
                            .execute(transaction);
        auto selected = connection.select("person")
                            .column("id")
                            .column("name")
                            .where("id", orm::comparison::equal, 4)
                            .execute(transaction);
        require_status(
            [&] { (void)inserted.affected_rows(); },
            ORM_STATUS_INVALID_STATE,
            "Redis result was readable before EXEC");
        require_status(
            [&] { (void)connection.select("person").column("id").execute(); },
            ORM_STATUS_INVALID_STATE,
            "Redis transaction could be bypassed through its connection");
        require_status(
            [&] { transaction.savepoint("unsupported"); },
            ORM_STATUS_UNSUPPORTED,
            "Redis accepted a savepoint");

        transaction.commit();
        require(inserted.affected_rows() == 1,
                "Redis transaction mutation result is incorrect");
        require(selected.rows() == 1 && selected.columns() == 2 &&
                    selected.int64(0, 0) == 4 && selected.text(0, 1) == "Dora",
                "Redis deferred selection was not materialized by EXEC");

        const std::vector<std::vector<std::string>> commands =
            fake_redis_commands();
        auto has_command = [&](const char* name) {
            for (const auto& command : commands) {
                if (!command.empty() && command.front() == name)
                    return true;
            }
            return false;
        };
        require(has_command("MULTI"), "Redis fake did not record MULTI");
        require(has_command("EXEC"), "Redis fake did not record EXEC");
        require(has_command("FT.SEARCH"), "Redis fake did not record FT.SEARCH");

        auto rolled_back = connection.begin_transaction(
            orm::isolation_level::serializable);
        auto discarded = connection.delete_from("person")
                             .where("id", orm::comparison::equal, 4)
                             .execute(rolled_back);
        rolled_back.rollback();
        require_status(
            [&] { (void)discarded.affected_rows(); },
            ORM_STATUS_INVALID_STATE,
            "Redis discarded result remained readable");

        {
            auto abandoned = connection.begin_transaction(
                orm::isolation_level::serializable);
            (void)connection.update("person")
                .set("name", "Abandoned")
                .where("id", orm::comparison::equal, 4)
                .execute(abandoned);
        }
        auto reusable = connection.begin_transaction(
            orm::isolation_level::serializable);
        reusable.rollback();

        auto partially_failed = connection.begin_transaction(
            orm::isolation_level::serializable);
        auto first = connection.insert("person")
                         .set("id", 5)
                         .set("name", "First")
                         .execute(partially_failed);
        auto duplicate = connection.insert("person")
                             .set("id", "duplicate")
                             .set("name", "Duplicate")
                             .execute(partially_failed);
        try {
            partially_failed.commit();
            throw std::runtime_error("Redis EXEC command error was ignored");
        } catch (const orm::status_error& error) {
            require(error.status() == ORM_STATUS_DATASTORE_ERROR &&
                        std::string(error.what()).find("does not roll back") !=
                            std::string::npos,
                    "Redis EXEC failure omitted its partial-execution contract");
        }
        require(first.affected_rows() == 1,
                "successful Redis EXEC reply was lost after a sibling error");
        require_status(
            [&] { (void)duplicate.affected_rows(); },
            ORM_STATUS_INVALID_STATE,
            "failed Redis EXEC result became readable");

        orm::config limited_config("redis");
        limited_config.option("transaction_command_limit", "1");
        orm::connection limited_connection(limited_config);
        auto limited = limited_connection.begin_transaction(
            orm::isolation_level::serializable);
        (void)limited_connection.insert("person")
            .set("id", 10)
            .execute(limited);
        require_status(
            [&] {
                (void)limited_connection.insert("person")
                    .set("id", 11)
                    .execute(limited);
            },
            ORM_STATUS_LIMIT_EXCEEDED,
            "Redis transaction command bound was not enforced");
        limited.rollback();

        std::cout << "Redis explicit transaction contract tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Redis transaction test failure: " << error.what() << '\n';
        return 1;
    }
}
