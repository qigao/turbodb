#include "query.hpp"

#include <redis_client.h>
#include <turbo_error.h>

#include <charconv>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orm_c_detail {
namespace {

constexpr std::uint16_t default_redis_port = 6379;
constexpr std::uint32_t default_redis_timeout_ms = 5000;
constexpr std::size_t default_redis_transaction_command_limit = 100;
constexpr std::size_t maximum_redis_transaction_command_limit = 65536;

constexpr const char insert_script[] =
    "if redis.call('EXISTS', KEYS[1]) ~= 0 then "
    "return redis.error_reply('ORM_DUPLICATE_KEY') end "
    "for i = 2, #ARGV, 2 do redis.call('HSET', KEYS[1], ARGV[i], ARGV[i + 1]) end "
    "if tonumber(ARGV[1]) > 0 then redis.call('EXPIRE', KEYS[1], ARGV[1]) end "
    "return 1";

constexpr const char update_script[] =
    "if redis.call('EXISTS', KEYS[1]) == 0 then return 0 end "
    "for i = 2, #ARGV, 3 do "
    "if ARGV[i] == 'set' then redis.call('HSET', KEYS[1], ARGV[i + 1], ARGV[i + 2]) "
    "else redis.call('HDEL', KEYS[1], ARGV[i + 1]) end end "
    "if tonumber(ARGV[1]) > 0 then redis.call('EXPIRE', KEYS[1], ARGV[1]) end "
    "return 1";

struct redis_client_deleter {
    void operator()(redis_client_t* client) const noexcept
    {
        redis_client_destroy(client);
    }
};

using redis_client_handle = std::unique_ptr<redis_client_t, redis_client_deleter>;

class command_result_guard final {
public:
    explicit command_result_guard(redis_command_result_t& result) noexcept
        : result_(&result)
    {
    }

    ~command_result_guard() noexcept
    {
        redis_command_result_clear(result_);
    }

    command_result_guard(const command_result_guard&) = delete;
    command_result_guard& operator=(const command_result_guard&) = delete;

private:
    redis_command_result_t* result_;
};

struct materialized_cell {
    bool is_null = true;
    std::string value;
};

class redis_result final : public result_backend {
public:
    redis_result(std::uint64_t rows,
                 std::uint64_t columns,
                 std::uint64_t affected_rows,
                 std::vector<materialized_cell> cells)
        : rows_(rows), columns_(columns), affected_rows_(affected_rows),
          cells_(std::move(cells))
    {
    }

    std::uint64_t rows() const noexcept override { return rows_; }
    std::uint64_t columns() const noexcept override { return columns_; }
    std::uint64_t affected_rows() const noexcept override { return affected_rows_; }

    bool is_null(std::uint64_t row, std::uint64_t column) const override
    {
        return get(row, column).is_null;
    }

    vstr cell(std::uint64_t row, std::uint64_t column) const override
    {
        const materialized_cell& selected = get(row, column);
        require(!selected.is_null, ORM_STATUS_NULL_VALUE,
                "Redis result cell is null");
        return vstr_from_buf(selected.value.data(), selected.value.size());
    }

private:
    const materialized_cell& get(std::uint64_t row, std::uint64_t column) const
    {
        require(row < rows_ && column < columns_, ORM_STATUS_OUT_OF_RANGE,
                "Redis result row or column is out of range");
        const std::uint64_t index = row * columns_ + column;
        require(index < cells_.size(), ORM_STATUS_INTERNAL_ERROR,
                "materialized Redis result has inconsistent dimensions");
        return cells_[static_cast<std::size_t>(index)];
    }

    std::uint64_t rows_;
    std::uint64_t columns_;
    std::uint64_t affected_rows_;
    std::vector<materialized_cell> cells_;
};

enum class deferred_result_status {
    pending,
    ready,
    unavailable
};

struct deferred_result_state {
    deferred_result_status status = deferred_result_status::pending;
    std::unique_ptr<result_backend> result;
    std::string message;
};

class redis_transaction_result final : public result_backend {
public:
    explicit redis_transaction_result(std::shared_ptr<deferred_result_state> state)
        : state_(std::move(state))
    {
    }

    std::uint64_t rows() const override { return resolved().rows(); }
    std::uint64_t columns() const override { return resolved().columns(); }
    std::uint64_t affected_rows() const override
    {
        return resolved().affected_rows();
    }

    bool is_null(std::uint64_t row, std::uint64_t column) const override
    {
        return resolved().is_null(row, column);
    }

