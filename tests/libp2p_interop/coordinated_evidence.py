"""Validate actor-native coordinated receipts without manufacturing a flat proof.

The shared reuse entry point can dispatch here for the canonical actual schema.
Legacy barrier-stream receipts must not be synthesized from this SYN barrier.
"""

import hashlib
import ipaddress
import math
from pathlib import Path
import re

from autorelay_acceptance import _options

ECHO_PROTOCOL = "/forge/interop/relay-echo/1"

REUSE_DIRECTIONS = {"forge_to_go", "go_to_forge", "forge_to_rust", "rust_to_forge"}
REUSE_PROFILES = {"native", "private"}
REUSE_RUNNER_IDS = {"native": "tcp_stage6/coordinated_dial_port_reuse",
                    "private": "private_tcp_yamux_pnet/coordinated_dial_port_reuse_private_pnet"}


class EvidenceError(ValueError):
    pass


def _require(condition, message):
    if not condition:
        raise EvidenceError(message)


def _obj(value):
    _require(isinstance(value, dict), "missing object receipt")
    return value


def _rows(value, *, maximum=256):
    _require(isinstance(value, list) and len(value) <= maximum, "missing or unbounded native rows")
    _require(all(isinstance(row, dict) for row in value), "invalid native row")
    return value


def _equal(actual, expected, message):
    _require(type(actual) is type(expected) and actual == expected, message)
    if isinstance(expected, dict):
        for key in expected:
            _equal(actual[key], expected[key], message)
    elif isinstance(expected, (list, tuple)):
        for value, wanted in zip(actual, expected):
            _equal(value, wanted, message)


def _id(value):
    return (type(value) is int and value > 0) or (isinstance(value, str) and 0 < len(value) <= 256)


def _token(token):
    _require(isinstance(token, str) and re.fullmatch(r"[0-9a-f]{32}", token), "invalid bounded case token")


def challenge(token):
    _token(token)
    return f"coordinated:{token}".encode("ascii")


def _endpoint(value, peer=None):
    _require(isinstance(value, str), "missing actual TCP endpoint")
    parts = value.split("/")
    _require(len(parts) in (5, 7) and parts[:2] == ["", "ip4"] and parts[3] == "tcp",
             "endpoint is not concrete native IPv4 TCP")
    if len(parts) == 7:
        _require(parts[5] == "p2p" and parts[6] == peer, "endpoint authenticated peer suffix differs")
    ip = ipaddress.ip_address(parts[2])
    _require(ip.version == 4 and not ip.is_unspecified and not ip.is_multicast, "nonconcrete socket IP")
    _require(parts[4].isdigit() and str(int(parts[4])) == parts[4] and 0 < int(parts[4]) < 65536,
             "nonconcrete socket port")
    return str(ip), int(parts[4])


def _listener(ready):
    return _endpoint(ready["listener_address"], ready["peer_id"])


def _errors(action):
    try:
        action()
    except (ValueError, TypeError, KeyError, IndexError, AttributeError) as error:
        return [str(error)]
    return []


def validate_ready(value, implementation, token, bind_ip):
    def check():
        value_object = _obj(value)
        _token(token)
        _equal(value_object.get("implementation"), implementation, "listener implementation differs")
        _equal(value_object.get("case_token"), token, "listener token differs")
        _equal(value_object.get("status"), "ready", "native listener is not ready")
        peer = value_object.get("peer_id")
        _require(isinstance(peer, str) and 0 < len(peer) <= 256 and "/" not in peer, "missing native listener identity")
        ip, port = _endpoint(value_object.get("listener_address"), peer)
        _equal(ip, bind_ip, "actual listener is not in its owned participant")
        _equal(value_object.get("listener_port"), port, "reported listener port differs from actual listener")
        _equal(value_object.get("listen_addrs"), [f"/ip4/{ip}/tcp/{port}/p2p/{peer}"], "ambiguous or misattributed listener")
        if implementation == "rust":
            _equal(value_object.get("preexisting_connection_ids"), [], "Rust listener already has native owners")
    return _errors(check)


def _event(value, kind):
    matches = [e for e in _rows(value.get("events")) if e.get("kind") == kind]
    _require(len(matches) == 1, f"missing or ambiguous native {kind}")
    return matches[0]


