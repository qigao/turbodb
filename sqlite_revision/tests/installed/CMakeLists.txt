cmake_minimum_required(VERSION 3.21)
project(TurboDBRevisionInstalledConsumer LANGUAGES C)

find_package(TurboDB CONFIG REQUIRED)
if(NOT TARGET TurboDB::SqliteRevision)
  message(FATAL_ERROR "Installed TurboDB SDK does not export TurboDB::SqliteRevision")
endif()

add_executable(turbodb_revision_installed_consumer main.c)
target_compile_features(turbodb_revision_installed_consumer PRIVATE c_std_11)
target_link_libraries(turbodb_revision_installed_consumer PRIVATE TurboDB::SqliteRevision)
