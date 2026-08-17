#ifndef PG_ORMLITE_HPP
#define PG_ORMLITE_HPP

#include <array>
#include <cstdint>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "pg_query_object.hpp"
#include "model_detail.hpp"
#include "traits_utils.hpp"

namespace pg_ormlite {

struct key_map {
    std::string fields;
};

struct auto_key_map {
    std::string fields;
};

struct not_null_map {
    std::set<std::string> fields;
};

template<typename... Args>
auto sort_tuple(const std::tuple<Args...>& tuple)
{
    if constexpr (sizeof...(Args) == 2) {
        auto [first, second] = tuple;
        if constexpr (!std::is_same_v<decltype(first), key_map> &&
                      !std::is_same_v<decltype(first), auto_key_map>)
            return std::make_tuple(second, first);
    }
    return tuple;
}

class pg_connection {
public:
    template<typename... Args>
    explicit pg_connection(Args&&... args)
    {
        static_assert(sizeof...(Args) == 5 || sizeof...(Args) == 6,
                      "pg_connection requires host, port, user, password, dbname, and optional connect_timeout");
        connect(std::array<std::string, sizeof...(Args)>{
            connection_value(std::forward<Args>(args))...
        });
    }

    pg_connection(const pg_connection&) = default;
    pg_connection& operator=(const pg_connection&) = default;
    pg_connection(pg_connection&&) noexcept = default;
    pg_connection& operator=(pg_connection&&) noexcept = default;
    ~pg_connection() = default;

    template<typename T>
    bool prepare(const std::string& sql)
    {
        using modeled_type = std::decay_t<T>;
        static_assert(orm::model::is_entity<modeled_type>::value,
                      "pg_connection::prepare requires an entity model");
        auto result = pg_ormlite_detail::adopt_result(
            PQprepare(conn_.get(),
                      "",
                      sql.c_str(),
                      static_cast<int>(orm::model::get_value<modeled_type>()),
                      nullptr));
        return result != nullptr && PQresultStatus(result.get()) == PGRES_COMMAND_OK;
    }

    template<typename T, typename... Args>
    bool create_table(Args&&... args)
    {
        const std::string sql = generate_create_table_sql<T>(std::forward<Args>(args)...);
        auto result = pg_ormlite_detail::adopt_result(PQexec(conn_.get(), sql.c_str()));
        return result != nullptr && PQresultStatus(result.get()) == PGRES_COMMAND_OK;
    }

    template<typename T>
    std::string generate_insert_sql(bool replace)
    {
        using modeled_type = std::decay_t<T>;
        static_assert(orm::model::is_entity<modeled_type>::value,
                      "pg_connection::generate_insert_sql requires an entity model");
        if (replace)
            throw std::invalid_argument("PostgreSQL does not support REPLACE INTO");

        std::string sql = "insert into ";
        sql += std::string(orm::model::get_name<modeled_type>());
        sql += "(" + std::string(orm::model::get_field<modeled_type>()) + ") values(";
        constexpr std::size_t field_count = orm::model::get_value<modeled_type>();
        for (std::size_t index = 0; index < field_count; ++index) {
            sql += "$" + std::to_string(index + 1);
            if (index + 1 != field_count)
                sql += ", ";
        }
        sql += ");";
        return sql;
    }

    template<typename T>
    void set_param_values(std::vector<std::string>& parameter_values, T&& value)
    {
        parameter_values.push_back(
            pg_query_object::detail::parameter_value(std::forward<T>(value)));
    }

    template<typename T>
    void set_param_values(std::vector<std::vector<char>>& parameter_values, T&& value)
    {
        const std::string encoded =
            pg_query_object::detail::parameter_value(std::forward<T>(value));
        std::vector<char> buffer(encoded.begin(), encoded.end());
        buffer.push_back('\0');
        parameter_values.push_back(std::move(buffer));
    }

    template<typename T>
    bool insert_impl(const std::string&, T&& value)
    {
        std::vector<std::string> parameters;
        using modeled_type = std::decay_t<T>;
        parameters.reserve(orm::model::get_value<modeled_type>());
        orm::model::for_each(value, [&](auto member, auto, auto) {
            set_param_values(parameters, value.*member);
        });

        if (parameters.size() != orm::model::get_value<modeled_type>())
            throw std::logic_error("encoded parameter count does not match modeled field count");
        const auto pointers = pg_ormlite_detail::parameter_pointers(parameters);
        auto result = pg_ormlite_detail::adopt_result(
            PQexecPrepared(conn_.get(),
                           "",
                           static_cast<int>(pointers.size()),
                           pointers.empty() ? nullptr : pointers.data(),
                           nullptr,
                           nullptr,
                           0));
        return result != nullptr && PQresultStatus(result.get()) == PGRES_COMMAND_OK;
    }