def started_receipt(value, implementation, token):
    if not isinstance(value, dict) or value.get("implementation") != implementation or value.get("case_token") != token:
        return False
    if implementation == "rust":
        return not _errors(lambda: _equal(_event(value, "control_completed").get("completion"),
                                         "native_dial_admitted", "native start acknowledgement absent"))
    return (value.get("status") in {"started", "connected", "exchanged"}
            and value.get("operation_admitted") is True
            and value.get("admission_source") == "coordinated_actor.preflight")


def _connection(value, implementation):
    if implementation == "rust":
        socket = _obj(value.get("socket"))
        return {"connection_id": socket.get("native_connection_id"),
                "local_peer_id": socket.get("authenticated_local_peer_id"),
                "remote_peer_id": socket.get("authenticated_remote_peer_id"),
                "local_address": socket.get("local_address"), "remote_address": socket.get("remote_address"),
                "security": socket.get("security"), "muxer": socket.get("muxer")}
    return _obj(_obj(value.get("receipt")).get("connection"))


def _connection_check(value, implementation, token, ready, remote, role):
    _obj(value)
    _equal(value.get("implementation"), implementation, "connection implementation differs")
    _equal(value.get("case_token"), token, "connection token differs")
    connection = _connection(value, implementation)
    _require(_id(connection.get("connection_id")), "missing actual native connection ID")
    _equal(connection.get("local_peer_id"), ready["peer_id"], "native local authenticated identity differs")
    _equal(connection.get("remote_peer_id"), remote["peer_id"], "native remote authenticated identity differs")
    _equal(_endpoint(connection.get("local_address"), ready["peer_id"]), _listener(ready),
           "ephemeral local socket is not listener-port reuse")
    _equal(_endpoint(connection.get("remote_address"), remote["peer_id"]), _listener(remote),
           "native remote socket is not the other owned listener")
    _require(connection.get("security") in {"/noise", "/tls/1.0.0"}, "native authentication is absent")
    _equal(connection.get("muxer"), "/yamux/1.0.0", "native inner Yamux is absent")
    if implementation == "rust":
        socket = value["socket"]
        _equal(socket.get("basis"), "unique_native_transport_output_and_swarm_endpoint", "Rust socket binding basis differs")
        _equal(socket.get("socket_source"), "rust.tcp.TcpStream.local_addr_peer_addr", "Rust lacks actual socket names")
        _equal(socket.get("connection_direction"), "outbound", "Rust native outgoing dial did not win")
    else:
        receipt = value["receipt"]
        source = {"forge": "forge.node.diagnostics.authenticated-session", "go": "go-libp2p.network.Conn.ConnState"}[implementation]
        _equal(connection.get("source"), source, "connection lacks actor-native authentication source")
        _equal(connection.get("transport"), "tcp", "coordinated owner is not native TCP")
        _require(receipt.get("physical_direction") in {"outbound", "inbound"}, "missing actual TCP direction")
        _equal(receipt.get("native_outgoing_winner"), receipt["physical_direction"] == "outbound", "outgoing winner contradicts actual native direction")
        _equal(receipt.get("direction_source"), {"forge": "forge.node.diagnostics.session.direction",
               "go": "go-libp2p.swarm.Conn.Stat"}[implementation], "TCP direction lacks its actual native source")
        if role == "source":
            _equal(receipt.get("native_outgoing_winner"), True, "source accepted socket masquerades as native outgoing winner")
            _equal(receipt.get("physical_direction"), "outbound", "source winner is not physically outbound")
    return connection


def validate_connected(value, implementation, token, ready, remote, role):
    return _errors(lambda: _connection_check(value, implementation, token, ready, remote, role))


def _roles(value, implementation, role):
    expected = "initiator" if role == "source" else "responder"
    if implementation == "rust":
        socket = value["socket"]
        _equal(value.get("coord_role"), expected, "Rust requested coordinated role differs")
        _equal(socket.get("security_role"), "client" if role == "source" else "server", "Rust actual security role differs")
        _equal(socket.get("security_role_basis"), "completed_native_security_and_muxer_delegates", "Rust role is requested rather than observed")
        return
    _equal(value.get("actor_role"), expected, "actor coordinated role differs")
    receipt = value["receipt"]
    roles = _obj(receipt.get("roles"))
    _equal(roles.get("security_role"), expected, "actual security role differs")
    _equal(roles.get("yamux_role"), expected, "actual Yamux role differs")
    if implementation == "go":
        _equal(receipt.get("role_source"), "security.SecureInbound/SecureOutbound+yamux.NewConn", "Go role is requested rather than observed")
    else:
        _equal(receipt.get("role_source"), "forge.node.diagnostics.session.security_role+yamux_role",
               "Forge role is requested rather than completed native delegate facts")


