"""Configured selection/owner tests with no execution of fixture binaries."""

from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import autonat_acceptance
import autonat_cases
import plugin_autonat_cases as cases
from plugin_autonat_evidence import configured_pair, validate_case, validate_terminal_sources
from test_plugin_autonat_evidence import synthetic_record


class PluginAutonatCaseTests(unittest.TestCase):
    def test_closed_eight_case_matrix_is_exact_public_v1_rust_v2_go_bilateral(self):
        selected = [spec for backend in cases.BACKENDS for spec in cases.case_specs(backend)]
        self.assertEqual(len(selected), 8)
        self.assertEqual(len({spec.scenario_id for spec in selected}), 8)
        self.assertEqual({(s.backend, s.client, s.server, s.version, s.transport) for s in selected},
            {(backend, client, server, version, "tcp") for backend in ("mdbx", "rocksdb")
             for client, server, version in (("forge", "rust", 1), ("rust", "forge", 1),
                                              ("forge", "go", 2), ("go", "forge", 2))})
        self.assertTrue(all(spec.native in autonat_cases.case_specs() for spec in selected))
        for values in (("memory", "forge", "rust", 1), ("mdbx", "forge", "go", 1),
                       ("mdbx", "forge", "rust", 2), ("mdbx", "forge", "rust", True),
                       ("mdbx", "forge", "rust", 1, "quic"), ("mdbx", "forge", "rust", 1, "tcp-pnet")):
            with self.subTest(values=values), self.assertRaises(ValueError):
                cases.Case(*values)
        with self.assertRaises(ValueError):
            cases.case_specs("memory")
        self.assertIsNone(autonat_cases.run_case.__kwdefaults__["forge_cli_arguments"])
        self.assertIs(autonat_cases.run_case.__kwdefaults__["semantic_checker"], autonat_cases.validate_autonat_evidence)
        self.assertIsNone(autonat_acceptance.validate_pair.__kwdefaults__["forge_cli_arguments"])
        self.assertIs(autonat_acceptance.validate_pair.__kwdefaults__["semantic_checker"], autonat_cases.validate_autonat_evidence)

    def test_wrapper_reuses_native_owner_and_only_checks_sources_after_owned_teardown(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            binaries = {implementation: root / implementation for implementation in ("forge", "go", "rust")}
            for path in binaries.values():
                path.write_text("synthetic non-executable binary\n")
            for index, spec in enumerate(spec for backend in cases.BACKENDS for spec in cases.case_specs(backend)):
                completed = []
                support = {key: object() for key in ("wait_json", "command_attempt")}

                def native(native_spec, actual_binaries, native_root, **kwargs):
                    self.assertEqual((native_spec, actual_binaries, native_root), (spec.native, binaries, cases.case_root(root, spec)))
                    self.assertIs(kwargs["wait_json"], support["wait_json"])
                    self.assertIs(kwargs["command_attempt"], support["command_attempt"])
                    self.assertIs(kwargs["semantic_checker"], configured_pair)
                    self.assertIsNone(kwargs["pnet_key"])
                    self.assertEqual(kwargs["pnet_fingerprint"], "")
                    store = native_root / native_spec.identifier / (spec.forge_role + "-store")
                    self.assertFalse(store.exists())
                    self.assertEqual(kwargs["forge_cli_arguments"](spec.forge_role, store), ("--store-backend", spec.backend))
                    result = synthetic_record(spec, root, binaries, index)["native"]
                    completed.append(True)
                    return result

                def semantics(record, selected):
                    self.assertEqual(completed, [True])
                    return validate_case(record, selected)

                def sources(record, actual_binaries, artifact_root):
                    self.assertEqual(completed, [True])
                    return validate_terminal_sources(record, actual_binaries, artifact_root)

                with self.subTest(case=spec.scenario_id), patch.object(cases.platform, "system", return_value="Linux"), \
                        patch.object(cases, "native_case", side_effect=native) as owner, \
                        patch("plugin_autonat_evidence.validate_case", side_effect=semantics), \
                        patch("plugin_autonat_evidence.validate_terminal_sources", side_effect=sources):
                    result = cases.run_case(spec, binaries, root, **support)
                self.assertEqual(result["status"], "observed", result)
                self.assertEqual(result["errors"], [])
                self.assertEqual(result["native"]["acceptance_scenario_ids"], [])
                self.assertIs(result["forge_store"]["existed_before_spawn"], False)
                owner.assert_called_once()

    def test_macos_and_other_nonlinux_are_not_run_before_paths_or_native_owner(self):
        for system in ("Darwin", "Windows", "FreeBSD"):
            with self.subTest(system=system), patch.object(cases.platform, "system", return_value=system), \
                    patch.object(cases, "native_case") as native, patch.object(Path, "mkdir") as mkdir:
                result = cases.run_case(cases.case_specs("mdbx")[0], {}, Path("/no-host-network"),
                                        wait_json=None, command_attempt=None)
            self.assertEqual(result["status"], "not_run")
            self.assertIn("NOT_RUN", result["preflight"]["reason"])
            self.assertIsNone(result["native"])
            native.assert_not_called()
            mkdir.assert_not_called()
            self.assertTrue(validate_case(result, cases.case_specs("mdbx")[0]))

    def test_nested_owner_is_rejected_before_platform_or_spawn(self):
        with patch.object(cases, "current_scope", return_value=object()), patch.object(cases, "platform_preflight") as preflight, \
                patch.object(cases, "native_case") as native, self.assertRaises(RuntimeError):
            cases.run_case(cases.case_specs("mdbx")[0], {}, Path("/no-new-runtime"), wait_json=None, command_attempt=None)
        preflight.assert_not_called()
        native.assert_not_called()

    def test_fresh_store_hook_rejects_reuse_wrong_owner_symlink_and_duplicate_spawn(self):
        for mode in ("reuse", "wrong owner", "wrong path", "symlink", "duplicate"):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory).resolve()
                spec = cases.case_specs("mdbx")[0]

                def native(native_spec, binaries, native_root, **kwargs):
                    store = native_root / native_spec.identifier / (spec.forge_role + "-store")
                    role = spec.forge_role
                    if mode == "reuse":
                        store.mkdir(parents=True)
                    elif mode == "wrong owner":
                        role = "server"
                    elif mode == "wrong path":
                        store = root / "unowned-store"
                    elif mode == "symlink":
                        store.parent.mkdir(parents=True)
                        store.symlink_to(root / "missing-target", target_is_directory=True)
                    elif mode == "duplicate":
                        self.assertEqual(kwargs["forge_cli_arguments"](role, store), ("--store-backend", "mdbx"))
                    return kwargs["forge_cli_arguments"](role, store)

                with self.subTest(mode=mode), patch.object(cases.platform, "system", return_value="Linux"), \
                        patch.object(cases, "native_case", side_effect=native), self.assertRaises(ValueError):
                    cases.run_case(spec, {}, root, wait_json=None, command_attempt=None)

    def test_native_cleanup_failure_cannot_be_repaired_by_configured_pass_labels(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            spec = cases.case_specs("mdbx")[0]
            binaries = {implementation: root / implementation for implementation in ("forge", "go", "rust")}
            record = synthetic_record(spec, root, binaries, mutate_harness=lambda harness: setattr(harness, "server_forced", True))
            self.assertTrue(validate_case(record, spec))
            self.assertNotEqual(record["native"]["owned_processes"][0]["terminal_status"]["termination"], "graceful")


if __name__ == "__main__":
    unittest.main()
