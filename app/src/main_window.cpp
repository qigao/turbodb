#include "main_window.h"

namespace turbodb::app {

namespace {

constexpr DWORD kSplitterStyle =
    WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
constexpr DWORD kPlaceholderStyle =
    WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE;

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

  if (explorer_placeholder_.Create(workspace_splitter_, client,
                                   L"Connections / Schemas",
                                   kPlaceholderStyle) == nullptr) {
    return -1;
  }

  if (editor_placeholder_.Create(query_splitter_, client, L"SQL Workspace",
                                 kPlaceholderStyle) == nullptr) {
    return -1;
  }

  if (result_placeholder_.Create(query_splitter_, client, L"Results",
                                 kPlaceholderStyle) == nullptr) {
    return -1;
  }

  workspace_splitter_.SetSplitterPanes(explorer_placeholder_, query_splitter_);
  workspace_splitter_.SetSplitterPosPct(24);

  query_splitter_.SetSplitterPanes(editor_placeholder_, result_placeholder_);
  query_splitter_.SetSplitterPosPct(64);

  return 0;
}

LRESULT MainWindow::OnSize(UINT, WPARAM, LPARAM lparam, BOOL &) {
  if (workspace_splitter_.IsWindow()) {
    workspace_splitter_.SetWindowPos(
        nullptr, 0, 0, LOWORD(lparam), HIWORD(lparam),
        SWP_NOACTIVATE | SWP_NOZORDER);
  }
  return 0;
}

LRESULT MainWindow::OnDestroy(UINT, WPARAM, LPARAM, BOOL &) {
  PostQuitMessage(0);
  return 0;
}

}  // namespace turbodb::app