def _body(value, token):
    body = _obj(value)
    payload = challenge(token)
    framed = bytes([len(payload)]) + payload
    for key, expected in (("framed_bytes", len(framed)), ("frames", 1), ("complete_frames", True),
                          ("invalid_or_over_limit", False), ("framed_sha256", hashlib.sha256(framed).hexdigest())):
        _equal(body.get(key), expected, "missing actual complete challenge frame I/O")


def _frame(value, token):
    frame = _obj(value)
    payload = challenge(token)
    _equal(frame.get("raw"), False, "receipt is not framed I/O")
    _equal(frame.get("framed_hex"), (bytes([len(payload)]) + payload).hex(), "actual challenge frame differs")
    _body(frame.get("read"), token)


def _application(value, implementation, token, role, connection):
    application = _obj(value.get("application") if implementation == "rust" else value["receipt"].get("application"))
    _equal(application.get("protocol"), ECHO_PROTOCOL, "application protocol differs")
    if implementation == "rust":
        _equal(application.get("basis"), "unique_retained_connection_and_actual_new_stream_framed_io", "Rust lacks actual retained-owner I/O binding")
        _equal(application.get("native_connection_id"), connection["connection_id"], "Rust probe replaced the native connection")
        _equal(application.get("connection_trace_id"), value["socket"].get("connection_trace_id"), "Rust probe trace owner differs")
        _require(_id(application.get("stream_trace_id")), "missing real Rust stream ID")
        _equal(application.get("stream_direction"), "outbound" if role == "source" else "inbound", "Rust actual application direction differs")
        _equal(application.get("write_close_returned"), True, "Rust echo did not close its native stream")
        _body(application.get("request"), token)
        _body(application.get("response"), token)
    else:
        _equal(application.get("connection_id"), connection["connection_id"], "probe replaced the retained native connection")
        _equal(application.get("fresh_dial"), False, "probe performed a replacement ordinary dial")
        if implementation == "forge":
            _require(_id(application.get("stream_id")), "missing actual Forge application stream")
            _equal(application.get("dial_observation_source"), "forge.node.metrics.sealed-gater-rejections",
                   "Forge probe lacks native admission observation")
            for key in ("native_dial_attempts_before", "native_dial_attempts_after"):
                _equal(application.get(key), 0, "Forge retained probe attempted a replacement dial")
        _frame(application.get("request"), token)
        _frame(application.get("response"), token)


