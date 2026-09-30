# TurboDB.Driver.PostgreSQL.Native

Independent native deployment package for the canonical PostgreSQL
`TurboDb.Driver` Plugin.

This package is intentionally separate from `TurboDB.Native`:

- `TurboDB.Native` provides the generic `Orm::C` runtime and public headers.
- `TurboDB.Driver.PostgreSQL.Native` provides only the PostgreSQL Driver MODULE
  and its driver-private runtime deployment files.
- applications load the exact module path with
  `orm_runtime_load_driver(... expected_driver_id="postgresql")`.
- there is no directory scan, source-build fallback, legacy
  `orm_postgresql_connect()`, or static libpq linkage in the generic runtime.

Package layout:

```
sdk/
  linux-x64/
    lib/turbodb/drivers/turbodb_driver_postgresql.so
    turbodb-postgresql-driver-manifest.txt
  windows-x64/
    bin/turbodb/drivers/turbodb_driver_postgresql.dll
    bin/turbodb/drivers/<driver-private DLL closure>
    turbodb-postgresql-driver-manifest.txt
```

The package depends on the latest published stable `TurboDB.Native`, which in
turn supplies the matching generic ORM runtime and Salts SDK dependencies.

## Runtime dependencies

### Linux x64

The canonical x64-linux package is built against the shared-cache `x64-linux`
provider, which links the PostgreSQL client into the Driver MODULE statically.
The package therefore has no runtime `libpq.so` dependency and does not copy
libraries from `/lib` or `/usr/lib` into the application bundle.

### Windows x64

The package carries the non-system runtime dependency closure discovered from
the cached vcpkg PostgreSQL provider. Generic `turbo_orm.dll` and Salts DLLs
remain owned by their respective packages and are deliberately excluded from
the Driver package.

Each RID directory contains a machine-readable text manifest with:

- package/version/RID;
- canonical driver id;
- TurboDb.Driver interface contract version;
- Salts Plugin ABI version;
- producer commit;
- runtime dependency policy.
