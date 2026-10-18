#!/usr/bin/env python3
"""Admission checks for the exact VM CI compiler and libc++ header contract."""

import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


VERSION = "22.1.8"
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_subparsers(dest="mode", required=True)
    check = mode.add_parser("bottle")
    check.add_argument("file", type=Path)
    check.add_argument("tag")
    apt = mode.add_parser("apt-packages")
    apt.add_argument("evidence", type=Path)
    header = mode.add_parser("headers")
    header.add_argument("evidence", type=Path)
    args = parser.parse_args()
    if args.mode == "bottle":
        bottle(args.file, args.tag)
    elif args.mode == "apt-packages":
        apt_packages(args.evidence)
    else:
        headers(args.evidence)


if __name__ == "__main__":
    try:
        main()
    except (OSError, KeyError, IndexError, ValueError, subprocess.SubprocessError) as error:
        print(f"HARNESS_ERROR: {error}", file=sys.stderr)
        sys.exit(2)
