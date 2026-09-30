target_sources(turbo_orm PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/orm_runtime.c
  ${CMAKE_CURRENT_SOURCE_DIR}/orm_runtime_plugin.c
  ${CMAKE_CURRENT_SOURCE_DIR}/orm_runtime_connection.c
)
