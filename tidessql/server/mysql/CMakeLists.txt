add_library(tidessql_mysql_wire STATIC
  frame.c command.c response.c handshake.c
  ${PROJECT_SOURCE_DIR}/mysql/wire/codec.c
  ${PROJECT_SOURCE_DIR}/mysql/wire/packet.c)
target_include_directories(tidessql_mysql_wire PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/mysql)
target_link_libraries(tidessql_mysql_wire PUBLIC TurboDB::Types)
cmake_config_target(tidessql_mysql_wire NO_INSTALL FOLDER "tidessql/server/mysql")

add_library(tidessql_mysql_frontend STATIC
  registry.c dispatch.c password.c authenticate.c frontend.c server.c)
target_include_directories(tidessql_mysql_frontend PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(tidessql_mysql_frontend PUBLIC
  tidessql_mysql_wire TurboDB::TidesSQL Salts::CSTL Salts::CNet
  PRIVATE GmSSL::GmSSL)
cmake_config_target(tidessql_mysql_frontend NO_INSTALL FOLDER "tidessql/server/mysql")
