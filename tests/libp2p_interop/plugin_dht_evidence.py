"""Strict DHT value receipts; causal Amino evidence is not negotiation event capture."""

import hashlib
import math
from pathlib import Path

from plugin_dht_cases import CASE_SECONDS, Case, command
from plugin_node_cases import listener_socket, socket
from plugin_node_evidence import accepted_configuration, counter, load_json, require

AMINO = "/ipfs/kad/1.0.0"
IDENTITY_MULTIHASH = "00240801122079b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664"
# Exact existing C++/Go/Rust golden bytes, not hashes invented for donor output.
PK_HEX = "0801122079b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664"
IPNS_HEX = (
    "0a1f2f697066732f6261666b716163336a6f627868676964736e3572777734796b1240b7be19b36e1955d2e1ccddd889d25c"
    "4eaef61aa72763bc44db9696697be7587e35d2efb2a625e7ac19b05f8c348086114103ee042a5a4041683e39c4ac0c460118"
    "00221e323033302d30312d30325430333a30343a30352e3132333435363738395a28073080f092cbdd0842408904024a1b09"
    "b52636334f17b9098f648f9a00214e6c6c89bb954c01300b00f54d085ddcacbe42952f2f819d70a48ff453d13329bb775d66"
    "e5a4b6165c38a40a4a76a56354544c1b00000045d964b8006556616c7565581f2f697066732f6261666b716163336a6f6278"
    "68676964736e3572777734796b6853657175656e6365076856616c6964697479581e323033302d30312d30325430333a30343a"
    "30352e3132333435363738395a6c56616c69646974795479706500"
)


def golden_record(scenario):
    require(scenario in ("dht_pk_put_get", "dht_ipns_put_get"), "unknown DHT value golden")
    value_hex = PK_HEX if scenario == "dht_pk_put_get" else IPNS_HEX
    value = bytes.fromhex(value_hex)
    return {"key_hex": ("2f706b2f" if scenario == "dht_pk_put_get" else "2f69706e732f") + IDENTITY_MULTIHASH,
            "value_hex": value_hex, "value_bytes": len(value), "value_sha256": hashlib.sha256(value).hexdigest()}


def _peer(value):
    require(isinstance(value, str) and bool(value) and value.strip() == value and "/" not in value,
            "missing actual process peer identity")
    return value


def _remote_socket(value, peer):
    return listener_socket(value, peer, True) if isinstance(value, str) and "/p2p/" in value else socket(value, True)


def _plugin_header(payload, spec, actor, *, final=True):
    require(isinstance(payload, dict) and type(payload.get("schema_version")) is int
            and payload["schema_version"] == 1 and payload.get("implementation") == "forge"
            and payload.get("role") == ("listener" if actor == "server" else "dialer")
            and payload.get("scenario") == spec.scenario and payload.get("status") == "ok"
            and payload.get("final") is final and payload.get("errors") == [] and "error" not in payload,
            "official DHT receipt is failed, relabeled or in the wrong completion phase")
    lifecycle = payload.get("lifecycle", {})
    require(set(lifecycle) == {"plugins_joined", "scheduler_joined"}
            and all(lifecycle[key] is final for key in lifecycle), "official plugin owner phase differs")
    require(isinstance(payload.get("current_phase"), str) and payload["current_phase"], "missing actual fixture phase")
    node = accepted_configuration(payload, spec, "listener" if actor == "server" else "dialer", "")
    require(node.get("dht") == {"profiles": [{"kind": "amino-v1", "mode": "server"}]},
            "accepted document does not select only native-default Amino server values")
    require(node["allow-insecure-test-mode"] is False
            and all(node["relay"][key] is False for key in
                    ("client-enabled", "server-enabled", "auto-discovery-enabled")),
            "security/relay booleans are not exact accepted policy values")
    require(payload.get("exchange") is None and "record_persisted" not in payload,
            "DHT value acceptance cannot substitute echo or claim cached GET persistence")
    return _peer(payload.get("local_peer_id"))


