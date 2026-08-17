#ifndef PG_QUERY_OBJECT_HPP
#define PG_QUERY_OBJECT_HPP

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "pg_detail.hpp"
#include "model_detail.hpp"

namespace pg_query_object {

namespace detail {

template<typename>
struct always_false : std::false_type {};

template<typename T>
std::string parameter_value(T&& value)
{
    using raw_type = std::remove_reference_t<T>;
    using value_type = std::decay_t<T>;

    if constexpr (std::is_array_v<raw_type>) {
        using element_type = std::remove_cv_t<std::remove_extent_t<raw_type>>;
        static_assert(std::is_same_v<element_type, char>,
                      "SQL array parameters must be character arrays");
        constexpr std::size_t size = std::extent_v<raw_type>;
        const auto end = std::find(value, value + size, '\0');
        if (end == value + size)
            throw std::invalid_argument("SQL character array parameter is not null terminated");
        return std::string(value, end);
    } else if constexpr (std::is_same_v<value_type, std::string>) {
        if (value.find('\0') != std::string::npos)
            throw std::invalid_argument("SQL text parameter contains an embedded null byte");
        return std::forward<T>(value);
    } else if constexpr (std::is_same_v<value_type, std::string_view>) {
        if (value.find('\0') != std::string_view::npos)
            throw std::invalid_argument("SQL text parameter contains an embedded null byte");
        return std::string(value);
    } else if constexpr (std::is_same_v<value_type, const char*> ||
                         std::is_same_v<value_type, char*>) {
        if (value == nullptr)
            throw std::invalid_argument("SQL string parameter is null");
        return std::string(value);
    } else if constexpr (std::is_same_v<value_type, bool>) {
        return value ? "true" : "false";
    } else if constexpr (std::is_enum_v<value_type>) {
        return parameter_value(static_cast<std::underlying_type_t<value_type>>(value));
    } else if constexpr (std::is_integral_v<value_type>) {
        return std::to_string(value);
    } else if constexpr (std::is_floating_point_v<value_type>) {
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(std::numeric_limits<value_type>::max_digits10) << value;
        if (!stream)
            throw std::invalid_argument("failed to encode floating-point SQL parameter");
        return stream.str();
    } else {
        static_assert(always_false<value_type>::value,
                      "unsupported SQL parameter type; use a string, arithmetic type, enum, or character array");
    }
}

template<typename Integer>
Integer parse_integer(const char* data, std::size_t size)
{
    static_assert(std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>,
                  "parse_integer requires a non-bool integral type");

    if constexpr (std::is_signed_v<Integer>) {
        long long parsed = 0;
        const auto result = std::from_chars(data, data + size, parsed);
        if (result.ec != std::errc{} || result.ptr != data + size ||
            parsed < static_cast<long long>(std::numeric_limits<Integer>::min()) ||
            parsed > static_cast<long long>(std::numeric_limits<Integer>::max()))
            throw std::range_error("PostgreSQL integer result is invalid or out of range");
        return static_cast<Integer>(parsed);
    } else {
        unsigned long long parsed = 0;
        const auto result = std::from_chars(data, data + size, parsed);
        if (result.ec != std::errc{} || result.ptr != data + size ||
            parsed > static_cast<unsigned long long>(std::numeric_limits<Integer>::max()))
            throw std::range_error("PostgreSQL unsigned integer result is invalid or out of range");
        return static_cast<Integer>(parsed);
    }
}

template<typename Floating>
Floating parse_floating(const char* data, std::size_t size)
{
    std::istringstream stream(std::string(data, size));
    stream.imbue(std::locale::classic());
    Floating parsed{};
    stream >> std::noskipws >> parsed;
    if (!stream || !stream.eof())
        throw std::range_error("PostgreSQL floating-point result is invalid or out of range");
    return parsed;
}

} // namespace detail

template<typename ResultType>
struct selectable {
    using result_type = ResultType;

    selectable(std::string_view field, std::string_view table_name, std::string_view operation)
        : expression_(operation.empty()
                          ? "(" + std::string(field) + ")"
                          : std::string(operation) + "(" + std::string(field) + ")"),
          table_name_(table_name)
    {
    }

    const std::string& to_string() const noexcept { return expression_; }
    const std::string& table_name() const noexcept { return table_name_; }

private:
    std::string expression_;
    std::string table_name_;
};

template<typename>
struct is_selectable : std::false_type {};

template<typename T>
struct is_selectable<selectable<T>> : std::true_type {};

class expr {
public:
    expr(std::string_view field, std::string_view table_name)
        : sql_template_(field), table_name_(table_name)
    {
    }

