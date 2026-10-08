"""Separate original Rust traffic from an explicitly instrumented shutdown run."""

from dataclasses import asdict
from pathlib import Path

from autorelay_acceptance import ATTEMPT_FIELDS, OWNER_FIELDS, _options, _path
from pubsub_cases import _ready, run_case
from pubsub_evidence import require, validate_active_case, validate_case
from provenance import reject_duplicate_json_keys
import json


SCOPE = "original_wire_and_instrumented_shutdown"


def _snapshot(path):
    path = Path(path)
    require(path.stat().st_size <= 16 * 1024 * 1024, "original snapshot exceeds bound")
    return json.loads(path.read_text(), object_pairs_hook=reject_duplicate_json_keys)


def needs_observer(spec):
    return spec.profile == "native_quic" and "rust" in (spec.source, spec.destination)


def complete_case(original, spec, binaries, root, observer_binary, *, command_attempt=None):
    """Keep original errors intact; another execution cannot explain their cause."""
    result = {"schema_version": 1, "suite": "pubsub-scoring", "scenario_id": spec.identifier,
              "acceptance_scenario_id": spec.scenario, "runner_scenario_id": spec.runner_id,
              "case": asdict(spec), "proof_scope": SCOPE, "original": original,
              "shutdown": None, "status": "HARNESS_ERROR", "errors": [], "cleanup_errors": []}
    active_valid = False
    try:
        require(needs_observer(spec), "QUIC observer outside exact Rust matrix")
        require(isinstance(original, dict), "invalid original active proof")
        barrier = original.get("shutdown_barrier")
        require(isinstance(barrier, dict) and set(barrier) == {"source", "operations"}
                and barrier.get("source") == "python.fixture.all_actor_prepare_barrier"
                and isinstance(barrier.get("operations"), list), "missing/invalid original active Prepare barrier")
        rows = barrier["operations"][:4]
        require(len(rows) == 4 and all(isinstance(row, dict) for row in rows)
                and {row.get("actor") for row in rows} == {"victim", "offender", "replacement", "sink"}
                and all(row.get("kind") == "prepare_ack" for row in rows),
                "original active proof lacks four distinct Prepare ACK references")
        wire, _, _ = validate_original(original, spec, root, binaries, _snapshot)
        active_valid = True
        observer_binaries = dict(binaries, rust=observer_binary)
        shutdown = run_case(spec, observer_binaries, Path(root) / "quic-observer", command_attempt=command_attempt)
        result["shutdown"] = shutdown
        require(original["case_token"] != shutdown["case_token"], "observer reused original run token")
        terminal = validate_case(shutdown)
        require(wire == terminal, "independent observer scenario differs from original traffic contract")
        result["evidence"] = {"original_wire": wire, "instrumented_shutdown": terminal,
                              "original_shutdown": "NOT_PROVEN"}
        result["status"] = "observed"
    except (ValueError, TypeError, KeyError, OSError, RuntimeError) as error:
        if not active_valid and isinstance(original, dict):
            for field in ("errors", "cleanup_errors"):
                diagnostics = original.get(field)
                if isinstance(diagnostics, list):
                    result[field].extend(f"original: {message[:1024]}" for message in diagnostics[:8]
                                         if isinstance(message, str))
        result["errors"].append(f"{type(error).__name__}: {error}")
    return result


