#include "orm.hpp"
#include "CoroNet/turbo_coro_context.h"
#include "redis_client.h"
#include "redis_pool.h"

#include <tinytest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr const char* live_test_environment = "TURBODB_REDIS_LIVE";
constexpr const char* host_environment = "TURBODB_REDIS_HOST";
constexpr const char* port_environment = "TURBODB_REDIS_PORT";
constexpr const char* default_host = "127.0.0.1";
constexpr std::uint16_t default_port = 6379;
constexpr const char* pipeline_key = "turbodb:bdd:pipeline";
constexpr const char* lua_key = "turbodb:bdd:lua";
constexpr const char* orm_table = "person";
constexpr const char* orm_index = "turbodb_bdd_idx:person";
constexpr const char* orm_key_prefix = "turbodb_bdd:";

struct client_deleter {
    void operator()(redis_client_t* client) const noexcept
    {
        redis_client_destroy(client);
    }
};

struct pool_deleter {
    void operator()(redis_pool_t* pool) const noexcept
    {
        redis_pool_stop(pool);
        redis_pool_destroy(pool);
    }
};

struct pipeline_deleter {
    void operator()(redis_pipeline_t* pipeline) const noexcept
    {
        redis_pipeline_destroy(pipeline);
    }
};

using client_handle = std::unique_ptr<redis_client_t, client_deleter>;
using pool_handle = std::unique_ptr<redis_pool_t, pool_deleter>;
using pipeline_handle = std::unique_ptr<redis_pipeline_t, pipeline_deleter>;

class command_result final {
public:
    ~command_result() noexcept
    {
        redis_command_result_clear(&value_);
    }

    command_result(const command_result&) = delete;
    command_result& operator=(const command_result&) = delete;

    command_result() = default;

    redis_command_result_t* output() noexcept
    {
        redis_command_result_clear(&value_);
        return &value_;
    }

    const redis_command_result_t& get() const noexcept
    {
        return value_;
    }

private:
    redis_command_result_t value_ = REDIS_COMMAND_RESULT_INIT;
};

struct live_report {
    coro_context_t* context = nullptr;
    std::string host = default_host;
    std::uint16_t port = default_port;
    std::string stage = "not started";
    std::string error;
    bool pipeline_verified = false;
    bool lua_verified = false;
    bool query_engine_available = false;
    bool orm_rejected_missing_query_engine = false;
    bool orm_search_verified = false;
    bool orm_aggregate_verified = false;
    bool orm_mutations_verified = false;
    bool orm_transaction_verified = false;
};

[[noreturn]] void fail(std::string message)
{
    throw std::runtime_error(std::move(message));
}

void require(bool condition, const char* message)
{
    if (!condition)
        fail(message);
}

std::string reply_text(const redis_reply_t* reply)
{
    if (reply == nullptr || reply->str == nullptr)
        return {};
    return std::string(reply->str, reply->len);
}

void execute(redis_client_t* client,
             const std::vector<std::string>& arguments,
             command_result& result)
{
    std::vector<const char*> argv;
    std::vector<std::size_t> lengths;
    argv.reserve(arguments.size());
    lengths.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        argv.push_back(argument.data());
        lengths.push_back(argument.size());
    }
    (void)redis_commandv_result(
        client, static_cast<int>(argv.size()), argv.data(), lengths.data(), result.output());
    const redis_command_result_t& value = result.get();
    if (value.outcome != REDIS_COMMAND_REPLIED)
        fail("Redis command did not receive a definite reply");
}

void execute_ok(redis_client_t* client,
                const std::vector<std::string>& arguments,
                command_result& result)
{
    execute(client, arguments, result);
    if (result.get().server_error != REDIS_SERVER_ERROR_NONE)
        fail("Redis server error: " + reply_text(result.get().reply));
}

bool reply_equals(const redis_reply_t* reply,
                  redis_reply_type_t type,
                  const char* expected)
{
    const std::size_t length = std::strlen(expected);
    return reply != nullptr && reply->type == type && reply->str != nullptr &&
           reply->len == length && std::memcmp(reply->str, expected, length) == 0;
}

