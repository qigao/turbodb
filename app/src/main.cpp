#include <windows.h>

#include <cwchar>

#include <atlbase.h>
#include <atlapp.h>

CAppModule _Module;

#include <atlwin.h>

#include "editor/scintilla_runtime.h"
#include "main_window.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line,
                    int show_command) {
  const bool self_test =
      command_line != nullptr && std::wcscmp(command_line, L"--self-test") == 0;

  const HRESULT com_result =
      ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (FAILED(com_result)) {
    return 1;
  }

  INITCOMMONCONTROLSEX controls = {
      sizeof(controls),
      ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES |
          ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES,
  };
  if (!::InitCommonControlsEx(&controls)) {
    ::CoUninitialize();
    return 1;
  }

  turbodb::app::ScintillaRuntime editor_runtime;
  if (!editor_runtime.Initialize()) {
    if (!self_test) {
      ::MessageBoxW(
          nullptr,
          L"TurboDB Studio could not load Scintilla.dll and Lexilla.dll.",
          L"TurboDB Studio", MB_OK | MB_ICONERROR);
    }
    ::CoUninitialize();
    return 2;
  }

  if (FAILED(_Module.Init(nullptr, instance))) {
    ::CoUninitialize();
    return 3;
  }

  if (self_test) {
    _Module.Term();
    ::CoUninitialize();
    return 0;
  }

  CMessageLoop message_loop;
  _Module.AddMessageLoop(&message_loop);

  turbodb::app::MainWindow main_window(editor_runtime);
  const DWORD style =
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
  if (main_window.Create(nullptr, CWindow::rcDefault, L"TurboDB Studio",
                         style) == nullptr) {
    _Module.RemoveMessageLoop();
    _Module.Term();
    ::CoUninitialize();
    return 1;
  }

  main_window.ShowWindow(show_command);
  main_window.UpdateWindow();

  const int result = message_loop.Run();

  _Module.RemoveMessageLoop();
  _Module.Term();
  ::CoUninitialize();
  return result;
}
