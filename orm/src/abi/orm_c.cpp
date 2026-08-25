#if !defined(ORM_C_STATIC) && !defined(ORM_C_BUILD)
#  define ORM_C_BUILD
#endif
#include "orm.h"

#include "orm_c_internal.hpp"
#include "mustache/mustache.h"
#include "mustache_render.hpp"

#include <algorithm>
#include <charconv>
#include <climits>
#include <cstddef>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t max_identifier_segment_bytes = 63;
constexpr std::size_t max_qualified_identifier_bytes = 255;
constexpr std::size_t max_connection_options = 64;
constexpr std::size_t max_connection_option_value_bytes = 16384;
constexpr std::size_t max_connection_option_total_bytes = 65536;
constexpr std::size_t max_sqlite_filename_bytes = 32767;
constexpr std::uint32_t default_embedded_busy_timeout_ms = 5000;

using orm_c_detail::bound_parameter;
using orm_c_detail::aggregate_expression;
using orm_c_detail::scalar_projection;
using orm_c_detail::scalar_token;
using orm_c_detail::assignment;
using orm_c_detail::condition_node;
using orm_c_detail::connection_limits;
using orm_c_detail::database_backend;
using orm_c_detail::fail;
using orm_c_detail::require;
using orm_c_detail::result_backend;
using orm_c_detail::join_clause;
using orm_c_detail::predicate;
using orm_c_detail::ordering_spec;
using orm_c_detail::query_kind;
using orm_c_detail::query_plan;
using orm_c_detail::status_error;
using orm_c_detail::transaction_backend;

namespace mustache_detail = orm_c_detail::mustache_detail;

struct connection_state {
    std::unique_ptr<database_backend> backend;
    connection_limits limits;
};

constexpr std::size_t legacy_error_size = offsetof(orm_error_t, backend_code);
constexpr std::size_t max_registered_drivers = 16;

struct driver_registry {
    std::mutex mutex;
    std::unordered_map<std::string, orm_c_detail::database_backend_factory> factories;
};

driver_registry& database_drivers()
{
    static driver_registry registry;
    return registry;
}

bool register_database_driver_impl(
    std::string_view name, orm_c_detail::database_backend_factory factory)
{
    if (name.empty() || factory == nullptr)
        return false;
    auto& registry = database_drivers();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto found = registry.factories.find(std::string(name));
    if (found != registry.factories.end())
        return found->second == factory;
    if (registry.factories.size() >= max_registered_drivers)
        return false;
    registry.factories.emplace(name, factory);
    return true;
}

orm_c_detail::database_backend_factory find_database_driver(std::string_view name)
{
    auto& registry = database_drivers();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto found = registry.factories.find(std::string(name));
    return found != registry.factories.end() ? found->second : nullptr;
}

void write_error(orm_error_t* error, orm_status_t status, const char* message,
                 const char* backend_code = nullptr) noexcept
{
    if (error == nullptr)
        return;

    const std::size_t reported_size = error->struct_size;
    const std::size_t status_end = offsetof(orm_error_t, status) + sizeof(error->status);
    if (reported_size >= status_end)
        error->status = status;

    const std::size_t message_offset = offsetof(orm_error_t, message);
    if (reported_size <= message_offset)
        return;

    const std::size_t available = std::min<std::size_t>(
        reported_size - message_offset, ORM_C_ERROR_MESSAGE_CAPACITY);
    if (available == 0)
        return;

    const char* source = message != nullptr ? message : "";
    const std::size_t source_size = std::strlen(source);
    const std::size_t copy_size = std::min(source_size, available - 1);
    std::memcpy(error->message, source, copy_size);
    error->message[copy_size] = '\0';

    const std::size_t code_offset = offsetof(orm_error_t, backend_code);
    if (reported_size <= code_offset)
        return;
    const std::size_t code_available = std::min<std::size_t>(
        reported_size - code_offset, ORM_C_BACKEND_CODE_CAPACITY);
    if (code_available == 0)
        return;
    const char* code_source = backend_code != nullptr ? backend_code : "";
    const std::size_t code_size = std::strlen(code_source);
    const std::size_t code_copy_size = std::min(code_size, code_available - 1);
    std::memcpy(error->backend_code, code_source, code_copy_size);
    error->backend_code[code_copy_size] = '\0';
}

template<typename Function>
orm_status_t api_call(orm_error_t* error, Function&& function) noexcept
{
    write_error(error, ORM_STATUS_OK, "");
    try {
        function();
        return ORM_STATUS_OK;
    } catch (const status_error& exception) {
        write_error(error, exception.status(), exception.what(),
                    exception.backend_code().c_str());
        return exception.status();
    } catch (const std::bad_alloc&) {
        write_error(error, ORM_STATUS_OUT_OF_MEMORY, "memory allocation failed");
        return ORM_STATUS_OUT_OF_MEMORY;
    } catch (const std::exception& exception) {
        write_error(error, ORM_STATUS_INTERNAL_ERROR, exception.what());
        return ORM_STATUS_INTERNAL_ERROR;
    } catch (...) {
        write_error(error, ORM_STATUS_INTERNAL_ERROR, "unknown C++ exception");
        return ORM_STATUS_INTERNAL_ERROR;
    }
}

bool is_identifier_start(char value) noexcept
{
    return (value >= 'a' && value <= 'z') ||
           (value >= 'A' && value <= 'Z') || value == '_';
}

bool is_identifier_continue(char value) noexcept
{
    return is_identifier_start(value) || (value >= '0' && value <= '9');
}

std::string copy_string_view(orm_string_view_t view,
                             const char* role,
                             bool allow_empty,
                             std::size_t maximum_size)
{
    if (view.len != 0 && view.data == nullptr)
        fail(ORM_STATUS_INVALID_ARGUMENT, std::string(role) + " has a null data pointer");
    if (!allow_empty && view.len == 0)
        fail(ORM_STATUS_INVALID_ARGUMENT, std::string(role) + " is empty");
    if (view.len > maximum_size)
        fail(ORM_STATUS_LIMIT_EXCEEDED, std::string(role) + " exceeds its byte limit");
    if (view.len != 0 && std::memchr(view.data, '\0', view.len) != nullptr)
        fail(ORM_STATUS_INVALID_ARGUMENT, std::string(role) + " contains an embedded null byte");
    return view.len == 0 ? std::string() : std::string(view.data, view.len);
}

std::string copy_blob_view(orm_blob_t view,
                             const char* role,
                             bool allow_empty,
                             std::size_t maximum_size)
{
    if (view.size != 0 && view.data == nullptr)
        fail(ORM_STATUS_INVALID_ARGUMENT, std::string(role) + " has a null data pointer");
    if (!allow_empty && view.size == 0)
        fail(ORM_STATUS_INVALID_ARGUMENT, std::string(role) + " is empty");
    if (view.size > maximum_size)
        fail(ORM_STATUS_LIMIT_EXCEEDED, std::string(role) + " exceeds its byte limit");
    return view.size == 0
               ? std::string()
               : std::string(static_cast<const char*>(view.data), view.size);
}

std::string hex_encode(std::string_view input)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size() * 2);
    for (unsigned char next : input) {
        output.push_back(digits[next >> 4]);
        output.push_back(digits[next & 0x0fU]);
    }
    return output;
}

std::size_t parameter_payload_size(const bound_parameter& parameter)
{
    return parameter.kind == ORM_VALUE_BLOB
               ? parameter.binary.size()
               : parameter.text.size();
}

std::string copy_identifier(orm_string_view_t view, const char* role, bool qualified)
{
    std::string identifier =
        copy_string_view(view, role, false, max_qualified_identifier_bytes);

    std::size_t segment_size = 0;
    for (std::size_t index = 0; index < identifier.size(); ++index) {
        const char value = identifier[index];
        if (qualified && value == '.') {
            if (segment_size == 0)
                fail(ORM_STATUS_INVALID_ARGUMENT,
                     std::string(role) + " contains an empty identifier segment");
            segment_size = 0;
            continue;
        }

        if (segment_size == 0) {
            if (!is_identifier_start(value))
                fail(ORM_STATUS_INVALID_ARGUMENT,
                     std::string(role) + " has an invalid first character");
        } else if (!is_identifier_continue(value)) {
            fail(ORM_STATUS_INVALID_ARGUMENT,
                 std::string(role) + " contains an invalid character");
        }

        ++segment_size;
        if (segment_size > max_identifier_segment_bytes)
            fail(ORM_STATUS_LIMIT_EXCEEDED,
                 std::string(role) + " segment exceeds 63 bytes");
    }

    if (segment_size == 0)
        fail(ORM_STATUS_INVALID_ARGUMENT,
             std::string(role) + " contains an empty identifier segment");
    return identifier;
}

void checked_add(std::size_t& total, std::size_t amount, std::size_t maximum,
                 const char* message)
{
    if (amount > maximum || total > maximum - amount)
        fail(ORM_STATUS_LIMIT_EXCEEDED, message);
    total += amount;
}

void append_checked(std::string& destination,
                    std::string_view value,
                    std::size_t maximum_size)
{
    if (value.size() > maximum_size || destination.size() > maximum_size - value.size())
        fail(ORM_STATUS_LIMIT_EXCEEDED, "SQL query exceeds max_query_bytes");
    destination.append(value.data(), value.size());
}

connection_limits validate_limits(std::uint32_t max_parameters,
                                  std::uint32_t max_columns,
                                  std::uint32_t max_predicates,
                                  std::uint32_t max_joins,
                                  std::uint32_t max_group_columns,
                                  std::uint32_t max_assignments,
                                  std::uint32_t max_condition_depth,
                                  std::uint64_t max_query_bytes,
                                  std::uint64_t max_parameter_bytes,
                                  std::uint64_t max_result_rows,
                                  std::uint64_t max_result_bytes)
{
    require(max_parameters != 0 && max_parameters <= INT_MAX,
            ORM_STATUS_INVALID_ARGUMENT,
            "max_parameters must be in [1, INT_MAX]");
    require(max_columns != 0 && max_columns <= INT_MAX,
            ORM_STATUS_INVALID_ARGUMENT,
            "max_columns must be in [1, INT_MAX]");
    require(max_predicates != 0 && max_predicates <= INT_MAX,
            ORM_STATUS_INVALID_ARGUMENT,
            "max_predicates must be in [1, INT_MAX]");
    require(max_joins != 0 && max_joins <= INT_MAX,
            ORM_STATUS_INVALID_ARGUMENT, "max_joins must be in [1, INT_MAX]");
    require(max_group_columns != 0 && max_group_columns <= INT_MAX,
            ORM_STATUS_INVALID_ARGUMENT,
            "max_group_columns must be in [1, INT_MAX]");
    require(max_assignments != 0 && max_assignments <= INT_MAX,
            ORM_STATUS_INVALID_ARGUMENT,
            "max_assignments must be in [1, INT_MAX]");
    require(max_condition_depth != 0 && max_condition_depth <= INT_MAX,
            ORM_STATUS_INVALID_ARGUMENT,
            "max_condition_depth must be in [1, INT_MAX]");
    require(max_query_bytes != 0 &&
                max_query_bytes <= std::numeric_limits<std::size_t>::max(),
            ORM_STATUS_INVALID_ARGUMENT,
            "max_query_bytes is invalid for this platform");
    require(max_parameter_bytes != 0 &&
                max_parameter_bytes <= std::numeric_limits<std::size_t>::max(),
            ORM_STATUS_INVALID_ARGUMENT,
            "max_parameter_bytes is invalid for this platform");
    require(max_result_rows != 0, ORM_STATUS_INVALID_ARGUMENT,
            "max_result_rows must be nonzero");
    require(max_result_bytes != 0, ORM_STATUS_INVALID_ARGUMENT,
            "max_result_bytes must be nonzero");

    return connection_limits{
        max_parameters,
        max_columns,
        max_predicates,
        max_joins,
        max_group_columns,
        max_assignments,
        max_condition_depth,
        static_cast<std::size_t>(max_query_bytes),
        static_cast<std::size_t>(max_parameter_bytes),
        max_result_rows,
        max_result_bytes};
}

