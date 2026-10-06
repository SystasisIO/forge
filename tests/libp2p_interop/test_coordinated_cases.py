"""Synthetic control/ownership tests, not native coordinated TCP acceptance."""

from copy import deepcopy
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import patch

import coordinated_cases as cases
from process_lifecycle import current_scope
from test_coordinated_evidence import go_result, ready, rust_result


class Rig:
    def __init__(self, fail=None):
        self.fail, self.owners, self.events = fail, [], []
        self.addresses = {"client": ("11.0.0.1",), "server": ("11.0.0.2",)}
        self.released = False

    def setup(self):
        self.events.append("network.setup")
        if self.fail == "setup":
            raise RuntimeError("synthetic setup failure")

    def namespace_command(self, role, argv):
        return ["/unit/ip", "netns", "exec", f"unit-{role}", *argv]

    def arm(self, ports):
        self.events.append("network.arm")
        self.ports = ports
        if self.fail == "arm":
            raise RuntimeError("synthetic second rule failure")

    def await_syn_sent(self, deadline):
        self.events.append("network.syn_sent")
        if self.fail == "syn_sent":
            raise RuntimeError("synthetic missing kernel socket")

    def release(self):
        self.events.append("network.release")
        if self.fail == "release":
            raise RuntimeError("synthetic partial rule deletion")
        self.released = True

    def close(self):
        self.events.append("network.close")
        if any(not o.closed for o in self.owners):
            raise RuntimeError("network deleted before actor join")
        return ["synthetic network cleanup failure"] if self.fail == "network_close" else []

    def evidence(self):
        return {"kind": "synthetic_unit_input_not_live_network", "coordination": {"released": self.released}}

    def spawn(self, command, log, stop_file, attempt, *, stop_budget):
        role = "source" if command[command.index("--coord-role") + 1] == "initiator" else "destination"
        if self.fail == "second_spawn" and self.owners:
            raise RuntimeError("synthetic second spawn failure")
        implementation = Path(command[4]).name
        owner = FakeOwner(self, implementation, role, command, log, stop_file, stop_budget)
        self.owners.append(owner)
        current_scope().processes.append(owner)
        if attempt is not None:
            attempt.update(pid=owner.process.pid, log_file=str(log), terminal_status=owner.terminal_status, outputs=[])
        self.events.append(f"spawn.{role}")
        return owner

    def wait(self, owner, path, predicate, deadline, *, capture=None):
        if path.name.endswith(".ready"):
            value = ready(owner.implementation, owner.role)
            value["case_token"] = owner.token
            if self.fail == "ready" and owner.role == "destination":
                value["listener_port"] = 45000
        else:
            starts = [o.control.exists() and "action=start" in o.control.read_text() for o in self.owners]
            self_assert = all(starts) and len(starts) == 2
            if not self.released:
                if not self_assert:
                    raise AssertionError("runner waited for one start before publishing both")
                value = {"implementation": owner.implementation, "case_token": owner.token, "status": "started",
                         "operation_admitted": True, "admission_source": "coordinated_actor.preflight"}
                if owner.implementation == "rust":
                    value["events"] = [{"kind": "control_completed", "control_sequence": 1,
                                        "action": "start", "completion": "native_dial_admitted"}]
                self.events.append(f"start_ack.{owner.role}")
            else:
                value = owner.result()
                probe = any("action=probe" in o.control.read_text() for o in self.owners)
                value["status"] = "exchanged" if probe else "connected"
                if probe:
                    source = next(o for o in self.owners if o.role == "source")
                    destination = next(o for o in self.owners if o.role == "destination")
                    if "action=probe" not in source.control.read_text() or "action=start" not in destination.control.read_text():
                        raise AssertionError("probe is not exclusively on the retained initiator")
                self.events.append(f"{value['status']}.{owner.role}")
                if self.fail == "connected" and owner.role == "destination":
                    if owner.implementation == "rust":
                        value["socket"]["remote_address"] = "/ip4/11.0.0.1/tcp/45000"
                    else:
                        value["receipt"]["connection"]["remote_address"] = "/ip4/11.0.0.1/tcp/45000"
        if not predicate(value):
            raise AssertionError("synthetic phase does not match requested predicate")
        if capture is not None:
            capture.write_text(json.dumps(value))
        return value


