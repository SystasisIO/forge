"""Four-process native P4-driven repair; no manual mesh or score mutation."""

from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import secrets
import time

from process_lifecycle import StopBudget, enter_scope, exit_scope, spawn_owned
from pubsub_evidence import (
    DIRECTIONS, EVENT_LIMIT, PROFILES, SOURCES, _donor_shutdown_event, _events, _owner, _rpc_peer, _terminal_owners,
    prepared_snapshot, quiesce_ack, require, shutdown_ack, validate_case,
)
from pubsub_wire import validate_rpc_receipt


@dataclass(frozen=True)
class Case:
    source: str
    destination: str
    version: str
    profile: str

    @property
    def scenario(self):
        base = "gossipsub_v1_0_fallback" if self.version == "1.0" else "gossipsub_v1_1"
        return base + {"native_quic": "", "native_tcp_yamux": "_native_tcp_yamux",
                       "private_tcp_yamux": "_private_tcp_yamux_pnet"}[self.profile]

    @property
    def identifier(self):
        return f"{self.scenario}.{self.source}_to_{self.destination}"

    @property
    def runner_id(self):
        prefix = {"native_quic": "quic_stage6", "native_tcp_yamux": "tcp_stage6",
                  "private_tcp_yamux": "private_tcp_yamux_pnet"}[self.profile]
        return f"{prefix}/{self.scenario}"


def case_specs():
    return tuple(Case(source, destination, version, profile)
                 for version in ("1.0", "1.1") for profile in PROFILES for source, destination in DIRECTIONS)


def _read(path, *, diagnostic=False):
    if path.stat().st_size > 16 * 1024 * 1024:
        raise ValueError("PubSub actor result exceeds trace byte bound")
    value = json.loads(path.read_text())
    if not isinstance(value, dict) or not diagnostic and (value.get("overflow") is True or value.get("error") is not None):
        raise RuntimeError("PubSub actor failed or overflowed its native trace")
    return value


def _await(owner, path, predicate, deadline):
    while time.monotonic() < deadline:
        if owner.process.poll() is not None:
            raise RuntimeError("PubSub actor exited before requested native observation")
        if path.exists():
            value = _read(path)
            if predicate(value):
                return value
        time.sleep(0.025)
    raise TimeoutError(f"PubSub native-state deadline: {path.name}")


def _ready(value, observation, implementation, actor, token, *, terminal=False):
    if type(terminal) is not bool or not isinstance(value, dict) or not isinstance(observation, dict):
        return False
    if (value.get("ready") is not True or value.get("subscription_created") is not True
            or value.get("topic") != "forge-pr11:" + token):
        return False
    peer = value.get("local_peer_id")
    if not isinstance(peer, str) or not peer.strip() or len(peer) > 512 or value.get("peer_id") != peer:
        return False
    addresses = value.get("listen_addrs")
    if (not isinstance(addresses, list) or len(addresses) != 1
            or not isinstance(addresses[0], str) or not addresses[0].strip()):
        return False
    for document in (value, observation):
        if (type(document.get("schema_version")) is not int or document["schema_version"] != 1
                or document.get("implementation") != implementation or document.get("actor") != actor
                or document.get("case_token") != token or document.get("local_peer_id") != peer):
            return False
    return (observation.get("finalized") is terminal and observation.get("joined") is terminal
            and observation.get("overflow") is False and observation.get("error") is None)


def _has(value, kind, **fields):
    if kind == "validation" and value.get("implementation") == "go":
        kind = "delivery" if fields.get("outcome") == "accept" else "rejection"
        fields.update(committed=True, phase="post_decision")
    return any(event.get("kind") == kind and all(event.get(key) == target for key, target in fields.items())
               for event in value.get("events", []))


def _latest(value):
    snapshots = [event for event in value.get("events", []) if event.get("kind") == "snapshot"]
    return snapshots[-1] if snapshots else None


def _mesh_is(value, peers):
    snapshot = _latest(value)
    return snapshot is not None and set(snapshot.get("mesh_peer_ids", [])) == set(peers)


def _score_is_negative(value, peer):
    snapshot = _latest(value)
    return snapshot is not None and any(score.get("peer_id") == peer and score.get("value", 0) < -80
                                        and (score.get("invalid_deliveries_available") is False
                                             or score.get("invalid_deliveries", 0) > 0)
                                        for score in snapshot.get("peer_scores", []))


