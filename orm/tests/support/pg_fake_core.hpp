#ifndef ORM_TEST_PG_FAKE_CORE_HPP
#define ORM_TEST_PG_FAKE_CORE_HPP

#include <libpq-fe.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// Shared PostgreSQL/libpq test double state and helpers.
//
// The two libpq fakes (`c_api_fake_libpq.cpp` and `dbs/postgres/test.cpp`)
// replace the same external C ABI.  Keeping the result model, recorded call
// shape, scripting state, and result helpers here avoids the two fakes
// drifting apart while leaving their distinct ABI entry points and support
// APIs in their respective test files.

struct pg_conn {
    ConnStatusType status = CONNECTION_OK;
    std::string error;
};

struct pg_result {
    ExecStatusType status = PGRES_TUPLES_OK;
    std::string error;
    int columns = 0;
    std::vector<std::vector<std::optional<std::string>>> rows;
    std::vector<Oid> column_types;
    std::string affected_rows;
    bool scripted_dimensions = false;
    int scripted_rows = 0;
    int scripted_columns = 0;
};

struct recorded_pg_call {
    std::string sql;
    std::vector<std::optional<std::string>> parameter_values;
    std::vector<Oid> parameter_types;
    std::vector<int> parameter_lengths;
    std::vector<int> parameter_formats;
    int result_format = 0;
    bool from_prepared = false;
};

struct scripted_pg_call {
    bool has_status = false;
    ExecStatusType status = PGRES_COMMAND_OK;
    bool has_error = false;
    std::string error;
    bool has_dimensions = false;
    int rows = 0;
    int columns = 0;
    bool has_affected_rows = false;
    std::string affected_rows;
    bool return_null_result = false;
};

namespace pg_fake {

inline std::vector<std::vector<std::optional<std::string>>> next_rows;
inline std::vector<Oid> next_column_types;
inline int next_columns = 0;
inline bool fail_next_query = false;
inline std::string last_sql;
inline std::vector<std::optional<std::string>> last_parameters;
inline int finished_connections = 0;
inline int cleared_results = 0;
inline int created_results = 0;
inline int prepared_calls = 0;
inline int fail_prepared_call = -1;
inline std::vector<std::string> commands;
inline std::vector<std::string> parameterized_sql;
inline std::vector<std::vector<std::string>> parameter_calls;
inline std::vector<recorded_pg_call> pg_calls;
inline scripted_pg_call next_call;

inline pg_result* make_result(ExecStatusType status)
{
    auto* result = new pg_result;
    result->status = status;
    return result;
}

inline std::vector<std::string> copy_parameters(int count,
                                                const char* const* values)
{
    std::vector<std::string> copied;
    copied.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index)
        copied.emplace_back(values[index]);
    return copied;
}

inline std::vector<std::optional<std::string>>
copy_optional_parameters(int count, const char* const* values)
{
    std::vector<std::optional<std::string>> copied;
    copied.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        if (values[index] == nullptr)
            copied.push_back(std::nullopt);
        else
            copied.emplace_back(values[index]);
    }
    return copied;
}

inline std::vector<Oid> copy_oids(int count, const Oid* values)
{
    std::vector<Oid> copied;
    if (values == nullptr)
        return copied;
    copied.assign(values, values + count);
    return copied;
}

inline std::vector<int> copy_ints(int count, const int* values)
{
    std::vector<int> copied;
    if (values == nullptr)
        return copied;
    copied.assign(values, values + count);
    return copied;
}

inline void reset_script_state()
{
    next_rows.clear();
    next_column_types.clear();
    next_columns = 0;
    fail_next_query = false;
    last_sql.clear();
    last_parameters.clear();
    prepared_calls = 0;
    fail_prepared_call = -1;
    commands.clear();
    parameterized_sql.clear();
    parameter_calls.clear();
    pg_calls.clear();
    next_call = scripted_pg_call{};
}

inline char* result_value(const pg_result* result, int row, int column)
{
    const auto& cell = result->rows.at(static_cast<std::size_t>(row))
                           .at(static_cast<std::size_t>(column));
    static char empty[] = "";
    return cell ? const_cast<char*>(cell->c_str()) : empty;
}

inline int result_length(const pg_result* result, int row, int column)
{
    const auto& cell = result->rows.at(static_cast<std::size_t>(row))
                           .at(static_cast<std::size_t>(column));
    return cell ? static_cast<int>(cell->size()) : 0;
}

inline int result_is_null(const pg_result* result, int row, int column)
{
    return result->rows.at(static_cast<std::size_t>(row))
               .at(static_cast<std::size_t>(column))
               .has_value()
               ? 0
               : 1;
}

inline Oid result_type(const pg_result* result, int column)
{
    if (result->column_types.empty())
        return 25;
    return result->column_types.at(static_cast<std::size_t>(column));
}

} // namespace pg_fake

#endif
