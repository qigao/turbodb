find_package(SQLite3 REQUIRED)

add_library(
  orm_sqlite_driver MODULE
  plugin.c ${ORM_SOURCE_DIR}/src/driver/orm_driver_backend_bridge.c
  backend.c async.c maintenance.c schema.c
  orm_sqlite_cursor.c ${ORM_SOURCE_DIR}/src/sql/orm_sql_render.c)
target_compile_definitions(orm_sqlite_driver PRIVATE ORM_C_STATIC)
target_include_directories(
  orm_sqlite_driver
  PRIVATE ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi ${ORM_SOURCE_DIR}/src/flow
          ${ORM_SOURCE_DIR}/src/sql ${ORM_SOURCE_DIR}/src/driver
          ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
  orm_sqlite_driver
  PRIVATE turbo_orm
          turbodb_schema_support
          SQLite::SQLite3
          Orm::DriverABI
          Salts::Core
          Salts::CSerde
          Salts::CFlow
          Salts::CSTL)
# Runtime MODULE owns the native SQLite dependency. Generic Orm::C must remain
# independent from SQLite headers, objects and link items.
get_target_property(ORM_SQLITE_DRIVER_LINK_CONTRACT orm_sqlite_driver
                    LINK_LIBRARIES)
list(FIND ORM_SQLITE_DRIVER_LINK_CONTRACT "SQLite::SQLite3"
     ORM_SQLITE_DRIVER_SQLITE_LINK_INDEX)
if(ORM_SQLITE_DRIVER_SQLITE_LINK_INDEX EQUAL -1)
  message(FATAL_ERROR "orm_sqlite_driver must directly link SQLite::SQLite3")
endif()
set_target_properties(
  orm_sqlite_driver
  PROPERTIES PREFIX ""
             OUTPUT_NAME "turbodb_driver_sqlite"
             C_VISIBILITY_PRESET hidden
             WINDOWS_EXPORT_ALL_SYMBOLS OFF
             FOLDER "drivers/sqlite")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Keep statically linked SQLite/Salts archive symbols private to the Driver
  # MODULE. salts_plugin_query is defined by the MODULE object itself and
  # remains the only intentional generic loader entry.
  target_link_options(orm_sqlite_driver PRIVATE "LINKER:--exclude-libs,ALL")
endif()

install(TARGETS orm_sqlite_driver
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/turbodb/drivers
                COMPONENT OrmSqliteDriver
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/turbodb/drivers
                COMPONENT OrmSqliteDriver)
