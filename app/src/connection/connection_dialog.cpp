#include "connection/connection_dialog.h"
#include "studio_colors.h"

#include <algorithm>
#include <vector>

#include <atldlgs.h>
#include <atlctrlx.h>
#include <cmeta_fs.h>

#include "main_window.h"

namespace turbodb::app {
namespace {

constexpr int kFieldCharacterLimit = 4096;
constexpr DWORD kConnectionTimeoutMs = 10000;
constexpr DWORD kFileDialogPathCharacters = 32768;
constexpr UINT_PTR kConnectionTimer = 1;
constexpr UINT kConnectionPollMs = 50;
constexpr const char* kDriverIds[] = {"sqlite", "postgresql", "mysql"};

std::wstring ErrorText(const std::string& text) {
  const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
      text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return L"Could not open the connection.";
  std::wstring output(static_cast<std::size_t>(size), L'\0');
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), output.data(), size) != size)
    return L"Could not open the connection.";
  return output;
}

}  // namespace

LRESULT ConnectionDialog::OnControlColor(UINT message, WPARAM wparam, LPARAM lparam,
                                         BOOL& handled) {
  const auto colors = CurrentStudioColors();
  if (colors.system) {
    handled = FALSE;
    return 0;
  }
  COLORREF foreground = colors.ink, background = colors.surface;
  const int id = message == WM_CTLCOLORDLG ? 0 :
      ::GetDlgCtrlID(reinterpret_cast<HWND>(lparam));
  if (message == WM_CTLCOLOREDIT) {
    background = colors.paper;
  } else if (id == IDC_CONNECTION_DRIVER_SECTION || id == IDC_CONNECTION_DATABASE_SECTION ||
             id == IDC_CONNECTION_TLS_SECTION) {
    foreground = colors.blue;
    background = colors.surface;
  } else if (id == IDC_CONNECTION_ERROR || id == IDC_CONNECTION_ERROR_LABEL) {
    foreground = colors.red;
    background = colors.error;
  } else if (id == IDC_CONNECTION_HINT) {
    foreground = colors.blue;
    background = colors.hover;
  } else if (message == WM_CTLCOLORSTATIC &&
             !::IsWindowEnabled(reinterpret_cast<HWND>(lparam))) {
    // Disabled native edits still need their system unavailable appearance.
    handled = FALSE;
    return 0;
  }
  return reinterpret_cast<LRESULT>(StudioControlColors(reinterpret_cast<HDC>(wparam),
                                                       foreground, background));
}

