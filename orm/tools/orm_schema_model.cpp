#include "orm_schema_model.hpp"

#include <array>
#include <cstring>
#include <utility>

namespace orm::schema {
namespace {

const Node* find_child(const Node* parent, const char* name) {
  if (parent == nullptr || name == nullptr) {
    return nullptr;
  }
  const Node* const* items = nullptr;
  size_t count = 0;
  if (parent->type == NODE_MAP) {
    items = parent->data.map.items;
    count = parent->data.map.count;
  } else if (parent->type == NODE_LIST) {
    items = parent->data.list.items;
    count = parent->data.list.count;
  } else {
    return nullptr;
  }
  for (size_t i = 0; i < count; ++i) {
    const Node* child = items[i];
    if (child != nullptr && child->name != nullptr &&
        std::strcmp(child->name, name) == 0) {
      return child;
    }
  }
  return nullptr;
}

const char* string_value(const Node* parent, const char* name) {
  const Node* child = find_child(parent, name);
  if (child == nullptr || child->type != NODE_STRING) {
    return nullptr;
  }
  return child->data.string_val;
}

const Node* attributes(const Node* node) { return find_child(node, "attributes"); }

const char* attribute_value(const Node* node, const char* attribute) {
  const Node* attrs = attributes(node);
  if (attrs == nullptr || attrs->type != NODE_LIST) {
    return nullptr;
  }
  for (size_t i = 0; i < attrs->data.list.count; ++i) {
    const Node* item = attrs->data.list.items[i];
    if (item == nullptr) {
      continue;
    }
    const char* item_name = item->name;
    if (item_name == nullptr) {
      item_name = string_value(item, "name");
    }
    if (item_name != nullptr && std::strcmp(item_name, attribute) == 0) {
      return string_value(item, "value");
    }
  }
  return nullptr;
}

bool has_attribute(const Node* node, const char* attribute) {
  const Node* attrs = attributes(node);
  if (attrs == nullptr || attrs->type != NODE_LIST) {
    return false;
  }
  for (size_t i = 0; i < attrs->data.list.count; ++i) {
    const Node* item = attrs->data.list.items[i];
    if (item == nullptr) {
      continue;
    }
    const char* item_name = item->name;
    if (item_name == nullptr) {
      item_name = string_value(item, "name");
    }
    if (item_name != nullptr && std::strcmp(item_name, attribute) == 0) {
      return true;
    }
  }
  return false;
}

bool truthy(const char* value) {
  return value != nullptr && (std::strcmp(value, "1") == 0 ||
                              std::strcmp(value, "true") == 0 ||
                              std::strcmp(value, "yes") == 0 ||
                              std::strcmp(value, "enabled") == 0);
}

std::string value_or_empty(const char* value) {
  return value == nullptr ? std::string() : std::string(value);
}

field_model normalize_field(const Node* field) {
  field_model result;
  result.name = value_or_empty(string_value(field, "name"));
  const char* c_member = attribute_value(field, "c");
  result.c_member = c_member == nullptr ? result.name : c_member;
  result.type = value_or_empty(string_value(field, "type"));
  const char* column = attribute_value(field, "column");
  result.column_declared = column != nullptr;
  result.column = column == nullptr ? result.name : column;
  result.primary_key = truthy(attribute_value(field, "id")) ||
                       truthy(attribute_value(field, "primary_key"));
  const char* version = attribute_value(field, "version");
  result.version_declared = version != nullptr;
  result.version = truthy(version);
  const char* relation = attribute_value(field, "relation");
  result.relation = relation != nullptr;
  result.relation_target = value_or_empty(relation);
  result.mapped_by = value_or_empty(attribute_value(field, "mapped_by"));
  result.foreign_key = value_or_empty(attribute_value(field, "foreign_key"));
  result.cascade = value_or_empty(attribute_value(field, "cascade"));
  result.fetch_declared = has_attribute(field, "fetch");
  result.fetch = value_or_empty(attribute_value(field, "fetch"));
  const char* orphan_removal = attribute_value(field, "orphan_removal");
  if (orphan_removal == nullptr) {
    orphan_removal = attribute_value(field, "orphan_remove");
  }
  result.orphan_removal_declared = orphan_removal != nullptr;
  result.orphan_removal = truthy(orphan_removal);
  const char* embedded = attribute_value(field, "embedded");
  result.embedded_declared = has_attribute(field, "embedded");
  result.embedded = truthy(embedded);
  const char* embedded_id = attribute_value(field, "embedded_id");
  result.embedded_id_declared = has_attribute(field, "embedded_id");
  result.embedded_id = truthy(embedded_id);
  if (result.embedded_id) result.embedded = true;
  result.optional = find_child(field, "is_optional") != nullptr;
  result.reference_type = value_or_empty(string_value(field, "inner_type"));
  result.collection_kind = value_or_empty(string_value(field, "collection_kind"));
  if (result.reference_type.empty()) {
    result.reference_type = result.type;
  }
  return result;
}

void normalize_entities(const Node* messages, schema_model& output) {
  static constexpr std::array<const char*, 7> lifecycle_events = {
      "pre_persist", "post_persist", "pre_update", "post_update",
      "pre_remove", "post_remove", "post_load"};
  if (messages == nullptr || messages->type != NODE_LIST) {
    return;
  }
  output.entities.reserve(messages->data.list.count);
  for (size_t i = 0; i < messages->data.list.count; ++i) {
    const Node* message = messages->data.list.items[i];
    if (message == nullptr || message->type != NODE_MAP) {
      continue;
    }
    entity_model entity;
    entity.name = value_or_empty(string_value(message, "message_name"));
    if (entity.name.empty()) {
      entity.name = value_or_empty(string_value(message, "name"));
    }
    const char* table = attribute_value(message, "table");
    entity.table_declared = table != nullptr;
    entity.table = value_or_empty(table);
    const char* inheritance = attribute_value(message, "inheritance");
    entity.inheritance_declared = inheritance != nullptr;
    entity.inheritance = value_or_empty(inheritance);
    const char* base_entity = attribute_value(message, "extends");
    entity.extends_declared = base_entity != nullptr;
    entity.base_entity = value_or_empty(base_entity);
    const char* discriminator_column =
        attribute_value(message, "discriminator_column");
    entity.discriminator_column_declared = discriminator_column != nullptr;
    entity.discriminator_column = value_or_empty(discriminator_column);
    const char* discriminator_value =
        attribute_value(message, "discriminator_value");
    entity.discriminator_value_declared = discriminator_value != nullptr;
    entity.discriminator_value = value_or_empty(discriminator_value);
    const char* embeddable = attribute_value(message, "embeddable");
    entity.embeddable_declared = has_attribute(message, "embeddable");
    entity.embeddable = truthy(embeddable);
    for (const char* event : lifecycle_events) {
      if (has_attribute(message, event)) {
        entity.lifecycle_callbacks.push_back(
            {event, value_or_empty(attribute_value(message, event))});
      }
    }
    const Node* fields = find_child(message, "fields");
    if (fields != nullptr && fields->type == NODE_LIST) {
      entity.fields.reserve(fields->data.list.count);
      for (size_t j = 0; j < fields->data.list.count; ++j) {
        const Node* field = fields->data.list.items[j];
        if (field != nullptr && field->type == NODE_MAP) {
          entity.fields.push_back(normalize_field(field));
        }
      }
    }
    output.entities.push_back(std::move(entity));
  }
}

void normalize_enums(const Node* enums, schema_model& output) {
  if (enums == nullptr || enums->type != NODE_LIST) {
    return;
  }
  output.enums.reserve(enums->data.list.count);
  for (size_t i = 0; i < enums->data.list.count; ++i) {
    const Node* item = enums->data.list.items[i];
    if (item == nullptr || item->type != NODE_MAP) {
      continue;
    }
    enum_model normalized;
    normalized.name = value_or_empty(string_value(item, "enum_name"));
    normalized.underlying_type =
        value_or_empty(string_value(item, "underlying_type"));
    if (normalized.underlying_type.empty()) {
      normalized.underlying_type = "int32";
    }
    output.enums.push_back(std::move(normalized));
  }
}

}  // namespace

const field_model* entity_model::find_field(std::string_view target) const noexcept {
  for (const field_model& field : fields) {
    if (field.name == target) {
      return &field;
    }
  }
  return nullptr;
}

const entity_model* schema_model::find_entity(std::string_view target) const noexcept {
  for (const entity_model& entity : entities) {
    if (entity.name == target) {
      return &entity;
    }
  }
  return nullptr;
}

bool normalize(const Node* root, schema_model& output,
               std::vector<diagnostic>& diagnostics) {
  output = {};
  diagnostics.clear();
  if (root == nullptr || root->type != NODE_MAP) {
    diagnostics.push_back({"", "", "schema root must be a map node"});
    return false;
  }

  const Node* schema = find_child(root, "schema");
  output.name = value_or_empty(string_value(schema, "schema_name"));
  output.orm_enabled = truthy(attribute_value(schema, "orm"));
  const char* cpp_namespace = attribute_value(schema, "cpp_namespace");
  output.cpp_namespace_declared = cpp_namespace != nullptr;
  output.cpp_namespace = value_or_empty(cpp_namespace);
  if (!output.orm_enabled) {
    return true;
  }

  normalize_entities(find_child(root, "messages"), output);
  normalize_enums(find_child(root, "enums"), output);
  return true;
}

}  // namespace orm::schema
