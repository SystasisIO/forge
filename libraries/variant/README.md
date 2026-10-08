# forge_variant

`forge_variant` provides the FORGE dynamic value model: scalar values, arrays,
objects, blobs, described-type conversion, `static_variant` and dynamic bitsets.
It is the bridge between typed C++ values and generic codec/config/log shapes.

## When To Use

- A layer needs JSON-like value trees without depending on a concrete parser.
- You need `to_variant/from_variant` for described structs, enums or containers.
- You need FC-like `static_variant` behavior for retained wire compatibility.

## When Not To Use

- Do not use `variant` as an internal data model when a typed struct is known.
- Do not put validation or required-field policy here; use `forge_schema`.
- Do not put binary serialization here; use `forge_raw`.

## Public Modules

- `forge.variant.value` — `variant`, `variant_object`, `mutable_variant_object`.
- `forge.variant.conversion` — scalar/string/blob conversions.
- `forge.variant.containers` — STL container conversions.
- `forge.variant.described` — Boost.Describe object/enum mapping.
- `forge.variant.chrono` — std chrono ISO conversion.
- `forge.variant.multiprecision` — Boost multiprecision conversions.
- `forge.variant.format` — display helpers.
- `forge.variant.schema` — recursive schema-aware conversion and safe diagnostic encoding.
- `forge.variant.static_variant` — FC-style static variant.
- `forge.variant.dynamic_bitset`, `forge.variant.variant_dynamic_bitset`.

Target: `forge_variant`.

Static variants use `[index, payload]`. Arithmetic alternatives keep their
dynamic scalar kind, including bool and full signed/unsigned 64-bit integers.
Non-arithmetic alternatives retain their owning namespace `to_variant` adapter.

Dependencies: `forge_chrono`, `forge_core`, `forge_reflect`, `forge_schema`, Boost headers, Boost.MultiIndex and
Boost.Multiprecision.

## Examples

### Build A Value Object

```cpp
import forge.variant.exceptions;
import forge.variant.value;
import forge.variant.conversion;
import forge.variant.containers;
import forge.variant.chrono;
import forge.variant.multiprecision;
import forge.variant.format;
import forge.variant.described;

auto object = forge::mutable_variant_object{};
object("name", "node-a")("enabled", true)("retries", 3);

forge::variant value{object};
auto enabled = value.get_object()["enabled"].as_bool();
```

### Convert A Described Type

```cpp
#include <boost/describe.hpp>

#include <string>

struct profile {
   std::string name;
   bool enabled = false;
};

BOOST_DESCRIBE_STRUCT(profile, (), (name, enabled))

import forge.variant.described;

auto source = profile{.name = "dev", .enabled = true};
auto value = forge::variant{source};
auto restored = value.as<profile>();
```

### Chrono Values Use ISO Text

```cpp
import forge.variant.chrono;

auto now = std::chrono::sys_time<std::chrono::microseconds>{std::chrono::microseconds{1}};
auto value = forge::variant{now};
auto restored = value.as<std::chrono::sys_time<std::chrono::microseconds>>();
```

## Security Notes

`variant` does not know what is secret. Redaction belongs to config/schema/log/UI
layers before rendering or serialization.

## Risks And Anti-Patterns

- Do not use `variant` as the primary model for application config when a typed
  Boost.Describe struct exists.
- Do not rely on dynamic field lookup for protocol compatibility. Raw contracts
  need typed DTOs and stable field order.
- Do not render arbitrary variants to logs before redaction. The value layer has
  no schema metadata.

## Typical Mistakes

- Do not move `variant` into `core`; many upper layers depend on it, but `core`
  must stay lower than dynamic value conversion.
- Do not assume unknown object fields are validation errors here. Schema/config
  decide that policy.
- `dynamic_bitset` is intentionally a `boost::dynamic_bitset<std::uint8_t>`
  alias because the C++ standard library has no equivalent runtime-size bitset
  with the needed block behavior.

## Tests

