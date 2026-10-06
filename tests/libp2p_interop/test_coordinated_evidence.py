"""Synthetic validator-boundary inputs only; never live socket/wire artifacts."""

from copy import deepcopy
import hashlib
import unittest

import coordinated_evidence as evidence
from coordinated_network import CoordinatedNetwork
from test_coordinated_network import TCPCommands

TOKEN = "a" * 32


def ready(implementation, role):
    ip, port = ("11.0.0.1", 40010) if role == "source" else ("11.0.0.2", 40020)
    peer = f"unit-{role}-peer"
    address = f"/ip4/{ip}/tcp/{port}"
    return {"implementation": implementation, "status": "ready", "case_token": TOKEN, "peer_id": peer,
            "listener_address": address, "listener_port": port, "listen_addrs": [f"{address}/p2p/{peer}"],
            "preexisting_connection_ids": []}


def body():
    payload = evidence.challenge(TOKEN)
    frame = bytes([len(payload)]) + payload
    return {"framed_bytes": len(frame), "framed_sha256": hashlib.sha256(frame).hexdigest(), "frames": 1,
            "complete_frames": True, "invalid_or_over_limit": False}


def go_result(role="source"):
    local, remote = ready("go", role), ready("forge", "destination" if role == "source" else "source")
    actual = "initiator" if role == "source" else "responder"
    frame = {"framed_hex": (bytes([len(evidence.challenge(TOKEN))]) + evidence.challenge(TOKEN)).hex(),
             "raw": False, "read": body()}
    connection = {"source": "go-libp2p.network.Conn.ConnState", "connection_id": "native-1",
                  "local_peer_id": local["peer_id"], "remote_peer_id": remote["peer_id"],
                  "local_address": local["listener_address"], "remote_address": remote["listener_address"],
                  "transport": "tcp", "security": "/noise", "muxer": "/yamux/1.0.0"}
    local_tuple, remote_tuple = evidence._endpoint(local["listener_address"]), evidence._endpoint(remote["listener_address"])
    return {"schema_version": 1, "implementation": "go", "local_peer_id": local["peer_id"], "case_token": TOKEN,
            "actor_role": actual, "status": "ok", "joined": True,
            "native_dial_joined": True, "operation_admitted": True, "admission_source": "coordinated_actor.preflight",
            "connections_after_stop": 0, "receipt": {"connection": connection, "native_outgoing_winner": True,
            "physical_direction": "outbound", "direction_source": "go-libp2p.swarm.Conn.Stat",
            "roles": {"security_role": actual, "yamux_role": actual},
            "role_source": "security.SecureInbound/SecureOutbound+yamux.NewConn",
            "native_dial_sockets": [{"source": "net.Dialer.DialContext.returned-socket",
                "local": f"{local_tuple[0]}:{local_tuple[1]}", "remote": f"{remote_tuple[0]}:{remote_tuple[1]}",
                "simultaneous_connect": True, "is_client": role == "source", "reason": TOKEN}],
            "application": {"protocol": evidence.ECHO_PROTOCOL, "connection_id": "native-1", "fresh_dial": False,
                            "request": deepcopy(frame), "response": deepcopy(frame)}}}


def rust_result(role="source"):
    local, remote = ready("rust", role), ready("forge", "destination" if role == "source" else "source")
    upgrade = "outbound" if role == "source" else "inbound"
    endpoint = {"direction": "outbound", "upgrade_role": upgrade, "remote_address": remote["listener_address"]}
    stream = {"stream_trace_id": 1, "direction": upgrade, "protocol": evidence.ECHO_PROTOCOL, "io_failed": False,
              "parser_error": None, "write_close_returned": True, "drop_observed": True, "read": body(), "write": body()}
    raw_connection = {"connection_trace_id": 1, "authenticated_local_peer_id": local["peer_id"],
                      "authenticated_remote_peer_id": remote["peer_id"], "local_address": local["listener_address"],
                      "remote_address": remote["listener_address"], "direction": "outbound", "endpoint": endpoint,
                      "selected_security": "/noise", "selected_muxer": "/yamux/1.0.0", "security_complete": True,
                      "muxer_complete": True, "security_delegate_completed": True, "muxer_delegate_completed": True,
                      "overflow": False, "events": [], "streams": [stream],
                      "negotiations": [{"direction": upgrade, "io_failed": False, "parser_error": None} for _ in range(2)]}
    raw_connection["transport_output_receipts"] = [{"basis": "donor_transport_output_identity", "connection_trace_id": 1,
        "after_event_sequence": 0, "dns_wrapper_enabled": False, "authenticated_remote_peer_id": remote["peer_id"],
        "request_endpoint": deepcopy(endpoint), "resolved_endpoint": deepcopy(endpoint),
        "local_address": local["listener_address"], "remote_address": remote["listener_address"]}]
    socket = {"basis": "unique_native_transport_output_and_swarm_endpoint", "socket_source": "rust.tcp.TcpStream.local_addr_peer_addr",
              "connection_trace_id": 1, "native_connection_id": "swarm-1", "local_address": local["listener_address"],
              "remote_address": remote["listener_address"], "authenticated_local_peer_id": local["peer_id"],
              "authenticated_remote_peer_id": remote["peer_id"], "connection_direction": "outbound", "security": "/noise",
              "muxer": "/yamux/1.0.0", "security_role": "client" if role == "source" else "server",
              "security_role_basis": "completed_native_security_and_muxer_delegates", "raw_upgrade_role": upgrade}
    events = [{"kind": "native_dial_admitted", "expected_peer_id": remote["peer_id"]},
              {"kind": "native_connection_established"},
              {"kind": "application_completed", "native_transport_dials_before": 1, "native_transport_dials_after": 1}]
    for index, row in enumerate(events, 1):
        row.update(sequence=index, mono_ns=index, native_connection_id="swarm-1")
    return {"schema": "forge.p2p.evidence.coordinated.v1", "implementation": "rust", "peer_id": local["peer_id"],
            "timeout_ms": 20000, "per_call_cancel_supported": False,
            "case_token": TOKEN, "coord_role": "initiator" if role == "source" else "responder",
            "status": "ok", "joined": True, "finalized": True, "error": None, "socket": socket,
            "application": {"basis": "unique_retained_connection_and_actual_new_stream_framed_io", "protocol": evidence.ECHO_PROTOCOL,
                "native_connection_id": "swarm-1", "connection_trace_id": 1, "stream_trace_id": 1, "stream_direction": upgrade,
                "write_close_returned": True, "request": body(), "response": body()},
            "native_transport_dials": [{"address": remote["listener_address"], "requested_role": "dialer" if role == "source" else "listener", "port_use": "Reuse"}],
            "events": events, "task_join": {"fixture_owned_tasks_joined": True, "overflow": False, "errors": []},
            "raw_upgrade_observations": {"overflow": False, "fixture_owned_tasks_joined": True, "complete": True,
                "connections": [raw_connection], "swarm_events": [{"swarm_connection_id": "swarm-1",
                    "authenticated_remote_peer_id": remote["peer_id"], "endpoint": endpoint}]}}


