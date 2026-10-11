#include "connection/connection_config.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include <cmeta_fs.h>
#include <ini_parser.h>
#include <tstr.h>

namespace turbodb::app {
namespace {

constexpr std::size_t kMaxConfigBytes = 64 * 1024;
constexpr std::size_t kMaxProfiles = 32;
constexpr std::size_t kMaxFieldBytes = 4096;
constexpr DWORD kNativePathCharacters = 32768;
using TextOwner = std::unique_ptr<char, decltype(&tstr_free)>;

bool Fail(std::wstring* error, const wchar_t* message) {
  *error = message;
  return false;
}

bool Decode(vstr value, std::wstring* output) {
  if (value.len == 0) { output->clear(); return true; }
  if (value.len > kMaxFieldBytes) return false;
  const int count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
      value.data, static_cast<int>(value.len), nullptr, 0);
  if (count <= 0) return false;
  output->resize(static_cast<std::size_t>(count));
  return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data,
      static_cast<int>(value.len), output->data(), count) == count;
}

TextOwner EncodePath(const std::wstring& path) {
  TextOwner text(nullptr, tstr_free);
  if (path.empty() || path.size() >= kNativePathCharacters ||
      path.find(L'\0') != std::wstring::npos) return text;
  const int count = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
      path.data(), static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
  if (count <= 0) return text;
  text.reset(tstr_new_len(nullptr, static_cast<std::size_t>(count)));
  if (text && ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path.data(),
      static_cast<int>(path.size()), text.get(), count, nullptr, nullptr) != count)
    text.reset();
  return text;
}

bool AbsolutePath(const std::wstring& path) {
  const bool drive = path.size() >= 3 &&
      ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
      path[1] == L':' && (path[2] == L'/' || path[2] == L'\\');
  const bool unc = path.size() >= 2 &&
      ((path[0] == L'\\' && path[1] == L'\\') || (path[0] == L'/' && path[1] == L'/'));
  auto encoded = EncodePath(path);
  return (drive || unc) && encoded && cmeta_fs_path_is_absolute(encoded.get());
}

bool ResolvePath(std::wstring* path, const std::wstring& config_path) {
  auto encoded = EncodePath(*path);
  if (!encoded) return false;
  const bool unc = path->size() >= 2 &&
      (((*path)[0] == L'\\' && (*path)[1] == L'\\') ||
       ((*path)[0] == L'/' && (*path)[1] == L'/'));
  if (!unc && (path->front() == L'/' || path->front() == L'\\')) return false;
  if (!AbsolutePath(*path)) {
    // Drive-relative and root-relative paths have process-dependent semantics.
    if (path->front() == L'/' || path->front() == L'\\' ||
        path->find(L':') != std::wstring::npos)
      return false;
    const auto separator = config_path.find_last_of(L"/\\");
    if (separator == std::wstring::npos) return false;
    *path = config_path.substr(0, separator + 1) + *path;
  }
  if (path->size() >= kNativePathCharacters) return false;
  std::wstring full(kNativePathCharacters, L'\0');
  const DWORD size = ::GetFullPathNameW(path->c_str(), kNativePathCharacters,
                                       full.data(), nullptr);
  if (size == 0 || size >= kNativePathCharacters || size > kMaxFieldBytes)
    return false;
  full.resize(size);
  *path = std::move(full);
  return true;
}

struct Field {
  const char* key;
  std::wstring ConnectionPreset::* member;
  bool network;
};
constexpr Field kFields[] = {
    {"module_path", &ConnectionPreset::module_path, false},
    {"display_name", &ConnectionPreset::display_name, false},
    {"database", &ConnectionPreset::database, false},
    {"host", &ConnectionPreset::host, true},
    {"port", &ConnectionPreset::port, true},
    {"user", &ConnectionPreset::user, true},
    {"ca_certificate", &ConnectionPreset::ca_certificate, true},
    {"tls_server_name", &ConnectionPreset::tls_server_name, true},
    {"password_env", &ConnectionPreset::password_env, true}};

bool Is(vstr value, const char* expected) {
  return vstr_eq(value, vstr_from_cstr(expected)) != 0;
}

}  // namespace