def _rust_raw(value, ready, remote, role):
    raw = _obj(value.get("raw_upgrade_observations"))
    _equal(raw.get("overflow"), False, "Rust upgrade capture overflow")
    _equal(raw.get("fixture_owned_tasks_joined"), True, "Rust raw observation was not finalized after task join")
    connections, events = _rows(raw.get("connections")), _rows(raw.get("swarm_events"))
    _require(len(connections) == len(events) == 1, "Rust native owner is ambiguous")
    connection, event = connections[0], events[0]
    socket, application = value["socket"], value["application"]
    direction = "outbound" if role == "source" else "inbound"
    _equal(socket.get("raw_upgrade_role"), direction, "Rust requested override is not its completed upgrade role")
    _equal(connection.get("connection_trace_id"), socket.get("connection_trace_id"), "Rust raw connection trace differs")
    for key, expected in (("authenticated_local_peer_id", ready["peer_id"]), ("authenticated_remote_peer_id", remote["peer_id"]),
                          ("local_address", socket["local_address"]), ("remote_address", socket["remote_address"]),
                          ("direction", "outbound"), ("selected_security", "/noise"), ("selected_muxer", "/yamux/1.0.0"),
                          ("security_complete", True), ("muxer_complete", True), ("security_delegate_completed", True),
                          ("muxer_delegate_completed", True), ("overflow", False)):
        _equal(connection.get(key), expected, "Rust native socket/security output differs")
    _equal(event.get("swarm_connection_id"), socket["native_connection_id"], "Rust Swarm connection ID differs")
    _equal(event.get("authenticated_remote_peer_id"), remote["peer_id"], "Rust Swarm authenticated peer differs")
    endpoint = _obj(event.get("endpoint"))
    _equal(endpoint.get("direction"), "outbound", "Rust accepted socket is not the outgoing native owner")
    _equal(endpoint.get("upgrade_role"), direction, "Rust Swarm role override differs")
    _equal(_endpoint(endpoint.get("remote_address"), remote["peer_id"]), _listener(remote), "Rust Swarm target differs")
    resolved = _obj(connection.get("endpoint"))
    _equal(resolved.get("direction"), "outbound", "Rust resolved native direction differs")
    _equal(resolved.get("upgrade_role"), direction, "Rust resolved native role differs")
    _equal(_endpoint(resolved.get("remote_address"), remote["peer_id"]), _listener(remote), "Rust resolved native target differs")
    outputs = _rows(connection.get("transport_output_receipts"))
    _require(len(outputs) == 1, "Rust transport output binding is missing or ambiguous")
    output = outputs[0]
    boundary = output.get("after_event_sequence")
    _require(type(boundary) is int and 0 <= boundary <= len(_rows(connection.get("events"))), "Rust transport output boundary differs")
    for key, expected in (("basis", "donor_transport_output_identity"), ("connection_trace_id", socket["connection_trace_id"]),
                          ("authenticated_remote_peer_id", remote["peer_id"]), ("request_endpoint", endpoint),
                          ("resolved_endpoint", connection.get("endpoint")), ("local_address", socket["local_address"]),
                          ("remote_address", socket["remote_address"])):
        _equal(output.get(key), expected, "Rust actual transport output receipt differs")
    _equal(output.get("dns_wrapper_enabled"), False, "coordinated proof must not substitute DNS resolution")
    phases = _rows(connection.get("negotiations"))
    _require(len(phases) == 2, "Rust lacks security and inner Yamux observations")
    for phase in phases:
        _equal(phase.get("direction"), direction, "Rust actual security/Yamux role differs")
        _equal(phase.get("io_failed"), False, "Rust upgrade I/O failed")
        _equal(phase.get("parser_error"), None, "Rust upgrade parser failed")
    streams = _rows(connection.get("streams"))
    identities = [stream.get("stream_trace_id") for stream in streams]
    _require(all(_id(identity) for identity in identities) and len(set(identities)) == len(identities),
             "Rust native stream IDs are missing or duplicated")
    echo_streams = []
    for stream in streams:
        if stream.get("protocol") == ECHO_PROTOCOL:
            echo_streams.append(stream)
        else:
            _require(stream.get("protocol") in {"/ipfs/id/1.0.0", "/ipfs/id/push/1.0.0"},
                     "unexpected Rust protocol beside coordinated echo/Identify")
            _equal(stream.get("io_failed"), False, "Rust Identify stream I/O failed")
            _equal(stream.get("parser_error"), None, "Rust Identify stream parser failed")
    _require(len(echo_streams) == 1, "Rust echo stream is missing or ambiguous")
    stream = echo_streams[0]
    for key, expected in (("stream_trace_id", application["stream_trace_id"]), ("direction", application["stream_direction"]),
                          ("protocol", ECHO_PROTOCOL), ("io_failed", False), ("parser_error", None),
                          ("write_close_returned", True), ("drop_observed", True),
                          ("read", application["response"]), ("write", application["request"])):
        _equal(stream.get(key), expected, "Rust actual retained stream differs")
    if role == "source":
        _equal(raw.get("complete"), True, "Rust independent native application output binding incomplete")
    audit = _rows(value.get("native_transport_dials"))
    _require(len(audit) == 1, "Rust probe requested a fresh native transport dial")
    _equal(_endpoint(audit[0].get("address"), remote["peer_id"]), _listener(remote), "Rust actual dial target differs")
    _equal(audit[0].get("requested_role"), "dialer" if role == "source" else "listener", "Rust actual native DialOpts context differs")
    _equal(audit[0].get("port_use"), "Reuse", "Rust native context disabled listener reuse")
    events = _rows(value.get("events"))
    _equal([e.get("sequence") for e in events], list(range(1, len(events) + 1)), "Rust actor event sequence is not contiguous")
    times = [e.get("mono_ns") for e in events]
    _require(all(type(t) is int and t > 0 for t in times) and times == sorted(times), "Rust actor event time is invalid")
    admitted, connected, completed = (_event(value, kind) for kind in ("native_dial_admitted", "native_connection_established", "application_completed"))
    _require(admitted["sequence"] < connected["sequence"] < completed["sequence"], "Rust operation/application order differs")
    for row in (admitted, connected, completed):
        _equal(row.get("native_connection_id"), socket["native_connection_id"], "Rust operation owner differs")
    _equal(admitted.get("expected_peer_id"), remote["peer_id"], "Rust admitted peer differs")
    for key in ("native_transport_dials_before", "native_transport_dials_after"):
        _equal(completed.get(key), 1, "Rust echo caused an ordinary replacement dial")
    join = _obj(value.get("task_join"))
    for key, expected in (("fixture_owned_tasks_joined", True), ("overflow", False), ("errors", [])):
        _equal(join.get(key), expected, "Rust native task cleanup did not join")


