#!/usr/bin/env python3
"""Admission checks for the exact VM CI compiler and libc++ header contract."""

import argparse
from email.parser import Parser
import hashlib
import json
import os
from pathlib import Path
import platform
import posixpath
import re
import shlex
import shutil
import subprocess
import sys
import tarfile


VERSION = "22.1.8"
SOURCE_SHA256 = "922f1817a0df7b1489272d18134ee0087a8b068828f87ac63b9861b1a9965888"
MIN_BOOTSTRAP_FREE_BYTES = 40 * 1024 ** 3
PACKAGE_MANIFEST = "share/forge-vm-ci-toolchain.json"
INTEL_BOTTLE_SHA256 = "2f07536754d0854565f9ac37436681bb3d04a4fbb15c45c51896933262df5e48"
INTEL_FORMULA_REF = "75d8fa416b18bc8caedacf1696831422902a2bb3"
INTEL_FORMULA_SHA256 = "2fa9df2ef5b3ad8cbf3c9bc3fe0b4f3d7133a7b9cf0e00647560d4fd44e665e8"
INTEL_Z3_FORMULA_SHA256 = "6dc219c3882bb64259408b786da411d7d61e25dc089a2e89768eba8f6b82c26c"
INTEL_Z3_BOTTLE_SHA256 = "a520309ac4d170897f5bcd9b238231bdec1cbb4f0f326c62abe7eb06b7616975"
INTEL_Z3_PATCH_SHA256 = "2e3922d6f8f6ed7ad678fa0c6df9ccc3fde6cbbe2aa9925fbd3c67b1b1a24d43"
INTEL_DERIVED_FORMULA_SHA256 = "3a3a71e07c7f242faeab9489194ced8eb5baf75440c5adc6db9b8b2bca2ef484"
STAGE0_VERSION = "1:22.1.8~++20260714014902+ca7933e47d3a-1~exp1~20260714135019.80"
STAGE0_CC = "/usr/bin/clang-22"
STAGE0_CXX = "/usr/bin/clang++-22"
STAGE0_LD = "/usr/bin/ld.lld-22"
STAGE0_PACKAGES = {
    "clang-22": "e969e7f97a6e71326bae2cec6d76059cb9bf6562bfe89d03bcb65c3cee1aac05",
    "lld-22": "4172d3b289825f729a84ae351b2f37a2c6c6715f2d1511edd60b59a79c3da200",
    "libclang-common-22-dev": "90cee393eefbe1cb5bf463b7d4aea4ab5512d06525641e6836d21c4d7191944e",
    "libclang-cpp22": "620b7960fef6ca4647b813889d4f3ae3f8bd5de71c949550c98a6030427acb18",
    "libclang1-22": "25ca957fa2054cc607218df0a5c01994416062abc76fe008327fb95420afc509",
    "libllvm22": "e94d23161316c53ebe8c9e35038468e06a5e47dbdf207a3c54a22ab40e37cbb6",
    "llvm-22-linker-tools": "ee6df5de094e62ba016f6d8ade319d34c246a0a5c79ed898aeaaa327332cd5b7",
}
SOURCE = "#include <vector>\n#include <functional>\n"
APT_PACKAGES = ("clang-22", "clang-tools-22", "libc++-22-dev", "libc++abi-22-dev",
                "libc++1-22", "libc++abi1-22", "libunwind-22-dev", "libunwind-22")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def run(argv, input_text=None):
    result = subprocess.run(argv, input=input_text, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=60, check=False)
    require(result.returncode == 0, f"{shlex.join(argv)} failed: {result.stderr.strip()}")
    return result.stdout, result.stderr


def bottle(file, tag):
    formula = json.loads(file.read_text())["formulae"][0]
    require(formula["versions"]["stable"] == VERSION, "Homebrew llvm@22 is not exactly 22.1.8")
    record = formula.get("bottle", {}).get("stable", {}).get("files", {}).get(tag)
    require(record and record.get("url") and re.fullmatch(r"[0-9a-f]{64}", record.get("sha256", "")),
            f"no qualified llvm@22 22.1.8 native bottle for {tag}; source build forbidden")
    print(f"READY: llvm@22 {VERSION} bottle {tag} sha256={record['sha256']}")


