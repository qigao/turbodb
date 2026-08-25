#ifndef PG_DETAIL_HPP
#define PG_DETAIL_HPP

#include <libpq-fe.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace pg_ormlite_detail {

struct result_deleter {
    void operator()(PGresult* result) const noexcept
    {
        if (result != nullptr)
            PQclear(result);
    }
};

using result_handle = std::unique_ptr<PGresult, result_deleter>;

inline result_handle adopt_result(PGresult* result)
{
    return result_handle(result);
}

struct connection_deleter {
    void operator()(PGconn* connection) const noexcept
    {
        if (connection != nullptr)
            PQfinish(connection);
    }
};

using connection_handle = std::shared_ptr<PGconn>;

inline connection_handle adopt_connection(PGconn* connection)
{
    return connection_handle(connection, connection_deleter{});
}

inline connection_handle borrow_connection(PGconn* connection)
{
    return connection_handle(connection, [](PGconn*) noexcept {});
}

inline std::string connection_error(PGconn* connection)
{
    if (connection == nullptr)
        return "libpq returned a null connection";
    const char* message = PQerrorMessage(connection);
    return message != nullptr ? std::string(message) : std::string("unknown libpq error");
}

inline std::string result_error(PGconn* connection, PGresult* result)
{
    if (result != nullptr) {
        const char* message = PQresultErrorMessage(result);
        if (message != nullptr && *message != '\0')
            return std::string(message);
    }
    return connection_error(connection);
}

inline std::string result_sqlstate(PGresult* result)
{
    if (result == nullptr)
        return {};
    const char* code = PQresultErrorField(result, PG_DIAG_SQLSTATE);
    return code != nullptr ? std::string(code) : std::string{};
}

inline std::vector<const char*> parameter_pointers(const std::vector<std::string>& parameters)
{
    std::vector<const char*> pointers;
    pointers.reserve(parameters.size());
    for (const auto& parameter : parameters)
        pointers.push_back(parameter.c_str());
    return pointers;
}

inline result_handle execute_params(PGconn* connection,
                                    const std::string& sql,
                                    const std::vector<std::string>& parameters)
{
    if (connection == nullptr)
        throw std::logic_error("cannot execute SQL without a PostgreSQL connection");

    const auto pointers = parameter_pointers(parameters);
    return adopt_result(PQexecParams(connection,
                                     sql.c_str(),
                                     static_cast<int>(pointers.size()),
                                     nullptr,
                                     pointers.empty() ? nullptr : pointers.data(),
                                     nullptr,
                                     nullptr,
                                     0));
}

} // namespace pg_ormlite_detail

#endif
