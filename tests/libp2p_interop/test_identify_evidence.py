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


class RawIdentifyEvidenceTests(unittest.TestCase):
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
