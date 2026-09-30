#include "editor/scintilla_runtime.h"

namespace turbodb::app {

ScintillaRuntime::~ScintillaRuntime() {
  create_lexer_ = nullptr;
  if (lexilla_module_ != nullptr) {
    ::FreeLibrary(lexilla_module_);
    lexilla_module_ = nullptr;
  }
  if (scintilla_module_ != nullptr) {
    ::FreeLibrary(scintilla_module_);
    scintilla_module_ = nullptr;
  }
}

bool ScintillaRuntime::Initialize() noexcept {
  if (create_lexer_ != nullptr && scintilla_module_ != nullptr &&
      lexilla_module_ != nullptr) {
    return true;
  }

  scintilla_module_ = ::LoadLibraryW(L"Scintilla.dll");
  if (scintilla_module_ == nullptr) return false;

  lexilla_module_ = ::LoadLibraryW(L"Lexilla.dll");
  if (lexilla_module_ == nullptr) {
    ::FreeLibrary(scintilla_module_);
    scintilla_module_ = nullptr;
    return false;
  }

  create_lexer_ = reinterpret_cast<Lexilla::CreateLexerFn>(
      ::GetProcAddress(lexilla_module_, LEXILLA_CREATELEXER));
  if (create_lexer_ == nullptr) {
    ::FreeLibrary(lexilla_module_);
    ::FreeLibrary(scintilla_module_);
    lexilla_module_ = nullptr;
    scintilla_module_ = nullptr;
    return false;
  }

  return true;
}

Scintilla::ILexer5* ScintillaRuntime::CreateLexer(
    const char* name) const noexcept {
  if (create_lexer_ == nullptr || name == nullptr) return nullptr;
  return create_lexer_(name);
}

}  // namespace turbodb::app
