if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME orm_core_dependencies
    COMMAND ${CMAKE_COMMAND} "-DORM_CORE=$<TARGET_FILE:turbo_orm>"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/core_dependencies_test.cmake)
  set_tests_properties(orm_core_dependencies PROPERTIES LABELS architecture TIMEOUT 30)
endif()