    vstr cell(std::uint64_t row, std::uint64_t column) const override
    {
        return resolved().cell(row, column);
    }

private:
    const result_backend& resolved() const
    {
        require(state_ != nullptr, ORM_STATUS_INTERNAL_ERROR,
                "Redis transaction result state is null");
        if (state_->status == deferred_result_status::pending)
            fail(ORM_STATUS_INVALID_STATE,
                 "Redis transaction result is unavailable until commit");
        if (state_->status == deferred_result_status::unavailable)
            fail(ORM_STATUS_INVALID_STATE, state_->message);
        require(state_->result != nullptr, ORM_STATUS_INTERNAL_ERROR,
                "resolved Redis transaction result is null");
        return *state_->result;
    }

    std::shared_ptr<deferred_result_state> state_;
};

struct redis_settings {
    std::string host = "127.0.0.1";
    std::uint16_t port = default_redis_port;
    std::string username;
    std::string password;
    int database = 0;
    std::uint32_t timeout_ms = default_redis_timeout_ms;
    std::uint32_t command_timeout_ms = default_redis_timeout_ms;
    std::string id_column = "id";
    std::string key_prefix = "orm:";
    std::string index_prefix = "idx:";
    std::uint64_t ttl_seconds = 0;
    std::size_t transaction_command_limit =
        default_redis_transaction_command_limit;
};

template<typename Integer>
Integer parse_integer(std::string_view text, const char* role)
{
    Integer value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        fail(ORM_STATUS_INVALID_ARGUMENT,
             std::string(role) + " is not a valid integer");
    return value;
}

redis_settings parse_settings(const std::vector<std::string>& keywords,
                              const std::vector<std::string>& values)
{
    require(keywords.size() == values.size(), ORM_STATUS_INTERNAL_ERROR,
            "Redis option arrays have different sizes");
    redis_settings settings;
    for (std::size_t index = 0; index < keywords.size(); ++index) {
        const std::string& key = keywords[index];
        const std::string& value = values[index];
        if (key == "host") {
            require(!value.empty(), ORM_STATUS_INVALID_ARGUMENT,
                    "Redis host is empty");
            settings.host = value;
        } else if (key == "port") {
            const unsigned parsed = parse_integer<unsigned>(value, "Redis port");
            require(parsed > 0 && parsed <= std::numeric_limits<std::uint16_t>::max(),
                    ORM_STATUS_INVALID_ARGUMENT, "Redis port is out of range");
            settings.port = static_cast<std::uint16_t>(parsed);
        } else if (key == "username") {
            settings.username = value;
        } else if (key == "password") {
            settings.password = value;
        } else if (key == "database") {
            settings.database = parse_integer<int>(value, "Redis database");
            require(settings.database >= 0 && settings.database <= 15,
                    ORM_STATUS_INVALID_ARGUMENT,
                    "Redis database must be in [0, 15]");
        } else if (key == "timeout_ms") {
            settings.timeout_ms = parse_integer<std::uint32_t>(value, "Redis timeout_ms");
            require(settings.timeout_ms != 0, ORM_STATUS_INVALID_ARGUMENT,
                    "Redis timeout_ms must be positive");
        } else if (key == "command_timeout_ms") {
            settings.command_timeout_ms =
                parse_integer<std::uint32_t>(value, "Redis command_timeout_ms");
            require(settings.command_timeout_ms != 0, ORM_STATUS_INVALID_ARGUMENT,
                    "Redis command_timeout_ms must be positive");
        } else if (key == "id_column") {
            require(!value.empty(), ORM_STATUS_INVALID_ARGUMENT,
                    "Redis id_column is empty");
            settings.id_column = value;
        } else if (key == "key_prefix") {
            require(!value.empty(), ORM_STATUS_INVALID_ARGUMENT,
                    "Redis key_prefix is empty");
            settings.key_prefix = value;
        } else if (key == "index_prefix") {
            require(!value.empty(), ORM_STATUS_INVALID_ARGUMENT,
                    "Redis index_prefix is empty");
            settings.index_prefix = value;
        } else if (key == "ttl_seconds") {
            settings.ttl_seconds =
                parse_integer<std::uint64_t>(value, "Redis ttl_seconds");
        } else if (key == "transaction_command_limit") {
            settings.transaction_command_limit =
                parse_integer<std::size_t>(value,
                                           "Redis transaction_command_limit");
            require(settings.transaction_command_limit > 0 &&
                        settings.transaction_command_limit <=
                            maximum_redis_transaction_command_limit,
                    ORM_STATUS_INVALID_ARGUMENT,
                    "Redis transaction_command_limit must be in [1, 65536]");
        } else {
            fail(ORM_STATUS_INVALID_ARGUMENT,
                 "unknown Redis connection option: " + key);
        }
    }
    require(settings.username.empty() || !settings.password.empty(),
            ORM_STATUS_INVALID_ARGUMENT,
            "Redis username requires a password");
    return settings;
}

std::string outcome_name(redis_command_outcome_t outcome)
{
    switch (outcome) {
    case REDIS_COMMAND_NOT_SENT: return "not sent";
    case REDIS_COMMAND_SEND_UNCERTAIN: return "send uncertain";
    case REDIS_COMMAND_REPLY_UNKNOWN: return "reply unknown";
    case REDIS_COMMAND_REPLIED: return "replied";
    default: return "invalid outcome";
    }
}

std::string reply_text(const redis_reply_t* reply)
{
    if (reply == nullptr || reply->str == nullptr)
        return {};
    return std::string(reply->str, reply->len);
}

std::string parameter_text(const bound_parameter& parameter)
{
    switch (parameter.kind) {
    case ORM_VALUE_BOOLEAN:
        return parameter.boolean_value ? "1" : "0";
    case ORM_VALUE_INT64:
    case ORM_VALUE_UINT64:
    case ORM_VALUE_TEXT:
        return parameter.text;
    case ORM_VALUE_DOUBLE:
        return format_redis_double(parameter.double_value);
    case ORM_VALUE_BLOB:
        return parameter.binary;
    case ORM_VALUE_NULL:
        return {};
    default:
        fail(ORM_STATUS_INTERNAL_ERROR, "unknown Redis parameter type");
    }
}

const redis_reply_t* find_field(const redis_reply_t& row, std::string_view name)
{
    require(row.type == REDIS_REPLY_ARRAY && row.element_count % 2 == 0,
            ORM_STATUS_DATASTORE_ERROR,
            "Redis Query Engine row is not a field/value array");
    for (std::size_t index = 0; index < row.element_count; index += 2) {
        const redis_reply_t* field = row.elements[index];
        require(field != nullptr &&
                    (field->type == REDIS_REPLY_STRING ||
                     field->type == REDIS_REPLY_BULK_STRING),
                ORM_STATUS_DATASTORE_ERROR,
                "Redis Query Engine field name has an invalid type");
        if (field->len == name.size() && field->str != nullptr &&
            std::string_view(field->str, field->len) == name)
            return row.elements[index + 1];
    }
    return nullptr;
}

materialized_cell materialize(const redis_reply_t* reply)
{
    materialized_cell cell;
    if (reply == nullptr || reply->type == REDIS_REPLY_NULL)
        return cell;
    cell.is_null = false;
    if (reply->type == REDIS_REPLY_INTEGER) {
        cell.value = std::to_string(reply->integer);
    } else if (reply->type == REDIS_REPLY_STRING ||
               reply->type == REDIS_REPLY_BULK_STRING) {
        require(reply->str != nullptr || reply->len == 0,
                ORM_STATUS_DATASTORE_ERROR,
                "Redis reply string has a null data pointer");
        if (reply->len != 0)
            cell.value.assign(reply->str, reply->len);
    } else {
        fail(ORM_STATUS_DATASTORE_ERROR,
             "Redis Query Engine value has an unsupported reply type");
    }
    return cell;
}

class redis_backend final : public database_backend {
    struct scripted_mutation {
        const char* script;
        std::vector<std::string> keys;
        std::vector<std::string> args;
    };

