"""Synthetic Rust-helper receipts, not live or independently signed evidence.

The pinned Rust helper verifies signatures; these checker tests model its
output and exercise raw wire/hash/identity and immutable process-source binding.
"""

import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import check_stage6_acceptance as checker
from stage6_evidence_contract import evidence_contract_for


def varint(value):
    result = bytearray()
    while value >= 128:
        result.append((value & 127) | 128)
        value >>= 7
    return bytes(result + bytes([value]))


def field(number, value):
    return varint(number << 3 | 2) + varint(len(value)) + value


def helper_receipt(legacy=False):
    key = b"\x08\x01" + field(2, bytes([1]) * 32)
    peer_bytes = bytes([0, len(key)]) + key
    peer = checker.relay_native_base58btc_encode(peer_bytes)
    routing = field(1, peer_bytes) + b"\x10\x01"
    kind = b"/libp2p/routing-state-record" if legacy else b"\x03\x01"
    envelope = field(1, key) + field(2, kind) + field(3, routing) + field(5, bytes(64))
    message = field(1, key) + field(8, envelope)
    raw = {
        "basis": "fixture_separate_authenticated_identify_exchange", "protocol": "/ipfs/id/1.0.0",
        "status": "verified", "error": None, "signed_peer_record_verified": True,
        "legacy_validation": {"status": "rejected", "error": "BadPayload(UnexpectedPayloadType)"},
        "interop_validation": {"status": "verified"},
        "envelope_format": "standard", "domain": "libp2p-peer-record", "payload_type_hex": "0301",
        "record_sequence": 1, "record_addresses": [],
        "raw_protobuf_hex": message.hex(), "raw_protobuf_sha256": hashlib.sha256(message).hexdigest(),
        "raw_protobuf_bytes": len(message),
        "signed_envelope_hex": envelope.hex(), "signed_envelope_sha256": hashlib.sha256(envelope).hexdigest(),
    }
    raw.update({name: peer for name in ("authenticated_remote_peer_id", "identify_public_key_peer_id",
                                       "signer_peer_id", "record_peer_id")})
    return peer, raw


def composed_rust_receipt(scenario, security):
    """Join synthetic parser fixtures, never a live cryptographic proof."""
    from test_rust_upgrade_evidence import REMOTE, IDENTIFY, body, receipt
    peer, raw = helper_receipt()
    raw["raw_capture_truncated"] = False

    def identities(value):
        if isinstance(value, dict):
            return {key: identities(item) for key, item in value.items()}
        if isinstance(value, list):
            return [identities(item) for item in value]
        if isinstance(value, str):
            return value.replace(REMOTE, peer)
        return value

    result = identities(receipt(scenario, security))
    result.update(raw_identify_exchange=raw, protocol_count=2, authenticated_remote_peer_id=peer)
    proof = result["upgrade_observation"]
    framed = body(bytes.fromhex(raw["raw_protobuf_hex"]))
    for stream in proof["connections"][0]["streams"]:
        if stream["protocol"] == IDENTIFY:
            stream["read"] = copy.deepcopy(framed)
    proof["applications"]["attempts"][0]["read"] = copy.deepcopy(framed)
    return result, {"peer_id": peer, "dialer": "rust", "listener": "forge",
                    "profile": "native", "scenario": scenario}