connection_limits validate_config(const orm_config_t& config)
{
    require(config.struct_size >= sizeof(orm_config_t),
            ORM_STATUS_ABI_MISMATCH,
            "ORM config struct is smaller than ABI version 2");
    require(config.abi_version == ORM_C_ABI_VERSION,
            ORM_STATUS_ABI_MISMATCH,
            "unsupported ORM C ABI version");
    require(config.driver.data != nullptr && config.driver.len != 0,
            ORM_STATUS_INVALID_ARGUMENT,
            "driver is required");
    require(config.option_count == 0 || config.options != nullptr,
            ORM_STATUS_INVALID_ARGUMENT,
            "options pointer is null");
    require(config.option_count <= max_connection_options,
            ORM_STATUS_LIMIT_EXCEEDED,
            "too many connection options");
    return validate_limits(config.max_parameters,
                           config.max_columns,
                           config.max_predicates,
                           config.max_joins,
                           config.max_group_columns,
                           config.max_assignments,
                           config.max_condition_depth,
                           config.max_query_bytes,
                           config.max_parameter_bytes,
                           config.max_result_rows,
                           config.max_result_bytes);
}

struct sqlite_settings {
    std::string filename;
    orm_c_detail::sqlite_open_mode open_mode =
        orm_c_detail::sqlite_open_mode::read_write_create;
    std::uint32_t busy_timeout_ms = default_embedded_busy_timeout_ms;
};

sqlite_settings parse_sqlite_settings(const std::vector<std::string>& keywords,
                                      const std::vector<std::string>& values)
{
    sqlite_settings settings;
    bool has_filename = false;
    for (std::size_t index = 0; index < keywords.size(); ++index) {
        const std::string& keyword = keywords[index];
        const std::string& value = values[index];
        if (keyword == "filename") {
            require(!value.empty(), ORM_STATUS_INVALID_ARGUMENT,
                    "filename option is empty");
            require(value.size() <= max_sqlite_filename_bytes,
                    ORM_STATUS_LIMIT_EXCEEDED,
                    "filename option exceeds its byte limit");
            settings.filename = value;
            has_filename = true;
        } else if (keyword == "open_mode") {
            if (value == "read_only") {
                settings.open_mode = orm_c_detail::sqlite_open_mode::read_only;
            } else if (value == "read_write") {
                settings.open_mode = orm_c_detail::sqlite_open_mode::read_write;
            } else if (value == "read_write_create") {
                settings.open_mode = orm_c_detail::sqlite_open_mode::read_write_create;
            } else {
                fail(ORM_STATUS_INVALID_ARGUMENT, "unknown open_mode option value");
            }
        } else if (keyword == "busy_timeout_ms") {
            std::uint32_t timeout = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), timeout);
            require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                        timeout <= static_cast<std::uint32_t>(INT_MAX),
                    ORM_STATUS_INVALID_ARGUMENT,
                    "busy_timeout_ms must be an integer in [0, INT_MAX]");
            settings.busy_timeout_ms = timeout;
        } else {
            fail(ORM_STATUS_INVALID_ARGUMENT,
                 "unsupported option for the selected driver: " + keyword);
        }
    }
    require(has_filename, ORM_STATUS_INVALID_ARGUMENT,
            "filename option is required for the selected driver");
    return settings;
}

std::string comparison_sql(orm_compare_t comparison)
{
    switch (comparison) {
    case ORM_COMPARE_EQUAL:
        return "=";
    case ORM_COMPARE_NOT_EQUAL:
        return "!=";
    case ORM_COMPARE_LESS:
        return "<";
    case ORM_COMPARE_LESS_EQUAL:
        return "<=";
    case ORM_COMPARE_GREATER:
        return ">";
    case ORM_COMPARE_GREATER_EQUAL:
        return ">=";
    case ORM_COMPARE_LIKE:
        return "like";
    case ORM_COMPARE_NOT_LIKE:
        return "not like";
    default:
        fail(ORM_STATUS_INVALID_ARGUMENT, "unknown comparison operator");
    }
}

bound_parameter encode_parameter(orm_value_t value, std::size_t maximum_size)
{
    require(value.reserved == 0, ORM_STATUS_ABI_MISMATCH,
            "orm_value_t.reserved must be zero");
    bound_parameter parameter;
    parameter.kind = value.kind;
    switch (value.kind) {
    case ORM_VALUE_INT64:
        parameter.int64_value = value.data.int64_value;
        parameter.text = std::to_string(value.data.int64_value);
        break;
    case ORM_VALUE_UINT64:
        parameter.uint64_value = value.data.uint64_value;
        parameter.text = std::to_string(value.data.uint64_value);
        break;
    case ORM_VALUE_DOUBLE: {
        parameter.double_value = value.data.double_value;
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(std::numeric_limits<double>::max_digits10)
               << value.data.double_value;
        if (!stream)
            fail(ORM_STATUS_TYPE_ERROR, "failed to encode double parameter");
        parameter.text = stream.str();
        break;
    }
    case ORM_VALUE_BOOLEAN:
        require(value.data.boolean_value <= 1, ORM_STATUS_INVALID_ARGUMENT,
                "boolean parameter must be 0 or 1");
        parameter.boolean_value = value.data.boolean_value != 0;
        parameter.text = parameter.boolean_value ? "true" : "false";
        break;
    case ORM_VALUE_TEXT:
        parameter.text = copy_string_view(value.data.text_value,
                                          "text parameter",
                                          true,
                                          maximum_size);
        break;
    case ORM_VALUE_BLOB:
        parameter.binary = copy_blob_view(value.data.blob_value,
                                          "blob parameter",
                                          true,
                                          maximum_size);
        parameter.text = "\\x" + hex_encode(parameter.binary);
        break;
    case ORM_VALUE_NULL:
        break;
    default:
        fail(ORM_STATUS_INVALID_ARGUMENT, "unknown value kind");
    }
    return parameter;
}

} // namespace

struct orm_connection {
    std::shared_ptr<connection_state> state;
};

struct orm_query {
    std::shared_ptr<connection_state> state;
    query_kind kind = query_kind::select;
    std::string table;
    std::string raw_sql;
    std::vector<std::string> columns;
    std::vector<aggregate_expression> aggregates;
    std::vector<scalar_projection> scalar_projections;
    std::vector<assignment> assignments;
    std::vector<join_clause> joins;
    std::vector<std::string> group_columns;
    condition_node where_root;
    condition_node having_root;
    std::vector<condition_node*> where_stack;
    std::vector<condition_node*> having_stack;
    std::vector<bound_parameter> raw_parameters;
    std::optional<ordering_spec> ordering;
    std::optional<std::uint64_t> limit;
    std::optional<std::uint64_t> offset;
    std::size_t parameter_count = 0;
    std::size_t parameter_bytes = 0;
    std::size_t predicate_count = 0;
    bool select_all = false;
    bool distinct = false;
};

struct orm_result {
    std::unique_ptr<result_backend> backend;
};

std::unique_ptr<condition_node> clone_condition_node(const condition_node& source)
{
    auto output = std::make_unique<condition_node>();
    output->is_group = source.is_group;
    output->is_exists = source.is_exists;
    output->is_in_subquery = source.is_in_subquery;
    output->is_scalar_subquery = source.is_scalar_subquery;
    output->negated = source.negated;
    output->logic = source.logic;
    output->value = source.value;
    output->subquery = source.subquery;
    output->subquery_column = source.subquery_column;
    output->subquery_operation = source.subquery_operation;
    output->subquery_quantifier = source.subquery_quantifier;
    output->children.reserve(source.children.size());
    for (const auto& child : source.children)
        output->children.push_back(clone_condition_node(*child));
    return output;
}

std::shared_ptr<const orm_query> clone_query(const orm_query& source)
{
    auto output = std::make_shared<orm_query>();
    output->state = source.state;
    output->kind = source.kind;
    output->table = source.table;
    output->raw_sql = source.raw_sql;
    output->columns = source.columns;
    output->aggregates = source.aggregates;
    output->scalar_projections = source.scalar_projections;
    output->assignments = source.assignments;
    output->joins = source.joins;
    output->group_columns = source.group_columns;
    output->where_root = std::move(*clone_condition_node(source.where_root));
    output->having_root = std::move(*clone_condition_node(source.having_root));
    output->where_stack.push_back(&output->where_root);
    output->having_stack.push_back(&output->having_root);
    output->raw_parameters = source.raw_parameters;
    output->ordering = source.ordering;
    output->limit = source.limit;
    output->offset = source.offset;
    output->parameter_count = source.parameter_count;
    output->parameter_bytes = source.parameter_bytes;
    output->predicate_count = source.predicate_count;
    output->select_all = source.select_all;
    output->distinct = source.distinct;
    return output;
}

std::size_t subquery_depth(const condition_node& node)
{
    std::size_t depth = 0;
    if (node.is_exists && node.subquery != nullptr) {
        depth = 1 + std::max(subquery_depth(node.subquery->where_root),
                             subquery_depth(node.subquery->having_root));
    }
    for (const auto& child : node.children)
        depth = std::max(depth, subquery_depth(*child));
    return depth;
}

bool contains_subquery(const condition_node& node)
{
    if (node.is_exists || node.is_in_subquery || node.is_scalar_subquery)
        return true;
    return std::any_of(node.children.begin(), node.children.end(),
                       [](const auto& child) { return contains_subquery(*child); });
}

bool contains_column_operand(const condition_node& node)
{
    if (!node.is_group && !node.is_exists && !node.is_in_subquery &&
        !node.is_scalar_subquery &&
        node.value.has_column_operand)
        return true;
    if ((node.is_exists || node.is_in_subquery || node.is_scalar_subquery) &&
        node.subquery != nullptr)
        return contains_column_operand(node.subquery->where_root) ||
               contains_column_operand(node.subquery->having_root);
    return std::any_of(node.children.begin(), node.children.end(),
                       [](const auto& child) {
                           return contains_column_operand(*child);
                       });
}

bool column_belongs_to(const orm_query& query, std::string_view column)
{
    if (column.find('.') == std::string_view::npos)
        return true;
    const auto belongs_to_table = [&](std::string_view table) {
        return column.size() > table.size() &&
               column.compare(0, table.size(), table) == 0 &&
               column[table.size()] == '.';
    };
    if (belongs_to_table(query.table))
        return true;
    return std::any_of(query.joins.begin(), query.joins.end(),
                       [&](const join_clause& join) {
                           return belongs_to_table(join.table);
                       });
}

bool references_known_scope(const condition_node& node,
                            const orm_query& inner,
                            const orm_query& outer)
{
    if (node.is_group)
        return std::all_of(node.children.begin(), node.children.end(),
                           [&](const auto& child) {
                               return references_known_scope(*child, inner, outer);
                           });
    if (node.is_exists || node.is_in_subquery || node.is_scalar_subquery)
        return true;
    const auto known = [&](std::string_view column) {
        return column_belongs_to(inner, column) || column_belongs_to(outer, column);
    };
    if (!known(node.value.column))
        return false;
    return !node.value.has_column_operand || known(node.value.right_column);
}

enum class transaction_state {
    active,
    committed,
    rolled_back,
    failed
};

struct orm_transaction {
    std::shared_ptr<connection_state> state;
    std::unique_ptr<transaction_backend> backend;
    transaction_state status = transaction_state::active;
};

