"""Exact eight-case coordinated reuse registration with shared source trust.

The caller verifies binary hashes and the evidence index before supplying
binary_paths/load_json. Neither embedded status nor a nominal reuse receipt is
an acceptance source. Original commands and actor observations are preserved.
"""

from copy import deepcopy
from dataclasses import asdict
import math
from pathlib import Path

from autorelay_acceptance import ATTEMPT_FIELDS, OWNER_FIELDS, _options, _owned_output, _path, _require
from coordinated_cases import ACTOR_TIMEOUT_MS, CASE_TIMEOUT_SECONDS, case_specs
from coordinated_evidence import REUSE_DIRECTIONS, REUSE_RUNNER_IDS, validate_case
from stage6_evidence_contract import evidence_contract_for

OWNER_ID = "connections.coordinated_dial_port_reuse"
PROFILES = {
    "native": ("coordinated_dial_port_reuse", "native", ("tcp", "yamux"), ()),
    "private": ("coordinated_dial_port_reuse_private_pnet", "private_network", ("tcp", "pnet", "yamux"),
                ("security.private_network_psk",)),
}


def is_record(record):
    return isinstance(record, dict) and (record.get("suite") == "coordinated"
           or record.get("scenario") in {value[0] for value in PROFILES.values()})


def _launch(spec, role, work, record, pnet_key_file, pnet_fingerprint):
    implementation = getattr(spec, role)
    options = {"--scenario": spec.scenario, "--transport": spec.transport,
               "--coord-role": "initiator" if role == "source" else "responder", "--case-token": record["case_token"],
               "--bind-ip": "11.0.0.1" if role == "source" else "11.0.0.2", "--timeout-ms": str(ACTOR_TIMEOUT_MS)}
    options.update({f"--{name}-file": str(work / f"{role}.{name}") for name in ("ready", "result", "stop", "control", "plan")})
    if implementation == "forge":
        options["--store-dir"] = str(work / f"{role}.store")
    if spec.profile == "private":
        _require(isinstance(pnet_key_file, (str, Path)) and isinstance(pnet_fingerprint, str),
                 "trusted private key path and fingerprint are required")
        key = Path(pnet_key_file)
        _require(key.is_absolute() and str(key.resolve()) == str(key), "private key input path is not canonical")
        options.update({"--pnet-key-file": str(key), "--pnet-fingerprint": pnet_fingerprint})
        _require(record["pnet_fingerprint"] == pnet_fingerprint, "record private fingerprint differs from trusted launcher input")
    else:
        _require(record["pnet_fingerprint"] is None, "native case claims a private protector")
    return implementation, work / f"{role}.log", options


