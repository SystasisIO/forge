# forge_schema

## Validated value adapters

A format or value owner may specialize `forge::schema::value_adapter<T>` with
`kind`, `static input_value encode(const T&)`, and `static T decode(const input_value&)`.
The adapter must validate its value and enforce its own bounds. Schema uses the
same adapter for typed encoding, decoding and exact-input validation without
importing the format backend. `value_kind::dynamic` supports an arbitrary owned
value, including a required null; dynamic fields are excluded from raw CLI and
environment string ingestion. The native JSON document adapter is one consumer;
its guest value module depends on no Schema or parser runtime.

`forge_schema` describes typed configuration and codec rules for Boost.Describe
objects. It is intentionally small: metadata, defaults, basic validation and
diagnostics. It is not a full business validation framework.

## When To Use

- A typed config struct needs defaults, required fields, aliases, ranges,
  nested object-list rules or secret/deprecated metadata.
- JSON/YAML/CLI decoding must return path-aware diagnostics instead of throwing
  generic parser errors.
- A library wants to publish its config contract without depending on a parser.

## When Not To Use

- Do not put runtime state validation here.
- Do not perform security/authority checks here.
- Do not use schema metadata as a persistence migration system; migrations are
  a separate layer.

## Public Modules

- `forge.schema.diagnostic` — `severity`, `diagnostic`.
- `forge.schema.exceptions` — typed failures for invalid values that cannot be
  represented as ordinary decode diagnostics.
- `forge.schema.value_kind` — supported scalar/list kind metadata.
- `forge.schema.object` — `rules<T>`, `object<T>()`, field builders.
- `forge.schema.enums` — described enum conversion helpers.

Target: `forge_schema`.

Dependencies: `forge_exceptions`, `forge_reflect` and Boost.Describe headers;
private native backend `Boost::regex` (Boost 1.90).

## Bounded Native Constraint Artifacts

`forge.schema.input_value` owns the **existing** recursive structural input
record extracted unchanged from `forge.schema.object`. There is one physical
declaration. It remains the schema/config structural interchange, not a second
JSON value runtime or codec. `object` re-exports it through `constraint`.
This extraction lets data-only constraints depend on structural input without
an `object -> constraint -> object` cycle. Existing schema callbacks consume
this input already; replacing it with a visitor would change the established
config/codec contract rather than eliminate a duplicate. Native Variant
adaptation remains entirely in `forge.variant.schema`; Schema imports no
Variant, JSON parser, product DTO, network or runtime.

`forge.schema.constraint` defines `constraint`, `artifact` (version 1), and
`constraint_limits`. `export_constraints<T>()` in `forge.schema.object` reads
the existing `rules<T>` and Boost.Describe metadata, not a second field registry.
New field refinements are `.length(min,max)`, `.ascii()`, `.pattern(text)`,
`.constant(input_value)`, `.decimal_range(uint64_min,uint64_max)`, and
`.shape(constraint)` for a concrete structural shape, notably dynamic objects.
Existing `.range`, `.non_empty`, `.min_items`, `.max_items`, and `.each_non_empty`
also export their constraints. A non-optional field is required in exact mode;
optional fields may be absent or null. String lengths count Unicode scalars;
with `.ascii()` they are also UTF-8 byte lengths. Parser-level byte limits
remain separate. URI semantics and cross-field/business checks are not inferred.

Supported artifact rules: exact JSON kinds; closed object properties/required
names (or explicit `additional_properties`); homogeneous arrays/items; size
bounds; strings/ASCII/anchored ASCII patterns; scalar constants/enums; exact
signed/unsigned integer bounds; canonical decimal-string bounds through
`UINT64_MAX`. Decimal export uses an equivalent digit-interval pattern, never
double arithmetic or a digits-only approximation. Patterns use a documented
anchored, escape-free, whitespace-free ASCII subset, compiled by Boost.Regex ECMAScript with
`no_perl_ex`, `no_mod_m`, `no_mod_s` and the classic locale. Backslash escapes
and POSIX character-class/collation extensions are not exportable. `.pattern()` selects
the ASCII string profile; data-only pattern rules must set `ascii = true`.
Patterns are at most 256 bytes. There is no pattern parser/safety analyzer or
custom matching engine. Exported patterns use a strict end assertion so a final
newline cannot evade a bound, and group alternatives under the start assertion.

