"""Three-process QUIC AUTO DCUtR orchestration; shared dispatch is coordinator-owned.

Required new dispatch: path-live --path-role relay|source|destination,
--case-token, --control-file, --plan-file and the existing lifecycle file flags.
Control files are atomic key=value records, monotonically numbered, not metrics.
No build, manual direct connect, policy bypass or host-network fallback is here.
"""

from dataclasses import asdict, dataclass
import json
from pathlib import Path
import secrets
import time

from path_evidence import (RUST_SOURCE_WAVE_SCOPE, native_terminal, rust_source_wave_case,
                           rust_source_wave_failure, rust_source_wave_pending, validate_case)
from path_network import PathNetwork
from process_lifecycle import StopBudget, enter_scope, exit_scope, spawn_owned

RUNNER_SCENARIO_ID = "quic_stage6/dcutr"


@dataclass(frozen=True)
class Case:
    identifier: str
    source: str
    destination: str
    relay: str
    outcome: str = "success"
    transport: str = "quic"
    profile: str = "native"


def case_specs():
    for source, destination in (("forge", "go"), ("go", "forge"), ("forge", "rust"), ("rust", "forge")):
        for outcome in ("success", "failed", "cancelled"):
            yield Case(f"path.auto_dcutr.{source}_to_{destination}.{outcome}", source, destination,
                       "rust" if "go" in (source, destination) else "go", outcome)


def _write(path, fields):
    if any(not isinstance(v, str) or any(c in v for c in "\r\n\x00") for v in fields.values()):
        raise ValueError("invalid fixture control")
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text("".join(f"{k}={v}\n" for k, v in fields.items()))
    temporary.replace(path)


def _read(path):
    if path.stat().st_size > 4 * 1024 * 1024:
        raise ValueError("path artifact exceeds bounded capture")
    return json.loads(path.read_text())


def _await(owner, path, predicate, deadline):
    while time.monotonic() < deadline:
        if owner.process.poll() is not None:
            raise RuntimeError("path fixture exited before requested observation")
        if path.exists():
            value = _read(path)
            if predicate(value):
                return value
        time.sleep(0.025)
    raise TimeoutError(f"path phase deadline: {path.name}")


def _has(value, kind, **fields):
    return any(e.get("kind") == kind and all(e.get(k) == v for k, v in fields.items())
               for e in value.get("events", []))


