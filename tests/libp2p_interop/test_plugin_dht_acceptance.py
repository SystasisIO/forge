"""Canonical DHT artifact rejection tests; synthetic files are not executable evidence."""

from contextlib import ExitStack, redirect_stderr, redirect_stdout
from copy import deepcopy
import io
import json
from pathlib import Path
from types import SimpleNamespace
import sys
import tempfile
import unittest
from unittest.mock import patch

import check_stage6_acceptance as provenance_checker
import plugin_node_acceptance as acceptance
from plugin_dht_evidence import validate_terminal_sources
from plugin_node_evidence import load_json
from provenance import fixture_donor_revision_bindings, load_canonical_donor_revisions
from test_plugin_dht_evidence import synthetic_record
from test_plugin_node_acceptance import build_provenance


class RegistrationTests(unittest.TestCase):
    def records(self, backends=("mdbx", "rocksdb")):
        return [{"scenario_id": spec.scenario_id, "status": "observed", "amino_evidence": {
            "kind": "causal-api-and-remote-io", "protocol": "/ipfs/kad/1.0.0", "negotiation_event_capture": False,
            "value_proof": "forge.dht-api.selected" if spec.forge_role == "client" else "donor.get_value.golden-comparison",
            "listener_snapshot_is_record_proof": False}}
            for spec in acceptance.registered_cases(list(backends), "plugin-dht").values()]

    def test_closed_sixteen_and_unchanged_node_defaults(self):
        self.assertEqual(len(acceptance.registered_cases(["mdbx", "rocksdb"], "plugin-dht")), 16)
        self.assertEqual(len(acceptance.registered_cases(["mdbx"], "plugin-dht")), 8)
        self.assertEqual(len(acceptance.registered_cases(["mdbx", "rocksdb"])), 84)
        for suite in ("stage6", "plugin-provider", "", None):
            with self.subTest(suite=suite), self.assertRaises(ValueError):
                acceptance.registered_cases(["mdbx", "rocksdb"], suite)

    def test_omission_duplicate_foreign_not_observed_and_bad_backend_fail(self):
        with patch.object(acceptance, "validate_dht_case", return_value=[]):
            baseline = self.records()
            self.assertEqual(acceptance.validate_matrix(baseline, ["mdbx", "rocksdb"], "plugin-dht"), [])
            variants = [baseline[:-1], baseline + [baseline[0]], baseline + [{"scenario_id": "plugin-node/echo"}],
                        baseline + [{"scenario_id": [], "status": "observed"}]]
            failed = deepcopy(baseline)
            failed[0]["status"] = "ok"
            variants.append(failed)
            for records in variants:
                self.assertTrue(acceptance.validate_matrix(records, ["mdbx", "rocksdb"], "plugin-dht"))
            for backends in ([], ["memory"], ["mdbx", "mdbx"], [True]):
                self.assertTrue(acceptance.validate_matrix(baseline, backends, "plugin-dht"))

    def test_constant_protocol_cannot_be_promoted_to_event_capture_or_record_persistence(self):
        with patch.object(acceptance, "validate_dht_case", return_value=[]):
            for field in ("negotiation_event_capture", "listener_snapshot_is_record_proof"):
                for bad in (True, 0, None):
                    record = self.records()
                    record[0]["amino_evidence"][field] = bad
                    self.assertTrue(acceptance.validate_matrix(record, ["mdbx", "rocksdb"], "plugin-dht"))
            missing = self.records()
            missing[0].pop("amino_evidence")
            self.assertTrue(acceptance.validate_matrix(missing, ["mdbx", "rocksdb"], "plugin-dht"))


class CanonicalArtifactTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.source = self.root / "tests/libp2p_interop"
        self.source.mkdir(parents=True)
        self.build = self.root / "build"
        self.build.mkdir()
        self.run_root = self.build / "plugin-dht-run"
        self.run_root.mkdir()
        self.artifact_path = self.build / "plugin-dht-artifacts.json"
        self.donors_root = self.root / "donors"
        self.donors_root.mkdir()
        self.identity = {"head": "a" * 40, "worktree_sha256": "b" * 64, "dirty": False,
                         "exact_identity": "git:" + "a" * 40 + ";worktree-sha256:" + "b" * 64}
        real_source = Path(__file__).parent
        for name in ("donor_cases.json", "fixture-lock.json"):
            (self.source / name).write_text((real_source / name).read_text())
        lock = load_json(self.source / "fixture-lock.json")
        revisions, errors = fixture_donor_revision_bindings(lock["donors"],
            load_canonical_donor_revisions(self.source / "donor_cases.json"))
        self.assertEqual(errors, [])
        recipe = build_provenance(self.build)
        paths = {implementation: Path(binary["path"]) for implementation, binary in recipe["binaries"].items()}
        paths["forge"] = self.build / "forge-plugin-fixture"
        for implementation, path in paths.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("synthetic non-executable " + implementation + "\n")
        recipe["binaries"] = {implementation: {"path": str(path), "sha256": provenance_checker.sha256_file(path)}
                              for implementation, path in paths.items()}
        self.paths = paths
        self.records = []
        for spec in acceptance.registered_cases(["mdbx", "rocksdb"], "plugin-dht").values():
            parent = self.run_root / spec.scenario_id.replace("/", "-")
            parent.mkdir()
            record = synthetic_record(spec, parent)
            for endpoint in record["endpoints"].values():
                process = endpoint["process"]
                # Fixture helper shares the command with the owned attempt.
                process["command"][0] = str(paths[endpoint["implementation"]])
                Path(process["log_file"]).write_text("synthetic owned stdout\n")
            record["status"] = "observed"
            record["amino_evidence"] = {"kind": "causal-api-and-remote-io", "protocol": "/ipfs/kad/1.0.0",
                "negotiation_event_capture": False,
                "value_proof": "forge.dht-api.selected" if spec.forge_role == "client" else "donor.get_value.golden-comparison",
                "listener_snapshot_is_record_proof": False}
            self.records.append(record)
        inputs = {"suite": "plugin-dht", "forge_root": str(self.root), "source_dir": str(self.source),
            "build_dir": str(self.build), "donors_root": str(self.donors_root), "acceptance_manifest": None,
            "plugin_store_backend": "all", "plugin_store_backends": ["mdbx", "rocksdb"]}
        recipe.update({"runner_inputs": inputs, "forge_worktree": {
            "start": self.identity, "end": self.identity, "changed_during_run": False},
            "fixture_build_info": {"schema_version": 2, "forge": self.identity, "fixture_kind": "configured-plugin",
                "store_backends": ["mdbx", "rocksdb"], "compiler": {"path": "/synthetic/clang", "id": "Clang", "version": "22"},
                "build_profile": "Release"}, "donors": lock["donors"], "donor_revisions": revisions})
        recipe["tools"]["python"] = {"path": str(Path(sys.executable).resolve()), "version_output": "synthetic Python version"}
        argv = [str(Path(sys.executable).resolve()), str(self.source / "runner.py"), "--enabled", "1", "--suite", "plugin-dht",
            "--forge-fixture", str(paths["forge"]), "--forge-root", str(self.root), "--source-dir", str(self.source),
            "--build-dir", str(self.build), "--donors-root", str(self.donors_root), "--plugin-store-backend", "all"]
        self.artifact = {"schema_version": 2, "fixture_provenance": recipe, "artifact_root": str(self.run_root),
            "acceptance_manifest": None, "artifacts": self.records, "failures": [], "runner_argv": argv,
            "started_at_unix": 2000, "finished_at_unix": 2001, "evidence_index": []}
        self.assertEqual(self.check(), [])

    def check(self, *, artifact=None, path=None, suite="plugin-dht", index=True, checkout_errors=None):
        artifact = artifact if artifact is not None else self.artifact
        path = path if path is not None else self.artifact_path
        if index:
            artifact["evidence_index"] = provenance_checker.build_evidence_index(self.run_root, artifact["artifacts"])
        path.write_text(json.dumps(artifact))
        with ExitStack() as stack:
            stack.enter_context(patch.object(acceptance, "validate_git_state", return_value=(1000, [])))
            stack.enter_context(patch.object(acceptance, "worktree_identity",
                return_value=SimpleNamespace(as_json=lambda: self.identity)))
            # Only external process/Git observations are mocked. Exact binary
            # SHA, donor pin comparisons, recipes, raw index and source checks run.
            stack.enter_context(patch.object(provenance_checker.subprocess, "check_output", return_value="synthetic Python version\n"))
            stack.enter_context(patch.object(provenance_checker, "fixture_donor_checkout_errors", return_value=checkout_errors or []))
            stack.enter_context(patch.object(acceptance.time, "time", return_value=3000))
            return acceptance.validate(self.root, path, self.identity["head"], suite)

    def test_synthetic_complete_canonical_dht_routes_through_existing_provenance_and_owned_sources(self):
        with patch.object(acceptance, "validate_execution_provenance", wraps=acceptance.validate_execution_provenance) as execution, \
                patch.object(acceptance, "validate_donor_provenance", wraps=acceptance.validate_donor_provenance) as donors, \
                patch.object(acceptance, "validate_dht_sources", wraps=validate_terminal_sources) as sources, \
                patch.object(acceptance, "validate_terminal_sources") as node_sources:
            self.assertEqual(self.check(), [])
        execution.assert_called_once()
        donors.assert_called_once()
        self.assertEqual(sources.call_count, 16)
        node_sources.assert_not_called()

    def test_noncanonical_suite_artifact_filename_root_and_runner_modes_fail(self):
        for mutate in (lambda a: a["fixture_provenance"]["runner_inputs"].update(suite="plugin-node"),
                       lambda a: a.update(artifact_root=str(self.build / "interop-run")),
                       lambda a: a.update(acceptance_manifest={"path": "/stage6/manifest"}),
                       lambda a: a["runner_argv"].extend(["--provenance-only", "1"]),
                       lambda a: a["runner_argv"].extend(["--suite", "plugin-dht"]),
                       lambda a: a["runner_argv"].__setitem__(a["runner_argv"].index("--suite") + 1, "plugin-node")):
            artifact = deepcopy(self.artifact)
            mutate(artifact)
            self.assertTrue(self.check(artifact=artifact))
        self.assertTrue(self.check(path=self.build / "plugin-node-artifacts.json"))
        self.assertTrue(self.check(suite="plugin-node"))

    def test_full_selection_cannot_silently_accept_eight_cases_or_one_backend(self):
        artifact = deepcopy(self.artifact)
        inputs = artifact["fixture_provenance"]["runner_inputs"]
        inputs["plugin_store_backends"] = ["mdbx"]
        artifact["fixture_provenance"]["fixture_build_info"]["store_backends"] = ["mdbx"]
        artifact["artifacts"] = [record for record in artifact["artifacts"] if record["spec"]["backend"] == "mdbx"]
        errors = self.check(artifact=artifact)
        self.assertTrue(any("exactly 16" in error for error in errors))
        for records in (self.records[:-1], self.records + [self.records[0]]):
            artifact = deepcopy(self.artifact)
            artifact["artifacts"] = records
            self.assertTrue(self.check(artifact=artifact))

    def test_binary_shas_donor_commits_revisions_and_compilation_inputs_not_bypassed(self):
        for implementation in ("forge", "go", "rust"):
            artifact = deepcopy(self.artifact)
            artifact["fixture_provenance"]["binaries"][implementation]["sha256"] = "0" * 64
            self.assertTrue(any("SHA-256" in error for error in self.check(artifact=artifact)))
        for mutation in (lambda p: p["donors"][0].update(commit="0" * 40),
                         lambda p: p["donor_revisions"].update(**{"go-libp2p": "0" * 40}),
                         lambda p: p["commands"][-1]["environment"].update(CARGO_NET_OFFLINE="false"),
                         lambda p: p["commands"][-1]["command"].remove("--frozen")):
            artifact = deepcopy(self.artifact)
            mutation(artifact["fixture_provenance"])
            self.assertTrue(self.check(artifact=artifact))
        self.assertIn("synthetic wrong donor checkout", self.check(checkout_errors=["synthetic wrong donor checkout"]))

    def test_source_forgery_fails_even_with_recomputed_raw_index(self):
        self.assertEqual(self.check(), [])
        captured = Path(self.records[0]["endpoints"]["writer"]["process"]["outputs"][0]["log_file"])
        payload = load_json(captured)
        payload["final"] = False
        captured.write_text(json.dumps(payload))
        errors = self.check()
        self.assertTrue(any("terminal sources" in error for error in errors))
        self.assertFalse(any("evidence index hash or size differs" in error for error in errors))

    def test_rebinding_to_raw_binary_or_another_actor_and_lost_join_fail(self):
        for mutation in (lambda r: r["endpoints"]["reader"]["process"]["command"].__setitem__(0, "/raw-fixture"),
                         lambda r: r["owned_processes"].pop(),
                         lambda r: r["endpoints"]["reader"].update(process=r["endpoints"]["writer"]["process"]),
                         lambda r: r["endpoints"]["writer"]["process"]["terminal_status"].update(termination="killed")):
            artifact = deepcopy(self.artifact)
            mutation(artifact["artifacts"][0])
            self.assertTrue(self.check(artifact=artifact))

    def test_index_hash_failure_is_preserved_in_new_suite(self):
        self.artifact["evidence_index"] = provenance_checker.build_evidence_index(self.run_root, self.records)
        self.artifact["evidence_index"][0]["sha256"] = "0" * 64
        self.assertTrue(any("hash or size differs" in error for error in self.check(index=False)))


class CommandLineTests(unittest.TestCase):
    def invoke(self, suite=None):
        argv = ["plugin_node_acceptance.py", "--forge-root", "/forge", "--artifact", "/artifact.json", "--head", "a" * 40]
        if suite is not None:
            argv += ["--suite", suite]
        with patch.object(sys, "argv", argv), patch.object(acceptance, "validate", return_value=[]) as validate, \
                redirect_stdout(io.StringIO()) as output, redirect_stderr(io.StringIO()):
            result = acceptance.main()
        return result, validate.call_args.args, output.getvalue()

    def test_existing_cli_stays_node_without_new_required_flags(self):
        result, args, output = self.invoke()
        self.assertEqual(result, 0)
        self.assertEqual(args[-1], "plugin-node")
        self.assertIn("initial configured-plugin matrix observed", output)

    def test_explicit_dht_dispatch_and_no_stage7_promotion_claim(self):
        result, args, output = self.invoke("plugin-dht")
        self.assertEqual(result, 0)
        self.assertEqual(args[-1], "plugin-dht")
        self.assertIn("DHT value selection observed", output)
        self.assertIn("no Stage6 capability or whole Stage7 promotion", output)


if __name__ == "__main__":
    unittest.main()
