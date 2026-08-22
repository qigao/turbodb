#include "orm_c_internal.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orm_c_detail {
namespace {

struct database_deleter {
    void operator()(sqlite3* database) const noexcept
    {
        if (database != nullptr)
            (void)sqlite3_close_v2(database);
    }
};

struct statement_deleter {
    void operator()(sqlite3_stmt* statement) const noexcept
    {
        if (statement != nullptr)
            (void)sqlite3_finalize(statement);
    }
};

using database_handle = std::unique_ptr<sqlite3, database_deleter>;
using statement_handle = std::unique_ptr<sqlite3_stmt, statement_deleter>;

orm_status_t sqlite_status(int code, orm_status_t fallback) noexcept
{
    switch (code & 0xff) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return ORM_STATUS_BUSY;
    case SQLITE_NOMEM:
        return ORM_STATUS_OUT_OF_MEMORY;
    case SQLITE_TOOBIG:
        return ORM_STATUS_LIMIT_EXCEEDED;
    case SQLITE_RANGE:
        return ORM_STATUS_OUT_OF_RANGE;
    case SQLITE_READONLY:
        return ORM_STATUS_INVALID_STATE;
    default:
        return fallback;
    }
}

[[noreturn]] void fail_sqlite(sqlite3* database,
                              int code,
                              orm_status_t fallback,
                              const char* operation)
{
    std::string message(operation);
    message += ": ";
    const char* detail = database != nullptr ? sqlite3_errmsg(database) : sqlite3_errstr(code);
    message += detail != nullptr ? detail : "unknown SQLite error";
    fail(sqlite_status(code, fallback), std::move(message));
}

int checked_sqlite_limit(std::uint64_t value, const char* role)
{
    if (value == 0 || value > static_cast<std::uint64_t>(INT_MAX))
        fail(ORM_STATUS_INVALID_ARGUMENT,
             std::string(role) + " must be in [1, INT_MAX]");
    return static_cast<int>(value);
}

void lower_limit(sqlite3* database, int category, int requested, const char* role)
{
    (void)sqlite3_limit(database, category, requested);
    const int actual = sqlite3_limit(database, category, -1);
    if (actual < requested)
        fail(ORM_STATUS_INVALID_ARGUMENT,
             std::string(role) + " exceeds this SQLite build's hard limit");
}

struct materialized_cell {
    bool is_null = true;
    std::string value;
};

class sqlite_result final : public result_backend {
public:
    sqlite_result(std::uint64_t rows,
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
                "result cell is SQL NULL");
        return vstr_from_buf(selected.value.data(), selected.value.size());
    }

private:
    const materialized_cell& get(std::uint64_t row, std::uint64_t column) const
    {
        require(row < rows_ && column < columns_,
                ORM_STATUS_OUT_OF_RANGE,
                "result row or column is out of range");
        const std::uint64_t index = row * columns_ + column;
        require(index < cells_.size(), ORM_STATUS_INTERNAL_ERROR,
                "materialized SQLite result has inconsistent dimensions");
        return cells_[static_cast<std::size_t>(index)];
    }

    std::uint64_t rows_;
    std::uint64_t columns_;
    std::uint64_t affected_rows_;
    std::vector<materialized_cell> cells_;
};

class sqlite_backend final : public database_backend {
    class explicit_transaction final : public transaction_backend {
    public:
        explicit_transaction(sqlite_backend& owner, orm_isolation_t isolation)
            : owner_(owner)
        {
            owner_.start_transaction(isolation);
            active_ = true;
        }

        ~explicit_transaction() override
        {
            rollback_noexcept();
        }

        std::unique_ptr<result_backend>
        execute_sql(std::string_view sql,
                    const std::vector<bound_parameter>& parameters,
                    bool structured_dml,
                    const connection_limits& limits) override
        {
            require_active();
            try {
                return owner_.execute_sql_impl(
                    sql, parameters, structured_dml, limits);
            } catch (...) {
                synchronize_after_error();
                throw;
            }
        }

        void commit() override { finish("commit", "commit SQLite transaction"); }
        void rollback() override { finish("rollback", "roll back SQLite transaction"); }

        void savepoint(std::string_view name) override
        {
            execute_control("savepoint \"" + std::string(name) + "\"");
        }

        void rollback_to_savepoint(std::string_view name) override
        {
            execute_control("rollback to savepoint \"" + std::string(name) + "\"");
        }

        void release_savepoint(std::string_view name) override
        {
            execute_control("release savepoint \"" + std::string(name) + "\"");
        }

    private:
        void require_active() const
        {
            require(active_, ORM_STATUS_INVALID_STATE,
                    "SQLite transaction is no longer active");
        }

        void synchronize_after_error() noexcept
        {
            if (sqlite3_get_autocommit(owner_.database_.get()) != 0) {
                active_ = false;
                owner_.transaction_active_ = false;
            }
        }

