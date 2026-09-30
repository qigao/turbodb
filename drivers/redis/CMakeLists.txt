add_library(
  orm_redis_driver MODULE
  plugin.c
  ${ORM_SOURCE_DIR}/src/driver/orm_driver_backend_bridge.c
  backend.c
  query.c
  orm_redis_lib.c
  orm_redis_cursor.c
  ${ORM_SOURCE_DIR}/src/flow/orm_text_token.c)
target_compile_definitions(orm_redis_driver
                           PRIVATE ORM_C_STATIC ORM_NATIVE_OWNER_CANDIDATE)
target_include_directories(
  orm_redis_driver
  PRIVATE ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi ${ORM_SOURCE_DIR}/src/flow
          ${ORM_SOURCE_DIR}/src/driver ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
  orm_redis_driver
  PRIVATE turbo_orm
          TurboDB::Redis
          Orm::DriverABI
          Salts::Core
          Salts::CSerde
          Salts::CFlow
          Salts::DataBind
          Salts::CSTL
          Salts::Platform)
set_target_properties(
  orm_redis_driver
  PROPERTIES PREFIX ""
             OUTPUT_NAME "turbodb_driver_redis"
             C_VISIBILITY_PRESET hidden
             WINDOWS_EXPORT_ALL_SYMBOLS OFF
             FOLDER "drivers/redis")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(orm_redis_driver PRIVATE "LINKER:--exclude-libs,ALL"
                      "LINKER:-z,defs")
endif()

install(TARGETS orm_redis_driver
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/turbodb/drivers
                COMPONENT OrmRedisDriver
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/turbodb/drivers
                COMPONENT OrmRedisDriver)
