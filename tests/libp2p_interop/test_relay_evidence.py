"""Synthetic paired RESERVE receipts exercise the checker, not live relay/circuit proof."""

import copy
import json
from pathlib import Path
import tempfile
import unittest

import check_stage6_acceptance as checker
from stage6_evidence_contract import evidence_contract_for


def peer_id(seed):
    key = b"\x08\x01\x12\x20" + bytes([seed]) * 32
    return checker.relay_native_base58btc_encode(bytes([0, len(key)]) + key)


class VoucherlessRustReservationTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.relay, self.client = peer_id(1), peer_id(2)
        self.address = f"/ip4/127.0.0.1/udp/4001/quic-v1/p2p/{self.relay}"
        self.binaries = {impl: self.root / impl for impl in ("rust", "forge")}
        self.result = self.root / "result.json"
        self.listener_result = self.root / "listener.json"
        self.ready_path = self.root / "ready.json"
        self.log = self.root / "dial.log"
        self.listener_log = self.root / "listen.log"
        self.snapshot = self.root / "dial.log.result-file.json"
        self.listener_snapshot = self.root / "listen.log.result-file.json"
        self.ready_snapshot = self.root / "listen.log.ready-file.json"
        for path in (self.log, self.listener_log):
            path.write_text("synthetic process stdout\n")
        self.payload = {
            "implementation": "forge", "role": "dialer", "scenario": "relay_reserve", "status": "ok",
            "reservation_basis": "forge.node.async_reserve_relay", "relay_peer_id": self.relay,
            "reservation_client_peer_id": self.client, "authenticated_remote_peer_id": self.relay,
            "authenticated_remote_address": self.address, "relay_endpoints": [self.address],
            "voucher_present": False, "voucher_bytes": 0,
            "reservation_started_at_unix_seconds": 1700000100,
            "reservation_received_at_unix_seconds": 1700000101,
            "reservation_expires_at_unix_seconds": 1700003700,
        }
        self.listener_payload = {
            "implementation": "rust", "role": "listener", "scenario": "relay_reserve", "status": "ok",
            "reservation_basis": "libp2p.relay.server.ReservationReqAccepted",
            "peer_id": self.relay, "listen_addrs": [self.address],
            "reservation_acceptances": [{"src_peer_id": self.client, "renewed": False,
                                         "observed_at_unix_seconds": 1700000101}],
            "trace_complete": True, "trace_overflow": False,
            "started_at_unix_seconds": 1700000090, "observed_at_unix_seconds": 1700000102,
        }
        ready = {"implementation": "rust", "role": "listener", "peer_id": self.relay,
                 "listen_addrs": [self.address], "relay_service_active": True, "status": "ready"}
        command = [str(self.binaries["forge"]), "dial", "--scenario", "relay_reserve", "--peer-id", self.relay,
                   "--addr", self.address, "--result-file", str(self.result),
                   "--store-dir", str(self.root / "dial-store"), "--transport", "quic"]
        listener_command = [str(self.binaries["rust"]), "listen", "--scenario", "relay_reserve",
                            "--transport", "quic", "--ready-file", str(self.ready_path),
                            "--result-file", str(self.listener_result), "--stop-file", str(self.root / "stop"),
                            "--store-dir", str(self.root / "listen-store"), "--features", "relay"]
        terminal = {"exit_code": 0, "termination": "graceful"}
        outputs = [{"argument": "--result-file", "path": str(self.result), "exists": True,
                    "log_file": str(self.snapshot)}]
        listener_outputs = [
            {"argument": "--result-file", "path": str(self.listener_result), "exists": True,
             "log_file": str(self.listener_snapshot)},
            {"argument": "--ready-file", "path": str(self.ready_path), "exists": True,
             "log_file": str(self.ready_snapshot)},
        ]
        attempt = {"kind": "dial", "scenario_id": "relay_reserve", "exit_code": 0, "pid": 123,
                   "terminal_status": terminal, "command": command, "log_file": str(self.log), "outputs": outputs}
        self.record = {
            "dialer": "forge", "listener": "rust", "scenario": "relay_reserve", "peer_id": self.relay,
            "profile": "native", "transport_stack": ["quic"], "transport": "quic", "addr": self.address,
            "runner_scenario_id": "quic_base/relay_reserve", "acceptance_scenario_id": "relay_v2_client_transport",
            "owned_processes": [
                {"pid": 123, "command": command, "log_file": str(self.log), "terminal_status": terminal,
                 "outputs": outputs},
                {"pid": 124, "command": listener_command, "log_file": str(self.listener_log),
                 "terminal_status": terminal, "outputs": listener_outputs, "ready": ready},
            ],
            "result": {"result_file": str(self.result), "attempts": [attempt]},
            "listener_result_file": str(self.listener_result),
            "listener_process": {"pid": 124, "log_file": str(self.listener_log), "terminal_status": terminal,
                                 "command": listener_command, "peer_id": self.relay, "listen_addrs": [self.address]},
            "effective_configuration": {"activation": "enabled", "profile": "native", "transport_stack": ["quic"],
                                        "dialer": {"transport": "quic"}, "listener": {"transport": "quic"}},
        }
        self.ready_path.write_text(json.dumps(ready))
        self.ready_snapshot.write_text(json.dumps(ready))
        self.save()

    def save(self, capture=True):
        self.record["result"] = self.payload | {key: self.record["result"][key] for key in ("result_file", "attempts")}
        self.record["listener_result"] = self.listener_payload
        self.result.write_text(json.dumps(self.payload))
        self.listener_result.write_text(json.dumps(self.listener_payload))
        if capture:
            self.snapshot.write_text(json.dumps(self.payload))
            self.listener_snapshot.write_text(json.dumps(self.listener_payload))
        self.index = checker.build_evidence_index(self.root, [self.record])

    def validate(self):
        indexed, errors = checker.validate_evidence_index(self.root / "artifact.json", self.root,
                                                         [self.record], self.index, self.binaries)
        errors += checker.validate_successful_raw_record(
            self.record, "relay.v2.client_transport", "forge_to_rust", "native", ("quic",),
            "quic_base/relay_reserve", "relay_v2_client_transport",
            evidence_contract_for("relay_v2_client_transport"), indexed, set(), self.binaries, self.root,
        )
        return errors

    def test_fresh_paired_reservation_without_voucher_is_not_circuit_echo(self):
        self.assertEqual(self.validate(), [])
        self.assertEqual(self.payload["voucher_bytes"], 0)
        self.assertNotIn("relay_echo", self.payload)
        # Donor external addresses may omit the terminal relay identity.
        self.payload["relay_endpoints"] = [self.address.split("/p2p/")[0]]
        self.save()
        self.assertEqual(self.validate(), [])

    def test_voucher_zero_alone_and_missing_or_wrong_server_event_rejected(self):
        self.assertTrue(checker.validate_relay_client_evidence(
            {"implementation": "forge", "voucher_bytes": 0}, self.record, None))
        original = copy.deepcopy(self.listener_payload)
        for events in (None, [], [{"src_peer_id": peer_id(3), "renewed": False,
                                  "observed_at_unix_seconds": 1700000101}],
                       [{"src_peer_id": self.client, "renewed": True,
                         "observed_at_unix_seconds": 1700000101}],
                       original["reservation_acceptances"] * 2):
            with self.subTest(events=events):
                self.listener_payload = original | {"reservation_acceptances": events}
                self.save()
                self.assertTrue(self.validate())

    def test_wrong_authenticated_peer_relay_and_returned_addresses_rejected(self):
        original = copy.deepcopy(self.payload)
        for changes in ({"authenticated_remote_peer_id": peer_id(3)}, {"relay_peer_id": peer_id(3)},
                        {"reservation_client_peer_id": peer_id(3)}, {"relay_endpoints": []},
                        {"relay_endpoints": [self.address.replace("4001", "4002")]},
                        {"authenticated_remote_address": self.address.replace("4001", "4002")},
                        {"relay_endpoints": [self.address.replace(self.relay, peer_id(3))]}):
            with self.subTest(changes=changes):
                self.payload = original | changes
                self.save()
                self.assertTrue(self.validate())

    def test_renewal_expired_or_stale_reservation_cannot_prove_fresh_acceptance(self):
        for expires in (0, True, 1700000100, 1700000101, 1700000102):
            with self.subTest(expires=expires):
                self.payload["reservation_expires_at_unix_seconds"] = expires
                self.save()
                self.assertTrue(self.validate())
        self.payload["reservation_expires_at_unix_seconds"] = 1700003700
        self.listener_payload["reservation_acceptances"][0]["observed_at_unix_seconds"] = 1700000099
        self.save()
        self.assertTrue(self.validate())

    def test_incomplete_overflow_or_wrong_direction_not_an_alternate(self):
        original = copy.deepcopy(self.listener_payload)
        for change in ({"trace_complete": False}, {"trace_overflow": True}, {"peer_id": peer_id(3)},
                       {"status": "error"}, {"reservation_acceptances": []}):
            with self.subTest(change=change):
                self.listener_payload = original | change
                self.save()
                self.assertTrue(self.validate())
        self.listener_payload = original
        self.record["listener"] = "go"
        self.save()
        self.assertTrue(any("Forge-to-pinned-Rust" in error for error in self.validate()))

    def test_missing_owner_or_forced_terminal_even_exit_zero_rejected(self):
        original = copy.deepcopy(self.record)
        for position in (0, 1):
            for mutation in ("missing", "forced"):
                with self.subTest(position=position, mutation=mutation):
                    self.record = copy.deepcopy(original)
                    if mutation == "missing":
                        self.record["owned_processes"].pop(position)
                    else:
                        self.record["owned_processes"][position]["terminal_status"] = {
                            "exit_code": 0, "termination": "terminated"}
                    self.save()
                    self.assertTrue(self.validate())

    def test_missing_index_hash_and_changed_immutable_source_rejected(self):
        for source in (self.snapshot, self.listener_snapshot, self.ready_snapshot):
            with self.subTest(source=source):
                original = source.read_text()
                source.write_text("tampered\n")
                self.assertTrue(self.validate())
                source.write_text(original)
                self.save()
                self.index = [entry for entry in self.index if entry["path"] != source.name]
                self.assertTrue(self.validate())
                self.save()
        self.payload["reservation_expires_at_unix_seconds"] += 1
        self.listener_payload["observed_at_unix_seconds"] += 1
        self.save(capture=False)
        self.assertTrue(any("immutable process snapshot" in error for error in self.validate()))

    def test_launch_target_and_ready_service_must_match_observed_pair(self):
        command = self.record["result"]["attempts"][0]["command"]
        command[command.index("--peer-id") + 1] = peer_id(3)
        self.save()
        self.assertTrue(self.validate())
        command[command.index("--peer-id") + 1] = self.relay
        ready = self.record["owned_processes"][1]["ready"]
        ready["relay_service_active"] = False
        self.ready_snapshot.write_text(json.dumps(ready))
        self.save()
        self.assertTrue(any("actual relay service" in error for error in self.validate()))


if __name__ == "__main__":
    unittest.main()
