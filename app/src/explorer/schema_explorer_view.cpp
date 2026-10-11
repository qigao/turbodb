#include "explorer/schema_explorer_view.h"
#include "command_ids.h"
#include "clipboard.h"
#include <atluser.h>
#include "studio_colors.h"

#include <limits>
#include <string>

namespace turbodb::app {

namespace {

constexpr int kHeadingHeight = 26;
constexpr int kIconSize = 16;
constexpr int kIconCount = 5;
constexpr COLORREF kTransparent = RGB(255, 0, 255);

int NodeImage(ExplorerNodeKind kind) {
  switch (kind) {
    case ExplorerNodeKind::connection: return 0;
    case ExplorerNodeKind::catalog:
    case ExplorerNodeKind::schema: return 1;
    case ExplorerNodeKind::table: return 2;
    case ExplorerNodeKind::view: return 3;
    case ExplorerNodeKind::column: return 4;
  }
  return -1;
}

bool DrawNodeGlyph(CDC& dc, int image) {
  const auto line = [&](int x1, int y1, int x2, int y2) {
    const POINT points[] = {{x1, y1}, {x2, y2}};
    return dc.Polyline(points, _countof(points)) != FALSE;
  };
  if (image == 0) {
    return dc.Ellipse(2, 1, 14, 6) && line(2, 3, 2, 12) && line(13, 3, 13, 12) &&
        dc.Arc(2, 5, 14, 10, 2, 7, 13, 7) && dc.Arc(2, 10, 14, 15, 2, 12, 13, 12);
  }
  if (image == 1) {
    const POINT folder[] = {{1, 4}, {1, 2}, {6, 2}, {8, 4}, {14, 4},
                            {14, 13}, {1, 13}, {1, 4}, {14, 4}};
    return dc.Polyline(folder, _countof(folder)) != FALSE;
  }
  if (image == 4)
    return dc.Rectangle(4, 1, 12, 15) && line(4, 5, 11, 5) && line(4, 9, 11, 9);
  const bool table = dc.Rectangle(1, 2, 15, 14) && line(1, 6, 14, 6) &&
      line(1, 10, 14, 10) && line(5, 6, 5, 13) && line(10, 6, 10, 13);
  return table && (image != 3 || line(3, 0, 12, 0));
}

bool CreateNodeImages(CImageListManaged& images, HDC reference, const StudioColors& colors) {
  if (!images.Create(kIconSize, kIconSize, ILC_COLOR32 | ILC_MASK, kIconCount, 0))
    return false;
  const COLORREF accents[] = {colors.purple, colors.amber, colors.blue, colors.teal, colors.muted};
  for (int image = 0; image < kIconCount; ++image) {
    CDC dc;
    CBitmap bitmap;
    CPen pen;
    CBrush background;
    if (dc.CreateCompatibleDC(reference) == nullptr ||
        bitmap.CreateCompatibleBitmap(reference, kIconSize, kIconSize) == nullptr ||
        pen.CreatePen(PS_SOLID, 1, accents[image]) == nullptr ||
        background.CreateSolidBrush(kTransparent) == nullptr)
      return false;
    const auto old_bitmap = dc.SelectBitmap(bitmap);
    const auto old_pen = dc.SelectPen(pen);
    const auto old_brush = dc.SelectStockBrush(NULL_BRUSH);
    RECT bounds{0, 0, kIconSize, kIconSize};
    const bool drawn = dc.FillRect(&bounds, background) && DrawNodeGlyph(dc, image);
    dc.SelectBrush(old_brush);
    dc.SelectPen(old_pen);
    dc.SelectBitmap(old_bitmap);
    if (!drawn || images.Add(bitmap, kTransparent) != image) return false;
  }
  return true;
}

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
                      TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_TRACKSELECT;
  if (tree_.Create(m_hWnd, client, nullptr, style, 0,
                   kTreeControlId) == nullptr) {
    return -1;
  }
  if (heading_.Create(m_hWnd, client, L"  Database",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS) == nullptr ||
      empty_.Create(m_hWnd, client, L"No active connection.\r\n\r\nFile > New Connection\r\nCtrl+N",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL) == nullptr)
    return -1;
  NONCLIENTMETRICSW metrics{sizeof(metrics)};
  if (!::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) ||
      font_.CreateFontIndirect(&metrics.lfMessageFont) == nullptr)
    return -1;
  tree_.SetFont(font_);
  heading_.SetFont(font_);
  empty_.SetFont(font_);
  if (!RefreshColors()) return -1;
  Refresh();
  Layout(client.right, client.bottom);
  return 0;
}

