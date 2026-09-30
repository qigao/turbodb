cmake_add_test(
  NAME orm_backend_profile_test
  FOLDER "orm/tests/abi"
  TEST_NAME orm_backend_profile
  SOURCES backend_profile_test.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::TinyTest
  DEFS ORM_C_STATIC)
target_compile_features(orm_backend_profile_test PRIVATE c_std_11)

set_property(TARGET orm_backend_profile_test PROPERTY LINKER_LANGUAGE CXX)

target_compile_options(orm_backend_profile_test
                       PRIVATE ${ORM_TEST_WARNING_OPTIONS})
if(MSVC)
  target_compile_options(orm_backend_profile_test
                         PRIVATE $<$<COMPILE_LANGUAGE:C>:/std:c11>)
endif()

if(WIN32)
  set_tests_properties(
    orm_backend_profile PROPERTIES ENVIRONMENT_MODIFICATION
                                   "${ORM_TEST_RUNTIME_PATH}")
endif()

add_subdirectory(redis)
