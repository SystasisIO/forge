"""PR10 semantics from actual endpoint connections and bounded public wire receipts.

Configured transport/security names and top-level status are never proof. Go
ConnState or Rust completed delegates corroborate Forge's live stream/session.
"""

import hashlib
import re

from autorelay_wire import _fields, varint
from rust_upgrade_evidence import (
    _peer, _varint, _negotiation, _upgrades, _lifecycle, _selection, _events, _milestones,
    _transport_receipts, _swarm_events, _bound_remote_endpoint,
    ERROR_EVENTS,
)
from upgrade_evidence import negotiation as go_negotiation, wire_token

ECHO = "/forge/interop/relay-echo/1"
MUXER = "/yamux/1.0.0"
PAYLOAD = b"private-profile-exchange"
UNKNOWN = "/forge/interop/private-unknown/1"
PRIVATE_CONTRACTS = {
    "tcp_yamux_private_pnet": (ECHO, "/noise"),
    "multistream_select_private_pnet": (ECHO, "/noise"),
    "noise_identity_private_pnet": (ECHO, "/noise"),
    "tls_identity_private_pnet": (ECHO, "/tls/1.0.0"),
    "ping_private_tcp_yamux_pnet": ("/ipfs/ping/1.0.0", "/noise"),
    "identify_private_tcp_yamux_pnet": ("/ipfs/id/1.0.0", "/noise"),
    "kademlia_amino_private_tcp_yamux_pnet": ("/ipfs/kad/1.0.0", "/noise"),
    "rendezvous_rust_private_tcp_yamux_pnet": ("/rendezvous/1.0.0", "/noise"),
}
INLINE_CONTRACTS = {
    "inline_muxer_go_noise": (ECHO, "/noise"),
    "inline_muxer_go_tls": (ECHO, "/tls/1.0.0"),
    "inline_muxer_go_noise_private_pnet": (ECHO, "/noise"),
    "inline_muxer_go_tls_private_pnet": (ECHO, "/tls/1.0.0"),
    "inline_muxer_rust_noise_fallback": (ECHO, "/noise"),
    "inline_muxer_rust_tls_fixed_alpn_fallback": (ECHO, "/tls/1.0.0"),
    "inline_muxer_rust_noise_fallback_private_pnet": (ECHO, "/noise"),
    "inline_muxer_rust_tls_fixed_alpn_fallback_private_pnet": (ECHO, "/tls/1.0.0"),
}
CONTRACTS = PRIVATE_CONTRACTS | INLINE_CONTRACTS


def require(value, reason):
    if not value:
        raise ValueError(reason)


def framed_receipt(value, *, raw=False):
    require(isinstance(value, dict), "missing public protocol receipt")
    wire = value.get("framed_hex")
    require(isinstance(wire, str) and 0 < len(wire) <= 16392 and len(wire) % 2 == 0
            and re.fullmatch(r"[a-f0-9]+", wire), "invalid bounded public frame hex")
    data = bytes.fromhex(wire)
    body = value.get("read")
    require(isinstance(body, dict) and type(body.get("framed_bytes")) is int
            and body["framed_bytes"] == len(data) and body.get("framed_sha256") == hashlib.sha256(data).hexdigest()
            and type(body.get("frames")) is int and body["frames"] == 1 and body.get("complete_frames") is True
            and body.get("invalid_or_over_limit") is False and value.get("raw", False) is raw,
            "public frame bytes/hash/framing contradiction")
    if raw:
        require(len(data) == 32, "Ping is not exactly 32 observed bytes")
        return data
    size, prefix = _varint(data, 0)
    require(0 < size <= 8192 and data[:prefix] == varint(size) and prefix + size == len(data),
            "noncanonical, truncated or concatenated public frame")
    return data[prefix:]


def singleton(fields, key, wire):
    values = fields.get(key, [])
    require(len(values) == 1 and values[0][0] == wire, "missing or ambiguous protobuf field")
    return values[0][1]