def _sources(record, spec, artifact_root, binary_paths, load_json, *, pnet_key_file=None, pnet_fingerprint=None):
    root = Path(artifact_root).resolve()
    work = root / spec.identifier
    expected = {"schema_version": 1, "suite": "coordinated", "case": asdict(spec), "scenario": spec.scenario,
                "scenario_id": spec.identifier, "runner_scenario_id": REUSE_RUNNER_IDS[spec.profile],
                "acceptance_scenario_ids": [spec.scenario], "status": "passed", "errors": [], "cleanup_errors": []}
    _require(isinstance(record, dict) and all(record.get(k) == v for k, v in expected.items()),
             "coordinated identity/claims/status/cleanup differs from exact case")
    _require(set(record) == set(expected) | {"case_token", "pnet_fingerprint", "raw", "plans", "controls", "phases",
             "phase_sources", "network", "attempts", "processes", "elapsed_seconds"}, "missing fields or undeclared coordinated claims")
    elapsed = record["elapsed_seconds"]
    _require(type(elapsed) in (int, float) and math.isfinite(elapsed) and 0 < elapsed <= 90, "unbounded coordinated execution interval")
    raw = record["raw"]
    _require(isinstance(raw, dict) and set(raw) == {"source", "destination"}, "missing exact two native actors")
    _require(isinstance(binary_paths, dict), "indexed binary paths are required")
    for implementation in (spec.source, spec.destination):
        binary = binary_paths[implementation]
        _require(Path(binary).is_absolute() and str(Path(binary).resolve()) == str(binary), "noncanonical indexed binary path")
    participants = record["network"]["participants"]
    _require(isinstance(participants, list) and len(participants) == 2 and all(isinstance(p, dict) for p in participants)
             and {p.get("role") for p in participants} == {"client", "server"}, "missing exact owned namespace participants")
    namespaces = {p["role"]: p["namespace"] for p in participants}
    _require(len(set(namespaces.values())) == 2, "bilateral actors share their namespace")
    hydrated, paths, owners = deepcopy(record), set(), []
    for role, actor in raw.items():
        _require(isinstance(actor, dict) and set(actor) == {"ready", "result", "process"}, "unexpected raw actor shape")
        implementation, log, options = _launch(spec, role, work, record, pnet_key_file, pnet_fingerprint)
        owner = actor["process"]
        _require(isinstance(owner, dict) and set(owner) == OWNER_FIELDS | {"stop_budget"}, "invalid exact coordinated process owner")
        budget, wanted = owner["stop_budget"], {"native_close_seconds": 8, "post_stop_seconds": 0, "scheduler_allowance_seconds": 2, "seconds": 10}
        _require(isinstance(budget, dict) and set(budget) == set(wanted)
                 and all(type(budget[k]) in (int, float) and math.isfinite(budget[k]) and budget[k] == v for k, v in wanted.items()),
                 "invalid exact coordinated native stop budget")
        command = owner.get("command")
        network_role = "client" if role == "source" else "server"
        _require(isinstance(command, list) and len(command) >= 6 and isinstance(command[0], str)
                 and Path(command[0]).is_absolute() and str(Path(command[0]).resolve()) == command[0]
                 and Path(command[0]).name == "ip" and command[1:4] == ["netns", "exec", namespaces[network_role]],
                 "coordinated actor is not inside its exact owned namespace")
        _require(command[4] == str(binary_paths[implementation]) and command[5] == "coordinated-live"
                 and _options(command[4:]) == options, "coordinated binary/argv differs from exact native case")
        for name in ("control", "plan"):
            _path(options[f"--{name}-file"], work)
        # The shared verifier expects unwrapped argv and its original owner
        # shape. Validate the namespace and budget above, then strip only those
        # mechanics from a copy, never from the original process evidence.
        view = deepcopy(actor)
        view["process"].pop("stop_budget")
        view["process"]["command"] = command[4:]
        loaded = _owned_output(view, implementation, "coordinated-live", log, options, work,
                               binary_paths, load_json, paths)
        hydrated["raw"][role].update(loaded)
        owners.append((role, owner))
    phases, sources = record["phases"], record["phase_sources"]
    _require(isinstance(phases, dict) and isinstance(sources, dict)
             and set(phases) == set(sources) == {"started", "connected", "exchanged"}, "missing exact indexed native phases")
    for phase in ("started", "connected", "exchanged"):
        _require(isinstance(phases[phase], dict) and isinstance(sources[phase], dict)
                 and set(phases[phase]) == set(sources[phase]) == {"source", "destination"}, "missing bilateral indexed phase")
        for role in ("source", "destination"):
            snapshot = str(work / f"{role}.{phase}.json")
            _require(sources[phase][role] == snapshot, "phase source differs from exact owned immutable capture")
            path = _path(snapshot, work)
            _require(path not in paths, "phase reused a final output or another native phase")
            paths.add(path)
            value = load_json(snapshot)
            _require(isinstance(value, dict) and value == phases[phase][role], "embedded native phase differs from indexed raw snapshot")
            hydrated["phases"][phase][role] = value
    processes, attempts = record["processes"], record["attempts"]
    _require(isinstance(processes, list) and len(processes) == 2
             and all(isinstance(p, dict) and type(p.get("pid")) is int and p["pid"] > 0 for p in processes)
             and len({p["pid"] for p in processes}) == 2 and all(sum(p == owner for p in processes) == 1 for _, owner in owners),
             "missing/duplicate canonical coordinated process owner")
    _require(isinstance(attempts, list) and len(attempts) == 2, "missing exact two tracked native execution attempts")
    checked = set()
    for role, owner in owners:
        matches = [(i, a) for i, a in enumerate(attempts) if isinstance(a, dict) and a.get("pid") == owner["pid"]]
        _require(len(matches) == 1, "missing/duplicate exact coordinated owned attempt")
        index, attempt = matches[0]
        _require(set(attempt) in (ATTEMPT_FIELDS, ATTEMPT_FIELDS | {"log_tail"})
                 and all(attempt.get(k) == owner[k] for k in ("pid", "command", "log_file", "terminal_status", "outputs"))
                 and type(attempt.get("pid")) is int and type(attempt.get("exit_code")) is int and attempt["exit_code"] == 0
                 and type(attempt.get("attempt_id")) is int and attempt["attempt_id"] == 1
                 and attempt.get("requested_log_file") == owner["log_file"] and attempt.get("kind") == role
                 and attempt.get("scenario_id") == spec.identifier
                 and type(attempt.get("timeout_seconds")) in (int, float) and attempt["timeout_seconds"] == CASE_TIMEOUT_SECONDS,
                 "attempt differs from exact successful coordinated owner")
        checked.add(index)
    _require(checked == {0, 1}, "undeclared coordinated execution attempt")
    return hydrated


