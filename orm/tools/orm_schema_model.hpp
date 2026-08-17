#ifndef ORM_SCHEMA_MODEL_HPP
#define ORM_SCHEMA_MODEL_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <node_tree.h>
}

namespace orm::schema {

inline constexpr std::size_t max_entity_relations = 64;

struct diagnostic {
  std::string entity;
  std::string field;
  std::string message;
};

struct field_model {
  std::string name;
  std::string c_member;
  std::string type;
  std::string reference_type;
  std::string collection_kind;
  std::string column;
  bool column_declared = false;
  bool primary_key = false;
  bool version_declared = false;
  bool version = false;
  bool relation = false;
  std::string relation_target;
  std::string mapped_by;
  std::string foreign_key;
  std::string cascade;
  bool fetch_declared = false;
  std::string fetch;
  bool orphan_removal_declared = false;
  bool orphan_removal = false;
  bool embedded_declared = false;
  bool embedded = false;
  bool embedded_id_declared = false;
  bool embedded_id = false;
  bool optional = false;
};

struct lifecycle_callback_model {
  std::string event;
  std::string method;
};

struct entity_model {
  std::string name;
  std::string table;
  bool table_declared = false;
  bool inheritance_declared = false;
  std::string inheritance;
  bool extends_declared = false;
  std::string base_entity;
  bool discriminator_column_declared = false;
  std::string discriminator_column;
  bool discriminator_value_declared = false;
  std::string discriminator_value;
  bool embeddable_declared = false;
  bool embeddable = false;
  std::vector<field_model> fields;
  std::vector<lifecycle_callback_model> lifecycle_callbacks;

  [[nodiscard]] const field_model* find_field(std::string_view name) const noexcept;
};

struct enum_model {
  std::string name;
  std::string underlying_type;
};

struct schema_model {
  std::string name;
  bool orm_enabled = false;
  bool cpp_namespace_declared = false;
  std::string cpp_namespace;
  std::vector<entity_model> entities;
  std::vector<enum_model> enums;

  [[nodiscard]] const entity_model* find_entity(std::string_view name) const noexcept;
};

/* Normalize the public TBE AST into an owning, format-independent ORM model. */
bool normalize(const Node* root, schema_model& output,
               std::vector<diagnostic>& diagnostics);

}  // namespace orm::schema

#endif  // ORM_SCHEMA_MODEL_HPP
