#pragma once

#include <windows.h>

#include <ILexer.h>
#include <Lexilla.h>

namespace turbodb::app {

class ScintillaRuntime {
 public:
  ScintillaRuntime() = default;
  ScintillaRuntime(const ScintillaRuntime&) = delete;
  ScintillaRuntime& operator=(const ScintillaRuntime&) = delete;
  ~ScintillaRuntime();

  bool Initialize() noexcept;
  Scintilla::ILexer5* CreateLexer(const char* name) const noexcept;

 private:
  HMODULE scintilla_module_ = nullptr;
  HMODULE lexilla_module_ = nullptr;
  Lexilla::CreateLexerFn create_lexer_ = nullptr;
};

}  // namespace turbodb::app
