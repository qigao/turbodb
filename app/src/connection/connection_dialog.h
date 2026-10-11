#pragma once

#include <array>
#include <string>

#include <atlbase.h>
#include <atlapp.h>
#include <atlwin.h>
#include <atlctrls.h>

#include "connection/connection_manager.h"
#include "connection/connection_resource.h"

namespace turbodb::app {

class MainWindow;

// The frame and its connection owner outlive this modal dialog. Profiles and
// credentials are used only for the explicit connection attempt, never saved.
class ConnectionDialog final : public CDialogImpl<ConnectionDialog> {
 public:
  enum { IDD = IDD_STUDIO_CONNECTION };
  explicit ConnectionDialog(MainWindow& owner, int recent_profile = -1) noexcept
      : owner_(owner), recent_profile_(recent_profile) {}

  BEGIN_MSG_MAP(ConnectionDialog)
    MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
    MESSAGE_HANDLER(WM_TIMER, OnConnectionTimer)
    MESSAGE_HANDLER(WM_CLOSE, OnClose)
    MESSAGE_HANDLER(WM_DESTROY, OnDestroy)
    MESSAGE_HANDLER(WM_CTLCOLORDLG, OnControlColor)
    MESSAGE_HANDLER(WM_CTLCOLORSTATIC, OnControlColor)
    MESSAGE_HANDLER(WM_CTLCOLOREDIT, OnControlColor)
    COMMAND_HANDLER(IDC_CONNECTION_DRIVER, CBN_SELCHANGE, OnDriverChanged)
    COMMAND_HANDLER(IDC_CONNECTION_PROFILE, CBN_SELCHANGE, OnProfileChanged)
    COMMAND_ID_HANDLER(IDC_CONNECTION_CONFIG_LOAD, OnLoadConfiguration)
    COMMAND_ID_HANDLER(IDC_CONNECTION_DATABASE_BROWSE, OnBrowse)
    COMMAND_ID_HANDLER(IDC_CONNECTION_CA_BROWSE, OnBrowse)
    COMMAND_RANGE_HANDLER(IDC_CONNECTION_NAME, IDC_CONNECTION_SERVER, OnFieldEdited)
    COMMAND_ID_HANDLER(IDOK, OnConnect)
    COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
  END_MSG_MAP()

 private:
  LRESULT OnInitDialog(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnConnectionTimer(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnClose(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnDestroy(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnControlColor(UINT, WPARAM, LPARAM, BOOL&);
  LRESULT OnDriverChanged(WORD, WORD, HWND, BOOL&);
  LRESULT OnProfileChanged(WORD, WORD, HWND, BOOL&);
  LRESULT OnLoadConfiguration(WORD, WORD, HWND, BOOL&);
  LRESULT OnBrowse(WORD, WORD, HWND, BOOL&);
  LRESULT OnConnect(WORD, WORD, HWND, BOOL&);
  LRESULT OnCancel(WORD, WORD, HWND, BOOL&);
  LRESULT OnFieldEdited(WORD, WORD, HWND, BOOL&);
  void UpdateDriver();
  void UpdateModulePath();
  void RefreshProfiles();
  bool ApplyProfile();
  void LayoutFields();
  void ClearError();
  void UpdateHint();
  void SetConnecting(bool connecting);
  std::wstring FieldText(int id) const;
  bool ReadField(int id, bool required, std::string* output);
  bool EncodeText(const std::wstring& text, int focus, std::string* output);
  bool ReadProfile(ConnectionProfile* profile);
  void ShowError(const std::wstring& message, int focus);

  MainWindow& owner_;
  int recent_profile_ = -1;
  // Driver location is internal dialog state, including while a preset is edited.
  std::wstring module_path_;
  struct ProviderDraft {
    std::wstring database;
    std::wstring port;
  };
  // Only this modal session keeps these per-provider drafts. Passwords remain
  // in the native password control and are cleared on provider changes.
  std::array<ProviderDraft, 3> drafts_;
  int current_driver_ = -1;
  bool updating_fields_ = false;
  bool error_visible_ = false;
  RECT expanded_window_{};
  std::array<RECT, 5> footer_bounds_{};
  int network_height_ = 0;
  int error_height_ = 0;
};

}  // namespace turbodb::app
