from pathlib import Path
import json
import tempfile
from types import SimpleNamespace
import unittest

from autorelay_cases import _capture, _listener_stop_budget, _read, case_specs
from autorelay_acceptance import SCENARIOS, claims_for
import runner


class AutoRelayCaseTests(unittest.TestCase):
    def test_matrix_is_bounded_bilateral_and_covers_all_supported_stacks(self):
        cases = case_specs()
        self.assertEqual(len(cases), 12)
        self.assertEqual(len({c.identifier for c in cases}), 12)
        for transport in ("quic", "tcp", "tcp-tls"):
            actual = {(c.source, c.relay, c.destination, c.kind) for c in cases if c.transport == transport}
            self.assertEqual(actual, {("go", "go", "forge", "lifecycle"), ("rust", "rust", "forge", "lifecycle"),
                                      ("go", "forge", "rust", "service"), ("rust", "forge", "go", "service")})

    def test_forge_fixture_has_no_manual_relay_or_reachability_bypass(self):
        source = Path(__file__).with_name("forge_autorelay_fixture.cpp").read_text()
        for forbidden in ("async_reserve_relay", "async_refresh_relay", "async_cancel_relay", "async_probe_reachability", "set_advertised_endpoints"):
            self.assertNotIn(forbidden, source)
        for required in ("async_start()", "async_connect(", "async_stop()", "post_stop", "snapshot.network.local_endpoints",
                         "snapshot.relay_reservations", "snapshot.autorelay"):
            self.assertIn(required, source)
        self.assertNotIn("peer.relay_reservations", source)

    def test_autorelay_factory_requires_real_authenticated_identity(self):
        source = Path(__file__).with_name("forge_interop_fixture.cpp").read_text()
        factory = source.split('args.at("command") == "autorelay-destination"', 1)[1].split(
            ".listen_endpoint = loopback_endpoint_for", 1)[0]
        self.assertIn("auto options = relay_node_options({}, generate_libp2p_identity());", factory)
        self.assertIn("options.relay_policy.service_enabled = service;", factory)
        self.assertNotIn("allow_insecure_test_mode", factory)
        self.assertNotIn("dht_profiles", factory)

    def test_relay_factory_authenticates_matching_identity_without_unused_dht(self):
        source = Path(__file__).with_name("forge_interop_fixture.cpp").read_text()
        base = source.split("forge::net::p2p::node::options node_options(", 1)[1].split(
            "forge::net::p2p::node::options relay_node_options(", 1)[0]
        for binding in (".certificate_pem = identity.certificate_pem",
                        ".private_key_pem = identity.private_key_pem",
                        ".explicit_peer_id = identity.peer", ".public_key = identity.public_key"):
            self.assertIn(binding, base)
        self.assertIn(".allow_insecure_test_mode = true", base)
        self.assertIn("out.dht_profiles.push_back(forge::net::p2p::amino_v1(", base)
        self.assertIn("peer_store::make_memory_persistence()", base)

        relay = source.split("forge::net::p2p::node::options relay_node_options(", 1)[1].split(
            "void configure_dns_server(", 1)[0]
        self.assertIn("auto out = node_options(store_path, identity);", relay)
        self.assertIn("out.allow_insecure_test_mode = false;", relay)
        self.assertIn("out.dht_profiles.clear();", relay)
        self.assertIn("return relay_node_options(store_path, local_identity());", relay)
        self.assertNotIn("allow_insecure_test_mode = true", relay)
        self.assertNotIn("dht_profiles.push_back", relay)
        self.assertNotIn("persistence", relay)

        local = source.split("const libp2p_identity& local_identity() {", 1)[1].split(
            "forge::net::p2p::node::options node_options(", 1)[0]
        self.assertIn("static const auto identity = generate_libp2p_identity();", local)

    def test_legacy_relay_roles_use_strict_factory_and_distinct_topology_identities(self):
        source = Path(__file__).with_name("forge_interop_fixture.cpp").read_text()
        for start, end in (("int destination_mode(", "std::string run_scenario("),
                           ("int dial_relay_mode(", "int topology_mode(")):
            role = source.split(start, 1)[1].split(end, 1)[0]
            with self.subTest(role=start):
                self.assertIn('node{runtime, relay_node_options(required(args, "store-dir"))}', role)
                self.assertNotIn('node{runtime, node_options(', role)
                self.assertNotIn("allow_insecure_test_mode", role)

        topology = source.split("int topology_mode(", 1)[1].split("int build_info_mode()", 1)[0]
        for role in ("relay", "source", "destination"):
            with self.subTest(role=role):
                self.assertIn(f"const auto {role}_identity = generate_libp2p_identity();", topology)
                self.assertIn(f'auto {role}_options = relay_node_options(root / "{role}-store", '
                              f"{role}_identity);", topology)
                self.assertNotIn(f"auto {role}_options = node_options(", topology)
        self.assertNotIn("allow_insecure_test_mode", topology)

    def test_legacy_destination_captures_direct_listener_before_relay_advertisement(self):
        source = Path(__file__).with_name("forge_interop_fixture.cpp").read_text()
        destination = source.split("int destination_mode(", 1)[1].split("std::string run_scenario(", 1)[0]
        listen = destination.index("value.async_listen(loopback_quic_endpoint())")
        direct = destination.index("const auto local = value.local_endpoint();")
        reserve = destination.index("value.async_reserve_relay(relay_peer)")
        ready = destination.index('write_file(required(args, "ready-file")')
        self.assertLess(listen, direct)
        self.assertLess(direct, reserve)
        self.assertLess(reserve, ready)
        self.assertEqual(destination.count("value.local_endpoint()"), 1)
        self.assertIn("endpoint_json(p2p_endpoint_for(*local, value.local_peer()))", destination)

    def test_listener_budget_is_derived_from_native_close_and_fixture_post_stop(self):
        root = Path(__file__).resolve().parents[2]
        native = (root / "libraries/net/yamux/include/forge/net/yamux/options.cppm").read_text()
        fixture = Path(__file__).with_name("forge_autorelay_fixture.cpp").read_text()
        self.assertIn("close_timeout{5'000}", native)
        self.assertIn("sleep_for(1200ms)", fixture)
        for command in ("autorelay-destination", "autorelay-service"):
            for transport in ("tcp", "tcp-tls"):
                budget = _listener_stop_budget("forge", command, transport)
                self.assertEqual((budget.native_close_seconds, budget.post_stop_seconds,
                                  budget.scheduler_allowance_seconds), (5.0, 1.2, 2.0))
                self.assertEqual(budget.seconds, 8.2)

    def test_listener_budget_does_not_change_donor_quic_or_legacy_fixtures(self):
        for implementation in ("forge", "go", "rust"):
            for command in ("autorelay-destination", "autorelay-service", "autorelay-relay",
                            "autorelay-observe", "listen", "destination", "dial-relay"):
                for transport in ("quic", "tcp", "tcp-tls", "tcp-pnet"):
                    selected = implementation == "forge" and command in (
                        "autorelay-destination", "autorelay-service") and transport in ("tcp", "tcp-tls")
                    if not selected:
                        with self.subTest(implementation=implementation, command=command, transport=transport):
                            self.assertIsNone(_listener_stop_budget(implementation, command, transport))

    def test_malformed_json_fails_once_with_exact_path_and_original_decode_error(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "destination.json"
            path.write_text('{"observations":[{"peer_id":"unfinished')
            with self.assertRaises(ValueError) as raised:
                _read(path)
            self.assertIn(str(path), str(raised.exception))
            self.assertIn(f"read_bytes={len(path.read_bytes())}", str(raised.exception))
            captured = path.with_name(path.name + ".invalid-read")
            self.assertEqual(captured.read_bytes(), path.read_bytes())
            self.assertIn(str(captured), str(raised.exception))
            self.assertIsInstance(raised.exception.__cause__, json.JSONDecodeError)

    def test_json_path_diagnostics_preserve_duplicate_key_and_object_guards(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "observer.json"
            for raw in ('{"complete":false,"complete":true}', '[]'):
                path.write_text(raw)
                with self.subTest(raw=raw), self.assertRaises(ValueError) as raised:
                    _read(path)
                self.assertIn(str(path), str(raised.exception))

    def test_terminal_capture_decode_failure_names_immutable_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "actor.log.result-file.json"
            path.write_text('{"events":[')
            owner = SimpleNamespace(log_file=Path(directory) / "actor.log", outputs=[{
                "argument": "--result-file", "exists": True, "log_file": str(path)}])
            with self.assertRaises(ValueError) as raised:
                _capture(owner, "--result-file")
            self.assertIn(str(path), str(raised.exception))
            self.assertIn("--result-file", str(raised.exception))
            self.assertIsInstance(raised.exception.__cause__, json.JSONDecodeError)

    def test_six_canonical_role_transport_registrations_bind_all_twelve_cases(self):
        self.assertEqual(len(runner.AUTORELAY_ACCEPTANCE_SCENARIOS), 6)
        expected = {f"{profile}/{name}": (name,) for name, (_, _, _, profile) in SCENARIOS.items()}
        self.assertEqual(runner.AUTORELAY_ACCEPTANCE_SCENARIOS, expected)
        for spec in case_specs():
            claim, = claims_for(spec)
            _, kind, transport, profile = SCENARIOS[claim]
            self.assertEqual((kind, transport), (spec.kind, spec.transport))
            self.assertIn(claim, runner.LIVE_SCENARIO_PROFILES[profile])

    def test_legacy_partial_manifest_does_not_silently_launch_autorelay(self):
        self.assertFalse(runner.manifest_registers_autorelay(None))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manifest.json"
            for name, registration, expected in (("ping", "registered", False),
                                                  ("autorelay_lifecycle", "planned", False),
                                                  ("autorelay_lifecycle", "registered", True)):
                path.write_text(json.dumps({"interop_acceptance_registry": {"capabilities": {
                    "relay": {"scenarios": [{"id": name, "registration": registration}]}}}}))
                with self.subTest(name=name, registration=registration):
                    self.assertEqual(runner.manifest_registers_autorelay(str(path)), expected)


if __name__ == "__main__":
    unittest.main()