def _session(session, local, remote, address, direction):
    require(isinstance(session, dict) and session.get("source") == "node.diagnostics-api.session"
            and counter(session.get("session_id"), 1) and session.get("local_peer_id") == local
            and session.get("remote_peer_id") == remote and session.get("identify_state") == "identified"
            and session.get("authentication") == "quic_tls" and session.get("path") == "direct"
            and session.get("direction") == direction and "stream_authentication" not in session,
            "actual QUIC/Identify DHT session is absent or bound to another peer")
    require(isinstance(session.get("muxer"), str) and session["muxer"] != "/yamux/1.0.0",
            "QUIC was mislabeled as a Yamux application stream")
    socket(session.get("local_address"), True)
    socket(session.get("remote_address"), True)
    require(session["remote_address" if direction == "outbound" else "local_address"] == address,
            "actual authenticated socket differs from the sole server target")
    return session


def _diagnostics(payload, actor, *, phase="before-stop"):
    state = payload.get("diagnostics", {})
    profile = state.get("profile", {})
    require(state.get("source") == "node.diagnostics-api.snapshot" and state.get("phase") == phase
            and profile.get("protocol") == AMINO and profile.get("kind") == "amino-v1"
            and profile.get("mode") == "server" and profile.get("values") is True,
            "missing actual before-stop Amino diagnostics (not a post-join state proof)")
    require(counter(profile.get("persistence_failures"), 0, 0)
            and all(profile.get(key) is False for key in
                    ("persistence_degraded", "durability_uncertain", "persistence_closing", "persistence_closed")),
            "DHT persistence is failed/degraded or not open during the captured service snapshot")
    for key in ("routing_active", "routing_candidates"):
        require(counter(profile.get(key)), "invalid actual routing counter")
    for key in ("dht_queries", "dht_responses", "handshakes_completed", "sessions_opened", "active_sessions",
                "protocol_streams_opened", "protocol_streams_accepted"):
        require(counter(state.get(key)), "invalid actual DHT/session diagnostic counter")
    if phase == "before-stop":
        require(counter(state["handshakes_completed"], 1), "no actual authenticated service ingress/egress")
    if actor == "server" and phase == "before-stop":
        # These native metrics count inbound service RPCs, not outbound API
        # replies. Outbound proof is the API's accepted/responses/quorum result.
        require(counter(state["dht_queries"], 2) and counter(state["dht_responses"], 2),
                "official server did not observe the writer and reader service RPCs")
    return state


def _completion(payload, implementation, *, final):
    value = payload.get("dht_completion")
    require(payload.get("final") is final and isinstance(value, dict)
            and set(value) == {"source", "mode", "phase", "stop_observed", "owner_joined"}
            and value["source"] == implementation + "-fixture.dht-completion-barrier.v1"
            and value["mode"] == "stop-file" and value["phase"] == ("joined" if final else "await_stop")
            and value["stop_observed"] is final and value["owner_joined"] is final,
        "DHT fixture completion is missing, failed or in the wrong owner phase")
    if final and implementation == "rust":
        lifecycle = payload.get("fixture_task_lifecycle", {})
        require(lifecycle.get("fixture_owned_tasks_joined") is True and lifecycle.get("errors") == []
                and lifecycle.get("overflow") is False, "Rust DHT fixture tasks did not join successfully")


def validate_completion_witness(payload, spec, actor, peer, address):
    require(spec.completion_barrier(actor) and isinstance(payload, dict)
            and payload.get("status") == "operation_observed", "missing isolated donor DHT operation witness")
    _completion(payload, spec.implementation(actor), final=False)
    return validate_operation({**payload, "status": "ok"}, spec, actor, peer, address, witness=True)


def server_session_receipt(payload, spec, actor, witness, peer, address):
    """Only real running-server observations; donor and server IDs have separate namespaces."""
    local = validate_completion_witness(witness, spec, actor, peer, address)
    require(_plugin_header(payload, spec, "server", final=False) == peer, "running DHT server identity changed")
    state = _diagnostics(payload, "server", phase="running")
    service = payload.get("service", {})
    require(service.get("source") == "official-node.native-dht-service-and-diagnostics-api"
            and service.get("operation") == "serve" and service.get("profile") == AMINO
            and service.get("record_observation") == "not-queried", "server witness is not its actual native service")
    sessions = service.get("sessions")
    require(isinstance(sessions, list) and len(sessions) <= 64, "missing bounded server session observations")
    matches = [s for s in sessions if isinstance(s, dict) and s.get("remote_peer_id") == local]
    if not matches:
        return None
    require(len(matches) == 1, "ambiguous actual server session for " + actor)
    require(counter(state["handshakes_completed"], 1), "session observation has no actual handshake")
    return _session(matches[0], peer, local, listener_socket(address, peer, True), "inbound")


