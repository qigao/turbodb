cmake_add_test(NAME sqlparser_test SOURCES parser_test.c
  LIBS sqlparser Salts::TinyTest FOLDER "sqlparser/tests")
cmake_add_test(NAME sqlparser_failure_test SOURCES failure_test.c
  LIBS sqlparser Salts::CSTL Salts::Core Salts::TinyTest
  INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../src ${CMAKE_CURRENT_BINARY_DIR}/..
  FOLDER "sqlparser/tests")
cmake_add_test(NAME sqlparser_benchmark SOURCES benchmark.c
  LIBS sqlparser Salts::TinyTest FOLDER "sqlparser/tests")
find_package(SQLite3 REQUIRED)
cmake_add_test(NAME sqlparser_dialect_test SOURCES dialect_test.c
  LIBS sqlparser SQLite::SQLite3 Salts::TinyTest FOLDER "sqlparser/tests")
cmake_add_test(NAME sqlparser_sqlite_extension_test SOURCES sqlite_extension_test.c
  LIBS sqlparser SQLite::SQLite3 Salts::TinyTest FOLDER "sqlparser/tests")

set(SQLPARSER_CORPUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../sqlite-spec")
file(GLOB_RECURSE SQLPARSER_CORPUS_FILES CONFIGURE_DEPENDS
  RELATIVE "${SQLPARSER_CORPUS_ROOT}" "${SQLPARSER_CORPUS_ROOT}/*.sql")
if(SQLPARSER_CORPUS_FILES)
  list(SORT SQLPARSER_CORPUS_FILES)
  set(SQLPARSER_CORPUS_ENTRIES "")
  foreach(path IN LISTS SQLPARSER_CORPUS_FILES)
    string(APPEND SQLPARSER_CORPUS_ENTRIES "  \"${path}\",\n")
  endforeach()
  configure_file(corpus_files.h.in corpus_files.h @ONLY)
  cmake_add_test(NAME sqlparser_corpus_test SOURCES corpus_test.c
    LIBS sqlparser Salts::Core Salts::TinyTest
    INCLUDES "${CMAKE_CURRENT_BINARY_DIR}" FOLDER "sqlparser/tests")
endif()

set(SQLPARSER_MYSQL_CORPUS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../mysql-spec")
set(SQLPARSER_MYSQL_MANIFEST "${SQLPARSER_MYSQL_CORPUS_ROOT}/manifest.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SQLPARSER_MYSQL_MANIFEST}")
file(READ "${SQLPARSER_MYSQL_MANIFEST}" SQLPARSER_MYSQL_MANIFEST_JSON)
string(JSON SQLPARSER_MYSQL_CASE_COUNT LENGTH "${SQLPARSER_MYSQL_MANIFEST_JSON}" cases)
if(SQLPARSER_MYSQL_CASE_COUNT EQUAL 0)
  message(FATAL_ERROR "MySQL SQL corpus manifest must contain test cases")
endif()
math(EXPR SQLPARSER_MYSQL_LAST_CASE "${SQLPARSER_MYSQL_CASE_COUNT} - 1")
set(SQLPARSER_MYSQL_CORPUS_ENTRIES "")
foreach(index RANGE ${SQLPARSER_MYSQL_LAST_CASE})
  string(JSON fixture GET "${SQLPARSER_MYSQL_MANIFEST_JSON}" cases ${index})
  string(APPEND SQLPARSER_MYSQL_CORPUS_ENTRIES "  {")
  foreach(field IN ITEMS file source mysql_expectation mysql_error sql_mode parser_expectation gap)
    string(JSON value GET "${fixture}" ${field})
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(APPEND SQLPARSER_MYSQL_CORPUS_ENTRIES "\"${value}\",")
  endforeach()
  foreach(field IN ITEMS first_line last_line offset length)
    string(JSON value GET "${fixture}" ${field})
    string(APPEND SQLPARSER_MYSQL_CORPUS_ENTRIES "${value},")
  endforeach()
  string(APPEND SQLPARSER_MYSQL_CORPUS_ENTRIES "},\n")
endforeach()
configure_file(mysql_corpus_files.h.in mysql_corpus_files.h @ONLY)
cmake_add_test(NAME sqlparser_mysql_corpus_test SOURCES mysql_corpus_test.c
  LIBS sqlparser Salts::Core Salts::TinyTest
  INCLUDES "${CMAKE_CURRENT_BINARY_DIR}" FOLDER "sqlparser/tests")
