"""Synthetic collector receipts, NOT live, signed-envelope or promotion evidence.

The deliberately opaque Identify payload exercises framing correlation only.
The acceptance caller must also run its existing semantic Identify validator
and indexed process/launcher validation; these tests do not replace either.
"""

import copy
import hashlib
import unittest

from rust_upgrade_evidence import (
    BASIS, ECHO, IDENTIFY, validate_rust_dial_upgrade, validate_rust_listener_upgrade,
)
from upgrade_evidence import HEADER, MUXER


def peer(seed):
    alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
    raw = b"\x12\x20" + hashlib.sha256(seed.encode()).digest()
    number, encoded = int.from_bytes(raw, "big"), ""
    while number:
        number, digit = divmod(number, 58)
        encoded = alphabet[digit] + encoded
    return encoded


LOCAL, REMOTE, OTHER = peer("local"), peer("remote"), peer("other")
LOCAL_ADDR = "/ip4/127.0.0.1/tcp/41000"
REMOTE_ADDR = "/ip4/127.0.0.1/tcp/42000"


def prefix(size):
    result = bytearray()
    while size >= 128:
        result.append((size & 127) | 128)
        size >>= 7
    result.append(size)
    return bytes(result)


def frame(payload):
    return prefix(len(payload)) + payload


def token(protocol):
    return frame((protocol + "\n").encode()).hex()


def body(payload=None):
    framed = b"" if payload is None else frame(payload)
    return {"framed_bytes": len(framed), "frames": int(payload is not None),
            "framed_sha256": hashlib.sha256(framed).hexdigest(),
            "complete_frames": payload is not None, "invalid_or_over_limit": False}


def selection(protocol, read=None, write=None):
    return {"direction": "outbound", "protocol": protocol, "proposed_protocol": protocol,
            "parser_error": None, "io_failed": False, "drop_observed": True,
            "read_eof": False, "write_close_returned": write is not None,
            "response_write_complete": write is not None,
            "upgrade_completed_sequence": None, "response_completed_sequence": None,
            "read": body(read), "write": body(write)}


def event(connection, phase, kind, sid=None, **fields):
    item = {"sequence": len(connection["events"]) + 1, "phase": phase,
            "kind": kind, "stream_trace_id": sid, **fields}
    connection["events"].append(item)
    return item["sequence"]


def negotiation(connection, phase, protocol, sid=None):
    for direction, value in (("write", HEADER), ("write", protocol),
                             ("read", HEADER), ("read", protocol)):
        fields = {"direction": direction, "frame_hex": token(value)}
        if direction == "read" and value == protocol:
            fields.update(selection_outcome="selected", protocol=protocol)
        event(connection, phase, "negotiation_frame", sid, **fields)


def raw_stream(connection, protocol, read=None, write=None):
    sid = len(connection["streams"]) + 1
    stream = selection(protocol, read, write)
    stream["stream_trace_id"] = sid
    connection["streams"].append(stream)
    event(connection, "application", "substream_opened", sid, detail={"direction": "outbound"})
    negotiation(connection, "application", protocol, sid)
    if write is not None:
        stream["response_completed_sequence"] = event(
            connection, "application", "response_completion", sid,
            detail={"protocol": protocol, "write": copy.deepcopy(stream["write"])})
    return stream


