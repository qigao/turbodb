vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL "https://github.com/ScintillaOrg/lexilla.git"
  REF 1dc209de60c86cc9af615b6ebfa8466e5db4a931
)

if(VCPKG_TARGET_IS_MINGW)
  # Use the upstream language/lexer sources and vcpkg's llvm-mingw compiler.
  # The paired Scintilla dependency supplies public headers only here.
  file(COPY "${CMAKE_CURRENT_LIST_DIR}/StudioLexilla.cmake"
       DESTINATION "${SOURCE_PATH}")
  file(RENAME "${SOURCE_PATH}/StudioLexilla.cmake"
              "${SOURCE_PATH}/CMakeLists.txt")
  vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS "-DSCINTILLA_INCLUDE_DIR=${CURRENT_INSTALLED_DIR}/include/scintilla")
  vcpkg_cmake_install()
  # Fail in the producer, not later at Studio configure or LoadLibraryW.
  # Only the target ABI's authentic generated DLL name is accepted.
  if(NOT EXISTS "${CURRENT_PACKAGES_DIR}/bin/Lexilla.dll")
    message(FATAL_ERROR
      "llvm-mingw Lexilla package must install bin/Lexilla.dll, "
      "never libLexilla.dll or a differently compiled ABI")
  endif()
  if(NOT VCPKG_BUILD_TYPE STREQUAL "release" AND
     NOT EXISTS "${CURRENT_PACKAGES_DIR}/debug/bin/Lexilla.dll")
    message(FATAL_ERROR "llvm-mingw Lexilla missing debug/bin/Lexilla.dll")
  endif()
  vcpkg_copy_pdbs()
else()
  # The pre-existing MSVC path remains valid, without supplying ATL.
  vcpkg_replace_string(
    "${SOURCE_PATH}/src/Lexilla.vcxproj"
    "..\\..\\scintilla\\include"
    "$(VcpkgInstalledDir)\\$(VcpkgTriplet)\\include\\scintilla")
  vcpkg_install_msbuild(
    SOURCE_PATH "${SOURCE_PATH}"
    PROJECT_SUBPATH src/Lexilla.vcxproj)
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/License.txt")
file(INSTALL "${SOURCE_PATH}/include/"
     DESTINATION "${CURRENT_PACKAGES_DIR}/include/${PORT}"
     FILES_MATCHING PATTERN "*.*")
file(INSTALL "${SOURCE_PATH}/lexlib/"
     DESTINATION "${CURRENT_PACKAGES_DIR}/include/${PORT}/lexlib"
     FILES_MATCHING PATTERN "*.h")
