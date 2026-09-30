target_sources(turbodb_mysql PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/codec.c
  ${CMAKE_CURRENT_SOURCE_DIR}/packet.c
  ${CMAKE_CURRENT_SOURCE_DIR}/query.c
  ${CMAKE_CURRENT_SOURCE_DIR}/result.c
  ${CMAKE_CURRENT_SOURCE_DIR}/handshake.c
  ${CMAKE_CURRENT_SOURCE_DIR}/statement.c
  ${CMAKE_CURRENT_SOURCE_DIR}/row.c)
install(FILES codec.h packet.h query.h result.h handshake.h statement.h row.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mysql/wire COMPONENT MySQL)