def receipt(scenario="identify", security="/noise"):
    endpoint = {"direction": "outbound", "upgrade_role": "outbound",
                "remote_address": REMOTE_ADDR + "/p2p/" + REMOTE}
    connection = {"connection_trace_id": 1, "direction": "outbound", "endpoint": endpoint,
                  "authenticated_local_peer_id": LOCAL, "authenticated_remote_peer_id": REMOTE,
                  "local_address": LOCAL_ADDR, "remote_address": REMOTE_ADDR,
                  "selected_security": security, "selected_muxer": MUXER,
                  "security_complete": True, "muxer_complete": True,
                  "security_delegate_completed": True, "muxer_delegate_completed": True,
                  "early_muxer_negotiation": False, "muxer_drop_observed": True,
                  "muxer_close_returned": False, "overflow": False,
                  "negotiations": [selection(security), selection(MUXER)], "streams": [], "events": []}
    event(connection, "security", "delegate_started", detail={"protocol": security})
    negotiation(connection, "security", security)
    event(connection, "security", "delegate_completed", detail={"protocol": security})
    connection["negotiations"][0]["upgrade_completed_sequence"] = event(
        connection, "security", "upgrade_completed", detail={"protocol": security})
    event(connection, "security", "authenticated_peer", detail={"peer_id": REMOTE})
    event(connection, "muxer", "delegate_started", detail={"protocol": MUXER})
    # V1Lazy: constructing Yamux completes before its wire proposal/ACK.
    event(connection, "muxer", "delegate_completed", detail={"protocol": MUXER})
    negotiation(connection, "muxer", MUXER)
    connection["negotiations"][1]["upgrade_completed_sequence"] = event(
        connection, "muxer", "upgrade_completed", detail={"protocol": MUXER})
    data = b"synthetic opaque Identify bytes, not a signed envelope"
    raw_stream(connection, IDENTIFY, read=data)  # Automatic exchange predates our open boundary.
    attempts = []
    for protocol in ([IDENTIFY] if scenario == "identify" else [IDENTIFY, ECHO]):
        before = len(connection["streams"])
        stream = raw_stream(connection, protocol, read=data if protocol == IDENTIFY else b"abc",
                            write=None if protocol == IDENTIFY else b"abc")
        attempts.append({
            "application_trace_id": len(attempts) + 1, "authenticated_remote_peer_id": REMOTE,
            "protocol": protocol, "direction": "outbound",
            "preexisting_raw_streams": [{"connection_trace_id": 1, "stream_count": before}],
            "opened": True, "application_io_complete": True, "attempt_ended": True,
            "stream_drop_returned": True, "write_close_returned": stream["write_close_returned"],
            "read_eof": False, "remote_receipt_claimed": False, "errors": [], "overflow": False,
            "read": copy.deepcopy(stream["read"]), "write": copy.deepcopy(stream["write"]),
            "binding": {"basis": BASIS, "connection_trace_id": 1, "stream_trace_id": stream["stream_trace_id"],
                        "swarm_connection_id": "17", "authenticated_local_peer_id": LOCAL,
                        "authenticated_remote_peer_id": REMOTE, "local_address": LOCAL_ADDR,
                        "remote_address": REMOTE_ADDR, "remote_receipt_claimed": False}})
    proof = {"source": "rust-libp2p.public-connection-upgrades.v1", "complete": True,
             "overflow": False, "finalized_after_swarm_drop": True, "fixture_owned_tasks_joined": True,
             "exact_application_binding_supported": True, "swarm_connection_binding_supported": True,
             "binding_basis": BASIS, "connections": [connection],
             "swarm_events": [{"kind": "connection_established", "swarm_connection_id": "17",
                               "authenticated_remote_peer_id": REMOTE, "endpoint": copy.deepcopy(endpoint)}],
             "applications": {"source": "actual_swarm_stream_framed_io", "overflow": False, "attempts": attempts}}
    if scenario == "echo":
        proof["application_pair_binding"] = {"basis": BASIS, "connection_trace_id": 1,
                                             "swarm_connection_id": "17", "identify_stream_trace_id": 2,
                                             "echo_stream_trace_id": 3}
    return {"implementation": "rust", "role": "dialer", "status": "ok", "scenario": scenario,
            "signed_peer_record": False, "protocol": IDENTIFY if scenario == "identify" else ECHO,
            "payload_bytes": 3, "echo_ok": scenario == "echo", "upgrade_observation": proof,
            "raw_identify_exchange": {"status": "verified", "error": None,
                                      "basis": "fixture_separate_authenticated_identify_exchange",
                                      "protocol": IDENTIFY, "authenticated_remote_peer_id": REMOTE,
                                      "raw_capture_truncated": False, "raw_protobuf_hex": data.hex(),
                                      "raw_protobuf_bytes": len(data),
                                      "raw_protobuf_sha256": hashlib.sha256(data).hexdigest()},
            "fixture_task_lifecycle": {
                "scope": "public_swarm_executor_and_fixture_echo_handler",
                "shutdown_mode": "close_admission_abort_join_after_swarm_drop",
                "fixture_owned_tasks_joined": True, "overflow": False, "errors": [],
                "tasks": [{"task_id": "29", "kind": "swarm_connection",
                           "abort_requested": True, "terminal": "completed"}]}}


def renumber(connection):
    for record in connection["negotiations"] + connection["streams"]:
        record["upgrade_completed_sequence"] = None
        record["response_completed_sequence"] = None
    for index, item in enumerate(connection["events"], 1):
        item["sequence"] = index
        if item["kind"] in {"upgrade_completed", "response_completion"}:
            if item["phase"] == "application":
                record = connection["streams"][item["stream_trace_id"] - 1]
                key = "response_completed_sequence"
            else:
                record = connection["negotiations"][int(item["phase"] == "muxer")]
                key = "upgrade_completed_sequence"
            record[key] = index