def identify_negotiation(identity, direction, protocol="/ipfs/id/1.0.0", sequences=range(1, 6), *, reverse_capture=False):
    """Native observer row shapes; application upgrade markers are never emitted."""
    sequence = list(sequences)
    header = "132f6d756c746973747265616d2f312e302e300a"
    proposal = {"/ipfs/id/1.0.0": "0f2f697066732f69642f312e302e300a",
                "/ipfs/id/push/1.0.0": "142f697066732f69642f707573682f312e302e300a"}[protocol]
    proposer, ack = ("write", "read") if direction == "outbound" else ("read", "write")
    wire = [(proposer, header), (proposer, proposal), (ack, header), (ack, proposal)] if direction == "outbound" \
        else [(proposer, header), (ack, header), (proposer, proposal), (ack, proposal)]
    if reverse_capture:
        wire = [(ack, header), (ack, proposal), (proposer, header), (proposer, proposal)]
    stream = {"stream_trace_id": identity, "protocol": protocol, "proposed_protocol": protocol,
              "direction": direction, "io_failed": False, "parser_error": None,
              "upgrade_completed_sequence": None, "negotiation_complete_frames": True}
    events = [{"sequence": sequence[0], "kind": "substream_opened", "phase": "application",
               "stream_trace_id": identity, "detail": {"direction": direction}}]
    for index, (side, frame) in enumerate(wire, 1):
        event = {"sequence": sequence[index], "kind": "negotiation_frame", "phase": "application",
                 "stream_trace_id": identity, "direction": side, "frame_hex": frame}
        if index == 4:
            event.update(selection_outcome="selected", protocol=protocol)
        events.append(event)
    return stream, events


def rust_cleanup_result(role="source", *, closed=True, stop_first=False, late_identify=False, retained_identify=False):
    """Synthetic receipt boundaries, including the observed 37/38 race shape."""
    value = rust_result(role)
    raw = value["raw_upgrade_observations"]
    raw.update(complete=False)
    connection = raw["connections"][0]
    connection["muxer_drop_observed"] = False
    connection["events"] = [{"sequence": index, "kind": "negotiation_frame", "phase": "security",
                              "stream_trace_id": None, "detail": {}} for index in range(1, 37)]
    connection["events"].append({"sequence": 37, "kind": "response_completion", "phase": "application",
                                  "stream_trace_id": 1, "detail": {"protocol": evidence.ECHO_PROTOCOL, "write": body()}})
    if retained_identify:
        # Copy the three real 4cc negotiation layouts, including interleaving.
        echo = connection["streams"][0]
        echo["stream_trace_id"] = value["application"]["stream_trace_id"] = 4
        connection["events"][-1]["stream_trace_id"] = 4
        connection["streams"] = []
        for identity, direction, protocol, sequences in (
                (1, "outbound", "/ipfs/id/1.0.0", (13, 14, 15, 22, 26)),
                (2, "inbound", "/ipfs/id/1.0.0", (19, 20, 21, 23, 24)),
                (3, "inbound", "/ipfs/id/push/1.0.0", (27, 28, 29, 30, 31))):
            stream, events = identify_negotiation(identity, direction, protocol, sequences)
            connection["streams"].append(stream)
            for event in events:
                connection["events"][event["sequence"] - 1] = event
        connection["streams"].append(echo)
    raw["applications"] = {"overflow": False, "attempts": []}
    if role == "source":
        raw["applications"]["attempts"] = [{"application_trace_id": 1, "authenticated_remote_peer_id": "unit-destination-peer",
            "protocol": evidence.ECHO_PROTOCOL, "direction": "outbound", "opened": True,
            "application_io_complete": True, "attempt_ended": True, "stream_drop_returned": True,
            "write_close_returned": True, "overflow": False, "errors": [], "read": body(), "write": body()}]
        raw["applications"]["attempts"][0]["preexisting_raw_streams"] = [{"connection_trace_id": 1, "stream_count": 3 if retained_identify else 0}]
    before = deepcopy(raw)
    before.pop("fixture_owned_tasks_joined")
    proof = {"source": "rust.coordinated.immutable_completed_application.v1", "application_completed_sequence": 3,
             "captured_unix_ns": 100, "native_connection_id": "swarm-1", "connection_trace_id": 1,
             "raw_event_sequence": 37, "application": deepcopy(value["application"]), "raw_upgrade_observations": before}
    value["completed_proof"] = proof
    if late_identify:
        identity = len(connection["streams"]) + 1
        stream, events = identify_negotiation(identity, "inbound", sequences=range(38, 43))
        connection["streams"].append(stream)
        connection["events"].extend(events)
    if closed:
        connection["events"].append({"sequence": len(connection["events"]) + 1, "kind": "muxer_poll_error", "phase": "muxer",
            "stream_trace_id": None, "detail": {"error": "connection is closed", "failure_stage": "post_upgrade"}})
        connection["negotiations"][1]["io_failed"] = True
    connection["muxer_drop_observed"] = True
    close_sequence, stop_sequence = (5, 4) if stop_first else (4, 5)
    cursor = 37 if stop_first else len(connection["events"])
    stop = {"source": "rust.coordinated.own_stop_file_metadata_and_read.v1", "stop_file": "/unit/source.stop",
            "authorization_basis": "completed_retained_owner_and_actual_stop_read",
            "native_error_ordering": "not_inferred_from_stop_or_filesystem_time",
            "marker_hex": "73746f700a", "modified_unix_ns": 200, "observed_unix_ns": 250 if stop_first else 400,
            "close_observed_unix_ns": None if stop_first else 300, "authenticated_remote_peer_id": value["socket"]["authenticated_remote_peer_id"],
            "native_connection_id": "swarm-1", "connection_trace_id": 1, "application_completed_sequence": 3,
            "raw_event_sequence": cursor, "native_close_sequence": close_sequence, "stop_observed_sequence": stop_sequence}
    close = {"kind": "native_connection_closed", "sequence": close_sequence, "mono_ns": close_sequence,
             "native_connection_id": "swarm-1", "peer_id": value["socket"]["authenticated_remote_peer_id"],
             "remaining_established": 0, "observed_unix_ns": 300, "raw_event_sequence": len(connection["events"]),
             "raw_terminal_event": deepcopy(connection["events"][-1]),
             "cause": {"source": "libp2p.swarm.ConnectionClosed.typed_native_cause", "kind": "yamux013_closed" if closed else "none",
                       "raw": {"display": "connection is closed", "debug": "IO(Custom(Closed))"} if closed else None}}
    stop_event = dict(deepcopy(stop), kind="own_stop_observed", sequence=stop_sequence, mono_ns=stop_sequence)
    if stop_first:
        stop_event["native_close_sequence"] = None
    value["events"].extend(sorted([close, stop_event], key=lambda row: row["sequence"]))
    value["cleanup_stop"] = stop
    return value


