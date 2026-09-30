add_executable(turbodb-mysql main.c)
target_link_libraries(turbodb-mysql PRIVATE turbodb_dbtool_core)
set_target_properties(turbodb-mysql PROPERTIES FOLDER "dbtools")
if(NOT WIN32)
  set_property(TARGET turbodb-mysql PROPERTY INSTALL_RPATH "$ORIGIN/../lib")
endif()
install(TARGETS turbodb-mysql RUNTIME_DEPENDENCY_SET DbToolsRuntimeDependencies
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT dbtools)
