#include "query.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace orm_c_detail {
namespace {

constexpr const char* reserved_id_field = "_id";

std::string array_key(std::size_t index)
{
    return std::to_string(index);
}

std::string field_name(std::string_view column, const mongo_settings& settings)
{
    return column == settings.id_column ? reserved_id_field
                                        : std::string(column);
}

std::string field_reference(std::string_view column,
                            const mongo_settings& settings)
{
    std::string reference = "$";
    reference += field_name(column, settings);
    return reference;
}


std::size_t checked_blob_length(std::size_t size)
{
    require(size <= static_cast<std::size_t>(
                         std::numeric_limits<std::uint32_t>::max()),
            ORM_STATUS_LIMIT_EXCEEDED,
            "blob value exceeds MongoDB's 32-bit length range");
    return size;
}

void append_parameter(bson_t* out,
                      const char* key,
                      const bound_parameter& parameter)
{
    switch (parameter.kind) {
    case ORM_VALUE_NULL:
        bson_append_null(out, key, -1);
        break;
    case ORM_VALUE_INT64:
        bson_append_int64(out, key, -1, parameter.int64_value);
        break;
    case ORM_VALUE_UINT64:
        require(parameter.uint64_value <=
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()),
                ORM_STATUS_OUT_OF_RANGE,
                "unsigned parameter exceeds MongoDB's signed 64-bit range");
        bson_append_int64(out, key, -1,
                          static_cast<std::int64_t>(parameter.uint64_value));
        break;
    case ORM_VALUE_DOUBLE:
        bson_append_double(out, key, -1, parameter.double_value);
        break;
    case ORM_VALUE_BOOLEAN:
        bson_append_bool(out, key, -1, parameter.boolean_value);
        break;
    case ORM_VALUE_TEXT:
        bson_append_utf8(out, key, -1, parameter.text.data(),
                         static_cast<int>(parameter.text.size()));
        break;
    case ORM_VALUE_BLOB:
        bson_append_binary(out, key, -1, BSON_SUBTYPE_BINARY,
                           reinterpret_cast<const std::uint8_t*>(
                               parameter.binary.data()),
                           static_cast<std::uint32_t>(
                               checked_blob_length(parameter.binary.size())));
        break;
    default:
        fail(ORM_STATUS_INTERNAL_ERROR,
             "generated MongoDB parameter has an unsupported type");
    }
}

void append_operand(bson_t* operator_document,
                    const char* operator_key,
                    const bound_parameter& parameter,
                    bool is_null)
{
    if (is_null)
        bson_append_null(operator_document, operator_key, -1);
    else
        append_parameter(operator_document, operator_key, parameter);
}

