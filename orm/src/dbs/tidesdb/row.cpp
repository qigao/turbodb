#include "row.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace orm_c_detail {
namespace {

constexpr std::uint8_t row_magic[] = {'O', 'R', 'M', 'T', 'D', 'B', 1, 0};
constexpr std::size_t row_header_bytes = sizeof(row_magic) + sizeof(std::uint32_t);
constexpr std::size_t field_header_bytes = sizeof(std::uint16_t) + 2 + sizeof(std::uint32_t);
constexpr std::uint8_t null_flag = 1;

enum class truth_value {
    false_value,
    true_value,
    unknown
};

template<typename Integer>
Integer parse_integer(std::string_view text, const char* role)
{
    Integer value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        fail(ORM_STATUS_DATASTORE_ERROR,
             std::string("invalid TidesDB ") + role + " encoding");
    return value;
}

double parse_double(std::string_view text)
{
    double value = 0.0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        !std::isfinite(value))
        fail(ORM_STATUS_DATASTORE_ERROR, "invalid TidesDB double encoding");
    return value;
}

bool is_known_kind(orm_value_kind_t kind) noexcept
{
    return kind >= ORM_VALUE_NULL && kind <= ORM_VALUE_BLOB;
}

bool is_numeric_kind(orm_value_kind_t kind) noexcept
{
    return kind == ORM_VALUE_INT64 || kind == ORM_VALUE_UINT64 ||
           kind == ORM_VALUE_DOUBLE || kind == ORM_VALUE_BOOLEAN;
}

void validate_cell(const tidesdb_cell& cell)
{
    require(is_known_kind(cell.kind), ORM_STATUS_DATASTORE_ERROR,
            "TidesDB row contains an unknown value kind");
    if (cell.is_null) {
        require(cell.kind == ORM_VALUE_NULL && cell.text.empty(),
                ORM_STATUS_DATASTORE_ERROR,
                "TidesDB null field has a payload or non-null kind");
        return;
    }
    require(cell.kind != ORM_VALUE_NULL, ORM_STATUS_DATASTORE_ERROR,
            "TidesDB non-null field has null kind");
    if (cell.kind != ORM_VALUE_BLOB)
        require(cell.text.find('\0') == std::string::npos,
                ORM_STATUS_DATASTORE_ERROR,
                "TidesDB row text contains an embedded null byte");
    switch (cell.kind) {
    case ORM_VALUE_INT64:
        (void)parse_integer<std::int64_t>(cell.text, "signed integer");
        break;
    case ORM_VALUE_UINT64:
        (void)parse_integer<std::uint64_t>(cell.text, "unsigned integer");
        break;
    case ORM_VALUE_DOUBLE:
        (void)parse_double(cell.text);
        break;
    case ORM_VALUE_BOOLEAN:
        require(cell.text == "0" || cell.text == "1",
                ORM_STATUS_DATASTORE_ERROR,
                "invalid TidesDB boolean encoding");
        break;
    case ORM_VALUE_TEXT:
    case ORM_VALUE_BLOB:
        break;
    default:
        fail(ORM_STATUS_DATASTORE_ERROR, "invalid TidesDB value kind");
    }
}

void append_checked(std::string& output,
                    const void* data,
                    std::size_t size,
                    std::size_t maximum_bytes)
{
    require(size <= maximum_bytes && output.size() <= maximum_bytes - size,
            ORM_STATUS_LIMIT_EXCEEDED,
            "encoded TidesDB row exceeds its byte limit");
    output.append(static_cast<const char*>(data), size);
}

void append_u16(std::string& output,
                std::uint16_t value,
                std::size_t maximum_bytes)
{
    const std::uint8_t encoded[] = {
        static_cast<std::uint8_t>(value),
        static_cast<std::uint8_t>(value >> 8)};
    append_checked(output, encoded, sizeof(encoded), maximum_bytes);
}

void append_u32(std::string& output,
                std::uint32_t value,
                std::size_t maximum_bytes)
{
    const std::uint8_t encoded[] = {
        static_cast<std::uint8_t>(value),
        static_cast<std::uint8_t>(value >> 8),
        static_cast<std::uint8_t>(value >> 16),
        static_cast<std::uint8_t>(value >> 24)};
    append_checked(output, encoded, sizeof(encoded), maximum_bytes);
}

