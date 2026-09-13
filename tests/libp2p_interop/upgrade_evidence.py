"""Validate bounded, actually observed Go upgrade events; never synthesize phases.

This module checks one outbound application stream. The caller must separately
bind the payload to an indexed snapshot of its successfully joined process.
"""

import re


HEADER = "/multistream/1.0.0"
MUXER = "/yamux/1.0.0"


def integer(value, low, high):
    return type(value) is int and low <= value <= high


def echo_frame_size(payload):
    if not integer(payload, 1, 4096):
        raise ValueError("invalid echo payload byte count")
    size = payload + max(1, (payload.bit_length() + 6) // 7)
    if size > 4096:
        raise ValueError("echo framing exceeds the observation bound")
    return size


def wire_token(value):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-f]{4,516}", value) or len(value) % 2:
        raise ValueError("malformed negotiation frame hex")
    data = bytes.fromhex(value)
    length = 0
    for offset, byte in enumerate(data[:2]):
        length |= (byte & 127) << (7 * offset)
        if byte & 128:
            continue
        if offset and byte == 0:
            raise ValueError("noncanonical negotiation length")
        body = data[offset + 1:]
        if not 1 <= length <= 256 or len(body) != length or body[-1:] != b"\n":
            raise ValueError("invalid negotiation framing")
        token = body[:-1].decode("ascii")
        if token != "na" and (not token.startswith("/") or any(ord(c) <= 32 for c in token)):
            raise ValueError("invalid negotiation token")
        return token
    raise ValueError("oversized negotiation length")


def negotiation(events, phase, stream, protocol, direction):
    """Validate each wire direction independently, allowing bounded na fallback."""
    subset = [e for e in events if e.get("phase") == phase and e.get("stream_trace_id", 0) == stream]
    frames = [e for e in subset if e["kind"] == "multistream_frame"]
    selected = [e for e in subset if e["kind"] == "protocol_selected"]
    if len(selected) != 1 or selected[0].get("protocol") != protocol or not frames:
        raise ValueError("missing unique observed protocol selection")
    streams = {"read": [], "write": []}
    positions = {"read": [], "write": []}
    for frame in frames:
        token = wire_token(frame.get("frame_hex"))
        side = frame.get("direction")
        if side not in streams or frame.get("protocol") != token or frame["sequence"] >= selected[0]["sequence"]:
            raise ValueError("negotiation frame contradicts its selection")
        streams[side].append(token)
        positions[side].append(frame["sequence"])
    if any(not tokens or tokens[0] != HEADER for tokens in streams.values()):
        raise ValueError("missing bilateral multistream header")
    proposals = streams["write" if direction == "Outbound" else "read"][1:]
    replies = streams["read" if direction == "Outbound" else "write"][1:]
    if (not 1 <= len(proposals) <= 8 or len(proposals) != len(replies)
            or any(not p.startswith("/") or p == HEADER for p in proposals)
            or replies[:-1] != ["na"] * (len(proposals) - 1)
            or proposals[-1] != protocol or replies[-1] != protocol):
        raise ValueError("unmatched negotiation proposals and acknowledgments")
    rejected = [e for e in subset if e["kind"] == "protocol_rejected"]
    if [e.get("protocol") for e in rejected] != proposals[:-1] \
            or any(e["sequence"] >= selected[0]["sequence"] for e in rejected):
        raise ValueError("rejection events contradict observed fallback")
    # Delegate calls may complete on either thread first. Require each rejection
    # after both corresponding frames, not a fictitious global read/write order.
    for index, event in enumerate(rejected, 1):
        if not max(positions["read"][index], positions["write"][index]) < event["sequence"] \
                < min(positions["read"][index + 1], positions["write"][index + 1]):
            raise ValueError("rejection is not between its paired frames and the next proposal")
    return selected[0]["sequence"], min(e["sequence"] for e in frames)


def unique_event(events, kind, protocol=None):
    matches = [e for e in events if e["kind"] == kind]
    if len(matches) != 1 or (protocol is not None and matches[0].get("protocol") != protocol):
        raise ValueError("missing or contradictory " + kind)
    return matches[0]


