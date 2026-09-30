cmake_add_test(
  NAME orm_sqlite_async_test
  FOLDER "orm/tests/flow/sqlite"
  TEST_NAME orm_sqlite_async
  SOURCES orm_sqlite_async_test.c
  LIBS orm_c_test_static SQLite::SQLite3 Salts::CFlow Salts::Platform
    Salts::TinyTest Salts::TinyMock
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi
    ${ORM_SOURCE_DIR}/src/flow)
set_tests_properties(orm_sqlite_async PROPERTIES TIMEOUT 60)
if(WIN32)
  set_tests_properties(orm_sqlite_async PROPERTIES ENVIRONMENT_MODIFICATION
                                                 "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_sqlite_flow_test
  FOLDER "orm/tests/flow/sqlite"
  TEST_NAME orm_sqlite_flow
  SOURCES orm_sqlite_flow_test.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} SQLite::SQLite3 Salts::DataBind Salts::CFlow
    Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/flow
    ${ORM_SOURCE_DIR}/../drivers/sqlite)

set_property(TARGET orm_sqlite_flow_test PROPERTY LINKER_LANGUAGE CXX)

if(WIN32)
  set_tests_properties(orm_sqlite_flow PROPERTIES ENVIRONMENT_MODIFICATION
                                                  "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_public_flow_test
  FOLDER "orm/tests/flow/sqlite"
  TEST_NAME orm_public_flow
  SOURCES orm_public_flow_test.c
  LIBS orm_c_test_static Salts::CFlow Salts::DataBind Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)

if(WIN32)
  set_tests_properties(orm_public_flow PROPERTIES ENVIRONMENT_MODIFICATION
                                                  "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_cpp_flow_test
  FOLDER "orm/tests/flow/sqlite"
  TEST_NAME orm_cpp_flow
  SOURCES orm_cpp_flow_test.cpp
  LIBS orm_c_test_static Salts::CFlow Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)
target_compile_features(orm_cpp_flow_test PRIVATE cxx_std_17)

set_target_properties(orm_cpp_flow_test PROPERTIES CXX_STANDARD_REQUIRED ON)

if(WIN32)
  set_tests_properties(orm_cpp_flow PROPERTIES ENVIRONMENT_MODIFICATION
                                               "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_public_ownership_test
  FOLDER "orm/tests/flow/sqlite"
  TEST_NAME orm_public_ownership
  SOURCES orm_public_ownership_test.c
  LIBS orm_c_test_static Salts::CFlow Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)

if(WIN32)
  set_tests_properties(
    orm_public_ownership PROPERTIES ENVIRONMENT_MODIFICATION
                                    "${ORM_TEST_RUNTIME_PATH}")
endif()

cmake_add_test(
  NAME orm_public_ownership_cpp_test
  FOLDER "orm/tests/flow/sqlite"
  TEST_NAME orm_public_ownership_cpp
  SOURCES orm_public_ownership_cpp_test.cpp
  LIBS orm_c_test_static Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)
target_compile_features(orm_public_ownership_cpp_test PRIVATE cxx_std_17)

set_target_properties(orm_public_ownership_cpp_test
                      PROPERTIES CXX_STANDARD_REQUIRED ON)

if(WIN32)
  set_tests_properties(
    orm_public_ownership_cpp PROPERTIES ENVIRONMENT_MODIFICATION
                                        "${ORM_TEST_RUNTIME_PATH}")
endif()

