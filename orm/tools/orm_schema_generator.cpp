#include "orm_schema_generator.hpp"

#include "orm_schema_template.hpp"

#include <mustache/mustache.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace orm::schema {
namespace {

using node_owner = std::unique_ptr<Node, decltype(&node_free)>;
using template_owner =
    std::unique_ptr<MUSTACHE_TEMPLATE, decltype(&mustache_release)>;

std::string cpp_string_literal(const std::string& input) {
  std::string output;
  output.reserve(input.size() + 2);
  output.push_back('"');
  for (const char value : input) {
    switch (value) {
      case '\\':
        output += "\\\\";
        break;
      case '"':
        output += "\\\"";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        output.push_back(value);
        break;
    }
  }
  output.push_back('"');
  return output;
}

struct generated_field {
  std::string member;
  std::string column;
  std::string expected_type;
  std::string enum_type;
  std::string enum_underlying_type;
  bool primary_key = false;
  bool version = false;
  bool checked_mapping = false;
  std::size_t flattened_count = 1;
};

struct generated_entity {
  std::string type_name;
  std::string base_type_name;
  std::string table_name;
  std::string discriminator_column;
  std::string discriminator_value;
  std::string inheritance_strategy;
  std::vector<generated_field> fields;
  std::vector<lifecycle_callback_model> lifecycle_callbacks;
  std::vector<std::string> primary_key_columns;
  std::vector<std::string> id_member_expressions;
  std::vector<std::pair<std::string, std::string>> discriminator_cases;
  std::vector<std::string> inheritance_tables;
  std::vector<std::string> column_tables;
};

struct generated_relation {
  std::string owner_type;
  std::string member;
  std::string target_type;
  std::string owner_key_member;
  std::string target_key_member;
  std::string target_key_column;
  std::string cascade;
  std::string fetch;
  std::size_t index = 0;
  bool collection = false;
  bool optional = false;
  bool orphan_removal = false;
};

enum class c_field_kind {
  signed_integer,
  unsigned_integer,
  boolean,
  floating32,
  floating64,
  string,
  bytes
};

struct c_type_info {
  c_field_kind kind;
  std::string c_type;
  std::string minimum;
  std::string maximum;
  std::string enum_name;
};

struct generated_c_entity {
  std::string type_name;
  std::string symbol;
  std::string table_literal;
  std::string primary_parameter;
  std::string primary_where_code;
  std::string entity_primary_where_code;
  std::string select_columns_code;
  std::string read_fields_code;
  std::string insert_fields_code;
  std::string update_fields_code;
  std::string version_overflow_code;
  std::string version_where_code;
  std::string version_remove_where_code;
  std::string version_commit_code;
  bool versioned = false;
};

std::string cpp_scalar_type(const std::string& type) {
  // Generated contracts must use TBE's canonical C++ spellings without
  // linking the tbe_compiler executable into the ORM build tool.
  static const std::unordered_map<std::string, std::string> mappings = {
      {"bool", "bool"},          {"byte", "std::uint8_t"},
      {"int8", "std::int8_t"},  {"int8_t", "std::int8_t"},
      {"i8", "std::int8_t"},    {"uint8", "std::uint8_t"},
      {"uint8_t", "std::uint8_t"}, {"u8", "std::uint8_t"},
      {"int16", "std::int16_t"}, {"int16_t", "std::int16_t"},
      {"i16", "std::int16_t"},  {"uint16", "std::uint16_t"},
      {"uint16_t", "std::uint16_t"}, {"u16", "std::uint16_t"},
      {"int32", "std::int32_t"}, {"int32_t", "std::int32_t"},
      {"i32", "std::int32_t"},  {"uint32", "std::uint32_t"},
      {"uint32_t", "std::uint32_t"}, {"u32", "std::uint32_t"},
      {"int64", "std::int64_t"}, {"int64_t", "std::int64_t"},
      {"i64", "std::int64_t"},  {"uint64", "std::uint64_t"},
      {"uint64_t", "std::uint64_t"}, {"u64", "std::uint64_t"},
      {"float", "float"},       {"double", "double"},
      {"string", "std::string"},
      {"bytes", "std::vector<std::uint8_t>"}};
  const auto found = mappings.find(type);
  return found == mappings.end() ? type : found->second;
}

bool is_c_identifier(const std::string& value) {
  static const std::unordered_map<std::string, bool> keywords = {
      {"auto", true},       {"break", true},      {"case", true},
      {"char", true},       {"const", true},      {"continue", true},
      {"default", true},    {"do", true},         {"double", true},
      {"else", true},       {"enum", true},       {"extern", true},
      {"float", true},      {"for", true},        {"goto", true},
      {"if", true},         {"inline", true},     {"int", true},
      {"long", true},       {"register", true},   {"restrict", true},
      {"return", true},     {"short", true},      {"signed", true},
      {"sizeof", true},     {"static", true},     {"struct", true},
      {"switch", true},     {"typedef", true},    {"union", true},
      {"unsigned", true},   {"void", true},       {"volatile", true},
      {"while", true},      {"_Alignas", true},   {"_Alignof", true},
      {"_Atomic", true},    {"_Bool", true},      {"_Complex", true},
      {"_Generic", true},   {"_Imaginary", true}, {"_Noreturn", true},
      {"_Static_assert", true}, {"_Thread_local", true}};
  if (value.empty() || keywords.find(value) != keywords.end()) return false;
  const auto first = static_cast<unsigned char>(value.front());
  if (std::isalpha(first) == 0 && first != '_') return false;
  for (const unsigned char character : value) {
    if (std::isalnum(character) == 0 && character != '_') return false;
  }
  return true;
}

std::optional<c_type_info> c_integer_type(const std::string& type) {
  static const std::unordered_map<std::string, c_type_info> mappings = {
      {"int8", {c_field_kind::signed_integer, "int8_t", "INT8_MIN", "INT8_MAX", ""}},
      {"int8_t", {c_field_kind::signed_integer, "int8_t", "INT8_MIN", "INT8_MAX", ""}},
      {"i8", {c_field_kind::signed_integer, "int8_t", "INT8_MIN", "INT8_MAX", ""}},
      {"byte", {c_field_kind::unsigned_integer, "uint8_t", "0", "UINT8_MAX", ""}},
      {"uint8", {c_field_kind::unsigned_integer, "uint8_t", "0", "UINT8_MAX", ""}},
      {"uint8_t", {c_field_kind::unsigned_integer, "uint8_t", "0", "UINT8_MAX", ""}},
      {"u8", {c_field_kind::unsigned_integer, "uint8_t", "0", "UINT8_MAX", ""}},
      {"int16", {c_field_kind::signed_integer, "int16_t", "INT16_MIN", "INT16_MAX", ""}},
      {"int16_t", {c_field_kind::signed_integer, "int16_t", "INT16_MIN", "INT16_MAX", ""}},
      {"i16", {c_field_kind::signed_integer, "int16_t", "INT16_MIN", "INT16_MAX", ""}},
      {"uint16", {c_field_kind::unsigned_integer, "uint16_t", "0", "UINT16_MAX", ""}},
      {"uint16_t", {c_field_kind::unsigned_integer, "uint16_t", "0", "UINT16_MAX", ""}},
      {"u16", {c_field_kind::unsigned_integer, "uint16_t", "0", "UINT16_MAX", ""}},
      {"int32", {c_field_kind::signed_integer, "int32_t", "INT32_MIN", "INT32_MAX", ""}},
      {"int32_t", {c_field_kind::signed_integer, "int32_t", "INT32_MIN", "INT32_MAX", ""}},
      {"i32", {c_field_kind::signed_integer, "int32_t", "INT32_MIN", "INT32_MAX", ""}},
      {"uint32", {c_field_kind::unsigned_integer, "uint32_t", "0", "UINT32_MAX", ""}},
      {"uint32_t", {c_field_kind::unsigned_integer, "uint32_t", "0", "UINT32_MAX", ""}},
      {"u32", {c_field_kind::unsigned_integer, "uint32_t", "0", "UINT32_MAX", ""}},
      {"int64", {c_field_kind::signed_integer, "int64_t", "INT64_MIN", "INT64_MAX", ""}},
      {"int64_t", {c_field_kind::signed_integer, "int64_t", "INT64_MIN", "INT64_MAX", ""}},
      {"i64", {c_field_kind::signed_integer, "int64_t", "INT64_MIN", "INT64_MAX", ""}},
      {"uint64", {c_field_kind::unsigned_integer, "uint64_t", "0", "UINT64_MAX", ""}},
      {"uint64_t", {c_field_kind::unsigned_integer, "uint64_t", "0", "UINT64_MAX", ""}},
      {"u64", {c_field_kind::unsigned_integer, "uint64_t", "0", "UINT64_MAX", ""}}};
  const auto found = mappings.find(type);
  if (found == mappings.end()) return std::nullopt;
  return found->second;
}

std::optional<c_type_info> c_type_for(const field_model& field,
                                      const schema_model& schema) {
  if (auto integer = c_integer_type(field.type)) return integer;
  if (field.type == "bool")
    return c_type_info{c_field_kind::boolean, "uint8_t", "0", "1", ""};
  if (field.type == "float")
    return c_type_info{c_field_kind::floating32, "float", "-FLT_MAX", "FLT_MAX", ""};
  if (field.type == "double")
    return c_type_info{c_field_kind::floating64, "double", "", "", ""};
  if (field.type == "string")
    return c_type_info{c_field_kind::string, "tstr", "", "", ""};
  if (field.type == "bytes")
    return c_type_info{c_field_kind::bytes, "tbe_bytes_t", "", "", ""};
  for (const enum_model& value : schema.enums) {
    if (value.name != field.type) continue;
    auto underlying = c_integer_type(value.underlying_type);
    if (!underlying) return std::nullopt;
    underlying->c_type = value.name + "_t";
    underlying->enum_name = value.name;
    return underlying;
  }
  return std::nullopt;
}

std::string c_entity_value_expression(const c_type_info& type,
                                      const std::string& expression) {
  switch (type.kind) {
    case c_field_kind::signed_integer:
      return "orm_i64((int64_t)(" + expression + "))";
    case c_field_kind::unsigned_integer:
      return "orm_u64((uint64_t)(" + expression + "))";
    case c_field_kind::boolean:
      return "orm_bool((" + expression + ") != 0u)";
    case c_field_kind::floating32:
    case c_field_kind::floating64:
      return "orm_f64((double)(" + expression + "))";
    case c_field_kind::string:
      return "orm_text_tstr(" + expression + ")";
    case c_field_kind::bytes:
      return "orm_blob(tbe_bytes_t_data_const(&(" + expression + ")), "
             "tbe_bytes_t_size(&(" + expression + ")))";
  }
  return "orm_null()";
}

std::string c_parameter_type(const c_type_info& type) {
  if (type.kind == c_field_kind::string) return "vstr";
  if (type.kind == c_field_kind::bytes) return "orm_blob_t";
  return type.c_type;
}

std::string c_parameter_value_expression(const c_type_info& type,
                                         const std::string& expression) {
  if (type.kind == c_field_kind::string)
    return "orm_text_v(" + expression + ")";
  if (type.kind == c_field_kind::bytes)
    return "orm_blob(" + expression + ".data, " + expression + ".size)";
  return c_entity_value_expression(type, expression);
}

std::string c_presence_expression(const std::string& owner,
                                  const field_model& field,
                                  const std::string& object) {
  const std::string bit = owner + "_OPTIONAL_" + field.name;
  return "((" + object + "->_presence[(size_t)" + bit +
         " / 8u] & (uint8_t)(1u << ((unsigned int)" + bit +
         " % 8u))) != 0u)";
}

std::string c_set_presence_statement(const std::string& owner,
                                     const field_model& field,
                                     const std::string& object) {
  const std::string bit = owner + "_OPTIONAL_" + field.name;
  return "  " + object + "._presence[(size_t)" + bit +
         " / 8u] |= (uint8_t)(1u << ((unsigned int)" + bit + " % 8u));\n";
}

std::unordered_map<std::string, std::string> read_enum_types(
    const schema_model& schema) {
  std::unordered_map<std::string, std::string> result;
  for (const enum_model& value : schema.enums) {
    result.emplace(value.name, cpp_scalar_type(value.underlying_type));
  }
  return result;
}

std::string c_read_value_statement(const std::string& schema_name,
                                   const std::string& owner,
                                   const field_model& field,
                                   const c_type_info& type,
                                   std::size_t column) {
  const std::string destination = "temporary." + field.c_member;
  const std::string column_text = std::to_string(column) + "u";
  std::string code = "  {\n";
  switch (type.kind) {
    case c_field_kind::signed_integer:
      code += "    int64_t value = 0;\n"
              "    status = orm_result_get_int64(result, 0u, " +
              column_text + ", &value, error);\n"
              "    if (status != ORM_STATUS_OK) goto cleanup;\n";
      if (type.c_type != "int64_t") {
        code += "    if (value < " + type.minimum + " || value > " + type.maximum + ") {\n"
                "      status = " + schema_name +
                "_orm_generated_error(error, ORM_STATUS_OUT_OF_RANGE, "
                "\"ORM generated signed field is outside its C destination range\");\n"
                "      goto cleanup;\n"
                "    }\n";
      }
      code += "    " + destination + " = (" + type.c_type + ")value;\n";
      break;
    case c_field_kind::unsigned_integer:
      code += "    uint64_t value = 0u;\n"
              "    status = orm_result_get_uint64(result, 0u, " +
              column_text + ", &value, error);\n"
              "    if (status != ORM_STATUS_OK) goto cleanup;\n";
      if (type.maximum != "UINT64_MAX") {
        code += "    if (value > " + type.maximum + ") {\n"
                "      status = " + schema_name +
                "_orm_generated_error(error, ORM_STATUS_OUT_OF_RANGE, "
                "\"ORM generated unsigned field is outside its C destination range\");\n"
                "      goto cleanup;\n"
                "    }\n";
      }
      code += "    " + destination + " = (" + type.c_type + ")value;\n";
      break;
    case c_field_kind::boolean:
      code += "    status = orm_result_get_boolean(result, 0u, " + column_text +
              ", &" + destination + ", error);\n"
              "    if (status != ORM_STATUS_OK) goto cleanup;\n";
      break;
    case c_field_kind::floating32:
      code += "    double value = 0.0;\n"
              "    status = orm_result_get_double(result, 0u, " + column_text +
              ", &value, error);\n"
              "    if (status != ORM_STATUS_OK) goto cleanup;\n"
              "    if (value < -FLT_MAX || value > FLT_MAX) {\n"
              "      status = " + schema_name +
              "_orm_generated_error(error, ORM_STATUS_OUT_OF_RANGE, "
              "\"ORM generated float field is outside its C destination range\");\n"
              "      goto cleanup;\n"
              "    }\n"
              "    " + destination + " = (float)value;\n";
      break;
    case c_field_kind::floating64:
      code += "    status = orm_result_get_double(result, 0u, " + column_text +
              ", &" + destination + ", error);\n"
              "    if (status != ORM_STATUS_OK) goto cleanup;\n";
      break;
    case c_field_kind::string:
      code += "    orm_string_view_t value;\n"
              "    tstr copied;\n"
              "    status = orm_result_get_text(result, 0u, " + column_text +
              ", &value, error);\n"
              "    if (status != ORM_STATUS_OK) goto cleanup;\n"
              "    copied = tstr_from_v(value);\n"
              "    if (copied == NULL) {\n"
              "      status = " + schema_name +
              "_orm_generated_error(error, ORM_STATUS_OUT_OF_MEMORY, "
              "\"ORM generated string copy failed\");\n"
              "      goto cleanup;\n"
              "    }\n"
              "    tstr_free(" + destination + ");\n"
              "    " + destination + " = copied;\n";
      break;
    case c_field_kind::bytes:
      code += "    orm_blob_t value;\n"
              "    status = orm_result_get_blob(result, 0u, " + column_text +
              ", &value, error);\n"
              "    if (status != ORM_STATUS_OK) goto cleanup;\n"
              "    if (turbo_vec_resize(&" + destination + ".raw, value.size) != TURBO_OK) {\n"
              "      status = " + schema_name +
              "_orm_generated_error(error, ORM_STATUS_OUT_OF_MEMORY, "
              "\"ORM generated bytes copy failed\");\n"
              "      goto cleanup;\n"
              "    }\n"
              "    if (value.size != 0u)\n"
              "      memcpy(tbe_bytes_t_data(&" + destination + "), value.data, value.size);\n";
      break;
  }
  if (!type.enum_name.empty()) {
    code += "    if (!" + type.enum_name + "_is_valid(" + destination + ")) {\n"
            "      status = " + schema_name +
            "_orm_generated_error(error, ORM_STATUS_TYPE_ERROR, "
            "\"ORM generated enum field contains an invalid value\");\n"
            "      goto cleanup;\n"
            "    }\n";
  }
  code += "  }\n";

  if (!field.optional) return code;
  std::string optional;
  optional += "  {\n"
              "    uint8_t is_null = 0u;\n"
              "    status = orm_result_is_null(result, 0u, " + column_text +
              ", &is_null, error);\n"
              "    if (status != ORM_STATUS_OK) goto cleanup;\n"
              "    if (is_null == 0u) {\n";
  for (const char value : code) {
    optional.push_back(value);
    if (value == '\n') optional += "  ";
  }
  optional += c_set_presence_statement(owner, field, "temporary");
  optional += "    }\n"
              "  }\n";
  return optional;
}

std::string c_set_statement(const std::string& owner,
                            const field_model& field,
                            const c_type_info& type,
                            const std::string& value_expression) {
  std::string value = c_entity_value_expression(type, value_expression);
  if (field.optional) {
    value = "(" + c_presence_expression(owner, field, "entity") + " ? " +
            value + " : orm_null())";
  }
  return "  status = orm_query_set(query, orm_view(" +
         cpp_string_literal(field.column) + "), " + value + ", error);\n"
         "  if (status != ORM_STATUS_OK) goto cleanup;\n";
}

std::string c_where_statement(const field_model& field,
                              const c_type_info& type,
                              const std::string& value_expression) {
  return "  status = orm_query_where(query, orm_view(" +
         cpp_string_literal(field.column) + "), ORM_COMPARE_EQUAL, " +
         c_entity_value_expression(type, value_expression) + ", error);\n"
         "  if (status != ORM_STATUS_OK) goto cleanup;\n";
}

std::string c_parameter_where_statement(const field_model& field,
                                        const c_type_info& type,
                                        const std::string& parameter) {
  return "  status = orm_query_where(query, orm_view(" +
         cpp_string_literal(field.column) + "), ORM_COMPARE_EQUAL, " +
         c_parameter_value_expression(type, parameter) + ", error);\n"
         "  if (status != ORM_STATUS_OK) goto cleanup;\n";
}

bool make_generated_c_entity(const entity_model& entity,
                             const schema_model& schema,
                             generated_c_entity& output,
                             std::vector<diagnostic>& diagnostics) {
  output = {};
  output.type_name = entity.name;
  output.symbol = schema.name + "_" + entity.name + "_orm";
  output.table_literal = cpp_string_literal(entity.table);
  const field_model* version = nullptr;
  std::vector<std::pair<const field_model*, c_type_info>> fields;
  std::vector<std::pair<const field_model*, const c_type_info*>> primaries;
  for (const field_model& field : entity.fields) {
    if (field.relation) continue;
    if (field.embedded) {
      diagnostics.push_back(
          {entity.name, field.name,
           "ORM C facade does not support embedded value fields"});
      return false;
    }
    if (!is_c_identifier(field.c_member)) {
      diagnostics.push_back(
          {entity.name, field.name, "ORM C member must be a valid C identifier"});
      return false;
    }
    if (!field.collection_kind.empty()) {
      diagnostics.push_back({entity.name, field.name,
                             "ORM C columns cannot use collection types"});
      return false;
    }
    auto type = c_type_for(field, schema);
    if (!type) {
      diagnostics.push_back(
          {entity.name, field.name,
           "ORM C facade does not support this column type: " + field.type});
      return false;
    }
    fields.emplace_back(&field, std::move(*type));
    if (field.version) version = &field;
  }
  for (const auto& item : fields) {
    if (item.first->primary_key)
      primaries.emplace_back(item.first, &item.second);
  }
  if (primaries.empty()) {
    diagnostics.push_back(
        {entity.name, "",
         "ORM C facade requires at least one direct primary-key field"});
    return false;
  }

  for (const auto& primary : primaries) {
    const field_model& field = *primary.first;
    const c_type_info& type = *primary.second;
    const std::string parameter = "primary_" + field.c_member;
    if (!output.primary_parameter.empty()) output.primary_parameter += ", ";
    output.primary_parameter += c_parameter_type(type) + " " + parameter;
    output.primary_where_code +=
        c_parameter_where_statement(field, type, parameter);
    output.entity_primary_where_code += c_where_statement(
        field, type, "entity->" + field.c_member);
  }

  for (std::size_t index = 0; index < fields.size(); ++index) {
    const field_model& field = *fields[index].first;
    const c_type_info& type = fields[index].second;
    output.select_columns_code +=
        "  status = orm_query_add_column(query, orm_view(" +
        cpp_string_literal(field.column) + "), error);\n"
        "  if (status != ORM_STATUS_OK) goto cleanup;\n";
    output.read_fields_code +=
        c_read_value_statement(schema.name, entity.name, field, type, index);
    output.insert_fields_code += c_set_statement(
        entity.name, field, type, "entity->" + field.c_member);
    if (!field.primary_key && !field.version) {
      output.update_fields_code += c_set_statement(
          entity.name, field, type, "entity->" + field.c_member);
    }
  }

  if (version != nullptr) {
    const auto version_it = std::find_if(
        fields.begin(), fields.end(),
        [&](const auto& item) { return item.first == version; });
    if (version_it == fields.end()) return false;
    const c_type_info& type = version_it->second;
    const std::string member = "entity->" + version->c_member;
    const std::string incremented = "(" + type.c_type + ")(" + member + " + 1)";
    output.versioned = true;
    output.version_overflow_code =
        "  if (" + member + " == " + type.maximum + ")\n"
        "    return " + schema.name +
        "_orm_generated_error(error, ORM_STATUS_OUT_OF_RANGE, "
        "\"ORM entity version overflow\");\n";
    output.update_fields_code +=
        c_set_statement(entity.name, *version, type, incremented);
    output.version_where_code = c_where_statement(*version, type, member);
    output.version_remove_where_code = output.version_where_code;
    output.version_commit_code = "  " + member + " = " + incremented + ";\n";
  }
  return true;
}

std::string cascade_policy_expression(const generated_relation& relation) {
  const std::string cascade = relation.cascade.empty() ? "none" : relation.cascade;
  std::string expression = "orm::relation::cascade_policy::" + cascade;
  if (relation.orphan_removal && cascade != "orphan_remove") {
    expression += " | orm::relation::cascade_policy::orphan_remove";
  }
  return expression;
}

generated_entity make_generated_entity(
    const entity_model& entity, const schema_model& schema,
    const std::unordered_map<std::string, std::string>& enum_types,
    std::vector<generated_relation>& relations) {
  generated_entity normalized;
  normalized.type_name = entity.name;
  normalized.base_type_name = entity.base_entity;
  std::vector<const entity_model*> hierarchy;
  const entity_model* current = &entity;
  while (current != nullptr) {
    hierarchy.push_back(current);
    current = current->extends_declared
                  ? schema.find_entity(current->base_entity)
                  : nullptr;
  }
  std::reverse(hierarchy.begin(), hierarchy.end());
  const entity_model& root = *hierarchy.front();
  normalized.inheritance_strategy = root.inheritance;
  normalized.table_name = root.inheritance == "table_per_class"
                              ? entity.table
                              : root.table;
  if (root.inheritance == "single_table" || root.inheritance == "joined") {
    normalized.discriminator_column = root.discriminator_column;
    normalized.discriminator_value = entity.discriminator_value;
  }
  if (root.inheritance == "joined") {
    for (const entity_model* level : hierarchy) {
      normalized.inheritance_tables.push_back(level->table);
    }
  } else {
    normalized.inheritance_tables.push_back(normalized.table_name);
  }
  for (const entity_model* level : hierarchy) {
    normalized.lifecycle_callbacks.insert(normalized.lifecycle_callbacks.end(),
                                          level->lifecycle_callbacks.begin(),
                                          level->lifecycle_callbacks.end());
  }
  if (!root.inheritance.empty() && &entity == &root) {
    for (const entity_model& candidate : schema.entities) {
      const entity_model* candidate_root = &candidate;
      std::size_t depth = 0;
      while (candidate_root->extends_declared &&
             depth++ <= schema.entities.size()) {
        candidate_root = schema.find_entity(candidate_root->base_entity);
        if (candidate_root == nullptr) break;
      }
      if (candidate_root == &root) {
        normalized.discriminator_cases.emplace_back(
            candidate.name, candidate.discriminator_value);
      }
    }
  }
  std::size_t relation_index = 0;
  for (const entity_model* level : hierarchy) {
  for (const field_model& field : level->fields) {
    if (field.relation) {
      generated_relation relation;
      relation.owner_type = normalized.type_name;
      relation.member = field.name;
      relation.target_type = field.relation_target;
      relation.collection = field.collection_kind == "list";
      relation.optional = field.optional;
      relation.cascade = field.cascade;
      relation.fetch = field.fetch;
      relation.index = relation_index++;
      relation.orphan_removal = field.orphan_removal;

      const std::string& mapping =
          !field.mapped_by.empty() ? field.mapped_by : field.foreign_key;
      if (!mapping.empty()) {
        const entity_model* target = schema.find_entity(relation.target_type);
        const field_model* target_field = nullptr;
        for (const entity_model* target_level = target;
             target_level != nullptr && target_field == nullptr;) {
          target_field = target_level->find_field(mapping);
          target_level = target_level->extends_declared
                             ? schema.find_entity(target_level->base_entity)
                             : nullptr;
        }
        for (const entity_model* owner_level : hierarchy) {
          for (const field_model& owner_field : owner_level->fields) {
            if (owner_field.primary_key) {
              relation.owner_key_member = owner_field.name;
              break;
            }
          }
          if (!relation.owner_key_member.empty()) break;
        }
        if (target_field != nullptr) {
          relation.target_key_member = target_field->name;
          relation.target_key_column = target_field->column;
        }
      }
      relations.push_back(std::move(relation));
      continue;
    }

    generated_field generated;
    generated.member = field.name;
    const auto enum_mapping = enum_types.find(field.type);
    const bool enum_field = enum_mapping != enum_types.end();
    if (field.optional || enum_field) {
      generated.expected_type = cpp_scalar_type(field.type);
      if (field.optional) {
        generated.expected_type = "std::optional<" + generated.expected_type + ">";
      }
      generated.checked_mapping = true;
    }
    if (enum_field) {
      generated.enum_type = field.type;
      generated.enum_underlying_type = enum_mapping->second;
    }
    generated.column = field.column;
    generated.primary_key = field.primary_key;
    generated.version = field.version;
    if (field.embedded) {
      generated.expected_type = field.type;
      generated.checked_mapping = true;
      generated.flattened_count = 0;
      const entity_model* embedded = schema.find_entity(field.type);
      if (embedded != nullptr) {
        for (const field_model& component : embedded->fields) {
          if (!component.relation && !component.embedded)
            ++generated.flattened_count;
        }
      }
    }
    if (field.primary_key) {
      normalized.primary_key_columns.push_back(field.column);
      normalized.id_member_expressions.push_back("entity." + field.name);
    } else if (field.embedded_id) {
      const entity_model* embedded = schema.find_entity(field.type);
      if (embedded != nullptr) {
        for (const field_model& component : embedded->fields) {
          if (component.relation || component.embedded) continue;
          normalized.primary_key_columns.push_back(component.column);
          normalized.id_member_expressions.push_back(
              "entity." + field.name + "." + component.name);
        }
      }
    }
    const std::string& column_table =
        root.inheritance == "joined"
            ? level->table
            : normalized.table_name;
    for (std::size_t index = 0; index < generated.flattened_count; ++index) {
      normalized.column_tables.push_back(column_table);
    }
    normalized.fields.push_back(std::move(generated));
  }
  }
  return normalized;
}

void add_owned_child(Node* parent, node_owner child) {
  if (parent == nullptr || child == nullptr) {
    throw std::bad_alloc();
  }
  const int result = parent->type == NODE_LIST ? list_add(parent, child.get())
                                               : map_add(parent, child.get());
  if (result != 0) {
    throw std::bad_alloc();
  }
  (void)child.release();
}

void add_string(Node* parent, const char* name, const std::string& value) {
  add_owned_child(parent, node_owner(create_node_string(name, value.c_str()), &node_free));
}

Node* add_map(Node* parent, const char* name) {
  node_owner child(create_node_map(name), &node_free);
  Node* const result = child.get();
  add_owned_child(parent, std::move(child));
  return result;
}

Node* add_list(Node* parent, const char* name) {
  node_owner child(create_node_list(name), &node_free);
  Node* const result = child.get();
  add_owned_child(parent, std::move(child));
  return result;
}

std::string join_field_values(const std::vector<generated_field>& fields,
                              std::string_view prefix, bool use_column) {
  std::string result;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) {
      result += ", ";
    }
    result += prefix;
    result += use_column ? cpp_string_literal(fields[i].column) : fields[i].member;
  }
  return result;
}

