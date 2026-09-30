target_sources(turbo_orm PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/orm_core.c
  ${CMAKE_CURRENT_SOURCE_DIR}/orm_explain.c
  ${CMAKE_CURRENT_SOURCE_DIR}/orm_metadata.c
  ${CMAKE_CURRENT_SOURCE_DIR}/orm_owner.c
  ${CMAKE_CURRENT_SOURCE_DIR}/orm_plan.c
  ${CMAKE_CURRENT_SOURCE_DIR}/orm_result.c
)
