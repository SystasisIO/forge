from pathlib import Path
import json
import tempfile
import unittest

from autorelay_cases import case_specs
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