LRESULT ConnectionDialog::OnInitDialog(UINT, WPARAM, LPARAM, BOOL&) {
  updating_fields_ = true;
  CComboBox drivers(GetDlgItem(IDC_CONNECTION_DRIVER));
  drivers.AddString(L"SQLite");
  drivers.AddString(L"PostgreSQL");
  drivers.AddString(L"MySQL");
  drivers.SetCurSel(0);
  for (int id : {IDC_CONNECTION_NAME,
       IDC_CONNECTION_DATABASE, IDC_CONNECTION_HOST, IDC_CONNECTION_PORT,
       IDC_CONNECTION_USER, IDC_CONNECTION_PASSWORD, IDC_CONNECTION_CA,
       IDC_CONNECTION_SERVER})
    SendDlgItemMessage(id, EM_SETLIMITTEXT, kFieldCharacterLimit);
  SendDlgItemMessage(IDC_CONNECTION_PORT, EM_SETLIMITTEXT, 5);
  SendDlgItemMessage(IDC_CONNECTION_NAME, EM_SETCUEBANNER, FALSE,
                    reinterpret_cast<LPARAM>(L"Optional"));
  SendDlgItemMessage(IDC_CONNECTION_CA, EM_SETCUEBANNER, FALSE,
                    reinterpret_cast<LPARAM>(L"Select a trusted CA certificate"));
  GetWindowRect(&expanded_window_);
  const int footer_ids[] = {IDC_CONNECTION_HINT, IDC_CONNECTION_ERROR_LABEL,
                            IDC_CONNECTION_ERROR, IDOK, IDCANCEL};
  for (std::size_t index = 0; index < footer_bounds_.size(); ++index) {
    GetDlgItem(footer_ids[index]).GetWindowRect(&footer_bounds_[index]);
    ::MapWindowPoints(nullptr, m_hWnd,
        reinterpret_cast<POINT*>(&footer_bounds_[index]), 2);
  }
  RECT database{};
  GetDlgItem(IDC_CONNECTION_DATABASE).GetWindowRect(&database);
  ::MapWindowPoints(nullptr, m_hWnd, reinterpret_cast<POINT*>(&database), 2);
  RECT gap{0, 0, 0, 8};
  ::MapDialogRect(m_hWnd, &gap);
  network_height_ = footer_bounds_[0].top - database.bottom - gap.bottom;
  error_height_ = footer_bounds_[3].top - footer_bounds_[1].top;
  drafts_[0].database = L":memory:";
  drafts_[1].port = L"5432";
  drafts_[2].port = L"3306";
  SetDlgItemText(IDC_CONNECTION_HOST, L"localhost");
  std::wstring error;
  const bool loaded = owner_.InitializeConnectionConfiguration(&error);
  UpdateDriver();
  RefreshProfiles();
  if (recent_profile_ >= 0 &&
      static_cast<std::size_t>(recent_profile_) < owner_.connection_configuration().profiles.size())
    CComboBox(GetDlgItem(IDC_CONNECTION_PROFILE)).SetCurSel(recent_profile_ + 1);
  if (!loaded) {
    ShowError(error, IDC_CONNECTION_CONFIG_LOAD);
    return FALSE;
  }
  if (!ApplyProfile()) return FALSE;
  if (recent_profile_ >= 0) PostMessage(WM_COMMAND, IDOK);
  return TRUE;
}

void ConnectionDialog::RefreshProfiles() {
  CComboBox profiles(GetDlgItem(IDC_CONNECTION_PROFILE));
  profiles.ResetContent();
  profiles.AddString(L"Manual entry");
  const auto& config = owner_.connection_configuration();
  for (const auto& preset : config.profiles) profiles.AddString(preset.name.c_str());
  profiles.SetCurSel(config.profiles.empty() ? 0 :
                    static_cast<int>(config.default_profile + 1));
}

bool ConnectionDialog::ApplyProfile() {
  const int selected = CComboBox(GetDlgItem(IDC_CONNECTION_PROFILE)).GetCurSel();
  const auto& config = owner_.connection_configuration();
  if (selected <= 0) {
    UpdateModulePath();
    SetDlgItemText(IDC_CONNECTION_PASSWORD, L"");
    ClearError();
    return true;
  }
  if (static_cast<std::size_t>(selected) > config.profiles.size()) return false;
  const auto& preset = config.profiles[static_cast<std::size_t>(selected - 1)];
  std::wstring password, error;
  const bool password_ready = ResolvePresetPassword(preset, &password, &error);
  const int driver = preset.provider == SqlProvider::sqlite ? 0 :
                     preset.provider == SqlProvider::postgresql ? 1 : 2;
  CComboBox(GetDlgItem(IDC_CONNECTION_DRIVER)).SetCurSel(driver);
  UpdateDriver();
  updating_fields_ = true;
  module_path_ = preset.module_path;
  SetDlgItemText(IDC_CONNECTION_NAME, preset.display_name.c_str());
  SetDlgItemText(IDC_CONNECTION_DATABASE, preset.database.c_str());
  SetDlgItemText(IDC_CONNECTION_HOST, preset.host.empty() ? L"localhost" : preset.host.c_str());
  SetDlgItemText(IDC_CONNECTION_PORT, preset.port.c_str());
  SetDlgItemText(IDC_CONNECTION_USER, preset.user.c_str());
  SetDlgItemText(IDC_CONNECTION_PASSWORD, password.c_str());
  SetDlgItemText(IDC_CONNECTION_CA, preset.ca_certificate.c_str());
  SetDlgItemText(IDC_CONNECTION_SERVER, preset.tls_server_name.c_str());
  updating_fields_ = false;
  if (!password_ready) {
    ShowError(error, IDC_CONNECTION_PASSWORD);
    return false;
  }
  ClearError();
  return true;
}