def _actor(value, implementation, token, ready, remote, role, profile):
    connection = _connection_check(value, implementation, token, ready, remote, role)
    _equal(value.get("status"), "ok", "actor did not complete its actual native probe")
    _equal(value.get("joined"), True, "actor-native workers did not join")
    _require(value.get("error") is None, "actor reported an error")
    _roles(value, implementation, role)
    _application(value, implementation, token, role, connection)
    if implementation == "rust":
        _equal(value.get("schema"), "forge.p2p.evidence.coordinated.v1", "Rust terminal native schema differs")
        _equal(value.get("peer_id"), ready["peer_id"], "Rust terminal native identity differs")
        _equal(value.get("timeout_ms"), 20000, "Rust actual native operation budget differs")
        _equal(value.get("per_call_cancel_supported"), False, "Rust host stop must not claim per-call cancellation")
        _equal(value.get("finalized"), True, "Rust result is not terminal")
        _rust_raw(value, ready, remote, role)
    else:
        _equal(value.get("schema_version"), 1, "actor terminal native schema differs")
        _equal(value.get("local_peer_id"), ready["peer_id"], "terminal native identity differs")
        _equal(value.get("native_dial_joined"), True, "native coordinated dial workers did not retire")
        _equal(value.get("operation_admitted"), True, "coordinated operation was never admitted")
        if implementation == "go":
            _equal(connection.get("security"), "/noise", "Go native coordinated transport lacks completed Noise")
            sockets = _rows(value["receipt"].get("native_dial_sockets"))
            _require(len(sockets) == 1, "Go native dial socket is absent or ambiguous")
            socket = sockets[0]
            local = _listener(ready)
            other = _listener(remote)
            for key, expected in (("source", "net.Dialer.DialContext.returned-socket"),
                                  ("local", f"{local[0]}:{local[1]}"), ("remote", f"{other[0]}:{other[1]}"),
                                  ("simultaneous_connect", True), ("is_client", role == "source"), ("reason", token)):
                _equal(socket.get(key), expected, "Go actual native simultaneous-connect socket/context differs")
            _equal(value.get("connections_after_stop"), 0, "Go retained native connection survived terminal cleanup")
        else:
            _equal(_obj(value.get("resources")).get("file_descriptors"), 0, "Forge native sockets survived cleanup")
    if profile == "private":
        _equal(connection.get("security"), "/noise", "private profile lacks inner Noise")
        if implementation == "rust":
            _equal(value.get("pnet_fingerprint_basis"), "installed_native_pnet_psk_operational_sha256_v1",
                   "Rust installed protector fingerprint is not derived from the actual native PSK")


