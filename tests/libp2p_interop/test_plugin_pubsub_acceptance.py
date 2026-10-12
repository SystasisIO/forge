"""Synthetic canonical PubSub gate tests, not live or raw capability promotion."""

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
import runner
from plugin_node_evidence import load_json
from plugin_pubsub_cases import Case
from plugin_pubsub_evidence import validate_terminal_sources
from process_lifecycle import current_scope
from provenance import fixture_donor_revision_bindings, load_canonical_donor_revisions
from test_plugin_node_acceptance import build_provenance
from test_plugin_pubsub_evidence import synthetic_record


class RegistrationTests(unittest.TestCase):
    def test_closed_eight_cases_and_existing_suite_registrations(self):
        self.assertEqual(acceptance.SUITES, runner.CONFIGURED_PLUGIN_SUITES)
        registered = acceptance.registered_cases(["mdbx", "rocksdb"], "plugin-pubsub")
        self.assertEqual(len(registered), 8)
        self.assertEqual({(spec.backend, spec.donor, spec.policy) for spec in registered.values()}, {
            (backend, donor, policy) for backend in ("mdbx", "rocksdb") for donor in ("go", "rust")
            for policy in ("yaml-only", "typed-override")})
        self.assertTrue(all(spec.transport == "tcp" for spec in registered.values()))
        self.assertEqual(len(acceptance.registered_cases(["mdbx"], "plugin-pubsub")), 4)
        self.assertEqual(len(acceptance.registered_cases(["mdbx", "rocksdb"])), 84)
        self.assertEqual(len(acceptance.registered_cases(["mdbx", "rocksdb"], "plugin-dht")), 16)
        for suite in ("pubsub-scoring", "pubsub-extensions", "stage6", None):
            with self.subTest(suite=suite), self.assertRaises(ValueError):
                acceptance.registered_cases(["mdbx", "rocksdb"], suite)

    def test_matrix_requires_every_case_once_and_observed(self):
        baseline = [{"scenario_id": scenario_id, "status": "observed"}
                    for scenario_id in acceptance.registered_cases(["mdbx", "rocksdb"], "plugin-pubsub")]
        with patch.object(acceptance, "validate_pubsub_case", return_value=[]):
            self.assertEqual(acceptance.validate_matrix(baseline, ["mdbx", "rocksdb"], "plugin-pubsub"), [])
            not_observed = deepcopy(baseline)
            not_observed[0]["status"] = "passed"
            for records in (baseline[:-1], baseline + [baseline[0]], not_observed,
                            baseline + [{"scenario_id": "plugin-dht/foreign", "status": "observed"}],
                            baseline + [{"scenario_id": [], "status": "observed"}]):
                self.assertTrue(acceptance.validate_matrix(records, ["mdbx", "rocksdb"], "plugin-pubsub"))
            for backends in ([], ["memory"], ["mdbx", "mdbx"], [True]):
                self.assertTrue(acceptance.validate_matrix(baseline, backends, "plugin-pubsub"))


class CanonicalArtifactTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name).resolve()
        self.source = self.root / "tests/libp2p_interop"
        self.source.mkdir(parents=True)
        self.build = self.root / "build"
        self.build.mkdir()
        self.run_root = self.build / "plugin-pubsub-run"
        self.run_root.mkdir()
        self.artifact_path = self.build / "plugin-pubsub-artifacts.json"
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
        self.paths = {implementation: Path(binary["path"]) for implementation, binary in recipe["binaries"].items()}
        self.paths["forge"] = self.build / "forge-plugin-fixture"
        for implementation, path in self.paths.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("synthetic non-executable " + implementation + "\n")
        recipe["binaries"] = {implementation: {"path": str(path), "sha256": provenance_checker.sha256_file(path)}
                              for implementation, path in self.paths.items()}
        self.records = []
        for spec in acceptance.registered_cases(["mdbx", "rocksdb"], "plugin-pubsub").values():
            parent = self.run_root / spec.scenario_id.replace("/", "-")
            parent.mkdir()
            record = synthetic_record(spec, parent)
            for endpoint in record["endpoints"].values():
                process = endpoint["process"]
                # The helper shares the actual command with its owner/attempt.
                process["command"][0] = str(self.paths[endpoint["implementation"]])
                Path(process["log_file"]).write_text("synthetic owned stdout\n")
            record["status"] = "observed"
            self.records.append(record)
        inputs = {"suite": "plugin-pubsub", "forge_root": str(self.root), "source_dir": str(self.source),
            "build_dir": str(self.build), "donors_root": str(self.donors_root), "acceptance_manifest": None,
            "plugin_store_backend": "all", "plugin_store_backends": ["mdbx", "rocksdb"]}
        recipe.update({"runner_inputs": inputs, "forge_worktree": {
            "start": self.identity, "end": self.identity, "changed_during_run": False},
            "fixture_build_info": {"schema_version": 2, "forge": self.identity, "fixture_kind": "configured-plugin",
                "store_backends": ["mdbx", "rocksdb"], "compiler": {"path": "/synthetic/clang", "id": "Clang", "version": "22"},
                "build_profile": "Release"}, "donors": lock["donors"], "donor_revisions": revisions})
        recipe["tools"]["python"] = {"path": str(Path(sys.executable).resolve()), "version_output": "synthetic Python version"}
        argv = [str(Path(sys.executable).resolve()), str(self.source / "runner.py"), "--enabled", "1", "--suite", "plugin-pubsub",
            "--forge-fixture", str(self.paths["forge"]), "--forge-root", str(self.root), "--source-dir", str(self.source),
            "--build-dir", str(self.build), "--donors-root", str(self.donors_root), "--plugin-store-backend", "all"]
        self.artifact = {"schema_version": 2, "fixture_provenance": recipe, "artifact_root": str(self.run_root),
            "acceptance_manifest": None, "artifacts": self.records, "failures": [], "runner_argv": argv,
            "started_at_unix": 2000, "finished_at_unix": 2001, "evidence_index": []}
        self.assertEqual(self.check(), [])

    def check(self, *, artifact=None, path=None, suite="plugin-pubsub", index=True, checkout_errors=None):
        artifact = artifact if artifact is not None else self.artifact
        path = path if path is not None else self.artifact_path
        if index:
            artifact["evidence_index"] = provenance_checker.build_evidence_index(self.run_root, artifact["artifacts"])
        path.write_text(json.dumps(artifact))
        with ExitStack() as stack:
            stack.enter_context(patch.object(acceptance, "validate_git_state", return_value=(1000, [])))
            stack.enter_context(patch.object(acceptance, "worktree_identity",
                return_value=SimpleNamespace(as_json=lambda: self.identity)))
            # Mock external Git/process observations only. Binary SHA, donor
            # pins, recipes, raw index, semantic receipts and sources stay real.
            stack.enter_context(patch.object(provenance_checker.subprocess, "check_output", return_value="synthetic Python version\n"))
            stack.enter_context(patch.object(provenance_checker, "fixture_donor_checkout_errors", return_value=checkout_errors or []))
            stack.enter_context(patch.object(acceptance.time, "time", return_value=3000))
            return acceptance.validate(self.root, path, self.identity["head"], suite)

    def test_same_provenance_gate_dispatches_eight_actual_shaped_sources(self):
        with patch.object(acceptance, "validate_execution_provenance", wraps=acceptance.validate_execution_provenance) as execution, \
                patch.object(acceptance, "validate_donor_provenance", wraps=acceptance.validate_donor_provenance) as donors, \
                patch.object(acceptance, "validate_pubsub_sources", wraps=validate_terminal_sources) as sources, \
                patch.object(acceptance, "validate_terminal_sources") as node_sources, \
                patch.object(acceptance, "validate_dht_sources") as dht_sources:
            self.assertEqual(self.check(), [])
        execution.assert_called_once()
        donors.assert_called_once()
        self.assertEqual(sources.call_count, 8)
        node_sources.assert_not_called()
        dht_sources.assert_not_called()

    def test_full_gate_rejects_omissions_duplicates_and_one_backend(self):
        for records in (self.records[:-1], self.records + [self.records[0]]):
            self.assertEqual(self.check(), [])
            artifact = deepcopy(self.artifact)
            artifact["artifacts"] = records
            self.assertTrue(self.check(artifact=artifact))
        self.assertEqual(self.check(), [])
        artifact = deepcopy(self.artifact)
        artifact["fixture_provenance"]["runner_inputs"]["plugin_store_backends"] = ["mdbx"]
        artifact["fixture_provenance"]["fixture_build_info"]["store_backends"] = ["mdbx"]
        artifact["artifacts"] = [record for record in artifact["artifacts"] if record["spec"]["backend"] == "mdbx"]
        self.assertTrue(any("exactly 8" in error for error in self.check(artifact=artifact)))

    def test_noncanonical_paths_suite_and_stage6_modes_are_rejected(self):
        for mutate in (lambda a: a["fixture_provenance"]["runner_inputs"].update(suite="plugin-node"),
                       lambda a: a.update(artifact_root=str(self.build / "interop-run")),
                       lambda a: a.update(acceptance_manifest={"path": "/stage6/manifest"}),
                       lambda a: a["runner_argv"].extend(["--provenance-only", "1"]),
                       lambda a: a["runner_argv"].extend(["--suite", "plugin-pubsub"]),
                       lambda a: a["runner_argv"].__setitem__(a["runner_argv"].index("--suite") + 1, "pubsub-scoring")):
            self.assertEqual(self.check(), [])
            artifact = deepcopy(self.artifact)
            mutate(artifact)
            self.assertTrue(self.check(artifact=artifact))
        self.assertTrue(self.check(path=self.build / "plugin-dht-artifacts.json"))
        self.assertTrue(self.check(suite="plugin-node"))

    def test_source_forgery_fails_even_after_rebuilding_the_index(self):
        self.assertEqual(self.check(), [])
        captured = Path(self.records[0]["endpoints"]["victim"]["process"]["outputs"][0]["log_file"])
        raw = load_json(captured)
        raw["final"] = False
        captured.write_text(json.dumps(raw))
        errors = self.check()
        self.assertTrue(any("terminal sources" in error for error in errors))
        self.assertFalse(any("evidence index hash or size differs" in error for error in errors))

    def test_binary_hashes_donor_pins_and_recipe_cannot_be_bypassed(self):
        for implementation in ("forge", "go", "rust"):
            self.assertEqual(self.check(), [])
            artifact = deepcopy(self.artifact)
            artifact["fixture_provenance"]["binaries"][implementation]["sha256"] = "0" * 64
            self.assertTrue(any("SHA-256" in error for error in self.check(artifact=artifact)))
        for mutate in (lambda p: p["donors"][0].update(commit="0" * 40),
                       lambda p: p["donor_revisions"].update(**{"go-libp2p": "0" * 40}),
                       lambda p: p["commands"][-1]["command"].remove("--frozen"),
                       lambda p: p["commands"][-1]["environment"].update(CARGO_NET_OFFLINE="false")):
            self.assertEqual(self.check(), [])
            artifact = deepcopy(self.artifact)
            mutate(artifact["fixture_provenance"])
            self.assertTrue(self.check(artifact=artifact))
        self.assertIn("synthetic wrong donor checkout", self.check(checkout_errors=["synthetic wrong donor checkout"]))

    def test_failure_join_status_and_evidence_hash_block_the_gate(self):
        for mutate in (lambda a: a.update(failures=["owned cleanup failed"]),
                       lambda a: a.pop("failures"),
                       lambda a: a["artifacts"][0].update(status="passed"),
                       lambda a: a["artifacts"][0]["owned_processes"].pop(),
                       lambda a: a["artifacts"][0]["endpoints"]["victim"]["process"]["terminal_status"].update(termination="killed")):
            self.assertEqual(self.check(), [])
            artifact = deepcopy(self.artifact)
            mutate(artifact)
            self.assertTrue(self.check(artifact=artifact))
        self.assertEqual(self.check(), [])
        artifact = deepcopy(self.artifact)
        artifact["evidence_index"][0]["sha256"] = "0" * 64
        self.assertTrue(any("hash or size differs" in error for error in self.check(artifact=artifact, index=False)))


