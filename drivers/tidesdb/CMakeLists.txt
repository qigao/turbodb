add_subdirectory(sql)

add_library(
  orm_tidesdb_driver MODULE
  plugin.c ${ORM_SOURCE_DIR}/src/driver/orm_driver_backend_bridge.c
  backend.c bridge.c row.c
  orm_tidesdb_cursor.c ${ORM_SOURCE_DIR}/src/abi/orm_plan.c)
target_compile_features(orm_tidesdb_driver PRIVATE c_std_11)
target_compile_definitions(orm_tidesdb_driver
                           PRIVATE ORM_C_STATIC ORM_NATIVE_OWNER_CANDIDATE)
target_include_directories(
  orm_tidesdb_driver
  PRIVATE ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi ${ORM_SOURCE_DIR}/src/flow
          ${ORM_SOURCE_DIR}/src/driver
          ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
  orm_tidesdb_driver
  PRIVATE turbo_orm
          orm_tidesdb_sql
          TidesDB::tidesdb
          Orm::DriverABI
          Salts::Core
          Salts::CSerde
          Salts::CFlow
          Salts::DataBind
          Salts::CSTL)
set_target_properties(
  orm_tidesdb_driver
  PROPERTIES PREFIX ""
             OUTPUT_NAME "turbodb_driver_tidesdb"
             C_STANDARD_REQUIRED ON
             C_VISIBILITY_PRESET hidden
             WINDOWS_EXPORT_ALL_SYMBOLS OFF
             FOLDER "drivers/tidesdb")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(orm_tidesdb_driver PRIVATE "LINKER:--exclude-libs,ALL"
                      "LINKER:-z,defs")
endif()

target_compile_options(orm_tidesdb_driver PRIVATE ${ORM_WARNING_OPTIONS})

install(TARGETS orm_tidesdb_driver
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/turbodb/drivers
                COMPONENT OrmTidesdbDriver
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/turbodb/drivers
                COMPONENT OrmTidesdbDriver)
