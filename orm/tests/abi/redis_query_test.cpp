#include "query.hpp"

#include <tinytest.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace orm_c_detail;

connection_limits limits()
{
    return connection_limits{256, 256, 256, 32, 64, 256, 16,
                             65536, 1048576, 10000, 16777216};
}

struct plan_fixture {
    query_kind kind = query_kind::select;
    std::string table = "person";
    std::string raw_sql;
    std::vector<std::string> columns;
    std::vector<aggregate_expression> aggregates;
    std::vector<assignment> assignments;
    std::vector<join_clause> joins;
    std::vector<std::string> group_columns;
    condition_node where_root;
    condition_node having_root;
    std::vector<bound_parameter> raw_parameters;
    std::optional<std::pair<std::string, orm_order_t>> ordering;
    std::optional<std::uint64_t> limit;
    std::optional<std::uint64_t> offset;
    std::size_t parameter_count = 0;
    bool select_all = false;

    query_plan view() const
    {
        return query_plan{kind, table, raw_sql, columns, aggregates, assignments,
                          joins, group_columns, where_root, having_root,
                          raw_parameters, ordering, limit, offset,
                          parameter_count, select_all};
    }

    void add_predicate(std::string column,
                       orm_compare_t comparison,
                       bound_parameter parameter)
    {
        auto node = std::make_unique<condition_node>();
        node->is_group = false;
        node->value.column = std::move(column);
        node->value.comparison = comparison;
        node->value.parameter = std::move(parameter);
        where_root.children.push_back(std::move(node));
        ++parameter_count;
    }
};

bound_parameter text_value(std::string value)
{
    bound_parameter parameter;
    parameter.kind = ORM_VALUE_TEXT;
    parameter.text = std::move(value);
    return parameter;
}

bound_parameter integer_value(std::int64_t value)
{
    bound_parameter parameter;
    parameter.kind = ORM_VALUE_INT64;
    parameter.int64_value = value;
    parameter.text = std::to_string(value);
    return parameter;
}

bound_parameter double_value(double value)
{
    bound_parameter parameter;
    parameter.kind = ORM_VALUE_DOUBLE;
    parameter.double_value = value;
    parameter.text = std::to_string(value);
    return parameter;
}

void check_arguments(const std::vector<std::string>& actual,
                     const std::vector<std::string>& expected)
{
    check_int_eq(actual.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index)
        check_str_eq(actual[index].c_str(), expected[index].c_str());
}

} // namespace

