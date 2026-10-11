#pragma once

#include <string>
#include <vector>

#include <vstr.h>

#include "workspace/sql_workspace_session.h"

namespace turbodb::app {

// Native UTF-16 form values. Parsing never loads a driver or opens a database.
// Passwords are resolved when selecting/connecting and never kept in this owner.
struct ConnectionPreset {
  std::wstring name;
  SqlProvider provider = SqlProvider::unknown;
  std::wstring module_path, display_name, database, host, port, user;
  std::wstring ca_certificate, tls_server_name, password_env;
};

struct ConnectionConfig {
  std::wstring source_path;
  std::vector<ConnectionPreset> profiles;
  std::size_t default_profile = 0;
};

// Accepts UTF-8 INI (optional BOM), up to 64 KiB and 32 profiles. Relative file
// paths use the absolute config_path's directory. Outputs change only on success;
// errors contain schema diagnostics, never configuration values or credentials.
// output and error are required; null pointers return false.
bool ParseConnectionConfig(vstr text, const std::wstring& config_path,
                           ConnectionConfig* output, std::wstring* error);
// Uses bounded native UTF-16 file reads. optional permits an absent default file;
// other read/parse failures remain errors and preserve the previous configuration.
bool LoadConnectionConfig(const std::wstring& path, ConnectionConfig* output,
                          std::wstring* error, bool optional = false);
std::wstring DefaultConnectionConfigPath();
// Manual entry uses the default matching preset, then the first matching preset,
// otherwise the provider's standard DLL beside the executable. No load/search is
// performed and configured modules are never substituted after a load failure.
// Unknown providers or failure to locate the executable return an empty path.
std::wstring DriverModulePath(const ConnectionConfig& config, SqlProvider provider);
// Missing named environment variables fail; an omitted password_env means empty.
bool ResolvePresetPassword(const ConnectionPreset& preset, std::wstring* password,
                           std::wstring* error);

}  // namespace turbodb::app