void remove_test_keys(redis_client_t* client)
{
    command_result result;
    execute_ok(client, {"DEL", pipeline_key, lua_key}, result);
}

class test_key_guard final {
public:
    explicit test_key_guard(redis_client_t* client) noexcept : client_(client) {}

    ~test_key_guard() noexcept
    {
        try {
            remove_test_keys(client_);
        } catch (...) {
        }
    }

private:
    redis_client_t* client_;
};

void verify_pipeline(redis_pool_t* pool)
{
    pipeline_handle pipeline(redis_pool_pipeline_create(pool));
    require(pipeline != nullptr, "create Redis pipeline failed");

    const char* set[] = {"SET", pipeline_key, "10"};
    const char* increment[] = {"INCRBY", pipeline_key, "5"};
    const char* get[] = {"GET", pipeline_key};
    require(redis_pipeline_addv(pipeline.get(), nullptr, nullptr, 3, set, nullptr) == TURBO_OK,
            "queue SET in pipeline failed");
    require(redis_pipeline_addv(pipeline.get(), nullptr, nullptr, 3, increment, nullptr) == TURBO_OK,
            "queue INCRBY in pipeline failed");
    require(redis_pipeline_addv(pipeline.get(), nullptr, nullptr, 2, get, nullptr) == TURBO_OK,
            "queue GET in pipeline failed");

    redis_pipeline_result_t result = REDIS_PIPELINE_RESULT_INIT;
    const int status = redis_pipeline_execute_result(pipeline.get(), &result);
    const bool valid = status == TURBO_OK && result.outcome == REDIS_COMMAND_REPLIED &&
                       result.reply_count == 3 &&
                       reply_equals(result.replies[0], REDIS_REPLY_STRING, "OK") &&
                       result.replies[1] != nullptr &&
                       result.replies[1]->type == REDIS_REPLY_INTEGER &&
                       result.replies[1]->integer == 15 &&
                       reply_equals(result.replies[2], REDIS_REPLY_BULK_STRING, "15");
    redis_pipeline_result_clear(&result);
    require(valid, "pipeline replies were not complete and in command order");
}

void verify_lua(redis_client_t* client)
{
    constexpr const char* script =
        "return redis.call('INCRBY', KEYS[1], ARGV[1])";
    command_result result;
    execute_ok(client, {"DEL", lua_key}, result);

    const int load_status = redis_script_load_result(client, script, result.output());
    require(load_status == TURBO_OK &&
                result.get().outcome == REDIS_COMMAND_REPLIED &&
                result.get().server_error == REDIS_SERVER_ERROR_NONE &&
                result.get().reply != nullptr &&
                (result.get().reply->type == REDIS_REPLY_STRING ||
                 result.get().reply->type == REDIS_REPLY_BULK_STRING),
            "SCRIPT LOAD did not return a digest");
    const std::string digest = reply_text(result.get().reply);

    const char* keys[] = {lua_key};
    const char* arguments[] = {"7"};
    const int eval_status = redis_evalsha_result(
        client, digest.c_str(), 1, keys, 1, arguments, result.output());
    require(eval_status == TURBO_OK &&
                result.get().outcome == REDIS_COMMAND_REPLIED &&
                result.get().server_error == REDIS_SERVER_ERROR_NONE &&
                result.get().reply != nullptr &&
                result.get().reply->type == REDIS_REPLY_INTEGER &&
                result.get().reply->integer == 7,
            "EVALSHA did not return the Lua mutation result");
}

bool query_engine_available(redis_client_t* client)
{
    command_result result;
    execute(client, {"FT._LIST"}, result);
    if (result.get().server_error != REDIS_SERVER_ERROR_NONE)
        return false;
    require(result.get().reply != nullptr &&
                result.get().reply->type == REDIS_REPLY_ARRAY,
            "FT._LIST returned an invalid capability reply");
    return true;
}

class index_guard final {
public:
    explicit index_guard(redis_client_t* client) noexcept : client_(client) {}

