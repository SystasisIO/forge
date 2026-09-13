#!/usr/bin/env python3
import hashlib
import ipaddress
import json
import math
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Optional

from dns_evidence import DNSADDR_SCENARIOS, validate_dnsaddr
from provider_evidence import validate_provider_evidence
from autonat_acceptance import (
    EVIDENCE_CONTRACTS as AUTONAT_EVIDENCE_CONTRACTS,
    ROLE_DIRECTIONS as AUTONAT_ROLE_DIRECTIONS,
    SCENARIOS as AUTONAT_SCENARIOS,
    validate_suite as validate_autonat_suite,
)

from provenance import (
    FIXTURE_DONOR_DIRECTORIES,
    fixture_donor_checkout_errors,
    fixture_donor_revision_bindings,
    load_canonical_donor_revisions,
    worktree_identity,
)
from stage6_evidence_contract import (
    EVIDENCE_CONTRACT_PREFIX,
    EVIDENCE_CONTRACT_SUFFIX,
    evidence_contract_for,
)


ARTIFACT_SCHEMA = {
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

CANONICAL_RUNNER = Path("tests/libp2p_interop/runner.py")
DIRECTIONS = {"forge_to_go", "go_to_forge", "forge_to_rust", "rust_to_forge"}
SHA256 = re.compile(r"[0-9a-f]{64}")
PROFILE_TRANSPORT_STACKS = {
    "native": {("quic",), ("tcp", "yamux")},
    "private_network": {("tcp", "pnet", "yamux")},
}
RUNNER_FLAGS = (
    "--enabled",
    "--forge-fixture",
    "--source-dir",
    "--build-dir",
    "--forge-root",
    "--donors-root",
    "--acceptance-manifest",
)
ENABLED_VALUES = {"1", "ON", "on", "true", "TRUE", "yes", "YES"}
PRIVATE_NETWORK_TRANSPORT = "tcp-pnet"
PRIVATE_NETWORK_PSK_DEPENDENCY = "security.private_network_psk"
PNET_FINGERPRINT_DOMAIN = b"forge.net.pnet.operational-fingerprint.v1\0"
PNET_FORBIDDEN_RUNTIME_FLAGS = (
    "autonat_v2_active",
    "relay_service_active",
    "relay_client_active",
    "dcutr_active",
)
RELAY_NATIVE_PEER_ID_IDENTITY_CODE = 0
RELAY_NATIVE_PEER_ID_SHA256_CODE = 0x12
RELAY_NATIVE_PEER_ID_MAX_DIGEST_BYTES = 64
RELAY_NATIVE_PEER_ID_MAX_IDENTITY_DIGEST_BYTES = 42
# The largest pinned Rust PeerId is 0x12, 0x40 and a 64-byte SHA2-256 digest;
# its canonical base58btc form is at most 90 characters, not 91.
RELAY_NATIVE_PEER_ID_MAX_BASE58BTC_LENGTH = 90
RELAY_NATIVE_BASE58BTC_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
RELAY_NATIVE_BASE58BTC_VALUES = {
    character: index for index, character in enumerate(RELAY_NATIVE_BASE58BTC_ALPHABET)
}
RELAY_FIXTURE_RELAY_PEER_ID = "QmcgpsyWgH8Y8ajJz1Cu72KnS5uo2Aa2LpzU7kinSupNKC"
RELAY_FIXTURE_CLIENT_PEER_ID = "QmNLfbof5rLekrACjeuLk9JmGZD2HDBHCU4z16iYKmx5SE"


TLS_EVIDENCE_CONTRACTS = {
    evidence_contract_for("tls_identity"),
    evidence_contract_for("inline_muxer_go_tls"),
    evidence_contract_for("inline_muxer_rust_tls_fixed_alpn_fallback"),
}


def expected_launcher_transport(profile: str, stack: tuple[str, ...], evidence_contract: str) -> Optional[str]:
    """Map the manifest's profile contract to the only permitted fixture transport."""
    if profile == "native" and stack == ("quic",):
        return "quic"
    if profile == "native" and stack == ("tcp", "yamux"):
        return "tcp-tls" if evidence_contract in TLS_EVIDENCE_CONTRACTS else "tcp"
    if profile == "private_network" and stack == ("tcp", "pnet", "yamux"):
        return PRIVATE_NETWORK_TRANSPORT
    return None


def reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def reject_nonfinite_json_constant(value: str) -> object:
    raise ValueError(f"non-finite JSON constant is not permitted: {value}")


def parse_finite_json_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed):
        raise ValueError(f"non-finite JSON number is not permitted: {value}")
    return parsed


def load_json(path: Path) -> object:
    return json.loads(
        path.read_text(),
        object_pairs_hook=reject_duplicate_keys,
        parse_constant=reject_nonfinite_json_constant,
        parse_float=parse_finite_json_float,
    )


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def required_scenarios(
    manifest: object,
    suite: str = "stage6",
) -> tuple[
    dict[tuple[str, str], tuple[set[str], str, str, tuple[str, ...], str, tuple[str, ...], str]],
    list[str],
]:
    errors: list[str] = []
    if suite not in ("stage6", "autonat"):
        return {}, ["unknown acceptance suite"]
    if not isinstance(manifest, dict):
        return {}, ["manifest must be a JSON object"]
    registry = manifest.get("interop_acceptance_registry")
    if not isinstance(registry, dict) or set(registry) != {
        "artifact_schema", "evidence_contracts", "capabilities"
    }:
        return {}, ["manifest interop_acceptance_registry has invalid shape"]
    if registry.get("artifact_schema") != ARTIFACT_SCHEMA:
        return {}, ["manifest artifact schema differs from the accepted schema"]
    declared_contracts = registry.get("evidence_contracts")
    if not isinstance(declared_contracts, list) or not declared_contracts or any(
        not isinstance(contract, str)
        or evidence_contract_for(contract.removeprefix(EVIDENCE_CONTRACT_PREFIX).removesuffix(EVIDENCE_CONTRACT_SUFFIX))
        != contract
        for contract in declared_contracts
    ) or len(set(declared_contracts)) != len(declared_contracts):
        return {}, ["manifest evidence contracts must be a unique closed registry"]
    declared_contract_set = set(declared_contracts)
    capabilities = registry.get("capabilities")
    if not isinstance(capabilities, dict) or not capabilities:
        return {}, ["manifest acceptance capabilities must be a non-empty object"]

    required: dict[tuple[str, str], tuple[set[str], str, str, tuple[str, ...], str, tuple[str, ...], str]] = {}
    referenced_contracts: set[str] = set()
    for capability_id, entry in capabilities.items():
        if not isinstance(capability_id, str) or not capability_id:
            errors.append("manifest acceptance capability id is invalid")
            continue
        if not isinstance(entry, dict):
            errors.append(f"manifest {capability_id}: acceptance entry must be an object")
            continue
        scenarios = entry.get("scenarios")
        if not isinstance(scenarios, list) or not scenarios:
            errors.append(f"manifest {capability_id}: scenarios must be a non-empty array")
            continue
        has_limitation = isinstance(entry.get("limitation"), dict)
        for scenario in scenarios:
            if not isinstance(scenario, dict):
                errors.append(f"manifest {capability_id}: scenario must be an object")
                continue
            scenario_id = scenario.get("id")
            runner_scenario_id = scenario.get("runner_scenario_id")
            profile = scenario.get("profile")
            transport_stack = scenario.get("transport_stack")
            activation = scenario.get("activation")
            directions = scenario.get("required_directions")
            status = scenario.get("expected_status")
            requires = scenario.get("requires_capabilities", [])
            registration = scenario.get("registration")
            evidence_contract = scenario.get("evidence_contract")
            stack = tuple(transport_stack) if isinstance(transport_stack, list) else ()
            selected = suite == "stage6" or (isinstance(scenario_id, str) and scenario_id in AUTONAT_SCENARIOS)
            if selected and registration != "registered":
                errors.append(
                    f"manifest {capability_id}/{scenario_id}: non-registered scenario is promotion-blocking "
                    "until its implementing PR registers an executable validator"
                )
            if (
                not isinstance(scenario_id, str)
                or not scenario_id
                or not isinstance(runner_scenario_id, str)
                or "/" not in runner_scenario_id
                or not isinstance(profile, str)
                or stack not in PROFILE_TRANSPORT_STACKS.get(profile, set())
                or activation != "enabled"
                or not isinstance(directions, list)
                or not directions
                or any(not isinstance(direction, str) or direction not in DIRECTIONS for direction in directions)
                or len(set(directions)) != len(directions)
                or not isinstance(requires, list)
                or any(not isinstance(capability, str) or not capability for capability in requires)
                or len(set(requires)) != len(requires)
                or not isinstance(evidence_contract, str)
                or evidence_contract != evidence_contract_for(scenario_id)
                or evidence_contract not in declared_contract_set
                or registration not in {"registered", "planned"}
                or (
                    profile == "private_network"
                    and capability_id != PRIVATE_NETWORK_PSK_DEPENDENCY
                    and PRIVATE_NETWORK_PSK_DEPENDENCY not in requires
                )
                or status not in {ARTIFACT_SCHEMA["passing_status"], ARTIFACT_SCHEMA["limited_status"]}
                or (status == ARTIFACT_SCHEMA["limited_status"] and not has_limitation)
            ):
                errors.append(f"manifest {capability_id}: scenario is invalid")
                continue
            key = (capability_id, scenario_id)
            if key in required or evidence_contract in referenced_contracts:
                errors.append(f"manifest {capability_id}: duplicate acceptance scenario {scenario_id}")
            else:
                referenced_contracts.add(evidence_contract)
                if registration == "registered" and evidence_contract not in (
                    set(EVIDENCE_CONTRACT_VALIDATORS) | AUTONAT_EVIDENCE_CONTRACTS
                ):
                    errors.append(
                        f"manifest {capability_id}/{scenario_id}: registered scenario has no executable validator"
                    )
                elif registration == "registered" and selected:
                    required[key] = (
                        set(directions), status, profile, stack, runner_scenario_id, tuple(requires), evidence_contract
                    )
    if declared_contract_set != referenced_contracts:
        errors.append("manifest evidence contract registry does not cover acceptance scenarios exactly")
    autonat_required = {key: value for key, value in required.items() if key[1] in AUTONAT_SCENARIOS}
    if suite == "autonat" or autonat_required:
        if {name for _, name in autonat_required} != set(AUTONAT_SCENARIOS):
            errors.append("AutoNAT suite requires all 12 registered role contracts")
        for (capability, name), value in autonat_required.items():
            owner, role, _, transport, runner_profile = AUTONAT_SCENARIOS[name]
            profile = "private_network" if transport == "tcp-pnet" else "native"
            stack = ("quic",) if transport == "quic" else (("tcp", "pnet", "yamux") if transport == "tcp-pnet" else ("tcp", "yamux"))
            dependencies = ("security.private_network_psk", "reachability.private_internet_policy") if transport == "tcp-pnet" else ()
            if capability != owner or value != (
                AUTONAT_ROLE_DIRECTIONS[role], "passed", profile, stack, f"{runner_profile}/{name}",
                dependencies, evidence_contract_for(name),
            ):
                errors.append(f"AutoNAT {name}: role directions or exact profile contract mismatch")
    return required, errors


def git_output(root: Path, *args: str) -> tuple[Optional[str], Optional[str]]:
    result = subprocess.run(
        ["git", "-C", str(root), *args], text=True, capture_output=True, check=False
    )
    if result.returncode != 0:
        return None, result.stderr.strip() or "git command failed"
    return result.stdout.strip(), None


def validate_git_state(root: Path, expected_head: str) -> tuple[Optional[float], list[str]]:
    errors: list[str] = []
    head, error = git_output(root, "rev-parse", "HEAD")
    if error is not None or head is None:
        return None, [f"cannot read checked-out git HEAD: {error}"]
    if head != expected_head:
        errors.append("checked-out git HEAD does not match the expected exact HEAD")
    status, error = git_output(root, "status", "--porcelain", "--untracked-files=no")
    if error is not None:
        errors.append(f"cannot read tracked git status: {error}")
    elif status:
        errors.append("checked-out tracked tree is dirty")
    timestamp, error = git_output(root, "show", "-s", "--format=%ct", "HEAD")
    if error is not None or timestamp is None:
        errors.append(f"cannot read git commit timestamp: {error}")
        return None, errors
    try:
        return float(timestamp), errors
    except ValueError:
        return None, [*errors, "git commit timestamp is invalid"]


def is_timestamp(value: object) -> bool:
    return type(value) is int or (type(value) is float and math.isfinite(value))


def absolute_path(value: object) -> Optional[Path]:
    if not isinstance(value, str) or not value:
        return None
    path = Path(value)
    return path.resolve() if path.is_absolute() else None


def validate_runner_inputs(root: Path, artifact_path: Path, artifact_root: Path, manifest_path: Path,
                           provenance: object, suite: str = "stage6") -> tuple[dict[str, Path], list[str]]:
    if not isinstance(provenance, dict):
        return {}, ["artifact fixture_provenance must be an object"]
    inputs = provenance.get("runner_inputs")
    path_keys = {
        "source_dir", "build_dir", "forge_root", "donors_root", "acceptance_manifest"
    }
    if (not isinstance(inputs, dict) or set(inputs) not in (path_keys, path_keys | {"suite"})
            or inputs.get("suite", "stage6") != suite or suite not in ("stage6", "autonat")):
        return {}, ["artifact runner input provenance has invalid schema"]
    paths = {key: absolute_path(inputs.get(key)) for key in path_keys}
    if any(path is None for path in paths.values()):
        return {}, ["artifact runner input provenance contains a non-absolute path"]
    resolved = {key: path for key, path in paths.items() if path is not None}
    build_dir = resolved["build_dir"]
    if (
        resolved["source_dir"] != (root / CANONICAL_RUNNER).parent.resolve()
        or resolved["forge_root"] != root.resolve()
        or resolved["acceptance_manifest"] != manifest_path.resolve()
        or not resolved["donors_root"].is_dir()
        or artifact_root != build_dir / ("autonat-run" if suite == "autonat" else "interop-run")
        or artifact_path.resolve() != build_dir / ("autonat-artifacts.json" if suite == "autonat" else "interop-artifacts.json")
    ):
        return {}, ["artifact runner input provenance does not bind canonical roots and artifact paths"]
    return resolved, []