def _subscription_received(value, remote, protocol, topic, transport, *, implementation, local_peer,
                           expected_fingerprint=None):
    """Use actual active RPC receipts, with the same owner authority as terminal evidence."""
    require(isinstance(value, dict) and type(value.get("schema_version")) is int and value["schema_version"] == 1
            and implementation in SOURCES and value.get("implementation") == implementation
            and value.get("local_peer_id") == local_peer and value.get("case_token") == topic.removeprefix("forge-pr11:")
            and value.get("finalized") is False and value.get("joined") is False
            and value.get("overflow") is False and value.get("error") is None,
            "subscription predicate lacks its actual active actor identity")
    events, received = value.get("events"), False
    require(isinstance(events, list) and len(events) <= EVENT_LIMIT, "unbounded/missing active subscription events")
    for event in events:
        require(isinstance(event, dict), "invalid active subscription event")
        if event.get("kind") != "rpc":
            continue
        direction, source = event.get("direction"), event.get("source")
        require(direction in {"read", "write"} and source in SOURCES[implementation]["rpc"]
                and type(event.get("sequence")) is int and 1 <= event["sequence"] <= len(events)
                and events[event["sequence"] - 1] is event,
                "subscription RPC lacks exact native source/direction/sequence")
        if implementation == "go":
            require(source == ("go.quic.native_stream." if transport == "quic" else "go.pubsub.native_stream.") + direction,
                    "subscription RPC has wrong transport/direction authority")
        actual_peer = _rpc_peer(event)
        owner = _owner(events, event, actual_peer, protocol, transport, expected_fingerprint)
        if source in {"go.quic.native_stream.read", "go.quic.native_stream.write"}:
            require(owner[0]["local_peer_id"] == local_peer, "subscription lower owner belongs to another local actor")
        rpc = validate_rpc_receipt(event.get("receipt"), protocol, direction)
        if actual_peer == remote and direction == "read" and any(
                subscription.get("topic") == topic and subscription.get("subscribe") is True
                for subscription in rpc["subscriptions"]):
            received = True
    return received


def _prepare_all(actors, sequence, token, deadline):
    if set(actors) != {"victim", "offender", "replacement", "sink"}:
        raise ValueError("prepare_shutdown requires all four actual actors")
    for role, (owner, files) in actors.items():
        sequence[role] += 1
        command = {"sequence": sequence[role], "kind": "prepare_shutdown", "actor": role,
                   "case_token": token, "local_peer_id": owner.ready["peer_id"]}
        with files["control"].open("a") as output:
            output.write(json.dumps(command, separators=(",", ":")) + "\n")
            output.flush()
    barrier = {"source": "python.fixture.all_actor_prepare_barrier", "operations": []}
    for role, (owner, files) in actors.items():
        def acknowledged(value):
            return shutdown_ack(value, owner.ready["implementation"], role, token, owner.ready["peer_id"],
                                sequence[role], active=True) is not None
        value = _await(owner, files["result"], acknowledged, deadline)
        ack = shutdown_ack(value, owner.ready["implementation"], role, token, owner.ready["peer_id"], sequence[role])
        path = files["result"].with_name(role + ".prepare-result.json")
        row = {"sequence": len(barrier["operations"]) + 1, "kind": "prepare_ack",
               "actor": role, "case_token": token, "local_peer_id": owner.ready["peer_id"],
               "command_sequence": sequence[role], "ack_event_sequence": ack["sequence"], "evidence_file": str(path)}
        snapshot = {"schema_version": 1, "source": "python.fixture.native_prepare_snapshot",
                    "actor": role, "case_token": token, "pid": owner.process.pid,
                    "command_sequence": sequence[role], "ack_event_sequence": ack["sequence"], "result": value}
        prepared_snapshot(snapshot, row, value, owner.process.pid)
        encoded = json.dumps(snapshot, separators=(",", ":")).encode()
        if len(encoded) > 16 * 1024 * 1024:
            raise ValueError("prepare snapshot exceeds indexed artifact bound")
        with path.open("xb") as output:
            output.write(encoded)
        barrier["operations"].append(row)
    return barrier


