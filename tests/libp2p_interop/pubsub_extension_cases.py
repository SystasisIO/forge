"""Native PR12 orchestration; observations are not canonical acceptance.

The existing process owner, wire receipts and all-actor shutdown barrier remain
authoritative. This runner adds no router mutation or synthetic positive frames.
"""

from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import secrets
import time

from process_lifecycle import StopBudget, enter_scope, exit_scope, spawn_owned
from pubsub_cases import (
    _await, _has, _mesh_is, _prepare_all, _read, _ready, _stop_prepared, _subscription_received,
)
from pubsub_evidence import DIRECTIONS, PROFILES, require
from pubsub_extension_evidence import native_rpcs


@dataclass(frozen=True)
class Case:
    source: str
    destination: str
    extension: str
    profile: str

    @property
    def version(self):
        return "1.2" if self.extension == "idontwant" else "1.3"

    @property
    def scenario(self):
        base = {"idontwant": "gossipsub_v1_2", "advertisement": "gossipsub_v1_3",
                "partial": "partial_messages"}[self.extension]
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

    def implementation(self, role):
        return {"victim": self.destination, "replacement": self.source,
                "offender": "forge", "sink": "forge"}[role]

    def mode(self, role):
        return "advertisement" if self.extension == "partial" and role in {"offender", "sink"} else self.extension


def case_specs():
    return tuple(Case(source, target, extension, profile)
                 for extension in ("idontwant", "advertisement", "partial")
                 for profile in PROFILES for source, target in DIRECTIONS)


