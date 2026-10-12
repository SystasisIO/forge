"""Two owned live actors for configured PubSub, not a second process runtime."""

from dataclasses import asdict, dataclass
import json
import secrets
import time

from plugin_node_evidence import load_json
from process_lifecycle import StopBudget, current_scope, spawn_owned

BACKENDS = ("mdbx", "rocksdb")
DONORS = ("go", "rust")
POLICIES = ("yaml-only", "typed-override")
ACTORS = ("victim", "replacement")
CASE_SECONDS = 40


@dataclass(frozen=True)
class Case:
    backend: str
    donor: str
    policy: str

    def __post_init__(self):
        if self.backend not in BACKENDS or self.donor not in DONORS or self.policy not in POLICIES:
            raise ValueError("unknown configured PubSub case")

    @property
    def version(self):
        return "1.2" if self.policy == "typed-override" else "1.1"

    @property
    def protocol(self):
        return "/meshsub/" + self.version + ".0"

    @property
    def payload_limit(self):
        return 96 if self.policy == "typed-override" else 128

    @property
    def transport(self):
        return "tcp"

    @property
    def dns(self):
        return False

    @property
    def scenario_id(self):
        return f"plugin-pubsub/{self.backend}/{self.policy}/{self.donor}"

    def implementation(self, actor):
        if actor not in ACTORS:
            raise ValueError("unknown configured PubSub actor")
        return "forge" if actor == "victim" else self.donor


def case_specs(backend):
    if backend not in BACKENDS:
        raise ValueError("configured PubSub requires a real ObjectDB backend, no fallback")
    return tuple(Case(backend, donor, policy) for donor in DONORS for policy in POLICIES)


def probes(spec, token):
    if len(token) != 32 or any(char not in "0123456789abcdef" for char in token):
        raise ValueError("invalid owned PubSub token")
    prefix = ("accept:" + token + ":forge:").encode()
    binary = prefix + bytes((index * 29) % 256 for index in range(spec.payload_limit - len(prefix)))
    return binary, binary + b"\xff", "accept:" + token + ":donor"


def command(spec, actor, binary, work, token):
    implementation = spec.implementation(actor)
    argv = [str(binary), "pubsub-live", "--version", spec.version, "--transport", "tcp",
            "--actor", actor, "--case-token", token,
            "--ready-file", str(work / (actor + ".ready.json")),
            "--result-file", str(work / (actor + ".json")),
            "--stop-file", str(work / (actor + ".stop")),
            "--control-file", str(work / (actor + ".control")),
            "--store-dir", str(work / (actor + ".store"))]
    if implementation == "forge":
        argv += ["--store-backend", spec.backend, "--pubsub-policy", spec.policy]
    return argv


def _read(path):
    if path.is_symlink() or path.stat().st_size > 16 * 1024 * 1024:
        raise ValueError("unbounded/aliased configured PubSub receipt")
    value = load_json(path)
    if not isinstance(value, dict) or value.get("overflow") is True or value.get("error") is not None:
        raise RuntimeError("configured PubSub actor failed or overflowed")
    return value


def _wait(owner, path, predicate, deadline):
    while time.monotonic() < deadline:
        if owner.process.poll() is not None:
            raise RuntimeError("configured PubSub actor exited before its requested observation")
        if path.is_file():
            value = _read(path)
            if predicate(value):
                return value
        # Poll the actual witness, never use elapsed time as success evidence.
        time.sleep(0.025)
    raise TimeoutError("configured PubSub observation deadline: " + path.name)


def _append_control(work, controls, process_role, kind, **fields):
    operation = {"sequence": len(controls[process_role]) + 1, "kind": kind, **fields}
    encoded = json.dumps(operation, separators=(",", ":")) + "\n"
    if len(encoded.encode()) > 16 * 1024 or len(controls[process_role]) >= 64:
        raise ValueError("configured PubSub control exceeds fixture bounds")
    with (work / (process_role + ".control")).open("a") as output:
        output.write(encoded)
        output.flush()
    controls[process_role].append(operation)
    return operation["sequence"]


