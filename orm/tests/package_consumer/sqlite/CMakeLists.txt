cmake_minimum_required(VERSION 3.21)

project(OrmSQLitePackageConsumer LANGUAGES C)

find_package(Orm CONFIG REQUIRED)

if(NOT DEFINED Orm_WITH_SQLITE)
  message(FATAL_ERROR "Orm package does not export backend capabilities")
endif()
if(NOT Orm_WITH_SQLITE)
  message(FATAL_ERROR "consumer requires SQLite backend support")
endif()