def intel_scope(scope):
    require(scope == "intel-runtime-off", "historical Intel candidate supports only intel-runtime-off")
    require(platform.system() == "Darwin" and platform.machine() == "x86_64"
            and platform.mac_ver()[0].startswith("15."), "historical Intel candidate requires native x86_64 macOS15")
    for variable in ("WAMR_JIT", "WAMR_AOT", "FORGE_ENABLE_VM_WASM_LLVM_JIT", "FORGE_ENABLE_VM_WASM_OPTIMIZING",
                     "FORGE_ENABLE_WASM_COMPILER", "FORGE_ENABLE_TOOLING"):
        require(os.environ.get(variable, "OFF") == "OFF", f"unsupported Intel runtime-only scope: {variable}")
    for variable in ("LLVM_PREFIX", "LLVM_DIR", "Clang_DIR"):
        require(not os.environ.get(variable), f"runtime-only candidate must not select {variable}")
    for variable in ("CMAKE_DISABLE_FIND_PACKAGE_LLVM", "CMAKE_DISABLE_FIND_PACKAGE_Clang"):
        require(os.environ.get(variable, "ON") == "ON", f"runtime-only candidate must disable {variable}")


def intel_archive(archive, formula, evidence):
    require(sha256(archive) == INTEL_BOTTLE_SHA256, "historical Intel bottle SHA mismatch")
    require(sha256(formula) == INTEL_FORMULA_SHA256, "historical Intel formula SHA/ref mismatch")
    # Homebrew owns bottle metadata and generates INSTALL_RECEIPT.json during pour.
    evidence.write_text(json.dumps({"state": "SHA_VERIFIED", "scope": "intel-runtime-off",
                                    "bottle_sha256": INTEL_BOTTLE_SHA256,
                                    "formula_ref": INTEL_FORMULA_REF, "formula_sha256": INTEL_FORMULA_SHA256,
                                    "metadata_owner": "Homebrew", "native_qualification": "PENDING",
                                    "LLVM_library_qualification": "NOT_APPLICABLE_RUNTIME_OFF"}, indent=2) + "\n")
    print("READY: Intel bottle/formula SHA verified; metadata Homebrew-owned; native qualification pending")


def intel_formula(file):
    formulae = json.loads(file.read_text())["formulae"]
    require(len(formulae) == 1, "ambiguous historical Intel tap formula metadata")
    formula = formulae[0]
    require(formula["name"] == "llvm" and formula.get("full_name") == "forge/vm-ci/llvm"
            and formula["versions"]["stable"] == VERSION,
            "Homebrew did not load the pinned forge/vm-ci/llvm22.1.8 formula")
    record = formula.get("bottle", {}).get("stable", {}).get("files", {}).get("sonoma", {})
    require(record.get("sha256") == INTEL_BOTTLE_SHA256
            and record.get("url") == f"https://ghcr.io/v2/homebrew/core/llvm/blobs/sha256:{INTEL_BOTTLE_SHA256}",
            "historical Intel tap formula must select the pinned Homebrew core Sonoma bottle")
    dependencies = formula.get("dependencies", [])
    require("forge/vm-ci/z3" in dependencies and "z3" not in dependencies and "homebrew/core/z3" not in dependencies,
            "historical Intel LLVM must depend on qualified forge/vm-ci/z3, not current core Z3")


def intel_z3_inputs(formula, dependency_patch):
    require(sha256(formula) == INTEL_Z3_FORMULA_SHA256, "historical Intel Z3 formula SHA mismatch")
    require(sha256(dependency_patch) == INTEL_Z3_PATCH_SHA256, "historical Intel dependency patch SHA mismatch")
    print("READY: pinned upstream Z3 formula and reviewed LLVM dependency patch")


