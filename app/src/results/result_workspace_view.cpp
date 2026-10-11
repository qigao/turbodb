#include "results/result_workspace_view.h"
#include "studio_colors.h"
#include "clipboard.h"
#include <atluser.h>
#include <windowsx.h>

#include <climits>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace turbodb::app {

namespace {

constexpr UINT kMessageId = 4202u;
constexpr UINT kStatusId = 4203u;
constexpr UINT kPlanTreeId = 4204u;
constexpr UINT kPlanRawId = 4205u;
constexpr int kStatusHeight = 24;
constexpr int kHeadingHeight = 26;

}  // namespace

LRESULT ResultWorkspaceView::OnCreate(UINT, WPARAM, LPARAM, BOOL&) {
  RECT client{};
  GetClientRect(&client);

  const DWORD grid_style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                           LVS_SHOWSELALWAYS | LVS_SINGLESEL;
  if (grid_.Create(m_hWnd, client, nullptr, grid_style, 0,
                   kGridId) == nullptr) {
    return -1;
  }
  grid_.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

  const DWORD message_style = WS_CHILD | WS_TABSTOP | ES_MULTILINE | ES_READONLY |
                              ES_AUTOVSCROLL | WS_VSCROLL;
  if (message_.Create(m_hWnd, client, nullptr, message_style,
                      0, kMessageId) == nullptr) {
    return -1;
  }

  const DWORD tree_style = WS_CHILD | WS_TABSTOP | TVS_HASBUTTONS |
                           TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS;
  if (plan_tree_.Create(m_hWnd, client, nullptr, tree_style,
                        0, kPlanTreeId) == nullptr) {
    return -1;
  }
  const DWORD raw_style = WS_CHILD | WS_TABSTOP | ES_MULTILINE | ES_READONLY |
                          ES_AUTOVSCROLL | WS_VSCROLL | WS_HSCROLL;
  if (plan_raw_.Create(m_hWnd, client, nullptr, raw_style,
                       WS_EX_CLIENTEDGE, kPlanRawId) == nullptr) {
    return -1;
  }

  if (status_.Create(m_hWnd, client, L"Ready",
                     WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 0,
                     kStatusId) == nullptr) {
    return -1;
  }
  if (tabs_.Create(m_hWnd, client, L"Result pages",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TCS_FOCUSONBUTTONDOWN, 0, kTabsId) == nullptr)
    return -1;
  for (const wchar_t* label : {L"Results", L"Messages", L"Plan"}) {
    TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<wchar_t*>(label);
    if (tabs_.InsertItem(tabs_.GetItemCount(), &item) < 0) return -1;
  }
  NONCLIENTMETRICSW metrics{sizeof(metrics)};
  if (!::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) ||
      font_.CreateFontIndirect(&metrics.lfMessageFont) == nullptr)
    return -1;
  for (HWND child : {grid_.m_hWnd, message_.m_hWnd, plan_tree_.m_hWnd,
                     plan_raw_.m_hWnd, status_.m_hWnd, tabs_.m_hWnd})
    ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_.m_hFont), FALSE);

  Clear();
  Layout(client.right - client.left, client.bottom - client.top);
  return 0;
}

LRESULT ResultWorkspaceView::OnSize(UINT, WPARAM, LPARAM lparam, BOOL&) {
  Layout(LOWORD(lparam), HIWORD(lparam));
  return 0;
}

LRESULT ResultWorkspaceView::OnControlColor(UINT, WPARAM wparam, LPARAM lparam,
                                            BOOL& handled) {
  const auto colors = CurrentStudioColors();
  const HWND control = reinterpret_cast<HWND>(lparam);
  if (colors.system) {
    handled = FALSE;
    return 0;
  }
  COLORREF foreground = colors.ink, background = colors.paper;
  if (control == status_ || control == message_) {
    switch (tone_) {
      case Tone::neutral: foreground = colors.blue; background = colors.surface; break;
      case Tone::success: foreground = colors.green; background = colors.success; break;
      case Tone::busy: foreground = colors.amber; background = colors.warning; break;
      case Tone::error: foreground = colors.red; background = colors.error; break;
    }
    // Keep the workspace white; concentrate status color in the footer.
    if (control == message_) background = colors.paper;
  } else if (control != plan_raw_) {
    handled = FALSE;
    return 0;
  }
  return reinterpret_cast<LRESULT>(StudioControlColors(reinterpret_cast<HDC>(wparam),
                                                       foreground, background));
}