def public_key_peer(key):
    fields = _fields(key)
    require(singleton(fields, 1, 0) == 1 and len(singleton(fields, 2, 2)) == 32
            and set(fields) == {1, 2}, "fixture record is not canonical Ed25519")
    return b"\x00" + varint(len(key)) + key


def signed_record(envelope, peer):
    fields = _fields(envelope)
    require(public_key_peer(singleton(fields, 1, 2)) == _peer(peer), "signed record signer differs from peer")
    require(singleton(fields, 2, 2) in (b"\x03\x01", b"/libp2p/routing-state-record")
            and len(singleton(fields, 5, 2)) == 64, "record domain type/signature shape mismatch")
    payload = _fields(singleton(fields, 3, 2))
    require(singleton(payload, 1, 2) == _peer(peer) and singleton(payload, 2, 0) > 0,
            "signed record peer or sequence mismatch")
    addresses = payload.get(3, [])
    require(1 <= len(addresses) <= 16 and all(w == 2 and singleton(_fields(a), 1, 2) for w, a in addresses),
            "signed record lacks bounded actual addresses")


def application(result, protocol, peer):
    require(result.get("protocol") == protocol and result.get("stream_closed") is True,
            "requested protocol was not opened/completed on the actual stream")
    raw = protocol == "/ipfs/ping/1.0.0"
    response = framed_receipt(result.get("response"), raw=raw)
    request = None if protocol == "/ipfs/id/1.0.0" else framed_receipt(result.get("request"), raw=raw)
    if protocol == ECHO:
        require(request == response == PAYLOAD, "echo request/response does not contain the bounded challenge")
    elif raw:
        require(request == response == bytes(range(1, 33)), "Ping did not echo the actual 32-byte challenge")
    elif protocol == "/ipfs/id/1.0.0":
        require("request" not in result and result.get("identify_verified") is True,
                "Identify lacks a native signature/identity verification receipt")
        fields = _fields(response)
        key = singleton(fields, 1, 2)
        require(public_key_peer(key) == _peer(peer), "Identify public key does not bind the authenticated peer")
        envelope = singleton(fields, 8, 2)
        require(singleton(_fields(envelope), 1, 2) == key, "Identify key differs from signed record signer")
        signed_record(envelope, peer)
        require((2, b"/ipfs/ping/1.0.0") in fields.get(3, []) and (2, ECHO.encode()) in fields.get(3, []),
                "Identify protocol list is not the live fixture service list")
    elif protocol == "/ipfs/kad/1.0.0":
        sent, received = _fields(request), _fields(response)
        require(singleton(sent, 1, 0) == singleton(received, 1, 0) == 4
                and singleton(sent, 2, 2) == _peer(peer), "not a FIND_NODE roundtrip to the expected peer key")
        require(2 not in received or singleton(received, 2, 2) == _peer(peer), "FIND_NODE response key mismatch")
        require(3 not in received and 9 not in received, "FIND_NODE was replaced by another DHT operation")
        require(len(received.get(8, [])) <= 20, "FIND_NODE peer count exceeds donor bound")
        for wire, entry in received.get(8, []):
            require(wire == 2, "invalid closer peer wire type")
            values = _fields(entry)
            require(0 < len(singleton(values, 1, 2)) <= 64 and all(w == 2 and a for w, a in values.get(2, [])),
                    "malformed actual closer peer result")
    elif protocol == "/rendezvous/1.0.0":
        sent, received = _fields(request), _fields(response)
        require(singleton(sent, 1, 0) == 3 and singleton(received, 1, 0) == 4, "not a Rendezvous discover response")
        discover = _fields(singleton(sent, 5, 2))
        require(singleton(discover, 1, 2) == b"forge.discovery" and singleton(discover, 2, 0) == 10,
                "Rendezvous namespace or requested limit differs")
        reply = _fields(singleton(received, 6, 2))
        require(singleton(reply, 3, 0) == 0 and 0 < len(singleton(reply, 2, 2)) <= 8192
                and len(reply.get(1, [])) == 1, "Rendezvous has no successful one-record discovery/cookie")
        registration = _fields(singleton(reply, 1, 2))
        require(singleton(registration, 1, 2) == b"forge.discovery" and singleton(registration, 3, 0) == 7200,
                "Rendezvous discovered namespace/TTL does not match registration")
        signed_record(singleton(registration, 2, 2), result["local_peer_id"])
        require(result.get("rendezvous_verified") is True, "Rendezvous record was not verified by the native decoder")
    return request, response


