"""Mocked process ownership and runner wiring; not live mDNS acceptance."""

from contextlib import ExitStack
import copy
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import mdns_cases as cases
import process_lifecycle
import runner
from provenance import WorktreeIdentity
from test_mdns_evidence import pair


class Network:
    def __init__(self, harness):
        self.harness = harness
        self.value = copy.deepcopy(harness.data["network"])
        self.value["state"] = "new"
        self.addresses = {p["role"]: tuple(p["addresses"]) for p in self.value["participants"]}

    def setup(self):
        self.harness.now += self.harness.setup_elapsed
        self.value["state"] = "ready"

    def namespace_command(self, role, command):
        return ["/unit/ip", "netns", "exec", role, *command]

    def close(self):
        self.harness.closed_after_processes = all(p.code is not None for p in self.harness.processes)
        self.value["state"] = "closed"
        self.value["cleanup_failures"] = ["busy"] if self.harness.cleanup_failure else []
        return self.value["cleanup_failures"]

    def evidence(self):
        return self.value


class Process:
    def __init__(self, command, stdout, harness):
        self.command, self.harness = command, harness
        self.role = command[3]
        self.pid = len(harness.processes) + 100
        self.code = None
        stdout.write("synthetic mDNS process\n")
        self.path("--ready-file").write_text(json.dumps(harness.data[f"{self.role}_ready"]))

    def path(self, flag):
        return Path(self.command[self.command.index(flag) + 1])

    def finish(self, code=0):
        self.code = code
        value = copy.deepcopy(self.harness.data[self.role])
        payload = self.command[self.command.index("--payload") + 1].encode()
        value["echo"].update(bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest())
        self.path("--result-file").write_text(json.dumps(value))
        if self.harness.changed_ready:
            self.path("--ready-file").write_text('{}')
        return code

    def poll(self):
        return self.code

    def wait(self, timeout):
        if self.harness.timeout and self.role == "client":
            raise subprocess.TimeoutExpired(self.command, timeout)
        if self.role == "server" and not self.path("--stop-file").exists():
            raise AssertionError("server joined before graceful stop")
        if self.role == "server" and self.harness.forced_server:
            raise subprocess.TimeoutExpired(self.command, timeout)
        if self.role == "client":
            self.harness.client_wait_timeout = timeout
            if timeout < self.harness.operation_elapsed:
                raise subprocess.TimeoutExpired(self.command, timeout)
            self.harness.now += self.harness.operation_elapsed
            server = self.harness.processes[0]
            # Listener has provisional success but is still serving until stop.
            server.path("--result-file").write_text('{"status":"ok"}')
        return self.finish(self.harness.client_code if self.role == "client" else 0)

    def send_signal(self, signal):
        self.harness.signals.append(signal)
        self.finish()  # TERM exit 0 must still fail the independent terminal gate.

    def kill(self):
        self.harness.signals.append("kill")
        self.finish(-9)


class Harness:
    def __init__(self, spec):
        self.spec = spec
        self.data = pair(spec.client, spec.server, spec.transport, spec.family)
        self.processes = []
        self.timeout = self.changed_ready = self.forced_server = self.cleanup_failure = False
        self.closed_after_processes = False
        self.spawn_failure = None
        self.now = 0
        self.setup_elapsed = self.readiness_elapsed = self.operation_elapsed = 0
        self.client_wait_timeout = None
        self.client_code = 0
        self.signals = []
        self.network = Network(self)

    def popen(self, command, stdout, stderr):
        if command[3] == self.spawn_failure:
            raise OSError("synthetic spawn failure")
        value = Process(command, stdout, self)
        self.processes.append(value)
        return value

    def wait_json(self, path, timeout):
        if path.name.endswith(".ready.json"):
            if timeout < self.readiness_elapsed:
                raise TimeoutError("synthetic readiness timeout")
            self.now += self.readiness_elapsed
        return json.loads(path.read_text())

    def run(self, root):
        with patch.object(cases, "IsolatedMdnsNetwork", return_value=self.network), \
                patch.object(process_lifecycle.subprocess, "Popen", side_effect=self.popen):
            return cases.run_case(
                self.spec, {name: Path("/unit") / name for name in ("forge", "go", "rust")}, root,
                pnet_key=Path(__file__).parent / "fixtures/pnet/swarm.key",
                wait_json=self.wait_json, command_attempt=runner.command_attempt,
            )


