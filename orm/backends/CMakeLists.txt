# Concrete ORM adapters own their native dependencies and plugin tests.
add_subdirectory(sqlite)
add_subdirectory(postgresql)
add_subdirectory(mysql)

if(BUILD_TESTS)
  add_subdirectory(tests)
endif()
