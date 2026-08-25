cmake_minimum_required(VERSION 3.20)
project(orm_postgresql_package_consumer LANGUAGES C)

find_package(OrmPostgreSQL CONFIG REQUIRED)

add_executable(orm_postgresql_package_consumer main.c)
target_compile_features(orm_postgresql_package_consumer PRIVATE c_std_11)
target_link_libraries(orm_postgresql_package_consumer
                      PRIVATE OrmPostgreSQL::Driver)
