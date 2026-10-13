"""Three owned processes for configured DHT values, never a local PUT/GET shortcut."""

from dataclasses import asdict, dataclass
import time

from plugin_node_cases import _address, _wait_result
from plugin_node_evidence import load_json
from process_lifecycle import StopBudget, current_scope, spawn_owned

SCENARIOS = ("dht_pk_put_get", "dht_ipns_put_get")
BACKENDS = ("mdbx", "rocksdb")
DONORS = ("go", "rust")
FORGE_ROLES = ("client", "server")
CASE_SECONDS = 40


@dataclass(frozen=True)
class Case:
    backend: str
    scenario: str
    donor: str
    forge_role: str

    def __post_init__(self):
        if (self.backend not in BACKENDS or self.scenario not in SCENARIOS
                or self.donor not in DONORS or self.forge_role not in FORGE_ROLES):
            raise ValueError("unknown configured DHT value case")

    @property
    def transport(self):
        return "quic"

    @property
    def dns(self):
        return False

    @property
    def scenario_id(self):
        return f"plugin-dht/{self.backend}/{self.scenario}/forge-{self.forge_role}/{self.donor}"

    def implementation(self, actor):
        if actor not in ("writer", "server", "reader"):
            raise ValueError("unknown DHT process role")
        return (self.donor if self.forge_role == "client" else "forge") if actor == "server" \
            else ("forge" if self.forge_role == "client" else self.donor)

    def completion_barrier(self, actor):
        return self.forge_role == "server" and actor in ("writer", "reader")


def case_specs(backend):
    if backend not in BACKENDS:
        raise ValueError("configured DHT needs an actual ObjectDB backend, no fallback")
    return tuple(Case(backend, scenario, donor, role) for scenario in SCENARIOS
                 for donor in DONORS for role in FORGE_ROLES)


def command(spec, actor, binary, work, *, peer="", address=""):
    implementation = spec.implementation(actor)
    prefix = work / actor
    argv = [str(binary), "listen" if actor == "server" else "dial",
            "--scenario", spec.scenario, "--transport", "quic",
            "--store-dir", str(prefix.with_suffix(".store")),
            "--result-file", str(prefix.with_suffix(".json")),
            "--stop-file", str(prefix.with_suffix(".stop"))]
    if implementation == "forge":
        argv += ["--store-backend", spec.backend]
    if actor == "server":
        argv += ["--ready-file", str(prefix.with_suffix(".ready.json")), "--features", "ping,identify"]
    else:
        if not peer or not address:
            raise ValueError("isolated DHT operation needs its sole server target")
        argv += ["--peer-id", peer, "--addr", address,
                 "--payload", "put_only" if actor == "writer" else "get_only"]
        if spec.completion_barrier(actor):
            argv += ["--dht-completion-barrier", "stop-file", "--ready-file",
                     str(prefix.with_suffix(".ready.json"))]
    return argv


def _wait_server_session(spec, actor, owners, work, witness, peer, address, *, deadline):
    from plugin_dht_evidence import server_session_receipt
    while time.monotonic() < deadline:
        # Even an already-published witness cannot excuse a premature owner exit.
        for name in ("server", actor):
            if owners[name].process.poll() is not None:
                raise RuntimeError("DHT " + name + " exited before server session receipt")
        path = work / "server.json"
        if path.exists():
            session = server_session_receipt(load_json(path), spec, actor, witness, peer, address)
            if session is not None:
                return session
        time.sleep(0.005)
    raise TimeoutError("DHT exact identified server session receipt deadline expired for " + actor)


