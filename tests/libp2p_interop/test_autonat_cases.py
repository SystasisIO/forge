"""Mocked orchestration and synthetic evidence, never live acceptance results."""

import copy
from contextlib import ExitStack
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import autonat_cases as cases
import process_lifecycle
import runner
from provenance import WorktreeIdentity
from test_autonat_evidence import CLIENT, SERVICE, valid_pair


class FakeNetwork:
    def __init__(self, secondary, processes):
        self.addresses = {"client": ("11.0.0.1", "11.0.0.3") if secondary else ("11.0.0.1",),
                          "server": ("11.0.0.2",)}
        self.state = "new"
        self.processes = processes
        self.cleanup_errors = []
        self.setup_error = None
        self.closed_port_error = None
        self.port_checks = []
        self.closed_after_processes = False

    def setup(self):
        if self.setup_error:
            self.state = "failed"
            raise self.setup_error
        self.state = "ready"

    def namespace_command(self, role, command):
        return ["/unit/ip", "netns", "exec", "unit-" + role, *command]

    def assert_closed_tcp_port(self, role, port):
        if self.closed_port_error:
            raise self.closed_port_error
        self.port_checks.append((role, port))

    def close(self):
        self.closed_after_processes = all(p.finished for p in self.processes)
        self.state = "failed" if self.cleanup_errors else "closed"
        return self.cleanup_errors

    def evidence(self):
        return {"state": self.state, "commands": [], "cleanup_failures": self.cleanup_errors,
                "cleanup_uncertainty": [], "closed_tcp_ports": self.port_checks}


class FakeProcess:
    def __init__(self, command, output, role, owner):
        self.command, self.output, self.role, self.owner = command, output, role, owner
        self.pid = len(owner.processes) + 100
        self.finished = False
        self.code = None
        output.write("synthetic process log\n")
        if role == "server" and owner.ready is not None:
            self.path("--ready-file").write_text(json.dumps(owner.ready))

    def path(self, flag):
        return Path(self.command[self.command.index(flag) + 1])

    def finish(self, code):
        self.finished, self.code = True, code
        if self.role not in self.owner.omit:
            payload = self.owner.results[self.role]
            self.path("--result-file").write_text(payload if isinstance(payload, str) else json.dumps(payload))
        if self.role == "server" and self.owner.changed_ready:
            self.path("--ready-file").write_text(json.dumps({**self.owner.ready, "peer_id": "changed-peer"}))
        return code

    def poll(self):
        return self.code if self.finished else None

    def wait(self, timeout):
        if self.finished:
            return self.code
        if self.role == "client" and self.owner.timeout:
            raise subprocess.TimeoutExpired(self.command, timeout)
        if self.role == "server":
            if self.owner.server_forced:
                raise subprocess.TimeoutExpired(self.command, timeout)
            if not self.path("--stop-file").exists():
                raise AssertionError("server was joined before graceful stop")
        return self.finish(self.owner.client_code if self.role == "client" else 0)

    def send_signal(self, signal):
        self.finish(0)  # Deliberate TERM-exit-0 control, still not graceful.

    def kill(self):
        self.finish(-9)