def validate_original(record, spec, root, binaries, load_json):
    """Bind active snapshots to original binary/PIDs and preserve diagnostic terminal JSON."""
    require(record.get("case") == asdict(spec) and record.get("scenario_id") == spec.identifier
            and record.get("suite") == "pubsub-scoring", "original scenario differs from exact matrix")
    work = Path(root).resolve() / spec.identifier
    actors, processes = record.get("raw"), record.get("processes")
    require(isinstance(actors, dict) and isinstance(processes, dict)
            and set(actors) == set(processes) == {"victim", "offender", "replacement", "sink"},
            "original lacks four actual native process owners")
    used, pids = set(), set()
    for role, raw in actors.items():
        implementation = spec.destination if role == "victim" else spec.source
        owner = processes[role]
        require(raw.get("finalized") is True and raw.get("joined") is True and raw.get("overflow") is False,
                "original raw terminal capture is unfinished or overflowed")
        require(set(owner) == OWNER_FIELDS | {"stop_budget", "returncode", "forced_termination"},
                "invalid original process schema")
        require(type(owner["pid"]) is int and owner["pid"] > 0 and owner["pid"] not in pids,
                "original process PID missing/reused")
        pids.add(owner["pid"])
        code = owner["returncode"]
        require(type(code) is int and code in (0, 1) and owner["forced_termination"] is False
                and owner["terminal_status"] == {"exit_code": code, "termination": "graceful"},
                "original owner killed, timed out or not actually joined")
        require(owner["stop_budget"] == {"native_close_seconds": 8, "post_stop_seconds": 0,
                                        "scheduler_allowance_seconds": 2, "seconds": 10},
                "original stop budget changed")
        options = {"--version": spec.version, "--transport": "quic", "--actor": role,
                   "--case-token": record["case_token"], "--store-dir": str(work / f"{role}.store")}
        options.update({f"--{name}-file": str(work / f"{role}.{name}")
                        for name in ("ready", "result", "stop", "control")})
        require(owner["command"][:2] == [str(binaries[implementation]), "pubsub-live"]
                and _options(owner["command"]) == options, "original binary/launch was replaced")
        log = work / f"{role}.log"
        require(owner["log_file"] == str(log), "original log path changed")
        used.add(_path(str(log), work))
        outputs = owner["outputs"]
        require(isinstance(outputs, list) and len(outputs) == 2, "original immutable outputs missing")
        for output, flag, expected in zip(outputs, ("--ready-file", "--result-file"), (owner["ready"], raw)):
            snapshot = str(log) + f".{flag[2:]}.json"
            require(output == {"argument": flag, "path": options[flag], "exists": True, "log_file": snapshot},
                    "original indexed snapshot changed")
            path = _path(snapshot, work)
            require(path not in used and load_json(snapshot) == expected,
                    "original embedded raw differs from indexed snapshot")
            used.add(path)
        attempts = [value for value in record["attempts"] if value.get("pid") == owner["pid"]]
        require(len(attempts) == 1, "original owner missing/duplicated attempt")
        attempt = attempts[0]
        require(set(attempt) in (ATTEMPT_FIELDS, ATTEMPT_FIELDS | {"log_tail"})
                and all(attempt.get(key) == owner[key] for key in ("pid", "command", "log_file", "terminal_status", "outputs"))
                and type(attempt.get("exit_code")) is int and attempt["exit_code"] == code
                and attempt.get("attempt_id") == 1 and attempt.get("scenario_id") == spec.identifier
                and attempt.get("kind") == role and attempt.get("requested_log_file") == owner["log_file"]
                and attempt.get("timeout_seconds") == 60, "original attempt differs from native owner")
    require(len(record["attempts"]) == 4, "original has extra native attempts")
    snapshots = {}
    for row in record["shutdown_barrier"]["operations"][:4]:
        role = row["actor"]
        require(row["evidence_file"] == str(work / f"{role}.prepare-result.json"), "original prepare path changed")
        path = _path(row["evidence_file"], work)
        require(path not in used, "original preparation reuses an indexed path")
        snapshots[role] = load_json(str(path))
        implementation = spec.destination if role == "victim" else spec.source
        require(_ready(processes[role]["ready"], snapshots[role].get("result"), implementation,
                       role, record["case_token"]), "original active readiness/identity differs")
        used.add(path)
    return validate_active_case(record, snapshots), used, pids


def original_stdout_owners(records, root, binaries):
    """Classify diagnostic stdout only after actual original traffic verification.

    Evidence-index hashing still follows; this grants no terminal-success claim.
    """
    from pubsub_cases import case_specs
    specs = {spec.identifier: spec for spec in case_specs() if needs_observer(spec)}
    owners = {}
    for record in records:
        if not isinstance(record, dict) or record.get("proof_scope") != SCOPE:
            continue
        try:
            spec = specs[record["scenario_id"]]
            original = record["original"]
            validate_original(original, spec, root, binaries, _snapshot)
            for owner in original["processes"].values():
                path = Path(owner["log_file"]).resolve()
                require(path not in owners, "original diagnostic stdout owner reused")
                owners[path] = owner
        except (ValueError, TypeError, KeyError, OSError, RuntimeError):
            continue
    return owners


def validate_split(record, spec, root, binaries, load_json, validate_shutdown):
    require(needs_observer(spec) and record.get("proof_scope") == SCOPE,
            "observer proof outside exact permitted scope")
    expected = {"schema_version": 1, "suite": "pubsub-scoring", "scenario_id": spec.identifier,
                "acceptance_scenario_id": spec.scenario, "runner_scenario_id": spec.runner_id,
                "case": asdict(spec), "proof_scope": SCOPE, "status": "observed", "errors": [], "cleanup_errors": []}
    require(set(record) == set(expected) | {"original", "shutdown", "evidence"}
            and all(record.get(key) == value for key, value in expected.items()), "invalid split proof schema")
    original, shutdown = record["original"], record["shutdown"]
    require(original["case_token"] != shutdown["case_token"], "independent runs reused token")
    wire, used, pids = validate_original(original, spec, root, binaries, load_json)
    observer_binaries = dict(binaries, rust=binaries["rust-quic-observer"])
    shutdown_used = validate_shutdown(shutdown, spec, Path(root) / "quic-observer", observer_binaries, load_json, None, None)
    shutdown_pids = {value["pid"] for value in shutdown["processes"].values()}
    require(not pids & shutdown_pids and not used & shutdown_used, "independent proofs reused physical owners")
    terminal = validate_case(shutdown)
    require(wire == terminal and record["evidence"] == {"original_wire": wire,
            "instrumented_shutdown": terminal, "original_shutdown": "NOT_PROVEN"},
            "split proof claims differ from independently validated runs")
    return used | shutdown_used, pids | shutdown_pids