def forge_result(role="source"):
    value = go_result(role)
    value["implementation"] = "forge"
    value["receipt"]["connection"]["source"] = "forge.node.diagnostics.authenticated-session"
    value["receipt"]["connection"]["connection_id"] = 1
    value["receipt"]["direction_source"] = "forge.node.diagnostics.session.direction"
    value["receipt"]["role_source"] = "forge.node.diagnostics.session.security_role+yamux_role"
    value["receipt"]["application"].update(connection_id=1, stream_id=1,
        native_dial_attempts_before=0, native_dial_attempts_after=0,
        dial_observation_source="forge.node.metrics.sealed-gater-rejections")
    value["resources"] = {"file_descriptors": 0}
    del value["receipt"]["native_dial_sockets"]
    del value["connections_after_stop"]
    return value


def actor_errors(value, implementation, role="source", profile="native"):
    other = "destination" if role == "source" else "source"
    return evidence._errors(lambda: evidence._actor(value, implementation, TOKEN, ready(implementation, role), ready("forge", other), role, profile))


class ActorEvidenceTests(unittest.TestCase):
    def test_rust_close_and_stop_orders_preserve_typed_raw_error_and_healthy_prefix(self):
        for role in ("source", "destination"):
            for closed in (False, True):
                for stop_first in (False, True):
                    value = rust_cleanup_result(role, closed=closed, stop_first=stop_first)
                    untouched = deepcopy(value)
                    self.assertEqual(actor_errors(value, "rust", role), [])
                    self.assertEqual(value, untouched)
                    self.assertEqual(value["completed_proof"]["raw_event_sequence"], 37)
                    self.assertEqual(value["cleanup_stop"]["raw_event_sequence"], 37 if stop_first or not closed else 38)
                    self.assertEqual(value["raw_upgrade_observations"]["connections"][0]["negotiations"][1]["io_failed"], closed)

    def test_rust_bounded_healthy_late_identify_is_not_a_cleanup_failure(self):
        for role in ("source", "destination"):
            for stop_first in (False, True):
                value = rust_cleanup_result(role, stop_first=stop_first, late_identify=True)
                self.assertEqual(actor_errors(value, "rust", role), [])
                for key, bad in (("io_failed", True), ("parser_error", "bad"), ("protocol", "/unknown/1"),
                                 ("stream_trace_id", 1), ("direction", "outbound"),
                                 ("proposed_protocol", "/ipfs/id/push/1.0.0"), ("negotiation_complete_frames", False),
                                 ("upgrade_completed_sequence", 39)):
                    changed = deepcopy(value)
                    changed["raw_upgrade_observations"]["connections"][0]["streams"][1][key] = bad
                    self.assertTrue(actor_errors(changed, "rust", role), key)
                changed = deepcopy(value)
                changed["raw_upgrade_observations"]["connections"][0]["events"][37]["kind"] = "read_error"
                self.assertTrue(actor_errors(changed, "rust", role))

    def test_rust_retained_identify_and_push_use_real_null_upgrade_marker_layouts(self):
        for role in ("source", "destination"):
            for stop_first in (False, True):
                value = rust_cleanup_result(role, stop_first=stop_first, retained_identify=True)
                untouched = deepcopy(value)
                self.assertEqual(actor_errors(value, "rust", role), [])
                self.assertEqual(value, untouched)
                prefix = value["completed_proof"]["raw_upgrade_observations"]["connections"][0]
                self.assertEqual([s["upgrade_completed_sequence"] for s in prefix["streams"][:3]], [None] * 3)
                self.assertEqual([e["sequence"] for e in prefix["events"]
                                  if e.get("selection_outcome") == "selected"], [24, 26, 31])
                self.assertFalse(any(e["kind"] == "upgrade_completed" and e["phase"] == "application"
                                     for e in prefix["events"]))
                self.assertEqual(value["completed_proof"]["raw_event_sequence"], 37)
                self.assertEqual(value["cleanup_stop"]["raw_event_sequence"], 37 if stop_first else 38)

    def test_rust_identify_and_push_bilateral_frames_cover_both_native_directions(self):
        for protocol in ("/ipfs/id/1.0.0", "/ipfs/id/push/1.0.0"):
            for direction in ("inbound", "outbound"):
                for reverse_capture in (False, True):
                    with self.subTest(protocol=protocol, direction=direction, reverse_capture=reverse_capture):
                        stream, events = identify_negotiation(2, direction, protocol, reverse_capture=reverse_capture)
                        self.assertEqual(evidence._errors(lambda: evidence._rust_identify_negotiation(stream, events)), [])
                        self.assertIsNone(stream["upgrade_completed_sequence"])
                        bad = deepcopy(events)
                        bad[2].update(selection_outcome="selected", protocol=protocol)
                        bad[-1].pop("selection_outcome")
                        bad[-1].pop("protocol")
                        self.assertTrue(evidence._errors(lambda: evidence._rust_identify_negotiation(stream, bad)))
                        invented = events + [{"sequence": 6, "phase": "application", "stream_trace_id": 2,
                                              "kind": "upgrade_completed", "detail": {"protocol": protocol}}]
                        self.assertTrue(evidence._errors(lambda: evidence._rust_identify_negotiation(stream, invented)))

    def test_rust_cleanup_accepts_reversed_native_capture_without_backdating_selection(self):
        for role in ("source", "destination"):
            value = rust_cleanup_result(role, late_identify=True)
            _, events = identify_negotiation(2, "inbound", sequences=range(38, 43), reverse_capture=True)
            value["raw_upgrade_observations"]["connections"][0]["events"][37:42] = events
            untouched = deepcopy(value)
            self.assertEqual(actor_errors(value, "rust", role), [])
            self.assertEqual(value, untouched)
            self.assertEqual(value["cleanup_stop"]["raw_event_sequence"], 43)
            self.assertEqual(value["completed_proof"]["raw_event_sequence"], 37)

    def test_rust_identify_requires_real_ack_and_exact_stream_connection_binding(self):
        for late in (False, True):
            for change in ("missing_ack", "fake_upgrade", "foreign_stream", "foreign_connection", "foreign_peer",
                           "wrong_phase", "wrong_ack", "wrong_direction", "missing_selection", "foreign_protocol",
                           "header_selection"):
                with self.subTest(late=late, change=change):
                    value = rust_cleanup_result(late_identify=late, retained_identify=not late)
                    raw = value["raw_upgrade_observations"]
                    connection = raw["connections"][0]
                    identity = 2 if late else 1
                    ack = next(e for e in connection["events"] if e.get("stream_trace_id") == identity
                               and e.get("selection_outcome") == "selected")
                    if change in ("missing_ack", "fake_upgrade"):
                        ack["kind"] = "response_completion" if change == "missing_ack" else "upgrade_completed"
                        ack["detail"] = {"protocol": "/ipfs/id/1.0.0"}
                        if change == "fake_upgrade":
                            next(s for s in connection["streams"] if s["stream_trace_id"] == identity)["upgrade_completed_sequence"] = ack["sequence"]
                    elif change == "foreign_stream":
                        ack["stream_trace_id"] = 99
                    elif change == "foreign_connection":
                        foreign = deepcopy(connection)
                        foreign["connection_trace_id"] = 2
                        raw["connections"].append(foreign)
                    elif change == "foreign_peer":
                        connection["authenticated_remote_peer_id"] = "foreign-peer"
                    elif change == "wrong_phase":
                        ack["phase"] = "muxer"
                    elif change == "wrong_ack":
                        ack["frame_hex"] = "036e610a"
                    elif change == "wrong_direction":
                        ack["direction"] = "write" if ack["direction"] == "read" else "read"
                    elif change == "missing_selection":
                        ack.pop("selection_outcome")
                    elif change == "foreign_protocol":
                        ack["protocol"] = "/ipfs/id/push/1.0.0"
                    else:
                        header = next(e for e in connection["events"] if e.get("stream_trace_id") == identity
                                      and e["kind"] == "negotiation_frame")
                        header.update(selection_outcome="selected", protocol="/ipfs/id/1.0.0")
                    if not late:
                        # Keep the immutable prefix intact so malformed raw
                        # negotiation, not a snapshot mismatch, is rejected.
                        previous = value["completed_proof"]["raw_upgrade_observations"]
                        previous["connections"][0] = deepcopy(connection)
                        previous["connections"][0]["events"].pop()
                        previous["connections"][0]["negotiations"][1]["io_failed"] = False
                        previous["connections"][0]["muxer_drop_observed"] = False
                    self.assertTrue(actor_errors(value, "rust"), change)

    def test_rust_stop_marker_current_cursor_and_exact_owner_are_mandatory(self):
        for key, bad in (("raw_event_sequence", 37), ("modified_unix_ns", None),
                         ("authorization_basis", "mtime_after_completion"), ("native_error_ordering", "stop_caused_error"),
                         ("marker_hex", ""), ("native_connection_id", "foreign"), ("connection_trace_id", 2),
                         ("authenticated_remote_peer_id", "foreign"), ("native_close_sequence", 3),
                         ("stop_observed_sequence", 4), ("application_completed_sequence", 2)):
            value = rust_cleanup_result()
            value["cleanup_stop"][key] = bad
            value["events"][-1][key] = bad
            self.assertTrue(actor_errors(value, "rust"), (key, bad))
        value = rust_cleanup_result()
        value.pop("cleanup_stop")
        self.assertTrue(actor_errors(value, "rust"))

    def test_rust_filesystem_mtime_is_preserved_but_never_causal_authority(self):
        for stop_first in (False, True):
            for modified in (99, 301, 999):
                value = rust_cleanup_result(stop_first=stop_first)
                value["cleanup_stop"]["modified_unix_ns"] = modified
                stop_event = next(e for e in value["events"] if e["kind"] == "own_stop_observed")
                stop_event["modified_unix_ns"] = modified
                untouched = deepcopy(value)
                self.assertEqual(actor_errors(value, "rust"), [])
                self.assertEqual(value, untouched)
                self.assertEqual(value["completed_proof"]["raw_event_sequence"], 37)
                self.assertEqual(value["cleanup_stop"]["raw_event_sequence"], 37 if stop_first else 38)

    def test_rust_typed_closed_does_not_authorize_arbitrary_io_or_filter_raw_flags(self):
        for change in ("cause", "display_only", "io_flag", "raw_cause", "different_index", "two_errors", "early_error", "foreign_peer", "foreign_owner"):
            value = rust_cleanup_result()
            raw = value["raw_upgrade_observations"]["connections"][0]
            close = value["events"][3]
            if change == "cause":
                close["cause"]["kind"] = "other"
            elif change == "display_only":
                close["cause"].pop("source")
            elif change == "io_flag":
                raw["negotiations"][1]["io_failed"] = False
            elif change == "raw_cause":
                close["cause"]["raw"] = None
            elif change == "different_index":
                close["raw_terminal_event"]["sequence"] = 37
            elif change == "two_errors":
                raw["events"].append(dict(deepcopy(raw["events"][-1]), sequence=39))
                close["raw_event_sequence"] = 39
                close["raw_terminal_event"] = deepcopy(raw["events"][-1])
                value["cleanup_stop"]["raw_event_sequence"] = value["events"][-1]["raw_event_sequence"] = 39
            elif change == "early_error":
                error = {"sequence": 1, "kind": "read_error", "phase": "security", "stream_trace_id": None,
                         "detail": {"error": "native failure", "failure_stage": "post_upgrade"}}
                raw["events"][0] = deepcopy(error)
                value["completed_proof"]["raw_upgrade_observations"]["connections"][0]["events"][0] = error
            elif change == "foreign_peer":
                close["peer_id"] = "foreign"
            else:
                close["native_connection_id"] = "foreign"
            self.assertTrue(actor_errors(value, "rust"), change)

    def test_rust_own_stop_never_waives_application_join_or_native_cleanup_error(self):
        for change in ("app_incomplete", "app_error", "old_stream", "echo_changed", "join_error", "join_deadline", "native_drop", "actor_error"):
            value = rust_cleanup_result()
            if change == "app_incomplete":
                value["completed_proof"]["raw_upgrade_observations"]["applications"]["attempts"][0]["application_io_complete"] = False
            elif change == "app_error":
                value["completed_proof"]["raw_upgrade_observations"]["applications"]["attempts"][0]["errors"] = ["write"]
            elif change == "old_stream":
                value["completed_proof"]["raw_upgrade_observations"]["applications"]["attempts"][0]["preexisting_raw_streams"][0]["stream_count"] = 1
            elif change == "echo_changed":
                value["raw_upgrade_observations"]["connections"][0]["streams"][0]["read"]["framed_sha256"] = "0" * 64
            elif change == "join_error":
                value["task_join"]["errors"] = ["native worker failure"]
            elif change == "join_deadline":
                value["task_join"]["fixture_owned_tasks_joined"] = False
            elif change == "native_drop":
                value["raw_upgrade_observations"]["connections"][0]["muxer_drop_observed"] = False
            else:
                value["events"][3]["error"] = "actual cleanup failure"
            self.assertTrue(actor_errors(value, "rust"), change)

    def test_forge_completed_delegate_roles_and_actual_framed_io(self):
        for role in ("source", "destination"):
            self.assertEqual(actor_errors(forge_result(role), "forge", role), [])

    def test_go_actual_context_and_both_handshake_roles(self):
        for role in ("source", "destination"):
            self.assertEqual(actor_errors(go_result(role), "go", role), [])

    def test_rust_outgoing_transport_can_have_inbound_upgrade_role(self):
        for role in ("source", "destination"):
            self.assertEqual(actor_errors(rust_result(role), "rust", role), [])

    def test_rust_native_identify_does_not_replace_unique_echo(self):
        for role in ("source", "destination"):
            value = rust_result(role)
            streams = value["raw_upgrade_observations"]["connections"][0]["streams"]
            streams.extend({"stream_trace_id": index, "protocol": protocol,
                            "io_failed": False, "parser_error": None}
                           for index, protocol in enumerate(("/ipfs/id/1.0.0", "/ipfs/id/push/1.0.0"), 2))
            self.assertEqual(actor_errors(value, "rust", role), [])
            for field, bad in (("protocol", "/unknown/1"), ("io_failed", True),
                               ("parser_error", "invalid"), ("stream_trace_id", 1)):
                changed = deepcopy(value)
                changed["raw_upgrade_observations"]["connections"][0]["streams"][1][field] = bad
                self.assertTrue(actor_errors(changed, "rust", role))
            for retained in (streams[1:], [*streams, deepcopy(streams[0])]):
                changed = deepcopy(value)
                changed["raw_upgrade_observations"]["connections"][0]["streams"] = retained
                self.assertTrue(actor_errors(changed, "rust", role))
            changed = deepcopy(value)
            duplicate = deepcopy(streams[0])
            duplicate["stream_trace_id"] = 4
            changed["raw_upgrade_observations"]["connections"][0]["streams"].append(duplicate)
            self.assertTrue(actor_errors(changed, "rust", role))

    def test_go_rejects_ephemeral_socket_or_wrong_remote(self):
        for field in ("local_address", "remote_address"):
            value = go_result()
            value["receipt"]["connection"][field] = "/ip4/11.0.0.1/tcp/45000"
            self.assertTrue(actor_errors(value, "go"))

    def test_source_accepted_socket_cannot_be_outgoing_winner(self):
        for field, bad in (("native_outgoing_winner", False), ("physical_direction", "inbound")):
            value = go_result()
            value["receipt"][field] = bad
            self.assertTrue(actor_errors(value, "go"))

    def test_requested_role_is_not_observed_security_or_yamux_role(self):
        for field in ("security_role", "yamux_role"):
            value = go_result()
            value["receipt"]["roles"][field] = "responder"
            self.assertTrue(actor_errors(value, "go"))
        value = go_result()
        value["receipt"]["role_source"] = "coordinator_requested_role"
        self.assertTrue(actor_errors(value, "go"))

    def test_go_native_dial_context_must_be_simultaneous_and_identity_bound(self):
        for field, bad in (("simultaneous_connect", False), ("is_client", False), ("reason", "b" * 32), ("source", "configured_socket")):
            value = go_result()
            value["receipt"]["native_dial_sockets"][0][field] = bad
            self.assertTrue(actor_errors(value, "go"))

    def test_probe_cannot_redial_or_replace_connection(self):
        for field, bad in (("fresh_dial", True), ("connection_id", "native-2")):
            value = go_result()
            value["receipt"]["application"][field] = bad
            self.assertTrue(actor_errors(value, "go"))

    def test_forge_no_redial_proof_uses_native_gater_not_path_selection_metrics(self):
        for key, bad in (("native_dial_attempts_before", 1), ("native_dial_attempts_after", 1),
                         ("dial_observation_source", "forge.node.metrics.path_direct_attempts")):
            value = forge_result()
            value["receipt"]["application"][key] = bad
            self.assertTrue(actor_errors(value, "forge"))

    def test_wrong_challenge_or_manufactured_frame_summary_fails(self):
        for field, bad in (("framed_bytes", 0), ("frames", 0), ("complete_frames", False), ("framed_sha256", "0" * 64)):
            value = go_result()
            value["receipt"]["application"]["request"]["read"][field] = bad
            self.assertTrue(actor_errors(value, "go"))
        value = go_result()
        value["receipt"]["application"]["request"]["framed_hex"] = b"coordinated:old-case".hex()
        self.assertTrue(actor_errors(value, "go"))

    def test_unjoined_workers_or_forced_cleanup_is_not_success(self):
        for field in ("joined", "native_dial_joined"):
            value = go_result()
            value[field] = False
            self.assertTrue(actor_errors(value, "go"))
        value = rust_result()
        value["task_join"]["fixture_owned_tasks_joined"] = False
        self.assertTrue(actor_errors(value, "rust"))

    def test_rust_requested_role_does_not_override_actual_upgrade(self):
        value = rust_result("destination")
        value["raw_upgrade_observations"]["connections"][0]["negotiations"][0]["direction"] = "outbound"
        self.assertTrue(actor_errors(value, "rust", "destination"))

    def test_rust_requires_actual_socket_and_transport_output_correlation(self):
        for mutation in ("socket", "output", "duplicate", "peer"):
            value = rust_result()
            connection = value["raw_upgrade_observations"]["connections"][0]
            if mutation == "socket":
                connection["local_address"] = "/ip4/11.0.0.1/tcp/45000"
            elif mutation == "output":
                connection["transport_output_receipts"] = []
            elif mutation == "duplicate":
                value["raw_upgrade_observations"]["connections"].append(deepcopy(connection))
            else:
                connection["authenticated_remote_peer_id"] = "wrong-peer"
            self.assertTrue(actor_errors(value, "rust"))

    def test_rust_requires_actual_stream_digests_and_no_replacement_dial(self):
        value = rust_result()
        value["raw_upgrade_observations"]["connections"][0]["streams"][0]["write"]["framed_sha256"] = "0" * 64
        self.assertTrue(actor_errors(value, "rust"))
        value = rust_result()
        value["native_transport_dials"].append(deepcopy(value["native_transport_dials"][0]))
        self.assertTrue(actor_errors(value, "rust"))

    def test_private_metadata_is_not_installed_key_proof(self):
        value = rust_result()
        value["pnet_fingerprint_basis"] = "coordinator_input_metadata"
        self.assertTrue(actor_errors(value, "rust", profile="private"))

    def test_rust_private_actor_requires_checked_installed_protector_basis(self):
        value = rust_result()
        value["pnet_fingerprint_basis"] = "installed_native_pnet_psk_operational_sha256_v1"
        self.assertEqual(actor_errors(value, "rust", profile="private"), [])
        for basis in (None, "requested_psk", "coordinator_input_metadata"):
            value["pnet_fingerprint_basis"] = basis
            self.assertTrue(actor_errors(value, "rust", profile="private"))

    def test_null_forge_actual_roles_fail_closed(self):
        value = forge_result()
        value["receipt"]["roles"] = None
        self.assertTrue(actor_errors(value, "forge"))
        value = forge_result()
        value["receipt"]["role_source"] = "requested_role"
        self.assertTrue(actor_errors(value, "forge"))

    def test_start_ack_is_not_connection_or_application_evidence(self):
        value = go_result()
        value["status"], value["receipt"] = "started", None
        self.assertTrue(evidence.started_receipt(value, "go", TOKEN))
        self.assertTrue(evidence.validate_connected(value, "go", TOKEN, ready("go", "source"), ready("forge", "destination"), "source"))

    def test_bounded_schema_and_bool_ports(self):
        value = ready("go", "source")
        value["listener_port"] = True
        self.assertTrue(evidence.validate_ready(value, "go", TOKEN, "11.0.0.1"))
        for value in (None, [], {"implementation": "go"}):
            self.assertTrue(evidence.validate_ready(value, "go", TOKEN, "11.0.0.1"))

    def test_native_listener_peer_suffix_is_bound_to_its_local_identity(self):
        value = ready("forge", "source")
        address = value["listener_address"]
        value["listener_address"] = address + "/p2p/" + value["peer_id"]
        self.assertEqual(evidence.validate_ready(value, "forge", TOKEN, "11.0.0.1"), [])
        value["listener_address"] = address + "/p2p/other-peer"
        self.assertTrue(evidence.validate_ready(value, "forge", TOKEN, "11.0.0.1"))


