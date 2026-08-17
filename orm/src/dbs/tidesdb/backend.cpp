#include "row.hpp"

#include "bridge.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orm_c_detail {
namespace {

constexpr std::uint64_t default_max_scan_rows = 100000;
constexpr std::uint64_t default_max_scan_bytes = 64U * 1024U * 1024U;

struct database_deleter {
    void operator()(tidesdb_t* database) const noexcept
    {
        if (database != nullptr)
            (void)tidesdb_close(database);
    }
};

struct transaction_deleter {
    void operator()(tidesdb_txn_t* transaction) const noexcept
    {
        tidesdb_txn_free(transaction);
    }
};

struct iterator_deleter {
    void operator()(tidesdb_iter_t* iterator) const noexcept
    {
        tidesdb_iter_free(iterator);
    }
};

struct value_deleter {
    void operator()(std::uint8_t* value) const noexcept
    {
        tidesdb_free(value);
    }
};

using database_handle = std::unique_ptr<tidesdb_t, database_deleter>;
using transaction_handle = std::unique_ptr<tidesdb_txn_t, transaction_deleter>;
using iterator_handle = std::unique_ptr<tidesdb_iter_t, iterator_deleter>;
using value_handle = std::unique_ptr<std::uint8_t, value_deleter>;

struct materialized_cell {
    bool is_null = true;
    std::string value;
};

class tidesdb_result final : public result_backend {
public:
    tidesdb_result(std::uint64_t rows,
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
                "TidesDB result cell is null");
        return tstr_v_from_buf(selected.value.data(), selected.value.size());
    }

private:
    const materialized_cell& get(std::uint64_t row, std::uint64_t column) const
    {
        require(row < rows_ && column < columns_, ORM_STATUS_OUT_OF_RANGE,
                "TidesDB result row or column is out of range");
        const std::uint64_t index = row * columns_ + column;
        require(index < cells_.size(), ORM_STATUS_INTERNAL_ERROR,
                "materialized TidesDB result has inconsistent dimensions");
        return cells_[static_cast<std::size_t>(index)];
    }

    std::uint64_t rows_;
    std::uint64_t columns_;
    std::uint64_t affected_rows_;
    std::vector<materialized_cell> cells_;
};

struct tidesdb_settings {
    std::string path;
    std::string column_family = "orm";
    bool create_if_missing = true;
    std::string id_column = "id";
    std::string key_prefix = "orm:";
    std::uint64_t ttl_seconds = 0;
    std::uint64_t max_scan_rows = default_max_scan_rows;
    std::uint64_t max_scan_bytes = default_max_scan_bytes;
};