def tcp_address(value):
    require(isinstance(value, str), "missing observed TCP endpoint")
    match = re.fullmatch(r"/ip4/127\.0\.0\.1/tcp/([1-9][0-9]{0,4})", value)
    require(match and int(match[1]) <= 65535, "endpoint is not the actual numeric loopback TCP socket")
    return value


def logical_tcp_address(value, peer):
    """Only the logical dial address may carry one authenticated peer suffix."""
    _peer(peer)
    require(isinstance(value, str), "missing logical TCP dial address")
    if "/p2p/" in value:
        value, suffix = value.rsplit("/p2p/", 1)
        require(suffix == peer, "logical endpoint peer suffix mismatch")
    return tcp_address(value)


def connection_receipt(value, implementation, local, remote, security):
    sources = {"forge": "forge.authenticated-stream-retained-session", "go": "go-libp2p.network.Conn.ConnState",
               "rust": "rust-libp2p.completed-public-upgrades"}
    require(isinstance(value, dict) and value.get("source") == sources[implementation]
            and value.get("local_peer_id") == local and value.get("remote_peer_id") == remote
            and value.get("security") == security and value.get("muxer") == MUXER and value.get("transport") == "tcp",
            "missing actual authenticated security/muxer/identity receipt")
    cid = value.get("connection_id")
    require(type(cid) is int and cid > 0 or isinstance(cid, str) and 0 < len(cid) <= 256,
            "actual connection receipt has no identity")
    tcp_address(value.get("local_address"))
    return tcp_address(value.get("remote_address"))


def native_stream_open(connection, stream, implementation, direction):
    kind = "stream_open" if implementation == "go" else "substream_opened"
    scoped = [e for e in connection["events"] if e.get("stream_trace_id") == stream["stream_trace_id"]]
    opened = [e for e in scoped if e.get("kind") == kind]
    require(len(opened) == 1, "protocol barrier stream lacks one actual native open")
    opened = opened[0]
    require(all(type(e.get("stream_trace_id")) is int for e in scoped),
            "protocol barrier stream event has no native integer identity")
    if implementation == "go":
        require(set(opened) == {"sequence", "kind", "stream_trace_id", "direction"}
                and opened["direction"] == direction, "Go protocol barrier native open direction/shape mismatch")
    else:
        require(set(opened) == {"sequence", "kind", "phase", "stream_trace_id", "detail"}
                and opened["phase"] == "application" and opened["detail"] == {"direction": direction},
                "Rust protocol barrier native open direction/shape mismatch")
    require(stream.get("direction") == direction
            and all(e["sequence"] > opened["sequence"] for e in scoped if e is not opened),
            "protocol barrier stream has events before its native open")
    return opened["sequence"]