void ResultWorkspaceView::SetStatus(const wchar_t* text, Tone tone) {
  page_status_[selected_page_] = text;
  page_tone_[selected_page_] = tone;
  tone_ = tone;
  status_.SetWindowText(text);
  status_.Invalidate();
  message_.Invalidate();
}

LRESULT ResultWorkspaceView::OnGridCustomDraw(int, LPNMHDR header, BOOL&) {
  const auto colors = CurrentStudioColors();
  if (colors.system) return CDRF_DODEFAULT;
  auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(header);
  if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
  // The themed control can report CDIS_SELECTED during WM_PRINT for rows that
  // are not selected. The list's item state remains the selection authority.
  if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT &&
      grid_.GetItemState(static_cast<int>(draw->nmcd.dwItemSpec), LVIS_SELECTED) == 0 &&
      (draw->nmcd.uItemState & CDIS_HOT) == 0) {
    draw->clrText = colors.ink;
    draw->clrTextBk = draw->nmcd.dwItemSpec % 2 == 0 ? colors.paper : colors.surface;
  }
  return CDRF_DODEFAULT;
}

void ResultWorkspaceView::Layout(int width, int height) {
  if (width < 0) width = 0;
  if (height < 0) height = 0;
  const int top = height < kHeadingHeight ? height : kHeadingHeight;
  const int footer = height - top < kStatusHeight ? height - top : kStatusHeight;
  const int body_height = height - top - footer;
  if (tabs_.IsWindow())
    tabs_.SetWindowPos(nullptr, 0, 0, width, top, SWP_NOACTIVATE | SWP_NOZORDER);
  if (grid_.IsWindow())
    grid_.SetWindowPos(nullptr, 0, top, width, body_height,
                       SWP_NOACTIVATE | SWP_NOZORDER);
  if (message_.IsWindow())
    message_.SetWindowPos(nullptr, 0, top, width, body_height,
                          SWP_NOACTIVATE | SWP_NOZORDER);
  if (message_.IsWindow()) {
    RECT text{12, 12, width > 24 ? width - 12 : 12,
              body_height > 24 ? body_height - 12 : 12};
    message_.SetRect(&text);
  }
  const int tree_height = body_height * 3 / 5;
  if (plan_tree_.IsWindow())
    plan_tree_.SetWindowPos(nullptr, 0, top, width, tree_height,
                            SWP_NOACTIVATE | SWP_NOZORDER);
  if (plan_raw_.IsWindow())
    plan_raw_.SetWindowPos(nullptr, 0, top + tree_height, width,
                           body_height - tree_height,
                           SWP_NOACTIVATE | SWP_NOZORDER);
  if (status_.IsWindow())
    status_.SetWindowPos(nullptr, 0, top + body_height, width, footer,
                         SWP_NOACTIVATE | SWP_NOZORDER);
}

void ResultWorkspaceView::ShowGrid(bool visible) {
  SelectPage(visible ? 0 : 1);
}

void ResultWorkspaceView::Clear(bool connected) {
  grid_.DeleteAllItems();
  while (grid_.DeleteColumn(0)) {}
  plan_tree_.DeleteAllItems();
  plan_items_.clear();
  has_plan_ = false;
  selected_column_ = 0;
  page_status_ = {L"No row results yet", L"", L"No execution plan yet"};
  page_tone_.fill(Tone::neutral);
  plan_raw_.SetWindowText(L"");
  message_.SetWindowText(connected
      ? L"Write SQL in the editor, then choose Execute (F5 or Ctrl+Enter)."
      : L"Open a connection: File > New Connection (Ctrl+N).");
  ShowPlan(false);
  ShowGrid(false);
  SetStatus(connected ? L"Connected" : L"Not connected",
             connected ? Tone::success : Tone::neutral);
}

