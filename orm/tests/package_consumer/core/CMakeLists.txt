cmake_minimum_required(VERSION 3.20)
project(orm_core_package_consumer LANGUAGES C)

find_package(Orm CONFIG REQUIRED)

add_executable(orm_core_package_consumer main.c)
target_compile_features(orm_core_package_consumer PRIVATE c_std_11)
target_link_libraries(orm_core_package_consumer PRIVATE Orm::C)