    template<typename T>
    int insert(T&& value)
    {
        using modeled_type = std::decay_t<T>;
        static_assert(orm::model::is_entity<modeled_type>::value,
                      "pg_connection::insert requires an entity model");
        const std::string sql = generate_insert_sql<modeled_type>(false);
        if (!prepare<modeled_type>(sql))
            return 0;
        return insert_impl(sql, std::forward<T>(value)) ? 1 : 0;
    }

    template<typename T>
    int insert(const std::vector<T>& values)
    {
        static_assert(orm::model::is_entity<T>::value,
                      "pg_connection::insert requires a vector of modeled values");
        if (values.empty())
            return 0;

        const std::string sql = generate_insert_sql<T>(false);
        if (!prepare<T>(sql) || !execute("begin;"))
            return 0;

        try {
            for (const auto& value : values) {
                if (!insert_impl(sql, value)) {
                    execute("rollback;");
                    return 0;
                }
            }
            if (!execute("commit;")) {
                execute("rollback;");
                return 0;
            }
        } catch (...) {
            execute("rollback;");
            throw;
        }
        return static_cast<int>(values.size());
    }

    template<typename T>
    int insert(std::vector<T>& values)
    {
        return insert(static_cast<const std::vector<T>&>(values));
    }

    template<typename T>
    auto get_type_names()
    {
        using modeled_type = std::decay_t<T>;
        constexpr std::size_t field_count = orm::model::get_value<modeled_type>();
        std::array<std::string, field_count> field_types{};
        modeled_type value{};
        orm::model::for_each(value, [&](auto member, auto, auto index) {
            constexpr std::size_t position = decltype(index)::value;
            using member_type = typename pg_query_object::field_attribute<
                std::decay_t<decltype(member)>>::return_type;
            using field_type = std::remove_cv_t<member_type>;

            if constexpr (std::is_same_v<field_type, bool> ||
                          std::is_same_v<field_type, int> ||
                          std::is_same_v<field_type, std::int32_t> ||
                          std::is_same_v<field_type, std::uint32_t> ||
                          std::is_enum_v<field_type>)
                field_types[position] = "integer";
            else if constexpr (std::is_same_v<field_type, std::int8_t> ||
                               std::is_same_v<field_type, std::uint8_t> ||
                               std::is_same_v<field_type, std::int16_t> ||
                               std::is_same_v<field_type, std::uint16_t>)
                field_types[position] = "smallint";
            else if constexpr (std::is_same_v<field_type, std::int64_t> ||
                               std::is_same_v<field_type, std::uint64_t>)
                field_types[position] = "bigint";
            else if constexpr (std::is_same_v<field_type, float>)
                field_types[position] = "real";
            else if constexpr (std::is_same_v<field_type, double>)
                field_types[position] = "double precision";
            else if constexpr (std::is_same_v<field_type, std::string>)
                field_types[position] = "text";
            else if constexpr (std::is_array_v<field_type> &&
                               std::is_same_v<std::remove_extent_t<field_type>, char>)
                field_types[position] =
                    "varchar(" + std::to_string(std::extent_v<field_type>) + ")";
            else
                static_assert(pg_query_object::detail::always_false<field_type>::value,
                              "unsupported modeled PostgreSQL field type");
        });
        return field_types;
    }

    template<typename T, typename... Args>
    std::string generate_create_table_sql(Args&&... args)
    {
        using modeled_type = std::decay_t<T>;
        static_assert(orm::model::is_entity<modeled_type>::value,
                      "generate_create_table_sql requires an entity model");
        using option_types = std::tuple<std::decay_t<Args>...>;
        static_assert(!(traits_utils::has_type<key_map, option_types>::value &&
                        traits_utils::has_type<auto_key_map, option_types>::value),
                      "key_map and auto_key_map cannot be used together");

        const auto field_names = orm::model::get_array<modeled_type>();
        const auto field_types = get_type_names<modeled_type>();
        const auto options = sort_tuple(std::make_tuple(std::forward<Args>(args)...));
        validate_options(field_names, options);

        std::string sql = "create table if not exists ";
        sql += std::string(orm::model::get_name<modeled_type>()) + "(";
        constexpr std::size_t field_count = orm::model::get_value<modeled_type>();
        for (std::size_t index = 0; index < field_count; ++index) {
            const std::string field_name(field_names[index]);
            const std::string& field_type = field_types[index];
            bool added = false;

            orm::model::for_each(options, [&](const auto& option, auto) {
                if (added)
                    return;
                using option_type = std::decay_t<decltype(option)>;
                if constexpr (std::is_same_v<option_type, not_null_map>) {
                    if (option.fields.find(field_name) != option.fields.end()) {
                        sql += field_name + " " + field_type + " not null";
                        added = true;
                    }
                } else if constexpr (std::is_same_v<option_type, key_map>) {
                    if (option.fields == field_name) {
                        sql += field_name + " " + field_type + " primary key";
                        added = true;
                    }
                } else if constexpr (std::is_same_v<option_type, auto_key_map>) {
                    if (option.fields == field_name) {
                        sql += field_name + " " + serial_type(field_type) + " primary key";
                        added = true;
                    }
                } else {
                    static_assert(pg_query_object::detail::always_false<option_type>::value,
                                  "unsupported create_table option type");
                }
            });

            if (!added)
                sql += field_name + " " + field_type;
            if (index + 1 != field_count)
                sql += ", ";
        }
        sql += ");";
        return sql;
    }