class Actors:
    """One bounded native execution, with shared PR11 process/shutdown ownership."""

    def __init__(self, spec, binaries, root, key, fingerprint, command_attempt):
        require(spec in case_specs(), "unknown extension matrix case")
        self.spec, self.binaries = spec, binaries
        self.key, self.fingerprint, self.command_attempt = key, fingerprint, command_attempt
        self.work, self.token = Path(root).resolve() / spec.identifier, secrets.token_hex(16)
        self.deadline = time.monotonic() + 60
        self.actors, self.sequence = {}, {}
        self.artifact = {"schema_version": 1, "suite": "pubsub-extensions", "case": asdict(spec),
                         "case_token": self.token, "scenario_id": spec.identifier,
                         "acceptance_scenario_id": spec.scenario, "runner_scenario_id": spec.runner_id,
                         "raw": {}, "processes": {}, "attempts": [], "errors": [], "cleanup_errors": [],
                         "status": "NOT_RUN"}

    def start(self, scope):
        self.work.mkdir(parents=True, exist_ok=False)
        private = self.spec.profile == "private_tcp_yamux"
        require(not private or self.key is not None and self.fingerprint is not None,
                "private extension case lacks key/fingerprint")
        for role in ("victim", "offender", "replacement", "sink"):
            implementation = self.spec.implementation(role)
            files = {name: self.work / f"{role}.{name}" for name in ("ready", "result", "stop", "control")}
            argv = [str(Path(self.binaries[implementation]).resolve()), "pubsub-live",
                    "--version", self.spec.version, "--extension", self.spec.mode(role),
                    "--transport", PROFILES[self.spec.profile], "--actor", role, "--case-token", self.token,
                    "--store-dir", str(self.work / f"{role}.store")]
            for name, path in files.items():
                argv.extend((f"--{name}-file", str(path)))
            if private:
                argv.extend(("--pnet-key-file", str(Path(self.key).resolve()), "--pnet-fingerprint", self.fingerprint))
            attempt = self.command_attempt(argv, self.work / f"{role}.log", self.spec.identifier, 1, role, 60) \
                if self.command_attempt else None
            if attempt is not None:
                self.artifact["attempts"].append(attempt)
                if not any(item is attempt for item in scope.attempts):
                    scope.attempts.append(attempt)
            owner = spawn_owned(argv, self.work / f"{role}.log", files["stop"], attempt,
                                stop_budget=StopBudget(8, 0, 2))
            self.actors[role] = (owner, files)
            self.sequence[role] = 0
            owner.ready = _await(owner, files["ready"], lambda value: (
                _ready(value, _read(files["result"]) if files["result"].exists() else None,
                       implementation, role, self.token)
                and value.get("extension") == self.spec.mode(role)), self.deadline)
        require(len({self.peer(role) for role in self.actors}) == 4, "extension actors share authenticated identity")

    def peer(self, role):
        return self.actors[role][0].ready["peer_id"]

    def wait(self, role, predicate):
        owner, files = self.actors[role]
        return _await(owner, files["result"], predicate, self.deadline)

    def control(self, role, kind, **fields):
        self.sequence[role] += 1
        command = {"sequence": self.sequence[role], "kind": kind, **fields}
        with self.actors[role][1]["control"].open("a") as output:
            output.write(json.dumps(command, separators=(",", ":")) + "\n")
            output.flush()
        self.wait(role, lambda value: _has(value, "command_done", command_sequence=self.sequence[role]))

    def connect(self, left, right):
        self.control(left, "connect", peer_id=self.peer(right), address=self.actors[right][0].ready["listen_addrs"][0])
        for role, remote in ((left, right), (right, left)):
            self.wait(role, lambda value: _subscription_received(
                value, self.peer(remote), "/meshsub/" + self.spec.version + ".0", "forge-pr11:" + self.token,
                PROFILES[self.spec.profile], implementation=self.spec.implementation(role), local_peer=self.peer(role),
                expected_fingerprint=self.fingerprint))

    def pair(self, left, right):
        self.connect(left, right)
        for role, remote in ((left, right), (right, left)):
            self.wait(role, lambda value: _mesh_is(value, [self.peer(remote)]))

    def observe_pairs(self):
        self.pair("victim", "offender")
        self.pair("replacement", "sink")
        self.connect("victim", "replacement")
        for role, mesh in (("victim", "offender"), ("replacement", "sink")):
            self.wait(role, lambda value: _mesh_is(value, [self.peer(mesh)]))
        for role in self.actors:
            self.control(role, "sample", label="extension_before")

    def full_delivery(self, sender, receiver, suffix):
        payload = "accept:" + self.token + ":" + suffix
        self.control(sender, "publish", payload=payload)
        self.wait(receiver, lambda value: _has(value, "delivery", payload_sha256=hashlib.sha256(payload.encode()).hexdigest()))

    def partial(self):
        self.observe_pairs()
        # Only the provider owns an advertised group. The consumer learns its
        # metadata over the off-mesh edge before asking for any bytes.
        self.control("replacement", "partial_offer", have=7)
        self.wait("victim", lambda value: _has(value, "partial_reconstructed"))
        self.full_delivery("offender", "victim", "full-fallback")

    def advertisement(self):
        self.observe_pairs()
        self.full_delivery("offender", "victim", "advertisement")
        self.full_delivery("sink", "replacement", "advertisement-reverse")

    def idontwant(self):
        # P=offender, F=victim, R=replacement, S=sink. No R-S edge can bypass F.
        self.connect("victim", "offender")
        self.connect("replacement", "offender")
        self.connect("victim", "replacement")
        self.connect("victim", "sink")
        graph = {"victim": ("offender", "replacement", "sink"), "offender": ("victim", "replacement"),
                 "replacement": ("victim", "offender"), "sink": ("victim",)}
        for role, members in graph.items():
            self.wait(role, lambda value: _mesh_is(value, [self.peer(other) for other in members]))
        for role in self.actors:
            self.control(role, "sample", label="extension_before")
        payload = "accept:" + self.token + ":idontwant:" + "x" * 1200
        digest = hashlib.sha256(payload.encode()).hexdigest()
        self.control("victim", "validation_hold", payload=payload)
        self.control("offender", "publish_extension", payload=payload)
        self.wait("victim", lambda value: _has(value, "validation_held", payload_sha256=digest))

        def suppression_received(value):
            views = native_rpcs(value["events"], self.spec.implementation("victim"), "/meshsub/1.2.0",
                                PROFILES[self.spec.profile], self.fingerprint)
            messages = [message for rpc in views if rpc.stream[-1] == "read" and rpc.peer == self.peer("offender")
                        for message in rpc.value["messages"] if message["payload_sha256"] == digest]
            ids = {message["author_hex"] + message["seqno_hex"] for message in messages}
            return any(set(item["ids_hex"]) & ids for rpc in views
                       if rpc.stream[-1] == "read" and rpc.peer == self.peer("replacement")
                       for item in rpc.value["idontwant"])

        self.wait("victim", suppression_received)
        self.control("victim", "validation_release")
        self.wait("sink", lambda value: _has(value, "delivery", payload_sha256=digest, propagation_peer=self.peer("victim")))

    def cleanup(self, scope):
        stopped = {row["actor"] for row in self.artifact.get("shutdown_barrier", {}).get("operations", [])
                   if row["kind"] == "stop_requested"}
        for role, (owner, _) in self.actors.items():
            if role not in stopped:
                try:
                    owner.request_stop()
                except Exception as error:
                    self.artifact["cleanup_errors"].append(f"{role} stop: {error}")
        for role, (owner, files) in self.actors.items():
            try:
                self.artifact["cleanup_errors"].extend(owner.close())
            except Exception as error:
                self.artifact["cleanup_errors"].append(f"{role} close: {error}")
            process = owner.evidence()
            process.update(returncode=owner.terminal_status.get("exit_code"),
                           forced_termination=owner.terminal_status.get("termination") != "graceful")
            self.artifact["processes"][role] = process
            if files["result"].exists():
                try:
                    self.artifact["raw"][role] = _read(files["result"], diagnostic=True)
                except Exception as error:
                    self.artifact["errors"].append(f"{role} capture: {error}")
        self.artifact["cleanup_errors"].extend(scope.close())


def run_case(spec, binaries, root, *, key=None, fingerprint=None, command_attempt=None):
    case = Actors(spec, binaries, root, key, fingerprint, command_attempt)
    scope, scope_token = enter_scope()
    try:
        case.start(scope)
        getattr(case, spec.extension)()
        case.artifact["shutdown_barrier"] = _prepare_all(case.actors, case.sequence, case.token, case.deadline)
        _stop_prepared(case.actors, case.artifact["shutdown_barrier"], deadline=case.deadline)
    except Exception as error:
        case.artifact["errors"].append(f"{type(error).__name__}: {error}")
    finally:
        try:
            case.cleanup(scope)
        finally:
            exit_scope(scope_token)
    if case.artifact["errors"] or case.artifact["cleanup_errors"]:
        case.artifact["status"] = "HARNESS_ERROR"
    else:
        # A completed execution still needs independent causal and source/provenance validation.
        case.artifact["status"] = "captured"
    return case.artifact
