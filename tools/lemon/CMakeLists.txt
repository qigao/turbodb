add_executable(lemon lemon.c)

set_target_properties(lemon PROPERTIES FOLDER tools)
set(LEMPAR
    "${CMAKE_SOURCE_DIR}/tools/lemon/lempar.c"
    CACHE PATH "Path to lemon parser template")
