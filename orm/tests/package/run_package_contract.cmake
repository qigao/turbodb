cmake_minimum_required(VERSION 3.21)

foreach(required_variable IN ITEMS ORM_PACKAGE_TEST_SOURCE_DIR
                                   ORM_PACKAGE_TEST_BINARY_ROOT
                                   ORM_PACKAGE_TEST_BUILD_DIR
                                   ORM_PACKAGE_TEST_INSTALL_SCRIPT
                                   ORM_PACKAGE_TEST_TURBOUTILS_DIR)
  if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
    message(FATAL_ERROR "${required_variable} is required")
  endif()
endforeach()

set(test_root "${ORM_PACKAGE_TEST_BINARY_ROOT}/shared")
set(test_prefix "${test_root}/prefix")
set(package_dir "${test_prefix}/lib/cmake/Orm")
set(consumer_build_dir "${test_root}/consumer-build")

file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}")

if(DEFINED ORM_PACKAGE_TEST_EXPECTED_RUNTIME_DIR AND
   NOT "${ORM_PACKAGE_TEST_EXPECTED_RUNTIME_DIR}" STREQUAL "")
  file(TO_CMAKE_PATH "${ORM_PACKAGE_TEST_EXPECTED_RUNTIME_DIR}"
       expected_runtime_dir)
  file(READ "${ORM_PACKAGE_TEST_INSTALL_SCRIPT}" install_script)
  string(FIND "${install_script}" "${expected_runtime_dir}"
         expected_runtime_dir_position)
  if(expected_runtime_dir_position EQUAL -1)
    message(FATAL_ERROR
            "Orm install script omits runtime directory: ${expected_runtime_dir}")
  endif()
endif()

set(install_command "${CMAKE_COMMAND}" --install
                    "${ORM_PACKAGE_TEST_BUILD_DIR}" --prefix "${test_prefix}")
if(DEFINED ORM_PACKAGE_TEST_CONFIG AND
   NOT "${ORM_PACKAGE_TEST_CONFIG}" STREQUAL "")
  list(APPEND install_command --config "${ORM_PACKAGE_TEST_CONFIG}")
endif()

set(original_install_path "$ENV{PATH}")
if(WIN32)
  set(filtered_install_path_entries)
  set(excluded_install_path_roots "${ORM_PACKAGE_TEST_BUILD_DIR}")
  if(DEFINED ORM_PACKAGE_TEST_VCPKG_INSTALLED_DIR AND
     NOT "${ORM_PACKAGE_TEST_VCPKG_INSTALLED_DIR}" STREQUAL "")
    list(APPEND excluded_install_path_roots
         "${ORM_PACKAGE_TEST_VCPKG_INSTALLED_DIR}")
  endif()
  set(install_path_entries "$ENV{PATH}")
  foreach(install_path_entry IN LISTS install_path_entries)
    file(TO_CMAKE_PATH "${install_path_entry}" install_path_entry_normalized)
    string(TOLOWER "${install_path_entry_normalized}/"
           install_path_entry_comparable)
    set(exclude_install_path_entry FALSE)
    foreach(excluded_install_path_root IN LISTS excluded_install_path_roots)
      file(TO_CMAKE_PATH "${excluded_install_path_root}"
           excluded_install_path_root_normalized)
      string(TOLOWER "${excluded_install_path_root_normalized}/"
             excluded_install_path_root_comparable)
      string(FIND "${install_path_entry_comparable}"
             "${excluded_install_path_root_comparable}"
             excluded_install_path_root_position)
      if(excluded_install_path_root_position EQUAL 0)
        set(exclude_install_path_entry TRUE)
        break()
      endif()
    endforeach()
    if(NOT exclude_install_path_entry)
      list(APPEND filtered_install_path_entries "${install_path_entry}")
    endif()
  endforeach()
  list(JOIN filtered_install_path_entries ";" filtered_install_path)
  set(ENV{PATH} "${filtered_install_path}")
endif()

execute_process(
  COMMAND ${install_command}
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_stdout
  ERROR_VARIABLE install_stderr)

set(ENV{PATH} "${original_install_path}")

if(NOT install_result EQUAL 0)
  message(FATAL_ERROR
          "Orm package staging install failed\n"
          "stdout:\n${install_stdout}\n"
          "stderr:\n${install_stderr}")
endif()

if(NOT EXISTS "${package_dir}/OrmConfig.cmake" OR
   NOT EXISTS "${package_dir}/OrmTargets.cmake")
  message(FATAL_ERROR "staged install did not produce the Orm CMake package")
endif()

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${ORM_PACKAGE_TEST_SOURCE_DIR}"
    -B "${consumer_build_dir}"
    "-DOrm_DIR=${package_dir}"
    "-DTurboUtils_DIR=${ORM_PACKAGE_TEST_TURBOUTILS_DIR}"
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE)

if(DEFINED ORM_PACKAGE_TEST_GENERATOR AND
   NOT "${ORM_PACKAGE_TEST_GENERATOR}" STREQUAL "")
  list(APPEND configure_command -G "${ORM_PACKAGE_TEST_GENERATOR}")
endif()
if(DEFINED ORM_PACKAGE_TEST_C_COMPILER AND
   NOT "${ORM_PACKAGE_TEST_C_COMPILER}" STREQUAL "")
  list(APPEND configure_command
       "-DCMAKE_C_COMPILER=${ORM_PACKAGE_TEST_C_COMPILER}")
endif()
if(DEFINED ORM_PACKAGE_TEST_CONFIG AND
   NOT "${ORM_PACKAGE_TEST_CONFIG}" STREQUAL "")
  list(APPEND configure_command
       "-DCMAKE_BUILD_TYPE=${ORM_PACKAGE_TEST_CONFIG}")
endif()

list(APPEND configure_command
     -DCMAKE_DISABLE_FIND_PACKAGE_PostgreSQL=TRUE
     -DCMAKE_DISABLE_FIND_PACKAGE_SQLite3=TRUE
     -DCMAKE_DISABLE_FIND_PACKAGE_TurboDB=TRUE
     -DCMAKE_DISABLE_FIND_PACKAGE_TidesDB=TRUE
     -DCMAKE_DISABLE_FIND_PACKAGE_mongoc-1.0=TRUE)

execute_process(
  COMMAND ${configure_command}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_stdout
  ERROR_VARIABLE configure_stderr)

if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR
          "shared Orm package consumer configure failed\n"
          "stdout:\n${configure_stdout}\n"
          "stderr:\n${configure_stderr}")
endif()

set(build_command "${CMAKE_COMMAND}" --build "${consumer_build_dir}")
if(DEFINED ORM_PACKAGE_TEST_CONFIG AND
   NOT "${ORM_PACKAGE_TEST_CONFIG}" STREQUAL "")
  list(APPEND build_command --config "${ORM_PACKAGE_TEST_CONFIG}")
endif()

execute_process(
  COMMAND ${build_command}
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_stdout
  ERROR_VARIABLE build_stderr)

if(NOT build_result EQUAL 0)
  message(FATAL_ERROR
          "shared Orm package consumer build failed\n"
          "stdout:\n${build_stdout}\n"
          "stderr:\n${build_stderr}")
endif()
