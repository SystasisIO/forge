"""Fail-closed PR10 path semantics. Status, counters and config are not proof."""

import hashlib
import ipaddress
from pathlib import Path
import re

from path_wire import address_bytes, validate_dcutr_frame

ECHO_PROTOCOL = "/forge/interop/path-echo/1"
EVENT_LIMIT = 256
OUTCOMES = {"success", "failed", "cancelled"}
RUST_SOURCE_WAVE_SCOPE = "rust_source_native_wave_failure"


def rust_source_wave_case(case):
    return (case.get("source"), case.get("destination"), case.get("transport"), case.get("profile")) == (
        "rust", "forge", "quic", "native") and case.get("outcome") in {"failed", "cancelled"}


def require(value, message):
    if not value:
        raise ValueError(message)


def challenge(token, phase):
    require(isinstance(token, str) and re.fullmatch(r"[a-f0-9]{32}", token), "invalid case token")
    require(phase in {"relay_before", "direct_after", "relay_after"}, "invalid echo phase")
    return f"path:{token}:{phase}".encode("ascii")


def _id(value):
    return isinstance(value, str) and 0 < len(value) <= 128


def _ns(value):
    return type(value) is int and 0 < value < 2**63


def validate_frame(event):
    kind, addresses, receipt = event.get("message_type"), event.get("addresses"), event.get("receipt")
    require(type(kind) is int and kind in {100, 300}, "missing native DCUtR type")
    require(isinstance(addresses, list) and len(addresses) <= 32 and len(set(addresses)) == len(addresses),
            "unbounded/duplicate DCUtR candidates")
    require(bool(addresses) == (kind == 100), "CONNECT/SYNC candidate mismatch")
    direction = event.get("direction")
    require(isinstance(receipt, dict) and direction in {"read", "write"}, "requires captured DCUtR I/O")
    require(isinstance(receipt.get(direction), dict), "missing receipt for actual I/O direction")
    # path_wire's structural parser names its digest input `read`. This is an
    # in-memory view of the actual selected receipt, not a new logged read claim.
    validate_dcutr_frame({"framed_hex": receipt.get("framed_hex"), "read": receipt[direction]}, kind, addresses)


def _events(raw, implementation, token, *, final=True):
    require(isinstance(raw, dict) and raw.get("schema_version") == 1, "missing path result schema")
    require(raw.get("implementation") == implementation and raw.get("case_token") == token,
            "result implementation/token mismatch")
    require(raw.get("overflow") is False and raw.get("finalized") is final and raw.get("joined") is final,
            "unbounded/unfinalized/unjoined observer")
    require(raw.get("error") is None, "fixture returned an error")
    events = raw.get("events")
    require(isinstance(events, list) and 0 < len(events) <= EVENT_LIMIT, "missing/bounded path events")
    previous = 0
    for index, event in enumerate(events, 1):
        require(isinstance(event, dict) and event.get("kind") != "application_error",
                "native application handler failed")
        require(isinstance(event, dict) and type(event.get("sequence")) is int and event["sequence"] == index,
                "event sequence is not contiguous")
        require(_ns(event.get("mono_ns")) and event["mono_ns"] >= previous, "event clock/order invalid")
        require(isinstance(event.get("source"), str) and event["source"].startswith(implementation + "."),
                "event lacks native capture source")
        previous = event["mono_ns"]
    return events


def _one(events, kind, **fields):
    found = [e for e in events if e.get("kind") == kind and all(e.get(k) == v for k, v in fields.items())]
    require(len(found) == 1, f"missing/ambiguous {kind}: {fields}")
    return found[0]


def _session(events, peer, path, implementation=None):
    event = _one(events, "authenticated_connection", remote_peer_id=peer, path=path)
    forge = implementation == "forge" or str(event.get("source", "")).startswith("forge.")
    if forge:
        require(event.get("source") == "forge.authenticated_stream.diagnostics",
                "unknown Forge authenticated stream capture source")
    require(_id(event.get("connection_id")) and event.get("authenticated") is True,
            "path is not bound to an authenticated connection")
    require(isinstance(event.get("remote_address"), str), "missing actual remote endpoint")
    if path == "direct":
        require(event.get("security") == "/tls/1.0.0" and event.get("transport") == "quic"
                and event.get("authentication_basis") == "native_quic_authenticated_output",
                "direct DCUtR requires actual native QUIC/TLS provenance")
        require("/p2p-circuit" not in event["remote_address"], "direct fact is circuit")
        if event.get("local_address") is not None:
            address_bytes(event["local_address"])
        address_bytes(event["remote_address"])
        require("/udp/" in event["remote_address"] and "/quic-v1" in event["remote_address"], "direct socket is not QUIC/UDP")
    else:
        require(event.get("security") in {"/noise", "/tls/1.0.0"} and event.get("transport") == "circuit"
                and event.get("muxer") == "/yamux/1.0.0"
                and event.get("authentication_basis") == "native_relay_inner_upgrade",
                "relay requires actual inner authenticated security/Yamux, not outer QUIC")
        require("/p2p-circuit" in event["remote_address"] or isinstance(event.get("local_address"), str)
                and "/p2p-circuit" in event["local_address"], "relay fact lacks actual circuit endpoint")
        if forge:
            require(event.get("endpoint_basis") == "logical_authenticated_circuit_route"
                    and event.get("carrier_basis") == "forge.node.diagnostics.native_carrier"
                    and _id(event.get("carrier_connection_id"))
                    and event["carrier_connection_id"] != event["connection_id"]
                    and _id(event.get("relay_peer_id"))
                    and event.get("carrier_remote_peer_id") == event["relay_peer_id"],
                    "Forge circuit route lacks its distinct authenticated carrier owner")
            require(event["remote_address"].endswith(
                "/p2p/" + event["relay_peer_id"] + "/p2p-circuit/p2p/" + peer),
                "Forge logical circuit route is not bound to its relay and authenticated target")
            _udp_socket(event.get("carrier_local_address"))
            carrier_socket = _udp_socket(event.get("carrier_remote_address"))
            require(_udp_socket(event["remote_address"].split("/p2p/", 1)[0]) == carrier_socket,
                    "logical circuit route differs from its actual remote carrier endpoint")
    return event


def _echo(events, token, phase, session):
    event = _one(events, "echo", phase=phase)
    payload = challenge(token, phase)
    require(event.get("protocol") == ECHO_PROTOCOL and event.get("connection_id") == session["connection_id"]
            and event.get("remote_peer_id") == session["remote_peer_id"] and _id(event.get("stream_id")),
            "application receipt not bound to actual same-peer connection/stream")
    require(event.get("path") == session["path"] and event.get("fresh_dial") is False,
            "application exchange used a fresh dial or wrong path")
    require(event.get("io_basis") == "retained_native_stream" and type(event.get("server")) is bool,
            "echo has no actual retained-stream I/O basis")
    for side in ("read", "write"):
        require(event.get(side + "_bytes") == len(payload)
                and event.get(side + "_sha256") == hashlib.sha256(payload).hexdigest(),
                "application echo lacks actual challenge read/write receipt")
    require(event["sequence"] > session["sequence"], "echo precedes authentication")
    require(type(event.get("dial_attempts_before")) is int and event["dial_attempts_before"] >= 0
            and type(event.get("dial_attempts_after")) is int
            and event["dial_attempts_after"] >= event["dial_attempts_before"], "echo lacks actual dial diagnostic snapshots")
    # Native workers may retry during any read/write, especially relay_after's
    # retained server read. Only the application opening interval is tested.
    if phase in {"relay_before", "direct_after"} and event["server"] is False:
        opening = _one(events, "application_open", stream_id=event["stream_id"])
        require(opening.get("connection_id") == session["connection_id"]
                and opening.get("connected_before") is True
                and opening.get("opening_basis") == "native_existing_connection" and opening.get("phase") == phase
                and type(opening.get("dial_attempts_before")) is int
                and opening["dial_attempts_before"] >= 0 and type(opening.get("dial_attempts_after")) is int
                and opening["dial_attempts_after"] >= opening["dial_attempts_before"]
                and session["sequence"] < opening["sequence"] < event["sequence"],
                "opening application stream lacks actual existing-owner proof")
        basis = opening.get("dial_counter_basis")
        rust_source = ("rust.path_application.NotifyHandler.One.actual_owner" if phase == "relay_before"
                       else "rust.stream.Control.open_stream.actual_owner")
        require((opening.get("source"), basis) in {
            ("go.network.Conn.NewStream", "native_conn_new_stream_no_dial_api"),
            (rust_source, "native_stream_behaviour_dial_requests")},
            "application opening has no attributable dial/no-dial API provenance")
        if basis == "native_stream_behaviour_dial_requests":
            require(opening["dial_attempts_before"] == opening["dial_attempts_after"],
                    "opening application stream initiated an attributable dial")
            if phase == "relay_before":
                _rust_relay_application(events, token, session, opening, event)
    return event