def protocol_barrier(connection, implementation, direction, receipt=None):
    """One observed UNKNOWN/na exchange, never a configured or first/last stream."""
    cid = connection.get("connection_trace_id")
    require(type(cid) is int and 1 <= cid <= 16, "protocol barrier has no bounded raw connection identity")
    native_direction = direction.capitalize() if implementation == "go" else direction
    require(connection.get("direction") == native_direction, "protocol barrier connection direction mismatch")
    phase, kind = ("application_multistream", "multistream_frame") if implementation == "go" else ("application", "negotiation_frame")
    frames = [e for e in connection["events"] if e.get("phase") == phase and e.get("kind") == kind]
    unknown = {e.get("stream_trace_id") for e in frames if wire_token(e.get("frame_hex")) == UNKNOWN}
    require(len(unknown) == 1, "protocol barrier lacks one unique observed UNKNOWN exchange")
    sid = unknown.pop()
    require(type(sid) is int and 1 <= sid <= 32, "protocol barrier has no bounded raw stream identity")
    streams = [s for s in connection["streams"] if s.get("stream_trace_id") == sid]
    require(len(streams) == 1, "protocol barrier references an absent or duplicate raw stream")
    stream = streams[0]
    opened = native_stream_open(connection, stream, implementation, native_direction)
    scoped = [e for e in connection["events"] if e.get("stream_trace_id") == sid]
    frames = [e for e in scoped if e.get("phase") == phase and e.get("kind") == kind]
    require(len(frames) == 4, "protocol barrier requires exactly four actual canonical negotiation frames")
    sides = {side: [e for e in frames if e.get("direction") == side] for side in ("read", "write")}
    proposer, reply = ("write", "read") if direction == "outbound" else ("read", "write")
    require(all(len(value) == 2 for value in sides.values())
            and [wire_token(e["frame_hex"]) for e in sides[proposer]] == ["/multistream/1.0.0", UNKNOWN]
            and [wire_token(e["frame_hex"]) for e in sides[reply]] == ["/multistream/1.0.0", "na"]
            and sides[proposer][1]["sequence"] < sides[reply][1]["sequence"],
            "protocol barrier lacks the actual bilateral ordered UNKNOWN/na rejection")
    if implementation == "rust":
        require(direction == "outbound" and stream.get("protocol") is None and stream.get("proposed_protocol") is None
                and stream.get("parser_error") is None and stream.get("io_failed") is False
                and stream.get("drop_observed") is True and stream.get("negotiation_complete_frames") is True
                and stream.get("response_write_complete") is False
                and stream.get("upgrade_completed_sequence") is None and stream.get("response_completed_sequence") is None,
                "Rust protocol barrier is not a complete native rejected negotiation")
        empty = {"framed_bytes": 0, "framed_sha256": hashlib.sha256(b"").hexdigest(), "frames": 0,
                 "complete_frames": False, "invalid_or_over_limit": False}
        require(stream.get("read") == stream.get("write") == empty, "Rust protocol barrier contains application bytes")
        selected, _ = _negotiation(scoped, stream)
        require(selected is None, "Rust protocol barrier actually selected a protocol")
        rejected = sides[reply][1]["sequence"]
        keys = {"source", "connection_trace_id", "stream_trace_id", "direction", "protocol", "opened_sequence",
                "rejected_sequence", "read_frames", "write_frames", "native_error"}
        require(isinstance(receipt, dict) and set(receipt) == keys
                and receipt.get("source") == "rust-libp2p.observed-multistream-rejection.v1"
                and type(receipt.get("connection_trace_id")) is int and receipt["connection_trace_id"] == cid
                and type(receipt.get("stream_trace_id")) is int and receipt["stream_trace_id"] == sid
                and receipt.get("direction") == direction and receipt.get("protocol") == UNKNOWN
                and receipt.get("native_error") == "unsupported_protocol"
                and type(receipt.get("opened_sequence")) is int and receipt["opened_sequence"] == opened
                and type(receipt.get("rejected_sequence")) is int and receipt["rejected_sequence"] == rejected,
                "Rust protocol_barrier receipt differs from its actual connection/direction/rejection")
        for side, observed in sides.items():
            captured = receipt[side + "_frames"]
            require(isinstance(captured, list) and len(captured) == 2
                    and all(isinstance(frame, dict) and set(frame) == {"sequence", "framed_hex"}
                            and type(frame.get("sequence")) is int for frame in captured)
                    and captured == [{"sequence": e["sequence"], "framed_hex": e["frame_hex"]} for e in observed],
                    "Rust protocol_barrier receipt does not bind its exact observed canonical frames")
        require(not any(e.get("kind") in ERROR_EVENTS for e in scoped),
                "Rust protocol barrier contradicts failed native I/O")
    else:
        require(stream.get("protocol") == "" and stream.get("application_io_complete") is False
                and stream.get("response_write_complete") is False
                and type(stream.get("failed")) is bool and type(stream.get("reset")) is bool,
                "Go protocol barrier was substituted for an application stream")
        for side in sides:
            require(type(stream.get(side + "_frames")) is int and stream[side + "_frames"] == 0
                    and type(stream.get(side + "_framed_bytes")) is int and stream[side + "_framed_bytes"] == 0
                    and stream.get(side + "_framed_sha256", hashlib.sha256(b"").hexdigest()) == hashlib.sha256(b"").hexdigest(),
                    "Go protocol barrier contains application bytes")
        for frame in frames:
            require(set(frame) == {"sequence", "kind", "phase", "stream_trace_id", "direction", "protocol", "frame_hex"}
                    and frame["protocol"] == wire_token(frame["frame_hex"]), "Go barrier frame annotation differs from native bytes")
        rejected = [e for e in scoped if e.get("kind") == "protocol_rejected"]
        require(len(rejected) == 1 and set(rejected[0]) == {"sequence", "kind", "phase", "stream_trace_id", "protocol"}
                and rejected[0]["phase"] == phase and rejected[0]["protocol"] == UNKNOWN
                and rejected[0]["sequence"] > max(e["sequence"] for e in frames),
                "Go protocol barrier lacks the native rejection after both actual frames")
        rejected = rejected[0]["sequence"]
        # Pinned Go resets unsupported streams. Its parser consequently records
        # negotiation_incomplete after na, not a successful application close.
        incomplete = [e for e in scoped if e.get("kind") == "negotiation_incomplete"]
        resets = [e for e in scoped if e.get("kind") == "stream_reset_returned"]
        require(len(incomplete) <= 1 and stream["failed"] == bool(incomplete)
                and all(set(e) == {"sequence", "kind", "phase", "stream_trace_id", "error"}
                        and e["phase"] == phase and e["sequence"] > rejected
                        and isinstance(e["error"], str) and 1 <= len(e["error"]) <= 256 for e in incomplete)
                and len(resets) <= 1 and stream["reset"] == bool(resets)
                and all(set(e) == {"sequence", "kind", "stream_trace_id"} and e["sequence"] > rejected for e in resets)
                and all(e.get("kind") in {"stream_open", kind, "protocol_rejected", "negotiation_incomplete", "stream_reset_returned"}
                        for e in scoped), "Go rejection has pre-na or unrelated failed/selected/completed I/O")
    return rejected


