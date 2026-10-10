add_executable(turbodb-sqlite main.c)
target_link_libraries(turbodb-sqlite PRIVATE turbodb_dbtool_core)
set_target_properties(turbodb-sqlite PROPERTIES FOLDER "dbtools")
if(MSVC)
  target_compile_options(turbodb-sqlite PRIVATE /W4 /WX)
else()
  target_compile_options(turbodb-sqlite PRIVATE -Wall -Wextra -Wpedantic
                                                -Werror)
endif()
if(NOT WIN32)
  set_property(TARGET turbodb-sqlite PROPERTY INSTALL_RPATH "$ORIGIN/../lib")
endif()
install(TARGETS turbodb-sqlite RUNTIME_DEPENDENCY_SET
                DbToolsRuntimeDependencies
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT dbtools)
