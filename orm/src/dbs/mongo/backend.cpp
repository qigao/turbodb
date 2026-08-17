#include "query.hpp"

#include <mongoc/mongoc.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orm_c_detail {
namespace {

constexpr std::uint32_t default_server_selection_timeout_ms = 5000;
constexpr const char* transaction_probe_collection = "__orm_transaction_probe";
constexpr std::int32_t mongo_illegal_operation_code = 20;

struct client_deleter {
    void operator()(mongoc_client_t* client) const noexcept
    {
        mongoc_client_destroy(client);
    }
};

struct database_deleter {
    void operator()(mongoc_database_t* database) const noexcept
    {
        mongoc_database_destroy(database);
    }
};

struct collection_deleter {
    void operator()(mongoc_collection_t* collection) const noexcept
    {
        mongoc_collection_destroy(collection);
    }
};

struct cursor_deleter {
    void operator()(mongoc_cursor_t* cursor) const noexcept
    {
        mongoc_cursor_destroy(cursor);
    }
};

struct session_deleter {
    void operator()(mongoc_client_session_t* session) const noexcept
    {
        mongoc_client_session_destroy(session);
    }
};

struct read_concern_deleter {
    void operator()(mongoc_read_concern_t* concern) const noexcept
    {
        mongoc_read_concern_destroy(concern);
    }
};

struct write_concern_deleter {
    void operator()(mongoc_write_concern_t* concern) const noexcept
    {
        mongoc_write_concern_destroy(concern);
    }
};

struct transaction_opts_deleter {
    void operator()(mongoc_transaction_opt_t* options) const noexcept
    {
        mongoc_transaction_opts_destroy(options);
    }
};

using client_handle = std::unique_ptr<mongoc_client_t, client_deleter>;
using database_handle = std::unique_ptr<mongoc_database_t, database_deleter>;
using collection_handle =
    std::unique_ptr<mongoc_collection_t, collection_deleter>;
using cursor_handle = std::unique_ptr<mongoc_cursor_t, cursor_deleter>;
using session_handle =
    std::unique_ptr<mongoc_client_session_t, session_deleter>;
using read_concern_handle =
    std::unique_ptr<mongoc_read_concern_t, read_concern_deleter>;
using write_concern_handle =
    std::unique_ptr<mongoc_write_concern_t, write_concern_deleter>;
using transaction_opts_handle =
    std::unique_ptr<mongoc_transaction_opt_t, transaction_opts_deleter>;

class bson_guard final {
public:
    bson_guard() noexcept
    {
        bson_init(&document_);
    }

    ~bson_guard() noexcept
    {
        bson_destroy(&document_);
    }

    bson_guard(const bson_guard&) = delete;
    bson_guard& operator=(const bson_guard&) = delete;