class row_reader final {
public:
    row_reader(const std::uint8_t* data, std::size_t size) : current_(data), remaining_(size) {}

    std::uint8_t read_u8()
    {
        require(remaining_ >= 1, ORM_STATUS_DATASTORE_ERROR,
                "truncated TidesDB row encoding");
        const std::uint8_t value = *current_;
        ++current_;
        --remaining_;
        return value;
    }

    std::uint16_t read_u16()
    {
        const std::uint16_t first = read_u8();
        return static_cast<std::uint16_t>(first |
                                         (static_cast<std::uint16_t>(read_u8()) << 8));
    }

    std::uint32_t read_u32()
    {
        std::uint32_t value = 0;
        for (unsigned shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(read_u8()) << shift;
        return value;
    }

    std::string read_string(std::size_t size)
    {
        require(size <= remaining_, ORM_STATUS_DATASTORE_ERROR,
                "truncated TidesDB row payload");
        std::string value;
        if (size != 0)
            value.assign(reinterpret_cast<const char*>(current_), size);
        current_ += size;
        remaining_ -= size;
        return value;
    }

    std::size_t remaining() const noexcept { return remaining_; }

private:
    const std::uint8_t* current_;
    std::size_t remaining_;
};

bool like_matches(std::string_view value, std::string_view pattern)
{
    std::size_t value_index = 0;
    std::size_t pattern_index = 0;
    std::size_t wildcard = std::string_view::npos;
    std::size_t wildcard_value = 0;
    while (value_index < value.size()) {
        if (pattern_index < pattern.size() &&
            (pattern[pattern_index] == '_' || pattern[pattern_index] == value[value_index])) {
            ++value_index;
            ++pattern_index;
        } else if (pattern_index < pattern.size() && pattern[pattern_index] == '%') {
            wildcard = pattern_index++;
            wildcard_value = value_index;
        } else if (wildcard != std::string_view::npos) {
            pattern_index = wildcard + 1;
            value_index = ++wildcard_value;
        } else {
            return false;
        }
    }
    while (pattern_index < pattern.size() && pattern[pattern_index] == '%')
        ++pattern_index;
    return pattern_index == pattern.size();
}

int compare_integer_cells(const tidesdb_cell& left, const tidesdb_cell& right)
{
    const bool left_signed = left.kind == ORM_VALUE_INT64;
    const bool right_signed = right.kind == ORM_VALUE_INT64;
    const std::int64_t left_i = left_signed
        ? parse_integer<std::int64_t>(left.text, "signed integer") : 0;
    const std::int64_t right_i = right_signed
        ? parse_integer<std::int64_t>(right.text, "signed integer") : 0;
    const std::uint64_t left_u = left_signed
        ? 0 : (left.kind == ORM_VALUE_BOOLEAN
                   ? static_cast<std::uint64_t>(left.text == "1")
                   : parse_integer<std::uint64_t>(left.text, "unsigned integer"));
    const std::uint64_t right_u = right_signed
        ? 0 : (right.kind == ORM_VALUE_BOOLEAN
                   ? static_cast<std::uint64_t>(right.text == "1")
                   : parse_integer<std::uint64_t>(right.text, "unsigned integer"));

    if (left_signed && right_signed)
        return left_i < right_i ? -1 : (left_i > right_i ? 1 : 0);
    if (!left_signed && !right_signed)
        return left_u < right_u ? -1 : (left_u > right_u ? 1 : 0);
    if (left_signed) {
        if (left_i < 0)
            return -1;
        const std::uint64_t converted = static_cast<std::uint64_t>(left_i);
        return converted < right_u ? -1 : (converted > right_u ? 1 : 0);
    }
    if (right_i < 0)
        return 1;
    const std::uint64_t converted = static_cast<std::uint64_t>(right_i);
    return left_u < converted ? -1 : (left_u > converted ? 1 : 0);
}

truth_value evaluate_predicate(const predicate& condition,
                               const tidesdb_row& row,
                               std::string_view table)
{
    const tidesdb_cell* stored = tidesdb_find_cell(row, condition.column);
    if (stored == nullptr) {
        const std::string column = tidesdb_unqualified_column(table, condition.column);
        stored = tidesdb_find_cell(row, column);
    }
    const bool is_null = stored == nullptr || stored->is_null;
    if (!condition.has_parameter) {
        const bool equal = condition.comparison == ORM_COMPARE_EQUAL;
        return (is_null == equal) ? truth_value::true_value : truth_value::false_value;
    }
    if (is_null)
        return truth_value::unknown;

    const tidesdb_cell expected = tidesdb_cell_from_parameter(condition.parameter);
    if (condition.comparison == ORM_COMPARE_LIKE ||
        condition.comparison == ORM_COMPARE_NOT_LIKE) {
        require(stored->kind == ORM_VALUE_TEXT && expected.kind == ORM_VALUE_TEXT,
                ORM_STATUS_TYPE_ERROR,
                "TidesDB LIKE requires text values");
        const bool matched = like_matches(stored->text, expected.text);
        const bool result = condition.comparison == ORM_COMPARE_NOT_LIKE ? !matched : matched;
        return result ? truth_value::true_value : truth_value::false_value;
    }

    const int comparison = tidesdb_compare_cells(*stored, expected);
    bool matched = false;
    switch (condition.comparison) {
    case ORM_COMPARE_EQUAL: matched = comparison == 0; break;
    case ORM_COMPARE_NOT_EQUAL: matched = comparison != 0; break;
    case ORM_COMPARE_LESS: matched = comparison < 0; break;
    case ORM_COMPARE_LESS_EQUAL: matched = comparison <= 0; break;
    case ORM_COMPARE_GREATER: matched = comparison > 0; break;
    case ORM_COMPARE_GREATER_EQUAL: matched = comparison >= 0; break;
    default:
        fail(ORM_STATUS_INVALID_ARGUMENT, "unknown TidesDB comparison operator");
    }
    return matched ? truth_value::true_value : truth_value::false_value;
}

truth_value evaluate_group(const condition_node& group,
                           const tidesdb_row& row,
                           std::string_view table)
{
    require(group.is_group, ORM_STATUS_INTERNAL_ERROR,
            "TidesDB condition root is not a group");
    if (group.children.empty())
        return truth_value::true_value;

    truth_value result = group.logic == ORM_LOGIC_AND
        ? truth_value::true_value : truth_value::false_value;
    for (const auto& child_pointer : group.children) {
        require(child_pointer != nullptr, ORM_STATUS_INTERNAL_ERROR,
                "TidesDB condition tree contains a null node");
        const condition_node& child = *child_pointer;
        const truth_value next = child.is_group
            ? evaluate_group(child, row, table)
            : evaluate_predicate(child.value, row, table);
        if (group.logic == ORM_LOGIC_AND) {
            if (next == truth_value::false_value)
                return truth_value::false_value;
            if (next == truth_value::unknown)
                result = truth_value::unknown;
        } else {
            if (next == truth_value::true_value)
                return truth_value::true_value;
            if (next == truth_value::unknown)
                result = truth_value::unknown;
        }
    }
    return result;
}

} // namespace

