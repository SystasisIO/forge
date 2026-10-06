"""Synthetic semantic regressions only; these are NOT live acceptance evidence."""

import copy
import hashlib
import json
import unittest

from autorelay_wire import varint
from private_profile_evidence import (
    CONTRACTS, PRIVATE_CONTRACTS, INLINE_CONTRACTS, ECHO, MUXER, PAYLOAD, UNKNOWN,
    framed_receipt, validate_private_profile, tcp_address, logical_tcp_address, donor_observation,
)
from test_rust_upgrade_evidence import (
    receipt as rust_receipt, listener_pair as rust_listener_pair,
    raw_stream, renumber, event, LOCAL as OLD_LOCAL, REMOTE as OLD_REMOTE,
)


def base58(raw):
    number, result = int.from_bytes(raw, "big"), ""
    while number:
        number, digit = divmod(number, 58)
        result = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"[digit] + result
    return "1" * (len(raw) - len(raw.lstrip(b"\0"))) + result


LOCAL_KEY = b"\x08\x01\x12\x20" + bytes(range(32))
REMOTE_KEY = b"\x08\x01\x12\x20" + bytes(range(32, 64))
LOCAL = base58(b"\0" + varint(len(LOCAL_KEY)) + LOCAL_KEY)
REMOTE = base58(b"\0" + varint(len(REMOTE_KEY)) + REMOTE_KEY)
LOCAL_ADDR = "/ip4/127.0.0.1/tcp/41000"
REMOTE_ADDR = "/ip4/127.0.0.1/tcp/42000"


def field(key, data):
    return varint(key << 3 | 2) + varint(len(data)) + data


def signed(key):
    peer = b"\0" + varint(len(key)) + key
    address = b"\x04\x7f\0\0\x01\x06\xa4\x10"
    record = field(1, peer) + b"\x10\x01" + field(3, field(1, address))
    return field(1, key) + field(2, b"\x03\x01") + field(3, record) + field(5, b"s" * 64)


def body(data):
    return {"framed_bytes": len(data), "framed_sha256": hashlib.sha256(data).hexdigest(),
            "frames": int(bool(data)), "complete_frames": bool(data), "invalid_or_over_limit": False}


def public_receipt(payload, raw=False):
    data = payload if raw else varint(len(payload)) + payload
    return {"framed_hex": data.hex(), "raw": raw, "read": body(data)}


def rust_stream(connection, protocol, sent, received, outbound):
    stream = raw_stream(connection, protocol, read=None if received is None else b"response",
                        write=None if sent is None else b"request")
    stream["read"], stream["write"] = body(received or b""), body(sent or b"")
    stream["direction"] = "outbound" if outbound else "inbound"
    for item in connection["events"]:
        if item["stream_trace_id"] != stream["stream_trace_id"]:
            continue
        if item["kind"] == "substream_opened":
            item["detail"]["direction"] = stream["direction"]
        if item["kind"] == "negotiation_frame" and not outbound:
            item["direction"] = "write" if item["direction"] == "read" else "read"
        if item["kind"] == "response_completion":
            item["detail"]["write"] = copy.deepcopy(stream["write"])
    return stream


def rust_barrier(connection):
    stream = rust_stream(connection, UNKNOWN, None, None, True)
    stream.update(protocol=None, proposed_protocol=None, negotiation_complete_frames=True)
    ack = connection["events"][-1]
    ack.update(frame_hex=(varint(3) + b"na\n").hex(), selection_outcome="rejected")
    del ack["protocol"]
    receipt = {"source": "rust-libp2p.observed-multistream-rejection.v1",
               "connection_trace_id": connection["connection_trace_id"], "stream_trace_id": stream["stream_trace_id"],
               "direction": "outbound", "protocol": UNKNOWN, "native_error": "unsupported_protocol"}
    refresh_barrier(connection, receipt)
    return receipt


def refresh_barrier(connection, receipt):
    """Only renumber existing synthetic raw events; never claim executed I/O or crypto."""
    scoped = [e for e in connection["events"] if e.get("stream_trace_id") == receipt["stream_trace_id"]]
    receipt["opened_sequence"] = next(e["sequence"] for e in scoped if e["kind"] == "substream_opened")
    receipt["rejected_sequence"] = next(e["sequence"] for e in scoped if e.get("selection_outcome") == "rejected")
    for side in ("read", "write"):
        receipt[side + "_frames"] = [{"sequence": e["sequence"], "framed_hex": e["frame_hex"]}
                                    for e in scoped if e["kind"] == "negotiation_frame" and e["direction"] == side]


def go_event(connection, kind, sid, **fields):
    connection["events"].append({"sequence": len(connection["events"]) + 1, "kind": kind,
                                 "stream_trace_id": sid, **fields})


def go_frames(connection, sid, outbound, protocol, rejected=False):
    proposer, reply = ("write", "read") if outbound else ("read", "write")
    for side, token in ((proposer, "/multistream/1.0.0"), (proposer, protocol),
                        (reply, "/multistream/1.0.0"), (reply, "na" if rejected else protocol)):
        wire = varint(len(token) + 1) + token.encode() + b"\n"
        go_event(connection, "multistream_frame", sid, phase="application_multistream", direction=side,
                 protocol=token, frame_hex=wire.hex())


