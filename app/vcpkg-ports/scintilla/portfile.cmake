vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL "https://github.com/jrsoftware/scintilla.git"
  REF 0583df2dc4a988bd91d892399de73373264c13a2
)

if(VCPKG_TARGET_IS_MINGW)
  # This upstream revision uses std::string/std::wstring in these two
  # Windows implementation units without including <string> itself.
  # MSVC's transitive includes hide the issue, but llvm-mingw libc++
  # intentionally does not. Do not remove GDI/Direct2D functionality.
  foreach(_studio_src IN ITEMS win32/ListBox.cxx win32/SurfaceD2D.cxx)
    set(_studio_src_path "${SOURCE_PATH}/${_studio_src}")
    file(READ "${_studio_src_path}" _studio_src_body)
    string(FIND "${_studio_src_body}" "#include <string_view>" _studio_include)
    if(_studio_include EQUAL -1)
      message(FATAL_ERROR "Pinned Scintilla <string_view> include anchor changed: ${_studio_src}")
    endif()
    vcpkg_replace_string("${_studio_src_path}"
      "#include <string_view>"
      "#include <string>\n#include <string_view>")
  endforeach()
  # The upstream win32/makefile already supports MinGW and Clang; compile
  # the same unchanged C++ source set with vcpkg's selected llvm-mingw
  # toolchain. Never link an MSVC-ABI Scintilla binary to Studio.
  file(COPY "${CMAKE_CURRENT_LIST_DIR}/StudioScintilla.cmake"
       DESTINATION "${SOURCE_PATH}")
  file(RENAME "${SOURCE_PATH}/StudioScintilla.cmake"
              "${SOURCE_PATH}/CMakeLists.txt")
  vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
  vcpkg_cmake_install()
  vcpkg_copy_pdbs()
else()
  vcpkg_install_msbuild(
    SOURCE_PATH "${SOURCE_PATH}"
    PROJECT_SUBPATH win32/Scintilla.vcxproj)
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/License.txt")
file(INSTALL "${SOURCE_PATH}/include/"
     DESTINATION "${CURRENT_PACKAGES_DIR}/include/${PORT}"
     FILES_MATCHING PATTERN "*.*")
