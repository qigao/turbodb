cmake_minimum_required(VERSION 3.21)
project(OrmSharedPackageConsumer LANGUAGES C)

find_package(Orm CONFIG REQUIRED)

add_executable(orm_shared_package_consumer main.c)
target_link_libraries(orm_shared_package_consumer PRIVATE Orm::C)
