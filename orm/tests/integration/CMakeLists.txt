add_subdirectory(sqlite)
add_subdirectory(tidesdb)

if(TARGET tidessqld)
  add_subdirectory(mysql)
endif()
