#!/usr/bin/env python3
"""Fail-closed qualification of the SHA-pinned official Linux LLVM archive."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
import tempfile


GNU = re.compile(r"GLIBCXX_|__gnu_(?:cxx|debug)::|std::__(?:cxx11|cxx1998|exception_ptr|debug|8)::|std::(?:basic_string|vector|deque|list|map|set|unordered_map|unordered_set)<")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def inside(file, prefix):
    return file.resolve().is_relative_to(prefix)


def command(argv, log, env=None):
    with log.open("w") as output:
        result = subprocess.run([str(x) for x in argv], stdout=output, stderr=subprocess.STDOUT,
                                env=env, timeout=300, check=False)
    require(result.returncode == 0, f"command failed ({result.returncode}); see {log}")
    return log.read_text(errors="replace")


def dependencies(file, evidence, env, prefix, name):
    command(["readelf", "--dynamic", "--wide", file], evidence / f"{name}-dynamic.txt", env)
    versions = command(["readelf", "--version-info", file], evidence / f"{name}-versions.txt", env)
    require("GLIBCXX_" not in versions, f"GNU C++ version requirement: {file}")
    text = command(["ldd", file], evidence / f"{name}-ldd.txt", env)
    require("not found" not in text, f"unresolved dependency: {file}")
    require("libstdc++" not in text and "GLIBCXX_" not in text, f"GNU C++ dependency: {file}")
    loaded = []
    for line in text.splitlines():
        match = re.search(r"(?:=>\s+)?(/\S+)", line)
        if not match:
            continue
        library = Path(match.group(1))
        loaded.append(library)
        if library.name.startswith(("libc++.", "libc++abi.", "libunwind.", "libLLVM", "libclang")):
            require(inside(library, prefix), f"C++ runtime outside pinned prefix: {library}")
    return loaded


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path)
    parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    require(platform.system() == "Linux" and platform.machine() == "x86_64", "native Linux x86_64 required")
    prefix = args.prefix.resolve(strict=True)
    out = args.evidence.resolve()
    out.mkdir(parents=True, exist_ok=False)
    tools = {name: prefix / "bin" / name for name in ("clang", "clang++", "clang-scan-deps", "llvm-config", "llvm-nm")}
    require(all(tool.is_file() for tool in tools.values()), "incomplete official LLVM toolchain")
    version = command([tools["llvm-config"], "--version"], out / "version.txt").strip()
    require(version == "22.1.8", f"LLVM version is {version}")
    for tool in ("clang", "clang++", "clang-scan-deps"):
        reported = command([tools[tool], "--version"], out / f"{tool}-version.txt")
        require(re.search(r"(?:clang|LLVM) version 22\.1\.8(?:\s|$)", reported), f"{tool} version is not 22.1.8")
    rtti = command([tools["llvm-config"], "--has-rtti"], out / "rtti.txt").strip()
    require(rtti == "YES", f"LLVM RTTI is {rtti}, not YES")
    targets = command([tools["llvm-config"], "--targets-built"], out / "targets.txt").split()
    require("X86" in targets, "LLVM native X86 target missing")
    cxxflags = command([tools["llvm-config"], "--cxxflags"], out / "llvm-cxxflags.txt")
    require("-stdlib=libstdc++" not in cxxflags and "-fno-rtti" not in cxxflags, "contrary LLVM C++ metadata")
    lib = Path(command([tools["clang++"], "-stdlib=libc++", "-print-file-name=libc++.so"],
                       out / "libcxx-path.txt").strip())
    require(lib.is_absolute() and lib.is_file() and inside(lib, prefix), "bundled shared libc++ not resolved")
    libdir = lib.parent
    flags = "-stdlib=libc++"
    ldflags = f"-stdlib=libc++ -L{libdir} -Wl,-rpath,{libdir}"
    env = dict(os.environ, CXXFLAGS=flags, LDFLAGS=ldflags,
               LD_LIBRARY_PATH=f"{libdir}:{prefix / 'lib'}")
    probe = Path(__file__).resolve().parents[1] / "probes" / "llvm-cxx-abi"
    preprocessed = command([tools["clang++"], "-stdlib=libc++", "-std=c++23", "-E", "-H",
                            "-x", "c++", "-include", "vector", "/dev/null"], out / "headers.txt", env)
    header_lines = [line for line in preprocessed.splitlines() if re.match(r"^\.+ ", line)]
    require(header_lines, "missing actual header path evidence")
    for line in header_lines:
        header = Path(re.sub(r"^\.+ ", "", line).split(" (", 1)[0])
        if "c++" in header.parts:
            require(inside(header, prefix), f"C++ header outside pinned prefix: {header}")
    macros = command([tools["clang++"], "-stdlib=libc++", "-std=c++23", "-dM", "-E", "-x", "c++",
                      "-include", "vector", "/dev/null"], out / "macros.txt", env)
    require(re.search(r"^#define _LIBCPP_VERSION 220108$", macros, re.M), "libc++ header version is not 220108")
    build = out / "build"
    command(["cmake", "-S", probe, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
             f"-DCMAKE_C_COMPILER={tools['clang']}", f"-DCMAKE_CXX_COMPILER={tools['clang++']}",
             f"-DLLVM_DIR={prefix / 'lib/cmake/llvm'}", f"-DCMAKE_CXX_FLAGS={flags}",
             f"-DCMAKE_EXE_LINKER_FLAGS={ldflags}"], out / "configure.txt", env)
    selected_paths = [Path(x) for x in (build / "selected-libraries.txt").read_text().splitlines() if x]
    selected = sorted({file.resolve(strict=True) for file in selected_paths})
    require(selected, "no LLVM libraries selected")
    summaries = []
    positive = False
    for index, library in enumerate(selected):
        require(inside(library, prefix), f"selected LLVM library outside pinned prefix: {library}")
        # Stream the potentially large archive symbol table; retain hashes and evidence,
        # not hundreds of megabytes of clean demangled symbol listings.
        with tempfile.TemporaryFile(mode="w+") as symbols:
            nm_args = [str(tools["llvm-nm"]), "--demangle"]
            if library.suffix != ".a":
                nm_args.append("--dynamic")
            process = subprocess.run([*nm_args, str(library)],
                                     stdout=symbols, stderr=subprocess.PIPE, text=True, timeout=300)
            require(process.returncode == 0, f"cannot inspect {library}: {process.stderr}")
            symbols.seek(0)
            digest = hashlib.sha256()
            count = 0
            local_positive = False
            for line in symbols:
                digest.update(line.encode())
                count += 1
                require(not GNU.search(line), f"GNU C++ ABI in {library}: {line.strip()}")
                local_positive |= "std::__1::" in line
            positive |= local_positive
            summaries.append({"path": str(library), "symbol_lines": count, "symbol_sha256": digest.hexdigest(),
                              "libcxx_abi_witness": local_positive})
        if library.suffix != ".a":
            dependencies(library, out, env, prefix, f"library-{index}")
    (out / "selected-library-audit.json").write_text(json.dumps(summaries, indent=2) + "\n")
    require(positive, "no positive libc++ ABI witness in selected LLVM libraries")
    command(["cmake", "--build", build, "--target", "llvm_cxx_abi", "-j", "2"], out / "build.txt", env)
    binary = build / "llvm_cxx_abi"
    linkmap = (build / "llvm-cxx-abi.linkmap").read_text()
    require(any(str(file) in linkmap for file in selected_paths), "no selected LLVM provider in smoke link map")
    require("libstdc++" not in linkmap, "GNU C++ library in smoke link map")
    loaded = dependencies(binary, out, env, prefix, "smoke")
    require(any(lib.name.startswith("libc++.") for lib in loaded), "smoke does not resolve bundled libc++")
    command([binary], out / "run.txt", env)
    (out / "result.json").write_text(json.dumps({"state": "READY", "LLVM": version, "RTTI": rtti,
                                                "selected_libraries": len(selected), "libcxx": str(lib.resolve())}, indent=2) + "\n")
    with Path(os.environ["GITHUB_ENV"]).open("a") as github:
        github.write(f"CC={tools['clang']}\nCXX={tools['clang++']}\nLLVM_PREFIX={prefix}\n")
        github.write(f"CXXFLAGS={flags}\nLDFLAGS={ldflags}\nLD_LIBRARY_PATH={env['LD_LIBRARY_PATH']}\n")
    print("READY: official LLVM22.1.8 X86/RTTI/libc++ ABI preflight; full Forge tests still required")


if __name__ == "__main__":
    try:
        main()
    except (OSError, KeyError, ValueError, subprocess.SubprocessError) as error:
        print(f"HARNESS_ERROR: {error}", file=sys.stderr)
        sys.exit(2)
