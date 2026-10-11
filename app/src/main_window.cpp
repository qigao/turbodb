#include "main_window.h"

#include <algorithm>
#include <cctype>
#include <chrono>

#include <Scintilla.h>

#include "connection/connection_dialog.h"

namespace turbodb::app {

namespace {

constexpr DWORD kSplitterStyle =
    WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
constexpr std::size_t kRecentConnectionLabelCharacters = 80;

struct MenuItem {
  UINT id;
  const wchar_t* label;
};

template <std::size_t N>
bool AppendPopup(CMenu& menu, const wchar_t* label,
                 const MenuItem (&items)[N]) {
  CMenu popup;
  if (!popup.CreatePopupMenu()) return false;
  for (const auto& item : items) {
    if (!popup.AppendMenu(item.id == 0 ? MF_SEPARATOR : MF_STRING,
                          item.id, item.label))
      return false;
  }
  if (!menu.AppendMenu(MF_POPUP, reinterpret_cast<UINT_PTR>(popup.m_hMenu),
                        label))
    return false;
  // The parent menu now owns the submenu, including failure cleanup.
  popup.Detach();
  return true;
}

}  // namespace

bool MainWindow::LoadConnectionConfiguration(const std::wstring& path,
                                              std::wstring* error) {
  if (!LoadConnectionConfig(path, &connection_config_, error)) return false;
  connection_config_initialized_ = true;
  ResetRecentConnections();
  return true;
}

bool MainWindow::InitializeConnectionConfiguration(std::wstring* error) {
  error->clear();
  if (connection_config_initialized_) return true;
  if (default_config_path_.empty()) {
    connection_config_initialized_ = true;
    return true;
  }
  if (!LoadConnectionConfig(default_config_path_, &connection_config_, error, true)) return false;
  connection_config_initialized_ = true;
  ResetRecentConnections();
  return true;
}

void MainWindow::ResetRecentConnections() {
  recent_connections_.clear();
  if (connection_config_.profiles.empty()) return;
  recent_connections_.push_back(connection_config_.default_profile);
  for (std::size_t index = 0; index < connection_config_.profiles.size(); ++index)
    if (index != connection_config_.default_profile) recent_connections_.push_back(index);
}

void MainWindow::RememberConnection(std::size_t profile) {
  const auto found = std::find(recent_connections_.begin(), recent_connections_.end(), profile);
  if (found != recent_connections_.end())
    std::rotate(recent_connections_.begin(), found, found + 1);
}

void MainWindow::UpdateRecentConnectionsMenu() {
  CMenuHandle menu(recent_connections_menu_);
  std::wstring error;
  const bool loaded = InitializeConnectionConfiguration(&error);
  while (menu.GetMenuItemCount() > 0) menu.DeleteMenu(0, MF_BYPOSITION);
  if (!loaded || recent_connections_.empty()) {
    menu.AppendMenu(MF_STRING | MF_GRAYED, UINT_PTR{0}, loaded
        ? L"No configured connections - use New Connection..."
        : L"INI could not be loaded - use New Connection...");
    return;
  }
  const UINT flags = MF_STRING | (IsCommandEnabled(kFileConnect) ? MF_ENABLED : MF_GRAYED);
  for (const auto index : recent_connections_) {
    // Labels cannot inject menu mnemonics or shortcut columns. Configuration
    // remains the sole owner of credentials; the menu only holds profile IDs.
    const auto& preset = connection_config_.profiles[index];
    std::wstring label;
    const auto& name = preset.display_name.empty() ? preset.name : preset.display_name;
    auto count = (std::min)(name.size(), kRecentConnectionLabelCharacters);
    // Do not split a supplementary Unicode character at the display limit.
    if (count < name.size() && count > 0 && IS_HIGH_SURROGATE(name[count - 1])) --count;
    for (std::size_t i = 0; i < count; ++i) {
      const wchar_t character = name[i];
      if (character == L'&') label += L'&';
      label += character < L' ' ? L' ' : character;
    }
    if (count < name.size()) label += L"...";
    menu.AppendMenu(flags, kRecentConnectionFirst + static_cast<UINT>(index), label.c_str());
  }
}

LRESULT MainWindow::OnRecentConnection(WORD, WORD id, HWND, BOOL&) {
  if (!IsCommandEnabled(kFileConnect)) return 0;
  const auto index = static_cast<std::size_t>(id - kRecentConnectionFirst);
  std::wstring error;
  if (!InitializeConnectionConfiguration(&error) || index >= connection_config_.profiles.size())
    return 0;
  ConnectionDialog dialog(*this, static_cast<int>(index));
  if (dialog.DoModal(m_hWnd) == -1)
    Results().ShowError("Could not create the connection dialog.");
  notebook_.ShowEditor();
  UpdateToolbarState();
  return 0;
}

bool MainWindow::CreateMenus() {
  const MenuItem file[] = {
      {kFileConnect, L"&New Connection...\tCtrl+N"},
      {kFileDisconnect, L"&Disconnect\tCtrl+Shift+D"},
      {kExplorerRefresh, L"&Refresh Schema\tCtrl+R"},
      {0, nullptr},
      {kQueryNew, L"New &Query\tCtrl+T"},
      {kQueryClose, L"&Close Query\tCtrl+W"},
      {0, nullptr},
      {kFileExit, L"E&xit\tAlt+F4"},
  };
  const MenuItem edit[] = {
      {kEditUndo, L"&Undo\tCtrl+Z"},
      {kEditRedo, L"&Redo\tCtrl+Y"},
      {0, nullptr},
      {kEditCut, L"Cu&t\tCtrl+X"},
      {kEditCopy, L"&Copy\tCtrl+C"},
      {kEditPaste, L"&Paste\tCtrl+V"},
      {0, nullptr},
      {kEditSelectAll, L"Select &All\tCtrl+A"},
  };
  const MenuItem query[] = {
      {kQueryExecute, L"&Execute SQL\tF5 / Ctrl+Enter"},
      {kQueryPlan, L"Execution &Plan\tF6"},
      {kQueryAnalyze, L"&Analyze SQL...\tShift+F6"},
  };
  const MenuItem view[] = {
      {kViewExplorer, L"&Schema Explorer"},
      {kViewEditor, L"SQL &Editor"},
      {kViewResults, L"&Results"},
      {kViewPlanDetails, L"Plan &Details\tCtrl+Shift+P"},
      {kViewToolbar, L"&Toolbar\tCtrl+Shift+T"},
      {kQueryNext, L"&Next Query\tCtrl+Tab"},
      {kQueryPrevious, L"Previous &Query\tCtrl+Shift+Tab"},
      {kViewQueryTabs, L"Query Ta&bs\tCtrl+Shift+Q"},
  };
  const MenuItem help[] = {{kHelpAbout, L"&About TurboDB Studio..."}};
  CMenu menu;
  if (!menu.CreateMenu() || !AppendPopup(menu, L"&File", file) ||
      !AppendPopup(menu, L"&Edit", edit) ||
      !AppendPopup(menu, L"&Query", query) ||
      !AppendPopup(menu, L"&View", view) ||
      !AppendPopup(menu, L"&Help", help))
    return false;

  CMenu recent;
  if (!recent.CreatePopupMenu() ||
      !recent.AppendMenu(MF_STRING | MF_GRAYED, UINT_PTR{0}, L"No configured connections") ||
      !::InsertMenuW(menu.GetSubMenu(0), 1, MF_BYPOSITION | MF_POPUP,
                    reinterpret_cast<UINT_PTR>(recent.m_hMenu), L"Recent C&onnections"))
    return false;
  recent_connections_menu_ = recent.Detach();

  ACCEL keys[] = {
      {FVIRTKEY | FCONTROL, 'N', kFileConnect},
      {FVIRTKEY | FCONTROL | FSHIFT, 'D', kFileDisconnect},
      {FVIRTKEY, VK_F5, kQueryExecute},
      {FVIRTKEY | FCONTROL, VK_RETURN, kQueryExecute},
      {FVIRTKEY, VK_F6, kQueryPlan},
      {FVIRTKEY | FSHIFT, VK_F6, kQueryAnalyze},
      {FVIRTKEY | FCONTROL | FSHIFT, 'T', kViewToolbar},
      {FVIRTKEY | FCONTROL | FSHIFT, 'P', kViewPlanDetails},
      {FVIRTKEY | FCONTROL, 'T', kQueryNew},
      {FVIRTKEY | FCONTROL, 'W', kQueryClose},
      {FVIRTKEY | FCONTROL, 'R', kExplorerRefresh},
      {FVIRTKEY | FCONTROL, VK_TAB, kQueryNext},
      {FVIRTKEY | FCONTROL | FSHIFT, VK_TAB, kQueryPrevious},
      {FVIRTKEY | FCONTROL | FSHIFT, 'Q', kViewQueryTabs},
  };
  if (accelerators_.CreateAcceleratorTable(keys, _countof(keys)) == nullptr ||
      !SetMenu(menu))
    return false;
  // Windows destroys the attached menu tree with the frame.
  menu.Detach();
  return true;
}

BOOL MainWindow::PreTranslateMessage(MSG* message) {
  if (!IsWindow() || accelerators_.IsNull() ||
      (message->hwnd != m_hWnd && !IsChild(message->hwnd)))
    return FALSE;
  if (notebook_.HandleKey(message)) return TRUE;
  if (explorer_.HandleKey(message)) return TRUE;
  if (message->message == WM_KEYDOWN && explorer_.IsChild(message->hwnd) &&
      message->wParam == VK_TAB && (::GetKeyState(VK_CONTROL) & 0x8000) == 0) {
    notebook_.FocusContent();
    return TRUE;
  }
  if (message->message == WM_KEYDOWN && message->hwnd == toolbar_) {
    if (message->wParam == VK_TAB || message->wParam == VK_ESCAPE) {
      notebook_.FocusContent();
      return TRUE;
    }
    if (message->wParam == VK_RETURN) {
      TBBUTTON button{};
      const int index = toolbar_.GetHotItem();
      if (index >= 0 && toolbar_.GetButton(index, &button) &&
          IsCommandEnabled(button.idCommand))
        SendMessage(WM_COMMAND, button.idCommand,
                    reinterpret_cast<LPARAM>(toolbar_.m_hWnd));
      return TRUE;
    }
  }
  return accelerators_.TranslateAccelerator(m_hWnd, message);
}

BOOL MainWindow::OnIdle() {
  UpdateToolbarState();
  return FALSE;
}

LRESULT MainWindow::OnToolbarCustomDraw(int, LPNMHDR header, BOOL&) {
  return toolbar_.CustomDraw(*reinterpret_cast<NMTBCUSTOMDRAW*>(header));
}

LRESULT MainWindow::OnToolbarInfoTip(int, LPNMHDR header, BOOL&) {
  auto& tip = *reinterpret_cast<NMTBGETINFOTIPW*>(header);
  if (tip.pszText != nullptr && tip.cchTextMax > 0)
    ::lstrcpynW(tip.pszText, CommandToolbar::CommandTip(tip.iItem), tip.cchTextMax);
  return 0;
}

LRESULT MainWindow::OnColorsChanged(UINT message, WPARAM, LPARAM, BOOL& handled) {
  if (message == WM_SYSCOLORCHANGE) {
    // Windows broadcasts this only to top-level windows; common controls need
    // the notification forwarded before the app reapplies its own palette.
    ::EnumChildWindows(m_hWnd, [](HWND child, LPARAM) -> BOOL {
      ::SendMessageW(child, WM_SYSCOLORCHANGE, 0, 0);
      return TRUE;
    }, 0);
  }
  if (toolbar_.IsWindow() && !toolbar_.RefreshColors()) {
    MessageBox(L"Unable to refresh toolbar colors. Close and reopen Studio to retry.",
               L"TurboDB Studio", MB_OK | MB_ICONERROR);
  }
  notebook_.RefreshColors();
  if (explorer_.IsWindow() && !explorer_.RefreshColors()) {
    MessageBox(L"Unable to refresh schema icons. Close and reopen Studio to retry.",
               L"TurboDB Studio", MB_OK | MB_ICONERROR);
  }
  RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
  handled = FALSE;  // Preserve the native controls' system-theme processing.
  return 0;
}

bool MainWindow::IsCommandEnabled(UINT command) const {
  if (ConnectionOpening()) return false;
  switch (command) {
    case kQueryNew: return notebook_.CanAdd();
    case kQueryClose: return request_page_ == nullptr;
    case kExplorerRefresh:
      return workspace_session_.connection().has_value() && request_page_ == nullptr;
    case kExplorerSelect: return explorer_.CanOpenSelection() && notebook_.CanAdd();
    case kExplorerCopyName: return explorer_.HasSelection();
    case kExplorerOpen:
      return explorer_.CanOpenSelection() && notebook_.CanAdd() && request_page_ == nullptr;
    case kViewPlanDetails: return Results().HasPlanDetails();
    case kFileConnect:
    case kFileDisconnect:
      return !query_controller_.busy() &&
             !query_controller_.has_pending_completion() &&
             !explain_controller_.busy() &&
             !explain_controller_.has_pending_completion() &&
             (command == kFileDisconnect
                  ? workspace_session_.connection().has_value()
                  : !workspace_session_.connection().has_value());
    case kEditUndo: return ::SendMessageW(Editor(), SCI_CANUNDO, 0, 0) != 0;
    case kEditRedo: return ::SendMessageW(Editor(), SCI_CANREDO, 0, 0) != 0;
    case kEditCut:
    case kEditCopy: return ::SendMessageW(Editor(), SCI_GETSELECTIONEMPTY, 0, 0) == 0;
    case kEditPaste: return ::SendMessageW(Editor(), SCI_CANPASTE, 0, 0) != 0;
    case kQueryExecute:
    case kQueryPlan:
    case kQueryAnalyze:
      return !query_controller_.busy() &&
             !query_controller_.has_pending_completion() &&
             !explain_controller_.busy() &&
             !explain_controller_.has_pending_completion();
    default: return true;
  }
}

bool MainWindow::IsPaneVisible(UINT command) const {
  switch (command) {
    case kViewExplorer: return workspace_splitter_.GetSinglePaneMode() != SPLIT_PANE_RIGHT;
    case kViewEditor: return notebook_.EditorVisible();
    case kViewResults: return notebook_.ResultsVisible();
    default: return false;
  }
}

void MainWindow::UpdateToolbarState() {
  notebook_.UpdateActions(!ConnectionOpening(), request_page_ == nullptr);
  if (!toolbar_.IsWindow()) return;
  for (int index = 0; index < toolbar_.GetButtonCount(); ++index) {
    TBBUTTON button{};
    if (toolbar_.GetButton(index, &button) && (button.fsStyle & BTNS_SEP) == 0) {
      toolbar_.EnableButton(button.idCommand, IsCommandEnabled(button.idCommand));
      if (button.idCommand >= kViewExplorer && button.idCommand <= kViewResults)
        toolbar_.CheckButton(button.idCommand, IsPaneVisible(button.idCommand));
    }
  }
}

LRESULT MainWindow::OnInitMenuPopup(UINT, WPARAM wparam, LPARAM lparam,
                                   BOOL& handled) {
  if (HIWORD(lparam) != 0) {
    handled = FALSE;
    return 0;
  }
  CMenuHandle popup(reinterpret_cast<HMENU>(wparam));
  if (popup.m_hMenu == recent_connections_menu_ ||
      popup.m_hMenu == ::GetSubMenu(GetMenu(), 0))
    UpdateRecentConnectionsMenu();
  for (UINT command = kFileExit; command <= kViewQueryTabs; ++command)
    popup.EnableMenuItem(command, MF_BYCOMMAND |
        (IsCommandEnabled(command) ? MF_ENABLED : MF_GRAYED));
  for (UINT command : {kViewExplorer, kViewEditor, kViewResults})
    popup.CheckMenuItem(command, MF_BYCOMMAND | (IsPaneVisible(command) ? MF_CHECKED : MF_UNCHECKED));
  UpdateToolbarState();
  return 0;
}

LRESULT MainWindow::OnMenuCommand(WORD, WORD id, HWND, BOOL& handled) {
  if (!IsCommandEnabled(id)) return 0;
  switch (id) {
    case kQueryNew:
      if (!notebook_.AddPage()) Results().ShowError("Could not create a query page.");
      UpdateToolbarState();
      return 0;
    case kQueryClose: notebook_.CloseActive(); UpdateToolbarState(); return 0;
    case kQueryNext: notebook_.Cycle(false); UpdateToolbarState(); return 0;
    case kQueryPrevious: notebook_.Cycle(true); UpdateToolbarState(); return 0;
    case kExplorerOpen: OpenSelectedRelation(true); return 0;
    case kExplorerSelect: OpenSelectedRelation(false); return 0;
    case kExplorerCopyName: explorer_.CopySelectedName(); return 0;
    case kExplorerRefresh: RefreshSchema(); return 0;
    case kViewQueryTabs: notebook_.FocusTabs(); return 0;
    case kFileConnect: {
      ConnectionDialog dialog(*this);
      if (dialog.DoModal(m_hWnd) == -1)
        Results().ShowError("Could not create the connection dialog.");
      notebook_.ShowEditor();
      UpdateToolbarState();
      return 0;
    }
    case kFileDisconnect: {
      std::string error;
      if (!CloseActiveConnection(&error)) Results().ShowError(error);
      notebook_.ShowEditor();
      UpdateToolbarState();
      return 0;
    }
    case kFileExit:
      SendMessage(WM_CLOSE);
      return 0;
    case kEditUndo: Editor().Undo(); break;
    case kEditRedo: Editor().Redo(); break;
    case kEditCut: Editor().SendMessage(SCI_CUT); break;
    case kEditCopy: Editor().SendMessage(SCI_COPY); break;
    case kEditPaste: Editor().SendMessage(SCI_PASTE); break;
    case kEditSelectAll: Editor().SendMessage(SCI_SELECTALL); break;
    case kQueryExecute:
      return OnSqlExecuteRequested(kSqlEditorExecuteRequested, 0, 0, handled);
    case kQueryPlan:
    case kQueryAnalyze:
      return OnSqlExplainRequested(
          kSqlEditorExplainRequested,
          static_cast<WPARAM>(id == kQueryPlan ? SqlExplainAction::plan
                                             : SqlExplainAction::analyze),
          0, handled);
    case kViewExplorer:
      if (IsPaneVisible(kViewExplorer)) {
        workspace_splitter_.SetSinglePaneMode(SPLIT_PANE_RIGHT);
        notebook_.FocusContent();
      } else {
        workspace_splitter_.SetSinglePaneMode(SPLIT_PANE_NONE);
        explorer_.FocusContent();
      }
      UpdateToolbarState();
      return 0;
    case kViewEditor:
      notebook_.ToggleEditor();
      UpdateToolbarState();
      return 0;
    case kViewResults:
      notebook_.ToggleResults();
      UpdateToolbarState();
      return 0;
    case kViewPlanDetails:
      notebook_.ShowResults(false);
      Results().FocusPlanDetails();
      UpdateToolbarState();
      return 0;
    case kViewToolbar:
      toolbar_.SetFocus();
      toolbar_.SetHotItem(0);
      return 0;
    case kHelpAbout:
      ::MessageBoxW(m_hWnd, L"TurboDB Studio\n\nSQL workspace for TurboDB.",
                    L"About TurboDB Studio", MB_OK | MB_ICONINFORMATION);
      return 0;
    default:
      handled = FALSE;
      return 0;
  }
  notebook_.ShowEditor();
  UpdateToolbarState();
  return 0;
}

LRESULT MainWindow::OnCreate(UINT, WPARAM, LPARAM, BOOL &) {
  if (!CreateMenus()) return -1;
  RECT client{};
  GetClientRect(&client);

  if (workspace_splitter_.Create(m_hWnd, client, nullptr, kSplitterStyle) ==
      nullptr) {
    return -1;
  }

  explorer_.Bind(&explorer_model_, &workspace_session_);
  if (explorer_.Create(workspace_splitter_, client, nullptr,
                       kSplitterStyle) == nullptr) {
    return -1;
  }

  notebook_.Bind(&editor_runtime_, &workspace_session_, &language_service_, m_hWnd);
  if (!notebook_.Create(workspace_splitter_, client, nullptr, kSplitterStyle)) return -1;
  workspace_splitter_.SetSplitterPanes(explorer_, notebook_);
  workspace_splitter_.SetSplitterPosPct(24);

  if (!toolbar_.Create(m_hWnd)) return -1;
  UpdateToolbarState();
  LayoutWorkspace(client.right - client.left, client.bottom - client.top);
  return 0;
}

bool MainWindow::OpenConnection(const ConnectionProfile& profile,
                                std::string* error) {
  if (!IsCommandEnabled(kFileConnect)) {
    if (error != nullptr)
      *error = "disconnect the active connection and finish pending work before opening another";
    return false;
  }
  return PublishConnection(PrepareConnection(connections_, profile), error);
}

MainWindow::PreparedConnection MainWindow::PrepareConnection(
    ConnectionManager& connections, const ConnectionProfile& profile) {
  PreparedConnection prepared;
  WorkspaceConnectionIdentity identity;
  try {
    if (!connections.Open(profile, &identity, &prepared.error)) return prepared;
    SchemaExplorerController explorer(prepared.model, prepared.session);
    orm_connection_t* connection = connections.Get(identity.id);
    if (connection != nullptr && explorer.Refresh(connection, identity, &prepared.error)) {
      prepared.opened = true;
      return prepared;
    }
  } catch (...) {
    prepared.error = "Could not prepare the connection. Check available memory and try again.";
  }
  if (identity.id != 0) {
    std::string close_error;
    if (!connections.Close(identity.id, &close_error)) prepared.error += "; " + close_error;
  }
  return prepared;
}

bool MainWindow::BeginOpenConnection(ConnectionProfile profile, std::string* error) {
  if (!IsCommandEnabled(kFileConnect)) {
    *error = "Finish pending work and disconnect before opening another connection.";
    return false;
  }
  try {
    pending_connection_ = std::async(std::launch::async,
        [this, profile = std::move(profile)] {
          return PrepareConnection(connections_, profile);
        });
  } catch (...) {
    *error = "Could not start the connection. Check available resources and try again.";
    return false;
  }
  UpdateToolbarState();
  return true;
}

bool MainWindow::ConnectionOpenReady() const {
  return pending_connection_.valid() &&
      pending_connection_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
}

bool MainWindow::CompleteOpenConnection(std::string* error) {
  if (!ConnectionOpenReady()) {
    *error = "The connection is still opening.";
    return false;
  }
  try {
    return PublishConnection(pending_connection_.get(), error);
  } catch (...) {
    *error = "Could not prepare the connection. Check available memory and try again.";
    UpdateToolbarState();
    return false;
  }
}

bool MainWindow::PublishConnection(PreparedConnection prepared, std::string* error) {
  if (!prepared.opened) {
    if (error != nullptr) *error = std::move(prepared.error);
    UpdateToolbarState();
    return false;
  }
  const auto id = prepared.session.connection()->id;
  explorer_model_ = std::move(prepared.model);
  workspace_session_ = std::move(prepared.session);
  explorer_.Refresh();
  if (notebook_.RefreshLanguage(true)) {
    UpdateToolbarState();
    return true;
  }

  std::string ignored;
  (void)connections_.Close(id, &ignored);
  workspace_session_.ClearConnection();
  explorer_model_.Clear();
  explorer_.Refresh();
  if (error != nullptr && error->empty())
    *error = "initialize SQL language service for connection";
  return false;
}

bool MainWindow::CloseActiveConnection(std::string* error) {
  if (ConnectionOpening() || query_controller_.busy() || query_controller_.has_pending_completion() ||
      explain_controller_.busy() ||
      explain_controller_.has_pending_completion()) {
    if (error != nullptr)
      *error = "cannot close connection while SQL/EXPLAIN work is active or pending UI consumption";
    return false;
  }
  const auto& identity = workspace_session_.connection();
  if (!identity.has_value()) return true;
  const std::uint64_t id = identity->id;
  if (!connections_.Close(id, error))
    return false;
  workspace_session_.ClearConnection();
  explorer_model_.Clear();
  explorer_.Refresh();
  UpdateToolbarState();
  return notebook_.RefreshLanguage(true);
}

LRESULT MainWindow::OnSize(UINT, WPARAM, LPARAM lparam, BOOL &) {
  LayoutWorkspace(LOWORD(lparam), HIWORD(lparam));
  return 0;
}

void MainWindow::LayoutWorkspace(int width, int height) {
  int toolbar_height = 0;
  if (toolbar_.IsWindow()) {
    toolbar_.AutoSize();
    RECT bounds{};
    toolbar_.GetWindowRect(&bounds);
    toolbar_height = bounds.bottom - bounds.top;
  }
  if (workspace_splitter_.IsWindow()) {
    workspace_splitter_.SetWindowPos(
        nullptr, 0, toolbar_height, width, std::max(0, height - toolbar_height),
        SWP_NOACTIVATE | SWP_NOZORDER);
  }
}

LRESULT MainWindow::OnExplorerSelectionChanged(UINT, WPARAM, LPARAM, BOOL&) {
  (void)notebook_.RefreshLanguage(false);
  return 0;
}

LRESULT MainWindow::OnSqlExecuteRequested(UINT, WPARAM, LPARAM source, BOOL&) {
  if (ConnectionOpening()) return 0;
  if (source && !notebook_.ActivateEditor(reinterpret_cast<HWND>(source))) return 0;
  notebook_.ShowResults(false);
  UpdateToolbarState();
  if (explain_controller_.busy() ||
      explain_controller_.has_pending_completion()) {
    Results().ShowError(
        "Wait for the active PLAN/ANALYZE result to be consumed.");
    return 0;
  }
  const auto& identity = workspace_session_.connection();
  if (!identity.has_value()) {
    Results().ShowError("Open a TurboDB connection before executing SQL.");
    return 0;
  }

  std::string sql = Editor().SelectedText();
  if (sql.empty()) sql = Editor().Text();
  const bool has_content = std::any_of(
      sql.begin(), sql.end(),
      [](unsigned char ch) { return std::isspace(ch) == 0; });
  if (!has_content) {
    Results().ShowError("SQL text is empty.");
    return 0;
  }

  std::string error;
  std::uint64_t request_id = 0u;
  if (!query_controller_.Execute(identity->id, std::move(sql), m_hWnd,
                                 &request_id, &error)) {
    Results().ShowError(error);
    return 0;
  }

  active_request_id_ = request_id;
  request_page_ = &notebook_.Active();
  workspace_session_.SetExecutionState(WorkspaceExecutionState::running);
  Results().SetRunning();
  UpdateToolbarState();
  return 0;
}

LRESULT MainWindow::OnSqlExplainRequested(UINT, WPARAM wparam, LPARAM source,
                                              BOOL&) {
  if (ConnectionOpening()) return 0;
  if (source && !notebook_.ActivateEditor(reinterpret_cast<HWND>(source))) return 0;
  notebook_.ShowResults(false);
  UpdateToolbarState();
  if (query_controller_.busy() ||
      query_controller_.has_pending_completion()) {
    Results().ShowError(
        "Wait for the active SQL result to be consumed.");
    return 0;
  }

  const auto action = static_cast<SqlExplainAction>(wparam);
  orm_explain_mode_t mode = ORM_EXPLAIN_PLAN;
  if (action == SqlExplainAction::plan) {
    mode = ORM_EXPLAIN_PLAN;
  } else if (action == SqlExplainAction::analyze) {
    mode = ORM_EXPLAIN_ANALYZE;
    const int choice = ::MessageBoxW(
        m_hWnd,
        L"ANALYZE may execute the SQL statement and observe real runtime work.\n\nContinue?",
        L"TurboDB Studio - Confirm ANALYZE",
        MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2);
    if (choice != IDOK) return 0;
  } else {
    Results().ShowError("Invalid execution-plan action.");
    return 0;
  }

  const auto& identity = workspace_session_.connection();
  if (!identity.has_value()) {
    Results().ShowError("Open a TurboDB connection before explaining SQL.");
    return 0;
  }

  std::string sql = Editor().SelectedText();
  if (sql.empty()) sql = Editor().Text();
  const bool has_content = std::any_of(
      sql.begin(), sql.end(),
      [](unsigned char ch) { return std::isspace(ch) == 0; });
  if (!has_content) {
    Results().ShowError("SQL text is empty.");
    return 0;
  }

  std::string error;
  std::uint64_t request_id = 0u;
  if (!explain_controller_.Execute(identity->id, std::move(sql), mode,
                                   m_hWnd, &request_id, &error)) {
    Results().ShowError(error);
    return 0;
  }

  active_explain_request_id_ = request_id;
  request_page_ = &notebook_.Active();
  workspace_session_.SetExecutionState(WorkspaceExecutionState::running);
  Results().SetExplainRunning(mode);
  UpdateToolbarState();
  return 0;
}

LRESULT MainWindow::OnQueryExecutionCompleted(UINT, WPARAM wparam, LPARAM,
                                              BOOL&) {
  const std::uint64_t request_id = static_cast<std::uint64_t>(wparam);
  auto result = query_controller_.TakeCompleted(request_id);
  if (!result) return 0;

  if (active_request_id_ == request_id) active_request_id_ = 0u;
  workspace_session_.SetExecutionState(WorkspaceExecutionState::idle);
  if (request_page_ != nullptr) request_page_->results.Render(*result);
  request_page_ = nullptr;
  UpdateToolbarState();
  return 0;
}

LRESULT MainWindow::OnExplainExecutionCompleted(UINT, WPARAM wparam, LPARAM,
                                                BOOL&) {
  const std::uint64_t request_id = static_cast<std::uint64_t>(wparam);
  auto plan = explain_controller_.TakeCompleted(request_id);
  if (!plan) return 0;

  if (active_explain_request_id_ == request_id)
    active_explain_request_id_ = 0u;
  workspace_session_.SetExecutionState(WorkspaceExecutionState::idle);
  if (request_page_ != nullptr) request_page_->results.RenderPlan(*plan);
  request_page_ = nullptr;
  UpdateToolbarState();
  return 0;
}

LRESULT MainWindow::OnDestroy(UINT, WPARAM, LPARAM, BOOL &) {
  PostQuitMessage(0);
  return 0;
}

LRESULT MainWindow::OnClose(UINT, WPARAM, LPARAM, BOOL& handled) {
  // The connection API has no cancellation handle. Keep pumping messages until
  // the attempt completes instead of destroying owners still borrowed by it.
  handled = ConnectionOpening() || request_page_ != nullptr || !notebook_.ConfirmDiscardAll();
  return 0;
}

void MainWindow::OpenSelectedRelation(bool execute) {
  const auto sql = explorer_.SelectedQuery();
  if (sql.empty()) return;
  if (!notebook_.AddPage({}, sql)) {
    Results().ShowError("Could not open the table in a new query page.");
    return;
  }
  UpdateToolbarState();
  if (execute) {
    BOOL handled = TRUE;
    OnSqlExecuteRequested(kSqlEditorExecuteRequested, 0, 0, handled);
  }
}

void MainWindow::RefreshSchema() {
  const auto identity = *workspace_session_.connection();
  if (!SetTimer(kRefreshTimer, 50)) {
    Results().ShowError("Could not schedule schema refresh."); return;
  }
  try {
    pending_connection_ = std::async(std::launch::async, [this, identity] {
      PreparedConnection prepared;
      SchemaExplorerController controller(prepared.model, prepared.session);
      auto* connection = connections_.Get(identity.id);
      if (connection != nullptr)
        prepared.opened = controller.Refresh(connection, identity, &prepared.error);
      else prepared.error = "The connection is no longer available.";
      return prepared;
    });
    refreshing_schema_ = true;
    explorer_.SetRefreshing(true);
  } catch (...) {
    KillTimer(kRefreshTimer);
    Results().ShowError("Could not start schema refresh.");
  }
  UpdateToolbarState();
}

LRESULT MainWindow::OnRefreshTimer(UINT, WPARAM timer, LPARAM, BOOL& handled) {
  if (timer != kRefreshTimer || !refreshing_schema_) { handled = FALSE; return 0; }
  if (!ConnectionOpenReady()) return 0;
  KillTimer(kRefreshTimer);
  refreshing_schema_ = false;
  explorer_.SetRefreshing(false);
  try {
    auto prepared = pending_connection_.get();
    if (prepared.opened) {
      explorer_model_ = std::move(prepared.model);
      workspace_session_ = std::move(prepared.session);
      explorer_.Refresh();
      if (!notebook_.RefreshLanguage(false)) Results().ShowError("Could not refresh SQL language assistance.");
    } else Results().ShowError(prepared.error);
  } catch (...) {
    Results().ShowError("Could not refresh schema. The previous schema is unchanged.");
  }
  UpdateToolbarState();
  return 0;
}

}  // namespace turbodb::app
