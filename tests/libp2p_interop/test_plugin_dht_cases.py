"""Synthetic matrix/ownership tests only; no subprocess or network acceptance."""

from pathlib import Path
import tempfile
from copy import deepcopy
import unittest
from unittest.mock import Mock, patch

from plugin_dht_cases import Case, _wait_server_session, case_specs, command, run_case
from process_lifecycle import enter_scope, exit_scope


class RegistrationTests(unittest.TestCase):
    def test_exact_sixteen_values_only(self):
        specs = [spec for backend in ("mdbx", "rocksdb") for spec in case_specs(backend)]
        self.assertEqual(len(specs), 16)
        self.assertEqual(len({spec.scenario_id for spec in specs}), 16)
        self.assertEqual({spec.scenario for spec in specs}, {"dht_pk_put_get", "dht_ipns_put_get"})
        self.assertEqual({spec.transport for spec in specs}, {"quic"})
        for spec in specs:
            self.assertEqual(spec.implementation("writer"), spec.implementation("reader"))
            self.assertNotEqual(spec.implementation("writer"), spec.implementation("server"))
            self.assertEqual(spec.implementation("server") == "forge", spec.forge_role == "server")

    def test_invalid_backend_scenario_donor_and_role_rejected(self):
        for backend in ("memory", "", "fallback"):
            with self.assertRaises(ValueError):
                case_specs(backend)
        for args in (("mdbx", "dht_provide_find_provider", "go", "client"),
                     ("mdbx", "dht_pk_put_get", "forge", "client"),
                     ("mdbx", "dht_pk_put_get", "go", "combined")):
            with self.assertRaises(ValueError):
                Case(*args)

    def test_commands_keep_donor_modes_and_three_private_stores(self):
        for backend in ("mdbx", "rocksdb"):
            for spec in case_specs(backend):
                stores = []
                for actor in ("server", "writer", "reader"):
                    argv = command(spec, actor, Path("/fixture"), Path("/case"),
                                   peer="server-peer", address="/ip4/127.0.0.1/udp/41002/quic-v1/p2p/server-peer")
                    options = dict(zip(argv[2::2], argv[3::2]))
                    self.assertEqual(options["--scenario"], spec.scenario)
                    self.assertEqual(options["--transport"], "quic")
                    self.assertEqual("--store-backend" in options, spec.implementation(actor) == "forge")
                    stores.append(options["--store-dir"])
                    self.assertNotIn("--pnet-key-file", options)
                    self.assertNotIn("--requested-count", options)
                    self.assertNotIn("--allow-insecure-test-mode", options)
                    self.assertEqual("--dht-completion-barrier" in options, spec.completion_barrier(actor))
                    if actor == "server":
                        self.assertEqual(argv[1], "listen")
                        self.assertNotIn("--payload", options)
                        self.assertNotIn("--addr", options)
                    else:
                        self.assertEqual(argv[1], "dial")
                        self.assertEqual(options["--peer-id"], "server-peer")
                        self.assertEqual(options["--payload"], "put_only" if actor == "writer" else "get_only")
                self.assertEqual(len(set(stores)), 3)

    def test_no_implicit_combined_or_targetless_operation(self):
        spec = case_specs("mdbx")[0]
        with self.assertRaises(ValueError):
            command(spec, "reader", Path("/fixture"), Path("/case"))
        with self.assertRaises(ValueError):
            spec.implementation("put_get")


