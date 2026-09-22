#!/usr/bin/env python3
import ast
import hashlib
import json
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Optional

from autonat_acceptance import (
    EVIDENCE_CONTRACTS as AUTONAT_EVIDENCE_CONTRACTS,
    ROLE_DIRECTIONS,
    SCENARIOS as AUTONAT_SCENARIOS,
)
from check_stage6_acceptance import EVIDENCE_CONTRACT_VALIDATORS, expected_launcher_transport
from mdns_acceptance import SCENARIOS as MDNS_SCENARIOS, EVIDENCE_CONTRACTS as MDNS_EVIDENCE_CONTRACTS
from provenance import (
    donor_checkout_head_errors,
    donor_revision_schema_errors,
    donor_source_object_errors,
)
from stage6_evidence_contract import (
    EVIDENCE_CONTRACT_PREFIX,
    EVIDENCE_CONTRACT_SUFFIX,
    evidence_contract_for,
)


REQUIRED_FIELDS = {
    "id",
    "state",
    "owner",
    "normal_activation",
    "configuration",
    "resource_ownership",
    "persistence",
    "maintenance",
    "diagnostics",
    "intended_disposition",
    "builtin_protocols",
    "capabilities",
    "evidence",
}

EVIDENCE_LAYERS = {
    "codec",
    "state_machine",
    "raw_node",
    "official_plugin",
    "restart_scale",
    "adversarial",
    "donor_interop",
}

REQUIRED_OWNERS = {
    "net.p2p.node": {
        "kind": "library",
        "path": "libraries/net/p2p",
        "target": "forge_net_p2p",
        "component": "net_p2p",
        "module_prefix": "forge.net.p2p",
        "module_root": "net/p2p",
    },
    "plugin.p2p.node": {
        "kind": "plugin",
        "path": "plugins/p2p/node",
        "target": "forge_plugins_p2p_node",
        "component": "plugins_p2p_node",
        "module_prefix": "forge.plugins.p2p.node",
        "module_root": "plugins/p2p/node",
    },
    "plugin.p2p.resolver": {
        "kind": "plugin",
        "path": "plugins/p2p/resolver",
        "target": "forge_plugins_p2p_resolver",
        "component": "plugins_p2p_resolver",
        "module_prefix": "forge.plugins.p2p.resolver",
        "module_root": "plugins/p2p/resolver",
    },
    "plugin.p2p.pubsub": {
        "kind": "plugin",
        "path": "plugins/p2p/pubsub",
        "target": "forge_plugins_p2p_pubsub",
        "component": "plugins_p2p_pubsub",
        "module_prefix": "forge.plugins.p2p.pubsub",
        "module_root": "plugins/p2p/pubsub",
    },
    "plugin.p2p.diagnostics": {
        "kind": "plugin",
        "path": "plugins/p2p/diagnostics",
        "target": "forge_plugins_p2p_diagnostics",
        "component": "plugins_p2p_diagnostics",
        "module_prefix": "forge.plugins.p2p.diagnostics",
        "module_root": "plugins/p2p/diagnostics",
    },
    "application": {"kind": "external"},
    "none": {"kind": "none"},
}


def reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def private_mdns_attribution_errors(capability: dict[str, object]) -> list[str]:
    sources = capability.get("donor_sources", [])
    required = (
        "donors/libp2p-specs/discovery/mdns.md",
        "donors/rust-libp2p/transports/pnet/src/lib.rs",
    )
    if (capability.get("origin") != "libp2p" or not isinstance(sources, list)
            or not all(source in sources for source in required)):
        return [
            "donor capability discovery.mdns_private_fingerprinted: must attribute the namespace "
            "to the libp2p specification and fingerprint computation to pinned Rust pnet"
        ]
    return []


def extract_namespace_names(source: str, namespace: str, declaration: str) -> set[str]:
    match = re.search(rf"namespace {namespace}\s*\{{(?P<body>.*?)\n\}}", source, re.DOTALL)
    if match is None:
        return set()
    return set(re.findall(declaration, match.group("body")))


def has_registered_live_interop(case: object) -> bool:
    if not isinstance(case, dict):
        return False
    scenarios = case.get("forge_live_scenario", [])
    tests = case.get("forge_tests", [])
    if case.get("mapping_state") != "mapped":
        return False
    if not isinstance(scenarios, list) or not scenarios or any(
        not isinstance(selector, dict)
        or set(selector) != {"profile", "scenario"}
        or not isinstance(selector.get("profile"), str)
        or not isinstance(selector.get("scenario"), str)
        for selector in scenarios
    ):
        return False
    if not isinstance(tests, list):
        return False
    return any(
        isinstance(reference, str)
        and reference.strip().split()[0] == "test_forge_libp2p_interop"
        for reference in tests
        if reference.strip()
    )


def donor_case_has_source(case: object, donor_prefix: str) -> bool:
    if not isinstance(case, dict):
        return False
    donor_files = case.get("donor_file", [])
    return isinstance(donor_files, list) and any(
        isinstance(source, str) and source.startswith(donor_prefix)
        for source in donor_files
    )


def donor_case_text(case: object) -> str:
    if not isinstance(case, dict):
        return ""
    values: list[str] = []
    for value in (case.get("donor_case", ""), case.get("known_gap", "")):
        if isinstance(value, str):
            values.append(value)
        elif isinstance(value, list):
            values.extend(item for item in value if isinstance(item, str))
    return " ".join(values).lower()


def donor_case_source_errors(
    root: Path, donor_cases: object, donor_revisions: object, donors_root: Optional[Path]
) -> list[str]:
    """Validate every donor case file directly against its pinned source object."""
    if not isinstance(donor_cases, list):
        return ["donor matrix: cases must be an array"]

    errors: list[str] = []
    for case in donor_cases:
        if not isinstance(case, dict):
            errors.append("donor matrix: every donor case must be an object")
            continue
        case_id = case.get("id")
        label = case_id if isinstance(case_id, str) and case_id else "<unknown>"
        donor_files = case.get("donor_file")
        if not isinstance(donor_files, list):
            errors.append(f"donor case {label}: donor_file must be an array")
            continue
        for donor_file in donor_files:
            if not isinstance(donor_file, str) or not donor_file:
                errors.append(f"donor case {label}: donor_file must contain non-empty strings")
                continue
            relative = Path(donor_file)
            if relative.is_absolute() or ".." in relative.parts:
                errors.append(f"donor case {label}: donor_file must be repository-relative: {donor_file}")
            elif relative.parts and relative.parts[0] == "docs":
                if not (root / relative).is_file():
                    errors.append(f"donor case {label}: local donor_file does not exist: {donor_file}")
            elif relative.parts and relative.parts[0] == "donors" and len(relative.parts) > 2:
                if donors_root is None:
                    errors.append(
                        f"donor case {label}: pinned donor_file validation requires DONORS_ROOT"
                    )
                    continue
                errors.extend(
                    f"donor case {label}: {error}"
                    for error in donor_source_object_errors(
                        donors_root, donor_revisions, donor_file
                    )
                )
            else:
                errors.append(f"donor case {label}: donor_file must start with docs/ or donors/<repo>/")
    return errors


def registered_runner_acceptance_pairs(runner_path: Path) -> set[tuple[str, str]]:
    tree = ast.parse(runner_path.read_text(), filename=str(runner_path))
    literal_maps: dict[str, object] = {}
    for statement in tree.body:
        if not isinstance(statement, ast.Assign) or len(statement.targets) != 1:
            continue
        target = statement.targets[0]
        if not isinstance(target, ast.Name) or target.id not in {
            "LIVE_SCENARIO_PROFILES",
            "CURRENT_ACCEPTANCE_SCENARIOS",
            "AUTONAT_ACCEPTANCE_SCENARIOS",
            "MDNS_ACCEPTANCE_SCENARIOS",
        }:
            continue
        if target.id in literal_maps:
            raise ValueError(f"runner declares {target.id} more than once")
        literal_maps[target.id] = ast.literal_eval(statement.value)

    profiles = literal_maps.get("LIVE_SCENARIO_PROFILES")
    if not isinstance(profiles, dict) or any(
        not isinstance(profile, str)
        or not isinstance(scenarios, tuple)
        or any(not isinstance(scenario, str) or not scenario for scenario in scenarios)
        for profile, scenarios in profiles.items()
    ):
        raise ValueError("LIVE_SCENARIO_PROFILES must be a literal profile-to-scenarios map")
    runner_scenario_ids = {
        f"{profile}/{scenario}"
        for profile, scenarios in profiles.items()
        for scenario in scenarios
    }

    acceptance_scenarios = literal_maps.get("CURRENT_ACCEPTANCE_SCENARIOS")
    if not isinstance(acceptance_scenarios, dict) or any(
        not isinstance(runner_scenario_id, str)
        or runner_scenario_id not in runner_scenario_ids
        or not isinstance(scenario_ids, tuple)
        or not scenario_ids
        or any(not isinstance(scenario_id, str) or not scenario_id for scenario_id in scenario_ids)
        or len(set(scenario_ids)) != len(scenario_ids)
        for runner_scenario_id, scenario_ids in acceptance_scenarios.items()
    ):
        raise ValueError(
            "CURRENT_ACCEPTANCE_SCENARIOS must be a literal, nonempty runner-scenario-to-acceptance-scenarios map"
        )
    autonat = literal_maps.get("AUTONAT_ACCEPTANCE_SCENARIOS", {})
    if autonat and autonat != {f"{value[4]}/{name}": (name,) for name, value in AUTONAT_SCENARIOS.items()}:
        raise ValueError("AutoNAT registration must cover all 12 exact role scenarios")
    mdns = literal_maps.get("MDNS_ACCEPTANCE_SCENARIOS", {})
    if mdns != {value[1]: (name,) for name, value in MDNS_SCENARIOS.items()}:
        raise ValueError("mDNS registration must cover both exact focused-suite contracts")
    return {
        (runner_scenario_id, scenario_id)
        for runner_scenario_id, scenario_ids in {**acceptance_scenarios, **autonat, **mdns}.items()
        for scenario_id in scenario_ids
    }


def registered_runner_pair_errors(
    manifest_pairs: set[tuple[str, str]], runner_pairs: set[tuple[str, str]]
) -> list[str]:
    errors: list[str] = []
    missing = sorted(runner_pairs - manifest_pairs)
    unexpected = sorted(manifest_pairs - runner_pairs)
    if missing:
        errors.append(
            "donor capabilities: registered manifest pairs are missing from the acceptance registry: "
            + ", ".join(f"{runner_scenario_id} -> {scenario_id}" for runner_scenario_id, scenario_id in missing)
        )
    if unexpected:
        errors.append(
            "donor capabilities: registered manifest pairs are not emitted by runner.py CURRENT_ACCEPTANCE_SCENARIOS: "
            + ", ".join(
                f"{runner_scenario_id} -> {scenario_id}"
                for runner_scenario_id, scenario_id in unexpected
            )
        )
    return errors


