#!/usr/bin/env python3
"""Protect evidence classification against precision loss and stale executables."""

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

SPEC = importlib.util.spec_from_file_location("native_compare", Path(__file__).with_name("compare.py"))
COMPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COMPARE)
GENERATE_SPEC = importlib.util.spec_from_file_location("native_generate", Path(__file__).with_name("generate_cases.py"))
GENERATE = importlib.util.module_from_spec(GENERATE_SPEC)
GENERATE_SPEC.loader.exec_module(GENERATE)


class EvidenceTests(unittest.TestCase):
    def test_rounded_float_cannot_pass_an_exact_integer_expectation(self):
        expected = {"status": "ok", "type": "int64", "value": "9007199254740993"}
        actual = {"status": "ok", "type": "double", "value": "9007199254740992"}
        self.assertFalse(COMPARE.exact_match(expected, actual)[0])

    def test_nonfinite_float_is_not_an_engine_error(self):
        expected = {"status": "error", "allowed_status": ["evaluation_error"]}
        for value in ("nan", "inf", "-inf"):
            self.assertFalse(COMPARE.exact_match(expected, {"status": "ok", "type": "double", "value": value})[0])

    def test_typecheck_failure_does_not_prove_overflow_detection(self):
        expected = {"status": "error", "allowed_status": ["evaluation_error"]}
        self.assertFalse(COMPARE.exact_match(expected, {"status": "compile_error"})[0])
        self.assertTrue(COMPARE.exact_match(expected, {"status": "evaluation_error"})[0])

    def test_stale_embedded_corpus_fails_before_comparison(self):
        case = {"id": "sample", "group": "test", "scope": "test", "benchmark": False,
                "expected": {"status": "ok", "type": "int64", "value": "1"}}
        output = "meta\tsample\tcel\t" + COMPARE.PINS["cel"] + "\tint64\tstale\n"
        output += "compile\t0\t100\nresult\tok\tnative\tint64\t1\n"
        with self.assertRaisesRegex(ValueError, "compiled corpus"):
            COMPARE.parse_output(output, case, "cel", 1, 1, "current")

    def test_missing_timing_sample_is_an_evidence_error(self):
        case = {"id": "sample", "group": "test", "scope": "test", "benchmark": True,
                "expected": {"status": "ok", "type": "int64", "value": "1"}}
        output = "meta\tsample\tcel\t" + COMPARE.PINS["cel"] + "\tint64\tcurrent\n"
        output += "compile\t0\t100\nresult\tok\tnative\tint64\t1\n"
        with self.assertRaisesRegex(ValueError, "samples"):
            COMPARE.parse_output(output, case, "cel", 1, 1, "current")

    def test_generator_keeps_decimal_inputs_and_binds_original_bytes(self):
        corpus = Path(__file__).with_name("corpus.json")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "cases.inc"
            GENERATE.generate(corpus, output)
            text = output.read_text()
            self.assertIn('"9007199254740993"', text)
            self.assertIn("corpus_sha256", text)
            document = json.loads(corpus.read_text())
            document["cases"].append(document["cases"][0])
            duplicate = Path(directory) / "duplicate.json"
            duplicate.write_text(json.dumps(document))
            with self.assertRaisesRegex(ValueError, "duplicate case"):
                GENERATE.generate(duplicate, output)


if __name__ == "__main__":
    unittest.main()
