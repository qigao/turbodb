#pragma once

#include <string>
#include <utility>

#include <atlbase.h>
#include <atlapp.h>

extern CAppModule _Module;

#include <atlwin.h>
#include <atlctrls.h>
#include <atlsplit.h>

#include "connection/connection_manager.h"
#include "editor/scintilla_runtime.h"
#include "editor/sql_editor.h"
#include "explorer/schema_explorer_model.h"
#include "explorer/schema_explorer_view.h"
#include "language/sql_language_service.h"
#include "workspace/sql_workspace_session.h"

namespace turbodb::app {

class MainWindow final : public CWindowImpl<MainWindow> {
 public:
  explicit MainWindow(ScintillaRuntime& editor_runtime) noexcept
      : editor_runtime_(editor_runtime),
        explorer_controller_(explorer_model_, workspace_session_) {}

  bool OpenConnection(const ConnectionProfile& profile, std::string* error);
  bool CloseActiveConnection(std::string* error);

  DECLARE_WND_CLASS_EX(L"TurboDBStudioMainWindow", CS_DBLCLKS,
                       COLOR_WINDOW)

  BEGIN_MSG_MAP(MainWindow)
    MESSAGE_HANDLER(WM_CREATE, OnCreate)
    MESSAGE_HANDLER(WM_SIZE, OnSize)
    MESSAGE_HANDLER(kExplorerSelectionChanged, OnExplorerSelectionChanged)
    MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
  END_MSG_MAP()

 private:
  LRESULT OnCreate(UINT message, WPARAM wparam, LPARAM lparam,
                   BOOL &handled);
  LRESULT OnSize(UINT message, WPARAM wparam, LPARAM lparam, BOOL &handled);
  LRESULT OnExplorerSelectionChanged(UINT message, WPARAM wparam,
                                     LPARAM lparam, BOOL& handled);
  LRESULT OnDestroy(UINT message, WPARAM wparam, LPARAM lparam,
                    BOOL &handled);

  ScintillaRuntime& editor_runtime_;
  ConnectionManager connections_;
  SqlWorkspaceSession workspace_session_;
  SqlLanguageService language_service_;
  SchemaExplorerModel explorer_model_;
  SchemaExplorerController explorer_controller_;

  CSplitterWindow workspace_splitter_;
  CHorSplitterWindow query_splitter_;
  SchemaExplorerView explorer_;
  SqlEditor editor_;
  CStatic result_placeholder_;
};

}  // namespace turbodb::app