LRESULT ConnectionDialog::OnProfileChanged(WORD, WORD, HWND, BOOL&) {
  if (owner_.ConnectionOpening()) return 0;
  ApplyProfile();
  return 0;
}

LRESULT ConnectionDialog::OnLoadConfiguration(WORD, WORD, HWND, BOOL&) {
  if (owner_.ConnectionOpening()) return 0;
  CFileDialog picker(TRUE, L"ini", nullptr,
      OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR,
      L"Connection configuration (*.ini)\0*.ini\0\0", m_hWnd);
  std::vector<wchar_t> path(kFileDialogPathCharacters, L'\0');
  picker.m_ofn.lpstrFile = path.data();
  picker.m_ofn.nMaxFile = static_cast<DWORD>(path.size());
  if (picker.DoModal() == IDOK) {
    std::wstring error;
    if (!owner_.LoadConnectionConfiguration(path.data(), &error))
      ShowError(error, IDC_CONNECTION_CONFIG_LOAD);
    else {
      RefreshProfiles();
      ApplyProfile();
    }
  } else if (::CommDlgExtendedError() != 0) {
    ShowError(L"Could not open the configuration file picker.", IDC_CONNECTION_CONFIG_LOAD);
  }
  return 0;
}

std::wstring ConnectionDialog::FieldText(int id) const {
  const CWindow field(GetDlgItem(id));
  const int length = field.GetWindowTextLength();
  std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
  const int read = field.GetWindowText(text.data(), length + 1);
  text.resize(static_cast<std::size_t>(read));
  return text;
}