    struct pending_command {
        query_kind kind;
        redis_query_command query;
        connection_limits limits;
        std::shared_ptr<deferred_result_state> result;
    };

    class explicit_transaction final : public transaction_backend {
    public:
        explicit_transaction(redis_backend& owner, orm_isolation_t isolation)
            : owner_(owner)
        {
            require(isolation == ORM_ISOLATION_SERIALIZABLE,
                    ORM_STATUS_UNSUPPORTED,
                    "Redis explicit transactions support only serializable isolation");
            pending_.reserve(owner_.settings_.transaction_command_limit);
            owner_.start_transaction();
            active_ = true;
        }

        ~explicit_transaction() override
        {
            rollback_noexcept();
        }

        std::unique_ptr<result_backend>
        execute_plan(const query_plan& plan,
                     const connection_limits& limits) override
        {
            require(active_, ORM_STATUS_INVALID_STATE,
                    "Redis transaction is no longer active");
            require(pending_.size() < owner_.settings_.transaction_command_limit,
                    ORM_STATUS_LIMIT_EXCEEDED,
                    "Redis transaction command count exceeds the configured pipeline limit");
            redis_query_command command = owner_.transaction_command(plan, limits);
            auto state = std::make_shared<deferred_result_state>();
            auto exposed =
                std::make_unique<redis_transaction_result>(state);
            pending_.push_back(
                pending_command{plan.kind, std::move(command), limits, state});
            try {
                redis_command_result_t queued = owner_.execute(
                    pending_.back().query.arguments,
                    plan.kind != query_kind::select);
                command_result_guard guard(queued);
                require(queued.reply != nullptr &&
                            queued.reply->type == REDIS_REPLY_STRING &&
                            reply_text(queued.reply) == "QUEUED",
                        ORM_STATUS_DATASTORE_ERROR,
                        "Redis transaction command was not queued");
            } catch (...) {
                pending_.pop_back();
                throw;
            }
            return exposed;
        }