struct scanned_row {
    tidesdb_row values;
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

bool parse_boolean(std::string_view text, const char* role)
{
    if (text == "true" || text == "1")
        return true;
    if (text == "false" || text == "0")
        return false;
    fail(ORM_STATUS_INVALID_ARGUMENT,
         std::string(role) + " must be true, false, 1, or 0");
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

tidesdb_settings parse_settings(const std::vector<std::string>& keywords,
                                const std::vector<std::string>& values)
{
    require(keywords.size() == values.size(), ORM_STATUS_INTERNAL_ERROR,
            "TidesDB option arrays have different sizes");
    tidesdb_settings settings;
    for (std::size_t index = 0; index < keywords.size(); ++index) {
        const std::string& key = keywords[index];
        const std::string& value = values[index];
        if (key == "path") {
            require(!value.empty(), ORM_STATUS_INVALID_ARGUMENT,
                    "TidesDB path is empty");
            settings.path = value;
        } else if (key == "column_family") {
            require(valid_identifier(value), ORM_STATUS_INVALID_ARGUMENT,
                    "TidesDB column_family is not a valid identifier");
            settings.column_family = value;
        } else if (key == "create_if_missing") {
            settings.create_if_missing = parse_boolean(value, "TidesDB create_if_missing");
        } else if (key == "id_column") {
            require(valid_identifier(value), ORM_STATUS_INVALID_ARGUMENT,
                    "TidesDB id_column is not a valid identifier");
            settings.id_column = value;
        } else if (key == "key_prefix") {
            require(!value.empty(), ORM_STATUS_INVALID_ARGUMENT,
                    "TidesDB key_prefix is empty");
            settings.key_prefix = value;
        } else if (key == "ttl_seconds") {
            settings.ttl_seconds =
                parse_integer<std::uint64_t>(value, "TidesDB ttl_seconds");
        } else if (key == "max_scan_rows") {
            settings.max_scan_rows =
                parse_integer<std::uint64_t>(value, "TidesDB max_scan_rows");
            require(settings.max_scan_rows != 0, ORM_STATUS_INVALID_ARGUMENT,
                    "TidesDB max_scan_rows must be positive");
        } else if (key == "max_scan_bytes") {
            settings.max_scan_bytes =
                parse_integer<std::uint64_t>(value, "TidesDB max_scan_bytes");
            require(settings.max_scan_bytes != 0, ORM_STATUS_INVALID_ARGUMENT,
                    "TidesDB max_scan_bytes must be positive");
        } else {
            fail(ORM_STATUS_INVALID_ARGUMENT,
                 "unknown TidesDB connection option: " + key);
        }
    }
    require(!settings.path.empty(), ORM_STATUS_INVALID_ARGUMENT,
            "TidesDB path option is required");
    return settings;
}

const char* tidesdb_error_name(int code) noexcept
{
    switch (code) {
    case TDB_SUCCESS: return "success";
    case TDB_ERR_MEMORY: return "memory allocation failed";
    case TDB_ERR_INVALID_ARGS: return "invalid arguments";
    case TDB_ERR_NOT_FOUND: return "not found";
    case TDB_ERR_IO: return "I/O failure";
    case TDB_ERR_CORRUPTION: return "data corruption";
    case TDB_ERR_EXISTS: return "already exists";
    case TDB_ERR_CONFLICT: return "transaction conflict";
    case TDB_ERR_TOO_LARGE: return "value too large";
    case TDB_ERR_MEMORY_LIMIT: return "memory limit exceeded";
    case TDB_ERR_INVALID_DB: return "invalid database";
    case TDB_ERR_LOCKED: return "database locked";
    case TDB_ERR_READONLY: return "database is read-only";
    case TDB_ERR_BUSY: return "database is busy";
    case TDB_ERR_PRECONDITION: return "precondition failed";
    default: return "unknown TidesDB error";
    }
}

orm_status_t map_tidesdb_status(int code, orm_status_t fallback) noexcept
{
    switch (code) {
    case TDB_ERR_MEMORY: return ORM_STATUS_OUT_OF_MEMORY;
    case TDB_ERR_INVALID_ARGS: return ORM_STATUS_INVALID_ARGUMENT;
    case TDB_ERR_READONLY: return ORM_STATUS_INVALID_STATE;
    case TDB_ERR_CONFLICT:
    case TDB_ERR_LOCKED:
    case TDB_ERR_BUSY:
    case TDB_ERR_PRECONDITION: return ORM_STATUS_BUSY;
    case TDB_ERR_TOO_LARGE:
    case TDB_ERR_MEMORY_LIMIT: return ORM_STATUS_LIMIT_EXCEEDED;
    case TDB_ERR_CORRUPTION:
    case TDB_ERR_INVALID_DB: return ORM_STATUS_DATASTORE_ERROR;
    default: return fallback;
    }
}

[[noreturn]] void fail_tidesdb(int code,
                               orm_status_t fallback,
                               const char* operation)
{
    fail(map_tidesdb_status(code, fallback),
         std::string(operation) + ": " + tidesdb_error_name(code) +
             " (" + std::to_string(code) + ")");
}

void check_tidesdb(int code,
                   orm_status_t fallback,
                   const char* operation)
{
    if (code != TDB_SUCCESS)
        fail_tidesdb(code, fallback, operation);
}

std::string parameter_text(const bound_parameter& parameter)
{
    switch (parameter.kind) {
    case ORM_VALUE_BOOLEAN: return parameter.boolean_value ? "1" : "0";
    case ORM_VALUE_INT64:
    case ORM_VALUE_UINT64:
    case ORM_VALUE_DOUBLE:
    case ORM_VALUE_TEXT: return parameter.text;
    default:
        fail(ORM_STATUS_INVALID_ARGUMENT, "TidesDB entity id cannot be null");
    }
}

std::string aggregate_expression_name(const aggregate_expression& aggregate)
{
    const char* name = nullptr;
    switch (aggregate.kind) {
    case ORM_AGGREGATE_COUNT_ALL: return "count(*)";
    case ORM_AGGREGATE_COUNT: name = "count"; break;
    case ORM_AGGREGATE_SUM: name = "sum"; break;
    case ORM_AGGREGATE_AVERAGE: name = "avg"; break;
    case ORM_AGGREGATE_MINIMUM: name = "min"; break;
    case ORM_AGGREGATE_MAXIMUM: name = "max"; break;
    default:
        fail(ORM_STATUS_INVALID_ARGUMENT, "unknown TidesDB aggregate kind");
    }
    return std::string(name) + "(" + aggregate.column + ")";
}

materialized_cell materialize(const tidesdb_cell* cell)
{
    if (cell == nullptr || cell->is_null)
        return {};
    return materialized_cell{false, cell->text};
}

const tidesdb_cell* resolve_cell(const tidesdb_row& row,
                                 std::string_view table,
                                 std::string_view column)
{
    if (const tidesdb_cell* exact = tidesdb_find_cell(row, column))
        return exact;
    const std::string unqualified = tidesdb_unqualified_column(table, column);
    return tidesdb_find_cell(row, unqualified);
}

std::string format_double(double value)
{
    require(std::isfinite(value), ORM_STATUS_DATASTORE_ERROR,
            "TidesDB aggregate produced a non-finite value");
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    require(static_cast<bool>(output), ORM_STATUS_INTERNAL_ERROR,
            "failed to format TidesDB aggregate value");
    return output.str();
}

class aggregate_accumulator final {
public:
    void add(const tidesdb_cell* cell, orm_aggregate_t kind)
    {
        if (kind == ORM_AGGREGATE_COUNT_ALL) {
            ++count_;
            return;
        }
        if (cell == nullptr || cell->is_null)
            return;
        ++count_;
        if (kind == ORM_AGGREGATE_COUNT)
            return;
        if (!seen_) {
            source_kind_ = cell->kind;
            minimum_ = *cell;
            maximum_ = *cell;
            seen_ = true;
        } else {
            require(source_kind_ == cell->kind, ORM_STATUS_TYPE_ERROR,
                    "TidesDB aggregate column contains mixed value kinds");
            if (tidesdb_compare_cells(*cell, minimum_) < 0)
                minimum_ = *cell;
            if (tidesdb_compare_cells(*cell, maximum_) > 0)
                maximum_ = *cell;
        }
        if (kind == ORM_AGGREGATE_MINIMUM || kind == ORM_AGGREGATE_MAXIMUM)
            return;
        require(cell->kind != ORM_VALUE_TEXT, ORM_STATUS_TYPE_ERROR,
                "TidesDB SUM and AVG require a numeric column");
        add_numeric(*cell);
    }

    tidesdb_cell finish(orm_aggregate_t kind) const
    {
        if (kind == ORM_AGGREGATE_COUNT_ALL || kind == ORM_AGGREGATE_COUNT)
            return tidesdb_cell{ORM_VALUE_UINT64, false, std::to_string(count_)};
        if (!seen_)
            return tidesdb_null_cell();
        if (kind == ORM_AGGREGATE_MINIMUM)
            return minimum_;
        if (kind == ORM_AGGREGATE_MAXIMUM)
            return maximum_;
        if (kind == ORM_AGGREGATE_AVERAGE) {
            const long double divisor = static_cast<long double>(count_);
            return tidesdb_cell{ORM_VALUE_DOUBLE, false,
                                format_double(static_cast<double>(sum_ / divisor))};
        }
        require(kind == ORM_AGGREGATE_SUM, ORM_STATUS_INTERNAL_ERROR,
                "unknown TidesDB aggregate finalizer");
        if (source_kind_ == ORM_VALUE_INT64)
            return tidesdb_cell{ORM_VALUE_INT64, false, std::to_string(signed_sum_)};
        if (source_kind_ == ORM_VALUE_UINT64 || source_kind_ == ORM_VALUE_BOOLEAN)
            return tidesdb_cell{ORM_VALUE_UINT64, false, std::to_string(unsigned_sum_)};
        return tidesdb_cell{ORM_VALUE_DOUBLE, false,
                            format_double(static_cast<double>(sum_))};
    }

private:
    void add_numeric(const tidesdb_cell& cell)
    {
        if (cell.kind == ORM_VALUE_INT64) {
            const std::int64_t next = parse_integer<std::int64_t>(cell.text, "TidesDB integer");
            if ((next > 0 && signed_sum_ > std::numeric_limits<std::int64_t>::max() - next) ||
                (next < 0 && signed_sum_ < std::numeric_limits<std::int64_t>::min() - next))
                fail(ORM_STATUS_LIMIT_EXCEEDED, "TidesDB signed SUM overflow");
            signed_sum_ += next;
            sum_ += static_cast<long double>(next);
        } else if (cell.kind == ORM_VALUE_UINT64 || cell.kind == ORM_VALUE_BOOLEAN) {
            const std::uint64_t next = cell.kind == ORM_VALUE_BOOLEAN
                ? static_cast<std::uint64_t>(cell.text == "1")
                : parse_integer<std::uint64_t>(cell.text, "TidesDB unsigned integer");
            require(unsigned_sum_ <= std::numeric_limits<std::uint64_t>::max() - next,
                    ORM_STATUS_LIMIT_EXCEEDED,
                    "TidesDB unsigned SUM overflow");
            unsigned_sum_ += next;
            sum_ += static_cast<long double>(next);
        } else if (cell.kind == ORM_VALUE_DOUBLE) {
            double next = 0.0;
            const auto parsed = std::from_chars(
                cell.text.data(), cell.text.data() + cell.text.size(), next);
            require(parsed.ec == std::errc{} &&
                        parsed.ptr == cell.text.data() + cell.text.size() &&
                        std::isfinite(next),
                    ORM_STATUS_DATASTORE_ERROR,
                    "TidesDB row contains an invalid double");
            sum_ += static_cast<long double>(next);
            require(std::isfinite(static_cast<double>(sum_)),
                    ORM_STATUS_LIMIT_EXCEEDED,
                    "TidesDB floating-point SUM overflow");
        } else {
            fail(ORM_STATUS_TYPE_ERROR, "TidesDB aggregate requires a numeric value");
        }
    }

    std::uint64_t count_ = 0;
    bool seen_ = false;
    orm_value_kind_t source_kind_ = ORM_VALUE_NULL;
    std::int64_t signed_sum_ = 0;
    std::uint64_t unsigned_sum_ = 0;
    long double sum_ = 0.0L;
    tidesdb_cell minimum_;
    tidesdb_cell maximum_;
};

struct aggregate_group {
    std::vector<tidesdb_cell> keys;
    std::vector<aggregate_accumulator> accumulators;
};

std::uint64_t checked_result_bytes(std::uint64_t current,
                                   const materialized_cell& cell,
                                   const connection_limits& limits)
{
    require(cell.value.size() <= limits.max_result_bytes - current,
            ORM_STATUS_LIMIT_EXCEEDED,
            "TidesDB result payload exceeds max_result_bytes");
    return current + cell.value.size();
}

class tidesdb_backend final : public database_backend {
    class explicit_transaction final : public transaction_backend {
    public:
        explicit_transaction(tidesdb_backend& owner, orm_isolation_t isolation)
            : owner_(owner), native_(owner.begin_native_transaction(
                  native_isolation(isolation)))
        {
            owner_.transaction_active_ = true;
        }

        ~explicit_transaction() override
        {
            if (native_ != nullptr) {
                (void)tidesdb_txn_rollback(native_.get());
                owner_.transaction_active_ = false;
            }
        }

        std::unique_ptr<result_backend>
        execute_plan(const query_plan& plan,
                     const connection_limits& limits) override
        {
            require(native_ != nullptr, ORM_STATUS_INVALID_STATE,
                    "TidesDB transaction is no longer active");
            return owner_.execute_plan_in_transaction(plan, limits, native_.get());
        }

        void commit() override
        {
            require(native_ != nullptr, ORM_STATUS_INVALID_STATE,
                    "TidesDB transaction is no longer active");
            check_tidesdb(tidesdb_txn_commit(native_.get()),
                          ORM_STATUS_DATASTORE_ERROR,
                          "commit TidesDB transaction");
            native_.reset();
            owner_.transaction_active_ = false;
        }

        void rollback() override
        {
            require(native_ != nullptr, ORM_STATUS_INVALID_STATE,
                    "TidesDB transaction is no longer active");
            check_tidesdb(tidesdb_txn_rollback(native_.get()),
                          ORM_STATUS_DATASTORE_ERROR,
                          "roll back TidesDB transaction");
            native_.reset();
            owner_.transaction_active_ = false;
        }

        void savepoint(std::string_view name) override
        {
            require(native_ != nullptr, ORM_STATUS_INVALID_STATE,
                    "TidesDB transaction is no longer active");
            const std::string owned(name);
            check_tidesdb(tidesdb_txn_savepoint(native_.get(), owned.c_str()),
                          ORM_STATUS_DATASTORE_ERROR,
                          "create TidesDB savepoint");
        }

        void rollback_to_savepoint(std::string_view name) override
        {
            require(native_ != nullptr, ORM_STATUS_INVALID_STATE,
                    "TidesDB transaction is no longer active");
            const std::string owned(name);
            check_tidesdb(
                tidesdb_txn_rollback_to_savepoint(native_.get(), owned.c_str()),
                ORM_STATUS_DATASTORE_ERROR,
                "roll back TidesDB savepoint");
        }

        void release_savepoint(std::string_view name) override
        {
            require(native_ != nullptr, ORM_STATUS_INVALID_STATE,
                    "TidesDB transaction is no longer active");
            const std::string owned(name);
            check_tidesdb(tidesdb_txn_release_savepoint(native_.get(), owned.c_str()),
                          ORM_STATUS_DATASTORE_ERROR,
                          "release TidesDB savepoint");
        }

    private:
        static tidesdb_isolation_level_t native_isolation(orm_isolation_t isolation)
        {
            switch (isolation) {
            case ORM_ISOLATION_READ_UNCOMMITTED:
                return TDB_ISOLATION_READ_UNCOMMITTED;
            case ORM_ISOLATION_READ_COMMITTED:
                return TDB_ISOLATION_READ_COMMITTED;
            case ORM_ISOLATION_REPEATABLE_READ:
                return TDB_ISOLATION_REPEATABLE_READ;
            case ORM_ISOLATION_SNAPSHOT:
                return TDB_ISOLATION_SNAPSHOT;
            case ORM_ISOLATION_SERIALIZABLE:
                return TDB_ISOLATION_SERIALIZABLE;
            default:
                fail(ORM_STATUS_INVALID_ARGUMENT,
                     "unknown TidesDB isolation level");
            }
        }

        tidesdb_backend& owner_;
        transaction_handle native_;
    };

public:
    tidesdb_backend(const std::vector<std::string>& keywords,
                    const std::vector<std::string>& values,
                    const connection_limits& limits)
        : settings_(parse_settings(keywords, values)), limits_(limits)
    {
        tidesdb_config_t config = tidesdb_default_config();
        config.db_path = settings_.path.data();
        tidesdb_t* raw_database = nullptr;
        const int open_status = tidesdb_open(&config, &raw_database);
        database_.reset(raw_database);
        if (open_status != TDB_SUCCESS)
            fail_tidesdb(open_status, ORM_STATUS_CONNECTION_ERROR,
                         "open TidesDB database");

        column_family_ = tidesdb_get_column_family(database_.get(),
                                                    settings_.column_family.c_str());
        if (column_family_ == nullptr && settings_.create_if_missing) {
            const tidesdb_column_family_config_t family_config =
                tidesdb_default_column_family_config();
            const int create_status = tidesdb_create_column_family(
                database_.get(), settings_.column_family.c_str(), &family_config);
            if (create_status != TDB_SUCCESS && create_status != TDB_ERR_EXISTS)
                fail_tidesdb(create_status, ORM_STATUS_CONNECTION_ERROR,
                             "create TidesDB column family");
            column_family_ = tidesdb_get_column_family(database_.get(),
                                                        settings_.column_family.c_str());
        }
        require(column_family_ != nullptr, ORM_STATUS_CONNECTION_ERROR,
                "TidesDB column family does not exist and create_if_missing is false");
    }

    execution_model model() const noexcept override
    {
        return execution_model::native_plan;
    }

    std::string placeholder(std::size_t) const override
    {
        fail(ORM_STATUS_UNSUPPORTED,
             "TidesDB native query plans do not use SQL placeholders");
    }

    std::string pagination(std::optional<std::uint64_t>,
                           std::optional<std::uint64_t>) const override
    {
        fail(ORM_STATUS_UNSUPPORTED,
             "TidesDB native query plans do not use SQL pagination");
    }

    std::unique_ptr<result_backend>
    execute_sql(std::string_view,
                const std::vector<bound_parameter>&,
                bool,
                const connection_limits&) override
    {
        fail(ORM_STATUS_UNSUPPORTED,
             "raw SQL is not supported by the TidesDB backend");
    }

    std::unique_ptr<result_backend>
    execute_plan(const query_plan& plan,
                 const connection_limits& limits) override
    {
        require(!transaction_active_, ORM_STATUS_INVALID_STATE,
                "execute TidesDB transaction queries through the transaction handle");
        return execute_plan_in_transaction(plan, limits, nullptr);
    }

    std::unique_ptr<transaction_backend>
    begin_transaction(orm_isolation_t isolation) override
    {
        require(!transaction_active_, ORM_STATUS_INVALID_STATE,
                "TidesDB connection already has an active transaction");
        return std::make_unique<explicit_transaction>(*this, isolation);
    }

private:
    std::unique_ptr<result_backend>
    execute_plan_in_transaction(const query_plan& plan,
                                const connection_limits& limits,
                                tidesdb_txn_t* transaction)
    {
        require(plan.joins.empty(), ORM_STATUS_UNSUPPORTED,
                "TidesDB joins require an external index/query layer");
        switch (plan.kind) {
        case query_kind::select: return execute_select(plan, limits, transaction);
        case query_kind::insert: return execute_insert(plan, transaction);
        case query_kind::update: return execute_update(plan, transaction);
        case query_kind::remove: return execute_delete(plan, transaction);
        case query_kind::raw:
            fail(ORM_STATUS_UNSUPPORTED,
                 "raw SQL is not supported by the TidesDB backend");
        default:
            fail(ORM_STATUS_INTERNAL_ERROR, "unknown TidesDB query plan kind");
        }
    }

    std::string table_prefix(std::string_view table) const
    {
        std::string prefix = settings_.key_prefix;
        prefix.append(table.data(), table.size());
        prefix.push_back('\0');
        return prefix;
    }

    std::string entity_key(std::string_view table,
                           const bound_parameter& id) const
    {
        require(id.kind != ORM_VALUE_NULL, ORM_STATUS_INVALID_ARGUMENT,
                "TidesDB entity id cannot be null");
        std::string key = table_prefix(table);
        key.push_back(static_cast<char>(id.kind));
        key += parameter_text(id);
        return key;
    }

    time_t expiration() const
    {
        if (settings_.ttl_seconds == 0)
            return 0;
        const time_t now = std::time(nullptr);
        require(now >= 0, ORM_STATUS_DATASTORE_ERROR,
                "read system time for TidesDB TTL failed");
        const std::uint64_t now_value = static_cast<std::uint64_t>(now);
        const std::uint64_t maximum =
            static_cast<std::uint64_t>(std::numeric_limits<time_t>::max());
        require(settings_.ttl_seconds <= maximum - now_value,
                ORM_STATUS_LIMIT_EXCEEDED,
                "TidesDB TTL exceeds time_t range");
        return static_cast<time_t>(now_value + settings_.ttl_seconds);
    }

    transaction_handle begin_native_transaction(tidesdb_isolation_level_t isolation) const
    {
        tidesdb_txn_t* raw = nullptr;
        check_tidesdb(tidesdb_txn_begin_with_isolation(database_.get(), isolation, &raw),
                      ORM_STATUS_DATASTORE_ERROR,
                      "begin TidesDB transaction");
        require(raw != nullptr, ORM_STATUS_INTERNAL_ERROR,
                "TidesDB transaction begin returned a null handle");
        return transaction_handle(raw);
    }

    static bool has_prefix(const std::uint8_t* key,
                           std::size_t key_size,
                           std::string_view prefix) noexcept
    {
        return key != nullptr && key_size >= prefix.size() &&
               std::equal(prefix.begin(), prefix.end(), key);
    }

    std::vector<scanned_row> scan(const query_plan& plan,
                                  tidesdb_txn_t* transaction) const
    {
        const std::string prefix = table_prefix(plan.table);
        transaction_handle owned_transaction;
        if (transaction == nullptr) {
            owned_transaction = begin_native_transaction(TDB_ISOLATION_SNAPSHOT);
            transaction = owned_transaction.get();
        }
        tidesdb_iter_t* raw_iterator = nullptr;
        check_tidesdb(tidesdb_iter_new(transaction, column_family_, &raw_iterator),
                      ORM_STATUS_DATASTORE_ERROR,
                      "create TidesDB iterator");
        iterator_handle iterator(raw_iterator);

        const int seek_status = tidesdb_iter_seek(
            iterator.get(), reinterpret_cast<const std::uint8_t*>(prefix.data()), prefix.size());
        if (seek_status == TDB_ERR_NOT_FOUND)
            return {};
        check_tidesdb(seek_status, ORM_STATUS_DATASTORE_ERROR,
                      "seek TidesDB table prefix");

        std::vector<scanned_row> rows;
        std::uint64_t scanned_rows = 0;
        std::uint64_t scanned_bytes = 0;
        while (tidesdb_iter_valid(iterator.get())) {
            std::uint8_t* key = nullptr;
            std::size_t key_size = 0;
            check_tidesdb(tidesdb_iter_key(iterator.get(), &key, &key_size),
                          ORM_STATUS_DATASTORE_ERROR,
                          "read TidesDB iterator key");
            if (!has_prefix(key, key_size, prefix))
                break;

            std::uint8_t* value = nullptr;
            std::size_t value_size = 0;
            check_tidesdb(tidesdb_iter_value(iterator.get(), &value, &value_size),
                          ORM_STATUS_DATASTORE_ERROR,
                          "read TidesDB iterator value");
            require(scanned_rows < settings_.max_scan_rows,
                    ORM_STATUS_LIMIT_EXCEEDED,
                    "TidesDB scan exceeds max_scan_rows");
            ++scanned_rows;
            require(key_size <= settings_.max_scan_bytes - scanned_bytes,
                    ORM_STATUS_LIMIT_EXCEEDED,
                    "TidesDB scan exceeds max_scan_bytes");
            scanned_bytes += key_size;
            require(value_size <= settings_.max_scan_bytes - scanned_bytes,
                    ORM_STATUS_LIMIT_EXCEEDED,
                    "TidesDB scan exceeds max_scan_bytes");
            scanned_bytes += value_size;

            tidesdb_row decoded = decode_tidesdb_row(
                value, value_size, limits_.max_parameter_bytes, limits_.max_assignments);
            if (tidesdb_matches(plan.where_root, decoded, plan.table))
                rows.push_back(scanned_row{std::move(decoded)});

            const int next_status = tidesdb_iter_next(iterator.get());
            if (next_status == TDB_ERR_NOT_FOUND)
                break;
            check_tidesdb(next_status, ORM_STATUS_DATASTORE_ERROR,
                          "advance TidesDB iterator");
        }
        return rows;
    }

    static void validate_projection(const query_plan& plan)
    {
        require(!plan.select_all, ORM_STATUS_UNSUPPORTED,
                "TidesDB SELECT requires an explicit projection");
        require(!plan.columns.empty() || !plan.aggregates.empty(),
                ORM_STATUS_INVALID_STATE,
                "TidesDB SELECT has no projected columns or aggregates");
        require(plan.having_root.children.empty() || !plan.aggregates.empty(),
                ORM_STATUS_UNSUPPORTED,
                "TidesDB HAVING requires an aggregate query");
        if (!plan.aggregates.empty()) {
            for (const std::string& column : plan.columns) {
                require(std::find(plan.group_columns.begin(), plan.group_columns.end(), column) !=
                            plan.group_columns.end(),
                        ORM_STATUS_UNSUPPORTED,
                        "TidesDB aggregate projections must appear in GROUP BY");
            }
        } else {
            require(plan.group_columns.empty(), ORM_STATUS_UNSUPPORTED,
                    "TidesDB GROUP BY requires an aggregate projection");
        }
    }

    static void sort_rows(std::vector<scanned_row>& rows,
                          const query_plan& plan)
    {
        if (!plan.ordering)
            return;
        const std::string column =
            tidesdb_unqualified_column(plan.table, plan.ordering->first);
        const bool descending = plan.ordering->second == ORM_ORDER_DESCENDING;
        std::stable_sort(rows.begin(), rows.end(), [&](const scanned_row& left,
                                                       const scanned_row& right) {
            const tidesdb_cell null_cell = tidesdb_null_cell();
            const tidesdb_cell* left_cell = tidesdb_find_cell(left.values, column);
            const tidesdb_cell* right_cell = tidesdb_find_cell(right.values, column);
            const int compared = tidesdb_compare_cells(
                left_cell != nullptr ? *left_cell : null_cell,
                right_cell != nullptr ? *right_cell : null_cell);
            return descending ? compared > 0 : compared < 0;
        });
    }

    static std::pair<std::size_t, std::size_t>
    page(std::size_t size,
         const query_plan& plan,
         const connection_limits& limits)
    {
        if (plan.limit)
            require(*plan.limit <= limits.max_result_rows,
                    ORM_STATUS_LIMIT_EXCEEDED,
                    "TidesDB query limit exceeds max_result_rows");
        const std::uint64_t offset_value = plan.offset.value_or(0);
        const std::size_t begin = offset_value >= size
            ? size : static_cast<std::size_t>(offset_value);
        const std::uint64_t available = size - begin;
        const std::uint64_t count = plan.limit.value_or(available);
        require(plan.limit.has_value() || available <= limits.max_result_rows,
                ORM_STATUS_LIMIT_EXCEEDED,
                "TidesDB query result exceeds max_result_rows; set LIMIT");
        const std::uint64_t bounded = std::min(count, available);
        return {begin, begin + static_cast<std::size_t>(bounded)};
    }

    std::unique_ptr<result_backend>
    execute_rows(const query_plan& plan,
                 const connection_limits& limits,
                 std::vector<scanned_row> rows) const
    {
        sort_rows(rows, plan);
        const auto bounds = page(rows.size(), plan, limits);
        const std::uint64_t row_count = bounds.second - bounds.first;
        const std::uint64_t column_count = plan.columns.size();
        require(column_count <= limits.max_columns, ORM_STATUS_LIMIT_EXCEEDED,
                "TidesDB result column count exceeds max_columns");
        require(column_count == 0 || row_count <=
                    std::numeric_limits<std::size_t>::max() / column_count,
                ORM_STATUS_LIMIT_EXCEEDED,
                "TidesDB result cell count overflows size_t");

        std::vector<materialized_cell> cells;
        cells.reserve(static_cast<std::size_t>(row_count * column_count));
        std::uint64_t retained = row_count * column_count * sizeof(materialized_cell);
        require(retained <= limits.max_result_bytes, ORM_STATUS_LIMIT_EXCEEDED,
                "TidesDB result metadata exceeds max_result_bytes");
        for (std::size_t index = bounds.first; index < bounds.second; ++index) {
            for (const std::string& projected : plan.columns) {
                const std::string column =
                    tidesdb_unqualified_column(plan.table, projected);
                materialized_cell cell =
                    materialize(tidesdb_find_cell(rows[index].values, column));
                retained = checked_result_bytes(retained, cell, limits);
                cells.push_back(std::move(cell));
            }
        }
        return std::make_unique<tidesdb_result>(row_count, column_count, 0,
                                                std::move(cells));
    }

    static std::vector<tidesdb_cell>
    group_values(const scanned_row& row, const query_plan& plan)
    {
        std::vector<tidesdb_cell> values;
        values.reserve(plan.group_columns.size());
        for (const std::string& grouped : plan.group_columns) {
            const std::string column = tidesdb_unqualified_column(plan.table, grouped);
            const tidesdb_cell* value = tidesdb_find_cell(row.values, column);
            values.push_back(value != nullptr ? *value : tidesdb_null_cell());
        }
        return values;
    }

    static tidesdb_row aggregate_row(const aggregate_group& group,
                                     const query_plan& plan)
    {
        tidesdb_row row;
        row.reserve(plan.group_columns.size() + plan.aggregates.size() * 2);
        for (std::size_t index = 0; index < plan.group_columns.size(); ++index) {
            const std::string column =
                tidesdb_unqualified_column(plan.table, plan.group_columns[index]);
            tidesdb_set_cell(row, column, group.keys[index],
                             plan.group_columns.size() + plan.aggregates.size() * 2);
        }
        for (std::size_t index = 0; index < plan.aggregates.size(); ++index) {
            const aggregate_expression& expression = plan.aggregates[index];
            tidesdb_cell value = group.accumulators[index].finish(expression.kind);
            tidesdb_set_cell(row, aggregate_expression_name(expression), value,
                             plan.group_columns.size() + plan.aggregates.size() * 2);
            if (!expression.alias.empty())
                tidesdb_set_cell(row, expression.alias, value,
                                 plan.group_columns.size() + plan.aggregates.size() * 2);
        }
        return row;
    }

    std::unique_ptr<result_backend>
    execute_aggregate(const query_plan& plan,
                      const connection_limits& limits,
                      const std::vector<scanned_row>& rows) const
    {
        std::map<std::string, aggregate_group> groups;
        if (plan.group_columns.empty()) {
            aggregate_group group;
            group.accumulators.resize(plan.aggregates.size());
            groups.emplace(std::string{}, std::move(group));
        }
        for (const scanned_row& row : rows) {
            std::vector<tidesdb_cell> keys = group_values(row, plan);
            const std::string key = tidesdb_group_key(keys);
            auto found = groups.find(key);
            if (found == groups.end()) {
                aggregate_group group;
                group.keys = std::move(keys);
                group.accumulators.resize(plan.aggregates.size());
                found = groups.emplace(key, std::move(group)).first;
            }
            for (std::size_t index = 0; index < plan.aggregates.size(); ++index) {
                const aggregate_expression& aggregate = plan.aggregates[index];
                const tidesdb_cell* value = nullptr;
                if (aggregate.kind != ORM_AGGREGATE_COUNT_ALL)
                    value = resolve_cell(row.values, plan.table, aggregate.column);
                found->second.accumulators[index].add(value, aggregate.kind);
            }
        }

        std::vector<scanned_row> materialized_groups;
        materialized_groups.reserve(groups.size());
        for (const auto& entry : groups) {
            tidesdb_row row = aggregate_row(entry.second, plan);
            if (tidesdb_matches(plan.having_root, row, plan.table))
                materialized_groups.push_back(scanned_row{std::move(row)});
        }
        sort_rows(materialized_groups, plan);
        const auto bounds = page(materialized_groups.size(), plan, limits);
        const std::uint64_t row_count = bounds.second - bounds.first;
        const std::uint64_t column_count = plan.columns.size() + plan.aggregates.size();
        require(column_count <= limits.max_columns, ORM_STATUS_LIMIT_EXCEEDED,
                "TidesDB aggregate column count exceeds max_columns");
        require(column_count == 0 || row_count <=
                    std::numeric_limits<std::size_t>::max() / column_count,
                ORM_STATUS_LIMIT_EXCEEDED,
                "TidesDB aggregate cell count overflows size_t");
        require(column_count == 0 || row_count <=
                    std::numeric_limits<std::uint64_t>::max() / column_count,
                ORM_STATUS_LIMIT_EXCEEDED,
                "TidesDB aggregate cell count overflows uint64_t");
        const std::uint64_t cell_count = row_count * column_count;
        require(cell_count <= std::numeric_limits<std::uint64_t>::max() /
                    sizeof(materialized_cell),
                ORM_STATUS_LIMIT_EXCEEDED,
                "TidesDB aggregate metadata size overflows uint64_t");

        std::vector<materialized_cell> cells;
        cells.reserve(static_cast<std::size_t>(cell_count));
        std::uint64_t retained = cell_count * sizeof(materialized_cell);
        require(retained <= limits.max_result_bytes, ORM_STATUS_LIMIT_EXCEEDED,
                "TidesDB aggregate metadata exceeds max_result_bytes");
        for (std::size_t index = bounds.first; index < bounds.second; ++index) {
            const tidesdb_row& row = materialized_groups[index].values;
            for (const std::string& projected : plan.columns) {
                materialized_cell cell = materialize(resolve_cell(row, plan.table, projected));
                retained = checked_result_bytes(retained, cell, limits);
                cells.push_back(std::move(cell));
            }
            for (const aggregate_expression& aggregate : plan.aggregates) {
                materialized_cell cell = materialize(
                    tidesdb_find_cell(row, aggregate_expression_name(aggregate)));
                retained = checked_result_bytes(retained, cell, limits);
                cells.push_back(std::move(cell));
            }
        }
        return std::make_unique<tidesdb_result>(row_count, column_count, 0,
                                                std::move(cells));
    }

    std::unique_ptr<result_backend>
    execute_select(const query_plan& plan,
                   const connection_limits& limits,
                   tidesdb_txn_t* transaction) const
    {
        validate_projection(plan);
        std::vector<scanned_row> rows = scan(plan, transaction);
        if (plan.aggregates.empty())
            return execute_rows(plan, limits, std::move(rows));
        return execute_aggregate(plan, limits, rows);
    }

    const predicate& require_id_predicate(const query_plan& plan) const
    {
        require(!plan.where_root.children.empty() &&
                    plan.where_root.logic == ORM_LOGIC_AND,
                ORM_STATUS_UNSUPPORTED,
                "TidesDB UPDATE/DELETE requires conjunctive equality predicates");
        const predicate* id = nullptr;
        for (const auto& child : plan.where_root.children) {
            require(child != nullptr && !child->is_group &&
                        child->value.comparison == ORM_COMPARE_EQUAL &&
                        child->value.has_parameter,
                    ORM_STATUS_UNSUPPORTED,
                    "TidesDB UPDATE/DELETE requires conjunctive non-null equality predicates");
            if (tidesdb_unqualified_column(plan.table, child->value.column) ==
                settings_.id_column) {
                require(id == nullptr, ORM_STATUS_INVALID_ARGUMENT,
                        "TidesDB UPDATE/DELETE contains duplicate id predicates");
                id = &child->value;
            }
        }
        require(id != nullptr, ORM_STATUS_UNSUPPORTED,
                "TidesDB UPDATE/DELETE requires a non-null id equality predicate");
        return *id;
    }

    std::unique_ptr<result_backend> execute_insert(const query_plan& plan,
                                                   tidesdb_txn_t* transaction)
    {
        require(!plan.assignments.empty(), ORM_STATUS_INVALID_STATE,
                "TidesDB INSERT has no values");
        const assignment* id = nullptr;
        tidesdb_row row;
        row.reserve(plan.assignments.size());
        for (const assignment& value : plan.assignments) {
            if (value.column == settings_.id_column) {
                require(value.has_parameter, ORM_STATUS_INVALID_ARGUMENT,
                        "TidesDB entity id cannot be null");
                id = &value;
            }
            tidesdb_set_cell(row, value.column,
                             value.has_parameter
                                 ? tidesdb_cell_from_parameter(value.parameter)
                                 : tidesdb_null_cell(),
                             limits_.max_assignments);
        }
        require(id != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "TidesDB INSERT requires the configured id column");
        const std::string key = entity_key(plan.table, id->parameter);
        const std::string encoded = encode_tidesdb_row(
            row, limits_.max_parameter_bytes, limits_.max_assignments);

        transaction_handle owned_transaction;
        if (transaction == nullptr) {
            owned_transaction = begin_native_transaction(TDB_ISOLATION_SERIALIZABLE);
            transaction = owned_transaction.get();
        }
        std::uint8_t* existing = nullptr;
        std::size_t existing_size = 0;
        const int get_status = tidesdb_txn_get(
            transaction, column_family_,
            reinterpret_cast<const std::uint8_t*>(key.data()), key.size(),
            &existing, &existing_size);
        value_handle existing_guard(existing);
        if (get_status == TDB_SUCCESS)
            fail(ORM_STATUS_DATASTORE_ERROR,
                 "TidesDB INSERT entity already exists");
        if (get_status != TDB_ERR_NOT_FOUND)
            check_tidesdb(get_status, ORM_STATUS_DATASTORE_ERROR,
                          "read TidesDB entity before INSERT");

        check_tidesdb(tidesdb_txn_put(
                          transaction, column_family_,
                          reinterpret_cast<const std::uint8_t*>(key.data()), key.size(),
                          reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size(),
                          expiration()),
                      ORM_STATUS_DATASTORE_ERROR,
                      "write TidesDB INSERT");
        if (owned_transaction != nullptr)
            check_tidesdb(tidesdb_txn_commit(transaction),
                          ORM_STATUS_DATASTORE_ERROR,
                          "commit TidesDB INSERT");
        return std::make_unique<tidesdb_result>(
            0, 0, 1, std::vector<materialized_cell>{});
    }

    std::unique_ptr<result_backend> execute_update(const query_plan& plan,
                                                   tidesdb_txn_t* transaction)
    {
        require(!plan.assignments.empty(), ORM_STATUS_INVALID_STATE,
                "TidesDB UPDATE has no assignments");
        const predicate& id = require_id_predicate(plan);
        const std::string key = entity_key(plan.table, id.parameter);
        transaction_handle owned_transaction;
        if (transaction == nullptr) {
            owned_transaction = begin_native_transaction(TDB_ISOLATION_SERIALIZABLE);
            transaction = owned_transaction.get();
        }
        std::uint8_t* raw_value = nullptr;
        std::size_t value_size = 0;
        const int get_status = tidesdb_txn_get(
            transaction, column_family_,
            reinterpret_cast<const std::uint8_t*>(key.data()), key.size(),
            &raw_value, &value_size);
        value_handle value(raw_value);
        if (get_status == TDB_ERR_NOT_FOUND)
            return std::make_unique<tidesdb_result>(
                0, 0, 0, std::vector<materialized_cell>{});
        check_tidesdb(get_status, ORM_STATUS_DATASTORE_ERROR,
                      "read TidesDB entity for UPDATE");
        tidesdb_row row = decode_tidesdb_row(
            value.get(), value_size, limits_.max_parameter_bytes, limits_.max_assignments);
        if (!tidesdb_matches(plan.where_root, row, plan.table))
            return std::make_unique<tidesdb_result>(
                0, 0, 0, std::vector<materialized_cell>{});
        for (const assignment& assignment : plan.assignments) {
            require(assignment.column != settings_.id_column, ORM_STATUS_UNSUPPORTED,
                    "TidesDB UPDATE cannot change the configured id column");
            tidesdb_set_cell(row, assignment.column,
                             assignment.has_parameter
                                 ? tidesdb_cell_from_parameter(assignment.parameter)
                                 : tidesdb_null_cell(),
                             limits_.max_assignments);
        }
        const std::string encoded = encode_tidesdb_row(
            row, limits_.max_parameter_bytes, limits_.max_assignments);
        check_tidesdb(tidesdb_txn_put(
                          transaction, column_family_,
                          reinterpret_cast<const std::uint8_t*>(key.data()), key.size(),
                          reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size(),
                          expiration()),
                      ORM_STATUS_DATASTORE_ERROR,
                      "write TidesDB UPDATE");
        if (owned_transaction != nullptr)
            check_tidesdb(tidesdb_txn_commit(transaction),
                          ORM_STATUS_DATASTORE_ERROR,
                          "commit TidesDB UPDATE");
        return std::make_unique<tidesdb_result>(
            0, 0, 1, std::vector<materialized_cell>{});
    }

    std::unique_ptr<result_backend> execute_delete(const query_plan& plan,
                                                   tidesdb_txn_t* transaction)
    {
        const predicate& id = require_id_predicate(plan);
        const std::string key = entity_key(plan.table, id.parameter);
        transaction_handle owned_transaction;
        if (transaction == nullptr) {
            owned_transaction = begin_native_transaction(TDB_ISOLATION_SERIALIZABLE);
            transaction = owned_transaction.get();
        }
        std::uint8_t* raw_value = nullptr;
        std::size_t value_size = 0;
        const int get_status = tidesdb_txn_get(
            transaction, column_family_,
            reinterpret_cast<const std::uint8_t*>(key.data()), key.size(),
            &raw_value, &value_size);
        value_handle value(raw_value);
        if (get_status == TDB_ERR_NOT_FOUND)
            return std::make_unique<tidesdb_result>(
                0, 0, 0, std::vector<materialized_cell>{});
        check_tidesdb(get_status, ORM_STATUS_DATASTORE_ERROR,
                      "read TidesDB entity for DELETE");
        const tidesdb_row row = decode_tidesdb_row(
            value.get(), value_size, limits_.max_parameter_bytes, limits_.max_assignments);
        if (!tidesdb_matches(plan.where_root, row, plan.table))
            return std::make_unique<tidesdb_result>(
                0, 0, 0, std::vector<materialized_cell>{});
        check_tidesdb(tidesdb_txn_delete(
                          transaction, column_family_,
                          reinterpret_cast<const std::uint8_t*>(key.data()), key.size()),
                      ORM_STATUS_DATASTORE_ERROR,
                      "write TidesDB DELETE");
        if (owned_transaction != nullptr)
            check_tidesdb(tidesdb_txn_commit(transaction),
                          ORM_STATUS_DATASTORE_ERROR,
                          "commit TidesDB DELETE");
        return std::make_unique<tidesdb_result>(
            0, 0, 1, std::vector<materialized_cell>{});
    }

    tidesdb_settings settings_;
    connection_limits limits_;
    database_handle database_;
    tidesdb_column_family_t* column_family_ = nullptr;
    bool transaction_active_ = false;
};

} // namespace

std::unique_ptr<database_backend>
make_tidesdb_backend(const std::vector<std::string>& keywords,
                     const std::vector<std::string>& values,
                     const connection_limits& limits)
{
    return std::make_unique<tidesdb_backend>(keywords, values, limits);
}

} // namespace orm_c_detail
