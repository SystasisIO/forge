"""Synthetic configured AutoRelay gate tests; no processes or capability promotion."""

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
from plugin_relay_cases import case_specs
from plugin_relay_evidence import validate_terminal_sources
from process_lifecycle import current_scope, enter_scope, exit_scope
from provenance import fixture_donor_revision_bindings, load_canonical_donor_revisions
from test_plugin_node_acceptance import build_provenance
from test_plugin_relay_evidence import recapture, synthetic_record


class RegistrationTests(unittest.TestCase):
    def test_distinct_eight_case_suite_preserves_other_registrations(self):
        self.assertEqual(acceptance.SUITES, runner.CONFIGURED_PLUGIN_SUITES)
        registered = acceptance.registered_cases(["mdbx", "rocksdb"], "plugin-relay")
        self.assertEqual(len(registered), 8)
        expected = {("go", "go", "forge", "lifecycle"), ("rust", "rust", "forge", "lifecycle"),
                    ("go", "forge", "rust", "service"), ("rust", "forge", "go", "service")}
        self.assertEqual({(spec.backend, spec.source, spec.relay, spec.destination, spec.kind)
                          for spec in registered.values()}, {(backend, *roles) for backend in ("mdbx", "rocksdb") for roles in expected})
        self.assertTrue(all(spec.transport == "tcp" for spec in registered.values()))
        self.assertEqual(len(acceptance.registered_cases(["mdbx"], "plugin-relay")), 4)
        self.assertEqual(len(acceptance.registered_cases(["mdbx", "rocksdb"])), 84)
        self.assertEqual(len(acceptance.registered_cases(["mdbx", "rocksdb"], "plugin-dht")), 16)
        self.assertEqual(len(acceptance.registered_cases(["mdbx", "rocksdb"], "plugin-pubsub")), 8)
        for suite in ("autorelay", "stage6", None):
            with self.subTest(suite=suite), self.assertRaises(ValueError):
                acceptance.registered_cases(["mdbx", "rocksdb"], suite)

    def test_matrix_rejects_omissions_duplicates_foreign_and_unobserved_cases(self):
        baseline = [{"scenario_id": scenario_id, "status": "observed"}
                    for scenario_id in acceptance.registered_cases(["mdbx", "rocksdb"], "plugin-relay")]
        with patch.object(acceptance, "validate_relay_case", return_value=[]):
            self.assertEqual(acceptance.validate_matrix(baseline, ["mdbx", "rocksdb"], "plugin-relay"), [])
            changed = deepcopy(baseline)
            changed[0]["status"] = "passed"
            for records in (baseline[:-1], baseline + [baseline[0]], changed,
                            baseline + [{"scenario_id": "autorelay-lifecycle-tcp-go-go-forge", "status": "observed"}],
                            baseline + [{"scenario_id": [], "status": "observed"}]):
                self.assertEqual(acceptance.validate_matrix(baseline, ["mdbx", "rocksdb"], "plugin-relay"), [])
                self.assertTrue(acceptance.validate_matrix(records, ["mdbx", "rocksdb"], "plugin-relay"))
            for backends in ([], ["memory"], ["mdbx", "mdbx"], [True]):
                self.assertTrue(acceptance.validate_matrix(baseline, backends, "plugin-relay"))


class CanonicalArtifactTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        self.source = self.root / "tests/libp2p_interop"
        self.source.mkdir(parents=True)
        self.build = self.root / "build"
        self.build.mkdir()
        self.run_root = self.build / "plugin-relay-run"
        self.run_root.mkdir()
        self.artifact_path = self.build / "plugin-relay-artifacts.json"
        self.donors_root = self.root / "donors"
        self.donors_root.mkdir()
        self.identity = {"head": "a" * 40, "worktree_sha256": "b" * 64, "dirty": False,
                         "exact_identity": "git:" + "a" * 40 + ";worktree-sha256:" + "b" * 64}
        real_source = Path(__file__).resolve().parent
        for name in ("donor_cases.json", "fixture-lock.json"):
            (self.source / name).write_text((real_source / name).read_text())
        lock = load_json(self.source / "fixture-lock.json")
        revisions, errors = fixture_donor_revision_bindings(lock["donors"],
            load_canonical_donor_revisions(self.source / "donor_cases.json"))
        self.assertEqual(errors, [])
        recipe = build_provenance(self.build)
        self.paths = {impl: Path(binary["path"]) for impl, binary in recipe["binaries"].items()}
        self.paths["forge"] = self.build / "forge-plugin-fixture"
        for implementation, path in self.paths.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("synthetic non-executable " + implementation + "\n")
        recipe["binaries"] = {impl: {"path": str(path), "sha256": provenance_checker.sha256_file(path)}
                              for impl, path in self.paths.items()}
        self.records = [synthetic_record(spec, self.run_root, self.paths, index)
                        for index, spec in enumerate(acceptance.registered_cases(["mdbx", "rocksdb"], "plugin-relay").values())]
        inputs = {"suite": "plugin-relay", "forge_root": str(self.root), "source_dir": str(self.source),
            "build_dir": str(self.build), "donors_root": str(self.donors_root), "acceptance_manifest": None,
            "plugin_store_backend": "all", "plugin_store_backends": ["mdbx", "rocksdb"]}
        recipe.update({"runner_inputs": inputs, "forge_worktree": {
            "start": self.identity, "end": self.identity, "changed_during_run": False},
            "fixture_build_info": {"schema_version": 2, "forge": self.identity, "fixture_kind": "configured-plugin",
                "store_backends": ["mdbx", "rocksdb"], "compiler": {"path": "/synthetic/clang", "id": "Clang", "version": "22"},
                "build_profile": "Release"}, "donors": lock["donors"], "donor_revisions": revisions})
        recipe["tools"]["python"] = {"path": str(Path(sys.executable).resolve()), "version_output": "synthetic Python version"}
        argv = [str(Path(sys.executable).resolve()), str(self.source / "runner.py"), "--enabled", "1", "--suite", "plugin-relay",
            "--forge-fixture", str(self.paths["forge"]), "--forge-root", str(self.root), "--source-dir", str(self.source),
            "--build-dir", str(self.build), "--donors-root", str(self.donors_root), "--plugin-store-backend", "all"]
        self.artifact = {"schema_version": 2, "fixture_provenance": recipe, "artifact_root": str(self.run_root),
            "acceptance_manifest": None, "artifacts": self.records, "failures": [], "runner_argv": argv,
            "started_at_unix": 2000, "finished_at_unix": 2001, "evidence_index": []}
        self.assertEqual(self.check(), [])

    def check(self, *, artifact=None, path=None, suite="plugin-relay", index=True, checkout_errors=None):
        artifact = artifact if artifact is not None else self.artifact
        path = path if path is not None else self.artifact_path
        if index:
            artifact["evidence_index"] = runner.evidence_index(self.run_root, artifact["artifacts"])
        path.write_text(json.dumps(artifact))
        with ExitStack() as stack:
            stack.enter_context(patch.object(acceptance, "validate_git_state", return_value=(1000, [])))
            stack.enter_context(patch.object(acceptance, "worktree_identity",
                return_value=SimpleNamespace(as_json=lambda: self.identity)))
            # Only external Git/process observations are mocked. Actual SHA,
            # recipes, donor pins, recursive index and configured sources run.
            stack.enter_context(patch.object(provenance_checker.subprocess, "check_output", return_value="synthetic Python version\n"))
            stack.enter_context(patch.object(provenance_checker, "fixture_donor_checkout_errors", return_value=checkout_errors or []))
            stack.enter_context(patch.object(acceptance.time, "time", return_value=3000))
            return acceptance.validate(self.root, path, self.identity["head"], suite)

    def test_existing_provenance_and_recursive_index_bind_all_eight_nested_cases(self):
        self.assertEqual(runner.evidence_index(self.run_root, self.records),
                         provenance_checker.build_evidence_index(self.run_root, self.records))
        indexed = {self.run_root / entry["path"] for entry in self.artifact["evidence_index"]}
        expected = {Path(owner["log_file"]) for record in self.records for owner in record["native"]["processes"]}
        expected.update(Path(output["log_file"]) for record in self.records
                        for owner in record["native"]["processes"] for output in owner["outputs"])
        self.assertTrue(expected <= indexed)
        with patch.object(acceptance, "validate_execution_provenance", wraps=acceptance.validate_execution_provenance) as execution, \
                patch.object(acceptance, "validate_donor_provenance", wraps=acceptance.validate_donor_provenance) as donors, \
                patch.object(acceptance, "validate_relay_sources", wraps=validate_terminal_sources) as sources, \
                patch.object(acceptance, "validate_terminal_sources") as node_sources, \
                patch.object(acceptance, "validate_dht_sources") as dht_sources, \
                patch.object(acceptance, "validate_pubsub_sources") as pubsub_sources:
            self.assertEqual(self.check(), [])
        execution.assert_called_once()
        donors.assert_called_once()
        self.assertEqual(sources.call_count, 8)
        node_sources.assert_not_called()
        dht_sources.assert_not_called()
        pubsub_sources.assert_not_called()

    def test_full_selection_rejects_missing_duplicate_and_one_backend(self):
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

    def test_noncanonical_suite_paths_and_stage6_promotions_are_rejected(self):
        mutations = (
            lambda a: a["fixture_provenance"]["runner_inputs"].update(suite="autorelay"),
            lambda a: a.update(artifact_root=str(self.build / "interop-run")),
            lambda a: a.update(acceptance_manifest={"path": "/stage6/manifest"}),
            lambda a: a["runner_argv"].extend(["--provenance-only", "1"]),
            lambda a: a["runner_argv"].extend(["--suite", "plugin-relay"]),
            lambda a: a["runner_argv"].__setitem__(a["runner_argv"].index("--suite") + 1, "autorelay"),
            lambda a: a["artifacts"][0]["native"].update(acceptance_scenario_ids=["autorelay_lifecycle"]),
        )
        for mutate in mutations:
            self.assertEqual(self.check(), [])
            artifact = deepcopy(self.artifact)
            mutate(artifact)
            self.assertTrue(self.check(artifact=artifact))
        self.assertEqual(self.check(), [])
        self.assertTrue(self.check(path=self.build / "autorelay-artifacts.json"))
        self.assertEqual(self.check(), [])
        self.assertTrue(self.check(suite="plugin-node"))

    def test_nested_sources_fail_even_with_a_fresh_index(self):
        for record in self.records:
            spec = record["spec"]
            role = "destination" if spec["kind"] == "lifecycle" else "relay"
            owner = record["native"]["raw"][role]["process"]
            for output in owner["outputs"]:
                for key in ("path", "log_file"):
                    with self.subTest(case=record["scenario_id"], field=output["argument"], source=key):
                        self.assertEqual(self.check(), [])
                        path = Path(output[key])
                        original = path.read_text()
                        try:
                            payload = load_json(path)
                            payload["configuration_accepted"] = False
                            path.write_text(json.dumps(payload))
                            errors = self.check()
                            self.assertTrue(any("terminal sources" in error for error in errors), errors)
                            self.assertFalse(any("index hash or size differs" in error for error in errors), errors)
                        finally:
                            path.write_text(original)

    def test_forged_forge_lifecycle_receipts_fail_even_with_consistent_sources_and_index(self):
        mutations = {
            "missing seed receipt": lambda r: r.pop("seed_connections"),
            "missing Forge echo": lambda r: r["echoes"].pop(),
            "wrong donor source": lambda r: r["echoes"][0]["session"].update(remote_peer_id=r["local_peer_id"]),
            "wrong carrier": lambda r: r["echoes"][0]["session"].update(carrier_session_id=5),
            "failed response write": lambda r: r["echoes"][0].update(response_write_completed=False),
            "failed stream close": lambda r: r["echoes"][0].update(stream_closed=False),
        }
        for index, baseline in enumerate(self.records):
            if baseline["spec"]["kind"] != "lifecycle":
                continue
            for name, mutation in mutations.items():
                with self.subTest(case=baseline["scenario_id"], mutation=name):
                    self.assertEqual(self.check(), [])
                    artifact = deepcopy(self.artifact)
                    changed = artifact["artifacts"][index]
                    mutation(changed["native"]["raw"]["destination"]["result"])
                    try:
                        recapture(changed)
                        errors = self.check(artifact=artifact)
                        self.assertTrue(any("configured Forge lifecycle receipts:" in error for error in errors), errors)
                        self.assertFalse(any("hash or size differs" in error for error in errors), errors)
                    finally:
                        recapture(baseline)

    def test_nested_index_hash_and_path_escape_cannot_be_skipped(self):
        self.assertEqual(self.check(), [])
        owner = self.records[0]["native"]["processes"][0]
        path = Path(owner["log_file"])
        original = path.read_text()
        try:
            path.write_text("tampered nested stdout\n")
            self.assertTrue(any("hash or size differs" in error for error in self.check(index=False)))
        finally:
            path.write_text(original)
        self.assertEqual(self.check(), [])
        artifact = deepcopy(self.artifact)
        artifact["artifacts"][0]["native"]["processes"][0]["outputs"][0]["log_file"] = str(self.build / "outside-run.json")
        self.assertTrue(any("escapes artifact_root" in error for error in self.check(artifact=artifact, index=False)))
        self.assertEqual(self.check(), [])
        artifact = deepcopy(self.artifact)
        artifact["evidence_index"].pop()
        self.assertTrue(any("exactly cover" in error for error in self.check(artifact=artifact, index=False)))

    def test_exact_sha_build_recipe_worktree_and_donor_pins_remain_authoritative(self):
        for implementation in ("forge", "go", "rust"):
            self.assertEqual(self.check(), [])
            artifact = deepcopy(self.artifact)
            artifact["fixture_provenance"]["binaries"][implementation]["sha256"] = "0" * 64
            self.assertTrue(any("SHA-256" in error for error in self.check(artifact=artifact)))
        mutations = (
            lambda p: p["donors"][0].update(commit="0" * 40),
            lambda p: p["donor_revisions"].update(**{"go-libp2p": "0" * 40}),
            lambda p: p["commands"][-1]["command"].remove("--frozen"),
            lambda p: p["commands"][-1]["environment"].update(CARGO_NET_OFFLINE="false"),
            lambda p: p["forge_worktree"].update(changed_during_run=True),
            lambda p: p["fixture_build_info"]["forge"].update(head="c" * 40),
        )
        for mutate in mutations:
            self.assertEqual(self.check(), [])
            artifact = deepcopy(self.artifact)
            mutate(artifact["fixture_provenance"])
            self.assertTrue(self.check(artifact=artifact))
        self.assertEqual(self.check(), [])
        self.assertIn("synthetic wrong donor checkout", self.check(checkout_errors=["synthetic wrong donor checkout"]))

    def test_failures_and_nested_unjoined_or_missing_actors_block_acceptance(self):
        mutations = (
            lambda a: a.update(failures=["owned cleanup failed"]),
            lambda a: a.pop("failures"),
            lambda a: a["artifacts"][0].update(status="passed"),
            lambda a: a["artifacts"][0]["native"]["processes"].pop(),
            lambda a: a["artifacts"][0]["native"]["processes"][0]["terminal_status"].update(termination="killed"),
            lambda a: a["artifacts"][0]["native"].update(cleanup_errors=["join failed"]),
        )
        for mutate in mutations:
            self.assertEqual(self.check(), [])
            artifact = deepcopy(self.artifact)
            mutate(artifact)
            self.assertTrue(self.check(artifact=artifact))


