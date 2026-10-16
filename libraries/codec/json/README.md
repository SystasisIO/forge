# forge_codec_json

## Opaque shared JSON values

`forge.codec.json.document` is a guest-safe owned `document { std::string text; }`.
Link `Forge::forge_codec_json_values` when a shared protocol must carry arbitrary
JSON without importing a parser, Variant, or native runtime into WASM. Raw encodes
the document exactly as one string. Guest code treats the bytes as opaque; they
do not prove that the text is valid JSON.

Native code imports `forge.codec.json` and uses `parse_document(text, limits)` or
`document_value(value, limits)`. These functions use the existing JSON backend,
reject malformed/duplicate/deep/oversized input, and enforce explicit byte/depth
limits. Native `to_variant/from_variant` adapters preserve the JSON value inside
described DTOs: an object remains an object, rather than a quoted JSON string.
Default document limits are 1 MiB and depth 128. Do not log document contents.

The native document adapter also implements the explicit
`schema::value_adapter<T>` hook, so schema-bound DTOs preserve nested JSON values
and required null values. Schema owns only the validated value conversion
boundary; it imports no JSON parser. Dynamic values are not exposed as raw CLI
or environment strings.

`forge_codec_json` is the JSON codec boundary. The public API is `namespace forge::codec::json`;
The parser backend is an internal implementation detail and never leaks into
module interfaces.

## When To Use

- Read/write generic `forge::variant` JSON values.
- Read/write `forge::config::core::document` for configuration.
- Decode typed Boost.Describe objects through `forge_schema` and get diagnostics.
- Strictly decode protocol and persisted records with recursive field and variant checks.

## When Not To Use

- Do not include or expose backend parser types in public FORGE or application APIs.
- Do not use JSON for binary contract compatibility; use `forge_raw`.
- Do not rely on JSON serialization as redaction. Redact before calling write.

## Public Module

- `forge.codec.json`

Target: `forge_codec_json`.

Dependencies: `forge_config_core`, `forge_schema`, `forge_variant`; the backend parser is
private.

## Examples

### Generic Value Roundtrip

```cpp
import forge.codec.json;
import forge.variant.exceptions;
import forge.variant.value;
import forge.variant.conversion;
import forge.variant.containers;
import forge.variant.chrono;
import forge.variant.multiprecision;
import forge.variant.format;
import forge.variant.described;

auto parsed = forge::codec::json::read_value(R"({"name":"node-a","enabled":true})");
if (!parsed.ok()) {
   report_diagnostics(parsed.diagnostics);
} else {
   const auto& value = parsed.value;
   auto name = value.get_object()["name"].get_string();
   auto out = forge::codec::json::write_value(value, {.pretty = true});
}
```

### Config Document Roundtrip

```cpp
import forge.config.core.key_path;
import forge.config.core.value;
import forge.config.core.document;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.migration;
import forge.codec.json;

auto document = forge::config::core::document{};
document.set("http.bind-host", "127.0.0.1");
document.set("http.bind-port", 8080);

auto written = forge::codec::json::write_document(document);
if (!written.ok()) {
   report_diagnostics(written.diagnostics);
} else {
   auto parsed = forge::codec::json::read_document(written.text);
   if (!parsed.ok()) {
      report_diagnostics(parsed.diagnostics);
   }
}
```

### Typed Decode With Unknown Field Policy

```cpp
import forge.codec.json;

auto options = forge::codec::json::read_options{};
options.unknown_fields = forge::codec::json::unknown_field_policy::error;

auto parsed = forge::codec::json::read<http_config>(
   R"({"bind-port":9090,"extra":1})",
   options);
if (!parsed.ok()) {
   report_diagnostics(parsed.diagnostics);
}
```

### Exact Described Records

Use `described_record_policy::exact` at protocol and persisted-data boundaries. Every
non-optional Boost.Describe member must be present, unknown members are rejected, and
nested sequences and `std::variant` values are validated before conversion. Ordinary
variants use the canonical `[index, payload]` representation; types with an owning
canonical scalar adapter, such as encoded public keys and signatures, retain that
adapter's JSON form. Schema-bound records use their declared field names and aliases.
Associative containers use arrays of exact `[key, value]` entries.
Typed `write<T>` and `save<T>` emit the schema's canonical field names, so their output
round-trips through exact reads even when C++ member names differ. Dotted schema names
use nested JSON objects consistently on both paths.

```cpp
import forge.codec.json;
import forge.chain.protocol.producer_authority;

auto options = forge::codec::json::read_options{
   .described_records = forge::codec::json::described_record_policy::exact,
};

auto loaded = forge::codec::json::load<forge::chain::protocol::producer_authority_schedule>(
   "producer-schedule.json", options);
if (!loaded.ok()) {
   report_diagnostics(loaded.diagnostics);
}
```

Diagnostics preserve the recursive path, for example `policies[1].authority[1].threshold`.
Missing `std::optional` members remain valid and decode as empty optionals.

### Decode An Owned JSON Value

`decode<T>(variant, read_options)` converts a JSON value that has already been
parsed. It shares typed `read<T>`/`load<T>` conversion, exact validation, schema
names/aliases, defaults, custom adapters and diagnostics. This supports a bounded
metadata transformation between parsing and typed conversion without writing and
parsing JSON again; integer and floating-point value kinds stay distinct.

```cpp
auto parsed = forge::codec::json::read_value(text, options);
if (parsed.ok()) {
   auto typed = forge::codec::json::decode<request>(std::move(parsed.value), options);
}
```

The input is owned because schema materialization may modify it after exact
validation. The caller must supply an already parsed, depth-bounded JSON value,
normally from `read_value`. Syntax, duplicate object keys and parser depth limits
are enforced during parsing; `decode` does not reapply `read_options::max_depth`
to an arbitrary caller-constructed Variant. Exact record validation still checks
schema aliases, missing/unknown fields, numeric ranges and indexed variants.

### File Helpers

```cpp
import forge.codec.json;

auto result = forge::codec::json::load_document("config.json");
if (!result.ok()) {
   report_diagnostics(result.diagnostics);
} else {
   auto saved = forge::codec::json::save_document("effective.json", result.value, {.pretty = true});
   if (!saved.ok()) {
      report_diagnostics(saved.diagnostics);
   }
}
```

## Diagnostics

Parser, type and schema errors are mapped into `std::vector<forge::schema::diagnostic>`.
Backend parser errors are normalized at the FORGE boundary. Application code should
print the FORGE diagnostic path, code and message instead of exposing parser
implementation details:

```cpp
#include <iostream>

import forge.codec.json;

auto parsed = forge::codec::json::read_value("{invalid json");
for (const auto& diagnostic : parsed.diagnostics) {
   std::cerr << diagnostic.path << " [" << diagnostic.code << "] "
             << diagnostic.message << "\n";
}
```

## Risks And Anti-Patterns

- Do not sign, hash or authorize JSON text. Formatting, field order and number
  rendering are not a binary protocol contract.
- Do not keep running after `read_*` or `write_*` returns error diagnostics.
  Surface diagnostics and fail before configuring runtime components.
- Do not persist redacted JSON as real config; placeholders would replace
  secrets on the next startup.

## Typical Mistakes

- Do not use the removed legacy JSON facade API.
- Do not assume all numbers are safe as `double`; large integer behavior is
  tested explicitly.
- Do not write secret-bearing config without `forge::config::core::redact`.

## Tests

`test_forge_codec_json` covers generic values, large integers, config documents, typed
schema reads, exact described-record read/write/load/save roundtrips, recursive
diagnostics, malformed input diagnostics and no public backend leakage.