class Harness:
    def __init__(self, spec):
        self.spec = spec
        pair = valid_pair(spec.client, spec.server, spec.version, spec.transport,
                          negative=spec.outcome == "unreachable")
        self.results = {"client": pair["client"], "server": pair["server"]}
        self.ready = {
            "implementation": spec.server, "role": "listener", "status": "ready",
            "peer_id": SERVICE, "protocol": cases.PROTOCOLS[spec.version],
            "scenario": spec.scenario, "service_enabled": True,
            "listen_addrs": ["/ip4/11.0.0.2/" + ("udp/42000/quic-v1" if spec.transport == "quic" else "tcp/42000")],
        }
        self.processes = []
        self.network = FakeNetwork(spec.version == 2, self.processes)
        self.timeout = self.server_forced = self.changed_ready = False
        self.client_code = 0
        self.omit = set()
        self.spawn_failure = None
        if spec.outcome == "policy_denied":
            client, server = self.results["client"], self.results["server"]
            client.update(status="rejected", reached=False, response_addr=None,
                          internet_egress="deny", autonat_probe_api_calls=0)
            client["actual_connections"] = [c for c in client["actual_connections"] if c["direction"] == "outbound"]
            server["actual_connections"] = [c for c in server["actual_connections"] if c["direction"] == "inbound"]
            if spec.server == "go":
                server.update(probe_connections=[], service_completed_requests=[])
            if spec.server == "rust":
                server.update(donor_events=[], v1_request_events=0, v2_result_events=0)
            if spec.client == "forge":
                client.update(policy_denied=True, autonat_probe_api_calls=1,
                              v1_vote=None, autonat_v2_address_state=None,
                              current_v2_address_state=None, probe_state="unknown")
                before = json.loads(client["diagnostics_before_stop"])
                before["reachability"].update(internet_egress_allowed=False,
                                               client_v1_enabled=False, client_v2_enabled=False)
                client["diagnostics_before_stop"] = json.dumps(before)
            elif spec.client == "go":
                client.update(policy_denied=True, fresh_inbound_count=0, fresh_inbound_connections=[])
            else:
                client.update(policy_basis="fixture_policy_not_donor_feature",
                              donor_api_basis="fixtureegresspolicy_after_authenticated_control",
                              donor_events=[], v1_request_events=0, v2_result_events=0,
                              fresh_inbound_count=0, fresh_inbound_connections=[])

    def popen(self, command, stdout, stderr):
        role = "server" if "listen" in command else "client"
        if role == self.spawn_failure:
            raise OSError("synthetic spawn failure")
        process = FakeProcess(command, stdout, role, self)
        self.processes.append(process)
        return process

    def wait_ready(self, path, timeout):
        if self.ready is None:
            raise TimeoutError("synthetic readiness timeout")
        return json.loads(path.read_text())

    def run(self, root):
        with patch.object(cases, "IsolatedAutonatNetwork", return_value=self.network), \
             patch.object(process_lifecycle.subprocess, "Popen", side_effect=self.popen):
            return cases.run_case(
                self.spec, {k: Path("/unit") / k for k in ("forge", "go", "rust")}, root,
                pnet_key=Path("/outside/swarm.key"), pnet_fingerprint="fingerprint",
                wait_json=self.wait_ready, command_attempt=runner.command_attempt,
            )


class AutonatCasesTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def assert_failed(self, artifact, text=None):
        self.assertEqual(artifact["status"], "failed", artifact)
        if text:
            self.assertIn(text, " ".join(artifact["errors"] + artifact["cleanup_errors"]))
        self.assertIsNone(process_lifecycle.current_scope())

    def test_exact_41_case_matrix(self):
        specs = cases.case_specs()
        self.assertEqual(len(specs), 41)
        self.assertEqual(len({s.identifier for s in specs}), 41)
        self.assertEqual(sum(s.transport != "tcp-pnet" and s.outcome == "reachable" for s in specs), 24)
        self.assertEqual(sum(s.transport == "tcp-pnet" and s.outcome == "reachable" for s in specs), 8)
        self.assertEqual(sum(s.outcome == "policy_denied" for s in specs), 8)
        self.assertEqual([s for s in specs if s.outcome == "unreachable"],
                         [cases.Case("forge", "go", 1, "tcp", "unreachable")])

    def test_all_synthetic_pairs_use_full_validation_after_cleanup(self):
        for spec in cases.case_specs():
            with self.subTest(spec=spec):
                harness = Harness(spec)
                artifact = harness.run(self.root)
                self.assertEqual(artifact["status"], "passed", artifact)
                self.assertTrue(harness.network.closed_after_processes)
                self.assertEqual(len(artifact["attempts"]), 2)
                self.assertEqual({a["attempt_id"] for a in artifact["attempts"]}, {1})
                self.assertTrue(all(p["terminal_status"] == {"exit_code": 0, "termination": "graceful"}
                                    for p in artifact["owned_processes"]))
                if spec.version == 2:
                    command = harness.processes[1].command
                    self.assertIn("/ip4/11.0.0.3/" + ("udp/0/quic-v1" if spec.transport == "quic" else "tcp/0"), command)
                if spec.outcome == "unreachable":
                    self.assertEqual(harness.network.port_checks, [("client", 9), ("client", 9)])
                if spec.outcome == "policy_denied":
                    self.assertEqual(artifact["proof_scope"], "client_policy_control")

    def test_bad_native_evidence_is_not_promoted_by_ok_status(self):
        harness = Harness(cases.Case("forge", "go", 2, "tcp"))
        harness.results["client"]["autonat_v2_address_state"] = None
        self.assert_failed(harness.run(self.root), "exact verified address")

    def test_namespace_and_ready_listener_bind_runtime_address(self):
        for mutation in ("client_ip", "control_port", "ready_peer", "implementation"):
            with self.subTest(mutation=mutation):
                harness = Harness(cases.Case("forge", "go", 1, "tcp"))
                if mutation == "client_ip":
                    harness.results["client"]["requested_addr"] = "/ip4/11.0.0.3/tcp/43000"
                elif mutation == "control_port":
                    harness.ready["listen_addrs"] = ["/ip4/11.0.0.2/tcp/42001"]
                elif mutation == "ready_peer":
                    harness.ready["peer_id"] = "another-service"
                else:
                    harness.results["client"]["implementation"] = "go"
                self.assert_failed(harness.run(self.root / mutation))

    def test_partial_readiness_and_spawn_failure_keep_owned_raw_evidence(self):
        for mode in ("readiness", "server_spawn", "client_spawn"):
            with self.subTest(mode=mode):
                harness = Harness(cases.Case("forge", "go", 1, "tcp"))
                if mode == "readiness":
                    harness.ready = None
                else:
                    harness.spawn_failure = mode.split("_")[0]
                artifact = harness.run(self.root / mode)
                self.assert_failed(artifact)
                self.assertTrue(harness.network.closed_after_processes)
                self.assertTrue(artifact["owned_processes"])
                self.assertTrue(runner.evidence_index(self.root / mode, [artifact]))
                self.assertLessEqual(len(harness.processes), 1)

    def test_timeout_is_failed_without_retry_even_if_term_exits_zero(self):
        harness = Harness(cases.Case("forge", "go", 1, "tcp", "unreachable"))
        harness.timeout = True
        artifact = harness.run(self.root)
        self.assert_failed(artifact, "forced SIGTERM")
        self.assertEqual(len(harness.processes), 2)
        self.assertEqual(artifact["attempts"][1]["timeout_class"], "fixture_timeout")
        self.assertEqual(artifact["attempts"][1]["terminal_status"]["exit_code"], 0)

    def test_primary_error_and_multiple_cleanup_failures_are_preserved(self):
        harness = Harness(cases.Case("forge", "go", 1, "tcp"))
        harness.client_code = 2
        harness.server_forced = True
        harness.network.cleanup_errors = ["namespace deletion failed"]
        artifact = harness.run(self.root)
        self.assert_failed(artifact, "client exited with 2")
        self.assertTrue(any("forced SIGTERM" in e for e in artifact["cleanup_errors"]))
        self.assertIn("namespace deletion failed", artifact["cleanup_errors"])
        self.assertIn("client", artifact)
        self.assertIn("server", artifact)

    def test_missing_malformed_and_changed_raw_results_fail_with_indexed_files(self):
        for mode in ("missing", "malformed", "changed_ready"):
            with self.subTest(mode=mode):
                harness = Harness(cases.Case("forge", "go", 1, "tcp"))
                if mode == "missing":
                    harness.omit.add("server")
                elif mode == "malformed":
                    harness.results["server"] = "{broken-json"
                else:
                    harness.changed_ready = True
                root = self.root / mode
                artifact = harness.run(root)
                self.assert_failed(artifact)
                index = runner.evidence_index(root, [artifact])
                for entry in index:
                    payload = (root / entry["path"]).read_bytes()
                    self.assertEqual(entry["sha256"], hashlib.sha256(payload).hexdigest())
                if mode == "malformed":
                    self.assertTrue(any(e["path"].endswith("server.log.result-file.json") for e in index))

    def test_network_setup_or_closed_negative_port_failure_never_launches(self):
        for mode in ("setup", "port"):
            with self.subTest(mode=mode):
                harness = Harness(cases.Case("forge", "go", 1, "tcp", "unreachable"))
                if mode == "setup":
                    harness.network.setup_error = RuntimeError("outer namespace not isolated")
                else:
                    harness.network.closed_port_error = RuntimeError("port 9 is listening")
                artifact = harness.run(self.root / mode)
                self.assert_failed(artifact)
                self.assertEqual(harness.processes, [])

    def test_client_policy_denial_does_not_allow_probe_or_weaken_server_cleanup(self):
        for impl in ("forge", "go", "rust"):
            for mode in ("extra_call", "underreported_call", "auth", "server_cleanup"):
                with self.subTest(impl=impl, mode=mode):
                    harness = Harness(cases.Case(impl, "go" if impl == "forge" else "forge",
                                                2, "tcp-pnet", "policy_denied"))
                    if mode == "extra_call":
                        harness.results["client"]["autonat_probe_api_calls"] = 2 if impl == "forge" else 1
                    elif mode == "underreported_call":
                        harness.results["client"]["autonat_probe_api_calls"] = 0 if impl == "forge" else -1
                    elif mode == "auth":
                        harness.results["client"]["authenticated_peer" if impl == "rust" else "control_authenticated_peer"] = "wrong"
                    else:
                        harness.results["server"]["hosts_closed"] = False
                    self.assert_failed(harness.run(self.root / impl / mode))