class OwnershipTests(unittest.TestCase):
    def exercise(self, spec, *, failed_join=False, failed_put=False, failed_hold=False):
        scope, token = enter_scope()
        sequence = []
        owners = {}
        snapshots = {}

        def spawn(argv, log, stop, attempt, **kwargs):
            actor = log.stem
            if actor == "reader":
                self.assertTrue(owners["writer"].closed)
                self.assertEqual(owners["writer"].process.poll(), 0)
                self.assertNotIn(str(log.parent / "writer.store"), argv)
            sequence.append("spawn-" + actor)
            owner = Mock()
            owner.process.pid = 100 + len(owners)
            owner.process.poll.return_value = None
            owner.closed = False
            owner.ready = {}
            owner.request_stop.side_effect = lambda: sequence.append("stop-" + actor)
            def close():
                if not owner.closed:
                    sequence.append("join-" + actor)
                    owner.closed = True
                    owner.process.poll.return_value = 0
                return ["injected writer cleanup failure"] if actor == "writer" and failed_join else []
            owner.close.side_effect = close
            owner.evidence.side_effect = lambda: {"pid": owner.process.pid, "ready": owner.ready}
            owners[actor] = owner
            scope.processes.append(owner)
            return owner

        def attempt(argv, log, scenario, number, actor, timeout):
            result = {"kind": actor, "command": argv, "attempt_id": number, "timeout_seconds": timeout}
            scope.attempts.append(result)
            return result

        def load(path):
            if path.name == "server.ready.json":
                return {"peer_id": "server-peer", "status": "ready", "listen_addrs": [
                    "/ip4/127.0.0.1/udp/41002/quic-v1/p2p/server-peer"]}
            return {"status": "ok", "actor": path.stem}

        def operation(payload, actual_spec, actor, peer, address):
            self.assertTrue(owners[actor].closed)
            sequence.append("validate-" + actor)
            if actor == "writer" and failed_put:
                raise ValueError("injected failed remote PUT")
            return actor + "-peer"

        def witness(payload, actual_spec, actor, peer, address):
            self.assertFalse(owners[actor].closed)
            sequence.append("witness-" + actor)
            return actor + "-peer"

        def server_receipt(actual_spec, actor, actual_owners, work, payload, peer, address, *, deadline):
            self.assertIs(actual_owners[actor], owners[actor])
            self.assertFalse(owners[actor].closed)
            sequence.append("server-receipt-" + actor)
            if failed_hold:
                raise TimeoutError("injected absent server session")
            return {"session_id": 7 if actor == "writer" else 8, "remote_peer_id": actor + "-peer"}

        try:
            with tempfile.TemporaryDirectory() as directory, \
                    patch("plugin_dht_cases.spawn_owned", side_effect=spawn), \
                    patch("plugin_dht_cases.load_json", side_effect=load), \
                    patch("plugin_dht_cases._wait_result"), \
                    patch("plugin_dht_cases._wait_server_session", side_effect=server_receipt), \
                    patch("plugin_dht_evidence.validate_completion_witness", side_effect=witness), \
                    patch("plugin_dht_evidence.validate_operation", side_effect=operation):
                if failed_join or failed_put or failed_hold:
                    with self.assertRaises((RuntimeError, ValueError, TimeoutError)):
                        run_case(spec, {name: Path("/" + name) for name in ("forge", "go", "rust")},
                                 Path(directory), command_attempt=attempt)
                else:
                    snapshots = run_case(spec, {name: Path("/" + name) for name in ("forge", "go", "rust")},
                                         Path(directory), command_attempt=attempt)
        finally:
            scope.close()
            exit_scope(token)
        return sequence, snapshots, owners

    def test_writer_join_before_reader_spawn_both_directions(self):
        for role in ("client", "server"):
            sequence, record, owners = self.exercise(Case("mdbx", "dht_pk_put_get", "go", role))
            self.assertLess(sequence.index("join-writer"), sequence.index("spawn-reader"))
            self.assertLess(sequence.index("validate-writer"), sequence.index("spawn-reader"))
            self.assertLess(sequence.index("join-reader"), sequence.index("join-server"))
            self.assertEqual(len(record["owned_processes"]), 3)
            self.assertTrue(all(owner.closed for owner in owners.values()))
            if role == "server":
                for actor in ("writer", "reader"):
                    self.assertLess(sequence.index("witness-" + actor), sequence.index("server-receipt-" + actor))
                    self.assertLess(sequence.index("server-receipt-" + actor), sequence.index("stop-" + actor))
                self.assertEqual(set(record["server_session_receipts"]), {"writer", "reader"})

    def test_failed_publication_or_join_never_spawns_reader_and_all_owners_close(self):
        for failed_join, failed_put in ((True, False), (False, True)):
            sequence, _, owners = self.exercise(Case("rocksdb", "dht_ipns_put_get", "rust", "server"),
                                                failed_join=failed_join, failed_put=failed_put)
            self.assertNotIn("spawn-reader", sequence)
            self.assertEqual(set(owners), {"server", "writer"})
            self.assertTrue(all(owner.closed for owner in owners.values()))

    def test_failed_server_receipt_never_spawns_reader_or_retries(self):
        sequence, _, owners = self.exercise(Case("mdbx", "dht_pk_put_get", "go", "server"), failed_hold=True)
        self.assertNotIn("spawn-reader", sequence)
        self.assertEqual(sequence.count("spawn-writer"), 1)
        self.assertEqual(set(owners), {"server", "writer"})
        self.assertTrue(all(owner.closed for owner in owners.values()))