    bson_t* get() noexcept
    {
        return &document_;
    }

private:
    bson_t document_;
};

struct materialized_cell {
    bool is_null = true;
    std::string value;
};

class mongo_result final : public result_backend {
public:
    mongo_result(std::uint64_t rows,
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

    tstr_v cell(std::uint64_t row, std::uint64_t column) const override
    {
        const materialized_cell& selected = get(row, column);
        require(!selected.is_null, ORM_STATUS_NULL_VALUE,
                "MongoDB result cell is null");
        return tstr_v_from_buf(selected.value.data(), selected.value.size());
    }

private:
    const materialized_cell& get(std::uint64_t row, std::uint64_t column) const
    {
        require(row < rows_ && column < columns_, ORM_STATUS_OUT_OF_RANGE,
                "MongoDB result row or column is out of range");
        const std::uint64_t index = row * columns_ + column;
        require(index < cells_.size(), ORM_STATUS_INTERNAL_ERROR,
                "materialized MongoDB result has inconsistent dimensions");
        return cells_[static_cast<std::size_t>(index)];
    }

    std::uint64_t rows_;
    std::uint64_t columns_;
    std::uint64_t affected_rows_;
    std::vector<materialized_cell> cells_;
};

template<typename Integer>
Integer parse_integer(std::string_view text, const char* role)
{
    Integer value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                        value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        fail(ORM_STATUS_INVALID_ARGUMENT,
             std::string(role) + " is not a valid integer");
    return value;
}

bool valid_identifier(std::string_view value) noexcept
{
    if (value.empty() || value.size() > 63)
        return false;
    const auto start = [](char next) {
        return (next >= 'a' && next <= 'z') ||
               (next >= 'A' && next <= 'Z') || next == '_';
    };
    if (!start(value.front()))
        return false;
    return std::all_of(value.begin() + 1, value.end(), [&](char next) {
        return start(next) || (next >= '0' && next <= '9');
    });
}

mongo_settings parse_settings(const std::vector<std::string>& keywords,
                              const std::vector<std::string>& values)
{
    require(keywords.size() == values.size(), ORM_STATUS_INTERNAL_ERROR,
            "MongoDB option arrays have different sizes");
    mongo_settings settings;
    bool has_database = false;
    for (std::size_t index = 0; index < keywords.size(); ++index) {
        const std::string& key = keywords[index];
        const std::string& value = values[index];
        if (key == "uri") {
            require(!value.empty(), ORM_STATUS_INVALID_ARGUMENT,
                    "MongoDB uri is empty");
            require(value.size() <= 4096, ORM_STATUS_LIMIT_EXCEEDED,
                    "MongoDB uri exceeds its byte limit");
            settings.uri = value;
        } else if (key == "database") {
            require(valid_identifier(value), ORM_STATUS_INVALID_ARGUMENT,
                    "MongoDB database is not a valid identifier");
            settings.database = value;
            has_database = true;
        } else if (key == "id_column") {
            require(valid_identifier(value), ORM_STATUS_INVALID_ARGUMENT,
                    "MongoDB id_column is not a valid identifier");
            settings.id_column = value;
        } else {
            fail(ORM_STATUS_INVALID_ARGUMENT,
                 "unknown MongoDB connection option: " + key);
        }
    }
    require(has_database, ORM_STATUS_INVALID_ARGUMENT,
            "MongoDB database option is required");
    return settings;
}

orm_status_t map_mongo_status(const bson_error_t& error,
                              orm_status_t fallback) noexcept
{
    // Connection-related errors
    if (error.domain == MONGOC_ERROR_SERVER_SELECTION ||
        error.domain == MONGOC_ERROR_STREAM ||
#ifdef MONGOC_ERROR_NETWORK
        error.domain == MONGOC_ERROR_NETWORK ||
#endif
        error.domain == MONGOC_ERROR_CLIENT_AUTHENTICATE) {
        return ORM_STATUS_CONNECTION_ERROR;
    }
    
    // Server errors
    if (error.domain == MONGOC_ERROR_SERVER) {
        if (error.code == mongo_illegal_operation_code)
            return ORM_STATUS_UNSUPPORTED;
        return ORM_STATUS_DATASTORE_ERROR;
    }
    
    // Protocol/BSON errors
    if (error.domain == MONGOC_ERROR_PROTOCOL ||
        error.domain == MONGOC_ERROR_BSON) {
        return ORM_STATUS_DATASTORE_ERROR;
    }
    
    // Cursor invalidation
#ifdef MONGOC_ERROR_CURSOR_INVALIDATED
    if (error.domain == MONGOC_ERROR_CURSOR_INVALIDATED) {
        return ORM_STATUS_INVALID_STATE;
    }
#endif
    
    // Command not found
#ifdef MONGOC_ERROR_COMMAND_NOT_FOUND
    if (error.domain == MONGOC_ERROR_COMMAND_NOT_FOUND) {
        return ORM_STATUS_UNSUPPORTED;
    }
#endif
    
    return fallback;
}

[[noreturn]] void fail_mongo(const bson_error_t& error,
                             orm_status_t fallback,
                             const char* operation)
{
    fail(map_mongo_status(error, fallback),
         std::string(operation) + ": " + error.message);
}

void check_mongo(bool ok,
                 const bson_error_t& error,
                 orm_status_t fallback,
                 const char* operation)
{
    if (!ok)
        fail_mongo(error, fallback, operation);
}

std::string format_double(double value)
{
    require(std::isfinite(value), ORM_STATUS_DATASTORE_ERROR,
            "MongoDB result produced a non-finite value");
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
           << value;
    require(static_cast<bool>(output), ORM_STATUS_INTERNAL_ERROR,
            "failed to format MongoDB result value");
    return output.str();
}

materialized_cell materialize_bson_value(const bson_iter_t& iterator)
{
    switch (bson_iter_type(&iterator)) {
    case BSON_TYPE_NULL:
    case BSON_TYPE_UNDEFINED:
        return {};
    case BSON_TYPE_BOOL:
        return materialized_cell{false,
                                 bson_iter_bool(&iterator) ? "1" : "0"};
    case BSON_TYPE_INT32:
        return materialized_cell{false,
                                 std::to_string(bson_iter_int32(&iterator))};
    case BSON_TYPE_INT64:
        return materialized_cell{false,
                                 std::to_string(bson_iter_int64(&iterator))};
    case BSON_TYPE_DOUBLE:
        return materialized_cell{false,
                                 format_double(bson_iter_double(&iterator))};
    case BSON_TYPE_UTF8: {
        std::uint32_t length = 0;
        const char* text = bson_iter_utf8(&iterator, &length);
        require(text != nullptr || length == 0, ORM_STATUS_DATASTORE_ERROR,
                "MongoDB returned a UTF-8 value with a null data pointer");
        return materialized_cell{false, std::string(text, length)};
    }
    case BSON_TYPE_BINARY: {
        bson_subtype_t subtype = BSON_SUBTYPE_BINARY;
        std::uint32_t length = 0;
        const std::uint8_t* data = nullptr;
        bson_iter_binary(&iterator, &subtype, &length, &data);
        require(data != nullptr || length == 0, ORM_STATUS_DATASTORE_ERROR,
                "MongoDB returned a binary value with a null data pointer");
        return materialized_cell{
            false, std::string(reinterpret_cast<const char*>(data), length)};
    }
    default:
        fail(ORM_STATUS_DATASTORE_ERROR,
             "MongoDB result contains an unsupported BSON type");
    }
}

materialized_cell materialize_field(const bson_t* document, const char* key)
{
    bson_iter_t iterator;
    if (!bson_iter_init_find(&iterator, document, key))
        return {};
    return materialize_bson_value(iterator);
}

class bson_subdocument final {
public:
    bool init(const bson_iter_t& iterator) noexcept
    {
        initialized_ = bson_iter_recurse(&iterator, &iterator_);
        return initialized_;
    }