std::string encode_tidesdb_row(const tidesdb_row& row,
                               std::size_t maximum_bytes,
                               std::size_t maximum_fields)
{
    require(row.size() <= maximum_fields &&
                row.size() <= std::numeric_limits<std::uint32_t>::max(),
            ORM_STATUS_LIMIT_EXCEEDED,
            "TidesDB row field count exceeds its limit");
    require(maximum_bytes >= row_header_bytes,
            ORM_STATUS_LIMIT_EXCEEDED,
            "TidesDB row byte limit is too small");

    std::string encoded;
    encoded.reserve(std::min(maximum_bytes,
                             row_header_bytes + row.size() * field_header_bytes));
    append_checked(encoded, row_magic, sizeof(row_magic), maximum_bytes);
    append_u32(encoded, static_cast<std::uint32_t>(row.size()), maximum_bytes);
    for (const tidesdb_field& field : row) {
        require(!field.name.empty() &&
                    field.name.size() <= std::numeric_limits<std::uint16_t>::max(),
                ORM_STATUS_DATASTORE_ERROR,
                "TidesDB row field name has an invalid length");
        require(field.name.find('\0') == std::string::npos,
                ORM_STATUS_DATASTORE_ERROR,
                "TidesDB row field name contains a null byte");
        validate_cell(field.value);
        require(field.value.text.size() <= std::numeric_limits<std::uint32_t>::max(),
                ORM_STATUS_LIMIT_EXCEEDED,
                "TidesDB row field value is too large");
        append_u16(encoded, static_cast<std::uint16_t>(field.name.size()), maximum_bytes);
        const std::uint8_t kind = static_cast<std::uint8_t>(field.value.kind);
        append_checked(encoded, &kind, sizeof(kind), maximum_bytes);
        const std::uint8_t flags = field.value.is_null ? null_flag : 0;
        append_checked(encoded, &flags, sizeof(flags), maximum_bytes);
        append_u32(encoded, static_cast<std::uint32_t>(field.value.text.size()), maximum_bytes);
        append_checked(encoded, field.name.data(), field.name.size(), maximum_bytes);
        append_checked(encoded, field.value.text.data(), field.value.text.size(), maximum_bytes);
    }
    return encoded;
}