def inbound_response(connection, protocol, mux_done):
    # Include failed attempts in uniqueness: a later successful stream cannot
    # erase evidence of a second exchange on the same authenticated connection.
    proposed = {e.get("stream_trace_id") for e in connection["events"]
                if e.get("phase") == "application_multistream" and e.get("kind") == "multistream_frame"
                and e.get("direction") == "read" and e.get("protocol") == protocol}
    matches = [s for s in connection["streams"]
               if s.get("direction") == "Inbound"
               and (s.get("protocol") == protocol or s["stream_trace_id"] in proposed)]
    if len(matches) != 1:
        raise ValueError("counterpart response is absent or ambiguous")
    stream = matches[0]
    if stream.get("response_write_complete") is not True or stream.get("failed") is not False \
            or stream.get("reset") is not False or stream.get("write_frames") != 1 \
            or type(stream.get("write_frames")) is not int \
            or not integer(stream.get("write_framed_bytes"), 1, 4096) \
            or not isinstance(stream.get("write_framed_sha256"), str) \
            or not re.fullmatch(r"[0-9a-f]{64}", stream["write_framed_sha256"]):
        raise ValueError("counterpart has not completed a bounded response")
    sid = stream["stream_trace_id"]
    events = [e for e in connection["events"] if e.get("stream_trace_id") == sid]
    if any("error" in e or e["kind"] in {"failure", "negotiation_incomplete", "stream_reset_returned"} for e in events):
        raise ValueError("counterpart response contains a failed operation")
    selected, first = negotiation(connection["events"], "application_multistream", sid, protocol, "Inbound")
    opened = unique_event(events, "stream_open")
    completed = unique_event(events, "response_write_complete", protocol)
    closes = [e["sequence"] for e in events
              if e["kind"] in {"stream_close_returned", "stream_close_write_returned"}]
    if (opened.get("direction") != "Inbound" or completed.get("direction") != "Inbound"
            or not 1 <= len(closes) <= 2
            or not mux_done < opened["sequence"] < first <= selected < min(closes) < completed["sequence"]):
        raise ValueError("counterpart response completion is not ordered after its exchange")
    return stream


def validate_go_listener_upgrade(payload, listener, remote, security):
    """Paired evidence, not a fabricated Forge transcript or Go delivery ACK.

    Callers must bind both payloads to their independent terminal-owned snapshots.
    The Forge receipt is a single fresh automatic Identify, while the Go receipt
    proves the unique corresponding inbound exchange on that authenticated link.
    """
    try:
        _validate_go_listener_upgrade(payload, listener, remote, security)
    except (ValueError, TypeError, KeyError, UnicodeError) as error:
        return ["paired Forge/Go upgrade evidence: " + str(error)]
    return []


