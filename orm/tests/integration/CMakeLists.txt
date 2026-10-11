add_subdirectory(sqlite)

if(TARGET tidessqld)
  add_subdirectory(mysql)
endif()