std::string field_names_literal(const std::vector<generated_field>& fields) {
  std::string names;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i != 0) {
      names.push_back(',');
    }
    names += fields[i].member;
  }
  return cpp_string_literal(names);
}

std::string join_string_literals(const std::vector<std::string>& values) {
  std::string result;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) result += ", ";
    result += cpp_string_literal(values[index]);
  }
  return result;
}

std::string join_values(const std::vector<std::string>& values) {
  std::string result;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) result += ", ";
    result += values[index];
  }
  return result;
}

void add_relation_context(Node* relations, const generated_relation& relation);

void add_entity_context(Node* entities, const generated_entity& entity,
                        const std::vector<generated_relation>& relations) {
  Node* item = add_map(entities, nullptr);
  add_string(item, "type_name", entity.type_name);
  if (!entity.base_type_name.empty()) {
    Node* inheritance_check = add_list(item, "inheritance_check");
    Node* check = add_map(inheritance_check, nullptr);
    add_string(check, "base_type", entity.base_type_name);
    add_string(check, "derived_type", entity.type_name);
  }
  add_string(item, "field_count", std::to_string(entity.fields.size()));
  add_string(item, "member_pointers",
             join_field_values(entity.fields, "&" + entity.type_name + "::", false));
  add_string(item, "column_literals", join_field_values(entity.fields, "", true));
  add_string(item, "field_names_literal", field_names_literal(entity.fields));
  add_string(item, "table_literal", cpp_string_literal(entity.table_name));
  Node* mapping_checks = add_list(item, "mapping_checks");
  for (const generated_field& field : entity.fields) {
    if (!field.checked_mapping) {
      continue;
    }
    Node* check = add_map(mapping_checks, nullptr);
    add_string(check, "member_type", "decltype(" + entity.type_name + "::" +
                                         field.member + ")");
    add_string(check, "expected_type", field.expected_type);
    add_string(check, "message_literal",
               cpp_string_literal("ORM schema mapping type mismatch for " +
                                  entity.type_name + "." + field.member));
    if (!field.enum_type.empty()) {
      Node* enum_checks = add_list(check, "enum_check");
      Node* enum_check = add_map(enum_checks, nullptr);
      add_string(enum_check, "enum_type", field.enum_type);
      add_string(enum_check, "enum_underlying_type", field.enum_underlying_type);
      add_string(enum_check, "enum_message_literal",
                 cpp_string_literal("ORM schema enum underlying type mismatch for " +
                                    field.enum_type));
    }
  }

  std::string primary_key;
  std::string version;
  for (const generated_field& field : entity.fields) {
    if (field.version) {
      version = field.column;
    }
  }
  if (entity.primary_key_columns.size() == 1) {
    primary_key = entity.primary_key_columns.front();
  }
  add_string(item, "primary_key_literal", cpp_string_literal(primary_key));
  add_string(item, "primary_key_count",
             std::to_string(entity.primary_key_columns.size()));
  add_string(item, "primary_key_literals",
             join_string_literals(entity.primary_key_columns));
  add_string(item, "version_literal", cpp_string_literal(version));
  if (!entity.inheritance_strategy.empty()) {
    Node* inheritance = add_list(item, "inheritance_strategy");
    Node* inheritance_item = add_map(inheritance, nullptr);
    add_string(inheritance_item, "strategy", entity.inheritance_strategy);
    add_string(inheritance_item, "table_count",
               std::to_string(entity.inheritance_tables.size()));
    add_string(inheritance_item, "table_literals",
               join_string_literals(entity.inheritance_tables));
    add_string(inheritance_item, "column_table_count",
               std::to_string(entity.column_tables.size()));
    add_string(inheritance_item, "column_table_literals",
               join_string_literals(entity.column_tables));
  }
  if (!entity.discriminator_column.empty()) {
    Node* discriminator = add_list(item, "discriminator");
    Node* discriminator_item = add_map(discriminator, nullptr);
    add_string(discriminator_item, "column_literal",
               cpp_string_literal(entity.discriminator_column));
    add_string(discriminator_item, "value_literal",
               cpp_string_literal(entity.discriminator_value));
  }
  if (!entity.discriminator_cases.empty()) {
    Node* hierarchy = add_list(item, "inheritance_hierarchy");
    Node* hierarchy_item = add_map(hierarchy, nullptr);
    std::vector<std::string> variant_types;
    std::vector<std::string> case_expressions;
    for (const auto& entry : entity.discriminator_cases) {
      variant_types.push_back(entry.first);
      case_expressions.push_back("orm::model::discriminator_case<" +
                                 entry.first + ">{" +
                                 cpp_string_literal(entry.second) + "}");
    }
    add_string(hierarchy_item, "variant_types", join_values(variant_types));
    add_string(hierarchy_item, "case_expressions",
               join_values(case_expressions));
  }

  if (!entity.id_member_expressions.empty()) {
    Node* entity_id = add_list(item, "entity_id");
    Node* id_item = add_map(entity_id, nullptr);
    add_string(id_item, "type_name", entity.type_name);
    add_string(id_item, "id_members",
               join_values(entity.id_member_expressions));
  }

  if (!entity.lifecycle_callbacks.empty()) {
    Node* lifecycle = add_list(item, "lifecycle");
    Node* lifecycle_item = add_map(lifecycle, nullptr);
    add_string(lifecycle_item, "type_name", entity.type_name);
    Node* callbacks = add_list(lifecycle_item, "lifecycle_callbacks");
    std::vector<std::pair<std::string, std::vector<std::string>>> grouped;
    for (const lifecycle_callback_model& callback :
         entity.lifecycle_callbacks) {
      auto found = std::find_if(
          grouped.begin(), grouped.end(), [&](const auto& item) {
            return item.first == callback.event;
          });
      if (found == grouped.end()) {
        grouped.push_back({callback.event, {callback.method}});
      } else {
        found->second.push_back(callback.method);
      }
    }
    for (const auto& callback : grouped) {
      Node* callback_item = add_map(callbacks, nullptr);
      add_string(callback_item, "event", callback.first);
      Node* methods = add_list(callback_item, "methods");
      for (const std::string& method : callback.second) {
        Node* method_item = add_map(methods, nullptr);
        add_string(method_item, "method", method);
      }
    }
  }

  Node* relation_items = add_list(item, "entity_relations");
  Node* relation_dispatch = add_list(item, "relation_dispatch");
  Node* loadable_many = add_list(item, "loadable_many");
  Node* loadable_optional_one = add_list(item, "loadable_optional_one");
  Node* loadable_required_one = add_list(item, "loadable_required_one");
  std::string relation_factories;
  std::string eager_relation_factories;
  std::uint64_t all_relation_mask = 0;
  std::uint64_t eager_relation_mask = 0;
  std::uint64_t loadable_relation_mask = 0;
  for (const generated_relation& relation : relations) {
    if (relation.owner_type != entity.type_name) {
      continue;
    }
    if (!relation_factories.empty()) {
      relation_factories += ", ";
    }
    relation_factories += relation.member + "()";
    add_relation_context(relation_items, relation);
    const std::uint64_t relation_mask = std::uint64_t{1} << relation.index;
    all_relation_mask |= relation_mask;
    const std::string mask_expression =
        "std::uint64_t{1} << " + std::to_string(relation.index);
    Node* graph = add_map(relation_dispatch, nullptr);
    add_string(graph, "member", relation.member);
    add_string(graph, "mask_expression", mask_expression);
    if (!relation.fetch.empty()) {
      loadable_relation_mask |= relation_mask;
      Node* loaders = relation.collection
                          ? loadable_many
                          : (relation.optional ? loadable_optional_one
                                               : loadable_required_one);
      Node* loader = add_map(loaders, nullptr);
      add_string(loader, "member", relation.member);
      add_string(loader, "target_type", relation.target_type);
      add_string(loader, "owner_key_member", relation.owner_key_member);
      add_string(loader, "target_key_column_literal",
                 cpp_string_literal(relation.target_key_column));
      add_string(loader, "missing_message_literal",
                 cpp_string_literal("ORM relation " + relation.owner_type + "." +
                                    relation.member + " is missing"));
    }
    if (relation.fetch != "eager") {
      continue;
    }
    eager_relation_mask |= relation_mask;
    if (!eager_relation_factories.empty()) {
      eager_relation_factories += ", ";
    }
    eager_relation_factories += relation.member + "()";
  }
  add_string(item, "relation_factories", relation_factories);
  add_string(item, "eager_relation_factories", eager_relation_factories);
  add_string(item, "all_relation_mask",
             std::to_string(all_relation_mask) + "ULL");
  add_string(item, "eager_relation_mask",
             std::to_string(eager_relation_mask) + "ULL");
  add_string(item, "loadable_relation_mask",
             std::to_string(loadable_relation_mask) + "ULL");
}

