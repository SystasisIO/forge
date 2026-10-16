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
        expected_files.add(path.resolve())
        if not path.is_file():
            errors.append(f"{component}: missing {path.relative_to(root)}")
            continue
        match = declaration.search(path.read_text())
        if not match or match.group(1) != module:
            actual = match.group(1) if match else "<none>"
            errors.append(f"{component}: expected module {module}, got {actual}")

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


def verify_optimizing_sources(root: pathlib.Path, spring: pathlib.Path, llvm_include: pathlib.Path,
                              platform_donor: pathlib.Path | None = None,
                              epoch_donor: pathlib.Path | None = None) -> list[str]:
    """Extend the existing provenance gate; pinned Git blobs ignore dirty donor files."""
    manifest = json.loads((root / "tests/vm_wasm/optimizing_source_manifest.json").read_text())
    errors = []
    donor = manifest["spring"]
    for source in donor["sources"]:
        content = subprocess.check_output(["git", "-C", str(spring), "show", f"{donor['commit']}:{source['path']}"])
        if hashlib.sha256(content).hexdigest() != source["sha256"]:
            errors.append(f"optimizing donor hash differs: {source['path']}")
        for target in source["targets"]:
            if not (root / target).is_file():
                errors.append(f"optimizing adaptation target is missing: {target}")
    for license_path in donor["licenses"]:
        if not (root / license_path).is_file():
            errors.append(f"optimizing donor notice is missing: {license_path}")
    reuse = manifest["eos_vm"]
    existing = json.loads((root / reuse["manifest"]).read_text())
    if existing["donor"]["commit"] != reuse["commit"]:
        errors.append("optimizing parser reuse has a different pinned EOS VM source")
    components = {item["component"]: item for item in existing["components"]}
    for name in reuse["components"]:
        if name not in components or not (root / components[name]["forge"]).is_file():
            errors.append(f"optimizing parser reuse component is missing: {name}")
    for header in manifest["llvm"]["headers"]:
        path = llvm_include / header["path"]
        if not path.is_file() or sha256(path) != header["sha256"]:
            errors.append(f"optimizing LLVM header differs from the pinned dependency: {header['path']}")
    config = llvm_include / "llvm/Config/llvm-config.h"
    if not config.is_file() or f'#define LLVM_VERSION_STRING "{manifest["llvm"]["version"]}"' not in config.read_text():
        errors.append("optimizing LLVM version differs from its compile identity")
    for family, snapshot in (("wasmtime_platform", platform_donor), ("wasmtime_epoch", epoch_donor)):
        if snapshot is None:
            continue
        for source in manifest[family]["sources"]:
            path = snapshot / source["path"]
            if not path.is_file() or sha256(path) != source["sha256"]:
                errors.append(f"optimizing {family} donor differs from its pinned hash: {source['path']}")
            for target in source["targets"]:
                if not (root / target).is_file():
                    errors.append(f"optimizing platform adaptation target is missing: {target}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--donor", type=pathlib.Path)
    parser.add_argument("--root", type=pathlib.Path, required=True)
    parser.add_argument("--optimizing-donor", type=pathlib.Path)
    parser.add_argument("--optimizing-platform-donor", type=pathlib.Path)
    parser.add_argument("--optimizing-epoch-donor", type=pathlib.Path)
    parser.add_argument("--llvm-include", type=pathlib.Path)
    parser.add_argument(
        "--manifest",
        type=pathlib.Path,
        default=pathlib.Path(__file__).with_name("donor_source_manifest.json"),
    )
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--write", action="store_true")
    action.add_argument("--check", action="store_true")
    arguments = parser.parse_args()

    if arguments.optimizing_donor:
        if not arguments.check or not arguments.llvm_include:
            parser.error("optimizing provenance requires --check and --llvm-include")
        platform_donor = arguments.optimizing_platform_donor.resolve() if arguments.optimizing_platform_donor else None
        epoch_donor = arguments.optimizing_epoch_donor.resolve() if arguments.optimizing_epoch_donor else None
        errors = verify_optimizing_sources(arguments.root.resolve(), arguments.optimizing_donor.resolve(), arguments.llvm_include.resolve(), platform_donor, epoch_donor)
        if errors:
            print("\n".join(errors))
            return 1
        print("PASS pinned optimizing provenance: Spring blobs, existing EOS VM owner, LLVM dependency and notices")
        if platform_donor is not None:
            print("PASS pinned Wasmtime platform pattern provenance: exact source hashes and owning Forge files")
        if epoch_donor is not None:
            print("PASS pinned Wasmtime epoch pattern provenance: exact source hashes and owning Forge files")
        return 0
    if not arguments.donor:
        parser.error("EOS VM source verification requires --donor")
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
