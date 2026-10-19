"""Consume actual Forge export with the existing qualification donor's tooling.

No local schema reconstruction, generator, generated-model patch or monkeypatch.
The shared donor provides exact JSON loading, equality and version checks.
"""

import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

from jsonschema import Draft202012Validator
from jsonschema.exceptions import ValidationError as SchemaValidationError
from pydantic import BaseModel, ValidationError
from referencing import Registry

from profile_checks import require

DONOR = Path(__file__).resolve().parents[1] / "api" / "openapi_python"
sys.path.insert(0, str(DONOR))
from qualify import check_versions, equal_json  # noqa: E402
from validated_models import load_json  # noqa: E402


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def offline_schema(value):
    if isinstance(value, dict):
        for key, item in value.items():
            if key in ("$ref", "$dynamicRef"):
                require(type(item) is str and item.startswith("#/"))
            offline_schema(item)
    elif isinstance(value, list):
        for item in value:
            offline_schema(item)


def no_remote(_uri):
    raise ValueError("remote schema lookup forbidden")


class ExportedModels:
    def __init__(self, root, generate=False):
        self.source = root / "document.schema.json"
        require(self.source.stat().st_size <= 1024 * 1024)
        schema = load_json(self.source.read_text(encoding="utf-8"))
        offline_schema(schema)
        Draft202012Validator.check_schema(schema)
        self.validator = Draft202012Validator(schema, registry=Registry(retrieve=no_remote))
        self.output = root / "python" / "models.py"
        if generate:
            self.versions = check_versions()
            # Same pinned tool/strictness/formatters as openapi_python/qualify.py;
            # only the input dialect and root name differ for a single schema.
            command = [sys.executable, "-m", "datamodel_code_generator", "--input", str(self.source),
                       "--input-file-type", "jsonschema", "--class-name", "Attestation",
                       "--output", str(self.output), "--output-model-type", "pydantic_v2.BaseModel",
                       "--target-python-version", "3.12", "--disable-timestamp",
                       "--strict-types", "str", "bytes", "int", "float", "bool",
                       "--field-constraints", "--use-annotated", "--use-standard-collections",
                       "--use-union-operator", "--formatters", "black", "isort"]
            result = subprocess.run(command, capture_output=True, timeout=120, check=False)
            require(result.returncode == 0)
        else:
            record = json.loads((root / "python" / "schema_evidence.json").read_text())
            require(record["status"] == "PASS")
            require(record["schema_sha256"] == sha(self.source) and record["generated_sha256"] == sha(self.output))
        self.generated_hash = sha(self.output)
        spec = importlib.util.spec_from_file_location("forge_attestation_generated", self.output)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        require(sha(self.output) == self.generated_hash)
        self.regex_engines = {
            name: model.model_config.get("regex_engine", "pydantic-default")
            for name, model in vars(module).items()
            if isinstance(model, type) and issubclass(model, BaseModel) and model.__module__ == module.__name__
        }
        require(self.regex_engines and all(engine in {"python-re", "rust-regex", "pydantic-default"}
                                          for engine in self.regex_engines.values()))
        self.model = module.Attestation

    def validate_json(self, raw):
        value = load_json(raw.decode("utf-8"))
        self.validator.validate(value)
        parsed = self.model.model_validate_json(raw, strict=True)
        require(equal_json(load_json(parsed.model_dump_json(exclude_unset=True, by_alias=True)), value))
        require(sha(self.output) == self.generated_hash)
        return value

    def qualify(self, root, cases):
        rows = []
        alias_regression = None
        structural = {"unknown-product-field", "unknown-range-field", "unknown-att-field", "claims-string",
                      "ranges-object", "range-null", "object-null", "object-empty", "note-number",
                      "wrong-version", "bool-version", "product-object-129", "product-note-129", "ranges-9",
                      "product-note-empty", "ranges-empty", "issuer-257", "audience-257", "purpose-65",
                      "schema-257", "jti-short", "jti-long"}
        for case in cases:
            name = case["id"]
            unicode_length = name.startswith("product-unicode-")
            selected = (case["accept"] or name in structural or name.startswith("decimal-")
                        or unicode_length
                        or name.startswith("forbidden-claim-")
                        or (name.startswith("date-") and name.rsplit("-", 1)[-1] in {"0", "1", "2", "4", "5"})
                        or (name.startswith("missing-") and not name.startswith("missing-header-")))
            if not selected:
                continue
            raw = base64.urlsafe_b64decode(case["payload"] + "=" * (-len(case["payload"]) % 4))
            value = load_json(raw.decode("utf-8"))
            schema_ok = self.validator.is_valid(value)
            try:
                parsed = self.model.model_validate_json(raw, strict=True)
                model_ok = True
            except ValidationError:
                model_ok = False
            try:
                self.validate_json(raw)
                qualified_ok = True
            except (SchemaValidationError, ValidationError, ValueError):
                qualified_ok = False
            # Schema alone must preserve required/unknown/nested/decimal bounds;
            # a stricter manual profile must not hide a broken native exporter.
            matches = (schema_ok is case["accept"] and qualified_ok is case["accept"]
                       and (not unicode_length or model_ok is case["accept"]))
            if name == "valid":
                aliased = load_json(parsed.model_dump_json(exclude_unset=True, by_alias=True)) if model_ok else {}
                by_name = load_json(parsed.model_dump_json(exclude_unset=True, by_alias=False)) if model_ok else {}
                att = aliased.get("att", {})
                alias_regression = {
                    "by_alias_roundtrip": equal_json(aliased, value),
                    "by_name_roundtrip": equal_json(by_name, value),
                    "physical_att_schema": type(att) is dict and "schema" in att and "schema_" not in att,
                }
                matches = matches and alias_regression["by_alias_roundtrip"] and alias_regression["physical_att_schema"]
            row = {"id": name, "expected_accept": case["accept"], "schema_ok": schema_ok,
                   "raw_model_ok": model_ok, "qualified_model_ok": qualified_ok,
                   "raw_model_parity_required": unicode_length,
                   "status": "PASS" if matches else "FAILED"}
            if name == "valid":
                row["alias_roundtrip"] = alias_regression
            rows.append(row)
        require(rows and any(row["id"] == "decimal-length-0" for row in rows))
        require(alias_regression is not None)
        required_unicode = {
            f"product-unicode-{field}-{label}-{count}{suffix}"
            for field in ("object", "note")
            for label in ("bmp", "supplementary")
            for count in (1, 65, 128, 129)
            for suffix in (("", "-escaped") if label == "supplementary" and count in (128, 129) else ("",))
        }
        require(required_unicode <= {row["id"] for row in rows})
        passed = all(row["status"] == "PASS" for row in rows)
        report = {"status": "PASS" if passed else "FAILED", "scope": "actual-export structural/model fixture; not full J4",
                  "schema_sha256": sha(self.source), "generated_sha256": self.generated_hash,
                  "generated_import": "PASS", "regex_engines": self.regex_engines,
                  "regex_policy": "pinned generator automatic lookaround detection; no override",
                  "versions": self.versions, "rows": rows,
                  "donor_hashes": {name: sha(DONOR / name) for name in ("qualify.py", "validated_models.py", "requirements.txt")}}
        (root / "python" / "schema_evidence.json").write_text(json.dumps(report, indent=2) + "\n")
        require(passed)