class MdnsCasesTests(unittest.TestCase):
    def test_registry_and_hidden_commands_all_28_cases(self):
        specs = cases.case_specs()
        self.assertEqual(len(specs), 28)
        self.assertEqual(len({s.identifier for s in specs}), 28)
        for spec in specs:
            with self.subTest(spec=spec), tempfile.TemporaryDirectory() as temp:
                harness = Harness(spec)
                artifact = harness.run(Path(temp))
                self.assertEqual(artifact["status"], "passed", artifact["errors"])
                self.assertEqual(artifact["acceptance_scenario_ids"], [])
                self.assertTrue(harness.closed_after_processes)
                self.assertEqual(len(artifact["attempts"]), 2)
                for process in harness.processes:
                    flags = set(process.command[6::2])
                    expected = {"--scenario", "--transport", "--bind-ip", "--payload", "--ready-file",
                                "--result-file", "--stop-file"}
                    if spec.transport == "tcp-pnet":
                        expected.add("--pnet-key-file")
                    self.assertEqual(flags, expected)
                    self.assertEqual(process.command[process.command.index("--bind-ip") + 1],
                                     harness.network.addresses[process.role][0])
                    self.assertEqual(len(process_lifecycle.current_scope().processes)
                                     if process_lifecycle.current_scope() else 0, 0)
                self.assertEqual(artifact["network"]["state"], "closed")

    def test_timeouts_forced_exit_changed_readiness_and_cleanup_never_pass(self):
        for failure in ("timeout", "forced_server", "changed_ready", "cleanup_failure"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temp:
                harness = Harness(cases.Case("forge", "go", "tcp"))
                setattr(harness, failure, True)
                artifact = harness.run(Path(temp))
                self.assertEqual(artifact["status"], "failed")
                self.assertTrue(artifact["errors"] or artifact["cleanup_errors"])
                self.assertTrue(harness.closed_after_processes)
                self.assertIsNone(process_lifecycle.current_scope())

    def test_setup_and_readiness_leave_time_for_typed_failure_and_shutdown(self):
        with tempfile.TemporaryDirectory() as temp:
            harness = Harness(cases.Case("forge", "go", "tcp"))
            harness.setup_elapsed = 40
            harness.readiness_elapsed = 10
            harness.operation_elapsed = 45 + 5
            harness.client_code = 2
            failure = {"category": "forge.net.p2p", "code": "timeout"}
            harness.data["client"].update(status="error", failure=failure)
            with patch.object(cases.time, "monotonic", side_effect=lambda: harness.now):
                artifact = harness.run(Path(temp))
            self.assertEqual(artifact["status"], "failed")
            self.assertEqual(harness.now, 110)
            self.assertGreaterEqual(harness.client_wait_timeout, 50)
            self.assertLessEqual(harness.client_wait_timeout, cases.PROCESS_TIMEOUT)
            self.assertEqual(harness.signals, [])
            self.assertTrue(harness.closed_after_processes)
            self.assertEqual(artifact["client"]["failure"], failure)
            client_attempt = next(a for a in artifact["attempts"] if a["kind"] == "client")
            self.assertEqual(client_attempt["terminal_status"], {"exit_code": 2, "termination": "graceful"})
            self.assertNotIn("timeout_class", client_attempt)
            self.assertTrue(any("mDNS dialer exited with 2" in error for error in artifact["errors"]))

    def test_excess_setup_allowance_fails_before_launch(self):
        with tempfile.TemporaryDirectory() as temp:
            harness = Harness(cases.Case("forge", "go", "tcp"))
            harness.setup_elapsed = cases.SETUP_TIMEOUT + 1
            with patch.object(cases.time, "monotonic", side_effect=lambda: harness.now):
                artifact = harness.run(Path(temp))
            self.assertEqual(artifact["status"], "failed")
            self.assertEqual(harness.processes, [])
            self.assertEqual(artifact["network"]["state"], "closed")
            self.assertTrue(any("setup allowance expired" in error for error in artifact["errors"]))

    def test_second_spawn_failure_still_joins_listener_before_network_cleanup(self):
        with tempfile.TemporaryDirectory() as temp:
            harness = Harness(cases.Case("forge", "go", "tcp"))
            harness.spawn_failure = "client"
            artifact = harness.run(Path(temp))
            self.assertEqual(artifact["status"], "failed")
            self.assertTrue(harness.closed_after_processes)
            self.assertIsNone(process_lifecycle.current_scope())

    def test_private_key_is_pinned_without_python_crypto_reimplementation(self):
        self.assertEqual(cases.private_network_fingerprint(Path(__file__).parent / "fixtures/pnet/swarm.key"),
                         cases.GOLDEN_NETWORK_FINGERPRINT)
        with self.assertRaises(ValueError):
            cases.private_network_fingerprint(Path(__file__).parent / "fixtures/pnet/mismatched-swarm.key")

    def test_unsupported_cases_are_not_silent_skips(self):
        for values in (("rust", "forge", "tcp-pnet", 4), ("go", "rust", "tcp", 4),
                       ("forge", "go", "tcp", True), ("forge", "go", "udp", 4)):
            with self.subTest(values=values), self.assertRaises(ValueError):
                cases.Case(*values)

    def test_mdns_runner_reuses_provenance_without_running_autonat_or_promoting_manifest(self):
        with tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
            root = Path(temp).resolve()
            identity = WorktreeIdentity("a" * 40, "b" * 64, True)
            stack.enter_context(patch.object(runner.sys, "argv", [
                "runner.py", "--enabled", "true", "--suite", "mdns", "--forge-fixture", str(root / "forge"),
                "--source-dir", str(root / "source"), "--build-dir", str(root / "build"),
                "--forge-root", str(root / "repo"), "--donors-root", str(root / "donors")]))
            stack.enter_context(patch.dict(runner.os.environ, {"FORGE_ENABLE_LIBP2P_INTEROP": "1"}))
            stack.enter_context(patch("sys.stdout", new_callable=io.StringIO))
            returns = {
                "require_tool": "/unit/git", "command_output": "unit version",
                "load_fixture_lock": {"donors": []}, "load_canonical_donor_revisions": {},
                "fixture_donor_revision_bindings": ({}, []), "fixture_donor_checkout_errors": [],
                "require_fixture_provenance": ({"unit": True}, {"command": ["unit", "build-info"]}),
                "sha256_file": "d" * 64, "require_donor": root, "run": None,
                "require_toolchain": ({"go": {"path": "go"}, "cargo": {"path": "cargo"}}, {}, {}),
                "export_fixture_deps": root / "deps", "prepare_go_fixture": (root / "go", []),
                "prepare_rust_fixture": (root / "rust", []),
                "pnet_fixture_paths": (root / "key", root / "wrong-key", "operational-fingerprint"),
                "worktree_identity": identity,
                "run_mdns_suite": [{"status": "passed", "suite": "mdns", "scenario_id": s.identifier,
                                    "acceptance_scenario_ids": []} for s in cases.case_specs()],
                "run_mdns_isolation_suite": [
                    {"status": "passed", "suite": "mdns", "acceptance_scenario_ids": [],
                     "scenario_id": f"mdns-isolation-ipv{family}-{mode}-forge-{role}"}
                    for mode in ("mismatched-psk", "public-private")
                    for role in ("client", "server") for family in (4, 6)],
                "run_mdns_churn_suite": [
                    {"status": "passed", "suite": "mdns", "acceptance_scenario_ids": [],
                     "scenario_id": f"mdns-churn-ipv{family}-forge-dialer"} for family in (4, 6)],
            }
            mocks = {name: stack.enter_context(patch.object(runner, name, return_value=value))
                     for name, value in returns.items()}
            autonat = stack.enter_context(patch.object(runner, "run_autonat_suite"))
            legacy = stack.enter_context(patch.object(runner, "run_pair"))
            self.assertEqual(runner.main(), 0)
            autonat.assert_not_called()
            legacy.assert_not_called()
            mocks["run_mdns_suite"].assert_called_once()
            mocks["run_mdns_isolation_suite"].assert_called_once()
            mocks["run_mdns_churn_suite"].assert_called_once()
            self.assertNotIn("pnet_fingerprint", mocks["run_mdns_suite"].call_args.kwargs)
            artifact = json.loads((root / "build/mdns-artifacts.json").read_text())
            self.assertEqual(len(artifact["artifacts"]), 38)
            self.assertFalse(artifact["fixture_provenance"]["forge_worktree"]["changed_during_run"])


if __name__ == "__main__":
    unittest.main()