class AutonatRunnerBranchTests(unittest.TestCase):
    def test_common_preflight_and_final_identity_surround_full_or_focused_suite(self):
        for suite in ("autonat", "stage6"):
            for changed in (False, True):
                with self.subTest(suite=suite, changed=changed), tempfile.TemporaryDirectory() as temp, ExitStack() as stack:
                    root = Path(temp).resolve()
                    identity = WorktreeIdentity("a" * 40, "b" * 64, True)
                    end = WorktreeIdentity(identity.head, "c" * 64, True) if changed else identity
                    argv = ["runner.py", "--enabled", "true", "--forge-fixture", str(root / "forge"),
                            "--source-dir", str(root / "source"), "--build-dir", str(root / "build"),
                            "--forge-root", str(root / "repo"), "--donors-root", str(root / "donors")]
                    if suite == "autonat":
                        argv += ["--suite", suite]
                    stack.enter_context(patch.object(runner.sys, "argv", argv))
                    stack.enter_context(patch("sys.stdout", new_callable=io.StringIO))
                    stack.enter_context(patch("sys.stderr", new_callable=io.StringIO))
                    returns = {
                        "require_tool": "/unit/git", "command_output": "unit version",
                        "load_fixture_lock": {"donors": []}, "load_canonical_donor_revisions": {},
                        "fixture_donor_revision_bindings": ({}, []), "fixture_donor_checkout_errors": [],
                        "require_fixture_provenance": ({"unit": True}, {"command": ["unit", "build-info"]}),
                        "sha256_file": "d" * 64, "require_donor": root, "run": None,
                        "require_toolchain": ({"go": {"path": "go"}, "cargo": {"path": "cargo"}}, {}, {}),
                        "export_fixture_deps": root / "deps",
                        "prepare_go_fixture": (root / "go", []), "prepare_rust_fixture": (root / "rust", []),
                        "pnet_fixture_paths": (root / "key", root / "wrong-key", "fingerprint"),
                        "run_autonat_suite": [{"status": "passed", "scenario_id": "unit-autonat"}],
                    }
                    mocks = {name: stack.enter_context(patch.object(runner, name, return_value=value))
                             for name, value in returns.items()}
                    stack.enter_context(patch.object(runner, "worktree_identity", side_effect=[identity, end]))
                    legacy = ("run_pair", "run_pair_with_transport", "run_dht_value_remote_get",
                              "run_pubsub_mixed_mesh_stress", "run_hidden_dht_find_peer", "run_topology",
                              "run_native_relay_topology")
                    legacy_mocks = [stack.enter_context(patch.object(runner, name, return_value={"unit": name}))
                                    for name in legacy]
                    self.assertEqual(runner.main(), 1 if changed else 0)
                    mocks["require_fixture_provenance"].assert_called_once_with(root / "forge", identity, {"donors": []})
                    mocks["prepare_go_fixture"].assert_called_once()
                    mocks["prepare_rust_fixture"].assert_called_once()
                    mocks["run_autonat_suite"].assert_called_once()
                    self.assertEqual(mocks["run_autonat_suite"].call_args.args,
                                     ({"forge": root / "forge", "go": root / "go", "rust": root / "rust"},
                                      root / "build" / ("autonat-run" if suite == "autonat" else "interop-run")))
                    self.assertEqual(any(m.called for m in legacy_mocks), suite == "stage6")
                    artifact = json.loads((root / "build" / ("autonat-artifacts.json" if suite == "autonat" else "interop-artifacts.json")).read_text())
                    self.assertEqual(artifact["fixture_provenance"]["forge_worktree"]["changed_during_run"], changed)
                    self.assertEqual(artifact["fixture_provenance"]["binaries"]["forge"]["sha256"], "d" * 64)


if __name__ == "__main__":
    unittest.main()
