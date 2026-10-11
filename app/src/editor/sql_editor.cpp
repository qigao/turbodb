#include "editor/sql_editor.h"
#include "studio_colors.h"
#include "command_ids.h"
#include <atluser.h>

#include <algorithm>
#include <cctype>
#include <limits>

#include <Scintilla.h>
#include <SciLexer.h>

namespace turbodb::app {

namespace {

constexpr int kDiagnosticMarker = 0;
constexpr char kCompletionSeparator = '\x1e';
constexpr LRESULT kHintScanBytes = 4096;
constexpr int kHoverDelayMs = 600;

bool IdentifierByte(int ch) {
  const auto byte = static_cast<unsigned char>(ch);
  return std::isalnum(byte) != 0 || byte == '_' || byte == '$' || byte == '.' || byte >= 0x80;
}

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
                     const SqlLanguageService* language_service,
                     HWND command_target) noexcept {
  runtime_ = runtime;
  session_ = session;
  language_service_ = language_service;
  command_target_ = command_target;
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
  if (wparam == VK_ESCAPE &&
      (SendMessage(SCI_CALLTIPACTIVE) || SendMessage(SCI_AUTOCACTIVE))) {
    CancelTip();
    SendMessage(SCI_AUTOCCANCEL);
    return 0;
  }
  if (control && wparam == VK_SPACE) {
    if ((::GetKeyState(VK_SHIFT) & 0x8000) != 0) {
      SendMessage(SCI_AUTOCCANCEL);
      CancelTip();
      if (!ShowParameterTip()) ShowSymbolTip(SendMessage(SCI_GETCURRENTPOS), false);
    } else {
      RequestCompletion(true);
    }
    handled = TRUE;
    return 0;
  }
  if ((control && wparam == VK_RETURN) || wparam == VK_F5) {
    if (command_target_ != nullptr && ::IsWindow(command_target_))
      (void)::PostMessageW(command_target_, kSqlEditorExecuteRequested, 0, reinterpret_cast<LPARAM>(m_hWnd));
    handled = TRUE;
    return 0;
  }
  if (wparam == VK_F6) {
    const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (command_target_ != nullptr && ::IsWindow(command_target_))
      (void)::PostMessageW(
          command_target_, kSqlEditorExplainRequested,
          shift ? static_cast<WPARAM>(SqlExplainAction::analyze)
                : static_cast<WPARAM>(SqlExplainAction::plan),
          reinterpret_cast<LPARAM>(m_hWnd));
    handled = TRUE;
    return 0;
  }
  handled = FALSE;
  return 0;
}

LRESULT SqlEditor::OnContextMenu(UINT, WPARAM, LPARAM lparam, BOOL&) {
  CancelTip();
  SendMessage(SCI_AUTOCCANCEL);
  SetFocus();
  POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
  if (point.x == -1 && point.y == -1) {
    const auto caret = SendMessage(SCI_GETCURRENTPOS);
    point.x = static_cast<LONG>(SendMessage(SCI_POINTXFROMPOSITION, 0, caret));
    point.y = static_cast<LONG>(SendMessage(SCI_POINTYFROMPOSITION, 0, caret) +
        SendMessage(SCI_TEXTHEIGHT, SendMessage(SCI_LINEFROMPOSITION, caret)));
    ClientToScreen(&point);
  }
  CMenu menu;
  if (!menu.CreatePopupMenu()) return 0;
  menu.AppendMenu(MF_STRING, kQueryExecute, L"Execute SQL\tF5 / Ctrl+Enter");
  menu.AppendMenu(MF_STRING, kQueryPlan, L"Execution Plan\tF6");
  menu.AppendMenu(MF_SEPARATOR);
  menu.AppendMenu(MF_STRING, kEditUndo, L"Undo\tCtrl+Z");
  menu.AppendMenu(MF_STRING, kEditRedo, L"Redo\tCtrl+Y");
  menu.AppendMenu(MF_SEPARATOR);
  menu.AppendMenu(MF_STRING, kEditCut, L"Cut\tCtrl+X");
  menu.AppendMenu(MF_STRING, kEditCopy, L"Copy\tCtrl+C");
  menu.AppendMenu(MF_STRING, kEditPaste, L"Paste\tCtrl+V");
  menu.AppendMenu(MF_STRING, kEditSelectAll, L"Select All\tCtrl+A");
  menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON, point.x, point.y, command_target_);
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
  SendMessage(SCI_AUTOCSETCHOOSESINGLE, FALSE);
  SendMessage(SCI_AUTOCSETDROPRESTOFWORD, TRUE);
  SendMessage(SCI_AUTOCSETMAXHEIGHT, 10);
  SendMessage(SCI_AUTOCSETMAXWIDTH, 64);
  SendMessage(SCI_SETMOUSEDWELLTIME, kHoverDelayMs);
}

