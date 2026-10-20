#!/usr/bin/env python3

import argparse
import json
import platform
import re
import shutil
import subprocess
import tarfile
import time
from pathlib import Path

from run_dual_target_fixtures import build_jobs, validate as validate_dual_target

LINUX_SDK_RUNTIME_PREFIXES = (
    "libstdc++.so",
    "libc++.so",
    "libc++abi.so",
    "libunwind.so",
    "libLLVM",
    "libclang-cpp",
    "liblld",
)

BOOST_INCLUDE = re.compile(rb'^\s*#\s*include\s*[<"](?P<path>boost/[^>"]+)[>"]', re.MULTILINE)
BUILD_CONFIGURATION = "Debug"


def is_linux_sdk_runtime(dependency: str) -> bool:
    return Path(dependency).name.startswith(LINUX_SDK_RUNTIME_PREFIXES)


def run(*command: str) -> None:
    subprocess.run(command, check=True)


def build_project(cmake: str, directory: Path) -> None:
    run(cmake, "--build", str(directory), "--config", BUILD_CONFIGURATION, "-j", build_jobs())


def contains_path(path: Path, needle: bytes) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    with path.open("rb") as stream:
        tail = b""
        while chunk := stream.read(1024 * 1024):
            data = tail + chunk
            if needle in data:
                return True
            tail = data[-len(needle) :] if needle else b""
    return False


