#include "orm_c_internal.hpp"

#include "pg_detail.hpp"

#include <charconv>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace orm_c_detail {
namespace {

orm_status_t map_postgres_error(std::string_view sqlstate) noexcept
{
    if (sqlstate == "23505" || sqlstate == "23503" ||
        sqlstate == "40001" || sqlstate == "40P01" ||
        sqlstate == "55P03")
        return ORM_STATUS_BUSY;
    if (sqlstate.size() >= 2 && sqlstate[0] == '0' && sqlstate[1] == '8')
        return ORM_STATUS_CONNECTION_ERROR;
    return ORM_STATUS_SQL_ERROR;
}

constexpr unsigned int pg_bytea_oid = 17;

int hex_digit(char value) noexcept
{
    if (value >= '0' && value <= '9')
        return value - '0';
    if (value >= 'a' && value <= 'f')
        return value - 'a' + 10;
    if (value >= 'A' && value <= 'F')
        return value - 'A' + 10;
    return -1;
}

std::string decode_pg_bytea(std::string_view input)
{
    // libpq returns BYTEA values in text format. The server default is hex
    // (\x...) but legacy escape format is still possible, so support both.
    if (input.size() >= 2 && input[0] == '\\' && input[1] == 'x') {
        require((input.size() - 2) % 2 == 0, ORM_STATUS_DATASTORE_ERROR,
                "PostgreSQL returned a malformed bytea hex value");
        std::string output;
        output.reserve((input.size() - 2) / 2);
        for (std::size_t index = 2; index < input.size(); index += 2) {
            const int high = hex_digit(input[index]);
            const int low = hex_digit(input[index + 1]);
            require(high >= 0 && low >= 0, ORM_STATUS_DATASTORE_ERROR,
                    "PostgreSQL returned an invalid bytea hex digit");
            output.push_back(static_cast<char>((high << 4) | low));
        }
        return output;
    }

    std::string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size();) {
        if (input[index] != '\\') {
            output.push_back(input[index++]);
            continue;
        }
        require(index + 1 < input.size(), ORM_STATUS_DATASTORE_ERROR,
                "PostgreSQL returned a truncated bytea escape value");
        if (input[index + 1] == '\\') {
            output.push_back('\\');
            index += 2;
            continue;
        }
        require(index + 3 < input.size() &&
                    input[index + 1] >= '0' && input[index + 1] <= '3' &&
                    input[index + 2] >= '0' && input[index + 2] <= '7' &&
                    input[index + 3] >= '0' && input[index + 3] <= '7',
                ORM_STATUS_DATASTORE_ERROR,
                "PostgreSQL returned an invalid bytea escape sequence");
        const int byte_value = ((input[index + 1] - '0') << 6) |
                               ((input[index + 2] - '0') << 3) |
                               (input[index + 3] - '0');
        output.push_back(static_cast<char>(byte_value));
        index += 4;
    }
    return output;
}

class postgres_result final : public result_backend {
public:
    postgres_result(pg_ormlite_detail::result_handle result,
                    int rows,
                    int columns,
                    std::uint64_t affected_rows)
        : result_(std::move(result)), rows_(rows), columns_(columns),
          affected_rows_(affected_rows)
    {
        for (int column = 0; column < columns_; ++column) {
            if (PQftype(result_.get(), column) == pg_bytea_oid) {
                has_bytea_ = true;
                break;
            }
        }
        if (has_bytea_ && rows_ != 0) {
            cells_.resize(static_cast<std::size_t>(rows_) *
                          static_cast<std::size_t>(columns_));
            for (int row = 0; row < rows_; ++row) {
                for (int column = 0; column < columns_; ++column) {
                    std::size_t index = static_cast<std::size_t>(row) *
                                            static_cast<std::size_t>(columns_) +
                                        static_cast<std::size_t>(column);
                    if (PQgetisnull(result_.get(), row, column) != 0)
                        continue;
                    const int length = PQgetlength(result_.get(), row, column);
                    require(length >= 0, ORM_STATUS_INTERNAL_ERROR,
                            "libpq returned a negative field length");
                    const char* data = PQgetvalue(result_.get(), row, column);
                    require(data != nullptr, ORM_STATUS_INTERNAL_ERROR,
                            "libpq returned a null field pointer for a non-null value");
                    if (PQftype(result_.get(), column) == pg_bytea_oid)
                        cells_[index] =
                            decode_pg_bytea({data, static_cast<std::size_t>(length)});
                    else
                        cells_[index].assign(data, static_cast<std::size_t>(length));
                }
            }
        }
    }

    std::uint64_t rows() const noexcept override
    {
        return static_cast<std::uint64_t>(rows_);
    }

    std::uint64_t columns() const noexcept override
    {
        return static_cast<std::uint64_t>(columns_);
    }