void SqlEditor::ConfigureStyles(SqlProvider provider) {
  const auto colors = CurrentStudioColors();
  SendMessage(SCI_STYLESETFORE, STYLE_DEFAULT, colors.ink);
  SendMessage(SCI_STYLESETBACK, STYLE_DEFAULT, colors.paper);
  // Reset all style slots when changing lexers as SQL and MySQL reuse IDs.
  SendMessage(SCI_STYLECLEARALL);
  SendMessage(SCI_CALLTIPSETFORE, colors.ink);
  SendMessage(SCI_CALLTIPSETBACK, colors.system ? colors.paper : colors.surface);
  SendMessage(SCI_CALLTIPSETFOREHLT, colors.system ? colors.ink : colors.blue);
  SendMessage(SCI_STYLESETFORE, STYLE_LINENUMBER, colors.muted);
  SendMessage(SCI_STYLESETBACK, STYLE_LINENUMBER, colors.surface);
  SendMessage(SCI_SETCARETFORE, colors.ink);
  SendMessage(SCI_SETCARETLINEBACK, colors.current_line);
  SendMessage(SCI_SETCARETLINEVISIBLE, !colors.system);
  SendMessage(SCI_SETSELFORE, TRUE,
              colors.system ? ::GetSysColor(COLOR_HIGHLIGHTTEXT) : colors.ink);
  SendMessage(SCI_SETSELBACK, TRUE, colors.selection);
  SendMessage(SCI_MARKERSETFORE, kDiagnosticMarker, colors.paper);
  SendMessage(SCI_MARKERSETBACK, kDiagnosticMarker, colors.red);
  if (provider == SqlProvider::mysql) {
    SendMessage(SCI_STYLESETFORE, SCE_MYSQL_COMMENT, colors.green);
    SendMessage(SCI_STYLESETFORE, SCE_MYSQL_COMMENTLINE, colors.green);
    SendMessage(SCI_STYLESETFORE, SCE_MYSQL_MAJORKEYWORD, colors.blue);
    SendMessage(SCI_STYLESETFORE, SCE_MYSQL_KEYWORD, colors.blue);
    SendMessage(SCI_STYLESETBOLD, SCE_MYSQL_MAJORKEYWORD, TRUE);
    SendMessage(SCI_STYLESETBOLD, SCE_MYSQL_KEYWORD, TRUE);
    SendMessage(SCI_STYLESETFORE, SCE_MYSQL_FUNCTION, colors.teal);
    SendMessage(SCI_STYLESETFORE, SCE_MYSQL_STRING, colors.red);
    SendMessage(SCI_STYLESETFORE, SCE_MYSQL_SQSTRING, colors.red);
    SendMessage(SCI_STYLESETFORE, SCE_MYSQL_DQSTRING, colors.red);
    SendMessage(SCI_STYLESETFORE, SCE_MYSQL_NUMBER, colors.purple);
    return;
  }

  SendMessage(SCI_STYLESETFORE, SCE_SQL_COMMENT, colors.green);
  SendMessage(SCI_STYLESETFORE, SCE_SQL_COMMENTLINE, colors.green);
  SendMessage(SCI_STYLESETFORE, SCE_SQL_COMMENTDOC, colors.green);
  SendMessage(SCI_STYLESETFORE, SCE_SQL_WORD, colors.blue);
  SendMessage(SCI_STYLESETBOLD, SCE_SQL_WORD, TRUE);
  SendMessage(SCI_STYLESETFORE, SCE_SQL_STRING, colors.red);
  SendMessage(SCI_STYLESETFORE, SCE_SQL_CHARACTER, colors.red);
  SendMessage(SCI_STYLESETFORE, SCE_SQL_NUMBER, colors.purple);
}

void SqlEditor::RefreshColors() {
  if (!IsWindow() || session_ == nullptr) return;
  ConfigureStyles(session_->provider());
  SendMessage(SCI_COLOURISE, 0, -1);
}

