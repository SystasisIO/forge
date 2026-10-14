# OpenAPI / JSON / Python qualification

This is a focused generator qualification, not a replacement codec or a Python
SDK. The native fixture emits schemas from `forge.api.http.openapi` and runs the
real Forge JSON decoders on the same original JSON strings. The harness invokes
the pinned `datamodel-code-generator` and imports its unmodified Pydantic v2
output. Qualified consumption requires JSON Schema 2020-12 validation of the
original input before Pydantic `strict=True` validation. Generated files are
build artifacts, never hand-maintained source.

## Consumer contract

Use [ValidatedModels](validated_models.py), not a generated model's
`model_validate_json` alone, when relying on this qualification:

```python
from pathlib import Path

import models  # Untouched generated models.py beside the exported openapi.json.
from validated_models import ValidatedModels, load_json

document = load_json(Path("openapi.json").read_text(encoding="utf-8"))
consumer = ValidatedModels(document, models)
record = consumer.validate_json("Record", '{"count":7,"maybe":18446744073709551615}')
# Returns the generated models.Record, with an exact Python integer for maybe.
choice = consumer.validate_json("IndexedChoice", '[1,"text"]')
# consumer.validate_json("IndexedChoice", '[true,"text"]') raises schema ValidationError.
```

The adapter takes a component schema name and the **original JSON text**. It
parses JSON while preserving integer tokens as Python `int` and fractional or
exponent tokens as exact `Decimal`, rejecting duplicate object members and
`NaN`/`Infinity` constants recursively. The existing `jsonschema` validator uses
its supported type-checker extension to recognize integral `Decimal` values as
JSON Schema integers; booleans remain booleans, not integers. All other schema
checks, including nested records/arrays, indexed alternatives, bounds, required
members and extra-member rejection, use the exported schema without fixture
allowlists. Schema validation happens **before** a generated model can normalize
`false` into literal `0` or `true` into literal `1`.

Only schema-valid input reaches `model_validate_json(original_json, strict=True)`.
There is no Python-container reserialization, generated-file rewrite, model
subclass or monkeypatch. Parse failures raise `json.JSONDecodeError`, schema
failures raise `jsonschema.exceptions.ValidationError`, and generated-model
failures raise `pydantic.ValidationError`. Use the matching schema and generated
models from the same export, with the pinned dependencies below.

**The generated models are not fixed.** Direct strict Pydantic validation still
accepts boolean indexed alternatives through `Literal[0]` / `Literal[1]`. The
report preserves that behavior as `raw_pydantic_ok` and `raw_model_json`, separate
from the qualified consumer verdict `validated_model_ok`. Raw acceptance is an
observation, never an accepted-input exception. The only two finite model
qualifications are `Int8` with `7.0` and `7e0`: JSON Schema accepts these
mathematical integers, while generated `StrictInt` rejects their lexical forms.
The adapter does not normalize those tokens to manufacture acceptance.

## Run

Use the supported Forge C++23 toolchain and an existing focused build directory.
Python 3.12 or newer is required; dependencies are isolated and disposable:

```sh
python3.12 -m venv build/openapi-parity/python
build/openapi-parity/python/bin/python -m pip install --no-cache-dir \
  -r tests/api/openapi_python/requirements.txt
cmake -S . -B build/openapi-parity/native \
  -DFORGE_OPENAPI_PARITY_PYTHON="$PWD/build/openapi-parity/python/bin/python"
cmake --build build/openapi-parity/native -j 4 \
  --target test_forge_openapi_wire_baseline test_forge_openapi_parity_fixture
ctest --test-dir build/openapi-parity/native --output-on-failure \
  -R '^test_forge_openapi_(wire_baseline|python_parity|python_harness)$'
```

CMake does not download Python tools or change a global environment. The optional
Python test is registered only when `FORGE_OPENAPI_PARITY_PYTHON` is explicitly
set. Native schema/codec regressions remain available without Python dependencies.

CTest generates fresh exporter/model artifacts before the harness and supplies
their location through `FORGE_OPENAPI_TEST_ARTIFACTS`. Manual harness runs must
also supply this variable; there is no fallback to an ignored historical cache:

```sh
FORGE_OPENAPI_TEST_ARTIFACTS="$PWD/build/openapi-parity/native/openapi-python-parity" \
  build/openapi-parity/python/bin/python -m unittest discover \
  -s tests/api/openapi_python -p 'test_*.py' -v
```

## Evidence boundary

The fixture records canonical schema validation, exact-mode decoding and the
actual HTTP-body-option conversion separately. The approved HTTP binding
contract selects exact described-record conversion for requests and responses;
it does not change global JSON or Raw defaults. This remains a decoder-policy
test, not a live HTTP routing test. Numeric lexical differences remain explicit
rather than being advertised as complete cross-language input equality.

JSON Schema defines `integer` mathematically: `0.0` also satisfies an integer
schema and `const: 0`. Forge exact mode distinguishes integer tokens from float
tokens. Separately, the pinned Pydantic `Literal[0]` accepts `false` and `0.0`
even under strict validation. The schema-first consumer rejects boolean indexes
recursively but preserves mathematical schema equality for integral numeric
spellings. This is not permission to silently coerce client inputs or claim full
language equality. Client validation is not a server security boundary; the
server remains responsible for validation and trusted-field injection.

The corpus preserves raw JSON text, especially uint64 extrema and values above
2^53. Python integers and schema bounds must not pass through a float. Existing
Forge root-int64 writer behavior is characterized separately; a composite-value
roundtrip does not establish that the root writer was repaired.

Forge omits disengaged optional object members, while Pydantic includes null by
default. The harness preserves each original input and separately compares the
decoded model states after reading Forge's canonical output. It does not assert
byte-identical optional-field serialization or hand-edit models to obtain it.

Both numeric lexical qualifications must be exercised. New consumer differences,
changed verdicts, missing coverage and changed decoded values fail the harness.
The report records each original JSON string, independent schema verdict,
raw-model evidence, validated-consumer verdict and native results; it explicitly
sets `unqualified_language_parity` to false. Counts and current codec lexical
behavior come from fresh `cases.json` / `report.json`, not a historical snapshot.

The native fixture aborts on unexpected exact-codec or HTTP-option results.
Python negative controls prove the qualification itself rejects wrong native,
schema and HTTP verdicts, an unlisted model difference, missing cases, duplicate
cases, incorrect decoded values and bypass of the consumer's schema gate.
Generated-model tests cover boolean indexes/payloads at root and nested
record/array positions, exact uint64 values, duplicate keys, non-finite numbers
and fractional numbers that binary64 would round into valid indexes. A green
result means these bounded qualifications passed, not that every compatibility
form became a safe client input or that stock Pydantic changed behavior.

Custom JSON adapters, arbitrary consumer `json_schema_traits`, recursive
credential injection, downstream applications and their Python client generation
are outside this qualification.

## Primary references

- [OpenAPI 3.1](https://spec.openapis.org/oas/v3.1.0)
- [JSON Schema number semantics](https://json-schema.org/draft/2020-12/json-schema-validation#section-6.1)
- [Generator supported types](https://datamodel-code-generator.koxudaxi.dev/supported-data-types/)
- [Pydantic strict mode](https://docs.pydantic.dev/latest/concepts/strict_mode/)