def _udp_socket(address):
    address_bytes(address)
    parts = address.split("/")
    require(len(parts) in {6, 8} and parts[1] in {"ip4", "ip6"} and parts[3] == "udp" and parts[5] == "quic-v1",
            "actual socket/listener is not numeric QUIC")
    return str(ipaddress.ip_address(parts[2])), int(parts[4])


def _rust_relay_application(events, token, relay, opening, echo):
    """Bind the targeted public One request to the original live inner relay."""
    request = _one(events, "application_open_requested", phase="relay_before")
    require(_one(events, "application_open", phase="relay_before") is opening
            and request.get("source") == "rust.path_application.NotifyHandler.One.request"
            and echo.get("source") == "rust.path_echo.io"
            and relay.get("source") == "rust.native_transport.authenticated_output",
            "relay application is not the actual targeted One producer")
    peer, native_id = relay["remote_peer_id"], request.get("native_connection_id")
    endpoint = relay.get("endpoint")
    require(_id(native_id) and isinstance(endpoint, dict)
            and endpoint.get("direction") == relay.get("direction")
            and endpoint.get("upgrade_role") == relay.get("direction")
            and endpoint.get("remote_address") == relay.get("remote_address")
            and (endpoint["direction"] != "inbound" or endpoint.get("local_address") == relay.get("local_address")),
            "targeted One request lacks the actual authenticated relay endpoint")
    swarm = _one(events, "swarm_connection", native_connection_id=native_id)
    require(swarm.get("source") == "rust.swarm.ConnectionEstablished"
            and swarm.get("remote_peer_id") == peer and swarm.get("endpoint") == endpoint
            and _one(events, "swarm_connection", remote_peer_id=peer, endpoint=endpoint) is swarm
            and relay["sequence"] < swarm["sequence"] < request["sequence"],
            "targeted One native ConnectionId does not bind the original inner relay")
    require(request.get("remote_peer_id") == peer and request.get("connection_id") == relay["connection_id"]
            and request.get("path") == "relay" and request.get("endpoint") == endpoint
            and request.get("protocol") == ECHO_PROTOCOL
            and request.get("existing_connection_ids") == [relay["connection_id"]]
            and request.get("connected_before") is True
            and request.get("live_state_basis") == "native_authenticated_outputs_retained_until_ConnectionClosed"
            and request.get("target_binding_basis") == "exact_live_native_ConnectionId_authenticated_inner_relay"
            and request.get("open_api_basis") == opening.get("open_api_basis") == "native_notify_handler_one_no_dial_path"
            and request.get("dial_counter_basis") == "native_stream_behaviour_dial_requests"
            and type(request.get("dial_attempts_before")) is int and request["dial_attempts_before"] >= 0
            and request["dial_attempts_before"] == opening["dial_attempts_before"] == opening["dial_attempts_after"]
            and opening.get("native_connection_id") == native_id and opening.get("remote_peer_id") == peer,
            "targeted One request/open changed the existing owner or attributable dial counter")
    require(not any(key in request for key in ("stream_id", "selection_policy", "native_success_sequence",
                    "closed_sequence", "retained_terminal_sequence", "retired_relay_native_connection_id",
                    "retired_relay_connection_id", "retired_relay_stream_id")),
            "targeted One request invents an unopened stream or success retirement")
    _wave_stamp(opening, "requested", request)
    closed = [e for e in events if e.get("kind") == "native_connection_closed"]
    require(not any((e.get("native_connection_id") == native_id or e.get("connection_id") == relay["connection_id"])
                    and e["sequence"] < echo["sequence"] for e in closed),
            "targeted One original relay closed before completed challenge")
    direct_ids = request.get("existing_direct_connection_ids")
    actual_direct = [e["connection_id"] for e in events if e.get("kind") == "authenticated_connection"
                     and e.get("remote_peer_id") == peer and e.get("path") == "direct"
                     and e.get("source") == "rust.native_transport.authenticated_output"
                     and e["sequence"] < request["sequence"]
                     and not any(c.get("connection_id") == e["connection_id"] and c["sequence"] < request["sequence"]
                                 for c in closed)]
    require(isinstance(direct_ids, list) and all(_id(i) for i in direct_ids)
            and len(set(direct_ids)) == len(direct_ids) == len(actual_direct)
            and set(direct_ids) == set(actual_direct), "targeted One direct snapshot is not actual live transport owners")
    frames = [e for e in events if e.get("kind") == "application_frame" and e.get("phase") == "relay_before"]
    payload = challenge(token, "relay_before")
    require(len(frames) == 2 and {e.get("direction") for e in frames} == {"write", "read"},
            "targeted One lacks both actual native muxer I/O directions")
    require([e.get("direction") for e in frames] == ["write", "read"],
            "targeted One initiating native application I/O is out of order")
    for frame in frames:
        require(frame.get("source") == "rust.native_muxer.application.io"
                and frame.get("connection_id") == relay["connection_id"] and frame.get("remote_peer_id") == peer
                and frame.get("stream_id") == echo["stream_id"] and frame.get("path") == "relay"
                and type(frame.get("payload_bytes")) is int and frame["payload_bytes"] == len(payload)
                and frame.get("payload_sha256") == hashlib.sha256(payload).hexdigest()
                and request["sequence"] < frame["sequence"] < opening["sequence"] < echo["sequence"],
                "targeted One actual I/O owner/challenge/order differs from pre-call")
    require(not any(e.get("kind") == "capture_error" for e in events), "Rust native I/O capture failed")