def run_case(spec, binaries, root, *, command_attempt):
    scope = current_scope()
    if scope is None:
        raise RuntimeError("configured DHT case requires the runner's existing process owner")
    work = root / spec.scenario_id.replace("/", "-")
    work.mkdir(parents=True, exist_ok=False)
    deadline = time.monotonic() + CASE_SECONDS
    timeline = []
    owners = {}
    server_receipts = {}

    def remaining(limit=20):
        budget = min(limit, deadline - time.monotonic())
        if budget <= 0:
            raise TimeoutError("configured DHT overall operation deadline expired")
        return budget

    def mark(event, owner, **facts):
        timeline.append({"event": event, "pid": owner.process.pid, "monotonic_ns": time.monotonic_ns(), **facts})

    def spawn(actor, **target):
        budget = remaining()
        store = (work / actor).with_suffix(".store")
        if store.exists():
            raise RuntimeError("DHT actor store is not fresh; no retry/store reuse is permitted")
        argv = command(spec, actor, binaries[spec.implementation(actor)], work, **target)
        attempt = command_attempt(argv, work / f"{actor}.log", spec.scenario_id, 1, actor, budget)
        owner = spawn_owned(argv, work / f"{actor}.log", work / f"{actor}.stop", attempt,
                            stop_budget=StopBudget(10, 0, 10) if spec.implementation(actor) == "forge" else None)
        owners[actor] = owner
        mark(actor + "_started", owner)
        return owner

    def wait(actor, statuses):
        path = work / f"{actor}.json"
        _wait_result(path, owners[actor], statuses, timeout=remaining())
        # Reuse the configured-plugin strict duplicate/nonfinite JSON decoder.
        receipt = load_json(path)
        if not isinstance(receipt, dict) or receipt.get("status") not in statuses:
            raise RuntimeError("DHT result changed or failed before its owned join")
        return receipt

    def join(actor):
        owner = owners[actor]
        owner.request_stop()
        errors = owner.close()
        if errors:
            raise RuntimeError("DHT owned cleanup failed: " + "; ".join(errors))
        mark(actor + "_joined", owner)

    def complete_operation(actor):
        if spec.completion_barrier(actor):
            from plugin_dht_evidence import validate_completion_witness
            path = work / (actor + ".ready.json")
            _wait_result(path, owners[actor], {"operation_observed"}, timeout=remaining())
            witness = load_json(path)
            validate_completion_witness(witness, spec, actor, peer, address)
            owners[actor].ready = witness
            session = _wait_server_session(spec, actor, owners, work, witness, peer, address,
                                           deadline=min(deadline, time.monotonic() + remaining()))
            server_receipts[actor] = session
            mark(actor + "_server_session_observed", owners[actor],
                 session_id=session["session_id"], remote_peer_id=session["remote_peer_id"])
        else:
            wait(actor, {"ok"})
        join(actor)
        from plugin_dht_evidence import validate_operation
        validate_operation(load_json(work / (actor + ".json")), spec, actor, peer, address)

    server = spawn("server")
    ready_path = work / "server.ready.json"
    _wait_result(ready_path, server, {"ready"}, timeout=remaining())
    server.ready = load_json(ready_path)
    if not isinstance(server.ready, dict) or server.ready.get("status") != "ready":
        raise RuntimeError("missing strict server readiness")
    address = _address(server.ready, "quic")
    peer = server.ready["peer_id"]
    mark("server_ready", server)

    spawn("writer", peer=peer, address=address)
    complete_operation("writer")
    # The writer is already joined. This independent process gets neither its
    # address nor its store; GET has only the still-running server as a seed.
    if server.process.poll() is not None:
        raise RuntimeError("DHT server exited before the fresh remote reader")
    spawn("reader", peer=peer, address=address)
    complete_operation("reader")
    remaining()
    join("server")

    endpoints = {}
    for actor, owner in owners.items():
        path = work / f"{actor}.json"
        endpoints[actor] = {"implementation": spec.implementation(actor), "process": owner.evidence(),
                            "result_file": str(path), "result": load_json(path),
                            "store_existed_before_spawn": False}
    return {"suite": "plugin-dht", "scenario_id": spec.scenario_id, "spec": asdict(spec),
            "peer_id": peer, "listen_address": address, "endpoints": endpoints,
            "timeline_source": "runner.process_lifecycle", "timeline": timeline,
            "server_session_receipts": server_receipts,
            "operation_budget_seconds": CASE_SECONDS, "owned_processes": scope.evidence(),
            "attempts": list(scope.attempts)}
