#include "main_window.h"

namespace turbodb::app {

namespace {

constexpr DWORD kSplitterStyle =
    WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
constexpr DWORD kPlaceholderStyle =
    WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE;
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

  editor_.Bind(&editor_runtime_, &workspace_session_, &language_service_);
  if (editor_.Create(query_splitter_, client, nullptr, kEditorStyle) ==
      nullptr) {
    return -1;
  }
  if (!editor_.Initialize()) {
    editor_.DestroyWindow();
    return -1;
  }

  if (result_placeholder_.Create(query_splitter_, client, L"Results",
                                 kPlaceholderStyle) == nullptr) {
    return -1;
  }

  workspace_splitter_.SetSplitterPanes(explorer_, query_splitter_);
  workspace_splitter_.SetSplitterPosPct(24);

  query_splitter_.SetSplitterPanes(editor_, result_placeholder_);
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
  return editor_.RefreshLanguage();
}

bool MainWindow::CloseActiveConnection(std::string* error) {
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

LRESULT MainWindow::OnDestroy(UINT, WPARAM, LPARAM, BOOL &) {
  PostQuitMessage(0);
  return 0;
}

}  // namespace turbodb::app