        void commit() override
        {
            require(active_, ORM_STATUS_INVALID_STATE,
                    "Redis transaction is no longer active");
            redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
            try {
                result = owner_.execute({"EXEC"}, true);
            } catch (...) {
                finish_native_transaction();
                reject_pending(
                    "Redis transaction outcome is unknown because EXEC did not return a reply");
                throw;
            }
            command_result_guard guard(result);
            finish_native_transaction();
            try {
                require(result.reply != nullptr &&
                            result.reply->type == REDIS_REPLY_ARRAY,
                        ORM_STATUS_DATASTORE_ERROR,
                        "Redis EXEC returned an invalid reply");
                require(result.reply->element_count == pending_.size(),
                        ORM_STATUS_DATASTORE_ERROR,
                        "Redis EXEC reply count does not match the queued command count");

                std::string first_error;
                for (std::size_t index = 0; index < pending_.size(); ++index) {
                    pending_command& pending = pending_[index];
                    const redis_reply_t* reply = result.reply->elements[index];
                    if (redis_server_error_classify(reply) != REDIS_SERVER_ERROR_NONE) {
                        pending.result->status = deferred_result_status::unavailable;
                        pending.result->message =
                            "Redis transaction command failed during EXEC: " +
                            reply_text(reply);
                        if (first_error.empty())
                            first_error = pending.result->message;
                        continue;
                    }
                    pending.result->result = owner_.materialize_transaction_reply(
                        pending, reply);
                    pending.result->status = deferred_result_status::ready;
                }
                if (!first_error.empty())
                    fail(ORM_STATUS_DATASTORE_ERROR,
                         first_error +
                         "; Redis does not roll back other commands executed by EXEC");
            } catch (...) {
                reject_pending(
                    "Redis transaction result is unavailable because EXEC processing failed");
                throw;
            }
        }

        void rollback() override
        {
            require(active_, ORM_STATUS_INVALID_STATE,
                    "Redis transaction is no longer active");
            redis_command_result_t result = owner_.execute({"DISCARD"}, false);
            command_result_guard guard(result);
            require(result.reply != nullptr && result.reply->type == REDIS_REPLY_STRING &&
                        reply_text(result.reply) == "OK",
                    ORM_STATUS_DATASTORE_ERROR,
                    "Redis DISCARD returned an invalid reply");
            finish_native_transaction();
            reject_pending("Redis transaction result was discarded by rollback");
        }

    private:
        void reject_pending(const char* message) noexcept
        {
            for (pending_command& pending : pending_) {
                if (pending.result->status == deferred_result_status::pending) {
                    pending.result->status = deferred_result_status::unavailable;
                    pending.result->message = message;
                }
            }
        }

        void finish_native_transaction() noexcept
        {
            active_ = false;
            owner_.transaction_active_ = false;
        }

        void rollback_noexcept() noexcept
        {
            if (!active_)
                return;
            try {
                redis_command_result_t result = owner_.execute({"DISCARD"}, false);
                redis_command_result_clear(&result);
            } catch (...) {
            }
            finish_native_transaction();
            reject_pending("Redis transaction result was discarded when the transaction ended");
        }

