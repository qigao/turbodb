#include "orm_schema_validator.hpp"

#include <cctype>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <algorithm>

namespace orm::schema {
namespace {

void add_diagnostic(std::vector<diagnostic>& diagnostics, const std::string& entity,
                    const std::string& field, const std::string& message) {
  diagnostics.push_back({entity, field, message});
}

bool is_cpp_keyword(const std::string& value) {
  static const std::unordered_set<std::string> keywords = {
      "alignas",      "alignof",     "and",          "and_eq",       "asm",
      "auto",         "bitand",      "bitor",        "bool",          "break",
      "case",         "catch",       "char",         "char8_t",       "char16_t",
      "char32_t",     "class",       "compl",        "concept",       "const",
      "consteval",    "constexpr",   "constinit",    "const_cast",    "continue",
      "co_await",     "co_return",   "co_yield",     "decltype",      "default",
      "delete",       "do",          "double",       "dynamic_cast",  "else",
      "enum",         "explicit",    "export",       "extern",        "false",
      "float",        "for",         "friend",       "goto",          "if",
      "inline",       "int",         "long",         "mutable",       "namespace",
      "new",          "noexcept",    "not",          "not_eq",        "nullptr",
      "operator",     "or",          "or_eq",        "private",       "protected",
      "public",       "reflexpr",   "register",     "reinterpret_cast", "requires",
      "return",       "short",       "signed",       "sizeof",        "static",
      "static_assert", "static_cast", "struct",       "switch",        "synchronized",
      "template",     "this",        "thread_local", "throw",         "true",
      "try",          "typedef",     "typeid",       "typename",      "union",
      "unsigned",     "using",       "virtual",      "void",          "volatile",
      "wchar_t",      "while",       "xor",          "xor_eq"};
  return keywords.find(value) != keywords.end();
}

bool is_cpp_identifier(const std::string& value) {
  if (value.empty() || is_cpp_keyword(value)) {
    return false;
  }
  const auto is_first = [](unsigned char character) {
    return std::isalpha(character) != 0 || character == '_';
  };
  const auto is_rest = [&](unsigned char character) {
    return is_first(character) || std::isdigit(character) != 0;
  };
  if (!is_first(static_cast<unsigned char>(value.front()))) {
    return false;
  }
  for (size_t i = 1; i < value.size(); ++i) {
    if (!is_rest(static_cast<unsigned char>(value[i]))) {
      return false;
    }
  }
  return true;
}

bool is_cpp_namespace(const std::string& value) {
  if (value.empty() || value.front() == ':' || value.back() == ':') {
    return false;
  }
  size_t begin = 0;
  while (begin < value.size()) {
    const size_t separator = value.find("::", begin);
    const size_t end = separator == std::string::npos ? value.size() : separator;
    if (end == begin || !is_cpp_identifier(value.substr(begin, end - begin))) {
      return false;
    }
    if (separator == std::string::npos) {
      return true;
    }
    begin = separator + 2;
  }
  return false;
}

bool allowed_cascade(const std::string& cascade) {
  static const std::unordered_set<std::string> values = {
      "none", "persist", "remove", "merge", "refresh", "detach", "all",
      "orphan_remove"};
  return cascade.empty() || values.find(cascade) != values.end();
}

bool allowed_fetch(const std::string& fetch) {
  return fetch == "lazy" || fetch == "eager";
}

bool allowed_inheritance(const std::string& strategy) {
  return strategy == "single_table" || strategy == "joined" ||
         strategy == "table_per_class";
}

bool is_integer_type(const std::string& type) {
  static const std::unordered_set<std::string> values = {
      "byte", "int8", "int8_t", "i8", "uint8", "uint8_t", "u8",
      "int16", "int16_t", "i16", "uint16", "uint16_t", "u16",
      "int32", "int32_t", "i32", "uint32", "uint32_t", "u32",
      "int64", "int64_t", "i64", "uint64", "uint64_t", "u64"};
  return values.find(type) != values.end();
}

bool is_optional_scalar_type(const std::string& type) {
  static const std::unordered_set<std::string> values = {
      "bool", "float", "double", "string", "bytes"};
  return is_integer_type(type) || values.find(type) != values.end();
}

std::size_t primary_component_count(const entity_model& entity,
                                    const schema_model& schema) {
  std::size_t count = 0;
  const entity_model* current = &entity;
  for (std::size_t depth = 0;
       current != nullptr && depth <= schema.entities.size(); ++depth) {
    for (const field_model& field : current->fields) {
      if (field.primary_key) ++count;
      if (!field.embedded_id) continue;
      const entity_model* embedded = schema.find_entity(field.type);
      if (embedded == nullptr) continue;
      for (const field_model& component : embedded->fields) {
        if (!component.relation && !component.embedded) ++count;
      }
    }
    current = current->extends_declared
                  ? schema.find_entity(current->base_entity)
                  : nullptr;
  }
  return count;
}

const entity_model* hierarchy_root(const entity_model& entity,
                                   const schema_model& schema) {
  const entity_model* current = &entity;
  for (std::size_t depth = 0; depth <= schema.entities.size(); ++depth) {
    if (!current->extends_declared) return current;
    current = schema.find_entity(current->base_entity);
    if (current == nullptr) return nullptr;
  }
  return nullptr;
}

const field_model* effective_primary_field(const entity_model& entity,
                                           const schema_model& schema) {
  const entity_model* current = &entity;
  for (std::size_t depth = 0;
       current != nullptr && depth <= schema.entities.size(); ++depth) {
    for (const field_model& field : current->fields) {
      if (field.primary_key) return &field;
    }
    current = current->extends_declared
                  ? schema.find_entity(current->base_entity)
                  : nullptr;
  }
  return nullptr;
}

}  // namespace

bool validate(const schema_model& schema, std::vector<diagnostic>& diagnostics) {
  diagnostics.clear();
  if (!schema.orm_enabled) {
    return true;
  }

  if (schema.cpp_namespace_declared &&
      !is_cpp_namespace(schema.cpp_namespace)) {
    add_diagnostic(diagnostics, "", "",
                   "cpp_namespace must contain valid C++ namespace identifiers");
  }

  if (schema.entities.empty()) {
    add_diagnostic(diagnostics, "", "", "ORM schema profile requires at least one message entity");
    return false;
  }

  std::unordered_set<std::string> enum_names;
  for (const enum_model& enum_value : schema.enums) {
      if (enum_value.name.empty() || !is_cpp_identifier(enum_value.name)) {
        add_diagnostic(diagnostics, enum_value.name, "",
                       "ORM enum name must be a valid C++ identifier");
      } else if (!enum_names.insert(enum_value.name).second) {
        add_diagnostic(diagnostics, enum_value.name, "", "duplicate ORM enum name");
      }
      if (!is_integer_type(enum_value.underlying_type)) {
        add_diagnostic(diagnostics, enum_value.name, "",
                       "ORM enum underlying type must be a scalar integer type");
      }
  }
  std::unordered_map<std::string, size_t> entity_names;
  std::unordered_map<std::string, size_t> table_names;
  std::size_t persistent_entity_count = 0;
  for (size_t i = 0; i < schema.entities.size(); ++i) {
    const entity_model& entity = schema.entities[i];
    if (entity.name.empty()) {
      add_diagnostic(diagnostics, "", "", "ORM entity is missing its message name");
    } else if (!is_cpp_identifier(entity.name)) {
      add_diagnostic(diagnostics, entity.name, "",
                     "ORM entity name must be a valid C++ identifier");
    } else if (!entity_names.emplace(entity.name, i).second) {
      add_diagnostic(diagnostics, entity.name, "", "duplicate ORM entity name");
    }
    if (entity.embeddable_declared && !entity.embeddable) {
      add_diagnostic(diagnostics, entity.name, "",
                     "embeddable value must be enabled with 1, true, yes, or enabled");
    }
    if (entity.embeddable) {
      if (entity.table_declared) {
        add_diagnostic(diagnostics, entity.name, "",
                       "ORM embeddable type cannot declare a table");
      }
    } else {
      ++persistent_entity_count;
      if (!entity.extends_declared &&
          (!entity.table_declared || entity.table.empty())) {
        add_diagnostic(diagnostics, entity.name, "",
                       "ORM entity requires [table(name)]");
      }
      if (!entity.table.empty() &&
          !table_names.emplace(entity.table, i).second) {
        add_diagnostic(diagnostics, entity.name, "", "duplicate ORM table name");
      }
    }
  }
  if (persistent_entity_count == 0) {
    add_diagnostic(diagnostics, "", "",
                   "ORM schema profile requires at least one persistent entity");
  }

  std::unordered_map<std::string, std::unordered_set<std::string>>
      discriminator_values;
  for (const entity_model& entity : schema.entities) {
    if (entity.embeddable) continue;
    if (entity.extends_declared) {
      const entity_model* base = schema.find_entity(entity.base_entity);
      if (base == nullptr) {
        add_diagnostic(diagnostics, entity.name, "",
                       "extends target entity does not exist: " +
                           entity.base_entity);
        continue;
      }
      if (base->embeddable) {
        add_diagnostic(diagnostics, entity.name, "",
                       "extends target must be a persistent entity");
      }
      if (entity.inheritance_declared || entity.discriminator_column_declared) {
        add_diagnostic(diagnostics, entity.name, "",
                       "inheritance strategy and discriminator column belong to the root entity");
      }
      const entity_model* root = hierarchy_root(entity, schema);
      if (root == nullptr) {
        add_diagnostic(diagnostics, entity.name, "",
                       "ORM inheritance hierarchy contains a cycle or missing base");
      } else if (!allowed_inheritance(root->inheritance)) {
        add_diagnostic(diagnostics, entity.name, "",
                       "extends requires a root with a supported inheritance strategy");
      } else if (root->inheritance == "single_table" ||
                 root->inheritance == "joined") {
        if (root->inheritance == "single_table" && entity.table_declared) {
          add_diagnostic(
              diagnostics, entity.name, "",
              "single-table subtype inherits its table and cannot declare [table(...)]");
        }
        if (root->inheritance == "joined" &&
            (!entity.table_declared || entity.table.empty())) {
          add_diagnostic(diagnostics, entity.name, "",
                         "joined subtype requires its own [table(name)]");
        }
        if (!entity.discriminator_value_declared ||
            entity.discriminator_value.empty()) {
          add_diagnostic(
              diagnostics, entity.name, "",
              root->inheritance == "joined"
                  ? "joined subtype requires [discriminator_value(value)]"
                  : "single-table subtype requires [discriminator_value(value)]");
        } else if (!discriminator_values[root->name]
                        .insert(entity.discriminator_value)
                        .second) {
          add_diagnostic(diagnostics, entity.name, "",
                         "duplicate discriminator value in inheritance hierarchy: " +
                             entity.discriminator_value);
        }
      } else if (root->inheritance == "table_per_class") {
        if (!entity.table_declared || entity.table.empty()) {
          add_diagnostic(
              diagnostics, entity.name, "",
              "table-per-class subtype requires its own [table(name)]");
        }
        if (entity.discriminator_value_declared) {
          add_diagnostic(
              diagnostics, entity.name, "",
              "table-per-class subtype cannot declare a discriminator value");
        }
      }
    } else if (entity.inheritance_declared) {
      if (!allowed_inheritance(entity.inheritance)) {
        add_diagnostic(diagnostics, entity.name, "",
                       "inheritance strategy must be single_table, joined, or table_per_class");
      } else if (entity.inheritance == "single_table" ||
                 entity.inheritance == "joined") {
        if (!entity.discriminator_column_declared ||
            entity.discriminator_column.empty()) {
          add_diagnostic(diagnostics, entity.name, "",
                         entity.inheritance == "joined"
                             ? "joined root requires [discriminator_column(column)]"
                             : "single-table root requires [discriminator_column(column)]");
        }
        if (!entity.discriminator_value_declared ||
            entity.discriminator_value.empty()) {
          add_diagnostic(diagnostics, entity.name, "",
                         entity.inheritance == "joined"
                             ? "joined root requires [discriminator_value(value)]"
                             : "single-table root requires [discriminator_value(value)]");
        } else if (!discriminator_values[entity.name]
                        .insert(entity.discriminator_value)
                        .second) {
          add_diagnostic(diagnostics, entity.name, "",
                         "duplicate discriminator value in inheritance hierarchy: " +
                             entity.discriminator_value);
        }
      } else if (entity.discriminator_column_declared ||
                 entity.discriminator_value_declared) {
        add_diagnostic(
            diagnostics, entity.name, "",
            "table-per-class root cannot declare discriminator metadata");
      }
    } else if (entity.discriminator_column_declared ||
               entity.discriminator_value_declared) {
      add_diagnostic(diagnostics, entity.name, "",
                     "discriminator metadata requires single-table inheritance");
    }
  }

  for (const entity_model& entity : schema.entities) {
    if (entity.embeddable && !entity.lifecycle_callbacks.empty()) {
      add_diagnostic(diagnostics, entity.name, "",
                     "ORM embeddable type cannot declare lifecycle callbacks");
    }
    for (const lifecycle_callback_model& callback :
         entity.lifecycle_callbacks) {
      if (!is_cpp_identifier(callback.method)) {
        add_diagnostic(
            diagnostics, entity.name, "",
            callback.event +
                " callback must name a valid C++ member function");
      }
    }
    size_t primary_count = 0;
    size_t version_count = 0;
    size_t relation_count = 0;
    std::unordered_set<std::string> field_names;
    std::unordered_map<std::string, std::string> column_names;
    for (const field_model& field : entity.fields) {
      if (field.relation) {
        ++relation_count;
      }
      if (field.name.empty()) {
        add_diagnostic(diagnostics, entity.name, "", "ORM field is missing its name");
        continue;
      }
      if (!is_cpp_identifier(field.name)) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "ORM field name must be a valid C++ identifier");
      }
      if (!field_names.insert(field.name).second) {
        add_diagnostic(diagnostics, entity.name, field.name, "duplicate ORM field name");
      }
      if (!field.relation && !field.embedded) {
        auto column = column_names.emplace(field.column, field.name);
        if (!column.second) {
          add_diagnostic(diagnostics, entity.name, field.name,
                         "duplicate ORM column name: " + field.column);
        }
      }
      if (field.primary_key) {
        ++primary_count;
        if (field.optional) {
          add_diagnostic(diagnostics, entity.name, field.name,
                         "ORM primary-key field cannot be optional");
        }
        if (!field.collection_kind.empty() ||
            (!is_integer_type(field.type) && field.type != "bool" &&
             field.type != "string" &&
             enum_names.find(field.type) == enum_names.end())) {
          add_diagnostic(
              diagnostics, entity.name, field.name,
              "ORM primary-key component must use an identity scalar or enum type");
        }
      }
      if (field.embedded_declared && !field.embedded) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "embedded value must be enabled with 1, true, yes, or enabled");
      }
      if (field.embedded_id_declared && !field.embedded_id) {
        add_diagnostic(
            diagnostics, entity.name, field.name,
            "embedded_id value must be enabled with 1, true, yes, or enabled");
      }
      if (field.embedded) {
        if (entity.embeddable) {
          add_diagnostic(diagnostics, entity.name, field.name,
                         "nested ORM embeddable values are not supported");
        }
        if (field.relation) {
          add_diagnostic(diagnostics, entity.name, field.name,
                         "embedded field cannot also be a relation");
        }
        if (field.column_declared) {
          add_diagnostic(diagnostics, entity.name, field.name,
                         "embedded field cannot declare a column");
        }
        if (field.optional || !field.collection_kind.empty()) {
          add_diagnostic(diagnostics, entity.name, field.name,
                         "embedded field must be a required scalar object");
        }
        if (field.primary_key) {
          add_diagnostic(diagnostics, entity.name, field.name,
                         "embedded field uses embedded_id instead of id");
        }
        if (field.version) {
          add_diagnostic(diagnostics, entity.name, field.name,
                         "embedded field cannot be a version field");
        }
      }
      if (field.version_declared && !field.version) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "version value must be enabled with 1, true, yes, or enabled");
      }
      if (field.version) {
        ++version_count;
        if (field.optional) {
          add_diagnostic(diagnostics, entity.name, field.name,
                         "ORM version field cannot be optional");
        }
        if (field.primary_key) {
          add_diagnostic(diagnostics, entity.name, field.name,
                         "ORM version field cannot also be the primary key");
        }
        if (field.relation) {
          add_diagnostic(diagnostics, entity.name, field.name,
                         "ORM version field cannot be a relation");
        }
        if (!field.collection_kind.empty() || !is_integer_type(field.type)) {
          add_diagnostic(diagnostics, entity.name, field.name,
                         "ORM version field must use a scalar integer type");
        }
      }
      if (!field.cascade.empty() && !allowed_cascade(field.cascade)) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "invalid cascade value: " + field.cascade);
      }
      if (!field.cascade.empty() && !field.relation) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "cascade is only valid on a relation field");
      }
      if (field.fetch_declared && !allowed_fetch(field.fetch)) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "fetch must be lazy or eager");
      }
      if (field.fetch_declared && !field.relation) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "fetch is only valid on a relation field");
      }
      if (field.orphan_removal_declared && !field.orphan_removal) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "orphan removal value must be enabled with 1, true, yes, or enabled");
      }
      if (field.orphan_removal && !field.relation) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "orphan removal is only valid on a relation field");
      }
      if (field.cascade == "orphan_remove" && !field.relation) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "orphan_remove cascade is only valid on a relation field");
      }
      if ((!field.mapped_by.empty() || !field.foreign_key.empty()) && !field.relation) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "relation mapping is only valid on a relation field");
      }
      if (!field.mapped_by.empty() && !field.foreign_key.empty()) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "relation cannot declare both mapped_by and foreign_key");
      }
      if (field.relation && field.column_declared) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "relation field cannot also declare [column(...)]");
      }
      if (field.relation && field.primary_key) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "relation field cannot be an ORM primary key");
      }
      if (field.optional && !field.relation && !field.embedded &&
          !is_optional_scalar_type(field.type) &&
          enum_names.find(field.type) == enum_names.end()) {
        add_diagnostic(
            diagnostics, entity.name, field.name,
            "optional ORM field must use a supported scalar, string, bytes, or enum type");
      }
    }
    std::size_t embedded_id_count = 0;
    for (const field_model& field : entity.fields) {
      if (field.embedded_id) ++embedded_id_count;
    }
    if (entity.embeddable) {
      if (entity.fields.empty()) {
        add_diagnostic(diagnostics, entity.name, "",
                       "ORM embeddable type requires at least one field");
      }
      if (primary_count != 0 || embedded_id_count != 0) {
        add_diagnostic(diagnostics, entity.name, "",
                       "ORM embeddable type cannot declare a primary key");
      }
      if (version_count != 0 || relation_count != 0) {
        add_diagnostic(diagnostics, entity.name, "",
                       "ORM embeddable type cannot declare version or relation fields");
      }
    } else if (!entity.extends_declared && primary_count == 0 &&
               embedded_id_count == 0) {
      add_diagnostic(diagnostics, entity.name, "",
                     "ORM entity requires at least one primary key component");
    }
    if (!entity.embeddable && primary_component_count(entity, schema) == 0) {
      add_diagnostic(diagnostics, entity.name, "",
                     "ORM entity primary key has no flattened components");
    }
    if (embedded_id_count > 1 ||
        (embedded_id_count != 0 && primary_count != 0)) {
      add_diagnostic(diagnostics, entity.name, "",
                     "ORM entity must use either direct id fields or one embedded_id");
    }
    if (version_count > 1) {
      add_diagnostic(diagnostics, entity.name, "", "ORM entity has multiple version fields");
    }
    if (entity.extends_declared &&
        (primary_count != 0 || embedded_id_count != 0)) {
      add_diagnostic(diagnostics, entity.name, "",
                     "single-table subtype inherits its primary key and cannot declare one");
    }
    if (entity.extends_declared && version_count != 0) {
      add_diagnostic(diagnostics, entity.name, "",
                     "single-table subtype inherits its version field and cannot declare one");
    }
    const entity_model* root = hierarchy_root(entity, schema);
    if (root != nullptr && root->inheritance == "single_table" &&
        !root->discriminator_column.empty() &&
        column_names.find(root->discriminator_column) != column_names.end()) {
      add_diagnostic(diagnostics, entity.name, "",
                     "discriminator column cannot also be a modeled field: " +
                         root->discriminator_column);
    }
    if (root != nullptr && root->inheritance == "joined") {
      if (primary_component_count(entity, schema) != 1) {
        add_diagnostic(
            diagnostics, entity.name, "",
            "joined inheritance currently requires one primary-key component");
      }
      if (entity.extends_declared) {
        const bool has_local_column = std::any_of(
            entity.fields.begin(), entity.fields.end(),
            [](const field_model& field) {
              return !field.relation && !field.embedded &&
                     !field.primary_key && !field.version;
            });
        if (!has_local_column) {
          add_diagnostic(
              diagnostics, entity.name, "",
              "joined subtype requires at least one local scalar column");
        }
      }
    }
    if (entity.extends_declared) {
      std::unordered_set<std::string> inherited_fields;
      std::unordered_set<std::string> inherited_columns;
      const entity_model* ancestor = schema.find_entity(entity.base_entity);
      for (std::size_t depth = 0;
           ancestor != nullptr && depth <= schema.entities.size(); ++depth) {
        for (const field_model& inherited : ancestor->fields) {
          inherited_fields.insert(inherited.name);
          if (!inherited.relation && !inherited.embedded) {
            inherited_columns.insert(inherited.column);
          }
          if (inherited.embedded) {
            const entity_model* embedded = schema.find_entity(inherited.type);
            if (embedded != nullptr) {
              for (const field_model& component : embedded->fields) {
                if (!component.relation && !component.embedded)
                  inherited_columns.insert(component.column);
              }
            }
          }
        }
        ancestor = ancestor->extends_declared
                       ? schema.find_entity(ancestor->base_entity)
                       : nullptr;
      }
      for (const field_model& field : entity.fields) {
        if (inherited_fields.find(field.name) != inherited_fields.end()) {
          add_diagnostic(diagnostics, entity.name, field.name,
                         "single-table subtype field hides an inherited ORM field");
        }
        if (!field.relation && !field.embedded &&
            inherited_columns.find(field.column) != inherited_columns.end()) {
          add_diagnostic(diagnostics, entity.name, field.name,
                         "single-table subtype column duplicates an inherited ORM column: " +
                             field.column);
        }
      }
    }
    if (relation_count > max_entity_relations) {
      add_diagnostic(diagnostics, entity.name, "",
                     "ORM entity exceeds the 64-relation graph limit");
    }

    const field_model* primary = effective_primary_field(entity, schema);
    for (const field_model& field : entity.fields) {
      const entity_model* referenced = schema.find_entity(field.type);
      if (field.embedded) {
        if (referenced == nullptr || !referenced->embeddable) {
          add_diagnostic(diagnostics, entity.name, field.name,
                         "embedded field type must name an ORM embeddable type");
          continue;
        }
        for (const field_model& component : referenced->fields) {
          if (component.relation || component.embedded) continue;
          if (field.embedded_id &&
              (component.optional || !component.collection_kind.empty() ||
               (!is_integer_type(component.type) && component.type != "bool" &&
                component.type != "string" &&
                enum_names.find(component.type) == enum_names.end()))) {
            add_diagnostic(
                diagnostics, entity.name, field.name,
                "embedded_id components must use required identity scalar or enum types");
          }
          auto inserted = column_names.emplace(
              component.column, field.name + "." + component.name);
          if (!inserted.second) {
            add_diagnostic(diagnostics, entity.name, field.name,
                           "duplicate flattened ORM column name: " +
                               component.column);
          }
        }
      } else if (!field.relation && referenced != nullptr) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       referenced->embeddable
                           ? "embeddable field requires [embedded(1)] or [embedded_id(1)]"
                           : "entity-valued field requires [relation(Target)]");
      }
    }
    for (const field_model& field : entity.fields) {
      if (!field.relation) {
        continue;
      }
      auto target_it = entity_names.find(field.relation_target);
      if (target_it == entity_names.end()) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "relation target entity does not exist: " + field.relation_target);
        continue;
      }
      const entity_model& target = schema.entities[target_it->second];
      if (target.embeddable) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "relation target must be a persistent entity");
      }
      if (primary_component_count(entity, schema) != 1) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "relations currently require a single-component owner primary key");
      }
      const std::string reference_type = field.reference_type;
      if (!reference_type.empty() && reference_type != "array" && reference_type != "vector" &&
          reference_type != "list" && reference_type != "set" &&
          reference_type != field.relation_target) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "relation field type does not match its target entity");
      }
      const std::string mapping = !field.mapped_by.empty() ? field.mapped_by : field.foreign_key;
      if (field.fetch_declared && allowed_fetch(field.fetch) && mapping.empty()) {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "fetch relation requires mapped_by or foreign_key");
      }
      if (!field.collection_kind.empty() && field.collection_kind != "list") {
        add_diagnostic(diagnostics, entity.name, field.name,
                       "ORM relation collections must use list<T>");
      }
      if (!mapping.empty()) {
        const field_model* mapped = target.find_field(mapping);
        if (mapped == nullptr) {
          add_diagnostic(diagnostics, entity.name, field.name,
                         "relation mapping field does not exist on target entity: " + mapping);
        } else if (primary != nullptr && mapped->type != primary->type) {
          add_diagnostic(diagnostics, entity.name, field.name,
                         "relation mapping field type must match the owner primary key type");
        }
      }
    }
  }
  return diagnostics.empty();
}

bool validate(const Node* root, std::vector<diagnostic>& diagnostics) {
  schema_model schema;
  if (!normalize(root, schema, diagnostics)) {
    return false;
  }
  return validate(schema, diagnostics);
}

}  // namespace orm::schema