namespace {

struct rendered_query {
    std::string sql;
    std::vector<bound_parameter> parameters;
};

std::string aggregate_sql(const aggregate_expression& aggregate)
{
    const char* function = nullptr;
    switch (aggregate.kind) {
    case ORM_AGGREGATE_COUNT_ALL:
        return "count(*)";
    case ORM_AGGREGATE_COUNT:
        function = "count";
        break;
    case ORM_AGGREGATE_SUM:
        function = "sum";
        break;
    case ORM_AGGREGATE_AVERAGE:
        function = "avg";
        break;
    case ORM_AGGREGATE_MINIMUM:
        function = "min";
        break;
    case ORM_AGGREGATE_MAXIMUM:
        function = "max";
        break;
    default:
        fail(ORM_STATUS_INVALID_ARGUMENT, "unknown aggregate kind");
    }
    return std::string(function) + "(" + aggregate.column + ")";
}

void append_parameter(std::string& sql,
                      std::vector<bound_parameter>& parameters,
                      const orm_query& query,
                      const bound_parameter& parameter,
                      bool leading_space = true)
{
    parameters.push_back(parameter);
    if (leading_space)
        append_checked(sql, " ", query.state->limits.max_query_bytes);
    append_checked(sql,
                   query.state->backend->placeholder(parameters.size()),
                   query.state->limits.max_query_bytes);
}

template <typename... Args>
void append_format(std::string& destination,
                   std::size_t maximum,
                   const char* format,
                   const Args&... args)
{
    tstr owned = tstr_format_typed_cpp(format, args...);
    if (owned == nullptr)
        fail(ORM_STATUS_OUT_OF_MEMORY, "format query fragment failed");
    const vstr view = tstr_to_v(owned);
    append_checked(destination, {view.data, view.len}, maximum);
    tstr_free(owned);
}

std::vector<scalar_token> parse_scalar_tokens(const orm_query& query,
                                             orm_scalar_expression_t expression,
                                             std::size_t& added_parameters,
                                             std::size_t& added_bytes)
{
    require(expression.tokens != nullptr && expression.token_count != 0,
            ORM_STATUS_INVALID_ARGUMENT, "scalar expression is empty");
    require(expression.token_count <= query.state->limits.max_query_bytes,
            ORM_STATUS_LIMIT_EXCEEDED,
            "scalar expression token count exceeds max_query_bytes");

    std::vector<scalar_token> tokens;
    tokens.reserve(expression.token_count);

    std::size_t depth = 0, local_parameters = 0, local_bytes = 0;
    for (std::uint32_t index = 0; index < expression.token_count; ++index) {
        const orm_scalar_token_t& input = expression.tokens[index];
        scalar_token token{};
        token.kind = input.kind;
        if (input.kind == ORM_SCALAR_COLUMN) {
            token.column = copy_identifier(input.column, "scalar expression column", true);
            ++depth;
        } else if (input.kind == ORM_SCALAR_VALUE) {
            token.parameter = encode_parameter(input.value, query.state->limits.max_parameter_bytes);
            ++depth;
            ++local_parameters;
            const std::size_t payload = parameter_payload_size(token.parameter);
            require(payload <= query.state->limits.max_parameter_bytes - local_bytes,
                    ORM_STATUS_LIMIT_EXCEEDED,
                    "parameter payload exceeds max_parameter_bytes");
            local_bytes += payload;
        } else {
            require(input.kind >= ORM_SCALAR_ADD && input.kind <= ORM_SCALAR_DIVIDE,
                    ORM_STATUS_INVALID_ARGUMENT, "unknown scalar expression operator");
            require(depth >= 2, ORM_STATUS_INVALID_ARGUMENT,
                    "scalar expression postfix stack underflow");
            --depth;
        }

        require(depth <= query.state->limits.max_condition_depth,
                ORM_STATUS_LIMIT_EXCEEDED,
                "scalar expression nesting exceeds max_condition_depth");
        tokens.push_back(std::move(token));
    }
    require(depth == 1, ORM_STATUS_INVALID_ARGUMENT,
            "scalar expression must produce exactly one value");
    require(local_parameters <= query.state->limits.max_parameters - query.parameter_count,
            ORM_STATUS_LIMIT_EXCEEDED, "parameter count exceeds max_parameters");
    require(local_bytes <= query.state->limits.max_parameter_bytes - query.parameter_bytes,
            ORM_STATUS_LIMIT_EXCEEDED, "parameter payload exceeds max_parameter_bytes");
    added_parameters = local_parameters;
    added_bytes = local_bytes;
    return tokens;
}

std::string render_select_sql(const orm_query& query,
                              std::vector<bound_parameter>& parameters);

std::string render_scalar_expression(const std::vector<scalar_token>& tokens,
                                     std::vector<bound_parameter>& parameters,
                                     const orm_query& query)
{
    std::vector<std::string> stack;
    stack.reserve(tokens.size());
    for (const scalar_token& token : tokens) {
        if (token.kind == ORM_SCALAR_COLUMN) {
            stack.push_back(token.column);
            continue;
        }
        if (token.kind == ORM_SCALAR_VALUE) {
            stack.emplace_back(query.state->backend->placeholder(parameters.size() + 1));
            parameters.push_back(token.parameter);
            continue;
        }
        require(stack.size() >= 2, ORM_STATUS_INTERNAL_ERROR,
                "invalid stored scalar expression");
        std::string right = std::move(stack.back());
        stack.pop_back();
        std::string left = std::move(stack.back());
        stack.pop_back();
        const char* op = token.kind == ORM_SCALAR_ADD ? "+"
                          : token.kind == ORM_SCALAR_SUBTRACT ? "-"
                          : token.kind == ORM_SCALAR_MULTIPLY ? "*"
                          : "/";
        std::string result = "(" + left + " " + op + " " + right + ")";
        stack.push_back(std::move(result));
    }
    require(stack.size() == 1, ORM_STATUS_INTERNAL_ERROR,
            "invalid stored scalar expression");
    return stack.back();
}

void render_condition_group(std::string& sql,
                            std::vector<bound_parameter>& parameters,
                            const orm_query& query,
                            const condition_node& group,
                            bool wrap)
{
    require(group.is_group && !group.children.empty(), ORM_STATUS_INVALID_STATE,
            "condition group is empty");
    const std::size_t maximum = query.state->limits.max_query_bytes;
    if (wrap)
        append_checked(sql, "(", maximum);
    for (std::size_t index = 0; index < group.children.size(); ++index) {
        if (index != 0)
            append_checked(sql,
                           group.logic == ORM_LOGIC_OR ? " or " : " and ",
                           maximum);
        const condition_node& node = *group.children[index];
        if (node.is_group) {
            render_condition_group(sql, parameters, query, node, true);
            continue;
        }
        if (node.is_exists) {
            require(node.subquery != nullptr, ORM_STATUS_INTERNAL_ERROR,
                    "EXISTS node has no subquery");
            if (node.negated)
                append_checked(sql, "not ", maximum);
            append_checked(sql, "exists (", maximum);
            append_checked(sql, render_select_sql(*node.subquery, parameters), maximum);
            append_checked(sql, ")", maximum);
            continue;
        }
        if (node.is_in_subquery) {
            require(node.subquery != nullptr, ORM_STATUS_INTERNAL_ERROR,
                    "IN node has no subquery");
            append_checked(sql, node.subquery_column, maximum);
            append_checked(sql, node.negated ? " not in (" : " in (", maximum);
            append_checked(sql, render_select_sql(*node.subquery, parameters), maximum);
            append_checked(sql, ")", maximum);
            continue;
        }
        if (node.is_scalar_subquery) {
            require(node.subquery != nullptr, ORM_STATUS_INTERNAL_ERROR,
                    "scalar-subquery node has no subquery");
            append_format(sql, maximum, "{} {} ", node.subquery_column,
                          node.subquery_operation);
            if (!node.subquery_quantifier.empty()) {
                append_checked(sql, node.subquery_quantifier, maximum);
                append_checked(sql, " ", maximum);
            }
            append_checked(sql, "(", maximum);
            append_checked(sql, render_select_sql(*node.subquery, parameters), maximum);
            append_checked(sql, ")", maximum);
            continue;
        }
        if (node.value.has_column_operand) {
            append_format(sql, maximum, "{} {} {}", node.value.column,
                          node.value.operation, node.value.right_column);
            continue;
        }
        if (node.value.has_parameter) {
            const std::string placeholder =
                query.state->backend->placeholder(parameters.size() + 1);
            parameters.push_back(node.value.parameter);
            append_format(sql, maximum, "{} {} {}", node.value.column,
                          node.value.operation, placeholder);
        } else {
            append_format(sql, maximum, "{} {}", node.value.column,
                          node.value.operation);
        }
    }
    if (wrap)
        append_checked(sql, ")", maximum);
}

void render_filters(std::string& sql,
                    std::vector<bound_parameter>& parameters,
                    const orm_query& query,
                    const condition_node& root,
                    std::string_view prefix)
{
    if (root.children.empty())
        return;
    append_checked(sql, prefix, query.state->limits.max_query_bytes);
    render_condition_group(sql, parameters, query, root, false);
}

constexpr const char select_mustache_template[] =
    "select {{{distinct}}}{{{select_list}}} from {{{table}}}{{#joins}}{{{.}}}{{/joins}}"
    "{{#where}} where {{{where}}}{{/where}}"
    "{{#group_by}} group by {{{group_by}}}{{/group_by}}"
    "{{#having}} having {{{having}}}{{/having}}"
    "{{#order_by}}{{{order_by}}}{{/order_by}}"
    "{{{pagination}}}";

constexpr const char insert_mustache_template[] =
    "insert into {{{table}}} ({{{columns}}}) values ({{{values}}})";

constexpr const char update_mustache_template[] =
    "update {{{table}}} set {{{assignments}}}"
    "{{#where}} where {{{where}}}{{/where}}";

constexpr const char delete_mustache_template[] =
    "delete from {{{table}}}{{#where}} where {{{where}}}{{/where}}";

const MUSTACHE_TEMPLATE* compiled_select_template()
{
    static const std::unique_ptr<MUSTACHE_TEMPLATE, void (*)(MUSTACHE_TEMPLATE*)>
        compiled(mustache_compile(select_mustache_template,
                                  sizeof(select_mustache_template) - 1,
                                  nullptr, nullptr, 0),
                 &mustache_release);
    require(compiled != nullptr, ORM_STATUS_INTERNAL_ERROR,
            "failed to compile select mustache template");
    return compiled.get();
}

const MUSTACHE_TEMPLATE* compiled_insert_template()
{
    static const std::unique_ptr<MUSTACHE_TEMPLATE, void (*)(MUSTACHE_TEMPLATE*)>
        compiled(mustache_compile(insert_mustache_template,
                                  sizeof(insert_mustache_template) - 1,
                                  nullptr, nullptr, 0),
                 &mustache_release);
    require(compiled != nullptr, ORM_STATUS_INTERNAL_ERROR,
            "failed to compile insert mustache template");
    return compiled.get();
}

const MUSTACHE_TEMPLATE* compiled_update_template()
{
    static const std::unique_ptr<MUSTACHE_TEMPLATE, void (*)(MUSTACHE_TEMPLATE*)>
        compiled(mustache_compile(update_mustache_template,
                                  sizeof(update_mustache_template) - 1,
                                  nullptr, nullptr, 0),
                 &mustache_release);
    require(compiled != nullptr, ORM_STATUS_INTERNAL_ERROR,
            "failed to compile update mustache template");
    return compiled.get();
}

const MUSTACHE_TEMPLATE* compiled_delete_template()
{
    static const std::unique_ptr<MUSTACHE_TEMPLATE, void (*)(MUSTACHE_TEMPLATE*)>
        compiled(mustache_compile(delete_mustache_template,
                                  sizeof(delete_mustache_template) - 1,
                                  nullptr, nullptr, 0),
                 &mustache_release);
    require(compiled != nullptr, ORM_STATUS_INTERNAL_ERROR,
            "failed to compile delete mustache template");
    return compiled.get();
}

using mustache_detail::node;

std::string render_select_sql(const orm_query& query,
                              std::vector<bound_parameter>& parameters)
{
    const std::size_t maximum = query.state->limits.max_query_bytes;
    std::string sql;
    sql.reserve(std::min<std::size_t>(maximum, 1024));

    std::string select_list;
    if (query.select_all) {
        select_list = "*";
    } else {
        require(!query.columns.empty() || !query.aggregates.empty() ||
                    !query.scalar_projections.empty(),
                ORM_STATUS_INVALID_STATE,
                "query has no selected columns or aggregates");
        bool needs_separator = false;
        for (const std::string& column : query.columns) {
            if (needs_separator)
                select_list += ", ";
            select_list += column;
            needs_separator = true;
        }
        for (const aggregate_expression& aggregate : query.aggregates) {
            if (needs_separator)
                select_list += ", ";
            select_list += aggregate_sql(aggregate);
            if (!aggregate.alias.empty()) {
                select_list += " as ";
                select_list += aggregate.alias;
            }
            needs_separator = true;
        }
        for (const scalar_projection& projection : query.scalar_projections) {
            if (needs_separator)
                select_list += ", ";
            select_list += render_scalar_expression(projection.tokens, parameters, query);
            if (!projection.alias.empty())
                select_list += " as " + projection.alias;
            needs_separator = true;
        }
    }

    std::vector<std::string> join_items;
    for (const join_clause& join : query.joins) {
        std::string clause = join.kind == ORM_JOIN_LEFT ? " left join " : " inner join ";
        clause += join.table;
        clause += " on ";
        clause += join.left_column;
        clause += " ";
        clause += join.operation;
        clause += " ";
        clause += join.right_column;
        join_items.push_back(std::move(clause));
    }

    std::string where_sql;
    render_filters(where_sql, parameters, query, query.where_root, "");

    std::string group_by;
    for (std::size_t index = 0; index < query.group_columns.size(); ++index) {
        if (index != 0)
            group_by += ", ";
        group_by += query.group_columns[index];
    }

    std::string having_sql;
    render_filters(having_sql, parameters, query, query.having_root, "");

    std::string order_by;
    if (query.ordering) {
        order_by = " order by ";
        if (query.ordering->is_expression)
            order_by += render_scalar_expression(query.ordering->tokens, parameters, query);
        else
            order_by += query.ordering->column;
        order_by += query.ordering->order == ORM_ORDER_DESCENDING ? " desc" : " asc";
    }

    const std::string pagination =
        query.state->backend->pagination(query.limit, query.offset);

    node root;
    mustache_detail::add_string(root.children, "distinct",
                                query.distinct ? "distinct " : "");
    mustache_detail::add_string(root.children, "select_list", std::move(select_list));
    mustache_detail::add_string(root.children, "table", query.table);
    if (!join_items.empty())
        mustache_detail::add_list(root.children, "joins", std::move(join_items));
    if (!where_sql.empty())
        mustache_detail::add_string(root.children, "where", std::move(where_sql));
    if (!group_by.empty())
        mustache_detail::add_string(root.children, "group_by", std::move(group_by));
    if (!having_sql.empty())
        mustache_detail::add_string(root.children, "having", std::move(having_sql));
    if (!order_by.empty())
        mustache_detail::add_string(root.children, "order_by", std::move(order_by));
    if (!pagination.empty())
        mustache_detail::add_string(root.children, "pagination", pagination);

    mustache_detail::render(compiled_select_template(), root, sql, maximum);
    return sql;
}

rendered_query render_select_mustache(const orm_query& query)
{
    rendered_query rendered;
    rendered.parameters.reserve(query.parameter_count);
    rendered.sql = render_select_sql(query, rendered.parameters);
    return rendered;
}

rendered_query render_insert_mustache(const orm_query& query)
{
    const std::size_t maximum = query.state->limits.max_query_bytes;
    require(!query.assignments.empty(), ORM_STATUS_INVALID_STATE,
            "insert query has no values");

    rendered_query rendered;
    rendered.sql.reserve(std::min<std::size_t>(maximum, 1024));
    rendered.parameters.reserve(query.parameter_count);

    std::string columns;
    std::string values;
    for (std::size_t index = 0; index < query.assignments.size(); ++index) {
        if (index != 0) {
            columns += ", ";
            values += ", ";
        }
        columns += query.assignments[index].column;
        if (query.assignments[index].has_parameter)
            append_parameter(values, rendered.parameters, query,
                             query.assignments[index].parameter, false);
        else
            values += "null";
    }

    node root;
    mustache_detail::add_string(root.children, "table", query.table);
    mustache_detail::add_string(root.children, "columns", std::move(columns));
    mustache_detail::add_string(root.children, "values", std::move(values));

    mustache_detail::render(compiled_insert_template(), root, rendered.sql, maximum);
    return rendered;
}

rendered_query render_update_mustache(const orm_query& query)
{
    const std::size_t maximum = query.state->limits.max_query_bytes;
    require(!query.assignments.empty(), ORM_STATUS_INVALID_STATE,
            "update query has no assignments");

    rendered_query rendered;
    rendered.sql.reserve(std::min<std::size_t>(maximum, 1024));
    rendered.parameters.reserve(query.parameter_count);

    std::string assignments;
    for (std::size_t index = 0; index < query.assignments.size(); ++index) {
        if (index != 0)
            assignments += ", ";
        assignments += query.assignments[index].column;
        assignments += " =";
        if (query.assignments[index].has_parameter)
            append_parameter(assignments, rendered.parameters, query,
                             query.assignments[index].parameter);
        else
            assignments += " null";
    }

    std::string where_sql;
    render_filters(where_sql, rendered.parameters, query, query.where_root, "");

    node root;
    mustache_detail::add_string(root.children, "table", query.table);
    mustache_detail::add_string(root.children, "assignments", std::move(assignments));
    if (!where_sql.empty())
        mustache_detail::add_string(root.children, "where", std::move(where_sql));

    mustache_detail::render(compiled_update_template(), root, rendered.sql, maximum);
    return rendered;
}

rendered_query render_delete_mustache(const orm_query& query)
{
    const std::size_t maximum = query.state->limits.max_query_bytes;
    rendered_query rendered;
    rendered.sql.reserve(std::min<std::size_t>(maximum, 1024));
    rendered.parameters.reserve(query.parameter_count);

    std::string where_sql;
    render_filters(where_sql, rendered.parameters, query, query.where_root, "");

    node root;
    mustache_detail::add_string(root.children, "table", query.table);
    if (!where_sql.empty())
        mustache_detail::add_string(root.children, "where", std::move(where_sql));

    mustache_detail::render(compiled_delete_template(), root, rendered.sql, maximum);
    return rendered;
}

rendered_query build_query(const orm_query& query)
{
    const std::size_t maximum = query.state->limits.max_query_bytes;
    require(query.where_stack.size() == 1 && query.having_stack.size() == 1,
            ORM_STATUS_INVALID_STATE,
            "query has an unclosed condition group");
    rendered_query rendered;
    rendered.sql.reserve(std::min<std::size_t>(maximum, 1024));
    rendered.parameters.reserve(query.parameter_count);

    if (query.kind == query_kind::raw) {
        rendered.sql = query.raw_sql;
        rendered.parameters = query.raw_parameters;
        return rendered;
    }

    if (query.kind == query_kind::insert)
        return render_insert_mustache(query);

    if (query.kind == query_kind::update)
        return render_update_mustache(query);

    if (query.kind == query_kind::remove)
        return render_delete_mustache(query);

    return render_select_mustache(query);
}

bool supports_where(query_kind kind) noexcept
{
    return kind == query_kind::select || kind == query_kind::update ||
           kind == query_kind::remove;
}

template<typename Executor>
std::unique_ptr<result_backend> execute_query(orm_query& query,
                                              Executor& executor)
{
    if (query.state->backend->model() ==
        database_backend::execution_model::native_plan) {
        require(!query.ordering || !query.ordering->is_expression,
                ORM_STATUS_UNSUPPORTED,
                "ORDER BY expression is not supported by native-plan backends");
        require(query.scalar_projections.empty(), ORM_STATUS_UNSUPPORTED,
                "scalar projection expressions are not supported by this backend");
        require(!contains_subquery(query.where_root) &&
                    !contains_subquery(query.having_root),
                ORM_STATUS_UNSUPPORTED,
                "subqueries are not supported by this backend");
        require(!contains_column_operand(query.where_root) &&
                    !contains_column_operand(query.having_root),
                ORM_STATUS_UNSUPPORTED,
                "column comparison predicates are not supported by this backend");
        const query_plan plan{
            query.kind, query.table, query.raw_sql, query.columns,
            query.aggregates, query.assignments, query.joins,
            query.group_columns, query.where_root, query.having_root,
            query.raw_parameters, query.ordering, query.limit,
            query.offset, query.parameter_count, query.select_all,
            query.distinct};
        return executor.execute_plan(plan, query.state->limits);
    }

    rendered_query rendered = build_query(query);
    require(rendered.parameters.size() == query.parameter_count,
            ORM_STATUS_INTERNAL_ERROR,
            "rendered parameter count does not match query state");
    const bool structured_dml = query.kind == query_kind::insert ||
                                query.kind == query_kind::update ||
                                query.kind == query_kind::remove;
    return executor.execute_sql(rendered.sql, rendered.parameters,
                                structured_dml, query.state->limits);
}

bool valid_isolation(orm_isolation_t isolation) noexcept
{
    return isolation >= ORM_ISOLATION_READ_UNCOMMITTED &&
           isolation <= ORM_ISOLATION_SERIALIZABLE;
}

void require_active(const orm_transaction_t* transaction)
{
    require(transaction != nullptr && transaction->backend != nullptr,
            ORM_STATUS_INVALID_ARGUMENT,
            "transaction handle is null");
    require(transaction->status == transaction_state::active,
            ORM_STATUS_INVALID_STATE,
            "transaction is no longer active");
}

void require_query_kind(const orm_query& query,
                        query_kind expected,
                        const char* message)
{
    require(query.kind == expected, ORM_STATUS_INVALID_STATE, message);
}

std::unique_ptr<orm_query> make_structured_query(orm_connection_t* connection,
                                                 orm_string_view_t table,
                                                 query_kind kind)
{
    require(connection != nullptr && connection->state != nullptr,
            ORM_STATUS_INVALID_ARGUMENT, "connection handle is null");
    auto query = std::make_unique<orm_query>();
    query->state = connection->state;
    query->kind = kind;
    query->table = copy_identifier(table, "table name", true);
    query->where_root.logic = ORM_LOGIC_AND;
    query->having_root.logic = ORM_LOGIC_AND;
    query->where_stack.push_back(&query->where_root);
    query->having_stack.push_back(&query->having_root);
    return query;
}

aggregate_expression make_aggregate(orm_aggregate_t kind,
                                    orm_string_view_t column,
                                    orm_string_view_t alias)
{
    aggregate_expression aggregate{kind, {}, {}};
    if (kind == ORM_AGGREGATE_COUNT_ALL) {
        require(column.len == 0, ORM_STATUS_INVALID_ARGUMENT,
                "count-all aggregate does not accept a column");
    } else {
        require(kind >= ORM_AGGREGATE_COUNT && kind <= ORM_AGGREGATE_MAXIMUM,
                ORM_STATUS_INVALID_ARGUMENT, "unknown aggregate kind");
        aggregate.column = copy_identifier(column, "aggregate column", true);
    }
    aggregate.alias = copy_string_view(alias, "aggregate alias", true,
                                       max_identifier_segment_bytes);
    if (!aggregate.alias.empty()) {
        const orm_string_view_t view{aggregate.alias.data(), aggregate.alias.size()};
        aggregate.alias = copy_identifier(view, "aggregate alias", false);
    }
    return aggregate;
}

void charge_parameter(orm_query& query, const bound_parameter& parameter)
{
    require(query.parameter_count < query.state->limits.max_parameters,
            ORM_STATUS_LIMIT_EXCEEDED, "parameter count exceeds max_parameters");
    const std::size_t payload = parameter_payload_size(parameter);
    require(payload <= query.state->limits.max_parameter_bytes - query.parameter_bytes,
            ORM_STATUS_LIMIT_EXCEEDED,
            "parameter payload exceeds max_parameter_bytes");
    ++query.parameter_count;
    query.parameter_bytes += payload;
}

predicate make_predicate(orm_query& query,
                         std::string expression,
                         orm_compare_t comparison,
                         orm_value_t value)
{
    require(query.predicate_count < query.state->limits.max_predicates,
            ORM_STATUS_LIMIT_EXCEEDED, "predicate count exceeds max_predicates");
    predicate next;
    next.column = std::move(expression);
    next.operation = comparison_sql(comparison);
    next.comparison = comparison;
    if (value.kind == ORM_VALUE_NULL) {
        require(value.reserved == 0, ORM_STATUS_ABI_MISMATCH,
                "orm_value_t.reserved must be zero");
        require(comparison == ORM_COMPARE_EQUAL || comparison == ORM_COMPARE_NOT_EQUAL,
                ORM_STATUS_INVALID_ARGUMENT,
                "SQL NULL supports only equal and not-equal comparisons");
        next.operation = comparison == ORM_COMPARE_EQUAL ? "is null" : "is not null";
        next.has_parameter = false;
        return next;
    }
    require(value.kind != ORM_VALUE_BLOB, ORM_STATUS_UNSUPPORTED,
            "BLOB predicates are not supported by the portable ORM");
    require((comparison != ORM_COMPARE_LIKE && comparison != ORM_COMPARE_NOT_LIKE) ||
                value.kind == ORM_VALUE_TEXT,
            ORM_STATUS_TYPE_ERROR, "LIKE comparisons require a text value");
    next.parameter = encode_parameter(value, query.state->limits.max_parameter_bytes);
    require(query.parameter_count < query.state->limits.max_parameters,
            ORM_STATUS_LIMIT_EXCEEDED, "parameter count exceeds max_parameters");
    const std::size_t payload = parameter_payload_size(next.parameter);
    require(payload <= query.state->limits.max_parameter_bytes - query.parameter_bytes,
            ORM_STATUS_LIMIT_EXCEEDED,
            "parameter payload exceeds max_parameter_bytes");
    return next;
}

predicate make_column_predicate(orm_query& query,
                                std::string left_column,
                                orm_compare_t comparison,
                                std::string right_column)
{
    require(query.predicate_count < query.state->limits.max_predicates,
            ORM_STATUS_LIMIT_EXCEEDED, "predicate count exceeds max_predicates");
    predicate next;
    next.column = std::move(left_column);
    next.operation = comparison_sql(comparison);
    next.right_column = std::move(right_column);
    next.comparison = comparison;
    next.has_parameter = false;
    next.has_column_operand = true;
    return next;
}

void append_predicate(orm_query& query,
                      std::vector<condition_node*>& stack,
                      predicate next)
{
    condition_node* group = stack.back();
    group->children.reserve(group->children.size() + 1);
    auto node = std::make_unique<condition_node>();
    node->is_group = false;
    node->value = std::move(next);
    const bool has_parameter = node->value.has_parameter;
    const std::size_t parameter_size = parameter_payload_size(node->value.parameter);
    group->children.push_back(std::move(node));
    ++query.predicate_count;
    if (has_parameter) {
        ++query.parameter_count;
        query.parameter_bytes += parameter_size;
    }
}

void append_subquery_predicate(orm_query& query,
                               const orm_query& subquery,
                               bool negated,
                               std::string in_column,
                               std::string scalar_operation = {},
                               std::string quantifier = {})
{
    require(references_known_scope(subquery.where_root, subquery, query) &&
                references_known_scope(subquery.having_root, subquery, query),
            ORM_STATUS_INVALID_ARGUMENT,
            "subquery predicate references a table outside its inner and outer query scopes");
    require(query.predicate_count < query.state->limits.max_predicates,
            ORM_STATUS_LIMIT_EXCEEDED, "predicate count exceeds max_predicates");
    require(subquery.predicate_count <=
                query.state->limits.max_predicates - query.predicate_count - 1,
            ORM_STATUS_LIMIT_EXCEEDED,
            "subquery predicates exceed max_predicates");
    require(subquery.parameter_count <=
                query.state->limits.max_parameters - query.parameter_count,
            ORM_STATUS_LIMIT_EXCEEDED,
            "subquery parameters exceed max_parameters");
    require(subquery.parameter_bytes <=
                query.state->limits.max_parameter_bytes - query.parameter_bytes,
            ORM_STATUS_LIMIT_EXCEEDED,
            "subquery parameter payload exceeds max_parameter_bytes");
    require(1 + std::max(subquery_depth(subquery.where_root),
                         subquery_depth(subquery.having_root)) <=
                query.state->limits.max_condition_depth,
            ORM_STATUS_LIMIT_EXCEEDED,
            "subquery nesting exceeds max_condition_depth");

    auto node = std::make_unique<condition_node>();
    node->is_group = false;
    node->is_exists = in_column.empty();
    node->is_scalar_subquery = !scalar_operation.empty();
    node->is_in_subquery = !node->is_scalar_subquery && !in_column.empty();
    node->negated = negated;
    node->subquery = clone_query(subquery);
    node->subquery_column = std::move(in_column);
    node->subquery_operation = std::move(scalar_operation);
    node->subquery_quantifier = std::move(quantifier);
    query.where_stack.back()->children.push_back(std::move(node));
    query.predicate_count += 1 + subquery.predicate_count;
    query.parameter_count += subquery.parameter_count;
    query.parameter_bytes += subquery.parameter_bytes;
}

void append_exists(orm_query& query,
                   const orm_query& subquery,
                   bool negated)
{
    append_subquery_predicate(query, subquery, negated, {});
}

void append_scalar_subquery(orm_query& query,
                            orm_string_view_t column,
                            orm_compare_t comparison,
                            const orm_query& subquery,
                            std::string quantifier = {})
{
    require(supports_where(query.kind), ORM_STATUS_INVALID_STATE,
            "scalar subquery requires a select, update, or delete query");
    require_query_kind(subquery, query_kind::select,
                       "scalar comparison requires a select subquery");
    require(query.state == subquery.state, ORM_STATUS_INVALID_ARGUMENT,
            "outer query and subquery must share one connection");
    require(subquery.where_stack.size() == 1 && subquery.having_stack.size() == 1,
            ORM_STATUS_INVALID_STATE, "subquery has an unclosed condition group");
    require(!subquery.select_all &&
                subquery.columns.size() + subquery.aggregates.size() +
                    subquery.scalar_projections.size() == 1,
            ORM_STATUS_INVALID_STATE,
            "scalar subquery must project exactly one expression");
    append_subquery_predicate(
        query, subquery, false,
        copy_identifier(column, "scalar subquery column", true),
        comparison_sql(comparison), std::move(quantifier));
}

void begin_group(orm_query& query,
                 std::vector<condition_node*>& stack,
                 orm_logic_t logic)
{
    require(logic == ORM_LOGIC_AND || logic == ORM_LOGIC_OR,
            ORM_STATUS_INVALID_ARGUMENT, "unknown condition-group logic");
    require(stack.size() < query.state->limits.max_condition_depth,
            ORM_STATUS_LIMIT_EXCEEDED,
            "condition nesting exceeds max_condition_depth");
    condition_node* parent = stack.back();
    parent->children.reserve(parent->children.size() + 1);
    stack.reserve(stack.size() + 1);
    auto group = std::make_unique<condition_node>();
    group->logic = logic;
    condition_node* child = group.get();
    parent->children.push_back(std::move(group));
    stack.push_back(child);
}

void end_group(std::vector<condition_node*>& stack)
{
    require(stack.size() > 1, ORM_STATUS_INVALID_STATE,
            "no open condition group to close");
    require(!stack.back()->children.empty(), ORM_STATUS_INVALID_STATE,
            "condition group is empty");
    stack.pop_back();
}

vstr get_cell(const orm_result& result,
                std::uint64_t row,
                std::uint64_t column,
                bool reject_null)
{
    require(result.backend != nullptr, ORM_STATUS_INVALID_STATE,
            "result backend is null");
    require(row < result.backend->rows() && column < result.backend->columns(),
            ORM_STATUS_OUT_OF_RANGE,
            "result row or column is out of range");
    if (result.backend->is_null(row, column)) {
        if (reject_null)
            fail(ORM_STATUS_NULL_VALUE, "result cell is SQL NULL");
        return vstr_from_buf(nullptr, 0);
    }
    return result.backend->cell(row, column);
}

template<typename Integer>
Integer parse_integer(vstr cell)
{
    Integer value{};
    const auto parsed = std::from_chars(cell.data, cell.data + cell.len, value);
    if (parsed.ec != std::errc{} || parsed.ptr != cell.data + cell.len)
        fail(ORM_STATUS_TYPE_ERROR, "result cell is not a valid integer");
    return value;
}

} // namespace

