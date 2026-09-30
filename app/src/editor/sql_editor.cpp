#include "editor/sql_editor.h"

#include <algorithm>
#include <cctype>
#include <limits>

#include <Scintilla.h>
#include <SciLexer.h>

namespace turbodb::app {

namespace {

constexpr int kDiagnosticMarker = 0;
constexpr char kCompletionSeparator = '\x1e';

std::string ReadEditorRange(HWND hwnd, LRESULT start, LRESULT end) {
  if (end <= start) return {};
  const std::size_t length = static_cast<std::size_t>(end - start);
  std::string text(length + 1, '\0');
  Sci_TextRangeFull range{};
  range.chrg.cpMin = static_cast<Sci_Position>(start);
  range.chrg.cpMax = static_cast<Sci_Position>(end);
  range.lpstrText = text.data();
  ::SendMessage(hwnd, SCI_GETTEXTRANGEFULL, 0,
                reinterpret_cast<LPARAM>(&range));
  text.resize(length);
  return text;
}

}  // namespace

void SqlEditor::Bind(ScintillaRuntime* runtime, SqlWorkspaceSession* session,
                     const SqlLanguageService* language_service) noexcept {
  runtime_ = runtime;
  session_ = session;
  language_service_ = language_service;
}

bool SqlEditor::Initialize() {
  if (!IsWindow() || runtime_ == nullptr || session_ == nullptr ||
      language_service_ == nullptr) {
    return false;
  }
  ConfigureBaseEditor();
  return RefreshLanguage();
}

LRESULT SqlEditor::OnKeyDown(UINT, WPARAM wparam, LPARAM, BOOL& handled) {
  const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
  if (control && wparam == VK_SPACE) {
    const std::string prefix = CurrentWordPrefix();
    const auto items = language_service_->Complete(*session_, prefix);
    ShowCompletionItems(items, prefix.size());
    handled = TRUE;
    return 0;
  }
  handled = FALSE;
  return 0;
}

void SqlEditor::ConfigureBaseEditor() {
  SendMessage(SCI_SETCODEPAGE, SC_CP_UTF8, 0);
  SendMessage(SCI_SETTABWIDTH, 4, 0);
  SendMessage(SCI_SETINDENT, 4, 0);
  SendMessage(SCI_SETUSETABS, FALSE, 0);
  SendMessage(SCI_SETWRAPMODE, SC_WRAP_NONE, 0);
  SendMessage(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
  SendMessage(SCI_SETMARGINWIDTHN, 0, 48);
  SendMessage(SCI_STYLESETFONT, STYLE_DEFAULT,
              reinterpret_cast<LPARAM>("Consolas"));
  SendMessage(SCI_STYLESETSIZE, STYLE_DEFAULT, 11);
  SendMessage(SCI_STYLECLEARALL, 0, 0);
  SendMessage(SCI_MARKERDEFINE, kDiagnosticMarker, SC_MARK_CIRCLE);
  SendMessage(SCI_AUTOCSETSEPARATOR, kCompletionSeparator, 0);
  SendMessage(SCI_AUTOCSETIGNORECASE, TRUE, 0);
}

void SqlEditor::ConfigureStyles(SqlProvider provider) {
  if (provider == SqlProvider::mysql) {
    SendMessage(SCI_STYLESETFORE, SCE_MYSQL_COMMENT, RGB(0, 128, 0));
    SendMessage(SCI_STYLESETFORE, SCE_MYSQL_COMMENTLINE, RGB(0, 128, 0));
    SendMessage(SCI_STYLESETFORE, SCE_MYSQL_MAJORKEYWORD, RGB(0, 0, 192));
    SendMessage(SCI_STYLESETFORE, SCE_MYSQL_KEYWORD, RGB(0, 0, 192));
    SendMessage(SCI_STYLESETFORE, SCE_MYSQL_STRING, RGB(160, 32, 32));
    SendMessage(SCI_STYLESETFORE, SCE_MYSQL_SQSTRING, RGB(160, 32, 32));
    SendMessage(SCI_STYLESETFORE, SCE_MYSQL_NUMBER, RGB(128, 0, 128));
    return;
  }

  SendMessage(SCI_STYLESETFORE, SCE_SQL_COMMENT, RGB(0, 128, 0));
  SendMessage(SCI_STYLESETFORE, SCE_SQL_COMMENTLINE, RGB(0, 128, 0));
  SendMessage(SCI_STYLESETFORE, SCE_SQL_COMMENTDOC, RGB(0, 128, 0));
  SendMessage(SCI_STYLESETFORE, SCE_SQL_WORD, RGB(0, 0, 192));
  SendMessage(SCI_STYLESETFORE, SCE_SQL_STRING, RGB(160, 32, 32));
  SendMessage(SCI_STYLESETFORE, SCE_SQL_CHARACTER, RGB(160, 32, 32));
  SendMessage(SCI_STYLESETFORE, SCE_SQL_NUMBER, RGB(128, 0, 128));
}

bool SqlEditor::RefreshLanguage() {
  if (!IsWindow() || runtime_ == nullptr || session_ == nullptr ||
      language_service_ == nullptr) {
    return false;
  }

  const std::string lexer_name =
      language_service_->LexerName(session_->provider());
  Scintilla::ILexer5* lexer = runtime_->CreateLexer(lexer_name.c_str());
  if (lexer == nullptr) return false;

  SendMessage(SCI_SETILEXER, 0, reinterpret_cast<LPARAM>(lexer));
  const std::string keywords =
      language_service_->KeywordList(session_->provider());
  SendMessage(SCI_SETKEYWORDS, 0,
              reinterpret_cast<LPARAM>(keywords.c_str()));
  ConfigureStyles(session_->provider());
  SendMessage(SCI_COLOURISE, 0, -1);
  return true;
}

std::string SqlEditor::Text() const {
  if (!IsWindow()) return {};
  const LRESULT length = ::SendMessageW(m_hWnd, SCI_GETTEXTLENGTH, 0, 0);
  if (length <= 0) return {};
  std::string text(static_cast<std::size_t>(length) + 1, '\0');
  ::SendMessageW(m_hWnd, SCI_GETTEXT, static_cast<WPARAM>(text.size()),
                 reinterpret_cast<LPARAM>(text.data()));
  text.resize(static_cast<std::size_t>(length));
  return text;
}

std::string SqlEditor::SelectedText() const {
  if (!IsWindow()) return {};
  const LRESULT start =
      ::SendMessageW(m_hWnd, SCI_GETSELECTIONSTART, 0, 0);
  const LRESULT end =
      ::SendMessageW(m_hWnd, SCI_GETSELECTIONEND, 0, 0);
  return ReadEditorRange(m_hWnd, std::min(start, end), std::max(start, end));
}

std::string SqlEditor::CurrentWordPrefix() const {
  if (!IsWindow()) return {};
  const LRESULT current = ::SendMessageW(m_hWnd, SCI_GETCURRENTPOS, 0, 0);
  LRESULT start = current;
  while (start > 0) {
    const int ch = static_cast<int>(::SendMessageW(
        m_hWnd, SCI_GETCHARAT, static_cast<WPARAM>(start - 1), 0));
    const unsigned char byte = static_cast<unsigned char>(ch);
    const bool identifier_byte =
        std::isalnum(byte) != 0 || byte == '_' || byte == '$' ||
        byte == '.' || byte >= 0x80;
    if (!identifier_byte) break;
    --start;
  }
  return ReadEditorRange(m_hWnd, start, current);
}

EditorCaret SqlEditor::Caret() const noexcept {
  if (!IsWindow()) return {};
  const LRESULT position =
      ::SendMessageW(m_hWnd, SCI_GETCURRENTPOS, 0, 0);
  const LRESULT line = ::SendMessageW(
      m_hWnd, SCI_LINEFROMPOSITION, static_cast<WPARAM>(position), 0);
  const LRESULT line_start = ::SendMessageW(
      m_hWnd, SCI_POSITIONFROMLINE, static_cast<WPARAM>(line), 0);
  return {static_cast<std::size_t>(line),
          static_cast<std::size_t>(position - line_start)};
}

void SqlEditor::SetText(std::string_view text) {
  if (!IsWindow()) return;
  std::string owned(text);
  SendMessage(SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(owned.c_str()));
}

void SqlEditor::ReplaceSelection(std::string_view text) {
  if (!IsWindow()) return;
  std::string owned(text);
  SendMessage(SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>(owned.c_str()));
}

void SqlEditor::Undo() {
  if (IsWindow()) SendMessage(SCI_UNDO, 0, 0);
}

void SqlEditor::Redo() {
  if (IsWindow()) SendMessage(SCI_REDO, 0, 0);
}

bool SqlEditor::FindNext(std::string_view needle, bool match_case) {
  if (!IsWindow() || needle.empty()) return false;
  const LRESULT current = SendMessage(SCI_GETCURRENTPOS, 0, 0);
  const LRESULT length = SendMessage(SCI_GETTEXTLENGTH, 0, 0);
  SendMessage(SCI_SETTARGETSTART, static_cast<WPARAM>(current), 0);
  SendMessage(SCI_SETTARGETEND, static_cast<WPARAM>(length), 0);
  SendMessage(SCI_SETSEARCHFLAGS, match_case ? SCFIND_MATCHCASE : 0, 0);
  const LRESULT found = SendMessage(
      SCI_SEARCHINTARGET, static_cast<WPARAM>(needle.size()),
      reinterpret_cast<LPARAM>(needle.data()));
  if (found < 0) return false;
  const LRESULT target_end = SendMessage(SCI_GETTARGETEND, 0, 0);
  SendMessage(SCI_SETSEL, static_cast<WPARAM>(found),
              static_cast<LPARAM>(target_end));
  SendMessage(SCI_SCROLLCARET, 0, 0);
  return true;
}

void SqlEditor::ShowCompletionItems(const std::vector<std::string>& items,
                                    std::size_t prefix_bytes) {
  if (!IsWindow() || items.empty()) return;
  std::string joined;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0) joined.push_back(kCompletionSeparator);
    joined += items[i];
  }
  SendMessage(SCI_AUTOCSHOW, static_cast<WPARAM>(prefix_bytes),
              reinterpret_cast<LPARAM>(joined.c_str()));
}