def read_abi(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def command_output(*command: str) -> str:
    return subprocess.run(command, check=True, text=True, stdout=subprocess.PIPE).stdout


def sdk_tools(sdk: Path) -> list[Path]:
    return [
        sdk / "bin" / "clang++",
        sdk / "bin" / "clang-scan-deps",
        sdk / "bin" / "llvm-ar",
        sdk / "bin" / "llvm-ranlib",
        sdk / "bin" / "wasm-ld",
        sdk / "bin" / "abigen",
        sdk / "bin" / "contract-check",
        sdk / "bin" / "contract-manifest",
        sdk / "lib" / "forge-contract" / "attr-plugin.so",
    ]


def verify_runtime_dependencies(sdk: Path) -> None:
    dangling = [path for path in sdk.rglob("*") if path.is_symlink() and not path.exists()]
    if dangling:
        raise RuntimeError(f"SDK contains a dangling symlink: {dangling[0]}")

    tools = [path for path in sdk_tools(sdk) if path.exists()]
    if len(tools) != len(sdk_tools(sdk)):
        raise RuntimeError("SDK runtime tool set is incomplete")

    system = platform.system()
    if system == "Darwin":
        for tool in tools:
            for line in command_output("otool", "-L", str(tool)).splitlines()[1:]:
                dependency = line.strip().split(" ", 1)[0]
                if dependency.startswith("@rpath/"):
                    bundled = sdk / "lib" / Path(dependency).name
                    if not bundled.is_file():
                        raise RuntimeError(f"SDK does not bundle {dependency} required by {tool}")
                elif dependency.startswith("/") and not dependency.startswith(("/System/Library/", "/usr/lib/")):
                    raise RuntimeError(f"SDK tool retains an external runtime dependency: {tool}: {dependency}")
    elif system == "Linux":
        sdk_prefix = str(sdk.resolve()) + "/"
        for tool in tools:
            for line in command_output("ldd", str(tool)).splitlines():
                if "not found" in line:
                    raise RuntimeError(f"SDK tool has an unresolved runtime dependency: {tool}: {line.strip()}")
                if "=>" not in line:
                    continue
                dependency = line.split("=>", 1)[1].strip().split(" ", 1)[0]
                if dependency.startswith("/") and is_linux_sdk_runtime(dependency) and not dependency.startswith(sdk_prefix):
                    raise RuntimeError(f"SDK tool uses an external LLVM runtime: {tool}: {dependency}")
                if dependency.startswith("/") and not dependency.startswith(
                    (sdk_prefix, "/lib/", "/lib64/", "/usr/lib/")
                ):
                    raise RuntimeError(f"SDK tool retains an external runtime dependency: {tool}: {dependency}")


def verify_boost_header_closure(sdk: Path) -> None:
    include = sdk / "share" / "forge-contract" / "include"
    pfr = include / "boost" / "pfr"
    if not pfr.is_dir():
        raise RuntimeError("SDK does not contain its Boost.PFR dependency")

    for header in pfr.rglob("*.hpp"):
        for match in BOOST_INCLUDE.finditer(header.read_bytes()):
            dependency = include / match.group("path").decode("utf-8")
            if not dependency.is_file():
                raise RuntimeError(f"SDK Boost.PFR dependency is not self-contained: {header}: {dependency}")


def type_target(abi: dict, name: str) -> str:
    return next(entry["type"] for entry in abi["types"] if entry["new_type_name"] == name)


def action_contract(abi: dict, name: str) -> str:
    return next(entry["ricardian_contract"] for entry in abi["actions"] if entry["name"] == name)


def clause_body(abi: dict, identifier: str) -> str:
    return next(entry["body"] for entry in abi["ricardian_clauses"] if entry["id"] == identifier)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--cxx-compiler", required=True, type=Path)
    parser.add_argument("--forge-package", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--dual-target-source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    build_jobs()

    output = args.output.resolve()
    forge_package = args.forge_package.resolve()
    shutil.rmtree(output, ignore_errors=True)
    unpacked = output / "sdk"
    unpacked.mkdir(parents=True)
    with tarfile.open(args.archive, "r:gz") as archive:
        archive.extractall(unpacked, filter="data")

    roots = [entry for entry in unpacked.iterdir() if entry.is_dir()]
    if len(roots) != 1:
        raise RuntimeError(f"expected one SDK root, found {len(roots)}")
    sdk = roots[0]
    verify_runtime_dependencies(sdk)
    verify_boost_header_closure(sdk)

    config = sdk / "lib" / "cmake" / "ForgeContract" / "ForgeContractConfig.cmake"
    release = 'set(ForgeContract_PROFILE "release")' in config.read_text()
    candidates = list(sdk.rglob("*"))
    if not release:
        candidates = [path for path in candidates if path.suffix in {".cmake", ".json", ".pc", ".txt"}]
    leaked = [path for path in candidates if contains_path(path, str(args.source_root.resolve()).encode())]
    if leaked:
        raise RuntimeError(f"installed SDK contains source path: {leaked[0]}")

    source = output / "consumer"
    shutil.copytree(sdk / "share" / "forge-contract" / "examples" / "hello", source)
    build = output / "build"
    package = sdk / "lib" / "cmake" / "ForgeContract"
    run(
        args.cmake,
        "-S",
        str(source),
        "-B",
        str(build),
        "-G",
        "Ninja Multi-Config",
        f"-DCMAKE_TOOLCHAIN_FILE={package / 'ForgeContractToolchain.cmake'}",
        f"-DForgeContract_DIR={package}",
    )
    build_project(args.cmake, build)

    artifacts = build / "artifacts" / BUILD_CONFIGURATION
    for suffix in ("wasm", "abi", "contract.json"):
        artifact = artifacts / f"hello.{suffix}"
        if not artifact.is_file() or artifact.stat().st_size == 0:
            raise RuntimeError(f"missing relocated SDK artifact: {artifact}")

    manifest = json.loads((artifacts / "hello.contract.json").read_text(encoding="utf-8"))
    if manifest["schema_version"] != 3:
        raise RuntimeError("contract manifest schema is not version 3")
    if "source_graph" in manifest:
        raise RuntimeError("runtime contract manifest contains source attestation")
    if manifest["sdk"]["profile"] == "release":
        expected_llvm = {
            "version": "llvmorg-22.1.8",
            "commit": "ca7933e47d3a3451d81e72ac174dcb5aa28b59d1",
        }
    else:
        expected_llvm = {"version": command_output(str(sdk / "bin" / "clang++"), "--version").splitlines()[0]}
    if manifest["llvm"] != expected_llvm:
        raise RuntimeError(f"contract manifest has the wrong toolchain identity: {manifest['llvm']!r}")
    if manifest["sysroot"]["schema_version"] != 1 or isinstance(manifest["sysroot"]["schema_version"], bool):
        raise RuntimeError("contract manifest sysroot schema version is not the numeric version 1")
    if manifest["intrinsics"]["interface_version"] != 1 or isinstance(
        manifest["intrinsics"]["interface_version"], bool
    ):
        raise RuntimeError("contract manifest intrinsic interface version is not the numeric version 1")

    abi_path = artifacts / "hello.abi"
    initial_abi = read_abi(abi_path)
    if type_target(initial_abi, "counter") != "uint32":
        raise RuntimeError("initial header ABI type was not generated")
    if action_contract(initial_abi, "count") != "Record a positive counter value.":
        raise RuntimeError("relative Ricardian contracts file was not loaded")
    if clause_body(initial_abi, "positive-counter") != "The counter value must be greater than zero.":
        raise RuntimeError("relative Ricardian clauses file was not loaded")

    types = source / "types.hpp"
    types.write_text(types.read_text(encoding="utf-8").replace("std::uint32_t", "std::uint64_t"), encoding="utf-8")
    build_project(args.cmake, build)
    if type_target(read_abi(abi_path), "counter") != "uint64":
        raise RuntimeError("included header change did not regenerate the contract ABI")

    contracts = source / "hello.contracts.md"
    contracts.write_text(
        contracts.read_text(encoding="utf-8").replace(
            "Record a positive counter value.", "Record an updated positive counter value."
        ),
        encoding="utf-8",
    )
    build_project(args.cmake, build)
    if action_contract(read_abi(abi_path), "count") != "Record an updated positive counter value.":
        raise RuntimeError("Ricardian contracts change did not regenerate the contract ABI")

    clauses = source / "hello.clauses.md"
    clauses.write_text(
        clauses.read_text(encoding="utf-8").replace(
            "The counter value must be greater than zero.", "The updated counter value must be greater than zero."
        ),
        encoding="utf-8",
    )
    build_project(args.cmake, build)
    if clause_body(read_abi(abi_path), "positive-counter") != "The updated counter value must be greater than zero.":
        raise RuntimeError("Ricardian clauses change did not regenerate the contract ABI")

    wasm = artifacts / "hello.wasm"
    first_mtime = wasm.stat().st_mtime_ns
    time.sleep(0.01)
    with (source / "hello.cpp").open("a", encoding="utf-8") as stream:
        stream.write("\n")
    build_project(args.cmake, build)
    if wasm.stat().st_mtime_ns <= first_mtime:
        raise RuntimeError("contract source change did not rebuild the WebAssembly artifact")

    aligned_source = output / "aligned-consumer"
    shutil.copytree(
        args.source_root / "guest" / "tests" / "relocation" / "aligned_multi_index",
        aligned_source,
    )
    aligned_build = output / "aligned-build"
    run(
        args.cmake,
        "-S",
        str(aligned_source),
        "-B",
        str(aligned_build),
        "-G",
        "Ninja Multi-Config",
        f"-DCMAKE_TOOLCHAIN_FILE={package / 'ForgeContractToolchain.cmake'}",
        f"-DForgeContract_DIR={package}",
    )
    build_project(args.cmake, aligned_build)
    for suffix in ("wasm", "abi", "contract.json"):
        artifact = (
            aligned_build / "artifacts" / BUILD_CONFIGURATION / f"alignedidx.{suffix}"
        )
        if not artifact.is_file() or artifact.stat().st_size == 0:
            raise RuntimeError(f"missing aligned multi-index artifact: {artifact}")

    aligned_host_build = output / "aligned-host-build"
    aligned_host_command = [
        args.cmake,
        "-S",
        str(aligned_source / "host"),
        "-B",
        str(aligned_host_build),
        "-G",
        "Ninja",
        "-DCMAKE_BUILD_TYPE=Debug",
        f"-DCMAKE_CXX_COMPILER={args.cxx_compiler}",
        f"-DForge_DIR={forge_package}",
        (
            "-DALIGNED_MULTI_INDEX_WASM="
            f"{aligned_build / 'artifacts' / BUILD_CONFIGURATION / 'alignedidx.wasm'}"
        ),
    ]
    if platform.system() == "Darwin":
        aligned_host_command.append(
            f"-DCMAKE_OSX_SYSROOT={command_output('xcrun', '--sdk', 'macosx', '--show-sdk-path').strip()}"
        )
    run(*aligned_host_command)
    run(
        args.cmake,
        "--build",
        str(aligned_host_build),
        "--target",
        "aligned_multi_index_vm_tests",
        "-j",
        build_jobs(),
    )
    run(str(aligned_host_build / "aligned_multi_index_vm_tests"))

    validate_dual_target(
        cmake=args.cmake,
        cxx_compiler=args.cxx_compiler,
        forge_package=forge_package,
        contract_package=package,
        forge_source_root=args.source_root.resolve(),
        source=args.dual_target_source.resolve(),
        output=output / "dual-target",
    )


if __name__ == "__main__":
    main()