    expr(const expr&) = default;
    expr(expr&&) noexcept = default;
    expr& operator=(const expr&) = default;
    expr& operator=(expr&&) noexcept = default;

    template<typename T>
    expr operator==(T&& value) const { return make_comparison("=", std::forward<T>(value)); }

    template<typename T,
             std::enable_if_t<!std::is_same_v<std::decay_t<T>, expr>, int> = 0>
    expr operator=(T&& value) const { return make_comparison("=", std::forward<T>(value)); }

    template<typename T>
    expr operator!=(T&& value) const { return make_comparison("!=", std::forward<T>(value)); }

    template<typename T>
    expr operator<(T&& value) const { return make_comparison("<", std::forward<T>(value)); }

    template<typename T>
    expr operator>(T&& value) const { return make_comparison(">", std::forward<T>(value)); }

    template<typename T>
    expr operator<=(T&& value) const { return make_comparison("<=", std::forward<T>(value)); }

    template<typename T>
    expr operator>=(T&& value) const { return make_comparison(">=", std::forward<T>(value)); }

    template<typename T>
    expr operator%(T&& value) const { return make_comparison("like", std::forward<T>(value)); }

    template<typename T>
    expr operator^(T&& value) const { return make_comparison("not like", std::forward<T>(value)); }

    expr operator&&(const expr& value) const { return combine("and", value, true); }
    expr operator||(const expr& value) const { return combine("or", value, true); }
    expr operator|(const expr& value) const { return combine(",", value, false); }

    std::string to_string() const
    {
        std::string sql;
        std::vector<std::string> ignored;
        append_to(sql, ignored);
        return sql;
    }

    const std::string& table_name() const noexcept { return table_name_; }

    void append_to(std::string& sql, std::vector<std::string>& parameters) const
    {
        std::size_t parameter_index = 0;
        for (char character : sql_template_) {
            if (character != parameter_marker) {
                sql.push_back(character);
                continue;
            }
            if (parameter_index >= parameters_.size())
                throw std::logic_error("SQL expression parameter marker mismatch");
            parameters.push_back(parameters_[parameter_index++]);
            sql += "$" + std::to_string(parameters.size());
        }
        if (parameter_index != parameters_.size())
            throw std::logic_error("SQL expression parameter count mismatch");
    }

private:
    static constexpr char parameter_marker = '\x1f';

    expr(std::string sql_template,
         std::string table_name,
         std::vector<std::string> parameters)
        : sql_template_(std::move(sql_template)),
          table_name_(std::move(table_name)),
          parameters_(std::move(parameters))
    {
    }

    void require_same_table(const expr& value) const
    {
        if (value.table_name_ != table_name_)
            throw std::invalid_argument("cannot combine ORM expressions from different tables");
    }

    template<typename T>
    expr make_comparison(const char* operation, T&& value) const
    {
        if constexpr (std::is_same_v<std::decay_t<T>, expr>) {
            require_same_table(value);
            std::vector<std::string> parameters = parameters_;
            parameters.insert(parameters.end(), value.parameters_.begin(), value.parameters_.end());
            return expr(sql_template_ + " " + operation + " " + value.sql_template_,
                        table_name_,
                        std::move(parameters));
        } else {
            std::vector<std::string> parameters = parameters_;
            parameters.push_back(detail::parameter_value(std::forward<T>(value)));
            return expr(sql_template_ + " " + operation + " " + parameter_marker,
                        table_name_,
                        std::move(parameters));
        }
    }

    expr combine(const char* operation, const expr& value, bool parenthesize) const
    {
        require_same_table(value);
        std::vector<std::string> parameters = parameters_;
        parameters.insert(parameters.end(), value.parameters_.begin(), value.parameters_.end());
        const std::string combined = parenthesize
                                         ? "(" + sql_template_ + ") " + operation + " (" + value.sql_template_ + ")"
                                         : sql_template_ + " " + operation + " " + value.sql_template_;
        return expr(combined, table_name_, std::move(parameters));
    }

    std::string sql_template_;
    std::string table_name_;
    std::vector<std::string> parameters_;
};

template<typename QueryResult>
class query_object {
    template<typename>
    friend class query_object;

    struct bound_query {
        std::string sql;
        std::vector<std::string> parameters;
    };

public:
    query_object(PGconn* connection, std::string_view table_name)
        : query_object(pg_ormlite_detail::borrow_connection(connection), table_name)
    {
    }

    query_object(PGconn* connection,
                 std::string_view table_name,
                 const std::string& delete_sql,
                 const std::string& update_sql)
        : query_object(pg_ormlite_detail::borrow_connection(connection),
                       table_name,
                       delete_sql,
                       update_sql)
    {
    }