        redis_backend& owner_;
        std::vector<pending_command> pending_;
        bool active_ = false;
    };

public:
    redis_backend(const std::vector<std::string>& keywords,
                  const std::vector<std::string>& values,
                  const connection_limits& limits)
        : settings_(parse_settings(keywords, values))
    {
        redis_config_t config{};
        config.host = settings_.host.c_str();
        config.port = settings_.port;
        config.username = settings_.username.empty() ? nullptr : settings_.username.c_str();
        config.password = settings_.password.empty() ? nullptr : settings_.password.c_str();
        config.database = settings_.database;
        config.timeout_ms = settings_.timeout_ms;
        config.command_timeout_ms = settings_.command_timeout_ms;
        config.max_pipeline = settings_.transaction_command_limit;
        client_.reset(redis_client_create_with_config(&config));
        require(client_ != nullptr, ORM_STATUS_OUT_OF_MEMORY,
                "create Redis client failed");
        const int status = redis_client_connect(client_.get(), nullptr, nullptr);
        if (status != TURBO_OK) {
            if (status == TURBO_EINVAL)
                fail(ORM_STATUS_CONNECTION_ERROR,
                     "Redis ORM connections must be created inside an active CoroNet coroutine");
            fail(ORM_STATUS_CONNECTION_ERROR,
                 "connect Redis failed with status " + std::to_string(status));
        }
        verify_query_engine(limits);
    }

    execution_model model() const noexcept override
    {
        return execution_model::native_plan;
    }

    std::string placeholder(std::size_t) const override
    {
        fail(ORM_STATUS_UNSUPPORTED,
             "Redis native query plans do not use SQL placeholders");
    }

    std::string pagination(std::optional<std::uint64_t>,
                           std::optional<std::uint64_t>) const override
    {
        fail(ORM_STATUS_UNSUPPORTED,
             "Redis native query plans do not use SQL pagination");
    }

    std::unique_ptr<result_backend>
    execute_sql(std::string_view,
                const std::vector<bound_parameter>&,
                bool,
                const connection_limits&) override
    {
        fail(ORM_STATUS_UNSUPPORTED,
             "raw SQL is not supported by the Redis backend");
    }

    std::unique_ptr<result_backend>
    execute_plan(const query_plan& plan,
                 const connection_limits& limits) override
    {
        require(!transaction_active_, ORM_STATUS_INVALID_STATE,
                "execute Redis transaction queries through the transaction handle");
        switch (plan.kind) {
        case query_kind::select:
            return execute_query(plan, limits);
        case query_kind::insert:
            return execute_insert(plan);
        case query_kind::update:
            return execute_update(plan);
        case query_kind::remove:
            return execute_delete(plan);
        case query_kind::raw:
            fail(ORM_STATUS_UNSUPPORTED,
                 "raw SQL is not supported by the Redis backend");
        default:
            fail(ORM_STATUS_INTERNAL_ERROR, "unknown Redis query plan kind");
        }
    }

    std::unique_ptr<transaction_backend>
    begin_transaction(orm_isolation_t isolation) override
    {
        require(!transaction_active_, ORM_STATUS_INVALID_STATE,
                "Redis connection already has an active transaction");
        return std::make_unique<explicit_transaction>(*this, isolation);
    }

private:
    void start_transaction()
    {
        redis_command_result_t result = execute({"MULTI"}, false);
        command_result_guard guard(result);
        require(result.reply != nullptr && result.reply->type == REDIS_REPLY_STRING &&
                    reply_text(result.reply) == "OK",
                ORM_STATUS_DATASTORE_ERROR,
                "Redis MULTI returned an invalid reply");
        transaction_active_ = true;
    }

    redis_query_command transaction_command(const query_plan& plan,
                                            const connection_limits& limits)
    {
        redis_query_command command;
        switch (plan.kind) {
        case query_kind::select:
            return build_redis_query_command(plan, limits, settings_.index_prefix);
        case query_kind::insert:
        case query_kind::update: {
            const scripted_mutation mutation = plan.kind == query_kind::insert
                ? build_insert_mutation(plan)
                : build_update_mutation(plan);
            command.arguments = {"EVAL", mutation.script,
                                 std::to_string(mutation.keys.size())};
            command.arguments.insert(command.arguments.end(),
                                     mutation.keys.begin(), mutation.keys.end());
            command.arguments.insert(command.arguments.end(),
                                     mutation.args.begin(), mutation.args.end());
            return command;
        }
        case query_kind::remove: {
            const predicate& id = require_id_predicate(plan);
            command.arguments = {"DEL", entity_key(plan.table, id.parameter)};
            return command;
        }
        case query_kind::raw:
            fail(ORM_STATUS_UNSUPPORTED,
                 "raw SQL is not supported by Redis transactions");
        default:
            fail(ORM_STATUS_INTERNAL_ERROR,
                 "unknown Redis transaction query plan kind");
        }
    }

