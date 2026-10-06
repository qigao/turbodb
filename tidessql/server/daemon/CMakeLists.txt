add_library(tidessqld_runtime STATIC config.c runtime.c)
target_include_directories(tidessqld_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tidessqld_runtime PUBLIC tidessql_mysql_frontend
  PRIVATE Salts::TomlParser Salts::Core GmSSL::GmSSL)
cmake_config_target(tidessqld_runtime NO_INSTALL FOLDER "tidessql/server/daemon")

add_executable(tidessqld main.c)
target_compile_definitions(tidessqld PRIVATE TIDESSQLD_VERSION="${PROJECT_VERSION}")
target_link_libraries(tidessqld PRIVATE tidessqld_runtime Salts::Core GmSSL::GmSSL)
set_target_properties(tidessqld PROPERTIES FOLDER "tidessql/server/daemon")
if(MSVC)
  target_compile_options(tidessqld_runtime PRIVATE /W4 /WX)
  target_compile_options(tidessqld PRIVATE /W4 /WX)
else()
  target_compile_options(tidessqld_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
  target_compile_options(tidessqld PRIVATE -Wall -Wextra -Wpedantic -Werror)
  if(APPLE)
    set_property(TARGET tidessqld PROPERTY INSTALL_RPATH "@loader_path/../lib")
  else()
    set_property(TARGET tidessqld PROPERTY INSTALL_RPATH "$ORIGIN/../lib")
  endif()
endif()
if(WIN32)
  add_custom_command(TARGET tidessqld POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:tidessqld> $<TARGET_FILE_DIR:tidessqld>
    COMMAND_EXPAND_LISTS)
endif()
install(TARGETS tidessqld RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT tidessqld)
if(WIN32)
  install(FILES $<TARGET_RUNTIME_DLLS:tidessqld>
    DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT tidessqld)
endif()
