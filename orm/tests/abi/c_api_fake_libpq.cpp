#include "c_api_test_support.h"

#include "pg_fake_core.hpp"

#include <cstdlib>
#include <limits>
#include <optional>

namespace {

PGresult* pending_full_result = nullptr;
std::vector<PGresult*> pending_results;
std::size_t pending_result_index = 0;
int send_query_count = 0;
int single_row_count = 0;

void clear_pending_results()
{
    delete pending_full_result;
    pending_full_result = nullptr;
    for (std::size_t index = pending_result_index;
         index < pending_results.size(); ++index)
        delete pending_results[index];
    pending_results.clear();
    pending_result_index = 0;
}

} // namespace

extern "C" {

void fake_pg_reset(void)
{
    clear_pending_results();
    pg_fake::reset_script_state();
    pg_fake::finished_connections = 0;
    pg_fake::cleared_results = 0;
    send_query_count = 0;
    single_row_count = 0;
}

void fake_pg_set_result(size_t rows,
                        size_t columns,
                        const char* const* values,
                        const uint8_t* null_flags)
{
    if ((rows != 0 && columns != 0) && (values == nullptr || null_flags == nullptr))
        std::abort();
    if (columns > static_cast<size_t>(std::numeric_limits<int>::max()))
        std::abort();

    pg_fake::next_rows.assign(rows, {});
    pg_fake::next_columns = static_cast<int>(columns);
    for (size_t row = 0; row < rows; ++row) {
        pg_fake::next_rows[row].reserve(columns);
        for (size_t column = 0; column < columns; ++column) {
            const size_t index = row * columns + column;
            if (null_flags[index] != 0) {
                pg_fake::next_rows[row].push_back(std::nullopt);
            } else {
                if (values[index] == nullptr)
                    std::abort();
                pg_fake::next_rows[row].emplace_back(values[index]);
            }
        }
    }
}

void fake_pg_set_result_types(size_t columns, const uint32_t* types)
{
    pg_fake::next_column_types.assign(columns, 25);
    for (size_t index = 0; index < columns; ++index)
        pg_fake::next_column_types[index] = static_cast<Oid>(types[index]);
}

void fake_pg_fail_next_query(void)
{
    pg_fake::fail_next_query = true;
}

const char* fake_pg_last_sql(void)
{
    return pg_fake::last_sql.c_str();
}

size_t fake_pg_parameter_count(void)
{
    return pg_fake::last_parameters.size();
}

const char* fake_pg_parameter_at(size_t index)
{
    return index < pg_fake::last_parameters.size() && pg_fake::last_parameters[index]
        ? pg_fake::last_parameters[index]->c_str()
        : nullptr;
}

int fake_pg_finished_connections(void)
{
    return pg_fake::finished_connections;
}

int fake_pg_cleared_results(void)
{
    return pg_fake::cleared_results;
}

int fake_pg_send_query_count(void)
{
    return send_query_count;
}

int fake_pg_single_row_count(void)
{
    return single_row_count;
}

PGconn* PQconnectdbParams(const char* const*, const char* const*, int)
{
    return new pg_conn;
}

void PQfinish(PGconn* connection)
{
    ++pg_fake::finished_connections;
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

PGresult* PQexecParams(PGconn*,
                       const char* sql,
                       int parameter_count,
                       const Oid* parameter_types,
                       const char* const* parameter_values,
                       const int* parameter_lengths,
                       const int* parameter_formats,
                       int result_format)
{
    pg_fake::last_sql = sql != nullptr ? sql : "";
    pg_fake::last_parameters =
        pg_fake::copy_optional_parameters(parameter_count, parameter_values);
    pg_fake::pg_calls.push_back(
        recorded_pg_call{pg_fake::last_sql,
                         pg_fake::last_parameters,
                         pg_fake::copy_oids(parameter_count, parameter_types),
                         pg_fake::copy_ints(parameter_count, parameter_lengths),
                         pg_fake::copy_ints(parameter_count, parameter_formats),
                         result_format,
                         false});

    if (pg_fake::fail_next_query) {
        pg_fake::fail_next_query = false;
        pg_fake::next_call = scripted_pg_call{};
        auto* result = pg_fake::make_result(PGRES_FATAL_ERROR);
        result->error = "forced SQL failure";
        return result;
    }

    if (pg_fake::next_call.return_null_result) {
        pg_fake::next_call = scripted_pg_call{};
        return nullptr;
    }

    const bool command = pg_fake::last_sql.rfind("insert ", 0) == 0 ||
                         pg_fake::last_sql.rfind("update ", 0) == 0 ||
                         pg_fake::last_sql.rfind("delete ", 0) == 0;
    ExecStatusType status =
        command ? PGRES_COMMAND_OK : PGRES_TUPLES_OK;
    if (pg_fake::next_call.has_status)
        status = pg_fake::next_call.status;

    auto* result = pg_fake::make_result(status);
    if (pg_fake::next_call.has_error)
        result->error = pg_fake::next_call.error;
    if (pg_fake::next_call.has_affected_rows)
        result->affected_rows = pg_fake::next_call.affected_rows;
    else if (command)
        result->affected_rows = "1";

    if (pg_fake::next_call.has_dimensions) {
        result->scripted_dimensions = true;
        result->scripted_rows = pg_fake::next_call.rows;
        result->scripted_columns = pg_fake::next_call.columns;
    } else {
        result->columns = pg_fake::next_columns;
        result->rows = std::move(pg_fake::next_rows);
        result->column_types = std::move(pg_fake::next_column_types);
    }

    pg_fake::next_rows.clear();
    pg_fake::next_column_types.clear();
    pg_fake::next_columns = 0;
    pg_fake::next_call = scripted_pg_call{};
    return result;
}

int PQsendQueryParams(PGconn* connection,
                      const char* sql,
                      int parameter_count,
                      const Oid* parameter_types,
                      const char* const* parameter_values,
                      const int* parameter_lengths,
                      const int* parameter_formats,
                      int result_format)
{
    ++send_query_count;
    if (pending_full_result != nullptr ||
        pending_result_index < pending_results.size())
        return 0;
    pending_full_result = PQexecParams(
        connection, sql, parameter_count, parameter_types, parameter_values,
        parameter_lengths, parameter_formats, result_format);
    return pending_full_result != nullptr ? 1 : 0;
}

int PQsetSingleRowMode(PGconn*)
{
    ++single_row_count;
    if (pending_full_result == nullptr)
        return 0;
    if (pending_full_result->status == PGRES_TUPLES_OK) {
        for (const auto& row : pending_full_result->rows) {
            auto* single = pg_fake::make_result(PGRES_SINGLE_TUPLE);
            single->columns = pending_full_result->columns;
            single->column_types = pending_full_result->column_types;
            single->rows.push_back(row);
            pending_results.push_back(single);
        }
        auto* terminal = pg_fake::make_result(PGRES_TUPLES_OK);
        terminal->columns = pending_full_result->columns;
        terminal->column_types = pending_full_result->column_types;
        terminal->scripted_dimensions =
            pending_full_result->scripted_dimensions;
        terminal->scripted_rows = pending_full_result->scripted_rows;
        terminal->scripted_columns = pending_full_result->scripted_columns;
        pending_results.push_back(terminal);
        delete pending_full_result;
    } else {
        pending_results.push_back(pending_full_result);
    }
    pending_full_result = nullptr;
    pending_result_index = 0;
    return 1;
}

PGresult* PQgetResult(PGconn*)
{
    PGresult* result;
    if (pending_result_index == pending_results.size()) {
        pending_results.clear();
        pending_result_index = 0;
        return nullptr;
    }
    result = pending_results[pending_result_index];
    pending_results[pending_result_index] = nullptr;
    ++pending_result_index;
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

char* PQfname(const PGresult*, int)
{
    static char name[] = "column";
    return name;
}

char* PQcmdTuples(PGresult* result)
{
    return const_cast<char*>(result->affected_rows.c_str());
}

char* PQgetvalue(const PGresult* result, int row, int column)
{
    return pg_fake::result_value(result, row, column);
}

int PQgetlength(const PGresult* result, int row, int column)
{
    return pg_fake::result_length(result, row, column);
}

Oid PQftype(const PGresult* result, int column)
{
    return pg_fake::result_type(result, column);
}

int PQgetisnull(const PGresult* result, int row, int column)
{
    return pg_fake::result_is_null(result, row, column);
}

void PQclear(PGresult* result)
{
    ++pg_fake::cleared_results;
    delete result;
}

} // extern "C"