LRESULT SchemaExplorerView::OnSize(UINT, WPARAM, LPARAM lparam, BOOL&) {
  Layout(LOWORD(lparam), HIWORD(lparam));
  return 0;
}

void SchemaExplorerView::Layout(int width, int height) {
  const int top = height < kHeadingHeight ? height : kHeadingHeight;
  if (heading_.IsWindow())
    heading_.SetWindowPos(nullptr, 0, 0, width, top, SWP_NOACTIVATE | SWP_NOZORDER);
  if (tree_.IsWindow()) {
    tree_.SetWindowPos(nullptr, 0, top, width, height - top,
                       SWP_NOACTIVATE | SWP_NOZORDER);
  }
  if (empty_.IsWindow()) {
    empty_.SetWindowPos(nullptr, 0, top, width, height - top,
                        SWP_NOACTIVATE | SWP_NOZORDER);
    RECT text{12, 12, width > 24 ? width - 12 : 12,
              height - top > 24 ? height - top - 12 : 12};
    empty_.SetRect(&text);
  }
}

bool SchemaExplorerView::RefreshColors() {
  if (!tree_.IsWindow()) return false;
  const auto colors = CurrentStudioColors();
  CImageListManaged replacement;
  CClientDC reference(tree_);
  if (reference.IsNull() || !CreateNodeImages(replacement, reference, colors)) return false;
  // The tree borrows this complete list; replacing images never rebuilds items
  // or changes native expansion, selection, or keyboard state.
  tree_.SetImageList(replacement, TVSIL_NORMAL);
  images_.Attach(replacement.Detach());
  tree_.SetBkColor(colors.paper);
  tree_.SetTextColor(colors.ink);
  Invalidate();
  return true;
}

LRESULT SchemaExplorerView::OnControlColor(UINT, WPARAM wparam, LPARAM lparam, BOOL&) {
  const auto colors = CurrentStudioColors();
  const bool heading = reinterpret_cast<HWND>(lparam) == heading_;
  return reinterpret_cast<LRESULT>(StudioControlColors(reinterpret_cast<HDC>(wparam),
      heading ? colors.ink : colors.muted,
      heading ? colors.surface : colors.paper));
}