def validate_operation(payload, spec, actor, peer, address, *, witness=False):
    """Validate an isolated operation before starting the next process, or raise."""
    require(actor in ("writer", "reader"), "only writer/reader may perform a value operation")
    expected_mode = "put_only" if actor == "writer" else "get_only"
    implementation = spec.implementation(actor)
    numeric = listener_socket(address, peer, True)
    golden = golden_record(spec.scenario)
    if implementation == "forge":
        local = _plugin_header(payload, spec, actor)
        require(local != peer, "official DHT operation queried itself")
        service = payload.get("service", {})
        require(service.get("source") == "official-node.dht-api" and service.get("operation") == expected_mode
                and service.get("profile") == AMINO and service.get("expected_peer_id") == peer
                and service.get("dial_address") == address and service.get("quorum_reached") is True
                and service.get("record_matches_golden") is True and "record_persisted" not in service,
                "official isolated DHT operation failed quorum or changed its native API/target")
        selected = service.get("record", {})
        require(isinstance(selected, dict) and counter(selected.get("value_bytes"), 1) and selected == golden,
                "actual Forge selected key/value bytes/hash differ from the golden record")
        session = _session(service.get("session"), local, peer, numeric, "outbound")
        connect = service.get("connect", {})
        require(connect.get("source") == "node.api.connect" and counter(connect.get("session_id"), 1)
                and connect["session_id"] == session["session_id"] and connect.get("remote_peer_id") == peer
                and connect.get("muxer") == session["muxer"] and counter(connect.get("identify_state")),
                "DHT API receipt does not match its real connect result")
        if actor == "writer":
            require(counter(service.get("attempted"), 1) and counter(service.get("accepted"), 1)
                    and service["accepted"] <= service["attempted"], "PUT has no remote accepted record")
        else:
            require(counter(service.get("responses"), 1) and counter(service.get("valid_records"), 1)
                    and service["valid_records"] <= service["responses"], "GET is local/cache-only or has no valid remote record")
        _diagnostics(payload, actor)
        return local
    require(isinstance(payload, dict) and payload.get("implementation") == implementation
            and payload.get("role") == "dialer" and payload.get("scenario") == spec.scenario
            and payload.get("status") == "ok" and "error" not in payload
            and payload.get("operation") == expected_mode and counter(payload.get("value_bytes"), 1)
            and payload["value_bytes"] == golden["value_bytes"], "donor isolated DHT operation did not succeed")
    local = _peer(payload.get("local_peer_id"))
    require(local != peer and payload.get("authenticated_remote_peer_id") == peer,
            "donor actual authenticated local/remote identity is missing or self-queried")
    require(_remote_socket(payload.get("connection_remote_addr"), peer) == numeric,
            "donor actual connection is not to the sole owned server")
    if actor == "reader":
        require(payload.get("remote_get") is True, "donor GET was not the isolated fresh remote reader")
    else:
        require(payload.get("remote_get") is None or payload["remote_get"] is False,
                "donor writer combined PUT with a local GET")
    require(isinstance(payload.get("connection_id"), str) and payload["connection_id"],
            "donor DHT identity/socket lacks its actual connection ID")
    if implementation == "go":
        require(payload.get("connection_witness_source") == "go-libp2p.network.Conn",
                "Go DHT witness is not sourced from the actual native connection")
    if spec.completion_barrier(actor) and not witness:
        _completion(payload, implementation, final=True)
    # Donor code checks GetValue against the same golden internally, but does
    # not export actual bytes/hash. Never synthesize a donor hash or count/quorum.
    return local


