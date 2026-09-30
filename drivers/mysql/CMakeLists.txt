add_library(
  orm_mysql_driver MODULE
  plugin.c
  schema.c
  backend.c
  dialect.c
  cursor.c
  ${ORM_SOURCE_DIR}/src/driver/orm_driver_backend_bridge.c)

target_compile_features(orm_mysql_driver PRIVATE c_std_11)
target_compile_definitions(orm_mysql_driver
  PRIVATE ORM_C_STATIC ORM_NATIVE_OWNER_CANDIDATE)
target_include_directories(orm_mysql_driver PRIVATE
  ${ORM_SOURCE_DIR}/src/abi
  ${ORM_SOURCE_DIR}/src/flow
  ${ORM_SOURCE_DIR}/src/driver)
target_link_libraries(orm_mysql_driver PRIVATE
  Orm::C Orm::DriverABI TurboDB::MySQL Salts::CSTL
  turbodb_schema_support Salts::JsonParser)
set_target_properties(orm_mysql_driver PROPERTIES
  PREFIX ""
  OUTPUT_NAME "turbodb_driver_mysql"
  C_STANDARD_REQUIRED ON
  C_VISIBILITY_PRESET hidden
  WINDOWS_EXPORT_ALL_SYMBOLS OFF
  FOLDER "drivers/mysql")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(orm_mysql_driver PRIVATE
    "LINKER:--exclude-libs,ALL" "LINKER:-z,defs")
endif()
target_compile_options(orm_mysql_driver PRIVATE ${ORM_WARNING_OPTIONS})

install(
  TARGETS orm_mysql_driver RUNTIME_DEPENDENCY_SET
          OrmMysqlDriverRuntimeDependencies
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}/turbodb/drivers
          COMPONENT OrmMysqlDriver
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/turbodb/drivers
          COMPONENT OrmMysqlDriver)
