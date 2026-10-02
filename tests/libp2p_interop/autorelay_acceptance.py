"""Canonical PR9 execution registration, never a production support claim."""

from copy import deepcopy
from pathlib import Path

from autorelay_cases import case_specs
from autorelay_evidence import validate_case
from stage6_evidence_contract import evidence_contract_for


ROLE_DIRECTIONS = {
    "lifecycle": {"forge_to_go", "forge_to_rust"},
    "service": {"go_to_forge", "rust_to_forge"},
}
SCENARIOS = {
    f"{base}{suffix}": (owner, role, transport, profile)
    for owner, role, base in (
        ("relay.autorelay_lifecycle", "lifecycle", "autorelay_lifecycle"),
        ("relay.circuit_v2_service", "service", "relay_v2_service"),
    )
    for suffix, transport, profile in (
        ("", "quic", "quic_stage6"),
        ("_native_tcp_yamux", "tcp", "tcp_stage6"),
        ("_native_tcp_tls_yamux", "tcp-tls", "tcp_tls_stage6"),
    )
}
EVIDENCE_CONTRACTS = {evidence_contract_for(name) for name in SCENARIOS}
OWNER_FIELDS = {"pid", "command", "log_file", "terminal_status", "ready", "outputs"}
ATTEMPT_FIELDS = {
    "kind", "scenario_id", "attempt_id", "command", "requested_log_file", "timeout_seconds",
    "exit_code", "log_file", "pid", "terminal_status", "outputs",
}


def acceptance_id(spec):
    return next(name for name, (_, role, transport, _) in SCENARIOS.items()
                if (role, transport) == (spec.kind, spec.transport))


def claims_for(spec):
    return [acceptance_id(spec)]


def is_record(record):
    return isinstance(record, dict) and (
        record.get("suite") == "autorelay" or record.get("scenario") == "autorelay"
    )


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _options(command):
    _require(isinstance(command, list) and len(command) >= 2 and len(command[2:]) % 2 == 0
             and all(isinstance(arg, str) and arg for arg in command), "invalid fixture command")
    options = dict(zip(command[2::2], command[3::2]))
    _require(len(options) * 2 == len(command[2:]), "duplicate fixture flags")
    return options


def _path(value, work):
    _require(isinstance(value, str) and Path(value).is_absolute()
             and Path(value).resolve().is_relative_to(work.resolve()), "process path escapes exact case")
    _require(str(Path(value).resolve()) == value, "noncanonical process path")
    return Path(value)


def _actor_launch(spec, label, work, raw):
    common = {"--scenario": "autorelay", "--transport": spec.transport,
              "--ready-file": str(work / f"{label}-ready.json"),
              "--stop-file": str(work / f"{label}.stop"),
              "--result-file": str(work / f"{label}.json")}
    if label == "destination" and spec.kind == "service":
        impl = spec.destination
        return impl, "destination", work / f"{impl}-destination.log", {
            "--scenario": "autorelay", "--transport": spec.transport,
            "--ready-file": str(work / f"{impl}-destination-ready.json"),
            "--stop-file": str(work / f"{impl}-destination.stop"),
            "--store-dir": str(work / f"{impl}-destination-store"),
            "--relay-peer-id": raw["relay"]["ready"]["peer_id"],
            "--relay-addr": raw["relay"]["ready"]["listen_addrs"][0],
        }
    if label in ("relay", "replacement"):
        impl = spec.relay if label == "relay" else ("rust" if spec.relay == "go" else "go")
        action = "autorelay-service" if impl == "forge" else "autorelay-relay"
        if impl != "forge":
            common["--relay-ttl-seconds"] = "8"
    elif label == "destination":
        impl, action = "forge", "autorelay-destination"
        common["--seed-file"] = str(work / "seeds.txt")
    else:
        impl, action = spec.source, "autorelay-observe"
        common.update({"--addr": raw["destination"]["ready"]["listen_addrs"][0],
                       "--peer-id": raw["destination"]["ready"]["peer_id"],
                       "--probe-file": str(work / "observer-probe.txt")})
    if impl == "forge":
        common["--store-dir"] = str(work / f"{label}-store")
    return impl, action, work / f"{label}.log", common