def go_stream(connection, protocol, sent, received, outbound, binding=False):
    sid = len(connection["streams"]) + 1
    direction = "Outbound" if outbound else "Inbound"
    stream = {"stream_trace_id": sid, "protocol": protocol, "direction": direction, "failed": False, "reset": False,
              "read_frames": int(bool(received)), "write_frames": int(bool(sent)),
              "read_framed_bytes": len(received or b""), "read_framed_sha256": hashlib.sha256(received or b"").hexdigest(),
              "write_framed_bytes": len(sent or b""), "write_framed_sha256": hashlib.sha256(sent or b"").hexdigest(),
              "application_io_complete": outbound and binding, "response_write_complete": not outbound}
    connection["streams"].append(stream)
    go_event(connection, "stream_open", sid, direction=direction)
    go_frames(connection, sid, outbound, protocol)
    go_event(connection, "protocol_selected", sid, phase="application_multistream", protocol=protocol, direction=direction)
    if binding:
        stream["network_stream_id"] = "actual-stream"
        for _ in range(2):
            go_event(connection, "application_stream_binding", sid, protocol=protocol, direction=direction,
                     network_connection_id="actual-connection", network_stream_id="actual-stream")
    if not outbound or binding:
        go_event(connection, "application_io_complete" if outbound else "response_write_complete", sid,
                 direction=direction, protocol=protocol, **({"network_stream_id": "actual-stream"} if binding else {}))
    return stream


def go_barrier(connection, outbound=False):
    sid = len(connection["streams"]) + 1
    direction = "Outbound" if outbound else "Inbound"
    connection["streams"].append({"stream_trace_id": sid, "direction": direction, "protocol": "",
        "application_io_complete": False, "response_write_complete": False, "failed": True, "reset": True,
        "read_frames": 0, "write_frames": 0, "read_framed_bytes": 0, "write_framed_bytes": 0})
    go_event(connection, "stream_open", sid, direction=direction)
    go_frames(connection, sid, outbound, UNKNOWN, rejected=True)
    go_event(connection, "protocol_rejected", sid, phase="application_multistream", protocol=UNKNOWN)
    go_event(connection, "negotiation_incomplete", sid, phase="application_multistream",
             error="stream ended before complete negotiation")
    go_event(connection, "stream_reset_returned", sid)


def renumber_native(connection, receipt=None):
    if "negotiations" in connection:
        renumber(connection)
    else:
        for sequence, item in enumerate(connection["events"], 1):
            item["sequence"] = sequence
    if receipt is not None:
        refresh_barrier(connection, receipt)


def semantic_fixture(name):
    protocol, security = CONTRACTS[name]
    rust = name.startswith("inline_muxer_rust_") or name == "rendezvous_rust_private_tcp_yamux_pnet"
    source, target = ("rust", "forge") if rust else ("forge", "go")
    private = name in PRIVATE_CONTRACTS or name.endswith("_private_pnet")
    request, response = PAYLOAD, PAYLOAD
    flags = {}
    if protocol == "/ipfs/ping/1.0.0":
        request = response = bytes(range(1, 33))
    elif protocol == "/ipfs/id/1.0.0":
        request = None
        response = field(1, REMOTE_KEY) + field(3, b"/ipfs/ping/1.0.0") + field(3, ECHO.encode()) + field(8, signed(REMOTE_KEY))
        flags["identify_verified"] = True
    elif protocol == "/ipfs/kad/1.0.0":
        request = b"\x08\x04" + field(2, b"\0" + varint(len(REMOTE_KEY)) + REMOTE_KEY)
        response = b"\x08\x04"
    elif protocol == "/rendezvous/1.0.0":
        request = b"\x08\x03" + field(5, field(1, b"forge.discovery") + b"\x10\x0a")
        registration = field(1, b"forge.discovery") + field(2, signed(LOCAL_KEY)) + b"\x18" + varint(7200)
        response = b"\x08\x04" + field(6, field(1, registration) + field(2, b"cookie") + b"\x18\0")
        flags["rendezvous_verified"] = True
    if name == "multistream_select_private_pnet":
        flags["unknown_protocol_rejected"] = True
    raw = protocol == "/ipfs/ping/1.0.0"
    result = {"implementation": source, "role": "dialer", "status": "ok", "scenario": name,
              "local_peer_id": LOCAL, "protocol": protocol, "stream_closed": True, "response": public_receipt(response, raw), **flags}
    if request is not None:
        result["request"] = public_receipt(request, raw)
    early = name.startswith("inline_muxer_go_")
    result["connection_receipt"] = {"source": "rust-libp2p.completed-public-upgrades" if rust else "forge.authenticated-stream-retained-session",
        "connection_id": 1, "local_peer_id": LOCAL, "remote_peer_id": REMOTE, "local_address": LOCAL_ADDR, "remote_address": REMOTE_ADDR,
        "security": security, "muxer": MUXER, "transport": "tcp", "early_muxer_negotiation": early}
    listener = {"implementation": target, "role": "listener", "status": "ok", "scenario": name, "local_peer_id": REMOTE}
    if private:
        result["pnet_fingerprint"] = listener["pnet_fingerprint"] = "a" * 64
    sent = b"" if request is None else bytes.fromhex(result["request"]["framed_hex"])
    received = bytes.fromhex(result["response"]["framed_hex"])
    if rust:
        fixture = json.loads(json.dumps(rust_receipt("echo", security)).replace(OLD_LOCAL, LOCAL).replace(OLD_REMOTE, REMOTE))
        proof = fixture["upgrade_observation"]
        c = proof["connections"][0]
        c["streams"] = []
        c["events"] = [e for e in c["events"] if e["phase"] != "application"]
        if protocol == "/rendezvous/1.0.0":
            rust_stream(c, protocol, sent, received, True)
            result["protocol_barrier"] = rust_barrier(c)
        rust_stream(c, protocol, sent if request is not None else None, received, True)
        result["upgrade_observation"], result["fixture_task_lifecycle"] = proof, fixture["fixture_task_lifecycle"]
        listener["connection_receipt"] = {"source":"forge.authenticated-stream-retained-session", "connection_id":2,
            "local_peer_id":REMOTE, "remote_peer_id":LOCAL, "local_address":REMOTE_ADDR, "remote_address":LOCAL_ADDR, "security":security,
            "muxer":MUXER, "transport":"tcp", "early_muxer_negotiation":False}
        listener["probe"] = public_receipt(b"private-receipt")
    else:
        c = {"connection_trace_id":1, "authenticated_local_peer_id":REMOTE, "authenticated_remote_peer_id":LOCAL,
             "local_address":REMOTE_ADDR, "remote_address":LOCAL_ADDR, "selected_security":security, "selected_muxer":MUXER,
             "direction":"Inbound", "early_muxer_negotiation":early, "failed":False, "events":[], "streams":[]}
        if protocol == "/ipfs/id/1.0.0":
            go_stream(c, protocol, received, sent, False)
            go_barrier(c)
        go_stream(c, protocol, received, sent, False)
        if name == "multistream_select_private_pnet":
            go_barrier(c)
        listener["private_observation"] = {"source":"go-libp2p.capable-conn.v1", "overflow":False,
            "finalized_after_host_close":True, "connections":[c]}
    record = {"dialer":source, "listener":target, "acceptance_scenario_id":name, "peer_id":REMOTE,
              "addr":REMOTE_ADDR + "/p2p/" + REMOTE}
    if private:
        sources = {"forge":"forge.node.metrics", "go":"go-libp2p.connection-gater", "rust":"rust-libp2p.swarm-events"}
        for kind in ("missing_key", "mismatched_key"):
            common = {"status":"rejected", "control_kind":kind, "correlation_token":name + "-" + kind,
                "attempted_connections":1, "established_connections":0, "identify_streams":0, "application_streams":0,
                "rejected_before_identify":True}
            record[kind] = {"result":common | {"implementation":source, "counter_source":sources[source],
                "expected_peer_id":REMOTE, "attempts":[{}]}, "listener_result":common | {"implementation":target,
                "counter_source":sources[target]}, "listener_process":{"peer_id":REMOTE}}
    return result, record, listener