namespace orm_c_detail {

orm_status_t register_database_driver(std::string_view name,
                                      database_backend_factory factory,
                                      orm_error_t* error) noexcept
{
    return api_call(error, [&] {
        require(register_database_driver_impl(name, factory),
                ORM_STATUS_INVALID_STATE,
                "database driver registration failed");
    });
}

} // namespace orm_c_detail

extern "C" {

uint32_t ORM_C_CALL orm_c_abi_version(void)
{
    return ORM_C_ABI_VERSION;
}

const char* ORM_C_CALL orm_status_message(orm_status_t status)
{
    switch (status) {
    case ORM_STATUS_OK:
        return "ok";
    case ORM_STATUS_INVALID_ARGUMENT:
        return "invalid argument";
    case ORM_STATUS_ABI_MISMATCH:
        return "ABI mismatch";
    case ORM_STATUS_OUT_OF_MEMORY:
        return "out of memory";
    case ORM_STATUS_CONNECTION_ERROR:
        return "connection error";
    case ORM_STATUS_SQL_ERROR:
        return "SQL error";
    case ORM_STATUS_TYPE_ERROR:
        return "type error";
    case ORM_STATUS_OUT_OF_RANGE:
        return "out of range";
    case ORM_STATUS_LIMIT_EXCEEDED:
        return "limit exceeded";
    case ORM_STATUS_INVALID_STATE:
        return "invalid state";
    case ORM_STATUS_NULL_VALUE:
        return "SQL null";
    case ORM_STATUS_INTERNAL_ERROR:
        return "internal error";
    case ORM_STATUS_BUSY:
        return "database busy";
    case ORM_STATUS_UNSUPPORTED:
        return "unsupported operation";
    case ORM_STATUS_DATASTORE_ERROR:
        return "data store error";
    default:
        return "unknown status";
    }
}

void ORM_C_CALL orm_error_init(orm_error_t* error)
{
    if (error == nullptr)
        return;
    error->struct_size = static_cast<uint32_t>(legacy_error_size);
    write_error(error, ORM_STATUS_OK, "");
}

void ORM_C_CALL orm_error_init_s(orm_error_t* error, uint32_t error_size)
{
    if (error == nullptr || error_size < sizeof(error->struct_size))
        return;
    error->struct_size = error_size;
    write_error(error, ORM_STATUS_OK, "");
}

const char* ORM_C_CALL orm_error_backend_code(const orm_error_t* error)
{
    if (error == nullptr ||
        error->struct_size <= offsetof(orm_error_t, backend_code))
        return "";
    return error->backend_code;
}

void ORM_C_CALL orm_config(orm_config_t* config)
{
    if (config == nullptr)
        return;
    std::memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->abi_version = ORM_C_ABI_VERSION;
    config->max_parameters = ORM_C_DEFAULT_MAX_PARAMETERS;
    config->max_columns = ORM_C_DEFAULT_MAX_COLUMNS;
    config->max_predicates = ORM_C_DEFAULT_MAX_PREDICATES;
    config->max_joins = ORM_C_DEFAULT_MAX_JOINS;
    config->max_group_columns = ORM_C_DEFAULT_MAX_GROUP_COLUMNS;
    config->max_assignments = ORM_C_DEFAULT_MAX_ASSIGNMENTS;
    config->max_condition_depth = ORM_C_DEFAULT_MAX_CONDITION_DEPTH;
    config->max_query_bytes = ORM_C_DEFAULT_MAX_QUERY_BYTES;
    config->max_parameter_bytes = ORM_C_DEFAULT_MAX_PARAMETER_BYTES;
    config->max_result_rows = ORM_C_DEFAULT_MAX_RESULT_ROWS;
    config->max_result_bytes = ORM_C_DEFAULT_MAX_RESULT_BYTES;
}

orm_status_t ORM_C_CALL
orm_connect(const orm_config_t* config,
            orm_connection_t** out_connection,
            orm_error_t* error)
{
    if (out_connection != nullptr)
        *out_connection = nullptr;
    return api_call(error, [&] {
        require(config != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "config pointer is null");
        require(out_connection != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "out_connection pointer is null");
        const connection_limits limits = validate_config(*config);
        const std::string driver = copy_identifier(config->driver, "driver", false);

        std::vector<std::string> keywords;
        std::vector<std::string> values;
        keywords.reserve(config->option_count);
        values.reserve(config->option_count);
        std::unordered_set<std::string> unique_keywords;
        unique_keywords.reserve(config->option_count);
        std::size_t total_option_bytes = 0;

        for (std::uint32_t index = 0; index < config->option_count; ++index) {
            std::string keyword =
                copy_identifier(config->options[index].keyword, "connection option keyword", false);
            std::string value = copy_string_view(config->options[index].value,
                                                 "connection option value",
                                                 true,
                                                 max_connection_option_value_bytes);
            checked_add(total_option_bytes, keyword.size(), max_connection_option_total_bytes,
                        "connection options exceed their total byte limit");
            checked_add(total_option_bytes, value.size(), max_connection_option_total_bytes,
                        "connection options exceed their total byte limit");
            if (!unique_keywords.insert(keyword).second)
                fail(ORM_STATUS_INVALID_ARGUMENT,
                     "duplicate connection option keyword");
            keywords.push_back(std::move(keyword));
            values.push_back(std::move(value));
        }

        auto state = std::make_shared<connection_state>();
        if (driver == "postgresql") {
            require(!keywords.empty(), ORM_STATUS_INVALID_ARGUMENT,
                    "the selected driver requires at least one option");
            const auto factory = find_database_driver(driver);
            require(factory != nullptr, ORM_STATUS_UNSUPPORTED,
                    "postgresql ORM driver is not registered");
            state->backend = factory(keywords, values, limits);
        } else if (driver == "sqlite") {
#if defined(ORM_WITH_SQLITE)
            sqlite_settings settings = parse_sqlite_settings(keywords, values);
            state->backend = orm_c_detail::make_sqlite_backend(
                std::move(settings.filename),
                settings.open_mode,
                settings.busy_timeout_ms,
                limits);
#else
            fail(ORM_STATUS_UNSUPPORTED,
                 "sqlite ORM backend was not enabled at build time");
#endif
        } else if (driver == "redis") {
#if defined(ORM_WITH_REDIS)
            state->backend = orm_c_detail::make_redis_backend(keywords, values, limits);
#else
            fail(ORM_STATUS_UNSUPPORTED,
                 "redis ORM backend was not enabled at build time");
#endif
        } else if (driver == "tidesdb") {
#if defined(ORM_WITH_TIDESDB)
            state->backend = orm_c_detail::make_tidesdb_backend(keywords, values, limits);
#else
            fail(ORM_STATUS_UNSUPPORTED,
                 "tidesdb ORM backend was not enabled at build time");
#endif
        } else if (driver == "mongo") {
#if defined(ORM_WITH_MONGO)
            state->backend = orm_c_detail::make_mongo_backend(keywords, values, limits);
#else
            fail(ORM_STATUS_UNSUPPORTED,
                 "mongo ORM backend was not enabled at build time");
#endif
        } else {
            fail(ORM_STATUS_INVALID_ARGUMENT, "unsupported ORM driver: " + driver);
        }
        state->limits = limits;
        auto handle = std::make_unique<orm_connection>();
        handle->state = std::move(state);
        *out_connection = handle.release();
    });
}

void ORM_C_CALL orm_disconnect(orm_connection_t* connection)
{
    delete connection;
}

orm_status_t ORM_C_CALL
orm_transaction_begin(orm_connection_t* connection,
                      orm_isolation_t isolation,
                      orm_transaction_t** out_transaction,
                      orm_error_t* error)
{
    if (out_transaction != nullptr)
        *out_transaction = nullptr;
    return api_call(error, [&] {
        require(connection != nullptr && connection->state != nullptr &&
                    connection->state->backend != nullptr,
                ORM_STATUS_INVALID_ARGUMENT,
                "connection handle is null");
        require(out_transaction != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "out_transaction pointer is null");
        require(valid_isolation(isolation), ORM_STATUS_INVALID_ARGUMENT,
                "unknown transaction isolation level");

        auto handle = std::make_unique<orm_transaction>();
        handle->state = connection->state;
        handle->backend = connection->state->backend->begin_transaction(isolation);
        require(handle->backend != nullptr, ORM_STATUS_INTERNAL_ERROR,
                "database backend returned a null transaction");
        *out_transaction = handle.release();
    });
}

orm_status_t ORM_C_CALL
orm_transaction_commit(orm_transaction_t* transaction,
                       orm_error_t* error)
{
    return api_call(error, [&] {
        require_active(transaction);
        try {
            transaction->backend->commit();
            transaction->status = transaction_state::committed;
        } catch (...) {
            transaction->status = transaction_state::failed;
            throw;
        }
    });
}

orm_status_t ORM_C_CALL
orm_transaction_rollback(orm_transaction_t* transaction,
                         orm_error_t* error)
{
    return api_call(error, [&] {
        require_active(transaction);
        try {
            transaction->backend->rollback();
            transaction->status = transaction_state::rolled_back;
        } catch (...) {
            transaction->status = transaction_state::failed;
            throw;
        }
    });
}

orm_status_t ORM_C_CALL
orm_transaction_savepoint(orm_transaction_t* transaction,
                          orm_string_view_t name,
                          orm_error_t* error)
{
    return api_call(error, [&] {
        require_active(transaction);
        transaction->backend->savepoint(
            copy_identifier(name, "transaction savepoint", false));
    });
}

orm_status_t ORM_C_CALL
orm_transaction_rollback_to_savepoint(orm_transaction_t* transaction,
                                      orm_string_view_t name,
                                      orm_error_t* error)
{
    return api_call(error, [&] {
        require_active(transaction);
        transaction->backend->rollback_to_savepoint(
            copy_identifier(name, "transaction savepoint", false));
    });
}

orm_status_t ORM_C_CALL
orm_transaction_release_savepoint(orm_transaction_t* transaction,
                                  orm_string_view_t name,
                                  orm_error_t* error)
{
    return api_call(error, [&] {
        require_active(transaction);
        transaction->backend->release_savepoint(
            copy_identifier(name, "transaction savepoint", false));
    });
}

void ORM_C_CALL orm_transaction_destroy(orm_transaction_t* transaction)
{
    if (transaction == nullptr)
        return;
    if (transaction->status == transaction_state::active &&
        transaction->backend != nullptr) {
        try {
            transaction->backend->rollback();
        } catch (...) {
        }
    }
    delete transaction;
}

orm_status_t ORM_C_CALL
orm_query_create(orm_connection_t* connection,
                 orm_string_view_t table,
                 orm_query_t** out_query,
                 orm_error_t* error)
{
    if (out_query != nullptr)
        *out_query = nullptr;
    return api_call(error, [&] {
        require(out_query != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "out_query pointer is null");
        *out_query = make_structured_query(connection, table, query_kind::select).release();
    });
}

#define ORM_DEFINE_DML_CREATE(function_name, kind_value)                         \
    orm_status_t ORM_C_CALL function_name(orm_connection_t* connection,         \
                                           orm_string_view_t table,              \
                                           orm_query_t** out_query,              \
                                           orm_error_t* error)                   \
    {                                                                            \
        if (out_query != nullptr)                                                 \
            *out_query = nullptr;                                                 \
        return api_call(error, [&] {                                              \
            require(out_query != nullptr, ORM_STATUS_INVALID_ARGUMENT,           \
                    "out_query pointer is null");                               \
            *out_query = make_structured_query(connection, table, kind_value)     \
                             .release();                                          \
        });                                                                       \
    }

ORM_DEFINE_DML_CREATE(orm_insert, query_kind::insert)
ORM_DEFINE_DML_CREATE(orm_update, query_kind::update)
ORM_DEFINE_DML_CREATE(orm_delete, query_kind::remove)

#undef ORM_DEFINE_DML_CREATE

orm_status_t ORM_C_CALL
orm_raw(orm_connection_t* connection,
        orm_string_view_t sql,
        orm_query_t** out_query,
        orm_error_t* error)
{
    if (out_query != nullptr)
        *out_query = nullptr;
    return api_call(error, [&] {
        require(connection != nullptr && connection->state != nullptr,
                ORM_STATUS_INVALID_ARGUMENT, "connection handle is null");
        require(out_query != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "out_query pointer is null");
        auto query = std::make_unique<orm_query>();
        query->state = connection->state;
        query->kind = query_kind::raw;
        query->raw_sql = copy_string_view(sql, "raw SQL", false,
                                          query->state->limits.max_query_bytes);
        query->where_stack.push_back(&query->where_root);
        query->having_stack.push_back(&query->having_root);
        *out_query = query.release();
    });
}

void ORM_C_CALL orm_query_destroy(orm_query_t* query)
{
    delete query;
}

orm_status_t ORM_C_CALL
orm_query_select_all(orm_query_t* query, orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require_query_kind(*query, query_kind::select,
                           "select_all requires a select query");
        query->columns.clear();
        query->aggregates.clear();
        query->select_all = true;
    });
}

