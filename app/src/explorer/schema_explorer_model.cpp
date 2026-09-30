#include "explorer/schema_explorer_model.h"

#include <algorithm>
#include <limits>
#include <map>
#include <tuple>
#include <utility>

namespace turbodb::app {

namespace {

std::string ToString(orm_string_view_t value) {
  if (value.data == nullptr || value.len == 0u) return {};
  return std::string(static_cast<const char*>(value.data), value.len);
}

ExplorerNodeKind RelationNodeKind(orm_metadata_kind_t kind) {
  return kind == ORM_METADATA_VIEW ? ExplorerNodeKind::view
                                   : ExplorerNodeKind::table;
}

std::string RelationKey(const std::string& catalog,
                        const std::string& schema,
                        const std::string& relation) {
  return catalog + "\x1f" + schema + "\x1f" + relation;
}

}  // namespace

void SchemaExplorerModel::Clear() noexcept {
  nodes_.clear();
  relations_.clear();
}

bool SchemaExplorerModel::Replace(
    const WorkspaceConnectionIdentity& connection,
    const std::vector<SchemaMetadataRecord>& records, std::string* error) {
  Clear();

  nodes_.push_back({ExplorerNodeKind::connection,
                    static_cast<std::size_t>(-1),
                    connection.display_name.empty() ? "Connection"
                                                    : connection.display_name,
                    {}, {}, {}, 0u});

  std::map<std::string, std::size_t> catalogs;
  std::map<std::pair<std::string, std::string>, std::size_t> schemas;
  std::map<std::string, std::size_t> relation_nodes;
  std::map<std::string, std::size_t> relation_models;

  const auto ensure_catalog = [&](const std::string& catalog) -> std::size_t {
    if (catalog.empty()) return 0u;
    auto existing = catalogs.find(catalog);
    if (existing != catalogs.end()) return existing->second;
    const std::size_t index = nodes_.size();
    nodes_.push_back({ExplorerNodeKind::catalog, 0u, catalog, catalog, {}, {}, 0u});
    catalogs.emplace(catalog, index);
    return index;
  };

  const auto ensure_schema = [&](const std::string& catalog,
                                 const std::string& schema) -> std::size_t {
    if (schema.empty()) return ensure_catalog(catalog);
    const auto key = std::make_pair(catalog, schema);
    auto existing = schemas.find(key);
    if (existing != schemas.end()) return existing->second;
    const std::size_t parent = ensure_catalog(catalog);
    const std::size_t index = nodes_.size();
    nodes_.push_back({ExplorerNodeKind::schema, parent, schema, catalog, schema, {}, 0u});
    schemas.emplace(key, index);
    return index;
  };

  for (const auto& record : records) {
    if (record.kind == ORM_METADATA_CATALOG) {
      (void)ensure_catalog(record.name);
      continue;
    }
    if (record.kind == ORM_METADATA_SCHEMA) {
      (void)ensure_schema(record.catalog, record.name);
      continue;
    }
    if (record.kind != ORM_METADATA_TABLE &&
        record.kind != ORM_METADATA_VIEW) {
      continue;
    }

    const std::size_t parent = ensure_schema(record.catalog, record.schema);
    const std::size_t index = nodes_.size();
    nodes_.push_back({RelationNodeKind(record.kind), parent, record.name,
                      record.catalog, record.schema, record.name, 0u});
    const std::string key = RelationKey(record.catalog, record.schema, record.name);
    relation_nodes[key] = index;

    RelationMetadata relation;
    relation.kind = record.kind == ORM_METADATA_VIEW ? RelationKind::view
                                                    : RelationKind::table;
    relation.schema = !record.schema.empty() ? record.schema : record.catalog;
    relation.name = record.name;
    relation_models[key] = relations_.size();
    relations_.push_back(std::move(relation));
  }

  for (const auto& record : records) {
    if (record.kind != ORM_METADATA_COLUMN) continue;
    const std::string key =
        RelationKey(record.catalog, record.schema, record.relation);
    const auto node_it = relation_nodes.find(key);
    const auto model_it = relation_models.find(key);
    if (node_it == relation_nodes.end() || model_it == relation_models.end()) {
      if (error != nullptr) *error = "column metadata refers to an unknown relation";
      nodes_.clear();
      relations_.clear();
      return false;
    }
    nodes_.push_back({ExplorerNodeKind::column, node_it->second, record.name,
                      record.catalog, record.schema, record.relation,
                      record.ordinal});
    relations_[model_it->second].columns.push_back(record.name);
  }

  return true;
}

bool SchemaExplorerModel::ApplySelection(
    std::size_t index, SqlWorkspaceSession& session) const {
  if (index >= nodes_.size()) return false;
  const auto& node = nodes_[index];
  switch (node.kind) {
    case ExplorerNodeKind::connection:
      session.SetCatalog(std::string{});
      return true;
    case ExplorerNodeKind::catalog:
      session.SetCatalog(node.catalog);
      return true;
    case ExplorerNodeKind::schema:
      session.SetCatalog(node.catalog);
      session.SetSchema(node.schema);
      return true;
    case ExplorerNodeKind::table:
    case ExplorerNodeKind::view:
    case ExplorerNodeKind::column:
      session.SetCatalog(node.catalog);
      session.SetSchema(node.schema);
      session.SetRelation(node.relation);
      return true;
  }
  return false;
}

bool SchemaExplorerController::Refresh(
    orm_connection_t* connection, WorkspaceConnectionIdentity identity,
    std::string* error) {
  orm_metadata_snapshot_t* snapshot = nullptr;
  orm_error_t orm_error;
  orm_error_init(&orm_error);
  if (orm_connection_metadata_snapshot(connection, &snapshot, &orm_error) !=
      ORM_STATUS_OK) {
    if (error != nullptr) *error = orm_error.message;
    return false;
  }

  uint64_t count = 0u;
  if (orm_metadata_snapshot_count(snapshot, &count, &orm_error) !=
      ORM_STATUS_OK) {
    if (error != nullptr) *error = orm_error.message;
    orm_metadata_snapshot_destroy(snapshot);
    return false;
  }

  if (count > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
    if (error != nullptr) *error = "metadata snapshot exceeds platform size";
    orm_metadata_snapshot_destroy(snapshot);
    return false;
  }
  std::vector<SchemaMetadataRecord> records;
  records.reserve(static_cast<std::size_t>(count));
  for (uint64_t i = 0u; i < count; ++i) {
    orm_metadata_entry_t entry = ORM_METADATA_ENTRY_INIT;
    if (orm_metadata_snapshot_get(snapshot, i, &entry, &orm_error) !=
        ORM_STATUS_OK) {
      if (error != nullptr) *error = orm_error.message;
      orm_metadata_snapshot_destroy(snapshot);
      return false;
    }
    records.push_back({entry.kind, ToString(entry.catalog), ToString(entry.schema),
                       ToString(entry.relation), ToString(entry.name), entry.ordinal});
  }
  orm_metadata_snapshot_destroy(snapshot);

  if (!model_.Replace(identity, records, error)) return false;
  session_.SetConnection(std::move(identity));
  session_.ReplaceRelations(model_.relations());
  return true;
}

}  // namespace turbodb::app
