add_executable(tidessql_example query.c)
target_link_libraries(tidessql_example PRIVATE TurboDB::TidesSQL)
set_target_properties(tidessql_example PROPERTIES LINKER_LANGUAGE CXX)
cmake_config_target(tidessql_example FOLDER "tidessql/examples" NO_INSTALL)
