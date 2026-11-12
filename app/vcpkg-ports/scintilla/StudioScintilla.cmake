cmake_minimum_required(VERSION 3.24)
project(StudioScintilla LANGUAGES CXX RC)

# Identical platform/common translation units as upstream win32/makefile at
# 0583df2dc4a988bd91d892399de73373264c13a2. No bundled second copy.
file(GLOB scintilla_core CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cxx")
add_library(Scintilla SHARED
  ${scintilla_core}
  win32/HanjaDic.cxx
  win32/PlatWin.cxx
  win32/ListBox.cxx
  win32/SurfaceGDI.cxx
  win32/SurfaceD2D.cxx
  win32/ScintillaWin.cxx
  win32/ScintillaDLL.cxx
  win32/ScintRes.rc)
target_include_directories(Scintilla PRIVATE include src win32)
target_compile_features(Scintilla PRIVATE cxx_std_17)
target_compile_definitions(Scintilla PRIVATE NDEBUG)
target_link_libraries(Scintilla PRIVATE
  gdi32 user32 imm32 ole32 uuid oleaut32 advapi32)
if(MINGW)
  target_link_options(Scintilla PRIVATE -static)
endif()
# MinGW prefixes shared DLLs with "lib" unless the runtime name is
# explicitly fixed. The existing Studio loader calls LoadLibraryW with the
# exact public name "Scintilla.dll"; no alias/fallback at runtime.
set_target_properties(Scintilla PROPERTIES OUTPUT_NAME Scintilla PREFIX "")
install(TARGETS Scintilla RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
