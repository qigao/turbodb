#include "query.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <string_view>

namespace orm_c_detail {
namespace {

std::string numeric_value(const bound_parameter& value)
{
    switch (value.kind) {
    case ORM_VALUE_INT64:
    case ORM_VALUE_UINT64:
        return value.text;
    case ORM_VALUE_DOUBLE:
        return format_redis_double(value.double_value);
    case ORM_VALUE_BOOLEAN:
        return value.boolean_value ? "1" : "0";
    default:
        fail(ORM_STATUS_UNSUPPORTED,
             "Redis range predicates require a numeric or boolean value");
    }
}

bool is_tag_special(char value) noexcept
{
    switch (value) {
    case ',': case '.': case '<': case '>': case '{': case '}':
    case '[': case ']': case '"': case '\'': case ':': case ';':
    case '!': case '@': case '#': case '$': case '%': case '^':
    case '&': case '*': case '(': case ')': case '-': case '+':
    case '=': case '~': case '|': case '\\': case '/': case '?':
        return true;
    default:
        return std::isspace(static_cast<unsigned char>(value)) != 0;
    }
}

std::string escape_tag(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char next : value) {
        if (is_tag_special(next))
            escaped.push_back('\\');
        escaped.push_back(next);
    }
    return escaped;
}

std::string build_like_pattern(std::string_view value)
{
    // Translate a SQL LIKE pattern into a RediSearch DIALECT 2 wildcard
    // pattern: '%' -> '*' (zero or more characters) and '_' -> '?' (exactly
    // one character). Inside w'...' only the single quote and the backslash
    // are escapes; every other character, including punctuation that is
    // special elsewhere in a RediSearch query, is literal. SQL LIKE treats
    // '*', '?', quotes and backslashes as plain characters, so they are
    // backslash-escaped below to keep the SQL semantics.
    std::string pattern;
    pattern.reserve(value.size());
    for (char next : value) {
        switch (next) {
        case '%':
            pattern.push_back('*');
            break;
        case '_':
            pattern.push_back('?');
            break;
        case '*':
        case '?':
        case '\'':
        case '\\':
            // SQL LIKE treats '*', '?', quotes and backslashes as plain
            // characters, but inside w'...' they are wildcards or escapes, so
            // backslash-escape them to keep the SQL semantics.
            pattern.push_back('\\');
            pattern.push_back(next);
            break;
        default:
            pattern.push_back(next);
            break;
        }
    }
    return pattern;
}

std::string render_predicate(const predicate& value)
{
    require(value.has_parameter, ORM_STATUS_UNSUPPORTED,
            "Redis Query Engine does not index SQL NULL predicates");
    const std::string field = "@" + value.column;
    if (value.parameter.kind == ORM_VALUE_TEXT) {
        if (value.comparison == ORM_COMPARE_EQUAL ||
            value.comparison == ORM_COMPARE_NOT_EQUAL) {
            const std::string expression =
                field + ":{" + escape_tag(value.parameter.text) + "}";
            return value.comparison == ORM_COMPARE_NOT_EQUAL
                       ? "-" + expression
                       : expression;
        }
        if (value.comparison == ORM_COMPARE_LIKE ||
            value.comparison == ORM_COMPARE_NOT_LIKE) {
            const std::string pattern = build_like_pattern(value.parameter.text);
            const std::string expression =
                field + ":(\"w'" + pattern + "'\")";
            if (value.comparison == ORM_COMPARE_NOT_LIKE) {
                // SQL NOT LIKE excludes NULL rows. A bare RediSearch negation
                // would include documents whose field is absent, so first
                // require the field to contain at least one term.
                return "(" + field + ":(\"w'*'\") -" + expression + ")";
            }
            return expression;
        }
        fail(ORM_STATUS_UNSUPPORTED,
             "Redis text fields support only equality and LIKE predicates");
    }

    const std::string encoded = numeric_value(value.parameter);
    switch (value.comparison) {
    case ORM_COMPARE_EQUAL:
        return field + ":[" + encoded + " " + encoded + "]";
    case ORM_COMPARE_NOT_EQUAL:
        // SQL `field != value` excludes rows where the field is NULL. A bare
        // RediSearch negation would include absent fields, so first require an
        // existing numeric value, then exclude the equal value.
        return "(" + field + ":[-inf +inf] -" + field + ":[" + encoded + " " +
               encoded + "])";
    case ORM_COMPARE_LESS:
        return field + ":[-inf (" + encoded + "]";
    case ORM_COMPARE_LESS_EQUAL:
        return field + ":[-inf " + encoded + "]";
    case ORM_COMPARE_GREATER:
        return field + ":[(" + encoded + " +inf]";
    case ORM_COMPARE_GREATER_EQUAL:
        return field + ":[" + encoded + " +inf]";
    default:
        fail(ORM_STATUS_UNSUPPORTED,
             "Redis numeric fields do not support LIKE predicates");
    }
}

template <typename... Args>
void append_format(std::string& destination,
                   std::size_t maximum,
                   const char* format,
                   const Args&... args)
{
    tstr owned = tstr_format_typed_cpp(format, args...);
    if (owned == nullptr)
        fail(ORM_STATUS_OUT_OF_MEMORY, "format Redis query fragment failed");
    const vstr view = tstr_to_v(owned);
    if (view.len > maximum || destination.size() > maximum - view.len)
        fail(ORM_STATUS_LIMIT_EXCEEDED, "Redis query exceeds max_query_bytes");
    destination.append(view.data, view.len);
    tstr_free(owned);
}

std::string render_group(const condition_node& group, std::size_t maximum)
{
    require(group.is_group, ORM_STATUS_INTERNAL_ERROR,
            "Redis condition root is not a group");
    if (group.children.empty())
        return "*";

    std::string output;
    output.push_back('(');
    for (std::size_t index = 0; index < group.children.size(); ++index) {
        const char* separator = index == 0
                                    ? ""
                                    : (group.logic == ORM_LOGIC_OR ? "|" : " ");
        const condition_node& child = *group.children[index];
        const std::string child_text = child.is_group
                                           ? render_group(child, maximum)
                                           : render_predicate(child.value);
        append_format(output, maximum, "{}{}", separator, child_text);
    }
    output.push_back(')');
    return output;
}

std::string aggregate_alias(const aggregate_expression& aggregate,
                            std::size_t index)
{
    if (!aggregate.alias.empty())
        return aggregate.alias;
    return "__orm_aggregate_" + std::to_string(index);
}

void append_limit(redis_query_command& command,
                  const query_plan& plan,
                  const connection_limits& limits)
{
    const std::uint64_t count = plan.limit.value_or(limits.max_result_rows);
    require(count <= limits.max_result_rows, ORM_STATUS_LIMIT_EXCEEDED,
            "Redis query limit exceeds max_result_rows");
    command.arguments.push_back("LIMIT");
    command.arguments.push_back(std::to_string(plan.offset.value_or(0)));
    command.arguments.push_back(std::to_string(count));
}

void append_sort(redis_query_command& command, const query_plan& plan, bool aggregate)
{
    if (!plan.ordering)
        return;
    command.arguments.push_back("SORTBY");
    if (plan.ordering && plan.ordering->is_expression)
        fail(ORM_STATUS_UNSUPPORTED, "Redis ORDER BY expression is not supported");
    if (aggregate)
        command.arguments.push_back("2");
    command.arguments.push_back(aggregate ? "@" + plan.ordering->column
                                          : plan.ordering->column);
    command.arguments.push_back(plan.ordering->order == ORM_ORDER_DESCENDING
                                    ? "DESC"
                                    : "ASC");
}

redis_query_command build_search(const query_plan& plan,
                                 const connection_limits& limits,
                                 std::string_view index_prefix)
{
    require(!plan.select_all, ORM_STATUS_UNSUPPORTED,
            "Redis SELECT requires an explicit projection");
    require(!plan.columns.empty(), ORM_STATUS_INVALID_STATE,
            "Redis SELECT has no projected columns");
    redis_query_command command;
    command.arguments = {"FT.SEARCH", std::string(index_prefix) + std::string(plan.table),
                         render_group(plan.where_root, limits.max_query_bytes), "RETURN",
                         std::to_string(plan.columns.size())};
    for (const std::string& column : plan.columns) {
        command.arguments.push_back(column);
        command.output_columns.push_back(column);
    }
    append_sort(command, plan, false);
    append_limit(command, plan, limits);
    command.arguments.push_back("DIALECT");
    command.arguments.push_back("2");
    return command;
}

redis_query_command build_aggregate(const query_plan& plan,
                                    const connection_limits& limits,
                                    std::string_view index_prefix)
{
    require(!plan.select_all, ORM_STATUS_UNSUPPORTED,
            "Redis aggregation requires an explicit projection");
    require(plan.columns.empty() || !plan.group_columns.empty(),
            ORM_STATUS_UNSUPPORTED,
            "Redis aggregate projections must be grouped columns");
    redis_query_command command;
    command.aggregate = true;
    command.arguments = {"FT.AGGREGATE",
                         std::string(index_prefix) + std::string(plan.table),
                         render_group(plan.where_root, limits.max_query_bytes)};

    if (!plan.group_columns.empty() || !plan.aggregates.empty()) {
        command.arguments.push_back("GROUPBY");
        command.arguments.push_back(std::to_string(plan.group_columns.size()));
        for (const std::string& column : plan.group_columns)
            command.arguments.push_back("@" + column);
    }

    for (const std::string& column : plan.columns) {
        require(std::find(plan.group_columns.begin(), plan.group_columns.end(), column) !=
                    plan.group_columns.end(),
                ORM_STATUS_UNSUPPORTED,
                "Redis projected aggregate columns must appear in GROUP BY");
        command.output_columns.push_back(column);
    }

    for (std::size_t index = 0; index < plan.aggregates.size(); ++index) {
        const aggregate_expression& aggregate = plan.aggregates[index];
        const std::string alias = aggregate_alias(aggregate, index);
        command.arguments.push_back("REDUCE");
        switch (aggregate.kind) {
        case ORM_AGGREGATE_COUNT_ALL:
            command.arguments.push_back("COUNT");
            command.arguments.push_back("0");
            break;
        case ORM_AGGREGATE_COUNT:
            fail(ORM_STATUS_UNSUPPORTED,
                 "Redis COUNT(column) cannot preserve SQL null semantics");
        case ORM_AGGREGATE_SUM:
            command.arguments.insert(command.arguments.end(),
                                     {"SUM", "1", "@" + aggregate.column});
            break;
        case ORM_AGGREGATE_AVERAGE:
            command.arguments.insert(command.arguments.end(),
                                     {"AVG", "1", "@" + aggregate.column});
            break;
        case ORM_AGGREGATE_MINIMUM:
            command.arguments.insert(command.arguments.end(),
                                     {"MIN", "1", "@" + aggregate.column});
            break;
        case ORM_AGGREGATE_MAXIMUM:
            command.arguments.insert(command.arguments.end(),
                                     {"MAX", "1", "@" + aggregate.column});
            break;
        default:
            fail(ORM_STATUS_INVALID_ARGUMENT, "unknown aggregate kind");
        }
        command.arguments.push_back("AS");
        command.arguments.push_back(alias);
        command.output_columns.push_back(alias);
    }

    require(plan.having_root.children.empty(), ORM_STATUS_UNSUPPORTED,
            "Redis aggregate HAVING is not supported by the portable ORM adapter");
    append_sort(command, plan, true);
    append_limit(command, plan, limits);
    command.arguments.push_back("DIALECT");
    command.arguments.push_back("2");
    return command;
}

} // namespace

redis_query_command
build_redis_query_command(const query_plan& plan,
                          const connection_limits& limits,
                          std::string_view index_prefix)
{
    require(!plan.distinct, ORM_STATUS_UNSUPPORTED,
            "Redis DISTINCT queries are not supported");
    require(plan.kind == query_kind::select, ORM_STATUS_UNSUPPORTED,
            "Redis Query Engine commands require a SELECT plan");
    require(plan.joins.empty(), ORM_STATUS_UNSUPPORTED,
            "Redis Query Engine does not support ORM joins");
    require(!plan.table.empty(), ORM_STATUS_INVALID_ARGUMENT,
            "Redis query table is empty");
    require(!index_prefix.empty(), ORM_STATUS_INVALID_ARGUMENT,
            "Redis index prefix is empty");
    return plan.aggregates.empty() && plan.group_columns.empty()
               ? build_search(plan, limits, index_prefix)
               : build_aggregate(plan, limits, index_prefix);
}

} // namespace orm_c_detail