void append_predicate_keyed(bson_t* out,
                            const predicate& value,
                            std::string key)
{
    const bool is_null = !value.has_parameter;
    switch (value.comparison) {
    case ORM_COMPARE_EQUAL:
        if (is_null)
            bson_append_null(out, key.c_str(), -1);
        else
            append_parameter(out, key.c_str(), value.parameter);
        break;
    case ORM_COMPARE_NOT_EQUAL: {
        bson_t operator_document;
        bson_append_document_begin(out, key.c_str(), -1, &operator_document);
        append_operand(&operator_document, "$ne", value.parameter, is_null);
        bson_append_document_end(out, &operator_document);
        break;
    }
    case ORM_COMPARE_LESS: {
        bson_t operator_document;
        bson_append_document_begin(out, key.c_str(), -1, &operator_document);
        append_operand(&operator_document, "$lt", value.parameter, is_null);
        bson_append_document_end(out, &operator_document);
        break;
    }
    case ORM_COMPARE_LESS_EQUAL: {
        bson_t operator_document;
        bson_append_document_begin(out, key.c_str(), -1, &operator_document);
        append_operand(&operator_document, "$lte", value.parameter, is_null);
        bson_append_document_end(out, &operator_document);
        break;
    }
    case ORM_COMPARE_GREATER: {
        bson_t operator_document;
        bson_append_document_begin(out, key.c_str(), -1, &operator_document);
        append_operand(&operator_document, "$gt", value.parameter, is_null);
        bson_append_document_end(out, &operator_document);
        break;
    }
    case ORM_COMPARE_GREATER_EQUAL: {
        bson_t operator_document;
        bson_append_document_begin(out, key.c_str(), -1, &operator_document);
        append_operand(&operator_document, "$gte", value.parameter, is_null);
        bson_append_document_end(out, &operator_document);
        break;
    }
    case ORM_COMPARE_LIKE:
    case ORM_COMPARE_NOT_LIKE: {
        require(!is_null, ORM_STATUS_INVALID_ARGUMENT,
                "LIKE comparisons require a non-null text value");
        const std::string pattern = mongo_like_pattern(value.parameter.text);
        bson_t operator_document;
        bson_append_document_begin(out, key.c_str(), -1, &operator_document);
        if (value.comparison == ORM_COMPARE_LIKE) {
            bson_append_utf8(&operator_document, "$regex", -1,
                             pattern.data(),
                             static_cast<int>(pattern.size()));
        } else {
            bson_t not_document;
            bson_append_document_begin(&operator_document, "$not", -1,
                                       &not_document);
            bson_append_utf8(&not_document, "$regex", -1, pattern.data(),
                             static_cast<int>(pattern.size()));
            bson_append_document_end(&operator_document, &not_document);
        }
        bson_append_document_end(out, &operator_document);
        break;
    }
    default:
        fail(ORM_STATUS_INVALID_ARGUMENT, "unknown MongoDB comparison");
    }
}

void append_filter(bson_t* out,
                   const condition_node& node,
                   const mongo_settings& settings)
{
    if (!node.is_group) {
        append_predicate_keyed(out, node.value,
                               field_name(node.value.column, settings));
        return;
    }
    if (node.children.empty())
        return;
    const char* operator_key = node.logic == ORM_LOGIC_AND ? "$and" : "$or";
    bson_t array;
    bson_append_array_begin(out, operator_key, -1, &array);
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        const std::string key = array_key(index);
        bson_t child;
        bson_append_document_begin(&array, key.c_str(), -1, &child);
        append_filter(&child, *node.children[index], settings);
        bson_append_document_end(&array, &child);
    }
    bson_append_array_end(out, &array);
}

void append_accumulator(bson_t* accumulator,
                        const aggregate_expression& aggregate,
                        const mongo_settings& settings)
{
    switch (aggregate.kind) {
    case ORM_AGGREGATE_COUNT_ALL:
        bson_append_int32(accumulator, "$sum", -1, 1);
        break;
    case ORM_AGGREGATE_COUNT: {
        require(!aggregate.column.empty(), ORM_STATUS_INVALID_ARGUMENT,
                "MongoDB COUNT requires a column");
        const std::string reference = field_reference(aggregate.column, settings);
        bson_t sum;
        bson_append_document_begin(accumulator, "$sum", -1, &sum);
        bson_t condition;
        bson_append_document_begin(&sum, "$cond", -1, &condition);
        bson_t not_equal;
        bson_append_document_begin(&condition, "if", -1, &not_equal);
        bson_t operands;
        bson_append_array_begin(&not_equal, "$ne", -1, &operands);
        bson_append_utf8(&operands, "0", -1, reference.data(),
                         static_cast<int>(reference.size()));
        bson_append_null(&operands, "1", -1);
        bson_append_array_end(&not_equal, &operands);
        bson_append_document_end(&condition, &not_equal);
        bson_append_int32(&condition, "then", -1, 1);
        bson_append_int32(&condition, "else", -1, 0);
        bson_append_document_end(&sum, &condition);
        bson_append_document_end(accumulator, &sum);
        break;
    }
    case ORM_AGGREGATE_SUM:
    case ORM_AGGREGATE_AVERAGE:
    case ORM_AGGREGATE_MINIMUM:
    case ORM_AGGREGATE_MAXIMUM: {
        require(!aggregate.column.empty(), ORM_STATUS_INVALID_ARGUMENT,
                "MongoDB aggregate requires a column");
        const char* operator_key = nullptr;
        switch (aggregate.kind) {
        case ORM_AGGREGATE_SUM: operator_key = "$sum"; break;
        case ORM_AGGREGATE_AVERAGE: operator_key = "$avg"; break;
        case ORM_AGGREGATE_MINIMUM: operator_key = "$min"; break;
        case ORM_AGGREGATE_MAXIMUM: operator_key = "$max"; break;
        default: break;
        }
        const std::string reference = field_reference(aggregate.column, settings);
        bson_append_utf8(accumulator, operator_key, -1, reference.data(),
                         static_cast<int>(reference.size()));
        break;
    }
    default:
        fail(ORM_STATUS_INVALID_ARGUMENT, "unknown MongoDB aggregate kind");
    }
}

