"""Exact PR10 native path execution and immutable source validation.

Binary hashes and evidence-index trust are checked by the shared checker before
passing binary_paths/load_json. No private TCP reuse claim is emitted here.
"""

from copy import deepcopy
from dataclasses import asdict
import math
from pathlib import Path

from autorelay_acceptance import ATTEMPT_FIELDS, OWNER_FIELDS, _options, _owned_output, _path, _require
from path_cases import RUNNER_SCENARIO_ID, case_specs
from path_evidence import RUST_SOURCE_WAVE_SCOPE, rust_source_wave_case, validate_case

DIRECTIONS = {"forge_to_go", "go_to_forge", "forge_to_rust", "rust_to_forge"}
SCENARIO_ID = "dcutr"
EVIDENCE_CONTRACT = "forge.p2p.evidence.dcutr.v1"
OWNER_ID = "relay.dcutr"


def claims_for(spec):
    return [SCENARIO_ID] if spec.outcome == "success" else []


def is_record(record):
    return isinstance(record, dict) and (record.get("suite") == "path" or record.get("scenario") == SCENARIO_ID)


def _actor_launch(spec, role, work, record):
    implementation = getattr(spec, role)
    addresses = {"relay": "11.0.0.1", "source": "10.1.0.2", "destination": "10.2.0.2"}
    options = {"--scenario": SCENARIO_ID, "--transport": "quic", "--path-role": role,
               "--case-token": record["case_token"], "--bind-ip": addresses[role]}
    options.update({"--" + name + "-file": str(work / f"{role}.{name}")
                    for name in ("ready", "result", "stop", "control", "plan")})
    if role != "relay":
        relay = record["raw"]["relay"]["ready"]
        _require(isinstance(relay.get("listen_addrs"), list) and len(relay["listen_addrs"]) == 1,
                 "missing exact relay listener launch source")
        options.update({"--relay-peer-id": relay["peer_id"], "--relay-addr": relay["listen_addrs"][0]})
    if implementation == "forge":
        options["--store-dir"] = str(work / f"{role}.store")
    return implementation, work / f"{role}.log", options