def intel_derived_formula(original, derived, dependency_patch, evidence):
    require(sha256(original) == INTEL_FORMULA_SHA256, "historical Intel original LLVM formula changed")
    require(sha256(dependency_patch) == INTEL_Z3_PATCH_SHA256, "historical Intel dependency patch SHA mismatch")
    require(sha256(derived) == INTEL_DERIVED_FORMULA_SHA256, "historical Intel derived LLVM formula SHA mismatch")
    evidence.write_text(json.dumps({"scope": "intel-runtime-off", "upstream_ref": INTEL_FORMULA_REF,
                                    "original_llvm_formula_sha256": INTEL_FORMULA_SHA256,
                                    "derived_llvm_formula_sha256": INTEL_DERIVED_FORMULA_SHA256,
                                    "dependency_patch_sha256": INTEL_Z3_PATCH_SHA256,
                                    "upstream_z3_formula_sha256": INTEL_Z3_FORMULA_SHA256,
                                    "native_qualification": "PENDING"}, indent=2) + "\n")


def intel_z3_formula(file):
    formulae = json.loads(file.read_text())["formulae"]
    require(len(formulae) == 1, "ambiguous historical Intel Z3 formula metadata")
    formula = formulae[0]
    require(formula["name"] == "z3" and formula.get("full_name") == "forge/vm-ci/z3"
            and formula["versions"]["stable"] == "4.16.0", "Homebrew did not load pinned forge/vm-ci/z3 4.16.0")
    record = formula.get("bottle", {}).get("stable", {}).get("files", {}).get("sonoma", {})
    require(record.get("sha256") == INTEL_Z3_BOTTLE_SHA256
            and record.get("url") == f"https://ghcr.io/v2/homebrew/core/z3/blobs/sha256:{INTEL_Z3_BOTTLE_SHA256}",
            "historical Intel Z3 must select the pinned Homebrew core Sonoma bottle")
    require(formula.get("dependencies") == [], "historical Intel Z3 must have no runtime dependencies")


def intel_z3_library(evidence):
    opt = Path("/usr/local/opt/z3")
    keg = Path("/usr/local/Cellar/z3/4.16.0")
    require(opt.is_symlink() and opt.resolve(strict=True) == keg, "Intel Z3 opt must resolve to the owned 4.16.0 keg")
    library = opt / "lib/libz3.4.16.dylib"
    payload = keg / "lib/libz3.4.16.0.0.dylib"
    require(library.is_file() and library.resolve(strict=True) == payload, "Intel Z3 4.16 dylib is missing/outside owned keg")
    evidence.write_text(json.dumps({"scope": "intel-runtime-off", "opt": str(opt), "keg": str(keg),
                                    "library": str(library), "payload": str(payload),
                                    "payload_sha256": sha256(payload), "native_qualification": "PENDING"}, indent=2) + "\n")


def intel_headers(prefix, evidence):
    intel_scope("intel-runtime-off")
    require(prefix == Path("/usr/local/Cellar/llvm/22.1.8"), "Intel compiler must use the exact native Cellar keg")
    for variable, name in (("CC", "clang"), ("CXX", "clang++")):
        require(os.environ.get(variable) == str(prefix / "bin" / name), "Intel compiler alias/outside-keg path refused")
    for name in ("clang", "clang++", "clang-scan-deps"):
        tool = prefix / "bin" / name
        require(tool.is_file() and tool.resolve().is_relative_to(prefix), "Intel tool resolves outside exact Cellar keg")
    headers(evidence)
    result = json.loads((evidence / "header-result.json").read_text())
    require(all(Path(file).resolve().is_relative_to(prefix) for file in result["headers"].values()),
            "Intel libc++ headers resolve outside exact Cellar keg")
    macros = (evidence / "macros.txt").read_text()
    for name in ("__EXCEPTIONS", "__GXX_RTTI"):
        require(re.search(rf"^#define {name} 1$", macros, re.M), f"Intel host feature disabled: {name}")
    print("READY: Intel runtime-OFF compiler/headers/host RTTI+EH; not LLVM/JIT/compiler qualification")


def apt_packages(evidence):
    selected = []
    for package in APT_PACKAGES:
        listing, _ = run(["apt-cache", "madison", package])
        versions = [line.split("|")[1].strip() for line in listing.splitlines() if line.count("|") >= 2]
        exact = next((value for value in versions if re.fullmatch(r"(?:[0-9]+:)?22\.1\.8(?:-[^\s~]+)?", value)), None)
        require(exact, f"no exact22.1.8 apt candidate for {package}; no newer-patch fallback")
        selected.append(f"{package}={exact}")
    evidence.write_text(json.dumps({"version": VERSION, "packages": selected}, indent=2) + "\n")
    print("\n".join(selected))


