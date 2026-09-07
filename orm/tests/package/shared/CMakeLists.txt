cmake_minimum_required(VERSION 3.21)
project(OrmSharedPackageConsumer LANGUAGES C)

find_package(Orm CONFIG REQUIRED)

foreach(required_variable IN ITEMS ORM_PACKAGE_EXPECT_SQLITE
                                   ORM_PACKAGE_EXPECT_POSTGRESQL)
  if(NOT DEFINED ${required_variable})
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

if(NOT DEFINED Orm_WITH_SQLITE OR NOT DEFINED Orm_WITH_POSTGRESQL)
  message(FATAL_ERROR "Orm package does not export backend capabilities")
endif()
if(NOT "${Orm_WITH_SQLITE}" STREQUAL "${ORM_PACKAGE_EXPECT_SQLITE}")
  message(FATAL_ERROR
          "Orm SQLite capability is ${Orm_WITH_SQLITE}; expected ${ORM_PACKAGE_EXPECT_SQLITE}")
endif()
if(NOT "${Orm_WITH_POSTGRESQL}" STREQUAL
   "${ORM_PACKAGE_EXPECT_POSTGRESQL}")
  message(FATAL_ERROR
          "Orm PostgreSQL capability is ${Orm_WITH_POSTGRESQL}; expected ${ORM_PACKAGE_EXPECT_POSTGRESQL}")
endif()

add_executable(orm_shared_package_consumer main.c)
target_link_libraries(orm_shared_package_consumer PRIVATE Orm::C)