def _owned_output(actor, impl, action, log, options, work, binaries, load_json, used_paths):
    owner = actor.get("process")
    _require(isinstance(owner, dict) and set(owner) == OWNER_FIELDS, "invalid exact process owner")
    _require(type(owner.get("pid")) is int and owner["pid"] > 0, "invalid owned PID")
    terminal = owner.get("terminal_status")
    _require(terminal == {"exit_code": 0, "termination": "graceful"}
             and type(terminal["exit_code"]) is int, "process lacks final graceful joined shutdown")
    command = owner.get("command")
    actual = _options(command)
    _require(command[:2] == [str(binaries[impl]), action] and actual == options,
             "fixture binary/action/options differ from exact case launch")
    _require(owner.get("log_file") == str(log), "log path differs from exact actor")
    paths = {_path(str(log), work)}
    for flag in ("--ready-file", "--result-file", "--stop-file", "--store-dir", "--seed-file", "--probe-file"):
        if flag in options:
            _path(options[flag], work)
    outputs = owner.get("outputs")
    flags = [flag for flag in ("--ready-file", "--result-file") if flag in options]
    _require(isinstance(outputs, list) and len(outputs) == len(flags), "missing final immutable outputs")
    loaded = {}
    for output, flag in zip(outputs, flags):
        snapshot = str(log) + f".{flag[2:]}.json"
        _require(isinstance(output, dict) and output.get("exists") is True
                 and output == {"argument": flag, "path": options[flag], "exists": True, "log_file": snapshot},
                 "output path differs from exact terminal snapshot")
        paths.add(_path(snapshot, work))
        payload = load_json(snapshot)
        key = "ready" if flag == "--ready-file" else "result"
        _require(isinstance(payload, dict) and actor.get(key) == payload,
                 "embedded payload differs from indexed final snapshot")
        loaded[key] = payload
        if key == "ready":
            _require(owner.get("ready") == payload, "process readiness differs from indexed snapshot")
    if "--ready-file" not in options:
        _require(owner.get("ready") == {}, "dialer has an undeclared readiness payload")
    _require(not paths & used_paths, "actors reuse indexed process snapshots/logs")
    used_paths.update(paths)
    return loaded


def validate_record(record, spec, root, binaries, load_indexed_json):
    """Load only indexed final snapshots, then apply the existing semantics."""
    expected = {"scenario": "autorelay", "scenario_id": spec.identifier, "status": "passed",
                "case": {"source": spec.source, "relay": spec.relay, "destination": spec.destination,
                         "transport": spec.transport, "kind": spec.kind}, "errors": [], "cleanup_errors": []}
    _require(isinstance(record, dict) and all(record.get(key) == value for key, value in expected.items()),
             "case identity/status/cleanup mismatch")
    _require(record.get("suite", "autorelay") == "autorelay", "wrong declared suite")
    _require(set(record) == set(expected) | {"acceptance_scenario_ids", "raw", "echoes", "faults",
                                           "processes", "attempts", "elapsed_seconds"} | ({"suite"} & set(record)),
             "case has missing fields or undeclared claims")
    _require(record.get("acceptance_scenario_ids") in ([], claims_for(spec)), "wrong Forge role/transport claims")
    elapsed = record.get("elapsed_seconds")
    _require(type(elapsed) in (int, float) and 0 < elapsed <= 120, "unbounded case interval")
    work = root / spec.identifier
    raw = record.get("raw")
    labels = {"relay", "destination", "observer", "replacement"} if spec.kind == "lifecycle" else {"relay", "destination"}
    _require(isinstance(raw, dict) and set(raw) == labels, "missing or extra case actors")
    echoes = record.get("echoes")
    phases = {"acquired_echo", "renewed_echo", "replacement_echo"} if spec.kind == "lifecycle" else {"service_echo"}
    _require(isinstance(echoes, list) and len(echoes) == len(phases)
             and all(isinstance(e, dict) and set(e) == {"phase", "result_file", "result", "process"} for e in echoes)
             and {e["phase"] for e in echoes} == phases, "missing or duplicate independent echoes")
    hydrated, paths, owners = deepcopy(record), set(), []
    for label, actor in raw.items():
        keys = {"ready", "process"} if label == "destination" and spec.kind == "service" else {"ready", "result", "process"}
        _require(isinstance(actor, dict) and set(actor) == keys, "unexpected actor payload shape")
        impl, action, log, options = _actor_launch(spec, label, work, raw)
        hydrated["raw"][label].update(_owned_output(
            actor, impl, action, log, options, work, binaries, load_indexed_json, paths))
        owners.append((label, actor["process"]))
    for index, echo in enumerate(echoes):
        phase, result, impl = echo["phase"], echo["result"], spec.source
        echo_work = work / phase
        base = echo_work / f"{impl}-relay-dial-autorelay"
        result_file, log = str(base) + ".json", Path(str(base) + ".log")
        _require(echo.get("result_file") == result_file, "echo result path differs from exact case")
        options = {"--scenario": "autorelay", "--transport": spec.transport,
                   "--peer-id": raw["destination"]["ready"]["peer_id"], "--relay-peer-id": result["relay_peer"],
                   "--relay-addr": result["relayed_addr"].split("/p2p-circuit")[0],
                   "--result-file": result_file, "--store-dir": str(base) + "-store"}
        loaded = _owned_output(echo, impl, "dial-relay", log, options, work, binaries, load_indexed_json, paths)
        _require(load_indexed_json(result_file) == loaded["result"], "echo raw result differs from indexed snapshot")
        paths.add(_path(result_file, work))
        hydrated["echoes"][index].update(loaded)
        owners.append((phase, echo["process"]))
    processes, attempts = record.get("processes"), record.get("attempts")
    _require(isinstance(processes, list) and len(processes) == len(owners)
             and all(isinstance(p, dict) and type(p.get("pid")) is int and p["pid"] > 0
                     and isinstance(p.get("terminal_status"), dict)
                     and type(p["terminal_status"].get("exit_code")) is int for p in processes)
             and len({p.get("pid") for p in processes}) == len(owners), "missing/duplicate owned processes or PIDs")
    _require(all(sum(p == owner for p in processes) == 1 for _, owner in owners),
             "embedded process differs from canonical owned process/PID")
    _require(isinstance(attempts, list), "missing execution attempts")
    checked = set()
    for label, owner in owners:
        matches = [(i, a) for i, a in enumerate(attempts) if isinstance(a, dict) and a.get("pid") == owner["pid"]]
        if label == "destination" and spec.kind == "service":
            _require(not matches, "service destination has an undeclared attempt")
            continue
        _require(len(matches) == 1, "missing/duplicate exact owned attempt")
        index, attempt = matches[0]
        echo = label in phases
        _require(set(attempt) in (ATTEMPT_FIELDS, ATTEMPT_FIELDS | {"log_tail"})
                 and type(attempt.get("pid")) is int
                 and isinstance(attempt.get("terminal_status"), dict)
                 and type(attempt["terminal_status"].get("exit_code")) is int
                 and all(attempt.get(key) == owner[key] for key in ("pid", "command", "log_file", "terminal_status", "outputs"))
                 and type(attempt.get("exit_code")) is int and attempt["exit_code"] == 0
                 and type(attempt.get("attempt_id")) is int and attempt["attempt_id"] == 1
                 and attempt.get("requested_log_file") == owner["log_file"]
                 and attempt.get("kind") == ("relay_dial" if echo else label)
                 and attempt.get("scenario_id") == ("autorelay" if echo else spec.identifier)
                 and type(attempt.get("timeout_seconds")) in (int, float)
                 and attempt["timeout_seconds"] == (15 if echo else 60), "attempt differs from exact successful owner")
        checked.add(index)
    _require(checked == set(range(len(attempts))), "undeclared or failed execution attempt")
    return validate_case(hydrated)


