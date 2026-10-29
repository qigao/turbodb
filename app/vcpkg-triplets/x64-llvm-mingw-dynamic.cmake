# Studio Windows x64 cross-compiler ABI. Vcpkg GUI packages and Studio
# must both use the same pinned llvm-mingw toolchain; never MSVC objects.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME MinGW)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE
    "${CMAKE_CURRENT_LIST_DIR}/../cmake/llvm-mingw-windows.cmake")

# vcpkg isolates port/compiler-detection environments. This exact compiler
# root must cross that boundary as a tracked input, otherwise the chainload
# toolchain fails before any Studio or editor source can be compiled.
set(VCPKG_ENV_PASSTHROUGH TURBODB_LLVM_MINGW_BIN)