class RunnerBoundaryTests(unittest.TestCase):
    def test_composed_native_owner_is_not_wrapped_or_reentered(self):
        spec, order = case_specs("mdbx")[0], []
        record = {"status": "observed", "errors": [], "native": {"processes": [], "attempts": [], "cleanup_errors": []}}
        captured = {}

        def launch(selected, binaries, root, **kwargs):
            self.assertIsNone(current_scope())
            self.assertEqual(selected, spec)
            self.assertEqual(kwargs, {"wait_json": runner.wait_json, "command_attempt": runner.command_attempt,
                "start_destination": runner.start_destination, "run_relay_dial": runner.run_relay_dial})
            scope, token = enter_scope()
            try:
                def close():
                    self.assertIs(current_scope(), scope)
                    order.append("native-join-and-capture")
                    captured["joined"] = True
                    return []

                order.append("native-launch")
                scope.processes.append(SimpleNamespace(close=close, evidence=lambda: dict(captured)))
                self.assertEqual(scope.close(), [])
                record["native"]["processes"] = scope.evidence()
            finally:
                exit_scope(token)
            self.assertIsNone(current_scope())
            order.append("delegate-sources-after-join")
            return record

        with patch.object(runner, "plugin_relay_case", side_effect=launch) as delegate, \
                patch.object(runner, "enter_scope", side_effect=AssertionError("second owner forbidden")):
            self.assertIs(runner.run_plugin_relay_case(spec, {}, Path("/case")), record)
        delegate.assert_called_once()
        self.assertEqual(order, ["native-launch", "native-join-and-capture", "delegate-sources-after-join"])
        self.assertEqual(record["native"]["processes"], [{"joined": True}])

    def test_failure_retains_original_nested_receipts_without_observed_relabel(self):
        spec = case_specs("rocksdb")[0]
        baseline = {"status": "observed", "errors": [], "native": {"processes": [{"pid": 101}], "attempts": []}}
        with patch.object(runner, "plugin_relay_case", return_value=baseline):
            self.assertIs(runner.run_plugin_relay_case(spec, {}, Path("/case")), baseline)
        broken = deepcopy(baseline)
        broken.update(status="failed", errors=["source/lease proof failed"])
        broken["native"]["cleanup_errors"] = ["native join failed"]
        with patch.object(runner, "plugin_relay_case", return_value=broken), self.assertRaises(runner.CaseFailure) as failed:
            runner.run_plugin_relay_case(spec, {}, Path("/case"))
        self.assertEqual(broken["status"], "failed")
        self.assertEqual(failed.exception.cleanup_errors, ["native join failed"])
        self.assertIs(failed.exception.artifact["uncommitted_result"], broken)
        self.assertIs(failed.exception.artifact["processes"], broken["native"]["processes"])
        artifacts, failures = [], []
        runner.record_case_failure(artifacts, failures, spec.scenario_id, failed.exception)
        self.assertEqual(len(failures), 1)
        self.assertIs(artifacts[0]["uncommitted_result"], broken)

    def test_nested_scope_is_rejected_before_delegation(self):
        with patch.object(runner, "current_scope", return_value=object()), patch.object(runner, "plugin_relay_case") as delegate, \
                self.assertRaises(RuntimeError):
            runner.run_plugin_relay_case(case_specs("mdbx")[0], {}, Path("/case"))
        delegate.assert_not_called()


class CommandLineTests(unittest.TestCase):
    def test_cli_reports_only_configured_observation_and_preserves_failure_exit(self):
        argv = ["plugin_node_acceptance.py", "--forge-root", "/forge", "--artifact", "/artifact.json",
                "--head", "a" * 40, "--suite", "plugin-relay"]
        with patch.object(sys, "argv", argv), patch.object(acceptance, "validate", return_value=[]) as check, \
                redirect_stdout(io.StringIO()) as output, redirect_stderr(io.StringIO()):
            self.assertEqual(acceptance.main(), 0)
        self.assertEqual(check.call_args.args[-1], "plugin-relay")
        self.assertIn("configured-plugin AutoRelay selection observed", output.getvalue())
        self.assertIn("no Stage6 capability or whole Stage7 promotion", output.getvalue())
        with patch.object(sys, "argv", argv), patch.object(acceptance, "validate", return_value=["missing relay lease"]), \
                redirect_stdout(io.StringIO()) as output, redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(acceptance.main(), 1)
        self.assertEqual(output.getvalue(), "")
        self.assertIn("PLUGIN-RELAY EVIDENCE ERROR: missing relay lease", errors.getvalue())


if __name__ == "__main__":
    unittest.main()