def validate_record(record, spec, artifact_root, binary_paths, load_json, **private_inputs):
    """Hydrate through the shared indexed owner verifier, then reuse semantics."""
    try:
        _require(callable(load_json), "indexed raw JSON loader is required")
        hydrated = _sources(record, spec, artifact_root, binary_paths, load_json, **private_inputs)
        return validate_case(hydrated)
    except (ValueError, TypeError, KeyError, IndexError, OSError, RuntimeError, AttributeError) as error:
        return [str(error)]


def _requirements(required):
    _require(isinstance(required, dict) and set(required) == {(OWNER_ID, value[0]) for value in PROFILES.values()},
             "coordinated manifest must require separate native/private four-direction scenarios")
    for profile, (scenario, manifest_profile, stack, capabilities) in PROFILES.items():
        value = required[(OWNER_ID, scenario)]
        _require(isinstance(value, (tuple, list)) and len(value) == 7
                 and isinstance(value[0], (set, list, tuple)) and set(value[0]) == REUSE_DIRECTIONS
                 and isinstance(value[3], (tuple, list)) and isinstance(value[5], (tuple, list))
                 and (value[1], value[2], tuple(value[3]), value[4], tuple(value[5]), value[6])
                 == ("passed", manifest_profile, stack, REUSE_RUNNER_IDS[profile], capabilities, evidence_contract_for(scenario)),
                 "coordinated manifest profile/stack/directions/contract differs from exact eight-case requirements")


def validate_suite(records, required, artifact_root, binary_paths, load_json, *, pnet_key_file=None, pnet_fingerprint=None):
    """Eight cases only; caller supplies trusted indexed sources/private inputs."""
    specs = {s.identifier: s for s in case_specs()}
    try:
        _requirements(required)
        _require(isinstance(records, list) and len(records) == 8 and all(isinstance(r, dict) and isinstance(r.get("scenario_id"), str) for r in records)
                 and {r["scenario_id"] for r in records} == set(specs), "coordinated receipt must cover all eight cases exactly once")
        _require(callable(load_json), "indexed raw JSON loader is required")
    except (ValueError, TypeError, KeyError) as error:
        return [str(error)]
    errors, pids, paths = [], set(), set()
    for record in records:
        spec = specs[record["scenario_id"]]
        used = set()
        def load(value):
            path = _path(value, Path(artifact_root).resolve() / spec.identifier)
            used.add(path)
            return load_json(value)
        try:
            hydrated = _sources(record, spec, artifact_root, binary_paths, load,
                                pnet_key_file=pnet_key_file, pnet_fingerprint=pnet_fingerprint)
            errors.extend(f"{spec.identifier}: {e}" for e in validate_case(hydrated))
            owned = {p["pid"] for p in record["processes"]}
            _require(not owned & pids and not used & paths, "coordinated cases reuse PIDs or indexed snapshots")
            pids.update(owned)
            paths.update(used)
        except (ValueError, TypeError, KeyError, IndexError, OSError, RuntimeError, AttributeError) as error:
            errors.append(f"{spec.identifier}: {error}")
    return errors