    const bson_iter_t* get_iterator() const noexcept
    {
        return initialized_ ? &iterator_ : nullptr;
    }

private:
    bson_iter_t iterator_;
    bool initialized_ = false;
};

materialized_cell materialize_group_key(const bson_t* document,
                                        std::size_t group_index)
{
    bson_iter_t id_iterator;
    if (!bson_iter_init_find(&id_iterator, document, "_id") ||
        bson_iter_type(&id_iterator) != BSON_TYPE_DOCUMENT)
        return {};
    bson_subdocument group_key;
    if (!group_key.init(id_iterator))
        return {};
    const std::string key = mongo_group_key_name(group_index);
    bson_iter_t field;
    const bson_iter_t* iter = group_key.get_iterator();
    if (iter == nullptr)
        return {};
    // Create a mutable copy for bson_iter_find
    field = *iter;
    if (!bson_iter_find(&field, key.c_str()))
        return {};
    return materialize_bson_value(field);
}

std::uint64_t reply_uint64(const bson_t* reply, const char* key)
{
    if (reply == nullptr)
        return 0;
    bson_iter_t iterator;
    if (!bson_iter_init_find(&iterator, reply, key))
        return 0;
    switch (bson_iter_type(&iterator)) {
    case BSON_TYPE_INT32:
        return static_cast<std::uint64_t>(bson_iter_int32(&iterator));
    case BSON_TYPE_INT64:
        return static_cast<std::uint64_t>(bson_iter_int64(&iterator));
    case BSON_TYPE_DOUBLE:
        require(bson_iter_double(&iterator) >= 0 &&
                    bson_iter_double(&iterator) <=
                        static_cast<double>(
                            std::numeric_limits<std::uint64_t>::max()),
                ORM_STATUS_DATASTORE_ERROR,
                "MongoDB reply field is not a valid non-negative count");
        return static_cast<std::uint64_t>(bson_iter_double(&iterator));
    default:
        return 0;
    }
}

void append_session(bson_t* options, mongoc_client_session_t* session)
{
    if (session == nullptr)
        return;
    bson_error_t error;
    check_mongo(mongoc_client_session_append(session, options, &error), error,
                ORM_STATUS_INTERNAL_ERROR, "attach MongoDB session");
}

class mongo_backend final : public database_backend {
    class explicit_transaction final : public transaction_backend {
    public:
        explicit_transaction(mongo_backend& owner, orm_isolation_t isolation)
            : owner_(owner)
        {
            bson_error_t error;
            mongoc_client_session_t* raw = mongoc_client_start_session(
                owner_.client_.get(), nullptr, &error);
            if (raw == nullptr)
                fail_mongo(error, ORM_STATUS_CONNECTION_ERROR,
                           "start MongoDB session");
            session_.reset(raw);
            transaction_opts_handle options = transaction_options(isolation);
            if (!mongoc_client_session_start_transaction(session_.get(),
                                                         options.get(), &error))
                fail_mongo(error, ORM_STATUS_UNSUPPORTED,
                           "start MongoDB transaction");
            verify_transaction_support();
            owner_.transaction_active_ = true;
        }