void add_relation_context(Node* relations, const generated_relation& relation) {
  Node* item = add_map(relations, nullptr);
  add_string(item, "symbol", relation.member);
  add_string(item, "cardinality", relation.collection ? "many" : "one");
  add_string(item, "owner_type", relation.owner_type);
  add_string(item, "member", relation.member);
  add_string(item, "mask_expression",
             "std::uint64_t{1} << " + std::to_string(relation.index));
  add_string(item, "cascade_expression", cascade_policy_expression(relation));
  if (!relation.owner_key_member.empty()) {
    Node* mapping = add_list(item, "mapping");
    Node* mapping_item = add_map(mapping, nullptr);
    add_string(mapping_item, "owner_type", relation.owner_type);
    add_string(mapping_item, "owner_key_member", relation.owner_key_member);
    add_string(mapping_item, "target_type", relation.target_type);
    add_string(mapping_item, "target_key_member", relation.target_key_member);
  }
  if (!relation.fetch.empty()) {
    Node* initializers = add_list(item, "initializer");
    Node* initializer = add_map(initializers, nullptr);
    add_string(initializer, "symbol", "initialize_" + relation.member);
    add_string(initializer, "owner_type", relation.owner_type);
    add_string(initializer, "graph_symbol", relation.member + "_graph");
    Node* accessors = add_list(item, "fetch_accessor");
    Node* accessor = add_map(accessors, nullptr);
    add_string(accessor, "symbol", "fetch_" + relation.member);
    add_string(accessor, "owner_type", relation.owner_type);
    add_string(accessor, "owner_key_member", relation.owner_key_member);
    add_string(accessor, "target_type", relation.target_type);
    add_string(accessor, "target_key_column_literal",
               cpp_string_literal(relation.target_key_column));
    const std::string operation = relation.fetch == "lazy"
                                      ? (relation.collection ? "defer_many" : "defer_one")
                                      : (relation.collection ? "find_many" : "find_one");
    add_string(accessor, "operation", operation);
  }
}