def stage0_packages(evidence):
    selected = []
    for package, expected in STAGE0_PACKAGES.items():
        text, _ = run(["apt-cache", "show", f"{package}={STAGE0_VERSION}"])
        records = [Parser().parsestr(block) for block in text.strip().split("\n\n")]
        require(len(records) == 1, f"ambiguous stage0 metadata: {package}")
        record = records[0]
        require(record["Package"] == package and record["Version"] == STAGE0_VERSION
                and record["Architecture"] == "amd64" and record["SHA256"] == expected,
                f"stage0 package identity/hash mismatch: {package}")
        selected.append({"package": package, "version": STAGE0_VERSION, "sha256": expected,
                         "filename": record["Filename"]})
    evidence.write_text(json.dumps({"origin": "https://apt.llvm.org/noble/", "packages": selected}, indent=2) + "\n")
    print("\n".join(f"{record['package']}={STAGE0_VERSION}" for record in selected))


def require_supported_stage0(version):
    require(version >= (20, 1, 0), "libc++22 stage0 requires Clang >=20.1; old compiler unsupported")
    require(version == (22, 1, 8), "stage0 must match the pinned noble Clang22.1.8 package")


def stage0(evidence):
    evidence.mkdir(parents=True, exist_ok=True)
    record = {}
    for compiler in (STAGE0_CC, STAGE0_CXX):
        output, error = run([compiler, "--version"])
        match = re.search(r"\bclang version (\d+)\.(\d+)\.(\d+)\b", output)
        require(match, f"unrecognized stage0 compiler: {compiler}")
        require_supported_stage0(tuple(map(int, match.groups())))
        macros, _ = run([compiler, "-dM", "-E", "-x", "c++", "-"], input_text="\n")
        numbers = []
        for name in ("__clang_major__", "__clang_minor__", "__clang_patchlevel__"):
            value = re.search(rf"^#define {name} (\d+)$", macros, re.M)
            require(value, "missing stage0 frontend version macro")
            numbers.append(int(value.group(1)))
        require_supported_stage0(tuple(numbers))
        record[compiler] = {"version": output + error, "frontend_version": numbers,
                            "sha256": sha256(Path(compiler).resolve())}
    output, error = run([STAGE0_LD, "--version"])
    require(re.search(r"\bLLD 22\.1\.8\b", output), "stage0 linker must be pinned LLD22.1.8")
    record[STAGE0_LD] = {"version": output + error, "sha256": sha256(Path(STAGE0_LD).resolve())}
    for package in STAGE0_PACKAGES:
        installed, _ = run(["dpkg-query", "--show", "--showformat=${Version}", package])
        require(installed.strip() == STAGE0_VERSION, f"wrong installed stage0 package: {package}")
    (evidence / "stage0.json").write_text(json.dumps({"state": "READY", "minimum_clang": "20.1.0",
                                                   "package_version": STAGE0_VERSION, "tools": record}, indent=2) + "\n")
    print("READY: supported pinned Clang22.1.8/LLD22.1.8 stage0; GNU host process ABI is not output ABI")


