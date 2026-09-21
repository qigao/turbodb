"""Explicit source-only diagnostic; not a Salts package/SDK CMake acceptance run."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys

SALTS_COMMIT = "c4197712261a563ed7e238152b34cb50a2ef98a9"


def run(argv: list[str], *, env: dict[str, str] | None = None) -> int:
    print("+ " + " ".join(argv), flush=True)
    return subprocess.run(argv, check=False, env=env).returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--salts-source", required=True, type=Path)
    parser.add_argument("--contract-source", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--config", choices=("debug", "release"), required=True)
    parser.add_argument("--test-source", type=Path,
                        default=Path("orm/tests/driver/orm_driver_prefix_test.c"))
    parser.add_argument("--layout-source", type=Path,
                        default=Path("orm/tests/driver/orm_driver_layout_test.cpp"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    salts = args.salts_source.resolve(strict=True)
    contract = args.contract_source.resolve(strict=True)
    test = args.test_source.resolve(strict=True)
    layout_source = args.layout_source.resolve(strict=True)
    if any(not p.is_relative_to(root) for p in (contract, test, layout_source)):
        parser.error("contract and test sources must belong to this checkout")
    head = subprocess.check_output(
        ["git", "-C", str(salts), "rev-parse", "HEAD"], text=True).strip()
    if head != SALTS_COMMIT:
        parser.error(f"Salts head mismatch: expected {SALTS_COMMIT}, got {head}")
    output = args.build_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cc, cxx = os.environ.get("CC", "cc"), os.environ.get("CXX", "c++")
    run([cc, "--version"])
    run([cxx, "--version"])
    print(f"mode=source-only config={args.config} salts={head}", flush=True)
    includes = ["-I" + str(root / "orm/include/orm"),
                "-I" + str(root / "orm/src/driver")]
    modules = ("tinytest", "cmeta", "cflow", "cstl", "cserde", "cbind", "utils",
               "platform", "concurrency", "coroutine", "native-io", "vendor/sds")
    for module in modules:
        directory = salts / module
        if not directory.is_dir():
            parser.error(f"required Salts directory absent: {directory}")
        for candidate in (directory / "include", directory):
            if candidate.is_dir():
                includes += ["-isystem", str(candidate)]
    includes += ["-isystem", str(salts)]
    flags = ["-g", "-Wall", "-Wextra", "-Werror", "-DORM_C_STATIC"]
    if args.config == "debug":
        flags += ["-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    else:
        flags += ["-O2"]
    executable = output / test.stem
    rc = run([cc, "-std=c11", *flags, *includes, str(contract), str(test),
              str(salts / "tinytest/src/tinytest.c"),
              str(salts / "tinytest/src/tinymock.c"), "-pthread", "-lm",
              "-o", str(executable)])
    if rc:
        return rc
    layout = output / layout_source.stem
    rc = run([cxx, "-std=c++17", *flags, *includes,
              str(layout_source), "-o", str(layout)])
    if rc:
        return rc
    rc = run([str(layout)])
    if rc:
        return rc
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    return run([str(executable)], env=env)


if __name__ == "__main__":
    sys.exit(main())