def donor_observation(payload, implementation, local, remote, security, protocol, request, response, direction):
    native_direction = direction.capitalize() if implementation == "go" else direction
    proof = payload.get("private_observation" if implementation == "go" else "upgrade_observation")
    require(isinstance(proof, dict) and proof.get("overflow") is False,
            "missing or overflowed actual donor observation")
    if implementation == "go":
        require(proof.get("source") == "go-libp2p.capable-conn.v1" and proof.get("finalized_after_host_close") is True,
                "Go collector is not finalized from actual capable connections")
    else:
        require(proof.get("source") == "rust-libp2p.public-connection-upgrades.v1"
                and proof.get("finalized_after_swarm_drop") is True, "Rust observer is not finalized after Swarm drop")
        _lifecycle(payload, proof)
    connections = proof.get("connections")
    require(isinstance(connections, list) and 1 <= len(connections) <= 16,
            "donor connection bound is absent or exceeded")
    matches = [c for c in connections if c.get("authenticated_local_peer_id") == local
               and c.get("authenticated_remote_peer_id") == remote]
    require(len(matches) == 1, "ambiguous or absent donor authenticated connection")
    c = matches[0]
    require(c.get("selected_security") == security and c.get("selected_muxer") == MUXER
            and c.get("direction") == native_direction and type(c.get("early_muxer_negotiation")) is bool,
            "donor actual connection disagrees with the requested security/muxer/direction")
    tcp_address(c.get("local_address")); tcp_address(c.get("remote_address"))
    events = c.get("events")
    require(isinstance(events, list) and 1 <= len(events) <= 256
            and all(isinstance(e, dict) and type(e.get("sequence")) is int and e["sequence"] == index
                    for index, e in enumerate(events, 1)), "noncanonical or unbounded donor event sequence")
    if implementation == "rust":
        require(c.get("muxer_drop_observed") is True, "Rust authenticated muxer was not dropped")
        _transport_receipts(c)
        swarm = [e for e in _swarm_events(proof) if e["authenticated_remote_peer_id"] == remote]
        require(len(swarm) == 1 and _bound_remote_endpoint(c, swarm[0]) == c["remote_address"],
                "actual Rust transport output does not bind the established Swarm connection")
    else:
        require(c.get("failed") is False, "actual Go upgrade failed")
    streams = c.get("streams")
    require(isinstance(streams, list) and len(streams) <= 32, "donor stream bound exceeded")
    require(all(isinstance(s, dict) and type(s.get("stream_trace_id")) is int and 1 <= s["stream_trace_id"] <= 32 for s in streams)
            and len({s["stream_trace_id"] for s in streams}) == len(streams), "noncanonical/duplicate donor stream identity")
    def body_match(s, side, data):
        if data is None:
            return (s.get(side + "_framed_bytes") == 0 and s.get(side + "_frames") == 0
                    if implementation == "go" else s.get(side, {}).get("framed_bytes") == 0)
        if implementation == "go":
            return type(s.get(side + "_frames")) is int and s[side + "_frames"] == 1 \
                and s.get(side + "_framed_bytes") == len(data) and s.get(side + "_framed_sha256") == hashlib.sha256(data).hexdigest()
        body = s.get(side, {})
        return body.get("framed_bytes") == len(data) and body.get("framed_sha256") == hashlib.sha256(data).hexdigest() \
            and body.get("complete_frames") is True and body.get("invalid_or_over_limit") is False
    # The donor listener receives the dialer's request and writes its response.
    sent, received = (request, response) if direction == "outbound" else (response, request)
    candidates = [s for s in streams if s.get("protocol") == protocol and s.get("direction") == native_direction
                  and body_match(s, "write", sent) and body_match(s, "read", received)]
    if implementation == "go" and direction == "outbound":
        # Identify and Kademlia may also run autonomously with identical bytes.
        # Select by the actual one-shot network stream, not arrival order; its
        # two native binding events are still checked below before acceptance.
        sid = payload.get("stream_id")
        require(isinstance(sid, str) and 0 < len(sid) <= 256,
                "Go one-shot lacks its actual bounded network stream identity")
        candidates = [s for s in candidates if s.get("network_stream_id") == sid]
    if (implementation == "rust" and direction == "outbound" and protocol in {"/ipfs/id/1.0.0", "/rendezvous/1.0.0"}
            or implementation == "go" and direction == "inbound" and protocol == "/ipfs/id/1.0.0"):
        rejected = protocol_barrier(c, implementation, direction, payload.get("protocol_barrier"))
        candidates = [s for s in candidates
                      if native_stream_open(c, s, implementation, native_direction) > rejected]
    require(len(candidates) == 1, "actual protocol response is not uniquely bound to a raw donor substream")
    stream = candidates[0]
    if implementation == "go":
        require(stream.get("failed") is False and stream.get("reset") is False, "matched Go application failed/reset")
        require(stream.get("application_io_complete" if direction == "outbound" else "response_write_complete") is True,
                "matched Go stream has no actual completed application I/O")
        require(not any("error" in e for e in events if e.get("stream_trace_id") == stream["stream_trace_id"]),
                "matched Go stream contradicts a failed operation")
        go_negotiation(c["events"], "application_multistream", stream["stream_trace_id"], protocol, native_direction)
    else:
        for selection in c.get("negotiations", []) + streams:
            _selection(selection)
        _events(c, streams)
        _milestones(c)
        require(stream.get("parser_error") is None and stream.get("io_failed") is False and stream.get("drop_observed") is True,
                "matched Rust stream failed or was not destroyed")
        events = [e for e in c["events"] if e["phase"] == "application" and e["stream_trace_id"] == stream["stream_trace_id"]]
        selected, _ = _negotiation(events, stream)
        require(selected is not None, "Rust application proposal did not receive its actual ACK")
        boundary = None
        if direction == "inbound":
            boundary = stream.get("response_completed_sequence")
            marks = [e for e in events if e["kind"] == "response_completion"]
            require(type(boundary) is int and len(marks) == 1 and marks[0]["sequence"] == boundary
                    and stream.get("response_write_complete") is True
                    and marks[0].get("detail", {}).get("write") == stream["write"],
                    "matched Rust response lacks its exact completed-write boundary")
        # Keep shutdown failures in history. Only an inbound response's actual
        # completed write/close, bound above to the requested bytes, permits a
        # later post-upgrade failure; an outbound request marker is not a reply.
        _upgrades(c, security, remote, last_required_response=boundary)
    if direction == "outbound":
        receipt = payload.get("connection_receipt", {})
        require(tcp_address(receipt.get("local_address")) == tcp_address(c["local_address"])
                and tcp_address(receipt.get("remote_address")) == tcp_address(c["remote_address"]),
                "donor connection receipt differs from its actual socket trace")
        if implementation == "rust":
            require(type(receipt.get("connection_id")) is int
                    and receipt["connection_id"] == c["connection_trace_id"], "Rust receipt uses another upgrade trace")
        else:
            bindings = [e for e in events if e.get("kind") == "application_stream_binding"
                        and e.get("stream_trace_id") == stream["stream_trace_id"]]
            require(len(bindings) == 2 and stream.get("application_io_complete") is True
                    and stream.get("network_stream_id") == payload.get("stream_id")
                    and all(e.get("network_connection_id") == receipt.get("connection_id")
                            and e.get("network_stream_id") == payload.get("stream_id") and e.get("protocol") == protocol
                            for e in bindings), "Go one-shot actual stream/connection binding is absent")
    return c