void ResultWorkspaceView::FocusContent() {
  if (selected_page_ == 2)
    plan_tree_.SetFocus();
  else if (selected_page_ == 0)
    grid_.SetFocus();
  else
    message_.SetFocus();
}

bool ResultWorkspaceView::HasPlanDetails() const {
  return has_plan_;
}

void ResultWorkspaceView::FocusPlanDetails() {
  if (HasPlanDetails()) { SelectPage(2); plan_raw_.SetFocus(); }
}

bool ResultWorkspaceView::HandleKey(MSG* message, HWND return_focus) {
  if (message->message != WM_KEYDOWN || !IsChild(message->hwnd)) return false;
  const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
  if (control && message->wParam == 'C' && message->hwnd == grid_) {
    if (!CopyGrid(shift ? kCopyCell : kCopyRow))
      MessageBox(L"Select a row and retry. Clipboard text is limited to 16 MiB.", L"Copy results", MB_OK | MB_ICONERROR);
    return true;
  }
  if (control && message->wParam == 'A' &&
      (message->hwnd == plan_raw_ || message->hwnd == message_)) {
    ::SendMessageW(message->hwnd, EM_SETSEL, 0, -1);
    return true;
  }
  if (message->wParam != VK_TAB || control) return false;
  if (message->hwnd == tabs_ && !shift) FocusContent();
  else if (message->hwnd == tabs_) ::SetFocus(return_focus);
  else if (selected_page_ == 2 && message->hwnd == plan_tree_ && !shift)
    plan_raw_.SetFocus();
  else if (selected_page_ == 2 && message->hwnd == plan_raw_ && shift)
    plan_tree_.SetFocus();
  else if (shift) tabs_.SetFocus();
  else
    ::SetFocus(return_focus);
  return true;
}

void ResultWorkspaceView::ShowPlan(bool visible) {
  if (visible) SelectPage(2);
  else if (selected_page_ == 2) SelectPage(1);
}

void ResultWorkspaceView::SelectPage(int page) {
  if (page < 0 || page > 2) return;
  selected_page_ = page;
  tabs_.SetCurSel(page);
  grid_.ShowWindow(page == 0 ? SW_SHOW : SW_HIDE);
  message_.ShowWindow(page == 1 ? SW_SHOW : SW_HIDE);
  plan_tree_.ShowWindow(page == 2 ? SW_SHOW : SW_HIDE);
  plan_raw_.ShowWindow(page == 2 ? SW_SHOW : SW_HIDE);
  tone_ = page_tone_[page];
  status_.SetWindowText(page_status_[page].c_str());
  status_.Invalidate();
  message_.Invalidate();
}

LRESULT ResultWorkspaceView::OnTabChanged(int, LPNMHDR, BOOL&) {
  SelectPage(tabs_.GetCurSel()); return 0;
}

bool ResultWorkspaceView::CopyGrid(UINT command) {
  try {
    std::wstring text;
    return GridText(command, text) && CopyDisplayText(m_hWnd, text);
  } catch (...) { return false; }
}

