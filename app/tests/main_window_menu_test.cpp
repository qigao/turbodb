#include "main_window.h"
#include "connection/connection_resource.h"
#include "connection_config_fixture.h"
#include "studio_colors.h"

#include <algorithm>
#include <memory>
#include <cwchar>
#include <string>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <functional>
#include <cmath>
#include <cstdio>
#include <limits>

#include <atlgdi.h>
#include <Scintilla.h>
#include <SciLexer.h>
#include <tinytest.hpp>

CAppModule _Module;

namespace {

using namespace turbodb::app;

struct DialogScript {
  HWND owner;
  std::function<void(HWND)> action;
  DWORD started;
  bool visited = false;
  bool connection_dialog = true;
};

// This fixture runs in the modal dialog's native message loop. No worker
// touches UI controls or the frame's connection owner.
DialogScript* active_dialog_script = nullptr;

void CALLBACK RunDialogScript(HWND, UINT, UINT_PTR timer, DWORD now) {
  auto* script = active_dialog_script;
  if (script == nullptr) return;
  HWND dialog = nullptr;
  ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND candidate, LPARAM data) -> BOOL {
    auto& found = *reinterpret_cast<HWND*>(data);
    if (!::IsWindowVisible(candidate) ||
        ::GetWindow(candidate, GW_OWNER) != active_dialog_script->owner ||
        (active_dialog_script->connection_dialog
            ? ::GetDlgItem(candidate, IDC_CONNECTION_DRIVER) == nullptr
            : ::GetDlgItem(candidate, IDCANCEL) == nullptr))
      return TRUE;
    found = candidate;
    return FALSE;
  }, reinterpret_cast<LPARAM>(&dialog));
  if (dialog != nullptr) {
    ::KillTimer(nullptr, timer);
    script->visited = true;
    script->action(dialog);
  } else if (now - script->started > 5000u) {
    ::KillTimer(nullptr, timer);
    ::PostQuitMessage(1);
  }
}

bool WithConnectionDialog(MainWindow& window, std::function<void(HWND)> action,
                          bool shortcut = false, UINT command = kFileConnect) {
  DialogScript script{window, std::move(action), ::GetTickCount()};
  active_dialog_script = &script;
  const UINT_PTR timer = ::SetTimer(nullptr, 0, 10, RunDialogScript);
  if (timer != 0) {
    if (shortcut) {
      BYTE previous[256]{}, control[256]{};
      ::GetKeyboardState(previous);
      control[VK_CONTROL] = 0x80;
      ::SetKeyboardState(control);
      MSG message{};
      message.hwnd = window;
      message.message = WM_KEYDOWN;
      message.wParam = 'N';
      window.PreTranslateMessage(&message);
      ::SetKeyboardState(previous);
    } else {
      window.SendMessage(WM_COMMAND, command);
    }
  }
  ::KillTimer(nullptr, timer);
  active_dialog_script = nullptr;
  return script.visited;
}

bool WithDiscardDialog(MainWindow& window, UINT command, bool accept) {
  DialogScript script{window, [accept](HWND dialog) {
    ::SendMessageW(dialog, WM_COMMAND, accept ? IDOK : IDCANCEL, 0);
  }, ::GetTickCount()};
  script.connection_dialog = false;
  active_dialog_script = &script;
  const auto timer = ::SetTimer(nullptr, 0, 10, RunDialogScript);
  if (timer) window.SendMessage(WM_COMMAND, command);
  ::KillTimer(nullptr, timer);
  active_dialog_script = nullptr;
  return script.visited;
}

std::wstring SqlitePluginPath() {
  const char* text = TURBODB_APP_SQLITE_PLUGIN_PATH;
  const int length = static_cast<int>(std::strlen(text));
  const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        text, length, nullptr, 0);
  std::wstring output(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, length,
                        output.data(), size);
  return output;
}

std::wstring ControlText(HWND dialog, int id) {
  const HWND control = ::GetDlgItem(dialog, id);
  const int size = ::GetWindowTextLengthW(control);
  std::wstring text(static_cast<std::size_t>(size) + 1, L'\0');
  ::GetWindowTextW(control, text.data(), size + 1);
  text.resize(static_cast<std::size_t>(size));
  return text;
}

bool MenuEnabled(MainWindow& window, UINT command) {
  const int popup = command >= kQueryExecute && command <= kQueryAnalyze ? 2 :
                    command >= kViewExplorer && command <= kViewToolbar ? 3 : 0;
  const HMENU file = ::GetSubMenu(::GetMenu(window), popup);
  window.SendMessage(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(file));
  return (::GetMenuState(file, command, MF_BYCOMMAND) & MF_GRAYED) == 0;
}

HMENU RecentConnectionsMenu(MainWindow& window) {
  const HMENU file = ::GetSubMenu(::GetMenu(window), 0);
  window.SendMessage(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(file));
  return ::GetSubMenu(file, 1);
}

bool ClickToolbar(CToolBarCtrl& toolbar, UINT command) {
  RECT button{};
  if (!toolbar.GetItemRect(toolbar.CommandToIndex(command), &button)) return false;
  const LPARAM point = MAKELPARAM((button.left + button.right) / 2,
                                 (button.top + button.bottom) / 2);
  toolbar.SendMessage(WM_LBUTTONDOWN, MK_LBUTTON, point);
  toolbar.SendMessage(WM_LBUTTONUP, 0, point);
  return true;
}

bool WaitForConnection(HWND dialog) {
  const DWORD started = ::GetTickCount();
  while (::IsWindow(dialog) && !::IsWindowEnabled(::GetDlgItem(dialog, IDOK))) {
    if (::GetTickCount() - started >= 5000u) return false;
    MSG message{};
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (!::IsWindow(dialog) || !::IsDialogMessageW(dialog, &message)) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
      }
    }
    ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
  }
  return true;
}

bool WaitForQuery(MainWindow& window) {
  const DWORD started = ::GetTickCount();
  while (!MenuEnabled(window, kFileDisconnect)) {
    if (::GetTickCount() - started >= 5000u) return false;
    MSG message{};
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (!window.PreTranslateMessage(&message)) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
      }
    }
    ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
  }
  return true;
}

HWND FindEditor(HWND frame) {
  HWND editor = nullptr;
  ::EnumChildWindows(frame, [](HWND child, LPARAM context) -> BOOL {
    wchar_t name[128]{};
    ::GetClassNameW(child, name, _countof(name));
    if (std::wcscmp(name, L"TurboDBStudioSqlEditor") != 0 ||
        (::GetWindowLongW(::GetParent(child), GWL_STYLE) & WS_VISIBLE) == 0) return TRUE;
    *reinterpret_cast<HWND*>(context) = child;
    return FALSE;
  }, reinterpret_cast<LPARAM>(&editor));
  return editor;
}

bool HasWindowText(HWND frame, const wchar_t* expected) {
  struct Search {
    const wchar_t* text;
    bool found = false;
  } search{expected};
  ::EnumChildWindows(frame, [](HWND child, LPARAM context) -> BOOL {
    auto& search = *reinterpret_cast<Search*>(context);
    const int length = ::GetWindowTextLengthW(child);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    ::GetWindowTextW(child, text.data(), length + 1);
    if (std::wstring_view(text.c_str()) != search.text) return TRUE;
    search.found = true;
    return FALSE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.found;
}

HWND FindExplorer(HWND editor) {
  return ::FindWindowExW(::GetParent(::GetParent(::GetParent(editor))), nullptr,
                         L"TurboDBStudioSchemaExplorer", nullptr);
}

bool RenderToolbar(HWND toolbar, SIZE size, std::vector<std::uint32_t>* pixels,
                   bool native_dialog = false) {
  CClientDC reference(toolbar);
  CDC dc;
  CBitmap bitmap;
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = size.cx;
  info.bmiHeader.biHeight = -size.cy;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  if (dc.CreateCompatibleDC(reference) == nullptr ||
      bitmap.CreateDIBSection(reference, &info, DIB_RGB_COLORS, &bits, nullptr, 0) == nullptr)
    return false;
  const HBITMAP old_bitmap = dc.SelectBitmap(bitmap);
  RECT bounds{0, 0, size.cx, size.cy};
  dc.FillRect(&bounds, ::GetSysColorBrush(COLOR_BTNFACE));
  if (native_dialog) {
    if (!::PrintWindow(toolbar, dc, PW_CLIENTONLY)) {
      dc.SelectBitmap(old_bitmap);
      return false;
    }
  } else {
    ::SendMessageW(toolbar, WM_PRINT, reinterpret_cast<WPARAM>(dc.m_hDC),
                    PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
  }
  ::GdiFlush();
  const auto* colors = static_cast<const std::uint32_t*>(bits);
  pixels->assign(colors, colors + static_cast<std::size_t>(size.cx) * size.cy);
  dc.SelectBitmap(old_bitmap);
  return true;
}

int InkCount(const std::vector<std::uint32_t>& pixels, int width, RECT bounds) {
  const auto background = pixels.front() & 0xffffff;
  int ink = 0;
  for (int y = bounds.top; y < bounds.bottom; ++y)
    for (int x = bounds.left; x < bounds.right; ++x)
      if ((pixels[static_cast<std::size_t>(y) * width + x] & 0xffffff) != background)
        ++ink;
  return ink;
}

int ColorCount(const std::vector<std::uint32_t>& pixels, COLORREF color) {
  const auto expected = (static_cast<std::uint32_t>(GetRValue(color)) << 16) |
                        (static_cast<std::uint32_t>(GetGValue(color)) << 8) |
                        GetBValue(color);
  return static_cast<int>(std::count_if(pixels.begin(), pixels.end(), [expected](auto pixel) {
    return (pixel & 0xffffff) == expected;
  }));
}

bool SaveToolbarCapture(const char* path, SIZE size,
                        const std::vector<std::uint32_t>& pixels) {
  BITMAPFILEHEADER file{};
  BITMAPINFOHEADER info{};
  file.bfType = 0x4d42;
  file.bfOffBits = sizeof(file) + sizeof(info);
  file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size() * sizeof(pixels[0]));
  info.biSize = sizeof(info);
  info.biWidth = size.cx;
  info.biHeight = -size.cy;
  info.biPlanes = 1;
  info.biBitCount = 32;
  info.biCompression = BI_RGB;
  std::vector<unsigned char> bytes(file.bfSize);
  std::memcpy(bytes.data(), &file, sizeof(file));
  std::memcpy(bytes.data() + sizeof(file), &info, sizeof(info));
  std::memcpy(bytes.data() + file.bfOffBits, pixels.data(), pixels.size() * sizeof(pixels[0]));
  return tt_write_file(path, bytes.data(), bytes.size()) == 0;
}

bool CaptureDialog(HWND dialog, const char* environment, bool native_dialog = true) {
  const char* path = std::getenv(environment);
  if (path == nullptr) return true;
  RECT client{};
  ::GetClientRect(dialog, &client);
  const SIZE size{client.right, client.bottom};
  std::vector<std::uint32_t> pixels;
  return RenderToolbar(dialog, size, &pixels, native_dialog) &&
         SaveToolbarCapture(path, size, pixels);
}

void SetSql(HWND editor, const char* sql) {
  ::SendMessageW(editor, SCI_AUTOCCANCEL, 0, 0);
  ::SendMessageW(editor, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(sql));
  const auto length = ::SendMessageW(editor, SCI_GETLENGTH, 0, 0);
  ::SendMessageW(editor, SCI_SETSEL, length, length);
}

void TypeSql(HWND editor, std::string_view text) {
  for (const unsigned char ch : text) ::SendMessageW(editor, WM_CHAR, ch, 1);
}

void RequestSqlHelp(HWND editor, bool shift) {
  BYTE previous[256]{}, keys[256]{};
  ::GetKeyboardState(previous);
  keys[VK_CONTROL] = 0x80;
  keys[VK_SHIFT] = shift ? 0x80 : 0;
  ::SetKeyboardState(keys);
  ::SendMessageW(editor, WM_KEYDOWN, VK_SPACE, 0);
  ::SetKeyboardState(previous);
}

void EditorNotification(HWND editor, unsigned int code, Sci_Position position = 0) {
  SCNotification notification{};
  notification.nmhdr.hwndFrom = editor;
  notification.nmhdr.code = code;
  notification.position = position;
  ::SendMessageW(::GetParent(editor), WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&notification));
}

bool CaptureSqlTip(HWND editor, const char* environment) {
  if (std::getenv(environment) == nullptr) return true;
  struct Search { HWND editor; HWND popup = nullptr; } search{editor};
  ::EnumThreadWindows(::GetCurrentThreadId(), [](HWND hwnd, LPARAM parameter) -> BOOL {
    auto& search = *reinterpret_cast<Search*>(parameter);
    wchar_t name[64]{};
    ::GetClassNameW(hwnd, name, _countof(name));
    if (::IsWindowVisible(hwnd) && std::wcscmp(name, L"CallTip") == 0 &&
        ::GetWindow(hwnd, GW_OWNER) == ::GetAncestor(search.editor, GA_ROOT)) search.popup = hwnd;
    return TRUE;
  }, reinterpret_cast<LPARAM>(&search));
  return search.popup != nullptr && CaptureDialog(search.popup, environment);
}

struct PopupProbe {
  std::vector<UINT> commands;
  std::vector<UINT> states;
};

LRESULT CALLBACK InspectPopup(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                              UINT_PTR, DWORD_PTR data) {
  const auto result = ::DefSubclassProc(hwnd, message, wparam, lparam);
  if (message == WM_INITMENUPOPUP) {
    auto& probe = *reinterpret_cast<PopupProbe*>(data);
    HMENU popup = reinterpret_cast<HMENU>(wparam);
    for (int i = 0; i < ::GetMenuItemCount(popup); ++i) {
      const UINT command = ::GetMenuItemID(popup, i);
      probe.commands.push_back(command);
      probe.states.push_back(::GetMenuState(popup, command, MF_BYCOMMAND));
    }
    ::PostMessageW(hwnd, WM_CANCELMODE, 0, 0);
  }
  return result;
}

PopupProbe ProbePopup(HWND owner, HWND control) {
  PopupProbe probe;
  if (!::SetWindowSubclass(owner, InspectPopup, 1, reinterpret_cast<DWORD_PTR>(&probe))) return probe;
  ::SendMessageW(control, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(control), MAKELPARAM(-1, -1));
  ::RemoveWindowSubclass(owner, InspectPopup, 1);
  return probe;
}

}  // namespace