def _validate_go_listener_upgrade(payload, listener, remote, security):
    if (not isinstance(listener, dict) or listener.get("implementation") != "go"
            or listener.get("role") != "listener" or listener.get("status") != "ok"
            or payload.get("implementation") != "forge" or payload.get("role") != "dialer"
            or payload.get("status") != "ok" or payload.get("scenario") not in {"identify", "echo"}
            or payload.get("single_fresh_connection_retained") is not True
            or payload.get("identify_event_basis") != "automatic_identify_single_fresh_connection"
            or not integer(payload.get("application_connection_id"), 1, 2**64 - 1)
            or type(payload.get("identify_event_connection_id")) is not int
            or payload["identify_event_connection_id"] != payload["application_connection_id"]
            or payload.get("signed_peer_record") is not True
            or not integer(payload.get("protocol_count"), 1, 128)
            or payload.get("negotiated_transport") != "tcp"
            or not isinstance(remote, str) or not remote or listener.get("local_peer_id") != remote
            or payload.get("authenticated_remote_peer_id") != remote):
        raise ValueError("missing fresh independently authenticated endpoint receipts")
    local = payload.get("local_peer_id")
    if not isinstance(local, str) or not local or local == remote:
        raise ValueError("missing distinct authenticated client identity")
    proof = listener.get("upgrade_observation")
    connections = observed_connections(proof)
    matches = [c for c in connections if c.get("authenticated_remote_peer_id") == local]
    if len(matches) != 1:
        raise ValueError("peer has multiple or absent authenticated counterpart connections")
    connection = matches[0]
    if (connection.get("authenticated_local_peer_id") != remote or connection.get("direction") != "Inbound"
            or not isinstance(connection.get("local_address"), str) or not connection["local_address"]
            or connection["local_address"] != payload.get("connection_remote_addr")
            or not isinstance(connection.get("remote_address"), str) or not connection["remote_address"]
            or connection.get("selected_security") != security or connection.get("selected_muxer") != MUXER
            or connection.get("underlying_close_returned") is not True or connection.get("failed") is not False
            or type(proof.get("target_connection_trace_id")) is not int
            or proof["target_connection_trace_id"] != connection["connection_trace_id"]):
        raise ValueError("counterpart authenticated address, identity or upgrade disagrees")
    mux_done = connection_phases(connection, security, "Inbound")
    identify = inbound_response(connection, "/ipfs/id/1.0.0", mux_done)
    if (type(identify.get("read_frames")) is not int or identify["read_frames"] != 0
            or type(identify.get("read_framed_bytes")) is not int or identify["read_framed_bytes"] != 0
            or identify.get("read_framed_sha256") is not None):
        raise ValueError("Identify response has unexpected inbound application data")
    target = identify
    if payload["scenario"] == "echo":
        protocol = "/forge/interop/relay-echo/1"
        target = inbound_response(connection, protocol, mux_done)
        framed_size = echo_frame_size(payload.get("payload_bytes"))
        if (payload.get("echo_ok") is not True or payload.get("application_close_returned") is not True
                or not integer(payload.get("application_stream_id"), 0, 2**63 - 1)
                or payload.get("application_protocol") != protocol or payload.get("protocol") != protocol
                or payload.get("negotiated_security") != security or payload.get("negotiated_muxer") != MUXER
                or type(target.get("read_frames")) is not int or target["read_frames"] != 1
                or not integer(target.get("read_framed_bytes"), 1, 4096)
                or target.get("read_framed_sha256") != target["write_framed_sha256"]
                or target["read_framed_bytes"] != framed_size
                or target["read_framed_bytes"] != target["write_framed_bytes"]):
            raise ValueError("echo lacks matching actual request/response and close")
        for side, source in (("request", "read"), ("response", "write")):
            count = payload.get(f"application_{side}_framed_bytes")
            if (not integer(count, 1, 4096) or count != target[f"{source}_framed_bytes"]
                    or payload.get(f"application_{side}_framed_sha256") != target[f"{source}_framed_sha256"]):
                raise ValueError("echo application and counterpart framing differ")
    if (proof.get("target_stream_trace_ids") != [target["stream_trace_id"]]
            or any(type(s) is not int for s in proof["target_stream_trace_ids"])):
        raise ValueError("selected counterpart target is not the independently matched exchange")


def validate_go_dial_upgrade(payload, remote, security):
    """Return errors for a native Go dialer's exact application-stream proof."""
    try:
        _validate_go_dial_upgrade(payload, remote, security)
    except (ValueError, TypeError, KeyError, UnicodeError) as error:
        return ["Go upgrade evidence: " + str(error)]
    return []


