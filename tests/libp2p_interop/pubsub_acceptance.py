"""PR11 closed matrix and indexed process/output bindings.

The shared promotion checker owns exact-head, binary, donor and source hashes.
This component loads only its verified evidence index, then checks causal RPCs.
"""

from copy import deepcopy
from dataclasses import asdict
import math
from pathlib import Path

from autorelay_acceptance import ATTEMPT_FIELDS, OWNER_FIELDS, _options, _owned_output, _path
from pubsub_cases import _ready, case_specs
from pubsub_evidence import DIRECTIONS, PROFILES, prepared_snapshot, require, validate_case
from stage6_evidence_contract import evidence_contract_for

OWNER_ID = "pubsub.gossipsub_v1_0_v1_1"
SCENARIOS = {spec.scenario: OWNER_ID for spec in case_specs()}


def is_record(record):
    return isinstance(record, dict) and record.get("suite") == "pubsub-scoring"


def _sources(record, spec, root, binaries, load_json, pnet_key_file, fingerprint):
    expected = {"schema_version": 1, "suite": "pubsub-scoring", "scenario_id": spec.identifier,
                "acceptance_scenario_id": spec.scenario, "runner_scenario_id": spec.runner_id,
                "case": asdict(spec), "status": "observed", "errors": [], "cleanup_errors": [],
                "roles": {role: role for role in ("offender", "replacement", "sink")}}
    require(isinstance(record, dict) and all(record.get(key) == value for key, value in expected.items()),
            "PubSub case differs from exact closed execution matrix")
    require(set(record) == set(expected) | {"case_token", "raw", "processes", "attempts", "evidence", "shutdown_barrier"},
            "PubSub case has missing fields or undeclared claims")
    work = Path(root).resolve() / spec.identifier
    actors, processes = record["raw"], record["processes"]
    require(isinstance(actors, dict) and isinstance(processes, dict)
            and set(actors) == set(processes) == {"victim", "offender", "replacement", "sink"},
            "PubSub matrix has missing/extraneous process owners")
    hydrated, used = deepcopy(record), set()
    for role, raw in actors.items():
        implementation = spec.destination if role == "victim" else spec.source
        owner = processes[role]
        require(isinstance(owner, dict) and set(owner) == OWNER_FIELDS | {"stop_budget", "returncode", "forced_termination"},
                "invalid exact PubSub process owner")
        budget = {"native_close_seconds": 8, "post_stop_seconds": 0, "scheduler_allowance_seconds": 2, "seconds": 10}
        require(isinstance(owner["stop_budget"], dict) and set(owner["stop_budget"]) == set(budget)
                and all(type(owner["stop_budget"][key]) in (int, float)
                        and math.isfinite(owner["stop_budget"][key]) and owner["stop_budget"][key] == value
                        for key, value in budget.items()), "PubSub native stop budget differs")
        require(type(owner["returncode"]) is int and owner["returncode"] == 0
                and owner["forced_termination"] is False, "PubSub actor was not joined gracefully")
        options = {"--version": spec.version, "--transport": PROFILES[spec.profile],
                   "--actor": role, "--case-token": record["case_token"], "--store-dir": str(work / f"{role}.store")}
        options.update({f"--{name}-file": str(work / f"{role}.{name}")
                        for name in ("ready", "result", "stop", "control")})
        if spec.profile == "private_tcp_yamux":
            require(pnet_key_file is not None and isinstance(fingerprint, str), "canonical private key binding missing")
            options.update({"--pnet-key-file": str(pnet_key_file), "--pnet-fingerprint": fingerprint})
        require(_options(owner["command"]) == options, "PubSub launch flags differ from fixed scenario")
        _path(options["--control-file"], work)
        view = {"ready": owner["ready"], "result": raw,
                "process": {key: value for key, value in owner.items() if key in OWNER_FIELDS}}
        loaded = _owned_output(view, implementation, "pubsub-live", work / f"{role}.log", options,
                               work, binaries, load_json, used)
        require(_ready(loaded["ready"], loaded["result"], implementation, role, record["case_token"], terminal=True),
                "native canonical readiness/result identity differs")
        hydrated["raw"][role] = loaded["result"]
        matches = [attempt for attempt in record["attempts"] if attempt.get("pid") == owner["pid"]]
        require(len(matches) == 1, "PubSub owner has missing/duplicate native execution attempt")
        attempt = matches[0]
        require(set(attempt) in (ATTEMPT_FIELDS, ATTEMPT_FIELDS | {"log_tail"})
                and all(attempt.get(key) == owner[key] for key in ("pid", "command", "log_file", "terminal_status", "outputs"))
                and type(attempt.get("exit_code")) is int and attempt["exit_code"] == 0
                and type(attempt.get("attempt_id")) is int and attempt["attempt_id"] == 1
                and attempt.get("scenario_id") == spec.identifier and attempt.get("kind") == role
                and attempt.get("requested_log_file") == owner["log_file"]
                and type(attempt.get("timeout_seconds")) in (int, float) and attempt["timeout_seconds"] == 60,
                "PubSub execution attempt differs from indexed owner")
    require(len(record["attempts"]) == 4, "PubSub case has undeclared extra attempts")
    evidence = validate_case(hydrated, expected_fingerprint=fingerprint if spec.profile == "private_tcp_yamux" else None)
    for row in hydrated["shutdown_barrier"]["operations"][:4]:
        role = row["actor"]
        expected_path = work / f"{role}.prepare-result.json"
        require(row["evidence_file"] == str(expected_path), "prepare evidence path differs from exact actor")
        path = _path(row["evidence_file"], work)
        require(path not in used, "actors reuse indexed prepare evidence")
        snapshot = load_json(row["evidence_file"])
        prepared_snapshot(snapshot, row, hydrated["raw"][role], processes[role]["pid"])
        used.add(path)
    require(record["evidence"] == evidence, "declared PubSub observations differ from native indexed result")
    return used


