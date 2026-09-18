"""Build real pinned Salts packages, then run Task 1 through SDK CMake/CTest."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys

SALTS_COMMIT = "c4197712261a563ed7e238152b34cb50a2ef98a9"
EXPECTED_TESTS = {"orm_driver_prefix", "orm_driver_layout"}
EXPECTED_CASES = 18


def run(argv: list[str], cwd: Path, env: dict[str, str], log: Path) -> str:
    """Keep complete command output and propagate the first real failure."""
    command = "+ " + subprocess.list2cmdline(argv)
    print(command, flush=True)
    with log.open("w", encoding="utf-8") as stream:
        stream.write(f"cwd={cwd}\n{command}\n")
        stream.flush()
        result = subprocess.run(argv, cwd=cwd, env=env, stdout=stream,
                                stderr=subprocess.STDOUT, check=False)
    text = log.read_text(encoding="utf-8", errors="replace")
    print(text, flush=True)
    print(f"exit_code={result.returncode}", flush=True)
    if result.returncode:
        raise subprocess.CalledProcessError(result.returncode, argv)
    return text.split("\n", 2)[2]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--salts-source", required=True, type=Path)
    parser.add_argument("--config", choices=("debug", "release"), required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    salts = args.salts_source.resolve(strict=True)
    system = {"Linux": "linux", "Windows": "windows"}.get(platform.system())
    if system is None or platform.machine().lower() not in ("x86_64", "amd64"):
        parser.error("this package gate requires native Linux/Windows x64")
    evidence = root / "evidence" / f"package-{system}-{args.config}"
    evidence.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    env["PROJECT_ROOT"] = str(root)
    head = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"],
                                   text=True).strip()
    salts_head = subprocess.check_output(
        ["git", "-C", str(salts), "rev-parse", "HEAD"], text=True).strip()
    if salts_head != SALTS_COMMIT:
        parser.error(f"Salts head mismatch: {salts_head}")
    env["ASAN_OPTIONS"] = ("halt_on_error=1" if system == "windows" else
                           "detect_leaks=1:halt_on_error=1")
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    upstream_platform = "win" if system == "windows" else "linux"
    upstream_profile = "dev" if args.config == "debug" else "release"
    salts_preset = f"{upstream_platform}-{upstream_profile}-user"
    sdk_preset = f"sdk-{system}-{args.config}"
    prefix = root / "external/pkgs/salts" / args.config
    env["SALTS_ROOT"] = str(prefix)
    sdk_root = root / "orm/driver-sdk"
    sdk_build = root / "build" / sdk_preset
    if system == "windows":
        salts_build = salts / "build" / ("Msvc" if args.config == "debug" else
                                         "Msvc-Release")
    else:
        salts_build = salts / "build" / f"linux-gcc-{args.config}"
    manifest = {
        "head": head, "salts_head": salts_head, "system": platform.platform(),
        "arch": platform.machine(), "profile": args.config,
        "salts_preset": salts_preset, "sdk_preset": sdk_preset,
        "mode": "installed-package", "status": "not-completed",
    }
    manifest_path = evidence / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    run(["git", "diff", "--exit-code"], salts, env, evidence / "source-before.log")
    run(["cmake", "--version"], root, env, evidence / "cmake-version.log")
    run(["cmake", "--preset", salts_preset, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"],
        salts, env, evidence / "salts-configure.log")
    run(["cmake", "--build", "--preset", salts_preset, "--parallel", "4"],
        salts, env, evidence / "salts-build.log")
    run(["cmake", "--install", str(salts_build), "--prefix", str(prefix)],
        salts, env, evidence / "salts-install.log")
    run(["git", "diff", "--exit-code"], salts, env, evidence / "source-after.log")
    if not (prefix / "lib/cmake/Salts/SaltsConfig.cmake").is_file():
        raise RuntimeError("normal upstream install did not produce SaltsConfig.cmake")
    installed = {str(p.relative_to(prefix)): hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in sorted(prefix.rglob("*")) if p.is_file()}
    (evidence / "installed-sha256.json").write_text(
        json.dumps(installed, indent=2) + "\n", encoding="utf-8")
    triplet = "x64-windows" if system == "windows" else "x64-linux"
    native = salts / "vcpkg_installed" / triplet
    if args.config == "debug":
        native /= "debug"
    if system == "windows":
        env["PATH"] = os.pathsep.join([str(prefix / "bin"), str(native / "bin"),
                                       env["PATH"]])
    else:
        env["LD_LIBRARY_PATH"] = os.pathsep.join([str(prefix / "lib"),
                                                  str(native / "lib")])
    run(["cmake", "--preset", sdk_preset], sdk_root, env,
        evidence / "sdk-configure.log")
    run(["cmake", "--build", "--preset", sdk_preset, "--verbose"], sdk_root, env,
        evidence / "sdk-build.log")
    commands = json.loads((sdk_build / "compile_commands.json").read_text())
    for item in commands:
        command = item["command"]
        sanitized = "sanitize=address" in command
        if sanitized != (args.config == "debug"):
            raise RuntimeError(f"wrong sanitizer policy: {item['file']}")
    (evidence / "compile_commands.json").write_text(
        json.dumps(commands, indent=2) + "\n", encoding="utf-8")
    listing = json.loads(run(["ctest", "--preset", sdk_preset, "--show-only=json-v1"],
                             sdk_root, env, evidence / "test-list.log"))
    names = [test["name"] for test in listing["tests"]]
    if len(names) != len(EXPECTED_TESTS) or set(names) != EXPECTED_TESTS:
        raise RuntimeError(f"unexpected Task 1 CTest inventory: {names}")
    output = run(["ctest", "--preset", sdk_preset, "--verbose", "--no-tests=error"],
                 sdk_root, env, evidence / "ctest.log")
    if not re.search(rf"\b{EXPECTED_CASES} passed, 0 failed, 0 skipped, 0 filtered", output):
        raise RuntimeError("CTest did not report all expected TinyTest behavior cases")
    executable_suffix = ".exe" if system == "windows" else ""
    binaries = {}
    for name in ("orm_driver_prefix_test", "orm_driver_layout_test"):
        path = sdk_build / "bin" / (name + executable_suffix)
        binaries[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
        imports = (["dumpbin", "/dependents", str(path)] if system == "windows" else
                   ["readelf", "-d", str(path)])
        run(imports, root, env, evidence / f"{name}-imports.log")
    manifest.update(status="passed", tests=names, tinytest_cases=EXPECTED_CASES,
                    binary_sha256=binaries,
                    sanitizer="address" if system == "windows" else "address,undefined")
    if args.config == "release":
        manifest["sanitizer"] = "none"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2), flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"Package contract failed: {error}", file=sys.stderr, flush=True)
        sys.exit(1)