void ConnectionDialog::LayoutFields() {
  const int provider_shift = current_driver_ == 0 ? network_height_ : 0;
  const int status_shift = error_visible_ ? 0 : error_height_;
  const int footer_ids[] = {IDC_CONNECTION_HINT, IDC_CONNECTION_ERROR_LABEL,
                            IDC_CONNECTION_ERROR, IDOK, IDCANCEL};
  for (std::size_t index = 0; index < footer_bounds_.size(); ++index) {
    const auto& bounds = footer_bounds_[index];
    GetDlgItem(footer_ids[index]).SetWindowPos(nullptr, bounds.left,
        bounds.top - provider_shift - (index >= 3 ? status_shift : 0),
        bounds.right - bounds.left, bounds.bottom - bounds.top,
        SWP_NOZORDER | SWP_NOACTIVATE);
  }
  GetDlgItem(IDC_CONNECTION_ERROR_LABEL).ShowWindow(error_visible_ ? SW_SHOW : SW_HIDE);
  GetDlgItem(IDC_CONNECTION_ERROR).ShowWindow(error_visible_ ? SW_SHOW : SW_HIDE);
  SetWindowPos(nullptr, 0, 0,
      expanded_window_.right - expanded_window_.left,
      expanded_window_.bottom - expanded_window_.top - provider_shift - status_shift,
      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  // Keep the footer on screen when the compact form grows for network fields
  // or diagnostics, using the native monitor/work-area centering contract.
  CenterWindow(GetParent());
}

void ConnectionDialog::UpdateHint() {
  SetDlgItemText(IDC_CONNECTION_HINT, current_driver_ == 0
      ? L"Use :memory: for a temporary database, or select a database file. Display name is optional."
      : L"The CA certificate and TLS server name verify the server. Display name and password may be empty.");
}

void ConnectionDialog::UpdateModulePath() {
  const auto provider = current_driver_ == 0 ? SqlProvider::sqlite :
                        current_driver_ == 1 ? SqlProvider::postgresql : SqlProvider::mysql;
  module_path_ = DriverModulePath(owner_.connection_configuration(), provider);
}

void ConnectionDialog::UpdateDriver() {
  const int driver = CComboBox(GetDlgItem(IDC_CONNECTION_DRIVER)).GetCurSel();
  if (driver < 0 || driver >= static_cast<int>(drafts_.size()) ||
      driver == current_driver_)
    return;
  updating_fields_ = true;
  if (current_driver_ >= 0) {
    auto& previous = drafts_[static_cast<std::size_t>(current_driver_)];
    previous.database = FieldText(IDC_CONNECTION_DATABASE);
    previous.port = FieldText(IDC_CONNECTION_PORT);
  }
  current_driver_ = driver;
  const bool network = driver != 0;
  for (int id : {IDC_CONNECTION_HOST, IDC_CONNECTION_HOST_LABEL,
       IDC_CONNECTION_PORT, IDC_CONNECTION_PORT_LABEL,
       IDC_CONNECTION_USER, IDC_CONNECTION_USER_LABEL,
       IDC_CONNECTION_PASSWORD, IDC_CONNECTION_PASSWORD_LABEL,
       IDC_CONNECTION_CA, IDC_CONNECTION_CA_LABEL, IDC_CONNECTION_CA_BROWSE,
       IDC_CONNECTION_SERVER, IDC_CONNECTION_SERVER_LABEL,
       IDC_CONNECTION_TLS_SECTION}) {
    GetDlgItem(id).EnableWindow(network);
    GetDlgItem(id).ShowWindow(network ? SW_SHOW : SW_HIDE);
  }
  GetDlgItem(IDC_CONNECTION_DATABASE_BROWSE).EnableWindow(!network);
  GetDlgItem(IDC_CONNECTION_DATABASE_BROWSE).ShowWindow(network ? SW_HIDE : SW_SHOW);
  RECT database_width{}, database_bounds{};
  GetDlgItem(network ? IDC_CONNECTION_NAME : IDC_CONNECTION_PROFILE)
      .GetWindowRect(&database_width);
  GetDlgItem(IDC_CONNECTION_DATABASE).GetWindowRect(&database_bounds);
  GetDlgItem(IDC_CONNECTION_DATABASE).SetWindowPos(nullptr, 0, 0,
      database_width.right - database_width.left,
      database_bounds.bottom - database_bounds.top,
      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  SetDlgItemText(IDC_CONNECTION_DATABASE_LABEL,
                 network ? L"Data&base:" : L"Database &file:");
  const auto& draft = drafts_[static_cast<std::size_t>(driver)];
  SetDlgItemText(IDC_CONNECTION_DATABASE, draft.database.c_str());
  SetDlgItemText(IDC_CONNECTION_PORT, draft.port.c_str());
  UpdateModulePath();
  SetDlgItemText(IDC_CONNECTION_PASSWORD, L"");
  error_visible_ = false;
  UpdateHint();
  LayoutFields();
  updating_fields_ = false;
}

LRESULT ConnectionDialog::OnDriverChanged(WORD, WORD, HWND, BOOL&) {
  if (owner_.ConnectionOpening()) return 0;
  CComboBox(GetDlgItem(IDC_CONNECTION_PROFILE)).SetCurSel(0);
  UpdateDriver();
  UpdateModulePath();
  return 0;
}

void ConnectionDialog::ShowError(const std::wstring& message, int focus) {
  SetDlgItemText(IDC_CONNECTION_ERROR, message.c_str());
  error_visible_ = true;
  LayoutFields();
  if (focus != 0) {
    ::SendMessage(m_hWnd, WM_NEXTDLGCTL,
                  reinterpret_cast<WPARAM>(::GetDlgItem(m_hWnd, focus)), TRUE);
    if (focus >= IDC_CONNECTION_NAME && focus <= IDC_CONNECTION_SERVER)
      SendDlgItemMessage(focus, EM_SETSEL, 0, -1);
  } else {
    ::SendMessage(m_hWnd, WM_NEXTDLGCTL,
                  reinterpret_cast<WPARAM>(::GetDlgItem(m_hWnd, IDC_CONNECTION_ERROR)), TRUE);
  }
  ::MessageBeep(MB_ICONWARNING);
}

void ConnectionDialog::ClearError() {
  if (!error_visible_) return;
  error_visible_ = false;
  SetDlgItemText(IDC_CONNECTION_ERROR, L"");
  LayoutFields();
}

LRESULT ConnectionDialog::OnFieldEdited(WORD notification, WORD, HWND, BOOL& handled) {
  if (owner_.ConnectionOpening()) return 0;
  if (notification != EN_CHANGE) {
    handled = FALSE;
    return 0;
  }
  if (!updating_fields_) {
    CComboBox(GetDlgItem(IDC_CONNECTION_PROFILE)).SetCurSel(0);
    ClearError();
  }
  return 0;
}

bool ConnectionDialog::ReadField(int id, bool required, std::string* output) {
  CWindow field(GetDlgItem(id));
  const int length = field.GetWindowTextLength();
  std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
  const int read = field.GetWindowText(text.data(), length + 1);
  if (read != length) {
    ShowError(L"Could not read the connection field.", id);
    return false;
  }
  output->clear();
  if (length == 0) {
    if (!required) return true;
    const CWindow label(::GetWindow(field, GW_HWNDPREV));
    const int label_length = label.GetWindowTextLength();
    std::wstring name(static_cast<std::size_t>(label_length) + 1, L'\0');
    const int label_read = label.GetWindowText(name.data(), label_length + 1);
    name.resize(static_cast<std::size_t>(label_read));
    name.erase(std::remove(name.begin(), name.end(), L'&'), name.end());
    if (!name.empty() && name.back() == L':') name.pop_back();
    ShowError(L"Enter a value for " + name + L".", id);
    return false;
  }
  text.resize(static_cast<std::size_t>(read));
  return EncodeText(text, id, output);
}

bool ConnectionDialog::EncodeText(const std::wstring& text, int focus, std::string* output) {
  output->clear();
  if (text.empty()) return true;
  // Both native fields and internally resolved paths are bounded at their source.
  const int length = static_cast<int>(text.size());
  const int bytes = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
      text.data(), length, nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) {
    ShowError(L"The connection value contains invalid Unicode.", focus);
    return false;
  }
  // std::string is the existing ConnectionProfile boundary; the dialog uses
  // native UTF-16 controls and passes explicit UTF-8 lengths to that owner.
  output->resize(static_cast<std::size_t>(bytes));
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length,
        output->data(), bytes, nullptr, nullptr) != bytes) {
    ShowError(L"Could not convert the connection value to UTF-8.", focus);
    return false;
  }
  return true;
}