tidesdb_row decode_tidesdb_row(const std::uint8_t* data,
                               std::size_t size,
                               std::size_t maximum_bytes,
                               std::size_t maximum_fields)
{
    require(data != nullptr || size == 0, ORM_STATUS_DATASTORE_ERROR,
            "TidesDB row has a null data pointer");
    require(size <= maximum_bytes, ORM_STATUS_LIMIT_EXCEEDED,
            "stored TidesDB row exceeds its byte limit");
    require(size >= row_header_bytes &&
                std::memcmp(data, row_magic, sizeof(row_magic)) == 0,
            ORM_STATUS_DATASTORE_ERROR,
            "TidesDB row has an unknown format or version");

    row_reader reader(data + sizeof(row_magic), size - sizeof(row_magic));
    const std::uint32_t count = reader.read_u32();
    require(count <= maximum_fields, ORM_STATUS_LIMIT_EXCEEDED,
            "stored TidesDB row field count exceeds its limit");
    tidesdb_row row;
    row.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::uint16_t name_size = reader.read_u16();
        const orm_value_kind_t kind = static_cast<orm_value_kind_t>(reader.read_u8());
        const std::uint8_t flags = reader.read_u8();
        const std::uint32_t value_size = reader.read_u32();
        require(name_size != 0 && (flags & ~null_flag) == 0,
                ORM_STATUS_DATASTORE_ERROR,
                "TidesDB row field header is invalid");
        tidesdb_field field;
        field.name = reader.read_string(name_size);
        require(field.name.find('\0') == std::string::npos,
                ORM_STATUS_DATASTORE_ERROR,
                "TidesDB row field name contains a null byte");
        require(tidesdb_find_cell(row, field.name) == nullptr,
                ORM_STATUS_DATASTORE_ERROR,
                "TidesDB row contains a duplicate field");
        field.value.kind = kind;
        field.value.is_null = (flags & null_flag) != 0;
        field.value.text = reader.read_string(value_size);
        validate_cell(field.value);
        row.push_back(std::move(field));
    }
    require(reader.remaining() == 0, ORM_STATUS_DATASTORE_ERROR,
            "TidesDB row has trailing bytes");
    return row;
}

tidesdb_cell tidesdb_cell_from_parameter(const bound_parameter& parameter)
{
    tidesdb_cell cell;
    cell.kind = parameter.kind;
    cell.is_null = parameter.kind == ORM_VALUE_NULL;
    switch (parameter.kind) {
    case ORM_VALUE_NULL:
        break;
    case ORM_VALUE_BOOLEAN:
        cell.text = parameter.boolean_value ? "1" : "0";
        break;
    case ORM_VALUE_INT64:
    case ORM_VALUE_UINT64:
    case ORM_VALUE_DOUBLE:
    case ORM_VALUE_TEXT:
        cell.text = parameter.text;
        break;
    case ORM_VALUE_BLOB:
        cell.text = parameter.binary;
        break;
    default:
        fail(ORM_STATUS_INTERNAL_ERROR, "unknown ORM parameter type for TidesDB");
    }
    validate_cell(cell);
    return cell;
}

tidesdb_cell tidesdb_null_cell()
{
    return tidesdb_cell{};
}