        void execute_control(std::string sql)
        {
            require_active();
            try {
                owner_.execute_control(sql);
            } catch (...) {
                synchronize_after_error();
                throw;
            }
        }

        void finish(const char* sql, const char* operation)
        {
            require_active();
            try {
                owner_.execute_control(sql);
            } catch (const status_error& error) {
                synchronize_after_error();
                fail(error.status(), std::string(operation) + ": " + error.what());
            }
            active_ = false;
            owner_.transaction_active_ = false;
        }

        void rollback_noexcept() noexcept
        {
            if (!active_)
                return;
            try {
                owner_.execute_control("rollback");
            } catch (...) {
            }
            active_ = false;
            owner_.transaction_active_ = false;
        }

        sqlite_backend& owner_;
        bool active_ = false;
    };

public:
    sqlite_backend(std::string filename,
                   sqlite_open_mode open_mode,
                   std::uint32_t busy_timeout_ms,
                   const connection_limits& limits)
    {
        int flags = SQLITE_OPEN_NOMUTEX;
        switch (open_mode) {
        case sqlite_open_mode::read_only:
            flags |= SQLITE_OPEN_READONLY;
            break;
        case sqlite_open_mode::read_write:
            flags |= SQLITE_OPEN_READWRITE;
            break;
        case sqlite_open_mode::read_write_create:
            flags |= SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
            break;
        default:
            fail(ORM_STATUS_INVALID_ARGUMENT, "unknown SQLite open mode");
        }

        sqlite3* raw_database = nullptr;
        const int open_code = sqlite3_open_v2(filename.c_str(), &raw_database, flags, nullptr);
        database_.reset(raw_database);
        if (open_code != SQLITE_OK)
            fail_sqlite(database_.get(), open_code,
                        ORM_STATUS_CONNECTION_ERROR, "open SQLite database");

        (void)sqlite3_extended_result_codes(database_.get(), 1);
        const int timeout_code = sqlite3_busy_timeout(
            database_.get(), static_cast<int>(busy_timeout_ms));
        if (timeout_code != SQLITE_OK)
            fail_sqlite(database_.get(), timeout_code,
                        ORM_STATUS_CONNECTION_ERROR, "configure SQLite busy timeout");

        lower_limit(database_.get(), SQLITE_LIMIT_SQL_LENGTH,
                    checked_sqlite_limit(limits.max_query_bytes, "max_query_bytes"),
                    "max_query_bytes");
        lower_limit(database_.get(), SQLITE_LIMIT_VARIABLE_NUMBER,
                    checked_sqlite_limit(limits.max_parameters, "max_parameters"),
                    "max_parameters");
        lower_limit(database_.get(), SQLITE_LIMIT_COLUMN,
                    checked_sqlite_limit(limits.max_columns, "max_columns"),
                    "max_columns");
        const std::uint64_t largest_value =
            std::max<std::uint64_t>(limits.max_parameter_bytes, limits.max_result_bytes);
        lower_limit(database_.get(), SQLITE_LIMIT_LENGTH,
                    checked_sqlite_limit(largest_value, "SQLite value byte limit"),
                    "SQLite value byte limit");
    }

    std::string placeholder(std::size_t one_based_index) const override
    {
        return "?" + std::to_string(one_based_index);
    }

    std::string pagination(std::optional<std::uint64_t> limit,
                           std::optional<std::uint64_t> offset) const override
    {
        std::string sql;
        if (limit)
            sql += " limit " + std::to_string(*limit);
        else if (offset)
            sql += " limit -1";
        if (offset)
            sql += " offset " + std::to_string(*offset);
        return sql;
    }

    std::unique_ptr<result_backend>
    execute_sql(std::string_view sql,
                const std::vector<bound_parameter>& parameters,
                bool structured_dml,
                const connection_limits& limits) override
    {
        require(!transaction_active_, ORM_STATUS_INVALID_STATE,
                "execute SQLite transaction queries through the transaction handle");
        return execute_sql_impl(sql, parameters, structured_dml, limits);
    }

