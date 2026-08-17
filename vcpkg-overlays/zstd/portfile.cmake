vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO facebook/zstd
    REF 5c7b7bad26808e6b40ac3b3d0075466e27738a9d
    SHA512 2dcc3b4a7d9af46d70aa560cbfa2a24919b039b847f9ffbf183548e26093d2c288afa2aaccc1a5d90237fc1b31ae5e1fa4e132d2c656f0a65a2faeb5d09f775c
    HEAD_REF dev
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/build/cmake"
    OPTIONS
        -DZSTD_BUILD_SHARED=OFF
        -DZSTD_BUILD_STATIC=ON
        -DZSTD_LEGACY_SUPPORT=ON
        -DZSTD_BUILD_PROGRAMS=OFF
        -DZSTD_BUILD_TESTS=OFF
        -DZSTD_BUILD_CONTRIB=OFF
        -DZSTD_MULTITHREAD_SUPPORT=ON
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/zstd)
vcpkg_fixup_pkgconfig()

if(VCPKG_TARGET_IS_WINDOWS)
    foreach(ZSTD_PKGCONFIG_FILE IN ITEMS
            "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/libzstd.pc"
            "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/libzstd.pc")
        if(EXISTS "${ZSTD_PKGCONFIG_FILE}")
            vcpkg_replace_string("${ZSTD_PKGCONFIG_FILE}"
                                 " -lzstd" " -lzstd_static")
        endif()
    endforeach()
endif()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

vcpkg_install_copyright(
    COMMENT "Zstandard is dual licensed under BSD-3-Clause and GPL-2.0-only."
    FILE_LIST
        "${SOURCE_PATH}/LICENSE"
        "${SOURCE_PATH}/COPYING"
)
