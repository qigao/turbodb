#pragma once

#include <cstdint>
#include <string>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlctrls.h>

#include "query/query_result_model.h"

namespace turbodb::app {

class ResultWorkspaceView final : public CWindowImpl<ResultWorkspaceView> {
 public:
  DECLARE_WND_CLASS_EX(L"TurboDBStudioResultWorkspace", CS_DBLCLKS,
                       COLOR_WINDOW)

  BEGIN_MSG_MAP(ResultWorkspaceView)
    MESSAGE_HANDLER(WM_CREATE, OnCreate)
    MESSAGE_HANDLER(WM_SIZE, OnSize)
  END_MSG_MAP()

  void SetRunning(std::uint64_t request_id);
  void ShowError(std::string message);
  void Render(const QueryResultSnapshot& result);

 private:
  LRESULT OnCreate(UINT message, WPARAM wparam, LPARAM lparam, BOOL& handled);
  LRESULT OnSize(UINT message, WPARAM wparam, LPARAM lparam, BOOL& handled);

  void Layout(int width, int height);
  void ShowGrid(bool visible);
  static std::wstring Utf8(std::string_view text);
  static std::wstring CellText(const QueryCell& cell);

  CListViewCtrl grid_;
  CEdit message_;
  CStatic status_;
};

}  // namespace turbodb::app