class ServerReceiptTests(unittest.TestCase):
    def setUp(self):
        from test_plugin_dht_evidence import synthetic_record
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.spec = Case("mdbx", "dht_pk_put_get", "go", "server")
        self.record = synthetic_record(self.spec, self.root)
        self.work = Path(self.record["endpoints"]["server"]["result_file"]).parent
        self.payload = deepcopy(self.record["endpoints"]["server"]["result"])
        self.payload.update(final=False, lifecycle={"plugins_joined": False, "scheduler_joined": False})
        self.payload["diagnostics"]["phase"] = "running"
        self.witness = self.record["endpoints"]["writer"]["process"]["ready"]
        self.witness["connection_id"] = "donor-native-opaque-id"
        self.owners = {actor: Mock() for actor in ("server", "writer")}
        for owner in self.owners.values():
            owner.process.poll.return_value = None

    def wait(self, payloads=None):
        payloads = [self.payload] if payloads is None else payloads
        with patch("plugin_dht_cases.load_json", side_effect=payloads), \
                patch("plugin_dht_cases.time.monotonic", side_effect=[0, 1, 2, 3, 4]), \
                patch("plugin_dht_cases.time.sleep"):
            return _wait_server_session(self.spec, "writer", self.owners, self.work, self.witness,
                                        self.record["peer_id"], self.record["listen_address"], deadline=3)

    def test_delayed_actual_receipt_requires_same_peer_socket_not_donor_connection_id(self):
        missing = deepcopy(self.payload)
        missing["service"]["sessions"] = []
        missing["diagnostics"]["handshakes_completed"] = 0
        actual = self.wait([missing, self.payload])
        self.assertEqual(actual, self.record["server_session_receipts"]["writer"])
        self.assertNotEqual(str(actual["session_id"]), self.witness["connection_id"])

    def test_missing_or_wrong_actor_receipt_times_out_without_stop(self):
        for sessions in ([], [self.payload["service"]["sessions"][1]]):
            with self.subTest(sessions=sessions):
                payload = deepcopy(self.payload)
                payload["service"]["sessions"] = sessions
                with self.assertRaises(TimeoutError):
                    self.wait([payload] * 3)
        self.owners["writer"].request_stop.assert_not_called()

    def test_server_failure_and_identity_transport_socket_mismatch_fail_closed(self):
        for field, bad in (("remote_address", "/ip4/127.0.0.1/udp/0/quic-v1"), ("authentication", "noise"),
                           ("local_peer_id", "foreign-server"), ("direction", "outbound"),
                           ("local_address", "/ip4/127.0.0.1/udp/1/quic-v1"), ("identify_state", "identifying")):
            with self.subTest(field=field):
                payload = deepcopy(self.payload)
                payload["service"]["sessions"][0][field] = bad
                with self.assertRaises(ValueError):
                    self.wait([payload])
        payload = deepcopy(self.payload)
        payload["status"] = "error"
        with self.assertRaises(ValueError):
            self.wait([payload])

    def test_either_owner_exit_rejects_even_an_already_present_receipt(self):
        for actor in ("server", "writer"):
            self.owners[actor].process.poll.return_value = 0
            with self.assertRaises(RuntimeError):
                self.wait()
            self.owners[actor].process.poll.return_value = None

    def test_empty_malformed_and_duplicate_json_fail_without_retry(self):
        from plugin_node_evidence import load_json
        path = self.work / "server.json"
        for content in ("", "{", "{}", '{"status":"ok","status":"error"}'):
            path.write_text(content)
            with patch("plugin_dht_cases.load_json", side_effect=load_json), \
                    patch("plugin_dht_cases.time.monotonic", return_value=0):
                with self.assertRaises((ValueError, RuntimeError)):
                    _wait_server_session(self.spec, "writer", self.owners, self.work, self.witness,
                                         self.record["peer_id"], self.record["listen_address"], deadline=3)


class RunnerBoundaryTests(unittest.TestCase):
    def test_terminal_source_validation_after_owned_teardown(self):
        import runner
        from process_lifecycle import current_scope
        spec = case_specs("mdbx")[0]
        record = {"owned_processes": [], "attempts": []}
        order = []
        def launch(*args, **kwargs):
            self.assertIsNotNone(current_scope())
            order.append("launch")
            return record
        def sources(*args):
            self.assertIsNone(current_scope())
            order.append("terminal-sources-after-teardown")
            return []
        with patch("runner.plugin_dht_case", side_effect=launch), \
                patch("runner.validate_plugin_dht_case", return_value=[]), \
                patch("runner.validate_plugin_dht_sources", side_effect=sources):
            observed = runner.run_plugin_dht_case(spec, {}, Path("/case"))
        self.assertEqual(order, ["launch", "terminal-sources-after-teardown"])
        self.assertEqual(observed["status"], "observed")
        self.assertFalse(observed["amino_evidence"]["negotiation_event_capture"])

    def test_source_failure_keeps_uncommitted_record_and_never_marks_observed(self):
        import runner
        record = {"owned_processes": [], "attempts": []}
        with patch("runner.run_owned_plugin_dht_case", return_value=record), \
                patch("runner.validate_plugin_dht_case", return_value=[]), \
                patch("runner.validate_plugin_dht_sources", return_value=["rebound source"]):
            with self.assertRaises(runner.CaseFailure) as failure:
                runner.run_plugin_dht_case(case_specs("mdbx")[0], {}, Path("/case"))
        self.assertEqual(failure.exception.artifact["uncommitted_result"], record)
        self.assertNotIn("status", record)


if __name__ == "__main__":
    unittest.main()