    template<typename T>
    std::enable_if_t<orm::model::is_entity<T>::value,
                     pg_query_object::query_object<T>>
    query()
    {
        return pg_query_object::query_object<T>(conn_, orm::model::get_name<T>());
    }

    template<typename T>
    std::enable_if_t<orm::model::is_entity<T>::value,
                     pg_query_object::query_object<T>>
    del()
    {
        return pg_query_object::query_object<T>(conn_, orm::model::get_name<T>(), "delete", "");
    }

    template<typename T>
    std::enable_if_t<orm::model::is_entity<T>::value,
                     pg_query_object::query_object<T>>
    update()
    {
        return pg_query_object::query_object<T>(conn_, orm::model::get_name<T>(), "", "update");
    }

    bool execute(const std::string& sql)
    {
        auto result = pg_ormlite_detail::adopt_result(PQexec(conn_.get(), sql.c_str()));
        return result != nullptr && PQresultStatus(result.get()) == PGRES_COMMAND_OK;
    }

    std::string last_error() const
    {
        return pg_ormlite_detail::connection_error(conn_.get());
    }

private:
    template<typename T>
    static std::string connection_value(T&& value)
    {
        using value_type = std::decay_t<T>;
        if constexpr (std::is_same_v<value_type, std::string>)
            return std::forward<T>(value);
        else if constexpr (std::is_same_v<value_type, std::string_view>)
            return std::string(value);
        else if constexpr (std::is_same_v<value_type, const char*> ||
                           std::is_same_v<value_type, char*>) {
            if (value == nullptr)
                throw std::invalid_argument("PostgreSQL connection parameter is null");
            return std::string(value);
        } else {
            std::ostringstream stream;
            stream << value;
            if (!stream)
                throw std::invalid_argument("failed to encode PostgreSQL connection parameter");
            return stream.str();
        }
    }

    template<std::size_t Count>
    void connect(const std::array<std::string, Count>& values)
    {
        static_assert(Count == 5 || Count == 6, "invalid PostgreSQL connection parameter count");
        std::array<const char*, 7> keywords{
            "host", "port", "user", "password", "dbname", "connect_timeout", nullptr
        };
        std::array<const char*, 7> parameter_values{};
        for (std::size_t index = 0; index < Count; ++index)
            parameter_values[index] = values[index].c_str();
        keywords[Count] = nullptr;
        parameter_values[Count] = nullptr;

        PGconn* raw_connection =
            PQconnectdbParams(keywords.data(), parameter_values.data(), 0);
        if (raw_connection == nullptr)
            throw std::runtime_error("PostgreSQL connection allocation failed");
        conn_ = pg_ormlite_detail::adopt_connection(raw_connection);
        if (PQstatus(conn_.get()) != CONNECTION_OK)
            throw std::runtime_error(pg_ormlite_detail::connection_error(conn_.get()));
    }

    static std::string serial_type(const std::string& field_type)
    {
        if (field_type == "smallint")
            return "smallserial";
        if (field_type == "integer")
            return "serial";
        if (field_type == "bigint")
            return "bigserial";
        throw std::invalid_argument("auto_key_map requires a smallint, integer, or bigint field");
    }

    template<std::size_t FieldCount, typename Options>
    static void validate_options(const std::array<std::string_view, FieldCount>& fields,
                                 const Options& options)
    {
        const auto contains = [&](const std::string& name) {
            return std::find(fields.begin(), fields.end(), name) != fields.end();
        };
        orm::model::for_each(options, [&](const auto& option, auto) {
            using option_type = std::decay_t<decltype(option)>;
            if constexpr (std::is_same_v<option_type, not_null_map>) {
                for (const auto& field : option.fields) {
                    if (!contains(field))
                        throw std::invalid_argument("not_null_map names an unknown modeled field: " + field);
                }
            } else {
                if (!contains(option.fields))
                    throw std::invalid_argument("key map names an unknown modeled field: " + option.fields);
            }
        });
    }

    pg_ormlite_detail::connection_handle conn_;
};

} // namespace pg_ormlite

#endif
