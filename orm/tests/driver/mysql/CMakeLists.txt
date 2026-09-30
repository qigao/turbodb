cmake_add_test(
  NAME orm_mysql_plugin_test
  FOLDER "orm/tests/driver/mysql"
  TEST_NAME orm_mysql_plugin
  SOURCES orm_mysql_plugin_test.c
  LIBS turbo_orm Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)
target_compile_features(orm_mysql_plugin_test PRIVATE c_std_11)

add_dependencies(orm_mysql_plugin_test orm_mysql_driver)
set_target_properties(orm_mysql_plugin_test PROPERTIES C_STANDARD_REQUIRED ON)
target_compile_options(orm_mysql_plugin_test
                       PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_mysql_plugin_test PRIVATE /WX)
else()
  target_compile_options(orm_mysql_plugin_test PRIVATE -Werror)
endif()

set_tests_properties(
  orm_mysql_plugin
  PROPERTIES ENVIRONMENT "ORM_MYSQL_PLUGIN=$<TARGET_FILE:orm_mysql_driver>")

# MySQL issue #33 focused protocol and Driver qualification.
cmake_add_test(
  NAME orm_mysql_wire_test
  FOLDER "orm/tests/driver/mysql"
  TEST_NAME orm_mysql_wire
  SOURCES orm_mysql_wire_test.c
  LIBS TurboDB::MySQL Salts::TinyTest
  INCLUDES ${ORM_MYSQL_SOURCE_DIR}/wire)
target_compile_features(orm_mysql_wire_test PRIVATE c_std_11)

set_target_properties(orm_mysql_wire_test PROPERTIES C_STANDARD_REQUIRED ON)
target_compile_options(orm_mysql_wire_test PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_mysql_wire_test PRIVATE /WX)
else()
  target_compile_options(orm_mysql_wire_test PRIVATE -Werror)
endif()

cmake_add_test(
  NAME orm_mysql_parameters_test
  FOLDER "orm/tests/driver/mysql"
  TEST_NAME orm_mysql_parameters
  SOURCES orm_mysql_parameters_test.c
  LIBS TurboDB::MySQL Salts::TinyTest
  INCLUDES ${ORM_MYSQL_SOURCE_DIR} ${ORM_MYSQL_SOURCE_DIR}/wire)
target_compile_features(orm_mysql_parameters_test PRIVATE c_std_11)

set_target_properties(orm_mysql_parameters_test
                      PROPERTIES C_STANDARD_REQUIRED ON)
target_compile_options(orm_mysql_parameters_test
                       PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_mysql_parameters_test PRIVATE /WX)
else()
  target_compile_options(orm_mysql_parameters_test PRIVATE -Werror)
endif()

cmake_add_test(
  NAME orm_mysql_dialect_test
  FOLDER "orm/tests/driver/mysql"
  TEST_NAME orm_mysql_dialect
  SOURCES orm_mysql_dialect_test.c ${ORM_SOURCE_DIR}/../drivers/mysql/dialect.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi
    ${ORM_SOURCE_DIR}/src/flow ${ORM_SOURCE_DIR}/../drivers/mysql
    ${ORM_MYSQL_SOURCE_DIR})
target_compile_features(orm_mysql_dialect_test PRIVATE c_std_11)

set_property(TARGET orm_mysql_dialect_test PROPERTY LINKER_LANGUAGE CXX)
set_target_properties(orm_mysql_dialect_test PROPERTIES C_STANDARD_REQUIRED ON)
target_compile_options(orm_mysql_dialect_test
                       PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_mysql_dialect_test PRIVATE /WX)
else()
  target_compile_options(orm_mysql_dialect_test PRIVATE -Werror)
endif()

cmake_add_test(
  NAME orm_mysql_statement_test
  FOLDER "orm/tests/driver/mysql"
  TEST_NAME orm_mysql_statement
  SOURCES orm_mysql_statement_test.c
  LIBS TurboDB::MySQL Salts::TinyTest
  INCLUDES ${ORM_MYSQL_SOURCE_DIR}/wire)
target_compile_features(orm_mysql_statement_test PRIVATE c_std_11)

set_target_properties(orm_mysql_statement_test PROPERTIES C_STANDARD_REQUIRED ON)
target_compile_options(orm_mysql_statement_test
                       PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_mysql_statement_test PRIVATE /WX)
else()
  target_compile_options(orm_mysql_statement_test PRIVATE -Werror)
endif()

cmake_add_test(
  NAME orm_mysql_query_test
  FOLDER "orm/tests/driver/mysql"
  TEST_NAME orm_mysql_query
  SOURCES orm_mysql_query_test.c
  LIBS TurboDB::MySQL Salts::TinyTest
  INCLUDES ${ORM_MYSQL_SOURCE_DIR}/wire)
target_compile_features(orm_mysql_query_test PRIVATE c_std_11)

set_target_properties(orm_mysql_query_test PROPERTIES C_STANDARD_REQUIRED ON)
target_compile_options(orm_mysql_query_test PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_mysql_query_test PRIVATE /WX)
else()
  target_compile_options(orm_mysql_query_test PRIVATE -Werror)
endif()