std::string aggregate_output_path(const query_plan& plan,
                                  std::string_view column)
{
    const auto group = std::find(plan.group_columns.begin(),
                                 plan.group_columns.end(), column);
    if (group != plan.group_columns.end()) {
        const std::size_t index =
            static_cast<std::size_t>(std::distance(plan.group_columns.begin(),
                                                   group));
        return std::string(reserved_id_field) + "." + mongo_group_key_name(index);
    }
    for (std::size_t index = 0; index < plan.aggregates.size(); ++index) {
        const aggregate_expression& aggregate = plan.aggregates[index];
        if (aggregate.alias == column ||
            mongo_aggregate_sql_name(aggregate) == column)
            return mongo_aggregate_value_name(index);
    }
    fail(ORM_STATUS_UNSUPPORTED,
         "MongoDB HAVING and ORDER BY can only reference grouped or "
         "aggregated columns: " + std::string(column));
}

void append_having_filter(bson_t* out,
                          const condition_node& node,
                          const query_plan& plan)
{
    if (!node.is_group) {
        append_predicate_keyed(out, node.value,
                               aggregate_output_path(plan, node.value.column));
        return;
    }
    if (node.children.empty())
        return;
    const char* operator_key = node.logic == ORM_LOGIC_AND ? "$and" : "$or";
    bson_t array;
    bson_append_array_begin(out, operator_key, -1, &array);
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        const std::string key = array_key(index);
        bson_t child;
        bson_append_document_begin(&array, key.c_str(), -1, &child);
        append_having_filter(&child, *node.children[index], plan);
        bson_append_document_end(&array, &child);
    }
    bson_append_array_end(out, &array);
}

std::int64_t checked_page_value(std::optional<std::uint64_t> value,
                                const char* role)
{
    if (!value.has_value())
        fail(ORM_STATUS_INTERNAL_ERROR, "MongoDB pagination value is missing");
    if (*value > static_cast<std::uint64_t>(
                     std::numeric_limits<std::int64_t>::max()))
        fail(ORM_STATUS_LIMIT_EXCEEDED,
             std::string(role) + " exceeds MongoDB's signed 64-bit range");
    return static_cast<std::int64_t>(*value);
}

} // namespace

std::string mongo_like_pattern(std::string_view sql_like)
{
    std::string pattern;
    pattern.reserve(sql_like.size() + 8);
    pattern += "\\A(?:";
    for (char next : sql_like) {
        switch (next) {
        case '%':
            pattern += ".*";
            break;
        case '_':
            pattern.push_back('.');
            break;
        case '.':
        case '^':
        case '$':
        case '*':
        case '+':
        case '?':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
        case '|':
        case '\\':
            pattern.push_back('\\');
            pattern.push_back(next);
            break;
        default:
            pattern.push_back(next);
            break;
        }
    }
    pattern += ")\\z";
    return pattern;
}

std::string mongo_aggregate_sql_name(const aggregate_expression& aggregate)
{
    const char* function = nullptr;
    switch (aggregate.kind) {
    case ORM_AGGREGATE_COUNT_ALL:
        return "count(*)";
    case ORM_AGGREGATE_COUNT: function = "count"; break;
    case ORM_AGGREGATE_SUM: function = "sum"; break;
    case ORM_AGGREGATE_AVERAGE: function = "avg"; break;
    case ORM_AGGREGATE_MINIMUM: function = "min"; break;
    case ORM_AGGREGATE_MAXIMUM: function = "max"; break;
    default:
        fail(ORM_STATUS_INVALID_ARGUMENT, "unknown MongoDB aggregate kind");
    }
    return std::string(function) + "(" + aggregate.column + ")";
}

