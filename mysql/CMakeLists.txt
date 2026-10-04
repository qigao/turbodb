include(GNUInstallDirs)
add_library(turbodb_mysql STATIC
  session.c
  transaction_control.c
  parameters.c
  cursor_row.c
  row_reader.c)
add_library(TurboDB::MySQL ALIAS turbodb_mysql)
add_subdirectory(wire)
add_subdirectory(auth)

target_compile_features(turbodb_mysql PUBLIC c_std_11)
target_include_directories(turbodb_mysql PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/mysql>)
target_link_libraries(turbodb_mysql
  PUBLIC Salts::Core Salts::CSerde
  PRIVATE Salts::CNet)
set_target_properties(turbodb_mysql PROPERTIES
  EXPORT_NAME MySQL
  POSITION_INDEPENDENT_CODE ON
  C_VISIBILITY_PRESET hidden
  FOLDER "mysql")

install(TARGETS turbodb_mysql EXPORT TurboDBTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT MySQL)
install(FILES turbodb_mysql.h session.h session_script.h session_async.h source.h session_cursor.h
  session_transaction.h cursor_row.h row_reader.h parameters.h
  transaction_control.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mysql COMPONENT MySQL)

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