    std::unique_ptr<result_backend>
    materialize_transaction_reply(const pending_command& pending,
                                  const redis_reply_t* reply)
    {
        if (pending.kind == query_kind::select)
            return materialize_query_reply(reply, pending.query, pending.limits);
        require(reply != nullptr && reply->type == REDIS_REPLY_INTEGER &&
                    reply->integer >= 0,
                ORM_STATUS_DATASTORE_ERROR,
                "Redis transaction mutation returned an invalid result");
        return std::make_unique<redis_result>(
            0, 0, static_cast<std::uint64_t>(reply->integer),
            std::vector<materialized_cell>{});
    }

    redis_command_result_t execute(const std::vector<std::string>& arguments,
                                   bool mutation)
    {
        std::vector<const char*> argv;
        std::vector<std::size_t> lengths;
        argv.reserve(arguments.size());
        lengths.reserve(arguments.size());
        for (const std::string& argument : arguments) {
            argv.push_back(argument.data());
            lengths.push_back(argument.size());
        }
        redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
        (void)redis_commandv_result(client_.get(), static_cast<int>(argv.size()),
                                    argv.data(), lengths.data(), &result);
        if (result.outcome != REDIS_COMMAND_REPLIED) {
            const std::string suffix = mutation &&
                    (result.outcome == REDIS_COMMAND_SEND_UNCERTAIN ||
                     result.outcome == REDIS_COMMAND_REPLY_UNKNOWN)
                ? "; mutation outcome is unknown and must not be retried blindly"
                : "";
            const std::string message =
                "Redis command " + outcome_name(result.outcome) +
                " with status " + std::to_string(result.status) + suffix;
            redis_command_result_clear(&result);
            fail(ORM_STATUS_CONNECTION_ERROR, message);
        }
        if (result.server_error != REDIS_SERVER_ERROR_NONE) {
            const std::string message = "Redis server error: " + reply_text(result.reply);
            redis_command_result_clear(&result);
            fail(ORM_STATUS_DATASTORE_ERROR, message);
        }
        return result;
    }

    void verify_query_engine(const connection_limits&)
    {
        redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
        const std::vector<std::string> command{"FT._LIST"};
        try {
            result = execute(command, false);
        } catch (const status_error& error) {
            if (error.status() == ORM_STATUS_DATASTORE_ERROR)
                fail(ORM_STATUS_UNSUPPORTED,
                     "Redis Query Engine is required but FT._LIST is unavailable");
            throw;
        }
        command_result_guard guard(result);
        require(result.reply != nullptr && result.reply->type == REDIS_REPLY_ARRAY,
                ORM_STATUS_DATASTORE_ERROR,
                "FT._LIST returned an invalid reply");
    }

    std::unique_ptr<result_backend>
    execute_query(const query_plan& plan, const connection_limits& limits)
    {
        const redis_query_command command =
            build_redis_query_command(plan, limits, settings_.index_prefix);
        redis_command_result_t result = execute(command.arguments, false);
        command_result_guard guard(result);
        return materialize_query_reply(result.reply, command, limits);
    }

    std::unique_ptr<result_backend>
    materialize_query_reply(const redis_reply_t* reply,
                            const redis_query_command& command,
                            const connection_limits& limits)
    {
        require(reply != nullptr && reply->type == REDIS_REPLY_ARRAY &&
                    reply->element_count >= 1,
                    ORM_STATUS_DATASTORE_ERROR,
                    "Redis Query Engine returned an invalid top-level reply");
        const redis_reply_t* count = reply->elements[0];
        require(count != nullptr && count->type == REDIS_REPLY_INTEGER &&
                    count->integer >= 0,
                ORM_STATUS_DATASTORE_ERROR,
                "Redis Query Engine returned an invalid row count");

        const std::size_t stride = command.aggregate ? 1 : 2;
        require((reply->element_count - 1) % stride == 0,
                ORM_STATUS_DATASTORE_ERROR,
                "Redis Query Engine returned an invalid row layout");
        const std::size_t rows = (reply->element_count - 1) / stride;
        require(rows <= limits.max_result_rows,
                ORM_STATUS_LIMIT_EXCEEDED,
                "Redis result row count exceeds max_result_rows");
        require(command.output_columns.size() <= limits.max_columns,
                ORM_STATUS_LIMIT_EXCEEDED,
                "Redis result column count exceeds max_columns");

        const std::size_t columns = command.output_columns.size();
        require(columns == 0 || rows <=
                    std::numeric_limits<std::size_t>::max() / columns,
                ORM_STATUS_LIMIT_EXCEEDED,
                "Redis result cell count overflows size_t");
        const std::size_t cell_count = rows * columns;
        require(cell_count <=
                    std::numeric_limits<std::uint64_t>::max() /
                        sizeof(materialized_cell),
                ORM_STATUS_LIMIT_EXCEEDED,
                "Redis result metadata byte count overflows uint64_t");
        std::vector<materialized_cell> cells;
        cells.reserve(cell_count);
        std::uint64_t retained = static_cast<std::uint64_t>(cell_count) *
                                 sizeof(materialized_cell);
        require(retained <= limits.max_result_bytes,
                ORM_STATUS_LIMIT_EXCEEDED,
                "Redis result metadata exceeds max_result_bytes");

        for (std::size_t row_index = 0; row_index < rows; ++row_index) {
            const std::size_t reply_index = 1 + row_index * stride +
                                            (command.aggregate ? 0 : 1);
            const redis_reply_t* row = reply->elements[reply_index];
            require(row != nullptr, ORM_STATUS_DATASTORE_ERROR,
                    "Redis Query Engine returned a null row");
            for (const std::string& column : command.output_columns) {
                materialized_cell cell = materialize(find_field(*row, column));
                require(cell.value.size() <= limits.max_result_bytes - retained,
                        ORM_STATUS_LIMIT_EXCEEDED,
                        "Redis result payload exceeds max_result_bytes");
                retained += cell.value.size();
                cells.push_back(std::move(cell));
            }
        }
        return std::make_unique<redis_result>(
            static_cast<std::uint64_t>(rows),
            static_cast<std::uint64_t>(columns), 0, std::move(cells));
    }

