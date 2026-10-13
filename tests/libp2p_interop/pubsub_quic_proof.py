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
    return complete_scoped_case(original, spec, binaries, root, observer_binary, suite="pubsub-scoring",
        eligible=needs_observer, original_validator=validate_original, run_shutdown=run_case,
        terminal_validator=validate_case, command_attempt=command_attempt)


def _split_evidence(wire, terminal, suite):
    require(suite in {"pubsub-scoring", "pubsub-extensions"}, "unknown split proof suite")
    if suite == "pubsub-scoring":
        require(wire == terminal, "independent observer scenario differs from original traffic contract")
    # PR12 results contain actual per-run IDs/bytes/sequences. Each validator
    # proves its complete case independently; receipts are never normalized.
    return {"original_wire": wire, "instrumented_shutdown": terminal, "original_shutdown": "NOT_PROVEN"}


def complete_scoped_case(original, spec, binaries, root, observer_binary, *, suite, eligible,
                         original_validator, run_shutdown, terminal_validator, command_attempt=None):
    """Trusted suite adapters, not artifact-selected policies or borrowed causes."""
    result = {"schema_version": 1, "suite": suite, "scenario_id": spec.identifier,
              "acceptance_scenario_id": spec.scenario, "runner_scenario_id": spec.runner_id,
              "case": asdict(spec), "proof_scope": SCOPE, "original": original,
              "shutdown": None, "status": "HARNESS_ERROR", "errors": [], "cleanup_errors": []}
    active_valid = False
    try:
        require(eligible(spec), "QUIC observer outside exact Rust matrix")
        require(observer_binary is not None, "observer binary missing")
        if suite == "pubsub-extensions":
            require(Path(observer_binary).resolve() != Path(binaries["rust"]).resolve(),
                    "observer binary aliases original Rust")
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
        wire, _, original_pids = original_validator(original, spec, root, binaries, _snapshot)
        active_valid = True
        observer_binaries = dict(binaries, rust=observer_binary)
        shutdown = run_shutdown(spec, observer_binaries, Path(root) / "quic-observer", command_attempt=command_attempt)
        result["shutdown"] = shutdown
        require(original["case_token"] != shutdown["case_token"], "observer reused original run token")
        terminal = terminal_validator(shutdown)
        if suite == "pubsub-extensions":
            require(not original_pids & {owner["pid"] for owner in shutdown["processes"].values()},
                    "independent proofs reused physical owners")
        result["evidence"] = _split_evidence(wire, terminal, suite)
        result["status"] = "observed"
    except (ValueError, TypeError, KeyError, IndexError, AttributeError, OSError, RuntimeError) as error:
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
    implementations = {role: spec.destination if role == "victim" else spec.source
                       for role in ("victim", "offender", "replacement", "sink")}
    snapshots, used, pids = original_sources(record, spec, root, binaries, load_json,
                                            suite="pubsub-scoring", implementations=implementations)
    return validate_active_case(record, snapshots), used, pids


def original_sources(record, spec, root, binaries, load_json, *, suite, implementations, extensions=None):
    """One indexed original-process binder for the two explicit proof scopes."""
    require(record.get("case") == asdict(spec) and record.get("scenario_id") == spec.identifier
            and record.get("suite") == suite, "original scenario differs from exact matrix")
    work = Path(root).resolve() / spec.identifier
    actors, processes = record.get("raw"), record.get("processes")
    require(isinstance(actors, dict) and isinstance(processes, dict)
            and set(actors) == set(processes) == {"victim", "offender", "replacement", "sink"},
            "original lacks four actual native process owners")
    require(set(implementations) == set(actors) and (extensions is None or set(extensions) == set(actors)),
            "original actor mapping differs")
    used, pids = set(), set()
    for role, raw in actors.items():
        implementation = implementations[role]
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
        if extensions is not None:
            options["--extension"] = extensions[role]
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
        implementation = implementations[role]
        require(_ready(processes[role]["ready"], snapshots[role].get("result"), implementation,
                       role, record["case_token"]), "original active readiness/identity differs")
        if extensions is not None:
            require(all(document.get("extension") == extensions[role]
                        and document.get("requests_partial") is (extensions[role] == "partial")
                        for document in (processes[role]["ready"], snapshots[role]["result"], actors[role])),
                    "original readiness/capture extension differs")
        used.add(path)
    return snapshots, used, pids


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
            if record.get("suite") == "pubsub-extensions":
                from pubsub_extension_acceptance import needs_observer as extension_needs_observer, validate_original as extension_original
                from pubsub_extension_cases import case_specs as extension_specs
                selected = {spec.identifier: spec for spec in extension_specs() if extension_needs_observer(spec)}
                spec, validate = selected[record["scenario_id"]], extension_original
            else:
                spec, validate = specs[record["scenario_id"]], validate_original
            original = record["original"]
            validate(original, spec, root, binaries, _snapshot)
            for owner in original["processes"].values():
                path = Path(owner["log_file"]).resolve()
                require(path not in owners, "original diagnostic stdout owner reused")
                owners[path] = owner
        except (ValueError, TypeError, KeyError, IndexError, AttributeError, OSError, RuntimeError):
            continue
    return owners


def validate_split(record, spec, root, binaries, load_json, validate_shutdown):
    return validate_scoped_split(record, spec, root, binaries, load_json, validate_shutdown,
        suite="pubsub-scoring", eligible=needs_observer, original_validator=validate_original,
        terminal_validator=validate_case)


def validate_scoped_split(record, spec, root, binaries, load_json, validate_shutdown, *, suite, eligible,
                           original_validator, terminal_validator):
    require(eligible(spec) and record.get("proof_scope") == SCOPE,
            "observer proof outside exact permitted scope")
    if suite == "pubsub-extensions":
        require(type(record.get("schema_version")) is int, "invalid extension split schema version")
    expected = {"schema_version": 1, "suite": suite, "scenario_id": spec.identifier,
                "acceptance_scenario_id": spec.scenario, "runner_scenario_id": spec.runner_id,
                "case": asdict(spec), "proof_scope": SCOPE, "status": "observed", "errors": [], "cleanup_errors": []}
    require(set(record) == set(expected) | {"original", "shutdown", "evidence"}
            and all(record.get(key) == value for key, value in expected.items()), "invalid split proof schema")
    original, shutdown = record["original"], record["shutdown"]
    require(original["case_token"] != shutdown["case_token"], "independent runs reused token")
    wire, used, pids = original_validator(original, spec, root, binaries, load_json)
    if suite == "pubsub-extensions":
        require(Path(binaries["rust-quic-observer"]).resolve() != Path(binaries["rust"]).resolve(),
                "observer binary aliases original Rust")
    observer_binaries = dict(binaries, rust=binaries["rust-quic-observer"])
    shutdown_used = validate_shutdown(shutdown, spec, Path(root) / "quic-observer", observer_binaries, load_json, None, None)
    shutdown_pids = {value["pid"] for value in shutdown["processes"].values()}
    require(not pids & shutdown_pids and not used & shutdown_used, "independent proofs reused physical owners")
    terminal = terminal_validator(shutdown)
    require(record["evidence"] == _split_evidence(wire, terminal, suite),
            "split proof claims differ from independently validated runs")
    return used | shutdown_used, pids | shutdown_pids
