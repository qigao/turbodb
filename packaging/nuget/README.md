# TurboDB.Native

Prebuilt native TurboDB SDK for consumers that use the canonical Orm/runtime Driver API.

Package layout:

```
sdk/
  linux-x64/
  windows-x64/
```

Each RID contains the installed `Orm` CMake package and public headers.

This package intentionally does **not** bundle database-specific Driver MODULEs or native client libraries. SQLite, PostgreSQL, MySQL, MongoDB, Redis, and TidesDB deployment remains independent from the generic Orm/runtime SDK.

Dependencies:

- latest published stable `Salts.Native`
- latest published stable `SaltsUtils.Native`

Typical CMake consumption restores the matching RID trees for all three native packages, sets `SALTS_ROOT` / `SALTS_UTILS_ROOT`, then points `CMAKE_PREFIX_PATH` (or `Orm_DIR`) at the TurboDB RID tree and uses:

```cmake
find_package(Orm CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE Orm::C)
```
