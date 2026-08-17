#include "pg_ormlite.hpp"

#include "pg_fake_core.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fake_libpq {

using pg_fake::cleared_results;
using pg_fake::commands;
using pg_fake::created_results;
using pg_fake::fail_prepared_call;
using pg_fake::finished_connections;
using pg_fake::next_rows;
using pg_fake::parameter_calls;
using pg_fake::parameterized_sql;
using pg_fake::pg_calls;
using pg_fake::prepared_calls;

inline PGresult* result(ExecStatusType status)
{
    auto* value = pg_fake::make_result(status);
    ++pg_fake::created_results;
    return value;
}

inline std::vector<std::string> copy_parameters(int count, const char* const* values)
{
    return pg_fake::copy_parameters(count, values);
}

inline void reset_calls()
{
    pg_fake::reset_script_state();
}

} // namespace fake_libpq

extern "C" {

PGconn* PQconnectdbParams(const char* const*, const char* const*, int)
{
    return new pg_conn;
}

void PQfinish(PGconn* connection)
{
    ++fake_libpq::finished_connections;
    delete connection;
}

ConnStatusType PQstatus(const PGconn* connection)
{
    return connection->status;
}

char* PQerrorMessage(const PGconn* connection)
{
    return const_cast<char*>(connection->error.c_str());
}

PGresult* PQexec(PGconn*, const char* sql)
{
    fake_libpq::commands.emplace_back(sql);
    return fake_libpq::result(PGRES_COMMAND_OK);
}

PGresult* PQexecParams(PGconn*,
                       const char* sql,
                       int parameter_count,
                       const Oid* parameter_types,
                       const char* const* parameter_values,
                       const int* parameter_lengths,
                       const int* parameter_formats,
                       int result_format)
{
    fake_libpq::parameterized_sql.emplace_back(sql);
    fake_libpq::parameter_calls.push_back(
        fake_libpq::copy_parameters(parameter_count, parameter_values));
    fake_libpq::pg_calls.push_back(
        recorded_pg_call{sql != nullptr ? sql : "",
                         pg_fake::copy_optional_parameters(parameter_count,
                                                           parameter_values),
                         pg_fake::copy_oids(parameter_count, parameter_types),
                         pg_fake::copy_ints(parameter_count, parameter_lengths),
                         pg_fake::copy_ints(parameter_count, parameter_formats),
                         result_format,
                         false});
    ExecStatusType status =
        std::strncmp(sql, "select", 6) == 0 ? PGRES_TUPLES_OK : PGRES_COMMAND_OK;
    if (pg_fake::next_call.has_status)
        status = pg_fake::next_call.status;

    auto* result = static_cast<pg_result*>(fake_libpq::result(status));
    if (pg_fake::next_call.has_error)
        result->error = pg_fake::next_call.error;
    if (pg_fake::next_call.has_affected_rows)
        result->affected_rows = pg_fake::next_call.affected_rows;

    if (pg_fake::next_call.has_dimensions) {
        result->scripted_dimensions = true;
        result->scripted_rows = pg_fake::next_call.rows;
        result->scripted_columns = pg_fake::next_call.columns;
    } else {
        result->rows = fake_libpq::next_rows;
        result->columns = result->rows.empty()
                              ? 0
                              : static_cast<int>(result->rows.front().size());
    }

    fake_libpq::next_rows.clear();
    pg_fake::next_call = scripted_pg_call{};
    return result;
}

PGresult* PQprepare(PGconn*, const char*, const char* sql, int, const Oid*)
{
    fake_libpq::commands.emplace_back(std::string("prepare:") + sql);
    return fake_libpq::result(PGRES_COMMAND_OK);
}

PGresult* PQexecPrepared(PGconn*,
                         const char* statement_name,
                         int parameter_count,
                         const char* const* parameter_values,
                         const int* parameter_lengths,
                         const int* parameter_formats,
                         int result_format)
{
    ++fake_libpq::prepared_calls;
    fake_libpq::parameter_calls.push_back(
        fake_libpq::copy_parameters(parameter_count, parameter_values));
    fake_libpq::pg_calls.push_back(
        recorded_pg_call{statement_name != nullptr ? statement_name : "",
                         pg_fake::copy_optional_parameters(parameter_count,
                                                           parameter_values),
                         {},
                         pg_fake::copy_ints(parameter_count, parameter_lengths),
                         pg_fake::copy_ints(parameter_count, parameter_formats),
                         result_format,
                         true});
    const bool fail = fake_libpq::prepared_calls == fake_libpq::fail_prepared_call;
    if (fail)
        return fake_libpq::result(PGRES_FATAL_ERROR);

    ExecStatusType status = PGRES_COMMAND_OK;
    if (pg_fake::next_call.has_status)
        status = pg_fake::next_call.status;
    auto* result = static_cast<pg_result*>(fake_libpq::result(status));
    if (pg_fake::next_call.has_error)
        result->error = pg_fake::next_call.error;
    if (pg_fake::next_call.has_affected_rows)
        result->affected_rows = pg_fake::next_call.affected_rows;
    if (pg_fake::next_call.has_dimensions) {
        result->scripted_dimensions = true;
        result->scripted_rows = pg_fake::next_call.rows;
        result->scripted_columns = pg_fake::next_call.columns;
    }
    pg_fake::next_call = scripted_pg_call{};
    return result;
}

ExecStatusType PQresultStatus(const PGresult* result)
{
    return result->status;
}

char* PQresultErrorMessage(const PGresult* result)
{
    return const_cast<char*>(result->error.c_str());
}

int PQntuples(const PGresult* result)
{
    return result->scripted_dimensions
        ? result->scripted_rows
        : static_cast<int>(result->rows.size());
}

int PQnfields(const PGresult* result)
{
    return result->scripted_dimensions
        ? result->scripted_columns
        : result->columns;
}

char* PQgetvalue(const PGresult* result, int row, int column)
{
    return pg_fake::result_value(result, row, column);
}

int PQgetlength(const PGresult* result, int row, int column)
{
    return pg_fake::result_length(result, row, column);
}

int PQgetisnull(const PGresult* result, int row, int column)
{
    return pg_fake::result_is_null(result, row, column);
}

void PQclear(PGresult* result)
{
    ++fake_libpq::cleared_results;
    delete result;
}

} // extern "C"

