"""Case inventory regressions, not native interoperability results."""

from collections import Counter
from copy import deepcopy
from io import StringIO
import json
from pathlib import Path
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from process_lifecycle import Listener, StopBudget, current_scope
from pubsub_cases import _await, _prepare_all, _ready, _stop_prepared, _subscription_received, case_specs, run_case
from pubsub_evidence import AUTHENTICATION, GO_QUIC_SOURCES, _same_json, shutdown_ack
from test_pubsub_evidence import synthetic_case


class PubSubCaseTests(unittest.TestCase):
    def subscription_documents(self, *, lower=True, implementation="go"):
        artifact = synthetic_case(lower_quic=lower)
        raw = artifact["raw"]["replacement" if implementation == "go" else "victim"]
        ack = next(value for value in raw["events"] if value["kind"] == "shutdown_prepared")
        raw.update(finalized=False, joined=False, events=raw["events"][:ack["sequence"] - 1])
        if implementation == "rust":
            raw["implementation"] = "rust"
            for event in raw["events"]:
                event["source"] = "rust.libp2p.passive-upgraded-stream-io" if event["kind"] in {"rpc", "protocol", "connection"} \
                    else "rust.libp2p.gossipsub.public-Behaviour"
                if event["kind"] == "connection":
                    event["authentication_basis"] = AUTHENTICATION["rust"]["quic" if lower else "tcp"]
        remote = artifact["raw"]["victim" if implementation == "go" else "replacement"]["local_peer_id"]
        args = (remote, "/meshsub/1.1.0", "forge-pr11:" + artifact["case_token"], "quic" if lower else "tcp")
        expected = {"implementation": implementation, "local_peer": raw["local_peer_id"]}
        return raw, args, expected

    def test_subscription_uses_same_actual_lower_owner_and_preserves_legacy_sources(self):
        for lower, implementation in ((True, "go"), (False, "go"), (True, "forge"), (False, "forge"),
                                      (True, "rust"), (False, "rust")):
            raw, args, expected = self.subscription_documents(lower=lower, implementation=implementation)
            before = deepcopy(raw)
            with self.subTest(lower=lower, implementation=implementation):
                self.assertTrue(_subscription_received(raw, *args, **expected))
                self.assertTrue(_same_json(before, raw))
                self.assertFalse(_subscription_received(raw, "different-authenticated-remote", *args[1:], **expected))

    def test_lower_subscription_rejects_identity_source_schema_and_owner_aliases(self):
        modes = ("conflict", "legacy_source", "foreign_source", "direction", "swarm_mapping", "boolean_stream_id",
                 "future_connection", "future_stream", "boolean_reference", "missing_owner", "wrong_authentication",
                 "wrong_key", "wrong_address", "wrong_protocol", "wrong_local_actor", "duplicate_connection", "duplicate_stream",
                 "duplicate_protocol", "float_clock")
        for mode in modes:
            raw, args, expected = self.subscription_documents()
            events = raw["events"]
            rpc = next(value for value in events if value["kind"] == "rpc" and value["direction"] == "read"
                       and value["remote_peer_id"] == args[0])
            connection = events[rpc["connection_receipt_sequence"] - 1]
            stream = events[rpc["native_stream_receipt_sequence"] - 1]
            protocol = next(value for value in events if value["source"] == GO_QUIC_SOURCES["protocol"]
                            and value["native_connection_id"] == rpc["native_connection_id"])
            if mode == "conflict":
                rpc["peer_id"] = "foreign-peer"
            elif mode == "legacy_source":
                rpc["source"] = "go.pubsub.native_stream.read"
            elif mode == "foreign_source":
                rpc["source"] = "rust.libp2p.passive-upgraded-stream-io"
            elif mode == "direction":
                rpc["direction"] = "write"
            elif mode == "swarm_mapping":
                rpc["stream_id"] = "invented-swarm-stream"
            elif mode == "boolean_stream_id":
                rpc["native_stream_id"] = False
            elif mode == "future_connection":
                rpc["connection_receipt_sequence"] = rpc["sequence"] + 1
            elif mode == "future_stream":
                rpc["native_stream_receipt_sequence"] = rpc["sequence"] + 1
            elif mode == "boolean_reference":
                rpc["native_stream_receipt_sequence"] = True
            elif mode == "missing_owner":
                connection["kind"] = "missing_connection"
            elif mode == "wrong_authentication":
                connection["authentication_basis"] = "configured_TLS"
            elif mode == "wrong_key":
                connection["remote_public_key_sha256"] = "f" * 64
            elif mode == "wrong_address":
                connection["remote_address"] = "/ip4/127.0.0.1/tcp/1"
            elif mode == "wrong_protocol":
                protocol["protocol"] = "/meshsub/1.0.0"
            elif mode == "wrong_local_actor":
                raw["local_peer_id"] = "foreign-local-actor"
            elif mode == "float_clock":
                protocol["mono_ns"] = float(protocol["mono_ns"])
            else:
                original = {"duplicate_connection": connection, "duplicate_stream": stream, "duplicate_protocol": protocol}[mode]
                events.append({**deepcopy(original), "sequence": len(events) + 1, "mono_ns": len(events) + 1})
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                _subscription_received(raw, *args, **expected)

    def test_subscription_does_not_return_early_over_a_later_conflicting_receipt(self):
        raw, args, expected = self.subscription_documents()
        rpc = next(value for value in raw["events"] if value["kind"] == "rpc")
        raw["events"].append({**deepcopy(rpc), "sequence": len(raw["events"]) + 1,
                              "mono_ns": len(raw["events"]) + 1, "peer_id": "foreign-peer"})
        with self.assertRaisesRegex(ValueError, "conflicting"):
            _subscription_received(raw, *args, **expected)

    def shutdown_actors(self, root, *, implementations=None, finalize=None, exit_codes=None):
        actors, documents, stops = {}, {}, []
        token = "a" * 32
        for pid, role in enumerate(("victim", "offender", "replacement", "sink"), 1):
            implementation = implementations[role] if implementations is not None else "forge" if role == "victim" else "rust"
            ready = {"implementation": implementation, "actor": role, "case_token": token,
                     "schema_version": 1, "peer_id": "synthetic-unit-" + role, "local_peer_id": "synthetic-unit-" + role,
                     "ready": True, "subscription_created": True, "topic": "forge-pr11:" + token,
                     "listen_addrs": ["/ip4/127.0.0.1/tcp/1234"]}
            files = {name: Path(root) / (role + "." + name) for name in ("result", "control", "stop")}
            raw = {"schema_version": 1, "implementation": implementation, "actor": role, "case_token": token,
                   "local_peer_id": ready["peer_id"], "finalized": False, "joined": False, "overflow": False, "error": None,
                   "events": [{"sequence": 1, "mono_ns": 1, "kind": "shutdown_prepared", "source": implementation + ".fixture.prepare_shutdown",
                               "command_sequence": 2, "actor": role, "case_token": token, "local_peer_id": ready["peer_id"],
                               "admission_closed": True, "pending_commands": 0},
                              {"sequence": 2, "mono_ns": 2, "kind": "command_done", "command_sequence": 2,
                               "command_kind": "prepare_shutdown", "status": "ok",
                               "source": {"forge": "forge.fixture.native_operation", "rust": "rust.fixture.control-native-operation-completion",
                                          "go": "go.fixture.append_only_control"}[implementation]}]}
            if implementation == "go":
                raw.update(pid=pid, active_stream_handlers_and_io=0, active_fixture_workers=1, active_callbacks=0)
            files["result"].write_text(json.dumps(raw))
            state = {"exit_code": None}
            process = SimpleNamespace(pid=pid, poll=lambda state=state: state["exit_code"])
            owner = Listener(process, ready, files["stop"], Path(root) / (role + ".log"), StringIO(),
                             ["synthetic-unit-fixture", "--result-file", str(files["result"])], stop_budget=StopBudget(8, 0, 2))
            owner.wait_observations = []
            publish = owner.request_stop
            def request_stop(role=role, owner=owner, publish=publish):
                was_requested = owner._stop_requested
                publish()
                if not was_requested:
                    stops.append(role)
            owner.request_stop = request_stop
            def wait(timeout, role=role, owner=owner, files=files, state=state):
                owner.wait_observations.append((tuple(stops), timeout))
                terminal = deepcopy(documents[role])
                terminal.update(finalized=True, joined=True)
                if owner.ready["implementation"] == "rust":
                    terminal["native_close"] = {"live_muxers": 0, "live_streams": 0, "connections": [
                        {"connection_id": "synthetic-native-" + role, "dropped": True, "close_returned": True}]}
                    terminal["task_join"] = {"fixture_owned_tasks_joined": True, "overflow": False, "errors": []}
                    fields = {"kind": "shutdown_requested", "source": "rust.fixture.native_host_close", "listeners": 1}
                elif owner.ready["implementation"] == "go":
                    terminal.update(host_close_returned=True, active_stream_handlers_and_io=0,
                                    active_fixture_workers=0, active_callbacks=0)
                    fields = {"kind": "shutdown", "source": "go.fixture.owned_context_cancel_and_drain",
                              "context_cancelled": True, "joined": True, "host_close_returned": True,
                              "active_stream_handlers_and_io": 0, "active_fixture_workers": 0}
                if owner.ready["implementation"] in {"rust", "go"}:
                    sequence = len(terminal["events"]) + 1
                    terminal["events"].append({**fields, "sequence": sequence, "mono_ns": sequence})
                if finalize is not None:
                    finalize(role, terminal)
                files["result"].write_text(json.dumps(terminal))
                state["exit_code"] = 0 if exit_codes is None else exit_codes[role]
                return state["exit_code"]
            process.wait = wait
            actors[role], documents[role] = (owner, files), raw
        return actors, documents, stops

    def acknowledge_quiesce(self, actors, documents, stops, owner, predicate):
        """Synthetic ACK only after the actual test control command is present."""
        self.assertEqual(stops, [])
        for candidate, files in actors.values():
            if candidate.ready["implementation"] == "go":
                self.assertEqual(json.loads(files["control"].read_text().splitlines()[-1])["kind"], "quiesce_shutdown")
        role = owner.ready["actor"]
        value = documents[role]
        command = json.loads(actors[role][1]["control"].read_text().splitlines()[-1])
        self.assertEqual(command["kind"], "quiesce_shutdown")
        if not any(event["kind"] == "shutdown_quiesced" for event in value["events"]):
            sequence = len(value["events"]) + 1
            value["events"].append({"sequence": sequence, "mono_ns": sequence,
                                    "kind": "pre_cancel_retained_resets_returned",
                                    "source": "go.fixture.owned_pre_cancel_retained_resets", "actor": role,
                                    "case_token": command["case_token"], "local_peer_id": command["local_peer_id"],
                                    "pid": owner.process.pid, "command_sequence": command["sequence"],
                                    "prepare_ack_sequence": command["prepare_ack_sequence"],
                                    "retained_owners": [], "reset_return_receipt_sequences": [],
                                    "native_admission_closed": True, "pubsub_callback_admission_closed": True,
                                    "pubsub_context_cancelled": False, "subscriber_context_cancelled": False,
                                    "active_stream_handlers_and_io": 0, "active_pubsub_streams": 0,
                                    "active_callbacks": 0,
                                    "phase_scope": "retained_stream_Reset_returns_not_IO_framing_callback_lower_QUIC_or_router_join"})
            sequence += 1
            value["events"].append({"sequence": sequence, "mono_ns": sequence, "kind": "shutdown_quiesced",
                                    "source": "go.fixture.owned_pubsub_quiesce", "actor": role,
                                    "case_token": command["case_token"], "local_peer_id": command["local_peer_id"],
                                    "pid": owner.process.pid, "command_sequence": command["sequence"],
                                    "prepare_ack_sequence": command["prepare_ack_sequence"], "native_admission_closed": True,
                                    "pubsub_callback_admission_closed": True,
                                    "pubsub_context_cancelled": True, "subscriber_context_cancelled": True,
                                    "active_stream_handlers_and_io": 0, "active_pubsub_streams": 0,
                                    "active_fixture_workers": 0, "active_callbacks": 0,
                                    "joined_scope": "fixture_subscriber_admitted_stream_IO_framing_pending_terminal_and_observer_callbacks"})
            value["events"].append({"sequence": sequence + 1, "mono_ns": sequence + 1, "kind": "command_done",
                                    "source": "go.fixture.append_only_control", "command_sequence": command["sequence"],
                                    "command_kind": "quiesce_shutdown", "status": "ok"})
            value.update(active_stream_handlers_and_io=0, active_fixture_workers=0, active_callbacks=0)
            actors[role][1]["result"].write_text(json.dumps(value))
        self.assertTrue(predicate(value))
        return value

    def test_all_four_actual_commands_and_acks_precede_first_stop(self):
        with tempfile.TemporaryDirectory() as root:
            actors, documents, stops = self.shutdown_actors(root)
            def wait(owner, path, predicate, deadline):
                self.assertEqual(stops, [])
                for role, (_, files) in actors.items():
                    command = json.loads(files["control"].read_text())
                    self.assertEqual(command, {"sequence": 2, "kind": "prepare_shutdown", "actor": role,
                                              "case_token": "a" * 32, "local_peer_id": actors[role][0].ready["peer_id"]})
                value = documents[owner.ready["actor"]]
                self.assertTrue(predicate(value))
                return value
            with patch("pubsub_cases._await", side_effect=wait):
                barrier = _prepare_all(actors, {role: 1 for role in actors}, "a" * 32, 10)
            self.assertEqual(stops, [])
            for row in barrier["operations"]:
                snapshot = json.loads(Path(row["evidence_file"]).read_text())
                self.assertEqual(snapshot["result"], documents[row["actor"]])
                self.assertEqual(snapshot["pid"], actors[row["actor"]][0].process.pid)
            _stop_prepared(actors, barrier)
            self.assertEqual(stops, ["offender", "replacement", "sink", "victim"])
            self.assertEqual([row["kind"] for row in barrier["operations"]],
                             ["prepare_ack"] * 4 + ["stop_requested"] * 3 + ["donor_joined"] * 3 + ["stop_requested"])
            self.assertEqual([row["actor"] for row in barrier["operations"] if row["kind"] == "stop_requested"], stops)
            for role in ("offender", "replacement", "sink"):
                owner = actors[role][0]
                self.assertEqual(owner.wait_observations, [(tuple(stops[:3]), owner.stop_budget.seconds)])
                self.assertTrue(owner.closed)
                self.assertEqual(owner.terminal_status, {"exit_code": 0, "termination": "graceful"})
                self.assertTrue(owner.outputs[0]["exists"])
            self.assertEqual(actors["victim"][0].wait_observations, [])

    def test_all_go_and_rust_stops_precede_any_wait_and_actual_joins_precede_forge_stop(self):
        configurations = ({"victim": "go"}, {"victim": "rust"},
                          {"offender": "go", "replacement": "go", "sink": "go"},
                          {"victim": "rust", "offender": "go"}, {})
        for configuration in configurations:
            with self.subTest(configuration=configuration), tempfile.TemporaryDirectory() as root:
                implementations = {role: configuration.get(role, "forge")
                                   for role in ("victim", "offender", "replacement", "sink")}
                actors, documents, stops = self.shutdown_actors(root, implementations=implementations)
                donors = tuple(role for role in actors if implementations[role] != "forge")
                forge = tuple(role for role in actors if implementations[role] == "forge")
                for role in forge:
                    owner = actors[role][0]
                    publish = owner.request_stop
                    def forge_stop(publish=publish):
                        for donor in donors:
                            candidate = actors[donor][0]
                            self.assertTrue(candidate.closed)
                            self.assertEqual(candidate.process.poll(), 0)
                            self.assertEqual(candidate.terminal_status, {"exit_code": 0, "termination": "graceful"})
                        publish()
                    owner.request_stop = forge_stop
                with patch("pubsub_cases._await", side_effect=lambda owner, path, predicate, deadline: documents[owner.ready["actor"]]):
                    barrier = _prepare_all(actors, {role: 1 for role in actors}, "a" * 32, 10)
                with patch("pubsub_cases._await", side_effect=lambda owner, path, predicate, deadline:
                           self.acknowledge_quiesce(actors, documents, stops, owner, predicate)):
                    _stop_prepared(actors, barrier)
                self.assertEqual(stops, list(donors + forge))
                self.assertEqual([row["actor"] for row in barrier["operations"] if row["kind"] == "stop_requested"], stops)
                self.assertEqual([row["actor"] for row in barrier["operations"] if row["kind"] == "donor_joined"], list(donors))
                self.assertEqual([row["kind"] for row in barrier["operations"]], ["prepare_ack"] * 4
                                 + ["quiesce_requested"] * sum(native == "go" for native in implementations.values())
                                 + ["quiesce_ack"] * sum(native == "go" for native in implementations.values())
                                 + ["stop_requested"] * len(donors) + ["donor_joined"] * len(donors)
                                 + ["stop_requested"] * len(forge))
                for row in barrier["operations"]:
                    if row["kind"] == "donor_joined":
                        owner, files = actors[row["actor"]]
                        raw = json.loads(files["result"].read_text())
                        event = raw["events"][row["shutdown_event_sequence"] - 1]
                        self.assertEqual(event["kind"], "shutdown" if implementations[row["actor"]] == "go" else "shutdown_requested")
                        self.assertEqual((row["pid"], row["case_token"], row["local_peer_id"]),
                                         (owner.process.pid, owner.ready["case_token"], owner.ready["peer_id"]))
                for role, (owner, _) in actors.items():
                    self.assertEqual(owner.wait_observations,
                                     [(donors, owner.stop_budget.seconds)] if role in donors else [])
                    self.assertEqual(owner.stop_budget, StopBudget(8, 0, 2))

    def test_invalid_rust_final_never_releases_normal_non_rust_stop(self):
        failures = ("missing_result", "not_final", "missing_join", "unjoined", "missing_native_close",
                    "live_stream", "task_error", "opaque_error", "opaque_event", "foreign_identity",
                    "changed_ack", "nonzero", "boolean_exit", "forced_exit")
        for failure in failures:
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as root:
                def finalize(role, raw):
                    if role != "offender":
                        return
                    if failure == "not_final":
                        raw["finalized"] = False
                    elif failure == "missing_join":
                        raw.pop("joined")
                    elif failure == "unjoined":
                        raw["task_join"]["fixture_owned_tasks_joined"] = False
                    elif failure == "missing_native_close":
                        raw.pop("native_close")
                    elif failure == "live_stream":
                        raw["native_close"]["live_streams"] = 1
                    elif failure == "task_error":
                        raw["task_join"]["errors"] = ["actual task failure"]
                    elif failure == "opaque_error":
                        raw["error"] = "opaque native connection error"
                    elif failure == "opaque_event":
                        raw["events"].append({"sequence": 3, "mono_ns": 3, "kind": "native_io_error",
                                              "source": "rust.libp2p.passive-upgraded-stream-io", "operation": "muxer_inbound",
                                              "typed_cause": "quic_connection_cause_unavailable"})
                    elif failure == "foreign_identity":
                        raw["local_peer_id"] = "foreign-terminal-owner"
                    elif failure == "changed_ack":
                        raw["events"][0]["admission_closed"] = False
                exits = {role: 0 for role in ("victim", "offender", "replacement", "sink")}
                if failure == "nonzero":
                    exits["offender"] = 1
                elif failure == "boolean_exit":
                    exits["offender"] = False
                actors, documents, stops = self.shutdown_actors(root, finalize=finalize, exit_codes=exits)
                owner, files = actors["offender"]
                if failure == "missing_result":
                    native_wait = owner.process.wait
                    def missing_result(timeout):
                        code = native_wait(timeout)
                        files["result"].unlink()
                        return code
                    owner.process.wait = missing_result
                elif failure == "forced_exit":
                    native_close = owner.close
                    def forced_exit():
                        errors = native_close()
                        owner.terminal_status["termination"] = "terminated"
                        return errors
                    owner.close = forced_exit
                with patch("pubsub_cases._await", side_effect=lambda owner, path, predicate, deadline: documents[owner.ready["actor"]]):
                    barrier = _prepare_all(actors, {role: 1 for role in actors}, "a" * 32, 10)
                with self.assertRaises((ValueError, RuntimeError, OSError)):
                    _stop_prepared(actors, barrier)
                self.assertEqual(stops, ["offender", "replacement", "sink"])
                self.assertEqual([row["actor"] for row in barrier["operations"][4:]], stops)
                self.assertEqual([row["kind"] for row in barrier["operations"][:4]], ["prepare_ack"] * 4)
                self.assertEqual(actors["victim"][0].wait_observations, [])
                self.assertFalse(actors["victim"][1]["stop"].exists())

    def test_invalid_or_delayed_go_join_never_releases_normal_forge_stop(self):
        failures = ("host_close", "io", "workers", "callbacks", "raw_error", "shutdown_missing", "shutdown_failed",
                    "delayed_join", "timeout", "nonzero", "forced_exit", "last_donor_failed")
        for failure in failures:
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as root:
                failed_role = "sink" if failure == "last_donor_failed" else "offender"
                def finalize(role, raw):
                    if role != failed_role:
                        return
                    if failure == "host_close":
                        raw["host_close_returned"] = False
                    elif failure in {"io", "workers", "callbacks"}:
                        raw[{"io": "active_stream_handlers_and_io", "workers": "active_fixture_workers",
                             "callbacks": "active_callbacks"}[failure]] = 1
                    elif failure in {"raw_error", "last_donor_failed"}:
                        raw["error"] = "actual native error"
                    elif failure == "shutdown_missing":
                        raw["events"].pop()
                    elif failure == "shutdown_failed":
                        raw["events"][-1]["joined"] = False
                implementations = {role: "forge" if role == "victim" else "go"
                                   for role in ("victim", "offender", "replacement", "sink")}
                exits = {role: 2 if failure == "nonzero" and role == failed_role else 0 for role in implementations}
                actors, documents, stops = self.shutdown_actors(root, implementations=implementations,
                                                               finalize=finalize, exit_codes=exits)
                owner = actors[failed_role][0]
                if failure == "delayed_join":
                    owner.close = lambda: []
                elif failure == "timeout":
                    def timed_out():
                        raise TimeoutError("actual donor close did not return within its existing budget")
                    owner.close = timed_out
                elif failure == "forced_exit":
                    close = owner.close
                    def forced():
                        errors = close()
                        owner.terminal_status["termination"] = "terminated"
                        return errors
                    owner.close = forced
                with patch("pubsub_cases._await", side_effect=lambda owner, path, predicate, deadline: documents[owner.ready["actor"]]):
                    barrier = _prepare_all(actors, {role: 1 for role in actors}, "a" * 32, 10)
                with self.assertRaises((ValueError, RuntimeError, TimeoutError)):
                    with patch("pubsub_cases._await", side_effect=lambda owner, path, predicate, deadline:
                               self.acknowledge_quiesce(actors, documents, stops, owner, predicate)):
                        _stop_prepared(actors, barrier)
                self.assertEqual(stops, ["offender", "replacement", "sink"])
                joined = [row["actor"] for row in barrier["operations"] if row["kind"] == "donor_joined"]
                self.assertEqual(joined, ["offender", "replacement"] if failure == "last_donor_failed" else [])
                self.assertEqual(actors["victim"][0].wait_observations, [])
                self.assertFalse(actors["victim"][1]["stop"].exists())

    def test_failed_donor_join_keeps_harness_error_and_finally_cleans_every_owner(self):
        for implementation, missing_quiesce in (("go", False), ("rust", False), ("go", True)):
            with self.subTest(implementation=implementation, missing_quiesce=missing_quiesce), tempfile.TemporaryDirectory() as root:
                def fail_join(role, raw):
                    if role == "offender":
                        raw["joined"] = False
                implementations = {role: "forge" if role == "victim" else implementation
                                   for role in ("victim", "offender", "replacement", "sink")}
                actors, documents, stops = self.shutdown_actors(root, implementations=implementations, finalize=fail_join)
                def spawn(argv, log, stop_file, attempt, *, stop_budget):
                    role = argv[argv.index("--actor") + 1]
                    owner, files = actors[role]
                    for name in ("ready", "result", "control", "stop"):
                        files[name] = Path(argv[argv.index("--" + name + "-file") + 1])
                    owner.command, owner.log_file, owner.stop_file = argv, log, stop_file
                    self.assertEqual(owner.stop_budget, stop_budget)
                    files["ready"].write_text(json.dumps(owner.ready))
                    files["result"].write_text(json.dumps(documents[role]))
                    current_scope().processes.append(owner)
                    return owner
                def wait(owner, path, predicate, deadline):
                    role = owner.ready["actor"]
                    if path == actors[role][1]["ready"]:
                        self.assertTrue(predicate(owner.ready))
                        return owner.ready
                    value = documents[role]
                    control = actors[role][1]["control"]
                    if control.exists():
                        command = json.loads(control.read_text().splitlines()[-1])
                        if command["kind"] == "prepare_shutdown":
                            for event in value["events"]:
                                event["command_sequence"] = command["sequence"]
                            actors[role][1]["result"].write_text(json.dumps(value))
                            self.assertTrue(predicate(value))
                        elif command["kind"] == "quiesce_shutdown":
                            if missing_quiesce and role == "sink":
                                self.assertFalse(predicate(value))
                                raise TimeoutError("missing actual Go quiesce ACK")
                            return self.acknowledge_quiesce(actors, documents, stops, owner, predicate)
                    return value
                spec = next(case for case in case_specs() if case.source == implementation and case.destination == "forge"
                            and case.version == "1.1" and case.profile == "native_quic")
                with patch("pubsub_cases.secrets.token_hex", return_value="a" * 32), \
                        patch("pubsub_cases.spawn_owned", side_effect=spawn), \
                        patch("pubsub_cases._await", side_effect=wait), \
                        patch("pubsub_cases.validate_case") as acceptance:
                    artifact = run_case(spec, {"forge": "synthetic-forge", implementation: "synthetic-" + implementation},
                                        Path(root) / "cases")
                acceptance.assert_not_called()
                self.assertEqual(artifact["status"], "HARNESS_ERROR")
                self.assertTrue(any(("quiesce ACK" if missing_quiesce else "unjoined") in error for error in artifact["errors"]))
                self.assertNotIn("evidence", artifact)
                self.assertEqual([row["actor"] for row in artifact["shutdown_barrier"]["operations"]
                                 if row["kind"] == "stop_requested"],
                                 [] if missing_quiesce else ["offender", "replacement", "sink"])
                self.assertEqual(stops, ["victim", "offender", "replacement", "sink"] if missing_quiesce
                                 else ["offender", "replacement", "sink", "victim"])
                self.assertIsNone(current_scope())
                for role, (owner, files) in actors.items():
                    self.assertTrue(files["stop"].exists())
                    self.assertTrue(owner.closed)
                    self.assertEqual(len(owner.wait_observations), 1)
                    self.assertEqual(artifact["processes"][role]["returncode"], 0)

    def test_all_go_quiesce_commands_precede_wait_and_bad_last_ack_never_releases_any_stop(self):
        modes = ("missing", "timeout", "pid", "prepare", "callback_admission", "worker", "sticky", "duplicate", "exit", "changed_earlier_ack")
        for mode in modes:
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as root:
                implementations = {role: "forge" if role == "victim" else "go"
                                   for role in ("victim", "offender", "replacement", "sink")}
                actors, documents, stops = self.shutdown_actors(root, implementations=implementations)
                with patch("pubsub_cases._await", side_effect=lambda owner, path, predicate, deadline: documents[owner.ready["actor"]]):
                    barrier = _prepare_all(actors, {role: 1 for role in actors}, "a" * 32, 10)
                def wait(owner, path, predicate, deadline):
                    self.assertEqual(stops, [])
                    if owner.ready["actor"] == "sink" and mode in {"missing", "timeout"}:
                        self.assertFalse(predicate(documents["sink"]))
                        raise TimeoutError("quiesce did not acknowledge before existing deadline")
                    value = self.acknowledge_quiesce(actors, documents, stops, owner, predicate)
                    if owner.ready["actor"] == "sink":
                        ack = next(event for event in value["events"] if event["kind"] == "shutdown_quiesced")
                        if mode == "pid": ack["pid"] += 100
                        elif mode == "prepare": ack["prepare_ack_sequence"] += 1
                        elif mode == "callback_admission": ack["pubsub_callback_admission_closed"] = False
                        elif mode == "worker": ack["active_fixture_workers"] = 1
                        elif mode == "sticky": value["error"] = "original native TCP error"
                        elif mode == "duplicate": value["events"].append({**ack, "sequence": len(value["events"]) + 1, "mono_ns": len(value["events"]) + 1})
                        elif mode == "exit": owner.process.poll = lambda: 0
                        elif mode == "changed_earlier_ack":
                            old = documents["offender"]
                            next(event for event in old["events"] if event["kind"] == "shutdown_quiesced")["pubsub_callback_admission_closed"] = False
                            actors["offender"][1]["result"].write_text(json.dumps(old))
                        path.write_text(json.dumps(value))
                    return value
                with patch("pubsub_cases._await", side_effect=wait), self.assertRaises((ValueError, RuntimeError, TimeoutError)):
                    _stop_prepared(actors, barrier, deadline=10)
                self.assertEqual(stops, [])
                self.assertFalse(any(row["kind"] in {"stop_requested", "donor_joined"} for row in barrier["operations"]))
                self.assertEqual([row["actor"] for row in barrier["operations"] if row["kind"] == "quiesce_requested"],
                                 ["offender", "replacement", "sink"])

    def test_changed_or_missing_prepare_capture_never_releases_stop(self):
        for missing in (False, True):
            with self.subTest(missing=missing), tempfile.TemporaryDirectory() as root:
                actors, documents, stops = self.shutdown_actors(root)
                with patch("pubsub_cases._await", side_effect=lambda owner, path, predicate, deadline: documents[owner.ready["actor"]]):
                    barrier = _prepare_all(actors, {role: 1 for role in actors}, "a" * 32, 10)
                path = Path(barrier["operations"][0]["evidence_file"])
                if missing:
                    path.unlink()
                else:
                    snapshot = json.loads(path.read_text())
                    snapshot["result"]["events"][0]["admission_closed"] = False
                    path.write_text(json.dumps(snapshot))
                with self.assertRaises((ValueError, OSError)):
                    _stop_prepared(actors, barrier)
                self.assertEqual(stops, [])

    def test_missing_foreign_or_failed_prepare_never_releases_successful_stop(self):
        for failure in ("missing", "foreign", "active_error", "teardown_error", "missing_actor"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as root:
                actors, documents, stops = self.shutdown_actors(root)
                sequence = {role: 1 for role in actors}
                raw = documents["sink"]
                if failure == "missing":
                    raw["events"] = []
                elif failure == "foreign":
                    raw["events"][0]["local_peer_id"] = actors["replacement"][0].ready["peer_id"]
                elif failure == "active_error":
                    raw["error"] = "failure before preparation"
                elif failure == "missing_actor":
                    del actors["sink"]
                def wait(owner, path, predicate, deadline):
                    value = documents[owner.ready["actor"]]
                    if not predicate(value):
                        raise TimeoutError("missing prepare acknowledgement")
                    return value
                with patch("pubsub_cases._await", side_effect=wait):
                    if failure != "teardown_error":
                        with self.assertRaises((ValueError, TimeoutError)):
                            _prepare_all(actors, sequence, "a" * 32, 10)
                    else:
                        barrier = _prepare_all(actors, sequence, "a" * 32, 10)
                        raw["error"] = "real native failure after preparation"
                        actors["sink"][1]["result"].write_text(json.dumps(raw))
                        with self.assertRaises(RuntimeError):
                            _stop_prepared(actors, barrier)
                self.assertEqual(stops, [])

    def test_missing_ack_times_out_in_actual_bounded_wait(self):
        with tempfile.TemporaryDirectory() as root:
            actors, documents, stops = self.shutdown_actors(root)
            owner, files = actors["victim"]
            documents["victim"]["events"] = []
            files["result"].write_text(json.dumps(documents["victim"]))
            self.assertIsNone(shutdown_ack(documents["victim"], "forge", "victim", "a" * 32, owner.ready["peer_id"], 2))
            with self.assertRaises(TimeoutError):
                _await(owner, files["result"], lambda raw: shutdown_ack(raw, "forge", "victim", "a" * 32,
                       owner.ready["peer_id"], 2), time.monotonic() - 1)
            self.assertEqual(stops, [])

    def readiness_documents(self, implementation="go", *, terminal=False):
        identity = {"schema_version": 1, "implementation": implementation, "actor": "victim",
                    "case_token": "a" * 32, "local_peer_id": "synthetic-unit-peer"}
        ready = {**identity, "peer_id": identity["local_peer_id"], "listen_addrs": ["/ip4/127.0.0.1/tcp/1234"],
                 "topic": "forge-pr11:" + identity["case_token"], "ready": True, "subscription_created": True}
        observation = {**identity, "finalized": terminal, "joined": terminal, "overflow": False, "error": None}
        return ready, observation

    def ready(self, value, observation, implementation="go", *, terminal=False):
        return _ready(value, observation, implementation, "victim", "a" * 32, terminal=terminal)

    def test_common_readiness_accepts_all_three_actors_without_status_labels(self):
        for implementation in ("forge", "go", "rust"):
            with self.subTest(implementation=implementation):
                value, observation = self.readiness_documents(implementation)
                self.assertNotIn("status", value)
                self.assertTrue(self.ready(value, observation, implementation))

    def test_active_and_terminal_readiness_are_disjoint_without_flag_rewrites(self):
        for implementation in ("forge", "go", "rust"):
            for terminal in (False, True):
                with self.subTest(implementation=implementation, terminal=terminal):
                    value, observation = self.readiness_documents(implementation, terminal=terminal)
                    before = deepcopy((value, observation))
                    self.assertTrue(self.ready(value, observation, implementation, terminal=terminal))
                    self.assertFalse(self.ready(value, observation, implementation, terminal=not terminal))
                    self.assertEqual((value, observation), before)
                    for field in ("finalized", "joined"):
                        for invalid in (None, 0, 1, "true", not terminal):
                            changed = deepcopy(observation)
                            changed[field] = invalid
                            self.assertFalse(self.ready(value, changed, implementation, terminal=terminal))
        value, observation = self.readiness_documents()
        for invalid in (None, 0, 1, "true"):
            self.assertFalse(self.ready(value, observation, terminal=invalid))

    def test_readiness_requires_actual_boolean_ready_and_subscription(self):
        for field in ("ready", "subscription_created"):
            value, observation = self.readiness_documents()
            del value[field]
            self.assertFalse(self.ready(value, observation))
            for invalid in (None, False, 0, 1, "true", "ready"):
                with self.subTest(field=field, invalid=invalid):
                    value, observation = self.readiness_documents()
                    value[field] = invalid
                    self.assertFalse(self.ready(value, observation))

    def test_readiness_requires_integer_schema_one_in_both_documents(self):
        for index in (0, 1):
            documents = self.readiness_documents()
            del documents[index]["schema_version"]
            self.assertFalse(self.ready(*documents))
            for invalid in (None, True, False, 0, 2, "1"):
                with self.subTest(document=index, invalid=invalid):
                    documents = self.readiness_documents()
                    documents[index]["schema_version"] = invalid
                    self.assertFalse(self.ready(*documents))

    def test_readiness_binds_implementation_actor_token_and_actual_identity(self):
        for index in (0, 1):
            for field, foreign in (("implementation", "rust"), ("actor", "sink"),
                                   ("case_token", "b" * 32), ("local_peer_id", "foreign-unit-peer")):
                with self.subTest(document=index, field=field):
                    documents = self.readiness_documents()
                    del documents[index][field]
                    self.assertFalse(self.ready(*documents))
                    documents = self.readiness_documents()
                    documents[index][field] = foreign
                    self.assertFalse(self.ready(*documents))
        for peer in (None, "", " ", 1, "foreign-unit-peer"):
            value, observation = self.readiness_documents()
            value["peer_id"] = peer
            self.assertFalse(self.ready(value, observation))
        value, observation = self.readiness_documents()
        del value["peer_id"]
        self.assertFalse(self.ready(value, observation))
        for peer in ("", " ", None, 1, "x" * 513):
            value, observation = self.readiness_documents()
            value["peer_id"] = value["local_peer_id"] = observation["local_peer_id"] = peer
            self.assertFalse(self.ready(value, observation))

    def test_readiness_requires_canonical_topic_and_actual_listener(self):
        for field, invalid in (("topic", "forge-pr11:" + "b" * 32), ("topic", None),
                               ("listen_addrs", None), ("listen_addrs", []), ("listen_addrs", [""]),
                               ("listen_addrs", [" "]), ("listen_addrs", [1]),
                               ("listen_addrs", ["one", "two"])):
            with self.subTest(field=field, invalid=invalid):
                value, observation = self.readiness_documents()
                value[field] = invalid
                self.assertFalse(self.ready(value, observation))

    def test_readiness_requires_active_matching_result_not_only_ready_file(self):
        value, observation = self.readiness_documents()
        self.assertFalse(self.ready(value, None))
        self.assertFalse(self.ready(None, observation))
        for field, invalid in (("finalized", True), ("joined", True), ("overflow", True), ("error", "failed")):
            with self.subTest(field=field):
                changed = deepcopy(observation)
                changed[field] = invalid
                self.assertFalse(self.ready(value, changed))
        for field in ("finalized", "joined", "overflow"):
            changed = deepcopy(observation)
            del changed[field]
            self.assertFalse(self.ready(value, changed))

    def test_legacy_labels_and_subscribed_alias_do_not_open_readiness_barrier(self):
        value, observation = self.readiness_documents()
        legacy = {"status": "ready", "peer_id": value["peer_id"], "listen_addrs": value["listen_addrs"]}
        self.assertFalse(self.ready(legacy, observation))
        del value["ready"]
        del value["subscription_created"]
        value["subscribed"] = True
        self.assertFalse(self.ready(value, observation))

    def test_all_24_bilateral_version_profile_cases_are_unique(self):
        cases = case_specs()
        self.assertEqual(len(cases), 24)
        self.assertEqual(len({case.identifier for case in cases}), 24)
        self.assertEqual(Counter(case.source + "_to_" + case.destination for case in cases),
                         {"forge_to_go": 6, "go_to_forge": 6, "forge_to_rust": 6, "rust_to_forge": 6})

    def test_contract_ids_match_declared_manifest_not_new_claims(self):
        manifest = json.loads(Path(__file__).with_name("p2p_donor_capabilities.json").read_text())
        scenarios = manifest["interop_acceptance_registry"]["capabilities"]["pubsub.gossipsub_v1_0_v1_1"]["scenarios"]
        expected = {(scenario["id"], scenario["runner_scenario_id"]) for scenario in scenarios}
        self.assertEqual({(case.scenario, case.runner_id) for case in case_specs()}, expected)
        # Executable registration is not a current runtime verdict.
        self.assertTrue(all(scenario["registration"] == "registered" for scenario in scenarios))
        self.assertTrue(manifest["interop_acceptance_registry"]["artifact_schema"]["registration_is_not_verdict"])


if __name__ == "__main__":
    unittest.main()