suite("orm redis query plan") {
    it("maps selection predicates sorting and paging to FT.SEARCH") {
        plan_fixture fixture;
        fixture.columns = {"id", "name"};
        fixture.add_predicate("status", ORM_COMPARE_EQUAL, text_value("active,user"));
        fixture.add_predicate("age", ORM_COMPARE_GREATER_EQUAL, integer_value(18));
        fixture.ordering = std::make_pair(std::string("age"), ORM_ORDER_DESCENDING);
        fixture.limit = 25;
        fixture.offset = 50;

        const redis_query_command command =
            build_redis_query_command(fixture.view(), limits(), "idx:");
        check(!command.aggregate);
        check_arguments(command.arguments,
                        {"FT.SEARCH", "idx:person",
                         "(@status:{active\\,user} @age:[18 +inf])",
                         "RETURN", "2", "id", "name", "SORTBY", "age",
                         "DESC", "LIMIT", "50", "25", "DIALECT", "2"});
        check_arguments(command.output_columns, {"id", "name"});
    }

    it("maps group reducers and paging to FT.AGGREGATE") {
        plan_fixture fixture;
        fixture.columns = {"country"};
        fixture.group_columns = {"country"};
        fixture.aggregates = {
            {ORM_AGGREGATE_COUNT_ALL, {}, "total"},
            {ORM_AGGREGATE_SUM, "score", "score_sum"}};
        fixture.limit = 10;

        const redis_query_command command =
            build_redis_query_command(fixture.view(), limits(), "idx:");
        check(command.aggregate);
        check_arguments(command.arguments,
                        {"FT.AGGREGATE", "idx:person", "*", "GROUPBY", "1",
                         "@country", "REDUCE", "COUNT", "0", "AS", "total",
                         "REDUCE", "SUM", "1", "@score", "AS", "score_sum",
                         "LIMIT", "0", "10", "DIALECT", "2"});
        check_arguments(command.output_columns,
                        {"country", "total", "score_sum"});
    }

    it("uses GROUPBY zero for a whole-result aggregate") {
        plan_fixture fixture;
        fixture.aggregates = {{ORM_AGGREGATE_COUNT_ALL, {}, "total"}};
        fixture.limit = 1;

        const redis_query_command command =
            build_redis_query_command(fixture.view(), limits(), "idx:");
        check_arguments(command.arguments,
                        {"FT.AGGREGATE", "idx:person", "*", "GROUPBY", "0",
                         "REDUCE", "COUNT", "0", "AS", "total",
                         "LIMIT", "0", "1", "DIALECT", "2"});
        check_arguments(command.output_columns, {"total"});
    }

    it("fails explicitly instead of scanning for select all") {
        plan_fixture fixture;
        fixture.select_all = true;
        bool rejected = false;
        try {
            (void)build_redis_query_command(fixture.view(), limits(), "idx:");
        } catch (const status_error& error) {
            rejected = error.status() == ORM_STATUS_UNSUPPORTED;
        }
        check(rejected);
    }

    it("rejects COUNT column because missing hashes differ from SQL null") {
        plan_fixture fixture;
        fixture.aggregates = {{ORM_AGGREGATE_COUNT, "score", "count_score"}};
        bool rejected = false;
        try {
            (void)build_redis_query_command(fixture.view(), limits(), "idx:");
        } catch (const status_error& error) {
            rejected = error.status() == ORM_STATUS_UNSUPPORTED;
        }
        check(rejected);
    }

    it("translates LIKE and NOT LIKE into DIALECT 2 wildcard patterns") {
        plan_fixture fixture;
        fixture.columns = {"id"};
        fixture.add_predicate("name", ORM_COMPARE_LIKE, text_value("A%"));
        const redis_query_command like_command =
            build_redis_query_command(fixture.view(), limits(), "idx:");
        check_arguments(like_command.arguments,
                        {"FT.SEARCH", "idx:person", "(@name:(\"w'A*'\"))",
                         "RETURN", "1", "id", "LIMIT", "0", "10000",
                         "DIALECT", "2"});

        plan_fixture not_like;
        not_like.columns = {"id"};
        not_like.add_predicate("name", ORM_COMPARE_NOT_LIKE, text_value("A%"));
        const redis_query_command not_like_command =
            build_redis_query_command(not_like.view(), limits(), "idx:");
        check_arguments(not_like_command.arguments,
                        {"FT.SEARCH", "idx:person",
                         "((@name:(\"w'*'\") -@name:(\"w'A*'\")))",
                         "RETURN", "1", "id", "LIMIT", "0", "10000",
                         "DIALECT", "2"});
    }

    it("escapes literal SQL LIKE wildcards and preserves underscore matching") {
        plan_fixture fixture;
        fixture.columns = {"id"};
        fixture.add_predicate("name", ORM_COMPARE_LIKE, text_value("a*b?c_d'e\\f"));
        const redis_query_command command =
            build_redis_query_command(fixture.view(), limits(), "idx:");
        check_str_eq(command.arguments[2].c_str(),
                     "(@name:(\"w'a\\*b\\?c?d\\'e\\\\f'\"))");
    }

    it("requires an existing numeric value for not-equal") {
        plan_fixture fixture;
        fixture.columns = {"id"};
        fixture.add_predicate("age", ORM_COMPARE_NOT_EQUAL, integer_value(18));
        const redis_query_command command =
            build_redis_query_command(fixture.view(), limits(), "idx:");
        check_str_eq(command.arguments[2].c_str(),
                     "((@age:[-inf +inf] -@age:[18 18]))");
    }

    it("renders double bounds in decimal form instead of scientific notation") {
        plan_fixture fixture;
        fixture.columns = {"id"};
        fixture.add_predicate("score", ORM_COMPARE_GREATER_EQUAL, double_value(1e20));
        const redis_query_command command =
            build_redis_query_command(fixture.view(), limits(), "idx:");
        check_str_eq(command.arguments[2].c_str(),
                     "(@score:[100000000000000000000 +inf])");

        plan_fixture fraction;
        fraction.columns = {"id"};
        fraction.add_predicate("score", ORM_COMPARE_EQUAL, double_value(0.0000001));
        const redis_query_command fraction_command =
            build_redis_query_command(fraction.view(), limits(), "idx:");
        check_str_eq(fraction_command.arguments[2].c_str(),
                     "(@score:[0.0000001 0.0000001])");
    }

    it("negates tag equality predicates") {
        plan_fixture fixture;
        fixture.columns = {"id"};
        fixture.add_predicate("status", ORM_COMPARE_NOT_EQUAL, text_value("active"));
        const redis_query_command command =
            build_redis_query_command(fixture.view(), limits(), "idx:");
        check_str_eq(command.arguments[2].c_str(), "(-@status:{active})");
    }
}
