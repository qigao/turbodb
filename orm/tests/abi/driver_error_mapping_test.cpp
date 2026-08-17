#include "orm.hpp"

#include "pg_fake_core.hpp"
#include "redis_fake_support.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Action>
void expect_status(Action&& action, orm_status_t expected, const char* message)
{
    try {
        action();
    } catch (const orm::status_error& error) {
        const std::string detail =
            std::string(message) + " (actual " +
            std::to_string(static_cast<int>(error.status())) + ")";
        require(error.status() == expected, detail.c_str());
        return;
    }
    throw std::runtime_error(std::string(message) + " (no status_error thrown)");
}

struct pg_case {
    const char* name;
    orm_status_t expected;
    void (*setup)();
};

void setup_fatal()
{
    pg_fake::next_call.has_status = true;
    pg_fake::next_call.status = PGRES_FATAL_ERROR;
    pg_fake::next_call.has_error = true;
    pg_fake::next_call.error = "forced SQL failure";
}

void setup_bad_response()
{
    pg_fake::next_call.has_status = true;
    pg_fake::next_call.status = PGRES_BAD_RESPONSE;
    pg_fake::next_call.has_error = true;
    pg_fake::next_call.error = "bad response";
}

void setup_null_result()
{
    pg_fake::next_call.return_null_result = true;
}

void setup_invalid_affected_rows()
{
    pg_fake::next_call.has_status = true;
    pg_fake::next_call.status = PGRES_COMMAND_OK;
    pg_fake::next_call.has_affected_rows = true;
    pg_fake::next_call.affected_rows = "not-a-number";
}

void setup_negative_rows()
{
    pg_fake::next_call.has_status = true;
    pg_fake::next_call.status = PGRES_TUPLES_OK;
    pg_fake::next_call.has_dimensions = true;
    pg_fake::next_call.rows = -1;
    pg_fake::next_call.columns = 1;
}

void setup_oversized_rows()
{
    pg_fake::next_call.has_status = true;
    pg_fake::next_call.status = PGRES_TUPLES_OK;
    pg_fake::next_call.has_dimensions = true;
    pg_fake::next_call.rows = 10001;
    pg_fake::next_call.columns = 1;
}

void setup_oversized_columns()
{
    pg_fake::next_call.has_status = true;
    pg_fake::next_call.status = PGRES_TUPLES_OK;
    pg_fake::next_call.has_dimensions = true;
    pg_fake::next_call.rows = 0;
    pg_fake::next_call.columns = 257;
}

orm::config postgres_config()
{
    orm::config config("postgresql");
    config.option("host", "localhost").option("dbname", "test");
    return config;
}

void run_postgres_mapping()
{
    orm::connection connection(postgres_config());
    const std::vector<pg_case> cases = {
        {"PGRES_FATAL_ERROR maps to SQL_ERROR", ORM_STATUS_SQL_ERROR, setup_fatal},
        {"PGRES_BAD_RESPONSE maps to SQL_ERROR", ORM_STATUS_SQL_ERROR, setup_bad_response},
        {"null libpq result maps to SQL_ERROR", ORM_STATUS_SQL_ERROR, setup_null_result},
        {"invalid affected rows map to INTERNAL_ERROR", ORM_STATUS_INTERNAL_ERROR,
         setup_invalid_affected_rows},
        {"negative row count maps to INTERNAL_ERROR", ORM_STATUS_INTERNAL_ERROR,
         setup_negative_rows},
        {"oversized row count maps to LIMIT_EXCEEDED", ORM_STATUS_LIMIT_EXCEEDED,
         setup_oversized_rows},
        {"oversized column count maps to LIMIT_EXCEEDED", ORM_STATUS_LIMIT_EXCEEDED,
         setup_oversized_columns},
    };

    for (const pg_case& entry : cases) {
        pg_fake::reset_script_state();
        entry.setup();
        expect_status(
            [&] { (void)connection.raw("select 1").execute(); },
            entry.expected, entry.name);
    }
}

orm::config redis_config()
{
    orm::config config("redis");
    return config;
}

void run_redis_mapping()
{
    {
        fake_redis_reset();
        orm::connection connection(redis_config());
        fake_redis_script_next_outcome(REDIS_COMMAND_NOT_SENT);
        expect_status(
            [&] { (void)connection.select("person").column("id").execute(); },
            ORM_STATUS_CONNECTION_ERROR, "NOT_SENT maps to CONNECTION_ERROR");
    }
    {
        fake_redis_reset();
        orm::connection connection(redis_config());
        fake_redis_script_next_outcome(REDIS_COMMAND_SEND_UNCERTAIN);
        expect_status(
            [&] { (void)connection.select("person").column("id").execute(); },
            ORM_STATUS_CONNECTION_ERROR, "SEND_UNCERTAIN maps to CONNECTION_ERROR");
    }
    {
        fake_redis_reset();
        orm::connection connection(redis_config());
        fake_redis_script_next_outcome(REDIS_COMMAND_REPLY_UNKNOWN);
        expect_status(
            [&] { (void)connection.select("person").column("id").execute(); },
            ORM_STATUS_CONNECTION_ERROR, "REPLY_UNKNOWN maps to CONNECTION_ERROR");
    }
    {
        fake_redis_reset();
        orm::connection connection(redis_config());
        fake_redis_script_next_server_error(REDIS_SERVER_ERROR_ERR, "boom");
        expect_status(
            [&] { (void)connection.select("person").column("id").execute(); },
            ORM_STATUS_DATASTORE_ERROR, "server error maps to DATASTORE_ERROR");
    }
    {
        fake_redis_reset();
        orm::connection connection(redis_config());
        auto transaction = connection.begin_transaction(
            orm::isolation_level::serializable);
        (void)connection.insert("person")
            .set("id", 1)
            .execute(transaction);
        fake_redis_script_exec_reply_count(0);
        expect_status(
            [&] { transaction.commit(); },
            ORM_STATUS_DATASTORE_ERROR, "EXEC reply mismatch maps to DATASTORE_ERROR");
    }
}

} // namespace

int main()
{
    try {
        run_postgres_mapping();
        run_redis_mapping();
        std::cout << "driver error mapping tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "driver error mapping test failure: " << error.what() << '\n';
        return 1;
    }
}