def validate_case(record, spec):
    try:
        require(record.get("suite") == "plugin-dht" and record.get("scenario_id") == spec.scenario_id
                and record.get("spec") == vars(spec), "DHT registration differs from the closed 16-case matrix")
        endpoints = record.get("endpoints", {})
        require(set(endpoints) == {"writer", "server", "reader"}, "three independent process owners are required")
        peer = _peer(record.get("peer_id"))
        numeric = listener_socket(record.get("listen_address"), peer, True)
        for actor, endpoint in endpoints.items():
            require(endpoint.get("implementation") == spec.implementation(actor)
                    and endpoint.get("store_existed_before_spawn") is False, "actor implementation/store freshness relabeled")
            process = endpoint.get("process", {})
            terminal = process.get("terminal_status", {})
            require(counter(process.get("pid"), 1) and type(terminal.get("exit_code")) is int
                    and terminal == {"exit_code": 0, "termination": "graceful"}, "an actor was not gracefully joined")
        require(len({ep["process"]["pid"] for ep in endpoints.values()}) == 3, "DHT actors reused a process")
        ready = endpoints["server"]["process"].get("ready", {})
        require(ready.get("peer_id") == peer and ready.get("implementation") == spec.implementation("server")
                and ready.get("status") == "ready" and ready.get("role") == "listener"
                and record["listen_address"] in ready.get("listen_addrs", []), "server target is not its actual readiness")
        locals_by_actor = {actor: validate_operation(endpoints[actor].get("result"), spec, actor,
                                                   peer, record["listen_address"]) for actor in ("writer", "reader")}
        require(len({peer, *locals_by_actor.values()}) == 3, "Writer, Server and fresh Reader identities must all differ")
        server = endpoints["server"].get("result")
        if spec.forge_role == "server":
            require(_plugin_header(server, spec, "server") == peer, "official server identity changed after readiness")
            _diagnostics(server, "server")
            service = server.get("service", {})
            require(service.get("source") == "official-node.native-dht-service-and-diagnostics-api"
                    and service.get("operation") == "serve" and service.get("profile") == AMINO
                    and service.get("record_observation") == "not-queried" and "record_persisted" not in service,
                    "server snapshot was relabeled as a value or persistence proof")
            sessions = service.get("sessions")
            require(isinstance(sessions, list) and len(sessions) <= 64, "missing bounded actual server session observations")
            require(isinstance(record.get("server_session_receipts"), dict)
                    and set(record["server_session_receipts"]) == {"writer", "reader"},
                    "both exact pre-stop server observations are required")
            for actor, local in locals_by_actor.items():
                matches = [s for s in sessions if isinstance(s, dict) and s.get("remote_peer_id") == local]
                require(len(matches) == 1, "actual identified server session for " + actor + " is missing/ambiguous")
                _session(matches[0], peer, local, numeric, "inbound")
                receipt = record.get("server_session_receipts", {}).get(actor)
                _session(receipt, peer, local, numeric, "inbound")
                require(receipt == matches[0], "pre-stop server observation differs from its final owned capture")
        else:
            require(isinstance(server, dict) and server.get("implementation") == spec.donor
                    and server.get("role") == "listener" and server.get("scenario") == spec.scenario
                    and server.get("status") == "ok" and "error" not in server, "donor server did not finish successfully")
            # A donor listener's record_persisted flag is not used as value
            # evidence. Acceptance rests on the new, isolated remote GET.
        expected = ("server_started", "server_ready", "writer_started", "writer_joined",
                    "reader_started", "reader_joined", "server_joined")
        if spec.forge_role == "server":
            expected = ("server_started", "server_ready", "writer_started", "writer_server_session_observed",
                        "writer_joined", "reader_started", "reader_server_session_observed", "reader_joined", "server_joined")
        timeline = record.get("timeline", [])
        require(record.get("timeline_source") == "runner.process_lifecycle"
                and [entry.get("event") for entry in timeline] == list(expected), "owned W-join-before-fresh-R ordering missing")
        for entry, event in zip(timeline, expected):
            require(counter(entry.get("monotonic_ns"))
                    and entry.get("pid") == endpoints[event.split("_")[0]]["process"]["pid"], "timeline owner/time differs")
        times = [entry["monotonic_ns"] for entry in timeline]
        by_event = {entry["event"]: entry for entry in timeline}
        if spec.forge_role == "server":
            for actor in ("writer", "reader"):
                observed = by_event[actor + "_server_session_observed"]
                session = record["server_session_receipts"][actor]
                require(counter(observed.get("session_id"), 1) and observed.get("session_id") == session["session_id"]
                        and observed.get("remote_peer_id") == locals_by_actor[actor]
                        and observed["monotonic_ns"] < by_event[actor + "_joined"]["monotonic_ns"],
                        "server session receipt did not precede exact actor join")
        require(times == sorted(times) and by_event["writer_joined"]["monotonic_ns"] < by_event["reader_started"]["monotonic_ns"]
                and record.get("operation_budget_seconds") == CASE_SECONDS
                and by_event["reader_joined"]["monotonic_ns"] - times[0] <= CASE_SECONDS * 10**9,
                "operation ordering or overall budget violated")
        return []
    except (ValueError, TypeError, KeyError, AttributeError, IndexError) as error:
        return ["plugin-dht evidence: " + str(error)]


