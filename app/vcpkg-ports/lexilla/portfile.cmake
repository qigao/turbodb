vcpkg_from_git(
  OUT_SOURCE_PATH SOURCE_PATH
  URL "https://github.com/ScintillaOrg/lexilla.git"
  REF 1dc209de60c86cc9af615b6ebfa8466e5db4a931
)

# Upstream expects a sibling Scintilla checkout. In vcpkg the Scintilla headers
# are installed by the dependent overlay port, so point MSBuild at that prefix.
vcpkg_replace_string(
  "${SOURCE_PATH}/src/Lexilla.vcxproj"
  "..\\..\\scintilla\\include"
  "$(VcpkgInstalledDir)\\$(VcpkgTriplet)\\include\\scintilla")

vcpkg_install_msbuild(
  SOURCE_PATH "${SOURCE_PATH}"
  PROJECT_SUBPATH src/Lexilla.vcxproj
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/License.txt")
file(INSTALL "${SOURCE_PATH}/include/"
     DESTINATION "${CURRENT_PACKAGES_DIR}/include/${PORT}"
     FILES_MATCHING PATTERN "*.*")
file(INSTALL "${SOURCE_PATH}/lexlib/"
     DESTINATION "${CURRENT_PACKAGES_DIR}/include/${PORT}/lexlib"
     FILES_MATCHING PATTERN "*.h")