node_owner build_render_context(const schema_model& schema,
                                const std::vector<generated_entity>& entities,
                                const std::vector<generated_relation>& relations) {
  node_owner root(create_node_map("orm_metadata"), &node_free);
  if (root == nullptr) {
    throw std::bad_alloc();
  }
  if (schema.cpp_namespace_declared) {
    Node* scopes = add_list(root.get(), "namespace_scope");
    Node* scope = add_map(scopes, nullptr);
    add_string(scope, "name", schema.cpp_namespace);
  }
  Node* entity_items = add_list(root.get(), "entities");
  for (const generated_entity& entity : entities) {
    add_entity_context(entity_items, entity, relations);
  }
  return root;
}

node_owner build_c_render_context(
    const schema_model& schema, std::string_view model_header,
    const std::vector<generated_c_entity>& entities) {
  node_owner root(create_node_map("orm_c_facade"), &node_free);
  if (root == nullptr) throw std::bad_alloc();
  std::string include_guard = "ORM_GENERATED_" + schema.name + "_C_FACADE_H";
  for (char& value : include_guard) {
    const auto character = static_cast<unsigned char>(value);
    if (std::isalnum(character) != 0)
      value = static_cast<char>(std::toupper(character));
    else
      value = '_';
  }
  add_string(root.get(), "include_guard", include_guard);
  add_string(root.get(), "schema_name", schema.name);
  add_string(root.get(), "model_header_literal",
             cpp_string_literal(std::string(model_header)));
  Node* entity_items = add_list(root.get(), "entities");
  for (const generated_c_entity& entity : entities) {
    Node* item = add_map(entity_items, nullptr);
    add_string(item, "type_name", entity.type_name);
    add_string(item, "symbol", entity.symbol);
    add_string(item, "table_literal", entity.table_literal);
    add_string(item, "primary_parameter", entity.primary_parameter);
    add_string(item, "primary_where_code", entity.primary_where_code);
    add_string(item, "entity_primary_where_code",
               entity.entity_primary_where_code);
    add_string(item, "select_columns_code", entity.select_columns_code);
    add_string(item, "read_fields_code", entity.read_fields_code);
    add_string(item, "insert_fields_code", entity.insert_fields_code);
    add_string(item, "update_fields_code", entity.update_fields_code);
    add_string(item, "version_overflow_code", entity.version_overflow_code);
    add_string(item, "version_where_code", entity.version_where_code);
    add_string(item, "version_remove_where_code",
               entity.version_remove_where_code);
    add_string(item, "version_commit_code", entity.version_commit_code);
    if (entity.versioned) {
      Node* versioned = add_list(item, "versioned");
      (void)add_map(versioned, nullptr);
    }
  }
  return root;
}