void SqlEditor::ClearDiagnosticMarkers() {
  if (IsWindow()) SendMessage(SCI_MARKERDELETEALL, kDiagnosticMarker, 0);
}

void SqlEditor::MarkDiagnosticLine(std::size_t zero_based_line) {
  if (!IsWindow() ||
      zero_based_line > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return;
  }
  SendMessage(SCI_MARKERADD, static_cast<WPARAM>(zero_based_line),
              kDiagnosticMarker);
}

}  // namespace turbodb::app
 ||
        byte == '.' || byte >= 0x80;
    if (!identifier_byte) break;
    --start;
  }
  return ReadEditorRange(m_hWnd, start, current);
}

EditorCaret SqlEditor::Caret() const noexcept {
  if (!IsWindow()) return {};
  const LRESULT position = SendMessage(SCI_GETCURRENTPOS, 0, 0);
  const LRESULT line =
      SendMessage(SCI_LINEFROMPOSITION, static_cast<WPARAM>(position), 0);
  const LRESULT line_start =
      SendMessage(SCI_POSITIONFROMLINE, static_cast<WPARAM>(line), 0);
  return {static_cast<std::size_t>(line),
          static_cast<std::size_t>(position - line_start)};
}

void SqlEditor::SetText(std::string_view text) {
  if (!IsWindow()) return;
  std::string owned(text);
  SendMessage(SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(owned.c_str()));
}

