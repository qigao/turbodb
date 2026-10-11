#pragma once

#include <string>
#include <utility>
#include <future>

#include <atlbase.h>
#include <atlapp.h>

extern CAppModule _Module;

#include <atlwin.h>
#include <atlctrls.h>
#include <atluser.h>
#include <atlsplit.h>

#include "command_ids.h"
#include "command_toolbar.h"
#include "connection/connection_manager.h"
#include "connection/connection_config.h"
#include "editor/scintilla_runtime.h"
#include "editor/sql_editor.h"
#include "explorer/schema_explorer_model.h"
#include "explorer/schema_explorer_view.h"
#include "language/sql_language_service.h"
#include "plan/explain_controller.h"
#include "query/query_controller.h"
#include "results/result_workspace_view.h"
#include "workspace/sql_workspace_session.h"
#include "workspace/query_notebook.h"

namespace turbodb::app {

class MainWindow final : public CWindowImpl<MainWindow>,
                         public CMessageFilter, public CIdleHandler {
 public:
  explicit MainWindow(ScintillaRuntime& editor_runtime,
                      std::wstring default_config_path = DefaultConnectionConfigPath()) noexcept
      : editor_runtime_(editor_runtime),
        default_config_path_(std::move(default_config_path)),
        query_controller_(connections_),
        explain_controller_(connections_) {}

  bool OpenConnection(const ConnectionProfile& profile, std::string* error);
  bool CloseActiveConnection(std::string* error);
  bool LoadConnectionConfiguration(const std::wstring& path, std::wstring* error);
  bool InitializeConnectionConfiguration(std::wstring* error);
  const ConnectionConfig& connection_configuration() const noexcept {
    return connection_config_;
  }
  BOOL PreTranslateMessage(MSG* message) override;
  BOOL OnIdle() override;

  DECLARE_WND_CLASS_EX(L"TurboDBStudioMainWindow", CS_DBLCLKS,
                       COLOR_WINDOW)

  BEGIN_MSG_MAP(MainWindow)
    MESSAGE_HANDLER(WM_CREATE, OnCreate)
    MESSAGE_HANDLER(WM_CLOSE, OnClose)
    MESSAGE_HANDLER(WM_SIZE, OnSize)
    MESSAGE_HANDLER(WM_INITMENUPOPUP, OnInitMenuPopup)
    MESSAGE_HANDLER(WM_SYSCOLORCHANGE, OnColorsChanged)
    MESSAGE_HANDLER(WM_SETTINGCHANGE, OnColorsChanged)
    MESSAGE_HANDLER(WM_THEMECHANGED, OnColorsChanged)
    MESSAGE_HANDLER(WM_TIMER, OnRefreshTimer)
    NOTIFY_HANDLER(CommandToolbar::kControlId, NM_CUSTOMDRAW, OnToolbarCustomDraw)
    NOTIFY_HANDLER(CommandToolbar::kControlId, TBN_GETINFOTIP, OnToolbarInfoTip)
    COMMAND_RANGE_HANDLER(kFileExit, kViewQueryTabs, OnMenuCommand)
    COMMAND_RANGE_HANDLER(kRecentConnectionFirst, kRecentConnectionLast, OnRecentConnection)
    MESSAGE_HANDLER(kExplorerSelectionChanged, OnExplorerSelectionChanged)
    MESSAGE_HANDLER(kSqlEditorExecuteRequested, OnSqlExecuteRequested)
    MESSAGE_HANDLER(kSqlEditorExplainRequested, OnSqlExplainRequested)
    MESSAGE_HANDLER(kQueryExecutionCompleted, OnQueryExecutionCompleted)
    MESSAGE_HANDLER(kExplainExecutionCompleted, OnExplainExecutionCompleted)
    MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
  END_MSG_MAP()

 private:
  friend class ConnectionDialog;
  struct PreparedConnection {
    SchemaExplorerModel model;
    SqlWorkspaceSession session;
    std::string error;
    bool opened = false;
  };
  static PreparedConnection PrepareConnection(ConnectionManager& connections,
                                               const ConnectionProfile& profile);
  bool PublishConnection(PreparedConnection prepared, std::string* error);
  bool BeginOpenConnection(ConnectionProfile profile, std::string* error);
  bool ConnectionOpenReady() const;
  bool CompleteOpenConnection(std::string* error);
  bool ConnectionOpening() const noexcept { return pending_connection_.valid(); }
  bool CreateMenus();
  void ResetRecentConnections();
  void RememberConnection(std::size_t profile);
  void UpdateRecentConnectionsMenu();
  LRESULT OnRecentConnection(WORD, WORD id, HWND, BOOL&);
  bool IsCommandEnabled(UINT command) const;
  bool IsPaneVisible(UINT command) const;
  void UpdateToolbarState();
  void LayoutWorkspace(int width, int height);
  LRESULT OnInitMenuPopup(UINT message, WPARAM wparam, LPARAM lparam,
                         BOOL& handled);
  LRESULT OnMenuCommand(WORD notification, WORD id, HWND control,
                        BOOL& handled);
  LRESULT OnCreate(UINT message, WPARAM wparam, LPARAM lparam,
                   BOOL &handled);
  LRESULT OnSize(UINT message, WPARAM wparam, LPARAM lparam, BOOL &handled);
  LRESULT OnExplorerSelectionChanged(UINT message, WPARAM wparam,
                                     LPARAM lparam, BOOL& handled);
  LRESULT OnSqlExecuteRequested(UINT message, WPARAM wparam,
                                LPARAM lparam, BOOL& handled);
  LRESULT OnSqlExplainRequested(UINT message, WPARAM wparam,
                                LPARAM lparam, BOOL& handled);
  LRESULT OnQueryExecutionCompleted(UINT message, WPARAM wparam,
                                   LPARAM lparam, BOOL& handled);
  LRESULT OnExplainExecutionCompleted(UINT message, WPARAM wparam,
                                      LPARAM lparam, BOOL& handled);
  LRESULT OnDestroy(UINT message, WPARAM wparam, LPARAM lparam,
                    BOOL &handled);
  LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL& handled);
  LRESULT OnColorsChanged(UINT, WPARAM, LPARAM, BOOL& handled);
  LRESULT OnToolbarCustomDraw(int, LPNMHDR header, BOOL&);
  LRESULT OnToolbarInfoTip(int, LPNMHDR header, BOOL&);
  SqlEditor& Editor() const { return notebook_.Active().editor; }
  ResultWorkspaceView& Results() const { return notebook_.Active().results; }
  void OpenSelectedRelation(bool execute);
  void RefreshSchema();
  LRESULT OnRefreshTimer(UINT, WPARAM, LPARAM, BOOL&);

