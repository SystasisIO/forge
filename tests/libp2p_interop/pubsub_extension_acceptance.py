"""PR12 closed matrix and indexed native execution authority.

Thirty cases require original terminal success. Exactly six Rust QUIC cases
bind original active traffic and a separate, fully checked shutdown execution.
"""

from copy import deepcopy
from dataclasses import asdict

from pubsub_acceptance import owned_actor_sources, prepared_sources
from pubsub_evidence import DIRECTIONS, require
from pubsub_extension_cases import case_specs, run_case
from pubsub_extension_validation import needs_observer, validate_active_capture, validate_capture
from pubsub_quic_proof import complete_scoped_case, original_sources, validate_scoped_split
from stage6_evidence_contract import evidence_contract_for

OWNERS = {"idontwant": "pubsub.gossipsub_v1_2", "advertisement": "pubsub.gossipsub_v1_3",
          "partial": "pubsub.partial_messages"}
SCENARIOS = {spec.scenario: OWNERS[spec.extension] for spec in case_specs()}
EVIDENCE_CONTRACTS = {evidence_contract_for(name) for name in SCENARIOS}
ROLES = ("victim", "offender", "replacement", "sink")


def is_record(record):
    return isinstance(record, dict) and record.get("suite") == "pubsub-extensions"


def _envelope(record, spec, status):
    expected = {"schema_version": 1, "suite": "pubsub-extensions", "scenario_id": spec.identifier,
                "acceptance_scenario_id": spec.scenario, "runner_scenario_id": spec.runner_id,
                "case": asdict(spec), "status": status, "errors": [], "cleanup_errors": []}
    fields = set(expected) | {"case_token", "raw", "processes", "attempts", "shutdown_barrier"}
    if status == "observed":
        fields.add("evidence")
    require(isinstance(record, dict) and type(record.get("schema_version")) is int
            and all(record.get(key) == value for key, value in expected.items())
            and set(record) == fields,
            "extension case differs from exact original terminal execution matrix")


def complete_case(record, spec, *, fingerprint=None):
    """Attach semantics without mutating raw observations or excusing failures."""
    result = deepcopy(record)
    if result.get("status") != "captured":
        return result
    try:
        require(spec in case_specs(), "unknown extension case")
        _envelope(result, spec, "captured")
        result["evidence"] = validate_capture(result, expected_fingerprint=fingerprint)
        result["status"] = "observed"
    except (ValueError, TypeError, KeyError, IndexError, OSError, RuntimeError, AttributeError) as error:
        result["status"] = "HARNESS_ERROR"
        result.setdefault("errors", []).append(f"extension terminal proof: {error}")
    return result


def validate_record(record, spec, root, binaries, load_json, pnet_key_file, fingerprint):
    _envelope(record, spec, "observed")
    hydrated, used = owned_actor_sources(record, spec, root, binaries, load_json, pnet_key_file, fingerprint,
        {role: spec.implementation(role) for role in ROLES},
        extensions={role: spec.mode(role) for role in ROLES})
    evidence = validate_capture(hydrated,
        expected_fingerprint=fingerprint if spec.profile == "private_tcp_yamux" else None)
    prepared_sources(hydrated, spec, root, load_json, used)
    require(record["evidence"] == evidence, "declared extension proof differs from indexed native results")
    return used


def validate_original(record, spec, root, binaries, load_json):
    require(needs_observer(spec), "original active proof outside six Rust QUIC cases")
    expected = {"schema_version": 1, "suite": "pubsub-extensions", "scenario_id": spec.identifier,
                "acceptance_scenario_id": spec.scenario, "runner_scenario_id": spec.runner_id, "case": asdict(spec)}
    require(isinstance(record, dict) and type(record.get("schema_version")) is int
            and all(record.get(key) == value for key, value in expected.items())
            and set(record) == set(expected) | {"case_token", "raw", "processes", "attempts", "shutdown_barrier",
                                                "status", "errors", "cleanup_errors"}
            and record.get("status") == ("HARNESS_ERROR" if record.get("errors") else "captured"),
            "original extension run is not an unmodified runner capture")
    snapshots, used, pids = original_sources(record, spec, root, binaries, load_json,
        suite="pubsub-extensions", implementations={role: spec.implementation(role) for role in ROLES},
        extensions={role: spec.mode(role) for role in ROLES})
    return validate_active_capture(record, snapshots), used, pids


