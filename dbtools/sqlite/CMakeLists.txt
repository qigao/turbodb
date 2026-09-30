add_executable(turbodb-sqlite main.c)
target_compile_features(turbodb-sqlite PRIVATE c_std_11)
target_link_libraries(turbodb-sqlite PRIVATE turbodb_dbtool_core)
set_target_properties(turbodb-sqlite PROPERTIES FOLDER "dbtools")
if(MSVC)
  target_compile_options(turbodb-sqlite PRIVATE /W4 /WX /std:c11)
else()
  target_compile_options(turbodb-sqlite PRIVATE -Wall -Wextra -Wpedantic
                                                -Werror)
endif()
if(WIN32)
  add_custom_command(
    TARGET turbodb-sqlite
    POST_BUILD
    COMMAND
      ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:turbodb-sqlite> $<TARGET_FILE_DIR:turbodb-sqlite>
    COMMAND_EXPAND_LISTS)
else()
  set_property(TARGET turbodb-sqlite PROPERTY INSTALL_RPATH "$ORIGIN/../lib")
endif()
install(TARGETS turbodb-sqlite RUNTIME_DEPENDENCY_SET
                DbToolsRuntimeDependencies
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT dbtools)