def validate_terminal_sources(record, binary_paths, artifact_root):
    """Bind final receipts/readiness to exact commands and post-join captures."""
    try:
        spec = Case(**record["spec"])
        root = Path(artifact_root).resolve()
        endpoints = record["endpoints"]
        owners = record.get("owned_processes", [])
        attempts = record.get("attempts", [])
        require(len(owners) == len(attempts) == 3, "exactly three owned attempts, without retries, are required")
        stores = []
        workdirs = []
        for actor in ("server", "writer", "reader"):
            endpoint = endpoints[actor]
            process = endpoint["process"]
            require(sum(owner == process for owner in owners) == 1, "actor is not its unique actual case owner")
            log = Path(process["log_file"]).resolve()
            log.relative_to(root)
            work = log.parent
            require(log.name == actor + ".log", "actor log was rebound")
            workdirs.append(work)
            argv = command(spec, actor, binary_paths[spec.implementation(actor)], work,
                           peer=record["peer_id"], address=record["listen_address"])
            require(process.get("command") == argv, "actor command changed binary/scenario/backend/mode/sole server target")
            store = (work / actor).with_suffix(".store").resolve()
            store.relative_to(root)
            stores.append(store)
            require(endpoint.get("store_existed_before_spawn") is False, "actor store was not fresh")
            matches = [a for a in attempts if a.get("kind") == actor]
            require(len(matches) == 1, "actor has absent/duplicate attempts")
            attempt = matches[0]
            require(attempt.get("command") == argv and attempt.get("scenario_id") == spec.scenario_id
                    and type(attempt.get("attempt_id")) is int and attempt["attempt_id"] == 1
                    and attempt.get("pid") == process["pid"] and type(attempt.get("exit_code")) is int
                    and attempt["exit_code"] == 0 and attempt.get("terminal_status") == process["terminal_status"]
                    and attempt.get("outputs") == process.get("outputs")
                    and attempt.get("requested_log_file") == attempt.get("log_file") == str(log),
                    "attempt provenance does not bind the actual joined process")
            seconds = attempt.get("timeout_seconds")
            require(type(seconds) in (int, float) and math.isfinite(seconds) and 0 < seconds <= 20,
                    "missing bounded actual command attempt")
            if endpoint["implementation"] == "forge":
                require(process.get("stop_budget") == {"native_close_seconds": 10, "post_stop_seconds": 0,
                        "scheduler_allowance_seconds": 10, "seconds": 20}, "official plugin join budget changed")
                accepted = endpoint["result"]["accepted_configuration"]["document"]
                require(accepted["plugins"]["db"]["store"]["stores"][0]["path"] == str(store),
                        "accepted ObjectDB store is not this actor's private launcher path")
            paths = [("--result-file", work / (actor + ".json"), endpoint["result"])]
            require(endpoint.get("result_file") == str(paths[0][1]), "inline result was rebound to another actor")
            if actor == "server":
                paths.append(("--ready-file", work / "server.ready.json", process["ready"]))
            elif spec.completion_barrier(actor):
                ready = process.get("ready", {})
                validate_completion_witness(ready, spec, actor, record["peer_id"], record["listen_address"])
                paths.append(("--ready-file", work / (actor + ".ready.json"), ready))
                final = endpoint["result"]
                require(all(final.get(key) == value for key, value in ready.items()
                            if key not in ("status", "final", "dht_completion")),
                        "final DHT operation changed its held witness facts")
            else:
                require(process.get("ready") == {}, "writer/reader substituted another process's readiness")
            for flag, path, inline in paths:
                path.resolve().relative_to(root)
                captures = [c for c in process.get("outputs", []) if c.get("argument") == flag
                            and c.get("path") == str(path) and c.get("exists") is True]
                require(len(captures) == 1, "missing unique terminal-owned " + flag + " capture")
                captured = Path(captures[0]["log_file"]).resolve()
                captured.relative_to(root)
                require(captured != path.resolve() and load_json(path) == load_json(captured) == inline,
                        "inline receipt differs from its actual file/post-join capture")
        require(len(set(stores)) == 3 and len(set(workdirs)) == 1, "actors shared stores or escaped the case owner")
        return []
    except (ValueError, TypeError, KeyError, AttributeError, IndexError, OSError) as error:
        return ["plugin-dht terminal sources: " + str(error)]
