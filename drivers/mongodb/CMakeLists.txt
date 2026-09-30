find_package(mongoc CONFIG REQUIRED)
find_package(bson CONFIG REQUIRED)

add_library(
  orm_mongodb_driver MODULE
  plugin.c
  ${ORM_SOURCE_DIR}/src/driver/orm_driver_backend_bridge.c
  backend.c
  query.c
  orm_mongo_lib.c
  orm_mongo_cursor.c
  ${ORM_SOURCE_DIR}/src/flow/orm_text_token.c)
target_compile_features(orm_mongodb_driver PRIVATE c_std_11)
target_compile_definitions(orm_mongodb_driver
                           PRIVATE ORM_C_STATIC ORM_NATIVE_OWNER_CANDIDATE)
target_include_directories(
  orm_mongodb_driver
  PRIVATE ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi ${ORM_SOURCE_DIR}/src/flow
          ${ORM_SOURCE_DIR}/src/driver ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
  orm_mongodb_driver
  PRIVATE turbo_orm
          Orm::DriverABI
          Salts::Core
          Salts::CSerde
          Salts::CFlow
          Salts::DataBind
          Salts::CSTL)
if(TARGET mongoc::shared)
  target_link_libraries(orm_mongodb_driver PRIVATE mongoc::shared)
elseif(TARGET mongoc::static)
  target_link_libraries(orm_mongodb_driver PRIVATE mongoc::static)
else()
  message(FATAL_ERROR "MongoDB Driver requires a canonical mongoc target")
endif()
if(TARGET bson::shared)
  target_link_libraries(orm_mongodb_driver PRIVATE bson::shared)
elseif(TARGET bson::static)
  target_link_libraries(orm_mongodb_driver PRIVATE bson::static)
else()
  message(FATAL_ERROR "MongoDB Driver requires a canonical bson target")
endif()
set_target_properties(
  orm_mongodb_driver
  PROPERTIES PREFIX ""
             OUTPUT_NAME "turbodb_driver_mongodb"
             C_STANDARD_REQUIRED ON
             C_VISIBILITY_PRESET hidden
             WINDOWS_EXPORT_ALL_SYMBOLS OFF
             FOLDER "drivers/mongodb")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(orm_mongodb_driver PRIVATE "LINKER:--exclude-libs,ALL"
                      "LINKER:-z,defs")
endif()

target_compile_options(orm_mongodb_driver PRIVATE ${ORM_WARNING_OPTIONS})

install(TARGETS orm_mongodb_driver
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/turbodb/drivers
                COMPONENT OrmMongodbDriver
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/turbodb/drivers
                COMPONENT OrmMongodbDriver)