def _sources(record, spec, artifact_root, binary_paths, load_json):
    """Use the existing shared strict owner verifier on a validated argv view.

    process_evidence is not present in this checkout; _owned_output is the
    current shared implementation. Namespace wrapping and path stop budgets
    are independently validated here before its normalized read-only view.
    Original commands/receipts are never changed or manufactured.
    """
    root = Path(artifact_root).resolve()
    work = root / spec.identifier
    expected = {"schema_version": 1, "suite": "path", "scenario": SCENARIO_ID,
                "scenario_id": spec.identifier, "runner_scenario_id": RUNNER_SCENARIO_ID,
                "case": asdict(spec), "status": "passed", "errors": [], "cleanup_errors": [],
                "acceptance_scenario_ids": claims_for(spec),
                "application_actor": "destination" if spec.source == "forge" else "source"}
    _require(isinstance(record, dict) and all(record.get(k) == v for k, v in expected.items()),
             "path identity/claims/status/cleanup differs from exact native case")
    fields = set(expected) | {"case_token", "raw", "network", "attempts", "processes", "elapsed_seconds"}
    if rust_source_wave_case(asdict(spec)):
        _require(record.get("terminal_scope") == RUST_SOURCE_WAVE_SCOPE,
                 "Rust SOURCE negative case lacks explicit native-wave-only terminal scope")
        fields.update({"terminal_scope", "native_wave_failure"})
    if spec.outcome == "cancelled":
        fields.add("cancellation")
    _require(set(record) == fields, "missing fields or undeclared path claims")
    elapsed = record["elapsed_seconds"]
    _require(type(elapsed) in (int, float) and math.isfinite(elapsed) and 0 < elapsed <= 90,
             "unbounded path execution/lifecycle interval")
    raw = record["raw"]
    _require(isinstance(raw, dict) and set(raw) == {"source", "destination", "relay"}, "missing exact three process actors")
    for implementation in ("forge", "go", "rust"):
        binary = binary_paths[implementation]
        _require(Path(binary).is_absolute() and str(Path(binary).resolve()) == str(binary), "noncanonical indexed binary path")
    namespaces = {p["role"]: p["namespace"] for p in record["network"]["participants"]}
    hydrated, used_paths, owners = deepcopy(record), set(), []
    for role, actor in raw.items():
        _require(isinstance(actor, dict) and set(actor) == {"ready", "result", "process"}, "unexpected raw actor shape")
        implementation, log, options = _actor_launch(spec, role, work, record)
        owner = actor["process"]
        _require(isinstance(owner, dict) and set(owner) == OWNER_FIELDS | {"stop_budget"}, "invalid exact path process owner")
        budget = owner["stop_budget"]
        expected_budget = {"native_close_seconds": 8, "post_stop_seconds": 0, "scheduler_allowance_seconds": 2, "seconds": 10}
        _require(isinstance(budget, dict) and set(budget) == set(expected_budget)
                 and all(type(budget[k]) in (int, float) and math.isfinite(budget[k]) and budget[k] == v
                         for k, v in expected_budget.items()), "invalid exact native path stop budget")
        command = owner.get("command")
        _require(isinstance(command, list) and len(command) >= 6 and isinstance(command[0], str)
                 and Path(command[0]).is_absolute() and str(Path(command[0]).resolve()) == command[0]
                 and Path(command[0]).name == "ip" and command[1:4] == ["netns", "exec", namespaces[role]],
                 "fixture is not inside its exact owned Linux NAT namespace")
        _require(command[4] == str(binary_paths[implementation]) and command[5] == "path-live"
                 and _options(command[4:]) == options, "path binary/argv differs from exact native case")
        for name in ("control", "plan"):
            _path(options["--" + name + "-file"], work)
        view = deepcopy(actor)
        view["process"].pop("stop_budget")
        view["process"]["command"] = command[4:]
        loaded = _owned_output(view, implementation, "path-live", log, options, work,
                              binary_paths, load_json, used_paths)
        hydrated["raw"][role].update(loaded)
        owners.append((role, owner))
    processes, attempts = record["processes"], record["attempts"]
    _require(isinstance(processes, list) and len(processes) == 3
             and all(isinstance(p, dict) and type(p.get("pid")) is int and p["pid"] > 0 for p in processes)
             and len({p["pid"] for p in processes}) == 3
             and all(sum(p == owner for p in processes) == 1 for _, owner in owners),
             "missing/duplicate canonical owned process or PID")
    _require(isinstance(attempts, list) and len(attempts) == 3, "missing/duplicate three owned execution attempts")
    used_attempts = set()
    for role, owner in owners:
        matches = [(i, a) for i, a in enumerate(attempts) if isinstance(a, dict) and a.get("pid") == owner["pid"]]
        _require(len(matches) == 1, "missing/duplicate exact owned attempt")
        index, attempt = matches[0]
        _require(set(attempt) in (ATTEMPT_FIELDS, ATTEMPT_FIELDS | {"log_tail"})
                 and all(attempt.get(k) == owner[k] for k in ("pid", "command", "log_file", "terminal_status", "outputs"))
                 and type(attempt.get("pid")) is int and type(attempt.get("exit_code")) is int and attempt["exit_code"] == 0
                 and type(attempt.get("attempt_id")) is int and attempt["attempt_id"] == 1
                 and attempt.get("requested_log_file") == owner["log_file"] and attempt.get("kind") == role
                 and attempt.get("scenario_id") == spec.identifier
                 and type(attempt.get("timeout_seconds")) in (int, float) and attempt["timeout_seconds"] == 55,
                 "attempt differs from exact successful native path owner")
        used_attempts.add(index)
    _require(used_attempts == set(range(3)), "undeclared native path attempt")
    return hydrated