        ~explicit_transaction() override
        {
            rollback_noexcept();
        }

        std::unique_ptr<result_backend>
        execute_plan(const query_plan& plan,
                     const connection_limits& limits) override
        {
            require(session_ != nullptr &&
                        mongoc_client_session_in_transaction(session_.get()),
                    ORM_STATUS_INVALID_STATE,
                    "MongoDB transaction is no longer active");
            return owner_.execute_plan_in_transaction(plan, limits,
                                                      session_.get());
        }

        void commit() override
        {
            require_active();
            bson_error_t error;
            check_mongo(mongoc_client_session_commit_transaction(
                            session_.get(), nullptr, &error), error,
                        ORM_STATUS_DATASTORE_ERROR,
                        "commit MongoDB transaction");
            session_.reset();
            owner_.transaction_active_ = false;
        }

        void rollback() override
        {
            require_active();
            bson_error_t error;
            check_mongo(mongoc_client_session_abort_transaction(
                            session_.get(), &error), error,
                        ORM_STATUS_DATASTORE_ERROR,
                        "abort MongoDB transaction");
            session_.reset();
            owner_.transaction_active_ = false;
        }

    private:
        static transaction_opts_handle
        transaction_options(orm_isolation_t isolation)
        {
            transaction_opts_handle options(mongoc_transaction_opts_new());
            require(options != nullptr, ORM_STATUS_OUT_OF_MEMORY,
                    "create MongoDB transaction options failed");
            switch (isolation) {
            case ORM_ISOLATION_READ_UNCOMMITTED:
            case ORM_ISOLATION_READ_COMMITTED:
                break;
            case ORM_ISOLATION_REPEATABLE_READ:
            case ORM_ISOLATION_SNAPSHOT: {
                read_concern_handle concern(mongoc_read_concern_new());
                require(concern != nullptr, ORM_STATUS_OUT_OF_MEMORY,
                        "create MongoDB read concern failed");
                mongoc_read_concern_set_level(concern.get(), "snapshot");
                mongoc_transaction_opts_set_read_concern(options.get(),
                                                         concern.get());
                break;
            }
            case ORM_ISOLATION_SERIALIZABLE: {
                read_concern_handle concern(mongoc_read_concern_new());
                require(concern != nullptr, ORM_STATUS_OUT_OF_MEMORY,
                        "create MongoDB read concern failed");
                mongoc_read_concern_set_level(concern.get(), "snapshot");
                mongoc_transaction_opts_set_read_concern(options.get(),
                                                         concern.get());
                write_concern_handle write_concern(mongoc_write_concern_new());
                require(write_concern != nullptr, ORM_STATUS_OUT_OF_MEMORY,
                        "create MongoDB write concern failed");
                mongoc_write_concern_set_wmajority(write_concern.get(), 0);
                mongoc_transaction_opts_set_write_concern(options.get(),
                                                          write_concern.get());
                break;
            }
            default:
                fail(ORM_STATUS_INVALID_ARGUMENT,
                     "unknown MongoDB transaction isolation level");
            }
            return options;
        }