def headers(evidence):
    evidence.mkdir(parents=True, exist_ok=True)
    reported = {}
    for variable in ("CC", "CXX"):
        compiler = shlex.split(os.environ[variable])
        output, error = run([*compiler, "--version"])
        (evidence / f"{variable}-version.txt").write_text(output + error)
        require(re.search(r"\bclang version 22\.1\.8(?:\s|$)", output),
                f"{variable} is not Clang22.1.8")
        reported[variable] = compiler
    scanner = Path(reported["CXX"][0]).parent / "clang-scan-deps"
    output, error = run([str(scanner), "--version"])
    (evidence / "clang-scan-deps-version.txt").write_text(output + error)
    require(re.search(r"(?:clang|LLVM) version 22\.1\.8(?:\s|$)", output),
            "clang-scan-deps is not exactly22.1.8 beside the selected compiler")
    flags = shlex.split(os.environ.get("CXXFLAGS", ""))
    require("-stdlib=libc++" in flags, "VM CI must explicitly select libc++")
    arguments = [*reported["CXX"], *flags, "-std=c++23", "-x", "c++", "-"]
    macros, error = run([*arguments, "-E", "-dM"], input_text=SOURCE)
    (evidence / "macros.txt").write_text(macros + error)
    require(re.search(r"^#define _LIBCPP_VERSION 220108$", macros, re.M),
            "live <vector>/<functional> _LIBCPP_VERSION must be 220108")
    require(not re.search(r"^#define (?:__GLIBCXX__|_GLIBCXX_RELEASE)\b", macros, re.M),
            "GNU C++ headers selected in VM lane")
    _, includes = run([*arguments, "-E", "-H", "-o", os.devnull], input_text=SOURCE)
    (evidence / "headers.txt").write_text(includes)
    paths = [line.split(" ", 1)[1] for line in includes.splitlines() if re.match(r"^\.+ ", line)]
    require(paths, "no live header path evidence")
    header_paths = {}
    for header in ("vector", "functional"):
        matches = [path for path in paths if Path(path).name == header]
        require(matches, f"no live <{header}> header path evidence")
        header_paths[header] = matches[0]
    (evidence / "header-result.json").write_text(json.dumps(
        {"state": "READY", "clang": VERSION, "libcxx": 220108, "compilers": reported, "scanner": str(scanner),
         "CXXFLAGS": flags, "headers": header_paths}, indent=2) + "\n")
    print("READY: Clang22.1.8 and live libc++220108 <vector>/<functional>; no Forge build")


def compiler_rt_install_path(prefix):
    prefix = prefix.resolve(strict=True)
    require(prefix.is_dir(), "Stage C installed prefix is not a directory")
    compiler = prefix / "bin/clang"
    require(compiler.is_file() and compiler.resolve(strict=True).is_relative_to(prefix),
            "Stage C Clang missing/outside installed prefix")
    output, _ = run([str(compiler), "--print-resource-dir"])
    resource = Path(output.strip())
    require(resource.is_absolute(), "Stage C resource directory must be absolute")
    resolved = resource.resolve(strict=True)
    require(resource == resolved and resolved.is_dir() and resolved != prefix and resolved.is_relative_to(prefix),
            "Stage C resource directory must be canonical and inside installed prefix")
    print(resolved.relative_to(prefix))


def sha256(file):
    digest = hashlib.sha256()
    with file.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def disk_preflight(directory, evidence):
    directory = directory.resolve(strict=True)
    free = shutil.disk_usage(directory).free
    ready = free >= MIN_BOOTSTRAP_FREE_BYTES
    evidence.write_text(json.dumps({"state": "READY" if ready else "REFUSED", "directory": str(directory),
                                    "free_bytes": free, "minimum_free_bytes": MIN_BOOTSTRAP_FREE_BYTES}, indent=2) + "\n")
    require(ready, f"LLVM bootstrap needs at least 40 GiB initially free; available={free} bytes; no cleanup/fallback")
    print(f"READY: bootstrap disk admission free={free} minimum={MIN_BOOTSTRAP_FREE_BYTES}; not a peak-size proof")


def recipe(evidence):
    require(platform.system() == "Linux" and platform.machine() == "x86_64", "native Linux x86_64 required")
    root = Path(__file__).resolve().parents[2]
    files = [root / ".github/scripts" / name for name in
             ("setup-vm-wasm-toolchain.sh", "check-vm-wasm-toolchain.py", "check-llvm-cxx-abi.py")]
    files += [root / ".github/workflows/vm-wasm.yml"]
    files += sorted((root / ".github/probes/llvm-cxx-abi").glob("*"))
    tools = {}
    for tool in (STAGE0_CC, STAGE0_CXX, STAGE0_LD, "/usr/bin/cmake", "/usr/bin/ninja"):
        output, error = run([tool, "--version"])
        tools[tool] = {"sha256": sha256(Path(tool).resolve()), "version": output + error}
    packages, _ = run(["dpkg-query", "--show"])
    record = {"schema": 1, "llvm": VERSION, "source_sha256": SOURCE_SHA256, "stage0_packages": packages,
              "stage0_llvm_version": STAGE0_VERSION, "stage0_package_sha256": STAGE0_PACKAGES,
              "platform": platform.platform(), "os_release": Path("/etc/os-release").read_text(),
              "runner_image": os.environ.get("ImageVersion"), "tools": tools,
              "recipe_files": {str(file.relative_to(root)): sha256(file) for file in files if file.is_file()}}
    key = hashlib.sha256(json.dumps(record, sort_keys=True).encode()).hexdigest()
    record["key"] = key
    evidence.write_text(json.dumps(record, indent=2) + "\n")
    print(key)


