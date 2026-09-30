#pragma once

#include <cstddef>
#include <vector>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlctrls.h>

#include "explorer/schema_explorer_model.h"

namespace turbodb::app {

constexpr UINT kExplorerSelectionChanged = WM_APP + 0x120;

class SchemaExplorerView final : public CWindowImpl<SchemaExplorerView> {
 public:
  DECLARE_WND_CLASS_EX(L"TurboDBStudioSchemaExplorer", CS_DBLCLKS,
                       COLOR_WINDOW)

  BEGIN_MSG_MAP(SchemaExplorerView)
    MESSAGE_HANDLER(WM_CREATE, OnCreate)
    MESSAGE_HANDLER(WM_SIZE, OnSize)
    NOTIFY_HANDLER(kTreeControlId, TVN_SELCHANGED, OnSelectionChanged)
  END_MSG_MAP()

  void Bind(const SchemaExplorerModel* model,
            SqlWorkspaceSession* session) noexcept;
  void Refresh();

 private:
  static constexpr UINT kTreeControlId = 4101u;

  LRESULT OnCreate(UINT message, WPARAM wparam, LPARAM lparam, BOOL& handled);
  LRESULT OnSize(UINT message, WPARAM wparam, LPARAM lparam, BOOL& handled);
  LRESULT OnSelectionChanged(int id_ctrl, LPNMHDR header, BOOL& handled);

  const SchemaExplorerModel* model_ = nullptr;
  SqlWorkspaceSession* session_ = nullptr;
  CTreeViewCtrl tree_;
  std::vector<HTREEITEM> items_;
};

}  // namespace turbodb::app