void* provider_root(void* data) { return data; }

int provider_dump(void* node, int (*write)(const char*, size_t, void*),
                  void* renderer, void*) {
  const auto* current = static_cast<const Node*>(node);
  if (current != nullptr && current->type == NODE_STRING &&
      current->data.string_val != nullptr) {
    return write(current->data.string_val, std::strlen(current->data.string_val), renderer);
  }
  return 0;
}

void* provider_child(void* node, const char* name, size_t size, void*) {
  auto* current = static_cast<Node*>(node);
  if (current == nullptr || current->type != NODE_MAP) {
    return nullptr;
  }
  for (size_t i = 0; i < current->data.map.count; ++i) {
    Node* child = current->data.map.items[i];
    if (child != nullptr && child->name != nullptr &&
        std::strlen(child->name) == size &&
        std::strncmp(child->name, name, size) == 0) {
      return child;
    }
  }
  return nullptr;
}

void* provider_index(void* node, unsigned index, void*) {
  auto* current = static_cast<Node*>(node);
  if (current == nullptr) {
    return nullptr;
  }
  if (current->type == NODE_LIST) {
    return index < current->data.list.count ? current->data.list.items[index] : nullptr;
  }
  return index == 0 ? current : nullptr;
}

struct render_state {
  std::string& output;
  bool failed = false;
};