def _rust_relay_retirement(events, token, peer, relay_peer, relay, direct, native):
    """Prove explicit inner retirement, not Rust Control's automatic preference."""
    before = _echo(events, token, "relay_before", relay)
    after = _echo(events, token, "direct_after", direct)
    opening = _one(events, "application_open", stream_id=after["stream_id"])
    request = _one(events, "relay_retirement_requested")
    closed = _one(events, "relay_retirement_closed")
    terminal = _one(events, "retained_stream_terminal")
    pre_call = _one(events, "application_open_requested", phase="direct_after")
    require(native.get("source") == "rust.dcutr.behaviour"
            and before.get("source") == after.get("source") == "rust.path_echo.io"
            and before.get("server") is False and after.get("server") is False,
            "retirement requires actual native success and completed initiating relay/direct I/O")
    require(request.get("source") == "rust.swarm.close_connection"
            and request.get("native_close_accepted") is True
            and closed.get("source") == "rust.swarm.ConnectionClosed",
            "retirement lacks accepted native inner close request/closure source")

    def binding(native_id, authenticated):
        require(_id(native_id) and _id(authenticated.get("connection_id"))
                and authenticated.get("source") == "rust.native_transport.authenticated_output",
                "retirement lacks actual native/authenticated transport owner")
        endpoint = authenticated.get("endpoint")
        require(isinstance(endpoint, dict) and endpoint.get("direction") == authenticated.get("direction")
                and endpoint.get("remote_address") == authenticated.get("remote_address")
                and endpoint.get("upgrade_role") in {"inbound", "outbound"},
                "retirement authenticated endpoint is unbound")
        if endpoint["direction"] == "inbound":
            require(endpoint.get("local_address") == authenticated.get("local_address")
                    and endpoint["upgrade_role"] == "inbound", "retirement inbound endpoint differs")
        swarm = _one(events, "swarm_connection", native_connection_id=native_id)
        require(swarm.get("source") == "rust.swarm.ConnectionEstablished"
                and swarm.get("remote_peer_id") == authenticated["remote_peer_id"]
                and swarm.get("endpoint") == endpoint
                and authenticated["sequence"] < swarm["sequence"] < request["sequence"],
                "retirement native ConnectionId does not bind original live transport endpoint")
        require(_one(events, "authenticated_connection", connection_id=authenticated["connection_id"]) is authenticated
                and _one(events, "swarm_connection", remote_peer_id=swarm["remote_peer_id"], endpoint=endpoint) is swarm,
                "retirement native/transport owner binding is ambiguous")
        return swarm

    inner_id = request.get("native_connection_id")
    direct_id = native.get("result", {}).get("connection_id")
    carrier_id = request.get("carrier_native_connection_id")
    carrier = _session(events, relay_peer, "direct", "rust")
    inner = binding(inner_id, relay)
    binding(direct_id, direct)
    binding(carrier_id, carrier)
    require(len({inner_id, direct_id, carrier_id}) == 3
            and len({relay["connection_id"], direct["connection_id"], carrier["connection_id"]}) == 3,
            "retirement aliases inner relay, successful direct or outer carrier")
    routes = set()
    for side in ("local_address", "remote_address"):
        address = relay.get("endpoint", {}).get(side)
        if isinstance(address, str) and "/p2p-circuit" in address:
            prefix = address.split("/p2p-circuit", 1)[0]
            require(prefix.endswith("/p2p/" + relay_peer), "retirement circuit route has wrong carrier peer")
            routes.add(_udp_socket(prefix.rsplit("/p2p/", 1)[0]))
    require(routes == {_udp_socket(carrier["remote_address"])},
            "retirement carrier socket does not match original native circuit route")
    actual_close = _one(events, "native_connection_closed", native_connection_id=inner_id)
    require(actual_close.get("source") == "rust.swarm.ConnectionClosed"
            and actual_close.get("connection_id") == relay["connection_id"]
            and actual_close.get("remote_peer_id") == peer
            and actual_close.get("endpoint") == inner["endpoint"]
            and "cause" in actual_close and actual_close["cause"] is None
            and type(actual_close.get("remaining_established")) is int and actual_close["remaining_established"] == 1,
            "retirement lacks actual clean inner ConnectionClosed with successful direct remaining")

    def stamp(receipt, prefix, event):
        for key in ("sequence", "mono_ns"):
            value = receipt.get(prefix + "_" + key)
            require(_ns(value) and value == event[key], "retirement stamp does not reference actual " + prefix)

    owners = {"native_connection_id": inner_id, "connection_id": relay["connection_id"],
              "stream_id": before["stream_id"], "remote_peer_id": peer,
              "direct_native_connection_id": direct_id, "direct_connection_id": direct["connection_id"],
              "carrier_native_connection_id": carrier_id, "carrier_connection_id": carrier["connection_id"],
              "carrier_remote_peer_id": relay_peer,
              "carrier_binding_basis": "unique_live_authenticated_carrier_for_native_circuit_route",
              "selection_policy": "success_only_explicit_inner_relay_retirement"}
    retired_keys = {"native_connection_id": "retired_relay_native_connection_id",
                    "connection_id": "retired_relay_connection_id", "stream_id": "retired_relay_stream_id"}
    require(not any(key in pre_call for key in retired_keys),
            "pre-Control request aliases a retired relay as an unopened stream owner")
    for event in (request, closed, terminal, pre_call):
        require(all(event.get(retired_keys.get(key, key) if event is pre_call else key) == value
                    for key, value in owners.items())
                and event.get("automatic_direct_preference_proven") is False,
                "retirement receipt changed original owners or claims automatic direct preference")
        stamp(event, "native_success", native)
        stamp(event, "relay_before", before)
        for prefix, bound in (("requested", request), ("closed", actual_close)):
            if event is request and prefix == "closed":
                require(all(prefix + "_" + key in event and event[prefix + "_" + key] is None
                            for key in ("sequence", "mono_ns")), "request claims a precompleted retirement")
            else:
                stamp(event, prefix, bound)
    stamp(closed, "native_closed", actual_close)
    require(closed.get("cause") is None and "cause" in closed
            and closed.get("direct_still_live") is True and closed.get("carrier_still_live") is True
            and closed.get("inner_relay_still_live") is False,
            "retirement did not preserve direct and outer carrier")
    require(terminal.get("source") == "rust.path_echo.retained_stream.native_io"
            and terminal.get("phase") == "relay_before"
            and terminal.get("terminal_basis") == "retained_initiator_actual_read_after_native_close"
            and terminal.get("direction") == "read" and terminal.get("expected_retirement") is True
            and terminal.get("error_kind") in {"eof", "reset"}
            and isinstance(terminal.get("error"), str) and 0 < len(terminal["error"]) <= 256
            and (terminal.get("io_bytes") is None or type(terminal["io_bytes"]) is int
                 and terminal["io_bytes"] == 0 and terminal["error_kind"] == "eof" and terminal["error"] == "EOF")
            and "io_bytes" in terminal,
            "retirement lacks actual retained-stream EOF/reset read after native closure")
    stamp(pre_call, "retained_terminal", terminal)
    require(pre_call.get("source") == "rust.libp2p_stream.Control.open_stream.request"
            and pre_call.get("protocol") == ECHO_PROTOCOL
            and pre_call.get("existing_connection_ids") == [direct["connection_id"]]
            and pre_call.get("existing_direct_connection_ids") == [direct["connection_id"]]
            and pre_call.get("connected_before") is True
            and pre_call.get("live_state_basis") == "native_authenticated_outputs_retained_until_ConnectionClosed"
            and pre_call.get("direct_still_live") is True and pre_call.get("carrier_still_live") is True
            and pre_call.get("inner_relay_still_live") is False
            and pre_call.get("dial_counter_basis") == "native_stream_behaviour_dial_requests"
            and type(pre_call.get("dial_attempts_before")) is int and pre_call["dial_attempts_before"] >= 0
            and pre_call["dial_attempts_before"] == opening["dial_attempts_before"] == opening["dial_attempts_after"],
            "retirement lacks actual pre-Control.open_stream live-owner/dial snapshot")
    ordered = [request, actual_close, closed, terminal, pre_call, opening, after]
    require(max(native["sequence"], before["sequence"]) < request["sequence"]
            and all(a["sequence"] < b["sequence"] and a["mono_ns"] <= b["mono_ns"]
                    for a, b in zip(ordered, ordered[1:]))
            and after["stream_id"] != before["stream_id"],
            "retirement/direct application order is invalid or original stream was relabeled")

    # Survivors are checked against native lifetime events, not only summary flags.
    for owner in (direct_id, carrier_id):
        require(not any(e.get("kind") == "native_connection_closed" and e.get("native_connection_id") == owner
                        and e["sequence"] < after["sequence"] for e in events),
                "retirement successful direct or outer carrier closed before direct echo")
    for checkpoint, expected in ((request, {inner_id, direct_id}), (pre_call, {direct_id}), (after, {direct_id})):
        for remote, ids in ((peer, expected), (relay_peer, {carrier_id})):
            live = {e.get("native_connection_id") for e in events if e.get("kind") == "swarm_connection"
                    and e.get("remote_peer_id") == remote and e["sequence"] < checkpoint["sequence"]
                    and not any(c.get("kind") == "native_connection_closed"
                                and c.get("native_connection_id") == e.get("native_connection_id")
                                and c["sequence"] < checkpoint["sequence"] for c in events)}
            require(live == ids, "retirement has missing/replaced/ambiguous live native owners")
    require(not any(e.get("kind") == "capture_error" for e in events), "Rust native I/O capture failed")
    for phase, echo, authenticated in (("relay_before", before, relay), ("direct_after", after, direct)):
        # application_open is post-transfer. Direct muxer I/O must follow the
        # independent pre-Control request, not just that later owner receipt.
        phase_open = _one(events, "application_open", stream_id=echo["stream_id"])
        frames = [e for e in events if e.get("kind") == "application_frame" and e.get("phase") == phase]
        require(len(frames) == 2 and {e.get("direction") for e in frames} == {"read", "write"},
                "retirement application lacks both actual native muxer I/O directions")
        require([e["direction"] for e in frames] == ["write", "read"],
                "retirement initiating native application I/O is out of order")
        payload = challenge(token, phase)
        for event in frames:
            require(event.get("source") == "rust.native_muxer.application.io"
                    and event.get("connection_id") == authenticated["connection_id"]
                    and event.get("remote_peer_id") == peer and event.get("stream_id") == echo["stream_id"]
                    and event.get("path") == authenticated["path"]
                    and type(event.get("payload_bytes")) is int and event["payload_bytes"] == len(payload)
                    and event.get("payload_sha256") == hashlib.sha256(payload).hexdigest()
                    and authenticated["sequence"] < event["sequence"] < phase_open["sequence"] < echo["sequence"]
                    and (phase != "direct_after" or pre_call["sequence"] < event["sequence"]),
                    "retirement application frame owner/order/actual challenge I/O differs")


