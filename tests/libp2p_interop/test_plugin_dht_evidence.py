"""Synthetic positive/rejection receipts, never a live donor acceptance claim."""

from copy import deepcopy
import json
from pathlib import Path
import tempfile
import unittest

from plugin_dht_cases import CASE_SECONDS, Case, case_specs, command
from plugin_dht_evidence import AMINO, golden_record, validate_case, validate_terminal_sources

BINARY_PATHS = {implementation: Path("/fixture/" + implementation) for implementation in ("forge", "go", "rust")}


def synthetic_record(spec, root):
    work = root / "case"
    work.mkdir()
    identities = {actor: actor + "-peer" for actor in ("server", "writer", "reader")}
    addresses = {actor: f"/ip4/127.0.0.1/udp/{port}/quic-v1"
                 for actor, port in (("server", 41002), ("writer", 41001), ("reader", 41003))}
    peer = identities["server"]
    target = addresses["server"] + "/p2p/" + peer
    ready = {"implementation": spec.implementation("server"), "role": "listener", "status": "ready",
             "peer_id": peer, "listen_addrs": [target]}

    def session(actor, inbound=False):
        return {"source": "node.diagnostics-api.session", "session_id": 1 if actor == "writer" else 2,
                "local_peer_id": peer if inbound else identities[actor],
                "remote_peer_id": identities[actor] if inbound else peer,
                "local_address": addresses["server"] if inbound else addresses[actor],
                "remote_address": addresses[actor] if inbound else addresses["server"],
                "direction": "inbound" if inbound else "outbound", "path": "direct",
                "identify_state": "identified", "authentication": "quic_tls", "muxer": ""}

    def plugin(actor):
        document = {"plugins": {"db": {"store": {"stores": [{"name": "peers", "driver": spec.backend,
            "path": str(work / (actor + ".store")), "object": {"family": "objectdb", "write-policy": "single-writer"}}]}},
            "net": {"p2p": {"node": {"allow-insecure-test-mode": False, "peer-store": {"store": "peers"},
                "identity": {"certificate-secret": "identity/certificate", "private-key-secret": "identity/private-key"},
                "path": {"policy": "direct-only"}, "topology": {"mode": "static-only"},
                "relay": {"client-enabled": False, "server-enabled": False, "auto-discovery-enabled": False},
                "listen": ["/ip4/127.0.0.1/udp/0/quic-v1"], "tcp": {"security": "noise"},
                "dht": {"profiles": [{"kind": "amino-v1", "mode": "server"}]}}}}}}
        diagnostics = {"source": "node.diagnostics-api.snapshot", "phase": "before-stop", "profile": {
            "protocol": AMINO, "kind": "amino-v1", "mode": "server", "values": True,
            "routing_active": 1, "routing_candidates": 0, "persistence_failures": 0,
            "persistence_degraded": False, "durability_uncertain": False,
            "persistence_closing": False, "persistence_closed": False},
            "dht_queries": 2 if actor == "server" else 0, "dht_responses": 2 if actor == "server" else 0,
            "handshakes_completed": 2 if actor == "server" else 1, "sessions_opened": 2 if actor == "server" else 1,
            "active_sessions": 0, "protocol_streams_opened": 1, "protocol_streams_accepted": 2 if actor == "server" else 0}
        if actor == "server":
            service = {"source": "official-node.native-dht-service-and-diagnostics-api", "operation": "serve",
                       "profile": AMINO, "record_observation": "not-queried",
                       "sessions": [session("writer", True), session("reader", True)]}
        else:
            actual = session(actor)
            service = {"source": "official-node.dht-api", "operation": "put_only" if actor == "writer" else "get_only",
                "profile": AMINO, "expected_peer_id": peer, "dial_address": target, "session": actual,
                "connect": {"source": "node.api.connect", "session_id": actual["session_id"], "remote_peer_id": peer,
                            "muxer": "", "identify_state": 1}, "record": golden_record(spec.scenario),
                "quorum_reached": True, "record_matches_golden": True}
            service.update({"attempted": 1, "accepted": 1} if actor == "writer" else {"responses": 1, "valid_records": 1})
        return {"schema_version": 1, "implementation": "forge", "suite": "plugin-node",
            "fixture_kind": "configured-plugin", "role": "listener" if actor == "server" else "dialer",
            "scenario": spec.scenario, "status": "ok", "final": True, "errors": [], "current_phase": "complete",
            "local_peer_id": identities[actor], "configuration_accepted": True, "store_backend": spec.backend,
            "psk_fingerprint": "", "accepted_configuration": {"source": "official-shell.accepted-document",
                "secret_material": "redacted", "identity_source": "Secrets.get_bytes", "identity_key_type": "ed25519",
                "backend_fallback": False, "psk_source": "absent", "document": document}, "exchange": None,
            "lifecycle": {"plugins_joined": True, "scheduler_joined": True}, "service": service, "diagnostics": diagnostics}

    endpoints, attempts = {}, []
    for index, actor in enumerate(("server", "writer", "reader")):
        implementation = spec.implementation(actor)
        if implementation == "forge":
            payload = plugin(actor)
        elif actor == "server":
            payload = {"implementation": implementation, "role": "listener", "status": "ok",
                       "scenario": spec.scenario, "record_persisted": True}
        else:
            payload = {"implementation": implementation, "role": "dialer", "status": "ok", "scenario": spec.scenario,
                "operation": "put_only" if actor == "writer" else "get_only", "remote_get": actor == "reader",
                "local_peer_id": identities[actor], "authenticated_remote_peer_id": peer,
                "connection_remote_addr": addresses["server"], "connection_id": str(index),
                "value_bytes": golden_record(spec.scenario)["value_bytes"]}
            if implementation == "go":
                payload["connection_witness_source"] = "go-libp2p.network.Conn"
        argv = command(spec, actor, BINARY_PATHS[implementation], work, peer=peer, address=target)
        process = {"pid": 100 + index, "command": argv, "log_file": str(work / (actor + ".log")),
            "ready": ready if actor == "server" else {}, "terminal_status": {"exit_code": 0, "termination": "graceful"},
            "outputs": []}
        if implementation == "forge":
            process["stop_budget"] = {"native_close_seconds": 10, "post_stop_seconds": 0,
                                      "scheduler_allowance_seconds": 10, "seconds": 20}
        entries = [("--result-file", work / (actor + ".json"), payload)]
        if actor == "server":
            entries.append(("--ready-file", work / "server.ready.json", ready))
        for flag, path, data in entries:
            captured = work / (actor + ".log." + flag[2:] + ".json")
            path.write_text(json.dumps(data))
            captured.write_text(json.dumps(data))
            process["outputs"].append({"argument": flag, "path": str(path), "exists": True, "log_file": str(captured)})
        endpoints[actor] = {"implementation": implementation, "process": process, "result_file": str(entries[0][1]),
                            "result": payload, "store_existed_before_spawn": False}
        attempts.append({"kind": actor, "scenario_id": spec.scenario_id, "attempt_id": 1, "command": argv,
            "requested_log_file": process["log_file"], "log_file": process["log_file"], "timeout_seconds": 20,
            "pid": process["pid"], "exit_code": 0, "terminal_status": process["terminal_status"], "outputs": process["outputs"]})
    events = ("server_started", "server_ready", "writer_started", "writer_joined", "reader_started", "reader_joined", "server_joined")
    return {"suite": "plugin-dht", "scenario_id": spec.scenario_id, "spec": vars(spec), "peer_id": peer,
        "listen_address": target, "endpoints": endpoints, "timeline_source": "runner.process_lifecycle",
        "timeline": [{"event": event, "pid": endpoints[event.split("_")[0]]["process"]["pid"], "monotonic_ns": index + 1}
                     for index, event in enumerate(events)], "operation_budget_seconds": CASE_SECONDS,
        "owned_processes": [endpoint["process"] for endpoint in endpoints.values()], "attempts": attempts}


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.spec = Case("mdbx", "dht_pk_put_get", "go", "client")
        self.record = synthetic_record(self.spec, self.root)

    def assertRejected(self, change, record=None, spec=None):
        broken = deepcopy(record or self.record)
        change(broken)
        self.assertTrue(validate_case(broken, spec or self.spec))

    def test_all_sixteen_synthetic_shapes_and_terminal_files(self):
        for backend in ("mdbx", "rocksdb"):
            for spec in case_specs(backend):
                with self.subTest(case=spec.scenario_id), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory).resolve()
                    record = synthetic_record(spec, root)
                    self.assertEqual(validate_case(record, spec), [])
                    self.assertEqual(validate_terminal_sources(record, BINARY_PATHS, root), [])

    def test_golden_is_identical_to_existing_cpp_fixture_bytes(self):
        source = (Path(__file__).parent / "dht_value_fixture.cpp").read_text()
        for scenario in ("dht_pk_put_get", "dht_ipns_put_get"):
            golden = golden_record(scenario)
            self.assertGreater(golden["value_bytes"], 0)
            self.assertIn(golden["key_hex"][len("2f706b2f") if scenario == "dht_pk_put_get" else len("2f69706e732f"):], source)
            # Concatenate literal payload lines, not a donor-reported hash.
            import re
            literals = "".join(re.findall(r'"([0-9a-f]+)"', source))
            self.assertIn(golden["value_hex"], literals)

    def test_no_local_get_or_combined_operation_substitution(self):
        for actor in ("writer", "reader"):
            self.assertRejected(lambda r, actor=actor: r["endpoints"][actor]["result"]["service"].update(operation="put_get"))
        for field in ("responses", "valid_records"):
            for bad in (0, True, -1, "1"):
                self.assertRejected(lambda r, field=field, bad=bad:
                    r["endpoints"]["reader"]["result"]["service"].update({field: bad}))
        for field in ("attempted", "accepted"):
            self.assertRejected(lambda r, field=field: r["endpoints"]["writer"]["result"]["service"].update({field: 0}))

    def test_actual_forge_key_bytes_hash_and_quorum_required(self):
        for actor in ("writer", "reader"):
            for field, bad in (("key_hex", "00"), ("value_hex", "00"), ("value_sha256", "0" * 64),
                               ("value_bytes", True), ("value_bytes", 1)):
                self.assertRejected(lambda r, actor=actor, field=field, bad=bad:
                    r["endpoints"][actor]["result"]["service"]["record"].update({field: bad}))
            for field in ("quorum_reached", "record_matches_golden"):
                self.assertRejected(lambda r, actor=actor, field=field:
                    r["endpoints"][actor]["result"]["service"].update({field: False}))

    def test_three_actual_distinct_peers_and_sockets_required(self):
        self.assertRejected(lambda r: r["endpoints"]["reader"]["result"].update(local_peer_id="writer-peer"))
        for field, bad in (("remote_peer_id", "writer-peer"), ("authentication", "test_insecure"),
                           ("identify_state", "identifying"), ("remote_address", "/ip4/127.0.0.1/udp/1/quic-v1"),
                           ("session_id", True), ("source", "configured-identity")):
            self.assertRejected(lambda r, field=field, bad=bad:
                r["endpoints"]["reader"]["result"]["service"]["session"].update({field: bad}))

    def test_stop_join_and_writer_before_reader_are_mandatory(self):
        for actor in ("writer", "server", "reader"):
            self.assertRejected(lambda r, actor=actor:
                r["endpoints"][actor]["process"]["terminal_status"].update(termination="terminated"))
        self.assertRejected(lambda r: r["endpoints"]["reader"].update(store_existed_before_spawn=True))
        self.assertRejected(lambda r: r["endpoints"]["writer"]["result"]["lifecycle"].update(scheduler_joined=False))
        self.assertRejected(lambda r: r["timeline"].__setitem__(3, r["timeline"][4]))
        self.assertRejected(lambda r: r["timeline"][5].update(monotonic_ns=(CASE_SECONDS + 1) * 10**9))

    def test_accepted_amino_backend_and_before_stop_snapshot_required(self):
        def node(r):
            return r["endpoints"]["reader"]["result"]["accepted_configuration"]["document"]["plugins"]["net"]["p2p"]["node"]
        self.assertRejected(lambda r: node(r)["dht"]["profiles"][0].update(mode="client"))
        self.assertRejected(lambda r: node(r).update(**{"allow-insecure-test-mode": True}))
        self.assertRejected(lambda r: r["endpoints"]["reader"]["result"].update(store_backend="memory"))
        self.assertRejected(lambda r: r["endpoints"]["reader"]["result"]["diagnostics"].update(phase="post-join"))
        self.assertRejected(lambda r: r["endpoints"]["reader"]["result"]["diagnostics"]["profile"].update(persistence_degraded=True))

    def test_donor_listener_persisted_flag_is_not_used_as_returned_value_proof(self):
        self.record["endpoints"]["server"]["result"].pop("record_persisted")
        self.assertEqual(validate_case(self.record, self.spec), [])

    def test_forge_server_requires_actual_sessions_not_record_or_donor_hash_invention(self):
        spec = Case("rocksdb", "dht_ipns_put_get", "rust", "server")
        with tempfile.TemporaryDirectory() as directory:
            record = synthetic_record(spec, Path(directory).resolve())
            self.assertEqual(validate_case(record, spec), [])
            for actor in ("writer", "reader"):
                for field in ("local_peer_id", "authenticated_remote_peer_id", "connection_remote_addr", "connection_id"):
                    self.assertRejected(lambda r, actor=actor, field=field: r["endpoints"][actor]["result"].pop(field), record, spec)
            self.assertRejected(lambda r: r["endpoints"]["reader"]["result"].update(remote_get=False), record, spec)
            self.assertRejected(lambda r: r["endpoints"]["server"]["result"]["service"].update(record_observation="get-cached"), record, spec)
            self.assertRejected(lambda r: r["endpoints"]["server"]["result"]["service"].update(sessions=[]), record, spec)
            # Constant negotiated_protocol is deliberately neither required nor
            # presented as a captured negotiation event.
            for actor in ("writer", "reader"):
                self.assertNotIn("value_sha256", record["endpoints"][actor]["result"])
                self.assertNotIn("negotiated_protocol", record["endpoints"][actor]["result"])

    def test_go_initial_connection_requires_native_source_and_id(self):
        spec = Case("mdbx", "dht_pk_put_get", "go", "server")
        with tempfile.TemporaryDirectory() as directory:
            record = synthetic_record(spec, Path(directory).resolve())
            self.assertEqual(validate_case(record, spec), [])
            for actor in ("writer", "reader"):
                for field in ("connection_id", "connection_witness_source"):
                    self.assertRejected(lambda r, actor=actor, field=field:
                        r["endpoints"][actor]["result"].pop(field), record, spec)
                    for bad in ("", None, True):
                        self.assertRejected(lambda r, actor=actor, field=field, bad=bad:
                            r["endpoints"][actor]["result"].update({field: bad}), record, spec)
                self.assertRejected(lambda r, actor=actor:
                    r["endpoints"][actor]["result"].update(connection_witness_source="requested-dial-argument"), record, spec)


class SourceTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.spec = Case("mdbx", "dht_pk_put_get", "go", "client")
        self.record = synthetic_record(self.spec, self.root)
        self.assertEqual(validate_terminal_sources(self.record, BINARY_PATHS, self.root), [])

    def assertSourceRejected(self, change):
        self.assertEqual(validate_terminal_sources(self.record, BINARY_PATHS, self.root), [])
        broken = deepcopy(self.record)
        change(broken)
        self.assertTrue(validate_terminal_sources(broken, BINARY_PATHS, self.root))

    def test_command_modes_binary_targets_and_store_binding_cannot_change(self):
        for actor in ("writer", "reader"):
            for flag, bad in (("--payload", "put_get"), ("--peer-id", "writer-peer"),
                              ("--addr", "/ip4/127.0.0.1/udp/1/quic-v1/p2p/writer-peer"),
                              ("--store-dir", str(self.root / "shared.store")), ("--store-backend", "memory")):
                def change(r, actor=actor, flag=flag, bad=bad):
                    argv = r["endpoints"][actor]["process"]["command"]
                    argv[argv.index(flag) + 1] = bad
                self.assertSourceRejected(change)
        self.assertSourceRejected(lambda r: r["endpoints"]["reader"]["process"]["command"].__setitem__(0, "/raw-node"))
        self.assertSourceRejected(lambda r: r["endpoints"]["reader"]["process"]["command"].extend(["--payload", "get_only"]))

    def test_exact_three_attempts_without_retry_or_owner_substitution(self):
        self.assertSourceRejected(lambda r: r["attempts"].append(deepcopy(r["attempts"][0])))
        self.assertSourceRejected(lambda r: r["attempts"][1].update(attempt_id=2))
        self.assertSourceRejected(lambda r: r["attempts"][1].update(timeout_seconds=float("inf")))
        self.assertSourceRejected(lambda r: r["owned_processes"].pop())
        self.assertSourceRejected(lambda r: r["endpoints"]["reader"].update(process=r["endpoints"]["writer"]["process"]))

    def test_sources_capture_actual_final_file_not_inline_or_mutable_witness(self):
        self.assertSourceRejected(lambda r: r["endpoints"]["reader"]["result"]["service"].update(quorum_reached=False))
        self.assertSourceRejected(lambda r: r["endpoints"]["reader"].update(result_file=r["endpoints"]["writer"]["result_file"]))
        self.assertSourceRejected(lambda r: r["endpoints"]["reader"]["process"].update(outputs=[]))
        self.assertSourceRejected(lambda r: r["endpoints"]["server"]["process"]["ready"].update(peer_id="writer-peer"))

    def test_duplicate_and_nonfinite_json_rejected_in_both_original_and_capture(self):
        endpoint = self.record["endpoints"]["reader"]
        original = Path(endpoint["result_file"])
        captured = Path(endpoint["process"]["outputs"][0]["log_file"])
        for path in (original, captured):
            valid = path.read_text()
            for invalid in ('{"status":"ok","status":"ok"}', '{"status":"ok","number":NaN}'):
                path.write_text(invalid)
                self.assertTrue(validate_terminal_sources(self.record, BINARY_PATHS, self.root))
            path.write_text(valid)


if __name__ == "__main__":
    unittest.main()
