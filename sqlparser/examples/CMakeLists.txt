add_executable(sqlparser_example parse.c)
target_link_libraries(sqlparser_example PRIVATE sqlparser)
cmake_config_target(sqlparser_example NO_INSTALL FOLDER "sqlparser/examples")