    query_object(pg_ormlite_detail::connection_handle connection, std::string_view table_name)
        : connection_(std::move(connection)), table_name_(table_name)
    {
    }

    query_object(pg_ormlite_detail::connection_handle connection,
                 std::string_view table_name,
                 const std::string& delete_sql,
                 const std::string& update_sql)
        : connection_(std::move(connection)),
          table_name_(table_name),
          delete_sql_(update_sql.empty() ? delete_sql + " from " + std::string(table_name) : ""),
          update_sql_(delete_sql.empty() ? update_sql + " " + std::string(table_name) : "")
    {
    }

    template<typename... Args>
    auto select(Args&&... args)
    {
        if constexpr (sizeof...(Args) == 0) {
            auto next = rebind<QueryResult>();
            next.select_sql_ = "select * from " + table_name_;
            return next;
        } else {
            static_assert((is_selectable<std::decay_t<Args>>::value && ...),
                          "query_object::select arguments must be produced by ORM_TYPE or ORM aggregate macros");
            using selected_result = std::tuple<typename std::decay_t<Args>::result_type...>;
            auto next = rebind<selected_result>();
            next.select_sql_ = "select ";
            std::size_t index = 0;
            const auto append = [&](const auto& argument) {
                if (argument.table_name() != table_name_)
                    throw std::invalid_argument("cannot select fields from different tables");
                next.select_sql_ += argument.to_string();
                if (++index != sizeof...(Args))
                    next.select_sql_ += ", ";
            };
            (append(args), ...);
            next.select_sql_ += " from " + table_name_;
            return next;
        }
    }

    query_object& set(const expr& expression)
    {
        require_same_table(expression);
        set_expression_ = expression;
        return *this;
    }

    query_object& where(const expr& expression)
    {
        require_same_table(expression);
        where_expression_ = expression;
        return *this;
    }

    query_object& group_by(const expr& expression)
    {
        require_same_table(expression);
        group_by_expression_ = expression;
        return *this;
    }

    query_object& having(const expr& expression)
    {
        require_same_table(expression);
        having_expression_ = expression;
        return *this;
    }

    query_object& order_by(const expr& expression)
    {
        require_same_table(expression);
        order_by_expression_ = expression;
        order_descending_ = false;
        return *this;
    }

    query_object& order_by_desc(const expr& expression)
    {
        require_same_table(expression);
        order_by_expression_ = expression;
        order_descending_ = true;
        return *this;
    }

    query_object& limit(std::size_t count)
    {
        limit_ = count;
        return *this;
    }

    query_object& offset(std::size_t count)
    {
        offset_ = count;
        return *this;
    }

    std::string to_string() const { return build_query().sql; }
    std::vector<std::string> parameters() const { return build_query().parameters; }

    template<typename T>
    void assign_value(T&& value, int row, int column)
    {
        if (active_result_ == nullptr)
            throw std::logic_error("assign_value requires an active PostgreSQL result");
        assign_value_from(active_result_, std::forward<T>(value), row, column);
    }

    std::vector<QueryResult> to_vector()
    {
        return query<QueryResult>(build_query());
    }

    bool execute()
    {
        const auto statement = build_query();
        auto result = pg_ormlite_detail::execute_params(connection_.get(),
                                                        statement.sql,
                                                        statement.parameters);
        return result != nullptr && PQresultStatus(result.get()) == PGRES_COMMAND_OK;
    }

private:
    query_object(pg_ormlite_detail::connection_handle connection,
                 std::string table_name,
                 std::string select_sql,
                 std::string delete_sql,
                 std::string update_sql,
                 std::optional<expr> set_expression,
                 std::optional<expr> where_expression,
                 std::optional<expr> group_by_expression,
                 std::optional<expr> having_expression,
                 std::optional<expr> order_by_expression,
                 bool order_descending,
                 std::optional<std::size_t> limit,
                 std::optional<std::size_t> offset)
        : connection_(std::move(connection)),
          table_name_(std::move(table_name)),
          select_sql_(std::move(select_sql)),
          delete_sql_(std::move(delete_sql)),
          update_sql_(std::move(update_sql)),
          set_expression_(std::move(set_expression)),
          where_expression_(std::move(where_expression)),
          group_by_expression_(std::move(group_by_expression)),
          having_expression_(std::move(having_expression)),
          order_by_expression_(std::move(order_by_expression)),
          order_descending_(order_descending),
          limit_(limit),
          offset_(offset)
    {
    }

