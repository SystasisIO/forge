"""Synthetic closed configured AutoNAT gate; external observations only are mocked."""

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
from plugin_node_evidence import load_json
from plugin_autonat_cases import case_specs
from plugin_autonat_evidence import validate_terminal_sources
from provenance import WorktreeIdentity, fixture_donor_revision_bindings, load_canonical_donor_revisions
import runner
from test_plugin_autonat_evidence import recapture, synthetic_record
from test_plugin_node_acceptance import build_provenance


class RegistrationTests(unittest.TestCase):
    def test_separate_suite_is_exact_eight_and_other_matrices_stay_registered(self):
        self.assertEqual(acceptance.SUITES, runner.CONFIGURED_PLUGIN_SUITES)
        registered = acceptance.registered_cases(["mdbx", "rocksdb"], "plugin-autonat")
        self.assertEqual(len(registered), 8)
        self.assertEqual({(s.backend, s.client, s.server, s.version, s.transport) for s in registered.values()},
            {(backend, client, server, version, "tcp") for backend in ("mdbx", "rocksdb")
             for client, server, version in (("forge", "rust", 1), ("rust", "forge", 1),
                                             ("forge", "go", 2), ("go", "forge", 2))})
        for suite, count in (("plugin-node", 84), ("plugin-dht", 16), ("plugin-pubsub", 8), ("plugin-relay", 8)):
            self.assertEqual(len(acceptance.registered_cases(["mdbx", "rocksdb"], suite)), count)
        with self.assertRaises(ValueError):
            acceptance.registered_cases(["mdbx", "rocksdb"], "autonat")

    def test_all_eight_both_backends_required_even_when_backend_selection_is_explicit(self):
        baseline = [{"scenario_id": name, "status": "observed"}
                    for name in acceptance.registered_cases(["mdbx", "rocksdb"], "plugin-autonat")]
        with patch.object(acceptance, "validate_autonat_case", return_value=[]):
            self.assertEqual(acceptance.validate_matrix(baseline, ["mdbx", "rocksdb"], "plugin-autonat"), [])
            one_backend = [r for r in baseline if r["scenario_id"].startswith("plugin-autonat/mdbx/")]
            self.assertTrue(any("all 8" in error for error in acceptance.validate_matrix(one_backend, ["mdbx"], "plugin-autonat")))
            for records in (baseline[:-1], baseline + [baseline[0]],
                            baseline + [{"scenario_id": "autonat_v1-tcp-forge-to-rust-reachable", "status": "observed"}],
                            baseline + [{"scenario_id": [], "status": "observed"}]):
                self.assertTrue(acceptance.validate_matrix(records, ["mdbx", "rocksdb"], "plugin-autonat"))
            for status in ("passed", "not_run", "failed"):
                records = deepcopy(baseline)
                records[0]["status"] = status
                self.assertTrue(acceptance.validate_matrix(records, ["mdbx", "rocksdb"], "plugin-autonat"))

    def test_runner_bridge_does_not_add_an_owner_or_drop_failure_evidence(self):
        spec = case_specs("mdbx")[0]
        success = {"status": "observed", "errors": []}
        with patch.object(runner, "plugin_autonat_case", return_value=success) as case:
            self.assertIs(runner.run_plugin_autonat_case(spec, {}, Path("/synthetic")), success)
        case.assert_called_once_with(spec, {}, Path("/synthetic"), wait_json=runner.wait_json, command_attempt=runner.command_attempt)
        failure = {"status": "failed", "errors": ["paired dialback missing"], "native": {
            "cleanup_errors": ["unjoined process"], "owned_processes": [{"pid": 123}], "attempts": [{"kind": "server"}]}}
        with patch.object(runner, "plugin_autonat_case", return_value=failure), self.assertRaises(runner.CaseFailure) as error:
            runner.run_plugin_autonat_case(spec, {}, Path("/synthetic"))
        self.assertEqual(error.exception.artifact["uncommitted_result"], failure)
        self.assertEqual(error.exception.artifact["cleanup_errors"], ["unjoined process"])
        self.assertEqual(error.exception.artifact["processes"], [{"pid": 123}])
        with patch.object(runner, "current_scope", return_value=object()), patch.object(runner, "plugin_autonat_case") as case, \
                self.assertRaises(RuntimeError):
            runner.run_plugin_autonat_case(spec, {}, Path("/synthetic"))
        case.assert_not_called()


class CanonicalArtifactTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        self.source = self.root / "tests/libp2p_interop"
        self.source.mkdir(parents=True)
        self.build = self.root / "build"
        self.build.mkdir()
        self.run_root = self.build / "plugin-autonat-run"
        self.run_root.mkdir()
        self.artifact_path = self.build / "plugin-autonat-artifacts.json"
        self.donors_root = self.root / "donors"
        self.donors_root.mkdir()
        self.identity = WorktreeIdentity("a" * 40, "b" * 64, False).as_json()
        source = Path(__file__).resolve().parent
        for name in ("donor_cases.json", "fixture-lock.json"):
            (self.source / name).write_text((source / name).read_text())
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
        self.records = [synthetic_record(spec, self.run_root, self.paths, index) for index, spec in enumerate(
            acceptance.registered_cases(["mdbx", "rocksdb"], "plugin-autonat").values())]
        inputs = {"suite": "plugin-autonat", "forge_root": str(self.root), "source_dir": str(self.source),
            "build_dir": str(self.build), "donors_root": str(self.donors_root), "acceptance_manifest": None,
            "plugin_store_backend": "all", "plugin_store_backends": ["mdbx", "rocksdb"]}
        recipe.update(runner_inputs=inputs, forge_worktree={
            "start": self.identity, "end": self.identity, "changed_during_run": False},
            fixture_build_info={"schema_version": 2, "forge": self.identity, "fixture_kind": "configured-plugin",
                "store_backends": ["mdbx", "rocksdb"], "compiler": {"path": "/synthetic/clang", "id": "Clang", "version": "22"},
                "build_profile": "Release"}, donors=lock["donors"], donor_revisions=revisions)
        recipe["tools"]["python"] = {"path": str(Path(sys.executable).resolve()), "version_output": "synthetic Python version"}
        argv = [str(Path(sys.executable).resolve()), str(self.source / "runner.py"), "--enabled", "1", "--suite", "plugin-autonat",
            "--forge-fixture", str(self.paths["forge"]), "--forge-root", str(self.root), "--source-dir", str(self.source),
            "--build-dir", str(self.build), "--donors-root", str(self.donors_root), "--plugin-store-backend", "all"]
        self.artifact = {"schema_version": 2, "fixture_provenance": recipe, "artifact_root": str(self.run_root),
            "acceptance_manifest": None, "artifacts": self.records, "failures": [], "runner_argv": argv,
            "started_at_unix": 2000, "finished_at_unix": 2001, "evidence_index": []}
        self.assertEqual(self.check(), [])

    def check(self, *, artifact=None, path=None, suite="plugin-autonat", index=True, checkout_errors=None):
        artifact = artifact if artifact is not None else self.artifact
        path = path if path is not None else self.artifact_path
        if index:
            artifact["evidence_index"] = runner.evidence_index(self.run_root, artifact["artifacts"])
        path.write_text(json.dumps(artifact))
        with ExitStack() as stack:
            stack.enter_context(patch.object(acceptance, "validate_git_state", return_value=(1000, [])))
            stack.enter_context(patch.object(acceptance, "worktree_identity", return_value=SimpleNamespace(as_json=lambda: self.identity)))
            stack.enter_context(patch.object(provenance_checker.subprocess, "check_output", return_value="synthetic Python version\n"))
            stack.enter_context(patch.object(provenance_checker, "fixture_donor_checkout_errors", return_value=checkout_errors or []))
            stack.enter_context(patch.object(acceptance.time, "time", return_value=3000))
            return acceptance.validate(self.root, path, self.identity["head"], suite)

    def test_all_eight_bind_existing_provenance_index_network_and_actual_source_checker(self):
        self.assertEqual(self.check(), [])
        self.assertEqual(runner.evidence_index(self.run_root, self.records),
                         provenance_checker.build_evidence_index(self.run_root, self.records))
        with patch.object(acceptance, "validate_execution_provenance", wraps=acceptance.validate_execution_provenance) as execution, \
                patch.object(acceptance, "validate_donor_provenance", wraps=acceptance.validate_donor_provenance) as donors, \
                patch.object(acceptance, "validate_autonat_sources", wraps=validate_terminal_sources) as sources, \
                patch.object(acceptance, "validate_relay_sources") as relay, \
                patch.object(acceptance, "validate_terminal_sources") as node:
            self.assertEqual(self.check(), [])
        execution.assert_called_once()
        donors.assert_called_once()
        self.assertEqual(sources.call_count, 8)
        relay.assert_not_called()
        node.assert_not_called()

    def test_partial_matrix_and_backend_override_never_count_as_complete_acceptance(self):
        for records in (self.records[:-1], self.records + [self.records[0]]):
            artifact = deepcopy(self.artifact)
            artifact["artifacts"] = records
            self.assertTrue(self.check(artifact=artifact))
        for selected in ("all", "mdbx"):
            artifact = deepcopy(self.artifact)
            provenance = artifact["fixture_provenance"]
            provenance["runner_inputs"].update(plugin_store_backends=["mdbx"], plugin_store_backend=selected)
            provenance["fixture_build_info"]["store_backends"] = ["mdbx"]
            artifact["artifacts"] = [record for record in artifact["artifacts"] if record["spec"]["backend"] == "mdbx"]
            flags = artifact["runner_argv"]
            flags[flags.index("--plugin-store-backend") + 1] = selected
            self.assertTrue(any("8" in error for error in self.check(artifact=artifact)))

    def test_raw_suite_promotion_foreign_argv_and_not_run_cannot_relabel_configured_evidence(self):
        mutations = (
            lambda a: a.update(acceptance_manifest={"path": "/stage6/manifest"}),
            lambda a: a["fixture_provenance"]["runner_inputs"].update(suite="autonat"),
            lambda a: a["runner_argv"].extend(["--suite", "plugin-autonat"]),
            lambda a: a["runner_argv"].extend(["--provenance-only", "1"]),
            lambda a: a["artifacts"][0]["native"].update(acceptance_scenario_ids=["autonat_v1_client_native_tcp_yamux"]),
            lambda a: a["artifacts"][0]["preflight"].update(observed_platform="Darwin", status="not_run"),
            lambda a: a["artifacts"][0].update(status="not_run"),
        )
        for mutate in mutations:
            artifact = deepcopy(self.artifact)
            mutate(artifact)
            self.assertTrue(self.check(artifact=artifact))
        self.assertTrue(self.check(path=self.build / "autonat-artifacts.json"))
        self.assertTrue(self.check(suite="plugin-node"))

    def test_native_causal_evidence_still_fails_after_recapture_and_fresh_index(self):
        mutations = (
            lambda r, role: r[role].update(autonat_probe_api_calls=1),
            lambda r, role: r[role]["diagnostics_before_stop"]["reachability"].update(probe_errors=1),
            lambda r, role: r[role]["diagnostics_after_detach"].update(rejected=False),
            lambda r, role: r[role]["accepted_configuration"].update(identity_source="raw-node"),
            lambda r, role: r["client"].update(actual_connections=[c for c in r["client"]["actual_connections"] if c["direction"] != "inbound"]),
        )
        for index, baseline in enumerate(self.records):
            for mutate in mutations:
                artifact = deepcopy(self.artifact)
                record = artifact["artifacts"][index]
                mutate(record["native"], "client" if record["spec"]["client"] == "forge" else "server")
                recapture(record)
                try:
                    errors = self.check(artifact=artifact)
                    self.assertTrue(errors)
                    self.assertFalse(any("index hash or size differs" in error for error in errors), errors)
                finally:
                    recapture(baseline)

    def test_cli_original_capture_network_transcript_and_binary_pins_are_bound(self):
        artifact = deepcopy(self.artifact)
        owner = artifact["artifacts"][0]["native"]["owned_processes"][1]
        owner["command"] = owner["command"][4:]
        self.assertTrue(self.check(artifact=artifact))
        record = self.records[0]
        output = record["native"]["owned_processes"][0]["outputs"][0]
        path = Path(output["path"])
        original = path.read_text()
        try:
            path.write_text('{"status":"ok"}')
            self.assertTrue(any("terminal sources" in error for error in self.check()))
        finally:
            path.write_text(original)
        artifact = deepcopy(self.artifact)
        network = artifact["artifacts"][0]["native"]["network"]
        command = next(c for c in network["commands"] if c["command"][-4:] == ["-j", "route", "show", "default"])
        command["stdout"] = '[{"dst":"default"}]'
        path = Path(network["result_file"])
        original = path.read_text()
        try:
            path.write_text(json.dumps({key: value for key, value in network.items() if key != "result_file"}))
            self.assertTrue(self.check(artifact=artifact))
        finally:
            path.write_text(original)
        self.assertTrue(self.check(checkout_errors=["donor not at exact pinned revision"]))
        self.paths["forge"].write_text("changed binary\n")
        self.assertTrue(any("SHA-256" in error for error in self.check()))

    def test_missing_index_and_forced_termination_cannot_be_joined_success(self):
        self.assertEqual(self.check(), [])
        artifact = deepcopy(self.artifact)
        artifact["evidence_index"].pop()
        self.assertTrue(self.check(artifact=artifact, index=False))
        artifact = deepcopy(self.artifact)
        for process in (*artifact["artifacts"][0]["native"]["owned_processes"], *artifact["artifacts"][0]["native"]["attempts"]):
            process["terminal_status"] = {"exit_code": 0, "termination": "terminated"}
        self.assertTrue(self.check(artifact=artifact))

    def test_canonical_runner_mac_preflight_records_all_not_run_without_donor_build_or_case_launch(self):
        identity = WorktreeIdentity(self.identity["head"], self.identity["worktree_sha256"], False)
        preflight = {"source": "python.platform.system", "required_platform": "Linux", "observed_platform": "Darwin",
                     "status": "not_run", "reason": "NOT_RUN: isolated configured AutoNAT requires Linux; no host-network fallback"}
        lock = load_json(self.source / "fixture-lock.json")
        build_info = self.artifact["fixture_provenance"]["fixture_build_info"]
        with ExitStack() as stack:
            stack.enter_context(redirect_stdout(io.StringIO()))
            stack.enter_context(redirect_stderr(io.StringIO()))
            stack.enter_context(patch.object(runner.sys, "argv", self.artifact["runner_argv"][1:]))
            stack.enter_context(patch.object(runner, "worktree_identity", return_value=identity))
            stack.enter_context(patch.object(runner, "require_tool", side_effect=lambda name: "/tools/" + name))
            stack.enter_context(patch.object(runner, "command_output", return_value="synthetic version"))
            stack.enter_context(patch.object(runner, "load_fixture_lock", return_value=lock))
            stack.enter_context(patch.object(runner, "require_fixture_provenance", return_value=(build_info, {"command": [str(self.paths["forge"]), "build-info"]})))
            stack.enter_context(patch.object(runner, "plugin_autonat_preflight", return_value=preflight))
            donor_check = stack.enter_context(patch.object(runner, "fixture_donor_checkout_errors"))
            go = stack.enter_context(patch.object(runner, "prepare_go_fixture"))
            rust = stack.enter_context(patch.object(runner, "prepare_rust_fixture"))
            case = stack.enter_context(patch.object(runner, "run_plugin_autonat_case"))
            self.assertEqual(runner.main(), 1)
        for hook in (donor_check, go, rust, case):
            hook.assert_not_called()
        artifact = load_json(self.artifact_path)
        self.assertEqual(len(artifact["artifacts"]), 8)
        self.assertTrue(all(r["status"] == "not_run" and r["native"] is None for r in artifact["artifacts"]))
        self.assertTrue(any("NOT_RUN" in failure for failure in artifact["failures"]))
        self.assertTrue(self.check(artifact=artifact))


if __name__ == "__main__":
    unittest.main()