class BarrierEvidenceTests(unittest.TestCase):
    def artifact(self):
        commands = TCPCommands()
        network = CoordinatedNetwork(command_runner=commands, system=lambda: "Linux", ip_lookup=lambda n: n,
                                     outer_namespace_isolated=lambda: True, namespace_token="unit")
        network.setup()
        network.arm({"client": 40010, "server": 40020})
        commands.sockets[network.namespaces["client"]] = "0 1 11.0.0.1:40010 11.0.0.2:40020\n"
        commands.sockets[network.namespaces["server"]] = "0 1 11.0.0.2:40020 11.0.0.1:40010\n"
        network.release()
        network.close()
        return {"network": network.evidence(), "raw": {role: {"ready": ready("go", role)} for role in ("source", "destination")}}

    def test_raw_owned_kernel_snapshots_and_exact_rule_lifetime(self):
        self.assertEqual(evidence.validate_barrier(self.artifact()), [])

    def test_counter_or_requested_port_without_raw_ss_is_rejected(self):
        for mutation in ("raw", "port", "released", "command"):
            artifact = self.artifact()
            coordination = artifact["network"]["coordination"]
            if mutation == "raw":
                coordination["syn_sent"]["client"]["stdout"] = ""
            elif mutation == "port":
                coordination["ports"]["client"] = 45000
            elif mutation == "released":
                coordination["released"] = False
            else:
                artifact["network"]["commands"] = []
            self.assertTrue(evidence.validate_barrier(artifact))

    def test_duplicate_socket_or_unrelated_tuple_is_rejected(self):
        for mutation in ("duplicate", "ephemeral"):
            artifact = self.artifact()
            snapshot = artifact["network"]["coordination"]["syn_sent"]["client"]
            snapshot["stdout"] = snapshot["stdout"] * 2 if mutation == "duplicate" else "0 1 11.0.0.1:45000 11.0.0.2:40020\n"
            self.assertTrue(evidence.validate_barrier(artifact))

    def test_release_requires_two_fresh_command_snapshots_before_first_delete(self):
        artifact = self.artifact()
        commands = artifact["network"]["commands"]
        first_delete = next(i for i, row in enumerate(commands) if "-D" in row["command"])
        commands[first_delete - 1]["stdout"] = ""
        self.assertTrue(evidence.validate_barrier(artifact))


