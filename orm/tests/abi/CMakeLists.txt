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