def _quic_listener_owner(network, role, ready, direct):
    """Compare actual bound listener with native local socket or router packets.

    Rust's public QUIC muxer hides outbound local_addr. A conntrack original
    tuple is actual LAN UDP socket traffic, not PortUse::Reuse/config intent.
    The snapshot/journal ownership is independently checked by _nat.
    """
    listen = ready.get("listen_addrs")
    require(isinstance(listen, list) and len(listen) == 1, "missing exact native bound QUIC listener")
    listener = _udp_socket(listen[0])
    lan = "10.1.0.2" if role == "source" else "10.2.0.2"
    wan = "11.0.0.2" if role == "source" else "11.0.0.3"
    require(listener[0] == lan, "listener is not bound to owned LAN socket")
    if direct.get("local_address") is not None:
        require(_udp_socket(direct["local_address"]) == listener,
                "actual QUIC local socket does not reuse bound listener IP/port")
    remote = _udp_socket(direct["remote_address"])
    snapshot = next(s for s in network["snapshots"] if s["phase"] == "after_upgrade")
    tuples = set()
    for line in snapshot["routers"][role + "_router"]["conntrack"].splitlines():
        fields = re.findall(r"src=(\S+) dst=(\S+) sport=(\d+) dport=(\d+)", line)
        if len(fields) != 2 or not re.search(r"\budp\s+17\s+", line) or "[UNREPLIED]" in line:
            continue
        original, reply = fields
        if original[0] == lan and (original[1], int(original[3])) == remote and reply[1] == wan:
            tuples.add((original[0], int(original[2])))
    require(tuples == {listener}, "actual conntrack UDP local IP/port differs from bound listener; reuse intent is insufficient")


def _nat(network, outcome):
    require(isinstance(network, dict) and network.get("kind") == "linux_dcutr_conntrack_nat"
            and network.get("state") == "closed" and not network.get("cleanup_failures")
            and not network.get("cleanup_uncertainty"), "missing clean Linux NAT ownership")
    require(network.get("outer_network", {}).get("external_links") == "absent", "nonisolated network")
    outer = network.get("outer_namespace")
    require(isinstance(outer, str) and re.fullmatch(r"path-o-[a-z0-9]{1,20}", outer), "missing owned outer namespace")
    participants = network.get("participants")
    require(isinstance(participants, list) and len(participants) == 5, "missing three-process/two-router topology")
    namespaces = {}
    for role, address in (("relay", "11.0.0.1"), ("source_router", "11.0.0.2"), ("destination_router", "11.0.0.3"),
                          ("source", "10.1.0.2"), ("destination", "10.2.0.2")):
        matches = [p for p in participants if p.get("role") == role]
        require(len(matches) == 1 and matches[0].get("addresses") == [address]
                and matches[0].get("namespace") == f"path-{role}-{outer.removeprefix('path-o-')}",
                "NAT participant is not scope-owned")
        namespaces[role] = matches[0]["namespace"]
    commands = network.get("commands")
    require(isinstance(commands, list) and commands, "missing executed packet-rule provenance")
    require(all(c.get("returncode") == 0 and not c.get("exception") for c in commands), "NAT command failed")
    snapshots = network.get("snapshots", [])
    if outcome != "success":
        require(network.get("faults") == [{"kind": "peer_udp_drop", "before_circuit_connect": True}],
                "negative path case lacks pre-circuit peer UDP fault")
        fault = [s for s in snapshots if s.get("phase") == "after_fault"]
        require(len(fault) == 1 and [s.get("phase") for s in snapshots].index("after_fault")
                < [s.get("phase") for s in snapshots].index("barrier"), "fault was not captured before circuit barrier")
        for role, other in (("source_router", "11.0.0.3"), ("destination_router", "11.0.0.2")):
            require(any(len(c.get("command", [])) >= 5 and c["command"][1:4] == ["netns", "exec", namespaces[role]]
                        and Path(c["command"][4]).name == "iptables" and c["command"][5:] ==
                        ["-w", "2", "-I", "FORWARD", "1", "-i", "eth0", "-o", "lan0", "-p", "udp", "-s", other, "-j", "DROP"]
                        for c in commands), "peer UDP fault lacks exact successful packet-rule command")
    for phase in ("before_connect", "barrier", "after_upgrade"):
        values = [s for s in snapshots if s.get("phase") == phase]
        require(len(values) == 1, f"missing/duplicate actual NAT snapshot {phase}")
        for role, wan in (("source_router", "11.0.0.2"), ("destination_router", "11.0.0.3")):
            router = values[0].get("routers", {}).get(role, {})
            rules = router.get("rules")
            require(isinstance(rules, str) and ":FORWARD DROP" in rules and "-j SNAT --to-source " + wan in rules
                    and any(state in rules for state in ("--ctstate RELATED,ESTABLISHED", "--ctstate ESTABLISHED,RELATED"))
                    and "-A INPUT -i eth0 -p udp -j DROP" in rules, "NAT is not observed stateful SNAT/DROP")
            inbound = [line for line in rules.splitlines() if "-A FORWARD" in line and "-i eth0" in line and "-j ACCEPT" in line]
            require(len(inbound) == 1 and "-m conntrack" in inbound[0], "NAT permits unsolicited inbound")
            require(isinstance(router.get("conntrack"), str), "missing Linux conntrack listing")
            # Every snapshot must be backed by a successful tool invocation and
            # exact raw output, not a hand-authored topology label.
            for tool, key in (("iptables-save", "rules"), ("conntrack", "conntrack")):
                require(any(len(c.get("command", [])) >= 5 and c["command"][1:4] == ["netns", "exec", namespaces[role]]
                            and Path(c["command"][4]).name == tool and c.get("stdout") == router[key] for c in commands),
                        "packet snapshot lacks matching command output")
            if phase == "before_connect":
                require(not router["conntrack"].strip(), "preexisting UDP/NAT state")
            if phase == "after_upgrade" and outcome == "success":
                other = "11.0.0.3" if role == "source_router" else "11.0.0.2"
                # UDP [ASSURED] is time-dependent; bidirectional reply tuples
                # without UNREPLIED are sufficient and actually observable.
                lan = "10.1.0.2" if role == "source_router" else "10.2.0.2"
                require(any("[UNREPLIED]" not in line and re.search(r"\budp\s+17\s+", line)
                            and f"src={lan} dst={other}" in line and f"src={other} dst={wan}" in line
                            for line in router["conntrack"].splitlines()), "missing bilateral native UDP reply flow")
    return namespaces


def native_terminal(result, implementation, peer, outcome):
    """Accessible native outcome only; never invent operation/attempt identifiers."""
    events = result.get("events", [])
    if implementation == "forge":
        samples = [e for e in events if e.get("kind") == "native_snapshot" and e.get("remote_peer_id") == peer]
        if len(samples) < 2:
            return False
        key = "hole_punch_successes" if outcome == "success" else "hole_punch_failures"
        return samples[-1].get(key, 0) > samples[0].get(key, 0)
    if implementation == "go":
        return any(e.get("kind") == "holepunch_trace" and e.get("native_type") == "EndHolePunch"
                   and e.get("remote_peer_id") == peer and e.get("success") is (outcome == "success") for e in events)
    return any(e.get("kind") == "native_dcutr_event" and e.get("remote_peer_id") == peer
               and e.get("result", {}).get("native_success") is (outcome == "success") for e in events)


