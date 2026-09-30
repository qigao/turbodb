add_library(turbodb_schema_abi INTERFACE)
add_library(TurboDB::SchemaABI ALIAS turbodb_schema_abi)
target_include_directories(turbodb_schema_abi INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:include/turbodb/schema>)
target_link_libraries(turbodb_schema_abi INTERFACE Salts::PluginABI)
set_target_properties(turbodb_schema_abi PROPERTIES
  EXPORT_NAME SchemaABI FOLDER "drivers/schema")

add_library(turbodb_schema_support STATIC dbtool_error.c)
target_link_libraries(turbodb_schema_support PUBLIC TurboDB::SchemaABI)
set_target_properties(turbodb_schema_support PROPERTIES
  C_VISIBILITY_PRESET hidden FOLDER "drivers/schema")

install(TARGETS turbodb_schema_abi EXPORT TurboDBTargets COMPONENT SchemaABI)
install(FILES dbtool_error.h dbtool_schema_driver.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/turbodb/schema COMPONENT SchemaABI)
