find_package(PostgreSQL REQUIRED)

add_library(
  orm_postgresql_driver MODULE
  plugin.c ${ORM_SOURCE_DIR}/src/driver/orm_driver_backend_bridge.c
  backend.c schema.c postgres_transport_policy.c postgres_direct_tls_bridge.c orm_postgres_cursor.c orm_postgres_libpq.c
  ${ORM_SOURCE_DIR}/src/flow/orm_text_token.c ${ORM_SOURCE_DIR}/src/sql/orm_sql_render.c)
target_compile_definitions(orm_postgresql_driver
                           PRIVATE ORM_C_STATIC ORM_NATIVE_OWNER_CANDIDATE)
target_include_directories(
  orm_postgresql_driver
  PRIVATE ${ORM_SOURCE_DIR}/include/orm ${ORM_SOURCE_DIR}/src/abi ${ORM_SOURCE_DIR}/src/flow
          ${ORM_SOURCE_DIR}/src/sql ${ORM_SOURCE_DIR}/src/driver
          ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
  orm_postgresql_driver
  PRIVATE turbo_orm
          turbodb_schema_support
          PostgreSQL::PostgreSQL
          Orm::DriverABI
          Salts::Core
          Salts::CNet
          Salts::CSerde
          Salts::CFlow
          Salts::CSTL)
set_target_properties(
  orm_postgresql_driver
  PROPERTIES PREFIX ""
             OUTPUT_NAME "turbodb_driver_postgresql"
             LINKER_LANGUAGE CXX
             C_VISIBILITY_PRESET hidden
             WINDOWS_EXPORT_ALL_SYMBOLS OFF
             FOLDER "drivers/postgresql")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(orm_postgresql_driver PRIVATE
                      "LINKER:--exclude-libs,ALL")
endif()
get_target_property(ORM_POSTGRESQL_DRIVER_LINK_CONTRACT orm_postgresql_driver
                    LINK_LIBRARIES)
list(FIND ORM_POSTGRESQL_DRIVER_LINK_CONTRACT "PostgreSQL::PostgreSQL"
     ORM_POSTGRESQL_DRIVER_NATIVE_LINK_INDEX)
if(ORM_POSTGRESQL_DRIVER_NATIVE_LINK_INDEX EQUAL -1)
  message(
    FATAL_ERROR
      "orm_postgresql_driver must directly link PostgreSQL::PostgreSQL")
endif()

install(TARGETS orm_postgresql_driver
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/turbodb/drivers
                COMPONENT OrmPostgresqlDriver
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/turbodb/drivers
                COMPONENT OrmPostgresqlDriver)