bool ParseConnectionConfig(vstr text, const std::wstring& config_path,
                           ConnectionConfig* output, std::wstring* error) {
  if (output == nullptr || error == nullptr) return false;
  error->clear();
  if (!AbsolutePath(config_path))
    return Fail(error, L"The configuration file needs an absolute path.");
  if (text.data == nullptr || text.len == 0 || text.len > kMaxConfigBytes ||
      std::memchr(text.data, 0, text.len) != nullptr)
    return Fail(error, L"The INI file must contain 1 to 65536 bytes of UTF-8 text.");
  if (::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data,
        static_cast<int>(text.len), nullptr, 0) <= 0)
    return Fail(error, L"The INI file is not valid UTF-8.");
  if (text.len >= 3 && std::memcmp(text.data, "\xef\xbb\xbf", 3) == 0) {
    text.data += 3;
    text.len -= 3;
  }
  // The lexer requires a readable sentinel beyond the explicit input length.
  TextOwner source(tstr_new_len(text.data, text.len), tstr_free);
  if (!source) return Fail(error, L"Could not allocate configuration text.");
  std::unique_ptr<ini_t, decltype(&ini_free)> ini(
      ini_parse(source.get(), text.len), ini_free);
  if (!ini) return Fail(error, L"Could not parse the INI configuration.");

  ConnectionConfig parsed;
  parsed.source_path = config_path;
  std::wstring default_name;
  for (std::size_t section_index = 0; section_index < ini_section_count(ini.get());
       ++section_index) {
    const char* section = ini_section_name(ini.get(), section_index);
    const vstr name = ini_section_name_v(ini.get(), section_index);
    if (Is(name, "studio")) {
      for (std::size_t key = 0; key < ini_key_count(ini.get(), section); ++key)
        if (!Is(ini_key_name_v(ini.get(), name, key), "default_connection"))
          return Fail(error, L"Unknown key in [studio].");
      if (!Decode(ini_get_v(ini.get(), section, "default_connection"), &default_name))
        return Fail(error, L"Invalid default_connection value.");
      continue;
    }
    constexpr char prefix[] = "connection.";
    if (name.len <= sizeof(prefix) - 1 ||
        std::memcmp(name.data, prefix, sizeof(prefix) - 1) != 0)
      return Fail(error, L"Use [studio] or [connection.NAME] sections.");
    if (parsed.profiles.size() >= kMaxProfiles)
      return Fail(error, L"An INI file supports at most 32 connections.");
    ConnectionPreset preset;
    if (!Decode(vstr_from_buf(name.data + sizeof(prefix) - 1,
                             name.len - sizeof(prefix) + 1), &preset.name))
      return Fail(error, L"Invalid connection profile name.");
    const vstr driver = ini_get_v(ini.get(), section, "driver");
    if (Is(driver, "sqlite")) preset.provider = SqlProvider::sqlite;
    else if (Is(driver, "postgresql")) preset.provider = SqlProvider::postgresql;
    else if (Is(driver, "mysql")) preset.provider = SqlProvider::mysql;
    else return Fail(error, L"driver must be sqlite, postgresql, or mysql.");
    const bool network = preset.provider != SqlProvider::sqlite;
    for (std::size_t key_index = 0; key_index < ini_key_count(ini.get(), section);
         ++key_index) {
      const vstr key = ini_key_name_v(ini.get(), name, key_index);
      if (Is(key, "driver")) continue;
      const auto field = std::find_if(std::begin(kFields), std::end(kFields),
          [&](const Field& item) { return Is(key, item.key); });
      if (field == std::end(kFields) || (field->network && !network))
        return Fail(error, L"Unknown or unsupported connection key; use password_env for credentials.");
      if (!Decode(ini_get_v(ini.get(), section, field->key), &(preset.*field->member)))
        return Fail(error, L"A connection field is invalid or exceeds 4096 bytes.");
    }
    if (preset.module_path.empty() || preset.database.empty())
      return Fail(error, L"Each connection needs module_path and database.");
    if (!ResolvePath(&preset.module_path, config_path) ||
        (!network && preset.database != L":memory:" &&
         !ResolvePath(&preset.database, config_path)))
      return Fail(error, L"Invalid driver or database file path.");
    if (network) {
      if (preset.host.empty()) preset.host = L"localhost";
      if (preset.port.empty()) preset.port = preset.provider == SqlProvider::postgresql
          ? L"5432" : L"3306";
      if (preset.port.size() > 5)
        return Fail(error, L"port must contain at most five decimal digits.");
      unsigned port = 0;
      for (wchar_t character : preset.port) {
        if (character < L'0' || character > L'9' || port > 6553u)
          return Fail(error, L"port must be a decimal number from 1 to 65535.");
        port = port * 10u + character - L'0';
      }
      if (port == 0 || port > 65535u)
        return Fail(error, L"port must be a decimal number from 1 to 65535.");
      if (preset.user.empty() || preset.ca_certificate.empty() || preset.tls_server_name.empty())
        return Fail(error, L"Network connections need user, ca_certificate, and tls_server_name.");
      if (!ResolvePath(&preset.ca_certificate, config_path))
        return Fail(error, L"Invalid CA certificate file path.");
      if (preset.password_env.find(L'=') != std::wstring::npos)
        return Fail(error, L"Invalid password environment variable name.");
    }
    if (preset.display_name.empty()) preset.display_name = preset.name;
    parsed.profiles.push_back(std::move(preset));
  }
  if (parsed.profiles.empty()) return Fail(error, L"The INI file contains no connections.");
  if (!default_name.empty()) {
    const auto selected = std::find_if(parsed.profiles.begin(), parsed.profiles.end(),
        [&](const ConnectionPreset& preset) { return preset.name == default_name; });
    if (selected == parsed.profiles.end())
      return Fail(error, L"default_connection does not name a configured connection.");
    parsed.default_profile = static_cast<std::size_t>(selected - parsed.profiles.begin());
  }
  *output = std::move(parsed);
  return true;
}