        void require_active() const
        {
            require(session_ != nullptr &&
                        mongoc_client_session_in_transaction(session_.get()),
                    ORM_STATUS_INVALID_STATE,
                    "MongoDB transaction is no longer active");
        }

        void verify_transaction_support() const
        {
            // find with limit 0 is permitted inside transactions and never
            // reads a document. Standalone deployments reject every command
            // carrying a transaction number with IllegalOperation (code 20).
            bson_guard filter;
            bson_guard options;
            bson_append_int64(options.get(), "limit", -1, 1);
            append_session(options.get(), session_.get());
            collection_handle probe = owner_.collection(
                transaction_probe_collection);
            bson_error_t error;
            mongoc_cursor_t* raw = mongoc_collection_find_with_opts(
                probe.get(), filter.get(), options.get(), nullptr);
            if (raw == nullptr) {
                // find_with_opts can only fail on invalid arguments, which the
                // builder cannot produce, so a null cursor here is internal.
                fail(ORM_STATUS_INTERNAL_ERROR,
                     "MongoDB transaction probe returned a null cursor");
            }
            cursor_handle cursor(raw);
            const bson_t* document = nullptr;
            (void)mongoc_cursor_next(cursor.get(), &document);
            if (mongoc_cursor_error(cursor.get(), &error)) {
                if (map_mongo_status(error, ORM_STATUS_DATASTORE_ERROR) ==
                    ORM_STATUS_UNSUPPORTED)
                    fail(ORM_STATUS_UNSUPPORTED,
                         "MongoDB transactions require a replica set or "
                         "sharded cluster: " + std::string(error.message));
                fail_mongo(error, ORM_STATUS_DATASTORE_ERROR,
                           "probe MongoDB transaction support");
            }
        }

        void rollback_noexcept() noexcept
        {
            if (session_ == nullptr ||
                !mongoc_client_session_in_transaction(session_.get()))
                return;
            bson_error_t error;
            (void)mongoc_client_session_abort_transaction(session_.get(),
                                                          &error);
            session_.reset();
            owner_.transaction_active_ = false;
        }

        mongo_backend& owner_;
        session_handle session_;
    };

public:
    mongo_backend(const std::vector<std::string>& keywords,
                  const std::vector<std::string>& values,
                  const connection_limits&)
        : settings_(parse_settings(keywords, values))
    {
        client_.reset(mongoc_client_new(settings_.uri.c_str()));
        require(client_ != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "invalid MongoDB connection URI");
        database_.reset(mongoc_client_get_database(
            client_.get(), settings_.database.c_str()));
        require(database_ != nullptr, ORM_STATUS_OUT_OF_MEMORY,
                "create MongoDB database handle failed");
        
        // Verify connection using ping command
        bson_error_t error;
        bson_guard ping_cmd;
        bson_append_int32(ping_cmd.get(), "ping", 4, 1);
        check_mongo(mongoc_client_command_simple(
                        client_.get(), "admin", ping_cmd.get(), 
                        nullptr, nullptr, &error), error,
                    ORM_STATUS_CONNECTION_ERROR, "ping MongoDB server");
    }