orm_status_t ORM_C_CALL
orm_query_set_distinct(orm_query_t* query, int enabled, orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "query handle is null");
        require_query_kind(*query, query_kind::select,
                           "distinct requires a select query");
        query->distinct = enabled != 0;
    });
}

orm_status_t ORM_C_CALL
orm_query_add_column(orm_query_t* query,
                        orm_string_view_t column,
                        orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require_query_kind(*query, query_kind::select,
                           "add_column requires a select query");
        require(!query->select_all, ORM_STATUS_INVALID_STATE,
                "cannot add a column after select_all");
        require(query->columns.size() + query->aggregates.size() +
                    query->scalar_projections.size() <
                    query->state->limits.max_columns,
                ORM_STATUS_LIMIT_EXCEEDED,
                "selected column count exceeds max_columns");
        std::string copied = copy_identifier(column, "column name", true);
        query->columns.push_back(std::move(copied));
    });
}

orm_status_t ORM_C_CALL
orm_query_add_aggregate(orm_query_t* query,
                        orm_aggregate_t aggregate,
                        orm_string_view_t column,
                        orm_string_view_t alias,
                        orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require_query_kind(*query, query_kind::select,
                           "add_aggregate requires a select query");
        require(!query->select_all, ORM_STATUS_INVALID_STATE,
                "cannot add an aggregate after select_all");
        require(query->columns.size() + query->aggregates.size() +
                    query->scalar_projections.size() <
                    query->state->limits.max_columns,
                ORM_STATUS_LIMIT_EXCEEDED,
                "selected expression count exceeds max_columns");
        query->aggregates.push_back(make_aggregate(aggregate, column, alias));
    });
}

