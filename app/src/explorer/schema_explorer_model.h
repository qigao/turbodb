#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <orm.h>

#include "workspace/sql_workspace_session.h"

namespace turbodb::app {

enum class ExplorerNodeKind {
  connection,
  catalog,
  schema,
  table,
  view,
  column,
};

struct SchemaMetadataRecord {
  orm_metadata_kind_t kind = ORM_METADATA_CATALOG;
  std::string catalog;
  std::string schema;
  std::string relation;
  std::string name;
  std::uint32_t ordinal = 0;
};

struct ExplorerNode {
  ExplorerNodeKind kind = ExplorerNodeKind::connection;
  std::size_t parent = static_cast<std::size_t>(-1);
  std::string display_name;
  std::string catalog;
  std::string schema;
  std::string relation;
  std::uint32_t ordinal = 0;
};

class SchemaExplorerModel {
 public:
  bool Replace(const WorkspaceConnectionIdentity& connection,
               const std::vector<SchemaMetadataRecord>& records,
               std::string* error);

  const std::vector<ExplorerNode>& nodes() const noexcept { return nodes_; }
  const std::vector<RelationMetadata>& relations() const noexcept {
    return relations_;
  }

  bool ApplySelection(std::size_t index, SqlWorkspaceSession& session) const;

 private:
  std::vector<ExplorerNode> nodes_;
  std::vector<RelationMetadata> relations_;
};

class SchemaExplorerController {
 public:
  SchemaExplorerController(SchemaExplorerModel& model,
                           SqlWorkspaceSession& session) noexcept
      : model_(model), session_(session) {}

  bool Refresh(orm_connection_t* connection,
               WorkspaceConnectionIdentity identity,
               std::string* error);

 private:
  SchemaExplorerModel& model_;
  SqlWorkspaceSession& session_;
};

}  // namespace turbodb::app
