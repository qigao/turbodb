#pragma once

#include <cstddef>
#include <vector>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlctrls.h>
#include <atlgdi.h>

#include "explorer/schema_explorer_model.h"

namespace turbodb::app {

constexpr UINT kExplorerSelectionChanged = WM_APP + 0x120;

class SchemaExplorerView final : public CWindowImpl<SchemaExplorerView> {
 public:
  static constexpr UINT kTreeControlId = 4101u;

  DECLARE_WND_CLASS_EX(L"TurboDBStudioSchemaExplorer", CS_DBLCLKS,
                       COLOR_WINDOW)

  BEGIN_MSG_MAP(SchemaExplorerView)
    MESSAGE_HANDLER(WM_CREATE, OnCreate)
    MESSAGE_HANDLER(WM_SIZE, OnSize)
    MESSAGE_HANDLER(WM_CTLCOLORSTATIC, OnControlColor)
    MESSAGE_HANDLER(WM_CONTEXTMENU, OnContextMenu)
    NOTIFY_HANDLER(kTreeControlId, TVN_SELCHANGED, OnSelectionChanged)
    NOTIFY_HANDLER(kTreeControlId, NM_DBLCLK, OnDoubleClick)
  END_MSG_MAP()

  void Bind(const SchemaExplorerModel* model,
            SqlWorkspaceSession* session) noexcept;
  void Refresh();
  bool RefreshColors();
  bool HandleKey(MSG* message);
  bool HasSelection() const { return SelectedNode() != nullptr; }
  bool CanOpenSelection() const;
  std::string SelectedQuery() const;
  void CopySelectedName();
  void SetRefreshing(bool refreshing) {
    heading_.SetWindowText(refreshing ? L"  Database - refreshing..." : L"  Database");
    tree_.EnableWindow(!refreshing);
  }
  void FocusContent() {
    if ((empty_.GetStyle() & WS_VISIBLE) != 0) empty_.SetFocus();
    else tree_.SetFocus();
  }

 private:
  LRESULT OnCreate(UINT message, WPARAM wparam, LPARAM lparam, BOOL& handled);
  LRESULT OnSize(UINT message, WPARAM wparam, LPARAM lparam, BOOL& handled);
  LRESULT OnSelectionChanged(int id_ctrl, LPNMHDR header, BOOL& handled);
  LRESULT OnControlColor(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnContextMenu(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnDoubleClick(int, LPNMHDR, BOOL&);
  const ExplorerNode* SelectedNode() const;
  void Layout(int width, int height);

  const SchemaExplorerModel* model_ = nullptr;
  SqlWorkspaceSession* session_ = nullptr;
  CTreeViewCtrl tree_;
  CStatic heading_;
  CEdit empty_;
  CFont font_;
  CImageListManaged images_;
  std::vector<HTREEITEM> items_;
};

}  // namespace turbodb::app