struct person {
    int id;
    char name[10];
};
ORM_MODEL(person, id, name)

struct measurement {
    int id;
    double reading;
};
ORM_MODEL(measurement, id, reading)

struct account {
    int id;
};
ORM_MODEL_WITH_NAME(account, "accounts", id)

void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

template<typename Exception, typename Function>
void require_throws(Function&& function, const char* message)
{
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void test_parameterized_expression()
{
    pg_query_object::query_object<person> query(nullptr, "person");
    const auto& built = query.where(
        ((ORM_FIELD(person::id) > 1 || ORM_FIELD(person::id) < 0) &&
         ORM_FIELD(person::name) == "x' OR TRUE --"));
    const std::string sql = built.to_string();
    const auto parameters = built.parameters();

    require(sql ==
                "select * from person where (((id > $1) or (id < $2)) and (name = $3));",
            "logical expression grouping or placeholders are incorrect");
    require(sql.find("OR TRUE") == std::string::npos,
            "string value leaked into generated SQL");
    require(parameters == std::vector<std::string>({"1", "0", "x' OR TRUE --"}),
            "bound expression parameters are incorrect");
    require(ORM_FIELD(account::id).to_string() == "id" &&
                ORM_FIELD(account::id).table_name() == "accounts",
            "custom table name corrupted modeled field extraction");
    require(ORM_TYPE(account::id).to_string() == "(id)" &&
                ORM_TYPE(account::id).table_name() == "accounts",
            "ORM_TYPE corrupted modeled selected-field extraction");
}

void test_connection_and_schema()
{
    const int finishes_before = fake_libpq::finished_connections;
    {
        pg_ormlite::pg_connection connection("host", "5432", "user", "secret", "db");
        {
            auto shared_connection = connection;
            require(fake_libpq::finished_connections == finishes_before,
                    "copying a connection released the shared handle early");
        }
        require(fake_libpq::finished_connections == finishes_before,
                "destroying a connection copy released the live handle");

        pg_ormlite::key_map primary_key{"id"};
        pg_ormlite::not_null_map not_null{{"name"}};
        require(connection.generate_create_table_sql<person>(primary_key, not_null) ==
                    "create table if not exists person(id integer primary key, name varchar(10) not null);",
                "generated schema SQL is incorrect");
        require(connection.generate_create_table_sql<person>(pg_ormlite::auto_key_map{"id"}) ==
                    "create table if not exists person(id serial primary key, name varchar(10));",
                "auto key did not use PostgreSQL serial syntax");
        require_throws<std::invalid_argument>(
            [&] {
                connection.generate_create_table_sql<person>(pg_ormlite::key_map{"missing"});
            },
            "unknown schema fields must fail fast");
    }
    require(fake_libpq::finished_connections == finishes_before + 1,
            "shared PostgreSQL connection was not released exactly once");
}

void test_safe_parameter_encoding_and_transactions()
{
    pg_ormlite::pg_connection connection("host", "5432", "user", "secret", "db");

    fake_libpq::reset_calls();
    measurement sample{1, std::numeric_limits<double>::max()};
    require(connection.insert(sample) == 1, "large floating-point insert failed");
    require(fake_libpq::parameter_calls.back().at(1).size() > 20,
            "large floating-point parameter was unexpectedly truncated");

    person unterminated{};
    unterminated.id = 1;
    std::fill(std::begin(unterminated.name), std::end(unterminated.name), 'x');
    require_throws<std::invalid_argument>(
        [&] { connection.insert(unterminated); },
        "unterminated character array parameter must fail fast");
    require_throws<std::invalid_argument>(
        [&] {
            auto query = connection.query<person>();
            query.where(ORM_FIELD(person::name) == std::string("a\0b", 3));
        },
        "embedded null bytes in text parameters must fail fast");

    std::vector<person> values{{1, "one"}, {2, "two"}, {3, "three"}};
    fake_libpq::reset_calls();
    fake_libpq::fail_prepared_call = 2;
    require(connection.insert(values) == 0, "failed batch insert reported success");
    require(std::find(fake_libpq::commands.begin(), fake_libpq::commands.end(), "begin;") !=
                fake_libpq::commands.end(),
            "batch insert did not start a transaction");
    require(std::find(fake_libpq::commands.begin(), fake_libpq::commands.end(), "rollback;") !=
                fake_libpq::commands.end(),
            "failed batch insert did not roll back");
    require(std::find(fake_libpq::commands.begin(), fake_libpq::commands.end(), "commit;") ==
                fake_libpq::commands.end(),
            "failed batch insert attempted to commit");

    fake_libpq::reset_calls();
    require(connection.insert(values) == static_cast<int>(values.size()),
            "successful batch insert returned the wrong count");
    require(std::find(fake_libpq::commands.begin(), fake_libpq::commands.end(), "commit;") !=
                fake_libpq::commands.end(),
            "successful batch insert did not commit");
}

void test_result_decoding()
{
    pg_ormlite::pg_connection connection("host", "5432", "user", "secret", "db");

    fake_libpq::reset_calls();
    fake_libpq::next_rows = {{{std::string("7")}, {std::string("Alice")}}};
    const auto rows = connection.query<person>()
                          .where(ORM_FIELD(person::id) == 7)
                          .to_vector();
    require(rows.size() == 1 && rows.front().id == 7 &&
                std::string(rows.front().name) == "Alice",
            "modeled query result was decoded incorrectly");
    require(fake_libpq::parameterized_sql.back().find("$1") != std::string::npos,
            "query did not execute with a PostgreSQL placeholder");
    require(fake_libpq::parameter_calls.back() == std::vector<std::string>({"7"}),
            "query parameter was not bound separately");
    const recorded_pg_call& call = fake_libpq::pg_calls.back();
    require(call.sql == "select * from person where (id = $1);",
            "recorded libpq call SQL is incorrect");
    require(call.parameter_values ==
                std::vector<std::optional<std::string>>({std::string("7")}),
            "recorded libpq bindings are incorrect");
    require(!call.from_prepared,
            "direct query was recorded as a prepared statement");

    fake_libpq::next_rows = {{{std::string("8")}, {std::string("1234567890")}}};
    require_throws<std::length_error>(
        [&] { connection.query<person>().to_vector(); },
        "oversized text result must not be copied into a fixed character array");

    fake_libpq::next_rows = {{{std::string("9")}, {std::nullopt}}};
    require_throws<std::runtime_error>(
        [&] { connection.query<person>().to_vector(); },
        "SQL NULL must not be silently converted to an empty character array");
}

int main()
{
    try {
        static_assert(orm::model::get_value<person>() == 2,
                      "C++17 entity model metadata is incorrect");
        test_parameterized_expression();
        test_connection_and_schema();
        test_safe_parameter_encoding_and_transactions();
        test_result_decoding();
        require(fake_libpq::created_results == fake_libpq::cleared_results,
                "a PGresult handle leaked during the tests");
        std::cout << "shared/orm C++17 tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "shared/orm test failure: " << error.what() << '\n';
        return 1;
    }
}
