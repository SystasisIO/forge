#!/usr/bin/env python3
"""Verify the one-component-per-module mapping from the pinned EOS VM source."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess


DONOR_COMMIT = "e5b1fc79c4b8d78f32749afa94a8d4c4d071f67f"
DONOR_PREFIX = "include/eosio/vm"
FORGE_PREFIX = "libraries/vm/wasm/interpret/include/forge/vm/wasm/interpret"

PRIMARY_COMPONENTS = {
    "allocator": ("allocator.hpp", "allocator.cppm", "forge.vm.wasm.interpret.allocator"),
    "argument_proxy": ("argument_proxy.hpp", "argument_proxy.cppm", "forge.vm.wasm.interpret.argument_proxy"),
    "backend": ("backend.hpp", "backend.cppm", "forge.vm.wasm.interpret.backend"),
    "base_visitor": ("base_visitor.hpp", "base_visitor.cppm", "forge.vm.wasm.interpret.backend:base_visitor"),
    "bitcode_writer": ("bitcode_writer.hpp", "bitcode_writer.cppm", "forge.vm.wasm.interpret.backend:bitcode_writer"),
    "config": ("config.hpp", "config.cppm", "forge.vm.wasm.interpret.backend:config"),
    "constants": ("constants.hpp", "constants.cppm", "forge.vm.wasm.interpret.constants"),
    "debug_info": ("debug_info.hpp", "debug_info.cppm", "forge.vm.wasm.interpret.debug_info"),
    "debug_visitor": ("debug_visitor.hpp", "debug_visitor.cppm", "forge.vm.wasm.interpret.backend:debug_visitor"),
    "exceptions": ("exceptions.hpp", "exceptions.cppm", "forge.vm.wasm.interpret.exceptions"),
    "execution_context": ("execution_context.hpp", "execution_context.cppm", "forge.vm.wasm.interpret.backend:execution_context"),
    "execution_interface": ("execution_interface.hpp", "execution_interface.cppm", "forge.vm.wasm.interpret.execution_interface"),
    "function_traits": ("function_traits.hpp", "function_traits.cppm", "forge.vm.wasm.interpret.host_function:function_traits"),
    "guarded_ptr": ("guarded_ptr.hpp", "guarded_ptr.cppm", "forge.vm.wasm.interpret.guarded_ptr"),
    "host_function": ("host_function.hpp", "host_function.cppm", "forge.vm.wasm.interpret.host_function"),
    "interpret_visitor": ("interpret_visitor.hpp", "interpret_visitor.cppm", "forge.vm.wasm.interpret.backend:interpret_visitor"),
    "leb128": ("leb128.hpp", "leb128.cppm", "forge.vm.wasm.interpret.backend:leb128"),
    "null_writer": ("null_writer.hpp", "null_writer.cppm", "forge.vm.wasm.interpret.backend:null_writer"),
    "opcodes": ("opcodes.hpp", "opcodes.cppm", "forge.vm.wasm.interpret.opcodes"),
    "options": ("options.hpp", "options.cppm", "forge.vm.wasm.interpret.options"),
    "parser": ("parser.hpp", "parser.cppm", "forge.vm.wasm.interpret.backend:parser"),
    "scope_guard": ("utils.hpp", "scope_guard.cppm", "forge.vm.wasm.interpret.scope_guard"),
    "sections": ("sections.hpp", "sections.cppm", "forge.vm.wasm.interpret.backend:sections"),
    "signals": ("signals.hpp", "signals.cppm", "forge.vm.wasm.interpret.backend:signals"),
    "softfloat": ("softfloat.hpp", "softfloat.cppm", "forge.vm.wasm.interpret.backend:softfloat"),
    "span": ("span.hpp", "span.cppm", "forge.vm.wasm.interpret.span"),
    "stack_elem": ("stack_elem.hpp", "stack_elem.cppm", "forge.vm.wasm.interpret.stack_elem"),
    "types": ("types.hpp", "types.cppm", "forge.vm.wasm.interpret.types"),
    "utils": ("utils.hpp", "utils.cppm", "forge.vm.wasm.interpret.utils"),
    "variant": ("variant.hpp", "variant.cppm", "forge.vm.wasm.interpret.variant"),
    "vector": ("vector.hpp", "vector.cppm", "forge.vm.wasm.interpret.vector"),
    "wasm_stack": ("wasm_stack.hpp", "wasm_stack.cppm", "forge.vm.wasm.interpret.wasm_stack"),
    "watchdog": ("watchdog.hpp", "watchdog.cppm", "forge.vm.wasm.interpret.watchdog"),
    "x86_64": ("x86_64.hpp", "x86_64.cppm", "forge.vm.wasm.interpret.backend:x86_64"),
}

SUPPORT_COMPONENTS = {
    "error_codes": ("error_codes.hpp", "exceptions.cppm", "merged into typed Forge exceptions"),
    "error_codes_def": ("error_codes_def.hpp", "exceptions.cppm", "merged into typed Forge exceptions"),
    "error_codes_pp": ("error_codes_pp.hpp", "exceptions.cppm", "merged into typed Forge exceptions"),
    "opcode_macros": ("opcodes_def.hpp", "opcode_macros.hpp", "macro-only opcode table"),
}

EXCLUSIONS = {
    "disassembly_visitor.hpp": "not included by the donor active backend or tests",
    "memory_dump.hpp": "inactive donor debugging component",
    "profile.hpp": "inactive donor profiling component",
    "validation.hpp": "not included by the donor active backend or tests",
}

# The manifest remains immutable historical provenance, not the WAMR build graph.
RETIRED_COMPONENTS = {
    "base_visitor", "bitcode_writer", "config", "debug_visitor", "execution_context",
    "interpret_visitor", "null_writer", "parser", "sections", "softfloat", "x86_64",
}

WAMR_COMPONENTS = {
    "capacity_profile.cppm": "forge.vm.wasm.interpret.capacity_profile",
    "floating_point_environment.cppm": "forge.vm.wasm.interpret.floating_point_environment",
    "prepared_module.cppm": "forge.vm.wasm.interpret.prepared_module",
    "runtime_backend.cppm": "forge.vm.wasm.interpret.backend:runtime_backend",
}


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def donor_head(donor: pathlib.Path) -> str:
    return subprocess.check_output(["git", "-C", str(donor), "rev-parse", "HEAD"], text=True).strip()


def build_manifest(donor: pathlib.Path) -> dict[str, object]:
    if donor_head(donor) != DONOR_COMMIT:
        raise RuntimeError(f"donor HEAD must be {DONOR_COMMIT}")

    components = []
    for component, (source, target, module) in sorted(PRIMARY_COMPONENTS.items()):
        donor_path = donor / DONOR_PREFIX / source
        components.append(
            {
                "component": component,
                "donor": f"{DONOR_PREFIX}/{source}",
                "donor_sha256": sha256(donor_path),
                "forge": f"{FORGE_PREFIX}/{target}",
                "module": module,
                "status": "ported",
            }
        )

    for component, (source, target, reason) in sorted(SUPPORT_COMPONENTS.items()):
        donor_path = donor / DONOR_PREFIX / source
        components.append(
            {
                "component": component,
                "donor": f"{DONOR_PREFIX}/{source}",
                "donor_sha256": sha256(donor_path),
                "forge": f"{FORGE_PREFIX}/{target}",
                "reason": reason,
                "status": "adapted",
            }
        )

    for source, reason in sorted(EXCLUSIONS.items()):
        donor_path = donor / DONOR_PREFIX / source
        components.append(
            {
                "component": pathlib.Path(source).stem,
                "donor": f"{DONOR_PREFIX}/{source}",
                "donor_sha256": sha256(donor_path),
                "reason": reason,
                "status": "excluded",
            }
        )

    return {
        "donor": {
            "commit": DONOR_COMMIT,
            "repository": "https://github.com/AntelopeIO/eos-vm.git",
        },
        "components": components,
    }


def verify_forge_units(root: pathlib.Path) -> list[str]:
    errors = []
    expected_files = set()
    declaration = re.compile(r"^\s*export\s+module\s+([^;]+);", re.MULTILINE)

    for component, (_, target, module) in sorted(PRIMARY_COMPONENTS.items()):
        path = root / FORGE_PREFIX / target
        if component in RETIRED_COMPONENTS:
            if path.exists():
                errors.append(f"{component}: retired module remains: {path.relative_to(root)}")
            continue
        expected_files.add(path.resolve())
        if not path.is_file():
            errors.append(f"{component}: missing {path.relative_to(root)}")
            continue
        match = declaration.search(path.read_text())
        if not match or match.group(1) != module:
            actual = match.group(1) if match else "<none>"
            errors.append(f"{component}: expected module {module}, got {actual}")

    for target, module in sorted(WAMR_COMPONENTS.items()):
        path = root / FORGE_PREFIX / target
        expected_files.add(path.resolve())
        if not path.is_file():
            errors.append(f"missing WAMR module: {path.relative_to(root)}")
            continue
        match = declaration.search(path.read_text())
        if not match or match.group(1) != module:
            actual = match.group(1) if match else "<none>"
            errors.append(f"{target}: expected module {module}, got {actual}")

    actual_files = {path.resolve() for path in (root / FORGE_PREFIX).glob("*.cppm")}
    for path in sorted(expected_files - actual_files):
        errors.append(f"missing mapped module unit: {path.relative_to(root)}")
    for path in sorted(actual_files - expected_files):
        errors.append(f"unmapped module unit: {path.relative_to(root)}")

    for _, (_, target, _) in sorted(SUPPORT_COMPONENTS.items()):
        path = root / FORGE_PREFIX / target
        if not path.is_file():
            errors.append(f"missing adapted source: {path.relative_to(root)}")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--donor", type=pathlib.Path, required=True)
    parser.add_argument("--root", type=pathlib.Path, required=True)
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        default=pathlib.Path(__file__).with_name("donor_source_manifest.json"),
    )
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--write", action="store_true")
    action.add_argument("--check", action="store_true")
    arguments = parser.parse_args()

    manifest_text = json.dumps(build_manifest(arguments.donor.resolve()), indent=2, sort_keys=True) + "\n"
    if arguments.write:
        arguments.manifest.write_text(manifest_text)
        return 0

    errors = verify_forge_units(arguments.root.resolve())
    if not arguments.manifest.is_file() or arguments.manifest.read_text() != manifest_text:
        errors.append("donor source manifest differs from the pinned source")
    if errors:
        print("\n".join(errors))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
