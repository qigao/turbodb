# This target publishes the contract only; loading belongs to Salts::Plugin.
add_library(orm_driver_abi INTERFACE)
add_library(Orm::DriverABI ALIAS orm_driver_abi)
target_include_directories(
  orm_driver_abi INTERFACE $<BUILD_INTERFACE:${ORM_SOURCE_DIR}/include/orm>
                           $<INSTALL_INTERFACE:include/shared/orm>)
target_link_libraries(orm_driver_abi INTERFACE Salts::PluginABI Salts::Core
                                               Salts::CFlow Salts::CSerde)
set_target_properties(orm_driver_abi PROPERTIES EXPORT_NAME DriverABI FOLDER
                                                                      "orm")
install(TARGETS orm_driver_abi EXPORT OrmTargets)