Regex execution is private to `constraint.cpp`. Native field rules and local
artifacts use the same helper, with `BOOST_REGEX_MAX_STATE_COUNT=100000`,
`BOOST_REGEX_MAX_BLOCKS=64`, `BOOST_REGEX_BLOCKSIZE=4096`. Boost 1.90's nonrecursive
matcher has an initial block plus at most 64 additional backtracking blocks
(260 KiB); this is a stack budget, not a claim about total allocator usage.
Pattern compilation remains bounded by pattern bytes and Boost's own compile
recursion/complexity checks. A zero-copy bidirectional view selects Boost's
fixed state-count path: its random-access `N*S*S` heuristic is deliberately not
used because it can exceed `MAX_STATE_COUNT`. A TU-local traits type isolates
budget-sensitive template instantiations from unrelated Regex consumers.
Backend complexity/stack/allocation failures throw typed `constraint_limit`;
syntax/export-profile failures throw `unsupported_export`. No timeout substitutes
for engine budgets. The adversarial `^(a+)+$` test must fail via this typed
resource limit in CI, not via pre-rejection by a homemade analyzer.

`json_schema(artifact)` is the single JSON Schema/OpenAPI 3.1 exporter.
`export_artifact(artifact)` emits a data-only `{version,schema}` envelope;
`load_artifact(input_value)` accepts only that local, reference-free subset.
It checks version, closed keywords, kinds, bounds, depth, node, member and string
limits before recursive loading. It does not fetch URIs, resolve `$ref`, execute
code or parse arbitrary JSON Schema. JSON encoding/decoding is exclusively the
existing native codec. `validate(artifact,input_value)` validates a dynamic
document without importing its original C++ type. Default structural limits
are depth 32, 4096 nodes, 16384 bytes/string and 256 entries/container.

Export rejects aliases/dotted paths, arbitrary validators such as `unique_by`,
floating/wide-integer rules and unsupported adapted types with
`schema::exceptions::unsupported_export`. It must not silently drop a rule.
Authority, URI or other product semantic checks remain explicit and are not
advertised as JSON Schema guarantees. These artifacts and the RTTI-based schema
metadata are native only, not part of a guest contract library.

JSON Schema describes parsed numbers; it cannot recover whether a parser
normalized `1e0` to integer `1`. Consumers must retain strict JSON numeric kinds
(for example strict integer validation in generated Pydantic models), in addition
to the exported ranges. Native exact reads reject floating/exponent tokens.

Qualification is covered by `tests/test_schema_attestation.cpp` and
`tests/test_json_strict.cpp`; those files use the shared unit-test main.

## Examples

### Describe Config Rules

```cpp
#include <boost/describe.hpp>

#include <cstdint>
#include <string>

import forge.schema.diagnostic;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;

struct http_config {
   std::uint16_t bind_port = 0;
   std::string bind_host;
   std::string token;
};

BOOST_DESCRIBE_STRUCT(http_config, (), (bind_port, bind_host, token))

template <>
struct forge::schema::rules<http_config> {
   static forge::schema::object_schema<http_config> define() {
      auto schema = forge::schema::object<http_config>();
      schema.field<&http_config::bind_port>("bind-port")
         .alias("port")
         .required()
         .default_value(8080)
         .range(1, 65535)
         .description("Local HTTP listen port");
      schema.field<&http_config::bind_host>("bind-host").default_value("127.0.0.1");
      schema.field<&http_config::token>("token").secret().deprecated("use vault-ref");
      return schema;
   }
};
```

### Apply Defaults And Validate

```cpp
import forge.schema.diagnostic;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;

auto rules = forge::schema::rules<http_config>::define();
auto config = http_config{};
rules.apply_defaults(config);
auto diagnostics = rules.validate(config, "http");
```

### Nested Object Lists

Use nested schemas for structured config fields such as local key entries. The
outer field stays one `object_list` for config registry, CLI and environment
adapters; nested fields still receive typed decode and indexed diagnostics.