bool ConnectionDialog::ReadProfile(ConnectionProfile* profile) {
  const int driver = CComboBox(GetDlgItem(IDC_CONNECTION_DRIVER)).GetCurSel();
  if (driver < 0 || driver >= static_cast<int>(_countof(kDriverIds))) {
    ShowError(L"Select a database driver.", IDC_CONNECTION_DRIVER);
    return false;
  }
  profile->driver_id = kDriverIds[driver];
  if (module_path_.empty()) {
    ShowError(L"Could not resolve the driver module. Check module_path in the INI configuration.",
              IDC_CONNECTION_CONFIG_LOAD);
    return false;
  }
  if (!EncodeText(module_path_, IDC_CONNECTION_CONFIG_LOAD, &profile->module_path) ||
      !ReadField(IDC_CONNECTION_NAME, false, &profile->display_name))
    return false;
  if (!cmeta_fs_path_is_absolute(profile->module_path.c_str())) {
    ShowError(L"The driver module must have an absolute path. Check module_path in the INI configuration.",
              IDC_CONNECTION_CONFIG_LOAD);
    return false;
  }
  std::string database;
  if (!ReadField(IDC_CONNECTION_DATABASE, true, &database)) return false;
  if (driver == 0) {
    profile->options.push_back({"filename", std::move(database)});
    return true;
  }

  std::string host, port, user, password, ca, server;
  if (!ReadField(IDC_CONNECTION_HOST, true, &host) ||
      !ReadField(IDC_CONNECTION_PORT, true, &port) ||
      !ReadField(IDC_CONNECTION_USER, true, &user) ||
      !ReadField(IDC_CONNECTION_PASSWORD, false, &password) ||
      !ReadField(IDC_CONNECTION_CA, true, &ca) ||
      !ReadField(IDC_CONNECTION_SERVER, true, &server))
    return false;
  unsigned port_number = 0;
  for (unsigned char character : port) {
    if (character < '0' || character > '9' || port_number > 6553u) {
      ShowError(L"Port must be a decimal number from 1 to 65535.", IDC_CONNECTION_PORT);
      return false;
    }
    port_number = port_number * 10u + character - '0';
  }
  if (port_number == 0 || port_number > 65535u) {
    ShowError(L"Port must be a decimal number from 1 to 65535.", IDC_CONNECTION_PORT);
    return false;
  }
  if (driver == 1) {
    profile->options = {
        {"dbname", std::move(database)}, {"user", std::move(user)},
        {"password", std::move(password)}, {"turbodb_pg_transport", "direct_tls"},
        {"turbodb_pg_remote_host", std::move(host)},
        {"turbodb_pg_remote_port", std::move(port)},
        {"turbodb_pg_ca_file", std::move(ca)},
        {"turbodb_pg_server_name", std::move(server)},
        {"turbodb_pg_connect_timeout_ms", std::to_string(kConnectionTimeoutMs)},
        {"turbodb_pg_handshake_timeout_ms", std::to_string(kConnectionTimeoutMs)}};
  } else {
    profile->options = {
        {"database", std::move(database)}, {"username", std::move(user)},
        {"password", std::move(password)}, {"host", std::move(host)},
        {"port", std::move(port)}, {"ca_file", std::move(ca)},
        {"server_name", std::move(server)},
        {"timeout_ms", std::to_string(kConnectionTimeoutMs)}};
  }
  return true;
}