orm_status_t ORM_C_CALL
orm_query_add_expression(orm_query_t* query, orm_scalar_expression_t expression,
                         orm_string_view_t alias, orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require_query_kind(*query, query_kind::select,
                           "scalar expression requires a select query");
        require(!query->select_all, ORM_STATUS_INVALID_STATE,
                "cannot add an expression after select_all");
        require(expression.tokens != nullptr && expression.token_count != 0,
                ORM_STATUS_INVALID_ARGUMENT, "scalar expression is empty");
        require(expression.token_count <= query->state->limits.max_query_bytes,
                ORM_STATUS_LIMIT_EXCEEDED,
                "scalar expression token count exceeds max_query_bytes");
        require(query->columns.size() + query->aggregates.size() +
                    query->scalar_projections.size() < query->state->limits.max_columns,
                ORM_STATUS_LIMIT_EXCEEDED, "selected expression count exceeds max_columns");
        scalar_projection projection;
        projection.alias = copy_string_view(alias, "scalar expression alias", true,
                                             max_identifier_segment_bytes);
        if (!projection.alias.empty()) {
            const orm_string_view_t view{projection.alias.data(), projection.alias.size()};
            projection.alias = copy_identifier(view, "scalar expression alias", false);
        }
        std::size_t added_parameters = 0, added_bytes = 0;
        projection.tokens = parse_scalar_tokens(*query, expression,
                                               added_parameters, added_bytes);
        query->scalar_projections.push_back(std::move(projection));
        query->parameter_count += added_parameters;
        query->parameter_bytes += added_bytes;
    });
}

