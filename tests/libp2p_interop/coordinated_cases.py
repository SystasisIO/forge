"""Two native actors, one owned TCP barrier, and one retained-connection probe.

This module neither builds actors nor supplies sockets, upgrades or receipts.
Dispatch and acceptance aggregation remain owned by the main runner.
"""

from dataclasses import asdict, dataclass
import json
from pathlib import Path
import secrets
import time

from coordinated_evidence import REUSE_RUNNER_IDS, started_receipt, validate_case, validate_connected, validate_ready
from coordinated_network import CoordinatedNetwork
from process_lifecycle import StopBudget, enter_scope, exit_scope, spawn_owned

ACTOR_TIMEOUT_MS = 20000
CASE_TIMEOUT_SECONDS = 55
SCENARIO = "coordinated_dial_port_reuse"
PRIVATE_SCENARIO = SCENARIO + "_private_pnet"


@dataclass(frozen=True)
class Case:
    identifier: str
    source: str
    destination: str
    profile: str = "native"
    transport: str = "tcp"

    @property
    def scenario(self):
        return PRIVATE_SCENARIO if self.profile == "private" else SCENARIO


def case_specs():
    for profile, transport in (("native", "tcp"), ("private", "tcp-pnet-noise")):
        for source, destination in (("forge", "go"), ("go", "forge"), ("forge", "rust"), ("rust", "forge")):
            yield Case(f"coordinated.{profile}.{source}_to_{destination}", source, destination, profile, transport)


def _check_spec(spec):
    if spec not in tuple(case_specs()):
        raise ValueError("only the eight owned bilateral coordinated TCP cases are supported")


def _write(path, fields):
    if any(not isinstance(v, str) or any(c in v for c in "\r\n\x00") for v in fields.values()):
        raise ValueError("invalid coordinated fixture control")
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text("".join(f"{k}={v}\n" for k, v in fields.items()))
    temporary.replace(path)


def _payload(path):
    if path.stat().st_size > 4 * 1024 * 1024:
        raise ValueError("coordinated artifact exceeds bounded capture")
    payload = path.read_bytes()
    if len(payload) > 4 * 1024 * 1024:
        raise ValueError("coordinated artifact exceeds bounded capture")
    value = json.loads(payload)
    if not isinstance(value, dict):
        raise ValueError("coordinated actor output must be an object")
    return value, payload


def _read(path):
    return _payload(path)[0]


def _await(owner, path, predicate, deadline, *, capture=None):
    while time.monotonic() < deadline:
        if owner.process.poll() is not None:
            raise RuntimeError("coordinated fixture exited before requested observation")
        if path.is_file():
            value, payload = _payload(path)
            if value.get("status") == "error":
                raise RuntimeError(f"actor failed: {value.get('error', 'no terminal detail')}")
            if predicate(value):
                if capture is not None:
                    with capture.open("xb") as snapshot:
                        snapshot.write(payload)
                return value
        time.sleep(0.025)
    raise TimeoutError(f"coordinated phase deadline: {path.name}")


