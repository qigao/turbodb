#pragma once

#include <atlbase.h>
#include <atlapp.h>

extern CAppModule _Module;

#include <atlwin.h>
#include <atlctrls.h>
#include <atlsplit.h>

#include "editor/scintilla_runtime.h"
#include "editor/sql_editor.h"
#include "language/sql_language_service.h"
#include "workspace/sql_workspace_session.h"

namespace turbodb::app {

class MainWindow final : public CWindowImpl<MainWindow> {
 public:
  explicit MainWindow(ScintillaRuntime& editor_runtime) noexcept
      : editor_runtime_(editor_runtime) {}

  DECLARE_WND_CLASS_EX(L"TurboDBStudioMainWindow", CS_DBLCLKS,
                       COLOR_WINDOW)

  BEGIN_MSG_MAP(MainWindow)
    MESSAGE_HANDLER(WM_CREATE, OnCreate)
    MESSAGE_HANDLER(WM_SIZE, OnSize)
    MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
  END_MSG_MAP()

 private:
  LRESULT OnCreate(UINT message, WPARAM wparam, LPARAM lparam,
                   BOOL &handled);
  LRESULT OnSize(UINT message, WPARAM wparam, LPARAM lparam, BOOL &handled);
  LRESULT OnDestroy(UINT message, WPARAM wparam, LPARAM lparam,
                    BOOL &handled);

  ScintillaRuntime& editor_runtime_;
  SqlWorkspaceSession workspace_session_;
  SqlLanguageService language_service_;

  CSplitterWindow workspace_splitter_;
  CHorSplitterWindow query_splitter_;
  CStatic explorer_placeholder_;
  SqlEditor editor_;
  CStatic result_placeholder_;
};

}  // namespace turbodb::app