def background(result, authenticated=OTHER):
    """A real-shaped failed upgrade; no failure is filtered from the receipt."""
    proof = result["upgrade_observation"]
    connection = copy.deepcopy(proof["connections"][0])
    connection["connection_trace_id"] = len(proof["connections"]) + 1
    connection["authenticated_remote_peer_id"] = authenticated
    connection["endpoint"]["remote_address"] = REMOTE_ADDR + "/p2p/" + (authenticated or REMOTE)
    phase = "muxer" if authenticated else "security"
    index = int(bool(authenticated))
    connection["streams"] = []
    connection["events"] = [e for e in connection["events"] if phase == "muxer" and e["phase"] == "security"]
    for item in connection["events"]:
        if item["kind"] == "authenticated_peer":
            item["detail"]["peer_id"] = authenticated
    for pos in range(index, 2):
        name = ("security", "muxer")[pos]
        connection[name + "_complete"] = connection[name + "_delegate_completed"] = False
        connection["selected_" + name] = None
        connection["negotiations"][pos] = selection(None)
    protocol = "/noise" if phase == "security" else MUXER
    negotiation(connection, phase, protocol)
    last = connection["events"][-1]
    last["frame_hex"] = token("/wrong-ack")
    del last["selection_outcome"], last["protocol"]
    failed = connection["negotiations"][index]
    failed.update(proposed_protocol=protocol, parser_error="acknowledgement disagrees with proposal", io_failed=True)
    event(connection, phase, "upgrade_error", detail={"error": "synthetic delegate failure",
                                                       "failure_stage": "pre_upgrade"})
    proof["connections"].append(connection)
    return connection


def replace(result, path, value):
    destination = result
    for key in path[:-1]:
        destination = destination[key]
    destination[path[-1]] = value


def listener_pair(scenario="identify", security="/noise", late_error=False):
    listener = receipt(scenario, security)
    listener = {key: listener[key] for key in
                ("implementation", "role", "status", "scenario", "upgrade_observation", "fixture_task_lifecycle")}
    listener["role"] = "listener"
    proof = listener["upgrade_observation"]
    proof["complete"] = False
    proof["applications"]["attempts"] = []
    proof.pop("application_pair_binding", None)
    c = proof["connections"][0]
    c["direction"] = "inbound"
    c["endpoint"] = {"direction": "inbound", "upgrade_role": "inbound",
                     "local_address": LOCAL_ADDR, "remote_address": REMOTE_ADDR}
    proof["swarm_events"][0]["endpoint"] = copy.deepcopy(c["endpoint"])
    for record in c["negotiations"] + c["streams"][1:]:
        record["direction"] = "inbound"
        record["read"], record["write"] = record["write"], record["read"]
    identify = c["streams"][1]
    identify["write_close_returned"] = identify["response_write_complete"] = True
    for item in c["events"]:
        if item["stream_trace_id"] == 1:
            continue  # Automatic Rust outbound Identify remains a distinct direction.
        if item["kind"] == "substream_opened":
            item["detail"]["direction"] = "inbound"
        if item["kind"] == "negotiation_frame":
            item["direction"] = "read" if item["direction"] == "write" else "write"
    # Model the captured inbound listener order, not a synthetic reversal of
    # the outbound V1Lazy delegate-before-wire case above.
    for phase in ("security", "muxer"):
        delegates = [e for e in c["events"] if e["phase"] == phase
                     and e["kind"] in {"delegate_started", "delegate_completed"}]
        for item in delegates:
            c["events"].remove(item)
        position = max(i for i, e in enumerate(c["events"])
                       if e["phase"] == phase and e["kind"] == "negotiation_frame") + 1
        c["events"][position:position] = delegates
    last_identify = max(index for index, e in enumerate(c["events"]) if e["stream_trace_id"] == 2)
    event(c, "application", "response_completion", 2,
          detail={"protocol": IDENTIFY, "write": copy.deepcopy(identify["write"])})
    marker = c["events"].pop()
    c["events"].insert(last_identify + 1, marker)
    renumber(c)
    if late_error:
        c["negotiations"][1]["io_failed"] = True
        event(c, "muxer", "muxer_poll_error",
              detail={"error": "arbitrary late failure, not a whitelisted string", "failure_stage": "post_upgrade"})
    forge = {"implementation": "forge", "role": "dialer", "status": "ok", "scenario": scenario,
             "single_fresh_connection_retained": True,
             "identify_event_basis": "automatic_identify_single_fresh_connection",
             "application_connection_id": 5, "identify_event_connection_id": 5,
             "signed_peer_record": True, "protocol_count": 10, "negotiated_transport": "tcp",
             "authenticated_remote_peer_id": LOCAL, "local_peer_id": REMOTE,
             "connection_remote_addr": LOCAL_ADDR}
    if scenario == "echo":
        echo = c["streams"][2]
        forge.update(echo_ok=True, payload_bytes=3, application_close_returned=True,
                     application_stream_id=7, application_protocol=ECHO, protocol=ECHO,
                     negotiated_security=security, negotiated_muxer=MUXER)
        for side, source in (("request", "read"), ("response", "write")):
            forge[f"application_{side}_framed_bytes"] = echo[source]["framed_bytes"]
            forge[f"application_{side}_framed_sha256"] = echo[source]["framed_sha256"]
    return forge, listener