    std::uint64_t affected_rows() const noexcept override { return affected_rows_; }

    bool is_null(std::uint64_t row, std::uint64_t column) const override
    {
        check_coordinates(row, column);
        return PQgetisnull(result_.get(), static_cast<int>(row), static_cast<int>(column)) != 0;
    }

    vstr cell(std::uint64_t row, std::uint64_t column) const override
    {
        check_coordinates(row, column);
        const int row_index = static_cast<int>(row);
        const int column_index = static_cast<int>(column);
        require(PQgetisnull(result_.get(), row_index, column_index) == 0,
                ORM_STATUS_NULL_VALUE,
                "result cell is SQL NULL");
        if (has_bytea_) {
            const std::size_t index =
                static_cast<std::size_t>(row_index) * static_cast<std::size_t>(columns_) +
                static_cast<std::size_t>(column_index);
            require(index < cells_.size(), ORM_STATUS_INTERNAL_ERROR,
                    "materialized PostgreSQL result has inconsistent dimensions");
            return vstr_from_buf(cells_[index].data(), cells_[index].size());
        }
        const int length = PQgetlength(result_.get(), row_index, column_index);
        require(length >= 0, ORM_STATUS_INTERNAL_ERROR,
                "libpq returned a negative field length");
        const char* data = PQgetvalue(result_.get(), row_index, column_index);
        require(data != nullptr, ORM_STATUS_INTERNAL_ERROR,
                "libpq returned a null field pointer for a non-null value");
        return vstr_from_buf(data, static_cast<std::size_t>(length));
    }

private:
    void check_coordinates(std::uint64_t row, std::uint64_t column) const
    {
        require(row < rows() && column < columns(),
                ORM_STATUS_OUT_OF_RANGE,
                "result row or column is out of range");
    }

    pg_ormlite_detail::result_handle result_;
    int rows_;
    int columns_;
    std::uint64_t affected_rows_;
    bool has_bytea_ = false;
    std::vector<std::string> cells_;
};

class postgres_backend final : public database_backend {
    class explicit_transaction final : public transaction_backend {
    public:
        explicit_transaction(postgres_backend& owner, orm_isolation_t isolation)
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
            require(active_, ORM_STATUS_INVALID_STATE,
                    "PostgreSQL transaction is no longer active");
            return owner_.execute_sql_impl(
                sql, parameters, structured_dml, limits);
        }

        void commit() override
        {
            finish("commit", "commit PostgreSQL transaction");
        }

        void rollback() override
        {
            finish("rollback", "roll back PostgreSQL transaction");
        }

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
        void execute_control(std::string sql)
        {
            require(active_, ORM_STATUS_INVALID_STATE,
                    "PostgreSQL transaction is no longer active");
            owner_.execute_control(sql);
        }

        void finish(const char* sql, const char* operation)
        {
            require(active_, ORM_STATUS_INVALID_STATE,
                    "PostgreSQL transaction is no longer active");
            try {
                owner_.execute_control(sql);
            } catch (const status_error& error) {
                fail(error.status(), std::string(operation) + ": " + error.what(),
                     error.backend_code());
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

        postgres_backend& owner_;
        bool active_ = false;
    };

public:
    postgres_backend(const std::vector<std::string>& keywords,
                     const std::vector<std::string>& values)
    {
        std::vector<const char*> keyword_pointers;
        std::vector<const char*> value_pointers;
        keyword_pointers.reserve(keywords.size() + 1);
        value_pointers.reserve(values.size() + 1);
        for (std::size_t index = 0; index < keywords.size(); ++index) {
            keyword_pointers.push_back(keywords[index].c_str());
            value_pointers.push_back(values[index].c_str());
        }
        keyword_pointers.push_back(nullptr);
        value_pointers.push_back(nullptr);

        connection_ = pg_ormlite_detail::adopt_connection(
            PQconnectdbParams(keyword_pointers.data(), value_pointers.data(), 0));
        if (connection_ == nullptr)
            fail(ORM_STATUS_CONNECTION_ERROR,
                 "libpq failed to allocate a PostgreSQL connection");
        if (PQstatus(connection_.get()) != CONNECTION_OK)
            fail(ORM_STATUS_CONNECTION_ERROR,
                 pg_ormlite_detail::connection_error(connection_.get()));
    }

    std::string placeholder(std::size_t one_based_index) const override
    {
        return "$" + std::to_string(one_based_index);
    }

    std::string pagination(std::optional<std::uint64_t> limit,
                           std::optional<std::uint64_t> offset) const override
    {
        std::string sql;
        if (limit)
            sql += " limit " + std::to_string(*limit);
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
                "execute PostgreSQL transaction queries through the transaction handle");
        return execute_sql_impl(sql, parameters, structured_dml, limits);
    }