void mongo_append_filter(bson_t* out,
                         const condition_node& root,
                         const mongo_settings& settings)
{
    append_filter(out, root, settings);
}

void mongo_append_find_options(bson_t* out,
                               const query_plan& plan,
                               const mongo_settings& settings)
{
    bson_t projection;
    bson_append_document_begin(out, "projection", -1, &projection);
    bool projects_id = false;
    
    // Check if id column is in the projection list
    for (const std::string& column : plan.columns) {
        if (column == settings.id_column) {
            projects_id = true;
            break;
        }
    }
    
    // Add _id first if it's projected
    if (projects_id)
        bson_append_int32(&projection, reserved_id_field, -1, 1);
    
    // Then add other columns
    for (const std::string& column : plan.columns) {
        if (column != settings.id_column) {
            require(column != reserved_id_field, ORM_STATUS_INVALID_ARGUMENT,
                    "MongoDB reserves _id for the configured id column");
            bson_append_int32(&projection, column.c_str(), -1, 1);
        }
    }
    
    // Suppress _id if not projected
    if (!projects_id)
        bson_append_int32(&projection, reserved_id_field, -1, 0);
        
    bson_append_document_end(out, &projection);

    if (plan.ordering && !plan.ordering->is_expression) {
        bson_t sort;
        bson_append_document_begin(out, "sort", -1, &sort);
        const std::string key = field_name(plan.ordering->column, settings);
        bson_append_int32(&sort, key.c_str(), -1,
                          plan.ordering->order == ORM_ORDER_DESCENDING ? -1 : 1);
        bson_append_document_end(out, &sort);
    }
    if (plan.offset)
        bson_append_int64(out, "skip", -1,
                          checked_page_value(plan.offset, "offset"));
    if (plan.limit)
        bson_append_int64(out, "limit", -1,
                          checked_page_value(plan.limit, "limit"));
}

void mongo_append_pipeline(bson_t* out,
                           const query_plan& plan,
                           const mongo_settings& settings)
{
    std::size_t stage = 0;
    if (!plan.where_root.children.empty()) {
        bson_t stage_document;
        bson_append_document_begin(out, array_key(stage++).c_str(), -1,
                                   &stage_document);
        bson_t match;
        bson_append_document_begin(&stage_document, "$match", -1, &match);
        append_filter(&match, plan.where_root, settings);
        bson_append_document_end(&stage_document, &match);
        bson_append_document_end(out, &stage_document);
    }

    bson_t stage_document;
    bson_append_document_begin(out, array_key(stage++).c_str(), -1,
                               &stage_document);
    bson_t group;
    bson_append_document_begin(&stage_document, "$group", -1, &group);
    if (plan.group_columns.empty()) {
        bson_append_null(&group, reserved_id_field, -1);
    } else {
        bson_t group_key;
        bson_append_document_begin(&group, reserved_id_field, -1, &group_key);
        for (std::size_t index = 0; index < plan.group_columns.size(); ++index) {
            const std::string key = mongo_group_key_name(index);
            const std::string reference =
                field_reference(plan.group_columns[index], settings);
            bson_append_utf8(&group_key, key.c_str(), -1, reference.data(),
                             static_cast<int>(reference.size()));
        }
        bson_append_document_end(&group, &group_key);
    }
    for (std::size_t index = 0; index < plan.aggregates.size(); ++index) {
        const std::string key = mongo_aggregate_value_name(index);
        bson_t accumulator;
        bson_append_document_begin(&group, key.c_str(), -1, &accumulator);
        append_accumulator(&accumulator, plan.aggregates[index], settings);
        bson_append_document_end(&group, &accumulator);
    }
    bson_append_document_end(&stage_document, &group);
    bson_append_document_end(out, &stage_document);

    if (!plan.having_root.children.empty()) {
        bson_t having_stage;
        bson_append_document_begin(out, array_key(stage++).c_str(), -1,
                                   &having_stage);
        bson_t match;
        bson_append_document_begin(&having_stage, "$match", -1, &match);
        
        // If there's only one condition and it's not a group, unwrap it
        if (plan.having_root.children.size() == 1 && 
            !plan.having_root.children[0]->is_group) {
            append_having_filter(&match, *plan.having_root.children[0], plan);
        } else {
            append_having_filter(&match, plan.having_root, plan);
        }
        
        bson_append_document_end(&having_stage, &match);
        bson_append_document_end(out, &having_stage);
    }
    if (plan.ordering && !plan.ordering->is_expression) {
        bson_t sort_stage;
        bson_append_document_begin(out, array_key(stage++).c_str(), -1,
                                   &sort_stage);
        bson_t sort;
        bson_append_document_begin(&sort_stage, "$sort", -1, &sort);
        const std::string key =
            aggregate_output_path(plan, plan.ordering->column);
        bson_append_int32(&sort, key.c_str(), -1,
                          plan.ordering->order == ORM_ORDER_DESCENDING ? -1 : 1);
        bson_append_document_end(&sort_stage, &sort);
        bson_append_document_end(out, &sort_stage);
    }
    if (plan.offset) {
        bson_t skip_stage;
        bson_append_document_begin(out, array_key(stage++).c_str(), -1,
                                   &skip_stage);
        bson_append_int64(&skip_stage, "$skip", -1,
                          checked_page_value(plan.offset, "offset"));
        bson_append_document_end(out, &skip_stage);
    }
    if (plan.limit) {
        bson_t limit_stage;
        bson_append_document_begin(out, array_key(stage++).c_str(), -1,
                                   &limit_stage);
        bson_append_int64(&limit_stage, "$limit", -1,
                          checked_page_value(plan.limit, "limit"));
        bson_append_document_end(out, &limit_stage);
    }
}