`tests/variant` and the JSON/YAML codec suites cover described roundtrip,
schema-aware mapping, missing/unknown field behavior, bad enum values, chrono ISO
conversion, `static_variant`, blob compatibility and dynamic bitsets.

## Schema-Aware Values

Import `forge.variant.schema` when a format codec must preserve canonical
`forge_schema` field names inside an otherwise schema-less described value.
`forge::variant_schema::encode` recursively applies schema field mappings, and
`forge::variant_schema::materialize` converts those nested mappings back before
ordinary `from_variant` decoding. JSON and YAML share this mechanism so their
typed write/read paths cannot drift.

## Diagnostic Values

Use `forge::variant_schema::encode_diagnostic(value)` before interpolating or
delivering a typed value to a log or diagnostic sink. Schema fields use canonical
names, including dotted paths; aliases are accepted input names only.
`.secret()` produces `"<redacted>"` before reading or serializing that field.
Nested objects, containers and pointers recurse even when the parent has no
Schema. An explicit empty `rules<T>` specialization produces an empty object,
rather than falling back to all reflected members.

```cpp
#include <boost/describe.hpp>
#include <string>

import forge.schema.object;
import forge.variant.schema;

struct credential_view {
   std::string account;
   std::string token;
};
BOOST_DESCRIBE_STRUCT(credential_view, (), (account, token))

template <> struct forge::schema::rules<credential_view> {
   static auto define() {
      auto rules = forge::schema::object<credential_view>();
      static_cast<void>(rules.field<&credential_view::account>("account"));
      rules.field<&credential_view::token>("access-token").secret();
      return rules;
   }
};

auto diagnostic = forge::variant_schema::encode_diagnostic(credential_view{"operator", "sensitive"});
// {"account":"operator", "access-token":"<redacted>"}
```

Without Schema, diagnostics reuse known scalar/container conversions and owning
namespace ADL `to_variant`, then Describe or compatible Boost.PFR traversal.
Custom conversion keeps precedence when a preliminary member walk finds no
Schema or secret descendants. When it does, typed traversal handles those
descendants before any custom serializer can expose them. See
[Reflect's PFR constraints and opt-out](../reflect/README.md#diagnostic-aggregates).
This preliminary walk can inspect only descendants exposed through Schema,
Describe, compatible PFR or the supported typed containers. An opaque custom
converter's author remains responsible for hidden private children and secret
data: Schema `.secret()` or `diagnostic_is_secret` supplies that metadata contract.

An owning type can export `diagnostic_is_secret(const T&) -> bool` in its own
namespace without depending on Variant. The hook is checked before conversion;
Crypto secret storage and private-key types use it. An ordinary string without
metadata remains ordinary data; names and contents do not guess secrecy.

Opaque types produce `"<unsupported>"`; conversion failures produce the fixed
`"<diagnostic-error>"` marker without exception text or a raw fallback. Pointer
cycles are detected on the current pointer path by address and pointed-to type;
an acyclic shared child still appears in every branch. A depth budget of 64 and
a total traversal budget of `MAX_NUM_ARRAY_ELEMENTS` also apply to the preliminary
privacy walk. Cycles or exhausted budgets produce `"<diagnostic-depth-limit>"`;
budget exhaustion never invokes a custom/raw fallback and stops remaining
container entries. These budgets do not bound work performed inside arbitrary
user-provided ADL callbacks. Known arrays retain the existing element limit.
A Schema-only object without
Describe or compatible PFR retains canonical field names and secret masking,
but its nonsecret fields are explicitly unsupported: its erased Schema member
callbacks cannot supply a typed Variant traversal.

`to_variant(const long double&, variant&)` converts to Variant's existing `double`
storage; diagnostics share this conversion. A platform with wider `long double`
precision can lose that extra precision. Schema config encoding still rejects
`long double`, and Raw serialization is unaffected.

Ordinary `encode`, JSON/YAML, network values and Raw serialization remain
unredacted. Diagnostic output is not a wire or persisted/config replacement.
Once a typed value is converted to a raw `variant`, absent Schema metadata cannot
be reconstructed by diagnostic encoding.
