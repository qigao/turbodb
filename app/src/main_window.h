#pragma once

#include <atlbase.h>
#include <atlapp.h>

extern CAppModule _Module;

#include <atlwin.h>
#include <atlctrls.h>
#include <atlsplit.h>

namespace turbodb::app {

class MainWindow final : public CWindowImpl<MainWindow> {
 public:
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

  CSplitterWindow workspace_splitter_;
  CHorSplitterWindow query_splitter_;
  CStatic explorer_placeholder_;
  CStatic editor_placeholder_;
  CStatic result_placeholder_;
};

}  // namespace turbodb::app