    std::unique_ptr<transaction_backend>
    begin_transaction(orm_isolation_t isolation) override
    {
        require(!transaction_active_, ORM_STATUS_INVALID_STATE,
                "SQLite connection already has an active transaction");
        return std::make_unique<explicit_transaction>(*this, isolation);
    }

private:
    std::unique_ptr<result_backend>
    execute_sql_impl(std::string_view sql,
                     const std::vector<bound_parameter>& parameters,
                     bool structured_dml,
                     const connection_limits& limits)
    {
        require(sql.size() <= static_cast<std::size_t>(INT_MAX),
                ORM_STATUS_LIMIT_EXCEEDED,
                "SQL query exceeds SQLite's byte-count range");
        sqlite3_stmt* raw_statement = nullptr;
        const char* tail = nullptr;
        const int prepare_code = sqlite3_prepare_v2(database_.get(),
                                                    sql.data(),
                                                    static_cast<int>(sql.size()),
                                                    &raw_statement,
                                                    &tail);
        statement_handle statement(raw_statement);
        if (prepare_code != SQLITE_OK)
            fail_sqlite(database_.get(), prepare_code,
                        ORM_STATUS_SQL_ERROR, "prepare SQLite query");
        require(statement != nullptr, ORM_STATUS_SQL_ERROR,
                "SQLite query did not contain a statement");
        require(tail != nullptr, ORM_STATUS_INTERNAL_ERROR,
                "SQLite did not return a statement tail pointer");
        require(tail == sql.data() + sql.size() ||
                    only_ascii_space(tail, sql.data() + sql.size()),
                ORM_STATUS_SQL_ERROR,
                "SQLite query contains more than one statement");
        require(sqlite3_bind_parameter_count(statement.get()) ==
                    static_cast<int>(parameters.size()),
                ORM_STATUS_INTERNAL_ERROR,
                "SQLite parameter count does not match generated SQL");

        for (std::size_t index = 0; index < parameters.size(); ++index)
            bind(statement.get(), static_cast<int>(index + 1), parameters[index]);

        const int column_count = sqlite3_column_count(statement.get());
        require(column_count >= 0, ORM_STATUS_INTERNAL_ERROR,
                "SQLite returned a negative column count");
        require(static_cast<std::uint64_t>(column_count) <= limits.max_columns,
                ORM_STATUS_LIMIT_EXCEEDED,
                "result column count exceeds max_columns");

        std::vector<materialized_cell> cells;
        std::uint64_t rows = 0;
        std::uint64_t retained_bytes = 0;
        const sqlite3_int64 total_changes_before =
            sqlite3_total_changes64(database_.get());
        for (;;) {
            const int step_code = sqlite3_step(statement.get());
            if (step_code == SQLITE_DONE)
                break;
            if (step_code != SQLITE_ROW)
                fail_sqlite(database_.get(), step_code,
                            ORM_STATUS_SQL_ERROR, "step SQLite query");

            require(rows < limits.max_result_rows,
                    ORM_STATUS_LIMIT_EXCEEDED,
                    "result row count exceeds max_result_rows");
            reserve_row(cells,
                        static_cast<std::size_t>(column_count),
                        retained_bytes,
                        limits.max_result_bytes);
            for (int column = 0; column < column_count; ++column) {
                materialized_cell cell;
                const int type = sqlite3_column_type(statement.get(), column);
                if (type != SQLITE_NULL) {
                    const void* raw = nullptr;
                    if (type == SQLITE_BLOB)
                        raw = sqlite3_column_blob(statement.get(), column);
                    else
                        raw = sqlite3_column_text(statement.get(), column);
                    if (raw == nullptr)
                        fail_sqlite(database_.get(), sqlite3_errcode(database_.get()),
                                    ORM_STATUS_OUT_OF_MEMORY,
                                    "read SQLite result value");
                    const int byte_count = sqlite3_column_bytes(statement.get(), column);
                    require(byte_count >= 0, ORM_STATUS_INTERNAL_ERROR,
                            "SQLite returned a negative result byte count");
                    charge(retained_bytes, static_cast<std::uint64_t>(byte_count),
                           limits.max_result_bytes);
                    cell.is_null = false;
                    cell.value.assign(static_cast<const char*>(raw),
                                      static_cast<std::size_t>(byte_count));
                }
                cells.push_back(std::move(cell));
            }
            ++rows;
        }

        const sqlite3_int64 total_changes_after =
            sqlite3_total_changes64(database_.get());
        require(total_changes_after >= total_changes_before,
                ORM_STATUS_INTERNAL_ERROR,
                "SQLite total-change counter moved backwards");
        const std::uint64_t affected_rows = structured_dml
            ? static_cast<std::uint64_t>(sqlite3_changes64(database_.get()))
            : (column_count == 0
                   ? static_cast<std::uint64_t>(total_changes_after - total_changes_before)
                   : 0);
        return std::make_unique<sqlite_result>(
            rows,
            static_cast<std::uint64_t>(column_count),
            affected_rows,
            std::move(cells));
    }

    void execute_control(std::string_view sql)
    {
        char* detail = nullptr;
        const std::string command(sql);
        const int code = sqlite3_exec(database_.get(), command.c_str(),
                                      nullptr, nullptr, &detail);
        if (code == SQLITE_OK)
            return;
        std::string message = "execute SQLite transaction control: ";
        if (detail != nullptr) {
            message += detail;
            sqlite3_free(detail);
        } else {
            const char* fallback = sqlite3_errmsg(database_.get());
            message += fallback != nullptr ? fallback : "unknown SQLite error";
        }
        fail(sqlite_status(code, ORM_STATUS_SQL_ERROR), std::move(message));
    }

