find_package(SQLite3 REQUIRED)
find_package(PostgreSQL REQUIRED)

get_target_property(ORM_SOURCE_DIR turbo_orm SOURCE_DIR)
get_target_property(ORM_MYSQL_SOURCE_DIR turbodb_mysql SOURCE_DIR)

# All ORM contract tests can include the public shared DTOs, including those
# that compile a driver translation unit without linking the ORM runtime.
link_libraries(TurboDB::Types)

set(ORM_INTERNAL_TEST_LIBRARY turbo_orm)
if(BUILD_TESTS)
  add_subdirectory(architecture)
  add_subdirectory(core)
  set(ORM_ABI_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/abi)
  include(support/TestEnvironment.cmake)
  add_subdirectory(support)
  add_subdirectory(abi)
  add_subdirectory(driver)
  add_subdirectory(flow)
  add_subdirectory(integration)
  add_subdirectory(ownership)
endif()

if(BUILD_E2E_TESTS)
  add_subdirectory(e2e)
endif()
