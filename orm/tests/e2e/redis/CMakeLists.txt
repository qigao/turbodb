cmake_add_test(
  NAME orm_redis_live_driver_test
  FOLDER "orm/tests/e2e/redis"
  TEST_NAME orm_redis_live_driver
  SOURCES redis_live_driver_test.c
  LIBS turbo_orm Salts::CFlow Salts::Platform
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm)

add_dependencies(orm_redis_live_driver_test orm_redis_driver)

set_tests_properties(orm_redis_live_driver PROPERTIES LABELS e2e RUN_SERIAL
                                                      TRUE)
if(WIN32)
  set_tests_properties(
    orm_redis_live_driver
    PROPERTIES
      ENVIRONMENT_MODIFICATION
      "PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::Core>;PATH=path_list_prepend:$<TARGET_FILE_DIR:Salts::DataBind>"
  )
endif()

set_tests_properties(
  orm_redis_live_driver
  PROPERTIES ENVIRONMENT "ORM_REDIS_PLUGIN=$<TARGET_FILE:orm_redis_driver>")
add_test(NAME orm_redis_index_setup COMMAND ${CMAKE_COMMAND}
  -DACTION=create "-DSTAMP=${CMAKE_CURRENT_BINARY_DIR}/redis-index-owned"
  -P ${CMAKE_CURRENT_SOURCE_DIR}/index_fixture.cmake)
add_test(NAME orm_redis_index_cleanup COMMAND ${CMAKE_COMMAND}
  -DACTION=drop "-DSTAMP=${CMAKE_CURRENT_BINARY_DIR}/redis-index-owned"
  -P ${CMAKE_CURRENT_SOURCE_DIR}/index_fixture.cmake)
set_tests_properties(orm_redis_index_setup PROPERTIES FIXTURES_SETUP redis_live_index)
set_tests_properties(orm_redis_index_cleanup PROPERTIES FIXTURES_CLEANUP redis_live_index)
set_tests_properties(orm_redis_live_driver PROPERTIES FIXTURES_REQUIRED redis_live_index)
set_tests_properties(orm_redis_index_setup orm_redis_index_cleanup
  PROPERTIES LABELS e2e RUN_SERIAL TRUE TIMEOUT 30)
