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
};

}  // namespace turbodb::app