    ~index_guard() noexcept
    {
        if (!created_)
            return;
        command_result result;
        try {
            execute(client_, {"FT.DROPINDEX", orm_index, "DD"}, result);
        } catch (...) {
        }
    }

    void create()
    {
        command_result result;
        execute(client_, {"FT.DROPINDEX", orm_index, "DD"}, result);
        execute_ok(client_,
                   {"FT.CREATE", orm_index, "ON", "HASH", "PREFIX", "1",
                    std::string(orm_key_prefix) + "{" + orm_table + "}:",
                    "SCHEMA", "id", "NUMERIC", "SORTABLE", "name", "TEXT",
                    "status", "TAG", "score", "NUMERIC", "SORTABLE",
                    "country", "TAG"},
                   result);
        created_ = true;
    }

private:
    redis_client_t* client_;
    bool created_ = false;
};

orm::config redis_orm_config(const live_report& report)
{
    orm::config configuration("redis");
    configuration.option("host", report.host)
        .option("port", std::to_string(report.port))
        .option("key_prefix", orm_key_prefix)
        .option("index_prefix", "turbodb_bdd_idx:")
        .option("timeout_ms", "2000")
        .option("command_timeout_ms", "2000");
    return configuration;
}

void wait_until_indexed(redis_client_t* client, coro_context_t* context)
{
    constexpr int attempts = 200;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        command_result result;
        execute_ok(client, {"FT.SEARCH", orm_index, "*", "LIMIT", "0", "0"}, result);
        const redis_reply_t* reply = result.get().reply;
        if (reply != nullptr && reply->type == REDIS_REPLY_ARRAY &&
            reply->element_count >= 1 && reply->elements[0] != nullptr &&
            reply->elements[0]->type == REDIS_REPLY_INTEGER &&
            reply->elements[0]->integer == 3)
            return;
        coro_sleep(context, 10);
    }
    fail("Redis Query Engine did not index the three ORM hashes within 2 seconds");
}