    execution_model model() const noexcept override
    {
        return execution_model::native_plan;
    }

    std::string placeholder(std::size_t) const override
    {
        fail(ORM_STATUS_UNSUPPORTED,
             "MongoDB native query plans do not use SQL placeholders");
    }

    std::string pagination(std::optional<std::uint64_t>,
                           std::optional<std::uint64_t>) const override
    {
        fail(ORM_STATUS_UNSUPPORTED,
             "MongoDB native query plans do not use SQL pagination");
    }

    std::unique_ptr<result_backend>
    execute_sql(std::string_view,
                const std::vector<bound_parameter>&,
                bool,
                const connection_limits&) override
    {
        fail(ORM_STATUS_UNSUPPORTED,
             "raw SQL is not supported by the MongoDB backend");
    }

    std::unique_ptr<result_backend>
    execute_plan(const query_plan& plan,
                 const connection_limits& limits) override
    {
        require(!transaction_active_, ORM_STATUS_INVALID_STATE,
                "execute MongoDB transaction queries through the transaction handle");
        return execute_plan_in_transaction(plan, limits, nullptr);
    }

    std::unique_ptr<transaction_backend>
    begin_transaction(orm_isolation_t isolation) override
    {
        require(!transaction_active_, ORM_STATUS_INVALID_STATE,
                "MongoDB connection already has an active transaction");
        return std::make_unique<explicit_transaction>(*this, isolation);
    }

private:
    collection_handle collection(std::string_view table) const
    {
        const std::string owned(table);
        mongoc_collection_t* raw = mongoc_client_get_collection(
            client_.get(), settings_.database.c_str(), owned.c_str());
        require(raw != nullptr, ORM_STATUS_OUT_OF_MEMORY,
                "create MongoDB collection handle failed");
        return collection_handle(raw);
    }

    std::unique_ptr<result_backend>
    execute_plan_in_transaction(const query_plan& plan,
                                const connection_limits& limits,
                                mongoc_client_session_t* session)
    {
        require(plan.joins.empty(), ORM_STATUS_UNSUPPORTED,
                "MongoDB joins are not supported by the ORM backend");
        switch (plan.kind) {
        case query_kind::select:
            return execute_select(plan, limits, session);
        case query_kind::insert:
            return execute_insert(plan, session);
        case query_kind::update:
            return execute_update(plan, session);
        case query_kind::remove:
            return execute_delete(plan, session);
        case query_kind::raw:
            fail(ORM_STATUS_UNSUPPORTED,
                 "raw SQL is not supported by the MongoDB backend");
        default:
            fail(ORM_STATUS_INTERNAL_ERROR,
                 "unknown MongoDB query plan kind");
        }
    }

    static void validate_projection(const query_plan& plan)
    {
        require(!plan.select_all, ORM_STATUS_UNSUPPORTED,
                "MongoDB SELECT requires an explicit projection");
        require(!plan.columns.empty() || !plan.aggregates.empty(),
                ORM_STATUS_INVALID_STATE,
                "MongoDB SELECT has no projected columns or aggregates");
        require(plan.having_root.children.empty() || !plan.aggregates.empty(),
                ORM_STATUS_UNSUPPORTED,
                "MongoDB HAVING requires an aggregate query");
        if (!plan.aggregates.empty()) {
            for (const std::string& column : plan.columns) {
                require(std::find(plan.group_columns.begin(),
                                  plan.group_columns.end(), column) !=
                            plan.group_columns.end(),
                        ORM_STATUS_UNSUPPORTED,
                        "MongoDB aggregate projections must appear in GROUP BY");
            }
        } else {
            require(plan.group_columns.empty(), ORM_STATUS_UNSUPPORTED,
                    "MongoDB GROUP BY requires an aggregate projection");
        }
    }

