enable_language(RC)

add_subdirectory("${PROJECT_SOURCE_DIR}/external/OpenWTL"
                 "${CMAKE_CURRENT_BINARY_DIR}/OpenWTL" EXCLUDE_FROM_ALL SYSTEM)

# The editor ports expose headers and libraries rather than CMake packages.
find_path(TURBODB_APP_SCINTILLA_INCLUDE_DIR
          NAMES Scintilla.h
          PATH_SUFFIXES scintilla REQUIRED)
find_library(TURBODB_APP_SCINTILLA_LIBRARY
             NAMES Scintilla scintilla REQUIRED)
find_path(TURBODB_APP_LEXILLA_INCLUDE_DIR
          NAMES Lexilla.h
          PATH_SUFFIXES lexilla REQUIRED)
find_library(TURBODB_APP_LEXILLA_LIBRARY
             NAMES Lexilla lexilla REQUIRED)



add_library(turbodb_app_editor_deps INTERFACE)
target_include_directories(
  turbodb_app_editor_deps
  INTERFACE
    "${TURBODB_APP_SCINTILLA_INCLUDE_DIR}"
    "${TURBODB_APP_LEXILLA_INCLUDE_DIR}")
target_link_libraries(
  turbodb_app_editor_deps
  INTERFACE
    "${TURBODB_APP_SCINTILLA_LIBRARY}"
    "${TURBODB_APP_LEXILLA_LIBRARY}")

set(turbodb_app_window_sources
  src/studio.manifest
  src/command_ids.h
  src/command_toolbar.cpp
  src/command_toolbar.h
  src/main_window.cpp
  src/main_window.h
  src/connection/connection_manager.cpp
  src/connection/connection_manager.h
  src/connection/connection_config.cpp
  src/connection/connection_config.h
  src/connection/connection_dialog.cpp
  src/connection/connection_dialog.h
  src/connection/connection_dialog.rc
  src/connection/connection_resource.h
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
  src/plan/execution_plan_model.cpp
  src/plan/execution_plan_model.h
  src/plan/explain_controller.cpp
  src/plan/explain_controller.h
  src/query/query_controller.cpp
  src/query/query_controller.h
  src/query/query_result_model.cpp
  src/query/query_result_model.h
  src/results/result_workspace_view.cpp
  src/results/result_workspace_view.h
  src/studio_colors.h
  src/clipboard.h
  src/workspace/query_notebook.cpp
  src/workspace/query_notebook.h
  src/workspace/sql_workspace_session.h)

add_executable(turbodb-studio WIN32 src/main.cpp ${turbodb_app_window_sources})

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
    OpenWTL::WTL
    Salts::IniParser
    turbodb_app_editor_deps)

set_target_properties(
  turbodb-studio
  PROPERTIES
    FOLDER "app"
    OUTPUT_NAME "TurboDBStudio")

if(BUILD_TESTS)
  cmake_add_test(NAME turbodb-app-config-test TEST_NAME turbodb-app-config
    SOURCES tests/connection_config_test.cpp src/connection/connection_config.cpp
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/src"
    DEFS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    LIBS Salts::IniParser Salts::TinyTest
    FOLDER "app/tests")

  cmake_add_test(NAME turbodb-app-menu-test TEST_NAME turbodb-app-menu
    SOURCES tests/main_window_menu_test.cpp ${turbodb_app_window_sources}
    INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/src"
    DEFS UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
      "TURBODB_APP_SQLITE_PLUGIN_PATH=\"$<TARGET_FILE:orm_sqlite_driver>\""
    LIBS Orm::Cpp OpenWTL::WTL turbodb_app_editor_deps Salts::TinyTest Salts::IniParser
    FOLDER "app/tests")
  add_dependencies(turbodb-app-menu-test orm_sqlite_driver)
  set_tests_properties(turbodb-app-menu PROPERTIES TIMEOUT 60)

  add_executable(
    turbodb-app-language-test
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

  add_executable(
    turbodb-app-query-test
    tests/query_controller_test.cpp
    src/connection/connection_manager.cpp
    src/connection/connection_manager.h
    src/plan/execution_plan_model.cpp
    src/plan/execution_plan_model.h
    src/plan/explain_controller.cpp
    src/plan/explain_controller.h
    src/query/query_controller.cpp
    src/query/query_controller.h
    src/query/query_result_model.cpp
    src/query/query_result_model.h
    src/workspace/sql_workspace_session.h)
  target_compile_features(turbodb-app-query-test PRIVATE cxx_std_17)
  target_compile_definitions(
    turbodb-app-query-test
    PRIVATE
      WIN32_LEAN_AND_MEAN
      NOMINMAX)
  target_include_directories(
    turbodb-app-query-test
    PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(turbodb-app-query-test PRIVATE Orm::C)
  target_compile_definitions(
    turbodb-app-query-test
    PRIVATE
      TURBODB_APP_SQLITE_PLUGIN_PATH="$<TARGET_FILE:orm_sqlite_driver>")
  add_dependencies(turbodb-app-query-test orm_sqlite_driver)
  set_target_properties(turbodb-app-query-test PROPERTIES FOLDER "app/tests")

  add_test(NAME turbodb-app-language COMMAND turbodb-app-language-test)
  add_test(NAME turbodb-app-query COMMAND turbodb-app-query-test)
  add_test(NAME turbodb-app-startup COMMAND turbodb-studio --self-test)
endif()