int append_output(const char* value, size_t size, void* data) {
  auto* state = static_cast<render_state*>(data);
  try {
    state->output.append(value, size);
    return 0;
  } catch (...) {
    state->failed = true;
    return 1;
  }
}

bool render_template(const char* template_text, std::size_t template_size,
                     std::string_view label, Node* context,
                     std::string& output,
                     std::vector<diagnostic>& diagnostics) {
  template_owner templ(
      mustache_compile(template_text, template_size, nullptr, nullptr, 0),
      &mustache_release);
  if (templ == nullptr) {
    diagnostics.push_back(
        {"", "", "failed to compile embedded ORM " + std::string(label) + " template"});
    return false;
  }
  MUSTACHE_RENDERER renderer{append_output, append_output};
  MUSTACHE_DATAPROVIDER provider{provider_dump, provider_root, provider_child,
                                 provider_index, nullptr, nullptr, nullptr};
  render_state state{output};
  const int result =
      mustache_process(templ.get(), &renderer, &state, &provider, context);
  if (state.failed || result != MUSTACHE_ERR_SUCCESS) {
    output.clear();
    diagnostics.push_back(
        {"", "", state.failed
                     ? "failed to allocate rendered ORM " + std::string(label)
                     : "failed to render ORM " + std::string(label)});
    return false;
  }
  return true;
}

