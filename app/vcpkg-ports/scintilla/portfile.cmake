vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL "https://github.com/jrsoftware/scintilla.git"
  REF 0583df2dc4a988bd91d892399de73373264c13a2
)

vcpkg_install_msbuild(
  SOURCE_PATH "${SOURCE_PATH}"
  PROJECT_SUBPATH win32/Scintilla.vcxproj
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/License.txt")
file(INSTALL "${SOURCE_PATH}/include/"
     DESTINATION "${CURRENT_PACKAGES_DIR}/include/${PORT}"
     FILES_MATCHING PATTERN "*.*")
