cmake_add_test(
  NAME orm_postgres_live_test
  FOLDER "orm/tests/e2e/postgresql"
  TEST_NAME orm_postgres_live
  SOURCES postgres_live_test.c
  LIBS turbo_orm Salts::TinyTest Salts::Platform
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)

add_dependencies(orm_postgres_live_test orm_postgresql_driver)

set_tests_properties(orm_postgres_live PROPERTIES LABELS e2e RUN_SERIAL TRUE)
if(WIN32)
  set_tests_properties(
    orm_postgres_live
    PROPERTIES
      ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::DataBind>"
  )
endif()

set_tests_properties(
  orm_postgres_live
  PROPERTIES ENVIRONMENT
             "ORM_POSTGRESQL_PLUGIN=$<TARGET_FILE:orm_postgresql_driver>")

# Ownership assertions need the private core layout, independently of unit
# tests.
cmake_add_test(
  NAME orm_owner_postgres_test
  FOLDER "orm/tests/e2e/postgresql"
  TEST_NAME orm_owner_postgres
  SOURCES orm_owner_postgres_test.c
  LIBS turbo_orm PostgreSQL::PostgreSQL Salts::DataBind Salts::CFlow Salts::TinyTest)
cmake_add_test(
  NAME orm_owner_postgres_loss_test
  FOLDER "orm/tests/e2e/postgresql"
  TEST_NAME orm_owner_postgres_loss
  SOURCES orm_owner_postgres_loss_test.c
  LIBS turbo_orm PostgreSQL::PostgreSQL Salts::DataBind Salts::CFlow Salts::TinyTest)
add_dependencies(orm_owner_postgres_test orm_postgresql_driver)
add_dependencies(orm_owner_postgres_loss_test orm_postgresql_driver)
set_property(TARGET orm_owner_postgres_test orm_owner_postgres_loss_test
             PROPERTY COMPILE_DEFINITIONS ORM_NATIVE_OWNER_CANDIDATE)
set_property(
  TARGET orm_owner_postgres_test orm_owner_postgres_loss_test
  PROPERTY
    INCLUDE_DIRECTORIES
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/abi;${CMAKE_CURRENT_SOURCE_DIR}/../../../src/flow"
)

set_property(TARGET orm_owner_postgres_test orm_owner_postgres_loss_test
             PROPERTY LINKER_LANGUAGE CXX)

if(MSVC)
  set_property(
    TARGET orm_owner_postgres_test orm_owner_postgres_loss_test
    APPEND
    PROPERTY COMPILE_OPTIONS /WX)
else()
  set_property(
    TARGET orm_owner_postgres_test orm_owner_postgres_loss_test
    APPEND
    PROPERTY COMPILE_OPTIONS -Werror)
endif()

set_tests_properties(orm_owner_postgres PROPERTIES LABELS e2e RUN_SERIAL TRUE)
if(WIN32)
  set_tests_properties(
    orm_owner_postgres
    PROPERTIES
      ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::DataBind>"
  )
endif()

set_tests_properties(orm_owner_postgres_loss PROPERTIES LABELS e2e RUN_SERIAL
                                                        TRUE)
if(WIN32)
  set_tests_properties(
    orm_owner_postgres_loss
    PROPERTIES
      ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::DataBind>"
  )
endif()

set_tests_properties(orm_owner_postgres orm_owner_postgres_loss
                     PROPERTIES TIMEOUT 60
                       ENVIRONMENT "ORM_POSTGRESQL_PLUGIN=$<TARGET_FILE:orm_postgresql_driver>")

cmake_add_test(
  NAME orm_postgres_direct_tls_live_test
  FOLDER "orm/tests/e2e/postgresql"
  TEST_NAME orm_postgres_direct_tls_live
  SOURCES postgres_direct_tls_live_test.c
  LIBS turbo_orm Salts::TinyTest Salts::Platform
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)
add_dependencies(orm_postgres_direct_tls_live_test orm_postgresql_driver)
set_tests_properties(
  orm_postgres_direct_tls_live
  PROPERTIES
    LABELS e2e
    RUN_SERIAL TRUE
    TIMEOUT 60
    ENVIRONMENT "ORM_POSTGRESQL_PLUGIN=$<TARGET_FILE:orm_postgresql_driver>")