class RunnerBoundaryTests(unittest.TestCase):
    def test_sources_run_after_owned_join_and_capture_not_inside_scope(self):
        record = {"owned_processes": [], "attempts": []}
        order, captured = [], {}

        def close():
            self.assertIsNotNone(current_scope())
            order.append("owned-join-and-capture")
            captured["joined_capture"] = True
            return []

        def launch(*args, **kwargs):
            self.assertIsNotNone(current_scope())
            order.append("launch")
            current_scope().processes.append(SimpleNamespace(close=close, evidence=lambda: dict(captured)))
            return record

        def sources(raw, *args):
            self.assertIsNone(current_scope())
            self.assertNotIn("status", raw)
            self.assertEqual(raw["owned_processes"], [{"joined_capture": True}])
            order.append("sources-after-teardown")
            return []

        with patch.object(runner, "plugin_pubsub_case", side_effect=launch), \
                patch.object(runner, "validate_plugin_pubsub_case", return_value=[]), \
                patch.object(runner, "validate_plugin_pubsub_sources", side_effect=sources):
            observed = runner.run_plugin_pubsub_case(Case("mdbx", "go", "yaml-only"), {}, Path("/case"))
        self.assertEqual(order, ["launch", "owned-join-and-capture", "sources-after-teardown"])
        self.assertEqual(observed["status"], "observed")

    def test_source_or_join_failure_never_marks_case_observed(self):
        spec = Case("mdbx", "rust", "typed-override")
        baseline = {"owned_processes": [], "attempts": []}
        with patch.object(runner, "run_owned_plugin_pubsub_case", return_value=deepcopy(baseline)), \
                patch.object(runner, "validate_plugin_pubsub_case", return_value=[]), \
                patch.object(runner, "validate_plugin_pubsub_sources", return_value=[]):
            self.assertEqual(runner.run_plugin_pubsub_case(spec, {}, Path("/case"))["status"], "observed")
        record = deepcopy(baseline)
        with patch.object(runner, "run_owned_plugin_pubsub_case", return_value=record), \
                patch.object(runner, "validate_plugin_pubsub_case", return_value=[]), \
                patch.object(runner, "validate_plugin_pubsub_sources", return_value=["rebound source"]):
            with self.assertRaises(runner.CaseFailure) as failed:
                runner.run_plugin_pubsub_case(spec, {}, Path("/case"))
        self.assertIs(failed.exception.artifact["uncommitted_result"], record)
        self.assertNotIn("status", record)

        def launch(*args, **kwargs):
            current_scope().processes.append(SimpleNamespace(close=lambda: ["join failed"], evidence=lambda: {}))
            return deepcopy(baseline)

        with patch.object(runner, "plugin_pubsub_case", side_effect=launch), \
                patch.object(runner, "validate_plugin_pubsub_case") as semantic, \
                patch.object(runner, "validate_plugin_pubsub_sources") as sources:
            with self.assertRaises(runner.CaseFailure) as failed:
                runner.run_plugin_pubsub_case(spec, {}, Path("/case"))
        self.assertEqual(failed.exception.cleanup_errors, ["join failed"])
        semantic.assert_not_called()
        sources.assert_not_called()

    def test_nested_scope_cannot_bypass_post_teardown_sources(self):
        with patch.object(runner, "current_scope", return_value=object()), \
                patch.object(runner, "run_owned_plugin_pubsub_case") as launch:
            with self.assertRaises(RuntimeError):
                runner.run_plugin_pubsub_case(Case("mdbx", "go", "yaml-only"), {}, Path("/case"))
        launch.assert_not_called()


class CommandLineTests(unittest.TestCase):
    def test_pubsub_cli_observation_is_not_raw_extension_scoring_or_stage7_promotion(self):
        argv = ["plugin_node_acceptance.py", "--forge-root", "/forge", "--artifact", "/artifact.json",
                "--head", "a" * 40, "--suite", "plugin-pubsub"]
        with patch.object(sys, "argv", argv), patch.object(acceptance, "validate", return_value=[]) as check, \
                redirect_stdout(io.StringIO()) as output, redirect_stderr(io.StringIO()):
            result = acceptance.main()
        self.assertEqual(result, 0)
        self.assertEqual(check.call_args.args[-1], "plugin-pubsub")
        self.assertIn("configured-plugin PubSub selection observed", output.getvalue())
        self.assertIn("no Stage6 capability or whole Stage7 promotion", output.getvalue())
        self.assertIn("no raw extension/scoring promotion", output.getvalue())
        with patch.object(sys, "argv", argv), patch.object(acceptance, "validate", return_value=["omitted case"]), \
                redirect_stdout(io.StringIO()) as output, redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(acceptance.main(), 1)
        self.assertEqual(output.getvalue(), "")
        self.assertIn("PLUGIN-PUBSUB EVIDENCE ERROR: omitted case", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
