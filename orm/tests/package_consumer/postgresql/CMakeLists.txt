cmake_minimum_required(VERSION 3.21)

project(OrmPostgreSQLPackageConsumer LANGUAGES C CXX)

find_package(Orm CONFIG REQUIRED)

if(NOT TARGET Orm::PostgreSQL)
  message(FATAL_ERROR
          "legacy PostgreSQL consumer requires the Orm::PostgreSQL compatibility target")
endif()
if(DEFINED Orm_WITH_SQLITE OR DEFINED Orm_WITH_POSTGRESQL)
  message(FATAL_ERROR
          "Orm package must not expose legacy backend capability flags")
endif()

add_executable(orm_postgresql_c_consumer main.c)
target_compile_features(orm_postgresql_c_consumer PRIVATE c_std_11)
target_link_libraries(orm_postgresql_c_consumer PRIVATE Orm::PostgreSQL)

add_executable(orm_postgresql_cpp_consumer main.cpp)
target_compile_features(orm_postgresql_cpp_consumer PRIVATE cxx_std_17)
target_link_libraries(orm_postgresql_cpp_consumer PRIVATE Orm::PostgreSQL)
