cmake_minimum_required(VERSION 3.21)
project(OrmSharedPackageConsumer LANGUAGES C CXX)

find_package(Orm CONFIG REQUIRED)

if(NOT DEFINED ORM_PACKAGE_EXPECT_LEGACY_POSTGRESQL)
  message(FATAL_ERROR "ORM_PACKAGE_EXPECT_LEGACY_POSTGRESQL is required")
endif()

if(NOT TARGET Orm::C OR NOT TARGET Orm::Cpp)
  message(FATAL_ERROR "Orm package must export generic Orm::C and Orm::Cpp")
endif()
if(DEFINED Orm_WITH_SQLITE OR DEFINED Orm_WITH_POSTGRESQL)
  message(FATAL_ERROR
          "generic Orm package must not export build-time backend capability flags")
endif()
if(ORM_PACKAGE_EXPECT_LEGACY_POSTGRESQL AND NOT TARGET Orm::PostgreSQL)
  message(FATAL_ERROR "expected legacy Orm::PostgreSQL compatibility target")
endif()
if(NOT ORM_PACKAGE_EXPECT_LEGACY_POSTGRESQL AND TARGET Orm::PostgreSQL)
  message(FATAL_ERROR "unexpected legacy Orm::PostgreSQL compatibility target")
endif()

add_executable(orm_shared_package_consumer main.c)
target_link_libraries(orm_shared_package_consumer PRIVATE Orm::C)

add_executable(orm_shared_package_cpp_consumer main.cpp)
target_compile_features(orm_shared_package_cpp_consumer PRIVATE cxx_std_17)
target_link_libraries(orm_shared_package_cpp_consumer PRIVATE Orm::Cpp)