def validate_donor_provenance(root: Path, provenance: object, inputs: dict[str, Path]) -> list[str]:
    """Bind execution evidence to canonical donor commits, not equivalent source trees."""
    if not isinstance(provenance, dict):
        return ["artifact fixture_provenance must be an object"]
    try:
        canonical_revisions = load_canonical_donor_revisions(
            root / CANONICAL_RUNNER.parent / "donor_cases.json"
        )
        fixture_lock = load_json(root / CANONICAL_RUNNER.parent / "fixture-lock.json")
    except (OSError, RuntimeError, ValueError) as error:
        return [f"cannot load canonical donor provenance: {error}"]
    expected_donors = fixture_lock.get("donors") if isinstance(fixture_lock, dict) else None
    donors = provenance.get("donors")
    if donors != expected_donors:
        return ["artifact fixture donors do not match the locked donor set"]
    bindings, errors = fixture_donor_revision_bindings(donors, canonical_revisions)
    if errors:
        return errors
    if provenance.get("donor_revisions") != bindings:
        return ["artifact donor revisions do not match canonical fixture donor revisions"]
    donors_root = inputs.get("donors_root")
    if donors_root is None:
        return ["artifact donor provenance has no canonical donors root"]
    return fixture_donor_checkout_errors(donors_root, donors, canonical_revisions)


def validate_runner_argv(root: Path, argv: object, manifest_path: Path, inputs: dict[str, Path],
                         binary_paths: dict[str, Path], suite: str = "stage6") -> list[str]:
    flags = RUNNER_FLAGS + (("--suite",) if suite == "autonat" else ())
    if not isinstance(argv, list) or len(argv) != 2 + 2 * len(flags) or any(
        not isinstance(argument, str) or not argument for argument in argv
    ):
        return ["artifact runner_argv must record the complete canonical runner invocation"]
    if Path(argv[0]).resolve() != Path(sys.executable).resolve():
        return ["artifact runner_argv executable differs from the current resolved sys.executable"]
    runner_path = Path(argv[1])
    resolved_runner = runner_path.resolve() if runner_path.is_absolute() else (root / runner_path).resolve()
    if resolved_runner != (root / CANONICAL_RUNNER).resolve() or not resolved_runner.is_file():
        return ["artifact runner argv does not execute the canonical runner under source root"]
    if tuple(argv[2::2]) != flags:
        return ["artifact runner argv flags differ from the canonical live runner mode"]
    values = dict(zip(flags, argv[3::2]))
    if suite == "autonat" and values["--suite"] != suite:
        return ["artifact runner argv suite differs from canonical inputs"]
    if values["--enabled"] not in ENABLED_VALUES:
        return ["artifact runner argv does not prove an enabled live execution"]
    expected = {
        "--forge-fixture": binary_paths.get("forge"),
        "--source-dir": inputs.get("source_dir"),
        "--build-dir": inputs.get("build_dir"),
        "--forge-root": root.resolve(),
        "--donors-root": inputs.get("donors_root"),
        "--acceptance-manifest": manifest_path.resolve(),
    }
    for flag, expected_path in expected.items():
        if expected_path is None or absolute_path(values[flag]) != expected_path:
            return [f"artifact runner argv {flag} differs from canonical runner inputs"]
    return []


def validate_identity(value: object, expected_head: str, label: str) -> tuple[Optional[dict], list[str]]:
    if not isinstance(value, dict) or set(value) != {
        "head", "worktree_sha256", "dirty", "exact_identity"
    }:
        return None, [f"{label} has invalid worktree identity schema"]
    head = value.get("head")
    fingerprint = value.get("worktree_sha256")
    dirty = value.get("dirty")
    exact_identity = value.get("exact_identity")
    if (
        head != expected_head
        or not isinstance(fingerprint, str)
        or SHA256.fullmatch(fingerprint) is None
        or dirty is not False
        or exact_identity != f"git:{expected_head};worktree-sha256:{fingerprint}"
    ):
        return None, [f"{label} does not bind the clean expected worktree identity"]
    return value, []


def validate_fixture_provenance(value: object, expected_head: str, current_identity: dict) -> list[str]:
    if not isinstance(value, dict):
        return ["artifact fixture_provenance must be an object"]
    worktree = value.get("forge_worktree")
    if not isinstance(worktree, dict) or set(worktree) != {"start", "end", "changed_during_run"}:
        return ["artifact forge_worktree provenance has invalid schema"]
    start, errors = validate_identity(worktree.get("start"), expected_head, "artifact start worktree")
    end, end_errors = validate_identity(worktree.get("end"), expected_head, "artifact end worktree")
    errors.extend(end_errors)
    if worktree.get("changed_during_run") is not False:
        errors.append("artifact worktree changed_during_run must be false")
    if start is not None and end is not None and start != end:
        errors.append("artifact start and end worktree identities differ")
    if start is not None and start != current_identity:
        errors.append("artifact start worktree identity differs from the checked-out worktree")
    if end is not None and end != current_identity:
        errors.append("artifact end worktree identity differs from the checked-out worktree")
    build_info = value.get("fixture_build_info")
    if not isinstance(build_info, dict) or build_info.get("schema_version") != 2:
        errors.append("artifact fixture build_info provenance is missing")
        return errors
    forge = build_info.get("forge")
    compiler = build_info.get("compiler")
    build_profile = build_info.get("build_profile")
    if start is not None and forge != start:
        errors.append("artifact fixture build_info does not bind the start worktree identity")
    if forge != current_identity:
        errors.append("artifact fixture build_info differs from the checked-out worktree identity")
    if not isinstance(compiler, dict) or any(
        not isinstance(compiler.get(field), str) or not compiler[field]
        for field in ("path", "id", "version")
    ) or not isinstance(build_profile, str) or not build_profile:
        errors.append("artifact fixture build_info compiler or build profile is incomplete")
    return errors


def validate_execution_provenance(value: object, expected_head: str,
                                  current_identity: dict) -> tuple[dict[str, Path], list[str]]:
    errors = validate_fixture_provenance(value, expected_head, current_identity)
    if not isinstance(value, dict):
        return {}, errors
    tools = value.get("tools")
    python_tool = tools.get("python") if isinstance(tools, dict) else None
    expected_python = Path(sys.executable).resolve()
    if not isinstance(python_tool, dict) or set(python_tool) != {"path", "version_output"}:
        errors.append("artifact Python tool provenance has invalid schema")
    elif absolute_path(python_tool.get("path")) != expected_python:
        errors.append("artifact Python tool provenance differs from current sys.executable")
    else:
        try:
            current_version = subprocess.check_output([str(expected_python), "--version"], text=True).strip()
        except OSError as error:
            errors.append(f"cannot read current Python tool provenance: {error}")
        else:
            if python_tool.get("version_output") != current_version:
                errors.append("artifact Python tool version provenance differs from current sys.executable")

    binaries = value.get("binaries")
    if not isinstance(binaries, dict) or set(binaries) != {"forge", "go", "rust"}:
        errors.append("artifact binary provenance must cover forge, go and rust exactly")
        return {}, errors
    paths: dict[str, Path] = {}
    for implementation, binary in binaries.items():
        path = absolute_path(binary.get("path")) if isinstance(binary, dict) else None
        digest = binary.get("sha256") if isinstance(binary, dict) else None
        if (
            path is None
            or not path.is_file()
            or not isinstance(digest, str)
            or SHA256.fullmatch(digest) is None
            or sha256_file(path) != digest
        ):
            errors.append(f"artifact {implementation} binary path or SHA-256 provenance is invalid")
        else:
            paths[implementation] = path
    return paths, errors


def raw_evidence_paths(value: object) -> set[Path]:
    paths: set[Path] = set()
    if isinstance(value, dict):
        for key, nested in value.items():
            if key in {"log_file", "result_file", "listener_result_file", "evidence_file"} and isinstance(nested, str):
                paths.add(Path(nested))
            paths.update(raw_evidence_paths(nested))
    elif isinstance(value, list):
        for nested in value:
            paths.update(raw_evidence_paths(nested))
    return paths


def validate_evidence_index(
    artifact_path: Path, artifact_root: Path, artifacts: list[object], index: object,
    empty_process_logs: frozenset[Path] = frozenset(),
) -> tuple[dict[Path, str], list[str]]:
    errors: list[str] = []
    if not isinstance(index, list) or not index:
        return {}, ["artifact evidence_index must be a non-empty array"]
    indexed: dict[Path, str] = {}
    for entry in index:
        if not isinstance(entry, dict) or set(entry) != set(ARTIFACT_SCHEMA["evidence_index_required_fields"]):
            errors.append("artifact evidence index entry has invalid schema")
            continue
        relative = entry.get("path")
        size = entry.get("size")
        expected_hash = entry.get("sha256")
        path = Path(relative) if isinstance(relative, str) else Path()
        if (
            not isinstance(relative, str)
            or not relative
            or path.is_absolute()
            or ".." in path.parts
            or path == Path(".")
            or type(size) is not int
            or size < 0
            or not isinstance(expected_hash, str)
            or SHA256.fullmatch(expected_hash) is None
        ):
            errors.append("artifact evidence index path, size or SHA-256 is invalid")
            continue
        resolved = (artifact_root / path).resolve()
        # A silent, joined fixture may have an empty stdout log, never an empty
        # JSON proof. The AutoNAT branch supplies only owned process log paths.
        if size == 0 and resolved not in empty_process_logs:
            errors.append("artifact evidence index has an empty non-process proof")
            continue
        try:
            resolved.relative_to(artifact_root)
        except ValueError:
            errors.append("artifact evidence index path escapes artifact_root")
            continue
        if resolved == artifact_path.resolve() or resolved in indexed:
            errors.append("artifact evidence index reuses a proof path")
            continue
        if not resolved.is_file() or resolved.stat().st_size != size or sha256_file(resolved) != expected_hash:
            errors.append(f"artifact evidence index hash or size differs: {relative}")
            continue
        indexed[resolved] = relative

    references: set[Path] = set()
    for reference in raw_evidence_paths(artifacts):
        resolved = reference.resolve()
        try:
            resolved.relative_to(artifact_root)
        except ValueError:
            errors.append(f"raw runner evidence escapes artifact_root: {reference}")
            continue
        references.add(resolved)
    if references != set(indexed):
        errors.append("artifact evidence_index does not exactly cover raw runner logs and results")
    return indexed, errors


def raw_result_payloads(value: object):
    if isinstance(value, dict):
        if isinstance(value.get("result_file"), str):
            yield value
        for nested in value.values():
            yield from raw_result_payloads(nested)
    elif isinstance(value, list):
        for nested in value:
            yield from raw_result_payloads(nested)


def validate_all_result_evidence(artifacts: list[object], indexed_evidence: dict[Path, str],
                                 artifact_root: Path) -> list[str]:
    errors: list[str] = []
    for index, record in enumerate(artifacts):
        if not isinstance(record, dict):
            continue
        for result in raw_result_payloads(record):
            result_path = path_within(result["result_file"], artifact_root)
            if result_path is None or result_path not in indexed_evidence:
                errors.append(f"raw runner record {index} result evidence is absent from the verified index")
            else:
                payload, payload_errors = load_evidence_json(result_path, f"raw runner record {index} result file")
                errors.extend(payload_errors)
                expected = {key: value for key, value in result.items() if key not in {"result_file", "attempts"}}
                if payload is not None and payload != expected:
                    errors.append(f"raw runner record {index} result file does not match its raw payload")
        listener_file = record.get("listener_result_file")
        if listener_file is not None:
            listener_path = path_within(listener_file, artifact_root)
            if listener_path is None or listener_path not in indexed_evidence:
                errors.append(f"raw runner record {index} listener result evidence is absent from the verified index")
            else:
                payload, payload_errors = load_evidence_json(
                    listener_path, f"raw runner record {index} listener result file"
                )
                errors.extend(payload_errors)
                if payload is not None and payload != record.get("listener_result"):
                    errors.append(f"raw runner record {index} listener result file does not match its raw payload")
    return errors


def direction_of(record: dict) -> Optional[str]:
    dialer = record.get("dialer")
    listener = record.get("listener")
    if not isinstance(dialer, str) or not isinstance(listener, str):
        return None
    direction = f"{dialer}_to_{listener}"
    return direction if direction in DIRECTIONS else None


def command_options(command: object, action: str) -> tuple[dict[str, str], list[str]]:
    if not isinstance(command, list) or len(command) < 2 or any(
        not isinstance(value, str) or not value for value in command
    ):
        return {}, ["command is not a non-empty string array"]
    if command[1] != action or (len(command) - 2) % 2:
        return {}, [f"command is not a {action} command with flag/value pairs"]
    options: dict[str, str] = {}
    for index in range(2, len(command), 2):
        flag, value = command[index], command[index + 1]
        if not flag.startswith("--") or flag in options:
            return {}, ["command has a duplicate or malformed option"]
        options[flag] = value
    return options, []


def path_within(path: object, root: Path) -> Optional[Path]:
    resolved = absolute_path(path)
    if resolved is None:
        return None
    try:
        resolved.relative_to(root)
    except ValueError:
        return None
    return resolved


def failure_text(value: object) -> bool:
    if not isinstance(value, dict):
        return False
    return any(
        isinstance(value.get(key), str) and value[key].strip()
        for key in ("error", "failure", "failure_class", "timeout_class")
    )


def load_evidence_json(path: Path, label: str) -> tuple[Optional[dict], list[str]]:
    try:
        payload = load_json(path)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        return None, [f"{label} is not valid JSON: {error}"]
    if not isinstance(payload, dict):
        return None, [f"{label} must be a JSON object"]
    if failure_text(payload):
        return None, [f"{label} contains contradictory failure text"]
    return payload, []


def positive_integer(value: object) -> bool:
    return type(value) is int and value > 0


def nonempty_string(value: object) -> bool:
    return isinstance(value, str) and bool(value)


def relay_native_base58btc_encode(value: bytes) -> str:
    """Encode bounded relay-native PeerId bytes in canonical base58btc form."""
    leading_zeroes = len(value) - len(value.lstrip(b"\0"))
    number = int.from_bytes(value, "big")
    digits: list[str] = []
    while number:
        number, remainder = divmod(number, 58)
        digits.append(RELAY_NATIVE_BASE58BTC_ALPHABET[remainder])
    return "1" * leading_zeroes + "".join(reversed(digits))