def public_surface_snapshot(
    root: Path, owner: dict[str, str]
) -> tuple[list[str], list[str], str, list[str]]:
    public_root = root / owner["path"] / "include/forge" / owner["module_root"]
    sources = sorted(public_root.glob("*.cppm"))
    headers = sorted([*public_root.glob("*.hpp"), *public_root.glob("*.h")])
    nested = sorted(
        source.relative_to(root).as_posix()
        for source in public_root.rglob("*")
        if source.is_file()
        and source.suffix in {".cppm", ".hpp", ".h"}
        and source.parent != public_root
    )
    digest = hashlib.sha256()
    modules: list[str] = []
    for source in [*sources, *headers]:
        modules.extend(
            re.findall(
                r"(?m)^(?:export\s+)?module\s+([A-Za-z0-9_.]+)\s*;",
                source.read_text(),
            )
        )
        digest.update(source.relative_to(root).as_posix().encode())
        digest.update(b"\0")
        digest.update(source.read_bytes())
        digest.update(b"\0")
    header_paths = [header.relative_to(root).as_posix() for header in headers]
    return modules, header_paths, digest.hexdigest(), nested


def main() -> int:
    if len(sys.argv) not in {5, 6}:
        print(
            "usage: check_p2p_feature_inventory.py "
            "SOURCE_ROOT INVENTORY DONOR_CAPABILITIES DONOR_CASES [DONORS_ROOT]",
            file=sys.stderr,
        )
        return 2

    root = Path(sys.argv[1]).resolve()
    inventory_path = Path(sys.argv[2]).resolve()
    capability_path = Path(sys.argv[3]).resolve()
    donor_path = Path(sys.argv[4]).resolve()
    donors_root = Path(sys.argv[5]).resolve() if len(sys.argv) == 6 and sys.argv[5] else None
    errors: list[str] = []
    try:
        inventory = json.loads(inventory_path.read_text(), object_pairs_hook=reject_duplicate_keys)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        print(f"ERROR: inventory: {error}", file=sys.stderr)
        return 1
    try:
        donor = json.loads(donor_path.read_text(), object_pairs_hook=reject_duplicate_keys)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        print(f"ERROR: donor matrix: {error}", file=sys.stderr)
        return 1
    try:
        capability_inventory = json.loads(
            capability_path.read_text(), object_pairs_hook=reject_duplicate_keys
        )
    except (OSError, json.JSONDecodeError, ValueError) as error:
        print(f"ERROR: donor capability inventory: {error}", file=sys.stderr)
        return 1
    if not isinstance(inventory, dict):
        print("ERROR: inventory: top-level value must be an object", file=sys.stderr)
        return 1
    if not isinstance(donor, dict):
        print("ERROR: donor matrix: top-level value must be an object", file=sys.stderr)
        return 1
    if not isinstance(capability_inventory, dict):
        print(
            "ERROR: donor capability inventory: top-level value must be an object",
            file=sys.stderr,
        )
        return 1

    if inventory.get("schema_version") != 1:
        errors.append("inventory: unsupported schema_version")
    if inventory.get("claim_scope") != "source_structure_and_declared_evidence_only":
        errors.append("inventory: claim_scope must not imply executed runtime evidence")
    if inventory.get("donor_capabilities") != capability_path.name:
        errors.append("inventory: donor_capabilities must reference the donor-first manifest")

    expected_states = {"live", "manual-only", "partial", "stub", "orphan", "unverified"}
    allowed_states_value = inventory.get("allowed_states", [])
    if not isinstance(allowed_states_value, list) or any(
        not isinstance(value, str) for value in allowed_states_value
    ):
        errors.append("inventory: allowed_states must be an array of strings")
        allowed_states: set[str] = set()
    else:
        allowed_states = set(allowed_states_value)
    if allowed_states != expected_states:
        errors.append("inventory: allowed_states must match the accepted hardening vocabulary")

    owners = inventory.get("owners", {})
    if not isinstance(owners, dict):
        errors.append("inventory: owners must be an object")
        owners = {}
    elif owners != REQUIRED_OWNERS:
        errors.append("inventory: owners must match the canonical P2P target/component/module mapping")
    root_cmake = (root / "CMakeLists.txt").read_text()
    package_config = (root / "cmake/ForgeConfig.cmake.in").read_text()
    for owner_id, owner in REQUIRED_OWNERS.items():
        kind = owner.get("kind", "")
        if kind in {"library", "plugin"}:
            path = owner.get("path", "")
            relative_path = Path(path)
            if (
                not path
                or relative_path.is_absolute()
                or ".." in relative_path.parts
                or not (root / relative_path).is_dir()
            ):
                errors.append(f"owner {owner_id}: path must reference a repository directory")
            for field in ("target", "component", "module_prefix", "module_root"):
                if not owner.get(field, ""):
                    errors.append(f"owner {owner_id}: missing {field}")
            cmake_path = root / relative_path / "CMakeLists.txt"
            cmake_source = cmake_path.read_text() if cmake_path.is_file() else ""
            target = owner.get("target", "")
            target_declaration = rf"add_library\s*\(\s*{re.escape(target)}(?:\s|\))"
            if not cmake_path.is_file() or not re.search(target_declaration, cmake_source):
                errors.append(f"owner {owner_id}: target is not declared by its CMakeLists.txt")
            module_root = owner.get("module_root", "")
            module_registration = (
                rf"forge_target_modules_at\s*\(\s*{re.escape(target)}\s+"
                rf"{re.escape(module_root)}\s*\)"
            )
            if target and module_root and not re.search(module_registration, cmake_source):
                errors.append(
                    f"owner {owner_id}: public modules are not registered to the canonical target/root"
                )
            component = owner.get("component", "")
            if not re.search(rf"(?m)^\s*{re.escape(component)}\s*$", root_cmake):
                errors.append(f"owner {owner_id}: component is not listed in FORGE_BUILT_COMPONENTS")
            if f'"{component}"' not in package_config:
                errors.append(f"owner {owner_id}: component is not handled by ForgeConfig.cmake.in")
            module_prefix = owner.get("module_prefix", "")
            module_sources = list((root / relative_path).glob("include/**/*.cppm"))
            declared_modules = {
                module
                for source in module_sources
                for module in re.findall(
                    r"(?m)^(?:export\s+)?module\s+([A-Za-z0-9_.]+)\s*;",
                    source.read_text(),
                )
            }
            if not declared_modules:
                errors.append(f"owner {owner_id}: no public modules were discovered")
            elif module_prefix and any(
                module != module_prefix and not module.startswith(f"{module_prefix}.")
                for module in declared_modules
            ):
                errors.append(f"owner {owner_id}: public module outside canonical module_prefix")
        elif kind not in {"external", "none"}:
            errors.append(f"owner {owner_id}: unknown kind {kind!r}")

    surface_snapshots = inventory.get("public_surface_snapshots", {})
    repository_owners = {
        owner_id for owner_id, owner in REQUIRED_OWNERS.items() if owner["kind"] in {"library", "plugin"}
    }
    if not isinstance(surface_snapshots, dict):
        errors.append("inventory: public_surface_snapshots must be an object")
        surface_snapshots = {}
    elif set(surface_snapshots) != repository_owners:
        errors.append("inventory: public_surface_snapshots must cover every repository P2P owner exactly once")

    donor_cases = donor.get("cases", [])
    if not isinstance(donor_cases, list):
        errors.append("donor matrix: cases must be an array")
        donor_cases = []
    donor_by_id = {
        case.get("id", ""): case
        for case in donor_cases
        if isinstance(case, dict) and isinstance(case.get("id", ""), str)
    }
    donor_ids = set(donor_by_id)
    if donor.get("status_scope") != "donor_case_coverage_only":
        errors.append("donor matrix: status_scope must prevent production interpretation")
    if donor.get("execution_scope") != "registered_optional_tests_not_current_results":
        errors.append("donor matrix: execution_scope must not imply current interop results")
    if donor.get("production_inventory") != inventory_path.name:
        errors.append("donor matrix: production_inventory must reference the feature inventory")
    if donor.get("capability_inventory") != capability_path.name:
        errors.append("donor matrix: capability_inventory must reference the donor-first manifest")

    if capability_inventory.get("schema_version") != 1:
        errors.append("donor capabilities: unsupported schema_version")
    if capability_inventory.get("claim_scope") != "donor_first_capability_classification_only":
        errors.append("donor capabilities: claim_scope must not imply implementation support")
    if capability_inventory.get("donor_matrix") != donor_path.name:
        errors.append("donor capabilities: donor_matrix must reference the donor case matrix")

    expected_support_requirements = {"required", "optional", "deferred", "excluded"}
    support_requirement_values = capability_inventory.get("allowed_support_requirements", [])
    if not isinstance(support_requirement_values, list) or any(
        not isinstance(value, str) for value in support_requirement_values
    ):
        errors.append("donor capabilities: allowed_support_requirements must be an array of strings")
        allowed_support_requirements: set[str] = set()
    else:
        allowed_support_requirements = set(support_requirement_values)
    if allowed_support_requirements != expected_support_requirements:
        errors.append("donor capabilities: allowed_support_requirements must match the accepted vocabulary")

    expected_default_activations = {"enabled", "opt_in", "not_applicable"}
    default_activation_values = capability_inventory.get("allowed_default_activations", [])
    if not isinstance(default_activation_values, list) or any(
        not isinstance(value, str) for value in default_activation_values
    ):
        errors.append("donor capabilities: allowed_default_activations must be an array of strings")
        allowed_default_activations: set[str] = set()
    else:
        allowed_default_activations = set(default_activation_values)
    if allowed_default_activations != expected_default_activations:
        errors.append("donor capabilities: allowed_default_activations must match the accepted vocabulary")

    expected_interop_applicability = {
        "go_and_rust",
        "go_only",
        "go_only_rust_limited",
        "rust_only_go_limited",
        "not_applicable",
    }
    interop_applicability_values = capability_inventory.get("allowed_interop_applicability", [])
    if not isinstance(interop_applicability_values, list) or any(
        not isinstance(value, str) for value in interop_applicability_values
    ):
        errors.append("donor capabilities: allowed_interop_applicability must be an array of strings")
        allowed_interop_applicability: set[str] = set()
    else:
        allowed_interop_applicability = set(interop_applicability_values)
    if allowed_interop_applicability != expected_interop_applicability:
        errors.append("donor capabilities: allowed_interop_applicability must match the accepted vocabulary")

    expected_decisions = {
        "current",
        "stage_6",
        "stage_7",
        "stage_9",
        "future_profile",
        "legacy_rejected",
        "test_only",
        "application_owned",
        "external_component",
    }
    decision_values = capability_inventory.get("allowed_decisions", [])
    if not isinstance(decision_values, list) or any(
        not isinstance(value, str) for value in decision_values
    ):
        errors.append("donor capabilities: allowed_decisions must be an array of strings")
        allowed_decisions: set[str] = set()
    else:
        allowed_decisions = set(decision_values)
    if allowed_decisions != expected_decisions:
        errors.append("donor capabilities: allowed_decisions must match the accepted roadmap vocabulary")

    profiles = capability_inventory.get("profiles", {})
    if not isinstance(profiles, dict) or not profiles:
        errors.append("donor capabilities: profiles must be a non-empty object")
        profiles = {}
    profile_capability_locks: dict[str, set[str]] = {}
    for profile_id, profile in profiles.items():
        if not isinstance(profile_id, str) or not profile_id:
            errors.append("donor capabilities: profile ids must be non-empty strings")
            continue
        if not isinstance(profile, dict) or set(profile) != {
            "description",
            "release_gate",
            "capability_ids",
        }:
            errors.append(f"donor capabilities: profile {profile_id!r} has invalid shape")
            continue
        if any(
            not isinstance(profile[field], str) or not profile[field].strip()
            for field in ("description", "release_gate")
        ):
            errors.append(f"donor capabilities: profile {profile_id!r} fields must be non-empty strings")
        locked_ids = profile.get("capability_ids", [])
        if not isinstance(locked_ids, list) or any(
            not isinstance(capability_id, str) or not capability_id.strip()
            for capability_id in locked_ids
        ):
            errors.append(
                f"donor capabilities: profile {profile_id!r} capability_ids must be strings"
            )
            locked_ids = []
        if len(set(locked_ids)) != len(locked_ids):
            errors.append(
                f"donor capabilities: profile {profile_id!r} capability_ids must be unique"
            )
        profile_capability_locks[profile_id] = set(locked_ids)

    donor_revisions = donor.get("donor_revisions", {})
    donor_revision_errors = donor_revision_schema_errors(donor_revisions)
    errors.extend(donor_revision_errors)
    if not isinstance(donor_revisions, dict):
        donor_revisions = {}
    capability_revisions = capability_inventory.get("donor_revisions", {})
    if not isinstance(capability_revisions, dict) or capability_revisions != donor_revisions:
        errors.append("donor capabilities: donor_revisions must exactly match the donor case matrix")
        capability_revisions = {}
    if donors_root is not None and not donor_revision_errors:
        errors.extend(donor_checkout_head_errors(donors_root, donor_revisions))
    errors.extend(donor_case_source_errors(root, donor_cases, donor_revisions, donors_root))

    required_capability_fields = {
        "id",
        "category",
        "profiles",
        "support_requirement",
        "default_activation",
        "interop_applicability",
        "decision",
        "forge_feature_ids",
        "donor_sources",
        "rationale",
    }
    capability_ids: set[str] = set()
    mapped_feature_coverage: Counter[str] = Counter()
    profile_coverage: Counter[str] = Counter()
    classified_profile_capabilities: dict[str, set[str]] = {
        profile_id: set() for profile_id in profiles
    }
    capabilities = capability_inventory.get("capabilities", [])
    if not isinstance(capabilities, list):
        errors.append("donor capabilities: capabilities must be an array")
        capabilities = []
    for capability in capabilities:
        if not isinstance(capability, dict):
            errors.append("donor capabilities: every capability must be an object")
            continue
        capability_id = capability.get("id", "")
        if not isinstance(capability_id, str) or not capability_id:
            errors.append("donor capabilities: capability without id")
            continue
        if capability_id in capability_ids:
            errors.append(f"donor capabilities: duplicate capability id {capability_id!r}")
        capability_ids.add(capability_id)
        missing = required_capability_fields - capability.keys()
        if missing:
            errors.append(f"donor capability {capability_id}: missing fields {sorted(missing)}")
            continue

        category = capability.get("category")
        rationale = capability.get("rationale")
        origin = capability.get("origin", "libp2p")
        support_requirement = capability.get("support_requirement")
        default_activation = capability.get("default_activation")
        interop_applicability = capability.get("interop_applicability")
        decision = capability.get("decision")
        capability_profiles = capability.get("profiles", [])
        feature_mappings = capability.get("forge_feature_ids", [])
        donor_sources = capability.get("donor_sources", [])
        if not isinstance(category, str) or not category.strip():
            errors.append(f"donor capability {capability_id}: category must be a non-empty string")
        if not isinstance(rationale, str) or not rationale.strip():
            errors.append(f"donor capability {capability_id}: rationale must be a non-empty string")
        if origin not in {"libp2p", "forge_extension"}:
            errors.append(f"donor capability {capability_id}: unknown origin {origin!r}")
        if support_requirement not in allowed_support_requirements:
            errors.append(f"donor capability {capability_id}: unknown support_requirement {support_requirement!r}")
        if default_activation not in allowed_default_activations:
            errors.append(f"donor capability {capability_id}: unknown default_activation {default_activation!r}")
        if interop_applicability not in allowed_interop_applicability:
            errors.append(
                f"donor capability {capability_id}: unknown interop_applicability "
                f"{interop_applicability!r}"
            )
        if decision not in allowed_decisions:
            errors.append(f"donor capability {capability_id}: unknown decision {decision!r}")
        if not isinstance(capability_profiles, list) or not capability_profiles or any(
            not isinstance(profile, str) or profile not in profiles for profile in capability_profiles
        ):
            errors.append(f"donor capability {capability_id}: profiles must reference known profiles")
            capability_profiles = []
        if len(set(capability_profiles)) != len(capability_profiles):
            errors.append(f"donor capability {capability_id}: profiles must be unique")
        profile_coverage.update(capability_profiles)
        for profile_id in capability_profiles:
            classified_profile_capabilities[profile_id].add(capability_id)
        if not isinstance(feature_mappings, list) or any(
            not isinstance(feature_id, str) or not feature_id.strip()
            for feature_id in feature_mappings
        ):
            errors.append(f"donor capability {capability_id}: forge_feature_ids must be strings")
            feature_mappings = []
        if len(set(feature_mappings)) != len(feature_mappings):
            errors.append(f"donor capability {capability_id}: forge_feature_ids must be unique")
        mapped_feature_coverage.update(feature_mappings)
        if not isinstance(donor_sources, list) or any(
            not isinstance(source, str) or not source.strip() for source in donor_sources
        ):
            errors.append(f"donor capability {capability_id}: donor_sources must be strings")
            donor_sources = []
        if origin == "libp2p" and not donor_sources:
            errors.append(f"donor capability {capability_id}: libp2p origin needs donor_sources")
        for source in donor_sources:
            relative = Path(source)
            if relative.is_absolute() or ".." in relative.parts:
                errors.append(f"donor capability {capability_id}: invalid donor source {source!r}")
            elif len(relative.parts) < 3 or relative.parts[0] != "donors":
                errors.append(
                    f"donor capability {capability_id}: donor source must start with donors/<repo>/"
                )
            elif relative.parts[1] not in capability_revisions:
                errors.append(
                    f"donor capability {capability_id}: donor repository is not pinned: {relative.parts[1]}"
                )
            elif donors_root is not None:
                errors.extend(
                    f"donor capability {capability_id}: {error}"
                    for error in donor_source_object_errors(
                        donors_root, capability_revisions, source
                    )
                )
        forge_sources = capability.get("forge_sources", [])
        if not isinstance(forge_sources, list) or any(
            not isinstance(source, str) or not source.strip() for source in forge_sources
        ):
            errors.append(f"donor capability {capability_id}: forge_sources must be strings")
            forge_sources = []
        if origin == "forge_extension" and not forge_sources:
            errors.append(f"donor capability {capability_id}: Forge extension needs forge_sources")
        if origin == "libp2p" and forge_sources:
            errors.append(f"donor capability {capability_id}: libp2p origin cannot use forge_sources")
        for source in forge_sources:
            relative = Path(source)
            if relative.is_absolute() or ".." in relative.parts or not (root / relative).is_file():
                errors.append(
                    f"donor capability {capability_id}: Forge source does not exist: {source}"
                )

        if decision == "current" and not feature_mappings:
            errors.append(f"donor capability {capability_id}: current decision needs a Forge feature mapping")
        if decision in {"stage_6", "stage_7", "stage_9"}:
            branch = capability.get("planned_branch")
            if not isinstance(branch, str) or not branch.startswith("forge-p2p-"):
                errors.append(f"donor capability {capability_id}: planned decision needs a P2P branch")
        elif "planned_branch" in capability:
            errors.append(f"donor capability {capability_id}: planned_branch is only valid for a staged decision")
        if support_requirement == "deferred" and decision not in {"stage_9", "future_profile"}:
            errors.append(f"donor capability {capability_id}: deferred support_requirement needs deferred decision")
        if support_requirement == "excluded" and decision not in {
            "legacy_rejected",
            "test_only",
            "application_owned",
        }:
            errors.append(f"donor capability {capability_id}: excluded support_requirement has active decision")
        if support_requirement in {"required", "optional"} and decision in {
            "legacy_rejected",
            "test_only",
            "application_owned",
        }:
            errors.append(f"donor capability {capability_id}: active support_requirement has excluded decision")
        if support_requirement in {"deferred", "excluded"} and default_activation != "not_applicable":
            errors.append(
                f"donor capability {capability_id}: inactive support_requirement cannot have a default activation"
            )
        if default_activation == "not_applicable" and support_requirement in {"required", "optional"}:
            errors.append(
                f"donor capability {capability_id}: active support_requirement needs enabled or opt_in activation"
            )
        if interop_applicability != "not_applicable" and support_requirement == "excluded":
            errors.append(
                f"donor capability {capability_id}: excluded support cannot claim donor interop applicability"
            )
        if interop_applicability == "go_only_rust_limited" and "Rust" not in rationale:
            errors.append(
                f"donor capability {capability_id}: Rust limitation must be explicit in rationale"
            )
        if interop_applicability == "rust_only_go_limited" and "Go" not in rationale:
            errors.append(
                f"donor capability {capability_id}: Go limitation must be explicit in rationale"
            )

    private_capabilities = classified_profile_capabilities.get("private_network", set())
    forbidden_private_capabilities = {
        "transport.quic_v1",
        "relay.circuit_v2_client_transport",
        "relay.autorelay_lifecycle",
        "relay.circuit_v2_service",
        "relay.dcutr",
    }
    if forbidden_private_capabilities & private_capabilities:
        errors.append(
            "donor capabilities: private_network must exclude QUIC, Relay and DCUtR "
            f"{sorted(forbidden_private_capabilities & private_capabilities)}"
        )
    if "security.private_network_psk" not in private_capabilities:
        errors.append("donor capabilities: private_network must include the transport PSK layer")
    if "reachability.private_internet_policy" not in private_capabilities:
        errors.append("donor capabilities: private_network must gate AutoNAT and UPnP behind Internet policy")
    if "connections.coordinated_dial_port_reuse" not in private_capabilities:
        errors.append("donor capabilities: private_network must retain modern coordinated dialing")
    if "connections.simultaneous_connect_legacy" in private_capabilities:
        errors.append("donor capabilities: private_network must not negotiate legacy simultaneous-connect")

    required_stage_6_opt_ins = {
        "connections.inlined_muxer_negotiation",
        "pubsub.partial_messages",
    }
    for capability in capabilities:
        if not isinstance(capability, dict) or capability.get("id") not in required_stage_6_opt_ins:
            continue
        if (
            capability.get("support_requirement") != "required"
            or capability.get("default_activation") != "opt_in"
            or capability.get("decision") != "stage_6"
        ):
            errors.append(
                f"donor capability {capability.get('id')}: Stage 6 support must be required and opt_in"
            )

    required_semantics = {
        "protocol.ping": (
            {"native", "private_network"}, "required", "enabled", "go_and_rust", "current"
        ),
        "reachability.periodic_ping_liveness": (
            {"native", "private_network"}, "required", "opt_in", "not_applicable", "stage_6"
        ),
        "reachability.autonat_v1_node_lifecycle": (
            {"native", "private_network"}, "required", "opt_in", "not_applicable", "stage_6"
        ),
        "reachability.autonat_v2_address_lifecycle": (
            {"native", "private_network"}, "required", "opt_in", "not_applicable", "stage_6"
        ),
        "reachability.private_internet_policy": (
            {"private_network"}, "required", "opt_in", "not_applicable", "stage_6"
        ),
        "discovery.mdns_public": (
            {"native"}, "optional", "opt_in", "go_and_rust", "stage_6"
        ),
        "discovery.mdns_private_fingerprinted": (
            {"private_network"}, "optional", "opt_in", "go_only_rust_limited", "stage_6"
        ),
        "security.private_network_psk": (
            {"private_network"}, "required", "enabled", "go_and_rust", "current"
        ),
        "connections.coordinated_dial_port_reuse": (
            {"native", "private_network"}, "required", "enabled", "go_and_rust", "stage_6"
        ),
        "connections.inlined_muxer_negotiation": (
            {"native", "private_network"}, "required", "opt_in", "go_only_rust_limited", "stage_6"
        ),
        "nat.upnp_mapping": (
            {"native", "private_network"}, "optional", "opt_in", "not_applicable", "stage_6"
        ),
        "relay.circuit_v2_service": (
            {"native"}, "optional", "opt_in", "go_and_rust", "stage_6"
        ),
        "dialing.ipv6_black_hole_detection": (
            {"native", "private_network"}, "required", "enabled", "not_applicable", "stage_6"
        ),
        "dialing.udp_black_hole_detection": (
            {"native"}, "required", "enabled", "not_applicable", "stage_6"
        ),
        "connections.simultaneous_connect_legacy": (
            {"legacy"}, "excluded", "not_applicable", "not_applicable", "legacy_rejected"
        ),
        "protocol.autonat_v1_client": (
            {"native", "private_network"}, "required", "opt_in", "go_and_rust", "stage_6"
        ),
        "protocol.autonat_v1_service": (
            {"native", "private_network"}, "optional", "opt_in", "go_and_rust", "stage_6"
        ),
        "protocol.autonat_v2_client": (
            {"native", "private_network"}, "required", "opt_in", "go_and_rust", "stage_6"
        ),
        "protocol.autonat_v2_service": (
            {"native", "private_network"}, "optional", "opt_in", "go_and_rust", "stage_6"
        ),
    }
    capabilities_by_id = {
        capability.get("id"): capability
        for capability in capabilities
        if isinstance(capability, dict) and isinstance(capability.get("id"), str)
    }
    for capability_id, expected in required_semantics.items():
        capability = capabilities_by_id.get(capability_id)
        if capability is None:
            errors.append(f"donor capabilities: required Stage 6 classification is missing {capability_id}")
            continue
        expected_profiles, expected_support, expected_activation, expected_interop, expected_decision = expected
        actual = (
            set(capability.get("profiles", [])),
            capability.get("support_requirement"),
            capability.get("default_activation"),
            capability.get("interop_applicability"),
            capability.get("decision"),
        )
        if actual != expected:
            errors.append(
                f"donor capability {capability_id}: semantic classification differs from the Stage 6 baseline"
            )

    stage_6_branches = (
        "forge-p2p-stage6-roadmap-v1",
        "forge-chrono-v1",
        "forge-p2p-host-protection-v1",
        "forge-crypto-xsalsa20-v1",
        "forge-p2p-private-network-v1",
        "forge-p2p-address-resolution-v1",
        "forge-p2p-reachability-v1",
        "forge-p2p-mdns-v1",
        "forge-p2p-nat-mapping-v1",
        "forge-p2p-autorelay-v1",
        "forge-p2p-path-management-v1",
        "forge-p2p-gossipsub-scoring-v1",
        "forge-p2p-gossipsub-extensions-v1",
    )
    stage_6_registry = capability_inventory.get("stage_6_pr_registry")
    if not isinstance(stage_6_registry, list):
        errors.append("donor capabilities: stage_6_pr_registry must be an array")
        stage_6_registry = []
    registry_by_branch: dict[str, dict[str, object]] = {}
    registry_owners: list[str] = []
    for index, entry in enumerate(stage_6_registry):
        if not isinstance(entry, dict) or set(entry) != {
            "ordinal",
            "branch",
            "dependencies",
            "allowed_capability_owners",
        }:
            errors.append(f"donor capabilities: Stage 6 registry entry {index} has invalid shape")
            continue
        ordinal = entry.get("ordinal")
        branch = entry.get("branch")
        dependencies = entry.get("dependencies")
        owners = entry.get("allowed_capability_owners")
        if ordinal != index or not isinstance(branch, str) or not branch:
            errors.append(f"donor capabilities: Stage 6 registry entry {index} has invalid ordinal or branch")
            continue
        if branch in registry_by_branch:
            errors.append(f"donor capabilities: duplicate Stage 6 branch {branch}")
        registry_by_branch[branch] = entry
        if not isinstance(dependencies, list) or any(
            not isinstance(dependency, str) or not dependency for dependency in dependencies
        ) or len(set(dependencies)) != len(dependencies):
            errors.append(f"donor capabilities: Stage 6 branch {branch} has invalid dependencies")
        if not isinstance(owners, list) or any(
            not isinstance(owner, str) or not owner for owner in owners
        ) or len(set(owners)) != len(owners):
            errors.append(f"donor capabilities: Stage 6 branch {branch} has invalid capability owners")
        elif owners:
            registry_owners.extend(owners)
    actual_branches = tuple(entry.get("branch") for entry in stage_6_registry if isinstance(entry, dict))
    if actual_branches != stage_6_branches:
        errors.append("donor capabilities: Stage 6 registry must have the exact approved branch set and order")
    required_stage_6_dependencies = {
        "forge-p2p-stage6-roadmap-v1": [],
        "forge-chrono-v1": ["forge-p2p-stage6-roadmap-v1"],
        "forge-p2p-host-protection-v1": ["forge-p2p-stage6-roadmap-v1"],
        "forge-crypto-xsalsa20-v1": ["forge-p2p-stage6-roadmap-v1"],
        "forge-p2p-private-network-v1": [
            "forge-chrono-v1",
            "forge-p2p-host-protection-v1",
            "forge-crypto-xsalsa20-v1",
        ],
        "forge-p2p-address-resolution-v1": [
            "forge-chrono-v1",
            "forge-p2p-host-protection-v1",
        ],
        "forge-p2p-reachability-v1": [
            "forge-chrono-v1",
            "forge-p2p-host-protection-v1",
            "forge-p2p-private-network-v1",
        ],
        "forge-p2p-mdns-v1": ["forge-chrono-v1", "forge-p2p-private-network-v1"],
        "forge-p2p-nat-mapping-v1": [
            "forge-chrono-v1",
            "forge-p2p-host-protection-v1",
            "forge-p2p-reachability-v1",
        ],
        "forge-p2p-autorelay-v1": [
            "forge-chrono-v1",
            "forge-p2p-host-protection-v1",
            "forge-p2p-reachability-v1",
        ],
        "forge-p2p-path-management-v1": [
            "forge-chrono-v1",
            "forge-p2p-host-protection-v1",
            "forge-p2p-address-resolution-v1",
            "forge-p2p-reachability-v1",
            "forge-p2p-autorelay-v1",
        ],
        "forge-p2p-gossipsub-scoring-v1": [
            "forge-chrono-v1",
            "forge-p2p-host-protection-v1",
        ],
        "forge-p2p-gossipsub-extensions-v1": [
            "forge-chrono-v1",
            "forge-p2p-gossipsub-scoring-v1",
        ],
    }
    for branch, expected_dependencies in required_stage_6_dependencies.items():
        if registry_by_branch.get(branch, {}).get("dependencies") != expected_dependencies:
            errors.append(f"donor capabilities: Stage 6 branch {branch} dependencies differ from baseline")
    for branch, entry in registry_by_branch.items():
        dependencies = entry.get("dependencies", [])
        for dependency in dependencies if isinstance(dependencies, list) else []:
            dependency_entry = registry_by_branch.get(dependency)
            if dependency_entry is None:
                errors.append(f"donor capabilities: Stage 6 branch {branch} depends on an unknown branch")
            elif dependency_entry.get("ordinal", 0) >= entry.get("ordinal", 0):
                errors.append(f"donor capabilities: Stage 6 branch {branch} dependency order is not a DAG")
    prerequisite_branches = {
        "forge-p2p-stage6-roadmap-v1",
        "forge-chrono-v1",
        "forge-crypto-xsalsa20-v1",
    }
    for branch in prerequisite_branches:
        if registry_by_branch.get(branch, {}).get("allowed_capability_owners") != []:
            errors.append(f"donor capabilities: prerequisite Stage 6 branch {branch} cannot own a capability")
    stage_6_capabilities = {
        capability_id
        for capability_id, capability in capabilities_by_id.items()
        if capability.get("decision") == "stage_6"
    }
    if len(registry_owners) != len(set(registry_owners)):
        errors.append("donor capabilities: Stage 6 capability owner has multiple PRs")
    if set(registry_owners) != stage_6_capabilities:
        errors.append(
            "donor capabilities: Stage 6 registry owners must exactly cover Stage 6 capabilities"
        )
    for capability_id in stage_6_capabilities:
        capability = capabilities_by_id[capability_id]
        branch = capability.get("planned_branch")
        if branch not in registry_by_branch:
            errors.append(f"donor capability {capability_id}: Stage 6 branch is not registered")
        elif capability_id not in registry_by_branch[branch].get("allowed_capability_owners", []):
            errors.append(f"donor capability {capability_id}: Stage 6 registry owner differs from planned_branch")

    for capability_id, capability in capabilities_by_id.items():
        if "capability_dependencies" in capability:
            errors.append(
                f"donor capability {capability_id}: global capability_dependencies are not allowed"
            )
        profile_dependencies = capability.get("profile_dependencies", {})
        capability_profiles = capability.get("profiles", [])
        if not isinstance(profile_dependencies, dict) or any(
            not isinstance(profile, str)
            or profile not in capability_profiles
            or not isinstance(dependencies, list)
            or not dependencies
            or any(
                not isinstance(dependency, str) or dependency not in capabilities_by_id
                for dependency in dependencies
            )
            or len(set(dependencies)) != len(dependencies)
            for profile, dependencies in profile_dependencies.items()
        ):
            errors.append(
                f"donor capability {capability_id}: profile_dependencies must name unique capabilities for its own profiles"
            )

    private_policy_dependents = {
        "reachability.autonat_v1_node_lifecycle",
        "reachability.autonat_v2_address_lifecycle",
        "protocol.autonat_v1_client",
        "protocol.autonat_v1_service",
        "protocol.autonat_v2_client",
        "protocol.autonat_v2_service",
        "nat.upnp_mapping",
    }
    for capability_id in private_policy_dependents:
        capability = capabilities_by_id.get(capability_id)
        if capability is None or capability.get("profile_dependencies") != {
            "private_network": ["reachability.private_internet_policy"]
        }:
            errors.append(
                f"donor capability {capability_id}: private profile must depend only on reachability.private_internet_policy"
            )

    interop_registry = capability_inventory.get("interop_acceptance_registry")
    if not isinstance(interop_registry, dict) or set(interop_registry) != {
        "artifact_schema",
        "evidence_contracts",
        "capabilities",
    }:
        errors.append("donor capabilities: interop_acceptance_registry has invalid shape")
        interop_registry = {}
    artifact_schema = interop_registry.get("artifact_schema", {})
    expected_artifact_schema = {
        "schema_version": 2,
        "claim_scope": "canonical_runner_exact_head_artifact_only",
        "required_fields": [
            "schema_version",
            "runner_argv",
            "started_at_unix",
            "finished_at_unix",
            "acceptance_manifest",
            "artifact_root",
            "fixture_provenance",
            "artifacts",
            "failures",
            "evidence_index",
        ],
        "raw_artifact_required_fields": [
            "dialer",
            "listener",
            "scenario",
            "runner_scenario_id",
            "acceptance_scenario_id",
            "profile",
            "transport_stack",
            "transport",
            "result",
            "listener_process",
            "effective_configuration",
        ],
        "evidence_index_required_fields": ["path", "size", "sha256"],
        "passing_status": "passed",
        "limited_status": "limited",
        "registration_is_not_verdict": True,
    }
    if artifact_schema != expected_artifact_schema:
        errors.append("donor capabilities: interop acceptance artifact schema must be exact")

    declared_contracts = interop_registry.get("evidence_contracts")
    if not isinstance(declared_contracts, list) or not declared_contracts or any(
        not isinstance(contract, str)
        or not contract
        or evidence_contract_for(
            contract.removeprefix(EVIDENCE_CONTRACT_PREFIX).removesuffix(EVIDENCE_CONTRACT_SUFFIX)
        ) != contract
        for contract in declared_contracts
    ) or len(set(declared_contracts)) != len(declared_contracts):
        errors.append("donor capabilities: evidence contracts must be a unique closed contract registry")
        declared_contract_set: set[str] = set()
    else:
        declared_contract_set = set(declared_contracts)

    try:
        runner_emitted_acceptance_pairs = registered_runner_acceptance_pairs(
            root / "tests/libp2p_interop/runner.py"
        )
    except (OSError, SyntaxError, ValueError) as error:
        errors.append(f"donor capabilities: cannot read registered runner acceptance scenarios: {error}")
        runner_emitted_acceptance_pairs = set()

    interop_capabilities = {
        capability_id
        for capability_id, capability in capabilities_by_id.items()
        if capability.get("interop_applicability") != "not_applicable"
    }
    acceptance_capabilities = interop_registry.get("capabilities", {})
    if not isinstance(acceptance_capabilities, dict) or set(acceptance_capabilities) != interop_capabilities:
        errors.append("donor capabilities: acceptance registry must cover interoperable capabilities exactly")
        acceptance_capabilities = {}

    expected_directions = {
        "go_and_rust": {"forge_to_go", "go_to_forge", "forge_to_rust", "rust_to_forge"},
        "go_only": {"forge_to_go", "go_to_forge"},
        "go_only_rust_limited": {"forge_to_go", "go_to_forge"},
        "rust_only_go_limited": {"forge_to_rust", "rust_to_forge"},
    }
    manifest_registered_pairs: set[tuple[str, str]] = set()
    limitation_implementation = {
        "go_only_rust_limited": "rust",
        "rust_only_go_limited": "go",
    }
    allowed_profile_transport_stacks = {
        "native": {("quic",), ("tcp", "yamux")},
        "private_network": {("tcp", "pnet", "yamux")},
    }
    seen_scenario_ids: set[str] = set()
    seen_evidence_contracts: set[str] = set()
    registered_evidence_contracts: set[str] = set()
    for capability_id, acceptance in acceptance_capabilities.items():
        capability = capabilities_by_id.get(capability_id, {})
        applicability = capability.get("interop_applicability")
        allowed_fields = {"scenarios"}
        if applicability in limitation_implementation:
            allowed_fields.add("limitation")
        if not isinstance(acceptance, dict) or set(acceptance) != allowed_fields:
            errors.append(f"donor capability {capability_id}: acceptance registry has invalid shape")
            continue
        sources = capability.get("donor_sources", [])
        required_prefixes = {
            "go_and_rust": ("donors/go-", "donors/rust-"),
            "go_only": ("donors/go-",),
            "go_only_rust_limited": ("donors/go-",),
            "rust_only_go_limited": ("donors/rust-",),
        }.get(applicability, ())
        if not isinstance(sources, list) or any(
            not any(isinstance(source, str) and source.startswith(prefix) for source in sources)
            for prefix in required_prefixes
        ):
            errors.append(f"donor capability {capability_id}: pinned donor sources do not match interop applicability")

        scenarios = acceptance.get("scenarios")
        if not isinstance(scenarios, list) or not scenarios:
            errors.append(f"donor capability {capability_id}: acceptance scenarios must be a non-empty array")
            continue
        expected_primary_directions = expected_directions.get(applicability, set())
        # Correct subject-role attribution, not reduced bilateral coverage:
        # the separate AutoNAT suite validator still requires all 41 cases.
        if capability_id in {value[0] for value in AUTONAT_SCENARIOS.values()}:
            expected_primary_directions = ROLE_DIRECTIONS[capability_id.rsplit("_", 1)[1]]
        has_primary_scenario = False
        has_registered_scenario = False
        for scenario in scenarios:
            if not isinstance(scenario, dict):
                errors.append(f"donor capability {capability_id}: acceptance scenario must be an object")
                continue
            registration = scenario.get("registration")
            allowed_scenario_fields = {
                "id",
                "runner_scenario_id",
                "profile",
                "transport_stack",
                "activation",
                "registration",
                "evidence_contract",
                "required_directions",
                "expected_status",
            }
            if registration == "registered":
                allowed_scenario_fields.add("source_case_id")
            if "requires_capabilities" in scenario:
                allowed_scenario_fields.add("requires_capabilities")
            if set(scenario) != allowed_scenario_fields:
                errors.append(f"donor capability {capability_id}: acceptance scenario has invalid shape")
                continue
            scenario_id = scenario.get("id")
            profile = scenario.get("profile")
            transport_stack = scenario.get("transport_stack")
            activation = scenario.get("activation")
            directions = scenario.get("required_directions")
            expected_status = scenario.get("expected_status")
            required_capabilities = scenario.get("requires_capabilities", [])
            evidence_contract = scenario.get("evidence_contract")
            if not isinstance(scenario_id, str) or not scenario_id or scenario_id in seen_scenario_ids:
                errors.append(f"donor capability {capability_id}: acceptance scenario id must be globally unique")
            elif scenario_id:
                seen_scenario_ids.add(scenario_id)
            if (
                not isinstance(scenario_id, str)
                or not scenario_id
                or evidence_contract != evidence_contract_for(scenario_id)
                or evidence_contract not in declared_contract_set
                or registration not in {"registered", "planned"}
                or (registration == "registered" and evidence_contract not in (set(EVIDENCE_CONTRACT_VALIDATORS) | AUTONAT_EVIDENCE_CONTRACTS | MDNS_EVIDENCE_CONTRACTS))
                or (registration == "planned" and evidence_contract in (set(EVIDENCE_CONTRACT_VALIDATORS) | AUTONAT_EVIDENCE_CONTRACTS | MDNS_EVIDENCE_CONTRACTS))
                or evidence_contract in seen_evidence_contracts
            ):
                errors.append(
                    f"donor capability {capability_id}: acceptance evidence contract must be exact, registered and unique"
                )
            elif isinstance(evidence_contract, str):
                seen_evidence_contracts.add(evidence_contract)
                if registration == "registered":
                    registered_evidence_contracts.add(evidence_contract)
            stack = tuple(transport_stack) if isinstance(transport_stack, list) else ()
            if (
                not isinstance(profile, str)
                or profile not in capability.get("profiles", [])
                or stack not in allowed_profile_transport_stacks.get(profile, set())
                or activation != "enabled"
                or not isinstance(evidence_contract, str)
                or expected_launcher_transport(profile, stack, evidence_contract) is None
            ):
                errors.append(
                    f"donor capability {capability_id}: acceptance profile, transport stack, contract or activation is invalid"
                )
            if not isinstance(directions, list) or any(
                not isinstance(direction, str) or direction not in {
                    "forge_to_go", "go_to_forge", "forge_to_rust", "rust_to_forge"
                }
                for direction in directions
            ) or len(set(directions)) != len(directions):
                errors.append(f"donor capability {capability_id}: acceptance directions are invalid")
                directions = []
            if not isinstance(required_capabilities, list) or any(
                not isinstance(required, str) or not required for required in required_capabilities
            ) or len(set(required_capabilities)) != len(required_capabilities):
                errors.append(f"donor capability {capability_id}: acceptance capability requirements are invalid")
            if expected_status not in {"passed", "limited"}:
                errors.append(f"donor capability {capability_id}: acceptance status is invalid")
            runner_scenario_id = scenario.get("runner_scenario_id")
            if not isinstance(runner_scenario_id, str) or not runner_scenario_id or "/" not in runner_scenario_id:
                errors.append(f"donor capability {capability_id}: acceptance runner scenario id is invalid")
            if expected_status == "passed" and set(directions) == expected_primary_directions:
                has_primary_scenario = True
            if scenario_id in AUTONAT_SCENARIOS and (
                capability_id != AUTONAT_SCENARIOS[scenario_id][0]
                or set(directions) != ROLE_DIRECTIONS[AUTONAT_SCENARIOS[scenario_id][1]]
            ):
                errors.append(f"donor capability {capability_id}: AutoNAT directions must match the actual Forge role")
            if registration == "registered":
                has_registered_scenario = True
                if isinstance(runner_scenario_id, str) and isinstance(scenario_id, str):
                    manifest_registered_pairs.add((runner_scenario_id, scenario_id))
                # Registration establishes an executable contract, not a live
                # verdict. Only the new paired suite may register while staged.
                if capability.get("decision") != "current" and not (
                    capability.get("decision") == "stage_6" and scenario_id in (set(AUTONAT_SCENARIOS) | set(MDNS_SCENARIOS))
                ):
                    errors.append(
                        f"donor capability {capability_id}: staged scenario cannot claim current runner registration"
                    )
                source_case_id = scenario.get("source_case_id")
                source_case = donor_by_id.get(source_case_id)
                if (
                    not isinstance(runner_scenario_id, str)
                    or not isinstance(scenario_id, str)
                    or (runner_scenario_id, scenario_id) not in runner_emitted_acceptance_pairs
                ):
                    errors.append(f"donor capability {capability_id}: current scenario is not registered by runner.py")
                if not isinstance(source_case_id, str) or not has_registered_live_interop(source_case):
                    errors.append(f"donor capability {capability_id}: current scenario lacks a registered donor case")
                else:
                    selector_ids = {
                        f"{selector.get('profile')}/{selector.get('scenario')}"
                        for selector in source_case.get("forge_live_scenario", [])
                        if isinstance(selector, dict)
                    }
                    if runner_scenario_id not in selector_ids:
                        errors.append(
                            f"donor capability {capability_id}: donor case does not register its runner scenario"
                        )
                    if any(
                        not donor_case_has_source(source_case, prefix) for prefix in required_prefixes
                    ):
                        errors.append(
                            f"donor capability {capability_id}: donor case does not match required donor implementations"
                        )
            elif registration == "planned":
                if not isinstance(required_capabilities, list) or any(
                    not isinstance(required, str)
                    or (
                        capabilities_by_id.get(required, {}).get("decision") != "stage_6"
                        and not (
                            required == "security.private_network_psk"
                            and capabilities_by_id.get(required, {}).get("decision") == "current"
                        )
                    )
                    for required in required_capabilities
                ) or len(set(required_capabilities)) != len(required_capabilities):
                    errors.append(
                        f"donor capability {capability_id}: planned scenario has invalid Stage 6 prerequisites"
                    )
                if capability.get("decision") == "current" and (
                    profile != "private_network"
                    or required_capabilities != ["security.private_network_psk"]
                ):
                    errors.append(
                        f"donor capability {capability_id}: current planned scenario must be the private PSK comprehensive gate"
                    )
            else:
                errors.append(
                    f"donor capability {capability_id}: acceptance registration must be registered or planned"
                )
            if (
                profile == "private_network"
                and capability_id != "security.private_network_psk"
                and required_capabilities[:1] != ["security.private_network_psk"]
            ):
                errors.append(
                    f"donor capability {capability_id}: private acceptance must require security.private_network_psk"
                )
            if (
                profile == "private_network"
                and isinstance(scenario_id, str)
                and scenario_id.startswith("autonat_")
                and required_capabilities != [
                    "security.private_network_psk", "reachability.private_internet_policy"
                ]
            ):
                errors.append(
                    f"donor capability {capability_id}: private AutoNAT acceptance must require PSK and private Internet policy"
                )
        if not has_primary_scenario:
            errors.append(f"donor capability {capability_id}: acceptance lacks its primary directions")
        if capability.get("decision") == "current" and not has_registered_scenario:
            errors.append(f"donor capability {capability_id}: current acceptance needs a registered runner scenario")

        if applicability in limitation_implementation:
            limitation = acceptance.get("limitation")
            expected_implementation = limitation_implementation[applicability]
            if not isinstance(limitation, dict) or set(limitation) != {
                "implementation", "source_case_id", "description"
            }:
                errors.append(f"donor capability {capability_id}: explicit implementation limitation is required")
                continue
            limitation_case = donor_by_id.get(limitation.get("source_case_id"))
            limitation_text = donor_case_text(limitation_case)
            description = limitation.get("description")
            # Rust is either the documented fallback or the only supported donor.
            limitation_evidence_prefix = "donors/rust-"
            if (
                limitation.get("implementation") != expected_implementation
                or not isinstance(description, str)
                or expected_implementation not in description.lower()
                or not donor_case_has_source(limitation_case, limitation_evidence_prefix)
                or expected_implementation not in limitation_text
                or not any(term in limitation_text for term in ("limitation", "fallback", "no official"))
            ):
                errors.append(f"donor capability {capability_id}: explicit {expected_implementation} limitation is invalid")

    errors.extend(
        registered_runner_pair_errors(manifest_registered_pairs, runner_emitted_acceptance_pairs)
    )

    if declared_contract_set != seen_evidence_contracts:
        errors.append("donor capabilities: evidence contract registry must cover acceptance scenarios exactly")
    if registered_evidence_contracts != set(EVIDENCE_CONTRACT_VALIDATORS) | AUTONAT_EVIDENCE_CONTRACTS | MDNS_EVIDENCE_CONTRACTS:
        errors.append(
            "donor capabilities: executable validator registry must match registered evidence contracts exactly"
        )

    required_stage_6_scenarios = {
        "protocol.autonat_v1_client": {
            "autonat_v1_client",
            "autonat_v1_client_native_tcp_yamux",
            "autonat_v1_client_private_tcp_yamux_pnet",
        },
        "protocol.autonat_v1_service": {
            "autonat_v1_service",
            "autonat_v1_service_native_tcp_yamux",
            "autonat_v1_service_private_tcp_yamux_pnet",
        },
        "protocol.autonat_v2_client": {
            "autonat_v2_client",
            "autonat_v2_client_native_tcp_yamux",
            "autonat_v2_client_private_tcp_yamux_pnet",
        },
        "protocol.autonat_v2_service": {
            "autonat_v2_service",
            "autonat_v2_service_native_tcp_yamux",
            "autonat_v2_service_private_tcp_yamux_pnet",
        },
        "relay.circuit_v2_service": {"relay_v2_service"},
        "relay.dcutr": {"dcutr"},
        "pubsub.gossipsub_v1_0_v1_1": {
            "gossipsub_v1_0_fallback",
            "gossipsub_v1_1",
            "gossipsub_v1_0_fallback_native_tcp_yamux",
            "gossipsub_v1_1_native_tcp_yamux",
            "gossipsub_v1_0_fallback_private_tcp_yamux_pnet",
            "gossipsub_v1_1_private_tcp_yamux_pnet",
        },
        "discovery.mdns_public": {"mdns_public"},
        "addressing.dnsaddr": {"dnsaddr", "dnsaddr_private_tcp_yamux_pnet"},
        "security.private_network_psk": {"pnet"},
        "pubsub.gossipsub_v1_2": {
            "gossipsub_v1_2",
            "gossipsub_v1_2_native_tcp_yamux",
            "gossipsub_v1_2_private_tcp_yamux_pnet",
        },
        "pubsub.gossipsub_v1_3": {
            "gossipsub_v1_3",
            "gossipsub_v1_3_native_tcp_yamux",
            "gossipsub_v1_3_private_tcp_yamux_pnet",
        },
        "pubsub.partial_messages": {
            "partial_messages",
            "partial_messages_native_tcp_yamux",
            "partial_messages_private_tcp_yamux_pnet",
        },
        "connections.inlined_muxer_negotiation": {
            "inline_muxer_go_noise",
            "inline_muxer_go_tls",
            "inline_muxer_go_noise_private_pnet",
            "inline_muxer_go_tls_private_pnet",
            "inline_muxer_rust_noise_fallback",
            "inline_muxer_rust_tls_fixed_alpn_fallback",
            "inline_muxer_rust_noise_fallback_private_pnet",
            "inline_muxer_rust_tls_fixed_alpn_fallback_private_pnet",
        },
    }
    for capability_id, expected_ids in required_stage_6_scenarios.items():
        scenarios = acceptance_capabilities.get(capability_id, {}).get("scenarios", [])
        actual_ids = {
            scenario.get("id") for scenario in scenarios if isinstance(scenario, dict)
        }
        if actual_ids != expected_ids:
            errors.append(f"donor capability {capability_id}: acceptance scenario ids differ from Stage 6 baseline")

    required_private_pnet_scenarios = {
        "routing.kademlia_amino": {
            "id": "kademlia_amino_private_tcp_yamux_pnet",
            "directions": {"forge_to_go", "go_to_forge", "forge_to_rust", "rust_to_forge"},
        },
        "discovery.rendezvous": {
            "id": "rendezvous_rust_private_tcp_yamux_pnet",
            "directions": {"forge_to_rust", "rust_to_forge"},
        },
    }
    for capability_id, expected in required_private_pnet_scenarios.items():
        scenarios = acceptance_capabilities.get(capability_id, {}).get("scenarios", [])
        matching = [
            scenario for scenario in scenarios
            if isinstance(scenario, dict) and scenario.get("id") == expected["id"]
        ]
        if len(matching) != 1:
            errors.append(f"donor capability {capability_id}: private TCP/Yamux+pnet acceptance scenario is required")
            continue
        scenario = matching[0]
        if (
            scenario.get("profile") != "private_network"
            or scenario.get("transport_stack") != ["tcp", "pnet", "yamux"]
            or scenario.get("activation") != "enabled"
            or scenario.get("registration") != "planned"
            or scenario.get("requires_capabilities") != ["security.private_network_psk"]
            or set(scenario.get("required_directions", [])) != expected["directions"]
            or scenario.get("runner_scenario_id") != f"private_tcp_yamux_pnet/{expected['id']}"
        ):
            errors.append(f"donor capability {capability_id}: private TCP/Yamux+pnet scenario is incomplete")

    inlined_muxer = acceptance_capabilities.get("connections.inlined_muxer_negotiation", {})
    inlined_scenarios = inlined_muxer.get("scenarios", []) if isinstance(inlined_muxer, dict) else []
    inlined_shape = {
        (scenario.get("id"), tuple(scenario.get("required_directions", [])), scenario.get("expected_status"))
        for scenario in inlined_scenarios
        if isinstance(scenario, dict)
    }
    if inlined_shape != {
        ("inline_muxer_go_noise", ("forge_to_go", "go_to_forge"), "passed"),
        ("inline_muxer_go_tls", ("forge_to_go", "go_to_forge"), "passed"),
        ("inline_muxer_go_noise_private_pnet", ("forge_to_go", "go_to_forge"), "passed"),
        ("inline_muxer_go_tls_private_pnet", ("forge_to_go", "go_to_forge"), "passed"),
        ("inline_muxer_rust_noise_fallback", ("forge_to_rust", "rust_to_forge"), "limited"),
        ("inline_muxer_rust_tls_fixed_alpn_fallback", ("forge_to_rust", "rust_to_forge"), "limited"),
        ("inline_muxer_rust_noise_fallback_private_pnet", ("forge_to_rust", "rust_to_forge"), "limited"),
        ("inline_muxer_rust_tls_fixed_alpn_fallback_private_pnet", ("forge_to_rust", "rust_to_forge"), "limited"),
    }:
        errors.append("donor capability connections.inlined_muxer_negotiation: Go inline and Rust fallback scenarios are required")

    noise_tls_sources = capabilities_by_id.get("security.noise_tls_identity", {}).get("donor_sources", [])
    required_noise_tls_sources = {
        "donors/go-libp2p/p2p/security/noise/transport.go",
        "donors/go-libp2p/p2p/security/tls/transport.go",
        "donors/rust-libp2p/transports/noise/src/io/handshake.rs",
        "donors/rust-libp2p/transports/tls/src/lib.rs",
    }
    if not isinstance(noise_tls_sources, list) or not required_noise_tls_sources <= set(noise_tls_sources):
        errors.append("donor capability security.noise_tls_identity: exact Noise and TLS donor paths are required")

    inlined_muxer_sources = capabilities_by_id.get(
        "connections.inlined_muxer_negotiation", {}
    ).get("donor_sources", [])
    required_inlined_muxer_sources = {
        "donors/go-libp2p/p2p/security/noise/transport.go",
        "donors/go-libp2p/p2p/security/tls/transport.go",
        "donors/rust-libp2p/transports/noise/src/io/handshake.rs",
        "donors/rust-libp2p/transports/tls/src/lib.rs",
    }
    if not isinstance(inlined_muxer_sources, list) or not required_inlined_muxer_sources <= set(
        inlined_muxer_sources
    ):
        errors.append(
            "donor capability connections.inlined_muxer_negotiation: Go TLS/Noise and Rust ALPN/extensions donors are required"
        )

    coordinated_dial_id = "connections.coordinated_dial_port_reuse"
    required_coordinated_dial_sources = {
        "donors/libp2p-specs/connections/simopen.md",
        "donors/go-libp2p/p2p/net/swarm/swarm_dial.go",
        "donors/go-libp2p/p2p/net/swarm/dial_worker.go",
        "donors/go-libp2p/p2p/transport/tcp/tcp.go",
        "donors/rust-libp2p/swarm/src/connection/pool.rs",
        "donors/rust-libp2p/swarm/src/dial_opts.rs",
        "donors/rust-libp2p/transports/tcp/src/lib.rs",
    }
    coordinated_dial_sources = capabilities_by_id.get(coordinated_dial_id, {}).get(
        "donor_sources", []
    )
    coordinated_dial_case_sources = donor_by_id.get(coordinated_dial_id, {}).get(
        "donor_file", []
    )
    if any(
        not isinstance(sources, list)
        or any(not isinstance(source, str) for source in sources)
        or len(sources) != len(set(sources))
        or set(sources) != required_coordinated_dial_sources
        for sources in (coordinated_dial_sources, coordinated_dial_case_sources)
    ):
        errors.append(
            "coordinated dial donor trace must exactly match the required spec, Go swarm/TCP and Rust pool/DialOpts/TCP sources in both capability and donor case"
        )

    gossipsub_v13 = capabilities_by_id.get("pubsub.gossipsub_v1_3", {})
    v13_rationale = gossipsub_v13.get("rationale", "")
    if not isinstance(v13_rationale, str) or not all(
        phrase in v13_rationale.lower()
        for phrase in ("first-rpc", "unknown", "capability matching")
    ):
        errors.append("donor capability pubsub.gossipsub_v1_3: v1.3 first-RPC advertisement semantics are required")
    v13_sources = gossipsub_v13.get("donor_sources", [])
    required_v13_sources = {
        "donors/go-libp2p-pubsub/extensions.go",
        "donors/rust-libp2p/protocols/gossipsub/src/behaviour.rs",
    }
    if not isinstance(v13_sources, list) or not required_v13_sources <= set(v13_sources):
        errors.append(
            "donor capability pubsub.gossipsub_v1_3: Go first-RPC and Rust advertisement donors are required"
        )

    host_local_policy_ids = {
        "reachability.private_internet_policy",
        "reachability.periodic_ping_liveness",
        "reachability.autonat_v1_node_lifecycle",
        "reachability.autonat_v2_address_lifecycle",
        "security.connection_gater",
        "resource.memory_fd_transient_service_scopes",
        "dialing.happy_eyeballs",
        "dialing.ipv6_black_hole_detection",
        "dialing.udp_black_hole_detection",
        "events.host_state",
        "nat.upnp_mapping",
        "reachability.observed_address_manager",
        "relay.autorelay_lifecycle",
    }
    for capability_id in host_local_policy_ids:
        capability = capabilities_by_id.get(capability_id)
        if capability is None:
            errors.append(f"donor capabilities: host-local policy classification is missing {capability_id}")
        elif capability.get("interop_applicability") != "not_applicable":
            errors.append(
                f"donor capability {capability_id}: host-local orchestration cannot claim bilateral interop"
            )

    forge_policy_extensions = {
        "reachability.private_internet_policy",
    }
    for capability_id in forge_policy_extensions:
        capability = capabilities_by_id.get(capability_id)
        if (
            capability is None
            or capability.get("origin") != "forge_extension"
            or capability.get("forge_sources")
            != ["docs/iterations/forge-p2p-production-implementation-v1.md"]
        ):
            errors.append(
                f"donor capability {capability_id}: must remain a Forge extension with its design source"
            )

    errors.extend(private_mdns_attribution_errors(
        capabilities_by_id.get("discovery.mdns_private_fingerprinted", {})
    ))

    gossipsub_branch_owners = {
        "pubsub.gossipsub_v1_0_v1_1": "forge-p2p-gossipsub-scoring-v1",
        "pubsub.gossipsub_v1_2": "forge-p2p-gossipsub-extensions-v1",
        "pubsub.gossipsub_v1_3": "forge-p2p-gossipsub-extensions-v1",
        "pubsub.partial_messages": "forge-p2p-gossipsub-extensions-v1",
    }
    for capability_id, expected_branch in gossipsub_branch_owners.items():
        capability = capabilities_by_id.get(capability_id)
        if capability is None:
            errors.append(f"donor capabilities: GossipSub branch owner is missing {capability_id}")
        elif capability.get("planned_branch") != expected_branch:
            errors.append(
                f"donor capability {capability_id}: expected GossipSub owner {expected_branch}"
            )

    missing_profiles = set(profiles) - set(profile_coverage)
    if missing_profiles:
        errors.append(f"donor capabilities: profiles without capabilities {sorted(missing_profiles)}")
    for profile_id, locked_ids in profile_capability_locks.items():
        classified_ids = classified_profile_capabilities.get(profile_id, set())
        if locked_ids != classified_ids:
            errors.append(
                f"donor capabilities: profile {profile_id!r} scope lock differs; "
                f"missing {sorted(locked_ids - classified_ids)}, "
                f"unlocked {sorted(classified_ids - locked_ids)}"
            )
    duplicate_feature_mappings = sorted(
        feature_id for feature_id, count in mapped_feature_coverage.items() if count != 1
    )
    if duplicate_feature_mappings:
        errors.append(
            "donor capabilities: Forge feature mappings must be owned exactly once "
            f"{duplicate_feature_mappings}"
        )
    required_feature_ids = set(mapped_feature_coverage)

    feature_ids: set[str] = set()
    builtin_coverage: Counter[str] = Counter()
    capability_coverage: Counter[str] = Counter()
    negotiated_protocol_coverage: Counter[str] = Counter()
    public_component_coverage: Counter[str] = Counter()
    test_manifest = (root / "tests/CMakeLists.txt").read_text()
    registered_tests = set(
        re.findall(
            r"add_(?:executable|custom_target)\s*\(\s*([A-Za-z0-9_]+)",
            test_manifest,
            re.DOTALL,
        )
    )
    registered_tests.update(
        re.findall(r"add_test\s*\(\s*NAME\s+([A-Za-z0-9_]+)", test_manifest, re.DOTALL)
    )

    features = inventory.get("features", [])
    if not isinstance(features, list):
        errors.append("inventory: features must be an array")
        features = []
    for feature in features:
        if not isinstance(feature, dict):
            errors.append("inventory: every feature must be an object")
            continue
        feature_id = feature.get("id", "")
        if not isinstance(feature_id, str) or not feature_id:
            errors.append("feature without id")
            continue
        if feature_id in feature_ids:
            errors.append(f"{feature_id}: duplicate feature id")
        feature_ids.add(feature_id)

        missing = REQUIRED_FIELDS - feature.keys()
        if missing:
            errors.append(f"{feature_id}: missing fields {sorted(missing)}")
            continue

        string_values: dict[str, str] = {}
        for field in (
            "owner",
            "normal_activation",
            "configuration",
            "resource_ownership",
            "persistence",
            "maintenance",
            "diagnostics",
            "intended_disposition",
        ):
            value = feature[field]
            if not isinstance(value, str) or not value.strip():
                errors.append(f"{feature_id}: {field} must be a non-empty string")
                string_values[field] = ""
            else:
                string_values[field] = value.strip()

        state_value = feature["state"]
        state = state_value if isinstance(state_value, str) else ""
        if not state:
            errors.append(f"{feature_id}: state must be a non-empty string")
        owner = string_values["owner"]
        if state not in allowed_states:
            errors.append(f"{feature_id}: unknown state {state!r}")
        if owner not in REQUIRED_OWNERS:
            errors.append(f"{feature_id}: unknown owner {owner!r}")

        if state == "orphan" and owner != "none":
            errors.append(f"{feature_id}: orphan feature must have owner 'none'")
        if state != "orphan" and owner == "none":
            errors.append(f"{feature_id}: owner 'none' requires orphan state")
        if state == "manual-only" and "explicit" not in string_values["normal_activation"]:
            errors.append(f"{feature_id}: manual-only feature must identify explicit activation")
        if state == "stub" and not any(
            action in string_values["intended_disposition"] for action in ("replace", "reject", "remove")
        ):
            errors.append(f"{feature_id}: stub must be replaced, rejected or removed")

        list_values: dict[str, list[str]] = {}
        for field in ("builtin_protocols", "capabilities", "negotiated_protocol_ids", "public_components"):
            value = feature.get(field, [])
            if not isinstance(value, list) or any(
                not isinstance(item, str) or not item.strip() for item in value
            ):
                errors.append(f"{feature_id}: {field} must be an array of non-empty strings")
                list_values[field] = []
            else:
                list_values[field] = value

        evidence = feature["evidence"]
        if not isinstance(evidence, dict):
            errors.append(f"{feature_id}: evidence must be an object")
            continue

        evidence_values: dict[str, list[str]] = {}
        for field in ("layers", "source_paths", "tests", "donor_sources", "donor_cases"):
            value = evidence.get(field, [])
            if not isinstance(value, list) or any(
                not isinstance(item, str) or not item.strip() for item in value
            ):
                errors.append(f"{feature_id}: evidence.{field} must be an array of non-empty strings")
                evidence_values[field] = []
            else:
                evidence_values[field] = value

        layers = set(evidence_values["layers"])
        unknown_layers = layers - EVIDENCE_LAYERS
        if unknown_layers:
            errors.append(f"{feature_id}: unknown evidence layers {sorted(unknown_layers)}")
        if state == "live":
            required_live = EVIDENCE_LAYERS
            if not required_live <= layers:
                errors.append(
                    f"{feature_id}: live claim lacks evidence layers {sorted(required_live - layers)}"
                )
            if string_values["diagnostics"].lower() in {"none", "unavailable"}:
                errors.append(f"{feature_id}: live claim lacks diagnostics")

        source_paths = evidence_values["source_paths"]
        tests = evidence_values["tests"]
        donor_sources = evidence_values["donor_sources"]
        case_ids = evidence_values["donor_cases"]
        if not source_paths:
            errors.append(f"{feature_id}: evidence must list source_paths")
        if not tests:
            errors.append(f"{feature_id}: evidence must list tests")
        if not donor_sources:
            errors.append(f"{feature_id}: evidence must list donor_sources")
        if state == "live" and not case_ids:
            errors.append(f"{feature_id}: live claim must reference donor cases")
        if state == "live" and not any(path.startswith("plugins/") for path in source_paths):
            errors.append(f"{feature_id}: live claim must reference its official-plugin path")
        if state == "live" and not any(test.startswith("test_forge_plugins") for test in tests):
            errors.append(f"{feature_id}: live claim must reference an official-plugin test target")
        if state == "live" and not any(
            has_registered_live_interop(donor_by_id.get(case_id)) for case_id in case_ids
        ):
            errors.append(f"{feature_id}: live claim must reference a donor case with registered live interop")

        for relative in source_paths + donor_sources:
            if Path(relative).is_absolute() or ".." in Path(relative).parts:
                errors.append(f"{feature_id}: evidence path must be repository-relative: {relative}")
            elif not (root / relative).is_file():
                errors.append(f"{feature_id}: evidence path does not exist: {relative}")

        for case_id in case_ids:
            if case_id not in donor_ids:
                errors.append(f"{feature_id}: unknown donor case {case_id!r}")
        for test in tests:
            if test not in registered_tests:
                errors.append(f"{feature_id}: unknown test target {test!r}")

        builtin_coverage.update(list_values["builtin_protocols"])
        capability_coverage.update(list_values["capabilities"])
        negotiated_protocol_coverage.update(list_values["negotiated_protocol_ids"])
        public_component_coverage.update(list_values["public_components"])

    missing_features = required_feature_ids - feature_ids
    unknown_features = feature_ids - required_feature_ids
    if missing_features:
        errors.append(f"inventory: missing required features {sorted(missing_features)}")
    if unknown_features:
        errors.append(f"inventory: features lack donor-capability ownership {sorted(unknown_features)}")

    expected_surface_features: dict[str, list[str]] = {owner: [] for owner in repository_owners}
    for feature in features:
        if not isinstance(feature, dict) or not isinstance(feature.get("id"), str):
            continue
        owner = feature.get("owner")
        surface_owner = owner if owner in repository_owners else "net.p2p.node"
        expected_surface_features[surface_owner].append(feature["id"])
    for owner_id in sorted(repository_owners):
        snapshot = surface_snapshots.get(owner_id, {})
        if not isinstance(snapshot, dict):
            errors.append(f"public surface {owner_id}: snapshot must be an object")
            continue
        owner = REQUIRED_OWNERS[owner_id]
        modules, headers, digest, nested = public_surface_snapshot(root, owner)
        if nested:
            errors.append(f"public surface {owner_id}: nested public source files are forbidden {nested}")
        if snapshot.get("owner") != owner_id:
            errors.append(f"public surface {owner_id}: owner must be exact")
        if snapshot.get("module_count") != len(modules):
            errors.append(f"public surface {owner_id}: public module count changed")
        if snapshot.get("modules") != modules:
            errors.append(f"public surface {owner_id}: public module inventory changed")
        if snapshot.get("headers") != headers:
            errors.append(f"public surface {owner_id}: public macro-header inventory changed")
        if snapshot.get("sha256") != digest:
            errors.append(
                f"public surface {owner_id}: declarations changed; update feature classification deliberately"
            )
        snapshot_features = snapshot.get("feature_ids", [])
        if not isinstance(snapshot_features, list) or any(
            not isinstance(feature_id, str) or not feature_id.strip()
            for feature_id in snapshot_features
        ):
            errors.append(f"public surface {owner_id}: feature_ids must be non-empty strings")
        elif sorted(snapshot_features) != sorted(expected_surface_features[owner_id]):
            errors.append(f"public surface {owner_id}: feature_ids do not match owned public surface")
        module_features = snapshot.get("module_features", {})
        if not isinstance(module_features, dict) or set(module_features) != set(modules):
            errors.append(f"public surface {owner_id}: every module needs exact feature classification")
            module_features = {}
        classified_features: set[str] = set()
        for module, classified in module_features.items():
            if not isinstance(classified, list) or not classified or any(
                not isinstance(feature_id, str) or feature_id not in feature_ids
                for feature_id in classified
            ):
                errors.append(f"public surface {owner_id}: invalid feature classification for {module}")
                continue
            classified_features.update(classified)
        header_features = snapshot.get("header_features", {})
        if not isinstance(header_features, dict) or set(header_features) != set(headers):
            errors.append(f"public surface {owner_id}: every macro header needs exact feature classification")
            header_features = {}
        for header, classified in header_features.items():
            if not isinstance(classified, list) or not classified or any(
                not isinstance(feature_id, str) or feature_id not in feature_ids
                for feature_id in classified
            ):
                errors.append(f"public surface {owner_id}: invalid feature classification for {header}")
                continue
            classified_features.update(classified)
        if classified_features != set(expected_surface_features[owner_id]):
            errors.append(f"public surface {owner_id}: module/header classifications miss owned features")

    protocol_path = root / "libraries/net/p2p/include/forge/net/p2p/protocol.cppm"
    protocol_source = protocol_path.read_text()
    declared_builtins = extract_namespace_names(
        protocol_source,
        "builtins",
        r"inline const protocol_id\s+([a-zA-Z0-9_]+)",
    )
    declared_capabilities = extract_namespace_names(
        protocol_source,
        "capabilities",
        r"inline constexpr std::uint64_t\s+([a-zA-Z0-9_]+)",
    )
    builtins_match = re.search(r"namespace builtins\s*\{(?P<body>.*?)\n\}", protocol_source, re.DOTALL)
    builtin_values = (
        set(re.findall(r'\.value\s*=\s*"([^"]+)"', builtins_match.group("body"))) if builtins_match else set()
    )
    p2p_sources = list((root / "libraries/net/p2p").glob("*.cpp"))
    p2p_sources.extend((root / "libraries/net/p2p/include").glob("**/*.cppm"))
    p2p_sources.extend((root / "plugins/p2p").glob("**/*.cpp"))
    p2p_sources.extend((root / "plugins/p2p").glob("**/*.cppm"))
    protocol_literals = {
        value
        for source in p2p_sources
        for value in re.findall(
            r'protocol_id(?:\s+[A-Za-z0-9_]+)?\s*\{\s*\.value\s*=\s*"([^"]+)"',
            source.read_text(),
        )
        if value.startswith("/")
    }
    declared_negotiated_protocols = protocol_literals - builtin_values
    public_components = {
        component
        for source in (root / "libraries/net/p2p/include").glob("**/*.cppm")
        for component in re.findall(
            r"(?m)^class\s+([A-Za-z_][A-Za-z0-9_]*::[A-Za-z_][A-Za-z0-9_]*)\s*[{:]",
            source.read_text(),
        )
    }

    plugin_sources = "\n".join(
        source.read_text() for source in (root / "plugins/p2p/node").glob("*.cpp")
    )
    if "start_maintenance" in plugin_sources or "async_refresh_discovery" in plugin_sources:
        errors.append("plugin.p2p.node: network maintenance must be owned by forge_net_p2p")
    node_lifecycle_source = (root / "libraries/net/p2p/node_impl_lifecycle.cpp").read_text()
    bootstrap_source = (root / "libraries/net/p2p/bootstrap_service.cpp").read_text()
    if "bootstrap_service" not in node_lifecycle_source or "start_maintenance" not in bootstrap_source:
        errors.append("net.p2p.node: bootstrap maintenance ownership is not structurally discoverable")

    for kind, declared, coverage in (
        ("built-in protocol", declared_builtins, builtin_coverage),
        ("capability", declared_capabilities, capability_coverage),
        ("negotiated protocol", declared_negotiated_protocols, negotiated_protocol_coverage),
        ("public nested component", public_components, public_component_coverage),
    ):
        unknown = set(coverage) - declared
        missing = declared - set(coverage)
        duplicates = sorted(name for name, count in coverage.items() if count != 1)
        if unknown:
            errors.append(f"inventory: unknown {kind}s {sorted(unknown)}")
        if missing:
            errors.append(f"inventory: missing {kind}s {sorted(missing)}")
        if duplicates:
            errors.append(f"inventory: {kind}s must be owned exactly once {duplicates}")

    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(
        "P2P source inventory valid (source-only; no live interop execution verdict): "
        f"{len(capability_ids)} classified capabilities, "
        f"{len(feature_ids)} implementation features, "
        f"{len(declared_builtins)} built-in protocols, "
        f"{len(declared_capabilities)} capabilities, "
        f"{len(declared_negotiated_protocols)} negotiated protocols, "
        f"{len(public_components)} public nested components"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