def bilateral_artifact(source="forge", destination="go", profile="native"):
    """Complete synthetic validation vector; it cannot establish native interop."""
    artifact = BarrierEvidenceTests().artifact()
    scenario = "coordinated_dial_port_reuse" + ("_private_pnet" if profile == "private" else "")
    identifier = f"coordinated.{profile}.{source}_to_{destination}"
    transport = "tcp" if profile == "native" else "tcp-pnet-noise"
    work = f"/unit/{identifier}"
    artifact.update(schema_version=1, suite="coordinated", case={"identifier": identifier, "source": source,
                    "destination": destination, "profile": profile, "transport": transport},
                    scenario=scenario, scenario_id=identifier, runner_scenario_id=evidence.REUSE_RUNNER_IDS[profile],
                    acceptance_scenario_ids=[scenario], case_token=TOKEN, pnet_fingerprint="b" * 64 if profile == "private" else None,
                    errors=[], cleanup_errors=[], status="passed", phases={"started": {}, "connected": {}, "exchanged": {}},
                    plans={}, controls=[], processes=[], elapsed_seconds=1)
    for index, (role, implementation) in enumerate((("source", source), ("destination", destination)), 1):
        other = "destination" if role == "source" else "source"
        listener = ready(implementation, role)
        result = {"forge": forge_result, "go": go_result, "rust": rust_result}[implementation](role)
        result["scenario"] = scenario
        result["pnet_fingerprint"] = artifact["pnet_fingerprint"]
        connected, exchanged = deepcopy(result), deepcopy(result)
        connected["status"], exchanged["status"] = "connected", "exchanged"
        connected["joined"], exchanged["joined"] = False, False
        if implementation == "rust":
            connected["application"] = None
        else:
            connected["receipt"].pop("application")
        artifact["phases"]["connected"][role] = connected
        artifact["phases"]["exchanged"][role] = exchanged
        artifact["phases"]["started"][role] = {"implementation": implementation, "case_token": TOKEN, "status": "started",
                    "operation_admitted": True, "admission_source": "coordinated_actor.preflight",
                    "events": [{"kind": "control_completed", "completion": "native_dial_admitted"}]}
        network_role = "client" if role == "source" else "server"
        namespace = next(p["namespace"] for p in artifact["network"]["participants"] if p["role"] == network_role)
        command = ["/unit/ip", "netns", "exec", namespace, f"/unit/{implementation}", "coordinated-live", "--scenario", scenario,
                   "--transport", transport, "--coord-role", "initiator" if role == "source" else "responder", "--case-token", TOKEN,
                   "--bind-ip", "11.0.0.1" if role == "source" else "11.0.0.2", "--timeout-ms", "20000"]
        for name in ("ready", "result", "stop", "control", "plan"):
            command += [f"--{name}-file", f"{work}/{role}.{name}"]
        if implementation == "forge":
            command += ["--store-dir", f"{work}/{role}.store"]
        if profile == "private":
            command += ["--pnet-key-file", "/unit/psk", "--pnet-fingerprint", "b" * 64]
        owner = {"pid": index, "command": command, "log_file": f"{work}/{role}.log", "ready": listener, "outputs": [],
                 "terminal_status": {"exit_code": 0, "termination": "graceful"},
                 "stop_budget": {"native_close_seconds": 8, "post_stop_seconds": 0, "scheduler_allowance_seconds": 2, "seconds": 10}}
        artifact["raw"][role] = {"ready": listener, "result": result, "process": owner}
        artifact["processes"].append(owner)
        target = ready(destination if role == "source" else source, other)
        artifact["plans"][role] = {"case-token": TOKEN, "peer-id": target["peer_id"], "addr": target["listen_addrs"][0]}
        artifact["controls"].append({"actor": role, "sequence": "1", "action": "start", "case-token": TOKEN})
    artifact["controls"].append({"actor": "source", "sequence": "2", "action": "probe", "case-token": TOKEN})
    return artifact