void SqlEditor::ReplaceSelection(std::string_view text) {
  if (!IsWindow()) return;
  std::string owned(text);
  SendMessage(SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>(owned.c_str()));
}

void SqlEditor::Undo() {
  if (IsWindow()) SendMessage(SCI_UNDO, 0, 0);
}

void SqlEditor::Redo() {
  if (IsWindow()) SendMessage(SCI_REDO, 0, 0);
}

bool SqlEditor::FindNext(std::string_view needle, bool match_case) {
  if (!IsWindow() || needle.empty()) return false;
  const LRESULT current = SendMessage(SCI_GETCURRENTPOS, 0, 0);
  const LRESULT length = SendMessage(SCI_GETTEXTLENGTH, 0, 0);
  SendMessage(SCI_SETTARGETSTART, static_cast<WPARAM>(current), 0);
  SendMessage(SCI_SETTARGETEND, static_cast<WPARAM>(length), 0);
  SendMessage(SCI_SETSEARCHFLAGS, match_case ? SCFIND_MATCHCASE : 0, 0);
  const LRESULT found = SendMessage(
      SCI_SEARCHINTARGET, static_cast<WPARAM>(needle.size()),
      reinterpret_cast<LPARAM>(needle.data()));
  if (found < 0) return false;
  const LRESULT target_end = SendMessage(SCI_GETTARGETEND, 0, 0);
  SendMessage(SCI_SETSEL, static_cast<WPARAM>(found),
              static_cast<LPARAM>(target_end));
  SendMessage(SCI_SCROLLCARET, 0, 0);
  return true;
}

void SqlEditor::ShowCompletionItems(const std::vector<std::string>& items,
                                    std::size_t prefix_bytes) {
  if (!IsWindow() || items.empty()) return;
  std::string joined;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0) joined.push_back(kCompletionSeparator);
    joined += items[i];
  }
  SendMessage(SCI_AUTOCSHOW, static_cast<WPARAM>(prefix_bytes),
              reinterpret_cast<LPARAM>(joined.c_str()));
}

void SqlEditor::ClearDiagnosticMarkers() {
  if (IsWindow()) SendMessage(SCI_MARKERDELETEALL, kDiagnosticMarker, 0);
}

void SqlEditor::MarkDiagnosticLine(std::size_t zero_based_line) {
  if (!IsWindow() ||
      zero_based_line > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return;
  }
  SendMessage(SCI_MARKERADD, static_cast<WPARAM>(zero_based_line),
              kDiagnosticMarker);
}

}  // namespace turbodb::app
