# These tests observe native owners in the existing full-backend test clone.
if(NOT TARGET orm_c_test_static)
  message(FATAL_ERROR "Native ownership tests require the real ORM test library")
endif()

add_executable(orm_owner_regression_test orm_owner_regression_test.c)
target_compile_features(orm_owner_regression_test PRIVATE c_std_11)
target_compile_definitions(orm_owner_regression_test PRIVATE ORM_C_STATIC)
target_include_directories(orm_owner_regression_test PRIVATE
  ../../include/orm ../../src/abi ../../src/flow)
target_link_libraries(orm_owner_regression_test PRIVATE
  orm_c_test_static Salts::TinyTest)
set_target_properties(orm_owner_regression_test PROPERTIES
  C_STANDARD_REQUIRED ON LINKER_LANGUAGE CXX FOLDER "orm/tests")
if(MSVC)
  target_compile_options(orm_owner_regression_test PRIVATE /W4 /WX /Zc:preprocessor)
else()
  target_compile_options(orm_owner_regression_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
add_test(NAME orm_owner_regression COMMAND orm_owner_regression_test)
if(WIN32)
  set_tests_properties(orm_owner_regression PROPERTIES ENVIRONMENT_MODIFICATION
    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>")
endif()
