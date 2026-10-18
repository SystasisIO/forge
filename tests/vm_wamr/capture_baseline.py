"""Capture the dirty VM comparison baseline without changing its worktree."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile


ROOTS = [
    "AGENTS.md", "CMakeLists.txt", "PROVENANCE.md", "cmake/ForgeConfig.cmake.in",
    "cmake/ForgeSoftFloatAbi.cmake", "libraries/vm", "libraries/execution/wasm",
    "libraries/tooling/wasm_compiler", "libraries/tooling/testing",
    "libraries/tooling/validation", "guest/cmake", "guest/CMakeLists.txt",
    "guest/libraries/contract/intrinsics.hpp", "tools/wasm-compiler",
    "tests/vm_wasm", "tests/execution_wasm", "tests/package_execution_wasm_component",
    "tests/package_vm_wasm_interpret_component", "tests/package_vm_wasm_optimizing_component",
]


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args])


def snapshot(root):
    paths = git(root, "ls-files", "-z", "--cached", "--others", "--exclude-standard", "--", *ROOTS)
    files = {}
    for relative in sorted(set(paths.decode().split("\0")) - {""}):
        path = root / relative
        if path.is_file():
            files[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    for required in ["libraries/vm/wasm/interpret", "libraries/vm/wasm/optimizing",
                     "libraries/tooling/wasm_compiler", "libraries/execution/wasm"]:
        if not any(name.startswith(required + "/") for name in files):
            raise SystemExit(f"incomplete comparison baseline: {required}")
    return {
        "schema": 1,
        "head": git(root, "rev-parse", "HEAD").decode().strip(),
        "branch": git(root, "branch", "--show-current").decode().strip(),
        "scope": ROOTS,
        "files": files,
        "scope_sha256": hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    current = snapshot(args.source)
    if args.check:
        recorded = json.loads(args.manifest.read_text())
        if current != recorded:
            raise SystemExit("baseline changed; recapture and requalify intentionally")
        print(f"unchanged baseline: {current['scope_sha256']}")
        return
    if args.manifest.exists() or (args.archive and args.archive.exists()):
        raise SystemExit("refusing to overwrite an existing baseline")
    if args.archive:
        args.archive.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(args.archive, "x", compression=zipfile.ZIP_DEFLATED) as archive:
            for relative, digest in current["files"].items():
                data = (args.source / relative).read_bytes()
                if hashlib.sha256(data).hexdigest() != digest:
                    raise SystemExit("source changed while capturing baseline")
                info = zipfile.ZipInfo(relative, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, data)
    if snapshot(args.source) != current:
        raise SystemExit("source changed while capturing baseline")
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(current, indent=2, sort_keys=True) + "\n")
    print(f"captured {len(current['files'])} files: {current['scope_sha256']}")


if __name__ == "__main__":
    main()