const tidesdb_cell* tidesdb_find_cell(const tidesdb_row& row,
                                      std::string_view column) noexcept
{
    const auto found = std::find_if(row.begin(), row.end(), [&](const tidesdb_field& field) {
        return field.name == column;
    });
    return found == row.end() ? nullptr : &found->value;
}

void tidesdb_set_cell(tidesdb_row& row,
                      std::string column,
                      tidesdb_cell value,
                      std::size_t maximum_fields)
{
    validate_cell(value);
    auto found = std::find_if(row.begin(), row.end(), [&](const tidesdb_field& field) {
        return field.name == column;
    });
    if (found != row.end()) {
        found->value = std::move(value);
        return;
    }
    require(row.size() < maximum_fields, ORM_STATUS_LIMIT_EXCEEDED,
            "TidesDB row field count exceeds max_assignments");
    row.push_back(tidesdb_field{std::move(column), std::move(value)});
}

std::string tidesdb_unqualified_column(std::string_view table,
                                       std::string_view column)
{
    const std::size_t separator = column.find('.');
    if (separator == std::string_view::npos)
        return std::string(column);
    require(column.find('.', separator + 1) == std::string_view::npos &&
                column.substr(0, separator) == table && separator + 1 < column.size(),
            ORM_STATUS_UNSUPPORTED,
            "TidesDB column must belong to the selected table");
    return std::string(column.substr(separator + 1));
}

bool tidesdb_matches(const condition_node& root,
                     const tidesdb_row& row,
                     std::string_view table)
{
    return evaluate_group(root, row, table) == truth_value::true_value;
}

int tidesdb_compare_cells(const tidesdb_cell& left,
                          const tidesdb_cell& right)
{
    if (left.is_null || right.is_null)
        return left.is_null == right.is_null ? 0 : (left.is_null ? 1 : -1);
    if (left.kind == ORM_VALUE_TEXT || right.kind == ORM_VALUE_TEXT) {
        require(left.kind == ORM_VALUE_TEXT && right.kind == ORM_VALUE_TEXT,
                ORM_STATUS_TYPE_ERROR,
                "TidesDB cannot compare text and numeric values");
        return left.text < right.text ? -1 : (left.text > right.text ? 1 : 0);
    }
    require(is_numeric_kind(left.kind) && is_numeric_kind(right.kind),
            ORM_STATUS_TYPE_ERROR,
            "TidesDB comparison requires compatible value types");
    if (left.kind == ORM_VALUE_DOUBLE || right.kind == ORM_VALUE_DOUBLE) {
        const long double left_value = left.kind == ORM_VALUE_DOUBLE
            ? static_cast<long double>(parse_double(left.text))
            : (left.kind == ORM_VALUE_INT64
                   ? static_cast<long double>(parse_integer<std::int64_t>(left.text,
                                                                          "signed integer"))
                   : static_cast<long double>(left.kind == ORM_VALUE_BOOLEAN
                       ? static_cast<std::uint64_t>(left.text == "1")
                       : parse_integer<std::uint64_t>(left.text, "unsigned integer")));
        const long double right_value = right.kind == ORM_VALUE_DOUBLE
            ? static_cast<long double>(parse_double(right.text))
            : (right.kind == ORM_VALUE_INT64
                   ? static_cast<long double>(parse_integer<std::int64_t>(right.text,
                                                                          "signed integer"))
                   : static_cast<long double>(right.kind == ORM_VALUE_BOOLEAN
                       ? static_cast<std::uint64_t>(right.text == "1")
                       : parse_integer<std::uint64_t>(right.text, "unsigned integer")));
        return left_value < right_value ? -1 : (left_value > right_value ? 1 : 0);
    }
    return compare_integer_cells(left, right);
}

std::string tidesdb_group_key(const std::vector<tidesdb_cell>& values)
{
    std::string key;
    for (const tidesdb_cell& value : values) {
        validate_cell(value);
        const std::uint8_t kind = static_cast<std::uint8_t>(value.kind);
        key.push_back(static_cast<char>(kind));
        key.push_back(value.is_null ? '\1' : '\0');
        const std::uint32_t size = static_cast<std::uint32_t>(value.text.size());
        for (unsigned shift = 0; shift < 32; shift += 8)
            key.push_back(static_cast<char>(size >> shift));
        key.append(value.text);
    }
    return key;
}

} // namespace orm_c_detail