cmake_add_test(
  NAME orm_mysql_savepoint_test
  FOLDER "orm/tests/driver/mysql"
  TEST_NAME orm_mysql_savepoint
  SOURCES orm_mysql_savepoint_test.c
  LIBS TurboDB::MySQL Salts::TinyTest
  INCLUDES ${ORM_MYSQL_SOURCE_DIR} ${ORM_MYSQL_SOURCE_DIR}/wire)
target_compile_features(orm_mysql_savepoint_test PRIVATE c_std_11)

set_target_properties(orm_mysql_savepoint_test PROPERTIES C_STANDARD_REQUIRED ON)
target_compile_options(orm_mysql_savepoint_test
                       PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_mysql_savepoint_test PRIVATE /WX)
else()
  target_compile_options(orm_mysql_savepoint_test PRIVATE -Werror)
endif()

cmake_add_test(
  NAME orm_mysql_binary_row_test
  FOLDER "orm/tests/driver/mysql"
  TEST_NAME orm_mysql_binary_row
  SOURCES orm_mysql_binary_row_test.c
  LIBS TurboDB::MySQL Salts::TinyTest
  INCLUDES ${ORM_MYSQL_SOURCE_DIR}/wire)
target_compile_features(orm_mysql_binary_row_test PRIVATE c_std_11)

set_target_properties(orm_mysql_binary_row_test
                      PROPERTIES C_STANDARD_REQUIRED ON)
target_compile_options(orm_mysql_binary_row_test
                       PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_mysql_binary_row_test PRIVATE /WX)
else()
  target_compile_options(orm_mysql_binary_row_test PRIVATE -Werror)
endif()

cmake_add_test(
  NAME orm_mysql_row_reader_test
  FOLDER "orm/tests/driver/mysql"
  TEST_NAME orm_mysql_row_reader
  SOURCES orm_mysql_row_reader_test.c
  LIBS TurboDB::MySQL Salts::TinyTest Salts::CSerde
  INCLUDES ${ORM_MYSQL_SOURCE_DIR} ${ORM_MYSQL_SOURCE_DIR}/wire)
target_compile_features(orm_mysql_row_reader_test PRIVATE c_std_11)

set_target_properties(orm_mysql_row_reader_test
                      PROPERTIES C_STANDARD_REQUIRED ON)
target_compile_options(orm_mysql_row_reader_test
                       PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_mysql_row_reader_test PRIVATE /WX)
else()
  target_compile_options(orm_mysql_row_reader_test PRIVATE -Werror)
endif()

cmake_add_test(
  NAME orm_mysql_cursor_row_test
  FOLDER "orm/tests/driver/mysql"
  TEST_NAME orm_mysql_cursor_row
  SOURCES orm_mysql_cursor_row_test.c
  LIBS TurboDB::MySQL Salts::TinyTest Salts::CSerde
  INCLUDES ${ORM_MYSQL_SOURCE_DIR} ${ORM_MYSQL_SOURCE_DIR}/wire)
target_compile_features(orm_mysql_cursor_row_test PRIVATE c_std_11)

set_target_properties(orm_mysql_cursor_row_test
                      PROPERTIES C_STANDARD_REQUIRED ON)
target_compile_options(orm_mysql_cursor_row_test
                       PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_mysql_cursor_row_test PRIVATE /WX)
else()
  target_compile_options(orm_mysql_cursor_row_test PRIVATE -Werror)
endif()

cmake_add_test(
  NAME orm_mysql_cursor_test
  FOLDER "orm/tests/driver/mysql"
  TEST_NAME orm_mysql_cursor
  SOURCES orm_mysql_cursor_test.c ${ORM_SOURCE_DIR}/../drivers/mysql/cursor.c
  LIBS TurboDB::MySQL Salts::Core Salts::CFlow Salts::CSerde Salts::TinyTest
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/flow
    ${ORM_SOURCE_DIR}/../drivers/mysql ${ORM_MYSQL_SOURCE_DIR}
    ${ORM_MYSQL_SOURCE_DIR}/wire)
target_compile_features(orm_mysql_cursor_test PRIVATE c_std_11)

set_target_properties(orm_mysql_cursor_test PROPERTIES C_STANDARD_REQUIRED ON)
target_compile_options(orm_mysql_cursor_test
                       PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_mysql_cursor_test PRIVATE /WX)
else()
  target_compile_options(orm_mysql_cursor_test PRIVATE -Werror)
endif()

cmake_add_test(
  NAME orm_mysql_handshake_test
  FOLDER "orm/tests/driver/mysql"
  TEST_NAME orm_mysql_handshake
  SOURCES orm_mysql_handshake_test.c
  LIBS TurboDB::MySQL Salts::TinyTest OpenSSL::Crypto
  INCLUDES ${ORM_MYSQL_SOURCE_DIR}/wire ${ORM_MYSQL_SOURCE_DIR}/auth)
target_compile_features(orm_mysql_handshake_test PRIVATE c_std_11)

set_target_properties(orm_mysql_handshake_test PROPERTIES C_STANDARD_REQUIRED ON)
target_compile_options(orm_mysql_handshake_test
                       PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_mysql_handshake_test PRIVATE /WX)
else()
  target_compile_options(orm_mysql_handshake_test PRIVATE -Werror)
endif()