def validate_suite(records, required, artifact_root, binary_paths, load_json, *, pnet_key_file=None, pnet_fingerprint=None):
    specs = {spec.identifier: spec for spec in case_specs()}
    expected = {(OWNER_ID, spec.scenario) for spec in specs.values()}
    directions = {source + "_to_" + target for source, target in DIRECTIONS}
    errors, pids, paths = [], set(), set()
    if not isinstance(required, dict) or set(required) != expected:
        return ["PubSub promotion requires all six version/profile contracts exactly"]
    for spec in specs.values():
        contract = required[(OWNER_ID, spec.scenario)]
        profile = "private_network" if spec.profile == "private_tcp_yamux" else "native"
        stack = {"native_quic": ("quic",), "native_tcp_yamux": ("tcp", "yamux"),
                 "private_tcp_yamux": ("tcp", "pnet", "yamux")}[spec.profile]
        dependencies = ("security.private_network_psk",) if profile == "private_network" else ()
        if len(contract) != 7 or set(contract[0]) != directions \
                or tuple(contract[1:]) != ("passed", profile, stack, spec.runner_id, dependencies,
                                          evidence_contract_for(spec.scenario)):
            return ["PubSub declared directions/profile/wire evidence contract differs"]
    if not isinstance(records, list) or len(records) != 24 or not callable(load_json) \
            or any(not isinstance(record, dict) for record in records) \
            or {record.get("scenario_id") for record in records} != set(specs):
        return ["PubSub promotion requires all 24 unique native cases and indexed JSON loader"]
    for record in records:
        try:
            used = _sources(record, specs[record["scenario_id"]], artifact_root, binary_paths, load_json,
                            pnet_key_file, pnet_fingerprint)
            owned = {owner["pid"] for owner in record["processes"].values()}
            require(not pids & owned and not paths & used, "PubSub cases reuse native process/output identities")
            pids.update(owned)
            paths.update(used)
        except (ValueError, TypeError, KeyError, IndexError, OSError, RuntimeError, AttributeError) as error:
            errors.append(f"{record.get('scenario_id')}: {error}")
    return errors