LRESULT ConnectionDialog::OnBrowse(WORD, WORD id, HWND, BOOL&) {
  if (owner_.ConnectionOpening()) return 0;
  const bool database = id == IDC_CONNECTION_DATABASE_BROWSE;
  const wchar_t* filter = database ? L"Database files\0*.db;*.sqlite;*.sqlite3\0All files\0*.*\0\0"
                 : L"CA certificates\0*.pem;*.crt;*.cer\0All files\0*.*\0\0";
  CFileDialog picker(TRUE, nullptr, nullptr,
      OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR,
      filter, m_hWnd);
  std::vector<wchar_t> path(kFileDialogPathCharacters, L'\0');
  picker.m_ofn.lpstrFile = path.data();
  picker.m_ofn.nMaxFile = static_cast<DWORD>(path.size());
  if (picker.DoModal() == IDOK) {
    SetDlgItemText(database ? IDC_CONNECTION_DATABASE : IDC_CONNECTION_CA,
                   path.data());
  } else if (::CommDlgExtendedError() != 0) {
    ShowError(L"Could not open the file picker.", 0);
  }
  return 0;
}

LRESULT ConnectionDialog::OnConnect(WORD, WORD, HWND, BOOL&) {
  if (owner_.ConnectionOpening()) return 0;
  if (CComboBox(GetDlgItem(IDC_CONNECTION_PROFILE)).GetCurSel() > 0 && !ApplyProfile())
    return 0;
  ConnectionProfile profile;
  if (!ReadProfile(&profile)) return 0;
  ClearError();
  if (SetTimer(kConnectionTimer, kConnectionPollMs) == 0) {
    ShowError(L"Could not start the connection. Close this dialog and try again.", 0);
    return 0;
  }
  std::string error;
  if (!owner_.BeginOpenConnection(std::move(profile), &error)) {
    KillTimer(kConnectionTimer);
    ShowError(ErrorText(error), 0);
    return 0;
  }
  SetConnecting(true);
  return 0;
}

