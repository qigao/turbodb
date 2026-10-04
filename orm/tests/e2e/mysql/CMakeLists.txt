cmake_add_test(
  NAME orm_mysql_driver_live_test
  FOLDER "orm/tests/e2e/mysql"
  TEST_NAME orm_mysql_driver_live
  SOURCES mysql_driver_live_test.c
  LIBS turbo_orm Salts::CFlow Salts::Platform
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)

add_dependencies(orm_mysql_driver_live_test orm_mysql_driver)
if(MSVC)
  target_compile_options(orm_mysql_driver_live_test PRIVATE /WX)
else()
  target_compile_options(orm_mysql_driver_live_test PRIVATE -Werror)
endif()

set_tests_properties(orm_mysql_driver_live PROPERTIES LABELS e2e RUN_SERIAL
                                                      TRUE)
if(WIN32)
  set_tests_properties(
    orm_mysql_driver_live
    PROPERTIES
      ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::DataBind>"
  )
endif()

set_tests_properties(
  orm_mysql_driver_live
  PROPERTIES ENVIRONMENT "ORM_MYSQL_PLUGIN=$<TARGET_FILE:orm_mysql_driver>")

cmake_add_test(
  NAME orm_mysql_live_test
  FOLDER "orm/tests/e2e/mysql"
  TEST_NAME orm_mysql_live
  SOURCES mysql_live_test.c ${ORM_MYSQL_SOURCE_DIR}/session.c
    ${ORM_MYSQL_SOURCE_DIR}/transaction_control.c
    ${ORM_SOURCE_DIR}/../drivers/mysql/cursor.c ${ORM_MYSQL_SOURCE_DIR}/cursor_row.c
    ${ORM_MYSQL_SOURCE_DIR}/row_reader.c ${ORM_MYSQL_SOURCE_DIR}/wire/codec.c
    ${ORM_MYSQL_SOURCE_DIR}/wire/packet.c ${ORM_MYSQL_SOURCE_DIR}/wire/query.c
    ${ORM_MYSQL_SOURCE_DIR}/wire/result.c ${ORM_MYSQL_SOURCE_DIR}/wire/handshake.c
    ${ORM_MYSQL_SOURCE_DIR}/wire/statement.c ${ORM_MYSQL_SOURCE_DIR}/wire/row.c
    ${ORM_MYSQL_SOURCE_DIR}/auth/auth.c
  LIBS Salts::CNet Salts::CSerde Salts::CFlow Salts::Core
  DEFS MYSQL_ENABLE_FAULT_INJECTION=1
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/flow
    ${ORM_SOURCE_DIR}/../drivers/mysql ${ORM_MYSQL_SOURCE_DIR}
    ${ORM_MYSQL_SOURCE_DIR}/wire ${ORM_MYSQL_SOURCE_DIR}/auth)

if(MSVC)
  target_compile_options(orm_mysql_live_test PRIVATE /WX)
else()
  target_compile_options(orm_mysql_live_test PRIVATE -Werror)
endif()

set_tests_properties(orm_mysql_live PROPERTIES LABELS e2e RUN_SERIAL TRUE)
if(WIN32)
  set_tests_properties(
    orm_mysql_live
    PROPERTIES
      ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::DataBind>"
  )
endif()
