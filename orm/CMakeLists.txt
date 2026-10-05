cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(
    Orm
    VERSION 2.2.0
    DESCRIPTION "Typed reactive ORM for C11 with C++17 ownership wrappers"
    LANGUAGES C)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
  include(CmakeUtils)
endif()

if(NOT TARGET Salts::Plugin OR NOT TARGET Salts::PluginABI)
  message(
    FATAL_ERROR
      "ORM requires an installed Salts SDK exporting Salts::Plugin and Salts::PluginABI"
  )
endif()

set(ORM_VERSION 2.2.0)
set(ORM_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

include(GNUInstallDirs)
add_library(turbo_orm SHARED)
add_subdirectory(src/abi)
add_subdirectory(src/flow)
add_subdirectory(src/driver)
add_subdirectory(src/runtime)
add_subdirectory(src/sql)
add_library(orm_cpp INTERFACE)
add_subdirectory(driver-sdk)
target_compile_definitions(turbo_orm PRIVATE ORM_C_BUILD
                                             ORM_NATIVE_OWNER_CANDIDATE)
target_include_directories(
  turbo_orm
  PUBLIC $<BUILD_INTERFACE:${ORM_SOURCE_DIR}/include/orm>
         $<INSTALL_INTERFACE:include/shared/orm>
  PRIVATE ${ORM_SOURCE_DIR}/src/abi ${ORM_SOURCE_DIR}/src/flow
          ${ORM_SOURCE_DIR}/src/sql ${ORM_SOURCE_DIR}/src/driver
          ${ORM_SOURCE_DIR}/src/runtime)
target_link_libraries(
  turbo_orm
  PUBLIC TurboDB::Types Salts::Core Salts::CFlow
  PRIVATE Salts::CSerde Salts::DataBind Salts::Plugin Salts::CSTL
          Salts::Platform ${CMAKE_DL_LIBS})

get_target_property(ORM_CORE_LINK_CONTRACT turbo_orm LINK_LIBRARIES)
set(ORM_CORE_FORBIDDEN_LINKS SQLite::SQLite3 PostgreSQL::PostgreSQL Salts::CNet
                             TidesDB::tidesdb TurboDB::Redis)
list(REMOVE_ITEM ORM_CORE_FORBIDDEN_LINKS "")
set(ORM_CORE_ALLOWED_LINKS ${ORM_CORE_LINK_CONTRACT})
list(REMOVE_ITEM ORM_CORE_ALLOWED_LINKS ${ORM_CORE_FORBIDDEN_LINKS})
if(NOT ORM_CORE_ALLOWED_LINKS STREQUAL ORM_CORE_LINK_CONTRACT)
  message(FATAL_ERROR "generic turbo_orm links a forbidden dependency")
endif()

set_target_properties(
  turbo_orm
  PROPERTIES
             LINKER_LANGUAGE C
             C_VISIBILITY_PRESET hidden)
target_compile_features(orm_cpp INTERFACE cxx_std_17)
target_include_directories(
  orm_cpp INTERFACE $<BUILD_INTERFACE:${ORM_SOURCE_DIR}/include/orm>
                    $<INSTALL_INTERFACE:include/shared/orm>)
target_link_libraries(orm_cpp INTERFACE turbo_orm)

cmake_config_target(
  turbo_orm
  ALIAS
  Orm::C
  EXPORT_NAME
  C
  EXPORT_SET
  OrmTargets
  FOLDER
  "orm"
  RUNTIME_DEPENDENCY_SET
  OrmRuntimeDependencies
  NO_VERSION)
cmake_config_target(
  orm_cpp
  ALIAS
  Orm::Cpp
  EXPORT_NAME
  Cpp
  EXPORT_SET
  OrmTargets
  FOLDER
  "orm"
  NO_VERSION)

cmake_install_headers(
  FILES
  include/orm/orm.h
  include/orm/orm.hpp
  include/orm/orm_flow.hpp
  include/orm/orm_driver_base.h
  include/orm/orm_driver_plan.h
  include/orm/orm_driver_ops.h
  include/orm/orm_driver_storage.h
  include/orm/orm_driver_interface.h
  include/orm/orm_driver_plugin.h
  include/orm/orm_runtime.h
  include/orm/orm_sqlite.h
  include/orm/orm_tidesdb.h
  DESTINATION
  ${CMAKE_INSTALL_INCLUDEDIR}/shared/orm)

if(BUILD_TESTS OR BUILD_E2E_TESTS)
  add_subdirectory(tests)
endif()