class CaseEvidenceTests(unittest.TestCase):
    def cleanup_artifact(self, role="source", **options):
        source, destination = ("rust", "forge") if role == "source" else ("forge", "rust")
        artifact = bilateral_artifact(source, destination, "private")
        value = rust_cleanup_result(role, **options)
        value.update(scenario=artifact["scenario"], pnet_fingerprint=artifact["pnet_fingerprint"],
                     pnet_fingerprint_basis="installed_native_pnet_psk_operational_sha256_v1")
        path = f"/unit/{artifact['case']['identifier']}/{role}.stop"
        value["cleanup_stop"]["stop_file"] = path
        next(e for e in value["events"] if e["kind"] == "own_stop_observed")["stop_file"] = path
        artifact["raw"][role]["result"] = value
        exchanged = deepcopy(value)
        exchanged.update(status="exchanged", joined=False, finalized=False, cleanup_stop=None,
                         events=deepcopy(value["events"][:3]),
                         raw_upgrade_observations=deepcopy(value["completed_proof"]["raw_upgrade_observations"]))
        artifact["phases"]["exchanged"][role] = exchanged
        return artifact

    def test_private_rust_observed_cleanup_keeps_complete_exchanged_proof_separate(self):
        for role in ("source", "destination"):
            for stop_first in (False, True):
                artifact = self.cleanup_artifact(role, stop_first=stop_first, late_identify=True)
                self.assertEqual(evidence.validate_case(artifact), [])

    def test_rust_cleanup_cannot_invent_exchanged_proof_or_observe_foreign_stop(self):
        for change in ("snapshot", "missing", "actor_prefix", "before_complete", "foreign_stop"):
            artifact = self.cleanup_artifact()
            value = artifact["raw"]["source"]["result"]
            if change == "snapshot":
                value["completed_proof"]["captured_unix_ns"] = 101
            elif change == "missing":
                artifact["phases"]["exchanged"]["source"].pop("completed_proof")
            elif change == "actor_prefix":
                artifact["phases"]["exchanged"]["source"]["events"][2]["mono_ns"] = 2
            elif change == "before_complete":
                artifact["phases"]["exchanged"]["source"]["events"].pop()
            else:
                value["cleanup_stop"]["stop_file"] = "/unit/foreign.stop"
                value["events"][-1]["stop_file"] = "/unit/foreign.stop"
            self.assertTrue(evidence.validate_case(artifact), change)

    def test_typed_rust_cleanup_does_not_authorize_nonzero_or_forced_process_exit(self):
        for terminal in ({"exit_code": 1, "termination": "graceful"}, {"exit_code": 0, "termination": "killed"}):
            artifact = self.cleanup_artifact()
            artifact["raw"]["source"]["process"]["terminal_status"] = terminal
            self.assertTrue(evidence.validate_case(artifact))

    def test_canonical_native_and_private_go_pair_synthetic_vectors(self):
        for source, destination in (("forge", "go"), ("go", "forge"), ("forge", "rust"), ("rust", "forge")):
            self.assertEqual(evidence.validate_case(bilateral_artifact(source, destination)), [])
        for source, destination in (("forge", "go"), ("go", "forge")):
            self.assertEqual(evidence.validate_case(bilateral_artifact(source, destination, "private")), [])

    def test_host_stop_does_not_manufacture_application_success(self):
        artifact = bilateral_artifact()
        artifact["phases"]["exchanged"]["source"] = deepcopy(artifact["phases"]["connected"]["source"])
        self.assertTrue(evidence.validate_case(artifact))

    def test_probe_replacement_is_rejected_even_if_its_final_receipt_is_valid(self):
        artifact = bilateral_artifact()
        artifact["phases"]["connected"]["source"]["receipt"]["connection"]["connection_id"] = 2
        self.assertTrue(evidence.validate_case(artifact))

    def test_metadata_only_rust_private_case_is_not_promoted(self):
        artifact = bilateral_artifact("forge", "rust", "private")
        artifact["raw"]["destination"]["result"]["pnet_fingerprint_basis"] = "coordinator_input_metadata"
        self.assertTrue(evidence.validate_case(artifact))

    def test_fingerprint_mismatch_or_private_tls_fails(self):
        for key, bad in (("pnet_fingerprint", "c" * 64), ("security", "/tls/1.0.0")):
            artifact = bilateral_artifact(profile="private")
            if key == "security":
                artifact["raw"]["source"]["result"]["receipt"]["connection"][key] = bad
            else:
                artifact["raw"]["source"]["result"][key] = bad
            self.assertTrue(evidence.validate_case(artifact))

    def test_ordinary_context_or_wrong_namespace_cannot_be_coordinated(self):
        for index, bad in ((5, "path-live"), (3, "host")):
            artifact = bilateral_artifact()
            artifact["raw"]["source"]["process"]["command"][index] = bad
            self.assertTrue(evidence.validate_case(artifact))

    def test_forced_exit_or_boolean_exit_code_is_not_joined_cleanup(self):
        for terminal in ({"exit_code": 0, "termination": "terminated"}, {"exit_code": False, "termination": "graceful"}):
            artifact = bilateral_artifact()
            artifact["raw"]["source"]["process"]["terminal_status"] = terminal
            self.assertTrue(evidence.validate_case(artifact))



if __name__ == "__main__":
    unittest.main()
