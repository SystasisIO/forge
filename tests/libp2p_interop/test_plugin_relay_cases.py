"""Closed configured selection and orchestration reuse; no child processes run."""

from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import autorelay_cases
import plugin_relay_cases as cases
from plugin_relay_evidence import configured_native_case, validate_case, validate_terminal_sources
from test_plugin_relay_evidence import synthetic_record


class PluginRelayCaseTests(unittest.TestCase):
    def test_eight_closed_cases_are_exactly_native_tcp_matrix_times_backends(self):
        native = {spec for spec in autorelay_cases.case_specs() if spec.transport == "tcp"}
        selected = [spec for backend in cases.BACKENDS for spec in cases.case_specs(backend)]
        self.assertEqual(len(selected), 8)
        self.assertEqual(len({spec.scenario_id for spec in selected}), 8)
        for backend in cases.BACKENDS:
            self.assertEqual({spec.native for spec in cases.case_specs(backend)}, native)
        for spec in selected:
            self.assertEqual(spec.forge_role, "destination" if spec.kind == "lifecycle" else "relay")
        for values in (("memory", "go", "go", "forge", "lifecycle"),
                       ("mdbx", "go", "go", "forge", "lifecycle", "quic"),
                       ("mdbx", "go", "rust", "forge", "lifecycle")):
            with self.subTest(values=values), self.assertRaises(ValueError):
                cases.Case(*values)
        with self.assertRaises(ValueError):
            cases.case_specs("memory")

    def test_actual_listener_builder_defaults_are_unchanged_and_only_forge_gets_hook(self):
        binaries = {impl: Path("/binary") / impl for impl in ("forge", "go", "rust")}
        work = Path("/case")
        for spec in autorelay_cases.case_specs():
            labels = (("relay", spec.relay, "autorelay-service" if spec.relay == "forge" else "autorelay-relay"),
                      ("destination", "forge", "autorelay-destination")) if spec.kind == "lifecycle" else (
                          ("relay", "forge", "autorelay-service"),)
            for label, implementation, command in labels:
                extra = ["--seed-file", str(work / "seeds.txt")] if label == "destination" else []
                expected = [str(binaries[implementation]), command, "--scenario", "autorelay", "--transport", spec.transport,
                            "--ready-file", str(work / (label + "-ready.json")), "--stop-file", str(work / (label + ".stop")),
                            "--result-file", str(work / (label + ".json")), *extra]
                if implementation == "forge":
                    expected += ["--store-dir", str(work / (label + "-store"))]
                self.assertEqual(autorelay_cases._listener_command(
                    spec, binaries, work, label, implementation, command, extra), expected)
                calls = []

                def hook(actual_label, actual_command, store):
                    calls.append((actual_label, actual_command, store))
                    return ("--store-backend", "mdbx")

                actual = autorelay_cases._listener_command(spec, binaries, work, label, implementation, command,
                                                           extra, forge_cli_arguments=hook)
                self.assertEqual(actual, expected + (["--store-backend", "mdbx"] if implementation == "forge" else []))
                self.assertEqual(calls, [(label, command, work / (label + "-store"))] if implementation == "forge" else [])
        self.assertIs(autorelay_cases.run_case.__kwdefaults__["semantic_checker"], autorelay_cases.validate_case)

    def test_wrapper_reuses_native_owner_and_checks_sources_only_after_return(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            binaries = {impl: root / impl for impl in ("forge", "go", "rust")}
            for path in binaries.values():
                path.write_text("synthetic binary, never executed\n")
            for index, spec in enumerate(spec for backend in cases.BACKENDS for spec in cases.case_specs(backend)):
                finished = []
                support = {key: object() for key in ("wait_json", "command_attempt", "start_destination", "run_relay_dial")}

                def native(native_spec, actual_binaries, native_root, **kwargs):
                    self.assertEqual((native_spec, actual_binaries, native_root), (spec.native, binaries, cases.case_root(root, spec)))
                    for key, value in support.items():
                        self.assertIs(kwargs[key], value)
                    self.assertEqual(kwargs["claims_for_case"](native_spec), [])
                    self.assertIs(kwargs["semantic_checker"], configured_native_case)
                    store = native_root / native_spec.identifier / (spec.forge_role + "-store")
                    command = "autorelay-destination" if spec.kind == "lifecycle" else "autorelay-service"
                    self.assertFalse(store.exists())
                    self.assertEqual(kwargs["forge_cli_arguments"](spec.forge_role, command, store),
                                     ("--store-backend", spec.backend))
                    result = synthetic_record(spec, root, binaries, index)["native"]
                    finished.append(True)
                    return result

                def semantics(record, selected):
                    self.assertEqual(finished, [True])
                    return validate_case(record, selected)

                def sources(record, actual_binaries, artifact_root):
                    self.assertEqual(finished, [True])
                    return validate_terminal_sources(record, actual_binaries, artifact_root)

                with self.subTest(case=spec.scenario_id), patch.object(cases, "native_case", side_effect=native) as owner, \
                        patch("plugin_relay_evidence.validate_case", side_effect=semantics), \
                        patch("plugin_relay_evidence.validate_terminal_sources", side_effect=sources):
                    result = cases.run_case(spec, binaries, root, **support)
                    self.assertEqual(result["errors"], [])
                    self.assertEqual(result["status"], "observed")
                    self.assertIs(result["forge_store"]["existed_before_spawn"], False)
                    owner.assert_called_once()

    def test_nested_runtime_is_rejected_before_any_native_owner_or_path(self):
        with patch.object(cases, "current_scope", return_value=object()), patch.object(cases, "native_case") as owner, \
                self.assertRaises(RuntimeError):
            cases.run_case(cases.case_specs("mdbx")[0], {}, Path("/must-not-exist"),
                           wait_json=None, command_attempt=None, start_destination=None, run_relay_dial=None)
        owner.assert_not_called()

    def test_store_hook_rejects_reuse_and_duplicate_forge_spawn_before_execution(self):
        for repeated in (False, True):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory).resolve()
                spec = cases.case_specs("mdbx")[0]

                def native(native_spec, binaries, native_root, **kwargs):
                    store = native_root / native_spec.identifier / "destination-store"
                    if repeated:
                        self.assertEqual(kwargs["forge_cli_arguments"]("destination", "autorelay-destination", store),
                                         ("--store-backend", "mdbx"))
                    else:
                        store.mkdir(parents=True)
                    return kwargs["forge_cli_arguments"]("destination", "autorelay-destination", store)

                with self.subTest(repeated=repeated), patch.object(cases, "native_case", side_effect=native), self.assertRaises(ValueError):
                    cases.run_case(spec, {}, root, wait_json=None, command_attempt=None,
                                   start_destination=None, run_relay_dial=None)


if __name__ == "__main__":
    unittest.main()
