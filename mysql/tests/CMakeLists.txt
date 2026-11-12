if(BUILD_TESTS)
add_subdirectory(session)
add_subdirectory(wire)
add_subdirectory(auth)
add_subdirectory(parameters)
endif()

if(BUILD_BENCHMARK)
  cmake_add_benchmark(NAME mysql_parameter_benchmark
    SOURCES parameters/parameter_benchmark.c
    LIBS TurboDB::MySQL Salts::TinyTest FOLDER "mysql/tests/parameters")
  add_test(NAME mysql_parameter_benchmark COMMAND mysql_parameter_benchmark)
  set_tests_properties(mysql_parameter_benchmark PROPERTIES LABELS benchmark)
endif()