orm_status_t ORM_C_CALL
orm_query_set(orm_query_t* query,
              orm_string_view_t column,
              orm_value_t value,
              orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require(query->kind == query_kind::insert || query->kind == query_kind::update,
                ORM_STATUS_INVALID_STATE,
                "set requires an insert or update query");
        require(query->assignments.size() < query->state->limits.max_assignments,
                ORM_STATUS_LIMIT_EXCEEDED,
                "assignment count exceeds max_assignments");
        assignment next;
        next.column = copy_identifier(column, "assignment column", false);
        require(std::none_of(query->assignments.begin(), query->assignments.end(),
                             [&](const assignment& item) {
                                 return item.column == next.column;
                             }),
                ORM_STATUS_INVALID_ARGUMENT, "duplicate assignment column");
        if (value.kind == ORM_VALUE_NULL) {
            require(value.reserved == 0, ORM_STATUS_ABI_MISMATCH,
                    "orm_value_t.reserved must be zero");
            next.has_parameter = false;
        } else {
            next.parameter = encode_parameter(value,
                                               query->state->limits.max_parameter_bytes);
            require(query->parameter_count < query->state->limits.max_parameters,
                    ORM_STATUS_LIMIT_EXCEEDED,
                    "parameter count exceeds max_parameters");
            const std::size_t payload = parameter_payload_size(next.parameter);
            require(payload <=
                        query->state->limits.max_parameter_bytes - query->parameter_bytes,
                    ORM_STATUS_LIMIT_EXCEEDED,
                    "parameter payload exceeds max_parameter_bytes");
        }
        query->assignments.reserve(query->assignments.size() + 1);
        query->assignments.push_back(std::move(next));
        if (query->assignments.back().has_parameter)
            charge_parameter(*query, query->assignments.back().parameter);
    });
}

orm_status_t ORM_C_CALL
orm_query_where(orm_query_t* query,
                   orm_string_view_t column,
                   orm_compare_t comparison,
                   orm_value_t value,
                   orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require(supports_where(query->kind), ORM_STATUS_INVALID_STATE,
                "where requires a select, update, or delete query");
        append_predicate(*query, query->where_stack,
                         make_predicate(*query,
                                        copy_identifier(column, "predicate column", true),
                                        comparison, value));
    });
}

orm_status_t ORM_C_CALL
orm_query_where_columns(orm_query_t* query,
                        orm_string_view_t left_column,
                        orm_compare_t comparison,
                        orm_string_view_t right_column,
                        orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "query handle is null");
        require(supports_where(query->kind), ORM_STATUS_INVALID_STATE,
                "column comparison requires a select, update, or delete query");
        append_predicate(
            *query, query->where_stack,
            make_column_predicate(
                *query, copy_identifier(left_column, "left predicate column", true),
                comparison,
                copy_identifier(right_column, "right predicate column", true)));
    });
}

orm_status_t ORM_C_CALL
orm_query_where_exists(orm_query_t* query,
                       const orm_query_t* subquery,
                       int negated,
                       orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "query handle is null");
        require(subquery != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "subquery handle is null");
        require(query != subquery, ORM_STATUS_INVALID_ARGUMENT,
                "query cannot contain itself as a subquery");
        require(supports_where(query->kind), ORM_STATUS_INVALID_STATE,
                "EXISTS requires a select, update, or delete query");
        require_query_kind(*subquery, query_kind::select,
                           "EXISTS requires a select subquery");
        require(query->state == subquery->state, ORM_STATUS_INVALID_ARGUMENT,
                "outer query and subquery must share one connection");
        require(subquery->where_stack.size() == 1 &&
                    subquery->having_stack.size() == 1,
                ORM_STATUS_INVALID_STATE,
                "subquery has an unclosed condition group");
        append_exists(*query, *subquery, negated != 0);
    });
}

orm_status_t ORM_C_CALL
orm_query_where_in_subquery(orm_query_t* query,
                            orm_string_view_t column,
                            const orm_query_t* subquery,
                            int negated,
                            orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "query handle is null");
        require(subquery != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "subquery handle is null");
        require(query != subquery, ORM_STATUS_INVALID_ARGUMENT,
                "query cannot contain itself as a subquery");
        require(supports_where(query->kind), ORM_STATUS_INVALID_STATE,
                "IN subquery requires a select, update, or delete query");
        require_query_kind(*subquery, query_kind::select,
                           "IN requires a select subquery");
        require(query->state == subquery->state, ORM_STATUS_INVALID_ARGUMENT,
                "outer query and subquery must share one connection");
        require(subquery->where_stack.size() == 1 &&
                    subquery->having_stack.size() == 1,
                ORM_STATUS_INVALID_STATE,
                "subquery has an unclosed condition group");
        require(!subquery->select_all &&
                    subquery->columns.size() + subquery->aggregates.size() +
                        subquery->scalar_projections.size() == 1,
                ORM_STATUS_INVALID_STATE,
                "IN subquery must project exactly one expression");
        append_subquery_predicate(
            *query, *subquery, negated != 0,
            copy_identifier(column, "IN subquery column", true));
    });
}

orm_status_t ORM_C_CALL
orm_query_where_scalar_subquery(orm_query_t* query,
                                orm_string_view_t column,
                                orm_compare_t comparison,
                                const orm_query_t* subquery,
                                orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "query handle is null");
        require(subquery != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "subquery handle is null");
        require(query != subquery, ORM_STATUS_INVALID_ARGUMENT,
                "query cannot contain itself as a subquery");
        append_scalar_subquery(*query, column, comparison, *subquery);
    });
}

orm_status_t ORM_C_CALL
orm_query_where_quantified_subquery(orm_query_t* query,
                                    orm_string_view_t column,
                                    orm_compare_t comparison,
                                    orm_subquery_quantifier_t quantifier,
                                    const orm_query_t* subquery,
                                    orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "query handle is null");
        require(subquery != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "subquery handle is null");
        require(query != subquery, ORM_STATUS_INVALID_ARGUMENT,
                "query cannot contain itself as a subquery");
        require(quantifier == ORM_SUBQUERY_ANY || quantifier == ORM_SUBQUERY_ALL,
                ORM_STATUS_INVALID_ARGUMENT, "unknown subquery quantifier");
        append_scalar_subquery(*query, column, comparison, *subquery,
                               quantifier == ORM_SUBQUERY_ANY ? "any" : "all");
    });
}

orm_status_t ORM_C_CALL
orm_query_begin_where_group(orm_query_t* query,
                            orm_logic_t logic,
                            orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require(supports_where(query->kind), ORM_STATUS_INVALID_STATE,
                "where group requires a select, update, or delete query");
        begin_group(*query, query->where_stack, logic);
    });
}

orm_status_t ORM_C_CALL
orm_query_end_where_group(orm_query_t* query, orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require(supports_where(query->kind), ORM_STATUS_INVALID_STATE,
                "where group requires a select, update, or delete query");
        end_group(query->where_stack);
    });
}

orm_status_t ORM_C_CALL
orm_query_join(orm_query_t* query,
               orm_join_t join,
               orm_string_view_t table,
               orm_string_view_t left_column,
               orm_compare_t comparison,
               orm_string_view_t right_column,
               orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require_query_kind(*query, query_kind::select, "join requires a select query");
        require(join == ORM_JOIN_INNER || join == ORM_JOIN_LEFT,
                ORM_STATUS_INVALID_ARGUMENT, "unknown join kind");
        require(query->joins.size() < query->state->limits.max_joins,
                ORM_STATUS_LIMIT_EXCEEDED, "join count exceeds max_joins");
        join_clause clause;
        clause.kind = join;
        clause.table = copy_identifier(table, "join table", true);
        clause.left_column = copy_identifier(left_column, "join left column", true);
        clause.operation = comparison_sql(comparison);
        clause.right_column = copy_identifier(right_column, "join right column", true);
        query->joins.push_back(std::move(clause));
    });
}

orm_status_t ORM_C_CALL
orm_query_group_by(orm_query_t* query,
                   orm_string_view_t column,
                   orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require_query_kind(*query, query_kind::select,
                           "group_by requires a select query");
        require(query->group_columns.size() < query->state->limits.max_group_columns,
                ORM_STATUS_LIMIT_EXCEEDED,
                "group column count exceeds max_group_columns");
        query->group_columns.push_back(copy_identifier(column, "group column", true));
    });
}

orm_status_t ORM_C_CALL
orm_query_having(orm_query_t* query,
                 orm_string_view_t column,
                 orm_compare_t comparison,
                 orm_value_t value,
                 orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require_query_kind(*query, query_kind::select,
                           "having requires a select query");
        append_predicate(*query, query->having_stack,
                         make_predicate(*query,
                                        copy_identifier(column, "having column", true),
                                        comparison, value));
    });
}