    std::unique_ptr<result_backend>
    fetch_rows(mongoc_cursor_t* cursor,
               const query_plan& plan,
               const connection_limits& limits,
               bool aggregate)
    {
        const std::uint64_t columns = aggregate
            ? static_cast<std::uint64_t>(plan.columns.size() +
                                         plan.aggregates.size())
            : static_cast<std::uint64_t>(plan.columns.size());
        require(columns <= limits.max_columns, ORM_STATUS_LIMIT_EXCEEDED,
                "MongoDB result column count exceeds max_columns");
        std::vector<materialized_cell> cells;
        std::uint64_t rows = 0;
        std::uint64_t retained = 0;
        const bson_t* document = nullptr;
        while (mongoc_cursor_next(cursor, &document)) {
            require(rows < limits.max_result_rows, ORM_STATUS_LIMIT_EXCEEDED,
                    "MongoDB result row count exceeds max_result_rows");
            for (std::uint64_t column = 0; column < columns; ++column) {
                materialized_cell cell;
                if (!aggregate) {
                    const std::string& projected = plan.columns[column];
                    const std::string key =
                        projected == settings_.id_column ? "_id" : projected;
                    cell = materialize_field(document, key.c_str());
                } else if (column < plan.columns.size()) {
                    const std::string& projected = plan.columns[column];
                    const auto found =
                        std::find(plan.group_columns.begin(),
                                  plan.group_columns.end(), projected);
                    require(found != plan.group_columns.end(),
                            ORM_STATUS_INTERNAL_ERROR,
                            "MongoDB aggregate projection lost its group key");
                    const std::size_t group_index = static_cast<std::size_t>(
                        std::distance(plan.group_columns.begin(), found));
                    cell = materialize_group_key(document, group_index);
                } else {
                    const std::size_t aggregate_index = static_cast<std::size_t>(
                        column - plan.columns.size());
                    cell = materialize_field(
                        document,
                        mongo_aggregate_value_name(aggregate_index).c_str());
                }
                require(cell.value.size() <= limits.max_result_bytes - retained,
                        ORM_STATUS_LIMIT_EXCEEDED,
                        "MongoDB result payload exceeds max_result_bytes");
                retained += cell.value.size();
                cells.push_back(std::move(cell));
            }
            ++rows;
        }
        bson_error_t error;
        if (mongoc_cursor_error(cursor, &error))
            fail_mongo(error, ORM_STATUS_DATASTORE_ERROR,
                       "iterate MongoDB cursor");
        return std::make_unique<mongo_result>(rows, columns, 0,
                                              std::move(cells));
    }

    std::unique_ptr<result_backend>
    execute_select(const query_plan& plan,
                   const connection_limits& limits,
                   mongoc_client_session_t* session)
    {
        validate_projection(plan);
        collection_handle target = collection(plan.table);
        bson_guard filter;
        mongo_append_filter(filter.get(), plan.where_root, settings_);
        if (plan.aggregates.empty()) {
            bson_guard options;
            mongo_append_find_options(options.get(), plan, settings_);
            append_session(options.get(), session);
            mongoc_cursor_t* raw = mongoc_collection_find_with_opts(
                target.get(), filter.get(), options.get(), nullptr);
            require(raw != nullptr, ORM_STATUS_INTERNAL_ERROR,
                    "MongoDB find returned a null cursor");
            cursor_handle cursor(raw);
            return fetch_rows(cursor.get(), plan, limits, false);
        }
        bson_guard pipeline;
        mongo_append_pipeline(pipeline.get(), plan, settings_);
        bson_guard options;
        append_session(options.get(), session);
        mongoc_cursor_t* raw = mongoc_collection_aggregate(
            target.get(), MONGOC_QUERY_NONE, pipeline.get(), options.get(),
            nullptr);
        require(raw != nullptr, ORM_STATUS_INTERNAL_ERROR,
                "MongoDB aggregate returned a null cursor");
        cursor_handle cursor(raw);
        return fetch_rows(cursor.get(), plan, limits, true);
    }