def run_case(spec, binaries, root, *, pnet_key_file=None, pnet_fingerprint=None,
             network_factory=CoordinatedNetwork, command_attempt=None):
    _check_spec(spec)
    if spec.profile == "private":
        if (not pnet_key_file or not Path(pnet_key_file).is_file()
                or not isinstance(pnet_fingerprint, str) or len(pnet_fingerprint) != 64
                or any(c not in "0123456789abcdef" for c in pnet_fingerprint)):
            raise ValueError("private coordination requires an existing key file and operational fingerprint")
    elif pnet_key_file is not None or pnet_fingerprint is not None:
        raise ValueError("native coordination must not receive private key inputs")
    work = Path(root).resolve() / spec.identifier
    scope, scope_token = enter_scope()
    network, actors, errors, cleanup = None, {}, [], []
    token, started = secrets.token_hex(16), time.monotonic()
    deadline = started + CASE_TIMEOUT_SECONDS
    artifact = {"schema_version": 1, "suite": "coordinated", "case": asdict(spec),
                "scenario": spec.scenario, "scenario_id": spec.identifier,
                "runner_scenario_id": REUSE_RUNNER_IDS[spec.profile],
                "acceptance_scenario_ids": [spec.scenario], "case_token": token,
                "pnet_fingerprint": pnet_fingerprint, "raw": {}, "plans": {}, "controls": [],
                "phases": {"started": {}, "connected": {}, "exchanged": {}},
                "phase_sources": {"started": {}, "connected": {}, "exchanged": {}},
                "errors": errors, "cleanup_errors": cleanup, "status": "NOT_RUN"}
    try:
        work.mkdir(parents=True, exist_ok=False)
        network = network_factory()
        network.setup()
        for role, network_role in (("source", "client"), ("destination", "server")):
            implementation = getattr(spec, role)
            files = {name: work / f"{role}.{name}" for name in ("ready", "result", "stop", "control", "plan")}
            argv = [str(Path(binaries[implementation]).resolve()), "coordinated-live",
                    "--scenario", spec.scenario, "--transport", spec.transport,
                    "--coord-role", "initiator" if role == "source" else "responder",
                    "--case-token", token, "--bind-ip", network.addresses[network_role][0],
                    "--ready-file", str(files["ready"]), "--result-file", str(files["result"]),
                    "--stop-file", str(files["stop"]), "--control-file", str(files["control"]),
                    "--plan-file", str(files["plan"]), "--timeout-ms", str(ACTOR_TIMEOUT_MS)]
            if implementation == "forge":
                argv += ["--store-dir", str(work / f"{role}.store")]
            if spec.profile == "private":
                argv += ["--pnet-key-file", str(Path(pnet_key_file).resolve()),
                         "--pnet-fingerprint", pnet_fingerprint]
            argv = network.namespace_command(network_role, argv)
            attempt = command_attempt(argv, work / f"{role}.log", spec.identifier, 1, role,
                                      CASE_TIMEOUT_SECONDS) if command_attempt else None
            if attempt is not None and not any(a is attempt for a in scope.attempts):
                scope.attempts.append(attempt)
            owner = spawn_owned(argv, work / f"{role}.log", files["stop"], attempt,
                                stop_budget=StopBudget(8, 0, 2))
            actors[role] = (owner, files)
            owner.ready = _await(owner, files["ready"], lambda v: v.get("status") == "ready",
                                 min(deadline, time.monotonic() + 15))
            failures = validate_ready(owner.ready, implementation, token, network.addresses[network_role][0])
            if failures:
                raise RuntimeError("; ".join(failures))
        if actors["source"][0].ready["peer_id"] == actors["destination"][0].ready["peer_id"]:
            raise RuntimeError("coordinated actors must have distinct native identities")
        for role, (_, files) in actors.items():
            other = "destination" if role == "source" else "source"
            ready = actors[other][0].ready
            plan = {"case-token": token, "peer-id": ready["peer_id"], "addr": ready["listen_addrs"][0]}
            _write(files["plan"], plan)
            artifact["plans"][role] = plan
        network.arm({"client": actors["source"][0].ready["listener_port"],
                     "server": actors["destination"][0].ready["listener_port"]})

        def control(role, action, sequence):
            fields = {"sequence": str(sequence), "action": action, "case-token": token}
            _write(actors[role][1]["control"], fields)
            artifact["controls"].append({"actor": role, **fields})

        # Publish both starts before waiting for either acknowledgement. These
        # acknowledgements are not evidence of a completed native connection.
        native_deadline = min(deadline, time.monotonic() + ACTOR_TIMEOUT_MS / 1000)
        for role in ("source", "destination"):
            control(role, "start", 1)
        for role, (owner, files) in actors.items():
            snapshot = work / f"{role}.started.json"
            artifact["phases"]["started"][role] = _await(
                owner, files["result"], lambda v, r=role: started_receipt(v, getattr(spec, r), token),
                native_deadline, capture=snapshot)
            artifact["phase_sources"]["started"][role] = str(snapshot)
        network.await_syn_sent(native_deadline)
        for owner, _ in actors.values():
            if owner.process.poll() is not None:
                raise RuntimeError("native actor retired before barrier release")
        network.release()
        for role, (owner, files) in actors.items():
            snapshot = work / f"{role}.connected.json"
            value = _await(owner, files["result"], lambda v: v.get("status") == "connected", native_deadline, capture=snapshot)
            artifact["phase_sources"]["connected"][role] = str(snapshot)
            other = "destination" if role == "source" else "source"
            failures = validate_connected(value, getattr(spec, role), token, owner.ready,
                                          actors[other][0].ready, role)
            if failures:
                raise RuntimeError("; ".join(failures))
            artifact["phases"]["connected"][role] = value
        control("source", "probe", 2)
        for role, (owner, files) in actors.items():
            snapshot = work / f"{role}.exchanged.json"
            artifact["phases"]["exchanged"][role] = _await(
                owner, files["result"], lambda v: v.get("status") == "exchanged",
                min(deadline, time.monotonic() + 15), capture=snapshot)
            artifact["phase_sources"]["exchanged"][role] = str(snapshot)
    except Exception as error:
        errors.append(f"execution: {error}")
    finally:
        try:
            # Host stop is terminal cleanup only, never cancellation/fallback proof.
            for owner, _ in actors.values():
                try:
                    owner.request_stop()
                except OSError as error:
                    cleanup.append(f"stop request: {error}")
            cleanup.extend(scope.close())
            for role, (owner, files) in actors.items():
                for attempt in scope.attempts:
                    if attempt.get("pid") == owner.process.pid:
                        attempt["exit_code"] = owner.terminal_status.get("exit_code")
                actor = {"ready": owner.ready, "process": owner.evidence()}
                if files["result"].is_file():
                    try:
                        actor["result"] = _read(files["result"])
                    except (OSError, ValueError) as error:
                        errors.append(f"capture {role}: {error}")
                artifact["raw"][role] = actor
        finally:
            try:
                if network is not None:
                    try:
                        cleanup.extend(network.close())
                    except Exception as error:
                        cleanup.append(f"network cleanup: {error}")
                    try:
                        artifact["network"] = network.evidence()
                    except Exception as error:
                        errors.append(f"network capture: {error}")
                artifact["attempts"] = scope.attempts
                artifact["processes"] = scope.evidence()
                artifact["elapsed_seconds"] = time.monotonic() - started
            finally:
                exit_scope(scope_token)
    if not errors and not cleanup:
        errors.extend(validate_case(artifact))
    artifact["status"] = "passed" if not errors and not cleanup else "failed"
    return artifact


def run_suite(binaries, root, *, pnet_key_file=None, pnet_fingerprint=None, **support):
    for spec in case_specs():
        private = {"pnet_key_file": pnet_key_file, "pnet_fingerprint": pnet_fingerprint} if spec.profile == "private" else {}
        yield run_case(spec, binaries, root, **private, **support)
