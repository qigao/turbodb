cmake_minimum_required(VERSION 3.24)
project(TurboDBInstalledConsumer LANGUAGES C CXX)
unset(TurboDB_DIR CACHE)
find_package(TurboDB CONFIG REQUIRED PATHS "$ENV{TURBODB_ROOT}"
  NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
add_executable(turbodb_installed_consumer main.c)
target_link_libraries(turbodb_installed_consumer PRIVATE Orm::C TurboDB::MySQL)
target_compile_features(turbodb_installed_consumer PRIVATE c_std_11)
target_compile_definitions(turbodb_installed_consumer PRIVATE
  SDK_DRIVER_DIR="${TurboDB_DRIVER_DIR}" SDK_MODULE_SUFFIX="${CMAKE_SHARED_MODULE_SUFFIX}")
enable_testing()
add_test(NAME installed_orm_drivers COMMAND turbodb_installed_consumer)
set_tests_properties(installed_orm_drivers PROPERTIES TIMEOUT 30)
