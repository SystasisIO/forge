#!/usr/bin/env python3

import argparse
import hashlib
import pathlib
import shutil
import subprocess
import sys

from run_dual_target_fixtures import build_jobs


def run(command):
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"command failed:\n{' '.join(map(str, command))}\n{result.stdout}")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(args, root):
    source = root / "source"
    binary = root / "build"
    shutil.copytree(args.source, source)
    run(
        [
            str(args.cmake),
            "-S",
            str(source),
            "-B",
            str(binary),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DForgeContract_DIR={args.package}",
            f"-DCMAKE_TOOLCHAIN_FILE={args.package / 'ForgeContractToolchain.cmake'}",
        ]
    )
    run([str(args.cmake), "--build", str(binary), "--target", "hello_artifacts", "-j", build_jobs()])
    cache = (binary / "CMakeCache.txt").read_text(encoding="utf-8")
    if "CMAKE_BUILD_TYPE:STRING=Release" not in cache:
        raise RuntimeError("forge_add_contract did not propagate the parent Release build type")
    return binary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True, type=pathlib.Path)
    parser.add_argument("--package", required=True, type=pathlib.Path)
    parser.add_argument("--source", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    build_jobs()

    if args.output.exists():
        shutil.rmtree(args.output)
    first = build(args, args.output / "absolute-path-a")
    second = build(args, args.output / "different" / "absolute-path-b")
    for artifact in ("hello.wasm", "hello.abi", "hello.contract.json"):
        left = first / "artifacts" / artifact
        right = second / "artifacts" / artifact
        if left.read_bytes() != right.read_bytes():
            raise RuntimeError(
                f"contract artifact depends on its absolute build path: {artifact} "
                f"({digest(left)} != {digest(right)})"
            )


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"run_reproducibility: {error}", file=sys.stderr)
        raise
