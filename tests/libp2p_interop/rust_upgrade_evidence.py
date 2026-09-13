"""Pure validation of Rust's native outbound upgrade/application observations.

Compose this with check_stage6_acceptance.validate_identify_evidence and the
existing indexed process/launcher validators. This module proves only upgrade
and application correlation: it does not parse Identify protobuf, validate a
signed envelope, authenticate a JSON producer, or claim remote receipt on drop.
"""

import hashlib
import ipaddress
import re

from upgrade_evidence import HEADER, MUXER, echo_frame_size, integer, wire_token

IDENTIFY = "/ipfs/id/1.0.0"
ECHO = "/forge/interop/relay-echo/1"
BASIS = "unique_authenticated_connection_and_framed_io"
EMPTY_HASH = hashlib.sha256(b"").hexdigest()
BASE58 = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
BODY_KEYS = {"framed_bytes", "frames", "framed_sha256", "complete_frames", "invalid_or_over_limit"}
ERROR_EVENTS = {
    "read_error", "write_error", "flush_error", "close_error", "upgrade_error",
    "muxer_inbound_error", "muxer_outbound_error", "muxer_close_error", "muxer_poll_error",
    "upgrade_future_dropped_before_completion",
}


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _object(value, name):
    _require(isinstance(value, dict), "invalid " + name)
    return value


def _list(value, low, high, name):
    _require(isinstance(value, list) and low <= len(value) <= high, "invalid " + name + " bound")
    return value


def _decimal(value):
    _require(isinstance(value, str) and re.fullmatch(r"[1-9][0-9]{0,19}", value)
             and int(value) <= 2**64 - 1, "invalid native task/Swarm id")
    return value


def _varint(data, offset):
    start, value = offset, 0
    while offset < len(data) and offset - start < 10:
        byte = data[offset]
        value |= (byte & 127) << (7 * (offset - start))
        offset += 1
        if not byte & 128:
            _require(value <= 2**64 - 1 and (offset - start == 1 or byte != 0), "noncanonical varint")
            return value, offset
    raise ValueError("truncated or oversized varint")


