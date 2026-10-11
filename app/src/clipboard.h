#pragma once

#include <windows.h>
#include <cstring>
#include <string_view>

namespace turbodb::app {

constexpr std::size_t kClipboardMaxChars = (16u * 1024u * 1024u) / sizeof(wchar_t) - 1;

// Windows clipboard adapter: ownership transfers only on SetClipboardData success.
inline bool CopyDisplayText(HWND owner, std::wstring_view text) {
  if (text.size() > kClipboardMaxChars) return false;
  const auto bytes = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL block = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (block == nullptr) return false;
  auto* data = static_cast<wchar_t*>(::GlobalLock(block));
  if (data == nullptr) { ::GlobalFree(block); return false; }
  std::memcpy(data, text.data(), text.size() * sizeof(wchar_t));
  data[text.size()] = L'\0';
  ::GlobalUnlock(block);
  bool copied = false;
  if (::OpenClipboard(owner)) {
    copied = ::EmptyClipboard() && ::SetClipboardData(CF_UNICODETEXT, block) != nullptr;
    ::CloseClipboard();
  }
  if (!copied) ::GlobalFree(block);
  return copied;
}

}  // namespace turbodb::app