spec("Studio native menu") {
  static ScintillaRuntime runtime;
  static std::unique_ptr<MainWindow> window;
  static HWND editor;
  static std::unique_ptr<ResultWorkspaceView> result_fixture;
  static std::unique_ptr<SchemaExplorerView> explorer_fixture;
  static SchemaExplorerModel explorer_model;
  static SqlWorkspaceSession explorer_session;
  static std::unique_ptr<ConnectionIniFile> assistance_database;
  static BYTE keyboard_state[256];

  before_all() {
    check_true(SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES |
        ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES |
        ICC_TAB_CLASSES};
    check_true(::InitCommonControlsEx(&controls) != FALSE);
    check_true(SUCCEEDED(_Module.Init(nullptr, ::GetModuleHandleW(nullptr))));
    check_true(runtime.Initialize());
  }

  before_each() {
    check_true(::GetKeyboardState(keyboard_state) != FALSE);
    BYTE clear_state[256]{};
    check_true(::SetKeyboardState(clear_state) != FALSE);
    window = std::make_unique<MainWindow>(runtime, L"");
    RECT bounds{0, 0, 900, 600};
    check_not_null(window->Create(nullptr, bounds, L"Studio menu test",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS));
    editor = FindEditor(*window);
    check_not_null(editor);
  }

  after_each() {
    if (explorer_fixture && explorer_fixture->IsWindow()) explorer_fixture->DestroyWindow();
    explorer_fixture.reset();
    explorer_model.Clear();
    explorer_session.ClearConnection();
    if (result_fixture && result_fixture->IsWindow()) result_fixture->DestroyWindow();
    result_fixture.reset();
    if (window && window->IsWindow()) window->DestroyWindow();
    window.reset();
    assistance_database.reset();
    ::SetKeyboardState(keyboard_state);
    MSG message{};
    while (::PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
  }

  after_all() {
    _Module.Term();
    ::CoUninitialize();
  }

  it("creates a native menu bar on the real frame") {
    const HMENU menu = ::GetMenu(*window);
    check_not_null(menu);
    check_equal(::GetMenuItemCount(menu), 5);
    const wchar_t* labels[] = {L"&File", L"&Edit", L"&Query", L"&View", L"&Help"};
    for (int index = 0; index < 5; ++index) {
      wchar_t label[64]{};
      check_greater(::GetMenuStringW(menu, index, label, _countof(label),
                                    MF_BYPOSITION), 0);
      check_true(std::wstring_view(label) == labels[index]);
      check_not_null(::GetSubMenu(menu, index));
    }
  }

  group("database connection") {
    it("shows an empty recent-connections submenu and ignores absent profile commands") {
      const HMENU recent = RecentConnectionsMenu(*window);
      check_not_null(recent);
      check_equal(::GetMenuItemCount(recent), 1);
      check_true((::GetMenuState(recent, 0, MF_BYPOSITION) & MF_GRAYED) != 0);
      window->SendMessage(WM_COMMAND, kRecentConnectionFirst);
      check_false(MenuEnabled(*window, kFileDisconnect));
    }

    it("reports an invalid default INI in the recent submenu and recovers after correction") {
      ConnectionIniFile file;
      check_true(file.Write("[invalid]\n", 10));
      window->DestroyWindow();
      MSG quit{};
      while (::PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
      window = std::make_unique<MainWindow>(runtime, file.path);
      RECT bounds{0, 0, 900, 600};
      check_not_null(window->Create(nullptr, bounds, L"Studio INI error test",
          WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS));
      const HMENU recent = RecentConnectionsMenu(*window);
      check_equal(::GetMenuItemCount(recent), 1);
      check_true((::GetMenuState(recent, 0, MF_BYPOSITION) & MF_GRAYED) != 0);
      wchar_t label[128]{};
      check_greater(::GetMenuStringW(recent, 0, label, _countof(label), MF_BYPOSITION), 0);
      check_true(std::wstring_view(label).find(L"INI could not be loaded") != std::wstring_view::npos);
      window->SendMessage(WM_COMMAND, kRecentConnectionLast);
      const char* valid = "[connection.local]\ndriver=sqlite\nmodule_path=x.dll\ndatabase=:memory:\n";
      check_true(file.Write(valid, std::strlen(valid)));
      check_equal(::GetMenuItemID(RecentConnectionsMenu(*window), 0), static_cast<UINT>(kRecentConnectionFirst));
      check_true((::GetMenuState(recent, 0, MF_BYPOSITION) & MF_GRAYED) == 0);
    }

    it("loads up to 32 recent connections from the default INI before opening a dialog") {
      ConnectionIniFile file;
      std::string text = "[studio]\ndefault_connection=local31\n";
      for (int index = 0; index < 32; ++index)
        text += "[connection.local" + std::to_string(index) +
            "]\ndriver=sqlite\nmodule_path=unused.dll\ndatabase=:memory:\n";
      text += "display_name=R&D\tLocal\n";
      check_true(file.Write(text.data(), text.size()));
      window->DestroyWindow();
      MSG quit{};
      while (::PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
      window = std::make_unique<MainWindow>(runtime, file.path);
      RECT bounds{0, 0, 900, 600};
      check_not_null(window->Create(nullptr, bounds, L"Studio recent connections test",
          WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS));
      const HMENU recent = RecentConnectionsMenu(*window);
      check_not_null(recent);
      check_equal(::GetMenuItemCount(recent), 32);
      check_equal(::GetMenuItemID(recent, 0), static_cast<UINT>(kRecentConnectionLast));
      check_equal(::GetMenuItemID(recent, 1), static_cast<UINT>(kRecentConnectionFirst));
      wchar_t label[128]{};
      check_greater(::GetMenuStringW(recent, 0, label, _countof(label), MF_BYPOSITION), 0);
      check_true(std::wstring_view(label) == L"R&&D Local");
      check_true((::GetMenuState(recent, 0, MF_BYPOSITION) & MF_GRAYED) == 0);
    }

    it("connects directly from recent entries, promotes only successful profiles and blocks switching while connected") {
      ConnectionIniFile file;
      const std::string text = "[connection.first]\ndriver=sqlite\nmodule_path=" +
          std::string(TURBODB_APP_SQLITE_PLUGIN_PATH) + "\ndatabase=:memory:\ndisplay_name=First\n"
          "[connection.second]\ndriver=sqlite\nmodule_path=" +
          std::string(TURBODB_APP_SQLITE_PLUGIN_PATH) + "\ndatabase=:memory:\ndisplay_name=Second\n"
          "[connection.broken]\ndriver=sqlite\nmodule_path=missing-driver.dll\ndatabase=:memory:\n";
      check_true(file.Write(text.data(), text.size()));
      std::wstring error;
      check_true(window->LoadConnectionConfiguration(file.path, &error));
      check_equal(::GetMenuItemID(RecentConnectionsMenu(*window), 0), static_cast<UINT>(kRecentConnectionFirst));
      bool automatic = false;
      check_true(WithConnectionDialog(*window, [&](HWND dialog) {
        automatic = ::SendDlgItemMessageW(dialog, IDC_CONNECTION_PROFILE, CB_GETCURSEL, 0, 0) == 2 &&
            !::IsWindowEnabled(::GetDlgItem(dialog, IDOK)) &&
            ControlText(dialog, IDC_CONNECTION_NAME) == L"Second";
        WaitForConnection(dialog);
        if (::IsWindow(dialog)) ::SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
      }, false, kRecentConnectionFirst + 1));
      check_true(automatic);
      check_true(MenuEnabled(*window, kFileDisconnect));
      HMENU recent = RecentConnectionsMenu(*window);
      check_equal(::GetMenuItemID(recent, 0), static_cast<UINT>(kRecentConnectionFirst + 1));
      for (int index = 0; index < 3; ++index)
        check_true((::GetMenuState(recent, index, MF_BYPOSITION) & MF_GRAYED) != 0);
      // A direct WM_COMMAND cannot bypass the disabled native menu.
      window->SendMessage(WM_COMMAND, kRecentConnectionFirst);
      check_true(MenuEnabled(*window, kFileDisconnect));
      window->SendMessage(WM_COMMAND, kFileDisconnect);
      bool failed = false;
      check_true(WithConnectionDialog(*window, [&](HWND dialog) {
        failed = WaitForConnection(dialog) && ::IsWindow(dialog) &&
            !ControlText(dialog, IDC_CONNECTION_ERROR).empty() &&
            ::IsWindowEnabled(::GetDlgItem(dialog, IDC_CONNECTION_CONFIG_LOAD));
        ::SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
      }, false, kRecentConnectionFirst + 2));
      check_true(failed);
      check_false(MenuEnabled(*window, kFileDisconnect));
      check_equal(::GetMenuItemID(RecentConnectionsMenu(*window), 0), static_cast<UINT>(kRecentConnectionFirst + 1));
      check_true(WithConnectionDialog(*window, [&](HWND dialog) {
        ::SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
      }));
      check_equal(::GetMenuItemID(RecentConnectionsMenu(*window), 0), static_cast<UINT>(kRecentConnectionFirst + 1));
      check_true(file.Write("[invalid]\n", 10));
      check_false(window->LoadConnectionConfiguration(file.path, &error));
      check_equal(::GetMenuItemID(RecentConnectionsMenu(*window), 0), static_cast<UINT>(kRecentConnectionFirst + 1));
      check_true(file.Write(text.data(), text.size()));
      check_true(window->LoadConnectionConfiguration(file.path, &error));
      check_equal(::GetMenuItemID(RecentConnectionsMenu(*window), 0), static_cast<UINT>(kRecentConnectionFirst));
    }

    it("keeps recent connection password failures editable without attempting a connection") {
      TestPassword restore;
      if (restore.existed) check_true(::SetEnvironmentVariableW(TestPassword::name, nullptr) != FALSE);
      ConnectionIniFile file;
      const char* text = "[connection.network]\ndriver=postgresql\nmodule_path=missing.dll\n"
          "database=db\nuser=reader\nca_certificate=ca.pem\ntls_server_name=db.example\n"
          "password_env=TURBODB_STUDIO_CONFIG_TEST_PASSWORD\n";
      check_true(file.Write(text, std::strlen(text)));
      std::wstring error;
      check_true(window->LoadConnectionConfiguration(file.path, &error));
      bool focused = false;
      check_true(WithConnectionDialog(*window, [&](HWND dialog) {
        focused = ::GetFocus() == ::GetDlgItem(dialog, IDC_CONNECTION_PASSWORD) &&
            ::IsWindowEnabled(::GetDlgItem(dialog, IDOK)) &&
            ControlText(dialog, IDC_CONNECTION_ERROR).find(L"environment variable") != std::wstring::npos;
        ::SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
      }, false, kRecentConnectionFirst));
      check_true(focused);
      check_false(MenuEnabled(*window, kFileDisconnect));
    }

    it("renders section hint and validation surfaces without losing field focus") {
      bool sections = false, error_surface = false, error_focus = false;
      const auto colors = CurrentStudioColors();
      check_true(WithConnectionDialog(*window, [&](HWND dialog) {
        RECT client{};
        ::GetClientRect(dialog, &client);
        std::vector<std::uint32_t> pixels;
        sections = RenderToolbar(dialog, {client.right, client.bottom}, &pixels, true) &&
            (colors.system || (ColorCount(pixels, colors.blue) > 20 &&
                               ColorCount(pixels, colors.hover) > 100 &&
                               ColorCount(pixels, colors.surface) > 100));
        ::SetDlgItemTextW(dialog, IDC_CONNECTION_DATABASE, L"");
        ::SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        error_focus = ::GetFocus() == ::GetDlgItem(dialog, IDC_CONNECTION_DATABASE) &&
            ::IsWindowVisible(::GetDlgItem(dialog, IDC_CONNECTION_ERROR));
        ::GetClientRect(dialog, &client);
        error_surface = RenderToolbar(dialog, {client.right, client.bottom}, &pixels, true) &&
            (colors.system || ColorCount(pixels, colors.error) > 100);
        ::SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
      }));
      check_true(sections);
      check_true(error_surface);
      check_true(error_focus);
    }

    it("loads the default INI on the first dialog and blocks missing password environment variables") {
      TestPassword restore;
      if (restore.existed)
        check_true(::SetEnvironmentVariableW(TestPassword::name, nullptr) != FALSE);
      ConnectionIniFile file;
      const char* text =
          "[connection.reporting]\ndriver=postgresql\nmodule_path=postgres.dll\n"
          "database=warehouse\nuser=reader\nca_certificate=ca.pem\ntls_server_name=db.example\n"
          "password_env=TURBODB_STUDIO_CONFIG_TEST_PASSWORD\n";
      check_true(file.Write(text, std::strlen(text)));
      window->DestroyWindow();
      MSG quit{};
      while (::PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {}
      window = std::make_unique<MainWindow>(runtime, file.path);
      RECT bounds{0, 0, 900, 600};
      check_not_null(window->Create(nullptr, bounds, L"Studio configuration test",
          WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS));
      check_true(window->connection_configuration().profiles.empty());
      bool selected = false, blocked = false, recovered = false, captured = true;
      check_true(WithConnectionDialog(*window, [&](HWND dialog) {
        selected = ControlText(dialog, IDC_CONNECTION_DATABASE) == L"warehouse" &&
            ::GetFocus() == ::GetDlgItem(dialog, IDC_CONNECTION_PASSWORD) &&
            ControlText(dialog, IDC_CONNECTION_ERROR).find(L"environment variable") != std::wstring::npos;
        ::SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        blocked = ::IsWindow(dialog) &&
            ControlText(dialog, IDC_CONNECTION_ERROR).find(L"environment variable") != std::wstring::npos;
        captured = CaptureDialog(dialog, "TURBODB_APP_PASSWORD_ERROR_CAPTURE");
        ::SetDlgItemTextW(dialog, IDC_CONNECTION_PASSWORD, L"manual-test-password");
        recovered = ::SendDlgItemMessageW(dialog, IDC_CONNECTION_PROFILE, CB_GETCURSEL, 0, 0) == 0 &&
            !::IsWindowVisible(::GetDlgItem(dialog, IDC_CONNECTION_ERROR));
        ::SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
      }));
      check_true(selected);
      check_true(blocked);
      check_true(recovered);
      check_true(captured);
      check_equal(window->connection_configuration().profiles.size(), std::size_t{1});
      check_false(MenuEnabled(*window, kFileDisconnect));
    }

    it("loads INI presets, switches their fields and connects through the existing SQLite runtime") {
      ConnectionIniFile file;
      const std::string text =
          "[studio]\ndefault_connection=local\n[connection.local]\ndriver=sqlite\nmodule_path=" +
          std::string(TURBODB_APP_SQLITE_PLUGIN_PATH) +
          "\ndatabase=:memory:\ndisplay_name=Configured SQLite\n"
          "[connection.reporting]\ndriver=postgresql\nmodule_path=postgres.dll\n"
          "database=warehouse\nhost=db.example\nuser=reader\n"
          "ca_certificate=ca.pem\ntls_server_name=db.example\n";
      check_true(file.Write(text.data(), text.size()));
      std::wstring error;
      check_true(window->LoadConnectionConfiguration(file.path, &error));
      // An invalid replacement cannot discard the loaded profiles.
      check_true(file.Write("[invalid]\n", 10));
      check_false(window->LoadConnectionConfiguration(file.path, &error));
      bool local = false, network = false, edited = false;
      check_true(WithConnectionDialog(*window, [&](HWND dialog) {
        local = ::SendDlgItemMessageW(dialog, IDC_CONNECTION_PROFILE, CB_GETCOUNT, 0, 0) == 3 &&
            ::SendDlgItemMessageW(dialog, IDC_CONNECTION_PROFILE, CB_GETCURSEL, 0, 0) == 1 &&
            ControlText(dialog, IDC_CONNECTION_NAME) == L"Configured SQLite" &&
            !HasWindowText(dialog, L"Driver &module:") &&
            !HasWindowText(dialog, window->connection_configuration().profiles[0].module_path.c_str());
        auto select = [&](int index) {
          ::SendDlgItemMessageW(dialog, IDC_CONNECTION_PROFILE, CB_SETCURSEL, index, 0);
          ::SendMessageW(dialog, WM_COMMAND,
                         MAKEWPARAM(IDC_CONNECTION_PROFILE, CBN_SELCHANGE), 0);
        };
        select(2);
        network = ControlText(dialog, IDC_CONNECTION_DATABASE) == L"warehouse" &&
            ControlText(dialog, IDC_CONNECTION_HOST) == L"db.example" &&
            ControlText(dialog, IDC_CONNECTION_PORT) == L"5432" &&
            ::IsWindowVisible(::GetDlgItem(dialog, IDC_CONNECTION_CA));
        ::SetDlgItemTextW(dialog, IDC_CONNECTION_DATABASE, L"edited");
        edited = ::SendDlgItemMessageW(dialog, IDC_CONNECTION_PROFILE, CB_GETCURSEL, 0, 0) == 0;
        select(1);
        ::SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        if (::IsWindow(dialog)) ::SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
      }));
      check_true(local);
      check_true(network);
      check_true(edited);
      check_true(MenuEnabled(*window, kFileDisconnect));
      window->SendMessage(WM_COMMAND, kFileDisconnect);
      bool retained = false;
      check_true(WithConnectionDialog(*window, [&](HWND dialog) {
        retained = ::SendDlgItemMessageW(dialog, IDC_CONNECTION_PROFILE, CB_GETCOUNT, 0, 0) == 3 &&
            ControlText(dialog, IDC_CONNECTION_NAME) == L"Configured SQLite";
        ::SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
      }));
      check_true(retained);
    }

    it("compacts SQLite and preserves per-driver input while switching providers") {
      bool compact = false, postgres = false, mysql = false, restored = false;
      bool captured = true;
      int local_height = 0, network_height = 0;
      check_true(WithConnectionDialog(*window, [&](HWND dialog) {
        RECT local_size{};
        ::GetWindowRect(dialog, &local_size);
        local_height = local_size.bottom - local_size.top;
        compact = !::IsWindowVisible(::GetDlgItem(dialog, IDC_CONNECTION_HOST)) &&
            !::IsWindowVisible(::GetDlgItem(dialog, IDC_CONNECTION_TLS_SECTION)) &&
            !::IsWindowVisible(::GetDlgItem(dialog, IDC_CONNECTION_ERROR));
        ::SetDlgItemTextW(dialog, IDC_CONNECTION_DATABASE, L"C:/data/example.db");
        auto select = [&](int index) {
          ::SendDlgItemMessageW(dialog, IDC_CONNECTION_DRIVER, CB_SETCURSEL, index, 0);
          ::SendMessageW(dialog, WM_COMMAND,
                         MAKEWPARAM(IDC_CONNECTION_DRIVER, CBN_SELCHANGE), 0);
        };
        select(1);
        RECT network_size{};
        ::GetWindowRect(dialog, &network_size);
        network_height = network_size.bottom - network_size.top;
        postgres = network_size.bottom - network_size.top > local_size.bottom - local_size.top &&
            ::IsWindowVisible(::GetDlgItem(dialog, IDC_CONNECTION_CA)) &&
            !::IsWindowVisible(::GetDlgItem(dialog, IDC_CONNECTION_DATABASE_BROWSE)) &&
            ControlText(dialog, IDC_CONNECTION_PORT) == L"5432";
        ::SetDlgItemTextW(dialog, IDC_CONNECTION_DATABASE, L"warehouse");
        ::SetDlgItemTextW(dialog, IDC_CONNECTION_PORT, L"5544");
        ::SetDlgItemTextW(dialog, IDC_CONNECTION_PASSWORD, L"temporary-secret");
        select(2);
        captured = CaptureDialog(dialog, "TURBODB_APP_NETWORK_CAPTURE");
        mysql = ControlText(dialog, IDC_CONNECTION_PORT) == L"3306" &&
            ControlText(dialog, IDC_CONNECTION_PASSWORD).empty() &&
            !HasWindowText(dialog, DriverModulePath(window->connection_configuration(), SqlProvider::mysql).c_str());
        select(1);
        restored = !HasWindowText(dialog, DriverModulePath(window->connection_configuration(), SqlProvider::postgresql).c_str()) &&
            ControlText(dialog, IDC_CONNECTION_DATABASE) == L"warehouse" &&
            ControlText(dialog, IDC_CONNECTION_PORT) == L"5544";
        select(0);
        RECT restored_size{};
        ::GetWindowRect(dialog, &restored_size);
        restored = restored &&
            !HasWindowText(dialog, DriverModulePath(window->connection_configuration(), SqlProvider::sqlite).c_str()) &&
            ControlText(dialog, IDC_CONNECTION_DATABASE) == L"C:/data/example.db" &&
            restored_size.bottom - restored_size.top == local_size.bottom - local_size.top;
        ::SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
      }));
      check_true(compact);
      check_greater(network_height, local_height);
      check_true(postgres);
      check_true(mysql);
      check_true(restored);
      check_true(captured);
    }

    it("expands errors on submit and clears them when the invalid field is edited") {
      bool expanded = false, cleared = false, contained = false;
      bool captured = true;
      int initial_height = 0, failed_height = 0;
      check_true(WithConnectionDialog(*window, [&](HWND dialog) {
        RECT initial{};
        ::GetWindowRect(dialog, &initial);
        initial_height = initial.bottom - initial.top;
        ::SetDlgItemTextW(dialog, IDC_CONNECTION_DATABASE, L"");
        ::SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        RECT failed{};
        ::GetWindowRect(dialog, &failed);
        failed_height = failed.bottom - failed.top;
        expanded = ::IsWindowVisible(::GetDlgItem(dialog, IDC_CONNECTION_ERROR)) &&
            failed.bottom - failed.top > initial.bottom - initial.top;
        RECT client{}, button{}, message{};
        ::GetClientRect(dialog, &client);
        ::GetWindowRect(::GetDlgItem(dialog, IDOK), &button);
        ::GetWindowRect(::GetDlgItem(dialog, IDC_CONNECTION_ERROR), &message);
        ::MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&button), 2);
        ::MapWindowPoints(nullptr, dialog, reinterpret_cast<POINT*>(&message), 2);
        contained = message.bottom < button.top && button.bottom < client.bottom;
        captured = CaptureDialog(dialog, "TURBODB_APP_CONNECTION_ERROR_CAPTURE");
        ::SetDlgItemTextW(dialog, IDC_CONNECTION_DATABASE, L":memory:");
        RECT edited{};
        ::GetWindowRect(dialog, &edited);
        cleared = !::IsWindowVisible(::GetDlgItem(dialog, IDC_CONNECTION_ERROR)) &&
            ControlText(dialog, IDC_CONNECTION_ERROR).empty() &&
            edited.bottom - edited.top == initial.bottom - initial.top;
        ::SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
      }));
      check_greater(failed_height, initial_height);
      check_true(expanded);
      check_true(cleared);
      check_true(contained);
      check_true(captured);
    }

    it("opens a labeled keyboard dialog and cancels without a connection") {
      bool native_state = false, captured = true, compact_rows = false;
      check_true(MenuEnabled(*window, kFileConnect));
      check_false(MenuEnabled(*window, kFileDisconnect));
      check_true(WithConnectionDialog(*window, [&](HWND dialog) {
        native_state = !::IsWindowEnabled(*window) &&
            ::SendDlgItemMessageW(dialog, IDC_CONNECTION_DRIVER, CB_GETCOUNT, 0, 0) == 3 &&
            ControlText(dialog, IDC_CONNECTION_DATABASE) == L":memory:" &&
            !HasWindowText(dialog, L"Driver &module:") &&
            !HasWindowText(dialog, DriverModulePath(window->connection_configuration(), SqlProvider::sqlite).c_str()) &&
            !::IsWindowEnabled(::GetDlgItem(dialog, IDC_CONNECTION_HOST)) &&
            (::GetWindowLongW(::GetDlgItem(dialog, IDC_CONNECTION_PASSWORD), GWL_STYLE) & ES_PASSWORD) != 0 &&
            ::GetNextDlgTabItem(dialog, ::GetDlgItem(dialog, IDC_CONNECTION_DRIVER), FALSE) ==
                ::GetDlgItem(dialog, IDC_CONNECTION_NAME) &&
            ::GetNextDlgTabItem(dialog, ::GetDlgItem(dialog, IDC_CONNECTION_NAME), TRUE) ==
                ::GetDlgItem(dialog, IDC_CONNECTION_DRIVER);
        RECT driver{}, name{}, row{0, 0, 0, 21};
        ::GetWindowRect(::GetDlgItem(dialog, IDC_CONNECTION_DRIVER), &driver);
        ::GetWindowRect(::GetDlgItem(dialog, IDC_CONNECTION_NAME), &name);
        ::MapDialogRect(dialog, &row);
        compact_rows = std::abs(name.top - driver.top - row.bottom) <= 1;
        captured = CaptureDialog(dialog, "TURBODB_APP_CONNECTION_CAPTURE");
        ::PostMessageW(dialog, WM_KEYDOWN, VK_ESCAPE, 0);
      }, true));
      check_true(native_state);
      check_true(compact_rows);
      check_true(captured);
      check_false(MenuEnabled(*window, kFileDisconnect));
    }

    it("validates fields and keeps driver-load errors available for correction") {
      bool missing = false, failed = false;
      ConnectionIniFile file;
      // A loadable system DLL is still not an admitted database plugin.
      wchar_t system_module[MAX_PATH]{};
      const auto length = ::GetModuleFileNameW(::GetModuleHandleW(L"kernel32.dll"),
                                               system_module, _countof(system_module));
      check_true(length > 0 && length < _countof(system_module));
      const int bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
          system_module, length, nullptr, 0, nullptr, nullptr);
      check_greater(bytes, 0);
      std::string module(static_cast<std::size_t>(bytes), '\0');
      check_equal(::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
          system_module, length, module.data(), bytes, nullptr, nullptr), bytes);
      const std::string text = "[connection.invalid]\ndriver=sqlite\nmodule_path=" + module + "\ndatabase=:memory:\n"
          "[connection.valid]\ndriver=sqlite\nmodule_path=" + std::string(TURBODB_APP_SQLITE_PLUGIN_PATH) +
          "\ndatabase=:memory:\n";
      check_true(file.Write(text.data(), text.size()));
      std::wstring error;
      check_true(window->LoadConnectionConfiguration(file.path, &error));
      check_true(WithConnectionDialog(*window, [&](HWND dialog) {
        ::SetDlgItemTextW(dialog, IDC_CONNECTION_DATABASE, L"");
        ::SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        missing = ::IsWindow(dialog) &&
            ControlText(dialog, IDC_CONNECTION_ERROR).find(L"Enter a value") != std::wstring::npos &&
            ::GetFocus() == ::GetDlgItem(dialog, IDC_CONNECTION_DATABASE);
        ::SetDlgItemTextW(dialog, IDC_CONNECTION_DATABASE, L":memory:");
        ::SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        failed = WaitForConnection(dialog) && ::IsWindow(dialog) &&
            !ControlText(dialog, IDC_CONNECTION_ERROR).empty() &&
            ControlText(dialog, IDC_CONNECTION_ERROR) != L"Connecting..." &&
            ::IsWindowEnabled(::GetDlgItem(dialog, IDOK));
        ::SendDlgItemMessageW(dialog, IDC_CONNECTION_PROFILE, CB_SETCURSEL, 2, 0);
        ::SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDC_CONNECTION_PROFILE, CBN_SELCHANGE), 0);
        ::SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        if (::IsWindow(dialog)) ::SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
      }));
      check_true(missing);
      check_true(failed);
      check_false(MenuEnabled(*window, kFileConnect));
      check_true(MenuEnabled(*window, kFileDisconnect));
      window->SendMessage(WM_COMMAND, kFileDisconnect);
    }

    it("rejects a module whose canonical driver ID differs from the selection") {
      bool mismatch = false, port_rejected = false;
      ConnectionIniFile file;
      const std::string text = "[connection.mismatch]\ndriver=postgresql\nmodule_path=" +
          std::string(TURBODB_APP_SQLITE_PLUGIN_PATH) +
          "\ndatabase=example\nuser=example\nca_certificate=ca.pem\ntls_server_name=db.example\n";
      check_true(file.Write(text.data(), text.size()));
      std::wstring error;
      check_true(window->LoadConnectionConfiguration(file.path, &error));
      check_true(WithConnectionDialog(*window, [&](HWND dialog) {
        ::SetDlgItemTextW(dialog, IDC_CONNECTION_PORT, L"65536");
        ::SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        port_rejected = ::IsWindow(dialog) &&
            ControlText(dialog, IDC_CONNECTION_ERROR).find(L"65535") != std::wstring::npos &&
            ::GetFocus() == ::GetDlgItem(dialog, IDC_CONNECTION_PORT);
        ::SetDlgItemTextW(dialog, IDC_CONNECTION_PORT, L"5432");
        ::SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        mismatch = WaitForConnection(dialog) && ::IsWindow(dialog) &&
            ControlText(dialog, IDC_CONNECTION_ERROR).find(L"Plugin ID") != std::wstring::npos;
        ::SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
      }));
      check_true(port_rejected);
      check_true(mismatch);
      check_false(MenuEnabled(*window, kFileDisconnect));
    }

    it("loads SQLite from the menu, executes SQL and disconnects after consumption") {
      check_true(WithConnectionDialog(*window, [&](HWND dialog) {
        ::SetDlgItemTextW(dialog, IDC_CONNECTION_NAME, L"Temporary database");
        ::SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        // Ensure a failing connection cannot leave this test in a modal loop.
        if (::IsWindow(dialog)) ::SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
      }));
      check_true(MenuEnabled(*window, kFileDisconnect));
      check_true(HasWindowText(*window,
          L"Write SQL in the editor, then choose Execute (F5 or Ctrl+Enter)."));
      HWND explorer = FindExplorer(editor);
      check_not_null(explorer);
      const HWND empty = ::FindWindowExW(explorer, nullptr, L"Edit", nullptr);
      check_not_null(empty);
      check_false((::GetWindowLongW(empty, GWL_STYLE) & WS_VISIBLE) != 0);
      check_true((::GetWindowLongW(::GetDlgItem(explorer, SchemaExplorerView::kTreeControlId),
                                  GWL_STYLE) & WS_VISIBLE) != 0);
      check_false(MenuEnabled(*window, kFileConnect));
      ConnectionProfile another;
      another.driver_id = "sqlite";
      another.module_path = TURBODB_APP_SQLITE_PLUGIN_PATH;
      another.options.push_back({"filename", ":memory:"});
      std::string error;
      check_false(window->OpenConnection(another, &error));
      ::SendMessageW(editor, SCI_SETTEXT, 0,
                     reinterpret_cast<LPARAM>("select 42 as answer"));
      window->SendMessage(WM_COMMAND, kQueryExecute);
      check_false(MenuEnabled(*window, kFileDisconnect));
      check_false(window->CloseActiveConnection(&error));
      const DWORD started = ::GetTickCount();
      while (!MenuEnabled(*window, kFileDisconnect) &&
             ::GetTickCount() - started < 5000u) {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
          ::TranslateMessage(&message);
          ::DispatchMessageW(&message);
        }
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
      }
      check_true(MenuEnabled(*window, kFileDisconnect));
      // The result host is the editor's sibling, rather than its parent.
      HWND result_host = ::FindWindowExW(::GetParent(editor), nullptr,
                                        L"TurboDBStudioResultWorkspace", nullptr);
      check_not_null(result_host);
      CListViewCtrl grid(::GetDlgItem(result_host, 4201));
      check_equal(grid.GetItemCount(), 1);
      wchar_t value[32]{};
      check_greater(grid.GetItemText(0, 0, value, _countof(value)), 0);
      check_true(std::wstring_view(value) == L"42");
      BYTE control_shift[256]{};
      control_shift[VK_CONTROL] = 0x80;
      control_shift[VK_SHIFT] = 0x80;
      ::SetKeyboardState(control_shift);
      MSG disconnect{};
      disconnect.hwnd = editor;
      disconnect.message = WM_KEYDOWN;
      disconnect.wParam = 'D';
      check_true(window->PreTranslateMessage(&disconnect) != FALSE);
      check_true(MenuEnabled(*window, kFileConnect));
      check_false(MenuEnabled(*window, kFileDisconnect));
      check_equal(grid.GetItemCount(), 0);
      check_true(HasWindowText(*window, L"Open a connection: File > New Connection (Ctrl+N)."));
      check_true((::GetWindowLongW(empty, GWL_STYLE) & WS_VISIBLE) != 0);
    }

    it("keeps the dialog responsive and protects the pending connection from duplicate submit and close") {
      bool pending = false, protected_owner = false, painted = false;
      check_true(WithConnectionDialog(*window, [&](HWND dialog) {
        ::SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        pending = ::IsWindow(dialog) &&
            !::IsWindowEnabled(::GetDlgItem(dialog, IDOK)) &&
            !::IsWindowEnabled(::GetDlgItem(dialog, IDC_CONNECTION_DRIVER)) &&
            ControlText(dialog, IDC_CONNECTION_HINT).find(L"cannot be cancelled") != std::wstring::npos;
        ::SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        ::SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
        ::SendMessageW(dialog, WM_CLOSE, 0, 0);
        window->SendMessage(WM_CLOSE);
        std::string error;
        protected_owner = ::IsWindow(dialog) && window->IsWindow() &&
            !MenuEnabled(*window, kFileConnect) && !MenuEnabled(*window, kQueryExecute) &&
            !window->CloseActiveConnection(&error);
        // Painting is serviced before consuming the completion, even if the
        // real SQLite worker has already finished. The old blocking path closes
        // the dialog before this UI turn can run.
        painted = CaptureDialog(dialog, "TURBODB_APP_CONNECTION_PENDING_CAPTURE");
        ::InvalidateRect(dialog, nullptr, TRUE);
        painted = ::UpdateWindow(dialog) != FALSE && painted;
      }));
      check_true(pending);
      check_true(protected_owner);
      check_true(painted);
      check_true(MenuEnabled(*window, kFileDisconnect));
      window->SendMessage(WM_COMMAND, kFileDisconnect);
    }

    it("navigates from the plan tree to selectable raw details and back using the keyboard") {
      check_true(WithConnectionDialog(*window, [&](HWND dialog) {
        ::SendMessageW(dialog, WM_COMMAND, IDOK, 0);
      }));
      ::SendMessageW(editor, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>("select 42"));
      window->SendMessage(WM_COMMAND, kQueryPlan);
      check_true(WaitForQuery(*window));
      HWND host = ::FindWindowExW(::GetParent(editor), nullptr,
                                  L"TurboDBStudioResultWorkspace", nullptr);
      const HWND tree = ::GetDlgItem(host, 4204);
      const HWND raw = ::GetDlgItem(host, 4205);
      check_greater(::GetWindowTextLengthW(raw), 0);
      window->ShowWindow(SW_SHOWNOACTIVATE);
      window->SendMessage(WM_COMMAND, kViewResults);
      check_false(::IsWindowVisible(host) != FALSE);
      window->SendMessage(WM_COMMAND, kViewResults);
      check_equal(::GetFocus(), tree);
      MSG key{};
      key.hwnd = tree; key.message = WM_KEYDOWN; key.wParam = VK_TAB;
      check_true(window->PreTranslateMessage(&key) != FALSE);
      check_equal(::GetFocus(), raw);
      BYTE state[256]{};
      state[VK_CONTROL] = 0x80;
      ::SetKeyboardState(state);
      key.hwnd = raw; key.wParam = 'A';
      check_true(window->PreTranslateMessage(&key) != FALSE);
      DWORD start = 0, end = 0;
      ::SendMessageW(raw, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
      check_equal(start, 0u);
      check_equal(end, static_cast<DWORD>(::GetWindowTextLengthW(raw)));
      state[VK_CONTROL] = 0; state[VK_SHIFT] = 0x80;
      ::SetKeyboardState(state);
      key.wParam = VK_TAB;
      check_true(window->PreTranslateMessage(&key) != FALSE);
      check_equal(::GetFocus(), tree);
      state[VK_CONTROL] = 0x80;
      ::SetKeyboardState(state);
      key.hwnd = editor; key.wParam = 'P';
      check_true(window->PreTranslateMessage(&key) != FALSE);
      check_equal(::GetFocus(), raw);
      state[VK_CONTROL] = 0; state[VK_SHIFT] = 0;
      ::SetKeyboardState(state);
      key.hwnd = raw; key.wParam = VK_TAB;
      check_true(window->PreTranslateMessage(&key) != FALSE);
      check_equal(::GetFocus(), editor);
    }

    it("drains an unpublished connection before destroying the frame's resource owners") {
      bool pending = false;
      check_true(WithConnectionDialog(*window, [&](HWND dialog) {
        ::SendMessageW(dialog, WM_COMMAND, IDOK, 0);
        pending = !::IsWindowEnabled(::GetDlgItem(dialog, IDOK));
        // Forced native teardown bypasses WM_CLOSE (normal close is guarded).
        // The future still owns the unpublished result and must drain before
        // the connection manager and plugin runtime are destroyed.
        window->DestroyWindow();
      }));
      check_true(pending);
      window.reset();
    }
  }

  group("result presentation") {
    before_each() {
      result_fixture = std::make_unique<ResultWorkspaceView>();
      RECT bounds{0, 0, 600, 300};
      check_not_null(result_fixture->Create(*window, bounds, nullptr, WS_CHILD | WS_VISIBLE));
    }

    it("preserves adjacent and extreme double values in the actual grid text") {
      const double values[] = {1.0, std::nextafter(1.0, 2.0), 0.1,
          std::numeric_limits<double>::max(), std::numeric_limits<double>::min(),
          std::numeric_limits<double>::denorm_min(), -0.0};
      QueryResultSnapshot result;
      result.kind = QueryOutcomeKind::rows;
      result.columns = {"value"};
      for (const double value : values) {
        QueryCell cell;
        cell.kind = ORM_VALUE_DOUBLE;
        cell.double_value = value;
        result.rows.push_back({cell});
      }
      result_fixture->Render(result);
      CListViewCtrl grid(result_fixture->GetDlgItem(4201));
      check_equal(grid.GetItemCount(), static_cast<int>(_countof(values)));
      for (int i = 0; i < static_cast<int>(_countof(values)); ++i) {
        wchar_t text[128]{};
        check_greater(grid.GetItemText(i, 0, text, _countof(text)), 0);
        wchar_t* end = nullptr;
        const double parsed = std::wcstod(text, &end);
        check_true(end != text && *end == L'\0');
        check_true(parsed == values[i]);
        check_equal(std::signbit(parsed), std::signbit(values[i]));
      }
    }

    it("explains empty and running states without internal worker terminology") {
      check_true(HasWindowText(*result_fixture,
          L"Open a connection: File > New Connection (Ctrl+N)."));
      result_fixture->Clear(true);
      check_true(HasWindowText(*result_fixture,
          L"Write SQL in the editor, then choose Execute (F5 or Ctrl+Enter)."));
      result_fixture->SetRunning();
      check_true(HasWindowText(*result_fixture, L"Running query...\r\nThis query cannot be cancelled."));
      result_fixture->SetExplainRunning(ORM_EXPLAIN_PLAN);
      check_true(HasWindowText(*result_fixture, L"Loading execution plan..."));
      result_fixture->SetExplainRunning(ORM_EXPLAIN_ANALYZE);
      check_true(HasWindowText(*result_fixture,
          L"Running ANALYZE...\r\nANALYZE may execute the SQL statement and cannot be cancelled."));
    }
  }

  it("routes select all, undo and redo to the SQL editor") {
    ::SendMessageW(editor, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>("select 1"));
    ::SendMessageW(editor, SCI_EMPTYUNDOBUFFER, 0, 0);
    window->SendMessage(WM_COMMAND, kEditSelectAll);
    check_equal(::SendMessageW(editor, SCI_GETSELECTIONSTART, 0, 0), 0);
    check_equal(::SendMessageW(editor, SCI_GETSELECTIONEND, 0, 0), 8);
    ::SendMessageW(editor, SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>("select 2"));
    window->SendMessage(WM_COMMAND, kEditUndo);
    char text[32]{};
    ::SendMessageW(editor, SCI_GETTEXT, sizeof(text), reinterpret_cast<LPARAM>(text));
    check_equal(text, "select 1");
    window->SendMessage(WM_COMMAND, kEditRedo);
    ::SendMessageW(editor, SCI_GETTEXT, sizeof(text), reinterpret_cast<LPARAM>(text));
    check_equal(text, "select 2");
  }

  it("disables unavailable edit actions and updates after selection") {
    HMENU edit = ::GetSubMenu(::GetMenu(*window), 1);
    window->SendMessage(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(edit));
    check_true((::GetMenuState(edit, kEditUndo, MF_BYCOMMAND) & MF_GRAYED) != 0);
    check_true((::GetMenuState(edit, kEditRedo, MF_BYCOMMAND) & MF_GRAYED) != 0);
    check_true((::GetMenuState(edit, kEditCut, MF_BYCOMMAND) & MF_GRAYED) != 0);
    check_true((::GetMenuState(edit, kEditCopy, MF_BYCOMMAND) & MF_GRAYED) != 0);
    ::SendMessageW(editor, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>("select 1"));
    window->SendMessage(WM_COMMAND, kEditSelectAll);
    window->SendMessage(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(edit));
    check_equal(::GetMenuState(edit, kEditCopy, MF_BYCOMMAND) & MF_GRAYED, 0u);
    check_equal(::GetMenuState(edit, kEditCut, MF_BYCOMMAND) & MF_GRAYED, 0u);
  }

  it("routes query menu actions through existing connection validation") {
    window->SendMessage(WM_COMMAND, kQueryExecute);
    check_true(HasWindowText(*window, L"Open a TurboDB connection before executing SQL."));
    window->SendMessage(WM_COMMAND, kQueryPlan);
    check_true(HasWindowText(*window, L"Open a TurboDB connection before explaining SQL."));
  }

  it("translates execution shortcuts from a workspace pane") {
    MSG message{};
    message.hwnd = ::FindWindowExW(*window, nullptr, nullptr, nullptr);
    check_not_null(message.hwnd);
    message.message = WM_KEYDOWN;
    message.wParam = VK_F5;
    check_true(window->PreTranslateMessage(&message) != FALSE);
    check_true(HasWindowText(*window, L"Open a TurboDB connection before executing SQL."));
    message.wParam = VK_F6;
    check_true(window->PreTranslateMessage(&message) != FALSE);
    check_true(HasWindowText(*window, L"Open a TurboDB connection before explaining SQL."));
    BYTE control_state[256]{};
    control_state[VK_CONTROL] = 0x80;
    check_true(::SetKeyboardState(control_state) != FALSE);
    message.wParam = VK_RETURN;
    check_true(window->PreTranslateMessage(&message) != FALSE);
    check_true(HasWindowText(*window, L"Open a TurboDB connection before executing SQL."));
  }

  it("leaves shortcuts belonging to other windows unhandled") {
    MSG message{};
    message.message = WM_KEYDOWN;
    message.wParam = VK_F5;
    check_false(window->PreTranslateMessage(&message) != FALSE);
  }

  group("query tabs and object operations") {
    it("navigates query tabs and actions without taking the editor outdent key") {
      window->ShowWindow(SW_SHOWNOACTIVATE);
      const HWND notebook = ::GetParent(::GetParent(editor));
      CTabCtrl tabs(::GetDlgItem(notebook, QueryNotebook::kTabsId));
      const HWND add = ::GetDlgItem(notebook, kQueryNew);
      const HWND close = ::GetDlgItem(notebook, kQueryClose);
      window->SendMessage(WM_COMMAND, kQueryNew);
      BYTE keys[256]{};
      keys[VK_CONTROL] = keys[VK_SHIFT] = 0x80;
      check_true(::SetKeyboardState(keys) != FALSE);
      MSG key{}; key.hwnd = FindEditor(*window); key.message = WM_KEYDOWN; key.wParam = 'Q';
      check_true(window->PreTranslateMessage(&key) != FALSE);
      check_equal(::GetFocus(), tabs.m_hWnd);
      keys[VK_CONTROL] = keys[VK_SHIFT] = 0;
      check_true(::SetKeyboardState(keys) != FALSE);
      tabs.SendMessage(WM_KEYDOWN, VK_LEFT);
      check_equal(tabs.GetCurSel(), 0);
      check_equal(FindEditor(*window), editor);
      key.hwnd = tabs; key.wParam = VK_TAB;
      check_true(window->PreTranslateMessage(&key) != FALSE);
      check_equal(::GetFocus(), editor);
      keys[VK_SHIFT] = 0x80;
      check_true(::SetKeyboardState(keys) != FALSE);
      key.hwnd = editor;
      check_false(window->PreTranslateMessage(&key) != FALSE);
      window->SendMessage(WM_COMMAND, kViewQueryTabs);
      key.hwnd = tabs;
      check_true(window->PreTranslateMessage(&key) != FALSE);
      check_equal(::GetFocus(), close);
      key.hwnd = close;
      check_true(window->PreTranslateMessage(&key) != FALSE);
      check_equal(::GetFocus(), add);
      key.hwnd = add;
      check_true(window->PreTranslateMessage(&key) != FALSE);
      check_equal(::GetFocus(), tabs.m_hWnd);
    }

    it("retains separate SQL undo and caret state while cycling native query tabs") {
      HWND notebook = ::GetParent(::GetParent(editor));
      CTabCtrl tabs(::GetDlgItem(notebook, QueryNotebook::kTabsId));
      check_equal(tabs.GetItemCount(), 1);
      SetSql(editor, "select 1");
      ::SendMessageW(editor, SCI_EMPTYUNDOBUFFER, 0, 0);
      ::SendMessageW(editor, SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>("23"));
      ::SendMessageW(editor, SCI_SETSEL, 2, 4);
      const HWND original = editor;
      window->SendMessage(WM_COMMAND, kQueryNew);
      HWND second = FindEditor(*window);
      check_not_equal(second, original);
      check_equal(tabs.GetItemCount(), 2);
      SetSql(second, "select 2");
      window->SendMessage(WM_COMMAND, kQueryPrevious);
      check_equal(FindEditor(*window), original);
      check_equal(::SendMessageW(original, SCI_GETSELECTIONSTART, 0, 0), 2);
      check_equal(::SendMessageW(original, SCI_GETSELECTIONEND, 0, 0), 4);
      window->SendMessage(WM_COMMAND, kEditUndo);
      char text[64]{};
      ::SendMessageW(original, SCI_GETTEXT, sizeof(text), reinterpret_cast<LPARAM>(text));
      check_equal(text, "select 1");
      window->SendMessage(WM_COMMAND, kQueryNext);
      ::SendMessageW(second, SCI_GETTEXT, sizeof(text), reinterpret_cast<LPARAM>(text));
      check_equal(text, "select 2");
      window->ShowWindow(SW_SHOWNOACTIVATE);
      window->RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
      check_true(CaptureDialog(*window, "TURBODB_APP_TABS_CAPTURE"));
    }

    it("bounds query pages and exposes native keyboard context menus") {
      HWND notebook = ::GetParent(::GetParent(editor));
      CTabCtrl tabs(::GetDlgItem(notebook, QueryNotebook::kTabsId));
      window->ShowWindow(SW_SHOWNOACTIVATE);
      for (std::size_t i = 1; i < QueryNotebook::kMaxPages; ++i)
        window->SendMessage(WM_COMMAND, kQueryNew);
      check_equal(tabs.GetItemCount(), static_cast<int>(QueryNotebook::kMaxPages));
      check_false(MenuEnabled(*window, kQueryNew));
      check_false(::IsWindowEnabled(::GetDlgItem(notebook, kQueryNew)) != 0);
      BYTE keys[256]{}; keys[VK_SHIFT] = 0x80;
      check_true(::SetKeyboardState(keys) != FALSE);
      MSG key{}; key.hwnd = ::GetDlgItem(notebook, kQueryClose);
      key.message = WM_KEYDOWN; key.wParam = VK_TAB;
      ::SetFocus(key.hwnd);
      check_true(window->PreTranslateMessage(&key) != FALSE);
      check_equal(::GetFocus(), tabs.m_hWnd);
      keys[VK_SHIFT] = 0;
      check_true(::SetKeyboardState(keys) != FALSE);
      window->SendMessage(WM_COMMAND, kQueryNew);
      check_equal(tabs.GetItemCount(), static_cast<int>(QueryNotebook::kMaxPages));
      auto tab_menu = ProbePopup(*window, tabs);
      check_true(std::find(tab_menu.commands.begin(), tab_menu.commands.end(), kQueryClose) != tab_menu.commands.end());
      window->SendMessage(WM_COMMAND, kQueryClose);
      check_equal(tabs.GetItemCount(), static_cast<int>(QueryNotebook::kMaxPages - 1));
      check_true(MenuEnabled(*window, kQueryNew));
      auto edit_menu = ProbePopup(*window, FindEditor(*window));
      check_true(std::find(edit_menu.commands.begin(), edit_menu.commands.end(), kQueryExecute) != edit_menu.commands.end());
      check_true(std::find(edit_menu.commands.begin(), edit_menu.commands.end(), kEditCopy) != edit_menu.commands.end());
    }

    it("confirms discard and keeps an empty query after closing the final page") {
      HWND notebook = ::GetParent(::GetParent(editor));
      CTabCtrl tabs(::GetDlgItem(notebook, QueryNotebook::kTabsId));
      SetSql(editor, "select 'keep me'");
      check_true(WithDiscardDialog(*window, kQueryClose, false));
      check_equal(tabs.GetItemCount(), 1);
      check_equal(FindEditor(*window), editor);
      check_greater(::SendMessageW(editor, SCI_GETLENGTH, 0, 0), 0);
      check_true(WithDiscardDialog(*window, kQueryClose, true));
      check_equal(tabs.GetItemCount(), 1);
      check_equal(::SendMessageW(FindEditor(*window), SCI_GETLENGTH, 0, 0), 0);
    }

    it("delivers pending results to the originating tab and blocks close until consumption") {
      ConnectionProfile profile;
      profile.driver_id = "sqlite"; profile.module_path = TURBODB_APP_SQLITE_PLUGIN_PATH;
      profile.options.push_back({"filename", ":memory:"});
      std::string error;
      check_true(window->OpenConnection(profile, &error));
      const HWND original = editor;
      const HWND first_results = ::FindWindowExW(::GetParent(editor), nullptr, L"TurboDBStudioResultWorkspace", nullptr);
      SetSql(editor, "SELECT 42 AS answer");
      window->SendMessage(WM_COMMAND, kQueryExecute);
      window->SendMessage(WM_COMMAND, kQueryNew);
      const HWND second = FindEditor(*window);
      check_not_equal(second, original);
      HWND notebook = ::GetParent(::GetParent(second));
      CTabCtrl tabs(::GetDlgItem(notebook, QueryNotebook::kTabsId));
      check_false(MenuEnabled(*window, kQueryClose));
      window->ShowWindow(SW_SHOWNOACTIVATE);
      MSG key{}; key.hwnd = ::GetDlgItem(notebook, kQueryNew);
      key.message = WM_KEYDOWN; key.wParam = VK_TAB;
      ::SetFocus(key.hwnd);
      check_true(window->PreTranslateMessage(&key) != FALSE);
      check_equal(::GetFocus(), tabs.m_hWnd);
      window->SendMessage(WM_COMMAND, kQueryClose);
      check_equal(tabs.GetItemCount(), 2);
      check_true(WaitForQuery(*window));
      check_equal(FindEditor(*window), second);
      CListViewCtrl first_grid(::GetDlgItem(first_results, 4201));
      check_equal(first_grid.GetItemCount(), 1);
      const HWND second_results = ::FindWindowExW(::GetParent(second), nullptr, L"TurboDBStudioResultWorkspace", nullptr);
      CListViewCtrl second_grid(::GetDlgItem(second_results, 4201));
      check_equal(second_grid.GetItemCount(), 0);
      check_true(MenuEnabled(*window, kQueryClose));
      window->SendMessage(WM_COMMAND, kQueryClose);
      check_equal(tabs.GetItemCount(), 1);
      check_equal(FindEditor(*window), original);
    }

    it("refreshes real schema and opens a quoted table in a new query without replacing SQL") {
      ConnectionProfile profile;
      profile.driver_id = "sqlite"; profile.module_path = TURBODB_APP_SQLITE_PLUGIN_PATH;
      profile.options.push_back({"filename", ":memory:"});
      std::string error;
      check_true(window->OpenConnection(profile, &error));
      SetSql(editor, "CREATE TABLE \"odd\"\"name\" (id INTEGER)");
      window->SendMessage(WM_COMMAND, kQueryExecute);
      check_true(WaitForQuery(*window));
      window->SendMessage(WM_COMMAND, kExplorerRefresh);
      check_false(MenuEnabled(*window, kFileDisconnect));
      check_true(WaitForQuery(*window));
      CTreeViewCtrl tree(::GetDlgItem(FindExplorer(editor), SchemaExplorerView::kTreeControlId));
      HTREEITEM table = nullptr;
      std::vector<HTREEITEM> todo{tree.GetRootItem()};
      while (!todo.empty()) {
        const HTREEITEM item = todo.back(); todo.pop_back();
        if (!item) continue;
        wchar_t name[128]{}; tree.GetItemText(item, name, _countof(name));
        if (std::wstring_view(name) == L"odd\"name") { table = item; break; }
        todo.push_back(tree.GetChildItem(item)); todo.push_back(tree.GetNextSiblingItem(item));
      }
      check_not_null(table);
      tree.SelectItem(table);
      window->ShowWindow(SW_SHOWNOACTIVATE);
      tree.EnsureVisible(table);
      auto popup = ProbePopup(*window, tree);
      check_true(std::find(popup.commands.begin(), popup.commands.end(), kExplorerOpen) != popup.commands.end());
      check_true(std::find(popup.commands.begin(), popup.commands.end(), kExplorerRefresh) != popup.commands.end());
      const HWND original = editor;
      MSG enter{}; enter.hwnd = tree; enter.message = WM_KEYDOWN; enter.wParam = VK_RETURN;
      check_true(window->PreTranslateMessage(&enter) != FALSE);
      HWND opened = FindEditor(*window);
      check_not_equal(opened, original);
      char sql[128]{};
      ::SendMessageW(opened, SCI_GETTEXT, sizeof(sql), reinterpret_cast<LPARAM>(sql));
      check_equal(sql, "SELECT * FROM \"main\".\"odd\"\"name\" LIMIT 100;");
      check_true(WaitForQuery(*window));
      HWND host = ::FindWindowExW(::GetParent(opened), nullptr, L"TurboDBStudioResultWorkspace", nullptr);
      CListViewCtrl grid(::GetDlgItem(host, 4201));
      check_equal(grid.GetHeader().GetItemCount(), 1);
      ::SendMessageW(original, SCI_GETTEXT, sizeof(sql), reinterpret_cast<LPARAM>(sql));
      check_contains(sql, "CREATE TABLE");
    }

    it("keeps result and plan pages selectable and builds untruncated Unicode copy text") {
      result_fixture = std::make_unique<ResultWorkspaceView>();
      RECT bounds{0, 0, 700, 300};
      check_not_null(result_fixture->Create(*window, bounds, nullptr, WS_CHILD | WS_VISIBLE));
      QueryResultSnapshot result;
      result.kind = QueryOutcomeKind::rows; result.columns = {"id", "text"};
      QueryCell id; id.kind = ORM_VALUE_INT64; id.int64_value = 7;
      QueryCell value; value.kind = ORM_VALUE_TEXT; value.bytes = std::string(1024, 'x') + "\xe4\xb8\xad\xe6\x96\x87";
      result.rows = {{id, value}, {id, value}};
      result_fixture->Render(result);
      CListViewCtrl grid(result_fixture->GetDlgItem(4201));
      grid.SetItemState(0, LVIS_SELECTED, LVIS_SELECTED);
      window->ShowWindow(SW_SHOWNOACTIVATE);
      auto popup = ProbePopup(*result_fixture, grid);
      check_true(std::find(popup.commands.begin(), popup.commands.end(), ResultWorkspaceView::kCopyCell) != popup.commands.end());
      std::wstring text;
      check_true(result_fixture->GridText(ResultWorkspaceView::kCopyRow, text));
      const std::wstring row = L"7\t" + std::wstring(1024, L'x') + L"\x4e2d\x6587";
      check_true(text == row);
      check_true(result_fixture->GridText(ResultWorkspaceView::kCopyAll, text));
      check_true(text == row + L"\r\n" + row);
      grid.SetItemState(-1, 0, LVIS_SELECTED);
      check_false(result_fixture->GridText(ResultWorkspaceView::kCopyRow, text));
      ExecutionPlanSnapshot plan;
      plan.status = ORM_STATUS_OK; plan.raw_detail = "SCAN items";
      result_fixture->RenderPlan(plan);
      CTabCtrl tabs(result_fixture->GetDlgItem(ResultWorkspaceView::kTabsId));
      check_equal(tabs.GetItemCount(), 3);
      check_equal(tabs.GetCurSel(), 2);
      tabs.SetCurSel(0);
      NMHDR changed{tabs, ResultWorkspaceView::kTabsId, TCN_SELCHANGE};
      result_fixture->SendMessage(WM_NOTIFY, ResultWorkspaceView::kTabsId, reinterpret_cast<LPARAM>(&changed));
      check_true((grid.GetStyle() & WS_VISIBLE) != 0);
      check_equal(grid.GetItemCount(), 2);
      check_true(result_fixture->HasPlanDetails());
      result_fixture->FocusPlanDetails();
      check_equal(tabs.GetCurSel(), 2);
    }
  }

  group("SQL assistance") {
    it("automatically completes typed prefixes while leaving strings comments and numbers alone") {
      window->ShowWindow(SW_SHOWNOACTIVATE);
      ::SetFocus(editor);
      TypeSql(editor, "se");
      check_true(::SendMessageW(editor, SCI_AUTOCACTIVE, 0, 0) != 0);
      ::SendMessageW(editor, SCI_AUTOCSELECT, 0, reinterpret_cast<LPARAM>("select"));
      ::SendMessageW(editor, WM_KEYDOWN, VK_TAB, 0);
      char text[64]{};
      ::SendMessageW(editor, SCI_GETTEXT, sizeof(text), reinterpret_cast<LPARAM>(text));
      check_equal(text, "select");
      check_false(::SendMessageW(editor, SCI_AUTOCACTIVE, 0, 0) != 0);
      SetSql(editor, "selec");
      ::SendMessageW(editor, SCI_SETSEL, 2, 2);
      RequestSqlHelp(editor, false);
      ::SendMessageW(editor, SCI_AUTOCSELECT, 0, reinterpret_cast<LPARAM>("select"));
      ::SendMessageW(editor, WM_KEYDOWN, VK_TAB, 0);
      ::SendMessageW(editor, SCI_GETTEXT, sizeof(text), reinterpret_cast<LPARAM>(text));
      check_equal(text, "select");
      for (const char* prefix : {"'", "-- ", "/* ", "\""}) {
        SetSql(editor, prefix);
        TypeSql(editor, "sel");
        check_false(::SendMessageW(editor, SCI_AUTOCACTIVE, 0, 0) != 0);
        RequestSqlHelp(editor, false);
        check_false(::SendMessageW(editor, SCI_AUTOCACTIVE, 0, 0) != 0);
      }
      SetSql(editor, "");
      TypeSql(editor, "123");
      check_false(::SendMessageW(editor, SCI_AUTOCACTIVE, 0, 0) != 0);
      SetSql(editor, "");
      RequestSqlHelp(editor, false);
      check_true(::SendMessageW(editor, SCI_AUTOCACTIVE, 0, 0) != 0);
      ::SendMessageW(editor, WM_KEYDOWN, VK_ESCAPE, 0);
      check_false(::SendMessageW(editor, SCI_AUTOCACTIVE, 0, 0) != 0);
      check_equal(::SendMessageW(editor, SCI_GETLENGTH, 0, 0), 0);
    }

    it("shows nested function parameters and dismisses stale tips without editing text") {
      window->ShowWindow(SW_SHOWNOACTIVATE);
      ::SetFocus(editor);
      TypeSql(editor, "replace(");
      check_true(::SendMessageW(editor, SCI_CALLTIPACTIVE, 0, 0) != 0);
      // Scintilla reports the caret when the tip was created, not its visual anchor.
      check_equal(::SendMessageW(editor, SCI_CALLTIPPOSSTART, 0, 0), 8);
      TypeSql(editor, "'a,b', lower(");
      check_true(::SendMessageW(editor, SCI_CALLTIPACTIVE, 0, 0) != 0);
      check_equal(::SendMessageW(editor, SCI_CALLTIPPOSSTART, 0, 0), 21);
      TypeSql(editor, "'X'), ");
      check_true(::SendMessageW(editor, SCI_CALLTIPACTIVE, 0, 0) != 0);
      check_equal(::SendMessageW(editor, SCI_CALLTIPPOSSTART, 0, 0), 27);
      check_true(CaptureSqlTip(editor, "TURBODB_APP_PARAMETER_TIP_CAPTURE"));
      TypeSql(editor, "'y')");
      check_false(::SendMessageW(editor, SCI_CALLTIPACTIVE, 0, 0) != 0);
      for (const char* sql : {"-- lower(", "'lower(", "custom_function(", "app.lower("}) {
        SetSql(editor, sql);
        RequestSqlHelp(editor, true);
        check_false(::SendMessageW(editor, SCI_CALLTIPACTIVE, 0, 0) != 0);
      }
      SetSql(editor, "coalesce(");
      RequestSqlHelp(editor, true);
      check_true(::SendMessageW(editor, SCI_CALLTIPACTIVE, 0, 0) != 0);
      const auto length = ::SendMessageW(editor, SCI_GETLENGTH, 0, 0);
      ::SendMessageW(editor, WM_KEYDOWN, VK_ESCAPE, 0);
      check_false(::SendMessageW(editor, SCI_CALLTIPACTIVE, 0, 0) != 0);
      check_equal(::SendMessageW(editor, SCI_GETLENGTH, 0, 0), length);
      RequestSqlHelp(editor, true);
      ::SendMessageW(editor, WM_KEYDOWN, VK_BACK, 0);
      check_false(::SendMessageW(editor, SCI_CALLTIPACTIVE, 0, 0) != 0);
      SetSql(editor, "coalesce(");
      RequestSqlHelp(editor, true);
      ::SendMessageW(editor, SCI_SETSEL, 0, 0);
      EditorNotification(editor, SCN_UPDATEUI);
      check_false(::SendMessageW(editor, SCI_CALLTIPACTIVE, 0, 0) != 0);
      SetSql(editor, "coalesce(");
      RequestSqlHelp(editor, true);
      SCNotification scroll{};
      scroll.nmhdr.hwndFrom = editor;
      scroll.nmhdr.code = SCN_UPDATEUI;
      scroll.updated = SC_UPDATE_V_SCROLL;
      ::SendMessageW(::GetParent(editor), WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&scroll));
      check_false(::SendMessageW(editor, SCI_CALLTIPACTIVE, 0, 0) != 0);
      RequestSqlHelp(editor, true);
      window->SendMessage(WM_COMMAND, kViewToolbar);
      check_false(::SendMessageW(editor, SCI_CALLTIPACTIVE, 0, 0) != 0);
    }

    it("describes loaded metadata without inventing aliases types or unknown functions") {
      SqlLanguageService language;
      SqlWorkspaceSession session;
      session.ReplaceRelations({{RelationKind::table, "main", "items", {"id", "name"}},
                                {RelationKind::view, "main", "active_items", {"id"}}});
      check_equal(language.DescribeSymbol(session, "items.name"), std::string("Column: main.items.name"));
      check_equal(language.DescribeSymbol(session, "MAIN.ITEMS"), std::string("Table: main.items"));
      check_equal(language.DescribeSymbol(session, "active_items"), std::string("View: main.active_items"));
      check_equal(language.DescribeSymbol(session, "id"),
                  std::string("Column: main.items.id\nColumn: main.active_items.id"));
      check_true(language.DescribeSymbol(session, "alias.name").empty());
      check_true(language.FunctionSignature(SqlProvider::sqlite, "custom_function").empty());
      check_equal(language.FunctionSignature(SqlProvider::postgresql, "LOWER"), std::string("lower(text)"));
      session.ClearConnection();
      check_true(language.DescribeSymbol(session, "items.name").empty());
    }

    it("shows hover and keyboard field help from a real SQLite schema and cancels on disconnect") {
      assistance_database = std::make_unique<ConnectionIniFile>();
      check_false(assistance_database->path.empty());
      const auto& wide = assistance_database->path;
      const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                            nullptr, 0, nullptr, nullptr);
      check_greater(size, 0);
      std::string path(static_cast<std::size_t>(size), '\0');
      check_equal(::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                        path.data(), size, nullptr, nullptr), size);
      ConnectionProfile profile;
      profile.driver_id = "sqlite";
      profile.module_path = TURBODB_APP_SQLITE_PLUGIN_PATH;
      profile.options.push_back({"filename", path});
      std::string error;
      check_true(window->OpenConnection(profile, &error));
      SetSql(editor, "CREATE TABLE items(id INTEGER, name TEXT)");
      window->SendMessage(WM_COMMAND, kQueryExecute);
      check_true(WaitForQuery(*window));
      check_true(window->CloseActiveConnection(&error));
      check_true(window->OpenConnection(profile, &error));
      window->ShowWindow(SW_SHOWNOACTIVATE);
      ::SetFocus(editor);
      SetSql(editor, "SELECT items.name FROM items;");
      EditorNotification(editor, SCN_DWELLSTART, 14);
      check_true(::SendMessageW(editor, SCI_CALLTIPACTIVE, 0, 0) != 0);
      check_equal(::SendMessageW(editor, SCI_CALLTIPPOSSTART, 0, 0), 29);
      check_true(CaptureSqlTip(editor, "TURBODB_APP_FIELD_TIP_CAPTURE"));
      check_true(::GetFocus() == editor);
      EditorNotification(editor, SCN_DWELLEND, 14);
      check_false(::SendMessageW(editor, SCI_CALLTIPACTIVE, 0, 0) != 0);
      SetSql(editor, "items.");
      TypeSql(editor, "na");
      check_true(::SendMessageW(editor, SCI_AUTOCACTIVE, 0, 0) != 0);
      char candidate[64]{};
      ::SendMessageW(editor, SCI_AUTOCGETCURRENTTEXT, 0, reinterpret_cast<LPARAM>(candidate));
      check_equal(candidate, "items.name");
      SetSql(editor, "items.name");
      RequestSqlHelp(editor, true);
      check_true(::SendMessageW(editor, SCI_CALLTIPACTIVE, 0, 0) != 0);
      check_true(window->CloseActiveConnection(&error));
      check_false(::SendMessageW(editor, SCI_CALLTIPACTIVE, 0, 0) != 0);
      RequestSqlHelp(editor, true);
      check_false(::SendMessageW(editor, SCI_CALLTIPACTIVE, 0, 0) != 0);
    }
  }

  group("workspace colors") {
    it("renders typed schema icons and preserves selection and expansion on recoloring") {
      WorkspaceConnectionIdentity connection;
      connection.display_name = "Sample database";
      const std::vector<SchemaMetadataRecord> records = {
          {ORM_METADATA_TABLE, "demo", "main", "", "orders", 0},
          {ORM_METADATA_VIEW, "demo", "main", "", "open_orders", 0},
          {ORM_METADATA_COLUMN, "demo", "main", "orders", "id", 1}};
      std::string error;
      check_true(explorer_model.Replace(connection, records, &error));
      explorer_fixture = std::make_unique<SchemaExplorerView>();
      explorer_fixture->Bind(&explorer_model, &explorer_session);
      RECT bounds{0, 0, 280, 350};
      check_not_null(explorer_fixture->Create(*window, bounds, L"", WS_CHILD | WS_VISIBLE));
      CTreeViewCtrl tree(explorer_fixture->GetDlgItem(SchemaExplorerView::kTreeControlId));
      const HTREEITEM root = tree.GetRootItem();
      const HTREEITEM catalog = tree.GetChildItem(root);
      const HTREEITEM schema = tree.GetChildItem(catalog);
      const HTREEITEM table = tree.GetChildItem(schema);
      const HTREEITEM view = tree.GetNextSiblingItem(table);
      const HTREEITEM column = tree.GetChildItem(table);
      const HTREEITEM items[] = {root, catalog, schema, table, view, column};
      const int images[] = {0, 1, 1, 2, 3, 4};
      check_equal(tree.GetCount(), 6u);
      for (int i = 0; i < 6; ++i) {
        check_not_null(items[i]);
        TVITEMW item{};
        item.mask = TVIF_IMAGE | TVIF_SELECTEDIMAGE;
        item.hItem = items[i];
        check_true(tree.GetItem(&item) != FALSE);
        check_equal(item.iImage, images[i]);
        check_equal(item.iSelectedImage, images[i]);
        tree.Expand(items[i]);
      }
      tree.SelectItem(table);
      check_equal(explorer_session.relation(), std::string("orders"));
      const auto colors = CurrentStudioColors();
      for (int refresh = 0; refresh < 3; ++refresh) {
        check_true(explorer_fixture->RefreshColors());
        check_equal(tree.GetSelectedItem(), table);
        check_true((tree.GetItemState(schema, TVIS_EXPANDED) & TVIS_EXPANDED) != 0);
        check_equal(tree.GetCount(), 6u);
        check_equal(::ImageList_GetImageCount(tree.GetImageList(TVSIL_NORMAL)), 5);
        RECT client{};
        tree.GetClientRect(&client);
        std::vector<std::uint32_t> pixels;
        check_true(RenderToolbar(tree, {client.right, client.bottom}, &pixels));
        check_greater(ColorCount(pixels, colors.paper), 100);
        if (!colors.system)
          for (auto accent : {colors.purple, colors.amber, colors.blue, colors.teal})
            check_greater(ColorCount(pixels, accent), 3);
      }
      check_true(CaptureDialog(*explorer_fixture, "TURBODB_APP_EXPLORER_CAPTURE", false));
    }

    it("styles SQL tokens and refreshes colors without changing editing state") {
      const auto colors = CurrentStudioColors();
      const char* sql = "SELECT 42, 'color'; -- comment\n";
      ::SendMessageW(editor, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(sql));
      ::SendMessageW(editor, SCI_EMPTYUNDOBUFFER, 0, 0);
      ::SendMessageW(editor, SCI_APPENDTEXT, 1, reinterpret_cast<LPARAM>(" "));
      ::SendMessageW(editor, SCI_SETSEL, 7, 9);
      window->SendMessage(WM_SETTINGCHANGE);
      check_equal(::SendMessageW(editor, SCI_GETSELECTIONSTART, 0, 0), 7);
      check_equal(::SendMessageW(editor, SCI_GETSELECTIONEND, 0, 0), 9);
      check_true(::SendMessageW(editor, SCI_CANUNDO, 0, 0) != 0);
      check_equal(::SendMessageW(editor, SCI_GETSTYLEAT, 0, 0), SCE_SQL_WORD);
      check_equal(::SendMessageW(editor, SCI_GETSTYLEAT, 7, 0), SCE_SQL_NUMBER);
      check_equal(::SendMessageW(editor, SCI_STYLEGETFORE, SCE_SQL_WORD, 0), colors.blue);
      check_equal(::SendMessageW(editor, SCI_STYLEGETFORE, SCE_SQL_NUMBER, 0), colors.purple);
      check_equal(::SendMessageW(editor, SCI_STYLEGETFORE, SCE_SQL_CHARACTER, 0), colors.red);
      check_equal(::SendMessageW(editor, SCI_STYLEGETBACK, STYLE_LINENUMBER, 0), colors.surface);
      check_equal(::SendMessageW(editor, SCI_GETCARETLINEVISIBLE, 0, 0), !colors.system);
      window->SendMessage(WM_COMMAND, kEditUndo);
      char text[128]{};
      ::SendMessageW(editor, SCI_GETTEXT, sizeof(text), reinterpret_cast<LPARAM>(text));
      check_equal(text, sql);
      check_true(CaptureDialog(editor, "TURBODB_APP_EDITOR_CAPTURE", false));
    }

    it("keeps text and semantic accents readable on editor and result surfaces") {
      const auto colors = CurrentStudioColors();
      if (!colors.system) {
        const auto luminance = [](COLORREF color) {
          const auto linear = [](BYTE channel) {
            const double value = channel / 255.0;
            return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
          };
          return 0.2126 * linear(GetRValue(color)) + 0.7152 * linear(GetGValue(color)) +
                 0.0722 * linear(GetBValue(color));
        };
        for (auto foreground : {colors.ink, colors.muted, colors.blue, colors.green,
                                colors.amber, colors.purple, colors.teal, colors.red}) {
          for (auto background : {colors.paper, colors.surface, colors.current_line}) {
            check_greater_equal((luminance(background) + 0.05) /
                                 (luminance(foreground) + 0.05), 4.5);
          }
        }
        const COLORREF pairs[][2] = {
            {colors.chrome_text, colors.chrome}, {colors.green, colors.success},
            {colors.amber, colors.warning}, {colors.red, colors.error},
            {colors.blue, colors.hover}, {colors.ink, colors.pressed}};
        for (const auto& pair : pairs) {
          const double a = luminance(pair[0]), b = luminance(pair[1]);
          check_greater_equal(((std::max)(a, b) + 0.05) /
                               ((std::min)(a, b) + 0.05), 4.5);
        }
        for (auto background : {colors.surface, colors.hover, colors.pressed})
          check_greater_equal((luminance(background) + 0.05) /
                               (luminance(colors.blue) + 0.05), 3.0);
        for (auto background : {colors.chrome, colors.chrome_hover, colors.chrome_pressed}) {
          check_greater_equal((luminance(colors.chrome_text) + 0.05) /
                               (luminance(background) + 0.05), 4.5);
          for (auto accent : {colors.chrome_focus, colors.chrome_green, colors.chrome_blue,
                              colors.chrome_amber, colors.chrome_purple, colors.chrome_teal})
            check_greater_equal((luminance(accent) + 0.05) /
                                 (luminance(background) + 0.05), 3.0);
        }
      }
    }

    it("paints result lifecycle feedback and keeps small panes within their bounds") {
      result_fixture = std::make_unique<ResultWorkspaceView>();
      RECT bounds{0, 0, 600, 300};
      check_not_null(result_fixture->Create(*window, bounds, L"", WS_CHILD | WS_VISIBLE));
      const auto colors = CurrentStudioColors();
      const COLORREF backgrounds[] = {colors.surface, colors.success, colors.warning,
                                      colors.error, colors.success, colors.surface};
      for (int state = 0; state < 6; ++state) {
        switch (state) {
          case 0: result_fixture->Clear(); break;
          case 1: result_fixture->Clear(true); break;
          case 2: result_fixture->SetRunning(); break;
          case 3: result_fixture->ShowError("Example error"); break;
          case 4: {
            QueryResultSnapshot result;
            result.kind = QueryOutcomeKind::command;
            result_fixture->Render(result);
            break;
          }
          case 5: result_fixture->Clear(); break;
        }
        for (int id : {4202, 4203}) {
          HWND child = result_fixture->GetDlgItem(id);
          RECT client{};
          ::GetClientRect(child, &client);
          std::vector<std::uint32_t> pixels;
          check_true(RenderToolbar(child, {client.right, client.bottom}, &pixels));
          if (!colors.system)
            check_greater(ColorCount(pixels, id == 4202 ? colors.paper : backgrounds[state]), 100);
        }
        if (state == 3)
          check_true(CaptureDialog(*result_fixture, "TURBODB_APP_RESULT_ERROR_CAPTURE", false));
      }
      for (const SIZE size : {SIZE{600, 300}, SIZE{24, 24}, SIZE{1, 1}}) {
        result_fixture->SetWindowPos(nullptr, 0, 0, size.cx, size.cy,
                                     SWP_NOZORDER | SWP_NOACTIVATE);
        for (HWND child = ::GetWindow(*result_fixture, GW_CHILD); child != nullptr;
             child = ::GetWindow(child, GW_HWNDNEXT)) {
          RECT rect{};
          ::GetWindowRect(child, &rect);
          ::MapWindowPoints(nullptr, *result_fixture, reinterpret_cast<POINT*>(&rect), 2);
          check_in_range(rect.left, 0L, size.cx);
          check_in_range(rect.top, 0L, size.cy);
          check_in_range(rect.right, rect.left, size.cx);
          check_in_range(rect.bottom, rect.top, size.cy);
        }
      }
    }

    it("renders colored workspace sections and the empty connection guidance") {
      check_true(HasWindowText(*window, L"  Database"));
      check_true(HasWindowText(*window, L"Result pages"));
      check_true(HasWindowText(*window,
          L"No active connection.\r\n\r\nFile > New Connection\r\nCtrl+N"));
      ::SendMessageW(editor, SCI_SETTEXT, 0,
          reinterpret_cast<LPARAM>("-- Open a connection to get started\nSELECT 42 AS answer;\n"));
      window->ShowWindow(SW_SHOWNOACTIVATE);
      window->RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
      HWND explorer = FindExplorer(editor);
      check_not_null(explorer);
      HWND empty = ::FindWindowExW(explorer, nullptr, L"Edit", nullptr);
      check_not_null(empty);
      check_true(::IsWindowVisible(empty) != FALSE);
      window->SendMessage(WM_COMMAND, kViewExplorer);
      check_false(::IsWindowVisible(explorer) != FALSE);
      window->SendMessage(WM_COMMAND, kViewExplorer);
      check_true(::GetFocus() == empty);
      MSG tab{};
      tab.hwnd = empty;
      tab.message = WM_KEYDOWN;
      tab.wParam = VK_TAB;
      check_true(window->PreTranslateMessage(&tab) != FALSE);
      check_true(::GetFocus() == editor);
      RECT client{};
      window->GetClientRect(&client);
      std::vector<std::uint32_t> pixels;
      check_true(RenderToolbar(*window, {client.right, client.bottom}, &pixels, true));
      const auto colors = CurrentStudioColors();
      if (!colors.system) {
        check_greater(ColorCount(pixels, colors.chrome), 1000);
        check_greater(ColorCount(pixels, colors.surface), 1000);
      }
      check_true(CaptureDialog(*window, "TURBODB_APP_WORKSPACE_CAPTURE"));
    }

    it("stripes result rows while retaining native selected-row drawing") {
      result_fixture = std::make_unique<ResultWorkspaceView>();
      RECT bounds{0, 0, 600, 300};
      check_not_null(result_fixture->Create(*window, bounds, L"", WS_CHILD | WS_VISIBLE));
      QueryResultSnapshot result;
      result.kind = QueryOutcomeKind::rows;
      result.columns = {"value"};
      QueryCell cell;
      cell.kind = ORM_VALUE_INT64;
      cell.int64_value = 42;
      result.rows = {{cell}, {cell}};
      result_fixture->Render(result);
      CListViewCtrl grid(result_fixture->GetDlgItem(4201));
      NMLVCUSTOMDRAW draw{};
      draw.nmcd.hdr.hwndFrom = result_fixture->GetDlgItem(4201);
      draw.nmcd.hdr.idFrom = 4201;
      draw.nmcd.hdr.code = NM_CUSTOMDRAW;
      draw.nmcd.dwDrawStage = CDDS_ITEMPREPAINT;
      const auto colors = CurrentStudioColors();
      for (const UINT state : {0u, static_cast<UINT>(CDIS_SELECTED)}) {
        draw.nmcd.uItemState = state;
        for (DWORD_PTR row = 0; row < 2; ++row) {
          grid.SetItemState(-1, 0, LVIS_SELECTED);
          grid.SetItemState(static_cast<int>(row), state == CDIS_SELECTED ? LVIS_SELECTED : 0,
                             LVIS_SELECTED);
          draw.nmcd.dwItemSpec = row;
          draw.clrText = CLR_DEFAULT;
          draw.clrTextBk = CLR_DEFAULT;
          result_fixture->SendMessage(WM_NOTIFY, 4201, reinterpret_cast<LPARAM>(&draw));
          const bool native = colors.system || state == CDIS_SELECTED;
          check_equal(draw.clrText, native ? CLR_DEFAULT : colors.ink);
          check_equal(draw.clrTextBk, native ? CLR_DEFAULT :
                      row == 0 ? colors.paper : colors.surface);
        }
      }
      grid.SetItemState(-1, 0, LVIS_SELECTED);
      RECT client{};
      grid.GetClientRect(&client);
      const SIZE size{client.right, client.bottom};
      std::vector<std::uint32_t> pixels;
      check_true(RenderToolbar(grid, size, &pixels));
      if (const char* path = std::getenv("TURBODB_APP_RESULTS_CAPTURE"))
        check_true(SaveToolbarCapture(path, size, pixels));
      if (!colors.system) {
        const auto stripe = (static_cast<std::uint32_t>(GetRValue(colors.surface)) << 16) |
                            (static_cast<std::uint32_t>(GetGValue(colors.surface)) << 8) |
                            GetBValue(colors.surface);
        check_greater(std::count_if(pixels.begin(), pixels.end(), [stripe](auto pixel) {
          return (pixel & 0xffffff) == stripe;
        }), 100);
      }
    }
  }

  group("toolbar") {
    it("toggles and restores each pane through native toolbar clicks with matching View checks") {
      window->ShowWindow(SW_SHOWNOACTIVATE);
      CToolBarCtrl toolbar(window->GetDlgItem(CommandToolbar::kControlId));
      const HWND explorer = FindExplorer(editor);
      const HWND empty = ::FindWindowExW(explorer, nullptr, L"Edit", nullptr);
      const HWND results = ::FindWindowExW(::GetParent(editor), nullptr,
          L"TurboDBStudioResultWorkspace", nullptr);
      const HWND message = ::GetDlgItem(results, 4202);
      const HMENU view = ::GetSubMenu(::GetMenu(*window), 3);
      for (const auto command : {kViewExplorer, kViewEditor, kViewResults}) {
        const HWND target = command == kViewExplorer ? empty : command == kViewEditor ? editor : message;
        const HWND pane = command == kViewExplorer ? explorer : command == kViewEditor ? editor : results;
        check_not_null(target);
        check_true(toolbar.IsButtonChecked(command) != FALSE);
        RECT before{}, after{};
        check_true(::GetWindowRect(pane, &before) != FALSE);
        ::SetFocus(toolbar);
        check_true(ClickToolbar(toolbar, command));
        check_false(::IsWindowVisible(pane) != FALSE);
        check_false(toolbar.IsButtonChecked(command) != FALSE);
        window->SendMessage(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(view));
        check_equal(::GetMenuState(view, command, MF_BYCOMMAND) & MF_CHECKED, 0u);
        check_true(::GetFocus() != target && ::IsWindowVisible(::GetFocus()));
        check_true(ClickToolbar(toolbar, command));
        check_true(::IsWindowVisible(pane) != FALSE);
        check_true(toolbar.IsButtonChecked(command) != FALSE);
        window->SendMessage(WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(view));
        check_not_equal(::GetMenuState(view, command, MF_BYCOMMAND) & MF_CHECKED, 0u);
        check_equal(::GetFocus(), target);
        check_true(::GetWindowRect(pane, &after) != FALSE);
        check_true(::EqualRect(&before, &after) != FALSE);
      }
      check_true(CaptureDialog(*window, "TURBODB_APP_PANES_CAPTURE"));
    }

    it("retains SQL undo and results per query and never hides the last query pane") {
      window->ShowWindow(SW_SHOWNOACTIVATE);
      ConnectionProfile profile;
      profile.driver_id = "sqlite"; profile.module_path = TURBODB_APP_SQLITE_PLUGIN_PATH;
      profile.options.push_back({"filename", ":memory:"});
      std::string error;
      check_true(window->OpenConnection(profile, &error));
      SetSql(editor, "SELECT 42");
      ::SendMessageW(editor, SCI_GOTOPOS, 9, 0);
      ::SendMessageW(editor, SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>(";"));
      window->SendMessage(WM_COMMAND, kQueryExecute);
      check_true(WaitForQuery(*window));
      const HWND results = ::FindWindowExW(::GetParent(editor), nullptr,
          L"TurboDBStudioResultWorkspace", nullptr);
      CListViewCtrl grid(::GetDlgItem(results, 4201));
      const HWND tabs = ::GetDlgItem(::GetParent(::GetParent(editor)), QueryNotebook::kTabsId);
      CToolBarCtrl toolbar(window->GetDlgItem(CommandToolbar::kControlId));
      window->SendMessage(WM_COMMAND, kViewEditor);
      check_false(::IsWindowVisible(editor) != FALSE);
      check_equal(::GetFocus(), grid.m_hWnd);
      window->SendMessage(WM_COMMAND, kQueryNew);
      check_true(toolbar.IsButtonChecked(kViewEditor) != FALSE);
      window->SendMessage(WM_COMMAND, kQueryPrevious);
      check_false(toolbar.IsButtonChecked(kViewEditor) != FALSE);
      check_equal(::GetFocus(), grid.m_hWnd);
      check_equal(grid.GetItemCount(), 1);
      check_true(::SendMessageW(editor, SCI_CANUNDO, 0, 0) != FALSE);
      MSG tab{}; tab.hwnd = grid; tab.message = WM_KEYDOWN; tab.wParam = VK_TAB;
      check_true(window->PreTranslateMessage(&tab) != FALSE);
      check_equal(::GetFocus(), tabs);
      tab.hwnd = tabs;
      check_true(window->PreTranslateMessage(&tab) != FALSE);
      check_equal(::GetFocus(), grid.m_hWnd);
      window->SendMessage(WM_COMMAND, kViewResults);
      check_true(::IsWindowVisible(editor) != FALSE);
      check_false(::IsWindowVisible(results) != FALSE);
      check_equal(::GetFocus(), editor);
      window->SendMessage(WM_COMMAND, kViewEditor);
      check_false(::IsWindowVisible(editor) != FALSE);
      check_true(::IsWindowVisible(results) != FALSE);
      check_true(CaptureDialog(*window, "TURBODB_APP_RESULTS_ONLY_CAPTURE"));
      window->SendMessage(WM_COMMAND, kEditUndo);
      check_true(::IsWindowVisible(editor) != FALSE);
      check_true(toolbar.IsButtonChecked(kViewEditor) != FALSE);
      char sql[32]{};
      ::SendMessageW(editor, SCI_GETTEXT, sizeof(sql), reinterpret_cast<LPARAM>(sql));
      check_equal(sql, "SELECT 42");
      check_equal(grid.GetItemCount(), 1);
    }

    it("reveals hidden output for execution errors and Plan Details") {
      window->ShowWindow(SW_SHOWNOACTIVATE);
      const HWND results = ::FindWindowExW(::GetParent(editor), nullptr,
          L"TurboDBStudioResultWorkspace", nullptr);
      window->SendMessage(WM_COMMAND, kViewResults);
      check_false(::IsWindowVisible(results) != FALSE);
      window->SendMessage(WM_COMMAND, kQueryExecute);
      check_true(::IsWindowVisible(results) != FALSE);
      check_true(HasWindowText(results, L"Open a TurboDB connection before executing SQL."));
      ConnectionProfile profile;
      profile.driver_id = "sqlite"; profile.module_path = TURBODB_APP_SQLITE_PLUGIN_PATH;
      profile.options.push_back({"filename", ":memory:"});
      std::string error;
      check_true(window->OpenConnection(profile, &error));
      SetSql(editor, "SELECT 42");
      window->SendMessage(WM_COMMAND, kQueryPlan);
      check_true(WaitForQuery(*window));
      window->SendMessage(WM_COMMAND, kViewResults);
      check_false(::IsWindowVisible(results) != FALSE);
      window->SendMessage(WM_COMMAND, kViewPlanDetails);
      check_true(::IsWindowVisible(results) != FALSE);
      check_equal(::GetFocus(), ::GetDlgItem(results, 4205));
      CTabCtrl pages(::GetDlgItem(results, ResultWorkspaceView::kTabsId));
      check_equal(pages.GetCurSel(), 2);
    }

    it("supports keyboard pane toggles and checked styling after narrow window reflow") {
      window->ShowWindow(SW_SHOWNOACTIVATE);
      CToolBarCtrl toolbar(window->GetDlgItem(CommandToolbar::kControlId));
      for (const int width : {360, 1000}) {
        window->SetWindowPos(nullptr, 0, 0, width, 600, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        window->SendMessage(WM_COMMAND, kViewToolbar);
        toolbar.SetHotItem(toolbar.CommandToIndex(kViewEditor));
        MSG enter{}; enter.hwnd = toolbar; enter.message = WM_KEYDOWN; enter.wParam = VK_RETURN;
        check_true(window->PreTranslateMessage(&enter) != FALSE);
        check_false(::IsWindowVisible(editor) != FALSE);
        check_false(toolbar.IsButtonChecked(kViewEditor) != FALSE);
        window->SendMessage(WM_COMMAND, kViewToolbar);
        toolbar.SetHotItem(toolbar.CommandToIndex(kViewEditor));
        toolbar.SendMessage(WM_KEYDOWN, VK_SPACE);
        toolbar.SendMessage(WM_KEYUP, VK_SPACE);
        check_true(::IsWindowVisible(editor) != FALSE);
        check_true(toolbar.IsButtonChecked(kViewEditor) != FALSE);
        toolbar.SetHotItem(-1);
        RECT client{}, button{};
        toolbar.GetClientRect(&client);
        check_true(toolbar.GetItemRect(toolbar.CommandToIndex(kViewEditor), &button) != FALSE);
        std::vector<std::uint32_t> pixels;
        check_true(RenderToolbar(toolbar, {client.right, client.bottom}, &pixels));
        const auto colors = CurrentStudioColors();
        if (!colors.system) {
          const int pressed_pixels = ColorCount(pixels, colors.chrome_pressed);
          if (pressed_pixels <= 100) {
            // Preserve the visual gate. Capture native Win32 geometry, toolbar
            // state and both print paths to distinguish a clipped/wrapped
            // button from a native custom-draw notification regression.
            std::vector<std::uint32_t> native_pixels;
            const bool native_print = RenderToolbar(toolbar,
                {client.right, client.bottom}, &native_pixels, true);
            const auto native_pressed = native_print ?
                ColorCount(native_pixels, colors.chrome_pressed) : -1;
            const auto state = ::SendMessageW(toolbar.m_hWnd, TB_GETSTATE, kViewEditor, 0);
            std::fprintf(stderr,
                "STUDIO_TOOLBAR_WRAP_RED width=%d client=%ldx%ld button=%ld,%ld,%ld,%ld "
                "checked=%ld state=0x%llx printed=%d native_print=%d native_pressed=%d "
                "chrome=%d hover=%d first_pixel=0x%x\\n",
                width, static_cast<long>(client.right), static_cast<long>(client.bottom),
                static_cast<long>(button.left), static_cast<long>(button.top),
                static_cast<long>(button.right), static_cast<long>(button.bottom),
                static_cast<long>(toolbar.IsButtonChecked(kViewEditor)),
                static_cast<unsigned long long>(state), pressed_pixels,
                native_print ? 1 : 0, native_pressed,
                ColorCount(pixels, colors.chrome),
                ColorCount(pixels, colors.chrome_hover),
                pixels.empty() ? 0u : pixels.front());
          }
          check_greater(pressed_pixels, 100);
        }
        check_less_equal(button.bottom, client.bottom);
        check_true(::IsWindowVisible(::GetFocus()) != FALSE);
      }
    }

    it("supplies action descriptions shortcuts and bounded native tooltip text") {
      HWND toolbar = window->GetDlgItem(CommandToolbar::kControlId);
      for (const UINT command : {kQueryExecute, kQueryPlan, kQueryAnalyze, kEditUndo,
                                 kEditRedo, kViewExplorer, kViewEditor, kViewResults}) {
        wchar_t text[256]{};
        NMTBGETINFOTIPW tip{};
        tip.hdr = {toolbar, CommandToolbar::kControlId, TBN_GETINFOTIPW};
        tip.iItem = command;
        tip.pszText = text;
        tip.cchTextMax = _countof(text);
        window->SendMessage(WM_NOTIFY, CommandToolbar::kControlId, reinterpret_cast<LPARAM>(&tip));
        check_greater(std::wcslen(text), std::size_t{10});
        check_not_null(std::wcschr(text, L'\n'));
        if (command == kQueryExecute) check_not_null(std::wcsstr(text, L"F5 / Ctrl+Enter"));
        if (command == kQueryAnalyze) check_not_null(std::wcsstr(text, L"May execute"));
        text[2] = L'!';
        tip.cchTextMax = 2;
        window->SendMessage(WM_NOTIFY, CommandToolbar::kControlId, reinterpret_cast<LPARAM>(&tip));
        check_equal(text[1], L'\0');
        check_equal(text[2], L'!');
      }
    }

    it("paints distinct normal hover pressed disabled and keyboard focus states") {
      CToolBarCtrl toolbar(window->GetDlgItem(CommandToolbar::kControlId));
      const auto colors = CurrentStudioColors();
      RECT client{};
      toolbar.GetClientRect(&client);
      const COLORREF backgrounds[] = {colors.chrome, colors.chrome_hover, colors.chrome_pressed,
                                      colors.chrome};
      for (int state = 0; state < 4; ++state) {
        toolbar.EnableButton(kQueryExecute, state != 3);
        toolbar.SetHotItem(state == 1 ? 0 : -1);
        toolbar.PressButton(kQueryExecute, state == 2);
        std::vector<std::uint32_t> pixels;
        check_true(RenderToolbar(toolbar, {client.right, client.bottom}, &pixels));
        RECT button{};
        check_true(toolbar.GetItemRect(0, &button) != FALSE);
        std::vector<std::uint32_t> button_pixels;
        for (int y = button.top; y < button.bottom; ++y)
          for (int x = button.left; x < button.right; ++x)
            button_pixels.push_back(pixels[static_cast<std::size_t>(y) * client.right + x]);
        if (!colors.system) check_greater(ColorCount(button_pixels, backgrounds[state]), 100);
        if (state == 1)
          check_true(CaptureDialog(toolbar, "TURBODB_APP_TOOLBAR_HOVER_CAPTURE", false));
      }
      toolbar.EnableButton(kQueryExecute, TRUE);
      toolbar.PressButton(kQueryExecute, FALSE);
      // A hidden top-level window cannot receive keyboard focus.
      window->ShowWindow(SW_SHOWNOACTIVATE);
      window->SendMessage(WM_COMMAND, kViewToolbar);
      check_true(::GetFocus() == toolbar.m_hWnd);
      std::vector<std::uint32_t> pixels;
      check_true(RenderToolbar(toolbar, {client.right, client.bottom}, &pixels));
      RECT focus{};
      check_true(toolbar.GetItemRect(0, &focus) != FALSE);
      std::vector<std::uint32_t> perimeter;
      for (int x = focus.left + 2; x < focus.right - 2; ++x)
        perimeter.push_back(pixels[static_cast<std::size_t>(focus.top + 2) * client.right + x]);
      if (!colors.system)
        check_equal(ColorCount(perimeter, colors.chrome_focus), static_cast<int>(perimeter.size()));
      check_true(CaptureDialog(toolbar, "TURBODB_APP_TOOLBAR_FOCUS_CAPTURE", false));
    }

    it("paints semantic icon colors and preserves them across theme refresh") {
      const auto colors = CurrentStudioColors();
      CToolBarCtrl toolbar(window->GetDlgItem(CommandToolbar::kControlId));
      RECT client{};
      toolbar.GetClientRect(&client);
      const SIZE size{client.right, client.bottom};
      const COLORREF accents[] = {colors.chrome_green, colors.chrome_blue, colors.chrome_amber,
                                  colors.chrome_purple, colors.chrome_teal};
      for (const UINT message : {WM_THEMECHANGED, WM_SYSCOLORCHANGE, WM_SETTINGCHANGE}) {
        const bool undo_enabled = toolbar.IsButtonEnabled(kEditUndo) != FALSE;
        window->SendMessage(message);
        check_equal(toolbar.IsButtonEnabled(kEditUndo) != FALSE, undo_enabled);
        check_equal(::ImageList_GetImageCount(toolbar.GetImageList()), 8);
        std::vector<std::uint32_t> pixels;
        check_true(RenderToolbar(toolbar, size, &pixels));
        for (auto accent : accents) {
          if (colors.system) accent = ::GetSysColor(COLOR_BTNTEXT);
          const auto pixel = (static_cast<std::uint32_t>(GetRValue(accent)) << 16) |
                             (static_cast<std::uint32_t>(GetGValue(accent)) << 8) |
                             GetBValue(accent);
          check_true(std::any_of(pixels.begin(), pixels.end(), [pixel](auto value) {
            return (value & 0xffffff) == pixel;
          }));
        }
      }
    }

    it("renders eight labeled action icons on the native control") {
      CToolBarCtrl toolbar(window->GetDlgItem(CommandToolbar::kControlId));
      check_true(toolbar.IsWindow() != FALSE);
      check_true((toolbar.GetStyle() & WS_VISIBLE) != 0);
      check_equal(toolbar.GetButtonCount(), 10);
      HIMAGELIST images = toolbar.GetImageList();
      check_not_null(images);
      check_equal(::ImageList_GetImageCount(images), 8);
      RECT client{};
      toolbar.GetClientRect(&client);
      SIZE size{client.right, client.bottom};
      check_greater(size.cy, 0);
      std::vector<std::uint32_t> pixels;
      check_true(RenderToolbar(toolbar, size, &pixels));
      int actions = 0;
      for (int index = 0; index < toolbar.GetButtonCount(); ++index) {
        TBBUTTON button{};
        check_true(toolbar.GetButton(index, &button) != FALSE);
        if ((button.fsStyle & BTNS_SEP) != 0) continue;
        ++actions;
        check_true((button.fsStyle & BTNS_SHOWTEXT) != 0);
        check_in_range(button.iBitmap, 0, 7);
        wchar_t label[64]{};
        check_greater(toolbar.GetButtonText(button.idCommand, label), 0);
        RECT button_rect{};
        check_true(toolbar.GetItemRect(index, &button_rect) != FALSE);
        check_less_equal(button_rect.right, client.right);
        check_less_equal(button_rect.bottom, client.bottom);
        RECT icon{button_rect.left + 4, button_rect.top + 4,
                  button_rect.left + 24, button_rect.bottom - 4};
        RECT text{button_rect.left + 28, button_rect.top + 4,
                  button_rect.right - 4, button_rect.bottom - 4};
        check_greater(InkCount(pixels, size.cx, icon), 0);
        check_greater(InkCount(pixels, size.cx, text), 0);
      }
      check_equal(actions, 8);
      if (const char* capture = std::getenv("TURBODB_APP_TOOLBAR_CAPTURE"))
        check_true(SaveToolbarCapture(capture, size, pixels));
    }

    it("wraps buttons at narrow widths and reserves workspace space below") {
      CToolBarCtrl toolbar(window->GetDlgItem(CommandToolbar::kControlId));
      HWND workspace = ::GetParent(::GetParent(::GetParent(editor)));
      for (const int width : {320, 900, 1400}) {
        window->SetWindowPos(nullptr, 0, 0, width, 650,
                              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RECT client{}, bar{}, pane{};
        window->GetClientRect(&client);
        toolbar.GetWindowRect(&bar);
        ::GetWindowRect(workspace, &pane);
        ::MapWindowPoints(nullptr, *window, reinterpret_cast<POINT*>(&bar), 2);
        ::MapWindowPoints(nullptr, *window, reinterpret_cast<POINT*>(&pane), 2);
        check_equal(bar.top, 0);
        check_less_equal(bar.right, client.right);
        check_equal(pane.top, bar.bottom);
        check_equal(pane.bottom, client.bottom);
        RECT toolbar_client{};
        toolbar.GetClientRect(&toolbar_client);
        for (int index = 0; index < toolbar.GetButtonCount(); ++index) {
          TBBUTTON button{};
          toolbar.GetButton(index, &button);
          if ((button.fsStyle & BTNS_SEP) != 0) continue;
          RECT bounds{};
          check_true(toolbar.GetItemRect(index, &bounds) != FALSE);
          check_less_equal(bounds.right, toolbar_client.right);
          check_less_equal(bounds.bottom, toolbar_client.bottom);
        }
      }
    }

    it("clicks Execute through the native toolbar command notification") {
      CToolBarCtrl toolbar(window->GetDlgItem(CommandToolbar::kControlId));
      RECT button{};
      check_true(toolbar.GetItemRect(0, &button) != FALSE);
      const LPARAM point = MAKELPARAM((button.left + button.right) / 2,
                                      (button.top + button.bottom) / 2);
      toolbar.SendMessage(WM_LBUTTONDOWN, MK_LBUTTON, point);
      toolbar.SendMessage(WM_LBUTTONUP, 0, point);
      check_true(HasWindowText(*window, L"Open a TurboDB connection before executing SQL."));
    }

    it("keeps Undo and Redo availability in sync with editor state") {
      CToolBarCtrl toolbar(window->GetDlgItem(CommandToolbar::kControlId));
      check_false(toolbar.IsButtonEnabled(kEditUndo) != FALSE);
      ::SendMessageW(editor, SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>("select 1"));
      window->OnIdle();
      check_true(toolbar.IsButtonEnabled(kEditUndo) != FALSE);
      window->SendMessage(WM_COMMAND, kEditUndo, reinterpret_cast<LPARAM>(toolbar.m_hWnd));
      check_false(toolbar.IsButtonEnabled(kEditUndo) != FALSE);
      check_true(toolbar.IsButtonEnabled(kEditRedo) != FALSE);
    }

    it("activates the focused toolbar command with Enter") {
      CToolBarCtrl toolbar(window->GetDlgItem(CommandToolbar::kControlId));
      window->SendMessage(WM_COMMAND, kViewToolbar);
      check_equal(toolbar.GetHotItem(), 0);
      MSG message{};
      message.hwnd = toolbar;
      message.message = WM_KEYDOWN;
      message.wParam = VK_RETURN;
      check_true(window->PreTranslateMessage(&message) != FALSE);
      check_true(HasWindowText(*window, L"Open a TurboDB connection before executing SQL."));
      toolbar.SendMessage(WM_KEYDOWN, VK_RIGHT);
      toolbar.SendMessage(WM_KEYUP, VK_RIGHT);
      check_equal(toolbar.GetHotItem(), 1);
      toolbar.SendMessage(WM_KEYDOWN, VK_SPACE);
      toolbar.SendMessage(WM_KEYUP, VK_SPACE);
      check_true(HasWindowText(*window, L"Open a TurboDB connection before explaining SQL."));
      message.wParam = VK_ESCAPE;
      check_true(window->PreTranslateMessage(&message) != FALSE);
    }
  }

  it("closes the frame and releases its menu through File Exit") {
    const HMENU menu = ::GetMenu(*window);
    window->SendMessage(WM_COMMAND, kFileExit);
    check_false(window->IsWindow() != FALSE);
    check_false(::IsMenu(menu) != FALSE);
  }
}