void verify_orm_query_engine(live_report& report, redis_client_t* client)
{
    index_guard index(client);
    index.create();

    orm::connection connection(redis_orm_config(report));
    require(connection.insert(orm_table)
                    .set("id", 1)
                    .set("name", "Alice")
                    .set("status", "active")
                    .set("score", 10.0)
                    .set("country", "US")
                    .execute()
                    .affected_rows() == 1,
            "first Redis ORM insert failed");
    require(connection.insert(orm_table)
                    .set("id", 2)
                    .set("name", "Bob")
                    .set("status", "inactive")
                    .set("score", 20.0)
                    .set("country", "UK")
                    .execute()
                    .affected_rows() == 1,
            "second Redis ORM insert failed");
    require(connection.insert(orm_table)
                    .set("id", 3)
                    .set("name", "Cara")
                    .set("status", "active")
                    .set("score", 30.0)
                    .set("country", "US")
                    .execute()
                    .affected_rows() == 1,
            "third Redis ORM insert failed");

    wait_until_indexed(client, report.context);

    auto search = connection.select(orm_table);
    const auto rows = search.column("id")
                          .column("name")
                          .column("score")
                          .where("status", orm::comparison::equal, "active")
                          .order_by("score", orm::sort_order::descending)
                          .execute();
    require(rows.rows() == 2 && rows.columns() == 3 &&
                rows.int64(0, 0) == 3 && rows.text(0, 1) == "Cara" &&
                rows.real(0, 2) == 30.0 && rows.int64(1, 0) == 1,
            "FT.SEARCH-backed ORM selection returned unexpected rows");
    report.orm_search_verified = true;

    auto aggregate = connection.select(orm_table);
    const auto groups = aggregate.column("country")
                            .aggregate(orm::aggregate_function::count_all, {}, "total")
                            .aggregate(orm::aggregate_function::sum, "score", "score_sum")
                            .group_by("country")
                            .execute();
    require(groups.rows() == 2 && groups.columns() == 3,
            "FT.AGGREGATE-backed ORM query returned an unexpected shape");
    bool found_us = false;
    bool found_uk = false;
    for (std::uint64_t row = 0; row < groups.rows(); ++row) {
        if (groups.text(row, 0) == "US") {
            found_us = groups.uint64(row, 1) == 2 && groups.real(row, 2) == 40.0;
        } else if (groups.text(row, 0) == "UK") {
            found_uk = groups.uint64(row, 1) == 1 && groups.real(row, 2) == 20.0;
        }
    }
    require(found_us && found_uk, "Redis ORM aggregate values were incorrect");
    report.orm_aggregate_verified = true;

    require(connection.update(orm_table)
                    .set("score", 25.0)
                    .where("id", orm::comparison::equal, 2)
                    .execute()
                    .affected_rows() == 1,
            "Redis ORM Lua update failed");
    require(connection.delete_from(orm_table)
                    .where("id", orm::comparison::equal, 3)
                    .execute()
                    .affected_rows() == 1,
            "Redis ORM delete failed");
    report.orm_mutations_verified = true;

    try {
        auto unsupported = connection.begin_transaction(
            orm::isolation_level::read_committed);
        (void)unsupported;
        fail("Redis accepted a non-serializable explicit transaction");
    } catch (const orm::status_error& error) {
        require(error.status() == ORM_STATUS_UNSUPPORTED,
                "Redis returned the wrong status for unsupported isolation");
    }

    auto transaction = connection.begin_transaction(
        orm::isolation_level::serializable);
    auto inserted = connection.insert(orm_table)
                        .set("id", 4)
                        .set("name", "Dora")
                        .set("status", "active")
                        .set("score", 40.0)
                        .set("country", "CA")
                        .execute(transaction);
    auto updated = connection.update(orm_table)
                       .set("score", 25.0)
                       .where("id", orm::comparison::equal, 2)
                       .execute(transaction);
    auto deferred_select = connection.select(orm_table)
                               .column("id")
                               .column("name")
                               .where("id", orm::comparison::equal, 4)
                               .execute(transaction);
    try {
        (void)inserted.affected_rows();
        fail("Redis transaction result was readable before EXEC");
    } catch (const orm::status_error& error) {
        require(error.status() == ORM_STATUS_INVALID_STATE,
                "Redis deferred result returned the wrong pre-commit status");
    }
    try {
        (void)connection.select(orm_table).column("id").execute();
        fail("Redis transaction was bypassed through the connection handle");
    } catch (const orm::status_error& error) {
        require(error.status() == ORM_STATUS_INVALID_STATE,
                "Redis transaction bypass returned the wrong status");
    }
    transaction.commit();
    require(inserted.affected_rows() == 1 && updated.affected_rows() == 1,
            "Redis EXEC returned incorrect mutation results");
    require(deferred_select.rows() == 1 && deferred_select.columns() == 2 &&
                deferred_select.int64(0, 0) == 4 &&
                deferred_select.text(0, 1) == "Dora",
            "Redis EXEC did not materialize the deferred query result");

    auto rolled_back = connection.begin_transaction(
        orm::isolation_level::serializable);
    auto discarded = connection.delete_from(orm_table)
                         .where("id", orm::comparison::equal, 1)
                         .execute(rolled_back);
    rolled_back.rollback();
    try {
        (void)discarded.affected_rows();
        fail("Redis rolled-back result remained readable");
    } catch (const orm::status_error& error) {
        require(error.status() == ORM_STATUS_INVALID_STATE,
                "Redis rolled-back result returned the wrong status");
    }

    report.orm_transaction_verified = true;
}

void verify_missing_query_engine_contract(live_report& report)
{
    try {
        orm::connection connection(redis_orm_config(report));
        (void)connection;
    } catch (const orm::status_error& error) {
        report.orm_rejected_missing_query_engine =
            error.status() == ORM_STATUS_UNSUPPORTED &&
            std::string(error.what()).find("FT._LIST") != std::string::npos;
        return;
    }
    fail("Redis ORM accepted a server without Query Engine support");
}

