#pragma once

#include <windows.h>
#include <string>

// Each fixture owns only its freshly created temporary file.
struct ConnectionIniFile {
  std::wstring path;
  ConnectionIniFile() {
    wchar_t directory[MAX_PATH]{}, file[MAX_PATH]{};
    if (!::GetTempPathW(MAX_PATH, directory) ||
        !::GetTempFileNameW(directory, L"tds", 0, file)) return;
    path = file;
    const auto unicode = path + L".\u8fde\u63a5.ini";
    if (::MoveFileW(path.c_str(), unicode.c_str())) path = unicode;
  }
  ConnectionIniFile(const ConnectionIniFile&) = delete;
  ConnectionIniFile& operator=(const ConnectionIniFile&) = delete;
  ~ConnectionIniFile() { if (!path.empty()) ::DeleteFileW(path.c_str()); }
  bool Write(const char* content, std::size_t size) {
    const HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = ::WriteFile(file, content, static_cast<DWORD>(size), &written, nullptr) &&
                    written == size;
    return ::CloseHandle(file) && ok;
  }
};

struct TestPassword {
  static constexpr const wchar_t* name = L"TURBODB_STUDIO_CONFIG_TEST_PASSWORD";
  std::wstring previous;
  bool existed;
  TestPassword() {
    ::SetLastError(ERROR_SUCCESS);
    const DWORD size = ::GetEnvironmentVariableW(name, nullptr, 0);
    existed = size != 0 || ::GetLastError() == ERROR_SUCCESS;
    if (size) {
      previous.resize(size);
      previous.resize(::GetEnvironmentVariableW(name, previous.data(), size));
    }
  }
  ~TestPassword() {
    ::SetEnvironmentVariableW(name, existed ? previous.c_str() : nullptr);
  }
};
