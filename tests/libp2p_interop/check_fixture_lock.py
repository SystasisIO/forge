#!/usr/bin/env python3
import json
import sys
from pathlib import Path

sys.dont_write_bytecode = True

from provenance import (
    fixture_donor_checkout_errors,
    fixture_donor_revision_bindings,
    graph_hash,
    load_canonical_donor_revisions,
    sha256_file,
)


EXPECTED_FORGE_BUILD_PROFILES = ["default", "Debug", "Release", "RelWithDebInfo", "MinSizeRel"]
EXPECTED_TOOLCHAINS = {
    "go": {
        "version": "1.26.3",
        "gotoolchain": "local",
        "goproxy": "off",
        "gosumdb": "off",
    },
    "rust": {
        "rustc_version": "1.95.0",
        "cargo_version": "1.95.0",
        "cargo_frozen": True,
        "cargo_net_offline": True,
    },
    "forge_fixture": {
        "compiler_id": "Clang",
        "compiler_version": "22.1.8",
        "build_profiles": EXPECTED_FORGE_BUILD_PROFILES,
    },
}

EXPECTED_FIXTURE_FILES = {
    "fixtures/pnet/mismatched-swarm.key",
    "fixtures/pnet/swarm.key",
    "go_fixture/go.mod",
    "go_fixture/go.sum",
    "go_fixture/main.go",
    "go_fixture/autonat.go",
    "go_fixture/autonat_test.go",
    "go_fixture/provider_evidence.go",
    "go_fixture/provider_evidence_test.go",
    "go_fixture/upgrade_observer.go",
    "go_fixture/upgrade_observer_test.go",
    "rust_fixture/Cargo.lock",
    "rust_fixture/Cargo.toml",
    "rust_fixture/main.rs",
    "rust_fixture/autonat.rs",
    "rust_fixture/provider.rs",
    "rust_fixture/rust-toolchain.toml",
}
EXPECTED_RUNTIME_ARTIFACT_SOURCES = {
    "../CMakeLists.txt",
    "check_fixture_lock.py",
    "autonat_cases.py",
    "autonat_acceptance.py",
    "autonat_evidence.py",
    "autonat_network.py",
    "dns_fixture.py",
    "dns_evidence.py",
    "forge_interop_fixture.cpp",
    "forge_autonat_fixture.cpp",
    "forge_autonat_fixture.hxx",
    "process_lifecycle.py",
    "provider_evidence.py",
    "provenance.py",
    "runner.py",
}
EXPECTED_EVIDENCE_SOURCES = {"donor_cases.json"}
EXPECTED_REGRESSION_SOURCES = {
    "test_provenance.py", "test_dns_fixture.py", "test_process_lifecycle.py", "test_provider_evidence.py",
    "test_autonat_cases.py", "test_autonat_evidence.py", "test_autonat_network.py",
    "test_autonat_acceptance.py", "test_identify_evidence.py", "test_relay_evidence.py",
}


def check_hashes(root: Path, label: str, values: object, expected_paths: set[str], errors: list[str]) -> None:
    if not isinstance(values, dict):
        errors.append(f"fixture lock {label} must be an object")
        return
    if set(values) != expected_paths:
        errors.append(f"fixture lock {label} sources do not match the supported exact baseline")
        return
    for relative, expected in values.items():
        if not isinstance(relative, str) or not isinstance(expected, str):
            errors.append(f"fixture lock {label} has an invalid hash entry")
            continue
        path = root / relative
        if not path.is_file():
            errors.append(f"fixture lock {label} source is missing: {relative}")
            continue
        actual = sha256_file(path)
        if actual != expected:
            errors.append(f"fixture lock {label} hash mismatch: {relative}")


def check_toolchains(values: object, errors: list[str]) -> None:
    if not isinstance(values, dict) or set(values) != set(EXPECTED_TOOLCHAINS):
        errors.append("fixture lock toolchains do not match the supported exact interop baseline")
        return
    forge_fixture = values.get("forge_fixture")
    expected_fixture = EXPECTED_TOOLCHAINS["forge_fixture"]
    if not isinstance(forge_fixture, dict) or set(forge_fixture) != set(expected_fixture):
        errors.append("fixture lock Forge fixture toolchain requirements are malformed")
        return
    if (
        forge_fixture.get("compiler_id") != expected_fixture["compiler_id"]
        or forge_fixture.get("compiler_version") != expected_fixture["compiler_version"]
        or forge_fixture.get("build_profiles") != EXPECTED_FORGE_BUILD_PROFILES
    ):
        errors.append("fixture lock Forge fixture toolchain requirements do not match the supported exact baseline")
    if values.get("go") != EXPECTED_TOOLCHAINS["go"] or values.get("rust") != EXPECTED_TOOLCHAINS["rust"]:
        errors.append("fixture lock toolchains do not match the supported exact interop baseline")


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: check_fixture_lock.py SOURCE_DIR DONORS_ROOT", file=sys.stderr)
        return 2
    source_dir = Path(sys.argv[1]).resolve()
    donors_root = Path(sys.argv[2]).resolve()
    try:
        lock = json.loads((source_dir / "fixture-lock.json").read_text())
    except (OSError, json.JSONDecodeError) as error:
        print(f"ERROR: fixture lock: {error}", file=sys.stderr)
        return 1
    errors: list[str] = []
    if lock.get("schema_version") != 2:
        errors.append("fixture lock schema_version must be 2")
    check_hashes(source_dir, "fixture_files", lock.get("fixture_files"), EXPECTED_FIXTURE_FILES, errors)
    check_hashes(
        source_dir,
        "runtime_artifact_sources",
        lock.get("runtime_artifact_sources"),
        EXPECTED_RUNTIME_ARTIFACT_SOURCES,
        errors,
    )
    check_hashes(source_dir, "evidence_sources", lock.get("evidence_sources"), EXPECTED_EVIDENCE_SOURCES, errors)
    check_hashes(
        source_dir,
        "regression_sources",
        lock.get("regression_sources"),
        EXPECTED_REGRESSION_SOURCES,
        errors,
    )
    check_toolchains(lock.get("toolchains"), errors)

    graphs = lock.get("dependency_graphs")
    if not isinstance(graphs, dict):
        errors.append("fixture lock dependency_graphs must be an object")
    else:
        for name, value in graphs.items():
            if not isinstance(name, str) or not isinstance(value, dict):
                errors.append("fixture lock has an invalid dependency graph")
                continue
            paths = value.get("files")
            expected = value.get("sha256")
            if not isinstance(paths, list) or not all(isinstance(path, str) for path in paths) or not isinstance(expected, str):
                errors.append(f"fixture lock dependency graph {name} is invalid")
                continue
            if graph_hash(source_dir, paths) != expected:
                errors.append(f"fixture lock dependency graph hash mismatch: {name}")

    donors = lock.get("donors")
    if not isinstance(donors, list) or len(donors) != 5:
        errors.append("fixture lock must contain the five pinned donors")
    else:
        try:
            canonical_donor_revisions = load_canonical_donor_revisions(
                source_dir / "donor_cases.json"
            )
        except RuntimeError as error:
            errors.append(str(error))
            canonical_donor_revisions = {}
        _, binding_errors = fixture_donor_revision_bindings(
            donors, canonical_donor_revisions
        )
        errors.extend(binding_errors)
        if not binding_errors:
            errors.extend(
                fixture_donor_checkout_errors(donors_root, donors, canonical_donor_revisions)
            )

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print("fixture lock ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