    template<typename NewResult>
    query_object<NewResult> rebind() const
    {
        return query_object<NewResult>(connection_,
                                       table_name_,
                                       select_sql_,
                                       delete_sql_,
                                       update_sql_,
                                       set_expression_,
                                       where_expression_,
                                       group_by_expression_,
                                       having_expression_,
                                       order_by_expression_,
                                       order_descending_,
                                       limit_,
                                       offset_);
    }

    void require_same_table(const expr& expression) const
    {
        if (expression.table_name() != table_name_)
            throw std::invalid_argument("ORM expression belongs to a different table");
    }

    static void append_expression(bound_query& statement,
                                  std::string_view prefix,
                                  const expr& expression,
                                  std::string_view suffix = {})
    {
        statement.sql += prefix;
        expression.append_to(statement.sql, statement.parameters);
        statement.sql += suffix;
    }

    bound_query build_query() const
    {
        bound_query statement;
        if (select_sql_.empty() && delete_sql_.empty() && update_sql_.empty())
            statement.sql = "select * from " + table_name_;
        else
            statement.sql = update_sql_ + delete_sql_ + select_sql_;

        if (set_expression_)
            append_expression(statement, " set ", *set_expression_);
        if (where_expression_)
            append_expression(statement, " where (", *where_expression_, ")");
        if (group_by_expression_)
            append_expression(statement, " group by (", *group_by_expression_, ")");
        if (having_expression_)
            append_expression(statement, " having (", *having_expression_, ")");
        if (order_by_expression_)
            append_expression(statement,
                              " order by ",
                              *order_by_expression_,
                              order_descending_ ? " desc" : " asc");
        if (limit_)
            statement.sql += " limit " + std::to_string(*limit_);
        if (offset_)
            statement.sql += " offset " + std::to_string(*offset_);
        statement.sql += ";";
        return statement;
    }

    template<typename T>
    static void assign_value_from(PGresult* result, T&& value, int row, int column)
    {
        using raw_type = std::remove_reference_t<T>;
        using value_type = std::remove_cv_t<raw_type>;

        if (row < 0 || row >= PQntuples(result) || column < 0 || column >= PQnfields(result))
            throw std::out_of_range("PostgreSQL result row or column is out of range");
        if (PQgetisnull(result, row, column) != 0)
            throw std::runtime_error("cannot assign SQL NULL to a non-nullable C++ field");

        const char* data = PQgetvalue(result, row, column);
        const int length = PQgetlength(result, row, column);
        if (data == nullptr || length < 0)
            throw std::runtime_error("libpq returned an invalid field value");
        const auto size = static_cast<std::size_t>(length);

        if constexpr (std::is_same_v<value_type, bool>) {
            const std::string text(data, size);
            if (text == "t" || text == "true" || text == "1")
                value = true;
            else if (text == "f" || text == "false" || text == "0")
                value = false;
            else
                throw std::range_error("PostgreSQL boolean result is invalid");
        } else if constexpr (std::is_integral_v<value_type>) {
            value = detail::parse_integer<value_type>(data, size);
        } else if constexpr (std::is_enum_v<value_type>) {
            using underlying_type = std::underlying_type_t<value_type>;
            value = static_cast<value_type>(detail::parse_integer<underlying_type>(data, size));
        } else if constexpr (std::is_floating_point_v<value_type>) {
            value = detail::parse_floating<value_type>(data, size);
        } else if constexpr (std::is_same_v<value_type, std::string>) {
            value.assign(data, size);
        } else if constexpr (std::is_array_v<value_type>) {
            using element_type = std::remove_cv_t<std::remove_extent_t<value_type>>;
            static_assert(std::is_same_v<element_type, char>,
                          "PostgreSQL array results require a character array destination");
            constexpr std::size_t capacity = std::extent_v<value_type>;
            if (size >= capacity)
                throw std::length_error("PostgreSQL text result does not fit in the destination character array");
            std::copy_n(data, size, value);
            value[size] = '\0';
        } else {
            static_assert(detail::always_false<value_type>::value,
                          "unsupported PostgreSQL result type");
        }
    }

    template<typename T>
    std::enable_if_t<orm::model::is_entity<T>::value, std::vector<T>>
    query(const bound_query& statement)
    {
        std::vector<T> values;
        auto result = pg_ormlite_detail::execute_params(connection_.get(),
                                                        statement.sql,
                                                        statement.parameters);
        if (result == nullptr || PQresultStatus(result.get()) != PGRES_TUPLES_OK)
            throw std::runtime_error(pg_ormlite_detail::result_error(connection_.get(), result.get()));
        if (PQnfields(result.get()) != static_cast<int>(orm::model::get_value<T>()))
            throw std::runtime_error("PostgreSQL result column count does not match entity model");

        active_result_ = result.get();
        try {
            const int row_count = PQntuples(result.get());
            values.reserve(static_cast<std::size_t>(row_count));
            for (int row = 0; row < row_count; ++row) {
                T value{};
                orm::model::for_each(value, [&](auto member, auto, auto index) {
                    assign_value(value.*member, row, static_cast<int>(decltype(index)::value));
                });
                values.push_back(std::move(value));
            }
        } catch (...) {
            active_result_ = nullptr;
            throw;
        }
        active_result_ = nullptr;
        return values;
    }