    const predicate& require_id_predicate(const query_plan& plan) const
    {
        require(plan.where_root.children.size() == 1 &&
                    !plan.where_root.children.front()->is_group,
                ORM_STATUS_UNSUPPORTED,
                "Redis UPDATE/DELETE requires exactly one id equality predicate");
        const predicate& id = plan.where_root.children.front()->value;
        require(id.column == settings_.id_column &&
                    id.comparison == ORM_COMPARE_EQUAL && id.has_parameter,
                ORM_STATUS_UNSUPPORTED,
                "Redis UPDATE/DELETE requires a non-null id equality predicate");
        return id;
    }

    std::string entity_key(std::string_view table,
                           const bound_parameter& id) const
    {
        const std::string encoded = parameter_text(id);
        require(!encoded.empty(), ORM_STATUS_INVALID_ARGUMENT,
                "Redis entity id is empty");
        return settings_.key_prefix + "{" + std::string(table) + "}:" + encoded;
    }

    std::uint64_t run_script(std::string& sha,
                             const char* script,
                             const std::vector<std::string>& keys,
                             const std::vector<std::string>& args)
    {
        if (sha.empty())
            sha = load_script(script);

        auto invoke = [&]() {
            std::vector<const char*> key_views;
            std::vector<const char*> arg_views;
            key_views.reserve(keys.size());
            arg_views.reserve(args.size());
            for (const std::string& key : keys) key_views.push_back(key.data());
            for (const std::string& arg : args) arg_views.push_back(arg.data());
            redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
            (void)redis_evalsha_result(client_.get(), sha.c_str(),
                                       static_cast<int>(key_views.size()), key_views.data(),
                                       static_cast<int>(arg_views.size()), arg_views.data(),
                                       &result);
            return result;
        };

        redis_command_result_t result = invoke();
        command_result_guard guard(result);
        if (result.outcome == REDIS_COMMAND_REPLIED &&
            result.server_error == REDIS_SERVER_ERROR_NO_SCRIPT) {
            redis_command_result_clear(&result);
            sha = load_script(script);
            result = invoke();
        }
        if (result.outcome != REDIS_COMMAND_REPLIED) {
            std::string message =
                "Redis Lua mutation " + outcome_name(result.outcome);
            if (result.outcome == REDIS_COMMAND_SEND_UNCERTAIN ||
                result.outcome == REDIS_COMMAND_REPLY_UNKNOWN)
                message += "; mutation outcome is unknown and must not be retried blindly";
            fail(ORM_STATUS_CONNECTION_ERROR, message);
        }
        if (result.server_error != REDIS_SERVER_ERROR_NONE) {
            const std::string message = "Redis Lua error: " + reply_text(result.reply);
            fail(ORM_STATUS_DATASTORE_ERROR, message);
        }
        require(result.reply != nullptr && result.reply->type == REDIS_REPLY_INTEGER &&
                    result.reply->integer >= 0,
                ORM_STATUS_DATASTORE_ERROR,
                "Redis Lua mutation returned a non-integer result");
        const std::uint64_t affected =
            static_cast<std::uint64_t>(result.reply->integer);
        return affected;
    }

