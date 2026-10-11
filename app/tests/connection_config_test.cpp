#include "connection/connection_config.h"
#include "connection_config_fixture.h"

#include <windows.h>

#include <cstring>
#include <tinytest.hpp>

namespace {
using namespace turbodb::app;
constexpr wchar_t kConfigPath[] = L"C:\\settings\\TurboDBStudio.ini";
constexpr char kSqlite[] =
    "[connection.local]\n"
    "driver=sqlite\nmodule_path=drivers/sqlite.dll\ndatabase=:memory:\n";

}  // namespace

spec("Studio connection configuration") {
  it("derives standard provider modules only from the executable directory") {
    ConnectionConfig config;
    const auto ini = DefaultConnectionConfigPath();
    check_false(ini.empty());
    const auto directory = ini.substr(0, ini.find_last_of(L"/\\") + 1);
    check_true(DriverModulePath(config, SqlProvider::sqlite) == directory + L"turbodb_driver_sqlite.dll");
    check_true(DriverModulePath(config, SqlProvider::postgresql) == directory + L"turbodb_driver_postgresql.dll");
    check_true(DriverModulePath(config, SqlProvider::mysql) == directory + L"turbodb_driver_mysql.dll");
    check_true(DriverModulePath(config, SqlProvider::unknown).empty());
  }

  it("prefers the default matching INI module then the first matching provider without probing files") {
    const char* text =
        "[studio]\ndefault_connection=second\n"
        "[connection.first]\ndriver=sqlite\nmodule_path=first.dll\ndatabase=:memory:\n"
        "[connection.second]\ndriver=sqlite\nmodule_path=absent.dll\ndatabase=:memory:\n"
        "[connection.network]\ndriver=mysql\nmodule_path=mysql.dll\ndatabase=db\nuser=reader\n"
        "ca_certificate=ca.pem\ntls_server_name=db.example\n";
    ConnectionConfig config;
    std::wstring error;
    check_true(ParseConnectionConfig(vstr_from_cstr(text), kConfigPath, &config, &error));
    check_true(DriverModulePath(config, SqlProvider::sqlite) == L"C:\\settings\\absent.dll");
    check_true(DriverModulePath(config, SqlProvider::mysql) == L"C:\\settings\\mysql.dll");
    config.default_profile = 2;
    check_true(DriverModulePath(config, SqlProvider::sqlite) == L"C:\\settings\\first.dll");
  }

  it("parses named profiles and selects the configured default without loading modules") {
    const char* text =
        "[studio]\ndefault_connection=production\n"
        "[connection.local]\ndriver=sqlite\nmodule_path=drivers/sqlite.dll\ndatabase=data/local.db\n"
        "[connection.production]\ndriver=postgresql\nmodule_path=drivers/postgres.dll\n"
        "database=warehouse\nuser=reader\nca_certificate=certs/ca.pem\ntls_server_name=db.example\n"
        "password_env=TURBODB_STUDIO_CONFIG_TEST_PASSWORD\n";
    ConnectionConfig config;
    std::wstring error;
    check_true(ParseConnectionConfig(vstr_from_cstr(text), kConfigPath, &config, &error));
    check_equal(config.profiles.size(), std::size_t{2});
    check_equal(config.default_profile, std::size_t{1});
    check_true(config.profiles[0].database == L"C:\\settings\\data\\local.db");
    const auto& network = config.profiles[1];
    check_true(network.provider == SqlProvider::postgresql);
    check_true(network.display_name == L"production");
    check_true(network.host == L"localhost");
    check_true(network.port == L"5432");
    check_true(network.ca_certificate == L"C:\\settings\\certs\\ca.pem");
    check_true(error.empty());
  }

  it("accepts UTF-8 BOM, Unicode labels, and the documented last-value INI semantics") {
    const char* text = "\xef\xbb\xbf[connection.local]\r\n"
        "driver=sqlite\r\nmodule_path=drivers/sqlite.dll\r\ndatabase=:memory:\r\n"
        "display_name=old\r\ndisplay_name=\xe6\x9c\xac\xe5\x9c\xb0\r\n";
    ConnectionConfig config;
    std::wstring error;
    check_true(ParseConnectionConfig(vstr_from_cstr(text), kConfigPath, &config, &error));
    check_true(config.profiles[0].display_name == L"\u672c\u5730");
    check_true(config.profiles[0].database == L":memory:");
  }

  it("rejects invalid schema, TLS settings, ports and paths without replacing the previous configuration") {
    const char* invalid[] = {
        "[connection.local]\ndriver=sqlite\ndatabase=:memory:\n",
        "[connection.local]\ndriver=other\nmodule_path=x.dll\ndatabase=x\n",
        "[connection.local]\ndriver=sqlite\nmodule_path=x.dll\ndatabase=x\npassword=secret-value\n",
        "[connection.local]\ndriver=sqlite\nmodule_path=C:x.dll\ndatabase=x\n",
        "[connection.local]\ndriver=sqlite\nmodule_path=/x.dll\ndatabase=x\n",
        "[connection.local]\ndriver=mysql\nmodule_path=x.dll\ndatabase=x\nuser=u\n",
        "[connection.local]\ndriver=mysql\nmodule_path=x.dll\ndatabase=x\nuser=u\n"
        "ca_certificate=ca.pem\ntls_server_name=db\nport=65536\n",
        "[studio]\ndefault_connection=missing\n[connection.local]\ndriver=sqlite\nmodule_path=x.dll\ndatabase=x\n",
        "[connection.local]\ndriver=sqlite\nmodule_path=x.dll\ndatabase=x\nunknown=value\n",
        "[studio]\nunknown=value\n",
        "[wrong]\ndriver=sqlite\n",
        "[connection.local\n"
    };
    ConnectionConfig config;
    std::wstring error;
    check_true(ParseConnectionConfig(vstr_from_cstr(kSqlite), kConfigPath, &config, &error));
    for (const char* text : invalid) {
      check_false(ParseConnectionConfig(vstr_from_cstr(text), kConfigPath, &config, &error));
      check_false(error.empty());
      check_true(error.find(L"secret-value") == std::wstring::npos);
      check_equal(config.profiles.size(), std::size_t{1});
      check_true(config.profiles[0].name == L"local");
    }
  }

  it("rejects invalid encodings, embedded NULs and oversized input") {
    ConnectionConfig config;
    std::wstring error;
    const char invalid_utf8[] = {'[', '\xff', ']'};
    check_false(ParseConnectionConfig(vstr_from_buf(invalid_utf8, sizeof(invalid_utf8)),
                                      kConfigPath, &config, &error));
    const char nul[] = {'[', 0, ']'};
    check_false(ParseConnectionConfig(vstr_from_buf(nul, sizeof(nul)), kConfigPath, &config, &error));
    const std::string large(65537, ' ');
    check_false(ParseConnectionConfig(vstr_from_buf(large.data(), large.size()),
                                      kConfigPath, &config, &error));
    const std::string field = std::string(kSqlite) + "display_name=" + std::string(4097, 'x') + "\n";
    check_false(ParseConnectionConfig(vstr_from_buf(field.data(), field.size()),
                                      kConfigPath, &config, &error));
  }

  it("accepts 32 profiles and rejects an additional profile atomically") {
    std::string text;
    for (int index = 0; index < 32; ++index)
      text += "[connection.local" + std::to_string(index) +
              "]\ndriver=sqlite\nmodule_path=x.dll\ndatabase=:memory:\n";
    ConnectionConfig config;
    std::wstring error;
    check_true(ParseConnectionConfig(vstr_from_buf(text.data(), text.size()),
                                     kConfigPath, &config, &error));
    check_equal(config.profiles.size(), std::size_t{32});
    text += "[connection.extra]\ndriver=sqlite\nmodule_path=x.dll\ndatabase=:memory:\n";
    check_false(ParseConnectionConfig(vstr_from_buf(text.data(), text.size()),
                                      kConfigPath, &config, &error));
    check_equal(config.profiles.size(), std::size_t{32});
  }

  it("reads Unicode filenames and keeps previous profiles after file failures") {
    ConnectionIniFile file;
    check_false(file.path.empty());
    check_true(file.path.find(L"\u8fde\u63a5") != std::wstring::npos);
    check_true(file.Write(kSqlite, std::strlen(kSqlite)));
    ConnectionConfig config;
    std::wstring error;
    check_true(LoadConnectionConfig(file.path, &config, &error));
    check_true(config.source_path == file.path);
    const std::string large(65537, ' ');
    check_true(file.Write(large.data(), large.size()));
    check_false(LoadConnectionConfig(file.path, &config, &error));
    check_equal(config.profiles.size(), std::size_t{1});
    check_true(LoadConnectionConfig(file.path + L".absent", &config, &error, true));
    check_false(LoadConnectionConfig(file.path + L".absent", &config, &error));
    check_true(config.source_path == file.path);
  }

  it("resolves passwords on demand and reports absent environment variables without their contents") {
    TestPassword restore;
    ConnectionPreset preset;
    preset.password_env = TestPassword::name;
    std::wstring password, error;
    check_true(::SetEnvironmentVariableW(TestPassword::name, L"test-secret") != FALSE);
    check_true(ResolvePresetPassword(preset, &password, &error));
    check_true(password == L"test-secret");
    check_true(::SetEnvironmentVariableW(TestPassword::name, nullptr) != FALSE);
    check_false(ResolvePresetPassword(preset, &password, &error));
    check_true(password.empty());
    check_true(error.find(TestPassword::name) != std::wstring::npos);
    check_true(error.find(L"Enter a password here") != std::wstring::npos);
    check_true(error.find(L"test-secret") == std::wstring::npos);
    preset.password_env.clear();
    check_true(ResolvePresetPassword(preset, &password, &error));
  }
}