  ScintillaRuntime& editor_runtime_;
  std::wstring default_config_path_;
  CAccelerator accelerators_;
  ConnectionManager connections_;
  ConnectionConfig connection_config_;
  bool connection_config_initialized_ = false;
  // Indices into connection_config_, rebuilt only after a successful INI load.
  std::vector<std::size_t> recent_connections_;
  HMENU recent_connections_menu_ = nullptr;  // Borrowed from the frame's menu.
  SqlWorkspaceSession workspace_session_;
  SqlLanguageService language_service_;
  SchemaExplorerModel explorer_model_;
  QueryController query_controller_;
  ExplainController explain_controller_;
  std::uint64_t active_request_id_ = 0u;
  std::uint64_t active_explain_request_id_ = 0u;

  CommandToolbar toolbar_;
  CSplitterWindow workspace_splitter_;
  SchemaExplorerView explorer_;
  QueryNotebook notebook_;
  // UI-thread borrow: closing pages is blocked until the initiating request is consumed.
  QueryPage* request_page_ = nullptr;
  static constexpr UINT_PTR kRefreshTimer = 4402;
  bool refreshing_schema_ = false;
  // Last member: its async task is joined before any borrowed owner is destroyed.
  // While valid, the worker exclusively accesses connections_; UI publishes only
  // after get(). No worker touches window handles or the live workspace session.
  std::future<PreparedConnection> pending_connection_;
};

}  // namespace turbodb::app
