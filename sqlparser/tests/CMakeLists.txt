cmake_add_test(NAME sqlparser_test SOURCES parser_test.c
  LIBS sqlparser Salts::TinyTest FOLDER "sqlparser/tests")
cmake_add_test(NAME sqlparser_failure_test SOURCES failure_test.c
  LIBS sqlparser CMeta::CSTL Salts::Core Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_BINARY_DIR}/..
  FOLDER "sqlparser/tests")
cmake_add_test(NAME sqlparser_benchmark SOURCES benchmark.c
  LIBS sqlparser Salts::TinyTest FOLDER "sqlparser/tests")
find_package(unofficial-sqlite3 CONFIG REQUIRED)
cmake_add_test(NAME sqlparser_dialect_test SOURCES dialect_test.c
  LIBS sqlparser unofficial::sqlite3::sqlite3 Salts::TinyTest FOLDER "sqlparser/tests")
cmake_add_test(NAME sqlparser_sqlite_extension_test SOURCES sqlite_extension_test.c
  LIBS sqlparser unofficial::sqlite3::sqlite3 Salts::TinyTest FOLDER "sqlparser/tests")

include("../../cmake/SqlCorpus.cmake")
turbodb_sqlite_corpus(
  "${CMAKE_CURRENT_SOURCE_DIR}/../sqlite-spec"
  "${CMAKE_CURRENT_SOURCE_DIR}/corpus_files.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/corpus_files.h")
if(SQLPARSER_CORPUS_FILES)
  cmake_add_test(NAME sqlparser_corpus_test SOURCES corpus_test.c
    LIBS sqlparser Salts::Core Salts::TinyTest
    INCLUDES "${CMAKE_CURRENT_BINARY_DIR}" FOLDER "sqlparser/tests")
endif()

set(SQLPARSER_MYSQL_CORPUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../mysql-spec")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${SQLPARSER_MYSQL_CORPUS_ROOT}/manifest.json")
turbodb_mysql_corpus(
  "${SQLPARSER_MYSQL_CORPUS_ROOT}"
  "${CMAKE_CURRENT_SOURCE_DIR}/mysql_corpus_files.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/mysql_corpus_files.h")
cmake_add_test(NAME sqlparser_mysql_corpus_test SOURCES mysql_corpus_test.c
  LIBS sqlparser Salts::Core Salts::TinyTest
  INCLUDES "${CMAKE_CURRENT_BINARY_DIR}" FOLDER "sqlparser/tests")