def validate_native_receipts(artifact):
    """Actual actor adapters for the shared reuse contract; no synthetic receipts."""
    def check():
        record = _obj(artifact)
        spec = _obj(record.get("case"))
        direction = f"{spec.get('source')}_to_{spec.get('destination')}"
        _require(direction in REUSE_DIRECTIONS and spec.get("profile") in REUSE_PROFILES, "not an owned reuse direction/profile")
        profile, token = spec["profile"], record.get("case_token")
        _token(token)
        identifier = f"coordinated.{profile}.{direction}"
        _equal(spec.get("identifier"), identifier, "reuse case identity differs")
        _equal(record.get("scenario_id"), identifier, "reuse record identity differs")
        _equal(spec.get("transport"), "tcp-pnet-noise" if profile == "private" else "tcp", "reuse transport/profile differs")
        _equal(record.get("runner_scenario_id"), REUSE_RUNNER_IDS[profile], "reuse runner identity differs")
        scenario = "coordinated_dial_port_reuse" + ("_private_pnet" if profile == "private" else "")
        _equal(record.get("scenario"), scenario, "reuse scenario differs")
        raw = _obj(record.get("raw"))
        _equal(set(raw), {"source", "destination"}, "reuse is not exactly two native actors")
        _require(raw["source"]["ready"]["peer_id"] != raw["destination"]["ready"]["peer_id"], "both actors claim the same identity")
        _equal(_connection(raw["source"]["result"], spec["source"]).get("security"),
               _connection(raw["destination"]["result"], spec["destination"]).get("security"),
               "bilateral actors disagree on actual negotiated authentication")
        for role, bind_ip in (("source", "11.0.0.1"), ("destination", "11.0.0.2")):
            other = "destination" if role == "source" else "source"
            actor = _obj(raw[role])
            implementation = spec[role]
            ready, remote, result = actor["ready"], raw[other]["ready"], actor["result"]
            failures = validate_ready(ready, implementation, token, bind_ip)
            _require(not failures, "; ".join(failures))
            _equal(result.get("scenario"), scenario, "terminal actor scenario differs")
            _actor(result, implementation, token, ready, remote, role, profile)
            fingerprint = result.get("pnet_fingerprint")
            if profile == "private":
                wanted = record.get("pnet_fingerprint")
                _require(isinstance(wanted, str) and re.fullmatch(r"[0-9a-f]{64}", wanted), "missing private operational fingerprint")
                _equal(fingerprint, wanted, "private actors installed different protector keys")
            else:
                _require(fingerprint is None or fingerprint == "", "native case unexpectedly claims private authentication")
            connected = _obj(_obj(record.get("phases")).get("connected"))[role]
            before = _connection_check(connected, implementation, token, ready, remote, role)
            _equal(before, _connection(result, implementation), "probe or terminal result replaced the captured connected owner")
            _equal(connected.get("status"), "connected", "probe ran without a prior connected receipt")
            exchanged = record["phases"]["exchanged"][role]
            _equal(exchanged.get("status"), "exchanged", "terminal host stop is not application evidence")
            _equal(_connection(exchanged, implementation), before, "probe replaced its preinstalled owner")
            _application(exchanged, implementation, token, role, before)
            final_app = result.get("application") if implementation == "rust" else result["receipt"].get("application")
            exchanged_app = exchanged.get("application") if implementation == "rust" else exchanged["receipt"].get("application")
            _equal(exchanged_app, final_app, "application receipt first appeared during host cleanup")
    return _errors(check)


