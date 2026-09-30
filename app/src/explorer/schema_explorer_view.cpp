#include "explorer/schema_explorer_view.h"

#include <limits>
#include <string>

namespace turbodb::app {

namespace {

std::wstring ToWide(const std::string& text) {
  if (text.empty()) return {};
  const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         text.data(),
                                         static_cast<int>(text.size()),
                                         nullptr, 0);
  if (size <= 0) return L"?";
  std::wstring output(static_cast<std::size_t>(size), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), output.data(),
                            size) != size) {
    return L"?";
  }
  return output;
}

}  // namespace

void SchemaExplorerView::Bind(const SchemaExplorerModel* model,
                              SqlWorkspaceSession* session) noexcept {
  model_ = model;
  session_ = session;
}

LRESULT SchemaExplorerView::OnCreate(UINT, WPARAM, LPARAM, BOOL&) {
  RECT client{};
  GetClientRect(&client);
  const DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS |
                      TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS;
  if (tree_.Create(m_hWnd, client, nullptr, style, WS_EX_CLIENTEDGE,
                   kTreeControlId) == nullptr) {
    return -1;
  }
  Refresh();
  return 0;
}

LRESULT SchemaExplorerView::OnSize(UINT, WPARAM, LPARAM lparam, BOOL&) {
  if (tree_.IsWindow()) {
    tree_.SetWindowPos(nullptr, 0, 0, LOWORD(lparam), HIWORD(lparam),
                       SWP_NOACTIVATE | SWP_NOZORDER);
  }
  return 0;
}

void SchemaExplorerView::Refresh() {
  if (!tree_.IsWindow()) return;
  tree_.DeleteAllItems();
  items_.clear();
  if (model_ == nullptr) return;

  const auto& nodes = model_->nodes();
  items_.resize(nodes.size(), nullptr);
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    const auto& node = nodes[index];
    const std::wstring label = ToWide(node.display_name);

    TVINSERTSTRUCTW insert{};
    insert.hParent = node.parent == static_cast<std::size_t>(-1)
                         ? TVI_ROOT
                         : items_[node.parent];
    insert.hInsertAfter = TVI_LAST;
    insert.item.mask = TVIF_TEXT | TVIF_PARAM;
    insert.item.pszText = const_cast<wchar_t*>(label.c_str());
    insert.item.lParam = static_cast<LPARAM>(index);
    items_[index] = TreeView_InsertItem(tree_.m_hWnd, &insert);
  }

  if (!items_.empty()) tree_.Expand(items_.front());
}

LRESULT SchemaExplorerView::OnSelectionChanged(int, LPNMHDR header,
                                               BOOL&) {
  if (header == nullptr || model_ == nullptr || session_ == nullptr)
    return 0;
  const auto* change = reinterpret_cast<const NMTREEVIEWW*>(header);
  TVITEMW item{};
  item.mask = TVIF_PARAM;
  item.hItem = change->itemNew.hItem;
  if (!TreeView_GetItem(tree_.m_hWnd, &item) || item.lParam < 0)
    return 0;

  const auto index = static_cast<std::size_t>(item.lParam);
  if (model_->ApplySelection(index, *session_)) {
    ::PostMessageW(GetParent(), kExplorerSelectionChanged,
                   static_cast<WPARAM>(index), 0);
  }
  return 0;
}

}  // namespace turbodb::app
