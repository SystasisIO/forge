# forge_schema

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

Dependencies: `forge_exceptions`, `forge_reflect` and Boost.Describe headers.

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