def direction_fixture(name, implementation, outbound):
    """Every bilateral direction is synthetic; no donor process is run here."""
    result, record, listener = semantic_fixture(name)
    protocol, security = CONTRACTS[name]
    source, target = (implementation, "forge") if outbound else ("forge", implementation)
    early = name.startswith("inline_muxer_go_")
    result.pop("upgrade_observation", None); result.pop("fixture_task_lifecycle", None); result.pop("protocol_barrier", None)
    result["implementation"], listener["implementation"] = source, target
    listener.pop("private_observation", None); listener.pop("connection_receipt", None); listener.pop("probe", None)
    local, remote = (LOCAL, REMOTE) if outbound else (REMOTE, LOCAL)
    local_addr, remote_addr = (LOCAL_ADDR, REMOTE_ADDR) if outbound else (REMOTE_ADDR, LOCAL_ADDR)
    request = None if "request" not in result else bytes.fromhex(result["request"]["framed_hex"])
    response = bytes.fromhex(result["response"]["framed_hex"])
    sent, received = (request, response) if outbound else (response, request)
    donor = result if outbound else listener
    if implementation == "rust":
        fixture = rust_receipt("echo", security) if outbound else rust_listener_pair("echo", security)[1]
        encoded = json.dumps(fixture)
        for old, new in ((OLD_LOCAL, local), (OLD_REMOTE, remote),
                         ("/ip4/127.0.0.1/tcp/41000", "SOCKET_LOCAL"),
                         ("/ip4/127.0.0.1/tcp/42000", "SOCKET_REMOTE")):
            encoded = encoded.replace(old, new)
        fixture = json.loads(encoded.replace("SOCKET_LOCAL", local_addr).replace("SOCKET_REMOTE", remote_addr))
        proof = fixture["upgrade_observation"]; c = proof["connections"][0]
        c["streams"] = []; c["events"] = [e for e in c["events"] if e["phase"] != "application"]
        if outbound and protocol in {"/ipfs/id/1.0.0", "/rendezvous/1.0.0"}:
            rust_stream(c, protocol, sent, received, True)
            donor["protocol_barrier"] = rust_barrier(c)
        rust_stream(c, protocol, sent, received, outbound)
        if name == "multistream_select_private_pnet":
            unknown = rust_stream(c, "/forge/interop/private-unknown/1", None, None, outbound)
            unknown["protocol"] = unknown["proposed_protocol"] = None
            ack = c["events"][-1]
            ack["frame_hex"] = (varint(3) + b"na\n").hex()
            ack["selection_outcome"] = "rejected"
            del ack["protocol"]
        renumber(c)
        donor["upgrade_observation"], donor["fixture_task_lifecycle"] = proof, fixture["fixture_task_lifecycle"]
    else:
        direction = "Outbound" if outbound else "Inbound"
        c = {"connection_trace_id":1, "authenticated_local_peer_id":local, "authenticated_remote_peer_id":remote,
             "local_address":local_addr, "remote_address":remote_addr, "selected_security":security, "selected_muxer":MUXER,
             "direction":direction, "early_muxer_negotiation":early, "failed":False, "events":[], "streams":[]}
        if not outbound and protocol == "/ipfs/id/1.0.0":
            go_stream(c, protocol, sent, received, False)
            go_barrier(c)
        stream = go_stream(c, protocol, sent, received, outbound, binding=outbound)
        if outbound:
            result["stream_id"] = stream["network_stream_id"]
        if name == "multistream_select_private_pnet":
            go_barrier(c, outbound)
        donor["private_observation"] = {"source":"go-libp2p.capable-conn.v1", "overflow":False,
                                        "finalized_after_host_close":True, "connections":[c]}
    receipt = result["connection_receipt"]
    receipt["source"] = {"go":"go-libp2p.network.Conn.ConnState", "rust":"rust-libp2p.completed-public-upgrades"}[source] if outbound else "forge.authenticated-stream-retained-session"
    if outbound:
        receipt["connection_id"] = "actual-connection" if implementation == "go" else 1
        listener["connection_receipt"] = {"source":"forge.authenticated-stream-retained-session", "connection_id":2,
            "local_peer_id":REMOTE, "remote_peer_id":LOCAL, "local_address":REMOTE_ADDR, "remote_address":LOCAL_ADDR,
            "security":security, "muxer":MUXER, "transport":"tcp", "early_muxer_negotiation":early}
        listener["probe"] = public_receipt(b"private-receipt")
    record["dialer"], record["listener"] = source, target
    sources = {"forge":"forge.node.metrics", "go":"go-libp2p.connection-gater", "rust":"rust-libp2p.swarm-events"}
    for kind in ("missing_key", "mismatched_key"):
        if kind in record:
            record[kind]["result"].update(implementation=source, counter_source=sources[source])
            record[kind]["listener_result"].update(implementation=target, counter_source=sources[target])
    return result, record, listener


