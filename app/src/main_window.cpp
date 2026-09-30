#include "main_window.h"

#include <algorithm>
#include <cctype>

namespace turbodb::app {

namespace {

constexpr DWORD kSplitterStyle =
    WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
constexpr DWORD kEditorStyle =
    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;

}  // namespace

LRESULT MainWindow::OnCreate(UINT, WPARAM, LPARAM, BOOL &) {
  RECT client{};
  GetClientRect(&client);

  if (workspace_splitter_.Create(m_hWnd, client, nullptr, kSplitterStyle) ==
      nullptr) {
    return -1;
  }

  if (query_splitter_.Create(workspace_splitter_, client, nullptr,
                             kSplitterStyle) == nullptr) {
    return -1;
  }

  explorer_.Bind(&explorer_model_, &workspace_session_);
  if (explorer_.Create(workspace_splitter_, client, nullptr,
                       kSplitterStyle) == nullptr) {
    return -1;
  }

  editor_.Bind(&editor_runtime_, &workspace_session_, &language_service_,
               m_hWnd);
  if (editor_.Create(query_splitter_, client, nullptr, kEditorStyle) ==
      nullptr) {
    return -1;
  }
  if (!editor_.Initialize()) {
    editor_.DestroyWindow();
    return -1;
  }

  if (result_view_.Create(query_splitter_, client, nullptr,
                          kSplitterStyle) == nullptr) {
    return -1;
  }

  workspace_splitter_.SetSplitterPanes(explorer_, query_splitter_);
  workspace_splitter_.SetSplitterPosPct(24);

  query_splitter_.SetSplitterPanes(editor_, result_view_);
  query_splitter_.SetSplitterPosPct(64);

  return 0;
}

bool MainWindow::OpenConnection(const ConnectionProfile& profile,
                                std::string* error) {
  WorkspaceConnectionIdentity identity;
  if (!connections_.Open(profile, &identity, error))
    return false;

  orm_connection_t* connection = connections_.Get(identity.id);
  if (connection == nullptr ||
      !explorer_controller_.Refresh(connection, identity, error)) {
    std::string ignored;
    (void)connections_.Close(identity.id, &ignored);
    return false;
  }

  explorer_.Refresh();
  if (editor_.RefreshLanguage())
    return true;

  std::string ignored;
  (void)connections_.Close(identity.id, &ignored);
  workspace_session_.ClearConnection();
  explorer_model_.Clear();
  explorer_.Refresh();
  if (error != nullptr && error->empty())
    *error = "initialize SQL language service for connection";
  return false;
}

bool MainWindow::CloseActiveConnection(std::string* error) {
  if (query_controller_.busy() || query_controller_.has_pending_completion() ||
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
  return editor_.RefreshLanguage();
}

LRESULT MainWindow::OnSize(UINT, WPARAM, LPARAM lparam, BOOL &) {
  if (workspace_splitter_.IsWindow()) {
    workspace_splitter_.SetWindowPos(
        nullptr, 0, 0, LOWORD(lparam), HIWORD(lparam),
        SWP_NOACTIVATE | SWP_NOZORDER);
  }
  return 0;
}

LRESULT MainWindow::OnExplorerSelectionChanged(UINT, WPARAM, LPARAM, BOOL&) {
  (void)editor_.RefreshLanguage();
  return 0;
}

LRESULT MainWindow::OnSqlExecuteRequested(UINT, WPARAM, LPARAM, BOOL&) {
  if (explain_controller_.busy() ||
      explain_controller_.has_pending_completion()) {
    result_view_.ShowError(
        "Wait for the active PLAN/ANALYZE result to be consumed.");
    return 0;
  }
  const auto& identity = workspace_session_.connection();
  if (!identity.has_value()) {
    result_view_.ShowError("Open a TurboDB connection before executing SQL.");
    return 0;
  }

  std::string sql = editor_.SelectedText();
  if (sql.empty()) sql = editor_.Text();
  const bool has_content = std::any_of(
      sql.begin(), sql.end(),
      [](unsigned char ch) { return std::isspace(ch) == 0; });
  if (!has_content) {
    result_view_.ShowError("SQL text is empty.");
    return 0;
  }

  std::string error;
  std::uint64_t request_id = 0u;
  if (!query_controller_.Execute(identity->id, std::move(sql), m_hWnd,
                                 &request_id, &error)) {
    result_view_.ShowError(error);
    return 0;
  }

  active_request_id_ = request_id;
  workspace_session_.SetExecutionState(WorkspaceExecutionState::running);
  result_view_.SetRunning(request_id);
  return 0;
}

LRESULT MainWindow::OnSqlExplainRequested(UINT, WPARAM wparam, LPARAM,
                                              BOOL&) {
  if (query_controller_.busy() ||
      query_controller_.has_pending_completion()) {
    result_view_.ShowError(
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
    result_view_.ShowError("Invalid execution-plan action.");
    return 0;
  }

  const auto& identity = workspace_session_.connection();
  if (!identity.has_value()) {
    result_view_.ShowError("Open a TurboDB connection before explaining SQL.");
    return 0;
  }

  std::string sql = editor_.SelectedText();
  if (sql.empty()) sql = editor_.Text();
  const bool has_content = std::any_of(
      sql.begin(), sql.end(),
      [](unsigned char ch) { return std::isspace(ch) == 0; });
  if (!has_content) {
    result_view_.ShowError("SQL text is empty.");
    return 0;
  }

  std::string error;
  std::uint64_t request_id = 0u;
  if (!explain_controller_.Execute(identity->id, std::move(sql), mode,
                                   m_hWnd, &request_id, &error)) {
    result_view_.ShowError(error);
    return 0;
  }

  active_explain_request_id_ = request_id;
  workspace_session_.SetExecutionState(WorkspaceExecutionState::running);
  result_view_.SetExplainRunning(request_id, mode);
  return 0;
}

LRESULT MainWindow::OnQueryExecutionCompleted(UINT, WPARAM wparam, LPARAM,
                                              BOOL&) {
  const std::uint64_t request_id = static_cast<std::uint64_t>(wparam);
  auto result = query_controller_.TakeCompleted(request_id);
  if (!result) return 0;

  if (active_request_id_ == request_id) active_request_id_ = 0u;
  workspace_session_.SetExecutionState(WorkspaceExecutionState::idle);
  result_view_.Render(*result);
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
  result_view_.RenderPlan(*plan);
  return 0;
}

LRESULT MainWindow::OnDestroy(UINT, WPARAM, LPARAM, BOOL &) {
  PostQuitMessage(0);
  return 0;
}

}  // namespace turbodb::app