orm_status_t ORM_C_CALL
orm_query_having_aggregate(orm_query_t* query,
                           orm_aggregate_t aggregate,
                           orm_string_view_t column,
                           orm_compare_t comparison,
                           orm_value_t value,
                           orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require_query_kind(*query, query_kind::select,
                           "aggregate having requires a select query");
        const orm_string_view_t empty{nullptr, 0};
        aggregate_expression expression = make_aggregate(aggregate, column, empty);
        append_predicate(*query, query->having_stack,
                         make_predicate(*query, aggregate_sql(expression), comparison, value));
    });
}

orm_status_t ORM_C_CALL
orm_query_begin_having_group(orm_query_t* query,
                             orm_logic_t logic,
                             orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require_query_kind(*query, query_kind::select,
                           "having group requires a select query");
        begin_group(*query, query->having_stack, logic);
    });
}

orm_status_t ORM_C_CALL
orm_query_end_having_group(orm_query_t* query, orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require_query_kind(*query, query_kind::select,
                           "having group requires a select query");
        end_group(query->having_stack);
    });
}

orm_status_t ORM_C_CALL
orm_query_bind(orm_query_t* query, orm_value_t value, orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require_query_kind(*query, query_kind::raw, "bind requires a raw query");
        bound_parameter parameter =
            encode_parameter(value, query->state->limits.max_parameter_bytes);
        require(query->parameter_count < query->state->limits.max_parameters,
                ORM_STATUS_LIMIT_EXCEEDED, "parameter count exceeds max_parameters");
        const std::size_t payload = parameter_payload_size(parameter);
        require(payload <=
                    query->state->limits.max_parameter_bytes - query->parameter_bytes,
                ORM_STATUS_LIMIT_EXCEEDED,
                "parameter payload exceeds max_parameter_bytes");
        query->raw_parameters.reserve(query->raw_parameters.size() + 1);
        query->raw_parameters.push_back(std::move(parameter));
        charge_parameter(*query, query->raw_parameters.back());
    });
}

orm_status_t ORM_C_CALL
orm_query_order_by(orm_query_t* query,
                      orm_string_view_t column,
                      orm_order_t order,
                      orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require_query_kind(*query, query_kind::select,
                           "order_by requires a select query");
        require(order == ORM_ORDER_ASCENDING || order == ORM_ORDER_DESCENDING,
                ORM_STATUS_INVALID_ARGUMENT,
                "unknown ordering direction");
        std::string copied = copy_identifier(column, "ordering column", true);
        ordering_spec spec;
        spec.column = std::move(copied);
        spec.order = order;
        query->ordering = std::move(spec);
    });
}

orm_status_t ORM_C_CALL
orm_query_order_by_expression(orm_query_t* query,
                               orm_scalar_expression_t expression,
                               orm_order_t order,
                               orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require_query_kind(*query, query_kind::select,
                           "ORDER BY expression requires a select query");
        require(order == ORM_ORDER_ASCENDING || order == ORM_ORDER_DESCENDING,
                ORM_STATUS_INVALID_ARGUMENT,
                "unknown ordering direction");
        std::size_t added_parameters = 0, added_bytes = 0;
        ordering_spec spec;
        spec.is_expression = true;
        spec.order = order;
        spec.tokens = parse_scalar_tokens(*query, expression, added_parameters, added_bytes);
        query->parameter_count += added_parameters;
        query->parameter_bytes += added_bytes;
        query->ordering = std::move(spec);
    });
}

orm_status_t ORM_C_CALL
orm_query_set_limit(orm_query_t* query,
                       uint64_t limit,
                       orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require_query_kind(*query, query_kind::select,
                           "limit requires a select query");
        query->limit = limit;
    });
}

orm_status_t ORM_C_CALL
orm_query_set_offset(orm_query_t* query,
                        uint64_t offset,
                        orm_error_t* error)
{
    return api_call(error, [&] {
        require(query != nullptr, ORM_STATUS_INVALID_ARGUMENT, "query handle is null");
        require_query_kind(*query, query_kind::select,
                           "offset requires a select query");
        query->offset = offset;
    });
}

orm_status_t ORM_C_CALL
orm_query_execute(orm_query_t* query,
                     orm_result_t** out_result,
                     orm_error_t* error)
{
    if (out_result != nullptr)
        *out_result = nullptr;
    return api_call(error, [&] {
        require(query != nullptr && query->state != nullptr,
                ORM_STATUS_INVALID_ARGUMENT,
                "query handle is null");
        require(out_result != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "out_result pointer is null");
        require(query->where_stack.size() == 1 && query->having_stack.size() == 1,
                ORM_STATUS_INVALID_STATE,
                "query has an unclosed condition group");

        auto handle = std::make_unique<orm_result>();
        handle->backend = execute_query(*query, *query->state->backend);
        require(handle->backend != nullptr, ORM_STATUS_INTERNAL_ERROR,
                "database backend returned a null result");
        *out_result = handle.release();
    });
}

orm_status_t ORM_C_CALL
orm_query_execute_in_transaction(orm_query_t* query,
                                 orm_transaction_t* transaction,
                                 orm_result_t** out_result,
                                 orm_error_t* error)
{
    if (out_result != nullptr)
        *out_result = nullptr;
    return api_call(error, [&] {
        require(query != nullptr && query->state != nullptr,
                ORM_STATUS_INVALID_ARGUMENT,
                "query handle is null");
        require_active(transaction);
        require(out_result != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "out_result pointer is null");
        require(query->state.get() == transaction->state.get(),
                ORM_STATUS_INVALID_ARGUMENT,
                "query and transaction belong to different connections");
        require(query->where_stack.size() == 1 && query->having_stack.size() == 1,
                ORM_STATUS_INVALID_STATE,
                "query has an unclosed condition group");

        auto handle = std::make_unique<orm_result>();
        handle->backend = execute_query(*query, *transaction->backend);
        require(handle->backend != nullptr, ORM_STATUS_INTERNAL_ERROR,
                "database transaction returned a null result");
        *out_result = handle.release();
    });
}

void ORM_C_CALL orm_result_destroy(orm_result_t* result)
{
    delete result;
}

orm_status_t ORM_C_CALL
orm_result_row_count(const orm_result_t* result,
                        uint64_t* out_count,
                        orm_error_t* error)
{
    return api_call(error, [&] {
        require(result != nullptr, ORM_STATUS_INVALID_ARGUMENT, "result handle is null");
        require(out_count != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "out_count pointer is null");
        require(result->backend != nullptr, ORM_STATUS_INVALID_STATE,
                "result backend is null");
        *out_count = result->backend->rows();
    });
}

orm_status_t ORM_C_CALL
orm_result_column_count(const orm_result_t* result,
                           uint64_t* out_count,
                           orm_error_t* error)
{
    return api_call(error, [&] {
        require(result != nullptr, ORM_STATUS_INVALID_ARGUMENT, "result handle is null");
        require(out_count != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "out_count pointer is null");
        require(result->backend != nullptr, ORM_STATUS_INVALID_STATE,
                "result backend is null");
        *out_count = result->backend->columns();
    });
}

orm_status_t ORM_C_CALL
orm_result_affected_rows(const orm_result_t* result,
                         uint64_t* out_count,
                         orm_error_t* error)
{
    return api_call(error, [&] {
        require(result != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "result handle is null");
        require(out_count != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "out_count pointer is null");
        require(result->backend != nullptr, ORM_STATUS_INVALID_STATE,
                "result backend is null");
        *out_count = result->backend->affected_rows();
    });
}

orm_status_t ORM_C_CALL
orm_result_is_null(const orm_result_t* result,
                      uint64_t row,
                      uint64_t column,
                      uint8_t* out_is_null,
                      orm_error_t* error)
{
    return api_call(error, [&] {
        require(result != nullptr, ORM_STATUS_INVALID_ARGUMENT, "result handle is null");
        require(out_is_null != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "out_is_null pointer is null");
        const vstr cell = get_cell(*result, row, column, false);
        *out_is_null = cell.data == nullptr ? UINT8_C(1) : UINT8_C(0);
    });
}

orm_status_t ORM_C_CALL
orm_result_get_text(const orm_result_t* result,
                       uint64_t row,
                       uint64_t column,
                       orm_string_view_t* out_value,
                       orm_error_t* error)
{
    if (out_value != nullptr)
        *out_value = orm_string_view_t{nullptr, 0};
    return api_call(error, [&] {
        require(result != nullptr, ORM_STATUS_INVALID_ARGUMENT, "result handle is null");
        require(out_value != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "out_value pointer is null");
        const vstr cell = get_cell(*result, row, column, true);
        out_value->data = cell.data;
        out_value->len = cell.len;
    });
}

orm_status_t ORM_C_CALL
orm_result_get_blob(const orm_result_t* result,
                       uint64_t row,
                       uint64_t column,
                       orm_blob_t* out_value,
                       orm_error_t* error)
{
    if (out_value != nullptr)
        *out_value = orm_blob_t{nullptr, 0};
    return api_call(error, [&] {
        require(result != nullptr, ORM_STATUS_INVALID_ARGUMENT, "result handle is null");
        require(out_value != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "out_value pointer is null");
        const vstr cell = get_cell(*result, row, column, true);
        out_value->data = cell.data;
        out_value->size = cell.len;
    });
}

orm_status_t ORM_C_CALL
orm_result_get_int64(const orm_result_t* result,
                        uint64_t row,
                        uint64_t column,
                        int64_t* out_value,
                        orm_error_t* error)
{
    return api_call(error, [&] {
        require(result != nullptr, ORM_STATUS_INVALID_ARGUMENT, "result handle is null");
        require(out_value != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "out_value pointer is null");
        *out_value = parse_integer<std::int64_t>(get_cell(*result, row, column, true));
    });
}

orm_status_t ORM_C_CALL
orm_result_get_uint64(const orm_result_t* result,
                         uint64_t row,
                         uint64_t column,
                         uint64_t* out_value,
                         orm_error_t* error)
{
    return api_call(error, [&] {
        require(result != nullptr, ORM_STATUS_INVALID_ARGUMENT, "result handle is null");
        require(out_value != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "out_value pointer is null");
        *out_value = parse_integer<std::uint64_t>(get_cell(*result, row, column, true));
    });
}

orm_status_t ORM_C_CALL
orm_result_get_double(const orm_result_t* result,
                         uint64_t row,
                         uint64_t column,
                         double* out_value,
                         orm_error_t* error)
{
    return api_call(error, [&] {
        require(result != nullptr, ORM_STATUS_INVALID_ARGUMENT, "result handle is null");
        require(out_value != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "out_value pointer is null");
        const vstr cell = get_cell(*result, row, column, true);
        std::istringstream stream(std::string(cell.data, cell.len));
        stream.imbue(std::locale::classic());
        double value = 0.0;
        stream >> std::noskipws >> value;
        if (!stream || !stream.eof())
            fail(ORM_STATUS_TYPE_ERROR, "result cell is not a valid double");
        *out_value = value;
    });
}

orm_status_t ORM_C_CALL
orm_result_get_boolean(const orm_result_t* result,
                          uint64_t row,
                          uint64_t column,
                          uint8_t* out_value,
                          orm_error_t* error)
{
    return api_call(error, [&] {
        require(result != nullptr, ORM_STATUS_INVALID_ARGUMENT, "result handle is null");
        require(out_value != nullptr, ORM_STATUS_INVALID_ARGUMENT,
                "out_value pointer is null");
        const vstr cell = get_cell(*result, row, column, true);
        const std::string_view value(cell.data, cell.len);
        if (value == "t" || value == "true" || value == "1") {
            *out_value = UINT8_C(1);
        } else if (value == "f" || value == "false" || value == "0") {
            *out_value = UINT8_C(0);
        } else {
            fail(ORM_STATUS_TYPE_ERROR, "result cell is not a valid boolean");
        }
    });
}

} // extern "C"