def validate_suite(records, required, root, binaries, load_indexed_json):
    specs = case_specs()
    expected = {spec.identifier: spec for spec in specs}
    canonical = {f"autorelay-{kind}-{transport}-{source}-{relay}-{dest}"
                 for transport in ("quic", "tcp", "tcp-tls")
                 for source, relay, dest, kind in (("go", "go", "forge", "lifecycle"),
                                                  ("rust", "rust", "forge", "lifecycle"),
                                                  ("go", "forge", "rust", "service"),
                                                  ("rust", "forge", "go", "service"))}
    if len(specs) != 12 or set(expected) != canonical:
        return ["canonical AutoRelay matrix must contain exactly 12 unique role/transport cases"]
    if (not isinstance(records, list) or len(records) != 12
            or any(not isinstance(r, dict) or not isinstance(r.get("scenario_id"), str) for r in records)
            or {r["scenario_id"] for r in records} != canonical):
        return ["AutoRelay receipt must cover all 12 cases exactly once"]
    errors, claims, paths, pids = [], set(), set(), set()
    for record in records:
        spec = expected[record["scenario_id"]]
        used = set()

        def load(value):
            path = _path(value, root / spec.identifier)
            used.add(path)
            return load_indexed_json(value)

        try:
            errors.extend(f"{spec.identifier}: {error}" for error in validate_record(
                record, spec, root, binaries, load))
            owned_pids = {p["pid"] for p in record["processes"]}
            _require(not owned_pids & pids, "cases reuse owned process PIDs")
            pids.update(owned_pids)
            _require(not used & paths, "cases reuse indexed snapshots")
            paths.update(used)
        except (ValueError, TypeError, KeyError, IndexError, OSError, RuntimeError, AttributeError, StopIteration) as error:
            errors.append(f"{spec.identifier}: {error}")
        name = acceptance_id(spec)
        # Lifecycle attribution is the reservation direction, not the echo source.
        direction = f"forge_to_{spec.relay}" if spec.kind == "lifecycle" else f"{spec.source}_to_forge"
        claims.add((SCENARIOS[name][0], name, direction))
    required_claims = {(owner, name, direction) for (owner, name), value in required.items() for direction in value[0]}
    if claims != required_claims:
        errors.append("AutoRelay role/transport claims differ from exact manifest requirements")
    return errors
