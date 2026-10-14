#!/usr/bin/env python3
"""Qualify untouched generated models against Forge's real schema and codec."""

import argparse
import hashlib
import importlib.metadata
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

from jsonschema.exceptions import ValidationError as SchemaValidationError
from pydantic import ValidationError

from validated_models import ValidatedModels, load_json, schema_validator


# Finite numeric lexical qualifications, not accepted boolean coercions.
# Every entry must be exercised; new or changed behavior fails the qualification.
MODEL_DIFFERENCES = {
    ("Int8", "7.0"): (False, "StrictInt rejects integral floating JSON tokens"),
    ("Int8", "7e0"): (False, "StrictInt rejects integral exponent JSON tokens"),
}


def require_coverage(cases):
    names = {case["name"] for case in cases}
    required = {
        f"{signed}{width} {boundary}"
        for signed in ("Int", "UInt") for width in (8, 16, 32, 64)
        for boundary in ("minimum", "maximum", "underflow", "overflow")
    } | {
        "overlap first alternative", "overlap second alternative",
        "integer index with text", "string index with integer",
        "negative index", "out of range index", "variant trailing item",
        "optional missing", "optional null", "optional value", "unknown field",
        "missing required field", "nested records and arrays", "nested wrong pairing",
        "UInt64 above binary64 exact range", "UInt64 nested exact values",
        "HTTP quoted integer", "HTTP missing record field", "HTTP variant trailing item",
        "boolean false index", "boolean true index", "array boolean index", "nested boolean index",
    }
    if required - names:
        raise RuntimeError(f"missing acceptance cases: {sorted(required - names)}")


def equal_json(left, right):
    """JSON numeric equality without Python's bool == int shortcut."""
    if isinstance(left, bool) or isinstance(right, bool):
        return type(left) is type(right) and left == right
    if isinstance(left, dict) and isinstance(right, dict):
        return left.keys() == right.keys() and all(equal_json(left[key], right[key]) for key in left)
    if isinstance(left, list) and isinstance(right, list):
        return len(left) == len(right) and all(equal_json(a, b) for a, b in zip(left, right))
    return left == right


def checked_run(command, log):
    result = subprocess.run(command, text=True, capture_output=True, timeout=120)
    log.write_text(result.stdout + result.stderr, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}); see {log}")


def check_versions():
    requirements = Path(__file__).with_name("requirements.txt")
    versions = {}
    for line in requirements.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        name, expected = line.split("==", 1)
        actual = importlib.metadata.version(name)
        if actual != expected:
            raise RuntimeError(f"{name}: expected {expected}, found {actual}")
        versions[name] = actual
    return versions


def generate_models(document, output, work):
    command = [
        sys.executable, "-m", "datamodel_code_generator",
        "--input", str(document), "--input-file-type", "openapi",
        "--output", str(output), "--output-model-type", "pydantic_v2.BaseModel",
        "--target-python-version", "3.12", "--disable-timestamp",
        "--strict-types", "str", "bytes", "int", "float", "bool",
        "--field-constraints", "--use-annotated", "--use-standard-collections",
        "--use-union-operator",
        "--formatters", "black", "isort",
    ]
    checked_run(command, work / "generation.log")
    # Import the generator's output without any rewriting or monkey patching.
    spec = importlib.util.spec_from_file_location("forge_openapi_generated", output)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module, command


def check_document(document, models):
    if document["x-forge-json-profile"]["canonical"] is not True:
        raise RuntimeError("missing canonical/HTTP compatibility annotation")
    operation = document["paths"]["/parity/fixed"]["post"]
    schema = operation["requestBody"]["content"]["application/json"]["schema"]
    if schema != document["components"]["schemas"]["Record"] or operation.get("parameters", []):
        raise RuntimeError("server-supplied argument changed the client request shape")
    if set(models.Record.model_fields) != {"count", "maybe"}:
        raise RuntimeError("generated request model lost fields or exposed server context")