bool render_cpp(Node* context, std::string& output,
                std::vector<diagnostic>& diagnostics) {
  return render_template(detail::cpp_metadata_template,
                         sizeof(detail::cpp_metadata_template) - 1,
                         "C++ metadata", context, output, diagnostics);
}

bool render_c(Node* context, std::string& output,
              std::vector<diagnostic>& diagnostics) {
  return render_template(detail::c_facade_template,
                         sizeof(detail::c_facade_template) - 1,
                         "C facade", context, output, diagnostics);
}

}  // namespace

bool generate_cpp(const Node* root, std::string& output,
                  std::vector<diagnostic>& diagnostics) {
  output.clear();
  schema_model schema;
  if (!normalize(root, schema, diagnostics) || !validate(schema, diagnostics)) {
    return false;
  }
  if (!schema.orm_enabled) {
    diagnostics.push_back(
        {"", "", "C++ ORM metadata generation requires schema [orm(1)]"});
    return false;
  }

  const auto enum_types = read_enum_types(schema);
  std::vector<generated_relation> relations;
  std::vector<generated_entity> entities;
  entities.reserve(schema.entities.size());
  for (const entity_model& entity : schema.entities) {
    entities.push_back(
        make_generated_entity(entity, schema, enum_types, relations));
  }

  try {
    node_owner context = build_render_context(schema, entities, relations);
    return render_cpp(context.get(), output, diagnostics);
  } catch (const std::bad_alloc&) {
    output.clear();
    diagnostics.push_back({"", "", "failed to allocate ORM template context"});
    return false;
  }
}