def validate_barrier(artifact):
    def check():
        network = _obj(artifact.get("network"))
        _equal(network.get("kind"), "linux_coordinated_tcp_netns", "reuse lacks owned kernel barrier")
        _equal(network.get("state"), "closed", "owned network did not close")
        for key in ("cleanup_uncertainty", "cleanup_failures"):
            _equal(network.get(key), [], "network cleanup was uncertain")
        outer = _obj(network.get("outer_network"))
        for key in ("default_route", "external_links"):
            _equal(outer.get(key), "absent", "reuse escaped isolated owned network")
        participants = _rows(network.get("participants"), maximum=2)
        _require(len(participants) == 2 and {p.get("role") for p in participants} == {"client", "server"}, "barrier participants differ")
        namespaces = {p["role"]: p["namespace"] for p in participants}
        _require(len(set(namespaces.values())) == 2, "actors share a network namespace")
        coordination = _obj(network.get("coordination"))
        _equal(coordination.get("released"), True, "bilateral native SYN barrier was not fully released")
        snapshots = _obj(coordination.get("syn_sent"))
        _equal(set(snapshots), {"client", "server"}, "missing bilateral real SYN-SENT snapshots")
        commands = _rows(network.get("commands"), maximum=4096)
        installs, deletes, captured = [], [], {}
        for role, actor_role, ip in (("client", "source", "11.0.0.1"), ("server", "destination", "11.0.0.2")):
            other = "server" if role == "client" else "client"
            remote_role = "destination" if actor_role == "source" else "source"
            local = _listener(artifact["raw"][actor_role]["ready"])
            remote = _listener(artifact["raw"][remote_role]["ready"])
            _equal(local[0], ip, "barrier actor IP differs")
            _equal(coordination["ports"].get(role), local[1], "barrier held a configured rather than actual listener port")
            snapshot = _obj(snapshots[role])
            _equal(snapshot.get("local"), f"{local[0]}:{local[1]}", "SYN local tuple differs")
            _equal(snapshot.get("remote"), f"{remote[0]}:{remote[1]}", "SYN remote tuple differs")
            output = snapshot.get("stdout")
            _require(isinstance(output, str) and len(output) <= 8192, "missing bounded raw ss output")
            matches = [line for line in output.splitlines() if line.split()[-2:] == [snapshot["local"], snapshot["remote"]]]
            _require(len(matches) == 1, "actual SYN-SENT tuple is absent or ambiguous")
            command = snapshot.get("command")
            _require(isinstance(command, list) and len(command) == 9 and Path(command[0]).name == "ip"
                     and command[1:4] == ["netns", "exec", namespaces[role]] and Path(command[4]).name == "ss"
                     and command[5:] == ["-H", "-nt", "state", "syn-sent"], "snapshot was not actual namespace ss SYN-SENT")
            captured[role] = (command, output)
            _require(any(row.get("command") == command and row.get("stdout") == output and type(row.get("returncode")) is int
                         and row["returncode"] == 0 for row in commands), "SYN snapshot lacks actual successful owned command capture")
            rule = ["OUTPUT", "-p", "tcp", "-s", local[0], "-d", remote[0], "--sport", str(local[1]),
                    "--dport", str(remote[1]), "--tcp-flags", "SYN,ACK", "SYN", "-j", "DROP"]
            positions = []
            for operation in ("-A", "-D"):
                hits = [index for index, row in enumerate(commands) if isinstance(row.get("command"), list)
                        and row["command"][1:4] == ["netns", "exec", namespaces[role]]
                        and len(row["command"]) == 8 + len(rule) and Path(row["command"][4]).name == "iptables"
                        and row["command"][5:] == ["-w", "2", operation, *rule]
                        and type(row.get("returncode")) is int and row["returncode"] == 0]
                _require(len(hits) == 1, "held exact initial SYN rule was not installed/deleted exactly once")
                positions.append(hits[0])
            _require(positions[0] < positions[1], "SYN release preceded its owned rule install")
            installs.append(positions[0])
            deletes.append(positions[1])
        first_delete = min(deletes)
        _require(max(installs) < first_delete - 2, "release lacks fresh bilateral snapshots after both installs")
        for index, role in enumerate(("client", "server"), first_delete - 2):
            command, output = captured[role]
            _equal(commands[index].get("command"), command, "release did not freshly recheck both owned sockets")
            _equal(commands[index].get("stdout"), output, "release snapshot differs from actual fresh kernel output")
            _equal(commands[index].get("returncode"), 0, "release socket query failed")
        iptables = [i for i, row in enumerate(commands) if isinstance(row.get("command"), list)
                    and len(row["command"]) > 4 and Path(row["command"][4]).name == "iptables"]
        _equal(set(iptables), set(installs + deletes), "unexpected coordination firewall operation")
    return _errors(check)