BARRIER_CASES = (("identify_private_tcp_yamux_pnet", "rust", True),
                 ("rendezvous_rust_private_tcp_yamux_pnet", "rust", True),
                 ("identify_private_tcp_yamux_pnet", "go", False))


def binding_view(result, record, listener):
    """Exercise raw I/O binding only, not synthetic signature verification flags."""
    outbound = record["dialer"] != "forge"
    implementation = record["dialer"] if outbound else record["listener"]
    donor = result if outbound else listener
    protocol, security = CONTRACTS[record["acceptance_scenario_id"]]
    request = bytes.fromhex(result["request"]["framed_hex"]) if "request" in result else None
    response = bytes.fromhex(result["response"]["framed_hex"])
    local, remote = (LOCAL, REMOTE) if outbound else (REMOTE, LOCAL)
    return donor_observation(donor, implementation, local, remote, security, protocol, request, response,
                             "outbound" if outbound else "inbound")


def barrier_parts(result, listener, implementation, outbound):
    donor = result if outbound else listener
    c = donor["upgrade_observation" if implementation == "rust" else "private_observation"]["connections"][0]
    # The synthetic construction explicitly opens old=1, UNKNOWN=2, requested=3.
    unknown = next(s for s in c["streams"] if s["stream_trace_id"] == 2)
    requested = next(s for s in c["streams"] if s["stream_trace_id"] == 3)
    return donor, c, unknown, requested


