"""Owner/phase regression tests; live isolation is run only by runner.py."""

import tempfile
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import mdns_isolation_cases as cases
from test_mdns_staged_evidence import quiet


class Clock:
    now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, duration):
        self.now += duration


class Network:
    def __init__(self, family):
        self.state = "new"

    def setup(self):
        self.state = "ready"

    def close(self):
        self.state = "closed"
        return []

    def evidence(self):
        return dict(state=self.state, cleanup_failures=[], cleanup_uncertainty=[])


class FakePhase:
    bad = None

    def __init__(self, network, *args, **kwargs):
        self.network = network
        self.ready, self.final, self.owners = {}, {}, {}
        self.record = dict(errors=[], cleanup_errors=[])
        self.stopped = False

    def __enter__(self):
        return self

    def launch(self, role, impl, binary, transport, service, payload, **kwargs):
        value, ready = quiet(impl)
        ready.update(role="listener" if role == "server" else "dialer", local_peer_id=role,
                     service_name=service or "_p2p-" + "b" * 32 + "._udp.local")
        value.update({key: ready[key] for key in ("role", "local_peer_id", "service_name")})
        self.ready[role], self.final[role] = ready, value
        self.owners[role] = SimpleNamespace(terminal_status=dict(exit_code=0, termination="graceful"))

    def remaining(self):
        return 10

    def require_alive(self):
        if self.bad == "deadline":
            raise TimeoutError("fixture expired")

    def stop(self):
        self.stopped = True

    def __exit__(self, *args):
        if self.bad == "late_failure":
            self.final["client"]["failure"] = {}
        if self.bad == "cleanup":
            self.record["cleanup_errors"].append("forced kill")


class IsolationTests(unittest.TestCase):
    def run_case(self, bad=None, control_status="passed", mode="mismatched-psk", forge_role="client"):
        clock = Clock()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            key, mismatch = root / "key", root / "mismatch"
            key.write_bytes(b"canonical fixture")
            mismatch.write_bytes(b"different fixture")
            with patch.object(cases, "IsolatedMdnsNetwork", Network), patch.object(cases, "Phase", FakePhase), \
                    patch.object(FakePhase, "bad", bad), patch.object(cases, "time", clock), \
                    patch.object(cases, "private_network_fingerprint", return_value="a" * 32), \
                    patch.object(cases, "run_case", return_value={"status": control_status}) as control:
                result = cases.run_case_isolation(mode, forge_role, 4, {"forge": "forge", "go": "go"}, root,
                                                  pnet_key=key, mismatch_key=mismatch,
                                                  wait_json=None, command_attempt=None)
                return result, control.call_args, control.call_count

    def test_all_namespace_role_variants_and_same_network_control(self):
        self.assertEqual(len(cases.case_specs()), 8)
        for mode in ("mismatched-psk", "public-private"):
            for role in ("client", "server"):
                result, call, count = self.run_case(mode=mode, forge_role=role)
                self.assertEqual(result["status"], "passed", result)
                self.assertEqual(count, 1)
                self.assertIsInstance(call.kwargs["supplied_network"], Network)
                self.assertGreaterEqual(result["quiet_stop_at"] - result["quiet_began_at"], 3)
                self.assertEqual(result["network"]["state"], "closed")

    def test_timeout_late_failure_and_forced_cleanup_never_pass(self):
        for bad in ("deadline", "late_failure", "cleanup"):
            result, _, count = self.run_case(bad)
            self.assertEqual(result["status"], "failed", result)
            self.assertEqual(count, 0)
            self.assertEqual(result["network"]["state"], "closed")

    def test_positive_control_is_required(self):
        result, _, count = self.run_case(control_status="failed")
        self.assertEqual(count, 1)
        self.assertEqual(result["status"], "failed")


if __name__ == "__main__":
    unittest.main()