bool ResultWorkspaceView::GridText(UINT command, std::wstring& text) const {
  text.clear();
  if (command < kCopyCell || command > kCopyAll) return false;
  const int selected = grid_.GetNextItem(-1, LVNI_SELECTED);
  if (command != kCopyAll && selected < 0) return false;
  const int columns = grid_.GetHeader().GetItemCount();
  if (columns <= 0 || grid_.GetItemCount() == 0) return false;
  const int first_row = command == kCopyAll ? 0 : selected;
  const int last_row = command == kCopyAll ? grid_.GetItemCount() : selected + 1;
  for (int row = first_row; row < last_row; ++row) {
    if (row != first_row) text += L"\r\n";
    const int first_col = command == kCopyCell ? std::min(selected_column_, columns - 1) : 0;
    const int last_col = command == kCopyCell ? first_col + 1 : columns;
    for (int column = first_col; column < last_col; ++column) {
      if (column != first_col) text += L'\t';
      // Read the native displayed value without fixed-buffer truncation.
      std::wstring cell(128, L'\0');
      for (;;) {
        const int length = grid_.GetItemText(row, column, cell.data(), static_cast<int>(cell.size()));
        if (length < static_cast<int>(cell.size()) - 1) { cell.resize(length); break; }
        if (cell.size() >= kClipboardMaxChars) return false;
        cell.resize(std::min(kClipboardMaxChars, cell.size() * 2));
      }
      if (text.size() > kClipboardMaxChars || cell.size() > kClipboardMaxChars - text.size()) return false;
      text += cell;
    }
  }
  return text.size() <= kClipboardMaxChars;
}

LRESULT ResultWorkspaceView::OnCopyCommand(UINT, WPARAM wparam, LPARAM, BOOL& handled) {
  const UINT command = LOWORD(wparam);
  if (command < kCopyCell || command > kCopyAll) { handled = FALSE; return 0; }
  if (!CopyGrid(command))
    MessageBox(L"Could not copy results. Select a row and retry; clipboard text is limited to 16 MiB.",
               L"Copy results", MB_OK | MB_ICONERROR);
  return 0;
}

LRESULT ResultWorkspaceView::OnContextMenu(UINT, WPARAM wparam, LPARAM lparam, BOOL& handled) {
  if (reinterpret_cast<HWND>(wparam) != grid_) { handled = FALSE; return 0; }
  POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  if (point.x == -1 && point.y == -1) {
    RECT item{};
    if (!grid_.GetItemRect(grid_.GetNextItem(-1, LVNI_SELECTED), &item, LVIR_BOUNDS))
      grid_.GetClientRect(&item);
    point = {item.left, item.top}; grid_.ClientToScreen(&point);
    selected_column_ = 0;
  } else {
    LVHITTESTINFO hit{}; hit.pt = point; grid_.ScreenToClient(&hit.pt);
    if (grid_.SubItemHitTest(&hit) < 0) {
      grid_.SetItemState(-1, 0, LVIS_SELECTED);
    } else {
      grid_.SetItemState(-1, 0, LVIS_SELECTED);
      grid_.SetItemState(hit.iItem, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
      selected_column_ = std::max(0, hit.iSubItem);
    }
  }
  grid_.SetFocus();
  CMenu menu; if (!menu.CreatePopupMenu()) return 0;
  const UINT state = MF_STRING | (grid_.GetNextItem(-1, LVNI_SELECTED) >= 0 ? MF_ENABLED : MF_GRAYED);
  menu.AppendMenu(state, kCopyCell, L"Copy Cell\tCtrl+Shift+C");
  menu.AppendMenu(state, kCopyRow, L"Copy Row\tCtrl+C");
  menu.AppendMenu(MF_STRING | (grid_.GetItemCount() ? MF_ENABLED : MF_GRAYED), kCopyAll, L"Copy All Rows");
  menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON, point.x, point.y, m_hWnd);
  return 0;
}

std::wstring ResultWorkspaceView::Utf8(std::string_view text) {
  if (text.empty()) return {};
  if (text.size() > static_cast<std::size_t>(INT_MAX)) return L"<too large>";
  const int size = ::MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0);
  if (size <= 0) return L"<invalid UTF-8>";
  std::wstring output(static_cast<std::size_t>(size), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), output.data(),
                            size) != size) {
    return L"<invalid UTF-8>";
  }
  return output;
}