def _go_cancel_claims(events, local_peer, peer):
    claims = [e for e in events if e.get("kind") == "holepunch_trace" and e.get("remote_peer_id") == peer
              and e.get("native_type") in {"StartHolePunch", "EndHolePunch", "ProtocolError"}]
    for event in claims:
        require(event.get("source") == "go.holepunch.tracer" and event.get("local_peer_id") == local_peer
                and _ns(event.get("native_unix_ns")), "Go cancellation lacks actual peer-bound native tracer claim")
        if event["native_type"] == "EndHolePunch":
            require(type(event.get("success")) is bool, "Go native terminal lacks its actual result")
        elif event["native_type"] == "ProtocolError":
            require(isinstance(event.get("error"), str) and bool(event["error"]), "Go native protocol error lacks its actual cause")
    return claims


def go_cancel_state(result, token, peer, role):
    """Observed native method state, not a worker count or aggregate outcome."""
    require(role in {"source", "destination"}, "invalid Go cancellation role")
    events = _events(result, "go", token, final=False)
    local_peer = result.get("local_peer_id")
    require(_id(local_peer), "Go cancellation lacks local identity")
    claims = _go_cancel_claims(events, local_peer, peer)
    start, previous_terminal, terminal, active, succeeded = None, None, None, False, False
    for event in claims:
        if event["native_type"] == "StartHolePunch":
            require(not active and not succeeded, "overlapping/completed Go native cancellation claims")
            start, previous_terminal, active = event, terminal, True
        else:
            require(event["native_type"] == "ProtocolError" or active, "Go native EndHolePunch lacks its StartHolePunch")
            terminal, active = event, False
            succeeded |= event.get("success") is True
    state = {"state": "active" if active else "terminal" if terminal else "not_started",
             "start_sequence": start["sequence"] if start else None,
             "terminal_sequence": terminal["sequence"] if terminal else None,
             "terminal_type": terminal["native_type"] if terminal else None}
    if not active:
        return state
    require(not any(e.get("kind") == "authenticated_connection" and e.get("path") == "direct"
                    and e.get("remote_peer_id") == peer for e in events), "Go cancellation already became direct")
    require(_ns(start.get("rtt_ns")), "Go active native claim lacks measured RTT")
    relay = _session(events, peer, "relay", "go")
    require(relay.get("direction") == ("outbound" if role == "source" else "inbound"),
            "Go cancellation claim has the wrong original relay owner")
    exchanges = _handshakes(events[:start["sequence"] - 1], peer, relay, role, "cancelled")
    boundary = previous_terminal["sequence"] if previous_terminal else relay["sequence"]
    matching = [frames for frames in exchanges if frames[0]["sequence"] > boundary
                and frames[-1]["sequence"] < start["sequence"]]
    require(len(matching) == 1, "Go active native claim lacks a unique current complete CONNECT/SYNC stream")
    frames = matching[0]
    require(all(e.get("source") == "go.native_dcutr.io" for e in frames), "Go active claim lacks actual native wire capture")
    remote_connect = next(e for e in frames if e["direction"] == "read" and e["message_type"] == 100)
    require(_wave_sockets(start.get("addresses"), peer) == _wave_sockets(remote_connect["addresses"], peer),
            "Go active native candidates differ from its current remote CONNECT")
    return {**state, "connection_id": relay["connection_id"], "stream_id": frames[0]["stream_id"],
            "start_mono_ns": start["mono_ns"]}


def _handshakes(events, peer, relay, role, outcome):
    frames = [e for e in events if e.get("kind") == "dcutr_frame"]
    require(frames, "missing donor read/write DCUtR capture")
    streams = {}
    for frame in frames:
        require(frame.get("remote_peer_id") == peer and frame.get("connection_id") == relay["connection_id"]
                and frame.get("protocol") == "/libp2p/dcutr" and _id(frame.get("stream_id")),
                "DCUtR I/O is not bound to authenticated expected circuit peer/stream")
        validate_frame(frame)
        streams.setdefault(frame["stream_id"], []).append(frame)
    require(1 <= len(streams) <= 3, "unbounded DCUtR retries")
    expected = [("read", 100), ("write", 100), ("read", 300)] if role == "source" else [
        ("write", 100), ("read", 100), ("write", 300)]
    completed = []
    for stream in streams.values():
        observed = [(e["direction"], e["message_type"]) for e in stream]
        prefix = len(observed) < len(expected) and observed == expected[:len(observed)]
        require(observed == expected or outcome in {"failed", "cancelled"} and prefix,
                "actual CONNECT/CONNECT/SYNC roles/order mismatch")
        if observed == expected:
            completed.append(stream)
            require(stream[1]["mono_ns"] > stream[0]["mono_ns"], "missing actual CONNECT turnaround")
        elif outcome == "failed":
            direction = expected[len(observed)][0]
            terminal = _one(events, "dcutr_stream_terminal", connection_id=relay["connection_id"],
                            stream_id=stream[0]["stream_id"], remote_peer_id=peer, direction=direction)
            require(terminal.get("source") == ("go.native_dcutr.io" if stream[0].get("source", "").startswith("go.")
                                              else "rust.native_muxer.dcutr.io")
                    and terminal.get("protocol") == "/libp2p/dcutr"
                    and terminal.get("error_kind") in {"reset", "eof", "canceled", "deadline", "io_error"}
                    and isinstance(terminal.get("error"), str) and bool(terminal["error"])
                    and terminal["sequence"] > stream[-1]["sequence"]
                    and terminal["mono_ns"] >= stream[-1]["mono_ns"],
                    "incomplete retry lacks subsequent bound native terminal I/O")
            require(type(terminal.get("completed_frame_count")) is int
                    and terminal["completed_frame_count"] == sum(e["direction"] == direction for e in stream)
                    and type(terminal.get("io_bytes")) is int and 0 <= terminal["io_bytes"] <= 4096
                    and type(terminal.get("pending_frame_bytes")) is int and 0 <= terminal["pending_frame_bytes"] <= 4096
                    and terminal.get("invalid_or_over_limit") is False,
                    "incomplete retry has malformed or unbounded native terminal I/O")
    require(completed or outcome == "cancelled", "missing complete native DCUtR handshake")
    return completed


def _wave_sockets(addresses, peer):
    require(isinstance(addresses, list) and 0 < len(addresses) <= 32, "missing bounded native wave candidates")
    sockets = []
    for address in addresses:
        require(isinstance(address, str), "non-native wave address")
        parts = address.rsplit("/p2p/", 1)
        require(len(parts) == 1 or parts[1] == peer, "native wave candidate has a foreign peer suffix")
        sockets.append(_udp_socket(parts[0]))
    require(len(set(sockets)) == len(sockets), "duplicate native wave candidate sockets")
    return set(sockets)


def _wave_stamp(event, prefix, captured):
    require(all(_ns(event.get(prefix + "_" + key)) and event[prefix + "_" + key] == captured[key]
                for key in ("sequence", "mono_ns")), "native wave stamp differs from actual " + prefix)