def tree(prefix):
    records = {}
    for file in sorted(prefix.rglob("*")):
        name = str(file.relative_to(prefix))
        if name == PACKAGE_MANIFEST or file.is_dir() and not file.is_symlink():
            continue
        if file.is_symlink():
            require(file.resolve().is_relative_to(prefix.resolve()), f"nonrelocatable package symlink: {name}")
            require(not Path(os.readlink(file)).is_absolute(), f"absolute package symlink: {name}")
            records[name] = {"symlink": os.readlink(file)}
        else:
            require(file.is_file(), f"unsupported package entry: {name}")
            records[name] = {"sha256": sha256(file), "mode": file.stat().st_mode & 0o777}
    return records


def pack(prefix, recipe_file, archive):
    record = json.loads(recipe_file.read_text())
    require(record["source_sha256"] == SOURCE_SHA256, "wrong source recipe")
    manifest = prefix / PACKAGE_MANIFEST
    manifest.parent.mkdir(parents=True, exist_ok=True)
    manifest.write_text(json.dumps({"recipe": record, "files": tree(prefix)}, indent=2) + "\n")
    with tarfile.open(archive, "w:gz", compresslevel=1) as output:
        for file in [prefix, *sorted(prefix.rglob("*"))]:
            # Keep relative symlinks, but store regular hardlinks independently.
            output.inodes.clear()
            output.add(file, arcname=str(Path("toolchain") / file.relative_to(prefix)), recursive=False)
    print(sha256(archive))