class FakeOwner:
    def __init__(self, rig, implementation, role, command, log, stop_file, stop_budget):
        self.rig, self.implementation, self.role = rig, implementation, role
        self.command, self.log_file, self.stop_file, self.stop_budget = command, log, stop_file, stop_budget
        self.token = command[command.index("--case-token") + 1]
        self.control = Path(command[command.index("--control-file") + 1])
        self.result_path = Path(command[command.index("--result-file") + 1])
        self.ready, self.closed, self.cleanup_errors = {}, False, []
        self.process = SimpleNamespace(pid=100 + len(rig.owners), poll=lambda: 0 if self.closed else None)
        self.terminal_status = {"exit_code": None, "termination": "running"}

    def result(self):
        value = rust_result(self.role) if self.implementation == "rust" else go_result(self.role)
        value["implementation"], value["case_token"] = self.implementation, self.token
        value["scenario"] = self.command[self.command.index("--scenario") + 1]
        value["joined"] = self.closed
        if self.implementation == "forge":
            value["receipt"]["connection"]["source"] = "forge.node.diagnostics.authenticated-session"
            value["receipt"]["direction_source"] = "forge.node.diagnostics.session.direction"
            value["receipt"]["roles"], value["receipt"]["role_source"] = None, None
        return value

    def close(self):
        if self.closed:
            return self.cleanup_errors
        if not all(o.stop_file.exists() for o in self.rig.owners):
            raise AssertionError("both stop requests must precede the first process join")
        self.closed = True
        self.rig.events.append(f"join.{self.role}")
        self.terminal_status.update(exit_code=0, termination="graceful")
        self.result_path.write_text(json.dumps(self.result()))
        if self.rig.fail == "actor_close" and self.role == "destination":
            self.cleanup_errors.append("synthetic forced SIGTERM")
        return self.cleanup_errors

    def evidence(self):
        return {"pid": self.process.pid, "command": self.command, "log_file": str(self.log_file), "ready": self.ready,
                "terminal_status": self.terminal_status, "outputs": []}


