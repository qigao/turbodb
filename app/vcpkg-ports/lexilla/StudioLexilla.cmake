cmake_minimum_required(VERSION 3.24)
project(StudioLexilla LANGUAGES CXX RC)

# Same original lexer and lexlib translation units as pinned src/makefile at
# 1dc209de60c86cc9af615b6ebfa8466e5db4a931.
file(GLOB lexilla_lexlib CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/lexlib/*.cxx")
file(GLOB lexilla_lexers CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/lexers/Lex*.cxx")
add_library(Lexilla SHARED
  src/Lexilla.cxx src/LexillaVersion.rc
  ${lexilla_lexlib} ${lexilla_lexers})
target_include_directories(Lexilla PRIVATE include lexlib
  "${SCINTILLA_INCLUDE_DIR}")
target_compile_features(Lexilla PRIVATE cxx_std_17)
target_compile_definitions(Lexilla PRIVATE NDEBUG)
if(MINGW)
  target_link_options(Lexilla PRIVATE -static)
endif()
# MinGW prefixes shared DLLs with "lib" unless the runtime name is
# explicitly fixed. The existing Studio loader calls LoadLibraryW with the
# exact public name "Lexilla.dll"; no alias/fallback at runtime.
set_target_properties(Lexilla PROPERTIES OUTPUT_NAME Lexilla PREFIX "")
install(TARGETS Lexilla RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
