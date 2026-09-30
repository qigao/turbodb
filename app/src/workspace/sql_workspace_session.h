#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace turbodb::app {

enum class SqlProvider {
  unknown,
  sqlite,
  mysql,
  postgresql,
  tidesdb,
};

enum class WorkspaceExecutionState {
  idle,
  running,
  cancelling,
};

enum class RelationKind {
  table,
  view,
};

struct WorkspaceConnectionIdentity {
  std::uint64_t id = 0;
  SqlProvider provider = SqlProvider::unknown;
  std::uint64_t capabilities = 0;
  std::string display_name;
};

struct RelationMetadata {
  RelationKind kind = RelationKind::table;
  std::string schema;
  std::string name;
  std::vector<std::string> columns;
};

class SqlWorkspaceSession {
 public:
  const std::optional<WorkspaceConnectionIdentity>& connection() const noexcept {
    return connection_;
  }

  SqlProvider provider() const noexcept {
    return connection_ ? connection_->provider : SqlProvider::unknown;
  }

  void SetConnection(WorkspaceConnectionIdentity connection) {
    connection_ = std::move(connection);
    catalog_.clear();
    schema_.clear();
    relations_.clear();
    execution_state_ = WorkspaceExecutionState::idle;
  }

  void ClearConnection() {
    connection_.reset();
    catalog_.clear();
    schema_.clear();
    relations_.clear();
    execution_state_ = WorkspaceExecutionState::idle;
  }

  const std::string& catalog() const noexcept { return catalog_; }
  const std::string& schema() const noexcept { return schema_; }

  void SetCatalog(std::string catalog) { catalog_ = std::move(catalog); }
  void SetSchema(std::string schema) { schema_ = std::move(schema); }

  const std::vector<RelationMetadata>& relations() const noexcept {
    return relations_;
  }

  void ReplaceRelations(std::vector<RelationMetadata> relations) {
    relations_ = std::move(relations);
  }

  WorkspaceExecutionState execution_state() const noexcept {
    return execution_state_;
  }

  void SetExecutionState(WorkspaceExecutionState state) noexcept {
    execution_state_ = state;
  }

 private:
  // This session intentionally stores only stable application identity.
  // orm_connection_t/native driver handles remain owned by a later connection
  // controller/manager rather than the editor or workspace view.
  std::optional<WorkspaceConnectionIdentity> connection_;
  std::string catalog_;
  std::string schema_;
  std::vector<RelationMetadata> relations_;
  WorkspaceExecutionState execution_state_ = WorkspaceExecutionState::idle;
};

}  // namespace turbodb::app
