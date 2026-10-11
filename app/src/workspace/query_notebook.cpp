#include "workspace/query_notebook.h"
#include "command_ids.h"
#include <algorithm>
#include <Scintilla.h>
#include <atluser.h>
#include <windowsx.h>

namespace turbodb::app {

void QueryNotebook::Bind(ScintillaRuntime* runtime, SqlWorkspaceSession* session,
                         const SqlLanguageService* language, HWND frame) {
  runtime_ = runtime; session_ = session; language_ = language; frame_ = frame;
}

LRESULT QueryNotebook::OnCreate(UINT, WPARAM, LPARAM, BOOL&) {
  RECT bounds{};
  GetClientRect(&bounds);
  if (!tabs_.Create(m_hWnd, bounds, L"Queries", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
      WS_CLIPSIBLINGS | TCS_FOCUSONBUTTONDOWN, 0, kTabsId) ||
      !add_.Create(m_hWnd, bounds, L"New", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                   0, kQueryNew) ||
      !close_.Create(m_hWnd, bounds, L"Close", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                     0, kQueryClose)) return -1;
  NONCLIENTMETRICSW metrics{sizeof(metrics)};
  if (!::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0) ||
      !font_.CreateFontIndirect(&metrics.lfMessageFont)) return -1;
  for (HWND child : {tabs_.m_hWnd, add_.m_hWnd, close_.m_hWnd})
    ::SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_.m_hFont), FALSE);
  return AddPage() ? 0 : -1;
}

bool QueryNotebook::AddPage(std::wstring title, std::string_view sql) {
  if (!CanAdd()) return false;
  try {
    auto page = std::make_unique<QueryPage>();
    page->title = title.empty() ? L"Query " + std::to_wstring(next_title_++) : std::move(title);
    RECT bounds{};
    GetClientRect(&bounds);
    constexpr DWORD style = WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    if (!page->splitter.Create(m_hWnd, bounds, nullptr, style)) return false;
    page->editor.Bind(runtime_, session_, language_, frame_);
    if (!page->editor.Create(page->splitter, bounds, nullptr, style | WS_VISIBLE | WS_TABSTOP) ||
        !page->editor.Initialize() ||
        !page->results.Create(page->splitter, bounds, nullptr, style | WS_VISIBLE)) return false;
    page->editor.SetText(sql);
    page->editor.SendMessage(SCI_EMPTYUNDOBUFFER);
    page->editor.SendMessage(SCI_SETSAVEPOINT);
    page->results.Clear(session_->connection().has_value());
    page->splitter.SetSplitterPanes(page->editor, page->results);
    page->splitter.SetSplitterPosPct(64);
    pages_.reserve(pages_.size() + 1);
    TCITEMW item{};
    item.mask = TCIF_TEXT;
    item.pszText = page->title.data();
    const int index = static_cast<int>(pages_.size());
    if (tabs_.InsertItem(index, &item) < 0) return false;
    pages_.push_back(std::move(page));
    Select(index, true);
    UpdateTitle(active_);
    return true;
  } catch (...) {
    return false;
  }
}

