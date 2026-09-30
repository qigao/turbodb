#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "explorer/schema_explorer_model.h"
#include "language/sql_language_service.h"
#include "workspace/sql_workspace_session.h"

namespace {

bool Contains(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

int Fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main() {
  using namespace turbodb::app;

  SqlWorkspaceSession session;
  SqlLanguageService language;

  if (language.LexerName(session.provider()) != "sql") {
    return Fail("unknown provider must use the generic SQL lexer");
  }

  session.SetConnection({42u, SqlProvider::mysql, 0x55u, "local-mysql"});
  session.SetCatalog("app");
  session.SetSchema("public");
  session.ReplaceRelations({
      {RelationKind::table, "app", "users", {"id", "name", "email"}},
      {RelationKind::view, "app", "user_summary", {"id", "name"}},
  });

  if (language.LexerName(session.provider()) != "mysql") {
    return Fail("MySQL provider must select the MySQL lexer");
  }

  const auto relation_completion = language.Complete(session, "use");
  if (!Contains(relation_completion, "users") ||
      !Contains(relation_completion, "user_summary")) {
    return Fail("relation metadata must participate in completion");
  }

  const auto column_completion = language.Complete(session, "users.");
  if (!Contains(column_completion, "users.id") ||
      !Contains(column_completion, "users.name") ||
      !Contains(column_completion, "users.email")) {
    return Fail("qualified column metadata must participate in completion");
  }

  const auto schema_column_completion = language.Complete(session, "app.users.");
  if (!Contains(schema_column_completion, "app.users.id") ||
      !Contains(schema_column_completion, "app.users.email")) {
    return Fail("schema-qualified columns must participate in completion");
  }

  session.SetConnection(
      {77u, SqlProvider::postgresql, 0xaau, "local-postgresql"});
  if (!session.catalog().empty() || !session.schema().empty() ||
      !session.relations().empty()) {
    return Fail("changing connections must invalidate scoped metadata");
  }

  const auto provider_completion = language.Complete(session, "ret");
  if (!Contains(provider_completion, "returning")) {
    return Fail("provider keywords must participate in completion");
  }

  session.SetExecutionState(WorkspaceExecutionState::running);
  session.ClearConnection();
  if (session.connection().has_value() ||
      session.execution_state() != WorkspaceExecutionState::idle) {
    return Fail("clearing a connection must reset execution state");
  }

  SchemaExplorerModel explorer;
  std::string explorer_error;
  const WorkspaceConnectionIdentity identity{
      101u, SqlProvider::postgresql, 0x11u, "local-postgresql"};
  const std::vector<SchemaMetadataRecord> records = {
      {ORM_METADATA_CATALOG, "", "", "", "appdb", 0u},
      {ORM_METADATA_SCHEMA, "appdb", "", "", "public", 0u},
      {ORM_METADATA_TABLE, "appdb", "public", "", "users", 0u},
      {ORM_METADATA_COLUMN, "appdb", "public", "users", "id", 0u},
      {ORM_METADATA_COLUMN, "appdb", "public", "users", "name", 1u},
      {ORM_METADATA_VIEW, "appdb", "public", "", "user_summary", 0u},
      {ORM_METADATA_COLUMN, "appdb", "public", "user_summary", "name", 0u},
  };
  if (!explorer.Replace(identity, records, &explorer_error)) {
    return Fail("schema explorer model rejected valid metadata");
  }
  if (explorer.nodes().size() != 8u || explorer.relations().size() != 2u) {
    return Fail("schema explorer model shape mismatch");
  }

  session.SetConnection(identity);
  session.ReplaceRelations(explorer.relations());
  std::size_t users_node = static_cast<std::size_t>(-1);
  for (std::size_t i = 0; i < explorer.nodes().size(); ++i) {
    const auto& node = explorer.nodes()[i];
    if (node.kind == ExplorerNodeKind::table && node.relation == "users") {
      users_node = i;
      break;
    }
  }
  if (users_node == static_cast<std::size_t>(-1) ||
      !explorer.ApplySelection(users_node, session)) {
    return Fail("schema explorer could not select users table");
  }
  if (session.catalog() != "appdb" || session.schema() != "public" ||
      session.relation() != "users") {
    return Fail("schema explorer selection did not update workspace context");
  }

  const auto explorer_completion = language.Complete(session, "users.");
  if (!Contains(explorer_completion, "users.id") ||
      !Contains(explorer_completion, "users.name")) {
    return Fail("explorer metadata did not feed SQL completion");
  }

  session.SetCatalog("otherdb");
  if (!session.schema().empty() || !session.relation().empty()) {
    return Fail("catalog selection must clear deeper workspace context");
  }

  return 0;
}