bool SqlEditor::RefreshLanguage() {
  if (!IsWindow() || runtime_ == nullptr || session_ == nullptr ||
      language_service_ == nullptr) {
    return false;
  }

  CancelTip();
  SendMessage(SCI_AUTOCCANCEL);

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
  if (!IsWindow()) return;
  if (items.empty()) {
    SendMessage(SCI_AUTOCCANCEL);
    return;
  }
  std::string joined;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i != 0) joined.push_back(kCompletionSeparator);
    joined += items[i];
  }
  SendMessage(SCI_AUTOCSHOW, static_cast<WPARAM>(prefix_bytes),
              reinterpret_cast<LPARAM>(joined.c_str()));
}

void SqlEditor::EnsureStyled(LRESULT end) {
  const LRESULT start = SendMessage(SCI_GETENDSTYLED);
  if (start < end) {
    // A closing quote carries the string style too. Restart at the line boundary
    // so Lexilla sees that delimiter instead of inheriting its style as state.
    const auto line = SendMessage(SCI_LINEFROMPOSITION, start);
    SendMessage(SCI_COLOURISE, SendMessage(SCI_POSITIONFROMLINE, line), end);
  }
}

bool SqlEditor::IsCode(LRESULT position) const {
  if (position < 0) return false;
  const auto style = ::SendMessageW(m_hWnd, SCI_GETSTYLEAT, position, 0);
  if (session_->provider() == SqlProvider::mysql) {
    return style != SCE_MYSQL_COMMENT && style != SCE_MYSQL_COMMENTLINE &&
        style != SCE_MYSQL_STRING && style != SCE_MYSQL_SQSTRING &&
        style != SCE_MYSQL_DQSTRING && style != SCE_MYSQL_QUOTEDIDENTIFIER;
  }
  return style != SCE_SQL_COMMENT && style != SCE_SQL_COMMENTLINE &&
      style != SCE_SQL_COMMENTDOC && style != SCE_SQL_COMMENTLINEDOC &&
      style != SCE_SQL_COMMENTDOCKEYWORD && style != SCE_SQL_COMMENTDOCKEYWORDERROR &&
      style != SCE_SQL_SQLPLUS_COMMENT && style != SCE_SQL_STRING &&
      style != SCE_SQL_CHARACTER && style != SCE_SQL_QUOTEDIDENTIFIER &&
      style != SCE_SQL_QOPERATOR;
}

void SqlEditor::CancelTip() {
  hover_tip_ = false;
  hint_caret_ = -1;
  if (IsWindow()) SendMessage(SCI_CALLTIPCANCEL);
}

bool SqlEditor::RequestCompletion(bool explicit_request) {
  const auto caret = SendMessage(SCI_GETCURRENTPOS);
  EnsureStyled(caret);
  const std::string prefix = CurrentWordPrefix();
  const bool identifier = prefix.empty() ||
      (!std::isdigit(static_cast<unsigned char>(prefix.front())) && prefix.front() != '.');
  if (SendMessage(SCI_GETSELECTIONSTART) != SendMessage(SCI_GETSELECTIONEND) ||
      (caret > 0 && !IsCode(caret - 1)) || !identifier ||
      (!explicit_request && prefix.size() < 2)) {
    SendMessage(SCI_AUTOCCANCEL);
    return false;
  }
  const auto items = language_service_->Complete(*session_, prefix);
  if (!items.empty()) CancelTip();
  ShowCompletionItems(items, prefix.size());
  return !items.empty();
}

