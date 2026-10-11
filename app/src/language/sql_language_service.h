#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "workspace/sql_workspace_session.h"

namespace turbodb::app {

class SqlLanguageService {
 public:
  std::string LexerName(SqlProvider provider) const;
  std::string KeywordList(SqlProvider provider) const;
  std::vector<std::string> Complete(const SqlWorkspaceSession& session,
                                    std::string_view prefix) const;
  // Curated built-ins only; an empty result means no verified signature.
  std::string FunctionSignature(SqlProvider provider, std::string_view name) const;
  // Loaded metadata only. Ambiguous unqualified names list each matching owner.
  std::string DescribeSymbol(const SqlWorkspaceSession& session,
                             std::string_view identifier) const;
};

}  // namespace turbodb::app
