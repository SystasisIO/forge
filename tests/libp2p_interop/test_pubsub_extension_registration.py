"""Source-only PR12 registration checks, never native execution evidence."""

import ast
from copy import deepcopy
import json
from pathlib import Path
import unittest

from check_p2p_feature_inventory import pubsub_registration_source_errors, registered_pubsub_pairs
from pubsub_extension_cases import case_specs


class RegistrationTests(unittest.TestCase):
    def test_complete_runner_matrix(self):
        source = Path(__file__).with_name("runner.py").read_text()
        expected = {(spec.runner_id, spec.scenario) for spec in case_specs()}
        self.assertEqual(len(expected), 9)
        self.assertEqual(registered_pubsub_pairs(ast.parse(source), extensions=True), expected)
        for before, after in (
            ("for spec in pubsub_extension_specs():", "for spec in pubsub_extension_specs()[:1]:"),
            ("run_pubsub_extension_case(spec, binaries, root, key=pnet_key_file,",
             "run_pubsub_extension_case(spec, binaries, root, key=None,"),
            ("run_pubsub_extension_case(spec, binaries, root, key=pnet_key_file,",
             "run_pubsub_extension_case(spec, replacement_binaries, root, key=pnet_key_file,"),
            ("artifacts.append(artifact)", "artifacts.append({})"),
        ):
            with self.subTest(after=after):
                self.assertIn(before, source)
                with self.assertRaises(ValueError):
                    registered_pubsub_pairs(ast.parse(source.replace(before, after)), extensions=True)

    def test_exact_manifest_contracts(self):
        directory = Path(__file__).parent
        root = directory.parent.parent
        manifest = json.loads((directory / "p2p_donor_capabilities.json").read_text())
        capabilities = {value["id"]: value for value in manifest["capabilities"]}
        owners = {"pubsub.gossipsub_v1_2", "pubsub.gossipsub_v1_3", "pubsub.partial_messages"}
        seen = set()
        for owner, entry in manifest["interop_acceptance_registry"]["capabilities"].items():
            if owner not in owners:
                continue
            capability = capabilities[owner]
            for scenario in entry["scenarios"]:
                with self.subTest(scenario=scenario["id"]):
                    seen.add(scenario["id"])
                    self.assertEqual(pubsub_registration_source_errors(root, owner, capability, scenario), [])
                    for field, value in (
                        ("source_case_id", "gossipsub.native_score_repair"),
                        ("runner_scenario_id", "foreign/runner"),
                        ("required_directions", ["forge_to_go"]),
                        ("expected_status", "limited"),
                        ("registration", "planned"),
                        ("transport_stack", ["websocket"]),
                    ):
                        changed = deepcopy(scenario)
                        changed[field] = value
                        self.assertTrue(pubsub_registration_source_errors(root, owner, capability, changed))
                    changed = dict(capability, planned_branch="forge-p2p-gossipsub-scoring-v1")
                    self.assertTrue(pubsub_registration_source_errors(root, owner, changed, scenario))
                    self.assertTrue(pubsub_registration_source_errors(root, "pubsub.foreign", capability, scenario))
        self.assertEqual(seen, {spec.scenario for spec in case_specs()})


if __name__ == "__main__":
    unittest.main()
