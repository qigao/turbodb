"""Recreate reviewed SQL slices from the pinned, local upstream files.

This is a data preparation tool, not a mysqltest interpreter or test runner.
It never changes expectations or downloads/executes SQL.
"""
import hashlib
import json
from pathlib import Path


def rebuild():
    root = Path(__file__).resolve().parents[1] / "mysql-spec"
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8"))
    sources = {}
    for source in manifest["sources"]:
        path = (root / "upstream" / source["path"]).resolve()
        if not path.is_relative_to(root / "upstream"):
            raise ValueError("source path escapes upstream directory")
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != source["sha256"]:
            raise ValueError(f"upstream hash mismatch: {path}")
        sources[source["path"]] = data

    outputs = {}
    for case in manifest["cases"]:
        path = (root / case["file"]).resolve()
        if not path.is_relative_to(root / "cases") or path in outputs:
            raise ValueError(f"invalid or duplicate case path: {path}")
        lines = sources[case["source"]].splitlines(keepends=True)
        first, last = case["first_line"], case["last_line"]
        if not 1 <= first <= last <= len(lines):
            raise ValueError(f"invalid line range: {path}")
        data = b"".join(lines[first - 1:last])
        if len(data) != case["length"] or sum(map(len, lines[:first - 1])) != case["offset"]:
            raise ValueError(f"source range mismatch: {path}")
        outputs[path] = data

    # Validate every source/range before replacing any generated SQL.
    for path, data in outputs.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    print(f"Rebuilt {len(outputs)} SQL files from {manifest['version']} ({manifest['commit']})")


if __name__ == "__main__":
    rebuild()