    void start_transaction(orm_isolation_t isolation)
    {
        require(isolation >= ORM_ISOLATION_READ_UNCOMMITTED &&
                    isolation <= ORM_ISOLATION_SERIALIZABLE,
                ORM_STATUS_INVALID_ARGUMENT,
                "unknown SQLite transaction isolation level");
        execute_control("begin");
        transaction_active_ = true;
    }

    static bool only_ascii_space(const char* begin, const char* end) noexcept
    {
        for (const char* current = begin; current != end; ++current) {
            if (std::isspace(static_cast<unsigned char>(*current)) == 0)
                return false;
        }
        return true;
    }

    void bind(sqlite3_stmt* statement, int index, const bound_parameter& parameter)
    {
        int code = SQLITE_MISUSE;
        switch (parameter.kind) {
        case ORM_VALUE_NULL:
            code = sqlite3_bind_null(statement, index);
            break;
        case ORM_VALUE_INT64:
            code = sqlite3_bind_int64(statement, index, parameter.int64_value);
            break;
        case ORM_VALUE_UINT64:
            require(parameter.uint64_value <=
                        static_cast<std::uint64_t>(INT64_MAX),
                    ORM_STATUS_OUT_OF_RANGE,
                    "unsigned parameter exceeds SQLite's signed 64-bit integer range");
            code = sqlite3_bind_int64(
                statement, index, static_cast<sqlite3_int64>(parameter.uint64_value));
            break;
        case ORM_VALUE_DOUBLE:
            code = sqlite3_bind_double(statement, index, parameter.double_value);
            break;
        case ORM_VALUE_BOOLEAN:
            code = sqlite3_bind_int(statement, index, parameter.boolean_value ? 1 : 0);
            break;
        case ORM_VALUE_TEXT:
            require(parameter.text.size() <= static_cast<std::size_t>(INT_MAX),
                    ORM_STATUS_LIMIT_EXCEEDED,
                    "text parameter exceeds SQLite's byte-count range");
            code = sqlite3_bind_text(statement,
                                     index,
                                     parameter.text.data(),
                                     static_cast<int>(parameter.text.size()),
                                     SQLITE_TRANSIENT);
            break;
        case ORM_VALUE_BLOB:
            require(parameter.binary.size() <= static_cast<std::size_t>(INT_MAX),
                    ORM_STATUS_LIMIT_EXCEEDED,
                    "blob parameter exceeds SQLite's byte-count range");
            code = sqlite3_bind_blob(statement,
                                     index,
                                     parameter.binary.data(),
                                     static_cast<int>(parameter.binary.size()),
                                     SQLITE_TRANSIENT);
            break;
        default:
            fail(ORM_STATUS_INTERNAL_ERROR,
                 "generated SQLite parameter has an unsupported type");
        }
        if (code != SQLITE_OK)
            fail_sqlite(database_.get(), code,
                        ORM_STATUS_SQL_ERROR, "bind SQLite parameter");
    }

    static void charge(std::uint64_t& total,
                       std::uint64_t amount,
                       std::uint64_t maximum)
    {
        require(total <= maximum && amount <= maximum - total,
                ORM_STATUS_LIMIT_EXCEEDED,
                "materialized result exceeds max_result_bytes");
        total += amount;
    }

    static void reserve_row(std::vector<materialized_cell>& cells,
                            std::size_t column_count,
                            std::uint64_t& retained_bytes,
                            std::uint64_t maximum)
    {
        require(column_count <= cells.max_size() - cells.size(),
                ORM_STATUS_LIMIT_EXCEEDED,
                "SQLite result cell count exceeds the platform container limit");
        const std::size_t required = cells.size() + column_count;
        if (required <= cells.capacity())
            return;

        const std::size_t old_capacity = cells.capacity();
        const std::uint64_t requested_bytes =
            static_cast<std::uint64_t>(required - old_capacity) * sizeof(materialized_cell);
        charge(retained_bytes, requested_bytes, maximum);
        cells.reserve(required);
        if (cells.capacity() > required) {
            const std::uint64_t extra_bytes =
                static_cast<std::uint64_t>(cells.capacity() - required) *
                sizeof(materialized_cell);
            charge(retained_bytes, extra_bytes, maximum);
        }
    }

    database_handle database_;
    bool transaction_active_ = false;
};

} // namespace

std::unique_ptr<database_backend>
make_sqlite_backend(std::string filename,
                    sqlite_open_mode open_mode,
                    std::uint32_t busy_timeout_ms,
                    const connection_limits& limits)
{
    return std::make_unique<sqlite_backend>(
        std::move(filename), open_mode, busy_timeout_ms, limits);
}

} // namespace orm_c_detail