def duplicate_inbound(c, sid, mode):
    stream = copy.deepcopy(c["streams"][sid - 1])
    old_events = [copy.deepcopy(e) for e in c["events"] if e["stream_trace_id"] == sid]
    stream["stream_trace_id"] = len(c["streams"]) + 1
    c["streams"].append(stream)
    if mode == "failed":
        stream.update(io_failed=True, response_write_complete=False, response_completed_sequence=None)
        old_events = [e for e in old_events if e["kind"] != "response_completion"]
    elif mode == "unfinished":
        stream.update(protocol=None, response_write_complete=False, response_completed_sequence=None,
                      write_close_returned=False, read=body(), write=body())
        old_events = [e for e in old_events if e["kind"] != "response_completion" and not e.get("selection_outcome")]
    elif mode == "rejected":
        stream.update(protocol=None, proposed_protocol=None, response_write_complete=False,
                      response_completed_sequence=None, write_close_returned=False, read=body(), write=body())
        old_events = [e for e in old_events if e["kind"] != "response_completion"]
        old_events[-1].update(frame_hex=token("na"), selection_outcome="rejected")
        del old_events[-1]["protocol"]
    for item in old_events:
        item["stream_trace_id"] = stream["stream_trace_id"]
        c["events"].append(item)
    renumber(c)
    if mode == "failed":
        event(c, "application", "write_error", stream["stream_trace_id"],
              detail={"error": "synthetic failed duplicate", "failure_stage": "post_upgrade"})