def _stop_prepared(actors, barrier, *, deadline=None):
    # Recheck every active result before the first stop; never infer preparation from ready/status.
    operations = barrier["operations"]
    if (set(actors) != {"victim", "offender", "replacement", "sink"}
            or len(operations) != 4 or {row["actor"] for row in operations} != set(actors)
            or any(row["kind"] != "prepare_ack" or type(row["sequence"]) is not int or row["sequence"] != index
                   for index, row in enumerate(operations, 1))):
        raise ValueError("missing/foreign all-actor prepare acknowledgement")
    for row in operations:
        owner, files = actors[row["actor"]]
        if row["case_token"] != owner.ready["case_token"] or row["actor"] != owner.ready["actor"]:
            raise ValueError("prepare acknowledgement is foreign to actual readiness")
        if owner.process.poll() is not None:
            raise RuntimeError("prepared actor exited before barrier release")
        current = _read(files["result"])
        ack = shutdown_ack(current, owner.ready["implementation"], row["actor"],
                           row["case_token"], owner.ready["peer_id"], row["command_sequence"], active=True)
        if ack is None or row["local_peer_id"] != owner.ready["peer_id"] or ack["sequence"] != row["ack_event_sequence"]:
            raise ValueError("prepare acknowledgement changed before stop")
        path = files["result"].with_name(row["actor"] + ".prepare-result.json")
        if row.get("evidence_file") != str(path):
            raise ValueError("foreign prepare snapshot path before stop")
        prepared_snapshot(_read(path), row, current, owner.process.pid)
    # Keep every host alive while all Go PubSub owners actually quiesce.
    go = [(role, owner, files) for role, (owner, files) in actors.items() if owner.ready["implementation"] == "go"]
    for role, owner, files in go:
        prepared = next(row for row in operations[:4] if row["actor"] == role)
        command = {"sequence": prepared["command_sequence"] + 1, "kind": "quiesce_shutdown", "actor": role,
                   "case_token": owner.ready["case_token"], "local_peer_id": owner.ready["peer_id"],
                   "prepare_ack_sequence": prepared["ack_event_sequence"]}
        with files["control"].open("a") as output:
            output.write(json.dumps(command, separators=(",", ":")) + "\n")
            output.flush()
        operations.append({"sequence": len(operations) + 1, "kind": "quiesce_requested", "actor": role,
                           "case_token": command["case_token"], "local_peer_id": command["local_peer_id"], "pid": owner.process.pid,
                           "command_sequence": command["sequence"], "prepare_ack_sequence": command["prepare_ack_sequence"]})
    quiesce_deadline = time.monotonic() + 8 if deadline is None else deadline
    for role, owner, files in go:
        request = next(row for row in operations[4:4 + len(go)] if row["actor"] == role)
        def acknowledged(value):
            return quiesce_ack(value, role, owner.ready["case_token"], owner.ready["peer_id"], owner.process.pid,
                               request["command_sequence"], request["prepare_ack_sequence"], active=True) is not None
        value = _await(owner, files["result"], acknowledged, quiesce_deadline)
        require(owner.process.poll() is None, "Go actor exited before quiesce barrier release")
        ack = quiesce_ack(value, role, owner.ready["case_token"], owner.ready["peer_id"], owner.process.pid,
                          request["command_sequence"], request["prepare_ack_sequence"], active=True)
        require(ack is not None, "missing actual Go quiesce acknowledgement")
        operations.append({**request, "sequence": len(operations) + 1, "kind": "quiesce_ack",
                           "quiesce_event_sequence": ack["sequence"]})
    for role, owner, files in go:
        request = next(row for row in operations[4:4 + len(go)] if row["actor"] == role)
        ack = quiesce_ack(_read(files["result"]), role, owner.ready["case_token"], owner.ready["peer_id"],
                          owner.process.pid, request["command_sequence"], request["prepare_ack_sequence"], active=True)
        observed = next(row for row in operations if row["kind"] == "quiesce_ack" and row["actor"] == role)
        require(owner.process.poll() is None and ack is not None and ack["sequence"] == observed["quiesce_event_sequence"],
                "Go quiesce acknowledgement failed before first Stop")
    def stop(role, owner):
        owner.request_stop()
        operations.append({"sequence": len(operations) + 1, "kind": "stop_requested", "actor": role,
                           "case_token": owner.ready["case_token"], "local_peer_id": owner.ready["peer_id"]})

    donors = [(role, owner, files) for role, (owner, files) in actors.items()
              if owner.ready["implementation"] in {"go", "rust"}]
    # Publish every donor stop before waiting; Forge remains alive until all actual donor close/join.
    for role, owner, _ in donors:
        stop(role, owner)
    for role, owner, files in donors:
        errors = owner.close()
        terminal = owner.terminal_status
        code = owner.process.poll()
        if errors or type(terminal.get("exit_code")) is not int or terminal["exit_code"] != 0 \
                or terminal.get("termination") != "graceful" or type(code) is not int or code != 0:
            raise RuntimeError(f"{role} donor shutdown did not actually close/join successfully: {errors}")
        raw = _read(files["result"])
        events = _events(raw, owner.ready["implementation"], owner.ready["case_token"], role)
        _terminal_owners(raw)
        row = next(row for row in operations[:4] if row["actor"] == role)
        prepared_snapshot(_read(Path(row["evidence_file"])), row, raw, owner.process.pid)
        shutdown = _donor_shutdown_event(raw, events)
        require(shutdown["sequence"] > row["ack_event_sequence"], "donor shutdown preceded actual preparation")
        operations.append({"sequence": len(operations) + 1, "kind": "donor_joined", "actor": role,
                           "case_token": owner.ready["case_token"], "local_peer_id": owner.ready["peer_id"],
                           "pid": owner.process.pid, "shutdown_event_sequence": shutdown["sequence"]})
    for role, (owner, _) in actors.items():
        if owner.ready["implementation"] == "forge":
            stop(role, owner)