```cpp
struct key_config {
   std::string id;
   std::string private_key;
   std::string input_profile = "forge";
   std::vector<std::string> purposes;
};

struct signer_config {
   std::vector<key_config> keys;
};

BOOST_DESCRIBE_STRUCT(key_config, (), (id, private_key, input_profile, purposes))
BOOST_DESCRIBE_STRUCT(signer_config, (), (keys))

template <>
struct forge::schema::rules<key_config> {
   static auto define() {
      auto schema = forge::schema::object<key_config>();
      schema.field<&key_config::id>("id").required().non_empty();
      schema.field<&key_config::private_key>("private-key").required().secret();
      schema.field<&key_config::input_profile>("input-profile").default_value("forge");
      schema.field<&key_config::purposes>("purposes").min_items(1).each_non_empty();
      return schema;
   }
};

template <>
struct forge::schema::rules<signer_config> {
   static auto define() {
      auto schema = forge::schema::object<signer_config>();
      schema.field<&signer_config::keys>("keys")
         .items<key_config>()
         .secret()
         .unique_by<&key_config::id>();
      return schema;
   }
};
```

### Convert Described Enums

```cpp
import forge.schema.diagnostic;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;

auto level = forge::schema::severity::info;
bool ok = forge::schema::enum_from_string("warning", level);
auto name = forge::schema::enum_to_string(forge::schema::severity::error);
```

## Diagnostics Contract

Diagnostics carry:

- `path` — dotted config/value path;
- `code` — stable machine-readable reason;
- `level` — info/warning/error/critical;
- `message` — human-facing explanation.

Scalar conversion and schema materialization failures that cannot be returned
as diagnostics throw `forge::schema::exceptions::invalid_value`. Standard and
Boost exceptions are confined to their implementation boundaries.

## Schema Lifetime

Schema copies share their field storage; moving a schema transfers that
ownership. A live field builder also keeps the shared storage alive. Defaults
and validators continue to observe builder changes through any surviving
schema copy, including after additional fields grow the storage vector.

`fields()` returns a borrowed const reference. Copying a `field_rule` preserves
its independent metadata values, but its schema-dependent `apply_default` and
built-in validators (`non_empty`, `min_items`, `max_items`, `each_non_empty`,
`unique_by`) borrow the owning schema storage. They retain that owner for the
duration of each invocation, without keeping the whole schema alive between
calls. Extracting or copying these callbacks alone is not an ownership handle:
after the last schema/builder owner is destroyed, invoking them throws
`forge::schema::exceptions::invalid_value`, even when the supplied object would
otherwise need no default or validation. Other independent field callbacks,
such as member readers, are not subject to this owner check.

Keep a schema copy or builder alive when retaining schema-dependent callbacks.
This lifetime contract does not make concurrent schema mutation thread-safe.

## Risks And Anti-Patterns

- Do not use schema rules as authorization, live connectivity or credential
  validation. Schema validates local value shape before application checks run.
- Do not hide parser-specific decisions in schema metadata. JSON, YAML, env and
  CLI adapters each own their source diagnostics.
- Do not use nested object-list fields as generated CLI/environment fields.
  Keep the top-level object-list secret or source it from a protected config
  document.
- Do not mark a field `secret()` and then print the raw document. Redaction is an
  explicit config/log/UI step.
- For typed diagnostic output, use `forge::variant_schema::encode_diagnostic`
  from `forge.variant.schema`. It checks `.secret()` before serialization and
  applies canonical names recursively. Ordinary Schema/JSON/YAML encoding keeps
  the original data.

## Encoding Policy Boundary

`object_schema::encode_object_with(input, encoder, base_path)` shares the normal
field/path iteration with consumers that need a different field encoding
policy. The callback receives `field_rule<T>`, the object and the canonical path,
and returns `input_value`. `member_name`, `member_address` and `type` let a typed
reflection consumer match a field without invoking `read_input`.

`has_explicit_rules_v<T>` distinguishes the default absent rules from an explicit
specialization, including an intentionally empty Schema. Diagnostics respect an
empty Schema as an empty object. Diagnostic field matching uses Describe names
or compatible PFR member addresses with type and uniqueness checks; a secret
field is masked before matching. Nonsecret members of an otherwise opaque
Schema-only object receive an unsupported marker. Schema remains independent of
Variant and Crypto and does not implement a second diagnostic serializer.

## Typical Mistakes

- Do not treat `secret()` as encryption. It only marks fields for redaction by
  config/log/UI layers.
- Do not assume `required()` overrides defaults; if a required field also has a
  default, consumers should decide whether defaults satisfy their contract.
- Do not put parser-specific behavior in schema. Parser adapters map their own
  errors into schema diagnostics.

## Tests

`test_forge_schema` and `test_forge_config_core` cover field traversal, defaults, range
validation, nested object-list decode, secret/deprecated metadata, and enum
string/int conversion.
