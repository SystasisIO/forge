"""Bounded capture and post-join evidence regression tests."""

import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from mdns_churn_cases import capture_stage
from mdns_phase import Phase, read_receipt
from mdns_network import IsolatedMdnsNetwork, NetworkError
from test_mdns_isolation_cases import Clock
from test_mdns_staged_evidence import lifecycle


class CaptureTests(unittest.TestCase):
    def test_interface_transition_requires_checked_kernel_state(self):
        network = IsolatedMdnsNetwork()
        network._state = "ready"
        network._ip = "/unit/ip"
        network._namespaces = {"outer": "unit-owned-outer", "client": "unit-owned-client",
                               "server": "unit-owned-server"}
        network._verified_interfaces = {"client": {"index": 7, "up": True}}
        network._inside = Mock()
        network._json_output = Mock(return_value=[dict(ifname="eth0", ifindex=7, flags=["MULTICAST"])])
        network.set_interface_up("client", False)
        network._inside.assert_called_once_with("client", "link", "set", "dev", "eth0", "down")
        self.assertFalse(network._verified_interfaces["client"]["up"])
        with self.assertRaises(NetworkError):
            network.set_interface_up("client", True)
        network._inside.side_effect = NetworkError("permission denied")
        with self.assertRaises(NetworkError):
            network.set_interface_up("client", True)

    def test_unsolicited_receipt_is_not_a_response_to_new_control(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "snapshot.json").write_text(json.dumps(lifecycle()))
            phase = SimpleNamespace(work=root, owners={"client": object()}, remaining=lambda limit: limit)
            with self.assertRaises(ValueError):
                capture_stage(phase, "client", "initial", 1, [])

    def test_snapshot_is_retained_before_overwrite_and_pid_is_launcher_bound(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            owner = SimpleNamespace(process=SimpleNamespace(pid=123, poll=lambda: None))
            phase = SimpleNamespace(work=root, owners={"client": owner},
                                    ready={"client": {"local_peer_id": "peer-A"}}, remaining=lambda limit: limit)
            source = root / "snapshot.json"
            samples = []
            original_replace = Path.replace

            def respond(path, target):
                result = original_replace(path, target)
                source.write_text(json.dumps(lifecycle()))
                return result

            with patch.object(Path, "replace", respond):
                capture_stage(phase, "client", "initial", 1, samples)
            self.assertEqual((root / "control").read_text(), "1 snapshot\n")
            source.write_text("{}")
            self.assertEqual(json.loads(samples[0]["raw"])["pid"], owner.process.pid)
            self.assertEqual(Path(samples[0]["evidence_file"]).read_text(), samples[0]["raw"])

    def test_future_epoch_wrong_pid_and_capture_error_fail(self):
        for changes in (dict(epoch=2), dict(pid=999), dict(capture_error="failed")):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                phase = SimpleNamespace(work=root, owners={"client": SimpleNamespace(
                    process=SimpleNamespace(pid=123, poll=lambda: None))},
                    ready={"client": {"local_peer_id": "peer-A"}}, remaining=lambda limit: limit)
                original_replace = Path.replace

                def respond(path, target):
                    result = original_replace(path, target)
                    (root / "snapshot.json").write_text(json.dumps(dict(lifecycle(), **changes)))
                    return result

                with patch.object(Path, "replace", respond), self.assertRaises(ValueError):
                    capture_stage(phase, "client", "initial", 1, [])

    def test_stale_epoch_does_not_satisfy_new_request(self):
        clock = Clock()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            raw = json.dumps(lifecycle())
            (root / "snapshot.json").write_text(raw)
            phase = SimpleNamespace(work=root, owners={"client": SimpleNamespace(
                process=SimpleNamespace(pid=123, poll=lambda: None))},
                ready={"client": {"local_peer_id": "peer-A"}}, remaining=lambda limit: limit)
            with patch("mdns_churn_cases.time", clock), self.assertRaises(TimeoutError):
                capture_stage(phase, "client", "down", 0, [{"epoch": 1, "raw": raw}])

    def test_byte_and_count_bounds(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "snapshot.json"
            path.write_bytes(b" " * 16385)
            with self.assertRaises(ValueError):
                read_receipt(path, limit=16384)
            phase = SimpleNamespace(work=root, owners={"client": object()}, remaining=lambda limit: limit)
            with self.assertRaises(ValueError):
                capture_stage(phase, "client", "up", 1, [{}] * 180)

    def test_phase_captures_final_only_after_join_and_rejects_changed_ready(self):
        with tempfile.TemporaryDirectory() as directory:
            phase = Phase(None, Path(directory) / "phase", "unit", None)
            joined = []
            with patch("mdns_phase._snapshot") as snapshot:
                with phase:
                    owner = SimpleNamespace()
                    phase.owners["client"] = owner
                    phase.ready["client"] = {"status": "ready"}
                    phase.scope.close = lambda: joined.append(True) or []
                    phase.scope.evidence = lambda: []
                    phase.scope.attempts.append({"exit_code": None, "terminal_status": {
                        "exit_code": 0, "termination": "graceful"}})

                    def captured(owner, flag):
                        self.assertTrue(joined)
                        return {"status": "error", "failure": {}} if flag == "--result-file" else {"status": "changed"}

                    snapshot.side_effect = captured
                self.assertEqual(phase.final["client"]["status"], "error")
                self.assertEqual(phase.record["attempts"][0]["exit_code"], 0)
                self.assertTrue(phase.record["errors"])


if __name__ == "__main__":
    unittest.main()