std::wstring ResultWorkspaceView::CellText(const QueryCell& cell) {
  switch (cell.kind) {
    case ORM_VALUE_NULL:
      return L"NULL";
    case ORM_VALUE_INT64:
      return std::to_wstring(cell.int64_value);
    case ORM_VALUE_UINT64:
      return std::to_wstring(cell.uint64_value);
    case ORM_VALUE_DOUBLE: {
      std::wostringstream stream;
      stream.imbue(std::locale::classic());
      stream << std::setprecision(std::numeric_limits<double>::max_digits10)
             << cell.double_value;
      return stream.str();
    }
    case ORM_VALUE_BOOLEAN:
      return cell.boolean_value ? L"true" : L"false";
    case ORM_VALUE_TEXT:
      return Utf8(cell.bytes);
    case ORM_VALUE_BLOB:
      return L"<BLOB " + std::to_wstring(cell.bytes.size()) + L" bytes>";
    default:
      return L"<?>";
  }
}

void ResultWorkspaceView::SetRunning() {
  ShowPlan(false);
  ShowGrid(false);
  message_.SetWindowText(
      L"Running query...\r\nThis query cannot be cancelled.");
  SetStatus(L"Running query", Tone::busy);
}

void ResultWorkspaceView::SetExplainRunning(orm_explain_mode_t mode) {
  ShowGrid(false);
  ShowPlan(false);
  const wchar_t* body =
      mode == ORM_EXPLAIN_ANALYZE
          ? L"Running ANALYZE...\r\nANALYZE may execute the SQL statement and cannot be cancelled."
          : L"Loading execution plan...";
  message_.SetWindowText(body);
  SetStatus(mode == ORM_EXPLAIN_ANALYZE
      ? L"Running ANALYZE" : L"Loading execution plan", Tone::busy);
}

void ResultWorkspaceView::ShowError(std::string message) {
  ShowPlan(false);
  ShowGrid(false);
  const std::wstring wide = Utf8(message);
  message_.SetWindowText(wide.c_str());
  SetStatus(L"Not executed", Tone::error);
}

std::wstring ResultWorkspaceView::PlanNodeText(
    const ExecutionPlanNodeSnapshot& node) {
  std::wostringstream text;
  text << Utf8(node.node_type.empty() ? std::string_view{"Plan Node"}
                                      : std::string_view{node.node_type});
  if (!node.relation.empty()) text << L" | " << Utf8(node.relation);
  if (!node.index_name.empty()) text << L" | index=" << Utf8(node.index_name);
  if ((node.flags & ORM_PLAN_NODE_HAS_ESTIMATED_ROWS) != 0u)
    text << L" | est rows=" << node.estimated_rows;
  if ((node.flags & ORM_PLAN_NODE_HAS_ACTUAL_ROWS) != 0u)
    text << L" | actual rows=" << node.actual_rows;
  if ((node.flags & ORM_PLAN_NODE_HAS_TOTAL_COST) != 0u)
    text << L" | cost=" << node.total_cost;
  if ((node.flags & ORM_PLAN_NODE_HAS_ACTUAL_TOTAL_MS) != 0u)
    text << L" | " << node.actual_total_ms << L" ms";
  return text.str();
}