def check_cases(document, cases, models):
    schemas = document["components"]["schemas"]
    consumer = ValidatedModels(document, models)
    names = set()
    results = []
    failures = []
    exercised = set()
    for case in cases:
        name = case["name"]
        if name in names:
            raise RuntimeError(f"duplicate case: {name}")
        names.add(name)
        schema_name = case["schema"]
        if schema_name not in schemas:
            raise RuntimeError(f"unknown schema for {name}: {schema_name}")
        validator = schema_validator(document, schema_name)
        try:
            value = load_json(case["json"])
            schema_ok = validator.is_valid(value)
        except json.JSONDecodeError:
            value = None
            schema_ok = False
        model = getattr(models, schema_name)
        try:
            raw_parsed = model.model_validate_json(case["json"], strict=True)
            raw_pydantic_ok = True
        except ValidationError:
            raw_parsed = None
            raw_pydantic_ok = False
        try:
            parsed = consumer.validate_json(schema_name, case["json"])
            validated_model_ok = True
        except (ValidationError, SchemaValidationError, json.JSONDecodeError):
            parsed = None
            validated_model_ok = False
        expected = case["expected"]
        expected_codec = case.get("expected_codec", expected)
        if expected_codec != expected and not (case.get("note") and case.get("category") != "canonical"):
            raise RuntimeError(f"undocumented codec/schema difference: {name}")
        expected_model = expected
        difference_key = (schema_name, case["json"])
        if difference_key in MODEL_DIFFERENCES:
            expected_model, reason = MODEL_DIFFERENCES[difference_key]
            exercised.add(difference_key)
        else:
            reason = None
        row = {**case, "schema_ok": schema_ok, "raw_pydantic_ok": raw_pydantic_ok,
               "validated_model_ok": validated_model_ok}
        expectations = {"codec_ok": expected_codec, "schema_ok": expected,
                        "validated_model_ok": expected_model}
        if "expected_http" in case:
            expectations["codec_http_ok"] = case["expected_http"]
        if not all(type(row[key]) is bool and type(wanted) is bool for key, wanted in expectations.items()):
            raise RuntimeError(f"non-boolean verdict: {name}")
        for engine, wanted in expectations.items():
            if row[engine] != wanted:
                failures.append(f"{name}: {engine}={row[engine]}, expected={wanted}")
        row["expectations"] = expectations
        row["documented_difference"] = len(set(expectations.values())) > 1
        row["raw_model_difference"] = raw_pydantic_ok != validated_model_ok
        if raw_pydantic_ok:
            row["raw_model_json"] = raw_parsed.model_dump_json(exclude_unset=True)
        if reason:
            row["model_limitation"] = reason
        if validated_model_ok and expected_codec:
            encoded = json.loads(parsed.model_dump_json(exclude_unset=True))
            if not equal_json(encoded, value):
                failures.append(f"{name}: generated model changed the JSON value")
            row["model_roundtrip"] = encoded
            if "codec_json" in case:
                # Forge omits disengaged optional members; Pydantic emits null
                # by default. Compare decoded states, not those serialization choices.
                try:
                    native_value = consumer.validate_json(schema_name, case["codec_json"])
                except (ValidationError, SchemaValidationError, json.JSONDecodeError):
                    failures.append(f"{name}: native output rejected by validated model")
                else:
                    if not equal_json(parsed.model_dump(mode="json"), native_value.model_dump(mode="json")):
                        failures.append(f"{name}: model/native decoded values differ")
        results.append(row)
    if not results:
        raise RuntimeError("empty native acceptance corpus")
    if MODEL_DIFFERENCES.keys() - exercised:
        failures.append(f"unexercised model qualifications: {sorted(MODEL_DIFFERENCES.keys() - exercised)}")
    return results, failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()
    versions = check_versions()
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    checked_run([str(args.fixture.resolve()), "--output", str(work)], work / "fixture.log")
    document_path = work / "openapi.json"
    document = load_json(document_path.read_text(encoding="utf-8"))
    if document["openapi"] != "3.1.0":
        raise RuntimeError("unexpected OpenAPI dialect")
    cases = load_json((work / "cases.json").read_text(encoding="utf-8"))
    require_coverage(cases)
    generated_path = work / "models.py"
    models, command = generate_models(document_path, generated_path, work)
    check_document(document, models)
    generated_hash = hashlib.sha256(generated_path.read_bytes()).hexdigest()
    results, failures = check_cases(document, cases, models)
    if hashlib.sha256(generated_path.read_bytes()).hexdigest() != generated_hash:
        raise RuntimeError("generated models changed during qualification")
    report = {
        "status": "failed" if failures else "qualified_with_documented_differences",
        "unqualified_language_parity": False,
        "consumer_contract": "strict JSON parse -> JSON Schema 2020-12 -> untouched Pydantic strict JSON",
        "raw_model_verdict_is_observation_only": True,
        "versions": versions, "generation_command": command,
        "generated_sha256": generated_hash, "cases": results, "failures": failures,
    }
    (work / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for failure in failures:
        print(failure, file=sys.stderr)
    differences = sum(case["documented_difference"] for case in results)
    print(f"{len(results)} cases, {differences} documented differences, "
          f"{len(failures)} unexpected results; report: {work / 'report.json'}")
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