def _rust_source_waves(result, token, peer):
    """Separate native SOURCE wave observation, never aggregate completion/join."""
    events = _events(result, "rust", token, final=False)
    require(_id(result.get("local_peer_id")), "native wave lacks actual local identity")
    pending = [e for e in events if e.get("kind") == "native_dcutr_dial_options"]
    if not pending:
        return events, []
    require(len(pending) <= 3, "native source wave retry bound exceeded")
    relay = _session(events, peer, "relay", "rust")
    require(relay.get("source") == "rust.native_transport.authenticated_output"
            and relay.get("direction") == "outbound" and isinstance(relay.get("endpoint"), dict)
            and relay["endpoint"].get("direction") == "outbound"
            and relay["endpoint"].get("remote_address") == relay["remote_address"],
            "native wave requires actual authenticated SOURCE relay endpoint")
    exchanges = _handshakes(events, peer, relay, "source", "cancelled")
    require(exchanges, "native source wave lacks full original relay CONNECT/SYNC")
    require(not any(e.get("kind") == "authenticated_connection" and e.get("path") == "direct"
                    and e.get("remote_peer_id") == peer for e in events), "failed native wave already became direct")
    waves, ids, streams = [], set(), set()
    for options in pending:
        native_id = options.get("native_connection_id")
        require(_id(native_id) and native_id not in ids and options.get("remote_peer_id") == peer,
                "native wave pending ID/peer is missing or duplicate")
        ids.add(native_id)
        requested = _one(events, "native_dcutr_dial_requested", native_connection_id=native_id)
        require(not any(e.get("kind") == "native_dcutr_wave_binding_rejected" and e.get("native_connection_id") == native_id
                        for e in events), "native source wave binding was actually rejected")
        require(requested.get("source") == "rust.dcutr.Behaviour.poll.ToSwarm.Dial"
                and requested.get("native_origin") == "dcutr::Behaviour::poll::ToSwarm::Dial"
                and requested.get("source_wave_bound") is True
                and options.get("source") == "rust.dcutr.Behaviour.handle_pending_outbound_connection"
                and options.get("requested_role") == "dialer" and options.get("native_callback_accepted") is True
                and options.get("native_returned_addresses") == []
                and options.get("candidate_match_basis") == "exact_numeric_socket_optional_same_peer_suffix",
                "native wave lacks actual DCUtR poll/pending origin, role or native return")
        stream = options.get("stream_id")
        matching = [frames for frames in exchanges if frames[0]["stream_id"] == stream]
        require(len(matching) == 1 and stream not in streams, "native wave reused or lost its original complete wire stream")
        streams.add(stream)
        frames = matching[0]
        relay_id = options.get("relay_native_connection_id")
        require(_id(relay_id) and native_id != relay_id, "native wave dial aliases its inner relay ID")
        established = _one(events, "swarm_connection", native_connection_id=relay_id)
        require(established.get("source") == "rust.swarm.ConnectionEstablished"
                and established.get("remote_peer_id") == peer and established.get("endpoint") == relay["endpoint"]
                and relay["sequence"] < established["sequence"] < frames[0]["sequence"],
                "native wave relay native ID differs from original authenticated endpoint")
        advertised = frames[0]["addresses"]
        _wave_sockets(frames[1]["addresses"], result["local_peer_id"])
        require(_wave_sockets(options.get("candidate_addresses"), peer) == _wave_sockets(advertised, peer),
                "native wave pending addresses differ from actual CONNECT candidates")
        for event in (requested, options):
            require(event.get("remote_peer_id") == peer and event.get("relay_native_connection_id") == relay_id
                    and event.get("relay_connection_id") == relay["connection_id"] and event.get("stream_id") == stream
                    and event.get("advertised_addresses") == advertised
                    and event.get("binding_basis") == "native_dcutr_poll_id_pending_callback_and_original_authenticated_relay_wire"
                    and event.get("proof_scope") == "native_wave_only"
                    and event.get("aggregate_dcutr_completed") is False and event.get("rust_behaviour_joined") is False,
                    "native wave changed original owners or claims aggregate completion/join")
            for prefix, frame in zip(("connect_read", "connect_write", "sync_read"), frames):
                require(frame.get("source") == "rust.native_muxer.dcutr.io", "native wave wire source is not actual muxer I/O")
                _wave_stamp(event, prefix, frame)
            _wave_stamp(event, "requested", requested)
        require(requested.get("candidate_addresses") == []
                and all(key in requested and requested[key] is None for key in ("pending_sequence", "pending_mono_ns")),
                "native wave request invents future pending options")
        _wave_stamp(options, "pending", options)
        require(frames[-1]["sequence"] < requested["sequence"] < options["sequence"]
                and not any(e.get("kind") == "native_connection_closed" and e.get("native_connection_id") == relay_id
                            for e in events), "native wave request/options order or original relay lifetime differs")
        waves.append((requested, options))
    return events, waves


def rust_source_wave_pending(result, token, peer):
    events, waves = _rust_source_waves(result, token, peer)
    require(not any(e.get("kind") in {"native_dcutr_event", "native_dcutr_wave_failed"}
                    and e.get("remote_peer_id") == peer for e in events), "Rust source wave terminated before cancellation observation")
    require(not any(e.get("kind") == "native_dial_error" and e.get("native_connection_id") == options["native_connection_id"]
                    for _, options in waves for e in events), "Rust source dial error preceded cancellation observation")
    return bool(waves)


def rust_source_wave_failure(result, token, peer):
    events, waves = _rust_source_waves(result, token, peer)
    failed = [e for e in events if e.get("kind") == "native_dcutr_wave_failed"]
    if not failed:
        return False
    require(not any(e.get("kind") == "native_dcutr_event" and e.get("remote_peer_id") == peer for e in events),
            "native SOURCE wave failure is not an aggregate DCUtR completion")
    require(0 < len(failed) <= 3 and len({e.get("native_connection_id") for e in failed}) == len(failed),
            "native source wave failure ID/count bound differs")
    for failure in failed:
        matching = [(request, options) for request, options in waves
                    if options["native_connection_id"] == failure.get("native_connection_id")]
        require(len(matching) == 1, "native wave failure lacks actual poll/pending dial ID")
        request, options = matching[0]
        require(failure.get("source") == "rust.swarm.OutgoingConnectionError.native_dcutr_wave"
                and failure.get("native_origin") == request["native_origin"]
                and failure.get("dial_error_variant") == "Transport" and failure.get("original_relay_live") is True,
                "native wave failure has no typed native dial-error origin/live relay")
        for key in ("remote_peer_id", "relay_native_connection_id", "relay_connection_id", "stream_id",
                    "advertised_addresses", "candidate_addresses", "binding_basis", "proof_scope",
                    "aggregate_dcutr_completed", "rust_behaviour_joined", "requested_sequence", "requested_mono_ns",
                    "pending_sequence", "pending_mono_ns", "connect_read_sequence", "connect_read_mono_ns",
                    "connect_write_sequence", "connect_write_mono_ns", "sync_read_sequence", "sync_read_mono_ns"):
            require(type(failure.get(key)) is type(options.get(key)) and failure.get(key) == options.get(key),
                    "native wave failure changed bound " + key)
        raw_error = _one(events, "native_dial_error", native_connection_id=failure["native_connection_id"])
        require(raw_error.get("source") == "rust.swarm.OutgoingConnectionError" and raw_error.get("remote_peer_id") == peer
                and isinstance(raw_error.get("error"), str) and bool(raw_error["error"])
                and options["sequence"] < raw_error["sequence"] < failure["sequence"]
                and not any(e.get("kind") == "swarm_connection" and e.get("native_connection_id") == failure["native_connection_id"]
                            for e in events), "native wave failure lacks same actual failed OutgoingConnectionError ID/peer/order")
        _wave_stamp(failure, "raw_error", raw_error)
        errors = failure.get("transport_errors")
        require(isinstance(errors, list) and 0 < len(errors) <= 32, "native wave lacks bounded typed QUIC transport errors")
        for error in errors:
            require(isinstance(error, dict) and error.get("transport_error_variant") == "Other"
                    and error.get("quic_error_type") == "libp2p_quic::Error"
                    and error.get("quic_error_variant") in {"Reach", "Connection", "Io", "HandshakeTimedOut",
                        "NoActiveListenerForDialAsListener", "HolePunchInProgress"}
                    and error.get("classification_basis") == "typed_downcast_and_public_native_enum"
                    and isinstance(error.get("error"), str) and 0 < len(error["error"]) <= 256
                    and isinstance(error.get("wrapper_types"), list) and 0 < len(error["wrapper_types"]) <= 12
                    and error["wrapper_types"][0] == "std::io::Error"
                    and all(t in {"std::io::Error", "native_path_OrTransport::Error"} for t in error["wrapper_types"]),
                    "native wave error is not a typed captured public QUIC error")
        require(_wave_sockets([e["address"] for e in errors], peer) == _wave_sockets(options["candidate_addresses"], peer),
                "native wave typed errors do not cover exact actual pending candidates")
    return True


