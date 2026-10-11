add_library(turbodb_dbtool_core STATIC
  src/dbtool_cli.c src/dbtool_file.c src/dbtool_plugin.c)

target_compile_features(turbodb_dbtool_core PUBLIC c_std_11)
target_include_directories(turbodb_dbtool_core
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(turbodb_dbtool_core PUBLIC
  turbodb_schema_support Salts::Core Salts::Plugin
  PRIVATE Salts::CmdParser)
set_target_properties(
  turbodb_dbtool_core
  PROPERTIES FOLDER "dbtools"
             VERSION ${PROJECT_VERSION}
             SOVERSION ${PROJECT_VERSION_MAJOR})

if(MSVC)
  target_compile_options(turbodb_dbtool_core PRIVATE /W4 /WX)
else()
  target_compile_options(turbodb_dbtool_core PRIVATE -Wall -Wextra -Wpedantic
                                                     -Werror)
endif()

add_executable(dbtools main.c)
target_link_libraries(dbtools PRIVATE turbodb_dbtool_core)
set_target_properties(dbtools PROPERTIES FOLDER "dbtools")
if(MSVC)
  target_compile_options(dbtools PRIVATE /W4 /WX)
else()
  target_compile_options(dbtools PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
if(NOT WIN32)
  set_property(TARGET dbtools PROPERTY INSTALL_RPATH "$ORIGIN/../lib")
endif()
install(TARGETS dbtools RUNTIME_DEPENDENCY_SET DbToolsRuntimeDependencies
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT dbtools)

if(TURBODB_BUILD_ORM)
  add_dependencies(dbtools orm_sqlite_driver orm_postgresql_driver orm_mysql_driver)
  # A dbtools component install must include the modules it consumes. Their
  # native/Salts dependencies remain provided by the deployment environment.
  install(TARGETS orm_sqlite_driver orm_postgresql_driver orm_mysql_driver
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/turbodb/drivers COMPONENT dbtools
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/turbodb/drivers COMPONENT dbtools)
  install(TARGETS turbo_orm
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT dbtools
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dbtools)
endif()

if(BUILD_TESTS OR BUILD_E2E_TESTS)
  add_subdirectory(tests)
endif()
