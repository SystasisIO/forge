"""Orchestration regressions, not native donor acceptance."""

from dataclasses import replace
import unittest
from unittest.mock import patch

from pubsub_extension_cases import Actors, Case, case_specs, run_case


class ExtensionCasesTests(unittest.TestCase):
    def test_closed_matrix_uses_real_versions_and_one_donor(self):
        specs = case_specs()
        self.assertEqual(len(specs), 36)
        self.assertEqual(len({spec.identifier for spec in specs}), 36)
        self.assertEqual(len({spec.scenario for spec in specs}), 9)
        for spec in specs:
            self.assertEqual(spec.version, "1.2" if spec.extension == "idontwant" else "1.3")
            owners = [spec.implementation(role) for role in ("victim", "offender", "replacement", "sink")]
            self.assertEqual(owners.count("forge"), 3)
            self.assertEqual(spec.implementation("victim"), spec.destination)
            self.assertEqual(spec.implementation("replacement"), spec.source)
            self.assertEqual(spec.mode("victim"), spec.extension)
            self.assertEqual(spec.mode("offender"), "advertisement" if spec.extension == "partial" else spec.extension)

    def test_unknown_matrix_rejected_before_creating_or_starting_anything(self):
        spec = case_specs()[0]
        for changed in (replace(spec, source="rust", destination="go"), replace(spec, extension="__init__"),
                        replace(spec, profile="ws")):
            with self.assertRaises(ValueError):
                Actors(changed, {}, "/not-created", None, None, None)

    def test_execution_alone_never_claims_accepted(self):
        spec = Case("forge", "go", "advertisement", "native_tcp_yamux")
        with patch.object(Actors, "start"), patch.object(Actors, "advertisement"), \
                patch.object(Actors, "cleanup"), patch("pubsub_extension_cases._prepare_all", return_value={}), \
                patch("pubsub_extension_cases._stop_prepared"):
            self.assertEqual(run_case(spec, {}, "/not-created")["status"], "captured")

    def test_failure_stays_failure_and_cleanup_runs(self):
        spec = case_specs()[0]
        with patch.object(Actors, "start", side_effect=RuntimeError("native failure")), \
                patch.object(Actors, "cleanup") as cleanup:
            result = run_case(spec, {}, "/not-created")
            self.assertEqual(result["status"], "HARNESS_ERROR")
            self.assertIn("native failure", result["errors"][0])
            cleanup.assert_called_once()


if __name__ == "__main__":
    unittest.main()
