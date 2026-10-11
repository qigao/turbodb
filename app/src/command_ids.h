#pragma once

#include <windows.h>

namespace turbodb::app {

enum StudioCommand : WORD {
  kFileExit = 4301,
  kEditUndo,
  kEditRedo,
  kEditCut,
  kEditCopy,
  kEditPaste,
  kEditSelectAll,
  kQueryExecute,
  kQueryPlan,
  kQueryAnalyze,
  kViewExplorer,
  kViewEditor,
  kViewResults,
  kViewPlanDetails,
  kViewToolbar,
  kHelpAbout,
  kFileConnect,
  kFileDisconnect,
  kQueryNew,
  kQueryClose,
  kQueryNext,
  kQueryPrevious,
  kExplorerOpen,
  kExplorerSelect,
  kExplorerCopyName,
  kExplorerRefresh,
  kViewQueryTabs,
};

// One stable command per INI profile (the parser admits at most 32).
constexpr WORD kRecentConnectionFirst = 4501;
constexpr WORD kRecentConnectionLast = kRecentConnectionFirst + 31;

}  // namespace turbodb::app