def run_case(spec, binaries, root, *, network_factory=PathNetwork, command_attempt=None):
    scope, scope_token = enter_scope()
    work = Path(root).resolve() / spec.identifier
    network, actors, errors, cleanup = network_factory(), {}, [], []
    token = secrets.token_hex(16)
    started, deadline = time.monotonic(), time.monotonic() + 55
    donor_role = "destination" if spec.source == "forge" else "source"
    cancellation_role = donor_role if getattr(spec, donor_role) == "go" else (
        "source" if spec.source == "forge" else "destination")
    artifact = {"schema_version": 1, "suite": "path", "scenario": "dcutr", "case": asdict(spec),
                "scenario_id": spec.identifier, "runner_scenario_id": RUNNER_SCENARIO_ID,
                "acceptance_scenario_ids": ["dcutr"] if spec.outcome == "success" else [],
                "application_actor": donor_role,
                "case_token": token, "raw": {}, "errors": errors, "cleanup_errors": cleanup,
                "status": "NOT_RUN"}
    wave_scope = rust_source_wave_case(artifact["case"])
    if wave_scope:
        artifact["terminal_scope"] = RUST_SOURCE_WAVE_SCOPE
    try:
        work.mkdir(parents=True, exist_ok=False)
        network.setup()

        def launch(role, extra=()):
            implementation = getattr(spec, role)
            files = {name: work / f"{role}.{name}" for name in ("ready", "result", "stop", "control", "plan")}
            argv = [str(Path(binaries[implementation]).resolve()), "path-live", "--scenario", "dcutr",
                    "--transport", "quic", "--path-role", role, "--case-token", token,
                    "--bind-ip", network.addresses[role][0], "--ready-file", str(files["ready"]),
                    "--result-file", str(files["result"]), "--stop-file", str(files["stop"]),
                    "--control-file", str(files["control"]), "--plan-file", str(files["plan"]), *extra]
            if implementation == "forge":
                argv += ["--store-dir", str(work / f"{role}.store")]
            argv = network.namespace_command(role, argv)
            attempt = command_attempt(argv, work / f"{role}.log", spec.identifier, 1, role, 55) if command_attempt else None
            if attempt is not None and not any(a is attempt for a in scope.attempts):
                scope.attempts.append(attempt)
            owner = spawn_owned(argv, work / f"{role}.log", files["stop"], attempt,
                                stop_budget=StopBudget(8, 0, 2))
            actors[role] = (owner, files)
            owner.ready = _await(owner, files["ready"], lambda v: v.get("status") == "ready", deadline)
            binding = "native_public_diagnostics_v1" if implementation == "forge" else "actual_io_and_native_attempt_v1"
            if owner.ready.get("path_bindings") != binding:
                raise RuntimeError(f"{implementation} lacks concrete native observation/no-dial bindings")
            return owner.ready

        relay = launch("relay")
        relay_addr = relay["listen_addrs"][0]
        extra = ["--relay-addr", relay_addr, "--relay-peer-id", relay["peer_id"]]
        destination = launch("destination", extra)
        source = launch("source", extra)
        for role, (_, files) in actors.items():
            other = "destination" if role == "source" else "source"
            _write(files["plan"], {"case-token": token, "peer-id": actors[other][0].ready["peer_id"],
                                    "relay-peer-id": relay["peer_id"], "relay-addr": relay_addr,
                                    "circuit-addr": destination["circuit_addr"], "outcome": spec.outcome})
        sequence = {role: 0 for role in actors}

        def control(role, action):
            sequence[role] += 1
            _write(actors[role][1]["control"], {"sequence": str(sequence[role]), "action": action, "case-token": token})
            wait(role, "control_completed", control_sequence=sequence[role], action=action)

        def wait(role, kind, **fields):
            owner, files = actors[role]
            return _await(owner, files["result"], lambda v: _has(v, kind, **fields), deadline)

        for role in ("source", "destination"):
            control(role, "bind")
            wait(role, "baseline", direct_connection_ids=[])
        if spec.outcome != "success":
            # The fault exists before AUTO starts. It cannot release a packet
            # gate after counters or let an ordinary direct dial win a race.
            network.block_peer_udp()
        control("source", "connect")
        # DCUtR direction is the circuit initiator. The independent application
        # initiator is always the donor, with an observed existing native owner;
        # Forge's general async_open can dial/explicitly upgrade and is not used.
        control(donor_role, "barrier")
        for role in ("source", "destination"):
            wait(role, "echo", phase="relay_before")
        network.capture("barrier")
        if spec.outcome == "cancelled":
            peer = actors["source" if donor_role == "destination" else "destination"][0].ready["peer_id"]
            if wave_scope:
                owner, files = actors[donor_role]
                value = _await(owner, files["result"], lambda v: rust_source_wave_pending(v, token, peer), deadline)
            else:
                value = wait(donor_role, "dcutr_frame", message_type=100)
            if native_terminal(value, getattr(spec, donor_role), peer, "success") or native_terminal(value, getattr(spec, donor_role), peer, "failed"):
                raise RuntimeError("native operation completed before cancellation observation")
            # Observe actual donor I/O, but cancel the real supported owner.
            # In Rust pairs that is Forge, never Rust Swarm/whole-host stop.
            owner, files = actors[cancellation_role]
            owner_value = _read(files["result"])
            owner_peer = actors["source" if cancellation_role == "destination" else "destination"][0].ready["peer_id"]
            if any(native_terminal(owner_value, getattr(spec, cancellation_role), owner_peer, outcome)
                   for outcome in ("success", "failed")):
                raise RuntimeError("cancel owner completed before captured active phase")
            artifact["cancellation"] = {"observed_active_phase": True, "actor": cancellation_role,
                                        "before_request": owner_value, "wire_actor": donor_role,
                                        "wire_before_request": value}
            control(cancellation_role, "cancel")
            if getattr(spec, donor_role) == "rust":
                owner, files = actors[donor_role]
                if wave_scope:
                    artifact["native_wave_failure"] = _await(owner, files["result"],
                        lambda value: rust_source_wave_failure(value, token, peer), deadline)
                else:
                    artifact["cancellation"]["natural_terminal"] = _await(owner, files["result"],
                        lambda value: native_terminal(value, "rust", peer, "failed"), deadline)
        else:
            for role in ("source", "destination"):
                other = "destination" if role == "source" else "source"
                owner, files = actors[role]
                if wave_scope and role == "source":
                    artifact["native_wave_failure"] = _await(owner, files["result"], lambda value: rust_source_wave_failure(
                        value, token, actors[other][0].ready["peer_id"]), deadline)
                else:
                    _await(owner, files["result"], lambda value: native_terminal(
                        value, getattr(spec, role), actors[other][0].ready["peer_id"], spec.outcome), deadline)
        phase = "direct_after" if spec.outcome == "success" else "relay_after"
        control(donor_role, phase)
        for role in ("source", "destination"):
            wait(role, "echo", phase=phase)
        network.capture("after_upgrade")
    except Exception as error:
        errors.append(f"execution: {error}")
    finally:
        try:
            cleanup.extend(scope.close())
            for role, (owner, files) in actors.items():
                actor = {"ready": owner.ready, "process": owner.evidence()}
                if files["result"].exists():
                    try:
                        actor["result"] = _read(files["result"])
                    except (OSError, ValueError) as error:
                        errors.append(f"capture {role}: {error}")
                artifact["raw"][role] = actor
        finally:
            cleanup.extend(network.close())
            artifact["network"] = network.evidence()
            artifact["attempts"] = scope.attempts
            artifact["processes"] = scope.evidence()
            artifact["elapsed_seconds"] = time.monotonic() - started
            exit_scope(scope_token)
    if not errors and not cleanup:
        errors.extend(validate_case(artifact))
    artifact["status"] = "passed" if not errors and not cleanup else "failed"
    return artifact


def run_suite(binaries, root, **support):
    for spec in case_specs():
        yield run_case(spec, binaries, root, **support)
