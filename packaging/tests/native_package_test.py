"""Run with TURBODB_NUPKG set via unittest discover -s packaging/tests -p '*_test.py'."""

import os
import unittest
import xml.etree.ElementTree as ET
from zipfile import ZipFile


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
        for rid in ("linux-x64", "windows-x64", "macos-arm64", "android-arm64-v8a"):
            windows = rid == "windows-x64"
            core = "bin/turbo_orm.dll" if windows else "lib/libturbo_orm.so"
            suffix = "dll" if windows else ("dylib" if rid == "macos-arm64" else "so")
            drivers = tuple(
                f"lib/turbodb/drivers/turbodb_driver_{driver}.{suffix}"
                for driver in ("sqlite", "postgresql", "mysql", "redis", "tidesdb")
            )
            for relative in (*common, core, *drivers):
                path = f"sdk/{rid}/{relative}"
                with self.subTest(path=path):
                    self.assertTrue(path in self.files, f"Missing SDK file: {path}")
                    self.assertGreater(self.package.getinfo(path).file_size, 0)

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
