"""Controlled process/artifact tests, not live donor compatibility evidence."""

import hashlib
import json
import signal
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import runner
import process_lifecycle


class FakeProcess:
    def __init__(self, pid, script, events):
        self.pid = pid
        self.returncode = None
        self.waits = list(script.get("waits", [0]))
        self.script = script
        self.events = events

    def poll(self):
        return self.returncode

    def wait(self, timeout):
        self.events.append((self.pid, "wait", timeout))
        outcome = self.waits.pop(0)
        if isinstance(outcome, Exception):
            raise outcome
        self.returncode = outcome
        return outcome

    def send_signal(self, value):
        self.events.append((self.pid, "signal", value))
        if self.script.get("signal_error"):
            raise OSError("signal delivery failed")

    def kill(self):
        self.events.append((self.pid, "kill"))
        if self.script.get("kill_error"):
            raise OSError("kill delivery failed")


def timeout():
    return subprocess.TimeoutExpired(["controlled-fixture"], 5)


class ProcessLifecycleTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.scripts = []
        self.processes = []
        self.logs = []
        self.events = []
        self.addCleanup(patch.stopall)
        patch.object(process_lifecycle.subprocess, "Popen", side_effect=self.spawn).start()
        patch.object(runner, "wait_json", side_effect=self.ready).start()

    def spawn(self, command, stdout, stderr):
        script = self.scripts.pop(0) if self.scripts else {}
        if script.get("spawn_error"):
            self.logs.append(stdout)
            raise OSError("spawn failed")
        process = FakeProcess(len(self.processes) + 100, script, self.events)
        self.processes.append(process)
        self.logs.append(stdout)
        stdout.write(f"controlled process {process.pid}\n")
        stdout.flush()
        for flag in ("--ready-file", "--result-file"):
            if flag not in command:
                continue
            if flag == "--result-file" and not script.get("write_result", True):
                continue
            path = Path(command[command.index(flag) + 1])
            if flag == "--ready-file" and not script.get("ready", True):
                if script.get("malformed_ready"):
                    path.write_text("{partial readiness")
                continue
            payload = ({"peer_id": f"peer-{process.pid}", "listen_addrs": ["/ip4/127.0.0.1/udp/1/quic-v1"]}
                       if flag == "--ready-file" else script.get("result", {"status": "ok"}))
            path.write_text(json.dumps(payload))
        return process

    @staticmethod
    def ready(path, seconds):
        if not path.exists():
            raise TimeoutError("controlled readiness failure")
        return json.loads(path.read_text())

    def listener(self, name="listener"):
        return runner.start_listener(Path("fixture"), name, self.root)

    def assert_closed(self):
        self.assertTrue(all(log.closed for log in self.logs))
        self.assertIsNone(process_lifecycle.current_scope())

    def test_success_is_committed_after_graceful_cleanup(self):
        @runner.owned_case
        def case():
            self.listener()
            return {"status": "ok"}

        result = case()
        self.assertEqual(result["status"], "ok")
        self.assertEqual(result["owned_processes"][0]["terminal_status"],
                         {"exit_code": 0, "termination": "graceful"})
        self.assert_closed()

    def test_tcp_upgrade_listener_is_joined_before_reading_final_evidence(self):
        proof = {"finalized_after_host_close": True, "complete": True, "overflow": False}
        self.scripts = [{"result": {"status": "ok", "upgrade_observation": proof}}]
        def read_after_join(path, seconds):
            if "listen-identify.json" in path.name:
                self.assertEqual(self.processes[0].returncode, 0)
            return self.ready(path, seconds)

        with patch.object(runner, "wait_json", side_effect=read_after_join), \
             patch.object(runner, "run_dial", return_value={"status": "ok"}):
            result = runner.run_pair_with_transport(
                Path("fixture"), "forge", Path("fixture"), "go", "identify", self.root,
                "tcp", "native", ("tcp", "yamux"), "tcp/identify", "noise_identity")
        self.assertEqual(result["listener_result"]["upgrade_observation"], proof)
        self.assertIn("--result-file", result["listener_process"]["command"])
        self.assertEqual(len(result["owned_processes"][0]["outputs"]), 2)
        self.assert_closed()

    def test_tcp_upgrade_listener_rejects_nonfinal_or_incomplete_evidence(self):
        for proof in ({}, {"finalized_after_host_close": False, "complete": True},
                      {"finalized_after_host_close": True, "complete": False},
                      {"finalized_after_host_close": True, "complete": True, "overflow": True}):
            with self.subTest(proof=proof):
                self.scripts = [{"result": {"status": "ok", "upgrade_observation": proof}}]
                with patch.object(runner, "run_dial", return_value={"status": "ok"}):
                    with self.assertRaises(runner.CaseFailure):
                        runner.run_pair_with_transport(
                            Path("fixture"), "forge", Path("fixture"), "go", "echo", self.root,
                            "tcp-tls", "native", ("tcp", "yamux"), "tcp_tls/echo", "tls_echo")
                self.assert_closed()

    def test_tcp_upgrade_listener_cleanup_failure_cannot_commit_evidence(self):
        self.scripts = [{"waits": [timeout(), 0], "result": {
            "status": "ok", "upgrade_observation": {
                "finalized_after_host_close": True, "complete": True, "overflow": False}}}]
        with patch.object(runner, "run_dial", return_value={"status": "ok"}):
            with self.assertRaises(runner.CaseFailure) as raised:
                runner.run_pair_with_transport(
                    Path("fixture"), "forge", Path("fixture"), "go", "echo", self.root,
                    "tcp", "native", ("tcp", "yamux"), "tcp/echo", "tcp_echo")
        self.assertIn("forced SIGTERM", str(raised.exception))
        self.assert_closed()

    def test_success_plus_forced_listener_exit_is_failure(self):
        self.scripts = [{"waits": [timeout(), -signal.SIGTERM]}]
        with patch.object(runner, "run_dial", return_value={"status": "ok"}):
            with self.assertRaises(runner.CaseFailure) as raised:
                runner.run_pair(Path("fixture"), "forge", Path("fixture"), "go", "echo", self.root)
        failure = raised.exception
        self.assertIsNone(failure.primary)
        self.assertIn("forced SIGTERM", str(failure))
        self.assertEqual(failure.artifact["uncommitted_result"]["result"]["status"], "ok")
        self.assert_closed()

    def test_term_exit_zero_still_fails(self):
        self.scripts = [{"waits": [timeout(), 0]}]
        @runner.owned_case
        def case():
            self.listener()
            return {"status": "ok"}

        with self.assertRaises(runner.CaseFailure) as raised:
            case()
        status = raised.exception.artifact["processes"][0]["terminal_status"]
        self.assertEqual(status, {"exit_code": 0, "termination": "terminated"})
        self.assertIn("forced SIGTERM", str(raised.exception))
        self.assert_closed()

    def test_primary_failure_and_all_cleanup_errors_survive(self):
        primary = ValueError("primary protocol mismatch")
        self.scripts = [
            {"waits": [timeout(), 0]},
            {"waits": [timeout(), timeout(), OSError("cannot join")],
             "signal_error": True, "kill_error": True},
        ]
        @runner.owned_case
        def case():
            self.listener("first")
            self.listener("second")
            raise primary

        with self.assertRaises(runner.CaseFailure) as raised:
            case()
        failure = raised.exception
        self.assertIs(failure.primary, primary)
        self.assertIs(failure.__cause__, primary)
        for message in ("primary protocol mismatch", "forced SIGTERM", "forced SIGKILL",
                        "signal delivery failed", "kill delivery failed", "cannot join"):
            self.assertIn(message, str(failure))
        self.assertEqual([event[0] for event in self.events if event[1] == "signal"], [101, 100])
        self.assertEqual(len(failure.artifact["processes"]), 2)
        self.assert_closed()

    def test_close_is_idempotent_and_keeps_escalation_record(self):
        self.scripts = [{"waits": [timeout(), timeout(), 0]}]
        @runner.owned_case
        def case():
            listener = self.listener()
            errors = list(listener.close())
            events = list(self.events)
            self.assertEqual(listener.close(), errors)
            self.assertEqual(self.events, events)
            return {"status": "ok"}

        with self.assertRaises(runner.CaseFailure) as raised:
            case()
        self.assertEqual(sum("forced SIGTERM" in error for error in raised.exception.cleanup_errors), 1)
        self.assertEqual(sum("forced SIGKILL" in error for error in raised.exception.cleanup_errors), 1)
        self.assertEqual(sum(event[1] == "kill" for event in self.events), 1)
        self.assert_closed()

    def test_partial_startup_in_each_scenario_is_owned(self):
        binaries = {name: Path("fixture") for name in ("forge", "go", "rust")}
        cases = {
            "pair": lambda root: runner.run_pair(binaries["forge"], "forge", binaries["go"], "go", "echo", root),
            "private": lambda root: runner.run_pair_with_transport(
                binaries["forge"], "forge", binaries["go"], "go", "pnet", root,
                "tcp-pnet", "private_network", ("tcp", "pnet", "yamux"), "pnet", "pnet",
                root / "key", root / "wrong-key", "fingerprint"),
            "private_control": lambda root: runner.run_pnet_control(
                binaries["forge"], "forge", binaries["go"], "go", root,
                root / "key", None, "fingerprint", "missing_key", "correlation"),
            "value": lambda root: runner.run_dht_value_remote_get(binaries, "forge", "go", "dht_pk_put_get", root),
            "hidden": lambda root: runner.run_hidden_dht_find_peer(binaries, "forge", "go", "rust", root),
            "mesh": lambda root: runner.run_pubsub_mixed_mesh_stress(binaries, root),
            "relay": lambda root: runner.run_native_relay_topology(
                binaries, "forge", "go", "rust", "relay_echo_topology", root),
        }
        for name, call in cases.items():
            with self.subTest(scenario=name):
                self.scripts = [{"ready": False, "waits": [timeout(), 0]}]
                with self.assertRaises(runner.CaseFailure) as raised:
                    call(self.root / name)
                self.assertIn("controlled readiness failure", str(raised.exception.primary))
                self.assertIn("forced SIGTERM", str(raised.exception))
                process = raised.exception.artifact["processes"][0]
                self.assertEqual(process["ready"], {})
                self.assertFalse(process["outputs"][0]["exists"])
                self.assert_closed()

    def test_partial_second_startup_closes_newest_before_ready_peer(self):
        binaries = {name: Path("fixture") for name in ("forge", "go", "rust")}
        cases = {
            "hidden": lambda root: runner.run_hidden_dht_find_peer(binaries, "forge", "go", "rust", root),
            "mesh": lambda root: runner.run_pubsub_mixed_mesh_stress(binaries, root),
            "destination": lambda root: runner.run_native_relay_topology(
                binaries, "forge", "go", "rust", "relay_echo_topology", root),
        }
        for name, call in cases.items():
            with self.subTest(scenario=name):
                self.events.clear()
                self.scripts = [{"waits": [timeout(), 0]}, {"ready": False, "waits": [timeout(), 0]}]
                with self.assertRaises(runner.CaseFailure) as raised:
                    call(self.root / name)
                processes = raised.exception.artifact["processes"]
                self.assertEqual([event[0] for event in self.events if event[1] == "signal"],
                                 [processes[1]["pid"], processes[0]["pid"]])
                self.assert_closed()

    def test_nested_control_cleanup_failure_is_not_lost(self):
        self.scripts = [{}, {"waits": [timeout(), 0]}]
        @runner.owned_case
        def control():
            self.listener("control").close()
            return {"status": "ok"}

        @runner.owned_case
        def case():
            self.listener("positive")
            return {"control": control()}

        with self.assertRaises(runner.CaseFailure) as raised:
            case()
        self.assertIn("forced SIGTERM", str(raised.exception))
        self.assertEqual([event[0] for event in self.events if event[1] == "wait"], [101, 101, 100])
        self.assert_closed()

    def test_spawn_failure_closes_log_and_previous_process(self):
        self.scripts = [{}, {"spawn_error": True}]
        @runner.owned_case
        def case():
            self.listener("first")
            self.listener("second")
            return {}

        with self.assertRaises(runner.CaseFailure) as raised:
            case()
        self.assertIn("spawn failed", str(raised.exception.primary))
        self.assertEqual(self.processes[0].returncode, 0)
        self.assert_closed()

    def test_command_timeout_escalation_cannot_be_hidden_by_successful_retry(self):
        self.scripts = [{"waits": [timeout(), 0], "result": {"attempt": 1}}, {"result": {"attempt": 2}}]
        result_file = self.root / "result.json"
        @runner.owned_case
        def case():
            return {"attempts": runner.run_command_with_attempts(
                ["fixture", "dial", "--result-file", str(result_file)], self.root / "dial.log",
                "echo", "dial", 1, reset_paths=(result_file,))}

        with self.assertRaises(runner.CaseFailure) as raised:
            case()
        attempts = raised.exception.artifact["uncommitted_result"]["attempts"]
        self.assertEqual(len(attempts), 2)
        self.assertNotEqual(attempts[0]["log_file"], attempts[1]["log_file"])
        outputs = [process["outputs"][0] for process in raised.exception.artifact["processes"]]
        self.assertNotEqual(outputs[0]["log_file"], outputs[1]["log_file"])
        self.assertEqual([json.loads(Path(output["log_file"]).read_text()) for output in outputs],
                         [{"attempt": 1}, {"attempt": 2}])
        self.assertIn("forced SIGTERM", str(raised.exception))
        self.assert_closed()

    def test_negative_command_timeout_preserves_primary_and_cleanup(self):
        self.scripts = [{"waits": [timeout(), 0]}]
        @runner.owned_case
        def case():
            return {"attempts": runner.run_command_once(
                ["fixture", "dial"], self.root / "control.log", "pnet", "pnet_control", 1)}

        with self.assertRaises(runner.CaseFailure) as raised:
            case()
        self.assertIn("timed out", str(raised.exception.primary))
        self.assertIn("forced SIGTERM", str(raised.exception))
        self.assertEqual(len(self.processes), 1)
        self.assert_closed()

    def test_failed_join_prevents_second_spawn_and_shared_path_reset(self):
        self.scripts = [{"waits": [timeout(), timeout(), OSError("unreaped")], "result": {"attempt": 1}}, {}]
        store = self.root / "store"
        result_file = self.root / "result.json"
        def spawn_and_mark(command, stdout, stderr):
            process = self.spawn(command, stdout, stderr)
            store.mkdir()
            (store / "owned-by-first").write_text("must survive failed join\n")
            return process

        @runner.owned_case
        def case():
            return {"attempts": runner.run_command_with_attempts(
                ["fixture", "dial", "--result-file", str(result_file)], self.root / "dial.log",
                "echo", "dial", 1, reset_paths=(store, result_file))}

        with patch.object(process_lifecycle.subprocess, "Popen", side_effect=spawn_and_mark) as spawning:
            with self.assertRaises(runner.CaseFailure) as raised:
                case()
        self.assertEqual(spawning.call_count, 1)
        self.assertEqual(len(self.scripts), 1)
        self.assertTrue((store / "owned-by-first").is_file())
        self.assertEqual(json.loads(result_file.read_text()), {"attempt": 1})
        self.assertIn("process was not reaped; retry disabled", str(raised.exception.primary))
        self.assertIn("unreaped", str(raised.exception))
        attempts = raised.exception.artifact["attempts"]
        self.assertEqual(len(attempts), 1)
        self.assertIsNone(attempts[0]["exit_code"])
        self.assertEqual(attempts[0]["timeout_class"], "fixture_timeout")
        self.assert_closed()

    def test_first_timeout_then_second_failure_retains_registered_attempt_history(self):
        for failure_mode in ("nonzero", "spawn"):
            with self.subTest(failure=failure_mode):
                work = self.root / failure_mode
                work.mkdir()
                self.scripts = [{"waits": [timeout(), 0]},
                                {"waits": [9]} if failure_mode == "nonzero" else {"spawn_error": True}]
                def observe_registered_attempt(command, stdout, stderr):
                    attempt = process_lifecycle.current_scope().attempts[-1]
                    self.assertEqual(attempt["command"], command)
                    self.assertEqual(attempt["timeout_seconds"], 3)
                    self.assertIsNone(attempt["exit_code"])
                    return self.spawn(command, stdout, stderr)

                @runner.owned_case
                def case():
                    return {"attempts": runner.run_command_with_attempts(
                        ["fixture", "dial"], work / "dial.log", "echo", "dial", 3)}

                with patch.object(process_lifecycle.subprocess, "Popen", side_effect=observe_registered_attempt):
                    with self.assertRaises(runner.CaseFailure) as raised:
                        case()
                failure = raised.exception
                self.assertNotIn("uncommitted_result", failure.artifact)
                first, second = failure.artifact["attempts"]
                self.assertEqual([first["attempt_id"], second["attempt_id"]], [1, 2])
                self.assertEqual(first["timeout_class"], "fixture_timeout")
                self.assertEqual(first["exit_code"], 0)
                self.assertEqual(first["terminal_status"]["termination"], "terminated")
                self.assertEqual(second["failure_class"], "process_exit" if failure_mode == "nonzero" else "spawn_error")
                self.assertEqual(second["exit_code"], 9 if failure_mode == "nonzero" else None)
                for attempt in (first, second):
                    self.assertEqual(attempt["timeout_seconds"], 3)
                    self.assertEqual(attempt["scenario_id"], "echo")
                    self.assertIn("log_tail", attempt)
                    self.assertTrue(Path(attempt["log_file"]).is_file())
                indexed = {entry["path"] for entry in runner.evidence_index(work, [failure.artifact])}
                self.assertEqual(indexed, {"dial.log", "dial-attempt-2.log"})
                self.assert_closed()

    def test_second_attempt_without_result_cannot_reuse_first_result(self):
        self.scripts = [{"waits": [timeout(), 0], "result": {"status": "ok", "attempt": 1}},
                        {"write_result": False}]
        with self.assertRaises(runner.CaseFailure) as raised:
            runner.run_dial(Path("fixture"), "forge", "echo", "peer", "/ip4/127.0.0.1/tcp/1", self.root)
        failure = raised.exception
        self.assertIsInstance(failure.primary, FileNotFoundError)
        self.assertFalse((self.root / "forge-dial-echo.json").exists())
        first, second = failure.artifact["attempts"]
        first_output = first["outputs"][0]
        self.assertEqual(json.loads(Path(first_output["log_file"]).read_text()), {"status": "ok", "attempt": 1})
        self.assertFalse(second["outputs"][0]["exists"])
        self.assertNotIn("log_file", second["outputs"][0])
        self.assert_closed()

    def test_failed_artifact_indexes_raw_process_logs_results_and_hashes(self):
        self.scripts = [{"ready": False, "malformed_ready": True, "waits": [timeout(), 0]}]
        @runner.owned_case
        def case():
            runner.start_listener(Path("fixture"), "listener", self.root,
                                  "echo", self.root / "result.json")
            return {}

        artifacts, failures = [], []
        with self.assertRaises(runner.CaseFailure) as raised:
            case()
        runner.record_case_failure(artifacts, failures, "fixture-case", raised.exception)
        artifact_file = self.root / "artifact.json"
        runner.write_artifact(artifact_file, self.root, {}, artifacts, failures, ["controlled-runner"], 0, None)
        artifact = json.loads(artifact_file.read_text())
        self.assertEqual(artifact["artifacts"][0]["status"], "failed")
        index = {entry["path"]: entry for entry in artifact["evidence_index"]}
        self.assertEqual(set(index), {"listener.log", "listener.log.ready-file.json", "listener.log.result-file.json"})
        for name, entry in index.items():
            data = (self.root / name).read_bytes()
            self.assertEqual(entry["size"], len(data))
            self.assertEqual(entry["sha256"], hashlib.sha256(data).hexdigest())
        (self.root / "listener.log").write_text("tampered\n")
        self.assertNotEqual(index["listener.log"]["sha256"], runner.sha256_file(self.root / "listener.log"))
        self.assert_closed()

    def test_unexpected_close_exception_keeps_primary_and_closes_other_processes(self):
        primary = ValueError("primary failure")
        @runner.owned_case
        def case():
            self.listener("first")
            second = self.listener("second")
            close = second.close
            def unexpected_close():
                close()
                raise RuntimeError("unexpected close exception")
            second.close = unexpected_close
            scope = process_lifecycle.current_scope()
            self.assertIn("unexpected close exception", "; ".join(scope.close()))
            events = list(self.events)
            scope.close()
            self.assertEqual(self.events, events)
            raise primary

        with self.assertRaises(runner.CaseFailure) as raised:
            case()
        self.assertIs(raised.exception.primary, primary)
        self.assertIn("unexpected close exception", str(raised.exception))
        self.assertEqual([event[0] for event in self.events if event[1] == "wait"], [101, 100])
        self.assert_closed()

    def test_standalone_dial_owns_process_and_keeps_raw_payload_shape(self):
        result = runner.run_dial(Path("fixture"), "forge", "echo", "peer", "/ip4/127.0.0.1/tcp/1", self.root)
        self.assertEqual({key: value for key, value in result.items() if key not in {"attempts", "result_file"}},
                         {"status": "ok"})
        self.assertEqual(result["attempts"][0]["owned_processes"][0]["terminal_status"]["exit_code"], 0)
        self.assert_closed()

    def test_graceful_nonzero_exit_is_failure_without_escalation(self):
        self.scripts = [{"waits": [7]}]
        @runner.owned_case
        def case():
            self.listener()
            return {"status": "ok"}

        with self.assertRaises(runner.CaseFailure) as raised:
            case()
        self.assertIn("terminal exit code 7", str(raised.exception))
        self.assertFalse(any(event[1] in {"signal", "kill"} for event in self.events))
        self.assert_closed()

    def test_log_open_failure_writes_failed_artifact_without_invented_evidence(self):
        log_file = self.root / "forbidden.log"
        primary = PermissionError("controlled log open denial")
        open_path = Path.open
        def deny_log_open(path, *args, **kwargs):
            if path == log_file:
                raise primary
            return open_path(path, *args, **kwargs)

        @runner.owned_case
        def case():
            return {"attempts": runner.run_command_once(["fixture", "dial"], log_file, "echo", "dial", 1)}

        with patch.object(Path, "open", new=deny_log_open):
            with self.assertRaises(runner.CaseFailure) as raised:
                case()
        failure = raised.exception
        self.assertIs(failure.primary, primary)
        self.assertEqual(self.processes, [])
        self.assertFalse(log_file.exists())
        attempt = failure.artifact["attempts"][0]
        self.assertEqual(attempt["requested_log_file"], str(log_file))
        self.assertEqual(attempt["failure_class"], "log_open_error")
        self.assertNotIn("log_file", attempt)
        self.assertNotIn("log_file", failure.artifact["processes"][0])
        artifacts, failures = [], []
        runner.record_case_failure(artifacts, failures, "log-open", failure)
        artifact_file = self.root / "artifact.json"
        runner.write_artifact(artifact_file, self.root, {}, artifacts, failures, ["controlled-runner"], 0, None)
        artifact = json.loads(artifact_file.read_text())
        self.assertEqual(artifact["artifacts"][0]["primary_error"], str(primary))
        self.assertEqual(artifact["evidence_index"], [])
        self.assertIn(str(primary), artifact["failures"][0])
        self.assertFalse(log_file.exists())
        self.assert_closed()

    def test_registered_log_disappearance_still_rejects_artifact(self):
        @runner.owned_case
        def case():
            self.listener()
            return {"status": "ok"}

        result = case()
        log_file = Path(result["owned_processes"][0]["log_file"])
        log_file.unlink()
        with self.assertRaisesRegex(RuntimeError, "interop evidence is missing"):
            runner.write_artifact(self.root / "artifact.json", self.root, {}, [result], [],
                                  ["controlled-runner"], 0, None)
        self.assert_closed()


if __name__ == "__main__":
    unittest.main()