def _run_shutdown(spec, binaries, root, *, command_attempt=None):
    return complete_case(run_case(spec, binaries, root, command_attempt=command_attempt), spec)


def complete_split_case(original, spec, binaries, root, observer_binary, *, command_attempt=None):
    return complete_scoped_case(original, spec, binaries, root, observer_binary, suite="pubsub-extensions",
        eligible=needs_observer, original_validator=validate_original, run_shutdown=_run_shutdown,
        terminal_validator=validate_capture, command_attempt=command_attempt)


def validate_split(record, spec, root, binaries, load_json):
    return validate_scoped_split(record, spec, root, binaries, load_json, validate_record,
        suite="pubsub-extensions", eligible=needs_observer, original_validator=validate_original,
        terminal_validator=validate_capture)


def validate_suite(records, required, artifact_root, binary_paths, load_json, *, pnet_key_file=None,
                   pnet_fingerprint=None):
    specs = {spec.identifier: spec for spec in case_specs()}
    expected = {(owner, scenario) for scenario, owner in SCENARIOS.items()}
    directions = {source + "_to_" + target for source, target in DIRECTIONS}
    if not isinstance(required, dict) or set(required) != expected:
        return ["extension promotion requires all nine mode/profile contracts exactly"]
    for spec in specs.values():
        contract = required[(OWNERS[spec.extension], spec.scenario)]
        profile = "private_network" if spec.profile == "private_tcp_yamux" else "native"
        stack = {"native_quic": ("quic",), "native_tcp_yamux": ("tcp", "yamux"),
                 "private_tcp_yamux": ("tcp", "pnet", "yamux")}[spec.profile]
        dependencies = ("security.private_network_psk",) if profile == "private_network" else ()
        if not isinstance(contract, (tuple, list)) or len(contract) != 7 \
                or not isinstance(contract[0], (set, frozenset, tuple, list)) \
                or len(contract[0]) != 4 or any(not isinstance(value, str) for value in contract[0]) \
                or set(contract[0]) != directions \
                or tuple(contract[1:]) != ("passed", profile, stack, spec.runner_id, dependencies,
                                          evidence_contract_for(spec.scenario)):
            return ["extension declared directions/profile/wire evidence contract differs"]
    if not isinstance(records, list) or len(records) != 36 or not callable(load_json) \
            or any(not isinstance(record, dict) or not isinstance(record.get("scenario_id"), str)
                   for record in records) \
            or {record["scenario_id"] for record in records} != set(specs):
        return ["extension promotion requires all 36 unique native cases and indexed JSON loader"]
    errors, pids, paths = [], set(), set()
    for record in records:
        try:
            spec = specs[record["scenario_id"]]
            if needs_observer(spec):
                used, owned = validate_split(record, spec, artifact_root, binary_paths, load_json)
                count = 8
            else:
                used = validate_record(record, spec, artifact_root, binary_paths, load_json,
                                       pnet_key_file, pnet_fingerprint)
                owned = {owner["pid"] for owner in record["processes"].values()}
                count = 4
            require(len(owned) == count and not pids & owned and not paths & used,
                    "extension cases reuse native process/output identities")
            pids.update(owned)
            paths.update(used)
        except (ValueError, TypeError, KeyError, IndexError, OSError, RuntimeError, AttributeError) as error:
            errors.append(f"{record.get('scenario_id')}: {error}")
    return errors
