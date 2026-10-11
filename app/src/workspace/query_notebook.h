#pragma once

#include <memory>
#include <vector>
#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlctrls.h>
#include <atlsplit.h>
#include "editor/sql_editor.h"
#include "results/result_workspace_view.h"

namespace turbodb::app {

struct QueryPage {
  CHorSplitterWindow splitter;
  SqlEditor editor;
  ResultWorkspaceView results;
  std::wstring title;
  QueryPage() = default;
  QueryPage(const QueryPage&) = delete;
  QueryPage& operator=(const QueryPage&) = delete;
  ~QueryPage() { if (splitter.IsWindow()) splitter.DestroyWindow(); }
};

class QueryNotebook final : public CWindowImpl<QueryNotebook> {
 public:
  static constexpr UINT kTabsId = 4401;
  static constexpr std::size_t kMaxPages = 32;
  DECLARE_WND_CLASS_EX(L"TurboDBStudioQueryNotebook", CS_DBLCLKS, COLOR_WINDOW)
  BEGIN_MSG_MAP(QueryNotebook)
    MESSAGE_HANDLER(WM_CREATE, OnCreate)
    MESSAGE_HANDLER(WM_SIZE, OnSize)
    MESSAGE_HANDLER(WM_NOTIFY, OnNotify)
    MESSAGE_HANDLER(WM_COMMAND, OnCommand)
    MESSAGE_HANDLER(WM_CONTEXTMENU, OnContextMenu)
  END_MSG_MAP()

  void Bind(ScintillaRuntime* runtime, SqlWorkspaceSession* session,
            const SqlLanguageService* language, HWND frame);
  bool AddPage(std::wstring title = {}, std::string_view sql = {});
  bool CloseActive();
  bool ConfirmDiscardAll();
  void Cycle(bool previous);
  bool ActivateEditor(HWND editor);
  QueryPage& Active() const { return *pages_[active_]; }
  bool HandleKey(MSG* message);
  bool RefreshLanguage(bool clear_results);
  void RefreshColors();
  bool CanAdd() const { return pages_.size() < kMaxPages; }
  void UpdateActions(bool available, bool can_close);
  void FocusTabs() { tabs_.SetFocus(); }
  bool EditorVisible() const { return Active().splitter.GetSinglePaneMode() != SPLIT_PANE_BOTTOM; }
  bool ResultsVisible() const { return Active().splitter.GetSinglePaneMode() != SPLIT_PANE_TOP; }
  void ToggleEditor();
  void ToggleResults();
  void FocusContent();
  void ShowEditor();
  void ShowResults(bool focus = true);

 private:
  void Select(int index, bool focus);
  void Layout();
  void UpdateTitle(std::size_t index);
  bool ConfirmDiscard(const QueryPage& page);
  LRESULT OnCreate(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnSize(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnNotify(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnCommand(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnContextMenu(UINT, WPARAM, LPARAM, BOOL&);
  CTabCtrl tabs_;
  CButton add_, close_;
  CFont font_;
  std::vector<std::unique_ptr<QueryPage>> pages_;
  std::size_t active_ = 0;
  unsigned int next_title_ = 1;
  ScintillaRuntime* runtime_ = nullptr;
  SqlWorkspaceSession* session_ = nullptr;
  const SqlLanguageService* language_ = nullptr;
  HWND frame_ = nullptr;
};

}  // namespace turbodb::app