    std::unique_ptr<result_backend>
    execute_insert(const query_plan& plan, mongoc_client_session_t* session)
    {
        collection_handle target = collection(plan.table);
        bson_guard document;
        mongo_append_insert_document(document.get(), plan, settings_);
        bson_guard options;
        append_session(options.get(), session);
        bson_error_t error;
        check_mongo(mongoc_collection_insert_one(
                        target.get(), document.get(), options.get(), nullptr,
                        &error), error,
                    ORM_STATUS_DATASTORE_ERROR, "insert MongoDB document");
        return std::make_unique<mongo_result>(
            0, 0, 1, std::vector<materialized_cell>{});
    }

    const predicate& require_id_predicate(const query_plan& plan) const
    {
        require(!plan.where_root.children.empty() &&
                    plan.where_root.logic == ORM_LOGIC_AND,
                ORM_STATUS_UNSUPPORTED,
                "MongoDB UPDATE/DELETE requires conjunctive equality predicates");
        const predicate* id = nullptr;
        for (const auto& child : plan.where_root.children) {
            require(child != nullptr && !child->is_group &&
                        child->value.comparison == ORM_COMPARE_EQUAL &&
                        child->value.has_parameter,
                    ORM_STATUS_UNSUPPORTED,
                    "MongoDB UPDATE/DELETE requires conjunctive non-null equality predicates");
            if (child->value.column == settings_.id_column) {
                require(id == nullptr, ORM_STATUS_INVALID_ARGUMENT,
                        "MongoDB UPDATE/DELETE contains duplicate id predicates");
                id = &child->value;
            }
        }
        require(id != nullptr, ORM_STATUS_UNSUPPORTED,
                "MongoDB UPDATE/DELETE requires a non-null id equality predicate");
        return *id;
    }

    std::unique_ptr<result_backend>
    execute_update(const query_plan& plan, mongoc_client_session_t* session)
    {
        (void)require_id_predicate(plan);
        collection_handle target = collection(plan.table);
        bson_guard filter;
        mongo_append_filter(filter.get(), plan.where_root, settings_);
        bson_guard update;
        mongo_append_update_document(update.get(), plan, settings_);
        bson_guard options;
        append_session(options.get(), session);
        bson_guard reply;
        bson_error_t error;
        check_mongo(mongoc_collection_update_one(
                        target.get(), filter.get(), update.get(),
                        options.get(), reply.get(), &error), error,
                    ORM_STATUS_DATASTORE_ERROR, "update MongoDB document");
        const std::uint64_t affected =
            reply_uint64(reply.get(), "matchedCount");
        return std::make_unique<mongo_result>(
            0, 0, affected, std::vector<materialized_cell>{});
    }

    std::unique_ptr<result_backend>
    execute_delete(const query_plan& plan, mongoc_client_session_t* session)
    {
        (void)require_id_predicate(plan);
        collection_handle target = collection(plan.table);
        bson_guard filter;
        mongo_append_filter(filter.get(), plan.where_root, settings_);
        bson_guard options;
        append_session(options.get(), session);
        bson_guard reply;
        bson_error_t error;
        check_mongo(mongoc_collection_delete_one(
                        target.get(), filter.get(), options.get(),
                        reply.get(), &error), error,
                    ORM_STATUS_DATASTORE_ERROR, "delete MongoDB document");
        const std::uint64_t affected =
            reply_uint64(reply.get(), "deletedCount");
        return std::make_unique<mongo_result>(
            0, 0, affected, std::vector<materialized_cell>{});
    }

    mongo_settings settings_;
    client_handle client_;
    database_handle database_;
    bool transaction_active_ = false;
};

} // namespace

std::unique_ptr<database_backend>
make_mongo_backend(const std::vector<std::string>& keywords,
                   const std::vector<std::string>& values,
                   const connection_limits& limits)
{
    return std::make_unique<mongo_backend>(keywords, values, limits);
}

} // namespace orm_c_detail