def relay_native_base58btc_decode(value: object) -> Optional[bytes]:
    """Decode the base58btc form accepted by the pinned Rust relay fixture."""
    if not nonempty_string(value) or len(value) > RELAY_NATIVE_PEER_ID_MAX_BASE58BTC_LENGTH:
        return None
    number = 0
    for character in value:
        digit = RELAY_NATIVE_BASE58BTC_VALUES.get(character)
        if digit is None:
            return None
        number = number * 58 + digit
    encoded = number.to_bytes((number.bit_length() + 7) // 8, "big") if number else b""
    leading_zeroes = len(value) - len(value.lstrip("1"))
    decoded = b"\0" * leading_zeroes + encoded
    return decoded if relay_native_base58btc_encode(decoded) == value else None


def relay_native_canonical_varint(data: bytes, offset: int) -> Optional[tuple[int, int]]:
    """Decode the unsigned canonical LEB128 values in a relay-native multihash."""
    value = 0
    shift = 0
    for index in range(offset, len(data)):
        byte = data[index]
        payload = byte & 0x7F
        if shift >= 64 or (shift == 63 and payload > 1):
            return None
        value |= payload << shift
        if not byte & 0x80:
            encoded = bytearray()
            remainder = value
            while True:
                next_byte = remainder & 0x7F
                remainder >>= 7
                encoded.append(next_byte | (0x80 if remainder else 0))
                if not remainder:
                    break
            if data[offset:index + 1] != bytes(encoded):
                return None
            return value, index + 1
        shift += 7
    return None


def relay_native_peer_id(value: object) -> bool:
    """Match pinned rust-libp2p PeerId parsing without claiming general multihash support."""
    multihash = relay_native_base58btc_decode(value)
    if multihash is None:
        return False
    return relay_native_multihash_is_peer_id(multihash)


def relay_native_multihash_is_peer_id(multihash: bytes) -> bool:
    """Apply pinned Rust PeerId rules to already-bounded relay-native multihash bytes."""
    code = relay_native_canonical_varint(multihash, 0)
    if code is None:
        return False
    length = relay_native_canonical_varint(multihash, code[1])
    if length is None:
        return False
    digest = multihash[length[1]:]
    if length[0] != len(digest) or len(digest) > RELAY_NATIVE_PEER_ID_MAX_DIGEST_BYTES:
        return False
    if code[0] == RELAY_NATIVE_PEER_ID_SHA256_CODE:
        return True
    return (
        code[0] == RELAY_NATIVE_PEER_ID_IDENTITY_CODE
        and len(digest) <= RELAY_NATIVE_PEER_ID_MAX_IDENTITY_DIGEST_BYTES
    )


def relay_native_quic_listener_endpoint(
    value: object,
) -> tuple[Optional[str], Optional[str], list[str]]:
    """Parse only Rust Relay's native QUIC listener grammar, not a general multiaddr."""
    if not nonempty_string(value):
        return None, None, ["Rust relay endpoint is not a non-empty native QUIC listener address"]
    components = value.split("/")
    if (
        len(components) not in {6, 8}
        or components[0] != ""
        or components[1] not in {"ip4", "ip6"}
        or components[3] != "udp"
        or components[5] != "quic-v1"
        or (len(components) == 8 and components[6] != "p2p")
    ):
        return None, None, ["Rust relay endpoint is not a native QUIC listener address"]

    address = components[2]
    try:
        parsed_address = ipaddress.ip_address(address)
    except ValueError:
        return None, None, ["Rust relay endpoint has an invalid IP address"]
    if (
        (components[1] == "ip4" and parsed_address.version != 4)
        or (components[1] == "ip6" and parsed_address.version != 6)
        or "%" in address
    ):
        return None, None, ["Rust relay endpoint IP family does not match its native QUIC address"]
    if str(parsed_address) != address:
        return None, None, ["Rust relay endpoint IP address is not canonical"]

    port = components[4]
    if not port or any(character < "0" or character > "9" for character in port):
        return None, None, ["Rust relay endpoint has a non-decimal UDP port"]
    if len(port) > 5:
        return None, None, ["Rust relay endpoint UDP port has more than five decimal digits"]
    port_number = int(port)
    if not 1 <= port_number <= 65535:
        return None, None, ["Rust relay endpoint UDP port is outside 1..65535"]
    if str(port_number) != port:
        return None, None, ["Rust relay endpoint UDP port is not canonical"]

    peer_id = components[7] if len(components) == 8 else None
    if peer_id is not None and not relay_native_peer_id(peer_id):
        return None, None, ["Rust relay endpoint has an invalid donor-compatible PeerId"]
    return "/".join(components[:6]), peer_id, []


def sha256_value(value: object) -> bool:
    return isinstance(value, str) and SHA256.fullmatch(value) is not None


def token_value(value: object) -> bool:
    return isinstance(value, str) and re.fullmatch(r"[a-z0-9][a-z0-9._:-]{7,127}", value) is not None


def exact_phase_transcript(result: dict, security_protocol: str) -> list[str]:
    """Require the wire upgrade order rather than a negotiated-label summary."""
    transcript = result.get("upgrade_transcript")
    application = result.get("application_protocol")
    expected = [
        {"phase": "multistream", "protocol": "/multistream/1.0.0"},
        {"phase": "security", "protocol": security_protocol},
        {"phase": "muxer", "protocol": "/yamux/1.0.0"},
        {"phase": "application", "protocol": application},
    ]
    if not nonempty_string(application) or transcript != expected:
        return ["contract requires the exact ordered multistream/security/Yamux transcript"]
    return []


def control_result(record: dict, name: str, correlation_token: object, expected_status: str) -> tuple[Optional[dict], list[str]]:
    control = record.get(name)
    if not isinstance(control, dict):
        return None, [f"contract lacks the required {name} control"]
    result = control.get("result")
    if not isinstance(result, dict) or (
        result.get("status") != expected_status
        or result.get("control_kind") != name
        or result.get("correlation_token") != correlation_token
    ):
        return None, [f"{name} control lacks correlated {expected_status} result evidence"]
    return result, []


def validate_ping_evidence(result: dict, _record: dict, _listener: Optional[dict]) -> list[str]:
    if result.get("ping_ok") is False:
        return ["Ping evidence explicitly reports ping_ok=false"]
    if not ((type(result.get("rtt_ms")) is int and result["rtt_ms"] >= 0) or result.get("ping_ok") is True):
        return ["Ping evidence lacks an RTT or a successful Ping result"]
    nonce = result.get("ping_nonce")
    reply_nonce = result.get("ping_reply_nonce")
    if nonce is not None or reply_nonce is not None:
        if not token_value(nonce) or nonce != reply_nonce:
            return ["Ping nonce/reply evidence is incomplete or uncorrelated"]
    return []


def validate_identify_evidence(result: dict, _record: dict, _listener: Optional[dict]) -> list[str]:
    if result.get("signed_peer_record") is True and (
        positive_integer(result.get("protocol_count")) or positive_integer(result.get("payload_bytes"))
    ):
        return []
    return ["Identify evidence lacks a signed peer record and protocol payload"]


def validate_quic_v1_transport_evidence(result: dict, record: dict, listener: Optional[dict]) -> list[str]:
    """Require the connected endpoint's transport and authenticated peer, not CLI intent."""
    errors = validate_identify_evidence(result, record, listener)
    if result.get("negotiated_transport") != "/quic-v1":
        errors.append("QUIC evidence lacks the endpoint-observed /quic-v1 transport")
    if result.get("authenticated_remote_peer_id") != record.get("peer_id"):
        errors.append("QUIC evidence lacks the endpoint-authenticated remote peer identity")
    return errors


def validate_tcp_yamux_evidence(result: dict, record: dict, listener: Optional[dict],
                                security_protocols: tuple[str, ...] = ("/noise",),
                                require_identify: bool = True) -> list[str]:
    """Require endpoint-observed TCP upgrade state, not requested CLI transport."""
    errors = validate_identify_evidence(result, record, listener) if require_identify else []
    if result.get("negotiated_transport") != "tcp":
        errors.append("TCP/Yamux evidence lacks endpoint-observed tcp transport")
    if result.get("negotiated_security") not in security_protocols:
        errors.append("TCP/Yamux evidence lacks endpoint-observed supported security")
    if result.get("negotiated_muxer") != "/yamux/1.0.0":
        errors.append("TCP/Yamux evidence lacks endpoint-observed /yamux/1.0.0 muxer")
    if result.get("authenticated_remote_peer_id") != record.get("peer_id"):
        errors.append("TCP/Yamux evidence lacks the endpoint-authenticated remote peer identity")
    if not (
        result.get("echo_ok") is True
        and positive_integer(result.get("payload_bytes"))
        and nonempty_string(result.get("protocol"))
    ):
        errors.append("TCP/Yamux evidence lacks a completed echo payload")
    return errors


def validate_noise_multistream_evidence(result: dict, record: dict, listener: Optional[dict]) -> list[str]:
    errors = validate_identify_evidence(result, record, listener)
    errors.extend(exact_phase_transcript(result, "/noise"))
    if result.get("negotiated_security") != "/noise" or result.get("negotiated_muxer") != "/yamux/1.0.0":
        errors.append("Noise/multistream evidence lacks endpoint-observed TCP security and muxer selection")
    if result.get("authenticated_remote_peer_id") != record.get("peer_id"):
        errors.append("Noise/multistream evidence lacks the endpoint-authenticated remote peer identity")
    if result.get("selected_protocols") != ["/noise", "/yamux/1.0.0", result.get("application_protocol")]:
        errors.append("Noise/multistream selected protocols do not match its endpoint upgrade transcript")
    return errors


def validate_tls_evidence(result: dict, record: dict, listener: Optional[dict]) -> list[str]:
    errors = validate_identify_evidence(result, record, listener)
    errors.extend(exact_phase_transcript(result, "/tls/1.0.0"))
    if result.get("negotiated_security") != "/tls/1.0.0" or result.get("negotiated_muxer") != "/yamux/1.0.0":
        errors.append("TLS evidence lacks endpoint-observed TCP security and muxer selection")
    if result.get("authenticated_remote_peer_id") != record.get("peer_id"):
        errors.append("TLS evidence lacks the endpoint-authenticated remote peer identity")
    return errors


def pnet_control_errors(record: dict, name: str) -> list[str]:
    control = record.get(name)
    if not isinstance(control, dict):
        return [f"pnet evidence lacks the {name} control"]
    result = control.get("result")
    listener = control.get("listener_result")
    if not isinstance(result, dict) or not isinstance(listener, dict):
        return [f"pnet {name} control lacks dialer/listener result evidence"]
    errors: list[str] = []
    expected_sources = {
        "forge": "forge.node.metrics",
        "go": "go-libp2p.connection-gater",
        "rust": "rust-libp2p.swarm-events",
    }
    for endpoint_name, payload in (("dialer", result), ("listener", listener)):
        implementation = payload.get("implementation")
        if payload.get("status") != "rejected" or payload.get("control_kind") != name:
            errors.append(f"pnet {name} {endpoint_name} control lacks rejected terminal evidence")
            continue
        if implementation not in expected_sources or payload.get("counter_source") != expected_sources[implementation]:
            errors.append(f"pnet {name} {endpoint_name} control lacks runtime counter provenance")
        counters = {
            key: payload.get(key)
            for key in (
                "attempted_connections", "established_connections", "identify_streams", "application_streams"
            )
        }
        if any(type(value) is not int or value < 0 for value in counters.values()):
            errors.append(f"pnet {name} {endpoint_name} control has invalid observed counters")
            continue
        if counters["attempted_connections"] < 1:
            errors.append(f"pnet {name} {endpoint_name} control did not observe its connection boundary")
        rejected_before_identify = (
            counters["established_connections"] == 0
            and counters["identify_streams"] == 0
            and counters["application_streams"] == 0
        )
        if payload.get("rejected_before_identify") is not rejected_before_identify or not rejected_before_identify:
            errors.append(f"pnet {name} {endpoint_name} control crossed the authenticated-session boundary")
    if listener.get("correlation_token") != result.get("correlation_token"):
        errors.append(f"pnet {name} control correlation differs between endpoints")
    expected_peer = result.get("expected_peer_id")
    listener_process = control.get("listener_process")
    if not isinstance(listener_process, dict) or expected_peer != listener_process.get("peer_id"):
        errors.append(f"pnet {name} control is not bound to its listener peer")
    attempts = result.get("attempts")
    if not isinstance(attempts, list) or len(attempts) != 1:
        errors.append(f"pnet {name} control did not record exactly one dial attempt")
    return errors


def validate_pnet_evidence(result: dict, record: dict, listener: Optional[dict]) -> list[str]:
    errors: list[str] = []
    expected_fingerprint = result.get("pnet_fingerprint")
    if not (
        result.get("pnet_enabled") is True
        and result.get("negotiated_pnet") is True
        and isinstance(expected_fingerprint, str)
        and SHA256.fullmatch(expected_fingerprint)
    ):
        errors.append("pnet evidence lacks negotiated protection and a Forge operational fingerprint")
    if not isinstance(listener, dict) or any(
        listener.get(field) != result.get(field)
        for field in ("pnet_enabled", "negotiated_pnet", "pnet_fingerprint")
    ):
        errors.append("pnet listener result does not report the same negotiated protection fingerprint")
    for endpoint_name, payload in (("dialer", result), ("listener", listener)):
        if isinstance(payload, dict) and payload.get("implementation") == "rust":
            for field in PNET_FORBIDDEN_RUNTIME_FLAGS:
                if payload.get(field) is not False:
                    errors.append(f"pnet Rust {endpoint_name} did not prove {field}=false")
    if not (
        result.get("negotiated_transport") == "tcp"
        and result.get("negotiated_security") in {"/noise", "/tls/1.0.0"}
        and result.get("negotiated_muxer") == "/yamux/1.0.0"
        and result.get("authenticated_remote_peer_id") == record.get("peer_id")
        and result.get("identify_observed") is True
        and result.get("echo_ok") is True
        and positive_integer(result.get("payload_bytes"))
        and nonempty_string(result.get("protocol"))
    ):
        errors.append("pnet evidence lacks TCP -> PNET -> security -> Yamux authenticated stream proof")
    for control in ("missing_key", "mismatched_key"):
        errors.extend(pnet_control_errors(record, control))
    return errors


def relay_native_quic_transport_endpoint(
    value: object, relay_peer: object,
) -> tuple[Optional[str], list[str]]:
    """Match Rust Relay's native QUIC base-address construction and terminal peer binding."""
    transport_endpoint, endpoint_peer, errors = relay_native_quic_listener_endpoint(value)
    if errors:
        return None, errors
    if not relay_native_peer_id(relay_peer):
        return None, ["Rust relay record peer is not a valid donor-compatible PeerId"]
    if endpoint_peer is not None and endpoint_peer != relay_peer:
        return None, ["Rust relay endpoint terminal peer differs from record.peer_id"]
    return transport_endpoint, []


def rust_relay_command_target_errors(record: dict, relay_peer: str, relay_endpoint: str) -> list[str]:
    """Bind the reservation event to the listener endpoint and Rust dial command."""
    errors: list[str] = []
    listener = record.get("listener_process")
    if not isinstance(listener, dict):
        return ["Rust relay record lacks listener endpoint evidence"]
    listen_addrs = listener.get("listen_addrs")
    if (
        not isinstance(listen_addrs, list)
        or not listen_addrs
        or any(relay_native_quic_listener_endpoint(address)[0] is None for address in listen_addrs)
    ):
        errors.append("Rust relay listener evidence lacks a valid native QUIC listener list")
    elif not any(address == relay_endpoint for address in listen_addrs):
        errors.append("Rust relay listener evidence does not contain the exact recorded relay endpoint")
    if listener.get("peer_id") != relay_peer:
        errors.append("Rust relay listener peer differs from the recorded relay peer")

    raw_result = record.get("result")
    attempts = raw_result.get("attempts") if isinstance(raw_result, dict) else None
    if not isinstance(attempts, list) or not attempts:
        return [*errors, "Rust relay record lacks dial command evidence"]
    for attempt in attempts:
        command = attempt.get("command") if isinstance(attempt, dict) else None
        options, command_errors = command_options(command, "dial")
        if command_errors:
            errors.append("Rust relay dial command evidence is malformed")
            continue
        if options.get("--peer-id") != relay_peer or options.get("--addr") != relay_endpoint:
            errors.append("Rust relay dial command target differs from the recorded relay endpoint")
    return errors


def validate_relay_client_evidence(result: dict, record: dict, _listener: Optional[dict]) -> list[str]:
    implementation = result.get("implementation")
    if implementation == "forge" and positive_integer(result.get("voucher_bytes")):
        return []
    if (
        implementation == "go"
        and result.get("voucher") is True
        and isinstance(result.get("reservation_addrs"), list)
        and result["reservation_addrs"]
    ):
        return []
    if implementation == "rust":
        relay_peer = record.get("peer_id")
        client_peer = result.get("relay_reservation_client_peer_id")
        circuit_addr = result.get("relay_reservation_circuit_addr")
        relay_endpoint = record.get("addr")
        transport_endpoint, endpoint_errors = relay_native_quic_transport_endpoint(relay_endpoint, relay_peer)
        expected_circuit_addr = (
            f"{transport_endpoint}/p2p/{relay_peer}/p2p-circuit/p2p/{client_peer}"
            if relay_native_peer_id(relay_peer)
            and relay_native_peer_id(client_peer)
            and transport_endpoint is not None
            else None
        )
        errors = list(endpoint_errors)
        if relay_native_peer_id(relay_peer) and nonempty_string(relay_endpoint):
            errors.extend(rust_relay_command_target_errors(record, relay_peer, relay_endpoint))
        else:
            errors.append("Rust relay record lacks a valid relay peer and endpoint")
        if not (
            result.get("relay_reservation_accepted") is True
            and relay_native_peer_id(relay_peer)
            and result.get("relay_reservation_relay_peer_id") == relay_peer
            and relay_native_peer_id(client_peer)
            and circuit_addr == expected_circuit_addr
            and result.get("relay_reservation_renewal") is False
            and result.get("authenticated_remote_peer_id") == relay_peer
            and result.get("negotiated_transport") == "/quic-v1"
        ):
            errors.append(
                "Rust relay client evidence lacks a correlated ReservationReqAccepted event and exact confirmed circuit address"
            )
        return errors
    return ["relay client evidence lacks an implementation-specific reservation/open proof"]


def validate_kademlia_evidence(result: dict, record: dict, _listener: Optional[dict]) -> list[str]:
    listener_peer = record.get("peer_id")
    return validate_provider_evidence(result, listener_peer if isinstance(listener_peer, str) else None)


def validate_rendezvous_evidence(result: dict, _record: dict, _listener: Optional[dict]) -> list[str]:
    if (
        result.get("negotiated_protocol") == "/rendezvous/1.0.0"
        and positive_integer(result.get("wire_registration_count"))
        and result.get("signed_peer_record_valid") is True
        and result.get("matching_peer_record") is True
        and positive_integer(result.get("record_sequence"))
        and positive_integer(result.get("record_address_count"))
        and positive_integer(result.get("registered_ttl_seconds"))
        and positive_integer(result.get("discovered_ttl_seconds"))
        and positive_integer(result.get("cookie_bytes"))
    ):
        return []
    return ["Rendezvous evidence lacks the register/discover wire record proof"]


def evidence_contracts(validator, *scenario_ids: str) -> dict[str, object]:
    return {evidence_contract_for(scenario_id): validator for scenario_id in scenario_ids}


def validate_dnsaddr_evidence(result: dict, record: dict, listener: Optional[dict]) -> list[str]:
    # DNS resolution proves authenticated echo, not the independent Identify contract.
    return validate_tcp_yamux_evidence(result, record, listener, ("/noise", "/tls/1.0.0"), False) + validate_dnsaddr(result, record)


def validate_private_dnsaddr_evidence(result: dict, record: dict, listener: Optional[dict]) -> list[str]:
    return validate_pnet_evidence(result, record, listener) + validate_dnsaddr(result, record)


EVIDENCE_CONTRACT_VALIDATORS = {
    **evidence_contracts(validate_quic_v1_transport_evidence, "quic_v1_transport"),
    **evidence_contracts(validate_identify_evidence, "identify", "identify_native_tcp_yamux"),
    **evidence_contracts(validate_tcp_yamux_evidence, "tcp_yamux"),
    **evidence_contracts(validate_noise_multistream_evidence, "multistream_select", "noise_identity"),
    **evidence_contracts(validate_tls_evidence, "tls_identity"),
    **evidence_contracts(validate_ping_evidence, "ping", "ping_native_tcp_yamux"),
    **evidence_contracts(validate_relay_client_evidence, "relay_v2_client_transport"),
    **evidence_contracts(validate_kademlia_evidence, "kademlia_amino"),
    **evidence_contracts(validate_rendezvous_evidence, "rendezvous_rust"),
    **evidence_contracts(validate_pnet_evidence, "pnet"),
    **evidence_contracts(validate_dnsaddr_evidence, "dnsaddr"),
    **evidence_contracts(validate_private_dnsaddr_evidence, "dnsaddr_private_tcp_yamux_pnet"),
}


def validate_result_semantics(evidence_contract: str, result: dict, record: dict,
                              listener: Optional[dict]) -> list[str]:
    errors: list[str] = []
    if result.get("implementation") != record.get("dialer") or result.get("role") != "dialer":
        errors.append("raw runner result does not identify the recorded dialer role")
    if result.get("scenario") != record.get("scenario"):
        errors.append("raw runner result scenario differs from the raw runner record")
    validator = EVIDENCE_CONTRACT_VALIDATORS.get(evidence_contract)
    if validator is None:
        errors.append(f"evidence contract has no semantic validator: {evidence_contract}")
    else:
        errors.extend(validator(result, record, listener))
    return errors


def launcher_execution_description(options: dict[str, str], _payload: dict) -> dict[str, object]:
    """Summarize only non-secret launcher inputs for a registered run."""
    return {"transport": options.get("--transport")}


def validate_effective_configuration(record: dict, expected_profile: str,
                                     expected_stack: tuple[str, ...], dial_options: dict[str, str],
                                     listener_options: dict[str, str], dial_payload: dict,
                                     listener_payload: Optional[dict]) -> list[str]:
    """The record may summarize commands, but cannot declare protocol success."""
    expected = {
        "activation": "enabled",
        "profile": expected_profile,
        "transport_stack": list(expected_stack),
        "dialer": launcher_execution_description(dial_options, dial_payload),
        "listener": launcher_execution_description(listener_options, listener_payload or {}),
    }
    if record.get("effective_configuration") != expected:
        return ["raw runner effective configuration does not match launcher inputs"]
    return []


def pnet_fingerprint_for_launcher_key(value: object, artifact_root: Path) -> tuple[Optional[str], list[str]]:
    path = absolute_path(value)
    if path is None or not path.is_file():
        return None, ["pnet launcher key file is unavailable"]
    if path_within(str(path), artifact_root) is not None:
        return None, ["pnet launcher key file must remain outside the artifact directory"]
    raw = path.read_bytes()
    if raw.endswith(b"\r\n"):
        raw = raw[:-2]
    elif raw.endswith(b"\n"):
        raw = raw[:-1]
    lines = raw.split(b"\n")
    if (
        len(lines) != 3
        or lines[0] != b"/key/swarm/psk/1.0.0/"
        or lines[1] != b"/base16/"
        or len(lines[2]) != 64
        or any(value not in b"0123456789abcdef" for value in lines[2])
    ):
        return None, ["pnet launcher key file is not the canonical base16 swarm-key form"]
    return hashlib.sha256(PNET_FINGERPRINT_DOMAIN + bytes.fromhex(lines[2].decode("ascii"))).hexdigest(), []


def validate_pnet_launchers(record: dict, result: dict, listener: Optional[dict], dial_options: dict[str, str],
                            listener_options: dict[str, str], artifact_root: Path) -> list[str]:
    errors: list[str] = []
    dial_fingerprint, dial_errors = pnet_fingerprint_for_launcher_key(dial_options.get("--pnet-key-file"), artifact_root)
    listener_fingerprint, listener_errors = pnet_fingerprint_for_launcher_key(
        listener_options.get("--pnet-key-file"), artifact_root
    )
    errors.extend(dial_errors)
    errors.extend(listener_errors)
    if (
        dial_fingerprint is None
        or listener_fingerprint is None
        or dial_fingerprint != listener_fingerprint
        or dial_options.get("--pnet-fingerprint") != dial_fingerprint
        or listener_options.get("--pnet-fingerprint") != dial_fingerprint
        or result.get("pnet_fingerprint") != dial_fingerprint
        or not isinstance(listener, dict)
        or listener.get("pnet_fingerprint") != dial_fingerprint
    ):
        errors.append("pnet launcher inputs and endpoint results do not independently bind the Forge fingerprint")
    if listener_options.get("--features") != "ping,identify":
        errors.append("pnet listener command claims a non-private capability set")

    for name in ("missing_key", "mismatched_key"):
        control = record.get(name)
        if not isinstance(control, dict):
            continue
        control_result_value = control.get("result")
        control_listener = control.get("listener_process")
        attempts = control_result_value.get("attempts") if isinstance(control_result_value, dict) else None
        if not isinstance(attempts, list) or len(attempts) != 1 or not isinstance(control_listener, dict):
            continue
        control_options, control_errors = command_options(attempts[0].get("command"), "dial")
        errors.extend(control_errors)
        listener_control_options, listener_control_errors = command_options(control_listener.get("command"), "listen")
        errors.extend(listener_control_errors)
        if (
            control_options.get("--transport") != PRIVATE_NETWORK_TRANSPORT
            or control_options.get("--pnet-control") != name
            or control_options.get("--pnet-correlation") != control_result_value.get("correlation_token")
            or control_options.get("--pnet-fingerprint") != dial_fingerprint
            or listener_control_options.get("--transport") != PRIVATE_NETWORK_TRANSPORT
            or listener_control_options.get("--pnet-control") != name
            or listener_control_options.get("--pnet-correlation") != control_result_value.get("correlation_token")
            or listener_control_options.get("--features") != "ping,identify"
        ):
            errors.append(f"pnet {name} control launcher does not bind its direct private-network attempt")
        control_listener_fingerprint, control_listener_errors = pnet_fingerprint_for_launcher_key(
            listener_control_options.get("--pnet-key-file"), artifact_root
        )
        errors.extend(control_listener_errors)
        if control_listener_fingerprint != dial_fingerprint:
            errors.append(f"pnet {name} control listener key differs from the positive profile")
        if name == "missing_key":
            if "--pnet-key-file" in control_options:
                errors.append("pnet missing-key control unexpectedly configures a dialer key")
        else:
            control_fingerprint, control_key_errors = pnet_fingerprint_for_launcher_key(
                control_options.get("--pnet-key-file"), artifact_root
            )
            errors.extend(control_key_errors)
            if control_fingerprint is None or control_fingerprint == dial_fingerprint:
                errors.append("pnet mismatched-key control does not use an independent key")
    return errors

def validate_successful_raw_record(
    record: object,
    capability_id: str,
    expected_direction: str,
    expected_profile: str,
    expected_stack: tuple[str, ...],
    expected_runner_scenario: str,
    expected_acceptance_scenario: str,
    expected_evidence_contract: str,
    indexed_evidence: dict[Path, str],
    used_evidence: set[Path],
    binary_paths: dict[str, Path],
    artifact_root: Path,
) -> list[str]:
    if not isinstance(record, dict):
        return ["raw runner record must be an object"]
    missing = [field for field in ARTIFACT_SCHEMA["raw_artifact_required_fields"] if field not in record]
    if missing:
        return [f"raw runner record is missing required fields: {missing}"]
    errors: list[str] = []
    if direction_of(record) != expected_direction:
        errors.append("raw runner direction differs from the capability requirement")
    if record.get("profile") != expected_profile:
        errors.append("raw runner profile differs from the capability requirement")
    stack = record.get("transport_stack")
    if not isinstance(stack, list) or tuple(stack) != expected_stack:
        errors.append("raw runner transport stack differs from the capability requirement")
    if record.get("runner_scenario_id") != expected_runner_scenario:
        errors.append("raw runner scenario differs from the capability requirement")
    if record.get("acceptance_scenario_id") != expected_acceptance_scenario:
        errors.append("raw runner acceptance scenario differs from the capability requirement")
    fixture_scenario = DNSADDR_SCENARIOS.get(expected_runner_scenario, expected_runner_scenario.split("/", 1)[1])
    if record.get("scenario") != fixture_scenario:
        errors.append("raw runner fixture scenario does not match runner_scenario_id")
    expected_transport = expected_launcher_transport(
        expected_profile, expected_stack, expected_evidence_contract
    )
    if expected_transport is None:
        errors.append("manifest profile/transport stack has no canonical launcher transport")
    elif record.get("transport") != expected_transport:
        errors.append("raw runner transport cannot override the manifest contract launcher mapping")
    result = record.get("result")
    if not isinstance(result, dict) or result.get("status") != "ok":
        errors.append("raw runner result does not report status=ok")
        return errors
    result_file = result.get("result_file")
    attempts = result.get("attempts")
    if not isinstance(result_file, str) or not isinstance(attempts, list) or not attempts:
        errors.append("raw runner result lacks a result file or successful command attempts")
        return errors
    result_path = path_within(result_file, artifact_root)
    if result_path is None:
        errors.append("raw runner result file escapes the artifact directory")
        return errors
    payload, payload_errors = load_evidence_json(result_path, "raw runner result file")
    errors.extend(payload_errors)
    raw_payload = {key: value for key, value in result.items() if key not in {"result_file", "attempts"}}
    if payload is not None and payload != raw_payload:
        errors.append("raw runner result file does not match the recorded raw result payload")
    if failure_text(result):
        errors.append("raw runner result contains contradictory failure text")
    claim_paths = {result_path}
    dial_options: dict[str, str] = {}
    for attempt in attempts:
        if not isinstance(attempt, dict) or type(attempt.get("exit_code")) is not int or attempt["exit_code"] != 0:
            errors.append("raw runner command attempt did not exit with code 0")
            continue
        if attempt.get("kind") != "dial" or attempt.get("scenario_id") != record.get("scenario") or failure_text(attempt):
            errors.append("raw runner command attempt has contradictory execution status")
            continue
        command = attempt.get("command")
        if not isinstance(command, list) or not command or absolute_path(command[0]) != binary_paths.get(record.get("dialer")):
            errors.append("raw runner dial command executable differs from recorded dialer binary")
            continue
        options, command_errors = command_options(command, "dial")
        errors.extend(command_errors)
        required_options = {"--scenario", "--peer-id", "--addr", "--result-file", "--store-dir", "--transport"}
        optional_options = {"--payload", "--target-peer-id"}
        if expected_runner_scenario in DNSADDR_SCENARIOS:
            required_options.add("--dns-server")
        if expected_profile == "private_network":
            required_options |= {"--pnet-key-file", "--pnet-fingerprint"}
        if set(options) - (required_options | optional_options) or not required_options <= set(options):
            errors.append("raw runner dial command has an invalid option schema")
        elif (
            options["--scenario"] != record.get("scenario")
            or path_within(options["--result-file"], artifact_root) != result_path
            or path_within(options["--store-dir"], artifact_root) is None
            or not options["--peer-id"]
            or not options["--addr"]
            or options["--transport"] != expected_transport
        ):
            errors.append("raw runner dial command does not match its recorded result")
        else:
            dial_options = options
        log_file = attempt.get("log_file")
        if not isinstance(log_file, str):
            errors.append("raw runner command attempt lacks a log file")
        else:
            claim_paths.add(Path(log_file).resolve())

    listener_options: dict[str, str] = {}
    listener_payload: Optional[dict] = None
    listener = record.get("listener_process")
    terminal = listener.get("terminal_status") if isinstance(listener, dict) else None
    listener_log = listener.get("log_file") if isinstance(listener, dict) else None
    if not isinstance(listener_log, str) or not isinstance(terminal, dict) or terminal.get("exit_code") != 0:
        errors.append("raw runner listener lacks a clean terminal status")
    else:
        claim_paths.add(Path(listener_log).resolve())
        command = listener.get("command") if isinstance(listener, dict) else None
        if not isinstance(command, list) or not command or absolute_path(command[0]) != binary_paths.get(record.get("listener")):
            errors.append("raw runner listener command executable differs from recorded listener binary")
        else:
            options, command_errors = command_options(command, "listen")
            errors.extend(command_errors)
            required_options = {"--ready-file", "--stop-file", "--store-dir", "--features", "--transport", "--scenario"}
            optional_options = {"--result-file", "--seed-file", "--seed-peer-id", "--seed-addr", "--expected-messages"}
            if expected_profile == "private_network":
                required_options |= {"--pnet-key-file", "--pnet-fingerprint"}
            if set(options) - (required_options | optional_options) or not required_options <= set(options):
                errors.append("raw runner listener command has an invalid option schema")
            elif (
                options["--scenario"] != record.get("scenario")
                or options["--transport"] != expected_transport
                or not options["--features"]
                or any(path_within(options[field], artifact_root) is None for field in ("--ready-file", "--stop-file", "--store-dir"))
                or (
                    record.get("listener_result_file") is not None
                    and path_within(options.get("--result-file"), artifact_root)
                    != path_within(record.get("listener_result_file"), artifact_root)
                )
            ):
                errors.append("raw runner listener command does not match the recorded listener")
            else:
                listener_options = options
    listener_result_file = record.get("listener_result_file")
    if listener_result_file is not None:
        if not isinstance(listener_result_file, str):
            errors.append("raw runner listener result file is malformed")
        else:
            listener_result_path = path_within(listener_result_file, artifact_root)
            if listener_result_path is None:
                errors.append("raw runner listener result file escapes the artifact directory")
            else:
                listener_payload, listener_errors = load_evidence_json(
                    listener_result_path, "raw runner listener result file"
                )
                errors.extend(listener_errors)
                if listener_payload is not None and listener_payload != record.get("listener_result"):
                    errors.append("raw runner listener result file does not match the recorded listener payload")
                claim_paths.add(listener_result_path)

    errors.extend(validate_result_semantics(
        expected_evidence_contract, payload or {}, record, listener_payload
    ))
    if expected_runner_scenario in DNSADDR_SCENARIOS:
        dns = record.get("dns_evidence", {})
        dns_path = path_within(dns.get("log_file"), artifact_root) if isinstance(dns, dict) else None
        if dns_path is None:
            errors.append("DNSADDR authoritative log escapes the artifact directory")
        else:
            claim_paths.add(dns_path)
            dns_payload, dns_errors = load_evidence_json(dns_path, "authoritative DNS log")
            errors.extend(dns_errors)
            if dns_payload != {key: value for key, value in dns.items() if key not in {"log_file", "sha256"}}:
                errors.append("DNSADDR log differs from recorded authoritative observations")
            if dns_path.is_file() and sha256_file(dns_path) != dns.get("sha256"):
                errors.append("DNSADDR log hash differs from recorded observations")
            if dial_options.get("--dns-server") != dns.get("nameserver") or dial_options.get("--addr") != dns.get("root"):
                errors.append("DNSADDR launcher bypasses the authoritative root or resolver")
    if expected_profile == "private_network":
        errors.extend(validate_pnet_launchers(
            record, payload or {}, listener_payload, dial_options, listener_options, artifact_root
        ))
    errors.extend(validate_effective_configuration(
        record,
        expected_profile,
        expected_stack,
        dial_options,
        listener_options,
        payload or {},
        listener_payload,
    ))

    for path in claim_paths:
        if path not in indexed_evidence:
            errors.append("raw runner evidence is absent from the verified evidence index")
        elif path in used_evidence:
            errors.append("raw runner evidence is reused by multiple capability directions")
        else:
            used_evidence.add(path)
    return errors


def validate_execution_receipt(receipt: object, artifact_path: Path, artifact: dict) -> list[str]:
    if not isinstance(receipt, dict) or set(receipt) != {
        "schema_version", "runner_argv", "started_at_unix", "finished_at_unix", "returncode",
        "invocation_directory", "artifact_path", "artifact_sha256",
    }:
        return ["promotion execution receipt has invalid schema"]
    started = receipt.get("started_at_unix")
    finished = receipt.get("finished_at_unix")
    artifact_started = artifact.get("started_at_unix")
    artifact_finished = artifact.get("finished_at_unix")
    if (
        receipt.get("schema_version") != 2
        or receipt.get("runner_argv") != artifact.get("runner_argv")
        or receipt.get("returncode") != 0
        or absolute_path(receipt.get("invocation_directory")) != artifact_path.parent.resolve()
        or absolute_path(receipt.get("artifact_path")) != artifact_path.resolve()
        or receipt.get("artifact_sha256") != sha256_file(artifact_path)
        or not is_timestamp(started)
        or not is_timestamp(finished)
        or not is_timestamp(artifact_started)
        or not is_timestamp(artifact_finished)
        or started > artifact_started
        or finished < artifact_finished
    ):
        return ["promotion execution receipt does not bind this successful runner invocation"]
    return []


def validate(
    root: Path, manifest_path: Path, artifact_path: Path, expected_head: str,
    execution_receipt: Optional[dict] = None,
    expected_suite: Optional[str] = None,
) -> tuple[list[str], bool]:
    if not root.is_dir():
        return ["source root is unavailable"], False
    try:
        manifest = load_json(manifest_path)
        manifest_hash = sha256_file(manifest_path)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        return [f"manifest cannot be read: {error}"], False
    errors = []
    commit_timestamp, git_errors = validate_git_state(root, expected_head)
    errors.extend(git_errors)
    if not artifact_path.is_file():
        return [*errors, "artifact is missing"], False
    try:
        artifact = load_json(artifact_path)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        return [*errors, f"artifact cannot be read: {error}"], False
    if not isinstance(artifact, dict) or set(artifact) != set(ARTIFACT_SCHEMA["required_fields"]):
        return [*errors, "artifact has invalid canonical runner schema"], False
    provenance = artifact.get("fixture_provenance")
    runner_inputs = provenance.get("runner_inputs") if isinstance(provenance, dict) else None
    suite = runner_inputs.get("suite", "stage6") if isinstance(runner_inputs, dict) else None
    if suite not in ("stage6", "autonat") or (expected_suite is not None and suite != expected_suite):
        return [*errors, "artifact suite differs from requested canonical suite"], False
    required, manifest_errors = required_scenarios(manifest, suite)
    errors.extend(manifest_errors)
    if artifact.get("schema_version") != ARTIFACT_SCHEMA["schema_version"]:
        errors.append("artifact schema_version is invalid")
    if execution_receipt is not None:
        errors.extend(validate_execution_receipt(execution_receipt, artifact_path, artifact))
    root_value = artifact.get("artifact_root")
    if not isinstance(root_value, str) or not root_value or not Path(root_value).is_absolute():
        return [*errors, "artifact_root must be an absolute runner artifact directory"], False
    artifact_root = Path(root_value).resolve()
    if not artifact_root.is_dir():
        return [*errors, "artifact_root is unavailable"], False
    started = artifact.get("started_at_unix")
    finished = artifact.get("finished_at_unix")
    now = time.time()
    if (
        not is_timestamp(started)
        or not is_timestamp(finished)
        or commit_timestamp is None
        or started < commit_timestamp
        or finished <= started
        or finished > now
    ):
        errors.append("artifact timestamps do not bind the checked-out commit interval")

    acceptance_manifest = artifact.get("acceptance_manifest")
    if not isinstance(acceptance_manifest, dict) or set(acceptance_manifest) != {"path", "sha256"}:
        errors.append("artifact acceptance_manifest has invalid schema")
    elif (
        not isinstance(acceptance_manifest.get("path"), str)
        or Path(acceptance_manifest["path"]).resolve() != manifest_path.resolve()
        or acceptance_manifest.get("sha256") != manifest_hash
    ):
        errors.append("artifact acceptance_manifest does not bind the checked manifest hash")

    try:
        current_identity = worktree_identity(root).as_json()
    except RuntimeError as error:
        errors.append(f"cannot fingerprint checked-out worktree: {error}")
        current_identity = {}
    binary_paths, provenance_errors = validate_execution_provenance(
        artifact.get("fixture_provenance"), expected_head, current_identity
    )
    errors.extend(provenance_errors)
    inputs, input_errors = validate_runner_inputs(
        root, artifact_path, artifact_root, manifest_path, artifact.get("fixture_provenance"), suite
    )
    errors.extend(input_errors)
    errors.extend(validate_donor_provenance(root, artifact.get("fixture_provenance"), inputs))
    errors.extend(validate_runner_argv(
        root, artifact.get("runner_argv"), manifest_path, inputs, binary_paths, suite
    ))
    failures = artifact.get("failures")
    if not isinstance(failures, list):
        errors.append("canonical runner failures has invalid schema")
    elif failures:
        errors.append("canonical runner failures must be exactly empty")
    artifacts = artifact.get("artifacts")
    if not isinstance(artifacts, list) or not artifacts:
        return [*errors, "canonical runner artifacts must be a non-empty array"], False
    autonat_records = [record for record in artifacts if isinstance(record, dict) and record.get("suite") == "autonat"]
    base_records = [record for record in artifacts if not isinstance(record, dict) or record.get("suite") != "autonat"]
    autonat_required = {key: value for key, value in required.items() if key[1] in AUTONAT_SCENARIOS}
    base_required = {key: value for key, value in required.items() if key[1] not in AUTONAT_SCENARIOS}
    empty_logs = frozenset(
        Path(owner["log_file"]).resolve()
        for record in autonat_records
        for owner in (record.get("owned_processes") if isinstance(record.get("owned_processes"), list) else [])
        if isinstance(owner, dict) and isinstance(owner.get("log_file"), str)
    )
    indexed_evidence, evidence_errors = validate_evidence_index(
        artifact_path, artifact_root, artifacts, artifact.get("evidence_index"), empty_logs
    )
    errors.extend(evidence_errors)
    errors.extend(validate_all_result_evidence(artifacts, indexed_evidence, artifact_root))

    used_evidence: set[Path] = set()
    # Standalone legacy parser fixtures can have a partial manifest, but every
    # promotion receipt and every registered AutoNAT claim requires the full 41.
    if suite == "autonat" or autonat_required or autonat_records or execution_receipt is not None:
        def load_indexed_json(value):
            path = path_within(value, artifact_root)
            if path is None or path not in indexed_evidence:
                raise ValueError("AutoNAT raw output is absent from verified evidence index")
            payload, failures = load_evidence_json(path, "AutoNAT raw output")
            if failures or payload is None:
                raise ValueError("; ".join(failures))
            return payload

        # No receipt is duplicated across role claims, cases or directions.
        for record in autonat_records:
            paths = {path.resolve() for path in raw_evidence_paths(record)}
            if paths & used_evidence:
                errors.append("AutoNAT cases reuse raw process/network evidence")
            used_evidence.update(paths)
        errors.extend(validate_autonat_suite(
            autonat_records, autonat_required, artifact_root, binary_paths, load_indexed_json,
            pnet_fingerprint_for_launcher_key, root / CANONICAL_RUNNER.parent / "fixtures/pnet/swarm.key",
        ))
    if suite == "autonat":
        if base_records:
            errors.append("focused AutoNAT suite contains unrelated base records")
        return errors, False

    used_records: set[int] = set()
    for (capability_id, scenario_id), (
        expected_directions,
        _expected_status,
        expected_profile,
        expected_stack,
        expected_runner_scenario,
        _expected_requires,
        expected_evidence_contract,
    ) in base_required.items():
        for direction in expected_directions:
            matches = [
                index
                for index, record in enumerate(base_records)
                if isinstance(record, dict)
                and record.get("acceptance_scenario_id") == scenario_id
                and direction_of(record) == direction
            ]
            if len(matches) != 1:
                errors.append(
                    f"{capability_id}/{scenario_id}/{direction} lacks one canonical raw runner record"
                )
                continue
            index = matches[0]
            if index in used_records:
                errors.append(
                    f"{capability_id}/{scenario_id}/{direction} reuses a raw runner record"
                )
                continue
            used_records.add(index)
            errors.extend(
                f"{capability_id}/{scenario_id}/{direction}: {error}"
                for error in validate_successful_raw_record(
                    base_records[index],
                    capability_id,
                    direction,
                    expected_profile,
                    expected_stack,
                    expected_runner_scenario,
                    scenario_id,
                    expected_evidence_contract,
                    indexed_evidence,
                    used_evidence,
                    binary_paths,
                    artifact_root,
                )
            )
    return errors, any(
        status == ARTIFACT_SCHEMA["limited_status"] for _, status, _, _, _, _, _ in required.values()
    )


CURRENT_FIXTURES = {
    "dnsaddr": ("addressing.dnsaddr", "tcp_stage6/dnsaddr", ("tcp", "yamux"), "echo"),
    "dnsaddr_private_tcp_yamux_pnet": ("addressing.dnsaddr", "private_tcp_yamux_pnet/dnsaddr_private_tcp_yamux_pnet", ("tcp", "pnet", "yamux"), "pnet"),
    "quic_v1_transport": ("transport.quic_v1", "quic_base/identify", ("quic",), "identify"),
    "tcp_yamux": ("transport.tcp_yamux", "tcp_noise/echo", ("tcp", "yamux"), "echo"),
    "multistream_select": ("negotiation.multistream_select", "tcp_noise/identify", ("tcp", "yamux"), "identify"),
    "noise_identity": ("security.noise_tls_identity", "tcp_noise/identify", ("tcp", "yamux"), "identify"),
    "tls_identity": ("security.noise_tls_identity", "tcp_tls/identify", ("tcp", "yamux"), "identify"),
    "ping": ("protocol.ping", "quic_base/ping", ("quic",), "ping"),
    "ping_native_tcp_yamux": ("protocol.ping", "tcp_noise/ping", ("tcp", "yamux"), "ping"),
    "identify": ("protocol.identify", "quic_base/identify", ("quic",), "identify"),
    "identify_native_tcp_yamux": ("protocol.identify", "tcp_noise/identify", ("tcp", "yamux"), "identify"),
    "relay_v2_client_transport": ("relay.circuit_v2_client_transport", "quic_base/relay_reserve", ("quic",), "relay_reserve"),
    "kademlia_amino": ("routing.kademlia_amino", "quic_dht/dht_provide_find_provider", ("quic",), "dht_provide_find_provider"),
    "rendezvous_rust": ("discovery.rendezvous", "quic_rendezvous/rendezvous_register_discover", ("quic",), "rendezvous_register_discover"),
    "pnet": ("security.private_network_psk", "private_tcp_yamux_pnet/pnet", ("tcp", "pnet", "yamux"), "pnet"),
}


def fixture_manifest(scenario_id: str = "tcp_yamux") -> dict[str, object]:
    capability_id, runner_scenario_id, stack, _ = CURRENT_FIXTURES[scenario_id]
    profile = "private_network" if stack == ("tcp", "pnet", "yamux") else "native"
    return {
        "interop_acceptance_registry": {
            "artifact_schema": ARTIFACT_SCHEMA,
            "evidence_contracts": [evidence_contract_for(scenario_id)],
            "capabilities": {
                capability_id: {
                    "scenarios": [{
                        "id": scenario_id,
                        "runner_scenario_id": runner_scenario_id,
                        "profile": profile,
                        "transport_stack": list(stack),
                        "activation": "enabled",
                        "registration": "registered",
                        "source_case_id": "self-test.case",
                        "evidence_contract": evidence_contract_for(scenario_id),
                        "required_directions": ["forge_to_go", "go_to_forge"],
                        "expected_status": "passed",
                    }],
                },
            },
        },
    }


def semantic_provider_network_proof() -> dict[str, object]:
    """Observed-result-shaped Forge proof for the shared provider contract."""
    provider_peer = "provider-peer"
    querier_peer = "querier-peer"
    listener_peer = "listener-peer"
    provider_key = "1220provider-key"
    address = "/ip4/127.0.0.1/udp/1/quic-v1"
    return {
        "schema": "forge.libp2p.provider-network-proof.v1",
        "implementation": "forge",
        "provider_peer": provider_peer,
        "querier_peer": querier_peer,
        "listener_peer": listener_peer,
        "provider_key": provider_key,
        "key_binding": {
            "kind": "provider_identity_multihash", "provider_peer": provider_peer,
            "provider_key": provider_key, "derived_per_run": True,
        },
        "provider_registration": {
            "api": "async_provide", "succeeded": True, "provider_peer": provider_peer,
            "provider_key": provider_key,
        },
        "api_lookup": {
            "api": "async_find_providers", "succeeded": True, "querier_peer": querier_peer,
            "returned_provider_peer": provider_peer, "provider_key": provider_key,
        },
        "address_proof": {
            "source": "get_providers_wire_reply", "provider_peer": provider_peer, "address": address,
        },
        "protocol_proof": {
            "protocol": "/ipfs/kad/1.0.0",
            "derivation": "successful_async_open_protocol_stream_with_get_providers_wire_reply",
            "successful_query": True,
            "opened_stream_protocol": "/ipfs/kad/1.0.0",
        },
        "source_query_proof": {
            "kind": "forge_async_find_providers_result", "querier_peer": querier_peer,
            "returned_provider_peer": provider_peer, "provider_key": provider_key,
            "opened_stream_protocol": "/ipfs/kad/1.0.0",
        },
        "wire_proof": {
            "kind": "forge_get_providers_wire_reply", "explicit_wire_confirmation": True,
            "listener_peer": listener_peer, "returned_provider_peer": provider_peer,
            "address": address, "opened_stream_protocol": "/ipfs/kad/1.0.0",
        },
    }


def semantic_fixture(scenario_id: str) -> tuple[dict, dict, Optional[dict]]:
    """One observed-result-shaped fixture per executable registered contract."""
    if scenario_id in {"dnsaddr", "dnsaddr_private_tcp_yamux_pnet"}:
        result, record, listener = semantic_fixture("tcp_yamux" if scenario_id == "dnsaddr" else "pnet")
        root = "/dnsaddr/root.test/p2p/listener-peer"
        address = "/ip4/127.0.0.1/tcp/1/p2p/listener-peer"
        record["listener_process"] = {"listen_addrs": [address]}
        record["dns_evidence"] = {
            "root": root, "nameserver": "127.0.0.1:1234",
            "records": {"_dnsaddr.root.test": "dnsaddr=/dnsaddr/target.root.test/p2p/listener-peer",
                        "_dnsaddr.target.root.test": f"dnsaddr={address}"},
            "queries": [{"name": name, "type": 16, "answered": True}
                        for name in ("_dnsaddr.root.test", "_dnsaddr.target.root.test")],
        }
        result.update({"dns_input_address": root, "dns_resolver_configured": True,
                       "expected_peer_id": "listener-peer"})
        return result, record, listener
    _, _, stack, scenario = CURRENT_FIXTURES[scenario_id]
    record = {"dialer": "forge", "peer_id": "listener-peer", "scenario": scenario}
    result: dict[str, object] = {
        "implementation": "forge", "role": "dialer", "scenario": scenario, "status": "ok",
    }
    identify = {"signed_peer_record": True, "protocol_count": 2}
    if scenario_id == "quic_v1_transport":
        result.update(identify | {
            "negotiated_transport": "/quic-v1", "authenticated_remote_peer_id": "listener-peer",
        })
    elif scenario_id == "tcp_yamux":
        result.update(identify | {
            "negotiated_transport": "tcp", "negotiated_security": "/noise",
            "negotiated_muxer": "/yamux/1.0.0", "authenticated_remote_peer_id": "listener-peer",
            "protocol": "/forge/interop/echo/1", "payload_bytes": 7, "echo_ok": True,
        })
    elif scenario_id in {"multistream_select", "noise_identity"}:
        application = "/forge/interop/identify/1"
        result.update(identify | {
            "negotiated_security": "/noise", "negotiated_muxer": "/yamux/1.0.0",
            "authenticated_remote_peer_id": "listener-peer", "application_protocol": application,
            "selected_protocols": ["/noise", "/yamux/1.0.0", application],
            "upgrade_transcript": [
                {"phase": "multistream", "protocol": "/multistream/1.0.0"},
                {"phase": "security", "protocol": "/noise"},
                {"phase": "muxer", "protocol": "/yamux/1.0.0"},
                {"phase": "application", "protocol": application},
            ],
        })
    elif scenario_id == "tls_identity":
        application = "/forge/interop/identify/1"
        result.update(identify | {
            "negotiated_security": "/tls/1.0.0", "negotiated_muxer": "/yamux/1.0.0",
            "authenticated_remote_peer_id": "listener-peer", "application_protocol": application,
            "upgrade_transcript": [
                {"phase": "multistream", "protocol": "/multistream/1.0.0"},
                {"phase": "security", "protocol": "/tls/1.0.0"},
                {"phase": "muxer", "protocol": "/yamux/1.0.0"},
                {"phase": "application", "protocol": application},
            ],
        })
    elif scenario_id.startswith("ping"):
        result.update({"ping_ok": True, "rtt_ms": 1})
    elif scenario_id.startswith("identify"):
        result.update(identify)
    elif scenario_id == "relay_v2_client_transport":
        result.update({"implementation": "forge", "voucher_bytes": 16})
    elif scenario_id == "kademlia_amino":
        result["network_proof"] = semantic_provider_network_proof()
    elif scenario_id == "rendezvous_rust":
        result.update({
            "negotiated_protocol": "/rendezvous/1.0.0", "wire_registration_count": 1,
            "signed_peer_record_valid": True, "matching_peer_record": True,
            "record_sequence": 1, "record_address_count": 1, "registered_ttl_seconds": 60,
            "discovered_ttl_seconds": 60, "cookie_bytes": 8,
        })
    elif scenario_id == "pnet":
        result.update({
            "identify_observed": True, "negotiated_transport": "tcp",
            "negotiated_security": "/noise", "negotiated_muxer": "/yamux/1.0.0",
            "authenticated_remote_peer_id": "listener-peer", "protocol": "/forge/interop/echo/1",
            "payload_bytes": 7, "echo_ok": True, "pnet_enabled": True, "negotiated_pnet": True,
            "pnet_fingerprint": "0" * 64,
        })
        control_template = {
            "status": "rejected", "attempted_connections": 1, "established_connections": 0,
            "identify_streams": 0, "application_streams": 0, "rejected_before_identify": True,
            "correlation_token": "pnet-control", "counter_source": "forge.node.metrics", "attempts": [{}],
        }
        for name in ("missing_key", "mismatched_key"):
            record[name] = {
                "result": control_template | {
                    "implementation": "forge", "role": "dialer", "scenario": "pnet",
                    "control_kind": name, "expected_peer_id": "control-peer",
                },
                "listener_result": control_template | {
                    "implementation": "forge", "role": "listener", "scenario": "pnet", "control_kind": name,
                },
                "listener_process": {"peer_id": "control-peer"},
            }
    else:
        raise ValueError(f"unknown current fixture {scenario_id}")
    if stack == ("tcp", "yamux") and scenario_id not in {
        "tcp_yamux", "multistream_select", "noise_identity", "tls_identity",
    }:
        result.update({"negotiated_transport": "tcp"})
    if scenario_id == "pnet":
        return result, record, {
            "pnet_enabled": True, "negotiated_pnet": True, "pnet_fingerprint": "0" * 64,
        }
    return result, record, None


def rust_relay_reservation_fixture() -> tuple[dict, dict, Optional[dict]]:
    """Observed Rust relay-client event fields required for the reservation contract."""
    result, record, listener = semantic_fixture("relay_v2_client_transport")
    record["peer_id"] = RELAY_FIXTURE_RELAY_PEER_ID
    relay_endpoint = f"/ip4/127.0.0.1/udp/4001/quic-v1/p2p/{RELAY_FIXTURE_RELAY_PEER_ID}"
    record.update({
        "addr": relay_endpoint,
        "listener_process": {
            "peer_id": record["peer_id"],
            "listen_addrs": [relay_endpoint],
        },
        "result": {
            "attempts": [{
                "kind": "dial",
                "command": [
                    "/fixture/rust", "dial", "--peer-id", record["peer_id"], "--addr", relay_endpoint,
                ],
            }],
        },
    })
    result.update({
        "implementation": "rust",
        "negotiated_transport": "/quic-v1",
        "authenticated_remote_peer_id": record["peer_id"],
        "relay_reservation_accepted": True,
        "relay_reservation_relay_peer_id": record["peer_id"],
        "relay_reservation_client_peer_id": RELAY_FIXTURE_CLIENT_PEER_ID,
        "relay_reservation_circuit_addr": (
            f"/ip4/127.0.0.1/udp/4001/quic-v1/p2p/{record['peer_id']}"
            f"/p2p-circuit/p2p/{RELAY_FIXTURE_CLIENT_PEER_ID}"
        ),
        "relay_reservation_renewal": False,
    })
    result.pop("voucher_bytes")
    return result, record, listener


def fixture_identity(root: Path) -> dict[str, object]:
    return worktree_identity(root).as_json()


def build_evidence_index(root: Path, artifacts: list[dict]) -> list[dict]:
    root = root.resolve()
    return [
        {
            "path": path.resolve().relative_to(root).as_posix(),
            "size": path.resolve().stat().st_size,
            "sha256": sha256_file(path.resolve()),
        }
        for path in sorted(raw_evidence_paths(artifacts), key=lambda value: str(value))
    ]


def write_artifact(root: Path, manifest_path: Path, artifact_path: Path, scenario_id: str,
                   donors_root: Path) -> None:
    """Write a canonical-schema parser fixture; it never models future Stage 6 behavior."""
    capability_id, runner_scenario_id, stack, scenario = CURRENT_FIXTURES[scenario_id]
    del capability_id
    canonical_revisions = load_canonical_donor_revisions(
        root / CANONICAL_RUNNER.parent / "donor_cases.json"
    )
    fixture_lock = load_json(root / CANONICAL_RUNNER.parent / "fixture-lock.json")
    assert isinstance(fixture_lock, dict)
    fixture_donors = fixture_lock["donors"]
    donor_revisions, donor_errors = fixture_donor_revision_bindings(
        fixture_donors, canonical_revisions
    )
    assert not donor_errors
    artifact_root = artifact_path.parent / "interop-run"
    artifact_root.mkdir(parents=True, exist_ok=True)
    binaries: dict[str, Path] = {}
    for implementation in ("forge", "go", "rust"):
        binary = artifact_root / f"{implementation}-fixture"
        binary.write_text(f"{implementation} fixture\n")
        binaries[implementation] = binary
    transport = expected_launcher_transport("native", stack, evidence_contract_for(scenario_id))
    assert transport is not None
    artifacts: list[dict] = []
    for dialer, listener in (("forge", "go"), ("go", "forge")):
        stem = f"{dialer}-to-{listener}"
        result_payload, _, _ = semantic_fixture(scenario_id)
        result_payload = dict(result_payload)
        result_payload["implementation"] = dialer
        listener_payload = {"implementation": listener, "role": "listener", "status": "ok"}
        result_file = artifact_root / f"{stem}.json"
        listener_file = artifact_root / f"{stem}-listener.json"
        dial_log = artifact_root / f"{stem}-dial.log"
        listener_log = artifact_root / f"{stem}-listen.log"
        result_file.write_text(json.dumps(result_payload) + "\n")
        listener_file.write_text(json.dumps(listener_payload) + "\n")
        dial_log.write_text("dial completed\n")
        listener_log.write_text("listener completed\n")
        dial_command = [
            str(binaries[dialer]), "dial", "--scenario", scenario, "--peer-id", "listener-peer",
            "--addr", "/ip4/127.0.0.1/tcp/1", "--result-file", str(result_file),
            "--store-dir", str(artifact_root / f"{stem}-dial-store"), "--transport", transport,
        ]
        listener_command = [
            str(binaries[listener]), "listen", "--ready-file", str(artifact_root / f"{stem}.ready"),
            "--stop-file", str(artifact_root / f"{stem}.stop"),
            "--store-dir", str(artifact_root / f"{stem}-listen-store"), "--features", "ping",
            "--transport", transport, "--scenario", scenario, "--result-file", str(listener_file),
        ]
        dial_options = dict(zip(dial_command[2::2], dial_command[3::2]))
        listener_options = dict(zip(listener_command[2::2], listener_command[3::2]))
        artifacts.append({
            "dialer": dialer, "listener": listener, "scenario": scenario,
            "runner_scenario_id": runner_scenario_id, "acceptance_scenario_id": scenario_id,
            "profile": "native", "transport_stack": list(stack), "transport": transport,
            "peer_id": "listener-peer", "addr": "/ip4/127.0.0.1/tcp/1",
            "effective_configuration": {
                "activation": "enabled", "profile": "native", "transport_stack": list(stack),
                "dialer": launcher_execution_description(dial_options, result_payload),
                "listener": launcher_execution_description(listener_options, listener_payload),
            },
            "result": result_payload | {"result_file": str(result_file), "attempts": [{
                "kind": "dial", "scenario_id": scenario, "command": dial_command,
                "log_file": str(dial_log), "exit_code": 0,
            }]},
            "listener_process": {
                "command": listener_command, "log_file": str(listener_log),
                "terminal_status": {"exit_code": 0, "termination": "graceful"},
            },
            "listener_result_file": str(listener_file), "listener_result": listener_payload,
        })
    identity = fixture_identity(root)
    timestamp = max(time.time(), float(subprocess.check_output(
        ["git", "-C", str(root), "show", "-s", "--format=%ct", "HEAD"], text=True
    ).strip()))
    artifact = {
        "schema_version": 2,
        "runner_argv": [
            str(Path(sys.executable).resolve()), str((root / CANONICAL_RUNNER).resolve()),
            "--enabled", "ON", "--forge-fixture", str(binaries["forge"]),
            "--source-dir", str((root / CANONICAL_RUNNER).parent.resolve()),
            "--build-dir", str(artifact_path.parent.resolve()), "--forge-root", str(root.resolve()),
            "--donors-root", str(donors_root.resolve()),
            "--acceptance-manifest", str(manifest_path.resolve()),
        ],
        "started_at_unix": timestamp, "finished_at_unix": timestamp + 0.001,
        "acceptance_manifest": {"path": str(manifest_path.resolve()), "sha256": sha256_file(manifest_path)},
        "artifact_root": str(artifact_root.resolve()),
        "fixture_provenance": {
            "forge_worktree": {"start": identity, "end": identity, "changed_during_run": False},
            "fixture_build_info": {
                "schema_version": 2, "forge": identity,
                "compiler": {"path": "/fixture/clang", "id": "Clang", "version": "22"},
                "build_profile": "self-test",
            },
            "tools": {"python": {
                "path": str(Path(sys.executable).resolve()),
                "version_output": subprocess.check_output([str(Path(sys.executable).resolve()), "--version"], text=True).strip(),
            }},
            "binaries": {name: {"path": str(path), "sha256": sha256_file(path)} for name, path in binaries.items()},
            "runner_inputs": {
                "source_dir": str((root / CANONICAL_RUNNER).parent.resolve()),
                "build_dir": str(artifact_path.parent.resolve()), "forge_root": str(root.resolve()),
                "donors_root": str(donors_root.resolve()), "acceptance_manifest": str(manifest_path.resolve()),
            },
            "donors": fixture_donors,
            "donor_revisions": donor_revisions,
        },
        "artifacts": artifacts, "failures": [], "evidence_index": build_evidence_index(artifact_root, artifacts),
    }
    artifact_path.write_text(json.dumps(artifact))


def refresh_evidence_index(artifact_path: Path) -> None:
    artifact = load_json(artifact_path)
    assert isinstance(artifact, dict)
    artifact_root = Path(artifact["artifact_root"])
    artifact["evidence_index"] = build_evidence_index(artifact_root, artifact["artifacts"])
    artifact_path.write_text(json.dumps(artifact))


def expect_rejected(root: Path, manifest_path: Path, artifact_path: Path, head: str,
                    label: str, expected: str) -> bool:
    errors, _ = validate(root, manifest_path, artifact_path, head)
    if any(expected in error for error in errors):
        return True
    print(f"self-test failed: {label}: {errors}", file=sys.stderr)
    return False


def git_call(root: Path, *args: str) -> None:
    result = subprocess.run(["git", "-C", str(root), *args], text=True, capture_output=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "git command failed")


def self_test_donor_provenance(donors_root: Path) -> tuple[dict[str, str], list[dict[str, str]]]:
    fixture_donors: list[dict[str, str]] = []
    revisions: dict[str, str] = {}
    donors_root.mkdir()
    for name, directory in FIXTURE_DONOR_DIRECTORIES.items():
        checkout = donors_root / directory
        checkout.mkdir()
        git_call(checkout, "init", "-q")
        git_call(checkout, "config", "user.name", "Stage6 donor self-test")
        git_call(checkout, "config", "user.email", "stage6-donor@example.invalid")
        (checkout / "source.txt").write_text(f"{directory}\n")
        git_call(checkout, "add", "source.txt")
        git_call(checkout, "commit", "-qm", "pinned donor")
        commit, commit_error = git_output(checkout, "rev-parse", "HEAD")
        tree, tree_error = git_output(checkout, "rev-parse", "HEAD^{tree}")
        if commit is None or tree is None or commit_error is not None or tree_error is not None:
            raise RuntimeError("cannot create self-test donor provenance")
        revisions[directory] = commit
        fixture_donors.append({
            "name": name,
            "directory": directory,
            "commit": commit,
            "tree": tree,
        })
    return revisions, fixture_donors


def self_test() -> int:
    if not all(is_timestamp(value) for value in (0, 1.5, -1)) or any(
        is_timestamp(value) for value in (True, float("nan"), float("inf"), float("-inf"))
    ):
        print("self-test failed: timestamp finiteness validation is incorrect", file=sys.stderr)
        return 1
    # Every registered contract gets a positive observed-result fixture, then a semantic mutation.
    for contract, validator in EVIDENCE_CONTRACT_VALIDATORS.items():
        scenario_id = contract.removeprefix(EVIDENCE_CONTRACT_PREFIX).removesuffix(EVIDENCE_CONTRACT_SUFFIX)
        result, record, listener = semantic_fixture(scenario_id)
        if validator(result, record, listener):
            print(f"self-test failed: valid {scenario_id} fixture was rejected", file=sys.stderr)
            return 1
        if not validator({"status": "ok"}, record, listener):
            print(f"self-test failed: generic status-only {scenario_id} fixture was accepted", file=sys.stderr)
            return 1

    mutations = {
        "quic_v1_transport": ("negotiated_transport", "authenticated_remote_peer_id", "signed_peer_record"),
        "tcp_yamux": (
            "negotiated_transport", "negotiated_security", "negotiated_muxer",
            "authenticated_remote_peer_id", "echo_ok", "payload_bytes",
        ),
        "multistream_select": (
            "upgrade_transcript", "selected_protocols", "negotiated_security",
            "negotiated_muxer", "authenticated_remote_peer_id",
        ),
        "noise_identity": (
            "upgrade_transcript", "selected_protocols", "negotiated_security",
            "negotiated_muxer", "authenticated_remote_peer_id",
        ),
        "tls_identity": (
            "upgrade_transcript", "negotiated_security", "negotiated_muxer",
            "authenticated_remote_peer_id",
        ),
        "identify": ("signed_peer_record", "protocol_count"),
        "identify_native_tcp_yamux": ("signed_peer_record", "protocol_count"),
        "relay_v2_client_transport": ("voucher_bytes",),
        "kademlia_amino": ("network_proof",),
        "rendezvous_rust": (
            "negotiated_protocol", "wire_registration_count", "signed_peer_record_valid",
            "matching_peer_record", "record_sequence", "record_address_count",
            "registered_ttl_seconds", "discovered_ttl_seconds", "cookie_bytes",
        ),
        "pnet": (
            "pnet_enabled", "negotiated_pnet", "pnet_fingerprint", "identify_observed",
            "negotiated_security", "negotiated_muxer", "authenticated_remote_peer_id", "echo_ok", "payload_bytes",
        ),
    }
    for scenario_id, fields in mutations.items():
        for field in fields:
            result, record, listener = semantic_fixture(scenario_id)
            result.pop(field)
            validator = EVIDENCE_CONTRACT_VALIDATORS[evidence_contract_for(scenario_id)]
            if not validator(result, record, listener):
                print(f"self-test failed: {scenario_id} accepted without {field}", file=sys.stderr)
                return 1
    result, record, listener = semantic_fixture("pnet")
    result["implementation"] = "rust"
    listener["implementation"] = "rust"
    for payload in (result, listener):
        payload.update({field: False for field in PNET_FORBIDDEN_RUNTIME_FLAGS})
    if validate_pnet_evidence(result, record, listener):
        print("self-test failed: valid Rust private profile evidence was rejected", file=sys.stderr)
        return 1
    for endpoint_name in ("dialer", "listener"):
        for field in PNET_FORBIDDEN_RUNTIME_FLAGS:
            result, record, listener = semantic_fixture("pnet")
            result["implementation"] = "rust"
            listener["implementation"] = "rust"
            for payload in (result, listener):
                payload.update({name: False for name in PNET_FORBIDDEN_RUNTIME_FLAGS})
            (result if endpoint_name == "dialer" else listener)[field] = True
            if not validate_pnet_evidence(result, record, listener):
                print(
                    f"self-test failed: Rust pnet accepted {endpoint_name} with {field}=true",
                    file=sys.stderr,
                )
                return 1
    for control_name in ("missing_key", "mismatched_key"):
        for endpoint_name in ("result", "listener_result"):
            result, record, listener = semantic_fixture("pnet")
            record[control_name][endpoint_name]["attempted_connections"] = 0
            if not validate_pnet_evidence(result, record, listener):
                print(
                    f"self-test failed: pnet accepted {control_name} without {endpoint_name} boundary evidence",
                    file=sys.stderr,
                )
                return 1
    rust_relay, relay_record, relay_listener = rust_relay_reservation_fixture()
    if validate_relay_client_evidence(rust_relay, relay_record, relay_listener):
        print("self-test failed: valid Rust relay reservation fixture was rejected", file=sys.stderr)
        return 1
    peerless_endpoint = "/ip6/2001:db8::1/udp/65535/quic-v1"
    transport_endpoint, endpoint_errors = relay_native_quic_transport_endpoint(
        peerless_endpoint, RELAY_FIXTURE_RELAY_PEER_ID
    )
    if endpoint_errors or transport_endpoint != peerless_endpoint:
        print("self-test failed: peer-less native QUIC relay endpoint was rejected", file=sys.stderr)
        return 1
    for digest_size in (63, 64):
        multihash = bytes((RELAY_NATIVE_PEER_ID_SHA256_CODE, digest_size)) + bytes(range(digest_size))
        peer_id = relay_native_base58btc_encode(multihash)
        if (
            len(peer_id) > RELAY_NATIVE_PEER_ID_MAX_BASE58BTC_LENGTH
            or relay_native_base58btc_decode(peer_id) != multihash
            or relay_native_base58btc_encode(multihash) != peer_id
            or not relay_native_peer_id(peer_id)
        ):
            print(
                f"self-test failed: SHA2-256 PeerId with {digest_size}-byte digest was rejected",
                file=sys.stderr,
            )
            return 1
    oversized_multihash = (
        bytes((RELAY_NATIVE_PEER_ID_SHA256_CODE, RELAY_NATIVE_PEER_ID_MAX_DIGEST_BYTES + 1))
        + bytes(range(RELAY_NATIVE_PEER_ID_MAX_DIGEST_BYTES + 1))
    )
    oversized_peer_id = relay_native_base58btc_encode(oversized_multihash)
    if (
        len(oversized_peer_id) <= RELAY_NATIVE_PEER_ID_MAX_BASE58BTC_LENGTH
        or relay_native_base58btc_decode(oversized_peer_id) is not None
        or relay_native_peer_id(oversized_peer_id)
        or relay_native_multihash_is_peer_id(oversized_multihash)
    ):
        print("self-test failed: SHA2-256 PeerId with 65-byte digest was not rejected", file=sys.stderr)
        return 1
    if relay_native_base58btc_decode("2" * (RELAY_NATIVE_PEER_ID_MAX_BASE58BTC_LENGTH + 1)) is not None:
        print("self-test failed: oversized base58btc PeerId text was decoded", file=sys.stderr)
        return 1
    invalid_relay_endpoints = {
        "invalid IPv4": (
            f"/ip4/999.0.0.1/udp/4001/quic-v1/p2p/{RELAY_FIXTURE_RELAY_PEER_ID}"
        ),
        "non-canonical IPv6": (
            f"/ip6/2001:0db8::1/udp/4001/quic-v1/p2p/{RELAY_FIXTURE_RELAY_PEER_ID}"
        ),
        "UDP port zero": (
            f"/ip4/127.0.0.1/udp/0/quic-v1/p2p/{RELAY_FIXTURE_RELAY_PEER_ID}"
        ),
        "oversized UDP port": (
            f"/ip4/127.0.0.1/udp/65536/quic-v1/p2p/{RELAY_FIXTURE_RELAY_PEER_ID}"
        ),
        "non-numeric UDP port": (
            f"/ip4/127.0.0.1/udp/not-a-port/quic-v1/p2p/{RELAY_FIXTURE_RELAY_PEER_ID}"
        ),
        "non-canonical UDP port": (
            f"/ip4/127.0.0.1/udp/04001/quic-v1/p2p/{RELAY_FIXTURE_RELAY_PEER_ID}"
        ),
        "excessively long UDP port": (
            f"/ip4/127.0.0.1/udp/{'9' * 4301}/quic-v1/p2p/{RELAY_FIXTURE_RELAY_PEER_ID}"
        ),
        "invalid base58 PeerId": "/ip4/127.0.0.1/udp/4001/quic-v1/p2p/not-a-peer-id",
        "invalid multihash PeerId": "/ip4/127.0.0.1/udp/4001/quic-v1/p2p/111",
    }
    for label, endpoint in invalid_relay_endpoints.items():
        transport_endpoint, endpoint_errors = relay_native_quic_transport_endpoint(
            endpoint, RELAY_FIXTURE_RELAY_PEER_ID
        )
        if transport_endpoint is not None or not endpoint_errors:
            print(f"self-test failed: {label} relay endpoint was accepted", file=sys.stderr)
            return 1
    for field in (
        "relay_reservation_accepted",
        "relay_reservation_relay_peer_id",
        "relay_reservation_client_peer_id",
        "relay_reservation_circuit_addr",
        "relay_reservation_renewal",
        "authenticated_remote_peer_id",
        "negotiated_transport",
    ):
        rust_relay, relay_record, relay_listener = rust_relay_reservation_fixture()
        rust_relay.pop(field)
        if not validate_relay_client_evidence(rust_relay, relay_record, relay_listener):
            print(f"self-test failed: Rust relay reservation accepted without {field}", file=sys.stderr)
            return 1
    rust_relay, relay_record, relay_listener = rust_relay_reservation_fixture()
    rust_relay["relay_reservation_relay_peer_id"] = RELAY_FIXTURE_CLIENT_PEER_ID
    if not validate_relay_client_evidence(rust_relay, relay_record, relay_listener):
        print("self-test failed: Rust relay reservation accepted an uncorrelated relay peer", file=sys.stderr)
        return 1
    rust_relay, relay_record, relay_listener = rust_relay_reservation_fixture()
    rust_relay["relay_reservation_client_peer_id"] = RELAY_FIXTURE_RELAY_PEER_ID
    if not validate_relay_client_evidence(rust_relay, relay_record, relay_listener):
        print("self-test failed: Rust relay reservation accepted an uncorrelated client peer", file=sys.stderr)
        return 1
    rust_relay, relay_record, relay_listener = rust_relay_reservation_fixture()
    rust_relay["relay_reservation_circuit_addr"] = "/ip4/127.0.0.1/udp/4001/quic-v1/p2p/wrong"
    if not validate_relay_client_evidence(rust_relay, relay_record, relay_listener):
        print("self-test failed: Rust relay reservation accepted an unconfirmed circuit address", file=sys.stderr)
        return 1
    rust_relay, relay_record, relay_listener = rust_relay_reservation_fixture()
    rust_relay["relay_reservation_circuit_addr"] = rust_relay["relay_reservation_circuit_addr"].replace(
        "/udp/4001/quic-v1", "/tcp/4001"
    )
    if not validate_relay_client_evidence(rust_relay, relay_record, relay_listener):
        print("self-test failed: Rust relay reservation accepted a forged transport prefix", file=sys.stderr)
        return 1
    rust_relay, relay_record, relay_listener = rust_relay_reservation_fixture()
    command = relay_record["result"]["attempts"][0]["command"]
    command[command.index("--addr") + 1] = (
        f"/ip4/127.0.0.1/udp/4999/quic-v1/p2p/{RELAY_FIXTURE_RELAY_PEER_ID}"
    )
    if not validate_relay_client_evidence(rust_relay, relay_record, relay_listener):
        print("self-test failed: Rust relay reservation accepted a mismatched dial command target", file=sys.stderr)
        return 1
    rust_relay, relay_record, relay_listener = rust_relay_reservation_fixture()
    command = relay_record["result"]["attempts"][0]["command"]
    command[command.index("--peer-id") + 1] = RELAY_FIXTURE_CLIENT_PEER_ID
    if not validate_relay_client_evidence(rust_relay, relay_record, relay_listener):
        print("self-test failed: Rust relay reservation accepted a mismatched dial command peer", file=sys.stderr)
        return 1
    rust_relay, relay_record, relay_listener = rust_relay_reservation_fixture()
    conflicting_endpoint = relay_record["addr"].replace(
        RELAY_FIXTURE_RELAY_PEER_ID, RELAY_FIXTURE_CLIENT_PEER_ID
    )
    relay_record["addr"] = conflicting_endpoint
    relay_record["listener_process"]["listen_addrs"] = [conflicting_endpoint]
    command = relay_record["result"]["attempts"][0]["command"]
    command[command.index("--addr") + 1] = conflicting_endpoint
    if not validate_relay_client_evidence(rust_relay, relay_record, relay_listener):
        print("self-test failed: Rust relay reservation accepted a conflicting endpoint peer suffix", file=sys.stderr)
        return 1
    rust_relay, relay_record, relay_listener = rust_relay_reservation_fixture()
    relay_record["listener_process"]["listen_addrs"] = relay_record["addr"]
    if not validate_relay_client_evidence(rust_relay, relay_record, relay_listener):
        print("self-test failed: Rust relay reservation accepted string-valued listener addresses", file=sys.stderr)
        return 1
    rust_relay, relay_record, relay_listener = rust_relay_reservation_fixture()
    rust_relay["relay_reservation_renewal"] = True
    if not validate_relay_client_evidence(rust_relay, relay_record, relay_listener):
        print("self-test failed: Rust relay reservation accepted renewal=true for its initial reservation", file=sys.stderr)
        return 1
    ping, record, listener = semantic_fixture("ping")
    ping.update({"ping_ok": False, "rtt_ms": 1})
    if not validate_ping_evidence(ping, record, listener):
        print("self-test failed: false Ping with RTT was accepted", file=sys.stderr)
        return 1
    ping, record, listener = semantic_fixture("ping_native_tcp_yamux")
    ping.update({"ping_ok": False, "rtt_ms": 1})
    if not validate_ping_evidence(ping, record, listener):
        print("self-test failed: false TCP Ping with RTT was accepted", file=sys.stderr)
        return 1
    kademlia, record, listener = semantic_fixture("kademlia_amino")
    kademlia["network_proof"]["api_lookup"]["returned_provider_peer"] = "querier-peer"
    if not validate_kademlia_evidence(kademlia, record, listener):
        print("self-test failed: uncorrelated Kademlia provider was accepted", file=sys.stderr)
        return 1

    planned = fixture_manifest()
    planned_scenario = planned["interop_acceptance_registry"]["capabilities"]["transport.tcp_yamux"]["scenarios"][0]
    planned_scenario["registration"] = "planned"
    _, planned_errors = required_scenarios(planned)
    if not any("promotion-blocking" in error for error in planned_errors):
        print("self-test failed: planned scenario did not block promotion", file=sys.stderr)
        return 1
    unknown = fixture_manifest()
    unknown_scenario = unknown["interop_acceptance_registry"]["capabilities"]["transport.tcp_yamux"]["scenarios"][0]
    unknown_scenario["id"] = "unknown_registered"
    unknown_scenario["evidence_contract"] = evidence_contract_for("unknown_registered")
    unknown["interop_acceptance_registry"]["evidence_contracts"] = [evidence_contract_for("unknown_registered")]
    _, unknown_errors = required_scenarios(unknown)
    if not any("no executable validator" in error for error in unknown_errors):
        print("self-test failed: unknown contract was accepted", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as directory:
        nonfinite_path = Path(directory) / "nonfinite.json"
        for constant in ("NaN", "Infinity", "-Infinity", "1e9999"):
            nonfinite_path.write_text(f'{{"value":{constant}}}')
            try:
                load_json(nonfinite_path)
            except ValueError:
                continue
            print(f"self-test failed: {constant} JSON constant was accepted", file=sys.stderr)
            return 1
        root = Path(directory) / "forge"
        (root / CANONICAL_RUNNER).parent.mkdir(parents=True)
        (root / CANONICAL_RUNNER).write_text("# canonical runner fixture\n")
        donors_root = Path(directory) / "donors"
        donor_revisions, fixture_donors = self_test_donor_provenance(donors_root)
        source_dir = root / CANONICAL_RUNNER.parent
        (source_dir / "donor_cases.json").write_text(json.dumps({"donor_revisions": donor_revisions}))
        (source_dir / "fixture-lock.json").write_text(json.dumps({"donors": fixture_donors}))
        manifest_path = root / "manifest.json"
        manifest_path.write_text(json.dumps(fixture_manifest()))
        git_call(root.parent, "init", str(root))
        git_call(root, "config", "user.name", "Stage6 self-test")
        git_call(root, "config", "user.email", "stage6@example.invalid")
        git_call(root, "add", ".")
        git_call(root, "commit", "-qm", "canonical fixture")
        head, error = git_output(root, "rev-parse", "HEAD")
        if error is not None or head is None:
            print("self-test failed: missing fixture HEAD", file=sys.stderr)
            return 1
        artifact_path = Path(directory) / "build" / "interop-artifacts.json"
        write_artifact(root, manifest_path, artifact_path, "tcp_yamux", donors_root)
        errors, _ = validate(root, manifest_path, artifact_path, head)
        if errors:
            print(f"self-test failed: valid canonical artifact: {errors}", file=sys.stderr)
            return 1
        artifact_text = artifact_path.read_text()
        artifact_path.write_text(
            re.sub(r'("started_at_unix":)\s*[^,}]+', r'\1NaN', artifact_text, count=1)
        )
        if not expect_rejected(root, manifest_path, artifact_path, head, "NaN timestamp", "artifact cannot be read"):
            return 1
        write_artifact(root, manifest_path, artifact_path, "tcp_yamux", donors_root)
        artifact_text = artifact_path.read_text()
        artifact_path.write_text(
            re.sub(r'("started_at_unix":)\s*[^,}]+', r'\g<1>1e9999', artifact_text, count=1)
        )
        if not expect_rejected(
            root, manifest_path, artifact_path, head, "overflow timestamp", "artifact cannot be read"
        ):
            return 1
        write_artifact(root, manifest_path, artifact_path, "tcp_yamux", donors_root)
        artifact = load_json(artifact_path)
        assert isinstance(artifact, dict)
        artifact["fixture_provenance"]["donor_revisions"]["go-libp2p"] = "0" * 40
        artifact_path.write_text(json.dumps(artifact))
        if not expect_rejected(
            root, manifest_path, artifact_path, head, "donor revision mismatch", "donor revisions"
        ):
            return 1

        write_artifact(root, manifest_path, artifact_path, "tcp_yamux", donors_root)
        artifact = load_json(artifact_path)
        assert isinstance(artifact, dict)
        result_file = Path(artifact["artifacts"][0]["result"]["result_file"])
        result_file.write_text(
            re.sub(r'("payload_bytes":)\s*[^,}]+', r'\g<1>1e9999', result_file.read_text(), count=1)
        )
        refresh_evidence_index(artifact_path)
        if not expect_rejected(
            root, manifest_path, artifact_path, head, "overflow evidence", "result file is not valid JSON"
        ):
            return 1

        write_artifact(root, manifest_path, artifact_path, "tcp_yamux", donors_root)
        artifact = load_json(artifact_path)
        assert isinstance(artifact, dict)
        artifact["artifacts"][0]["result"].pop("authenticated_remote_peer_id")
        result_file = Path(artifact["artifacts"][0]["result"]["result_file"])
        payload = load_json(result_file)
        assert isinstance(payload, dict)
        payload.pop("authenticated_remote_peer_id")
        result_file.write_text(json.dumps(payload))
        artifact_path.write_text(json.dumps(artifact))
        refresh_evidence_index(artifact_path)
        if not expect_rejected(root, manifest_path, artifact_path, head, "missing TCP endpoint proof", "authenticated"):
            return 1

        manifest_path.write_text(json.dumps(fixture_manifest("quic_v1_transport")))
        git_call(root, "add", "manifest.json")
        git_call(root, "commit", "-qm", "quic manifest")
        head, error = git_output(root, "rev-parse", "HEAD")
        if error is not None or head is None:
            print("self-test failed: missing QUIC fixture HEAD", file=sys.stderr)
            return 1
        write_artifact(root, manifest_path, artifact_path, "quic_v1_transport", donors_root)
        artifact = load_json(artifact_path)
        assert isinstance(artifact, dict)
        for raw in artifact["artifacts"]:
            raw["transport"] = "tcp"
            for command in (raw["result"]["attempts"][0]["command"], raw["listener_process"]["command"]):
                command[command.index("--transport") + 1] = "tcp"
            raw["effective_configuration"]["dialer"]["transport"] = "tcp"
            raw["effective_configuration"]["listener"]["transport"] = "tcp"
        artifact_path.write_text(json.dumps(artifact))
        refresh_evidence_index(artifact_path)
        if not expect_rejected(root, manifest_path, artifact_path, head, "QUIC metadata with TCP command", "launcher mapping"):
            return 1

        manifest_path.write_text(json.dumps(fixture_manifest()))
        git_call(root, "add", "manifest.json")
        git_call(root, "commit", "-qm", "tcp manifest")
        head, error = git_output(root, "rev-parse", "HEAD")
        if error is not None or head is None:
            print("self-test failed: missing TCP fixture HEAD", file=sys.stderr)
            return 1
        write_artifact(root, manifest_path, artifact_path, "tcp_yamux", donors_root)
        artifact = load_json(artifact_path)
        assert isinstance(artifact, dict)
        artifact["failures"] = [{"message": "runner failure"}]
        artifact_path.write_text(json.dumps(artifact))
        if not expect_rejected(root, manifest_path, artifact_path, head, "runner failures", "failures"):
            return 1

        write_artifact(root, manifest_path, artifact_path, "tcp_yamux", donors_root)
        artifact = load_json(artifact_path)
        assert isinstance(artifact, dict)
        artifact["evidence_index"][0]["sha256"] = "0" * 64
        artifact_path.write_text(json.dumps(artifact))
        if not expect_rejected(root, manifest_path, artifact_path, head, "bad evidence hash", "hash or size"):
            return 1

    print(
        "stage6 acceptance checker self-test ok: closed current validators, planned promotion block, "
        "endpoint transport/upgrade proof, Ping contradiction, Kademlia correlation, canonical artifact and hashes"
    )
    return 0

def main() -> int:
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    if len(sys.argv) != 5:
        print(
            "usage: check_stage6_acceptance.py SOURCE_ROOT MANIFEST ARTIFACT EXPECTED_HEAD | --self-test",
            file=sys.stderr,
        )
        return 2
    errors, has_documented_limitations = validate(
        Path(sys.argv[1]).resolve(),
        Path(sys.argv[2]).resolve(),
        Path(sys.argv[3]).resolve(),
        sys.argv[4],
    )
    if errors:
        for error in errors:
            status = "FAILED" if error == "canonical runner failures must be exactly empty" else "NOT_RUN"
            print(f"{status}: {error}", file=sys.stderr)
        return 1
    if has_documented_limitations:
        print("CONSISTENT_WITH_DOCUMENTED_LIMITATIONS: standalone artifact consistency only; not promotion")
    else:
        print("CONSISTENT: standalone artifact consistency only; CMake promotion wrapper owns PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
