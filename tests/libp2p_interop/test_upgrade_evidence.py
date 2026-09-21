"""Synthetic observer receipts exercise validation; they are not live evidence."""

import copy
import json
from pathlib import Path
import tempfile
import unittest

from upgrade_evidence import HEADER, MUXER, validate_go_dial_upgrade, validate_go_listener_upgrade, wire_token


def wire(token):
    body = token.encode("ascii") + b"\n"
    size = len(body)
    prefix = bytes([size]) if size < 128 else bytes([size % 128 + 128, size // 128])
    return (prefix + body).hex()


def receipt(early=True, fallback=False):
    app = "/ipfs/id/1.0.0"
    events = []
    def event(kind, **fields):
        events.append({"sequence": len(events) + 1, "kind": kind, **fields})
    def negotiation(phase, protocol, sid=0, rejected=False):
        fields = {"phase": phase, **({"stream_trace_id": sid} if sid else {})}
        for side in ("write", "read"):
            event("multistream_frame", **fields, direction=side, protocol=HEADER, frame_hex=wire(HEADER))
        if rejected:
            event("multistream_frame", **fields, direction="write", protocol="/tls/1.0.0", frame_hex=wire("/tls/1.0.0"))
            event("multistream_frame", **fields, direction="read", protocol="na", frame_hex=wire("na"))
            event("protocol_rejected", **fields, protocol="/tls/1.0.0")
        for side in ("write", "read"):
            event("multistream_frame", **fields, direction=side, protocol=protocol, frame_hex=wire(protocol))
        event("protocol_selected", **fields, protocol=protocol)

    event("tcp_connection")
    negotiation("security_multistream", "/noise", rejected=fallback)
    event("security_enter", protocol="/noise")
    event("security_complete", protocol="/noise")
    if early:
        event("security_handshake_muxer_selected", protocol=MUXER)
    else:
        negotiation("muxer_multistream", MUXER)
    event("muxer_enter", protocol=MUXER)
    event("muxer_complete", protocol=MUXER)
    event("stream_open", stream_trace_id=1, direction="Outbound")
    event("application_stream_binding", stream_trace_id=1, protocol=app,
          network_connection_id="connection-1", network_stream_id="stream-1")
    negotiation("application_multistream", app, sid=1)
    event("application_stream_binding", stream_trace_id=1, protocol=app,
          network_connection_id="connection-1", network_stream_id="stream-1")
    event("stream_close_returned", stream_trace_id=1)
    event("application_io_complete", stream_trace_id=1, protocol=app, network_stream_id="stream-1")
    stream = {"stream_trace_id": 1, "direction": "Outbound", "protocol": app, "network_stream_id": "stream-1",
              "application_io_complete": True, "response_write_complete": False, "failed": False, "reset": False,
              "read_frames": 1, "write_frames": 0, "read_framed_bytes": 20, "write_framed_bytes": 0,
              "read_framed_sha256": "0" * 64}
    connection = {
        "connection_trace_id": 1, "direction": "Outbound", "authenticated_local_peer_id": "local",
        "authenticated_remote_peer_id": "remote", "local_address": "/ip4/127.0.0.1/tcp/1",
        "remote_address": "/ip4/127.0.0.1/tcp/2", "selected_security": "/noise", "selected_muxer": MUXER,
        "early_muxer_negotiation": early, "underlying_close_returned": True, "failed": False,
        "events": events, "streams": [stream],
    }
    return {
        "implementation": "go", "role": "dialer", "status": "ok", "local_peer_id": "local",
        "scenario": "identify", "signed_peer_record": True, "payload_bytes": 20,
        "identify_event_basis": "automatic_identify_separate_exchange_same_connection",
        "identify_event_connection_id": "connection-1",
        "authenticated_remote_peer_id": "remote", "connection_local_addr": connection["local_address"],
        "connection_remote_addr": connection["remote_address"], "negotiated_transport": "tcp",
        "negotiated_security": "/noise", "negotiated_muxer": MUXER, "application_protocol": app,
        "application_connection_trace_id": 1, "application_stream_trace_id": 1,
        "application_connection_id": "connection-1", "application_stream_id": "stream-1",
        "upgrade_observation": {
            "source": "go-libp2p.public-upgrade-hooks.v1", "finalized_after_host_close": True,
            "complete": True, "overflow": False, "target_connection_trace_id": 1, "target_stream_trace_ids": [1],
            "connections": [connection],
        },
    }


def paired_receipt(echo=False):
    sample = receipt()
    connection = sample["upgrade_observation"]["connections"][0]
    connection.update(direction="Inbound", authenticated_local_peer_id="remote", authenticated_remote_peer_id="local")
    connection["events"] = [e for e in connection["events"] if e["kind"] != "application_stream_binding"]
    for event in connection["events"]:
        if event.get("direction") in {"read", "write"}:
            event["direction"] = "read" if event["direction"] == "write" else "write"
        if event.get("direction") == "Outbound":
            event["direction"] = "Inbound"
        if event["kind"] == "application_io_complete":
            event.update(kind="response_write_complete", direction="Inbound")
    stream = connection["streams"][0]
    stream.update(direction="Inbound", application_io_complete=False, response_write_complete=True,
                  read_frames=0, read_framed_bytes=0, write_frames=1, write_framed_bytes=20,
                  write_framed_sha256=stream.pop("read_framed_sha256"))
    del stream["network_stream_id"]
    forge = {
        "implementation": "forge", "role": "dialer", "status": "ok", "scenario": "identify",
        "signed_peer_record": True, "protocol_count": 10, "negotiated_transport": "tcp",
        "local_peer_id": "local", "authenticated_remote_peer_id": "remote",
        "connection_remote_addr": connection["local_address"], "application_connection_id": 77,
        "identify_event_connection_id": 77, "single_fresh_connection_retained": True,
        "identify_event_basis": "automatic_identify_single_fresh_connection",
    }
    if echo:
        protocol = "/forge/interop/relay-echo/1"
        stream2 = stream | {"stream_trace_id": 2, "protocol": protocol, "read_frames": 1,
                            "read_framed_bytes": 20, "read_framed_sha256": "0" * 64}
        connection["streams"].append(stream2)
        for event in list(connection["events"]):
            if event.get("stream_trace_id") == 1:
                event = event | {"stream_trace_id": 2}
                if event.get("protocol") == "/ipfs/id/1.0.0":
                    event["protocol"] = protocol
                    if "frame_hex" in event:
                        event["frame_hex"] = wire(protocol)
                connection["events"].append(event)
        forge.update(scenario="echo", protocol=protocol, application_protocol=protocol,
                     echo_ok=True, payload_bytes=19, application_close_returned=True, application_stream_id=91,
                     negotiated_security="/noise", negotiated_muxer=MUXER,
                     application_request_framed_bytes=20, application_response_framed_bytes=20,
                     application_request_framed_sha256="0" * 64, application_response_framed_sha256="0" * 64)
        sample["upgrade_observation"]["target_stream_trace_ids"] = [2]
    for sequence, event in enumerate(connection["events"], 1):
        event["sequence"] = sequence
    listener = {"implementation": "go", "role": "listener", "status": "ok", "local_peer_id": "remote",
                "upgrade_observation": sample["upgrade_observation"]}
    return forge, listener


def echo_receipt():
    sample = receipt()
    protocol = "/forge/interop/relay-echo/1"
    sample.update(scenario="echo", protocol=protocol, application_protocol=protocol, echo_ok=True, payload_bytes=19)
    connection = sample["upgrade_observation"]["connections"][0]
    stream = connection["streams"][0]
    stream.update(protocol=protocol, write_frames=1, write_framed_bytes=20, write_framed_sha256="0" * 64)
    for event in connection["events"]:
        if event.get("stream_trace_id") == 1 and event.get("protocol") == "/ipfs/id/1.0.0":
            event["protocol"] = protocol
            if "frame_hex" in event:
                event["frame_hex"] = wire(protocol)
    return sample


def attach_terminal_owners(record, payload, listener_payload):
    """Create synthetic indexed-output shapes, never execute a fixture process."""
    terminal = {"exit_code": 0, "termination": "graceful"}
    owners = []
    for pid, mode, view, output, value in (
        (101, "dial", record["result"]["attempts"][0], record["result"]["result_file"], payload),
        (102, "listen", record["listener_process"], record["listener_result_file"], listener_payload),
    ):
        log = Path(view["log_file"])
        snapshot = Path(str(log) + ".result-file.json")
        snapshot.write_text(json.dumps(value))
        outputs = [{"argument": "--result-file", "path": str(output), "exists": True, "log_file": str(snapshot)}]
        owner = {"pid": pid, "command": view["command"], "log_file": str(log),
                 "terminal_status": terminal, "outputs": outputs}
        if mode == "listen":
            command = view["command"]
            ready_file = Path(command[command.index("--ready-file") + 1])
            ready = {"implementation": record["listener"], "role": "listener", "status": "ready",
                     "peer_id": record["peer_id"]}
            ready_file.write_text(json.dumps(ready))
            ready_snapshot = Path(str(log) + ".ready-file.json")
            ready_snapshot.write_text(json.dumps(ready))
            outputs.append({"argument": "--ready-file", "path": str(ready_file), "exists": True,
                            "log_file": str(ready_snapshot)})
            owner["ready"] = ready
            view["pid"] = pid
        else:
            view.update(pid=pid, terminal_status=terminal, outputs=outputs)
        owners.append(owner)
    record["owned_processes"] = owners


class PairedUpgradeTests(unittest.TestCase):
    def validate(self, pair):
        return validate_go_listener_upgrade(*pair, "remote", "/noise")

    def test_two_sided_identity_and_echo_without_equating_local_ids(self):
        for echo in (False, True):
            self.assertEqual(self.validate(paired_receipt(echo)), [])

    def test_stale_or_reconnected_forge_receipt_rejected(self):
        for change in ({"single_fresh_connection_retained": False}, {"identify_event_connection_id": 78},
                       {"application_connection_id": True}, {"identify_event_basis": "cached"},
                       {"local_peer_id": "remote"}, {"connection_remote_addr": "other"}):
            forge, listener = paired_receipt()
            self.assertTrue(self.validate((forge | change, listener)), change)

    def test_duplicate_or_failed_counterpart_cannot_be_replaced(self):
        for kind in ("connection", "stream", "failure", "close", "target"):
            pair = paired_receipt()
            proof = pair[1]["upgrade_observation"]
            connection = proof["connections"][0]
            if kind == "connection":
                proof["connections"].append(copy.deepcopy(connection) | {"connection_trace_id": 2, "failed": True})
            elif kind == "stream":
                connection["streams"].append(connection["streams"][0] | {"stream_trace_id": 2, "failed": True})
            elif kind == "failure":
                connection["streams"][0]["failed"] = True
            elif kind == "close":
                connection["events"][-2]["kind"] = "stream_reset_returned"
            else:
                proof["target_stream_trace_ids"] = [2]
            self.assertTrue(self.validate(pair), kind)

    def test_echo_requires_matching_body_counts_hashes_and_close(self):
        for field, value in (("application_request_framed_sha256", "1" * 64),
                             ("application_response_framed_bytes", 21), ("application_close_returned", False),
                             ("application_stream_id", True), ("negotiated_security", "/tls/1.0.0")):
            forge, listener = paired_receipt(True)
            forge[field] = value
            self.assertTrue(self.validate((forge, listener)), field)

    def test_close_error_cannot_supply_successful_completion(self):
        pair = paired_receipt()
        events = pair[1]["upgrade_observation"]["connections"][0]["events"]
        next(e for e in events if e["kind"] == "stream_close_returned")["error"] = "close failed"
        self.assertTrue(self.validate(pair))
        outbound = receipt()
        events = outbound["upgrade_observation"]["connections"][0]["events"]
        next(e for e in events if e["kind"] == "stream_close_returned")["error"] = "close failed"
        self.assertTrue(validate_go_dial_upgrade(outbound, "remote", "/noise"))

    def test_failed_unselected_identify_proposal_counts_as_second_attempt(self):
        pair = paired_receipt()
        connection = pair[1]["upgrade_observation"]["connections"][0]
        connection["streams"].append(connection["streams"][0] | {
            "stream_trace_id": 2, "protocol": "", "failed": True, "response_write_complete": False,
        })
        events = connection["events"]
        events.append({"sequence": len(events) + 1, "kind": "multistream_frame", "phase": "application_multistream",
                       "direction": "read", "stream_trace_id": 2, "protocol": "/ipfs/id/1.0.0",
                       "frame_hex": wire("/ipfs/id/1.0.0")})
        self.assertTrue(self.validate(pair))

    def test_failed_proposal_label_cannot_hide_actual_identify_wire(self):
        pair = paired_receipt()
        connection = pair[1]["upgrade_observation"]["connections"][0]
        connection["streams"].append(connection["streams"][0] | {
            "stream_trace_id": 2, "protocol": "", "failed": True, "response_write_complete": False,
        })
        events = connection["events"]
        events.append({"sequence": len(events) + 1, "kind": "multistream_frame", "phase": "application_multistream",
                       "direction": "read", "stream_trace_id": 2, "protocol": "/other",
                       "frame_hex": wire("/ipfs/id/1.0.0")})
        self.assertTrue(self.validate(pair))


class UpgradeEvidenceTests(unittest.TestCase):
    def validate(self, value):
        return validate_go_dial_upgrade(value, "remote", "/noise")

    def test_regular_early_and_rejected_proposal_paths(self):
        for early in (False, True):
            for fallback in (False, True):
                with self.subTest(early=early, fallback=fallback):
                    self.assertEqual(self.validate(receipt(early, fallback)), [])

    def test_echo_requires_its_own_automatic_identify_connection(self):
        self.assertEqual(self.validate(echo_receipt()), [])
        for fields in ({"identify_event_connection_id": "other"}, {"signed_peer_record": False},
                       {"identify_event_basis": "cached"}, {"application_protocol": "/ipfs/id/1.0.0"}):
            self.assertTrue(self.validate(echo_receipt() | fields), fields)

    def test_unsigned_identify_event_is_not_native_upgrade_evidence(self):
        self.assertTrue(self.validate(receipt() | {"signed_peer_record": False}))

    def test_echo_payload_size_matches_single_actual_wire_frame(self):
        for size in (20, True, 19.0):
            with self.subTest(size=size):
                self.assertTrue(self.validate(echo_receipt() | {"payload_bytes": size}))
                forge, listener = paired_receipt(True)
                forge["payload_bytes"] = size
                self.assertTrue(validate_go_listener_upgrade(forge, listener, "remote", "/noise"))
        candidate = echo_receipt()
        stream = candidate["upgrade_observation"]["connections"][0]["streams"][0]
        stream["read_frames"] = stream["write_frames"] = 2
        self.assertTrue(self.validate(candidate))

    def test_echo_canonical_length_prefix_boundaries(self):
        for payload, framed in ((1, 2), (127, 128), (128, 130), (4094, 4096)):
            candidate = echo_receipt() | {"payload_bytes": payload}
            stream = candidate["upgrade_observation"]["connections"][0]["streams"][0]
            stream["read_framed_bytes"] = stream["write_framed_bytes"] = framed
            self.assertEqual(self.validate(candidate), [])
            stream["read_framed_bytes"] = stream["write_framed_bytes"] = framed - 1
            self.assertTrue(self.validate(candidate))

    def test_label_only_and_incomplete_receipts_rejected(self):
        for changes in ({"upgrade_observation": None}, {"application_stream_id": "wrong"},
                        {"authenticated_remote_peer_id": "other"}, {"connection_remote_addr": "other"},
                        {"application_connection_trace_id": True}):
            self.assertTrue(self.validate(receipt() | changes))
        for field, value in (("overflow", True), ("complete", False), ("finalized_after_host_close", False),
                             ("target_stream_trace_ids", [True]), ("target_connection_trace_id", True)):
            candidate = receipt()
            candidate["upgrade_observation"][field] = value
            self.assertTrue(self.validate(candidate))

    def test_same_connection_different_stream_cannot_supply_completion(self):
        candidate = receipt()
        connection = candidate["upgrade_observation"]["connections"][0]
        connection["streams"].append(connection["streams"][0] | {"stream_trace_id": 2})
        connection["events"][-1]["stream_trace_id"] = 2
        self.assertTrue(self.validate(candidate))

    def test_identify_event_from_another_connection_is_not_substituted(self):
        candidate = receipt()
        candidate["identify_event_connection_id"] = "different"
        self.assertTrue(self.validate(candidate))

    def test_binding_and_rejection_cannot_be_reported_out_of_order(self):
        for kind in ("application_stream_binding", "protocol_rejected"):
            candidate = receipt(fallback=True)
            events = candidate["upgrade_observation"]["connections"][0]["events"]
            event = next(e for e in events if e["kind"] == kind)
            events.remove(event)
            events.insert(len(events) if kind == "application_stream_binding" else 1, event)
            for index, item in enumerate(events, 1):
                item["sequence"] = index
            with self.subTest(kind=kind):
                self.assertTrue(self.validate(candidate))

    def test_event_stream_ids_are_exact_integers(self):
        for value in (True, 1.0, "1", 0, 33):
            candidate = receipt()
            candidate["upgrade_observation"]["connections"][0]["events"][-1]["stream_trace_id"] = value
            with self.subTest(value=value):
                self.assertTrue(self.validate(candidate))

    def test_exact_attach_and_complete_bindings_are_required(self):
        for defect in ("missing_completion", "both_before_selection", "extra_binding"):
            candidate = receipt()
            events = candidate["upgrade_observation"]["connections"][0]["events"]
            bindings = [e for e in events if e["kind"] == "application_stream_binding"]
            if defect == "missing_completion":
                events.remove(bindings[1])
            elif defect == "both_before_selection":
                events.remove(bindings[1])
                events.insert(events.index(bindings[0]) + 1, bindings[1])
            else:
                events.insert(events.index(bindings[1]), copy.deepcopy(bindings[1]))
            for index, event in enumerate(events, 1):
                event["sequence"] = index
            with self.subTest(defect=defect):
                self.assertTrue(self.validate(candidate))

    def test_identify_payload_count_matches_selected_stream(self):
        candidate = receipt()
        candidate["payload_bytes"] = 21
        self.assertTrue(self.validate(candidate))

    def test_checker_requires_observer_for_go_not_label_only_transcript(self):
        import check_stage6_acceptance as checker
        candidate = receipt()
        record = {"dialer": "go", "scenario": "identify", "peer_id": "remote"}
        self.assertEqual(checker.validate_noise_multistream_evidence(candidate, record, None), [])
        del candidate["upgrade_observation"]
        candidate["selected_protocols"] = ["/noise", MUXER, "/ipfs/id/1.0.0"]
        candidate["upgrade_transcript"] = [
            {"phase": "multistream", "protocol": HEADER}, {"phase": "security", "protocol": "/noise"},
            {"phase": "muxer", "protocol": MUXER}, {"phase": "application", "protocol": "/ipfs/id/1.0.0"},
        ]
        self.assertTrue(checker.validate_noise_multistream_evidence(candidate, record, None))

    def test_duplicate_ids_sequence_and_missing_io_rejected(self):
        for defect in ("duplicate_connection", "duplicate_stream", "sequence", "missing_io", "reset", "bad_digest"):
            candidate = receipt()
            proof = candidate["upgrade_observation"]
            connection = proof["connections"][0]
            if defect == "duplicate_connection":
                proof["connections"].append(copy.deepcopy(connection))
            elif defect == "duplicate_stream":
                connection["streams"].append(copy.deepcopy(connection["streams"][0]))
            elif defect == "sequence":
                connection["events"][0]["sequence"] = True
            elif defect == "missing_io":
                connection["events"].pop()
            elif defect == "reset":
                connection["streams"][0]["reset"] = True
            else:
                connection["streams"][0]["read_framed_sha256"] = "bad"
            with self.subTest(defect=defect):
                self.assertTrue(self.validate(candidate))

    def test_phase_permutation_and_mixed_early_path_rejected(self):
        candidate = receipt()
        events = candidate["upgrade_observation"]["connections"][0]["events"]
        enter = next(i for i, e in enumerate(events) if e["kind"] == "security_enter")
        done = next(i for i, e in enumerate(events) if e["kind"] == "muxer_complete")
        events[enter], events[done] = events[done], events[enter]
        for index, event in enumerate(events, 1):
            event["sequence"] = index
        self.assertTrue(self.validate(candidate))
        for early in (False, True):
            candidate = receipt(early)
            candidate["upgrade_observation"]["connections"][0]["early_muxer_negotiation"] = not early
            self.assertTrue(self.validate(candidate))

    def test_frame_tampering_rejected_even_with_matching_label(self):
        for wire_value in ("00", "ff" * 259, "81002f", "012f", "022f0A"):
            candidate = receipt()
            candidate["upgrade_observation"]["connections"][0]["events"][1]["frame_hex"] = wire_value
            self.assertTrue(self.validate(candidate))
        candidate = receipt(fallback=True)
        events = candidate["upgrade_observation"]["connections"][0]["events"]
        reply = next(e for e in events if e.get("protocol") == "na")
        reply.update(protocol="/wrong", frame_hex=wire("/wrong"))
        self.assertTrue(self.validate(candidate))

    def test_failed_background_is_retained_without_replacing_target(self):
        candidate = receipt()
        candidate["upgrade_observation"]["connections"].append({
            "connection_trace_id": 2, "failed": True, "streams": [],
            "events": [{"sequence": 1, "kind": "failure", "error": "background upgrade failed"}],
        })
        self.assertEqual(self.validate(candidate), [])
        candidate["upgrade_observation"]["connections"][0]["failed"] = True
        self.assertTrue(self.validate(candidate))

    def test_frame_decoder_has_canonical_two_byte_length(self):
        self.assertEqual(wire_token(wire("/" + "p" * 130)), "/" + "p" * 130)


class UpgradeProcessEvidenceTests(unittest.TestCase):
    def setUp(self):
        import check_stage6_acceptance as checker
        self.checker = checker
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.binaries = {name: self.root / name for name in ("go", "forge")}
        self.payload = receipt()
        self.result = self.root / "result.json"
        self.snapshot = self.root / "dial.log.result-file.json"
        self.log = self.root / "dial.log"
        self.listener_log = self.root / "listen.log"
        self.log.write_text("synthetic owned process\n")
        self.listener_log.write_text("synthetic owned process\n")
        command = [str(self.binaries["go"]), "dial", "--scenario", "identify", "--peer-id", "remote",
                   "--addr", "/ip4/127.0.0.1/tcp/2", "--result-file", str(self.result),
                   "--store-dir", str(self.root / "dial-store"), "--transport", "tcp"]
        terminal = {"exit_code": 0, "termination": "graceful"}
        outputs = [{"argument": "--result-file", "path": str(self.result), "exists": True,
                    "log_file": str(self.snapshot)}]
        attempt = {"kind": "dial", "scenario_id": "identify", "exit_code": 0, "pid": 123,
                   "terminal_status": terminal, "command": command, "log_file": str(self.log), "outputs": outputs}
        self.record = {
            "dialer": "go", "listener": "forge", "scenario": "identify", "peer_id": "remote",
            "profile": "native", "transport_stack": ["tcp", "yamux"], "transport": "tcp",
            "runner_scenario_id": "tcp_noise/identify", "acceptance_scenario_id": "noise_identity",
            "owned_processes": [{"pid": 123, "command": command, "log_file": str(self.log),
                                 "terminal_status": terminal, "outputs": outputs}],
            "result": self.payload | {"result_file": str(self.result), "attempts": [attempt]},
            "listener_process": {
                "log_file": str(self.listener_log), "terminal_status": terminal,
                "command": [str(self.binaries["forge"]), "listen", "--scenario", "identify", "--transport", "tcp",
                            "--ready-file", str(self.root / "ready"), "--stop-file", str(self.root / "stop"),
                            "--store-dir", str(self.root / "listen-store"), "--features", "ping,identify"],
            },
            "effective_configuration": {"activation": "enabled", "profile": "native", "transport_stack": ["tcp", "yamux"],
                                        "dialer": {"transport": "tcp"}, "listener": {"transport": "tcp"}},
        }
        self.save()

    def save(self, capture=True):
        self.record["result"].update(self.payload)
        self.result.write_text(json.dumps(self.payload))
        if capture:
            self.snapshot.write_text(json.dumps(self.payload))
        self.index = self.checker.build_evidence_index(self.root, [self.record])

    def validate(self):
        from stage6_evidence_contract import evidence_contract_for
        checker = self.checker
        indexed, errors = checker.validate_evidence_index(self.root / "artifact.json", self.root,
                                                         [self.record], self.index, self.binaries)
        return errors + checker.validate_successful_raw_record(
            self.record, "security.noise", f"{self.record['dialer']}_to_{self.record['listener']}", "native", ("tcp", "yamux"),
            "tcp_noise/identify", "noise_identity", evidence_contract_for("noise_identity"),
            indexed, set(), self.binaries, self.root,
        )

    def test_owned_snapshot_accepts_valid_observation(self):
        self.assertEqual(self.validate(), [])

    def test_changed_report_with_new_hash_cannot_replace_terminal_snapshot(self):
        self.payload["application_connection_id"] = "replacement"
        self.payload["identify_event_connection_id"] = "replacement"
        for event in self.payload["upgrade_observation"]["connections"][0]["events"]:
            if "network_connection_id" in event:
                event["network_connection_id"] = "replacement"
        self.save(capture=False)
        self.assertEqual(validate_go_dial_upgrade(self.payload, "remote", "/noise"), [])
        self.assertTrue(self.validate())

    def test_wrong_owner_or_failed_terminal_rejected(self):
        self.record["owned_processes"][0]["pid"] = 456
        self.assertTrue(self.validate())
        self.record["owned_processes"][0]["pid"] = 123
        self.record["owned_processes"][0]["terminal_status"] = {"exit_code": 0, "termination": "terminated"}
        self.assertTrue(self.validate())


class PairedProcessEvidenceTests(UpgradeProcessEvidenceTests):
    def setUp(self):
        super().setUp()
        self.payload, self.listener_payload = paired_receipt()
        self.record.update(dialer="forge", listener="go")
        attempt = self.record["result"]["attempts"][0]
        attempt["command"][0] = str(self.binaries["forge"])
        self.record["result"] = self.payload | {"result_file": str(self.result), "attempts": [attempt]}
        self.listener_result = self.root / "listener.json"
        self.listener_snapshot = Path(str(self.listener_log) + ".result-file.json")
        ready_file = self.root / "ready"
        ready_snapshot = Path(str(self.listener_log) + ".ready-file.json")
        ready = {"implementation": "go", "role": "listener", "status": "ready", "peer_id": "remote"}
        ready_file.write_text(json.dumps(ready))
        ready_snapshot.write_text(json.dumps(ready))
        listener = self.record["listener_process"]
        listener.update(pid=456)
        listener["command"][0] = str(self.binaries["go"])
        listener["command"].extend(["--result-file", str(self.listener_result)])
        owner = copy.deepcopy(listener) | {"ready": ready, "outputs": [
            {"argument": "--ready-file", "path": str(ready_file), "exists": True, "log_file": str(ready_snapshot)},
            {"argument": "--result-file", "path": str(self.listener_result), "exists": True,
             "log_file": str(self.listener_snapshot)},
        ]}
        self.record["owned_processes"].append(owner)
        self.record["listener_result_file"] = str(self.listener_result)
        self.record["listener_result"] = self.listener_payload
        self.listener_result.write_text(json.dumps(self.listener_payload))
        self.listener_snapshot.write_text(json.dumps(self.listener_payload))
        self.save()

    def test_changed_report_with_new_hash_cannot_replace_terminal_snapshot(self):
        self.payload["application_connection_id"] = 78
        self.payload["identify_event_connection_id"] = 78
        self.save(capture=False)
        self.assertEqual(validate_go_listener_upgrade(self.payload, self.listener_payload, "remote", "/noise"), [])
        self.assertTrue(self.validate())

    def test_changed_counterpart_with_new_index_cannot_replace_snapshot(self):
        self.listener_payload["diagnostic_extra"] = "not in terminal snapshot"
        self.listener_result.write_text(json.dumps(self.listener_payload))
        self.save()
        self.assertEqual(validate_go_listener_upgrade(self.payload, self.listener_payload, "remote", "/noise"), [])
        self.assertTrue(self.validate())

    def test_failed_listener_or_wrong_readiness_owner_rejected(self):
        owner = self.record["owned_processes"][1]
        owner["terminal_status"] = {"exit_code": 0, "termination": "terminated"}
        self.assertTrue(self.validate())
        owner["terminal_status"] = {"exit_code": 0, "termination": "graceful"}
        owner["ready"]["peer_id"] = "wrong"
        self.assertTrue(self.validate())


if __name__ == "__main__":
    unittest.main()