bool generate_c(const Node* root, std::string_view model_header,
                std::string& output,
                std::vector<diagnostic>& diagnostics) {
  output.clear();
  schema_model schema;
  if (!normalize(root, schema, diagnostics) || !validate(schema, diagnostics)) {
    return false;
  }
  if (!schema.orm_enabled) {
    diagnostics.push_back(
        {"", "", "C ORM facade generation requires schema [orm(1)]"});
    return false;
  }
  if (!is_c_identifier(schema.name)) {
    diagnostics.push_back(
        {"", "", "ORM C facade requires a valid C schema name"});
    return false;
  }
  if (model_header.empty()) {
    diagnostics.push_back(
        {"", "", "ORM C facade generation requires a TBE model header"});
    return false;
  }
  for (const entity_model& entity : schema.entities) {
    if (entity.inheritance_declared || entity.extends_declared) {
      diagnostics.push_back(
          {entity.name, "",
           "C ORM facade does not support C++ single-table inheritance layouts"});
      return false;
    }
  }

  std::vector<generated_c_entity> entities;
  entities.reserve(schema.entities.size());
  for (const entity_model& entity : schema.entities) {
    if (entity.embeddable) continue;
    generated_c_entity generated;
    if (!make_generated_c_entity(entity, schema, generated, diagnostics)) {
      output.clear();
      return false;
    }
    entities.push_back(std::move(generated));
  }

  try {
    node_owner context = build_c_render_context(schema, model_header, entities);
    return render_c(context.get(), output, diagnostics);
  } catch (const std::bad_alloc&) {
    output.clear();
    diagnostics.push_back({"", "", "failed to allocate ORM C facade context"});
    return false;
  }
}

}  // namespace orm::schema
