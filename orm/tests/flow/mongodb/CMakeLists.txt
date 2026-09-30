cmake_add_test(
  NAME orm_mongo_flow_test
  FOLDER "orm/tests/flow/mongodb"
  TEST_NAME orm_mongo_flow
  SOURCES orm_mongo_flow_test.c ${ORM_SOURCE_DIR}/../drivers/mongodb/orm_mongo_cursor.c
  LIBS ${ORM_INTERNAL_TEST_LIBRARY} Salts::DataBind Salts::CFlow Salts::TinyTest
  DEFS ORM_C_STATIC
  INCLUDES ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/flow
    ${ORM_SOURCE_DIR}/../drivers/mongodb)

set_property(TARGET orm_mongo_flow_test PROPERTY LINKER_LANGUAGE CXX)

if(WIN32)
  set_tests_properties(orm_mongo_flow PROPERTIES ENVIRONMENT_MODIFICATION
                                                 "${ORM_TEST_RUNTIME_PATH}")
endif()