def run_case(spec, binaries, root, *, command_attempt):
    from plugin_pubsub_evidence import (
        active_actor, command_done, delivery_ready, donor_marker_written, forge_ack, forge_delivery, ready_actor, validate_delivery,
    )
    from pubsub_evidence import quiesce_ack, shutdown_ack

    scope = current_scope()
    if scope is None:
        raise RuntimeError("configured PubSub needs the runner's existing process owner")
    work = root / spec.scenario_id.replace("/", "-")
    work.mkdir(parents=True, exist_ok=False)
    token = secrets.token_hex(16)
    deadline = time.monotonic() + CASE_SECONDS
    owners, controls, prepared = {}, {actor: [] for actor in ACTORS}, {}
    barrier = {"source": "python.fixture.configured_pubsub_prepare_barrier", "operations": []}

    def mark(kind, actor, **fields):
        owner = owners[actor]
        barrier["operations"].append({"sequence": len(barrier["operations"]) + 1, "kind": kind,
            "actor": actor, "case_token": token, "local_peer_id": owner.ready["peer_id"],
            "pid": owner.process.pid, **fields})

    def issue(process_role, kind, **fields):
        return _append_control(work, controls, process_role, kind, **fields)

    def wait(actor, predicate):
        return _wait(owners[actor], work / (actor + ".json"), predicate, deadline)

    def execute(process_role, kind, **fields):
        sequence = issue(process_role, kind, **fields)
        def completed(raw):
            active_actor(raw, spec, process_role, token, owners[process_role].ready["peer_id"])
            return command_done(raw, sequence, kind) is not None
        return wait(process_role, completed)

    for actor in ACTORS:
        budget = min(20, deadline - time.monotonic())
        if budget <= 0 or (work / (actor + ".store")).exists():
            raise RuntimeError("configured PubSub deadline/fresh store requirement failed")
        argv = command(spec, actor, binaries[spec.implementation(actor)], work, token)
        attempt = command_attempt(argv, work / (actor + ".log"), spec.scenario_id, 1, actor, budget)
        owner = spawn_owned(argv, work / (actor + ".log"), work / (actor + ".stop"), attempt,
                            stop_budget=StopBudget(10, 0, 10) if actor == "victim" else StopBudget(8, 0, 2))
        owners[actor] = owner
        ready = _wait(owner, work / (actor + ".ready.json"),
                      lambda raw: ready_actor(raw, spec, actor, token) is not None, deadline)
        owner.ready = ready
        wait(actor, lambda raw, actor=actor: active_actor(raw, spec, actor, token, ready["peer_id"]) is not None)
    if owners["victim"].ready["peer_id"] == owners["replacement"].ready["peer_id"]:
        raise ValueError("configured PubSub actors share an identity")

    # Connect once through the official facade. Publication waits for the
    # donor's actual subscription RPC AND mesh snapshot, not a warm-up delay.
    execute("victim", "connect", peer_id=owners["replacement"].ready["peer_id"],
            address=owners["replacement"].ready["listen_addrs"][0])
    wait("replacement", lambda raw: delivery_ready(raw, spec, owners["victim"].ready["peer_id"], token))
    binary, oversized, marker = probes(spec, token)
    execute("victim", "publish_binary", payload_hex=binary.hex())
    from plugin_pubsub_evidence import donor_delivery
    wait("replacement", lambda raw: donor_delivery(raw, spec, owners["victim"].ready["peer_id"], token, binary) is not None)
    execute("victim", "publish_oversized", payload_hex=oversized.hex())
    before = execute("victim", "sample", label="before-donor")
    execute("replacement", "publish", payload=marker)
    def forge_received(raw):
        active_actor(raw, spec, "victim", token, owners["victim"].ready["peer_id"])
        return forge_delivery(raw, spec, owners["replacement"].ready["peer_id"], token, before=before) is not None
    received = wait("victim", forge_received)
    delivered = forge_delivery(received, spec, owners["replacement"].ready["peer_id"], token, before=before)
    # Topic.Publish completion and receiver delivery can precede publication of
    # the donor's native-write receipt. Wait for that same signed message only.
    wait("replacement", lambda raw: donor_marker_written(raw, spec, owners["victim"].ready["peer_id"], token, delivered))
    execute("victim", "sample", label="after-donor")
    validate_delivery({actor: _read(work / (actor + ".json")) for actor in ACTORS}, spec, token)

    # Separate controls and immutable active captures precede any host stop.
    for actor in ACTORS:
        issue(actor, "prepare_shutdown", actor=actor, case_token=token, local_peer_id=owners[actor].ready["peer_id"])
    for actor in ACTORS:
        sequence = len(controls[actor])
        local = owners[actor].ready["peer_id"]
        implementation = spec.implementation(actor)
        def ack(raw, actor=actor, sequence=sequence, local=local, implementation=implementation):
            active_actor(raw, spec, actor, token, local)
            return (forge_ack(raw, token, local, sequence, active=True) if actor == "victim" else
                    shutdown_ack(raw, implementation, actor, token, local, sequence, active=True))
        raw = wait(actor, lambda raw: ack(raw) is not None)
        acknowledgement = ack(raw)
        path = work / (actor + ".prepare.json")
        capture = {"schema_version": 1, "source": "python.fixture.native_prepare_snapshot",
            "actor": actor, "case_token": token, "pid": owners[actor].process.pid,
            "command_sequence": sequence, "ack_event_sequence": acknowledgement["sequence"], "result": raw}
        with path.open("x") as output:
            json.dump(capture, output, separators=(",", ":"))
        prepared[actor] = capture
        mark("prepare_ack", actor, command_sequence=sequence, ack_event_sequence=acknowledgement["sequence"],
             evidence_file=str(path))
    if spec.donor == "go":
        actor = "replacement"
        sequence = issue(actor, "quiesce_shutdown", actor=actor, case_token=token,
                         local_peer_id=owners[actor].ready["peer_id"],
                         prepare_ack_sequence=prepared[actor]["ack_event_sequence"])
        fields = {"command_sequence": sequence, "prepare_ack_sequence": prepared[actor]["ack_event_sequence"]}
        mark("quiesce_requested", actor, **fields)
        def quiesced(raw):
            return quiesce_ack(raw, actor, token, owners[actor].ready["peer_id"], owners[actor].process.pid,
                               sequence, prepared[actor]["ack_event_sequence"], active=True)
        raw = wait(actor, lambda raw: quiesced(raw) is not None)
        mark("quiesce_ack", actor, ack_event_sequence=quiesced(raw)["sequence"], **fields)
    for actor in ("replacement", "victim"):
        mark("donor_stop_published" if actor == "replacement" else "forge_stop_published", actor)
        owners[actor].request_stop()
        errors = owners[actor].close()
        if errors:
            raise RuntimeError("configured PubSub owned cleanup failed: " + "; ".join(errors))
        mark("donor_joined" if actor == "replacement" else "forge_joined", actor)
    return {"suite": "plugin-pubsub", "scenario_id": spec.scenario_id, "spec": asdict(spec),
        "case_token": token, "operation_budget_seconds": CASE_SECONDS,
        "endpoints": {actor: {"implementation": spec.implementation(actor), "process": owners[actor].evidence(),
            "result_file": str(work / (actor + ".json")), "result": _read(work / (actor + ".json")),
            "store_existed_before_spawn": False} for actor in ACTORS},
        "controls": controls, "prepared": prepared, "shutdown_barrier": barrier,
        "owned_processes": scope.evidence(), "attempts": list(scope.attempts)}