def run_case(spec, binaries, root, *, key=None, fingerprint=None, command_attempt=None):
    scope, scope_token = enter_scope()
    actors, errors, cleanup, attempts = {}, [], [], []
    work, token = Path(root).resolve() / spec.identifier, secrets.token_hex(16)
    deadline = time.monotonic() + 60
    artifact = {"schema_version": 1, "suite": "pubsub-scoring", "case": asdict(spec), "case_token": token,
                "scenario_id": spec.identifier, "acceptance_scenario_id": spec.scenario,
                "runner_scenario_id": spec.runner_id, "roles": {role: role for role in ("offender", "replacement", "sink")},
                "raw": {}, "processes": {}, "attempts": attempts, "errors": errors, "cleanup_errors": cleanup,
                "status": "NOT_RUN"}
    try:
        work.mkdir(parents=True, exist_ok=False)
        if spec.profile == "private_tcp_yamux" and (key is None or fingerprint is None):
            raise ValueError("private PubSub requires an explicit test key and verified fingerprint")
        for role in ("victim", "offender", "replacement", "sink"):
            implementation = spec.destination if role == "victim" else spec.source
            files = {name: work / f"{role}.{name}" for name in ("ready", "result", "stop", "control")}
            argv = [str(Path(binaries[implementation]).resolve()), "pubsub-live", "--version", spec.version,
                    "--transport", PROFILES[spec.profile], "--actor", role, "--case-token", token,
                    "--ready-file", str(files["ready"]), "--result-file", str(files["result"]),
                    "--stop-file", str(files["stop"]), "--control-file", str(files["control"]),
                    "--store-dir", str(work / f"{role}.store")]
            if spec.profile == "private_tcp_yamux":
                argv += ["--pnet-key-file", str(Path(key).resolve()), "--pnet-fingerprint", fingerprint]
            attempt = command_attempt(argv, work / f"{role}.log", spec.identifier, 1, role, 60) if command_attempt else None
            if attempt is not None:
                attempts.append(attempt)
                if not any(value is attempt for value in scope.attempts):
                    scope.attempts.append(attempt)
            owner = spawn_owned(argv, work / f"{role}.log", files["stop"], attempt,
                                stop_budget=StopBudget(8, 0, 2))
            actors[role] = (owner, files)
            owner.ready = _await(
                owner, files["ready"],
                lambda value: _ready(value, _read(files["result"]) if files["result"].exists() else None,
                                     implementation, role, token), deadline)
        require_peers = [owner.ready.get("peer_id") for owner, _ in actors.values()]
        if len(set(require_peers)) != 4 or any(not isinstance(peer, str) for peer in require_peers):
            raise RuntimeError("PubSub readiness lacks four distinct actual identities")
        sequence = {role: 0 for role in actors}

        def wait(role, predicate):
            owner, files = actors[role]
            return _await(owner, files["result"], predicate, deadline)

        def control(role, kind, **fields):
            sequence[role] += 1
            command = {"sequence": sequence[role], "kind": kind, **fields}
            line = json.dumps(command, separators=(",", ":")) + "\n"
            with actors[role][1]["control"].open("a") as output:
                output.write(line)
                output.flush()
            wait(role, lambda value: _has(value, "command_done", command_sequence=sequence[role]))

        def connect(role, other):
            ready = actors[other][0].ready
            addresses = ready.get("listen_addrs")
            if not isinstance(addresses, list) or len(addresses) != 1:
                raise RuntimeError("PubSub actor must have exactly one actual fixture listener")
            control(role, "connect", peer_id=ready["peer_id"], address=addresses[0])

        def peer(role):
            return actors[role][0].ready["peer_id"]

        connect("victim", "offender")
        wait("victim", lambda value: _mesh_is(value, [peer("offender")]))
        wait("offender", lambda value: _mesh_is(value, [peer("victim")]))
        connect("replacement", "sink")
        wait("replacement", lambda value: _mesh_is(value, [peer("sink")]))
        wait("sink", lambda value: _mesh_is(value, [peer("replacement")]))
        connect("victim", "replacement")
        protocol, topic = "/meshsub/" + spec.version + ".0", "forge-pr11:" + token

        def subscription_received(value, role, remote):
            return _subscription_received(value, remote, protocol, topic, PROFILES[spec.profile],
                                          implementation=actors[role][0].ready["implementation"], local_peer=peer(role),
                                          expected_fingerprint=fingerprint)

        wait("victim", lambda value: subscription_received(value, "victim", peer("replacement"))
             and _mesh_is(value, [peer("offender")]))
        wait("replacement", lambda value: subscription_received(value, "replacement", peer("victim"))
             and _mesh_is(value, [peer("sink")]))
        for role in actors:
            control(role, "sample", label="before")
        gossip_payload = "accept:" + token + ":gossip"
        control("offender", "publish", payload=gossip_payload)
        wait("sink", lambda value: _has(value, "delivery", payload_sha256=hashlib.sha256(gossip_payload.encode()).hexdigest(),
                                       propagation_peer=peer("replacement")))
        control("offender", "publish", payload="ignore:" + token + ":one")
        wait("victim", lambda value: _has(value, "validation", outcome="ignore",
             payload_sha256=hashlib.sha256(("ignore:" + token + ":one").encode()).hexdigest()))
        control("victim", "sample", label="ignored")
        control("offender", "publish", payload="reject:" + token + ":one")
        wait("victim", lambda value: _score_is_negative(value, peer("offender")))
        control("victim", "sample", label="penalized")
        wait("victim", lambda value: _mesh_is(value, [peer("replacement")]))
        wait("replacement", lambda value: _mesh_is(value, [peer("victim"), peer("sink")]))
        control("victim", "sample", label="repaired")
        control("replacement", "sample", label="repaired")
        payload = "accept:" + token + ":repaired"
        control("victim", "publish", payload=payload)
        wait("sink", lambda value: _has(value, "delivery", payload_sha256=hashlib.sha256(payload.encode()).hexdigest(),
                                       propagation_peer=peer("replacement")))
        artifact["shutdown_barrier"] = _prepare_all(actors, sequence, token, deadline)
        _stop_prepared(actors, artifact["shutdown_barrier"], deadline=deadline)
    except Exception as error:
        errors.append(f"{type(error).__name__}: {error}")
    finally:
        stopped = {row["actor"] for row in artifact.get("shutdown_barrier", {}).get("operations", [])
                   if row["kind"] == "stop_requested"}
        for role, (owner, _) in actors.items():
            if role in stopped:
                continue
            try:
                # Failure cleanup is not a successful barrier and cannot produce acceptance.
                owner.request_stop()
            except Exception as error:
                cleanup.append(f"pid={owner.process.pid}: stop publication failed: {error}")
        for role, (owner, files) in actors.items():
            try:
                cleanup.extend(owner.close())
            except Exception as error:
                cleanup.append(f"{role} close failed: {error}")
            process = owner.evidence()
            process.update(returncode=owner.terminal_status.get("exit_code"),
                           forced_termination=owner.terminal_status.get("termination") != "graceful")
            artifact["processes"][role] = process
            if files["result"].exists():
                try:
                    raw = _read(files["result"], diagnostic=True)
                    artifact["raw"][role] = raw
                    if raw.get("overflow") is True or raw.get("error") is not None:
                        errors.append(f"{role} terminal capture: native failure/overflow retained")
                except Exception as error:
                    errors.append(f"{role} terminal capture: {type(error).__name__}: {error}")
        try:
            cleanup.extend(scope.close())
        finally:
            exit_scope(scope_token)
    if not errors and not cleanup:
        try:
            artifact["evidence"] = validate_case(artifact, expected_fingerprint=fingerprint)
            # Causal observations alone do not prove binary/donor/output provenance.
            # Only canonical promotion may emit PASS after its indexed source checks.
            artifact["status"] = "observed"
        except ValueError as error:
            errors.append(str(error))
    if errors or cleanup:
        artifact["status"] = "HARNESS_ERROR"
    return artifact
