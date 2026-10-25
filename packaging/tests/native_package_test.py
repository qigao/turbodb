"""Run with TURBODB_NUPKG set via unittest discover -s packaging/tests -p '*_test.py'."""

import os
import unittest
import xml.etree.ElementTree as ET
from zipfile import ZipFile


_LEGACY_CRYPTO_PREFIXES = (
    "share/openssl/",
    "share/boringssl/",
    "include/openssl/",
    "lib/crypto.",
    "lib/ssl.",
    "lib/libcrypto.",
    "lib/libssl.",
    "lib/pkgconfig/libcrypto.",
    "lib/pkgconfig/libssl.",
    "debug/lib/crypto.",
    "debug/lib/ssl.",
    "debug/lib/libcrypto.",
    "debug/lib/libssl.",
    "debug/lib/pkgconfig/libcrypto.",
    "debug/lib/pkgconfig/libssl.",
)

_WINDOWS_RUNTIME_DLLS = {
    "libpq.dll",
    "sqlite3.dll",
    "tedis.dll",
    "turbo_orm.dll",
}

_WINDOWS_DRIVER_DLLS = {
    "libpq.dll",
    "sqlite3.dll",
    "turbodb_driver_mysql.dll",
    "turbodb_driver_postgresql.dll",
    "turbodb_driver_sqlite.dll",
}


def _find_legacy_crypto_files(files, rid):
    prefixes = tuple(
        f"sdk/{rid}/{relative}".casefold()
        for relative in _LEGACY_CRYPTO_PREFIXES
    )
    return sorted(
        path for path in files if path.casefold().startswith(prefixes)
    )


class NativePackageTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.package = ZipFile(os.environ["TURBODB_NUPKG"])
        cls.addClassCleanup(cls.package.close)
        cls.files = {item.filename for item in cls.package.infolist() if not item.is_dir()}

    def test_sdk_contents(self):
        common = (
            "lib/cmake/TurboDB/TurboDBConfig.cmake",
            "lib/cmake/TurboDB/OrmTargets.cmake",
            "lib/cmake/TurboDB/TurboDBTargets.cmake",
            "include/shared/orm/orm_runtime.h",
            "include/shared/orm/orm_flow.hpp",
            "include/mysql/session_async.h",
            "include/mysql/session_script.h",
            "include/redis/redis_io.h",
        )
        for rid in ("linux-x64", "linux-arm64", "macos-arm64", "windows-x64", "android-arm64-v8a"):
            windows = rid == "windows-x64"
            macos = rid == "macos-arm64"
            host = rid != "android-arm64-v8a"
            shared_suffix = "dylib" if macos else "so"
            core = "bin/turbo_orm.dll" if windows else f"lib/libturbo_orm.{shared_suffix}"
            suffix = "dll" if windows else "so"
            dbtools = (("bin/dbtools.exe" if windows else "bin/dbtools",)
                       if host else ())
            drivers = tuple(
                f"lib/turbodb/drivers/turbodb_driver_{driver}.{suffix}"
                for driver in ("sqlite", "postgresql", "mysql")
            )
            for relative in (*common, core, *dbtools, *drivers):
                path = f"sdk/{rid}/{relative}"
                with self.subTest(path=path):
                    self.assertTrue(path in self.files, f"Missing SDK file: {path}")
                    self.assertGreater(self.package.getinfo(path).file_size, 0)
            for executable in ("bin/dbtools", "bin/dbtools.exe"):
                if not host:
                    self.assertNotIn(f"sdk/{rid}/{executable}", self.files)
            for driver in ("sqlite", "postgresql", "mysql"):
                for extension in ("", ".exe"):
                    self.assertNotIn(f"sdk/{rid}/bin/turbodb-{driver}{extension}",
                                     self.files)
            redis_driver = (
                f"sdk/{rid}/lib/turbodb/drivers/turbodb_driver_redis.{suffix}"
            )
            self.assertNotIn(redis_driver, self.files)
            self.assertNotIn(
                f"sdk/{rid}/lib/turbodb/drivers/turbodb_driver_tidesdb.{suffix}",
                self.files,
            )
            self.assertNotIn(f"sdk/{rid}/include/shared/orm/orm_tidesdb.h", self.files)
            leaked_crypto = _find_legacy_crypto_files(self.files, rid)
            self.assertEqual([], leaked_crypto,
                             f"Legacy OpenSSL/BoringSSL files leaked: {leaked_crypto}")

        windows_bin = "sdk/windows-x64/bin/"
        runtime_dlls = {
            path[len(windows_bin):].casefold()
            for path in self.files
            if path.casefold().startswith(windows_bin)
            and "/" not in path[len(windows_bin):]
            and path.casefold().endswith(".dll")
        }
        self.assertEqual(_WINDOWS_RUNTIME_DLLS, runtime_dlls)

        windows_drivers = "sdk/windows-x64/lib/turbodb/drivers/"
        driver_dlls = {
            path[len(windows_drivers):].casefold()
            for path in self.files
            if path.casefold().startswith(windows_drivers)
            and "/" not in path[len(windows_drivers):]
            and path.casefold().endswith(".dll")
        }
        self.assertEqual(_WINDOWS_DRIVER_DLLS, driver_dlls)

    def test_tides_engines_are_not_packaged(self):
        leaked = sorted(path for path in self.files
                        if path.startswith("sdk/")
                        and any(name in path.casefold()
                                for name in ("tidesdb", "tidessql")))
        self.assertEqual([], leaked,
                         f"TidesDB/TidesSQL files must not be shipped: {leaked}")

    def test_github_dependencies_are_not_embedded(self):
        manifest = ET.fromstring(self.package.read("TurboDB.Native.nuspec"))
        dependencies = {
            node.attrib["id"] for node in manifest.iter()
            if node.tag.rsplit("}", 1)[-1] == "dependency"
        }
        forbidden = {"Salts.Native", "SaltsUtils.Native"}
        self.assertTrue(forbidden.isdisjoint(dependencies),
                        f"GitHub package dependency metadata must be omitted: {dependencies}")


if __name__ == "__main__":
    unittest.main()
