add_executable(turbodb-postgresql main.c)
target_link_libraries(turbodb-postgresql PRIVATE turbodb_dbtool_core)
set_target_properties(turbodb-postgresql PROPERTIES FOLDER "dbtools")
if(MSVC)
  target_compile_options(turbodb-postgresql PRIVATE /W4 /WX)
else()
  target_compile_options(turbodb-postgresql PRIVATE -Wall -Wextra -Wpedantic
                                                    -Werror)
endif()
if(WIN32)
  add_custom_command(
    TARGET turbodb-postgresql
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:turbodb-postgresql>
      $<TARGET_FILE_DIR:turbodb-postgresql>
    COMMAND_EXPAND_LISTS)
else()
  set_property(TARGET turbodb-postgresql PROPERTY INSTALL_RPATH
                                                  "$ORIGIN/../lib")
endif()
install(TARGETS turbodb-postgresql RUNTIME_DEPENDENCY_SET
                DbToolsRuntimeDependencies
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT dbtools)