void live_redis_task(coro_t* coroutine, void* argument) noexcept
{
    (void)coroutine;
    auto& report = *static_cast<live_report*>(argument);
    try {
        report.stage = "connect Redis client";
        client_handle client(redis_client_create(report.host.c_str(), report.port));
        require(client != nullptr, "create Redis client failed");
        require(redis_client_connect(client.get(), nullptr, nullptr) == TURBO_OK,
                "connect Redis client failed");
        remove_test_keys(client.get());
        test_key_guard key_cleanup(client.get());

        report.stage = "execute ordered pipeline";
        redis_pool_config_t pool_config{};
        pool_config.master_host = report.host.c_str();
        pool_config.master_port = report.port;
        pool_config.min_connections = 1;
        pool_config.max_connections = 1;
        pool_config.connect_timeout_ms = 2000;
        pool_config.command_timeout_ms = 2000;
        pool_config.idle_timeout_ms = 60000;
        pool_config.health_check_ms = 30000;
        pool_config.pipeline_max = 100;
        pool_config.pipeline_timeout_ms = 10;
        pool_handle pool(redis_pool_create(&pool_config));
        require(pool != nullptr, "create Redis pool failed");
        require(redis_pool_start(pool.get()) == TURBO_OK, "start Redis pool failed");
        verify_pipeline(pool.get());
        report.pipeline_verified = true;

        report.stage = "execute cached Lua script";
        verify_lua(client.get());
        report.lua_verified = true;

        report.stage = "probe Redis Query Engine";
        report.query_engine_available = query_engine_available(client.get());
        if (report.query_engine_available) {
            report.stage = "execute Redis ORM search aggregate and mutations";
            verify_orm_query_engine(report, client.get());
        } else {
            report.stage = "verify Redis ORM Query Engine fail-fast contract";
            verify_missing_query_engine_contract(report);
        }

        report.stage = "clean test keys";
        remove_test_keys(client.get());
        report.stage = "complete";
    } catch (const std::exception& error) {
        report.error = error.what();
    } catch (...) {
        report.error = "unknown exception";
    }
}

bool live_tests_enabled()
{
    const char* value = std::getenv(live_test_environment);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

live_report live_configuration()
{
    live_report report;
    if (const char* host = std::getenv(host_environment); host != nullptr && host[0] != '\0')
        report.host = host;
    if (const char* port = std::getenv(port_environment); port != nullptr && port[0] != '\0') {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(port, &end, 10);
        if (end == port || *end != '\0' || parsed == 0 || parsed > 65535)
            fail("TURBODB_REDIS_PORT is not a valid TCP port");
        report.port = static_cast<std::uint16_t>(parsed);
    }
    return report;
}

} // namespace

spec("Redis ORM live integration") {
    (void)__bdd_config__;
    given("an explicitly enabled local Redis endpoint") {
        when("pipeline Lua and ORM capability contracts execute inside CoroNet") {
            then("preserves ordered results and matches Query Engine capability") {
                if (!live_tests_enabled()) {
                    info("set TURBODB_REDIS_LIVE=1 to run the live Redis BDD");
                    check(true);
                } else {
                    live_report report = live_configuration();
                    report.context = coro_context_create(nullptr);
                    check_not_null(report.context);
                    if (report.context != nullptr) {
                        const int spawn_status =
                            coro_context_spawn(report.context, live_redis_task, &report);
                        check_int_eq(spawn_status, TURBO_OK);
                        if (spawn_status == TURBO_OK)
                            coro_context_run(report.context, TURBO_RUN_DEFAULT);
                        coro_context_destroy(report.context);
                        report.context = nullptr;
                    }

                    if (!report.error.empty()) {
                        info("live Redis BDD failed at '%s': %s",
                             report.stage.c_str(), report.error.c_str());
                    }
                    check(report.error.empty());
                    check(report.pipeline_verified);
                    check(report.lua_verified);
                    if (report.query_engine_available) {
                        check(report.orm_search_verified);
                        check(report.orm_aggregate_verified);
                        check(report.orm_mutations_verified);
                        check(report.orm_transaction_verified);
                    } else {
                        info("Redis endpoint has no Query Engine; verified fail-fast contract");
                        check(report.orm_rejected_missing_query_engine);
                    }
                }
            }
        }
    }
}