    std::string load_script(const char* script)
    {
        redis_command_result_t result = REDIS_COMMAND_RESULT_INIT;
        command_result_guard guard(result);
        (void)redis_script_load_result(client_.get(), script, &result);
        if (result.outcome != REDIS_COMMAND_REPLIED) {
            const std::string message =
                "Redis SCRIPT LOAD " + outcome_name(result.outcome);
            fail(ORM_STATUS_CONNECTION_ERROR, message);
        }
        if (result.server_error != REDIS_SERVER_ERROR_NONE) {
            const std::string message = "Redis SCRIPT LOAD error: " + reply_text(result.reply);
            fail(ORM_STATUS_DATASTORE_ERROR, message);
        }
        require(result.reply != nullptr &&
                    (result.reply->type == REDIS_REPLY_STRING ||
                     result.reply->type == REDIS_REPLY_BULK_STRING),
                ORM_STATUS_DATASTORE_ERROR,
                "Redis SCRIPT LOAD returned an invalid digest");
        std::string sha = reply_text(result.reply);
        return sha;
    }

    std::unique_ptr<result_backend> execute_insert(const query_plan& plan)
    {
        const scripted_mutation mutation = build_insert_mutation(plan);
        const std::uint64_t affected = run_script(
            insert_sha_, mutation.script, mutation.keys, mutation.args);
        return std::make_unique<redis_result>(0, 0, affected,
                                              std::vector<materialized_cell>{});
    }

    scripted_mutation build_insert_mutation(const query_plan& plan) const
    {
        require(!plan.assignments.empty(), ORM_STATUS_INVALID_STATE,
                "Redis INSERT has no values");
        const assignment* id = nullptr;
        std::vector<std::string> args{std::to_string(settings_.ttl_seconds)};
        for (const assignment& value : plan.assignments) {
            if (value.column == settings_.id_column) {
                require(value.has_parameter, ORM_STATUS_INVALID_ARGUMENT,
                        "Redis entity id cannot be null");
                id = &value;
            }
            if (!value.has_parameter)
                continue;
            args.push_back(value.column);
            args.push_back(parameter_text(value.parameter));
        }
        require(id != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "Redis INSERT requires the configured id column");
        return scripted_mutation{
            insert_script, {entity_key(plan.table, id->parameter)}, std::move(args)};
    }

    std::unique_ptr<result_backend> execute_update(const query_plan& plan)
    {
        const scripted_mutation mutation = build_update_mutation(plan);
        const std::uint64_t affected = run_script(
            update_sha_, mutation.script, mutation.keys, mutation.args);
        return std::make_unique<redis_result>(0, 0, affected,
                                              std::vector<materialized_cell>{});
    }

    scripted_mutation build_update_mutation(const query_plan& plan) const
    {
        require(!plan.assignments.empty(), ORM_STATUS_INVALID_STATE,
                "Redis UPDATE has no assignments");
        const predicate& id = require_id_predicate(plan);
        std::vector<std::string> args{std::to_string(settings_.ttl_seconds)};
        for (const assignment& value : plan.assignments) {
            require(value.column != settings_.id_column, ORM_STATUS_UNSUPPORTED,
                    "Redis UPDATE cannot change the configured id column");
            args.push_back(value.has_parameter ? "set" : "delete");
            args.push_back(value.column);
            args.push_back(value.has_parameter ? parameter_text(value.parameter) : std::string{});
        }
        return scripted_mutation{
            update_script, {entity_key(plan.table, id.parameter)}, std::move(args)};
    }

    std::unique_ptr<result_backend> execute_delete(const query_plan& plan)
    {
        const predicate& id = require_id_predicate(plan);
        redis_command_result_t result = execute(
            {"DEL", entity_key(plan.table, id.parameter)}, true);
        command_result_guard guard(result);
        require(result.reply != nullptr && result.reply->type == REDIS_REPLY_INTEGER &&
                    result.reply->integer >= 0,
                ORM_STATUS_DATASTORE_ERROR,
                "Redis DEL returned an invalid result");
        const std::uint64_t affected =
            static_cast<std::uint64_t>(result.reply->integer);
        return std::make_unique<redis_result>(0, 0, affected,
                                              std::vector<materialized_cell>{});
    }

    redis_settings settings_;
    redis_client_handle client_;
    std::string insert_sha_;
    std::string update_sha_;
    bool transaction_active_ = false;
};

} // namespace

std::unique_ptr<database_backend>
make_redis_backend(const std::vector<std::string>& keywords,
                   const std::vector<std::string>& values,
                   const connection_limits& limits)
{
    return std::make_unique<redis_backend>(keywords, values, limits);
}

} // namespace orm_c_detail
