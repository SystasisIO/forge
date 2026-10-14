"""Strict consumer regression tests using untouched native-generated artifacts."""

import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from jsonschema.exceptions import ValidationError as SchemaValidationError
from pydantic import ValidationError as ModelValidationError

import qualify


class GeneratedConsumerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        configured = os.environ.get("FORGE_OPENAPI_TEST_ARTIFACTS")
        if not configured:
            raise RuntimeError("FORGE_OPENAPI_TEST_ARTIFACTS must name fresh native-exporter "
                               "and model-generator artifacts (openapi.json, cases.json, models.py)")
        artifacts = Path(configured)
        cls.document = json.loads((artifacts / "openapi.json").read_text(encoding="utf-8"))
        cls.corpus = json.loads((artifacts / "cases.json").read_text(encoding="utf-8"))
        cls.generated_path = artifacts / "models.py"
        spec = importlib.util.spec_from_file_location(
            "forge_openapi_strict_test_generated", cls.generated_path)
        cls.models = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = cls.models
        spec.loader.exec_module(cls.models)

    def consumer(self, document=None, models=None):
        self.assertTrue(callable(getattr(qualify, "ValidatedModels", None)),
                        "missing reusable schema-gated generated-model consumer")
        return qualify.ValidatedModels(
            self.document if document is None else document,
            self.models if models is None else models)

    def check_cases(self, additional):
        lexical = {("Int8", "7.0"), ("Int8", "7e0")}
        cases = [case for case in self.corpus
                 if (case["schema"], case["json"]) in lexical]
        return qualify.check_cases(self.document, cases + additional, self.models)

    def test_raw_literal_behavior_is_still_observable(self):
        for schema, text in (
            ("IndexedChoice", "[false,7]"),
            ("IndexedChoice", '[true,"text"]'),
            ("ChoiceList", '[[0,7],[true,"text"]]'),
            ("Nested", '{"child":{"count":7},"choices":[[false,7]]}'),
        ):
            with self.subTest(schema=schema, text=text):
                parsed = getattr(self.models, schema).model_validate_json(text, strict=True)
                self.assertIsNotNone(parsed)

    def test_boolean_indexes_are_rejected_at_root_and_nested_positions(self):
        consumer = self.consumer()
        cases = (
            ("IndexedChoice", "[false,7]"),
            ("IndexedChoice", '[true,"text"]'),
            ("OverlappingChoice", "[false,7]"),
            ("OverlappingChoice", "[true,7]"),
            ("ObjectChoice", '[false,{"count":7}]'),
            ("ObjectChoice", '[true,"text"]'),
            ("OverlappingObjects", '[false,{"count":7}]'),
            ("OverlappingObjects", '[true,{"count":7}]'),
            ("ChoiceList", "[[false,7]]"),
            ("ChoiceList", '[[true,"text"]]'),
            ("ChoiceList", '[[0,7],[true,"text"]]'),
            ("ChoiceList", '[[1,"text"],[false,7]]'),
            ("Nested", '{"child":{"count":7},"choices":[[false,7]]}'),
            ("Nested", '{"child":{"count":7},"choices":[[true,"text"]]}'),
            ("Nested", '{"child":{"count":7},"choices":[[0,7],[true,"text"]]}'),
        )
        for schema, text in cases:
            with self.subTest(schema=schema, text=text):
                with self.assertRaises(SchemaValidationError):
                    consumer.validate_json(schema, text)

    def test_booleans_are_not_integer_payloads(self):
        consumer = self.consumer()
        for schema, text in (
            ("Int8", "false"), ("Int8", "true"),
            ("UInt64", "true"), ("UInt64List", "[0,true]"),
            ("IndexedChoice", "[0,true]"), ("ChoiceList", "[[0,false]]"),
            ("Record", '{"count":true}'), ("Record", '{"count":7,"maybe":false}'),
            ("Nested", '{"child":{"count":false},"choices":[]}'),
            ("Nested", '{"child":{"count":7,"maybe":true},"choices":[]}'),
        ):
            with self.subTest(schema=schema, text=text):
                with self.assertRaises(SchemaValidationError):
                    consumer.validate_json(schema, text)

    def test_validation_uses_schema_not_fixture_names_or_input_allowlists(self):
        document = copy.deepcopy(self.document)
        document["components"]["schemas"]["DifferentName"] = {
            "$ref": "#/components/schemas/IndexedChoice"}
        models = SimpleNamespace(DifferentName=self.models.IndexedChoice)
        consumer = self.consumer(document, models)
        for text in (" [ false , 7 ] ", '[ true, "another string" ]'):
            with self.subTest(text=text):
                with self.assertRaises(SchemaValidationError):
                    consumer.validate_json("DifferentName", text)
        parsed = consumer.validate_json("DifferentName", '[1,"another string"]')
        self.assertEqual(json.loads(parsed.model_dump_json()), [1, "another string"])

    def test_schema_rejection_precedes_lossy_literal_normalization(self):
        consumer = self.consumer()
        raw = self.models.IndexedChoice.model_validate_json("[false,7]", strict=True)
        self.assertEqual(json.loads(raw.model_dump_json()), [0, 7])
        with self.assertRaises(SchemaValidationError):
            consumer.validate_json("IndexedChoice", "[false,7]")

    def test_original_json_is_used_for_strict_model_validation(self):
        consumer = self.consumer()
        parsed = consumer.validate_json("IndexedChoice", "[0,7]")
        self.assertEqual(json.loads(parsed.model_dump_json()), [0, 7])
        for text in ("7.0", "7e0"):
            with self.subTest(text=text):
                with self.assertRaises(ModelValidationError):
                    consumer.validate_json("Int8", text)

    def test_exact_uint64_survives_root_array_and_record_roundtrips(self):
        consumer = self.consumer()
        for schema, text in (
            ("UInt64", "9007199254740993"),
            ("UInt64", "18446744073709551615"),
            ("UInt64List", "[9007199254740993,18446744073709551615]"),
            ("Record", '{"count":7,"maybe":18446744073709551615}'),
            ("Nested", '{"child":{"count":7,"maybe":9007199254740993},"choices":[[0,7]]}'),
        ):
            with self.subTest(schema=schema, text=text):
                parsed = consumer.validate_json(schema, text)
                encoded = json.loads(parsed.model_dump_json(exclude_unset=True))
                self.assertTrue(qualify.equal_json(encoded, json.loads(text)))
                if schema == "UInt64":
                    self.assertIs(type(encoded), int)

    def test_schema_constraints_are_enforced_without_coercion(self):
        consumer = self.consumer()
        for schema, text in (
            ("UInt64", "18446744073709551616"),
            ("UInt64List", "[18446744073709551616]"),
            ("Record", '{"count":7,"maybe":18446744073709551616}'),
            ("Record", "{}"), ("Record", '{"count":7,"unknown":0}'),
            ("IndexedChoice", "[0,7,8]"), ("IndexedChoice", '[0,"text"]'),
            ("Int8", '"7"'),
        ):
            with self.subTest(schema=schema, text=text):
                with self.assertRaises(SchemaValidationError):
                    consumer.validate_json(schema, text)

    def test_invalid_json_is_rejected(self):
        consumer = self.consumer()
        with self.assertRaises(json.JSONDecodeError):
            consumer.validate_json("IndexedChoice", "[0,")

    def test_duplicate_record_members_are_rejected_before_model_validation(self):
        consumer = self.consumer()
        for schema, text in (
            ("Record", '{"count":true,"count":7}'),
            ("Record", '{"count":7,"count":7}'),
            ("Nested", '{"child":{"count":false,"count":7},"choices":[]}'),
        ):
            with self.subTest(schema=schema, text=text):
                with self.assertRaises(json.JSONDecodeError):
                    consumer.validate_json(schema, text)

    def test_nonfinite_numbers_are_rejected_before_model_validation(self):
        consumer = self.consumer()
        for schema, text in (
            ("UInt64", "NaN"), ("UInt64", "Infinity"), ("UInt64", "-Infinity"),
            ("Nested", '{"child":{"count":7,"maybe":NaN},"choices":[]}'),
        ):
            with self.subTest(schema=schema, text=text):
                with self.assertRaises(json.JSONDecodeError):
                    consumer.validate_json(schema, text)

    def test_fractional_index_is_not_rounded_into_valid_literal(self):
        consumer = self.consumer()
        parsed = consumer.validate_json("IndexedChoice", "[0e-9999,7]")
        self.assertEqual(json.loads(parsed.model_dump_json()), [0, 7])
        for text in ("[1e-9999,7]", '[1.0000000000000000000001,"text"]'):
            with self.subTest(text=text):
                with self.assertRaises(SchemaValidationError):
                    consumer.validate_json("IndexedChoice", text)

    def test_harness_distinguishes_raw_acceptance_from_validated_rejection(self):
        cases = [
            {"name": "strict root false", "schema": "IndexedChoice", "json": "[false,7]",
             "expected": False, "codec_ok": False},
            {"name": "strict nested true", "schema": "Nested",
             "json": '{"child":{"count":7},"choices":[[true,"text"]]}',
             "expected": False, "codec_ok": False},
        ]
        rows, failures = self.check_cases(cases)
        self.assertEqual(failures, [])
        for row in rows[-len(cases):]:
            with self.subTest(name=row["name"]):
                self.assertIn("raw_pydantic_ok", row)
                self.assertIn("validated_model_ok", row)
                self.assertIs(row["raw_pydantic_ok"], True)
                self.assertIs(row["validated_model_ok"], False)
                self.assertIs(row["schema_ok"], False)
                self.assertIs(row["expectations"]["validated_model_ok"], False)
                self.assertNotIn("raw_pydantic_ok", row["expectations"])
                self.assertNotIn("model_roundtrip", row)
                self.assertNotIn("model_limitation", row)

    def test_harness_fails_if_consumer_schema_gate_is_bypassed(self):
        self.consumer()
        models = self.models

        class UngatedConsumer:
            def validate_json(self, schema_name, text):
                return getattr(models, schema_name).model_validate_json(text, strict=True)

        # Fault injection replaces the consumer boundary, never generated models.
        with patch.object(qualify, "ValidatedModels", return_value=UngatedConsumer()):
            _, failures = self.check_cases([
                {"name": "bypassed false", "schema": "IndexedChoice", "json": "[false,7]",
                 "expected": False, "codec_ok": False},
            ])
        self.assertTrue(any("validated_model_ok=True, expected=False" in failure
                            for failure in failures))

    def test_full_native_corpus_qualifies_without_bool_exceptions(self):
        self.consumer()
        rows, failures = qualify.check_cases(self.document, self.corpus, self.models)
        self.assertEqual(failures, [])
        self.assertTrue(all("raw_pydantic_ok" in row and "validated_model_ok" in row
                            for row in rows))

    def test_root_and_recursive_boolean_coverage_cannot_be_removed(self):
        for name in ("boolean false index", "boolean true index",
                     "array boolean index", "nested boolean index"):
            with self.subTest(name=name):
                trimmed = [case for case in self.corpus if case["name"] != name]
                self.assertLess(len(trimmed), len(self.corpus), f"missing corpus case: {name}")
                with self.assertRaisesRegex(RuntimeError, name):
                    qualify.require_coverage(trimmed)

    def test_consumer_does_not_edit_generated_models(self):
        before = hashlib.sha256(self.generated_path.read_bytes()).hexdigest()
        consumer = self.consumer()
        consumer.validate_json("UInt64", "18446744073709551615")
        with self.assertRaises(SchemaValidationError):
            consumer.validate_json("IndexedChoice", "[false,7]")
        self.assertEqual(hashlib.sha256(self.generated_path.read_bytes()).hexdigest(), before)
        self.assertIsNotNone(self.models.IndexedChoice.model_validate_json("[false,7]", strict=True))


class ArtifactConfigurationTests(unittest.TestCase):
    def test_generated_tests_require_explicit_fresh_artifact_directory(self):
        with patch.dict(os.environ, {"FORGE_OPENAPI_TEST_ARTIFACTS": ""}):
            with self.assertRaisesRegex(RuntimeError, "FORGE_OPENAPI_TEST_ARTIFACTS"):
                GeneratedConsumerTests.setUpClass()


if __name__ == "__main__":
    unittest.main()