bool QueryNotebook::ConfirmDiscard(const QueryPage& page) {
  if (!::SendMessageW(page.editor, SCI_GETLENGTH, 0, 0) &&
      !::SendMessageW(page.editor, SCI_GETMODIFY, 0, 0)) return true;
  const std::wstring question = L"Close " + page.title + L" and discard its SQL?\n\nQueries are not saved between sessions.";
  return ::MessageBoxW(frame_, question.c_str(), L"Discard query", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
}

bool QueryNotebook::ConfirmDiscardAll() {
  for (const auto& page : pages_) if (!ConfirmDiscard(*page)) return false;
  return true;
}

bool QueryNotebook::CloseActive() {
  if (!ConfirmDiscard(Active())) return false;
  const auto removed = active_;
  if (pages_.size() == 1 && !AddPage()) return false;
  tabs_.DeleteItem(static_cast<int>(removed));
  // Destroy HWNDs before releasing the WTL objects backing their window procedures.
  pages_[removed]->splitter.DestroyWindow();
  pages_.erase(pages_.begin() + removed);
  active_ = std::min(removed, pages_.size() - 1);
  Select(static_cast<int>(active_), true);
  return true;
}

void QueryNotebook::Select(int index, bool focus) {
  if (index < 0 || static_cast<std::size_t>(index) >= pages_.size()) return;
  for (const auto& page : pages_) {
    page->editor.SendMessage(SCI_CALLTIPCANCEL);
    page->editor.SendMessage(SCI_AUTOCCANCEL);
    page->splitter.ShowWindow(SW_HIDE);
  }
  active_ = static_cast<std::size_t>(index);
  tabs_.SetCurSel(index);
  Layout();
  Active().splitter.ShowWindow(SW_SHOW);
  if (focus) FocusContent();
}

void QueryNotebook::FocusContent() {
  if (EditorVisible()) Active().editor.SetFocus();
  else Active().results.FocusContent();
}

void QueryNotebook::ShowEditor() {
  if (!EditorVisible()) Active().splitter.SetSinglePaneMode(SPLIT_PANE_NONE);
  Active().editor.SetFocus();
}

void QueryNotebook::ShowResults(bool focus) {
  if (!ResultsVisible()) Active().splitter.SetSinglePaneMode(SPLIT_PANE_NONE);
  if (focus) Active().results.FocusContent();
}

void QueryNotebook::ToggleEditor() {
  // Native splitter mode owns visibility and retains the user's divider position.
  // Hiding the last query pane reveals its sibling instead of leaving a blank page.
  if (EditorVisible()) {
    Active().splitter.SetSinglePaneMode(SPLIT_PANE_BOTTOM);
    Active().results.FocusContent();
  } else ShowEditor();
}

void QueryNotebook::ToggleResults() {
  if (ResultsVisible()) {
    Active().splitter.SetSinglePaneMode(SPLIT_PANE_TOP);
    Active().editor.SetFocus();
  } else ShowResults();
}

void QueryNotebook::Cycle(bool previous) {
  Select(static_cast<int>((active_ + pages_.size() + (previous ? -1 : 1)) % pages_.size()), true);
}

bool QueryNotebook::ActivateEditor(HWND editor) {
  for (std::size_t i = 0; i < pages_.size(); ++i) {
    if (pages_[i]->editor == editor) {
      Select(static_cast<int>(i), true);
      return true;
    }
  }
  return false;
}

void QueryNotebook::UpdateTitle(std::size_t index) {
  auto title = pages_[index]->title;
  if (::SendMessageW(pages_[index]->editor, SCI_GETMODIFY, 0, 0)) title += L" *";
  TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = title.data();
  tabs_.SetItem(static_cast<int>(index), &item);
}

void QueryNotebook::Layout() {
  RECT bounds{}; GetClientRect(&bounds);
  const int height = std::min(30, static_cast<int>(bounds.bottom));
  const int button = std::min(56, static_cast<int>(bounds.right) / 4);
  tabs_.SetWindowPos(nullptr, 0, 0, std::max(0L, bounds.right - button * 2), height, SWP_NOZORDER);
  add_.SetWindowPos(nullptr, bounds.right - button * 2, 0, button, height, SWP_NOZORDER);
  close_.SetWindowPos(nullptr, bounds.right - button, 0, button, height, SWP_NOZORDER);
  if (!pages_.empty()) Active().splitter.SetWindowPos(nullptr, 0, height, bounds.right,
      std::max(0L, bounds.bottom - height), SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT QueryNotebook::OnSize(UINT, WPARAM, LPARAM, BOOL&) { Layout(); return 0; }

LRESULT QueryNotebook::OnNotify(UINT, WPARAM wparam, LPARAM lparam, BOOL&) {
  auto* header = reinterpret_cast<NMHDR*>(lparam);
  if (header->hwndFrom == tabs_ && header->code == TCN_SELCHANGE) {
    Select(tabs_.GetCurSel(), false);
    return 0;
  }
  for (std::size_t i = 0; i < pages_.size(); ++i) {
    if (header->hwndFrom != pages_[i]->editor) continue;
    pages_[i]->editor.HandleNotification(header);
    if (header->code == SCN_SAVEPOINTLEFT || header->code == SCN_SAVEPOINTREACHED)
      UpdateTitle(i);
    return 0;
  }
  return ::SendMessageW(frame_, WM_NOTIFY, wparam, lparam);
}

LRESULT QueryNotebook::OnCommand(UINT, WPARAM wparam, LPARAM, BOOL&) {
  ::PostMessageW(frame_, WM_COMMAND, LOWORD(wparam), 0);
  return 0;
}

LRESULT QueryNotebook::OnContextMenu(UINT, WPARAM wparam, LPARAM lparam, BOOL& handled) {
  if (reinterpret_cast<HWND>(wparam) != tabs_) { handled = FALSE; return 0; }
  POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  if (point.x == -1 && point.y == -1) {
    RECT item{}; tabs_.GetItemRect(static_cast<int>(active_), &item);
    point = {item.left, item.bottom}; tabs_.ClientToScreen(&point);
  } else {
    TCHITTESTINFO hit{}; hit.pt = point; tabs_.ScreenToClient(&hit.pt);
    const int index = tabs_.HitTest(&hit);
    if (index >= 0) Select(index, false);
  }
  CMenu popup; if (!popup.CreatePopupMenu()) return 0;
  popup.AppendMenu(MF_STRING, kQueryNew, L"New Query\tCtrl+T");
  popup.AppendMenu(MF_STRING, kQueryClose, L"Close Query\tCtrl+W");
  popup.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON, point.x, point.y, frame_);
  return 0;
}

bool QueryNotebook::HandleKey(MSG* message) {
  if (message->message != WM_KEYDOWN) return false;
  if (Active().results.HandleKey(message, EditorVisible() ? Active().editor.m_hWnd : tabs_.m_hWnd)) return true;
  if (message->wParam == VK_RETURN && (message->hwnd == add_ || message->hwnd == close_)) {
    if (::IsWindowEnabled(message->hwnd))
      ::PostMessageW(frame_, WM_COMMAND, message->hwnd == add_ ? kQueryNew : kQueryClose, 0);
    return true;
  }
  const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
  if (message->wParam == VK_TAB && (::GetKeyState(VK_CONTROL) & 0x8000) == 0) {
    if (message->hwnd == tabs_) {
      if (shift && close_.IsWindowEnabled()) close_.SetFocus();
      else if (shift && add_.IsWindowEnabled()) add_.SetFocus();
      else FocusContent();
    }
    else if (message->hwnd == add_)
      ::SetFocus(!shift && close_.IsWindowEnabled() ? close_.m_hWnd : tabs_.m_hWnd);
    else if (message->hwnd == close_)
      ::SetFocus(shift && add_.IsWindowEnabled() ? add_.m_hWnd : tabs_.m_hWnd);
    else return false;
    return true;
  }
  return false;
}

bool QueryNotebook::RefreshLanguage(bool clear_results) {
  bool success = true;
  for (auto& page : pages_) {
    if (!page->editor.RefreshLanguage()) success = false;
    if (clear_results) page->results.Clear(session_->connection().has_value());
  }
  return success;
}

void QueryNotebook::RefreshColors() {
  for (auto& page : pages_) page->editor.RefreshColors();
}

void QueryNotebook::UpdateActions(bool available, bool can_close) {
  if (add_.IsWindow()) add_.EnableWindow(available && CanAdd());
  if (close_.IsWindow()) close_.EnableWindow(available && can_close);
  const HWND focus = ::GetFocus();
  if ((focus == add_ || focus == close_) && !::IsWindowEnabled(focus)) FocusTabs();
}

}  // namespace turbodb::app
