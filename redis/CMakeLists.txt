# Redis Client (RESP Protocol) Async Redis client implementing RESP protocol

# Public headers are selected explicitly; private headers must not be installed.
set(REDIS_HEADERS redis_export.h redis_reply.h redis_io.h redis_cflow.h redis_lua_apply.h
                  redis_lua_apply_batch.h redis_lua_apply_batch_compact.h redis_pool.h
                  redis_cluster.h redis_sentinel.h redis_stream_group.h)
file(GLOB REDIS_SOURCES CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.c")

if(NOT RE2C_EXECUTABLE)
  message(FATAL_ERROR "Redis RESP parser requires re2c")
endif()

cmake_add_grammar(
  REDIS_RESP
  LEXER_RE ${CMAKE_CURRENT_SOURCE_DIR}/parser/redis_resp_parser.re
  RE2C_EXECUTABLE "${RE2C_EXECUTABLE}"
  FOLDER "redis")

add_library(tedis SHARED ${REDIS_SOURCES} ${REDIS_RESP_LEXER_GEN} ${REDIS_HEADERS})
add_dependencies(tedis REDIS_RESP_lexer_codegen)

cmake_config_target(
  tedis
  ALIAS
  TurboDB::Redis
  FOLDER
  redis
  EXPORT_SET
  TurboDBTargets
  EXPORT_NAME
  Redis)

target_compile_definitions(tedis PRIVATE REDIS_BUILD)
if(BUILD_TESTS)
  target_compile_definitions(tedis PRIVATE REDIS_TESTING)
endif()

target_include_directories(
  tedis PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
               $<INSTALL_INTERFACE:include> $<INSTALL_INTERFACE:include/redis>
        PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(tedis PUBLIC Salts::Core Salts::CFlow)

# Platform-specific settings
if(WIN32)
  target_compile_definitions(tedis PRIVATE _CRT_SECURE_NO_WARNINGS
                                           WIN32_LEAN_AND_MEAN)
  target_link_libraries(tedis PRIVATE ws2_32)
endif()

# Tests
if(BUILD_TESTS)
  add_subdirectory(tests)
endif()

cmake_install_headers(FILES ${REDIS_HEADERS}
                      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/redis)