class CoordinatedCasesTests(unittest.TestCase):
    def run_unit(self, spec=None, fail=None, private=False, command_attempt=None):
        spec = spec or list(cases.case_specs())[4 if private else 0]
        rig = Rig(fail)
        with tempfile.TemporaryDirectory() as temporary:
            key = Path(temporary) / "unit-key"
            key.write_text("synthetic key input, never passed to real actors")
            support = {"pnet_key_file": key, "pnet_fingerprint": "b" * 64} if spec.profile == "private" else {}
            with patch.object(cases, "spawn_owned", side_effect=rig.spawn), patch.object(cases, "_await", side_effect=rig.wait), \
                    patch.object(cases, "validate_case", return_value=["synthetic unit harness is not native acceptance"]):
                artifact = cases.run_case(spec, {n: f"/unit/{n}" for n in ("forge", "go", "rust")}, temporary,
                                          network_factory=lambda: rig, command_attempt=command_attempt, **support)
        self.assertIsNone(current_scope())
        self.assertEqual(artifact["status"], "failed")
        return artifact, rig

    def test_exact_eight_cases_use_existing_profile_runner_ids(self):
        specs = list(cases.case_specs())
        self.assertEqual(len(specs), 8)
        self.assertEqual({s.profile for s in specs}, {"native", "private"})
        self.assertEqual(len({s.identifier for s in specs}), 8)
        for spec in specs:
            artifact, rig = self.run_unit(spec)
            self.assertEqual(artifact["runner_scenario_id"], cases.REUSE_RUNNER_IDS[spec.profile])
            self.assertEqual(artifact["errors"], ["synthetic unit harness is not native acceptance"])
            self.assertEqual(rig.ports, {"client": 40010, "server": 40020})

    def test_start_both_then_kernel_release_then_connected_then_single_probe(self):
        artifact, rig = self.run_unit()
        events = rig.events
        self.assertLess(events.index("network.arm"), events.index("start_ack.source"))
        self.assertLess(events.index("start_ack.destination"), events.index("network.syn_sent"))
        self.assertLess(events.index("network.syn_sent"), events.index("network.release"))
        self.assertLess(events.index("network.release"), events.index("connected.source"))
        self.assertLess(events.index("connected.destination"), events.index("exchanged.source"))
        self.assertEqual([(r["actor"], r["action"]) for r in artifact["controls"]],
                         [("source", "start"), ("destination", "start"), ("source", "probe")])

    def test_all_commands_have_explicit_native_budget_and_no_ordinary_mode(self):
        artifact, rig = self.run_unit()
        for owner in rig.owners:
            command = owner.command
            self.assertEqual(command[5], "coordinated-live")
            self.assertEqual(command[command.index("--timeout-ms") + 1], "20000")
            self.assertNotIn("--pnet-key-file", command)
            self.assertNotIn("path-live", command)
            self.assertEqual("--store-dir" in command, owner.implementation == "forge")
            plan = artifact["plans"][owner.role]
            self.assertEqual(set(plan), {"case-token", "peer-id", "addr"})

    def test_private_requires_exact_key_inputs_but_does_not_record_key_contents(self):
        artifact, rig = self.run_unit(private=True)
        self.assertEqual(artifact["pnet_fingerprint"], "b" * 64)
        for owner in rig.owners:
            self.assertIn("--pnet-key-file", owner.command)
            self.assertEqual(owner.command[owner.command.index("--transport") + 1], "tcp-pnet-noise")
        self.assertNotIn("synthetic key input", json.dumps(artifact))

    def test_join_both_actors_before_deleting_the_owned_network(self):
        _, rig = self.run_unit()
        self.assertLess(rig.events.index("join.source"), rig.events.index("network.close"))
        self.assertLess(rig.events.index("join.destination"), rig.events.index("network.close"))

    def test_command_attempts_receive_only_the_actual_joined_exit_code(self):
        def attempt(command, log, scenario, number, role, timeout):
            value = {"command": command, "requested_log_file": str(log), "scenario_id": scenario,
                     "attempt_id": number, "kind": role, "timeout_seconds": timeout, "exit_code": None}
            current_scope().attempts.append(value)
            return value
        artifact, _ = self.run_unit(command_attempt=attempt)
        self.assertEqual(len(artifact["attempts"]), 2)
        self.assertEqual([a["exit_code"] for a in artifact["attempts"]], [0, 0])

    def test_each_partial_failure_keeps_cleanup_and_resets_process_scope(self):
        for failure in ("setup", "second_spawn", "ready", "arm", "syn_sent", "release", "connected"):
            with self.subTest(failure=failure):
                artifact, rig = self.run_unit(fail=failure)
                self.assertTrue(artifact["errors"])
                self.assertIn("network.close", rig.events)
                self.assertTrue(all(o.closed for o in rig.owners))
                self.assertFalse(any(r["action"] == "probe" for r in artifact["controls"]))

    def test_forced_process_or_network_cleanup_is_never_success(self):
        for failure in ("actor_close", "network_close"):
            artifact, _ = self.run_unit(fail=failure)
            self.assertTrue(artifact["cleanup_errors"])

    def test_existing_workdir_is_not_reused_or_overwritten(self):
        spec = next(cases.case_specs())
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary) / spec.identifier
            work.mkdir()
            sentinel = work / "source.ready"
            sentinel.write_text("previous raw observation")
            factory = unittest.mock.Mock()
            artifact = cases.run_case(spec, {}, temporary, network_factory=factory)
            self.assertEqual(artifact["status"], "failed")
            self.assertEqual(sentinel.read_text(), "previous raw observation")
            factory.assert_not_called()
        self.assertIsNone(current_scope())

    def test_private_inputs_and_unowned_case_fail_before_any_process(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaises(ValueError):
                cases.run_case(list(cases.case_specs())[4], {}, temporary)
            with self.assertRaises(ValueError):
                cases.run_case(next(cases.case_specs()), {}, temporary, pnet_fingerprint="b" * 64)
            with self.assertRaises(ValueError):
                cases.run_case(cases.Case("unowned", "go", "rust"), {}, temporary)
        self.assertIsNone(current_scope())

    def test_control_and_capture_bounds_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "unit.control"
            with self.assertRaises(ValueError):
                cases._write(path, {"action": "start\nprobe"})
            path.write_text("[]")
            with self.assertRaises(ValueError):
                cases._read(path)

    def test_phase_snapshot_preserves_the_exact_actor_bytes_not_rebuilt_json(self):
        with tempfile.TemporaryDirectory() as temporary:
            path, capture = Path(temporary) / "actor.result", Path(temporary) / "source.started.json"
            raw = b'{ "status" : "started", "actor_value" : 17 }\n'
            path.write_bytes(raw)
            owner = SimpleNamespace(process=SimpleNamespace(poll=lambda: None))
            value = cases._await(owner, path, lambda v: v["status"] == "started", cases.time.monotonic() + 1, capture=capture)
            self.assertEqual(value["actor_value"], 17)
            self.assertEqual(capture.read_bytes(), raw)
            with self.assertRaises(FileExistsError):
                cases._await(owner, path, lambda v: True, cases.time.monotonic() + 1, capture=capture)


if __name__ == "__main__":
    unittest.main()
