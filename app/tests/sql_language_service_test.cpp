#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

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

  return 0;
}