def observed_connections(proof):
    if (not isinstance(proof, dict)
            or proof.get("source") != "go-libp2p.public-upgrade-hooks.v1"
            or proof.get("finalized_after_host_close") is not True
            or proof.get("complete") is not True or proof.get("overflow") is not False):
        raise ValueError("missing finalized complete bounded observation")
    connections = proof.get("connections")
    if not isinstance(connections, list) or not 1 <= len(connections) <= 16:
        raise ValueError("invalid connection bound")
    ids = []
    for connection in connections:
        if not isinstance(connection, dict):
            raise ValueError("invalid connection record")
        cid = connection.get("connection_trace_id")
        if not integer(cid, 1, 16) or cid in ids:
            raise ValueError("invalid or duplicate connection trace id")
        ids.append(cid)
        events, streams = connection.get("events"), connection.get("streams")
        if not isinstance(events, list) or not 1 <= len(events) <= 256 \
                or not isinstance(streams, list) or len(streams) > 32:
            raise ValueError("invalid event or stream bound")
        for sequence, event in enumerate(events, 1):
            if (not isinstance(event, dict) or type(event.get("sequence")) is not int
                    or event["sequence"] != sequence or not isinstance(event.get("kind"), str)):
                raise ValueError("invalid ordered event sequence")
            if "stream_trace_id" in event and not integer(event["stream_trace_id"], 1, 32):
                raise ValueError("invalid event stream trace id")
            # Failed/background proposals still participate in uniqueness. Their
            # readable labels must never conceal different captured wire bytes.
            if event["kind"] == "multistream_frame" and wire_token(event.get("frame_hex")) != event.get("protocol"):
                raise ValueError("observed negotiation label contradicts captured wire")
        stream_ids = [s.get("stream_trace_id") for s in streams if isinstance(s, dict)]
        if (len(stream_ids) != len(streams) or any(not integer(s, 1, 32) for s in stream_ids)
                or len(set(stream_ids)) != len(stream_ids)):
            raise ValueError("invalid or duplicate stream trace id")
        if any(e["stream_trace_id"] not in stream_ids for e in events if "stream_trace_id" in e):
            raise ValueError("event refers to an absent stream")
    return connections


def connection_phases(connection, security, direction):
    events = connection["events"]
    if any(e["kind"] == "failure" for e in events):
        raise ValueError("target connection failed")
    security_selected, _ = negotiation(events, "security_multistream", 0, security, direction)
    entered = unique_event(events, "security_enter", security)["sequence"]
    secured = unique_event(events, "security_complete", security)["sequence"]
    mux_enter = unique_event(events, "muxer_enter", MUXER)["sequence"]
    mux_done = unique_event(events, "muxer_complete", MUXER)["sequence"]
    if not security_selected < entered < secured < mux_enter < mux_done:
        raise ValueError("security and muxer phases are out of order")
    early = connection.get("early_muxer_negotiation")
    if early is True:
        selected = unique_event(events, "security_handshake_muxer_selected", MUXER)["sequence"]
        if not secured < selected < mux_enter or any(e.get("phase") == "muxer_multistream" for e in events):
            raise ValueError("early muxer proof mixes incompatible negotiation paths")
    elif early is False:
        selected, first = negotiation(events, "muxer_multistream", 0, MUXER, direction)
        if not secured < first <= selected < mux_enter \
                or any(e["kind"] == "security_handshake_muxer_selected" for e in events):
            raise ValueError("regular muxer proof mixes incompatible negotiation paths")
    else:
        raise ValueError("missing explicit early muxer state")
    return mux_done