class PrivateProfileEvidenceTests(unittest.TestCase):
    def test_native_barrier_selects_only_the_post_na_exchange_not_first_or_last_reply(self):
        for name, implementation, outbound in BARRIER_CASES:
            with self.subTest(name=name, implementation=implementation):
                result, record, listener = direction_fixture(name, implementation, outbound)
                donor, c, _, requested = barrier_parts(result, listener, implementation, outbound)
                self.assertIs(binding_view(result, record, listener), c)
                self.assertEqual(validate_private_profile(result, record, listener), [])
                # A later, different observed packet must not become the target.
                # These tests make no claim about the packet's crypto validity.
                sent = bytes.fromhex(result["request"]["framed_hex"]) if outbound and "request" in result else None
                if not outbound:
                    go_stream(c, requested["protocol"], bytes.fromhex(public_receipt(b"another response")["framed_hex"]), None, False)
                else:
                    rust_stream(c, requested["protocol"], sent,
                                bytes.fromhex(public_receipt(b"another response")["framed_hex"]), True)
                    renumber_native(c, donor["protocol_barrier"])
                self.assertIs(binding_view(result, record, listener), c)

    def test_rust_outbound_requires_its_exact_native_barrier_receipt(self):
        for name, implementation, outbound in BARRIER_CASES[:2]:
            for change in ("missing", "source", "connection", "stream", "direction", "protocol", "typed_error",
                           "opened", "rejected", "bool_connection", "read_bytes", "write_sequence", "frames", "extra_claim"):
                with self.subTest(name=name, change=change):
                    result, record, listener = direction_fixture(name, implementation, outbound)
                    receipt = result["protocol_barrier"]
                    if change == "missing": del result["protocol_barrier"]
                    if change == "source": receipt["source"] = "configured-unknown-barrier"
                    if change == "connection": receipt["connection_trace_id"] = 2
                    if change == "stream": receipt["stream_trace_id"] = 1
                    if change == "direction": receipt["direction"] = "inbound"
                    if change == "protocol": receipt["protocol"] = ECHO
                    if change == "typed_error": receipt["native_error"] = "timeout"
                    if change == "opened": receipt["opened_sequence"] += 1
                    if change == "rejected": receipt["rejected_sequence"] += 1
                    if change == "bool_connection": receipt["connection_trace_id"] = True
                    if change == "read_bytes": receipt["read_frames"][1]["framed_hex"] = "036f6b0a"
                    if change == "write_sequence": receipt["write_frames"][1]["sequence"] -= 1
                    if change == "frames": receipt["read_frames"] += copy.deepcopy(receipt["read_frames"][:1])
                    if change == "extra_claim": receipt["configured_verified"] = True
                    with self.assertRaisesRegex(ValueError, "Rust protocol_barrier receipt"):
                        binding_view(result, record, listener)

    def test_barrier_requires_four_canonical_bilateral_native_frames_not_labels(self):
        for name, implementation, outbound in BARRIER_CASES:
            for change in ("label_only", "missing_na", "duplicate_na", "wrong_reply", "partial", "noncanonical",
                           "wrong_side", "wrong_phase", "reply_before_proposal", "wrong_open_direction", "missing_open",
                           "missing_stream", "application_bytes", "application_complete"):
                with self.subTest(name=name, implementation=implementation, change=change):
                    result, record, listener = direction_fixture(name, implementation, outbound)
                    _, c, unknown, _ = barrier_parts(result, listener, implementation, outbound)
                    kind = "negotiation_frame" if implementation == "rust" else "multistream_frame"
                    frames = [e for e in c["events"] if e.get("stream_trace_id") == 2 and e["kind"] == kind]
                    proposal = next(e for e in frames if bytes.fromhex(e["frame_hex"]).endswith((UNKNOWN + "\n").encode()))
                    na = next(e for e in frames if e["frame_hex"] == "036e610a")
                    opened = next(e for e in c["events"] if e.get("stream_trace_id") == 2 and e["kind"] in {"stream_open", "substream_opened"})
                    if change == "label_only": proposal["frame_hex"] = (varint(len(ECHO) + 1) + ECHO.encode() + b"\n").hex()
                    if change == "missing_na": c["events"].remove(na)
                    if change == "duplicate_na": c["events"].insert(c["events"].index(na), copy.deepcopy(na))
                    if change == "wrong_reply": na["frame_hex"] = proposal["frame_hex"]
                    if change == "partial": na["frame_hex"] = "036e61"
                    if change == "noncanonical": na["frame_hex"] = "83006e610a"
                    if change == "wrong_side": na["direction"] = proposal["direction"]
                    if change == "wrong_phase": na["phase"] = "security"
                    if change == "reply_before_proposal":
                        c["events"].remove(na); c["events"].insert(c["events"].index(proposal), na)
                    if change == "wrong_open_direction":
                        if implementation == "rust": opened["detail"]["direction"] = "inbound"
                        else: opened["direction"] = "Outbound"
                    if change == "missing_open": c["events"].remove(opened)
                    if change == "application_bytes":
                        if implementation == "rust": unknown["read"] = body(b"not-negotiation")
                        else: unknown["read_framed_bytes"] = 1
                    if change == "application_complete": unknown["response_write_complete"] = True
                    renumber_native(c)
                    if change == "missing_stream": c["streams"].remove(unknown)
                    with self.assertRaises(ValueError):
                        binding_view(result, record, listener)

    def test_requested_native_open_must_follow_na_even_if_its_ack_is_later(self):
        for name, implementation, outbound in BARRIER_CASES:
            for change in ("before_na", "missing_open", "duplicate_open", "wrong_direction", "packet", "missing_ack"):
                with self.subTest(name=name, implementation=implementation, change=change):
                    result, record, listener = direction_fixture(name, implementation, outbound)
                    donor, c, _, requested = barrier_parts(result, listener, implementation, outbound)
                    opened = next(e for e in c["events"] if e.get("stream_trace_id") == 3 and e["kind"] in {"stream_open", "substream_opened"})
                    na = next(e for e in c["events"] if e.get("stream_trace_id") == 2 and e.get("frame_hex") == "036e610a")
                    reply = "read" if outbound else "write"
                    ack = next(e for e in c["events"] if e.get("stream_trace_id") == 3
                               and e["kind"] in {"multistream_frame", "negotiation_frame"}
                               and e["direction"] == reply and e.get("frame_hex") != "132f6d756c746973747265616d2f312e302e300a")
                    if change == "before_na":
                        c["events"].remove(opened); c["events"].insert(c["events"].index(na), opened)
                    if change == "missing_open": c["events"].remove(opened)
                    if change == "duplicate_open": c["events"].insert(c["events"].index(opened), copy.deepcopy(opened))
                    if change == "wrong_direction":
                        if implementation == "rust": opened["detail"]["direction"] = "inbound"
                        else: opened["direction"] = "Outbound"
                    if change == "packet":
                        if implementation == "rust": requested["read"]["framed_sha256"] = "0" * 64
                        else: requested["write_framed_sha256"] = "0" * 64
                    if change == "missing_ack": c["events"].remove(ack)
                    renumber_native(c, donor.get("protocol_barrier"))
                    if change == "before_na":
                        self.assertGreater(ack["sequence"], na["sequence"])
                        with self.assertRaisesRegex(ValueError, "not uniquely bound"):
                            binding_view(result, record, listener)
                    else:
                        with self.assertRaises(ValueError):
                            binding_view(result, record, listener)

    def test_barrier_cannot_choose_between_duplicate_post_na_exchanges_or_unknowns(self):
        for name, implementation, outbound in BARRIER_CASES:
            for change in ("duplicate_reply", "duplicate_unknown", "another_connection"):
                with self.subTest(name=name, implementation=implementation, change=change):
                    result, record, listener = direction_fixture(name, implementation, outbound)
                    donor, c, _, requested = barrier_parts(result, listener, implementation, outbound)
                    if change == "duplicate_reply":
                        if implementation == "rust":
                            rust_stream(c, requested["protocol"], bytes.fromhex(result["request"]["framed_hex"]) if "request" in result else None,
                                        bytes.fromhex(result["response"]["framed_hex"]), True)
                            renumber_native(c, donor["protocol_barrier"])
                        else:
                            go_stream(c, requested["protocol"], bytes.fromhex(result["response"]["framed_hex"]), None, False)
                    if change == "duplicate_unknown":
                        if implementation == "rust": rust_barrier(c)
                        else: go_barrier(c)
                    if change == "another_connection":
                        proof = donor["upgrade_observation" if implementation == "rust" else "private_observation"]
                        foreign = copy.deepcopy(c)
                        foreign.update(connection_trace_id=2, authenticated_remote_peer_id=LOCAL if outbound else REMOTE)
                        proof["connections"].append(foreign)
                        c["events"] = [e for e in c["events"] if e.get("stream_trace_id") != 2]
                        renumber_native(c)
                    with self.assertRaisesRegex(ValueError, "not uniquely bound|unique observed UNKNOWN"):
                        binding_view(result, record, listener)

    def test_go_rejected_stream_cleanup_cannot_hide_pre_na_or_unrelated_native_errors(self):
        for change in ("early_incomplete", "missing_rejected", "wrong_rejected_protocol", "early_rejected",
                       "duplicate_rejected", "reset_error", "other_error", "wrong_failed", "wrong_reset"):
            with self.subTest(change=change):
                result, record, listener = direction_fixture("identify_private_tcp_yamux_pnet", "go", False)
                _, c, unknown, _ = barrier_parts(result, listener, "go", False)
                scoped = [e for e in c["events"] if e.get("stream_trace_id") == 2]
                rejected = next(e for e in scoped if e["kind"] == "protocol_rejected")
                na = next(e for e in scoped if e.get("frame_hex") == "036e610a")
                incomplete = next(e for e in scoped if e["kind"] == "negotiation_incomplete")
                reset = next(e for e in scoped if e["kind"] == "stream_reset_returned")
                if change == "early_incomplete":
                    c["events"].remove(incomplete); c["events"].insert(c["events"].index(na), incomplete)
                if change == "missing_rejected": c["events"].remove(rejected)
                if change == "wrong_rejected_protocol": rejected["protocol"] = ECHO
                if change == "early_rejected":
                    c["events"].remove(rejected); c["events"].insert(c["events"].index(na), rejected)
                if change == "duplicate_rejected": c["events"].insert(c["events"].index(rejected), copy.deepcopy(rejected))
                if change == "reset_error": reset["error"] = "real native reset failure"
                if change == "other_error": incomplete["kind"] = "read_error"
                if change == "wrong_failed": unknown["failed"] = False
                if change == "wrong_reset": unknown["reset"] = False
                renumber_native(c)
                with self.assertRaisesRegex(ValueError, "Go protocol barrier lacks|Go rejection has"):
                    binding_view(result, record, listener)

    def test_logical_peer_suffix_does_not_become_a_socket_receipt(self):
        self.assertEqual(logical_tcp_address(REMOTE_ADDR, REMOTE), REMOTE_ADDR)
        self.assertEqual(logical_tcp_address(REMOTE_ADDR + "/p2p/" + REMOTE, REMOTE), REMOTE_ADDR)
        for value in (REMOTE_ADDR + "/p2p/" + LOCAL,
                      REMOTE_ADDR + "/p2p/" + REMOTE + "/p2p/" + REMOTE):
            with self.assertRaises(ValueError):
                logical_tcp_address(value, REMOTE)
        with self.assertRaises(ValueError):
            tcp_address(REMOTE_ADDR + "/p2p/" + REMOTE)
        for field in ("local_address", "remote_address"):
            result, record, listener = semantic_fixture("tcp_yamux_private_pnet")
            result["connection_receipt"][field] += "/p2p/" + REMOTE
            self.assertTrue(validate_private_profile(result, record, listener), field)

    def test_rust_late_shutdown_preserves_error_after_exact_inbound_response(self):
        for name in ("noise_identity_private_pnet", "rendezvous_rust_private_tcp_yamux_pnet"):
            result, record, listener = direction_fixture(name, "rust", False)
            c = listener["upgrade_observation"]["connections"][0]
            c["negotiations"][1]["io_failed"] = True
            event(c, "muxer", "muxer_poll_error", detail={"error": "not a whitelisted close string",
                                                          "failure_stage": "post_upgrade"})
            self.assertEqual(validate_private_profile(result, record, listener), [], name)
            self.assertEqual(c["events"][-1]["kind"], "muxer_poll_error")

    def test_rust_error_cannot_be_hidden_by_a_label_or_another_response(self):
        for change in ("before_reply", "pre_upgrade", "no_marker", "wrong_sequence", "wrong_bytes",
                       "other_stream", "no_close", "io_failure", "parser_failure", "duplicate_marker"):
            result, record, listener = direction_fixture("noise_identity_private_pnet", "rust", False)
            c = listener["upgrade_observation"]["connections"][0]
            stream = c["streams"][0]
            marker = next(e for e in c["events"] if e["kind"] == "response_completion")
            c["negotiations"][1]["io_failed"] = True
            event(c, "muxer", "muxer_poll_error", detail={"error": "connection is closed", "failure_stage": "post_upgrade"})
            if change == "before_reply":
                error = c["events"].pop()
                c["events"].insert(c["events"].index(marker), error)
                renumber(c)
            if change == "pre_upgrade": c["events"][-1]["detail"]["failure_stage"] = "pre_upgrade"
            if change == "no_marker": c["events"].remove(marker); renumber(c)
            if change == "wrong_sequence": stream["response_completed_sequence"] -= 1
            if change == "wrong_bytes": marker["detail"]["write"]["framed_sha256"] = "0" * 64
            if change == "other_stream":
                rust_stream(c, ECHO, public_receipt(b"other")["framed_hex"].encode(), b"other", False)
                c["events"].remove(marker)
                renumber(c)
            if change == "no_close": stream["write_close_returned"] = False
            if change == "io_failure": stream["io_failed"] = True
            if change == "parser_failure": stream["parser_error"] = "invalid negotiation frame length"
            if change == "duplicate_marker": c["events"].append(copy.deepcopy(marker)); renumber(c)
            self.assertTrue(validate_private_profile(result, record, listener), change)

    def test_rust_outbound_request_close_is_not_a_response_boundary(self):
        result, record, listener = direction_fixture("noise_identity_private_pnet", "rust", True)
        c = result["upgrade_observation"]["connections"][0]
        c["negotiations"][1]["io_failed"] = True
        event(c, "muxer", "muxer_poll_error", detail={"error": "connection is closed", "failure_stage": "post_upgrade"})
        self.assertTrue(validate_private_profile(result, record, listener))

    def test_native_rust_ping_and_kad_flush_are_exact_inbound_response_boundaries(self):
        cases = (("ping_private_tcp_yamux_pnet", "native_ping_response_flush"),
                 ("kademlia_amino_private_tcp_yamux_pnet", "native_kad_response_flush"))
        for name, basis in cases:
            result, record, listener = direction_fixture(name, "rust", False)
            c = listener["upgrade_observation"]["connections"][0]
            stream = c["streams"][0]
            marker = next(e for e in c["events"] if e["kind"] == "response_completion")
            stream.update(write_close_returned=False, write_flush_returned=True)
            marker["detail"].update(completion_basis=basis, read=copy.deepcopy(stream["read"]))
            c["negotiations"][1]["io_failed"] = True
            event(c, "muxer", "muxer_poll_error", detail={"error": "after completed native reply", "failure_stage": "post_upgrade"})
            self.assertEqual(validate_private_profile(result, record, listener), [], name)

    def test_rust_native_flush_cannot_replace_another_protocol_or_packet(self):
        for change in ("basis", "no_flush", "read", "write", "frames", "partial", "ping_size", "before_reply", "outbound"):
            result, record, listener = direction_fixture("ping_private_tcp_yamux_pnet", "rust", False)
            c = listener["upgrade_observation"]["connections"][0]
            stream = c["streams"][0]
            marker = next(e for e in c["events"] if e["kind"] == "response_completion")
            stream.update(write_close_returned=False, write_flush_returned=True)
            marker["detail"].update(completion_basis="native_ping_response_flush", read=copy.deepcopy(stream["read"]))
            if change == "basis": marker["detail"]["completion_basis"] = "native_kad_response_flush"
            if change == "no_flush": stream["write_flush_returned"] = False
            if change == "read": marker["detail"]["read"]["framed_sha256"] = "0" * 64
            if change == "write": marker["detail"]["write"]["framed_sha256"] = "0" * 64
            if change == "frames": stream["read"]["frames"] = marker["detail"]["read"]["frames"] = 2
            if change == "partial": stream["read"]["complete_frames"] = marker["detail"]["read"]["complete_frames"] = False
            if change == "ping_size":
                for b in (stream["read"], stream["write"], marker["detail"]["read"], marker["detail"]["write"]):
                    b["framed_bytes"] = 31
            if change == "before_reply":
                event(c, "muxer", "muxer_poll_error", detail={"error": "before reply", "failure_stage": "post_upgrade"})
                failure = c["events"].pop(); c["events"].insert(c["events"].index(marker), failure); renumber(c)
            if change == "outbound": stream["direction"] = "outbound"
            self.assertTrue(validate_private_profile(result, record, listener), change)

    def test_all_46_bilateral_records_have_packet_and_endpoint_checks(self):
        from private_profile_cases import case_specs
        for spec in case_specs("stage6"):
            donor = spec.listener if spec.dialer == "forge" else spec.dialer
            result, record, listener = direction_fixture(spec.contract, donor, spec.dialer != "forge")
            with self.subTest(spec=spec):
                self.assertEqual(validate_private_profile(result, record, listener), [])

    def test_go_receipt_cannot_use_another_stream_or_connection(self):
        for change in ("stream", "connection", "binding", "socket", "completion"):
            result, record, listener = direction_fixture("ping_private_tcp_yamux_pnet", "go", True)
            c = result["private_observation"]["connections"][0]
            if change == "stream": result["stream_id"] = "different-stream"
            if change == "connection": result["connection_receipt"]["connection_id"] = "different-connection"
            if change == "binding":
                c["events"].remove([e for e in c["events"] if e["kind"] == "application_stream_binding"][-1])
                renumber_native(c)
            if change == "socket": result["connection_receipt"]["local_address"] = REMOTE_ADDR
            if change == "completion": c["streams"][0]["application_io_complete"] = False
            self.assertTrue(validate_private_profile(result, record, listener), change)

    def test_go_identical_background_reply_does_not_replace_native_stream_binding(self):
        for name in ("identify_private_tcp_yamux_pnet", "kademlia_amino_private_tcp_yamux_pnet"):
            result, record, listener = direction_fixture(name, "go", True)
            c = result["private_observation"]["connections"][0]
            background = go_stream(c, c["streams"][0]["protocol"],
                                   bytes.fromhex(result["request"]["framed_hex"]) if "request" in result else None,
                                   bytes.fromhex(result["response"]["framed_hex"]), True)
            self.assertEqual(validate_private_profile(result, record, listener), [], name)
            background["network_stream_id"] = result["stream_id"]
            self.assertTrue(validate_private_profile(result, record, listener), name)

    def test_go_binding_cannot_cover_a_different_packet_or_missing_native_events(self):
        for change in ("packet", "missing_events", "wrong_event_stream", "missing_id"):
            result, record, listener = direction_fixture("identify_private_tcp_yamux_pnet", "go", True)
            c = result["private_observation"]["connections"][0]
            if change == "packet": c["streams"][0]["read_framed_sha256"] = "0" * 64
            if change == "missing_events":
                c["events"] = [e for e in c["events"] if e["kind"] != "application_stream_binding"]
                renumber_native(c)
            if change == "wrong_event_stream":
                [e for e in c["events"] if e["kind"] == "application_stream_binding"][-1]["network_stream_id"] = "background-stream"
            if change == "missing_id": del result["stream_id"]
            self.assertTrue(validate_private_profile(result, record, listener), change)

    def test_every_registered_contract_has_packet_semantics(self):
        for name in CONTRACTS:
            with self.subTest(name=name):
                result, record, listener = semantic_fixture(name)
                self.assertEqual(validate_private_profile(result, record, listener), [])
                self.assertTrue(validate_private_profile({"status":"ok"}, record, listener))

    def test_negotiated_connection_not_configuration(self):
        for field in ("source", "security", "muxer", "remote_peer_id", "connection_id", "remote_address"):
            result, record, listener = semantic_fixture("noise_identity_private_pnet")
            del result["connection_receipt"][field]
            self.assertTrue(validate_private_profile(result, record, listener), field)

    def test_configured_or_invented_security_cannot_replace_native_receipt(self):
        result, record, listener = semantic_fixture("tls_identity_private_pnet")
        listener["private_observation"]["source"] = "configured_tls_yamux"
        self.assertTrue(validate_private_profile(result, record, listener))

    def test_body_hash_length_and_frame_ambiguity(self):
        for field, value in (("framed_bytes", True), ("framed_sha256", "0"*64), ("frames", 2), ("complete_frames", False)):
            result, record, listener = semantic_fixture("tcp_yamux_private_pnet")
            result["response"]["read"][field] = value
            self.assertTrue(validate_private_profile(result, record, listener))
        with self.assertRaises(ValueError):
            framed_receipt(public_receipt(b"\x81\0a", raw=True))

    def test_raw_packet_must_match_one_actual_donor_substream(self):
        for change in ("hash", "duplicate", "frames", "peer", "overflow"):
            result, record, listener = semantic_fixture("ping_private_tcp_yamux_pnet")
            proof = listener["private_observation"]; c = proof["connections"][0]
            if change == "hash": c["streams"][0]["write_framed_sha256"] = "0"*64
            if change == "duplicate": c["streams"].append(copy.deepcopy(c["streams"][0]))
            if change == "frames": c["events"] = []
            if change == "peer": c["authenticated_remote_peer_id"] = REMOTE
            if change == "overflow": proof["overflow"] = True
            self.assertTrue(validate_private_profile(result, record, listener), change)

    def test_both_key_controls_require_bilateral_runtime_ingress(self):
        for kind in ("missing_key", "mismatched_key"):
            for side in ("result", "listener_result"):
                for field, bad in (("attempted_connections", 0), ("established_connections", 1),
                                   ("application_streams", 1), ("counter_source", "configured"), ("correlation_token", "wrong")):
                    result, record, listener = semantic_fixture("tcp_yamux_private_pnet")
                    record[kind][side][field] = bad
                    self.assertTrue(validate_private_profile(result, record, listener), (kind, side, field))

    def test_protocol_results_are_not_boolean_success(self):
        for name in ("identify_private_tcp_yamux_pnet", "kademlia_amino_private_tcp_yamux_pnet", "rendezvous_rust_private_tcp_yamux_pnet"):
            result, record, listener = semantic_fixture(name)
            result["response"] = public_receipt(b"\x08\x01")
            self.assertTrue(validate_private_profile(result, record, listener))

    def test_go_inline_requires_observed_early_muxer_state(self):
        result, record, listener = semantic_fixture("inline_muxer_go_tls")
        listener["private_observation"]["connections"][0]["early_muxer_negotiation"] = False
        self.assertTrue(validate_private_profile(result, record, listener))

    def test_rust_fallback_requires_actual_post_security_bytes(self):
        result, record, listener = semantic_fixture("inline_muxer_rust_tls_fixed_alpn_fallback")
        c = result["upgrade_observation"]["connections"][0]
        c["events"] = [e for e in c["events"] if e["phase"] != "muxer" or e["kind"] != "negotiation_frame"]
        result["configured_alpn"] = "libp2p"
        self.assertTrue(validate_private_profile(result, record, listener))

    def test_unobserved_unknown_protocol_rejection_does_not_count(self):
        result, record, listener = semantic_fixture("multistream_select_private_pnet")
        del result["unknown_protocol_rejected"]
        self.assertTrue(validate_private_profile(result, record, listener))
        result, record, listener = semantic_fixture("multistream_select_private_pnet")
        listener["private_observation"]["connections"][0]["events"] = listener["private_observation"]["connections"][0]["events"][:5]
        self.assertTrue(validate_private_profile(result, record, listener))

    def test_rust_receipt_must_bind_actual_transport_output_and_swarm(self):
        for change in ("trace", "transport", "swarm", "socket", "drop"):
            result, record, listener = semantic_fixture("inline_muxer_rust_noise_fallback")
            proof = result["upgrade_observation"]
            c = proof["connections"][0]
            if change == "trace": result["connection_receipt"]["connection_id"] = 9
            if change == "transport": c["transport_output_receipts"] = []
            if change == "swarm": proof["swarm_events"] = []
            if change == "socket": result["connection_receipt"]["local_address"] = REMOTE_ADDR
            if change == "drop": c["muxer_drop_observed"] = False
            self.assertTrue(validate_private_profile(result, record, listener), change)


if __name__ == "__main__":
    unittest.main()