bool SqlEditor::ShowParameterTip() {
  const auto caret = SendMessage(SCI_GETCURRENTPOS);
  EnsureStyled(caret);
  if (caret == 0 || !IsCode(caret - 1)) return false;
  const LRESULT lower = caret > kHintScanBytes ? caret - kHintScanBytes : 0;
  int depth = 0;
  std::size_t argument = 0;
  for (LRESULT pos = caret - 1; pos >= lower; --pos) {
    if (!IsCode(pos)) continue;
    const int ch = static_cast<int>(SendMessage(SCI_GETCHARAT, pos));
    if (ch == ';') return false;
    if (ch == ')') ++depth;
    else if (ch == '(') {
      if (depth != 0) { --depth; continue; }
      LRESULT end = pos;
      while (end > lower && std::isspace(static_cast<unsigned char>(SendMessage(SCI_GETCHARAT, end - 1))))
        --end;
      LRESULT start = end;
      while (start > lower && IdentifierByte(static_cast<int>(SendMessage(SCI_GETCHARAT, start - 1))) &&
             IsCode(start - 1)) --start;
      if (start == end || (start == lower && lower != 0)) return false;
      const auto signature = language_service_->FunctionSignature(session_->provider(),
          ReadEditorRange(m_hWnd, start, end));
      if (signature.empty()) return false;
      SendMessage(SCI_CALLTIPSHOW, start, reinterpret_cast<LPARAM>(signature.c_str()));
      // The signature's comma-separated parameter labels are UI data, not SQL.
      std::size_t first = signature.find('(') + 1, index = 0;
      std::size_t last = signature.find(',', first);
      while (index < argument && last != std::string::npos) {
        first = last + 1;
        last = signature.find(',', first);
        ++index;
      }
      if (last == std::string::npos) last = signature.rfind(')');
      while (first < last && signature[first] == ' ') ++first;
      if (index == argument || signature.find("...", first) != std::string::npos)
        SendMessage(SCI_CALLTIPSETHLT, first, last);
      else SendMessage(SCI_CALLTIPSETHLT, 0, 0);
      hint_caret_ = caret;
      hover_tip_ = false;
      return true;
    } else if (ch == ',' && depth == 0) ++argument;
  }
  return false;
}

void SqlEditor::ShowSymbolTip(LRESULT position, bool hover) {
  const auto length = SendMessage(SCI_GETLENGTH);
  if (!hover && position > 0 && (position == length ||
      !IdentifierByte(static_cast<int>(SendMessage(SCI_GETCHARAT, position))))) --position;
  if (position < 0 || position >= length) return;
  EnsureStyled(position + 1);
  if (!IsCode(position) || !IdentifierByte(static_cast<int>(SendMessage(SCI_GETCHARAT, position)))) return;
  LRESULT start = position, end = position + 1;
  while (start > 0 && position - start < kHintScanBytes &&
         IdentifierByte(static_cast<int>(SendMessage(SCI_GETCHARAT, start - 1)))) --start;
  while (end < length && end - start < kHintScanBytes &&
         IdentifierByte(static_cast<int>(SendMessage(SCI_GETCHARAT, end)))) ++end;
  if (end - start >= kHintScanBytes) return;
  const auto name = ReadEditorRange(m_hWnd, start, end);
  auto text = language_service_->DescribeSymbol(*session_, name);
  if (text.empty()) text = language_service_->FunctionSignature(session_->provider(), name);
  if (text.empty()) return;
  SendMessage(SCI_CALLTIPSHOW, start, reinterpret_cast<LPARAM>(text.c_str()));
  hint_caret_ = SendMessage(SCI_GETCURRENTPOS);
  hover_tip_ = hover;
}

void SqlEditor::HandleNotification(const NMHDR* header) {
  if (session_ == nullptr || language_service_ == nullptr) return;
  const auto& notification = *reinterpret_cast<const SCNotification*>(header);
  switch (header->code) {
    case SCN_CHARADDED:
      if (notification.characterSource == SC_CHARACTERSOURCE_TENTATIVE_INPUT ||
          ::GetFocus() != m_hWnd) break;
      CancelTip();
      if (!RequestCompletion(false)) ShowParameterTip();
      break;
    case SCN_DWELLSTART:
      if (::GetFocus() == m_hWnd && !SendMessage(SCI_AUTOCACTIVE) && !SendMessage(SCI_CALLTIPACTIVE))
        ShowSymbolTip(notification.position, true);
      break;
    case SCN_DWELLEND:
      if (hover_tip_) CancelTip();
      break;
    case SCN_UPDATEUI:
      if (hint_caret_ >= 0 && (hint_caret_ != SendMessage(SCI_GETCURRENTPOS) ||
          (notification.updated & (SC_UPDATE_V_SCROLL | SC_UPDATE_H_SCROLL)) != 0)) CancelTip();
      break;
    case SCN_MODIFIED:
      if ((notification.modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) != 0) CancelTip();
      break;
    case SCN_FOCUSOUT:
      CancelTip();
      SendMessage(SCI_AUTOCCANCEL);
      break;
  }
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