def _validate_go_dial_upgrade(payload, remote, security):
    if payload.get("implementation") != "go" or payload.get("role") != "dialer" or payload.get("status") != "ok":
        raise ValueError("not a successful Go dialer")
    proof = payload.get("upgrade_observation")
    connections = observed_connections(proof)

    cid = payload.get("application_connection_trace_id")
    sid = payload.get("application_stream_trace_id")
    if (not integer(cid, 1, 16) or not integer(sid, 1, 32)
            or type(proof.get("target_connection_trace_id")) is not int
            or proof["target_connection_trace_id"] != cid
            or proof.get("target_stream_trace_ids") != [sid]
            or any(type(s) is not int for s in proof["target_stream_trace_ids"])):
        raise ValueError("target differs from the application-bound stream")
    matches = [c for c in connections if c["connection_trace_id"] == cid]
    if len(matches) != 1:
        raise ValueError("missing application connection")
    connection = matches[0]
    expected = {
        "direction": "Outbound", "authenticated_local_peer_id": payload.get("local_peer_id"),
        "authenticated_remote_peer_id": remote, "local_address": payload.get("connection_local_addr"),
        "remote_address": payload.get("connection_remote_addr"), "selected_security": security,
        "selected_muxer": MUXER,
    }
    if (any(not isinstance(v, str) or not v or connection.get(k) != v for k, v in expected.items())
            or connection.get("failed") is not False or connection.get("underlying_close_returned") is not True
            or payload.get("authenticated_remote_peer_id") != remote
            or payload.get("negotiated_transport") != "tcp"
            or payload.get("negotiated_security") != security or payload.get("negotiated_muxer") != MUXER):
        raise ValueError("application connection facts disagree")
    events = connection["events"]
    mux_done = connection_phases(connection, security, "Outbound")

    matches = [s for s in connection["streams"] if s["stream_trace_id"] == sid]
    if len(matches) != 1:
        raise ValueError("missing application stream")
    stream = matches[0]
    application = payload.get("application_protocol")
    network_id = payload.get("application_stream_id")
    connection_id = payload.get("application_connection_id")
    if (not all(isinstance(x, str) and x for x in (application, network_id, connection_id))
            or stream.get("protocol") != application or stream.get("direction") != "Outbound"
            or stream.get("network_stream_id") != network_id or stream.get("application_io_complete") is not True
            or stream.get("failed") is not False or stream.get("reset") is not False):
        raise ValueError("application stream is unbound, failed or incomplete")
    for direction in ("read", "write"):
        count, frames = stream.get(direction + "_framed_bytes"), stream.get(direction + "_frames")
        digest = stream.get(direction + "_framed_sha256")
        if (not integer(count, 0, 4096) or not integer(frames, 0, 4)
                or (count == 0 and (frames != 0 or digest is not None))
                or (count > 0 and (frames == 0 or not isinstance(digest, str)
                                   or not re.fullmatch(r"[0-9a-f]{64}", digest)))):
            raise ValueError("invalid completed application framing digest")
    if stream["read_frames"] == 0:
        raise ValueError("application completion lacks a response")
    if payload.get("scenario") in {"identify", "echo"} and (
        payload.get("signed_peer_record") is not True
        or payload.get("identify_event_basis") != "automatic_identify_separate_exchange_same_connection"
        or payload.get("identify_event_connection_id") != connection_id
    ):
        raise ValueError("automatic Identify is not bound to the application connection")
    if payload.get("scenario") == "identify" and (
        application != "/ipfs/id/1.0.0"
        or not integer(payload.get("payload_bytes"), 1, 4096)
        or payload["payload_bytes"] != stream["read_framed_bytes"]
    ):
        raise ValueError("Identify application framing disagrees with the observed stream")
    if payload.get("scenario") == "echo" and application != "/forge/interop/relay-echo/1":
        raise ValueError("echo application is not the observed echo protocol")
    if application == "/forge/interop/relay-echo/1" and (
        stream["read_frames"] != 1 or stream["write_frames"] != 1
        or stream["read_framed_bytes"] != echo_frame_size(payload.get("payload_bytes"))
        or stream["read_framed_bytes"] != stream["write_framed_bytes"]
        or stream["read_framed_sha256"] != stream.get("write_framed_sha256")
    ):
        raise ValueError("echo request and reply observations differ")
    stream_events = [e for e in events if e.get("stream_trace_id") == sid]
    if any("error" in e or e["kind"] in {"negotiation_incomplete", "stream_reset_returned", "failure"} for e in stream_events):
        raise ValueError("target stream contains a failure")
    selected, first = negotiation(events, "application_multistream", sid, application, "Outbound")
    opened = unique_event(stream_events, "stream_open")
    completed = unique_event(stream_events, "application_io_complete", application)
    closed = unique_event(stream_events, "stream_close_returned")
    bindings = [e for e in stream_events if e["kind"] == "application_stream_binding"]
    if (len(bindings) != 2 or any(e.get("network_connection_id") != connection_id
                           or e.get("network_stream_id") != network_id or e.get("protocol") != application
                           or not opened["sequence"] < e["sequence"] < closed["sequence"]
                           for e in bindings)
            or completed.get("network_stream_id") != network_id or closed.get("error")
            or opened.get("direction") != "Outbound"
            or not bindings[0]["sequence"] < bindings[1]["sequence"]
            or not selected < bindings[1]["sequence"]
            or not mux_done < opened["sequence"] < first <= selected < closed["sequence"] < completed["sequence"]):
        raise ValueError("application completion does not follow its observed stream negotiation")