def unpack(archive, prefix, digest, key):
    require(re.fullmatch(r"[0-9a-f]{64}", digest) and sha256(archive) == digest, "toolchain archive SHA mismatch")
    require(re.fullmatch(r"[0-9a-f]{64}", key), "invalid recipe key")
    require(not prefix.exists(), "toolchain destination already exists")
    with tarfile.open(archive, "r:gz") as source:
        members = source.getmembers()
        names = set()
        for member in members:
            parts = Path(member.name).parts
            require(parts and parts[0] == "toolchain" and ".." not in parts and not member.name.startswith("/"),
                    f"unsafe archive path: {member.name}")
            require(member.name not in names, "duplicate archive path")
            names.add(member.name)
            require(member.isdir() or member.isfile() or member.issym(), "unsupported archive entry")
            if member.issym():
                target = posixpath.normpath(posixpath.join(posixpath.dirname(member.name), member.linkname))
                require(not member.linkname.startswith("/") and target.startswith("toolchain/"),
                        "unsafe archive symlink")
        # No archive member may write through another member's symlink.
        links = {member.name for member in members if member.issym()}
        for member in members:
            require(not any(str(parent) in links for parent in Path(member.name).parents), "archive traverses symlink")
        prefix.parent.mkdir(parents=True, exist_ok=True)
        staging = prefix.parent / "toolchain"
        require(not staging.exists(), "toolchain extraction staging already exists")
        source.extractall(prefix.parent)
        staging.rename(prefix)
    record = json.loads((prefix / PACKAGE_MANIFEST).read_text())
    require(record["recipe"]["key"] == key and record["recipe"]["source_sha256"] == SOURCE_SHA256,
            "toolchain recipe identity mismatch")
    require(record["files"] == tree(prefix), "toolchain extracted file/mode/symlink mismatch")
    print(f"READY: SHA-pinned toolchain extracted; recipe={key}; ABI preflight still required")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_subparsers(dest="mode", required=True)
    check = mode.add_parser("bottle")
    check.add_argument("file", type=Path)
    check.add_argument("tag")
    intel = mode.add_parser("intel-scope")
    intel.add_argument("scope")
    intel_payload = mode.add_parser("intel-archive")
    intel_payload.add_argument("archive", type=Path)
    intel_payload.add_argument("formula", type=Path)
    intel_payload.add_argument("evidence", type=Path)
    intel_info = mode.add_parser("intel-formula")
    intel_info.add_argument("file", type=Path)
    intel_live = mode.add_parser("intel-headers")
    intel_live.add_argument("prefix", type=Path)
    intel_live.add_argument("evidence", type=Path)
    z3_inputs = mode.add_parser("intel-z3-inputs")
    z3_inputs.add_argument("formula", type=Path)
    z3_inputs.add_argument("dependency_patch", type=Path)
    derived = mode.add_parser("intel-derived-formula")
    derived.add_argument("original", type=Path)
    derived.add_argument("derived", type=Path)
    derived.add_argument("dependency_patch", type=Path)
    derived.add_argument("evidence", type=Path)
    z3_formula = mode.add_parser("intel-z3-formula")
    z3_formula.add_argument("file", type=Path)
    z3_library = mode.add_parser("intel-z3-library")
    z3_library.add_argument("evidence", type=Path)
    apt = mode.add_parser("apt-packages")
    apt.add_argument("evidence", type=Path)
    stage0_install = mode.add_parser("stage0-packages")
    stage0_install.add_argument("evidence", type=Path)
    stage0_guard = mode.add_parser("stage0")
    stage0_guard.add_argument("evidence", type=Path)
    header = mode.add_parser("headers")
    header.add_argument("evidence", type=Path)
    resource = mode.add_parser("compiler-rt-install-path")
    resource.add_argument("prefix", type=Path)
    identity = mode.add_parser("recipe")
    identity.add_argument("evidence", type=Path)
    disk = mode.add_parser("disk")
    disk.add_argument("directory", type=Path)
    disk.add_argument("evidence", type=Path)
    package = mode.add_parser("pack")
    package.add_argument("prefix", type=Path)
    package.add_argument("recipe", type=Path)
    package.add_argument("archive", type=Path)
    extract = mode.add_parser("unpack")
    extract.add_argument("archive", type=Path)
    extract.add_argument("prefix", type=Path)
    extract.add_argument("sha256")
    extract.add_argument("key")
    args = parser.parse_args()
    if args.mode == "bottle":
        bottle(args.file, args.tag)
    elif args.mode == "intel-scope":
        intel_scope(args.scope)
    elif args.mode == "intel-archive":
        intel_archive(args.archive, args.formula, args.evidence)
    elif args.mode == "intel-formula":
        intel_formula(args.file)
    elif args.mode == "intel-headers":
        intel_headers(args.prefix, args.evidence)
    elif args.mode == "intel-z3-inputs":
        intel_z3_inputs(args.formula, args.dependency_patch)
    elif args.mode == "intel-derived-formula":
        intel_derived_formula(args.original, args.derived, args.dependency_patch, args.evidence)
    elif args.mode == "intel-z3-formula":
        intel_z3_formula(args.file)
    elif args.mode == "intel-z3-library":
        intel_z3_library(args.evidence)
    elif args.mode == "apt-packages":
        apt_packages(args.evidence)
    elif args.mode == "stage0-packages":
        stage0_packages(args.evidence)
    elif args.mode == "stage0":
        stage0(args.evidence)
    elif args.mode == "headers":
        headers(args.evidence)
    elif args.mode == "compiler-rt-install-path":
        compiler_rt_install_path(args.prefix)
    elif args.mode == "recipe":
        recipe(args.evidence)
    elif args.mode == "disk":
        disk_preflight(args.directory, args.evidence)
    elif args.mode == "pack":
        pack(args.prefix, args.recipe, args.archive)
    else:
        unpack(args.archive, args.prefix, args.sha256, args.key)


if __name__ == "__main__":
    try:
        main()
    except (OSError, KeyError, IndexError, ValueError, tarfile.TarError, subprocess.SubprocessError) as error:
        print(f"HARNESS_ERROR: {error}", file=sys.stderr)
        sys.exit(2)
