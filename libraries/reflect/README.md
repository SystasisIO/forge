# forge_reflect

`forge_reflect` is a thin Boost.Describe and diagnostic Boost.PFR utility layer. It centralizes described
type detection, member traversal and enum conversion so `raw`, `variant`,
`schema` and other libraries do not each write their own reflection boilerplate.

## When To Use

- You need to iterate Boost.Describe members in stable order.
- You need base-first traversal for described derived types.
- You need enum name/int conversion for diagnostics or codecs.
- You need named traversal of a compatible, undescribed aggregate for diagnostics.

## When Not To Use

- Do not put `to_variant/from_variant` here. Described value mapping belongs to
  `forge_variant`.
- Do not put validation rules here. Validation metadata belongs to `forge_schema`.
- Do not put application schema or config defaults here.

## Public Modules

- `forge.reflect.reflect`

Target: `forge_reflect`.

Dependencies: `forge_core`, Boost.Describe and Boost.PFR headers. `forge_reflect` must not link
or import `forge_variant`.

## Examples

### Describe A Struct Once

```cpp
#include <boost/describe.hpp>

#include <cstdint>
#include <string>

struct endpoint_config {
   std::string host;
   std::uint16_t port = 0;
};

BOOST_DESCRIBE_STRUCT(endpoint_config, (), (host, port))
```

The same member order is then consumed by `forge_raw`, `forge_variant` and
`forge_schema` helpers.

### Convert Described Enum Names

```cpp
#include <boost/describe.hpp>

enum class mode { active, passive };
BOOST_DESCRIBE_ENUM(mode, active, passive)

import forge.reflect.reflect;

auto text = forge::reflect::enum_to_string(mode::active);
auto parsed = mode{};
auto ok = forge::reflect::enum_from_string("passive", parsed);
```

## Compatibility Rule

For types that replace old `FC_REFLECT(TYPE, (a)(b)(c))`, the new
`BOOST_DESCRIBE_*` member list must keep the same order. `forge_raw` uses that
order for byte-compatible packing.

## Diagnostic Aggregates

`is_diagnostic_aggregate_v<T>` admits aggregate classes with standard layout,
excludes unions, and respects Boost.PFR's `is_reflectable` customization.
`for_each_aggregate_member(value, visitor)` supplies each field's name and const
reference. A compatible aggregate containing `std::string` does not need
Boost.Describe or an opt-in. Value mapping belongs to `forge_variant`; this
traversal never defines wire order or changes Raw serialization.

Boost.PFR's potential-reflection trait does not prove that an arbitrary C++
aggregate is supported. Bitfields, reference members, inheritance and C-array
members may require explicit Describe metadata or an opt-out before diagnostic
instantiation. Do not rely on the potential trait to reject every incompatible
shape automatically. For example:

```cpp
#include <boost/pfr/traits.hpp>
#include <type_traits>

struct bitfield_record { unsigned flags : 3; };

import forge.reflect.reflect;

template <>
struct boost::pfr::is_reflectable<bitfield_record, forge::reflect::diagnostic_aggregate_tag>
   : std::false_type {};
```

Variant diagnostics return an explicit unsupported marker for an opted-out
undescribed type without a custom conversion.

## Risks And Anti-Patterns

- Do not treat reflection metadata as business validation. It describes shape
  and order; schema/application layers validate meaning.
- Do not reorder described members as a cleanup unless every raw/wire consumer
  gets a compatibility migration.
- Do not add application-specific reflection macros here. FORGE stays neutral and
  Boost.Describe remains the explicit source of member order.

## Typical Mistakes

- Do not add `FORGE_DESCRIBE_*` wrappers casually. The canonical spelling is
  Boost.Describe until a separate compatibility decision changes it.
- Do not import higher-level variant modules from this library to "make it
  convenient".

## Tests

Reflect behavior is mostly exercised through `test_forge_raw`,
`test_forge_variant`, `test_forge_schema` and `test_forge_config_core`, where member order
and enum mapping are observable.
