"""Negative controls for the qualification harness, without editing generated code."""

import copy
from decimal import Decimal
import json
from types import SimpleNamespace
import unittest

from pydantic import RootModel, StrictBool, StrictInt

import qualify


class Int8(RootModel[StrictInt]):
    pass


class Flag(RootModel[StrictBool]):
    pass


class StrictJSONTests(unittest.TestCase):
    def load_json(self, text):
        self.assertTrue(callable(getattr(qualify, "load_json", None)),
                        "missing lossless strict JSON input parser")
        return qualify.load_json(text)

    def test_integer_tokens_remain_exact_python_integers(self):
        value = self.load_json("[9007199254740993,18446744073709551615]")
        self.assertEqual(value, [9007199254740993, 18446744073709551615])
        self.assertTrue(all(type(item) is int for item in value))

    def test_fractional_tokens_do_not_pass_through_binary64(self):
        value = self.load_json("[9007199254740993.0,1.0000000000000000000001,1e-9999]")
        self.assertEqual(value, [Decimal("9007199254740993.0"),
                                 Decimal("1.0000000000000000000001"), Decimal("1e-9999")])
        self.assertTrue(all(type(item) is Decimal for item in value))

    def test_duplicate_keys_are_rejected_recursively(self):
        for text in ('{"x":false,"x":0}', '[{"x":1,"x":1}]',
                     '{"x":{"y":1,"\\u0079":2}}'):
            with self.subTest(text=text):
                with self.assertRaises(json.JSONDecodeError):
                    self.load_json(text)

    def test_nonfinite_constants_are_rejected_recursively(self):
        for text in ("NaN", "Infinity", "-Infinity", '[{"x":NaN}]'):
            with self.subTest(text=text):
                with self.assertRaises(json.JSONDecodeError):
                    self.load_json(text)

    def test_boolean_schema_values_remain_valid(self):
        consumer = qualify.ValidatedModels(
            {"components": {"schemas": {"Flag": {"type": "boolean"}}}},
            SimpleNamespace(Flag=Flag))
        self.assertIs(consumer.validate_json("Flag", "true").root, True)
        self.assertIs(consumer.validate_json("Flag", "false").root, False)


class HarnessTests(unittest.TestCase):
    def setUp(self):
        self.document = {"components": {"schemas": {"Int8": {"type": "integer"}}}}
        self.models = SimpleNamespace(Int8=Int8)
        self.case = {"name": "integer", "schema": "Int8", "json": "7",
                     "expected": True, "codec_ok": True}

    def test_matching_case_is_recorded(self):
        rows, failures = qualify.check_cases(self.document, [self.case], self.models)
        self.assertEqual(rows[0]["model_roundtrip"], 7)
        # A truncated corpus must fail, even when its only case is valid.
        self.assertTrue(any("unexercised" in failure for failure in failures))

    def test_raw_and_validated_model_evidence_are_separate(self):
        rows, _ = qualify.check_cases(self.document, [self.case], self.models)
        self.assertIn("raw_pydantic_ok", rows[0])
        self.assertIn("validated_model_ok", rows[0])
        self.assertIs(rows[0]["raw_pydantic_ok"], True)
        self.assertIs(rows[0]["validated_model_ok"], True)
        self.assertNotIn("raw_pydantic_ok", rows[0]["expectations"])
        self.assertIs(rows[0]["expectations"]["validated_model_ok"], True)

    def test_only_two_numeric_lexical_qualifications_remain(self):
        self.assertEqual(set(qualify.MODEL_DIFFERENCES), {
            ("Int8", "7.0"), ("Int8", "7e0"),
        })

    def test_wrong_native_result_fails(self):
        self.case["codec_ok"] = False
        _, failures = qualify.check_cases(self.document, [self.case], self.models)
        self.assertTrue(any("codec_ok=False" in failure for failure in failures))

    def test_wrong_schema_result_fails(self):
        self.document["components"]["schemas"]["Int8"]["maximum"] = 6
        _, failures = qualify.check_cases(self.document, [self.case], self.models)
        self.assertTrue(any("schema_ok=False" in failure for failure in failures))

    def test_new_model_difference_is_not_automatically_allowed(self):
        self.case.update(json="8.0", expected=True, expected_codec=False,
                         codec_ok=False, category="integer_lexical", note="Token distinction")
        _, failures = qualify.check_cases(self.document, [self.case], self.models)
        self.assertTrue(any("validated_model_ok=False" in failure for failure in failures))

    def test_missing_note_fails(self):
        self.case.update(expected_codec=False, codec_ok=False)
        with self.assertRaisesRegex(RuntimeError, "undocumented"):
            qualify.check_cases(self.document, [self.case], self.models)

    def test_non_boolean_verdict_fails(self):
        self.case["codec_ok"] = 1
        with self.assertRaisesRegex(RuntimeError, "non-boolean"):
            qualify.check_cases(self.document, [self.case], self.models)

    def test_duplicate_case_fails(self):
        with self.assertRaisesRegex(RuntimeError, "duplicate"):
            qualify.check_cases(self.document, [self.case, copy.deepcopy(self.case)], self.models)

    def test_missing_coverage_fails(self):
        with self.assertRaisesRegex(RuntimeError, "missing acceptance"):
            qualify.require_coverage([self.case])

    def test_wrong_http_verdict_fails(self):
        self.case.update(expected_http=True, codec_http_ok=False)
        _, failures = qualify.check_cases(self.document, [self.case], self.models)
        self.assertTrue(any("codec_http_ok=False" in failure for failure in failures))

    def test_json_equality_preserves_bool_and_exact_uint64(self):
        self.assertFalse(qualify.equal_json([False], [0]))
        self.assertTrue(qualify.equal_json([0.0], [0]))
        self.assertFalse(qualify.equal_json(18446744073709551615, float(18446744073709551615)))

    def test_wrong_decoded_native_value_fails(self):
        self.case["codec_json"] = "8"
        _, failures = qualify.check_cases(self.document, [self.case], self.models)
        self.assertTrue(any("decoded values differ" in failure for failure in failures))

    def test_invalid_native_output_is_recorded_as_failure(self):
        self.case["codec_json"] = "true"
        _, failures = qualify.check_cases(self.document, [self.case], self.models)
        self.assertTrue(any("native output rejected" in failure for failure in failures))


if __name__ == "__main__":
    unittest.main()