void SchemaExplorerView::Refresh() {
  if (!tree_.IsWindow()) return;
  tree_.DeleteAllItems();
  items_.clear();
  const bool empty = model_ == nullptr || model_->nodes().empty();
  // Mutually exclusive bodies avoid overlapping native child windows. The
  // read-only empty state remains focusable and exposes its guidance as text.
  empty_.ShowWindow(empty ? SW_SHOW : SW_HIDE);
  tree_.ShowWindow(empty ? SW_HIDE : SW_SHOW);
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
    insert.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    insert.item.iImage = NodeImage(node.kind);
    insert.item.iSelectedImage = insert.item.iImage;
    insert.item.pszText = const_cast<wchar_t*>(label.c_str());
    insert.item.lParam = static_cast<LPARAM>(index);
    if (insert.hParent == TVI_ROOT) {
      insert.item.mask |= TVIF_STATE;
      insert.item.stateMask = TVIS_BOLD;
      insert.item.state = TVIS_BOLD;
    }
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

const ExplorerNode* SchemaExplorerView::SelectedNode() const {
  if (model_ == nullptr || !tree_.IsWindow()) return nullptr;
  TVITEMW item{}; item.mask = TVIF_PARAM; item.hItem = tree_.GetSelectedItem();
  if (item.hItem == nullptr || !TreeView_GetItem(tree_, &item) || item.lParam < 0 ||
      static_cast<std::size_t>(item.lParam) >= model_->nodes().size()) return nullptr;
  return &model_->nodes()[static_cast<std::size_t>(item.lParam)];
}

bool SchemaExplorerView::CanOpenSelection() const {
  const auto* node = SelectedNode();
  return node != nullptr && (node->kind == ExplorerNodeKind::table || node->kind == ExplorerNodeKind::view);
}

std::string SchemaExplorerView::SelectedQuery() const {
  if (!CanOpenSelection() || session_ == nullptr || session_->provider() == SqlProvider::unknown) return {};
  const auto& node = *SelectedNode();
  const char delimiter = session_->provider() == SqlProvider::mysql ? '`' : '"';
  const auto quote = [delimiter](const std::string& name) {
    std::string result(1, delimiter);
    for (char ch : name) { result += ch; if (ch == delimiter) result += ch; }
    result += delimiter;
    return result;
  };
  // SQLite's attached-database name, like MySQL's database, is a catalog in ORM metadata.
  const auto& qualifier = session_->provider() != SqlProvider::postgresql && node.schema.empty()
      ? node.catalog : node.schema;
  if (node.relation.empty() || node.relation.find('\0') != std::string::npos ||
      qualifier.find('\0') != std::string::npos) return {};
  return "SELECT * FROM " + (qualifier.empty() ? std::string{} : quote(qualifier) + ".") +
      quote(node.relation) + " LIMIT 100;";
}

void SchemaExplorerView::CopySelectedName() {
  const auto* node = SelectedNode();
  if (node != nullptr && !CopyDisplayText(m_hWnd, ToWide(node->display_name)))
    MessageBox(L"Could not copy the name to the clipboard.", L"Copy name", MB_OK | MB_ICONERROR);
}

LRESULT SchemaExplorerView::OnContextMenu(UINT, WPARAM wparam, LPARAM lparam, BOOL& handled) {
  if (reinterpret_cast<HWND>(wparam) != tree_) { handled = FALSE; return 0; }
  POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
  if (point.x == -1 && point.y == -1) {
    RECT item{};
    if (!tree_.GetItemRect(tree_.GetSelectedItem(), &item, TRUE)) return 0;
    point = {item.left, item.bottom}; tree_.ClientToScreen(&point);
  } else {
    TVHITTESTINFO hit{}; hit.pt = point; tree_.ScreenToClient(&hit.pt);
    if (!tree_.HitTest(&hit) || (hit.flags & TVHT_ONITEM) == 0) return 0;
    tree_.SelectItem(hit.hItem);
  }
  tree_.SetFocus();
  CMenu popup; if (!popup.CreatePopupMenu()) return 0;
  const UINT relation = MF_STRING | (CanOpenSelection() ? MF_ENABLED : MF_GRAYED);
  popup.AppendMenu(relation, kExplorerOpen, L"Open First 100 Rows\tEnter");
  popup.AppendMenu(relation, kExplorerSelect, L"SELECT in New Query");
  popup.AppendMenu(MF_SEPARATOR);
  popup.AppendMenu(MF_STRING, kExplorerCopyName, L"Copy Name\tCtrl+C");
  popup.AppendMenu(MF_STRING, kExplorerRefresh, L"Refresh Schema\tCtrl+R");
  popup.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON, point.x, point.y, ::GetAncestor(m_hWnd, GA_ROOT));
  return 0;
}

LRESULT SchemaExplorerView::OnDoubleClick(int, LPNMHDR, BOOL& handled) {
  DWORD position = ::GetMessagePos();
  TVHITTESTINFO hit{}; hit.pt = {static_cast<short>(LOWORD(position)), static_cast<short>(HIWORD(position))};
  tree_.ScreenToClient(&hit.pt);
  tree_.HitTest(&hit);
  if (hit.hItem == tree_.GetSelectedItem() && (hit.flags & TVHT_ONITEMLABEL) && CanOpenSelection()) {
    ::SendMessageW(::GetAncestor(m_hWnd, GA_ROOT), WM_COMMAND, kExplorerOpen, 0);
    return 1;
  }
  handled = FALSE;
  return 0;
}

bool SchemaExplorerView::HandleKey(MSG* message) {
  if (message->hwnd != tree_ || message->message != WM_KEYDOWN) return false;
  if (message->wParam == VK_RETURN && CanOpenSelection()) {
    ::SendMessageW(::GetAncestor(m_hWnd, GA_ROOT), WM_COMMAND, kExplorerOpen, 0);
    return true;
  }
  if (message->wParam == 'C' && (::GetKeyState(VK_CONTROL) & 0x8000)) {
    CopySelectedName(); return true;
  }
  return false;
}

}  // namespace turbodb::app
