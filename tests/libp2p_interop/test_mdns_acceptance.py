"""Synthetic adversarial acceptance tests, never live multicast claims."""

import copy
from contextlib import ExitStack
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace

import mdns_acceptance as acceptance
import mdns_cases as cases
import process_lifecycle
import runner
import check_stage6_acceptance as checker
from check_stage6_acceptance import required_scenarios
from mdns_network import IsolatedMdnsNetwork
from test_mdns_cases import Harness
from test_mdns_network import MdnsCommands


class AcceptanceTests(unittest.TestCase):
    def test_full_runner_composes_41_plus_38_without_generic_dispatch(self):
        expected = acceptance.expected_cases()
        mdns = [{"scenario_id": name, "suite": "mdns"} for name in expected]
        autonat = [{"scenario_id": f"autonat-{i}", "suite": "autonat"} for i in range(41)]
        for suite, count in (("stage6", 79), ("autonat", 41), ("mdns", 38)):
            with ExitStack() as stack:
                mocks = {}
                for name, records in (("run_autonat_suite", autonat), ("run_mdns_suite", mdns[:28]),
                                      ("run_mdns_isolation_suite", mdns[28:36]), ("run_mdns_churn_suite", mdns[36:])):
                    mocks[name] = stack.enter_context(patch.object(runner, name, return_value=copy.deepcopy(records)))
                generic = stack.enter_context(patch.object(runner, "run_pair"))
                records = list(runner.run_paired_suites(suite, {}, Path("/unit"), pnet_key=Path("/key"),
                                                       mismatch_key=Path("/other-key"), pnet_fingerprint="diagnostic"))
                self.assertEqual(len(records), count)
                generic.assert_not_called()
                self.assertEqual(mocks["run_autonat_suite"].call_count, int(suite != "mdns"))
                for name in ("run_mdns_suite", "run_mdns_isolation_suite", "run_mdns_churn_suite"):
                    self.assertEqual(mocks[name].call_count, int(suite != "autonat"))
                for record in records:
                    if record["suite"] == "mdns":
                        self.assertEqual(record["acceptance_scenario_ids"], acceptance.claims_for(record))

    def test_checker_partitions_full_suite_and_preserves_partial_legacy(self):
        # These mocks isolate dispatch only. Actual raw validation and missing/
        # duplicate matrix rejection are exercised by the other tests here.
        mdns_required = {(capability, name): (set(), "passed", "native", ("tcp", "yamux"), scenario, (), "contract")
                         for name, (capability, scenario) in acceptance.SCENARIOS.items()}
        auto_name = next(iter(checker.AUTONAT_SCENARIOS))
        auto_required = {("protocol.autonat", auto_name): (set(), "passed", "native", ("quic",), "scenario", (), "contract")}
        mdns_records = [{"suite": "mdns", "scenario_id": name} for name in acceptance.expected_cases()]
        auto_records = [{"suite": "autonat", "scenario_id": str(i)} for i in range(41)]
        cases = (
            ("stage6", {**auto_required, **mdns_required}, auto_records + mdns_records, True, True, True),
            ("stage6", {**auto_required, **mdns_required}, auto_records + mdns_records, True, True, False),
            ("stage6", mdns_required, [{"suite": "legacy"}], False, True, False),
            ("stage6", {}, [{"suite": "legacy"}], False, False, False),
            ("mdns", mdns_required, mdns_records, False, True, True),
            ("autonat", auto_required, auto_records, True, False, True),
        )
        for suite, required, records, want_auto, want_mdns, receipt in cases:
            with self.subTest(suite=suite, required=tuple(required)), tempfile.TemporaryDirectory() as directory, ExitStack() as stack:
                root = Path(directory)
                manifest_path, artifact_path = root / "manifest.json", root / "artifact.json"
                manifest_path.write_text("{}")
                artifact = dict.fromkeys(checker.ARTIFACT_SCHEMA["required_fields"])
                artifact.update(schema_version=checker.ARTIFACT_SCHEMA["schema_version"], artifact_root=str(root),
                    fixture_provenance={"runner_inputs": {"suite": suite}}, started_at_unix=1, finished_at_unix=2,
                    acceptance_manifest={"path": str(manifest_path), "sha256": checker.sha256_file(manifest_path)},
                    failures=[], artifacts=records)
                artifact_path.write_text(json.dumps(artifact))
                returns = {"validate_git_state": (0, []), "required_scenarios": (required, []),
                    "validate_execution_provenance": ({}, []), "validate_runner_inputs": ({}, []),
                    "validate_donor_provenance": [], "validate_runner_argv": [], "validate_evidence_index": ({}, []),
                    "validate_all_result_evidence": [], "validate_execution_receipt": [],
                    "worktree_identity": SimpleNamespace(as_json=lambda: {})}
                for name, value in returns.items():
                    stack.enter_context(patch.object(checker, name, return_value=value))
                auto = stack.enter_context(patch.object(checker, "validate_autonat_suite", return_value=[]))
                missing_mdns = want_mdns and not any(r.get("suite") == "mdns" for r in records)
                mdns = stack.enter_context(patch.object(checker, "validate_mdns_suite",
                    return_value=["mDNS requires exactly 38 unique canonical cases"] if missing_mdns else []))
                generic = stack.enter_context(patch.object(checker, "validate_successful_raw_record", return_value=[]))
                errors, _ = checker.validate(root, manifest_path, artifact_path, "a" * 40,
                                             execution_receipt={} if receipt else None)
                if missing_mdns:
                    self.assertIn("mDNS requires exactly 38 unique canonical cases", errors)
                if suite == "stage6" and want_auto and want_mdns and not receipt:
                    # Standalone validates consistency, not promotion. Both raw
                    # suite validators still run; no receipt is fabricated.
                    self.assertEqual(errors, [])
                self.assertEqual(auto.call_count, int(want_auto))
                self.assertEqual(mdns.call_count, int(want_mdns))
                generic.assert_not_called()
                if want_mdns:
                    self.assertEqual(mdns.call_args.args[0], [r for r in records if r.get("suite") == "mdns"])
                    self.assertEqual(mdns.call_args.args[1], mdns_required)
                if want_auto:
                    self.assertEqual(auto.call_args.args[0], auto_records)
                    self.assertEqual(auto.call_args.args[1], auto_required)

    def test_exact_matrix_and_scoped_contracts(self):
        expected = acceptance.expected_cases()
        self.assertEqual(len(expected), 38)
        self.assertEqual(sum(kind == "positive" for kind, _ in expected.values()), 28)
        self.assertEqual(sum(kind == "isolation" for kind, _ in expected.values()), 8)
        self.assertEqual(sum(kind == "churn" for kind, _ in expected.values()), 2)
        manifest = json.loads(Path(__file__).with_name("p2p_donor_capabilities.json").read_text())
        required, errors = required_scenarios(manifest, "mdns")
        self.assertEqual(errors, [])
        self.assertEqual({name for _, name in required}, set(acceptance.SCENARIOS))
        records = [{"scenario_id": name} for name in expected]
        for invalid in (records[:-1], records + [records[0]], records[:-1] + [records[0]]):
            self.assertIn("38 unique", acceptance.validate_suite(invalid, required, Path("/unit"), {}, None, None)[0])

    def test_manifest_mdns_exact_contract_mutations_are_rejected(self):
        original = json.loads(Path(__file__).with_name("p2p_donor_capabilities.json").read_text())
        mutations = (
            ("discovery.mdns_public", {"transport_stack": ["quic"]}),
            ("discovery.mdns_public", {"runner_scenario_id": "quic_stage6/mdns_public"}),
            ("discovery.mdns_public", {"required_directions": ["forge_to_go", "go_to_forge"]}),
            ("discovery.mdns_public", {"requires_capabilities": ["security.private_network_psk"]}),
            ("discovery.mdns_public", {"profile": "private_network", "transport_stack": ["tcp", "pnet", "yamux"],
                                       "requires_capabilities": ["security.private_network_psk"]}),
            ("discovery.mdns_private_fingerprinted", {"requires_capabilities": []}),
            ("discovery.mdns_private_fingerprinted", {"runner_scenario_id": "tcp_stage6/mdns_private_fingerprinted_go"}),
        )
        for capability, changes in mutations:
            manifest = copy.deepcopy(original)
            manifest["interop_acceptance_registry"]["capabilities"][capability]["scenarios"][0].update(changes)
            with self.subTest(capability=capability, changes=changes):
                _, errors = required_scenarios(manifest, "mdns")
                self.assertTrue(errors, "mutated mDNS manifest must not accept unchanged TCP evidence")

    def pair(self, root, transport="tcp"):
        spec = cases.Case("forge", "go", transport)
        harness, fake = Harness(spec), MdnsCommands()
        network = IsolatedMdnsNetwork(4,
            command_runner=lambda command: fake(["ip" if arg == "/unit/ip" else arg for arg in command]),
            system=lambda: "Linux", ip_lookup=lambda name: f"/unit/{name}",
            outer_namespace_isolated=lambda: True, namespace_token="acceptance")
        fake.network = network
        harness.network = network
        binaries = {name: Path("/unit") / name for name in ("forge", "go", "rust")}
        key = Path(__file__).parent / "fixtures/pnet/swarm.key"

        def popen(command, **kwargs):
            translated = list(command)
            translated[3] = next(role for role in ("client", "server") if network.namespaces[role] == command[3])
            return harness.popen(translated, **kwargs)

        with patch.object(cases, "IsolatedMdnsNetwork", return_value=network), \
                patch.object(process_lifecycle.subprocess, "Popen", side_effect=popen):
            record = cases.run_case(spec, binaries, root, pnet_key=key,
                                    wait_json=harness.wait_json, command_attempt=runner.command_attempt)
        self.assertEqual(record["status"], "passed", record)
        return record, spec, binaries, key

    def test_positive_replays_network_and_requires_indexed_receipts(self):
        for transport in ("tcp", "tcp-pnet"):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                record, spec, binaries, key = self.pair(root, transport)
                load = lambda path: json.loads(Path(path).read_text())
                acceptance.positive(record, spec, root, binaries, load, key)
                changed = copy.deepcopy(record)
                changed["network"]["commands"][0]["returncode"] = 1
                with self.assertRaises(ValueError):
                    acceptance.positive(changed, spec, root, binaries, load, key)
                with self.assertRaises(ValueError):
                    acceptance.positive(record, spec, root, binaries,
                                        lambda path: (_ for _ in ()).throw(ValueError("not indexed")), key)

    def test_command_pid_cleanup_and_raw_receipt_mutations_fail(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            record, spec, binaries, key = self.pair(root)
            load = lambda path: json.loads(Path(path).read_text())
            for mutation in ("remote", "pid", "forced", "embedded", "missing_output", "attempt_failure"):
                changed = copy.deepcopy(record)
                owner = changed["owned_processes"][0]
                if mutation == "remote":
                    owner["command"] += ["--peer-id", "injected"]
                elif mutation == "pid":
                    owner["pid"] = True
                elif mutation == "forced":
                    owner["terminal_status"]["termination"] = "terminated"
                elif mutation == "embedded":
                    changed["client"]["failure"] = {}
                elif mutation == "missing_output":
                    owner["outputs"].pop()
                else:
                    changed["attempts"][0]["timeout_class"] = "timeout"
                with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                    acceptance.positive(changed, spec, root, binaries, load, key)

    def test_registration_does_not_claim_isolation_churn_or_rust_private(self):
        for name, (kind, spec) in acceptance.expected_cases().items():
            claims = acceptance.claims_for({"scenario_id": name})
            if kind != "positive":
                self.assertEqual(claims, [])
            elif claims == ["mdns_private_fingerprinted_go"]:
                self.assertEqual({spec.client, spec.server}, {"forge", "go"})


if __name__ == "__main__":
    unittest.main()