def _owner_command(artifact, role, owner):
    network_role = "client" if role == "source" else "server"
    namespaces = {p["role"]: p["namespace"] for p in artifact["network"]["participants"]}
    command = owner.get("command")
    _require(isinstance(command, list) and len(command) >= 6 and all(isinstance(c, str) and c for c in command), "missing actual process argv")
    _require(Path(command[0]).is_absolute() and Path(command[0]).name == "ip"
             and command[1:4] == ["netns", "exec", namespaces[network_role]], "actor was not launched in its owned namespace")
    _require(Path(command[4]).is_absolute() and str(Path(command[4]).resolve()) == command[4], "noncanonical actor binary path")
    _equal(command[5], "coordinated-live", "ordinary fixture context masquerades as coordinated")
    options = _options(command[4:])
    spec = artifact["case"]
    work = Path(owner["log_file"]).parent
    _require(work.is_absolute() and work.name == spec["identifier"] and Path(owner["log_file"]) == work / f"{role}.log", "process log escaped exact case ownership")
    wanted = {"--scenario": artifact["scenario"], "--transport": spec["transport"],
              "--coord-role": "initiator" if role == "source" else "responder", "--case-token": artifact["case_token"],
              "--bind-ip": "11.0.0.1" if role == "source" else "11.0.0.2", "--timeout-ms": "20000"}
    wanted.update({f"--{name}-file": str(work / f"{role}.{name}") for name in ("ready", "result", "stop", "control", "plan")})
    if spec[role] == "forge":
        wanted["--store-dir"] = str(work / f"{role}.store")
    if spec["profile"] == "private":
        key = options.get("--pnet-key-file")
        _require(isinstance(key, str) and Path(key).is_absolute() and str(Path(key).resolve()) == key, "missing canonical private key file input")
        wanted.update({"--pnet-key-file": key, "--pnet-fingerprint": artifact["pnet_fingerprint"]})
    _equal(options, wanted, "actor command differs from exact coordinated native contract")
    budget = _obj(owner.get("stop_budget"))
    _equal(budget, {"native_close_seconds": 8, "post_stop_seconds": 0, "scheduler_allowance_seconds": 2, "seconds": 10}, "native actor stop budget differs")
    return options


def validate_case(artifact):
    """Canonical actual reuse semantics plus owned kernel/process evidence."""
    errors = list(validate_native_receipts(artifact))
    errors.extend(validate_barrier(artifact))
    def check():
        _equal(artifact.get("schema_version"), 1, "invalid coordinated artifact schema")
        _equal(artifact.get("suite"), "coordinated", "invalid coordinated suite")
        _equal(artifact.get("errors"), [], "coordinated execution failed")
        _equal(artifact.get("cleanup_errors"), [], "coordinated cleanup failed")
        _equal(artifact.get("acceptance_scenario_ids"), [artifact["scenario"]], "extra coordinated acceptance claims")
        elapsed = artifact.get("elapsed_seconds")
        _require(type(elapsed) in (int, float) and math.isfinite(elapsed) and 0 < elapsed <= 90, "unbounded coordinated lifecycle interval")
        processes = _rows(artifact.get("processes"), maximum=2)
        _require(len(processes) == 2, "missing exact bilateral process ownership")
        _require(len({p.get("pid") for p in processes}) == 2 and all(type(p.get("pid")) is int and p["pid"] > 0 for p in processes), "invalid or reused process owners")
        for role in ("source", "destination"):
            actor = artifact["raw"][role]
            owner = _obj(actor.get("process"))
            _require(sum(row == owner for row in processes) == 1, "raw actor is not its tracked process owner")
            _equal(owner.get("ready"), actor["ready"], "process listener identity differs")
            _equal(owner.get("terminal_status"), {"exit_code": 0, "termination": "graceful"}, "actor was killed or did not join")
            _owner_command(artifact, role, owner)
            _require(started_receipt(artifact["phases"]["started"][role], artifact["case"][role], artifact["case_token"]), "native start acknowledgement is absent")
            _equal(artifact["phases"]["exchanged"][role].get("status"), "exchanged", "probe was not completed before host stop")
            other = "destination" if role == "source" else "source"
            _equal(artifact["plans"][role], {"case-token": artifact["case_token"], "peer-id": artifact["raw"][other]["ready"]["peer_id"],
                   "addr": artifact["raw"][other]["ready"]["listen_addrs"][0]}, "peer plan differs from actual owned listener")
        _equal(artifact.get("controls"), [{"actor": role, "sequence": "1", "action": "start", "case-token": artifact["case_token"]}
                                        for role in ("source", "destination")]
               + [{"actor": "source", "sequence": "2", "action": "probe", "case-token": artifact["case_token"]}],
               "coordinated control sequence used an ordinary connect or responder probe")
    errors.extend(_errors(check))
    return errors