    std::unique_ptr<transaction_backend>
    begin_transaction(orm_isolation_t isolation) override
    {
        require(!transaction_active_, ORM_STATUS_INVALID_STATE,
                "PostgreSQL connection already has an active transaction");
        return std::make_unique<explicit_transaction>(*this, isolation);
    }

private:
    std::unique_ptr<result_backend>
    execute_sql_impl(std::string_view sql,
                     const std::vector<bound_parameter>& parameters,
                     bool,
                     const connection_limits& limits)
    {
        std::vector<const char*> parameter_values;
        parameter_values.reserve(parameters.size());
        for (const bound_parameter& parameter : parameters) {
            parameter_values.push_back(parameter.kind == ORM_VALUE_NULL
                                           ? nullptr
                                           : parameter.text.c_str());
        }
        const std::string sql_text(sql);
        auto result = pg_ormlite_detail::adopt_result(
            PQexecParams(connection_.get(),
                         sql_text.c_str(),
                         static_cast<int>(parameter_values.size()),
                         nullptr,
                         parameter_values.empty() ? nullptr : parameter_values.data(),
                         nullptr,
                         nullptr,
                         0));
        const ExecStatusType status =
            result != nullptr ? PQresultStatus(result.get()) : PGRES_FATAL_ERROR;
        if (result == nullptr ||
            (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK)) {
            const std::string sqlstate =
                pg_ormlite_detail::result_sqlstate(result.get());
            fail(map_postgres_error(sqlstate),
                 pg_ormlite_detail::result_error(connection_.get(), result.get()),
                 sqlstate);
        }

        const int rows = PQntuples(result.get());
        const int columns = PQnfields(result.get());
        require(rows >= 0 && columns >= 0, ORM_STATUS_INTERNAL_ERROR,
                "libpq returned a negative result dimension");
        require(static_cast<std::uint64_t>(rows) <= limits.max_result_rows,
                ORM_STATUS_LIMIT_EXCEEDED,
                "result row count exceeds max_result_rows");
        require(static_cast<std::uint64_t>(columns) <= limits.max_columns,
                ORM_STATUS_LIMIT_EXCEEDED,
                "result column count exceeds max_columns");

        std::uint64_t result_bytes = 0;
        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column < columns; ++column) {
                if (PQgetisnull(result.get(), row, column) != 0)
                    continue;
                const int length = PQgetlength(result.get(), row, column);
                require(length >= 0, ORM_STATUS_INTERNAL_ERROR,
                        "libpq returned a negative field length");
                const std::uint64_t size = static_cast<std::uint64_t>(length);
                require(result_bytes <= limits.max_result_bytes &&
                            size <= limits.max_result_bytes - result_bytes,
                        ORM_STATUS_LIMIT_EXCEEDED,
                        "result payload exceeds max_result_bytes");
                result_bytes += size;
            }
        }

        std::uint64_t affected_rows = 0;
        const char* affected_text = PQcmdTuples(result.get());
        if (affected_text != nullptr && *affected_text != '\0') {
            const char* end = affected_text + std::char_traits<char>::length(affected_text);
            const auto parsed = std::from_chars(affected_text, end, affected_rows);
            require(parsed.ec == std::errc{} && parsed.ptr == end,
                    ORM_STATUS_INTERNAL_ERROR,
                    "libpq returned an invalid affected-row count");
        }
        return std::make_unique<postgres_result>(
            std::move(result), rows, columns, affected_rows);
    }

    void execute_control(std::string_view sql)
    {
        const connection_limits no_result_limits{};
        (void)execute_sql_impl(sql, {}, false, no_result_limits);
    }

    void start_transaction(orm_isolation_t isolation)
    {
        const char* sql = nullptr;
        switch (isolation) {
        case ORM_ISOLATION_READ_UNCOMMITTED:
        case ORM_ISOLATION_READ_COMMITTED:
            sql = "begin isolation level read committed";
            break;
        case ORM_ISOLATION_REPEATABLE_READ:
        case ORM_ISOLATION_SNAPSHOT:
            sql = "begin isolation level repeatable read";
            break;
        case ORM_ISOLATION_SERIALIZABLE:
            sql = "begin isolation level serializable";
            break;
        default:
            fail(ORM_STATUS_INVALID_ARGUMENT,
                 "unknown PostgreSQL transaction isolation level");
        }
        execute_control(sql);
        transaction_active_ = true;
    }

    pg_ormlite_detail::connection_handle connection_;
    bool transaction_active_ = false;
};

} // namespace

std::unique_ptr<database_backend>
make_postgres_backend(const std::vector<std::string>& keywords,
                      const std::vector<std::string>& values,
                      const connection_limits&)
{
    return std::make_unique<postgres_backend>(keywords, values);
}

} // namespace orm_c_detail
