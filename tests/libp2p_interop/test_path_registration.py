"""Canonical DCUtR registration checks, never a live interoperability verdict."""

import ast
import copy
import json
from pathlib import Path
import unittest
from unittest.mock import patch

from check_p2p_feature_inventory import (
    PATH_REGISTRATION_SCENARIOS, path_registration_source_errors, registered_path_pairs,
    registered_runner_acceptance_pairs,
)
from check_stage6_acceptance import ACCEPTANCE_SUITES, DIRECTIONS, required_scenarios
from path_acceptance import EVIDENCE_CONTRACT, OWNER_ID, SCENARIO_ID, validate_suite


class PathRegistrationTests(unittest.TestCase):
    def manifest(self):
        manifest = json.loads(Path(__file__).with_name("p2p_donor_capabilities.json").read_text())
        entry = manifest["interop_acceptance_registry"]["capabilities"][OWNER_ID]["scenarios"][0]
        entry["registration"] = "registered"
        return manifest, entry

    def test_focused_contract_has_exact_owner_transport_and_directions(self):
        manifest, _ = self.manifest()
        required, errors = required_scenarios(manifest, "path")
        self.assertEqual(errors, [])
        self.assertEqual(required, {(OWNER_ID, SCENARIO_ID): (
            DIRECTIONS, "passed", "native", ("quic",), "quic_stage6/dcutr", (), EVIDENCE_CONTRACT)})
        self.assertIn("path", ACCEPTANCE_SUITES)

    def test_planned_scenario_does_not_become_executable_evidence(self):
        manifest, entry = self.manifest()
        entry["registration"] = "planned"
        required, errors = required_scenarios(manifest, "path")
        self.assertFalse(required)
        self.assertTrue(errors)

    def test_missing_directions_wrong_profile_status_or_runner_cannot_promote(self):
        baseline, _ = self.manifest()
        for field, value in (("required_directions", ["forge_to_go"]),
                             ("profile", "private_network"),
                             ("transport_stack", ["tcp", "yamux"]),
                             ("runner_scenario_id", "quic_stage6/relay_echo"),
                             ("expected_status", "limited")):
            with self.subTest(field=field):
                manifest = copy.deepcopy(baseline)
                manifest["interop_acceptance_registry"]["capabilities"][OWNER_ID]["scenarios"][0][field] = value
                self.assertTrue(required_scenarios(manifest, "path")[1])

    def test_four_success_labels_cannot_replace_twelve_owned_live_cases(self):
        required, errors = required_scenarios(self.manifest()[0], "path")
        self.assertEqual(errors, [])
        labels = [{"status": "passed", "direction": direction} for direction in DIRECTIONS]
        self.assertTrue(validate_suite(labels, required, Path("/not-a-live-run"), {}, None))