void ConnectionDialog::SetConnecting(bool connecting) {
  for (int id : {IDC_CONNECTION_PROFILE, IDC_CONNECTION_DRIVER,
       IDC_CONNECTION_CONFIG_LOAD,
       IDC_CONNECTION_NAME, IDC_CONNECTION_DATABASE, IDC_CONNECTION_DATABASE_BROWSE,
       IDC_CONNECTION_HOST, IDC_CONNECTION_PORT, IDC_CONNECTION_USER,
       IDC_CONNECTION_PASSWORD, IDC_CONNECTION_CA, IDC_CONNECTION_CA_BROWSE,
       IDC_CONNECTION_SERVER, IDOK, IDCANCEL}) {
    const bool network_field = id == IDC_CONNECTION_HOST || id == IDC_CONNECTION_PORT ||
        id == IDC_CONNECTION_USER || id == IDC_CONNECTION_PASSWORD ||
        id == IDC_CONNECTION_CA || id == IDC_CONNECTION_CA_BROWSE || id == IDC_CONNECTION_SERVER;
    const bool applicable = network_field ? current_driver_ != 0 :
        id != IDC_CONNECTION_DATABASE_BROWSE || current_driver_ == 0;
    GetDlgItem(id).EnableWindow(!connecting && applicable);
  }
  ::EnableMenuItem(GetSystemMenu(FALSE), SC_CLOSE,
                   MF_BYCOMMAND | (connecting ? MF_GRAYED : MF_ENABLED));
  if (connecting)
    SetDlgItemText(IDC_CONNECTION_HINT,
        L"Opening the connection and loading schemas... This attempt cannot be cancelled.");
  else
    UpdateHint();
}

LRESULT ConnectionDialog::OnConnectionTimer(UINT, WPARAM timer, LPARAM, BOOL& handled) {
  if (timer != kConnectionTimer) { handled = FALSE; return 0; }
  if (!owner_.ConnectionOpenReady()) return 0;
  KillTimer(kConnectionTimer);
  std::string error;
  const bool opened = owner_.CompleteOpenConnection(&error);
  SetConnecting(false);
  if (opened) {
    const int selected = CComboBox(GetDlgItem(IDC_CONNECTION_PROFILE)).GetCurSel();
    if (selected > 0) owner_.RememberConnection(static_cast<std::size_t>(selected - 1));
    EndDialog(IDOK);
  } else ShowError(ErrorText(error), 0);
  return 0;
}

LRESULT ConnectionDialog::OnCancel(WORD, WORD, HWND, BOOL&) {
  if (!owner_.ConnectionOpening()) EndDialog(IDCANCEL);
  return 0;
}

LRESULT ConnectionDialog::OnClose(UINT, WPARAM, LPARAM, BOOL&) {
  if (!owner_.ConnectionOpening()) EndDialog(IDCANCEL);
  return 0;
}

LRESULT ConnectionDialog::OnDestroy(UINT, WPARAM, LPARAM, BOOL& handled) {
  KillTimer(kConnectionTimer);
  handled = FALSE;
  return 0;
}

}  // namespace turbodb::app