class RustListenerUpgradeEvidenceTests(unittest.TestCase):
    def assert_valid(self, pair, security="/noise"):
        self.assertEqual(validate_rust_listener_upgrade(*pair, LOCAL, security), [])

    def assert_invalid(self, pair, contains=None):
        errors = validate_rust_listener_upgrade(*pair, LOCAL, "/noise")
        self.assertTrue(errors)
        if contains:
            self.assertIn(contains, " ".join(errors))

    def test_synthetic_inbound_noise_tls_identify_echo_and_late_errors(self):
        for security in ("/noise", "/tls/1.0.0"):
            for scenario in ("identify", "echo"):
                for late in (False, True):
                    with self.subTest(security=security, scenario=scenario, late=late):
                        pair = listener_pair(scenario, security, late)
                        before = copy.deepcopy(pair)
                        self.assert_valid(pair, security)
                        self.assertEqual(pair, before)
                        self.assertFalse(pair[1]["upgrade_observation"]["complete"])

    def test_connection_error_after_identify_but_before_echo_is_not_late(self):
        pair = listener_pair("echo", late_error=True)
        c = pair[1]["upgrade_observation"]["connections"][0]
        failure = c["events"].pop()
        echo_start = next(i for i, e in enumerate(c["events"]) if e["stream_trace_id"] == 3)
        c["events"].insert(echo_start, failure)
        renumber(c)
        self.assert_invalid(pair)

    def test_pre_upgrade_stage_and_unexplained_io_failed_are_rejected(self):
        pair = listener_pair(late_error=True)
        pair[1]["upgrade_observation"]["connections"][0]["events"][-1]["detail"]["failure_stage"] = "pre_upgrade"
        self.assert_invalid(pair, "failure stage")
        pair = listener_pair()
        pair[1]["upgrade_observation"]["connections"][0]["negotiations"][1]["io_failed"] = True
        self.assert_invalid(pair, "target upgrade")

    def test_duplicate_inbound_selected_failed_unfinished_or_rejected_is_ambiguous(self):
        for scenario, sid in (("identify", 2), ("echo", 3)):
            for mode in ("complete", "failed", "unfinished", "rejected"):
                with self.subTest(scenario=scenario, mode=mode):
                    pair = listener_pair(scenario)
                    duplicate_inbound(pair[1]["upgrade_observation"]["connections"][0], sid, mode)
                    self.assert_invalid(pair, "ambiguous inbound response")

    def test_duplicate_authenticated_connection_and_swarm_are_not_filtered(self):
        pair = listener_pair()
        pair[1]["upgrade_observation"]["connections"].append(background(receipt(), REMOTE))
        self.assert_invalid(pair, "ambiguous authenticated inbound")
        pair = listener_pair()
        swarm = pair[1]["upgrade_observation"]["swarm_events"]
        extra = copy.deepcopy(swarm[0])
        extra["swarm_connection_id"] = "18"
        swarm.append(extra)
        self.assert_invalid(pair, "ambiguous matching inbound Swarm")

    def test_failed_target_stream_is_not_excused_by_late_connection_policy(self):
        pair = listener_pair("echo", late_error=True)
        c = pair[1]["upgrade_observation"]["connections"][0]
        c["streams"][2].update(io_failed=True, response_write_complete=False)
        event(c, "application", "read_error", 3,
              detail={"error": "synthetic late stream error", "failure_stage": "post_upgrade"})
        self.assert_invalid(pair, "inbound response failed")

    def test_fresh_forge_receipt_is_required_not_only_status_ok(self):
        changes = [("single_fresh_connection_retained", False), ("identify_event_basis", "explicit_exchange"),
                   ("application_connection_id", True), ("identify_event_connection_id", 6),
                   ("identify_event_connection_id", True), ("protocol_count", 0),
                   ("signed_peer_record", False), ("negotiated_transport", "quic")]
        for key, value in changes:
            with self.subTest(key=key):
                pair = listener_pair()
                pair[0][key] = value
                self.assert_invalid(pair, "fresh authenticated Forge")

    def test_wrong_identity_direction_and_endpoint_fail(self):
        changes = [(0, ["local_peer_id"], OTHER), (0, ["authenticated_remote_peer_id"], OTHER),
                   (0, ["connection_remote_addr"], REMOTE_ADDR), (0, ["connection_local_addr"], LOCAL_ADDR),
                   (1, ["upgrade_observation", "connections", 0, "authenticated_local_peer_id"], OTHER),
                   (1, ["upgrade_observation", "swarm_events", 0, "endpoint", "remote_address"], LOCAL_ADDR)]
        for side, path, value in changes:
            with self.subTest(path=path):
                pair = listener_pair()
                replace(pair[side], path, value)
                self.assert_invalid(pair)
        pair = listener_pair()
        c = pair[1]["upgrade_observation"]["connections"][0]
        c["direction"] = c["endpoint"]["direction"] = "outbound"
        self.assert_invalid(pair, "direction")

    def test_cross_endpoint_echo_counts_hash_and_exact_payload_prefix(self):
        changes = [("payload_bytes", 4), ("payload_bytes", True), ("application_request_framed_bytes", 3),
                   ("application_response_framed_bytes", True), ("application_request_framed_sha256", "00" * 32),
                   ("application_response_framed_sha256", "00" * 32), ("application_stream_id", True),
                   ("application_close_returned", False), ("negotiated_security", "/tls/1.0.0")]
        for key, value in changes:
            with self.subTest(key=key):
                pair = listener_pair("echo")
                pair[0][key] = value
                self.assert_invalid(pair)

    def test_actual_response_marker_close_drop_and_join_are_required(self):
        changes = [(["connections", 0, "streams", 1, "drop_observed"], False),
                   (["connections", 0, "streams", 1, "write_close_returned"], False),
                   (["connections", 0, "streams", 1, "response_completed_sequence"], None),
                   (["connections", 0, "streams", 1, "stream_trace_id"], True),
                   (["connections", 0, "muxer_drop_observed"], False),
                   (["fixture_owned_tasks_joined"], False), (["finalized_after_swarm_drop"], False),
                   (["complete"], True), (["overflow"], True),
                   (["swarm_events", 0, "swarm_connection_id"], True)]
        for path, value in changes:
            with self.subTest(path=path):
                pair = listener_pair()
                replace(pair[1]["upgrade_observation"], path, value)
                self.assert_invalid(pair)
        pair = listener_pair()
        pair[1]["fixture_task_lifecycle"]["errors"] = ["join failed"]
        self.assert_invalid(pair, "cleanup")

    def test_listener_application_claim_or_identify_request_body_rejected(self):
        pair = listener_pair()
        pair[1]["upgrade_observation"]["applications"]["attempts"] = [{}]
        self.assert_invalid(pair, "outbound application claims")
        pair = listener_pair()
        pair[1]["upgrade_observation"]["connections"][0]["streams"][1]["read"] = body(b"unexpected")
        self.assert_invalid(pair, "unexpected inbound body")


