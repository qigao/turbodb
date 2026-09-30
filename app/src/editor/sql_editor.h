#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>

#include "editor/scintilla_runtime.h"
#include "language/sql_language_service.h"
#include "workspace/sql_workspace_session.h"

namespace turbodb::app {

constexpr UINT kSqlEditorExecuteRequested = WM_APP + 0x122;

struct EditorCaret {
  std::size_t line = 0;
  std::size_t column = 0;
};

class SqlEditor final : public CWindowImpl<SqlEditor, CWindow> {
 public:
  DECLARE_WND_SUPERCLASS(L"TurboDBStudioSqlEditor", L"Scintilla")

  BEGIN_MSG_MAP(SqlEditor)
    MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
  END_MSG_MAP()

  void Bind(ScintillaRuntime* runtime, SqlWorkspaceSession* session,
            const SqlLanguageService* language_service,
            HWND command_target) noexcept;
  bool Initialize();

  std::string Text() const;
  std::string SelectedText() const;
  std::string CurrentWordPrefix() const;
  EditorCaret Caret() const noexcept;

  void SetText(std::string_view text);
  void ReplaceSelection(std::string_view text);
  void Undo();
  void Redo();

  bool FindNext(std::string_view needle, bool match_case);
  bool RefreshLanguage();
  void ShowCompletionItems(const std::vector<std::string>& items,
                           std::size_t prefix_bytes);

  void ClearDiagnosticMarkers();
  void MarkDiagnosticLine(std::size_t zero_based_line);

 private:
  LRESULT OnKeyDown(UINT message, WPARAM wparam, LPARAM lparam, BOOL& handled);

  void ConfigureBaseEditor();
  void ConfigureStyles(SqlProvider provider);

  ScintillaRuntime* runtime_ = nullptr;
  SqlWorkspaceSession* session_ = nullptr;
  const SqlLanguageService* language_service_ = nullptr;
  HWND command_target_ = nullptr;
};

}  // namespace turbodb::app
