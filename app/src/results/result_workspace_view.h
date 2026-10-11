#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <array>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlctrls.h>
#include <atlgdi.h>

#include "plan/execution_plan_model.h"
#include "query/query_result_model.h"

namespace turbodb::app {

class ResultWorkspaceView final : public CWindowImpl<ResultWorkspaceView> {
 public:
  DECLARE_WND_CLASS_EX(L"TurboDBStudioResultWorkspace", CS_DBLCLKS,
                       COLOR_WINDOW)

  BEGIN_MSG_MAP(ResultWorkspaceView)
    MESSAGE_HANDLER(WM_CREATE, OnCreate)
    MESSAGE_HANDLER(WM_SIZE, OnSize)
    MESSAGE_HANDLER(WM_CTLCOLORSTATIC, OnControlColor)
    MESSAGE_HANDLER(WM_CONTEXTMENU, OnContextMenu)
    MESSAGE_HANDLER(WM_COMMAND, OnCopyCommand)
    NOTIFY_HANDLER(kGridId, NM_CUSTOMDRAW, OnGridCustomDraw)
    NOTIFY_HANDLER(kTabsId, TCN_SELCHANGE, OnTabChanged)
  END_MSG_MAP()

  void SetRunning();
  void SetExplainRunning(orm_explain_mode_t mode);
  void ShowError(std::string message);
  void Render(const QueryResultSnapshot& result);
  void RenderPlan(const ExecutionPlanSnapshot& plan);
  void FocusContent();
  void FocusPlanDetails();
  bool HasPlanDetails() const;
  bool HandleKey(MSG* message, HWND return_focus);
  void Clear(bool connected = false);
  static constexpr UINT kTabsId = 4206;
  static constexpr UINT kCopyCell = 4210, kCopyRow = 4211, kCopyAll = 4212;
  bool GridText(UINT command, std::wstring& text) const;

 private:
  static constexpr UINT kGridId = 4201u;
  LRESULT OnCreate(UINT message, WPARAM wparam, LPARAM lparam, BOOL& handled);
  LRESULT OnSize(UINT message, WPARAM wparam, LPARAM lparam, BOOL& handled);
  LRESULT OnControlColor(UINT, WPARAM wparam, LPARAM lparam, BOOL& handled);
  LRESULT OnGridCustomDraw(int, LPNMHDR header, BOOL& handled);
  LRESULT OnTabChanged(int, LPNMHDR, BOOL&);
  LRESULT OnContextMenu(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnCopyCommand(UINT, WPARAM, LPARAM, BOOL&);
  void SelectPage(int page);
  bool CopyGrid(UINT command);

  void Layout(int width, int height);
  void ShowGrid(bool visible);
  void ShowPlan(bool visible);
  enum class Tone { neutral, success, busy, error };
  void SetStatus(const wchar_t* text, Tone tone);
  static std::wstring Utf8(std::string_view text);
  static std::wstring CellText(const QueryCell& cell);
  static std::wstring PlanNodeText(const ExecutionPlanNodeSnapshot& node);

  CListViewCtrl grid_;
  CEdit message_;
  CTreeViewCtrl plan_tree_;
  CEdit plan_raw_;
  std::vector<HTREEITEM> plan_items_;
  CStatic status_;
  CTabCtrl tabs_;
  CFont font_;
  Tone tone_ = Tone::neutral;
  int selected_page_ = 1;
  int selected_column_ = 0;
  bool has_plan_ = false;
  std::array<std::wstring, 3> page_status_;
  std::array<Tone, 3> page_tone_{};
};

}  // namespace turbodb::app
