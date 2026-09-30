if(NOT WIN32)
  message(FATAL_ERROR "TurboDB Studio is supported only on Windows")
endif()

if(NOT TARGET Orm::Cpp)
  message(FATAL_ERROR "TurboDB Studio requires the in-tree Orm::Cpp target")
endif()

# The WTL, Scintilla, and Lexilla vcpkg ports do not expose one uniform CMake
# package contract. Keep the adaptation local to the app so no GUI dependency
# leaks into TurboDB libraries or dbtools.
find_path(TURBODB_APP_WTL_INCLUDE_DIR
          NAMES atlapp.h)
find_path(TURBODB_APP_SCINTILLA_INCLUDE_DIR
          NAMES Scintilla.h
          PATH_SUFFIXES scintilla)
find_library(TURBODB_APP_SCINTILLA_LIBRARY
             NAMES Scintilla scintilla)
find_path(TURBODB_APP_LEXILLA_INCLUDE_DIR
          NAMES Lexilla.h
          PATH_SUFFIXES lexilla)
find_library(TURBODB_APP_LEXILLA_LIBRARY
             NAMES Lexilla lexilla)

foreach(required_var
        TURBODB_APP_WTL_INCLUDE_DIR
        TURBODB_APP_SCINTILLA_INCLUDE_DIR
        TURBODB_APP_SCINTILLA_LIBRARY
        TURBODB_APP_LEXILLA_INCLUDE_DIR
        TURBODB_APP_LEXILLA_LIBRARY)
  if(NOT ${required_var})
    message(FATAL_ERROR
      "TurboDB Studio dependency ${required_var} was not resolved; configure with the vcpkg app feature")
  endif()
endforeach()

add_library(turbodb_app_editor_deps INTERFACE)
target_include_directories(
  turbodb_app_editor_deps
  INTERFACE
    "${TURBODB_APP_WTL_INCLUDE_DIR}"
    "${TURBODB_APP_SCINTILLA_INCLUDE_DIR}"
    "${TURBODB_APP_LEXILLA_INCLUDE_DIR}")
target_link_libraries(
  turbodb_app_editor_deps
  INTERFACE
    "${TURBODB_APP_SCINTILLA_LIBRARY}"
    "${TURBODB_APP_LEXILLA_LIBRARY}")

add_executable(
  turbodb-studio
  WIN32
  src/main.cpp
  src/main_window.cpp
  src/main_window.h
  src/connection/connection_manager.cpp
  src/connection/connection_manager.h
  src/editor/scintilla_runtime.cpp
  src/editor/scintilla_runtime.h
  src/editor/sql_editor.cpp
  src/editor/sql_editor.h
  src/explorer/schema_explorer_model.cpp
  src/explorer/schema_explorer_model.h
  src/explorer/schema_explorer_view.cpp
  src/explorer/schema_explorer_view.h
  src/language/sql_language_service.cpp
  src/language/sql_language_service.h
  src/workspace/sql_workspace_session.h)

target_include_directories(
  turbodb-studio
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_features(turbodb-studio PRIVATE cxx_std_17)
target_compile_definitions(
  turbodb-studio
  PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX)
target_link_libraries(
  turbodb-studio
  PRIVATE
    Orm::Cpp
    turbodb_app_editor_deps
    comctl32
    ole32)

set_target_properties(
  turbodb-studio
  PROPERTIES
    FOLDER "app"
    OUTPUT_NAME "TurboDBStudio")

add_executable(
  turbodb-app-language-test
  EXCLUDE_FROM_ALL
  tests/sql_language_service_test.cpp
  src/connection/connection_manager.cpp
  src/connection/connection_manager.h
  src/explorer/schema_explorer_model.cpp
  src/explorer/schema_explorer_model.h
  src/language/sql_language_service.cpp
  src/language/sql_language_service.h
  src/workspace/sql_workspace_session.h)
target_compile_features(turbodb-app-language-test PRIVATE cxx_std_17)
target_include_directories(
  turbodb-app-language-test
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(turbodb-app-language-test PRIVATE Orm::C)
target_compile_definitions(
  turbodb-app-language-test
  PRIVATE
    TURBODB_APP_SQLITE_PLUGIN_PATH="$<TARGET_FILE:orm_sqlite_driver>")
add_dependencies(turbodb-app-language-test orm_sqlite_driver)
set_target_properties(turbodb-app-language-test PROPERTIES FOLDER "app/tests")