class RawIdentifyEvidenceTests(unittest.TestCase):
    def test_paired_rust_gate_requires_independent_listener_observation(self):
        from test_rust_upgrade_evidence import LOCAL, listener_pair
        for security, validate in (("/noise", checker.validate_noise_multistream_evidence),
                                    ("/tls/1.0.0", checker.validate_tls_evidence)):
            for scenario in ("identify", "echo"):
                with self.subTest(security=security, scenario=scenario):
                    payload, listener = listener_pair(scenario, security, True)
                    record = {"peer_id": LOCAL, "dialer": "forge", "listener": "rust",
                              "profile": "native", "scenario": scenario}
                    self.assertEqual(validate(payload, record, listener), [])
                    self.assertTrue(validate(payload, record, None))
                    broken = copy.deepcopy(listener)
                    broken["upgrade_observation"]["fixture_owned_tasks_joined"] = False
                    self.assertTrue(validate(payload, record, broken))
                    if scenario == "echo":
                        self.assertEqual(checker.validate_tcp_yamux_evidence(
                            payload, record, listener, (security,)), [])

    def test_paired_rust_requires_immutable_terminal_owned_counterpart(self):
        from test_rust_upgrade_evidence import LOCAL, listener_pair
        from test_upgrade_evidence import PairedProcessEvidenceTests, attach_terminal_owners
        fixture = PairedProcessEvidenceTests()
        fixture.setUp()
        self.addCleanup(fixture.doCleanups)
        fixture.binaries["rust"] = fixture.root / "rust"
        fixture.payload, fixture.listener_payload = listener_pair(late_error=True)
        record = fixture.record
        record.update(listener="rust", peer_id=LOCAL, listener_result=fixture.listener_payload)
        attempt = record["result"]["attempts"][0]
        command = attempt["command"]
        command[command.index("--peer-id") + 1] = LOCAL
        record["result"] = fixture.payload | {"result_file": str(fixture.result), "attempts": [attempt]}
        record["listener_process"]["command"][0] = str(fixture.binaries["rust"])
        fixture.listener_result.write_text(json.dumps(fixture.listener_payload))
        attach_terminal_owners(record, fixture.payload, fixture.listener_payload)
        fixture.save()
        self.assertEqual(fixture.validate(), [])
        fixture.listener_payload["extra_diagnostic"] = "not in the terminal snapshot"
        fixture.listener_result.write_text(json.dumps(fixture.listener_payload))
        fixture.save()
        self.assertTrue(fixture.validate())

    def test_native_rust_gate_composes_semantic_and_upgrade_validation(self):
        for security, validate in (("/noise", checker.validate_noise_multistream_evidence),
                                    ("/tls/1.0.0", checker.validate_tls_evidence)):
            for scenario in ("identify", "echo"):
                with self.subTest(security=security, scenario=scenario):
                    payload, record = composed_rust_receipt(scenario, security)
                    self.assertEqual(validate(payload, record, None), [])
                    broken = copy.deepcopy(payload)
                    broken["raw_identify_exchange"]["signed_envelope_sha256"] = "0" * 64
                    self.assertTrue(validate(broken, record, None))
                    broken = copy.deepcopy(payload)
                    broken["upgrade_observation"]["applications"]["attempts"][0]["binding"]["stream_trace_id"] = True
                    self.assertTrue(validate(broken, record, None))
                    if scenario == "echo":
                        self.assertEqual(checker.validate_tcp_yamux_evidence(
                            payload, record, None, (security,)), [])

    def test_echo_retains_verified_separate_identify_without_relabeling_behaviour(self):
        peer, raw = helper_receipt()
        payload = {"implementation": "rust", "role": "dialer", "scenario": "echo", "status": "ok",
                   "protocol_count": 2, "signed_peer_record": False,
                   "authenticated_remote_peer_id": peer, "raw_identify_exchange": raw}
        record = {"peer_id": peer}
        self.assertEqual(checker.validate_identify_evidence(payload, record, None), [])
        self.assertIs(payload["signed_peer_record"], False)
        for field, value in (("basis", "behaviour_summary"), ("status", "failed"),
                             ("authenticated_remote_peer_id", "other-peer")):
            broken = copy.deepcopy(payload)
            broken["raw_identify_exchange"][field] = value
            with self.subTest(field=field):
                self.assertTrue(checker.validate_identify_evidence(broken, record, None))

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.peer, raw = helper_receipt()
        self.binaries = {impl: self.root / impl for impl in ("rust", "forge")}
        self.payload = {"implementation": "rust", "role": "dialer", "scenario": "identify", "status": "ok",
                        "protocol_count": 2, "signed_peer_record": False,
                        "authenticated_remote_peer_id": self.peer, "raw_identify_exchange": raw}
        self.result = self.root / "result.json"
        self.log = self.root / "dial.log"
        self.snapshot = self.root / "dial.log.result-file.json"
        self.listener_log = self.root / "listen.log"
        for path in (self.log, self.listener_log):
            path.write_text("synthetic process stdout\n")
        command = [str(self.binaries["rust"]), "dial", "--scenario", "identify", "--peer-id", self.peer,
                   "--addr", "/ip4/127.0.0.1/udp/4001/quic-v1", "--result-file", str(self.result),
                   "--store-dir", str(self.root / "dial-store"), "--transport", "quic"]
        terminal = {"exit_code": 0, "termination": "graceful"}
        outputs = [{"argument": "--result-file", "path": str(self.result), "exists": True,
                    "log_file": str(self.snapshot)}]
        attempt = {"kind": "dial", "scenario_id": "identify", "exit_code": 0, "pid": 123,
                   "terminal_status": terminal, "command": command, "log_file": str(self.log), "outputs": outputs}
        self.record = {
            "dialer": "rust", "listener": "forge", "scenario": "identify", "peer_id": self.peer,
            "profile": "native", "transport_stack": ["quic"], "transport": "quic",
            "runner_scenario_id": "quic_base/identify", "acceptance_scenario_id": "identify",
            "owned_processes": [{"pid": 123, "command": command, "log_file": str(self.log),
                                 "terminal_status": terminal, "outputs": outputs}],
            "result": self.payload | {"result_file": str(self.result), "attempts": [attempt]},
            "listener_process": {
                "log_file": str(self.listener_log), "terminal_status": terminal,
                "command": [str(self.binaries["forge"]), "listen", "--scenario", "identify", "--transport", "quic",
                            "--ready-file", str(self.root / "ready"), "--stop-file", str(self.root / "stop"),
                            "--store-dir", str(self.root / "listen-store"), "--features", "ping,identify"],
            },
            "effective_configuration": {"activation": "enabled", "profile": "native", "transport_stack": ["quic"],
                                        "dialer": {"transport": "quic"}, "listener": {"transport": "quic"}},
        }
        self.save()

    def save(self, capture=True):
        self.record["result"].update(self.payload)
        self.result.write_text(json.dumps(self.payload))
        if capture:
            self.snapshot.write_text(json.dumps(self.payload))
        self.index = checker.build_evidence_index(self.root, [self.record])

    def validate(self):
        indexed, errors = checker.validate_evidence_index(self.root / "artifact.json", self.root,
                                                         [self.record], self.index, self.binaries)
        errors += checker.validate_successful_raw_record(
            self.record, "protocol.identify", "rust_to_forge", "native", ("quic",),
            "quic_base/identify", "identify", evidence_contract_for("identify"), indexed, set(), self.binaries, self.root,
        )
        return errors

    def test_standard_separate_exchange_preserves_false_behaviour_flag(self):
        self.assertEqual(self.validate(), [])
        self.assertIs(self.record["result"]["signed_peer_record"], False)

    def test_raw_bytes_hash_frame_and_identity_tampering_rejected(self):
        original = copy.deepcopy(self.payload["raw_identify_exchange"])
        changes = ({"raw_protobuf_hex": "00"}, {"raw_protobuf_sha256": "0" * 64},
                   {"raw_protobuf_hex": "ff" * 4097}, {"raw_protobuf_hex": "AA"},
                   {"raw_protobuf_bytes": True}, {"raw_protobuf_bytes": 4097},
                   {"authenticated_remote_peer_id": "wrong"}, {"signer_peer_id": "wrong"},
                   {"record_peer_id": "wrong"}, {"signed_peer_record_verified": False},
                   {"interop_validation": {"status": "rejected", "error": "signature failure"}},
                   {"status": "failed", "error": "verification failed"},
                   {"envelope_format": "legacy", "payload_type_hex": "0301"})
        for change in changes:
            with self.subTest(change=change):
                self.payload["raw_identify_exchange"] = original | change
                self.save()
                self.assertTrue(self.validate())
        self.payload["raw_identify_exchange"] = None
        self.save()
        self.assertTrue(self.validate())

    def test_duplicate_or_truncated_wire_rejected_with_fresh_hash(self):
        raw = self.payload["raw_identify_exchange"]
        original = bytes.fromhex(raw["raw_protobuf_hex"])
        for suffix in (field(1, b"duplicate"), field(8, b"duplicate"), b"\x12\x02\x01", b"\x00"):
            with self.subTest(suffix=suffix):
                wire = original + suffix
                raw.update(raw_protobuf_hex=wire.hex(), raw_protobuf_bytes=len(wire),
                           raw_protobuf_sha256=hashlib.sha256(wire).hexdigest())
                self.save()
                self.assertTrue(self.validate())

    def test_legacy_wire_cannot_be_relabelled_standard_with_fresh_hashes(self):
        _, self.payload["raw_identify_exchange"] = helper_receipt(legacy=True)
        self.save()
        self.assertTrue(any("standard payload type mismatch" in error for error in self.validate()))

    def test_key_bytes_must_derive_reported_authenticated_peer(self):
        raw = self.payload["raw_identify_exchange"]
        for name in ("raw_protobuf", "signed_envelope"):
            data = bytes.fromhex(raw[name + "_hex"]).replace(bytes([1]) * 32, bytes([2]) * 32)
            raw[name + "_hex"] = data.hex()
            raw[name + "_sha256"] = hashlib.sha256(data).hexdigest()
        self.save()
        self.assertTrue(any("derive the authenticated peer" in error for error in self.validate()))

    def test_launcher_expected_peer_cannot_disagree_with_raw_exchange(self):
        command = self.record["result"]["attempts"][0]["command"]
        command[command.index("--peer-id") + 1] = "wrong-peer"
        self.save()
        self.assertTrue(any("launch does not bind" in error for error in self.validate()))

    def test_missing_owner_snapshot_hash_or_successful_terminal_rejected(self):
        original = copy.deepcopy(self.record)
        self.snapshot.write_text("tampered captured stdout result")
        self.assertTrue(self.validate())
        self.save()
        self.index = [entry for entry in self.index if entry["path"] != self.snapshot.name]
        self.assertTrue(self.validate())
        self.record["owned_processes"] = []
        self.save()
        self.assertTrue(self.validate())
        self.record = original
        self.record["owned_processes"][0]["terminal_status"] = {"exit_code": 0, "termination": "terminated"}
        self.save()
        self.assertTrue(self.validate())

    def test_rehashed_report_cannot_replace_immutable_helper_output(self):
        self.payload["raw_identify_exchange"]["record_sequence"] = 2
        self.save(capture=False)
        self.assertTrue(any("immutable process snapshot" in error for error in self.validate()))


if __name__ == "__main__":
    unittest.main()
