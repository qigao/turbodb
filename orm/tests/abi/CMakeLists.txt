cmake_add_test(
  NAME orm_backend_profile_test
  FOLDER "orm/tests/abi"
  TEST_NAME orm_backend_profile
  SOURCES backend_profile_test.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::TinyTest
  DEFS ORM_C_STATIC)

set_property(TARGET orm_backend_profile_test PROPERTY LINKER_LANGUAGE CXX)

if(WIN32)
  set_tests_properties(
    orm_backend_profile PROPERTIES ENVIRONMENT_MODIFICATION
                                   "${ORM_TEST_RUNTIME_PATH}")
endif()

add_subdirectory(redis)

cmake_add_test(
  NAME orm_execution_plan_test
  FOLDER "orm/tests/abi"
  TEST_NAME orm_execution_plan
  SOURCES execution_plan_test.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::CSerde Salts::TinyTest
  DEFS ORM_C_STATIC ORM_NATIVE_OWNER_CANDIDATE
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi
           ${ORM_SOURCE_DIR}/src/flow)
set_property(TARGET orm_execution_plan_test PROPERTY LINKER_LANGUAGE CXX)
if(WIN32)
  set_tests_properties(
    orm_execution_plan PROPERTIES ENVIRONMENT_MODIFICATION
                                  "${ORM_TEST_RUNTIME_PATH}")
endif()