void mongo_append_insert_document(bson_t* out,
                                  const query_plan& plan,
                                  const mongo_settings& settings)
{
    require(!plan.assignments.empty(), ORM_STATUS_INVALID_STATE,
            "MongoDB INSERT has no values");
    bool has_id = false;
    for (const assignment& value : plan.assignments) {
        if (value.column == settings.id_column) {
            require(value.has_parameter, ORM_STATUS_INVALID_ARGUMENT,
                    "MongoDB entity id cannot be null");
            has_id = true;
            append_parameter(out, reserved_id_field, value.parameter);
        } else {
            require(value.column != reserved_id_field,
                    ORM_STATUS_INVALID_ARGUMENT,
                    "MongoDB reserves _id for the configured id column");
            if (value.has_parameter)
                append_parameter(out, value.column.c_str(), value.parameter);
            else
                bson_append_null(out, value.column.c_str(), -1);
        }
    }
    require(has_id, ORM_STATUS_INVALID_ARGUMENT,
            "MongoDB INSERT requires the configured id column");
}

void mongo_append_update_document(bson_t* out,
                                  const query_plan& plan,
                                  const mongo_settings& settings)
{
    require(!plan.assignments.empty(), ORM_STATUS_INVALID_STATE,
            "MongoDB UPDATE has no assignments");
    bson_t set;
    bson_append_document_begin(out, "$set", -1, &set);
    for (const assignment& value : plan.assignments) {
        require(value.column != settings.id_column, ORM_STATUS_UNSUPPORTED,
                "MongoDB UPDATE cannot change the configured id column");
        require(value.column != reserved_id_field,
                ORM_STATUS_INVALID_ARGUMENT,
                "MongoDB reserves _id for the configured id column");
        if (value.has_parameter)
            append_parameter(&set, value.column.c_str(), value.parameter);
        else
            bson_append_null(&set, value.column.c_str(), -1);
    }
    bson_append_document_end(out, &set);
}

void mongo_append_id_filter(bson_t* out,
                            const bound_parameter& id,
                            const mongo_settings& settings)
{
    (void)settings;
    append_parameter(out, reserved_id_field, id);
}

} // namespace orm_c_detail