    template<typename T>
    std::enable_if_t<!orm::model::is_entity<T>::value, std::vector<T>>
    query(const bound_query& statement)
    {
        static_assert(orm::model::is_tuple<std::decay_t<T>>::value,
                      "query results without an entity model must be std::tuple instances");

        std::vector<T> values;
        auto result = pg_ormlite_detail::execute_params(connection_.get(),
                                                        statement.sql,
                                                        statement.parameters);
        if (result == nullptr || PQresultStatus(result.get()) != PGRES_TUPLES_OK)
            throw std::runtime_error(pg_ormlite_detail::result_error(connection_.get(), result.get()));

        active_result_ = result.get();
        try {
            const int row_count = PQntuples(result.get());
            values.reserve(static_cast<std::size_t>(row_count));
            for (int row = 0; row < row_count; ++row) {
                T value{};
                int column = 0;
                orm::model::for_each(value, [&](auto& item, auto) {
                    if constexpr (orm::model::is_entity_v<std::decay_t<decltype(item)>>) {
                        using item_type = std::decay_t<decltype(item)>;
                        item_type nested{};
                        orm::model::for_each(nested, [&](auto member, auto, auto) {
                            assign_value(nested.*member, row, column++);
                        });
                        item = std::move(nested);
                    } else {
                        assign_value(item, row, column++);
                    }
                });
                if (column != PQnfields(result.get()))
                    throw std::runtime_error("PostgreSQL result column count does not match selected tuple");
                values.push_back(std::move(value));
            }
        } catch (...) {
            active_result_ = nullptr;
            throw;
        }
        active_result_ = nullptr;
        return values;
    }

    pg_ormlite_detail::connection_handle connection_;
    std::string table_name_;
    std::string select_sql_;
    std::string delete_sql_;
    std::string update_sql_;
    std::optional<expr> set_expression_;
    std::optional<expr> where_expression_;
    std::optional<expr> group_by_expression_;
    std::optional<expr> having_expression_;
    std::optional<expr> order_by_expression_;
    bool order_descending_ = false;
    std::optional<std::size_t> limit_;
    std::optional<std::size_t> offset_;
    PGresult* active_result_ = nullptr;
};

template<typename T>
struct field_attribute;

template<typename T, typename U>
struct field_attribute<U T::*> {
    using type = T;
    using return_type = U;
};

template<typename U>
constexpr std::string_view get_field_name(std::string_view full_name)
{
    const auto separator = full_name.rfind("::");
    return separator == std::string_view::npos
               ? full_name
               : full_name.substr(separator + 2);
}

template<typename U>
constexpr std::string_view get_table_name(std::string_view)
{
    using T = typename field_attribute<U>::type;
    return orm::model::get_name<T>();
}

} // namespace pg_query_object

#define ORM_FIELD(field)                                                          \
    pg_query_object::expr(                                                        \
        pg_query_object::get_field_name<decltype(&field)>(std::string_view(#field)), \
        pg_query_object::get_table_name<decltype(&field)>(std::string_view(#field)))

#define ORM_AGG(field, op, type)                                                  \
    pg_query_object::selectable<type>(                                            \
        pg_query_object::get_field_name<decltype(&field)>(std::string_view(#field)), \
        pg_query_object::get_table_name<decltype(&field)>(std::string_view(#field)), \
        op)

#define ORM_TYPE(field) ORM_AGG(field, "", pg_query_object::field_attribute<decltype(&field)>::return_type)
#define ORM_COUNT(field) ORM_AGG(field, "count", std::size_t)
#define ORM_SUM(field) ORM_AGG(field, "sum", pg_query_object::field_attribute<decltype(&field)>::return_type)
#define ORM_AVG(field) ORM_AGG(field, "avg", pg_query_object::field_attribute<decltype(&field)>::return_type)
#define ORM_MAX(field) ORM_AGG(field, "max", pg_query_object::field_attribute<decltype(&field)>::return_type)
#define ORM_MIN(field) ORM_AGG(field, "min", pg_query_object::field_attribute<decltype(&field)>::return_type)

#endif