bool LoadConnectionConfig(const std::wstring& path, ConnectionConfig* output,
                          std::wstring* error, bool optional) {
  if (output == nullptr || error == nullptr) return false;
  error->clear();
  if (!AbsolutePath(path))
    return Fail(error, L"Invalid configuration file path.");
  TextOwner content(tstr_new_len(nullptr, kMaxConfigBytes + 1), tstr_free);
  if (!content) return Fail(error, L"Could not allocate configuration text.");
  // Salts' current Windows stream open uses the ANSI CRT. This application
  // adapter preserves native file-picker paths, including Chinese characters.
  const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD status = ::GetLastError();
    if (optional && (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND))
      return true;
    return Fail(error, L"Could not open the INI configuration file.");
  }
  LARGE_INTEGER expected{};
  const bool bounded = ::GetFileType(file) == FILE_TYPE_DISK &&
      ::GetFileSizeEx(file, &expected) && expected.QuadPart > 0 &&
      expected.QuadPart <= static_cast<LONGLONG>(kMaxConfigBytes);
  std::size_t size = 0;
  bool readable = true;
  while (bounded && size <= kMaxConfigBytes) {
    DWORD read = 0;
    readable = ::ReadFile(file, content.get() + size,
        static_cast<DWORD>(kMaxConfigBytes + 1 - size), &read, nullptr) != FALSE;
    if (!readable || read == 0) break;
    size += read;
  }
  const bool closed = ::CloseHandle(file) != FALSE;
  if (!bounded)
    return Fail(error, L"The INI file must contain 1 to 65536 bytes of UTF-8 text.");
  if (!readable || !closed || size != static_cast<std::size_t>(expected.QuadPart))
    return Fail(error, L"The configuration could not be read completely or changed while reading.");
  return ParseConnectionConfig(vstr_from_buf(content.get(), size), path, output, error);
}

std::wstring DefaultConnectionConfigPath() {
  std::wstring path(kNativePathCharacters, L'\0');
  const DWORD size = ::GetModuleFileNameW(nullptr, path.data(), kNativePathCharacters);
  if (size == 0 || size >= kNativePathCharacters) return {};
  path.resize(size);
  const auto separator = path.find_last_of(L"/\\");
  if (separator == std::wstring::npos) return {};
  return path.substr(0, separator + 1) + L"TurboDBStudio.ini";
}

std::wstring DriverModulePath(const ConnectionConfig& config, SqlProvider provider) {
  if (config.default_profile < config.profiles.size() &&
      config.profiles[config.default_profile].provider == provider)
    return config.profiles[config.default_profile].module_path;
  for (const auto& preset : config.profiles)
    if (preset.provider == provider) return preset.module_path;
  const wchar_t* filename = nullptr;
  switch (provider) {
    case SqlProvider::sqlite: filename = L"turbodb_driver_sqlite.dll"; break;
    case SqlProvider::postgresql: filename = L"turbodb_driver_postgresql.dll"; break;
    case SqlProvider::mysql: filename = L"turbodb_driver_mysql.dll"; break;
    default: return {};
  }
  const auto config_path = DefaultConnectionConfigPath();
  const auto separator = config_path.find_last_of(L"/\\");
  if (separator == std::wstring::npos) return {};
  return config_path.substr(0, separator + 1) + filename;
}

bool ResolvePresetPassword(const ConnectionPreset& preset, std::wstring* password,
                           std::wstring* error) {
  if (password == nullptr || error == nullptr) return false;
  password->clear();
  error->clear();
  if (preset.password_env.empty()) return true;
  ::SetLastError(ERROR_SUCCESS);
  const DWORD needed = ::GetEnvironmentVariableW(preset.password_env.c_str(), nullptr, 0);
  if (needed == 0) {
    if (::GetLastError() == ERROR_SUCCESS) return true;
    *error = L"The password environment variable " + preset.password_env +
        L" is not set. Enter a password here, or set the variable before launching Studio.";
    return false;
  }
  if (needed > kMaxFieldBytes + 1) {
    *error = L"The password environment variable " + preset.password_env +
        L" exceeds the field limit. Use a shorter password.";
    return false;
  }
  std::wstring value(needed, L'\0');
  const DWORD size = ::GetEnvironmentVariableW(preset.password_env.c_str(), value.data(), needed);
  if (size == 0 || size >= needed) {
    *error = L"Could not read the password environment variable " + preset.password_env +
        L". Enter a password here, or set the variable before launching Studio.";
    return false;
  }
  value.resize(size);
  *password = std::move(value);
  return true;
}

}  // namespace turbodb::app
