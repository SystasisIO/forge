"""Closed matrix/commands and bounded observation tests, no native process run."""

from pathlib import Path
import json
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from plugin_pubsub_cases import ACTORS, Case, _append_control, _read, _wait, case_specs, command, probes, run_case


class PluginPubSubCaseTests(unittest.TestCase):
    def test_exact_eight_case_matrix_and_modes(self):
        cases = tuple(spec for backend in ("mdbx", "rocksdb") for spec in case_specs(backend))
        self.assertEqual(len(cases), 8)
        self.assertEqual(len({spec.scenario_id for spec in cases}), 8)
        self.assertEqual({(spec.backend, spec.donor, spec.policy) for spec in cases}, {
            (backend, donor, policy) for backend in ("mdbx", "rocksdb") for donor in ("go", "rust")
            for policy in ("yaml-only", "typed-override")})
        for spec in cases:
            self.assertEqual(spec.transport, "tcp")
            self.assertFalse(spec.dns)
            self.assertEqual(spec.protocol, "/meshsub/1.2.0" if spec.policy == "typed-override" else "/meshsub/1.1.0")
            self.assertEqual(spec.implementation("victim"), "forge")
            self.assertEqual(spec.implementation("replacement"), spec.donor)

    def test_closed_inputs_no_backend_or_version_fallback(self):
        for values in (("memory", "go", "yaml-only"), ("mdbx", "python", "yaml-only"), ("mdbx", "go", "typed")):
            with self.subTest(values=values), self.assertRaises(ValueError):
                Case(*values)
        with self.assertRaises(ValueError):
            case_specs("memory")
        with self.assertRaises(ValueError):
            Case("mdbx", "go", "yaml-only").implementation("offender")

    def test_actual_cli_is_pubsub_live_and_only_forge_receives_plugin_flags(self):
        token, work = "a" * 32, Path("/case")
        for backend in ("mdbx", "rocksdb"):
            for spec in case_specs(backend):
                for actor in ACTORS:
                    argv = command(spec, actor, Path("/binary/" + spec.implementation(actor)), work, token)
                    self.assertEqual(argv[:2], ["/binary/" + spec.implementation(actor), "pubsub-live"])
                    flags = dict(zip(argv[2::2], argv[3::2]))
                    self.assertEqual(flags["--transport"], "tcp")
                    self.assertEqual(flags["--version"], spec.version)
                    self.assertEqual(flags["--actor"], actor)
                    self.assertEqual(flags["--case-token"], token)
                    self.assertEqual(flags["--control-file"], "/case/" + actor + ".control")
                    self.assertNotIn("--scenario", flags)
                    self.assertNotIn("--features", flags)
                    if actor == "victim":
                        self.assertEqual(flags["--store-backend"], backend)
                        self.assertEqual(flags["--pubsub-policy"], spec.policy)
                    else:
                        self.assertNotIn("--store-backend", flags)
                        self.assertNotIn("--pubsub-policy", flags)

    def test_exact_binary_boundary_and_separate_ascii_donor_marker(self):
        for policy, limit in (("yaml-only", 128), ("typed-override", 96)):
            spec, token = Case("mdbx", "go", policy), "b" * 32
            binary, rejected, marker = probes(spec, token)
            self.assertEqual(len(binary), limit)
            self.assertEqual(len(rejected), limit + 1)
            self.assertEqual(rejected[:-1], binary)
            self.assertIn(0, binary)
            self.assertTrue(any(byte > 127 for byte in binary))
            self.assertTrue(binary.startswith(("accept:" + token + ":forge:").encode()))
            self.assertEqual(marker, "accept:" + token + ":donor")
        for token in ("", "a" * 31, "A" * 32, "z" * 32):
            with self.assertRaises(ValueError):
                probes(spec, token)

    def test_actual_control_append_preserves_sample_labels_and_prepare_actors(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory).resolve()
            controls = {actor: [] for actor in ACTORS}
            expected = {actor: [] for actor in ACTORS}
            commands = (
                ("victim", "sample", {"label": "before-donor"}),
                ("victim", "sample", {"label": "after-donor"}),
                ("victim", "prepare_shutdown", {"actor": "victim", "case_token": "a" * 32}),
                ("replacement", "prepare_shutdown", {"actor": "replacement", "case_token": "a" * 32}),
                ("replacement", "quiesce_shutdown", {"actor": "replacement", "prepare_ack_sequence": 7}),
            )
            for process_role, kind, fields in commands:
                sequence = len(expected[process_role]) + 1
                self.assertEqual(_append_control(work, controls, process_role, kind, **fields), sequence)
                expected[process_role].append({"sequence": sequence, "kind": kind, **fields})
                self.assertEqual(controls, expected)
                persisted = [json.loads(line) for line in (work / (process_role + ".control")).read_text().splitlines()]
                self.assertEqual(persisted, expected[process_role])

    def test_strict_reader_rejects_duplicate_nonfinite_and_actor_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory).resolve() / "result.json"
            for text in ('{"events":[],"events":[]}', '{"value":NaN}', '{"value":Infinity}',
                         '{"error":"delivery failed"}', '{"overflow":true}', '[]'):
                path.write_text(text)
                with self.subTest(text=text), self.assertRaises((ValueError, RuntimeError)):
                    _read(path)
            path.write_text('{"error":null,"overflow":false}')
            self.assertEqual(_read(path), {"error": None, "overflow": False})
            alias = path.with_name("alias")
            alias.symlink_to(path)
            with self.assertRaises(ValueError):
                _read(alias)

    def test_wait_has_no_time_only_success_or_wait_after_actor_exit(self):
        owner = SimpleNamespace(process=SimpleNamespace(poll=lambda: None))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory).resolve() / "result.json"
            path.write_text('{"observed":true}')
            with patch("plugin_pubsub_cases.time.monotonic", return_value=1), \
                    patch("plugin_pubsub_cases.time.sleep", side_effect=AssertionError("no success delay")):
                self.assertEqual(_wait(owner, path, lambda raw: raw.get("observed") is True, 2), {"observed": True})
            owner.process.poll = lambda: 1
            with self.assertRaises(RuntimeError):
                _wait(owner, path, lambda raw: True, float("inf"))
            owner.process.poll = lambda: None
            with patch("plugin_pubsub_cases.time.monotonic", return_value=2), self.assertRaises(TimeoutError):
                _wait(owner, path, lambda raw: False, 2)

    def test_requires_existing_runner_owner_before_creating_artifacts(self):
        with patch("plugin_pubsub_cases.current_scope", return_value=None), self.assertRaises(RuntimeError):
            run_case(Case("mdbx", "go", "yaml-only"), {}, Path("/never-created"), command_attempt=None)


if __name__ == "__main__":
    unittest.main()