def validate_suite(records, required, artifact_root, binary_paths, load_json):
    """Canonical 12 cases, four successful native directions, indexed sources.

    required is the shared checker's {(owner, scenario): (directions, ...)}.
    load_json must reject non-indexed, changed or missing artifact files.
    """
    specs = {s.identifier: s for s in case_specs()}
    if (not isinstance(records, list) or len(records) != 12
            or any(not isinstance(r, dict) or not isinstance(r.get("scenario_id"), str) for r in records)
            or {r["scenario_id"] for r in records} != set(specs)):
        return ["path receipt must cover all 12 success/failure/cancellation cases exactly once"]
    requirement = required.get((OWNER_ID, SCENARIO_ID)) if isinstance(required, dict) else None
    if (not isinstance(required, dict) or set(required) != {(OWNER_ID, SCENARIO_ID)}
            or not isinstance(requirement, (list, tuple)) or len(requirement) != 7
            or not isinstance(requirement[0], (set, list, tuple)) or set(requirement[0]) != DIRECTIONS
            or not isinstance(requirement[3], (list, tuple))
            or (requirement[1], requirement[2], tuple(requirement[3]), requirement[4])
               != ("passed", "native", ("quic",), RUNNER_SCENARIO_ID)
            or requirement[5] != () or requirement[6] != EVIDENCE_CONTRACT):
        return ["native QUIC path claims differ from exact manifest requirements; TCP/private reuse is separate"]
    if not callable(load_json):
        return ["indexed raw JSON loader is required"]
    errors, pids, paths, claims = [], set(), set(), set()
    for record in records:
        spec = specs[record["scenario_id"]]
        used = set()
        def load(value):
            path = _path(value, Path(artifact_root).resolve() / spec.identifier)
            used.add(path)
            return load_json(value)
        try:
            hydrated = _sources(record, spec, artifact_root, binary_paths, load)
            errors.extend(f"{spec.identifier}: {e}" for e in validate_case(hydrated))
            owned = {p["pid"] for p in record["processes"]}
            _require(not owned & pids and not used & paths, "cases reuse owned PIDs or indexed snapshots")
            pids.update(owned)
            paths.update(used)
            if spec.outcome == "success":
                claims.add((OWNER_ID, SCENARIO_ID, f"{spec.source}_to_{spec.destination}"))
        except (ValueError, TypeError, KeyError, IndexError, OSError, RuntimeError, AttributeError, StopIteration) as error:
            errors.append(f"{spec.identifier}: {error}")
    if claims != {(OWNER_ID, SCENARIO_ID, d) for d in DIRECTIONS}:
        errors.append("missing native QUIC successful direction claims")
    return errors


def validate_record(record, *, verify_provenance):
    """Shared checker supplies its existing immutable binary/output provenance check."""
    errors = validate_case(record)
    if record.get("suite") != "path" or record.get("schema_version") != 1 or record.get("scenario") != "dcutr":
        errors.append("not a canonical path record")
    specs = {spec.identifier: spec for spec in case_specs()}
    spec = specs.get(record.get("case", {}).get("identifier"))
    if spec is None or record.get("case") != asdict(spec):
        errors.append("case differs from closed PR10 matrix")
    if record.get("status") != "passed":
        errors.append("path execution did not pass")
    if not callable(verify_provenance):
        errors.append("coordinator binary/raw-output provenance verifier is required")
    elif not errors:
        try:
            errors.extend(verify_provenance(record))
        except (ValueError, OSError, TypeError) as error:
            errors.append(f"provenance: {error}")
    return errors


def validate_matrix(records, *, verify_provenance):
    errors, observed = [], set()
    for record in records:
        identifier = record.get("case", {}).get("identifier")
        if identifier in observed:
            errors.append(f"duplicate path case: {identifier}")
        observed.add(identifier)
        errors.extend(validate_record(record, verify_provenance=verify_provenance))
    missing = {s.identifier for s in case_specs()} - observed
    if missing:
        errors.append("missing success/failure/cancellation directions: " + ", ".join(sorted(missing)))
    return errors