class PathInventoryRegistrationTests(unittest.TestCase):
    source_dir = Path(__file__).resolve().parent
    root = source_dir.parent.parent

    def manifest(self):
        return json.loads((self.source_dir / "p2p_donor_capabilities.json").read_text())

    def entries(self):
        manifest = self.manifest()
        capabilities = {value["id"]: value for value in manifest["capabilities"]}
        for owner, entry in manifest["interop_acceptance_registry"]["capabilities"].items():
            for scenario in entry["scenarios"]:
                if scenario["id"] in PATH_REGISTRATION_SCENARIOS:
                    yield owner, capabilities[owner], scenario

    def test_three_exact_staged_registrations_use_real_runner_dispatch(self):
        expected = {(value[4], name) for name, value in PATH_REGISTRATION_SCENARIOS.items()}
        source = self.source_dir / "runner.py"
        self.assertEqual(len(expected), 3)
        self.assertEqual(registered_path_pairs(ast.parse(source.read_text())), expected)
        self.assertTrue(expected <= registered_runner_acceptance_pairs(source))
        for owner, capability, scenario in self.entries():
            self.assertEqual(path_registration_source_errors(self.root, owner, capability, scenario), [])
            self.assertEqual(scenario["registration"], "registered")
            self.assertEqual(capability["decision"], "stage_6")

    def test_source_contract_rejects_owner_profile_directions_and_dependencies_drift(self):
        for owner, capability, scenario in self.entries():
            for field, value in (("source_case_id", "unregistered-source"), ("profile", "legacy"),
                                 ("registration", "planned"), ("expected_status", "limited"),
                                 ("runner_scenario_id", "quic_topology/dcutr_relay_topology"),
                                 ("transport_stack", []), ("required_directions", ["forge_to_go"]),
                                 ("required_directions", ["forge_to_go"] * 4),
                                 ("required_directions", ["forge_to_go", {}])):
                with self.subTest(scenario=scenario["id"], field=field, value=value):
                    self.assertTrue(path_registration_source_errors(
                        self.root, owner, capability, {**scenario, field: value}))
            self.assertTrue(path_registration_source_errors(self.root, "protocol.ping", capability, scenario))
            self.assertTrue(path_registration_source_errors(
                self.root, owner, {**capability, "decision": "current"}, scenario))
            if scenario["profile"] == "private_network":
                self.assertTrue(path_registration_source_errors(
                    self.root, owner, capability, {**scenario, "requires_capabilities": []}))

    def test_dispatch_requires_actual_adapters_attempts_private_inputs_and_capture(self):
        source = (self.source_dir / "runner.py").read_text()
        for old, new in (("for artifact in run_path_suite(", "for artifact in unrelated_path_suite("),
                         ("for artifact in run_coordinated_suite(", "for artifact in unrelated_coordinated_suite("),
                         ("from path_cases import run_suite", "from unrelated_cases import run_suite"),
                         ("for artifact in run_path_suite(binaries, root, command_attempt=command_attempt)",
                          "for artifact in run_path_suite(binaries, root)"),
                         ("pnet_fingerprint=pnet_fingerprint, command_attempt=command_attempt",
                          "pnet_fingerprint=untrusted_input, command_attempt=command_attempt"),
                         ("artifacts.append(artifact)", "unowned_capture.append(artifact)")):
            with self.subTest(old=old):
                self.assertIn(old, source)
                with self.assertRaises(ValueError):
                    registered_path_pairs(ast.parse(source.replace(old, new)))
        maps_only = "CURRENT_ACCEPTANCE_SCENARIOS = {'quic_stage6/dcutr': ('dcutr',)}"
        self.assertEqual(registered_path_pairs(ast.parse(maps_only)), set())

    def test_source_requires_actual_native_validators_and_actor_files(self):
        read_text = Path.read_text
        for owner, capability, scenario in self.entries():
            suite = PATH_REGISTRATION_SCENARIOS[scenario["id"]][1]
            for module in ("cases", "acceptance"):
                target = self.source_dir / f"{suite}_{module}.py"
                def altered(path, *args, **kwargs):
                    text = read_text(path, *args, **kwargs)
                    return text.replace(f"from {suite}_evidence import", "from unrelated_evidence import") if path == target else text
                with self.subTest(scenario=scenario["id"], module=module), patch.object(Path, "read_text", altered):
                    self.assertTrue(path_registration_source_errors(self.root, owner, capability, scenario))
            with patch.object(Path, "is_file", return_value=False):
                self.assertTrue(path_registration_source_errors(self.root, owner, capability, scenario))

    def test_donor_bindings_match_canonical_selectors_without_changing_remaining_stages(self):
        cases = {value["id"]: value for value in json.loads((self.source_dir / "donor_cases.json").read_text())["cases"]}
        for _, _, scenario in self.entries():
            case = cases[scenario["source_case_id"]]
            selectors = {f"{value['profile']}/{value['scenario']}" for value in case["forge_live_scenario"]}
            self.assertEqual(case["mapping_state"], "mapped")
            self.assertIn(scenario["runner_scenario_id"], selectors)
            for prefix in ("donors/go-libp2p/", "donors/rust-libp2p/"):
                self.assertTrue(any(value.startswith(prefix) for value in case["donor_file"]))
        manifest = self.manifest()
        planned = [(owner, scenario) for owner, entry in manifest["interop_acceptance_registry"]["capabilities"].items()
                   for scenario in entry["scenarios"] if scenario["registration"] == "planned"]
        self.assertEqual(len(planned), 15)
        self.assertTrue(all(owner.startswith("pubsub.") for owner, _ in planned))
        self.assertEqual(len(required_scenarios(manifest, "stage6")[1]), 15)
        states = {value["id"]: value["state"] for value in json.loads((self.source_dir / "p2p_feature_inventory.json").read_text())["features"]}
        self.assertEqual(states["protocol.dcutr"], "partial")
        self.assertEqual(states["transport.tcp_yamux"], "unverified")


if __name__ == "__main__":
    unittest.main()