def _validate(record):
    spec = record.get("case", {})
    require(spec.get("transport") == "quic" and spec.get("profile") == "native" and spec.get("outcome") in OUTCOMES,
            "DCUtR is native QUIC only; private PSK relay/DCUtR is prohibited")
    require((spec.get("source"), spec.get("destination")) in
            {("forge", "go"), ("go", "forge"), ("forge", "rust"), ("rust", "forge")}, "unsupported direction")
    require(spec.get("relay") in {"go", "rust"} and not record.get("errors") and not record.get("cleanup_errors"),
            "invalid relay/execution/cleanup")
    token = record.get("case_token")
    challenge(token, "relay_before")
    wave_scope = rust_source_wave_case(spec)
    require(record.get("terminal_scope") == RUST_SOURCE_WAVE_SCOPE and "native_wave_failure" in record
            if wave_scope else "terminal_scope" not in record and "native_wave_failure" not in record,
            "native wave terminal scope is exclusive to Rust SOURCE negative cases")
    raw = record.get("raw", {})
    require(set(raw) == {"source", "destination", "relay"}, "requires exactly three live processes")
    namespaces = _nat(record.get("network"), spec["outcome"])
    peers, events, pids = {}, {}, []
    for role in ("source", "destination", "relay"):
        actor = raw[role]
        ready, result, process = actor.get("ready", {}), actor.get("result", {}), actor.get("process", {})
        peers[role] = ready.get("peer_id")
        require(_id(peers[role]) and result.get("local_peer_id") == peers[role], "ready/authenticated local peer mismatch")
        require(ready.get("implementation") == spec[role] and ready.get("case_token") == token, "ready role/token mismatch")
        require(ready.get("path_bindings") == ("native_public_diagnostics_v1" if spec[role] == "forge"
                else "actual_io_and_native_attempt_v1"), "fixture does not expose the concrete native proof contract")
        require(type(process.get("pid")) is int and process["pid"] > 0 and process.get("terminal_status") ==
                {"exit_code": 0, "termination": "graceful"}, "process did not exit gracefully and join")
        require(process.get("ready") == ready and isinstance(process.get("command"), list)
                and process.get("outputs"), "process ownership/raw capture missing")
        command = process["command"]
        require(len(command) >= 6 and Path(command[0]).name == "ip" and command[1:4] == ["netns", "exec", namespaces[role]]
                and command[5] == "path-live", "fixture process is not inside its owned NAT namespace")
        pids.append(process["pid"])
        events[role] = _events(result, spec[role], token)
    require(len(set(peers.values())) == 3 and len(set(pids)) == 3, "reused identity/process")
    relays, directs = {}, {}
    donor_role = "destination" if spec["source"] == "forge" else "source"
    require(record.get("application_actor") == donor_role, "application must open on donor's actual existing connection")
    for role, es in events.items():
        require(not any(e.get("kind") in {"relay_retirement_requested", "relay_retirement_closed", "retained_stream_terminal"}
                        or e.get("kind") == "application_open_requested"
                        and (e.get("phase") == "direct_after" or any(key in e for key in
                             ("selection_policy", "native_success_sequence", "requested_sequence", "closed_sequence",
                              "retained_terminal_sequence")))
                        for e in es) or spec["outcome"] == "success" and role == donor_role and spec[role] == "rust",
                "relay retirement is permitted only on the successful Rust application actor")
        if role == donor_role and spec[role] == "rust":
            phases = {"relay_before", "direct_after"} if spec["outcome"] == "success" else {"relay_before"}
            require(all(e.get("phase") in phases for e in es
                        if e.get("kind") in {"application_open_requested", "application_open"}),
                    "Rust application open is outside its exact native phase contract")
    for role, other in (("source", "destination"), ("destination", "source")):
        es = events[role]
        baseline = _one(es, "baseline", remote_peer_id=peers[other])
        require(baseline.get("direct_connection_ids") == [], "preexisting direct connection is not DCUtR proof")
        require(not any(e.get("kind") in {"manual_upgrade", "ordinary_direct_success", "operation_finished", "attempt_started", "application_dial_requested"}
                        for e in es), "manual/unilateral or fabricated operation evidence is not accepted")
        relay = _session(es, peers[other], "relay", spec[role])
        relays[role] = relay
        require(relay.get("relay_peer_id") == peers["relay"] and relay.get("direction") ==
                ("outbound" if role == "source" else "inbound"), "circuit identity/direction mismatch")
        barrier = _echo(es, token, "relay_before", relay)
        require(barrier["server"] is (role != donor_role), "application initiator/server provenance differs from native composition")
        require(baseline["sequence"] < relay["sequence"] < barrier["sequence"], "circuit challenge precedes bound baseline")
        if spec["outcome"] == "success":
            direct = _session(es, peers[other], "direct", spec[role])
            directs[role] = direct
            _quic_listener_owner(record["network"], role, raw[role]["ready"], direct)
            require(direct["connection_id"] != relay["connection_id"] and direct["sequence"] > baseline["sequence"],
                    "direct connection is not new after baseline")
            echo = _echo(es, token, "direct_after", direct)
            require(echo["server"] is (role != donor_role), "new direct stream has wrong application initiator")
            require(native_terminal(raw[role]["result"], spec[role], peers[other], "success"),
                    "authenticated direct receipt lacks independent native DCUtR outcome")
        else:
            after = _echo(es, token, "relay_after", relay)
            require(after["stream_id"] == barrier["stream_id"] and after["sequence"] > barrier["sequence"],
                    "failure/cancellation did not retain original authenticated relay stream")
            require(not any(e.get("kind") == "authenticated_connection" and e.get("path") == "direct"
                            and e.get("remote_peer_id") == peers[other] for e in es), "faulted upgrade became direct")
            if spec["outcome"] == "failed" and not (wave_scope and role == "source"):
                require(native_terminal(raw[role]["result"], spec[role], peers[other], "failed"),
                        "failed upgrade lacks genuine native completion")
    other_role = "source" if donor_role == "destination" else "destination"
    donor_events = events[donor_role]
    exchanges = _handshakes(donor_events, peers[other_role], relays[donor_role], donor_role, spec["outcome"])
    if wave_scope:
        wave = record["native_wave_failure"]
        require(isinstance(wave, dict) and wave.get("local_peer_id") == peers["source"]
                and rust_source_wave_failure(wave, token, peers["destination"]), "missing actual Rust SOURCE native wave failure")
        wave_events = wave["events"]
        require(wave_events == donor_events[:len(wave_events)]
                and not any(e.get("kind") == "native_dcutr_event" and e.get("remote_peer_id") == peers["destination"]
                            for e in donor_events), "Rust SOURCE wave prefix changed or claims aggregate completion")
        after = _one(donor_events, "echo", phase="relay_after")
        require(after["sequence"] > wave_events[-1]["sequence"], "retained relay echo precedes native wave observation")
        if spec["outcome"] == "failed":
            forge_after = _one(events["destination"], "echo", phase="relay_after")
            require(native_terminal({"events": events["destination"][:forge_after["sequence"] - 1]}, "forge", peers["source"], "failed"),
                    "Forge native owner failure did not precede retained relay echo")
    if spec["outcome"] == "success":
        direct = directs[donor_role]
        coordinated = [e for e in donor_events if e.get("kind") == "native_coordinated_authenticated"
                       and e.get("connection_id") == direct["connection_id"] and e.get("remote_peer_id") == peers[other_role]]
        expected_security = ("client" if donor_role == "source" else "server") if spec[donor_role] == "go" else "client"
        require(len(coordinated) == 1 and coordinated[0].get("security_role") == expected_security,
                "missing actual native coordinated QUIC security role (not requested endpoint role)")
        require(coordinated[0].get("security_role_basis") == ("native_quic_secured_callback"
                if spec[donor_role] == "go" else "pinned_quic_authenticated_transport_output"),
                "requested role is not actual QUIC handshake provenance")
        require(coordinated[0].get("authenticated") is True
                and any(stream[-1]["sequence"] < coordinated[0]["sequence"] for stream in exchanges),
                "direct authentication does not follow actual native SYNC")
        if spec[donor_role] == "go":
            receipt = coordinated[0].get("secured_receipt", {})
            require(isinstance(receipt, dict)
                    and receipt.get("source") == "go.quic.transport.InterceptSecured"
                    and receipt.get("native_connection_basis") == "network.Conn.As(**quic.Conn)"
                    and _id(receipt.get("native_connection_id"))
                    and receipt.get("local_peer_id") == peers[donor_role]
                    and receipt.get("remote_peer_id") == peers[other_role]
                    and receipt.get("direction") == ("outbound" if expected_security == "client" else "inbound")
                    and receipt.get("security_role") == expected_security
                    and _ns(receipt.get("observed_mono_ns"))
                    and receipt["observed_mono_ns"] <= coordinated[0]["mono_ns"],
                    "missing actual QUIC secured owner/role receipt")
            for side in ("local", "remote"):
                require(_udp_socket(receipt.get(side + "_address")) == _udp_socket(direct.get(side + "_address"))
                        == _udp_socket(coordinated[0].get(side + "_address")),
                        "QUIC secured receipt differs from actual authenticated socket")
            starts = [e for e in donor_events if e.get("native_type") == "StartHolePunch" and e.get("remote_peer_id") == peers[other_role]]
            require(starts and all(_ns(e.get("rtt_ns")) for e in starts), "missing native tracer RTT")
        else:
            # The pinned Rust behaviour uses normal dial opts for an inbound
            # CONNECT, and override_role() for an outbound CONNECT. QUIC still
            # authenticates both successful dial outputs as a TLS client.
            requested_role = "dialer" if donor_role == "source" else "listener"
            dials = [e for e in donor_events if e.get("kind") == "native_coordinated_dial"
                     and e.get("remote_peer_id") == peers[other_role]
                     and isinstance(e.get("address"), str) and "/quic-v1" in e["address"] and "/p2p-circuit" not in e["address"]
                     and e.get("requested_role") == requested_role
                     and any(stream[-1]["sequence"] < e["sequence"] < coordinated[0]["sequence"] for stream in exchanges)]
            require(dials and coordinated[0].get("requested_role") == requested_role,
                    "missing actual native coordinated QUIC dial after SYNC")
            completions = [e for e in donor_events if e.get("kind") == "native_dcutr_event"
                           and e.get("remote_peer_id") == peers[other_role] and e.get("result", {}).get("native_success") is True]
            require(len(completions) == 1, "missing/ambiguous native successful DCUtR completion")
            native = completions[0]
            swarm = _one(donor_events, "swarm_connection", native_connection_id=native["result"].get("connection_id"))
            require(swarm.get("remote_peer_id") == peers[other_role] and swarm.get("endpoint") == direct.get("endpoint"),
                    "native DCUtR ConnectionId does not match actual authenticated transport endpoint")
            _rust_relay_retirement(donor_events, token, peers[other_role], peers["relay"],
                                   relays[donor_role], direct, native)
    elif spec["outcome"] == "cancelled":
        cancellations = [(role, e) for role in ("source", "destination") for e in events[role]
                         if e.get("kind") == "cancellation_requested"]
        require(len(cancellations) == 1 and record.get("cancellation", {}).get("observed_active_phase") is True,
                "cancellation lacks controller observation of actual unfinished native I/O phase")
        role, cancelled = cancellations[0]
        other_peer = peers["destination" if role == "source" else "source"]
        captured = record["cancellation"].get("before_request", {})
        prefix = captured.get("events")
        require(record["cancellation"].get("actor") == role and captured.get("implementation") == spec[role]
                and captured.get("case_token") == token and captured.get("local_peer_id") == peers[role]
                and isinstance(prefix, list) and prefix and prefix == events[role][:len(prefix)]
                and prefix[-1]["sequence"] < cancelled["sequence"], "missing actual pre-cancel raw event prefix")
        wire_role = record["cancellation"].get("wire_actor")
        wire = record["cancellation"].get("wire_before_request", {})
        wire_events = wire.get("events")
        require(wire_role == donor_role and wire.get("implementation") == spec[wire_role]
                and wire.get("case_token") == token and wire.get("local_peer_id") == peers[wire_role]
                and isinstance(wire_events, list) and wire_events and wire_events == events[wire_role][:len(wire_events)]
                and any(e.get("kind") == "dcutr_frame" and e.get("remote_peer_id") == peers[other_role] for e in wire_events)
                and (spec[wire_role] == "go" or not any(native_terminal(wire, spec[wire_role], peers[other_role], outcome)
                                                       for outcome in ("success", "failed"))),
                "cancellation was not observed during actual unfinished donor DCUtR I/O")
        prior = {"events": [e for e in events[role] if e["sequence"] < cancelled["sequence"]]}
        require(spec[role] == "go" or not any(native_terminal(prior, spec[role], other_peer, outcome)
                                              for outcome in ("success", "failed")),
                "cancellation requested only after native completion")
        joined = _one(events[role], "native_service_joined", remote_peer_id=other_peer)
        if spec[donor_role] == "go":
            require(role == donor_role and cancelled.get("source") == "go.holepunch.Service.Close.request"
                    and cancelled.get("remote_peer_id") == other_peer, "missing supported native Go service cancellation")
            observed = go_cancel_state(wire, token, other_peer, role)
            require(observed["state"] == "active", "cancellation lacks an unfinished donor Go native method claim")
            require(go_cancel_state(captured, token, other_peer, role) == observed
                    and go_cancel_state({**captured, **prior}, token, other_peer, role) == observed,
                    "Go native cancellation attempt changed or completed before request")
            drain = _one(events[role], "native_handlers_drained", remote_peer_id=other_peer)
            require(drain.get("source") == "go.native_dcutr.handler_return" and type(drain.get("entered")) is int
                    and drain["entered"] >= (1 if role == "source" else 0) and type(drain.get("completed")) is int
                    and drain["completed"] == drain["entered"] and type(drain.get("active")) is int and drain["active"] == 0
                    and cancelled["sequence"] < drain["sequence"] < joined["sequence"], "Go inbound native handlers did not actually drain")
            require(joined.get("source") == "go.holepunch.Service.Close.and_handler_drain", "Close alone is not Go native handler join")
            claims = _go_cancel_claims(events[role], peers[role], other_peer)
            following = [e for e in claims if e["sequence"] > observed["start_sequence"]]
            require(following and following[0]["native_type"] in {"EndHolePunch", "ProtocolError"}
                    and following[0].get("success") is not True
                    and cancelled["sequence"] < following[0]["sequence"] < drain["sequence"],
                    "selected Go native method did not actually terminate after cancellation before drain")
            drained = go_cancel_state({**captured, "events": events[role][:joined["sequence"] - 1]}, token, other_peer, role)
            require(drained["state"] == "terminal" and not any(e["sequence"] > joined["sequence"] for e in claims),
                    "Go native workers remained active at service join")
        else:
            require(spec[role] == "forge" and role != donor_role
                    and cancelled.get("source") == "forge.node.async_cancel_hole_punch.request"
                    and cancelled.get("remote_peer_id") == other_peer, "Rust pairs must cancel actual Forge peer owner, not Rust/host stop")
            accepted = _one(events[role], "native_cancel_completed", remote_peer_id=other_peer)
            require(accepted.get("source") == "forge.node.async_cancel_hole_punch.return" and accepted.get("accepted") is True
                    and cancelled["sequence"] < accepted["sequence"] < joined["sequence"]
                    and joined.get("source") == "forge.node.async_cancel_hole_punch.join" and joined.get("accepted") is True,
                    "Forge cancellation did not accept and join the active shared native owner")
            if wave_scope:
                require("natural_terminal" not in record["cancellation"]
                        and rust_source_wave_pending(wire, token, peers[other_role])
                        and all(e["sequence"] > wire_events[-1]["sequence"] for e in wave_events
                                if e.get("kind") == "native_dcutr_wave_failed"),
                        "Rust SOURCE cancellation lacks an actual unfinished native wave before request")
            else:
                terminal = record["cancellation"].get("natural_terminal", {})
                terminal_events = terminal.get("events", [])
                require(terminal.get("implementation") == "rust" and terminal.get("case_token") == token
                        and terminal.get("local_peer_id") == peers[donor_role] and terminal_events
                        and terminal_events == donor_events[:len(terminal_events)]
                        and native_terminal(terminal, "rust", peers[other_role], "failed"),
                        "missing separate actual Rust natural terminal after Forge cancellation")
        require(joined.get("cancelled") is True and joined["sequence"] > cancelled["sequence"], "native cancelled owner was not joined")
        after = _one(events[role], "echo", phase="relay_after")
        require(after["sequence"] > joined["sequence"], "relay delivery preceded native cancellation join")


def validate_case(record):
    try:
        _validate(record)
    except (ValueError, TypeError, KeyError, AttributeError, OverflowError, StopIteration, IndexError) as error:
        return [str(error)]
    return []
