#ifndef ORM_C_INTERNAL_HPP
#define ORM_C_INTERNAL_HPP

#include "orm.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orm_c_detail {

struct connection_limits {
    std::size_t max_parameters;
    std::size_t max_columns;
    std::size_t max_predicates;
    std::size_t max_joins;
    std::size_t max_group_columns;
    std::size_t max_assignments;
    std::size_t max_condition_depth;
    std::size_t max_query_bytes;
    std::size_t max_parameter_bytes;
    std::uint64_t max_result_rows;
    std::uint64_t max_result_bytes;
};

struct bound_parameter {
    orm_value_kind_t kind = ORM_VALUE_NULL;
    std::string text;
    std::string binary;
    std::int64_t int64_value = 0;
    std::uint64_t uint64_value = 0;
    double double_value = 0.0;
    bool boolean_value = false;
};

enum class query_kind {
    select,
    insert,
    update,
    remove,
    raw
};

struct predicate {
    std::string column;
    std::string operation;
    std::string right_column;
    orm_compare_t comparison = ORM_COMPARE_EQUAL;
    bound_parameter parameter;
    bool has_parameter = true;
    bool has_column_operand = false;
};

struct aggregate_expression {
    orm_aggregate_t kind;
    std::string column;
    std::string alias;
};

struct scalar_token {
    orm_scalar_token_kind_t kind;
    std::string column;
    bound_parameter parameter;
};

struct scalar_projection {
    std::vector<scalar_token> tokens;
    std::string alias;
};

struct ordering_spec {
    bool is_expression = false;
    std::string column;
    std::vector<scalar_token> tokens;
    orm_order_t order = ORM_ORDER_ASCENDING;
};

struct assignment {
    std::string column;
    bound_parameter parameter;
    bool has_parameter = true;
};

struct join_clause {
    orm_join_t kind;
    std::string table;
    std::string left_column;
    std::string operation;
    std::string right_column;
};

struct condition_node {
    bool is_group = true;
    bool is_exists = false;
    bool is_in_subquery = false;
    bool is_scalar_subquery = false;
    bool negated = false;
    orm_logic_t logic = ORM_LOGIC_AND;
    predicate value;
    std::shared_ptr<const ::orm_query> subquery;
    std::string subquery_column;
    std::string subquery_operation;
    std::string subquery_quantifier;
    std::vector<std::unique_ptr<condition_node>> children;
};

struct query_plan {
    query_kind kind;
    std::string_view table;
    std::string_view raw_sql;
    const std::vector<std::string>& columns;
    const std::vector<aggregate_expression>& aggregates;
    const std::vector<assignment>& assignments;
    const std::vector<join_clause>& joins;
    const std::vector<std::string>& group_columns;
    const condition_node& where_root;
    const condition_node& having_root;
    const std::vector<bound_parameter>& raw_parameters;
    const std::optional<ordering_spec>& ordering;
    const std::optional<std::uint64_t>& limit;
    const std::optional<std::uint64_t>& offset;
    std::size_t parameter_count;
    bool select_all;
    bool distinct;
};

class status_error final : public std::runtime_error {
public:
    status_error(orm_status_t status, std::string message)
        : std::runtime_error(std::move(message)), status_(status)
    {
    }

    orm_status_t status() const noexcept { return status_; }

private:
    orm_status_t status_;
};

[[noreturn]] inline void fail(orm_status_t status, std::string message)
{
    throw status_error(status, std::move(message));
}

inline void require(bool condition, orm_status_t status, const char* message)
{
    if (!condition)
        fail(status, message);
}

class result_backend {
public:
    virtual ~result_backend() = default;
    virtual std::uint64_t rows() const = 0;
    virtual std::uint64_t columns() const = 0;
    virtual std::uint64_t affected_rows() const = 0;
    virtual bool is_null(std::uint64_t row, std::uint64_t column) const = 0;
    virtual vstr cell(std::uint64_t row, std::uint64_t column) const = 0;
};

class transaction_backend {
public:
    virtual ~transaction_backend() = default;

    virtual std::unique_ptr<result_backend>
    execute_sql(std::string_view,
                const std::vector<bound_parameter>&,
                bool,
                const connection_limits&)
    {
        fail(ORM_STATUS_UNSUPPORTED,
             "database transaction does not support SQL execution");
    }

    virtual std::unique_ptr<result_backend>
    execute_plan(const query_plan&, const connection_limits&)
    {
        fail(ORM_STATUS_UNSUPPORTED,
             "database transaction does not support native query plans");
    }

    virtual void commit() = 0;
    virtual void rollback() = 0;

    virtual void savepoint(std::string_view)
    {
        fail(ORM_STATUS_UNSUPPORTED,
             "database transaction does not support savepoints");
    }

    virtual void rollback_to_savepoint(std::string_view)
    {
        fail(ORM_STATUS_UNSUPPORTED,
             "database transaction does not support savepoints");
    }

    virtual void release_savepoint(std::string_view)
    {
        fail(ORM_STATUS_UNSUPPORTED,
             "database transaction does not support savepoints");
    }
};

class database_backend {
public:
    enum class execution_model {
        sql,
        native_plan
    };

    virtual ~database_backend() = default;
    virtual execution_model model() const noexcept { return execution_model::sql; }
    virtual std::string placeholder(std::size_t one_based_index) const = 0;
    virtual std::string pagination(std::optional<std::uint64_t> limit,
                                   std::optional<std::uint64_t> offset) const = 0;
    virtual std::unique_ptr<result_backend>
    execute_sql(std::string_view sql,
                const std::vector<bound_parameter>& parameters,
                bool structured_dml,
                const connection_limits& limits) = 0;

    virtual std::unique_ptr<result_backend>
    execute_plan(const query_plan&, const connection_limits&)
    {
        fail(ORM_STATUS_UNSUPPORTED,
             "database backend does not support native query plans");
    }

    virtual std::unique_ptr<transaction_backend>
    begin_transaction(orm_isolation_t)
    {
        fail(ORM_STATUS_UNSUPPORTED,
             "database backend does not support explicit transactions");
    }
};

enum class sqlite_open_mode {
    read_only,
    read_write,
    read_write_create
};

#if defined(ORM_WITH_PGSQL)
std::unique_ptr<database_backend>
make_postgres_backend(const std::vector<std::string>& keywords,
                      const std::vector<std::string>& values);
#endif

#if defined(ORM_WITH_SQLITE)
std::unique_ptr<database_backend>
make_sqlite_backend(std::string filename,
                    sqlite_open_mode open_mode,
                    std::uint32_t busy_timeout_ms,
                    const connection_limits& limits);
#endif

#if defined(ORM_WITH_REDIS)
std::unique_ptr<database_backend>
make_redis_backend(const std::vector<std::string>& keywords,
                   const std::vector<std::string>& values,
                   const connection_limits& limits);
#endif

#if defined(ORM_WITH_TIDESDB)
std::unique_ptr<database_backend>
make_tidesdb_backend(const std::vector<std::string>& keywords,
                     const std::vector<std::string>& values,
                     const connection_limits& limits);
#endif

#if defined(ORM_WITH_MONGO)
std::unique_ptr<database_backend>
make_mongo_backend(const std::vector<std::string>& keywords,
                   const std::vector<std::string>& values,
                   const connection_limits& limits);
#endif

} // namespace orm_c_detail

#endif