class RustDialUpgradeEvidenceTests(unittest.TestCase):
    def assert_valid(self, result, security="/noise"):
        self.assertEqual(validate_rust_dial_upgrade(result, REMOTE, security), [])

    def assert_invalid(self, result, contains=None):
        errors = validate_rust_dial_upgrade(result, REMOTE, "/noise")
        self.assertTrue(errors)
        if contains:
            self.assertIn(contains, " ".join(errors))

    def test_synthetic_noise_tls_identify_and_identify_then_echo(self):
        for security in ("/noise", "/tls/1.0.0"):
            for scenario in ("identify", "echo"):
                with self.subTest(security=security, scenario=scenario):
                    value = receipt(scenario, security)
                    before = copy.deepcopy(value)
                    self.assert_valid(value, security)
                    self.assertEqual(value, before)
                    self.assertFalse(value["signed_peer_record"])

    def test_v1lazy_delegate_precedes_wire_ack_but_marker_cannot(self):
        result = receipt()
        c = result["upgrade_observation"]["connections"][0]
        done = next(e for e in c["events"] if e["phase"] == "muxer" and e["kind"] == "delegate_completed")
        ack = next(e for e in c["events"] if e["phase"] == "muxer" and e.get("selection_outcome") == "selected")
        self.assertLess(done["sequence"], ack["sequence"])
        self.assert_valid(result)
        marker = next(e for e in c["events"] if e["phase"] == "muxer" and e["kind"] == "upgrade_completed")
        c["events"].remove(marker)
        c["events"].insert(c["events"].index(ack), marker)
        renumber(c)
        self.assert_invalid(result, "completion marker precedes")

    def test_bounded_na_fallback_is_replayed(self):
        result = receipt()
        c = result["upgrade_observation"]["connections"][0]
        proposal = next(e for e in c["events"] if e.get("frame_hex") == token("/noise"))
        ack = next(e for e in c["events"] if e.get("selection_outcome") == "selected")
        proposal["frame_hex"] = token("/tls/1.0.0")
        ack["frame_hex"], ack["selection_outcome"] = token("na"), "rejected"
        del ack["protocol"]
        pos = c["events"].index(ack) + 1
        frames = [{"phase": "security", "stream_trace_id": None, "kind": "negotiation_frame",
                   "direction": "write", "frame_hex": token("/noise")},
                  {"phase": "security", "stream_trace_id": None, "kind": "negotiation_frame",
                   "direction": "read", "frame_hex": token("/noise"),
                   "selection_outcome": "selected", "protocol": "/noise"}]
        c["events"][pos:pos] = frames
        renumber(c)
        self.assert_valid(result)
        ack["selection_outcome"] = "selected"
        self.assert_invalid(result, "selection annotation")

    def test_raw_negotiation_hex_framing_and_labels_fail_closed(self):
        bad = ["0", "AA", "80002f0a", "01ff", "022fff", token("/bad\x7f"),
               token("/" + "x" * 256), token("/wrong-first-header")]
        for value in bad:
            with self.subTest(value=value[:32]):
                result = receipt()
                result["upgrade_observation"]["connections"][0]["events"][1]["frame_hex"] = value
                self.assert_invalid(result)
        result = receipt()
        result["upgrade_observation"]["connections"][0]["events"][4]["protocol"] = "/tls/1.0.0"
        self.assert_invalid(result, "annotation")

    def test_good_target_and_failed_other_peer_history_are_retained(self):
        result = receipt()
        background(result)
        before = copy.deepcopy(result)
        self.assert_valid(result)
        self.assertEqual(result, before)

    def test_captured_failure_label_must_match_replay(self):
        for error in (None, "invalid negotiation frame length", "made up parser failure"):
            with self.subTest(error=error):
                result = receipt()
                failed = background(result)
                failed["negotiations"][1]["parser_error"] = error
                self.assert_invalid(result, "captured failure disagrees")

    def test_failed_same_peer_connection_is_still_ambiguous(self):
        result = receipt()
        background(result, REMOTE)
        self.assert_invalid(result, "ambiguous authenticated connection")

    def test_unauthenticated_endpoint_hint_is_not_authenticated_identity(self):
        result = receipt()
        failed = background(result, "")
        self.assertIn(REMOTE, failed["endpoint"]["remote_address"])
        self.assert_valid(result)
        connection = result["upgrade_observation"]["connections"][0]
        connection["authenticated_remote_peer_id"] = OTHER
        connection["endpoint"]["remote_address"] = REMOTE_ADDR + "/p2p/" + OTHER
        next(e for e in connection["events"] if e["kind"] == "authenticated_peer")["detail"]["peer_id"] = OTHER
        self.assert_invalid(result, "absent or ambiguous authenticated connection")

    def test_corrupt_unauthenticated_hint_rejected(self):
        result = receipt()
        background(result, "")["endpoint"]["remote_address"] = REMOTE_ADDR + "/p2p/not-a-peer"
        self.assert_invalid(result, "PeerId")

    def test_error_stage_cannot_override_sequence_or_target_failure(self):
        result = receipt()
        failed = background(result)
        failed["events"][-1]["detail"]["failure_stage"] = "post_upgrade"
        self.assert_invalid(result, "failure stage")
        result = receipt()
        c = result["upgrade_observation"]["connections"][0]
        c["negotiations"][1]["io_failed"] = True
        event(c, "muxer", "muxer_poll_error", detail={"error": "synthetic closed connection",
                                                    "failure_stage": "post_upgrade"})
        self.assert_invalid(result, "target connection upgrade failed")

    def test_guard_drop_and_partial_failed_prefix_remain_background_history(self):
        result = receipt()
        failed = background(result, "")
        failed["events"] = []
        failed["negotiations"][0].update(proposed_protocol=None, io_failed=False,
                                           parser_error="invalid negotiation frame length")
        event(failed, "security", "upgrade_future_dropped_before_completion", detail={})
        self.assert_valid(result)
        failed["negotiations"][0]["parser_error"] = "acknowledgement disagrees with proposal"
        self.assert_invalid(result, "consistent failed replay")

    def test_missing_duplicate_and_forged_upgrade_markers_rejected(self):
        for mutation in ("missing", "duplicate", "snapshot", "delegate", "protocol"):
            with self.subTest(mutation=mutation):
                result = receipt()
                c = result["upgrade_observation"]["connections"][0]
                marker = next(e for e in c["events"] if e["kind"] == "upgrade_completed")
                if mutation == "missing":
                    c["events"].remove(marker)
                    renumber(c)
                elif mutation == "duplicate":
                    c["events"].insert(c["events"].index(marker), copy.deepcopy(marker))
                    renumber(c)
                elif mutation == "snapshot":
                    c["negotiations"][0]["upgrade_completed_sequence"] = True
                elif mutation == "delegate":
                    c["security_delegate_completed"] = False
                else:
                    marker["detail"]["protocol"] = "/tls/1.0.0"
                self.assert_invalid(result)

    def test_automatic_identify_is_excluded_only_by_causal_boundary(self):
        result = receipt()
        app = result["upgrade_observation"]["applications"]["attempts"][0]
        self.assert_valid(result)
        app["preexisting_raw_streams"] = []
        self.assert_invalid(result, "ambiguous raw framed I/O")

    def test_failed_matching_raw_stream_cannot_be_discarded(self):
        result = receipt()
        c = result["upgrade_observation"]["connections"][0]
        raw = bytes.fromhex(result["raw_identify_exchange"]["raw_protobuf_hex"])
        second = raw_stream(c, IDENTIFY, read=raw)
        second["io_failed"] = True
        event(c, "application", "read_error", second["stream_trace_id"],
              detail={"error": "synthetic read failure", "failure_stage": "post_upgrade"})
        self.assert_invalid(result, "ambiguous raw framed I/O")

    def test_binding_ids_peer_address_and_remote_receipt_are_recomputed(self):
        for key, value in (("connection_trace_id", True), ("stream_trace_id", 1),
                           ("swarm_connection_id", "1"), ("authenticated_remote_peer_id", OTHER),
                           ("local_address", REMOTE_ADDR), ("remote_receipt_claimed", True)):
            with self.subTest(key=key):
                result = receipt()
                result["upgrade_observation"]["applications"]["attempts"][0]["binding"][key] = value
                self.assert_invalid(result)

    def test_malformed_or_noncausal_before_counts_rejected(self):
        for value in (True, -1, 2, 99, "1"):
            with self.subTest(value=value):
                result = receipt()
                result["upgrade_observation"]["applications"]["attempts"][0]["preexisting_raw_streams"][0]["stream_count"] = value
                self.assert_invalid(result)

    def test_echo_requires_identify_then_distinct_same_connection_binding(self):
        for mutation in ("missing_identify", "reverse", "other_swarm", "same_stream", "early_echo", "missing_pair"):
            with self.subTest(mutation=mutation):
                result = receipt("echo")
                proof = result["upgrade_observation"]
                apps = proof["applications"]["attempts"]
                if mutation == "missing_identify":
                    del apps[0]
                elif mutation == "reverse":
                    apps.reverse()
                elif mutation == "other_swarm":
                    apps[1]["binding"]["swarm_connection_id"] = "18"
                elif mutation == "same_stream":
                    proof["application_pair_binding"]["echo_stream_trace_id"] = 2
                elif mutation == "early_echo":
                    apps[1]["preexisting_raw_streams"][0]["stream_count"] = 1
                else:
                    del proof["application_pair_binding"]
                self.assert_invalid(result)

    def test_echo_pair_receipt_and_payload_cannot_replace_observation(self):
        for path, value in ((["payload_bytes"], 4), (["echo_ok"], False),
                            (["upgrade_observation", "application_pair_binding", "connection_trace_id"], True),
                            (["upgrade_observation", "application_pair_binding_error"], "failed")):
            with self.subTest(path=path):
                result = receipt("echo")
                replace(result, path, value)
                self.assert_invalid(result)

    def test_response_marker_requires_exact_written_frame_and_same_stream(self):
        for mutation in ("missing", "hash", "stream", "sequence", "protocol"):
            with self.subTest(mutation=mutation):
                result = receipt("echo")
                c = result["upgrade_observation"]["connections"][0]
                marker = c["events"][-1]
                if mutation == "missing":
                    c["events"].pop()
                elif mutation == "hash":
                    marker["detail"]["write"]["framed_sha256"] = "00" * 32
                elif mutation == "stream":
                    marker["stream_trace_id"] = 2
                elif mutation == "sequence":
                    c["streams"][2]["response_completed_sequence"] = 1
                else:
                    marker["detail"]["protocol"] = IDENTIFY
                self.assert_invalid(result)

    def test_selected_without_local_completion_or_drop_is_not_proof(self):
        for path in (("applications", "attempts", 1, "stream_drop_returned"),
                     ("applications", "attempts", 1, "application_io_complete"),
                     ("connections", 0, "streams", 2, "drop_observed"),
                     ("connections", 0, "streams", 2, "write_close_returned")):
            with self.subTest(path=path):
                result = receipt("echo")
                replace(result["upgrade_observation"], path, False)
                self.assert_invalid(result)

    def test_raw_identify_capture_must_match_complete_framed_read(self):
        for key, value in (("raw_protobuf_hex", "00"), ("raw_protobuf_sha256", "00" * 32),
                           ("raw_protobuf_bytes", True), ("raw_capture_truncated", True),
                           ("authenticated_remote_peer_id", OTHER), ("status", "failed")):
            with self.subTest(key=key):
                result = receipt()
                result["raw_identify_exchange"][key] = value
                self.assert_invalid(result)
        result = receipt()
        del result["raw_identify_exchange"]
        self.assert_invalid(result)

    def test_identify_semantic_validation_is_explicit_caller_composition(self):
        result = receipt()
        # No envelope/key parser is duplicated here. These bytes and even a
        # claimed status cannot pass acceptance without the semantic validator.
        result["raw_identify_exchange"]["signed_peer_record_verified"] = False
        self.assert_valid(result)
        self.assertFalse(result["raw_identify_exchange"]["signed_peer_record_verified"])
        result["signed_peer_record"] = True
        self.assert_invalid(result, "Behaviour Identify flag")

    def test_cleanup_overflow_terminal_and_native_ids_are_strict(self):
        changes = [(["fixture_task_lifecycle", "fixture_owned_tasks_joined"], False),
                   (["fixture_task_lifecycle", "errors"], ["join failed"]),
                   (["fixture_task_lifecycle", "tasks", 0, "terminal"], "panicked"),
                   (["fixture_task_lifecycle", "tasks", 0, "task_id"], "029"),
                   (["upgrade_observation", "overflow"], True),
                   (["upgrade_observation", "connections", 0, "overflow"], True),
                   (["upgrade_observation", "fixture_owned_tasks_joined"], False),
                   (["upgrade_observation", "swarm_events", 0, "swarm_connection_id"], 17),
                   (["upgrade_observation", "connections", 0, "connection_trace_id"], True),
                   (["upgrade_observation", "connections", 0, "events", 0, "sequence"], True)]
        for path, value in changes:
            with self.subTest(path=path):
                result = receipt()
                replace(result, path, value)
                self.assert_invalid(result)

    def test_owner_cancel_requires_join_and_abort_request(self):
        result = receipt()
        task = result["fixture_task_lifecycle"]["tasks"][0]
        task["terminal"] = "cancelled_by_owner"
        self.assert_valid(result)
        task["abort_requested"] = False
        self.assert_invalid(result, "task was not joined")

    def test_bounded_counts_and_body_types_are_not_truthy_coercions(self):
        for key, value in (("framed_bytes", True), ("frames", -1), ("framed_sha256", "FF" * 32),
                           ("complete_frames", 1), ("invalid_or_over_limit", True)):
            with self.subTest(key=key):
                result = receipt()
                result["upgrade_observation"]["applications"]["attempts"][0]["read"][key] = value
                self.assert_invalid(result)
        result = receipt()
        c = result["upgrade_observation"]["connections"][0]
        c["events"] = [copy.deepcopy(c["events"][0]) for _ in range(257)]
        self.assert_invalid(result, "event bound")

    def test_swarm_peer_direction_endpoint_and_duplicates_rejected(self):
        for mutation in ("peer", "direction", "address", "duplicate"):
            with self.subTest(mutation=mutation):
                result = receipt()
                events = result["upgrade_observation"]["swarm_events"]
                if mutation == "peer":
                    events[0]["authenticated_remote_peer_id"] = OTHER
                elif mutation == "direction":
                    events[0]["endpoint"]["direction"] = "inbound"
                elif mutation == "address":
                    events[0]["endpoint"]["remote_address"] = LOCAL_ADDR
                else:
                    events.append(copy.deepcopy(events[0]))
                self.assert_invalid(result)

    def test_malformed_nested_shapes_return_errors_not_exceptions(self):
        for value in (None, [], True, "json"):
            with self.subTest(value=value):
                self.assert_invalid(value)
                result = receipt()
                result["upgrade_observation"]["connections"][0]["streams"][1]["read"] = value
                self.assert_invalid(result)


if __name__ == "__main__":
    unittest.main()