def unsupported_protocol(connection, implementation, direction):
    phase, kind = ("application_multistream", "multistream_frame") if implementation == "go" else ("application", "negotiation_frame")
    events = [e for e in connection["events"] if e.get("phase") == phase and e.get("kind") == kind]
    ids = {e.get("stream_trace_id") for e in events if wire_token(e.get("frame_hex")) == UNKNOWN}
    require(len(ids) == 1, "unknown protocol lacks one observed donor negotiation")
    sid = ids.pop()
    require(type(sid) is int and 1 <= sid <= 32, "unknown protocol has no actual bounded stream identity")
    frames = [e for e in events if e.get("stream_trace_id") == sid]
    tokens = {"read": [], "write": []}
    for event in frames:
        require(event.get("direction") in tokens, "unknown protocol frame direction mismatch")
        tokens[event["direction"]].append(wire_token(event.get("frame_hex")))
    proposer = "write" if direction == "outbound" else "read"
    reply = "read" if proposer == "write" else "write"
    require(tokens[proposer] == ["/multistream/1.0.0", UNKNOWN]
            and tokens[reply] == ["/multistream/1.0.0", "na"], "unknown protocol lacks a real bilateral na rejection")


def validate_private_profile(result, record, listener):
    """Registry-compatible entrypoint; every claimed id runs full packet semantics."""
    try:
        name = record.get("acceptance_scenario_id")
        require(name in CONTRACTS, "unknown private/inline evidence contract")
        protocol, security = CONTRACTS[name]
        dialer, server = record.get("dialer"), record.get("listener")
        require((dialer, server) in (("forge", "go"), ("go", "forge"), ("forge", "rust"), ("rust", "forge")),
                "contract lacks a bilateral Forge/donor direction")
        require(isinstance(listener, dict) and result.get("implementation") == dialer and listener.get("implementation") == server
                and result.get("role") == "dialer" and listener.get("role") == "listener"
                and result.get("scenario") == listener.get("scenario") == name and result.get("status") == listener.get("status") == "ok",
                "contradictory endpoint roles/scenarios or terminal status")
        peer, local = record.get("peer_id"), result.get("local_peer_id")
        _peer(peer); _peer(local)
        require(peer != local and listener.get("local_peer_id") == peer, "paired local/remote identity mismatch")
        if name == "rendezvous_rust_private_tcp_yamux_pnet":
            require("rust" in (dialer, server), "Go does not implement the Rendezvous contract")
        request, response = application(result, protocol, peer)
        request_wire = None if request is None else bytes.fromhex(result["request"]["framed_hex"])
        response_wire = bytes.fromhex(result["response"]["framed_hex"])
        remote_address = connection_receipt(result.get("connection_receipt"), dialer, local, peer, security)
        require(remote_address == logical_tcp_address(record.get("addr"), peer), "application connection did not dial the observed listener")
        implementation = server if server != "forge" else dialer
        donor = listener if server != "forge" else result
        donor_local, donor_remote = (peer, local) if server != "forge" else (local, peer)
        direction = "inbound" if server != "forge" else "outbound"
        c = donor_observation(donor, implementation, donor_local, donor_remote, security, protocol, request_wire, response_wire, direction)
        if server == "forge":
            receipt = listener.get("connection_receipt")
            address = connection_receipt(receipt, "forge", peer, local, security)
            require(address == tcp_address(c["local_address"]), "Forge inbound receipt is not paired with the donor socket")
            require(tcp_address(receipt.get("local_address")) == tcp_address(c["remote_address"]),
                    "Forge inbound local socket differs from the actual donor remote endpoint")
            require(framed_receipt(listener.get("probe")) == b"private-receipt", "Forge did not observe an authenticated reverse stream")
        else:
            require(tcp_address(c["local_address"]) == remote_address, "donor listener socket differs from Forge receipt")
            require(tcp_address(result["connection_receipt"].get("local_address")) == tcp_address(c["remote_address"]),
                    "Forge outbound local socket differs from actual donor ingress")
        if name == "multistream_select_private_pnet":
            require(result.get("unknown_protocol_rejected") is True, "missing actual unsupported-protocol rejection")
            unsupported_protocol(c, implementation, direction)
        if name.startswith("inline_muxer_go_"):
            require(implementation == "go" and c["early_muxer_negotiation"] is True,
                    "Go actual ConnState does not prove inline muxer negotiation")
            forge = result if dialer == "forge" else listener
            require(forge["connection_receipt"].get("early_muxer_negotiation") is True,
                    "Forge retained session did not observe inline negotiation")
        elif name.startswith("inline_muxer_rust_"):
            require(implementation == "rust" and c["early_muxer_negotiation"] is False,
                    "Rust fallback must have actual post-security multistream muxer frames")
            forge = result if dialer == "forge" else listener
            require(forge["connection_receipt"].get("early_muxer_negotiation") is False,
                    "Forge retained session contradicts fallback")
        if name in PRIVATE_CONTRACTS or name.endswith("_private_pnet"):
            fingerprint = result.get("pnet_fingerprint")
            require(isinstance(fingerprint, str) and re.fullmatch(r"[0-9a-f]{64}", fingerprint)
                    and listener.get("pnet_fingerprint") == fingerprint, "private endpoint fingerprint mismatch")
            # Existing rejection counter checks are shared, not redefined.
            from check_stage6_acceptance import pnet_control_errors
            for control in ("missing_key", "mismatched_key"):
                errors = pnet_control_errors(record, control)
                require(not errors, "; ".join(errors))
                require(record[control]["result"].get("implementation") == dialer
                        and record[control]["listener_result"].get("implementation") == server,
                        "private rejection counters came from different implementations")
    except (ValueError, TypeError, KeyError, IndexError, AttributeError) as error:
        return [str(error)]
    return []
