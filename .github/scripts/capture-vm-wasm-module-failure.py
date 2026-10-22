#!/usr/bin/env python3
"""Capture the failing executor module graph without compiling or changing it."""

import argparse
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess


def fingerprint(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": digest.hexdigest()}


def command(argv, cwd, source=None):
    try:
        result = subprocess.run(argv, cwd=cwd, input=source, capture_output=True, text=True,
                                errors="backslashreplace", timeout=60, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        return {"argv": argv, "cwd": str(cwd), "state": "PROBE_ERROR", "error": str(error),
                "exit_code": None, "stdout": "", "stderr": ""}
    return {"argv": argv, "cwd": str(cwd), "exit_code": result.returncode,
            "stdout": result.stdout, "stderr": result.stderr}


def arguments(row):
    return row.get("arguments") or shlex.split(row["command"])


def capture(build, output):
    output.mkdir(parents=True, exist_ok=True)
    database = build / "compile_commands.json"
    if not database.is_file():
        (output / "state.json").write_text(json.dumps({"state": "NO_MODULE_BUILD", "build": str(build)}) + "\n")
        return
    rows = json.loads(database.read_text())
    consumer = next(row for row in rows if row["file"].endswith("/tests/vm_wasm_execution/execution_tests.cpp"))
    consumer_args = arguments(consumer)
    cwd = Path(consumer["directory"])
    response = next(cwd / arg[1:] for arg in consumer_args if arg.startswith("@"))
    flags = shlex.split(response.read_text())
    module_paths = {}
    for flag in flags:
        if flag.startswith("-fmodule-file="):
            name, file = flag.removeprefix("-fmodule-file=").split("=", 1)
            module_paths[name] = (cwd / file).resolve()
    if len(module_paths) > 64 or sum(path.stat().st_size for path in module_paths.values()) > 1024 ** 3:
        raise ValueError("Diagnostic module graph exceeds the 64-file/one-GiB capture budget")
    selected = [consumer]
    response_files = [response]
    for row in rows:
        if not row["file"].endswith(".cppm"):
            continue
        row_cwd = Path(row["directory"])
        row_response = next((row_cwd / arg[1:] for arg in arguments(row) if arg.startswith("@")), None)
        if row_response is None or not row_response.is_file():
            continue
        module_flags = shlex.split(row_response.read_text())
        target = next((flag.removeprefix("-fmodule-output=") for flag in module_flags
                       if flag.startswith("-fmodule-output=")), None)
        if target and (row_cwd / target).resolve() in module_paths.values():
            selected.append(row)
            response_files.append(row_response)
    compiler = Path(consumer_args[0]).resolve()
    prefix = compiler.parent.parent
    tools = [compiler, compiler.parent / "clang-scan-deps"]
    tools += [path for name in ("libLLVM.dylib", "libclang-cpp.dylib")
              if (path := prefix / "lib" / name).is_file()]
    results = {"driver": command([*consumer_args, "-###"], cwd),
               "compiler": command([str(compiler), "--version"], cwd),
               "scanner": command([str(compiler.parent / "clang-scan-deps"), "--version"], cwd),
               "git": command(["git", "rev-parse", "HEAD"], cwd)}
    header_args = []
    skip = False
    for arg in consumer_args:
        if skip:
            skip = False
            continue
        if arg in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
        elif arg not in ("-c", "-MD", "-MMD", consumer["file"]) and not arg.startswith("@"):
            header_args.append(arg)
    header_source = "".join(f"#include <{name}>\n" for name in
                            ("boost/version.hpp", "boost/test/unit_test.hpp", "boost/describe.hpp",
                             "boost/pfr/core.hpp", "vector", "functional"))
    results["headers"] = command([*header_args, "-E", "-H", "-x", "c++", "-o", "/dev/null", "-"],
                                 cwd, header_source)
    if shutil.which("otool"):
        results["macho"] = [command(["otool", "-L", "-l", str(tool)], cwd) for tool in tools]
    for index, file in enumerate(response_files):
        shutil.copyfile(file, output / f"{index:02d}-{file.name}")
    for index, row in enumerate(selected):
        source = Path(row["file"])
        shutil.copyfile(source, output / f"{index:02d}-{source.name}")
    fixture = Path(consumer["file"]).with_name("fixtures.hxx")
    shutil.copyfile(fixture, output / fixture.name)
    probes = [probe for value in results.values() for probe in (value if isinstance(value, list) else [value])]
    state = "CAPTURE_PARTIAL_NOT_A_TEST_RESULT" if any(probe.get("state") == "PROBE_ERROR" for probe in probes) \
        else "CAPTURED_NOT_A_TEST_RESULT"
    result = {"state": state, "commands": selected, "results": results,
              "modules": {name: fingerprint(file) for name, file in module_paths.items()},
              "tools": [fingerprint(tool) for tool in tools],
              "inputs": [fingerprint(Path(row["file"])) for row in selected] + [fingerprint(fixture)]}
    (output / "state.json").write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        capture(args.build.resolve(), args.output.resolve())
    except (OSError, ValueError, KeyError, StopIteration) as error:
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / "state.json").write_text(json.dumps(
            {"state": "CAPTURE_INCOMPLETE_NOT_A_TEST_RESULT", "build": str(args.build.resolve()),
             "error": str(error)}, indent=2) + "\n")
        raise SystemExit(2)
