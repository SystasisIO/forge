#!/usr/bin/env python3
"""Check CTest's selected registry, without running or filtering any test."""

import argparse
import json
import os
from pathlib import Path
import sys


def require_file(value, test, executable=False):
    path = Path(value)
    if not path.is_file() or (executable and not os.access(path, os.X_OK)):
        raise ValueError(f"{test}: missing {'executable' if executable else 'file'}: {value}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("registry", type=Path)
    parser.add_argument("--jit", choices=("ON", "OFF"), required=True)
    parser.add_argument("--aot", choices=("ON", "OFF"), required=True)
    args = parser.parse_args()
    tests = json.loads(args.registry.read_text())["tests"]
    if not tests:
        raise ValueError("empty selected test registry")
    names = {test["name"] for test in tests}
    if len(names) != len(tests):
        raise ValueError("duplicate selected test names")
    required = {
        "test_forge_vm_wasm_invocation",
        "test_forge_vm_wasm_context_exit_fp",
        "test_forge_vm_wasm_context_exit_allocation_failure",
        "test_forge_vm_wasm_numeric",
        "test_forge_vm_wasm_floating_point_environment",
        "test_forge_vm_wasm_softfloat_tls",
        "test_forge_vm_wasm_profile_public",
        "test_forge_wamr_fp_status",
        "test_forge_wamr_fp_status_provider_selftest",
    }
    if args.jit == "ON":
        required.update(("test_forge_wamr_jit_bridge", "test_forge_wamr_jit_native_maps",
                         "test_forge_wamr_jit_error_threads", "test_forge_wamr_jit_profile_map",
                         "test_forge_package_vm_wasm_interpret_component"))
    if args.aot == "ON":
        required.update(("wamr_export_names_compile", "wamr_export_names_runtime",
                         "wamr_optimizing_session_bounded", "wamr_optimizing_session_probe_late",
                         "wamr_optimizing_session_probe_overlap", "wamr_optimizing_session_cleanup",
                         "wamr_optimizing_8192_preparation", "wamr_optimizing_guest_depth",
                         "test_forge_package_vm_wasm_optimizing_component",
                         "wamr_optimizing_stack_capacity_entry", "wamr_optimizing_stack_capacity_start",
                         "wamr_optimizing_stack_capacity_post_instantiate"))
    if not required <= names:
        raise ValueError(f"missing selected registrations: {sorted(required - names)}")
    for test in tests:
        name = test["name"]
        command = test.get("command", [])
        if not command:
            raise ValueError(f"{name}: no resolved CTest command (unbuilt target?)")
        require_file(command[0], name, executable=True)
        properties = {prop["name"]: prop["value"] for prop in test.get("properties", [])}
        if properties.get("DISABLED"):
            raise ValueError(f"{name}: selected test is disabled")
        for flag in ("--binary", "--generator", "--map"):
            if flag in command:
                require_file(command[command.index(flag) + 1], name, executable=(flag != "--map"))
        for value in command[1:]:
            if value.startswith("-DFORGE_PACKAGE_TEST_HELPER="):
                require_file(value.split("=", 1)[1], name, executable=True)
            elif Path(value).name == "forge-wasm-compiler":
                require_file(value, name, executable=True)
    print(f"READY: {len(tests)} selected tests; all commands/wrapped helpers/maps exist; no tests run")


if __name__ == "__main__":
    try:
        main()
    except (OSError, KeyError, IndexError, ValueError) as error:
        print(f"HARNESS_ERROR: {error}", file=sys.stderr)
        sys.exit(2)