def _peer(value):
    _require(isinstance(value, str) and 2 <= len(value) <= 90
             and all(c in BASE58 for c in value), "malformed PeerId")
    number = 0
    for char in value:
        number = number * 58 + BASE58.index(char)
    raw = b"\0" * (len(value) - len(value.lstrip("1")))
    raw += number.to_bytes((number.bit_length() + 7) // 8, "big")
    code, offset = _varint(raw, 0)
    size, offset = _varint(raw, offset)
    _require(len(raw) == offset + size and ((code == 0 and 1 <= size <= 42)
             or (code == 0x12 and size == 32)), "invalid PeerId multihash")
    return raw


def _tcp(value, peer, suffix=False):
    _require(isinstance(value, str) and len(value) <= 256, "invalid TCP address")
    parts = value.split("/")
    if suffix and len(parts) == 7:
        _require(parts[5] == "p2p" and (peer is None or parts[6] == peer), "TCP peer suffix mismatch")
        _peer(parts[6])
        parts = parts[:5]
    _require(len(parts) == 5 and parts[0] == "" and parts[1] in {"ip4", "ip6"}
             and parts[3] == "tcp", "not a numeric direct TCP address")
    address = ipaddress.ip_address(parts[2])
    _require(address.version == (4 if parts[1] == "ip4" else 6)
             and str(address) == parts[2] and not address.is_unspecified and not address.is_multicast,
             "noncanonical or unusable TCP IP")
    _require(re.fullmatch(r"[1-9][0-9]{0,4}", parts[4]) and int(parts[4]) <= 65535,
             "invalid TCP port")
    return "/".join(parts)


def _hex(value, low, high):
    _require(isinstance(value, str) and 2 * low <= len(value) <= 2 * high
             and len(value) % 2 == 0 and re.fullmatch(r"[0-9a-f]*", value), "invalid bounded canonical hex")
    return bytes.fromhex(value)


def _body(value):
    value = _object(value, "framed body")
    _require(set(value) == BODY_KEYS, "unexpected framed body shape")
    size, frames = value["framed_bytes"], value["frames"]
    _require(integer(size, 0, 256 * 1024) and integer(frames, 0, size // 2)
             and type(value["complete_frames"]) is bool and type(value["invalid_or_over_limit"]) is bool,
             "invalid framed byte/count flags")
    _hex(value["framed_sha256"], 32, 32)
    if size == 0:
        _require(frames == 0 and value["framed_sha256"] == EMPTY_HASH
                 and value["complete_frames"] is False and value["invalid_or_over_limit"] is False,
                 "nonempty evidence for zero application bytes")
    if value["complete_frames"]:
        _require(frames > 0 and value["invalid_or_over_limit"] is False, "contradictory complete framing")
    return value


def _protocol(value):
    _require(isinstance(value, str) and 1 <= len(value) <= 255 and value.startswith("/")
             and value != HEADER and all(33 <= ord(c) <= 126 for c in value), "invalid protocol id")


def _selection(value):
    value = _object(value, "selection")
    _require(value.get("direction") in {"inbound", "outbound"}, "invalid stream direction")
    for key in ("drop_observed", "io_failed", "read_eof", "write_close_returned", "response_write_complete"):
        _require(type(value.get(key)) is bool, "invalid selection flag " + key)
    for key in ("protocol", "proposed_protocol"):
        _require(key in value, "missing " + key)
        if value[key] is not None:
            _protocol(value[key])
    _require("parser_error" in value and (value["parser_error"] is None
             or isinstance(value["parser_error"], str) and 1 <= len(value["parser_error"]) <= 256),
             "malformed parser error")
    _body(value.get("read"))
    _body(value.get("write"))
    for key in ("upgrade_completed_sequence", "response_completed_sequence"):
        _require(key in value and (value[key] is None or integer(value[key], 1, 256)),
                 "invalid completion sequence")
    return value


def _negotiation(events, record):
    """Replay observed tokens, including partial background attempts, not Go events."""
    headers = {"read": False, "write": False}
    pending, selected, proposed = {}, None, None
    proposer = "write" if record["direction"] == "outbound" else "read"
    count, selected_sequence, failure = 0, None, None
    frames = [e for e in events if e["kind"] == "negotiation_frame"]
    for event in frames:
        side, token = event.get("direction"), wire_token(event.get("frame_hex"))
        _require(all(33 <= ord(c) <= 126 for c in token), "invalid ASCII negotiation token")
        _require(side in headers and selected is None and failure is None and side not in pending,
                 "frame after selection or unmatched proposal")
        outcome = None
        if not headers[side]:
            _require(token == HEADER, "missing bilateral multistream header")
            headers[side] = True
        else:
            if token == HEADER:
                failure = "unexpected negotiation header or token"
            elif side == proposer:
                count += 1
                if count > 8 or not token.startswith("/"):
                    failure = "invalid or excessive proposal"
                else:
                    proposed = token
            else:
                _require(token == "na" or token.startswith("/"), "invalid acknowledgement")
            if failure is None:
                pending[side] = token
            if failure is None and len(pending) == 2:
                reply = pending["read" if proposer == "write" else "write"]
                if reply == "na":
                    outcome, proposed = "rejected", None
                    pending.clear()
                else:
                    if reply != pending[proposer]:
                        failure = "acknowledgement disagrees with proposal"
                    else:
                        selected, selected_sequence, outcome = reply, event["sequence"], "selected"
        _require(event.get("selection_outcome") == outcome, "selection annotation differs from captured frames")
        _require(event.get("protocol") == (selected if outcome == "selected" else None),
                 "protocol annotation differs from captured frames")
    _require(record["protocol"] == selected and record["proposed_protocol"] == proposed,
             "selection snapshot contradicts captured negotiation")
    if failure is not None:
        _require(record["parser_error"] == failure, "captured failure disagrees with parser error")
    elif record["parser_error"] is not None:
        # The collector does not emit a frame for an invalid partial prefix/body.
        # Retain such failed background attempts, never treating the label as a
        # successful transcript. A witnessed token failure must match exactly above.
        hidden = {"negotiation frame prefix exceeds bound", "invalid negotiation frame length",
                  "non UTF-8 negotiation frame", "invalid multistream token or header"}
        rejected_tail = (record["parser_error"] == "unmatched bytes after rejected proposal"
                         and frames and frames[-1].get("selection_outcome") == "rejected")
        _require(selected is None and (record["parser_error"] in hidden or rejected_tail),
                 "parser error lacks a consistent failed replay")
    return selected_sequence, frames[0]["sequence"] if frames else None


def _events(connection, streams):
    events = _list(connection.get("events"), 0, 256, "event")
    opened = {}
    allowed = ERROR_EVENTS | {"negotiation_frame", "delegate_started", "delegate_completed",
                              "authenticated_peer", "substream_opened", "muxer_address_change",
                              "upgrade_completed", "response_completion"}
    for sequence, event in enumerate(events, 1):
        _object(event, "event")
        _require(type(event.get("sequence")) is int and event["sequence"] == sequence,
                 "invalid event sequence")
        kind, phase, sid = event.get("kind"), event.get("phase"), event.get("stream_trace_id")
        _require(isinstance(kind, str) and kind in allowed and phase in {"security", "muxer", "application"}
                 and "stream_trace_id" in event, "unknown event shape")
        base = {"sequence", "kind", "phase", "stream_trace_id"}
        if kind == "negotiation_frame":
            required = base | {"direction", "frame_hex"}
            _require(required <= set(event) <= required | {"protocol", "selection_outcome"},
                     "invalid raw negotiation event fields")
        else:
            _require(set(event) == base | {"detail"}, "invalid structured event fields")
        if phase == "application":
            _require(integer(sid, 1, len(streams)), "event references missing raw stream")
        else:
            _require(sid is None, "connection event carries an application id")
        if kind == "substream_opened":
            _require(phase == "application" and sid == len(opened) + 1
                     and event.get("detail") == {"direction": streams[sid - 1]["direction"]},
                     "invalid raw stream opening order or direction")
            opened[sid] = sequence
        elif phase == "application":
            _require(sid in opened and opened[sid] < sequence, "event precedes raw stream opening")
        if kind in {"delegate_started", "delegate_completed"}:
            _require(phase != "application", "substream trait substituted for connection upgrade")
            _protocol(_object(event.get("detail"), "delegate detail").get("protocol"))
        if kind == "authenticated_peer":
            _require(phase == "security", "authentication outside security upgrade")
            _peer(_object(event.get("detail"), "authentication detail").get("peer_id"))
        if kind in ERROR_EVENTS:
            detail = _object(event.get("detail"), "error detail")
            if kind == "upgrade_future_dropped_before_completion":
                # UpgradeGuard drop is a separate lifecycle observation, not
                # Connection::failure, and the current collector emits {}.
                _require(phase != "application" and detail == {}, "invalid upgrade guard drop")
            else:
                _require(isinstance(detail.get("error"), str) and 1 <= len(detail["error"]) <= 256,
                         "invalid error evidence")
                upgrade_phase = "security" if phase == "security" else "muxer"
                completed = any(e["kind"] == "upgrade_completed" and e["phase"] == upgrade_phase
                                for e in events[:sequence - 1])
                _require(detail.get("failure_stage") == ("post_upgrade" if completed else "pre_upgrade"),
                         "failure stage contradicts completion sequence")
        if kind == "muxer_address_change":
            _require(phase == "muxer", "address change outside muxer")
            _tcp(_object(event.get("detail"), "address change").get("address"),
                 connection["authenticated_remote_peer_id"])
    _require(len(opened) == len(streams), "raw stream lacks opening event")
    return events, opened


def _milestones(connection):
    events = connection["events"]
    for index, record in enumerate(connection["negotiations"] + connection["streams"]):
        phase = ("security", "muxer")[index] if index < 2 else "application"
        sid = None if index < 2 else index - 1
        subset = [e for e in events if e["phase"] == phase and e["stream_trace_id"] == sid]
        selected, _ = _negotiation(subset, record)
        for kind, key in (("upgrade_completed", "upgrade_completed_sequence"),
                          ("response_completion", "response_completed_sequence")):
            marks = [e for e in subset if e["kind"] == kind]
            sequence = record[key]
            _require(len(marks) == (0 if sequence is None else 1), "missing or duplicate completion marker")
            if sequence is None:
                continue
            mark = marks[0]
            _require(mark["sequence"] == sequence and selected is not None and sequence > selected,
                     "completion marker precedes selection or differs from snapshot")
            detail = _object(mark.get("detail"), "completion detail")
            _require(detail.get("protocol") == record["protocol"], "completion protocol mismatch")
            if kind == "upgrade_completed":
                _require(index < 2 and record["response_completed_sequence"] is None,
                         "upgrade marker on an application stream")
                started, done = _unique(subset, "delegate_started"), _unique(subset, "delegate_completed")
                _require(started["sequence"] < done["sequence"] < sequence
                         and started["detail"]["protocol"] == done["detail"]["protocol"] == record["protocol"],
                         "completion marker lacks the selected successful delegate")
            else:
                _require(index >= 2 and record["upgrade_completed_sequence"] is None
                         and record["write_close_returned"] is True, "response marker lacks application close")
                write = _body(detail.get("write"))
                _require(write["complete_frames"] is True
                         and write["framed_bytes"] <= record["write"]["framed_bytes"],
                         "response marker lacks complete framed write")
                if write["framed_bytes"] == record["write"]["framed_bytes"]:
                    _require(write == record["write"], "response marker contradicts final framed write")
            # Markers are sticky: errors after them stay in history, but cannot
            # retroactively turn an earlier failed phase into a completed one.
            relevant = {"security"} if index == 0 else {"security", "muxer"}
            _require(not any(e["kind"] in ERROR_EVENTS and e["sequence"] < sequence
                             and (e["phase"] in relevant or sid is not None and e["stream_trace_id"] == sid)
                             for e in events), "completion marker follows a failed path")
        if index < 2:
            prefix = ("security", "muxer")[index]
            _require(connection[prefix + "_complete"] == (record["upgrade_completed_sequence"] is not None),
                     "sticky upgrade snapshot disagrees with marker")


def _connections(proof):
    connections = _list(proof.get("connections"), 1, 16, "connection")
    for cid, connection in enumerate(connections, 1):
        _object(connection, "connection")
        _require(type(connection.get("connection_trace_id")) is int and connection["connection_trace_id"] == cid,
                 "noncanonical connection trace id")
        _peer(connection.get("authenticated_local_peer_id"))
        peer = connection.get("authenticated_remote_peer_id")
        _require(isinstance(peer, str), "malformed authenticated peer")
        if peer:
            _peer(peer)
            _require(peer != connection["authenticated_local_peer_id"], "self-authentication is not a remote connection")
        _tcp(connection.get("local_address"), peer)
        _tcp(connection.get("remote_address"), peer)
        for key in ("security_complete", "muxer_complete", "security_delegate_completed", "muxer_delegate_completed",
                    "early_muxer_negotiation", "muxer_drop_observed", "muxer_close_returned", "overflow"):
            _require(type(connection.get(key)) is bool, "invalid connection flag " + key)
        _require(connection["overflow"] is False and connection["early_muxer_negotiation"] is False,
                 "overflow or unsupported early muxer claim")
        endpoint = _object(connection.get("endpoint"), "raw endpoint")
        _require(connection.get("direction") in {"outbound", "inbound"}
                 and endpoint.get("direction") == connection["direction"]
                 and endpoint.get("upgrade_role") in {"outbound", "inbound"}
                 and _tcp(endpoint.get("remote_address"), peer or None, True) == connection["remote_address"],
                 "raw endpoint contradicts connection")
        if connection["direction"] == "inbound":
            _require(_tcp(endpoint.get("local_address"), peer) == connection["local_address"], "local endpoint mismatch")
        selections = _list(connection.get("negotiations"), 2, 2, "connection negotiation")
        streams = _list(connection.get("streams"), 0, 32, "raw stream")
        for sid, stream in enumerate(streams, 1):
            _selection(stream)
            _require(type(stream.get("stream_trace_id")) is int and stream["stream_trace_id"] == sid,
                     "noncanonical raw stream id")
        events, _ = _events(connection, streams)
        for phase, record in zip(("security", "muxer"), selections):
            _selection(record)
            _require(record["direction"] == endpoint["upgrade_role"], "negotiation role override mismatch")
            _require(record["read"]["framed_bytes"] == record["write"]["framed_bytes"] == 0
                     and record["response_write_complete"] is False, "security/muxer application body claim")
            _negotiation([e for e in events if e["phase"] == phase], record)
            done = [e for e in events if e["phase"] == phase and e["kind"] == "delegate_completed"]
            _require(len(done) == int(connection[phase + "_delegate_completed"]),
                     "delegate snapshot disagrees with recorded completion")
            if done:
                started = _unique([e for e in events if e["phase"] == phase], "delegate_started")
                _require(started["sequence"] < done[0]["sequence"]
                         and started["detail"]["protocol"] == done[0]["detail"]["protocol"]
                         == connection.get("selected_" + phase), "delegate snapshot protocol mismatch")
            else:
                _require("selected_" + phase in connection and connection["selected_" + phase] is None,
                         "selected delegate without completion")
        authenticated = [e for e in events if e["kind"] == "authenticated_peer"]
        _require(len(authenticated) == (1 if peer else 0), "authentication snapshot lacks unique actual event")
        if authenticated:
            _require(authenticated[0]["detail"]["peer_id"] == peer
                     and connection["security_delegate_completed"] is True,
                     "authenticated peer differs from security event")
        for stream in streams:
            _negotiation([e for e in events if e["stream_trace_id"] == stream["stream_trace_id"]], stream)
            response = (stream["protocol"] is not None and stream["parser_error"] is None and not stream["io_failed"]
                        and stream["write_close_returned"] and stream["write"]["complete_frames"])
            _require(stream["response_write_complete"] == response, "contradictory local write completion")
        _milestones(connection)
    return connections


def _unique(events, kind):
    matches = [e for e in events if e["kind"] == kind]
    _require(len(matches) == 1, "missing or duplicate " + kind)
    return matches[0]


def _upgrades(connection, security, peer, last_required_response=None):
    events = connection["events"]
    completed = {}
    for index, (phase, protocol) in enumerate((("security", security), ("muxer", MUXER))):
        subset = [e for e in events if e["phase"] == phase]
        failures = [e for e in subset if e["kind"] in ERROR_EVENTS]
        if last_required_response is None:
            _require(not failures, "target connection upgrade failed")
        else:
            _require(integer(last_required_response, 1, 256)
                     and all(e["sequence"] > last_required_response
                             and e.get("detail", {}).get("failure_stage") == "post_upgrade"
                             for e in failures), "connection failed before required responses completed")
        record = connection["negotiations"][index]
        _require(record["protocol"] == protocol and record["parser_error"] is None
                 and record["io_failed"] == bool(failures) and record["drop_observed"] is True,
                 "target upgrade is failed or not destroyed")
        selected, first = _negotiation(subset, record)
        started, done = _unique(subset, "delegate_started"), _unique(subset, "delegate_completed")
        _require(started["detail"]["protocol"] == done["detail"]["protocol"] == protocol
                 and started["sequence"] < done["sequence"] and selected is not None,
                 "delegate protocol or order disagrees")
        _require(connection["selected_" + phase] == protocol and connection[phase + "_complete"] is True
                 and connection[phase + "_delegate_completed"] is True, "missing completed native delegate")
        marker = _unique(subset, "upgrade_completed")
        completed[phase] = (started["sequence"], done["sequence"], selected, first, marker["sequence"])
    auth = _unique([e for e in events if e["phase"] == "security"], "authenticated_peer")
    sec, mux = completed["security"], completed["muxer"]
    _require(auth["detail"]["peer_id"] == peer and sec[2] < sec[1] < sec[4] < auth["sequence"] < mux[0]
             and auth["sequence"] < mux[3], "security/authentication/muxer causal order mismatch")
    # V1Lazy may buffer even the proposal until delegate I/O. In particular, a
    # constructed Yamux muxer can precede both its wire ACK and outbound streams.
    for event in events:
        if event["kind"] == "substream_opened":
            _require(event["sequence"] > mux[1], "raw stream opened before Yamux delegate completion")
    return mux[4]


def _lifecycle(result, proof):
    lifecycle = _object(result.get("fixture_task_lifecycle"), "task lifecycle")
    _require(lifecycle.get("scope") == "public_swarm_executor_and_fixture_echo_handler"
             and lifecycle.get("shutdown_mode") == "close_admission_abort_join_after_swarm_drop"
             and lifecycle.get("fixture_owned_tasks_joined") is True
             and lifecycle.get("overflow") is False and lifecycle.get("errors") == [], "task cleanup failed or missing")
    tasks = _list(lifecycle.get("tasks"), 1, 256, "task")
    ids = set()
    for task in tasks:
        _object(task, "task")
        identifier = _decimal(task.get("task_id"))
        _require(identifier not in ids and task.get("kind") in {"swarm_connection", "echo_handler"}
                 and type(task.get("abort_requested")) is bool, "invalid owned task")
        ids.add(identifier)
        _require(task.get("terminal") == "completed" or
                 task.get("terminal") == "cancelled_by_owner" and task["abort_requested"] is True,
                 "task was not joined successfully")
    _require(any(t["kind"] == "swarm_connection" for t in tasks), "no owned connection task")
    _require(proof.get("fixture_owned_tasks_joined") is True, "observer predates task join")


def _prefix(size):
    data = bytearray()
    while size >= 128:
        data.append((size & 127) | 128)
        size >>= 7
    return bytes(data + bytes([size]))


def _identify(result, app, peer):
    raw = _object(result.get("raw_identify_exchange"), "separate raw Identify exchange")
    _require(raw.get("status") == "verified" and raw.get("error") is None
             and raw.get("basis") == "fixture_separate_authenticated_identify_exchange"
             and raw.get("protocol") == IDENTIFY and raw.get("raw_capture_truncated") is False
             and raw.get("authenticated_remote_peer_id") == peer,
             "failed or uncorrelated raw Identify capture")
    data = _hex(raw.get("raw_protobuf_hex"), 1, 4096)
    _require(integer(raw.get("raw_protobuf_bytes"), 1, 4096) and raw["raw_protobuf_bytes"] == len(data)
             and raw.get("raw_protobuf_sha256") == hashlib.sha256(data).hexdigest(), "raw Identify size/hash mismatch")
    framed = _prefix(len(data)) + data
    _require(app["read"]["framed_bytes"] == len(framed)
             and app["read"]["framed_sha256"] == hashlib.sha256(framed).hexdigest()
             and app["write"]["framed_bytes"] == 0, "Identify app framing differs from captured protobuf")
    _require(result.get("signed_peer_record") is False, "Behaviour Identify flag was replaced by separate exchange")


def validate_rust_dial_upgrade(result, expected_peer, security):
    """Return errors, without mutating a fixture result or trusting its binding label."""
    try:
        _validate(result, expected_peer, security)
    except (ValueError, TypeError, KeyError, UnicodeError, IndexError) as error:
        return ["Rust upgrade evidence: " + str(error)]
    return []


def validate_rust_listener_upgrade(forge_payload, listener, remote, security):
    """Validate paired fresh Forge/Rust receipts, not a fabricated outbound app.

    The caller owns both terminal-indexed process snapshots and semantic Identify
    validation. Rust's local response completion is not a remote delivery ACK;
    the independent Forge receipt supplies the corresponding successful read.
    """
    try:
        _validate_listener(forge_payload, listener, remote, security)
    except (ValueError, TypeError, KeyError, UnicodeError, IndexError) as error:
        return ["paired Forge/Rust upgrade evidence: " + str(error)]
    return []


def _inbound_response(connection, protocol):
    def proposed(stream):
        if protocol in (stream["protocol"], stream["proposed_protocol"]):
            return True
        # A rejected proposal clears the snapshot's proposal. It still belongs
        # to history and must not disappear from exchange uniqueness.
        return any(e["kind"] == "negotiation_frame" and e["stream_trace_id"] == stream["stream_trace_id"]
                   and e["direction"] == "read" and wire_token(e["frame_hex"]) == protocol
                   for e in connection["events"])

    matches = [s for s in connection["streams"] if s["direction"] == "inbound" and proposed(s)]
    _require(len(matches) == 1, "missing or ambiguous inbound response, including failed history")
    stream = matches[0]
    _require(stream["protocol"] == protocol and stream["parser_error"] is None and stream["io_failed"] is False
             and stream["drop_observed"] is True and stream["write_close_returned"] is True
             and stream["response_write_complete"] is True, "inbound response failed or has not closed/dropped")
    events = [e for e in connection["events"] if e["stream_trace_id"] == stream["stream_trace_id"]]
    _require(not any(e["kind"] in ERROR_EVENTS for e in events), "inbound response has failed I/O")
    selected, _ = _negotiation(events, stream)
    marker = _unique(events, "response_completion")
    mux = connection["negotiations"][1]["upgrade_completed_sequence"]
    _require(integer(mux, 1, 256) and selected is not None and mux < selected < marker["sequence"]
             and marker["sequence"] == stream["response_completed_sequence"]
             and marker["detail"]["protocol"] == protocol
             and marker["detail"]["write"] == stream["write"], "response is not bound to its completed upgrade/write")
    write = stream["write"]
    _require(write["complete_frames"] is True and write["frames"] == 1
             and write["invalid_or_over_limit"] is False
             and integer(write["framed_bytes"], 2, 4098 if protocol == IDENTIFY else 4096),
             "response is not one bounded complete frame")
    return stream


def _validate_listener(payload, listener, remote, security):
    _object(payload, "Forge payload")
    _object(listener, "Rust listener")
    _peer(remote)
    _require(security in {"/noise", "/tls/1.0.0"}, "unsupported native TCP security")
    _require(listener.get("implementation") == "rust" and listener.get("role") == "listener"
             and listener.get("status") == "ok" and listener.get("scenario") == payload.get("scenario")
             and payload.get("implementation") == "forge" and payload.get("role") == "dialer"
             and payload.get("status") == "ok" and payload.get("scenario") in {"identify", "echo"}
             and payload.get("single_fresh_connection_retained") is True
             and payload.get("identify_event_basis") == "automatic_identify_single_fresh_connection"
             and integer(payload.get("application_connection_id"), 1, 2**64 - 1)
             and type(payload.get("identify_event_connection_id")) is int
             and payload["application_connection_id"] == payload["identify_event_connection_id"]
             and payload.get("signed_peer_record") is True and integer(payload.get("protocol_count"), 1, 128)
             and payload.get("negotiated_transport") == "tcp"
             and payload.get("authenticated_remote_peer_id") == remote, "missing fresh authenticated Forge receipt")
    local = payload.get("local_peer_id")
    _peer(local)
    _require(local != remote, "client identity is not distinct")
    if "local_peer_id" in listener:
        _require(listener["local_peer_id"] == remote, "listener identity contradicts receipt")
    proof = _object(listener.get("upgrade_observation"), "listener observation")
    _require(proof.get("source") == "rust-libp2p.public-connection-upgrades.v1"
             and proof.get("overflow") is False and proof.get("complete") is False
             and proof.get("binding_basis") == BASIS and proof.get("finalized_after_swarm_drop") is True
             and proof.get("exact_application_binding_supported") is True
             and proof.get("swarm_connection_binding_supported") is True,
             "missing finalized listener collector or fabricated outbound completion")
    applications = _object(proof.get("applications"), "listener applications")
    _require(applications.get("source") == "actual_swarm_stream_framed_io"
             and applications.get("overflow") is False and applications.get("attempts") == []
             and "application_pair_binding" not in proof and "application_pair_binding_error" not in proof,
             "listener has unexpected outbound application claims")
    _lifecycle(listener, proof)
    connections = _connections(proof)
    matches = [c for c in connections if c["authenticated_remote_peer_id"] == local]
    _require(len(matches) == 1, "missing or ambiguous authenticated inbound connection")
    connection = matches[0]
    _require(connection["authenticated_local_peer_id"] == remote
             and connection["direction"] == connection["endpoint"]["upgrade_role"] == "inbound"
             and connection["muxer_drop_observed"] is True
             and _tcp(payload.get("connection_remote_addr"), remote) == connection["local_address"],
             "paired connection identity/direction/endpoint disagrees")
    if "connection_local_addr" in payload:
        _require(_tcp(payload["connection_local_addr"], local) == connection["remote_address"],
                 "Forge local endpoint disagrees with counterpart")
    swarm = [e for e in _swarm_events(proof) if e["authenticated_remote_peer_id"] == local]
    _require(len(swarm) == 1 and swarm[0]["endpoint"] == connection["endpoint"],
             "missing or ambiguous matching inbound Swarm endpoint")
    identify = _inbound_response(connection, IDENTIFY)
    _require(identify["read"]["framed_bytes"] == 0, "Identify response has unexpected inbound body")
    responses = [identify]
    if payload["scenario"] == "echo":
        echo = _inbound_response(connection, ECHO)
        responses.append(echo)
        size = echo_frame_size(payload.get("payload_bytes"))
        _require(payload.get("echo_ok") is True and payload.get("application_close_returned") is True
                 and integer(payload.get("application_stream_id"), 0, 2**63 - 1)
                 and payload.get("application_protocol") == payload.get("protocol") == ECHO
                 and payload.get("negotiated_security") == security and payload.get("negotiated_muxer") == MUXER
                 and echo["read"] == echo["write"] and echo["read"]["framed_bytes"] == size,
                 "echo lacks matching actual frames, payload prefix, security or close")
        for side, source in (("request", "read"), ("response", "write")):
            _require(integer(payload.get(f"application_{side}_framed_bytes"), 1, 4096)
                     and payload[f"application_{side}_framed_bytes"] == echo[source]["framed_bytes"]
                     and payload.get(f"application_{side}_framed_sha256") == echo[source]["framed_sha256"],
                     "Forge/Rust echo count/hash differs")
    for key, expected in (("negotiated_security", security), ("negotiated_muxer", MUXER)):
        if key in payload:
            _require(payload[key] == expected, "Forge upgrade metadata contradicts actual delegate")
    _upgrades(connection, security, local, max(s["response_completed_sequence"] for s in responses))


def _swarm_events(proof):
    events = _list(proof.get("swarm_events"), 1, 16, "Swarm event")
    ids = set()
    for event in events:
        _object(event, "Swarm event")
        identifier = _decimal(event.get("swarm_connection_id"))
        _peer(event.get("authenticated_remote_peer_id"))
        _require(event.get("kind") == "connection_established" and identifier not in ids,
                 "duplicate or invalid Swarm event")
        point = _object(event.get("endpoint"), "Swarm endpoint")
        _require(point.get("direction") in {"inbound", "outbound"}
                 and point.get("upgrade_role") in {"inbound", "outbound"}, "invalid Swarm endpoint role")
        _tcp(point.get("remote_address"), event["authenticated_remote_peer_id"], True)
        if point["direction"] == "inbound":
            _tcp(point.get("local_address"), event["authenticated_remote_peer_id"])
        ids.add(identifier)
    return events


def _validate(result, peer, security):
    _peer(peer)
    _require(security in {"/noise", "/tls/1.0.0"}, "unsupported native TCP security")
    _object(result, "result")
    _require(result.get("implementation") == "rust" and result.get("role") == "dialer"
             and result.get("status") == "ok" and result.get("scenario") in {"identify", "echo"},
             "not a successful scoped Rust dial result")
    proof = _object(result.get("upgrade_observation"), "upgrade observation")
    _require(proof.get("source") == "rust-libp2p.public-connection-upgrades.v1"
             and proof.get("binding_basis") == BASIS and proof.get("overflow") is False,
             "wrong or overflowed observer")
    for flag in ("complete", "exact_application_binding_supported", "swarm_connection_binding_supported", "finalized_after_swarm_drop"):
        _require(proof.get(flag) is True, "missing " + flag)
    _lifecycle(result, proof)
    connections = _connections(proof)
    candidates = [c for c in connections if c["authenticated_remote_peer_id"] == peer]
    _require(len(candidates) == 1, "absent or ambiguous authenticated connection, including failed history")
    connection = candidates[0]
    _require(connection["direction"] == connection["endpoint"]["upgrade_role"] == "outbound"
             and connection["muxer_drop_observed"] is True, "wrong target direction or unterminated muxer")
    mux_ack = _upgrades(connection, security, peer)
    for key, expected in (("authenticated_remote_peer_id", peer), ("negotiated_transport", "tcp"),
                          ("negotiated_security", security), ("negotiated_muxer", MUXER)):
        if result.get(key) is not None:
            _require(result[key] == expected, "top-level connection metadata contradicts observation")
    swarm_events = _swarm_events(proof)
    swarm = [e for e in swarm_events if e["authenticated_remote_peer_id"] == peer]
    _require(len(swarm) == 1, "absent or ambiguous authenticated Swarm event")
    endpoint = _object(swarm[0].get("endpoint"), "Swarm endpoint")
    _require(endpoint.get("direction") == endpoint.get("upgrade_role") == "outbound"
             and _tcp(endpoint.get("remote_address"), peer, True) == connection["remote_address"], "Swarm endpoint mismatch")
    applications = _object(proof.get("applications"), "applications")
    _require(applications.get("source") == "actual_swarm_stream_framed_io"
             and applications.get("overflow") is False, "wrong application collector")
    protocols = [IDENTIFY] if result["scenario"] == "identify" else [IDENTIFY, ECHO]
    attempts = _list(applications.get("attempts"), len(protocols), len(protocols), "scenario application")
    streams = []
    for index, (app, protocol) in enumerate(zip(attempts, protocols), 1):
        streams.append(_application(app, index, protocol, connections, connection, swarm[0], mux_ack, peer))
    _identify(result, attempts[0], peer)
    if result["scenario"] == "echo":
        app, stream = attempts[1], streams[1]
        boundary = {entry["connection_trace_id"]: entry["stream_count"] for entry in app["preexisting_raw_streams"]}
        _require(boundary.get(connection["connection_trace_id"], 0) >= streams[0]["stream_trace_id"],
                 "echo began before the explicit Identify raw stream existed")
        size = echo_frame_size(result.get("payload_bytes"))
        _require(result.get("echo_ok") is True and result.get("protocol") == ECHO
                 and app["read"] == app["write"] and app["read"]["framed_bytes"] == size
                 and app["write_close_returned"] is True and stream["write_close_returned"] is True,
                 "echo frame size/hash or local close mismatch")
        pair = _object(proof.get("application_pair_binding"), "Identify/echo pair binding")
        expected = {"basis": BASIS, "connection_trace_id": connection["connection_trace_id"],
                    "swarm_connection_id": swarm[0]["swarm_connection_id"],
                    "identify_stream_trace_id": streams[0]["stream_trace_id"],
                    "echo_stream_trace_id": stream["stream_trace_id"]}
        _require(all(type(pair.get(key)) is int for key in
                     ("connection_trace_id", "identify_stream_trace_id", "echo_stream_trace_id"))
                 and pair == expected and "application_pair_binding_error" not in proof,
                 "pair receipt differs from independently recomputed bindings")
    else:
        _require("application_pair_binding" not in proof and "application_pair_binding_error" not in proof,
                 "unexpected pair receipt for Identify-only exchange")


def _application(app, index, protocol, connections, connection, swarm, mux_ack, peer):
    _object(app, "application attempt")
    _require(type(app.get("application_trace_id")) is int and app["application_trace_id"] == index
             and app.get("authenticated_remote_peer_id") == peer and app.get("protocol") == protocol
             and app.get("direction") == "outbound" and app.get("errors") == []
             and "binding_error" not in app and app.get("overflow") is False
             and app.get("remote_receipt_claimed") is False, "invalid or failed application attempt")
    for flag in ("opened", "application_io_complete", "attempt_ended", "stream_drop_returned"):
        _require(app.get(flag) is True, "application has not completed/dropped")
    for flag in ("read_eof", "write_close_returned"):
        _require(type(app.get(flag)) is bool, "malformed application I/O flag")
    for side in ("read", "write"):
        _body(app.get(side))
        _require(app[side]["invalid_or_over_limit"] is False, "invalid application body")
    _require(app["read"]["complete_frames"] is True and app["read"]["frames"] == 1, "missing complete application response")
    previous = _list(app.get("preexisting_raw_streams"), 0, len(connections), "causal boundary")
    before = {}
    for cid, entry in enumerate(previous, 1):
        _object(entry, "causal boundary")
        _require(type(entry.get("connection_trace_id")) is int and entry["connection_trace_id"] == cid
                 and integer(entry.get("stream_count"), 0, len(connections[cid - 1]["streams"])),
                 "invalid preexisting raw stream prefix")
        before[cid] = entry["stream_count"]
    matches = [s for s in connection["streams"] if s["stream_trace_id"] > before.get(connection["connection_trace_id"], 0)
               and s["direction"] == app["direction"] and protocol in (s["protocol"], s["proposed_protocol"])
               and s["read"] == app["read"] and s["write"] == app["write"]]
    _require(len(matches) == 1, "absent or ambiguous raw framed I/O candidates, including failed history")
    stream = matches[0]
    _require(stream["protocol"] == protocol and stream["parser_error"] is None
             and stream["io_failed"] is False and stream["drop_observed"] is True,
             "matched raw stream failed or is not destroyed")
    subset = [e for e in connection["events"] if e["stream_trace_id"] == stream["stream_trace_id"]]
    _require(not any(e["kind"] in ERROR_EVENTS for e in subset), "matched raw stream has I/O errors")
    selected, _ = _negotiation(subset, stream)
    _require(selected is not None and selected > mux_ack, "application ACK precedes confirmed muxer")
    binding = _object(app.get("binding"), "binding")
    _require(type(binding.get("connection_trace_id")) is int and type(binding.get("stream_trace_id")) is int,
             "malformed binding trace id")
    expected_binding = {"basis": BASIS, "connection_trace_id": connection["connection_trace_id"],
                        "stream_trace_id": stream["stream_trace_id"], "swarm_connection_id": swarm["swarm_connection_id"],
                        "authenticated_local_peer_id": connection["authenticated_local_peer_id"],
                        "authenticated_remote_peer_id": peer, "local_address": connection["local_address"],
                        "remote_address": connection["remote_address"], "remote_receipt_claimed": False}
    _require(binding == expected_binding and binding.get("remote_receipt_claimed") is False,
             "declared binding differs from recomputed unique match")
    if protocol == ECHO:
        response = _unique(subset, "response_completion")
        _require(response["detail"]["write"] == app["write"]
                 and response["sequence"] > mux_ack and stream["response_write_complete"] is True,
                 "echo completion marker differs from exact application write")
    else:
        _require(stream["response_completed_sequence"] is None,
                 "Identify read was relabeled as local response write")
    return stream