void ResultWorkspaceView::RenderPlan(const ExecutionPlanSnapshot& plan) {
  if (plan.status != ORM_STATUS_OK) {
    ShowPlan(false);
    ShowGrid(false);
    std::wstring body =
        (plan.mode == ORM_EXPLAIN_ANALYZE ? L"ANALYZE failed" : L"PLAN failed");
    body += L" (TurboDB status " + std::to_wstring(plan.status) + L")";
    if (!plan.message.empty()) body += L": " + Utf8(plan.message);
    message_.SetWindowText(body.c_str());
  } else {
    has_plan_ = true;
    ShowPlan(true);
    plan_tree_.DeleteAllItems();
    plan_items_.assign(plan.nodes.size(), nullptr);
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
      const auto& node = plan.nodes[i];
      HTREEITEM parent = TVI_ROOT;
      if (node.parent_index != ORM_EXECUTION_PLAN_ROOT_INDEX &&
          node.parent_index < plan_items_.size())
        parent = plan_items_[static_cast<std::size_t>(node.parent_index)];
      const std::wstring label = PlanNodeText(node);
      TVINSERTSTRUCTW insert{};
      insert.hParent = parent;
      insert.hInsertAfter = TVI_LAST;
      insert.item.mask = TVIF_TEXT;
      insert.item.pszText = const_cast<wchar_t*>(label.c_str());
      plan_items_[i] = TreeView_InsertItem(plan_tree_.m_hWnd, &insert);
    }
    for (HTREEITEM item : plan_items_) {
      if (item != nullptr) plan_tree_.Expand(item);
    }
    const std::wstring raw = Utf8(plan.raw_detail);
    plan_raw_.SetWindowText(raw.c_str());
  }

  const double elapsed_ms =
      static_cast<double>(plan.elapsed_microseconds) / 1000.0;
  std::wostringstream status_text;
  status_text << (plan.mode == ORM_EXPLAIN_ANALYZE ? L"ANALYZE" : L"PLAN")
              << L" | provider=" << Utf8(plan.provider)
              << L" | nodes=" << plan.nodes.size()
              << L" | " << std::fixed << std::setprecision(2)
              << elapsed_ms << L" ms";
  const std::wstring status = status_text.str();
  SetStatus(status.c_str(), plan.status == ORM_STATUS_OK ? Tone::success : Tone::error);
  if (plan.status == ORM_STATUS_OK) {
    message_.SetWindowText(status.c_str());
    page_status_[1] = status; page_tone_[1] = Tone::success;
  }
}

void ResultWorkspaceView::Render(const QueryResultSnapshot& result) {
  ShowPlan(false);
  const double elapsed_ms =
      static_cast<double>(result.elapsed_microseconds) / 1000.0;
  std::wostringstream status_text;
  status_text << std::fixed << std::setprecision(2);

  if (result.kind == QueryOutcomeKind::rows) {
    ShowGrid(true);
    grid_.DeleteAllItems();
    while (grid_.DeleteColumn(0)) {}

    for (std::size_t column = 0; column < result.columns.size(); ++column) {
      std::wstring name = Utf8(result.columns[column]);
      grid_.InsertColumn(static_cast<int>(column), name.c_str(), LVCFMT_LEFT,
                         140, static_cast<int>(column));
    }
    for (std::size_t row = 0; row < result.rows.size(); ++row) {
      if (result.rows[row].empty()) continue;
      std::wstring first = CellText(result.rows[row][0]);
      const int item = grid_.InsertItem(static_cast<int>(row), first.c_str());
      for (std::size_t column = 1; column < result.rows[row].size(); ++column) {
        std::wstring value = CellText(result.rows[row][column]);
        grid_.SetItemText(item, static_cast<int>(column), value.c_str());
      }
    }
    status_text << L"Rows: " << result.rows.size() << L" | Columns: "
                << result.columns.size() << L" | " << elapsed_ms << L" ms";
  } else if (result.kind == QueryOutcomeKind::command) {
    ShowGrid(false);
    const std::wstring body =
        L"Command completed.\r\nAffected rows: " +
        std::to_wstring(result.affected_rows);
    message_.SetWindowText(body.c_str());
    status_text << L"Command complete | " << elapsed_ms << L" ms";
  } else {
    ShowGrid(false);
    std::wstring body = L"TurboDB status " + std::to_wstring(result.status);
    if (!result.message.empty()) body += L": " + Utf8(result.message);
    message_.SetWindowText(body.c_str());
    status_text << L"Failed | " << elapsed_ms << L" ms";
  }

  const std::wstring status = status_text.str();
  SetStatus(status.c_str(), result.kind == QueryOutcomeKind::error ? Tone::error : Tone::success);
  if (result.kind == QueryOutcomeKind::rows) {
    message_.SetWindowText(status.c_str());
    page_status_[1] = status; page_tone_[1] = Tone::success;
  }
}

}  // namespace turbodb::app
